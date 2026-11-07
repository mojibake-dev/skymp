import { CellFullyLoadedEvent, ObjectReference } from "skyrimPlatform";
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
// of the player's cell). Nothing is sent for a cell whose load event came
// (no report comes; or the engine's own cellFullyLoaded for it comes before
// the OnCellLoad pass ended, which ends the walk there, and one after it is
// said with how late it came), for an exterior (the decision's case is the
// interior; whether an exterior's save load raises its cells' entry events
// is unread), for a reconnect with the game up once the login's events went
// (it spawns through MoveRefrToPosition and loads nothing, so it arms no
// arrival) or after a plugin reload (client-hot keeps the connection, so no
// login and no load follow it): a cell the engine loaded is never sent them.
// A reconnect before they went keeps the login's cell, its report and a cut
// walk's sets, and the walk goes on after the new connection's grant. The
// client setting loginCellEvents false turns this service off.
//
// When: after the cell's grant and ScriptStateService's apply pass for it
// (afterHosting: the gate open, the cell's records applied or given up), so
// the events land owned and after the records (a pillar whose record set
// doOnce does not jump again); with no grant within GRANT_WAIT_MS of the
// report (another seat hosts the cell, or none does), then, as its guest. A
// reference whose script record still waits for its apply (viewOf "pending":
// a record with a view holds its load events until then) is skipped, and
// after the OnCellLoad pass it is sent both events (unseenOnly) once its
// record leaves pending, for up to RECORDS_WAIT_MS: under H-L1 the gate held
// nothing, so a record given up or dropped would leave its script empty.
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
// had served one of them already (H-L1's read: 0). After it, "login cell <c>:
// <n> sent after their records, <k> given up" (a trace), and "login cell <c>:
// the engine's own load event came <ms> ms after its OnCellLoad pass".
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
// after the OnCellLoad pass, how long a reference skipped for its pending
// script record is waited for, ms
const RECORDS_WAIT_MS = 20_000;

