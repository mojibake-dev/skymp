#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/locks.md (M4): the server tells the player's game that its
// activation met a lock without the key, so the game runs the engine's own
// activation on its locked reference (the lockpicking menu, or the "requires
// key" box). Server to client, reliable, the activating player alone. The
// contract is skymp-wire's (wire-schema skymp.rs, LockPrompt, MsgType 65,
// wire id 73).
struct LockPromptMessage : public MessageBase<LockPromptMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::LockPrompt)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType).Serialize("refr", refr);
  }

  uint32_t refr = 0; // the reference's form id, never 0
};
