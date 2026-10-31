#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>
#include <vector>

// thuum docs/verbs/perks.md (M5): the server's whole perk set and the points
// left for one player: at login, after each pick (accepted or refused), after
// a level change and after the console's AddPerk, RemovePerk or SetLevel.
// Server to client, reliable. The game sets its perk points, adds what it
// lacks and drops a tree perk the set lacks. The contract is skymp-wire's
// (wire-schema skymp.rs, PerkState, MsgType 68, wire id 76).
struct PerkStateMessage : public MessageBase<PerkStateMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PerkState)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("perks", perks)
      .Serialize("points", points);
  }

  std::vector<uint32_t> perks; // the perks held, none 0, at most 512
  uint16_t points = 0;         // the points left to spend
};
