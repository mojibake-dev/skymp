import { logLab, logTrace } from "../../logging";
import { MsgType } from "../../messages";
import { SummonMessage } from "../messages/summonMessage";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { SummonEndEvent, SummonStartEvent } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";

const TAG = "verb:summon";

// thuum docs/verbs/summon.md (tracker #84; lab/.cache/drafts/summons/design.md
// steps 1, 2 and 5, the client half): this game's own summons are the server's
// to place for every other seat. The caster's engine computes the summon (its
// effect, its place, its life; nothing else can), so the caster's game reports
// it and the server records it within bounds (R2). The platform's summon
// tracker raises summonStart once the summon effect's actor has appeared
// (archetype kSummonCreature, filtered in native; skyrim-platform
// src/platform_se/skyrim_platform/EventHandler.cpp:308) and summonEnd at the
// effect's remove, matched by the unique id kept since (EventHandler.cpp:193);
// both land as update tasks, so the handlers read the engine's forms directly
// as the magicEffectApply handler does (hitService.ts:44-57, raised the same
// way at EventHandler.cpp:1210).
//
// Only the local player's summons cross the wire: the event's casterId against
// 0x14, the same own-player test as actionSyncService.ts:42 (NPC summons are
// out of scope unless they share this path for free). A start goes once as
// Summon 83, ended false, with the ids the event carried and the place read
// from the summon actor itself (ObjectReferenceEx.getPos and getWorldOrCell,
// the senders' WorldOrCell convention, objectReferenceEx.ts:10-22 and
// hostService.ts:878-887); the wire's rot is radians (wire-schema skymp.rs's
// Summon doc: "which way the summon faces, radians"), this engine's angle
// reads being degrees (skyrimPlatform.ts:2164-2166), so they convert here
// (the same arithmetic formView.ts:171 runs the other way). remainingS is the
// event's durationS minus elapsedS, the active effect's own
// (ActiveEffect.h:108-109; the event carries both, skyrimPlatform.ts:255-256).
// An end goes once with the local id alone (the wire reads nothing else at an
// end). Both go out by the OwnHeadParts reporter's own path
// (ownLookService.ts:164, commit 84eb42db): sendMessage, reliable.
//
// The server answers a start with the summon's create word, its props carrying
// ownLocal (the bind), which FormView takes and reports back through
// noteBound. A start with no bind within bindWaitMs is a refusal (design step
// 5), and this game dispels its own summon through the engine's own look: kill
// with no killer (skyrimPlatform.ts:2412), the alpha faded out
// (setAlpha(0.0, true), skyrimPlatform.ts:2443), then delete after the fade
// GMST fSummonedCreatureFadeOutSeconds' seconds, read from the engine here and
// never from memory (Game.getGameSettingFloat, skyrimPlatform.ts:3007); the
// design marks the engine's own Finish look a static read, HYPOTHESIS. The
// delete is guarded as the cleaner guards its own (worldCleanerService.ts:
// 97-103: a second delete is the freed-form crash of b-cell-host
// 20261021-192835).
//
// The engine's own ceiling is two live summons a caster (the Twin Souls perk,
// the design's step 4), and the platform's own live list holds 64
// (EventHandler.cpp:88), so the live map here is bounded at the same 64: the
// oldest goes first when it fills.
type Live = { at: number; bound: boolean };

