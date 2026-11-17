#pragma once

#include <cstddef>
#include <cstdint>
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
struct Locomotion
{
  float speed = 0.f;
  float directionDeg = 0.f;
  float turnRateDeg = 0.f;
  // steady-clock milliseconds at the store; a sample older than
  // kStaleMs feeds speed 0 (the record stood still and the host sent no
  // newer sample, or the relay paused): playtest 32, a conversation pair
  // walking in place on the puppet screen (Embry 0x3550c, 2026-10-22)
  uint64_t stampMs = 0;
};
constexpr uint64_t kLocomotionStaleMs = 400;
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

// the two detours (Frida Gum replacements through FridaHookHandler::Replace,
// the DamageActor hook's shape), AE only (ADR-025's 1.6.1170)
void InstallHooks();
}
