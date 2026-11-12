#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum ADR-068 rung 4 (AliasFill; amendment B, read 8): a quest's reference
// alias filled or cleared. A quest is nobody's (D-F1 C): every seat runs its
// scripts, so each seat reports its own ReferenceAlias.ForceRefTo or Clear
// (served false). Both directions, reliable: the server records one fill per
// (quest, alias) on the world's journal, the latest report winning and an
// equal one folded (wire-rules alias_fills, ActionListener::OnAliasFill),
// relays a fill that moved the record to every other seat (served false) and
// serves the record at login (served true). The items an alias creates are
// the server's own from the game files (PartOne::AddCreatedItems) and never
// travel here. The contract is skymp-wire's (wire-schema skymp.rs,
// AliasFill, MsgType 79, wire id 87).
struct AliasFillMessage : public MessageBase<AliasFillMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::AliasFill)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("quest", quest)
      .Serialize("alias", alias)
      .Serialize("refr", refr)
      .Serialize("served", served);
  }

  uint32_t quest = 0;  // the QUST, a form of the shared load order
  uint32_t alias = 0;  // the alias's id in its quest (ALST), 0xFFFF at most
  uint32_t refr = 0;   // the reference filling it, 0 when cleared
  bool served = false; // the record served at login
};
