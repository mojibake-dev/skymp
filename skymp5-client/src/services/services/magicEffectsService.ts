import { EffectShader, Game, MagicEffect, ObjectReference } from "skyrimPlatform";
import { logError, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { MagicEffectsMessage } from "../messages/magicEffectsMessage";
import { getObjectReference } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RemoteServer } from "./remoteServer";

// thuum docs/verbs/magic-effects.md: the effects the server runs on an actor
// show on this screen, and the server's are the player's only ones. The
// server sends an actor's running effects (MagicEffects) whenever the set
// changes and when this game first sees the actor; this game plays each
// effect's hit shader (its MGEF's, MagicEffect.GetHitShader) on that actor
// for the seconds left, and stops a shader whose effect left the set. No
// ActiveEffect is made here, so nothing changes a value but the server: its
// ChangeValues for the percentages, and for the player the sums its running
// buffs put on its actor values (modifiers), held here through
// TESModPlatform.ModPlayerActorValue as the change from what this game
// holds already. The player's engine applies no drink of its own
// (TESModPlatform.SetPotionEffectsBlocked): a fortify it ran beside the
// server's gave its maximum back twice at the end (b-potion
// 20261011-004053). Natives need the VM's context, so each message waits
// for an update of its own, in the order the messages came: the server
// sends a change's MagicEffects before its ChangeValues, which waits the
// same way (remoteServer.ts), so the maximum moves before the percentage.
export class MagicEffectsService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("magicEffectsMessage", (e) => this.onMagicEffects(e));
        this.controller.once("update", () => this.blockEngineDrinks());
    }

    private enabled(): boolean {
        return this.sp.settings["skymp5-client"]["magicEffects"] !== false;
    }

    private blockEngineDrinks() {
        if (!this.enabled()) {
            return;
        }
        try {
            this.sp.callNative("TESModPlatform", "SetPotionEffectsBlocked", undefined, true);
        } catch (err) {
            logError(this, "TESModPlatform.SetPotionEffectsBlocked failed", err);
        }
    }

    private onMagicEffects(e: ConnectionMessage<MagicEffectsMessage>) {
        if (!this.enabled()) {
            return;
        }
        const msg = e.message;
        this.controller.once("update", () => this.apply(msg));
    }

    private apply(msg: MagicEffectsMessage) {
        const remoteServer = this.controller.lookupListener(RemoteServer);
        const id = remoteServer.getIdManager().getId(msg.idx);
        const own = id === remoteServer.getMyActorIndex();
        if (own) {
            this.holdModifiers(msg);
        }
        const refr: ObjectReference | null = own ? Game.getPlayer() : getObjectReference(id);
        if (!refr) {
            logTrace(this, "no reference here for the effects of", msg.idx);
            return;
        }
        const key = refr.getFormID();
        const before = this.shown.get(key) ?? new Set<number>();
        const now = new Set<number>();
        for (const effect of msg.effects) {
            const mgef = MagicEffect.from(Game.getFormEx(effect.effect));
            const shader = mgef ? mgef.getHitShader() : null;
            if (!shader) {
                continue;
            }
            now.add(shader.getFormID());
            shader.play(refr, effect.remaining > 0 ? effect.remaining : MagicEffectsService.shownOnceSeconds);
        }
        before.forEach((shaderId) => {
            if (!now.has(shaderId)) {
                EffectShader.from(Game.getFormEx(shaderId))?.stop(refr);
            }
        });
        this.shown.set(key, now);
    }

    // The player's actor values take the server's buff sums: each moves by
    // the difference from what this game holds already (a value the message
    // leaves out is at 0), so a relaunched game, holding nothing, takes them
    // whole, and a buff's end gives back exactly what it gave
    private holdModifiers(msg: MagicEffectsMessage) {
        const want = new Map<number, number>();
        for (const m of msg.modifiers ?? []) {
            want.set(m.av, m.amount);
        }
        // the client targets ES5 (tsconfig.json): no spread of an iterator
        const avs = new Set<number>();
        this.held.forEach((_amount, av) => avs.add(av));
        want.forEach((_amount, av) => avs.add(av));
        avs.forEach((av) => {
            const target = want.get(av) ?? 0;
            const delta = target - (this.held.get(av) ?? 0);
            if (Math.abs(delta) < MagicEffectsService.epsilon) {
                return;
            }
            try {
                const ok = this.sp.callNative("TESModPlatform", "ModPlayerActorValue", undefined, av, delta);
                if (ok !== true) {
                    logError(this, "ModPlayerActorValue refused", av, delta);
                    return;
                }
            } catch (err) {
                logError(this, "TESModPlatform.ModPlayerActorValue failed", err);
                return;
            }
            if (target === 0) {
                this.held.delete(av);
            } else {
                this.held.set(av, target);
            }
        });
    }

    // how long an effect without duration (a heal at once) shows: a
    // presentation choice, about the engine's own flash
    private static readonly shownOnceSeconds = 1.5;
    // a sum's change too small to make
    private static readonly epsilon = 1e-4;

    private shown = new Map<number, Set<number>>();
    // what this game holds of the server's buff sums, by actor value
    private held = new Map<number, number>();
}
