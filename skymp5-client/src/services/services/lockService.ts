import {
  Game,
  LockChangedEvent,
  MenuCloseEvent,
  MenuOpenEvent,
  ObjectReference,
} from 'skyrimPlatform';
import { MsgType } from '../../messages';
import { logTrace } from '../../logging';
import { ConnectionMessage } from '../events/connectionMessage';
import { LockPromptMessage } from '../messages/lockPromptMessage';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { ModelApplyUtils } from '../../view/modelApplyUtils';
import { localIdToRemoteId } from '../../view/worldViewMisc';
import { HostService } from './hostService';
import { WorldGlobalService } from './worldGlobalService';

// thuum docs/verbs/locks.md (M4): the lock is the server's. Every door and
// container is activation-blocked in this game (objectReferenceEx's
// dealWithRef), so the player's E goes to the server as an Activate; when the
// server finds the reference locked and the player without its key it sends
// LockPrompt, and this service runs the engine's own activation on the
// locked local reference (the lock stands here since dealWithRef applies the
// server's lock prop): the engine opens its lockpicking menu, or its
// "requires key" box, as in the single game. The attempt's end is read from
// the engine: lockChanged on the reference (the lock turned), the
// Lockpicking Menu's close with the lock standing (picks broken, counted
// from the Lockpicks held before and after, or the menu left), and goes to
// the server as LockReport; the server writes the unlock (the lock property
// to every game) or takes the broken picks from the record. A prompt whose
// menu never opens within promptMs (the key opened it, or the box) is
// forgotten.
interface Attempt {
  remoteId: number;
  localId: number;
  picksBefore: number;
  promptedAt: number;
  menuOpened: boolean;
  reported: boolean;
}

export class LockService extends ClientListener {
  constructor(
    private sp: Sp,
    private controller: CombinedController,
  ) {
    super();
    this.controller.emitter.on('lockPromptMessage', (e) => this.onPrompt(e));
    this.controller.on('menuOpen', (e) => this.onMenuOpen(e));
    this.controller.on('menuClose', (e) => this.onMenuClose(e));
    this.controller.on('lockChanged', (e) => this.onLockChanged(e));
    // thuum ADR-073 step 3: a bound reference with no recorded lock reports its
    // engine's standing lock, and again when this game starts hosting its cell
    // (a report from a seat that hosted neither side was not kept)
    ModelApplyUtils.onUnrecordedLock = (refr) => this.reportStanding(refr);
    this.controller.emitter.on('cellHostChanged', (e) => {
      if (e.hosted) {
        this.controller.once('update', () => this.reportStandingIn(e.cell));
      }
    });
    this.controller.on('update', () => this.onUpdate());
  }

  private onPrompt(e: ConnectionMessage<LockPromptMessage>) {
    const remoteId = e.message.refr;
    // a message handler has no Papyrus VM context: Skyrim Platform refuses
    // natives here ("'Game.getFormEx' can't be called in this context",
    // x-lockpick 20261031-144606), so the lookup, the lock read and the
    // engine's activation wait for the next update, as remoteServer's
    // teleport does
    this.controller.once('update', () => this.prompt(remoteId));
  }

  private prompt(remoteId: number) {
    const refr = ObjectReference.from(this.sp.Game.getFormEx(remoteId));
    if (!refr) {
      logTrace(
        this,
        `lock prompt ${remoteId.toString(16)}: no such reference in this game`,
      );
      return;
    }
    this.attempt = {
      remoteId,
      localId: refr.getFormID(),
      picksBefore: this.picksHeld(),
      promptedAt: Date.now(),
      menuOpened: false,
      reported: false,
    };
    // thuum #47: the server prompts on a lock no game has written (the game
    // file's seed) as on any lock it holds locked. Where this engine has it
    // unlocked (a shop's door by day, which the owner's schedule opened
    // here), the engine's own activation would walk the player through a
    // door the server never opened, and the server would put the player back.
    // So this engine's lock goes to the server as the prompted player's word,
    // which the server takes on a lock no game wrote, and the activation goes
    // again, once per lock in reactivateMs (a refused report prompts again,
    // and that prompt runs the engine's own activation)
    const now = Date.now();
    const last = this.reactivated.get(remoteId) ?? 0;
    if (!refr.isLocked() && now - last > LockService.reactivateMs) {
      this.attempt = undefined;
      this.reactivated.set(remoteId, now);
      if (this.reactivated.size > LockService.unrecordedMax) {
        const oldest = this.reactivated.keys().next().value;
        if (oldest !== undefined) {
          this.reactivated.delete(oldest);
        }
      }
      this.reportEngineWrite(refr, true, 'prompted');
      const caster = localIdToRemoteId(0x14);
      if (caster) {
        this.controller.emitter.emit('sendMessage', {
          message: { t: MsgType.Activate, data: { caster, target: remoteId, isSecondActivation: false } },
          reliability: 'reliable',
        });
      }
      logTrace(
        this,
        `lock prompt ${remoteId.toString(16)}: unlocked in this engine (level ${refr.getLockLevel()}); its lock reported and the activation sent again (#47)`,
      );
      return;
    }
    logTrace(
      this,
      `lock prompt ${remoteId.toString(16)}: the engine's activation run (locked ${refr.isLocked()}, level ${refr.getLockLevel()}, ${this.attempt.picksBefore} picks held)`,
    );
    refr.activate(this.sp.Game.getPlayer(), true);
  }

