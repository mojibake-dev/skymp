import { Quest, QuestStageEvent, QuestStartStopEvent } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { QuestStageMessage } from "../messages/questStageMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

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

// How many quests may hold a parked stage at once; the oldest parking makes
// room for a new quest
const PARKED_MAX = 64;

// How long after the login's game load the arrival waits for the player's
// cell to carry a location before it gives up, ms (the engine attaches the
// player's cell before its load-game event, so the first update should read
// it; the window is a hedge the trace line reports)
const ARRIVAL_WINDOW_MS = 3000;

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
// seat's own. In mode all a quest the game starts by itself (the record's
// Start Game Enabled flag: a layer's controller, MCM, version-tracker and
// player-alias quests, the alternate start's own) is each seat's own too
// unless the server's `allowed` list names it (memo D62); that rule is the
// server's alone and this seat does not filter for it: Skyrim Platform's
// Quest has no getter for the flag (skyrimPlatform.ts knows isRunning,
// isStopped, isStarting, isStopping and isActive) and the login's one
// `shared` list carries the denied set, so such a stage is reported as any
// other and the server refuses it (E_QUEST_OWN_START), the asymmetry the
// server holds. A quest that refuses Quest.Start has the world's
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
// the world effect) ran on the talker's game already, so the pair is handed
// to Skyrim Platform's replacement of the VM's quest stage sink
// (TESModPlatform.SuppressQuestStageFragment, FridaHooks.cpp) before the
// SetStage and taken back at the latent call's answer, set or refused or
// thrown; the journal entry, the done bit and the promise stay the engine's.
// This seat's own stages (the report path, onQuestStage) never pass there.
// Off under the questFragmentSuppress setting (default on). HYPOTHESIS until
// lab/frida/quest-stage-chain.js reads the chain.
export class QuestStageService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("questStageMessage", (e) => this.onMessage(e));
        this.controller.on("questStage", (e) => this.onQuestStage(e));
        this.controller.on("questStart", (e) => this.onQuestStart(e));
        this.controller.on("update", () => this.onUpdate());
        this.controller.emitter.on("gameLoad", () => this.onGameLoad());
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
        this.controller.emitter.on("connectionAccepted", () => this.onConnectionAccepted());
        this.resume();
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
        this.pending.push({ quest: m.quest, stage: m.stage });
    }

    // one stage per update, in the server's order (ascending within a quest
    // at a login's replay, so each SetStage sees the stages before it)
    private onUpdate() {
        this.arrival();
        if (!this.enabled()) {
            this.pending = [];
            return;
        }
        const next = this.pending.shift();
        if (next) {
            this.apply(next.quest, next.stage);
        }
        if (this.applying.size > 0) {
            const now = Date.now();
            this.applying.forEach((until, key) => {
                if (until < now) {
                    this.applying.delete(key);
                }
            });
        }
    }

    private apply(questId: number, stage: number) {
        const quest = this.sp.Quest.from(this.sp.Game.getFormEx(questId));
        if (!quest) {
            if (!this.missing.has(questId)) {
                this.missing.add(questId);
                logError(this, "the world's quest", questId.toString(16), "is not a quest in this game");
            }
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
        // (numidium's driver does the same under set-stage {start: true})
        const started: Promise<boolean> = quest.isRunning() ? Promise.resolve(true) : quest.start();
        started.then((startOk) => {
            if (!startOk) {
                this.applying.delete(key);
                // Quest.Start answers false by design on an event quest (the
                // Story Manager's to start) and on a quest whose aliases or
                // conditions fail; the stage waits for the engine's own start
                this.park(questId, stage);
                logTrace(this, "the world's stage", stage, "of quest", questId.toString(16), "parked until the engine starts it");
                return;
            }
            return this.setStage(questId, stage, key);
        }).catch((err) => {
            this.applying.delete(key);
            logError(this, "SetStage failed on quest", questId.toString(16), "stage", stage, err);
        });
    }

    // the engine's SetStage under the echo mark the caller set, so the
    // engine's own stage event from this apply is consumed, never reported.
    // The quest is looked up again here: a wrapper from an earlier frame is
    // out of Skyrim Platform's object pool after a latent call's answer
    // ("Invalid _skyrimPlatform_indexInPool", the lab driver's set-stage,
    // playtest 37 at 05:59Z)
    private setStage(questId: number, stage: number, key: string): Promise<void> {
        const quest = this.sp.Quest.from(this.sp.Game.getFormEx(questId));
        if (!quest) {
            this.applying.delete(key);
            logError(this, "the world's quest", questId.toString(16), "is gone before its stage", stage, "was set");
            return Promise.resolve();
        }
        // slice two: the pair is the native sink's from here to the answer
        const suppressed = this.suppressFragment(quest, questId, stage);
        return quest.setCurrentStageID(stage).then((ok) => {
            if (suppressed) {
                this.releaseFragment(questId, stage);
            }
            logTrace(this, "the world's stage", stage, "of quest", questId.toString(16), ok ? "set" : "refused by the engine");
            if (!ok) {
                this.applying.delete(key);
            }
        }, (err) => {
            if (suppressed) {
                this.releaseFragment(questId, stage);
            }
            throw err;
        });
    }

    // Slice two's hand-over of a pair to the native sink (see the class
    // comment). Both calls run where the SetStage runs, inside the update
    // tick's promise chain, the VM context callNative needs; the release
    // looks the quest up again, because the wrapper from before the latent
    // call is out of Skyrim Platform's object pool (as setStage above). A
    // refusal or a throw leaves the fragment the engine's, logged, and the
    // stage still applies.
    private suppressFragment(quest: Quest, questId: number, stage: number): boolean {
        if (!this.fragmentSuppressEnabled()) {
            return false;
        }
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
    // the map is full
    private park(questId: number, stage: number) {
        const had = this.parked.get(questId);
        if (had !== undefined) {
            if (stage > had) {
                this.parked.set(questId, stage);
            }
            return;
        }
        if (this.parked.size >= PARKED_MAX) {
            const oldest = this.parked.keys().next().value;
            if (oldest !== undefined) {
                this.parked.delete(oldest);
            }
        }
        this.parked.set(questId, stage);
    }

    // the engine's own start of a quest in this game (Skyrim Platform's
    // questStart, sent from its update task, so game calls are allowed here):
    // a stage parked for the quest is applied now, the same way as any other
    private onQuestStart(e: QuestStartStopEvent) {
        if (this.parked.size === 0 || !this.enabled() || !e.quest) {
            return;
        }
        const questId = e.quest.getFormID();
        const stage = this.parked.get(questId);
        if (stage === undefined) {
            return;
        }
        this.parked.delete(questId);
        if (e.quest.isStageDone(stage)) {
            return; // the start itself got there (a startup stage), nothing to set
        }
        const key = QuestStageService.key(questId, stage);
        this.applying.set(key, Date.now() + APPLY_ECHO_MS);
        logTrace(this, "the engine started quest", questId.toString(16) + ", the world's parked stage", stage, "applied");
        this.setStage(questId, stage, key).catch((err) => {
            this.applying.delete(key);
            logError(this, "SetStage failed on quest", questId.toString(16), "stage", stage, err);
        });
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
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.QuestStage, quest: questId, stage: e.stage },
            reliability: "reliable",
        });
        logTrace(this, "this game set stage", e.stage, "of shared quest", questId.toString(16) + "; reported");
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
    // login's arrival is its own and none is carried across a reconnect
    private onConnectionAccepted() {
        this.arrivalDone();
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

    // slice two's switch: a relayed stage keeps its fragment here when false
    private fragmentSuppressEnabled(): boolean {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        return !s || s["questFragmentSuppress"] !== false;
    }

    private static key(questId: number, stage: number): string {
        return questId.toString(16) + ":" + stage;
    }

    // the sharing mode the server named at the login (0 list, 1 all); none
    // before it, and nothing is reported until then
    private mode: number | undefined;
    // the quests the login named (form ids, the load order both ends share):
    // the shared quests in mode list, the denied quests in mode all
    private shared = new Set<number>();
    // the world's stages still to apply, in the server's order
    private pending: Array<{ quest: number; stage: number }> = [];
    // the stages this service is setting, by quest and stage, until the
    // engine's event echoes or the mark expires
    private applying = new Map<string, number>();
    // the highest stage the world asked for, by quest, on the quests that
    // refused Quest.Start here; applied at the engine's own start of each
    private parked = new Map<number, number>();
    private missing = new Set<number>();
    // the login's arrival: this connection's own actor has been created; the
    // game has loaded; the window's end once both hold (0 until then). All
    // three start over at every connectionAccepted
    private arrivalLoginSeen = false;
    private arrivalGameLoaded = false;
    private arrivalDeadline = 0;
}
