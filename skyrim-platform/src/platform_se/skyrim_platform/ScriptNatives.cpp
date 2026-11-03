#include "ScriptNatives.h"
#include "CallNativeApi.h"
#include "EventsApi.h"
#include "FridaHookHandler.h"
#include "FridaHooks.h"
#include "Hooks.h"
#include "Hosted.h"
#include "JsUtils.h"
#include "PlatformCall.h"
#include "Puppets.h"
#include "SkyrimPlatform.h"
#include <RE/A/Actor.h>
#include <RE/B/BGSBaseAlias.h>
#include <RE/F/FormTypes.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESQuest.h>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>

// the platform's current direct-call stack (main.cpp's OnUpdate): a native
// entered on it is the platform's own call, not a script frame's
extern CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

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
  // the death report's evidence: never dropped by the budget
  bool kill;
};

// thuum docs/research/adr062/papyrus-native-entries.md's rows (the
// re-analyst's Ghidra read of 1.6.1170, 2026-11-02): every row's entry is
// `R f(BSScript::IVirtualMachine* vm, StackID stackId, Self* self, args...)`,
// so self is Gum's argument 2 and a script argument i is argument 3 + i.
// HYPOTHESIS until a lab trace reads a sane self per row; until then the
// first calls of each row probe the self under SEH (ProbeSelf) and a row
// whose self is no form of its kind stands down instead of crashing the seat.
constexpr std::array<Native, 24> kNatives{ {
  { "ObjectReference", "AddItem", SelfKind::Ref, 0, 1, false, false },
  { "ObjectReference", "RemoveItem", SelfKind::Ref, 0, 1, false, false },
  { "ObjectReference", "Enable", SelfKind::Ref, -1, 0, true, false },
  { "ObjectReference", "EnableNoWait", SelfKind::Ref, -1, 0, true, false },
  { "ObjectReference", "Disable", SelfKind::Ref, -1, 0, true, false },
  { "ObjectReference", "DisableNoWait", SelfKind::Ref, -1, 0, true, false },
  { "ObjectReference", "Delete", SelfKind::Ref, -1, -1, false, false },
  { "ObjectReference", "PlaceAtMe", SelfKind::Ref, 0, 1, false, false },
  { "ObjectReference", "PlaceActorAtMe", SelfKind::Ref, 0, 1, false, false },
  { "ObjectReference", "MoveTo", SelfKind::Ref, 0, -1, false, false },
  { "ObjectReference", "SetOpen", SelfKind::Ref, -1, 0, true, false },
  { "ObjectReference", "Lock", SelfKind::Ref, -1, 0, true, false },
  { "ObjectReference", "SetLockLevel", SelfKind::Ref, -1, 0, false, false },
  { "ObjectReference", "Activate", SelfKind::Ref, 0, 1, true, false },
  { "ObjectReference", "SetDestroyed", SelfKind::Ref, -1, 0, true, false },
  { "ObjectReference", "DamageObject", SelfKind::Ref, -1, -1, false, false },
  { "Actor", "Kill", SelfKind::Ref, 0, -1, false, true },
  { "Actor", "KillSilent", SelfKind::Ref, 0, -1, false, true },
  { "Actor", "Resurrect", SelfKind::Ref, -1, -1, false, false },
  { "Quest", "SetCurrentStageID", SelfKind::Form, -1, 0, false, false },
  { "Quest", "Start", SelfKind::Form, -1, -1, false, false },
  { "Quest", "Stop", SelfKind::Form, -1, -1, false, false },
  { "GlobalVariable", "SetValue", SelfKind::Form, -1, -1, false, false },
  { "ReferenceAlias", "ForceRefTo", SelfKind::Alias, 0, -1, false, false },
} };

std::mutex g_mutex;
// entry address to the row's index in kNatives
std::unordered_map<uintptr_t, size_t> g_byEntry;
// rows whose self probe failed: the hook stands down for them
std::array<bool, kNatives.size()> g_bad{};
// how many calls of each row were probed so far
std::array<uint32_t, kNatives.size()> g_probed{};
constexpr uint32_t kProbeCalls = 3;
// each row's lower-case key for the latent notes (PlatformCall::TakeLatent)
std::array<std::string, kNatives.size()> g_lowerKeys{};
// the budget of per-call events crossing into JS: at most kBudget a second
// (rule 9), the rest tallied; a kill row is never dropped (the death
// report's evidence)
constexpr uint32_t kBudget = 64;
uint64_t g_windowStartMs = 0;
uint32_t g_inWindow = 0;
// the tallies, flushed to JS as one event per key every kFlushMs: the quest,
// global and alias rows (every seat runs those scripts; a count is the
// census, the per-call crossing is not worth it), the calls filtered as not
// hosted here, the platform's own, the budget's drops
constexpr uint64_t kFlushMs = 30'000;
std::map<std::string, uint32_t> g_tally;
uint64_t g_lastFlushMs = 0;

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

std::string NameOf(const Native& n)
{
  return std::string(n.cls) + "." + n.name;
}

