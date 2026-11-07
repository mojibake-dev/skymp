#include "ScriptNatives.h"
#include "CallNativeApi.h"
#include "EventsApi.h"
#include "FileUtils.h"
#include "FridaHookHandler.h"
#include "FridaHooks.h"
#include "Hooks.h"
#include "Hosted.h"
#include "JsUtils.h"
#include "PlatformCall.h"
#include "Puppets.h"
#include "Settings.h"
#include "SkyrimPlatform.h"
#include "TrapFamily.h"
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
#include <filesystem>
#include <fmt/format.h>
#include <intrin.h>
#include <iterator>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
// the selves an everyone row (a quest's or a global's call; an alias row's
// is batched since rung 4) names in one window: at most kSelvesPerRow
// distinct ids per row
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

// the state rows whose owned call crosses in the batch, as kind "state",
// with its arguments, its caller and the caller's self (QueueFromFrame):
// rung 2's two, whose kill the client's death report reads there (cause 3,
// skymp5-client's DeathReportService), the flip's never-dropped kill rows;
// ProcessTrapHit, a trap's hit on an actor this seat simulates (its self the
// hit actor, ADR-068 D-S3 point 2), which the client reports as that actor's
// death evidence (thuum docs/verbs/npc-fate.md, playtest 48: the blades'
// kill of draugr 800a6 came after their trigger's notes lapsed and was
// refused); and since rung 4 (the state rows, thuum ADR-068 amendment B;
// docs/research/rung4-state-rows-2026-11-07.md 1(a)) the record rows, an
// owner's change to a reference's engine state, which skymp5-client sends
// as ScriptCall 75 for the server's record channel: RefState's five (a
// reference enabled, disabled or deleted, the state DestructionStage 78
// serves) and the standing's six (an NPC's faction ranks, crime faction and
// relationship ranks; AddToFaction is Papyrus over SetFactionRank, no
// native of its own), each named as FunctionsDump.txt has it. A record
// row's owner is OwnedForRecord's, and the lever's `recorded` skips its
// latent body on a seat that does not own it. The player's own hits and
// standing cross too and the client drops them: a handful an encounter, and
// the batch crosses once a frame whatever it holds. Every other state row
// is counted and, since rung 3, touched (QueueTouch)
enum class RecordRow : std::uint8_t
{
  None,
  RefState,
  Standing
};

struct BatchedRowName
{
  const char* cls;
  const char* fn;
  RecordRow record;
};

constexpr BatchedRowName kBatchedStateRows[] = {
  { "Actor", "Kill", RecordRow::None },
  { "Actor", "KillSilent", RecordRow::None },
  { "ObjectReference", "ProcessTrapHit", RecordRow::None },
  { "ObjectReference", "Enable", RecordRow::RefState },
  { "ObjectReference", "EnableNoWait", RecordRow::RefState },
  { "ObjectReference", "Disable", RecordRow::RefState },
  { "ObjectReference", "DisableNoWait", RecordRow::RefState },
  { "ObjectReference", "Delete", RecordRow::RefState },
  { "Actor", "SetFactionRank", RecordRow::Standing },
  { "Actor", "ModFactionRank", RecordRow::Standing },
  { "Actor", "RemoveFromFaction", RecordRow::Standing },
  { "Actor", "RemoveFromAllFactions", RecordRow::Standing },
  { "Actor", "SetCrimeFaction", RecordRow::Standing },
  { "Actor", "SetRelationshipRank", RecordRow::Standing }
};

// thuum docs/DECISIONS.md ADR-068 D-S3 (traps follow the interaction;
// TrapFamily.h): the three state rows whose call on a trap family reference
// crosses in the batch as kind "state" (the slice's interface of
// 2026-11-05): SetDestroyed, a single-use result skymp5-client reports as
// DestructionStage (point 3), and SetMotionType and ApplyHavokImpulse, the
// firing that sets a trap body moving, which the client's prop stream claims
// (point 4). On a body another seat streams (TrapFamily::Followed) the two
// motion rows run with a neutral argument instead and are not queued
enum class TrapRow : std::uint8_t
{
  None,
  Destroyed,
  Motion,
  Impulse
};

struct TrapRowName
{
  const char* cls;
  const char* fn;
  TrapRow trap;
};

constexpr TrapRowName kTrapRows[] = {
  { "ObjectReference", "SetDestroyed", TrapRow::Destroyed },
  { "ObjectReference", "SetMotionType", TrapRow::Motion },
  { "ObjectReference", "ApplyHavokImpulse", TrapRow::Impulse }
};

