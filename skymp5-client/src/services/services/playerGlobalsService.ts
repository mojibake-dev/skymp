import { MsgType } from "../../messages";
import { logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { PlayerGlobalsMessage } from "../messages/playerGlobalsMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";

// How often the watched globals are read, ms: the server takes one report a
// second (wire-validate's player_globals_budget)
const WATCH_MS = 1000;

// The storage key of the list to watch (sp.storage survives a plugin reload)
const WATCH_STORAGE_KEY = "playerGlobalsWatch";

// thuum docs/verbs/player-globals.md (ADR-029's A2): the globals the server
// keeps per player (Apocalypse's progress and its MCM settings) follow the
// player. After a login the server sends the list to watch and the recorded
// values, which are set on the next update (GlobalVariable.setValue, as
// TimeService sets the clock's); then the watched globals are read once a
// second and the ones whose values changed are sent. Nothing is watched
// before the server's list arrives, so no report comes before the record is
// set.
//
// The list lives in the platform's storage (ADR-041's survivor of a plugin
// reload), not in this object alone: a hot-reloaded client (lab client-hot,
// ADR-054) keeps its connection and never logs in again, so the server's list
// never comes a second time. Playtest 36 (2026-10-28): rotfern's Survival
// needs froze in the record at 13:25:21Z, two seconds after the seat's hot
// push, until its relaunch at 17:45Z. A fresh service with a stored list
// resumes the watch and reports every watched value once, so the record
// catches up with whatever moved during the reload.
export class PlayerGlobalsService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("playerGlobalsMessage", (e) => this.onPlayerGlobalsMessage(e));
        this.controller.on("update", () => this.onUpdate());
        this.resume();
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
        this.sp.storage[WATCH_STORAGE_KEY] = [...m.watch];
        this.last = new Map();
        for (const id of this.watch) {
            const value = this.read(id);
            if (value !== undefined) {
                this.last.set(id, value);
            }
        }
        logTrace(this, "watching", this.watch.length, "globals,", m.values.length, "set from the record");
    }

    // After a plugin reload: the list the last login brought, from storage;
    // `last` stays empty so the next read reports every watched value once
    private resume() {
        const stored: unknown = this.sp.storage[WATCH_STORAGE_KEY];
        if (!Array.isArray(stored) || stored.length === 0) {
            return;
        }
        const list = stored.filter((id): id is number => typeof id === "number" && Number.isInteger(id) && id > 0);
        if (list.length === 0) {
            return;
        }
        this.watch = list;
        this.last = new Map();
        logTrace(this, "resumed watching", this.watch.length, "globals after a reload; a full report follows");
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
