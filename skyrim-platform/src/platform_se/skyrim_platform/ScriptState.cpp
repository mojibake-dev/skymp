#include "ScriptState.h"
#include "CallNativeApi.h"
#include "PlatformCall.h"
#include "TrapFamily.h"

#include <RE/A/Array.h>
#include <RE/B/BSAtomic.h>
#include <RE/F/FunctionArguments.h>
#include <RE/I/IFunction.h>
#include <RE/I/IObjectHandlePolicy.h>
#include <RE/O/Object.h>
#include <RE/O/ObjectTypeInfo.h>
#include <RE/S/SkyrimVM.h>
#include <RE/S/Stack.h>
#include <RE/S/StackFrame.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TypeInfo.h>
#include <RE/V/Variable.h>
#include <RE/V/VirtualMachine.h>
#include <RE/Z/ZeroFunctionArguments.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fmt/format.h>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// the platform's borrowed stack (main.cpp's OnUpdate: TESModPlatform.Add's
// own), whose top frame callNative rewrites for every native it calls, its
// self among them (CallNative.cpp), so the quiescence walk skips it
extern CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

/**
 * thuum docs/DECISIONS.md ADR-068 rung 3 (the script record; amendment A) and
 * the build plan docs/research/rung3-script-record-2026-11-06.md section 1(a);
 * ScriptState.h names the three natives' answers. The engine facts are
 * docs/research/adr065-papyrus-state-2026-11-03.md: Q1 (an object's layout,
 * its variables positional and parent-first, the handle's encoding), Q2
 * (FindBoundObject over attachedScripts), Q3 (the engine's own save:
 * Object::Save 104249's flag byte, its locks taken in the order
 * attachedScriptsLock, runningStacksLock, arraysLock; the loader's exact
 * match and its direct state write) and Q4 (SendEvent queues the handler;
 * the lifecycle sinks send with a ZeroFunctionArguments, then relay). Every
 * member read here is CommonLibSSE-NG's own declaration at the pin b93280e8
 * (file:line at its use), with the fork's overlay patch 02 (Variable's
 * members public) and 07 (SkyrimVM::GetImpl); no id, offset or slot is from
 * memory (thuum CLAUDE.md rule 1), and the natives call no engine function
 * CommonLib does not wrap.
 *
 * HYPOTHESIS (rule 2; the plan's section 5 names each one's observation):
 *   H-R2  a mid-latent object is told by a stack whose frame has it as self,
 *         in allRunningStacks or waitingLatentReturns
 *         (VirtualMachine.h:211-212), or by its lockStructure (Object.h:45):
 *         a read of 0xaa8cb 0.5 s into a turn answers quiescent false, a
 *         state-3 stack (x-script-state-read).
 *   H-R3  the direct currentState write runs no OnEndState or OnBeginState
 *         (the engine's loader writes the state the same way): zero
 *         DispatchMethodCall1 (104804) calls of either name between an
 *         apply's lines (script-state-trace.js).
 *   H-R4  the checks and the writes under the two locks are race-free, and
 *         deadlock-free from this thread (no engine path takes
 *         runningStacksLock, then attachedScriptsLock; CreateArray takes
 *         arraysLock after both, the save's own order): stacksOnObject 0
 *         and holdUs under 1000 at every apply.
 *   H-R6  a kind-0 handle names the same object on every seat; kinds 1 and 2
 *         (an inventory item's unique id, an active effect's) are a seat's
 *         own: myLinkedRef resolves, the seatLocal count.
 *   H-R10 SendEvent consumes its arguments before it returns (its
 *         DispatchMethodCall1 builds each handler's argument array at the
 *         dispatch, adr065 Q4), so a ZeroFunctionArguments on this stack
 *         outlives its use, as the engine's own sinks' does.
 *   H-R11 a variable's old array or object released by a write here is never
 *         its last reference on this thread (the VM's arrays table,
 *         VirtualMachine.h:224-227, holds every array; an object's release
 *         is the engine's own DecRef, Object.cpp:53-58), so no engine
 *         destructor runs under the two locks.
 *   H-R12 the TESLoadGameEvent sink runs before the load raises its first
 *         lifecycle event, so the seen set's clear there forgets the old
 *         world alone (H-L1's read, sendevent-rate.js at a login).
 *   H-W3  (docs/research/fifty-fixes-2026-11-07.md section 4) SendTrigger's
 *         argument, CommonLibSSE-NG's FunctionArguments<TESObjectREFR*>
 *         (include/RE/F/FunctionArguments.h:27-73), packs as the engine's own
 *         trigger argument does: one Variable, the actionRef's bound
 *         ObjectReference object found, or created and bound, through the
 *         handle policy (CommonLib's PackHandle, src/RE/P/PackUnpack.cpp:
 *         48-80; the engine's packer, lab/.cache/drafts/fifty/
 *         trigger-entry-static.md 2.10 and 3.1): the host's pack row under
 *         a relay reads agree true with the figure's handle (x-cobweb-relay
 *         v1 with lab/frida/trigger-entry-trace.js).
 */
