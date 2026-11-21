#include "Hooks.h"
#include "EventHandler.h"
#include "EventsApi.h"
#include "FridaHooks.h"
#include "Hosted.h"
#include "JsUtils.h"
#include "MpClientPluginApi.h"
#include "PapyrusTESModPlatform.h"
#include "Puppets.h"
#include "ScriptNatives.h"
#include "SkyrimPlatform.h"
#include <RE/D/DelayFunctor.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <vector>

namespace hook::internal {

uintptr_t GetAllocationBase(void* ptr)
{
  MEMORY_BASIC_INFORMATION mbi{};
  if (VirtualQuery(ptr, &mbi, sizeof(mbi))) {
    return reinterpret_cast<uintptr_t>(mbi.AllocationBase);
  }
  return 0;
}

std::mutex g_mutex;
std::vector<std::tuple<std::string, std::string, RE::BSScript::IFunction*,
                       uintptr_t, uintptr_t, uintptr_t>>
  g_boundNatives;
}

/**
 * @brief This hooks into the game main cycle
 * which behaves much like "our" tick cycle
 * but with a slight artificial delay between ticks.
 * Is mostly used for testing atm.
 */
struct OnFrameUpdate
{
  static void thunk(std::int64_t unk) { func(unk); };
  static inline REL::Relocation<decltype(&thunk)> func;
};

void InstallOnFrameUpdateHook()
{
  Hooks::write_thunk_call<OnFrameUpdate>(
    Offsets::Hooks::FrameUpdate.address());
}

struct OnConsoleVPrint
{
  static void thunk(void* unk1, const char* msg)
  {
    if (msg) {
      EventHandler::SendEventConsoleMsg(msg);
    }

    func(unk1, msg);
  };
  static inline REL::Relocation<decltype(&thunk)> func;
};

void InstallOnConsoleVPrintHook()
{
  Hooks::write_thunk_call<OnConsoleVPrint>(Offsets::Hooks::VPrint.address());
}

// thuum docs/verbs/cell-host.md (the quitting host, ADR-047's D12 B): the
// one call WinMain (36544) makes to the engine's shutdown routine (36551)
// once Main::quitGame is seen true, at WinMain+0x1ae. Every quit sets that
// byte and passes here on the main thread with the network thread alive:
// the console's qqq, the main and system menus' Quit, Alt-F4 and the
// title bar's X (WM_DESTROY), Skyrim Platform's own exitProcess; the engine
// never calls ExitProcess itself, the CRT does after WinMain returns (REA on
// sky-re 2026-10-24, CONFIRMED-STATIC; the Frida trace of the verb doc's
// Dynamic plan is owed). The client is torn down first, so its disconnect
// datagram goes out before the engine's teardown and the server passes the
// quitter's cells at once, not at the 4 s stale rule or netcode's 15 s
// timeout (the 24 s wait of the 2026-10-22 playtest). The teardown's wait
// is bounded in the client (wire-client-ffi, 200 ms). The shutdown
// routine's arguments are unread: the four integer-register arguments are
// forwarded as they came, the result as it returns.
struct OnQuitShutdown
{
  static std::int64_t thunk(std::int64_t a1, std::int64_t a2, std::int64_t a3,
                            std::int64_t a4)
  {
    const auto started = std::chrono::steady_clock::now();
    bool destroyed = false;
    try {
      destroyed = MpClientPluginApi::DestroyClientAtQuit();
    } catch (std::exception& e) {
      logger::error("[verb:cell-host] quit: the client's teardown threw: {}",
                    e.what());
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - started)
                      .count();
    const bool fastExit = MpClientPluginApi::QuitFastExit();
    logger::info(
      "[verb:cell-host] quit: {} in {} ms; the engine's shutdown {}",
      destroyed ? "the client disconnected" : "no client to disconnect", ms,
      fastExit ? "is skipped (fast exit, D24-B)" : "follows");
    // thuum docs/research/m3/decisions-after-playtest-34.md D24 (B): the
    // engine's shutdown crashes at every quit in the Scaleform heap teardown,
    // since two SKSE plugins hold the always-open Loading Menu's IMenu in
    // statics that nothing clears (x-quit-scan v3 20261026-021707:
    // NextGenDecapitations.dll, DeathDropOverhaul.dll; cell-host.md's
    // quitting-host row). Nothing of ours needs that teardown: the client is
    // gone above and the logs are flushed here, so the process ends the way
    // the fast-exit plugins end it. skymp5-client's quitFastExit setting
    // (default on) turns it off for a read of the engine's own shutdown
    if (fastExit) {
      try {
        spdlog::default_logger()->flush();
      } catch (...) {
      }
      TerminateProcess(GetCurrentProcess(), 0);
    }
    return func(a1, a2, a3, a4);
  };
  static inline REL::Relocation<decltype(&thunk)> func;
};

void InstallQuitDisconnectHook()
{
  if (!REL::Module::IsAE()) {
    logger::info("[verb:cell-host] the quit hook installs on AE only; the "
                 "disconnect waits for netcode's timeout here");
    return;
  }
  Hooks::write_thunk_call<OnQuitShutdown>(
    Offsets::Hooks::QuitShutdownCall.address());
}

