import { ContainerChangedEvent, FormType, MotionType, ObjectReference, PropContactEvent, PropImpulseEvent, ScriptCall } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { PropOwnerMessage } from "../messages/propOwnerMessage";
import { PropTransformMessage } from "../messages/propTransformMessage";
import { FormTypeEx } from "../../extensions/formTypeEx";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { NiPoint3 } from "../../sync/movement";
import { FollowSample, FollowStep, PropFollow } from "../../sync/propFollow";
import { FormModel } from "../../view/model";
import { getViewFromStorage, localIdToRemoteId, remoteIdToLocalId } from "../../view/worldViewMisc";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { HostService } from "./hostService";
import { RemoteServer } from "./remoteServer";

const TAG = "verb:prop-physics";

// PropClaim's touch codes (wire-rules props.rs touch::*): a contact of the
// character controller, the engine's grab, a drop (a grabbed prop let go, or
// an item dropped from the inventory)
const TOUCH_CONTACT = 0;
const TOUCH_GRAB = 1;
const TOUCH_DROP = 2;
// a shout's or spell's impulse, and ADR-068 D-S3 point 4's: a trap body this
// seat's own script set moving (the server admits a world object that is no
// item for this touch alone)
const TOUCH_IMPULSE = 3;

// How often the tracked set is rebuilt from the view's references, ms
const RESCAN_MS = 500;
// A prop is tracked within this many units of the player, the nearest
// TRACK_MAX of them (the contact-ready set), and at any distance while its 3D
// is loaded in the player's world or cell when this seat owns it, follows its
// stream, claimed it moments ago or its own engine woke it here (physics rung
// 3, B5: the rescan's keeps)
const TRACK_RADIUS = 1024;
const TRACK_MAX = 64;
// game-file item references the server subscribed this seat to, remembered from their CreateActor (see engineRefs)
const ENGINE_REFS_MAX = 1024;
// A prop just tracked is left alone this long: the view's SpawnProcess places
// a server-made copy over a few frames (setPosition, enable, then its own
// setMotionType Keyframed), and a motion type set under that chain would be
// overwritten by its last step
const TRACK_GRACE_MS = 1000;
// The simulator's stream (ADR-058 decision 2; v2, ADR-071 decision 6): while
// the engine has the body awake (readBodies, its Havok island in the world's
// active list), a sample when it moved this far or turned this much since the
// last sample sent, or its hand changed, at most one per SEND_MIN_MS; one
// sample more, asleep and reliable, at the read where the engine reports its
// island deactivated (rest is the engine's, never a stir test: havok-props.md
// E42 read a still held body asleep by one)
const SEND_MIN_MS = 100;
const SAMPLE_UNITS = 2;
const SAMPLE_DEGREES = 2;
// The heartbeat (ADR-071's D-PH4 B): an awake body this seat streams sends a
// sample at least this often, moved or not, and a held one never sends the
// asleep sample (A6), so the server never reads a live stream stalled (its
// PROP_MOVING_MS, 1600: about three of these and a frame, so two lost beats in
// a row are no stall) and no follower's stream goes quiet (FOLLOW_QUIET_MS); a
// creeping body under SAMPLE_UNITS and a still hand both stay on the wire. A
// body the engine stops reporting awake while a stream is open, not by a fall
// the update's read saw (a hold's end over a body its engine had put to rest,
// a grant's body never woken), rests after this long without an awake read
const STREAM_HEARTBEAT_MS = 500;
// A sample's sequence within (prop, epoch) runs from 1 to this and wraps to 1
// (0 is the server's record answer, which no seat sends; the rule's serial
// number arithmetic, wire-rules props.rs newer)
const SEQ_MAX = 65535;
// The priority accumulator (ADR-071 decision 6): an update's samples go out
// within a mirror of the server's prop budget (wire-validate's prop_budget,
// 128 at once and 96 a second), held at 80 a second for margin, so E_VAL_RATE
// from this seat stays 0; the rests first and always (past the budget if need
// be, the debt repaid by the refill inside the server's margin), the rest by
// the time since each body's last sample weighted by its speed plus
// PRIORITY_SPEED_FLOOR (a still held body's heartbeat counts as a body this
// fast, game units a second), each sent sample's priority reset and the
// unsent carrying theirs to the next update (accessible: Fiedler, "State
// Synchronization", gafferongames.com, 2015; foundational: Frohnmayer and
// Gift, "The TRIBES Engine Networking Model", GDC 2000). UNMEASURED
const SEND_BUDGET_BURST = 128;
const SEND_BUDGET_PER_S = 80;
const PRIORITY_SPEED_FLOOR = 100;
// The wire's bounds on a sample (wire-validate check_prop_transform), kept
// here so no sample of this seat's is refused at the edge: each coordinate
// within WORLD_ABS_MAX, each angle within one turn, |lin| within the rule's
// speed cap (game units a second) and |ang| within 1000 radians a second (a
// faster read scaled down, its direction kept)
const WORLD_ABS_MAX = 1e6;
const LIN_MAX_UNITS_PER_S = 6000;
const ANG_MAX_RAD_PER_S = 1000;
// The follower (ADR-071 decision 3; sync/propFollow.ts): the copy shows the
// stream this far behind the owner's own clock, never ahead of it: the client
// setting propFollowDelayMs (so the lab sizes it by the relaunch files lever
// without a build; 0 to FOLLOW_DELAY_MAX_MS), three samples at the owner's
// SEND_MIN_MS by default, ADR-071 decision 3's "about 300 ms" (UNMEASURED)
const FOLLOW_DELAY_MS = 300;
const FOLLOW_DELAY_MAX_MS = 2000;
// The stream went quiet when the render clock runs two heartbeats past its
// newest awake sample (one lost beat is no quiet: the next comes a heartbeat
// later and the delay covers its flight), and a word naming another seat
// awaits that seat's first sample this long and the delay; D73's rest follows
const FOLLOW_QUIET_MS = 2 * STREAM_HEARTBEAT_MS;
// A segment the render clock entered late, or a target it already passed, is
// covered in at least this long, so no glide is a one-frame teleport; a
// translate's speed is never under GLIDE_MIN_SPEED; a target JUMP_UNITS or
// farther from the copy is a jump, within JUMP_MS (below), counted
const SEGMENT_MIN_MS = 50;
const GLIDE_MIN_SPEED = 8;
const JUMP_UNITS = 256;
// A contact's claim and an island member's go at most once per CLAIM_EVERY_MS
// per prop, and a prop claimed is kept tracked that long, wherever it is, for
// the word that answers it (physics rung 3, B5)
const CLAIM_EVERY_MS = 2000;
// Physics rung 3 (ADR-071 decision 4: the caster claims what its impact moved
// in one PropClaim per impact): a shout's cone raises one propImpulse per
// collidable it pushes, across ticks, under one impact number, so a cone's
// impact is claimed this long after its first event, in one claim (16
// references at most, PropClaim v2's); an explosion's and a projectile's
// impact is claimed on the update its event comes (one event per frame). The
// cone crosses the Riverwood Trader's cart in about 70 ms (1,536 units a
// second, x-shout-once's read, over its cabbages' 100 units). UNMEASURED:
// x-prop-impact's leg I2 reads the claim per cone. The events held between
// two updates are bounded
const CONE_BATCH_MS = 100;
const IMPULSES_MAX = 64;
// A drop's claim: the server's reference for an item this player dropped is
// matched by its base within this window and this distance of the player
const DROP_MATCH_MS = 3000;
const DROP_MATCH_UNITS = 512;
// "applied from the stream" is said once per prop per this
const APPLIED_LOG_MS = 10_000;
// D73 (docs/DECISIONS.md, ADR-058 decision 3 refined): a follower's body is
// Keyframed only while another seat streams it awake (its stream live on the
// render clock, FOLLOW_QUIET_MS, or a word naming that seat awaiting its
// first sample); then, and REST_DYNAMIC_MS after the render clock applied an
// asleep sample, it is Dynamic and may wake, so a kick or a grab acts on it
// at once. The quiet window is a heartbeat-based failure detector (accessible:
// Joshi, "HeartBeat", Patterns of Distributed Systems; foundational: Ongaro
// and Ousterhout, Raft, USENIX ATC 2014, section 5.6); with the delay it
// stands a gap between arrivals of FOLLOW_QUIET_MS and the delay, 1,300 ms by
// default (one lost beat), inside the server's PROP_MOVING_MS 1,600 (two).
// UNMEASURED
const REST_DYNAMIC_MS = 300;
// a jump's translate lands within this (the translate's speed is the distance over it), ms
const JUMP_MS = 100;
// ADR-068 D-S3 point 4 (a trap body by the seat whose firing moved it): the
// trap verb's tag for the trap body lines; the firing's window, a body this
// seat's script sets moving again within this long of its claim being the same
// firing (PhysicsTrap.fireTrap's SetMotionType, then its ApplyHavokImpulse a
// latent call later; UNMEASURED); how far a family reference's record must
// rest from this engine's copy to be a body a firing moved before this
// session (a settling hanging body moves a few units; UNMEASURED); the bodies
// held at once (a dungeon's handful)
const TRAP_TAG = "verb:traps";
const TRAP_FIRING_MS = 2000;
const TRAP_MOVED_UNITS = 16;
const TRAP_BODIES_MAX = 256;

// a place and a turn: game units, Euler degrees
type Pose = { pos: NiPoint3; rot: NiPoint3 };

// a Havok body as the platform reads it (readBodies, PuppetApi.cpp), by this
// engine's form id: pos and rot its 3D root's world transform (the drawn
// place, never the reference's: havok-props.md E21), lin game units a second,
// ang radians a second, motion Havok's type (1, 2, 3 and 6 dynamic, 4
// Keyframed, 5 Fixed), awake its island in the world's active list; island,
// when the read asked for it (physics rung 3), the island's other dynamic
// members by this engine's id and their bases' FormType numbers
type Member = { localId: number; formType: number };
type Body = Pose & { formId: number; lin: NiPoint3; ang: NiPoint3; motion: number; awake: boolean; island?: Member[] };

// a take-over waiting for its placement to land (takeOver): the newest
// sample's velocities to hand, its sequence, how far the copy jumped ahead
// to it, and when the Dynamic set goes
type HandOff = { lin: NiPoint3; ang: NiPoint3; seq: number; ahead: number; due: number };

// one sample an update's stream made due (stream), sent by sendDue within the
// budget: the read, whether it is the rest, and the line it says when sent
// (the stream's opening for an awake one, the rest's cause for an asleep one)
type Due = { prop: Prop; body: Body; asleep: boolean; line: string | undefined };

// one tracked prop, by the server's id
type Prop = {
    refr: number;        // the server's id, the wire's refr
    localId: number;     // this engine's reference for it (the view's binding)
    formIdx: number;     // its index in the world model, checked at each use
    cell: number;        // the engine's parent cell at the last rescan (the host rule's cell)
    distance: number;    // to the player at the last rescan
    trackedAt: number;   // 0 for a reference tracked in place (trackInPlace): no copy is being spawned, so no grace
    // the record's transform imposed once, before anything else (the login's
    // place, which the view's own apply may or may not have reached yet), and
    // again at a record answer
    placed: boolean;
    placing: boolean;
    placedAt: number;
    // a placement of a Dynamic body set it Keyframed first (E21): its
    // translate lands then, and REST_DYNAMIC_MS later its motion is decided
    // again (0 none)
    landsAt: number;
    simulating: boolean;           // this seat's body is Dynamic and streams
    effective: number | undefined; // the simulator last computed (0 the host, by fallback)
    motionAsked: MotionType | undefined;
    loaded3D: boolean;
    // the simulator's side
    lastSent: Pose | undefined;    // the last sample out, or the first read as a baseline
    lastSendAt: number;
    sentHeld: boolean;             // the last sample out said held
    awakeAt: number;               // the engine last read the body awake (or the stream opened, or a hand let go)
    streaming: boolean;            // awake samples went out since the last rest
    announce: boolean;             // a granted word named this seat: its first read goes out at once (see stream)
    handOff: HandOff | undefined;  // a take-over's velocities, handed once its placement landed
    // the hand: this seat's engine holds the body (Game.getPlayerGrabbedRef named it at the update's read, readHolds)
    held: boolean;
    heldAt: number;                // when the hold began (the grab's claim went then)
    holdLost: boolean;             // another seat's word came after the grab: this hand let go (see tick)
    // the follower's side
    follow: PropFollow | undefined; // the stream this seat follows (sync/propFollow.ts), until its end
    keyframeDue: boolean;          // a word named another seat: the copy goes Keyframed before any of its samples
    lastAppliedLog: number;
    lastClaimAt: number;
    restAt: number;                // the render clock applied the stream's asleep sample then, for the Dynamic set after it (0 none)
    // the "body" lines (propBodyReadLog): their next read (0 none), and whether the first track's is asked
    bodyLogDue: number;
    bodyTracked: boolean;
    badBodySaid: boolean;          // the platform read no body for it, or one the wire cannot carry: said once
    trap: boolean;                 // ADR-068 D-S3: a trap body (no item; see trapBodies)
};

// the server's last word on a prop: the player (0 the cell's host by
// default) and its epoch, and when it came (a hold reads it against its own
// start); no lease and no timer here (A1 of ADR-071 decision 2)
type Owner = { player: number; epoch: number; at: number };

// ADR-068 D-S3 point 4: a trap body this seat knows, by the server's id: a
// family reference this seat's own script set moving (the scriptCalls batch),
// one an owner word names (the firing seat's claim), or one whose record rests
// away from this engine's copy (a restart, a late join); never a trap's
// trigger (trapBodyRole). localId this engine's
// (a game-file reference's own id); claimedAt this seat's last claim; followed
// while another seat's firing streams it (setPropFollowed)
type TrapBody = { localId: number; claimedAt: number; followed: boolean };

// the shortest signed turn from heading b to heading a, in (-180, 180]
// (sync/movementApply.ts's)
const angleDelta = (a: number, b: number): number => ((((a - b + 180) % 360) + 360) % 360) - 180;

// The service's one clock, ms: Node's performance.now() where Skyrim
// Platform's embedded Node exposes it (monotonic; HYPOTHESIS, the rung 2
// plan's section 5), else Date.now(). A sample's tMs (PropTransform v2, the
// owner's clock at the read) is its read on this clock counted from the
// client's start (performance.now()'s own origin, else this module's load),
// whole milliseconds, so it never wraps inside 49 days of a session; a
// follower stamps arrivals on it too, the render clock's one base
const perf = (globalThis as unknown as { performance?: { now?: () => number } }).performance;
const clockMs = (): number => (perf !== undefined && typeof perf.now === "function" ? perf.now() : Date.now());
const CLOCK_START_MS = perf !== undefined && typeof perf.now === "function" ? 0 : Date.now();
const tMsOf = (now: number): number => Math.max(0, Math.floor(now - CLOCK_START_MS)) % 4294967296;