namespace {
using ScriptObject = RE::BSScript::Object;
using ScriptType = RE::BSScript::ObjectTypeInfo;
using RawType = RE::BSScript::TypeInfo::RawType;

// the record's caps, the wire's (the plan's 1(b): SCRIPT_OBJECTS 16,
// SCRIPT_VARS 512 and SCRIPT_STRING 1024, UNMEASURED; SCRIPT_ARRAY 128, the
// engine's own Array::MAX_SIZE, CommonLibSSE-NG include/RE/A/Array.h:69;
// PAPYRUS_NAME 128, wire-schema's cap on a Papyrus name), so a record the
// validator would refuse is refused here first, and the answer's 160 KiB
// under the 192 KiB the ScriptState message takes
constexpr std::size_t kMaxScripts = 16;
constexpr std::size_t kMaxVars = 512;
constexpr std::size_t kMaxString = 1024;
constexpr std::size_t kMaxArray = 128;
constexpr std::size_t kMaxName = 128;
constexpr std::size_t kMaxAnswer = 160 * 1024;
// a class chain and a stack's frames walked at most (cuts, not measures;
// AdmittedByChain's 16 in FridaHooks.cpp), and the stacks a busy text names
// (the rest counted)
constexpr std::size_t kMaxChain = 16;
constexpr int kMaxFrames = 1024;
constexpr std::size_t kBusyStacksNamed = 8;
// the seen set's handles at most, then cleared once with a line (a cut, not
// a measure: OnCellDetach drops a handle, so the set holds about the
// attached references)
constexpr std::size_t kSeenCap = 65536;

// the three lifecycle events SendScriptEvent raises and the seen set knows,
// each with no argument, their spelling and their bit
struct Lifecycle
{
  const char* name;
  std::uint8_t bit;
};

constexpr Lifecycle kLifecycle[] = { { "OnCellAttach", 1 },
                                     { "OnLoad", 2 },
                                     { "OnCellLoad", 4 } };

const Lifecycle* LifecycleOf(std::string_view name)
{
  for (const Lifecycle& event : kLifecycle) {
    if (name.size() == std::strlen(event.name) &&
        _strnicmp(name.data(), event.name, name.size()) == 0) {
      return &event;
    }
  }
  return nullptr;
}

// the two trigger events SendTriggerEvent raises, each with its one argument
// (akActionRef, the Creation Kit's OnTriggerEnter and OnTriggerLeave), in
// their spelling
constexpr const char* kTriggerEvents[] = { "OnTriggerEnter",
                                           "OnTriggerLeave" };

const char* TriggerEventOf(std::string_view name)
{
  for (const char* event : kTriggerEvents) {
    if (name.size() == std::strlen(event) &&
        _strnicmp(name.data(), event, name.size()) == 0) {
      return event;
    }
  }
  return nullptr;
}

// the pending references (ScriptState.h), each with the names the gate
// blanked for it, in Hosted.h's idiom: mutex-guarded, the size kept for a
// read with no lock
struct
{
  std::unordered_map<std::uint32_t, std::vector<std::string>> marks;
  std::mutex m;
  std::atomic<std::size_t> size{ 0 };
} g_pending;

// the seen set (ScriptState.h): a handle's served lifecycle events as
// Lifecycle bits
struct
{
  std::unordered_map<RE::VMHandle, std::uint8_t> bits;
  std::mutex m;
} g_seen;

// the names blanked for a pending reference: read for a refusal, taken with
// the mark for an applied record
std::vector<std::string> Blanked(std::uint32_t formId, bool take)
{
  std::lock_guard l(g_pending.m);
  const auto it = g_pending.marks.find(formId);
  if (it == g_pending.marks.end()) {
    return {};
  }
  std::vector<std::string> names = it->second;
  if (take) {
    g_pending.marks.erase(it);
    g_pending.size.store(g_pending.marks.size());
  }
  return names;
}

// a kind of line logged for its first three and every 100th (the gate's
// throttle, FridaHooks.cpp)
std::atomic<std::uint64_t> g_readLines{ 0 };
std::atomic<std::uint64_t> g_busyLines{ 0 };
std::atomic<std::uint64_t> g_refusedLines{ 0 };
std::atomic<std::uint64_t> g_retryLines{ 0 };
std::atomic<std::uint64_t> g_sendLines{ 0 };
std::atomic<std::uint64_t> g_triggerSendLines{ 0 };

bool LogThis(std::atomic<std::uint64_t>& lines)
{
  const auto n = ++lines;
  return n <= 3 || n % 100 == 0;
}

// FNV-1a 32 over the dump's bytes (ChangeForm.cpp's)
std::uint32_t Fnv1a(const std::string& bytes)
{
  std::uint32_t h = 0x811c9dc5u;
  for (const char c : bytes) {
    h ^= static_cast<std::uint8_t>(c);
    h *= 0x01000193u;
  }
  return h;
}

// well-formed UTF-8 (RFC 3629: no overlong form, no surrogate, nothing past
// U+10FFFF). nlohmann's dump would replace a bad byte, so a string in another
// code page is refused here, never sent altered
bool IsUtf8(std::string_view s)
{
  std::size_t i = 0;
  while (i < s.size()) {
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    std::size_t n = 0;
    std::uint32_t cp = 0;
    if ((c & 0xE0) == 0xC0) {
      n = 1;
      cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
      n = 2;
      cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
      n = 3;
      cp = c & 0x07;
    } else {
      return false;
    }
    if (s.size() - i <= n) {
      return false;
    }
    for (std::size_t j = 1; j <= n; ++j) {
      const auto d = static_cast<unsigned char>(s[i + j]);
      if ((d & 0xC0) != 0x80) {
        return false;
      }
      cp = (cp << 6) | (d & 0x3F);
    }
    if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
        (n == 3 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
      return false;
    }
    i += n + 1;
  }
  return true;
}

// a name the wire carries (PAPYRUS_NAME): at most kMaxName bytes of UTF-8
bool NameFits(const char* name)
{
  return name && std::strlen(name) <= kMaxName && IsUtf8(name);
}

// the HYPOTHESIS encoding of a VM handle (adr065 Q1, four decompiles: 53508,
// 53512, 53522, 53523; H-R1's trace read kinds 0 to 2 and a plain form as
// kind 0 with mid 0xFFFF): bits 48 to 63 a kind, 32 to 47 a middle word, 0 to
// 31 a form id, as ScriptNatives.cpp's IsPlainFormHandle reads it
struct HandleParts
{
  std::uint32_t kind = 0;
  std::uint32_t mid = 0;
  std::uint32_t form = 0;
};

HandleParts Split(RE::VMHandle handle)
{
  HandleParts parts;
  parts.kind = static_cast<std::uint32_t>(handle >> 48);
  parts.mid = static_cast<std::uint32_t>((handle >> 32) & 0xFFFF);
  parts.form = static_cast<std::uint32_t>(handle & 0xFFFFFFFF);
  return parts;
}

RE::VMHandle Join(const HandleParts& parts)
{
  return (static_cast<RE::VMHandle>(parts.kind & 0xFFFF) << 48) |
    (static_cast<RE::VMHandle>(parts.mid & 0xFFFF) << 32) | parts.form;
}

// a value's folded kind, the save's (Variable::Save folds a class pointer to
// 1 and a class array to 11, adr065 Q1): TypeInfo::GetUnmangledRawType
// (CommonLibSSE-NG include/RE/T/TypeInfo.h:52, src/RE/T/TypeInfo.cpp:91-99)
int Fold(const TypeInfo& type)
{
  return static_cast<int>(type.GetUnmangledRawType());
}

bool KnownKind(std::int64_t k)
{
  return (k >= 0 && k <= 5) || (k >= 10 && k <= 15);
}

// the class an object or object-array type names: its raw type with bit 0
// cleared, only past kArraysEnd ("the type id for classes IS their class
// ptr; an object is an array if its first bit is set", TypeInfo.h:28-31),
// null below it; never TypeInfo::GetTypeInfo, whose array mask clears bit 3
// of the pointer as well (TypeInfo.cpp:81-89; VmProvider.cpp's
// MakeValueType)
const ScriptType* ClassOf(const TypeInfo& type)
{
  const auto raw = static_cast<std::size_t>(type.GetRawType());
  if (raw < static_cast<std::size_t>(RawType::kArraysEnd)) {
    return nullptr;
  }
  return reinterpret_cast<const ScriptType*>(
    raw & ~static_cast<std::size_t>(RawType::kObject));
}

const char* NameOf(const ScriptType* type)
{
  return type ? type->GetName() : "";
}

// the script object a Variable holds, read in place (patch 02;
// ScriptNatives.cpp's ObjectOf): null for None or another type
const ScriptObject* ObjectOf(const Variable& v)
{
  return v.IsObject() ? v.value.obj.get() : nullptr;
}

bool Holds(const std::vector<ScriptObject*>& objects, const ScriptObject* self)
{
  return self &&
    std::find(objects.begin(), objects.end(), self) != objects.end();
}

// one declared variable: its name (for the lab) and its declared type
struct Declared
{
  std::string name;
  TypeInfo type;
};

// a script's declared variables in variables[]'s order: its class chain
// (ObjectTypeInfo::GetParent, ObjectTypeInfo.h:127-128) walked ancestors
// first, each class's VariableInfo list (:51-58, :149-152;
// ObjectTypeInfo.cpp:83-93) in declaration order (Object.cpp:84-112, H-R1);
// false when a class is not linked (its data blob is then an unlinked
// native-function list, ObjectTypeInfo.h:180, and GetVariableIter reads
// garbage), the chain is past kMaxChain, or the count is not the chain's
// total (GetTotalNumVariables, ObjectTypeInfo.cpp:74-81)
bool DeclaredOf(const ScriptType* type, std::vector<Declared>& out)
{
  out.clear();
  std::vector<const ScriptType*> chain;
  for (const ScriptType* cls = type; cls; cls = cls->GetParent()) {
    if (chain.size() >= kMaxChain || !cls->IsLinked()) {
      return false;
    }
    chain.push_back(cls);
  }
  std::reverse(chain.begin(), chain.end());
  for (const ScriptType* cls : chain) {
    const auto* vars = cls->GetVariableIter();
    const std::uint32_t n = cls->GetNumVariables();
    if (n > 0 && !vars) {
      return false;
    }
    for (std::uint32_t i = 0; i < n; ++i) {
      out.push_back(Declared{ vars[i].name.c_str(), vars[i].type });
    }
  }
  return type && out.size() == type->GetTotalNumVariables();
}

// a state the chain declares: "" (the empty state) or a NamedStateInfo name
// of a class in it (ObjectTypeInfo.h:95-122, :170-172), without case
bool StateDeclared(const ScriptType* type, const std::string& state)
{
  if (state.empty()) {
    return true;
  }
  std::size_t depth = 0;
  for (const ScriptType* cls = type; cls && depth < kMaxChain;
       cls = cls->GetParent(), ++depth) {
    const auto* states = cls->GetNamedStateIter();
    for (std::uint32_t i = 0; states && i < cls->GetNumNamedStates(); ++i) {
      if (_stricmp(states[i].name.c_str(), state.c_str()) == 0) {
        return true;
      }
    }
  }
  return false;
}

// Object::Save's flag byte (104249, adr065 Q3): constructed 0x01, valid
// 0x02, a lock holder 0x04, initialized 0x08, the lock pointer's flag bit
// 0x10 (Object.h:39-45); a quiescent object reads 0x0B
std::uint32_t FlagsOf(const ScriptObject* object)
{
  const auto lock = reinterpret_cast<std::uintptr_t>(object->lockStructure);
  std::uint32_t flags = 0;
  flags |= object->IsConstructed() ? 0x01u : 0u;
  flags |= object->IsValid() ? 0x02u : 0u;
  flags |= (lock & ~std::uintptr_t{ 1 }) != 0 ? 0x04u : 0u;
  flags |= object->IsInitialized() ? 0x08u : 0u;
  flags |= (lock & std::uintptr_t{ 1 }) != 0 ? 0x10u : 0u;
  return flags;
}

// the handle's attached script objects, the gate's walk (FridaHooks.cpp
// OnSendEventEnter; attachedScripts, VirtualMachine.h:220), the caller
// holding attachedScriptsLock; the array's size is read, never its empty(),
// inverted in the pinned headers (BSTArray.h:749)
bool AttachedObjects(VM* vm, RE::VMHandle handle,
                     std::vector<ScriptObject*>& out)
{
  out.clear();
  const auto it = vm->attachedScripts.find(handle);
  if (it == vm->attachedScripts.end()) {
    return false;
  }
  auto& scripts = it->second;
  for (std::uint32_t i = 0; i < scripts.size(); ++i) {
    out.push_back(scripts[i].get());
  }
  return !out.empty();
}

// the stacks on the objects (the plan's finding 2; H-R2): each stack of
// allRunningStacks and waitingLatentReturns (VirtualMachine.h:211-212, the
// caller holding runningStacksLock, :210) one of whose frames, walked from
// its top (Stack.h:76) down previousFrame (StackFrame.h:22), has one of the
// objects as self (StackFrame.h:27); the platform's borrowed stack skipped.
// Each one counted, and named into `named` when asked, the first
// kBusyStacksNamed of them: "stack <id> state <state> in <type>.<function>"
// (Stack.h:32's states, 3 waiting on a latent; H-R2's reading)
std::uint32_t StacksOn(VM* vm, const std::vector<ScriptObject*>& objects,
                       std::vector<std::string>* named)
{
  const RE::VMStackID borrowed = g_nativeCallRequirements.stackId;
  std::uint32_t n = 0;
  const auto walk = [&](const auto& stacks) {
    for (const auto& entry : stacks) {
      const RE::BSScript::Stack* stack = entry.second.get();
      if (!stack || stack->stackID == borrowed) {
        continue;
      }
      int depth = 0;
      for (const RE::BSScript::StackFrame* frame = stack->top;
           frame && depth < kMaxFrames;
           frame = frame->previousFrame, ++depth) {
        if (!Holds(objects, ObjectOf(frame->self))) {
          continue;
        }
        ++n;
        if (named && named->size() < kBusyStacksNamed) {
          const RE::BSScript::IFunction* fn = frame->owningFunction.get();
          named->push_back(fmt::format(
            "stack {} state {} in {}.{}", stack->stackID,
            stack->state.underlying(), NameOf(frame->owningObjectType.get()),
            fn ? fn->GetName().c_str() : ""));
        }
        break;
      }
    }
  };
  walk(vm->allRunningStacks);
  walk(vm->waitingLatentReturns);
  return n;
}

// the objects' quiescence (the plan's finding 2; H-R2): "" when each is
// initialized (Object.h:23, :40), holds no lock (lockStructure, :45, the
// Stack holding the object's lock with a flag bit, adr065 Q1) and no stack
// is on it; else every reason joined with "; ": each object's own word (a
// null script, its lock held, not initialized), then each stack on the
// objects as StacksOn names it, the stacks walked whatever the words, so a
// locked object's text still names its stack and that stack's state (the
// review's FIX FIRST 3: x-script-state-read's claim reads "state 3"); the
// caller holds both locks
std::string BusyText(VM* vm, const std::vector<ScriptObject*>& objects)
{
  std::vector<std::string> parts;
  for (const ScriptObject* object : objects) {
    if (!object) {
      parts.push_back("a null attached script");
      continue;
    }
    const char* name = NameOf(object->GetTypeInfo());
    if (object->lockStructure) {
      parts.push_back(fmt::format("{} locked by a stack", name));
    }
    if (!object->IsInitialized()) {
      parts.push_back(fmt::format("{} not initialized", name));
    }
  }
  std::vector<std::string> stacks;
  const std::uint32_t n = StacksOn(vm, objects, &stacks);
  parts.insert(parts.end(), stacks.begin(), stacks.end());
  if (n > stacks.size()) {
    parts.push_back(fmt::format("{} more stack(s) on it", n - stacks.size()));
  }
  std::string text;
  for (const std::string& part : parts) {
    if (!text.empty()) {
      text += "; ";
    }
    text += part;
  }
  return text;
}

// one value as the record carries it, into `out` (which holds its k): i, f, b
// or s for a literal (Variable.h:62-69), o for an object, absent for a typed
// None and for an object with no portable handle, counted in `seatLocal`
// (the player's own 0x14, a runtime form at or above 0xff000000, an unbound
// object's empty handle, an unknown kind; kinds 1 and 2 travel, for the
// receiver to turn to None, H-R6); false with `why` for a value the wire
// cannot carry
bool EncodeScalar(const Variable& v, int k, nlohmann::json& out,
                  std::uint32_t& seatLocal, std::string& why)
{
  switch (k) {
    case 0:
      return true;
    case 1: {
      const ScriptObject* object = ObjectOf(v);
      if (!object) {
        return true;
      }
      const HandleParts parts = Split(object->GetHandle());
      if (parts.kind > 2 || parts.form == 0 || parts.form == 0x14 ||
          parts.form >= 0xff000000) {
        ++seatLocal;
        return true;
      }
      const char* type = NameOf(object->GetTypeInfo());
      if (!NameFits(type)) {
        why = fmt::format("an object of a class named past {} bytes or not "
                          "in UTF-8",
                          kMaxName);
        return false;
      }
      nlohmann::json o = nlohmann::json::object();
      o["kind"] = parts.kind;
      o["mid"] = parts.mid;
      o["form"] = parts.form;
      o["type"] = type;
      out["o"] = std::move(o);
      return true;
    }
    case 2: {
      const std::string_view s = v.GetString();
      if (s.size() > kMaxString || !IsUtf8(s)) {
        why = fmt::format("a string of {} bytes, past {} or not in UTF-8",
                          s.size(), kMaxString);
        return false;
      }
      out["s"] = std::string(s);
      return true;
    }
    case 3:
      out["i"] = v.GetSInt();
      return true;
    case 4: {
      const float f = v.GetFloat();
      if (!std::isfinite(f)) {
        why = "a float that is not finite";
        return false;
      }
      out["f"] = f;
      return true;
    }
    case 5:
      out["b"] = v.GetBool();
      return true;
    default:
      break;
  }
  why = fmt::format("a value of kind {}", k);
  return false;
}

// one variable as the record carries it (ScriptVar): its folded kind and
// value; an array (10 to 15) as elem (an object array's element class) and
// a, its elements (Array.h:73-77, at most 128 by :69) each of kind k less 10
// (a None element of an object array a typed None), absent for a None array
bool EncodeVar(const Variable& v, nlohmann::json& out,
               std::uint32_t& seatLocal, std::string& why)
{
  const int k = Fold(v.GetType());
  out = nlohmann::json::object();
  out["k"] = k;
  if (k < 10) {
    return EncodeScalar(v, k, out, seatLocal, why);
  }
  if (k > 15) {
    why = fmt::format("a value of kind {}", k);
    return false;
  }
  if (k == 11) {
    if (const ScriptType* cls = ClassOf(v.GetType())) {
      if (!NameFits(cls->GetName())) {
        why = fmt::format("an array of a class named past {} bytes or not in "
                          "UTF-8",
                          kMaxName);
        return false;
      }
      out["elem"] = cls->GetName();
    }
  }
  const RE::BSScript::Array* array = k == 10 ? nullptr : v.value.arr.get();
  if (!array) {
    return true;
  }
  if (array->size() > kMaxArray) {
    why = fmt::format("an array of {} elements, past {}", array->size(),
                      kMaxArray);
    return false;
  }
  nlohmann::json items = nlohmann::json::array();
  for (std::uint32_t j = 0; j < array->size(); ++j) {
    const Variable& element = (*array)[j];
    int ek = Fold(element.GetType());
    if (k == 11 && ek == 0) {
      ek = 1;
    }
    if (ek != k - 10) {
      why =
        fmt::format("element {} of kind {} in an array of kind {}", j, ek, k);
      return false;
    }
    nlohmann::json item = nlohmann::json::object();
    item["k"] = ek;
    std::string elementWhy;
    if (!EncodeScalar(element, ek, item, seatLocal, elementWhy)) {
      why = fmt::format("element {}: {}", j, elementWhy);
      return false;
    }
    items.push_back(std::move(item));
  }
  out["a"] = std::move(items);
  return true;
}

// the record's scripts for a set of objects (ScriptObjectState: type,
// state, flags, vars), their variables' names apart; the caller holds both
// locks
struct Dump
{
  nlohmann::json scripts = nlohmann::json::array();
  std::vector<std::vector<std::string>> names;
  std::uint32_t seatLocal = 0;
};

bool DumpObjects(const std::vector<ScriptObject*>& objects, Dump& dump,
                 std::string& why)
{
  if (objects.size() > kMaxScripts) {
    why =
      fmt::format("{} attached scripts, past {}", objects.size(), kMaxScripts);
    return false;
  }
  std::vector<Declared> declared;
  for (const ScriptObject* object : objects) {
    if (!object) {
      why = "a null attached script";
      return false;
    }
    const ScriptType* type = object->GetTypeInfo();
    const char* typeName = NameOf(type);
    if (!type || !DeclaredOf(type, declared)) {
      why = fmt::format("{}: its class chain is not linked or its variables "
                        "do not add up to GetTotalNumVariables",
                        typeName);
      return false;
    }
    if (declared.size() > kMaxVars) {
      why = fmt::format("{}: {} variables, past {}", typeName, declared.size(),
                        kMaxVars);
      return false;
    }
    const char* state = object->currentState.c_str();
    if (!NameFits(typeName) || !NameFits(state)) {
      why = fmt::format("{}: a type or state name past {} bytes or not in "
                        "UTF-8",
                        typeName, kMaxName);
      return false;
    }
    nlohmann::json vars = nlohmann::json::array();
    std::vector<std::string> names;
    for (std::size_t i = 0; i < declared.size(); ++i) {
      nlohmann::json var;
      std::string varWhy;
      if (!EncodeVar(object->variables[i], var, dump.seatLocal, varWhy)) {
        why = fmt::format("{}.{}: {}", typeName, declared[i].name, varWhy);
        return false;
      }
      vars.push_back(std::move(var));
      names.push_back(declared[i].name);
    }
    nlohmann::json script = nlohmann::json::object();
    script["type"] = typeName;
    script["state"] = state;
    script["flags"] = FlagsOf(object);
    script["vars"] = std::move(vars);
    dump.scripts.push_back(std::move(script));
    dump.names.push_back(std::move(names));
  }
  return true;
}

std::string DumpText(const nlohmann::json& j)
{
  return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// the premises Serialize and Apply share: a reference that is no actor (its
// simulator's state is a later rung), no runtime reference and no trap
// family reference (the seam's rule, ScriptNatives.cpp OnCall; ADR-068 D-S3:
// every seat runs its own), the VM and its handle policy; the reference's
// handle from the policy (GetHandleForObject, IObjectHandlePolicy.h:33)
bool Premises(RE::TESObjectREFR* refr, VM*& vm, RE::VMHandle& handle,
              std::string& why)
{
  if (!refr) {
    why = "no reference";
    return false;
  }
  if (refr->GetFormType() == RE::FormType::ActorCharacter) {
    why = "an actor (its simulator's state is a later rung)";
    return false;
  }
  if (refr->formID >= 0xff000000) {
    why = "a runtime reference (0xff000000 and up), this seat's own";
    return false;
  }
  vm = VM::GetSingleton();
  auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
  if (!policy) {
    why = "no VM or no handle policy";
    return false;
  }
  handle = policy->GetHandleForObject(refr->GetFormType(), refr);
  if (TrapFamily::IsFamily(refr, handle)) {
    why = "a trap family reference (every seat runs its own, ADR-068 D-S3)";
    return false;
  }
  return true;
}

// one value of a served record (ScriptValue), parsed before the locks
struct RecValue
{
  int k = 0;
  std::int32_t i = 0;
  float f = 0.f;
  bool b = false;
  std::string s;
  // an object's handle and class; absent for a typed None
  bool hasObject = false;
  HandleParts handle;
  std::string type;
};

// one variable (ScriptVar): its value, an object array's element class, an
// array's elements (absent for a None array)
struct RecVar
{
  RecValue value;
  bool hasElem = false;
  std::string elem;
  bool hasArray = false;
  std::vector<RecValue> items;
};

// one script (ScriptObjectState); flags and names are not read
struct RecScript
{
  std::string type;
  std::string state;
  std::vector<RecVar> vars;
};

// a member present and not null (an Option the wire's JSON writes as null
// reads as absent)
const nlohmann::json* Member(const nlohmann::json& obj, const char* key)
{
  if (!obj.is_object()) {
    return nullptr;
  }
  const auto it = obj.find(key);
  return it == obj.end() || it->is_null() ? nullptr : &*it;
}

// an integer member within [lo, hi]
bool ReadInt(const nlohmann::json* v, std::int64_t lo, std::int64_t hi,
             std::int64_t& out)
{
  if (!v || !v->is_number_integer()) {
    return false;
  }
  if (v->is_number_unsigned()) {
    const auto u = v->get<std::uint64_t>();
    if (u >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
      return false;
    }
    out = static_cast<std::int64_t>(u);
  } else {
    out = v->get<std::int64_t>();
  }
  return out >= lo && out <= hi;
}

// a string member of at most `max` bytes with no NUL (BSFixedString's
// constructor reads a C string)
bool ReadString(const nlohmann::json* v, std::size_t max, std::string& out)
{
  if (!v || !v->is_string()) {
    return false;
  }
  out = v->get<std::string>();
  return out.size() <= max && out.find('\0') == std::string::npos;
}

// one value: its kind and exactly the payload the kind names (the
// validator's rule, the plan's 1(b)); a float as any JSON number (a writer
// may print 1.0 as 1)
bool ParseValue(const nlohmann::json& j, RecValue& out, std::string& why)
{
  std::int64_t k = 0;
  if (!j.is_object() || !ReadInt(Member(j, "k"), 0, 15, k) || !KnownKind(k)) {
    why = "a value with no kind or an unknown one";
    return false;
  }
  out.k = static_cast<int>(k);
  const nlohmann::json* i = Member(j, "i");
  const nlohmann::json* f = Member(j, "f");
  const nlohmann::json* b = Member(j, "b");
  const nlohmann::json* s = Member(j, "s");
  const nlohmann::json* o = Member(j, "o");
  if ((k == 3) != (i != nullptr) || (k == 4) != (f != nullptr) ||
      (k == 5) != (b != nullptr) || (k == 2) != (s != nullptr) ||
      (o != nullptr && k != 1)) {
    why = fmt::format("a value of kind {} whose payload does not match it", k);
    return false;
  }
  std::int64_t n = 0;
  switch (out.k) {
    case 1:
      if (o) {
        std::int64_t kind = 0;
        std::int64_t mid = 0;
        std::int64_t form = 0;
        if (!ReadInt(Member(*o, "kind"), 0, 0xFFFF, kind) ||
            !ReadInt(Member(*o, "mid"), 0, 0xFFFF, mid) ||
            !ReadInt(Member(*o, "form"), 0, 0xFFFFFFFF, form) ||
            !ReadString(Member(*o, "type"), kMaxName, out.type) ||
            out.type.empty()) {
          why = "an object with no handle or no class";
          return false;
        }
        out.hasObject = true;
        out.handle.kind = static_cast<std::uint32_t>(kind);
        out.handle.mid = static_cast<std::uint32_t>(mid);
        out.handle.form = static_cast<std::uint32_t>(form);
      }
      return true;
    case 2:
      if (!ReadString(s, kMaxString, out.s)) {
        why = fmt::format("a string past {} bytes or with a NUL", kMaxString);
        return false;
      }
      return true;
    case 3:
      if (!ReadInt(i, std::numeric_limits<std::int32_t>::min(),
                   std::numeric_limits<std::int32_t>::max(), n)) {
        why = "an int that is no 32-bit integer";
        return false;
      }
      out.i = static_cast<std::int32_t>(n);
      return true;
    case 4: {
      const double d = f->is_number()
        ? f->get<double>()
        : std::numeric_limits<double>::quiet_NaN();
      if (!std::isfinite(d) ||
          std::fabs(d) > std::numeric_limits<float>::max()) {
        why = "a float that is not a finite number";
        return false;
      }
      out.f = static_cast<float>(d);
      return true;
    }
    case 5:
      if (!b->is_boolean()) {
        why = "a bool that is no boolean";
        return false;
      }
      out.b = b->get<bool>();
      return true;
    default:
      break;
  }
  return true;
}

bool ParseVar(const nlohmann::json& j, RecVar& out, std::string& why)
{
  if (!ParseValue(j, out.value, why)) {
    return false;
  }
  const int k = out.value.k;
  if (const nlohmann::json* elem = Member(j, "elem")) {
    if (k != 11 || !ReadString(elem, kMaxName, out.elem)) {
      why = "elem on a value that is no object array";
      return false;
    }
    out.hasElem = true;
  }
  if (const nlohmann::json* a = Member(j, "a")) {
    if (k < 11 || !a->is_array() || a->size() > kMaxArray) {
      why = fmt::format("a on a value that is no array, or past {} elements",
                        kMaxArray);
      return false;
    }
    for (const nlohmann::json& item : *a) {
      RecValue element;
      if (!ParseValue(item, element, why)) {
        return false;
      }
      if (element.k != k - 10) {
        why = fmt::format("an element of kind {} in an array of kind {}",
                          element.k, k);
        return false;
      }
      out.items.push_back(std::move(element));
    }
    out.hasArray = true;
  }
  return true;
}

bool ParseRecord(const nlohmann::json& scripts, std::vector<RecScript>& out,
                 std::string& why)
{
  if (!scripts.is_array() || scripts.empty() || scripts.size() > kMaxScripts) {
    why = fmt::format("no scripts, or past {}", kMaxScripts);
    return false;
  }
  for (std::size_t n = 0; n < scripts.size(); ++n) {
    const nlohmann::json& script = scripts[n];
    RecScript rec;
    const nlohmann::json* vars = Member(script, "vars");
    if (!ReadString(Member(script, "type"), kMaxName, rec.type) ||
        rec.type.empty() ||
        !ReadString(Member(script, "state"), kMaxName, rec.state) || !vars ||
        !vars->is_array() || vars->size() > kMaxVars) {
      why = fmt::format("script {}: no type, state or vars, or past a cap", n);
      return false;
    }
    for (std::size_t m = 0; m < vars->size(); ++m) {
      RecVar var;
      std::string varWhy;
      if (!ParseVar((*vars)[m], var, varWhy)) {
        why = fmt::format("script {} ({}) variable {}: {}", n, rec.type, m,
                          varWhy);
        return false;
      }
      rec.vars.push_back(std::move(var));
    }
    out.push_back(std::move(rec));
  }
  return true;
}

// the apply's counts, the answer's: object values resolved (found or kept),
// written as None (the record's own and the unresolved ones), unresolved,
// and seat-local (a handle of kind 1 or 2, or one no seat shares)
struct ApplyCounts
{
  std::uint32_t resolved = 0;
  std::uint32_t none = 0;
  std::uint32_t unresolved = 0;
  std::uint32_t seatLocal = 0;
};

// a record's variable against its declared type (the chain's VariableInfo,
// ObjectTypeInfo.h:51-58): the same folded kind, or None (kind 0) into an
// object or an array variable and a None array (kind 10) into a typed array
// variable, both written as the declared type's None; an object array's
// element class, when the record names one, the declared one's
bool KindFits(const RecVar& var, const TypeInfo& declared, std::string& why)
{
  const int kd = Fold(declared);
  const int k = var.value.k;
  if (k == kd) {
    const ScriptType* cls = k == 11 ? ClassOf(declared) : nullptr;
    if (cls && var.hasElem &&
        _stricmp(cls->GetName(), var.elem.c_str()) != 0) {
      why = fmt::format("an array of {} into an array of {}", var.elem,
                        cls->GetName());
      return false;
    }
    return true;
  }
  if ((k == 0 && (kd == 1 || kd >= 10)) || (k == 10 && kd > 10)) {
    return true;
  }
  why = fmt::format("a value of kind {} into a variable of kind {}", k, kd);
  return false;
}

// a literal written over `target` through Variable::operator= (Variable.h:
// 40-41; Variable.cpp:49-69: Cleanup, then the copy or the move), the source
// built by the setters (:71-79; SetString re-interns, :310-314)
void WriteLiteral(Variable& target, const RecValue& value, int kd)
{
  Variable v;
  switch (kd) {
    case 2:
      v.SetString(value.s);
      break;
    case 3:
      v.SetSInt(value.i);
      break;
    case 4:
      v.SetFloat(value.f);
      break;
    case 5:
      v.SetBool(value.b);
      break;
    default:
      return;
  }
  target = std::move(v);
}

// an object written over `target` (an object variable, or an object array's
// element, of the declared type): a kind-0 handle's bound object of the
// record's class (FindBoundObject, IVirtualMachine.h:81: the first attached
// script of that class or of one extending it, adr065 Q2), set with the
// declared type (Variable::SetObject(obj, type), Variable.h:78), or kept as
// it is when the variable already holds that handle's object of that class
// (a VMAD property's own value on both seats, which a miss must not clear);
// else the declared type's None (Variable(const TypeInfo&), Variable.cpp:
// 24-27), never SetNone, which folds the type to kNone (:260-263): for the
// record's None, a handle of kind 1 or 2 (H-R6), one naming the player or a
// runtime form, or one that resolves to nothing here
void WriteObject(VM* vm, Variable& target, const RecValue& value,
                 const TypeInfo& declared, ApplyCounts& counts)
{
  if (!value.hasObject) {
    ++counts.none;
    target = Variable(declared);
    return;
  }
  const HandleParts& parts = value.handle;
  if (parts.kind != 0 || parts.form == 0 || parts.form == 0x14 ||
      parts.form >= 0xff000000) {
    ++counts.seatLocal;
    target = Variable(declared);
    return;
  }
  const RE::VMHandle handle = Join(parts);
  if (const ScriptObject* held = ObjectOf(target)) {
    if (held->GetHandle() == handle &&
        _stricmp(NameOf(held->GetTypeInfo()), value.type.c_str()) == 0) {
      ++counts.resolved;
      return;
    }
  }
  RE::BSTSmartPointer<ScriptObject> found;
  if (vm->FindBoundObject(handle, value.type.c_str(), found) && found) {
    Variable v;
    v.SetObject(std::move(found), declared.GetRawType());
    target = std::move(v);
    ++counts.resolved;
    return;
  }
  ++counts.none;
  ++counts.unresolved;
  target = Variable(declared);
}

// an array written over `target`: a new array of the declared element type
// (CreateArray, IVirtualMachine.h:108, takes the element type as CommonLib's
// own PackValue passes it, PackUnpackImpl.h:42-43: a literal array's kind
// less 10, a class array's class), its elements written as the scalars are,
// then set on the variable (Variable::SetArray types it as the array,
// Array.cpp's type()); the record's None array, or one the VM would not
// create, as the declared type's None. A shared array's aliasing is not
// carried (adr065 Q1)
void WriteArray(VM* vm, Variable& target, const RecVar& var,
                const TypeInfo& declared, ApplyCounts& counts)
{
  if (!var.hasArray) {
    target = Variable(declared);
    return;
  }
  const int kd = Fold(declared);
  TypeInfo element(RawType::kObject);
  if (kd == 11) {
    if (const ScriptType* cls = ClassOf(declared)) {
      element = TypeInfo(cls->GetRawType());
    }
  } else {
    element = TypeInfo(static_cast<RawType>(kd - 10));
  }
  const auto size = static_cast<std::uint32_t>(var.items.size());
  RE::BSTSmartPointer<RE::BSScript::Array> array;
  if (!vm->CreateArray(element, size, array) || !array ||
      array->size() != size) {
    ++counts.none;
    target = Variable(declared);
    return;
  }
  for (std::uint32_t j = 0; j < size; ++j) {
    Variable& slot = (*array)[j];
    if (kd == 11) {
      WriteObject(vm, slot, var.items[j], element, counts);
    } else {
      WriteLiteral(slot, var.items[j], kd - 10);
    }
  }
  Variable v;
  v.SetArray(std::move(array));
  target = std::move(v);
}

void WriteVar(VM* vm, Variable& target, const RecVar& var,
              const TypeInfo& declared, ApplyCounts& counts)
{
  const int kd = Fold(declared);
  if (kd == 1) {
    WriteObject(vm, target, var.value, declared, counts);
  } else if (kd >= 10) {
    WriteArray(vm, target, var, declared, counts);
  } else {
    WriteLiteral(target, var.value, kd);
  }
}

// one script of the record over this seat's object of exactly its class
// (the engine loader's exact match, adr065 Q2), the caller holding both
// locks: the count against the chain's total, each kind against its declared
// type and the state against the chain's states, all before the first
// write; then each variable in place (WriteVar) and currentState directly
// (Object.h:44; H-R3), as the engine's loader writes them
nlohmann::json ApplyScript(VM* vm, const std::vector<ScriptObject*>& objects,
                           const RecScript& script, ApplyCounts& counts,
                           std::size_t& applied)
{
  nlohmann::json result = nlohmann::json::object();
  result["type"] = script.type;
  result["applied"] = false;
  auto refused = [&](const std::string& reason) {
    result["reason"] = reason;
    return result;
  };
  ScriptObject* object = nullptr;
  for (ScriptObject* candidate : objects) {
    if (candidate &&
        _stricmp(NameOf(candidate->GetTypeInfo()), script.type.c_str()) == 0) {
      object = candidate;
      break;
    }
  }
  if (!object) {
    return refused("no attached script of this class");
  }
  const ScriptType* type = object->GetTypeInfo();
  std::vector<Declared> declared;
  if (!DeclaredOf(type, declared)) {
    return refused("its class chain is not linked or its variables do not "
                   "add up to GetTotalNumVariables");
  }
  if (script.vars.size() != declared.size()) {
    return refused(fmt::format("{} variables against the chain's {}",
                               script.vars.size(), declared.size()));
  }
  for (std::size_t i = 0; i < declared.size(); ++i) {
    std::string why;
    if (!KindFits(script.vars[i], declared[i].type, why)) {
      return refused(fmt::format("{}: {}", declared[i].name, why));
    }
  }
  if (!StateDeclared(type, script.state)) {
    return refused(fmt::format(
      "state '{}' is declared by no class of the chain", script.state));
  }
  for (std::size_t i = 0; i < declared.size(); ++i) {
    WriteVar(vm, object->variables[i], script.vars[i], declared[i].type,
             counts);
  }
  object->currentState = script.state.c_str();
  result["applied"] = true;
  result["vars"] = declared.size();
  ++applied;
  return result;
}

std::string SerializeImpl(RE::TESObjectREFR* refr)
{
  const RE::FormID formId = refr ? refr->formID : 0;
  auto refuse = [&](const std::string& reason) {
    if (LogThis(g_refusedLines)) {
      logger::info("[verb:scripts] SerializeScriptState refused {:x}: {}",
                   formId, reason);
    }
    return std::string();
  };
  VM* vm = nullptr;
  RE::VMHandle handle = 0;
  std::string why;
  if (!Premises(refr, vm, handle, why)) {
    return refuse(why);
  }
  Dump dump;
  std::string busy;
  bool attached = false;
  bool dumped = false;
  {
    RE::BSSpinLockGuard attachedGuard{ vm->attachedScriptsLock };
    RE::BSSpinLockGuard runningGuard{ vm->runningStacksLock };
    std::vector<ScriptObject*> objects;
    attached = AttachedObjects(vm, handle, objects);
    if (attached) {
      busy = BusyText(vm, objects);
      dumped = busy.empty() && DumpObjects(objects, dump, why);
    }
  }
  if (!attached) {
    return refuse("no attached script");
  }
  if (!busy.empty()) {
    if (LogThis(g_busyLines)) {
      logger::info("[verb:scripts] SerializeScriptState {:x} is not "
                   "quiescent: {}",
                   formId, busy);
    }
    nlohmann::json answer = nlohmann::json::object();
    answer["formId"] = formId;
    answer["quiescent"] = false;
    answer["busy"] = busy;
    return DumpText(answer);
  }
  if (!dumped) {
    return refuse(why);
  }
  // the record's hash over its scripts without their names, before the
  // names join them
  const std::uint32_t fnv = Fnv1a(DumpText(dump.scripts));
  for (std::size_t n = 0; n < dump.names.size() && n < dump.scripts.size();
       ++n) {
    dump.scripts[n]["names"] = std::move(dump.names[n]);
  }
  const std::size_t count = dump.scripts.size();
  nlohmann::json answer = nlohmann::json::object();
  answer["formId"] = formId;
  answer["quiescent"] = true;
  answer["fnv1a"] = fnv;
  answer["seatLocal"] = dump.seatLocal;
  answer["scripts"] = std::move(dump.scripts);
  std::string text = DumpText(answer);
  if (text.size() > kMaxAnswer) {
    return refuse(
      fmt::format("an answer of {} bytes, past 160 KiB", text.size()));
  }
  if (LogThis(g_readLines)) {
    logger::info("[verb:scripts] SerializeScriptState {:x}: {} script(s), "
                 "fnv {:x}, {} seat-local value(s), {} bytes",
                 formId, count, fnv, dump.seatLocal, text.size());
  }
  return text;
}

std::string ApplyImpl(RE::TESObjectREFR* refr, std::string_view scriptsJson)
{
  const RE::FormID formId = refr ? refr->formID : 0;
  nlohmann::json answer = nlohmann::json::object();
  answer["formId"] = formId;
  answer["applied"] = false;
  answer["retry"] = false;
  answer["reason"] = "";
  // a refusal keeps the pending mark and reads the names blanked (the
  // client's give-up lifts the mark); `retry` tells a moment's refusal from
  // a record that cannot apply here
  auto refuse = [&](const std::string& reason, bool retry) {
    answer["retry"] = retry;
    answer["reason"] = reason;
    answer["blanked"] = Blanked(formId, false);
    if (!retry || LogThis(g_retryLines)) {
      logger::info("[verb:scripts] ApplyScriptState {} {:x}: {}",
                   retry ? "waits on" : "refused", formId, reason);
    }
    return DumpText(answer);
  };
  VM* vm = nullptr;
  RE::VMHandle handle = 0;
  std::string why;
  if (!Premises(refr, vm, handle, why)) {
    return refuse(why, false);
  }
  const nlohmann::json parsed = nlohmann::json::parse(
    scriptsJson.data(), scriptsJson.data() + scriptsJson.size(), nullptr,
    false);
  if (parsed.is_discarded()) {
    return refuse("the record is no JSON", false);
  }
  const nlohmann::json* scripts =
    parsed.is_array() ? &parsed : Member(parsed, "scripts");
  std::vector<RecScript> record;
  if (!scripts || !ParseRecord(*scripts, record, why)) {
    return refuse(why.empty() ? "no scripts array" : why, false);
  }
  nlohmann::json results = nlohmann::json::array();
  ApplyCounts counts;
  std::size_t applied = 0;
  std::uint32_t stacksAfter = 0;
  std::uint32_t fnvAfter = 0;
  std::string fnvWhy;
  std::string wait;
  std::int64_t holdUs = 0;
  {
    RE::BSSpinLockGuard attachedGuard{ vm->attachedScriptsLock };
    RE::BSSpinLockGuard runningGuard{ vm->runningStacksLock };
    const auto held = std::chrono::steady_clock::now();
    std::vector<ScriptObject*> objects;
    if (!AttachedObjects(vm, handle, objects)) {
      wait = "no attached script yet";
    } else {
      const std::string busy = BusyText(vm, objects);
      if (!busy.empty()) {
        wait = "busy: " + busy;
      }
    }
    if (wait.empty()) {
      for (const RecScript& script : record) {
        results.push_back(ApplyScript(vm, objects, script, counts, applied));
      }
      // H-R4's reading: a stack on an object after the writes, still under
      // the locks, means the locks did not keep the scripts out
      stacksAfter = StacksOn(vm, objects, nullptr);
      Dump after;
      if (DumpObjects(objects, after, fnvWhy)) {
        fnvAfter = Fnv1a(DumpText(after.scripts));
      }
    }
    holdUs = std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now() - held)
               .count();
  }
  if (!wait.empty()) {
    return refuse(wait, true);
  }
  // applied: the record's scripts written under the locks, each one its own
  // checks refused named in scripts with its reason; the pending mark is
  // lifted here and the names blanked are taken with it
  const bool all = applied == record.size();
  answer["applied"] = true;
  answer["reason"] = all ? std::string()
                         : fmt::format("{} of {} script(s) refused",
                                       record.size() - applied, record.size());
  answer["scripts"] = std::move(results);
  answer["resolved"] = counts.resolved;
  answer["none"] = counts.none;
  answer["unresolved"] = counts.unresolved;
  answer["seatLocal"] = counts.seatLocal;
  answer["stacksOnObject"] = stacksAfter;
  answer["holdUs"] = holdUs;
  answer["fnvAfter"] = fnvAfter;
  if (!fnvWhy.empty()) {
    answer["fnvAfterRefused"] = fnvWhy;
  }
  answer["blanked"] = Blanked(formId, true);
  logger::info("[verb:scripts] ApplyScriptState {:x}: {} of {} script(s) "
               "applied, {} resolved, {} none ({} unresolved), {} seat-local, "
               "{} stack(s) on it after, held {} us, fnv after {:x}",
               formId, applied, record.size(), counts.resolved, counts.none,
               counts.unresolved, counts.seatLocal, stacksAfter, holdUs,
               fnvAfter);
  return DumpText(answer);
}

// SendScriptEvent's and SendTriggerEvent's one mechanism (the plan's read 2:
// one body, two entry points by the event's argument shape): the reference's
// VM handle and its attached scripts, counted under attachedScriptsLock
// (VirtualMachine.h:219-220)
struct EventTarget
{
  RE::SkyrimVM* skyrimVM = nullptr;
  RE::VMHandle handle = 0;
  std::uint32_t scripts = 0;
};

// false, with the reason, when the event has nothing to reach
bool TargetOf(RE::TESObjectREFR* refr, EventTarget& target, std::string& why)
{
  auto* skyrimVM = RE::SkyrimVM::GetSingleton();
  auto* vm = VM::GetSingleton();
  auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
  if (!skyrimVM || !skyrimVM->GetImpl() || !policy) {
    why = "no VM or no handle policy";
    return false;
  }
  target.skyrimVM = skyrimVM;
  target.handle = policy->GetHandleForObject(refr->GetFormType(), refr);
  {
    RE::BSSpinLockGuard guard{ vm->attachedScriptsLock };
    const auto it = vm->attachedScripts.find(target.handle);
    if (it != vm->attachedScripts.end()) {
      target.scripts = it->second.size();
    }
  }
  if (target.scripts == 0) {
    why = "no attached script";
    return false;
  }
  return true;
}

// SkyrimVM::SendAndRelayEvent's two calls (CommonLibSSE-NG
// src/RE/S/SkyrimVM.cpp:26-33: impl->SendEvent, then RelayEvent to the
// reference's aliases and effects), the VM taken through GetImpl as the
// fork's overlay patch 07 asks every caller (the implementation pointer sits
// at 0x210 on 1.7.x; SendAndRelayEvent reads `impl` at 0x200), with the
// caller's arguments on its own stack as the engine's own sinks pass theirs
// (adr065 Q4; H-R10). Under the platform's mark: callNative's Override
// passes the event at the gate (FridaHooks.cpp), and the pending blank
// stands down inside a platform call
void SendAndRelay(const EventTarget& target, const char* name,
                  RE::BSScript::IFunctionArguments* args)
{
  FixedString eventName(name);
  PlatformCall::Scope platformCall;
  target.skyrimVM->GetImpl()->SendEvent(target.handle, eventName, args);
  target.skyrimVM->RelayEvent(target.handle, &eventName, args, nullptr);
}

std::string SendImpl(RE::TESObjectREFR* refr, std::string_view name,
                     bool unseenOnly)
{
  const RE::FormID formId = refr ? refr->formID : 0;
  nlohmann::json answer = nlohmann::json::object();
  answer["formId"] = formId;
  answer["event"] = std::string(name);
  answer["sent"] = false;
  answer["seen"] = false;
  answer["scripts"] = 0;
  answer["reason"] = "";
  auto refuse = [&](const std::string& reason) {
    answer["reason"] = reason;
    if (LogThis(g_sendLines)) {
      logger::info("[verb:scripts] SendScriptEvent {} to {:x} not sent: {}",
                   std::string(name), formId, reason);
    }
    return DumpText(answer);
  };
  if (!refr) {
    return refuse("no reference");
  }
  const Lifecycle* event = LifecycleOf(name);
  if (!event) {
    return refuse("not OnCellAttach, OnLoad or OnCellLoad (the events this "
                  "native raises take no argument)");
  }
  EventTarget target;
  std::string why;
  const bool reachable = TargetOf(refr, target, why);
  answer["scripts"] = target.scripts;
  if (!reachable) {
    return refuse(why);
  }
  if (unseenOnly && ScriptState::Seen::Has(target.handle, event->name)) {
    answer["seen"] = true;
    return refuse("the engine served it to this handle since the last load");
  }
  // a ZeroFunctionArguments, as the engine's own lifecycle sinks pass one
  // (adr065 Q4)
  RE::BSScript::ZeroFunctionArguments args;
  SendAndRelay(target, event->name, &args);
  ScriptState::Seen::OnEvent(target.handle, event->name, true);
  answer["sent"] = true;
  answer["event"] = event->name;
  if (LogThis(g_sendLines)) {
    logger::info("[verb:scripts] SendScriptEvent {} to {:x}: sent to {} "
                 "script(s) and relayed",
                 event->name, formId, target.scripts);
  }
  return DumpText(answer);
}

// thuum ADR-068 D-C2 A (ScriptState.h): a relayed trigger entry
std::string SendTriggerImpl(RE::TESObjectREFR* refr, std::string_view name,
                            RE::TESObjectREFR* actionRef)
{
  const RE::FormID formId = refr ? refr->formID : 0;
  const RE::FormID actionRefId = actionRef ? actionRef->formID : 0;
  nlohmann::json answer = nlohmann::json::object();
  answer["formId"] = formId;
  answer["event"] = std::string(name);
  answer["sent"] = false;
  answer["scripts"] = 0;
  answer["reason"] = "";
  auto refuse = [&](const std::string& reason) {
    answer["reason"] = reason;
    if (LogThis(g_triggerSendLines)) {
      logger::info("[verb:traps] SendTriggerEvent {} to {:x} by {:x} not "
                   "sent: {}",
                   std::string(name), formId, actionRefId, reason);
    }
    return DumpText(answer);
  };
  if (!refr) {
    return refuse("no reference");
  }
  const char* event = TriggerEventOf(name);
  if (!event) {
    return refuse("not OnTriggerEnter or OnTriggerLeave (a lifecycle event "
                  "goes through SendScriptEvent)");
  }
  if (!actionRef) {
    return refuse("no actionRef (None)");
  }
  // an actor is no trigger volume, and its scripts run where it is simulated
  // (ADR-062), never by a relay
  if (refr->GetFormType() == RE::FormType::ActorCharacter) {
    return refuse("an actor trigger");
  }
  EventTarget target;
  std::string why;
  const bool reachable = TargetOf(refr, target, why);
  answer["scripts"] = target.scripts;
  if (!reachable) {
    return refuse(why);
  }
  // CommonLibSSE-NG's one-object argument, BSScript::FunctionArguments<void,
  // TESObjectREFR*> (include/RE/F/FunctionArguments.h:27-73) through its
  // alias RE::FunctionArguments (:77): its pack resizes the array to one and
  // packs the pointer through Variable::Pack, PackHandle for a form (H-W3),
  // as the engine's trigger sinks' own argument packs the actionRef. On this
  // stack: SendEvent and RelayEvent consume it before they return (H-R10),
  // as the sinks' own does on theirs. The constructor takes its arguments as
  // rvalues, hence the copy moved in
  RE::TESObjectREFR* akActionRef = actionRef;
  RE::FunctionArguments<RE::TESObjectREFR*> args{ std::move(akActionRef) };
  SendAndRelay(target, event, &args);
  answer["sent"] = true;
  answer["event"] = event;
  if (LogThis(g_triggerSendLines)) {
    logger::info("[verb:traps] SendTriggerEvent {} to {:x} by {:x}: sent to "
                 "{} script(s) and relayed",
                 event, formId, actionRefId, target.scripts);
  }
  return DumpText(answer);
}

// an answer for a native whose body threw, in its own shape (an apply's with
// the names blanked, read as a refusal reads them)
std::string ExceptionAnswer(std::uint32_t formId, const char* field,
                            const char* what,
                            const std::vector<std::string>* blanked = nullptr)
{
  try {
    nlohmann::json answer = nlohmann::json::object();
    answer["formId"] = formId;
    answer[field] = false;
    answer["retry"] = false;
    answer["reason"] = std::string("exception: ") + (what ? what : "");
    if (blanked) {
      answer["blanked"] = *blanked;
    }
    return DumpText(answer);
  } catch (...) {
    return fmt::format("{{\"{}\":false,\"retry\":false,\"reason\":"
                       "\"exception\"}}",
                       field);
  }
}
}

