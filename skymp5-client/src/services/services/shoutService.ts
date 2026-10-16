import { Actor, Form, Game, Shout, Spell } from "skyrimPlatform";
// @ts-expect-error (TODO: Remove in 2.10.0)
import { getAnimationVariablesFromActor } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logTrace } from "../../logging";
import { SpellCastMsgData } from "../messages/spellCastMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// thuum docs/verbs/shouts.md: a shout fires no spellCast (x-shout-probe
// 20261011-060059), so the server heard none. The player's game plays
// shoutStart, then MT_BreathExhaleShort at the release, whichever variation
// it was (x-shout-probe2), or CombatReady_BreathExhaleShort with magic
// readied or a weapon drawn (x-shout-drawn2 20261013-221426: the release
// went unseen in those states, playtest fifteen's Unrelenting Force with
// Flames readied among them), and sets the voice recovery: the variation's
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
//
// When the recovery names no variation (god mode sets none: TGM's "no shout
// cooldown"; or the read came late), the shout's own hit does: its spell is
// one of the equipped shout's three, and the report goes out with it before
// the hit. A hit of a shout's spell with no report in the last chargeMs
// (the release unseen) reports it the same way. Playtest fifteen: no shout
// of test 1's reached the server for a quarter hour, its Unrelenting Force
// hits refused for want of a cast (thuum docs/verbs/shouts.md)
type HeldHit = { spell: number; send: () => void };
export const shoutGate = {
    pending: false,
    held: [] as Array<HeldHit>,
    // the last report: its spell and when, so a shout's own hits after it
    // report nothing more
    reportedSpell: 0,
    reportedAt: 0,
    // set by ShoutService: reports the equipped shout's cast from a hit's
    // spell when it is one of the shout's variations; true when it did
    reportFromHit: (_spell: number, _why: string): boolean => false,
};

const releaseHeld = () => {
    shoutGate.pending = false;
    const held = shoutGate.held.splice(0);
    held.forEach((h) => h.send());
};

