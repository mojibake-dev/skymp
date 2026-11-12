#include "ScriptNatives.h"
#include "ActingScope.h"
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
#include "QuestCause.h"
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
#include <RE/P/PackUnpack.h>
#include <RE/S/Stack.h>
#include <RE/S/StackFrame.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/T/TESQuest.h>
#include <RE/T/TypeInfo.h>
#include <RE/V/Variable.h>
#include <RE/V/VirtualMachine.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fmt/format.h>
#include <intrin.h>
#include <iterator>
#include <limits>
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

// Arg0Method (thuum ADR-068 D-W2 A, the plan's section 7.2(a)): a method
// row whose subject is its first argument and whose call's self is the
// frame's own, Sound.Play's shape (the subject the source reference, the self
// the Sound)
enum class NativeSubject : std::uint8_t
{
  Self,
  Arg0,
  None,
  Arg0Method
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
// native of its own), each named as FunctionsDump.txt has it. A RefState
// row's owner is OwnedForRecord's, and the lever's `recorded` skips its
// latent body on a seat that does not own it; a standing row's owner is
// OwnedHere's and no lever skips its body (rung 4's fresh review, note 1). The
// player's own hits and standing cross too and the client drops them: a
// handful an encounter, and the batch crosses once a frame whatever it holds.
// Every other state row is counted and, since rung 3, touched (QueueTouch).
// Since rung 5 (thuum ADR-068 rung 5's full build, Eli's D-Q2 word, "yes
// full transformation"; docs/research/rung5-quests-2026-11-09.md 8(b)) the
// transformation's seven, a per-player quest's change to its own player:
// Actor.SetRace, AddSpell, RemoveSpell, AddShout, RemoveShout and EquipItem
// (FunctionsDump.txt: `SetRace(Race akRace)`, `Bool AddSpell(SPELL akSpell,
// Bool abVerbose)`, `Bool RemoveSpell(SPELL akSpell)`, `Bool AddShout(Shout
// akShout)`, `Bool RemoveShout(Shout akShout)`, `EquipItem(Form akItem, Bool
// abPreventRemoval, Bool abSilent)`), and since Eli's D-E1 A
// Actor.ChangeHeadPart, SKSE's (FunctionsDump.txt: `ChangeHeadPart(HeadPart
// hPart)`, its callback outside the game's module, so InstallHooks binds it
// from the VM's other bindings), none latent, batched with their typed
// arguments on the seat's own player alone (OwnedHere's 0x14), so the
// server records them beside RefState and the standing (R1: the form
// argument must be one the calling script's attachment names, H-VR1); on any
// other actor this seat owns each is a plain state row, counted and touched,
// and a non-owned one is judged by the lever as every state row is
// (a figure's copy of a twin's race is the server's appearance word's).
// Since Eli's D-R1 C (thuum docs/DECISIONS.md, rung 5's stage-eve words) the
// player's inventory too: ObjectReference.AddItem and RemoveItem
// (FunctionsDump.txt: `AddItem(Form akItemToAdd, Int aiCount, Bool
// abSilent)` and `RemoveItem(Form akItemToRemove, Int aiCount, Bool
// abSilent, ObjectReference akOtherContainer)`, both latent), batched with
// their typed arguments on the seat's own player alone, which the server
// records as reported (R2, no bound): a quest's reward, which the client's
// inventory apply undid while nothing told the server of it (x-acting-frame
// 20261110-072506). An owned call on any other reference stays the counted
// and touched state row it was, and a non-owned one keeps the latent rule
// under every lever (SkipsBody): only the player's own is recorded, so
// nothing else carries a skipped body's item. Inside another player's acting
// scope the self is that player's figure by now (step (0) of OnCall), never
// 0x14, so the fragment's grant crosses once, from the acting seat
enum class RecordRow : std::uint8_t
{
  None,
  RefState,
  Standing,
  PlayerOwn
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
  { "Actor", "SetRelationshipRank", RecordRow::Standing },
  { "Actor", "SetRace", RecordRow::PlayerOwn },
  { "Actor", "AddSpell", RecordRow::PlayerOwn },
  { "Actor", "RemoveSpell", RecordRow::PlayerOwn },
  { "Actor", "AddShout", RecordRow::PlayerOwn },
  { "Actor", "RemoveShout", RecordRow::PlayerOwn },
  { "Actor", "EquipItem", RecordRow::PlayerOwn },
  { "Actor", "ChangeHeadPart", RecordRow::PlayerOwn },
  { "ObjectReference", "AddItem", RecordRow::PlayerOwn },
  { "ObjectReference", "RemoveItem", RecordRow::PlayerOwn }
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

// thuum ADR-068 D-W2 A (the trigger's own calls from its firing seat; the
// plan's section 7.2(a)): the look-only rows, which cross only as a trap
// family trigger's look (OnFamilyCall) and are counted " (local)" and never
// queued anywhere else, so a hosted world script's sound or noise stays
// local as today. Sound.Play (FunctionsDump.txt: `Int Play(ObjectReference
// akSource)`, not latent; NativeSubject::Arg0Method, its subject the source,
// its call's self the Sound), and ObjectReference.CreateDetectionEvent
// (`CreateDetectionEvent(Actor akOwner, Int aiSoundLevel)`, not latent; its
// subject and self the reference it is called on, the trigger: SoundTrap's
// [active] onBeginState calls it on self with lastTriggerRef as akOwner, op
// 23 of USSEP's copy by lab/pex.py), carried so a guest's bone alarm still
// wakes the NPCs its cell's host simulates. Every other look row is a row of
// its own kind: the action rows, and kTrapRows' SetMotionType and
// ApplyHavokImpulse
constexpr std::pair<const char*, const char*> kLookOnlyRows[] = {
  { "Sound", "Play" },
  { "ObjectReference", "CreateDetectionEvent" }
};

// the Undead FX build (thuum ADR-060's amendment of 2026-11-12; the
// reanimate-fx memo's D6 B and D3 B): an actor's look rows, the action rows
// a script makes on an actor, which its simulator's engine plays and every
// other seat plays as the call (ADR-068 decision 1's ACTION, "a shader";
// decision 6, an actor's scripts are its simulator's). Undead FX's decay and
// eyes: EffectShader.Play and Stop (FunctionsDump.txt `Play(ObjectReference,
// Float)`, `Stop(ObjectReference)`), VisualEffect.Play and Stop
// (`Play(ObjectReference, Float, ObjectReference)`, `Stop(ObjectReference)`),
// each NativeSubject::Arg0Method, its subject the first argument and its
// call's self the effect form, and Actor.SetEyeTexture (`SetEyeTexture(
// TextureSet akNewTexture)`); vanilla ReanimateAshPile's TurnToAsh: the
// shader, Actor.SetAlpha (`SetAlpha(Float afTargetAlpha, Bool abFade)`),
// SetCriticalStage (`SetCriticalStage(Int aiStage)`) and AttachAshPile
// (`AttachAshPile(Form akAshPileBase)`); none latent. On a game-file actor
// this seat simulates, each crosses in the batch as kind "action", its self
// the effect form for an Arg0Method row (QueueLook), and the server relays
// it to the actor's listeners. On a world object, on this seat's own player
// or on an actor at or above 0xff000000 (a figure, what a seat places) it is
// counted " (local)" and runs here alone, as before the rows: the server's
// table gives these rows to an actor's simulator alone (wire-rules natives,
// Owner::Simulator), and skymp5-client reads an Arg0Method row's epoch from
// its self, which names no cell (scriptCallService's subjectOf names
// Sound.Play's source alone), so a world object's look would be refused
constexpr std::pair<const char*, const char*> kActorLookRows[] = {
  { "EffectShader", "Play" },      { "EffectShader", "Stop" },
  { "VisualEffect", "Play" },      { "VisualEffect", "Stop" },
  { "Actor", "SetEyeTexture" },    { "Actor", "SetAlpha" },
  { "Actor", "SetCriticalStage" }, { "Actor", "AttachAshPile" }
};

// thuum ADR-068 rung 5 (the acting scope, ActingScope.h; docs/research/
// rung5-quests-2026-11-09.md 1(c)): the local row whose result a scoped
// stack reads as the acting player's figure (Game.GetPlayer, the Local
// branch's swapReturn), and the three everyone rows whose call inside a
// scoped stack carries the scope to the stage or the word it sets (read 7's
// propagation, PropagateScope)
constexpr std::pair<const char*, const char*> kPlayerRow{ "Game",
                                                          "GetPlayer" };

enum class QuestRow : std::uint8_t
{
  None,
  Stage,
  Start,
  Stop
};

struct QuestRowName
{
  const char* cls;
  const char* fn;
  QuestRow quest;
};

constexpr QuestRowName kQuestRows[] = { { "Quest", "SetCurrentStageID",
                                          QuestRow::Stage },
                                        { "Quest", "Start", QuestRow::Start },
                                        { "Quest", "Stop", QuestRow::Stop } };

// PQ-3 (the plan's section 8(a)): the everyone row a scoped stack never runs
// on a player's own global (ActingScope::PlayerGlobals)
constexpr std::pair<const char*, const char*> kGlobalSetRow{ "GlobalVariable",
                                                             "SetValue" };

// thuum train 52, Fix 2 (the plan's section 2; ADR-068 decision 6, D-S2 B,
// D-H1 B): the row whose call inside this seat's own player's effect may
// place a local helper, PlaceAtMe (FunctionsDump.txt: `ObjectReference
// PlaceAtMe(Form akFormToPlace, Int aiCount, Bool abForcePersist, Bool
// abInitiallyDisabled)`, not latent), and the row whose call on a helper
// lets it go, Delete
constexpr std::pair<const char*, const char*> kPlaceAtMeRow{ "ObjectReference",
                                                             "PlaceAtMe" };
constexpr std::pair<const char*, const char*> kDeleteRow{ "ObjectReference",
                                                          "Delete" };

// thuum train 52, Fix 1 (D-R1 C's leveled gap; the plan's section 1.3): the
// row whose call of a leveled list on the seat's own player is parked until
// its AddItemFunctor lands the roll, AddItem (FunctionsDump.txt: `AddItem(Form
// akItemToAdd, Int aiCount, Bool abSilent)`, latent)
constexpr std::pair<const char*, const char*> kAddItemRow{ "ObjectReference",
                                                           "AddItem" };

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
  // other row: a RefState row's owner is OwnedForRecord's, and the lever's
  // `recorded` skips its latent body on a non-owner; a standing row's owner
  // is OwnedHere's and its body is never skipped (the fresh review, note 1)
  RecordRow record = RecordRow::None;
  // one of kAliasRows (rung 4): batched on every seat (OnAliasCall)
  bool alias = false;
  // the row's place among kTrapRows (D-S3), None for every other row
  TrapRow trap = TrapRow::None;
  // D-W2 A: a look row (an action row, SetMotionType, ApplyHavokImpulse, a
  // look-only row), and one of kLookOnlyRows
  bool look = false;
  bool lookOnly = false;
  // the Undead FX build: one of kActorLookRows, an actor's look
  bool actorLook = false;
  // rung 5: Game.GetPlayer's row (kPlayerRow), and the row's place among
  // kQuestRows, None for every other row
  bool getPlayer = false;
  QuestRow quest = QuestRow::None;
  // PQ-3: GlobalVariable.SetValue's row (kGlobalSetRow)
  bool globalSet = false;
  // train 52's Fix 2: PlaceAtMe's row (kPlaceAtMeRow) and Delete's
  // (kDeleteRow)
  bool placeAtMe = false;
  bool deletes = false;
  // train 52's Fix 1: AddItem's row (kAddItemRow)
  bool addItem = false;
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
// an animation row on an actor, an actor's look row on anything but a
// game-file actor, the Undead FX build's); " (unrelayable)" an owned action
// row with an array parameter; " (unresolved)" a subject that names no
// reference, or an alias row's self that names no alias of a quest (rung 4);
// " (frame mismatch)" a dispatch whose top frame is not the native's own, or
// whose arguments' page cannot be found (H-S1 failing), or a followed body's
// motion call whose frame does not hold the row's declared shape; " (skipped)"
// a non-owned state row whose body the lever skipped (scriptNonOwnerSkip
// nonLatent, or recorded since rung 4: a latent record row's too); " (trap)"
// a trap family reference's call, every seat's own (ADR-068 D-S3 point 1: a
// state row run here, an action row run under the script action's mark,
// neither queued); " (followed)" a script's SetMotionType or
// ApplyHavokImpulse on a trap body another seat streams, run neutralized
// (D-S3 point 4); " (look)" a trap family trigger's own call on itself in
// its firing window, queued as kind "look" and run here (D-W2 A); " (acting)"
// a call the acting scope changed (rung 5, ActingScope.h: a self or an
// argument naming the player's own actor swapped for the acting player's
// figure, beside the call's own kind; a call whose self names the player
// skipped, the figure absent here, alone; Game.GetPlayer's result swapped in
// place of its " (local)", or a player's own global's SetValue skipped in
// place of its self's count, PQ-3); " (effect)" a PlaceAtMe on a self this
// seat does not own, run inside this seat's own player's effect, its result
// this seat's local helper (train 52's Fix 2, clause 1), or an actor's look
// row on an actor this seat does not simulate, run inside an effect this
// seat's own player cast and queued as this seat's (the Undead FX build,
// D3 B); " (helper)" a call on such a helper, run here and crossing nowhere
// (clause 2); no suffix, an owned self's state row, run here and counted,
// and since rung 3 touched (QueueTouch: the script record's trigger).
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
  kCountLook,
  kCountActing,
  kCountEffect,
  kCountHelper,
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
                                                    " (followed)",
                                                    " (look)",
                                                    " (acting)",
                                                    " (effect)",
                                                    " (helper)" };

std::array<std::array<std::atomic<std::uint32_t>, kCountKinds>, kRowCount>
  g_counts{};

// rung 5: the acting scope's swaps in the frames of natives the table does
// not hold (a PlayerRef property is a 0x14 self on any native: the plan's
// finding 5), raised at the flush as one row, "untabled native (acting)"
std::atomic<std::uint32_t> g_actingUntabled{ 0 };

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

// train 52's Fix 1: one base a leveled roll put in the seat's own player, and
// how many (the wire's ItemDelta, positive here; wire-schema ScriptCall's
// `landed`)
struct Landing
{
  std::uint32_t formId = 0;
  std::int32_t count = 0;
};

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
  // D-W2 A's look (OnFamilyCall), raised as kind "look"
  bool look = false;
  // train 52's Fix 1: a parked leveled AddItem's roll as its AddItemFunctor
  // landed it, each base once (empty when the roll landed nothing), raised
  // as `landed`; none on every other call
  std::optional<std::vector<Landing>> landed;
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
// D-W2 A's switch (setTriggerLook, PuppetApi.cpp; ScriptNatives.h), off by
// default
std::atomic<bool> g_triggerLook{ false };

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
// 3's touch (an owned state row's call, its args empty), "look" for D-W2's
// look (a trap family trigger's own call on itself from its firing seat);
// alias on rung 4's alias rows alone (ForceRefTo, Clear), absent from every
// other call; since train 52's Fix 1 landed, [{ formId, count }], on a parked
// leveled AddItem on the seat's own player alone, absent from every other
// call
Napi::Object CallToJs(Napi::Env env, const QueuedCall& call)
{
  const NativeRow& native = kRows[call.row];
  auto obj = Napi::Object::New(env);
  AddObjProperty(&obj, "cls", native.cls);
  AddObjProperty(&obj, "fn", native.fn);
  AddObjProperty(&obj, "kind",
                 call.touch ? "touch"
                            : (call.look ? "look" : KindName(native.kind)));
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
  if (call.landed) {
    auto landed = Napi::Array::New(env, call.landed->size());
    for (uint32_t i = 0; i < static_cast<uint32_t>(call.landed->size()); ++i) {
      auto landing = Napi::Object::New(env);
      AddObjProperty(&landing, "formId", (*call.landed)[i].formId);
      AddObjProperty(&landing, "count",
                     static_cast<int>((*call.landed)[i].count));
      landed.Set(i, landing);
    }
    obj.Set("landed", landed);
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
// skipped. A standing row's body is never skipped until x-standing v1 decides
// W3 (rung 4's fresh review, note 1): a guest's dialogue or stage fragment on
// an NPC another seat simulates runs on the guest alone (the relayed stage's
// fragment is suppressed on the owner), so the guest's own copy is the only
// one, and the owner's record overwrites it where one exists. A latent
// player's own row (AddItem and RemoveItem, D-R1 C) is recorded on the
// seat's own player alone, so recorded keeps the latent rule for it: no
// record carries a container's or an NPC's item, or a figure's (the seven
// non-latent own rows are skipped as before)
bool SkipsBody(const RowInfo& info)
{
  if (info.record == RecordRow::Standing) {
    return false;
  }
  switch (g_nonOwnerSkip) {
    case NonOwnerSkip::NonLatent:
      return !info.latent;
    case NonOwnerSkip::Recorded:
      return !info.latent ||
        (info.record != RecordRow::None &&
         info.record != RecordRow::PlayerOwn);
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
using LineSlots = std::array<std::atomic<std::uint64_t>, kSkipLinesPerRow>;
std::array<LineSlots, kRowCount> g_skipLines{};
// train 52's Fix 2: the effect rule's lines, clause 1's placement and clause
// 2's call on a helper, on the same slots and bound
std::array<LineSlots, kRowCount> g_effectLines{};
std::array<LineSlots, kRowCount> g_helperLines{};

// true when the self takes a free slot of its row's window, its line due;
// false for a self already logged this window or past the bound
bool SelfLineDue(LineSlots& slots, std::uint32_t selfId)
{
  const std::uint64_t key = kSlotUsed | selfId;
  for (auto& slot : slots) {
    std::uint64_t seen = slot.load(std::memory_order_relaxed);
    if (seen == 0 &&
        slot.compare_exchange_strong(seen, key, std::memory_order_relaxed)) {
      return true;
    }
    if (seen == key) {
      return false;
    }
  }
  return false;
}

// every slot freed at the census's flush
void FreeLineSlots(LineSlots& slots)
{
  for (auto& slot : slots) {
    slot.store(0, std::memory_order_relaxed);
  }
}

void LogSkipped(std::size_t row, std::uint32_t selfId)
{
  if (SelfLineDue(g_skipLines[row], selfId)) {
    logger::info("[verb:scripts] {}.{} on {:x}: skipped, not hosted here "
                 "({})",
                 kRows[row].cls, kRows[row].fn, selfId,
                 LeverName(g_nonOwnerSkip));
  }
}

// thuum train 52, Fix 2 (the plan's section 2.3; ADR-068 decision 6, "the
// player's own (0x14, the player's effects, the player-alias quests) are the
// player's seat's", and D-S2 B, one owner for consequences, cosmetic or
// mechanism copies anywhere): the references a PlaceAtMe placed inside this
// seat's own player's effect, each this seat's local helper. A helper is R3
// (client-local, declared here): any tabled row on it runs here, counted
// " (helper)", and crosses nowhere, and every seat that runs the effect
// makes its own. A helper is only an activator, a static or a light (D-H1 B:
// the census's helper kinds that simulate nothing, docs/private/
// apocalypse-census-spells.tsv: ACTI 10, STAT 7, LIGH 2 of the 19 helper
// spells; FURN and CONT out with every actor, item, explosion and hazard).
// ActingScope.h's idiom: one mutex, the size in an atomic for the read with
// no lock; game-free (the caller reads a reference's base), so a later
// harness takes it whole. A helper holds while its reference's base is the
// base recorded for it, so a recycled dynamic id never inherits it, and is
// let go at a Delete row's call on it, kHelperMs after its placement, past
// kMaxHelpers (the oldest first) and at a game load (ClearAtLoad)
constexpr std::uint64_t kHelperMs = 600'000;
constexpr std::size_t kMaxHelpers = 256;

namespace Helpers {
struct Entry
{
  std::uint32_t base = 0;
  std::uint64_t createdMs = 0;
};

struct
{
  std::unordered_map<std::uint32_t, Entry> entries;
  std::mutex m;
  std::atomic<std::size_t> size{ 0 };
} g_table;

bool Any()
{
  return g_table.size.load(std::memory_order_relaxed) != 0;
}

void Add(std::uint32_t id, std::uint32_t base, std::uint64_t now)
{
  std::lock_guard<std::mutex> lock(g_table.m);
  if (g_table.entries.count(id) == 0 &&
      g_table.entries.size() >= kMaxHelpers) {
    auto oldest = g_table.entries.begin();
    for (auto it = g_table.entries.begin(); it != g_table.entries.end();
         ++it) {
      if (it->second.createdMs < oldest->second.createdMs) {
        oldest = it;
      }
    }
    g_table.entries.erase(oldest);
  }
  g_table.entries[id] = Entry{ base, now };
  g_table.size.store(g_table.entries.size());
}

bool Is(std::uint32_t id, std::uint32_t base, std::uint64_t now)
{
  if (!Any()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_table.m);
  const auto it = g_table.entries.find(id);
  return it != g_table.entries.end() && it->second.base == base &&
    now < it->second.createdMs + kHelperMs;
}

void Drop(std::uint32_t id)
{
  std::lock_guard<std::mutex> lock(g_table.m);
  g_table.entries.erase(id);
  g_table.size.store(g_table.entries.size());
}

void Sweep(std::uint64_t now)
{
  if (!Any()) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_table.m);
  for (auto it = g_table.entries.begin(); it != g_table.entries.end();) {
    if (now >= it->second.createdMs + kHelperMs) {
      it = g_table.entries.erase(it);
    } else {
      ++it;
    }
  }
  g_table.size.store(g_table.entries.size());
}

void Clear()
{
  std::lock_guard<std::mutex> lock(g_table.m);
  g_table.entries.clear();
  g_table.size.store(0);
}
}

// the effect rule's switch (scriptEffectHelpers, ClientSetting's; thuum
// .claude/rules/native.md's kill switch), read once at kDataLoaded before
// the replacement is installed and never written after; on unless false
bool g_effectHelpers = true;

// clause 2's test: a reference the table holds as a helper of its present
// base; one atomic load while the table is empty (rule 9)
bool IsHelper(const RE::TESObjectREFR& ref)
{
  if (!Helpers::Any()) {
    return false;
  }
  const RE::TESBoundObject* base = ref.GetBaseObject();
  return base && Helpers::Is(ref.formID, base->formID, NowMs());
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
  // rung 5's acting scope (ActingScope.h): Game.GetPlayer's result written
  // over with the scope's figure once the original answers kCompleted
  // (SwapReturn), the scope beside it, and, when its line is due, the
  // calling script's names read before the original
  bool swapReturn = false;
  ActingScope::Scope scope{};
  bool line = false;
  std::string caller;
  std::string handler;
  // train 52's Fix 2: a PlaceAtMe whose result is this seat's local helper,
  // recorded once the original answers kCompleted (NoteHelper)
  bool placeHelper = false;
  // train 52's Fix 1: a leveled AddItem parked by its stack id, let go when
  // the original answers anything but kInProgress (no functor queued)
  bool parked = false;
};

// one call built from its frame (the shared shape's Call): its self (an
// alias row's quest, beside the alias's id), its arguments read through the
// engine's accessor with the frame's page (found here when the subject's
// read did not need it) and its caller; nothing, counted " (frame
// mismatch)", when the arguments' page cannot be found
std::optional<QueuedCall> CallFromFrame(
  std::size_t row, const RowInfo& info, const RE::BSScript::Stack* stack,
  const RE::BSScript::StackFrame* top, std::uint32_t page, std::uint32_t self,
  std::optional<std::uint16_t> alias = std::nullopt, bool look = false)
{
  QueuedCall call;
  call.row = row;
  call.self = self;
  call.alias = alias;
  call.look = look;
  call.latent = info.latent;
  call.stackId = stack->stackID;
  call.argCount = info.params;
  if (info.params > 0) {
    if (page == kNoPage) {
      page = top->GetPageForFrame();
    }
    if (page == kNoPage) {
      Count(row, kCountFrameMismatch);
      return std::nullopt;
    }
    for (std::uint32_t i = 0; i < info.params; ++i) {
      call.args[i] =
        ReadArg(top->GetStackFrameVariable(i, page), info.types[i]);
    }
  }
  NameCaller(top->previousFrame, call.caller, call.handler);
  const RE::TESObjectREFR* callerRef = CallerRefOf(top->previousFrame);
  call.callerSelf = callerRef ? callerRef->formID : 0;
  return call;
}

// one call queued from its frame into this frame's batch (CallFromFrame's);
// false when it cannot be built
bool QueueFromFrame(std::size_t row, const RowInfo& info,
                    const RE::BSScript::Stack* stack,
                    const RE::BSScript::StackFrame* top, std::uint32_t page,
                    std::uint32_t self,
                    std::optional<std::uint16_t> alias = std::nullopt,
                    bool look = false)
{
  std::optional<QueuedCall> call =
    CallFromFrame(row, info, stack, top, page, self, alias, look);
  if (!call) {
    return false;
  }
  QueueCall(std::move(*call));
  return true;
}

// thuum train 52, Fix 1 (D-R1 C's leveled gap; the plan's sections 1.2 and
// 1.3: park, note, close). A script's AddItem of a leveled list on the
// seat's own player (OnCall's step (e)) is parked here by its stack id
// instead of queued: the native answers kInProgress with no container event,
// and 3.0 to 3.4 ms later SkyrimScript::AddItemFunctor's operator() runs on a
// worker thread with the call's stack id (DelayFunctor.h:43, stackID, equal
// to the call's Stack::stackID six of six) and lands the roll inside it as
// container events (the trace x-additem-functor 20261110-151634,
// papyrus-events.md on r5b-docs :654-697). The functor's window
// (ScriptNatives::FunctorWindow, Hooks.cpp's thunk) notes each add into the
// call's self onto the parked call (NoteContainerChange), and its close
// queues the call into the ordinary batch with what landed: each base once,
// its count summed (saturating at the engine's int32), at most kMaxLanded
// (the wire's cap::SCRIPT_CALL_LANDED); a base past it, or one the wire
// cannot carry (at or above 0xff000000), is counted and never sent. A stack
// waits on its latent call, so one stack id names one parked call. The
// table: its own mutex, its size in an atomic for the lock-free empty test,
// at most kMaxParked (the oldest let go with a line), each let go with a
// line kParkMs after its park when no functor ran (a killed stack, a load
// mid-wait), swept from Tick and cleared at a load (ClearAtLoad). The park
// mutex is released before QueueCall takes the batch mutex, no engine
// function is called under either, and the batch's flush is scheduled
// through AddTickTask, whose queue takes its own lock
// (viet/include/TaskQueue.h:18-22), so the functor's worker thread may queue.
// HYPOTHESIS H-LR1: a list that rolls several entries (LVLF use all, or each
// item in count) lands every one inside the functor (the trace read
// one-entry rolls); H-LR4: the functor runs within kParkMs of its call while
// a menu is open (60 s, not the trace's milliseconds, so a VM held back by a
// menu does not expire a real reward; UNMEASURED)
constexpr std::uint64_t kParkMs = 60'000;
constexpr std::size_t kMaxParked = 64;
constexpr std::size_t kMaxLanded = 255;

struct Parked
{
  QueuedCall call;
  std::vector<Landing> landed;
  // bases past kMaxLanded, or at or above 0xff000000: noted, never sent
  std::uint32_t overflow = 0;
  std::uint64_t parkedMs = 0;
};

struct
{
  std::unordered_map<RE::VMStackID, Parked> calls;
  std::mutex m;
  std::atomic<std::size_t> size{ 0 };
} g_park;

// the window open on this thread: the stack of the AddItemFunctor whose
// operator() runs here, 0 outside one (ScriptNatives::FunctorWindow)
thread_local RE::VMStackID t_functorStack = 0;

// the switch (scriptLeveledLandings, ClientSetting's; thuum
// .claude/rules/native.md's kill switch), read once at kDataLoaded before
// the replacement is installed and never written after; on unless false.
// Off, nothing is parked: a leveled AddItem on the seat's own player crosses
// at its call as before and the server refuses it E_SCRIPTCALL_LEVELED (the
// lab's A/B), the functor's window opening and noting nothing
bool g_leveledLandings = true;

// the slot's read-back, once (ReadSlotOnce)
std::atomic<bool> g_slotRead{ false };

// the drop and expiry lines, one a minute, each with the count since the last
constexpr std::uint64_t kDropLineMs = 60'000;
std::atomic<std::uint64_t> g_dropLineMs{ 0 };
std::atomic<std::uint32_t> g_dropsSince{ 0 };
// the close lines: the first kCloseLines, then every hundredth
constexpr std::uint64_t kCloseLines = 64;
std::atomic<std::uint64_t> g_closes{ 0 };

// a parked call as its lines name it: "<Class>.<Name> <list> on <self> (from
// <caller>.<handler>), stack <id>"
std::string ParkedName(const QueuedCall& call)
{
  const std::uint32_t list =
    call.argCount > 0 && call.args[0].kind == CallArg::Kind::Form
    ? call.args[0].formId
    : 0;
  return fmt::format("{}.{} {:x} on {:x} (from {}.{}), stack {}",
                     kRows[call.row].cls, kRows[call.row].fn, list, call.self,
                     call.caller, call.handler, call.stackId);
}

// a parked call let go with nothing reported, the line one a minute with the
// count of such calls since the last line
void DropLine(const QueuedCall& call, const char* why)
{
  g_dropsSince.fetch_add(1, std::memory_order_relaxed);
  const std::uint64_t now = NowMs();
  std::uint64_t last = g_dropLineMs.load(std::memory_order_relaxed);
  if (last != 0 && now - last < kDropLineMs) {
    return;
  }
  if (!g_dropLineMs.compare_exchange_strong(last, now)) {
    return;
  }
  logger::info("[verb:scripts] own {}: {}, nothing reported ({} such since "
               "the last line)",
               ParkedName(call), why, g_dropsSince.exchange(0));
}

// H-LR2 (no plugin rewrites slot 01 after the thunk's write; SKSE took slot 1
// of the cast handlers over the fork's own write, Hooks.cpp's note on
// InstallCastAnswerSink): the slot read back once, at the first park, and a
// warn line when it no longer holds the thunk, so a later write is never
// silent; each parked call then expires with its line, and the server
// refuses a leveled call that carries no landing as before
void ReadSlotOnce()
{
  bool expected = false;
  if (!g_slotRead.compare_exchange_strong(expected, true)) {
    return;
  }
  if (!Hooks::AddItemFunctorSlotHolds()) {
    logger::warn("[verb:scripts] AddItemFunctor's operator() slot 01 no "
                 "longer holds the platform's thunk (a later write, H-LR2): a "
                 "parked leveled AddItem expires unreported");
  }
}

// a leveled AddItem parked by its stack id; a call already parked on the
// stack, or the oldest past kMaxParked, let go with its line
void Park(RE::VMStackID stackId, QueuedCall&& call)
{
  ReadSlotOnce();
  std::optional<QueuedCall> displaced;
  const char* why = "";
  {
    std::lock_guard<std::mutex> lock(g_park.m);
    auto it = g_park.calls.find(stackId);
    if (it != g_park.calls.end()) {
      displaced = std::move(it->second.call);
      why = "a later call on its stack parked over it";
      g_park.calls.erase(it);
    } else if (g_park.calls.size() >= kMaxParked) {
      auto oldest = g_park.calls.begin();
      for (auto jt = g_park.calls.begin(); jt != g_park.calls.end(); ++jt) {
        if (jt->second.parkedMs < oldest->second.parkedMs) {
          oldest = jt;
        }
      }
      displaced = std::move(oldest->second.call);
      why = "the park full, the oldest let go";
      g_park.calls.erase(oldest);
    }
    Parked& parked = g_park.calls[stackId];
    parked.call = std::move(call);
    parked.parkedMs = NowMs();
    g_park.size.store(g_park.calls.size());
  }
  if (displaced) {
    DropLine(*displaced, why);
  }
}

// the parked call of a stack taken out; nothing for a stack with none, one
// atomic load while the park is empty
std::optional<Parked> TakeParked(RE::VMStackID stackId)
{
  if (g_park.size.load(std::memory_order_relaxed) == 0) {
    return std::nullopt;
  }
  std::lock_guard<std::mutex> lock(g_park.m);
  const auto it = g_park.calls.find(stackId);
  if (it == g_park.calls.end()) {
    return std::nullopt;
  }
  std::optional<Parked> parked = std::move(it->second);
  g_park.calls.erase(it);
  g_park.size.store(g_park.calls.size());
  return parked;
}

// a window's close (ScriptNatives::FunctorWindow): its stack's parked call
// queued with its landing, the lock released first
void CloseParked(RE::VMStackID stackId)
{
  std::optional<Parked> parked = TakeParked(stackId);
  if (!parked) {
    return;
  }
  const std::uint64_t n = g_closes.fetch_add(1) + 1;
  if (n <= kCloseLines || n % 100 == 0) {
    const std::string overflow = parked->overflow == 0
      ? std::string()
      : fmt::format(" ({} more not sent: past the wire's {} or no "
                    "load-order form)",
                    parked->overflow, kMaxLanded);
    if (parked->landed.empty()) {
      logger::info("[verb:scripts] own {}: its roll landed nothing in its "
                   "AddItemFunctor, reported with an empty landing{}",
                   ParkedName(parked->call), overflow);
    } else {
      std::string bases;
      for (const Landing& landing : parked->landed) {
        bases += fmt::format("{}{:x} x{}", bases.empty() ? "" : ", ",
                             landing.formId, landing.count);
      }
      logger::info("[verb:scripts] own {}: its roll landed {} base(s) in its "
                   "AddItemFunctor ({}), reported with the call{}",
                   ParkedName(parked->call), parked->landed.size(), bases,
                   overflow);
    }
  }
  parked->call.landed = std::move(parked->landed);
  QueueCall(std::move(parked->call));
}

// a parked call let go with its line (OnCall's native answered without
// queueing its functor)
void DropParked(RE::VMStackID stackId, const char* why)
{
  std::optional<Parked> parked = TakeParked(stackId);
  if (parked) {
    DropLine(parked->call, why);
  }
}

// the parked calls past kParkMs let go with their lines, once a frame from
// Tick; one atomic load while the park is empty
void SweepParked(std::uint64_t now)
{
  if (g_park.size.load(std::memory_order_relaxed) == 0) {
    return;
  }
  std::vector<QueuedCall> expired;
  {
    std::lock_guard<std::mutex> lock(g_park.m);
    for (auto it = g_park.calls.begin(); it != g_park.calls.end();) {
      if (now >= it->second.parkedMs + kParkMs) {
        expired.push_back(std::move(it->second.call));
        it = g_park.calls.erase(it);
      } else {
        ++it;
      }
    }
    g_park.size.store(g_park.calls.size());
  }
  for (const QueuedCall& call : expired) {
    DropLine(call, "no AddItemFunctor ran within 60 s");
  }
}

void ClearParked()
{
  std::lock_guard<std::mutex> lock(g_park.m);
  g_park.calls.clear();
  g_park.size.store(0);
}

// a window's note (ScriptNatives::NoteContainerChange) onto its stack's
// parked call: a new item (no old container) in the call's self, a base and
// a positive count, folded by base
void NoteLanding(RE::VMStackID stackId, std::uint32_t oldContainer,
                 std::uint32_t newContainer, std::uint32_t base,
                 std::int32_t count)
{
  if (oldContainer != 0 || base == 0 || count <= 0 ||
      g_park.size.load(std::memory_order_relaxed) == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_park.m);
  const auto it = g_park.calls.find(stackId);
  if (it == g_park.calls.end() || newContainer != it->second.call.self) {
    return;
  }
  Parked& parked = it->second;
  if (base >= 0xff000000) {
    ++parked.overflow;
    return;
  }
  for (Landing& landing : parked.landed) {
    if (landing.formId == base) {
      landing.count = static_cast<std::int32_t>(
        std::min<std::int64_t>(std::int64_t{ landing.count } + count,
                               std::numeric_limits<std::int32_t>::max()));
      return;
    }
  }
  if (parked.landed.size() >= kMaxLanded) {
    ++parked.overflow;
    return;
  }
  parked.landed.push_back(Landing{ base, count });
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
// motion rows run neutralized instead, counted " (followed)".
//
// First, D-W2's look (thuum docs/DECISIONS.md ADR-068, "D-W2 A and D-C3 A",
// Eli 2026-11-07; docs/research/fifty-fixes-2026-11-07.md section 7.2(a)):
// under D-W1 a family trigger fires on its presser's owner alone, and that
// seat owns the trigger's own calls on itself during the firing (a Tripwire's
// SetMotionType on its rope, a plate's 'Down' and 'Up', its sound, a bone
// alarm's impulse and its noise), which cross from it and play on every
// other seat. With the switch on (setTriggerLook, the presser rule on too),
// a look row's call is a look when the subject's firing window is open
// (TrapFamily::Firing, noted by the presser rule's owned branch) and the
// calling script's self (CallerRefOf, the frame below the native's) is the
// subject, so a linked trap's own calls and another reference's never are:
// it is queued as kind "look" (its self the subject's form id, or the
// frame's own for an Arg0Method row, Sound.Play's Sound), counted " (look)",
// and still runs here as the script wrote it: an animation row under the
// script action's mark, a motion row unneutralized (a trigger is never
// Followed). A look that cannot be queued (an array parameter, a self that
// names no form, a frame mismatch: each counted) still runs here. A
// look-only row outside a look is counted " (local)" and never queued
Marks OnFamilyCall(std::size_t row, const RowInfo& info,
                   const RE::BSScript::Stack* stack,
                   const RE::BSScript::StackFrame* top, std::uint32_t page,
                   std::uint32_t self)
{
  Marks marks;
  const NativeRow& native = kRows[row];
  if (info.look && g_triggerLook.load(std::memory_order_relaxed) &&
      Frida::TriggerPresserRuleOn() &&
      TrapFamily::Firing::Open(self, NowMs())) {
    const RE::TESObjectREFR* callerRef = CallerRefOf(top->previousFrame);
    if (callerRef && callerRef->formID == self) {
      // the Sound's own form id for Sound.Play (EveryoneSelfId's read: a
      // form's id from its kind-0 handle, kept when the form table knows it)
      const std::uint32_t callSelf =
        native.subject == NativeSubject::Arg0Method ? EveryoneSelfId(top->self)
                                                    : self;
      marks.action = native.kind == NativeKind::Action && !info.lookOnly;
      if (!info.relayable) {
        Count(row, kCountUnrelayable);
        return marks;
      }
      if (callSelf == 0) {
        Count(row, kCountUnresolved);
        return marks;
      }
      if (!QueueFromFrame(row, info, stack, top, page, callSelf, std::nullopt,
                          true)) {
        return marks;
      }
      Count(row, kCountLook);
      static std::atomic<uint64_t> s_looks{ 0 };
      const auto n = ++s_looks;
      if (n <= 3 || n % 100 == 0) {
        logger::info("[verb:traps] {}.{} on {:x}: its firing seat's look, "
                     "queued (D-W2) ({} so far)",
                     native.cls, native.fn, self, n);
      }
      return marks;
    }
  }
  if (info.lookOnly) {
    Count(row, kCountLocal);
    return marks;
  }
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

// thuum docs/DECISIONS.md ADR-068 rung 5 (D-F1 C, D-Q3 A) and
// docs/research/rung5-quests-2026-11-09.md 1(c): the acting scope
// (ActingScope.h). A stage, a start or a stop another player's seat set, or
// a trigger entry another player's figure made, runs its scripts here for
// that player: on a stack scoped to the acting player, step (0) of OnCall
// below makes the player's own actor (0x14) in the native's frame that
// player's figure, Game.GetPlayer answers the figure (the return swap), and
// a stage, a start or a stop the stack sets carries the scope on (read 7's
// propagation). A scoped call is then judged as any call on the figure: an
// animation row on it local, a state row on it not hosted (a figure is never
// in the hosted set), so a relayed fragment's player-targeted effects land
// on the figure, cosmetic, or on None when it is absent here, never on this
// seat's own player; the acting player's own seat runs that fragment
// natively, and its own channels carry the effects (the plan's rule table)

// the player's own actor in a value: a script object whose handle is the
// plain form 0x14's (the encoding HYPOTHESIS above)
bool IsPlayersOwn(const RE::BSScript::Variable& v)
{
  const RE::BSScript::Object* object = ObjectOf(v);
  if (!object) {
    return false;
  }
  const RE::VMHandle handle = object->GetHandle();
  return IsPlainFormHandle(handle) && FormIdOfHandle(handle) == 0x14;
}

// the acting player's figure as this seat holds it, null when it is absent
// here (its local id 0, or an id the form table names no actor for)
RE::Actor* FigureOf(const ActingScope::Player& player)
{
  return player.figure != 0 ? RE::TESForm::LookupByID<RE::Actor>(player.figure)
                            : nullptr;
}

// the figure packed over a value: CommonLibSSE-NG's PackHandle
// (src/RE/P/PackUnpack.cpp:48-80) with ActorCharacter's VM type, the figure's
// bound Actor object found, or created and bound, through the handle policy,
// as a native's own object result is packed; None for a null figure
// (PackHandle's SetNone, first). HYPOTHESIS H-AP4: a 0x14 self or argument
// rewritten so dispatches on the figure (x-acting-player v1: the census's
// " (acting)" row, and no "Cannot call ... on a None object" for a figure
// that is present)
void PackFigure(RE::BSScript::Variable& v, RE::Actor* figure)
{
  RE::BSScript::PackHandle(&v, figure,
                           static_cast<RE::VMTypeID>(RE::Actor::FORMTYPE));
}

// the join's key: the stack's root frame, its first, read down
// previousFrame (CommonLibSSE-NG include/RE/S/StackFrame.h:22) at most
// Stack::frames times (Stack.h:74; cut at kMaxRootFrames), and its self's
// form id by the handle's low 32 bits (FormIdOfHandle): a quest's fragment
// and its alias scripts carry the quest's id there (HYPOTHESIS H-AP1: a
// relayed stage's fragment stack roots at the QF script's Fragment_N with
// the quest's object as self, x-acting-frame v1; the alias half is H-AF3's
// encoding), a trigger's script the trigger's; 0 for a root with no object,
// or one whose handle is of a kind other than 0 (an inventory item's or an
// active effect's, ScriptState.cpp's H-R6), which no arm's subject names
// (rung 5's fresh review, note 9)
constexpr std::uint32_t kMaxRootFrames = 1024;

// the walk, one for both the join's key and D-Q4 C's root (MarkQuestCause):
// the stack's first frame, null for a stack with none
const RE::BSScript::StackFrame* RootFrameOf(const RE::BSScript::Stack* stack)
{
  const RE::BSScript::StackFrame* frame = stack->top;
  const std::uint32_t frames = std::min(stack->frames, kMaxRootFrames);
  for (std::uint32_t i = 1; frame && frame->previousFrame && i < frames; ++i) {
    frame = frame->previousFrame;
  }
  return frame;
}

// a frame's self's form id by its handle's low 32 bits, 0 as above
std::uint32_t SubjectOfFrame(const RE::BSScript::StackFrame* frame)
{
  const RE::BSScript::Object* object = frame ? ObjectOf(frame->self) : nullptr;
  if (!object) {
    return 0;
  }
  const RE::VMHandle handle = object->GetHandle();
  return (handle >> 48) == 0 ? FormIdOfHandle(handle) : 0;
}

std::uint32_t RootSubjectOf(const RE::BSScript::Stack* stack)
{
  return SubjectOfFrame(RootFrameOf(stack));
}

// step (0)'s scope: the stack's own once it joined one (kept for its life,
// ActingScope::kJoinedMs), else, inside an armed window, the arm's for the
// stack's root subject, joined at this call. Asked for every native the game
// calls, not only tabled ones (a PlayerRef property is a 0x14 self on any
// native: the plan's finding 5), one relaxed load while no scope exists
// (H-AP5). The platform's own call is never scoped: step (b)'s direct test
// first, its live test before a join (a latent call the platform dispatched
// roots at the very quest the client applies); the frame must be the
// native's own, step (c)'s test, before its frame is read
std::optional<ActingScope::Scope> ScopeOfCall(
  const RE::BSScript::IFunction* fn, const RE::BSScript::Stack* stack)
{
  if (!ActingScope::Any() || !stack) {
    return std::nullopt;
  }
  if (PlatformCall::Active() ||
      (g_nativeCallRequirements.stackId != 0 &&
       stack->stackID == g_nativeCallRequirements.stackId)) {
    return std::nullopt;
  }
  const RE::BSScript::StackFrame* top = stack->top;
  if (!top || top->owningFunction.get() != fn) {
    return std::nullopt;
  }
  if (auto scope = ActingScope::Of(stack->stackID)) {
    return scope;
  }
  if (!ActingScope::Armed() ||
      PlatformCall::IsLiveCallback(stack->callback.get())) {
    return std::nullopt;
  }
  const std::uint32_t root = RootSubjectOf(stack);
  const auto joined = ActingScope::Join(stack->stackID, root);
  if (!joined) {
    return std::nullopt;
  }
  if (ActingScope::LineDue(joined->scope.seq, ActingScope::Line::kJoin)) {
    logger::info("[verb:quests] stack {} rooted at {:x} joined acting "
                 "player {:x}'s scope ({} stack(s) joined by its arm)",
                 stack->stackID, root, joined->scope.player.serverId,
                 joined->joins);
  }
  return joined->scope;
}

// step (0)'s swaps, in the native's own frame, from which its body reads
// its self and its arguments (MarshallAndDispatch, CommonLibSSE-NG
// include/RE/N/NativeFunction.h:81-92): its self (StackFrame.h:27) and each
// argument, read through the engine's own accessor (Stack::
// GetStackFrameVariable, Address Library 104484) with the frame's page, that
// names the player's own actor is rewritten to the acting player's figure;
// with the figure absent here an argument becomes None (HYPOTHESIS H-AP6:
// the native's body takes a None argument as a script's None), and a self
// naming the player leaves the frame alone and asks for the call to be
// skipped (`skip`; rung 5's fresh review, FIX FIRST 2): a None self written
// here would reach the engine's own Call after the VM's None check ran, and
// what Call does with it is unread, so OnCall takes the lever's skip path
// instead (SetEmptyResult, the native's declared return's empty result,
// kCompleted; a latent native's skip rests on H-F2, as the lever's does).
// The count of values rewritten or skipped, its first lines per scope logged
std::uint32_t SwapPlayer(const RE::BSScript::IFunction* fn,
                         RE::BSScript::Stack* stack,
                         const ActingScope::Scope& scope, bool& skip)
{
  RE::BSScript::StackFrame* top = stack->top;
  RE::Actor* figure = nullptr;
  bool looked = false;
  const auto figureOnce = [&]() {
    if (!looked) {
      looked = true;
      figure = FigureOf(scope.player);
    }
    return figure;
  };
  std::uint32_t swapped = 0;
  if (IsPlayersOwn(top->self)) {
    if (!figureOnce()) {
      skip = true;
      if (ActingScope::LineDue(scope.seq, ActingScope::Line::kSwap)) {
        logger::info("[verb:quests] {}.{}: its self names the player, acting "
                     "player {:x}'s figure {:x} (absent here, skipped), "
                     "stack {}",
                     fn->GetObjectTypeName().c_str(), fn->GetName().c_str(),
                     scope.player.serverId, scope.player.figure,
                     stack->stackID);
      }
      return 1;
    }
    PackFigure(top->self, figure);
    ++swapped;
  }
  const std::uint32_t params = std::min(fn->GetParamCount(), kMaxArgs);
  const std::uint32_t page = params > 0 ? top->GetPageForFrame() : kNoPage;
  for (std::uint32_t i = 0; page != kNoPage && i < params; ++i) {
    RE::BSScript::Variable& arg = top->GetStackFrameVariable(i, page);
    if (IsPlayersOwn(arg)) {
      PackFigure(arg, figureOnce());
      ++swapped;
    }
  }
  if (swapped != 0 &&
      ActingScope::LineDue(scope.seq, ActingScope::Line::kSwap)) {
    logger::info("[verb:quests] {}.{}: {} value(s) naming the player swapped "
                 "for acting player {:x}'s figure {:x}{}, stack {}",
                 fn->GetObjectTypeName().c_str(), fn->GetName().c_str(),
                 swapped, scope.player.serverId, scope.player.figure,
                 figure ? "" : " (absent here, None)", stack->stackID);
  }
  return swapped;
}

// a request's stage or word as the lines name it
std::string StageText(std::int32_t stage)
{
  if (stage == ActingScope::kStart) {
    return "start";
  }
  if (stage == ActingScope::kStop) {
    return "stop";
  }
  return fmt::format("stage {}", stage);
}

// read 7's propagation: a Quest row called inside a scoped stack (a relayed
// fragment's chained SetStage, its Start or its Stop) requests the scope for
// the stage or the word it sets (chained), so the quest stage sink scopes
// that stage's fragment too and this seat's report of the stage names the
// scope's player (getActingPlayer, the C2S QuestStage's `acting`); a start
// or a stop arms the quest's scope at once, as setActingPlayer arms a
// relayed word. SetCurrentStageID's stage is its first argument, read
// through 104484 as the swaps read theirs; one outside the engine's u16
// requests nothing
void PropagateScope(std::size_t row, const RowInfo& info,
                    const RE::BSScript::Stack* stack,
                    const RE::BSScript::StackFrame* top, std::uint32_t quest,
                    const ActingScope::Scope& scope)
{
  if (quest == 0) {
    return;
  }
  std::int32_t stage = 0;
  switch (info.quest) {
    case QuestRow::Stage: {
      const std::uint32_t page =
        info.params > 0 ? top->GetPageForFrame() : kNoPage;
      if (page == kNoPage) {
        return;
      }
      const RE::BSScript::Variable& arg = top->GetStackFrameVariable(0, page);
      if (!arg.IsInt() || arg.GetSInt() < 0 || arg.GetSInt() > 0xFFFF) {
        return;
      }
      stage = arg.GetSInt();
      break;
    }
    case QuestRow::Start:
      stage = ActingScope::kStart;
      break;
    case QuestRow::Stop:
      stage = ActingScope::kStop;
      break;
    case QuestRow::None:
      return;
  }
  ActingScope::Request(quest, stage, scope.player, ActingScope::kChainedMs,
                       ActingScope::Origin::kChained);
  if (stage < 0) {
    ActingScope::Arm(quest, scope.player, ActingScope::kArmMs);
  }
  if (ActingScope::LineDue(scope.seq, ActingScope::Line::kChain)) {
    logger::info("[verb:quests] {}.{} on {:x} ({}) inside acting player "
                 "{:x}'s scope, stack {}: the scope requested for it "
                 "(chained)",
                 kRows[row].cls, kRows[row].fn, quest, StageText(stage),
                 scope.player.serverId, stack->stackID);
  }
}

// D-Q4 C (QuestCause.h): a Fragment_ root's self by its form type, the
// fragment script's attachment: a quest's own fragment, a topic info's, a
// scene's, a package's, a perk's (CommonLibSSE-NG include/RE/F/FormTypes.h:
// 216, 217, 219, 232 and 262); none for any other form or none
QuestCause::Self SelfKindOf(std::uint32_t subject)
{
  const RE::TESForm* form =
    subject != 0 ? RE::TESForm::LookupByID(subject) : nullptr;
  if (!form) {
    return QuestCause::Self::kNone;
  }
  switch (form->GetFormType()) {
    case RE::FormType::Quest:
      return QuestCause::Self::kQuest;
    case RE::FormType::Info:
      return QuestCause::Self::kInfo;
    case RE::FormType::Scene:
      return QuestCause::Self::kScene;
    case RE::FormType::Package:
      return QuestCause::Self::kPackage;
    case RE::FormType::Perk:
      return QuestCause::Self::kPerk;
    default:
      return QuestCause::Self::kNone;
  }
}

// D-Q4 C (QuestCause.h; the run-cause read's rule): a Quest row's call (the
// table's SetCurrentStageID, Start and Stop, kQuestRows) marked with its
// verdict for the pair it sets. The verdict is the stack's own once it joined
// a fragment's, else its root frame's class, read in one walk (RootFrameOf;
// the frame's script and function as NameCaller reads a caller's, its self's
// form type): a world root, a timer, the seat's own, or a quest's own
// fragment, which joins the quest's arm (the verdict of whoever set its
// stage, armed by FridaHooks.cpp's quest stage sink), so a seat-own chain
// stays the seat's and a world one the world's. A start or a stop is marked
// with the run word's verdict (a direct timer's the seat's own); a stage
// with the verdict it carries to its fragment (a timer's stage is the
// world's progress), and the quest's start beside it with the run word's
// verdict while the quest is not running (TESQuest::IsEnabled,
// CommonLibSSE-NG src/RE/T/TESQuest.cpp:42-45), since SetStage starts a
// stopped quest and that start is a word the calling stack made (a timer's
// the seat's own, the review of edf64dee's note 5). A Start on a running
// quest and a Stop on a stopped one change nothing and raise no event, so
// they mark nothing: an untaken mark would lend its cause to the quest's
// next engine start or stop and the stages riding it (the review's note 4).
// Both tests are QuestCause::WordMade's, the one rule the console's typed
// commands share (PuppetApi.cpp markQuestCause, train 52's Fix 3); a quest
// the form table holds no TESQuest for marks its word as before.
// SetCurrentStageID's stage is its first argument, read as PropagateScope
// reads it; one outside the engine's u16 marks nothing. Never the platform's
// own call: OnCall's step (b) returns before
void MarkQuestCause(std::size_t row, const RowInfo& info,
                    const RE::BSScript::Stack* stack,
                    const RE::BSScript::StackFrame* top, std::uint32_t quest)
{
  if (quest == 0) {
    return;
  }
  std::int32_t stage = 0;
  switch (info.quest) {
    case QuestRow::Stage: {
      const std::uint32_t page =
        info.params > 0 ? top->GetPageForFrame() : kNoPage;
      if (page == kNoPage) {
        return;
      }
      const RE::BSScript::Variable& arg = top->GetStackFrameVariable(0, page);
      if (!arg.IsInt() || arg.GetSInt() < 0 || arg.GetSInt() > 0xFFFF) {
        return;
      }
      stage = arg.GetSInt();
      break;
    }
    case QuestRow::Start:
      stage = QuestCause::kStart;
      break;
    case QuestRow::Stop:
      stage = QuestCause::kStop;
      break;
    case QuestRow::None:
      return;
  }
  const auto* questForm = RE::TESForm::LookupByID<RE::TESQuest>(quest);
  const bool running = questForm && questForm->IsEnabled();
  const bool startsEnabled = questForm && questForm->StartsEnabled();
  if (questForm && !QuestCause::WordMade(stage, running, startsEnabled)) {
    return;
  }
  bool runWorld = false;
  bool carriedWorld = false;
  bool carried = false;
  std::string root;
  if (const auto joined = QuestCause::Of(stack->stackID)) {
    runWorld = carriedWorld = joined->world;
    carried = true;
    root = joined->root;
  } else {
    const RE::BSScript::StackFrame* frame = RootFrameOf(stack);
    std::string script;
    std::string function;
    NameCaller(frame, script, function);
    if (!script.empty() || !function.empty()) {
      root = script + "." + function;
    }
    const std::uint32_t subject = SubjectOfFrame(frame);
    switch (QuestCause::Classify(script, function, SelfKindOf(subject))) {
      case QuestCause::Root::kWorld:
        runWorld = carriedWorld = true;
        break;
      case QuestCause::Root::kTimer:
        carriedWorld = true;
        break;
      case QuestCause::Root::kStageFragment: {
        const auto joinedNow = QuestCause::Join(stack->stackID, subject, root);
        runWorld = carriedWorld = joinedNow && joinedNow->world;
        carried = joinedNow.has_value();
        break;
      }
      case QuestCause::Root::kSeat:
      case QuestCause::Root::kUnknown:
        break;
    }
  }
  const bool world = stage < 0 ? runWorld : carriedWorld;
  QuestCause::Mark(quest, stage, { world, root });
  if (info.quest == QuestRow::Stage && questForm &&
      QuestCause::WordMade(QuestCause::kStart, running, startsEnabled)) {
    QuestCause::Mark(quest, QuestCause::kStart, { runWorld, root });
  }
  QuestCause::Count(world, carried);
  logger::trace("[verb:quest-stage] {}.{} on {:x} ({}): cause {} (root {}), "
                "stack {}",
                kRows[row].cls, kRows[row].fn, quest, StageText(stage),
                world ? "world" : "seat", root.empty() ? "none" : root,
                stack->stackID);
}

// train 52's Fix 2 (D-H1 B): a helper's base kind, an activator, a static or
// a light (CommonLibSSE-NG include/RE/F/FormTypes.h:164, :174 and :171), as
// the lines name it; null for every other form type
const char* HelperKindOf(RE::FormType type)
{
  switch (type) {
    case RE::FormType::Activator:
      return "ACTI";
    case RE::FormType::Static:
      return "STAT";
    case RE::FormType::Light:
      return "LIGH";
    default:
      return nullptr;
  }
}

// a row's first argument's form (PlaceAtMe's akFormToPlace, AddItem's
// akItemToAdd), read through the engine's accessor with the frame's page
// (found here when the subject's read did not need it, and kept in `page`
// for the caller's next read), the handle a plain form's (the encoding
// HYPOTHESIS above); null for a None, an argument that is no plain form, or
// a frame whose page cannot be found
const RE::TESForm* Arg0Form(const RowInfo& info,
                            const RE::BSScript::StackFrame* top,
                            std::uint32_t& page)
{
  if (info.params == 0) {
    return nullptr;
  }
  if (page == kNoPage) {
    page = top->GetPageForFrame();
  }
  if (page == kNoPage) {
    return nullptr;
  }
  const RE::BSScript::Object* object =
    ObjectOf(top->GetStackFrameVariable(0, page));
  if (!object) {
    return nullptr;
  }
  const RE::VMHandle handle = object->GetHandle();
  const std::uint32_t id = FormIdOfHandle(handle);
  return IsPlainFormHandle(handle) && id != 0 ? RE::TESForm::LookupByID(id)
                                              : nullptr;
}

// a PlaceAtMe's placed base (Arg0Form's) when it is a helper's kind; nothing
// for a None, a form of any other kind, or a frame whose page cannot be found
struct HelperBase
{
  std::uint32_t id = 0;
  const char* kind = nullptr;
};

std::optional<HelperBase> HelperBaseOfArg0(const RowInfo& info,
                                           const RE::BSScript::StackFrame* top,
                                           std::uint32_t page)
{
  const RE::TESForm* form = Arg0Form(info, top, page);
  const char* kind = form ? HelperKindOf(form->GetFormType()) : nullptr;
  if (!kind) {
    return std::nullopt;
  }
  return HelperBase{ form->formID, kind };
}

// clause 1's test (the plan's section 2.3, creation inside the player's
// effect), cheapest first: the placed base a helper's kind, then the stack's
// root frame's self an active effect this seat runs for its own player, its
// caster or its target (Frida::PlayersEffect, the Papyrus gate's own test,
// FridaHooks.cpp OnSendEventEnter; RootFrameOf's walk). Asked only for a
// PlaceAtMe the owner test refused (rule 9). HYPOTHESIS H-EF1: an effect
// event's stack roots at the ActiveMagicEffect script's handler with the
// effect's object as self, whose handle HandleIsType(142) answers true
// (x-effect-helper v1's line for WB_CrackleNew_Script.OnEffectStart reads it)
struct EffectPlacement
{
  HelperBase base;
  Frida::PlayersEffectRole role = Frida::PlayersEffectRole::kNone;
  const RE::BSScript::StackFrame* root = nullptr;
};

// the effect rule's one test, the helper's and the look's: the stack's root
// frame (RootFrameOf, left in `root`) and its self's role as an active
// effect this seat runs for its own player (Frida::PlayersEffect), kNone for
// a root that is no such effect's
Frida::PlayersEffectRole RootEffectRole(
  const RE::BSScript::Stack* stack, RE::BSScript::IObjectHandlePolicy* policy,
  const RE::BSScript::StackFrame*& root)
{
  root = RootFrameOf(stack);
  const RE::BSScript::Object* object = root ? ObjectOf(root->self) : nullptr;
  if (!object || !policy) {
    return Frida::PlayersEffectRole::kNone;
  }
  return Frida::PlayersEffect(policy, object->GetHandle());
}

std::optional<EffectPlacement> EffectPlacementOf(
  const RowInfo& info, const RE::BSScript::Stack* stack,
  const RE::BSScript::StackFrame* top, std::uint32_t page,
  RE::BSScript::IObjectHandlePolicy* policy)
{
  const std::optional<HelperBase> base = HelperBaseOfArg0(info, top, page);
  if (!base || !policy) {
    return std::nullopt;
  }
  const RE::BSScript::StackFrame* root = nullptr;
  const Frida::PlayersEffectRole role = RootEffectRole(stack, policy, root);
  if (role == Frida::PlayersEffectRole::kNone) {
    return std::nullopt;
  }
  return EffectPlacement{ *base, role, root };
}

// the Undead FX build: an actor's look row's call queued as kind "action"
// (OnCall's owned branch and the effect rule's), its self the subject, or
// for an Arg0Method row the method's own, the effect form (EveryoneSelfId's
// read, as a firing's look takes Sound.Play's Sound in OnFamilyCall), which
// the server relays it on and the receiver runs it on; false, counted, when
// the row cannot cross (an array parameter) or that self names no form, and
// when the frame's arguments cannot be read (CallFromFrame's count)
bool QueueLook(std::size_t row, const RowInfo& info,
               const RE::BSScript::Stack* stack,
               const RE::BSScript::StackFrame* top, std::uint32_t page,
               std::uint32_t subject)
{
  if (!info.relayable) {
    Count(row, kCountUnrelayable);
    return false;
  }
  const std::uint32_t self = kRows[row].subject == NativeSubject::Arg0Method
    ? EveryoneSelfId(top->self)
    : subject;
  if (self == 0) {
    Count(row, kCountUnresolved);
    return false;
  }
  return QueueFromFrame(row, info, stack, top, page, self);
}

// the seam's judgment of one dispatch, 6.2's order (rule 9: the cheapest
// test first). The body runs unless the lever skips it (marks.skip)
Marks OnCall(const RE::BSScript::IFunction* fn, RE::BSScript::Stack* stack)
{
  Marks marks;
  // (0) rung 5's acting scope (ScopeOfCall, SwapPlayer): on a stack scoped
  // to another player's request, every self or object argument naming the
  // player's own actor is the acting player's figure from here on, before
  // any judgment below reads the frame
  const std::optional<ActingScope::Scope> scope = ScopeOfCall(fn, stack);
  bool absentSelf = false;
  const std::uint32_t swapped =
    scope ? SwapPlayer(fn, stack, *scope, absentSelf) : 0;
  // a self naming the player with the figure absent here: the frame left
  // alone, the body skipped, the native's declared return's empty result
  // left in its place (the lever's path, SetEmptyResult; FIX FIRST 2)
  if (absentSelf) {
    marks.skip = true;
    marks.result = fn->GetReturnType().GetUnmangledRawType();
  }
  // (a) not a tabled native: nothing more, the only cost of every other
  // native in the game (step (0)'s one relaxed load outside a scope, and one
  // hash lookup)
  const auto it = g_rowOf.find(fn);
  if (it == g_rowOf.end()) {
    if (swapped != 0) {
      g_actingUntabled.fetch_add(1, std::memory_order_relaxed);
    }
    return marks;
  }
  const std::size_t row = it->second;
  const NativeRow& native = kRows[row];
  const RowInfo& info = g_rows[row];
  if (swapped != 0) {
    Count(row, kCountActing);
  }
  if (marks.skip) {
    return marks;
  }
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
  // a local row never crosses (Game.GetPlayer), except that inside an acting
  // scope (rung 5) GetPlayer answers the acting player's figure: its result
  // is written over once the original returns (marks.swapReturn,
  // SwapReturn), counted " (acting)"
  if (native.kind == NativeKind::Local) {
    if (scope && info.getPlayer) {
      marks.swapReturn = true;
      marks.scope = *scope;
      marks.line =
        ActingScope::LineDue(scope->seq, ActingScope::Line::kReturn);
      if (marks.line) {
        NameCaller(top->previousFrame, marks.caller, marks.handler);
      }
      Count(row, kCountActing);
      return marks;
    }
    Count(row, kCountLocal);
    return marks;
  }
  // an everyone row has no owner (D-F1 C): every seat runs it, its self
  // counted; an alias row's call is batched instead (rung 4, OnAliasCall).
  // Inside an acting scope a Quest row carries the scope on to the stage or
  // the word it sets (rung 5, PropagateScope)
  if (native.kind == NativeKind::Everyone) {
    if (info.alias) {
      OnAliasCall(row, info, stack, top);
      return marks;
    }
    const std::uint32_t selfId = EveryoneSelfId(top->self);
    if (scope && info.quest != QuestRow::None) {
      PropagateScope(row, info, stack, top, selfId, *scope);
    }
    // D-Q4 C: every Quest row's call marked with its cause for the pair it
    // sets (MarkQuestCause), inside an acting scope or not
    if (info.quest != QuestRow::None) {
      MarkQuestCause(row, info, stack, top, selfId);
    }
    // PQ-3 (the plan's section 8(a)): inside another player's acting scope
    // a player's own global (setPlayerGlobals) is never written here: the
    // body is skipped, the row's empty result returned (SetValue's None),
    // counted " (acting)"; the acting player's own seat runs the call
    // natively and its PlayerGlobals report carries the value
    if (scope && info.globalSet && ActingScope::PlayerGlobals::Is(selfId)) {
      marks.skip = true;
      marks.result = info.result;
      Count(row, kCountActing);
      if (ActingScope::LineDue(scope->seq, ActingScope::Line::kSkip)) {
        logger::info("[verb:quests] GlobalVariable.SetValue on {:x}, a "
                     "player's own global, skipped inside acting player "
                     "{:x}'s scope, stack {} (PQ-3)",
                     selfId, scope->player.serverId, stack->stackID);
      }
      return marks;
    }
    CountSelf(row, selfId);
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
  } else if ((native.subject == NativeSubject::Arg0 ||
              native.subject == NativeSubject::Arg0Method) &&
             info.params > 0) {
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
  // on an actor is local): every rung 2 action row is an animation row. An
  // actor's look row (kActorLookRows, the Undead FX build) is none: it goes
  // on to the owner below on a game-file actor other than this seat's own
  // player, the one subject its channel names, and stays here, counted
  // local, on anything else
  if (native.kind == NativeKind::Action && isActor && !info.actorLook) {
    Count(row, kCountLocal);
    return marks;
  }
  if (info.actorLook &&
      (!isActor || ref->formID == 0x14 || ref->formID >= 0xff000000)) {
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
  // D-W2 A's look-only rows cross as a trap family trigger's look alone
  // (OnFamilyCall): anywhere else counted local and never queued, so a hosted
  // world script's sound or noise stays local as before
  if (info.lookOnly) {
    Count(row, kCountLocal);
    return marks;
  }
  // train 52's Fix 2, clause 2 (the plan's section 2.3: a helper is its
  // creator's): a non-actor at or above 0xff000000 that this seat's own
  // player's effect placed (Helpers, above) is this seat's local helper, so
  // any tabled row on it runs here, counted " (helper)", and crosses
  // nowhere: never batched, never touched, never relayed. An action row on
  // one runs under the script action's mark for its extent, as a trap family
  // reference's does (OnFamilyCall), so the engine-level animation hooks
  // leave it too and nothing reports it. A PlaceAtMe on a helper of a
  // helper's base makes another helper (WB_CrackleNew_Script's TheShooter,
  // placed at TheBox); a Delete on one lets it go. Asked only for a
  // non-actor at or above 0xff000000 (where the players' figures and what
  // they place begin), one atomic load while the table is empty (rule 9)
  if (!isActor && ref->formID >= 0xff000000 && IsHelper(*ref)) {
    Count(row, kCountHelper);
    if (SelfLineDue(g_helperLines[row], ref->formID)) {
      logger::info("[verb:scripts] {}.{} on {:x}: a helper this seat's own "
                   "player's effect placed, run here and kept local (R3)",
                   native.cls, native.fn, ref->formID);
    }
    if (info.deletes) {
      Helpers::Drop(ref->formID);
    } else if (info.placeAtMe && HelperBaseOfArg0(info, top, page)) {
      marks.placeHelper = true;
    }
    marks.action = native.kind == NativeKind::Action;
    return marks;
  }
  // (d) the owner, a record row's by OwnedForRecord (rung 4). Not owned:
  // counted, and the body runs (an action row's jump, the pillar's
  // StartState0N from a guest's OnCellLoad, is wanted everywhere), unless
  // the lever skips a state row's body (6.11 item 3: a guest's lifecycle
  // handler performs the host's PlaceAtMe or Lock on its own copy, skipped
  // by nonLatent; its RemoveItem and MoveTo are latent, FunctionsDump.txt,
  // and wait for H-F1's read; its Disable, a latent record row, is skipped
  // by recorded, H-F2), the first skipped selves logged (LogSkipped)
  // a standing row's owner is the plain test, its NPC's simulator (rung 4's
  // fresh review, note 1: an NPC nobody simulates is nobody's to report, and
  // the cell host's report of one would be refused)
  const bool owned = info.record == RecordRow::RefState
    ? OwnedForRecord(ref, isActor)
    : OwnedHere(ref, isActor);
  if (!owned) {
    // train 52's Fix 2, clause 1 (the plan's section 2.3: creation inside
    // the player's effect; the class b-second-hand reds on, second-hand.md:
    // WB_CrackleNew_Script's OnEffectStart places TheBox at its target, c2's
    // figure, which no seat hosts, so the lever skipped it and RemoteCast had
    // no source): a PlaceAtMe of a helper's base on a stack whose root is an
    // active effect this seat runs for its own player, its caster or its
    // target, runs its body whatever its subject's owner, and the reference
    // it returns is this seat's local helper (marks.placeHelper, NoteHelper),
    // counted " (effect)". Nothing crosses (R3): a helper is never batched,
    // and the consequence that matters, the proc's hit, has its own channel
    // (the caster's OnHit, second-hand's claim). Everything else on another
    // owner's object keeps the judgment below: a PlaceAtMe of an NPC, an
    // item, a container, furniture, a door, an explosion or a hazard, and
    // every other row
    if (info.placeAtMe && g_effectHelpers) {
      if (const auto placed =
            EffectPlacementOf(info, stack, top, page, policy)) {
        marks.placeHelper = true;
        Count(row, kCountEffect);
        if (SelfLineDue(g_effectLines[row], ref->formID)) {
          std::string script;
          std::string function;
          NameCaller(placed->root, script, function);
          logger::info(
            "[verb:scripts] {}.{} on {:x}: run inside this seat's own "
            "player's effect ({}.{}, the player its {}), its helper {:x} "
            "({}) kept local here (ADR-068 decision 6, R3)",
            native.cls, native.fn, ref->formID, script, function,
            placed->role == Frida::PlayersEffectRole::kCaster ? "caster"
                                                              : "target",
            placed->base.id, placed->base.kind);
        }
        return marks;
      }
    }
    // the Undead FX build (the reanimate-fx memo's D3 B; the fix
    // docs/verbs/second-hand.md owes: "a call made inside an active effect
    // whose caster is the seat's own player counts as that seat's"): an
    // actor's look row on a stack whose root is an active effect this seat
    // runs with its own player the caster (RootEffectRole, the gate's own
    // test) is this seat's whoever simulates the actor now, so the look a
    // player's spell makes on its target crosses from the caster's seat:
    // vanilla ReanimateAshPile's TurnToAsh on a thrall whose command ADR-060
    // ends at its death, before the script's first Wait ends. Queued as an
    // owned look is (QueueLook), under the script action's mark, and counted
    // " (effect)", the first selves a window logged. An effect on this seat's
    // own player (kTarget) keeps the owner's judgment. The server judges the
    // actor by its simulator (wire-rules natives), so a report after the
    // command's end is refused there (E_SCRIPTCALL_OWNER) until the server
    // knows an effect's caster: that refusal is the probe's read
    if (info.actorLook && g_effectHelpers) {
      const RE::BSScript::StackFrame* root = nullptr;
      if (RootEffectRole(stack, policy, root) ==
          Frida::PlayersEffectRole::kCaster) {
        if (!QueueLook(row, info, stack, top, page, ref->formID)) {
          return marks;
        }
        marks.action = true;
        Count(row, kCountEffect);
        if (SelfLineDue(g_effectLines[row], ref->formID)) {
          std::string script;
          std::string function;
          NameCaller(root, script, function);
          logger::info("[verb:scripts] {}.{} on {:x}: run inside this seat's "
                       "own player's effect ({}.{}, the player its caster) on "
                       "an actor this seat does not simulate, queued as this "
                       "seat's look (ADR-068 decision 6)",
                       native.cls, native.fn, ref->formID, script, function);
        }
        return marks;
      }
    }
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
  // instead, a player's own row (rung 5's transformation, D-R1 C's
  // inventory) on the seat's own player alone.
  // Since rung 3 (thuum ADR-068, the script record) every other
  // state row's call also crosses as a touch (QueueTouch): the owner's mark
  // that the self's or the calling script's state may have changed
  const bool batched = info.batchedState &&
    (info.record != RecordRow::PlayerOwn || ref->formID == 0x14);
  if (native.kind == NativeKind::State && !batched) {
    Count(row, kCountOwned);
    QueueTouch(row, info, stack, top, ref, isActor);
    return marks;
  }
  // an owned action row's extent is the script action's (6.7) once its call
  // is queued below: a path that drops the call (unrelayable, a frame
  // mismatch) leaves the mark down, so the engine-level hooks still report the
  // animation and nothing goes silent (rung 2's review, finding 5). An
  // actor's look row, the actor simulated here, is queued with its effect
  // form for an Arg0Method row's self (QueueLook)
  if (info.actorLook) {
    marks.action = QueueLook(row, info, stack, top, page, ref->formID);
    return marks;
  }
  if (!info.relayable) {
    Count(row, kCountUnrelayable);
    return marks;
  }
  // train 52's Fix 1 (D-R1 C's leveled gap): an AddItem of a leveled list
  // (CommonLibSSE-NG include/RE/F/FormTypes.h:193, LVLI) on the seat's own
  // player is parked by its stack id until its AddItemFunctor lands the roll
  // (the park table, above), the switch on; the batch carries it at the
  // functor's close with `landed`, what the engine's roll put in the player
  if (info.addItem && ref->formID == 0x14 && g_leveledLandings) {
    const RE::TESForm* list = Arg0Form(info, top, page);
    if (list && list->GetFormType() == RE::FormType::LeveledItem) {
      std::optional<QueuedCall> call =
        CallFromFrame(row, info, stack, top, page, ref->formID);
      if (!call) {
        return marks;
      }
      Park(stack->stackID, std::move(*call));
      marks.parked = true;
      return marks;
    }
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
  for (const auto& [cls, name] : kLookOnlyRows) {
    if (_stricmp(kRows[row].cls, cls) == 0 &&
        _stricmp(kRows[row].fn, name) == 0) {
      info.lookOnly = true;
    }
  }
  // the Undead FX build's actor look rows, by name
  for (const auto& [cls, name] : kActorLookRows) {
    if (_stricmp(kRows[row].cls, cls) == 0 &&
        _stricmp(kRows[row].fn, name) == 0) {
      info.actorLook = true;
    }
  }
  // rung 5's acting scope: GetPlayer's row and the Quest rows, by name
  info.getPlayer = _stricmp(kRows[row].cls, kPlayerRow.first) == 0 &&
    _stricmp(kRows[row].fn, kPlayerRow.second) == 0;
  for (const auto& questRow : kQuestRows) {
    if (_stricmp(kRows[row].cls, questRow.cls) == 0 &&
        _stricmp(kRows[row].fn, questRow.fn) == 0) {
      info.quest = questRow.quest;
    }
  }
  info.globalSet = _stricmp(kRows[row].cls, kGlobalSetRow.first) == 0 &&
    _stricmp(kRows[row].fn, kGlobalSetRow.second) == 0;
  // train 52's Fix 2: the effect rule's two rows, by name
  info.placeAtMe = _stricmp(kRows[row].cls, kPlaceAtMeRow.first) == 0 &&
    _stricmp(kRows[row].fn, kPlaceAtMeRow.second) == 0;
  info.deletes = _stricmp(kRows[row].cls, kDeleteRow.first) == 0 &&
    _stricmp(kRows[row].fn, kDeleteRow.second) == 0;
  // train 52's Fix 1: the leveled landing's row, by name
  info.addItem = _stricmp(kRows[row].cls, kAddItemRow.first) == 0 &&
    _stricmp(kRows[row].fn, kAddItemRow.second) == 0;
  // D-W2 A's look rows (the plan's section 7.2(a)): the action rows, the two
  // motion rows among kTrapRows, and the look-only rows; an actor's look row
  // is never a trigger's own call on itself (OnCall keeps it off a world
  // object before the trap family)
  info.look = (kRows[row].kind == NativeKind::Action && !info.actorLook) ||
    info.trap == TrapRow::Motion || info.trap == TrapRow::Impulse ||
    info.lookOnly;
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
  if (info.lookOnly) {
    return ", look only (D-W2: a family trigger's own call from its firing "
           "seat)";
  }
  if (info.actorLook) {
    return ", an actor's look (batched from its simulator, the Undead FX "
           "build)";
  }
  if (info.alias) {
    return ", batched on every seat (an alias fill)";
  }
  switch (info.record) {
    case RecordRow::RefState:
      return ", batched (a ref state record row)";
    case RecordRow::Standing:
      return ", batched (a standing record row)";
    case RecordRow::PlayerOwn:
      return ", batched on the seat's own player alone (a player's own row)";
    case RecordRow::None:
      break;
  }
  return info.batchedState ? ", batched" : "";
}

// one key of skymp5-client-settings.txt, found in the platform's plugin
// folders (SkyrimPlatform.ini's PluginFolders, Settings::GetPluginFolders:
// the folders SkyrimPlatform.cpp loads every "-settings.txt" from, a later
// folder's file winning as it does there) and read with the same reader
// (Viet::ReadFileIntoString), so the lab sets each key as it sets cellHost
// and npcPuppet (relaunch {files: [{json:
// Data/Platform/Plugins/skymp5-client-settings.txt, key, value}]}): the
// winning file's value, and nothing when that file lacks the key, is no JSON
// object or cannot be read, or no file exists. One reader for the seam's
// switches (train 52 factored it out of the lever's): scriptNonOwnerSkip,
// scriptEffectHelpers and scriptLeveledLandings
struct ClientSettingRead
{
  std::optional<nlohmann::json> value;
  // the winning file, empty when none exists
  std::string path;
};

ClientSettingRead ClientSetting(const char* key)
{
  ClientSettingRead read;
  try {
    const std::string name(key);
    const auto folders = Settings::GetPlatformSettings()->GetPluginFolders();
    for (const auto& folder : *folders) {
      const auto path = folder / "skymp5-client-settings.txt";
      if (!std::filesystem::exists(path)) {
        continue;
      }
      read.value.reset();
      read.path = path.string();
      const auto settings =
        nlohmann::json::parse(Viet::ReadFileIntoString(path), nullptr, false);
      if (!settings.is_object()) {
        logger::warn("[verb:scripts] {} is no JSON object; {} read as unset",
                     read.path, name);
        continue;
      }
      const auto it = settings.find(name);
      if (it != settings.end()) {
        read.value = *it;
      }
    }
  } catch (const std::exception& e) {
    logger::warn("[verb:scripts] {} unread ({}); read as unset", key,
                 e.what());
    return ClientSettingRead{};
  }
  return read;
}

// a switch on unless the winning file sets it false, the client's own
// settings' rule (`settings[key] !== false`)
bool ClientSwitchOn(const char* key)
{
  const ClientSettingRead read = ClientSetting(key);
  return !(read.value && read.value->is_boolean() && !read.value->get<bool>());
}

// the lever's value: "scriptNonOwnerSkip" (ClientSetting's; relaunch {files:
// [{json: Data/Platform/Plugins/skymp5-client-settings.txt, key:
// scriptNonOwnerSkip, value: nonLatent}]}). "nonLatent" skips a non-latent
// body, "recorded" a latent record row's too (rung 4); "none", an absent key
// or file, an unreadable file or any other value leaves the skip off
NonOwnerSkip ReadNonOwnerSkip()
{
  const ClientSettingRead read = ClientSetting("scriptNonOwnerSkip");
  if (!read.value) {
    return NonOwnerSkip::None;
  }
  const std::string value =
    read.value->is_string() ? read.value->get<std::string>() : "";
  if (value == "nonLatent") {
    return NonOwnerSkip::NonLatent;
  }
  if (value == "recorded") {
    return NonOwnerSkip::Recorded;
  }
  if (value != "none") {
    logger::warn("[verb:scripts] scriptNonOwnerSkip in {} is not none, "
                 "nonLatent or recorded; read as none",
                 read.path);
  }
  return NonOwnerSkip::None;
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
  // D-Q4 C: the window's Quest row calls by the cause marked for them
  // (MarkQuestCause), one line when any was
  const auto causes = QuestCause::TakeCounts();
  if (causes.world + causes.seat != 0) {
    logger::info("[verb:quest-stage] {} quest call(s) in {} s marked by "
                 "cause: {} world, {} the seat's own ({} by a fragment's "
                 "carried verdict)",
                 causes.world + causes.seat, kFlushMs / 1000, causes.world,
                 causes.seat, causes.carried);
  }
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
    // the next window logs its own first skipped selves (LogSkipped), and
    // the effect rule's first placements and helper calls (train 52's Fix 2)
    FreeLineSlots(g_skipLines[row]);
    FreeLineSlots(g_effectLines[row]);
    FreeLineSlots(g_helperLines[row]);
  }
  // rung 5: the acting scope's swaps on natives no row holds, one row
  const uint32_t untabled = g_actingUntabled.exchange(0);
  if (untabled != 0) {
    rows.emplace_back("untabled native (acting)", untabled);
    total += untabled;
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

// rung 5's return swap (NativeCallReplacement): Game.GetPlayer's result on a
// scoped stack written over with the acting player's figure, or None when
// it is absent here, once the original answered kCompleted; the VM takes a
// native's result from the stack's returnValue (Stack.h:79), the contract
// SetEmptyResult above relies on (HYPOTHESIS H-AP2, rung 5's H-A1: c2's
// "acting player ff000000 returned yes" line, then the fragment's AddItem
// counted " (acting)" on the figure and c2's own gold unchanged). The line,
// the first three per scope and every hundredth, names the calling script
// and its handler as OnCall read them
void SwapReturn(RE::BSScript::Stack& stack, const Marks& marks)
{
  PackFigure(stack.returnValue, FigureOf(marks.scope.player));
  if (!marks.line) {
    return;
  }
  if (stack.returnValue.IsObject()) {
    logger::info("[verb:quests] acting player {:x} returned yes: "
                 "Game.GetPlayer in {}.{}, stack {}",
                 marks.scope.player.serverId, marks.caller, marks.handler,
                 stack.stackID);
  } else {
    logger::info("[verb:quests] acting player {:x} returned no (absent "
                 "here, None): Game.GetPlayer in {}.{}, stack {}",
                 marks.scope.player.serverId, marks.caller, marks.handler,
                 stack.stackID);
  }
}

// train 52's Fix 2: a placeHelper call's result, the reference the original
// placed, read from the stack's returnValue once it answered kCompleted
// (Stack.h:79, SwapReturn's contract above; HYPOTHESIS H-EF2, x-effect-
// helper v1's census of the helper's own calls reads it) through the VM's
// handle policy (RefOfObject), and recorded as this seat's local helper with
// its base. A None result (the engine placed nothing), an actor, or an id
// below 0xff000000 records nothing; of a PlaceAtMe with aiCount above 1 the
// result, the last reference placed, alone
void NoteHelper(const RE::BSScript::Stack& stack)
{
  auto* vm = VM::GetSingleton();
  const RE::TESObjectREFR* ref = RefOfObject(
    vm ? vm->GetObjectHandlePolicy() : nullptr, ObjectOf(stack.returnValue));
  if (!ref || ref->formID < 0xff000000 ||
      ref->GetFormType() == RE::FormType::ActorCharacter) {
    return;
  }
  const RE::TESBoundObject* base = ref->GetBaseObject();
  if (!base) {
    return;
  }
  Helpers::Add(ref->formID, base->formID, NowMs());
}

// The replacement (the QuestStageSink 53987 shape, through
// HookHandler::Replace): what the attach did at the call's entry, then the
// body through Gum's trampoline in every case the judgment says run, the
// level (and its marks) held for the call's extent. What each path returns:
// the original's own CallResult whenever the body runs (Game.GetPlayer's
// result in returnValue written over inside an acting scope, rung 5's
// SwapReturn); kCompleted, with the row's empty result in returnValue, when
// the lever skips it (a latent record row's too under recorded, H-F2). A
// call of
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
  const RE::BSScript::IFunction::CallResult result =
    g_nativeCallOriginal(self, stack, logger, vm, arg4);
  // rung 5's return swap: GetPlayer's own result is in returnValue by now
  if (marks.swapReturn &&
      result == RE::BSScript::IFunction::CallResult::kCompleted && stack) {
    SwapReturn(*stack.get(), marks);
  }
  // train 52's Fix 2: a helper's PlaceAtMe placed its reference by now
  if (marks.placeHelper &&
      result == RE::BSScript::IFunction::CallResult::kCompleted && stack) {
    NoteHelper(*stack.get());
  }
  // train 52's Fix 1: a parked call whose native answered anything but
  // kInProgress (IFunction::CallResult, CommonLibSSE-NG
  // include/RE/I/IFunction.h:34-41: kCompleted 0, kInProgress 2) queued no
  // AddItemFunctor, so it is let go with its line
  if (marks.parked &&
      result != RE::BSScript::IFunction::CallResult::kInProgress && stack) {
    DropParked(stack->stackID, "the native completed with no functor");
  }
  return result;
}
}

void ScriptNatives::Tick()
{
  const uint64_t now = NowMs();
  FlushCensus(now);
  // rung 5: the acting scope's expired entries dropped once a frame, and
  // D-Q4 C's marks, arms and joined stacks
  ActingScope::Sweep();
  QuestCause::Sweep();
  // train 52's Fix 2: the helpers past their life; Fix 1: the parked calls
  // no functor closed in time
  Helpers::Sweep(now);
  SweepParked(now);
}

void ScriptNatives::ClearAtLoad()
{
  // train 52's Fix 2: a helper's id names the old world's reference; Fix 1:
  // a parked call's stack id the old world's stack
  Helpers::Clear();
  ClearParked();
}

ScriptNatives::FunctorWindow::FunctorWindow(std::uint32_t stackId)
  : stackId_(stackId)
  , saved_(t_functorStack)
{
  t_functorStack = stackId;
}

ScriptNatives::FunctorWindow::~FunctorWindow()
{
  // the close inside the functor's own extent, on its own thread; nothing
  // leaves a destructor that runs inside the engine's call
  try {
    if (stackId_ != 0) {
      CloseParked(stackId_);
    }
  } catch (...) {
  }
  t_functorStack = saved_;
}

void ScriptNatives::NoteContainerChange(std::uint32_t oldContainer,
                                        std::uint32_t newContainer,
                                        std::uint32_t base, std::int32_t count)
{
  const RE::VMStackID stackId = t_functorStack;
  if (stackId == 0) {
    return;
  }
  // inside the engine's event dispatch: nothing leaves it
  try {
    NoteLanding(stackId, oldContainer, newContainer, base, count);
  } catch (...) {
  }
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

void ScriptNatives::SetTriggerLook(bool on)
{
  g_triggerLook.store(on, std::memory_order_relaxed);
}

bool ScriptNatives::TriggerLook()
{
  return g_triggerLook.load(std::memory_order_relaxed);
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
  // module base, the callback's offset, the long-signature mark); a row
  // binds the game's own module's native (SKSE's natives are not scripts'
  // consequences here), except a row the game's module binds nothing of its
  // name for, which binds the VM's latest other binding of it: rung 5's
  // Actor.ChangeHeadPart, SKSE's (Eli's D-E1 A), the one table row the game
  // does not hold. HYPOTHESIS H-HP1: SKSE's native reaches this seam's hook,
  // its IFunction::Call running the game's NativeFunctionBase::Call body
  // (104651) as CommonLib's own does (src/RE/N/NativeFunctionBase.cpp:
  // 106-112), its frame read as a game native's and its latency at
  // NativeFunctionBase's _isLatent (include/RE/N/NativeFunctionBase.h:69);
  // the census's Actor.ChangeHeadPart row counting a call reads it, and the
  // latest binding being the one the VM dispatches is part of it
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
    const RE::BSScript::IFunction* other = nullptr;
    std::uintptr_t otherBase = 0;
    for (const auto& [cls, name, func, moduleBase, offset, longSig] : bound) {
      if (_stricmp(cls.c_str(), native.cls) != 0 ||
          _stricmp(name.c_str(), native.fn) != 0) {
        continue;
      }
      if (moduleBase == gameBase) {
        function = func;
        break;
      }
      other = func;
      otherBase = moduleBase;
    }
    const bool fromOther = !function && other;
    if (fromOther) {
      function = other;
      logger::info("[verb:scripts] table row {}.{}: no binding in the game's "
                   "module; bound from its binding outside it (allocation "
                   "base {:x}), the VM's latest of its name (H-HP1)",
                   native.cls, native.fn, otherBase);
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
  for (const auto& [cls, name] : kLookOnlyRows) {
    WarnUnbound("look-only", cls, name);
  }
  for (const auto& [cls, name] : kActorLookRows) {
    WarnUnbound("actor look", cls, name);
  }
  WarnUnbound("acting player", kPlayerRow.first, kPlayerRow.second);
  for (const auto& questRow : kQuestRows) {
    WarnUnbound("acting quest", questRow.cls, questRow.fn);
  }
  WarnUnbound("player global", kGlobalSetRow.first, kGlobalSetRow.second);
  WarnUnbound("effect helper", kPlaceAtMeRow.first, kPlaceAtMeRow.second);
  WarnUnbound("helper delete", kDeleteRow.first, kDeleteRow.second);
  WarnUnbound("leveled landing", kAddItemRow.first, kAddItemRow.second);
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
  // train 52's Fix 2: the effect rule's switch, read beside the lever; since
  // the Undead FX build it carries an actor's look inside an effect this
  // seat's own player cast as well
  g_effectHelpers = ClientSwitchOn("scriptEffectHelpers");
  if (g_effectHelpers) {
    logger::info("[verb:scripts] scriptEffectHelpers on: a PlaceAtMe of an "
                 "activator, a static or a light inside this seat's own "
                 "player's effect runs whatever its subject's owner, and what "
                 "it places is this seat's local helper; an actor's look "
                 "inside an effect this seat's player cast is this seat's");
  } else {
    logger::info("[verb:scripts] scriptEffectHelpers off: a PlaceAtMe or an "
                 "actor's look inside this seat's own player's effect is "
                 "judged by its subject's owner, as every call is");
  }
  // the Undead FX build: the Papyrus event gate's owner rule (FridaHooks.cpp
  // OnSendEventEnter, the reanimate-fx memo's D5 B), its switch read here
  // with the seam's by the one reader, before the game's scripts run
  const bool ownerEvents = ClientSwitchOn("scriptOwnerEvents");
  Frida::SetScriptOwnerEvents(ownerEvents);
  if (ownerEvents) {
    logger::info("[verb:papyrus-events] scriptOwnerEvents on: an event to a "
                 "world quest's reference alias passes on the seat that owns "
                 "the alias's reference, and one to an effect on an actor "
                 "that is not the player on the seat that simulates it, each "
                 "tallied as \"owner rule tally\" (D5 B)");
  } else {
    logger::info("[verb:papyrus-events] scriptOwnerEvents off: the owner "
                 "rule asks nothing, and an alias's or an actor's effect's "
                 "events keep the block's rules (D5 B)");
  }
  // train 52's Fix 1: the leveled landing's switch, read beside the lever,
  // and the AddItemFunctor hook Hooks::Install wrote at the plugin's load,
  // named in one line: the vtable CommonLibSSE-NG's
  // include/RE/Offsets_VTABLE.h:4616 names (AE id 218372, `just addr
  // 218372` on 1.6.1170), its slot 01 (include/RE/D/DelayFunctor.h:35), and
  // what the slot held before the write (Hooks::AddItemFunctorSlotWas)
  g_leveledLandings = ClientSwitchOn("scriptLeveledLandings");
  if (g_leveledLandings) {
    const auto [slotBase, slotOffset] = Hooks::AddItemFunctorSlotWas();
    std::string held = "nothing: not hooked";
    if (slotBase == gameBase) {
      held = fmt::format("the game's module +{:x}", slotOffset);
    } else if (slotBase != 0 || slotOffset != 0) {
      held = fmt::format("allocation base {:x} +{:x}", slotBase, slotOffset);
    }
    logger::info("[verb:scripts] AddItemFunctor's operator() hooked "
                 "(CommonLibSSE-NG VTABLE_SkyrimScript____AddItemFunctor slot "
                 "01, AE id 218372; the slot held {}): a leveled AddItem on "
                 "the seat's own player is reported with its roll",
                 held);
  } else {
    logger::info("[verb:scripts] scriptLeveledLandings off: a leveled AddItem "
                 "on the seat's own player crosses without its roll");
  }
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