// The public natives: the bodies above under a catch. The dispatch between
// callNative and TESModPlatform::Add has none (ChangeForm.cpp's note: a
// throw out of a native leaves the JS pool thread waiting forever, a frozen
// seat), and the locks' guards release on the unwind. The realistic throws
// are nlohmann's (a type_error on a member read the checks missed) and
// bad_alloc
std::string ScriptState::Serialize(RE::TESObjectREFR* refr)
{
  try {
    return SerializeImpl(refr);
  } catch (const std::exception& e) {
    logger::error("[verb:scripts] SerializeScriptState {:x} threw: {}",
                  refr ? refr->formID : 0u, e.what());
  } catch (...) {
    logger::error("[verb:scripts] SerializeScriptState {:x} threw",
                  refr ? refr->formID : 0u);
  }
  return std::string();
}

// a throw is a refusal: the pending mark stays for the client's give-up, the
// names blanked read into the answer
std::string ScriptState::Apply(RE::TESObjectREFR* refr,
                               std::string_view scriptsJson)
{
  const std::uint32_t formId = refr ? refr->formID : 0u;
  std::string what;
  try {
    return ApplyImpl(refr, scriptsJson);
  } catch (const std::exception& e) {
    what = e.what();
  } catch (...) {
    what = "a non-standard exception";
  }
  std::vector<std::string> blanked;
  try {
    logger::error("[verb:scripts] ApplyScriptState {:x} threw: {}", formId,
                  what);
    blanked = Blanked(formId, false);
  } catch (...) {
  }
  return ExceptionAnswer(formId, "applied", what.c_str(), &blanked);
}

