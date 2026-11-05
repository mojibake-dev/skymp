import { ObjectReference, SendPapyrusEventHook } from 'skyrimPlatform';

import { ObjectReferenceEx } from '../../extensions/objectReferenceEx';
import { logError, logLab, logTrace } from '../../logging';
import { MsgType } from '../../messages';
import { localIdToRemoteId, remoteIdToLocalId } from '../../view/worldViewMisc';
import { ConnectionMessage } from '../events/connectionMessage';
import { CreateActorMessage } from '../messages/createActorMessage';
import { DestructionStageMessage } from '../messages/destructionStageMessage';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { HostService } from './hostService';

const TAG = 'verb:traps';
// game-file references end where the server's copies begin; the validator
// refuses a refr at 0xff000000 and up (a copy carries no game-file result)
const LAST_GAME_FILE_ID = 0xfeffffff;
// the validator's rate (a burst of 8, then 2 a second), mirrored as a bucket
// so a burst of reports waits here a moment instead of being refused there
const REPORT_BURST = 8;
const REPORT_PER_S = 2;
// a record waits this long for its reference's 3D (the create word's
// onceLoad comes first; UNMEASURED), then is tried on the reference as it is
const APPLY_WAIT_MS = 10_000;
// the apply's damage through the engine's own DamageObject, in health points
// a call: DAMAGE_FIRST, doubled after a call that leaves the stage where it
// was and back to DAMAGE_FIRST after a rise, at most DAMAGE_MAX a call and
// DAMAGE_CALLS calls a record. A stage is a share of the base's destruction
// health (DSTD's health and each stage's healthPercentage, CommonLibSSE-NG
// include/RE/B/BGSDestructibleObjectForm.h), which no export reads.
// UNMEASURED: a step that crosses two stages' shares at once lands a stage
// past the record, and a stage that caps damage stops the climb (said)
const DAMAGE_FIRST = 1;
const DAMAGE_MAX = 32;
const DAMAGE_CALLS = 64;
// a reference stays this seat's own apply this long after its last call, so
// a stage change of a live relay's apply (which no blank covers) is not
// reported back (UNMEASURED); a served record's blank is lifted one update
// after the last call (the platform's blank runs before the event hook), not
// in the same turn: DamageObject is latent, and whether the engine raises the
// stage's events inside the calls or after their promises resolve is
// unread (HYPOTHESIS H-DS1; the platform-and-client review, finding 1;
// x-trap-lamp's restart leg reads the platform's "blanked: raised while the
// server's record is applied to it" line). The engine's own stage effects
// (a destructible's stage explosion placed by its stage data, script or
// not) are no Papyrus event, so no blank covers them: a declared limit
const SETTLE_MS = 500;
// a LIVE record's destroyed flag (a relay, not a served record) waits this
// long before this seat sets it, so this seat's own copy, already firing for
// the figure it saw, fires its children first (D-S3 point 1: every seat fires
// the family itself; Eli's D-T2 outcome, the trap fires in both games). In
// Embershard's three rockfall runs (20261105-153200, -154408, -210948) the
// record of tripwire 0xb6d3f reached the non-tripping seat 70 to 140 ms before
// its own Tripwire.onBeginState reached its Activate(self), and that seat's
// wire forwarded nothing to its linker 0xb6d3e: no "fired here" line there in
// any run against 9 or 10 on the tripper, the rigged platform 0xb6d47 never
// broken and its dust never raised. HYPOTHESIS: a destroyed reference's
// activation forwards nothing to its activate children (the tripper's own
// forward came 450 ms after its script's SetDestroyed, which follows the
// Activate call, so the flag is read before that). Its own script's
// SetDestroyed (Tripwire's DoNothing state) ends a firing here, so the wait
// lets the copy fire at most once and the record deafens it after the wait
// when it never fired (UNMEASURED: the runs needed about 150 ms). A served
// record (a late joiner, a restart) is applied at once, as before
const FIRE_GRACE_MS = 3000;
const KNOWN_MAX = 4096;

type PendingRecord = {
  remote: number;
  stage: number;
  destroyed: boolean;
  served: boolean;
  since: number;
};

