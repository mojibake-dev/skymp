import { logLab } from "../../logging";
import { Actor, DeathEvent } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { ActorDeathMessage } from "../messages/actorDeathMessage";
import { HostService } from "./hostService";
import { DeathService } from "./deathService";
import { localIdToRemoteId } from "../../view/worldViewMisc";

// thuum docs/verbs/npc-fate.md (ADR-048's B): this game's report of a death
// its engine dealt to a game-file actor it simulates, for the server to
// check against what it saw (ActionListener::OnActorDeath). Two readings of
// the same fact:
// - the floor: every viewed actor carries the engine's deferred kill
//   (formView startDeferredKill), so a death the server has no number for
//   (a fall, a trap, an actor's blow whose engine damage outran the server's)
//   leaves the actor at or under zero health and alive, with no death event
//   at all (x-npc-fall-report 20261025-004503: the wolf fell 4000 units and
//   nothing fired). The poll below reads every hosted game-file actor's
//   health twice a second and reports the first time it reaches the floor,
//   once per visit there (cause 0: the server picks its evidence, a drop in
//   this game's samples, a numbered hit, a trigger's firing);
// - deathStart, for an actor the deferred kill did not hold: the kill
//   itself (deathEnd waits for the ragdoll to settle), cause 4 with the
//   killer when the engine names an actor, else 0.
// Not reported: a death that is the server's own word (DeathService.
// takeServerKill marks the kills it plays), the player's own, a figure's
// or a copy's (0xff ids: the server's actors, their deaths the server's
// numbers), a puppet's (another game simulates it: not mine).
export class DeathReportService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("deathStart", (e) => this.onDeathStart(e));
        // ADR-050 decision 2: the floor poll is retired; the engine that
        // simulates an NPC dies by itself from the server's cause and
        // deathStart is the one signal (a puppet never reaches zero alive on
        // this seat: the deferred kill is the reconciler's for puppets alone)
    }

    private onDeathStart(e: DeathEvent) {
        const dying = e.actorDying ? this.sp.Actor.from(e.actorDying) : null;
        if (!dying) {
            return;
        }
        const localId = dying.getFormID();
        if (localId === 0x14 || localId >= 0xff000000) {
            return;
        }
        if (this.controller.lookupListener(DeathService).takeServerKill(localId)) {
            return;
        }
        if (!this.controller.lookupListener(HostService).isMineLocal(localId)) {
            return;
        }
        const killer = e.actorKiller ? this.sp.Actor.from(e.actorKiller) : null;
        const killerLocal = killer ? killer.getFormID() : 0;
        this.send(dying, localIdToRemoteId(localId), killerLocal !== 0 ? 4 : 0,
            killerLocal !== 0 ? localIdToRemoteId(killerLocal, true) : 0, "deathStart");
    }

    private send(actor: Actor, refrId: number, cause: number, killer: number, how: string) {
        if (refrId === 0) {
            return;
        }
        const message: ActorDeathMessage = {
            t: MsgType.ActorDeath,
            refrId,
            cause,
            killer,
            pos: [actor.getPositionX(), actor.getPositionY(), actor.getPositionZ()],
        };
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logLab("verb:npc-fate", `death report: ${refrId.toString(16)} died in this game by ${how}, killer ${killer.toString(16)}, cause ${cause}, sent`);
    }

}
