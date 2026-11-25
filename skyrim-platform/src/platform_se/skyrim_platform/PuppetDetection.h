#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// Whose answer the engine's detection reader gives on this game (the hook on
// Actor::RequestDetectionLevel, CommonLibSSE-NG include/RE/A/Actor.h:619,
// FridaHooks.cpp). The reader returns the detector's own knowledge of the
// target (its ActorKnowledge's DetectionState::level, CommonLibSSE-NG
// include/RE/D/DetectionState.h), which the detector's AI fills, and -1000
// for a detector that is not a high process. A puppet's AI is off on this
// game, so it never fills a level for this game's player, and a theft in its
// full view finds no witness in the thief's StealAlarm. The actor's host
// computes that level (its CalculateDetection, where another player's figure
// counts as the player) and relays it in HostedActorState; a puppet's
// detection of this game's own player answers that relayed level. Every other
// detector and target keeps the engine's own answer.
namespace PuppetDetection {

// RequestDetectionLevel's answer for a detector with no state (0xfffffc18)
inline constexpr std::int32_t kUndetected = -1000;

// the level the actor's host last relayed for its detection of this game's
// player, if any came since it became a puppet here
struct Relayed
{
  bool has = false;
  std::int32_t level = kUndetected;
};

// the relayed level when the detector is a puppet and the target this game's
// own player, and the host relayed one; nothing, the engine's own answer,
// otherwise
inline std::optional<std::int32_t> Answer(bool targetIsPlayer,
                                          bool detectorIsPuppet,
                                          const Relayed& relayed)
{
  if (!targetIsPlayer || !detectorIsPuppet || !relayed.has) {
    return std::nullopt;
  }
  return relayed.level;
}

// the stealth eye's answer for this game's player (the replacement of
// ProcessLists::RequestHighestDetectionLevelAgainstActor, Address Library
// 41408, CommonLibSSE-NG src/RE/P/ProcessLists.cpp:89-92): the scan reads each
// high actor's own detection state of the player, which a puppet never fills
// here, so its highest level is the engine's own or any puppet's relayed
// level, whichever is higher
inline std::int32_t HighestWithRelayed(std::int32_t engineHighest,
                                       const std::int32_t* relayed,
                                       std::size_t count)
{
  std::int32_t highest = engineHighest;
  for (std::size_t i = 0; relayed && i < count; ++i) {
    if (relayed[i] > highest) {
      highest = relayed[i];
    }
  }
  return highest;
}

// one puppet whose host's combat target is this game's player, as the compass
// replacement reads it
struct HostileCopy
{
  std::uint32_t formId = 0;
  bool dead = false;
  bool loaded = false;
};

// the compass's additions after its own fill (the replacement of the player's
// HUD actor fill, Address Library 41240: every member of every combat group
// that targets the player, which a puppet's dropped combat never stays in):
// each puppet its host's combat target names, alive, with its 3D, and not
// already listed, in the order given
inline std::vector<std::uint32_t> CompassAdds(
  const std::vector<std::uint32_t>& listed,
  const std::vector<HostileCopy>& hostile)
{
  std::vector<std::uint32_t> adds;
  for (const auto& copy : hostile) {
    if (copy.dead || !copy.loaded) {
      continue;
    }
    bool known = false;
    for (std::uint32_t id : listed) {
      known = known || id == copy.formId;
    }
    for (std::uint32_t id : adds) {
      known = known || id == copy.formId;
    }
    if (!known) {
      adds.push_back(copy.formId);
    }
  }
  return adds;
}

}
