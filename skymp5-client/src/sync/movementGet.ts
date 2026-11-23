import { FormModel } from '../view/model';
import { ObjectReference, Actor, TESModPlatform, getActorLocomotion } from "skyrimPlatform";
import { NiPoint3, Movement, RunMode } from "./movement";
import { ObjectReferenceEx } from '../extensions/objectReferenceEx';

class PlayerCharacterSpeedCalculator {
  static savePosition(pos: NiPoint3, worldOrCell: number) {
    this.lastPcPos = pos;
    this.lastPcPosCheck = Date.now();
    this.lastPcWorldOrCell = worldOrCell;
  }

  static getSpeed(currentPos: NiPoint3, worldOrCell: number) {
    if (this.lastPcPosCheck === -1) {
      return 0;
    }

    const timeDeltaSec = (Date.now() - this.lastPcPosCheck) / 1000;
    if (timeDeltaSec > 5) return 0; // Too inaccurate
    if (timeDeltaSec === 0) return 0; // Division by zero
    if (worldOrCell !== this.lastPcWorldOrCell) {
      return 0;
    }

    const distance = ObjectReferenceEx.getDistance(currentPos, this.lastPcPos);
    return distance / timeDeltaSec;
  }

  private static lastPcPos: NiPoint3 = [0, 0, 0];
  private static lastPcPosCheck = -1;
  private static lastPcWorldOrCell = 0;
}

// thuum tracker #111 (playtest 53, Eli: "they slide in from somewhere ...
// and then they slide around and arent animated"): an actor this game
// simulates reports its engine's own locomotion, the speed and move
// direction its movement step left in MiddleHighProcessData, the fields the
// watcher's Move detour writes on the puppet (the fork's Skyrim Platform
// getActorLocomotion, PuppetHooks.cpp's sample beside its write): one rule
// for a game-file actor, a server-made copy and a bound summon alike. The
// graph's variables were the source before: "Speed" for a game-file actor
// (x-npc-legs, docs/verbs/npc-puppet.md), "SpeedSampled" for any runtime id,
// a summon's own among them (ff000c32 in playtest 53), and "Direction" for
// the direction; getRunMode reads Standing from a SpeedSampled of 0. The
// atronach's did read 0: Ariadne's copy ff000f49 of rotfern's ff000003 was
// moved by the receiver's settle alone, seven "settle ff000f49" lines in
// 30 s while it crossed about 380 units (playtest 53 c2 console :11002 to
// :11269), a line movementApply prints for a Standing record only, and a
// summon holds no furniture. The recorder's variable snapshot reads nothing
// on a creature (c2.log:2778: "graph project 'AtronachFlame' (variable set
// of 75) is not the master behaviour: no variables read or written"), so
// the traces' empty creature series are no read of the graph; the per-name
// Papyrus read used here is unread for a creature's Speed. The export
// answers a puppet too (the relay's feed as the detour wrote it); this game
// samples only the actors it simulates and its player (sendInputsService),
// so the answer here is the engine's own. null (a platform without the
// step's detour, or no sample yet) keeps the graph reads below
type EngineLocomotion = { speed: number; directionDeg: number; turnRateDeg: number; ageMs: number };
const engineLocomotionOf = (refr: ObjectReference): EngineLocomotion | null => {
  const id = refr.getFormID();
  if (id === 0x14 || typeof getActorLocomotion !== "function") {
    return null;
  }
  const sample = getActorLocomotion(id);
  if (!sample) {
    return null;
  }
  // a sample older than the step's cadence is a step that has not run (the
  // actor left the high process): its engine moved it no further
  if (sample.ageMs > ENGINE_SAMPLE_STALE_MS) {
    return { speed: 0, directionDeg: 0, turnRateDeg: 0, ageMs: sample.ageMs };
  }
  return sample;
};
// the platform's own staleness for a puppet's fed sample (Puppets.h
// kLocomotionStaleMs, 400)
const ENGINE_SAMPLE_STALE_MS = 400;
// the Speed channel's own threshold for moveStart and moveStop
// (docs/research/m3/re-puppet-locomotion.md, the Speed poll's row: 5.0)
const ENGINE_MOVE_THRESHOLD = 5;

