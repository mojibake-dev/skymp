import { Actor, DxScanCode, Game, MenuCloseEvent, Ui, ButtonEvent } from "skyrimPlatform";
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
// Playtest 35 (2026-10-27): a guest's granted claim was never released (the
// claimed resident walked across a cell border, the server's follow rule
// ended the claim with no word, and the menu's close event never came), and
// every later E on another puppet returned silently behind the held claim.
// So the held claim is watched every update: the word ending it (the lease,
// a pass, a release) forgets it, the Dialogue Menu seen open and then closed
// releases it (the close event or not), a menu that never opens within ten
// seconds releases it, and E on another puppet releases it first; every
// decline of an E leaves a trace.
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
        if (this.pending) {
            logTrace(this, `E: claim ${this.pending.remoteId.toString(16)} still pending, ignored`);
            return;
        }
        const refr = Game.getCurrentCrosshairRef();
        const actor = refr ? Actor.from(refr) : null;
        if (!actor) {
            logTrace(this, "E: nothing or no actor under the crosshair");
            return;
        }
        const localId = actor.getFormID();
        if (localId === 0x14) {
            return;
        }
        if (this.claimed && this.claimed.localId === localId) {
            // E on the claimed actor: the engine's own activation, the dialogue
            return;
        }
        if (!this.controller.lookupListener(PuppetService).isPuppet(localId)) {
            logTrace(this, `E: ${localId.toString(16)} is no puppet here (live or a figure), the engine's own activation`);
            return;
        }
        if (actor.isDead()) {
            // a body is looted, never talked to: the engine's own activation
            // (playtest 36: E on a corpse sent a claim the server refused,
            // E_CLAIM_DEAD, and the seat waited 3 s on it)
            logTrace(this, `E: ${localId.toString(16)} is dead, the engine's own activation (a loot)`);
            return;
        }
        const base = actor.getBaseObject();
        const baseId = base ? base.getFormID() : 0;
        if (!baseId || baseId === 0x7 || baseId >= 0xff000000) {
            logTrace(this, `E: ${localId.toString(16)} base ${baseId.toString(16)} is not a claimable NPC`);
            return;
        }
        const remoteId = localIdToRemoteId(localId);
        if (!remoteId) {
            logTrace(this, `E: ${localId.toString(16)} has no remote id`);
            return;
        }
        if (this.claimed) {
            this.release("a new target under E");
        }
        this.send(remoteId, true);
        this.pending = { localId, remoteId, since: Date.now() };
        logLab("verb:npc-talk", `claim ${remoteId.toString(16)} (local ${localId.toString(16)}) sent on E`);
    }

    private onUpdate() {
        if (this.claimed) {
            this.watchClaim();
        }
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
            this.claimed = { localId, remoteId, since: Date.now(), renewedAt: Date.now(), menuSeen: false, retries: 0 };
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

    // the held claim's end, read every update: the word ending it (the lease,
    // a pass, a release the server made) forgets it; the Dialogue Menu seen
    // open and then closed releases it whether or not the close event came
    // (playtest 35: it did not); a menu that never opens releases it after
    // menuWaitMs
    private watchClaim() {
        const held = this.claimed;
        if (!held) {
            return;
        }
        if (!this.controller.lookupListener(HostService).isMineLocal(held.localId)) {
            logLab("verb:npc-talk", `claim ${held.remoteId.toString(16)} ended by the word (the lease, a pass or a release): forgotten`);
            this.claimed = undefined;
            return;
        }
        if (Ui.isMenuOpen("Dialogue Menu")) {
            held.menuSeen = true;
            // playtest 36 (2026-10-28 17:55Z to 17:58Z): a dialogue of 175 s
            // outlived the claim's 120 s lease (wire-rules CLAIM_LEASE_MS); the
            // server renews a claim its holder sends again (ClaimVerdict
            // Reclaimed), so the held claim is sent again every renewMs while
            // the menu stays open, and the lease is the safety net alone
            const now = Date.now();
            if (now - held.renewedAt >= TalkService.renewMs) {
                held.renewedAt = now;
                this.send(held.remoteId, true);
                logLab("verb:npc-talk", `claim ${held.remoteId.toString(16)} renewed, the Dialogue Menu open ${((now - held.since) / 1000).toFixed(0)} s`);
            }
            return;
        }
        if (held.menuSeen) {
            this.release("the Dialogue Menu's close");
            return;
        }
        // x-npc-talk-cross 20261028-131626 and the sweep's 123118: the claim
        // granted, the actor live, activatePickRef ran (the engine's activation
        // fired), and no Dialogue Menu opened; in 131626 the resident had begun
        // a greeting 33 ms before. The activation is asked again at one and two
        // seconds after the grant while the menu stays shut (HYPOTHESIS on the
        // cause: a greeting in flight or the AI's first frame); the ten-second
        // release stands after that
        const waited = Date.now() - held.since;
        if (held.retries < TalkService.activateRetries && waited > 1000 * (held.retries + 1)) {
            held.retries += 1;
            const queued = this.sp.activatePickRef();
            logLab("verb:npc-talk", `claim ${held.remoteId.toString(16)}: no Dialogue Menu ${(waited / 1000).toFixed(1)} s after the grant, activatePickRef again (${queued ? "queued" : "refused"}, try ${held.retries})`);
            return;
        }
        if (waited > TalkService.menuWaitMs) {
            this.release(`no Dialogue Menu within ${TalkService.menuWaitMs} ms`);
        }
    }

    private onMenuClose(e: MenuCloseEvent) {
        if (e.name !== "Dialogue Menu" || !this.claimed) {
            return;
        }
        this.release("the Dialogue Menu's close");
    }

    private release(why: string) {
        const held = this.claimed;
        if (!held) {
            return;
        }
        this.send(held.remoteId, false);
        logLab("verb:npc-talk", `claim ${held.remoteId.toString(16)} released at ${why}`);
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
    private claimed?: { localId: number; remoteId: number; since: number; renewedAt: number; menuSeen: boolean; retries: number };
    private promotedAt = 0;
    private static readonly menuWaitMs = 10000;
    // half the server's lease (wire-rules cells.rs CLAIM_LEASE_MS, 120 s)
    private static readonly renewMs = 60000;
    private static readonly activateRetries = 2;
    private readonly enabled: boolean;
    private readonly key: number;
}
