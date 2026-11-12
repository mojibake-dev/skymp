import { Actor, Art, EffectShader, Game, MagicEffect, ObjectReference, Spell } from "skyrimPlatform";
import { logError, logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { MagicEffectsMessage } from "../messages/magicEffectsMessage";
import { getObjectReference, localIdToRemoteId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
import { RemoteServer } from "./remoteServer";

// thuum docs/verbs/magic-effects.md: the effects the server runs on an actor
// show on this screen, and the server's are the player's only ones. The
// server sends an actor's running effects (MagicEffects) whenever the set
// changes and when this game first sees the actor; this game plays each
// effect's hit shader (its MGEF's, MagicEffect.GetHitShader) on that actor
// for the seconds left, and stops a shader whose effect left the set; its
// hit effect art plays beside it the same way (showArts), unless an engine
// effect of the same record runs on the actor here and draws its own. No
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

// a hit effect art to show on an actor, and the seconds it shows
type ArtShown = { art: Art; seconds: number };

export class MagicEffectsService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("magicEffectsMessage", (e) => this.onMagicEffects(e));
        this.controller.emitter.on("connectionAccepted", () => this.onConnectionAccepted());
        this.controller.once("update", () => this.blockEngineDrinks());
        this.controller.on("update", () => this.commandUpkeep());
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
        this.noteThrall(own, key, msg);
        const before = this.shown.get(key) ?? new Set<number>();
        const now = new Set<number>();
        for (const effect of msg.effects) {
            const mgef = MagicEffect.from(Game.getFormEx(effect.effect));
            if (!mgef) {
                continue;
            }
            const shader = mgef.getHitShader();
            if (shader) {
                now.add(shader.getFormID());
                shader.play(refr, MagicEffectsService.secondsOf(effect.remaining));
            }
        }
        before.forEach((shaderId) => {
            if (!now.has(shaderId)) {
                EffectShader.from(Game.getFormEx(shaderId))?.stop(refr);
            }
        });
        this.shown.set(key, now);
        // the art pass, for another's actor a second update later: the server
        // sends a self spell's MagicEffects before it relays the cast
        // (ActionListener's OnSpellCast), and each message waits its own
        // update, so the engine's own effect of the record, which draws its
        // own art, may land on a player's figure only after this one (the
        // platform review of 3a31c05a, F3)
        if (own) {
            this.artPass(refr, msg);
        } else {
            this.controller.once("update", () => {
                const later = ObjectReference.from(Game.getFormEx(key));
                if (later) {
                    this.artPass(later, msg);
                }
            });
        }
        this.dispelEnded(own, refr, msg);
    }

    // the seconds an effect shows: the server's seconds left; an effect
    // without duration (0) for the flash; a state with no clock (a negative,
    // the server's -1: a thrall's reanimate, which ends with its actor's
    // death) for -1, no end, until the server's set drops it (thuum
    // docs/WIRE.md, MagicEffects' remaining; the platform review of 3a31c05a,
    // F5)
    private static secondsOf(remaining: number): number {
        if (remaining < 0) {
            return -1;
        }
        return remaining > 0 ? remaining : MagicEffectsService.shownOnceSeconds;
    }

    // each effect's hit art, unless the engine runs an effect of its record
    // on the actor here and draws the art itself: a second art is a second
    // model (a reanimated body's raise is its commander's engine's alone,
    // Hooks.cpp CharacterAddTarget, so a watcher draws the record's); an art
    // two effects share shows for the longer, no end the longest
    private artPass(refr: ObjectReference, msg: MagicEffectsMessage) {
        const actor = Actor.from(refr);
        const arts = new Map<number, ArtShown>();
        for (const effect of msg.effects) {
            const mgef = MagicEffect.from(Game.getFormEx(effect.effect));
            if (!mgef) {
                continue;
            }
            const art = mgef.getHitEffectArt();
            if (art && !(actor && actor.hasMagicEffect(mgef))) {
                const seconds = MagicEffectsService.secondsOf(effect.remaining);
                const artId = art.getFormID();
                const shownFor = arts.get(artId);
                const longer = !shownFor ? seconds : shownFor.seconds < 0 || seconds < 0 ? -1 : Math.max(shownFor.seconds, seconds);
                arts.set(artId, { art, seconds: longer });
            }
        }
        this.showArts(refr, arts);
    }

    // thuum docs/verbs/magic-effects.md, the hit art (the reanimation look's
    // D2 B): each art played on the actor for the seconds left through
    // TESModPlatform.ApplyArtObject, the engine's own
    // TESObjectREFR::ApplyArtObject, as the shader plays above (a second play
    // of an art replaces this game's first, so a message's replay keeps one
    // copy), and an art no longer shown stopped through StopArtObject, which
    // finishes every live copy of an art this game played on the actor, an
    // engine-drawn copy there with them (narrow: the pass plays no art where
    // the engine runs the effect)
    private showArts(refr: ObjectReference, arts: Map<number, ArtShown>) {
        const key = refr.getFormID();
        const before = this.shownArts.get(key) ?? new Set<number>();
        const now = new Set<number>();
        arts.forEach((shown, artId) => {
            try {
                if (this.sp.callNative("TESModPlatform", "ApplyArtObject", undefined, refr, shown.art, shown.seconds) === true) {
                    now.add(artId);
                } else {
                    logTrace(this, "no art", artId.toString(16), "played on", key.toString(16));
                }
            } catch (err) {
                logError(this, "TESModPlatform.ApplyArtObject failed", err);
            }
        });
        before.forEach((artId) => {
            if (now.has(artId)) {
                return;
            }
            const art = Art.from(Game.getFormEx(artId));
            if (!art) {
                return;
            }
            try {
                this.sp.callNative("TESModPlatform", "StopArtObject", undefined, refr, art);
            } catch (err) {
                logError(this, "TESModPlatform.StopArtObject failed", err);
            }
        });
        this.shownArts.set(key, now);
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

    // thuum docs/verbs/npc-fate.md C4 (ADR-060's return; Eli, 2026-11-12:
    // "thrall returns to caster obvs"): the caster each actor's Reanimate
    // state names, by local form id, from its MagicEffects (the state with
    // no clock, remaining negative, and its caster). A thrall stays its
    // caster's through the caster's departure, and the server hands it back
    // at the caster's return (UpdateHoster); this fresh game's engine holds
    // no effect on it, so the upkeep below commands it
    private noteThrall(own: boolean, key: number, msg: MagicEffectsMessage) {
        if (own) {
            return;
        }
        const state = msg.effects.find((e) => e.remaining < 0 && (e.caster ?? 0) !== 0);
        if (state && state.caster !== undefined) {
            this.thralls.set(key, state.caster);
        } else {
            this.thralls.delete(key);
        }
    }

    // Every commandEveryUpdates updates, for each thrall: while its record
    // names this game's player its caster, this game simulates it
    // (HostService, the server's word) and it lives, Skyrim Platform's
    // commandActor(refrId, true) starts the engine's command or refreshes it
    // (38799, the static read's per-update refresh at the read's 30 frames,
    // H-C4-2); once any of the three stops, commandActor(refrId, false) once
    // ends it. The export queues its work on the main thread and leaves the
    // engine's own command (the original cast's) alone, so a thrall this
    // game raised itself costs one task per period and nothing else
    private commandUpkeep() {
        this.commandTick += 1;
        if (this.commandTick < MagicEffectsService.commandEveryUpdates) {
            return;
        }
        this.commandTick = 0;
        if (!this.enabled() || (this.thralls.size === 0 && this.commanded.size === 0)) {
            return;
        }
        const me = localIdToRemoteId(0x14);
        const hostService = this.controller.lookupListener(HostService);
        const ids = new Set<number>();
        this.thralls.forEach((_caster, id) => ids.add(id));
        this.commanded.forEach((id) => ids.add(id));
        ids.forEach((id) => {
            const caster = this.thralls.get(id);
            const commanded = this.commanded.has(id);
            if (caster !== me && !commanded) {
                return;
            }
            const actor = Actor.from(Game.getFormEx(id));
            const wish = caster === me && !!actor && !actor.isDead() && hostService.isMineLocal(id);
            if (!wish && !commanded) {
                return;
            }
            try {
                const queued = this.sp.commandActor(id, wish);
                if (wish && !commanded) {
                    this.commanded.add(id);
                    logLab("verb:npc-fate", `commandActor ${id.toString(16)}: asked, its record's caster ${me.toString(16)} is this game's player and this game simulates it (queued ${queued})`);
                } else if (!wish) {
                    this.commanded.delete(id);
                    logLab("verb:npc-fate", `commandActor ${id.toString(16)}: ended (${!actor ? "no reference here" : actor.isDead() ? "dead" : caster !== me ? "its record names no caster of this game" : "simulated by another game"}; queued ${queued})`);
                }
            } catch (err) {
                logError(this, "commandActor failed", err);
            }
        });
    }

    // the upkeep's period, in updates (the read's 38799 every 30 frames,
    // lab/.cache/drafts/reanimate/read-return-152001.md section 3)
    private static readonly commandEveryUpdates = 30;

    // how long an effect without duration (a heal at once) shows: a
    // presentation choice, about the engine's own flash
    private static readonly shownOnceSeconds = 1.5;
    // a sum's change too small to make
    private static readonly epsilon = 1e-4;

    private shown = new Map<number, Set<number>>();
    // the hit arts this game plays on each actor, by art (showArts)
    private shownArts = new Map<number, Set<number>>();
    // the sources (spells, potions) of each actor's running effects, as the
    // server last sent them
    private sources = new Map<number, Set<number>>();
    // what this game holds of the server's buff sums, by actor value
    private held = new Map<number, number>();
    // the caster each actor's Reanimate state names, by local form id
    // (noteThrall), and the thralls the upkeep asked to command
    private thralls = new Map<number, number>();
    private commanded = new Set<number>();
    private commandTick = 0;
}
