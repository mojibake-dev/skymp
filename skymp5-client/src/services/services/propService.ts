import { ContainerChangedEvent, GrabReleaseEvent, MotionType, ObjectReference, PropContactEvent } from "skyrimPlatform";
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
import { getViewFromStorage, localIdToRemoteId } from "../../view/worldViewMisc";
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

// How often the tracked set is rebuilt from the view's references, ms
const RESCAN_MS = 500;
// A prop is tracked within this many units of the player, the nearest
// TRACK_MAX of them
const TRACK_RADIUS = 1024;
const TRACK_MAX = 64;
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
// The lease's mirror: after an asleep sample a named owner lapses to the
// cell's host after this long (wire-rules props.rs PROP_LEASE_MS, UNMEASURED
// there too)
const PROP_LEASE_MS = 3000;
// A drop's claim: the server's reference for an item this player dropped is
// matched by its base within this window and this distance of the player
const DROP_MATCH_MS = 3000;
const DROP_MATCH_UNITS = 512;
// "applied from the stream" is said once per prop per this
const APPLIED_LOG_MS = 10_000;

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
    // the follower's side
    pending: Sample | undefined;   // the newest sample in, applied on the next update
    lastSampleAt: number;
    prevSampleAt: number;
    lastAppliedLog: number;
    lastTouchRead: Sample | undefined;
    lastTouchReadAt: number;
    lastClaimAt: number;
};

