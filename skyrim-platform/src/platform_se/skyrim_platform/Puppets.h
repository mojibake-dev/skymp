#pragma once

#include "PuppetDetection.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

// thuum docs/verbs/npc-puppet.md (ADR-045): the actors this game shows as
// puppets, by form id. A puppet's AI is off (Actor::EnableAI, the kProcessMe
// bit); the two detours (PuppetHooks.cpp) keep its animation graph ticking and
// skip the engine's movement step for it; skymp5-client moves and animates it
// from the host's relay. Written by the setPuppet export (PuppetApi.cpp),
// read by the detours on the stage threads: a mutex-guarded set, the
// weapDrawnMode share's idiom (PapyrusTESModPlatform.cpp).
namespace Puppets {
bool Is(uint32_t formId);
void Set(uint32_t formId, bool puppet);
size_t Count();

// The host's latest movement sample for a puppet, the graph's locomotion
// inputs (docs/research/m3/re-puppet-locomotion.md): speed in game units per
// second, the move direction in degrees clockwise from the facing (0 ahead),
// the heading rate in degrees per second (0 when unknown). Written by the
// setPuppetLocomotion export at every relayed sample (130 ms), read by the
// Move detour every frame, which writes it where the engine's Speed,
// Direction and TurnDelta channels read (MiddleHighProcessData).
// kLocomotionStaleMs is the least time a sample stays fresh, and a sample's
// own window when its feed names none.
constexpr uint64_t kLocomotionStaleMs = 400;
struct Locomotion
{
  float speed = 0.f;
  float directionDeg = 0.f;
  float turnRateDeg = 0.f;
  // the sample's heading in degrees, which the Move detour turns the puppet
  // toward at turnRateDeg every frame; a movement step's own sample has none
  float headingDeg = 0.f;
  bool hasHeading = false;
  // steady-clock milliseconds at the store; a sample older than staleMs
  // feeds speed 0 and turns nothing (the record stood still and the host
  // sent no newer sample, or the relay paused), so a puppet whose feed
  // stopped does not walk in place
  uint64_t stampMs = 0;
  // how long the sample stays fresh: the feed's own window, which follows
  // the copy's sample cadence (a far copy's samples come about 500 ms
  // apart), never under kLocomotionStaleMs
  uint64_t staleMs = kLocomotionStaleMs;
};
uint64_t NowMs();
void SetLocomotion(uint32_t formId, const Locomotion& sample);
bool GetLocomotion(uint32_t formId, Locomotion& out);

// thuum tracker #111 (playtest 53, Eli: "they slide in from somewhere ...
// and then they slide around and arent animated"): the other end of the
// same fields, MiddleHighProcessData currentMovementSpeed, rotation.z and
// rotationSpeed.z as this frame's movement step left them, in Locomotion's
// units. For an actor this game simulates (the hosted set, Hosted.h) the
// engine's own, sampled right after the engine's step, so a host reports
// what its engine moved whatever its graph's variables are named; for a
// puppet the relay's feed as the Move detour wrote it, what its graph's
// channels poll (the lab's watcher read). Sampled on the game thread
// (PuppetHooks.cpp), read on the JS thread by the getActorLocomotion export.
// stampMs is the sample's time; false when the actor was never sampled.
// MovementSampled says whether the movement-step detour is installed (AE
// only), with no sample ever taken without it
void SetStepLocomotion(uint32_t formId, const Locomotion& sample);
bool GetStepLocomotion(uint32_t formId, Locomotion& out);
void SetMovementSampled(bool installed);
bool MovementSampled();

// The engine's high-process actors (RE::ProcessLists::highActorHandles,
// CommonLibSSE-NG include/RE/P/ProcessLists.h:69), snapshotted by form id on
// the game thread at every "Actor animation" stage (PuppetHooks.cpp) and read
// on the JS thread by the getHighProcessActors export: the roster's source
// and the demote poll's. The cellAttach events name temporary references
// alone (playtest 33, 2026-10-23: the host's rosters carried the animals,
// the creatures and the placements and never a named resident, so a resident
// with no saved record was loaded by nobody and ran live in both games).
void SetHighActors(std::vector<uint32_t> ids);
std::vector<uint32_t> GetHighActors();

// thuum tracker #121 (playtest 53, Gorm frozen in Highmoon Hall; Eli's
// "go with b for 121", ADR-035 point 8 as amended 2026-11-16): the cells
// each high-process actor's running package names as its location, by
// CommonLibSSE-NG's names (Actor::GetCurrentPackage, Actor.h:536;
// TESPackage::packLoc and ::data, TESPackage.h:302-303;
// TESCustomPackageData::data, the package's data list, and each
// BGSPackageDataLocation's PackageLocation, PackageLocation.h), snapshotted
// beside the high-process list on the game thread and read by the
// getPackageDestinationCells export with no game call. A location near a
// reference names that reference's cell, one in a cell names the cell; the
// other kinds (an alias, the editor location, the actor itself) name
// nothing. The destination's host keeps the actor live on its way there
// (skymp5-client PuppetService)
void SetPackageCells(
  std::unordered_map<uint32_t, std::vector<uint32_t>> cells);
std::vector<uint32_t> GetPackageCells(uint32_t formId);

// the two detours (Frida Gum replacements through FridaHookHandler::Replace,
// the DamageActor hook's shape), AE only (ADR-025's 1.6.1170)
// thuum docs/verbs/guest-witness.md: a puppet's detection of this game's
// player as its host relayed it, written by the setPuppetDetection export on
// the JS thread and read by the RequestDetectionLevel replacement on the
// engine's job threads (FridaHooks.cpp), under the registry's lock; a level
// of nothing forgets it, and a puppet's end here forgets it too (Set)
void SetDetection(uint32_t formId, std::optional<int32_t> level);
// whether the actor is a puppet here, and its relayed level if any, read
// together under one lock
struct DetectionRead
{
  bool isPuppet = false;
  PuppetDetection::Relayed relayed;
};
DetectionRead ReadDetection(uint32_t formId);
// every puppet's relayed level of this game's player, read under one lock (the
// stealth eye's scan, FridaHooks.cpp)
std::vector<int32_t> RelayedLevels();
// the puppets whose host's combat target is this game's player (the compass's
// additions, FridaHooks.cpp), written by setPuppetHostileToPlayer; a puppet's
// end here forgets it (Set)
void SetHostileToPlayer(uint32_t formId, bool hostile);
std::vector<uint32_t> HostileToPlayer();

void InstallHooks();
}
