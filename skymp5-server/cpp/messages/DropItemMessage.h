#pragma once
#include "../server_guest_lib/Inventory.h"
#include "MessageBase.h"
#include "MsgType.h"
#include <type_traits>

// thuum tracker #98 (SCHEMA_VERSION 63, Eli's look 2: "cannot drop" a brew or
// an enchanted item): a drop names its stack with the extras and the created
// forms a put carries (Inventory::ExtraData; the wire's DropItem has no worn
// flags, so those read as none)
struct DropItemMessage
  : public MessageBase<DropItemMessage>
  , public Inventory::ExtraData
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::DropItem)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("baseId", baseId)
      .Serialize("count", count);

    ExtraData::Serialize(archive);
  }

  uint64_t baseId = 0;
  uint32_t count = 0;
};
