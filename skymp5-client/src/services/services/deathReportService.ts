import { logLab } from "../../logging";
import { DeathEvent } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { ActorDeathMessage } from "../messages/actorDeathMessage";
import { HostService } from "./hostService";
import { DeathService } from "./deathService";
import { localIdToRemoteId } from "../../view/worldViewMisc";

// thuum docs/verbs/npc-fate.md (ADR-048's B): this game's report of a death
// its engine dealt to a game-file actor it simulates, for the server to
// check against what it saw (ActionListener::OnActorDeath). Not reported: a
// death that is the server's own word (DeathService.takeServerKill, the kill
// it played), the player's own, a figure's or a copy's (0xff ids: the
// server's actors, their deaths the server's numbers), a puppet's (another
// game simulates it: not mine). Read at deathStart, the kill itself, since
// deathEnd waits for the ragdoll to settle and the server's windows for the
// evidence (a hit within 3 s, a drop within 4 s) run from the kill. The
// cause is 4 with the killer when the engine names an actor, else 0: the
// server picks any evidence it has (a drop in this game's samples, an
// activation beside the body).
export class DeathReportService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("deathStart", (e) => this.onDeathStart(e));
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
        const message: ActorDeathMessage = {
            t: MsgType.ActorDeath,
            refrId: localIdToRemoteId(localId),
            cause: killerLocal !== 0 ? 4 : 0,
            killer: killerLocal !== 0 ? localIdToRemoteId(killerLocal, true) : 0,
            pos: [dying.getPositionX(), dying.getPositionY(), dying.getPositionZ()],
        };
        if (message.refrId === 0) {
            return;
        }
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logLab("verb:npc-fate", `death report: ${localId.toString(16)} died in this game, killer ${killerLocal.toString(16)} (remote ${message.killer.toString(16)}), cause ${message.cause}, sent`);
    }
}
