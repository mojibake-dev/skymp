import { logLab } from "../../logging";
import { Actor } from "skyrimPlatform";
import { ApplyDeathStateEvent } from "../events/applyDeathStateEvent";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { RespawnNeededError } from "../../lib/errors";
import { AnimationEventName, takeRelayedStagger } from "../../sync/animation";
import { RagdollService } from "./ragdollService";

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
    this.applyDeathState(e.actor, e.isDead);
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

  private applyDeathState = (actor: Actor, isDead: boolean) => {
    if (isDead === true) {
      // thuum docs/verbs/headshot.md (ADR-038): the server's word confirms
      // a death this game played ahead; the real kill goes over the ragdoll
      this.predicted.delete(actor.getFormID());
    }
    if (actor.isDead() === isDead && this.isPlayer(actor) === false) {
      return;
    }
    if (isDead === true) {
      this.killActor(actor, null);
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

  private killActor = (actor: Actor, killer: Actor | null = null): void => {
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
      actor.endDeferredKill();
      actor.kill(killer);
      if (actor.getFormID() < 0xff000000) {
        // thuum docs/verbs/npc-fate.md (the deaths): the body is the
        // server's container, its activation blocked as a CONT's is
        // (dealWithRef): the engine's own loot menu never opens ahead of the
        // server's inventory and OpenContainer, while the activate hook
        // still reports the player's activation
        actor.blockActivation(true);
      }
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
    } else if (actor.getFormID() < 0xff000000) {
      // thuum docs/verbs/npc-fate.md (the deaths): a game-file actor stands
      // again by the engine's own Resurrect (inventory and AI reset, the
      // cell reset's shape, re-npc-roll.md section 9); the server's fresh
      // roll follows through ActorFate's late path and its placement through
      // the death container's teleport; never the copy's delete. A puppet
      // keeps its block; the host's own actor gets its engine behavior back
      // one game-thread task (FateApi.cpp resurrectActor): AI on for the
      // call, a puppet demoted again after it, a live actor unblocked;
      // Papyrus's Resurrect on a just-demoted corpse did not stand it up
      // (x-npc-respawn 070508)
      const id = actor.getFormID();
      const queued = this.sp.resurrectActor(id);
      logLab("verb:npc-fate", `resurrect ${id.toString(16)} (puppet ${this.sp.isPuppet(id)}) ${queued ? "queued" : "refused"}`);
    } else {
      throw new RespawnNeededError("needs to be respawned");
    }
  };

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

  private busyForOtherReasonsCounter = 0;
}
