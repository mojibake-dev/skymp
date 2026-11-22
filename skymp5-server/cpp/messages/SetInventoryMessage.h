#pragma once
#include "../server_guest_lib/Inventory.h"
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <optional>
#include <type_traits>

struct SetInventoryMessage : public MessageBase<SetInventoryMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::SetInventory)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("inventory", inventory)
      .Serialize("craftAck", craftAck);
  }

  Inventory inventory;
  // SCHEMA_VERSION 62: the seq of the CraftReport this inventory answers,
  // the record after the report was taken or refused; absent on every other
  // inventory
  std::optional<uint32_t> craftAck;
};