export const getMovement = (refr: ObjectReference, form?: FormModel): Movement => {
  const ac = Actor.from(refr);
  const engine = engineLocomotionOf(refr);
  // each engine value read once per sample
  const id = refr.getFormID();
  const dead = !!(ac && ac.isDead());

  // It is running for ObjectReferences because Standing
  // Doesn't lead to translateTo call
  const runMode = ac ? getRunMode(ac, engine) : "Running";

  let healthPercentage = ac && ac.getActorValuePercentage("health");
  if (dead) {
    healthPercentage = 0;
  }
  // thuum docs/verbs/npc-puppet.md (x-npc-death 20261023-054215): under the
  // engine's deferred kill an actor's health can sit below zero (a script,
  // a fall) and read as a negative percentage; the wire carries [0, 1] and
  // the validator refuses anything else
  if (typeof healthPercentage === "number") {
    healthPercentage = Math.min(1, Math.max(0, healthPercentage));
  }

  let lookAt: undefined | NiPoint3 = undefined;
  if (id !== 0x14) {
    const combatTarget = ac?.getCombatTarget();
    if (combatTarget) {
      lookAt = [
        combatTarget.getPositionX(),
        combatTarget.getPositionY(),
        combatTarget.getPositionZ(),
      ];
    }
  }

  const pos = ObjectReferenceEx.getPos(refr);

  let speed;
  if (engine) {
    speed = engine.speed;
  } else if (id !== 0x14 && id < 0xff000000) {
    // thuum docs/verbs/npc-puppet.md (x-npc-legs 20261022-202106 against the
    // host baseline 195031): an actor from the game files reports Speed, the
    // graph's own locomotion input (the movement controller's speed, about 80
    // at a walk and 220 at a run), which its puppets' graphs are fed from
    // natively; SpeedSampled runs 1.3 to 1.5 times higher and blended the
    // puppets toward a jog
    speed = refr.getAnimationVariableFloat("Speed");
  } else if (id !== 0x14) {
    speed = refr.getAnimationVariableFloat("SpeedSampled");
  } else {
    // Real players often run into the wall.
    // We need to have zero speed in this case. SpeedSampled doesn't help
    const w = ObjectReferenceEx.getWorldOrCell(refr);
    speed = PlayerCharacterSpeedCalculator.getSpeed(pos, w);
    PlayerCharacterSpeedCalculator.savePosition(pos, w);

    // It's unrealistic speed. It still may happen due to teleports
    if (speed > 2000) {
      speed = 0;
    }
  }

  const worldOrCell = refr.getWorldSpace() || refr.getParentCell();

  return {
    worldOrCell: worldOrCell?.getFormID() || 0,
    pos,
    rot: [refr.getAngleX(), refr.getAngleY(), refr.getAngleZ()],
    runMode: runMode,
    direction: runMode !== "Standing"
      ? (engine ? engine.directionDeg : 360 * refr.getAnimationVariableFloat("Direction"))
      : 0,
    isInJumpState: !!(ac && ac.getAnimationVariableBool("bInJumpState")),
    isSneaking: !!(ac && isSneaking(ac)),
    isBlocking: !!(ac && ac.getAnimationVariableBool("IsBlocking")),
    isWeapDrawn: !!(ac && ac.isWeaponDrawn()),
    isDead: (form?.isDead ?? false) || dead,
    healthPercentage: healthPercentage || 0,
    lookAt,
    speed
  };
}

const isSneaking = (ac: Actor) =>
  ac.isSneaking() || ac.getAnimationVariableBool("IsSneaking");

const getRunMode = (ac: Actor, engine: EngineLocomotion | null): RunMode => {
  if (ac.isSprinting()) {
    return "Sprinting";
  }

  // the engine's own speed where it was read (tracker #111, above), with the
  // Speed channel's moveStart threshold; the graph's SpeedSampled otherwise
  const speed = engine ? engine.speed : ac.getAnimationVariableFloat("SpeedSampled");
  if (engine ? speed < ENGINE_MOVE_THRESHOLD : !speed) {
    return "Standing";
  }

  const furniture = ac.getFurnitureReference();
  if (furniture !== null) return "Standing"; // TODO: Sitting?

  let isRunning = true;
  if (ac.getFormID() == 0x14) {
    if (!TESModPlatform.isPlayerRunningEnabled() || speed < 150)
      isRunning = false;
  } else {
    if (!ac.isRunning() || speed < 150) {
      isRunning = false;
    }
  }

  if (ac.getAnimationVariableFloat("IsBlocking")) {
    isRunning = isSneaking(ac);
  }

  const carryWeight = ac.getActorValue("CarryWeight");
  const totalItemWeight = ac.getTotalItemWeight();
  if (carryWeight < totalItemWeight) {
    isRunning = false;
  }

  return isRunning ? "Running" : "Walking";
};
