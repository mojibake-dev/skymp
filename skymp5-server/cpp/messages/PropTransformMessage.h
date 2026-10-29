#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <array>
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/prop-physics.md (ADR-058 decision 2): where a physics
// prop (a kicked basket, a dropped sword) is, from the one seat that
// simulates it. Client to server, unreliable, at most once per prop per 100
// ms while the body is awake and once more when it sleeps (asleep true); R2:
// the simulator's Havok computes, the server records the last transform and
// the flag on the reference's record (ActionListener::OnPropTransform, the
// owner's samples alone, wire-rules props) and relays the same bytes to the
// cell's other listeners, unreliable while awake and reliable at sleep. The
// contract is skymp-wire's (wire-schema skymp.rs, PropTransform, MsgType 58,
// wire id 66).
struct PropTransformMessage : public MessageBase<PropTransformMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PropTransform)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("refr", refr)
      .Serialize("pos", pos)
      .Serialize("rot", rot)
      .Serialize("asleep", asleep);
  }

  uint32_t refr = 0; // the prop's reference form id, never 0
  std::array<float, 3> pos = { 0.f, 0.f, 0.f }; // engine units
  std::array<float, 3> rot = { 0.f, 0.f, 0.f }; // Euler degrees, SkyMP's
  bool asleep = false; // the body came to rest at this transform
};
