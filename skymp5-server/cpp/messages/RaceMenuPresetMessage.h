#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <string>
#include <type_traits>

// A player's RaceMenu look, as RaceMenu saves it (thuum
// docs/verbs/racemenu-sync.md). Client to server after the race menu closed
// and the look changed, `actor` 0; server to client, the look of the player
// `actor` names, after a login and with that player's figure.
struct RaceMenuPresetMessage : public MessageBase<RaceMenuPresetMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::RaceMenuPreset)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("actor", actor)
      .Serialize("preset", preset);
  }

  uint32_t actor = 0; // 0 from a client; the player's server id from the
                      // server
  std::string preset; // RaceMenu's JSON
};
