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
// A played pair comes back through the engine's own activate event, which
// ActivationService reads; since ADR-068 D-S3's phase 2 a player-only
// trigger's pair is reported from any seat, so each play is marked here and
// ActivationService consumes the mark (wasPlayedHere) instead of reporting the
// relay's own play, which the server would relay back to its reporter.
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
    this.markPlayed(target.getFormID(), caster.getFormID());
    target.activate(caster, false);
    logTrace(
      this,
      `relayed activation ${remoteTarget.toString(16)} by ${remoteCaster.toString(16)}`,
    );
  }

  // Whether this seat just played the pair (local ids, the activate event's);
  // the mark is consumed by its hit
  wasPlayedHere(localTarget: number, localCaster: number): boolean {
    const key = `${localTarget}:${localCaster}`;
    const until = this.played.get(key);
    if (until === undefined) {
      return false;
    }
    this.played.delete(key);
    return Date.now() <= until;
  }

  private markPlayed(localTarget: number, localCaster: number) {
    if (this.played.size >= RelayedActivationService.playedMax) {
      this.played.clear();
    }
    this.played.set(
      `${localTarget}:${localCaster}`,
      Date.now() + RelayedActivationService.playedMs,
    );
  }

  // the pairs played here, until when their activate event counts as the
  // relay's own (UNMEASURED: the engine raises it within a frame or two of
  // the activate call)
  private readonly played = new Map<string, number>();
  private static readonly playedMs = 2000;
  private static readonly playedMax = 64;
}