/**
 * While skymp5-client holds the server's values for the player
 * (TESModPlatform.SetPotionEffectsBlocked), the server computes every number
 * another actor deals the player, and the player's own game plays the rest of
 * that actor's cast as the engine does: its effect, its shader and art, its
 * stagger and paralysis, and its script (a vampire's drain rolls its disease
 * at OnEffectStart; Unrelenting Force's VoicePushEffectScript throws the
 * player from the caster's figure).
 *
 * The player's MagicTarget vtable is the fifth of PlayerCharacter's:
 * TESObjectREFR has four (VTABLE_TESObjectREFR) and Actor's MagicTarget base
 * follows them (CommonLibSSE-NG include/RE/A/Actor.h:126), as VTABLE_Actor's
 * ten count. Two of its virtuals are replaced
 * (include/RE/M/MagicTarget.h:78, :85):
 * - AddTarget (01), called once per effect with its caster, magic item and
 *   effect (AddTargetData, :56-67), refuses every effect of an alchemy item
 *   (a potion, a food, a poison: spell type kAlchemy), the player's own
 *   drinks being the server's alone, and lets one add through for each
 *   enchanted weapon's hit on the player (PlayerAddTarget::IsHitsSecondAdd).
 *   It passes AddTargetData's magnitude on as it came: the engine's AddTarget
 *   (Address Library 38786) answers false for a fire damage enchantment's
 *   effect handed a zero magnitude and adds nothing, shader and script
 *   included;
 * - EffectAdded (08) zeroes the added ActiveEffect's own magnitude
 *   (include/RE/A/ActiveEffect.h:110) where another actor's cast lands an
 *   effect on the player that modifies Health, Magicka or Stamina
 *   (PlayerEffectAdded), so the effect runs whole with no number of its own.
 *   HYPOTHESIS: EffectAdded runs before the effect's first update and its
 *   start, so no part of the engine's number lands before the zero.
 */
struct PlayerAddTarget
{
  static bool IsServerValue(RE::ActorValue a_av)
  {
    return a_av == RE::ActorValue::kHealth ||
      a_av == RE::ActorValue::kMagicka || a_av == RE::ActorValue::kStamina;
  }

  // a value modifier archetype (include/RE/E/EffectArchetypes.h:10-44) by
  // its primary or second actor value (include/RE/E/EffectSetting.h:89, :94)
  static bool ChangesServerValue(const RE::Effect* a_effect)
  {
    const auto mgef = a_effect ? a_effect->baseEffect : nullptr;
    if (!mgef) {
      return false;
    }
    using Archetype = RE::EffectSetting::Archetype;
    switch (mgef->GetArchetype()) {
      case Archetype::kValueModifier:
      case Archetype::kPeakValueModifier:
      case Archetype::kAccumulateMagnitude:
      case Archetype::kValueAndParts:
        return IsServerValue(mgef->data.primaryAV);
      case Archetype::kDualValueModifier:
      case Archetype::kAbsorb:
        return IsServerValue(mgef->data.primaryAV) ||
          IsServerValue(mgef->data.secondaryAV);
      default:
        return false;
    }
  }

  // an actor's cast: a spell, power, shout or scroll, and an enchantment,
  // whether its enchanted weapon's hit, its staff's or its explosion's (an
  // explosion casts its EXPL's enchantment through the owner's instant
  // caster); include/RE/M/MagicSystem.h:60-80 for the types
  static bool IsCast(const RE::MagicItem* a_item)
  {
    using SpellType = RE::MagicSystem::SpellType;
    switch (a_item->GetSpellType()) {
      case SpellType::kSpell:
      case SpellType::kPower:
      case SpellType::kLesserPower:
      case SpellType::kVoicePower:
      case SpellType::kScroll:
      case SpellType::kEnchantment:
      case SpellType::kStaffEnchantment:
        return true;
      default:
        return false;
    }
  }

  // One enchanted weapon's hit on the player reaches AddTarget twice: from
  // the engine, as the attacker's figure's weapon hits the player in this
  // game (through the weapon's hand), and from the server's relay of the hit
  // the attacker's own game registered, which skymp5-client casts from the
  // figure through its instant caster (castSpellImmediate; kInstant,
  // include/RE/M/MagicSystem.h:23-29). Both carry the same caster, magic item
  // and effect. The first add of an effect lands; an add from the other
  // source within kHitPairWindow is the same hit's second and is refused, so
  // one effect lands whichever arrives first. A pair closes at its second
  // add, so the weapon's next hit opens a new one, and an add whose other
  // half never comes (the figure's swing missing in this game, or a hit the
  // server never relayed) lands alone. The window outlasts the gap between
  // the two adds of one hit, each a server trip from the attacker's game,
  // and the pairing keeps one effect a hit even where it spans two hits.
  struct HitAdd
  {
    std::uint64_t id = 0;
    RE::FormID caster = 0;
    RE::FormID item = 0;
    RE::FormID mgef = 0;
    bool relayed = false;
    std::chrono::steady_clock::time_point at;
  };
  static constexpr auto kHitPairWindow = std::chrono::milliseconds(1000);
  static inline std::mutex hitAddsMutex;
  static inline std::vector<HitAdd> hitAdds;
  static inline std::uint64_t nextHitAdd = 0;
  static inline std::atomic<std::uint32_t> secondAdds{ 0 };

  // true when this add is the second of a hit whose first is held; else
  // this add is held as its hit's first, under a_held, until it lands or
  // ForgetHitAdd drops it
  static bool IsHitsSecondAdd(const RE::Actor* a_caster,
                              const RE::MagicTarget::AddTargetData& a_data,
                              std::uint64_t& a_held)
  {
    const auto mgef = a_data.effect ? a_data.effect->baseEffect : nullptr;
    if (!mgef) {
      return false;
    }
    HitAdd add;
    add.caster = a_caster->GetFormID();
    add.item = a_data.magicItem->GetFormID();
    add.mgef = mgef->GetFormID();
    add.relayed =
      a_data.castingSource == RE::MagicSystem::CastingSource::kInstant;
    add.at = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(hitAddsMutex);
    hitAdds.erase(std::remove_if(hitAdds.begin(), hitAdds.end(),
                                 [&](const HitAdd& held) {
                                   return add.at - held.at > kHitPairWindow;
                                 }),
                  hitAdds.end());
    const auto first =
      std::find_if(hitAdds.begin(), hitAdds.end(), [&](const HitAdd& held) {
        return held.caster == add.caster && held.item == add.item &&
          held.mgef == add.mgef && held.relayed != add.relayed;
      });
    if (first != hitAdds.end()) {
      hitAdds.erase(first);
      return true;
    }
    add.id = ++nextHitAdd;
    a_held = add.id;
    hitAdds.push_back(add);
    return false;
  }

