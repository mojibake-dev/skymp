import { ObjectReference, Actor, Game, TESModPlatform, Debug, setPuppetLocomotion, printConsole, settings } from "skyrimPlatform";
import { puppetGazeHeld } from "./puppetGaze";
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
  // slice 5: a puppet whose gaze the host's state names keeps that target
  // (PuppetFaceService); the point heuristic is for the rest
  if (puppet && puppetGazeHeld.has(ac.getFormID())) {
    // the gaze is set by the state's handler
  } else if (m.lookAt) {
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
    localGaze.delete(ac.getFormID());
  } else if (!(puppet && puppetGazeHeld.has(ac.getFormID()))) {
    // thuum D50 (playtest 35, Eli: "HIS face and head doesn't move the way
    // hilde's does"): the host's engine tracks heads near ITS player only
    // (H-54d), so a resident far from its host stood stiff on the guest's
    // screen while a live one turned to meet the guest. A puppet with no
    // relayed target looks at the LOCAL player when it is near, as a live
    // NPC's own head tracking would; the radius is a knob
    // (movement.localGazeRadius, 450 units, 0 off) with a hysteresis of a
    // hundred units so the head does not flicker at the edge. A relayed
    // target (above) still wins.
    const id = ac.getFormID();
    const radius = movementKnobs().localGazeRadius;
    const held = localGaze.has(id);
    const player = puppet && radius > 0 ? Game.getPlayer() : null;
    if (player) {
      const d = ObjectReferenceEx.getDistanceNoZ(ObjectReferenceEx.getPos(player), ObjectReferenceEx.getPos(refr));
      if (d < (held ? radius + 100 : radius)) {
        if (!held) {
          ac.setHeadTracking(true);
          ac.setLookAt(player, false);
          localGaze.add(id);
        }
      } else {
        if (held) {
          localGaze.delete(id);
          ac.clearLookAt();
        }
        ac.setHeadTracking(false);
      }
    } else {
      if (held) {
        localGaze.delete(id);
        ac.clearLookAt();
      }
      ac.setHeadTracking(false);
    }
  }

  // ac.stopCombat();
  // a figure's activation is blocked here, per sample as upstream had it; a
  // puppet's is the native demote's (PuppetApi.cpp, playtest 33: no
  // Papyrus-backed call per sample for the village's puppets)
  if (!puppet) {
    ac.blockActivation(true);
  }

  if (puppet) {
    if (ac.isDead()) {
      restBody(ac, m);
    } else {
      restBodyDone.delete(ac.getFormID());
      deadSince.delete(ac.getFormID());
      applyPuppetPose(ac, m);
    }
  } else {
    keepOffsetFromActor(ac, m);
  }

  applySprinting(ac, m.runMode === "Sprinting");
  applyBlocking(ac, m);
  applySneaking(ac, m.isSneaking);
  applyWeapDrawn(ac, m.isWeapDrawn);
  applyHealthPercentage(ac, m.healthPercentage);

  // thuum docs/verbs/npc-fate.md, ADR-048's seam (death-path.md section 2):
  // a game-file actor (an engine reference) dies by the server's word alone,
  // the isDead property the server broadcasts at a death or a respawn, never
  // by the host's relayed sample, which no record stood behind (rule 5); a
  // figure or a 0xff copy keeps the sample's word as before
  if (ac.getFormID() >= 0xff000000) {
    SpApiInteractor.getControllerInstance().emitter.emit("applyDeathStateEvent", { actor: ac, isDead: m.isDead });
  }
};