  // thuum #47: the locks whose activation went again after a prompt, when
  private reactivated = new Map<number, number>();
  private static readonly reactivateMs = 10000;

  private onMenuOpen(e: MenuOpenEvent) {
    if (e.name !== LockService.menu || !this.attempt) {
      return;
    }
    this.attempt.menuOpened = true;
    logTrace(
      this,
      `lock ${this.attempt.remoteId.toString(16)}: the Lockpicking Menu is open`,
    );
  }

  private onLockChanged(e: LockChangedEvent) {
    const a = this.attempt;
    if (!e.lockedObject) {
      return;
    }
    if (a && !a.reported && e.lockedObject.getFormID() === a.localId) {
      if (!e.lockedObject.isLocked()) {
        this.report(a, 0, Math.max(0, a.picksBefore - this.picksHeld()));
      }
      return;
    }
    if (a && e.lockedObject.getFormID() === a.localId) {
      return;
    }
    this.reportEngineWrite(e.lockedObject);
  }

  // thuum ADR-073 step 3 (decision 12): a lock this engine wrote by itself
  // (a script's Lock or SetLockLevel, an owner's door walk at a package's
  // start, an NPC's key or pick), never this game's own apply of the record
  // (ModelApplyUtils remembers each), goes to the server as outcome 3 with the
  // lock as the engine holds it. The server records it from the seat that
  // hosts the reference's cell or its load door's other side, folds a write
  // equal to the record, and sends the record back to any other seat (R0,
  // rule 5), so this game never judges ownership itself.
  // a reference bound with no recorded lock (dealWithRef): its standing lock,
  // when it has one at all, goes as the owner's report, and the reference is
  // kept by its cell for the re-report at a grant (bounded)
  private reportStanding(ref: ObjectReference) {
    const localId = ref.getFormID();
    if (!ref.isLocked() && ref.getLockLevel() <= 0) {
      return;
    }
    const cell = ref.getParentCell()?.getFormID() ?? 0;
    if (this.unrecorded.size >= LockService.unrecordedMax && !this.unrecorded.has(localId)) {
      const oldest = this.unrecorded.keys().next().value;
      if (oldest !== undefined) {
        this.unrecorded.delete(oldest);
        this.standingOwned.delete(oldest);
      }
    }
    this.unrecorded.set(localId, cell);
    // thuum #63: a load door's other side, read on the game thread now, so
    // the owner test knows it by the first write
    if (typeof this.sp.pollLinkedDoors === 'function') {
      this.sp.pollLinkedDoors([localId]);
    }
    const state = this.mayOwnState(ref);
    if (state === 'yes') {
      this.standingOwned.add(localId);
      this.reportEngineWrite(ref, true, 'standing');
    } else if (state === 'unknown') {
      this.hold(ref, 'standing');
    }
  }

  // thuum #47 and #63: a grant brings this game more to own. Each bound
  // reference with no recorded lock whose standing lock has not gone as an
  // owner's goes now if this game may own it, checked every recheckEveryMs
  // for recheckMs after the grant (the cell's grid square and the doors'
  // links answer from the game thread a frame or more later). Before, the
  // re-check matched a reference's parent cell to the granted cell, which a
  // persistent exterior door never matched (its parent is the worldspace's
  // persistent cell): the trader's door 0x1341f stood unrecorded under c1's
  // grant of 0x9731 (x-lead-claim 20261115-094229)
  private reportStandingIn(_cell: number) {
    this.recheckUntil = Date.now() + LockService.recheckMs;
  }