// MSVC SEH: no object with a destructor lives in this function. Reads the
// self as a form of the row's kind; false on a fault or another kind (the
// convention HYPOTHESIS failing for this row). A reference of any placed
// kind passes (REFR, ACHR, the placed missiles, arrows, beams, flames,
// cones, barriers and hazards: TESForm::AsReference answers for them all)
bool ProbeSelf(void* selfPtr, int kind) noexcept
{
  __try {
    if (kind == 2) {
      auto* alias = reinterpret_cast<RE::BGSBaseAlias*>(selfPtr);
      RE::TESQuest* quest = alias->owningQuest;
      if (!quest) {
        return false;
      }
      return quest->GetFormType() == RE::FormType::Quest;
    }
    auto* form = reinterpret_cast<RE::TESForm*>(selfPtr);
    if (kind == 0) {
      return form->AsReference() != nullptr;
    }
    const auto type = form->GetFormType();
    return type == RE::FormType::Quest || type == RE::FormType::Global;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// the form id behind a form argument, 0 on a fault (the argument's slot is
// the convention's HYPOTHESIS too; MSVC SEH, no destructors here)
uint32_t ReadFormId(void* formPtr) noexcept
{
  __try {
    return formPtr ? reinterpret_cast<RE::TESForm*>(formPtr)->formID : 0u;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0u;
  }
}

void Emit(const std::string& name, uint32_t selfId, uint32_t formArg,
          int32_t value, uint32_t cell, bool actor, uint32_t stackId,
          uint32_t count)
{
  SkyrimPlatform::GetSingleton()->AddTickTask([name, selfId, formArg, value,
                                               cell, actor, stackId,
                                               count](Napi::Env env) {
    auto obj = Napi::Object::New(env);
    AddObjProperty(&obj, "native", name);
    AddObjProperty(&obj, "self", selfId);
    AddObjProperty(&obj, "arg", formArg);
    AddObjProperty(&obj, "value", value);
    AddObjProperty(&obj, "cell", cell);
    AddObjProperty(&obj, "actor", actor);
    AddObjProperty(&obj, "stackId", stackId);
    AddObjProperty(&obj, "count", count);
    EventsApi::SendEvent("scriptNative", { obj });
  });
}

// under g_mutex: flush every tally as one event per key once the window
// has passed (called at every tally and from the update's Tick, so a quiet
// seat emits its last window too)
void FlushIfDue(uint64_t now)
{
  if (g_lastFlushMs == 0) {
    g_lastFlushMs = now;
    return;
  }
  if (now - g_lastFlushMs < kFlushMs) {
    return;
  }
  g_lastFlushMs = now;
  if (g_tally.empty()) {
    return;
  }
  uint32_t total = 0;
  for (const auto& [name, count] : g_tally) {
    total += count;
    Emit(name, 0, 0, 0, 0, false, 0, count);
  }
  logger::info("[verb:scripts] {} tallied native call(s) in {} s over {} "
               "row(s) flushed to the census",
               total, kFlushMs / 1000, g_tally.size());
  g_tally.clear();
}

// under g_mutex: count one call under `key`
void Tally(const std::string& key, uint64_t now)
{
  ++g_tally[key];
  FlushIfDue(now);
}

void OnNativeEnter(GumInvocationContext* ic)
{
  size_t idx = 0;
  {
    const auto entry = reinterpret_cast<uintptr_t>(ic->function);
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_byEntry.find(entry);
    if (it == g_byEntry.end() || g_bad[it->second]) {
      return;
    }
    idx = it->second;
  }
  const Native& native = kNatives[idx];
  const auto now = NowMs();
  const auto stackId = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
    gum_invocation_context_get_nth_argument(ic, 1)));
  // the platform's own call (callNative's worker, a latent dispatch, an
  // SpSnippet): the client applying the server's word is no script's
  // consequence; counted apart
  if (PlatformCall::Active() ||
      (g_nativeCallRequirements.stackId != 0 &&
       stackId == g_nativeCallRequirements.stackId)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Tally(NameOf(native) + " (platform)", now);
    return;
  }
  auto* selfPtr = gum_invocation_context_get_nth_argument(ic, 2);
  if (!selfPtr) {
    return;
  }
  // the platform's own latent call, noted by (row, self) before its dispatch
  // (CallNative.cpp): the same self pointer the native receives
  if (PlatformCall::TakeLatent(g_lowerKeys[idx], selfPtr)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    Tally(NameOf(native) + " (platform)", now);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_probed[idx] < kProbeCalls) {
      if (!ProbeSelf(selfPtr, static_cast<int>(native.self))) {
        g_bad[idx] = true;
        logger::warn("[verb:scripts] native {}: the self at the entry is no "
                     "form of its kind (the convention HYPOTHESIS fails for "
                     "this row); the hook stands down for it",
                     NameOf(native));
        return;
      }
      ++g_probed[idx];
    }
  }
  uint32_t selfId = 0;
  uint32_t cell = 0;
  bool actor = false;
  uint32_t aliasId = 0;
  switch (native.self) {
    case SelfKind::Ref: {
      auto* ref = reinterpret_cast<RE::TESObjectREFR*>(selfPtr);
      selfId = ref->formID;
      if (auto* parent = ref->GetParentCell()) {
        cell = parent->formID;
      }
      actor = ref->GetFormType() == RE::FormType::ActorCharacter;
      // a live actor's call counts where this game simulates it (a host's
      // NPC, a server-made one included; never the player's own 0x14 nor a
      // figure, which the hosted set never names); a world object's in a
      // cell this game hosts (ADR-062 decision 1)
      const bool hosted = actor ? (Hosted::Is(selfId) && !Puppets::Is(selfId))
                                : (cell != 0 && Frida::IsCellHosted(cell));
      if (!hosted) {
        // rule 9: a call this game does not own never crosses the bridge;
        // the count is the census of what non-owners' scripts attempt
        std::lock_guard<std::mutex> lock(g_mutex);
        Tally(NameOf(native) + " (not hosted)", now);
        return;
      }
      break;
    }
    case SelfKind::Form:
    case SelfKind::Alias: {
      // a quest's, a global's or an alias's call has no cell and every seat
      // runs the script: counted, flushed with the census, no per-call event
      std::lock_guard<std::mutex> lock(g_mutex);
      Tally(NameOf(native), now);
      return;
    }
  }
  uint32_t formArg = 0;
  if (native.formArg >= 0) {
    formArg = ReadFormId(
      gum_invocation_context_get_nth_argument(ic, 3 + native.formArg));
  }
  int32_t value = 0;
  if (native.numberArg >= 0) {
    const auto raw = reinterpret_cast<uintptr_t>(
      gum_invocation_context_get_nth_argument(ic, 3 + native.numberArg));
    value = native.numberIsBool ? static_cast<int32_t>(raw & 0xff)
                                : static_cast<int32_t>(raw & 0xffffffff);
  }
  (void)aliasId;
  // the budget: a kill row always crosses (the death report's evidence)
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (now - g_windowStartMs >= 1000) {
      g_windowStartMs = now;
      g_inWindow = 0;
    }
    if (!native.kill && ++g_inWindow > kBudget) {
      Tally(NameOf(native) + " (over budget)", now);
      return;
    }
  }
  Emit(NameOf(native), selfId, formArg, value, cell, actor, stackId, 1);
}