// one login cell's walk
type Walk = {
    cell: number;
    how: string;            // how the events went: after its grant, or as its guest
    phase: "attach" | "load" | "records";  // every OnCellAttach, then every OnCellLoad, then the skipped references' records
    next: number;           // the next index: into the cell's references, then into `scripted`
    visited: Set<number>;   // each reference walked once, should the cell's list shift between updates
    scripted: number[];     // the references an OnCellAttach answer named scripted, in the cell's order
    sent: Set<number>;      // sent at least one of the two
    seen: Set<number>;      // the engine had served at least one of the two (unseenOnly skipped it)
    attachSeen: number;
    loadSeen: number;
    skipped: number[];      // left to a script record still waiting for its apply, revisited after the OnCellLoad pass
    recordsUntil: number;   // the end of that wait
    sentAfter: number;      // skipped references sent both events once their record left pending
    givenUp: number;        // skipped references gone before their record left pending
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
        // on unless the client setting loginCellEvents is false
        // (QuestStageService's storyArrivalAtLogin read)
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        this.enabled = !s || s["loginCellEvents"] !== false;
        if (!this.enabled) {
            logTrace(this, "loginCellEvents is off in settings: a login inside a cell is sent no entry events (D-L1 B)");
        }
        this.controller.emitter.on("connectionAccepted", () => this.onConnectionAccepted());
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
        this.controller.emitter.on("gameLoad", () => this.onGameLoad());
        this.controller.emitter.on("cellHostChanged", (e) => this.onCellHostChanged(e));
        this.controller.on("cellFullyLoaded", (e) => this.onCellFullyLoaded(e));
        this.controller.on("update", () => this.onUpdate());
    }

    // HostService's, from its once-a-second read: the player's own cell stood
    // attached with no load event for it (JS state alone; the update decides
    // whether it is the login's cell)
    noteWithoutLoadEvent(cell: number): void {
        if (!this.enabled) {
            return;
        }
        if (!this.done && !this.reported.has(cell)) {
            this.reported.set(cell, Date.now());
        }
    }

    // a new connection (NetworkingService's connectionAccepted, before the
    // server's CreateActor). Before the login's events went (its game loaded,
    // no verdict yet) it is a reconnect with the game up, which loads
    // nothing: the login's cell, its report (re-stamped, so the grant's wait
    // counts from now) and a cut walk's sets are kept and the walk goes on
    // after the new connection's grant; only the grants clear. Otherwise the
    // arrival starts over, so each login's is its own
    private onConnectionAccepted(): void {
        this.generation++;
        if (this.gameLoaded && !this.done) {
            if (this.walk) {
                logTrace(this, `login cell ${this.walk.cell.toString(16)}: a reconnect cut its walk (${this.walk.sent.size} reference(s) sent so far), kept for the new connection's grant`);
                this.cut = this.walk;
                this.walk = undefined;
            }
            const now = Date.now();
            this.reported.forEach((_at, cell) => this.reported.set(cell, now));
            this.granted.clear();
            this.gateOpen.clear();
            return;
        }
        this.walk = undefined;
        this.cut = undefined;
        this.loginSeen = false;
        this.gameLoaded = false;
        this.done = false;
        this.armedAt = 0;
        this.loginCell = 0;
        this.loginCellAt = 0;
        this.loadPassEndedAt = 0;
        this.lateLoadSaid = false;
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

    // the engine's own load event for the login cell (HostService drops one
    // that comes after its report): before the OnCellLoad pass ended, the
    // engine raised the events itself, so the walk ends there (or the arrival
    // settles, nothing sent); after it, said once with how late it came
    private onCellFullyLoaded(e: CellFullyLoadedEvent): void {
        if (this.loginCell === 0 || this.lateLoadSaid || !e.cell || e.cell.getFormID() !== this.loginCell) {
            return;
        }
        const hex = this.loginCell.toString(16);
        if (this.loadPassEndedAt !== 0) {
            this.lateLoadSaid = true;
            logLab(TAG, `login cell ${hex}: the engine's own load event came ${Date.now() - this.loadPassEndedAt} ms after its OnCellLoad pass`);
            return;
        }
        if (this.done) {
            return;
        }
        const w = this.walk ?? this.cut;
        if (w) {
            this.endWalk(w, "the engine's own load event came");
            return;
        }
        this.settle(`login cell ${hex}: the engine's own load event came; no entry events sent`);
    }

    // the arrival's verdict, then the walk, on the update (callNative needs
    // the VM's context; every wrapper is taken again each update, since one
    // lives one Papyrus update)
    private onUpdate(): void {
        if (!this.enabled) {
            return;
        }
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
            if (this.cut) {
                this.endWalk(this.cut, "the player left it before the walk's end");
            } else {
                this.settle(`login cell ${hex}: the player left it before its entry events went; none sent (the next cell's load is the engine's)`);
            }
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
        const cut = this.cut;
        this.cut = undefined;
        if (cut && cut.cell === this.loginCell) {
            // a reconnect's cut walk goes on where it stood, its sets kept
            cut.how = how;
            this.walk = cut;
        } else {
            this.walk = {
                cell: this.loginCell, how, phase: "attach", next: 0, visited: new Set<number>(), scripted: [],
                sent: new Set<number>(), seen: new Set<number>(), attachSeen: 0, loadSeen: 0, skipped: [],
                recordsUntil: 0, sentAfter: 0, givenUp: 0, refused: 0, firstRefusal: "", passed: 0, unreadable: 0, since: now,
            };
        }
        this.step(this.walk);
    }

    // a throw ends the walk with its line, never an update's error every frame
    private step(w: Walk): void {
        try {
            this.stepWalk(w);
        } catch (e) {
            if (this.walk === w) {
                const why = `the walk threw (${String(e).slice(0, 60)})`;
                if (w.phase === "records") {
                    this.finishRecords(w, why);
                } else {
                    this.endWalk(w, why);
                }
            }
        }
    }

    // one update's share of the walk: the OnCellAttach pass over the cell's
    // references, then the OnCellLoad pass over the scripted ones, the cell
    // and every reference taken again by id
    private stepWalk(w: Walk): void {
        if (w.phase === "records") {
            this.stepRecords(w);
            return;
        }
        const cell = this.sp.Cell.from(this.sp.Game.getFormEx(w.cell));
        if (!cell || !cell.isAttached()) {
            this.endWalk(w, `the cell detached after ${w.visited.size} reference(s) walked`);
            return;
        }
        let budget = REFS_PER_UPDATE;
        if (w.phase === "attach") {
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
                w.phase = "load";
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
            this.endLoadPass(w);
        }
    }

    // the OnCellLoad pass over: the login's line, then the skipped references'
    // records waited for, or the walk over
    private endLoadPass(w: Walk): void {
        this.loadPassEndedAt = Date.now();
        this.reportWalk(w, "");
        if (w.skipped.length === 0) {
            this.finishWalk();
            return;
        }
        w.phase = "records";
        w.recordsUntil = this.loadPassEndedAt + RECORDS_WAIT_MS;
    }

    // the references skipped for a pending record, each sent both events
    // (unseenOnly) once viewOf leaves "pending", up to RECORDS_WAIT_MS after
    // the OnCellLoad pass; the rest given up
    private stepRecords(w: Walk): void {
        const cell = this.sp.Cell.from(this.sp.Game.getFormEx(w.cell));
        if (!cell || !cell.isAttached()) {
            this.finishRecords(w, "the cell detached");
            return;
        }
        const scripts = this.controller.lookupListener(ScriptStateService);
        let budget = REFS_PER_UPDATE;
        const left: number[] = [];
        for (const id of w.skipped) {
            if (budget <= 0 || scripts.viewOf(id) === "pending") {
                left.push(id);
                continue;
            }
            budget--;
            const ref = this.sp.ObjectReference.from(this.sp.Game.getFormEx(id));
            if (!ref) {
                w.givenUp++;
                continue;
            }
            if (!this.send(w, ref, "OnCellAttach") || !this.send(w, ref, "OnCellLoad")) {
                return; // SendScriptEvent is not on this platform: said, the walk is over
            }
            w.sentAfter++;
        }
        w.skipped = left;
        if (w.skipped.length === 0 || Date.now() >= w.recordsUntil) {
            this.finishRecords(w, "");
        }
    }

    private finishRecords(w: Walk, why: string): void {
        logTrace(this, `login cell ${w.cell.toString(16)}: ${w.sentAfter} sent after their records, ${w.givenUp + w.skipped.length} given up${why !== "" ? ` (${why})` : ""}`);
        this.finishWalk();
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
            w.skipped.push(id);
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
                this.finishWalk();
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

    // a walk over before or without its records' wait: its line, then done
    private endWalk(w: Walk, why: string): void {
        this.reportWalk(w, why);
        this.finishWalk();
    }

    // the walk over for this login
    private finishWalk(): void {
        this.walk = undefined;
        this.cut = undefined;
        this.done = true;
    }

    // the login's one line, then the walk's coverage for a reader
    private reportWalk(w: Walk, why: string): void {
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
        if (w.skipped.length > 0) {
            notes.push(`${w.skipped.length} left to their script records`);
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
        this.cut = undefined;
        logTrace(this, why);
    }

    private readonly enabled: boolean;
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
    // a walk a reconnect cut, kept to go on after the new connection's grant
    private cut: Walk | undefined;
    // when the OnCellLoad pass ended (0 before), and whether a late load event
    // of the login cell was said
    private loadPassEndedAt = 0;
    private lateLoadSaid = false;
}
