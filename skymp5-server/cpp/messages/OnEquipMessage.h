#pragma once
#include "../server_guest_lib/CreatedForm.h"
#include "MessageBase.h"
#include "MsgType.h"
#include <optional>
#include <type_traits>

struct OnEquipMessage : public MessageBase<OnEquipMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::OnEquip)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("baseId", baseId)
      .Serialize("createdBase", createdBase);
  }

  uint32_t baseId = 0;
  // thuum tracker #98: a brewed potion drunk or a brewed poison put on a
  // weapon, by its definition; baseId is then its game-file template
  std::optional<CreatedForm> createdBase;
};
