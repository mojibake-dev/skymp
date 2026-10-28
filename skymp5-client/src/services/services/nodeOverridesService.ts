import { NetImmerse } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RemoteServer } from "./remoteServer";
import { ModelApplyUtils } from "../../view/modelApplyUtils";
import { logTrace } from "../../logging";

// thuum docs/verbs/racemenu-sync.md (playtest 35, 2026-10-27: rotfern's head
// node read 0.94 against RaceMenu's 1 after every teleport and after a
// relaunch into Bleak Falls, 1.0 after Riverwood's; Prisoner's 1 throughout):
// the server's node scales and texture sets for the player's own actor were
// applied once, at CreateActor, and a 3D rebuilt after that (a cell change's
// load, an interior's late 3D) came up with the race's own transforms. They
// go on again at every 3D load of the player's actor, and the scale is read
// back: a node not yet built takes nothing, so the apply retries over the
// next updates until the engine reads the server's number. Figures get the
// same in formView.
export class NodeOverridesService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("update", () => this.onUpdate());
    }

    private onUpdate() {
        const player = this.sp.Game.getPlayer();
        if (!player) {
            return;
        }
        if (!player.is3DLoaded()) {
            this.appliedTo3D = false;
            this.tries = 0;
            return;
        }
        if (this.appliedTo3D) {
            return;
        }
        const overrides = this.controller.lookupListener(RemoteServer).getMyNodeOverrides();
        if (!overrides) {
            return;
        }
        this.tries += 1;
        ModelApplyUtils.applyModelNodeScale(player, overrides.setNodeScale);
        ModelApplyUtils.applyModelNodeTextureSet(player, overrides.setNodeTextureSet);
        const scales = overrides.setNodeScale || [];
        const landed = scales.every((entry) => Math.abs(NetImmerse.getNodeScale(player, entry.nodeName, false) - entry.scale) < 0.001);
        if (landed || this.tries >= NodeOverridesService.maxTries) {
            this.appliedTo3D = true;
            logTrace(this, `the player's node overrides re-applied at a 3D load: ${scales.length} scale(s), ${(overrides.setNodeTextureSet || []).length} texture set(s), ${landed ? "landed" : "not read back"} after ${this.tries} update(s)`);
        }
    }

    private appliedTo3D = false;
    private tries = 0;
    private static readonly maxTries = 30;
}
