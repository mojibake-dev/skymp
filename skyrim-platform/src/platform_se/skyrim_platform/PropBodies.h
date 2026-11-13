#pragma once

#include <RE/B/bhkRigidBody.h>
#include <RE/H/hkpMotion.h>
#include <RE/H/hkpRigidBody.h>
#include <RE/N/NiAVObject.h>
#include <cstdint>

// thuum docs/DECISIONS.md ADR-071 rung 4 (statics and a body's parts; Eli's
// D-PH9 A and D-PH10 A, the impose route K3 under D-PH26 A; the rung's build
// spec, sections A C3, B and C): the Havok bodies under one reference's
// loaded 3D, found by one walk that every reader and writer shares, so the
// owner's read (readBodies' bones), the class each seat reads
// (getBodyClass), the follower's impose (imposeBodies) and the contact hook
// all name the same parts in the same order. A part is a rigid body whose
// motion type is not Invalid, Fixed or Character (CommonLibSSE-NG
// include/RE/H/hkpMotion.h:26-38): the Riverwood Trader sign's boards on its
// Fixed anchor, a dead actor's bones at rung 5. Plain data and engine field
// reads alone, except DriveBody and PlaceBody, which write through
// CommonLib's own bhkRigidBody calls and run on the main thread only.
namespace PropBodies {

// a node's own Havok body (rung 2's PropBody, which also keeps the node now):
// the node's collision object (include/RE/N/NiAVObject.h:156) as a
// bhkNiCollisionObject (N/NiObject.h:61), its body
// (B/bhkNiCollisionObject.h:56) as a bhkRigidBody (N/NiObject.h:64), and the
// hkpRigidBody it refers to (B/bhkRefObject.h:26; H/hkpRigidBody.h:7, an
// hkpEntity). All null for a node that carries no rigid body
struct Body
{
  RE::bhkRigidBody* rigid = nullptr;
  RE::hkpRigidBody* havok = nullptr;
  RE::NiAVObject* node = nullptr;
};

// the walk CommonLib's own GetCollisionLayer makes for one node
// (src/RE/N/NiAVObject.cpp:61-75), moved from PuppetApi.cpp (rung 2)
Body OwnBodyOf(RE::NiAVObject& node);

// hkpMotion::MotionType's dynamic values (include/RE/H/hkpMotion.h:26-38):
// the lab's resting Dynamic props read 2, a basket, and 3, an ingot (thuum
// docs/engine/havok-props.md E1, E13), never 1. Moved from PuppetApi.cpp; the
// contact hook's world-body switch reads it too (FridaHooks.cpp)
inline bool IsDynamicMotion(RE::hkpMotion::MotionType type)
{
  using Type = RE::hkpMotion::MotionType;
  return type == Type::kDynamic || type == Type::kSphereInertia ||
    type == Type::kBoxInertia || type == Type::kThinBoxInertia;
}

// a part: every body that is not Invalid, Fixed or Character, so types 1, 2,
// 3, 4 and 6 (hkpMotion.h:26-38). Keyframed counts, so a later route that
// sets a follower's parts Keyframed (the spec's section C, K4 or K1) keeps the
// part count both seats compare
inline bool IsPart(RE::hkpMotion::MotionType type)
{
  return IsDynamicMotion(type) ||
    type == RE::hkpMotion::MotionType::kKeyframed;
}

// at most this many parts: wire-schema's cap::PROP_BONES (skymp-wire
// crates/wire-schema/src/lib.rs:360), the poses one sample carries
constexpr uint32_t kPartsMax = 64;
// at most this many nodes looked at and this deep (both UNMEASURED: 194559
// walked 68 and 71 nodes for a humanoid's 19 bodies, thuum
// lab/.cache/drafts/physics/read-corpse-193821-194559.md:127, :156)
constexpr uint32_t kWalkNodesMax = 512;
constexpr uint32_t kWalkDepthMax = 32;

// one reference's bodies: every rigid body counted in bodies, each Fixed one
// in fixed, the first in walk order in first, and each part in parts in walk
// order (nParts of them); cut when the walk passed kPartsMax parts,
// kWalkNodesMax nodes or kWalkDepthMax levels, so the counts are partial and
// no bones are read or imposed
struct Walk
{
  Body parts[kPartsMax];
  uint32_t nParts = 0;
  uint32_t bodies = 0;
  uint32_t fixed = 0;
  bool cut = false;
  Body first;
};

// the walk itself, plain data alone (an explicit stack, no std::function) so
// GuardedWalkBodies may guard it (MSVC C2712), in CommonLib's own scene-graph
// order (BSVisit::TraverseScenegraphCollision, src/RE/B/BSVisit.cpp:11-37):
// a node's own collision object first, then its children in array order,
// depth first; children through NiAVObject::AsNode (N/NiObject.h:46) and
// NiNode::GetChildren (N/NiNode.h:57-65), every slot to the array's capacity
// (N/NiTArray.h:69-97), an empty one skipped. No lock: field reads alone
void WalkBodies(RE::NiAVObject& root, Walk& out);

// the walk in a structured exception frame, GuardedWalkIsland's shape
// (PuppetApi.cpp; the fork's GetNthVtableElement, PapyrusTESModPlatform.cpp:
// 253-258): false when a node or body freed mid-walk faulted the read
// (HYPOTHESIS, risk H-P1: the walk on the JS thread races the main thread's
// scene graph). The caller counts and says the fault (ReadWalk)
bool GuardedWalkBodies(RE::NiAVObject& root, Walk& out);

// GuardedWalkBodies with its fault counted and said, at the first and every
// hundredth: "[verb:prop-physics] body walk of <id> faulted: no read this time
// (<n> so far)" (the spec's line N4)
bool ReadWalk(RE::NiAVObject& root, uint32_t formId, Walk& out);

// a whole reference: one body, not Fixed (the items, a trap rock, a lone
// wheel), whose row is rung 3's; an anchor with any moving part is not whole
// (DEF-4d refined), and sends every part
inline bool IsWhole(const Walk& walk)
{
  return walk.bodies == 1 && walk.fixed == 0;
}

// a body's pose in Havok's units: pos the body origin's world position, quat
// its rotation as Havok keeps one, lanes x, y, z, w (wire-schema skymp.rs's
// PropBone; H/hkQuaternion.h:10, one hkVector4 whose lanes run x, y, z, w,
// H/hkVector4.h:19-21)
struct Pose
{
  float pos[3] = { 0.f, 0.f, 0.f };
  float quat[4] = { 0.f, 0.f, 0.f, 1.f };
};

// the body's pose by field reads, rung 2's shape, under no lock: pos
// motion.motionState.transform.translation (H/hkpEntity.h:99,
// H/hkpMotion.h:82, H/hkMotionState.h:14, H/hkTransform.h:12; 194559 read it
// equal to bhkRigidBody::GetPosition on all 19 bodies, thuum
// lab/.cache/drafts/physics/read-corpse-193821-194559.md:156-158) and quat
// motion.motionState.sweptTransform.rotation1 (H/hkMotionState.h:15,
// H/hkSweptTransform.h:14), normalized when its length is a positive number.
// HYPOTHESIS (rule 2, the build spec's F11 and its section 0.3 row 5):
// rotation1 equals bhkRigidBody::GetRotation (B/bhkRigidBody.h:42, slot
// 0x34), whose decompile the spec records reading the hkpRigidBody's +0x1E0,
// rotation1's place by these headers (hkpEntity::motion at 0x150,
// hkpMotion::motionState at 0x20, hkMotionState::sweptTransform at 0x40,
// hkSweptTransform::rotation1 at 0x30)
Pose PoseOf(const RE::hkpRigidBody& body);

// thuum ADR-071 rung 4's impose route K3 (D-PH26 A: a follower's part stays a
// live Dynamic body steered toward the owner's pose), the one function a
// later read changes (the spec's section C): the velocity that carries the
// body's centre of mass and rotation onto the target in one step of dt
// seconds, target in Havok's units. com is
// sweptTransform.centerOfMassLocal (H/hkSweptTransform.h:15); c = p + R(q)
// com, now and at the target; v = (c_target - c_now) / dt; the rotation's
// error q_e = q_target * conj(q_now), negated when its w is negative, gives
// omega = axis(q_e) * 2 atan2(|q_e.xyz|, q_e.w) / dt. v is clamped to 6000
// game units a second times bhkWorld::GetWorldScale (wire-validate's
// PROP_MAX_LIN_UNITS_PER_S, skymp-wire crates/wire-validate/src/lib.rs:76;
// B/bhkWorld.h:42-46) and omega to 1000 radians a second (lib.rs:79). Written
// through bhkRigidBody::SetLinearVelocity and SetAngularVelocity
// (B/bhkRigidBody.h:55, :53; src/RE/B/bhkRigidBody.cpp:12-31, Address
// Library 78089 and 78090, rung 2's setBodyVelocity calls). Main thread only.
// HYPOTHESIS (the spec's F10): Havok's linear velocity is the centre of
// mass's and its angular velocity is in the world's frame. Accessible:
// Fiedler, "Physics in 3D", gafferongames.com, 2004; foundational: Baraff,
// "Rigid Body Simulation", SIGGRAPH 97 course notes. A dt or a target that
// is not a number writes nothing
void DriveBody(const Body& body, const Pose& target, float dt);

// the body put on the target outright, target in Havok's units:
// bhkRigidBody::SetRotation then SetPosition (B/bhkRigidBody.h:44 and :43,
// slots 0x36 then 0x35, the pair and the order 194559 ran, which put all 19
// bodies on P0 exactly: thuum lab/.cache/drafts/physics/
// read-corpse-193821-194559.md:183-186, row 36; the position last, so the
// origin lands on the target whatever point SetRotation keeps fixed), then
// both velocities zero through 78089 and 78090. Main thread only
void PlaceBody(const Body& body, const Pose& target);
}
