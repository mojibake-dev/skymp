import { EffectShader, EquipEvent, Game, MagicEffect, ObjectReference, Potion } from "skyrimPlatform";
import { logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { MagicEffectsMessage } from "../messages/magicEffectsMessage";
import { getObjectReference } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RemoteServer } from "./remoteServer";

// thuum docs/verbs/magic-effects.md: the effects the server runs on an actor
// show on this screen. The server sends an actor's running effects
// (MagicEffects) whenever the set changes and when this game first sees the
// actor; this game plays each effect's hit shader (its MGEF's,
// MagicEffect.GetHitShader) on that actor for the seconds left, and stops a
// shader whose effect left the set. Visuals only: no ActiveEffect is made
// here, so nothing changes a value (the server's ChangeValues do). The
// player's own drinks are skipped: its engine drinks them and shows them.
// Natives need the VM's context, so a message waits for the next update.
export class MagicEffectsService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("magicEffectsMessage", (e) => this.onMagicEffects(e));
        this.controller.on("equip", (e) => this.onEquip(e));
        this.controller.on("update", () => this.onUpdate());
    }

    private onEquip(e: EquipEvent) {
        if (!e.actor || e.actor.getFormID() !== 0x14 || !e.baseObj) {
            return;
        }
        if (Potion.from(e.baseObj)) {
            this.ownDrinks.set(e.baseObj.getFormID(), Date.now());
        }
    }

    private onMagicEffects(e: ConnectionMessage<MagicEffectsMessage>) {
        if (this.sp.settings["skymp5-client"]["magicEffects"] === false) {
            return;
        }
        this.pending.push(e.message);
    }

    private isOwnDrink(source: number): boolean {
        const at = this.ownDrinks.get(source);
        return at !== undefined && Date.now() - at < MagicEffectsService.ownDrinkMs;
    }

    private onUpdate() {
        if (this.pending.length === 0) {
            return;
        }
        const messages = this.pending;
        this.pending = [];
        const remoteServer = this.controller.lookupListener(RemoteServer);
        for (const msg of messages) {
            const id = remoteServer.getIdManager().getId(msg.idx);
            const own = id === remoteServer.getMyActorIndex();
            const refr: ObjectReference | null = own ? Game.getPlayer() : getObjectReference(id);
            if (!refr) {
                logTrace(this, "no reference here for the effects of", msg.idx);
                continue;
            }
            const key = refr.getFormID();
            const before = this.shown.get(key) ?? new Set<number>();
            const now = new Set<number>();
            for (const effect of msg.effects) {
                if (own && this.isOwnDrink(effect.source)) {
                    continue;
                }
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
    }

    // how long an effect without duration (a heal at once) shows: a
    // presentation choice, about the engine's own flash
    private static readonly shownOnceSeconds = 1.5;
    // how long after its own drink this game leaves the drink's effects to
    // its engine: the server's message comes back within a round trip
    private static readonly ownDrinkMs = 5000;

    private pending: MagicEffectsMessage[] = [];
    private shown = new Map<number, Set<number>>();
    private ownDrinks = new Map<number, number>();
}
