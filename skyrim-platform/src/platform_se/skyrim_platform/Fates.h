#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace RE {
class BGSOutfit;
class InventoryChanges;
}

// thuum docs/verbs/npc-fate.md (ADR-046 decision 1, ADR-049): the server's
// one roll for a leveled placement, held by this game ahead of the engine's
// own roll. Keyed by the reference's form id: the NPC_ the server named for
// the placement (0: no actor there, the engine deletes the placement as for
// its own empty roll) and the worn items the server rolled for it, each one
// an inventory entry tagged ExtraOutfitItem for the actor's default outfit
// by the outfit detour (an empty list dresses nothing: the server's list is
// the outfit). Written by the FateApi exports on the JS thread, read by the
// two detours (FateHooks.cpp) on whichever thread loads the cell: a
// mutex-guarded map, the puppet table's idiom (Puppets.h, PuppetApi.cpp).
namespace Fates {
struct Fate
{
  uint32_t baseId = 0;
  std::vector<uint32_t> worn;
};

// The runtime switch, default off: both detours are installed at startup
// (Frida::InstallHooks, the puppet detours' moment) and pass every call
// through until the TS side calls setActorFateEnabled(true). The leveled
// detour's two observations held three times in the lab (2026-10-23); the
// outfit detour's chain (docs/research/m3/re-npc-roll.md section 8.4, the
// header of FateHooks.cpp) is HYPOTHESIS until its trace runs.
bool Enabled();
void SetEnabled(bool on);

void Set(uint32_t refrId, const Fate& fate);
// true when a fate was held
bool Clear(uint32_t refrId);
bool Get(uint32_t refrId, Fate& out);
size_t Count();

// What one dressing did: the entries it added and the forms it found
// already carrying the outfit's tag (the engine's own skip at the site).
struct Dressing
{
  size_t added = 0;
  size_t carried = 0;
};

// The one "add tagged" loop the outfit detour and redressActor share
// (FateHooks.cpp): for each worn form of the fate, a fresh ExtraDataList
// tagged ExtraOutfitItem{outfit} by the engine's own setter and added to the
// changes through the engine's own entry adder (re-npc-roll.md 8.4); a form
// that is no bound object in this game is skipped with a warn. Runs inside
// the engine's roll (the detour) or on the game thread (the redress).
Dressing AddWornTagged(RE::InventoryChanges* changes, RE::BGSOutfit* outfit,
                       const Fate& fate, uint32_t ownerId);

// the two detours (Frida Gum replacements through FridaHookHandler::Replace,
// the puppet detours' shape), AE only (ADR-025's 1.6.1170); called once from
// Frida::InstallHooks
void InstallHooks();
}
