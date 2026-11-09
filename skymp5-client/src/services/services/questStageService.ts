import { Quest, QuestStageEvent, QuestStartStopEvent } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { QuestStageMessage } from "../messages/questStageMessage";
import { remoteIdToLocalId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RemoteServer } from "./remoteServer";

const TAG = "verb:quest-stage";

// The storage key of the sharing mode and its list (sp.storage survives a
// plugin reload, ADR-041's survivor): they come once, at the login, and a
// hot-reloaded client keeps its connection and never logs in again. v2 holds
// {mode, ids} (memo D58 B); v1 held the list alone
const SHARED_STORAGE_KEY = "thuum.questStage.shared.v2";

// The sharing modes the server names at a login (the wire's QuestStage
// `mode`): list, the quests in `shared` are reported; all, every quest is
// reported but the quests in `shared`, the denied
const MODE_LIST = 0;
const MODE_ALL = 1;

// How long a stage this service set is expected to echo back as the engine's
// own quest-stage event, ms; after that the mark is dropped
const APPLY_ECHO_MS = 5000;

// How long after an apply the fragment mark it set is taken back by the
// safety if the stage's event never claimed it, ms. The VM queues a stage set
// on a quest not yet running and sets it at the quest's start, after the
// latent call's answer, so the mark must outlive the promise (the review of
// m3-pass-train-2, finding 4); past this window the engine is left its
// fragment
const FRAGMENT_SAFETY_MS = 10000;

// How many quests may hold a parked stage at once; the oldest parking makes
// room for a new quest
const PARKED_MAX = 64;

// How long after the login's game load the arrival waits for the player's
// cell to carry a location before it gives up, ms (the engine attaches the
// player's cell before its load-game event, so the first update should read
// it; the window is a hedge the trace line reports)
const ARRIVAL_WINDOW_MS = 3000;

// A report equal to the last one this game sent for the quest within this
// window is dropped and counted (docs/verbs/quest-stage.md, the stage-0
// churn: DLC1VCController 0x0201a428 restarts itself in bursts of 32 and
// every restart fires its stage-0 event; the server treats a repeated
// (quest, stage) as a no-op already, so nothing is lost), ms. UNMEASURED
// beyond the burst's shape in x-quest-mode v5's 151127
const REPORT_REPEAT_MS = 30000;

// How many quests the repeat memory holds; the oldest entry makes room
const REPORT_MEMORY_MAX = 256;

// How often the update pass writes the dropped count of a window that ended
// with no further report, ms
const REPEAT_FLUSH_MS = 5000;

// ADR-068 rung 5: the run words on QuestStage (the wire's QUEST_START and
// QUEST_STOP), and the stage the platform's acting scope names each by
const WORD_START = 1;
const WORD_STOP = 2;
const SCOPE_START = -1;
const SCOPE_STOP = -2;

// The players' actors are server ids from here up (the validator refuses an
// `acting` below it)
const FIRST_SERVER_ID = 0xff000000;

// How long the acting scope a live apply names holds, ms: the fragment
// mark's window, for the same reason (the VM sets a stage queued on a quest
// not yet running at the quest's start, after the latent answer)
const ACTING_TTL_MS = FRAGMENT_SAFETY_MS;

// The churn hold (docs/research/rung5-quests-2026-11-09.md, read 5): a quest
// whose run flips more than RUN_FLIPS times within RUN_WINDOW_MS here is held
// for the rest of the connection, its words no longer reported.
// DLC1VCController 0x0201a428 restarts itself in bursts of 32 about a minute
// apart on every seat, and a layer's quest at 0x30002859 starts and stops
// about 1.3 times a second (papyrus-events.md). The server's quest_runs
// bound mirrored; UNMEASURED until x-quest-runs v1 sizes both
const RUN_FLIPS = 6;
const RUN_WINDOW_MS = 60000;
const RUN_MEMORY_MAX = 256;

// How long a quest's next word or stage waits for the engine to answer the
// one before it (a latent start or SetStage), ms; past it the next goes
// anyway, logged
const BUSY_MAX_MS = 15000;

// one of the server's quest words or stages waiting for the update pass
type QuestItem = {
    quest: number;
    stage: number;
    word?: number;   // WORD_START or WORD_STOP; absent for a stage
    acting?: number; // the server's stamp on a relay
    served: boolean; // apply as a load (the login's replay)
};

