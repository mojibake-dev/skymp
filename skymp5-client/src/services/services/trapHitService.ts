import { ObjectReference, TrapHitEvent } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { MsgType } from '../../messages';
import { HostService } from './hostService';
import { localIdToRemoteId } from '../../view/worldViewMisc';
import { logTrace } from '../../logging';

// thuum docs/verbs/traps.md (ADR-059 decision 4): the cell's host runs the
// trap's scripts, so a trap's harm on another player's body is dealt by the
// host's engine, which the platform's DamageActor replacement cuts for a
// figure; the server owns that player's health. Skyrim Platform's trapHit
// event (the fork's hook on the Papyrus native ProcessTrapHit) names the
// trap, the hit reference and the script's damage; when this game hosts the
// trap's cell and the hit reference is a server copy (another player's
// figure: the player's own body is its own actor-values report, a game-file
// NPC's values are the hosted state), the host reports TrapHit (MsgType 72)
// and the server validates it against the trap's own table and reach
// (wire-rules traps) before applying it. R1. Line for the lab: "trap hit
// <trap> on <target> for <damage> reported".
const FIRST_SERVER_COPY = 0xff000000;

export class TrapHitService extends ClientListener {
  constructor(
    private sp: Sp,
    private controller: CombinedController,
  ) {
    super();
    this.controller.on('trapHit', (e) => this.onTrapHit(e));
  }

  private onTrapHit(e: TrapHitEvent) {
    if (e.trap === 0 || e.target < FIRST_SERVER_COPY) {
      return;
    }
    if (!Number.isFinite(e.damage) || e.damage < 0) {
      return;
    }
    const trap = ObjectReference.from(this.sp.Game.getFormEx(e.trap));
    if (!trap) {
      return;
    }
    const host = this.controller.lookupListener(HostService);
    if (!host.isHostedHere(trap)) {
      return;
    }
    const refr = localIdToRemoteId(e.trap);
    const target = localIdToRemoteId(e.target);
    if (refr === 0 || target === 0) {
      return;
    }
    this.controller.emitter.emit('sendMessage', {
      message: { t: MsgType.TrapHit, refr, target, damage: e.damage },
      reliability: 'reliable',
    });
    logTrace(
      this,
      `trap hit ${e.trap.toString(16)} on ${e.target.toString(16)} for ${e.damage} reported`,
    );
  }
}
