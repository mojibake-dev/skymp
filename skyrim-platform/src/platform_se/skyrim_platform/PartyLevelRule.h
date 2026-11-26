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

// The party's level times the base's mult as GetLevel computes its own
// player's (14384's PC-level-mult branch, the static read on 1.6.1170,
// HYPOTHESIS: the integer product taken as a float times 0.001f, cut toward
// zero), then raised to calcLevelMin and lowered to calcLevelMax where each
// is set. None, the engine's own answer standing, with no party level told,
// for a base without the PC level mult and for the player's own base.
inline std::optional<uint16_t> LevelFor(const BaseFacts& base,
                                        uint16_t partyLevel)
{
  if (partyLevel == 0 || !base.pcLevelMult || base.playerBase) {
    return std::nullopt;
  }
  const uint32_t product = static_cast<uint32_t>(partyLevel) * base.level;
  uint32_t level = static_cast<uint32_t>(static_cast<float>(product) * 0.001f);
  if (base.calcLevelMin != 0 && level < base.calcLevelMin) {
    level = base.calcLevelMin;
  }
  if (base.calcLevelMax != 0 && level > base.calcLevelMax) {
    level = base.calcLevelMax;
  }
  return static_cast<uint16_t>(level > 0xffffu ? 0xffffu : level);
}

}
