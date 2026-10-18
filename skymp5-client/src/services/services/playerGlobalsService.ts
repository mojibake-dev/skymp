import { MsgType } from "../../messages";
import { logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { PlayerGlobalsMessage } from "../messages/playerGlobalsMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// How often the watched globals are read, ms: the server takes one report a
// second (wire-validate's player_globals_budget)
const WATCH_MS = 1000;

// thuum docs/verbs/player-globals.md (ADR-029's A2): the globals the server
// keeps per player (Apocalypse's progress and its MCM settings) follow the
// player. After a login the server sends the list to watch and the recorded
// values, which are set on the next update (GlobalVariable.setValue, as
// TimeService sets the clock's); then the watched globals are read once a
// second and the ones whose values changed are sent. Nothing is watched
// before the server's list arrives, so no report comes before the record is
// set.
export class PlayerGlobalsService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("playerGlobalsMessage", (e) => this.onPlayerGlobalsMessage(e));
        this.controller.on("update", () => this.onUpdate());
    }

    private onPlayerGlobalsMessage(e: ConnectionMessage<PlayerGlobalsMessage>) {
        this.pending = e.message;
    }

    private onUpdate() {
        if (this.pending) {
            const m = this.pending;
            this.pending = undefined;
            this.apply(m);
            return;
        }
        const now = Date.now();
        if (this.watch.length === 0 || now - this.lastRead < WATCH_MS) {
            return;
        }
        this.lastRead = now;
        const changed: Array<{ global: number; value: number }> = [];
        for (const id of this.watch) {
            const value = this.read(id);
            if (value === undefined || value === this.last.get(id)) {
                continue;
            }
            this.last.set(id, value);
            changed.push({ global: id, value });
        }
        if (changed.length > 0) {
            this.controller.emitter.emit("sendMessage", {
                message: { t: MsgType.PlayerGlobals, watch: [], values: changed },
                reliability: "reliable",
            });
        }
    }

    private apply(m: PlayerGlobalsMessage) {
        for (const { global, value } of m.values) {
            const g = this.global(global);
            if (g) {
                g.setValue(value);
            }
        }
        this.watch = m.watch;
        this.last = new Map();
        for (const id of this.watch) {
            const value = this.read(id);
            if (value !== undefined) {
                this.last.set(id, value);
            }
        }
        logTrace(this, "watching", this.watch.length, "globals,", m.values.length, "set from the record");
    }

    private global(id: number) {
        return this.sp.GlobalVariable.from(this.sp.Game.getFormEx(id));
    }

    private read(id: number): number | undefined {
        const g = this.global(id);
        return g ? g.getValue() : undefined;
    }

    private pending?: PlayerGlobalsMessage;
    private watch: number[] = [];
    private last = new Map<number, number>();
    private lastRead = 0;
}
