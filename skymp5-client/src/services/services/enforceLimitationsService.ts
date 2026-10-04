import { GameLoadEvent } from "../events/gameLoadEvent";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// Saving stays off: the server owns the world. Waiting is allowed (thuum
// docs/verbs/rest.md): a rest is the player's own, the shared clock does not
// move, and TimeService reports it for the server to grant its recovery.
export class EnforceLimitationsService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        controller.once("update", () => this.onceUpdate());
        controller.emitter.on("gameLoad", (e) => this.onGameLoad(e));
    }

    private onceUpdate() {
        this.sp.Game.setInChargen(true, false, false);
    }

    private onGameLoad(event: GameLoadEvent) {
        this.sp.Game.setInChargen(true, false, false);
    }
}