  // a held first add the engine refused, so the hit's other add may land
  static void ForgetHitAdd(std::uint64_t a_held)
  {
    std::lock_guard<std::mutex> lock(hitAddsMutex);
    hitAdds.erase(
      std::remove_if(hitAdds.begin(), hitAdds.end(),
                     [&](const HitAdd& held) { return held.id == a_held; }),
      hitAdds.end());
  }

  static bool thunk(RE::MagicTarget* a_this,
                    RE::MagicTarget::AddTargetData& a_data)
  {
    if (!TESModPlatform::ArePotionEffectsBlocked() || !a_data.magicItem) {
      return func(a_this, a_data);
    }
    using SpellType = RE::MagicSystem::SpellType;
    const auto spellType = a_data.magicItem->GetSpellType();
    if (spellType == SpellType::kAlchemy) {
      return false;
    }
    auto caster = a_data.caster ? a_data.caster->As<RE::Actor>() : nullptr;
    if (spellType != SpellType::kEnchantment || !caster ||
        caster->IsPlayerRef()) {
      return func(a_this, a_data);
    }
    std::uint64_t held = 0;
    if (IsHitsSecondAdd(caster, a_data, held)) {
      const std::uint32_t n = ++secondAdds;
      if ((n & (n - 1)) == 0) {
        logger::info("[verb:magic-effects] an enchanted hit's second add on "
                     "the player refused ({} so far; the latest {:x}'s "
                     "{:x}, {})",
                     n, caster->GetFormID(), a_data.magicItem->GetFormID(),
                     a_data.castingSource ==
                         RE::MagicSystem::CastingSource::kInstant
                       ? "the relay's"
                       : "the engine's");
      }
      return false;
    }
    const bool added = func(a_this, a_data);
    if (!added && held) {
      ForgetHitAdd(held);
    }
    return added;
  }
  static inline REL::Relocation<decltype(thunk)> func;
};

// The added effect's magnitude (ActiveEffect +0x78) is the number its
// archetype works on the actor value with. Zeroed here, before the engine's
// own EffectAdded runs, the effect keeps its shader, art, hit visuals and
// script, and the server's number stays the only one. The caster is the
// effect's own, its handle resolved (include/RE/A/ActiveEffect.h:93, :100),
// so an effect added later than its AddTarget is judged the same way.
struct PlayerEffectAdded
{
  static void thunk(RE::MagicTarget* a_this, RE::ActiveEffect* a_effect)
  {
    if (a_effect && a_effect->spell &&
        TESModPlatform::ArePotionEffectsBlocked() &&
        PlayerAddTarget::IsCast(a_effect->spell) &&
        PlayerAddTarget::ChangesServerValue(a_effect->effect)) {
      const auto caster = a_effect->GetCasterActor();
      if (caster && !caster->IsPlayerRef()) {
        a_effect->magnitude = 0.f;
        const std::uint32_t n = ++zeroed;
        if ((n & (n - 1)) == 0) {
          const auto mgef = a_effect->GetBaseObject();
          logger::info("[verb:magic-effects] another actor's effect on the "
                       "player zeroed at its add ({} so far; the latest "
                       "{:x}'s {:x}, effect {:x})",
                       n, caster->GetFormID(), a_effect->spell->GetFormID(),
                       mgef ? mgef->GetFormID() : 0);
        }
      }
    }
    func(a_this, a_effect);
  }
  static inline REL::Relocation<decltype(thunk)> func;
  static inline std::atomic<std::uint32_t> zeroed{ 0 };
};

void InstallPlayerAddTargetHook()
{
  REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[4] };
  PlayerAddTarget::func = vtbl.write_vfunc(0x1, PlayerAddTarget::thunk);
  PlayerEffectAdded::func = vtbl.write_vfunc(0x8, PlayerEffectAdded::thunk);
}

/**
 * The player's arrest is its own game's, and its sentence runs on the shared
 * clock. PlayerCharacter's own virtuals, its primary vtable
 * (VTABLE_PlayerCharacter[0], Address Library 208040 on 1.6.1170), slots
 * from CommonLibSSE-NG include/RE/A/Actor.h: SetCrimeGoldValue 0xB5,
 * ModCrimeGoldValue 0xB6, RemoveCrimeGoldValue 0xB7 (:379-381), GoToPrison
 * 0xB9 and ServePrisonTime 0xBA (:383-384), and
 * include/RE/P/PlayerCharacter.h:538's ClearAllCrimeGold 0x12D.
 * - GoToPrison: the arrest reported at its entry, a real jail only, before
 *   the engine strips and moves the player (EventHandler::OnArrest);
 * - ServePrisonTime: never run (its time skip would be one player's alone),
 *   what was asked reported (EventHandler::OnServePrisonTime);
 * - the crime gold setters: each change reported after the engine's
 *   (EventHandler::OnCrimeGoldChanged). Assumed, not yet traced: the
 *   engine's witnessed crimes reach these virtuals rather than writing the
 *   player's crimeGoldMap themselves.
 */
struct PlayerGoToPrison
{
  static void thunk(RE::PlayerCharacter* a_this, RE::TESFaction* a_faction,
                    bool a_removeInventory, bool a_realJail)
  {
    if (a_realJail) {
      EventHandler::OnArrest(a_this, a_faction);
    }
    func(a_this, a_faction, a_removeInventory, a_realJail);
  }
  static inline REL::Relocation<decltype(thunk)> func;
};

struct PlayerServePrisonTime
{
  static void thunk(RE::PlayerCharacter* a_this)
  {
    EventHandler::OnServePrisonTime(a_this);
  }
  static inline REL::Relocation<decltype(thunk)> func;
};

struct PlayerSetCrimeGoldValue
{
  static void thunk(RE::PlayerCharacter* a_this, RE::TESFaction* a_faction,
                    bool a_violent, std::uint32_t a_amount)
  {
    func(a_this, a_faction, a_violent, a_amount);
    EventHandler::OnCrimeGoldChanged(a_faction);
  }
  static inline REL::Relocation<decltype(thunk)> func;
};

