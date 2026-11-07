import { ObjectReference } from "skyrimPlatform";
import { logError, logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { DestructionStageMessage } from "../messages/destructionStageMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
import { SpSnippetService } from "./spSnippetService";

// thuum docs/DECISIONS.md ADR-068 amendment B (rung 4, the state rows;
// docs/research/rung4-state-rows-2026-11-07.md (d), and read 1 of "Reads that
// change the approved design"): a reference's engine state, its enabled and
// deleted flags, follows its owner's engine. The owner (a world object's cell
// host, an actor's simulator) reports its scripts' Enable, Disable and Delete
// as batched state rows (ScriptCallService's record queue); the server records
// them on the reference (ref_state.rs: deleted never clears, a repeat folded)
// and says them in DestructionStage 78, the one S2C word for a reference's
// state: relayed to the reference's other listeners with the object's seq
// (served false), and served behind its create word, a late joiner's and
// after a restart (served true). R2: the owner computes, the server records,
// this seat applies the record.
//
// The word is held per form in the message's context (JS state alone, the
// newest replacing a held one; deleted stays) and applied on the update once
// ObjectReference.from(Game.getFormEx(id)) answers. A live word applies only
// above SpSnippetService's seq for the form (the per-object sequence the
// owner's ScriptCall relays share, so an action and a state of one object
// land in the owner's order) and raises it; a served word always applies and
// leaves the seq alone: the server serves it right before the reference's
// ScriptStateApply, whose seq can be older (an unchanged script is not read
// again after its Disable), and a seq raised past it would have
// ScriptStateService drop that record as stale on a late joiner.
// Nothing is applied for a reference standing in a cell this seat hosts (its
// engine holds the state: its own scripts made it), except inside the grant's
// beforeHosting pass (scriptStateService.ts): HostService marks the cell at
// the grant, the words for it wait, and the grant's gate applies them after
// the cell's script records and before the platform's hosted set opens, so
// the new host's engine starts from the record. The apply compares
// isDisabled() and calls disableNoWait(false) or enableNoWait(false), the
// platform's own calls, which the seam tallies "(platform)" and never batches,
// so nothing applied here is reported back. A deleted game-file reference is
// disabled: the engine keeps a game file's placement, the record keeps it gone.
// A word that waits HOLD_MS for its reference is let go: the next subscribe
// serves the record again.
//
// Lines for the lab (verb:scripts): "ref state <id> applied (served)" or
// "(relayed seq <n>)", "ref state <id> here already (...)", "ref state <id>
// dropped: seq <n> at or below <m>"; the hosted cell's and the timeout's
// "ref state <id> not applied: ..." are traces.
const TAG = "verb:scripts";
// game-file references end where the server's copies begin (the validator
// refuses a refr at 0xff000000 and up)
const LAST_GAME_FILE_ID = 0xfeffffff;
// a word waits this long for its reference (a create word's onceLoad, a
// slow landing's cell load: RemoteServer's PENDING_LOAD_MS), UNMEASURED
const HOLD_MS = 120_000;
const WORDS_MAX = 4096;
// a grant's mark holds its cell's words this long at most: the gate opens
// within the pass's 3 s (scriptStateService.ts GATE_TIMEOUT_MS), and a gate
// that never opens (its grant replaced or gone) must not hold them for good
// (UNMEASURED)
const GRANT_HOLD_MS = 10_000;
// the references looked up per update, so a cell's serve spreads over frames
// (UNMEASURED), and a word waiting for its reference or its grant's gate
// looked at again this often (scriptStateService.ts's APPLY_RETRY_MS)
const LOOKUPS_PER_UPDATE = 64;
const RETRY_MS = 250;

type Word = {
    disabled: boolean | undefined; // the record's disabled flag; undefined when the word named none
    deleted: boolean;
    seq: number;
    served: boolean;
    since: number;                 // the first word's arrival, for HOLD_MS
    nextTryAt: number;             // its next look, 0 for the next update
};

export class RefStateService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("destructionStageMessage", (e) => this.onWord(e));
        this.controller.emitter.on("connectionAccepted", () => this.onConnectionAccepted());
        this.controller.on("update", () => this.onUpdate());
    }

    // HostService's, at a grant (a message handler: JS state alone): the
    // words for references in the cell wait for the grant's gate instead of
    // being dropped as a hosted cell's
    holdForGrant(cell: number): void {
        if (cell !== 0) {
            this.grants.set(cell, Date.now() + GRANT_HOLD_MS);
        }
    }

    // HostService's, from the grant's gate (HostService.openGate), which
    // ScriptStateService's beforeHosting pass calls in an update's context
    // after the cell's script records: every held word whose reference
    // stands in the cell is applied as the new host's, before the platform's
    // hosted set and the activation lift. A reference not found yet keeps its
    // word, judged as a hosted cell's once it is
    applyAtGrant(cell: number): void {
        this.grants.delete(cell);
        if (this.words.size === 0) {
            return;
        }
        const cellHex = cell.toString(16);
        let applied = 0;
        for (const [id, w] of Array.from(this.words.entries())) {
            try {
                const refr = ObjectReference.from(this.sp.Game.getFormEx(id));
                const parent = refr ? refr.getParentCell() : null;
                if (!refr || !parent || parent.getFormID() !== cell) {
                    continue;
                }
                this.words.delete(id);
                this.apply(id, refr, w, ` at the grant of ${cellHex}`);
                applied++;
            } catch (e) {
                this.words.delete(id);
                logError(this, `ref state ${id.toString(16)} at the grant of ${cellHex} threw, let go`, e);
            }
        }
        if (applied > 0) {
            logTrace(this, `cell ${cellHex}: ${applied} ref state word(s) applied in the grant's pass, before its gate`);
        }
    }

    // a DestructionStage word (a message handler: JS state alone, the apply
    // waits for the update). Rule 5: the reference must be a game-file one;
    // its form and its cell are read at the apply
    private onWord(e: ConnectionMessage<DestructionStageMessage>): void {
        const m = e.message;
        const hasDisabled = typeof m.disabled === "boolean";
        const hasDeleted = typeof m.deleted === "boolean";
        if (!hasDisabled && !hasDeleted) {
            // a destruction step alone: TrapResultService's
            return;
        }
        const hex = typeof m.refr === "number" ? m.refr.toString(16) : String(m.refr);
        if (typeof m.refr !== "number" || !Number.isInteger(m.refr) || m.refr <= 0 || m.refr > LAST_GAME_FILE_ID) {
            logError(this, `ref state ${hex} from the server: not a game-file reference's id, not held`);
            return;
        }
        const seq = typeof m.seq === "number" && Number.isInteger(m.seq) && m.seq >= 0 ? m.seq : 0;
        if (!m.served) {
            const snippets = this.controller.lookupListener(SpSnippetService);
            const last = snippets.seqOf(m.refr);
            if (last !== undefined && seq <= last) {
                logLab(TAG, `ref state ${hex} dropped: seq ${seq} at or below ${last}`);
                return;
            }
            snippets.raiseSeq(m.refr, seq);
        }
        const held = this.words.get(m.refr);
        if (!held && this.words.size >= WORDS_MAX) {
            logTrace(this, `${this.words.size} ref state words held; the table starts over`);
            this.words.clear();
        }
        this.words.set(m.refr, {
            disabled: hasDisabled ? m.disabled : held ? held.disabled : undefined,
            deleted: m.deleted === true || (held !== undefined && held.deleted),
            seq,
            served: m.served === true,
            since: held ? held.since : Date.now(),
            nextTryAt: 0,
        });
    }

    // a new connection (NetworkingService's connectionAccepted, ahead of all
    // its messages): the server serves its records anew
    private onConnectionAccepted(): void {
        this.words.clear();
        this.grants.clear();
    }

    private onUpdate(): void {
        if (this.words.size === 0) {
            return;
        }
        const now = Date.now();
        const host = this.controller.lookupListener(HostService);
        // a grant released before its gate opened, or a gate that never
        // opened, leaves no mark
        this.grants.forEach((until, cell) => {
            if (!host.hostsCell(cell) || now >= until) {
                this.grants.delete(cell);
            }
        });
        let lookups = 0;
        for (const [id, w] of Array.from(this.words.entries())) {
            if (lookups >= LOOKUPS_PER_UPDATE) {
                return;
            }
            if (now < w.nextTryAt) {
                continue;
            }
            lookups++;
            const hex = id.toString(16);
            try {
                const refr = ObjectReference.from(this.sp.Game.getFormEx(id));
                if (!refr) {
                    this.words.delete(id);
                    if (now - w.since >= HOLD_MS) {
                        logTrace(this, `ref state ${hex} not applied: no reference here in ${now - w.since} ms (the next subscribe serves it again)`);
                    } else {
                        // to the back of the scan, so the words behind it are
                        // looked at too
                        w.nextTryAt = now + RETRY_MS;
                        this.words.set(id, w);
                    }
                    continue;
                }
                const parent = refr.getParentCell();
                const cell = parent ? parent.getFormID() : 0;
                if (cell !== 0 && host.hostsCell(cell)) {
                    if (this.grants.has(cell)) {
                        // the grant's gate applies it (applyAtGrant)
                        w.nextTryAt = now + RETRY_MS;
                        this.words.delete(id);
                        this.words.set(id, w);
                        continue;
                    }
                    this.words.delete(id);
                    logTrace(this, `ref state ${hex} not applied: cell ${cell.toString(16)} is hosted here, its engine holds the state`);
                    continue;
                }
                this.words.delete(id);
                this.apply(id, refr, w, "");
            } catch (e) {
                this.words.delete(id);
                logError(this, `ref state ${hex}: its apply threw, let go`, e);
            }
        }
    }

    // the record's state on this engine's reference, an update's context
    private apply(id: number, refr: ObjectReference, w: Word, where: string): void {
        const hex = id.toString(16);
        const how = w.served ? "served" : `relayed seq ${w.seq}`;
        // a deleted game-file reference is disabled
        const disabled = w.deleted ? true : w.disabled;
        if (disabled === undefined) {
            // deleted false and no disabled flag: nothing to set
            return;
        }
        if (refr.isDisabled() === disabled) {
            logLab(TAG, `ref state ${hex} here already (${how})${where}`);
            return;
        }
        if (disabled) {
            refr.disableNoWait(false);
        } else {
            refr.enableNoWait(false);
        }
        logLab(TAG, `ref state ${hex} applied (${how})${where}: ${disabled ? "disabled" : "enabled"}${w.deleted ? ", deleted" : ""}`);
    }

    // the words held by the reference's form id (a game-file id: the server's
    // and this engine's agree)
    private readonly words = new Map<number, Word>();
    // the cells whose grant's gate has not opened yet, until when (Date.now())
    private readonly grants = new Map<number, number>();
}
