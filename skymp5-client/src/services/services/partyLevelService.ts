import { logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { PartyLevelMessage } from "../messages/partyLevelMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// docs/verbs/leveling.md (the party's level): the level this game's
// PC-level-mult NPCs scale to, the party's level the server tells it
// (PartyLevel, MsgType 90; R0). Each new level is handed to the platform's
// setPartyLevel, whose detour of the engine's level answers it for every
// PC-level-mult base in place of this game's own player's level and whose
// recompute gives the loaded ones their new health at once. A level that is
// not a whole number from 1 to 65535 is never handed on; the same level
// twice is handed on once.
export class PartyLevelService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("partyLevelMessage", (e) => this.onPartyLevel(e));
    }

    private onPartyLevel(event: ConnectionMessage<PartyLevelMessage>) {
        const level = event.message.level;
        if (typeof level !== "number" || !Number.isInteger(level) || level < 1 || level > 0xffff) {
            logTrace(this, `party level ${String(level)} refused: not a whole number from 1 to 65535`);
            return;
        }
        if (level === this.handed) {
            return;
        }
        this.handed = level;
        this.sp.setPartyLevel(level);
        logTrace(this, `party level ${level}: the PC-level-mult NPCs here scale to it`);
    }

    private handed = 0;
}
