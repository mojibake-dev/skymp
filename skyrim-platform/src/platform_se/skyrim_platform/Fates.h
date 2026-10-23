#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// thuum docs/verbs/npc-fate.md (ADR-046 decision 1, ADR-049): the server's
// one roll for a leveled placement, held by this game ahead of the engine's
// own roll. Keyed by the reference's form id: the NPC_ the server named for
// the placement (0: no actor there, the engine deletes the placement as for
// its own empty roll) and the worn items the server rolled for it, each one
// an inventory entry tagged ExtraOutfitItem by the outfit detour (an empty
// list dresses nothing: the server's list is the outfit). Written by the
// FateApi exports on the JS thread, read by the two detours (FateHooks.cpp)
// on whichever thread loads the cell: a mutex-guarded map, the puppet table's
// idiom (Puppets.h, PuppetApi.cpp).
namespace Fates {
struct Fate
{
  uint32_t baseId = 0;
  std::vector<uint32_t> worn;
};

// The runtime switch, default off: both detours are installed at startup
// (Frida::InstallHooks, the puppet detours' moment) and pass every call
// through until the TS side calls setActorFateEnabled(true), which it does
// only once the lab read x-npc-fate-roll has confirmed the roll's moments
// (docs/research/m3/re-npc-roll.md section 7, the five observations at the
// top of FateHooks.cpp).
bool Enabled();
void SetEnabled(bool on);

void Set(uint32_t refrId, const Fate& fate);
// true when a fate was held
bool Clear(uint32_t refrId);
bool Get(uint32_t refrId, Fate& out);
size_t Count();

// the two detours (Frida Gum replacements through FridaHookHandler::Replace,
// the puppet detours' shape), AE only (ADR-025's 1.6.1170); called once from
// Frida::InstallHooks
void InstallHooks();
}
