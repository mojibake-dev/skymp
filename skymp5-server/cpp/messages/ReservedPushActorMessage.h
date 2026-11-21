#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <type_traits>

// The wire's id contract (wire-schema SCHEMA_VERSION's note, thuum tracker
// #97): MsgType 85 is thuum tracker #175's PushActor on fork branch
// m4-pt56-175-npc-shout, kept here as a dead placeholder so jail's Arrest (86)
// and CrimeGold (87) sit past it. Never sent: the validator refuses it before
// the core sees it, and the core handles no MsgType 85 (PacketParser.cpp).
// Read and written back for the wire's fixture contract
// (unit/WireJsonContractTest.cpp). Whichever branch merges to canon after the
// other replaces it with PushActorMessage or renumbers.
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
