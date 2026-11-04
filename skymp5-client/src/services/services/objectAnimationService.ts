import { FormType, ObjectReference } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { ConnectionMessage } from '../events/connectionMessage';
import { ObjectAnimationMessage } from '../messages/objectAnimationMessage';
import { CreateActorMessage } from '../messages/createActorMessage';
import { MsgType } from '../../messages';
import { HostService } from './hostService';
import { localIdToRemoteId, remoteIdToLocalId } from '../../view/worldViewMisc';
import { logTrace } from '../../logging';

// thuum docs/verbs/traps.md (ADR-059 decision 3): the cell's host runs the
// world scripts, so guests see what the host's engine SHOWS. On every seat
// Skyrim Platform's sendAnimationEvent hook, matched in native to the
// world-object event family (rule 9; HookPattern: exact, or one '*' at an
// end), notes an event on a game-file reference, from either engine family:
// a behavior graph's event (NotifyAnimationGraph: the swinging wall, the
// plate) or a NiControllerManager sequence (TESObjectREFR::PlayAnimation_Impl:
// the puzzle pillars; the platform raises the sequence's name as the same
// hook, x-object-anim v3 20261102-051618 read the pillar silent without it);
// on the next update (natives live there) the seat that hosts the reference's
// cell reports it as ObjectAnimation, never for an actor (the stream's) or a
// door (isOpen's).
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
  'Trigger*', 'Turned*', 'StartState*', 'FullPull*', 'open', 'opened', 'close', 'Open', 'Close',
  'Down', '90*', '180*', '270*', 'Rearm', 'Reset', 'Idle',
];
// game-file references end where the server's copies begin; the player 0x14 is
// left out of the hook's native range (rule 9: nothing of its graph crosses)
const LAST_GAME_FILE_ID = 0xfeffffff;
const PLAYER_ID = 0x14;
// an event this seat played itself (a CreateActor's lastAnimation replay in
// remoteServer, a relayed ObjectAnimation here) re-enters the hook like the
// engine's own; it is no report (the review of 2026-11-01: the host would
// re-report its own replay and every guest would spin a stale event). The
// mark outlives remoteServer's five 2 s retries
// one retry interval of remoteServer's record replay: a mark hides an engine
// event equal to the marked one for this long and is consumed on its hit
// (the review of 843fed95, F2: a TypeScript playAnimation runs under
// CallNativeApi's Override, which the animation hooks return early under,
// so no replay re-enters the hook; the marks only ever hid genuine reports,
// the host's first turn within 20 s of a pillar's CreateActor among them)
const SELF_PLAYED_MS = 3_000;

export class ObjectAnimationService extends ClientListener {
  constructor(
    private sp: Sp,
    private controller: CombinedController,
  ) {
    super();
    this.controller.emitter.on('objectAnimationMessage', (e) =>
      this.onRelayed(e),
    );
    this.controller.emitter.on('createActorMessage', (e) =>
      this.onCreateActor(e),
    );
    for (const pattern of EVENT_PATTERNS) {
      const handler = {
        enter: () => { },
        leave: (ctx: { selfId: number; animEventName: string; animationSucceeded: boolean }) => {
          if (!ctx.animationSucceeded) {
            return;
          }
          this.noted.push({ selfId: ctx.selfId, event: ctx.animEventName });
        },
      };
      // two native ranges around the player's id
      this.sp.hooks.sendAnimationEvent.add(handler, 1, PLAYER_ID - 1, pattern);
      this.sp.hooks.sendAnimationEvent.add(handler, PLAYER_ID + 1, LAST_GAME_FILE_ID, pattern);
    }
    this.controller.on('update', () => this.onUpdate());
  }

  // a recorded lastAnimation arriving with a game-file reference's CreateActor
  // is about to be replayed by remoteServer on this seat: not ours to report
  private onCreateActor(e: ConnectionMessage<CreateActorMessage>) {
    const msg = e.message;
    const event = msg.props?.lastAnimation;
    if (typeof event !== 'string' || msg.refrId === undefined || msg.refrId >= 0xff000000) {
      return;
    }
    this.markSelfPlayed(msg.refrId, event);
  }

  private markSelfPlayed(localId: number, event: string) {
    this.selfPlayed.set(`${localId}:${event}`, Date.now() + SELF_PLAYED_MS);
  }

  private wasSelfPlayed(localId: number, event: string): boolean {
    const key = `${localId}:${event}`;
    const until = this.selfPlayed.get(key);
    if (until === undefined) {
      return false;
    }
    this.selfPlayed.delete(key);
    return Date.now() <= until;
  }

