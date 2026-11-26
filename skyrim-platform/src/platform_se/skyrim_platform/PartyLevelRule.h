#pragma once
#include <cstdint>
#include <optional>

// The level a PC-level-mult NPC takes on a game told its party's level, the
// level TESActorBaseData's GetLevel (CommonLibSSE-NG include/RE/T/
// TESActorBaseData.h:120) answers in place of its own (PartyLevel.cpp's
// detour), decided here from facts the detour reads off the base, so the rule
// itself needs no game.
namespace PartyLevelRule {

// what the detour read of an actor base
struct BaseFacts
{
  bool pcLevelMult = false;  // ACBS kPCLevelMult (TESActorBaseData.h:29)
  bool playerBase = false;   // the player's own NPC_, 0x7
  uint16_t level = 0;        // ACBS level: with the mult, the mult x 1000
  uint16_t calcLevelMin = 0; // 0: no floor
  uint16_t calcLevelMax = 0; // 0: no ceiling
};

// None: the engine's own answer stands.
inline std::optional<uint16_t> LevelFor(const BaseFacts& base,
                                        uint16_t partyLevel)
{
  (void)base;
  (void)partyLevel;
  return std::nullopt;
}

}