type Owner = { player: number; epoch: number; leaseUntil: number };

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
// falls nor fights the stream. Who simulates is the server's word (PropOwner,
// R0): the player the last word named, while the body is awake or its lease
// holds after a sleep; with none, the cell's host (HostService). The touch is
// what passes the simulation (PropClaim): the player's character controller
// meeting the body (the fork's propContact event: FridaHooks.cpp's hook on
// the proxy controller's contact callback, Address Library 79131, one event
// per reference per 250 ms), the engine's grab and its release, a drop; the
// proximity stand-in below stays behind propContactFallback. Slice one: items
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
// moves locally, which a Keyframed body never does.
export class PropService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.emitter.on("propTransformMessage", (e) => this.onTransform(e));
        this.controller.emitter.on("propOwnerMessage", (e) => this.onOwner(e));
        this.controller.emitter.on("createActorMessage", (e) => this.onCreateActor(e));
        this.controller.on("grabRelease", (e) => this.onGrabRelease(e));
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
        const owner = this.owners.get(m.refr);
        if (owner) {
            // the lease's mirror: a sleep starts it, a wake ends it
            owner.leaseUntil = m.asleep ? now + PROP_LEASE_MS : 0;
        }
        const prop = this.props.get(m.refr);
        if (!prop) {
            return;
        }
        if (prop.simulating) {
            // the server relays to the other listeners alone (ActionListener
            // OnPropTransform skips the sender); said once per prop
            if (!this.echoSaid.has(m.refr)) {
                this.echoSaid.add(m.refr);
                logError(this, `a sample for prop ${m.refr.toString(16)} this seat simulates arrived from the server; ignored`);
            }
            return;
        }
        prop.pending = sample;
        prop.prevSampleAt = prop.lastSampleAt;
        prop.lastSampleAt = now;
    }

    // The server's word on who simulates a prop from now; an epoch not above
    // the known one is a late word (one counter per prop, the server's). The
    // body's motion type follows on the update
    private onOwner(e: ConnectionMessage<PropOwnerMessage>) {
        const m = e.message;
        const known = this.owners.get(m.refr);
        if (known && m.epoch <= known.epoch) {
            logTrace(this, `owner word for prop ${m.refr.toString(16)} at epoch ${m.epoch} ignored: epoch ${known.epoch} known`);
            return;
        }
        this.owners.set(m.refr, { player: m.player, epoch: m.epoch, leaseUntil: 0 });
        const mine = m.player !== 0 && m.player === this.myId();
        logLab(TAG, `prop ${m.refr.toString(16)} owner ${m.player.toString(16)} epoch ${m.epoch} (${mine ? "mine" : "theirs"})`);
    }

    // The player's own actor is created: a fresh table on the server's side
    // of this connection (a login, a reconnect, a restart), so the words and
    // their epochs clear (HostService's rule for its grants)
    private onCreateActor(e: ConnectionMessage<CreateActorMessage>) {
        if (!e.message.isMe) {
            return;
        }
        this.owners.clear();
        this.echoSaid.clear();
        this.props.forEach((prop) => {
            prop.effective = undefined;
        });
    }

    // The engine's grab (isGrabbed true) and its release of a reference. The
    // id alone is read here (an event's wrapper is not kept across ticks);
    // the claim goes out from the update, once the reference is known as a
    // tracked prop. The release is PropClaim touch 2 (wire-rules props.rs:
    // "a grabbed prop let go"): the server holds a grabbed prop against every
    // other claim until its holder's drop, so a release that said nothing
    // would hold the prop for the session; the holder keeps the simulation
    // until its lease lapses either way
    private onGrabRelease(e: GrabReleaseEvent) {
        if (!e.refr) {
            return;
        }
        this.touches.push({ localId: e.refr.getFormID(), touch: e.isGrabbed ? TOUCH_GRAB : TOUCH_DROP });
        if (this.touches.length > 32) {
            this.touches.shift();
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
    // dropper's seat from its first frame (ADR-058 decision 4)
    private onContainerChanged(e: ContainerChangedEvent) {
        if (!this.enabled()) {
            return;
        }
        const player = this.sp.Game.getPlayer();
        if (!player || !e.oldContainer || !e.baseObj || !e.reference) {
            return;
        }
        if (e.newContainer !== null && e.newContainer !== undefined) {
            return;
        }
        if (e.oldContainer.getFormID() !== player.getFormID()) {
            return;
        }
        this.drops.push({ baseId: e.baseObj.getFormID(), at: Date.now() });
        if (this.drops.length > 16) {
            this.drops.shift();
        }
    }

    private onUpdate() {
        if (!this.enabled()) {
            if (this.props.size > 0) {
                this.untrackAll();
            }
            return;
        }
        const now = Date.now();
        if (now - this.lastRescan >= RESCAN_MS) {
            this.lastRescan = now;
            this.rescan(now);
        }
        if (this.touches.length > 0) {
            this.claimTouches(now);
        }
        if (this.contacts.length > 0) {
            this.claimContacts(now);
        }
        if (this.props.size === 0) {
            return;
        }
        const player = this.sp.Game.getPlayer();
        if (!player) {
            return;
        }
        const playerPos = ObjectReferenceEx.getPos(player);
        const myId = this.myId();
        const fallback = this.contactFallback();
        for (const prop of Array.from(this.props.values())) {
            this.tick(prop, now, myId, playerPos, fallback);
        }
    }

    // One prop, one update: the simulator computed from the owner table, the
    // body's motion type set to match (once, and again when its 3D reloads),
    // then the stream out or the stream's apply and, under the fallback, the
    // proximity read
    private tick(prop: Prop, now: number, myId: number, playerPos: NiPoint3, fallback: boolean) {
        if (now - prop.trackedAt < TRACK_GRACE_MS) {
            return;
        }
        const owner = this.ownerOf(prop, now, myId);
        const word = this.owners.get(prop.refr);
        const named = word !== undefined && word.player === owner;
        if (owner !== prop.effective) {
            if (!named && prop.effective !== undefined) {
                // the fallback to the cell's host after a word: the lease
                // lapsed or the owner left (a prop first tracked under the
                // host's default says nothing; the word's own line is onOwner's)
                logLab(TAG, `prop ${prop.refr.toString(16)} owner ${owner.toString(16)} epoch ${word ? word.epoch : 0} (host)`);
            }
            prop.effective = owner;
        }
        const simulate = myId !== 0 && owner === myId;
        const near = prop.distance <= TOUCH_RADIUS + 512;
        const touchReadDue = fallback && !simulate && near && now - prop.lastTouchReadAt >= TOUCH_READ_MS;
        const wantsRef = simulate || !prop.placed || prop.simulating !== simulate || prop.motionAsked === undefined
            || prop.pending !== undefined || touchReadDue;
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
        if (prop.simulating !== simulate) {
            // a word that names this seat is a touch granted: the body wakes
            // (a drop's copy may stand in the air, a passed prop mid-motion);
            // the host's default clutter rests until a contact wakes it
            this.flip(prop, refr, simulate, simulate && named, now);
        } else if (loaded3D && prop.motionAsked === undefined) {
            this.setMotion(prop, refr, simulate ? MotionType.Dynamic : MotionType.Keyframed, false);
        }
        if (prop.simulating) {
            this.stream(prop, refr, now, myId);
        } else {
            this.follow(prop, refr, now, playerPos, touchReadDue);
        }
    }

    // Who simulates the prop as this seat reads the table: the player the
    // last word named while that word is live (awake, or asleep inside the
    // lease) and the player is still in this seat's world; else the cell's
    // host, this seat when it hosts the prop's cell and 0 (another seat's,
    // or nobody's: an unhosted cell's prop freezes) otherwise
    private ownerOf(prop: Prop, now: number, myId: number): number {
        const word = this.owners.get(prop.refr);
        if (word && word.player !== 0 && (word.leaseUntil === 0 || now < word.leaseUntil) && this.present.has(word.player)) {
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
        if (!m) {
            prop.placed = true;
            prop.placedAt = now;
            return;
        }
        const read = PropService.read(refr, now);
        const target: Sample = { pos: m.pos, rot: m.rot, asleep: false, at: now };
        if (!PropService.apart(read, target, SAMPLE_UNITS, SAMPLE_DEGREES)) {
            prop.placed = true;
            prop.placedAt = now;
            return;
        }
        prop.placing = true;
        const [x, y, z] = m.pos;
        const [ax, ay, az] = m.rot;
        const done = (why: string) => {
            prop.placing = false;
            prop.placed = true;
            prop.placedAt = Date.now();
            logTrace(this, `prop ${prop.refr.toString(16)} placed at its record${why}`);
        };
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
                done(" (the placement failed)");
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
            refr.stopTranslation();
            prop.pending = undefined;
        }
        this.setMotion(prop, refr, simulate ? MotionType.Dynamic : MotionType.Keyframed, simulate && wake);
        prop.simulating = simulate;
        prop.lastSent = undefined;
        prop.lastRead = undefined;
        prop.lastSendAt = 0;
        prop.streaming = false;
        prop.stirredAt = now;
        prop.lastTouchRead = undefined;
        logTrace(this, `prop ${prop.refr.toString(16)} ${simulate ? "simulated here: Dynamic" : "followed: Keyframed"}`);
    }

    // abAllowActivate is Havok's wake: Dynamic without it, the body rests as
    // the file's clutter does until a contact wakes it, so the host's props do
    // not settle and stream all at once at each cell; with it (a granted
    // touch) the body is live at once. Keyframed as dealWithRef sets it.
    // HYPOTHESIS: a clutter body takes both switches at runtime
    private setMotion(prop: Prop, refr: ObjectReference, type: MotionType, wake: boolean) {
        prop.motionAsked = type;
        refr.setMotionType(type, wake).catch((err) => {
            prop.motionAsked = undefined;
            logError(this, `setMotionType(${type}) on prop ${prop.refr.toString(16)} failed`, err);
        });
    }

    // The simulator's read and stream: a sample when the body moved past the
    // thresholds since the last sent, at most every SEND_MIN_MS, unreliable;
    // the sleep sample, reliable, when it has not stirred for SLEEP_AFTER_MS
    // after moving; nothing more until it moves again
    private stream(prop: Prop, refr: ObjectReference, now: number, myId: number) {
        if (!prop.streaming && prop.lastRead && now - prop.lastRead.at < REST_READ_MS) {
            return;
        }
        const read = PropService.read(refr, now);
        if (!prop.lastSent) {
            // the baseline: nothing goes out until the body moves from here
            prop.lastSent = read;
            prop.lastRead = read;
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
        if (prop.streaming && now - prop.stirredAt >= SLEEP_AFTER_MS) {
            this.send(prop, read, true, now);
            prop.streaming = false;
            logLab(TAG, `stream ${prop.refr.toString(16)} asleep`);
            // the lease's mirror on the owner's own seat: the server starts
            // the lease at this sample, and after it the cell's host simulates
            const word = this.owners.get(prop.refr);
            if (word && word.player === myId) {
                word.leaseUntil = now + PROP_LEASE_MS;
            }
        }
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
            this.apply(prop, refr, pending, now);
            // the stream moved it: the touch read starts over after the glide
            prop.lastTouchRead = undefined;
            prop.lastTouchReadAt = now;
            return;
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
            refr.setPosition(x, y, z)
                .then(() => {
                    const again = this.refOf(prop);
                    return again ? again.setAngle(ax, ay, az) : undefined;
                })
                .catch((err) => logError(this, `placing prop ${prop.refr.toString(16)} at its sample failed`, err));
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

    // The grabs and releases queued by the event, claimed for the tracked
    // props among them (a grab's claim goes even for a prop this seat
    // simulates: the server marks it held against the other seats' claims)
    private claimTouches(now: number) {
        const touches = this.touches;
        this.touches = [];
        for (const t of touches) {
            const refr = localIdToRemoteId(t.localId);
            const prop = refr ? this.props.get(refr) : undefined;
            if (!prop) {
                logTrace(this, `touch ${t.touch} on ${t.localId.toString(16)}: not a tracked prop, no claim`);
                continue;
            }
            this.claim(prop, t.touch, now);
        }
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
            const owned = prop.simulating || (myId !== 0 && prop.effective === myId);
            if (owned) {
                logLab(TAG, `${head}: owned, no claim`);
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
    // TRACK_MAX are tracked, the rest let go. The players and NPCs present
    // (the owner rule's "still here") are the records' ids
    private rescan(now: number) {
        const view = getViewFromStorage();
        const player = this.sp.Game.getPlayer();
        if (!view || !player) {
            return;
        }
        const world = this.controller.lookupListener(RemoteServer).getWorldModel();
        const myId = this.myId();
        this.present.clear();
        for (const f of world.forms) {
            if (f && f.refrId !== undefined) {
                this.present.add(f.refrId);
            }
        }
        if (myId) {
            this.present.add(myId);
        }
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
        found.sort((a, b) => a.distance - b.distance);
        const keep = new Set<number>();
        for (const c of found.slice(0, TRACK_MAX)) {
            keep.add(c.refr);
            let prop = this.props.get(c.refr);
            if (!prop) {
                prop = PropService.newProp(c.refr, c.localId, c.formIdx, c.cell, c.distance, now);
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
        const base = ref ? ref.getBaseObject() : null;
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
        if (prop.simulating) {
            // a body this seat leaves Dynamic would move here unstreamed: its
            // transform goes out once more as the final sample (the server's
            // lease starts, the record rests there, the other seats follow),
            // then the body is parked
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
            pending: undefined,
            lastSampleAt: 0,
            prevSampleAt: 0,
            lastAppliedLog: 0,
            lastTouchRead: undefined,
            lastTouchReadAt: 0,
            lastClaimAt: 0,
        };
    }

    // the tracked props by the server's id, and the same by this engine's id
    private readonly props = new Map<number, Prop>();
    private readonly byLocal = new Map<number, number>();
    // the server's words, by prop, tracked or not
    private readonly owners = new Map<number, Owner>();
    // the records' ids present in this seat's world at the last rescan
    private readonly present = new Set<number>();
    // each local id's base verdict, by the record it is bound to
    private readonly kinds = new Map<number, { refr: number; item: boolean }>();
    private readonly echoSaid = new Set<number>();
    // the grabs and releases since the last update, by local id
    private touches: Array<{ localId: number; touch: number }> = [];
    // the controller's contacts since the last update, by local id
    private contacts: Array<{ localId: number; layer: number; motionType: number }> = [];
    // the player's drops awaiting the server's reference
    private drops: Array<{ baseId: number; at: number }> = [];
    private lastRescan = 0;
}
