import { ActivateEvent, Actor, ObjectReference, TriggerEntryEvent } from "skyrimPlatform";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { getInventory } from "../../sync/inventory";
import { ConnectionMessage } from "../events/connectionMessage";
import { SpSnippetMessage } from "../messages/spSnippetMessage";

// TODO: refactor this out
import { isSyncedActorId, localIdToRemoteId, snippetIdToLocalId } from "../../view/worldViewMisc";

import { LastInvService } from "./lastInvService";
import { HostService } from "./hostService";
import { FIRED_MS, RelayedActivationService } from "./relayedActivationService";
import { logError, logLab, logTrace } from "../../logging";
import { notePickupAsked } from "../../sync/bookHold";
import { FormTypeEx } from "../../extensions/formTypeEx";

// a reference as the lab's lines name it: its server id (a game-file
// reference's own), else this engine's
const idOf = (localId: number): string => (localIdToRemoteId(localId) || localId).toString(16);

const TAG = "verb:traps";
const PLAYER_ID = 0x14;
// game-file references end where the server's copies begin; the validator
// refuses an entry whose trigger is at 0xff000000 or up
const LAST_GAME_FILE_ID = 0xfeffffff;
// D-W1's marks, at most (past it they start over)
const FIRED_MAX = 256;
// D-C2's entries waiting for the update, at most, and the ones relayed a
// second (the plan's 1(d): a window of their own beside the linked one,
// inside the validator's entry_budget of 8 at once then 4 a second,
// UNMEASURED)
const ENTRIES_MAX = 64;
const ENTRIES_PER_S = 4;
// D-W3 B: the platform's held entries are asked again this long after an
// answer that still holds some (the platform's 60 s expiry and a second), so
// the expired go with their lines and a pair whose owner came by no counted
// flip is read once more
const HELD_AGAIN_MS = 61_000;
// thuum tracker #121, the arrival's window per walker and door: one report
// in the first, the roster's tick (HostService.sightingsMs), so the
// snippet's own activation and the engine's blocked package retrying the
// door land inside it; doubled after each report the server answered with no
// snippet (a refusal sends nothing back), up to the last, and back to the
// first at a snippet, which starts the window again (UNMEASURED: the blocked
// package's period is unread). Crossing (iii) keeps its windows in the same
// map, doubled the same way each time the walker is back at the door
const ARRIVAL_FIRST_MS = 2_000;
const ARRIVAL_MAX_MS = 32_000;
// past this since its window began, a walker and door's window is forgotten
const ARRIVAL_STALE_MS = 2 * ARRIVAL_MAX_MS;
// the walkers and doors with a window, at most (past it they start over)
const ARRIVALS_MAX = 256;

