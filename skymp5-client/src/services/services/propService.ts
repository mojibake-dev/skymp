import { ContainerChangedEvent, MotionType, ObjectReference, PropContactEvent, ScriptCall } from "skyrimPlatform";
import { MsgType } from "../../messages";
import { logError, logLab, logTrace } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { PropOwnerMessage } from "../messages/propOwnerMessage";
import { PropTransformMessage } from "../messages/propTransformMessage";
import { FormTypeEx } from "../../extensions/formTypeEx";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { NiPoint3 } from "../../sync/movement";
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
// TRACK_MAX of them
const TRACK_RADIUS = 1024;
const TRACK_MAX = 64;
// game-file item references the server subscribed this seat to, remembered from their CreateActor (see engineRefs)
const ENGINE_REFS_MAX = 1024;
// A prop just tracked is left alone this long: the view's SpawnProcess places
// a server-made copy over a few frames (setPosition, enable, then its own
// setMotionType Keyframed), and a motion type set under that chain would be
// overwritten by its last step
const TRACK_GRACE_MS = 1000;
// The simulator's stream (ADR-058 decision 2): a sample when the body moved
// this far or turned this much since the last sample sent, at most one per
// SEND_MIN_MS; one sample more, asleep, when it has not stirred (STIR_*: any
// motion the read resolves) for SLEEP_AFTER_MS
const SEND_MIN_MS = 100;
const SAMPLE_UNITS = 2;
const SAMPLE_DEGREES = 2;
const STIR_UNITS = 0.25;
const STIR_DEGREES = 0.25;
const SLEEP_AFTER_MS = 500;
// The heartbeat (ADR-071's D-PH4 B, decision 6's 500 ms on v1 samples): an
// awake body this seat streams sends a sample at least this often, moved or
// not, and a held one never sends the asleep sample (A6), so the server never
// reads a live stream stalled (its PROP_MOVING_MS, 1600: about three of these
// and a frame, so two lost beats in a row are no stall) and no follower rests
// its copy mid-stream (STREAM_LIVE_MS, the same); a creeping body under
// SAMPLE_UNITS and a still hand both stay on the wire
const STREAM_HEARTBEAT_MS = 500;
// a resting body this seat simulates is read for a stir ten times a second,
// not every frame (sixty-four host-default props at rest cost nine natives
// each per read)
const REST_READ_MS = 100;
// The follower's apply: a glide over the sample interval (floored, capped)
// under JUMP_UNITS, a placement at once from there and at sleep
const GLIDE_MIN_MS = 50;
const GLIDE_MAX_MS = 300;
const GLIDE_MIN_SPEED = 8;
const JUMP_UNITS = 256;
// The proximity touch, the Havok contact's stand-in before the contact event
// (ADR-058's consequences), behind the sub-setting propContactFallback
// (default off): a prop this seat does not simulate that moved by itself (no
// sample from the stream in TOUCH_QUIET_MS, no placement of ours) while the
// player stood within TOUCH_RADIUS; read every TOUCH_READ_MS. A contact's
// claim, the event's or the stand-in's, goes at most once per CLAIM_EVERY_MS
// per prop
const TOUCH_RADIUS = 128;
const TOUCH_QUIET_MS = 300;
const TOUCH_READ_MS = 100;
const CLAIM_EVERY_MS = 2000;
// A drop's claim: the server's reference for an item this player dropped is
// matched by its base within this window and this distance of the player
const DROP_MATCH_MS = 3000;
const DROP_MATCH_UNITS = 512;
// "applied from the stream" is said once per prop per this
const APPLIED_LOG_MS = 10_000;
// D73 (docs/DECISIONS.md, ADR-058 decision 3 refined): a follower's body is
// Keyframed only while another seat streams it awake, this long after the
// last awake sample; then, and REST_DYNAMIC_MS after an asleep sample's
// apply, it is Dynamic and may wake, so a kick or a grab acts on it at once.
// The server's PROP_MOVING_MS, the same length: about three heartbeat
// intervals (STREAM_HEARTBEAT_MS) and a frame, so two lost beats in a row
// still read live (accessible: Joshi, "HeartBeat", Patterns of Distributed
// Systems; foundational: Ongaro and Ousterhout, Raft, USENIX ATC 2014,
// section 5.6). UNMEASURED
const STREAM_LIVE_MS = 1600;
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

