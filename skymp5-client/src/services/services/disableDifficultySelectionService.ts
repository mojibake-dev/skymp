import { ClientListener, Sp, CombinedController } from "./clientListener";
import { logError, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { CreateActorMessage } from "../messages/createActorMessage";

// The server's difficulty is this game's: the player's own create word names
// it (props.difficulty, 0 Novice to 5 Legendary), and this service writes it
// to the player's own difficulty field through the platform's
// setPlayerDifficulty, which DamageActor (37335) indexes for every hit this
// engine applies itself (a trap's, a fall's): the damage one actor deals
// another is the server's and takes the same difficulty there
// (docs/verbs/difficulty.md). The INI setting iDifficulty:GamePlay is written
// to the same value, though it never reaches the field. Both are written again
// every 60 updates, so a choice in the game's menu is put back; the platform
// writes the field, and logs it, only when it differs. Nothing is written
// before the server names a difficulty, so the game keeps its own until then.
export class DisableDifficultySelectionService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
        this.controller.on("update", () => this.onUpdate());
    }

    private onCreateActor(e: ConnectionMessage<CreateActorMessage>) {
        if (!e.message.isMe) {
            return;
        }
        const v = e.message.props?.difficulty;
        if (v === undefined) {
            return;
        }
        if (!Number.isInteger(v) || v < 0 || v > 5) {
            logError(this, `difficulty ${JSON.stringify(v)} refused (an integer from 0 to 5); ${this.difficulty ?? "the game's own"} kept`);
            return;
        }
        logTrace(this, `the server's difficulty ${v}`);
        this.difficulty = v;
        this.counter = DisableDifficultySelectionService.updatesPerWrite;
    }

    private onUpdate() {
        if (this.difficulty === undefined) {
            return;
        }
        this.counter++;
        if (this.counter < DisableDifficultySelectionService.updatesPerWrite) {
            return;
        }
        this.counter = 0;
        this.sp.setPlayerDifficulty(this.difficulty);
        this.sp.Utility.setINIInt("iDifficulty:GamePlay", this.difficulty);
    }

    private static readonly updatesPerWrite = 60;

    private difficulty: number | undefined;

    private counter = 0;
}
