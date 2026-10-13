import { Actor, Form, Game, Shout, Spell } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logTrace } from "../../logging";
import { SpellCastMsgData } from "../messages/spellCastMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// thuum docs/verbs/shouts.md: a shout fires no spellCast (x-shout-probe
// 20261011-060059), so the server heard none. The player's game plays
// shoutStart, then MT_BreathExhaleShort at the release, whichever variation
// it was (x-shout-probe2), and sets the voice recovery: the variation's
// recovery time times ShoutRecoveryMult, which names the variation
// (x-shout-probe3 20261011-221358). The hooks only note the release (no
// script function runs inside a hook); the next frames read the recovery
// once the game has set it, find the variation among the equipped shout's
// three and report a cast from the voice (castingSource 2) with that
// variation's spell, which the server judges against its record.
//
// The player's spell hits wait while a report is due: the shout's spell
// hits on the frames after the release, and a hit the server sees before
// its shout has no cast to claim (spell-cast's claim). Both go reliable on
// one ordered channel, so a hit sent after the report reaches the server
// after it. HitService asks the gate before it sends.
export const shoutGate = {
    pending: false,
    held: [] as Array<() => void>,
};

const releaseHeld = () => {
    shoutGate.pending = false;
    const held = shoutGate.held.splice(0);
    held.forEach((send) => send());
};

export class ShoutService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("update", () => this.onUpdate());
        this.sp.hooks.sendAnimationEvent.add({
            enter: () => { },
            leave: () => { this.shoutStartedAt = Date.now(); },
        }, this.playerId, this.playerId, "shoutStart");
        this.sp.hooks.sendAnimationEvent.add({
            enter: () => { },
            leave: () => {
                if (Date.now() - this.shoutStartedAt <= ShoutService.chargeMs) {
                    this.releasedAt = Date.now();
                    shoutGate.pending = true;
                }
                this.shoutStartedAt = 0;
            },
        }, this.playerId, this.playerId, "MT_BreathExhale*");
    }

    private onUpdate() {
        if (this.releasedAt === 0) {
            return;
        }
        try {
            this.readRelease();
        } catch (err) {
            // a throw here used to drop the report silently, every frame
            logError(this, "E_SHOUT_REPORT", err);
            this.releasedAt = 0;
            releaseHeld();
        }
    }

    private readRelease() {
        const player = Game.getPlayer();
        const shout = player ? player.getEquippedShout() : null;
        if (!player || !shout) {
            this.releasedAt = 0;
            releaseHeld();
            return;
        }
        const recovery = this.sp.callNative("Actor", "GetVoiceRecoveryTime", player) as number;
        if (!(recovery > 0)) {
            if (Date.now() - this.releasedAt > ShoutService.readMs) {
                logError(this, "E_SHOUT_UNREAD: no voice recovery", ShoutService.readMs, "ms after the release of", shout.getFormID().toString(16));
                this.releasedAt = 0;
                releaseHeld();
            }
            return;
        }
        this.releasedAt = 0;
        const variation = this.variationOf(shout, player, recovery);
        // the typed binding first: with callNative's game object the report
        // traced and nothing was sent (b-shout 20261012-020313,
        // x-shout-console 20261012-020808; HYPOTHESIS: that object has no
        // Form methods, so getFormID threw); callNative only as a fallback
        let spell: Spell | null = variation === null ? null : shout.getNthSpell(variation);
        if (!spell && variation !== null) {
            spell = Spell.from(this.sp.callNative("Shout", "GetNthSpell", shout, variation) as Form | null);
        }
        if (variation === null || !spell) {
            logError(this, "E_SHOUT_UNREAD: a recovery of", recovery, "s names no variation of", shout.getFormID().toString(16));
            releaseHeld();
            return;
        }
        logTrace(this, "shout", shout.getFormID().toString(16), "variation", variation, "recovery", recovery, "hits held", shoutGate.held.length);
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.SpellCast, data: this.castOf(spell.getFormID()) },
            reliability: "reliable",
        });
        releaseHeld();
    }

    // The variation whose recovery the game set: the one whose recovery
    // times ShoutRecoveryMult is at or just above what is left of it (it
    // counts down from the release), or null
    private variationOf(shout: Shout, player: Actor, recovery: number): number | null {
        const mult = player.getActorValue("ShoutRecoveryMult");
        let best: number | null = null;
        let bestSet = Infinity;
        for (let n = 0; n < 3; ++n) {
            const set = (this.sp.callNative("Shout", "GetNthRecoveryTime", shout, n) as number) * mult;
            const counted = set - recovery;
            if (counted >= -ShoutService.slackS && counted <= ShoutService.countedS && set < bestSet) {
                best = n;
                bestSet = set;
            }
        }
        return best;
    }

    // the cast from the voice the server judges and relays: every game that
    // sees the shouter launches it from its figure (thuum
    // docs/verbs/shouts.md), so it carries the shouter's facing as a spell's
    // aim, in radians (CommonLibSSE-NG Actor::GetAimAngle falls back to the
    // actor's angle X, GetAimHeading to its heading); Papyrus reads degrees,
    // and the pitch is held within the quarter turn the validator allows.
    // Dual casting and the graph's variables mean nothing for a shout
    private castOf(spell: number): SpellCastMsgData {
        const player = this.sp.Game.getPlayer();
        let pitch = player ? player.getAngleX() : 0;
        if (pitch > 180) {
            pitch -= 360;
        }
        pitch = Math.max(-90, Math.min(90, pitch));
        const heading = player ? player.getAngleZ() : 0;
        const toRadians = Math.PI / 180;
        return {
            caster: this.playerId,
            target: 0,
            spell,
            isDualCasting: false,
            interruptCast: false,
            castingSource: 2,
            aimAngle: pitch * toRadians,
            aimHeading: heading * toRadians,
            actorAnimationVariables: { booleans: [], floats: [], integers: [] },
        };
    }

    private readonly playerId = 0x14;
    private shoutStartedAt = 0;
    private releasedAt = 0;
    // three words charge in about 0.9 s (x-shout-probe2)
    private static readonly chargeMs = 3000;
    // how long the recovery may take to show after the release
    private static readonly readMs = 1500;
    // how much of the recovery may have counted down when read, and how far a
    // read may sit above the set value
    private static readonly countedS = 1.5;
    private static readonly slackS = 0.05;
}