  private recheckStanding() {
    const now = Date.now();
    if (now > this.recheckUntil || now - this.lastRecheck < LockService.recheckEveryMs) {
      return;
    }
    this.lastRecheck = now;
    let n = 0;
    this.unrecorded.forEach((_cell, localId) => {
      if (this.standingOwned.has(localId)) {
        return;
      }
      const ref = ObjectReference.from(this.sp.Game.getFormEx(localId));
      if (ref && this.mayOwn(ref)) {
        this.standingOwned.add(localId);
        this.reportEngineWrite(ref, true, 'standing at the grant');
        ++n;
      }
    });
    if (n > 0) {
      this.sayOnce('grant', `a grant: ${n} standing lock(s) reported, this game hosting their cell or their door's other side`);
    }
  }

  // thuum #63 (x-door-night-lock 20261115-085512 and 100213): whether this
  // game may own a write on `ref`'s lock, the server's owner rule as far as
  // this game can read it: it hosts the cell the reference stands in, or its
  // load door's other side's (the engine shares one lock between the two),
  // or it is the world streamer, which owns what nobody hosts (ADR-073
  // decision 6). A door whose link has not answered yet counts as owned, so
  // the server judges it. A non-owner's engine runs the owners' schedules in
  // low process and writes their doors (Riverwood's houses on c1, 256
  // refusals in one run); those writes are no fact and stay here (ADR-063:
  // non-owners never simulate). The server still judges every report
  // thuum #63 (x-house-night-lock 20261115-150328: 256 E_LOCK_OWNER refusals
  // in 4 minutes, each pushed back to the seat that wrote it, both seats, no
  // "not reported" line on either): a door whose link had not answered yet
  // counted as owned, and a night's schedule writes a town's doors in one
  // burst before their links answer, so every one was reported and refused.
  // An unanswered link is now "unknown": the write waits for it (pending,
  // resolvePending) and goes only if the answer makes this game an owner
  private mayOwnState(ref: ObjectReference): 'yes' | 'no' | 'unknown' {
    const host = this.controller.lookupListener(HostService);
    if (host.hostsRef(ref)) {
      return 'yes';
    }
    if (this.controller.lookupListener(WorldGlobalService).isStreaming()) {
      return 'yes';
    }
    if (typeof this.sp.linkedDoorOf !== 'function') {
      return 'yes';
    }
    const pair = this.sp.linkedDoorOf(ref.getFormID());
    if (pair === null) {
      return 'unknown';
    }
    if (pair === 0) {
      return 'no';
    }
    const pairRef = ObjectReference.from(this.sp.Game.getFormEx(pair));
    return !!pairRef && host.hostsRef(pairRef) ? 'yes' : 'no';
  }

  private mayOwn(ref: ObjectReference): boolean {
    return this.mayOwnState(ref) === 'yes';
  }

  // a write or a standing lock whose owner test waits for its door's link:
  // asked on the game thread, held at most pendingMs, then reported when the
  // answer makes this game an owner, dropped when it does not
  private hold(ref: ObjectReference, why: string): void {
    const localId = ref.getFormID();
    if (typeof this.sp.pollLinkedDoors === 'function') {
      this.sp.pollLinkedDoors([localId]);
    }
    if (this.pending.size >= LockService.unrecordedMax && !this.pending.has(localId)) {
      return;
    }
    this.pending.set(localId, { until: Date.now() + LockService.pendingMs, why });
  }

  private resolvePending(): void {
    if (this.pending.size === 0) {
      return;
    }
    const now = Date.now();
    this.pending.forEach((p, localId) => {
      const ref = ObjectReference.from(this.sp.Game.getFormEx(localId));
      if (!ref) {
        this.pending.delete(localId);
        return;
      }
      const state = this.mayOwnState(ref);
      if (state === 'unknown') {
        if (now > p.until) {
          this.pending.delete(localId);
          this.sayOnce('link unanswered', `lock ${localId.toString(16)}: its door's link never answered within ${LockService.pendingMs} ms; not reported (#63)`);
        }
        return;
      }
      this.pending.delete(localId);
      if (state === 'no') {
        this.sayOnce('not owned', `lock ${localId.toString(16)}: its engine wrote it, and this game hosts neither its cell nor its door's other side and is no world streamer; not reported (#63)`);
        return;
      }
      if (p.why === 'standing') {
        this.standingOwned.add(localId);
      }
      if (p.why === 'standing') {
        this.reportEngineWrite(ref, true, 'standing');
      } else {
        this.reportEngineWrite(ref, false, 'write', true);
      }
    });
  }

  private unrecorded = new Map<number, number>();
  // thuum #63: the locks whose owner test waits for their door's link
  private pending = new Map<number, { until: number; why: string }>();
  private static readonly pendingMs = 5000;
  private standingOwned = new Set<number>();
  private recheckUntil = 0;
  private lastRecheck = 0;
  private static readonly unrecordedMax = 2048;
  private static readonly recheckMs = 3000;
  private static readonly recheckEveryMs = 500;

