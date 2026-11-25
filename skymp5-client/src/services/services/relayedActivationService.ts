import { Inventory, ObjectReference } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { ConnectionMessage } from '../events/connectionMessage';
import { RelayedActivationMessage } from '../messages/relayedActivationMessage';
import { remoteIdToLocalId } from '../../view/worldViewMisc';
import { logError, logLab, logTrace } from '../../logging';
import { PuppetService } from './puppetService';
import { HostService } from './hostService';
import { RemoteServer } from './remoteServer';
import { ScriptStateService } from './scriptStateService';
import { ScriptEvent, eventAnswerOf, relayDropLine, scriptEventLine, scriptEventOf } from '../../sync/relayedEvent';

const TAG = 'verb:traps';
// this game's own player
const PLAYER_ID = 0x14;

// thuum docs/DECISIONS.md ADR-068 rung 5 (the acting scope; docs/research/
// rung5-quests-2026-11-09.md 1(c), read 7): the stage the platform's
// setActingPlayer names a relayed trigger entry by (its subject the
// trigger's form id), and how long the request waits for SendTriggerEvent to
// take it, ms (the send follows at once; the platform bounds a ttl at 60000)
const ENTRY_SCOPE = -3;
const ENTRY_ACTING_TTL_MS = 10000;
// thuum tracker #117 (playtest 53's Sanctum door, Eli: "door animated and
// opened for both, only rotfern hosting was able to interact with it"): the
// stage setActingPlayer names a relayed activation by (its subject the
// pressed reference), taken by the platform's TESModPlatform.ArmActivation
// in the same update, before this seat's own activate call
const ACTIVATION_SCOPE = -4;
const ACTIVATION_ACTING_TTL_MS = 10000;
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

// ArmActivation's answer for the line ({formId, armed, scripts, items,
// reason}): "the presser's scope armed over <n> script(s)[, its record's <m>
// item(s) carried]" or "not armed (<reason>)"
const describeArm = (raw: unknown): string => {
  if (typeof raw !== 'string') {
    return `ArmActivation answered no string (${typeof raw})`;
  }
  try {
    const o = JSON.parse(raw) as Record<string, unknown> | null;
    if (o && typeof o === 'object' && typeof o['armed'] === 'boolean') {
      if (o['armed']) {
        const scripts = o['scripts'];
        const items = o['items'];
        const carried = typeof items === 'number' && items > 0 ? `, its record's ${items} item(s) carried` : '';
        return `the presser's scope armed over ${typeof scripts === 'number' ? scripts : '?'} script(s)${carried}`;
      }
      const reason = o['reason'];
      return `not armed (${typeof reason === 'string' && reason !== '' ? reason : 'refused'})`;
    }
  } catch (e) {
    // the raw answer below
  }
  return `ArmActivation answered ${raw.slice(0, 40)}`;
};

