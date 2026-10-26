import { Actor, Game } from "skyrimPlatform";
import { logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { DamageCauseMessage } from "../messages/damageCauseMessage";
import { getObjectReference } from "../../view/worldViewMisc";
import { remoteIdToLocalId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
import { RemoteServer } from "./remoteServer";

// thuum docs/verbs/npc-fate.md (ADR-050 decision 2, deaths by the engine):
// the server's number for a hit on an NPC this game hosts arrives as a cause
// (the resulting health, the aggressor, a kill move) instead of absolute
// values, and the engine applies it through its own damage entry (the fork's
// Skyrim Platform export damageActor: the delta from the engine's current
// health, downward through DamageActor so the engine reacts, records the
// killer and dies by itself at zero, upward through its restore). This game's
// own engine damage to an NPC it simulates is cut at the same entry when the
// hand is a player's or a figure's (FridaHooks.cpp; the server's number is
// the one number), under the client setting npcEngineDamage, which must
// stand with the server's npcEngineDeaths (both default on). A cause for an
// actor this game does not simulate (a late word after a handoff) is dropped
// here; the puppets' values ride ChangeValues.
export class DamageCauseService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.enabled = this.sp.settings["skymp5-client"]["npcEngineDamage"] !== false;
        this.sp.setEngineDamageCut(this.enabled);
        logTrace(this, this.enabled
            ? "npcEngineDamage is on (the default): a hosted NPC takes the server's number through the engine's own damage entry, the engine's own cut"
            : "npcEngineDamage is off by the settings file: the engine's own damage stands and the cause is ignored");
        this.controller.emitter.on("damageCauseMessage", (e) => this.onDamageCause(e));
    }

    isEnabled(): boolean {
        return this.enabled;
    }

    private onDamageCause(event: ConnectionMessage<DamageCauseMessage>) {
        if (!this.enabled) {
            return;
        }
        const msg = event.message;
        // the forms are looked up on the update tick, never in the handler
        this.controller.once("update", () => {
            const remote = this.controller.lookupListener(RemoteServer);
            const id = remote.getIdManager().getId(msg.idx);
            const refr = getObjectReference(id);
            const actor = refr ? Actor.from(refr) : null;
            if (!actor) {
                logTrace(this, `cause for idx ${msg.idx}: no actor held here, dropped`);
                return;
            }
            const localId = actor.getFormID();
            if (!this.controller.lookupListener(HostService).isMineLocal(localId)) {
                logTrace(this, `cause for ${localId.toString(16)}: not simulated here, dropped`);
                return;
            }
            const attackerLocal = msg.attacker ? remoteIdToLocalId(msg.attacker) : 0;
            const queued = this.sp.damageActor(localId, msg.health, attackerLocal || 0, msg.killMove);
            logLab("verb:npc-fate", `cause on ${localId.toString(16)}: health ${msg.health.toFixed(3)}, attacker ${msg.attacker.toString(16)} (local ${(attackerLocal || 0).toString(16)}), killMove ${msg.killMove}: ${queued ? "queued" : "refused"}`);
        });
    }

    private readonly enabled: boolean;
}
