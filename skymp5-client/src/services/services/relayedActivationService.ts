import { ObjectReference } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { ConnectionMessage } from '../events/connectionMessage';
import { RelayedActivationMessage } from '../messages/relayedActivationMessage';
import { remoteIdToLocalId } from '../../view/worldViewMisc';
import { logError, logLab, logTrace } from '../../logging';
import { PuppetService } from './puppetService';

const TAG = 'verb:traps';

// thuum docs/DECISIONS.md ADR-068 rung 5 (the acting scope; docs/research/
// rung5-quests-2026-11-09.md 1(c), read 7): the stage the platform's
// setActingPlayer names a relayed trigger entry by (its subject the
// trigger's form id), and how long the request waits for SendTriggerEvent to
// take it, ms (the send follows at once; the platform bounds a ttl at 60000)
const ENTRY_SCOPE = -3;
const ENTRY_ACTING_TTL_MS = 10000;
// the players' actors (and the server's copies) are server ids from here up
const FIRST_SERVER_ID = 0xff000000;

// thuum docs/DECISIONS.md D-W1 A (ActivationService's root rule): a family
// trigger pressed or fired on this seat is a firing's root for this long, and
// a reference a relay played here is that relay's consequence for as long
// (UNMEASURED: the longest delay read is the tripwire's XAPR, 0.5 s)
export const FIRED_MS = 5000;

