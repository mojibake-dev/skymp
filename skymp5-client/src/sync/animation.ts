/* eslint-disable @typescript-eslint/no-empty-function */
import {
  ObjectReference,
  Debug,
  hooks,
  Actor,
  printConsole,
  Utility,
  Game,
  storage,
  // @ts-expect-error (TODO: Remove in 2.10.0)
  setCollision,
  isPuppet,
  isPerformingAction,
  once
} from "skyrimPlatform";
import { Movement } from "./movement";
import { applyWeapDrawn } from "./movementApply";
import { clearKnockedDown, isKnockedDown, markKnockedDown } from "./knockState";
import { isHandCastEvent } from "../services/services/actionSyncService";

export enum AnimationEventName {
  Ragdoll = "Ragdoll",
  GetUpBegin = "GetUpBegin",
};

export interface Animation {
  animEventName: string;
  numChanges: number;
}

export interface AnimationApplyState {
  lastNumChanges: number;
  useAnimOverrides: boolean;
}

const allowedIdles = new Array<[number, string]>();
const refsWithDefaultAnimsDisabled = new Set<number>();
const allowedAnims = new Set<string>();

// thuum docs/verbs/stagger.md: a figure's staggerStart relayed from its own
// player's game, which staggered at the server's word; deathService refuses
// every other staggerStart on a figure (this game's own verdict on a hit)
const relayedStaggers = new Set<number>();
export const takeRelayedStagger = (refrId: number): boolean =>
  relayedStaggers.delete(refrId);

// thuum docs/verbs/push.md (the flight): in the shouter's game the vanilla
// push throws the target's figure (A1's caster half), so the figure is down
// already when its player's relayed Ragdoll comes. The relayed handler's
// zero-force push, landing 20 to 50 ms after that knock, gave the figure a
// fresh knock in place and it never flew (x-push-flight 20261014-215458 and
// -220105 snapped; -210908, the relayed push first, and -215801, 76 ms
// after, flew). So the handler leaves a figure this game has knocked down
// alone (sync/knockState.ts), holding only its movement; a bystander's game,
// which runs no push, still knocks it. relayedKnocks: the handler's own
// pushes, whose Ragdoll does not count as this game's own knock
const relayedKnocks = new Map<number, number>();
const relayedKnockMs = 500;

const actorSitAnimsLowerCase = [
  'idlestoolenterplayer',
  'idlestoolenter',
  'idlestoolenterinstant',
  'idlechairrightenter',
  'idlechairleftenter',
  'idlechairfrontenter',
  'idlechairenterinstant',
  'idlejarlchairenter',
  'idlejarlchairenterinstant',
  'idlesnowelfprincechairdialogue',
  'idlesnowelfprincechairenter',
  'idlesnowelfprincechairenterinstant',
  'idlechairchildenterinstant',
  'idlechairchildfrontenter',
  'idlechairchildleftenter',
  'idlechairchildrightenter',
];

const actorGetUpAnimsLowerCase = [
  'idlestoolbackexit',
  'idlechairrightexit',
  'idlechairrightquickexit',
  'idlechairleftexit',
  'idlechairleftquickexit',
  'idlechairfrontexit',
  'idlechairfrontquickexit',
  'idlechairchildfrontexit',
  'idlechairchildleftexit',
  'idlechairchildrightexit'
];

// It's critical for values to be the correct case, not just lowercase, otherwise 'allowedIdles' check will break
// We don't want to modify the check itself, because it'll be slower
const animOverridesLowerCase: Record<string, string | undefined> = {
  'idlechairbook_onepage': 'IdleChairEnterInstant',
  'idlechairshoulderflex': 'IdleChairEnterInstant',
  'idlechairwrite': 'IdleChairEnterInstant',
  'idlechairarmscrossedvar1': 'IdleChairEnterInstant',
  'chaireatingstart_vampiremeat': 'IdleChairEnterInstant',
  'chairreadingstart': 'IdleChairEnterInstant',
  'chairvampireeatingstart': 'IdleChairEnterInstant',
  'chairdrinkingstart': 'IdleChairEnterInstant',
  'chaireatingstart': 'IdleChairEnterInstant',

  // The only triple animation we know for now. One base anim to sit, then two to eat
  'chaireatingsoupstart': 'IdleChairEnterInstant',
  'idleeatsoup': 'IdleChairEnterInstant',

  // No need to re-play the animation, use instant variant for spawning actors
  // This is not essential, but makes the sync feel more smooth. The list is not complete.
  'idlechairrightenter': 'IdleChairEnterInstant',
  'idlechairleftenter': 'IdleChairEnterInstant',
  'idlechairfrontenter': 'IdleChairEnterInstant',

  // Untested yet looks correct
  'idlesnowelfprincefireandforget': 'IdleSnowElfPrinceChairEnterInstant',
  'idletablemugenter': 'IdleTableEnterInstant',
  'idletabledrinkenter': 'IdleTableEnterInstant',
  'idletabledrinkandmugenter': 'IdleTableEnterInstant'
};