type Sample = { pos: NiPoint3; rot: NiPoint3; asleep: boolean; at: number };

// one tracked prop, by the server's id
type Prop = {
    refr: number;        // the server's id, the wire's refr
    localId: number;     // this engine's reference for it (the view's binding)
    formIdx: number;     // its index in the world model, checked at each use
    cell: number;        // the engine's parent cell at the last rescan (the host rule's cell)
    distance: number;    // to the player at the last rescan
    trackedAt: number;
    // the record's transform imposed once, before anything else (the login's
    // place, which the view's own apply may or may not have reached yet)
    placed: boolean;
    placing: boolean;
    placedAt: number;
    simulating: boolean;           // this seat's body is Dynamic and streams
    effective: number | undefined; // the simulator last computed (0 the host, by fallback)
    motionAsked: MotionType | undefined;
    loaded3D: boolean;
    // the simulator's side
    lastSent: Sample | undefined;  // the last sample out, or the first read as a baseline
    lastSendAt: number;
    lastRead: Sample | undefined;
    stirredAt: number;
    streaming: boolean;            // awake samples went out since the last sleep
    announce: boolean;             // a granted word named this seat: its first read goes out at once (see stream)
    // the hand: this seat's engine holds the body (Game.getPlayerGrabbedRef named it at the update's read, readHolds)
    held: boolean;
    heldAt: number;                // when the hold began (the grab's claim went then)
    holdLost: boolean;             // another seat's word came after the grab: this hand let go (see tick)
    // the follower's side
    pending: Sample | undefined;   // the newest sample in, applied on the next update
    lastSampleAt: number;
    prevSampleAt: number;
    lastAppliedLog: number;
    lastTouchRead: Sample | undefined;
    lastTouchReadAt: number;
    lastClaimAt: number;
    lastSampleAsleep: boolean;     // the newest sample in said asleep (the stream ended there)
    restAt: number;                // when the asleep sample came, for the Dynamic set after its apply (0 none)
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

// thuum docs/verbs/prop-physics.md (ADR-058; Eli: "the canon perfection of
// multiplayer phys objects is gmod"): a physics prop (a basket, a dropped
// sword: an item reference the server knows) is simulated by ONE seat at a
// time and shown by the stream on every other. The simulator's body is
// Dynamic and PropTransform carries its transform at 100 ms while it moves
// and once more when it rests (R2: this seat's Havok computes, the server
// records); every other seat's body stays Keyframed, as the view forces every
// item on arrival (objectReferenceEx.ts dealWithRef, spawnProcess.ts), and
// takes the stream's transform glided over the sample interval, so it neither
// falls nor fights the stream. Who simulates is the server's word alone
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
// claim sent while the engine names the body and its release when the hold
// ends; a drop; the proximity stand-in below stays behind
// propContactFallback. An awake body streams at least every
// STREAM_HEARTBEAT_MS and a held one never rests on the wire. Slice one: items
// only (FormTypeEx.isItem's types), within TRACK_RADIUS of the player, the
// nearest TRACK_MAX. Everything the engine is asked happens on the update
// (a message handler queues; Skyrim Platform refuses natives in its context).
//
// HYPOTHESIS, until x-prop-push and x-prop-pass: that setMotionType flips a
// clutter body between Keyframed and Dynamic cleanly both ways (Dynamic
// without the wake, so the host's props rest as the file's clutter does until
// a contact wakes them); that a glide over the sample interval reads as the
// simulator's motion a sample behind (nothing predicts, ADR-058 decision 3);
// that 500 ms without a stir is the body's sleep (Skyrim Platform exposes no
// Havok state); that the contact callback fires on this seat for a Keyframed
// body the player walks into (the re-analyst's static read, thuum
// docs/research/m3/re-havok-contact.md; lab/frida/prop-contact.js is its
// trace), so a non-host's walk into a basket claims it; the grab and the
// drop are the slice's sure touches, and the proximity stand-in
// (propContactFallback) fires only where a body this seat does not simulate
// moves locally, which a Keyframed body never does. HYPOTHESIS (physics rung
// 1): that Game.getPlayerGrabbedRef names the body for exactly as long as the
// engine's grab holds it, from StartGrabObject to DestroyMouseSprings (the
// player's grabbedObject, CommonLibSSE-NG P/PlayerCharacter.h:558 and :697;
// thuum docs/engine/havok-props.md E40 and E41 read the grab's ends, never
// this native), and never names a body a refused grab closed in its own
// frame (E43); and that a held body this seat sets Keyframed is let go by the
// engine, as a grab on a Keyframed body closes at once (E28, read at a grab's
// start, never mid-hold).
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

    // A sample from the prop's simulator, relayed (pure JavaScript here: the
    // apply waits for the update). The record in this seat's model follows
    // every sample, tracked or not, so the view places an untracked prop by
    // it and a prop tracked later starts from it
    private onTransform(e: ConnectionMessage<PropTransformMessage>) {
        const m = e.message;
        const now = Date.now();
        const sample: Sample = {
            pos: [m.pos[0], m.pos[1], m.pos[2]],
            rot: [m.rot[0], m.rot[1], m.rot[2]],
            asleep: m.asleep,
            at: now,
        };
        this.recordSample(m.refr, sample);
        // ADR-068 D-S3: a followed trap body's at-rest sample ends its follow,
        // after this update's apply (restTrapBodies), tracked or not
        if (m.asleep && this.trapBodies.get(m.refr)?.followed === true) {
            this.trapRests.push(m.refr);
            if (this.trapRests.length > 64) {
                this.trapRests.shift();
            }
        }
        const prop = this.props.get(m.refr);
        if (!prop) {
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
        prop.pending = sample;
        prop.prevSampleAt = prop.lastSampleAt;
        prop.lastSampleAt = now;
        prop.lastSampleAsleep = sample.asleep;
        prop.restAt = sample.asleep ? now : 0;
    }

    // The server's word on who simulates a prop from now; an epoch below the
    // known one is a late word (one counter per prop, the server's) and an
    // equal one replaces (A5: the answer to a refused claim or sample repeats
    // the standing word, so a seat that applied it wrongly heals). The body's
    // motion type follows on the update
    private onOwner(e: ConnectionMessage<PropOwnerMessage>) {
        const m = e.message;
        const known = this.owners.get(m.refr);
        if (known && m.epoch < known.epoch) {
            logTrace(this, `owner word for prop ${m.refr.toString(16)} at epoch ${m.epoch} ignored: epoch ${known.epoch} known`);
            return;
        }
        this.owners.set(m.refr, { player: m.player, epoch: m.epoch, at: Date.now() });
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
    // their epochs clear (HostService's rule for its grants)
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
        const firing = body !== undefined && Date.now() - body.claimedAt < TRAP_FIRING_MS;
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
        this.drops.push({ baseId, at: Date.now() });
        if (this.drops.length > 16) {
            this.drops.shift();
        }
        logTrace(this, `drop of base ${baseId.toString(16)} recorded for the copy's match`);
    }

    private onUpdate() {
        if (!this.enabled()) {
            if (this.props.size > 0) {
                this.untrackAll();
            }
            this.trapMotions = [];
            this.trapWords = [];
            this.trapRests = [];
            return;
        }
        const now = Date.now();
        // ADR-068 D-S3's trap bodies first: a firing's claim, and a word's
        // follow made Keyframed, before the rescan keeps them and the ticks
        // stream or follow them
        if (this.trapMotions.length > 0) {
            this.claimTrapMotions(now);
        }
        if (this.trapWords.length > 0) {
            this.followTrapWords(now);
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
            const playerPos = ObjectReferenceEx.getPos(player);
            const myId = this.myId();
            const fallback = this.contactFallback();
            for (const prop of Array.from(this.props.values())) {
                this.tick(prop, now, myId, playerPos, fallback);
            }
        }
        // a followed trap body's at-rest sample, applied by the ticks above
        if (this.trapRests.length > 0) {
            this.restTrapBodies();
        }
    }

    // One prop, one update: the simulator computed from the owner table, the
    // body's motion type set to match (once, and again when its 3D reloads),
    // then the stream out or the stream's apply and, under the fallback, the
    // proximity read
    private tick(prop: Prop, now: number, myId: number, playerPos: NiPoint3, fallback: boolean) {
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
            prop.effective = owner;
        }
        // A4's rest reaches a follower as two messages, the owner's asleep
        // sample (relayed) and the word (player 0, or this seat's own grant),
        // often in one update; flip clears a pending sample and stops a glide,
        // so a seat the word makes the simulator follows on until
        // REST_DYNAMIC_MS after the rest it heard, and its copy takes the rest
        // pose before it goes Dynamic (the review of b7151a42: the host's copy
        // went Dynamic at the last awake pose, the rest never applied)
        const simulate = myId !== 0 && owner === myId
            && (prop.simulating || prop.restAt === 0 || now - prop.restAt >= REST_DYNAMIC_MS);
        // A6: a hand whose grab another seat's later word overrides (a double
        // grab lost, the server's Held answer naming the holder) lets go, so
        // no two hands move one prop (ADR-063 decision 3)
        const lostHold = prop.held && !prop.holdLost && word !== undefined && word.at > prop.heldAt
            && word.player !== 0 && word.player !== myId;
        const near = prop.distance <= TOUCH_RADIUS + 512;
        const touchReadDue = fallback && !prop.trap && !simulate && near && now - prop.lastTouchReadAt >= TOUCH_READ_MS;
        // D73's rest: a follower's Keyframed body goes Dynamic REST_DYNAMIC_MS after
        // the asleep sample or STREAM_LIVE_MS after the last awake one, so a kick
        // or a grab acts on it at once; the check below needs the reference, so
        // the tick must reach it (x-prop-grab v8 202618: with no pending sample
        // and the touch fallback off the tick returned here every time, no seat
        // ever logged "at rest: Dynamic", and c2's E on the still-Keyframed basket
        // picked it up instead of grabbing it)
        // (a followed trap body is held Keyframed until its owner's at-rest
        // sample, not let go when the stream goes quiet: D-S3)
        const restDue = !simulate && prop.placed && prop.motionAsked === MotionType.Keyframed
            && ((prop.restAt !== 0 && now - prop.restAt >= REST_DYNAMIC_MS)
                || (!this.followedTrap(prop) && !prop.lastSampleAsleep && now - prop.lastSampleAt >= STREAM_LIVE_MS));
        const wantsRef = simulate || !prop.placed || prop.simulating !== simulate || prop.motionAsked === undefined
            || prop.pending !== undefined || touchReadDue || restDue || lostHold;
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
        if (prop.simulating) {
            this.stream(prop, refr, now);
        } else {
            this.follow(prop, refr, now, playerPos, touchReadDue);
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
    // here) imposed once on the body before it is simulated or followed: an
    // engine reference moved in an earlier session stands at its file place
    // until something moves it to the record, and the view's own apply may
    // not have run yet for a reference just bound
    private placeAtRecord(prop: Prop, refr: ObjectReference, now: number) {
        if (prop.placing) {
            return;
        }
        const form = this.formOf(prop.refr);
        const m = form ? form.movement : undefined;
        // a game-file item has no model: its record's pose is the CreateActor's (engineRefs)
        const pose = m ? { pos: m.pos, rot: m.rot } : this.engineRefs.get(prop.refr);
        if (!pose) {
            prop.placed = true;
            prop.placedAt = now;
            return;
        }
        const read = PropService.read(refr, now);
        const target: Sample = { pos: pose.pos, rot: pose.rot, asleep: false, at: now };
        if (!PropService.apart(read, target, SAMPLE_UNITS, SAMPLE_DEGREES)) {
            prop.placed = true;
            prop.placedAt = now;
            return;
        }
        prop.placing = true;
        const [x, y, z] = pose.pos;
        const [ax, ay, az] = pose.rot;
        const done = (why: string, placed = true) => {
            prop.placing = false;
            prop.placed = true;
            prop.placedAt = Date.now();
            logTrace(this, `prop ${prop.refr.toString(16)} placed at its record${why}`);
            if (prop.trap && placed) {
                logLab(TRAP_TAG, `trap body ${prop.refr.toString(16)} placed at its recorded rest`);
            }
        };
        if (prop.loaded3D) {
            // H-P4: the placement by the engine's keyframed translate, the
            // collision moving with the node (a setPosition'd body flew at its
            // later Dynamic set, pass v10b 025124)
            const distance = ObjectReferenceEx.getDistance(read.pos, target.pos);
            const speed = Math.max(GLIDE_MIN_SPEED, distance / (JUMP_MS / 1000));
            refr.stopTranslation();
            refr.translateTo(x, y, z, ax, ay, az, speed, 0);
            done(" (translated)");
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
    // before the body goes Dynamic; the stream's baseline starts afresh
    private flip(prop: Prop, refr: ObjectReference, simulate: boolean, wake: boolean, now: number) {
        if (!prop.loaded3D) {
            return; // the motion type is the 3D's; set when it loads
        }
        if (simulate) {
            // a trap body this seat now simulates is its own again before its
            // Dynamic set (D-S3: a followed body's motion calls run neutralized)
            if (prop.trap) {
                this.releaseTrapFollow(prop.refr, "this seat simulates it");
            }
            refr.stopTranslation();
            prop.pending = undefined;
            // D73: a body already Dynamic (a resting follower kicked or grabbed
            // here, the claim granted) takes no motion set, so the hand's hold
            // and the kick's motion carry on; a Keyframed one (claimed while
            // another seat streamed it) goes Dynamic and may wake (x-prop-push
            // v9 021439: a Dynamic body denied activation took no contact)
            if (prop.motionAsked !== MotionType.Dynamic) {
                this.setMotion(prop, refr, MotionType.Dynamic, true);
            }
        }
        // the loss of the simulation sets no motion here: the new owner's
        // first awake sample makes the body Keyframed (follow), and a body
        // nobody streams rests Dynamic (D73)
        prop.simulating = simulate;
        prop.lastSent = undefined;
        prop.lastRead = undefined;
        prop.lastSendAt = 0;
        prop.streaming = false;
        prop.stirredAt = now;
        prop.lastTouchRead = undefined;
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
    // streams it awake right now (a sample within STREAM_LIVE_MS, not asleep)
    private restMotion(prop: Prop, simulate: boolean, now: number): MotionType {
        if (simulate) {
            return MotionType.Dynamic;
        }
        if (this.followedTrap(prop)) {
            return MotionType.Keyframed;
        }
        const live = prop.lastSampleAt !== 0 && !prop.lastSampleAsleep && now - prop.lastSampleAt < STREAM_LIVE_MS;
        return live ? MotionType.Keyframed : MotionType.Dynamic;
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

    // The simulator's read and stream: a sample when the body moved past the
    // thresholds since the last sent, at most every SEND_MIN_MS, and at least
    // every STREAM_HEARTBEAT_MS while it is awake (D-PH4 B), unreliable; the
    // sleep sample, reliable, when it has not stirred for SLEEP_AFTER_MS after
    // moving and no hand holds it (A6: a held body never rests on the wire,
    // whatever the stir test reads, havok-props.md E42); nothing more until it
    // moves again or a hand takes it
    private stream(prop: Prop, refr: ObjectReference, now: number) {
        if (!prop.streaming && !prop.held && prop.lastRead && now - prop.lastRead.at < REST_READ_MS) {
            return;
        }
        const read = PropService.read(refr, now);
        // a paused world (a menu, the console) stirs nothing, so the sleep clock
        // holds until the world runs again: x-prop-launch v3 190643 read a drop's
        // copy "asleep" under the dropper's InventoryMenu and the server took
        // its rest while the body had never settled (it was ejected at the
        // unpause)
        if (prop.streaming && this.sp.Utility.isInMenuMode()) {
            prop.stirredAt = now;
        }
        if (!prop.lastSent) {
            prop.lastRead = read;
            if (prop.announce) {
                // the grant's first sample: the stream is open from here, so the
                // asleep sample follows SLEEP_AFTER_MS later when nothing stirs
                // and no hand holds the body, and the server returns the prop
                // on it
                prop.announce = false;
                this.send(prop, read, false, now);
                prop.streaming = true;
                prop.stirredAt = now;
                logLab(TAG, `stream ${prop.refr.toString(16)} started at the grant`);
                return;
            }
            // the baseline: nothing goes out until the body moves from here
            prop.lastSent = read;
            prop.stirredAt = 0;
            return;
        }
        if (prop.lastRead && PropService.apart(read, prop.lastRead, STIR_UNITS, STIR_DEGREES)) {
            prop.stirredAt = now;
        }
        prop.lastRead = read;
        if (PropService.apart(read, prop.lastSent, SAMPLE_UNITS, SAMPLE_DEGREES)) {
            if (now - prop.lastSendAt < SEND_MIN_MS) {
                return;
            }
            this.send(prop, read, false, now);
            if (!prop.streaming) {
                prop.streaming = true;
                logLab(TAG, `stream ${prop.refr.toString(16)} started`);
            }
            return;
        }
        if (!prop.streaming) {
            if (prop.held) {
                // a hand took a resting body this seat simulates (the host's
                // own clutter): the stream opens at once, so the followers'
                // copies go Keyframed in the hand's place and the server's row
                // stays awake under the hold
                this.send(prop, read, false, now);
                prop.streaming = true;
                prop.stirredAt = now;
                logLab(TAG, `stream ${prop.refr.toString(16)} started: held`);
            }
            return;
        }
        if (prop.held || now - prop.stirredAt < SLEEP_AFTER_MS) {
            // awake, or in the hand: the heartbeat
            if (now - prop.lastSendAt >= STREAM_HEARTBEAT_MS) {
                this.send(prop, read, false, now);
            }
            return;
        }
        // the owner's own final sample: the server returns the prop to the
        // cell's host on it and its word (player 0) comes back; nothing is
        // inferred here (A4)
        this.send(prop, read, true, now);
        prop.streaming = false;
        logLab(TAG, `stream ${prop.refr.toString(16)} asleep`);
    }

    private send(prop: Prop, s: Sample, asleep: boolean, now: number) {
        const message: PropTransformMessage = {
            t: MsgType.PropTransform,
            refr: prop.refr,
            pos: [s.pos[0], s.pos[1], s.pos[2]],
            rot: [s.rot[0], s.rot[1], s.rot[2]],
            asleep,
        };
        this.controller.emitter.emit("sendMessage", {
            message,
            reliability: asleep ? "reliable" : "unreliable",
        });
        prop.lastSent = { pos: s.pos, rot: s.rot, asleep, at: now };
        prop.lastSendAt = now;
        // the record in this seat's model follows its own samples too
        this.recordSample(prop.refr, s);
    }

    // The follower: the newest sample applied, else the proximity read (the
    // contact's stand-in, propContactFallback)
    private follow(prop: Prop, refr: ObjectReference, now: number, playerPos: NiPoint3, touchReadDue: boolean) {
        const pending = prop.pending;
        if (pending) {
            prop.pending = undefined;
            // D73: the stream drives a Keyframed body; the first awake sample of
            // a burst sets it (a resting body is Dynamic)
            if (!pending.asleep && prop.loaded3D && prop.motionAsked !== MotionType.Keyframed) {
                this.setMotion(prop, refr, MotionType.Keyframed, true);
            }
            // D-S3: a trap body another seat streams awake again (its owner's
            // word unchanged since the rest that ended the follow) is followed
            // again
            if (!pending.asleep && prop.trap) {
                this.holdTrapBody(prop.refr);
            }
            this.apply(prop, refr, pending, now);
            // the stream moved it: the touch read starts over after the glide
            prop.lastTouchRead = undefined;
            prop.lastTouchReadAt = now;
            return;
        }
        // D73: back to Dynamic once the stream ended (REST_DYNAMIC_MS after the
        // asleep sample's apply) or went quiet (STREAM_LIVE_MS after the last
        // awake sample), so the resting body takes a kick or a grab at once
        if (prop.loaded3D && prop.motionAsked === MotionType.Keyframed) {
            const rested = prop.restAt !== 0 && now - prop.restAt >= REST_DYNAMIC_MS;
            const quiet = !this.followedTrap(prop) && !prop.lastSampleAsleep && now - prop.lastSampleAt >= STREAM_LIVE_MS;
            if (rested || quiet) {
                prop.restAt = 0;
                this.setMotion(prop, refr, MotionType.Dynamic, true);
                logTrace(this, `prop ${prop.refr.toString(16)} at rest: Dynamic (${rested ? "the stream ended" : "the stream went quiet"})`);
            }
        }
        if (!touchReadDue) {
            return;
        }
        prop.lastTouchReadAt = now;
        const read = PropService.read(refr, now);
        const prev = prop.lastTouchRead;
        prop.lastTouchRead = read;
        if (!prev) {
            return;
        }
        if (ObjectReferenceEx.getDistance(read.pos, prev.pos) <= SAMPLE_UNITS) {
            return;
        }
        // moved by itself: not by the stream, not by our placement
        if (now - prop.lastSampleAt < TOUCH_QUIET_MS + GLIDE_MAX_MS || now - prop.placedAt < TOUCH_QUIET_MS + GLIDE_MAX_MS) {
            return;
        }
        if (ObjectReferenceEx.getDistance(playerPos, read.pos) > TOUCH_RADIUS) {
            return;
        }
        if (now - prop.lastClaimAt < CLAIM_EVERY_MS) {
            return;
        }
        this.claim(prop, TOUCH_CONTACT, now);
    }

    // The stream's transform on the Keyframed body: at sleep or from
    // JUMP_UNITS, placed at once (setPosition, then setAngle); nearer, glided
    // by the engine's TranslateTo over the sample interval (the time between
    // this sample and the one before, floored and capped), the way a figure
    // is glided to its report (sync/movementApply.ts), so the body is still
    // moving when the next sample lands. HYPOTHESIS: that the glide reads as
    // the simulator's motion a sample behind
    private apply(prop: Prop, refr: ObjectReference, s: Sample, now: number) {
        const cur = PropService.read(refr, now);
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
        if (s.asleep || distance >= JUMP_UNITS) {
            refr.stopTranslation();
            if (prop.loaded3D) {
                // H-P4 (pass v10b 025124): a body placed by setPosition and later
                // set Dynamic flew on every seat it happened to, the drop's copy
                // that was never placed did not; the engine's keyframed translate
                // moves the collision with the node, so a jump translates at a
                // speed that lands within JUMP_MS
                const speed = Math.max(GLIDE_MIN_SPEED, distance / (JUMP_MS / 1000));
                refr.translateTo(x, y, z, ax, ay, az, speed, 0);
            } else {
                refr.setPosition(x, y, z)
                    .then(() => {
                        const again = this.refOf(prop);
                        return again ? again.setAngle(ax, ay, az) : undefined;
                    })
                    .catch((err) => logError(this, `placing prop ${prop.refr.toString(16)} at its sample failed`, err));
            }
        } else if (distance < 0.1) {
            refr.setAngle(ax, ay, az).catch((err) => logError(this, `turning prop ${prop.refr.toString(16)} failed`, err));
        } else {
            const intervalMs = prop.prevSampleAt > 0
                ? Math.min(GLIDE_MAX_MS, Math.max(GLIDE_MIN_MS, s.at - prop.prevSampleAt))
                : GLIDE_MAX_MS;
            const speed = Math.max(GLIDE_MIN_SPEED, distance / (intervalMs / 1000));
            refr.translateTo(x, y, z, ax, ay, az, speed, 0);
        }
        if (now - prop.lastAppliedLog >= APPLIED_LOG_MS) {
            prop.lastAppliedLog = now;
            logLab(TAG, `applied ${prop.refr.toString(16)} from the stream`);
        }
    }

    // The hand, read from the engine once per update (ADR-071's D-PH3 A: a
    // sample's held is the engine's own grab state, never an event this
    // service tracks, so a missed release cannot strand a prop). A tracked
    // prop the engine starts holding is claimed by grab then, and only then:
    // a grab the engine refuses in its own frame (a Keyframed copy, E28 and
    // E31) still raised grabRelease true and false together and claimed touch
    // 1, and the server granted a moving row to a seat that never held it
    // (havok-props.md E43, x-prop-engine v2.1 20261112-003027). A hold that
    // ends, by the next press of E or by the engine itself (E40, E41), is the
    // release claim (touch 2) when this seat simulates the prop, and the sleep
    // clock starts again from it, so the fall gets SLEEP_AFTER_MS to show. A
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
    // set): the release claim when this seat simulates the prop and no other
    // seat's word ended the hold first, and the sleep clock from here
    private letGo(prop: Prop, now: number, why: string) {
        prop.held = false;
        logLab(TAG, `prop ${prop.refr.toString(16)} let go: ${why}${prop.holdLost ? " (another seat's word had ended this hand's hold)" : ""}`);
        prop.stirredAt = now;
        if (prop.simulating && !prop.holdLost) {
            this.claim(prop, TOUCH_DROP, now);
        }
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
            // another seat's awake stream is live here (its samples' age, never
            // the contact's motion type: a resting copy reads 3, havok-props.md
            // E26)
            const streamed = prop.lastSampleAt !== 0 && !prop.lastSampleAsleep && now - prop.lastSampleAt < STREAM_LIVE_MS;
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

    private claim(prop: Prop, touch: number, now: number) {
        prop.lastClaimAt = now;
        this.controller.emitter.emit("sendMessage", {
            message: { t: MsgType.PropClaim, refr: prop.refr, touch },
            reliability: "reliable",
        });
        logLab(TAG, `claim ${prop.refr.toString(16)} touch ${touch}`);
    }

    // The tracked set, from the view's references: every bound view whose
    // record is no actor's and whose base is an item (read once per binding),
    // in the player's world or cell, within TRACK_RADIUS; the nearest
    // TRACK_MAX are tracked, the rest let go
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
                return;
            }
            const cell = refr.getParentCell();
            found.push({ refr: refrId, localId: refrId, formIdx: -1, distance, cell: cell ? cell.getFormID() : 0, baseId: rec.baseId });
        });
        found.sort((a, b) => a.distance - b.distance);
        const chosen = found.slice(0, TRACK_MAX);
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
                    prop.motionAsked = undefined;
                    prop.loaded3D = false;
                    prop.lastSent = undefined;
                    prop.lastRead = undefined;
                    prop.lastTouchRead = undefined;
                    prop.trackedAt = now;
                }
                prop.formIdx = c.formIdx;
                prop.distance = c.distance;
                if (c.cell) {
                    prop.cell = c.cell;
                }
            }
        }
        for (const prop of Array.from(this.props.values())) {
            if (!keep.has(prop.refr)) {
                this.untrack(prop, "out of reach");
            }
        }
        this.drops = this.drops.filter((d) => now - d.at <= DROP_MATCH_MS);
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
        if (prop.held) {
            // a held prop leaving the tracked set (a teleport, a door) lets go
            // first: a hold the server never heard end holds the prop against
            // every other seat until this seat's next grab, unload or
            // departure
            this.letGo(prop, Date.now(), `untracked (${why})`);
        }
        if (prop.simulating) {
            // a body this seat leaves Dynamic would move here unstreamed: its
            // transform goes out once more as the final sample (the server
            // returns the prop to the host on it, the record rests there, the
            // other seats follow), then the body is parked
            const refr = this.refOf(prop);
            if (refr && refr.is3DLoaded()) {
                if (prop.streaming) {
                    const now = Date.now();
                    this.send(prop, PropService.read(refr, now), true, now);
                    prop.streaming = false;
                }
                refr.setMotionType(MotionType.Keyframed, false).catch((err) =>
                    logError(this, `setMotionType(Keyframed) on untracked prop ${prop.refr.toString(16)} failed`, err));
            }
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
    private recordSample(refr: number, s: Sample) {
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

    // the proximity stand-in for the contact, off by default now that the
    // contact event exists; on for a seat whose hook never fires
    private contactFallback(): boolean {
        const s = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
        return s !== undefined && s["propContactFallback"] === true;
    }

    private static read(refr: ObjectReference, now: number): Sample {
        return {
            pos: ObjectReferenceEx.getPos(refr),
            rot: [refr.getAngleX(), refr.getAngleY(), refr.getAngleZ()],
            asleep: false,
            at: now,
        };
    }

    // whether two transforms differ past the thresholds (any axis of the turn)
    private static apart(a: Sample, b: Sample, units: number, degrees: number): boolean {
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
            simulating: false,
            effective: undefined,
            motionAsked: undefined,
            loaded3D: false,
            lastSent: undefined,
            lastSendAt: 0,
            lastRead: undefined,
            stirredAt: 0,
            streaming: false,
            announce: false,
            held: false,
            heldAt: 0,
            holdLost: false,
            pending: undefined,
            lastSampleAt: 0,
            prevSampleAt: 0,
            lastAppliedLog: 0,
            lastTouchRead: undefined,
            lastTouchReadAt: 0,
            lastClaimAt: 0,
            lastSampleAsleep: false,
            restAt: 0,
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
    // each local id's base verdict, by the record it is bound to (trap: D-S3's
    // verdict on a family reference's record, for a reference no item)
    private readonly kinds = new Map<number, { refr: number; item: boolean; trap?: boolean }>();
    private readonly echoSaid = new Set<number>();
    // the controller's contacts since the last update, by local id
    private contacts: Array<{ localId: number; layer: number; motionType: number }> = [];
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
