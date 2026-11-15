#include "PropBodies.h"
#include <RE/B/bhkNiCollisionObject.h>
#include <RE/B/bhkWorld.h>
#include <RE/H/hkQuaternion.h>
#include <RE/H/hkVector4.h>
#include <RE/N/NiNode.h>
#include <atomic>
#include <cmath>
#include <xmmintrin.h>

namespace {
// the walk's faults so far (ReadWalk's line N4)
std::atomic<uint64_t> g_walkFaults{ 0 };

// the fastest a drive moves a part: wire-validate's PROP_MAX_LIN_UNITS_PER_S
// (skymp-wire crates/wire-validate/src/lib.rs:76), game units a second, and
// PROP_MAX_ANG_RAD_PER_S (:79), radians a second, so a driven copy never
// moves faster than the stream that leads it may carry
constexpr float kDriveLinMaxUnits = 6000.f;
constexpr float kDriveAngMax = 1000.f;

// an hkVector4's four lanes (H/hkVector4.h:53, an __m128)
void Lanes(const RE::hkVector4& v, float out[4])
{
  _mm_storeu_ps(out, v.quad);
}

struct Vec
{
  float x = 0.f;
  float y = 0.f;
  float z = 0.f;
};

// Havok's lane order, x, y, z, w (H/hkQuaternion.h:10)
struct Quat
{
  float x = 0.f;
  float y = 0.f;
  float z = 0.f;
  float w = 1.f;
};

Vec Cross(const Vec& a, const Vec& b)
{
  return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z,
           a.x * b.y - a.y * b.x };
}