// unclassified:

// IdleChairEnterInstant
// IdleChairEnterStart
// IdleChairEnterStop
// IdleChairEnterToSit
// IdleChairExitStart
// IdleChairExitToStand
// IdleChairSitting
// IdleLeftChairEnterStart
// ChairIdle
// IdleRightChairEnterStart
// IdleLeftChairEnterStart

const isIdle = (animEventName: string) => {
  const animEventNameLowerCase = animEventName.toLowerCase();
  return (
    animEventNameLowerCase === "motiondrivenidle" ||
    (animEventNameLowerCase.startsWith("idle") &&
      animEventNameLowerCase !== "idlestop" &&
      animEventNameLowerCase !== "idleforcedefaultstate")
  );
};

export const applyAnimation = (
  refr: ObjectReference,
  anim: Animation,
  state: AnimationApplyState
): void => {
  if (state.lastNumChanges === anim.numChanges) {
    return;
  }
  state.lastNumChanges = anim.numChanges;

  if (state.useAnimOverrides) {
    const animOverride = animOverridesLowerCase[anim.animEventName.toLowerCase()];
    if (animOverride !== undefined) {
      anim.animEventName = animOverride;
    }
  }

  const animEventNameLowerCase = anim.animEventName.toLowerCase();

  if (isIdle(anim.animEventName)) {
    allowedIdles.push([refr.getFormID(), anim.animEventName]);
  }

  const ac = Actor.from(refr);

  if (anim.animEventName === "SkympFakeEquip") {
    if (ac) {
      applyWeapDrawn(ac, true);
    }
    return;
  }

  if (anim.animEventName === "SkympFakeUnequip") {
    if (ac) {
      applyWeapDrawn(ac, false);
    }
    return;
  }

  if (anim.animEventName === "Ragdoll") {
    if (ac) {
      if (storage["animationFunc1Set"] === true) {
        // @ts-ignore
        storage["animationFunc1"](ac);
      } else {
        const refrId = refr.getFormID();
        if (!isKnockedDown(refrId)) {
          relayedKnocks.set(refrId, Date.now());
          ac.pushActorAway(ac, 0);
        }
        ac.setActorValue("Variable10", -1000);
      }
    }
    return;
  }

  if (refsWithDefaultAnimsDisabled.has(refr.getFormID())) {
    if (animEventNameLowerCase.includes("attack")) {
      allowedAnims.add(refr.getFormID() + ":" + anim.animEventName);
    }
  }

  if (animEventNameLowerCase === "staggerstart") {
    relayedStaggers.add(refr.getFormID());
  }

  Debug.sendAnimationEvent(refr, anim.animEventName);

  if (anim.animEventName === "GetUpBegin") {
    const refrId = refr.getFormID();
    Utility.wait(1).then(() => {
      const ac = Actor.from(Game.getFormEx(refrId));
      if (ac) {
        ac.setActorValue("Variable10", 1000);
      }
    });
  }

  if (actorSitAnimsLowerCase.find((x) => x === animEventNameLowerCase) !== undefined) {
    setCollision(refr.getFormID(), false);
  }

  if (actorGetUpAnimsLowerCase.find((x) => x === animEventNameLowerCase) !== undefined) {
    setCollision(refr.getFormID(), true);
  }
};

export const setDefaultAnimsDisabled = (
  refrId: number,
  disabled: boolean
): void => {
  if (disabled) {
    refsWithDefaultAnimsDisabled.add(refrId);
  } else {
    refsWithDefaultAnimsDisabled.delete(refrId);
  }
};

// thuum docs/verbs/npc-stream.md: the attack event an action replay is about
// to send through the engine's action entry (view/formView.ts replayActions)
// on a figure or a puppet whose default animations are blanked, admitted once
// by the hook above, as applyAnimation admits a relayed attack event
export const allowAnimationOnce = (refrId: number, animEventName: string): void => {
  if (refsWithDefaultAnimsDisabled.has(refrId) && animEventName.toLowerCase().includes("attack")) {
    allowedAnims.add(refrId + ":" + animEventName);
  }
};