// thuum docs/verbs/quest-stage.md (ADR-056, memo D55 B; Eli, playtest 37:
// "appropriately communicating the worlds shared quest state ... makes sense
// to start now as part of the dialogue work"): the world's quest stages. A
// stage this game sets on a shared quest (a dialogue fragment, a script, the
// lab's console) is reported to the server (R1), which records it for the
// world, relays it to every other seat and replays the record after a login;
// a stage from the server is applied through the engine's own SetStage
// (Quest.setCurrentStageID, the latent native), so this seat's topics,
// conditions and journal read the same quest state as the talker's. The
// engine's event from that apply is consumed by the mark below and never
// reported back; a stage the engine already has done is skipped. Nothing is
// reported before the server's login message names the sharing mode: in
// mode list a quest off the list stays this seat's own; in mode all (memo
// D58 B, TES3MP's shared journal) every quest is the world's but the denied
// ones (the main quest's intro chain, the DLC starts), which stay this
// seat's own. Since ADR-068 rung 5 the login's list in mode all names the
// per-player quests too (D-Q1's derivation, the server's alone: a quest
// whose aliases or state natives are all the player's), and memo D62's
// start-game-enabled rule is retired with its `allowed` list, so a listed
// quest is never reported and every other one is, the start-game-enabled
// ones included. A quest that refuses Quest.Start has the world's
// stage parked (the highest asked per quest) and applied when the engine's
// own start of it arrives as the questStart event, because an event quest is
// the Story Manager's alone to start (the Creation Kit's rule: "Any quest
// which specifies an Event can only be started by the Story Manager"; A
// Lovely Letter 0x74a83 starts on the player's Change Location into Riverwood).
//
// The login's arrival (H-56a, docs/research/m3/re-story-location.md): the
// engine raises the Change Location event only when the player's location
// changes, and a seat logging in INSIDE a location (every playtest seat: the
// camp is the landing) attaches with none to report, so its Story Manager
// never starts the location's quests, while a seat placed in from outside
// logged locationChanged and questStart in one millisecond
// (x-story-manager-walk 20261029-205447). Once per login, after the server
// has created this connection's own actor (CreateActor with isMe, the login
// itself; HostService reads the same message for its tables) and the game
// has loaded, this service asks TESModPlatform.SendStoryChangeLocation for
// the same event from no location to the player's current one, through the
// engine's own story event queue, from the update pass (callNative needs the
// VM context). The sharing-mode message is no part of the condition: a
// server with nothing shared sends none (the review of m3-pass-train-2,
// finding 3), and the arrival is the login's, whatever is shared. Every new
// connection starts the once-per-login flags over. HYPOTHESIS until
// x-story-manager-walk v3: the Story Manager then starts what an arrival
// starts. Behind the client setting storyArrivalAtLogin (default on).
// Slice two (the verb doc's "Slice two", the re-analyst's read of
// 2026-10-29): a stage applied from the relay runs the engine's SetStage
// whole, but its Papyrus fragment (the QF_ script's Fragment_N: the reward,
// the world effect) ran on the talker's game already, so one mark on the
// pair is handed to Skyrim Platform's replacement of the VM's quest stage
// sink (TESModPlatform.SuppressQuestStageFragment, FridaHooks.cpp) before
// the SetStage. The sink consumes the mark at the stage's first event,
// whenever the VM sets the stage (a stage queued on a quest not yet running
// is set at its start, after the promise's answer, so the answer is no
// place to take the mark back), and the safety release FRAGMENT_SAFETY_MS
// after the apply takes back a mark no event claimed (a refused stage, a
// quest never started), so none leaks; two applies of one pair are two
// marks. The journal entry, the done bit and the promise stay the engine's.
// This seat's own stages (the report path, onQuestStage) never pass there.
// HYPOTHESIS until lab/frida/quest-stage-chain.js reads the chain.
//
// ADR-068 rung 5 (docs/research/rung5-quests-2026-11-09.md 1(d); D-F1 C, D-Q3
// A). A quest's run state is the world's too: this game's own start or stop
// of a quest it reports (the platform's questStart and questStop) goes out as
// QuestStage with `word` 1 or 2 and stage 0, never for a listed quest and
// never for one held by the churn bound (RUN_FLIPS run flips within
// RUN_WINDOW_MS here: "quest <q>'s run flipped <n> times in <s> s here:
// held, not reported (churn)", then held until the connection ends; the
// server's E_QUEST_CHURN is the backstop). A relayed start runs Quest.Start
// (latent; refused, as for an event quest, it parks until the engine's own
// start, which is then the world's and not reported), a relayed stop
// Quest.Stop (a stop drops what was parked for the quest: the next start is
// a new run), each under an echo mark consumed at the engine's event as a
// stage's is. Every report names `acting`, the platform's getActingPlayer
// for the pair (-1 a start, -2 a stop), when a relayed request's scope set it
// here (a chained stage, a relayed trigger's script). Before a live apply,
// setActingPlayer names the server's `acting` for the pair: its figure here
// (remoteIdToLocalId, 0 when this seat holds none) and its server id, for
// ACTING_TTL_MS; never for this seat's own actor (its own player is the
// player already) and never for a served apply (a load). The fragment: a
// served stage always carries the mark (the login's replay is a load, D-Q3
// A); a live stage carries it while the setting questFragmentLive is false
// (the default, the H-A1 fallback, until x-acting-player v1 is green), and
// runs its fragment under the acting scope when it is true. The words and
// stages apply in the server's order, one an update, and a quest's next one
// waits for the engine's answer to its last (at a login: starts, stages,
// stops). The setting questRuns false turns the words off both ways; a
// stage-0 report goes out either way (b-quest-report-dedupe counts it), and
// the server judges it a start (read 5). Declared: a served
// start runs the quest's start-up stage fragments, since the mark is per
// stage and Skyrim Platform's Quest names no start-up stage (UNVERIFIED how
// often a recorded running quest is not running on a late joiner).
export class QuestStageService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("questStageMessage", (e) => this.onMessage(e));
        this.controller.on("questStage", (e) => this.onQuestStage(e));
        this.controller.on("questStart", (e) => this.onQuestStart(e));
        this.controller.on("questStop", (e) => this.onQuestStop(e));
        this.controller.on("update", () => this.onUpdate());
        this.controller.emitter.on("gameLoad", () => this.onGameLoad());
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
        this.controller.emitter.on("connectionAccepted", () => this.onConnectionAccepted());
        this.resume();
        this.sayRetiredSetting();
    }

    private onMessage(e: ConnectionMessage<QuestStageMessage>) {
        const m = e.message;
        if (m.quest === 0) {
            // the login's message: the sharing mode and its list (the quests to
            // report in mode list, the quests never to report in mode all)
            const mode = m.mode === MODE_ALL ? MODE_ALL : MODE_LIST;
            const ids = (m.shared ?? []).filter((id) => Number.isInteger(id) && id > 0);
            this.setShared(mode, ids);
            this.sp.storage[SHARED_STORAGE_KEY] = { mode, ids };
            logTrace(this, "the server shares quests in mode", QuestStageService.modeName(mode) + ":", ids.length, mode === MODE_ALL ? "quest(s) denied" : "quest(s) listed");
            return;
        }
        if (m.word !== undefined && m.word !== WORD_START && m.word !== WORD_STOP) {
            logError(this, "a quest word", m.word, "for quest", m.quest.toString(16), "is neither a start nor a stop; dropped");
            return;
        }
        // rule 5: an `acting` that names no player's actor is no stamp
        const acting = typeof m.acting === "number" && Number.isInteger(m.acting) && m.acting >= FIRST_SERVER_ID ? m.acting : undefined;
        this.pending.push({ quest: m.quest, stage: m.stage, word: m.word, acting, served: m.served === true });
    }

    // one word or stage per update, in the server's order (ascending within a
    // quest at a login's replay, so each SetStage sees the stages before it;
    // its starts first and its stops last), a quest's next one after the
    // engine answered its last
    private onUpdate() {
        this.arrival();
        const now = Date.now();
        this.flushRepeats(now);
        // slice two's safety: a fragment mark whose stage event never came
        // within its window is taken back (callNative runs here, in the
        // update pass's VM context); the list is in order of expiry, and it
        // runs before the switch below so a switched-off service leaks none
        while (this.fragmentReleases.length > 0 && this.fragmentReleases[0].at <= now) {
            const due = this.fragmentReleases.shift();
            if (due) {
                this.releaseFragment(due.questId, due.stage);
            }
        }
        if (!this.enabled()) {
            this.pending = [];
            return;
        }
        const next = this.nextItem(now);
        if (next) {
            if (next.word !== undefined) {
                this.applyWord(next);
            } else {
                this.apply(next);
            }
        }
        if (this.applying.size > 0) {
            this.applying.forEach((until, key) => {
                if (until < now) {
                    this.applying.delete(key);
                }
            });
        }
        if (this.applyingRuns.size > 0) {
            this.applyingRuns.forEach((until, key) => {
                if (until < now) {
                    this.applyingRuns.delete(key);
                }
            });
        }
    }

    // the first waiting word or stage whose quest has no answer of the
    // engine's pending (its own order is the server's; another quest's go
    // past it); a quest whose answer is BUSY_MAX_MS late lets its next go
    private nextItem(now: number): QuestItem | undefined {
        for (let i = 0; i < this.pending.length; i++) {
            const questId = this.pending[i].quest;
            const until = this.busy.get(questId);
            if (until !== undefined) {
                if (until > now) {
                    continue;
                }
                this.busy.delete(questId);
                logTrace(this, "quest", questId.toString(16) + "'s last apply had no answer in", BUSY_MAX_MS / 1000, "s; its next goes on");
            }
            return this.pending.splice(i, 1)[0];
        }
        return undefined;
    }

    private apply(item: QuestItem) {
        const questId = item.quest;
        const stage = item.stage;
        const quest = this.questHere(questId);
        if (!quest) {
            return;
        }
        if (quest.isStageDone(stage)) {
            // the engine has it already: idempotent, nothing to report; a stage
            // parked for the quest is stale too, the engine got there by itself
            this.parked.delete(questId);
            return;
        }
        const key = QuestStageService.key(questId, stage);
        this.applying.set(key, Date.now() + APPLY_ECHO_MS);
        // a quest not running here is started first (Quest.Start, latent): the
        // engine's SetStage is said to start a stopped quest by itself, but the
        // lab's live set-stage on A Lovely Letter 0x74a83 (playtest 37, 03:59Z:
        // stage 0, running false before and after) set nothing with no answer
        // read, so the start is asked for on its own and each answer is logged
        // (numidium's driver does the same under set-stage {start: true}).
        // That start is the world's too: its event is not reported back, and
        // a live apply names the acting scope for it as for the stage
        const running = quest.isRunning();
        const runKey = QuestStageService.runKey(questId, WORD_START);
        if (!running) {
            this.applyingRuns.set(runKey, Date.now() + APPLY_ECHO_MS);
            this.nameActing(item, SCOPE_START);
        }
        this.nameActing(item, stage);
        this.busy.set(questId, Date.now() + BUSY_MAX_MS);
        const started: Promise<boolean> = running ? Promise.resolve(true) : quest.start();
        started.then((startOk) => {
            if (!startOk) {
                this.applying.delete(key);
                this.applyingRuns.delete(runKey);
                // Quest.Start answers false by design on an event quest (the
                // Story Manager's to start) and on a quest whose aliases or
                // conditions fail; the stage waits for the engine's own start
                this.park(item);
                logTrace(this, "the world's stage", stage, "of quest", questId.toString(16), "parked until the engine starts it");
                return;
            }
            return this.setStage(questId, stage, key, this.marksFragment(item.served));
        }).catch((err) => {
            this.applying.delete(key);
            logError(this, "SetStage failed on quest", questId.toString(16), "stage", stage, err);
        }).then(() => {
            this.busy.delete(questId);
        });
    }

    // the engine's SetStage under the echo mark the caller set, so the
    // engine's own stage event from this apply is consumed, never reported.
    // The quest is looked up again here: a wrapper from an earlier frame is
    // out of Skyrim Platform's object pool after a latent call's answer
    // ("Invalid _skyrimPlatform_indexInPool", the lab driver's set-stage,
    // playtest 37 at 05:59Z). `mark`: the fragment is the world's already
    // (a served stage, or a live one while questFragmentLive is false)
    private setStage(questId: number, stage: number, key: string, mark: boolean): Promise<void> {
        const quest = this.sp.Quest.from(this.sp.Game.getFormEx(questId));
        if (!quest) {
            this.applying.delete(key);
            logError(this, "the world's quest", questId.toString(16), "is gone before its stage", stage, "was set");
            return Promise.resolve();
        }
        // slice two: one mark on the pair for the native sink, consumed at the
        // stage's event; the safety in onUpdate takes it back if none comes
        if (mark && this.suppressFragment(quest, questId, stage)) {
            this.fragmentReleases.push({ questId, stage, at: Date.now() + FRAGMENT_SAFETY_MS });
        }
        return quest.setCurrentStageID(stage).then((ok) => {
            logTrace(this, "the world's stage", stage, "of quest", questId.toString(16), ok ? "set" : "refused by the engine");
            if (!ok) {
                this.applying.delete(key);
            }
        });
    }

    // Slice two's hand-over of one mark to the native sink (see the class
    // comment). The suppress runs where the SetStage runs, inside the update
    // tick's promise chain, and the safety release from the update pass
    // itself, both the VM context callNative needs; the release looks the
    // quest up anew, because a wrapper from an earlier tick is out of Skyrim
    // Platform's object pool (as setStage above), and the native takes back
    // one mark only if the pair still holds one. A refusal or a throw of the
    // suppress leaves the fragment the engine's, logged, and the stage still
    // applies.
    private suppressFragment(quest: Quest, questId: number, stage: number): boolean {
        try {
            const ok = this.sp.callNative("TESModPlatform", "SuppressQuestStageFragment", undefined, quest, stage);
            if (ok !== true) {
                logError(this, "TESModPlatform.SuppressQuestStageFragment refused quest", questId.toString(16), "stage", stage);
                return false;
            }
            return true;
        } catch (err) {
            logError(this, "TESModPlatform.SuppressQuestStageFragment failed on quest", questId.toString(16), "stage", stage, err);
            return false;
        }
    }

    private releaseFragment(questId: number, stage: number) {
        const quest = this.sp.Quest.from(this.sp.Game.getFormEx(questId));
        if (!quest) {
            logError(this, "the world's quest", questId.toString(16), "is gone before its stage", stage, "fragment was released");
            return;
        }
        try {
            this.sp.callNative("TESModPlatform", "ReleaseQuestStageFragment", undefined, quest, stage);
        } catch (err) {
            logError(this, "TESModPlatform.ReleaseQuestStageFragment failed on quest", questId.toString(16), "stage", stage, err);
        }
    }

    // the highest stage the world asked for on a quest that would not start,
    // kept until the engine starts the quest; the oldest parking goes when
    // the map is full. The item keeps its served flag and acting stamp for
    // the apply at the engine's start
    private park(item: QuestItem) {
        const had = this.parked.get(item.quest);
        if (had !== undefined) {
            if (item.stage > had.stage) {
                this.parked.set(item.quest, item);
            }
            return;
        }
        if (this.parked.size >= PARKED_MAX) {
            const oldest = this.parked.keys().next().value;
            if (oldest !== undefined) {
                this.parked.delete(oldest);
            }
        }
        this.parked.set(item.quest, item);
    }

    // a world's start the engine refused (an event quest), kept until the
    // engine starts the quest by itself, a start that is then the world's
    // and not reported; the oldest goes when the set is full
    private parkStart(questId: number) {
        if (this.parkedStarts.has(questId)) {
            return;
        }
        if (this.parkedStarts.size >= PARKED_MAX) {
            const oldest = this.parkedStarts.values().next().value;
            if (oldest !== undefined) {
                this.parkedStarts.delete(oldest);
            }
        }
        this.parkedStarts.add(questId);
    }

    // the engine's start of a quest in this game (Skyrim Platform's
    // questStart, sent from its update task, so game calls are allowed here):
    // the world's start landing here (its echo mark), or the engine's own
    // start of a quest the world runs (a parked start or stage; a parked
    // stage is applied now, the same way as any other), or this game's own
    // start, reported (rung 5)
    private onQuestStart(e: QuestStartStopEvent) {
        if (!this.enabled() || !e.quest) {
            return;
        }
        const questId = e.quest.getFormID();
        const echo = this.applyingRuns.delete(QuestStageService.runKey(questId, WORD_START));
        const parkedStart = this.parkedStarts.delete(questId);
        const parkedStage = this.applyParked(e.quest, questId);
        if (echo || parkedStart || parkedStage) {
            return;
        }
        this.reportRun(questId, WORD_START);
    }

    // the stage parked for a quest the engine just started, applied; false
    // when none was parked
    private applyParked(quest: Quest, questId: number): boolean {
        const item = this.parked.get(questId);
        if (item === undefined) {
            return false;
        }
        this.parked.delete(questId);
        const stage = item.stage;
        if (quest.isStageDone(stage)) {
            return true; // the start itself got there (a startup stage), nothing to set
        }
        const key = QuestStageService.key(questId, stage);
        this.applying.set(key, Date.now() + APPLY_ECHO_MS);
        this.nameActing(item, stage);
        logTrace(this, "the engine started quest", questId.toString(16) + ", the world's parked stage", stage, "applied");
        this.setStage(questId, stage, key, this.marksFragment(item.served)).catch((err) => {
            this.applying.delete(key);
            logError(this, "SetStage failed on quest", questId.toString(16), "stage", stage, err);
        });
        return true;
    }

    // the engine's stop of a quest in this game (questStop, the same task):
    // the world's stop landing here (its echo mark, the engine's answer to
    // it), or this game's own stop, reported (rung 5)
    private onQuestStop(e: QuestStartStopEvent) {
        if (!this.enabled() || !e.quest) {
            return;
        }
        const questId = e.quest.getFormID();
        if (this.applyingRuns.delete(QuestStageService.runKey(questId, WORD_STOP))) {
            this.busy.delete(questId);
            return;
        }
        this.reportRun(questId, WORD_STOP);
    }

    // a world's start or stop (served at a login or relayed), applied through
    // the engine's own Quest.Start (latent) or Quest.Stop under an echo mark;
    // nothing for a quest already in that state here
    private applyWord(item: QuestItem) {
        if (!this.runsEnabled()) {
            if (!this.runsOffSaid) {
                this.runsOffSaid = true;
                logTrace(this, "questRuns is off in settings: the world's quest starts and stops are not applied here, and this game's are not reported");
            }
            return;
        }
        const questId = item.quest;
        const quest = this.questHere(questId);
        if (!quest) {
            return;
        }
        const how = item.served ? "served" : "relayed";
        if (item.word === WORD_START) {
            if (quest.isRunning()) {
                return; // running here already
            }
            const runKey = QuestStageService.runKey(questId, WORD_START);
            this.applyingRuns.set(runKey, Date.now() + APPLY_ECHO_MS);
            this.nameActing(item, SCOPE_START);
            this.busy.set(questId, Date.now() + BUSY_MAX_MS);
            quest.start().then((ok) => {
                if (ok) {
                    logTrace(this, "the world's start of quest", questId.toString(16), "(" + how + ") applied");
                    return;
                }
                this.applyingRuns.delete(runKey);
                this.parkStart(questId);
                logTrace(this, "the world's start of quest", questId.toString(16), "(" + how + ") parked until the engine starts it (Quest.Start refused: an event quest is the Story Manager's to start)");
            }).catch((err) => {
                this.applyingRuns.delete(runKey);
                logError(this, "Quest.Start failed on quest", questId.toString(16), err);
            }).then(() => {
                this.busy.delete(questId);
            });
            return;
        }
        // a stop ends the run the world had: what waited for its start goes
        this.parkedStarts.delete(questId);
        this.parked.delete(questId);
        if (!quest.isRunning() && !quest.isStarting()) {
            return; // stopped here already
        }
        this.applyingRuns.set(QuestStageService.runKey(questId, WORD_STOP), Date.now() + APPLY_ECHO_MS);
        this.nameActing(item, SCOPE_STOP);
        // the engine's stop event answers it (onQuestStop)
        this.busy.set(questId, Date.now() + BUSY_MAX_MS);
        quest.stop();
        logTrace(this, "the world's stop of quest", questId.toString(16), "(" + how + ") applied");
    }

    // this game's own start or stop of a quest (no mark of the world's):
    // reported as the world's unless the quest is listed, held for churn, or
    // the words are off
    private reportRun(questId: number, word: number) {
        if (!this.runsEnabled() || !this.reports(questId) || this.churned(questId)) {
            return;
        }
        const message: QuestStageMessage = { t: MsgType.QuestStage, quest: questId, stage: 0, word };
        const acting = this.reportedActing(questId, word === WORD_START ? SCOPE_START : SCOPE_STOP);
        if (acting !== undefined) {
            message.acting = acting;
        }
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logTrace(this, "this game", word === WORD_START ? "started" : "stopped", "shared quest", questId.toString(16) + "; reported" + QuestStageService.forActing(acting));
    }

    // the churn hold: true when the quest is held, or when this flip passes
    // RUN_FLIPS within its window (said once; held until the connection ends)
    private churned(questId: number): boolean {
        if (this.heldRuns.has(questId)) {
            return true;
        }
        const now = Date.now();
        let window = this.runFlips.get(questId);
        if (!window || now - window.since >= RUN_WINDOW_MS) {
            if (!window && this.runFlips.size >= RUN_MEMORY_MAX) {
                const oldest = this.runFlips.keys().next().value;
                if (oldest !== undefined) {
                    this.runFlips.delete(oldest);
                }
            }
            window = { since: now, flips: 0 };
            this.runFlips.set(questId, window);
        }
        window.flips++;
        if (window.flips <= RUN_FLIPS) {
            return false;
        }
        this.runFlips.delete(questId);
        if (this.heldRuns.size >= RUN_MEMORY_MAX) {
            const oldest = this.heldRuns.values().next().value;
            if (oldest !== undefined) {
                this.heldRuns.delete(oldest);
            }
        }
        this.heldRuns.add(questId);
        logLab(TAG, `quest ${questId.toString(16)}'s run flipped ${window.flips} times in ${Math.round((now - window.since) / 1000)} s here: held, not reported (churn)`);
        return true;
    }

    // the acting scope for a live apply (the class comment): the server's
    // acting player for the pair, its figure here or 0; none for a served
    // apply, for a relay with no stamp, or for this seat's own actor
    private nameActing(item: QuestItem, scopeStage: number) {
        const acting = item.acting;
        if (item.served || acting === undefined || this.actingExportsMissing || this.isMe(acting)) {
            return;
        }
        const figure = remoteIdToLocalId(acting);
        try {
            if (this.sp.setActingPlayer(item.quest, scopeStage, figure, acting, ACTING_TTL_MS) !== true) {
                logError(this, "setActingPlayer refused quest", item.quest.toString(16), "stage", scopeStage, "for", acting.toString(16));
            }
        } catch (err) {
            this.actingExportsMissing = true;
            logLab(TAG, `setActingPlayer unavailable on this platform (${err}); a relayed request runs as this seat's own player`);
        }
    }

    // the scope's player the platform names for a reported pair, a server id;
    // undefined for none (the sender is the acting player)
    private reportedActing(questId: number, scopeStage: number): number | undefined {
        if (this.actingExportsMissing) {
            return undefined;
        }
        let id: unknown;
        try {
            id = this.sp.getActingPlayer(questId, scopeStage);
        } catch (err) {
            this.actingExportsMissing = true;
            logLab(TAG, `getActingPlayer unavailable on this platform (${err}); reports name no acting player`);
            return undefined;
        }
        return typeof id === "number" && Number.isInteger(id) && id >= FIRST_SERVER_ID && id <= 0xffffffff ? id : undefined;
    }

    // whether a server id is this connection's own actor (a 64-bit id
    // compared by its low 32 bits, as localIdToRemoteId reads one)
    private isMe(actorId: number): boolean {
        const mine = this.controller.lookupListener(RemoteServer).getMyRemoteRefrId();
        return mine > 0 && mine % 0x100000000 === actorId;
    }

    private static forActing(acting: number | undefined): string {
        return acting === undefined ? "" : " for acting player " + acting.toString(16);
    }

    // the engine's own event, for every quest's stage change in this game
    private onQuestStage(e: QuestStageEvent) {
        if (!this.enabled() || !e.quest) {
            return;
        }
        const questId = e.quest.getFormID();
        const key = QuestStageService.key(questId, e.stage);
        if (this.applying.has(key)) {
            this.applying.delete(key); // the world's stage landing here, not this game's own
            return;
        }
        if (!this.reports(questId)) {
            return;
        }
        if (this.repeated(questId, e.stage)) {
            return; // the same stage again within its window: dropped and counted
        }
        // rung 5: the scope's player when a relayed request set it here
        const message: QuestStageMessage = { t: MsgType.QuestStage, quest: questId, stage: e.stage };
        const acting = this.reportedActing(questId, e.stage);
        if (acting !== undefined) {
            message.acting = acting;
        }
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logTrace(this, "this game set stage", e.stage, "of shared quest", questId.toString(16) + "; reported" + QuestStageService.forActing(acting));
    }

    // the login's game load (LoadGameService's gameLoad, from the engine's
    // own load-game event, so the player's cell is attached): the other half
    // of the arrival's condition. A reconnect with the game up spawns through
    // MoveRefrToPosition and loads nothing, so it raises no arrival: that
    // seat's Story Manager heard its login's
    private onGameLoad() {
        this.arrivalGameLoaded = true;
    }

    // the server's CreateActor of this connection's own actor (isMe: a login,
    // a reconnect, a restart, as HostService reads it): the login half of
    // the arrival's condition. The game's load follows it on a fresh launch
    // (RemoteServer loads the save for the actor it was given)
    private onCreateActor(e: ConnectionMessage<CreateActorMessage>) {
        if (!e.message.isMe) {
            return;
        }
        this.arrivalLoginSeen = true;
    }

    // a new connection (NetworkingService's connectionAccepted, before the
    // server's CreateActor): the once-per-login flags start over, so each
    // login's arrival is its own and none is carried across a reconnect; so
    // do rung 5's run marks, parked starts and churn holds (the server serves
    // the run state anew)
    private onConnectionAccepted() {
        this.arrivalDone();
        this.applyingRuns.clear();
        this.parkedStarts.clear();
        this.busy.clear();
        this.runFlips.clear();
        this.heldRuns.clear();
    }

    // the login's arrival, from the update pass: once this connection's own
    // actor has been created and the game has loaded, the Change Location
    // event from no location to the player's current one, once; a player in
    // no location gets none (the engine's own event comes at the first
    // location entered)
    private arrival() {
        if (!this.arrivalLoginSeen || !this.arrivalGameLoaded) {
            return;
        }
        if (!this.arrivalEnabled()) {
            this.arrivalDone();
            return;
        }
        if (this.arrivalDeadline === 0) {
            this.arrivalDeadline = Date.now() + ARRIVAL_WINDOW_MS;
        }
        const player = this.sp.Game.getPlayer();
        const location = player ? player.getCurrentLocation() : null;
        if (!player || !location) {
            if (Date.now() < this.arrivalDeadline) {
                return; // read again next update, within the window
            }
            this.arrivalDone();
            logTrace(this, "no location at the login's cell; no arrival raised (the engine's own event comes at the first location entered)");
            return;
        }
        this.arrivalDone();
        const locationId = location.getFormID();
        try {
            const raised = this.sp.callNative("TESModPlatform", "SendStoryChangeLocation", undefined, player, null, location);
            if (raised === true) {
                logLab("verb:quest-stage", "arrival event raised at login for location", locationId.toString(16));
            } else {
                logError(this, "TESModPlatform.SendStoryChangeLocation refused for location", locationId.toString(16));
            }
        } catch (err) {
            // an older dist without the native throws here
            logError(this, "TESModPlatform.SendStoryChangeLocation failed for location", locationId.toString(16), err);
        }
    }

    private arrivalDone() {
        this.arrivalLoginSeen = false;
        this.arrivalGameLoaded = false;
        this.arrivalDeadline = 0;
    }

    private arrivalEnabled(): boolean {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        return !s || s["storyArrivalAtLogin"] !== false;
    }

    // whether a stage this game set on the quest is the world's to report:
    // nothing before the server's login message; in mode list the listed
    // quests; in mode all every quest but the denied (memo D58 B), a
    // start-game-enabled one included, which the server refuses unless it
    // is allowed (memo D62; the class comment says why the filter is its)
    // the repeat window: true when this game reported the same stage of the
    // quest within REPORT_REPEAT_MS (the repeat is counted and dropped); a
    // new stage, or the same one past the window, starts a new window and
    // writes the old one's count
    private repeated(questId: number, stage: number): boolean {
        const now = Date.now();
        const last = this.lastReports.get(questId);
        if (last && last.stage === stage && now - last.since < REPORT_REPEAT_MS) {
            last.dropped++;
            return true;
        }
        if (last) {
            this.closeRepeatWindow(questId, last, now);
        } else if (this.lastReports.size >= REPORT_MEMORY_MAX) {
            const oldest = this.lastReports.keys().next().value;
            if (oldest !== undefined) {
                this.lastReports.delete(oldest);
            }
        }
        this.lastReports.set(questId, { stage, since: now, dropped: 0 });
        return false;
    }

    // one line per window that dropped anything, when the window ends (the
    // next report of the quest, or the update pass's flush)
    private closeRepeatWindow(questId: number, last: { stage: number; since: number; dropped: number }, now: number) {
        if (last.dropped > 0) {
            logTrace(this, "stage", last.stage, "of quest", questId.toString(16), "repeated", last.dropped, "times in", Math.round((now - last.since) / 1000), "s; dropped");
            last.dropped = 0;
        }
    }

    private flushRepeats(now: number) {
        if (now < this.nextRepeatFlush) {
            return;
        }
        this.nextRepeatFlush = now + REPEAT_FLUSH_MS;
        this.lastReports.forEach((last, questId) => {
            if (now - last.since >= REPORT_REPEAT_MS) {
                this.closeRepeatWindow(questId, last, now);
                this.lastReports.delete(questId);
            }
        });
    }

    private reports(questId: number): boolean {
        if (this.mode === undefined) {
            return false;
        }
        return this.mode === MODE_ALL ? !this.shared.has(questId) : this.shared.has(questId);
    }

    private setShared(mode: number, ids: number[]) {
        this.mode = mode;
        this.shared = new Set(ids);
    }

    // After a plugin reload: the mode and the list the last login brought,
    // from storage
    private resume() {
        const stored = this.sp.storage[SHARED_STORAGE_KEY] as { mode?: unknown; ids?: unknown } | undefined;
        if (!stored || typeof stored !== "object" || !Array.isArray(stored.ids)) {
            return;
        }
        if (stored.mode !== MODE_LIST && stored.mode !== MODE_ALL) {
            return;
        }
        const ids = stored.ids.filter((id): id is number => typeof id === "number" && Number.isInteger(id) && id > 0);
        this.setShared(stored.mode, ids);
        logTrace(this, "resumed after a reload in mode", QuestStageService.modeName(stored.mode) + ":", ids.length, stored.mode === MODE_ALL ? "quest(s) denied" : "quest(s) listed");
    }

    private static modeName(mode: number): string {
        return mode === MODE_ALL ? "all" : "list";
    }

    private enabled(): boolean {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        return !s || s["questStage"] !== false;
    }

    // rung 5's run words, both ways (questRuns, on unless false)
    private runsEnabled(): boolean {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        return !s || s["questRuns"] !== false;
    }

    // rung 5's live fragment: a relayed stage runs its fragment here under
    // the acting scope (questFragmentLive, off unless true; the H-A1 fallback
    // until x-acting-player v1 is green)
    private fragmentLive(): boolean {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        return !!s && s["questFragmentLive"] === true;
    }

    // whether an apply hands its stage's fragment mark to the sink: a served
    // stage always (a load, D-Q3 A), a live one while the fragment is not live
    private marksFragment(served: boolean): boolean {
        return served || !this.fragmentLive();
    }

    // slice two's switch questFragmentSuppress is retired by rung 5:
    // questFragmentLive is the live path's (a served stage always skips its
    // fragment), said once to a seat whose settings still name it
    private sayRetiredSetting() {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        if (s && s["questFragmentSuppress"] !== undefined) {
            logLab(TAG, "questFragmentSuppress is retired and ignored: a served stage always skips its fragment, a relayed one while questFragmentLive is not true");
        }
    }

    // the quest a world's word or stage names, said once when this game has
    // no such quest
    private questHere(questId: number): Quest | null {
        const quest = this.sp.Quest.from(this.sp.Game.getFormEx(questId));
        if (!quest && !this.missing.has(questId)) {
            this.missing.add(questId);
            logError(this, "the world's quest", questId.toString(16), "is not a quest in this game");
        }
        return quest;
    }

    private static key(questId: number, stage: number): string {
        return questId.toString(16) + ":" + stage;
    }

    private static runKey(questId: number, word: number): string {
        return questId.toString(16) + (word === WORD_START ? ":start" : ":stop");
    }

    // the sharing mode the server named at the login (0 list, 1 all); none
    // before it, and nothing is reported until then
    private mode: number | undefined;
    // the quests the login named (form ids, the load order both ends share):
    // the shared quests in mode list, the denied quests in mode all
    private shared = new Set<number>();
    // the world's words and stages still to apply, in the server's order
    private pending: QuestItem[] = [];
    // the stages this service is setting, by quest and stage, until the
    // engine's event echoes or the mark expires
    private applying = new Map<string, number>();
    // the same for the starts and stops it runs, by quest and word
    private applyingRuns = new Map<string, number>();
    // the quests whose last apply waits for the engine's answer, until when
    private busy = new Map<number, number>();
    // the highest stage the world asked for, by quest, on the quests that
    // refused Quest.Start here; applied at the engine's own start of each
    private parked = new Map<number, QuestItem>();
    // the world's starts the engine refused here (event quests)
    private parkedStarts = new Set<number>();
    // the churn hold's windows (since when, how many flips) and held quests
    private runFlips = new Map<number, { since: number; flips: number }>();
    private heldRuns = new Set<number>();
    private runsOffSaid = false;
    // an older platform without setActingPlayer and getActingPlayer
    private actingExportsMissing = false;
    // the last stage this game reported per quest: when its window began and
    // how many repeats of it were dropped since (REPORT_REPEAT_MS)
    private lastReports = new Map<number, { stage: number; since: number; dropped: number }>();
    private nextRepeatFlush = 0;
    private missing = new Set<number>();
    // slice two's safety: the fragment marks this service's applies set,
    // each with the time its release is due, in that order (one apply per
    // update tick, so at most a window's worth of entries)
    private fragmentReleases: Array<{ questId: number; stage: number; at: number }> = [];
    // the login's arrival: this connection's own actor has been created; the
    // game has loaded; the window's end once both hold (0 until then). All
    // three start over at every connectionAccepted
    private arrivalLoginSeen = false;
    private arrivalGameLoaded = false;
    private arrivalDeadline = 0;
}
