#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// A script's ObjectReference.PushActorAway in the sender's game onto an
// actor another seat simulates (a puppet there), skipped there and reported.
// The script itself keeps running on its caster's seat; the server checks
// consistency only and has the target's host run the push with the caster's
// figure as self (ActionListener::OnPushActor). Client to server, reliable.
// The contract is skymp-wire's (wire-schema skymp.rs, PushActor, MsgType 85,
// wire id 93).
struct PushActorMessage : public MessageBase<PushActorMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PushActor)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("caster", caster)
      .Serialize("target", target)
      .Serialize("force", force);
  }

  uint32_t caster = 0;
  uint32_t target = 0;
  float force = 0.f;
};