// thuum docs/DECISIONS.md ADR-068 D-S3 point 3 (traps follow the interaction:
// a single-use result recorded once; docs/research/trap-slice-d-s3-2026-11-05.md).
// The reporter: a trap family reference's SetDestroyed(true) from the
// scriptCalls batch (ScriptCallService hands it over; the platform batches
// that state row for a family reference, which every seat runs), and a
// destruction stage's rise heard through Skyrim Platform's Papyrus event hook,
// filtered in native to game-file references and OnDestructionStageChanged
// alone (rule 9) and read on the next update (GetCurrentDestructionStage):
// reported as DestructionStage when the reference is in the family (every
// seat's engine runs it; the server keeps the first report of each step) or
// stands in a cell this seat hosts (its scripts run here alone), and only past
// what this seat knows (its own reports and the server's records). The
// receiver: a record ahead of this engine's copy applied with the engine's own
// calls, DamageObject until the record's stage then SetDestroyed, which are
// the platform's own calls (the seam tallies them "(platform)" and batches
// none, so nothing applied here is reported back); a served record (a late
// subscriber, a restart) under the platform's apply blank (setRecordApplying:
// the gate blanks every Papyrus event to the reference meanwhile), so a late
// joiner's oil lamp never replays its fall, a live relay without it, so the
// reference's own scripts answer the step as the reporter's did, its destroyed
// flag after FIRE_GRACE_MS so this seat's own copy fires first. Every form is
// taken again by id after a wait (a wrapper lives one Papyrus update).
//
// Lines for the lab: "trap result <refr>: stage <n>, destroyed <b> reported",
// "trap result <refr> applied from the record: stage <n>, destroyed <b> (here
// stage <m>)", "trap result <refr> from the record: here already", "trap result
// <refr>: the record's destroyed waits <ms> ms for this seat's own copy",
// "trap result <refr>: destroyed here before the record's wait ended", "trap
// result <refr>: the record's destroyed applied after <ms> ms, this seat's own
// copy unfired".
export class TrapResultService extends ClientListener {
  constructor(
    private sp: Sp,
    private controller: CombinedController,
  ) {
    super();
    const self = this;
    // Skyrim Platform's Handler takes enter AND leave (Handler.cpp wraps both;
    // wordWallService's note): a variable, not a literal, under the published
    // typing that declares enter alone
    const handler = {
      enter(ctx: SendPapyrusEventHook.Context) {
        self.heard.add(ctx.selfId);
      },
      leave() {},
    };
    // the hook's patterns are case-exact (Handler.cpp) while the engine's
    // events are case-blind, and a vanilla script may intern another casing
    // (docs/engine/scripts.md S1, the whale's cause): both spellings
    // registered, as the pillars' patterns are (the review's finding 3)
    this.sp.hooks.sendPapyrusEvent.add(
      handler,
      1,
      LAST_GAME_FILE_ID,
      'OnDestructionStageChanged',
    );
    this.sp.hooks.sendPapyrusEvent.add(
      handler,
      1,
      LAST_GAME_FILE_ID,
      'onDestructionStageChanged',
    );
    this.controller.emitter.on('destructionStageMessage', (e) =>
      this.onRecord(e),
    );
    this.controller.emitter.on('createActorMessage', (e) =>
      this.onCreateActor(e),
    );
    this.controller.on('update', () => this.onUpdate());
  }

  // ScriptCallService's: the batch's SetDestroyed on a family reference. JS
  // state alone (the batch's context allows no game call); read on the update
  noteSetDestroyed(localId: number, destroyed: boolean) {
    if (!destroyed || localId === 0) {
      return;
    }
    this.toReport.set(localId, true);
  }

  // a record from the server, relayed or served: JS state alone here (a
  // message handler may call no native), applied on the update. What this
  // seat knows rises to it at once, so a report of the same step is not sent
  private onRecord(e: ConnectionMessage<DestructionStageMessage>) {
    const m = e.message;
    const localId = remoteIdToLocalId(m.refr);
    if (!localId) {
      return;
    }
    this.know(m.refr, m.stage, m.destroyed);
    const queued = this.records.get(localId);
    this.records.set(localId, {
      remote: m.refr,
      stage: Math.max(m.stage, queued ? queued.stage : 0),
      destroyed: m.destroyed || (queued ? queued.destroyed : false),
      served: m.served || (queued ? queued.served : false),
      since: queued ? queued.since : Date.now(),
    });
  }

  // the player's own actor is created: a fresh table on the server's side (a
  // login, a reconnect, a restart), which serves its records again
  private onCreateActor(e: ConnectionMessage<CreateActorMessage>) {
    if (e.message.isMe) {
      this.known.clear();
    }
  }

  private onUpdate() {
    const now = Date.now();
    if (this.settling.size > 0) {
      this.settle(now);
    }
    if (this.heard.size > 0) {
      this.heard.forEach((localId) => {
        if (!this.toReport.has(localId)) {
          this.toReport.set(localId, false);
        }
      });
      this.heard.clear();
    }
    if (this.toReport.size > 0) {
      this.report(now);
    }
    if (this.records.size > 0) {
      this.applyRecords(now);
    }
    if (this.destroyLater.size > 0) {
      this.destroyDue(now);
    }
  }

