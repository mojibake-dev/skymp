#include "ItemMenuRefresh.h"

#include <RE/I/InterfaceStrings.h>
#include <RE/I/InventoryUpdateData.h>
#include <RE/U/UI.h>
#include <RE/U/UIMessage.h>
#include <RE/U/UIMessageQueue.h>

bool ItemMenuRefresh::Post(RE::TESObjectREFR& owner)
{
  auto* ui = RE::UI::GetSingleton();
  if (!ui || !ui->IsItemMenuOpen()) {
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
