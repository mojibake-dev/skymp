import {
  ObjectReference,
  Actor,
  Game,
  TESModPlatform,
  Debug
} from "skyrimPlatform";
import { RespawnNeededError } from "../lib/errors";
import { Movement, RunMode, AnimationVariables, Transform, NiPoint3 } from "./movement";
import { ObjectReferenceEx } from "../extensions/objectReferenceEx";
import { SpApiInteractor } from "../services/spApiInteractor";
import { isKnockedDown } from "./knockState";

const sqr = (x: number) => x * x;

export const applyMovement = (refr: ObjectReference, m: Movement, isMyClone?: boolean): void => {
  if (teleportIfNeed(refr, m)) {
    return;
  }

  // Z axis isn't useful here
  const acX = refr.getPositionX();
  const acY = refr.getPositionY();
  const lagUnitsNoZ = Math.round(Math.sqrt(sqr(m.pos[0] - acX) + sqr(m.pos[1] - acY)));

  if (isMyClone === true) {
    SpApiInteractor.getControllerInstance().emitter.emit("newLocalLagValueCalculated", { lagUnitsNoZ });
  }

  translateTo(refr, m);

  const ac = Actor.from(refr);
  if (!ac) {
    return;
  }

  let lookAt = null;
  if (m.lookAt) {
    try {
      lookAt = Game.findClosestActor(
        m.lookAt[0],
        m.lookAt[1],
        m.lookAt[2],
        128
      );
    } catch (e) {
      lookAt = null;
    }
  }

  if (lookAt) {
    ac.setHeadTracking(true);
    ac.setLookAt(lookAt, false);
  } else {
    ac.setHeadTracking(false);
  }

  // ac.stopCombat();
  ac.blockActivation(true);

  keepOffsetFromActor(ac, m);

  applySprinting(ac, m.runMode === "Sprinting");
  applyBlocking(ac, m);
  applySneaking(ac, m.isSneaking);
  applyWeapDrawn(ac, m.isWeapDrawn);
  applyHealthPercentage(ac, m.healthPercentage);

  SpApiInteractor.getControllerInstance().emitter.emit("applyDeathStateEvent", { actor: ac, isDead: m.isDead });
};

const keepOffsetFromActor = (ac: Actor, m: Movement) => {
  let offsetAngle = m.rot[2] - ac.getAngleZ();
  if (Math.abs(offsetAngle) < 5) {
    offsetAngle = 0;
  }

  if (m.runMode === "Standing") {
    return ac.keepOffsetFromActor(ac, 0, 0, 0, 0, 0, offsetAngle, 1, 1);
  }
  const offset = [
    3 * Math.sin((m.direction / 180) * Math.PI),
    3 * Math.cos((m.direction / 180) * Math.PI),
    getOffsetZ(m.runMode),
  ];

  ac.keepOffsetFromActor(
    ac,
    offset[0],
    offset[1],
    offset[2],
    0,
    0,
    offsetAngle,
    m.runMode === "Walking" ? 2048 : 1,
    1,
  );
};

const getOffsetZ = (runMode: RunMode) => {
  switch (runMode) {
    case "Walking":
      return -512;
    case "Running":
      return -1024;
  }
  return 0;
};

const applySprinting = (ac: Actor, isSprinting: boolean) => {
  if (ac.isSprinting() != isSprinting) {
    Debug.sendAnimationEvent(ac, isSprinting ? "SprintStart" : "SprintStop");
  }
};

const applyBlocking = (ac: Actor, m: AnimationVariables) => {
  if (ac.getAnimationVariableBool("IsBlocking") != m.isBlocking) {
    Debug.sendAnimationEvent(ac, m.isBlocking ? "BlockStart" : "BlockStop");
    Debug.sendAnimationEvent(ac, m.isSneaking ? "SneakStart" : "SneakStop");
  }
};

// thuum docs/verbs/movement-relay.md (playtests thirteen and fourteen,
// x-crouch-magic2 20261012-235927): with magic readied the graph's IsSneaking
// stays false and a figure's isSneaking() never turns true, so reading them
// could not tell the figure still crouched and SneakStop was never sent: the
// figure stood only when its hands were sheathed. The last state sent to each
// figure is kept, so a change of the report sends its event whatever the
// graph says. A graph that disagrees with an unchanged report (a figure whose
// 3D reloaded) is told again at most once a second: with magic readied it
// never agrees, and resending every update sent a crouched figure SneakStart
// every 100 to 130 ms (x-crouch-latency2 20261013-060851).
const sneakSent = new Map<number, { isSneaking: boolean; at: number }>();
const sneakResendMs = 1000;

const applySneaking = (ac: Actor, isSneaking: boolean) => {
  const id = ac.getFormID();
  const sent = sneakSent.get(id);
  const now = Date.now();
  if (sent && sent.isSneaking === isSneaking) {
    const currentIsSneaking =
      ac.isSneaking() || ac.getAnimationVariableBool("IsSneaking");
    if (currentIsSneaking === isSneaking || now - sent.at < sneakResendMs) {
      return;
    }
  }
  Debug.sendAnimationEvent(ac, isSneaking ? "SneakStart" : "SneakStop");
  sneakSent.set(id, { isSneaking, at: now });
};

