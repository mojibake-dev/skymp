import { logLab, logTrace } from "../../logging";
import { Actor } from "skyrimPlatform";
import { ApplyDeathStateEvent } from "../events/applyDeathStateEvent";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RespawnNeededError } from "../../lib/errors";
import { AnimationEventName, takeRelayedStagger } from "../../sync/animation";
import { RagdollService } from "./ragdollService";
import { PuppetService } from "./puppetService";
import { ChangeFormService } from "./changeFormService";
import { RemoteServer } from "./remoteServer";
import { localIdToRemoteId } from "../../view/worldViewMisc";

export class DeathService extends ClientListener {
  constructor(private sp: Sp, private controller: CombinedController) {
    super();
    controller.once("update", () => this.onceUpdate());
    controller.emitter.on("applyDeathStateEvent", (e) => this.onApplyDeathState(e));
    this.hookDisableKillMoves();
    this.hookDisableStagger();
    this.hookDisableBlockedAnims();
  }

  public isBusy() {
    return this.playerDead || this.busyForOtherReasonsCounter > 0;
  }

  private onceUpdate() {
    const player = this.sp.Game.getPlayer();
    player?.startDeferredKill();
  }

  private onApplyDeathState(e: ApplyDeathStateEvent) {
    this.applyDeathState(e.actor, e.isDead, e.silent === true);
  }

  private hookDisableKillMoves() {
    this.sp.hooks.sendAnimationEvent.add(
      {
        enter(ctx) {
          ctx.animEventName = "";
        },
        leave() { },
      },
      0xff000000,
      0xffffffff,
      "KillMove*"
    );
    // thuum docs/verbs/npc-fate.md (the guest's half-up body, 2026-11-03): a
    // puppet's death is the server's word (ADR-050 decision 2), so a kill
    // move's victim half never reaches a puppet's graph, whichever mouth it
    // comes by (the action replay, dropped in formView.replayActions, or the
    // event relay, which a creature's events and a humanoid event outside the
    // PerformAction entry ride): the figures' blank widened to the game-file
    // range with the puppet read inside the hook, the animation.ts idiom; the
    // id range and the patterns filter in native (rule 9), the puppet read
    // costs one native per matching event. The names are the idles' ENAMs
    // less "pa_" (lab/esm.py on Skyrim.esm and Update.esm, numidium's read):
    // KillMove1HMStabUpFace and KillMoveD seen in the lab; KillingBlow (three
    // bleed-out idles), 2HWKillMoveA and KillWerewolfPaired* in the tree,
    // HYPOTHESIS beyond the two seen
    // the whole id range: a server-made NPC's copy (0xff000000 and up) is a
    // puppet on the seats that do not host it and takes the replay and the
    // event relay like any NPC model; the puppet read keeps figures out (a
    // figure is never flagged), and a KillMove* the figures' handler already
    // blanked reaches these as "" and matches nothing (the fresh review of
    // 7d0d4d7e, note 1)
    const sp = this.sp;
    for (const pattern of ["KillMove*", "KillingBlow*", "2HWKillMove*", "KillWerewolf*"]) {
      this.sp.hooks.sendAnimationEvent.add(
        {
          enter(ctx) {
            if (sp.isPuppet(ctx.selfId)) {
              ctx.animEventName = "";
            }
          },
          leave() { },
        },
        0,
        0xffffffff,
        pattern
      );
    }
  }

  // A figure staggers only by its own player's relayed stagger, which the
  // server imposed there (thuum docs/verbs/stagger.md); this game's engine's
  // own stagger of a figure is its verdict on a hit, refused
  private hookDisableStagger() {
    this.sp.hooks.sendAnimationEvent.add(
      {
        enter(ctx) {
          if (takeRelayedStagger(ctx.selfId)) {
            return;
          }
          ctx.animEventName = "";
        },
        leave() { },
      },
      0xff000000,
      0xffffffff,
      "staggerStart"
    );
  }

  private hookDisableBlockedAnims() {
    this.sp.hooks.sendAnimationEvent.add(
      {
        enter: (ctx) => {
          if (this.allowedPlayerAnimations === null) {
            return;
          }
          if (!this.allowedPlayerAnimations.includes(ctx.animEventName)) {
            ctx.animEventName = "";
          }
        },
        leave() { },
      },
      this.playerActorId,
      this.playerActorId
    );
  }

  private applyDeathState = (actor: Actor, isDead: boolean, silent = false) => {
    if (isDead === true) {
      // thuum docs/verbs/headshot.md (ADR-038): the server's word confirms
      // a death this game played ahead; the real kill goes over the ragdoll
      this.predicted.delete(actor.getFormID());
    }
    if (actor.isDead() === isDead && this.isPlayer(actor) === false) {
      // the word and the engine agree: a queued stand-up has landed
      this.resurrectQueuedAt.delete(actor.getFormID());
      return;
    }
    if (isDead === true) {
      this.killActor(actor, null, silent);
    } else {
      this.resurrectActor(actor);
    }
  };

