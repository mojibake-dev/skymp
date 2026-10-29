#include "ItemMenuRefresh.h"

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
      return "the queue gave no message data";
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
  if (!queue || !strings) {
    return PostResult::kNoQueue;
  }
  auto* data = static_cast<RE::InventoryUpdateData*>(
    queue->CreateUIMessageData(strings->inventoryUpdateData));
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
