#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <type_traits>

// MsgType 84, held for CraftReport where it is not built, so the ids after it
// keep their numbers. Never sent: the validator refuses it and the core
// handles no MsgType 84 (PacketParser.cpp). Serialized only for the wire's
// fixture contract (unit/WireJsonContractTest.cpp).
struct ReservedCraftReportMessage
  : public MessageBase<ReservedCraftReportMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::ReservedCraftReport)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType);
  }
};