export class ActivationService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("activate", (e) => this.onActivate(e));
        // thuum tracker #121: the server's answer to an arrival report, read
        // off the snippet as it lands (JS state alone, a message's context)
        this.controller.emitter.on("spSnippetMessage", (e) => this.onArrivalSnippet(e));
        this.presserRule = this.startPresserRule();
        this.startTriggerLook(this.presserRule);
        if (this.presserRule) {
            this.controller.on("update", () => this.replayHeldEntries());
            this.controller.on("update", () => this.sendEntries());
        }
    }

    private onActivate(e: ActivateEvent) {
        let caster = e.caster ? e.caster.getFormID() : 0;
        let target = e.target ? e.target.getFormID() : 0;

        // thuum playtest 37 (2026-10-29 02:03Z, the live chest's second test,
        // finding 49): the baseline the container diff runs against
        // (containersService: a take or put is the engine's player inventory
        // against this) was taken at EVERY activation in the game, any
        // actor's (a resident's door, a puppet's replayed activation, a
        // trap's firing of itself), so an activation landing between the
        // player's take and the containerChanged event's delivery re-read the
        // inventory with the taken item already in it and the diff came out
        // empty: rotfern's garnet never left c1 as a TakeItem ("diff:" with no
        // entry at 02:03:34.860Z) while her puts, with no activation in their
        // window, did. The baseline is the player's own activation's alone.
        // HYPOTHESIS until a take under a busy cell's activations lands
        if (caster === 0x14) {
            const lastInvService = this.controller.lookupListener(LastInvService);
            lastInvService.lastInv = getInventory(this.sp.Game.getPlayer() as Actor);
        }

        if (!target || !caster) {
          return;
        }

        // thuum tracker #121 (design B, the destination's host takes the
        // arrival): an NPC this engine walked to a load door whose far side
        // is a cell this game hosts, with no game's word holding it here
        // (reportArrival), or one at a door whose two sides and own place
        // this game neither hosts nor has attached (crossUnwatched, ADR-051
        // decision 1's dark schedule); a hosted walker keeps D6's report
        // below
        if (caster !== PLAYER_ID && this.reportArrival(e, caster, target)) {
          return;
        }

        // the player, or a server actor (a 0xff copy or figure, or an actor
        // from the game files the server sent: docs/verbs/npc-puppet.md);
        // any other engine actor's activation is nobody's
        if (caster !== 0x14 && !isSyncedActorId(caster)) {
          // thuum docs/DECISIONS.md D-W1 A: with the presser rule on, the
          // root rule (judgeByPresser); the branches below are its A/B's off
          // arm (triggerPresser false, or a platform without the rule) as D-S3
          // left them, retired with the switch after playtest 50's T4 close
          if (this.presserRule) {
            this.judgeByPresser(e, caster, target);
            return;
          }
          // thuum docs/verbs/npc-fate.md (ADR-048's B, the hazard evidence): a
          // reference's activation of itself is a trap trigger's firing
          // (TrapTriggerBase, PressurePlate and Tripwire all activate(self),
          // and the engine forwards it to the trap with the plate as the
          // activator, never the stepping actor, so no activation names the
          // victim). The game that hosts the trigger's cell reports it as a
          // record: the server notes where and when, activates nothing, and
          // checks a hazard death against it. Every seat's engine runs the
          // trap for itself (a puppet steps on the plate too), so only the
          // host's word travels.
          const hostedHere = this.controller.lookupListener(HostService).isHostedHere(e.target);
          if (caster !== target) {
            // thuum docs/DECISIONS.md ADR-068 D-S3 (traps follow the
            // interaction; docs/research/trap-slice-d-s3-2026-11-05.md), read
            // on every seat, since every seat's engine fires the trap family
            // itself (point 1). Phase 2 first (Eli's requirement: any seat
            // triggers): a trigger that answers its own player alone
            // (TrapTriggerBase's playerOnlyTrigger: the trigger boxes 0x86018
            // in the Temple and 0x4e42b in the Sanctum; the platform's read is
            // HYPOTHESIS H-PO1) fires on the seat whose player entered it and
            // on no other, so its pair is reported from whichever seat it
            // fired on, and the server admits it from a reporter with the cell
            // loaded. The relay's own play of such a pair here comes back
            // through this event and is not reported again (else the two seats
            // would relay it to each other)
            const trigger = ObjectReferenceEx.trapRole(caster);
            if (trigger !== null && trigger.playerOnly) {
              if (this.controller.lookupListener(RelayedActivationService).wasPlayedHere(target, caster)) {
                logLab("verb:traps", `trap ${idOf(target)} fired here by ${idOf(caster)}: the relay's own play, not reported again (D-S3 phase 2)`);
                return;
              }
              logLab("verb:traps", `trap ${idOf(target)} fired here by ${idOf(caster)}: a player-only trigger, reported from this seat (D-S3 phase 2)`);
              this.reportLinked(caster, target);
              return;
            }
            // point 5 (the relay): a pair whose trigger AND target are both in
            // the family fires on every seat by itself, so it travels nowhere;
            // a pair with a mechanism on either side (a pull chain's blades,
            // the puzzle lever's darts: the design's F3) is the host's to
            // report and the server's to relay, as before
            if (trigger !== null && trigger.family && ObjectReferenceEx.isTrapFamily(target)) {
              logLab("verb:traps", `trap ${idOf(target)} fired here by ${idOf(caster)}: every seat fires the trap family itself, not reported (D-S3)`);
              return;
            }
            if (!hostedHere) {
              return;
            }
            this.reportLinked(caster, target);
            return;
          }
          if (!hostedHere) {
            return;
          }
          this.sendOwnActivation(target);
          return;
        }

        // thuum docs/DECISIONS.md ADR-062 decision 1: the player's own press on
        // an activator or movable static in a cell this game hosts is the
        // engine's to process (its block is lifted there: the script hears
        // OnActivate, the activation parents fire), so no Activate goes to the
        // server for it; the consequences travel as the host's reports (the
        // linked activations above, the object animations, the natives'
        // hooks). Everything else (doors, containers, items, furniture, any
        // cell this game does not host) goes to the server as before
        if (caster === 0x14 && e.target) {
            const base = e.target.getBaseObject();
            const kind = base ? base.getType() : -1;
            if (ObjectReferenceEx.liftsInHostedCell(kind) && ObjectReferenceEx.hostedHere(e.target)) {
                logTrace(this, `the engine's own activation of ${target.toString(16)} in a hosted cell; its consequences travel as the host's reports (ADR-062)`);
                return;
            }
        }

        // thuum docs/verbs/npc-handoff.md D6: a hosted actor's activation is
        // its host's report, so an actor this game does not simulate (a
        // puppet: its engine's furniture use before the demote, a relayed
        // sit) sends nothing from here; the server refused c2's report of
        // Jenassa taking her chair with "Bad hoster is attached to caster
        // 0xe1ba9" (x-npc-protected v1 20261025-234704), a message for nothing
        if (caster !== 0x14 && !this.controller.lookupListener(HostService).isMineLocal(caster)) {
            logTrace(this, `a puppet's own activation is its host's to report: `, caster.toString(16));
            return;
        }

        target = localIdToRemoteId(target);
        if (!target) {
            logError(this, 'localIdToRemoteId returned 0 (target) in on(\'activate\')');
            return;
        }

        caster = localIdToRemoteId(caster);
        if (!caster) {
            logError(this, 'localIdToRemoteId returned 0 (caster) in on(\'activate\')');
            return;
        }

        const openState = e.target.getOpenState();

        // TODO: add this to skyrimPlatform.ts
        const enum OpenState {
            None,
            Open,
            Opening,
            Closed,
            Closing,
        }

        if (openState === OpenState.Opening || openState === OpenState.Closing) {
            logTrace(this, "Ignoring activation of door because it's already opening or closing");
            return;
        }

        // the player's own activation of a game-file item asks the server for
        // its pickup, which this game's engine runs on the server's grant: the
        // reference and its base are noted here, in the activation's game
        // context, and become a hold when the grant arrives
        // (containersService), so a refused activation never holds
        const targetBase = e.target.getBaseObject();
        const localTarget = e.target.getFormID();
        if (e.caster && e.caster.getFormID() === 0x14 && targetBase && FormTypeEx.isItem(targetBase.getType()) && localTarget < 0xff000000) {
            notePickupAsked(localTarget, targetBase.getFormID(), Date.now());
        }

        this.controller.emitter.emit("sendMessage", {
            message: {
                t: MsgType.Activate,
                data: { caster, target, isSecondActivation: false }
            },
            reliability: "reliable"
        });

        logTrace(this, `Sent activation for caster=`, caster.toString(16), `and target=`, target.toString(16));
    }

  // thuum tracker #121 (playtest 55, Eli: "Wait for Gorm"; ADR-035 point 8's
  // design B, the destination's host takes the arrival). Gorm 0x1aa69's
  // record stood frozen in Highmoon Hall 0x138cc, which nobody hosted, while
  // c2 hosted the Moorside Inn 0x138ce and its engine walked him in the dark
  // (ADR-051 decision 1) to the inn's street door 0x177af. The engine's
  // ActivateRef 19796 (from the ActivateDoor handler 37878) refused him there
  // on the block every bound door carries (ObjectReferenceEx.dealWithRef's
  // kBlockActivate, read by the trace of tracker #121 note 3311, run
  // 20261118-023818 on c2, with him held in kMovementBlocked after it; the
  // hall's door 0x173c6 carried no block and he crossed it), and this seat
  // said nothing: no form held him, so
  // the event went to the presser rule, which reports a door in a hosted
  // cell alone. Now such a walker is reported as D6 reports a hosted one,
  // and the server's one door walk judges the report for this game
  // (MpObjectReference::WalkThroughDoor, the Rust cells judge): it accepts
  // only while no game simulates him and the far side is this game's own
  // cell, and then its snippet runs ObjectReference.Activate(walker, true)
  // here, the engine's own transit through the block (Papyrus's
  // BlockActivation idiom, D6's proven reply). HYPOTHESIS: that call crosses
  // a walker no seat runs at high process, as it crossed D6's loaded ones.
  //
  // The walker: an actor from the game files, alive, no puppet (a puppet's
  // activations are its host's), and no game's word here: a word for this
  // game is D6's report below, and a word "hosted by other" sends nothing
  // (the critic's rate rule). That word does not tell another game from
  // nobody: the server sends isHostedByOther true to every listener of an
  // actor no game simulates (MpObjectReference::UpdateHoster, PartOne's
  // CreateActor props), so a seat that holds a form for a frozen walker never
  // reports his arrival. Rare by the grid: the far side's host stands in the
  // far cell, across the load door from the record, and so is no listener of
  // it; the role-swapped x-gorm-inn-v5 reads what c1 holds for 0x1aa69.
  // The door: a Door whose activation is blocked, its load door link read
  // (pollLinkedDoors; a first meeting asks and lets the event go, and the
  // walker tries the door again after the engine's blocked package). The far
  // side's host is read as LockService reads it (HostService.hostsRef, by
  // grid square for a persistent exterior door; the server judges the XTEL
  // landing's cell instead, so for an exterior far side whose door stands at
  // a cell's edge the two can name different cells: a refused report, backed
  // off, or a walker never reported, a known edge the inn's interior is
  // clear of). One report per walker and
  // door per window (ARRIVAL_FIRST_MS, doubled after a report that brought no
  // snippet, up to ARRIVAL_MAX_MS); an event inside the window is taken and
  // dropped, so the branches below never report the same door for him. A
  // door whose far side this game does not host is crossed here unreported
  // when none of the walker's place and the door's two sides is watched here
  // (crossUnwatched, crossing (iii)), and goes to those branches as before
  // otherwise. True when this branch took the event; an activate event's
  // context, so the game reads are legal here
  private reportArrival(e: ActivateEvent, caster: number, target: number): boolean {
    if (caster > LAST_GAME_FILE_ID || target > LAST_GAME_FILE_ID || typeof this.sp.linkedDoorOf !== "function") {
      return false;
    }
    // the door first: most of an NPC's activations (a chair, a container)
    // leave here, before the world model is searched for its word
    const base = e.target.getBaseObject();
    if (!base || base.getType() !== this.sp.FormType.Door || !e.target.isActivationBlocked()) {
      return false;
    }
    const walker = this.sp.Actor.from(e.caster);
    if (!walker || walker.isDead()) {
      return false;
    }
    const host = this.controller.lookupListener(HostService);
    if (host.isMineLocal(caster) || host.isOthersLocal(caster) || this.sp.isPuppet(caster)) {
      return false;
    }
    const pair = this.sp.linkedDoorOf(target);
    if (pair === null) {
      if (typeof this.sp.pollLinkedDoors === "function") {
        this.sp.pollLinkedDoors([target]);
      }
      return false;
    }
    if (pair === 0) {
      return false;
    }
    const far = this.sp.ObjectReference.from(this.sp.Game.getFormEx(pair));
    if (!far) {
      return false;
    }
    if (!host.hostsRef(far)) {
      return this.crossUnwatched(e, caster, target, walker, far, host);
    }
    const now = Date.now();
    const key = `${caster}:${target}`;
    // an entry older than two of the last windows is of another visit, so
    // the window starts again at the first (one old refusal costs no 32 s)
    const kept = this.arrivals.get(key);
    const last = kept !== undefined && now - kept.sentAt <= ARRIVAL_STALE_MS ? kept : undefined;
    // a crossing's window (crossUnwatched's, doubled while he comes back)
    // holds a report back for the first window alone, enough for the
    // crossing's own event: a far side this game hosts now is reported
    const windowMs = last === undefined ? 0 : last.crossing ? Math.min(last.waitMs, ARRIVAL_FIRST_MS) : last.waitMs;
    if (last !== undefined && now - last.sentAt < windowMs) {
      return true;
    }
    const waitMs = last === undefined || last.answered ? ARRIVAL_FIRST_MS : Math.min(last.waitMs * 2, ARRIVAL_MAX_MS);
    if (last !== undefined && !last.answered) {
      logLab("verb:npc-handoff", `door ${idOf(target)}: ${idOf(caster)}'s last arrival report brought no snippet; reported again, the next after ${waitMs / 1000} s (tracker #121)`);
    }
    if (this.arrivals.size >= ARRIVALS_MAX && !this.arrivals.has(key)) {
      this.arrivals.clear();
    }
    this.arrivals.set(key, { sentAt: now, waitMs, answered: false, crossing: false });
    this.controller.emitter.emit("sendMessage", {
        message: {
            t: MsgType.Activate,
            data: { caster, target, isSecondActivation: false }
        },
        reliability: "reliable"
    });
    const cell = ObjectReferenceEx.getWorldOrCell(far);
    logLab("verb:npc-handoff", `door ${idOf(target)}: ${idOf(caster)} walks into ${cell.toString(16)}, which this game hosts; its arrival reported (tracker #121)`);
    return true;
  }

  // thuum tracker #121, crossing (iii) (ADR-051 decision 1: an actor no game
  // simulates walks his schedule in each engine's dark, and nothing of it is
  // reported; here that schedule reaches its doors). The case it is for is a
  // static read, HYPOTHESIS until x-gorm-inn-v6-swap runs on this branch's
  // build: in that frame c1, the inn's host, stood in the hall first, so its
  // client bound the hall's own door 0x173c6 with the block every bound Door
  // carries (ObjectReferenceEx.dealWithRef; a Door is no kind
  // liftsInHostedCell lifts), the block outlives the leave, and the door's
  // far side is the street, which nobody hosts, so reportArrival does not
  // cover it. Once the release rule (PuppetService) frees c1's copy in the
  // hall's buffer, c1's engine walks him to that door and the block would
  // refuse him there. Its stage run 20261118-040503 did not read this: it ran
  // the train's build (m4-train-55 0e7c9701, none of this branch's commits),
  // where c1 held him in 0x138cc as a puppet with his AI off (2 lines of
  // "puppet 1aa69: AI off", none of "released 1aa69", c1's npc-state
  // aiEnabled false, is3DLoaded true), so c1's engine never walked him and
  // the block on 0x173c6 was never reached. The read that decides it: on
  // this build, c1's release line for 1aa69 followed by "door 173c6: 1aa69's
  // schedule crosses here unreported", or the door trace of note 3311
  // (door-cross-trace.js) on c1 showing 0x173c6's activation by 0x1aa69 with
  // the block set.
  //
  // The walker has passed reportArrival's admission (from the game files,
  // alive, no game's word held here, this game's or another's, no puppet, a
  // blocked Door with its link read) and the far side is no cell this game
  // hosts. When this game watches none of the walker's own place, the door's
  // and the far side's (HostService.watchesRef: hosts the cell, or has it
  // attached as the release rule reads it, an exterior reference in the
  // player's worldspace by its grid square among the loaded set's), nobody
  // here sees the crossing and our block refusing it is our accident, so the
  // engine's own transit runs here: the door's Activate with the walker and
  // default processing only (Papyrus's BlockActivation idiom,
  // https://ck.uesp.net/wiki/Activate_-_ObjectReference; D6's reply, run on
  // this seat). Anything this game watches keeps the block, and the branches
  // below run as before. A known edge: the far side is read at its door's
  // square, while the walker lands at the door's XTEL landing, which at a
  // loaded grid's edge can stand in a square this game renders; the same
  // edge reportArrival names for hostsRef, beside an attached cell the
  // loaded set lacks (the design's risk 6).
  //
  // The window: at most one call per walker and door per ARRIVAL_FIRST_MS,
  // in the arrival's map, marked before the call. A walker back at the same
  // door inside ARRIVAL_STALE_MS did not cross (a crossing that worked puts
  // him on the far side, and a false return leaves him at the door), so the
  // window doubles each time, up to ARRIVAL_MAX_MS, as an unanswered report's
  // does; a crossing of this walker and door after a stale window starts at
  // the first again. The crossing line is written only when the call returns
  // true, so its count is the calls that took; a false return writes the
  // "returned false" line alone.
  //
  // The census: the call is the platform's own direct call (SkyrimPlatform.cpp's
  // worker under PlatformCall::Scope), so the ADR-062 census counts it as
  // "(platform)" (ScriptNatives::CountPlatformCall) and never raises it as a
  // script's consequence, as for D6's snippet call; slice 1b leaves
  // ObjectReference.Activate census-only (docs/NATIVES.md), and the seam's
  // step for the platform's own comes before any verdict. No second
  // crossing: a hosted walker (D6) and one with another game's word held
  // here (isOthersLocal) are refused at the admission; an arrival's snippet
  // starts its entry's window again as it lands (onArrivalSnippet), so its
  // own Activate lands inside a window whichever branch it reaches; and this
  // call's own event, if it raises one, lands in this window. HYPOTHESIS (the
  // static read of ActivateRef 19796, lab/frida/lever-furn-trace.js and
  // book-take.js): default processing only skips the TESActivateEvent, so
  // this call raises no event here at all. HYPOTHESIS, as for the arrival:
  // the call crosses a walker no seat runs at high process. True when this
  // branch took the event, false for the branches below; the activate event
  // is the platform's update task, so the call is legal here and never
  // inside the engine's own ActivateRef
  private crossUnwatched(e: ActivateEvent, caster: number, target: number, walker: Actor, far: ObjectReference, host: HostService): boolean {
    if (host.watchesRef(walker) || host.watchesRef(e.target) || host.watchesRef(far)) {
      return false;
    }
    const now = Date.now();
    const key = `${caster}:${target}`;
    const kept = this.arrivals.get(key);
    const last = kept !== undefined && now - kept.sentAt <= ARRIVAL_STALE_MS ? kept : undefined;
    if (last !== undefined && now - last.sentAt < last.waitMs) {
      return true;
    }
    // the last crossing of this walker and door, inside the stale window
    const again = last !== undefined && last.crossing ? last : undefined;
    const waitMs = again !== undefined ? Math.min(again.waitMs * 2, ARRIVAL_MAX_MS) : ARRIVAL_FIRST_MS;
    if (again !== undefined) {
      logLab("verb:npc-handoff", `door ${idOf(target)}: ${idOf(caster)} is back at it ${Math.round((now - again.sentAt) / 1000)} s after the last crossing; tried again, the next after ${waitMs / 1000} s (tracker #121)`);
    }
    if (this.arrivals.size >= ARRIVALS_MAX && !this.arrivals.has(key)) {
      this.arrivals.clear();
    }
    // answered: no snippet is awaited for a crossing, so a later arrival
    // report of this walker and door starts at the first window
    this.arrivals.set(key, { sentAt: now, waitMs, answered: true, crossing: true });
    const crossed = e.target.activate(e.caster, true);
    if (crossed) {
      logLab("verb:npc-handoff", `door ${idOf(target)}: ${idOf(caster)}'s schedule crosses here unreported (ADR-051 decision 1, tracker #121)`);
    } else {
      logLab("verb:npc-handoff", `door ${idOf(target)}: its Activate for ${idOf(caster)} returned false (tracker #121)`);
    }
    return true;
  }

  // the server's answer to an arrival report: D6's snippet
  // (ObjectReference.Activate on the door, the walker and true as its
  // arguments) marks the window answered, so the next report waits the first
  // window again, and starts that window as it lands (sentAt now, waitMs the
  // first): the snippet's own Activate, if it raises an event, lands inside
  // a window in reportArrival and crossUnwatched alike, however late the
  // snippet came (the review of ce94e7e61). A message's context: JS state
  // alone, no game call
  private onArrivalSnippet(event: ConnectionMessage<SpSnippetMessage>): void {
    const msg = event.message;
    if (this.arrivals.size === 0 || msg.class.toLowerCase() !== "objectreference" || msg.function.toLowerCase() !== "activate") {
      return;
    }
    const arg = Array.isArray(msg.arguments) ? msg.arguments[0] : undefined;
    const walkerId = arg !== null && typeof arg === "object" && typeof arg.formId === "number" ? snippetIdToLocalId(arg.formId) : 0;
    const doorId = snippetIdToLocalId(msg.selfId);
    if (!walkerId || !doorId) {
      return;
    }
    const arrival = this.arrivals.get(`${walkerId}:${doorId}`);
    if (arrival !== undefined && !arrival.answered) {
      const now = Date.now();
      logTrace(this, `door ${doorId.toString(16)}: the server walks ${walkerId.toString(16)} through here, its snippet in ${now - arrival.sentAt} ms (tracker #121)`);
      arrival.answered = true;
      arrival.sentAt = now;
      arrival.waitMs = ARRIVAL_FIRST_MS;
    }
  }

  // thuum docs/DECISIONS.md D-W1 A (a trigger volume's entry is its presser's
  // owner's; docs/research/fifty-fixes-2026-11-07.md 1(d), the root rule).
  // With the presser rule on, the platform's gate passes a trigger's
  // OnTriggerEnter only on the seat that owns its presser (its own player, an
  // NPC it simulates, a body in a cell it hosts), so a trap family trigger
  // fires on that seat alone. That seat reports the firing's first links (the
  // root), the server relays them to the cell's other games, and each of
  // their engines plays the root (RelayedActivationService) and propagates
  // the rest itself (a linker's nine children, the rocks): the cause crosses,
  // each engine computes the consequences (the plan's read 5). The marks: the
  // gate's triggerEntry on a family trigger (pressed here) and the trigger's
  // own activation (fired here), each for FIRED_MS. A trigger's own
  // activation (caster the target): a family trigger's is marked and
  // reported from this seat, whichever seat it is (the server admits a seat
  // with the cell loaded and an actor in reach); any other trigger's by the
  // cell's host, as before. A linked pair, in order: (1) the relay's own play
  // is not reported again; (2) a pair whose caster is a family trigger
  // pressed or fired here within FIRED_MS, and not itself played here by a
  // relay in that window, is the root, reported whatever its target; (3) a
  // pair whose caster and target are both in the family (D-S3 point 5's
  // test) is a later link, each seat's own: a family caster's script runs on
  // every seat (the linker 0xb6d3e is TrapTriggerBase, so the rocks stay
  // later links); (4) anything else is the cell's host's, as before, a
  // mechanism activating a family target among them (the puzzle lever
  // 0xaa8c3's darts, the pull chains 0xfda45 and 0xfda43's blades: no family
  // trigger, so its pair is the host's to report). An exempt trigger (D-W2's
  // fallback, getTrapRole's exempt: every seat's engine fires it, D-S3) is
  // never a root and its own activation is the cell host's report, so its
  // pairs take steps (3) and (4) as D-S3's point 5 did and no seat fires it
  // twice. HYPOTHESIS H-W1 until
  // x-trigger-owner: the press mark or the trigger's own activation reaches
  // this service before the pair it causes (the lines' order reads it). An
  // activate event's context: the game reads are legal here
  private judgeByPresser(e: ActivateEvent, caster: number, target: number): void {
    const now = Date.now();
    const relays = this.controller.lookupListener(RelayedActivationService);
    if (caster === target) {
      const own = ObjectReferenceEx.trapRole(caster);
      if (own !== null && own.family && own.trigger === true && own.exempt !== true) {
        this.markFired(caster, now);
        logLab(TAG, `trap ${idOf(target)} fired here by ${idOf(caster)}: its own activation, reported (D-W1)`);
        this.sendOwnActivation(target);
        return;
      }
      if (this.controller.lookupListener(HostService).isHostedHere(e.target)) {
        this.sendOwnActivation(target);
      }
      return;
    }
    if (relays.wasPlayedHere(target, caster)) {
      logLab(TAG, `trap ${idOf(target)} fired here by ${idOf(caster)}: the relay's own play, not reported again (D-W1)`);
      return;
    }
    const role = ObjectReferenceEx.trapRole(caster);
    if (role !== null && role.family && role.trigger === true && role.exempt !== true && this.firedHere(caster, now) && !relays.wasRelayPlayed(caster, now)) {
      logLab(TAG, `trap ${idOf(target)} fired here by ${idOf(caster)}: the presser's owner's firing, reported (D-W1)`);
      this.reportLinked(caster, target);
      return;
    }
    if (role !== null && role.family && ObjectReferenceEx.isTrapFamily(target)) {
      logLab(TAG, role.exempt === true
        ? `trap ${idOf(target)} fired here by ${idOf(caster)}: every seat fires the trap family itself, not reported (D-S3)`
        : `trap ${idOf(target)} fired here by ${idOf(caster)}: a later link of a firing, each seat's own (D-W1)`);
      return;
    }
    if (this.controller.lookupListener(HostService).isHostedHere(e.target)) {
      this.reportLinked(caster, target);
    }
  }

  // a trigger's own activation (a reference activating itself: TrapTriggerBase,
  // PressurePlate and Tripwire all activate(self)) as a record for the server,
  // which notes where and when, activates nothing and checks a hazard death
  // against it (thuum docs/verbs/npc-fate.md, ADR-048's B)
  private sendOwnActivation(target: number) {
    const remoteTrigger = localIdToRemoteId(target);
    if (!remoteTrigger) {
      return;
    }
    this.controller.emitter.emit("sendMessage", {
        message: {
            t: MsgType.Activate,
            data: { caster: remoteTrigger, target: remoteTrigger, isSecondActivation: false }
        },
        reliability: "reliable"
    });
    logTrace(this, `Sent a trigger's own activation for `, remoteTrigger.toString(16));
  }

  // a family trigger pressed (the gate's triggerEntry) or fired (its own
  // activation) here, rule 2's mark for FIRED_MS; JS state alone
  private markFired(localId: number, now: number) {
    if (this.firedAt.size >= FIRED_MAX && !this.firedAt.has(localId)) {
      this.firedAt.clear();
    }
    this.firedAt.set(localId, now);
  }

  private firedHere(localId: number, now: number): boolean {
    const at = this.firedAt.get(localId);
    return at !== undefined && now - at <= FIRED_MS;
  }

  // the platform's triggerEntry (a tick task from the gate's presser rule; no
  // game call in its handler, JS state alone): an owned presser's entry into
  // a family trigger marks it pressed here; one into a world trigger in a
  // cell this seat does not host (family false) waits for the update, which
  // relays it to the cell's host (D-C2 A). A leave of a family trigger marks
  // nothing
  private onTriggerEntry(e: TriggerEntryEvent) {
    if (!this.presserRule) {
      return;
    }
    const { refrId, presserId } = e;
    if (typeof refrId !== "number" || typeof presserId !== "number" || refrId === 0 || presserId === 0) {
      return;
    }
    const enter = e.enter === true;
    if (e.family === true) {
      if (enter) {
        this.markFired(refrId, Date.now());
        logLab(TAG, `trigger entry ${idOf(refrId)} by ${idOf(presserId)} (enter): a family trigger, pressed here (D-W1)`);
      }
      return;
    }
    if (this.entries.length >= ENTRIES_MAX) {
      logTrace(this, `trigger entry ${idOf(refrId)} by ${idOf(presserId)} dropped: ${ENTRIES_MAX} entries already wait for the update`);
      return;
    }
    this.entries.push({ refrId, presserId, enter });
  }

  // D-C2 A, the update's half: each entry into a world trigger in a cell this
  // seat does not host goes to the server as an Activate with its entry (1
  // OnTriggerEnter, 2 OnTriggerLeave), the trigger as its target and the
  // presser as the caster the wire names (the plan's 1(a): 0x14 for this
  // seat's own player, else an actor it simulates, by its server id; any
  // other presser is none of this seat's to report), at most ENTRIES_PER_S a
  // second. The server relays it to the cell's host alone, whose engine runs
  // the script with the presser's figure as akActionRef
  // (RelayedActivationService.playEntry)
  private sendEntries() {
    if (this.entries.length === 0) {
      return;
    }
    const entries = this.entries;
    this.entries = [];
    const host = this.controller.lookupListener(HostService);
    for (const { refrId, presserId, enter } of entries) {
      const head = `trigger entry ${idOf(refrId)} by ${idOf(presserId)} (${enter ? "enter" : "leave"})`;
      const caster = presserId === PLAYER_ID ? PLAYER_ID : host.isMineLocal(presserId) ? localIdToRemoteId(presserId) : 0;
      const target = localIdToRemoteId(refrId);
      if (!caster || !target || target > LAST_GAME_FILE_ID) {
        logTrace(this, `${head}: not relayed, ${!caster ? "the presser is none of this seat's" : "the trigger has no game-file id"}`);
        continue;
      }
      if (!this.entryBudget.take()) {
        logTrace(this, `${head}: not relayed, past ${ENTRIES_PER_S} entries a second`);
        continue;
      }
      this.controller.emitter.emit("sendMessage", {
          message: {
              t: MsgType.Activate,
              data: { caster, target, isSecondActivation: false, entry: enter ? 1 : 2 }
          },
          reliability: "reliable"
      });
      logLab(TAG, `${head}: relayed to the cell's host (D-C2)`);
    }
  }

  // thuum docs/DECISIONS.md ADR-068 D-W3 B (Eli 2026-11-10): an NPC standing
  // inside a world trigger at this seat's own load enters it before the
  // grant, its owner undecided here, so the platform's gate blanks the enter
  // and holds it (FridaHooks.cpp, the held entries; x-trigger-preload v1 read
  // no second enter after the grant). The platform replays a held enter once
  // through SendTriggerEvent's body when its presser is this seat's by the
  // gate's own owner test and its trigger stands in a cell this seat hosts;
  // it is asked through TESModPlatform.ReplayHeldEntries at the update after
  // the one that saw this seat's own hosting state change (HostService.
  // ownerFlips: a cell's gate opened, an actor marked hosted or promoted),
  // so never inside the change (the read's puppet flag cleared after
  // setHosted, in the same pass), once an update at most while changes keep
  // coming, and again HELD_AGAIN_MS after an answer that still holds some;
  // not while nothing changed. A platform without the native says so once
  // and is asked no more
  private replayHeldEntries() {
    if (this.replayHeldMissing) {
      return;
    }
    this.heldUpdates += 1;
    const flips = this.controller.lookupListener(HostService).ownerFlips();
    if (flips !== this.ownerFlipsSeen) {
      this.ownerFlipsSeen = flips;
      if (this.heldAskAt === 0) {
        this.heldAskAt = this.heldUpdates + 1;
      }
    }
    const now = Date.now();
    if (this.heldAgainAtMs !== 0 && now >= this.heldAgainAtMs) {
      this.heldAgainAtMs = 0;
      if (this.heldAskAt === 0) {
        this.heldAskAt = this.heldUpdates;
      }
    }
    if (this.heldAskAt === 0 || this.heldUpdates < this.heldAskAt) {
      return;
    }
    this.heldAskAt = 0;
    let raw: unknown;
    try {
      raw = this.sp.callNative("TESModPlatform", "ReplayHeldEntries", undefined);
    } catch (e) {
      const text = String(e);
      if (text.indexOf("Native function not found") !== -1) {
        this.replayHeldMissing = true;
        logLab(TAG, `ReplayHeldEntries unavailable on this platform (${text}); an entry held at a load is not replayed`);
        return;
      }
      logError(this, `ReplayHeldEntries threw (${text.slice(0, 64)})`);
      return;
    }
    let answer: Record<string, unknown> = {};
    try {
      const parsed: unknown = typeof raw === "string" && raw !== "" ? JSON.parse(raw) : {};
      if (parsed !== null && typeof parsed === "object") {
        answer = parsed as Record<string, unknown>;
      }
    } catch (e) {
      logError(this, `ReplayHeldEntries answered no JSON (${String(raw).slice(0, 64)})`);
      return;
    }
    const count = (key: string): number => (typeof answer[key] === "number" ? (answer[key] as number) : 0);
    const replayed = count("replayed");
    const refused = count("refused");
    const expired = count("expired");
    const held = count("held");
    if (replayed + refused + expired > 0) {
      logLab(TAG, `held trigger entries: ${replayed} replayed, ${refused} refused, ${expired} expired, ${held} still held (D-W3 B)`);
    }
    this.heldAgainAtMs = held > 0 ? now + HELD_AGAIN_MS : 0;
  }

  // the presser rule's A/B switch (.claude/rules/native.md): the client
  // setting triggerPresser, on unless false (ScriptStateService's read of
  // scriptRecord), told to the platform's gate through setTriggerPresserRule
  // (off by default in the platform, so an older client keeps D-S3). The rule
  // is on here only when the platform took it: a platform without the event
  // or the export keeps D-S3's branches on both sides, said once
  private startPresserRule(): boolean {
    const settings = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
    if (settings && settings["triggerPresser"] === false) {
      logTrace(this, "triggerPresser is off in settings: D-S3's branches stand, no trigger entry is relayed");
      try {
        this.sp.setTriggerPresserRule(false);
      } catch (e) {
        // an older platform: its gate has no presser rule to turn off
      }
      return false;
    }
    try {
      this.controller.on("triggerEntry", (e) => this.onTriggerEntry(e));
    } catch (e) {
      logLab(TAG, `triggerEntry unavailable on this platform (${e}); D-S3's branches stand`);
      return false;
    }
    try {
      if (this.sp.setTriggerPresserRule(true) === false) {
        logError(this, "setTriggerPresserRule(true) refused; D-S3's branches stand");
        return false;
      }
    } catch (e) {
      logLab(TAG, `setTriggerPresserRule unavailable on this platform (${e}); D-S3's branches stand`);
      return false;
    }
    logTrace(this, "the presser rule is on (D-W1): a trigger's entry is its presser's owner's");
    return true;
  }

  // thuum docs/DECISIONS.md D-W2 A (the trigger's own calls from its firing
  // seat; docs/research/fifty-fixes-2026-11-07.md 7.2(a) and (c)): the look's
  // A/B switch, the client setting triggerLook (on unless false), told to the
  // platform's gate through setTriggerLook the way the presser rule is (off
  // by default there); the look acts only with the presser rule on, so it is
  // told off whenever that rule is off here. Beside it the fallback lever,
  // triggerPresserExempt (the script classes whose non-owned presses the
  // presser rule passes, D-S3's per-seat firing for them; empty by default,
  // a list the lab fills only on a red), told through
  // setTriggerPresserExempt. A platform without an export says so once
  private startTriggerLook(presserOn: boolean): void {
    const settings = this.sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
    const off = settings !== undefined && settings["triggerLook"] === false;
    const look = presserOn && !off;
    if (off) {
      logTrace(this, "triggerLook is off in settings: a trigger's own calls stay each seat's (D-S3)");
    }
    try {
      if (this.sp.setTriggerLook(look) === false && look) {
        logError(this, "setTriggerLook(true) refused; a trigger's own calls stay each seat's");
      } else if (look) {
        logTrace(this, "the trigger look is on (D-W2): a firing's own calls cross from its firing seat");
      }
    } catch (e) {
      if (look) {
        logLab(TAG, `setTriggerLook unavailable on this platform (${e}); a trigger's own calls stay each seat's`);
      }
    }
    if (!presserOn) {
      return;
    }
    const listed: unknown = settings !== undefined ? settings["triggerPresserExempt"] : undefined;
    const exempt = Array.isArray(listed) ? listed.filter((x): x is string => typeof x === "string" && x !== "") : [];
    if (listed !== undefined && (!Array.isArray(listed) || exempt.length !== listed.length)) {
      logError(this, "triggerPresserExempt is not a list of script class names; its other entries are ignored");
    }
    try {
      if (this.sp.setTriggerPresserExempt(exempt) === false) {
        logError(this, `setTriggerPresserExempt refused ${exempt.length} class(es)`);
      } else if (exempt.length > 0) {
        logLab(TAG, `presser rule exempt (D-W2's fallback, D-S3 for them): ${exempt.join(", ")}`);
      }
    } catch (e) {
      if (exempt.length > 0) {
        logLab(TAG, `setTriggerPresserExempt unavailable on this platform (${e}); no class is exempt`);
      }
    }
  }

  // thuum docs/verbs/traps.md (M4): a linked activation this game's engine
  // ran (a plate firing its spike wall, a lever opening its portcullis)
  // reported for the server to judge against the game files' activation
  // parents and relay to the cell's other games; at most eight a second leave
  // this game (the server's own bound), the rest are dropped here. HYPOTHESIS
  // until x-bleak-traps: how many engine-internal activations a dungeon cell
  // raises.
  private reportLinked(caster: number, target: number) {
    if (!this.linkedBudget.take()) {
      return;
    }
    const remoteCaster = localIdToRemoteId(caster);
    const remoteTarget = localIdToRemoteId(target);
    if (!remoteCaster || !remoteTarget) {
      return;
    }
    this.controller.emitter.emit("sendMessage", {
        message: {
            t: MsgType.Activate,
            data: { caster: remoteCaster, target: remoteTarget, isSecondActivation: false }
        },
        reliability: "reliable"
    });
    logTrace(this, `Sent a linked activation of `, remoteTarget.toString(16), `by`, remoteCaster.toString(16));
  }

  // eight linked activations a second from this game (thuum docs/verbs/traps.md)
  private readonly linkedBudget = new SecondWindow(8);

  // D-W1 and D-C2: whether the presser rule is on here (the setting and the
  // platform's answer), the family triggers pressed or fired here by when,
  // the entries waiting for the update, and their own window
  private readonly presserRule: boolean;
  private readonly firedAt = new Map<number, number>();
  private entries: { refrId: number; presserId: number; enter: boolean }[] = [];
  private readonly entryBudget = new SecondWindow(ENTRIES_PER_S);
  // D-W3 B: the owner flips seen (HostService.ownerFlips), this service's
  // update count, the update at which the platform's held entries are asked
  // (0 for none due), when they are asked again, and an older platform
  // without the native
  private ownerFlipsSeen = 0;
  private heldUpdates = 0;
  private heldAskAt = 0;
  private heldAgainAtMs = 0;
  private replayHeldMissing = false;
  // thuum tracker #121: the arrival reports and crossings by walker and door
  // ("<walker>:<door>", local ids), each with its window (from sentAt: the
  // report's or the crossing's time, or the snippet's once one answers),
  // whether a snippet answered it, and whether it is a crossing (iii)'s
  private readonly arrivals = new Map<string, { sentAt: number; waitMs: number; answered: boolean; crossing: boolean }>();

};

// a count within a rolling one-second window
class SecondWindow {
  constructor(private readonly most: number) {}
  take(): boolean {
    const now = Date.now();
    if (now - this.startMs >= 1000) {
      this.startMs = now;
      this.count = 0;
    }
    if (this.count >= this.most) {
      return false;
    }
    this.count += 1;
    return true;
  }
  private startMs = 0;
  private count = 0;
}