  private report(now: number) {
    this.refill(now);
    const host = this.controller.lookupListener(HostService);
    for (const [localId, setDestroyed] of Array.from(
      this.toReport.entries(),
    )) {
      // this seat's own apply of a record: the record's step, known already
      if (this.applying.has(localId) || this.settling.has(localId)) {
        this.toReport.delete(localId);
        continue;
      }
      const refr = ObjectReference.from(this.sp.Game.getFormEx(localId));
      const remote = localIdToRemoteId(localId);
      if (!refr || !remote || remote > LAST_GAME_FILE_ID) {
        this.toReport.delete(localId);
        continue;
      }
      const role = ObjectReferenceEx.trapRole(localId);
      if (!(role !== null && role.family) && !host.isHostedHere(refr)) {
        // another seat hosts it and its scripts run there: the host reports
        this.toReport.delete(localId);
        continue;
      }
      // a reference with no destruction data reads as stage 0, the game
      // files' state
      const stage = Math.max(0, refr.getCurrentDestructionStage());
      const destroyed = setDestroyed || (role !== null && role.destroyed);
      const known = this.known.get(remote);
      const rises =
        stage > (known ? known.stage : 0) ||
        (destroyed && !(known ? known.destroyed : false));
      if (!rises) {
        this.toReport.delete(localId);
        continue;
      }
      if (stage > 127) {
        logError(
          this,
          `trap result ${remote.toString(16)}: stage ${stage} is past the wire's 127, not reported`,
        );
        this.toReport.delete(localId);
        continue;
      }
      if (this.tokens < 1) {
        // the rest wait in toReport for the bucket
        return;
      }
      this.tokens -= 1;
      this.toReport.delete(localId);
      this.know(remote, stage, destroyed);
      const message: DestructionStageMessage = {
        t: MsgType.DestructionStage,
        refr: remote,
        stage,
        destroyed,
        served: false,
      };
      this.controller.emitter.emit('sendMessage', {
        message,
        reliability: 'reliable',
      });
      logLab(
        TAG,
        `trap result ${remote.toString(16)}: stage ${stage}, destroyed ${destroyed} reported`,
      );
    }
  }

  private applyRecords(now: number) {
    for (const [localId, rec] of Array.from(this.records.entries())) {
      // one apply at a time per reference; a newer record waits for it
      // (records only rise, and the queued one merged them)
      if (this.applying.has(localId)) {
        continue;
      }
      const refr = ObjectReference.from(this.sp.Game.getFormEx(localId));
      const waited = now - rec.since;
      if (!refr) {
        if (waited >= APPLY_WAIT_MS) {
          this.records.delete(localId);
          logError(
            this,
            `trap result ${rec.remote.toString(16)}: no reference here for the record (stage ${rec.stage}, destroyed ${rec.destroyed}) in ${APPLY_WAIT_MS} ms`,
          );
        }
        continue;
      }
      if (!refr.is3DLoaded() && waited < APPLY_WAIT_MS) {
        continue;
      }
      this.records.delete(localId);
      this.apply(localId, rec).catch((err) =>
        logError(
          this,
          `trap result ${rec.remote.toString(16)}: the record's apply failed`,
          err,
        ),
      );
    }
  }

