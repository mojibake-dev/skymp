#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <type_traits>

// MsgType 85, held for PushActor where it is not built, so the ids after it
// keep their numbers. Never sent: the validator refuses it and the core
// handles no MsgType 85 (PacketParser.cpp). Serialized only for the wire's
// fixture contract (unit/WireJsonContractTest.cpp).
struct ReservedPushActorMessage : public MessageBase<ReservedPushActorMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::ReservedPushActor)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType);
  }
};