struct PlayerModCrimeGoldValue
{
  static void thunk(RE::PlayerCharacter* a_this, RE::TESFaction* a_faction,
                    bool a_violent, std::int32_t a_amount)
  {
    func(a_this, a_faction, a_violent, a_amount);
    EventHandler::OnCrimeGoldChanged(a_faction);
  }
  static inline REL::Relocation<decltype(thunk)> func;
};

struct PlayerRemoveCrimeGoldValue
{
  static void thunk(RE::PlayerCharacter* a_this, RE::TESFaction* a_faction,
                    bool a_violent, std::int32_t a_amount)
  {
    func(a_this, a_faction, a_violent, a_amount);
    EventHandler::OnCrimeGoldChanged(a_faction);
  }
  static inline REL::Relocation<decltype(thunk)> func;
};

struct PlayerClearAllCrimeGold
{
  static void thunk(RE::PlayerCharacter* a_this, RE::TESFaction* a_faction)
  {
    func(a_this, a_faction);
    EventHandler::OnCrimeGoldChanged(a_faction);
  }
  static inline REL::Relocation<decltype(thunk)> func;
};

void InstallPlayerPrisonHooks()
{
  REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
  PlayerSetCrimeGoldValue::func =
    vtbl.write_vfunc(0xB5, PlayerSetCrimeGoldValue::thunk);
  PlayerModCrimeGoldValue::func =
    vtbl.write_vfunc(0xB6, PlayerModCrimeGoldValue::thunk);
  PlayerRemoveCrimeGoldValue::func =
    vtbl.write_vfunc(0xB7, PlayerRemoveCrimeGoldValue::thunk);
  PlayerGoToPrison::func = vtbl.write_vfunc(0xB9, PlayerGoToPrison::thunk);
  PlayerServePrisonTime::func =
    vtbl.write_vfunc(0xBA, PlayerServePrisonTime::thunk);
  PlayerClearAllCrimeGold::func =
    vtbl.write_vfunc(0x12D, PlayerClearAllCrimeGold::thunk);
}

/**
 * thuum docs/verbs/npc-fate.md (ADR-060: a thrall is its commander's game's
 * for the effect's life; ADR-063: one engine owns each piece of the world,
 * and a non-owner renders and never simulates), the reanimation look's D4 B
 * (thuum lab/.cache/drafts/reanimate-fx.md 4 and 6.4): a magic item that
 * carries an effect of archetype kReanimate (CommonLibSSE-NG
 * include/RE/E/EffectArchetypes.h:32) lands on a Character here only when
 * this game runs the raise. A cast relayed from another seat is a real cast
 * from its caster's figure (MagicApi.cpp castSpellImmediate), so a watcher's
 * engine would raise its own copy of the body beside the commander's, a
 * second simulator of one body (the look's read, HYPOTHESIS: the live word's
 * Resurrect(true, true), FateApi.cpp, resets the copy's inventory, a relayed
 * underskin with it, and the copy's own effect kills it a second time at its
 * end).
 *
 * The raise is the caster's game's, as the server names the commander
 * (ActionListener::ClaimReanimated: the caster's user for a player, else the
 * caster's host): this game's own player, or an actor this game simulates
 * (Hosted::SimulatedHere); a raise with no actor behind it, the body's
 * simulator's. The caster is read and not the body: the commander's engine
 * raises its copy at its own hit, before the server's grant makes the body
 * this game's (ADR-060's H2: a body this game neither hosts nor holds as a
 * puppet then), and the watcher still hosts its copy when the relayed
 * missile lands, the caster's report of its hit waiting two updates
 * (hitService.ts) before the grant's UpdateHoster can leave the server
 * (HYPOTHESIS, the order x-reanimate-look's U4 reads).
 *
 * Every effect of such an item is refused, one AddTarget per effect
 * (MagicTarget.h:58-60) as PlayerAddTarget refuses a potion's: the item is
 * its commander's whole, so the relayed cast lands nothing on the watcher's
 * copy, Reanimate Corpse's ash carrier 0xf52ab (ReanimateAshPile) and Dark
 * Souls rider 0xcab63, Dead Thrall's 0x8bb2a and a mod's added effects
 * alike. An item raises when its effect list (include/RE/M/MagicItem.h:127)
 * holds an effect whose base (include/RE/E/Effect.h:39) has the archetype
 * (include/RE/E/EffectSetting.h:141); with no item, the effect in hand
 * alone. Filtered in native, nothing crosses into JS: the item's few
 * effects compared per AddTarget on a Character, no engine call, and the
 * two sets read for a raising item alone. Each refused effect logs one line
 * under its own effect id.
 *
 * The slot is AddTarget, vfunc 01 (include/RE/M/MagicTarget.h:78), of
 * Character's MagicTarget vtable, RE::VTABLE_Character[4]
 * (include/RE/Offsets_VTABLE.h:2215, AE id 207894; `just addr 207894`
 * resolves it on 1.6.1170): TESObjectREFR's four vtables
 * (include/RE/T/TESObjectREFR.h:108-112), then Actor's MagicTarget base
 * (include/RE/A/Actor.h:126), the player's [4] above counted the same way.
 * The table's complete object locator on the 1.6.1170 image (read
 * statically) names Character with the MagicTarget base at offset 0xA0, and
 * its slot 01 holds the function the player's and Actor's tables hold
 * (Address Library 38786), so this write leaves PlayerAddTarget's table
 * alone. The body is GetTargetStatsObject, vfunc 02 (MagicTarget.h:79),
 * Actor's override answering the actor (include/RE/A/Actor.h:354; the
 * slot's body on the image is `lea rax, [rcx-0A0h]; ret`).
 */
