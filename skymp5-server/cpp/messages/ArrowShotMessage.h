#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <type_traits>

// A neighbour's shot the server took, for the receiving game to launch the
// arrow from that player's figure (thuum docs/verbs/marksman.md). Server to
// client: the shooter's game made the shot, the server checked it, and the
// figure's arrow is drawn while its damage stays the server's.
struct ArrowShotMessage : public MessageBase<ArrowShotMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ArrowShot)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("idx", idx)
      .Serialize("weaponId", weaponId)
      .Serialize("ammoId", ammoId)
      .Serialize("power", power)
      .Serialize("aimAngle", aimAngle)
      .Serialize("aimHeading", aimHeading);
  }

  uint32_t idx = 0;
  uint32_t weaponId = 0;
  uint32_t ammoId = 0;
  float power = 0.f;
  float aimAngle = 0.f;
  float aimHeading = 0.f;
};
