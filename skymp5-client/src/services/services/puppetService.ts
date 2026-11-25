import { Actor, Art, CellAttachDetachEvent, CombatEvent, DeathEvent, EffectShader, ObjectReference } from "skyrimPlatform";
import { logError, logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { HostedActorStateMessage } from "../messages/hostedActorStateMessage";
import { SpSnippetMessage } from "../messages/spSnippetMessage";
import { Movement } from "../../sync/movement";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { localIdToRemoteId, remoteIdToLocalId, snippetIdToLocalId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
import { PropService } from "./propService";
import { placesAtPromote, runsActorHere } from "../../sync/hostPuppet";
import { SEAT_WAIT_MS, seatVerdict, seated } from "../../sync/seatWait";
import { RemoteServer } from "./remoteServer";
import { SceneClaimService } from "./sceneClaimService";

// thuum docs/verbs/npc-puppet.md (ADR-045): every NPC this game does not host
// is a puppet, the engine's own placed reference (or a server-made copy) with
// its AI off, moved and animated from the host's relay; one it hosts is live
// under the engine. The switch is the fork's Skyrim Platform export
// setPuppet: Q4's demote and promote in one native call on the game thread,
// and the flag the two detours read (Puppets::Is, the one truth about an
// actor's brain on this seat).
//
// ADR-050 decision 3 (train 51): the state is computed in ONE place,
// reconcile(), from (the server's word, loaded here, dead, kind) and applied
// idempotently from every event that could change an input (the view's
// update, a cell attach or detach, the loss of a form, a resurrect, the
// combat sink) and from one two-second poll. "Loaded here" is the cell's
// attach or the 3D in a cell the engine has attached (ADR-051: an actor this
// engine does not load is released to its schedule after a grace, so a 3D
// the late fate dropped for a moment does not flip a loaded actor live,
// playtest 34's composition; tracker #121: a 3D kept in the buffer of a
// detached cell is no load). The
// deferred kill is this service's too: every NPC this game shows holds at
// zero until the server's word (train 52 moves the host's own to the engine).
//
// A dead body is no puppet on any seat (playtest 37, findings 58 and 62): a
// flagged one is released once its ragdoll has settled. Its loot is the
// server's container, and since ADR-067 decision 6 (the exclusive container
// hold; docs/private/container-hold-scope.md D-C2 A) a body is
// activation-blocked like a CONT on every seat: E sends Activate, the server
// admits one looter and answers OpenContainer, and the ContainerMenu opens
// through remoteServer's handler alone; a refused looter sees the server's
// notification and no menu. The block is this service's (blockDeadBody): the
// state function below asserts it on every dead body loaded here, and
// re-asserts it on a later tick after each native lift (the promote, the
// release, the fate's resurrect), since those land on the game thread after
// the JavaScript tick (skyrim-platform main.cpp OnUpdate: JsTick, then
// gameThrQ->Update) and would undo a block issued in the lift's own tick.

// thuum docs/verbs/npc-puppet.md (Ownership; ADR-063, ADR-073 decision 1):
// the looks this seat plays on an actor another seat hosts: the record's
// effect set they came from (the very array, so a new record is a new
// reference) and the hit shaders and hit effect arts playing, by form id
type EffectLooks = { from: number[]; shaders: Set<number>; arts: Set<number> };

export class PuppetService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.enabled = this.sp.settings["skymp5-client"]["npcPuppet"] !== false;
        // ADR-050 decision 2: with the server's number arriving as a cause,
        // the engine of the game that simulates an NPC dies by itself; the
        // deferred kill holds puppets alone. Off (npcEngineDamage false, the
        // bisect), every NPC holds at zero for the server's word as before
        this.engineDeaths = this.sp.settings["skymp5-client"]["npcEngineDamage"] !== false;
        // thuum docs/verbs/npc-puppet.md (Ownership): the looks of the effects
        // the host's engine runs on its actor, as the server records them, are
        // played on this seat's puppet of it (playtest 37 finding 60's
        // atronach, R-V4's sunlit vampire); off, a puppet stands bare (a bisect
        // switch, its old name kept)
        this.effectLooksOn = this.sp.settings["skymp5-client"]["npcCloakLook"] !== false;
        if (!this.enabled) {
            logTrace(this, "npcPuppet is off in settings: no actor's AI is switched (a bisect switch, not a play mode)");
        }
        this.controller.on("cellAttach", (e) => this.onCellAttach(e));
        this.controller.on("cellDetach", (e) => this.onCellDetach(e));
        this.controller.on("combatState", (e) => this.onCombatState(e));
        // ADR-067 decision 6 (D-C2 A): the engine's own death of an actor this
        // seat simulates (ADR-050 decision 2) reaches no kill of deathService's,
        // and formView's dead check skips a body already dead, so the block is
        // laid at the death's start; the poll below is its backstop
        this.controller.on("deathStart", (e) => this.onDeathStart(e));
        this.controller.on("update", () => this.onUpdate());
        this.controller.emitter.on("hostedActorStateMessage", (e) => this.onHostedState(e));
        // thuum tracker #130: the server's end of a fight between this seat's
        // player and an NPC another game hosts (reconcileAggro)
        this.controller.emitter.on("spSnippetMessage", (e) => this.onStopCombatAlarm(e));
        // a create word starts the actor's record over here: the server
        // serves its recorded effects right behind it when it holds any
        // (MpActor::SendHostedEffectsTo), so a set kept from an earlier sight
        // of the actor never outlives an empty record
        this.controller.emitter.on("createActorMessage", (e) => {
            if (e.message.refrId !== undefined) {
                this.recordedEffects.delete(e.message.refrId);
                // and its host's combat target with it (tracker #130): the
                // host's next state names it again
                this.relayedTargets.delete(e.message.refrId);
                this.aggroHeldUntil.delete(e.message.refrId);
            }
        });
    }

    // The one function. `mine` is the server's word as the caller read it
    // (HostService.isMine over the actor's model); undefined asks here.
    // `last` is the record's last sample, placed at a promote (the relay
    // stops at the grant).
    reconcile(localId: number, mine?: boolean, last?: Movement): void {
        if (!this.enabled || localId === 0x14) {
            return;
        }
        const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(localId));
        const ac = refr ? this.sp.Actor.from(refr) : null;
        if (!ac) {
            this.forget(localId);
            return;
        }
        if (this.isFigure(localId)) {
            // thuum tracker #119 (playtest 53, Eli: "ariadne just died to
            // sunlight ... i didn't see gold fire effect on her"): a player's
            // own game reports the effects with a look its engine runs on its
            // player (HostedStateService.reportOwnEffects), the server records
            // and relays them, and the figure plays exactly that set, as a
            // puppet plays its host's (Ownership; R-V4's rule: only what the
            // owner's engine holds active, so a burn its conditions keep off
            // indoors shows nowhere). The figure is no puppet otherwise
            if (ac.is3DLoaded()) {
                this.showEffectLooks(localId, ac);
            } else {
                this.effectLooks.delete(localId);
            }
            return;
        }
        // the fate's resurrect runs on the main thread, unordered against this
        // tick since fork cc5c82b6: while one is in flight for this actor
        // (deathService's RESURRECT_INFLIGHT_MS, mirrored here) no demote,
        // release or block of ours races its AI and activation writes (the
        // review of cc5c82b6, finding 4)
        const inFlightUntil = this.resurrectInFlightUntil.get(localId);
        if (inFlightUntil !== undefined) {
            if (Date.now() < inFlightUntil) {
                return;
            }
            this.resurrectInFlightUntil.delete(localId);
        }
        const host = this.controller.lookupListener(HostService);
        const hostedHere = mine ?? host.isMineLocal(localId);
        // an actor this seat hosts whose knocked body another seat's stream
        // owns (a grip's or a throw's row) is held as every other seat's copy
        // is, its AI off until the follow ends (sync/hostPuppet.ts)
        const bodyOwnedElsewhere = hostedHere && this.controller.lookupListener(PropService).bodyOwnedElsewhere(localId);
        const word = runsActorHere({ hostedHere, bodyOwnedElsewhere });
        if (bodyOwnedElsewhere && !this.heldForBody.has(localId)) {
            this.heldForBody.add(localId);
            logLab("verb:npc-handoff", `hold ${localId.toString(16)}: another seat's stream owns its body, this hosted copy held as a puppet until the follow ends`);
        }
        const has3D = ac.is3DLoaded();
        // thuum tracker #121 (ADR-051 decision 1's "its cell unattached";
        // the critic's release rule): a 3D the engine keeps in its buffer for
        // a cell it has detached is no load, so the copy is released to its
        // schedule after the grace below instead of standing as a puppet
        // with its AI off. Both x-gorm-inn-v4 reds (20261117-035309 and
        // 20261117-052416, steps 20 and 26): c1 stood in the inn and held
        // Gorm 0x1aa69 in Highmoon Hall 0x138cc with aiEnabled false and
        // is3DLoaded true, so its engine never walked him; were c1 the inn's
        // host, nobody would have taken him through the door. The cell's
        // attachment is HostService's read (cellAttachedFor); where it cannot
        // say (the player's own worldspace), the 3D decides as before
        const loadedHere = this.attached.has(localId) || (has3D && host.cellAttachedFor(ac) !== false);
        const flagged = this.sp.isPuppet(localId);
        const now = Date.now();
        if (!has3D) {
            // a shader or an art played on an actor ends with the 3D it was
            // attached to (Papyrus's OnLoad re-play idiom), so the record's
            // looks are played again on the next 3D
            this.effectLooks.delete(localId);
        }

        // the deferred kill (re-armed after a resurrect): a puppet never dies
        // by this game's hand, the server's word kills it; an actor this game
        // simulates dies by its own engine from the server's cause (ADR-050
        // decision 2), or holds at zero too while npcEngineDamage is off
        // thuum tracker #128: never armed again on a seat that holds at zero
        // over a body the server's word killed. The record is "ended" once
        // deathService.killActor has ended the hold for that word
        // (deferredKillEnded, before its Kill or KillSilent), and stays so
        // until the word stands the actor up (onResurrected) or a pass finds
        // this seat simulating it (the clear below, where the old record's
        // stale "armed" cleared: a raise this seat's engine took, #112, or a
        // host's own Resurrect, never pass through onResurrected), so a
        // later loss of the hosting arms the hold again as before. The record
        // used to keep "armed" past the kill, which held a re-arm back only
        // for an actor armed before its kill; one with no record at its kill
        // would be armed by the same pass right after its queued kill
        // (x-server-body-late-v3 20261118-030533's copy ff000df4, killed at
        // its bind in the pass that also demoted it), if no earlier pass had.
        // HYPOTHESIS, unread: that no earlier pass (a cell attach's) had
        // armed that copy, and that the arm is what swallowed its kill
        // (formView's dead check reads the kill back)
        const holdAtZero = !word || !loadedHere || !this.engineDeaths;
        const held = this.deferredKill.get(localId);
        if (holdAtZero && held === undefined) {
            ac.startDeferredKill();
            this.deferredKill.set(localId, "armed");
        } else if (!holdAtZero && held !== undefined) {
            if (held === "armed") {
                ac.endDeferredKill();
            }
            this.deferredKill.delete(localId);
        }

        if (!loadedHere) {
            // thuum ADR-051 (npc-handoff D11; b-npc-handoff-night 092208,
            // 114904): an actor this engine no longer loads is simulated by
            // nobody here and sighted by nobody while its AI is off; after
            // the grace (a 3D dropped by the late fate comes back within it,
            // playtest 34) it is released: the flag off, the AI and the
            // activation back on, left to its own schedule. The next pass
            // makes it a puppet again when it loads on a seat that does not
            // host it, and the host's roster sights it.
            host.markHosted(localId, false);
            const since = this.unloadedSince.get(localId);
            if (since === undefined) {
                this.unloadedSince.set(localId, now);
                return;
            }
            if (flagged && now - since >= PuppetService.releaseGraceMs) {
                // tracker #130: the release turns its AI on, so a combat this
                // seat started on its player stops first (tracker #121: a
                // release can now keep its 3D, a cell this engine detached and
                // buffers, so the reason names which)
                this.endAggro(localId, ac, has3D ? "its release, its cell detached here, its 3D in the buffer" : "its release, no 3D here");
                if (has3D) {
                    // tracker #121: a release that keeps its 3D (a cell this
                    // engine detached and buffers) undoes the puppet's imposed
                    // state as the promote below does (movementApply.ts holds
                    // the relay's look-at and head tracking off on a puppet;
                    // PuppetApi.cpp ReleasePuppet clears the block and the AI
                    // alone), so a copy this seat later hosts from the buffer
                    // carries none of it. Before the release rule every
                    // release came with the 3D gone
                    ac.clearLookAt();
                    ac.setHeadTracking(true);
                    ac.clearExpressionOverride();
                }
                this.sp.releasePuppet(localId);
                this.puppets.delete(localId);
                // the release lifts the block natively (PuppetApi.cpp
                // ReleasePuppet: AI on, flag and block off), for a live actor's
                // schedule; a body comes back under the block at its next load
                // here (ADR-067 decision 6)
                this.markLifted(localId);
                const why = has3D ? "its cell detached here, its 3D in the buffer (tracker #121)" : "no 3D here";
                logTrace(this, `released ${localId.toString(16)}: ${why}, left to its schedule (npc-handoff D11)`);
            }
            return;
        }
        this.unloadedSince.delete(localId);

        if (word) {
            // tracker #130: an actor this game hosts fights under its own
            // engine, so a combat this seat started on it as a puppet stops
            this.endAggro(localId, ac, flagged ? "its promote, this game hosts it now" : "this game hosts it");
            // and the server relays its state to every listener but its host,
            // so the last one this seat heard is no truth any more: a later
            // demote waits for the next host's first state
            this.forgetRelay(localId);
            // thuum docs/verbs/npc-stream.md (ADR-050): the word says this
            // game simulates the actor: its actions stream from here (the
            // hosted set the action hook reads)
            host.markHosted(localId, true);
            // and this engine runs its effects and draws their looks itself
            // (Ownership): any look of the record this seat played on it while
            // it was another's, a puppet's or a released body's, is stopped
            this.stopEffectLooks(localId, ac);
            // ADR-067 decision 6 (D-C2 A): a body this seat hosts is blocked
            // like a CONT. The dead read (a native) runs on the poll's pass and
            // on the first pass two tick counts after a native lift, not on
            // the view's per-frame call: the death's start lays the block at
            // once (onDeathStart), the poll re-asserts it within pollMs (a
            // body the engine's own Resurrect, a reanimation or a failed fate
            // resurrect left unblocked), and a stood-up actor this seat hosts
            // gets its activation back, as the promote gives a live one
            const lifted = this.liftedAt.get(localId);
            if (mine === undefined || (lifted !== undefined && this.tick >= lifted + 2)) {
                if (ac.isDead()) {
                    this.blockDeadBody(localId, ac, "hosted here");
                } else {
                    this.standsAgain(localId, ac, true);
                }
            }
            if (!flagged) {
                return;
            }
            // docs/engine/non-owning-seat.md R1: the promote's placement, read
            // before the code changes it (the instrumented run on the parent):
            // where the engine had the copy, what it is placed from, and the
            // cell the record (the last sample this seat holds) names
            // an actor the engine holds in a furniture is placed by it, never by
            // the record (HostService.seatedBy, tracker #116: Arvel thrown out
            // of his web at the promote)
            const seat = HostService.seatedBy(ac);
            {
                const eCell = refr ? ObjectReferenceEx.getWorldOrCell(refr) : 0;
                const at = `(${ac.getPositionX().toFixed(0)}, ${ac.getPositionY().toFixed(0)}, ${ac.getPositionZ().toFixed(0)}) in ${eCell.toString(16)}`;
                const from = seat
                    ? `seated here (${seat}), its furniture places it`
                    : this.heldForBody.has(localId)
                    ? "held for another seat's stream of its body: the follow's end placed it at the owner's rest"
                    : last
                    ? `placed from the last sample at (${last.pos[0].toFixed(0)}, ${last.pos[1].toFixed(0)}, ${last.pos[2].toFixed(0)}) in ${last.worldOrCell.toString(16)}`
                    : "no sample to place from, the engine's own stands";
                logLab("verb:npc-handoff", `promote ${localId.toString(16)}: ${from}; the engine had it at ${at}`);
            }
            if (last && placesAtPromote({ heldForBody: this.heldForBody.has(localId), hasSample: true, seated: !!seat })) {
                // the record's last point and heading first: the relay stops
                // at the grant (the native promote leaves the transform alone)
                ac.setPosition(last.pos[0], last.pos[1], last.pos[2]);
                ac.setAngle(last.rot[0], last.rot[1], last.rot[2]);
            }
            this.heldForBody.delete(localId);
            // the puppet's imposed state undone, the inverse of the apply's
            // (movementApply.ts: head tracking on the relayed look-at point);
            // the activation block is the native demote's and the native
            // promote lifts it (PuppetApi.cpp; playtest 32's dead Talk prompts
            // after a handoff, playtest 33's dialogue stuck on an unblocked
            // puppet)
            ac.clearLookAt();
            ac.setHeadTracking(true);
            // and the expression override the puppet period may have left
            // (the re-analyst's read of 2026-10-28: the engine never sets it
            // itself, and while set it blocks the line's own expression on
            // the live NPC; playtest 37's exprOverride true on the talker's
            // Orgnar)
            ac.clearExpressionOverride();
            this.sp.setPuppet(localId, false);
            // ADR-068 D-W3 B: an owner gained here (the flag is off from this
            // call on, PuppetApi.cpp), so the platform's held trigger entries
            // are asked at the next update (HostService.ownerFlips)
            host.noteOwnerFlip();
            // the native promote lifts the block for a live actor and a dead
            // one alike (PuppetApi.cpp Promote; its dead branch leaves the AI
            // off, H-54e). A body promoted here comes back under the block on
            // a later pass (ADR-067 decision 6, D-C2 A), never in this tick:
            // the re-block that stood here until c1de7167 went out in the same
            // tick as the promote, whose task lands after the tick (main.cpp
            // OnUpdate) and so would have undone it (a reading of the source,
            // no trace); that, with D71's AI-off refusal (engine model E25),
            // is the reading of playtest 37's "the server's activation never
            // opened a blocked body".
            this.markLifted(localId);
            this.puppets.delete(localId);
            logTrace(this, `live ${localId.toString(16)}`);
            return;
        }

        // not this game's to simulate: a puppet, once it has a 3D (an actor
        // still loading is left alone until it has one; the view asks again
        // every update: the gate's hang at a landing, 2026-10-21, under
        // reading: the demote must not race the loader on a half-made actor)
        host.markHosted(localId, false);
        // thuum docs/verbs/npc-puppet.md (Ownership): the looks of the effects
        // its host's engine runs on it, as recorded, alive or a body (the
        // host's engine dispels what a death dispels, and its next report
        // says so)
        if (has3D) {
            this.showEffectLooks(localId, ac);
        }
        if (ac.isDead()) {
            // tracker #130: a body fights nobody, and the host's target from
            // before its death is forgotten (seesDeath)
            this.endAggro(localId, ac, "its death");
            this.seesDeath(localId);
            // thuum docs/verbs/npc-fate.md (the deaths, as revised by playtest
            // 37's findings 58 and 62): a dead NPC body is never a puppet, on
            // any seat. A body that died flagged (the server's word killed a
            // puppet) is released through the promote's dead branch
            // (PuppetApi.cpp Promote: the block lifted, the AI left off, the
            // flag cleared) and never demoted again; the deferred kill above
            // keeps holding it for the server's word, and the fate's resurrect
            // (FateApi.cpp, the AI on for the call) stands it up as a live
            // actor, which the next pass makes a puppet as before.
            // ADR-067 decision 6 (D-C2 A, docs/private/container-hold-scope.md):
            // the loot is the server's container and the body is
            // activation-blocked like a CONT: E sends Activate (the hook
            // reports a blocked reference's activation as it does a chest's),
            // the server admits one looter and answers OpenContainer, and
            // remoteServer's handler opens the ContainerMenu (openBodyOnTimers
            // for a released body: the AI on for the open alone, D71). Finding
            // 58's "the server's activation never opened a blocked body" read
            // the AI-off refusal (engine model E25: a dead actor with no
            // AIProcess refuses the activation) and, by the source, a re-block
            // lost to the same tick as a promote; that the engine's
            // activate(player, true) opens the loot menu on a blocked dead
            // actor as it opens a blocked CONT is the HYPOTHESIS
            // x-container-hold's corpse leg reads. The view lays the server's
            // list under the menu as before (memo D52 B). The block lands two
            // tick counts after the release below, never in its tick (the
            // release's task runs after that tick's JavaScript).
            if (flagged) {
                // playtest 41's item 5 (Eli, 2026-11-02: "still have frozen
                // peeps on death"; playtest 40's F3 read the same): a body
                // released at the instant of its death has its AI off and
                // its flag cleared, so neither the engine (its kProcessMe
                // gate skips AI-off actors) nor the puppet detour (flagged
                // actors alone) ticks its animation graph, and the death
                // ragdoll never runs: the mesh freezes mid-pose and sinks.
                // The flag stays through the ragdoll's settle, the detour
                // keeps the graph ticking, then the release as before
                // (HYPOTHESIS until x-npc-death-freeze: the detour's job
                // drives the ragdoll; the settle fits ragdollSettleMs)
                const since = this.deadFlaggedSince.get(localId);
                if (since === undefined) {
                    this.deadFlaggedSince.set(localId, now);
                    return;
                }
                if (now - since < PuppetService.ragdollSettleMs) {
                    return;
                }
                this.deadFlaggedSince.delete(localId);
                this.sp.setPuppet(localId, false);
                this.puppets.delete(localId);
                this.markLifted(localId);
                logTrace(this, `body ${localId.toString(16)}: released from the puppet state after the ragdoll's ${((now - since) / 1000).toFixed(1)} s, its activation the engine's`);
                return;
            }
            // released, or never flagged here (a body that died under this
            // seat's own engine before the host flipped, a body loaded dead):
            // blocked like a CONT, once per load (the mark), two tick counts
            // after any native lift (ADR-067 decision 6)
            this.blockDeadBody(localId, ac, "on a seat that does not host it");
            return;
        }
        // the body is alive here: a stand-up that took (the fate's resurrect
        // lands on the game thread after the JS tick, so the clear cannot sit
        // in onResurrected: the same tick's reconcile still saw the body dead
        // and restamped it; numidium's review of db0e19a5). The ragdoll settle
        // clock forgets the death that was undone, so a later death gets its
        // whole settle (x-npc-death-freeze 140538 and 152912: the release came
        // 0.75 to 1.0 s into the second fall and froze the skeleton, and would
        // still have come 1.0 to 1.85 s into it with db0e19a5's clear)
        this.deadFlaggedSince.delete(localId);
        this.deadSeen.delete(localId);
        // a body that stood up on a seat that does not host it: the demote
        // below blocks it as a puppet, so the dead block's mark is dropped
        // (a later death lays it again)
        this.standsAgain(localId, ac, false);
        if (!has3D) {
            return;
        }
        // thuum tracker #121 (ADR-035 point 8 as amended 2026-11-16, Eli's
        // "go with b for 121"): the destination's host owns the arrival. An
        // actor no game's word holds, whose running package heads for a cell
        // this game hosts, keeps its AI on its way there across cells nobody
        // hosts. Nothing reports it on the way (it is in no hosted cell of
        // this game's, so neither the roster nor the hosted set names it,
        // and a frozen cell stays unreported); once its engine has it in this
        // game's cell the roster sights it, and the server takes the sighting
        // from that cell's host (PartOne's rule: a record in a cell nobody
        // hosts yields to the host of the cell the engine has it in). Playtest
        // 53: Ariadne's engine walked Gorm 0x1aa69 out of the frozen Highmoon
        // Hall at his supper hour, and this demote stopped him at its door
        // ("demote 1aa69: the engine had it at (-38681, 63455, -13786) in 3c",
        // c2-console 01:46:29Z). Bounded: another game's word, or a package
        // that names no cell this game hosts, demotes as before
        if (!flagged && !bodyOwnedElsewhere && this.headsForHostedCell(localId, host)) {
            return;
        }
        if (!flagged && this.waitsForSeat(localId, refr, ac, now)) {
            return;
        }
        if (!flagged) {
            if (!this.sp.setPuppet(localId, true)) {
                logError(this, `setPuppet(${localId.toString(16)}, true) found no actor`);
                return;
            }
            this.puppets.add(localId);
            // tracker #130: the demote's native StopCombatAndAlarmOnActor lands
            // after this tick, so no aggro start runs before two tick counts
            this.demotedAt.set(localId, this.tick);
            // non-owning-seat.md R1: the demote's side of the same read
            {
                const eCell = refr ? ObjectReferenceEx.getWorldOrCell(refr) : 0;
                logLab("verb:npc-handoff", `demote ${localId.toString(16)}: the engine had it at (${ac.getPositionX().toFixed(0)}, ${ac.getPositionY().toFixed(0)}, ${ac.getPositionZ().toFixed(0)}) in ${eCell.toString(16)}; the record's cell ${last ? last.worldOrCell.toString(16) : "unknown here"}`);
            }
            // the engine's activation (Talk, trade, pickpocket) is the host's
            // alone: the native demote blocks it, so nothing Papyrus-backed runs
            // here, at the load's first attaches (train-34's first head: the one
            // Papyrus call this demote made ran into Intuitive Dragon Ride
            // Control's quest start-up half a second later on the same game
            // thread, b-invis-recast on gate 20261023-031111)
            logTrace(this, `puppet ${localId.toString(16)}`);
        }
        this.stopLocalScene(localId, ac);
        this.reconcileAggro(localId, ac, host, mine === undefined);
    }

    // thuum tracker #130 (design A): an NPC another game hosts that fights
    // this seat's player drew no combat from this seat's engine, since its
    // puppet here has its AI off and H6 undoes any combat it enters, so this
    // player's compass showed no enemy and the wait menu opened beside it.
    // The host's combat target is R2: the host's engine reads it
    // (HostedStateService), the server records it (SetHostedState) and
    // relays the state to the actor's other listeners
    // (ActionListener::OnHostedActorState). When that target is this seat's
    // own player, by its server id (RemoteServer.getMyRemoteRefrId: a seat
    // holds no view of its own player, so the view's id map never answers
    // 0x14 for it), the puppet gets Papyrus StartCombat on the player and
    // the aggro mark, and H6 leaves a marked puppet's combat alone.
    // The mark comes off, with StopCombat on the puppet, when the host's
    // target turns to anyone else or to none, at the puppet's death, its
    // release or its promote, when no other game hosts it any more, and at
    // the server's StopCombatAlarm for the pair (PartOne::TickFights' end,
    // onStopCombatAlarm). It comes off with no stop when the puppet's engine
    // ends the combat (the combat event) or holds none at a poll's pass:
    // Papyrus StartCombat is a queued task that refuses a dead, processless
    // or talking actor (re-ai-puppet.md:181-184). A new start then waits
    // aggroRetryMs. The server's end holds a new start for aggroHoldMs: its
    // D2 end stops the NPC's combat in its host's game as well, and that
    // host's next state, not the target this seat heard before the end,
    // says whether the NPC fights on. A start waits two tick counts after a
    // demote, whose native StopCombatAndAlarmOnActor lands after the tick
    // (the dead block's rule, blockDeadBody). The relay is unreliable unless
    // it carries effects, and every state carries the target, so a lost
    // change waits for the host's next state.
    // The combat this seat's engine runs on the puppet is meant to be R3:
    // this seat reports nothing of a puppet's combat. HYPOTHESIS until a
    // two-seat probe reads it: that its effects stay on this seat. Actors
    // this seat hosts (its player's follower, summon or thrall, the
    // puppet's allies in a cell this seat hosts) may answer that combat
    // under their own AI, and then their hits reach the server as R1 hits
    // and their new targets as R2 states, the channels any fight of theirs
    // takes. The probe counts those: hits on the puppet by actors this seat
    // hosts, and any target they report that turns to this seat's player.
    // HYPOTHESIS until the same probe reads this seat's wait refusal: that
    // StartCombat on a puppet with its AI off puts it on this seat's compass
    // and gates the wait menu. The static read has both reading the combat
    // groups that target the player (thuum ghidra/notes/hostility-1-7-104.md
    // :43-70, the wait gate; :92-97, the compass), and Papyrus StartCombat
    // makes no AI test (re-ai-puppet.md:181-187), but a-hostility proved the
    // compass only for a player's figure, whose AI runs.
    private reconcileAggro(localId: number, ac: Actor, host: HostService, readEngine: boolean): void {
        if (this.relayedTargets.size === 0 && this.aggro.size === 0) {
            return;
        }
        const refrId = localIdToRemoteId(localId);
        const target = refrId ? this.relayedTargets.get(refrId) ?? 0 : 0;
        const started = this.aggro.get(localId);
        if (!target && started === undefined) {
            return;
        }
        const me = this.controller.lookupListener(RemoteServer).getMyRemoteRefrId();
        let why = "";
        if (me === 0 || target !== me) {
            why = `its host's target is ${target ? target.toString(16) : "none"} now`;
        } else if (!host.isOthersLocal(localId)) {
            // no game's state of it reaches this seat now, so the last one is
            // forgotten: the next host's first state names the target again
            this.forgetRelay(localId);
            why = "no other game hosts it now";
        } else if (this.isAggroHeld(refrId)) {
            why = `the server ended the fight (StopCombatAlarm); a new start held ${PuppetService.aggroHoldMs} ms for its host's next state`;
        }
        if (why) {
            this.endAggro(localId, ac, why);
            return;
        }
        if (started !== undefined) {
            // the poll's pass reads the engine, two tick counts after the
            // start at the least (its task lands after the start's tick):
            // isInCombat, the read a trace took true on a puppet a hit drafted
            // (npc-puppet.md, H6's row)
            if (readEngine && this.tick >= started + 2 && !ac.isInCombat()) {
                this.endAggro(localId, null, "its engine holds no combat (the start refused, or ended with no event here)", true);
            }
            return;
        }
        const retryAt = this.aggroRetryAt.get(localId);
        if (retryAt !== undefined) {
            if (Date.now() < retryAt) {
                return;
            }
            this.aggroRetryAt.delete(localId);
        }
        const demoted = this.demotedAt.get(localId);
        if (demoted !== undefined) {
            if (this.tick < demoted + 2) {
                return;
            }
            this.demotedAt.delete(localId);
        }
        const player = this.sp.Game.getPlayer();
        if (!player) {
            return;
        }
        ac.startCombat(player);
        this.aggro.set(localId, this.tick);
        logLab("verb:hostility-sync", `aggro: ${localId.toString(16)} starts combat on this seat's player: its host's target is ${me.toString(16)}, this seat's own (tracker #130)`);
    }

    // tracker #130: the aggro mark off and the puppet's combat stopped, one
    // line. Null for the actor stops nothing: no actor here, a caller whose
    // own native stops the combat, or an engine that holds none. `retry`
    // holds a new start for aggroRetryMs (an engine that dropped a start)
    private endAggro(localId: number, ac: Actor | null, why: string, retry = false): void {
        if (!this.aggro.delete(localId)) {
            return;
        }
        if (ac) {
            ac.stopCombat();
        }
        if (retry) {
            this.aggroRetryAt.set(localId, Date.now() + PuppetService.aggroRetryMs);
        }
        logLab("verb:hostility-sync", `aggro: ${localId.toString(16)} stops its combat on this seat's player: ${why}${retry ? `; a new start no sooner than ${PuppetService.aggroRetryMs} ms on` : ""} (tracker #130)`);
    }

    // tracker #130: whether a new start on the actor (by server id) is held
    // after the server's end; a hold past its time is dropped
    private isAggroHeld(refrId: number): boolean {
        const until = this.aggroHeldUntil.get(refrId);
        if (until === undefined) {
            return false;
        }
        if (Date.now() < until) {
            return true;
        }
        this.aggroHeldUntil.delete(refrId);
        return false;
    }

    // tracker #130: the relay of the actor's state to this seat has ended
    // (this game hosts it, no game does, its record died), so its host's
    // last target here is forgotten, and a hold with it
    private forgetRelay(localId: number): void {
        if (this.relayedTargets.size === 0 && this.aggroHeldUntil.size === 0) {
            return;
        }
        const refrId = localIdToRemoteId(localId);
        if (refrId) {
            this.relayedTargets.delete(refrId);
            this.aggroHeldUntil.delete(refrId);
        }
    }

    // tracker #130: a death's first sight on this seat (its start here, or a
    // pass that finds a body). The server relays no state of a dead record
    // (ActionListener::OnHostedActorState's dead verdict), so the host's
    // target from before the death is forgotten, once per death: a state
    // that arrives while the body still lies here (the host's first after a
    // stand-up) is kept for the body's stand-up on this seat
    private seesDeath(localId: number): void {
        if (this.deadSeen.has(localId)) {
            return;
        }
        this.deadSeen.add(localId);
        this.forgetRelay(localId);
    }

    // tracker #130: the server's end of the fight between this seat's player
    // and an actor (PartOne::TickFights: Actor.StopCombatAlarm on the actor,
    // sent to each player of the pair; the snippet itself runs through
    // SpSnippetService). While the host's target still names this player a
    // new start is held for aggroHoldMs; a marked puppet's combat stops on
    // the next update (this is the message's context, no natives here)
    private onStopCombatAlarm(event: ConnectionMessage<SpSnippetMessage>) {
        const msg = event.message;
        if (msg.class.toLowerCase() !== "actor" || msg.function.toLowerCase() !== "stopcombatalarm") {
            return;
        }
        const localId = snippetIdToLocalId(msg.selfId);
        if (!localId || localId === 0x14) {
            return;
        }
        const refrId = localIdToRemoteId(localId);
        const me = this.controller.lookupListener(RemoteServer).getMyRemoteRefrId();
        if (refrId && me !== 0 && this.relayedTargets.get(refrId) === me) {
            this.aggroHeldUntil.set(refrId, Date.now() + PuppetService.aggroHoldMs);
        }
        if (!this.aggro.has(localId)) {
            return;
        }
        this.controller.once("update", () => {
            this.endAggro(localId, this.sp.Actor.from(this.sp.Game.getFormEx(localId)), `the server ended the fight (StopCombatAlarm); a new start held ${PuppetService.aggroHoldMs} ms for its host's next state`);
        });
    }

    // The poll: every actor in the engine's high-process list (a named
    // resident the engine runs is found here within pollMs of its appearance,
    // the cellAttach events naming temporary references alone) and every
    // actor this service has touched, through reconcile. A 0xff actor the
    // server does not describe (a summon, the engine's own temporaries) is
    // nobody's and left alone; a copy with a view goes through the view's
    // own call as well.
    private onUpdate() {
        // the JavaScript tick's count, the clock the dead block's re-assert
        // reads (blockDeadBody): a native lift queued in tick N lands after
        // tick N's JavaScript, so a block is safe once the count has moved
        // twice since the lift (a call later in tick N may already see N + 1)
        this.tick += 1;
        if (!this.enabled) {
            return;
        }
        const now = Date.now();
        // a copy waiting for its seat is frozen at the bound every frame, whether or not anything reconciles it
        // again: reconcile runs on a copy's events and the poll, and a quiet copy's wait outlasted SEAT_WAIT_MS by
        // tens of seconds with its AI on
        for (const [localId, since] of Array.from(this.seatWaitSince)) {
            if (now - since >= SEAT_WAIT_MS) {
                this.reconcile(localId);
            }
        }
        if (now - this.lastPoll < PuppetService.pollMs) {
            return;
        }
        this.lastPoll = now;
        const ids = new Set<number>(this.sp.getHighProcessActors());
        this.puppets.forEach((id) => ids.add(id));
        this.attached.forEach((id) => ids.add(id));
        ids.forEach((id) => {
            if (id === 0x14) {
                return;
            }
            if (id >= 0xff000000 && !localIdToRemoteId(id)) {
                return;
            }
            this.reconcile(id);
        });
    }

    // playtest 41's Sven scene (Eli, 2026-11-02: "the sven scene is wack, it
    // starts over when the other player walks up and animations play for
    // neither"): DialogueRiverwoodIntroScene (quest 0x954d9) started on BOTH
    // engines when each player arrived (22:55:54Z, 0.6 s apart); the host's
    // ran its phases over its live actors and relayed their lines, faces and
    // idles, while this seat's own copy drove the same actors as puppets with
    // no AI, held their behavior against the relayed idles, and restarted at
    // the walk-up. Scenes are the host's (CLAUDE.md: we do not reimplement
    // what a host client runs for us; a guest renders the relay), so a scene
    // the engine plays here on a puppet is stopped (Papyrus Scene.Stop), once
    // per scene and actor until the engine starts it again (the poll re-stops
    // it). A live actor's scene (this seat hosts it) is left alone.
    // thuum ADR-073 step 5a (decision 6 with D-L2 B): a scene this seat
    // started is its own, and its claim of the actor promotes the actor here;
    // while that claim is unsent or unanswered the scene plays on
    // (SceneClaimService.holdsScene) and the poll asks again after it
    // tracker #121: whether an actor no other game's word holds is walking
    // to a cell this game hosts, by its running package's location (the
    // platform's game-thread snapshot, getPackageDestinationCells), said once
    // per actor and destination. An older platform without the export
    // answers no, and the demote runs as before
    private headsForHostedCell(localId: number, host: HostService): boolean {
        if (host.isOthersLocal(localId) || typeof this.sp.getPackageDestinationCells !== "function") {
            return false;
        }
        const cell = this.sp.getPackageDestinationCells(localId).find((c) => host.hostsCell(c));
        if (cell === undefined) {
            this.walkingHere.delete(localId);
            return false;
        }
        if (this.walkingHere.get(localId) !== cell) {
            this.walkingHere.set(localId, cell);
            logLab("verb:npc-handoff", `keep ${localId.toString(16)} live: its package heads for cell ${cell.toString(16)}, which this game hosts; nothing reported until it stands there (tracker #121)`);
        }
        return true;
    }

    // tracker #121: the actors kept live on their way here, by destination
    private readonly walkingHere = new Map<number, number>();

    // A copy the game file ties to a furniture is its engine's to seat at the
    // load (sync/seatWait.ts): while one stands near its linked furniture
    // unseated, within SEAT_WAIT_MS of its first 3D here, the demote waits
    // and its AI runs to seat it; then it is frozen as every copy is, seated
    // or where it stands. One line when the wait starts and one at its end
    private waitsForSeat(localId: number, refr: ObjectReference | null, ac: Actor, now: number): boolean {
        const linked = refr ? refr.getLinkedRef(null) : null;
        const furniture = linked && this.sp.Furniture.from(linked.getBaseObject()) ? linked : null;
        if (!furniture) {
            this.seatWaitSince.delete(localId);
            return false;
        }
        const dx = ac.getPositionX() - furniture.getPositionX();
        const dy = ac.getPositionY() - furniture.getPositionY();
        const dz = ac.getPositionZ() - furniture.getPositionZ();
        const units = Math.sqrt(dx * dx + dy * dy + dz * dz);
        const held = ac.getFurnitureReference();
        const sitState = ac.getSitState();
        const isSeated = seated({ sitState, furnitureIsLinked: !!held && held.getFormID() === furniture.getFormID(), unitsFromFurniture: units });
        const since = this.seatWaitSince.get(localId);
        const verdict = seatVerdict({ linkedFurniture: true, seated: isSeated, unitsFromFurniture: units, sinceMs: since === undefined ? 0 : now - since });
        // the engine's own furniture and its sleep state beside the sit state: a lying user (a sarcophagus, a bed) reads
        // sit state 0 while its engine holds the furniture, off its origin by more than SEATED_UNITS
        const at = `${units.toFixed(0)} units from furniture ${furniture.getFormID().toString(16)}, sit state ${sitState}, sleep state ${ac.getSleepState()}, the engine's furniture ${held ? held.getFormID().toString(16) : "none"}`;
        if (verdict === "wait") {
            if (since === undefined) {
                this.seatWaitSince.set(localId, now);
                logLab("verb:npc-handoff", `seat wait ${localId.toString(16)}: its engine seats it at the load (${at}); not frozen until seated or ${SEAT_WAIT_MS} ms`);
            }
            return true;
        }
        if (since !== undefined) {
            this.seatWaitSince.delete(localId);
            logLab("verb:npc-handoff", `seat wait ${localId.toString(16)}: ${isSeated ? `seated after ${now - since} ms` : `not seated within ${SEAT_WAIT_MS} ms`} (${at}); frozen ${isSeated ? "seated" : "where it stands"}`);
        }
        return false;
    }

    // the first 3D here of each copy waiting for its seat
    private readonly seatWaitSince = new Map<number, number>();

    private stopLocalScene(localId: number, ac: Actor): void {
        const scene = ac.getCurrentScene();
        if (!scene || !scene.isPlaying()) {
            this.scenesStopped.delete(localId);
            return;
        }
        const sceneId = scene.getFormID();
        if (this.scenesStopped.get(localId) === sceneId) {
            return;
        }
        if (this.controller.lookupListener(SceneClaimService).holdsScene(localId, sceneId)) {
            return;
        }
        this.scenesStopped.set(localId, sceneId);
        const quest = scene.getOwningQuest();
        const questId = quest ? quest.getFormID() : 0;
        scene.stop();
        logTrace(this, `scene ${sceneId.toString(16)} of quest ${questId ? questId.toString(16) : "none"} stopped on this seat: ${localId.toString(16)} is the host's puppet (playtest 41's Sven scene)`);
        // x-quest-runs 20261110-071422's read: the engine then stops the
        // scene's quest here (c2's 3a17d of 3a17c stopped at 07:20:18.202, the
        // quest's stop at .522), a stop that is this seat's own and never the
        // world's, so QuestStageService is told the quest (JS state alone)
        if (questId !== 0) {
            this.controller.emitter.emit("puppetSceneStopped", { quest: questId, scene: sceneId });
        }
    }

    // Whether this game switched the actor's AI off (by its local form id):
    // the native flag, the detours' truth.
    isPuppet(localId: number): boolean {
        return this.sp.isPuppet(localId);
    }

    // Whether the actor is a player's figure or the player's own clone: a
    // server actor whose model carries no NPC base (the server omits base
    // 0x7, PartOne.cpp) or is the clone. A figure is never a puppet: its
    // own game's reports hold it, and its hold (the keep-offset on itself)
    // is an AI package, so a demote leaves it standing where it spawned
    // (a-racemenu 20261022-050437 on the gate: c2's figure of c1 absent
    // where the server had c1, after c2 demoted it at its attach). Also
    // RelayedActivationService's (rung 5: a relayed entry's presser is
    // another player when it is a figure)
    isFigure(localId: number): boolean {
        // the engine's own answer first: a figure is placed from the player
        // base 0x7 or from a runtime base the appearance made (0xff and
        // above); a server-made copy of a game-file NPC carries that NPC's
        // base. The view's id map below can lag a copy's cellAttach by a
        // tick, and a figure read as a copy was demoted (a-racemenu
        // 20261022-050437; playtest 31's figures moving oddly for the other
        // seat, 2026-10-22)
        const ac = this.sp.Actor.from(this.sp.Game.getFormEx(localId));
        const base = ac ? ac.getBaseObject() : null;
        if (base) {
            const baseId = base.getFormID();
            if (baseId === 0x7 || baseId >= 0xff000000) {
                return true;
            }
        }
        const remoteId = localIdToRemoteId(localId);
        if (!remoteId) {
            return false;
        }
        const world = this.controller.lookupListener(RemoteServer).getWorldModel();
        const form = world.forms.find((f) => f !== undefined && f.refrId === remoteId);
        if (!form) {
            return false;
        }
        // a player's figure: no base, the player base, or the message's
        // default 0 (every player's CreateActor carries baseId 0, 2026-10-22)
        return form.isMyClone === true || !form.baseId || form.baseId === 0x7;
    }

    // After a hot reload, once the outgoing client's teardown has run its
    // demotes: each actor the restored hosted set holds that the platform's
    // set lost is set again (HostService.setLostHostedAgain) and reconciled,
    // so a puppet flag the outgoing client put on it is lifted now rather
    // than at the next poll
    afterReload(): void {
        const lost = this.controller.lookupListener(HostService).setLostHostedAgain();
        lost.forEach((id) => this.reconcile(id));
    }

    // The view's two words of before, kept for their callers: a puppet
    // unless the word says otherwise, live by the word.
    ensurePuppet(refr: ObjectReference): void {
        this.reconcile(refr.getFormID(), false);
    }

    ensureLive(refr: ObjectReference, last?: Movement): void {
        this.reconcile(refr.getFormID(), true, last);
    }

    hostedCount(): number {
        return this.puppets.size;
    }

    // An actor whose form the server took back while it was live here (this
    // game left its reach; a disconnect): no word, so not this game's; a
    // puppet if loaded, released to its schedule after the grace if not.
    demoteOnLoss(localId: number): void {
        this.reconcile(localId, false);
    }

    // A stood-up actor (deathService's resurrect) holds at zero again for
    // the server's word: the deferred kill is re-armed on the next pass. The
    // ragdoll settle clock is cleared in reconcile's live path, not here: the
    // resurrect is queued to the main thread and this tick's reconcile still
    // sees the body dead (x-npc-death-freeze 140538 and 152912, the guest's
    // body half-up: the release came into the SECOND fall with the clock
    // counting from the first death).
    onResurrected(localId: number): void {
        this.deferredKill.delete(localId);
        // the fate's resurrect runs on the game's main thread at its next UI
        // drain, unordered against this tick since fork cc5c82b6 (FateApi.cpp
        // resurrectActor: a puppet blocked, a live actor that stood up
        // unblocked, a body the call left dead kept blocked): the dead
        // block's mark is dropped, a later pass reads the body again, and
        // reconcile stands aside while the resurrect is in flight
        this.markLifted(localId);
        this.resurrectInFlightUntil.set(localId, Date.now() + PuppetService.resurrectInFlightMs);
    }

    // thuum tracker #128: deathService.killActor ended the actor's deferred
    // kill for the server's word (endDeferredKill, then Kill or KillSilent),
    // so the record says so ("ended", never "armed" past it) and no pass on
    // a seat that holds at zero arms the hold again over the queued kill or
    // on the body; the word's stand-up (onResurrected) or a pass that finds
    // this seat simulating it clears it, and a later pass arms it as before.
    // Never a player's figure: reconcile leaves a figure before the arm, so
    // the record holds none (killActor's branch for an actor other than the
    // local player reaches figures too)
    deferredKillEnded(localId: number): void {
        if (this.isFigure(localId)) {
            return;
        }
        this.deferredKill.set(localId, "ended");
    }

    // ADR-067 decision 6 (the exclusive container hold; docs/private/
    // container-hold-scope.md D-C2 A): a dead NPC body is the server's
    // container and is activation-blocked like a CONT, on every seat, so its E
    // sends Activate and its loot menu opens on the server's OpenContainer
    // alone (remoteServer's handler). Once per body per load (the mark);
    // never a player's figure (no corpse to the server: IsCorpse excludes an
    // actor created as a player, and a figure is blocked at its spawn anyway,
    // spawnProcess's dealWithRef); never in the tick of a native lift
    // (markLifted: a lift queued in this tick lands after it, on the VM
    // thread's drain for the puppet natives or on the main thread's next UI
    // drain for the fate's resurrect, and would undo the block; two counts
    // of the tick clock rule that tick out, and the resurrect's in-flight
    // window keeps reconcile away). Called at the death's start here, at the server's kill
    // (deathService.killActor) and from reconcile's passes.
    blockDeadBody(localId: number, ac: Actor, why: string): void {
        if (localId === 0x14 || this.deadBlocked.has(localId)) {
            return;
        }
        const lifted = this.liftedAt.get(localId);
        if (lifted !== undefined && this.tick < lifted + 2) {
            return;
        }
        if (this.isFigure(localId)) {
            return;
        }
        ac.blockActivation(true);
        this.deadBlocked.add(localId);
        this.liftedAt.delete(localId);
        logLab("verb:containers", `body ${localId.toString(16)}: activation blocked, the loot is the server's container (${why}; ADR-067 decision 6)`);
    }

    // a native lift of the block in this tick (the promote, the release, the
    // fate's resurrect): the mark comes off and a pass two tick counts on
    // reads the body again
    private markLifted(localId: number): void {
        this.deadBlocked.delete(localId);
        this.liftedAt.set(localId, this.tick);
    }

    // the body is alive: the dead block's mark is dropped; on the seat that
    // hosts it the block itself comes off when this service laid it (the
    // engine's own Resurrect, a necromancer's reanimation: Hooks.cpp
    // HostedResurrect sets the AI alone), as the promote gives a live actor;
    // elsewhere the demote blocks it as a puppet
    private standsAgain(localId: number, ac: Actor, hostedHere: boolean): void {
        this.liftedAt.delete(localId);
        if (!this.deadBlocked.delete(localId)) {
            return;
        }
        if (hostedHere) {
            ac.blockActivation(false);
            logLab("verb:containers", `body ${localId.toString(16)}: stood up on its host's seat, its activation the engine's again`);
        }
    }

    private onDeathStart(e: DeathEvent) {
        const dying = e.actorDying ? this.sp.Actor.from(e.actorDying) : null;
        if (!dying) {
            return;
        }
        const localId = dying.getFormID();
        if (localId === 0x14) {
            return;
        }
        this.endAggro(localId, dying, "its death started here");
        this.seesDeath(localId);
        this.blockDeadBody(localId, dying, "its death started here");
    }

    // thuum docs/verbs/npc-puppet.md (Ownership; ADR-063, ADR-073 decision
    // 1): the actor's effects are its host's engine's, which runs them and
    // draws their looks; the host reports the effects whose look its engine
    // keeps (HostedStateService, the platform's ReadEffectLooks: active,
    // neither inactive nor dispelled, a hit shader or a hit effect art, a
    // constant effect or FX Persist), the server records the set on the
    // actor (R2) and relays it with the hosted state, and serves the record
    // to a seat that subscribes later. The last set per actor, by server id;
    // the actor's next pass (the view's update, the poll) plays it
    private onHostedState(event: ConnectionMessage<HostedActorStateMessage>) {
        const remote = this.controller.lookupListener(RemoteServer);
        const i = remote.getIdManager().getId(event.message.idx);
        const form = i >= 0 ? remote.getWorldModel().forms[i] : undefined;
        const refrId = form ? form.refrId : undefined;
        if (refrId) {
            this.noteRelayedTarget(refrId, event.message.combatTarget, remote.getMyRemoteRefrId());
        }
        const effects = event.message.effects;
        if (!Array.isArray(effects)) {
            return;
        }
        if (!refrId) {
            logTrace(this, `effects: no form here for idx ${event.message.idx}, the record's ${effects.length} effect(s) not kept`);
            return;
        }
        this.recordedEffects.set(refrId, effects.slice());
    }

    // tracker #130: the host's combat target per actor, by server id, kept
    // while it names anyone (pure JavaScript: this is the message's
    // context). Any change of it lifts a hold; a change to or from this
    // seat's player asks the actor's pass on the next update (the view's
    // update and the poll ask too), which starts or stops the aggro
    private noteRelayedTarget(refrId: number, combatTarget: unknown, me: number): void {
        const target = typeof combatTarget === "number" && Number.isFinite(combatTarget) ? combatTarget : 0;
        const before = this.relayedTargets.get(refrId) ?? 0;
        if (target === before) {
            return;
        }
        if (target) {
            this.relayedTargets.set(refrId, target);
        } else {
            this.relayedTargets.delete(refrId);
        }
        this.aggroHeldUntil.delete(refrId);
        if (me === 0 || (target !== me && before !== me)) {
            return;
        }
        const localId = remoteIdToLocalId(refrId);
        if (localId) {
            this.controller.once("update", () => this.reconcile(localId));
        }
    }

    // The record's looks on an actor this seat does not host: for each
    // recorded effect, its record's hit shader (MagicEffect.getHitShader,
    // EffectShader.play until stopped) and its hit effect art
    // (MagicEffect.getHitEffectArt, played through TESModPlatform.
    // ApplyArtObject, the engine's own TESObjectREFR::ApplyArtObject, until
    // stopped), each form once; a look whose effects all left the record is
    // stopped (EffectShader.stop, TESModPlatform.StopArtObject). Exactly the
    // record: an actor with none recorded shows none, so an effect the host's
    // engine holds inactive (SunAffectsNPCVampires' burn in a cave, R-V4)
    // shows nowhere. Played again on a new record or a new 3D only.
    private showEffectLooks(localId: number, ac: Actor): void {
        if (!this.effectLooksOn) {
            return;
        }
        const refrId = localIdToRemoteId(localId);
        const want = (refrId ? this.recordedEffects.get(refrId) : undefined) ?? PuppetService.noEffects;
        const shown = this.effectLooks.get(localId);
        if (shown && shown.from === want) {
            return;
        }
        const shaders = new Map<number, EffectShader>();
        const arts = new Map<number, Art>();
        for (const effectId of want) {
            const mgef = this.sp.MagicEffect.from(this.sp.Game.getFormEx(effectId));
            if (!mgef) {
                // ADR-073 decision 7: said once per kind, then counted
                const n = ++this.unresolvedEffects;
                if (n === 1 || (n & (n - 1)) === 0) {
                    logLab("verb:npc-puppet", `effects: ${effectId.toString(16)} on ${localId.toString(16)} is no magic effect in this load order (${n} so far): its look falls through`);
                }
                continue;
            }
            // thuum tracker #164 (playtest 55, Eli: "rotfern has a lingering
            // particle effect on ariadnes screen from catching brain rot"): a
            // hit shader stays only on an effect flagged FX Persist, SKSE's
            // FXPersist 0x00001000 (MagicEffect.psc). Brain Rot's
            // TrapDisDamageMagicka (0x10a251) is a constant effect whose one
            // look is the HealFXS shader without that flag, played here until
            // stopped (-1) while its owner's engine ran it once (HYPOTHESIS,
            // unread: nothing reads a shader on a reference). A constant
            // effect's hit art stays as before
            const shader = mgef.isEffectFlagSet(PuppetService.fxPersist) ? mgef.getHitShader() : null;
            if (shader) {
                shaders.set(shader.getFormID(), shader);
            }
            const art = mgef.getHitEffectArt();
            if (art) {
                arts.set(art.getFormID(), art);
            }
        }
        const id = localId.toString(16);
        const before = shown ?? { from: PuppetService.noEffects, shaders: new Set<number>(), arts: new Set<number>() };
        const now: EffectLooks = { from: want, shaders: new Set<number>(), arts: new Set<number>() };
        let played = 0;
        let stopped = 0;
        try {
            shaders.forEach((shader, shaderId) => {
                now.shaders.add(shaderId);
                if (before.shaders.has(shaderId)) {
                    return;
                }
                // a copy the engine kept across a 3D's reload would stack a
                // second one; a stop of a look not playing does nothing
                shader.stop(ac);
                shader.play(ac, -1);
                ++played;
                logTrace(this, `effects: ${id} plays shader ${shaderId.toString(16)}`);
            });
            arts.forEach((art, artId) => {
                if (before.arts.has(artId)) {
                    now.arts.add(artId);
                    return;
                }
                // a second play of the art replaces this native's first
                // (PapyrusTESModPlatform.cpp ApplyArtObject); false without a 3D
                if (this.sp.callNative("TESModPlatform", "ApplyArtObject", undefined, ac, art, -1) === true) {
                    now.arts.add(artId);
                    ++played;
                    logTrace(this, `effects: ${id} plays art ${artId.toString(16)}`);
                } else {
                    logTrace(this, `effects: ${id} took no art ${artId.toString(16)} (no 3D yet)`);
                }
            });
            before.shaders.forEach((shaderId) => {
                if (!now.shaders.has(shaderId)) {
                    this.sp.EffectShader.from(this.sp.Game.getFormEx(shaderId))?.stop(ac);
                    ++stopped;
                }
            });
            before.arts.forEach((artId) => {
                if (now.arts.has(artId)) {
                    return;
                }
                const art = this.sp.Art.from(this.sp.Game.getFormEx(artId));
                if (art) {
                    this.sp.callNative("TESModPlatform", "StopArtObject", undefined, ac, art);
                    ++stopped;
                }
            });
        } catch (err) {
            logError(this, `effects: the looks on ${id} failed`, err);
        }
        this.effectLooks.set(localId, now);
        if (played > 0 || stopped > 0) {
            logLab("verb:npc-puppet", `effects: ${id} shows the record's ${want.length} effect(s) (${want.length ? want.map((e) => e.toString(16)).join(", ") : "none"}): ${played} look(s) played, ${stopped} stopped, ${now.shaders.size} shader(s) and ${now.arts.size} art(s) on`);
        }
    }

    // every look this seat played on the actor, stopped: its engine draws
    // them from the promote on (the live actor's own effects), or it is gone
    private stopEffectLooks(localId: number, ac: Actor): void {
        const shown = this.effectLooks.get(localId);
        this.effectLooks.delete(localId);
        if (!shown) {
            return;
        }
        try {
            shown.shaders.forEach((shaderId) => {
                this.sp.EffectShader.from(this.sp.Game.getFormEx(shaderId))?.stop(ac);
            });
            shown.arts.forEach((artId) => {
                const art = this.sp.Art.from(this.sp.Game.getFormEx(artId));
                if (art) {
                    this.sp.callNative("TESModPlatform", "StopArtObject", undefined, ac, art);
                }
            });
        } catch (err) {
            logError(this, `effects: stopping the looks on ${localId.toString(16)} failed`, err);
        }
        if (shown.shaders.size > 0 || shown.arts.size > 0) {
            logLab("verb:npc-puppet", `effects: ${localId.toString(16)} is this seat's to run: ${shown.shaders.size} shader(s) and ${shown.arts.size} art(s) of the record stopped, its engine draws its own`);
        }
    }

    private forget(localId: number) {
        this.endAggro(localId, null, "no actor here");
        this.aggroRetryAt.delete(localId);
        this.demotedAt.delete(localId);
        this.deadSeen.delete(localId);
        this.puppets.delete(localId);
        this.effectLooks.delete(localId);
        this.attached.delete(localId);
        this.deferredKill.delete(localId);
        this.unloadedSince.delete(localId);
        this.scenesStopped.delete(localId);
        this.deadFlaggedSince.delete(localId);
        this.deadBlocked.delete(localId);
        this.liftedAt.delete(localId);
    }

    // An actor attaching to a cell is loaded here from this moment, whatever
    // its 3D says (ADR-050 decision 3's "loaded here from the attach"); its
    // state follows from the word.
    private onCellAttach(e: CellAttachDetachEvent) {
        if (!this.enabled || !e.refr) {
            return;
        }
        const ac = this.sp.Actor.from(e.refr);
        if (!ac || ac.getFormID() === 0x14) {
            return;
        }
        const id = ac.getFormID();
        this.attached.add(id);
        this.reconcile(id);
    }

    // A detached reference unloads; its reload is a fresh attach. The flag
    // stays until the grace releases it (an unloaded actor streams nothing
    // from here either way).
    private onCellDetach(e: CellAttachDetachEvent) {
        if (!e.refr) {
            return;
        }
        const id = e.refr.getFormID();
        this.attached.delete(id);
        this.controller.lookupListener(HostService).markHosted(id, false);
        this.reconcile(id);
    }

    // A puppet drafted into combat (a hit, a script's StartCombat; the AI bit
    // gates neither, re-ai-puppet.md:181-187): undone at the first event (H6)
    // by the demote again, which is idempotent and stops the combat natively.
    // Tracker #130's exception: a puppet with the aggro mark keeps its
    // combat whatever target its engine picks. Target selection runs with
    // the AI off and the combat behavior does not (re-ai-puppet.md:169-180,
    // confirmed static there), so a summon or a follower of this seat's that
    // engages the puppet may draw its target; that it never swings is the
    // same note's HYPOTHESIS (:185-187). HYPOTHESIS: that the puppet then
    // stays on this seat's compass and keeps the wait gate shut,
    // since both read every combat group with the player among its targets
    // (hostility-1-7-104.md:43-70, :92-97), not each member's own target.
    // The end of a marked puppet's combat takes the mark off: its engine
    // ended it, or the server's StopCombatAlarm did (reconcileAggro)
    private onCombatState(e: CombatEvent) {
        if (!e.actor) {
            return;
        }
        const id = e.actor.getFormID();
        if (!(e.isCombat || e.isSearching)) {
            if (this.aggro.has(id)) {
                // the server's end holds a new start itself; an end of the
                // engine's own waits aggroRetryMs
                const refrId = localIdToRemoteId(id);
                const held = refrId !== 0 && this.isAggroHeld(refrId);
                this.endAggro(id, null, held ? `the server's StopCombatAlarm ended it; a new start held ${PuppetService.aggroHoldMs} ms for its host's next state` : "its engine ended it", !held);
            }
            return;
        }
        if (!this.sp.isPuppet(id) || this.aggro.has(id)) {
            return;
        }
        this.sp.setPuppet(id, true);
        this.demotedAt.set(id, this.tick);
    }

    // A grant or a release reaches every actor as the server's word per actor
    // (isHostedByOther flipped under its id); each view's update runs
    // reconcile with it, persistent references included. The cell walk that
    // stood here (Cell.getNthRef over the cell's own references) missed them
    // and ran at the message's tick, outside the update
    // (b-npc-puppet 20261022-081631: 5 promoted of 23, "Game.getFormEx can't
    // be called in this context" 27 times).

    private readonly puppets = new Set<number>();
    // hosted actors held as puppets while another seat's stream owns their body
    // (reconcile): their promote places nothing (sync/hostPuppet.ts)
    private readonly heldForBody = new Set<number>();
    // tracker #130: the host's combat target per actor (by server id, none
    // kept for none); the moment a held start is free again (by server id);
    // the puppets this seat put in combat on its player (the aggro mark, by
    // local id, with the tick of the start); the moment a puppet's next
    // start is free after its engine dropped one; the tick of each puppet's
    // last demote; the deaths this seat has seen (seesDeath)
    private readonly relayedTargets = new Map<number, number>();
    private readonly aggroHeldUntil = new Map<number, number>();
    private readonly aggro = new Map<number, number>();
    private readonly aggroRetryAt = new Map<number, number>();
    private readonly demotedAt = new Map<number, number>();
    private readonly deadSeen = new Set<number>();
    // Ownership: the host's recorded effect set per actor, by server id, and
    // the looks playing on each actor this seat does not host, by local id
    private readonly recordedEffects = new Map<number, number[]>();
    private readonly effectLooks = new Map<number, EffectLooks>();
    private static readonly noEffects: number[] = [];
    // MagicEffect.IsEffectFlagSet's FX Persist, SKSE's MagicEffect.psc
    // (thuum tracker #164)
    private static readonly fxPersist = 0x00001000;
    // ADR-073 decision 7: recorded effects that are no MGEF here, counted
    private unresolvedEffects = 0;
    private readonly attached = new Set<number>();
    // the deferred kill's state per NPC, by local id, one record agreeing
    // with the engine's: "armed" by reconcile, "ended" by the server's kill
    // (deferredKillEnded), absent never armed, stood up by the word, or
    // simulated here since (tracker #128)
    private readonly deferredKill = new Map<number, "armed" | "ended">();
    // the moment a dead puppet was first seen dead here (the flag's settle)
    private readonly deadFlaggedSince = new Map<number, number>();
    private static readonly ragdollSettleMs = 4_000;
    // ADR-067 decision 6: the dead bodies this service has blocked (once per
    // load), the tick of each body's last native lift, and the tick count
    private readonly deadBlocked = new Set<number>();
    private readonly liftedAt = new Map<number, number>();
    private tick = 0;
    private readonly unloadedSince = new Map<number, number>();
    // playtest 41's Sven scene: the scene this seat last stopped on each puppet
    private readonly scenesStopped = new Map<number, number>();
    private lastPoll = 0;
    private static readonly pollMs = 2000;
    // deathService's RESURRECT_INFLIGHT_MS, the window a queued resurrect owns the actor's AI and block
    private static readonly resurrectInFlightMs = 2000;
    private readonly resurrectInFlightUntil = new Map<number, number>();
    // two polls: a 3D the late fate dropped comes back within it (train-47b
    // reloads it), and a walk out of reach is a release either way
    private static readonly releaseGraceMs = 4000;
    // tracker #130, policies of this client's, not engine numbers: a start
    // after the server's end waits one host poll (HostedStateService's
    // 500 ms) and a second more for the host's own end to run and its state
    // to come back; a start after the engine dropped one waits one poll
    // here, the pass that reads the engine
    private static readonly aggroHoldMs = 1500;
    private static readonly aggroRetryMs = 2000;
    private readonly enabled: boolean;
    private readonly engineDeaths: boolean;
    private readonly effectLooksOn: boolean;
}
