#pragma once
#include "../server_guest_lib/Inventory.h"
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>
#include <vector>

// thuum tracker #98 (crafting; Eli's decision of 2026-11-16: relay the
// crafter's record): one sitting at a crafting bench as the crafter's engine
// read the player's inventory at the menu's open and its close. `lost` are
// the stacks that left, `gained` the stacks that came, a created form by its
// definition (CreatedForm.h), `learnedEnchantments` the game-file
// enchantments a disenchant taught. Client to server, reliable, R2: the
// server takes `lost` from the record when it holds every row (else it
// refuses the whole report and sends the record back), adds `gained` as
// reported and keeps the learned enchantments (CraftService::OnCraftReport).
// The contract is skymp-wire's (wire-schema skymp.rs, CraftReport, MsgType
// 84, wire id 92).
struct CraftReportMessage : public MessageBase<CraftReportMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::CraftReport)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("workbench", workbench)
      .Serialize("lost", lost)
      .Serialize("gained", gained)
      .Serialize("learnedEnchantments", learnedEnchantments);
  }

  uint32_t workbench = 0; // the bench, as the sender's view names it
  std::vector<Inventory::Entry> lost;
  std::vector<Inventory::Entry> gained;
  std::vector<uint32_t> learnedEnchantments;
};
