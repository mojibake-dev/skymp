import { ScriptCall, ScriptCallsEvent, ScriptCensusEvent } from "skyrimPlatform";
import { NATIVE_ARG0_SUBJECTS, NATIVE_EVIDENCE, NATIVE_GRAPH_EVENT_RELAYS, NATIVE_HOOKS, NATIVE_RECORD_FAMILIES } from "../../lib/nativeRows.generated";
import { logError, logLab, logTrace } from "../../logging";
import { MsgType } from "../../messages";
import { ScriptCallMessage } from "../messages/scriptCallMessage";
import { SpSnippetArgument } from "../messages/spSnippetMessage";
import { localIdToRemoteId } from "../../view/worldViewMisc";
import { keepDeficit, keepSurplus } from "../../sync/inventory";
import { AliasFillService } from "./aliasFillService";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
import { ObjectAnimationService } from "./objectAnimationService";
import { OwnLookService } from "./ownLookService";
import { PropService } from "./propService";
import { holdPcInventoryBase } from "./remoteServer";
import { ScriptStateService } from "./scriptStateService";
import { TrapResultService } from "./trapResultService";
import { noteSpellAddedSinceLogin } from "../../sync/spell";
import { forwardMark, keepsForward } from "../../sync/scriptForward";
import { QuestStageService } from "./questStageService";

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
// thuum docs/verbs/npc-fate.md (playtest 48): a trap's hit on an actor this
// seat simulates crosses in the batch too (ProcessTrapHit, kind "state": its
// self the hit actor, its first argument the trap, ADR-068 D-S3 point 2), and
// this service sends it to the server as a ScriptCall, which the server keeps
// as that NPC's death evidence (wire-rules natives' Keep::Evidence, R2) and
// relays to nobody: so a trap's kill of a hosted NPC is taken however long
// after its trigger's notes (Bleak Falls Temple's blades swing on for
// minutes; 48 refused draugr 800a6's death 106 s after the box fired). One
// report per actor a second at most (the server's budget is the reporter's
// 8 a second over every call); the player's own hits are its own seat's and
// never sent. DeathReportService asks for the queued hits before its report,
// so the server reads a hit before the death it explains.
// thuum docs/DECISIONS.md ADR-068 amendment B (rung 4, the state rows;
// docs/research/rung4-state-rows-2026-11-07.md (d)): the platform batches the
// owned calls of the record rows too, kind "state" with their arguments:
// RefState's five (Enable, EnableNoWait, Disable, DisableNoWait and Delete, a
// reference's enabled and deleted state) and the standing's six
// (SetFactionRank, ModFactionRank, RemoveFromFaction, RemoveFromAllFactions,
// SetCrimeFaction and SetRelationshipRank, an NPC's). Each goes to the server
// as one ScriptCall through the record queue (noteRecord): coalesced per self
// and piece of state in the frame, the last call winning, sent on the update
// with the epoch HostService holds for the subject's cell, through a bucket
// mirroring the server's record budget, each object's calls (actions among
// them) in the engine's order. The server records them (wire-rules
// ref_state.rs and standing.rs) and says them back as DestructionStage 78
// (RefStateService) and as applier snippets with the object's seq
// (SpSnippetService). Neither is sent for the player's own actor. The
// batch's everyone rows ReferenceAlias.ForceRefTo and Clear are
// AliasFillService's. PlaceAtMe and PlaceActorAtMe (Record(Placement)) are
// never sent: W3, the script placement waits (the fake client alone, T2).
// thuum ADR-068 rung 5 (docs/research/rung5-quests-2026-11-09.md 8(b), Eli's
// "yes full transformation"; m4-scripts-5-platform b3f93eb7): the
// transformation's six (Actor.SetRace, AddSpell, RemoveSpell, AddShout,
// RemoveShout and EquipItem), batched kind "state" on this seat's own player
// alone (0x14), go the same way with selfId 0x14 (the wire's name for the
// sender's own actor), each call in the engine's order, never folded (a
// SetRace moved past the spells added after it would change which of them
// the server's overlay holds); the server judges them by the calling
// script's attachment and records them on the player's change form or in
// its race overlay (wire-rules player_own, D-T1 A). Eli's D-E1 A adds a
// seventh the same way, Actor.ChangeHeadPart (m4-scripts-5-server a403ddd4,
// m4-scripts-5-platform cbb59343): its part an HDPT the player's recorded
// race may wear, which the server records on the player's look.
// Eli's D-R1 C (thuum docs/DECISIONS.md, rung 5's stage-eve words): a
// script's ObjectReference.AddItem and RemoveItem on this seat's own player
// (0x14) are the player's own inventory, batched kind "state" with their
// arguments (the item as { formId }, the count, the silent flag and a
// removal's other container) and sent the same way, unfolded, which the
// server records as reported (R2, no bound): a quest's reward, A Lovely
// Letter's 25 gold among them. Each holds its base against remoteServer.ts's
// 5 s inventory apply from the call's arrival here until ownItemHoldMs past
// its send (holdOwnItem), so no apply undoes it before the server's
// SetInventory carries it into pcInv. Inside another player's acting scope
// the platform names that player's figure where the script named the
// player, a self that is never 0x14 and never batched (not this seat's), so
// a relayed fragment's grant is reported once, by the acting seat.
// thuum ADR-073 step 4's S6 (Eli's D-NT4 A; D-PO1 B, D-G6 A): a quest
// script's ObjectReference.AddToMap, an everyone row the platform batches on
// every seat that runs it (kind "everyone", the generated table's "marker"
// family), goes the same way through the record queue, one piece per marker
// in the frame: the server judges its cause (the calling script a quest's, a
// topic info's or a scene's whose quest it holds live) and its subject (a
// map marker by the master files), records it for every player the quest is
// shown to and relays AddToMap to the others (docs/verbs/map-markers.md).
// Every batched call keeps rung 3's read trigger (noteCall below).
type Outgoing = {
    call: ScriptCall;
    record: boolean;          // a record row's call, which spends the record bucket
    key: string | undefined;  // its piece of state (recordSlot) with its self; undefined for an action or a step
    dropped: boolean;         // a later call of the same piece superseded it before its send
};

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
            // the batch's readers by the generated table's hook and keep
            // columns (thuum ADR-073 step 4's review, SHOULD 6), never by a
            // native's spelling: a row the overlay gives a hook reaches its
            // reader with no change here
            const hook = ScriptCallService.hookOf(name);
            // thuum tracker #168b: a script's AddSpell on this seat's own
            // player is noted, so the login's first spell pass never removes
            // it (sync/spell.ts)
            if (call.self === 0x14 && name === "Actor.AddSpell") {
                const spell = call.args[0];
                if (spell !== null && typeof spell === "object") {
                    noteSpellAddedSinceLogin(spell.formId);
                }
            }
            if (hook === "death-report") {
                if (this.kills.size >= 64) {
                    this.kills.clear();
                }
                const killer = call.args[0];
                this.kills.set(call.self, { at: Date.now(), killer: killer !== null && typeof killer === "object" ? killer.formId : 0 });
            }
            if (hook === "trap-destroyed") {
                this.controller.lookupListener(TrapResultService).noteSetDestroyed(call.self, call.args[0] === true);
            }
            if (hook === "trap-motion" || hook === "trap-impulse") {
                this.controller.lookupListener(PropService).noteTrapMotion(call.self, call.fn, call.args);
            }
            if (NATIVE_EVIDENCE.includes(name)) {
                this.noteTrapHit(call);
            }
            // a state row's record, and since ADR-073 step 4's S6 an everyone
            // row's (AddToMap's quest marker), by the generated keep column
            if (call.kind === "state" || call.kind === "everyone") {
                const family = ScriptCallService.recordFamily(call);
                if (family !== undefined) {
                    this.noteRecord(call, family);
                }
            }
            if (call.kind === "everyone" && hook === "alias-fill") {
                this.controller.lookupListener(AliasFillService).noteFill(call);
            }
            if (call.kind === "action") {
                this.queue({ call, record: false, key: undefined, dropped: false });
                // the hooks never see an owned call's animation (the seam's
                // mark), so a pillar's side is noted from the call itself
                const event = ScriptCallService.graphEventOf(call);
                if (event !== undefined) {
                    this.controller.lookupListener(ObjectAnimationService).noteScriptSide(call.self, event);
                }
            }
            if (call.kind === "look") {
                this.noteLook(call);
            }
            // thuum ADR-073 decision 13, the spell channel (#54): a spell's
            // placement and every call on what it placed, sent as they ran
            // here, in the engine's order with this seat's other calls
            if (call.kind === "created") {
                this.queue({ call, record: false, key: undefined, dropped: false });
            }
            // a state call this seat's engine skipped on another seat's
            // subject, sent on for that owner to run
            if (call.kind === "forward") {
                const quests = this.controller.lookupListener(QuestStageService);
                if (keepsForward(call.rootQuest, (quest) => quests.relays(quest))) {
                    this.queue({ call, record: false, key: undefined, dropped: false });
                } else {
                    logLab("verb:scripts", `forward of ${call.cls}.${call.fn} on ${call.self.toString(16)} left to its owner: quest ${(call.rootQuest ?? 0).toString(16)}'s stages relay`);
                }
            }
        }
    }

    // thuum docs/DECISIONS.md D-W2 A (the trigger's own calls from its firing
    // seat; docs/research/fifty-fixes-2026-11-07.md 7.2(c)): the platform
    // batches kind "look" for a trap family trigger's own call on itself
    // while a firing its presser's owner started here is open (the presser
    // rule's note, LOOK_MS): its action rows, SetMotionType and
    // ApplyHavokImpulse, and Sound.Play (its self the Sound, its subject the
    // source in its first argument). Each ran here already, and is sent
    // exactly as an action (ScriptCall 75 through the queue, the epoch
    // HostService holds for the subject's cell) for the server to judge by
    // the firing's facts (the cell's host, or a game with the cell loaded and
    // an actor in reach) and play on the subject's other listeners, kept
    // nowhere. The motion rows reached PropService above by their hooks, whose
    // trigger rule makes no body and no claim of a trigger (the 48 rule,
    // unchanged); a trigger is no pillar, so no side is noted. JS state alone.
    // D-W2's detection row (the platform's P3): the trigger script's
    // ObjectReference.CreateDetectionEvent during its firing, kind "look"
    // with the trigger its self and akOwner its first argument (0x14 for this
    // seat's own player, which a look sends as 0x14: toSnippetArgument's
    // newCast is off for a look, and the server maps it to the reporter's
    // actor), goes the same way; the server relays it to the cell's host
    // alone, so a guest's bone alarm wakes the NPCs the host simulates
    private noteLook(call: ScriptCall) {
        this.queue({ call, record: false, key: undefined, dropped: false });
    }

    private onCensus(e: ScriptCensusEvent) {
        for (const { row, count } of e.rows) {
            this.counts.set(row, (this.counts.get(row) ?? 0) + count);
        }
    }

    private onUpdate() {
        if (this.outgoing.length > 0) {
            this.flush(Date.now());
        }
        this.sendTrapHits();
        this.summarize();
    }

    // a record row's call (JS state alone), queued with the piece of state it
    // sets: the last call of a piece wins and is queued after the calls
    // between them, so what is sent keeps the engine's order. A reference's
    // Enable, Disable and Delete are one piece, and a queued Delete stays (a
    // deleted reference never comes back: ref_state.rs's rule); a faction's
    // SetFactionRank and RemoveFromFaction are one, the crime faction one, a
    // relationship one per other actor, RemoveFromAllFactions one;
    // ModFactionRank is a step and every call of it goes. The player's own
    // actor sends neither: its standing waits for rung 5 (the server refuses
    // it, E_SCRIPTCALL_ROW), and its ref state is its seat's own (ADR-068
    // decision 6; a recorded disable of a player's actor would end its next
    // login, PartOne::SetUserActor)
    private noteRecord(call: ScriptCall, family: "ref" | "standing" | "own" | "marker") {
        if (family === "own") {
            // rung 5's transformation rows and D-R1 C's inventory rows: the
            // player's own alone, unfolded
            if (call.self === 0x14) {
                this.holdOwnItem(call, Date.now());
                this.queue({ call, record: true, key: undefined, dropped: false });
                // thuum D-53-8 B: after its own race change this game reports
                // the head parts its engine draws (OwnLookService, JS state
                // here)
                const race = call.args[0];
                if (call.cls === "Actor" && call.fn === "SetRace" && race !== null && typeof race === "object") {
                    this.controller.lookupListener(OwnLookService).noteOwnRace(race.formId);
                }
            }
            return;
        }
        if (call.self === 0x14) {
            this.recordsOwn++;
            return;
        }
        const slot = ScriptCallService.recordSlot(call, family);
        const key = slot === undefined ? undefined : `${call.self.toString(16)} ${slot}`;
        if (key !== undefined) {
            const queued = this.recordKeys.get(key);
            if (queued !== undefined) {
                this.recordsFolded++;
                if (family === "ref" && queued.call.fn === "Delete") {
                    return;
                }
                queued.dropped = true;
            }
        }
        const entry: Outgoing = { call, record: true, key, dropped: false };
        if (key !== undefined) {
            this.recordKeys.set(key, entry);
        }
        this.queue(entry);
    }

    // the outgoing queue, bounded: past its cap the oldest call goes, counted
    private queue(entry: Outgoing) {
        if (this.outgoing.length >= ScriptCallService.outgoingMax) {
            const oldest = this.outgoing.shift();
            if (oldest !== undefined && !oldest.dropped) {
                this.outgoingDropped++;
                if (oldest.key !== undefined && this.recordKeys.get(oldest.key) === oldest) {
                    this.recordKeys.delete(oldest.key);
                }
            }
        }
        this.outgoing.push(entry);
    }

    // the queued calls in order: an action as before, a record row's while the
    // record bucket holds a token (the server's own record budget, BURST_RECORD
    // 128 then 32 a second, the plan's (c), UNMEASURED), so a burst waits here
    // instead of being refused there and lost (E_SCRIPTCALL_RATE). A self whose
    // record waits holds its later calls back, so one object's calls reach the
    // server in the engine's order (its per-object seq); other selves go on
    private flush(now: number) {
        this.recordTokens = Math.min(ScriptCallService.recordBurst, this.recordTokens + ((now - this.recordRefilledAt) / 1000) * ScriptCallService.recordPerS);
        this.recordRefilledAt = now;
        const host = this.controller.lookupListener(HostService);
        const queued = this.outgoing;
        this.outgoing = [];
        const held = new Set<number>();
        for (const entry of queued) {
            if (entry.dropped) {
                continue;
            }
            if (held.has(entry.call.self)) {
                this.outgoing.push(entry);
                continue;
            }
            if (!entry.record) {
                this.send(entry.call, host);
                continue;
            }
            if (localIdToRemoteId(entry.call.self) === 0) {
                this.recordsUnsent++;
                this.forget(entry);
                continue;
            }
            if (this.recordTokens < 1) {
                held.add(entry.call.self);
                this.outgoing.push(entry);
                continue;
            }
            this.recordTokens -= 1;
            this.forget(entry);
            if (this.send(entry.call, host)) {
                this.recordsSent++;
                // D-R1 C: the hold runs on from the send, which the record
                // bucket may have held back
                this.holdOwnItem(entry.call, now);
            }
        }
    }

    // thuum docs/DECISIONS.md D-R1 C: an own AddItem or RemoveItem holds its
    // item's base against remoteServer.ts's inventory apply until
    // ownItemHoldMs past `now`, an add's surplus (keepSurplus) or a
    // removal's deficit (keepDeficit), so the apply never undoes the grant
    // before the server's SetInventory carries it. A leveled list's id names
    // no inventory entry, so since train 52 an AddItem that carries `landed`
    // (the platform's read of the roll) holds each landed base's surplus
    // instead (HYPOTHESIS H-RW1: an apply in the tick before the hold is
    // healed by the server's SetInventory echo). JS state alone
    private holdOwnItem(call: ScriptCall, now: number) {
        if (call.self !== 0x14 || call.cls !== "ObjectReference" || ScriptCallService.recordFamily(call) !== "own") {
            return;
        }
        const until = now + ScriptCallService.ownItemHoldMs;
        if (call.fn === "AddItem" && call.landed !== undefined) {
            for (const landing of call.landed) {
                holdPcInventoryBase(landing.formId, keepSurplus, until);
            }
            return;
        }
        const item = call.args[0];
        if (item === null || typeof item !== "object" || item.formId === 0) {
            return;
        }
        holdPcInventoryBase(item.formId, call.fn === "AddItem" ? keepSurplus : keepDeficit, until);
    }

    private forget(entry: Outgoing) {
        if (entry.key !== undefined && this.recordKeys.get(entry.key) === entry) {
            this.recordKeys.delete(entry.key);
        }
    }

    // the record rows (docs/NATIVES.md's keep column, Record(RefState) and
    // Record(Standing), and rung 5's seven of the player's own with D-R1 C's
    // two of its inventory), by the table's spelling: the generated table's
    // keep column (thuum ADR-073 step 4's S4, nativeRows.generated.ts), never
    // a list of names here
    private static recordFamily(call: ScriptCall): "ref" | "standing" | "own" | "marker" | undefined {
        const native = `${call.cls}.${call.fn}`;
        return Object.prototype.hasOwnProperty.call(NATIVE_RECORD_FAMILIES, native) ? NATIVE_RECORD_FAMILIES[native] : undefined;
    }

    // the piece of state a record row's call sets (noteRecord's key with its
    // self); undefined for ModFactionRank, a step that is never folded. A
    // quest marker's is the marker itself (S6: the frame's last call wins)
    private static recordSlot(call: ScriptCall, family: "ref" | "standing" | "own" | "marker"): string | undefined {
        if (family === "ref" || family === "marker") {
            return family;
        }
        const first = call.args[0];
        const form = first !== null && typeof first === "object" ? first.formId.toString(16) : "none";
        switch (call.fn) {
            case "SetFactionRank":
            case "RemoveFromFaction":
                return `rank ${form}`;
            case "RemoveFromAllFactions":
                return "factions";
            case "SetCrimeFaction":
                return "crime";
            case "SetRelationshipRank":
                return `relationship ${form}`;
            default:
                return undefined;
        }
    }

    // a trap's hit on an actor, queued for the next send (the event's context
    // allows no game call): never the player's own (its hit is its seat's,
    // its health its own report), one an actor a second at most, the queue
    // bounded
    private noteTrapHit(call: ScriptCall) {
        if (call.self === 0x14) {
            return;
        }
        const now = Date.now();
        const last = this.lastHitAt.get(call.self);
        if (last !== undefined && now - last < ScriptCallService.trapHitEveryMs) {
            this.hitsThrottled++;
            return;
        }
        if (this.lastHitAt.size >= 256) {
            this.lastHitAt.clear();
        }
        this.lastHitAt.set(call.self, now);
        if (this.hits.length >= 64) {
            this.hits.shift();
        }
        this.hits.push(call);
    }

    // the queued trap hits sent now, each a ScriptCall as the platform read
    // it, for an actor this seat still simulates (another's is no hit of this
    // seat's to report); at every update and, from DeathReportService, right
    // before a death report, so the server notes the hit ahead of the death
    // it explains (the platform raises the batch ahead of a death's event,
    // ScriptNatives::FlushAheadOnUpdate). A game-call context only
    sendTrapHits(): void {
        if (this.hits.length === 0) {
            return;
        }
        const hits = this.hits;
        this.hits = [];
        const host = this.controller.lookupListener(HostService);
        for (const call of hits) {
            if (!host.isMineLocal(call.self)) {
                this.hitsNotMine++;
                continue;
            }
            if (this.send(call, host)) {
                this.hitsSent++;
            }
        }
    }

    // one action row's call, reliable, as the platform read it: the subject
    // by its server id, the epoch HostService holds for the subject's cell
    // (the stamp ChangeForm carries; 0 when no hosted cell names it, which
    // the server refuses as E_SCRIPTCALL_OWNER), the arguments in SpSnippet's
    // shape. A subject with no server id (0, or a server copy the view does
    // not map) is not sent and is counted for the summary. Rung 5: inside an
    // acting scope the platform names the acting player's figure by its local
    // id where the script named the player (a relayed fragment's
    // ForceRefTo, SetRelationshipRank), so every form argument travels by its
    // server id (toSnippetArgument); a call with an argument the view cannot
    // map (a figure or a server copy gone from it) is not sent, counted,
    // rather than sent with None in its place. False when not sent
    private send(call: ScriptCall, host: HostService): boolean {
        if (call.kind === "created") {
            return this.sendCreated(call);
        }
        const selfId = call.self !== 0 ? localIdToRemoteId(call.self) : 0;
        if (selfId === 0) {
            this.unsent++;
            return false;
        }
        const args: SpSnippetArgument[] = [];
        for (const arg of call.args) {
            const packed = this.toSnippetArgument(arg, call.kind !== "look");
            if (packed === undefined) {
                this.argsUnmapped++;
                return false;
            }
            args.push(packed);
        }
        // the subject's cell names the epoch: the self, or Sound.Play's source
        // (a look row whose self is the Sound, D-W2)
        const subject = ScriptCallService.subjectOf(call);
        const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(subject));
        const message: ScriptCallMessage = {
            t: MsgType.ScriptCall,
            epoch: refr ? host.epochForActor(refr) : 0,
            selfId,
            cls: call.cls,
            fn: call.fn,
            args,
            caller: call.caller,
            handler: call.handler,
            latent: call.latent,
            ...forwardMark(call.kind),
        };
        // train 52 (D-R1 C's leveled gap): a leveled AddItem's roll as the
        // platform read it landing on this seat's own player, each base by its
        // id (a load-order form: the same on every seat and the server)
        if (call.landed !== undefined) {
            message.landed = call.landed.map((landing) => ({ item: landing.formId, count: landing.count }));
        }
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        if (call.kind === "forward") {
            logLab("verb:scripts", `forwarded ${call.cls}.${call.fn} on ${call.self.toString(16)} to its owner: this seat's engine skipped it`);
            return true;
        }
        if (call.kind === "look") {
            logLab("verb:traps", `trigger look ${(localIdToRemoteId(subject) || subject).toString(16)} ${call.cls}.${call.fn} reported (D-W2)`);
            return true;
        }
        // ObjectAnimationService's report line, byte for byte, for an
        // animation row on a world object (an actor's animation is the
        // stream's): its own report path stays for the engine-driven family
        const event = ScriptCallService.graphEventOf(call);
        if (event !== undefined && refr && !this.sp.Actor.from(refr)) {
            logTrace(this, `object animation ${call.self.toString(16)} ${event} reported`);
        }
        return true;
    }

    // thuum ADR-073 decision 13, the spell channel (#54; wire-schema
    // ScriptCall's placed and localArgs, SCHEMA_VERSION 55): a created
    // reference travels by this seat's own local id, as its self or as an
    // argument the platform marked, and the server maps it to its copy; this
    // seat's own player as 0x14, which the server maps to the reporter's
    // actor; every other form by its server id, as any call's. The epoch is
    // unread (the creator is the owner). False when an unmarked form has no
    // server id
    private sendCreated(call: ScriptCall): boolean {
        const marks = call.localArgs ?? 0;
        const local = (formId: number) => formId === 0x14 || formId >= 0xff000000;
        const selfId = call.self === 0 ? 0 : local(call.self) ? call.self : localIdToRemoteId(call.self);
        if (selfId === 0) {
            this.unsent++;
            return false;
        }
        const args: SpSnippetArgument[] = [];
        for (let i = 0; i < call.args.length; ++i) {
            const arg = call.args[i];
            if (i < 16 && (marks & (1 << i)) !== 0 && arg !== null && typeof arg === "object") {
                args.push({ formId: arg.formId, type: "ObjectReference" });
                continue;
            }
            const packed = this.toSnippetArgument(arg, false);
            if (packed === undefined) {
                this.argsUnmapped++;
                return false;
            }
            args.push(packed);
        }
        const message: ScriptCallMessage = {
            t: MsgType.ScriptCall,
            epoch: 0,
            selfId,
            cls: call.cls,
            fn: call.fn,
            args,
            caller: call.caller,
            handler: call.handler,
            latent: call.latent,
        };
        if (call.placed !== undefined) {
            message.placed = call.placed;
        }
        if (marks !== 0) {
            message.localArgs = marks;
        }
        this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
        return true;
    }

    // a form travels by its server id (the player's own actor as its figure's,
    // which the receiver's deserializeArg maps back) and the Papyrus type the
    // receiver casts it to: Actor, ObjectReference, Faction (the standing's
    // rows, rung 4), else Form (no other row passes another kind of form).
    // newCast off (a D-W2 look: m4-train-50's D-W2 review, note 2) keeps this
    // seat's own player as 0x14, which the server maps to the reporter's
    // actor: wire-validate refuses an object argument at 0xff000000 and up
    // (E_VAL_RANGE), so a look naming the player by its server id never
    // reached the server's look rule. A form the view cannot map to a server
    // id (a figure's or a server copy's local id, 0xff000000 and up, gone from
    // the view; this seat's own player before its login) answers undefined,
    // and the call is not sent
    private toSnippetArgument(arg: ScriptCall["args"][number], newCast: boolean): SpSnippetArgument | undefined {
        if (arg === null || typeof arg !== "object") {
            return arg;
        }
        const form = this.sp.Game.getFormEx(arg.formId);
        // thuum #54 (x-raise-wall-v6 20261115-134951): a form with no Papyrus
        // type (a moving static, Raise Wall's MSTT 0x98529) makes a class's
        // from() throw "Expected 'objectTypeInfo' to not to be nullptr", which
        // took the whole call down unsent: each cast is asked in turn, and a
        // form none of them takes is a Form
        const casts = (cls: { from(f: unknown): unknown }): boolean => {
            try {
                return !!form && !!cls.from(form);
            } catch (e) {
                return false;
            }
        };
        const type = casts(this.sp.Actor) ? "Actor" : casts(this.sp.ObjectReference) ? "ObjectReference"
            : casts(this.sp.Faction) ? "Faction" : "Form";
        const formId = localIdToRemoteId(arg.formId, newCast);
        if (formId === 0 && arg.formId !== 0) {
            return undefined;
        }
        return { formId, type };
    }

    // a call's subject: its self, but a method's first argument where its
    // row's subject is that argument (the generated table's subject column,
    // NATIVE_ARG0_SUBJECTS: Sound.Play's source, D-W2's look row whose self is
    // the Sound)
    private static subjectOf(call: ScriptCall): number {
        if (NATIVE_ARG0_SUBJECTS.includes(`${call.cls}.${call.fn}`)) {
            const first = call.args[0];
            return first !== null && typeof first === "object" ? first.formId : 0;
        }
        return call.self;
    }

    // a row's hook code (the generated table's hook column, wire-rules
    // natives.rs Hook::name), undefined for a row with none
    private static hookOf(native: string): string | undefined {
        return Object.prototype.hasOwnProperty.call(NATIVE_HOOKS, native) ? NATIVE_HOOKS[native] : undefined;
    }

    // the call's behavior graph event when its row's relay sends one on its
    // own self (the generated table's NATIVE_GRAPH_EVENT_RELAYS, the event's
    // argument by its row's first string parameter: PlayAnimationAndWait is
    // relayed as its twin PlayAnimation); undefined otherwise
    private static graphEventOf(call: ScriptCall): string | undefined {
        const native = `${call.cls}.${call.fn}`;
        if (!Object.prototype.hasOwnProperty.call(NATIVE_GRAPH_EVENT_RELAYS, native)) {
            return undefined;
        }
        const event = call.args[NATIVE_GRAPH_EVENT_RELAYS[native]];
        return typeof event === "string" ? event : undefined;
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
    // suffix is a call on a subject this seat owns, from the frames' batches,
    // or an alias's fill or clear the batch carries since rung 4
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
        if (this.argsUnmapped > 0) {
            logTrace(this, `${this.argsUnmapped} call(s) in ${ScriptCallService.summaryMs / 1000} s not sent: a form argument with no server id (a figure or a server copy gone from the view)`);
            this.argsUnmapped = 0;
        }
        // short of printConsole's 128-byte cut: throttled is within a second
        // of the actor's last report, not this seat's an actor it no longer
        // simulates
        if (this.hitsSent + this.hitsThrottled + this.hitsNotMine > 0) {
            logLab("verb:npc-fate", `trap hits in ${ScriptCallService.summaryMs / 1000} s: ${this.hitsSent} sent as evidence, ${this.hitsThrottled} throttled, ${this.hitsNotMine} not this seat's`);
            this.hitsSent = 0;
            this.hitsThrottled = 0;
            this.hitsNotMine = 0;
        }
        // the record queue's window (rung 4): sent, folded into a later call of
        // the same piece, with no server id, the player's own; still waiting
        const waiting = this.outgoing.filter((o) => o.record && !o.dropped).length;
        if (this.recordsSent + this.recordsFolded + this.recordsUnsent + this.recordsOwn + this.outgoingDropped > 0 || waiting > 0) {
            logLab("verb:scripts", `record rows in ${ScriptCallService.summaryMs / 1000} s: ${this.recordsSent} sent, ${this.recordsFolded} folded, ${this.recordsUnsent} without a server id, ${this.recordsOwn} the player's own; ${waiting} waiting`);
            if (this.outgoingDropped > 0) {
                logError(this, `${this.outgoingDropped} queued call(s) dropped past ${ScriptCallService.outgoingMax} in ${ScriptCallService.summaryMs / 1000} s`);
            }
            this.recordsSent = 0;
            this.recordsFolded = 0;
            this.recordsUnsent = 0;
            this.recordsOwn = 0;
            this.outgoingDropped = 0;
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

    // the calls to send in the engine's order: actions and record rows
    private outgoing: Outgoing[] = [];
    // the queued record row for each piece of state (noteRecord's key)
    private readonly recordKeys = new Map<string, Outgoing>();
    private recordTokens = ScriptCallService.recordBurst;
    private recordRefilledAt = Date.now();
    private recordsSent = 0;
    private recordsFolded = 0;
    private recordsUnsent = 0;
    private recordsOwn = 0;
    private outgoingDropped = 0;
    private unsent = 0;
    // rung 5: calls not sent for a form argument with no server id
    private argsUnmapped = 0;
    private hits: ScriptCall[] = [];
    private readonly lastHitAt = new Map<number, number>();
    private hitsSent = 0;
    private hitsThrottled = 0;
    private hitsNotMine = 0;
    private readonly counts = new Map<string, number>();
    private readonly logged = new Map<string, number>();
    private readonly kills = new Map<number, { at: number; killer: number }>();
    private lastSummary = Date.now();
    private static readonly summaryMs = 30_000;
    private static readonly killWindowMs = 10_000;
    // UNMEASURED: well inside the server's trap window (30 s), so the death's
    // own hit, when throttled, leaves a note at most a second older
    private static readonly trapHitEveryMs = 1_000;
    private static readonly logPerWindow = 5;
    private static readonly recordBurst = 128;
    private static readonly recordPerS = 32;
    // UNMEASURED: four thousand calls are over two minutes of the record budget
    private static readonly outgoingMax = 4096;
    // UNMEASURED: two of remoteServer.ts's 5 s apply periods, far past a
    // report's round trip to the server's SetInventory
    private static readonly ownItemHoldMs = 10_000;
}
