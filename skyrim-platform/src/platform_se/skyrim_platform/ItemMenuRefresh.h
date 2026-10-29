#pragma once
#include <RE/T/TESObjectREFR.h>

// thuum memo D52 B (playtest 37, 2026-10-28; docs/research/m3/
// re-container-menu-refresh.md): the engine has one refresh for the item
// menus (ContainerMenu, InventoryMenu, BarterMenu, GiftMenu), an
// InventoryUpdateData {owner's handle, no object} addressed "Top Menu" on the
// UI message queue (UI_MESSAGE_TYPE::kInventoryUpdate); Papyrus's AddItem and
// RemoveItem each end with it, and a menu showing that inventory rebuilds its
// rows on both sides when it processes the message. Post it from the game
// thread after the engine change has landed; never ItemList::Update (it
// blanks the menu's selection until the message is processed).
namespace ItemMenuRefresh {
// An item menu is open: the ContainerMenu, InventoryMenu, BarterMenu or
// GiftMenu by name. Not UI::IsItemMenuOpen: playtest 37 (02:03Z, the live
// chest's second test) read it FALSE with the ContainerMenu open on 1.6.1170
// (the counter the pin names numItemMenus does not count it there), so both
// gates keyed on it dropped every change under the menu and posted nothing
bool ItemMenuOpen();
// The two inventories an open item menu shows: the player's own, and the
// ContainerMenu's target while that menu is open. Only a change to one of
// them is worth a message (and safe to announce): playtest 37's crash on c1
// (02:24Z, 63 s after the train's relaunch, a ContainerMenu alive) came with
// every hosted actor's equipment apply posting an update for its own
// reference while a menu was open, dozens a second, each rebuilding rows
// the engine was not expecting to rebuild. HYPOTHESIS until the next sitting
bool Shows(RE::TESObjectREFR& owner);
// true when an item menu shows the owner's inventory and the message was
// queued
bool Post(RE::TESObjectREFR& owner);
}
