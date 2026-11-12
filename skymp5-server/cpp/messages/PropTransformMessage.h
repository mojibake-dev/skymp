#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <array>
#include <cstdint>
#include <type_traits>
#include <vector>

// thuum docs/verbs/prop-physics.md (ADR-058 decision 2; ADR-071 decision 6,
// physics rung 2's v2): where a physics prop (a kicked basket, a dropped
// sword) is and how it moves, from the one seat that simulates it, under the
// owner epoch that seat holds and a sequence within it. Client to server,
// unreliable, at most once per prop per 100 ms while the body moves, at least
// every 500 ms while it is awake or held (the heartbeat) and once more when
// its island deactivates (asleep true, reliable; a held body never rests on
// the wire); R2: the simulator's Havok computes, the server records the last
// transform and the flag on the reference's record
// (ActionListener::OnPropTransform: by the epoch first, then the owner, then
// the order and the step budget, wire-rules props) and relays the same bytes
// to the prop's other listeners, unreliable while awake and reliable at
// sleep. Server to client it is also the record answer to a refused sample,
// at seq 0 (PartOne::SendPropRecord), which a client never sends. The
// contract is skymp-wire's (wire-schema skymp.rs, PropTransform, MsgType 58,
// wire id 66).
struct PropTransformMessage : public MessageBase<PropTransformMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PropTransform)>{};

  // one rigid body's pose of a body of several (wire-schema PropBone): a
  // world object's parts (rung 4) or a dead actor's bones (rung 5); rung 2's
  // server takes a sample with none (wire-validate holds each inside the
  // world, within 512 units of pos, its quaternion within 1% of unit length)
  struct Bone
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("pos", pos).Serialize("quat", quat);
    }

    friend bool operator==(const Bone& lhs, const Bone& rhs)
    {
      return lhs.pos == rhs.pos && lhs.quat == rhs.quat;
    }

    std::array<float, 3> pos = { 0.f, 0.f, 0.f }; // engine units, absolute
    // a unit quaternion as Havok keeps one, x, y, z, w
    std::array<float, 4> quat = { 0.f, 0.f, 0.f, 1.f };
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("refr", refr)
      .Serialize("epoch", epoch)
      .Serialize("seq", seq)
      .Serialize("tMs", tMs)
      .Serialize("pos", pos)
      .Serialize("rot", rot)
      .Serialize("lin", lin)
      .Serialize("ang", ang)
      .Serialize("asleep", asleep)
      .Serialize("held", held)
      .Serialize("bones", bones);
  }

  uint32_t refr = 0; // the prop's reference form id, never 0
  // the owner epoch of the word the sender holds (0 before any word)
  uint32_t epoch = 0;
  // the sender's sample number within (refr, epoch), from 1, wrapping 65535
  // to 1; 0 is the server's record answer
  uint16_t seq = 0;
  // the owner's clock at the read, ms since its client started; 0 in the
  // record answer
  uint32_t tMs = 0;
  std::array<float, 3> pos = { 0.f, 0.f, 0.f }; // engine units
  std::array<float, 3> rot = { 0.f, 0.f, 0.f }; // Euler degrees, SkyMP's
  std::array<float, 3> lin = { 0.f, 0.f, 0.f }; // engine units a second
  std::array<float, 3> ang = { 0.f, 0.f, 0.f }; // radians a second
  bool asleep = false;     // the body came to rest at this transform
  bool held = false;       // the sender's engine holds the body; never asleep
  std::vector<Bone> bones; // at most 64; empty for an item
};