  // thuum docs/verbs/headshot.md (ADR-038, Eli: "for marksman combat, some
  // elements at least, the feel is more important"): a figure's death this
  // game plays ahead of the server's word, the way the local player's own
  // death is played (the ragdoll, nothing killed: the figure's million and
  // startDeferredKill stand), and takes back if the word never comes. The
  // kill itself stays the server's (ADR-028); this is its playback.
  private readonly predicted = new Map<number, number>();

  public predictFigureDeath(actor: Actor): boolean {
    if (this.isPlayer(actor) || actor.isDead()) {
      return false;
    }
    const id = actor.getFormID();
    if (this.predicted.has(id)) {
      return false;
    }
    this.predicted.set(id, Date.now());
    actor.pushActorAway(actor, 0);
    return true;
  }

  public hasPredictedDeath(actorId: number): boolean {
    return this.predicted.has(actorId);
  }

  // thuum docs/verbs/npc-fate.md (ADR-048's B): whether `actorId`'s death
  // the engine just played was the server's own word (killActor above);
  // consumed by the report that asks, so a later death of the same actor
  // (after its respawn) is reported again. A mark the engine never answered
  // (a kill on a body already dead raises no deathStart) expires after
  // serverKillMarkMs, so it cannot swallow a real death after the respawn
  public takeServerKill(actorId: number): boolean {
    const markedAt = this.serverKills.get(actorId);
    if (markedAt === undefined) {
      return false;
    }
    this.serverKills.delete(actorId);
    return Date.now() - markedAt <= DeathService.serverKillMarkMs;
  }

  private readonly serverKills = new Map<number, number>();
  private static readonly serverKillMarkMs = 10_000;

  // the puppet to the host's last sample of its actor (the world model's
  // movement for the form), when it stands more than 64 units from it
  private restPuppetAtHost(actor: Actor): void {
    const localId = actor.getFormID();
    if (!this.controller.lookupListener(PuppetService).isPuppet(localId)) {
      return;
    }
    const remoteId = localIdToRemoteId(localId);
    const world = this.controller.lookupListener(RemoteServer).getWorldModel();
    const form = world.forms.find((f) => f !== undefined && f.refrId === remoteId);
    const pos = form?.movement?.pos;
    if (!pos || pos.length < 3) {
      return;
    }
    const dx = pos[0] - actor.getPositionX();
    const dy = pos[1] - actor.getPositionY();
    const dz = pos[2] - actor.getPositionZ();
    const apart = Math.sqrt(dx * dx + dy * dy + dz * dz);
    if (!Number.isFinite(apart) || apart < 64) {
      return;
    }
    actor.setPosition(pos[0], pos[1], pos[2]);
    logTrace(this, `npc-fate: puppet ${localId.toString(16)} placed at the host's spot before its death, ${apart.toFixed(0)} units away`);
  }

  // the word never came: the figure stands back up where it fell
  public revertPredictedDeath(actor: Actor): void {
    const id = actor.getFormID();
    if (!this.predicted.delete(id) || actor.isDead()) {
      return;
    }
    const ragdollService = this.controller.lookupListener(RagdollService);
    ragdollService.safeRemoveRagdollFromWorld(actor, () => {
      const again = Actor.from(this.sp.Game.getFormEx(id));
      if (!again || again.isDead()) {
        return;
      }
      again.setAnimationVariableInt("iGetUpType", 1);
      this.sp.Debug.sendAnimationEvent(again, AnimationEventName.GetUpBegin);
    });
  }

