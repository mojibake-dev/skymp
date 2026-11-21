#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <type_traits>

// The wire's id contract (wire-schema SCHEMA_VERSION's note, thuum tracker
// #175): MsgType 84 is thuum tracker #98's CraftReport on fork branch
// m4-pt56-98, kept here as a dead placeholder so PushActor (85) sits past it.
// Never sent: the validator refuses it before the core sees it, and the core
// handles no MsgType 84 (PacketParser.cpp). Read and written back for the
// wire's fixture contract (unit/WireJsonContractTest.cpp). Whichever branch
// merges to canon second replaces it with CraftReportMessage or renumbers.
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
