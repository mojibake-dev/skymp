#include "ItemMenuRefresh.h"

#include <RE/B/BSTCreateFactoryManager.h>
#include <RE/B/BSTDerivedCreator.h>
#include <RE/B/BarterMenu.h>
#include <RE/C/ContainerMenu.h>
#include <RE/G/GiftMenu.h>
#include <RE/I/InterfaceStrings.h>
#include <RE/I/InventoryMenu.h>
#include <RE/I/InventoryUpdateData.h>
#include <RE/U/UI.h>
#include <RE/U/UIMessage.h>
#include <RE/U/UIMessageQueue.h>
#include <string_view>

// A menu is open when its instance exists (UI::GetMenu by name: the menu
// map holds an instance between the menu's open and its close). Not
// UI::IsMenuOpen: it asks the menu's kOnStack flag, and with Skyrim Souls RE
// on the seats (its MenuFlagHandler rewrites the unpaused menus' flags) the
// flag reads false while the ContainerMenu stands, as numItemMenus did
// (playtest 37, the chest's third test: "refreshContainerMenu 371e9: the
// engine's inventory update not posted" with the menu found and its target
// matched a line earlier)
static bool MenuExists(RE::UI& ui, std::string_view name)
{
  return ui.GetMenu(name) != nullptr;
}

bool ItemMenuRefresh::ItemMenuOpen()
{
  auto* ui = RE::UI::GetSingleton();
  return ui &&
    (MenuExists(*ui, RE::ContainerMenu::MENU_NAME) ||
     MenuExists(*ui, RE::InventoryMenu::MENU_NAME) ||
     MenuExists(*ui, RE::BarterMenu::MENU_NAME) ||
     MenuExists(*ui, RE::GiftMenu::MENU_NAME));
}

bool ItemMenuRefresh::Shows(RE::TESObjectREFR& owner)
{
  auto* ui = RE::UI::GetSingleton();
  if (!ui || !ItemMenuOpen()) {
    return false;
  }
  if (owner.formID == 0x14) {
    return true;
  }
  // a body's loot menu too: the re-analyst's read of the corpse crash
  // (docs/research/m3/re-corpse-menu-crash.md): the body's Long Bow row's
  // ExtraDataList was freed by the server's inventory landing under the
  // open menu (RemoveItem, equip handling) and NOTHING posted a null after
  // it, so the item card's RequestItemCardInfo read the stale row (12329
  // GetByType on a dead presence pointer). A posted null rebuilds every row
  // before the next card request, so the body is announced as any target;
  // the exclusion that stood here for one build was the gap, not the fix
  if (!MenuExists(*ui, RE::ContainerMenu::MENU_NAME)) {
    return false;
  }
  const auto target = RE::ContainerMenu::GetTargetRefHandle();
  return target != 0 && target == owner.CreateRefHandle().native_handle();
}

const char* ItemMenuRefresh::Name(PostResult result)
{
  switch (result) {
    case PostResult::kPosted:
      return "posted";
    case PostResult::kNotShown:
      return "no open item menu shows that inventory";
    case PostResult::kNoQueue:
      return "no UI message queue or interface strings";
    case PostResult::kNoData:
      return "the factory has no creator for the inventory update or gave "
             "no data";
  }
  return "unknown";
}

ItemMenuRefresh::PostResult ItemMenuRefresh::TryPost(RE::TESObjectREFR& owner)
{
  auto* ui = RE::UI::GetSingleton();
  if (!ui || !Shows(owner)) {
    return PostResult::kNotShown;
  }
  auto* queue = RE::UIMessageQueue::GetSingleton();
  auto* strings = RE::InterfaceStrings::GetSingleton();
  auto* factory = RE::MessageDataFactoryManager::GetSingleton();
  if (!queue || !strings || !factory) {
    return PostResult::kNoQueue;
  }
  // the engine's own way to a fresh InventoryUpdateData (the re-analyst's
  // read, docs/research/m3/re-inventory-update-factory.md): the creator the
  // engine registered under "InventoryUpdateData" before WinMain, the one
  // its Papyrus AddItem and RemoveItem reach. UIMessageQueue's
  // CreateUIMessageData is no factory: it hands out a pre-made object from a
  // per-name pool and no pool exists for this name, so it answered null on
  // every post of playtest 37 ("the queue gave no message data" on both
  // seats, the chest 371e9 and the body 29600 alike), the in-place redraw
  // never happened and the reopen stayed the players' workaround
  const auto* creator =
    factory->GetCreator<RE::InventoryUpdateData>(strings->inventoryUpdateData);
  auto* data = creator
    ? static_cast<RE::InventoryUpdateData*>(creator->Create())
    : nullptr;
  if (!data) {
    return PostResult::kNoData;
  }
  data->inventoryRef = owner.CreateRefHandle().native_handle();
  data->updateObj = nullptr;
  queue->AddMessage(strings->topMenu, RE::UI_MESSAGE_TYPE::kInventoryUpdate,
                    data);
  return PostResult::kPosted;
}

bool ItemMenuRefresh::Post(RE::TESObjectREFR& owner)
{
  return TryPost(owner) == PostResult::kPosted;
}

bool ItemMenuRefresh::TargetByFormId(const RE::TESObjectREFR& owner,
                                     std::uint32_t& menuHandle)
{
  menuHandle = 0;
  auto* ui = RE::UI::GetSingleton();
  if (!ui || !MenuExists(*ui, RE::ContainerMenu::MENU_NAME)) {
    return false;
  }
  menuHandle = RE::ContainerMenu::GetTargetRefHandle();
  RE::NiPointer<RE::TESObjectREFR> target;
  RE::TESObjectREFR::LookupByHandle(menuHandle, target);
  return target && target->formID == owner.formID;
}