// thuum docs/verbs/npc-fate.md (ADR-050 decision 2, train 52): a dead
// puppet's body lies where the host's body lies. Each seat's own ragdoll put
// the body somewhere (playtest 34: about 112 units apart), so once the host's
// body has settled (two seconds after this seat saw the death) the host's
// sample places the puppet's body there, once; the ragdoll's pose stays this
// seat's. A sample the host sends while its actor is alive again (a respawn)
// clears the mark.
const deadSince = new Map<number, number>();
const restBodyDone = new Set<number>();
const restBody = (ac: Actor, m: Movement) => {
  const id = ac.getFormID();
  const now = Date.now();
  const since = deadSince.get(id);
  if (since === undefined) {
    deadSince.set(id, now);
    return;
  }
  if (!m.isDead || restBodyDone.has(id) || now - since < 2000) {
    return;
  }
  restBodyDone.add(id);
  const dx = m.pos[0] - ac.getPositionX();
  const dy = m.pos[1] - ac.getPositionY();
  const dz = m.pos[2] - ac.getPositionZ();
  const apart = Math.sqrt(dx * dx + dy * dy + dz * dz);
  if (apart < 64) {
    return;
  }
  ac.setPosition(m.pos[0], m.pos[1], m.pos[2]);
  printConsole(`npc-fate: body ${id.toString(16)} rested at the host's spot, ${apart.toFixed(0)} units away`);
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
  // the fed speed is bounded by what the record itself moved since the
  // previous sample: a talker's small turns report a Speed of 10 to 20 with
  // no displacement, and a puppet fed that walks in place (playtest 32,
  // Embry's conversation pair, 2026-10-22). A record that moved at the
  // host's walk feeds the host's Speed; a still record feeds 0
  const id = ac.getFormID();
  const now = Date.now();
  const prev = puppetTrack.get(id);
  puppetTrack.set(id, { x: m.pos[0], y: m.pos[1], t: now });
  let fed = m.speed;
  if (prev) {
    const dt = Math.max(0.05, (now - prev.t) / 1000);
    const moved = Math.sqrt(sqr(m.pos[0] - prev.x) + sqr(m.pos[1] - prev.y));
    const trackSpeed = moved / dt;
    fed = Math.min(m.speed, trackSpeed * 1.5);
    if (moved < 2) {
      fed = 0;
    }
  }
  setPuppetLocomotion(id, fed, m.direction, 0);
};

// a puppet's record at the previous sample, for the fed speed's bound
const puppetTrack = new Map<number, { x: number; y: number; t: number }>();

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

// thuum docs/verbs/npc-puppet.md (x-npc-death 20261023-054215): the step
// never takes a figure or a puppet to zero. The server's death state is the
// only death; a host's actor floored below zero under the engine's deferred
// kill stood on its host while its puppet, under the same deferred kill with
// its AI off, died of the relayed number applied here
const kHealthFloor = 0.01;

