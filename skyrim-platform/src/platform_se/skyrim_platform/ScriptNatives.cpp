#include "ScriptNatives.h"
#include "EventsApi.h"
#include "FridaHookHandler.h"
#include "FridaHooks.h"
#include "Hooks.h"
#include "Hosted.h"
#include "JsUtils.h"
#include "Puppets.h"
#include "SkyrimPlatform.h"
#include <RE/A/Actor.h>
#include <RE/B/BGSBaseAlias.h>
#include <RE/F/FormTypes.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {
// what the native's self is: a reference (ObjectReference, Actor), a form
// (Quest, GlobalVariable) or an alias (ReferenceAlias: BGSBaseAlias, no
// form id of its own, so the owning quest's id and the alias id travel)
enum class SelfKind
{
  Ref,
  Form,
  Alias
};

struct Native
{
  const char* cls;
  const char* name;
  SelfKind self;
  // the script argument (0 the first after self) that names the acting or
  // target form, or -1
  int formArg;
  // the script argument carrying a number (a bool, an int or a float the
  // hook reads as an integer register; the float rows carry none), or -1
  int numberArg;
  bool numberIsBool;
};

// thuum docs/research/adr062/papyrus-native-entries.md's rows (the
// re-analyst's Ghidra read of 1.6.1170, 2026-11-02): every row's entry is
// `R f(BSScript::IVirtualMachine* vm, StackID stackId, Self* self, args...)`,
// so self is Gum's argument 2 and a script argument i is argument 3 + i.
// HYPOTHESIS until the first lab trace reads a sane self id per row.
const Native kNatives[] = {
  { "ObjectReference", "AddItem", SelfKind::Ref, 0, 1, false },
  { "ObjectReference", "RemoveItem", SelfKind::Ref, 0, 1, false },
  { "ObjectReference", "Enable", SelfKind::Ref, -1, 0, true },
  { "ObjectReference", "EnableNoWait", SelfKind::Ref, -1, 0, true },
  { "ObjectReference", "Disable", SelfKind::Ref, -1, 0, true },
  { "ObjectReference", "DisableNoWait", SelfKind::Ref, -1, 0, true },
  { "ObjectReference", "Delete", SelfKind::Ref, -1, -1, false },
  { "ObjectReference", "PlaceAtMe", SelfKind::Ref, 0, 1, false },
  { "ObjectReference", "PlaceActorAtMe", SelfKind::Ref, 0, 1, false },
  { "ObjectReference", "MoveTo", SelfKind::Ref, 0, -1, false },
  { "ObjectReference", "SetOpen", SelfKind::Ref, -1, 0, true },
  { "ObjectReference", "Lock", SelfKind::Ref, -1, 0, true },
  { "ObjectReference", "SetLockLevel", SelfKind::Ref, -1, 0, false },
  { "ObjectReference", "Activate", SelfKind::Ref, 0, 1, true },
  { "ObjectReference", "SetDestroyed", SelfKind::Ref, -1, 0, true },
  { "ObjectReference", "DamageObject", SelfKind::Ref, -1, -1, false },
  { "Actor", "Kill", SelfKind::Ref, 0, -1, false },
  { "Actor", "KillSilent", SelfKind::Ref, 0, -1, false },
  { "Actor", "Resurrect", SelfKind::Ref, -1, -1, false },
  { "Quest", "SetCurrentStageID", SelfKind::Form, -1, 0, false },
  { "Quest", "Start", SelfKind::Form, -1, -1, false },
  { "Quest", "Stop", SelfKind::Form, -1, -1, false },
  { "GlobalVariable", "SetValue", SelfKind::Form, -1, -1, false },
  { "ReferenceAlias", "ForceRefTo", SelfKind::Alias, 0, -1, false },
};

std::mutex g_mutex;
std::unordered_map<uintptr_t, const Native*> g_byEntry;
// the budget: at most kBudget events a second cross into JS, the rest are
// counted and dropped (rule 9), the count said once a window
constexpr uint32_t kBudget = 64;
uint64_t g_windowStartMs = 0;
uint32_t g_inWindow = 0;
uint32_t g_dropped = 0;

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