void OnNativeLeave(GumInvocationContext*)
{
}
}

void ScriptNatives::Register()
{
  for (size_t idx = 0; idx < kNatives.size(); ++idx) {
    g_lowerKeys[idx] =
      PlatformCall::LatentKey(kNatives[idx].cls, kNatives[idx].name);
  }
  Frida::HookHandler::GetSingleton()->Register(
    Frida::HookID::HOOK_SCRIPT_NATIVE,
    std::make_shared<Frida::Hook>(OnNativeEnter, OnNativeLeave));
}

void ScriptNatives::Tick()
{
  std::lock_guard<std::mutex> lock(g_mutex);
  FlushIfDue(NowMs());
}

void ScriptNatives::InstallHooks()
{
  // the VM's bindings as Hooks.cpp recorded them (class, name, IFunction*,
  // module base, the callback's offset, the long-signature mark); the game's
  // own module alone (SKSE's natives are not scripts' consequences here)
  const auto gameBase = REL::Module::get().base();
  const auto bound = Hooks::GetBoundNatives();
  size_t attached = 0;
  for (size_t idx = 0; idx < kNatives.size(); ++idx) {
    const Native& native = kNatives[idx];
    uintptr_t entry = 0;
    for (const auto& [cls, name, func, moduleBase, offset, longSig] : bound) {
      if (moduleBase == gameBase && _stricmp(cls.c_str(), native.cls) == 0 &&
          _stricmp(name.c_str(), native.name) == 0) {
        entry = moduleBase + offset;
        break;
      }
    }
    if (entry == 0) {
      logger::warn("[verb:scripts] native {} is not among the VM's bindings; "
                   "no hook",
                   NameOf(native));
      continue;
    }
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_byEntry.count(entry)) {
        // the table reads Enable and EnableNoWait as separate entries over
        // one body; a shared entry would be a surprise worth a line
        logger::info("[verb:scripts] native {} shares its entry +{:x} with "
                     "another; one hook serves both",
                     NameOf(native), entry - gameBase);
        continue;
      }
      g_byEntry[entry] = idx;
    }
    if (!Frida::HookHandler::GetSingleton()->Attach(
          Frida::HookID::HOOK_SCRIPT_NATIVE, entry)) {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_byEntry.erase(entry);
      logger::warn("[verb:scripts] native {} at +{:x}: Gum refused the "
                   "attach; no hook",
                   NameOf(native), entry - gameBase);
      continue;
    }
    ++attached;
    // the offset for the reader, held against the table's ids by `just addr`
    logger::info("[verb:scripts] native hook {} at +{:x}", NameOf(native),
                 entry - gameBase);
  }
  logger::info("[verb:scripts] {} of {} native entries hooked (ADR-062 "
               "slice 1b)",
               attached, kNatives.size());
}
