#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/cast-teleport.md (the teleport snap): the actor at idx
// made a jump the server vouches for, so the other games place its figure
// at the movement report that follows instead of gliding it there. Server
// to client, reliable, just before that report's relay.
struct ActorJumpMessage : public MessageBase<ActorJumpMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ActorJump)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType).Serialize("idx", idx);
  }

  uint32_t idx = 0;
};
