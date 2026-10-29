import { QuestStageEvent } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { QuestStageMessage } from "../messages/questStageMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// The storage key of the shared list (sp.storage survives a plugin reload,
// ADR-041's survivor): the list comes once, at the login, and a hot-reloaded
// client keeps its connection and never logs in again
const SHARED_STORAGE_KEY = "thuum.questStage.shared.v1";

// How long a stage this service set is expected to echo back as the engine's
// own quest-stage event, ms; after that the mark is dropped
const APPLY_ECHO_MS = 5000;

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
// reported before the server's list arrives, and a quest off the list stays
// this seat's own, as today.
export class QuestStageService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("questStageMessage", (e) => this.onMessage(e));
        this.controller.on("questStage", (e) => this.onQuestStage(e));
        this.controller.on("update", () => this.onUpdate());
        this.resume();
    }

    private onMessage(e: ConnectionMessage<QuestStageMessage>) {
        const m = e.message;
        if (m.shared && m.shared.length > 0) {
            this.shared = new Set(m.shared.filter((id) => Number.isInteger(id) && id > 0));
            this.sp.storage[SHARED_STORAGE_KEY] = Array.from(this.shared);
            logTrace(this, "the server shares", this.shared.size, "quest(s)");
        }
        if (m.quest !== 0) {
            this.pending.push({ quest: m.quest, stage: m.stage });
        }
    }

    // one stage per update, in the server's order (ascending within a quest
    // at a login's replay, so each SetStage sees the stages before it)
    private onUpdate() {
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
            return; // the engine has it already: idempotent, nothing to report
        }
        this.applying.set(QuestStageService.key(questId, stage), Date.now() + APPLY_ECHO_MS);
        quest.setCurrentStageID(stage).then((ok) => {
            logTrace(this, "the world's stage", stage, "of quest", questId.toString(16), ok ? "set" : "refused by the engine");
            if (!ok) {
                this.applying.delete(QuestStageService.key(questId, stage));
            }
        }).catch((err) => {
            this.applying.delete(QuestStageService.key(questId, stage));
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
        if (!this.shared.has(questId)) {
            return;
        }
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.QuestStage, quest: questId, stage: e.stage },
            reliability: "reliable",
        });
        logTrace(this, "this game set stage", e.stage, "of shared quest", questId.toString(16) + "; reported");
    }

    // After a plugin reload: the list the last login brought, from storage
    private resume() {
        const stored: unknown = this.sp.storage[SHARED_STORAGE_KEY];
        if (!Array.isArray(stored) || stored.length === 0) {
            return;
        }
        const list = stored.filter((id): id is number => typeof id === "number" && Number.isInteger(id) && id > 0);
        if (list.length > 0) {
            this.shared = new Set(list);
            logTrace(this, "resumed with", this.shared.size, "shared quest(s) after a reload");
        }
    }

    private enabled(): boolean {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        return !s || s["questStage"] !== false;
    }

    private static key(questId: number, stage: number): string {
        return questId.toString(16) + ":" + stage;
    }

    // the quests the server shares (form ids, the load order both ends share)
    private shared = new Set<number>();
    // the world's stages still to apply, in the server's order
    private pending: Array<{ quest: number; stage: number }> = [];
    // the stages this service is setting, by quest and stage, until the
    // engine's event echoes or the mark expires
    private applying = new Map<string, number>();
    private missing = new Set<number>();
}
