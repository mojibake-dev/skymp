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
  setCollision
} from "skyrimPlatform";
import { Movement } from "./movement";
import { applyWeapDrawn } from "./movementApply";
import { clearKnockedDown, isKnockedDown, markKnockedDown } from "./knockState";

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

// thuum docs/verbs/spell-cast.md (the concentration pose): a hand's spell
// start lands on a figure's graph only while the figure holds its caster's
// casting state. The engine's caster sets its hand's want-cast variable and
// starts the cast's motion a frame after the graph has answered it; the
// relay delivers the start first and the variables after (with the cast, or
// the caster's 500 ms snapshot), so the figure refused the start and never
// made the grab and push. The same start, sent to the figure while it held
// the relayed variables, was taken (x-figure-cast3 20261014-182649: refused
// at 1189 ms before the variables came at 1482, taken at 2073, the figure
// casting: IsCastingRight 1, NotCasting 0, iState 10). The want variable
// alone is not that state: set by itself it holds the graph at iState 10 for
// one frame, and starts sent 16 to 266 ms after it were refused
// (x-figure-cast2 20261014-183340). So the view holds a hand's start, and
// the events behind it, until the figure is casting with that hand
// (IsCastingRight or IsCastingLeft, the relayed state) since an earlier
// frame.
const castStartVariables = (animEventNameLowerCase: string): string[] => {
  if (!animEventNameLowerCase.includes("spell") || !animEventNameLowerCase.endsWith("start")) {
    return [];
  }
  if (animEventNameLowerCase.startsWith("mrh_")) return ["IsCastingRight"];
  if (animEventNameLowerCase.startsWith("mlh_")) return ["IsCastingLeft"];
  if (animEventNameLowerCase.startsWith("dualmagic_")) return ["IsCastingRight", "IsCastingLeft"];
  return [];
};

// null: the event never waits; true: the figure is not casting with the
// start's hand yet; false: it is
export const castStartWaiting = (refr: ObjectReference, animEventName: string): boolean | null => {
  const variables = castStartVariables(animEventName.toLowerCase());
  if (variables.length === 0) return null;
  return !variables.some((name) => refr.getAnimationVariableBool(name));
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

export class AnimationSource {
  constructor(refr: ObjectReference) {
    this.refrId = refr.getFormID();
    hooks.sendAnimationEvent.add({
      enter: () => { },
      leave: (ctx) => {
        if (ctx.selfId !== this.refrId) {
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
    });
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
    if (ignoredAnims.has(animEventName)) {
      return;
    }

    const lower = animEventName.toLowerCase();

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

  hooks.sendAnimationEvent.add({
    enter: (ctx) => {
      if (refsWithDefaultAnimsDisabled.has(ctx.selfId)) {
        if (ctx.animEventName.toLowerCase().includes("attack")) {
          const animKey = ctx.selfId + ":" + ctx.animEventName;
          if (allowedAnims.has(animKey)) {
            allowedAnims.delete(animKey);
          } else {
            printConsole("block anim " + ctx.animEventName);
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
