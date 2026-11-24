import { LookAskedEvent } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { logLab } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { PlayerLookAskMessage } from "../messages/playerLookAskMessage";
import { PlayerLookMessage } from "../messages/playerLookMessage";
import { localIdToRemoteId } from "../../view/worldViewMisc";
import { LOOK_CHECK_MS, LookWatches } from "../../sync/lookWatch";

// A look a script asks of another player, answered by that player's own game.
// The asking side: a script this seat runs for another player (inside that
// player's acting scope) asked the player's figure whether it sees a target,
// which the engine refuses for anyone but its own player when the target is
// no actor; the platform answers the script from its look table and raises
// lookAsked, and this seat sends PlayerLookAsk. The server's PlayerLook, that
// player's own game's answer, is written into the table (setPlayerLook). The
// looking side: the server's PlayerLookAsk starts or extends a watch of this
// seat's own player's look at the target (LookWatches), read with the
// engine's own test (the camera's) and sent as PlayerLook.
export class LookService extends ClientListener {
    private watches = new LookWatches();
    private lastCheck = 0;

    constructor(
        private sp: Sp,
        private controller: CombinedController,
    ) {
        super();
        try {
            this.controller.on("lookAsked", (e) => this.onLookAsked(e));
        } catch {
            // an older platform raises no lookAsked
        }
        this.controller.emitter.on("playerLookMessage", (e) => this.onPlayerLook(e));
        this.controller.emitter.on("playerLookAskMessage", (e) => this.onAsk(e));
        this.controller.on("update", () => this.onUpdate());
    }

    private onLookAsked(e: LookAskedEvent) {
        const message: PlayerLookAskMessage = { t: MsgType.PlayerLookAsk, player: e.player, target: e.target };
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        logLab("verb:look", `asked ${e.player.toString(16)}'s own game for its look at ${e.target.toString(16)}`);
    }

    private onPlayerLook(e: ConnectionMessage<PlayerLookMessage>) {
        const m = e.message;
        const stored = typeof this.sp.setPlayerLook === "function" ? this.sp.setPlayerLook(m.player, m.target, m.los) : false;
        logLab("verb:look", `${m.player.toString(16)}'s own game: ${m.los ? "sees" : "does not see"} ${m.target.toString(16)}${stored ? "" : " (not stored: no setPlayerLook)"}`);
    }

    private onAsk(e: ConnectionMessage<PlayerLookAskMessage>) {
        this.watches.ask(e.message.target, Date.now());
    }

    private onUpdate() {
        const now = Date.now();
        if (now - this.lastCheck < LOOK_CHECK_MS) {
            return;
        }
        this.lastCheck = now;
        const targets = this.watches.targets(now);
        if (targets.length === 0) {
            return;
        }
        const player = this.sp.Game.getPlayer();
        const me = localIdToRemoteId(0x14);
        for (const target of targets) {
            const ref = this.sp.ObjectReference.from(this.sp.Game.getFormEx(target));
            const los = !!(player && ref && player.hasLOS(ref));
            if (!this.watches.read(target, los) || me === 0) {
                continue;
            }
            const message: PlayerLookMessage = { t: MsgType.PlayerLook, player: me, target, los };
            this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
            logLab("verb:look", `this player ${los ? "sees" : "does not see"} ${target.toString(16)}: sent`);
        }
    }
}
