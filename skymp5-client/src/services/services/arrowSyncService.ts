import { Actor, Ammo, Game, Weapon } from "skyrimPlatform";
import { logError, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { ArrowShotMessage } from "../messages/arrowShotMessage";
import { getObjectReference } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RemoteServer } from "./remoteServer";

// thuum docs/verbs/marksman.md: a neighbour's arrow flies on this screen. The
// server sends each shot it took from another player (ArrowShot); this game
// launches the arrow from that player's figure along its shooter's aim
// (TESModPlatform.LaunchArrow). The figure's arrow is drawn: a figure deals
// no damage here (formView.ts, attackDamageMult 0), and the hit that counts
// is the shooter's own game's, checked by the server. Natives need the VM's
// context, so a shot waits for the next update.
export class ArrowSyncService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("arrowShotMessage", (e) => this.onArrowShot(e));
        this.controller.on("update", () => this.onUpdate());
    }

    private onArrowShot(e: ConnectionMessage<ArrowShotMessage>) {
        if (this.sp.settings["skymp5-client"]["arrowSync"] === false) {
            return;
        }
        this.pending.push(e.message);
    }

    private onUpdate() {
        if (this.pending.length === 0) {
            return;
        }
        const shots = this.pending;
        this.pending = [];
        const remoteServer = this.controller.lookupListener(RemoteServer);
        for (const shot of shots) {
            const id = remoteServer.getIdManager().getId(shot.idx);
            if (id === remoteServer.getMyActorIndex()) {
                continue; // this player's own shot: its game flew it already
            }
            const shooter = Actor.from(getObjectReference(id));
            const weapon = Weapon.from(Game.getFormEx(shot.weaponId));
            const ammo = Ammo.from(Game.getFormEx(shot.ammoId));
            if (!shooter || !weapon || !ammo) {
                logTrace(this, "no figure, bow or arrow here for a shot of", shot.idx);
                continue;
            }
            try {
                this.sp.callNative("TESModPlatform", "LaunchArrow", undefined,
                    shooter, weapon, ammo, shot.power, shot.aimAngle, shot.aimHeading);
            } catch (err) {
                logError(this, "TESModPlatform.LaunchArrow failed", err);
            }
        }
    }

    private pending: ArrowShotMessage[] = [];
}
