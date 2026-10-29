#include "ItemMenuRefresh.h"

#include <RE/A/Actor.h>
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
  // a body's loot menu never: Eli's repro of the crash (02:2xZ) is "crash
  // happens when i open a corpse container", the body's inventory landing
  // and posting as its menu builds; a dead actor's rows are the engine's own
  // to build, the change shows at the next open as before tonight
  if (owner.As<RE::Actor>() ||
      !MenuExists(*ui, RE::ContainerMenu::MENU_NAME)) {
    return false;
  }
  const auto target = RE::ContainerMenu::GetTargetRefHandle();
  return target != 0 && target == owner.CreateRefHandle().native_handle();
}

bool ItemMenuRefresh::Post(RE::TESObjectREFR& owner)
{
  auto* ui = RE::UI::GetSingleton();
  if (!ui || !Shows(owner)) {
    return false;
  }
  auto* queue = RE::UIMessageQueue::GetSingleton();
  auto* strings = RE::InterfaceStrings::GetSingleton();
  if (!queue || !strings) {
    return false;
  }
  auto* data = static_cast<RE::InventoryUpdateData*>(
    queue->CreateUIMessageData(strings->inventoryUpdateData));
  if (!data) {
    return false;
  }
  data->inventoryRef = owner.CreateRefHandle().native_handle();
  data->updateObj = nullptr;
  queue->AddMessage(strings->topMenu, RE::UI_MESSAGE_TYPE::kInventoryUpdate,
                    data);
  return true;
}