// SendTriggerEvent's answer for the line ({formId, event, sent, scripts,
// reason}, SendScriptEvent's shape): "<event> sent to <n> script(s)" or
// "<event> not sent (<reason>)"; anything else as it came
const describeTriggerSend = (raw: unknown, event: string): string => {
  if (typeof raw !== 'string') {
    return `${event} answered no string (${typeof raw})`;
  }
  try {
    const o = JSON.parse(raw) as Record<string, unknown> | null;
    if (o && typeof o === 'object' && typeof o['sent'] === 'boolean') {
      if (o['sent']) {
        const scripts = o['scripts'];
        return `${event} sent to ${typeof scripts === 'number' ? scripts : '?'} script(s)`;
      }
      const reason = o['reason'];
      return `${event} not sent (${typeof reason === 'string' && reason !== '' ? reason : 'refused'})`;
    }
  } catch (e) {
    // the raw answer below
  }
  return `${event} answered ${raw.slice(0, 40)}`;
};

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
// D-W1 A (docs/research/fifty-fixes-2026-11-07.md 1(d)): with the presser
// rule on, one press is one firing per seat (the presser's owner's engine
// fires, every other seat plays its relayed root), so the double play above
// is gone there; two presses by two owners still give two, declared. Every
// play marks its target relay-played for FIRED_MS (wasRelayPlayed), which the
// root rule reads: a trigger a relay played is no root of its own. D-C2 A: a
// relay with an entry is a guest's entry into a world trigger of a cell this
// seat hosts, sent to the trigger's scripts with the presser as akActionRef
// (playEntry) instead of activating. Rung 5: when the presser is another
// player's figure, the entry names that player as its acting player first
// (setActingPlayer with stage -3: the trigger's form id, the figure's local
// id, the player's server id), which SendTriggerEvent takes to arm the
// trigger's scope, so the trigger's script reads Game.GetPlayer() as the
// guest's figure (akActionRef == Game.GetPlayer() holds on the host) and a
// stage it sets names the guest (HYPOTHESIS H-AP7).
//
// Lines for the lab: "relayed activation <target> by <caster>", and
// "[verb:traps] relayed entry <target> by <caster>: <event> sent to <n>
// script(s)" (or "not sent (<reason>)").
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
    const { target, caster, entry } = e.message;
    if (entry === undefined || entry === null) {
      this.controller.once('update', () => this.play(target, caster));
      return;
    }
    if (entry !== 1 && entry !== 2) {
      logError(this, `relayed entry ${target.toString(16)} by ${caster.toString(16)}: entry ${String(entry)} names no trigger event, not sent`);
      return;
    }
    this.controller.once('update', () => this.playEntry(target, caster, entry));
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
    this.markRelayPlayed(target.getFormID());
    target.activate(caster, false);
    logTrace(
      this,
      `relayed activation ${remoteTarget.toString(16)} by ${remoteCaster.toString(16)}`,
    );
  }

  // D-C2 A (a world script's trigger, the cobwebs' first): a guest's entry
  // relayed to this seat, the cell's host, sent to the trigger's scripts with
  // the presser's figure as akActionRef through the platform's
  // TESModPlatform.SendTriggerEvent (OnTriggerEnter or OnTriggerLeave, then
  // the reference's aliases: the engine's own sink's shape, SendScriptEvent's
  // twin with the argument), on the update where natives are legal (the
  // relay waits for it, as play does); the platform's own call passes the
  // gate, and the script's consequences cross as any host script's do
  // (ScriptCall 75). A platform without the native throws "Native function
  // not found" (CallNative.cpp): said once, and no relayed entry is sent from
  // then; any other throw is that entry's alone (LoginCellService's idiom)
  private playEntry(remoteTarget: number, remoteCaster: number, entry: number) {
    const event = entry === 1 ? 'OnTriggerEnter' : 'OnTriggerLeave';
    if (this.sendTriggerMissing) {
      return;
    }
    const head = `relayed entry ${remoteTarget.toString(16)} by ${remoteCaster.toString(16)}`;
    const target = ObjectReference.from(
      this.sp.Game.getFormEx(remoteIdToLocalId(remoteTarget)),
    );
    const caster = ObjectReference.from(
      this.sp.Game.getFormEx(remoteIdToLocalId(remoteCaster)),
    );
    if (!target || !caster) {
      logLab(TAG, `${head}: ${event} not sent, no ${target ? 'presser' : 'trigger'} here`);
      return;
    }
    this.markRelayPlayed(target.getFormID());
    this.nameEntryActing(target.getFormID(), caster.getFormID(), remoteCaster);
    let raw: unknown;
    try {
      raw = this.sp.callNative('TESModPlatform', 'SendTriggerEvent', undefined, target, event, caster);
    } catch (e) {
      const text = String(e);
      if (text.indexOf('Native function not found') !== -1) {
        this.sendTriggerMissing = true;
        logLab(TAG, `SendTriggerEvent unavailable on this platform (${text}); relayed entries are not sent`);
        return;
      }
      logLab(TAG, `${head}: ${event} not sent, the native threw (${text.slice(0, 32)})`);
      return;
    }
    logLab(TAG, `${head}: ${describeTriggerSend(raw, event)}`);
  }

  // rung 5: a relayed entry's acting player, when its presser here is
  // another player's figure (PuppetService.isFigure: a server copy of an
  // NPC is no player, and the platform arms such a presser with no player);
  // a table write, no game call. Said once on an older platform without the
  // export
  private nameEntryActing(triggerId: number, presserLocalId: number, presserServerId: number) {
    if (this.actingMissing || presserLocalId < FIRST_SERVER_ID || presserServerId < FIRST_SERVER_ID) {
      return;
    }
    if (!this.controller.lookupListener(PuppetService).isFigure(presserLocalId)) {
      return;
    }
    if (typeof this.sp.setActingPlayer !== 'function') {
      this.actingMissing = true;
      logLab(TAG, "setActingPlayer unavailable on this platform; a relayed entry's script reads this seat's own player");
      return;
    }
    try {
      if (this.sp.setActingPlayer(triggerId, ENTRY_SCOPE, presserLocalId, presserServerId, ENTRY_ACTING_TTL_MS) !== true) {
        logError(this, `setActingPlayer refused the relayed entry ${triggerId.toString(16)} by ${presserServerId.toString(16)}`);
      }
    } catch (e) {
      logError(this, `setActingPlayer threw for the relayed entry ${triggerId.toString(16)} by ${presserServerId.toString(16)}`, e);
    }
  }

  // Whether a relay played the reference here within FIRED_MS (local id, as
  // the target of a played pair or of a relayed entry): the root rule's test
  // that a trigger's firing is a relay's consequence, not this seat's own
  wasRelayPlayed(localId: number, now: number): boolean {
    const until = this.relayPlayed.get(localId);
    return until !== undefined && now <= until;
  }

  private markRelayPlayed(localId: number) {
    if (this.relayPlayed.size >= RelayedActivationService.playedMax && !this.relayPlayed.has(localId)) {
      this.relayPlayed.clear();
    }
    this.relayPlayed.set(localId, Date.now() + FIRED_MS);
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
  // the references a relay played here, until when (wasRelayPlayed)
  private readonly relayPlayed = new Map<number, number>();
  private sendTriggerMissing = false;
  // an older platform without setActingPlayer (rung 5)
  private actingMissing = false;
}
