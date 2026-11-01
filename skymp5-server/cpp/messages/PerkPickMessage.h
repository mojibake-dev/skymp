#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/perks.md (M5): a player's game reports the perk its Skills
// menu took. Client to server, reliable, R1: the server judges the pick by
// the perk record's own conditions and the points left (wire-rules perks,
// ActionListener::OnPerkPick), writes it to the record and answers with
// PerkState; a refused pick is undone by that answer. The contract is
// skymp-wire's (wire-schema skymp.rs, PerkPick, MsgType 67, wire id 75).
struct PerkPickMessage : public MessageBase<PerkPickMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PerkPick)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType).Serialize("perk", perk);
  }

  uint32_t perk = 0; // the perk's form id, never 0
};
