import { EffectShader, FormType, FurnitureAnimationEvent, ObjectReference, TextureSet, Utility, VisualEffect } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { ConnectionMessage } from '../events/connectionMessage';
import { ObjectAnimationMessage } from '../messages/objectAnimationMessage';
import { CreateActorMessage } from '../messages/createActorMessage';
import { ActivateMessage } from '../messages/activateMessage';
import { AnyMessage } from '../messages/anyMessage';
import { GraphVar } from '../messages/scriptStateApplyMessage';
import { MsgType } from '../../messages';
import { HostService } from './hostService';
import { ScriptStateService } from './scriptStateService';
import { LOOK_KEEP_CODES, SpSnippetService } from './spSnippetService';
import { ObjectReferenceEx } from '../../extensions/objectReferenceEx';
import { localIdToRemoteId, remoteIdToLocalId, snippetIdToLocalId } from '../../view/worldViewMisc';
import { logError, logLab, logTrace } from '../../logging';
import { HELD_PLAY_MS, HeldPlays } from '../../sync/heldPlays';
import { reapplyNow } from '../../sync/reapplyGate';

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
// The furniture (thuum docs/DECISIONS.md D-W1 and D-C2's lever item, H-P1;
// docs/research/fifty-fixes-2026-11-07.md 1(d)): a FURN's own graph event
// (the puzzle lever's Lever_PlayerPush, which the engine's NoPlayer branch
// sends inside remoteServer's activate(player, true), under the platform's
// mark on the worker, where the hook above never crosses) comes from the
// platform's furniture branch as furnitureAnimation, whatever its name (the
// idle data picks it, so no pattern), and is noted beside the hook's events. Its owner is its user
// (ADR-063, a container's opener's rule): this seat while the server's
// OpenContainer named it here (until this seat's closing activation or
// USE_MS, or while the player's furniture is it), else the seat that hosts
// its cell (an NPC's use); the server checks the recorded occupant's owner.
// A relay's play here re-enters through the furniture branch (it notes inside
// the platform's call too, unlike the hook above), and its self-played mark
// drops it.
// A furniture's event is kept nowhere (a lever back at rest would push again
// at every late join and every 3D load). The client setting
// furnitureAnimation (on unless false) turns on the platform's branch
// (setFurnitureAnimation) and this path; off, or a platform without them
// (said once), the cell's host reports as before. The report keeps "object
// animation <refr> <event> reported", then "(furniture, this seat's use)" or
// "(furniture, cell hosted here)".
// The platform's native filter matches an event name's case exactly (Handler::Matches compares bytes), while the
// engine matches a graph event without case; a script may name its event in either: defaultPuzzlePillarScript turns
// the whale with PlayAnimationAndWait("trigger02", "Turned02") (x-native-frame 20261105-033116, the script's own call
// read at NativeFunctionBase::Call), so 'Trigger*' never let the whale turn through and no seat ever reported it
// (playtests 44 to 46). Each pillar pattern is registered in both cases; every match on a name below is case-blind.
const EVENT_PATTERNS = [
  'Trigger*', 'trigger*', 'Turned*', 'turned*', 'StartState*', 'startState*', 'startstate*', 'FullPull*', 'open', 'opened',
  'close', 'Open', 'Close', 'Down', '90*', '180*', '270*', 'Rearm', 'Reset', 'Idle',
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
// rung 3: a pillar whose script record was applied at a grant without its
// view (no 3D before the gate opened) waits this long for its 3D, asked every
// step, before its graph goes on the record's side (UNMEASURED)
const GRAPH_3D_WAIT_S = 10;
const GRAPH_WAIT_STEP_S = 0.25;
// the side a pillar's record puts it on: StartState0M is M (a jump at a load,
// no turn since), Trigger0N turns from N to the next
const sideOfRecord = (event: string): number | undefined => {
  const jump = /^StartState0([1-3])$/i.exec(event);
  if (jump) {
    return Number(jump[1]);
  }
  const turn = /^Trigger0([1-3])$/i.exec(event);
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
// (the review of 843fed95, F2: a TypeScript playAnimation runs under the
// platform's mark on the thread that runs it, PlatformCall.h, which the
// animation hooks return early under, so no replay re-enters the hook;
// HYPOTHESIS: the graph's event is raised on that thread, inside the call;
// the marks only ever hid genuine reports,
// the host's first turn within 20 s of a pillar's CreateActor among them)
const SELF_PLAYED_MS = 3_000;
// thuum docs/DECISIONS.md ADR-068 rung 3 (the script record), its view past a
// 3D unload (playtest 49, 2026-11-06, the guest's console): after a server
// restart the guest played each pillar's view ("view Trigger01 to play (seq
// 4, none here)", 06:13:30.739), was teleported out of the Temple and back,
// and the apply at 06:14:25.802 judged "view Trigger01 current (seq 4)" from
// that play while the pillars' 3D had come back at its default faces; the
// cell's two loads after it applied nothing. So this service keeps each world
// object's last animation (keep, below), and a seat that does not host the
// reference's cell plays it again at every 3D load of the reference: the node
// overrides' shape (formView puts its node scales on at each 3D load,
// NodeOverridesService the player's), is3DLoaded polled on the update over
// the kept set (the platform's objectLoaded, TESObjectLoadedEvent, has no
// listener in this client and is unread for a reference that is no actor).
// The kept set and the polls an update are bounded, and a reference whose 3D
// is not here is looked at every REAPPLY_WAIT_MS, not every update; a
// re-apply the graph refuses is tried again, REAPPLY_TRIES in all a second
// apart, as ScriptStateService plays a guest's view (its VIEW_WAIT_MS,
// VIEW_TRIES and VIEW_RETRY_MS).
// Its line, "[verb:scripts] object animation <refr> <event> re-applied at a
// 3D load (seq <n>)", is no "played" line: a relay claim (b-puzzle-relay)
// counts relays alone. HYPOTHESIS until a probe reads a guest's pillar nodes
// equal to the host's after a teleport out and back
const KEPT_MAX = 128;
const POLLS_PER_UPDATE = 32;
const REAPPLY_WAIT_MS = 250;
const REAPPLY_TRIES = 3;
const REAPPLY_RETRY_MS = 1000;
// the re-apply's line, at most this often per reference
const REAPPLY_LINE_MS = 10_000;
// the game-file references a create word named in this connection, at most
// (past it the set starts over: a relay is kept again after its reference's
// next create word)
const SUBSCRIBED_MAX = 16384;
// the furniture: a use this seat's OpenContainer opened counts as this seat's
// for this long at most (UNMEASURED: remoteServer's furniture wait gives up
// at 15 s and then sends the closing activation), and the uses kept at most
const USE_MS = 20_000;
const USES_MAX = 64;

// a world object's last animation as this seat keeps it: the record's view
// or the newest relay, whichever has the higher seq (SpSnippetService's
// per-object seq; a relayed ObjectAnimation carries none and is the newest
// word here, so it takes the newest seq known for the object)
//
// D-C3 A (PENDING ELI; docs/research/fifty-fixes-2026-11-07.md 1(d)): the
// entry carries the object's graph variables too (the cobweb's fToggleBlend,
// set by FXtriggerAmbFX's SetAnimationVariableFloat on its owner), each with
// its own seq, the newest per name kept: a relayed call's (setVariable) or
// the record's (applyRecordVariables). A graph variable lives on the 3D's
// graph (H-C3b until x-cobweb-view), so a seat that does not host the cell
// sets them again at each 3D load, before it plays the kept event. An entry
// may hold variables and no event ("", seq 0, never shown). Known limit: the
// host's own variables are kept nowhere here (its script's calls are never
// relayed back to it), so its own 3D reload does not set them again. The
// names are keyed by their lower case (the engine's strings are case-blind,
// markSelfPlayed's rule; the server keeps one variable per name ignoring
// case), each kept with the spelling of its newest call, which setOne passes
type KeptVar = {
  name: string;            // the newest call's own spelling
  k: number;               // 3 int, 4 float, 5 bool (ScriptVar's folded kinds)
  value: number | boolean;
  seq: number;             // the object's seq of the call that set it
};
// The 3D keep of a relayed look (thuum 3df63969, the reanimate-fx memo's 6.4
// D6 B, lab/.cache/drafts/reanimate-fx.md): the seam relays an actor's look
// rows from its simulator to its listeners, and SpSnippetService ran each at
// once. A look draws on its subject's 3D, so one that lands before the copy's
// 3D does nothing there, and one the 3D's unload takes is gone at its reload
// (apocrypha's code read of 2026-11-14, evening: EffectShader.Play,
// VisualEffect.Play and SetEyeTexture need the loaded 3D; the unload's part
// HYPOTHESIS). Undead FX plays a stage's flat shader with no duration and
// again every 2.5 s, and a transition shader for 2.5 s (the memo's 6.2), so a
// copy that walks back in waits up to 2.5 s for its holes, and a transition
// that lands with no 3D is never seen. So a look whose subject this seat does
// not own (an actor's simulator, HostService.isMineLocal; any other
// reference's cell host, epochForActor) is kept on the subject's entry here,
// beside its graph variables: the last Play per (subject, shader or effect)
// and the last eye texture. One whose 3D is here plays at once ("played"); one
// whose 3D is not is kept and plays at the load ("held for 3D"); each later 3D
// load plays every kept one again ("replayed at load", pollOne's same poll,
// no scan of its own: rule 9), a timed Play for the seconds it has left from
// its message's arrival, one with no duration until its Stop; a Stop stops
// it here and clears it; a timed one whose seconds ran out goes ("dropped as
// expired"). A look this seat owns, a subject no create word named, or a row
// that keeps nothing runs as it always has. Each outcome's first line is said
// whole, the rest counted in LOOK_SUMMARY_MS windows (ADR-073 decision 7).
// The kinds: an EffectShader's Play and Stop ("shader"), a VisualEffect's
// ("effect"), Actor.SetEyeTexture ("eyes": its subject the call's self, one
// kept per subject, no Stop). HYPOTHESIS until x-reanimate-claim v2's U3 (the
// memo's 6.5) reads the stage on a copy that walked off and back
export type LookKind = 'shader' | 'effect' | 'eyes';
// a relayed look as SpSnippetService reads it off its snippet
export type RelayedLook = {
  row: string;      // its class.function as relayed, for the lines
  kind: LookKind;
  stop: boolean;    // a Stop, which clears what it names
  subject: number;  // the subject's server id (a Self row's may be a long id)
  form: number;     // the shader's, the effect's or the texture set's server id (0: None)
  seconds: number;  // a Play's duration as relayed, below 0 for none (the eyes: -1)
  facing: number;   // VisualEffect.Play's akFacingObject by server id, 0 for None
  seq: number;      // the relay's seq (the effect form's for an Arg0Method row)
  arrived: number;  // Date.now() at its message
};
type KeptLook = {
  row: string;
  kind: LookKind;
  form: number;
  seconds: number;
  facing: number;
  seq: number;
  at: number;       // its message's arrival: a timed one replays what is left from here
};
type LastAnimation = {
  event: string;    // "" when the entry holds graph variables alone
  seq: number;
  cell: number;     // its parent cell at the keep: a release of that cell drops it
  shown: boolean;   // played on the 3D now loaded; false again once that 3D goes
  tries: number;    // re-applies on this 3D the graph refused
  lookAt: number;   // the next look, at the earliest (0: every update)
  vars: Map<string, KeptVar>;  // the graph variables by lower-case name, at most GRAPH_VARS_MAX
  varsShown: boolean;          // all of them set on the 3D now loaded
  looks: Map<string, KeptLook>;  // the relayed looks by kind and form (lookKey), at most LOOKS_MAX
  looksShown: boolean;           // all of them played on the 3D now loaded
  actor: boolean;   // an actor's entry: its looks are its simulator's, not its cell's
  flips: number;    // HostService.ownerFlips at the looks' last owner test (pollOne)
  answered?: boolean;  // this seat's own script's play the seam answered at once, shown once on this seat's graph (answeredAtOnce)
};
// the wire's cap on a record's graph variables (cap::GRAPH_VARS, UNMEASURED):
// past it the longest unset name goes
const GRAPH_VARS_MAX = 16;
const varText = (v: KeptVar): string => (typeof v.value === 'number' ? String(Math.round(v.value * 1000) / 1000) : String(v.value));
// the looks kept per subject (a decay's flat shader and its transition, v4's
// fade-in, the eyes' effect and texture fit), past it the longest unplayed
// goes; the subjects holding looks, past it the longest unplayed subject's
// looks go, so the looks never take more than half of KEPT_MAX from the world
// objects' animations (consistency bounds, ADR-073 decision 5 (b))
const LOOKS_MAX = 8;
const LOOK_SUBJECTS_MAX = 64;
// the looks' counts, said once a window when any moved
const LOOK_SUMMARY_MS = 30_000;
// the outcomes counted, and the kinds whose first line was said at most
// (bounded by construction: the outcomes, the drop reasons and the rows'
// fall-throughs; the bound a guard)
type LookOutcome = 'played' | 'held' | 'replayed' | 'expired' | 'stopped' | 'dropped' | 'through';
const noLookCounts = (): Record<LookOutcome, number> => ({ played: 0, held: 0, replayed: 0, expired: 0, stopped: 0, dropped: 0, through: 0 });
const LOOK_SAID_MAX = 64;
const lookKey = (kind: LookKind, form: number): string => (kind === 'eyes' ? 'eyes' : `${kind}:${form}`);
// a kept look's seconds left at `now`: -1 for none (until its Stop), 0 once
// they ran out
const lookLeft = (look: KeptLook, now: number): number => (look.seconds < 0 ? -1 : Math.max(0, look.seconds - (now - look.at) / 1000));
const lookSecs = (s: number): string => (s < 0 ? 'until a Stop' : `${Math.round(s * 10) / 10} s`);

export class ObjectAnimationService extends ClientListener {
  constructor(
    private sp: Sp,
    private controller: CombinedController,
  ) {
    super();
    // the kept last animations and their re-apply follow the script record's
    // switch (the review of b86884a5, note 4; .claude/rules/native.md's A/B
    // switch): scriptRecord false in skymp5-client-settings.txt keeps nothing
    // and re-applies nothing, as ScriptStateService then applies nothing
    const settings = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
    this.keepEnabled = !settings || settings["scriptRecord"] !== false;
    this.furnitureEnabled = this.startFurniture(settings);
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
    // next update (the review of 30839d4b, F1). ADR-068 rung 3 (the script
    // record): it waits for the cell's gate instead, which opens once the
    // cell's script records are applied (ScriptStateService.afterHosting runs
    // it in that update, or on the next one with no pass), and a pillar whose
    // record was applied there is left alone (onCellHosted); F6 deletes the
    // re-sync once x-puzzle-handoff v3 and b-script-state-restart read it idle
    this.controller.emitter.on('cellHostChanged', (e) => {
      if (e.hosted) {
        this.controller.lookupListener(ScriptStateService).afterHosting(e.cell, () => this.onCellHosted(e.cell));
      } else {
        this.dropCell(e.cell);
      }
    });
    // a new connection (connectionAccepted, ahead of all its messages): the
    // server's seqs and subscriptions are the connection's (SpSnippetService's
    // seqs), so the kept last animations start over, as ScriptStateService's
    // tables do
    this.controller.emitter.on('connectionAccepted', () => {
      this.lastAnimations.clear();
      this.subscribed.clear();
      this.subscribedIdx.clear();
      // the looks' first lines and their counts are each connection's
      this.lookSaid.clear();
      this.lookCounts = noLookCounts();
    });
    // the server's unsubscribe (DestroyActor, the create word's emitter idx,
    // PartOne.cpp's onSubscribe and onUnsubscribe) ends the reference's
    // subscription here and takes its kept animation with it: a re-entry's 3D
    // can load before the next create word (the lab's teleport is the
    // client's), and a word kept from before the seat left must not replay
    // over what changed while it was away (the review of b86884a5, note 1).
    // An actor's kept looks go the same way: the subject left the view, and
    // a Stop sent while it was away never came. A message handler: JS state
    // alone
    this.controller.emitter.on('destroyActorMessage', (e) => {
      const localId = this.subscribedIdx.get(e.message.idx);
      if (localId === undefined) {
        return;
      }
      this.subscribedIdx.delete(e.message.idx);
      this.subscribed.delete(localId);
      this.forgetEntry(localId, 'left the view');
    });
    for (const pattern of EVENT_PATTERNS) {
      const handler = {
        enter: () => { },
        leave: (ctx: { selfId: number; animEventName: string; animationSucceeded: boolean }) => {
          // the engine's return flag (NotifyAnimationGraph's) is kept on the
          // note: a pillar's Trigger0N is reported whatever it says (below),
          // every other event only when it took
          this.noted.push({ selfId: ctx.selfId, event: ctx.animEventName, took: ctx.animationSucceeded, furniture: false });
        },
      };
      // two native ranges around the player's id
      this.sp.hooks.sendAnimationEvent.add(handler, 1, PLAYER_ID - 1, pattern);
      this.sp.hooks.sendAnimationEvent.add(handler, PLAYER_ID + 1, LAST_GAME_FILE_ID, pattern);
    }
    this.controller.on('update', () => this.onUpdate());
    this.controller.on('update', () => this.pollLastAnimations());
    this.controller.on('update', () => this.retryHeldPlays());
  }

  // a recorded lastAnimation arriving with a game-file reference's CreateActor
  // is about to be replayed by remoteServer on this seat: not ours to report
  private onCreateActor(e: ConnectionMessage<CreateActorMessage>) {
    const msg = e.message;
    const event = msg.props?.lastAnimation;
    if (typeof event === 'string' && msg.refrId !== undefined && msg.refrId < 0xff000000) {
      this.noteRecord(remoteIdToLocalId(msg.refrId), event);
    }
    // the create word starts this seat's subscription to the reference over:
    // what was kept for it before (from relays it may have missed since) goes,
    // and the record served right behind the word, or the next relay, keeps
    // its last animation again (rung 3). The word also names a reference the
    // server holds (PartOne's onSubscribe sends one for every emitter): a
    // relay is kept for such a reference alone, as the server keeps a
    // lastAnimation (a placement it holds no reference for, the hall door's
    // FX, is relayed to its cell's loaders with nothing kept, a one-shot)
    if (!msg.isMe && msg.refrId !== undefined && msg.refrId < 0xff000000) {
      const localId = remoteIdToLocalId(msg.refrId);
      this.forgetEntry(localId, 'a new create word');
      if (this.subscribed.size >= SUBSCRIBED_MAX) {
        this.subscribed.clear();
        this.subscribedIdx.clear();
      }
      this.subscribed.add(localId);
      this.subscribedIdx.set(msg.idx, localId);
      // thuum #52: the looks the server kept for the actor ride its create
      // word (wire-rules actor_looks), so a copy whose subscription began
      // after its simulator's calls draws them; each taken as the relay it
      // stands for, on the next update (look needs the game), a timed one
      // for the seconds it has left from here
      const served = msg.props?.looks;
      const subject = msg.refrId;
      if (served !== undefined && served.length > 0) {
        const arrived = Date.now();
        this.controller.once('update', () => {
          let taken = 0;
          for (const l of served) {
            const rule = LOOK_KEEP_CODES[l.keep];
            if (rule === undefined || rule.stop) {
              continue;
            }
            const row = rule.kind === 'shader' ? 'EffectShader.Play' : rule.kind === 'effect' ? 'VisualEffect.Play' : 'Actor.SetEyeTexture';
            if (this.look({ row, kind: rule.kind, stop: false, subject, form: l.form, seconds: l.seconds, facing: l.facing, seq: l.seq, arrived })) {
              ++taken;
            }
          }
          // one line a create word that carries looks (rare: a kept actor's
          // return), the probe's read of the serve
          logLab('verb:scripts', `look ${localId.toString(16)}: ${served.length} served with its create word, ${taken} taken (#52)`);
        });
      }
    }
    // no self-played mark here (the review of 066cce4e, finding 1): the replay this message announces comes two seconds or more
    // later and a hosting seat never replays, so a mark set now could be used up by the seat's own genuine event (its
    // OnCellLoad jump) and drop it; playRelayed marks each play at play time
  }

  // the marks key on the event's lower case: the engine's strings are case-blind, so a seat may intern another casing than the
  // relay's (the review of 066cce4e, finding 6)
  private markSelfPlayed(localId: number, event: string) {
    this.selfPlayed.set(`${localId}:${event.toLowerCase()}`, Date.now() + SELF_PLAYED_MS);
  }

  private wasSelfPlayed(localId: number, event: string): boolean {
    const key = `${localId}:${event.toLowerCase()}`;
    const until = this.selfPlayed.get(key);
    if (until === undefined) {
      return false;
    }
    this.selfPlayed.delete(key);
    return Date.now() <= until;
  }

  // ADR-068 (rung 2, the one seam): a script's animation of a world object
  // this seat owns is reported at the boundary now, as ScriptCall
  // (ScriptCallService, with this path's "reported" line): the platform's two
  // animation hooks return early inside a tabled action native's extent (the
  // Call hook's "inside a script action" depth), as they do inside the
  // platform's own call, so that note never reaches the handlers above. What
  // still reaches this path and goes out as ObjectAnimation: an event the
  // engine drives itself (no script native on the stack), an untabled
  // native's (no row in docs/NATIVES.md), and a script's call the platform
  // judged not this seat's while HostService already hosts the cell (the
  // update before SetCellHosted lands). H-D1 counts what is left; rung 7 retires this path if it is none
  private onUpdate() {
    if (this.noted.length === 0) {
      return;
    }
    const noted = this.noted;
    this.noted = [];
    const host = this.controller.lookupListener(HostService);
    const now = Date.now();
    for (const { selfId, event, took, furniture } of noted) {
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
      const pillarTrigger = /^Trigger0[1-3]$/i.test(event);
      if (!took && !pillarTrigger) {
        continue;
      }
      // every genuine hook event on a pillar is the local script's own act
      // (a TypeScript play never re-enters the hook): StartState0N at a cell
      // load puts the script on side N, its own Trigger0N moves it to N+1,
      // so the LAST side-naming event is the script's side, which a seat
      // that hosted, turned and walked out keeps in its Papyrus state (the
      // review of 30839d4b, F3); a furniture's use event is no script's act
      if (!furniture) {
        this.noteScriptSide(selfId, event);
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
      const kind = refr.getBaseObject()?.getType();
      if (kind === FormType.Door) {
        continue;
      }
      // a furniture's event is its user's (the header's furniture paragraph):
      // this seat's use, else its cell's host's (an NPC's use)
      let owner = "";
      if (this.furnitureEnabled && kind === FormType.Furniture) {
        if (this.usedHere(selfId, now)) {
          owner = " (furniture, this seat's use)";
        } else if (host.isHostedHere(refr)) {
          owner = " (furniture, cell hosted here)";
        } else {
          logTrace(this, `object animation ${selfId.toString(16)} ${event} dropped: furniture not used or hosted here`);
          continue;
        }
      } else if (!host.isHostedHere(refr)) {
        continue;
      }
      this.controller.emitter.emit('sendMessage', {
        message: { t: MsgType.ObjectAnimation, refr: localIdToRemoteId(selfId), event },
        reliability: 'reliable',
      });
      logTrace(this, `object animation ${selfId.toString(16)} ${event} reported${took ? "" : " (the graph's flag said refused)"}${owner}`);
    }
  }

  // the furniture path's switch (.claude/rules/native.md's A/B): the client
  // setting furnitureAnimation, on unless false (scriptRecord's read), told to
  // the platform through setFurnitureAnimation (off by default there, so an
  // older client keeps the old path); on only when the platform takes it and
  // raises furnitureAnimation, said once otherwise
  private startFurniture(settings: Record<string, unknown> | undefined): boolean {
    if (settings && settings["furnitureAnimation"] === false) {
      logTrace(this, "furnitureAnimation is off in settings: a furniture's events stay its cell host's to report");
      try {
        this.sp.setFurnitureAnimation(false);
      } catch (e) {
        // an older platform: it has no furniture branch to turn off
      }
      return false;
    }
    try {
      this.controller.on('furnitureAnimation', (e) => this.onFurnitureAnimation(e));
    } catch (e) {
      logLab('verb:furniture', `furnitureAnimation unavailable on this platform (${e}); the old path stands`);
      return false;
    }
    try {
      if (this.sp.setFurnitureAnimation(true) === false) {
        logError(this, "setFurnitureAnimation(true) refused; the old path stands");
        return false;
      }
    } catch (e) {
      logLab('verb:furniture', `setFurnitureAnimation unavailable on this platform (${e}); the old path stands`);
      return false;
    }
    this.controller.emitter.on('openContainerMessage', (e) => this.noteUse(e.message.target));
    this.controller.emitter.on('sendMessage', (e) => this.noteClose(e.message));
    return true;
  }

  // the platform's furnitureAnimation (a tick task at the hook's leave; no
  // game call in its handler, JS state alone): noted for the update with the
  // furniture mark, as the hook's events are
  private onFurnitureAnimation(e: FurnitureAnimationEvent) {
    if (!this.furnitureEnabled) {
      return;
    }
    const { refrId, event } = e;
    if (typeof refrId !== 'number' || refrId <= 0 || refrId === PLAYER_ID || refrId > LAST_GAME_FILE_ID || typeof event !== 'string' || event === '') {
      return;
    }
    this.noted.push({ selfId: refrId, event, took: e.took === true, furniture: true });
  }

  // this seat's use of a furniture: the server's OpenContainer names it to the
  // one user it admitted, ahead of remoteServer's activate(player, true), the
  // call that runs the engine's furniture code (lever-furn-static.md step 4),
  // so the mark is up before the event it lets through. A message handler: JS
  // state alone, the target by id (a container's or a body's mark is never
  // asked: only a FURN's graph raises a furniture event)
  private noteUse(remoteTarget: number) {
    const localId = remoteIdToLocalId(remoteTarget);
    if (localId === 0) {
      return;
    }
    if (this.uses.size >= USES_MAX && !this.uses.has(localId)) {
      this.uses.clear();
    }
    this.uses.set(localId, Date.now() + USE_MS);
  }

  // the use ends at this seat's closing activation of it (remoteServer's
  // second Activate, isSecondActivation true). JS state alone
  private noteClose(message: AnyMessage) {
    if (message.t !== MsgType.Activate) {
      return;
    }
    const data = (message as ActivateMessage).data;
    if (data && data.isSecondActivation === true && data.caster === PLAYER_ID) {
      this.uses.delete(remoteIdToLocalId(data.target));
    }
  }

  // whether the furniture's use is this seat's now: its use mark, or the
  // player's own furniture (a seat held longer than USE_MS). The update's
  // context
  private usedHere(localId: number, now: number): boolean {
    const until = this.uses.get(localId);
    if (until !== undefined) {
      if (now <= until) {
        return true;
      }
      this.uses.delete(localId);
    }
    const seat = this.sp.Game.getPlayer()?.getFurnitureReference();
    return !!seat && seat.getFormID() === localId;
  }

  private onRelayed(e: ConnectionMessage<ObjectAnimationMessage>) {
    const { refr, event } = e.message;
    this.controller.once('update', () => this.play(refr, event));
  }

  // also SpSnippetService's, for a relayed script call's PlayAnimation on a
  // game-file reference (ADR-068 rung 2): the same play, mark and line as a
  // relayed ObjectAnimation, with the call's seq for the kept last animation
  // (rung 3) where a create word named the reference; a relayed
  // ObjectAnimation carries none and takes the newest seq known here
  play(remoteRefr: number, event: string, seq?: number) {
    const localId = remoteIdToLocalId(remoteRefr);
    const refr = ObjectReference.from(this.sp.Game.getFormEx(localId));
    if (!refr) {
      return;
    }
    const keepSeq = this.subscribed.has(localId) ? (seq ?? this.newestSeq(localId)) : undefined;
    const pillar = this.isPillar(refr);
    if (pillar) {
      // a later relay of the pillar supersedes a refused turn still queued
      this.pillarRelays.set(localId, (this.pillarRelays.get(localId) ?? 0) + 1);
    }
    // a later relay of the reference supersedes a play held for it
    this.heldPlays.drop(localId);
    const played = this.playRelayed(refr, event, "relayed", remoteRefr, true, keepSeq);
    const side = pillar && /^Trigger0[1-3]$/i.test(event) ? sideOfRecord(event) : undefined;
    if (!played && side !== undefined && this.controller.lookupListener(HostService).epochForActor(refr) === 0) {
      void this.settleRefusedTurn(localId, remoteRefr, event, side);
    }
    // a play the copy refused waits until its graph takes it (heldPlays.ts):
    // a pillar's refused turn has its own settle above, and a play kept for a
    // 3D not loaded yet is played at that load by the keep's poll
    const keptForLoad = keepSeq !== undefined && this.keepEnabled && !refr.is3DLoaded() && this.keepsFor(refr, localId);
    if (!played && !pillar && !keptForLoad) {
      this.heldPlays.hold(localId, remoteRefr, event, Date.now());
    }
  }

  // a graph state restored for the reference stands for every relay at or
  // below its seq, so a play held for the copy's graph goes; true when one did
  dropHeldPlay(localId: number): boolean {
    const had = this.heldPlays.has(localId);
    this.heldPlays.drop(localId);
    return had;
  }

  // the held plays, each update: one whose copy's 3D is loaded is played
  // again, and said once when its graph takes it or at the bound. A play
  // taken here is no record of its own: its relay's keep already holds it.
  // The update's context
  private retryHeldPlays(): void {
    if (this.heldPlays.size === 0) {
      return;
    }
    const now = Date.now();
    const { retry, expired } = this.heldPlays.due(now);
    for (const play of expired) {
      logLab('verb:scripts', `object animation ${play.remote.toString(16)} ${play.event} dropped: its copy's graph refused it for ${HELD_PLAY_MS} ms (relayed)`);
    }
    for (const play of retry) {
      const refr = ObjectReference.from(this.sp.Game.getFormEx(play.id));
      if (!refr) {
        this.heldPlays.drop(play.id);
        logLab('verb:scripts', `object animation ${play.remote.toString(16)} ${play.event} dropped: its reference is gone (relayed)`);
        continue;
      }
      if (!refr.is3DLoaded()) {
        continue;
      }
      const { played, jump } = this.playOn(refr, play.id, play.event, true);
      if (!played) {
        continue;
      }
      const taken = this.heldPlays.taken(play.id, now);
      logLab('verb:scripts', `object animation ${play.remote.toString(16)} ${play.event} taken after ${taken ? taken.heldMs : 0} ms held: its copy's graph refused it first (relayed${jump})`);
    }
  }

  // Train 53, R-C4 (playtest 52's Temple pillar, lab/.cache/drafts/reads-53/
  // r-c1-c2-c4-claw.md): a relayed turn the guest's graph refuses was
  // dropped, and nothing corrected the side after it; Trigger03 was taken at
  // 20:48:41.803 and Trigger01 refused 2.54 s later on a guest about 8,300
  // units from the pillar (HYPOTHESIS: the engine does not advance a far
  // reference's graph, so that turn never reached side 1). So a refused turn
  // is queued once, PILLAR_TURN_S on (a nearby graph still in its last turn
  // takes it then and shows the animation), and if the graph refuses it
  // again the pillar jumps to the side the server recorded (sideOfRecord):
  // StartState0N for sides 2 and 3, and for side 1, which has no jump,
  // StartState03 then Trigger03 half a second apart (placeOnSide's and
  // resync's route). That route is the turn playOn already tried twice, a
  // half second between, so side 1's line says "turned toward", never
  // "jumped" (train 53's review, correctness-9; HYPOTHESIS, R-C4's far
  // graph: it stalls until the guest comes near, then animates the stale
  // turn). A guest's alone (a seat that hosts the cell plays its
  // script's graph, playtest 46), stopped by a later relay of the pillar, by
  // the pillar's loss or by a grant of its cell here, each said; the pillar
  // is taken again by id after every wait (a wrapper lives one Papyrus
  // update, resync's note). Like resync's plays, the jump marks nothing
  // self-played (F6). Its line: "[verb:traps] pillar <refr> relayed <event>
  // refused: ..." with each play's answer. HYPOTHESIS until the lab: a far
  // graph takes the jump, and lands on the side once the guest comes near
  private async settleRefusedTurn(id: number, remoteRefr: number, event: string, side: number): Promise<void> {
    const host = this.controller.lookupListener(HostService);
    const relay = this.pillarRelays.get(id) ?? 0;
    const head = `pillar ${remoteRefr.toString(16)} relayed ${event} refused`;
    const reason = (refr: ObjectReference | null): string => {
      if (!refr) {
        return 'the pillar is gone';
      }
      if ((this.pillarRelays.get(id) ?? 0) !== relay) {
        return 'a later relay of it came first';
      }
      return host.epochForActor(refr) !== 0 ? 'this seat hosts its cell now' : '';
    };
    let step = 'the queued turn';
    try {
      await Utility.wait(PILLAR_TURN_S);
      let refr = ObjectReference.from(this.sp.Game.getFormEx(id));
      let why = reason(refr);
      if (!refr || why) {
        logLab('verb:traps', `${head}: its queued turn not played, ${why}`);
        return;
      }
      const queued = this.playOn(refr, id, event, true);
      if (queued.played) {
        logLab('verb:traps', `${head}: taken at its queued turn ${PILLAR_TURN_S} s on${queued.jump}`);
        return;
      }
      let jumps: string;
      if (side === 1) {
        step = 'StartState03';
        const first = refr.playAnimation('StartState03');
        await Utility.wait(0.5);
        refr = ObjectReference.from(this.sp.Game.getFormEx(id));
        why = reason(refr);
        if (!refr || why) {
          logLab('verb:traps', `${head}, and again at its queued turn${queued.jump}: not on the recorded side 1 (StartState03 ${first ? 'taken' : 'refused'}), then ${why}`);
          return;
        }
        step = 'Trigger03';
        const second = refr.playAnimation('Trigger03');
        jumps = `StartState03 ${first ? 'taken' : 'refused'}, Trigger03 ${second ? 'taken' : 'refused'}`;
      } else {
        step = `StartState0${side}`;
        jumps = `StartState0${side} ${refr.playAnimation(`StartState0${side}`) ? 'taken' : 'refused'}`;
      }
      const landed = side === 1
        ? `turned toward the recorded side 1 (no jump reaches side 1: ${jumps})`
        : `jumped to the recorded side ${side} (${jumps})`;
      logLab('verb:traps', `${head}, and again at its queued turn${queued.jump}: ${landed}`);
    } catch (e) {
      logError(this, `${head}: the jump to the recorded side ${side} failed at ${step}: ${e}`);
    }
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
  // A seq (a relay's, the record's view's) keeps the play as the object's
  // last animation (rung 3, keep); remoteServer's replay of a create word's
  // lastAnimation passes none: it stands only where no record carries a view
  playRelayed(refr: ObjectReference, event: string, how: string, remoteRefr?: number, jumpFirst = true, seq?: number): boolean {
    const localId = refr.getFormID();
    const id = (remoteRefr ?? localIdToRemoteId(localId)).toString(16);
    this.noteRecord(localId, event);
    const { played, jump } = this.playOn(refr, localId, event, jumpFirst);
    // "played" is the live relay's word (b-puzzle-relay's claim on a guest);
    // the record's replay says "replayed", which that claim's " played" never
    // matches, so a host's refused replays do not count as plays
    logTrace(this, `object animation ${id} ${event} ${jumpFirst ? "played" : "replayed"} (${how}, ${played ? "taken" : "refused"}${jump})`);
    if (seq !== undefined) {
      // a play on a reference with no 3D does nothing (placeOnSide's note):
      // shown only when its 3D is here, else played at its load
      this.keep(refr, localId, event, seq, refr.is3DLoaded());
    }
    return played;
  }

  // the play itself, playRelayed's and the re-apply's: a pillar's Trigger02
  // or Trigger03 after the jump to its side (StartState0N), each event marked
  // self-played
  private playOn(refr: ObjectReference, localId: number, event: string, jumpFirst: boolean): { played: boolean; jump: string } {
    const m = jumpFirst ? /^Trigger0([23])$/i.exec(event) : null;
    let jump = "";
    if (m) {
      const jumpEvent = `StartState0${m[1]}`;
      this.markSelfPlayed(localId, jumpEvent);
      const taken = refr.playAnimation(jumpEvent);
      jump = `, after ${jumpEvent} ${taken ? "taken" : "refused"}`;
    }
    this.markSelfPlayed(localId, event);
    return { played: refr.playAnimation(event), jump };
  }

  // the server's last event for a reference as this seat knows it
  // (CreateActor's lastAnimation, then every relayed or replayed event), the
  // grant's re-sync's (d1f23444, which F6 deletes); a 3D load's re-apply plays
  // the kept last animation (keep) instead
  private noteRecord(localId: number, event: string) {
    this.recordEvent.set(localId, event);
    this.notePillar(localId, event);
  }

  // ADR-068 (rung 2): a script's animation of a world object this seat owns
  // is reported at the boundary, and the seam silences the animation hooks
  // for the call's extent, so the host's own pillar turns and its OnCellLoad
  // jump never reach onUpdate; ScriptCallService feeds each such call here
  // instead, so the hand-off re-sync still reads the script's last
  // side-naming act (until rung 3's record retires the re-sync). JS state
  // alone: safe in the scriptCalls event's context, where no native runs
  noteScriptSide(localId: number, event: string) {
    this.scriptActs.set(localId, (this.scriptActs.get(localId) ?? 0) + 1);
    // the script's newer act stands on the graph now: an earlier one answered
    // with no 3D is no longer the one to show
    const kept = this.lastAnimations.get(localId);
    if (kept !== undefined) {
      kept.answered = false;
    }
    this.notePillar(localId, event);
    const side = sideOfRecord(event);
    if (side !== undefined) {
      this.scriptSide.set(localId, side);
    }
  }

  private notePillar(localId: number, event: string) {
    if (/^(Trigger|StartState)0[1-3]$/i.test(event)) {
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
    const scripts = this.controller.lookupListener(ScriptStateService);
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
      // the record put the script on its owner's state and the graph on the
      // record's view before the gate opened: nothing to feed
      if (scripts.appliedAtGrant(id)) {
        logLab('verb:traps', `pillar ${id.toString(16)} at the grant of ${cell.toString(16)}: no re-sync, its script record was applied there`);
        return;
      }
      // a record applied there whose view could not play (no 3D before the
      // gate opened): its script stands on the view's side already, so the
      // graph alone goes there once its 3D comes and no turn is fed (fed turns
      // would carry the script past its record); a view naming no side leaves
      // the pillar be
      const unplayed = scripts.unplayedViewAtGrant(id);
      if (unplayed !== undefined) {
        const side = sideOfRecord(unplayed);
        if (side === undefined) {
          logLab('verb:traps', `pillar ${id.toString(16)} at the grant of ${cell.toString(16)}: no re-sync, its script record's view ${unplayed} names no side`);
          return;
        }
        void this.placeOnSide(id, cell, side);
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
  // hosts the cell (F5). A form's wrapper lives one Papyrus update: the
  // platform's object pool forgets every wrapper at the next update
  // (Sp3NativeValueCasts.cpp, NativeObjectToJsObject's ForgetAll), and a
  // wrapper kept across a wait throws "Invalid _skyrimPlatform_indexInPool"
  // at its next native (x-puzzle-handoff-late v2 20261105-060929: the
  // Trigger03 after the reset's first wait), so after every wait the pillar
  // and the player are taken again by id (Skyrim Platform's docs/native.md:
  // "Form references may be already lost"; remoteServer's replay does the
  // same), and the error line names the step before the platform's text,
  // which the console cuts
  private async resync(refr: ObjectReference, id: number, cell: number, initial: number, turns: number, record: string, target: number): Promise<void> {
    const host = this.controller.lookupListener(HostService);
    const pillarAgain = (): ObjectReference | null => ObjectReference.from(this.sp.Game.getFormEx(id));
    let step = `StartState0${initial === 1 ? 3 : initial}`;
    try {
      this.resyncing.add(id);
      if (initial === 1) {
        refr.playAnimation('StartState03');
        await Utility.wait(0.5);
        if (!host.hostsCell(cell)) {
          return;
        }
        step = 'Trigger03';
        const again = pillarAgain();
        if (!again) {
          logLab('verb:traps', `pillar ${id.toString(16)}: the re-sync at the grant of ${cell.toString(16)} stopped, the pillar is gone (the reset's Trigger03)`);
          return;
        }
        if (again.playAnimation('Trigger03')) {
          await Utility.wait(PILLAR_TURN_S);
        }
      } else {
        refr.playAnimation(`StartState0${initial}`);
        await Utility.wait(0.5);
      }
      for (let i = 0; i < turns; i++) {
        if (!host.hostsCell(cell)) {
          logLab('verb:traps', `pillar ${id.toString(16)}: the re-sync at the grant of ${cell.toString(16)} stopped, this seat no longer hosts the cell (${i} of ${turns} turn(s) fed)`);
          return;
        }
        step = `turn ${i + 1} of ${turns}`;
        const again = pillarAgain();
        const player = this.sp.Game.getPlayer();
        if (!again || !player) {
          logLab('verb:traps', `pillar ${id.toString(16)}: the re-sync at the grant of ${cell.toString(16)} stopped, the ${again ? 'player' : 'pillar'} is gone (${i} of ${turns} turn(s) fed)`);
          return;
        }
        again.activate(player, false);
        await Utility.wait(PILLAR_TURN_S + 0.5);
      }
      logLab('verb:traps', `pillar ${id.toString(16)} re-synced at the grant of ${cell.toString(16)}: the script's side ${initial}, the record ${record} (side ${target}), ${turns} turn(s) fed to the script`);
    } catch (e) {
      logError(this, `pillar ${id.toString(16)} re-sync at the grant of ${cell.toString(16)} failed at ${step}: ${e}`);
    } finally {
      this.resyncing.delete(id);
    }
  }

  // rung 3 (the script record): a record applied at the grant without its
  // view puts the graph on the record's side once the pillar's 3D comes (a
  // play on a reference with no 3D does nothing: CommonLibSSE-NG
  // src/RE/T/TESObjectREFR.cpp:814-817, PlayAnimation returns when Get3D is
  // null; the rung's delta review, N1), with the re-sync's own jump
  // (StartState0N; for Side01, which has no jump, StartState03 then Trigger03)
  // and no turn fed: its script stands on that side already. It stops when
  // this seat no longer hosts the cell, when the pillar's own script acted on
  // it first (the graph is the script's then), or after GRAPH_3D_WAIT_S with
  // no 3D; its line names each jump's answer and is not the re-sync's
  // "re-synced" (that path is d1f23444's, which F6 deletes). The pillar is
  // taken again by id after every wait (a wrapper lives one Papyrus update)
  private async placeOnSide(id: number, cell: number, side: number): Promise<void> {
    const host = this.controller.lookupListener(HostService);
    const head = `pillar ${id.toString(16)} at the grant of ${cell.toString(16)}`;
    const acts = this.scriptActs.get(id) ?? 0;
    let step = 'the wait for its 3D';
    try {
      this.resyncing.add(id);
      let refr = ObjectReference.from(this.sp.Game.getFormEx(id));
      for (let waited = 0; ; waited += GRAPH_WAIT_STEP_S) {
        if (!refr || !host.hostsCell(cell)) {
          logLab('verb:traps', `${head}: graph not on the record's side ${side}, ${refr ? 'this seat no longer hosts the cell' : 'the pillar is gone'}`);
          return;
        }
        if ((this.scriptActs.get(id) ?? 0) !== acts) {
          logLab('verb:traps', `${head}: graph not on the record's side ${side}, its own script acted on it first`);
          return;
        }
        if (refr.is3DLoaded()) {
          break;
        }
        if (waited >= GRAPH_3D_WAIT_S) {
          logLab('verb:traps', `${head}: graph not on the record's side ${side}, no 3D in ${GRAPH_3D_WAIT_S} s`);
          return;
        }
        await Utility.wait(GRAPH_WAIT_STEP_S);
        refr = ObjectReference.from(this.sp.Game.getFormEx(id));
      }
      let jumps: string;
      if (side === 1) {
        step = 'StartState03';
        const first = refr.playAnimation('StartState03');
        await Utility.wait(0.5);
        const again = ObjectReference.from(this.sp.Game.getFormEx(id));
        if (!again || !host.hostsCell(cell)) {
          logLab('verb:traps', `${head}: graph not on the record's side 1 (StartState03 ${first ? 'taken' : 'refused'}), then ${again ? 'unhosted' : 'the pillar gone'}`);
          return;
        }
        step = 'Trigger03';
        const second = again.playAnimation('Trigger03');
        jumps = first === second ? `StartState03, Trigger03 ${first ? 'taken' : 'refused'}` : `StartState03 ${first ? 'taken' : 'refused'}, Trigger03 ${second ? 'taken' : 'refused'}`;
      } else {
        step = `StartState0${side}`;
        jumps = `StartState0${side} ${refr.playAnimation(`StartState0${side}`) ? 'taken' : 'refused'}`;
      }
      logLab('verb:traps', `${head}: graph on the record's side ${side} (${jumps}), no turn fed`);
    } catch (e) {
      logError(this, `${head}: the record's side failed at ${step}: ${e}`);
    } finally {
      this.resyncing.delete(id);
    }
  }

  // an owned latent action this seat's own script ran that the seam answered
  // at once (the platform's `answered`): the reference's 3D was not loaded
  // here, or another player's press started the script, and this seat's
  // engine advances that graph only near its own player. The script stands
  // past the play while the graph never took it, and the pillar's own
  // OnCellLoad turns the graph only once, from its first side. The play is
  // kept as a relay's is and shown once by the 3D poll though this seat hosts
  // the cell (reapplyGate), the side's jump first, so the graph stands where
  // the script does: at once while the 3D is here (the graph takes it as it
  // advances), else at the next 3D load. Called after noteScriptSide for the
  // same call. JS state alone: safe in the scriptCalls event's context, where
  // no native runs; the keep reads the reference, so it waits for the next
  // update, and a newer act of the script's on it before then stands instead
  answeredAtOnce(localId: number, event: string): void {
    const acts = this.scriptActs.get(localId) ?? 0;
    this.controller.once('update', () => {
      if ((this.scriptActs.get(localId) ?? 0) !== acts) {
        return;
      }
      try {
        this.keepAnswered(localId, event);
      } catch (e) {
        logError(this, `object animation ${localId.toString(16)} ${event}: its answered play's keep threw: ${e}`);
      }
    });
  }

  // answeredAtOnce's keep, in an update's context
  private keepAnswered(localId: number, event: string): void {
    const refr = ObjectReference.from(this.sp.Game.getFormEx(localId));
    if (!refr) {
      return;
    }
    this.keep(refr, localId, event, this.newestSeq(localId), false);
    const kept = this.lastAnimations.get(localId);
    if (kept === undefined || kept.event !== event) {
      return;
    }
    kept.answered = true;
    logLab('verb:scripts', `object animation ${localId.toString(16)} ${event}: this seat's script played it, answered at once, to be shown on this seat's graph`);
  }

  // rung 3: the object's last animation kept, the record's view (keepView, at
  // its apply) or a relay (playRelayed with the relay's seq), whichever has
  // the higher seq; at an equal seq the later word stands, as a guest applies
  // a record whose seq is not below its own. An update's context
  private keep(refr: ObjectReference, localId: number, event: string, seq: number, shown: boolean): void {
    if (!this.keepEnabled) {
      return;
    }
    const old = this.lastAnimations.get(localId);
    if (old !== undefined && seq < old.seq) {
      return;
    }
    if (!this.keepsFor(refr, localId)) {
      return;
    }
    const parent = refr.getParentCell();
    // Map iteration is insertion order: a keep moves the entry last, so the
    // longest unkept goes past KEPT_MAX
    this.lastAnimations.delete(localId);
    // D-C3 A: the graph variables kept for the object stay with it, and so
    // do its looks
    this.lastAnimations.set(localId, {
      event, seq, cell: parent ? parent.getFormID() : 0, shown, tries: 0, lookAt: 0,
      vars: old !== undefined ? old.vars : new Map<string, KeptVar>(), varsShown: old !== undefined ? old.varsShown : true,
      looks: old !== undefined ? old.looks : new Map<string, KeptLook>(), looksShown: old !== undefined ? old.looksShown : true,
      actor: false, flips: old !== undefined ? old.flips : 0,
    });
    this.capKept();
  }

  // the kept set's bound: Map iteration is insertion order, so the longest
  // unkept goes past KEPT_MAX
  private capKept(): void {
    if (this.lastAnimations.size > KEPT_MAX) {
      const oldest = this.lastAnimations.keys().next().value;
      if (oldest !== undefined) {
        this.forgetEntry(oldest, 'past the cap');
      }
    }
  }

  // an entry gone (its subject's unsubscribe, a new create word, the cap),
  // its looks counted as dropped for the reason. JS state alone
  private forgetEntry(localId: number, why: string): void {
    const kept = this.lastAnimations.get(localId);
    if (kept === undefined) {
      return;
    }
    this.lastAnimations.delete(localId);
    kept.looks.forEach((look) => this.sayLookDropped(localId, look, why));
  }

  // D-C3 A: one graph variable kept for the object, the newest seq per name;
  // `set` whether it stands on the 3D now loaded (else the next poll that
  // finds the 3D here sets it). An update's context
  private keepVariable(refr: ObjectReference, localId: number, v: KeptVar, set: boolean): void {
    if (!this.keepEnabled || !this.keepsFor(refr, localId)) {
      return;
    }
    const key = v.name.toLowerCase();
    let kept = this.lastAnimations.get(localId);
    const old = kept?.vars.get(key);
    if (old !== undefined && v.seq < old.seq) {
      return;
    }
    if (kept === undefined) {
      const parent = refr.getParentCell();
      kept = {
        event: '', seq: 0, cell: parent ? parent.getFormID() : 0, shown: false, tries: 0, lookAt: 0, vars: new Map<string, KeptVar>(), varsShown: true,
        looks: new Map<string, KeptLook>(), looksShown: true, actor: false, flips: 0,
      };
    }
    if (old === undefined && kept.vars.size >= GRAPH_VARS_MAX) {
      const oldest = kept.vars.keys().next().value;
      if (oldest !== undefined) {
        kept.vars.delete(oldest);
      }
    }
    kept.vars.delete(key);
    kept.vars.set(key, v);
    kept.varsShown = kept.varsShown && set;
    this.lastAnimations.delete(localId);
    this.lastAnimations.set(localId, kept);
    this.capKept();
  }

  // one variable on the reference's graph through the platform's own calls
  // (the platform's mark, PlatformCall.h: the seam tallies them "(platform)"
  // and batches none, so nothing set here is reported back)
  private setOne(refr: ObjectReference, v: KeptVar): void {
    if (v.k === 5) {
      refr.setAnimationVariableBool(v.name, v.value === true);
    } else if (v.k === 3) {
      refr.setAnimationVariableInt(v.name, Number(v.value));
    } else {
      refr.setAnimationVariableFloat(v.name, Number(v.value));
    }
  }

  // every kept variable the 3D now loaded has not had, set; how many. Also
  // ScriptStateService's playView, right before a record's view plays on a 3D
  // that came after the apply (the variables go first, as at a 3D load)
  showVariables(refr: ObjectReference): number {
    const kept = this.lastAnimations.get(refr.getFormID());
    if (kept === undefined || kept.varsShown || kept.vars.size === 0 || !refr.is3DLoaded()) {
      return 0;
    }
    kept.varsShown = true;
    kept.vars.forEach((v) => this.setOne(refr, v));
    return kept.vars.size;
  }

  // SpSnippetService's, for a relayed script call's SetAnimationVariableBool,
  // Int or Float with a seq on a game-file reference (ADR-068 rung 2): run
  // here as such a relay always ran, then kept with the call's seq where a
  // create word named the reference, as play keeps a relayed animation. An
  // update's context
  setVariable(remoteRefr: number, name: string, k: number, value: number | boolean, seq: number): void {
    const localId = remoteIdToLocalId(remoteRefr);
    const refr = ObjectReference.from(this.sp.Game.getFormEx(localId));
    if (!refr) {
      return;
    }
    const v: KeptVar = { name, k, value, seq };
    const loaded = refr.is3DLoaded();
    this.setOne(refr, v);
    logTrace(this, `graph variable ${remoteRefr.toString(16)} ${name} ${varText(v)} set (relayed, seq ${seq}${loaded ? '' : ', no 3D here'})`);
    if (this.subscribed.has(localId)) {
      this.keepVariable(refr, localId, v, loaded);
    }
  }

  // ScriptStateService's, at a record's apply, before its view plays (D-C3
  // A): each served variable whose seq is above the one kept here for its
  // name (or equal while the 3D now loaded has not had it, the view's
  // "current" rule after playtest 49) is set where the 3D is here and kept;
  // one with no 3D here waits for its load. The counts for the apply's line.
  // An update's context
  applyRecordVariables(refr: ObjectReference, vars: GraphVar[]): { set: number; waiting: number; current: number } {
    const localId = refr.getFormID();
    const loaded = refr.is3DLoaded();
    const out = { set: 0, waiting: 0, current: 0 };
    for (const g of vars) {
      const v: KeptVar = { name: g.name, k: g.k, value: g.k === 5 ? g.b === true : Number(g.k === 3 ? g.i : g.f), seq: g.seq };
      const kept = this.lastAnimations.get(localId);
      const old = kept?.vars.get(g.name.toLowerCase());
      if (kept !== undefined && old !== undefined && (g.seq < old.seq || (g.seq === old.seq && kept.varsShown && loaded))) {
        out.current++;
        continue;
      }
      if (loaded) {
        this.setOne(refr, v);
        out.set++;
      } else {
        out.waiting++;
      }
      this.keepVariable(refr, localId, v, loaded);
    }
    return out;
  }

  // ScriptStateService's, at a record's apply whose view is to play here: the
  // view is kept before it plays, so one that finds no 3D in its window (given
  // up) still plays at the reference's next 3D load. An update's context
  keepView(refr: ObjectReference, view: string, seq: number): void {
    this.keep(refr, refr.getFormID(), view, seq, false);
  }

  // SpSnippetService's, for a relayed look of the 3D keep's rows (the header
  // above LookKind): true when taken here (played, held for 3D, or stopped),
  // false for the snippet to run as it always has, said once per (row, why)
  // and counted: the keep off (scriptRecord false, the kept animations'
  // switch), no reference or no form here, no create word for the subject
  // (a relay is kept for a subscribed reference alone, play's rule), or a
  // subject this seat owns (its own engine draws its looks). A Stop, and
  // SetEyeTexture(None), clears what it names first, whichever way the
  // snippet then goes, so no kept Play outlives its Stop (the review of
  // eaa6ba47, finding 3: a Stop that fell through for its form not loaded
  // yet, a subscription table just cleared or a subject owned here left its
  // Play to the next 3D load). An update's context
  look(l: RelayedLook): boolean {
    const localId = snippetIdToLocalId(l.subject);
    const refr = localId !== 0 ? ObjectReference.from(this.sp.Game.getFormEx(localId)) : null;
    const key = lookKey(l.kind, l.form);
    const kept = this.lastAnimations.get(localId);
    const had = (l.stop || l.form === 0) && kept !== undefined && kept.looks.delete(key);
    if (kept !== undefined && this.holdsNothing(kept)) {
      this.lastAnimations.delete(localId);
    }
    if (!this.keepEnabled || !refr || !this.subscribed.has(localId)) {
      const why = !this.keepEnabled ? 'scriptRecord off' : !refr ? 'no reference here' : 'no create word for it';
      this.lookFallsThrough(l.row, localId || l.subject, why);
      return false;
    }
    const actor = this.sp.Actor.from(refr) !== null;
    if (this.ownsHere(refr, localId, actor)) {
      this.lookFallsThrough(l.row, localId, 'its subject owned here');
      return false;
    }
    // SetEyeTexture(None): what the engine makes of it is unread, so it runs
    // as it always has and the eye texture kept before it goes
    if (l.form === 0) {
      this.lookFallsThrough(l.row, localId, `None${had ? ', its kept texture cleared' : ''}`);
      return false;
    }
    // the form as its kind's type (the review's finding 6): one that is here
    // but no TextureSet, EffectShader or VisualEffect as the row needs runs
    // as it always has, never said "played" and kept
    if (this.lookForm(l.kind, l.form) === null) {
      this.lookFallsThrough(l.row, localId, 'its form not here');
      return false;
    }
    const head = `look ${localId.toString(16)} ${l.row} ${l.form.toString(16)}`;
    if (l.stop) {
      this.stopLook(refr, l.kind, l.form);
      this.sayLook('stopped', 'stopped', () => `${head} stopped (seq ${l.seq}, ${had ? 'its kept Play cleared' : 'nothing kept'})`);
      return true;
    }
    const look: KeptLook = { row: l.row, kind: l.kind, form: l.form, seconds: l.seconds, facing: l.facing, seq: l.seq, at: l.arrived };
    const shown = this.drawable(refr);
    if (shown) {
      this.showLook(refr, look, l.seconds, false);
      this.sayLook('played', 'played', () => `${head} played (seq ${l.seq}, ${lookSecs(l.seconds)})`);
    } else {
      this.sayLook('held', 'held', () => `${head} held for 3D (seq ${l.seq}, ${lookSecs(l.seconds)})`);
    }
    this.keepLook(refr, localId, actor, key, look, shown);
    return true;
  }

  // a relayed look the 3D keep does not take, which runs as it always has:
  // its first line per (row, why) names the list, the row that fell through
  // and one subject, the rest are counted (ADR-073 decision 7). Also
  // SpSnippetService's, for 3df63969's three actor look rows the keep has no
  // kind for (SetAlpha, SetCriticalStage, AttachAshPile: a one-shot each, which
  // a replay at every 3D load would repeat). JS state alone
  lookFallsThrough(row: string, subject: number, why: string): void {
    this.sayLook('through', `through:${row}:${why}`, () => `look ${subject.toString(16)} ${row} falls through the 3D keep (${why}): run as always`);
  }

  // whether this seat owns the look's subject: its own player; an actor this
  // seat simulates (the server's word per actor, HostService.isMineLocal, so
  // a thrall's commander and not its cell's host); any other reference whose
  // cell this seat hosts (epochForActor, pollOne's test). An update's context
  private ownsHere(refr: ObjectReference, localId: number, actor: boolean): boolean {
    if (localId === PLAYER_ID) {
      return true;
    }
    const host = this.controller.lookupListener(HostService);
    return actor ? host.isMineLocal(localId) : host.epochForActor(refr) !== 0;
  }

  // a look draws on a loaded 3D, of a reference neither disabled nor deleted
  // (sync/equipment.ts cannotDress's two: apocrypha's read of 2026-11-14 gives
  // no replay to a copy that cannot dress, and none of a look either)
  private drawable(refr: ObjectReference): boolean {
    return refr.is3DLoaded() && !refr.isDisabled() && !refr.isDeleted();
  }

  // one look kept on its subject's entry (one shape with the graph
  // variables', keepVariable), the newest per key, moved last as keep moves an
  // entry; `shown` whether it plays on the 3D now loaded. An update's context
  private keepLook(refr: ObjectReference, localId: number, actor: boolean, key: string, look: KeptLook, shown: boolean): void {
    if ((this.lastAnimations.get(localId)?.looks.size ?? 0) === 0) {
      this.capLookSubjects();
    }
    let kept = this.lastAnimations.get(localId);
    if (kept === undefined) {
      const parent = refr.getParentCell();
      kept = {
        event: '', seq: 0, cell: parent ? parent.getFormID() : 0, shown: false, tries: 0, lookAt: 0, vars: new Map<string, KeptVar>(), varsShown: true,
        looks: new Map<string, KeptLook>(), looksShown: true, actor, flips: 0,
      };
    }
    if (!kept.looks.has(key) && kept.looks.size >= LOOKS_MAX) {
      const oldest = kept.looks.keys().next().value;
      const gone = oldest !== undefined ? kept.looks.get(oldest) : undefined;
      if (oldest !== undefined && gone !== undefined) {
        kept.looks.delete(oldest);
        this.sayLookDropped(localId, gone, 'past the cap');
      }
    }
    kept.looks.delete(key);
    kept.looks.set(key, look);
    kept.looksShown = kept.looksShown && shown;
    kept.actor = actor;
    // look's owner test just ran
    kept.flips = this.controller.lookupListener(HostService).ownerFlips();
    this.lastAnimations.delete(localId);
    this.lastAnimations.set(localId, kept);
    this.capKept();
  }

  // past LOOK_SUBJECTS_MAX subjects holding looks, the longest unkept one's
  // looks go, its entry with them when it holds nothing else; a walk of the
  // kept set (KEPT_MAX at most) only when a subject takes its first look
  private capLookSubjects(): void {
    const entries = Array.from(this.lastAnimations.entries());
    let holding = 0;
    let oldest = -1;
    for (const [id, entry] of entries) {
      if (entry.looks.size > 0) {
        holding++;
        if (oldest === -1) {
          oldest = id;
        }
      }
    }
    const kept = oldest !== -1 ? this.lastAnimations.get(oldest) : undefined;
    if (holding < LOOK_SUBJECTS_MAX || kept === undefined) {
      return;
    }
    kept.looks.forEach((look) => this.sayLookDropped(oldest, look, 'past the cap'));
    kept.looks.clear();
    if (this.holdsNothing(kept)) {
      this.lastAnimations.delete(oldest);
    }
  }

  // one look on the subject's 3D through the platform's own calls
  // (CallNativeApi's Override: the seam counts them "(platform)" and batches
  // none, so nothing played here is reported back, setOne's rule); false when
  // its form is not here. `again` at a 3D load: a shader or an effect is
  // stopped on the subject first, since whether the unload dropped the
  // running one or only suspended it for the reload is unread
  // (ShaderReferenceEffect overrides ReferenceEffect's Suspend and Resume and
  // calls itself stackable, CommonLibSSE-NG include/RE/S/ShaderReferenceEffect.h:61,
  // :66-67 and include/RE/R/ReferenceEffect.h:33-34), so a bare second Play
  // could draw it twice, and a Stop of one already gone is nothing
  // (HYPOTHESIS both ways until U3 reads it)
  private showLook(refr: ObjectReference, look: KeptLook, seconds: number, again: boolean): boolean {
    const form = this.sp.Game.getFormEx(snippetIdToLocalId(look.form));
    if (look.kind === 'eyes') {
      const set = this.sp.TextureSet.from(form);
      const actor = this.sp.Actor.from(refr);
      if (!set || !actor) {
        return false;
      }
      actor.setEyeTexture(set);
      return true;
    }
    if (look.kind === 'shader') {
      const shader = this.sp.EffectShader.from(form);
      if (!shader) {
        return false;
      }
      if (again) {
        shader.stop(refr);
      }
      shader.play(refr, seconds);
      return true;
    }
    const effect = this.sp.VisualEffect.from(form);
    if (!effect) {
      return false;
    }
    const facingId = look.facing !== 0 ? snippetIdToLocalId(look.facing) : 0;
    const facing = facingId !== 0 ? ObjectReference.from(this.sp.Game.getFormEx(facingId)) : null;
    if (again) {
      effect.stop(refr);
    }
    effect.play(refr, seconds, facing);
    return true;
  }

  // a relayed Stop, run here whatever was kept (as the snippet ran it)
  private stopLook(refr: ObjectReference, kind: LookKind, form: number): void {
    const f = this.sp.Game.getFormEx(snippetIdToLocalId(form));
    if (kind === 'shader') {
      this.sp.EffectShader.from(f)?.stop(refr);
    } else if (kind === 'effect') {
      this.sp.VisualEffect.from(f)?.stop(refr);
    }
  }

  // a look's form as its kind's type, the TextureSet, the EffectShader or
  // the VisualEffect showLook plays; null for one not here or of another
  // type
  private lookForm(kind: LookKind, form: number): TextureSet | EffectShader | VisualEffect | null {
    const f = this.sp.Game.getFormEx(snippetIdToLocalId(form));
    return kind === 'eyes' ? this.sp.TextureSet.from(f) : kind === 'shader' ? this.sp.EffectShader.from(f) : this.sp.VisualEffect.from(f);
  }

  // pollOne's, at a 3D load that has not had the kept looks: each played
  // again, a timed one for its seconds left (expireLooks took those run out
  // just before); every one dropped once this seat owns the subject (a
  // hand-off: its own engine draws them now); none on a copy that cannot
  // draw one, which waits REAPPLY_WAIT_MS for its next look and pays no
  // owner test meanwhile (the review of eaa6ba47, finding 5: ownsHere's
  // actor test walks the world model, HostService.isMineLocal). The update's
  // context
  private replayLooks(refr: ObjectReference, id: number, kept: LastAnimation, now: number): void {
    if (!this.drawable(refr)) {
      kept.lookAt = now + REAPPLY_WAIT_MS;
      return;
    }
    if (this.ownsHere(refr, id, kept.actor)) {
      this.dropOwnedLooks(id, kept);
      return;
    }
    kept.looksShown = true;
    kept.looks.forEach((look, key) => {
      const left = lookLeft(look, now);
      if (!this.showLook(refr, look, left, true)) {
        kept.looks.delete(key);
        this.sayLookDropped(id, look, 'its form not here');
        return;
      }
      this.sayLook('replayed', 'replayed', () => `look ${id.toString(16)} ${look.row} ${look.form.toString(16)} replayed at load (${left < 0 ? lookSecs(left) : `${lookSecs(left)} left of ${lookSecs(look.seconds)}`})`);
    });
  }

  // every kept timed look whose seconds ran out, gone (Map.forEach takes a
  // delete of the entry it is on). JS state alone
  private expireLooks(id: number, kept: LastAnimation, now: number): void {
    kept.looks.forEach((look, key) => {
      if (lookLeft(look, now) !== 0) {
        return;
      }
      kept.looks.delete(key);
      this.sayLook('expired', 'expired', () => `look ${id.toString(16)} ${look.row} ${look.form.toString(16)} dropped as expired (${lookSecs(look.seconds)}, ${kept.looksShown ? 'shown' : 'not shown'})`);
    });
  }

  // the subject's looks gone, this seat its owner now
  private dropOwnedLooks(id: number, kept: LastAnimation): void {
    kept.looks.forEach((look) => this.sayLookDropped(id, look, 'its subject owned here'));
    kept.looks.clear();
    kept.looksShown = true;
  }

  private holdsNothing(kept: LastAnimation): boolean {
    return kept.event === '' && kept.vars.size === 0 && kept.looks.size === 0;
  }

  private sayLookDropped(localId: number, look: KeptLook, why: string): void {
    this.sayLook('dropped', `dropped:${why}`, () => `look ${localId.toString(16)} ${look.row} ${look.form.toString(16)} dropped: ${why}`);
  }

  // one outcome counted, its kind's first line said whole (each connection's)
  private sayLook(outcome: LookOutcome, kind: string, line: () => string): void {
    this.lookCounts[outcome]++;
    if (this.lookSaid.has(kind) || this.lookSaid.size >= LOOK_SAID_MAX) {
      return;
    }
    this.lookSaid.add(kind);
    logLab('verb:scripts', line());
  }

  // the window's counts, said once a window when any moved (the second line
  // only for drops and fall-throughs), short of printConsole's 128-byte cut;
  // a line names only the counts that moved (the review of eaa6ba47, finding
  // 4: a window with nothing replayed said "0 replayed at load", which a
  // probe's "replayed at load" matched)
  private sayLookCounts(now: number): void {
    if (now - this.lookSummaryAt < LOOK_SUMMARY_MS) {
      return;
    }
    this.lookSummaryAt = now;
    const c = this.lookCounts;
    const s = LOOK_SUMMARY_MS / 1000;
    const moved = (terms: [number, string][]): string => terms.filter(([n]) => n > 0).map(([n, w]) => `${n} ${w}`).join(', ');
    const shown = moved([[c.played, 'played'], [c.held, 'held for 3D'], [c.replayed, 'replayed at load'], [c.expired, 'dropped as expired'], [c.stopped, 'stopped']]);
    if (shown !== '') {
      logLab('verb:scripts', `looks in ${s} s: ${shown}`);
    }
    const gone = moved([[c.dropped, 'dropped'], [c.through, 'fell through the 3D keep (run as always)']]);
    if (gone !== '') {
      logLab('verb:scripts', `looks in ${s} s: ${gone}`);
    }
    this.lookCounts = noLookCounts();
  }

  // a world object whose last animation is kept and re-applied here: never an
  // actor (the stream's) nor a trap family reference (every seat fires the
  // family itself, D-S3; remoteServer's replay skips it the same way), nor,
  // with the furniture path on, a furniture: its use event is relayed and
  // kept nowhere (a lever back at rest would push again at every late join
  // and every 3D load; the server keeps none either)
  private keepsFor(refr: ObjectReference, localId: number): boolean {
    if (this.sp.Actor.from(refr) || ObjectReferenceEx.isTrapFamily(localId)) {
      return false;
    }
    return !(this.furnitureEnabled && refr.getBaseObject()?.getType() === FormType.Furniture);
  }

  // ScriptStateService's "current" test beside the seq, in an update's
  // context: the kept last animation was played on the 3D now loaded (its
  // unload dropped the mark); a reference this service never keeps (an actor,
  // a trap family reference) is judged by the seq alone, as before
  shownHere(refr: ObjectReference): boolean {
    const localId = refr.getFormID();
    if (!this.keepsFor(refr, localId)) {
      return true;
    }
    return this.lastAnimations.get(localId)?.shown === true;
  }

  // the newest seq known here for the object: SpSnippetService's (its last
  // relay's, or a record's raise) or the kept one's
  private newestSeq(localId: number): number {
    return Math.max(this.controller.lookupListener(SpSnippetService).seqOf(localId) ?? 0, this.lastAnimations.get(localId)?.seq ?? 0);
  }

  // a release of a cell this seat hosted (HostService's, a reconnect's too):
  // its own script drove the cell's graphs past the words kept here, so they
  // go, and the next relay or record keeps them again. An actor's entry
  // stays: its looks are its simulator's (ADR-060's commander need not host
  // the cell), and the poll drops them once this seat simulates it. A message
  // handler: JS state alone
  private dropCell(cell: number): void {
    this.lastAnimations.forEach((kept, id) => {
      if (kept.cell === cell && !kept.actor) {
        this.forgetEntry(id, 'cell released here');
      }
    });
  }

  // the kept references' 3D, POLLS_PER_UPDATE of them an update in turn
  private pollLastAnimations(): void {
    const now = Date.now();
    this.sayLookCounts(now);
    if (!this.keepEnabled) {
      return;
    }
    const size = this.lastAnimations.size;
    if (size === 0) {
      return;
    }
    const entries = Array.from(this.lastAnimations.entries());
    const n = Math.min(size, POLLS_PER_UPDATE);
    const flips = this.controller.lookupListener(HostService).ownerFlips();
    for (let i = 0; i < n; i++) {
      const [id, kept] = entries[(this.pollAt + i) % size];
      try {
        this.pollOne(id, kept, now, flips);
      } catch (e) {
        // its looks counted as they go (the review of eaa6ba47, finding 7)
        this.forgetEntry(id, 'its 3D poll threw');
        logError(this, `object animation ${id.toString(16)}: its 3D poll threw, its last animation dropped: ${e}`);
      }
    }
    this.pollAt = (this.pollAt + n) % size;
  }

  // one kept reference: a 3D gone takes its shown mark (and a re-apply in
  // flight) with it, formView's nodeOverridesOn3D; a 3D here that has not
  // shown the kept animation plays it, unless this seat hosts the reference's
  // cell (its script drives the graph, and rung 3's new owner plays the
  // record's view once before its gate opens, ScriptStateService's pass, so
  // nothing is marked here) or a record of the reference is still being
  // applied (its own play shows the view). The hosted test is
  // epochForActor's, which also finds a persistent exterior reference's host
  // through its worldspace (its parent cell is the worldspace's persistent
  // cell, where isHostedHere misses it: the review of b86884a5, note 2).
  // A 3D here and shown is looked at every update, so its unload is seen.
  // The looks go first, on their own owner test (ownsHere): an actor's are
  // its simulator's, which the cell's host need not be. The test runs at
  // each 3D load (replayLooks) and whenever this seat's owners grew since the
  // last one (`flips`, HostService.ownerFlips: a promote, a gate's opening),
  // so a hand-off to this seat ends the kept looks whether or not the 3D
  // reloads, and costs one comparison an entry otherwise
  private pollOne(id: number, kept: LastAnimation, now: number, flips: number): void {
    if (now < kept.lookAt) {
      return;
    }
    if (kept.looks.size > 0) {
      this.expireLooks(id, kept, now);
      if (this.holdsNothing(kept)) {
        this.lastAnimations.delete(id);
        return;
      }
    }
    const refr = ObjectReference.from(this.sp.Game.getFormEx(id));
    if (refr && kept.looks.size > 0 && kept.flips !== flips) {
      kept.flips = flips;
      if (this.ownsHere(refr, id, kept.actor)) {
        this.dropOwnedLooks(id, kept);
        if (this.holdsNothing(kept)) {
          this.lastAnimations.delete(id);
          return;
        }
      }
    }
    if (!refr || !refr.is3DLoaded()) {
      kept.shown = false;
      kept.varsShown = false;
      kept.looksShown = false;
      kept.tries = 0;
      kept.lookAt = now + REAPPLY_WAIT_MS;
      return;
    }
    kept.lookAt = 0;
    if (kept.looks.size > 0 && !kept.looksShown) {
      this.replayLooks(refr, id, kept, now);
      if (this.holdsNothing(kept)) {
        this.lastAnimations.delete(id);
        return;
      }
    }
    // D-C3 A: the graph variables this 3D has not had, then the event
    const varsDue = kept.vars.size > 0 && !kept.varsShown;
    const eventDue = kept.event !== '' && !kept.shown;
    if (!varsDue && !eventDue) {
      return;
    }
    if (!reapplyNow({
      hostedHere: this.controller.lookupListener(HostService).epochForActor(refr) !== 0,
      viewPending: this.controller.lookupListener(ScriptStateService).viewOf(id) === 'pending',
      answered: kept.answered === true,
    })) {
      kept.lookAt = now + REAPPLY_WAIT_MS;
      return;
    }
    if (!this.keepsFor(refr, id)) {
      this.lastAnimations.delete(id);
      return;
    }
    if (varsDue) {
      const n = this.showVariables(refr);
      const first = kept.vars.values().next().value;
      if (n > 0 && first !== undefined && this.sayReapplied(id, now, this.varsSaidAt)) {
        let seq = 0;
        kept.vars.forEach((v) => {
          seq = Math.max(seq, v.seq);
        });
        logLab('verb:scripts', `object animation ${id.toString(16)} ${first.name} ${varText(first)}${n > 1 ? ` +${n - 1}` : ''} re-applied at a 3D load (graph variable, seq ${seq})`);
      }
    }
    if (!eventDue) {
      return;
    }
    const { played, jump } = this.playOn(refr, id, kept.event, true);
    kept.tries++;
    if (!played && kept.tries < REAPPLY_TRIES) {
      kept.lookAt = now + REAPPLY_RETRY_MS;
      return;
    }
    kept.shown = true;
    kept.answered = false;
    if (this.sayReapplied(id, now)) {
      logLab('verb:scripts', `object animation ${id.toString(16)} ${kept.event} re-applied at a 3D load (seq ${kept.seq}); ${played ? 'taken' : 'refused'}${kept.tries > 1 ? ` at try ${kept.tries}` : ''}${jump}`);
    }
  }

  // a re-apply's line at most once in REAPPLY_LINE_MS per reference (the
  // event's, or the graph variables' in their own table)
  private sayReapplied(id: number, now: number, said: Map<number, number> = this.reappliedSaidAt): boolean {
    const at = said.get(id);
    if (at !== undefined && now - at < REAPPLY_LINE_MS) {
      return false;
    }
    if (said.size >= 4096) {
      said.clear();
    }
    said.set(id, now);
    return true;
  }

  private noted: { selfId: number; event: string; took: boolean; furniture: boolean }[] = [];
  private selfPlayed = new Map<string, number>();
  private scriptSide = new Map<number, number>();
  private recordEvent = new Map<number, string>();
  private pillars = new Set<number>();
  // R-C4: the relays each pillar took here, the queued turn's supersede test
  private pillarRelays = new Map<number, number>();
  // relayed plays the copy's graph refused, tried again until taken
  private heldPlays = new HeldPlays();
  private resyncing = new Set<number>();
  // this seat's script's animation acts per pillar (noteScriptSide), read by
  // placeOnSide's guard
  private scriptActs = new Map<number, number>();
  // rung 3: each world object's last animation as kept here (keep), the
  // poll's place in the kept set, when each re-apply line was said, and the
  // game-file references a create word named in this connection
  private lastAnimations = new Map<number, LastAnimation>();
  private pollAt = 0;
  private reappliedSaidAt = new Map<number, number>();
  private varsSaidAt = new Map<number, number>();
  // the 3D keep's lines: each kind's first said (this connection's), the
  // window's counts, when the window began
  private lookSaid = new Set<string>();
  private lookCounts: Record<LookOutcome, number> = noLookCounts();
  private lookSummaryAt = 0;
  private subscribed = new Set<number>();
  // the create word's emitter idx for each subscribed reference, so its
  // DestroyActor (which names the idx alone) ends it (note 1)
  private subscribedIdx = new Map<number, number>();
  private keepEnabled: boolean;
  // the furniture path (startFurniture), and this seat's uses by local id,
  // until when (noteUse, noteClose)
  private furnitureEnabled: boolean;
  private uses = new Map<number, number>();
}
