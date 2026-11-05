import { FormType, ObjectReference, Utility } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { ConnectionMessage } from '../events/connectionMessage';
import { ObjectAnimationMessage } from '../messages/objectAnimationMessage';
import { CreateActorMessage } from '../messages/createActorMessage';
import { MsgType } from '../../messages';
import { HostService } from './hostService';
import { localIdToRemoteId, remoteIdToLocalId } from '../../view/worldViewMisc';
import { logError, logLab, logTrace } from '../../logging';

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
// the Nordic puzzle pillar (docs/verbs/traps.md "The pillar's side"): its
// graph Behavior00.hkx is a three-cycle, Side01, Side02, Side03, turned by
// Trigger0N from Side0N alone; StartState02/03 jump to a side, Side01 is
// the graph's own start; a reference is known as one by the base
// NorDefaultPuzzlePillar01 or by a Trigger0N or StartState0N it played
const PILLAR_BASES = new Set<number>([0xaa8ce]);
const PILLAR_CYCLE = 3;
// one turn's animation, the script's busy window (UNMEASURED: the lab's read
// in x-puzzle-handoff; 46's server log puts a report 66 ms after the press
// and the next accepted press about two seconds on)
const PILLAR_TURN_S = 2.5;
// the side a pillar's record puts it on: StartState0M is M (a jump at a load,
// no turn since), Trigger0N turns from N to the next
const sideOfRecord = (event: string): number | undefined => {
  const jump = /^StartState0([1-3])$/.exec(event);
  if (jump) {
    return Number(jump[1]);
  }
  const turn = /^Trigger0([1-3])$/.exec(event);
  if (turn) {
    return (Number(turn[1]) % PILLAR_CYCLE) + 1;
  }
  return undefined;
};
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
    // the hand-off (docs/verbs/traps.md "The next build: the hand-off",
    // playtest 46's second lay): a seat's pillar graphs move while it is a
    // guest (the record's replay at its load, every relay after), its
    // scripts sit on their initial sides, and the seat that inherits the
    // cell has scripts whose first Trigger0N has no transition on graphs it
    // never moved: PlayAnimationAndWait never returns and the pillar is dead
    // for every seat. At a grant the new host puts each pillar's graph back
    // on its script's initial side and feeds its own script the record's
    // side as activations, so script, graph and record agree before the
    // first press
    // a message handler may call no native (hostService's rule, the
    // platform's gameThrQ is the update's): the grant's work waits for the
    // next update (the review of 30839d4b, F1)
    this.controller.emitter.on('cellHostChanged', (e) => {
      if (e.hosted) {
        this.controller.once('update', () => this.onCellHosted(e.cell));
      }
    });
    for (const pattern of EVENT_PATTERNS) {
      const handler = {
        enter: () => { },
        leave: (ctx: { selfId: number; animEventName: string; animationSucceeded: boolean }) => {
          // the engine's return flag (NotifyAnimationGraph's) is kept on the
          // note: a pillar's Trigger0N is reported whatever it says (below),
          // every other event only when it took
          this.noted.push({ selfId: ctx.selfId, event: ctx.animEventName, took: ctx.animationSucceeded });
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
    if (typeof event === 'string' && msg.refrId !== undefined && msg.refrId < 0xff000000) {
      this.noteRecord(remoteIdToLocalId(msg.refrId), event);
    }
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
    for (const { selfId, event, took } of noted) {
      // playtest 46 (2026-11-04 23:36Z), and 45's and the green relay run's
      // server logs before it: the host never reported a pillar's Trigger02,
      // only Trigger01 and Trigger03, so a guest sat one side behind until
      // the cycle passed it (44's "resumed syncing once the rotation passed
      // the one it was desynced ON"; with the side jump, the visible double
      // catch-up). The whale turn's Trigger02 comes back from the engine
      // with the flag false while the pillar visibly turns (HYPOTHESIS on
      // the why: Behavior00's Turn02 a different transition kind than
      // Turn01 and Turn03, Havok's handleEvent answering false for it), so a
      // pillar's Trigger0N is reported whatever the flag says: the script
      // sends a Trigger only when it turns, and a refused one is said
      const pillarTrigger = /^Trigger0[1-3]$/.test(event);
      if (!took && !pillarTrigger) {
        continue;
      }
      // every genuine hook event on a pillar is the local script's own act
      // (a TypeScript play never re-enters the hook): StartState0N at a cell
      // load puts the script on side N, its own Trigger0N moves it to N+1,
      // so the LAST side-naming event is the script's side, which a seat
      // that hosted, turned and walked out keeps in its Papyrus state (the
      // review of 30839d4b, F3)
      this.notePillar(selfId, event);
      const side = sideOfRecord(event);
      if (side !== undefined) {
        this.scriptSide.set(selfId, side);
      }
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
      logTrace(this, `object animation ${selfId.toString(16)} ${event} reported${took ? "" : " (the graph's flag said refused)"}`);
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
  // The jump is a guest's (a seat that does not host the reference: the
  // host receives no relay of its own reports and never replays the record,
  // remoteServer's host-skip): playtest 46 (2026-11-04 22:42Z, 843fed95 live):
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
    this.noteRecord(localId, event);
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

  // the server's last event for a reference as this seat knows it
  // (CreateActor's lastAnimation, then every relayed or replayed event)
  private noteRecord(localId: number, event: string) {
    this.recordEvent.set(localId, event);
    this.notePillar(localId, event);
  }

  private notePillar(localId: number, event: string) {
    if (/^(Trigger|StartState)0[1-3]$/.test(event)) {
      this.pillars.add(localId);
    }
  }

  private isPillar(refr: ObjectReference): boolean {
    const base = refr.getBaseObject()?.getFormID() ?? 0;
    if (base !== 0) {
      return PILLAR_BASES.has(base);
    }
    return this.pillars.has(refr.getFormID());
  }

  private onCellHosted(cell: number) {
    const ids = new Set<number>(this.pillars);
    this.recordEvent.forEach((_e, id) => ids.add(id));
    ids.forEach((id) => {
      if (this.resyncing.has(id)) {
        return;
      }
      const refr = ObjectReference.from(this.sp.Game.getFormEx(id));
      if (!refr || !this.isPillar(refr)) {
        return;
      }
      const parent = refr.getParentCell();
      if (!parent || parent.getFormID() !== cell) {
        return;
      }
      const initial = this.scriptSide.get(id) ?? 1;
      const record = this.recordEvent.get(id);
      const recordSide = record !== undefined ? sideOfRecord(record) : undefined;
      if (record !== undefined && recordSide === undefined) {
        logLab('verb:traps', `pillar ${id.toString(16)} at the grant of ${cell.toString(16)}: the record's event ${record} names no side, the script's side ${initial} stands`);
      }
      const target = recordSide ?? initial;
      const turns = (((target - initial) % PILLAR_CYCLE) + PILLAR_CYCLE) % PILLAR_CYCLE;
      void this.resync(refr, id, cell, initial, turns, record ?? 'none', target);
    });
  }

  // the graph back on the script's side, then the record's side fed to the
  // script as the host's own activations (each reported as the host's turn,
  // relayed to the guests, recorded). The reset is one turn at most: a jump
  // for Side02/03; for Side01, which has no jump event, StartState03 (instant
  // from 01 or 02, refused on 03) then Trigger03. No activation block: the
  // Creation Kit's BlockActivation withholds default processing alone and
  // OnActivate still reaches the script (the review of 30839d4b, F2), so a
  // press inside the reset's turn (under three seconds at a grant) is the
  // known limit, the hold of the cell's scripts through the gate its next
  // shape. A TypeScript play never re-enters the hook, so nothing here is
  // marked self-played (F6). The feed stops the moment this seat no longer
  // hosts the cell (F5)
  private async resync(refr: ObjectReference, id: number, cell: number, initial: number, turns: number, record: string, target: number): Promise<void> {
    const host = this.controller.lookupListener(HostService);
    try {
      this.resyncing.add(id);
      if (initial === 1) {
        refr.playAnimation('StartState03');
        await Utility.wait(0.5);
        if (!host.hostsCell(cell)) {
          return;
        }
        if (refr.playAnimation('Trigger03')) {
          await Utility.wait(PILLAR_TURN_S);
        }
      } else {
        refr.playAnimation(`StartState0${initial}`);
        await Utility.wait(0.5);
      }
      const player = this.sp.Game.getPlayer();
      for (let i = 0; i < turns && player; i++) {
        if (!host.hostsCell(cell)) {
          logLab('verb:traps', `pillar ${id.toString(16)}: the re-sync at the grant of ${cell.toString(16)} stopped, this seat no longer hosts the cell (${i} of ${turns} turn(s) fed)`);
          return;
        }
        refr.activate(player, false);
        await Utility.wait(PILLAR_TURN_S + 0.5);
      }
      logLab('verb:traps', `pillar ${id.toString(16)} re-synced at the grant of ${cell.toString(16)}: the script's side ${initial}, the record ${record} (side ${target}), ${turns} turn(s) fed to the script`);
    } catch (e) {
      logError(this, `pillar ${id.toString(16)} re-sync at the grant of ${cell.toString(16)} failed: ${e}`);
    } finally {
      this.resyncing.delete(id);
    }
  }

  private noted: { selfId: number; event: string; took: boolean }[] = [];
  private selfPlayed = new Map<string, number>();
  private scriptSide = new Map<number, number>();
  private recordEvent = new Map<number, string>();
  private pillars = new Set<number>();
  private resyncing = new Set<number>();
}
