import { Actor, DxScanCode, Game, MenuCloseEvent, ObjectReference, ButtonEvent } from "skyrimPlatform";
import { logLab, logTrace } from "../../logging";
import { MsgType } from "../../messages";
import { ActorClaimMessage } from "../messages/actorClaimMessage";
import { MessageWithRefrId } from "../events/sendMessageWithRefrIdEvent";
import { localIdToRemoteId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
import { PuppetService } from "./puppetService";

// thuum docs/verbs/npc-talk.md (ADR-052): a guest opens dialogue with an NPC
// another game simulates. The Activate key on a puppet NPC under the
// crosshair sends ActorClaim (the engine's activate event never fires for a
// blocked activation, so the key is the trigger); when the server's word
// names this game (the reconciler promotes the actor: AI on, activation
// unblocked), one update later the player's own activation of its crosshair
// pick runs through the fork's activatePickRef (PlayerCharacter::
// ActivatePickRef), which opens the dialogue as the player's E would; at the
// Dialogue Menu's close the claim is released and the cell's rule names the
// host again. A claim the word never answers is forgotten after 3 s (the
// server's refusal is its log line). The setting npcTalk switches it; the
// key is talkKey (DxScanCode, 18 by default, E).
export class TalkService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        const settings = this.sp.settings["skymp5-client"];
        this.enabled = settings["npcTalk"] !== false;
        const key = settings["talkKey"];
        this.key = typeof key === "number" ? key : 18;
        if (!this.enabled) {
            logTrace(this, "npcTalk is off in settings: no claim goes out, a guest's E on a puppet does nothing");
            return;
        }
        this.controller.on("buttonEvent", (e) => this.onButton(e));
        this.controller.on("menuClose", (e) => this.onMenuClose(e));
        this.controller.on("update", () => this.onUpdate());
    }

    private onButton(e: ButtonEvent) {
        if (e.code !== (this.key as DxScanCode) || !e.isPressed || e.heldDuration > 0) {
            return;
        }
        if (this.pending || this.claimed) {
            return;
        }
        const refr = Game.getCurrentCrosshairRef();
        const actor = refr ? Actor.from(refr) : null;
        if (!actor) {
            return;
        }
        const localId = actor.getFormID();
        if (localId === 0x14 || !this.controller.lookupListener(PuppetService).isPuppet(localId)) {
            return;
        }
        const base = actor.getBaseObject();
        const baseId = base ? base.getFormID() : 0;
        if (!baseId || baseId === 0x7 || baseId >= 0xff000000) {
            return;
        }
        const remoteId = localIdToRemoteId(localId);
        if (!remoteId) {
            return;
        }
        this.send(remoteId, true);
        this.pending = { localId, remoteId, since: Date.now() };
        logLab("verb:npc-talk", `claim ${remoteId.toString(16)} (local ${localId.toString(16)}) sent on E`);
    }

    private onUpdate() {
        if (!this.pending) {
            return;
        }
        const { localId, remoteId, since } = this.pending;
        const host = this.controller.lookupListener(HostService);
        if (host.isMineLocal(localId)) {
            // the word has named this game and the reconciler promoted the
            // actor on this update; its AI ticks once before the activation
            if (this.promotedAt === 0) {
                this.promotedAt = Date.now();
                return;
            }
            if (Date.now() - this.promotedAt < 100) {
                return;
            }
            const ref = Game.getCurrentCrosshairRef();
            const under = ref ? ref.getFormID() : 0;
            const queued = this.sp.activatePickRef();
            logLab("verb:npc-talk", `claim ${remoteId.toString(16)} granted: activatePickRef ${queued ? "queued" : "refused"} (crosshair ${under.toString(16)})`);
            this.claimed = { localId, remoteId };
            this.pending = undefined;
            this.promotedAt = 0;
            return;
        }
        if (Date.now() - since > 3000) {
            logLab("verb:npc-talk", `claim ${remoteId.toString(16)} unanswered after 3 s, forgotten (the server's refusal is its line)`);
            this.pending = undefined;
            this.promotedAt = 0;
        }
    }

    private onMenuClose(e: MenuCloseEvent) {
        if (e.name !== "Dialogue Menu" || !this.claimed) {
            return;
        }
        const { remoteId } = this.claimed;
        this.send(remoteId, false);
        logLab("verb:npc-talk", `claim ${remoteId.toString(16)} released at the Dialogue Menu's close`);
        this.claimed = undefined;
    }

    private send(remoteId: number, claim: boolean) {
        const message: MessageWithRefrId<ActorClaimMessage> = {
            t: MsgType.ActorClaim,
            claim,
            _refrId: remoteId,
        };
        this.controller.emitter.emit("sendMessageWithRefrId", { message, reliability: "reliable" });
    }

    private pending?: { localId: number; remoteId: number; since: number };
    private claimed?: { localId: number; remoteId: number };
    private promotedAt = 0;
    private readonly enabled: boolean;
    private readonly key: number;
}