std::string ScriptState::Send(RE::TESObjectREFR* refr, std::string_view name,
                              bool unseenOnly)
{
  const std::uint32_t formId = refr ? refr->formID : 0u;
  std::string what;
  try {
    return SendImpl(refr, name, unseenOnly);
  } catch (const std::exception& e) {
    what = e.what();
  } catch (...) {
    what = "a non-standard exception";
  }
  try {
    logger::error("[verb:scripts] SendScriptEvent {:x} threw: {}", formId,
                  what);
  } catch (...) {
  }
  return ExceptionAnswer(formId, "sent", what.c_str());
}

std::string ScriptState::SendTrigger(RE::TESObjectREFR* refr,
                                     std::string_view name,
                                     RE::TESObjectREFR* actionRef)
{
  const std::uint32_t formId = refr ? refr->formID : 0u;
  std::string what;
  try {
    return SendTriggerImpl(refr, name, actionRef);
  } catch (const std::exception& e) {
    what = e.what();
  } catch (...) {
    what = "a non-standard exception";
  }
  try {
    logger::error("[verb:traps] SendTriggerEvent {:x} threw: {}", formId,
                  what);
  } catch (...) {
  }
  return ExceptionAnswer(formId, "sent", what.c_str());
}

bool ScriptState::Pending::Is(std::uint32_t formId)
{
  std::lock_guard l(g_pending.m);
  return g_pending.marks.count(formId) != 0;
}

