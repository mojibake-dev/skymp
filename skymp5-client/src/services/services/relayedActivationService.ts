import { ObjectReference } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { ConnectionMessage } from '../events/connectionMessage';
import { RelayedActivationMessage } from '../messages/relayedActivationMessage';
import { remoteIdToLocalId } from '../../view/worldViewMisc';
import { logTrace } from '../../logging';

// thuum docs/verbs/traps.md (M4): a linked activation the cell's host
// reported, played here with the caster the host's engine named
// (ObjectReference.activate takes any activator), so the spike wall swings
// and the portcullis rises on this screen as on the host's. A message handler
// has no Papyrus VM context, so the natives wait for the next update, as the
// lock prompt's do. HYPOTHESIS until x-bleak-traps: an activation-blocked
// reference (every ACTI and DOOR here, objectReferenceEx) still runs its own
// script on a scripted activate call; a relayed trap this game's own engine
// also fires (both players on one plate) plays twice.
//
// Line for the lab: "relayed activation <target> by <caster>".
export class RelayedActivationService extends ClientListener {
  constructor(
    private sp: Sp,
    private controller: CombinedController,
  ) {
    super();
    this.controller.emitter.on('relayedActivationMessage', (e) =>
      this.onRelayed(e),
    );
  }

  private onRelayed(e: ConnectionMessage<RelayedActivationMessage>) {
    const { target, caster } = e.message;
    this.controller.once('update', () => this.play(target, caster));
  }

  private play(remoteTarget: number, remoteCaster: number) {
    const target = ObjectReference.from(
      this.sp.Game.getFormEx(remoteIdToLocalId(remoteTarget)),
    );
    const caster = ObjectReference.from(
      this.sp.Game.getFormEx(remoteIdToLocalId(remoteCaster)),
    );
    if (!target || !caster) {
      return;
    }
    target.activate(caster, false);
    logTrace(
      this,
      `relayed activation ${remoteTarget.toString(16)} by ${remoteCaster.toString(16)}`,
    );
  }
}
