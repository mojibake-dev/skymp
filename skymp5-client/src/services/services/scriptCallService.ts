import { ScriptCall, ScriptCallsEvent, ScriptCensusEvent } from "skyrimPlatform";
import { logLab, logTrace } from "../../logging";
import { MsgType } from "../../messages";
import { ScriptCallMessage } from "../messages/scriptCallMessage";
import { SpSnippetArgument } from "../messages/spSnippetMessage";
import { localIdToRemoteId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
import { ObjectAnimationService } from "./objectAnimationService";
import { PropService } from "./propService";
import { ScriptStateService } from "./scriptStateService";
import { TrapResultService } from "./trapResultService";

// thuum docs/DECISIONS.md ADR-068 (rung 2, the one seam; docs/research/
// scripts-one-system-2026-11-05.md 6.2 to 6.4): the platform's one hook on the
// VM's native dispatch (NativeFunctionBase::Call, a replacement keyed by
// docs/NATIVES.md's table; ScriptNatives.cpp) sees every tabled native a
// script calls on this seat. A call on a subject this seat does not own is
// filtered there and tallied "(not hosted)", the platform's own calls (a
// callNative, a snippet) "(platform)"; a call on a subject this seat owns
// crosses into JS in the frame's batch, scriptCalls, and the tallies every
// 30 s as scriptCensus. An ACTION row's call (an animation event, an
// animation variable: what the engine never saves) goes to the server as one
// ScriptCall, which it checks by its shape and mirrors to the subject's other
// listeners as SpSnippet with the object's seq (SpSnippetService applies them
// in seq order). A STATE or EVERYONE row is counted only at this rung (rungs
// 4 and 5 give those rows their records), but for a script's Kill, whose mark
// DeathReportService reads as cause 3. The census and the first calls of a row
// per window are logged in ScriptNativeService's text (the flip's), so the
// probes' claims on them read on, and the host's report of a world object's
// animation keeps ObjectAnimationService's line, "object animation <refr>
// <event> reported" (b-puzzle-relay's and b-object-anim's claim).
// ADR-068 D-S3 (traps follow the interaction): on a trap family reference the
// platform runs the state rows on every seat and batches some of them here
// (kind "state"), which this service, the batch's one reader, hands to their
// readers: SetDestroyed to TrapResultService (the single-use result), and
// SetMotionType and ApplyHavokImpulse to PropService (the trap body's claim);
// a body another seat streams runs its two motion calls neutralized and never
// reaches the batch.
export class ScriptCallService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("scriptCalls", (e) => this.onCalls(e));
        this.controller.on("scriptCensus", (e) => this.onCensus(e));
        this.controller.on("update", () => this.onUpdate());
    }

    // a script's Kill on `localId` within the window, with the killer it
    // named (a form id, 0 for none); undefined otherwise. The mark is
    // consumed: one death report per kill
    scriptKill(localId: number): { at: number; killer: number } | undefined {
        const mark = this.kills.get(localId);
        if (!mark) {
            return undefined;
        }
        this.kills.delete(localId);
        if (Date.now() - mark.at > ScriptCallService.killWindowMs) {
            return undefined;
        }
        return mark;
    }

    // the event's context allows no game call: the counts, the lines and the
    // kill marks are JS state (a deathStart may come before the next update,
    // so a mark is set here, as the flip's was); a send needs the subject's
    // cell and waits for the update
    private onCalls(e: ScriptCallsEvent) {
        const scripts = this.controller.lookupListener(ScriptStateService);
        for (const call of e.calls) {
            // ADR-068 rung 3 (the script record): every call in the batch makes
            // its self and its caller's self due for a record read
            // (ScriptStateService, JS state alone); a "touch" is an owned
            // self's state row the platform batches for that read alone (no
            // arguments, once a frame per self), never counted or sent here
            scripts.noteCall(call.self, call.callerSelf ?? 0);
            if (call.kind === "touch") {
                continue;
            }
            const name = `${call.cls}.${call.fn}`;
            this.counts.set(name, (this.counts.get(name) ?? 0) + 1);
            const logged = this.logged.get(name) ?? 0;
            if (logged < ScriptCallService.logPerWindow) {
                this.logged.set(name, logged + 1);
                // the head is the flip's ("native <Class.Name> on <id>"); the
                // arguments before the caller, so a console cut near 100
                // characters keeps them
                const args = call.args.map(ScriptCallService.describe).join(", ");
                const from = call.caller || call.handler ? ` from ${call.caller}.${call.handler}` : "";
                logLab("verb:scripts", `native ${name} on ${call.self.toString(16)} ${call.kind}${call.latent ? " latent" : ""}: ${args}${from}`);
            }
            if (call.cls === "Actor" && (call.fn === "Kill" || call.fn === "KillSilent")) {
                if (this.kills.size >= 64) {
                    this.kills.clear();
                }
                const killer = call.args[0];
                this.kills.set(call.self, { at: Date.now(), killer: killer !== null && typeof killer === "object" ? killer.formId : 0 });
            }
            if (call.cls === "ObjectReference" && call.fn === "SetDestroyed") {
                this.controller.lookupListener(TrapResultService).noteSetDestroyed(call.self, call.args[0] === true);
            }
            if (call.cls === "ObjectReference" && (call.fn === "SetMotionType" || call.fn === "ApplyHavokImpulse")) {
                this.controller.lookupListener(PropService).noteTrapMotion(call.self, call.fn, call.args);
            }
            if (call.kind === "action") {
                this.pending.push(call);
                // the hooks never see an owned call's animation (the seam's
                // mark), so a pillar's side is noted from the call itself
                const event = call.args[0];
                if (ScriptCallService.isAnimationRow(call) && typeof event === "string") {
                    this.controller.lookupListener(ObjectAnimationService).noteScriptSide(call.self, event);
                }
            }
        }
    }

    private onCensus(e: ScriptCensusEvent) {
        for (const { row, count } of e.rows) {
            this.counts.set(row, (this.counts.get(row) ?? 0) + count);
        }
    }

    private onUpdate() {
        if (this.pending.length > 0) {
            const pending = this.pending;
            this.pending = [];
            const host = this.controller.lookupListener(HostService);
            for (const call of pending) {
                this.send(call, host);
            }
        }
        this.summarize();
    }

    // one action row's call, reliable, as the platform read it: the subject
    // by its server id, the epoch HostService holds for the subject's cell
    // (the stamp ChangeForm carries; 0 when no hosted cell names it, which
    // the server refuses as E_SCRIPTCALL_OWNER), the arguments in SpSnippet's
    // shape. A subject with no server id (0, or a server copy the view does
    // not map) is not sent and is counted for the summary
    private send(call: ScriptCall, host: HostService) {
        const selfId = call.self !== 0 ? localIdToRemoteId(call.self) : 0;
        if (selfId === 0) {
            this.unsent++;
            return;
        }
        const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(call.self));
        const message: ScriptCallMessage = {
            t: MsgType.ScriptCall,
            epoch: refr ? host.epochForActor(refr) : 0,
            selfId,
            cls: call.cls,
            fn: call.fn,
            args: call.args.map((arg) => this.toSnippetArgument(arg)),
            caller: call.caller,
            handler: call.handler,
            latent: call.latent,
        };
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        // ObjectAnimationService's report line, byte for byte, for an
        // animation row on a world object (an actor's animation is the
        // stream's): its own report path stays for the engine-driven family
        const event = call.args[0];
        if (ScriptCallService.isAnimationRow(call) && typeof event === "string" && refr && !this.sp.Actor.from(refr)) {
            logTrace(this, `object animation ${call.self.toString(16)} ${event} reported`);
        }
    }

    // a form travels by its server id (the player's own actor as its figure's,
    // which the receiver's deserializeArg maps back) and the Papyrus type the
    // receiver casts it to: Actor, ObjectReference, else Form (no rung 2 row
    // passes another kind of form)
    private toSnippetArgument(arg: ScriptCall["args"][number]): SpSnippetArgument {
        if (arg === null || typeof arg !== "object") {
            return arg;
        }
        const form = this.sp.Game.getFormEx(arg.formId);
        const type = form && this.sp.Actor.from(form) ? "Actor" : form && this.sp.ObjectReference.from(form) ? "ObjectReference" : "Form";
        return { formId: localIdToRemoteId(arg.formId, true), type };
    }

    // the table's animation rows (docs/NATIVES.md): PlayAnimationAndWait is
    // relayed as its twin PlayAnimation, the event its first argument
    private static isAnimationRow(call: ScriptCall): boolean {
        return call.cls === "ObjectReference" && (call.fn === "PlayAnimation" || call.fn === "PlayAnimationAndWait");
    }

    // an argument as a census-side line shows it: a form by its id in hex,
    // null as None
    private static describe(arg: ScriptCall["args"][number]): string {
        if (arg === null) {
            return "None";
        }
        return typeof arg === "object" ? arg.formId.toString(16) : String(arg);
    }

    // the census, once a window: how many calls of each native this seat's
    // scripts made, the filtered ones by their suffix (the lab reads it). A
    // key is the native's name ("Quest.Start") plus the suffix the platform
    // tallied it under (scriptCensus): " (platform)" for the platform's own
    // call, " (not hosted)" for a call on a subject this seat does not own,
    // " on <id>" for a quest's, global's or alias's call with its self (up to
    // the platform's cap of selves a row a window, then " on others"); no
    // suffix is a call on a subject this seat owns, from the frames' batches
    private summarize() {
        const now = Date.now();
        if (now - this.lastSummary < ScriptCallService.summaryMs) {
            return;
        }
        this.lastSummary = now;
        this.logged.clear();
        if (this.unsent > 0) {
            logTrace(this, `${this.unsent} action call(s) in ${ScriptCallService.summaryMs / 1000} s not sent: no server id for the subject`);
            this.unsent = 0;
        }
        if (this.counts.size === 0) {
            return;
        }
        // one row per line: printConsole cuts a line at 128 bytes
        // (InGameConsolePrinter.cpp:26-30), so x-script-natives v3 (run
        // 20261103-062800) lost every summary row past "ObjectReference.Se...",
        // the relayed stage's "(platform)" key among them; the longest census
        // line is about 85 characters, the total line 83
        const names = Array.from(this.counts.keys()).sort();
        let total = 0;
        for (const name of names) {
            const n = this.counts.get(name) ?? 0;
            total += n;
            logLab("verb:scripts", `census ${name} ${n} in ${ScriptCallService.summaryMs / 1000} s`);
        }
        logLab("verb:scripts", `${total} native call(s) in ${ScriptCallService.summaryMs / 1000} s: ${names.length} row(s), one census line each`);
        this.counts.clear();
        logTrace(this, `${this.kills.size} script kill mark(s) held`);
    }

    private pending: ScriptCall[] = [];
    private unsent = 0;
    private readonly counts = new Map<string, number>();
    private readonly logged = new Map<string, number>();
    private readonly kills = new Map<number, { at: number; killer: number }>();
    private lastSummary = Date.now();
    private static readonly summaryMs = 30_000;
    private static readonly killWindowMs = 10_000;
    private static readonly logPerWindow = 5;
}