// the mark and its names taken together under the set's lock (Blanked),
// the names answered as the JSON array releaseScriptRecordPending returns
std::string ScriptState::Pending::Release(std::uint32_t formId)
{
  return DumpText(nlohmann::json(Blanked(formId, true)));
}

void ScriptState::Pending::Set(std::uint32_t formId, bool pending)
{
  std::lock_guard l(g_pending.m);
  if (pending) {
    g_pending.marks.try_emplace(formId);
  } else {
    g_pending.marks.erase(formId);
  }
  g_pending.size.store(g_pending.marks.size());
}

std::size_t ScriptState::Pending::Count()
{
  std::lock_guard l(g_pending.m);
  return g_pending.marks.size();
}

bool ScriptState::Pending::Any()
{
  return g_pending.size.load(std::memory_order_relaxed) != 0;
}

bool ScriptState::Pending::IsBlankedName(const char* eventName)
{
  return eventName &&
    (_stricmp(eventName, "OnLoad") == 0 ||
     _stricmp(eventName, "OnCellLoad") == 0);
}

void ScriptState::Pending::NoteBlanked(std::uint32_t formId,
                                       const char* eventName)
{
  if (!eventName) {
    return;
  }
  const Lifecycle* event = LifecycleOf(eventName);
  const std::string name = event ? event->name : eventName;
  std::lock_guard l(g_pending.m);
  const auto it = g_pending.marks.find(formId);
  if (it == g_pending.marks.end()) {
    return;
  }
  auto& names = it->second;
  if (std::find(names.begin(), names.end(), name) == names.end()) {
    names.push_back(name);
  }
}

