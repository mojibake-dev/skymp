#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum tracker #97, ADR-079 (docs/verbs/jail.md): the arrested player's own
// game reports its arrest as its engine starts it (GoToPrison), before the
// strip's moves. Client to server, reliable. The arrest is that game's
// (ADR-063): the server records the jail term, its end on the shared clock,
// permits the one jump to the faction's own jail landing and takes the strip
// and the outfit for the arrest's window (PartOne::OnArrest). The contract is
// skymp-wire's (wire-schema skymp.rs, Arrest, MsgType 86, wire id 94).
struct ArrestMessage : public MessageBase<ArrestMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::Arrest)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("faction", faction)
      .Serialize("days", days);
  }

  uint32_t faction = 0; // the crime faction that jailed the player, never 0
  uint32_t days = 0;    // the sentence its engine set, 1 to 7 game days
};