  private onUpdate() {
    if (this.noted.length === 0) {
      return;
    }
    const noted = this.noted;
    this.noted = [];
    const host = this.controller.lookupListener(HostService);
    for (const { selfId, event } of noted) {
      if (this.wasSelfPlayed(selfId, event)) {
        // rule 8: the mark's hit is logged so a run says whether a replay
        // ever re-enters the hook (zero lines with a record present means
        // the marks can go)
        logTrace(this, `object animation ${selfId.toString(16)} ${event} dropped as self-played`);
        continue;
      }
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
    const localId = remoteIdToLocalId(remoteRefr);
    const refr = ObjectReference.from(this.sp.Game.getFormEx(localId));
    if (!refr) {
      return;
    }
    this.playRelayed(refr, event, "relayed", remoteRefr);
  }

  // Behavior00.hkx, the puzzle pillar's graph (RuinsPuzzlePillar01.hkx's
  // project, read from Skyrim - Animations.bsa on 2026-11-04): a state
  // machine of Side01, Side02 and Side03 whose Turn0N fires on Trigger0N from
  // Side0N ALONE; StartState02 and StartState03 jump to a side without a
  // turn, Side01 the graph's own start. A seat whose graph sits on another side
  // than the host's drops every relayed Trigger silently until the host's
  // cycle passes its side: the host's own script jumps its graph at the load
  // with StartState0N, which the relay never carried, and a late joiner's
  // replay of the record's Trigger0N from Side01 has no transition either
  // (playtest 44: "2nd pillar STARTED desynced ... resumed syncing once the
  // rotation passed the one it was desynced ON"; playtest 45: the guest's own
  // turn on the host's screen alone, the gate opening on the host's state;
  // numidium's read of the graph, 2026-11-04). So a relayed or replayed
  // Trigger0N is played after the graph's own jump to Side0N (StartState0N;
  // Side01 has none, so Trigger01 plays alone), which makes every relayed
  // turn its own side sync; StartState* is relayed and recorded like any
  // other event. The line keeps "object animation <refr> <event> played" as
  // its head (b-puzzle-relay's claim). HYPOTHESIS until x-puzzle-guest-view
  // v2 reads the node on the guest's screen.
  // The jump is for a LIVE relay alone (a seat that does not host the
  // reference: the host receives no relay of its own reports). The record's
  // replay at a load (remoteServer) plays the plain event and skips a seat
  // that hosts the cell: playtest 46 (2026-11-04 22:42Z, 843fed95 live):
  // with the jump in the replay, the host's graph was moved to the record's
  // side while its script stayed on its own initial state, so the script's
  // first Trigger0N had no transition, PlayAnimationAndWait never saw its
  // Turned0N and the script stayed busy for good: no pillar turned on either
  // seat. The host's graph is its script's; the record reaches the host's
  // script through its activations (ADR-059 decision 5), not through graph
  // events. Every seat's script runs OnCellLoad (a lifecycle event passes
  // the gate everywhere) and jumps its graph to the reference's initial
  // side, so a guest sits on the host's initial side at a load; a plain
  // replay of a recorded Trigger0N on a guest moves it one side past a host
  // whose script restarted, and the next live relay's jump brings it back
  // (one turn of drift, self-healing), the limit until the host's script
  // state is fed from the record. "after StartState0N refused" on a guest
  // already on Side0N is the expected word (a self-transition the graph may
  // refuse, HYPOTHESIS), not a fault.
  playRelayed(refr: ObjectReference, event: string, how: string, remoteRefr?: number, jumpFirst = true): boolean {
    const localId = refr.getFormID();
    const id = (remoteRefr ?? localIdToRemoteId(localId)).toString(16);
    const m = jumpFirst ? /^Trigger0([23])$/.exec(event) : null;
    let jump = "";
    if (m) {
      const jumpEvent = `StartState0${m[1]}`;
      this.markSelfPlayed(localId, jumpEvent);
      const taken = refr.playAnimation(jumpEvent);
      jump = `, after ${jumpEvent} ${taken ? "taken" : "refused"}`;
    }
    this.markSelfPlayed(localId, event);
    const played = refr.playAnimation(event);
    // "played" is the live relay's word (b-puzzle-relay's claim on a guest);
    // the record's replay says "replayed", which that claim's " played" never
    // matches, so a host's refused replays do not count as plays
    logTrace(this, `object animation ${id} ${event} ${jumpFirst ? "played" : "replayed"} (${how}, ${played ? "taken" : "refused"}${jump})`);
    return played;
  }

  private noted: { selfId: number; event: string }[] = [];
  private selfPlayed = new Map<string, number>();
}
