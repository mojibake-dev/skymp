import { Actor, EffectShader, Game, MagicEffect, ObjectReference, Spell } from "skyrimPlatform";
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
        this.controller.emitter.on("connectionAccepted", () => this.onConnectionAccepted());
        this.controller.once("update", () => this.blockEngineDrinks());
    }

    // A login, a reconnect's included, sets the player's base values from
    // its CreateActor (remoteServer.ts, the isMe props), which drops what
    // this game held of the server's buffs (x-restart-effects-probe
    // 20261011-021833: the maximum 200 before a server restart, 100 after
    // the reconnect); the login's MagicEffects then puts them back whole
    private onConnectionAccepted() {
        this.held.clear();
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
        this.dispelEnded(own, refr, msg);
    }

    // thuum docs/verbs/self-cast.md: a figure's Self spell runs in this game
    // too (the relayed cast casts it on the figure, so its own look shows),
    // on this game's clock; when the server's set no longer holds a spell
    // of the figure's, the copy here is dispelled, so it never outlives the
    // caster's (playtest fifteen: Invisibility's look stayed on the figure
    // after its player waited). This game's own player is the engine's.
    private dispelEnded(own: boolean, refr: ObjectReference, msg: MagicEffectsMessage) {
        const key = refr.getFormID();
        const sourcesNow = new Set<number>();
        for (const effect of msg.effects) {
            sourcesNow.add(effect.source);
        }
        const sourcesBefore = this.sources.get(key);
        this.sources.set(key, sourcesNow);
        if (own || !sourcesBefore) {
            return;
        }
        const actor = Actor.from(refr);
        if (!actor) {
            return;
        }
        sourcesBefore.forEach((source) => {
            if (sourcesNow.has(source)) {
                return;
            }
            const spell = Spell.from(Game.getFormEx(source));
            if (spell) {
                actor.dispelSpell(spell);
                logTrace(this, "dispelled", source.toString(16), "from the figure", key.toString(16));
            }
        });
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
    // the sources (spells, potions) of each actor's running effects, as the
    // server last sent them
    private sources = new Map<number, Set<number>>();
    // what this game holds of the server's buff sums, by actor value
    private held = new Map<number, number>();
}
