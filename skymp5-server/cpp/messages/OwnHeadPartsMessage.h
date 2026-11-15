#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>
#include <vector>

// thuum D-53-8 B (train 53's V-live; lab/.cache/drafts/reads-53/
// stage53-reads.md, train53-greens): the head parts a player's own game
// draws on its own player after that player's race changed, its base's
// overlay list while the engine answers HasOverlays and the list holds
// parts, else its base list (CommonLibSSE-NG TESNPC). Client to server,
// reliable, R2: the message names no actor, so it is only ever the sender's
// own player's; ActionListener::OnOwnHeadParts records it on that player's
// look when its race is the recorded one and every part is an HDPT, and
// sends the stored RaceMenu look and the look to every other game that sees
// the player, as a recorded ChangeHeadPart is. The contract is skymp-wire's
// (wire-schema skymp.rs, OwnHeadParts, MsgType 82, wire id 90).
struct OwnHeadPartsMessage : public MessageBase<OwnHeadPartsMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::OwnHeadParts)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("race", race)
      .Serialize("parts", parts)
      .Serialize("overlay", overlay);
  }

  uint32_t race = 0;           // the race the parts are drawn under, never 0
  std::vector<uint32_t> parts; // in the engine's order, each once, 1 to 64
  bool overlay = false;        // the base's overlay list, else its base list
};