const applyHealthPercentage = (ac: Actor, healthPercentage: number) => {
  const target = Math.min(1, Math.max(kHealthFloor, healthPercentage));
  const currentPercentage = ac.getActorValuePercentage('health');
  if (currentPercentage === target) {
    return;
  }

  const currentMax = ac.getBaseActorValue('health');
  const deltaPercentage = target - currentPercentage;
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
// thuum D49 (the turbo day, 2026-10-28): the feel numbers as settings knobs
// (skymp5-client.movement in the settings file, read once; a plugin reload
// re-reads them), so a motion change is a settings line and a reload, not a
// build. The defaults are the values this file carried.
type MovementKnobs = {
  leadSeconds: number;      // the running lead, speed x this ahead of the record
  leadDecay: number;        // the lead's run-out per standing sample (half)
  deadbandMoving: number;   // units a figure may stand off a fresh sample
  deadbandStill: number;    // units it settles from when the record is still
  popDistance: number;      // a jump this far, to a place not sent, is a pop
  popWindowMs: number;      // how long after a pop a stale report is ignored
  jumpDistance: number;     // a report this far from the figure places, never translates
  localGazeRadius: number;  // a puppet with no relayed gaze looks at the local player within this (0 off)
};
let knobsCache: MovementKnobs | undefined;
// the puppets looking at the local player right now (the local gaze, below)
const localGaze = new Set<number>();
const movementKnobs = (): MovementKnobs => {
  if (knobsCache) return knobsCache;
  const defaults: MovementKnobs = { leadSeconds: 0.1, leadDecay: 0.5, deadbandMoving: 8, deadbandStill: 4, popDistance: 256, popWindowMs: 600, jumpDistance: 1024, localGazeRadius: 450 };
  const s = settings["skymp5-client"] as Record<string, unknown> | undefined;
  const given = (s && typeof s["movement"] === "object" && s["movement"]) ? s["movement"] as Record<string, unknown> : {};
  const out = { ...defaults } as Record<string, number>;
  for (const key of Object.keys(defaults)) {
    const v = given[key];
    if (typeof v === "number" && Number.isFinite(v)) out[key] = v;
  }
  knobsCache = out as MovementKnobs;
  return knobsCache;
};
const lastSent = new Map<number, NiPoint3>();
const lastReal = new Map<number, NiPoint3>();
// the record's target at the previous apply, per actor (its stillness)
const lastTarget = new Map<number, NiPoint3>();
// the running lead still to run out, per actor (thuum playtest 35,
// 2026-10-27; Eli: "a player running is snapped back a tiny bit after
// jogging"; c1 wrote 108 settle lines on Prisoner's figure, mean 6.7 units,
// max 36.8): a running figure is led speed x 0.2 s ahead of its record, and
// at the first Standing sample the lead vanished, so the settle pulled the
// figure back onto the record. The lead now runs out over the standing
// samples (halved each, gone under one unit) and the deadband stays at eight
// until it is gone
const leads = new Map<number, [number, number]>();
const pops = new Map<number, { at: number; pos: NiPoint3; from: NiPoint3 }>();

const isStaleAfterPop = (refrId: number, real: NiPoint3, target: NiPoint3): boolean => {
  const now = Date.now();
  const sent = lastSent.get(refrId);
  const before = lastReal.get(refrId);
  lastReal.set(refrId, [real[0], real[1], real[2]]);
  if (
    sent && before &&
    ObjectReferenceEx.getDistance(real, before) > movementKnobs().popDistance &&
    ObjectReferenceEx.getDistance(real, sent) > movementKnobs().popDistance
  ) {
    pops.set(refrId, { at: now, pos: [real[0], real[1], real[2]], from: [before[0], before[1], before[2]] });
  }
  const pop = pops.get(refrId);
  if (!pop) {
    return false;
  }
  if (now - pop.at > movementKnobs().popWindowMs) {
    pops.delete(refrId);
    return false;
  }
  return ObjectReferenceEx.getDistance(target, pop.from) < ObjectReferenceEx.getDistance(target, pop.pos);
};

const translateTo = (refr: ObjectReference, m: Movement, settle = false) => {
  const knobs = movementKnobs();
  const time = knobs.leadSeconds;

  // Local lag compensation
  // TODO: Remove "|| 0" hack (added to support old MpClientPlugin)
  const distanceAdd = (m.speed || 0) * time;
  const direction = m.rot[2] + m.direction;
  gTempTargetPos[0] = m.pos[0];
  gTempTargetPos[1] = m.pos[1];
  gTempTargetPos[2] = m.pos[2];

  // We do not want to add pos in case of standing-jumping
  const leadId = refr.getFormID();
  if (m.runMode !== "Standing") {
    const lead: [number, number] = [Math.sin(direction / 180 * Math.PI) * distanceAdd, Math.cos(direction / 180 * Math.PI) * distanceAdd];
    leads.set(leadId, lead);
    gTempTargetPos[0] += lead[0];
    gTempTargetPos[1] += lead[1];
  } else {
    // the lead's run-out: half of what was left, until it is under one unit
    const left = leads.get(leadId);
    if (left) {
      const lead: [number, number] = [left[0] * knobs.leadDecay, left[1] * knobs.leadDecay];
      if (Math.hypot(lead[0], lead[1]) < 1) {
        leads.delete(leadId);
      } else {
        leads.set(leadId, lead);
        gTempTargetPos[0] += lead[0];
        gTempTargetPos[1] += lead[1];
      }
    }
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
  // The record's stillness is read from consecutive records, not from the
  // last translation sent: a figure placed by a teleport that never walked
  // has no translation behind it and never settled on 3bd8d8ae (b-duel
  // 000622 and 000954: the figure 4.4 and 4.9 off at the end of phase 2, no
  // settle fired)
  const refrId0 = refr.getFormID();
  const prevTarget = lastTarget.get(refrId0);
  lastTarget.set(refrId0, [gTempTargetPos[0], gTempTargetPos[1], gTempTargetPos[2]]);
  const recordStill = prevTarget !== undefined && ObjectReferenceEx.getDistance(prevTarget, gTempTargetPos) < 0.5;
  const deadband = settle || (recordStill && !leads.has(refrId0)) ? knobs.deadbandStill : knobs.deadbandMoving;
  const offBy = ObjectReferenceEx.getDistanceNoZ(refrRealPos, gTempTargetPos);
  if (
    m.runMode !== "Standing" ||
    m.isInJumpState ||
    offBy > deadband ||
    angleDiff > 80 ||
    Actor.from(refr)?.getSitState() === 3
  ) {
    if (m.runMode === "Standing" && offBy > deadband && deadband === knobs.deadbandStill) {
      // the settle, on the in-game console (rare: a standing figure or puppet
      // found off its still record); the watch's track is the lab's read
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
      if (distance > knobs.jumpDistance || m.placed) {
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
