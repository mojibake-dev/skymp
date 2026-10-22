#pragma once

#include <cstddef>
#include <cstdint>

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
};
void SetLocomotion(uint32_t formId, const Locomotion& sample);
bool GetLocomotion(uint32_t formId, Locomotion& out);

// the two detours (Frida Gum replacements through FridaHookHandler::Replace,
// the DamageActor hook's shape), AE only (ADR-025's 1.6.1170)
void InstallHooks();
}
