#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace RE {
class BGSOutfit;
class InventoryChanges;
class TESObjectREFR;
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
  // thuum docs/verbs/npc-fate.md (playtest 44's 1c): the record's inventory
  // rows apart from the worn items, {form id, count}: the server's own
  // container roll, written by the container-path detour at the engine's
  // init instead of its roll, and reconciled onto a loaded copy by the late
  // redress
  std::vector<std::pair<uint32_t, uint32_t>> inv;
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

// thuum ADR-050 decision 3 (train 51): a server-made NPC's fate, armed by
// its base before the copy's spawn (the view's placeAtMe) and bound to the
// copy's own id by the first detour that meets a new 0xff reference of that
// base with no fate yet (BindArmed, called by the three detours before they
// give up on an unknown id), so the copy's engine rolls the server's roll
// inside its placement as a game-file actor's does at its init. One fate per
// Arm, taken in spawn order per base.
void Arm(uint32_t baseId, const Fate& fate);
bool BindArmed(RE::TESObjectREFR* ref);

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

// thuum lab/.cache/drafts/reads-53/r-c3-temple-crash.md (playtest 51 break
// 7: every live re-roll before the crash logged "armed" twice, its two init
// passes, 37323's own and the ResetInventory after it, each writing the
// fate's weapons): the container detour's arms of one owner on this thread
// while the witness lives, so the re-roll can arm once (FateApi.cpp
// RerollOnGameThread skips its reset when 37323's own init armed). A scope
// on the game thread, nested ones each counting their own owner; the
// detour calls Note at each arm. HYPOTHESIS until a run of this branch's
// build logs "armed" once per re-roll and its copy wears the fate
// (x-anim-listener runs canon, without this branch).
class ArmWitness
{
public:
  explicit ArmWitness(uint32_t ownerId);
  ~ArmWitness();
  ArmWitness(const ArmWitness&) = delete;
  ArmWitness& operator=(const ArmWitness&) = delete;

  uint32_t Arms() const { return arms; }

  // the container detour's one call per arm (FateHooks.cpp)
  static void Note(uint32_t ownerId);

private:
  uint32_t owner;
  uint32_t arms = 0;
  ArmWitness* outer;
};

// the two detours (Frida Gum replacements through FridaHookHandler::Replace,
// the puppet detours' shape), AE only (ADR-025's 1.6.1170); called once from
// Frida::InstallHooks
void InstallHooks();
}