void ScriptState::Seen::OnEvent(RE::VMHandle handle, const char* eventName,
                                bool served)
{
  if (!eventName || !*eventName) {
    return;
  }
  const bool detach = _stricmp(eventName, "OnCellDetach") == 0;
  const Lifecycle* event = detach ? nullptr : LifecycleOf(eventName);
  if (!detach && (!event || !served)) {
    return;
  }
  bool cut = false;
  {
    std::lock_guard l(g_seen.m);
    if (detach) {
      g_seen.bits.erase(handle);
      return;
    }
    if (g_seen.bits.size() >= kSeenCap && g_seen.bits.count(handle) == 0) {
      g_seen.bits.clear();
      cut = true;
    }
    auto& bits = g_seen.bits[handle];
    bits = static_cast<std::uint8_t>(bits | event->bit);
  }
  if (cut) {
    logger::warn("[verb:scripts] the seen set passed {} handles and was "
                 "cleared: an unseen-only send may raise an event the "
                 "engine already served",
                 kSeenCap);
  }
}

bool ScriptState::Seen::Has(RE::VMHandle handle, std::string_view eventName)
{
  const Lifecycle* event = LifecycleOf(eventName);
  if (!event) {
    return false;
  }
  std::lock_guard l(g_seen.m);
  const auto it = g_seen.bits.find(handle);
  return it != g_seen.bits.end() && (it->second & event->bit) != 0;
}

// called from the engine's TESLoadGameEvent sink, which no exception may
// leave
void ScriptState::Seen::Clear()
{
  try {
    std::size_t n = 0;
    {
      std::lock_guard l(g_seen.m);
      n = g_seen.bits.size();
      g_seen.bits.clear();
    }
    logger::info("[verb:scripts] seen set cleared at a game load ({} "
                 "handle(s))",
                 n);
  } catch (...) {
  }
}
