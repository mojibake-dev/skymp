import { Actor, Art, CellAttachDetachEvent, CombatEvent, DeathEvent, EffectShader, ObjectReference } from "skyrimPlatform";
import { logError, logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { HostedActorStateMessage } from "../messages/hostedActorStateMessage";
import { Movement } from "../../sync/movement";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { localIdToRemoteId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
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
// attach or the 3D (ADR-051: an actor this engine does not load is released
// to its schedule after a grace, so a 3D the late fate dropped for a moment
// does not flip a loaded actor live, playtest 34's composition). The
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
        // a create word starts the actor's record over here: the server
        // serves its recorded effects right behind it when it holds any
        // (MpActor::SendHostedEffectsTo), so a set kept from an earlier sight
        // of the actor never outlives an empty record
        this.controller.emitter.on("createActorMessage", (e) => {
            if (e.message.refrId !== undefined) {
                this.recordedEffects.delete(e.message.refrId);
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
        const word = mine ?? host.isMineLocal(localId);
        const has3D = ac.is3DLoaded();
        const loadedHere = has3D || this.attached.has(localId);
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
        const holdAtZero = !word || !loadedHere || !this.engineDeaths;
        if (holdAtZero && !this.deferredArmed.has(localId)) {
            ac.startDeferredKill();
            this.deferredArmed.add(localId);
        } else if (!holdAtZero && this.deferredArmed.has(localId)) {
            ac.endDeferredKill();
            this.deferredArmed.delete(localId);
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
                this.sp.releasePuppet(localId);
                this.puppets.delete(localId);
                // the release lifts the block natively (PuppetApi.cpp
                // ReleasePuppet: AI on, flag and block off), for a live actor's
                // schedule; a body comes back under the block at its next load
                // here (ADR-067 decision 6)
                this.markLifted(localId);
                logTrace(this, `released ${localId.toString(16)}: no 3D here, left to its schedule (npc-handoff D11)`);
            }
            return;
        }
        this.unloadedSince.delete(localId);

        if (word) {
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
                    : last
                    ? `placed from the last sample at (${last.pos[0].toFixed(0)}, ${last.pos[1].toFixed(0)}, ${last.pos[2].toFixed(0)}) in ${last.worldOrCell.toString(16)}`
                    : "no sample to place from, the engine's own stands";
                logLab("verb:npc-handoff", `promote ${localId.toString(16)}: ${from}; the engine had it at ${at}`);
            }
            if (last && !seat) {
                // the record's last point and heading first: the relay stops
                // at the grant (the native promote leaves the transform alone)
                ac.setPosition(last.pos[0], last.pos[1], last.pos[2]);
                ac.setAngle(last.rot[0], last.rot[1], last.rot[2]);
            }
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
        // a body that stood up on a seat that does not host it: the demote
        // below blocks it as a puppet, so the dead block's mark is dropped
        // (a later death lays it again)
        this.standsAgain(localId, ac, false);
        if (!has3D) {
            return;
        }
        if (!flagged) {
            if (!this.sp.setPuppet(localId, true)) {
                logError(this, `setPuppet(${localId.toString(16)}, true) found no actor`);
                return;
            }
            this.puppets.add(localId);
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
        this.deferredArmed.delete(localId);
        // the fate's resurrect runs on the game's main thread at its next UI
        // drain, unordered against this tick since fork cc5c82b6 (FateApi.cpp
        // resurrectActor: a puppet blocked, a live actor that stood up
        // unblocked, a body the call left dead kept blocked): the dead
        // block's mark is dropped, a later pass reads the body again, and
        // reconcile stands aside while the resurrect is in flight
        this.markLifted(localId);
        this.resurrectInFlightUntil.set(localId, Date.now() + PuppetService.resurrectInFlightMs);
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
        const effects = event.message.effects;
        if (!Array.isArray(effects)) {
            return;
        }
        const remote = this.controller.lookupListener(RemoteServer);
        const i = remote.getIdManager().getId(event.message.idx);
        const form = i >= 0 ? remote.getWorldModel().forms[i] : undefined;
        const refrId = form ? form.refrId : undefined;
        if (!refrId) {
            logTrace(this, `effects: no form here for idx ${event.message.idx}, the record's ${effects.length} effect(s) not kept`);
            return;
        }
        this.recordedEffects.set(refrId, effects.slice());
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
            const shader = mgef.getHitShader();
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
        this.puppets.delete(localId);
        this.effectLooks.delete(localId);
        this.attached.delete(localId);
        this.deferredArmed.delete(localId);
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
    // by the demote again, which is idempotent and stops the combat natively
    private onCombatState(e: CombatEvent) {
        if (!e.actor || !(e.isCombat || e.isSearching)) {
            return;
        }
        const id = e.actor.getFormID();
        if (!this.sp.isPuppet(id)) {
            return;
        }
        this.sp.setPuppet(id, true);
    }

    // A grant or a release reaches every actor as the server's word per actor
    // (isHostedByOther flipped under its id); each view's update runs
    // reconcile with it, persistent references included. The cell walk that
    // stood here (Cell.getNthRef over the cell's own references) missed them
    // and ran at the message's tick, outside the update
    // (b-npc-puppet 20261022-081631: 5 promoted of 23, "Game.getFormEx can't
    // be called in this context" 27 times).

    private readonly puppets = new Set<number>();
    // Ownership: the host's recorded effect set per actor, by server id, and
    // the looks playing on each actor this seat does not host, by local id
    private readonly recordedEffects = new Map<number, number[]>();
    private readonly effectLooks = new Map<number, EffectLooks>();
    private static readonly noEffects: number[] = [];
    // ADR-073 decision 7: recorded effects that are no MGEF here, counted
    private unresolvedEffects = 0;
    private readonly attached = new Set<number>();
    private readonly deferredArmed = new Set<number>();
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
    private readonly enabled: boolean;
    private readonly engineDeaths: boolean;
    private readonly effectLooksOn: boolean;
}
