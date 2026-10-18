#include "Hooks.h"
#include "EventHandler.h"
#include "EventsApi.h"
#include "FridaHooks.h"
#include "JsUtils.h"
#include "PapyrusTESModPlatform.h"
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

  static bool IsCast(const RE::MagicItem* a_item)
  {
    using SpellType = RE::MagicSystem::SpellType;
    switch (a_item->GetSpellType()) {
      case SpellType::kSpell:
      case SpellType::kPower:
      case SpellType::kLesserPower:
      case SpellType::kVoicePower:
      case SpellType::kScroll:
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
  HookVirtualMachineBind();
  InstallPlayerAddTargetHook();

  logger::info("CommonLib hooks installed.");
}