export class ShoutService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("update", () => this.onUpdate());
        shoutGate.reportFromHit = (spell, why) => this.reportFromHit(spell, why);
        // the engine's own fire of the voice (SKSE's action event, ADR-034):
        // the shout's spell at the moment the engine casts it, whether or not
        // it hits anything and whatever the voice recovery reads. Playtest
        // nineteen's third session: 13 of 18 god-mode breaths never reached
        // the server, every second breath within three seconds among them,
        // because with a zero recovery the release was reported only from a
        // hit (reportFromHeld) and a breath that hits nobody has none
        this.controller.on("skseAction", (e) => this.onSkseAction(e));
        this.sp.hooks.sendAnimationEvent.add({
            enter: () => { },
            leave: () => { this.shoutStartedAt = Date.now(); },
        }, this.playerId, this.playerId, "shoutStart");
        for (const release of ["MT_BreathExhale*", "CombatReady_BreathExhale*"]) {
            this.sp.hooks.sendAnimationEvent.add({
                enter: () => { },
                leave: () => {
                    // every exhale is a release (thuum docs/verbs/cast-action.md,
                    // the god-mode breath): one that followed a shoutStart within
                    // chargeMs used to arm the gate, but with no voice recovery a
                    // second breath 2.5 s after the first played no shoutStart of
                    // its own, its exhale armed nothing, and its hits, within 3 s
                    // of the first report, were read as the first breath's
                    // (x-tgm-breath4 20261015-035032: three shouts taken of four)
                    this.releasedAt = Date.now();
                    shoutGate.pending = true;
                    this.shoutStartedAt = 0;
                },
            }, this.playerId, this.playerId, release);
        }
    }

    private onSkseAction(e: { actor: Actor; type: number; slot: number; sourceForm: number }) {
        if (e.type !== ShoutService.kVoiceFire || !e.sourceForm) {
            return;
        }
        // no game call in the event's context: the form is read on the update
        this.controller.once("update", () => {
            const form = Game.getFormEx(e.sourceForm);
            const spell = form ? Spell.from(form) : null;
            logTrace(this, "voice fire: source", e.sourceForm.toString(16), spell ? "a spell" : form ? "not a spell" : "no form");
            if (!spell) {
                // a shout's form names no variation; the recovery read stays
                return;
            }
            const spellId = spell.getFormID();
            if (spellId === shoutGate.reportedSpell && Date.now() - shoutGate.reportedAt <= ShoutService.fireReportWindowMs) {
                return;
            }
            this.report(spellId);
            this.releasedAt = 0;
            releaseHeld();
        });
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
                this.releasedAt = 0;
                if (!this.reportFromHeld(shout, "no voice recovery")) {
                    logError(this, "E_SHOUT_UNREAD: no voice recovery", ShoutService.readMs, "ms after the release of", shout.getFormID().toString(16), "and no hit of it held");
                }
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
            if (!this.reportFromHeld(shout, `a recovery of ${recovery} s names no variation`)) {
                logError(this, "E_SHOUT_UNREAD: a recovery of", recovery, "s names no variation of", shout.getFormID().toString(16), "and no hit of it held");
            }
            releaseHeld();
            return;
        }
        logTrace(this, "shout", shout.getFormID().toString(16), "variation", variation, "recovery", recovery, "hits held", shoutGate.held.length);
        this.report(spell.getFormID());
        releaseHeld();
    }

    private report(spell: number) {
        shoutGate.reportedSpell = spell;
        shoutGate.reportedAt = Date.now();
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.SpellCast, data: this.castOf(spell) },
            reliability: "reliable",
        });
    }

    // The equipped shout's variation a spell is, or null
    private variationSpellOf(shout: Shout, spell: number): number | null {
        for (let n = 0; n < 3; ++n) {
            const s = shout.getNthSpell(n);
            if (s && s.getFormID() === spell) {
                return n;
            }
        }
        return null;
    }

    // A held hit names the shout when the recovery cannot: the first held
    // hit whose spell is one of the shout's variations reports it
    private reportFromHeld(shout: Shout, why: string): boolean {
        for (const h of shoutGate.held) {
            const n = this.variationSpellOf(shout, h.spell);
            if (n !== null) {
                logError(this, "E_SHOUT_FROM_HIT:", why, "; shout", shout.getFormID().toString(16), "variation", n, "named by its hit");
                this.report(h.spell);
                return true;
            }
        }
        return false;
    }

    // HitService's check for a hit sent with no report pending: a hit of the
    // equipped shout's spell with no report of it within chargeMs means the
    // release went unseen, and the report goes out first
    private reportFromHit(spell: number, why: string): boolean {
        // one breath's hits span about a second (r-shout-firebreath-a: the
        // cone's hits 220 ms after the exhale); a hit of the same spell past
        // that is another shout's, the release unseen
        if (spell === shoutGate.reportedSpell && Date.now() - shoutGate.reportedAt <= ShoutService.hitWindowMs) {
            return false;
        }
        // a stream's hits come ten a second: the equipped shout's spells are
        // read at most once a second
        const now = Date.now();
        if (now - this.voiceSpellsReadAt > 1000) {
            const player = Game.getPlayer();
            const shout = player ? player.getEquippedShout() : null;
            this.voiceSpells = [];
            for (let n = 0; shout && n < 3; ++n) {
                const s = shout.getNthSpell(n);
                this.voiceSpells.push(s ? s.getFormID() : 0);
            }
            this.voiceShout = shout ? shout.getFormID() : 0;
            this.voiceSpellsReadAt = now;
        }
        const n = this.voiceSpells.indexOf(spell);
        if (n < 0) {
            return false;
        }
        logError(this, "E_SHOUT_UNREPORTED:", why, "; shout", this.voiceShout.toString(16), "variation", n, "reported from its hit");
        this.report(spell);
        return true;
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
    // It carries the shouter's graph variables too, as a spell's cast does:
    // the figure's game applies them before it casts, and refuses a cast
    // that has none (Skyrim Platform MagicApi.cpp, castSpellImmediate).
    // Dual casting means nothing for a shout
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
            actorAnimationVariables: this.animationVariables(),
        };
    }

    private animationVariables() {
        const vars = getAnimationVariablesFromActor(this.playerId);
        return {
            booleans: Array.from(new Uint8Array(vars.booleans)),
            floats: Array.from(new Uint8Array(vars.floats)),
            integers: Array.from(new Uint8Array(vars.integers)),
        };
    }

    private readonly playerId = 0x14;
    private voiceSpells: number[] = [];
    private voiceShout = 0;
    private voiceSpellsReadAt = 0;
    private shoutStartedAt = 0;
    private releasedAt = 0;
    // three words charge in about 0.9 s (x-shout-probe2)
    private static readonly chargeMs = 3000;
    // SKSE::ActionEvent::Type::kVoiceFire (CommonLibSSE-NG include/SKSE/Events.h)
    private static readonly kVoiceFire = 4;
    // a release reported from the recovery read within this is the same shout
    private static readonly fireReportWindowMs = 500;
    // how long one shout's own hits may follow its report
    private static readonly hitWindowMs = 1200;
    // how long the recovery may take to show after the release
    private static readonly readMs = 1500;
    // how much of the recovery may have counted down when read, and how far a
    // read may sit above the set value
    private static readonly countedS = 1.5;
    private static readonly slackS = 0.05;
}
