#pragma once

#include "../server_guest_lib/Appearance.h"
#include "MessageBase.h"
#include "MsgType.h"
#include "RaceOverlay.h"
#include <cstdint>
#include <optional>
#include <type_traits>

struct UpdateAppearanceMessage : public MessageBase<UpdateAppearanceMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::UpdateAppearance)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("idx", idx)
      .Serialize("data", data)
      .Serialize("overlay", overlay);
  }

  uint32_t idx = 0;
  std::optional<Appearance> data;
  // thuum ADR-068 rung 5 (D-T1 A): server to client, the race overlay
  // another player's figure wears with this look; absent while it wears its
  // record (the overlay's end is an UpdateAppearance without it)
  std::optional<RaceOverlay> overlay;
};
