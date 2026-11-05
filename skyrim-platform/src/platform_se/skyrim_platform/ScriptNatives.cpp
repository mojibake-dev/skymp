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
#include <RE/F/FormTypes.h>
#include <RE/I/IFunction.h>
#include <RE/I/IObjectHandlePolicy.h>
#include <RE/I/IStackCallbackFunctor.h>
#include <RE/N/NativeFunctionBase.h>
#include <RE/O/Object.h>
#include <RE/O/ObjectTypeInfo.h>
#include <RE/S/Stack.h>
#include <RE/S/StackFrame.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TypeInfo.h>
#include <RE/V/Variable.h>
#include <RE/V/VirtualMachine.h>
#include <array>
#include <atomic>
#include <chrono>
#include <fmt/format.h>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

// the platform's current direct-call stack (main.cpp's OnUpdate): a native
// entered on it is the platform's own call, not a script frame's
extern CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

namespace {
// the dispatch hook's raises for the platform's latent calls, logged for
// the first three and every 1000th
std::atomic<uint64_t> g_callRaises{ 0 };
// the census's window: the counts below flush to JS as one scriptCensus
// event every kFlushMs from the update's Tick, so a quiet seat emits its
// last window too
constexpr uint64_t kFlushMs = 30'000;
std::atomic<uint64_t> g_lastFlushMs{ 0 };
// the selves an everyone row (a quest's, a global's or an alias's call)
// names in one window: at most kSelvesPerRow distinct ids per row
// ("Quest.Start on 30002859"), the rest under "on others", so a census line
// per self never grows past the window's bound (the review of 2026-11-03:
// unbounded selves were unbounded lines). 32, not 8: a rare row names every
// self (x-script-natives v6, run 20261103-073235: SetCurrentStageID ran 11
// times on 10 selves in the guest's load window, two of them under "on
// others", so a claim reading "on 10d3bb" could not rule the stage in or
// out); a probe that leans on a row's self lines also claims that row's "on
// others" line absent, so the bound biting reads as a red, never as a pass
constexpr size_t kSelvesPerRow = 32;

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

/**
 * thuum docs/DECISIONS.md ADR-068 (rung 2, the one seam; docs/research/
 * scripts-one-system-2026-11-05.md 6.2): the table the dispatch hook reads
 * and its judgment of one call
 */

// a row's kind and subject, NativeRows.inc's columns (thuum docs/NATIVES.md)
enum class NativeKind : std::uint8_t
{
  Action,
  State,
  Everyone,
  Local
};

enum class NativeSubject : std::uint8_t
{
  Self,
  Arg0,
  None
};

struct NativeRow
{
  const char* cls;
  const char* fn;
  NativeKind kind;
  NativeSubject subject;
  // the receiver's native for an action row ("same", "twin:<Class.Name>",
  // "none"): the server's table relays by it; carried here so the generated
  // file is one source, and said in the install line
  const char* relay;
};

constexpr NativeRow kRows[] = {
#include "NativeRows.inc"
};
constexpr std::size_t kRowCount = std::size(kRows);

// rung 2's two state rows whose owned call crosses in the batch, as kind
// "state": the client's death report reads a script's kill there (cause 3,
// skymp5-client's DeathReportService), the flip's never-dropped kill rows;
// every other state row is counted until rung 4's record trigger
constexpr std::pair<const char*, const char*> kBatchedStateRows[] = {
  { "Actor", "Kill" },
  { "Actor", "KillSilent" }
};

// at most 11 arguments (CommonLibSSE-NG src/RE/N/NativeFunctionBase.cpp:15,
// "native supports max 11")
constexpr std::uint32_t kMaxArgs = 11;

// what InstallHooks reads once from a row's bound IFunction, written before
// the dispatch hook attaches and never after, so the hook reads it with no
// lock
struct RowInfo
{
  const RE::BSScript::IFunction* fn = nullptr;
  // IFunction::GetParamCount and each parameter's declared type
  // (IFunction::GetParam; CommonLibSSE-NG include/RE/I/IFunction.h:51-52)
  std::uint32_t params = 0;
  std::array<RE::BSScript::TypeInfo, kMaxArgs> types{};
  // NativeFunctionBase::GetIsLatent (include/RE/N/NativeFunctionBase.h:58)
  bool latent = false;
  // no array parameter and at most kMaxArgs of them: the wire's SnippetArg
  // carries a bool, a number, a string or an object, never an array
  bool relayable = false;
  bool batchedState = false;
  // the declared return's kind (TypeInfo::GetUnmangledRawType)
  RE::BSScript::TypeInfo::RawType result =
    RE::BSScript::TypeInfo::RawType::kNone;
};

std::array<RowInfo, kRowCount> g_rows{};
// a row's bound IFunction, the `this` of its every dispatch, to its index:
// written by InstallHooks before the dispatch hook attaches and never after
std::unordered_map<const RE::BSScript::IFunction*, std::size_t> g_rowOf;

// the census's kinds, a fixed index per row (6.11 item 11: no lock, no
// string and no allocation on the path; the names are built at the flush,
// the flip's): " (platform)" the platform's own call (a direct call under
// PlatformCall::Scope or on the platform's borrowed stack, or a latent one
// whose Stack's callback is a live VmCallback); " (not hosted)" a self this
// seat does not own; " (local)" a row that never crosses (Game.GetPlayer,
// an animation row on an actor); " (unrelayable)" an owned action row with
// an array parameter; " (unresolved)" a subject that names no reference;
// " (frame mismatch)" a dispatch whose top frame is not the native's own, or
// whose arguments' page cannot be found (H-S1 failing); no suffix, an owned
// self's state row, run here and counted (rung 4 adds its record trigger).
// An everyone row's calls are counted by self instead (g_rowSelves, the
// flip's "Class.Name on <id>"); a batched call is the batch's, never counted
enum CountKind : std::size_t
{
  kCountOwned,
  kCountPlatform,
  kCountNotHosted,
  kCountLocal,
  kCountUnrelayable,
  kCountUnresolved,
  kCountFrameMismatch,
  kCountKinds
};

constexpr const char* kCountSuffix[kCountKinds] = { "",
                                                    " (platform)",
                                                    " (not hosted)",
                                                    " (local)",
                                                    " (unrelayable)",
                                                    " (unresolved)",
                                                    " (frame mismatch)" };

std::array<std::array<std::atomic<std::uint32_t>, kCountKinds>, kRowCount>
  g_counts{};

// an everyone row's selves in one window, the flip's bound (kSelvesPerRow)
// on fixed slots: a slot's key is kSlotUsed | the self's form id, 0 while
// free; a self past the bound is counted under "on others"
constexpr std::uint64_t kSlotUsed = std::uint64_t{ 1 } << 32;

struct SelfSlots
{
  std::array<std::atomic<std::uint64_t>, kSelvesPerRow> keys{};
  std::array<std::atomic<std::uint32_t>, kSelvesPerRow> counts{};
  std::atomic<std::uint32_t> others{ 0 };
};

std::array<SelfSlots, kRowCount> g_rowSelves{};

// plain-form handles whose low 32 bits disagreed with the form the VM's
// policy resolved (the encoding HYPOTHESIS below failing)
std::atomic<std::uint32_t> g_handleMisses{ 0 };

void Count(std::size_t row, CountKind kind)
{
  g_counts[row][kind].fetch_add(1, std::memory_order_relaxed);
}

void CountSelf(std::size_t row, std::uint32_t selfId)
{
  auto& slots = g_rowSelves[row];
  const std::uint64_t key = kSlotUsed | selfId;
  for (std::size_t i = 0; i < kSelvesPerRow; ++i) {
    std::uint64_t seen = slots.keys[i].load(std::memory_order_relaxed);
    if (seen == 0 &&
        slots.keys[i].compare_exchange_strong(seen, key,
                                              std::memory_order_relaxed)) {
      // claimed for this self (a failed claim reads back who took it)
      seen = key;
    }
    if (seen == key) {
      slots.counts[i].fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  slots.others.fetch_add(1, std::memory_order_relaxed);
}

// the HYPOTHESIS encoding of a VM handle (docs/research/
// adr065-papyrus-state-2026-11-03.md Q1, four decompiles: 53508, 53512,
// 53522, 53523): bits 48 to 63 a kind, 32 to 47 a middle word, 0 to 31 a
// form id; kind 0 with mid 0xFFFF a plain form, kind 0 with a smaller mid an
// alias whose low 32 bits are its quest's form id
constexpr bool IsPlainFormHandle(RE::VMHandle handle)
{
  return (handle >> 48) == 0 && ((handle >> 32) & 0xFFFF) == 0xFFFF;
}

constexpr std::uint32_t FormIdOfHandle(RE::VMHandle handle)
{
  return static_cast<std::uint32_t>(handle & 0xFFFFFFFF);
}

// the script object a Variable holds (null for None or a non-object): the
// value read in place (the fork's port patch 02 makes Variable's members
// public), no reference taken, since the frame's Variable keeps the object
// alive for the call (the research's finding 16: the VM's own smart pointer
// holds the frame)
const RE::BSScript::Object* ObjectOf(const RE::BSScript::Variable& v)
{
  if (!v.IsObject()) {
    return nullptr;
  }
  return v.value.obj.get();
}

// the reference a script object stands for, through the VM's own handle
// policy as OnSendEventEnter resolves an event's self (FridaHooks.cpp: the
// actor, then the reference; CommonLibSSE-NG include/RE/I/
// IObjectHandlePolicy.h:34), the handle from the engine's own accessor
// (Object::GetHandle, Address Library 104247); a plain form's handle is held
// against the encoding above and the form table (TESForm::LookupByID), a
// disagreement counted and its first three logged. Null for no object, no
// policy, or a handle that names no reference (an alias's, an effect's)
RE::TESObjectREFR* RefOfObject(RE::BSScript::IObjectHandlePolicy* policy,
                               const RE::BSScript::Object* object)
{
  if (!policy || !object) {
    return nullptr;
  }
  const RE::VMHandle handle = object->GetHandle();
  RE::TESForm* form =
    policy->GetObjectForHandle(RE::FormType::ActorCharacter, handle);
  if (!form) {
    form = policy->GetObjectForHandle(RE::FormType::Reference, handle);
  }
  if (!form) {
    return nullptr;
  }
  if (IsPlainFormHandle(handle) &&
      (FormIdOfHandle(handle) != form->formID ||
       RE::TESForm::LookupByID(form->formID) != form)) {
    const auto n = g_handleMisses.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3) {
      logger::warn("[verb:scripts] handle {:x}: the VM's policy resolved "
                   "form {:x}, the handle's low 32 bits read {:x} (the "
                   "encoding HYPOTHESIS of adr065 Q1)",
                   handle, form->formID, FormIdOfHandle(handle));
    }
  }
  return form->AsReference();
}

// an everyone row's self for the census: the low 32 bits of its handle (a
// quest's or a global's form id, an alias's quest's: kind 0 in either case,
// the encoding above), kept when the form table knows the id, else 0
std::uint32_t EveryoneSelfId(const RE::BSScript::Variable& self)
{
  const RE::BSScript::Object* object = ObjectOf(self);
  if (!object) {
    return 0;
  }
  const RE::VMHandle handle = object->GetHandle();
  const std::uint32_t id = FormIdOfHandle(handle);
  if ((handle >> 48) != 0 || id == 0 || !RE::TESForm::LookupByID(id)) {
    return 0;
  }
  return id;
}

// ADR-068 decision 6's owner of a reference's scripts, judged on this seat:
// the player's own actor (0x14) is this seat's; a live actor is its
// simulator's (hosted here and no puppet, Hosted.h and Puppets.h; a figure,
// 0xff000000 and up, is never in the hosted set); a world object is its
// cell host's (Frida::IsCellHosted, the set SetCellHosted keeps)
bool OwnedHere(const RE::TESObjectREFR* ref, bool isActor)
{
  const std::uint32_t id = ref->formID;
  if (id == 0x14) {
    return true;
  }
  if (isActor) {
    return Hosted::Is(id) && !Puppets::Is(id);
  }
  const RE::TESObjectCELL* cell = ref->GetParentCell();
  return cell && Frida::IsCellHosted(cell->formID);
}

// one argument in the batch, the shared shape's Arg: a boolean, a number, a
// string, { formId } or null (unread)
struct CallArg
{
  enum class Kind : std::uint8_t
  {
    Null,
    Bool,
    Number,
    String,
    Form
  };
  Kind kind = Kind::Null;
  bool b = false;
  double number = 0;
  std::string str;
  std::uint32_t formId = 0;
};

// one argument decoded by the Variable's own type, the way
// MarshallAndDispatch unpacks it (Variable::Unpack, NativeFunction.h:42):
// a bool, an int and a float as themselves, a string copied, an object as
// its plain form's id (None as formId 0, and a None in an object parameter
// too); anything else (an array, an object that is no plain form) is null
CallArg ReadArg(const RE::BSScript::Variable& v,
                const RE::BSScript::TypeInfo& declared)
{
  CallArg arg;
  if (v.IsBool()) {
    arg.kind = CallArg::Kind::Bool;
    arg.b = v.GetBool();
  } else if (v.IsInt()) {
    arg.kind = CallArg::Kind::Number;
    arg.number = static_cast<double>(v.GetSInt());
  } else if (v.IsFloat()) {
    arg.kind = CallArg::Kind::Number;
    arg.number = static_cast<double>(v.GetFloat());
  } else if (v.IsString()) {
    arg.kind = CallArg::Kind::String;
    const std::string_view text = v.GetString();
    arg.str.assign(text.data(), text.size());
  } else if (v.IsObject() || (v.IsNoneObject() && declared.IsObject())) {
    const RE::BSScript::Object* object = ObjectOf(v);
    if (!object) {
      arg.kind = CallArg::Kind::Form;
    } else {
      const RE::VMHandle handle = object->GetHandle();
      const std::uint32_t id = FormIdOfHandle(handle);
      if (IsPlainFormHandle(handle) && id != 0 &&
          RE::TESForm::LookupByID(id)) {
        arg.kind = CallArg::Kind::Form;
        arg.formId = id;
      }
    }
  }
  return arg;
}

// the calling script and its handler: the frame below the native's own
// (the research's finding 9; H-S3, HYPOTHESIS until x-native-frame v1's
// caller rows read defaultPuzzlePillarScript), its owningObjectType's name
// (ObjectTypeInfo::GetName) and its owningFunction's (IFunction::GetName);
// both empty when the native's frame is its stack's first (a stack the VM
// or the platform started on the native itself)
void NameCaller(const RE::BSScript::StackFrame* below, std::string& caller,
                std::string& handler)
{
  if (!below) {
    return;
  }
  if (const RE::BSScript::ObjectTypeInfo* type =
        below->owningObjectType.get()) {
    caller = type->GetName();
  }
  if (const RE::BSScript::IFunction* function = below->owningFunction.get()) {
    handler = function->GetName().c_str();
  }
}

// one captured call of an owned self, the shared shape's Call
struct QueuedCall
{
  std::size_t row = 0;
  std::uint32_t self = 0;
  std::uint32_t argCount = 0;
  std::array<CallArg, kMaxArgs> args{};
  std::string caller;
  std::string handler;
  bool latent = false;
  std::uint32_t stackId = 0;
};

// this frame's batch: appended on the dispatching thread for a batched row
// alone (an owned self's action row, or Kill), so the lock is taken rarely
// and never for an untabled call; the flag is up while a flush is
// scheduled
std::mutex g_batchMutex;
std::vector<QueuedCall> g_batch;
std::atomic<bool> g_batchScheduled{ false };

const char* KindName(NativeKind kind)
{
  switch (kind) {
    case NativeKind::Action:
      return "action";
    case NativeKind::State:
      return "state";
    case NativeKind::Everyone:
      return "everyone";
    case NativeKind::Local:
      break;
  }
  return "local";
}

Napi::Value ArgToJs(Napi::Env env, const CallArg& arg)
{
  switch (arg.kind) {
    case CallArg::Kind::Bool:
      return Napi::Boolean::New(env, arg.b);
    case CallArg::Kind::Number:
      return Napi::Number::New(env, arg.number);
    case CallArg::Kind::String:
      return Napi::String::New(env, arg.str);
    case CallArg::Kind::Form: {
      auto form = Napi::Object::New(env);
      AddObjProperty(&form, "formId", arg.formId);
      return form;
    }
    case CallArg::Kind::Null:
      break;
  }
  return env.Null();
}

// one Call of the shared shape (thuum's rung 2 spec, the platform's JS
// event): { cls, fn, kind, self, args, caller, handler, latent, stackId },
// cls and fn the table's spelling
Napi::Object CallToJs(Napi::Env env, const QueuedCall& call)
{
  const NativeRow& native = kRows[call.row];
  auto obj = Napi::Object::New(env);
  AddObjProperty(&obj, "cls", native.cls);
  AddObjProperty(&obj, "fn", native.fn);
  AddObjProperty(&obj, "kind", KindName(native.kind));
  AddObjProperty(&obj, "self", call.self);
  auto args = Napi::Array::New(env, call.argCount);
  for (uint32_t i = 0; i < call.argCount; ++i) {
    args.Set(i, ArgToJs(env, call.args[i]));
  }
  obj.Set("args", args);
  AddObjProperty(&obj, "caller", call.caller);
  AddObjProperty(&obj, "handler", call.handler);
  AddObjProperty(&obj, "latent", call.latent);
  AddObjProperty(&obj, "stackId", call.stackId);
  return obj;
}

// the batch's crossing (6.11 item 11), on the JS thread from the tick task
// QueueCall scheduled: every call queued since the last flush as one event,
// scriptCalls { calls: Call[] }
void FlushBatch(Napi::Env env)
{
  std::vector<QueuedCall> calls;
  {
    std::lock_guard<std::mutex> lock(g_batchMutex);
    calls.swap(g_batch);
    // lowered under the lock: a call queued after the swap finds the flag
    // down and schedules the next flush, one queued before rides this one
    g_batchScheduled.store(false);
  }
  if (calls.empty()) {
    return;
  }
  auto array = Napi::Array::New(env, calls.size());
  for (uint32_t i = 0; i < static_cast<uint32_t>(calls.size()); ++i) {
    array.Set(i, CallToJs(env, calls[i]));
  }
  auto event = Napi::Object::New(env);
  event.Set("calls", array);
  EventsApi::SendEvent("scriptCalls", { event });
}

void QueueCall(QueuedCall&& call)
{
  {
    std::lock_guard<std::mutex> lock(g_batchMutex);
    g_batch.push_back(std::move(call));
  }
  // one crossing a frame: the first call after a flush schedules the next
  // on SkyrimPlatform's tick (its tasks run once a frame on the JS
  // thread), the rest of the frame's calls ride it
  if (!g_batchScheduled.exchange(true)) {
    SkyrimPlatform::GetSingleton()->AddTickTask(
      [](Napi::Env env) { FlushBatch(env); });
  }
}

// StackFrame::GetPageForFrame's answer for a frame in no page of its stack
// (CommonLibSSE-NG src/RE/S/Stack.cpp:16-26)
constexpr std::uint32_t kNoPage = static_cast<std::uint32_t>(-1);

// what the dispatch hook raises for one call's extent (PlatformCall.h's
// bracket)
struct Marks
{
  // the platform's depth: a latent call the platform dispatched (VmCall::Run;
  // the dispatched Stack's callback is a live VmCallback)
  bool platform = false;
  // the script action's: an owned action row's call (6.7)
  bool action = false;
};

// the seam's judgment of one dispatch, 6.2's order (rule 9: the cheapest
// test first). The body runs in every case (the hook is an attach)
Marks OnCall(const RE::BSScript::IFunction* fn, RE::BSScript::Stack* stack)
{
  Marks marks;
  // (a) not a tabled native: nothing more, the only cost of every other
  // native in the game (one hash lookup)
  const auto it = g_rowOf.find(fn);
  if (it == g_rowOf.end()) {
    return marks;
  }
  const std::size_t row = it->second;
  const NativeRow& native = kRows[row];
  const RowInfo& info = g_rows[row];
  if (!stack) {
    Count(row, kCountUnresolved);
    return marks;
  }
  // (b) the platform's own: a direct call (callNative's worker inside
  // OnUpdate, a non-latent SpSnippet: PlatformCall::Scope, or any call on
  // the platform's borrowed stack, main.cpp's OnUpdate) or a latent one (the
  // Stack's callback is the VmCallback the platform handed the dispatch: the
  // flip's marking, H-L1 to H-L3, raised for the extent as before): the
  // client applying the server's word is no script's consequence
  const bool direct = PlatformCall::Active() ||
    (g_nativeCallRequirements.stackId != 0 &&
     stack->stackID == g_nativeCallRequirements.stackId);
  const bool live =
    !direct && PlatformCall::IsLiveCallback(stack->callback.get());
  if (direct || live) {
    marks.platform = live;
    if (live) {
      const auto n = ++g_callRaises;
      if (n <= 3 || n % 1000 == 0) {
        logger::info("[verb:scripts] native {}.{} dispatched by the "
                     "platform on stack {} (latent call {}): marked on the "
                     "VM's thread for its extent",
                     native.cls, native.fn, stack->stackID, n);
      }
    }
    Count(row, kCountPlatform);
    return marks;
  }
  // (c) the frame: at the dispatch the top frame is the native's own (H-S1,
  // HYPOTHESIS until x-native-frame v1 reads it: the platform's direct path
  // builds exactly that frame before its own Call, CallNative.cpp, and
  // MarshallAndDispatch unpacks the self and the arguments from it,
  // NativeFunction.h:81-92), so a frame another function owns is counted,
  // never read
  const RE::BSScript::StackFrame* top = stack->top;
  if (!top || top->owningFunction.get() != fn) {
    Count(row, kCountFrameMismatch);
    return marks;
  }
  // a local row never crosses (Game.GetPlayer: rung 5 adds its scope)
  if (native.kind == NativeKind::Local) {
    Count(row, kCountLocal);
    return marks;
  }
  // an everyone row has no owner (D-F1 C): every seat runs it, its self
  // counted
  if (native.kind == NativeKind::Everyone) {
    CountSelf(row, EveryoneSelfId(top->self));
    return marks;
  }
  // the subject: the frame's self for a method row, the first argument for
  // a static row (Debug.SendAnimationEvent's reference). The arguments are
  // read through the engine's own accessor, Stack::GetStackFrameVariable
  // (Address Library 104484, CommonLibSSE-NG src/RE/S/Stack.cpp:28-33) with
  // the frame's page as the hint, as MarshallAndDispatch reads them
  // (NativeFunction.h:91-92), never by pointer arithmetic over the frame (a
  // stack is pages, Stack.h:54-62; H-S3)
  std::uint32_t page = kNoPage;
  const RE::BSScript::Variable* subject = nullptr;
  if (native.subject == NativeSubject::Self) {
    subject = &top->self;
  } else if (native.subject == NativeSubject::Arg0 && info.params > 0) {
    page = top->GetPageForFrame();
    if (page == kNoPage) {
      Count(row, kCountFrameMismatch);
      return marks;
    }
    subject = &top->GetStackFrameVariable(0, page);
  }
  auto* vm = VM::GetSingleton();
  auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
  RE::TESObjectREFR* ref =
    subject ? RefOfObject(policy, ObjectOf(*subject)) : nullptr;
  if (!ref) {
    Count(row, kCountUnresolved);
    return marks;
  }
  const bool isActor = ref->GetFormType() == RE::FormType::ActorCharacter;
  // an animation row on an actor is the stream's (6.7: the boundary emits
  // nothing for actor animation rows; the table's Debug.SendAnimationEvent
  // on an actor is local): every rung 2 action row is an animation row
  if (native.kind == NativeKind::Action && isActor) {
    Count(row, kCountLocal);
    return marks;
  }
  // (d) the owner. Not owned: counted, and the body runs (an attach cannot
  // skip it)
  if (!OwnedHere(ref, isActor)) {
    Count(row, kCountNotHosted);
    return marks;
  }
  // (e) owned. A state row is the record's (ADR-065: the change form carries
  // it), counted; Kill and KillSilent cross in the batch too
  if (native.kind == NativeKind::State && !info.batchedState) {
    Count(row, kCountOwned);
    return marks;
  }
  // an owned action row's extent is the script action's (6.7), whether it
  // crosses or not: the boundary is its one observe point
  marks.action = native.kind == NativeKind::Action;
  if (!info.relayable) {
    Count(row, kCountUnrelayable);
    return marks;
  }
  QueuedCall call;
  call.row = row;
  call.self = ref->formID;
  call.latent = info.latent;
  call.stackId = stack->stackID;
  call.argCount = info.params;
  if (info.params > 0) {
    if (page == kNoPage) {
      page = top->GetPageForFrame();
    }
    if (page == kNoPage) {
      Count(row, kCountFrameMismatch);
      return marks;
    }
    for (std::uint32_t i = 0; i < info.params; ++i) {
      call.args[i] =
        ReadArg(top->GetStackFrameVariable(i, page), info.types[i]);
    }
  }
  NameCaller(top->previousFrame, call.caller, call.handler);
  QueueCall(std::move(call));
  return marks;
}

// a row's IFunction read once at kDataLoaded: its parameters' count and
// declared types, its latency, its return's kind
void ReadRow(std::size_t row, const RE::BSScript::IFunction* function)
{
  RowInfo& info = g_rows[row];
  info.fn = function;
  info.params = function->GetParamCount();
  bool arrays = false;
  for (std::uint32_t i = 0; i < info.params && i < kMaxArgs; ++i) {
    RE::BSFixedString unusedName;
    RE::BSScript::TypeInfo type;
    function->GetParam(i, unusedName, type);
    info.types[i] = type;
    arrays = arrays || type.IsArray() || type.IsNoneArray();
  }
  info.relayable = info.params <= kMaxArgs && !arrays;
  // a game native is a NativeFunctionBase (the fork reads the same flag at
  // +0x42 by hand, GetNativeFunctionAddr.cpp:18; here through the header's
  // accessor)
  info.latent = function->GetIsNative() &&
    static_cast<const RE::BSScript::NF_util::NativeFunctionBase*>(function)
      ->GetIsLatent();
  info.result = function->GetReturnType().GetUnmangledRawType();
  for (const auto& [cls, name] : kBatchedStateRows) {
    if (_stricmp(kRows[row].cls, cls) == 0 &&
        _stricmp(kRows[row].fn, name) == 0) {
      info.batchedState = true;
    }
  }
}

// the census, every kFlushMs from the update's Tick: each row's counts read
// and zeroed, named the flip's way ("Class.Name (platform)", "Class.Name
// (not hosted)", the bare "Class.Name" for an owned self's state row,
// "Class.Name on <hex id>" and "Class.Name on others" for an everyone row's
// selves) and raised as one event, scriptCensus { rows: [{ row, count }] };
// the names are built here, never on a call's path. A call racing the flush
// is counted in this window or the next (a self slot's count can pass to
// the slot's next self at the window's edge: the census's precision)
void FlushCensus(uint64_t now)
{
  const uint64_t last = g_lastFlushMs.load();
  if (last == 0) {
    g_lastFlushMs.store(now);
    return;
  }
  if (now - last < kFlushMs) {
    return;
  }
  g_lastFlushMs.store(now);
  std::vector<std::pair<std::string, uint32_t>> rows;
  uint64_t total = 0;
  for (size_t row = 0; row < kRowCount; ++row) {
    const std::string name = std::string(kRows[row].cls) + "." + kRows[row].fn;
    for (size_t kind = 0; kind < kCountKinds; ++kind) {
      const uint32_t n = g_counts[row][kind].exchange(0);
      if (n != 0) {
        rows.emplace_back(name + kCountSuffix[kind], n);
        total += n;
      }
    }
    // one row per self, a self two slots claimed at a window's edge
    // included
    std::map<uint32_t, uint32_t> selves;
    auto& slots = g_rowSelves[row];
    for (size_t i = 0; i < kSelvesPerRow; ++i) {
      const uint64_t key = slots.keys[i].exchange(0);
      const uint32_t n = slots.counts[i].exchange(0);
      if (key != 0 && n != 0) {
        selves[static_cast<uint32_t>(key)] += n;
      }
    }
    for (const auto& [selfId, n] : selves) {
      rows.emplace_back(fmt::format("{} on {:x}", name, selfId), n);
      total += n;
    }
    const uint32_t others = slots.others.exchange(0);
    if (others != 0) {
      rows.emplace_back(name + " on others", others);
      total += others;
    }
  }
  const uint32_t misses = g_handleMisses.exchange(0);
  if (misses != 0) {
    logger::warn("[verb:scripts] {} plain-form handle(s) in {} s disagreed "
                 "with the VM's policy (the encoding HYPOTHESIS of adr065 Q1)",
                 misses, kFlushMs / 1000);
  }
  if (rows.empty()) {
    return;
  }
  logger::info("[verb:scripts] {} tallied native call(s) in {} s over {} "
               "row(s) flushed to the census",
               total, kFlushMs / 1000, rows.size());
  SkyrimPlatform::GetSingleton()->AddTickTask(
    [rows = std::move(rows)](Napi::Env env) {
      auto array = Napi::Array::New(env, rows.size());
      for (uint32_t i = 0; i < static_cast<uint32_t>(rows.size()); ++i) {
        auto entry = Napi::Object::New(env);
        AddObjProperty(&entry, "row", rows[i].first);
        AddObjProperty(&entry, "count", rows[i].second);
        array.Set(i, entry);
      }
      auto event = Napi::Object::New(env);
      event.Set("rows", array);
      EventsApi::SendEvent("scriptCensus", { event });
    });
}

// thuum docs/DECISIONS.md ADR-068 decision 2 (rung 2, the one seam;
// docs/research/scripts-one-system-2026-11-05.md 6.2): the VM's one native
// dispatch, BSScript::NF_util::NativeFunctionBase::Call (CommonLibSSE-NG
// include/RE/N/NativeFunctionBase.h:47, slot 0x0F; Address Library 104651,
// game/Offsets.h NativeFunctionCall, `just addr` 0x143dc00), where every
// tabled native's call is judged from its typed frame (OnCall above). A
// latent call the platform dispatches (VmCall::Run) runs its body later on a
// Papyrus thread under a new stack id, inside this Call with the dispatched
// Stack as the argument, and that Stack's callback is the VmCallback the
// platform handed the dispatch; when it is a live one
// (PlatformCall::IsLiveCallback) the call is the platform's and the
// platform's depth is raised here for its extent, as Scope raises it on the
// direct path. The cost on every native call in the game: Gum's bracket,
// the bracket's level (PlatformCall.h) and one hash lookup of `this`
// against the table; a call outside it does nothing more. This replaced
// the flip's 24 entry hooks and their register conventions (each row's
// layout a HYPOTHESIS, the float arguments unreadable).
// HYPOTHESIS H-S2: a native queued as a FunctionMessage (one not callable
// from tasklets, run by VirtualMachine::Update) still enters through this
// slot, so the seam sees it; the guest's GlobalVariable.SetValue census row
// against the script that sets the global settles it.
// HYPOTHESIS H-L1: the dispatched stack's callback is the functor VmCall::Run
// passed, unwrapped (the VM keeps the smart pointer it was given). H-L2: the
// native's body runs within Call's extent on the same thread (statically
// yes: Call marshals the frame and calls MarshallAndDispatch,
// NativeFunction.h:81). H-L3: Stack's stackID at +0x80 and callback at
// +0x90 (Stack.h:80 and :83) hold on 1.6.1170 (the header's static layout,
// unmeasured). The trace that settles them: Frida on 104651 (this, the
// Stack's +0x80 and +0x90, the thread) beside the 24 entries and
// DispatchMethodCall2 on a guest applying a relayed stage, expecting
// Dispatch(F) then Call(S, S.callback == F, S.id == N) then the entry with N
// on the same thread.
void OnNativeCallEnter(GumInvocationContext* ic)
{
  // the entry stack pointer, the stranded-level guard's key (PlatformCall.h);
  // the house reads Gum's registers this way (FridaHooks.cpp's rcx). The
  // stranded levels go first, so the platform's depth OnCall reads is live
  const auto sp = static_cast<std::uintptr_t>(ic->cpu_context->rsp);
  PlatformCall::PopStrandedCalls(sp);
  const auto* fn = static_cast<const RE::BSScript::IFunction*>(
    gum_invocation_context_get_nth_argument(ic, 0));
  // the second argument is a const BSTSmartPointer<Stack>&: a pointer to
  // the smart pointer
  const auto* stackRef =
    static_cast<const RE::BSTSmartPointer<RE::BSScript::Stack>*>(
      gum_invocation_context_get_nth_argument(ic, 1));
  const Marks marks = OnCall(fn, stackRef ? stackRef->get() : nullptr);
  PlatformCall::EnterCall(marks.platform, marks.action, sp);
}

void OnNativeCallLeave(GumInvocationContext*)
{
  PlatformCall::LeaveCall();
}
}

void ScriptNatives::Register()
{
  // the dispatch hook's callbacks, attached at kDataLoaded once the table is
  // bound (InstallHooks)
  Frida::HookHandler::GetSingleton()->Register(
    Frida::HookID::HOOK_NATIVE_CALL,
    std::make_shared<Frida::Hook>(OnNativeCallEnter, OnNativeCallLeave));
}

void ScriptNatives::Tick()
{
  FlushCensus(NowMs());
}

bool ScriptNatives::InScriptAction()
{
  return PlatformCall::InAction();
}

void ScriptNatives::InstallHooks()
{
  // the VM's bindings as Hooks.cpp recorded them (class, name, IFunction*,
  // module base, the callback's offset, the long-signature mark); the game's
  // own module alone (SKSE's natives are not scripts' consequences here)
  const auto gameBase = REL::Module::get().base();
  const auto bound = Hooks::GetBoundNatives();
  // the table (NativeRows.inc): each row's IFunction, the `this` of its
  // every dispatch, found by class and name without case (Papyrus's rule;
  // the platform's dump spells Game.GetPlayer "getplayer") and read once,
  // all of it before the dispatch hook attaches below
  size_t boundRows = 0;
  for (size_t row = 0; row < kRowCount; ++row) {
    const NativeRow& native = kRows[row];
    const RE::BSScript::IFunction* function = nullptr;
    for (const auto& [cls, name, func, moduleBase, offset, longSig] : bound) {
      if (moduleBase == gameBase && _stricmp(cls.c_str(), native.cls) == 0 &&
          _stricmp(name.c_str(), native.fn) == 0) {
        function = func;
        break;
      }
    }
    if (!function) {
      logger::warn("[verb:scripts] table row {}.{} is not among the VM's "
                   "bindings; never judged",
                   native.cls, native.fn);
      continue;
    }
    if (!g_rowOf.emplace(function, row).second) {
      logger::warn("[verb:scripts] table row {}.{} binds a function another "
                   "row holds; the first row judges it",
                   native.cls, native.fn);
      continue;
    }
    ReadRow(row, function);
    ++boundRows;
    const RowInfo& info = g_rows[row];
    logger::info("[verb:scripts] table row {}.{}: {} parameter(s), {}{}, "
                 "relay {}",
                 native.cls, native.fn, info.params,
                 info.latent ? "latent" : "not latent",
                 info.relayable ? "" : ", unrelayable (an array parameter)",
                 native.relay);
  }
  logger::info("[verb:scripts] {} of {} table rows bound (NativeRows.inc, "
               "thuum docs/NATIVES.md)",
               boundRows, kRowCount);
  // the dispatch hook (OnNativeCallEnter), attached once the table's map is
  // complete, so it never reads a map being written
  const auto callAddress = Offsets::Hooks::NativeFunctionCall.address();
  if (Frida::HookHandler::GetSingleton()->Attach(
        Frida::HookID::HOOK_NATIVE_CALL, callAddress)) {
    logger::info("[verb:scripts] native dispatch hook on "
                 "NativeFunctionBase::Call at +{:x} (Address Library 104651): "
                 "{} table row(s) judged from the frame, the platform's "
                 "latent calls marked on the VM's thread",
                 callAddress - gameBase, g_rowOf.size());
  } else {
    logger::warn("[verb:scripts] NativeFunctionBase::Call at +{:x}: Gum "
                 "refused the attach; no table row is judged and the "
                 "platform's latent calls count as a script's",
                 callAddress - gameBase);
  }
}