export const applyWeapDrawn = (ac: Actor, isWeapDrawn: boolean): void => {
  if (ac.isWeaponDrawn() !== isWeapDrawn) {
    TESModPlatform.setWeaponDrawnMode(ac, isWeapDrawn ? 1 : 0);
  }
};

const applyHealthPercentage = (ac: Actor, healthPercentage: number) => {
  const currentPercentage = ac.getActorValuePercentage('health');
  if (currentPercentage === healthPercentage) {
    return;
  }

  const currentMax = ac.getBaseActorValue('health');
  const deltaPercentage = healthPercentage - currentPercentage;
  const k = 0.25;
  if (deltaPercentage > 0) {
    ac.restoreActorValue('health', deltaPercentage * currentMax * k);
  } else if (deltaPercentage < 0) {
    ac.damageActorValue('health', deltaPercentage * currentMax * k);
  }
};

// Use global temp var to avoid allocation of an array on each translateTo
const gTempTargetPos: NiPoint3 = [0, 0, 0];

// thuum docs/verbs/cast-teleport.md (playtest fourteen: a teleporting player
// slid across the other screen): a figure further from its report than any
// run covers between two reports (130 ms apart; Whirlwind Sprint's three
// words spread 2140 units over several, docs/verbs/movement-speed.md) moved
// by a jump, and is put there instead of glided across in 0.2 s
const jumpDistance = 1024;

const translateTo = (refr: ObjectReference, m: Movement) => {
  let time = 0.2;
  if (m.isInJumpState || m.runMode !== "Standing") {
    time = 0.2;
  }

  // Local lag compensation
  // TODO: Remove "|| 0" hack (added to support old MpClientPlugin)
  const distanceAdd = (m.speed || 0) * time;
  const direction = m.rot[2] + m.direction;
  gTempTargetPos[0] = m.pos[0];
  gTempTargetPos[1] = m.pos[1];
  gTempTargetPos[2] = m.pos[2];

  // We do not want to add pos in case of standing-jumping
  if (m.runMode !== "Standing") {
    gTempTargetPos[0] += Math.sin(direction / 180 * Math.PI) * distanceAdd;
    gTempTargetPos[1] += Math.cos(direction / 180 * Math.PI) * distanceAdd;
  }

  const refrRealPos = ObjectReferenceEx.getPos(refr);
  const distance = ObjectReferenceEx.getDistance(refrRealPos, gTempTargetPos);

  const speed = distance / time;

  const angleDiff = Math.abs(m.rot[2] - refr.getAngleZ());
  if (
    m.runMode !== "Standing" ||
    m.isInJumpState ||
    ObjectReferenceEx.getDistanceNoZ(refrRealPos, gTempTargetPos) > 8 ||
    angleDiff > 80 ||
    Actor.from(refr)?.getSitState() === 3
  ) {
    const actor = Actor.from(refr);
    // down: by its player's relayed Ragdoll (Variable10), or by this game's
    // own knock until its get-up (sync/knockState.ts, the push's flight)
    if (actor && (actor.getActorValue("Variable10") < -999 || isKnockedDown(refr.getFormID()))) {
      return;
    }

    if (!actor || !actor.isDead()) {
      if (distance > jumpDistance) {
        refr.setPosition(gTempTargetPos[0], gTempTargetPos[1], gTempTargetPos[2]);
        refr.setAngle(m.rot[0], m.rot[1], m.rot[2]);
        return;
      }
      refr.translateTo(
        gTempTargetPos[0],
        gTempTargetPos[1],
        gTempTargetPos[2],
        m.rot[0],
        m.rot[1],
        m.rot[2],
        speed,
        0
      );
    }
  }
};

const teleportIfNeed = (refr: ObjectReference, m: Transform) => {
  if (
    isInDifferentWorldOrCell(refr, m.worldOrCell) ||
    (!refr.is3DLoaded() && isInDifferentExteriorCell(refr, m.pos))
  ) {
    throw new RespawnNeededError("needs to be respawned");
  }
  return false;
};

const cellWidth = 4096;

const isInDifferentExteriorCell = (refr: ObjectReference, pos: NiPoint3) => {
  const currentPos = ObjectReferenceEx.getPos(refr);
  const playerPos = ObjectReferenceEx.getPos(Game.getPlayer() as Actor);
  const targetDistanceToPlayer = ObjectReferenceEx.getDistance(playerPos, pos);
  const currentDistanceToPlayer = ObjectReferenceEx.getDistance(playerPos, currentPos);
  return currentDistanceToPlayer > cellWidth && targetDistanceToPlayer <= cellWidth;
};

const isInDifferentWorldOrCell = (
  refr: ObjectReference,
  worldOrCell: number
) => {
  return worldOrCell !== ObjectReferenceEx.getWorldOrCell(refr);
};
