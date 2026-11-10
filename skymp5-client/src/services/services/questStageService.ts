import { Quest, QuestStageEvent, QuestStartStopEvent } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { QuestStageMessage } from "../messages/questStageMessage";
import { Mod } from "../messages_http/serverManifest";
import { remoteIdToLocalId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RemoteServer } from "./remoteServer";
import { SettingsService } from "./settingsService";

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

// x-quest-runs 20261110-071422's read, the login burst's safety net: the
// server's QuestStage budget, mirrored. wire-validate's quest_stage_budget
// (skymp-wire/crates/wire-validate/src/lib.rs:286, taken at :1677 for every
// QuestStage a client sends, a stage and a run word alike) holds 32 at once
// and refills 8 a second, one bucket a connection, and a report past it is
// refused, counted and logged, and lost: nothing tells this seat. The pacing
// spends the same bucket before each send and keeps QUEST_SEND_RESERVE
// tokens back, since the server counts arrivals and a reliable packet resent
// after a loss lands with the ones queued behind it, closing the gaps kept
// here (UNMEASURED; four tokens are half a second of the refill)
const QUEST_SEND_BURST = 32;
const QUEST_SEND_PER_S = 8;
const QUEST_SEND_RESERVE = 4;

// How many reports the pacing holds at most; past it the oldest held run
// word goes (the oldest stage only when no word is held), counted in the
// episode's line
const QUEST_SEND_HELD_MAX = 1024;

// How many quests the puppet rule's scene marks and their said lines keep;
// the oldest makes room
const PUPPET_SCENE_MEMORY_MAX = 256;

// How often at most the count of reports skipped for client-only plugins'
// quests is written while it moves, ms (in x-quest-runs' read
// RL_DetectionQuest alone started or stopped about 2.6 times a second a seat)
const SKIP_SAY_MS = 60000;

// one of the server's quest words or stages waiting for the update pass
type QuestItem = {
    quest: number;
    stage: number;
    word?: number;   // WORD_START or WORD_STOP; absent for a stage
    acting?: number; // the server's stamp on a relay
    served: boolean; // apply as a load (the login's replay)
    wordStages?: number[]; // a word's: the quest's start-up stages (a start) or shut-down stages (a stop)
};

