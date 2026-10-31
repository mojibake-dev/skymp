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
    this.controller.on('update', () => this.onUpdate());
  }

  private onPrompt(e: ConnectionMessage<LockPromptMessage>) {
    const remoteId = e.message.refr;
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
    logTrace(
      this,
      `lock prompt ${remoteId.toString(16)}: the engine's activation run (locked ${refr.isLocked()}, level ${refr.getLockLevel()}, ${this.attempt.picksBefore} picks held)`,
    );
    refr.activate(this.sp.Game.getPlayer(), true);
  }

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
    if (
      !a ||
      a.reported ||
      !e.lockedObject ||
      e.lockedObject.getFormID() !== a.localId
    ) {
      return;
    }
    if (!e.lockedObject.isLocked()) {
      this.report(a, 0, Math.max(0, a.picksBefore - this.picksHeld()));
    }
  }

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
