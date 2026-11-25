#pragma once
#include <cstdint>
#include <optional>

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
  (void)targetIsPlayer;
  (void)detectorIsPuppet;
  (void)relayed;
  return std::nullopt;
}

}