export class AnimationSource {
  constructor(refr: ObjectReference) {
    this.refrId = refr.getFormID();
    // thuum docs/verbs/npc-stream.md (rule 9): the hook pre-matched in
    // native to this one id; a host with a village's residents on its list
    // had a JavaScript crossing per source for every event of every actor
    this.hookId = hooks.sendAnimationEvent.add({
      enter: () => { },
      leave: (ctx) => {
        if (ctx.selfId !== this.refrId) {
          return;
        }

        // thuum docs/verbs/npc-stream.md (ADR-050): for an actor this game
        // hosts, an event the engine sends inside the actor's own action
        // entry is the action's (isPerformingAction, read here while the
        // game thread waits inside the hook): it travels as UpdateAction
        // (services/actionSyncService.ts) and a puppet performs it through
        // the same entry, so the relay drops it, or the puppet would play it
        // twice (P4's H1). The player keeps the name rule below: its melee
        // and voice events feed the server's AnimationSystem
        if (this.refrId !== 0x14 && isPerformingAction(this.refrId)) {
          return;
        }

        if (!ctx.animationSucceeded) {
          // Workaround, see carryAnimSystem.ts in gamemode
          // Case-sensetive check here for better performance
          if (ctx.animEventName !== "OffsetCarryBasketStart") {
            return;
          }
        }
        this.onSendAnimationEvent(ctx.animEventName);
      },
    }, this.refrId, this.refrId);
  }

  // the hook goes with the source (sendInputsService drops a source whose
  // actor left the hosted list)
  dispose(): void {
    hooks.sendAnimationEvent.remove(this.hookId);
  }

  filterMovement(mov: Movement): Movement {
    if (this.weapDrawnBlocker >= Date.now()) {
      mov.isWeapDrawn = true;
    }
    if (this.weapNonDrawnBlocker >= Date.now()) {
      mov.isWeapDrawn = false;
    }

    if (this.sneakBlocker === mov.isSneaking) {
      this.sneakBlocker = null;
    } else if (this.sneakBlocker === true) {
      mov.isSneaking = true;
    } else if (this.sneakBlocker === false) {
      mov.isSneaking = false;
    }

    return mov;
  }

  // thuum docs/verbs/spell-cast.md: the events the graph took, oldest first,
  // one per call (the sender ships one a frame). A concentration cast's start
  // and its release come in one frame, and an observer's figure that gets the
  // release alone streams without raising its hand
  getAnimation(): Animation {
    const next = this.pending.shift();
    if (next) {
      this.last = next;
    }
    return this.last;
  }

  private onSendAnimationEvent(animEventName: string) {
    // thuum docs/verbs/npc-puppet.md: the movement controller's own events
    // (moveStart, moveStop, the turns) are not relayed for any actor. A
    // figure's keep-offset AI makes its own; a puppet's come from the
    // engine's Speed and Direction channels once the detour feeds them
    // (docs/research/m3/re-puppet-locomotion.md, train-33), and a relayed
    // moveStop landing while the fed speed stays above the threshold would
    // leave its graph idle until the next crossing. Train-32 relayed them
    // for an NPC source and they arrived but changed nothing
    // (x-npc-legs 20261022-190831).
    if (ignoredAnims.has(animEventName)) {
      return;
    }

    const lower = animEventName.toLowerCase();

    // thuum docs/verbs/cast-action.md: a hand's cast events (MRh_, MLh_,
    // DualMagic_ starts, ready, release and the Equipped exit) travel as the
    // actions that sent them (UpdateAction, services/actionSyncService.ts),
    // which a figure performs through the engine's own entry; relayed as
    // events too they would play twice, and the Equipped exit, rewritten
    // below into SkympFakeEquip, never reached a figure's graph at all
    // (x-flames-stop 20261015-031410: the figure cast for 11 s after the
    // caster stopped, playtest eighteen's first finding)
    if (isHandCastEvent(animEventName)) {
      return;
    }

    const isTorchEvent = lower.includes("torch");
    if (animEventName.toLowerCase().includes("unequip") && !isTorchEvent) {
      this.weapNonDrawnBlocker = Date.now() + 300;
      animEventName = "SkympFakeUnequip";
    } else if (animEventName.toLowerCase().includes("equip") && !isTorchEvent) {
      this.weapDrawnBlocker = Date.now() + 300;
      animEventName = "SkympFakeEquip";
    }

    if (animEventName === "SneakStart") {
      this.sneakBlocker = true;
      return;
    }
    if (animEventName === "SneakStop") {
      this.sneakBlocker = false;
      return;
    }

    this.numChanges++;
    this.pending.push({ numChanges: this.numChanges, animEventName });
    if (this.pending.length > maxPendingAnimations) {
      this.pending.shift();
    }
  }

