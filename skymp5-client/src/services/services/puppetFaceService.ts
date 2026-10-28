import { Actor, Game, ObjectReference } from "skyrimPlatform";
import { logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { HostedActorStateMessage } from "../messages/hostedActorStateMessage";
import { getObjectReference, remoteIdToLocalId } from "../../view/worldViewMisc";
import { puppetGazeHeld } from "../../sync/puppetGaze";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RemoteServer } from "./remoteServer";

// thuum docs/verbs/npc-stream.md (slice 5, the gaze and the face, D30): the
// host's state for an actor, relayed by the server to the actor's other
// listeners, carries the head-track target the host's engine tracks and the
// emotion it wears. A puppet here aims at the same target (setLookAt on the
// resolved actor; the movement apply's point heuristic stands aside while a
// target is named) and wears the host's emotion through the engine's
// expression override (setExpressionOverride, SKSE's mood index: the engine's
// EmotionType mapped onto the Mood expressions, CK wiki "SetExpressionOverride
// - Actor": 7 MoodNeutral, 8 MoodAnger, 9 MoodFear, 10 MoodHappy, 11 MoodSad,
// 12 MoodSurprise, 13 MoodPuzzled, 14 MoodDisgust; HYPOTHESIS until the probe
// reads the puppet's face). The message's percentages are ignored here: the
// server's judged ChangeValues carry them.
const MOOD_BY_EMOTION = [7, 8, 14, 9, 11, 10, 12, 13];

export class PuppetFaceService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("hostedActorStateMessage", (e) => this.onState(e));
    }

    private onState(event: ConnectionMessage<HostedActorStateMessage>) {
        const msg = event.message;
        this.controller.once("update", () => {
            const remote = this.controller.lookupListener(RemoteServer);
            const id = remote.getIdManager().getId(msg.idx);
            const refr = getObjectReference(id);
            const actor = refr ? Actor.from(refr) : null;
            if (!actor) {
                return;
            }
            const localId = actor.getFormID();
            if (!this.sp.isPuppet(localId)) {
                return;
            }
            // the gaze
            const targetLocal = msg.gazeTarget ? remoteIdToLocalId(msg.gazeTarget) : 0;
            const target = targetLocal ? ObjectReference.from(Game.getFormEx(targetLocal)) : null;
            const last = this.last.get(localId);
            if (target) {
                if (!last || last.target !== targetLocal) {
                    actor.setHeadTracking(true);
                    actor.setLookAt(target, false);
                    puppetGazeHeld.add(localId);
                    logLab("verb:npc-stream", `face: ${localId.toString(16)} gaze -> ${targetLocal.toString(16)} (slot ${msg.gazeSlot})`);
                }
            } else if (puppetGazeHeld.has(localId)) {
                puppetGazeHeld.delete(localId);
                actor.clearLookAt();
                logTrace(this, `face: ${localId.toString(16)} gaze cleared`);
            }
            // the face: recorded, no longer imposed. The re-analyst's read of
            // 2026-10-28 (docs/research/m3/re-puppet-head-lips.md B): the
            // engine's expression override is written only by the Papyrus
            // native and the console, never by its own dialogue, and while
            // set it blocks the engine's per-line expression (37544 state 3,
            // which runs on the puppet inside our voice pair too) and the
            // lip-synced line's emotion on the live NPC after a promote (our
            // leftover: exprOverride true on the talker's own Orgnar). So a
            // puppet wears what its say gives it, and any override this
            // seat set earlier is cleared once (playtest 37)
            const type = Math.min(7, Math.max(0, msg.emotionType | 0));
            const value = Math.min(100, Math.max(0, msg.emotionValue | 0));
            if (!this.cleared.has(localId)) {
                actor.clearExpressionOverride();
                this.cleared.add(localId);
                logLab("verb:npc-stream", `face: ${localId.toString(16)} expression override cleared (the engine's own per-line expression from here)`);
            }
            if (!last || last.type !== type || Math.abs(last.value - value) >= 10) {
                logLab("verb:npc-stream", `face: ${localId.toString(16)} expression ${type} ${value} recorded (mood ${MOOD_BY_EMOTION[type] ?? 7}, not imposed)`);
            }
            this.last.set(localId, { target: targetLocal, type, value });
        });
    }

    private readonly last = new Map<number, { target: number; type: number; value: number }>();
    private readonly cleared = new Set<number>();
}