struct CharacterAddTarget
{
  // thuum #84 (a player's conjured atronach as an owned actor; ADR-063): a
  // summon is its caster's game's as a raise is, by D4 B's rule. An item
  // carrying an effect of archetype kSummonCreature (CommonLibSSE-NG
  // include/RE/E/EffectArchetypes.h:28) is refused here exactly as a raise
  // is, so a cast relayed to a watcher's figure (MagicApi.cpp
  // castSpellImmediate) summons nothing on the watcher's engine: the
  // caster's engine summons, the server records its summon and every other
  // game shows the server's copy. The summon's own effect lands on its
  // caster (a Self or a target-location summon alike, HYPOTHESIS), so the
  // caster read below is the summoner, a figure's on a watcher. Returns the
  // archetype that makes the item its caster's game's, kNone for any other
  static RE::EffectSetting::Archetype OwnedArchetype(
    const RE::Effect* a_effect)
  {
    if (a_effect && a_effect->baseEffect) {
      const auto archetype = a_effect->baseEffect->GetArchetype();
      if (archetype == RE::EffectSetting::Archetype::kReanimate ||
          archetype == RE::EffectSetting::Archetype::kSummonCreature) {
        return archetype;
      }
    }
    return RE::EffectSetting::Archetype::kNone;
  }

  static RE::EffectSetting::Archetype Owned(RE::MagicItem* a_item,
                                            const RE::Effect* a_effect)
  {
    if (!a_item) {
      return OwnedArchetype(a_effect);
    }
    for (auto* effect : a_item->effects) {
      const auto archetype = OwnedArchetype(effect);
      if (archetype != RE::EffectSetting::Archetype::kNone) {
        return archetype;
      }
    }
    return RE::EffectSetting::Archetype::kNone;
  }

  static bool thunk(RE::MagicTarget* a_this,
                    RE::MagicTarget::AddTargetData& a_data)
  {
    const auto owned = Owned(a_data.magicItem, a_data.effect);
    if (owned == RE::EffectSetting::Archetype::kNone) {
      return func(a_this, a_data);
    }
    const auto caster =
      a_data.caster ? a_data.caster->As<RE::Actor>() : nullptr;
    const auto body = a_this ? a_this->GetTargetStatsObject() : nullptr;
    const RE::TESObjectREFR* raiser = caster ? caster : body;
    if (!raiser || raiser->IsPlayerRef() ||
        Hosted::SimulatedHere(raiser->formID)) {
      return func(a_this, a_data);
    }
    const auto mgef = a_data.effect ? a_data.effect->baseEffect : nullptr;
    if (owned == RE::EffectSetting::Archetype::kSummonCreature) {
      logger::info("[verb:summon] summon {:x} (effect {:x}) on {:x} refused "
                   "here: the summon is {:x}'s game's, its caster's engine "
                   "summons (thuum #84, D4 B's rule)",
                   a_data.magicItem ? a_data.magicItem->formID : 0,
                   mgef ? mgef->formID : 0, body ? body->formID : 0,
                   raiser->formID);
      return false;
    }
    logger::info("[verb:npc-fate] reanimate {:x} (effect {:x}) on {:x} "
                 "refused here: the raise is {:x}'s game's, its commander's "
                 "engine raises the body (ADR-060, D4 B)",
                 a_data.magicItem ? a_data.magicItem->formID : 0,
                 mgef ? mgef->formID : 0, body ? body->formID : 0,
                 raiser->formID);
    return false;
  }
  static inline REL::Relocation<decltype(thunk)> func;
};

void InstallCharacterAddTargetHook()
{
  REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_Character[4] };
  CharacterAddTarget::func = vtbl.write_vfunc(0x1, CharacterAddTarget::thunk);
}

/**
 * thuum #84 (a player's conjured atronach as an owned actor): the summon
 * tracker's two seams, SummonCreatureEffect's own Update and Finish
 * (CommonLibSSE-NG include/RE/S/SummonCreatureEffect.h:17, :28; ActiveEffect's
 * slots 0x04 and 0x15, include/RE/A/ActiveEffect.h:66, :85) on its vtable,
 * RE::VTABLE_SummonCreatureEffect[0] (include/RE/Offsets_VTABLE.h:1942, AE
 * 206083; `just addr 206083` resolves it on 1.6.1170). The class overrides
 * both, so the write touches summon effects alone. Update runs the engine's
 * first (it places the summon, the design's static read, HYPOTHESIS until
 * lab/frida/summon-track.js's rows), then the tracker, which raises
 * summonStart once the actor is there; Finish runs the tracker first, while
 * the summon still stands, then the engine's (which kills, fades and deletes
 * it). Nothing here changes what the engine does.
 */
struct SummonCreatureEffectHooks
{
  static void Update(RE::SummonCreatureEffect* a_this, float a_delta)
  {
    update(a_this, a_delta);
    EventHandler::OnSummonUpdate(a_this);
  }

  static void Finish(RE::SummonCreatureEffect* a_this)
  {
    EventHandler::OnSummonFinish(a_this);
    finish(a_this);
  }

  static inline REL::Relocation<decltype(Update)> update;
  static inline REL::Relocation<decltype(Finish)> finish;
};

void InstallSummonCreatureEffectHooks()
{
  REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_SummonCreatureEffect[0] };
  SummonCreatureEffectHooks::update =
    vtbl.write_vfunc(0x4, SummonCreatureEffectHooks::Update);
  SummonCreatureEffectHooks::finish =
    vtbl.write_vfunc(0x15, SummonCreatureEffectHooks::Finish);
}

