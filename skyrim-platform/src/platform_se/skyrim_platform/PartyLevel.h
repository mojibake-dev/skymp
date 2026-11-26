#pragma once

#include "NapiHelper.h"
#include <cstdint>

// docs/verbs/leveling.md (the party's level): the level this game's
// PC-level-mult NPCs scale to, the party's level the server tells it
// (PartyLevel, MsgType 90). TESActorBaseData's GetLevel is detoured so a
// PC-level-mult base answers the party's level times its mult
// (PartyLevelRule.h) where the engine reads its own player's; 0, the level
// before any is told, leaves every answer the engine's own.
namespace PartyLevel {
// Holds `level` (0 lets go) for the detour, read on any thread; on a change
// the engine's own level-up recompute runs on the main thread through SKSE's
// task queue: every PC-level-mult base's auto-calc stats from the new level,
// then each loaded non-player actor of such a base reads its values again,
// keeping the damage it has taken. False when the level was already held.
bool Set(uint16_t level);
uint16_t Get();
// The detour on GetLevel, AE only (game/Offsets.h, PartyLevel).
void InstallHooks();
}

namespace PartyLevelApi {
// setPartyLevel(level): PartyLevel::Set, a whole number 0 to 65535; true
// when the level changed
Napi::Value SetPartyLevel(const Napi::CallbackInfo& info);

inline void Register(Napi::Env env, Napi::Object& exports)
{
  exports.Set(
    "setPartyLevel",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetPartyLevel)));
}
}
