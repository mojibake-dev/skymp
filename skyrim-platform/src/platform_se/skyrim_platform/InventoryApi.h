#pragma once

#include "NapiHelper.h"

namespace InventoryApi {

Napi::Value GetExtraContainerChanges(const Napi::CallbackInfo& info);
Napi::Value GetContainer(const Napi::CallbackInfo& info);
Napi::Value SetInventory(const Napi::CallbackInfo& info);

// thuum tracker #98 (crafting; Eli's decision of 2026-11-16: relay the
// crafter's record): a form this engine made at runtime, for the wire and
// back. readCreatedForm(formId): {kind, effects} for a created potion,
// poison or enchantment (a form id above 0xFEFFFFFF the engine's
// BGSCreatedObjectManager holds), kind 0 a weapon enchantment, 1 an armor
// enchantment, 2 a potion, 3 a poison, each effect its MGEF's form id,
// magnitude, area, duration and cost; null for anything else
Napi::Value ReadCreatedForm(const Napi::CallbackInfo& info);
// createCreatedForm(kind, effects): the local form of a definition: its id
// when this native made or read one before, else 0 with the engine's creator
// queued on the main thread (a later call answers its form); -1 for a
// definition no creator takes (a kind outside 0 to 3, no effects or more
// than eight, an MGEF this engine does not hold, a creator that made none)
Napi::Value CreateCreatedForm(const Napi::CallbackInfo& info);
// createdFormTemplate(kind): the game-file form the engine copies a created
// form of `kind` from (BGSDefaultObjectManager's kBaseWeaponEnchantment,
// kBaseArmorEnchantment, kBasePotion, kBasePoison), 0 for none
Napi::Value CreatedFormTemplate(const Napi::CallbackInfo& info);

void Register(Napi::Env env, Napi::Object& exports);
}