// the quests whose start-up stages a start word named, kept for the implicit
// start a stage's apply makes (the stages are the QUST record's, fixed); the
// oldest goes past this
const START_UP_MEMORY_MAX = 2048;

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
// runs its fragment under the acting scope when it is true. The login's
// message also names the player-class quests (`player`, the list's entry
// shape): each seat runs its own player's (the ownership rule; the vampire's
// stage progresses only where its scripts' events pass the platform's
// SendEvent gate), so the list goes to the platform's setPlayerQuests at
// every login and reconnect ([] when the field is absent), again after a
// game load and a plugin reload, "[verb:quest-stage] player quests: <n> told
// to the platform at the login"; none of them is ever reported. The words and
// stages apply in the server's order, one an update, and a quest's next one
// waits for the engine's answer to its last (at a login: starts, stages,
// stops). The setting questRuns false turns the words off both ways; a
// stage-0 report goes out either way (b-quest-report-dedupe counts it), and
// the server judges it a start (read 5). D-Q3 A's start-up and shut-down
// fragments (the fresh review's note 7): a start runs its quest's start-up
// stages' fragments and a stop may run its shut-down stages', so the server
// stamps each word with `wordStages` (the QUST record's StartUpStage for a
// start, ShutDownStage for a stop); before Quest.Start or Quest.Stop each
// of them gets an echo mark (`applying`, so the stage's event is not reported
// back) and, when the word's fragment is the world's already
// (marksFragment: served, or questFragmentLive false), the sink's suppress
// mark with its safety release, as a replayed stage's. The implicit start a
// stage's apply makes on a stopped quest uses the start-up stages the
// quest's last start word named (a login's served starts name every recorded
// running quest's). An absent field changes nothing.
//
// x-quest-runs 20261110-071422's read (a canon run read against rung 5's
// code, 2026-11-10): three rules ahead of the run words. The login's load: a
// seat's own game load raises some 400 questStart and questStop events in
// its first 8 s (c1 405, c2 412 by the run's driver logs, held repeats
// spread; about 280 in the first second): the template save's quests, the
// start-game-enabled ones, and USSEP's retroactive-fix quests, which start at
// the load and stop themselves 1.7 to 7 s later. Reported, they would overrun
// the server's budget (below) and write the template save's state into the
// world's record: a load-time start of a quest the record holds stopped
// would be recorded and relayed, running it again on every other seat. So
// from the login's CreateActor while no update has run in this game (the
// main menu, where RemoteServer's spawn loads the save; a seat with the game
// up moves its player and loads nothing) until the login's arrival resolves
// (raised, given up after ARRIVAL_WINDOW_MS, or switched off), this game's
// own run events are held and counted, one line at the arrival: "<n> run
// event(s) of the login's load held: the template save's, not reported" (a
// new connection before the arrival ends the hold with the same line). It
// opens at the login, not at gameLoad, because the engine raises some of
// them inside the save load, queued ahead of its load-game event: c1's first
// 14 starts (07:14:57.672 to .674) reached this service before gameLoad
// (LoginCellService's first line after gameLoad, at .993, follows them),
// c2's none. In that read the hold takes 320 of c1's 405 and 376 of c2's 412,
// both logins landing in no location, so each arrival came at the end of its
// window. A login inside a location resolves its arrival at the first update
// after gameLoad, ahead of the start-game-enabled quests' starts (c1 261 and
// c2 276 in the 0.5 s after gameLoad), which then go as words: the bound is
// the arrival's (the fix pass's choice, no new timer), and that case is
// open. A run change after the arrival is a word like any other (W1 B
// retired the start-game-enabled rule): there, 74 of c1's USSEP self-stops
// and 23 of c2's. A stage event of the load is no run event and still goes
// (b-quest-report-dedupe counts the login's stage 0 of 2002b6e).
// The server's budget: every report this game sends passes a mirror of
// wire-validate's quest_stage_budget (QUEST_SEND_BURST, QUEST_SEND_PER_S,
// less QUEST_SEND_RESERVE): sent at once while a token is spare, else held,
// a stage ahead of every run word, and sent from the update pass as the
// bucket refills, so a burst is delayed here and never refused there; an
// episode that held anything says so in one line once its last report goes.
// The puppet rule's scene stop: PuppetService stops a scene the engine plays
// here on a host's puppet (playtest 41's Sven scene), and the engine then
// stops the scene's quest (c2 at 07:20:18: the start of 3a17c,
// MS13BleakFallsBarrowCampfireScene, its scene 3a17d stopped on 39fe4 and
// 39fe2, the quest's stop 0.32 s later at .522; c1 at 07:19:52 with 954da
// of 954d9, the stop 3.1 s later). That stop is this seat's own, never a
// word: reported, it would be recorded and relayed to the host, stopping
// the scene the host's live actors play (ADR-063: a non-owner renders,
// never simulates). PuppetService names the quest (its
// puppetSceneStopped event), and the quest's stops here go unreported until
// its next start here, said once a quest a connection: "quest <q>'s stop not
// reported: its scene <s> was stopped here by the puppet rule".
// A client-only plugin's quest: a quest of a plugin the server does not
// serve is this seat's own, a stage and a run word alike. The server
// refuses its report as E_QUEST_UNKNOWN before any judging (OnQuestStage's
// FormDesc::FromFormId), and in that read RL_DetectionQuest 0x30002859 of
// RLAiO.esp, a plugin the clients load and the server does not (its full
// plugins end at rotfern.esp, 0x2f), started and stopped 1,514 times on c1
// and 1,559 on c2. The server's plugin list is its own manifest.json, asked
// once a connection through SettingsService.getServerMods (the game
// server's resources port first, the master after), and a quest's plugin is
// read by this game's own load order: a full plugin by the form id's top
// byte, a light one at 0xFE by bits 12 to 23. The login never waits for the
// list: until the first one comes every quest is reported as before, and a
// connection whose list never comes stays so and says it once, "the
// server's plugin list unavailable (<reason>): every quest reported as
// before (F3 off)". A reconnect to the same server (its host and port,
// SettingsService.getServerAddress) keeps the last connection's list while
// its own ask is in flight, said once, "the server's plugin list kept from
// the last connection (<k> client-only plugin(s)) until the new answer".
// Cleared at the reconnect, as d8182c19 had it, the list would let
// RL_DetectionQuest's flips, about 2.6 a second, reach the server after a
// restart until the answer came, each an E_QUEST_UNKNOWN line that
// b-quest-stage-restart v2's refusal claims count (a fresh review of this
// branch, 2026-11-10: a read of the code, not a run). The answer replaces
// the kept list; a connection to another server, or an answer that says the
// list is unavailable, drops it.
// With a list: "the server serves <n> plugin(s): quests of <k> client-only
// plugin(s) are this seat's own (<names>)", and the skipped reports are
// counted, a line at most each SKIP_SAY_MS while the count moves and one at
// the connection's end.
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
        this.controller.emitter.on("puppetSceneStopped", (e) => this.onPuppetSceneStopped(e));
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
            // rung 5: the player-class quests, resolved as the list's entries are
            const player = (m.player ?? []).filter((id) => Number.isInteger(id) && id > 0);
            this.setShared(mode, ids);
            this.playerQuests = player;
            this.sp.storage[SHARED_STORAGE_KEY] = { mode, ids, player };
            logTrace(this, "the server shares quests in mode", QuestStageService.modeName(mode) + ":", ids.length, mode === MODE_ALL ? "quest(s) denied" : "quest(s) listed");
            this.tellPlayerQuests("at the login", true);
            return;
        }
        if (m.word !== undefined && m.word !== WORD_START && m.word !== WORD_STOP) {
            logError(this, "a quest word", m.word, "for quest", m.quest.toString(16), "is neither a start nor a stop; dropped");
            return;
        }
        // rule 5: an `acting` that names no player's actor is no stamp
        const acting = typeof m.acting === "number" && Number.isInteger(m.acting) && m.acting >= FIRST_SERVER_ID ? m.acting : undefined;
        // note 7: a word's start-up or shut-down stages, u16s
        const wordStages = m.word !== undefined && Array.isArray(m.wordStages)
            ? m.wordStages.filter((s) => Number.isInteger(s) && s >= 0 && s <= 0xffff)
            : undefined;
        if (m.word === WORD_START && wordStages !== undefined) {
            this.rememberStartUp(m.quest, wordStages);
        }
        this.pending.push({ quest: m.quest, stage: m.stage, word: m.word, acting, served: m.served === true, wordStages });
    }

    // a start word's start-up stages, by quest, for a later implicit start
    private rememberStartUp(questId: number, stages: number[]) {
        if (!this.startUpStages.has(questId) && this.startUpStages.size >= START_UP_MEMORY_MAX) {
            const oldest = this.startUpStages.keys().next().value;
            if (oldest !== undefined) {
                this.startUpStages.delete(oldest);
                logTrace(this, "start-up stages kept for", START_UP_MEMORY_MAX, "quests: quest", oldest.toString(16) + "'s let go");
            }
        }
        this.startUpStages.delete(questId);
        this.startUpStages.set(questId, stages);
    }

    // note 7 (D-Q3 A): a start's start-up stages or a stop's shut-down
    // stages, before Quest.Start or Quest.Stop: each an echo mark, and, when
    // the fragment is the world's already, the sink's suppress mark with its
    // safety release, as a replayed stage's; none named, nothing
    private markWordStages(quest: Quest, questId: number, stages: number[] | undefined, served: boolean) {
        if (!stages || stages.length === 0) {
            return;
        }
        const now = Date.now();
        const mark = this.marksFragment(served);
        for (const stage of stages) {
            this.applying.set(QuestStageService.key(questId, stage), now + BUSY_MAX_MS);
            if (mark && this.suppressFragment(quest, questId, stage)) {
                this.fragmentReleases.push({ questId, stage, at: now + FRAGMENT_SAFETY_MS });
            }
        }
    }

    // one word or stage per update, in the server's order (ascending within a
    // quest at a login's replay, so each SetStage sees the stages before it;
    // its starts first and its stops last), a quest's next one after the
    // engine answered its last
    private onUpdate() {
        // the login load's hold (x-quest-runs' read): the game is up here, as
        // it never is at the main menu
        this.updateSeen = true;
        this.arrival();
        const now = Date.now();
        this.flushRepeats(now);
        // the pacing: what the server's budget held, as the bucket refills
        this.drainSends(now);
        // the client-only plugins' list: a reloaded plugin keeps its
        // connection and hears no connectionAccepted, so its first update
        // asks; and the skipped reports' line
        if (this.pluginListAsked === 0) {
            this.askPluginList();
        }
        this.saySkipped(now);
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
            this.applyingRuns.set(runKey, Date.now() + BUSY_MAX_MS);
            this.nameActing(item, SCOPE_START);
            this.markWordStages(quest, questId, this.startUpStages.get(questId), item.served);
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
            this.applyingRuns.delete(runKey);
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
                const gone = this.parked.get(oldest);
                this.parked.delete(oldest);
                logLab(TAG, `parked stages full at ${PARKED_MAX}: the world's stage ${gone ? gone.stage : "?"} of quest ${oldest.toString(16)} let go for quest ${item.quest.toString(16)}'s`);
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
                logLab(TAG, `parked starts full at ${PARKED_MAX}: the world's start of quest ${oldest.toString(16)} let go for quest ${questId.toString(16)}'s`);
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
        // the puppet rule's scene stop (x-quest-runs' read): a start here ends
        // the quest's mark, whoever's start it is
        this.puppetScenes.delete(questId);
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
            this.applyingRuns.set(runKey, Date.now() + BUSY_MAX_MS);
            this.nameActing(item, SCOPE_START);
            this.markWordStages(quest, questId, item.wordStages, item.served);
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
        const stopKey = QuestStageService.runKey(questId, WORD_STOP);
        this.applyingRuns.set(stopKey, Date.now() + BUSY_MAX_MS);
        this.nameActing(item, SCOPE_STOP);
        this.markWordStages(quest, questId, item.wordStages, item.served);
        // the engine's stop event answers it (onQuestStop)
        this.busy.set(questId, Date.now() + BUSY_MAX_MS);
        try {
            quest.stop();
        } catch (err) {
            this.applyingRuns.delete(stopKey);
            this.busy.delete(questId);
            logError(this, "Quest.Stop failed on quest", questId.toString(16), err);
            return;
        }
        logTrace(this, "the world's stop of quest", questId.toString(16), "(" + how + ") applied");
    }

    // this game's own start or stop of a quest (no mark of the world's):
    // reported as the world's unless the login's load raised it (held and
    // counted for the arrival's line), the words are off, the quest is
    // listed, it is a stop after the puppet rule stopped the quest's scene
    // here, or the quest is held for churn; sent through the server's budget
    // (the class comment's x-quest-runs paragraph)
    private reportRun(questId: number, word: number) {
        if (this.loginLoad) {
            this.loginLoadHeld++;
            return;
        }
        if (!this.runsEnabled() || !this.reports(questId)) {
            return;
        }
        if (this.skipClientOnly(questId, true)) {
            return;
        }
        if (word === WORD_STOP && this.puppetScenes.has(questId)) {
            this.sayPuppetStop(questId);
            return;
        }
        if (this.churned(questId)) {
            return;
        }
        const message: QuestStageMessage = { t: MsgType.QuestStage, quest: questId, stage: 0, word };
        const acting = this.reportedActing(questId, word === WORD_START ? SCOPE_START : SCOPE_STOP);
        if (acting !== undefined) {
            message.acting = acting;
        }
        this.send(message);
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

    // the login load's hold is over (its arrival resolved, or a new
    // connection came first): the one line of what it held
    private endLoginLoad(why: string) {
        if (!this.loginLoad) {
            return;
        }
        this.loginLoad = false;
        logLab(TAG, `${this.loginLoadHeld} run event(s) of the login's load held: the template save's, not reported${why}`);
        this.loginLoadHeld = 0;
    }

    // the pacing: one of this game's reports to the server, into its lane (a
    // stage's or a run word's), then as many sent as the bucket spares, every
    // stage first; one still waiting is held, and the first held opens the
    // episode
    private send(message: QuestStageMessage) {
        const lane = message.word === undefined ? this.heldStages : this.heldWords;
        lane.push(message);
        const now = Date.now();
        this.drainSends(now);
        if (lane.length === 0 || lane[lane.length - 1] !== message) {
            return; // sent now
        }
        if (message.word === undefined) {
            this.heldStageCount++;
        } else {
            this.heldWordCount++;
        }
        if (this.heldSince === 0) {
            this.heldSince = now;
            logTrace(this, "the server's QuestStage budget is spent here: reports held, sent at", QUEST_SEND_PER_S, "a second, stages first");
        }
        if (this.heldStages.length + this.heldWords.length > QUEST_SEND_HELD_MAX) {
            (this.heldWords.length > 0 ? this.heldWords : this.heldStages).shift();
            if (this.heldDropped++ === 0) {
                logLab(TAG, `QuestStage pacing holds ${QUEST_SEND_HELD_MAX} report(s): the oldest held run word goes for each new one (the oldest stage when no word is held)`);
            }
        }
    }

    // the pacing's bucket credited for the time since its last credit (the
    // fractions carry, as the server's do), then the held reports sent while
    // a token is spare above the reserve, every stage before any run word; an
    // episode that held anything ends once nothing is held, in one line
    private drainSends(now: number) {
        if (this.heldStages.length === 0 && this.heldWords.length === 0) {
            return;
        }
        this.sendTokens = Math.min(QUEST_SEND_BURST, this.sendTokens + Math.max(0, now - this.sendCreditedAt) * QUEST_SEND_PER_S / 1000);
        this.sendCreditedAt = now;
        while (this.sendTokens >= QUEST_SEND_RESERVE + 1) {
            const next = this.heldStages.length > 0 ? this.heldStages.shift() : this.heldWords.shift();
            if (next === undefined) {
                break;
            }
            this.sendTokens -= 1;
            this.controller.emitter.emit("sendMessage", { message: next, reliability: "reliable" });
        }
        if (this.heldStages.length > 0 || this.heldWords.length > 0 || this.heldSince === 0) {
            return;
        }
        const held = this.heldStageCount + this.heldWordCount;
        const gone = this.heldDropped > 0 ? `${this.heldDropped} dropped past ${QUEST_SEND_HELD_MAX}, the rest sent` : "all sent";
        logLab(TAG, `QuestStage pacing: ${held} report(s) held under the server's budget (${QUEST_SEND_BURST} at once, then ${QUEST_SEND_PER_S} a second) for ${((now - this.heldSince) / 1000).toFixed(1)} s, ${this.heldStageCount} stage(s) and ${this.heldWordCount} run word(s); ${gone}`);
        this.heldSince = 0;
        this.heldStageCount = 0;
        this.heldWordCount = 0;
        this.heldDropped = 0;
    }

    // a new connection's budget is the server's anew (a full bucket a
    // connection), and the reports the old one held go with it, as a send
    // on a dropped connection does (NetworkingService hands each send to the
    // transport at once, with no queue of its own)
    private resetSends() {
        const left = this.heldStages.length + this.heldWords.length;
        if (left > 0) {
            logLab(TAG, `QuestStage pacing: ${left} held report(s) dropped at a new connection (${this.heldStages.length} stage(s), ${this.heldWords.length} run word(s)), lost with the old one`);
        }
        this.heldStages = [];
        this.heldWords = [];
        this.heldSince = 0;
        this.heldStageCount = 0;
        this.heldWordCount = 0;
        this.heldDropped = 0;
        this.sendTokens = QUEST_SEND_BURST;
        this.sendCreditedAt = 0;
    }

    // PuppetService stopped a scene of the quest here on a host's puppet
    // (its reconcile; JS state alone): the quest's stops here are this
    // seat's own until the quest's next start here
    private onPuppetSceneStopped(e: { quest: number; scene: number }) {
        if (!Number.isInteger(e.quest) || e.quest <= 0) {
            return;
        }
        if (!this.puppetScenes.has(e.quest) && this.puppetScenes.size >= PUPPET_SCENE_MEMORY_MAX) {
            const oldest = this.puppetScenes.keys().next().value;
            if (oldest !== undefined) {
                this.puppetScenes.delete(oldest);
            }
        }
        this.puppetScenes.set(e.quest, e.scene);
    }

    // the puppet rule's unreported stop, said once a quest a connection (a
    // trace after)
    private sayPuppetStop(questId: number) {
        const scene = this.puppetScenes.get(questId);
        const which = scene !== undefined && scene > 0 ? `its scene ${scene.toString(16)}` : "its scene";
        const line = `quest ${questId.toString(16)}'s stop not reported: ${which} was stopped here by the puppet rule`;
        if (this.puppetStopSaid.has(questId)) {
            logTrace(this, line);
            return;
        }
        if (this.puppetStopSaid.size >= PUPPET_SCENE_MEMORY_MAX) {
            const oldest = this.puppetStopSaid.values().next().value;
            if (oldest !== undefined) {
                this.puppetStopSaid.delete(oldest);
            }
        }
        this.puppetStopSaid.add(questId);
        logLab(TAG, line);
    }

    // the client-only plugins' list for this connection, asked once (at the
    // connection, or at the first update of a reloaded plugin): the server's
    // own manifest first, then the master (SettingsService.getServerMods).
    // Nothing waits for it; an answer to an earlier ask is dropped
    private askPluginList() {
        const asked = ++this.pluginListAsked;
        let settings: SettingsService;
        try {
            settings = this.controller.lookupListener(SettingsService);
        } catch (err) {
            this.pluginListMissing(`no SettingsService here: ${err}`);
            return;
        }
        let reason = "";
        const server = this.serverKey();
        settings.getServerMods(true, (why) => {
            reason = why;
        }).then((mods) => {
            if (asked === this.pluginListAsked) {
                this.takePluginList(mods, reason, server);
            }
        }).catch((err) => {
            if (asked === this.pluginListAsked) {
                this.pluginListMissing(`${err}`);
            }
        });
    }

    // the server's plugins against this game's own load order (the read's
    // mapping): a full plugin by its index, the form id's top byte, a light
    // one by its index at 0xFE, bits 12 to 23; client-only when the server's
    // manifest names no plugin of its name, case aside. The promise's answer
    // comes in the update pass, where the Game calls run
    // (LoadOrderVerificationService reads the light list there too)
    private takePluginList(mods: Mod[], reason: string, server: string) {
        const served = new Set<string>();
        for (const mod of Array.isArray(mods) ? mods : []) {
            const name = mod && typeof mod.filename === "string" ? mod.filename.toLowerCase() : "";
            if (name.endsWith(".esm") || name.endsWith(".esp") || name.endsWith(".esl")) {
                served.add(name);
            }
        }
        if (served.size === 0) {
            this.pluginListMissing(reason || "the manifest names no plugin");
            return;
        }
        const full = new Set<number>();
        const light = new Set<number>();
        const names: string[] = [];
        try {
            const game = this.sp.Game;
            const fullCount = game.getModCount();
            for (let i = 0; i < fullCount; i++) {
                const name = game.getModName(i);
                if (typeof name === "string" && !served.has(name.toLowerCase())) {
                    full.add(i);
                    names.push(name);
                }
            }
            const lightCount = game.getLightModCount();
            for (let i = 0; i < lightCount; i++) {
                const name = game.getLightModName(i);
                if (typeof name === "string" && !served.has(name.toLowerCase())) {
                    light.add(i);
                    names.push(name);
                }
            }
        } catch (err) {
            this.pluginListMissing(`this game's load order unreadable: ${err}`);
            return;
        }
        this.clientOnly = { full, light };
        this.clientOnlyServer = server;
        logLab(TAG, `the server serves ${served.size} plugin(s): quests of ${names.length} client-only plugin(s) are this seat's own (${names.length > 0 ? names.join(", ") : "none"})`);
    }

    // no list this connection: every quest reported as before (a list kept
    // from the last connection goes too), said once
    private pluginListMissing(reason: string) {
        this.clientOnly = undefined;
        this.clientOnlyServer = "";
        if (this.pluginListMissSaid) {
            return;
        }
        this.pluginListMissSaid = true;
        logLab(TAG, `the server's plugin list unavailable (${reason}): every quest reported as before (F3 off)`);
    }

    // a new connection: the last client-only list stays while this
    // connection's ask is in flight when the connection is with the server
    // the list came from (the same host and port), said once a reconnect; a
    // connection to another server, or one whose server is unknown, drops it
    private keepOrDropPluginList() {
        const list = this.clientOnly;
        if (!list) {
            return;
        }
        const server = this.serverKey();
        if (server !== "" && server === this.clientOnlyServer) {
            logLab(TAG, `the server's plugin list kept from the last connection (${list.full.size + list.light.size} client-only plugin(s)) until the new answer`);
            return;
        }
        this.clientOnly = undefined;
        this.clientOnlyServer = "";
    }

    // the game server this client connects to as "host:port"
    // (SettingsService's target peer, else the settings' address), "" when
    // none is named: the key a kept client-only list is matched by
    private serverKey(): string {
        try {
            const address = this.controller.lookupListener(SettingsService).getServerAddress();
            return address ? `${address.host}:${address.port}` : "";
        } catch (err) {
            return "";
        }
    }

    // whether the quest is a client-only plugin's by this game's own load
    // order, a skipped report counted when it is; false while no list holds
    private skipClientOnly(questId: number, word: boolean): boolean {
        const list = this.clientOnly;
        if (!list) {
            return false;
        }
        const index = questId >>> 24;
        const own = index === 0xfe ? list.light.has((questId >>> 12) & 0xfff) : index < 0xfe && list.full.has(index);
        if (!own) {
            return false;
        }
        if (word) {
            this.skippedWords++;
        } else {
            this.skippedStages++;
        }
        return true;
    }

    // the skipped reports' line: at most once each SKIP_SAY_MS, and only when
    // the count moved
    private saySkipped(now: number) {
        const total = this.skippedStages + this.skippedWords;
        if (total === this.skippedSaid || now < this.skipSayAt) {
            return;
        }
        this.skippedSaid = total;
        this.skipSayAt = now + SKIP_SAY_MS;
        logLab(TAG, `${total} report(s) of client-only plugins' quests skipped on this connection (${this.skippedStages} stage(s), ${this.skippedWords} run word(s)): this seat's own, never reported`);
    }

    // a connection's end: its last count said whole, then the counts start
    // over for the next
    private endSkipped() {
        if (this.skippedStages + this.skippedWords > this.skippedSaid) {
            this.skipSayAt = 0;
            this.saySkipped(Date.now());
        }
        this.skippedStages = 0;
        this.skippedWords = 0;
        this.skippedSaid = 0;
        this.skipSayAt = 0;
    }

    // the acting scope for a live apply (the class comment): the server's
    // acting player for the pair, its figure here or 0; none for a served
    // apply, for a relay with no stamp, or for this seat's own actor
    private nameActing(item: QuestItem, scopeStage: number) {
        const acting = item.acting;
        if (item.served || acting === undefined || this.actingExportsMissing || this.isMe(acting)) {
            return;
        }
        if (typeof this.sp.setActingPlayer !== "function") {
            this.actingExportsMissing = true;
            logLab(TAG, "setActingPlayer unavailable on this platform; a relayed request runs as this seat's own player");
            return;
        }
        const figure = remoteIdToLocalId(acting);
        try {
            if (this.sp.setActingPlayer(item.quest, scopeStage, figure, acting, ACTING_TTL_MS) !== true) {
                logError(this, "setActingPlayer refused quest", item.quest.toString(16), "stage", scopeStage, "for", acting.toString(16));
            }
        } catch (err) {
            logError(this, "setActingPlayer threw for quest", item.quest.toString(16), "stage", scopeStage, err);
        }
    }

    // the scope's player the platform names for a reported pair, a server id;
    // undefined for none (the sender is the acting player)
    private reportedActing(questId: number, scopeStage: number): number | undefined {
        if (this.actingExportsMissing) {
            return undefined;
        }
        if (typeof this.sp.getActingPlayer !== "function") {
            this.actingExportsMissing = true;
            logLab(TAG, "getActingPlayer unavailable on this platform; reports name no acting player");
            return undefined;
        }
        let id: unknown;
        try {
            id = this.sp.getActingPlayer(questId, scopeStage);
        } catch (err) {
            logError(this, "getActingPlayer threw for quest", questId.toString(16), "stage", scopeStage, err);
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
        if (this.skipClientOnly(questId, false)) {
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
        this.send(message);
        logTrace(this, "this game set stage", e.stage, "of shared quest", questId.toString(16) + "; reported" + QuestStageService.forActing(acting));
    }

    // the login's game load (LoadGameService's gameLoad, from the engine's
    // own load-game event, so the player's cell is attached): the other half
    // of the arrival's condition. A reconnect with the game up spawns through
    // MoveRefrToPosition and loads nothing, so it raises no arrival: that
    // seat's Story Manager heard its login's
    private onGameLoad() {
        this.arrivalGameLoaded = true;
        // rung 5: the platform's list of this seat's own per-player quests,
        // told again past the load (the platform clears its acting tables at a
        // game load; whether this one too is its own, unread here)
        this.tellPlayerQuests("after a game load", false);
    }

    // the server's CreateActor of this connection's own actor (isMe: a login,
    // a reconnect, a restart, as HostService reads it): the login half of
    // the arrival's condition. The game's load follows it on a fresh launch
    // (RemoteServer loads the save for the actor it was given). While no
    // update has run in this game yet it stands at the main menu, where
    // RemoteServer's spawn loads the template save (with the game up it moves
    // the player and loads nothing; LoadOrderVerificationService's first
    // update printed after the load in x-quest-runs' read): the login load's
    // hold opens, and every run event of this game until the arrival is that
    // save's
    private onCreateActor(e: ConnectionMessage<CreateActorMessage>) {
        if (!e.message.isMe) {
            return;
        }
        this.arrivalLoginSeen = true;
        if (!this.updateSeen && !this.loginLoad) {
            this.loginLoad = true;
            this.loginLoadHeld = 0;
        }
    }

    // a new connection (NetworkingService's connectionAccepted, before the
    // server's CreateActor): the once-per-login flags start over, so each
    // login's arrival is its own and none is carried across a reconnect; so
    // do rung 5's run marks, parked starts and churn holds (the server serves
    // the run state anew). The x-quest-runs rules: a login load's hold the
    // connection cut ends with its line, the pacing starts from the new
    // connection's full bucket, and the puppet rule's lines are said anew
    // (its marks stay: they are this engine's, not the connection's); the
    // last connection's skipped count is said whole, and the client-only
    // plugins' list is asked anew, the last one kept meanwhile on a
    // reconnect to the same server (keepOrDropPluginList)
    private onConnectionAccepted() {
        this.arrivalDone();
        this.applyingRuns.clear();
        this.parkedStarts.clear();
        this.busy.clear();
        this.runFlips.clear();
        this.heldRuns.clear();
        this.endLoginLoad(" (a new connection came before the arrival)");
        this.resetSends();
        this.puppetStopSaid.clear();
        this.endSkipped();
        this.keepOrDropPluginList();
        this.pluginListMissSaid = false;
        this.askPluginList();
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
            this.arrived();
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
            this.arrived();
            logTrace(this, "no location at the login's cell; no arrival raised (the engine's own event comes at the first location entered)");
            return;
        }
        this.arrived();
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

    // the login's arrival resolved (raised, given up, or switched off): its
    // flags go, and the login load's hold ends with its line
    private arrived() {
        this.arrivalDone();
        this.endLoginLoad("");
    }

    private arrivalEnabled(): boolean {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        return !s || s["storyArrivalAtLogin"] !== false;
    }

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

    // whether a stage or a run word this game raised on the quest is the
    // world's to report: nothing before the server's login message, never a
    // player-class quest (the login's `player`, D-Q1); in mode list the
    // listed quests; in mode all every quest but the denied (memo D58 B),
    // the start-game-enabled ones included since rung 5 retired memo D62's
    // `allowed` list and the server's own-start refusal (W1 B). A quest of a
    // plugin the server does not serve is skipped after this, once the
    // connection's plugin list came (skipClientOnly)
    private reports(questId: number): boolean {
        if (this.mode === undefined || this.playerQuests.indexOf(questId) >= 0) {
            return false; // nothing before the login's list; a player-class quest never
        }
        return this.mode === MODE_ALL ? !this.shared.has(questId) : this.shared.has(questId);
    }

    // rung 5: the player-class quests the login named, to the platform's
    // setPlayerQuests (this seat runs its own player's); the lab's line at the
    // login, a trace for a re-tell. An older platform without the export is
    // said once
    private tellPlayerQuests(when: string, lab: boolean) {
        if (this.playerQuestsMissing) {
            return;
        }
        if (typeof this.sp.setPlayerQuests !== "function") {
            this.playerQuestsMissing = true;
            logLab(TAG, "setPlayerQuests unavailable on this platform; this seat's own per-player quests stay behind the gate");
            return;
        }
        try {
            if (this.sp.setPlayerQuests([...this.playerQuests]) !== true) {
                logError(this, "setPlayerQuests refused", this.playerQuests.length, "quest(s)", when);
                return;
            }
        } catch (err) {
            logError(this, "setPlayerQuests threw", when, err);
            return;
        }
        if (lab) {
            logLab(TAG, `player quests: ${this.playerQuests.length} told to the platform ${when}`);
        } else {
            logTrace(this, "player quests:", this.playerQuests.length, "told to the platform", when);
        }
    }

    private setShared(mode: number, ids: number[]) {
        this.mode = mode;
        this.shared = new Set(ids);
    }

    // After a plugin reload: the mode and the list the last login brought,
    // from storage
    private resume() {
        const stored = this.sp.storage[SHARED_STORAGE_KEY] as { mode?: unknown; ids?: unknown; player?: unknown } | undefined;
        if (!stored || typeof stored !== "object" || !Array.isArray(stored.ids)) {
            return;
        }
        if (stored.mode !== MODE_LIST && stored.mode !== MODE_ALL) {
            return;
        }
        const ids = stored.ids.filter((id): id is number => typeof id === "number" && Number.isInteger(id) && id > 0);
        this.setShared(stored.mode, ids);
        logTrace(this, "resumed after a reload in mode", QuestStageService.modeName(stored.mode) + ":", ids.length, stored.mode === MODE_ALL ? "quest(s) denied" : "quest(s) listed");
        const player = Array.isArray(stored.player) ? stored.player : [];
        this.playerQuests = player.filter((id): id is number => typeof id === "number" && Number.isInteger(id) && id > 0);
        this.tellPlayerQuests("after a reload", false);
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
    // note 7: the start-up stages each quest's last start word named
    private startUpStages = new Map<number, number[]>();
    // the player-class quests the login named (each seat runs its own
    // player's), and an older platform without setPlayerQuests
    private playerQuests: number[] = [];
    private playerQuestsMissing = false;
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
    // x-quest-runs 20261110-071422's read. The login load's hold: open from
    // the login's CreateActor at the main menu to its arrival; the run events
    // it held; whether an update has run in this game yet (none does at the
    // main menu)
    private loginLoad = false;
    private loginLoadHeld = 0;
    private updateSeen = false;
    // the pacing: the mirrored bucket's tokens and the time they were last
    // credited at; the reports waiting for a token, stages and run words
    // apart, each in its order; the episode's start and its counts
    private sendTokens = QUEST_SEND_BURST;
    private sendCreditedAt = 0;
    private heldStages: QuestStageMessage[] = [];
    private heldWords: QuestStageMessage[] = [];
    private heldSince = 0;
    private heldStageCount = 0;
    private heldWordCount = 0;
    private heldDropped = 0;
    // the puppet rule's scene stop: the quests whose scene PuppetService
    // stopped here on a host's puppet (the scene by quest), each until the
    // quest's next start here, and the quests whose unreported stop this
    // connection said
    private puppetScenes = new Map<number, number>();
    private puppetStopSaid = new Set<number>();
    // a client-only plugin's quest: the client-only plugins by this game's
    // own load order (a full plugin by its index, a light one by its index
    // at 0xFE) from the last answer, kept across a reconnect to the same
    // server until the new answer, and undefined before the first, after an
    // answer that says the list is unavailable, and on another server's
    // connection (every quest reported as before); the server the list came
    // from, "host:port"; the asks so far (an answer to an earlier one is
    // dropped); whether this connection said its fallback line; the skipped
    // reports and their line's state
    private clientOnly: { full: Set<number>; light: Set<number> } | undefined;
    private clientOnlyServer = "";
    private pluginListAsked = 0;
    private pluginListMissSaid = false;
    private skippedStages = 0;
    private skippedWords = 0;
    private skippedSaid = 0;
    private skipSayAt = 0;
}