export class SummonService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        // an older platform without the tracker's events refuses the
        // subscription (the sceneStart guard's shape, sceneClaimService.ts:
        // 71-75); no summon is reported and nothing dispels, the pre-#84
        // behavior
        try {
            this.controller.on("summonStart", (e) => this.onSummonStart(e));
        } catch (e) {
            logTrace(this, `summonStart unavailable on this platform (${e}); no summon is reported (#84)`);
        }
        try {
            this.controller.on("summonEnd", (e) => this.onSummonEnd(e));
        } catch (e) {
            logTrace(this, `summonEnd unavailable on this platform (${e}); no summon's end is reported (#84)`);
        }
        this.controller.on("update", () => this.onUpdate());
    }

    // FormView's bind (#84's step 5): a create word whose props carried
    // ownLocal named this engine's summon as the server's actor, so the start
    // report that named the same id is answered and the refusal fallback is
    // disarmed for it. A bind for no live report (a rebind after a cell
    // change, a word the report never named) changes nothing
    noteBound(localId: number): void {
        const live = this.live.get(localId);
        if (live && !live.bound) {
            live.bound = true;
            logLab(TAG, `summon ${localId.toString(16)} bound: the server's create word named it as its ownLocal`);
        }
    }

    private onSummonStart(e: SummonStartEvent): void {
        if (!e || e.casterId !== 0x14) {
            return;
        }
        // once: the platform raises a start once per summon (its live list),
        // and a second start for a live id folds server-side besides (the
        // verb doc's Idempotency)
        if (this.live.has(e.summonId)) {
            return;
        }
        const base = e.summon.getBaseObject();
        const message: SummonMessage = {
            t: MsgType.Summon,
            localId: e.summonId,
            baseId: base ? base.getFormID() : 0,
            effectId: e.effect.getFormID(),
            spellId: e.spell ? e.spell.getFormID() : 0,
            pos: ObjectReferenceEx.getPos(e.summon),
            // the engine's own GetAngle words, degrees, sent raw as the
            // movement family's rotations travel (movementGet.ts:96)
            rot: [
                e.summon.getAngleX(),
                e.summon.getAngleY(),
                e.summon.getAngleZ(),
            ],
            worldOrCell: ObjectReferenceEx.getWorldOrCell(e.summon),
            // at least 0 (the wire's bound; an elapsed past its duration clamps)
            remainingS: Math.max(e.durationS - e.elapsedS, 0),
            ended: false,
        };
        if (this.live.size >= SummonService.liveMax) {
            const oldest = this.live.keys().next();
            if (!oldest.done) {
                this.live.delete(oldest.value);
            }
        }
        this.live.set(e.summonId, { at: Date.now(), bound: false });
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logLab(TAG, `the player's summon ${e.summonId.toString(16)} (base ${message.baseId.toString(16)}, effect ${message.effectId.toString(16)}, spell ${message.spellId.toString(16)}) reported: at (${message.pos.map((v) => v.toFixed(0)).join(", ")}) in ${message.worldOrCell.toString(16)}, ${message.remainingS.toFixed(1)} s left`);
    }

    private onSummonEnd(e: SummonEndEvent): void {
        if (!e || e.casterId !== 0x14) {
            return;
        }
        // the report carries the local id alone; the wire reads nothing else
        // at an end (zeros elsewhere, wire-schema skymp.rs's Summon doc), and
        // the server folds an end for no live summon
        this.live.delete(e.summonId);
        const message: SummonMessage = {
            t: MsgType.Summon,
            localId: e.summonId,
            baseId: 0,
            effectId: 0,
            spellId: 0,
            pos: [0, 0, 0],
            rot: [0, 0, 0],
            worldOrCell: 0,
            remainingS: 0,
            ended: true,
        };
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logLab(TAG, `the player's summon ${e.summonId.toString(16)} ended: reported`);
    }

    // the refusal fallback (design step 5): a start the server has not
    // answered with a bind within bindWaitMs. The Map walks by forEach
    // (the es5 target takes no Map iteration; the pcInv holds' own walk,
    // remoteServer.ts:129) and the refusals go out after it
    private onUpdate(): void {
        if (this.live.size === 0) {
            return;
        }
        const now = Date.now();
        const refused: number[] = [];
        this.live.forEach((live, localId) => {
            if (live.bound || now - live.at <= SummonService.bindWaitMs) {
                return;
            }
            refused.push(localId);
        });
        for (const localId of refused) {
            this.live.delete(localId);
            this.dispel(localId);
        }
    }

    // the engine's own Finish look, as the design's step 5 spells it: kill
    // (no killer) if it lives, the alpha faded out, the reference deleted
    // after the fade GMST's seconds
    private dispel(localId: number): void {
        const actor = this.sp.Actor.from(this.sp.Game.getFormEx(localId));
        if (!actor || actor.isDeleted()) {
            logLab(TAG, `summon ${localId.toString(16)} refused (no bind in ${(SummonService.bindWaitMs / 1000).toFixed(0)} s): already gone, nothing to dispel`);
            return;
        }
        if (!actor.isDead()) {
            actor.kill(null);
        }
        actor.setAlpha(0.0, true);
        const fade = this.sp.Game.getGameSettingFloat("fSummonedCreatureFadeOutSeconds");
        const seconds = Number.isFinite(fade) && fade > 0 ? fade : 0;
        this.sp.Utility.wait(seconds).then(() => {
            const again = this.sp.ObjectReference.from(this.sp.Game.getFormEx(localId));
            // a form gone or already deleted (the engine's own Finish, or a
            // load) is left alone: a second delete is the freed-form crash
            // (worldCleanerService.ts:97-103)
            if (!again || again.isDeleted()) {
                return;
            }
            again.delete();
        });
        logLab(TAG, `summon ${localId.toString(16)} refused (no bind in ${(SummonService.bindWaitMs / 1000).toFixed(0)} s): dispelled locally (killed, faded, deleted in ${seconds.toFixed(1)} s)`);
    }

    // the starts reported and not yet bound
    private readonly live = new Map<number, Live>();
    // the refusal fallback's window (design step 5: "no bind within 5 s")
    private static readonly bindWaitMs = 5000;
    // the platform's own live ceiling (EventHandler.cpp:88)
    private static readonly liveMax = 64;
}