/**
 * thuum train 52, Fix 1 (D-R1 C's leveled gap; the plan's section 1.3):
 * SkyrimScript::AddItemFunctor's operator(), the latent AddItem's body that
 * lands a leveled list's roll in the container (the trace x-additem-functor
 * 20261110-151634: on a worker thread, 3.0 to 3.4 ms after the native's
 * kInProgress, its stackID the call's six of six, one container event per
 * landing inside it). Its vtable is CommonLibSSE-NG's
 * include/RE/Offsets_VTABLE.h:4616, VTABLE_SkyrimScript____AddItemFunctor
 * (AE id 218372; `just addr 218372` resolves it on 1.6.1170), its slot 01
 * the header's own (include/RE/D/DelayFunctor.h:35, operator(), `// 01`),
 * written with REL::Relocation::write_vfunc (include/REL/Relocation.h:
 * 392-399: the slot's old value returned, the new one written through
 * safe_write), the fork's idiom above. The thunk opens ScriptNatives'
 * FunctorWindow for the functor's stack (DelayFunctor.h:43, stackID) around
 * the original, so the container events the roll raises inside it name that
 * stack's parked call (ScriptNatives.h). operator() returns
 * BSScript::Variable by value: a member function returning a class by value
 * takes `this` in RCX and the caller's return buffer in RDX and returns the
 * buffer in RAX (the Microsoft x64 convention's return values; Raymond Chen,
 * The Old New Thing 2013-02-20: "The this pointer goes ahead of the output
 * structure pointer"; LLVM review D3618), as the trace measured on this
 * build (rax equal to rdx at entry, six of six), so the thunk names both,
 * `this` first: a static thunk declared to return Variable by value would
 * take its hidden pointer first and be wrong. Observe only: the original
 * runs whole, nothing is suppressed. The switch is the seam's
 * (scriptLeveledLandings, ScriptNatives.cpp): off, nothing is parked and the
 * window notes nothing. Ghidra cross-check of slot 01's target: UNVERIFIED
 * (Ghidra); the trace's live read of the slot at load, agreeing with the
 * type's RTTI, stands in for it
 */
constexpr std::size_t kAddItemFunctorCallSlot = 0x1;

struct AddItemFunctorCall
{
  static RE::BSScript::Variable* thunk(RE::SkyrimScript::DelayFunctor* a_this,
                                       RE::BSScript::Variable* a_result)
  {
    const ScriptNatives::FunctorWindow window(a_this ? a_this->stackID : 0);
    return func(a_this, a_result);
  }
  static inline REL::Relocation<decltype(thunk)> func;
};

// the slot's address and what it held before the write (Hooks.h: the seam's
// install line and its read-back), 0 before the install
namespace {
std::uintptr_t g_addItemFunctorSlot = 0;
std::uintptr_t g_addItemFunctorSlotWas = 0;
}

void InstallAddItemFunctorHook()
{
  REL::Relocation<std::uintptr_t> vtbl{
    RE::VTABLE_SkyrimScript____AddItemFunctor[0]
  };
  g_addItemFunctorSlot =
    vtbl.address() + sizeof(void*) * kAddItemFunctorCallSlot;
  AddItemFunctorCall::func =
    vtbl.write_vfunc(kAddItemFunctorCallSlot, AddItemFunctorCall::thunk);
  g_addItemFunctorSlotWas = AddItemFunctorCall::func.address();
}

/**
 * thuum ADR-037 (docs/DECISIONS.md, proposed; docs/research/apocalypse/
 * target-location.md): the server's numbers alone reach a player (ADR-028),
 * and an explosion a figure owns deals its EXPL record's damage to the
 * player through the engine's own damage chain with the figure as attacker,
 * never through AddTarget (the re-analyst on sky-re, 2026-10-17: 43849, then
 * 37335, then the write to Health). Where the engine hands the player's
 * actor its health damage, Actor::HandleHealthDamage (CommonLibSSE-NG
 * include/RE/A/Actor.h:458, vfunc 0x104: the attacker and the damage),
 * damage from a figure (a dynamic form, the other players' actors in this
 * game) is dropped. Melee and arrows from figures already deal zero
 * (attackDamageMult 0, skymp5-client formView.ts), so this changes nothing
 * for them. HYPOTHESIS until a lab run: that this virtual applies the
 * number rather than reporting it after (Skyrim Together gates 37335, one
 * call earlier, docs/TE-MAP.md; the fallback). The player's own and an
 * NPC's damage pass as before.
 */
struct PlayerHealthDamage
{
  static void thunk(RE::Actor* a_this, RE::Actor* a_attacker, float a_damage)
  {
    if (a_attacker && a_attacker != a_this &&
        a_attacker->formID >= 0xFF000000) {
      return;
    }
    func(a_this, a_attacker, a_damage);
  }
  static inline REL::Relocation<decltype(thunk)> func;
};

void InstallPlayerHealthDamageHook()
{
  REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[0] };
  PlayerHealthDamage::func =
    vtbl.write_vfunc(0x104, PlayerHealthDamage::thunk);
}

