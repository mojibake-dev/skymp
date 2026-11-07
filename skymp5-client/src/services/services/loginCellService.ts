import { ObjectReference } from "skyrimPlatform";
import { logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
import { ScriptStateService } from "./scriptStateService";

// thuum docs/DECISIONS.md D-L1 B (Eli, 2026-11-06; built from the plan's
// section 4, docs/research/rung3-script-record-2026-11-06.md, and the finding
// in docs/verbs/traps.md, "The login inside a cell"): a seat that logs in
// inside a cell reaches it by a save load (RemoteServer's "loading game in
// world/cell", the template save rewritten with the server's cell and
// position), which raises no OnCellAttach and no OnCellLoad for that cell's
// scripts (HYPOTHESIS H-L1), so playtest 47's Bleak Falls Temple pillars sat
// in their empty state and its darts never armed. Once per login this service
// sends the two events a real cell load sends, OnCellAttach and then
// OnCellLoad, to each scripted reference of that cell, through rung 3's
// TESModPlatform.SendScriptEvent (under the platform's mark, so the gate
// passes them as it passes a real load's) with unseenOnly, which skips an
// event the engine served to the reference since the load: a second
// OnCellLoad can put a pillar's script back on its first side while its graph
// stays turned.
//
// Which cell: the player's own interior cell at the login's arrival
// (QuestStageService's idiom: this connection's own CreateActor and the
// game's load, both since connectionAccepted), once HostService reports it
// without its load event (noteWithoutLoadEvent, from its once-a-second read
// of the player's cell). Nothing is sent for a cell whose load event came,
// for an exterior (the decision's case is the interior; whether an
// exterior's save load raises its cells' entry events is unread), for a
// reconnect with the game up (it spawns through MoveRefrToPosition and loads
// nothing) or after a plugin reload (client-hot keeps the connection, so no
// login and no load follow it): a cell the engine loaded is never sent them.
//
// When: after the cell's grant and ScriptStateService's apply pass for it
// (afterHosting: the gate open, the cell's records applied or given up), so
// the events land owned and after the records (a pillar whose record set
// doOnce does not jump again); with no grant within GRANT_WAIT_MS of the
// report (another seat hosts the cell, or none does), then, as its guest. A
// reference whose script record still waits for its apply (viewOf "pending":
// a record with a view holds its load events until then) is left to it.
//
// What: the cell's references through Cell.getNumRefs and getNthRef with
// filter 0, every reference (SKSE's, skse64 2.02.06 PapyrusCell.cpp:9-57),
// REFS_PER_UPDATE an update, every OnCellAttach first and then every
// OnCellLoad, the order of the engine's attach and fully-loaded events. An
// actor is passed over (its scripts run where it is simulated, ADR-062, a
// puppet's never, and a send under the platform's mark passes the gate), as
// are the player and the server's copies; a reference no Papyrus call reads
// (a projectile: every call on one aborts, lab-driver's refs-near note) is
// counted unreadable. The line, the counts first (the console tee cuts a line
// near 128 bytes): "login cell <c>: OnCellAttach and OnCellLoad sent once to
// <n>, <m> already seen; <how>" (<how> "after its grant" or the guest's
// words), n the references sent at least one of the two, m those the engine
// had served one of them already (H-L1's read: 0).
const TAG = "verb:scripts";
const PLAYER_ID = 0x14;
// the server's copies begin here (ScriptStateService's LAST_GAME_FILE_ID)
const FIRST_COPY_ID = 0xff000000;
// SKSE's Cell.GetNumRefs and GetNthRef filter: 0 is every reference
const EVERY_TYPE = 0;
// SendScriptEvent's refusal for a reference with no script (ScriptState.cpp)
const NO_SCRIPT = "no attached script";
// after the arrival, how long HostService has to report the player's cell
// without its load event (it reads the cell once a second), ms
const ARRIVAL_WINDOW_MS = 3000;
// after that report, how long a grant of the cell is waited for before the
// events go as its guest's, ms (UNMEASURED: the server elects at the report,
// so a grant comes within a round trip)
const GRANT_WAIT_MS = 10_000;
// the references walked an update (UNMEASURED: about four natives each, every
// one a round trip to the OnUpdate thread, so a big interior spreads over
// frames)
const REFS_PER_UPDATE = 128;

// one login cell's walk
type Walk = {
    cell: number;
    how: string;            // how the events went: after its grant, or as its guest
    loading: boolean;       // the OnCellLoad pass, after every OnCellAttach
    next: number;           // the next index: into the cell's references, then into `scripted`
    visited: Set<number>;   // each reference walked once, should the cell's list shift between updates
    scripted: number[];     // the references an OnCellAttach answer named scripted, in the cell's order
    sent: Set<number>;      // sent at least one of the two
    seen: Set<number>;      // the engine had served at least one of the two (unseenOnly skipped it)
    attachSeen: number;
    loadSeen: number;
    pending: number;        // left to a script record still waiting for its apply
    refused: number;
    firstRefusal: string;
    passed: number;         // actors, the player and the server's copies
    unreadable: number;
    since: number;
};

// SendScriptEvent's answer ({formId, event, sent, seen, scripts, reason}) as
// the walk reads it
type SendAnswer = { sent: boolean; seen: boolean; reason: string };

const parseSend = (raw: unknown): SendAnswer => {
    if (typeof raw !== "string") {
        return { sent: false, seen: false, reason: `answered no string (${typeof raw})` };
    }
    try {
        const o = JSON.parse(raw) as Record<string, unknown> | null;
        if (o && typeof o === "object" && typeof o["sent"] === "boolean") {
            const reason = o["reason"];
            return { sent: o["sent"] === true, seen: o["seen"] === true, reason: typeof reason === "string" ? reason : "" };
        }
    } catch (e) {
        // the raw answer below
    }
    return { sent: false, seen: false, reason: `answered ${raw.slice(0, 40)}` };
};

export class LoginCellService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("connectionAccepted", () => this.onConnectionAccepted());
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
        this.controller.emitter.on("gameLoad", () => this.onGameLoad());
        this.controller.emitter.on("cellHostChanged", (e) => this.onCellHostChanged(e));
        this.controller.on("update", () => this.onUpdate());
    }

    // HostService's, from its once-a-second read: the player's own cell stood
    // attached with no load event for it (JS state alone; the update decides
    // whether it is the login's cell)
    noteWithoutLoadEvent(cell: number): void {
        if (!this.done && !this.reported.has(cell)) {
            this.reported.set(cell, Date.now());
        }
    }

    // a new connection (NetworkingService's connectionAccepted, before the
    // server's CreateActor): the arrival starts over, so each login's is its
    // own and none is carried across a reconnect; a walk it cuts is said
    private onConnectionAccepted(): void {
        if (this.walk) {
            logTrace(this, `login cell ${this.walk.cell.toString(16)}: its walk cut by a new connection, ${this.walk.sent.size} reference(s) sent the entry events`);
            this.walk = undefined;
        }
        this.generation++;
        this.loginSeen = false;
        this.gameLoaded = false;
        this.done = false;
        this.armedAt = 0;
        this.loginCell = 0;
        this.loginCellAt = 0;
        this.reported.clear();
        this.granted.clear();
        this.gateOpen.clear();
    }

    // this connection's own actor (a login, a reconnect, a restart): the
    // login half of the arrival
    private onCreateActor(e: ConnectionMessage<CreateActorMessage>): void {
        if (e.message.isMe) {
            this.loginSeen = true;
        }
    }

    // the game's load (LoadGameService's gameLoad, the engine's own load-game
    // event): the other half; a reconnect with the game up loads nothing
    private onGameLoad(): void {
        this.gameLoaded = true;
    }

    // HostService's grant or release (a message handler: JS state alone): a
    // granted cell's gate opens after its records' apply pass, on an update
    // (afterHosting runs this there); a release shuts it
    private onCellHostChanged(e: { cell: number; hosted: boolean }): void {
        if (this.done) {
            return;
        }
        const cell = e.cell;
        this.gateOpen.delete(cell);
        if (!e.hosted) {
            return;
        }
        this.granted.add(cell);
        const generation = this.generation;
        this.controller.lookupListener(ScriptStateService).afterHosting(cell, () => {
            if (generation === this.generation) {
                this.gateOpen.add(cell);
            }
        });
    }

    // the arrival's verdict, then the walk, on the update (callNative needs
    // the VM's context; every wrapper is taken again each update, since one
    // lives one Papyrus update)
    private onUpdate(): void {
        if (this.walk) {
            this.step(this.walk);
            return;
        }
        if (this.done || !this.loginSeen || !this.gameLoaded) {
            return;
        }
        const now = Date.now();
        if (this.armedAt === 0) {
            this.armedAt = now;
        }
        const player = this.sp.Game.getPlayer();
        const here = player ? player.getParentCell() : null;
        if (this.loginCell === 0) {
            if (!here || !here.isAttached()) {
                if (now - this.armedAt >= ARRIVAL_WINDOW_MS) {
                    this.settle(`the login's arrival found the player in no attached cell in ${ARRIVAL_WINDOW_MS} ms; no entry events sent`);
                }
                return;
            }
            this.loginCell = here.getFormID();
            this.loginCellAt = now;
            if (!here.isInterior()) {
                this.settle(`login cell ${this.loginCell.toString(16)}: an exterior, no entry events sent (D-L1 B is an interior's)`);
                return;
            }
        }
        const hex = this.loginCell.toString(16);
        const reportedAt = this.reported.get(this.loginCell);
        if (reportedAt === undefined) {
            if (now - this.loginCellAt >= ARRIVAL_WINDOW_MS) {
                this.settle(`login cell ${hex}: its load event came (not reported without it in ${ARRIVAL_WINDOW_MS} ms); no entry events sent`);
            }
            return;
        }
        if (!here || here.getFormID() !== this.loginCell) {
            this.settle(`login cell ${hex}: the player left it before its entry events went; none sent (the next cell's load is the engine's)`);
            return;
        }
        const hosted = this.controller.lookupListener(HostService).hostsCell(this.loginCell);
        const granted = this.granted.has(this.loginCell);
        if (hosted && !this.gateOpen.has(this.loginCell)) {
            return; // the grant's pass still applies the cell's records
        }
        if (!hosted && !granted && now - reportedAt < GRANT_WAIT_MS) {
            return; // a grant may still come
        }
        const how = hosted ? "after its grant" : granted ? "as a guest, its grant gone" : `as a guest, no grant in ${GRANT_WAIT_MS / 1000} s`;
        this.walk = {
            cell: this.loginCell, how, loading: false, next: 0, visited: new Set<number>(), scripted: [],
            sent: new Set<number>(), seen: new Set<number>(), attachSeen: 0, loadSeen: 0, pending: 0,
            refused: 0, firstRefusal: "", passed: 0, unreadable: 0, since: now,
        };
        this.step(this.walk);
    }

    // a throw ends the walk with its line, never an update's error every frame
    private step(w: Walk): void {
        try {
            this.stepWalk(w);
        } catch (e) {
            if (this.walk === w) {
                this.endWalk(w, `the walk threw (${String(e).slice(0, 60)})`);
            }
        }
    }

    // one update's share of the walk: the OnCellAttach pass over the cell's
    // references, then the OnCellLoad pass over the scripted ones, the cell
    // and every reference taken again by id
    private stepWalk(w: Walk): void {
        const cell = this.sp.Cell.from(this.sp.Game.getFormEx(w.cell));
        if (!cell || !cell.isAttached()) {
            this.endWalk(w, `the cell detached after ${w.visited.size} reference(s) walked`);
            return;
        }
        let budget = REFS_PER_UPDATE;
        if (!w.loading) {
            const scripts = this.controller.lookupListener(ScriptStateService);
            const n = cell.getNumRefs(EVERY_TYPE);
            for (; budget > 0 && w.next < n; budget--) {
                const ref = cell.getNthRef(w.next++, EVERY_TYPE);
                if (ref && !this.attachOne(w, ref, scripts)) {
                    return; // SendScriptEvent is not on this platform: said, the walk is over
                }
            }
            if (w.next >= n) {
                // the OnCellLoad pass from the next update, after the attach
                // events, as the engine's fully-loaded event follows them
                w.loading = true;
                w.next = 0;
            }
            return;
        }
        for (; budget > 0 && w.next < w.scripted.length; budget--) {
            const id = w.scripted[w.next++];
            const ref = this.sp.ObjectReference.from(this.sp.Game.getFormEx(id));
            if (!ref) {
                continue; // gone since its OnCellAttach
            }
            const answer = this.send(w, ref, "OnCellLoad");
            if (!answer) {
                return;
            }
            this.tally(w, id, answer, false);
        }
        if (w.next >= w.scripted.length) {
            this.endWalk(w, "");
        }
    }

    // one reference's OnCellAttach; false when SendScriptEvent is not on
    // this platform (the walk ended)
    private attachOne(w: Walk, ref: ObjectReference, scripts: ScriptStateService): boolean {
        let id = 0;
        let passed = false;
        try {
            id = ref.getFormID();
            passed = id === PLAYER_ID || id >= FIRST_COPY_ID || this.sp.Actor.from(ref) !== null;
        } catch (e) {
            w.unreadable++;
            return true;
        }
        if (w.visited.has(id)) {
            return true;
        }
        w.visited.add(id);
        if (passed) {
            w.passed++;
            return true;
        }
        if (scripts.viewOf(id) === "pending") {
            w.pending++;
            return true;
        }
        const answer = this.send(w, ref, "OnCellAttach");
        if (!answer) {
            return false;
        }
        if (answer.reason === NO_SCRIPT) {
            return true;
        }
        if (answer.sent || answer.seen) {
            w.scripted.push(id);
        }
        this.tally(w, id, answer, true);
        return true;
    }

    // SendScriptEvent with unseenOnly; undefined when the native is not on
    // this platform (an older dist: said once, the walk over); another throw
    // is that reference's refusal
    private send(w: Walk, ref: ObjectReference, name: string): SendAnswer | undefined {
        try {
            return parseSend(this.sp.callNative("TESModPlatform", "SendScriptEvent", undefined, ref, name, true));
        } catch (e) {
            const text = String(e);
            if (text.indexOf("Native function not found") !== -1) {
                this.walk = undefined;
                this.done = true;
                logLab(TAG, `login cell ${w.cell.toString(16)}: SendScriptEvent unavailable on this platform (${text}); no entry events sent`);
                return undefined;
            }
            return { sent: false, seen: false, reason: `threw (${text.slice(0, 60)})` };
        }
    }

    private tally(w: Walk, id: number, answer: SendAnswer, attach: boolean): void {
        if (answer.sent) {
            w.sent.add(id);
            return;
        }
        if (answer.seen) {
            w.seen.add(id);
            if (attach) {
                w.attachSeen++;
            } else {
                w.loadSeen++;
            }
            return;
        }
        w.refused++;
        if (w.firstRefusal === "") {
            w.firstRefusal = `${id.toString(16)}: ${answer.reason !== "" ? answer.reason : "refused"}`;
        }
    }

    // the login's one line, then the walk's coverage for a reader
    private endWalk(w: Walk, why: string): void {
        this.walk = undefined;
        this.done = true;
        const hex = w.cell.toString(16);
        // the per-event split and the notes after <how>, so the console's cut
        // near 128 bytes keeps the counts and how they went
        const notes: string[] = [];
        if (w.seen.size > 0) {
            notes.push(`seen: OnCellAttach ${w.attachSeen}, OnCellLoad ${w.loadSeen}`);
        }
        if (why !== "") {
            notes.push(why);
        }
        if (w.pending > 0) {
            notes.push(`${w.pending} left to their script records`);
        }
        if (w.refused > 0) {
            notes.push(`${w.refused} refused (${w.firstRefusal})`);
        }
        logLab(TAG, `login cell ${hex}: OnCellAttach and OnCellLoad sent once to ${w.sent.size}, ${w.seen.size} already seen; ${w.how}${notes.length > 0 ? `; ${notes.join("; ")}` : ""}`);
        logTrace(this, `login cell ${hex}: ${w.visited.size} reference(s) walked in ${Date.now() - w.since} ms, ${w.scripted.length} scripted, ${w.passed} actor(s) and copies passed over, ${w.unreadable} unreadable`);
    }

    // a verdict with nothing to send: once for this login
    private settle(why: string): void {
        this.done = true;
        logTrace(this, why);
    }

    private generation = 0;
    // the login's arrival, per connection: its own CreateActor, the game's
    // load, its verdict reached (done), the first update with both halves
    // (armedAt), and the player's cell first read attached after it
    // (loginCell, at loginCellAt; HostService's report is waited for from there)
    private loginSeen = false;
    private gameLoaded = false;
    private done = false;
    private armedAt = 0;
    private loginCell = 0;
    private loginCellAt = 0;
    // the cells HostService reported without their load event, by the
    // report's time; the cells granted since the login, and those whose
    // grant's gate opened
    private readonly reported = new Map<number, number>();
    private readonly granted = new Set<number>();
    private readonly gateOpen = new Set<number>();
    private walk: Walk | undefined;
}
