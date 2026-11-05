import { ActivateEvent, Actor } from "skyrimPlatform";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { getInventory } from "../../sync/inventory";

// TODO: refactor this out
import { isSyncedActorId, localIdToRemoteId } from "../../view/worldViewMisc";

import { LastInvService } from "./lastInvService";
import { HostService } from "./hostService";
import { RelayedActivationService } from "./relayedActivationService";
import { logError, logLab, logTrace } from "../../logging";

// a reference as the lab's lines name it: its server id (a game-file
// reference's own), else this engine's
const idOf = (localId: number): string => (localIdToRemoteId(localId) || localId).toString(16);

export class ActivationService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        this.controller.on("activate", (e) => this.onActivate(e));
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

        // the player, or a server actor (a 0xff copy or figure, or an actor
        // from the game files the server sent: docs/verbs/npc-puppet.md);
        // any other engine actor's activation is nobody's
        if (caster !== 0x14 && !isSyncedActorId(caster)) {
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

        this.controller.emitter.emit("sendMessage", {
            message: {
                t: MsgType.Activate,
                data: { caster, target, isSecondActivation: false }
            },
            reliability: "reliable"
        });

        logTrace(this, `Sent activation for caster=`, caster.toString(16), `and target=`, target.toString(16));
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