// thuum ADR-068 amendment B (rung 4, AliasFill's reference half; the plan's
// 1(a)): the two everyone rows whose call crosses in the batch on every
// seat, as kind "everyone" (OnAliasCall): ReferenceAlias.ForceRefTo (its
// argument the reference) and Clear, which skymp5-client reports as
// AliasFill 79. An alias has no owner (D-F1 C: every seat's quests run), so
// every seat runs the call and reports it, and the server keeps the latest
// fill per quest and alias; the call's self is the alias's quest and its
// alias the alias's id (AliasIdOfHandle)
constexpr std::pair<const char*, const char*> kAliasRows[] = {
  { "ReferenceAlias", "ForceRefTo" },
  { "ReferenceAlias", "Clear" }
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
  // the row's record kind among kBatchedStateRows (rung 4), None for every
  // other row: a record row's owner is OwnedForRecord's, and the lever's
  // `recorded` skips its latent body on a non-owner
  RecordRow record = RecordRow::None;
  // one of kAliasRows (rung 4): batched on every seat (OnAliasCall)
  bool alias = false;
  // the row's place among kTrapRows (D-S3), None for every other row
  TrapRow trap = TrapRow::None;
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
// an array parameter; " (unresolved)" a subject that names no reference,
// or an alias row's self that names no alias of a quest (rung 4);
// " (frame mismatch)" a dispatch whose top frame is not the native's own, or
// whose arguments' page cannot be found (H-S1 failing), or a followed body's
// motion call whose frame does not hold the row's declared shape; " (skipped)"
// a non-owned state row whose body the lever skipped (scriptNonOwnerSkip
// nonLatent, or recorded since rung 4: a latent record row's too); " (trap)"
// a trap family reference's call, every seat's own (ADR-068 D-S3 point 1: a
// state row run here, an action row run under the script action's mark,
// neither queued); " (followed)" a script's SetMotionType or
// ApplyHavokImpulse on a trap body another seat streams, run neutralized
// (D-S3 point 4); no suffix, an owned self's state row, run here and
// counted, and since rung 3 touched (QueueTouch: the script record's
// trigger).
// An everyone row's calls are counted by self instead (g_rowSelves, the
// flip's "Class.Name on <id>"), except an alias row's, batched on every seat
// since rung 4; a batched call is the batch's, never counted
enum CountKind : std::size_t
{
  kCountOwned,
  kCountPlatform,
  kCountNotHosted,
  kCountLocal,
  kCountUnrelayable,
  kCountUnresolved,
  kCountFrameMismatch,
  kCountSkipped,
  kCountTrap,
  kCountFollowed,
  kCountKinds
};

constexpr const char* kCountSuffix[kCountKinds] = { "",
                                                    " (platform)",
                                                    " (not hosted)",
                                                    " (local)",
                                                    " (unrelayable)",
                                                    " (unresolved)",
                                                    " (frame mismatch)",
                                                    " (skipped)",
                                                    " (trap)",
                                                    " (followed)" };

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

// an alias's id within its quest, by the encoding above: the middle word of
// a kind-0 handle that is no plain form's (BGSBaseAlias::aliasID,
// CommonLibSSE-NG include/RE/B/BGSBaseAlias.h:77, its quest's form id in the
// low 32 bits; adr065 Q1: 53508 builds such a handle for VM types 139 to
// 141, BGSBaseAlias.h:15, and 53512 resolves it through the quest's alias by
// id). HYPOTHESIS H-AF3 (the rung 4 plan's section 4): x-alias-premise v1
// reads the batch's quest and alias for a ForceRefTo against the alias
// step's. None for any other handle
constexpr std::optional<std::uint16_t> AliasIdOfHandle(RE::VMHandle handle)
{
  if ((handle >> 48) != 0 || IsPlainFormHandle(handle)) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>((handle >> 32) & 0xFFFF);
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

// rung 4's owner of a record row's subject (the plan's 1(a), the rows
// table's "Subject's owner"): OwnedHere's, except a game-file actor the
// server has given this seat no word on (neither hosted here nor a puppet,
// Hosted.h and Puppets.h), which is judged as a world object is, by its
// cell's host: an actor the server never loaded has no simulator (the
// ambush draugr 0x380ba, which trigger 0x4e421 enables; thuum
// docs/research/trap-census-2026-11-05.md:162), and its cell's host enables
// it. An actor at or above 0xff000000 never is (the players'
// figures and what they place begin there, the gate's line in OnCall): its
// owner is the server's word alone
bool OwnedForRecord(const RE::TESObjectREFR* ref, bool isActor)
{
  const std::uint32_t id = ref->formID;
  if (!isActor || id == 0x14 || id >= 0xff000000 || Hosted::Is(id) ||
      Puppets::Is(id)) {
    return OwnedHere(ref, isActor);
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

// rung 3 (thuum ADR-068, the script record; the plan's 1(a)): the calling
// script's reference, the frame below the native's own, its self
// (CommonLibSSE-NG include/RE/S/StackFrame.h:27) through RefOfObject; null
// for no such frame, a global function's frame, or a script that stands for
// no reference (a quest's, an alias's, an effect's). Every batched call
// carries its form id as callerSelf, 0 for none. HYPOTHESIS H-R9:
// previousFrame's self is the calling script's object (a touch on the
// portcullis's Activate names the lever, x-script-state-read's pull)
const RE::TESObjectREFR* CallerRefOf(const RE::BSScript::StackFrame* below)
{
  if (!below) {
    return nullptr;
  }
  auto* vm = VM::GetSingleton();
  return RefOfObject(vm ? vm->GetObjectHandlePolicy() : nullptr,
                     ObjectOf(below->self));
}

// one captured call of an owned self, the shared shape's Call; since rung 3
// also the calling script's self and the touch mark, since rung 4 an alias
// row's alias
struct QueuedCall
{
  std::size_t row = 0;
  std::uint32_t self = 0;
  std::uint32_t argCount = 0;
  std::array<CallArg, kMaxArgs> args{};
  std::string caller;
  std::string handler;
  // the calling script's self (CallerRefOf), 0 for none
  std::uint32_t callerSelf = 0;
  bool latent = false;
  std::uint32_t stackId = 0;
  // rung 3's touch: an owned state row's call, no arguments (QueueTouch),
  // raised as kind "touch"
  bool touch = false;
  // rung 4's alias row (OnAliasCall): the alias's id within the quest that
  // `self` names, raised as `alias`; none on every other call
  std::optional<std::uint16_t> alias;
};

// this frame's batch: appended on the dispatching thread for a batched row
// alone (an owned self's action row, or Kill), so the lock is taken rarely
// and never for an untabled call; the flag is up while a flush is
// scheduled
std::mutex g_batchMutex;
std::vector<QueuedCall> g_batch;
std::atomic<bool> g_batchScheduled{ false };
// rung 3's touches already in this frame's batch, by self and the caller's
// self (QueueTouch's key), under g_batchMutex and cleared with the batch at
// the flush
std::unordered_set<std::uint64_t> g_touched;
// rung 3's switch on the touch (setScriptRecordTouches, PuppetApi.cpp; thuum
// .claude/rules/native.md's kill switch), on by default so a client that
// never sets it keeps the batch it has; read first in QueueTouch
std::atomic<bool> g_touchesEnabled{ true };

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
// event): { cls, fn, kind, self, args, caller, handler, callerSelf, latent,
// stackId, alias? }, cls and fn the table's spelling; kind "touch" for rung
// 3's touch (an owned state row's call, its args empty); alias on rung 4's
// alias rows alone (ForceRefTo, Clear), absent from every other call
Napi::Object CallToJs(Napi::Env env, const QueuedCall& call)
{
  const NativeRow& native = kRows[call.row];
  auto obj = Napi::Object::New(env);
  AddObjProperty(&obj, "cls", native.cls);
  AddObjProperty(&obj, "fn", native.fn);
  AddObjProperty(&obj, "kind", call.touch ? "touch" : KindName(native.kind));
  AddObjProperty(&obj, "self", call.self);
  auto args = Napi::Array::New(env, call.argCount);
  for (uint32_t i = 0; i < call.argCount; ++i) {
    args.Set(i, ArgToJs(env, call.args[i]));
  }
  obj.Set("args", args);
  AddObjProperty(&obj, "caller", call.caller);
  AddObjProperty(&obj, "handler", call.handler);
  AddObjProperty(&obj, "callerSelf", call.callerSelf);
  AddObjProperty(&obj, "latent", call.latent);
  AddObjProperty(&obj, "stackId", call.stackId);
  if (call.alias) {
    AddObjProperty(&obj, "alias", *call.alias);
  }
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
    g_touched.clear();
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

// the table's map and RowInfo complete (InstallHooks): the platform's
// direct-call count reads them from the worker, which no hook orders
std::atomic<bool> g_tableReady{ false };

// the non-owner skip lever (6.11 item 3, rung 2's last commit; rung 4's
// recorded), read once at kDataLoaded before the replacement is installed
// and never written after. None (the default): every body runs, as the
// attach's did. NonLatent: a non-owned state row whose native is not latent
// skips its body. Recorded (rung 4, the plan's 1(a)): NonLatent's, and a
// non-owned record row's latent body too (Enable, Disable and Delete by
// FunctionsDump.txt, read from the bound rows' own latency), so a guest's
// lifecycle Disable leaves its copy to the served word (H-F2, at
// SetEmptyResult). Every other latent row always runs (H-F1: what a
// skipped latent must return is unread; x-native-frame's trace settles it)
enum class NonOwnerSkip
{
  None,
  NonLatent,
  Recorded
};
NonOwnerSkip g_nonOwnerSkip = NonOwnerSkip::None;

const char* LeverName(NonOwnerSkip skip)
{
  switch (skip) {
    case NonOwnerSkip::NonLatent:
      return "nonLatent";
    case NonOwnerSkip::Recorded:
      return "recorded";
    case NonOwnerSkip::None:
      break;
  }
  return "none";
}

// the lever's judgment of a non-owned state row: true when its body is
// skipped
bool SkipsBody(const RowInfo& info)
{
  switch (g_nonOwnerSkip) {
    case NonOwnerSkip::NonLatent:
      return !info.latent;
    case NonOwnerSkip::Recorded:
      return !info.latent || info.record != RecordRow::None;
    case NonOwnerSkip::None:
      break;
  }
  return false;
}

// the lever's skipped selves, the first kSkipLinesPerRow distinct ones per
// row in each census window logged (the rung 4 plan's line, which
// x-skip-latent v1 reads beside the self's script record, H-F2), on fixed
// slots as CountSelf keeps them: a slot's key is kSlotUsed | the self's form
// id, 0 while free, every slot freed at the census's flush
constexpr std::size_t kSkipLinesPerRow = 3;
std::array<std::array<std::atomic<std::uint64_t>, kSkipLinesPerRow>, kRowCount>
  g_skipLines{};

void LogSkipped(std::size_t row, std::uint32_t selfId)
{
  const std::uint64_t key = kSlotUsed | selfId;
  for (auto& slot : g_skipLines[row]) {
    std::uint64_t seen = slot.load(std::memory_order_relaxed);
    if (seen == 0 &&
        slot.compare_exchange_strong(seen, key, std::memory_order_relaxed)) {
      logger::info("[verb:scripts] {}.{} on {:x}: skipped, not hosted here "
                   "({})",
                   kRows[row].cls, kRows[row].fn, selfId,
                   LeverName(g_nonOwnerSkip));
      return;
    }
    if (seen == key) {
      return;
    }
  }
}

// what the dispatch hook raises for one call's extent (PlatformCall.h's
// bracket)
struct Marks
{
  // the platform's depth: a latent call the platform dispatched (VmCall::Run;
  // the dispatched Stack's callback is a live VmCallback)
  bool platform = false;
  // the script action's: an owned action row's call (6.7)
  bool action = false;
  // the body skipped (the lever), and the declared return's kind for the
  // result left in its place
  bool skip = false;
  RE::BSScript::TypeInfo::RawType result =
    RE::BSScript::TypeInfo::RawType::kNone;
};

// one call queued from its frame into this frame's batch (the shared shape's
// Call): its self (an alias row's quest, beside the alias's id), its
// arguments read through the engine's accessor with the frame's page (found
// here when the subject's read did not need it) and its caller; false,
// counted " (frame mismatch)", when the arguments' page cannot be found
bool QueueFromFrame(std::size_t row, const RowInfo& info,
                    const RE::BSScript::Stack* stack,
                    const RE::BSScript::StackFrame* top, std::uint32_t page,
                    std::uint32_t self,
                    std::optional<std::uint16_t> alias = std::nullopt)
{
  QueuedCall call;
  call.row = row;
  call.self = self;
  call.alias = alias;
  call.latent = info.latent;
  call.stackId = stack->stackID;
  call.argCount = info.params;
  if (info.params > 0) {
    if (page == kNoPage) {
      page = top->GetPageForFrame();
    }
    if (page == kNoPage) {
      Count(row, kCountFrameMismatch);
      return false;
    }
    for (std::uint32_t i = 0; i < info.params; ++i) {
      call.args[i] =
        ReadArg(top->GetStackFrameVariable(i, page), info.types[i]);
    }
  }
  NameCaller(top->previousFrame, call.caller, call.handler);
  const RE::TESObjectREFR* callerRef = CallerRefOf(top->previousFrame);
  call.callerSelf = callerRef ? callerRef->formID : 0;
  QueueCall(std::move(call));
  return true;
}

// rung 3's touch (thuum ADR-068, the script record; OnCall's (e)): an owned
// state row's call queued as kind "touch", no arguments, with its caller and
// the caller's self as every batched call carries them, so skymp5-client's
// ScriptStateService takes the self and the caller's self as due for
// SerializeScriptState. Filtered here (rule 9), cheapest first: nothing at
// all while the switch is off (setScriptRecordTouches), then dropped when
// neither is a world object a record can carry (the self an actor or at or
// above 0xff000000; the caller's self no reference, an actor, or at or
// above it: SerializeScriptState refuses those) and when this frame's batch
// already holds the pair, so a script's loop of state calls crosses once a
// frame; the caller's names are read last, for a touch that is queued
void QueueTouch(std::size_t row, const RowInfo& info,
                const RE::BSScript::Stack* stack,
                const RE::BSScript::StackFrame* top,
                const RE::TESObjectREFR* ref, bool isActor)
{
  if (!g_touchesEnabled.load(std::memory_order_relaxed)) {
    return;
  }
  const RE::TESObjectREFR* callerRef = CallerRefOf(top->previousFrame);
  const std::uint32_t callerSelf = callerRef ? callerRef->formID : 0;
  const bool selfCarries = !isActor && ref->formID < 0xff000000;
  const bool callerCarries = callerRef &&
    callerRef->GetFormType() != RE::FormType::ActorCharacter &&
    callerSelf < 0xff000000;
  if (!selfCarries && !callerCarries) {
    return;
  }
  const std::uint64_t key =
    (static_cast<std::uint64_t>(ref->formID) << 32) | callerSelf;
  {
    std::lock_guard<std::mutex> lock(g_batchMutex);
    if (!g_touched.insert(key).second) {
      return;
    }
  }
  QueuedCall call;
  call.row = row;
  call.self = ref->formID;
  call.callerSelf = callerSelf;
  call.latent = info.latent;
  call.stackId = stack->stackID;
  call.touch = true;
  NameCaller(top->previousFrame, call.caller, call.handler);
  QueueCall(std::move(call));
}

// a followed body's motion call neutralized in the native's own frame, before
// the original marshals it (MarshallAndDispatch unpacks every argument from
// the frame at the call, CommonLibSSE-NG include/RE/N/NativeFunction.h:91-92):
// SetMotionType's motion type (its first parameter, an Int, FunctionsDump.txt)
// becomes 4, Keyframed (TESObjectREFR::MotionType::kKeyframed,
// include/RE/T/TESObjectREFR.h:126-135), and ApplyHavokImpulse's magnitude
// (its fourth, a Float) becomes 0, so the body runs and ends the script's
// latent wait as the engine ends it, while the body stays where the prop
// stream holds it. Run, not skipped: both rows are latent, and what a skipped
// latent must leave is unread (H-F1), a skipped one perhaps never resuming
// its script. HYPOTHESIS H-AW: the value written here is the one the body
// reads, and the engine's Keyframed and a zero impulse leave a followed body
// still under the stream (x-trap-boulders v1: the seven rocks agree within
// 32 units on both seats and the server after 6 s). False, nothing written,
// when the frame does not hold the row's declared shape; `what` names the
// write for the log
bool Neutralize(const RowInfo& info, const RE::BSScript::StackFrame* top,
                std::uint32_t page, std::string& what)
{
  const bool motion = info.trap == TrapRow::Motion;
  const std::uint32_t index = motion ? 0 : 3;
  if (info.params != (motion ? 2u : 4u) ||
      !(motion ? info.types[index].IsInt() : info.types[index].IsFloat())) {
    return false;
  }
  if (page == kNoPage) {
    page = top->GetPageForFrame();
  }
  if (page == kNoPage) {
    return false;
  }
  RE::BSScript::Variable& arg = top->GetStackFrameVariable(index, page);
  if (motion && arg.IsInt()) {
    what = fmt::format("motion type 4 (Keyframed) for the script's {}",
                       arg.GetSInt());
    arg.SetSInt(
      static_cast<std::int32_t>(RE::TESObjectREFR::MotionType::kKeyframed));
    return true;
  }
  if (!motion && arg.IsFloat()) {
    what = fmt::format("magnitude 0 for the script's {}", arg.GetFloat());
    arg.SetFloat(0.f);
    return true;
  }
  return false;
}

// a trap family reference's call (ADR-068 D-S3 point 1), every seat's own:
// every seat fires its own copy, so the call runs here and is counted
// " (trap)". An action row runs under the script action's mark for its
// extent, so the engine-level animation hooks leave it too, and is never
// queued (every seat plays it from its own firing: no ScriptCall, no mirror,
// no lastAnimation). A state row is the copy's own state, except the three
// kTrapRows, which cross in the batch as kind "state"; a followed body's two
// motion rows run neutralized instead, counted " (followed)"
Marks OnFamilyCall(std::size_t row, const RowInfo& info,
                   const RE::BSScript::Stack* stack,
                   const RE::BSScript::StackFrame* top, std::uint32_t page,
                   std::uint32_t self)
{
  Marks marks;
  const NativeRow& native = kRows[row];
  if (native.kind == NativeKind::Action) {
    marks.action = true;
    Count(row, kCountTrap);
    return marks;
  }
  if (info.trap == TrapRow::None) {
    Count(row, kCountTrap);
    return marks;
  }
  if (info.trap != TrapRow::Destroyed && TrapFamily::Followed::Is(self)) {
    std::string what;
    if (!Neutralize(info, top, page, what)) {
      Count(row, kCountFrameMismatch);
      return marks;
    }
    Count(row, kCountFollowed);
    static std::atomic<uint64_t> s_neutralized{ 0 };
    const auto n = ++s_neutralized;
    if (n <= 3 || n % 100 == 0) {
      logger::info("[verb:traps] {}.{} on {:x}, a trap body another seat "
                   "streams: run with {}, counted (followed) ({} so far)",
                   native.cls, native.fn, self, what, n);
    }
    return marks;
  }
  if (!info.relayable) {
    Count(row, kCountUnrelayable);
    return marks;
  }
  QueueFromFrame(row, info, stack, top, page, self);
  return marks;
}

// rung 4's AliasFill, the reference half (thuum ADR-068 amendment B; the
// plan's 1(a)): an alias row's call (kAliasRows) queued on every seat, its
// self the alias's quest (the handle's low 32 bits, EveryoneSelfId's id) and
// beside it the alias's id (AliasIdOfHandle), its arguments, caller and
// caller's self as every batched call carries them; never counted (a
// batched call is the batch's). Filtered here (rule 9) as the engine's own
// GetObjectForHandle filters an alias's handle (adr065 Q1: 53512 refuses a
// form that is no Quest, FormTypes.h:217): a self whose handle is no
// alias's, or whose low 32 bits name no quest the form table knows, is
// counted " (unresolved)", its first three logged, and never crosses (H-AF3
// failing reads there)
void OnAliasCall(std::size_t row, const RowInfo& info,
                 const RE::BSScript::Stack* stack,
                 const RE::BSScript::StackFrame* top)
{
  const RE::BSScript::Object* object = ObjectOf(top->self);
  const RE::VMHandle handle = object ? object->GetHandle() : 0;
  const std::optional<std::uint16_t> alias = AliasIdOfHandle(handle);
  const std::uint32_t quest = FormIdOfHandle(handle);
  const RE::TESForm* form =
    alias && quest != 0 ? RE::TESForm::LookupByID(quest) : nullptr;
  if (!form || form->GetFormType() != RE::FormType::Quest) {
    Count(row, kCountUnresolved);
    static std::atomic<uint64_t> s_unresolved{ 0 };
    if (++s_unresolved <= 3) {
      logger::warn("[verb:scripts] {}.{}'s self, handle {:x}, names no "
                   "alias of a quest the form table knows (H-AF3's encoding: "
                   "kind 0, a middle word below 0xFFFF, the quest in the low "
                   "32 bits); counted (unresolved), not batched",
                   kRows[row].cls, kRows[row].fn, handle);
    }
    return;
  }
  if (!info.relayable) {
    Count(row, kCountUnrelayable);
    return;
  }
  QueueFromFrame(row, info, stack, top, kNoPage, quest, alias);
}

// the seam's judgment of one dispatch, 6.2's order (rule 9: the cheapest
// test first). The body runs unless the lever skips it (marks.skip)
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
    // a call under the direct path's Scope is counted where the platform
    // makes it (ScriptNatives::CountPlatformCall: nested in the OnUpdate
    // native's own Call, it never reaches the replacement), every other one
    // of the platform's here
    if (!PlatformCall::Active()) {
      Count(row, kCountPlatform);
    }
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
  // counted; an alias row's call is batched instead (rung 4, OnAliasCall)
  if (native.kind == NativeKind::Everyone) {
    if (info.alias) {
      OnAliasCall(row, info, stack, top);
      return marks;
    }
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
  // the trap family, before the owner (ADR-068 D-S3 point 1; TrapFamily.h):
  // a world object whose every attached script is in the family, by the
  // gate's own test on the gate's own world objects (no actor, a parent
  // cell, an id below 0xff000000, where the players' figures and what they
  // place begin; FridaHooks.cpp OnSendEventEnter), is every seat's own,
  // whoever hosts its cell (OnFamilyCall). An actor never is:
  // ProcessTrapHit's self is the hit actor, judged by (d) below, so the hit
  // counts on that actor's own seat alone and a non-owner's copy is counted,
  // or skipped by the lever
  if (!isActor && ref->formID < 0xff000000 && ref->GetParentCell()) {
    const RE::BSScript::Object* object = ObjectOf(*subject);
    if (object && TrapFamily::IsFamily(ref, object->GetHandle())) {
      return OnFamilyCall(row, info, stack, top, page, ref->formID);
    }
  }
  // (d) the owner, a record row's by OwnedForRecord (rung 4). Not owned:
  // counted, and the body runs (an action row's jump, the pillar's
  // StartState0N from a guest's OnCellLoad, is wanted everywhere), unless
  // the lever skips a state row's body (6.11 item 3: a guest's lifecycle
  // handler performs the host's PlaceAtMe or Lock on its own copy, skipped
  // by nonLatent; its RemoveItem and MoveTo are latent, FunctionsDump.txt,
  // and wait for H-F1's read; its Disable, a latent record row, is skipped
  // by recorded, H-F2), the first skipped selves logged (LogSkipped)
  const bool owned = info.record != RecordRow::None
    ? OwnedForRecord(ref, isActor)
    : OwnedHere(ref, isActor);
  if (!owned) {
    if (native.kind == NativeKind::State && SkipsBody(info)) {
      marks.skip = true;
      marks.result = info.result;
      Count(row, kCountSkipped);
      LogSkipped(row, ref->formID);
      return marks;
    }
    Count(row, kCountNotHosted);
    return marks;
  }
  // (e) owned. A state row is the record's (ADR-065: the change form carries
  // it), counted; a batched one (Kill and KillSilent, ProcessTrapHit, and
  // since rung 4 the record rows) crosses in the batch with its arguments
  // instead. Since rung 3 (thuum ADR-068, the script record) every other
  // state row's call also crosses as a touch (QueueTouch): the owner's mark
  // that the self's or the calling script's state may have changed
  if (native.kind == NativeKind::State && !info.batchedState) {
    Count(row, kCountOwned);
    QueueTouch(row, info, stack, top, ref, isActor);
    return marks;
  }
  // an owned action row's extent is the script action's (6.7) once its call
  // is queued below: a path that drops the call (unrelayable, a frame
  // mismatch) leaves the mark down, so the engine-level hooks still report the
  // animation and nothing goes silent (rung 2's review, finding 5)
  if (!info.relayable) {
    Count(row, kCountUnrelayable);
    return marks;
  }
  if (!QueueFromFrame(row, info, stack, top, page, ref->formID)) {
    return marks;
  }
  marks.action = native.kind == NativeKind::Action;
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
  for (const auto& batched : kBatchedStateRows) {
    if (_stricmp(kRows[row].cls, batched.cls) == 0 &&
        _stricmp(kRows[row].fn, batched.fn) == 0) {
      info.batchedState = true;
      info.record = batched.record;
    }
  }
  for (const auto& trapRow : kTrapRows) {
    if (_stricmp(kRows[row].cls, trapRow.cls) == 0 &&
        _stricmp(kRows[row].fn, trapRow.fn) == 0) {
      info.trap = trapRow.trap;
    }
  }
  for (const auto& [cls, name] : kAliasRows) {
    if (_stricmp(kRows[row].cls, cls) == 0 &&
        _stricmp(kRows[row].fn, name) == 0) {
      info.alias = true;
    }
  }
}

// a name among the seam's lists (kBatchedStateRows, kTrapRows, kAliasRows)
// that no bound table row carries, said once at kDataLoaded: the lists find
// their rows by class and name, so a row the table lacks or spells
// otherwise is never silent
void WarnUnbound(const char* list, const char* cls, const char* fn)
{
  for (std::size_t row = 0; row < kRowCount; ++row) {
    if (g_rows[row].fn && _stricmp(kRows[row].cls, cls) == 0 &&
        _stricmp(kRows[row].fn, fn) == 0) {
      return;
    }
  }
  logger::warn("[verb:scripts] the {} row {}.{} is no bound table row; "
               "never judged as one",
               list, cls, fn);
}

// how InstallHooks names a row's crossing beside its latency
const char* CrossingNote(const RowInfo& info)
{
  if (info.alias) {
    return ", batched on every seat (an alias fill)";
  }
  switch (info.record) {
    case RecordRow::RefState:
      return ", batched (a ref state record row)";
    case RecordRow::Standing:
      return ", batched (a standing record row)";
    case RecordRow::None:
      break;
  }
  return info.batchedState ? ", batched" : "";
}

// the lever's value: "scriptNonOwnerSkip" in skymp5-client-settings.txt,
// found in the platform's plugin folders (SkyrimPlatform.ini's
// PluginFolders, Settings::GetPluginFolders: the folders SkyrimPlatform.cpp
// loads every "-settings.txt" from, a later folder's file winning as it
// does there) and read with the same reader (Viet::ReadFileIntoString), so
// the lab sets it as it sets cellHost and npcPuppet (relaunch {files: [{json:
// Data/Platform/Plugins/skymp5-client-settings.txt, key: scriptNonOwnerSkip,
// value: nonLatent}]}). "nonLatent" skips a non-latent body, "recorded" a
// latent record row's too (rung 4); "none", an absent key or file, an
// unreadable file or any other value leaves the skip off
NonOwnerSkip ReadNonOwnerSkip()
{
  NonOwnerSkip skip = NonOwnerSkip::None;
  try {
    const auto folders = Settings::GetPlatformSettings()->GetPluginFolders();
    for (const auto& folder : *folders) {
      const auto path = folder / "skymp5-client-settings.txt";
      if (!std::filesystem::exists(path)) {
        continue;
      }
      skip = NonOwnerSkip::None;
      const auto settings =
        nlohmann::json::parse(Viet::ReadFileIntoString(path), nullptr, false);
      if (!settings.is_object()) {
        logger::warn("[verb:scripts] {} is no JSON object; "
                     "scriptNonOwnerSkip read as none",
                     path.string());
        continue;
      }
      const auto it = settings.find("scriptNonOwnerSkip");
      if (it == settings.end()) {
        continue;
      }
      const std::string value = it->is_string() ? it->get<std::string>() : "";
      if (value == "nonLatent") {
        skip = NonOwnerSkip::NonLatent;
      } else if (value == "recorded") {
        skip = NonOwnerSkip::Recorded;
      } else if (value != "none") {
        logger::warn("[verb:scripts] scriptNonOwnerSkip in {} is not none, "
                     "nonLatent or recorded; read as none",
                     path.string());
      }
    }
  } catch (const std::exception& e) {
    logger::warn("[verb:scripts] scriptNonOwnerSkip unread ({}); none",
                 e.what());
    return NonOwnerSkip::None;
  }
  return skip;
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
    // the next window logs its own first skipped selves (LogSkipped)
    for (auto& slot : g_skipLines[row]) {
      slot.store(0, std::memory_order_relaxed);
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
// direct path. The cost on every native call in the game: Gum's
// replacement thunk, the bracket's level (PlatformCall.h) and one hash
// lookup of `this` against the table; a call outside it does nothing more.
// This replaced the flip's 24 entry hooks and their register conventions
// (each row's layout a HYPOTHESIS, the float arguments unreadable), and the
// attach of rung 2's first commits, which could not skip a body.
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
// Stack's +0x80 and +0x90, the thread) and DispatchMethodCall2 on a guest
// applying a relayed stage, expecting Dispatch(F) then Call(S, S.callback
// == F, S.id == N) on a Papyrus thread, the census's "(platform)" row for
// the stage's native.
using NativeCallFn = RE::BSScript::IFunction::CallResult (*)(
  RE::BSScript::NF_util::NativeFunctionBase* self,
  const RE::BSTSmartPointer<RE::BSScript::Stack>& stack,
  RE::BSScript::ErrorLogger* logger,
  RE::BSScript::Internal::VirtualMachine* vm, bool arg4);
NativeCallFn g_nativeCallOriginal = nullptr;

// the result a skipped body leaves (H-F1, HYPOTHESIS until x-native-frame's
// trace reads a skipped row): the VM takes a native's result from the
// Stack's returnValue once Call answers kCompleted (Stack.h:79; the
// platform's own direct path reads it there after its Call, CallNative.cpp,
// on the OnUpdate native's own frame, which Call therefore neither pops nor
// replaces), and the result is the declared type's empty value, what a body
// that does nothing writes: None for a void or an object return
// (MarshallAndDispatch's SetNone, NativeFunction.h:96-99; PackHandle's null,
// PackUnpack.cpp:48-55), false, 0, 0.0 and "" for the literals. A latent
// record row skipped under recorded (rung 4) leaves the same and answers
// kCompleted where its body would leave the stack waiting on a latent
// result (IFunction::CallResult's kCompleted 0 against kInProgress 2,
// CommonLibSSE-NG include/RE/I/IFunction.h:36-38; a latent native's own
// answer is a LatentStatus, kFailed or kStarted, NativeLatentFunction.h:
// 59-62): HYPOTHESIS H-F2, the VM resumes the stack as after a non-latent
// call and no stack waits on a result that never comes (x-skip-latent v1:
// c2's script_state(id).quiescent true within 1 s of its skipped line for
// that id; if it fails, nonLatent stays the lab's lever)
void SetEmptyResult(RE::BSScript::Variable& result,
                    RE::BSScript::TypeInfo::RawType type)
{
  using RawType = RE::BSScript::TypeInfo::RawType;
  switch (type) {
    case RawType::kBool:
      result.SetBool(false);
      return;
    case RawType::kInt:
      result.SetSInt(0);
      return;
    case RawType::kFloat:
      result.SetFloat(0.f);
      return;
    case RawType::kString:
      result.SetString("");
      return;
    default:
      result.SetNone();
      return;
  }
}

// The replacement (the QuestStageSink 53987 shape, through
// HookHandler::Replace): what the attach did at the call's entry, then the
// body through Gum's trampoline in every case the judgment says run, the
// level (and its marks) held for the call's extent. What each path returns:
// the original's own CallResult whenever the body runs; kCompleted, with
// the row's empty result in returnValue, when the lever skips it (a latent
// record row's too under recorded, H-F2). A call of
// this function nested in a replaced call's original on one thread never
// reaches here: Gum runs it straight to the original (frida-gum
// 698b356fef0e, frida 16.0.19's, gum/guminterceptor.c:1503-1513: the top
// invocation entry is this function's, its calling_replacement set at
// :1606 for the replacement's whole extent), so the
// platform's direct calls, made inside the OnUpdate native's own Call, are
// counted where they are made (CountPlatformCall) and run untouched, as the
// platform's own would anyway; a script's call is never so nested
// (HYPOTHESIS: a native's body runs no other native through the VM's
// dispatch on its own thread; a Frida trace on 104651 would show a nested
// Call on a Papyrus thread if one does). The calling convention is the
// member function's (Windows x64: this in RCX, the stack's smart pointer by
// reference in RDX, the logger in R8, the VM in R9, the bool on the stack),
// the DamageActor note's HYPOTHESIS, read by the first stage's census (the
// table's rows judged, the game's natives running)
RE::BSScript::IFunction::CallResult NativeCallReplacement(
  RE::BSScript::NF_util::NativeFunctionBase* self,
  const RE::BSTSmartPointer<RE::BSScript::Stack>& stack,
  RE::BSScript::ErrorLogger* logger,
  RE::BSScript::Internal::VirtualMachine* vm, bool arg4)
{
  // the entry stack pointer (the slot of this call's return address, the
  // rsp the attach read from Gum's context), the stranded-level guard's key
  // (PlatformCall.h); the stranded levels go first, so the platform's depth
  // OnCall reads is live
  const auto sp = reinterpret_cast<std::uintptr_t>(_AddressOfReturnAddress());
  PlatformCall::PopStrandedCalls(sp);
  const Marks marks = OnCall(self, stack.get());
  if (marks.skip) {
    // the body never runs: the call completes with the row's empty result
    SetEmptyResult(stack->returnValue, marks.result);
    return RE::BSScript::IFunction::CallResult::kCompleted;
  }
  // the level is left when the call returns or a C++ exception unwinds
  // through this frame; an unwind that skips it (SEH, longjmp) is the
  // stranded-level guard's
  const PlatformCall::CallLevel level(marks.platform, marks.action, sp);
  return g_nativeCallOriginal(self, stack, logger, vm, arg4);
}
}

void ScriptNatives::Tick()
{
  FlushCensus(NowMs());
}

void ScriptNatives::FlushAheadOnUpdate()
{
  {
    std::lock_guard<std::mutex> lock(g_batchMutex);
    if (g_batch.empty()) {
      return;
    }
  }
  // the batch's flush on the update's queue, ahead of the task the caller
  // adds next; the tick's flush already scheduled finds the batch empty (or
  // carries a call queued after this one) and FlushBatch lowers the flag, so
  // the next call schedules its own as before
  SkyrimPlatform::GetSingleton()->AddUpdateTask(
    [](Napi::Env env) { FlushBatch(env); });
}

bool ScriptNatives::InScriptAction()
{
  return PlatformCall::InAction();
}

void ScriptNatives::SetTouchesEnabled(bool enabled)
{
  g_touchesEnabled.store(enabled, std::memory_order_relaxed);
}

bool ScriptNatives::TouchesEnabled()
{
  return g_touchesEnabled.load(std::memory_order_relaxed);
}

void ScriptNatives::CountPlatformCall(const RE::BSScript::IFunction* fn)
{
  if (!g_tableReady.load(std::memory_order_acquire)) {
    return;
  }
  const auto it = g_rowOf.find(fn);
  if (it != g_rowOf.end()) {
    Count(it->second, kCountPlatform);
  }
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
  // all of it before the dispatch is replaced below
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
    logger::info("[verb:scripts] table row {}.{}: {} parameter(s), {}{}{}, "
                 "relay {}",
                 native.cls, native.fn, info.params,
                 info.latent ? "latent" : "not latent",
                 info.relayable ? "" : ", unrelayable (an array parameter)",
                 CrossingNote(info), native.relay);
  }
  logger::info("[verb:scripts] {} of {} table rows bound (NativeRows.inc, "
               "thuum docs/NATIVES.md)",
               boundRows, kRowCount);
  // the seam's name lists, found by class and name as the rows are
  for (const auto& batched : kBatchedStateRows) {
    WarnUnbound("batched state", batched.cls, batched.fn);
  }
  for (const auto& trapRow : kTrapRows) {
    WarnUnbound("trap", trapRow.cls, trapRow.fn);
  }
  for (const auto& [cls, name] : kAliasRows) {
    WarnUnbound("alias", cls, name);
  }
  // thuum ADR-068 D-S3: the trap family the gate and this seam read, named
  // once per process beside the table it extends (TrapFamily.h)
  TrapFamily::LogReady();
  // the lever, read before the replacement can ask it; under recorded the
  // line names the latent record rows it skips, by the bound rows' latency
  g_nonOwnerSkip = ReadNonOwnerSkip();
  std::string skipping = "every body runs";
  if (g_nonOwnerSkip == NonOwnerSkip::NonLatent) {
    skipping = "a non-owned state row's non-latent body is skipped, its "
               "row's empty result returned";
  } else if (g_nonOwnerSkip == NonOwnerSkip::Recorded) {
    std::string latentRecords;
    for (size_t row = 0; row < kRowCount; ++row) {
      if (g_rows[row].fn && g_rows[row].latent &&
          g_rows[row].record != RecordRow::None) {
        latentRecords +=
          fmt::format("{}{}.{}", latentRecords.empty() ? "" : ", ",
                      kRows[row].cls, kRows[row].fn);
      }
    }
    skipping = fmt::format(
      "a non-owned state row's non-latent body is skipped, and a non-owned "
      "record row's latent one ({}), each answered completed with its row's "
      "empty result (H-F2)",
      latentRecords.empty() ? std::string("none bound") : latentRecords);
  }
  logger::info("[verb:scripts] scriptNonOwnerSkip {}: {}",
               LeverName(g_nonOwnerSkip), skipping);
  g_tableReady.store(true, std::memory_order_release);
  // the dispatch replaced, once the table's map and the lever are complete,
  // so it never reads either being written
  const auto callAddress = Offsets::Hooks::NativeFunctionCall.address();
  if (Frida::HookHandler::GetSingleton()->Replace(
        callAddress, reinterpret_cast<void*>(&NativeCallReplacement),
        reinterpret_cast<void**>(&g_nativeCallOriginal))) {
    logger::info("[verb:scripts] native dispatch replaced: "
                 "NativeFunctionBase::Call at +{:x} (Address Library 104651), "
                 "{} table row(s) judged from the frame, the platform's "
                 "latent calls marked on the VM's thread",
                 callAddress - gameBase, g_rowOf.size());
  } else {
    logger::error("[verb:scripts] NativeFunctionBase::Call at +{:x}: Gum "
                  "refused the replacement; no table row is judged and "
                  "every body runs",
                  callAddress - gameBase);
  }
}