// thuum tracker #117 (Eli's design B): the presser's recorded inventory, as
// the server sent it with the press (RelayedActivation's presserInventory),
// flattened to the base id and count pairs setActingPlayer's items take; the
// platform sums a base's stacks and answers the press's GetItemCount on the
// figure from them. undefined when the relay carried none (a press with no
// record runs its count on the figure as before)
const itemPairs = (inventory?: Inventory): number[] | undefined => {
  if (!inventory || !Array.isArray(inventory.entries)) {
    return undefined;
  }
  const pairs: number[] = [];
  for (const e of inventory.entries) {
    if (e && typeof e.baseId === 'number' && typeof e.count === 'number' && e.baseId > 0 && e.count > 0) {
      pairs.push(e.baseId, e.count);
    }
  }
  return pairs;
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
    const { target, caster, entry, presserInventory } = e.message;
    // a script event for the object's scripts alone (sync/relayedEvent): it
    // waits for the object's cell's grant pass as an entry does
    const ev = scriptEventOf(e.message);
    if (ev === null) {
      logLab(TAG, `relayed event ${String(e.message.event)} on ${target.toString(16)} by ${caster.toString(16)} names no script event: not sent`);
      return;
    }
    if (ev !== undefined) {
      this.controller.once('update', () => {
        const object = ObjectReference.from(this.sp.Game.getFormEx(remoteIdToLocalId(target)));
        const cell = object?.getParentCell()?.getFormID() ?? 0;
        this.controller.lookupListener(ScriptStateService).afterHosting(cell, () => this.playEvent(target, caster, ev, presserInventory ?? undefined));
      });
      return;
    }
    if (entry === undefined || entry === null) {
      this.controller.once('update', () => this.play(target, caster, presserInventory ?? undefined));
      return;
    }
    if (entry !== 1 && entry !== 2) {
      logError(this, `relayed entry ${target.toString(16)} by ${caster.toString(16)}: entry ${String(entry)} names no trigger event, not sent`);
      return;
    }
    // C-entry: a relay that comes with this seat's grant of the trigger's
    // cell (its own blanked entry sent back, or an enter held for the grant)
    // waits for the grant's pass, as ObjectAnimationService and
    // LoginCellService do: the cell's script records and ref state applied
    // and the gate open (HostService.openGate) before the event reaches the
    // trigger's scripts, so its natives are judged as the owner's (OwnedHere);
    // with no pass waiting it plays on the update after. The trigger's cell
    // needs a game call, hence the first update (train 53's review,
    // contracts-1)
    this.controller.once('update', () => {
      const trigger = ObjectReference.from(this.sp.Game.getFormEx(remoteIdToLocalId(target)));
      const cell = trigger?.getParentCell()?.getFormID() ?? 0;
      // a script override of the trigger the server sent ahead of this entry
      // (the state a player's own record decides) is applied first
      const scripts = this.controller.lookupListener(ScriptStateService);
      scripts.afterHosting(cell, () => scripts.afterOverride(remoteIdToLocalId(target), () => this.playEntry(target, caster, entry)));
    });
  }

  private play(remoteTarget: number, remoteCaster: number, presserInventory?: Inventory) {
    const target = ObjectReference.from(
      this.sp.Game.getFormEx(remoteIdToLocalId(remoteTarget)),
    );
    const caster = ObjectReference.from(
      this.sp.Game.getFormEx(remoteIdToLocalId(remoteCaster)),
    );
    if (!target || !caster) {
      logLab(TAG, relayDropLine(remoteTarget, remoteCaster, !!target, !!caster));
      return;
    }
    this.markPlayed(target.getFormID(), caster.getFormID());
    this.markRelayPlayed(target.getFormID());
    this.armPressActing(target, caster, remoteCaster, presserInventory);
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
    const own = this.isOwnPlayer(remoteCaster);
    const caster = ObjectReference.from(
      this.sp.Game.getFormEx(own ? PLAYER_ID : remoteIdToLocalId(remoteCaster)),
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
    logLab(TAG, `${head}${own ? ' (this seat\'s own player, its entry the gate blanked here)' : ''}: ${describeTriggerSend(raw, event)}`);
  }

  // a script event another seat's player made on an object this seat owns,
  // sent to the object's scripts alone through the platform's
  // TESModPlatform.SendObjectEvent (never the engine's default processing),
  // called as SendTriggerEvent is, the presser's figure the event's actor,
  // inside the presser's acting scope as a relayed press runs
  // (namePressActing's request for the object, which the native arms); this
  // seat's own player, a press the gate blanked here, as itself. An update's
  // context. A platform without the native throws "Native function not
  // found": said once, and no script event is sent from then
  private playEvent(remoteTarget: number, remoteCaster: number, ev: ScriptEvent, presserInventory?: Inventory) {
    if (this.sendEventMissing) {
      return;
    }
    const target = ObjectReference.from(this.sp.Game.getFormEx(remoteIdToLocalId(remoteTarget)));
    const own = this.isOwnPlayer(remoteCaster);
    const caster = ObjectReference.from(this.sp.Game.getFormEx(own ? PLAYER_ID : remoteIdToLocalId(remoteCaster)));
    if (!target || !caster) {
      logLab(TAG, `relayed ${ev.name} on ${remoteTarget.toString(16)} by ${remoteCaster.toString(16)} not sent: no ${!target && !caster ? 'target or presser' : !target ? 'target' : 'presser'} here`);
      return;
    }
    this.markRelayPlayed(target.getFormID());
    if (!own) {
      this.namePressActing(target, caster, remoteCaster, presserInventory);
    }
    // an event's form and projectile as this game names them, None for none
    // or one not here
    const form = (formId: number) => (formId === 0 ? null : this.sp.Game.getFormEx(remoteIdToLocalId(formId)));
    let raw: unknown;
    try {
      raw = this.sp.callNative('TESModPlatform', 'SendObjectEvent', undefined, target, ev.code, caster, form(ev.source), form(ev.projectile), ev.count, ev.flags);
    } catch (e) {
      const text = String(e);
      if (text.indexOf('Native function not found') !== -1) {
        this.sendEventMissing = true;
        logLab(TAG, `SendObjectEvent unavailable on this platform (${text}); relayed script events are not sent`);
        return;
      }
      logLab(TAG, `relayed ${ev.name} on ${remoteTarget.toString(16)} by ${remoteCaster.toString(16)} not sent: the native threw (${text.slice(0, 32)})`);
      return;
    }
    logLab(TAG, scriptEventLine(remoteTarget, remoteCaster, ev, eventAnswerOf(raw)));
  }

  // Train 53, C-entry (lab/.cache/drafts/reads-53/r-c1-c2-c4-claw.md, R-C1):
  // the server sends a host's own entry back to it when the gate blanked it
  // here before the cell's grant (the seat's own presser entering a world
  // trigger of a cell it did not host yet, then granted the cell before the
  // server read the entry), so the presser is this seat's own player.
  // remoteIdToLocalId cannot name it: the view holds no form view of this
  // connection's own actor (FormViewArray's updateAll and syncFormView skip
  // playerCharacterFormIdx unless show-me is set, and with show-me its view
  // is an offset copy, not the player), so the own server id is the player,
  // 0x14, here. A server id compared by its low 32 bits, as
  // QuestStageService.isMe reads one. No acting player is named for it
  // (nameEntryActing takes figures alone): the trigger's script reads
  // Game.GetPlayer() as this seat's own player, its presser
  private isOwnPlayer(remoteId: number): boolean {
    const mine = this.controller.lookupListener(RemoteServer).getMyRemoteRefrId();
    return mine > 0 && mine % 0x100000000 === remoteId;
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

  // thuum tracker #117 (ADR-063: the cell's host owns its world objects'
  // scripts; ADR-068 rung 5's acting scope): a guest's press of a scripted
  // reference in a cell this seat hosts reaches here as a relayed activation
  // with the guest's figure as the caster, and the engine's own activate
  // below plays it. A script that takes a press only from the player
  // (`akActionRef == Game.GetPlayer()`: the Sanctum rings'
  // HallofStoriesDiskScript and its keyhole's HallofStoriesKeyholeScript,
  // thuum lab/pex.py disasm of the winning copies; the census's 27 of 54
  // Bleak Falls and Embershard world scripts that call GetPlayer,
  // docs/research/scripts-one-system-2026-11-05.md section 8) took none of
  // the guest's, so the guest's rings never turned (playtest 53's server.log
  // 00:32:16 to 00:32:20Z: five guest presses relayed to the host, no ring
  // animated). The press now runs for its presser, as a relayed trigger entry
  // does (nameEntryActing): the activation's acting player named for the
  // reference, then the platform's ArmActivation takes it and arms the
  // reference's scope for its own attached scripts, so the OnActivate the
  // activate raises reads Game.GetPlayer() as the figure (HYPOTHESIS: the
  // engine raises it on a stack rooted at the reference's script within the
  // arm's window). Only where this seat hosts the reference's cell (the
  // owner's input; a host's own press relayed to a guest for render stays a
  // plain activate there, its script the host's) and only for another
  // player's figure (PuppetService.isFigure: an NPC's copy is no player).
  // Said once on a platform without the export or the native
  private armPressActing(target: ObjectReference, caster: ObjectReference, presserServerId: number, presserInventory?: Inventory) {
    if (this.activationArmMissing) {
      return;
    }
    if (!this.namePressActing(target, caster, presserServerId, presserInventory)) {
      return;
    }
    const head = `relayed activation ${target.getFormID().toString(16)} by ${presserServerId.toString(16)}`;
    let raw: unknown;
    try {
      raw = this.sp.callNative('TESModPlatform', 'ArmActivation', undefined, target, caster);
    } catch (e) {
      const text = String(e);
      if (text.indexOf('Native function not found') !== -1) {
        this.activationArmMissing = true;
        logLab(TAG, `ArmActivation unavailable on this platform (${text}); a relayed press's script reads this seat's own player`);
        return;
      }
      logLab(TAG, `${head}: not armed, the native threw (${text.slice(0, 32)})`);
      return;
    }
    logLab(TAG, `${head}: ${describeArm(raw)}`);
  }

  // the presser named as the acting player for the object (setActingPlayer
  // with stage ACTIVATION_SCOPE, the presser's inventory for its
  // GetItemCount), which the platform's ArmActivation or SendObjectEvent
  // takes for the object's scripts: only where this seat hosts the object's
  // cell and only for another player's figure; true when named
  private namePressActing(target: ObjectReference, caster: ObjectReference, presserServerId: number, presserInventory?: Inventory): boolean {
    if (this.pressActingMissing) {
      return false;
    }
    const presserLocalId = caster.getFormID();
    if (presserLocalId < FIRST_SERVER_ID || presserServerId < FIRST_SERVER_ID) {
      return false;
    }
    if (!this.controller.lookupListener(PuppetService).isFigure(presserLocalId)) {
      return false;
    }
    const cell = target.getParentCell()?.getFormID() ?? 0;
    if (cell === 0 || !this.controller.lookupListener(HostService).hostsCell(cell)) {
      return false;
    }
    const head = `relayed press ${target.getFormID().toString(16)} by ${presserServerId.toString(16)}`;
    if (typeof this.sp.setActingPlayer !== 'function') {
      this.pressActingMissing = true;
      logLab(TAG, "setActingPlayer unavailable on this platform; a relayed press's script reads this seat's own player");
      return false;
    }
    try {
      if (this.sp.setActingPlayer(target.getFormID(), ACTIVATION_SCOPE, presserLocalId, presserServerId, ACTIVATION_ACTING_TTL_MS, itemPairs(presserInventory)) !== true) {
        logLab(TAG, `${head}: the platform refused the press's acting player (stage ${ACTIVATION_SCOPE}); its script reads this seat's own player`);
        return false;
      }
    } catch (e) {
      logError(this, `setActingPlayer threw for the ${head}`, e);
      return false;
    }
    return true;
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
  private activationArmMissing = false;
  // an older platform without setActingPlayer for a press, or without
  // SendObjectEvent
  private pressActingMissing = false;
  private sendEventMissing = false;
  // an older platform without setActingPlayer (rung 5)
  private actingMissing = false;
}