  private async apply(localId: number, rec: PendingRecord): Promise<void> {
    const id = rec.remote.toString(16);
    const first = ObjectReference.from(this.sp.Game.getFormEx(localId));
    if (!first) {
      return;
    }
    const here = Math.max(0, first.getCurrentDestructionStage());
    const role = ObjectReferenceEx.trapRole(localId);
    const hereDestroyed = role !== null && role.destroyed;
    if (here >= rec.stage && (!rec.destroyed || hereDestroyed)) {
      logLab(TAG, `trap result ${id} from the record: here already`);
      return;
    }
    this.applying.add(localId);
    if (rec.served) {
      this.setApplying(localId, true);
    }
    let stage = here;
    let calls = 0;
    try {
      let step = DAMAGE_FIRST;
      while (stage < rec.stage && calls < DAMAGE_CALLS) {
        const ref = ObjectReference.from(this.sp.Game.getFormEx(localId));
        if (!ref) {
          break;
        }
        await ref.damageObject(step);
        calls++;
        const again = ObjectReference.from(this.sp.Game.getFormEx(localId));
        if (!again) {
          break;
        }
        const now = again.getCurrentDestructionStage();
        step = now > stage ? DAMAGE_FIRST : Math.min(step * 2, DAMAGE_MAX);
        stage = Math.max(stage, now);
      }
      if (rec.destroyed && !hereDestroyed) {
        if (rec.served) {
          const ref = ObjectReference.from(this.sp.Game.getFormEx(localId));
          if (ref) {
            ref.setDestroyed(true);
          }
        } else {
          this.destroyLater.set(localId, {
            remote: rec.remote,
            due: Date.now() + FIRE_GRACE_MS,
          });
          logLab(
            TAG,
            `trap result ${id}: the record's destroyed waits ${FIRE_GRACE_MS} ms for this seat's own copy`,
          );
        }
      }
      logLab(
        TAG,
        `trap result ${id} applied from the record: stage ${rec.stage}, destroyed ${rec.destroyed} (here stage ${here})`,
      );
      if (stage < rec.stage) {
        logError(
          this,
          `trap result ${id}: the record's stage ${rec.stage} not reached, stage ${stage} after ${calls} DamageObject call(s)`,
        );
      }
    } finally {
      // the last DamageObject has resolved and SetDestroyed returned; the
      // blank stays up one more update (H-DS1)
      if (rec.served) {
        this.controller.once('update', () => this.setApplying(localId, false));
      }
      this.applying.delete(localId);
      this.settling.set(localId, Date.now() + SETTLE_MS);
    }
  }

  // a live record's destroyed flag at the end of its wait: set here unless
  // this seat's own copy fired meanwhile (its script's SetDestroyed)
  private destroyDue(now: number) {
    for (const [localId, wait] of Array.from(this.destroyLater.entries())) {
      if (now < wait.due) {
        continue;
      }
      this.destroyLater.delete(localId);
      const id = wait.remote.toString(16);
      const role = ObjectReferenceEx.trapRole(localId);
      if (role !== null && role.destroyed) {
        logLab(TAG, `trap result ${id}: destroyed here before the record's wait ended`);
        continue;
      }
      const ref = ObjectReference.from(this.sp.Game.getFormEx(localId));
      if (!ref) {
        continue;
      }
      ref.setDestroyed(true);
      logLab(
        TAG,
        `trap result ${id}: the record's destroyed applied after ${FIRE_GRACE_MS} ms, this seat's own copy unfired`,
      );
    }
  }

  // the applies' tails: a reference's own stage changes count as reports
  // again
  private settle(now: number) {
    for (const [localId, until] of Array.from(this.settling.entries())) {
      if (now >= until) {
        this.settling.delete(localId);
      }
    }
  }

  private setApplying(localId: number, applying: boolean) {
    try {
      if (!this.sp.setRecordApplying(localId, applying)) {
        logError(
          this,
          `setRecordApplying(${localId.toString(16)}, ${applying}) refused`,
        );
      }
    } catch (e) {
      if (!this.applyingMissingSaid) {
        this.applyingMissingSaid = true;
        logError(this, `setRecordApplying unavailable on this platform: ${e}`);
      }
    }
  }

  private know(remote: number, stage: number, destroyed: boolean) {
    if (this.known.size >= KNOWN_MAX && !this.known.has(remote)) {
      logTrace(this, `${this.known.size} results known; the table starts over`);
      this.known.clear();
    }
    const known = this.known.get(remote);
    this.known.set(remote, {
      stage: Math.max(stage, known ? known.stage : 0),
      destroyed: destroyed || (known ? known.destroyed : false),
    });
  }

  private refill(now: number) {
    this.tokens = Math.min(
      REPORT_BURST,
      this.tokens + ((now - this.refilledAt) / 1000) * REPORT_PER_S,
    );
    this.refilledAt = now;
  }

  // the references the hook heard, by local id, read on the next update
  private readonly heard = new Set<number>();
  // to report on the update: true for the batch's SetDestroyed(true), false
  // for a stage change heard
  private readonly toReport = new Map<number, boolean>();
  // the records to apply, by local id (the newest step merged in)
  private readonly records = new Map<number, PendingRecord>();
  // the highest step this seat knows for each reference, by the server's id:
  // its own reports and the server's records
  private readonly known = new Map<
    number,
    { stage: number; destroyed: boolean }
  >();
  private readonly applying = new Set<number>();
  // the applies' tails, until when (Date.now())
  private readonly settling = new Map<number, number>();
  // live records' destroyed flags waiting for this seat's own copy, by local
  // id, until when (Date.now())
  private readonly destroyLater = new Map<
    number,
    { remote: number; due: number }
  >();
  private tokens = REPORT_BURST;
  private refilledAt = Date.now();
  private applyingMissingSaid = false;
}
