import {
  ObjectReference,
  Actor,
  Game,
  TESModPlatform,
  Debug, setPuppetLocomotion, printConsole } from "skyrimPlatform";
import { RespawnNeededError } from "../lib/errors";
import { Movement, RunMode, AnimationVariables, Transform, NiPoint3 } from "./movement";
import { ObjectReferenceEx } from "../extensions/objectReferenceEx";
import { SpApiInteractor } from "../services/spApiInteractor";
import { isKnockedDown } from "./knockState";

const sqr = (x: number) => x * x;

// `settle`: the view's periodic re-apply of an unchanged record (no new sample
// from the actor's game), which brings a standing figure or puppet back to
// the record from one unit; a fresh sample keeps the eight-unit deadband
export const applyMovement = (refr: ObjectReference, m: Movement, isMyClone?: boolean, puppet?: boolean, settle?: boolean): void => {
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

  translateTo(refr, m, settle === true);

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

  if (puppet) {
    applyPuppetPose(ac, m);
  } else {
    keepOffsetFromActor(ac, m);
  }

  applySprinting(ac, m.runMode === "Sprinting");
  applyBlocking(ac, m);
  applySneaking(ac, m.isSneaking);
  applyWeapDrawn(ac, m.isWeapDrawn);
  applyHealthPercentage(ac, m.healthPercentage);

  SpApiInteractor.getControllerInstance().emitter.emit("applyDeathStateEvent", { actor: ac, isDead: m.isDead });
};

// the shortest signed turn from heading b to heading a, in (-180, 180]
const angleDelta = (a: number, b: number): number => ((((a - b + 180) % 360) + 360) % 360) - 180;

// thuum docs/verbs/npc-puppet.md (ADR-045, Rule B; H2, H11): a puppet's
// heading is written absolutely from the host's sample. A keep-offset on the
// actor itself with a nonzero angle is a turn that renews itself every frame
// (the planner agent's goal is the actor's own angle plus the offset, REA
// 2026-10-21): playtest thirty's spinning wolves. Its graph is fed the host's
// Speed and Direction so the walk cycle plays on the re-admitted animation
// job (H2; the AI that wrote them is off on a puppet).
const applyPuppetPose = (ac: Actor, m: Movement) => {
  if (Math.abs(angleDelta(m.rot[2], ac.getAngleZ())) >= 1) {
    ac.setAngle(m.rot[0], m.rot[1], m.rot[2]);
  }
  // docs/research/m3/re-puppet-locomotion.md: the graph's Speed and
  // Direction are data channels the engine refreshes every tick from the
  // movement step's fields, so a SetAnimationVariableFloat here lived less
  // than a frame (x-npc-legs 20261022-190831: every input read 0 under
  // per-sample writes). The sample goes to the native side, which writes
  // the fields every frame in the movement-step detour; the engine's own
  // moveStart, moveStop and direction actions follow from them (HYPOTHESIS)
  // the host's Speed as reported, whatever the run mode: getRunMode answers
  // "Standing" for any actor holding a furniture reference (movementGet.ts),
  // which Alvor keeps between the forge's and the tanning rack's markers, so
  // a Standing gate here fed his puppet 0 while the record walked him
  // between them (x-npc-legs 20261022-204726: Speed 0 through two walks at
  // 51 to 86 units/s, every event taken); the host's Speed channel is 0
  // when an actor truly stands
  setPuppetLocomotion(ac.getFormID(), m.speed, m.direction, 0);
};

