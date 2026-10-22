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

// the two detours (Frida Gum replacements through FridaHookHandler::Replace,
// the DamageActor hook's shape), AE only (ADR-025's 1.6.1170)
void InstallHooks();
}