  // `silent`: the bind kill's word (formView's dead check; playtest 37, 3i
  // lay, Eli: "they stay down but i hear a death sound and they are in
  // different places if killed when someone else enters the cell"): an actor
  // the game files spawned alive while the record holds it dead has no death
  // to play, so it dies by KillSilent, the Papyrus native that sends no death
  // events, instead of Kill's full death (the sound, the push). The server's
  // kill mark, the puppet's rest and the loot block stay as for any kill; the
  // player's own death never comes this way (the dead check skips 0x14), so
  // the flag means nothing on that branch
  private killActor = (actor: Actor, killer: Actor | null = null, silent = false): void => {
    if (this.isPlayer(actor) === true) {
      this.playerDead = true;
      this.busyForOtherReasonsCounter++;
      this.sp.Utility.wait(7.5).then(() => this.busyForOtherReasonsCounter--);
      this.allowedPlayerAnimations = [];
      actor.setDontMove(true);
      this.killWithPush(actor);
    } else {
      if (actor.getFormID() < 0xff000000) {
        // thuum docs/verbs/npc-fate.md (ADR-048's B): the kill below fires
        // the engine's death events; DeathReportService must not report a
        // death that is the server's own word
        this.serverKills.set(actor.getFormID(), Date.now());
      }
      // thuum docs/verbs/npc-fate.md (ADR-050 decision 2, train 52): a
      // puppet's body lies where its host's does. The host's last sample
      // places the puppet BEFORE the kill, while it can still be moved: a
      // dead ragdoll takes no setPosition (x-npc-death-host v3
      // 20261027-091759: the rest after the death was logged, 329 units,
      // and the body stayed; the bodies lay 326 units apart)
      this.restPuppetAtHost(actor);
      actor.endDeferredKill();
      if (silent) {
        actor.killSilent(killer);
        // thuum playtest 38's finding 71 ("essentially t posing half stuck in
        // the ground"): a body moved to its record's spot and killed silently
        // plays no fall, so it keeps its standing pose with its root at the
        // record's ground point. The engine's push with no force ragdolls it
        // where it stands, and the ragdoll settles on the floor as a fallen
        // body does. HYPOTHESIS until a probe reads the pose (x-body-pose).
        // By id after the wait, as revertPredictedDeath above: the wrapper
        // captured here may be stale once the tick has passed (the reference
        // deleted or replaced), and a body no longer there or no longer dead
        // gets no push
        const id = actor.getFormID();
        this.sp.Utility.wait(0.1).then(() => {
          const again = Actor.from(this.sp.Game.getFormEx(id));
          if (!again || !again.isDead()) {
            return;
          }
          // thuum ADR-067 by ADR-065's path (ChangeFormService): a body with
          // a change-form record held for this kill or laid on it is not
          // pushed; the push's ragdoll would undo the record's pose and start
          // playtest 42's F4 slide again (the kill above is deferred and the
          // apply lands with it, a frame or two in, so the record is still
          // "held" at this wait's end: docs/research/adr062/
          // papyrus-native-entries.md:73)
          if (this.controller.lookupListener(ChangeFormService).hasRecord(localIdToRemoteId(id) || id)) {
            logLab("verb:npc-fate", `body ${id.toString(16)}: no push, the change-form record laid it`);
            return;
          }
          again.pushActorAway(again, 0);
        });
      } else {
        actor.kill(killer);
      }
      // thuum ADR-067 by ADR-065's path (ChangeFormService, docs/verbs/
      // change-form.md): a change-form record of the body the server served
      // (the owner's own save of its corpse) and held for this kill is
      // applied as the kill lands (the kill above is deferred: Kill and
      // KillSilent queue a type-10 task and the actor is alive at the
      // native's return, docs/research/adr062/papyrus-native-entries.md:73;
      // the service tries every update until the copy reads dead), the
      // engine's own load laying the body in the owner's pose at the owner's
      // place; a record that comes later is applied at its arrival, the body
      // dead already. The bind (silent) applies on any seat; a kill by the
      // word never takes a held record over this seat's own engine's death
      // of an actor it simulates
      this.controller.lookupListener(ChangeFormService).onKilledHere(actor.getFormID(), silent);
      // thuum ADR-067 decision 6 (the exclusive container hold;
      // docs/private/container-hold-scope.md D-C2 A): the body is the
      // server's container, its activation blocked as a CONT's is
      // (dealWithRef's shape), so the engine's own loot menu never opens
      // ahead of the server's word: E sends Activate, the server admits one
      // looter and answers OpenContainer, remoteServer's handler opens the
      // ContainerMenu, and a refused looter sees the notification and no
      // menu. The block is PuppetService's (blockDeadBody: once per body, a
      // figure never, re-asserted a tick or two after each native lift);
      // laid here at the server's word and at the bind's silent kill, at the
      // death's start for this seat's own engine deaths, and from the
      // reconciler's passes. The block that stood here until c1de7167 was
      // taken out on
      // playtest 37's findings 58 and 62 ("the server's activation never
      // opened a blocked body"); D71 then read the refusal as the released
      // body's AI off (engine model E25), which openBodyOnTimers switches on
      // for the open, and the promote's re-block went out in the promote's
      // own tick, which its task would have undone (a source reading, no
      // trace). That the engine's activate(player, true) opens the loot menu
      // on a blocked dead actor as it opens a blocked CONT is the HYPOTHESIS
      // x-container-hold's corpse leg reads.
      // formView lays the server's list under the menu as before (memo D52 B).
      this.controller.lookupListener(PuppetService).blockDeadBody(actor.getFormID(), actor, silent ? "the bind's silent kill" : "the server's word");
    }
  };