float Length(const Vec& v)
{
  return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// v rotated by the unit quaternion q (q v q*): v + w t + u x t with u the
// vector part and t = 2 (u x v)
Vec Rotate(const Quat& q, const Vec& v)
{
  const Vec u{ q.x, q.y, q.z };
  const Vec c = Cross(u, v);
  const Vec t{ 2.f * c.x, 2.f * c.y, 2.f * c.z };
  const Vec ut = Cross(u, t);
  return { v.x + q.w * t.x + ut.x, v.y + q.w * t.y + ut.y,
           v.z + q.w * t.z + ut.z };
}

// the Hamilton product a b (b applied first, then a)
Quat Mul(const Quat& a, const Quat& b)
{
  return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
           a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
           a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
           a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}

Quat Conj(const Quat& q)
{
  return { -q.x, -q.y, -q.z, q.w };
}

Quat QuatOf(const PropBodies::Pose& pose)
{
  return { pose.quat[0], pose.quat[1], pose.quat[2], pose.quat[3] };
}

// v scaled down to at most max long; a vector whose length is not a number
// is left to the caller's finite check
Vec Clamped(const Vec& v, float max)
{
  const float len = Length(v);
  if (!(len > max)) {
    return v;
  }
  const float k = max / len;
  return { v.x * k, v.y * k, v.z * k };
}

bool Finite(const Vec& v)
{
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

bool FinitePose(const PropBodies::Pose& pose)
{
  for (float f : pose.pos) {
    if (!std::isfinite(f)) {
      return false;
    }
  }
  for (float f : pose.quat) {
    if (!std::isfinite(f)) {
      return false;
    }
  }
  return true;
}
} // namespace

PropBodies::Body PropBodies::OwnBodyOf(RE::NiAVObject& node)
{
  Body res;
  auto* collision = node.collisionObject
    ? node.collisionObject->AsBhkNiCollisionObject()
    : nullptr;
  auto* rigid =
    collision && collision->body ? collision->body->AsBhkRigidBody() : nullptr;
  if (rigid && rigid->referencedObject) {
    res.rigid = rigid;
    res.havok = static_cast<RE::hkpRigidBody*>(rigid->referencedObject.get());
    res.node = &node;
  }
  return res;
}

void PropBodies::WalkBodies(RE::NiAVObject& root, Walk& out)
{
  out = Walk{};
  struct Entry
  {
    RE::NiAVObject* object;
    uint32_t depth;
  };
  // pre-order on an explicit stack: a node is visited when it is popped, and
  // its children are pushed last to first so the first pops next
  Entry stack[kWalkNodesMax];
  uint32_t top = 0;
  uint32_t visited = 0;
  stack[top++] = { &root, 0 };
  while (top > 0) {
    const Entry entry = stack[--top];
    if (++visited > kWalkNodesMax) {
      out.cut = true;
      return;
    }
    RE::NiAVObject* object = entry.object;
    const Body body = OwnBodyOf(*object);
    if (body.havok) {
      ++out.bodies;
      if (out.bodies == 1) {
        out.first = body;
      }
      const auto type = body.havok->motion.type.get();
      if (type == RE::hkpMotion::MotionType::kFixed) {
        ++out.fixed;
      }
      if (IsPart(type)) {
        if (out.nParts < kPartsMax) {
          out.parts[out.nParts++] = body;
        } else {
          out.cut = true;
        }
      }
    }
    RE::NiNode* node = object->AsNode();
    if (!node) {
      continue;
    }
    auto& children = node->GetChildren();
    RE::NiPointer<RE::NiAVObject>* const begin = children.begin();
    RE::NiPointer<RE::NiAVObject>* slot = children.end();
    while (begin && slot != begin) {
      --slot;
      RE::NiAVObject* child = slot->get();
      if (!child) {
        continue;
      }
      if (entry.depth + 1 > kWalkDepthMax || top == kWalkNodesMax) {
        out.cut = true;
        break;
      }
      stack[top++] = { child, entry.depth + 1 };
    }
  }
}

bool PropBodies::GuardedWalkBodies(RE::NiAVObject& root, Walk& out)
{
  __try {
    WalkBodies(root, out);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool PropBodies::ReadWalk(RE::NiAVObject& root, uint32_t formId, Walk& out)
{
  if (GuardedWalkBodies(root, out)) {
    return true;
  }
  const uint64_t faults =
    g_walkFaults.fetch_add(1, std::memory_order_relaxed) + 1;
  if (faults == 1 || faults % 100 == 0) {
    logger::warn("[verb:prop-physics] body walk of {:x} faulted: no read "
                 "this time ({} so far)",
                 formId, faults);
  }
  return false;
}

PropBodies::Pose PropBodies::PoseOf(const RE::hkpRigidBody& body)
{
  const auto& state = body.motion.motionState;
  float t[4];
  float q[4];
  Lanes(state.transform.translation, t);
  Lanes(state.sweptTransform.rotation1.vec, q);
  Pose pose;
  for (size_t i = 0; i < 3; ++i) {
    pose.pos[i] = t[i];
  }
  const float n =
    std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  const bool unit = std::isfinite(n) && n > 0.f;
  for (size_t i = 0; i < 4; ++i) {
    pose.quat[i] = unit ? q[i] / n : q[i];
  }
  return pose;
}

void PropBodies::DriveBody(const Body& body, const Pose& target, float dt)
{
  if (!body.rigid || !body.havok || !std::isfinite(dt) || !(dt > 0.f) ||
      !FinitePose(target)) {
    return;
  }
  const Pose now = PoseOf(*body.havok);
  float local[4];
  Lanes(body.havok->motion.motionState.sweptTransform.centerOfMassLocal,
        local);
  const Vec com{ local[0], local[1], local[2] };
  const Quat qNow = QuatOf(now);
  const Quat qTarget = QuatOf(target);
  // the centre of mass now and at the target, each its origin plus the
  // local centre turned by its rotation
  const Vec rNow = Rotate(qNow, com);
  const Vec rTarget = Rotate(qTarget, com);
  const Vec lin =
    Clamped({ (target.pos[0] + rTarget.x - now.pos[0] - rNow.x) / dt,
              (target.pos[1] + rTarget.y - now.pos[1] - rNow.y) / dt,
              (target.pos[2] + rTarget.z - now.pos[2] - rNow.z) / dt },
            kDriveLinMaxUnits * RE::bhkWorld::GetWorldScale());
  // the turn left to make, the short way round
  Quat e = Mul(qTarget, Conj(qNow));
  if (e.w < 0.f) {
    e = { -e.x, -e.y, -e.z, -e.w };
  }
  const float s = Length({ e.x, e.y, e.z });
  // the axis times the angle over dt; below a millionth of the vector part
  // the angle's own limit, 2 s / w with w about 1
  const float k = s > 1e-6f ? 2.f * std::atan2(s, e.w) / (s * dt) : 2.f / dt;
  const Vec ang = Clamped({ e.x * k, e.y * k, e.z * k }, kDriveAngMax);
  if (!Finite(lin) || !Finite(ang)) {
    return;
  }
  body.rigid->SetLinearVelocity(RE::hkVector4(lin.x, lin.y, lin.z, 0.f));
  body.rigid->SetAngularVelocity(RE::hkVector4(ang.x, ang.y, ang.z, 0.f));
}

void PropBodies::PlaceBody(const Body& body, const Pose& target)
{
  if (!body.rigid || !FinitePose(target)) {
    return;
  }
  RE::hkQuaternion rotation{ RE::hkVector4(target.quat[0], target.quat[1],
                                           target.quat[2], target.quat[3]) };
  RE::hkVector4 position(target.pos[0], target.pos[1], target.pos[2], 0.f);
  body.rigid->SetRotation(rotation);
  body.rigid->SetPosition(position);
  // hkVector4's default constructor zeroes every lane (H/hkVector4.h:11-13)
  const RE::hkVector4 zero;
  body.rigid->SetLinearVelocity(zero);
  body.rigid->SetAngularVelocity(zero);
}
