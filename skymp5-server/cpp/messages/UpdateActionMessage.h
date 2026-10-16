#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include "SpellCastMessage.h"
#include <cstdint>
#include <string>
#include <type_traits>

// thuum docs/verbs/cast-action.md: an action the player's actor performed
// through the engine's action entry, with the idle the engine chose, the
// graph event it sent, the input type and the actor's behavior graph
// variables as the graph was asked; relayed to the actor's neighbours, whose
// figures replay it. The contract is skymp-wire's (wire-schema skymp.rs,
// UpdateAction, MsgType 44).
struct UpdateActionMessage : public MessageBase<UpdateActionMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::UpdateAction)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("idx", idx)
      .Serialize("action", action)
      .Serialize("idle", idle)
      .Serialize("eventName", eventName)
      .Serialize("type", type)
      .Serialize("actorAnimationVariables", actorAnimationVariables);
  }

  uint32_t idx = 0;
  uint32_t action = 0;
  uint32_t idle = 0;
  std::string eventName;
  uint32_t type = 0;
  ActorAnimationVariables actorAnimationVariables;
};
