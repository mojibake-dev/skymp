import { Actor, Game, isPuppet } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { ConnectionMessage } from "../events/connectionMessage";
import { ActorSpeechMessage } from "../messages/actorSpeechMessage";
import { RemoteServer } from "./remoteServer";
import { HostService } from "./hostService";
import { remoteIdToLocalId } from "../../view/worldViewMisc";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { puppetGazeHeld } from "../../sync/puppetGaze";
import { logLab, logTrace } from "../../logging";

type Pos = ReturnType<typeof ObjectReferenceEx.getPos>;

// thuum memo D53 (playtest 36, Eli: "it needs to work for NPCS talking to
// eachother on both"): two puppets in a conversation face each other on the
// observer's screen. The host's engine relays a gaze target only while it
// tracks one, and it tracks heads near its own player alone (H-54d), so two
// residents trading lines far from their host stood stiff here, or looked at
// the local player (D50's local gaze). The conversation names the target:
// when a puppet's relayed line arrives, its partner is the most recent OTHER
// puppet speaker within partnerMs and partnerUnits; the speaker looks at the
// partner and the partner at the speaker for holdMs after the line, the pair
// marked held so the local gaze stands aside; a relayed target already held
// wins and is never overridden. At the hold's end the heads are released
// and the local gaze resumes on the next sample. Nothing crosses the wire.
export class ConversationGazeService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.enabled = this.sp.settings["skymp5-client"]["conversationGaze"] !== false;
        this.controller.emitter.on("actorSpeechMessage", (e) => this.onActorSpeech(e));
        this.controller.on("update", () => this.onUpdate());
    }

    private onActorSpeech(event: ConnectionMessage<ActorSpeechMessage>) {
        if (!this.enabled) {
            return;
        }
        const msg = event.message;
        this.controller.once("update", () => {
            const world = this.controller.lookupListener(RemoteServer).getWorldModel();
            const form = world.forms.find((f) => f !== undefined && f.idx === msg.idx);
            if (!form || form.refrId === undefined) {
                return;
            }
            const localId = remoteIdToLocalId(form.refrId);
            if (!localId || localId === 0x14 || !isPuppet(localId)) {
                return;
            }
            if (this.controller.lookupListener(HostService).isMineLocal(localId)) {
                return;
            }
            const speaker = Actor.from(Game.getFormEx(localId));
            if (!speaker || speaker.isDead()) {
                return;
            }
            const now = Date.now();
            const pos = ObjectReferenceEx.getPos(speaker);
            const partnerId = this.partnerOf(localId, pos, now);
            this.recent.set(localId, { at: now, pos });
            if (partnerId === undefined) {
                return;
            }
            const partner = Actor.from(Game.getFormEx(partnerId));
            if (!partner || partner.isDead()) {
                return;
            }
            this.face(localId, speaker, partnerId, partner, now);
            this.face(partnerId, partner, localId, speaker, now);
            logLab("verb:npc-stream", `gaze: ${localId.toString(16)} and ${partnerId.toString(16)} face each other (a conversation)`);
        });
    }

    // the most recent other puppet speaker near enough and recent enough
    private partnerOf(speaker: number, pos: Pos, now: number): number | undefined {
        let best: { id: number; at: number } | undefined;
        for (const [id, seen] of Array.from(this.recent.entries())) {
            if (now - seen.at > ConversationGazeService.partnerMs) {
                this.recent.delete(id);
                continue;
            }
            if (id === speaker) {
                continue;
            }
            if (ObjectReferenceEx.getDistanceNoZ(pos, seen.pos) > ConversationGazeService.partnerUnits) {
                continue;
            }
            if (!best || seen.at > best.at) {
                best = { id, at: seen.at };
            }
        }
        return best?.id;
    }

    // one head toward the other, unless a relayed target already holds it
    private face(id: number, actor: Actor, targetId: number, target: Actor, now: number) {
        const mine = this.holding.has(id);
        if (puppetGazeHeld.has(id) && !mine) {
            logTrace(this, `gaze: ${id.toString(16)} keeps its relayed target`);
            return;
        }
        if (!mine || this.holding.get(id)?.target !== targetId) {
            actor.setHeadTracking(true);
            actor.setLookAt(target, false);
        }
        puppetGazeHeld.add(id);
        this.holding.set(id, { target: targetId, until: now + ConversationGazeService.holdMs });
    }

    private onUpdate() {
        const now = Date.now();
        if (now - this.lastSweep < 500) {
            return;
        }
        this.lastSweep = now;
        for (const [id, hold] of Array.from(this.holding.entries())) {
            if (now < hold.until) {
                continue;
            }
            this.holding.delete(id);
            puppetGazeHeld.delete(id);
            const actor = Actor.from(Game.getFormEx(id));
            if (actor && isPuppet(id)) {
                actor.clearLookAt();
            }
            logTrace(this, `gaze: ${id.toString(16)} released after the conversation`);
        }
    }

    private readonly enabled: boolean;
    private readonly recent = new Map<number, { at: number; pos: Pos }>();
    private readonly holding = new Map<number, { target: number; until: number }>();
    private lastSweep = 0;
    private static readonly partnerMs = 8000;
    private static readonly partnerUnits = 700;
    private static readonly holdMs = 7000;
}