  private refrId = 0;
  private hookId = 0;
  private numChanges = 0;
  private pending = new Array<Animation>();
  private last: Animation = { numChanges: 0, animEventName: "" };

  private weapNonDrawnBlocker = 0;
  private weapDrawnBlocker = 0;
  private sneakBlocker: boolean | null = null;
}

// a burst longer than this keeps its newest events (the old relay kept one)
const maxPendingAnimations = 8;

const ignoredAnims = new Set<string>([
  "moveStart",
  "moveStop",
  "turnStop",
  "CyclicCrossBlend",
  "CyclicFreeze",
  "TurnLeft",
  "TurnRight",
]);

// thuum docs/verbs/npc-stream.md: the movement controller's own events, which
// no channel carries for an NPC (a puppet's locomotion is the engine's Speed
// and Direction channels fed by the detour); the action stream drops the
// actions that send them too (services/actionSyncService.ts)
export const isMovementControllerEvent = (animEventName: string): boolean => ignoredAnims.has(animEventName);

export const setupHooks = (): void => {
  // a figure's own knock in this game (not the relayed handler's push) marks
  // it down until its get-up; filtered in native to figures and the two
  // events (rule 9)
  hooks.sendAnimationEvent.add({
    enter: () => { },
    leave: (ctx) => {
      if (!ctx.animationSucceeded) return;
      const relayedAt = relayedKnocks.get(ctx.selfId);
      relayedKnocks.delete(ctx.selfId);
      if (relayedAt !== undefined && Date.now() - relayedAt <= relayedKnockMs) return;
      markKnockedDown(ctx.selfId);
    },
  }, 0xff000000, 0xffffffff, "Ragdoll");
  hooks.sendAnimationEvent.add({
    enter: () => { },
    leave: (ctx) => {
      if (ctx.animationSucceeded) clearKnockedDown(ctx.selfId);
    },
  }, 0xff000000, 0xffffffff, "GetUpBegin");

  // thuum docs/verbs/npc-puppet.md (x-npc-legs 20261022-202106, Alvor): a
  // puppet's graph took its host's chair enter but REFUSED the relayed
  // IdleChairExitStart, stayed in the furniture state, which consumes no
  // Speed, and the relay carried a sitting body across the yard. A refused
  // furniture exit on a puppet (an actor from the game files this game does
  // not run) is answered with the graph's default state on the next update,
  // the Papyrus-side reset the engine offers (IdleForceDefaultState, the one
  // idle isIdle lets through). Filtered in native to game-file ids and the
  // exit idles (rule 9). HYPOTHESIS until a probe catches a refused exit.
  hooks.sendAnimationEvent.add({
    enter: () => { },
    leave: (ctx) => {
      if (ctx.animationSucceeded || !isPuppet(ctx.selfId)) return;
      const refrId = ctx.selfId;
      once("update", () => {
        const refr = ObjectReference.from(Game.getFormEx(refrId));
        if (refr && isPuppet(refrId)) {
          Debug.sendAnimationEvent(refr, "IdleForceDefaultState");
        }
      });
    },
  }, 0, 0xfeffffff, "*ExitStart");

  hooks.sendAnimationEvent.add({
    enter: (ctx) => {
      if (refsWithDefaultAnimsDisabled.has(ctx.selfId)) {
        if (ctx.animEventName.toLowerCase().includes("attack")) {
          const animKey = ctx.selfId + ":" + ctx.animEventName;
          if (allowedAnims.has(animKey)) {
            allowedAnims.delete(animKey);
          } else {
            // the actor named, so a seat's count can be scoped to one (b-npc-stream-actions
            // 20261027-120242 counted a creature's blocked attack at c2's connect point against the bandit)
            printConsole("block anim " + ctx.animEventName + " on " + ctx.selfId.toString(16));
            return (ctx.animEventName = "");
          }
        }
      }

      // ShowRaceMenu forces this anim
      if (ctx.animEventName === "OffsetBoundStandingPlayerInstant") {
        return (ctx.animEventName = "");
      }

      // Disable idle animations for 0xff actors
      if (ctx.selfId < 0xff000000) {
        return;
      }
      if (isIdle(ctx.animEventName)) {
        const i = allowedIdles.findIndex((pair) => {
          return pair[0] === ctx.selfId && pair[1] === ctx.animEventName;
        });
        i === -1 ? (ctx.animEventName = "") : allowedIdles.splice(i, 1);
      }
    },
    leave: () => { },
  });
};