/**
 * thuum docs/verbs/npc-fate.md (playtest 37's finding 54): a necromancer
 * hosted by one seat reanimated a dead bandit; the body stood on that
 * screen alone, attacked nobody, and the server never learned it was alive,
 * so the other seat's puppet stayed a body. On its host a dead actor keeps
 * its AI off (PuppetApi.cpp Promote, npc-fate H-54e: a package would walk a
 * body), so the engine revived a body with no AI, and it stood with no NPC
 * in it. For an actor this seat hosts (Hosted.h, the server's word per
 * actor) the AI is enabled before the engine's own Actor::Resurrect
 * (CommonLibSSE-NG include/RE/A/Actor.h:369, vfunc 0AB) runs, and left on
 * after it.
 *
 * The slot serves the console's ResurrectActor and Papyrus's Actor.Resurrect
 * (the read docs/research/m3/re-resurrect-hook.md: the console executor
 * 22037 and the VM functor 54639 both call slot 0xAB; the slot holds 37321,
 * Actor::Resurrect's body, which CommonLib's own src/RE/A/Actor.cpp:1355
 * relocates there on SE and AE). It does not serve the Reanimate effect:
 * ReanimateEffect::Start (34750) never calls the slot or 37321; it reaches
 * the body through its own function, 38820, which the Frida hook in
 * FridaHooks.cpp (OnReanimateEnter) covers. Playtest 37's reanimation logged
 * nothing here for that reason, not because the slot was wrong.
 *
 * The life state is read before and after through CommonLib's accessor
 * (Actor::AsActorState, Actor.h:736; ActorState::GetLifeState,
 * include/RE/A/ActorState.h:158; the enum at ActorState.h:7-18: kAlive 0,
 * kDying 1, kDead 2, kUnconcious 3, kReanimate 4, kRecycle 5). 37321 zeroes
 * actorState1 inside the call (38989 in the read), so the after read needs
 * no deferral. The AI is not turned off again whatever the after read says:
 * the dead state clears inside the call, and a body left with its AI off is
 * finding 54's standing body. HYPOTHESIS (the read is static): the AI bit
 * (Actor::BOOL_BITS::kProcessMe, Actor.h:160, what EnableAI writes,
 * Actor.h:521) is what kept the lab's earlier Resurrect from taking on an
 * AI-off actor, the reason FateApi::ResurrectActor wraps its call in
 * EnableAI(true); the read lists no AI check inside 37321, so the two life
 * states logged here are the evidence. The host's sampler (skymp5-client
 * hostedStateService) then reads the flip through IsDead (Actor.h:345,
 * actorState1 alone) and the server records the resurrection
 * (ActionListener::OnHostedActorState, a Respawn without a teleport), whose
 * live word stands the puppets up. A puppet is not touched here: a
 * watcher's copy follows the server's word as today (FateApi.cpp
 * resurrectActor). The slot is written on Character's primary vtable
 * (RE::VTABLE_Character[0], include/RE/Offsets_VTABLE.h:2215), the class
 * every NPC and creature is; Character overrides no Resurrect
 * (include/RE/C/Character.h:30-57), and the player has its own vtable
 * (VTABLE_PlayerCharacter, include/RE/P/PlayerCharacter.h:245) that this
 * write never touches. PlayerHealthDamage's shape above (write_vfunc,
 * include/REL/Relocation.h:392-406).
 */
struct HostedResurrect
{
  static void thunk(RE::Actor* a_this, bool a_resetInventory, bool a_attach3D)
  {
    // Actor::IsAIEnabled (Actor.h:579), TESForm::formID (include/RE/T/
    // TESForm.h:354); a hosted actor with its AI on has nothing to enable
    const bool taken = a_this && !a_this->IsAIEnabled() &&
      Hosted::SimulatedHere(a_this->formID);
    if (!taken) {
      func(a_this, a_resetInventory, a_attach3D);
      return;
    }
    // Actor::AsActorState (Actor.h:736), ActorState::GetLifeState
    // (ActorState.h:158), Actor::EnableAI (Actor.h:521)
    const auto before =
      static_cast<uint32_t>(a_this->AsActorState()->GetLifeState());
    a_this->EnableAI(true);
    func(a_this, a_resetInventory, a_attach3D);
    const auto after =
      static_cast<uint32_t>(a_this->AsActorState()->GetLifeState());
    logger::info("[verb:npc-fate] resurrect {:x}: hosted body, AI enabled "
                 "for the engine's own resurrection (life state before {}, "
                 "after {})",
                 a_this->formID, before, after);
  }
  static inline REL::Relocation<decltype(thunk)> func;
};

void InstallHostedResurrectHook()
{
  REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_Character[0] };
  HostedResurrect::func = vtbl.write_vfunc(0xAB, HostedResurrect::thunk);
}

/**
 * The graph's answers to a cast request (thuum docs/verbs/cast-action.md).
 * The engine's caster sets the hand's want variable and performs the Attack
 * action only when the graph answers with the BeginCastRight (BeginCastLeft,
 * BeginCastVoice) graph event; the response dispatcher (38447 on 1.6.1170)
 * hands it to the hand's functor through slot 1 of its vtable
 * (RightHandSpellCastHandler and kin, CommonLibSSE-NG
 * include/RE/Offsets_VTABLE.h:2392-2394; the re-analyst on sky-re,
 * 2026-10-15: slot 1 is 42820, 42819, 42821, the only way in). SKSE owns that
 * slot: skse64 writes its own function there for its action events, over
 * any plugin's write (fork 5df863fd's write_vfunc never fired; slot-probe.js
 * on the running game found skse64_1_6_1170.dll+0xd760 in it). So the
 * answer is read where SKSE publishes it: SKSE::ActionEvent kSpellCast
 * (slot kLeft, kRight) and kVoiceCast (include/SKSE/Events.h:37-67), through
 * SKSE::GetActionEventSource() (include/SKSE/API.h:31), one event per answer
 * for every actor. The sink notes the answer for its hand by the actor's
 * form id; the view performs the start when castReady says so. The player's
 * own answers are the engine's business and are not noted. Registered at
 * kDataLoaded, once.
 */
class CastAnswerSink final : public RE::BSTEventSink<SKSE::ActionEvent>
{
public:
  static CastAnswerSink* GetSingleton()
  {
    static CastAnswerSink singleton;
    return &singleton;
  }