  private resurrectActor = (actor: Actor): void => {
    if (this.isPlayer(actor) === true) {
      this.playerDead = false;
      this.busyForOtherReasonsCounter++;
      this.sp.Utility.wait(7.5).then(() => this.busyForOtherReasonsCounter--);
      this.allowedPlayerAnimations = null;
      actor.setDontMove(false);
      this.ressurectWithPushKill(actor);
    } else if (actor.getFormID() < 0xff000000 || this.isNpcCopy(actor)) {
      // thuum docs/verbs/npc-fate.md (the deaths): a game-file actor stands
      // again by the engine's own Resurrect (inventory and AI reset, the
      // cell reset's shape, re-npc-roll.md section 9); the server's fresh
      // roll follows through ActorFate's late path and its placement through
      // the death container's teleport; never the copy's delete. A puppet
      // keeps its block; the host's own actor gets its engine behavior back
      // one main-thread task (FateApi.cpp resurrectActor, SKSE's UI task
      // queue since fork cc5c82b6): AI on for the
      // call, a puppet demoted again after it, a live actor unblocked;
      // Papyrus's Resurrect on a just-demoted corpse did not stand it up
      // (x-npc-respawn 070508). A server-made NPC's copy (playtest 37's
      // finding 54: a reanimated bandit's puppet stayed a body) takes the
      // same path: the server's live word stands its puppet up as a
      // game-file actor's, never the delete and re-placement a player's
      // figure takes (the throw below)
      const id = actor.getFormID();
      // thuum docs/verbs/npc-fate.md 1e (x-fate-dress-bleak v5 20261104-070209:
      // "resurrect 39fe4 (puppet false) queued" 33 times a respawn): the live
      // word is applied by every view update, movement and property message
      // while the engine still reads the copy dead, and the native's task
      // lands a frame or more later, so each tick queued another stand-up.
      // One in flight per actor: a second ask within RESURRECT_INFLIGHT_MS
      // is dropped, the mark cleared when the word and the engine agree
      const now = Date.now();
      const queuedAt = this.resurrectQueuedAt.get(id);
      if (queuedAt !== undefined && now - queuedAt < DeathService.RESURRECT_INFLIGHT_MS) {
        return;
      }
      this.resurrectQueuedAt.set(id, now);
      // thuum ADR-065's path (ChangeFormService): a dead record held for this
      // body is stale once the live word stands it up
      this.controller.lookupListener(ChangeFormService).onStoodUp(id);
      const queued = this.sp.resurrectActor(id);
      // the reconciler arms the deferred kill again on its next pass
      // (ADR-050 decision 3: the stood-up actor holds at zero for the
      // server's word as before its death)
      this.controller.lookupListener(PuppetService).onResurrected(id);
      logLab("verb:npc-fate", `resurrect ${id.toString(16)} (puppet ${this.sp.isPuppet(id)}) ${queued ? "queued" : "refused"}`);
    } else {
      throw new RespawnNeededError("needs to be respawned");
    }
  };

  // a server-made NPC's copy, as remoteServer.ts's npcCopy reads the world
  // model: a form with an NPC base (never the players' 0x7) that is not this
  // player's own clone; a player's figure is not one
  private isNpcCopy(actor: Actor): boolean {
    const remoteId = localIdToRemoteId(actor.getFormID());
    if (!remoteId) {
      return false;
    }
    const world = this.controller.lookupListener(RemoteServer).getWorldModel();
    const form = world.forms.find((f) => f !== undefined && f.refrId === remoteId);
    return !!form && !!form.baseId && form.baseId !== 0x7 && form.isMyClone !== true;
  }

  private killWithPush = (actor: Actor): void => {
    this.allowedPlayerAnimations?.push(AnimationEventName.Ragdoll);
    actor.pushActorAway(actor, 0);
  };

  private ressurectWithPushKill = (act: Actor): void => {
    const formId = act.getFormID();
    const ragdollService = this.controller.lookupListener(RagdollService);
    ragdollService.safeRemoveRagdollFromWorld(act, () => {
      const actor = Actor.from(this.sp.Game.getFormEx(formId));
      if (!actor) {
        return;
      }
      // TODO: should use actor variable instead of getPlayer?
      // TODO: use different iGetUpType if ressurecting under water
      this.sp.Game.getPlayer()!.setAnimationVariableInt("iGetUpType", 1);
      this.sp.Debug.sendAnimationEvent(actor, AnimationEventName.GetUpBegin);
    });
  };

  private isPlayer = (actor: Actor): boolean => {
    return actor.getFormID() === this.playerActorId;
  };

  // Null to allow all animations. Empty array to disallow all
  private allowedPlayerAnimations: string[] | null = null;

  private readonly playerActorId = 0x14;

  private playerDead = false;
  // the stand-ups queued to the game thread and not yet seen landed, by actor
  private resurrectQueuedAt = new Map<number, number>();
  private static readonly RESURRECT_INFLIGHT_MS = 2000;

  private busyForOtherReasonsCounter = 0;
}