const keepOffsetFromActor = (ac: Actor, m: Movement) => {
  // the turn the short way round: an unwrapped difference in (-360, 360)
  // sent a figure the long way (npc-puppet's read of playtest thirty)
  let offsetAngle = angleDelta(m.rot[2], ac.getAngleZ());
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

// the draw state last asked of each actor and when: the latent native is
// asked again only when the wanted state changes or a second has passed with
// the actor still not in it (playtest twenty: a figure whose state never
// matched its movement had the native called on every movement message, 20 a
// second, once the relayed drawn flag began to expire)
const weapDrawnAsked = new Map<number, { drawn: boolean; at: number }>();

export const applyWeapDrawn = (ac: Actor, isWeapDrawn: boolean): void => {
  if (ac.isWeaponDrawn() === isWeapDrawn) {
    return;
  }
  const id = ac.getFormID();
  const last = weapDrawnAsked.get(id);
  const now = Date.now();
  if (last && last.drawn === isWeapDrawn && now - last.at < 1000) {
    return;
  }
  weapDrawnAsked.set(id, { drawn: isWeapDrawn, at: now });
  TESModPlatform.setWeaponDrawnMode(ac, isWeapDrawn ? 1 : 0);
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

// thuum docs/verbs/cast-teleport.md (Eli, playtest twenty-two: "teleports
// slide behavior was never fully fixed"): a cast teleport's look pops the
// figure to the landing by the server's SetPosition, which rides the
// reliable channel, while a movement report its caster sent before the
// landing rides the unreliable one and can arrive after the pop: glided to,
// it pulls the figure back toward the start, then the next reports glide it
// forward again (x-teleport-look 20261017-015128: about 40 to 85 units back
// inside 250 ms, then a glide of about 80). A figure that jumped further than
// popDistance since the last report, to somewhere this code did not send it,
// was placed by something else: a pop. For popWindowMs after it, a report
// nearer the figure's place before the pop than the pop itself is such a
// stale report and is left unapplied
const popDistance = 256;
const popWindowMs = 600;
const lastSent = new Map<number, NiPoint3>();
const lastReal = new Map<number, NiPoint3>();
const pops = new Map<number, { at: number; pos: NiPoint3; from: NiPoint3 }>();

const isStaleAfterPop = (refrId: number, real: NiPoint3, target: NiPoint3): boolean => {
  const now = Date.now();
  const sent = lastSent.get(refrId);
  const before = lastReal.get(refrId);
  lastReal.set(refrId, [real[0], real[1], real[2]]);
  if (
    sent && before &&
    ObjectReferenceEx.getDistance(real, before) > popDistance &&
    ObjectReferenceEx.getDistance(real, sent) > popDistance
  ) {
    pops.set(refrId, { at: now, pos: [real[0], real[1], real[2]], from: [before[0], before[1], before[2]] });
  }
  const pop = pops.get(refrId);
  if (!pop) {
    return false;
  }
  if (now - pop.at > popWindowMs) {
    pops.delete(refrId);
    return false;
  }
  return ObjectReferenceEx.getDistance(target, pop.from) < ObjectReferenceEx.getDistance(target, pop.pos);
};

const translateTo = (refr: ObjectReference, m: Movement, settle = false) => {
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
  // thuum docs/verbs/spell-cast.md (b-duel 215524, 222125, 224131, 224453 on
  // train-33 against 192438 and 223749): a stagger's push left c2's figure 6
  // to 7.6 units from the record on the caster's screen, under the 8-unit
  // deadband a standing figure keeps against a fresh sample's jitter, and
  // with c2 standing still no sample ever moved it back: the 2 s re-apply
  // ran the same gate, and the Flames stream aimed at the record missed
  // half its ticks. The periodic re-apply settles from one unit instead
  // The re-apply's `settle` never comes for a player's figure: its game
  // reports movement at every tick while it stands, so every apply is a
  // fresh sample (x-duel-stream 20261022-233951 on 8a581ede: the figure 6.9
  // units off for ten seconds). So the record's own stillness decides: a
  // target equal to the last one this code sent (the player stands where it
  // stood) has no jitter to guard against, and a standing figure settles
  // onto it from one unit; a moving target keeps the eight
  const refrId0 = refr.getFormID();
  const sentBefore = lastSent.get(refrId0);
  const recordStill = sentBefore !== undefined && ObjectReferenceEx.getDistance(sentBefore, gTempTargetPos) < 0.5;
  const deadband = settle || recordStill ? 1 : 8;
  const offBy = ObjectReferenceEx.getDistanceNoZ(refrRealPos, gTempTargetPos);
  if (
    m.runMode !== "Standing" ||
    m.isInJumpState ||
    offBy > deadband ||
    angleDiff > 80 ||
    Actor.from(refr)?.getSitState() === 3
  ) {
    if (m.runMode === "Standing" && offBy > deadband && deadband === 1) {
      // the settle, on the console for the lab's read (rare: a standing figure
      // or puppet found off its still record)
      printConsole(`settle ${refrId0.toString(16)}: ${offBy.toFixed(1)} units from (${refrRealPos[0].toFixed(0)}, ${refrRealPos[1].toFixed(0)}) to (${gTempTargetPos[0].toFixed(0)}, ${gTempTargetPos[1].toFixed(0)})`);
    }
    const actor = Actor.from(refr);
    // down: by its player's relayed Ragdoll (Variable10), or by this game's
    // own knock until its get-up (sync/knockState.ts, the push's flight)
    if (actor && (actor.getActorValue("Variable10") < -999 || isKnockedDown(refr.getFormID()))) {
      return;
    }

    if (!actor || !actor.isDead()) {
      const refrId = refr.getFormID();
      if (isStaleAfterPop(refrId, refrRealPos, gTempTargetPos)) {
        return;
      }
      lastSent.set(refrId, [gTempTargetPos[0], gTempTargetPos[1], gTempTargetPos[2]]);
      // thuum docs/verbs/cast-teleport.md (the teleport snap): a report the
      // server vouched for as a jump (ActorJump) places at any distance
      if (distance > jumpDistance || m.placed) {
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
