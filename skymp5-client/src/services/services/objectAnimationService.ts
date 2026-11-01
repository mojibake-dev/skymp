import { FormType, ObjectReference } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { ConnectionMessage } from '../events/connectionMessage';
import { ObjectAnimationMessage } from '../messages/objectAnimationMessage';
import { MsgType } from '../../messages';
import { HostService } from './hostService';
import { localIdToRemoteId, remoteIdToLocalId } from '../../view/worldViewMisc';
import { logTrace } from '../../logging';

// thuum docs/verbs/traps.md (ADR-059 decision 3): the cell's host runs the
// world scripts, so guests see what the host's engine SHOWS. On every seat
// Skyrim Platform's sendAnimationEvent hook, matched in native to the
// world-object event family (rule 9; HookPattern: exact, or one '*' at an
// end), notes an event on a game-file reference; on the next update (natives
// live there) the seat that hosts the reference's cell reports it as
// ObjectAnimation, never for an actor (the stream's) or a door (isOpen's).
// A relayed ObjectAnimation plays on this seat through the engine's own
// PlayAnimation, as remoteServer replays a CreateActor's lastAnimation.
//
// The family, from the scripts' own names (the game's pex files and the base
// records' VMAD, read 2026-11-01; docs/verbs/traps.md): the puzzle pillars'
// Trigger0N and Turned0N (defaultPuzzlePillarScript), a lever's FullPull and
// FullPulledDown (NorLever01SCRIPT), a two-state activator's open, close and
// opened (default2StateActivator: a portcullis), a pressure plate's Down
// (PressurePlate), a swinging wall's 90For/180For/270For, the Half, End and
// Back of each, and Rearm (TrapSwingingWall's anim properties; TransStart,
// TransHalf, TransStop and Done are the graph's own events back to the
// script, not sent to it), a trap's Reset and Idle (TrapBase). The engine's
// exact set is HYPOTHESIS until x-object-anim reads it. Lines for the lab:
// "object animation <refr> <event> reported" on the host, "object animation
// <refr> <event> played" on a guest.
const EVENT_PATTERNS = [
  'Trigger*', 'Turned*', 'FullPull*', 'open', 'opened', 'close', 'Open', 'Close',
  'Down', '90*', '180*', '270*', 'Rearm', 'Reset', 'Idle',
];
// game-file references end where the server's copies begin
const LAST_GAME_FILE_ID = 0xfeffffff;

export class ObjectAnimationService extends ClientListener {
  constructor(
    private sp: Sp,
    private controller: CombinedController,
  ) {
    super();
    this.controller.emitter.on('objectAnimationMessage', (e) =>
      this.onRelayed(e),
    );
    for (const pattern of EVENT_PATTERNS) {
      const handler = {
        enter: () => { },
        leave: (ctx: { selfId: number; animEventName: string; animationSucceeded: boolean }) => {
          if (!ctx.animationSucceeded || ctx.selfId === 0x14) {
            return;
          }
          this.noted.push({ selfId: ctx.selfId, event: ctx.animEventName });
        },
      };
      this.sp.hooks.sendAnimationEvent.add(handler, 1, LAST_GAME_FILE_ID, pattern);
    }
    this.controller.on('update', () => this.onUpdate());
  }

  private onUpdate() {
    if (this.noted.length === 0) {
      return;
    }
    const noted = this.noted;
    this.noted = [];
    const host = this.controller.lookupListener(HostService);
    for (const { selfId, event } of noted) {
      const refr = ObjectReference.from(this.sp.Game.getFormEx(selfId));
      if (!refr || this.sp.Actor.from(refr)) {
        continue;
      }
      if (refr.getBaseObject()?.getType() === FormType.Door) {
        continue;
      }
      if (!host.isHostedHere(refr)) {
        continue;
      }
      this.controller.emitter.emit('sendMessage', {
        message: { t: MsgType.ObjectAnimation, refr: localIdToRemoteId(selfId), event },
        reliability: 'reliable',
      });
      logTrace(this, `object animation ${selfId.toString(16)} ${event} reported`);
    }
  }

  private onRelayed(e: ConnectionMessage<ObjectAnimationMessage>) {
    const { refr, event } = e.message;
    this.controller.once('update', () => this.play(refr, event));
  }

  private play(remoteRefr: number, event: string) {
    const refr = ObjectReference.from(
      this.sp.Game.getFormEx(remoteIdToLocalId(remoteRefr)),
    );
    if (!refr) {
      return;
    }
    refr.playAnimation(event);
    logTrace(this, `object animation ${remoteRefr.toString(16)} ${event} played`);
  }

  private noted: { selfId: number; event: string }[] = [];
}