  RE::BSEventNotifyControl ProcessEvent(
    const SKSE::ActionEvent* a_event,
    RE::BSTEventSource<SKSE::ActionEvent>*) override
  {
    if (!a_event || !a_event->actor) {
      return RE::BSEventNotifyControl::kContinue;
    }
    if (a_event->actor == RE::PlayerCharacter::GetSingleton()) {
      // the player's own action events go to JS as skseAction (ADR-034: an
      // engine moment comes from SKSE's event where SKSE publishes one): the
      // shout's fire for the breath report (docs/verbs/shouts.md), later the
      // cast's hand and the draw state. Raised on the next tick, the way
      // actionPerformed is; no game call in its handler
      const uint32_t type = static_cast<uint32_t>(a_event->type.get());
      const uint32_t slot = static_cast<uint32_t>(a_event->slot.get());
      const uint32_t sourceForm =
        a_event->sourceForm ? a_event->sourceForm->formID : 0;
      SkyrimPlatform::GetSingleton()->AddTickTask(
        [type, slot, sourceForm](Napi::Env env) {
          auto obj = Napi::Object::New(env);
          AddObjProperty(
            &obj, "actor",
            static_cast<RE::TESForm*>(RE::PlayerCharacter::GetSingleton()),
            "Actor");
          AddObjProperty(&obj, "type", type);
          AddObjProperty(&obj, "slot", slot);
          AddObjProperty(&obj, "sourceForm", sourceForm);
          EventsApi::SendEvent("skseAction", { obj });
        });
      return RE::BSEventNotifyControl::kContinue;
    }
    uint32_t bit = 0;
    if (a_event->type == SKSE::ActionEvent::Type::kSpellCast) {
      bit = a_event->slot == SKSE::ActionEvent::Slot::kLeft ? 1 : 2;
    } else if (a_event->type == SKSE::ActionEvent::Type::kVoiceCast) {
      bit = 4;
    }
    if (bit) {
      Frida::NoteCastReady(a_event->actor->formID, bit);
    }
    return RE::BSEventNotifyControl::kContinue;
  }
};

void Hooks::InstallCastAnswerSink()
{
  static bool installed = false;
  if (installed) {
    return;
  }
  auto* source = SKSE::GetActionEventSource();
  if (!source) {
    logger::error("[verb:cast-action] no SKSE action event source; a "
                  "figure's start goes without the graph's answer");
    return;
  }
  source->AddEventSink(CastAnswerSink::GetSingleton());
  installed = true;
  logger::info("[verb:cast-action] cast answer sink on SKSE's action events "
               "(the player's raised to JS as skseAction)");
}

void BindNativeMethod(RE::BSScript::Internal::VirtualMachine* thisArg,
                      RE::BSScript::IFunction* func);

decltype(&BindNativeMethod) _BindNativeMethod;

void HookVirtualMachineBind()
{
  spdlog::info("Hooking VirtualMachine::Bind");
  REL::Relocation<std::uintptr_t> Vtbl{
    RE::BSScript::Internal::VirtualMachine::VTABLE[0]
  };
  _BindNativeMethod = reinterpret_cast<decltype(_BindNativeMethod)>(
    Vtbl.write_vfunc(0x18, BindNativeMethod));
}

void BindNativeMethod(RE::BSScript::Internal::VirtualMachine* thisArg,
                      RE::BSScript::IFunction* func)
{
  std::stringstream memory;

  for (int i = 0; i < 100; i++) {
    uint8_t* funcPtr = reinterpret_cast<uint8_t*>(func);
    if (i % 8 == 0) {
      memory << std::hex << '[' << (int)i << ']' << ' ';
    }
    memory << std::hex << (int)funcPtr[i] << ' ';
  }

  uint8_t* raw = reinterpret_cast<uint8_t*>(func);
  uintptr_t realFunc = *reinterpret_cast<uintptr_t*>(raw + 0x50);

  uintptr_t moduleBase =
    hook::internal::GetAllocationBase(reinterpret_cast<void*>(realFunc));
  uintptr_t funcOffset = realFunc - moduleBase;

  // The SKSE runtime DLL is named after the game version it was built for
  // (skse64_1_6_1170.dll, skse64_1_7_104.dll); derive it from the running
  // executable instead of pinning one version here.
  const auto gameVersion = REL::Module::get().version();
  const std::string skseDll =
    fmt::format("skse64_{}_{}_{}.dll", gameVersion.major(),
                gameVersion.minor(), gameVersion.patch());
  auto skse = (uintptr_t)GetModuleHandleA(skseDll.c_str());

  uintptr_t isLongSignature =
    moduleBase == skse ? *reinterpret_cast<uint8_t*>(raw + 0x58) : 0;

  const char* funcName = func ? func->GetName().data() : "<null func>";
  const char* className =
    func ? func->GetObjectTypeName().data() : "<null IFunction>";
  spdlog::trace("VirtualMachine::Bind called {} {} {} funcOffset={:x}, "
                "realFunc={:x}, moduleBase={:x}",
                className, funcName, memory.str(), funcOffset, realFunc,
                moduleBase);

  if (func) {
    std::lock_guard<std::mutex> lock(hook::internal::g_mutex);
    hook::internal::g_boundNatives.push_back(
      { className, funcName, func, moduleBase, funcOffset, isLongSignature });
  }

  _BindNativeMethod(thisArg, func);
}

std::pair<std::uintptr_t, std::uintptr_t> Hooks::AddItemFunctorSlotWas()
{
  if (g_addItemFunctorSlotWas == 0) {
    return { 0, 0 };
  }
  const std::uintptr_t base = hook::internal::GetAllocationBase(
    reinterpret_cast<void*>(g_addItemFunctorSlotWas));
  return { base, g_addItemFunctorSlotWas - base };
}

bool Hooks::AddItemFunctorSlotHolds()
{
  return g_addItemFunctorSlot != 0 &&
    *reinterpret_cast<const std::uintptr_t*>(g_addItemFunctorSlot) ==
    stl::unrestricted_cast<std::uintptr_t>(AddItemFunctorCall::thunk);
}

std::vector<std::tuple<std::string, std::string, RE::BSScript::IFunction*,
                       uintptr_t, uintptr_t, uintptr_t>>
Hooks::GetBoundNatives()
{
  std::lock_guard<std::mutex> lock(hook::internal::g_mutex);
  return hook::internal::g_boundNatives;
}

void Hooks::Install()
{
  // InstallOnFrameUpdateHook();
  InstallOnConsoleVPrintHook();
  InstallQuitDisconnectHook();
  HookVirtualMachineBind();
  InstallPlayerAddTargetHook();
  InstallPlayerPrisonHooks();
  InstallCharacterAddTargetHook();
  InstallSummonCreatureEffectHooks();
  InstallPlayerHealthDamageHook();
  InstallHostedResurrectHook();
  InstallAddItemFunctorHook();

  logger::info("CommonLib hooks installed.");
}
