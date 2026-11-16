import { logError, logLab, logTrace } from "../../logging";
import { MsgType } from "../../messages";
import { SummonMessage } from "../messages/summonMessage";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { SummonEndEvent, SummonStartEvent } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { WorldCleanerService } from "./worldCleanerService";

const TAG = "verb:summon";

// thuum docs/verbs/summon.md (tracker #84; lab/.cache/drafts/summons/design.md
// steps 1, 2 and 5, the client half): this game's own summons are the server's
// to place for every other seat. The caster's engine computes the summon (its
// effect, its place, its life; nothing else can), so the caster's game reports
// it and the server records it within bounds (R2). The platform's summon
// tracker, on SummonCreatureEffect's own Update and Finish (skyrim-platform
// src/platform_se/skyrim_platform/Hooks.cpp SummonCreatureEffectHooks, summon
// effects alone), raises summonStart once the effect's actor has appeared
// (EventHandler.cpp:176) and summonEnd at the effect's Finish
// (EventHandler.cpp:217); both land as update tasks, so the handlers read the
// engine's forms directly as the magicEffectApply handler does
// (hitService.ts:44-57).
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
// (EventHandler.cpp:62), so the live map here is bounded at the same 64: the
// oldest goes first when it fills.
//
// Two holds, both from x-summon-track 20261116-172548's trace on c1
// (lab/frida/summon-track.js; the Potent atronach's spell 0x100e76 placed one
// summon per effect, MGEF 0x1ceaa's and 0x4e945's, 0.38 s after its start):
// - the cleaner's guard. The cleaner skips a commanded actor
//   (worldCleanerService.ts:72-83), but the engine's Finish unlinks the summon
//   (the trace's unlink row) before its kill, fade and delete, and in that
//   window the cleaner deleted it too: c1 crashed 0.5 s after the Potent
//   atronach's Finish, EXCEPTION_ACCESS_VIOLATION at 41702+0x22 in a
//   MovementControllerNPC job on "Potent Flame Atronach" [0x00000000], the
//   double delete worldCleanerService.ts:86-95 records for a deer. So every
//   summon the platform names, every caster's, is protected from the cleaner
//   from its start until the engine has deleted it (the protection counter
//   FormView's bind uses, worldCleanerService.ts:14-21); the engine places
//   and deletes its summons itself.
// - the report's hold. MGEF 0x1ceaa's summon (the effect Elemental Potency's
//   condition refuses) appeared and its effect finished one frame later (the
//   trace's rows: commandedActor read at elapsed 0.381, Finish 31 ms after at
//   the same elapsed). A start is held reportAfterUpdates updates; an end
//   inside the hold drops it unreported, so no other seat draws a summon its
//   caster's engine kept for a frame.
type Live = { at: number; bound: boolean };
// A held start keeps plain numbers alone: an event's forms are Skyrim
// Platform's pooled native objects, valid in their own event's frame only
// (Sp3NativeValueCasts.cpp:40-52; x-summon 20261116-180439: the held event's
// effect read 3 updates later threw "Invalid _skyrimPlatform_indexInPool"
// and the Potent summon was never reported)
type Held = {
    summonId: number;
    effectId: number;
    spellId: number;
    durationS: number;
    elapsedS: number;
    updates: number;
    at: number;
};

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
        if (!e) {
            return;
        }
        // every caster's summon is the engine's to delete
        this.guard(e.summonId);
        if (e.casterId !== 0x14) {
            return;
        }
        // once: the platform raises a start once per summon (its live list),
        // and a second start for a live id folds server-side besides (the
        // verb doc's Idempotency)
        if (this.live.has(e.summonId) || this.held.has(e.summonId)) {
            return;
        }
        this.held.set(e.summonId, {
            summonId: e.summonId,
            effectId: e.effect.getFormID(),
            spellId: e.spell ? e.spell.getFormID() : 0,
            durationS: e.durationS,
            elapsedS: e.elapsedS,
            updates: 0,
            at: Date.now(),
        });
    }

    // a held start reported: the summon read again by its id, so one the
    // engine has dropped since is not reported
    private report(h: Held): void {
        const summon = this.sp.Actor.from(this.sp.Game.getFormEx(h.summonId));
        if (!summon || summon.isDeleted() || summon.isDead()) {
            logLab(TAG, `the player's summon ${h.summonId.toString(16)} gone before its report: not reported`);
            return;
        }
        const base = summon.getBaseObject();
        const message: SummonMessage = {
            t: MsgType.Summon,
            localId: h.summonId,
            baseId: base ? base.getFormID() : 0,
            effectId: h.effectId,
            spellId: h.spellId,
            pos: ObjectReferenceEx.getPos(summon),
            // the engine's own GetAngle words, degrees, sent raw as the
            // movement family's rotations travel (movementGet.ts:96)
            rot: [
                summon.getAngleX(),
                summon.getAngleY(),
                summon.getAngleZ(),
            ],
            worldOrCell: ObjectReferenceEx.getWorldOrCell(summon),
            // at least 0 (the wire's bound; an elapsed past its duration
            // clamps), less the hold's own time
            remainingS: Math.max(h.durationS - h.elapsedS - (Date.now() - h.at) / 1000, 0),
            ended: false,
        };
        if (this.live.size >= SummonService.liveMax) {
            const oldest = this.live.keys().next();
            if (!oldest.done) {
                this.live.delete(oldest.value);
            }
        }
        this.live.set(h.summonId, { at: Date.now(), bound: false });
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logLab(TAG, `the player's summon ${h.summonId.toString(16)} (base ${message.baseId.toString(16)}, effect ${message.effectId.toString(16)}, spell ${message.spellId.toString(16)}) reported: at (${message.pos.map((v) => v.toFixed(0)).join(", ")}) in ${message.worldOrCell.toString(16)}, ${message.remainingS.toFixed(1)} s left`);
    }

    private onSummonEnd(e: SummonEndEvent): void {
        if (!e || e.casterId !== 0x14) {
            return;
        }
        // an end inside the start's hold: never reported, so nothing to end
        const held = this.held.get(e.summonId);
        if (held) {
            this.held.delete(e.summonId);
            logLab(TAG, `the player's summon ${e.summonId.toString(16)} ended ${held.updates} update(s) after its start: neither reported`);
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
        this.reportHeld();
        this.releaseGuards();
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

    // the held starts, reported after their hold
    private reportHeld(): void {
        if (this.held.size === 0) {
            return;
        }
        const due: number[] = [];
        this.held.forEach((h, id) => {
            h.updates += 1;
            if (h.updates >= SummonService.reportAfterUpdates) {
                due.push(id);
            }
        });
        for (const id of due) {
            const h = this.held.get(id);
            this.held.delete(id);
            if (!h) {
                continue;
            }
            // a throw never loses a summon silently
            try {
                this.report(h);
            } catch (err) {
                logError(this, `the player's summon ${id.toString(16)}: its report failed`, err);
                logLab(TAG, `the player's summon ${id.toString(16)}: its report failed (${err}): not reported`);
            }
        }
    }

    // the cleaner's guard on a summon, once
    private guard(summonId: number): void {
        if (this.guarded.has(summonId)) {
            return;
        }
        if (this.guarded.size >= SummonService.liveMax) {
            const oldest = this.guarded.keys().next();
            if (!oldest.done) {
                this.unguard(oldest.value);
            }
        }
        this.controller.lookupListener(WorldCleanerService).modWcProtection(summonId, 1);
        this.guarded.set(summonId, Date.now());
    }

    private unguard(summonId: number): void {
        if (this.guarded.delete(summonId)) {
            this.controller.lookupListener(WorldCleanerService).modWcProtection(summonId, -1);
        }
    }

    // a guard ends once the engine has deleted its summon (or the game holds
    // no such form, after a load)
    private releaseGuards(): void {
        if (this.guarded.size === 0) {
            return;
        }
        const gone: number[] = [];
        this.guarded.forEach((_at, id) => {
            const ref = this.sp.ObjectReference.from(this.sp.Game.getFormEx(id));
            if (!ref || ref.isDeleted()) {
                gone.push(id);
            }
        });
        for (const id of gone) {
            this.unguard(id);
        }
    }

    // the starts reported and not yet bound
    private readonly live = new Map<number, Live>();
    // the starts held before their report
    private readonly held = new Map<number, Held>();
    // the summons guarded from the cleaner, by id, since when
    private readonly guarded = new Map<number, number>();
    // the report's hold, in updates: the refused effect's summon ended one
    // frame after it appeared (x-summon-track 20261116-172548's trace)
    private static readonly reportAfterUpdates = 3;
    // the refusal fallback's window (design step 5: "no bind within 5 s")
    private static readonly bindWaitMs = 5000;
    // the platform's own live ceiling (EventHandler.cpp:62)
    private static readonly liveMax = 64;
}
