import { logTrace } from "../../logging";
import { ActorFateMessage } from "../messages/actorFateMessage";
import { ConnectionMessage } from "../events/connectionMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// thuum docs/verbs/npc-fate.md (ADR-046 decision 1, ADR-049): the server's
// roll is the one roll. ActorFate names, for every game-file actor in the
// squares this game is about to attach, the NPC_ the server's leveled roll
// chose and the worn list its outfit roll chose; the fates go into the fork's
// Skyrim Platform table (setActorFate), where two native detours make the
// engine roll them at the reference's load (the leveled roll wrapper) and
// at its first dressing (InventoryChanges::InitOutfitItems). A fate for a
// reference the engine already holds (its cell attached before the fate
// arrived, or a respawn's new fate) goes through the engine's own re-roll
// (rerollActor), one pop, accepted by ADR-049. The switch npcFate (settings
// skymp5-client.npcFate, default OFF) arms the detours; until the lab read
// x-npc-fate-roll confirms the roll's moments (docs/research/m3/
// re-npc-roll.md section 7) they are HYPOTHESIS and stay pass-throughs, the
// fates kept for the day they turn on.
export class ActorFateService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.enabled = this.sp.settings["skymp5-client"]["npcFate"] === true;
        this.sp.setActorFateEnabled(this.enabled);
        logTrace(this, this.enabled
            ? "npcFate is on: the engine rolls what the server rolled (the two detours armed)"
            : "npcFate is off (the default until x-npc-fate-roll): fates are kept, the detours pass every roll through");
        this.controller.emitter.on("actorFateMessage", (e) => this.onActorFate(e));
    }

    private onActorFate(event: ConnectionMessage<ActorFateMessage>) {
        for (const fate of event.message.fates) {
            this.sp.setActorFate(fate.refrId, fate.baseId, fate.worn);
            const key = `${fate.baseId}:${fate.worn.join(",")}`;
            const before = this.applied.get(fate.refrId);
            this.applied.set(fate.refrId, key);
            if (!this.enabled || before === key) {
                continue;
            }
            // the late path: the engine has the reference already (the save
            // around the player at a login, a cell attached ahead of the fate,
            // a respawn's new roll): its own re-roll goes through the detour
            const refr = this.sp.Game.getFormEx(fate.refrId);
            if (refr && this.sp.Actor.from(refr)) {
                const queued = this.sp.rerollActor(fate.refrId);
                this.sp.printConsole(`[verb:npc-fate] late fate for ${fate.refrId.toString(16)}: reroll ${queued ? "queued" : "refused"}`);
            }
        }
    }

    // the last fate applied per reference (base and worn), so a repeated fate
    // for the same roll re-rolls nothing
    private applied = new Map<number, string>();
    private enabled: boolean;
}
