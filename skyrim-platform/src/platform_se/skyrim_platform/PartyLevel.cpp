#include "PartyLevel.h"
#include "FridaHookHandler.h"
#include "PartyLevelRule.h"
#include "game/Offsets.h"
#include <RE/A/Actor.h>
#include <RE/P/ProcessLists.h>
#include <RE/T/TESActorBase.h>
#include <RE/T/TESDataHandler.h>
#include <atomic>
#include <stdexcept>
#include <string>

namespace {
std::atomic<uint16_t> g_partyLevel{ 0 };

using GetLevelFn = std::uint16_t (*)(const RE::TESActorBaseData* data);
GetLevelFn g_getLevelOriginal = nullptr;

// GetLevel's replacement: the party level rule's answer for a PC-level-mult
// base once a party level is held, the engine's own otherwise. A base's
// TESActorBaseData is its TESActorBase's second base class
// (include/RE/T/TESActorBase.h:21), so the owning form is a cast away.
std::uint16_t GetLevelReplacement(const RE::TESActorBaseData* data)
{
  const uint16_t party = g_partyLevel.load(std::memory_order_relaxed);
  if (party == 0 || !data || !data->HasPCLevelMult()) {
    return g_getLevelOriginal(data);
  }
  const auto* owner = static_cast<const RE::TESActorBase*>(data);
  PartyLevelRule::BaseFacts facts;
  facts.pcLevelMult = true;
  facts.playerBase = owner->IsPlayer(); // TESForm.h:343, the NPC_ 0x7
  facts.level = data->actorData.level;
  facts.calcLevelMin = data->actorData.calcLevelMin;
  facts.calcLevelMax = data->actorData.calcLevelMax;
  const auto level = PartyLevelRule::LevelFor(facts, party);
  return level ? *level : g_getLevelOriginal(data);
}

// the engine's level-up recompute for the held level, on the main thread
void Recompute(uint16_t level)
{
  using RecomputeFn = void (*)(RE::TESDataHandler*);
  using RefreshFn = void (*)(RE::Actor*);
  REL::Relocation<RecomputeFn> recompute{
    Offsets::PartyLevel::RecomputePCLevelMultBases
  };
  REL::Relocation<RefreshFn> refresh{
    Offsets::PartyLevel::RefreshActorValues
  };
  auto* data = RE::TESDataHandler::GetSingleton();
  if (!data) {
    return;
  }
  recompute(data);
  uint32_t refreshed = 0;
  if (auto* lists = RE::ProcessLists::GetSingleton()) {
    lists->ForAllActors([&](RE::Actor* actor) {
      const auto* base = actor ? actor->GetActorBase() : nullptr;
      if (base && base->HasPCLevelMult() && !actor->IsPlayerRef()) {
        refresh(actor);
        ++refreshed;
      }
      return RE::BSContainer::ForEachResult::kContinue;
    });
  }
  logger::info("[verb:leveling] party level {}: the PC-level-mult bases "
               "recomputed, {} loaded actors of them read again",
               level, refreshed);
}
}

uint16_t PartyLevel::Get()
{
  return g_partyLevel.load(std::memory_order_relaxed);
}

bool PartyLevel::Set(uint16_t level)
{
  if (g_partyLevel.exchange(level) == level) {
    return false;
  }
  if (!REL::Module::IsAE()) {
    return true;
  }
  auto* tasks = SKSE::GetTaskInterface();
  if (!tasks) {
    logger::warn("[verb:leveling] party level {}: no SKSE task interface, "
                 "the bases keep the stats of the level before",
                 level);
    return true;
  }
  tasks->AddUITask([level] {
    try {
      Recompute(level);
    } catch (const std::exception& e) {
      logger::error("[verb:leveling] party level {}: the recompute threw on "
                    "the main thread: {}",
                    level, e.what());
    }
  });
  return true;
}

void PartyLevel::InstallHooks()
{
  if (!REL::Module::IsAE()) {
    return;
  }
  const bool installed = Frida::HookHandler::GetSingleton()->Replace(
    Offsets::PartyLevel::GetLevel.address(),
    reinterpret_cast<void*>(&GetLevelReplacement),
    reinterpret_cast<void**>(&g_getLevelOriginal));
  logger::info("[verb:leveling] GetLevel detour installed {}, the engine's "
               "own level until a party level is told",
               installed);
}

Napi::Value PartyLevelApi::SetPartyLevel(const Napi::CallbackInfo& info)
{
  const auto level = NapiHelper::ExtractUInt32(info[0], "level");
  if (level > 0xffff) {
    throw std::runtime_error("setPartyLevel: level " + std::to_string(level) +
                             " is past 65535");
  }
  const bool changed = PartyLevel::Set(static_cast<uint16_t>(level));
  if (changed) {
    logger::info("[verb:leveling] party level told: {}", level);
  }
  return Napi::Boolean::New(info.Env(), changed);
}
