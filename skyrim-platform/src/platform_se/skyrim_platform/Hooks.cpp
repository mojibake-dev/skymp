#include "Hooks.h"
#include "EventHandler.h"
#include "EventsApi.h"
#include "FridaHooks.h"
#include "Hosted.h"
#include "JsUtils.h"
#include "MpClientPluginApi.h"
#include "PapyrusTESModPlatform.h"
#include "Puppets.h"
#include "SkyrimPlatform.h"
#include <mutex>

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
 * thuum docs/verbs/magic-effects.md and docs/verbs/push.md (ADR-028, ADR-031):
 * while skymp5-client holds the server's values for the player
 * (TESModPlatform.SetPotionEffectsBlocked), the player's
 * MagicTarget::AddTarget (vfunc 01, CommonLibSSE-NG
 * include/RE/M/MagicTarget.h:78, called once per effect: AddTargetData's
 * caster, magic item and effect, :58-60) refuses the effects whose work is a
 * number the server computes, and nothing else:
 * - every effect of an alchemy item (a potion, a food, a poison: AlchemyItem's
 *   spell type is kAlchemy): the player's own drinks are the server's alone,
 *   and its effects stand alone (its ChangeValues, and the buffs its
 *   MagicEffects carries);
 * - an effect that another actor's spell, power, shout or scroll
 *   (include/RE/M/MagicSystem.h:60-80) lands on the player and that modifies
 *   Health, Magicka or Stamina: a value modifier archetype
 *   (include/RE/E/EffectArchetypes.h:10-44) by its primary or second actor
 *   value (include/RE/E/EffectSetting.h:89, :94). The server computes the
 *   damage one actor deals another and sends the percentages (ADR-028).
 * Every other effect of another actor's cast runs here as the engine plays
 * it (ADR-031: cut at the number, not the event). That includes its
 * stagger, its paralysis, its look, and its script: Unrelenting Force's
 * VoicePushEffectScript throws the player from the caster's figure. Until
 * 2026-10-14 skymp5-client dispelled a figure's whole spell from the player
 * at its hit, before the effects' first update, so their OnEffectStart was
 * never asked for and the push never ran (x-push2 20261014-143427 against
 * the solo x-push-solo 20261014-143127, lab/frida/push-trace.js). The
 * player's MagicTarget vtable is the fifth of PlayerCharacter's:
 * TESObjectREFR has four (VTABLE_TESObjectREFR) and Actor's MagicTarget base
 * follows them (include/RE/A/Actor.h:126), as VTABLE_Actor's ten count.
 */
struct PlayerAddTarget
{
  static bool IsServerValue(RE::ActorValue a_av)
  {
    return a_av == RE::ActorValue::kHealth ||
      a_av == RE::ActorValue::kMagicka || a_av == RE::ActorValue::kStamina;
  }

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

  // thuum ADR-037 (docs/DECISIONS.md, proposed): a figure's enchantment
  // counts as its cast too. An explosion a figure owns casts its EXPL's
  // enchantment at the player through the figure's instant caster, an
  // EnchantmentItem (the re-analyst on sky-re, 2026-10-17: the explosion
  // pass 43849), and so does a figure's enchanted weapon on a hit; neither
  // is the server's number (include/RE/M/MagicSystem.h:60-80 for the types)
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

  static bool thunk(RE::MagicTarget* a_this,
                    RE::MagicTarget::AddTargetData& a_data)
  {
    if (TESModPlatform::ArePotionEffectsBlocked() && a_data.magicItem) {
      if (a_data.magicItem->GetSpellType() ==
          RE::MagicSystem::SpellType::kAlchemy) {
        return false;
      }
      auto caster = a_data.caster ? a_data.caster->As<RE::Actor>() : nullptr;
      if (caster && !caster->IsPlayerRef() && IsCast(a_data.magicItem) &&
          ChangesServerValue(a_data.effect)) {
        return false;
      }
    }
    return func(a_this, a_data);
  }
  static inline REL::Relocation<decltype(thunk)> func;
};

void InstallPlayerAddTargetHook()
{
  REL::Relocation<std::uintptr_t> vtbl{ RE::VTABLE_PlayerCharacter[4] };
  PlayerAddTarget::func = vtbl.write_vfunc(0x1, PlayerAddTarget::thunk);
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
 * body), and under AI off the engine's own Actor::Resurrect (CommonLibSSE-NG
 * include/RE/A/Actor.h:369, vfunc 0AB) does not take: isDead stays true,
 * the lab's earlier finding and the reason FateApi::ResurrectActor wraps
 * the call in EnableAI(true) (FateApi.cpp). So for an actor this seat hosts
 * (Hosted.h, the server's word per actor) the AI is enabled before the
 * engine's own Resurrect runs, the call the reanimate effect's Start makes
 * (include/RE/R/ReanimateEffect.h:21; HYPOTHESIS, finding 54's read: that
 * Start reaches the body through this virtual). HYPOTHESIS: the AI bit
 * (Actor::BOOL_BITS::kProcessMe, Actor.h:160, what EnableAI writes,
 * Actor.h:521) is what blocks Resurrect on an AI-off actor. The engine then
 * stands the body with isDead false; the host's sampler (skymp5-client
 * hostedStateService) reports the flip reliable and the server records the
 * resurrection (ActionListener::OnHostedActorState, a Respawn without a
 * teleport), whose live word stands the puppets up. A puppet is not touched
 * here: a watcher's copy follows the server's word as today (FateApi.cpp
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
      Hosted::Is(a_this->formID) && !Puppets::Is(a_this->formID);
    if (!taken) {
      func(a_this, a_resetInventory, a_attach3D);
      return;
    }
    // Actor::IsDead (Actor.h:345), Actor::EnableAI (Actor.h:521)
    const bool deadBefore = a_this->IsDead();
    a_this->EnableAI(true);
    func(a_this, a_resetInventory, a_attach3D);
    const bool deadAfter = a_this->IsDead();
    if (deadAfter) {
      // the resurrection did not take (the hypothesis above failed): the
      // body keeps H-54e's AI off, so no package walks it
      a_this->EnableAI(false);
    }
    logger::info("[verb:npc-fate] resurrect {:x}: hosted body, AI enabled "
                 "for the engine's own resurrection (dead before {}, dead "
                 "after {}{})",
                 a_this->formID, deadBefore, deadAfter,
                 deadAfter ? ", AI off again" : "");
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
  InstallPlayerHealthDamageHook();
  InstallHostedResurrectHook();

  logger::info("CommonLib hooks installed.");
}