void OnNativeEnter(GumInvocationContext* ic)
{
  const Native* native = nullptr;
  {
    const auto entry = reinterpret_cast<uintptr_t>(ic->function);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_byEntry.find(entry);
    if (it == g_byEntry.end()) {
      return;
    }
    native = it->second;
  }
  auto* selfPtr = gum_invocation_context_get_nth_argument(ic, 2);
  if (!selfPtr) {
    return;
  }
  const auto stackId = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
    gum_invocation_context_get_nth_argument(ic, 1)));
  uint32_t selfId = 0;
  uint32_t cell = 0;
  bool actor = false;
  uint32_t aliasId = 0;
  bool hosted = false;
  switch (native->self) {
    case SelfKind::Ref: {
      auto* ref = reinterpret_cast<RE::TESObjectREFR*>(selfPtr);
      selfId = ref->formID;
      if (auto* parent = ref->GetParentCell()) {
        cell = parent->formID;
      }
      actor = ref->GetFormType() == RE::FormType::ActorCharacter;
      // a player's own actor and the figures are a player's (ADR-044's
      // rules); a live actor's call counts where this game simulates it; a
      // world object's in a cell this game hosts (ADR-062 decision 1)
      const bool ownedByAPlayer = selfId == 0x14 || selfId >= 0xff000000;
      if (ownedByAPlayer) {
        return;
      }
      hosted = actor ? (Hosted::Is(selfId) && !Puppets::Is(selfId))
                     : (cell != 0 && Frida::IsCellHosted(cell));
      break;
    }
    case SelfKind::Form: {
      auto* form = reinterpret_cast<RE::TESForm*>(selfPtr);
      selfId = form->formID;
      // a quest's or a global's call has no cell: every seat that runs the
      // script reports it, the server dedupes by its own rule
      hosted = true;
      break;
    }
    case SelfKind::Alias: {
      auto* alias = reinterpret_cast<RE::BGSBaseAlias*>(selfPtr);
      selfId = alias->owningQuest ? alias->owningQuest->formID : 0;
      aliasId = alias->aliasID;
      hosted = true;
      break;
    }
  }
  // rule 9: a call this game does not own never crosses the bridge
  if (!hosted) {
    return;
  }
  uint32_t formArg = 0;
  if (native->formArg >= 0) {
    auto* f = reinterpret_cast<RE::TESForm*>(
      gum_invocation_context_get_nth_argument(ic, 3 + native->formArg));
    if (f) {
      formArg = f->formID;
    }
  }
  int32_t value = 0;
  if (native->numberArg >= 0) {
    const auto raw = reinterpret_cast<uintptr_t>(
      gum_invocation_context_get_nth_argument(ic, 3 + native->numberArg));
    value = native->numberIsBool ? static_cast<int32_t>(raw & 0xff)
                                 : static_cast<int32_t>(raw & 0xffffffff);
  } else if (native->self == SelfKind::Alias) {
    value = static_cast<int32_t>(aliasId);
  }
  // the budget
  {
    const auto now = NowMs();
    std::lock_guard<std::mutex> lock(g_mutex);
    if (now - g_windowStartMs >= 1000) {
      if (g_dropped > 0) {
        logger::info("[verb:scripts] {} native call(s) over the budget of {} "
                     "a second were counted and dropped",
                     g_dropped, kBudget);
      }
      g_windowStartMs = now;
      g_inWindow = 0;
      g_dropped = 0;
    }
    if (++g_inWindow > kBudget) {
      ++g_dropped;
      return;
    }
  }
  const std::string name = std::string(native->cls) + "." + native->name;
  SkyrimPlatform::GetSingleton()->AddTickTask(
    [name, selfId, formArg, value, cell, actor, stackId](Napi::Env env) {
      auto obj = Napi::Object::New(env);
      AddObjProperty(&obj, "native", name);
      AddObjProperty(&obj, "self", selfId);
      AddObjProperty(&obj, "arg", formArg);
      AddObjProperty(&obj, "value", value);
      AddObjProperty(&obj, "cell", cell);
      AddObjProperty(&obj, "actor", actor);
      AddObjProperty(&obj, "stackId", stackId);
      EventsApi::SendEvent("scriptNative", { obj });
    });
}

void OnNativeLeave(GumInvocationContext*)
{
}
}

void ScriptNatives::InstallHooks()
{
  // the VM's bindings as Hooks.cpp recorded them (class, name, IFunction*,
  // module base, the callback's offset, the long-signature mark); the game's
  // own module alone (SKSE's natives are not scripts' consequences here)
  const auto gameBase = REL::Module::get().base();
  const auto bound = Hooks::GetBoundNatives();
  size_t installed = 0;
  bool first = true;
  for (const auto& native : kNatives) {
    uintptr_t entry = 0;
    for (const auto& [cls, name, func, moduleBase, offset, longSig] : bound) {
      if (moduleBase == gameBase && _stricmp(cls.c_str(), native.cls) == 0 &&
          _stricmp(name.c_str(), native.name) == 0) {
        entry = moduleBase + offset;
        break;
      }
    }
    if (entry == 0) {
      logger::warn("[verb:scripts] native {}.{} is not among the VM's "
                   "bindings; no hook",
                   native.cls, native.name);
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_byEntry.count(entry)) {
        // Enable and EnableNoWait share one body in the table's reading;
        // here each name has its own callback, so a shared entry is a
        // surprise worth a line
        logger::info("[verb:scripts] native {}.{} shares its entry +{:x} "
                     "with another; one hook serves both",
                     native.cls, native.name, entry - gameBase);
        continue;
      }
      g_byEntry[entry] = &native;
    }
    if (first) {
      Frida::HookHandler::GetSingleton()->Install(
        Frida::HookID::HOOK_SCRIPT_NATIVE, entry,
        std::make_shared<Frida::Hook>(OnNativeEnter, OnNativeLeave));
      first = false;
    } else {
      Frida::HookHandler::GetSingleton()->Attach(
        Frida::HookID::HOOK_SCRIPT_NATIVE, entry);
    }
    ++installed;
    // the offset for the reader, held against the table's ids by `just addr`
    logger::info("[verb:scripts] native hook {}.{} at +{:x}", native.cls,
                 native.name, entry - gameBase);
  }
  logger::info("[verb:scripts] {} of {} native entries hooked (ADR-062 "
               "slice 1b)",
               installed, std::size(kNatives));
}