  private reportEngineWrite(ref: ObjectReference, force = false, why = 'write', ownerChecked = false) {
    const localId = ref.getFormID();
    // thuum #63: a write this game cannot own stays here (mayOwn), and one
    // whose door's link has not answered waits for it (hold); the prompted
    // player's word on a lock no game wrote is its own (#47)
    if (why === 'write' && !ownerChecked) {
      const state = this.mayOwnState(ref);
      if (state === 'unknown') {
        this.hold(ref, 'write');
        return;
      }
      if (state === 'no') {
        this.sayOnce('not owned', `lock ${localId.toString(16)}: its engine wrote it, and this game hosts neither its cell nor its door's other side and is no world streamer; not reported (#63)`);
        return;
      }
    }
    const locked = ref.isLocked();
    const level = ref.getLockLevel();
    if (!force && ModelApplyUtils.isAppliedLock(localId, locked, level)) {
      return;
    }
    const remoteId = localId < 0xff000000 ? localId : localIdToRemoteId(localId);
    if (!remoteId) {
      this.sayOnce('unmapped', `lock ${localId.toString(16)}: its engine wrote the lock on a reference with no server id here; not reported`);
      return;
    }
    // the write is this engine's state now: a second event for the same lock
    // reports nothing more
    ModelApplyUtils.noteLockApplied(localId, locked, level);
    this.controller.emitter.emit('sendMessage', {
      message: {
        t: MsgType.LockReport,
        refr: remoteId,
        outcome: 3,
        broken: 0,
        locked,
        level: Math.max(0, Math.min(255, Math.round(level))),
      },
      reliability: 'reliable',
    });
    this.sayOnce(
      why,
      why === 'write'
        ? `lock ${remoteId.toString(16)}: its engine wrote it ${locked ? 'locked' : 'unlocked'} (level ${level}), reported as the owner's write (ADR-073 step 3)`
        : `lock ${remoteId.toString(16)}: no recorded lock, its engine's standing lock ${locked ? 'locked' : 'unlocked'} (level ${level}) reported (${why}, ADR-073 step 3)`,
    );
  }

  // ADR-073 decision 7: a kind's first line in full, then a count at the
  // 2nd, 4th, 8th and so on
  private sayOnce(kind: string, line: string) {
    const n = (this.said.get(kind) ?? 0) + 1;
    this.said.set(kind, n);
    if (n === 1) {
      logTrace(this, line);
    } else if ((n & (n - 1)) === 0) {
      logTrace(this, `${line} (${n} so far)`);
    }
  }

  private said = new Map<string, number>();

  private onMenuClose(e: MenuCloseEvent) {
    if (e.name !== LockService.menu || !this.attempt) {
      return;
    }
    const a = this.attempt;
    this.attempt = undefined;
    if (a.reported) {
      return;
    }
    const refr = ObjectReference.from(this.sp.Game.getFormEx(a.localId));
    const broken = Math.max(0, a.picksBefore - this.picksHeld());
    if (refr && !refr.isLocked()) {
      this.report(a, 0, broken);
    } else if (broken > 0) {
      this.report(a, 1, broken);
    } else {
      this.report(a, 2, 0);
    }
  }

  private onUpdate() {
    this.recheckStanding();
    this.resolvePending();
    const a = this.attempt;
    if (!a || a.menuOpened) {
      return;
    }
    if (Date.now() - a.promptedAt > LockService.promptMs) {
      logTrace(
        this,
        `lock ${a.remoteId.toString(16)}: no Lockpicking Menu within ${LockService.promptMs} ms of the prompt (a key, or the engine's box); forgotten`,
      );
      this.attempt = undefined;
    }
  }

  private report(a: Attempt, outcome: number, broken: number) {
    a.reported = true;
    this.controller.emitter.emit('sendMessage', {
      message: {
        t: MsgType.LockReport,
        refr: a.remoteId,
        outcome,
        broken: Math.min(broken, 8),
      },
      reliability: 'reliable',
    });
    logTrace(
      this,
      `lock report ${a.remoteId.toString(16)}: ${['picked', 'broken', 'left'][outcome]}, ${broken} pick(s) broken`,
    );
  }

  private picksHeld(): number {
    const player = this.sp.Game.getPlayer();
    const pick = this.sp.Game.getFormEx(LockService.lockpick);
    return player && pick ? player.getItemCount(pick) : 0;
  }

  private attempt: Attempt | undefined;
  private static readonly menu = 'Lockpicking Menu';
  // the Lockpick, Skyrim.esm MISC 0xa
  private static readonly lockpick = 0xa;
  private static readonly promptMs = 3000;
}
