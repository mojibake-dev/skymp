import { Actor, Form, MagicEffect, ObjectReference } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { DeathService } from "./deathService";
import { logTrace } from "../../logging";

// thuum docs/verbs/headshot.md, ADR-038 (accepted 2026-10-18): the server
// kills on a claimed bow hit 27 to 50 ms after the hit arrives, plus the
// round trip, and plays the death from its record; Headshot Kills' own kill
// in the shooter's game comes in the hit's frame, so the shooter felt "a
// little lag" (playtest twenty-three). When the mod's kill spell applies to
// a figure this player just hit, this game plays the figure's death at once
// (DeathService.predictFigureDeath) and the server's word confirms it, or
// its absence within the bound stands the figure back up. The kill spells
// are the client's settings' "headshot-spells" ("801:Headshot Kills.esp",
// the server's own headshot block's spelling), resolved against this game's
// load order at the first use.
export class HeadshotPredictionService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        controller.on("magicEffectApply", (e) => this.onEffectApplied(e));
    }

    // the hit report, the server's claim and the death's return, with room:
    // measured 168 to 316 ms from the release to the kill (results/
    // playtest-23), the bound is four times the slowest
    private static readonly boundMs = 1200;

    private spells: number[] | null = null;

    private resolveSpells(): number[] {
        if (this.spells) {
            return this.spells;
        }
        const out: number[] = [];
        const raw = this.sp.settings["skymp5-client"]["headshot-spells"];
        if (Array.isArray(raw)) {
            for (const entry of raw) {
                if (typeof entry !== "string") {
                    continue;
                }
                const sep = entry.indexOf(":");
                if (sep < 0) {
                    continue;
                }
                const id = parseInt(entry.slice(0, sep), 16);
                const file = entry.slice(sep + 1);
                const form = Number.isFinite(id) ? this.sp.Game.getFormFromFile(id, file) : null;
                if (form) {
                    out.push(form.getFormID());
                } else {
                    logTrace(`headshotPrediction: ${entry} is in no loaded plugin`);
                }
            }
        }
        this.spells = out;
        return out;
    }

    private onEffectApplied(e: { effect: MagicEffect; spell?: Form | null; caster: ObjectReference; target: ObjectReference }) {
        if (!e.caster || !e.target || !e.spell || e.caster.getFormID() !== 0x14) {
            return;
        }
        const targetId = e.target.getFormID();
        if (targetId < 0xff000000) {
            return; // a figure only: never the player itself or a world actor
        }
        if (!this.resolveSpells().includes(e.spell.getFormID())) {
            return;
        }
        const actor = Actor.from(e.target);
        if (!actor) {
            return;
        }
        const death = this.controller.lookupListener(DeathService);
        if (!death.predictFigureDeath(actor)) {
            return;
        }
        logTrace(`headshotPrediction: figure ${targetId.toString(16)} played dead ahead of the server`);
        this.sp.Utility.wait(HeadshotPredictionService.boundMs / 1000).then(() => {
            if (!death.hasPredictedDeath(targetId)) {
                return; // confirmed
            }
            const again = Actor.from(this.sp.Game.getFormEx(targetId));
            if (!again) {
                return;
            }
            logTrace(`headshotPrediction: no word on ${targetId.toString(16)} within ${HeadshotPredictionService.boundMs} ms; standing it back up`);
            death.revertPredictedDeath(again);
        });
    }
}
