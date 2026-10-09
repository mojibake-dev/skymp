#pragma once

#include <functional>

#include "PCH.h"

namespace TESModPlatform {
extern std::function<void(IVM* vm, StackID stackId)> onPapyrusUpdate;

int32_t Add(IVM* vm, StackID stackId, RE::StaticFunctionTag*, int32_t, int32_t,
            int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t,
            int32_t, int32_t, int32_t);

void MoveRefrToPosition(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                        RE::TESObjectREFR* refr, RE::TESObjectCELL* cell,
                        RE::TESWorldSpace* world, float posX, float posY,
                        float posZ, float rotX, float rotY, float rotZ);

enum
{
  WEAP_DRAWN_MODE_DEFAULT = -1,
  WEAP_DRAWN_MODE_ALWAYS_FALSE = 0,
  WEAP_DRAWN_MODE_ALWAYS_TRUE = 1,

  WEAP_DRAWN_MODE_MIN = WEAP_DRAWN_MODE_DEFAULT,
  WEAP_DRAWN_MODE_MAX = WEAP_DRAWN_MODE_ALWAYS_TRUE
};

void SetWeaponDrawnMode(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                        RE::Actor* actor, int32_t weapDrawnMode);

int32_t GetNthVtableElement(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                            RE::TESForm* pointer, int32_t pointerOffset,
                            int32_t elementIndex);

bool IsPlayerRunningEnabled(IVM* vm, StackID stackId, RE::StaticFunctionTag*);

RE::BGSColorForm* GetSkinColor(IVM* vm, StackID stackId,
                               RE::StaticFunctionTag*, RE::TESNPC* base);

RE::TESNPC* CreateNpc(IVM* vm, StackID stackId, RE::StaticFunctionTag*);

RE::TESNPC* EvaluateLeveledNpc(IVM* vm, StackID stackId,
                               RE::StaticFunctionTag*,
                               FixedString commaSeparatedListOfIds);

void SetNpcSex(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
               RE::TESNPC* npc, int32_t sex);

void SetNpcRace(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                RE::TESNPC* npc, RE::TESRace* race);

void SetNpcSkinColor(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                     RE::TESNPC* npc, int32_t skinColor);

void SetNpcHairColor(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                     RE::TESNPC* npc, int32_t skinColor);

void ResizeHeadpartsArray(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                          RE::TESNPC* npc, int8_t size);

void ResizeTintsArray(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                      int32_t size);

void SetFormIdUnsafe(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                     RE::TESForm* form, uint32_t newId);

void ClearTintMasks(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                    RE::Actor* targetActor);

void PushTintMask(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                  RE::Actor* targetActor, int32_t type, uint32_t argb,
                  FixedString texturePath);

void PushWornState(IVM* vm, StackID stackId, RE::StaticFunctionTag*, bool worn,
                   bool wornLeft);

void AddItemEx(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
               RE::TESObjectREFR* containerRefr, RE::TESForm* item,
               int32_t countDelta, float health,
               RE::EnchantmentItem* enchantment, int32_t maxCharge,
               bool removeEnchantmentOnUnequip, float chargePercent,
               FixedString textDisplayData, int32_t soul,
               RE::AlchemyItem* poison, int32_t poisonCount);

void UpdateEquipment(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                     RE::Actor* containerRefr, RE::TESForm* item,
                     bool leftHand);

void ResetContainer(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                    RE::TESForm* container);
void CloseMenu(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
               std::string_view name);

// Sets the engine's day count so that GameDaysPassed reads daysPassed and
// runs on from it (thuum docs/verbs/time.md)
void SetGameDaysPassed(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                       float daysPassed);

// A neighbour's arrow, launched from its figure along its shooter's aim
// (thuum docs/verbs/marksman.md): the figure's weapon node, a crossbow's
// magic node, as CommonLibSSE-NG's Projectile::LaunchArrow finds it, with
// the draw's power; false when an argument is missing or not a number
bool LaunchArrow(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                 RE::Actor* shooter, RE::TESObjectWEAP* weapon,
                 RE::TESAmmo* ammo, float power, float aimAngle,
                 float aimHeading);

// The player's favorites as (form id, hotkey) pairs, hotkey -1 for none, and
// marking one with a hotkey, -1 for none (thuum docs/verbs/favorites.md)
std::vector<int32_t> GetFavorites(IVM* vm, StackID stackId,
                                  RE::StaticFunctionTag*);
bool SetFavorite(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                 RE::TESForm* form, int32_t hotkey);

// The player's actor values and progress (thuum docs/verbs/actor-values.md):
// its 164 base values by actor value; one set; and its progress as xp,
// threshold, level, per skill level, xp and threshold, per skill its
// legendary count, read and written
std::vector<float> GetActorValueBases(IVM* vm, StackID stackId,
                                      RE::StaticFunctionTag*);
bool SetActorValueBase(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                       int32_t av, float base);
std::vector<float> GetPlayerProgress(IVM* vm, StackID stackId,
                                     RE::StaticFunctionTag*);
bool SetPlayerSkill(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                    int32_t skill, float level, float xp, float threshold,
                    int32_t legendary);
bool SetPlayerExperience(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                         float xp, float threshold, int32_t level);

void BlockPapyrusEvents(IVM* vm, StackID stackId, RE::StaticFunctionTag*,
                        bool blocked);

RE::TESObjectREFR* CreateReferenceAtLocation(
  IVM* vm, StackID stackId, RE::StaticFunctionTag*, RE::TESForm* baseForm,
  RE::TESObjectCELL* cell, RE::TESWorldSpace* world, float posX, float posY,
  float posZ, float rotX, float rotY, float rotZ, bool persist);

// Threadsafe
void BlockMoveRefrToPosition(bool blocked);
int GetWeapDrawnMode(uint32_t actorId);
uint64_t GetNumPapyrusUpdates();
std::shared_ptr<RE::BSTArray<RE::TintMask*>> GetTintsFor(uint32_t actorId);
bool GetPapyrusEventsBlocked();

void Update();

bool Register(IVM* vm);
}