// a vector's length, and the vector within max of it (its direction kept,
// a component that is not a number taken as 0; scaled to just under max, so
// the wire's own single-precision test of the bound passes)
const lengthOf = (v: NiPoint3): number => Math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
const bounded = (v: NiPoint3, max: number): NiPoint3 => {
    const w: NiPoint3 = [isFinite(v[0]) ? v[0] : 0, isFinite(v[1]) ? v[1] : 0, isFinite(v[2]) ? v[2] : 0];
    const n = lengthOf(w);
    if (n <= max) {
        return w;
    }
    const k = (max * 0.999) / n;
    return [w[0] * k, w[1] * k, w[2] * k];
};

// a triple for a lab line, one decimal each
const triple = (v: NiPoint3): string => `${v[0].toFixed(1)}, ${v[1].toFixed(1)}, ${v[2].toFixed(1)}`;

// thuum docs/verbs/prop-physics.md (ADR-058; Eli: "the canon perfection of
// multiplayer phys objects is gmod"): a physics prop (a basket, a dropped
// sword: an item reference the server knows) is simulated by ONE seat at a
// time and shown by the stream on every other. The simulator's body is
// Dynamic and PropTransform v2 (ADR-071 decision 6, physics rung 2) carries
// its Havok body's place, velocities and hand, under the owner epoch of the
// word this seat holds and its own clock, at 100 ms while it moves and once
// more when the engine deactivates it (R2: this seat's Havok computes, the
// server records); every other seat's copy rests Dynamic as the game files
// make it (D73; physics rung 3 retired the Keyframing on arrival, since
// another seat's cause is zeroed at the impulse site) and goes Keyframed while
// another seat streams it, taking the stream FOLLOW_DELAY_MS behind the
// owner's clock (sync/propFollow.ts), so it neither falls nor fights it, and
// a seat a word makes the simulator mid-flight takes the body over with the
// stream's last velocities. Who simulates is the server's word alone
// (PropOwner, R0; A1 to A7 of ADR-071 decision 2, physics rung 1): the player
// the last word named, or the cell's host when the word names nobody (player
// 0: a return at rest, a departure, an unload) or none came (HostService); no
// timer here hands a prop back (playtest 41: the lease mirror demoted owners
// mid-flight), and an answer repeating the standing word (an equal epoch)
// replaces it. The touch is what passes the simulation (PropClaim): the
// player's character controller meeting the body (the fork's propContact
// event: FridaHooks.cpp's hook on the proxy controller's contact callback,
// Address Library 79131, one event per reference per 250 ms), held back while
// another seat streams the body awake (A3); the engine's grab, read from the
// engine once per update (Game.getPlayerGrabbedRef, ADR-071's D-PH3 A), its
// claim sent while the engine names the body and its end the first sample
// that says held false; a drop; and (physics rung 3, ADR-071 decision 4) an
// impulse this seat's own cause gave, the platform's propImpulse, claimed in
// one PropClaim per impact by touch 3, and the resting bodies a moving body
// of this seat's knocks into its island, by the same touch. An awake body
// streams at least every STREAM_HEARTBEAT_MS and a held one never rests on
// the wire; the server's record answer (seq 0) to a refused sample places the
// copy at the record. The engine is read through readBodies (rule 9: one call
// an update, a resting body answered only at its fall), never the reference.
// Items (FormTypeEx.isItem's types) and trap bodies are the rows: a static and
// a ragdoll are rungs 4 and 5's, pushed meanwhile by their own engine's cause
// alone (D-PH5). The tracked set is the nearest TRACK_MAX within TRACK_RADIUS
// of the player and the rescan's keeps at any distance (TRACK_RADIUS's note).
// Everything the engine is asked happens on the update (a message handler
// queues; Skyrim Platform refuses natives in its context).
//
// HYPOTHESIS, until x-prop-push and x-prop-pass: that setMotionType flips a
// clutter body between Keyframed and Dynamic cleanly both ways (Dynamic
// without the wake, so the host's props rest as the file's clutter does until
// a contact wakes them); that the contact callback fires on this seat for a
// Keyframed body the player walks into (the re-analyst's static read, thuum
// docs/research/m3/re-havok-contact.md; lab/frida/prop-contact.js is its
// trace), so a non-host's walk into a basket claims it; the grab and the
// drop are the slice's sure touches. HYPOTHESIS (physics rung 3, the rung 3
// plan's B10 rows): that propImpulse names every body this seat's own cause
// pushed and no other seat's (x-prop-impact v1's leg I1); that a cone's events
// of one impact fall inside CONE_BATCH_MS; that a member of a moving body's
// island read awake is a body this seat's engine knocked (H4's facts, the
// claim rule x-prop-impact's leg I2 reads). HYPOTHESIS (physics rung
// 1): that Game.getPlayerGrabbedRef names the body for exactly as long as the
// engine's grab holds it, from StartGrabObject to DestroyMouseSprings (the
// player's grabbedObject, CommonLibSSE-NG P/PlayerCharacter.h:558 and :697;
// thuum docs/engine/havok-props.md E40 and E41 read the grab's ends, never
// this native), and never names a body a refused grab closed in its own
// frame (E43); and that a held body this seat sets Keyframed is let go by the
// engine, as a grab on a Keyframed body closes at once (E28, read at a grab's
// start, never mid-hold). HYPOTHESIS (physics rung 2, the plan's section 9
// rows): that the render clock three samples behind reads as the simulator's
// motion (nothing predicts, ADR-058 decision 3; x-prop-follow v1); that
// readBodies' awake bit falls at the engine's deactivation and a held body's
// island stays awake (x-body-read v1's leg A, havok-props.md E10); that the
// 3D root's XYZ Euler is the reference's angle at rest (x-body-read's P0);
// that a Dynamic body set Keyframed before a keyframed translate moves with
// its reference (x-body-read's leg B, E21's remedy); that a Keyframed copy
// set Dynamic starts at rest and the velocity set after it carries the
// motion (x-prop-follow's leg D).
//
// ADR-068 D-S3 point 4 (a trap body by the seat whose firing moved it;
// docs/research/trap-slice-d-s3-2026-11-05.md): every seat runs the trap
// family itself, so a falling rock (PhysicsTrap: SetMotionType Dynamic, then
// ApplyHavokImpulse) is set moving by each seat's own script. A seat whose
// script did (the scriptCalls batch, through ScriptCallService) claims the
// body by touch 3 once per firing; the server grants the first firing (a later
// claim on the moving body reads E_PROP_MOVING), and the owner streams the
// body as it streams an item. A seat whose word names another seat follows:
// setPropFollowed (this seat's own script's motion calls on the body run
// neutralized, Keyframed and a zero impulse) and the body Keyframed until that
// seat's at-rest sample, then its own again. A family reference whose record
// rests away from this engine's copy (a restart, a late join) is placed at its
// record. Trap bodies are tracked at any distance in the player's world or
// cell and past the item cap, take no grace, and are never claimed by a
// contact, a grab or a drop (the server admits touch 3 alone for them). A
// trap's trigger is never a trap body (trapBodyRole; playtest 48): its own
// script's motion calls claim nothing and a word on it is never followed, so
// each seat's own copy moves its own (a tripwire's rope, a bone alarm's
// bones), while the rocks it fires stay the firing seat's streamed bodies.
// HYPOTHESIS H-AW (the neutral arguments hold a followed body) and H-KF (a
// Keyframed follower rock may never hurt its own seat's player), the design's.
export class PropService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("propTransformMessage", (e) => this.onTransform(e));
        this.controller.emitter.on("propOwnerMessage", (e) => this.onOwner(e));
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
        this.controller.on("propContact", (e) => this.onPropContact(e));
        this.controller.on("propImpulse", (e) => this.onPropImpulse(e));
        this.controller.on("containerChanged", (e) => this.onContainerChanged(e));
        this.controller.on("update", () => this.onUpdate());
    }

    // Whether this service places the engine reference: the view skips the
    // record's movement apply for it (formView.ts applyAll), since the record
    // is the login's place and the body is this seat's Havok or the stream's
    placesIt(localId: number): boolean {
        return this.byLocal.has(localId);
    }

    // ADR-068 D-S3 point 4: ScriptCallService hands over the scriptCalls
    // batch's SetMotionType and ApplyHavokImpulse calls (the platform batches
    // them for a trap family reference no other seat streams; a followed
    // body's run neutralized and never reach the batch). JS state alone (the
    // batch's context allows no game call); the claim goes from the update
    noteTrapMotion(localId: number, fn: string, args: ScriptCall["args"]) {
        if (localId === 0) {
            return;
        }
        const motionType = fn === "SetMotionType" && typeof args[0] === "number" ? args[0] : undefined;
        this.trapMotions.push({ localId, motionType, impulse: fn === "ApplyHavokImpulse" });
        if (this.trapMotions.length > 64) {
            this.trapMotions.shift();
        }
    }

    // A sample from the prop's simulator, relayed, or the server's record
    // answer to this seat's refused sample (seq 0): pure JavaScript here (the
    // apply waits for the update). The record in this seat's model follows
    // every sample, tracked or not, so the view places an untracked prop by
    // it and a prop tracked later starts from it. A relayed sample joins the
    // stream this seat follows at its arrival, which feeds that stream's clock
    // alone (sync/propFollow.ts); one below the epoch this seat holds is a late
    // packet of an earlier tenure (the reliable word overtook the unreliable
    // sample; the server relays current samples alone) and is dropped. An
    // awake relayed sample of a prop this seat does not track waits for the
    // update, which tracks the prop if its reference is loaded here and feeds
    // it then (physics rung 3, B5: a watcher past TRACK_RADIUS follows the
    // caster's stream; trackStreams)
    private onTransform(e: ConnectionMessage<PropTransformMessage>) {
        const m = e.message;
        const now = clockMs();
        this.recordSample(m.refr, { pos: [m.pos[0], m.pos[1], m.pos[2]], rot: [m.rot[0], m.rot[1], m.rot[2]] });
        const record = m.seq === 0;
        // ADR-068 D-S3: a followed trap body's at-rest sample ends its follow,
        // after this update's apply (restTrapBodies), tracked or not
        if (!record && m.asleep && this.trapBodies.get(m.refr)?.followed === true) {
            this.trapRests.push(m.refr);
            if (this.trapRests.length > 64) {
                this.trapRests.shift();
            }
        }
        const prop = this.props.get(m.refr);
        if (!prop) {
            if (!record && !m.asleep) {
                this.streamsUntracked.push({ message: m, at: now });
                if (this.streamsUntracked.length > IMPULSES_MAX) {
                    this.streamsUntracked.shift();
                }
            }
            return;
        }
        this.feed(prop, m, now, record);
    }

    // A sample of a tracked prop, at its arrival `now` (onTransform's, or the
    // update's for a sample that waited for its prop's tracking): the record
    // answer, or the stream this seat follows
    private feed(prop: Prop, m: PropTransformMessage, now: number, record: boolean) {
        if (record) {
            // ADR-071 decision 6: the record answer, after the word on the one
            // reliable channel (PartOne::AnswerPropRecord), is a place this
            // seat puts its copy, simulated or followed (the server answers no
            // live owner's own stale sample, whose body runs ahead of the
            // record: ActionListener OnPropTransform). The stream followed
            // ends, and the next update places the body at the record just
            // recorded (placeAtRecord)
            this.endFollow(prop);
            prop.placed = false;
            prop.placing = false;
            logTrace(this, `prop ${m.refr.toString(16)} answered with its record at epoch ${m.epoch}: placed there`);
            return;
        }
        if (prop.simulating) {
            // the server relays to the other listeners alone (ActionListener
            // OnPropTransform skips the sender), so another seat's sample here
            // means a word this seat has not heard yet: every owner change is
            // a word (A4, A7), and this seat's refused samples are answered
            // with the current one (A5), whose apply flips the body to follow;
            // said once per prop
            if (!this.echoSaid.has(m.refr)) {
                this.echoSaid.add(m.refr);
                logError(this, `a sample for prop ${m.refr.toString(16)} this seat simulates arrived from the server; ignored`);
            }
            return;
        }
        const word = this.owners.get(m.refr);
        const known = word ? word.epoch : 0;
        if (m.epoch < known) {
            return;
        }
        // the seat the word names at the sample's epoch (0 the cell's host),
        // none yet when the sample came ahead of its word
        const seat = m.epoch === known ? (word ? word.player : 0) : undefined;
        const sample: FollowSample = {
            epoch: m.epoch,
            seq: m.seq,
            tMs: m.tMs,
            pos: [m.pos[0], m.pos[1], m.pos[2]],
            rot: [m.rot[0], m.rot[1], m.rot[2]],
            lin: [m.lin[0], m.lin[1], m.lin[2]],
            ang: [m.ang[0], m.ang[1], m.ang[2]],
            asleep: m.asleep,
            held: m.held,
        };
        const f = prop.follow;
        if (f !== undefined) {
            const kind = f.classify(sample, seat);
            if (kind === "this") {
                f.take(sample, seat, now);
                return;
            }
            if (kind === "stale") {
                return;
            }
            // another clock: the stream before it ends here
            this.endFollow(prop);
        }
        prop.follow = new PropFollow(sample, seat, now);
        prop.restAt = 0;
    }

    // The server's word on who simulates a prop from now; an epoch below the
    // known one is a late word (one counter per prop, the server's) and an
    // equal one replaces (A5: the answer to a refused claim or sample repeats
    // the standing word, so a seat that applied it wrongly heals). The body's
    // motion type follows on the update; this seat's samples carry the word's
    // epoch from here, their sequence from 1 at a new one
    private onOwner(e: ConnectionMessage<PropOwnerMessage>) {
        const m = e.message;
        const known = this.owners.get(m.refr);
        if (known && m.epoch < known.epoch) {
            logTrace(this, `owner word for prop ${m.refr.toString(16)} at epoch ${m.epoch} ignored: epoch ${known.epoch} known`);
            return;
        }
        this.owners.set(m.refr, { player: m.player, epoch: m.epoch, at: clockMs() });
        const sent = this.sent.get(m.refr);
        if (sent !== undefined && sent.epoch < m.epoch) {
            this.sent.delete(m.refr);
        }
        const mine = m.player !== 0 && m.player === this.myId();
        logLab(TAG, `prop ${m.refr.toString(16)} owner ${m.player.toString(16)} epoch ${m.epoch} (${m.player === 0 ? "the host" : mine ? "mine" : "theirs"})`);
        // ADR-068 D-S3: a word on a trap body is read on the update (its family
        // test is a game read, and a message handler may make none)
        this.trapWords.push(m.refr);
        if (this.trapWords.length > 64) {
            this.trapWords.shift();
        }
    }

    // The player's own actor is created: a fresh table on the server's side
    // of this connection (a login, a reconnect, a restart), so the words and
    // their epochs clear (HostService's rule for its grants), and with them the
    // streams followed under them. The sequences this seat sent stay: a
    // reconnect's server still holds each row's last one at its epoch, and a
    // restarted one holds none, so either takes the next
    // Game-file item references the server subscribed this seat to, keyed by
    // the engine's id (a game-file reference's record id is its engine id),
    // with the record's pose from the CreateActor (the login's place). The
    // view never models them: RemoteServer.skipFormViewCreation sends every
    // reference below 0xff000000 that is no DOOR and no NPC_ to onceLoad and
    // dealWithRef (Keyframed) and makes no FormModel and no FormView, so a
    // rescan over the view's forms alone finds no clutter at all (x-prop-push
    // 133010 and x-prop-pass 133232 at the union's stage: zero PropService
    // lines on any seat across ten runs, the server untouched). Bounded; an
    // entry whose reference is unloaded or far is simply not found at a
    // rescan; the server's unsubscribe needs no handling here
    private engineRefs = new Map<number, { baseId: number; pos: NiPoint3; rot: NiPoint3; worldOrCell: number }>();

    private onCreateActor(e: ConnectionMessage<CreateActorMessage>) {
        if (!e.message.isMe) {
            this.noteEngineRef(e.message);
            return;
        }
        this.owners.clear();
        this.echoSaid.clear();
        this.props.forEach((prop) => {
            prop.effective = undefined;
            this.endFollow(prop);
        });
    }

    private noteEngineRef(msg: CreateActorMessage) {
        const refrId = msg.refrId;
        if (refrId === undefined || refrId >= 0xff000000 || !msg.transform) {
            return;
        }
        if (msg.baseRecordType === "DOOR" || msg.baseRecordType === "NPC_") {
            return;
        }
        if (!this.engineRefs.has(refrId) && this.engineRefs.size >= ENGINE_REFS_MAX) {
            const oldest = this.engineRefs.keys().next().value;
            if (oldest !== undefined) {
                this.engineRefs.delete(oldest);
            }
        }
        const t = msg.transform;
        const pos: NiPoint3 = [t.pos[0], t.pos[1], t.pos[2]];
        const rot: NiPoint3 = [t.rot[0], t.rot[1], t.rot[2]];
        this.engineRefs.set(refrId, { baseId: msg.baseId || 0, pos, rot, worldOrCell: t.worldOrCell });
        // ADR-068 D-S3 point 4: a trap body's create word brings the server's
        // record (after a restart, at a re-subscription): placed at it on the
        // next tick, unless this seat streams the body (its record is this
        // seat's own samples)
        // not while this seat's own firing of the body is fresh: its grant may
        // still be on the way, and a re-place would yank a falling rock (the
        // platform-and-client review, finding 5)
        const prop = this.props.get(refrId);
        const body = this.trapBodies.get(refrId);
        const firing = body !== undefined && clockMs() - body.claimedAt < TRAP_FIRING_MS;
        if (prop && prop.trap && !prop.simulating && !firing) {
            prop.placed = false;
        }
    }

    // The player's character controller met a prop's Havok body (the fork's
    // hook on the proxy controller's contact callback, docs/verbs/
    // prop-physics.md the touch; one event per reference per 250 ms). Ids
    // alone here (the event's context allows no game call); the claim goes
    // from the update, for a tracked prop this seat does not simulate
    private onPropContact(e: PropContactEvent) {
        if (!this.enabled()) {
            return;
        }
        this.contacts.push({ localId: e.refrId, layer: e.layer, motionType: e.motionType });
        if (this.contacts.length > 32) {
            this.contacts.shift();
        }
    }

    // Physics rung 3 (ADR-071 decision 4, "caster only"): an impulse this
    // seat's own cause gave pushed bodies (the platform's propImpulse, raised
    // at the impulse frame's leave; another seat's cause is zeroed there and
    // raises nothing). Ids alone here (the event's context allows no game
    // call); the impact's claim goes from the update (claimImpulses)
    private onPropImpulse(e: PropImpulseEvent) {
        if (!this.enabled()) {
            return;
        }
        this.impulses.push({ impact: e.impact, kind: e.kind, cause: e.cause, refrs: e.refrs.map((r) => ({ localId: r.refrId, formType: r.formType })) });
        if (this.impulses.length > IMPULSES_MAX) {
            this.impulses.shift();
        }
    }

    // An item left the player's inventory for the world (oldContainer the
    // player, no new container, a reference made): the engine's own dropped
    // reference is deleted by DropItemService and the server's reference for
    // the drop arrives as a copy; the rescan matches that copy by its base
    // within DROP_MATCH_MS and claims it by touch 2, so the drop falls on the
    // dropper's seat from its first frame (ADR-058 decision 4). The event's
    // reference is not required: Skyrim Platform leaves it undefined when the
    // engine's event carries an empty handle, which a drop's does, so the
    // guard that asked for it recorded no drop at all and no claim ever left
    // a seat (x-prop-drop v3 143937 and v4 150222: the copy tracked, the
    // drops list empty); DropItemService's own gate accepts the same event
    private onContainerChanged(e: ContainerChangedEvent) {
        if (!this.enabled()) {
            return;
        }
        // DropItemService's own gate: a drop is made from the inventory menu;
        // the login's outfit removals have the same event shape (the player's
        // container, no new one, no reference) and would be recorded as drops
        // (x-prop-drop v4 162049: eight "drop of base ... recorded" lines per
        // seat at the login), and a server copy of one of those bases arriving
        // within DROP_MATCH_MS would be claimed wrongly
        if (!this.sp.Ui.isMenuOpen("InventoryMenu")) {
            return;
        }
        const player = this.sp.Game.getPlayer();
        if (!player || !e.oldContainer || !e.baseObj) {
            return;
        }
        if (e.newContainer !== null && e.newContainer !== undefined) {
            return;
        }
        if (e.oldContainer.getFormID() !== player.getFormID()) {
            return;
        }
        const baseId = e.baseObj.getFormID();
        this.drops.push({ baseId, at: clockMs() });
        if (this.drops.length > 16) {
            this.drops.shift();
        }
        logTrace(this, `drop of base ${baseId.toString(16)} recorded for the copy's match`);
    }

    private onUpdate() {
        this.applyImpulseRule();
        if (!this.enabled()) {
            if (this.props.size > 0) {
                this.untrackAll();
            }
            this.trapMotions = [];
            this.trapWords = [];
            this.trapRests = [];
            this.impulses = [];
            this.impactBatches.clear();
            this.streamsUntracked = [];
            return;
        }
        const now = clockMs();
        // ADR-068 D-S3's trap bodies first: a firing's claim, and a word's
        // follow made Keyframed, before the rescan keeps them and the ticks
        // stream or follow them
        if (this.trapMotions.length > 0) {
            this.claimTrapMotions(now);
        }
        if (this.trapWords.length > 0) {
            this.followTrapWords(now);
        }
        // physics rung 3: the impacts this seat's own cause gave, claimed and
        // tracked in place, and the streams of untracked props this seat's
        // reference stands loaded for, tracked and fed, before the rescan
        // keeps them
        if (this.impulses.length > 0 || this.impactBatches.size > 0) {
            this.claimImpulses(now);
        }
        if (this.streamsUntracked.length > 0) {
            this.trackStreams();
        }
        if (now - this.lastRescan >= RESCAN_MS) {
            this.lastRescan = now;
            this.rescan(now);
        }
        if (this.props.size > 0) {
            this.readHolds(now);
        }
        if (this.contacts.length > 0) {
            this.claimContacts(now);
        }
        const player = this.props.size > 0 ? this.sp.Game.getPlayer() : null;
        if (player) {
            const myId = this.myId();
            const bodies = this.readSimulated();
            // the islands this seat's moving bodies are in, read with them
            this.claimIslands(bodies, now, myId);
            for (const prop of Array.from(this.props.values())) {
                this.tick(prop, now, myId, bodies);
            }
            // the samples the ticks made due, within the budget
            this.sendDue(now);
        }
        // a followed trap body's at-rest sample, applied by the ticks above
        if (this.trapRests.length > 0) {
            this.restTrapBodies();
        }
    }

    // One prop, one update: the simulator computed from the owner table, the
    // body's motion type set to match (once, and again when its 3D reloads),
    // then the stream out (from the update's read of the simulated bodies,
    // `bodies`) or the stream's render
    private tick(prop: Prop, now: number, myId: number, bodies: Map<number, Body>) {
        // a trap body is a game-file reference in place, no copy being
        // spawned, and a firing's first second is its fall: no grace
        if (!prop.trap && now - prop.trackedAt < TRACK_GRACE_MS) {
            return;
        }
        const owner = this.ownerOf(prop, myId);
        const word = this.owners.get(prop.refr);
        // a word naming nobody names no simulator: a seat that does not host
        // reads owner 0 then, which is no match for player 0 (the review of
        // b7151a42: 0 === 0 hid the fallback line below on every such seat)
        const named = word !== undefined && word.player !== 0 && word.player === owner;
        if (owner !== prop.effective) {
            if (!named && prop.effective !== undefined) {
                // the cell's host by default: no word yet, or the word named
                // nobody (a return at rest, a departure, an unload); the cell
                // is this seat's read of the copy's parent cell, the host
                // rule's (a prop first tracked under the host's default says
                // nothing; the word's own line is onOwner's)
                logLab(TAG, `prop ${prop.refr.toString(16)} owner ${owner.toString(16)} epoch ${word ? word.epoch : 0} (host of cell ${prop.cell.toString(16)})`);
            }
            // ADR-071 decision 3: a word naming another seat makes the copy
            // Keyframed before any sample of that seat's, so its stream never
            // meets a Dynamic copy mid-flight
            if (named && owner !== myId) {
                prop.keyframeDue = true;
            }
            prop.effective = owner;
        }
        // A4's rest reaches a follower as two messages, the owner's asleep
        // sample (relayed) and the word (player 0, or this seat's own grant),
        // often in one update, and the render clock reaches the rest a delay
        // later; flip ends the stream followed, so a seat the word makes the
        // simulator follows on until REST_DYNAMIC_MS after the render clock
        // applied the rest, and its copy takes the rest pose before it goes
        // Dynamic (the review of b7151a42: the host's copy went Dynamic at the
        // last awake pose, the rest never applied). A copy following a live
        // stream is taken over at once (flip, takeOver)
        const restPending = prop.follow !== undefined && prop.follow.restPending();
        const simulate = myId !== 0 && owner === myId
            && (prop.simulating || (!restPending && (prop.restAt === 0 || now - prop.restAt >= REST_DYNAMIC_MS)));
        // A6: a hand whose grab another seat's later word overrides (a double
        // grab lost, the server's Held answer naming the holder) lets go, so
        // no two hands move one prop (ADR-063 decision 3)
        const lostHold = prop.held && !prop.holdLost && word !== undefined && word.at > prop.heldAt
            && word.player !== 0 && word.player !== myId;
        // D73's rest: a follower's Keyframed body goes Dynamic REST_DYNAMIC_MS after
        // the render clock applied the asleep sample, or once the stream went
        // quiet, so a kick or a grab acts on it at once; the check below needs
        // the reference, so the tick must reach it (x-prop-grab v8 202618: with
        // no pending sample the tick returned here every time, no seat ever
        // logged "at rest: Dynamic", and c2's E on the still-Keyframed basket
        // picked it up instead of grabbing it); a stream followed is rendered
        // every update until its end
        // (a followed trap body is held Keyframed until its owner's at-rest
        // sample, not let go when the stream goes quiet: D-S3)
        const restDue = !simulate && prop.placed && prop.landsAt === 0 && prop.motionAsked === MotionType.Keyframed
            && ((prop.restAt !== 0 && now - prop.restAt >= REST_DYNAMIC_MS)
                || (!this.followedTrap(prop) && !restPending && !this.followLive(prop, now)));
        // E21's placement of a Dynamic body: its motion decided again
        // REST_DYNAMIC_MS after its translate landed
        const landed = prop.landsAt !== 0 && now - prop.landsAt >= REST_DYNAMIC_MS;
        const bodyLogDue = prop.bodyLogDue !== 0 && now >= prop.bodyLogDue;
        const wantsRef = simulate || !prop.placed || prop.simulating !== simulate || prop.motionAsked === undefined
            || prop.follow !== undefined || prop.keyframeDue || restDue || lostHold || landed || bodyLogDue;
        if (!wantsRef) {
            return;
        }
        const refr = this.refOf(prop);
        if (!refr) {
            this.untrack(prop, "its reference is gone");
            return;
        }
        // a body whose 3D reloaded (a cell's re-attach) has the file's motion
        // type again, so it is set again
        const loaded3D = refr.is3DLoaded();
        if (loaded3D && !prop.loaded3D) {
            prop.motionAsked = undefined;
        }
        prop.loaded3D = loaded3D;
        if (!prop.placed) {
            this.placeAtRecord(prop, refr, now);
            return;
        }
        if (landed) {
            prop.landsAt = 0;
            prop.motionAsked = undefined;
        }
        if (bodyLogDue) {
            prop.bodyLogDue = 0;
            this.logBody(prop, refr);
        }
        if (lostHold) {
            // the copy goes Keyframed (wake false: it follows the holder's
            // stream from here), which the engine's grab cannot keep (the
            // header's HYPOTHESIS); the hold's end is the engine's next read,
            // and no release claim goes (the prop is another seat's)
            prop.holdLost = true;
            if (prop.loaded3D) {
                this.setMotion(prop, refr, MotionType.Keyframed, false);
            }
            logLab(TAG, `prop ${prop.refr.toString(16)}: another seat owns it while this hand held it; the hold ends`);
        }
        if (prop.simulating !== simulate) {
            // a word that names this seat is a touch granted: the body wakes
            // (a drop's copy may stand in the air, a passed prop mid-motion);
            // the host's default clutter rests until a contact wakes it
            this.flip(prop, refr, simulate, simulate && named, now);
        } else if (loaded3D && prop.motionAsked === undefined) {
            this.setMotion(prop, refr, this.restMotion(prop, simulate, now), true);
        }
        if (prop.keyframeDue) {
            prop.keyframeDue = false;
            if (!prop.simulating && prop.loaded3D && prop.motionAsked !== MotionType.Keyframed) {
                this.setMotion(prop, refr, MotionType.Keyframed, true);
            }
        }
        if (prop.simulating) {
            this.stream(prop, refr, now, bodies.get(prop.localId));
        } else {
            this.follow(prop, refr, now);
        }
    }

    // Who simulates the prop as this seat reads the table (A1: by the
    // server's word alone): the player the last word named; with no word, or a
    // word naming nobody (player 0, the host by default), the cell's host,
    // this seat when it hosts the prop's cell and 0 (another seat's, or
    // nobody's: an unhosted cell's prop freezes) otherwise. No lease, no
    // silence and no presence test here: each of those demoted an owner the
    // server still named (playtest 41)
    private ownerOf(prop: Prop, myId: number): number {
        const word = this.owners.get(prop.refr);
        if (word && word.player !== 0) {
            return word.player;
        }
        return this.controller.lookupListener(HostService).hostsCell(prop.cell) ? myId : 0;
    }

    // The record's transform (CreateActor's, or the last sample recorded
    // here) imposed on the body before it is simulated or followed, and again
    // at the server's record answer: an engine reference moved in an earlier
    // session stands at its file place until something moves it to the
    // record, and the view's own apply may not have run yet for a reference
    // just bound. Whether the copy stands apart from the record is read from
    // its body (E21, rung 0's x-prop-blast 20261111-223256: after canon's
    // translate put-back two resting Dynamic cabbages' references stood at
    // their records and their bodies 940 and 263 units off), and a body that
    // is Dynamic is set Keyframed before the keyframed translate, its motion
    // decided again REST_DYNAMIC_MS after the translate lands (landsAt).
    // HYPOTHESIS: that a translate moves a Keyframed body and its reference
    // together and leaves a Dynamic one (E21's mechanism; x-body-read v1's
    // leg B). The first track's "body" lines follow the placement
    private placeAtRecord(prop: Prop, refr: ObjectReference, now: number) {
        if (prop.placing) {
            return;
        }
        const pose = this.recordOf(prop.refr);
        if (!pose) {
            prop.placed = true;
            prop.placedAt = now;
            this.bodyTrack(prop, now);
            return;
        }
        const body = prop.loaded3D ? this.readBody(prop) : undefined;
        const read = body !== undefined ? body : PropService.read(refr);
        if (!PropService.apart(read, pose, SAMPLE_UNITS, SAMPLE_DEGREES)) {
            prop.placed = true;
            prop.placedAt = now;
            this.bodyTrack(prop, now);
            return;
        }
        prop.placing = true;
        const [x, y, z] = pose.pos;
        const [ax, ay, az] = pose.rot;
        const done = (why: string, placed = true) => {
            prop.placing = false;
            prop.placed = true;
            prop.placedAt = clockMs();
            logTrace(this, `prop ${prop.refr.toString(16)} placed at its record${why}`);
            if (prop.trap && placed) {
                logLab(TRAP_TAG, `trap body ${prop.refr.toString(16)} placed at its recorded rest`);
            }
            // the body read once the translate landed, still Keyframed
            this.bodyTrack(prop, prop.placedAt + REST_DYNAMIC_MS);
        };
        if (prop.loaded3D) {
            // H-P4: the placement by the engine's keyframed translate, the
            // collision moving with the node (a setPosition'd body flew at its
            // later Dynamic set, pass v10b 025124)
            const distance = ObjectReferenceEx.getDistance(read.pos, pose.pos);
            const speed = Math.max(GLIDE_MIN_SPEED, distance / (JUMP_MS / 1000));
            const translate = (r: ObjectReference) => {
                r.stopTranslation();
                r.translateTo(x, y, z, ax, ay, az, speed, 0);
                done(" (translated)");
            };
            if (body === undefined || !PropService.isDynamic(body.motion)) {
                translate(refr);
                return;
            }
            // E21's fix: Keyframed first, the translate in the set's answer
            // (the reference looked up again: a wrapper from an earlier frame
            // is out of Skyrim Platform's pool)
            prop.motionAsked = MotionType.Keyframed;
            refr.setMotionType(MotionType.Keyframed, false)
                .then(() => {
                    const again = this.refOf(prop);
                    if (!again) {
                        done(" (its reference went)", false);
                        return;
                    }
                    translate(again);
                    prop.landsAt = clockMs() + JUMP_MS;
                })
                .catch((err) => {
                    prop.motionAsked = undefined;
                    logError(this, `setMotionType(${MotionType.Keyframed}) before placing prop ${prop.refr.toString(16)} failed`, err);
                    done(" (the Keyframed set failed)", false);
                });
            return;
        }
        // the reference is looked up again after a latent call's answer (a
        // wrapper from an earlier frame is out of Skyrim Platform's pool)
        refr.setPosition(x, y, z)
            .then(() => {
                const again = this.refOf(prop);
                return again ? again.setAngle(ax, ay, az) : undefined;
            })
            .then(() => done(""))
            .catch((err) => {
                logError(this, `placing prop ${prop.refr.toString(16)} at its record failed`, err);
                done(" (the placement failed)", false);
            });
    }

    // The body's motion type follows the simulator: Dynamic here, Keyframed
    // everywhere else (ADR-058 decision 3). A glide in flight is stopped
    // before the body goes Dynamic and the stream followed ends; a copy that
    // followed a live stream is taken over (takeOver); the stream's baseline
    // starts afresh
    private flip(prop: Prop, refr: ObjectReference, simulate: boolean, wake: boolean, now: number) {
        if (!prop.loaded3D) {
            return; // the motion type is the 3D's; set when it loads
        }
        prop.handOff = undefined;
        prop.restAt = 0;
        if (simulate) {
            // a trap body this seat now simulates is its own again before its
            // Dynamic set (D-S3: a followed body's motion calls run neutralized)
            if (prop.trap) {
                this.releaseTrapFollow(prop.refr, "this seat simulates it");
            }
            refr.stopTranslation();
            const f = prop.follow;
            const live = f !== undefined && f.live(now, this.followDelayMs(), FOLLOW_QUIET_MS);
            const newest = f !== undefined ? f.newest() : undefined;
            this.endFollow(prop);
            if (live && newest !== undefined) {
                this.takeOver(prop, refr, newest, now);
            } else if (prop.motionAsked !== MotionType.Dynamic) {
                // D73: a body already Dynamic (a resting follower kicked or
                // grabbed here, the claim granted) takes no motion set, so the
                // hand's hold and the kick's motion carry on; a Keyframed one
                // (claimed while another seat streamed it) goes Dynamic and may
                // wake (x-prop-push v9 021439: a Dynamic body denied activation
                // took no contact)
                this.setMotion(prop, refr, MotionType.Dynamic, true);
            }
        }
        // the loss of the simulation sets no motion here: a word naming
        // another seat makes the body Keyframed (tick's keyframeDue), and a
        // body nobody streams rests Dynamic (D73)
        prop.simulating = simulate;
        prop.lastSent = undefined;
        prop.lastSendAt = 0;
        prop.sentHeld = false;
        prop.streaming = false;
        prop.awakeAt = now;
        // a granted touch (wake): the first sample goes out at once, so the
        // server's row and the other seats start from the owner's baseline
        // (the moving window and the step budget run from it; no silence
        // lapses a row since A1 to A7, and the heartbeat keeps it awake). The
        // host-default simulation (no wake) keeps the lazy baseline: nothing
        // goes out until a move or a hold
        prop.announce = simulate && wake;
        logTrace(this, `prop ${prop.refr.toString(16)} ${simulate ? "simulated here: Dynamic" : "followed: Keyframed"}`);
    }

    // D73: the motion a body takes when first set: Dynamic unless another seat
    // streams it awake right now (followLive)
    private restMotion(prop: Prop, simulate: boolean, now: number): MotionType {
        if (simulate) {
            return MotionType.Dynamic;
        }
        if (this.followedTrap(prop)) {
            return MotionType.Keyframed;
        }
        return this.followLive(prop, now) ? MotionType.Keyframed : MotionType.Dynamic;
    }

    // The take-over (the rung 2 plan's section 5): a word names this seat
    // while its copy follows a live stream (a departure's or an unload's
    // return to this host, A7 and D-PH2 A; a stalled row's claim, A3; a grab
    // on a moving copy the engine refuses, E28 and E31, and claims nothing
    // under D-PH3 A). The copy goes to the stream's NEWEST sample, not the
    // rendered one, so the record and the third seats see no rewind of the
    // render delay (this seat's own copy jumps ahead by up to it, said in the
    // line), by the keyframed translate within JUMP_MS; once that lands the
    // body goes Dynamic and, in setMotionType's answer, takes the newest
    // sample's velocities (handOver). HYPOTHESIS (prop-physics.md:41-42, a
    // read of code, no trace): a Keyframed copy set Dynamic starts at rest,
    // so without the hand-off a departing owner's flight stops dead here;
    // x-prop-follow v1's leg D reads it, and if the set keeps the velocity
    // the hand-off is harmless
    private takeOver(prop: Prop, refr: ObjectReference, newest: FollowSample, now: number) {
        const body = this.readBody(prop);
        const cur = body !== undefined ? body : PropService.read(refr);
        const ahead = ObjectReferenceEx.getDistance(cur.pos, newest.pos);
        const [x, y, z] = newest.pos;
        const [ax, ay, az] = newest.rot;
        refr.translateTo(x, y, z, ax, ay, az, Math.max(GLIDE_MIN_SPEED, ahead / (JUMP_MS / 1000)), 0);
        prop.motionAsked = MotionType.Keyframed;
        prop.handOff = { lin: newest.lin, ang: newest.ang, seq: newest.seq, ahead, due: now + JUMP_MS };
    }

    // The take-over's second half, its translate landed: the body Dynamic,
    // and in the set's answer the velocities through the platform's
    // setBodyVelocity (CommonLib's bhkRigidBody::SetLinearVelocity, Address
    // Library 78089, and SetAngularVelocity, queued to the main thread; its
    // task logs the set or why not), said once with how the hand went
    private handOver(prop: Prop, refr: ObjectReference) {
        const h = prop.handOff;
        if (h === undefined) {
            return;
        }
        prop.handOff = undefined;
        prop.motionAsked = MotionType.Dynamic;
        refr.setMotionType(MotionType.Dynamic, true)
            .then(() => {
                let how: string;
                try {
                    how = this.sp.setBodyVelocity(prop.localId, h.lin, h.ang) ? "velocity handed" : "velocity refused";
                } catch (e) {
                    how = "an older platform";
                }
                logLab(TAG, `took over ${prop.refr.toString(16)} at seq ${h.seq} moving ${Math.round(lengthOf(h.lin))} units a second, ${Math.round(h.ahead)} units ahead (${how})`);
            })
            .catch((err) => {
                prop.motionAsked = undefined;
                logError(this, `setMotionType(${MotionType.Dynamic}) on prop ${prop.refr.toString(16)} at its take-over failed`, err);
            });
    }

    // abAllowActivate is Havok's leave to wake: a Dynamic body denied it took
    // no contact from the host standing in it or running through it (x-prop-push
    // v9 021439 on the physics stage), so every body this seat simulates is set
    // Dynamic with it; the stream's lazy baseline (nothing out until a move)
    // keeps a resting cell's clutter silent. Keyframed as dealWithRef sets it.
    // HYPOTHESIS until v10: a body denied activation is inert to the controller
    private setMotion(prop: Prop, refr: ObjectReference, type: MotionType, wake: boolean) {
        prop.motionAsked = type;
        refr.setMotionType(type, wake).catch((err) => {
            prop.motionAsked = undefined;
            logError(this, `setMotionType(${type}) on prop ${prop.refr.toString(16)} failed`, err);
        });
    }

    // The update's one read of the bodies this seat simulates (readBodies
    // with all false), by this engine's id: a body while the engine has it
    // awake, and once more at the read its island deactivated, so a resting
    // prop costs nothing here (rule 9: filtered in native); an awake body's
    // island with it (physics rung 3, claimIslands)
    private readSimulated(): Map<number, Body> {
        const ids: number[] = [];
        this.props.forEach((prop) => {
            if (prop.simulating && prop.placed && prop.loaded3D) {
                ids.push(prop.localId);
            }
        });
        const bodies = new Map<number, Body>();
        if (ids.length > 0) {
            for (const body of this.readBodies(ids, false, true)) {
                bodies.set(body.formId, body);
            }
        }
        return bodies;
    }

    // The simulator's stream, from the update's read (readSimulated): a sample
    // is due when the body moved or turned past the dead band since the last
    // sent, or its hand changed mid-stream, at most every SEND_MIN_MS, and at
    // least every STREAM_HEARTBEAT_MS while it is awake or held (D-PH4 B),
    // unreliable; the rest, reliable, at the read where the engine
    // deactivated it and no hand holds it (A6: a held body never rests on the
    // wire, havok-props.md E42), or once the engine has not read an open
    // stream's body awake for a heartbeat; nothing more until it wakes or a
    // hand takes it. A paused world steps no Havok, so its islands stay as they
    // were and need no hold here (E11; under Souls RE's running menu Havok runs
    // and deactivates as ever, HYPOTHESIS, read by the gate's b-prop-push).
    // The grant's first sample, a hand on a resting body and an open stream's
    // body the update's read left out are read whole (readBody). Due samples
    // go out after the ticks, within the budget (sendDue)
    private stream(prop: Prop, refr: ObjectReference, now: number, read: Body | undefined) {
        if (prop.handOff !== undefined) {
            // a take-over's placement in flight: the stream starts from its hand-off
            if (now >= prop.handOff.due) {
                this.handOver(prop, refr);
            }
            return;
        }
        // the update's read named the body not awake: the read its island fell
        const fell = read !== undefined && !read.awake;
        let body = read;
        if (body === undefined) {
            if (!prop.announce && !prop.held && !prop.streaming) {
                return; // at rest, and nothing asks for it
            }
            body = this.readBody(prop);
            if (body === undefined) {
                return;
            }
        }
        if (body.awake) {
            prop.awakeAt = now;
        }
        if (!prop.lastSent) {
            if (prop.announce) {
                // the grant's first sample: the stream is open from here, so the
                // rest follows when the engine deactivates the body and no hand
                // holds it, and the server returns the prop on it
                this.due.push({ prop, body, asleep: false, line: "started at the grant" });
                return;
            }
            // the baseline: nothing goes out until the body moves from here
            prop.lastSent = { pos: body.pos, rot: body.rot };
            return;
        }
        const moved = PropService.apart(body, prop.lastSent, SAMPLE_UNITS, SAMPLE_DEGREES);
        if (!prop.held && !body.awake) {
            // the owner's own final sample, the engine's rest: the server
            // returns the prop to the cell's host on it and its word (player 0)
            // comes back; nothing is inferred here (A4). A body that moved past
            // the dead band rests on the wire even if its stream never opened
            // (its opening sample held back by the budget), so the record keeps
            // its place
            if ((prop.streaming || moved) && (fell || now - prop.awakeAt >= STREAM_HEARTBEAT_MS)) {
                this.due.push({ prop, body, asleep: true, line: fell ? "deactivated" : "inactive" });
            }
            return;
        }
        if (moved || (prop.streaming && prop.held !== prop.sentHeld)) {
            if (now - prop.lastSendAt >= SEND_MIN_MS) {
                this.due.push({ prop, body, asleep: false, line: prop.streaming ? undefined : "started" });
            }
            return;
        }
        if (!prop.streaming) {
            if (prop.held) {
                // a hand took a resting body this seat simulates (the host's
                // own clutter): the stream opens at once, so the followers'
                // copies go Keyframed in the hand's place and the server's row
                // stays awake under the hold
                this.due.push({ prop, body, asleep: false, line: "started: held" });
            }
            return;
        }
        // awake, or in the hand: the heartbeat
        if (now - prop.lastSendAt >= STREAM_HEARTBEAT_MS) {
            this.due.push({ prop, body, asleep: false, line: undefined });
        }
    }

    // The update's due samples within the budget (the priority accumulator,
    // SEND_BUDGET_BURST and SEND_BUDGET_PER_S): the rests first and always,
    // then the awake ones by priority while a whole token is left; the unsent
    // are made due again by the next update's stream, their priority grown with
    // their wait. A sample sent says its stream's opening or its rest
    private sendDue(now: number) {
        if (this.due.length === 0) {
            return;
        }
        const due = this.due;
        this.due = [];
        this.refillBudget(now);
        const priority = (d: Due): number => (now - d.prop.lastSendAt) * (lengthOf(d.body.lin) + PRIORITY_SPEED_FLOOR);
        due.sort((a, b) => (a.asleep !== b.asleep ? (a.asleep ? -1 : 1) : priority(b) - priority(a)));
        for (const d of due) {
            if (!d.asleep && this.budget < 1) {
                break;
            }
            const seq = this.send(d.prop, d.body, d.asleep, now);
            const id = d.prop.refr.toString(16);
            if (d.asleep) {
                // a rest the wire cannot carry ends the stream unsent (logged)
                d.prop.streaming = false;
                if (seq !== 0) {
                    logLab(TAG, `stream ${id} asleep (${d.line}, seq ${seq})`);
                    this.bodyLogAt(d.prop, now);
                }
            } else if (seq !== 0 && d.line !== undefined) {
                d.prop.announce = false;
                d.prop.streaming = true;
                d.prop.awakeAt = now;
                logLab(TAG, `stream ${id} ${d.line}`);
            }
        }
    }

    // the budget's tokens refilled to now, at most SEND_BUDGET_BURST
    private refillBudget(now: number) {
        if (this.budgetAt !== 0) {
            this.budget = Math.min(SEND_BUDGET_BURST, this.budget + ((now - this.budgetAt) * SEND_BUDGET_PER_S) / 1000);
        }
        this.budgetAt = now;
    }

    // One sample out (PropTransform v2, ADR-071 decision 6): the epoch of the
    // word this seat holds (0 with none), the next sequence within it (from 1
    // at a new epoch, SEQ_MAX wrapping to 1), the read's time on the owner's
    // clock (tMsOf), the body's place, turn, velocities and hand within the
    // wire's bounds, no bones (an item is one body); a token of the budget
    // spent. Returns the sequence, or 0 for a read the wire cannot carry (a
    // place or turn that is not a number, a place outside the world), which
    // goes nowhere and is said
    private send(prop: Prop, body: Body, asleep: boolean, now: number): number {
        const pos = body.pos;
        const rot: NiPoint3 = [body.rot[0] % 360, body.rot[1] % 360, body.rot[2] % 360];
        for (let i = 0; i < 3; ++i) {
            if (!isFinite(pos[i]) || Math.abs(pos[i]) > WORLD_ABS_MAX || !isFinite(rot[i])) {
                if (!prop.badBodySaid) {
                    prop.badBodySaid = true;
                    logError(this, `prop ${prop.refr.toString(16)}'s body read (${pos.join(", ")}) (${rot.join(", ")}) is no sample: not sent`);
                }
                return 0;
            }
        }
        const word = this.owners.get(prop.refr);
        const epoch = word ? word.epoch : 0;
        const last = this.sent.get(prop.refr);
        const seq = last !== undefined && last.epoch === epoch ? (last.seq >= SEQ_MAX ? 1 : last.seq + 1) : 1;
        this.sent.set(prop.refr, { epoch, seq });
        // A6: never held and asleep together (the validator refuses the pair)
        const held = prop.held && !asleep;
        const message: PropTransformMessage = {
            t: MsgType.PropTransform,
            refr: prop.refr,
            epoch,
            seq,
            tMs: tMsOf(now),
            pos: [pos[0], pos[1], pos[2]],
            rot,
            lin: bounded(body.lin, LIN_MAX_UNITS_PER_S),
            ang: bounded(body.ang, ANG_MAX_RAD_PER_S),
            asleep,
            held,
            bones: [],
        };
        this.controller.emitter.emit("sendMessage", {
            message,
            reliability: asleep ? "reliable" : "unreliable",
        });
        this.budget -= 1;
        prop.lastSent = { pos: message.pos, rot };
        prop.lastSendAt = now;
        prop.sentHeld = held;
        // the record in this seat's model follows its own samples too
        this.recordSample(prop.refr, prop.lastSent);
        return seq;
    }

    // The follower: the stream followed rendered on its clock (one translate a
    // segment, the rest applied when the render clock reaches it) and its end
    private follow(prop: Prop, refr: ObjectReference, now: number) {
        const f = prop.follow;
        if (f !== undefined) {
            const awake = !f.newest().asleep;
            // D73: the stream drives a Keyframed body; the first awake sample of
            // a burst sets it (a resting body is Dynamic)
            if (awake && prop.loaded3D && prop.motionAsked !== MotionType.Keyframed) {
                this.setMotion(prop, refr, MotionType.Keyframed, true);
            }
            // D-S3: a trap body another seat streams awake again (its owner's
            // word unchanged since the rest that ended the follow) is followed
            // again
            if (awake && prop.trap) {
                this.holdTrapBody(prop.refr);
            }
            const step = f.step(now, this.followDelayMs());
            if (step.fresh) {
                this.apply(prop, refr, f, step, now);
            }
            if (step.rested) {
                prop.restAt = now;
            }
        }
        // D73: back to Dynamic once the stream ended (REST_DYNAMIC_MS after the
        // render clock applied its asleep sample) or went quiet (followLive),
        // so the resting body takes a kick or a grab at once; the stream's end
        // is said (endFollow) either way. A stream that ended at a rest ends by
        // its rest alone, and a placement's Keyframed hold by its landing
        // (landsAt)
        const rested = prop.restAt !== 0 && now - prop.restAt >= REST_DYNAMIC_MS;
        const keyframed = prop.loaded3D && prop.motionAsked === MotionType.Keyframed;
        const quiet = !rested && (f !== undefined ? !f.newest().asleep : keyframed) && !this.followedTrap(prop)
            && !this.followLive(prop, now);
        if (prop.landsAt === 0 && (rested || quiet)) {
            prop.restAt = 0;
            if (keyframed) {
                this.setMotion(prop, refr, MotionType.Dynamic, true);
                logTrace(this, `prop ${prop.refr.toString(16)} at rest: Dynamic (${rested ? "the stream ended" : "the stream went quiet"})`);
            }
            if (f !== undefined) {
                this.endFollow(prop);
                this.bodyLogAt(prop, now + REST_DYNAMIC_MS);
            }
        }
    }

    // A segment of the stream on the Keyframed copy, once each, as the render
    // clock enters it: one translate to the segment's end at the speed that
    // covers what is left of the copy's way there in what is left of the
    // segment on the owner's clock (at least SEGMENT_MIN_MS), so the copy
    // arrives as the render clock does and is still moving when the next
    // segment starts; from JUMP_UNITS a jump within JUMP_MS, counted; a turn in
    // place by setAngle; a reference with no 3D placed (setPosition, then
    // setAngle). The copy's place is its body's (readBody), its reference's
    // where the platform reads no body. H-P4 (pass v10b 025124): a body placed
    // by setPosition and later set Dynamic flew on every seat it happened to;
    // the engine's keyframed translate moves the collision with the node
    private apply(prop: Prop, refr: ObjectReference, f: PropFollow, step: FollowStep, now: number) {
        const s = step.target;
        const body = prop.loaded3D ? this.readBody(prop) : undefined;
        const cur = body !== undefined ? body : PropService.read(refr);
        const distance = ObjectReferenceEx.getDistance(cur.pos, s.pos);
        let turned = 0;
        for (let i = 0; i < 3; ++i) {
            turned = Math.max(turned, Math.abs(angleDelta(s.rot[i], cur.rot[i])));
        }
        if (distance < 0.1 && turned < 0.1) {
            return;
        }
        const [x, y, z] = s.pos;
        const [ax, ay, az] = s.rot;
        if (!prop.loaded3D) {
            refr.setPosition(x, y, z)
                .then(() => {
                    const again = this.refOf(prop);
                    return again ? again.setAngle(ax, ay, az) : undefined;
                })
                .catch((err) => logError(this, `placing prop ${prop.refr.toString(16)} at its sample failed`, err));
        } else if (distance < 0.1) {
            refr.setAngle(ax, ay, az).catch((err) => logError(this, `turning prop ${prop.refr.toString(16)} failed`, err));
        } else {
            const jumped = distance >= JUMP_UNITS;
            const windowMs = jumped ? JUMP_MS : Math.max(SEGMENT_MIN_MS, step.remainingMs);
            if (jumped) {
                refr.stopTranslation();
            }
            refr.translateTo(x, y, z, ax, ay, az, Math.max(GLIDE_MIN_SPEED, distance / (windowMs / 1000)), 0);
            f.noteStep(distance, jumped);
        }
        if (now - prop.lastAppliedLog >= APPLIED_LOG_MS) {
            prop.lastAppliedLog = now;
            logLab(TAG, `applied ${prop.refr.toString(16)} from the stream`);
        }
    }

    // Whether another seat streams the prop awake as this seat sees it: the
    // stream followed live on its render clock (its newest sample awake and
    // under FOLLOW_QUIET_MS behind it), or a word naming another seat, newer
    // than any stream followed, whose first sample is still on its way
    // (FOLLOW_QUIET_MS and the delay from the word). The follower's D73 rest,
    // its first motion and a contact's A3 test all read it
    private followLive(prop: Prop, now: number): boolean {
        const f = prop.follow;
        const delay = this.followDelayMs();
        if (f !== undefined && f.live(now, delay, FOLLOW_QUIET_MS)) {
            return true;
        }
        const word = this.owners.get(prop.refr);
        if (word === undefined || word.player === 0 || word.player === this.myId()) {
            return false;
        }
        return (f === undefined || f.epoch() < word.epoch) && now - word.at < FOLLOW_QUIET_MS + delay;
    }

    // A followed stream's end (its rest's Dynamic set, its quiet, a new
    // owner's first sample, this seat's simulation or take-over, a record
    // answer, a fresh table, an untrack): its figures in two lines, since one
    // passes the console's 128-byte cut (InGameConsolePrinter.cpp:26-28), the
    // first x-prop-follow's claims (gaps, dry frames, jumps), the second its
    // records (the largest step, the delay)
    private endFollow(prop: Prop) {
        const f = prop.follow;
        if (f === undefined) {
            return;
        }
        prop.follow = undefined;
        const id = prop.refr.toString(16);
        // the seat by id, 0 the cell's host, "unheard" before its word came
        const seat = f.seat === undefined ? "unheard" : f.seat.toString(16);
        logLab(TAG, `follow ${id} epoch ${f.epoch()} from ${seat}: ${f.samples} samples, ${f.gaps()} seq gaps, ${f.dry} dry frames, ${f.jumps} jumps`);
        logLab(TAG, `follow ${id} epoch ${f.epoch()}: largest step ${f.largestStep.toFixed(1)} units, delay ${this.followDelayMs()} ms`);
    }

    // The hand, read from the engine once per update (ADR-071's D-PH3 A: a
    // sample's held is the engine's own grab state, never an event this
    // service tracks, so a missed release cannot strand a prop). A tracked
    // prop the engine starts holding is claimed by grab then, and only then:
    // a grab the engine refuses in its own frame (a Keyframed copy, E28 and
    // E31) still raised grabRelease true and false together and claimed touch
    // 1, and the server granted a moving row to a seat that never held it
    // (havok-props.md E43, x-prop-engine v2.1 20261112-003027). A hold that
    // ends, by the next press of E or by the engine itself (E40, E41), ends on
    // the server by the stream's next sample, which says held false (D-PH3 A
    // on the wire, physics rung 2: no release claim), and the rest's grace
    // starts again from it, so the fall gets a heartbeat to wake. A
    // grab's claim goes even for a prop this seat simulates: the server marks
    // it held against the other seats' claims. The releases go before the
    // grab when the hand moved from one prop to another between two reads, so
    // the server hears the holds in the engine's order (one grabbed object
    // per player; a grab ends the claimant's other hold there too). A trap
    // body moves by a firing alone
    private readHolds(now: number) {
        const grabbed = this.sp.Game.getPlayerGrabbedRef();
        const heldLocal = grabbed ? grabbed.getFormID() : 0;
        const props = Array.from(this.props.values());
        const holds = (prop: Prop) => heldLocal !== 0 && prop.localId === heldLocal && !prop.trap;
        for (const prop of props) {
            if (prop.held && !holds(prop)) {
                this.letGo(prop, now, "the engine's grab ended");
            }
        }
        for (const prop of props) {
            if (!prop.held && holds(prop)) {
                prop.held = true;
                prop.heldAt = now;
                prop.holdLost = false;
                logLab(TAG, `prop ${prop.refr.toString(16)} held: the engine's grab names it`);
                this.claim(prop, TOUCH_GRAB, now);
            }
        }
    }

    // a hold that ended (the engine's grab, or the prop leaving the tracked
    // set): the stream's next sample says so (stream: a hand changed
    // mid-stream is a sample due), and the rest's grace runs from here
    private letGo(prop: Prop, now: number, why: string) {
        prop.held = false;
        logLab(TAG, `prop ${prop.refr.toString(16)} let go: ${why}${prop.holdLost ? " (another seat's word had ended this hand's hold)" : ""}`);
        prop.awakeAt = now;
        prop.holdLost = false;
    }

    // The contacts since the last update, by local id: a tracked prop this
    // seat does not simulate is claimed (touch 0), at most once per
    // CLAIM_EVERY_MS per prop (a granted claim comes back as PropOwner, after
    // which the body is this seat's and the next contacts say so); the
    // expected body is the follower's, Keyframed (motion 4)
    private claimContacts(now: number) {
        const contacts = this.contacts;
        this.contacts = [];
        const myId = this.myId();
        for (const c of contacts) {
            const refr = localIdToRemoteId(c.localId);
            const prop = refr ? this.props.get(refr) : undefined;
            if (!prop) {
                logTrace(this, `contact ${c.localId.toString(16)} layer ${c.layer} motion ${c.motionType}: not a tracked prop, no claim`);
                continue;
            }
            const head = `contact ${prop.refr.toString(16)} layer ${c.layer} motion ${c.motionType}`;
            if (prop.trap) {
                logTrace(this, `${head}: a trap body moves by a firing alone, no claim`);
                continue;
            }
            const owned = prop.simulating || (myId !== 0 && prop.effective === myId);
            if (owned) {
                logLab(TAG, `${head}: owned, no claim`);
                continue;
            }
            // A3: a non-owner's contact never takes a moving prop; the server
            // would refuse it (E_PROP_MOVING), so the claim stays home while
            // another seat's awake stream is live here (followLive, on the
            // render clock; never the contact's motion type: a resting copy
            // reads 3, havok-props.md E26)
            const streamed = this.followLive(prop, now);
            if (streamed) {
                logLab(TAG, `${head}: another seat streams it awake, no claim`);
                continue;
            }
            if (now - prop.lastClaimAt < CLAIM_EVERY_MS) {
                logTrace(this, `${head}: claimed ${now - prop.lastClaimAt} ms ago, no claim`);
                continue;
            }
            this.claim(prop, TOUCH_CONTACT, now);
            logLab(TAG, `${head}: claimed`);
        }
    }

    // one prop's claim, PropClaim v2's list of one (an impact's bodies and an
    // island's go in one, claimMany)
    private claim(prop: Prop, touch: number, now: number) {
        prop.lastClaimAt = now;
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.PropClaim, refrs: [prop.refr], touch },
            reliability: "reliable",
        });
        logLab(TAG, `claim ${prop.refr.toString(16)} touch ${touch}`);
    }

    // several props in one claim (PropClaim v2: 16 references at most, each
    // judged alone on the server, its own word or answer), a larger set in
    // several; `why` rides the line, "claim <ids> touch 3 (impact 2: cone,
    // cause 14)" or "(island of <id>)", one line a message
    private claimMany(props: Prop[], touch: number, now: number, why: string) {
        for (let i = 0; i < props.length; i += 16) {
            const batch = props.slice(i, i + 16);
            for (const prop of batch) {
                prop.lastClaimAt = now;
            }
            this.controller.emitter.emit("sendMessage", {
                message: { t: MsgType.PropClaim, refrs: batch.map((p) => p.refr), touch },
                reliability: "reliable",
            });
            logLab(TAG, `claim ${batch.map((p) => p.refr.toString(16)).join(",")} touch ${touch} (${why})`);
        }
    }

    // Physics rung 3 (ADR-071 decision 4; the rung 3 plan's B4): the impacts
    // this seat's own cause gave since the last update, folded by impact
    // number, a cone's held CONE_BATCH_MS from its first event (its events
    // come one per collidable across ticks) and every other kind's claimed at
    // once. Each impact's rows (items and trap bodies; a static and a ragdoll
    // are no row before rungs 4 and 5) are tracked in place at any distance
    // (B5), and the ones this seat does not own go in one claim by touch 3; a
    // row another seat streams awake stays home (A3: the server refuses it
    // E_PROP_MOVING, and its Keyframed copy here took no push, E27). A row this
    // seat owns (the cell's host's resting clutter, its own live row) streams
    // the push itself (the host's wake, A7)
    private claimImpulses(now: number) {
        for (const e of this.impulses) {
            let batch = this.impactBatches.get(e.impact);
            if (!batch) {
                batch = { kind: e.kind, cause: e.cause, firstAt: now, refs: [] };
                this.impactBatches.set(e.impact, batch);
            }
            for (const r of e.refrs) {
                if (!batch.refs.some((x) => x.localId === r.localId)) {
                    batch.refs.push(r);
                }
            }
        }
        this.impulses = [];
        const myId = this.myId();
        this.impactBatches.forEach((batch, impact) => {
            if (batch.kind === "cone" && now - batch.firstAt < CONE_BATCH_MS && batch.refs.length < 16) {
                return;
            }
            this.impactBatches.delete(impact);
            const claims: Prop[] = [];
            let rows = 0;
            for (const r of batch.refs) {
                const prop = this.rowInPlace(r, now, "pushed by this seat's impulse");
                if (!prop) {
                    continue;
                }
                rows += 1;
                if (this.ownedHere(prop, myId)) {
                    continue;
                }
                if (this.followLive(prop, now)) {
                    logLab(TAG, `impact ${impact}: prop ${prop.refr.toString(16)} another seat streams awake, no claim`);
                    continue;
                }
                claims.push(prop);
            }
            logTrace(this, `impact ${impact} (${batch.kind}, cause ${batch.cause.toString(16)}): ${batch.refs.length} references pushed, ${rows} rows, ${claims.length} to claim`);
            if (claims.length > 0) {
                this.claimMany(claims, TOUCH_IMPULSE, now, `impact ${impact}: ${batch.kind}, cause ${batch.cause.toString(16)}`);
            }
        });
    }

    // Physics rung 3 (the rung 3 plan's B4, the island; H4: a resting pile is
    // one sleeping island, a neighbour joins the moving body's island on the
    // first moving frame, and islands split and merge on contact in flight):
    // the update's read of the bodies this seat simulates carries each awake
    // one's island, its other dynamic members; each member that is a row is
    // tracked in place, and the ones this seat does not own and has not
    // claimed in the last CLAIM_EVERY_MS go in one claim by touch 3 per
    // island, "(island of <id>)". Read on the JS thread with the stream's own
    // read, never inside the step. A member another seat streams awake is
    // Keyframed here and in no island (a moving island with two owners is a
    // declared limit: each engine meets the other's copy Keyframed)
    private claimIslands(bodies: Map<number, Body>, now: number, myId: number) {
        bodies.forEach((body) => {
            const island = body.island;
            const of = this.byLocal.get(body.formId);
            if (island === undefined || island.length === 0 || of === undefined) {
                return;
            }
            const claims: Prop[] = [];
            for (const member of island) {
                const prop = this.rowInPlace(member, now, `knocked into ${of.toString(16)}'s island`);
                if (!prop || this.ownedHere(prop, myId) || now - prop.lastClaimAt < CLAIM_EVERY_MS || this.followLive(prop, now)) {
                    continue;
                }
                claims.push(prop);
            }
            if (claims.length > 0) {
                this.claimMany(claims, TOUCH_IMPULSE, now, `island of ${of.toString(16)}`);
            }
        });
    }

    // Physics rung 3 (B5): the awake samples of props this seat did not
    // track, kept by onTransform since the last update: a prop whose reference
    // stands loaded in the player's world or cell is tracked in place (a
    // watcher past TRACK_RADIUS follows the caster's stream) and fed its
    // samples at their arrival times
    private trackStreams() {
        const waiting = this.streamsUntracked;
        this.streamsUntracked = [];
        for (const w of waiting) {
            let prop = this.props.get(w.message.refr);
            if (!prop) {
                const localId = remoteIdToLocalId(w.message.refr);
                prop = localId ? this.trackInPlace(w.message.refr, localId, undefined, "its stream arrived") : undefined;
            }
            if (prop) {
                this.feed(prop, w.message, w.at, false);
            }
        }
    }

    // A body an engine read named (an impact's, an island member's), by this
    // engine's id and its base's FormType number, as a tracked row: an item,
    // or a trap body this seat knows (the server's world-body row, touch 3
    // alone), tracked in place; undefined for a static, a ragdoll, an
    // unloaded reference or one the server has no id for here
    private rowInPlace(r: Member, now: number, why: string): Prop | undefined {
        const refr = localIdToRemoteId(r.localId);
        if (!refr) {
            return undefined;
        }
        if (!FormTypeEx.isItem(r.formType as FormType) && !this.trapBodies.has(refr)) {
            return undefined;
        }
        const known = this.props.get(refr);
        if (known) {
            // the engine's body moved by this seat's cause is the truth from
            // here: a prop the rescan tracked moments ago (its grace running,
            // its record placement not yet made) is in place, never put back
            known.trackedAt = 0;
            if (!known.placed && !known.placing) {
                known.placed = true;
                known.placedAt = now;
            }
            return known;
        }
        const body = this.trapBodies.get(refr);
        if (body) {
            return this.trackTrapBody(refr, body, now, false);
        }
        // a pushed or knocked body is Dynamic (a Keyframed one takes no push,
        // havok-props.md E27; an island's members are its dynamic ones)
        return this.trackInPlace(refr, r.localId, MotionType.Dynamic, why);
    }

    // A prop tracked where its engine has it (physics rung 3, B5): the
    // reference loaded in the player's world or cell, placed already (an
    // engine's own body in flight is never put back at the record; the record
    // follows its stream), no grace (no copy is being spawned), `motion` the
    // body's as read (undefined: the tick sets it, D73). Undefined when the
    // reference is not loaded here
    private trackInPlace(refr: number, localId: number, motion: MotionType | undefined, why: string): Prop | undefined {
        const ref = this.sp.ObjectReference.from(this.sp.Game.getFormEx(localId));
        const player = this.sp.Game.getPlayer();
        if (!ref || !player || !ref.is3DLoaded()) {
            return undefined;
        }
        const worldOrCell = ObjectReferenceEx.getWorldOrCell(player);
        if (worldOrCell !== 0 && ObjectReferenceEx.getWorldOrCell(ref) !== worldOrCell) {
            return undefined;
        }
        const now = clockMs();
        const distance = ObjectReferenceEx.getDistance(ObjectReferenceEx.getPos(player), ObjectReferenceEx.getPos(ref));
        const cell = ref.getParentCell();
        const prop = PropService.newProp(refr, localId, -1, cell ? cell.getFormID() : 0, distance, 0);
        prop.placed = true;
        prop.placedAt = now;
        prop.loaded3D = true;
        prop.motionAsked = motion;
        this.props.set(refr, prop);
        this.byLocal.set(localId, refr);
        logTrace(this, `prop ${refr.toString(16)} tracked in place, ${Math.round(distance)} units off: ${why}`);
        return prop;
    }

    // whether this seat owns the prop as it reads the table: it simulates it,
    // or the word names it, or the cell's host by default is this seat
    private ownedHere(prop: Prop, myId: number): boolean {
        return prop.simulating || (myId !== 0 && this.ownerOf(prop, myId) === myId);
    }

    // The tracked set, from the view's references: every bound view whose
    // record is no actor's and whose base is an item (read once per binding),
    // in the player's world or cell, within TRACK_RADIUS; the nearest
    // TRACK_MAX are tracked, with the trap bodies and (physics rung 3, B5) the
    // props kept at any distance (keptAnywhere) and the far items this seat's
    // own engine woke in a cell it hosts (wakeFar); the rest let go
    private rescan(now: number) {
        const view = getViewFromStorage();
        const player = this.sp.Game.getPlayer();
        if (!view || !player) {
            return;
        }
        const world = this.controller.lookupListener(RemoteServer).getWorldModel();
        this.releaseOrphanTrapFollows(this.myId());
        const playerPos = ObjectReferenceEx.getPos(player);
        const playerWorldOrCell = ObjectReferenceEx.getWorldOrCell(player);
        const views = view.getFormViews();
        const n = views.getFormViewsArrayLength();
        type Found = { refr: number; localId: number; formIdx: number; distance: number; cell: number; baseId: number };
        const found: Found[] = [];
        // the items past TRACK_RADIUS, by this engine's id, for wakeFar
        const far: number[] = [];
        for (let i = 0; i < n; ++i) {
            if (i === world.playerCharacterFormIdx) {
                continue;
            }
            const fv = views.getNthFormView(i);
            if (!fv) {
                continue;
            }
            const localId = fv.getLocalRefrId();
            if (!localId || localId === 0x14) {
                continue;
            }
            const form = world.forms[i];
            if (!form || !form.refrId) {
                continue;
            }
            // an actor's record (a figure's look, the word per NPC) is no
            // prop, said without a native
            if (form.appearance || form.isHostedByOther !== undefined) {
                continue;
            }
            if (form.movement && playerWorldOrCell !== 0 && form.movement.worldOrCell !== playerWorldOrCell) {
                continue;
            }
            if (!this.isItem(localId, form.refrId)) {
                continue;
            }
            const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(localId));
            if (!refr) {
                continue;
            }
            const distance = ObjectReferenceEx.getDistance(playerPos, ObjectReferenceEx.getPos(refr));
            if (distance > TRACK_RADIUS) {
                far.push(localId);
                continue;
            }
            const cell = refr.getParentCell();
            found.push({ refr: form.refrId, localId, formIdx: i, distance, cell: cell ? cell.getFormID() : 0, baseId: form.baseId || 0 });
        }
        // the game-file items the server subscribed this seat to (no view, no model: engineRefs)
        this.engineRefs.forEach((rec, refrId) => {
            if (found.some((c) => c.refr === refrId)) {
                return;
            }
            if (playerWorldOrCell !== 0 && rec.worldOrCell !== playerWorldOrCell) {
                return;
            }
            if (!this.isItem(refrId, refrId)) {
                // ADR-068 D-S3 point 4: no item, but perhaps a trap body a
                // firing moved before this session (the trap bodies' loop
                // below takes it)
                this.judgeTrapRecord(refrId, rec);
                return;
            }
            const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(refrId));
            if (!refr) {
                return;
            }
            const distance = ObjectReferenceEx.getDistance(playerPos, ObjectReferenceEx.getPos(refr));
            if (distance > TRACK_RADIUS) {
                far.push(refrId);
                return;
            }
            const cell = refr.getParentCell();
            found.push({ refr: refrId, localId: refrId, formIdx: -1, distance, cell: cell ? cell.getFormID() : 0, baseId: rec.baseId });
        });
        found.sort((a, b) => a.distance - b.distance);
        const chosen = found.slice(0, TRACK_MAX);
        for (const c of found.slice(TRACK_MAX)) {
            far.push(c.localId);
        }
        // ADR-068 D-S3 point 4: the trap bodies, past the item cap and at any
        // distance in the player's world or cell (a follower far from the
        // firing still holds the stream's rocks)
        this.trapBodies.forEach((body, refrId) => {
            if (chosen.some((c) => c.refr === refrId)) {
                return;
            }
            const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(body.localId));
            if (!refr || (playerWorldOrCell !== 0 && ObjectReferenceEx.getWorldOrCell(refr) !== playerWorldOrCell)) {
                return;
            }
            const cell = refr.getParentCell();
            const distance = ObjectReferenceEx.getDistance(playerPos, ObjectReferenceEx.getPos(refr));
            chosen.push({ refr: refrId, localId: body.localId, formIdx: -1, distance, cell: cell ? cell.getFormID() : 0, baseId: 0 });
        });
        const keep = new Set<number>();
        for (const c of chosen) {
            keep.add(c.refr);
            let prop = this.props.get(c.refr);
            const body = this.trapBodies.get(c.refr);
            if (!prop && body) {
                // taken again, or first, by the rescan: placed at its record
                // (nothing moves when it stands there already)
                prop = this.trackTrapBody(c.refr, body, now, true);
                if (!prop) {
                    keep.delete(c.refr);
                }
            } else if (!prop) {
                prop = PropService.newProp(c.refr, c.localId, c.formIdx, c.cell, c.distance, now);
                // a follower's new prop was silent (placed within two units, Keyframed from the start): one line per track
                logTrace(this, `prop ${c.refr.toString(16)} tracked, ${Math.round(c.distance)} units off`);
                this.props.set(c.refr, prop);
                this.byLocal.set(c.localId, c.refr);
                this.claimDrop(prop, c.baseId, c.distance, now);
            } else {
                if (prop.localId !== c.localId) {
                    // the view rebound the record to another reference (a
                    // respawned copy): the body is a new one, placed and set anew
                    this.byLocal.delete(prop.localId);
                    prop.localId = c.localId;
                    this.byLocal.set(c.localId, c.refr);
                    prop.placed = false;
                    prop.placing = false;
                    prop.landsAt = 0;
                    prop.motionAsked = undefined;
                    prop.loaded3D = false;
                    prop.lastSent = undefined;
                    prop.handOff = undefined;
                    prop.bodyTracked = false;
                    prop.badBodySaid = false;
                    prop.trackedAt = now;
                    this.endFollow(prop);
                }
                prop.formIdx = c.formIdx;
                prop.distance = c.distance;
                if (c.cell) {
                    prop.cell = c.cell;
                }
            }
        }
        // physics rung 3 (B5): kept wherever it is, while its 3D is loaded in
        // the player's world or cell
        const myId = this.myId();
        this.props.forEach((prop) => {
            if (!keep.has(prop.refr) && this.keptAnywhere(prop, myId, now) && this.loadedHere(prop, playerWorldOrCell)) {
                keep.add(prop.refr);
            }
        });
        this.wakeFar(far, keep);
        for (const prop of Array.from(this.props.values())) {
            if (!keep.has(prop.refr)) {
                this.untrack(prop, "out of reach");
            }
        }
        this.drops = this.drops.filter((d) => now - d.at <= DROP_MATCH_MS);
    }

    // Physics rung 3 (the rung 3 plan's B5): a prop kept at any distance:
    // one this seat owns by a live word naming it, or streams or holds; one
    // whose stream this seat follows (followLive: an awake sample on the
    // render clock within the quiet window, or a word naming another seat
    // whose first sample is on its way); one this seat claimed moments ago
    // (an impact, an island, a contact), awaiting the word; a trap body is the
    // trap bodies' loop's
    private keptAnywhere(prop: Prop, myId: number, now: number): boolean {
        const word = this.owners.get(prop.refr);
        if (myId !== 0 && word !== undefined && word.player === myId) {
            return true;
        }
        return prop.streaming || prop.held || this.followLive(prop, now) || (prop.lastClaimAt !== 0 && now - prop.lastClaimAt < CLAIM_EVERY_MS);
    }

    // whether the prop's reference stands with its 3D loaded in the player's
    // world or cell (the engine has its body to simulate or to show)
    private loadedHere(prop: Prop, playerWorldOrCell: number): boolean {
        const ref = this.refOf(prop);
        return ref !== null && ref.is3DLoaded()
            && (playerWorldOrCell === 0 || ObjectReferenceEx.getWorldOrCell(ref) === playerWorldOrCell);
    }

    // Physics rung 3 (B5): the far items this seat's own engine woke (an NPC
    // knocking clutter far from the player), read once a rescan by one
    // readBodies with all false over the untracked items past TRACK_RADIUS
    // (filtered in native, rule 9: answered while awake), at most
    // ENGINE_REFS_MAX ids; an awake one in a cell this seat hosts is tracked
    // in place, so it streams as the host's wake (A7). Another seat's far
    // woken copy is not its to stream (the declared limit R6 of the plan: a
    // puppet's walk into a far item moves it with no claim). HYPOTHESIS: rare;
    // the count line reads it
    private wakeFar(far: number[], keep: Set<number>) {
        const ids = far.filter((localId) => !this.byLocal.has(localId)).slice(0, ENGINE_REFS_MAX);
        if (ids.length === 0) {
            return;
        }
        const hosts = this.controller.lookupListener(HostService);
        let woken = 0;
        for (const body of this.readBodies(ids, false)) {
            if (!body.awake || !PropService.isDynamic(body.motion)) {
                continue;
            }
            const refr = localIdToRemoteId(body.formId);
            const ref = refr ? this.sp.ObjectReference.from(this.sp.Game.getFormEx(body.formId)) : null;
            const cell = ref ? ref.getParentCell() : null;
            if (!refr || !cell || !hosts.hostsCell(cell.getFormID())) {
                continue;
            }
            const prop = this.trackInPlace(refr, body.formId, body.motion as MotionType, "its engine woke it here, past TRACK_RADIUS");
            if (prop) {
                keep.add(refr);
                woken += 1;
            }
        }
        if (woken > 0) {
            logLab(TAG, `rescan: ${woken} far item(s) this seat's engine woke in a cell it hosts, tracked`);
        }
    }

    // A prop just tracked that is the server's reference for an item this
    // player dropped moments ago (its base, near the player) is claimed by
    // touch 2. Two players dropping the same base within the window both
    // claim; the server grants the newest (a slice-one limit)
    private claimDrop(prop: Prop, baseId: number, distance: number, now: number) {
        if (!baseId || this.drops.length === 0 || distance > DROP_MATCH_UNITS) {
            return;
        }
        const i = this.drops.findIndex((d) => d.baseId === baseId && now - d.at <= DROP_MATCH_MS);
        if (i < 0) {
            return;
        }
        this.drops.splice(i, 1);
        this.claim(prop, TOUCH_DROP, now);
    }

    // Whether the reference's base is an item (FormTypeEx.isItem's types),
    // read once per binding of a local id to a record
    private isItem(localId: number, refr: number): boolean {
        const known = this.kinds.get(localId);
        if (known && known.refr === refr) {
            return known.item;
        }
        const ref = this.sp.ObjectReference.from(this.sp.Game.getFormEx(localId));
        if (!ref) {
            // not loaded in this game yet: decided when it is, so no cache entry
            return false;
        }
        const base = ref.getBaseObject();
        const item = base !== null && FormTypeEx.isItem(base.getType());
        if (this.kinds.size > 4096) {
            this.kinds.clear();
        }
        this.kinds.set(localId, { refr, item });
        return item;
    }

    private untrack(prop: Prop, why: string) {
        this.props.delete(prop.refr);
        if (this.byLocal.get(prop.localId) === prop.refr) {
            this.byLocal.delete(prop.localId);
        }
        const now = clockMs();
        if (prop.held) {
            // a held prop leaving the tracked set (a teleport, a door) lets go
            // first: a hold the server never heard end holds the prop against
            // every other seat until this seat's next grab, unload or
            // departure, and the final sample below says held false
            this.letGo(prop, now, `untracked (${why})`);
        }
        this.endFollow(prop);
        prop.handOff = undefined;
        const refr = this.refOf(prop);
        if (prop.simulating && refr && refr.is3DLoaded()) {
            // an open stream's body place goes out once more as the final
            // sample (the server returns the prop to the host on it, the record
            // rests there, the other seats follow)
            const body = prop.streaming ? this.readBody(prop) : undefined;
            if (body !== undefined) {
                this.send(prop, body, true, now);
            }
            prop.streaming = false;
        }
        // physics rung 3 (B6): the body rests Dynamic as the game files make
        // it, wherever this service leaves it, so its own engine's next cause
        // moves it here (a far wake, an impact: tracked again) and another
        // seat's cause is zeroed at the impulse site; the Keyframed park, and
        // a copy left Keyframed past TRACK_RADIUS, were the remote blast's
        // shield (havok-props.md E34). A followed trap body is its follow's
        if (refr && refr.is3DLoaded() && prop.motionAsked === MotionType.Keyframed && !this.followedTrap(prop)) {
            refr.setMotionType(MotionType.Dynamic, false).catch((err) =>
                logError(this, `setMotionType(Dynamic) on untracked prop ${prop.refr.toString(16)} failed`, err));
        }
        logTrace(this, `prop ${prop.refr.toString(16)} untracked: ${why}`);
    }

    private untrackAll() {
        for (const prop of Array.from(this.props.values())) {
            this.untrack(prop, "propPhysics is off");
        }
    }

    // The record in this seat's model: the last transform known for the
    // prop, CreateActor's until a sample comes
    private recordSample(refr: number, s: Pose) {
        const form = this.formOf(refr);
        if (form && form.movement) {
            form.movement.pos = [s.pos[0], s.pos[1], s.pos[2]];
            form.movement.rot = [s.rot[0], s.rot[1], s.rot[2]];
        }
        const rec = this.engineRefs.get(refr);
        if (rec) {
            rec.pos = [s.pos[0], s.pos[1], s.pos[2]];
            rec.rot = [s.rot[0], s.rot[1], s.rot[2]];
        }
    }

    // the record's pose in this seat's model: the view's model's, or for a
    // game-file item (no model) the CreateActor's (engineRefs), each kept by
    // the samples (recordSample)
    private recordOf(refr: number): Pose | undefined {
        const form = this.formOf(refr);
        const m = form ? form.movement : undefined;
        if (m) {
            return { pos: m.pos, rot: m.rot };
        }
        const rec = this.engineRefs.get(refr);
        return rec ? { pos: rec.pos, rot: rec.rot } : undefined;
    }

    // The platform's body reads (readBodies, PuppetApi.cpp; the client's
    // typing in clientListener.ts), as the wire's triples. On the JS thread,
    // as the engine's own reads are, so any update context reads them, and
    // with no engine lock there (rung 2's fresh review, finding 1: an engine
    // thread can wait on this one while it holds a world lock); with islands,
    // an awake dynamic body's island too (physics rung 3). A platform without
    // the export (an older dist) reads no body, said once
    private readBodies(localIds: number[], all: boolean, islands = false): Body[] {
        try {
            return this.sp.readBodies(localIds, all, islands).map((b) => ({
                formId: b.formId,
                awake: b.awake,
                motion: b.motion,
                pos: [b.pos[0], b.pos[1], b.pos[2]],
                rot: [b.rot[0], b.rot[1], b.rot[2]],
                lin: [b.lin[0], b.lin[1], b.lin[2]],
                ang: [b.ang[0], b.ang[1], b.ang[2]],
                island: b.island !== undefined ? b.island.map((r) => ({ localId: r.refrId, formType: r.formType })) : undefined,
            }));
        } catch (e) {
            if (!this.readBodiesSaid) {
                this.readBodiesSaid = true;
                logError(this, `readBodies unavailable on this platform: ${e}`);
            }
            return [];
        }
    }

    // One prop's body read whole (all true, the filter left alone): a
    // placement's test, the follower's segment, the grant's first sample, a
    // hand on a resting body, an open stream's end, the take-over's jump and
    // the "body" lines. A loaded 3D the platform reads no body for (none on its
    // root or the root's children, or no cell world) is said once: its stream
    // cannot run (a body of several parts is rung 4's walk)
    private readBody(prop: Prop): Body | undefined {
        const body = this.readBodies([prop.localId], true)[0];
        if (body === undefined && prop.loaded3D && !prop.badBodySaid) {
            prop.badBodySaid = true;
            logLab(TAG, `prop ${prop.refr.toString(16)}: no Havok body read (none on its 3D root or its children, or no cell world)`);
        }
        return body;
    }

    // The "body" lines (the client setting propBodyReadLog, off by default):
    // the engine's place of the prop, its 3D root's, and how far its
    // reference and the record in this seat's model stand from it (x-body-
    // read's premise P0; rung 3's check that no body parts from its reference,
    // havok-props.md E21), at a prop's first track once any placement landed
    // and at each stream's end, on every seat. Two lines, since one with both
    // places passes the console's 128-byte cut (InGameConsolePrinter.cpp:26-28)
    private logBody(prop: Prop, refr: ObjectReference) {
        const body = this.readBody(prop);
        if (body === undefined) {
            return;
        }
        const ref = PropService.read(refr);
        let turned = 0;
        for (let i = 0; i < 3; ++i) {
            turned = Math.max(turned, Math.abs(angleDelta(body.rot[i], ref.rot[i])));
        }
        const id = prop.refr.toString(16);
        const record = this.recordOf(prop.refr);
        const fromRecord = record ? `its record by ${ObjectReferenceEx.getDistance(body.pos, record.pos).toFixed(1)} units` : "no record known";
        logLab(TAG, `body ${id} node (${triple(body.pos)}) (${triple(body.rot)}), motion ${body.motion}`);
        logLab(TAG, `body ${id} off its reference by ${ObjectReferenceEx.getDistance(body.pos, ref.pos).toFixed(1)} units and ${turned.toFixed(1)} degrees, ${fromRecord}`);
    }

    // the "body" lines asked for at `at` (propBodyReadLog on), and at the
    // first track once
    private bodyLogAt(prop: Prop, at: number) {
        if (this.bodyReadLog()) {
            prop.bodyLogDue = Math.max(at, 1);
        }
    }

    private bodyTrack(prop: Prop, at: number) {
        if (!prop.bodyTracked) {
            prop.bodyTracked = true;
            this.bodyLogAt(prop, at);
        }
    }

    private formOf(refr: number): FormModel | undefined {
        const world = this.controller.lookupListener(RemoteServer).getWorldModel();
        const prop = this.props.get(refr);
        if (prop) {
            const f = world.forms[prop.formIdx];
            if (f && f.refrId === refr) {
                return f;
            }
        }
        return world.forms.find((f) => f !== undefined && f.refrId === refr);
    }

    private refOf(prop: Prop): ObjectReference | null {
        return this.sp.ObjectReference.from(this.sp.Game.getFormEx(prop.localId));
    }

    private myId(): number {
        return this.controller.lookupListener(RemoteServer).getMyRemoteRefrId();
    }

    // On unless the client setting propPhysics is false. The union merged
    // this verb dark (off by default, rule 3 and D68 of 2026-10-30, no green
    // T3 scenario then); this train carries b-prop-push as the verb's own
    // scenario and merges only when it is green, so the default is on again
    // here, and parity never sees it on with a red scenario
    private enabled(): boolean {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        return !s || s["propPhysics"] !== false;
    }

    // Physics rung 3 (ADR-071 decision 4, "caster only"): the platform's
    // impulse rule, on while propPhysics is and the client setting
    // propImpulseCause is not false, told when it changes (at the first
    // update, and at a settings change). On, another seat's cause is zeroed at
    // the impulse site on every physics body (D-PH5) and this seat's own
    // raises propImpulse; off, every engine pushes by its own numbers (the
    // lab's A/B, .claude/rules/native.md's kill switch). The platform answers
    // whether its ScaleGameplayImpulseForce replacement went in; an older one
    // has no rule, said once
    private applyImpulseRule() {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        const want = this.enabled() && (s === undefined || s["propImpulseCause"] !== false);
        if (want === this.impulseRuleSent) {
            return;
        }
        this.impulseRuleSent = want;
        try {
            const replaced = this.sp.setImpulseCauseRule(want);
            if (!want) {
                logLab(TAG, "impulse rule off: every engine pushes by its own numbers");
            } else if (replaced) {
                logLab(TAG, "impulse rule on: another seat's cause is zeroed here, this seat's claims what it moved");
            } else {
                logLab(TAG, "impulse rule on, but the platform's impulse replacement did not go in: no push is zeroed here");
            }
        } catch (e) {
            logError(this, `setImpulseCauseRule unavailable on this platform: ${e}`);
        }
    }

    // the follower's delay behind the owner's clock: the client setting
    // propFollowDelayMs, 0 to FOLLOW_DELAY_MAX_MS ms, else FOLLOW_DELAY_MS
    private followDelayMs(): number {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        const v = s !== undefined ? s["propFollowDelayMs"] : undefined;
        return typeof v === "number" && isFinite(v) && v >= 0 && v <= FOLLOW_DELAY_MAX_MS ? v : FOLLOW_DELAY_MS;
    }

    // the "body" lines (logBody), off unless the client setting
    // propBodyReadLog is true
    private bodyReadLog(): boolean {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        return s !== undefined && s["propBodyReadLog"] === true;
    }

    // the reference's own place and turn: the proximity read's, and the
    // fallback where the platform reads no body
    private static read(refr: ObjectReference): Pose {
        return {
            pos: ObjectReferenceEx.getPos(refr),
            rot: [refr.getAngleX(), refr.getAngleY(), refr.getAngleZ()],
        };
    }

    // Havok's dynamic motion types (MotionType 1, 2, 3 and 6; 4 is Keyframed,
    // 5 Fixed)
    private static isDynamic(motion: number): boolean {
        return motion === MotionType.Dynamic || motion === MotionType.SphereInertia
            || motion === MotionType.BoxInertia || motion === MotionType.ThinBoxInertia;
    }

    // whether two transforms differ past the thresholds (any axis of the turn)
    private static apart(a: Pose, b: Pose, units: number, degrees: number): boolean {
        if (ObjectReferenceEx.getDistance(a.pos, b.pos) > units) {
            return true;
        }
        for (let i = 0; i < 3; ++i) {
            if (Math.abs(angleDelta(a.rot[i], b.rot[i])) > degrees) {
                return true;
            }
        }
        return false;
    }

    private static newProp(refr: number, localId: number, formIdx: number, cell: number, distance: number, now: number): Prop {
        return {
            refr,
            localId,
            formIdx,
            cell,
            distance,
            trackedAt: now,
            placed: false,
            placing: false,
            placedAt: 0,
            landsAt: 0,
            simulating: false,
            effective: undefined,
            motionAsked: undefined,
            loaded3D: false,
            lastSent: undefined,
            lastSendAt: 0,
            sentHeld: false,
            awakeAt: 0,
            streaming: false,
            announce: false,
            handOff: undefined,
            held: false,
            heldAt: 0,
            holdLost: false,
            follow: undefined,
            keyframeDue: false,
            lastAppliedLog: 0,
            lastClaimAt: 0,
            restAt: 0,
            bodyLogDue: 0,
            bodyTracked: false,
            badBodySaid: false,
            trap: false,
        };
    }

    // ADR-068 D-S3 point 4: the batch's motion calls since the last update. A
    // family reference this seat's own script set moving (SetMotionType
    // Dynamic, or an impulse) is a trap body, tracked at once and claimed by
    // touch 3 once per firing: not while a live word names this seat, nor
    // again within TRAP_FIRING_MS of this seat's last claim. A motion type the
    // script set is the body's from here (a Dynamic body takes no motion set
    // from this service, so its fall carries on). A trap's trigger set moving
    // by its own script (the Tripwire's [Active] SetMotionType(self, 1, true))
    // is left to it: no body, no claim (trapBodyRole)
    private claimTrapMotions(now: number) {
        const motions = this.trapMotions;
        this.trapMotions = [];
        const myId = this.myId();
        for (const m of motions) {
            const refr = localIdToRemoteId(m.localId);
            if (!refr) {
                continue;
            }
            const moving = m.impulse || m.motionType === MotionType.Dynamic;
            let body = this.trapBodies.get(refr);
            if (!body) {
                // a script parking its reference (a Keyframed set) makes no
                // body; the batch carries a family reference's calls alone,
                // read again here, and a trap's trigger makes none
                if (!moving || this.trapBodyRole(refr, m.localId, "moved by its own script") !== "body") {
                    continue;
                }
                body = this.addTrapBody(refr, m.localId);
                if (!body) {
                    continue;
                }
            }
            const prop = this.trackTrapBody(refr, body, now, false);
            if (!prop) {
                continue;
            }
            if (m.motionType !== undefined) {
                prop.motionAsked = m.motionType as MotionType;
            }
            if (!moving) {
                continue;
            }
            // a word naming this seat is its own firing still streaming the
            // body (the rest's word names nobody, A4)
            const word = this.owners.get(refr);
            const named = myId !== 0 && word !== undefined && word.player === myId;
            if (named || now - body.claimedAt < TRAP_FIRING_MS) {
                continue;
            }
            body.claimedAt = now;
            logLab(TRAP_TAG, `trap body ${refr.toString(16)} set moving by this seat's script: claimed by impulse`);
            this.claim(prop, TOUCH_IMPULSE, now);
        }
    }

    // ADR-068 D-S3 point 4: the owner words since the last update, read
    // against the trap family (an owner word on a family reference is a trap
    // body's: the server grants touch 3 alone on a world object). A word that
    // names another seat puts this seat behind that seat's firing: the body is
    // followed and Keyframed at once where this seat's copy stands (in the
    // race, a round trip into its own fall) and the stream takes it from
    // there. A word that names this seat or nobody ends a follow. A word on a
    // trap's trigger (another seat's claim of one, from a build before
    // playtest 48's fix) is never followed (trapBodyRole)
    private followTrapWords(now: number) {
        const words = this.trapWords;
        this.trapWords = [];
        const myId = this.myId();
        for (const refr of words) {
            const word = this.owners.get(refr);
            if (!word) {
                continue;
            }
            const named = `named by ${word.player.toString(16)}'s owner word`;
            let body = this.trapBodies.get(refr);
            if (!body) {
                const tracked = this.props.get(refr);
                if (tracked && !tracked.trap) {
                    continue; // an item's word
                }
                const localId = remoteIdToLocalId(refr);
                if (!localId || this.trapBodyRole(refr, localId, named) !== "body") {
                    continue;
                }
                body = this.addTrapBody(refr, localId);
                if (!body) {
                    continue;
                }
            } else if (this.trapBodyRole(refr, body.localId, named) === "trigger") {
                // whatever made the body (no path here makes one of a
                // trigger), a trigger is never followed: a follow already up
                // on it ends as a word naming this seat ends one
                this.releaseTrapFollow(refr, "a trap trigger is its own seat's");
                continue;
            }
            if (word.player === 0 || word.player === myId) {
                this.releaseTrapFollow(refr, word.player === 0 ? "the word names nobody" : "the word names this seat");
                continue;
            }
            const prop = this.trackTrapBody(refr, body, now, false);
            this.holdTrapBody(refr);
            const ref = prop ? this.refOf(prop) : null;
            if (body.followed && prop && ref && ref.is3DLoaded() && prop.motionAsked !== MotionType.Keyframed) {
                prop.loaded3D = true;
                this.setMotion(prop, ref, MotionType.Keyframed, true);
            }
        }
    }

    // a followed trap body's owner sent its at-rest sample: this seat's own
    // motion calls on it run again (D73's rest then makes it Dynamic)
    private restTrapBodies() {
        const rests = this.trapRests;
        this.trapRests = [];
        for (const refr of rests) {
            this.releaseTrapFollow(refr, "its owner's at-rest sample");
        }
    }

    // a follow no word names another seat for any more ends here: its word
    // gone with a reconnect's fresh table (the words themselves end a follow
    // at a word naming nobody or this seat, followTrapWords, and at the
    // owner's rest, restTrapBodies)
    private releaseOrphanTrapFollows(myId: number) {
        this.trapBodies.forEach((body, refr) => {
            if (body.followed && !this.theirs(refr, myId)) {
                this.releaseTrapFollow(refr, "no word names another seat");
            }
        });
    }

    // whether another seat's word names the prop, as ownerOf reads a word (A1:
    // the word alone, no lease and no presence test)
    private theirs(refr: number, myId: number): boolean {
        const word = this.owners.get(refr);
        return word !== undefined && word.player !== 0 && word.player !== myId;
    }

    // a trap body another seat's word names is followed, once
    private holdTrapBody(refr: number) {
        const body = this.trapBodies.get(refr);
        const word = this.owners.get(refr);
        if (!body || body.followed || !word || !this.theirs(refr, this.myId())) {
            return;
        }
        this.setFollowed(refr, body, true);
        logLab(TRAP_TAG, `trap body ${refr.toString(16)} followed: ${word.player.toString(16)}'s firing streams it, Keyframed until its rest`);
    }

    private releaseTrapFollow(refr: number, why: string) {
        const body = this.trapBodies.get(refr);
        if (!body || !body.followed) {
            return;
        }
        this.setFollowed(refr, body, false);
        logLab(TRAP_TAG, `trap body ${refr.toString(16)} followed no more: ${why}`);
    }

    // the platform's word (setPropFollowed): while true this seat's own
    // SetMotionType and ApplyHavokImpulse on the body run neutralized
    // (Keyframed, a zero impulse) and are never batched
    private setFollowed(refr: number, body: TrapBody, followed: boolean) {
        body.followed = followed;
        try {
            if (!this.sp.setPropFollowed(body.localId, followed)) {
                logError(this, `setPropFollowed(${body.localId.toString(16)}, ${followed}) refused for trap body ${refr.toString(16)}`);
            }
        } catch (e) {
            logError(this, `setPropFollowed unavailable on this platform: ${e}`);
        }
    }

    private followedTrap(prop: Prop): boolean {
        return prop.trap && this.trapBodies.get(prop.refr)?.followed === true;
    }

    // ADR-068 D-S3 point 4 (playtest 48, 2026-11-06): what a reference is to
    // the trap bodies, read where a body would be made or followed. A family
    // reference is a body ("body") unless it is a trap's trigger ("trigger":
    // getTrapRole's trigger, a script on it reaching TrapTriggerBase; the
    // plates, the tripwire, the bone alarms, the linkers: TrapFamily.cpp),
    // which never is: its own script's motion calls claim nothing, another
    // seat's word on it is never followed and its record never places it, so
    // each seat's own copy moves its own. Embershard's wire 0xb6d3f is why:
    // its Tripwire state's [Active] onBeginState opens with SetMotionType(self,
    // 1, true), so the host claimed it by touch 3 and the guest followed it,
    // its own SetMotionType run Keyframed ("a trap body another seat
    // streams"), and nothing the host's stream carries moves a rope:
    // TrapTripwire01.nif is a Havok rope (13 rigid bodies in two ball-socket
    // constraint chains and a phantom, thuum-cf's read of the mesh) and the
    // stream carries one transform per reference, so the guest's rope hung
    // taut. "none" is no role or no family; a trigger is said once per
    // reference; an older platform answers no trigger, so its rule is the one
    // before
    private trapBodyRole(refr: number, localId: number, how: string): "body" | "trigger" | "none" {
        const role = ObjectReferenceEx.trapRole(localId);
        if (role === null || !role.family) {
            return "none";
        }
        if (role.trigger !== true) {
            return "body";
        }
        if (!this.trapTriggersSaid.has(refr)) {
            this.trapTriggersSaid.add(refr);
            logLab(TRAP_TAG, `trap trigger ${refr.toString(16)} ${how}: its own seat's, never a trap body`);
        }
        return "trigger";
    }

    private addTrapBody(refr: number, localId: number): TrapBody | undefined {
        if (this.trapBodies.size >= TRAP_BODIES_MAX) {
            if (!this.trapBodiesFullSaid) {
                this.trapBodiesFullSaid = true;
                logError(this, `${TRAP_BODIES_MAX} trap bodies held: ${refr.toString(16)} and later ones not taken`);
            }
            return undefined;
        }
        const body: TrapBody = { localId, claimedAt: 0, followed: false };
        this.trapBodies.set(refr, body);
        return body;
    }

    // a trap body's prop, tracked now if it is not (its reference loaded);
    // atRecord, the rescan's, places it at its record first (placeAtRecord),
    // while a body a firing set moving or a word put behind another seat's
    // stream is its firing's from here, never its record's
    private trackTrapBody(refr: number, body: TrapBody, now: number, atRecord: boolean): Prop | undefined {
        const known = this.props.get(refr);
        if (known) {
            known.trap = true;
            return known;
        }
        const ref = this.sp.ObjectReference.from(this.sp.Game.getFormEx(body.localId));
        if (!ref) {
            return undefined;
        }
        const player = this.sp.Game.getPlayer();
        const distance = player ? ObjectReferenceEx.getDistance(ObjectReferenceEx.getPos(player), ObjectReferenceEx.getPos(ref)) : 0;
        const cell = ref.getParentCell();
        const prop = PropService.newProp(refr, body.localId, -1, cell ? cell.getFormID() : 0, distance, now);
        prop.trap = true;
        prop.placed = !atRecord;
        prop.placedAt = now;
        // the 3D as found, so the first tick takes no load of it for a reload
        // and sets no motion type over the script's
        prop.loaded3D = ref.is3DLoaded();
        this.props.set(refr, prop);
        this.byLocal.set(body.localId, refr);
        logTrace(this, `prop ${refr.toString(16)} tracked, a trap body, ${Math.round(distance)} units off`);
        return prop;
    }

    // ADR-068 D-S3 point 4: a family reference whose record (its CreateActor's
    // pose, or the last sample recorded here) rests more than TRAP_MOVED_UNITS
    // from this engine's copy is a trap body a firing moved before this
    // session (a restart, a late join): taken, and placed at its record when
    // tracked ("placed at its recorded rest"). Judged once per binding, as
    // isItem's verdict is; a body this seat already knows is its firing's. A
    // trap's trigger is none (trapBodyRole), whatever its record says (a world
    // saved while a build before playtest 48's fix streamed one)
    private judgeTrapRecord(refrId: number, rec: { pos: NiPoint3 }) {
        if (this.trapBodies.has(refrId)) {
            return;
        }
        const known = this.kinds.get(refrId);
        if (known && known.refr === refrId && known.trap !== undefined) {
            return;
        }
        const refr = this.sp.ObjectReference.from(this.sp.Game.getFormEx(refrId));
        if (!refr) {
            return; // not loaded in this game yet: judged when it is
        }
        const off = ObjectReferenceEx.getDistance(ObjectReferenceEx.getPos(refr), rec.pos);
        const moved = off > TRAP_MOVED_UNITS
            && this.trapBodyRole(refrId, refrId, `${Math.round(off)} units from its record`) === "body";
        this.kinds.set(refrId, { refr: refrId, item: false, trap: moved });
        if (moved && this.addTrapBody(refrId, refrId)) {
            logTrace(this, `trap body ${refrId.toString(16)}: its record rests ${Math.round(off)} units from this engine's copy`);
        }
    }

    // the tracked props by the server's id, and the same by this engine's id
    private readonly props = new Map<number, Prop>();
    private readonly byLocal = new Map<number, number>();
    // the server's words, by prop, tracked or not
    private readonly owners = new Map<number, Owner>();
    // this seat's last sample by prop: the epoch it carried and its sequence
    // there (send), dropped once a word raises the epoch past it (onOwner)
    private readonly sent = new Map<number, { epoch: number; seq: number }>();
    // the update's due samples (stream), and the send budget's tokens and
    // their last refill (sendDue)
    private due: Due[] = [];
    private budget = SEND_BUDGET_BURST;
    private budgetAt = 0;
    private readBodiesSaid = false;
    // each local id's base verdict, by the record it is bound to (trap: D-S3's
    // verdict on a family reference's record, for a reference no item)
    private readonly kinds = new Map<number, { refr: number; item: boolean; trap?: boolean }>();
    private readonly echoSaid = new Set<number>();
    // the controller's contacts since the last update, by local id
    private contacts: Array<{ localId: number; layer: number; motionType: number }> = [];
    // physics rung 3: the impulse events since the last update, the impacts
    // awaiting their claim by impact number (a cone's for CONE_BATCH_MS), the
    // awake samples of untracked props awaiting their tracking (trackStreams),
    // and the impulse rule as last told to the platform (applyImpulseRule)
    private impulses: Array<{ impact: number; kind: PropImpulseEvent["kind"]; cause: number; refrs: Member[] }> = [];
    private readonly impactBatches = new Map<number, { kind: PropImpulseEvent["kind"]; cause: number; firstAt: number; refs: Member[] }>();
    private streamsUntracked: Array<{ message: PropTransformMessage; at: number }> = [];
    private impulseRuleSent: boolean | undefined = undefined;
    // the player's drops awaiting the server's reference
    private drops: Array<{ baseId: number; at: number }> = [];
    private lastRescan = 0;
    // ADR-068 D-S3 point 4: the trap bodies by the server's id; the batch's
    // motion calls, the owner words and the followed bodies' at-rest samples
    // since the last update (JS state from their handlers)
    private readonly trapBodies = new Map<number, TrapBody>();
    private trapMotions: Array<{ localId: number; motionType: number | undefined; impulse: boolean }> = [];
    private trapWords: number[] = [];
    private trapRests: number[] = [];
    private trapBodiesFullSaid = false;
    // the trap triggers trapBodyRole has said, by the server's id (a dungeon's
    // handful)
    private readonly trapTriggersSaid = new Set<number>();
}
