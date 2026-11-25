#include "FridaHooks.h"
#include "ActingScope.h"
#include "ChangeForm.h"
#include "EventsApi.h"
#include "Fates.h"
#include "FridaHookHandler.h"
#include "FridaHooksUtils.h"
#include "Hosted.h"
#include "JsUtils.h"
#include "MagicApi.h"
#include "PapyrusTESModPlatform.h"
#include "PlatformCall.h"
#include "PropBodies.h"
#include "Puppets.h"
#include "QuestCause.h"
#include "ScriptNatives.h"
#include "ScriptState.h"
#include "SkyrimPlatform.h"
#include "StringHolder.h"
#include "TrapFamily.h"

#include <RE/A/AIProcess.h>
#include <RE/A/ActiveEffect.h>
#include <RE/A/Actor.h>
#include <RE/A/ActorMagicCaster.h>
#include <RE/A/ActorValues.h>
#include <RE/B/BGSBaseAlias.h>
#include <RE/B/BGSLocAlias.h>
#include <RE/B/BGSRefAlias.h>
#include <RE/B/BGSScene.h>
#include <RE/B/BSSoundHandle.h>
#include <RE/B/BSTEvent.h>
#include <RE/B/bhkCharProxyController.h>
#include <RE/B/bhkRigidBody.h>
#include <RE/D/DialogueItem.h>
#include <RE/E/Effect.h>
#include <RE/E/EffectSetting.h>
#include <RE/E/Explosion.h>
#include <RE/H/HighProcessData.h>
#include <RE/H/hkpEntity.h>
#include <RE/H/hkpRigidBody.h>
#include <RE/H/hkpRootCdPoint.h>
#include <RE/I/IFunctionArguments.h>
#include <RE/M/MagicTarget.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiControllerSequence.h>
#include <RE/O/Object.h>
#include <RE/P/Projectile.h>
#include <RE/T/TESHavokUtilities.h>
#include <RE/T/TESQuest.h>
#include <RE/T/TESQuestStageEvent.h>
#include <RE/T/TESTopic.h>
#include <RE/T/TESTopicInfo.h>
#include <RE/V/Variable.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <deque>
#include <fmt/format.h>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * The admitted scripts (thuum ADR-044, docs/verbs/papyrus-events.md)
 */

// Under skymp5-client's Papyrus event block (blockPapyrusEventsService.ts,
// set at the client's first tick), the SendEvent hook below blanks every
// event a script would receive unless the script is admitted. Two ways in
// since train-29: by class chain (the script or any class it extends is
// SkyUI's, SKI_*: a mod's MCM extends SKI_ConfigBase, MCM Helper's
// MCM_ConfigBase extends it too), and by plugin (Data/Platform/
// papyrus-scripts.txt, laid by the mod layer: one line per admitted plugin,
// "<plugin>: <class> <class> ...", a plugin's script class names as its
// VMAD and .pex spell them; the Survival Mode Creation first). The file is
// read once here, at Skyrim Platform's load, because a script whose first
// OnUpdate is blanked never re-arms its RegisterForSingleUpdate chain
// (x-survival-unblock 20261020-023907: a block lifted after login changed
// nothing; x-survival-unblock3 20261020-024406: lifted and re-armed, the
// Creation's check ran and its prompt appeared). No file: an empty list and
// the behavior before train-29. Names compare without case (Papyrus's rule).
namespace {
std::unordered_set<std::string> g_admittedScripts;
// ADR-059 decision 1, ADR-062 decision 1: the cells this game hosts
// (SetCellHosted), where every world script runs
std::mutex g_hostedCellsMutex;
std::unordered_set<uint32_t> g_hostedCells;

std::string LowerAscii(const char* s)
{
  std::string out = s ? s : "";
  for (auto& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

void LoadAdmittedScripts()
{
  g_admittedScripts.clear();
  std::ifstream file("Data/Platform/papyrus-scripts.txt");
  if (!file.is_open()) {
    logger::info("[verb:papyrus-events] no Data/Platform/papyrus-scripts.txt: "
                 "no plugin's scripts admitted");
    return;
  }
  std::string line;
  size_t plugins = 0;
  size_t hostRows = 0;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    auto colon = line.find(": ");
    if (colon == std::string::npos) {
      continue;
    }
    std::string plugin = LowerAscii(line.substr(0, colon).c_str());
    while (!plugin.empty() &&
           (plugin.back() == ' ' || plugin.back() == '\t')) {
      plugin.pop_back();
    }
    // "<plugin> host: ..." was ADR-059's host-scoped row, retired by ADR-062
    // decision 1 (in a cell this game hosts every world script runs, named
    // or not): an older layer file's row is skipped, its names admitted
    // nowhere
    if (plugin.size() > 5 &&
        plugin.compare(plugin.size() - 5, 5, " host") == 0) {
      ++hostRows;
      continue;
    }
    ++plugins;
    std::string rest = line.substr(colon + 2);
    size_t pos = 0;
    while (pos < rest.size()) {
      auto end = rest.find(' ', pos);
      if (end == std::string::npos) {
        end = rest.size();
      }
      if (end > pos) {
        auto name = rest.substr(pos, end - pos);
        while (!name.empty() && (name.back() == '\r' || name.back() == '\t')) {
          name.pop_back();
        }
        if (!name.empty()) {
          g_admittedScripts.insert(LowerAscii(name.c_str()));
        }
      }
      pos = end + 1;
    }
  }
  logger::info(
    "[verb:papyrus-events] {} script class name(s) admitted from {} "
    "plugin line(s) in Data/Platform/papyrus-scripts.txt, {} host-scoped "
    "row(s) skipped (ADR-062 decision 1)",
    g_admittedScripts.size(), plugins, hostRows);
}

// A script passes if it, or any class it extends, is SkyUI's (SKI_*) or is
// named by an admitted plugin's line. The chain is ObjectTypeInfo's
// parentTypeInfo (CommonLibSSE-NG include/RE/O/ObjectTypeInfo.h:127-128,
// :178); it ends at a null parent, and the walk stops at 16 either way.
// ADR-059 decision 1's lifecycle events: the engine telling a script the
// world's state (its reference loaded, its cell attached, a reset), not a
// player's act. A world object's script takes these on every seat
// (OnSendEventEnter's rule), because they come before any seat hosts the cell
// (x-object-anim v3 20261102-051618: the temple loaded at c1's teleport and c1
// hosted it 3.5 s later, so the puzzle pillars' defaultPuzzlePillarScript
// never got its OnCellLoad, stayed in its empty state with no onActivate, and
// c1's turn did nothing) and because a guest's pillar should stand on its
// initial face (OnCellLoad's StartState02 and StartState03). What a lifecycle
// handler does beyond state (a mid-firing trap's fireTrap at onCellAttach)
// plays as a visual on a guest; the harm, OnTrapHitStart, stays the host's.
// Since ADR-068's D-S3 a trap family reference's every event runs on every
// seat (OnSendEventEnter's family rule, TrapFamily.h), and its harm counts on
// the hit actor's own seat alone (the seam's owner test on ProcessTrapHit,
// ScriptNatives.cpp).
bool IsLifecycleEvent(const std::string& lowerName)
{
  static const char* const kLifecycle[] = {
    "oninit",       "onload",       "onunload",         "oncellload",
    "oncellattach", "oncelldetach", "onattachedtocell", "ondetachedfromcell",
    "onreset"
  };
  for (const char* name : kLifecycle) {
    if (lowerName == name) {
      return true;
    }
  }
  return false;
}

bool AdmittedByChain(const RE::BSScript::ObjectTypeInfo* info)
{
  for (int depth = 0; info && depth < 16; ++depth, info = info->GetParent()) {
    auto name = LowerAscii(info->GetName());
    if (name.size() >= 4 && name.compare(0, 4, "ski_") == 0) {
      return true;
    }
    if (!g_admittedScripts.empty() && g_admittedScripts.count(name)) {
      return true;
    }
  }
  return false;
}

// thuum docs/verbs/shadowbond.md (b-shadowbond's second half, 2026-11-01):
// scripts whose events stay blocked in every game whatever the rules below
// admit, because the server owns their one consequence. Apocalypse's
// WB_InvisProcTeleport_Script polls both bonded actors' invisibility every
// 0.2 s and swaps their places itself the first time either is visible; with
// the player as the caster the ADR-031 effect rule would run it, and the
// server's own dispel at a bond's run-out tripped its swap (run
// 20261101-224953: the caster moved to the target's place, the figure to the
// box, 131 units carried to the server as movement). The server casts the
// procs, swaps at a break and dispels at the run-out (PartOne::RecordBond,
// BreakBondsOf, TickBonds); the script's sound and visual effects at the
// swap are the caster's loss until a snippet carries them. Names compare
// without case, through the parent chain, as the admitted ones do.
const char* const kNeverAdmitted[] = { "wb_invisprocteleport_script" };

bool NeverAdmittedByChain(const RE::BSScript::ObjectTypeInfo* info)
{
  for (int depth = 0; info && depth < 16; ++depth, info = info->GetParent()) {
    auto name = LowerAscii(info->GetName());
    for (auto never : kNeverAdmitted) {
      if (name == never) {
        return true;
      }
    }
  }
  return false;
}

// the form id an event's handle names, resolved as OnSendEventEnter resolves
// its self (the actor, then the reference, through the VM's handle policy);
// 0 for a handle that names neither (a quest's, an effect's)
uint32_t SelfIdOfHandle(RE::VMHandle handle)
{
  auto* vm = VM::GetSingleton();
  auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
  if (!policy) {
    return 0;
  }
  if (auto* actor =
        policy->GetObjectForHandle(RE::FormType::ActorCharacter, handle)) {
    return actor->GetFormID();
  }
  if (auto* refr =
        policy->GetObjectForHandle(RE::FormType::Reference, handle)) {
    return refr->GetFormID();
  }
  return 0;
}

// thuum ADR-068 rung 5's full build (docs/research/rung5-quests-2026-11-09.md
// 8(a); ActingScope.h, PlayerQuests): the per-player quest an event's handle
// belongs to, read through the VM's handle policy by CommonLib's own types,
// as SelfIdOfHandle above reads an actor or a reference and the gate's
// effect branch reads its effect: the quest itself (TESQuest::FORMTYPE,
// CommonLibSSE-NG include/RE/T/TESQuest.h:195), else an alias's owning quest
// (BGSBaseAlias::owningQuest, include/RE/B/BGSBaseAlias.h:76), the alias
// asked by the three alias VM types (BGSRefAlias::VMTYPEID, BGSRefAlias.h:18;
// BGSLocAlias::VMTYPEID, BGSLocAlias.h:13; BGSBaseAlias::VMTYPEID,
// BGSBaseAlias.h:15) and resolved by the first that holds; 0 when the handle
// names neither, or its quest is not in the set
uint32_t PlayerQuestOfHandle(RE::BSScript::IObjectHandlePolicy* policy,
                             RE::VMHandle handle)
{
  uint32_t questId = 0;
  if (policy->HandleIsType(RE::TESQuest::FORMTYPE, handle)) {
    if (RE::TESForm* quest =
          policy->GetObjectForHandle(RE::TESQuest::FORMTYPE, handle)) {
      questId = quest->GetFormID();
    }
  }
  if (questId == 0) {
    const RE::VMTypeID aliasTypes[] = { RE::BGSRefAlias::VMTYPEID,
                                        RE::BGSLocAlias::VMTYPEID,
                                        RE::BGSBaseAlias::VMTYPEID };
    for (const RE::VMTypeID type : aliasTypes) {
      if (!policy->HandleIsType(type, handle)) {
        continue;
      }
      const auto* alias = static_cast<const RE::BGSBaseAlias*>(
        policy->GetObjectForHandle(type, handle));
      if (alias && alias->owningQuest) {
        questId = alias->owningQuest->GetFormID();
      }
      break;
    }
  }
  return questId != 0 && ActingScope::PlayerQuests::Is(questId) ? questId : 0;
}

// the gate's player-quest line, once per quest (a mutex set, cleared past
// 1024 quests)
std::mutex g_playerQuestLinesMutex;
std::unordered_set<uint32_t> g_playerQuestLines;

bool PlayerQuestLineDue(uint32_t questId)
{
  std::lock_guard<std::mutex> lock(g_playerQuestLinesMutex);
  if (g_playerQuestLines.size() > 1024 &&
      g_playerQuestLines.count(questId) == 0) {
    g_playerQuestLines.clear();
  }
  return g_playerQuestLines.insert(questId).second;
}
}

// thuum docs/research/rung5-quests-2026-11-09.md 1(c): every grant and
// release logged, so a grant and the gate's lines order in one platform log
// (risk (c)'s read, H-TP1: an NPC's entry blanked at the host's own load
// before its grant)
void Frida::SetCellHosted(uint32_t cellFormId, bool hosted)
{
  size_t hostedCells = 0;
  {
    std::lock_guard<std::mutex> lock(g_hostedCellsMutex);
    if (hosted) {
      g_hostedCells.insert(cellFormId);
    } else {
      g_hostedCells.erase(cellFormId);
    }
    hostedCells = g_hostedCells.size();
  }
  logger::info("[verb:cell-host] SetCellHosted({:x}, {}): {} cell(s) hosted "
               "here",
               cellFormId, hosted, hostedCells);
}

bool Frida::IsCellHosted(uint32_t cellFormId)
{
  std::lock_guard<std::mutex> lock(g_hostedCellsMutex);
  return g_hostedCells.count(cellFormId) > 0;
}

// the steady clock in milliseconds, defined below beside the cast starts
uint64_t NowMs();

/**
 * The presser rule (thuum docs/DECISIONS.md ADR-068 D-W1 A and D-C2 A as one
 * rule, Eli 2026-11-06; docs/research/fifty-fixes-2026-11-07.md 1(b)): a
 * trigger volume's entry counts once, on its presser's owner, and travels to
 * the trigger script's owner. Every seat's engine raises OnTriggerEnter and
 * OnTriggerLeave for whatever collidable enters its copy of a volume: the
 * engine's admission tests the collidable's collision layer and nothing of
 * the seat (TESTrapListener's overlap callback, no player, AI, puppet or
 * life-state test: lab/.cache/drafts/fifty/trigger-entry-static.md section
 * 4, HYPOTHESIS), so another seat's figure or a puppet presses this seat's
 * copy (H-TE2) and one press fired a trap family trigger on every seat.
 *
 * The presser is the event's one argument, akActionRef. SkyrimVM's two
 * trigger sinks (BSTEventSink<TESTriggerEnterEvent> and
 * <TESTriggerLeaveEvent>, CommonLibSSE-NG include/RE/S/SkyrimVM.h:130-131)
 * pass it to SendEvent, the gate's hook below, inside their own argument
 * object, `anonymous namespace'::SingleObjectRefrHandleArgument, whose
 * vtable CommonLib names VTABLE___SingleObjectRefrHandleArgument
 * (include/RE/Offsets_VTABLE.h:4309; Address Library 217161, `just addr`
 * 2026-11-07, addrlib: 0x1912b00 on 1.6.1170; CONFIRMED-STATIC): SendEvent's
 * arg 3, its first word that vtable. The gate compares that word before it
 * reads anything else, so an argument of another class (a mod's own) is
 * never dereferenced: it passes as before, counted, with a line a minute.
 *
 * HYPOTHESIS H-TE1 (trigger-entry-static.md 2.6 and 2.10): the object holds
 * the actionRef as a plain TESObjectREFR* at +0x08, where the sink stores it
 * with its reference count raised, its packer (slot 01) reads it and its
 * destructor (slot 00) releases it. Confirmed only by x-trigger-entry v1 on
 * both seats (lab/frida/trigger-entry-trace.js: the ready row's argOk,
 * enterSinkOk and leaveSinkOk true, and every pack row's agree true); this
 * rule does not merge before that read.
 */
namespace {
// the rule's switch (Frida::SetTriggerPresserRule), off by default
std::atomic<bool> g_triggerPresserRule{ false };

// H-TE1's member: the actionRef's place in the engine's trigger argument
constexpr std::ptrdiff_t kTriggerArgumentActionRef = 0x08;

// the entry an event name is, without case (Papyrus's rule): 1 for
// OnTriggerEnter, 2 for OnTriggerLeave (the wire's entry values), 0 for any
// other name; OnTrigger, the continuous event, is none of them (the word
// wall's sendPapyrusEvent read of it is untouched)
int TriggerEntryOf(const char* name)
{
  if (!name) {
    return 0;
  }
  if (_stricmp(name, "OnTriggerEnter") == 0) {
    return 1;
  }
  if (_stricmp(name, "OnTriggerLeave") == 0) {
    return 2;
  }
  return 0;
}

// the engine's trigger argument's vtable, resolved once through CommonLib's
// Relocation (Hooks.cpp's vtable idiom), first at the hook's install
// (InstallSendEventHook); 0 if the Address Library names no such id, and
// then no argument ever matches and every trigger event passes as before
std::uintptr_t TriggerArgumentVtable()
{
  static const REL::Relocation<std::uintptr_t> vtable{
    RE::VTABLE___SingleObjectRefrHandleArgument[0]
  };
  return vtable.address();
}

// how the gate read a trigger event's argument
enum class PresserRead
{
  kPresser,
  kNoArgument,
  kOtherClass,
  kNoPresser
};

const char* PresserReadText(PresserRead how)
{
  switch (how) {
    case PresserRead::kNoArgument:
      return "no argument";
    case PresserRead::kOtherClass:
      return "its argument is not the engine's trigger argument "
             "(SingleObjectRefrHandleArgument)";
    case PresserRead::kNoPresser:
      return "the engine's trigger argument names no presser";
    default:
      return "its presser read";
  }
}

// the presser the engine's trigger argument names, or null: the vtable
// compared first, the member (H-TE1) read only on a match; one compare and
// one load (rule 9)
RE::TESObjectREFR* ReadPresser(const void* args, PresserRead& how)
{
  if (!args) {
    how = PresserRead::kNoArgument;
    return nullptr;
  }
  const std::uintptr_t vtable = TriggerArgumentVtable();
  if (vtable == 0 || *static_cast<const std::uintptr_t*>(args) != vtable) {
    how = PresserRead::kOtherClass;
    return nullptr;
  }
  auto* presser = *reinterpret_cast<RE::TESObjectREFR* const*>(
    static_cast<const std::uint8_t*>(args) + kTriggerArgumentActionRef);
  how = presser ? PresserRead::kPresser : PresserRead::kNoPresser;
  return presser;
}

// the presser's owner here (the plan's rule table, "A trigger
// volume's entry"; the train's fresh review, note 2: the seam's owner,
// ScriptNatives.cpp OwnedForRecord, so the gate and the seam name the same
// one): its own player; a non-actor (a prop, a thrown item: D-W1 names
// actors) unless it is a trap body another seat streams
// (TrapFamily::Followed); an actor at or above 0xff000000 (a player's
// figure, what the server or a seat places) by the server's word alone, its
// simulator's (Hosted.h, no puppet, Puppets.h; a figure is never hosted, so
// a dead player's figure counts on its own seat and not on the cell's host);
// a body (TESObjectREFR::IsDead, include/RE/T/TESObjectREFR.h:353) in a cell
// this seat hosts, a body being its cell host's until M4; a live game-file
// actor its simulator's, hosted here and no puppet, the gate's own
// simulatedActor test; and a game-file actor the server gave this seat no
// word on, neither hosted nor a puppet (the ambush draugr 0x380ba, which no
// seat simulates until its trigger enables it), its cell host's. Reads
// alone: the form type byte (include/RE/T/TESForm.h:289), the life state
// behind IsDead and the parent cell (TESObjectREFR.h:413). Three answers:
// this seat, another, and the last case's undecided one (a live game-file
// actor neither hosted nor a puppet, in a cell this seat does not host: no
// seat's word reached this seat yet), the one whose world trigger entries
// D-W3 B holds (below); this seat owns the presser on kThisSeat alone.
//
// One taxonomy, two callers (ADR-071 rung 3, lab/.cache/drafts/physics/
// rung3-plan.md B3 point 4): the presser rule's, and an impulse's cause at
// the replaced ScaleGameplayImpulseForce and AddExplosionImpulse (the
// impulse rule, below), whose cause is the explosion's owner or the
// projectile's shooter. Its actor branches are the projectile launch hook's
// split of shooters (OnProjectileLaunchEnter: the player, a hosted actor, a
// puppet, a figure), so a cause this seat's engine simulates is this seat's
// and another seat's figure or puppet is another's. Every read is a
// mutex-guarded set or a field of the cause, so any engine thread may ask
// (the impulse's run on Havok's workers, the read's 080520 rows)
enum class CauseOwner
{
  kThisSeat,
  kOther,
  kUndecided
};

CauseOwner OwnerOf(const RE::TESObjectREFR* cause)
{
  const uint32_t id = cause->formID;
  if (id == 0x14) {
    return CauseOwner::kThisSeat;
  }
  if (cause->GetFormType() != RE::FormType::ActorCharacter) {
    return TrapFamily::Followed::Is(id) ? CauseOwner::kOther
                                        : CauseOwner::kThisSeat;
  }
  if (id >= 0xff000000) {
    return Hosted::SimulatedHere(id) ? CauseOwner::kThisSeat
                                     : CauseOwner::kOther;
  }
  if (cause->IsDead()) {
    const RE::TESObjectCELL* cell = cause->GetParentCell();
    return cell && Frida::IsCellHosted(cell->GetFormID())
      ? CauseOwner::kThisSeat
      : CauseOwner::kOther;
  }
  const bool hosted = Hosted::Is(id);
  const bool puppet = Puppets::Is(id);
  if (hosted || puppet) {
    return hosted && !puppet ? CauseOwner::kThisSeat : CauseOwner::kOther;
  }
  const RE::TESObjectCELL* cell = cause->GetParentCell();
  if (!cell) {
    return CauseOwner::kOther;
  }
  return Frida::IsCellHosted(cell->GetFormID()) ? CauseOwner::kThisSeat
                                                : CauseOwner::kUndecided;
}

// whether the handle has an attached script (the VM's attachedScripts under
// its own lock, CommonLibSSE-NG include/RE/V/VirtualMachine.h:219-220, the
// size read and never empty(): include/RE/B/BSTArray.h:749 answers
// inverted): a trigger with none has no script for its entry to reach, and
// the host's SendTriggerEvent would refuse it
bool HasAttachedScript(VM* vm, RE::VMHandle handle)
{
  if (!vm) {
    return false;
  }
  RE::BSSpinLockGuard guard{ vm->attachedScriptsLock };
  const auto it = vm->attachedScripts.find(handle);
  return it != vm->attachedScripts.end() && it->second.size() != 0;
}

// thuum docs/research/rung5-quests-2026-11-09.md 1(c), fix (b): the rule's
// three lines (a press passed for an exempt class, one ignored, one told) are
// due per (kind, trigger), each kind's first three for a trigger and every
// hundredth, so one trigger's burst no longer hides another's first line
// (the plan's read 10: in the 50 stage's two reds the host-to-be's own Temple
// load spent the process-wide first three ignored lines on ambush NPCs'
// entries, so no probe could claim the web's or the plate's own line);
// FurnitureLineDue's twin (below, beside the animation hook: a mutex map
// cleared past 1024 keys). Each line's text is unchanged, its "(<n> so far)"
// still the process-wide count, and the lock is taken on those three
// branches alone (rule 9)
enum class TriggerLine : uint32_t
{
  kExempted = 1,
  kIgnored = 2,
  kTold = 3,
  // D-W3 B's (the held entries, below): a hold, a family trigger's enter
  // not held, a held pair's presser leaving before its owner's grant, an
  // owned presser's event settling a held pair, an expiry
  kHeld = 4,
  kHeldFamily = 5,
  kHeldLeft = 6,
  kHeldSettled = 7,
  kHeldExpired = 8
};

std::mutex g_triggerLinesMutex;
std::unordered_map<uint64_t, uint64_t> g_triggerLines;

bool TriggerLineDue(TriggerLine kind, uint32_t triggerId)
{
  const uint64_t key =
    (static_cast<uint64_t>(static_cast<uint32_t>(kind)) << 32) | triggerId;
  std::lock_guard<std::mutex> lock(g_triggerLinesMutex);
  if (g_triggerLines.size() > 1024 && g_triggerLines.count(key) == 0) {
    g_triggerLines.clear();
  }
  const uint64_t n = ++g_triggerLines[key];
  return n <= 3 || n % 100 == 0;
}

/**
 * The held entries (thuum docs/DECISIONS.md ADR-068 D-W3 B, Eli's rung 5
 * stage-eve words, 2026-11-10; docs/research/rung5-quests-2026-11-09.md
 * section 5, risk (c)'s fix sketch, rebuilt from x-trigger-preload v1's read
 * of run 20261110-073038). An NPC standing inside a trigger when this seat's
 * engine loads the cell enters it before this seat's grant, while its owner
 * here is undecided (OwnerOf's kUndecided), so the rule blanks the enter
 * (H-TP1), and the engine raises no second one while the NPC stays inside
 * (H-TP2; both read on two builds). Blanked alone, a world trigger waiting
 * on that NPC (MS13TriggerArvelRunSceneScript, for Arvel) never runs on any
 * seat. So the rule keeps those enters per (trigger, presser), for a
 * scripted world trigger alone: a trap family trigger (TrapFamily::IsFamily)
 * with an NPC on it stays unfired until its next press, so no trap fires at
 * a host's arrival (D-W3 B).
 *
 * The store counts: the engine raised one presser's enters in twos with no
 * leave between (the prior read's oil pool 0x9b0de by the draugr 0x383f8),
 * so an enter adds one, a leave takes one off and the last leave drops the
 * pair; a pair expires kHeldEntryMs after its first enter. Enters reach the
 * gate on several engine threads (four in the read), so the store has its
 * own mutex, and it keeps taking holds after the grant (an NPC still
 * undecided then). The demote at this seat's own load raises the puppet
 * flag before its grant (Arvel's 97 ms before his setHosted in the read),
 * so a puppet flag drops nothing: a held pair waits for its presser's owner,
 * a leave or the expiry.
 *
 * Its owner is OwnerOf's kThisSeat, read again at skymp5-client's update
 * after the one that saw the client's own hosting state change (a cell's
 * gate opened, an actor marked hosted or promoted; the puppet flag clears in
 * the reconcile pass that marks the actor hosted, after its setHosted, so
 * never at that call), with the trigger in a cell this seat hosts:
 * TESModPlatform.ReplayHeldEntries then replays its enter ONCE through
 * SendTriggerEvent's body under the platform's mark (ReplayHeldEntriesImpl
 * below). An owned presser's own event on a pair still held (its owner came
 * between the hold and that update) settles it: an enter is the engine's
 * delivery, so the held one goes; a leave is blanked, its enter never having
 * reached the script. Only trigger events pay: one atomic read on a
 * presser's event while nothing is held, the lock on a hold, on a held
 * pair's event and in the replay (rule 9).
 *
 * HYPOTHESIS until x-trigger-preload v2 (every engine behaviour): the replay
 * reaches the trigger's script as the engine's own enter would, its aliases
 * relayed. Declared: a pair replayed at a count of 2 delivers one enter and
 * its two leaves follow (the read's load raised one enter per pair); a
 * puppet that never becomes this seat's never comes due (the host's ambush
 * draugr in the read, npc-stream's question, not this store's); a pair held
 * on a seat another seat out-raced to the grant waits while its presser is
 * that seat's puppet and replays if this seat becomes its owner inside
 * kHeldEntryMs, though the other seat's engine counted its own (bounded by
 * the expiry and by rung 3's script record, which carries a stateful
 * script's done state; UNMEASURED); an owned presser's held enter into a
 * trigger in a cell another seat hosts waits until it expires (D-C2 relays
 * live entries alone)
 */
constexpr uint64_t kHeldEntryMs = 60000;
// the most pairs held (UNMEASURED: the read's Temple load held four)
constexpr size_t kHeldPairsMax = 256;

struct HeldEntry
{
  // the enters held, less the leaves since
  uint32_t count = 0;
  // the steady clock at the pair's first enter
  uint64_t firstMs = 0;
};

std::mutex g_heldMutex;
// (trigger << 32 | presser) to its held entry
std::unordered_map<uint64_t, HeldEntry> g_held;
// the pairs held, for the reads that take no lock
std::atomic<size_t> g_heldPairs{ 0 };

uint64_t HeldKey(uint32_t triggerId, uint32_t presserId)
{
  return (static_cast<uint64_t>(triggerId) << 32) | presserId;
}

// an undecided presser's enter to a scripted world trigger, held (the gate
// blanks it as before): one more for the pair, whose clock starts at its
// first enter (an expired pair starts over); false, nothing held, past
// kHeldPairsMax pairs. count and pairs answer the pair's count and the pairs
// held, for the line
bool HoldEntry(uint32_t triggerId, uint32_t presserId, uint64_t nowMs,
               uint32_t& count, size_t& pairs)
{
  std::lock_guard<std::mutex> lock(g_heldMutex);
  const uint64_t key = HeldKey(triggerId, presserId);
  auto it = g_held.find(key);
  if (it != g_held.end() && nowMs >= it->second.firstMs + kHeldEntryMs) {
    g_held.erase(it);
    it = g_held.end();
  }
  bool held = true;
  if (it == g_held.end()) {
    if (g_held.size() >= kHeldPairsMax) {
      held = false;
    } else {
      HeldEntry fresh;
      fresh.firstMs = nowMs;
      it = g_held.emplace(key, fresh).first;
    }
  }
  if (held) {
    count = ++it->second.count;
  }
  pairs = g_held.size();
  g_heldPairs.store(pairs, std::memory_order_relaxed);
  return held;
}

// a leave of a held pair: one off its count, the pair dropped with its last;
// false when the pair is not held, dropped saying whether it went
bool LeaveHeld(uint32_t triggerId, uint32_t presserId, bool& dropped)
{
  dropped = false;
  if (g_heldPairs.load(std::memory_order_relaxed) == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_heldMutex);
  const auto it = g_held.find(HeldKey(triggerId, presserId));
  if (it == g_held.end()) {
    return false;
  }
  if (it->second.count <= 1) {
    g_held.erase(it);
    dropped = true;
  } else {
    --it->second.count;
  }
  g_heldPairs.store(g_held.size(), std::memory_order_relaxed);
  return true;
}

// an owned presser's enter on a held pair: the engine's own enter reaches
// the script now, so the held pair goes; false when the pair is not held
bool DropHeld(uint32_t triggerId, uint32_t presserId)
{
  if (g_heldPairs.load(std::memory_order_relaxed) == 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(g_heldMutex);
  const auto it = g_held.find(HeldKey(triggerId, presserId));
  if (it == g_held.end()) {
    return false;
  }
  g_held.erase(it);
  g_heldPairs.store(g_held.size(), std::memory_order_relaxed);
  return true;
}

// the rule turned off: nothing is blanked from then on, so nothing waits
void ClearHeld()
{
  std::lock_guard<std::mutex> lock(g_heldMutex);
  g_held.clear();
  g_heldPairs.store(0, std::memory_order_relaxed);
}

// a reference by form id (TESForm::LookupByID, CommonLibSSE-NG
// include/RE/T/TESForm.h:210; AsReference, :270), or null
RE::TESObjectREFR* ReferenceOf(uint32_t formId)
{
  RE::TESForm* form = RE::TESForm::LookupByID(formId);
  return form ? form->AsReference() : nullptr;
}
}

bool Frida::TriggerPresserRuleOn()
{
  return g_triggerPresserRule.load(std::memory_order_relaxed);
}

void Frida::SetTriggerPresserRule(bool on)
{
  g_triggerPresserRule.store(on, std::memory_order_relaxed);
  if (!on) {
    ClearHeld();
  }
  logger::info("[verb:traps] setTriggerPresserRule({}): {}", on,
               on ? "a trigger entry counts on its presser's owner alone "
                    "(D-W1)"
                  : "every presser's entry counts on every seat (D-S3)");
}

/**
 * The everyone pass (thuum ADR-073 decision 9; step 1's first commit)
 */

// thuum docs/DECISIONS.md ADR-073 decision 9 (D-L1 B, Eli 2026-11-14:
// "ADR-068 decision 6 as written: every seat whose engine raises the event
// runs it"; the design is lab/.cache/drafts/step0/step0-spec.md section
// B.5): under the block, an event to a quest's own scripts or to an alias's
// passes on every seat whose engine raises it, whatever the script's class.
// Nobody owns a quest or an alias; their consequences meet the seam at their
// own selves, and SetStage folds as the idempotent record (ADR-068 decision
// 7). The kind is the handle's VM type, asked of the VM's handle policy
// (IObjectHandlePolicy::HandleIsType, CommonLibSSE-NG
// include/RE/I/IObjectHandlePolicy.h:20; a form type is its VM type,
// src/RE/I/IObjectHandlePolicy.cpp:7-10): the quest (TESQuest::FORMTYPE,
// include/RE/T/TESQuest.h:195, 77), then the reference alias
// (BGSRefAlias::VMTYPEID, include/RE/B/BGSRefAlias.h:18, 140), the location
// alias (BGSLocAlias.h:13, 141) and the base alias (BGSBaseAlias.h:15, 139),
// in PlayerQuestOfHandle's order, so the first that holds names it.
// What the pass leaves goes on to the rules below as before, counted by its
// kind (ADR-073 decision 7): a scene (BGSScene::FORMTYPE,
// include/RE/B/BGSScene.h:36, 122), whose run and fragments are the seat's
// that started it (decisions 6 and 9 as written, D-L2 B; whether that
// reading governs is ADR-073's own Open question), an effect
// (ActiveEffect::VMTYPEID, include/RE/A/ActiveEffect.h:31, 142), which
// decision 9 gives its target's owner (PlayersEffect's pass above it stays,
// and C4's owner rule takes the rest when m4-reanimate-return lands), and any
// other handle with no self. Each kind's first event is logged with its
// handle, its first script's class and its quest, then counted, the window's
// counts flushed with the census (FlushGatePasses). The lines are the step
// 0 tally's shape (step0-spec.md section B.1), its pass table's everyone row
// before that table exists: when m4-loud-seat's P2 lands, these counts fold
// into its table and FlushGatePasses into its FlushGateTally.
// The switch (D-S0-29 B) is off unless the client setting
// scriptEveryoneEvents is true, so a stage A/Bs one build and a client that
// never sets it changes nothing. It is a list under ADR-073 decision 5 (c), a
// stopgap: decision 9's pass is the rule replacing it, and the commit that
// lands decision 11's forward flips its default and deletes its off path
// (risk H7's reason: until the table and the forward exist, a quest's or an
// alias's script run on every seat calls untabled natives on every seat, as
// a live fragment does). With it off nothing here is asked
namespace {
std::atomic<bool> g_scriptEveryoneEvents{ false };

// the kinds the pass takes, by VM type; kNone is a handle it leaves
enum class EveryoneKind : uint8_t
{
  kNone,
  kQuest,
  kRefAlias,
  kLocAlias,
  kBaseAlias
};

// the kinds it leaves to the rules below
enum class EveryoneLeft : uint8_t
{
  kScene,
  kEffect,
  kOther
};

// each kind's name in a line, its VM type number with it (the tally's names,
// step0-spec.md section B.1); index 0 is kNone's, never logged
constexpr const char* kEveryoneKindNames[] = { "none", "quest 77", "alias 140",
                                               "alias 141", "alias 139" };
constexpr const char* kEveryoneLeftNames[] = { "scene 122", "effect 142",
                                               "other" };

// the window's counts, each kind's first line claimed once per process
std::atomic<uint32_t> g_everyonePassed[std::size(kEveryoneKindNames)]{};
std::atomic<bool> g_everyonePassedSaid[std::size(kEveryoneKindNames)]{};
std::atomic<uint32_t> g_everyoneLeft[std::size(kEveryoneLeftNames)]{};
std::atomic<bool> g_everyoneLeftSaid[std::size(kEveryoneLeftNames)]{};

EveryoneKind EveryoneKindOf(RE::BSScript::IObjectHandlePolicy* policy,
                            RE::VMHandle handle)
{
  if (policy->HandleIsType(RE::TESQuest::FORMTYPE, handle)) {
    return EveryoneKind::kQuest;
  }
  if (policy->HandleIsType(RE::BGSRefAlias::VMTYPEID, handle)) {
    return EveryoneKind::kRefAlias;
  }
  if (policy->HandleIsType(RE::BGSLocAlias::VMTYPEID, handle)) {
    return EveryoneKind::kLocAlias;
  }
  if (policy->HandleIsType(RE::BGSBaseAlias::VMTYPEID, handle)) {
    return EveryoneKind::kBaseAlias;
  }
  return EveryoneKind::kNone;
}

EveryoneLeft EveryoneLeftOf(RE::BSScript::IObjectHandlePolicy* policy,
                            RE::VMHandle handle)
{
  if (policy->HandleIsType(RE::BGSScene::FORMTYPE, handle)) {
    return EveryoneLeft::kScene;
  }
  if (policy->HandleIsType(RE::ActiveEffect::VMTYPEID, handle)) {
    return EveryoneLeft::kEffect;
  }
  return EveryoneLeft::kOther;
}

// the class of the handle's first attached script, "no script" for none
// and "no class read" for a script whose type names nothing, so a line never
// prints an empty field: the VM's attachedScripts under its own lock
// (HasAttachedScript's read), on a first line alone. m4-reanimate-return's
// FirstScriptClass is the same read; when C4 merges one of the two goes
std::string FirstAttachedClass(VM* vm, RE::VMHandle handle)
{
  if (!vm) {
    return std::string("no script");
  }
  RE::BSSpinLockGuard guard{ vm->attachedScriptsLock };
  const auto it = vm->attachedScripts.find(handle);
  if (it == vm->attachedScripts.end() || it->second.size() == 0) {
    return std::string("no script");
  }
  const auto* script = it->second[0].get();
  const auto* info = script ? script->GetTypeInfo() : nullptr;
  const char* name = info ? info->GetName() : nullptr;
  return name ? std::string(name) : std::string("no class read");
}

// a passed kind's quest for its first line: the quest itself, or an alias's
// owning quest and its alias id (BGSBaseAlias::owningQuest and aliasID,
// include/RE/B/BGSBaseAlias.h:76-77), PlayerQuestOfHandle's reads
std::string EveryoneQuestOf(RE::BSScript::IObjectHandlePolicy* policy,
                            RE::VMHandle handle, EveryoneKind kind)
{
  if (kind == EveryoneKind::kQuest) {
    const RE::TESForm* quest =
      policy->GetObjectForHandle(RE::TESQuest::FORMTYPE, handle);
    return quest ? fmt::format("quest {:x}", quest->GetFormID())
                 : std::string("no quest read");
  }
  const RE::VMTypeID type = kind == EveryoneKind::kRefAlias
    ? RE::BGSRefAlias::VMTYPEID
    : kind == EveryoneKind::kLocAlias ? RE::BGSLocAlias::VMTYPEID
                                      : RE::BGSBaseAlias::VMTYPEID;
  const auto* alias = static_cast<const RE::BGSBaseAlias*>(
    policy->GetObjectForHandle(type, handle));
  if (!alias || !alias->owningQuest) {
    return "no quest read";
  }
  return fmt::format("quest {:x} alias {}", alias->owningQuest->GetFormID(),
                     alias->aliasID);
}

// the pass itself (OnSendEventEnter, after PlayersEffect's pass and before
// rung 5's): true for a quest's or an alias's handle, counted; a handle it
// leaves is counted by its kind and goes on through the gate
bool EveryonePasses(VM* vm, RE::BSScript::IObjectHandlePolicy* policy,
                    RE::VMHandle handle, const std::string& eventName)
{
  const EveryoneKind kind = EveryoneKindOf(policy, handle);
  if (kind != EveryoneKind::kNone) {
    const auto i = static_cast<size_t>(kind);
    g_everyonePassed[i].fetch_add(1, std::memory_order_relaxed);
    if (!g_everyonePassedSaid[i].exchange(true, std::memory_order_relaxed)) {
      logger::info("[census] k9-gate-passes passed: everyone {}; {} to handle "
                   "{:x}, {}, {} (first of this kind)",
                   kEveryoneKindNames[i], eventName, handle,
                   FirstAttachedClass(vm, handle),
                   EveryoneQuestOf(policy, handle, kind));
    }
    return true;
  }
  const auto left = static_cast<size_t>(EveryoneLeftOf(policy, handle));
  g_everyoneLeft[left].fetch_add(1, std::memory_order_relaxed);
  if (!g_everyoneLeftSaid[left].exchange(true, std::memory_order_relaxed)) {
    logger::info("[fall] k9-everyone-pass skipped: {}; {} to handle {:x}, {} "
                 "(first of this kind)",
                 kEveryoneLeftNames[left], eventName, handle,
                 FirstAttachedClass(vm, handle));
  }
  return false;
}

/**
 * The platform pass (thuum D-S0-14 B)
 */

// thuum docs/DECISIONS.md D-S0-14 B (Eli 2026-11-14 19:19:35Z, step 0's
// recommendations; the dated note under it, read 1's control
// x-golden-claw-alias v1.1 20261115-040058): an event raised inside the
// platform's own call passes the gate (OnSendEventEnter), told by the seam's
// one mark, PlatformCall::Active() (PlatformCall.h), a thread-local depth
// raised only on the thread that runs the call: the worker's Scope around a
// native's body (SkyrimPlatform.cpp, RunInIOContext), callNative's Scope
// around a direct addItem or removeItem on the JavaScript thread
// (CallNativeApi.cpp), a latent call's extent on its Papyrus thread
// (ScriptNatives.cpp's Call hook) and ScriptState's sends. It replaces
// callNative's Override, a process-wide flag: every thread's event passed
// while any non-latent call was open, so a world event on another thread
// passed or was judged by when it landed (read 1's H1a), and the first of
// two overlapping calls to end cleared it while the second still ran (R11).
// HYPOTHESIS (read 1's control): the engine raises the call's own events
// synchronously inside the call, on the call's thread (RemoveItem's
// OnContainerChanged on the JavaScript thread); an event the engine queues
// to another thread meets the rules below, as any world event does.
// The pass is a census (ADR-073 decision 7's shape, step0-spec.md section
// B.1's platform row): counted by the handle's kind, asked of the VM's
// handle policy alone (IObjectHandlePolicy::HandleIsType, CommonLibSSE-NG
// include/RE/I/IObjectHandlePolicy.h:20): the everyone pass's quest and
// aliases, then its scene and effect, then the actor
// (FormType::ActorCharacter 0x3E, include/RE/F/FormTypes.h:202) before the
// reference (FormType::Reference 0x3D, :201), so an actor's handle names the
// actor whichever HandleIsType answers for 0x3D on it, then any other. Each
// kind's first is logged with its event, handle, first script's class, self
// and thread, then counted, the window's counts flushed with the everyone
// row (FlushGatePasses)
enum class PlatformKind : uint8_t
{
  kQuest,
  kRefAlias,
  kLocAlias,
  kBaseAlias,
  kScene,
  kEffect,
  kActor,
  kReference,
  kOther
};

// each kind's name with its VM type number (the tally's names, B.1)
constexpr const char* kPlatformKindNames[] = {
  "quest 77",   "alias 140", "alias 141",    "alias 139", "scene 122",
  "effect 142", "actor 62",  "reference 61", "other"
};

std::atomic<uint32_t> g_platformPassed[std::size(kPlatformKindNames)]{};
std::atomic<bool> g_platformPassedSaid[std::size(kPlatformKindNames)]{};

PlatformKind PlatformKindOf(RE::BSScript::IObjectHandlePolicy* policy,
                            RE::VMHandle handle)
{
  if (!policy) {
    return PlatformKind::kOther;
  }
  switch (EveryoneKindOf(policy, handle)) {
    case EveryoneKind::kQuest:
      return PlatformKind::kQuest;
    case EveryoneKind::kRefAlias:
      return PlatformKind::kRefAlias;
    case EveryoneKind::kLocAlias:
      return PlatformKind::kLocAlias;
    case EveryoneKind::kBaseAlias:
      return PlatformKind::kBaseAlias;
    case EveryoneKind::kNone:
      break;
  }
  switch (EveryoneLeftOf(policy, handle)) {
    case EveryoneLeft::kScene:
      return PlatformKind::kScene;
    case EveryoneLeft::kEffect:
      return PlatformKind::kEffect;
    case EveryoneLeft::kOther:
      break;
  }
  if (policy->HandleIsType(RE::FormType::ActorCharacter, handle)) {
    return PlatformKind::kActor;
  }
  if (policy->HandleIsType(RE::FormType::Reference, handle)) {
    return PlatformKind::kReference;
  }
  return PlatformKind::kOther;
}

// a kind's self for its first line: a quest's or an alias's quest
// (EveryoneQuestOf), an actor's or a reference's form, nothing for the rest
std::string PlatformSelfOf(RE::BSScript::IObjectHandlePolicy* policy,
                           RE::VMHandle handle, PlatformKind kind)
{
  if (!policy) {
    return std::string("no self read");
  }
  switch (kind) {
    case PlatformKind::kQuest:
      return EveryoneQuestOf(policy, handle, EveryoneKind::kQuest);
    case PlatformKind::kRefAlias:
      return EveryoneQuestOf(policy, handle, EveryoneKind::kRefAlias);
    case PlatformKind::kLocAlias:
      return EveryoneQuestOf(policy, handle, EveryoneKind::kLocAlias);
    case PlatformKind::kBaseAlias:
      return EveryoneQuestOf(policy, handle, EveryoneKind::kBaseAlias);
    case PlatformKind::kActor:
    case PlatformKind::kReference: {
      const RE::TESForm* self = policy->GetObjectForHandle(
        kind == PlatformKind::kActor ? RE::FormType::ActorCharacter
                                     : RE::FormType::Reference,
        handle);
      return self ? fmt::format("self {:x}", self->GetFormID())
                  : std::string("no self read");
    }
    default:
      return std::string("no self read");
  }
}

// the pass's count (OnSendEventEnter, after the record-death and
// record-apply blanks, the gate's first pass)
void CountPlatformPass(RE::VMHandle handle, const char* eventName)
{
  auto* vm = VM::GetSingleton();
  auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
  const PlatformKind kind = PlatformKindOf(policy, handle);
  const auto i = static_cast<size_t>(kind);
  g_platformPassed[i].fetch_add(1, std::memory_order_relaxed);
  if (!g_platformPassedSaid[i].exchange(true, std::memory_order_relaxed)) {
    logger::info("[census] k9-gate-passes passed: platform {}; {} to handle "
                 "{:x}, {}, {}, thread {} (first of this kind)",
                 kPlatformKindNames[i], eventName ? eventName : "", handle,
                 FirstAttachedClass(vm, handle),
                 PlatformSelfOf(policy, handle, kind), GetCurrentThreadId());
  }
}
}

void Frida::SetScriptEveryoneEvents(bool on)
{
  g_scriptEveryoneEvents.store(on, std::memory_order_relaxed);
}

void Frida::FlushGatePasses(uint64_t windowMs)
{
  const uint64_t seconds = windowMs / 1000;
  for (size_t i = 0; i < std::size(g_platformPassed); ++i) {
    const uint32_t n =
      g_platformPassed[i].exchange(0, std::memory_order_relaxed);
    if (n != 0) {
      logger::info("[census] k9-gate-passes passed in {} s: platform {} x{}",
                   seconds, kPlatformKindNames[i], n);
    }
  }
  for (size_t i = 1; i < std::size(g_everyonePassed); ++i) {
    const uint32_t n =
      g_everyonePassed[i].exchange(0, std::memory_order_relaxed);
    if (n != 0) {
      logger::info("[census] k9-gate-passes passed in {} s: everyone {} x{}",
                   seconds, kEveryoneKindNames[i], n);
    }
  }
  for (size_t i = 0; i < std::size(g_everyoneLeft); ++i) {
    const uint32_t n =
      g_everyoneLeft[i].exchange(0, std::memory_order_relaxed);
    if (n != 0) {
      logger::info("[fall] k9-everyone-pass skipped in {} s: {} x{}", seconds,
                   kEveryoneLeftNames[i], n);
    }
  }
}

namespace {
// ADR-068 D-W3 B (the held entries, above): TESModPlatform.ReplayHeldEntries'
// body, on the worker inside callNative, under its PlatformCall::Scope
// (SkyrimPlatform.cpp), the platform's mark the gate passes on this thread
// (the platform pass, above). The expired pairs go first, each with its
// line. Then a pair is due when its presser is this seat's by the gate's own
// rule (OwnerOf: the client's hosting state as the client told it, through
// setHosted, setPuppet and SetCellHosted) and its trigger stands in a cell
// this seat hosts (the trigger's script is the cell host's, ADR-062 decision
// 1); the reads run outside the store's lock (OwnerOf takes the hosted, the
// puppet and the cell sets' own). Each due pair is taken under the lock (a
// leave that dropped it meanwhile wins) and its enter replayed once, outside
// the lock, through SendTriggerEvent's body (ScriptState::SendTrigger:
// SendEvent, then RelayEvent to the trigger's aliases, as the engine's own
// trigger sinks do), whose send re-enters the gate. A JSON string:
// replayed, refused, expired, held
std::string ReplayHeldEntriesImpl()
{
  if (g_heldPairs.load(std::memory_order_relaxed) == 0) {
    return "{\"replayed\":0,\"refused\":0,\"expired\":0,\"held\":0}";
  }
  struct Pair
  {
    uint32_t trigger = 0;
    uint32_t presser = 0;
    uint64_t firstMs = 0;
  };
  const uint64_t now = NowMs();
  std::vector<Pair> live;
  std::vector<Pair> expired;
  {
    std::lock_guard<std::mutex> lock(g_heldMutex);
    for (auto it = g_held.begin(); it != g_held.end();) {
      Pair pair;
      pair.trigger = static_cast<uint32_t>(it->first >> 32);
      pair.presser = static_cast<uint32_t>(it->first & 0xffffffffu);
      pair.firstMs = it->second.firstMs;
      if (now >= it->second.firstMs + kHeldEntryMs) {
        expired.push_back(pair);
        it = g_held.erase(it);
      } else {
        live.push_back(pair);
        ++it;
      }
    }
    g_heldPairs.store(g_held.size(), std::memory_order_relaxed);
  }
  for (const Pair& pair : expired) {
    if (TriggerLineDue(TriggerLine::kHeldExpired, pair.trigger)) {
      logger::info("[verb:traps] OnTriggerEnter to {:x} by {:x}: held at the "
                   "load, dropped after {} s with no owner here (D-W3 B)",
                   pair.trigger, pair.presser, (now - pair.firstMs) / 1000);
    }
  }
  // a pair whose trigger or presser was disabled since, or whose presser
  // died or unloaded since, is never due and expires (TESObjectREFR::
  // IsDisabled, IsDead and Is3DLoaded, CommonLibSSE-NG
  // include/RE/T/TESObjectREFR.h:442, :353, :435): the replay is a live
  // NPC's standing inside, the engine's own entry
  std::vector<Pair> due;
  for (const Pair& pair : live) {
    RE::TESObjectREFR* trigger = ReferenceOf(pair.trigger);
    RE::TESObjectREFR* presser = ReferenceOf(pair.presser);
    if (!trigger || !presser || trigger->IsDisabled() ||
        presser->IsDisabled() || presser->IsDead() || !presser->Is3DLoaded()) {
      continue;
    }
    const RE::TESObjectCELL* cell = trigger->GetParentCell();
    if (!cell || !Frida::IsCellHosted(cell->GetFormID())) {
      continue;
    }
    if (OwnerOf(presser) != CauseOwner::kThisSeat) {
      continue;
    }
    due.push_back(pair);
  }
  std::vector<Pair> taken;
  if (!due.empty()) {
    std::lock_guard<std::mutex> lock(g_heldMutex);
    for (const Pair& pair : due) {
      const auto it = g_held.find(HeldKey(pair.trigger, pair.presser));
      if (it == g_held.end()) {
        continue;
      }
      taken.push_back(pair);
      g_held.erase(it);
    }
    g_heldPairs.store(g_held.size(), std::memory_order_relaxed);
  }
  size_t replayed = 0;
  size_t refused = 0;
  for (const Pair& pair : taken) {
    const std::string answer = ScriptState::SendTrigger(
      ReferenceOf(pair.trigger), "OnTriggerEnter", ReferenceOf(pair.presser));
    // the answer read as ScriptNatives.cpp reads its settings (find, then
    // the kind), nothing thrown on a bad one
    const auto parsed = nlohmann::json::parse(answer, nullptr, false);
    bool sent = false;
    std::string reason = "no answer";
    if (parsed.is_object()) {
      const auto sentIt = parsed.find("sent");
      sent =
        sentIt != parsed.end() && sentIt->is_boolean() && sentIt->get<bool>();
      const auto reasonIt = parsed.find("reason");
      if (reasonIt != parsed.end() && reasonIt->is_string()) {
        reason = reasonIt->get<std::string>();
      }
    }
    if (sent) {
      ++replayed;
      logger::info("[verb:traps] OnTriggerEnter to {:x} by {:x}: held at the "
                   "load (no owner then), replayed at its owner's grant",
                   pair.trigger, pair.presser);
      continue;
    }
    ++refused;
    logger::info("[verb:traps] OnTriggerEnter to {:x} by {:x}: held at the "
                 "load (no owner then), not replayed at its owner's grant: {}",
                 pair.trigger, pair.presser, reason);
  }
  return fmt::format(
    "{{\"replayed\":{},\"refused\":{},\"expired\":{},\"held\":{}}}", replayed,
    refused, expired.size(), g_heldPairs.load(std::memory_order_relaxed));
}
}

std::string Frida::ReplayHeldEntries()
{
  try {
    return ReplayHeldEntriesImpl();
  } catch (const std::exception& e) {
    try {
      logger::error("[verb:traps] ReplayHeldEntries threw: {}", e.what());
    } catch (...) {
    }
  } catch (...) {
    try {
      logger::error("[verb:traps] ReplayHeldEntries threw");
    } catch (...) {
    }
  }
  return "{\"replayed\":0,\"refused\":0,\"expired\":0,\"held\":0,"
         "\"reason\":\"threw\"}";
}

// the gate's own test of an effect event's handle (OnSendEventEnter below,
// ADR-031's push), moved here so the seam asks it too (FridaHooks.h; thuum
// train 52's Fix 2): ActiveEffect's VM type id, its caster and its target
// (CommonLibSSE-NG include/RE/A/ActiveEffect.h:31, :100 and :104). The
// target is the actor's MagicTarget base (+0xA0 on 1.6.1170, measured), so
// it is compared with the player's AsMagicTarget (include/RE/A/Actor.h:720),
// never through ActiveEffect::GetTargetActor, whose reinterpret_cast skips
// that base; the caster is a handle (GetCasterActor,
// src/RE/A/ActiveEffect.cpp:25-28). The target first, as the gate tested it
Frida::PlayersEffectRole Frida::PlayersEffect(
  RE::BSScript::IObjectHandlePolicy* policy, RE::VMHandle handle)
{
  if (!policy || !policy->HandleIsType(RE::ActiveEffect::VMTYPEID, handle)) {
    return PlayersEffectRole::kNone;
  }
  auto effect = static_cast<RE::ActiveEffect*>(
    policy->GetObjectForHandle(RE::ActiveEffect::VMTYPEID, handle));
  auto player = RE::PlayerCharacter::GetSingleton();
  if (!effect || !player) {
    return PlayersEffectRole::kNone;
  }
  if (effect->target == player->AsMagicTarget()) {
    return PlayersEffectRole::kTarget;
  }
  if (effect->GetCasterActor().get() == player) {
    return PlayersEffectRole::kCaster;
  }
  return PlayersEffectRole::kNone;
}

/**
 * The owner rule (the Undead FX build; thuum ADR-068 decision 6 applied to an
 * actor's aliases and effects, the reanimate-fx memo's D5 B)
 */

// Under the block an alias's and an effect's scripts ran only for the
// player's own (the player quests, ADR-031's effects) or an admitted class,
// so Undead FX's applicators (their aliases' OnInit adds the decay's
// ability) and the decay's own effect script ran on no seat (the memo's
// 6.3). The rule is ownership, no name list (ADR-063, ADR-068): an event to a
// World quest's reference alias passes on the seat that owns the alias's
// reference, an actor's simulator or a world object's cell host; an event to
// an active effect on an actor that is not the player passes on the seat that
// simulates the effect's target. A per-player quest's alias keeps rung 5's
// branch; the player's own actor and everything at or above 0xff000000 (a
// figure, what a seat places) keep ADR-044's rules, as the gate's own
// simulatedActor test does; an effect another seat's player cast (a caster at
// or above 0xff000000 this seat does not simulate) stays its caster's seat's,
// decision 6's "the player's effects are the player's seat's", so one engine
// runs it. Every event the rule passes is tallied by its branch and its
// script's class (the first stage measures the breadth before the build
// ships)
namespace {
// the rule's switch (Frida::SetScriptOwnerEvents), on until the client's
// settings turn it off; off, the rule asks nothing, no engine call among it,
// and the gate is the block's as before (the lab's A/B and its lever)
std::atomic<bool> g_scriptOwnerEvents{ true };

// which branch of the rule names an event: an alias's or an effect's
enum class OwnerBranch : uint8_t
{
  kAlias,
  kEffect
};

const char* OwnerBranchName(OwnerBranch branch)
{
  return branch == OwnerBranch::kAlias ? "alias" : "effect";
}

// an event the rule names: its branch, the alias's reference or the effect's
// target, and for an alias its quest
struct OwnedEvent
{
  OwnerBranch branch = OwnerBranch::kAlias;
  uint32_t ref = 0;
  uint32_t quest = 0;
};

// the rule's owner of a reference here, the gate's own tests: a live or dead
// actor simulated here (Hosted.h, no puppet, Puppets.h: simulatedActor's), a
// world object in a cell this seat hosts (Frida::IsCellHosted: inHostedCell's
// set); never this seat's own player or a reference at or above 0xff000000
// (ownedByAPlayer's ids)
bool OwnedReference(const RE::TESObjectREFR* ref)
{
  const uint32_t id = ref->formID;
  if (id == 0x14 || id >= 0xff000000) {
    return false;
  }
  if (ref->GetFormType() == RE::FormType::ActorCharacter) {
    return Hosted::SimulatedHere(id);
  }
  const RE::TESObjectCELL* cell = ref->GetParentCell();
  return cell && Frida::IsCellHosted(cell->GetFormID());
}

// the owner of a container an item moved into, the alias branch's
// OnContainerChanged (thuum #55): this seat's own player; an actor this
// seat simulates (Hosted.h, no puppet, the gate's simulatedActor test, a
// figure never); any other container, its cell's host
// (Frida::IsCellHosted). The player's take from a body or a chest is its own
// engine's move, so the take's event is the taker's
bool ContainerOwnedHere(const RE::TESObjectREFR* container)
{
  const uint32_t id = container->formID;
  if (id == 0x14) {
    return true;
  }
  if (container->GetFormType() == RE::FormType::ActorCharacter) {
    return Hosted::SimulatedHere(id);
  }
  const RE::TESObjectCELL* cell = container->GetParentCell();
  return cell && Frida::IsCellHosted(cell->GetFormID());
}

// the reference an event's first argument names, read through CommonLib's
// own interface (IFunctionArguments' pack, include/RE/I/
// IFunctionArguments.h:20, into a BSScrapArray of Variables, its size read
// and never empty(): BSTArray.h:749 answers inverted) and resolved as
// SelfIdOfHandle resolves a handle (the actor, then the reference); null for
// no argument, a None, or a value that is no reference
RE::TESObjectREFR* FirstRefArgument(RE::BSScript::IObjectHandlePolicy* policy,
                                    const void* argsPtr)
{
  if (!argsPtr) {
    return nullptr;
  }
  const auto* args =
    static_cast<const RE::BSScript::IFunctionArguments*>(argsPtr);
  RE::BSScrapArray<RE::BSScript::Variable> vars;
  if (!(*args)(vars) || vars.size() == 0 || !vars[0].IsObject()) {
    return nullptr;
  }
  const auto object = vars[0].GetObject();
  if (!object) {
    return nullptr;
  }
  const RE::VMHandle h = object->GetHandle();
  if (RE::TESForm* actor =
        policy->GetObjectForHandle(RE::FormType::ActorCharacter, h)) {
    return actor->As<RE::TESObjectREFR>();
  }
  if (RE::TESForm* refr =
        policy->GetObjectForHandle(RE::FormType::Reference, h)) {
    return refr->As<RE::TESObjectREFR>();
  }
  return nullptr;
}

// the event the rule names, if any, for a handle that names no reference:
// - an event to a quest's reference alias (BGSRefAlias::VMTYPEID,
//   BGSRefAlias.h:18; thuum #55, ADR-073 decision 6 as this rule's header
//   gives it): it passes on the seat that owns the alias's reference
//   (BGSRefAlias::GetReference, BGSRefAlias.h:149), an actor this seat
//   simulates or a world object in a cell it hosts (OwnedReference). An
//   OnContainerChanged is an item's move, and its owner is the new
//   container's (its first argument, ContainerOwnedHere), so the Golden
//   Claw's alias script (MS13's alias 11, a runtime reference in the
//   engine's inventory) runs on the seat whose player took it; a move with
//   no new container (a drop into the world) asks the reference itself.
//   A per-player quest's alias passed rung 5's branch above. HYPOTHESIS
//   until x-gate-owner's leg B: the take raises the alias's
//   OnContainerChanged through this hook with the player as its first
//   argument (x-golden-claw-alias v1 read the script run, 20261107-222959);
// - an active effect (ActiveEffect::VMTYPEID, ActiveEffect.h:31) whose
//   target (ActiveEffect.h:104, read as a reference through its
//   MagicTarget's own GetTargetStatsObject, MagicTarget.h:79, which an
//   actor answers with itself, Actor.h:354; the platform's idiom,
//   EventHandler.cpp's spell cast) is an actor this seat simulates, and
//   whose caster (GetCasterActor, ActiveEffect.h:93) is none, a game-file
//   actor, or one at or above 0xff000000 this seat simulates.
// An alias's OnInit never passes SendEvent (the VM dispatches it at the
// object's birth, 104875; docs/research/adr065-papyrus-state-2026-11-03.md
// Q4), so the applicators run on every seat and the seam keeps their
// AddSpell on the owner (x-reanimate-look-u0 20261112-085636).
std::optional<OwnedEvent> OwnedEventOf(
  RE::BSScript::IObjectHandlePolicy* policy, RE::VMHandle handle,
  const std::string& eventName, const void* args)
{
  if (policy->HandleIsType(RE::BGSRefAlias::VMTYPEID, handle)) {
    const auto* alias = static_cast<const RE::BGSRefAlias*>(
      policy->GetObjectForHandle(RE::BGSRefAlias::VMTYPEID, handle));
    if (!alias) {
      return std::nullopt;
    }
    const uint32_t quest =
      alias->owningQuest ? alias->owningQuest->GetFormID() : 0;
    RE::TESObjectREFR* ref = alias->GetReference();
    const uint32_t refId = ref ? ref->formID : 0;
    if (_stricmp(eventName.c_str(), "OnContainerChanged") == 0) {
      if (const RE::TESObjectREFR* into = FirstRefArgument(policy, args)) {
        return ContainerOwnedHere(into)
          ? std::optional<OwnedEvent>(
              OwnedEvent{ OwnerBranch::kAlias, refId, quest })
          : std::nullopt;
      }
    }
    return ref && OwnedReference(ref) ? std::optional<OwnedEvent>(OwnedEvent{
                                          OwnerBranch::kAlias, refId, quest })
                                      : std::nullopt;
  }
  if (policy->HandleIsType(RE::ActiveEffect::VMTYPEID, handle)) {
    auto* effect = static_cast<RE::ActiveEffect*>(
      policy->GetObjectForHandle(RE::ActiveEffect::VMTYPEID, handle));
    RE::MagicTarget* target = effect ? effect->target : nullptr;
    const RE::TESObjectREFR* ref =
      target ? target->GetTargetStatsObject() : nullptr;
    if (!ref || ref->GetFormType() != RE::FormType::ActorCharacter ||
        !OwnedReference(ref)) {
      return std::nullopt;
    }
    const RE::NiPointer<RE::Actor> caster = effect->GetCasterActor();
    if (caster && caster->formID >= 0xff000000 &&
        !Hosted::SimulatedHere(caster->formID)) {
      return std::nullopt;
    }
    return OwnedEvent{ OwnerBranch::kEffect, ref->formID, 0 };
  }
  return std::nullopt;
}

// the class of the handle's first attached script (the VM's attachedScripts
// under its own lock, HasAttachedScript's read), empty for none
std::string FirstScriptClass(VM* vm, RE::VMHandle handle)
{
  if (!vm) {
    return {};
  }
  RE::BSSpinLockGuard guard{ vm->attachedScriptsLock };
  const auto it = vm->attachedScripts.find(handle);
  if (it == vm->attachedScripts.end() || it->second.size() == 0) {
    return {};
  }
  const auto* script = it->second[0].get();
  const auto* info = script ? script->GetTypeInfo() : nullptr;
  return info ? std::string(info->GetName()) : std::string();
}

// the rule's tally: each event the rule passes counted by its key, "<branch>
// <class>", in windows of kOwnerTallyMs, at most kOwnerTallyClasses keys a
// window and the rest as "others"; a window's lines are logged when the
// first event after it ends is tallied (a quiet seat's last window waits for
// its next one); and each key's first event is logged once, the keys said
// kept to kOwnerTallySaid
constexpr uint64_t kOwnerTallyMs = 30'000;
constexpr size_t kOwnerTallyClasses = 64;
constexpr size_t kOwnerTallySaid = 1024;

struct
{
  std::mutex m;
  uint64_t windowMs = 0;
  std::unordered_map<std::string, uint32_t> counts;
  uint32_t others = 0;
  uint32_t aliases = 0;
  uint32_t effects = 0;
  std::unordered_set<std::string> said;
} g_ownerTally;

void TallyOwnedEvent(const OwnedEvent& owned, const std::string& cls,
                     const std::string& eventName, RE::VMHandle handle)
{
  const uint64_t now = NowMs();
  const std::string key =
    fmt::format("{} {}", OwnerBranchName(owned.branch), cls);
  std::vector<std::string> window;
  bool first = false;
  {
    std::lock_guard<std::mutex> lock(g_ownerTally.m);
    if (g_ownerTally.windowMs == 0) {
      g_ownerTally.windowMs = now;
    }
    if (now - g_ownerTally.windowMs >= kOwnerTallyMs) {
      const uint64_t seconds = (now - g_ownerTally.windowMs) / 1000;
      window.push_back(fmt::format(
        "[verb:papyrus-events] the owner rule passed {} event(s) in {} s: {} "
        "to world quests' aliases, {} to actors' effects, {} key(s) (D5 B)",
        g_ownerTally.aliases + g_ownerTally.effects, seconds,
        g_ownerTally.aliases, g_ownerTally.effects,
        g_ownerTally.counts.size()));
      for (const auto& [name, count] : g_ownerTally.counts) {
        window.push_back(
          fmt::format("[verb:papyrus-events] owner rule tally: {} {} in {} s",
                      name, count, seconds));
      }
      if (g_ownerTally.others != 0) {
        window.push_back(
          fmt::format("[verb:papyrus-events] owner rule tally: others {} in "
                      "{} s",
                      g_ownerTally.others, seconds));
      }
      g_ownerTally.counts.clear();
      g_ownerTally.others = 0;
      g_ownerTally.aliases = 0;
      g_ownerTally.effects = 0;
      g_ownerTally.windowMs = now;
    }
    if (const auto it = g_ownerTally.counts.find(key);
        it != g_ownerTally.counts.end()) {
      ++it->second;
    } else if (g_ownerTally.counts.size() < kOwnerTallyClasses) {
      g_ownerTally.counts.emplace(key, 1);
    } else {
      ++g_ownerTally.others;
    }
    ++(owned.branch == OwnerBranch::kAlias ? g_ownerTally.aliases
                                           : g_ownerTally.effects);
    if (g_ownerTally.said.size() >= kOwnerTallySaid &&
        g_ownerTally.said.count(key) == 0) {
      g_ownerTally.said.clear();
    }
    first = g_ownerTally.said.insert(key).second;
  }
  for (const auto& line : window) {
    logger::info("{}", line);
  }
  if (!first) {
    return;
  }
  if (owned.branch == OwnerBranch::kAlias) {
    logger::info("[verb:papyrus-events] {} to handle {:x} passed (owned "
                 "alias): {}, an alias of world quest {:x} filled with {:x}, "
                 "its owner here (D5 B; the key's first event)",
                 eventName, handle, cls, owned.quest, owned.ref);
  } else {
    logger::info("[verb:papyrus-events] {} to handle {:x} passed (simulated "
                 "effect): {}, an effect on {:x}, which this seat simulates "
                 "(D5 B; the key's first event)",
                 eventName, handle, cls, owned.ref);
  }
}
}

bool Frida::ScriptOwnerEventsOn()
{
  return g_scriptOwnerEvents.load(std::memory_order_relaxed);
}

void Frida::SetScriptOwnerEvents(bool on)
{
  g_scriptOwnerEvents.store(on, std::memory_order_relaxed);
}

/**
 * Send Event hook
 */

// (VMHandle handle, const BSFixedString& eventName, IFunctionArguments* args)
void OnSendEventEnter(GumInvocationContext* ic)
{
  // Every event a death completed from the server's record raises runs no
  // script, whatever its name (ChangeForm::CompletingRecordDeath,
  // ChangeForm.h; rung 1's review, finding 2, and its own review): the actor
  // died once, where it happened, and its scripts ran there; the completion's
  // OnDeath, the relays to its aliases and effects (SkyrimVM::RelayEvent), and
  // the events its side effects raise (OnCombatStateChanged from StopCombat,
  // OnGetUp from a furniture exit) are the replay's, not the world's. The
  // re-analyst's static read: KillImmediate's death event reaches this hook on
  // its own thread, inside the call (HYPOTHESIS until x-corpse-record's leg
  // R).
  if (ChangeForm::CompletingRecordDeath()) {
    static std::atomic<uint64_t> s_recordDeathBlanked{ 0 };
    const auto n = ++s_recordDeathBlanked;
    if (n <= 3 || n % 100 == 0) {
      auto name = (char**)gum_invocation_context_get_nth_argument(ic, 2);
      logger::info("[verb:scripts] {} blanked: raised by a death completed "
                   "from the record, not a world death ({} so far)",
                   (name && *name) ? *name : "", n);
    }
    static const auto fsEmptyRecordDeath = new FixedString("");
    gum_invocation_context_replace_nth_argument(ic, 2, fsEmptyRecordDeath);
    return;
  }
  // thuum docs/DECISIONS.md ADR-068 D-S3 point 3 (traps follow the
  // interaction; TrapFamily.h): every event raised to a reference whose
  // server record skymp5-client is applying (setRecordApplying) runs no script
  // either, whatever its name and inside the platform's own calls too, so the
  // test comes before the platform pass: the record is the world's past, its
  // consequences ran where they happened, and a late joiner's oil lamp must
  // not replay its fall. Like the record-death blank it comes before the
  // sendPapyrusEvent hook below, so JS hears none of these events. The set is
  // empty outside an apply, so the cost is one atomic read; the event's self
  // is resolved only while a record is being applied
  if (TrapFamily::RecordApplying::Any()) {
    const auto applyingId = SelfIdOfHandle(
      (RE::VMHandle)gum_invocation_context_get_nth_argument(ic, 1));
    if (applyingId != 0 && TrapFamily::RecordApplying::Is(applyingId)) {
      static std::atomic<uint64_t> s_recordApplyBlanked{ 0 };
      const auto n = ++s_recordApplyBlanked;
      if (n <= 3 || n % 100 == 0) {
        auto name = (char**)gum_invocation_context_get_nth_argument(ic, 2);
        logger::info("[verb:traps] {} to {:x} blanked: raised while the "
                     "server's record is applied to it, not a world event "
                     "({} so far)",
                     (name && *name) ? *name : "", applyingId, n);
      }
      static const auto fsEmptyRecordApply = new FixedString("");
      gum_invocation_context_replace_nth_argument(ic, 2, fsEmptyRecordApply);
      return;
    }
  }
  // thuum D-S0-14 B (the platform pass, above): an event raised inside the
  // platform's own call, on the thread that runs it, passes, counted by its
  // kind; a lifecycle event passed here was served all the same (thuum
  // ADR-068 rung 3's seen set, ScriptState.h)
  if (PlatformCall::Active()) {
    const auto platformHandle =
      (RE::VMHandle)gum_invocation_context_get_nth_argument(ic, 1);
    auto platformName = (char**)gum_invocation_context_get_nth_argument(ic, 2);
    const char* platformEvent = platformName ? *platformName : nullptr;
    CountPlatformPass(platformHandle, platformEvent);
    ScriptState::Seen::OnEvent(platformHandle, platformEvent, true);
    return;
  }
  auto handle = (RE::VMHandle)gum_invocation_context_get_nth_argument(ic, 1);
  auto eventName = (char**)gum_invocation_context_get_nth_argument(ic, 2);

  // thuum docs/DECISIONS.md ADR-068 amendment A (rung 3, the script record;
  // ScriptState.h): an OnLoad or an OnCellLoad raised to a reference whose
  // served script record waits for its apply (setScriptRecordPending,
  // raised by skymp5-client only for a record that carries its view) runs
  // no script, so the copy's own load handler does not run before the
  // record's state lands (defaultPuzzlePillarScript's OnCellLoad turns the
  // graph from the script's first side, then sets doOnce: the plan's first
  // read). The name is kept for the apply's answer (blanked) and
  // skymp5-client sends it again through SendScriptEvent once the record is
  // written. After the platform pass, so it is never asked inside a
  // platform call and the platform's own resend passes. The client's
  // setting scriptRecord false sets no mark, so the blank is off with it;
  // the set is empty outside a pending apply, so the cost is one atomic
  // read. Like the record-apply blank above, it comes before the
  // sendPapyrusEvent hook, so JS hears none of these events
  if (ScriptState::Pending::Any() && eventName && *eventName &&
      ScriptState::Pending::IsBlankedName(*eventName)) {
    const auto pendingId = SelfIdOfHandle(handle);
    if (pendingId != 0 && ScriptState::Pending::Is(pendingId)) {
      ScriptState::Pending::NoteBlanked(pendingId, *eventName);
      logger::info("[verb:scripts] {} to {:x} blanked: its script record "
                   "pending",
                   *eventName, pendingId);
      static const auto fsEmptyPending = new FixedString("");
      gum_invocation_context_replace_nth_argument(ic, 2, fsEmptyPending);
      return;
    }
  }

  auto vm = VM::GetSingleton();

  uint32_t selfId = 0;
  // the cell the event's reference stands in (ADR-059 decision 1's gate,
  // ADR-062 decision 1's rule); a quest's or an effect's event has none
  uint32_t selfCell = 0;
  // the self is an actor (ADR-062: an actor's script runs where the actor
  // is simulated, so a hosted live actor's passes and a puppet's stays
  // blanked; before ADR-062 an actor's event never saw the cell rule, the
  // spider's masterAmbushScript among them, playtest 41)
  bool selfIsActor = false;
  // the self's reference, for the trap family's destroyed test (D-S3)
  RE::TESObjectREFR* selfRef = nullptr;

  auto policy = vm->GetObjectHandlePolicy();
  if (policy) {
    if (auto actor =
          policy->GetObjectForHandle(RE::FormType::ActorCharacter, handle)) {
      selfId = actor->GetFormID();
      selfIsActor = true;
      if (auto ref = static_cast<RE::TESObjectREFR*>(actor)) {
        if (auto cell = ref->GetParentCell()) {
          selfCell = cell->GetFormID();
        }
      }
    }
    if (auto refr =
          policy->GetObjectForHandle(RE::FormType::Reference, handle)) {
      selfId = refr->GetFormID();
      if (auto ref = static_cast<RE::TESObjectREFR*>(refr)) {
        selfRef = ref;
        if (auto cell = ref->GetParentCell()) {
          selfCell = cell->GetFormID();
        }
      }
    }
  }
  bool inHostedCell = false;
  if (selfCell != 0) {
    std::lock_guard<std::mutex> lock(g_hostedCellsMutex);
    inHostedCell = g_hostedCells.count(selfCell) > 0;
  }
  // thuum docs/DECISIONS.md ADR-062 decision 1 (Eli, playtest 41: "shouldn't
  // 'scripts work' be a holistic fix?"): in a cell this game hosts every
  // WORLD OBJECT's script runs, whatever its name, and a live ACTOR's runs
  // when this game simulates the actor (Hosted.h, not a puppet); a player's
  // own actor (0x14) and the figures (0xff000000 and up) keep ADR-044's
  // rules, their scripts being a player's effects. Outside a hosted cell
  // nothing changes. The names of the layer's host rows retire with this.

  auto eventNameStr = std::string(*eventName);
  EventsApi::SendPapyrusEventEnter(selfId, eventNameStr);
  const bool lifecycle = IsLifecycleEvent(LowerAscii(eventNameStr.c_str()));
  // A world object's script: in a hosted cell every event, elsewhere the
  // lifecycle events alone (ADR-059 decision 1's reason: they come before
  // any seat hosts the cell, and a guest's pillar must stand on its initial
  // face). A live actor's script: wherever this game simulates the actor
  // (a follow's actor outside the hosted cell included); a puppet's never.
  const bool ownedByAPlayer = selfId == 0x14 || selfId >= 0xff000000;
  const bool worldObject = !selfIsActor && selfCell != 0 && !ownedByAPlayer;
  const bool simulatedActor =
    selfIsActor && !ownedByAPlayer && Hosted::SimulatedHere(selfId);

  auto blockEvents = TESModPlatform::GetPapyrusEventsBlocked();
  // the trap family's verdict, asked at most once for this event: the
  // presser rule and the family rule below both read it (TrapFamily caches
  // the verdict per class; the walk of the attached scripts under the VM's
  // lock is per call)
  std::optional<bool> familyVerdict;
  const auto isFamily = [&]() {
    if (!familyVerdict) {
      familyVerdict = TrapFamily::IsFamily(selfRef, handle);
    }
    return *familyVerdict;
  };

  // thuum docs/DECISIONS.md ADR-068 D-W1 A and D-C2 A, the presser rule
  // (above; setTriggerPresserRule, off by default): an OnTriggerEnter or an
  // OnTriggerLeave to a world object, under the block, counts on its presser's
  // owner alone. A presser this seat does not own (OwnerOf: another seat's
  // figure, a puppet, a body or an actor no seat was given in a cell this seat
  // does not host, a trap body another seat streams) is blanked here, before
  // any rule below could pass it: its owner's engine counts that entry, and
  // what it causes crosses from there (a trap family trigger's firing as its
  // first link, RelayedActivation 70; a world script's entry to the cell's
  // host as Activate's entry, played there by TESModPlatform.SendTriggerEvent,
  // which this gate passes by the platform pass above). An owned
  // entry on a trap family trigger, or on a scripted trigger in a cell this
  // seat does not host, is told to skymp5-client as triggerEntry on the next
  // tick (a tick task, never a handler's PushAndWait: the plan's read 1) and
  // then goes on through the gate as before, so a family trigger's runs here
  // and a guest's world script stays blanked; an owned entry on a hosted
  // cell's world trigger runs here and tells nobody. The family is asked in a
  // hosted cell too, its verdict cached per class. D-W3 B (the held entries,
  // above) keeps an undecided presser's enter to a scripted world trigger for
  // its owner's grant, and an owned presser's event on a pair still held
  // settles it. Trigger events alone pay for this (rule 9)
  if (blockEvents && worldObject &&
      g_triggerPresserRule.load(std::memory_order_relaxed)) {
    const int entry = TriggerEntryOf(eventNameStr.c_str());
    if (entry != 0) {
      PresserRead how = PresserRead::kNoArgument;
      RE::TESObjectREFR* presser =
        ReadPresser(gum_invocation_context_get_nth_argument(ic, 3), how);
      const CauseOwner owner = presser ? OwnerOf(presser) : CauseOwner::kOther;
      if (!presser) {
        static std::atomic<uint64_t> s_presserUnread{ 0 };
        static std::atomic<uint64_t> s_presserUnreadLineMs{ 0 };
        const auto n = ++s_presserUnread;
        const uint64_t now = NowMs();
        uint64_t last = s_presserUnreadLineMs.load(std::memory_order_relaxed);
        if ((last == 0 || now >= last + 60000) &&
            s_presserUnreadLineMs.compare_exchange_strong(last, now)) {
          logger::info("[verb:traps] {} to {:x}: {}, passed as before (the "
                       "presser rule's argument unread; {} so far, a line a "
                       "minute)",
                       eventNameStr, selfId, PresserReadText(how), n);
        }
      } else if (owner != CauseOwner::kThisSeat) {
        // D-W2 A's fallback lever: a family reference of an exempt class
        // keeps D-S3, its non-owned press passed on to the gate's rules below
        // (every seat fires its own copy)
        if (TrapFamily::Exempt::Any() && isFamily() &&
            TrapFamily::Exempt::Is(handle)) {
          static std::atomic<uint64_t> s_presserExempted{ 0 };
          const auto n = ++s_presserExempted;
          if (TriggerLineDue(TriggerLine::kExempted, selfId)) {
            logger::info("[verb:traps] {} to {:x} by {:x}: not this seat's "
                         "presser, passed: an exempt class keeps D-S3 ({} so "
                         "far)",
                         eventNameStr, selfId, presser->formID, n);
          }
        } else {
          // D-W3 B (the held entries, above): an undecided presser's enter
          // to a scripted world trigger is kept for its owner's grant, a trap
          // family trigger's is not (it waits for its next press); a leave of
          // a held pair takes one off whatever its presser's owner, the last
          // dropping the pair. Blanked below as before either way
          if (entry == 1 && owner == CauseOwner::kUndecided) {
            if (isFamily()) {
              if (TriggerLineDue(TriggerLine::kHeldFamily, selfId)) {
                logger::info("[verb:traps] {} to {:x} by {:x}: not held, a "
                             "trap family trigger waits for its next press "
                             "(D-W3 B)",
                             eventNameStr, selfId, presser->formID);
              }
            } else if (HasAttachedScript(vm, handle)) {
              uint32_t count = 0;
              size_t pairs = 0;
              if (HoldEntry(selfId, presser->formID, NowMs(), count, pairs)) {
                if (TriggerLineDue(TriggerLine::kHeld, selfId)) {
                  logger::info("[verb:traps] {} to {:x} by {:x}: held, the "
                               "presser's owner undecided here (D-W3 B; {} "
                               "for the pair, {} pair(s) held)",
                               eventNameStr, selfId, presser->formID, count,
                               pairs);
                }
              } else if (TriggerLineDue(TriggerLine::kHeld, selfId)) {
                logger::info("[verb:traps] {} to {:x} by {:x}: not held, {} "
                             "pairs held already (D-W3 B)",
                             eventNameStr, selfId, presser->formID, pairs);
              }
            }
          } else if (entry == 2) {
            bool dropped = false;
            if (LeaveHeld(selfId, presser->formID, dropped) && dropped &&
                TriggerLineDue(TriggerLine::kHeldLeft, selfId)) {
              logger::info("[verb:traps] {} to {:x} by {:x}: its held enter "
                           "dropped, the presser left before its owner's "
                           "grant (D-W3 B)",
                           eventNameStr, selfId, presser->formID);
            }
          }
          static std::atomic<uint64_t> s_presserIgnored{ 0 };
          const auto n = ++s_presserIgnored;
          if (TriggerLineDue(TriggerLine::kIgnored, selfId)) {
            logger::info("[verb:traps] {} to {:x} by {:x}: not this seat's "
                         "presser, ignored (D-W1) ({} so far)",
                         eventNameStr, selfId, presser->formID, n);
          }
          static const auto fsEmptyPresser = new FixedString("");
          gum_invocation_context_replace_nth_argument(ic, 2, fsEmptyPresser);
          return;
        }
      } else {
        // D-W3 B (the held entries, above): an owned presser's event on a
        // pair still held (its owner came between the hold and the replay's
        // update) settles it. An enter is the engine's own delivery, so the
        // held one goes and this one passes; a leave is blanked, its enter
        // never having reached the script, and takes one off the count. One
        // atomic read while nothing is held
        if (entry == 1) {
          if (DropHeld(selfId, presser->formID) &&
              TriggerLineDue(TriggerLine::kHeldSettled, selfId)) {
            logger::info("[verb:traps] {} to {:x} by {:x}: this seat's "
                         "presser, its held enter dropped, the engine's own "
                         "delivers it (D-W3 B)",
                         eventNameStr, selfId, presser->formID);
          }
        } else {
          bool dropped = false;
          if (LeaveHeld(selfId, presser->formID, dropped)) {
            if (TriggerLineDue(TriggerLine::kHeldSettled, selfId)) {
              logger::info("[verb:traps] {} to {:x} by {:x}: this seat's "
                           "presser, blanked, its enter held and never "
                           "replayed (D-W3 B)",
                           eventNameStr, selfId, presser->formID);
            }
            static const auto fsEmptyHeldLeave = new FixedString("");
            gum_invocation_context_replace_nth_argument(ic, 2,
                                                        fsEmptyHeldLeave);
            return;
          }
        }
        const bool family = isFamily();
        // ADR-068 D-W2 A (section 7.2(a)): an owned entry to a family
        // trigger, an enter or a leave, opens its firing window, in which the
        // trigger's own calls on itself are this seat's look
        // (ScriptNatives.cpp OnFamilyCall); an exempt class's opens none (D-S3
        // kept), and neither does a family reference that is no trigger
        // (TrapFamily::IsTrigger; D-W2's fresh review, note 1: the swinging
        // wall, TrapSwingingWall, counts the actors in its own volume, and its
        // PlayAnimationAndWait on itself must stay each seat's own swing)
        if (family && TrapFamily::IsTrigger(handle) &&
            !TrapFamily::Exempt::Is(handle)) {
          TrapFamily::Firing::Note(selfId, NowMs());
        }
        // the server's C-entry (wire-rules trigger_entry.rs admit, train 53)
        // relays a cell host's own entry back to it on the premise that this
        // tell fires only for an entry blanked here, never in a hosted cell
        // (hostedWorldScript below); a tell of an entry the engine also ran
        // would play twice on the host (train 53's review, rules-4)
        if (family || (!inHostedCell && HasAttachedScript(vm, handle))) {
          const uint32_t refrId = selfId;
          const uint32_t presserId = presser->formID;
          const bool enter = entry == 1;
          static std::atomic<uint64_t> s_presserTold{ 0 };
          const auto n = ++s_presserTold;
          if (TriggerLineDue(TriggerLine::kTold, refrId)) {
            logger::info("[verb:traps] {} to {:x} by {:x}: this seat's "
                         "presser, told as triggerEntry ({}) ({} so far)",
                         eventNameStr, refrId, presserId,
                         family ? "a trap family trigger"
                                : "a world trigger in a cell this seat "
                                  "does not host",
                         n);
          }
          SkyrimPlatform::GetSingleton()->AddTickTask(
            [refrId, presserId, enter, family](Napi::Env env) {
              auto obj = Napi::Object::New(env);
              AddObjProperty(&obj, "refrId", refrId);
              AddObjProperty(&obj, "presserId", presserId);
              AddObjProperty(&obj, "enter", enter);
              AddObjProperty(&obj, "family", family);
              EventsApi::SendEvent("triggerEntry", { obj });
            });
        }
      }
    }
  }

  // thuum docs/DECISIONS.md ADR-068 D-S3 point 1 (TrapFamily.h): a world
  // object whose every attached script is in the trap family runs its events
  // in every game, in any cell, as a hosted cell's world objects do: each
  // seat fires its own copy where its trigger is met (D-S2 B's cosmetic
  // copies, R3 per seat for the firing; HYPOTHESIS H-T2, the design's: an
  // unblocked plate on a seat that does not host its cell fires its children
  // there, x-trap-dodge v1), and the seam counts the reference as every
  // seat's own (ScriptNatives.cpp). Asked under the block alone, and
  // only where the answer decides something (rule 9: a hosted cell's live
  // objects cost what they did): outside a hosted cell, where it passes the
  // event, and on a destroyed reference anywhere, the spent trap below; a
  // lifecycle event passes a world object either way, so it is never asked
  // for one. The family's verdict is cached per class
  const bool trapFamily = blockEvents && worldObject && !lifecycle &&
    (!inHostedCell || TrapFamily::Destroyed(selfRef)) && isFamily();
  const bool hostedWorldScript =
    (worldObject && (inHostedCell || lifecycle || trapFamily)) ||
    simulatedActor;

  // thuum docs/verbs/shadowbond.md: a script on the never-admitted list keeps
  // its events blanked before any rule below could admit them
  if (blockEvents && vm) {
    bool never = false;
    vm->attachedScriptsLock.Lock();
    auto attached = vm->attachedScripts.find(handle);
    if (attached != vm->attachedScripts.end()) {
      auto& scripts = attached->second;
      for (size_t i = 0; i < scripts.size(); i++) {
        auto script = scripts[i].get();
        if (script && NeverAdmittedByChain(script->GetTypeInfo())) {
          never = true;
          break;
        }
      }
    }
    vm->attachedScriptsLock.Unlock();
    if (never) {
      static const auto fsEmptyNever = new FixedString("");
      gum_invocation_context_replace_nth_argument(ic, 2, fsEmptyNever);
      return;
    }
  }

  // thuum docs/DECISIONS.md ADR-068 D-S3 point 3: a spent trap goes deaf. A
  // destroyed family reference (TrapFamily::Destroyed: its script's own
  // SetDestroyed, the tripwire's, or the server's record applied) hears its
  // lifecycle events alone, so a copy whose script never fired here (a late
  // joiner's, one the record reached first) cannot fire a single-use trap
  // again; its script's state is no guide on such a copy. Its
  // OnDestructionStageChanged passes too: the engine telling it the world's
  // state, and possibly the destroying stage's own event (HYPOTHESIS H-DS1:
  // whether the engine sets the destroyed flag before it raises that stage's
  // event is unread; passed either way, the stage's handler keeps its
  // consequence, the oil pool's explosion among them, TrapExplosiveGas being
  // in the family since D-T1). JS still hears the event (the hook above);
  // trapFamily is never asked for a lifecycle event
  if (trapFamily && TrapFamily::Destroyed(selfRef) &&
      _stricmp(eventNameStr.c_str(), "OnDestructionStageChanged") != 0) {
    static std::atomic<uint64_t> s_spentBlanked{ 0 };
    const auto n = ++s_spentBlanked;
    if (n <= 3 || n % 100 == 0) {
      logger::info("[verb:traps] {} to {:x} blanked: a destroyed trap family "
                   "reference, a spent trap, hears its lifecycle alone ({} "
                   "so far)",
                   eventNameStr, selfId, n);
    }
    static const auto fsEmptySpent = new FixedString("");
    gum_invocation_context_replace_nth_argument(ic, 2, fsEmptySpent);
    return;
  }

  // thuum docs/verbs/push.md (ADR-031; ADR-029's A1, test 1): an event on
  // an effect the player bears, or one the player cast, runs in this game,
  // so a consequence a record's script carries plays where it lands.
  // Unrelenting Force's throw is one: VoicePushEffectScript's OnEffectStart
  // calls Caster.PushActorAway(Target, PushForce). Borne, it throws the
  // player in the player's own game; cast, its push onto another player's
  // figure is skipped by the push-figure hook (ScriptNatives.cpp OnStaged,
  // thuum tracker #178: the figure's throw is its player's own game's,
  // followed on the live-body stream), while its other consequences run
  // here as before. The test is Frida::PlayersEffect's (above: the player the
  // effect's target or its caster), which the seam asks too, so an effect's
  // script places its helpers wherever this gate lets it run (thuum train
  // 52's Fix 2). An effect between two figures stays blanked (A1's
  // bystanders, docs/apocalypse-options.md 6).
  if (blockEvents && policy &&
      Frida::PlayersEffect(policy, handle) !=
        Frida::PlayersEffectRole::kNone) {
    blockEvents = false;
  }

  // thuum docs/DECISIONS.md ADR-073 decision 9, the everyone pass (above,
  // beside the presser rule's switch): under its switch, an event to a
  // quest's or an alias's handle passes here on every seat whose engine
  // raises it, whatever its class, counted, before rung 5's player quests,
  // the class chain and the RaceMenu prefix below could pass it. Those three
  // stay as the off path until the switch's flip, and delete there once the
  // pass table reads them passing nothing outside the quest and alias kinds
  // over one stage and one playtest (step0-spec.md section B.5, H-GT10). An
  // actor's or a reference's event (selfId resolved above) never asks; a
  // handle the pass leaves is counted by its kind and goes on to the rules
  // below. The record, pending and never-admitted blanks, the spent trap and
  // the platform pass returned above, and PlayersEffect's pass came just
  // before, so each stays as it was. Off, nothing here is asked
  if (blockEvents && selfId == 0 && policy &&
      g_scriptEveryoneEvents.load(std::memory_order_relaxed) &&
      EveryonePasses(vm, policy, handle, eventNameStr)) {
    blockEvents = false;
  }

  // thuum docs/DECISIONS.md ADR-068 rung 5's full build (Eli's D-Q2 word,
  // "yes full transformation"; docs/research/rung5-quests-2026-11-09.md
  // 8(a); ActingScope.h, PlayerQuests): the rule is ownership, each seat
  // runs its own player's per-player quests, so an event to such a quest's
  // own scripts, or to its aliases' scripts, passes whatever their class
  // (PlayerVampireQuestScript's OnUpdateGameTime, PlayerWerewolfChangeScript's
  // timers), before the class-chain test below would blank it; the server
  // refuses such a quest's stages and words (E_QUEST_PLAYER, the plan's A4),
  // so no seat applies another player's. Counted in the gate's tally as
  // "(player quest)", the first admission per quest logged. An actor's or a
  // reference's event (selfId resolved above) never asks (rung 5's fresh
  // review, note 10), and the rest pay one atomic read while the set is
  // empty (rule 9). The effects of the disease on the player and of the
  // power the player cast pass by the branch above, as before
  if (blockEvents && selfId == 0 && policy &&
      ActingScope::PlayerQuests::Any()) {
    const uint32_t playerQuest = PlayerQuestOfHandle(policy, handle);
    if (playerQuest != 0) {
      blockEvents = false;
      static std::atomic<uint64_t> s_playerQuestPassed{ 0 };
      const auto n = ++s_playerQuestPassed;
      if (PlayerQuestLineDue(playerQuest)) {
        logger::info("[verb:quests] {} to handle {:x} passed (player quest): "
                     "a script of {:x}, this seat's own player's quest ({} "
                     "so far)",
                     eventNameStr, handle, playerQuest, n);
      }
    }
  }

  // the owner rule (above, the Undead FX build: ADR-068 decision 6 applied
  // to an actor's aliases and effects): an event to a World quest's
  // reference alias passes on the seat that owns the alias's reference, and
  // one to an active effect on an actor that is not the player on the seat
  // that simulates the effect's target, whatever the script's class, each
  // tallied by branch and class. With the rule's switch off nothing here is
  // asked. An actor's or a reference's event (selfId resolved above) never
  // asks, and a handle with no attached script runs nothing to pass. The
  // never admitted list and the spent trap returned above, so they stay
  // blanked
  if (blockEvents && selfId == 0 && policy && Frida::ScriptOwnerEventsOn()) {
    if (const auto owned =
          OwnedEventOf(policy, handle, eventNameStr,
                       gum_invocation_context_get_nth_argument(ic, 3))) {
      const std::string cls = FirstScriptClass(vm, handle);
      if (!cls.empty()) {
        TallyOwnedEvent(*owned, cls, eventNameStr, handle);
        blockEvents = false;
      }
    }
  }

  // Every event, OnUpdate included, consults the admitted scripts (thuum
  // ADR-044 decision 3): an admitted script receives its OnUpdate, the
  // rest stay blanked. Before train-29 OnUpdate skipped this lookup and
  // was blanked for every script.
  if (blockEvents && vm) {
    vm->attachedScriptsLock.Lock();
    auto it = vm->attachedScripts.find(handle);

    if (it != vm->attachedScripts.end()) {
      auto& scripts = it->second;

      for (size_t i = 0; i < scripts.size(); i++) {
        auto script = scripts[i].get();
        auto info = script->GetTypeInfo();
        auto name = info->GetName();

        if (hostedWorldScript || AdmittedByChain(info)) {
          blockEvents = false;
          break;
        } else if (!strnicmp(name, "RaceMenu", 8)) {
          // RaceMenu's menu is glued to the game by its scripts (RaceMenu,
          // RaceMenuBase, RaceMenuLoad and its slider plugins such as
          // RaceMenuMorphsCBBE): the menu's tint, hair color and body morph
          // sliders reach the actor through their mod events
          // (OnTintColorChange, OnHairColorChange and the rest). Blocked,
          // the sliders do nothing and a preset loaded in the menu lands
          // half applied (thuum docs/verbs/racemenu-sync.md, 2026-10-07).
          // Like SkyUI's, these scripts run UI, not game state.
          blockEvents = false;
          break;
        } else if (!stricmp(name, "GRIM_ALT_TeleportationMarker")) {
          // Lost Grimoire's Teleportation (thuum docs/verbs/cast-teleport.md):
          // the marker its cast places moves the caster onto itself when it
          // loads (thuum docs/MODS.md). It exists only in the caster's game,
          // since the server relays no teleport cast, and the server takes
          // the jump within the spell's reach of the cast (R1). Blocked, the
          // cast spends its magicka and moves nobody (x-teleport-probe2
          // 20261012-071906).
          blockEvents = false;
          break;
        } else if (!stricmp(name, "WB_DestroyAfterDelay_Script")) {
          // Apocalypse's placed visuals (thuum docs/verbs/spell-cast.md,
          // target location; docs/research/apocalypse/train-27-spells.md):
          // Lightning Strike's bolt and Fingers of the Mountain's are
          // activators an explosion places, whose one script waits WB_Delay
          // (0.3 s) at OnLoad, then disables and deletes the reference.
          // Blocked, the bolt stays in the sky of whichever game placed it
          // (playtests twenty-one to twenty-four: "the bolt looks the same
          // and hangs"). The script touches no state but its own reference's
          // life: R3, visual, in every game
          blockEvents = false;
          break;
        } else if (!stricmp(name, "defaultDisableHavokOnLoad")) {
          // Maybe worth unblocking events only for this script, not for all
          blockEvents = false;
          break;
        }
      }
    }

    vm->attachedScriptsLock.Unlock();
  }

  if (blockEvents) {
    static const auto fsEmpty = new FixedString("");
    gum_invocation_context_replace_nth_argument(ic, 2, fsEmpty);
  }
  // rung 3's seen set (ScriptState.h): a lifecycle event this gate passed
  // was served to the handle's scripts; an OnCellDetach drops the handle's
  // entry either way
  ScriptState::Seen::OnEvent(handle, eventNameStr.c_str(), !blockEvents);
}

void OnSendEventLeave(GumInvocationContext* ic)
{
  // the enter's platform pass on the same thread's mark, so the two agree
  // (the depth is the same at an event's enter and its leave)
  if (PlatformCall::Active()) {
    return;
  }
  EventsApi::SendPapyrusEventLeave();
}

void InstallSendEventHook()
{
  // the presser rule's argument class resolved here, at the install, not on
  // a VM thread at the first trigger event
  const std::uintptr_t triggerArgument = TriggerArgumentVtable();
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::SEND_EVENT, Offsets::Hooks::SendEvent.address(),
    std::make_shared<Frida::Hook>(OnSendEventEnter, OnSendEventLeave));
  // the lab tells one platform build from another by such install lines
  logger::info(
    "[verb:traps] the presser rule (D-W1) installed on the "
    "Papyrus event gate, off until setTriggerPresserRule(true): "
    "the engine's trigger argument by its vtable, "
    "VTABLE___SingleObjectRefrHandleArgument (Address Library "
    "217161), {}",
    triggerArgument
      ? fmt::format("at +{:x}", triggerArgument - REL::Module::get().base())
      : std::string("not resolved: every trigger event passes as "
                    "before"));
}

/**
 *  Draw Sheathe Weapon PC hook
 */
void OnDrawSheatheWeaponPcEnter(GumInvocationContext* ic)
{
  auto refr =
    ic->cpu_context->rcx ? (RE::Actor*)(ic->cpu_context->rcx) : nullptr;
  uint32_t formId = refr ? refr->formID : 0;

  union
  {
    size_t draw;
    uint8_t byte[8];
  };

  draw = (size_t)gum_invocation_context_get_nth_argument(ic, 1);

  auto falseValue = gpointer(*byte ? draw - 1 : draw);
  auto trueValue = gpointer(*byte ? draw : draw + 1);

  auto mode = TESModPlatform::GetWeapDrawnMode(formId);
  if (mode == TESModPlatform::WEAP_DRAWN_MODE_ALWAYS_TRUE) {
    gum_invocation_context_replace_nth_argument(ic, 1, trueValue);
  } else if (mode == TESModPlatform::WEAP_DRAWN_MODE_ALWAYS_FALSE) {
    gum_invocation_context_replace_nth_argument(ic, 1, falseValue);
  }
}

void InstallDrawSheatheWeaponPcHook()
{
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::DRAW_SHEATHE_WEAPON_PC,
    Offsets::Hooks::DrawSheatheWeaponPC.address(),
    std::make_shared<Frida::Hook>(OnDrawSheatheWeaponPcEnter, nullptr));
}

/**
 * Draw Sheathe Weapon Actor hook
 */
void OnDrawSheatheWeaponActorEnter(GumInvocationContext* ic)
{
  auto refr =
    ic->cpu_context->rcx ? (RE::Actor*)(ic->cpu_context->rcx) : nullptr;
  uint32_t formId = refr ? refr->formID : 0;

  auto draw = (uint32_t*)gum_invocation_context_get_nth_argument(ic, 1);

  auto mode = TESModPlatform::GetWeapDrawnMode(formId);
  if (mode == TESModPlatform::WEAP_DRAWN_MODE_ALWAYS_TRUE) {
    gum_invocation_context_replace_nth_argument(ic, 1, gpointer(1));
  } else if (mode == TESModPlatform::WEAP_DRAWN_MODE_ALWAYS_FALSE) {
    gum_invocation_context_replace_nth_argument(ic, 1, gpointer(0));
  }
}

void InstallDrawSheatheWeaponActorHook()
{
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::DRAW_SHEATHE_WEAPON_ACTOR,
    Offsets::Hooks::DrawSheatheWeaponActor.address(),
    std::make_shared<Frida::Hook>(OnDrawSheatheWeaponActorEnter, nullptr));
}

/**
 * Send Animation Event hook
 */

/**
 * The furniture branch (thuum docs/research/fifty-fixes-2026-11-07.md 1(b)
 * and read 1; the static read lab/.cache/drafts/fifty/lever-furn-static.md):
 * a furniture's own graph event during a use is its user's engine's (the
 * plan's rule table: R2, the user's engine computes, the server checks the
 * user and relays, and keeps it nowhere). Bleak Falls Temple's puzzle lever
 * 0xaa8c3 (FURN 0xb7db4 LeverPushAnimatingNoPlayer) moves by the engine's
 * furniture code alone: TESFurniture::Activate's NoPlayer branch (17420)
 * sends one 'Lever_PlayerPush' to the lever's own graph (HYPOTHESIS H-LF2),
 * on the puller's seat alone, synchronously inside the client's own
 * `activate(player, true)` (remoteServer.ts), a non-latent callNative whose
 * body runs on the worker under the platform's mark (PlatformCall::Scope,
 * SkyrimPlatform.cpp's RunInIOContext; H-LF4). The handlers cannot take it
 * there: the hook reaches them through Hook.cpp's PushAndWait to the
 * JavaScript pool thread, the very thread that waits on that call
 * (SkyrimPlatform.cpp's PushToWorkerAndWait), so it would never return, the
 * reason the platform's mark returns first (until thuum tracker #129, whose
 * hand-off that thread serves while it waits; the mark's return now stands
 * on D-S0-14 B's rule, OnSendAnimationEventEnter). So a furniture holder's
 * event is noted here, filtered in native (rule 9: a plain reference whose
 * base is a FURN, and no name pattern, the names being the engine's idle
 * data's), never reaches the handlers, and crosses after the call's leave as
 * furnitureAnimation on the next tick (a tick task, propContact's shape).
 *
 * The enter and leave contract: every enter pushes one note on its thread
 * and every leave pops one, so the two pair under nesting whatever branch
 * returns early; a furniture note's leave reads the graph's answer and calls
 * no handler, and any other note's leave runs the hook's leave as before.
 * Suppressed while setFurnitureAnimation(true) holds: every graph event a
 * FURN's plain reference is sent reaching the sendAnimationEvent handlers
 * (they see none of them). Inside a script action the event stays the
 * seam's (a script's own call on a FURN is reported once, by the boundary),
 * so that test stays first; then the furniture note, inside the platform's
 * call or not; then the platform's return for every other holder.
 *
 * HYPOTHESIS H-LF6: frida-gum's attach on 38048 (the actor holders' slot 1,
 * a thunk whose one instruction is a JMP to 32901) follows the jump, so this
 * hook sits on 32901 and sees every holder's events, a world object's
 * included; read by x-lever-furn v1's ready row (lever-furn-trace.js:
 * resolved.notifyGraph.bytes begins e9), H-LF2 by its notify row inside
 * furnitureActivate. If H-LF6 falls, the same pair goes in a second time on
 * VTABLE_TESObjectREFR[3]'s slot 1 (CommonLibSSE-NG
 * include/RE/Offsets_VTABLE.h:649, 190265), through CommonLib's vtable.
 */
namespace {
// the branch's switch (Frida::SetFurnitureAnimation), off by default
std::atomic<bool> g_furnitureAnimation{ false };

// one per enter on its thread, popped by that call's leave (the contract
// above); furniture false for every enter but a furniture holder's
struct FurnitureNote
{
  bool furniture = false;
  uint32_t refrId = 0;
  std::string event;
};
thread_local std::vector<FurnitureNote> g_furnitureNotes;

// a plain reference's own holder vtable, TESObjectREFR's
// IAnimationGraphManagerHolder base at +0x38 (CommonLibSSE-NG
// include/RE/T/TESObjectREFR.h:112) being its fourth (VTABLE_TESObjectREFR[3],
// include/RE/Offsets_VTABLE.h:649, Address Library 190265, whose slot 1 is
// 32901: lever-furn-static.md step 9), resolved once through CommonLib's
// Relocation, first at the hook's install (InstallSendAnimationEventHook)
std::uintptr_t RefrHolderVtable()
{
  static const REL::Relocation<std::uintptr_t> vtable{
    RE::VTABLE_TESObjectREFR[3]
  };
  return vtable.address();
}

// the furniture whose holder the hook's rcx is, or null. The holder's first
// word is compared with a plain reference's holder vtable before any member
// is read: 32901 serves holders that are no reference too (a weapon's graph,
// SimpleAnimationGraphManagerHolder), and a placed FURN is a plain
// TESObjectREFR. Then the reference through the header's base cast and its
// base's form type (TESObjectREFR::GetBaseObject, TESObjectREFR.h:383;
// TESForm::GetFormType, include/RE/T/TESForm.h:289; FormType::Furniture,
// include/RE/F/FormTypes.h:180): reads alone
RE::TESObjectREFR* FurnitureOf(std::uintptr_t holder)
{
  const std::uintptr_t vtable = RefrHolderVtable();
  if (!holder || vtable == 0 ||
      *reinterpret_cast<const std::uintptr_t*>(holder) != vtable) {
    return nullptr;
  }
  auto* refr = static_cast<RE::TESObjectREFR*>(
    reinterpret_cast<RE::IAnimationGraphManagerHolder*>(holder));
  const RE::TESBoundObject* base = refr->GetBaseObject();
  return base && base->GetFormType() == RE::FormType::Furniture ? refr
                                                                : nullptr;
}

// a line per furniture for its first three events and every hundredth, the
// counts kept under a mutex (the hook runs on any thread; propContact's
// map, cleared past 1024 references)
std::mutex g_furnitureLinesMutex;
std::unordered_map<uint32_t, uint64_t> g_furnitureLines;

bool FurnitureLineDue(uint32_t refrId)
{
  std::lock_guard<std::mutex> lock(g_furnitureLinesMutex);
  if (g_furnitureLines.size() > 1024 && g_furnitureLines.count(refrId) == 0) {
    g_furnitureLines.clear();
  }
  const uint64_t n = ++g_furnitureLines[refrId];
  return n <= 3 || n % 100 == 0;
}
}

void Frida::SetFurnitureAnimation(bool on)
{
  g_furnitureAnimation.store(on, std::memory_order_relaxed);
  logger::info("[verb:furniture] setFurnitureAnimation({}): {}", on,
               on ? "a furniture's own graph event crosses as "
                    "furnitureAnimation, never to the sendAnimationEvent "
                    "handlers"
                  : "every graph event goes to the sendAnimationEvent "
                    "handlers as before");
}

void OnSendAnimationEventEnter(GumInvocationContext* ic)
{
  // this call's note, pushed before any return (its leave pops it)
  g_furnitureNotes.emplace_back();
  // inside a script action (thuum docs/DECISIONS.md ADR-068 decision 7,
  // docs/research/scripts-one-system-2026-11-05.md 6.7): an owned action
  // row's call (a script's PlayAnimation on a world object in a cell this
  // game hosts) is reported once, by the boundary (ScriptNatives.cpp's
  // scriptCalls), so the event its body sends is left alone here, as a
  // platform call's is; the leave below stands down on the same mark, so
  // the hook's enter and leave stay paired (Hook.cpp). HYPOTHESIS (6.7,
  // H-D1): the native's body reaches this entry synchronously on its own
  // thread, inside the mark's extent; a body that queued the event would
  // send it outside the mark, and the animation would be reported twice
  if (ScriptNatives::InScriptAction()) {
    return;
  }
  // the furniture branch (above), inside the platform's call or not: noted,
  // never sent to the handlers
  if (g_furnitureAnimation.load(std::memory_order_relaxed)) {
    if (auto* furniture = FurnitureOf(ic->cpu_context->rcx)) {
      auto furnitureEvent =
        (char**)gum_invocation_context_get_nth_argument(ic, 1);
      if (furnitureEvent && *furnitureEvent && **furnitureEvent) {
        auto& note = g_furnitureNotes.back();
        note.furniture = true;
        note.refrId = furniture->formID;
        note.event = *furnitureEvent;
        return;
      }
    }
  }
  // inside the platform's own call on this thread (PlatformCall.h; thuum
  // D-S0-14 B): no handler is called, since Hook.cpp's PushAndWait would
  // wait on the JavaScript thread that waits on this call (the furniture
  // branch's read 1; callNative's direct addItem and removeItem run on that
  // thread itself). Another thread's event reaches the handlers as any
  // engine event does, its leave paired by the same thread's mark. Since
  // thuum tracker #129 that wait would end (the JavaScript thread serves a
  // hook's ask while it waits on the worker and declines one asked on
  // itself: SkyrimPlatform::HandOffAndWait), so this return stands on
  // D-S0-14 B's rule alone: the platform's own call's events are its own
  if (PlatformCall::Active()) {
    return;
  }
  auto refr = ic->cpu_context->rcx
    ? (RE::TESObjectREFR*)(ic->cpu_context->rcx - 0x38)
    : nullptr;
  uint32_t formId = refr ? refr->formID : 0;

  constexpr int argIdx = 1;
  auto animEventName =
    (char**)gum_invocation_context_get_nth_argument(ic, argIdx);

  // thuum tracker #129 (HookPairing.h): the leave below reaches
  // SendAnimationEventLeave after these two returns, so each leaves its
  // enter's note, and a nested event's leave never takes an outer enter's
  if (!refr || !animEventName) {
    EventsApi::SkipAnimationEventEnter();
    return;
  }

  std::string str = *animEventName;
  if (str == "") {
    EventsApi::SkipAnimationEventEnter();
    return;
  }
  EventsApi::SendAnimationEventEnter(formId, str);
  if (str != *animEventName) {
    auto fs =
      const_cast<RE::BSFixedString*>(&StringHolder::ThreadSingleton()[str]);
    auto newAnimEventName = reinterpret_cast<char**>(fs);
    gum_invocation_context_replace_nth_argument(ic, argIdx, newAnimEventName);
  }
}

void OnSendAnimationEventLeave(GumInvocationContext* ic)
{
  // this call's note (its enter pushed one, whatever it did)
  FurnitureNote note;
  if (!g_furnitureNotes.empty()) {
    note = std::move(g_furnitureNotes.back());
    g_furnitureNotes.pop_back();
  }
  if (note.furniture) {
    // the graph's answer, NotifyAnimationGraph's bool
    // (IAnimationGraphManagerHolder.h:25), returned in AL (the Windows x64
    // convention): the return register's low byte alone
    const bool took = (reinterpret_cast<std::uintptr_t>(
                         gum_invocation_context_get_return_value(ic)) &
                       0xff) != 0;
    if (FurnitureLineDue(note.refrId)) {
      logger::info("[verb:furniture] {:x} '{}' to its own graph ({}, thread "
                   "{})",
                   note.refrId, note.event, took ? "taken" : "refused",
                   GetCurrentThreadId());
    }
    SkyrimPlatform::GetSingleton()->AddTickTask([refrId = note.refrId,
                                                 event = std::move(note.event),
                                                 took](Napi::Env env) {
      auto obj = Napi::Object::New(env);
      AddObjProperty(&obj, "refrId", refrId);
      AddObjProperty(&obj, "event", event);
      AddObjProperty(&obj, "took", took);
      EventsApi::SendEvent("furnitureAnimation", { obj });
    });
    return;
  }
  if (PlatformCall::Active() || ScriptNatives::InScriptAction()) {
    return;
  }
  bool res = !!gum_invocation_context_get_return_value(ic);
  EventsApi::SendAnimationEventLeave(res);
}

void InstallSendAnimationEventHook()
{
  // the furniture branch's class test resolved here, at the install, not on
  // a game thread at the first graph event
  const std::uintptr_t refrHolder = RefrHolderVtable();
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::HOOK_SEND_ANIMATION_EVENT,
    Offsets::Hooks::SendAnimation.address(),
    std::make_shared<Frida::Hook>(OnSendAnimationEventEnter,
                                  OnSendAnimationEventLeave));
  // the lab tells one platform build from another by such install lines
  logger::info(
    "[verb:furniture] the furniture branch installed on the animation hook, "
    "off until setFurnitureAnimation(true): a plain reference's holder by its "
    "vtable, VTABLE_TESObjectREFR[3] (Address Library 190265), {}",
    refrHolder
      ? fmt::format("at +{:x}", refrHolder - REL::Module::get().base())
      : std::string("not resolved: no furniture note is taken"));
}

/**
 * thuum docs/verbs/traps.md (ADR-059 decision 3): the engine's other
 * animation family. A world object without a behavior graph animates by
 * NiControllerManager sequences (a puzzle pillar's Trigger01 and Turned01, a
 * door's Open), which Papyrus's PlayAnimation and PlayAnimationAndWait reach
 * through TESObjectREFR::PlayAnimation(from, to) and its
 * PlayAnimation_Impl(manager, toSeq, fromSeq, bool) (CommonLibSSE-NG
 * src/RE/T/TESObjectREFR.cpp:812-840, :927-932), never through the graph's
 * NotifyAnimationGraph that the hook above sees (x-object-anim v3
 * 20261102-051618: the swinging wall's 180For and the plate's Down reached
 * JS, a pillar's turn could not). This hook raises the sequence's name as
 * the same sendAnimationEvent on the reference, so one JS family (the
 * ObjectAnimationService's patterns and ranges) covers both; the sequence's
 * name is NiControllerSequence::name (include/RE/N/NiControllerSequence.h:87).
 * rcx this, rdx manager, r8 toSeq, r9 fromSeq.
 * Not only a script's call reaches here: a reference's 3D load sets its
 * open state and plays the sequence on the loading thread (thuum tracker
 * #129's dump: the background process thread loading a container's 3D
 * during a COC played its "Close" here, Address Library 14287 into 14297,
 * the read's ids through lab/addr.py; HYPOTHESIS until a second dump or a
 * Frida read of this entry's thread agrees), while the COC's native held
 * the JavaScript thread. The hand-off is served then (HandOff.h), and a
 * sequence no handler's filter takes never crosses (Hook.cpp).
 */
void OnPlayAnimationEnter(GumInvocationContext* ic)
{
  // inside a script action the sequence is the boundary's to report
  // (OnSendAnimationEventEnter's note, 6.7): a script's PlayAnimation and
  // PlayAnimationAndWait reach PlayAnimation_Impl from their bodies (the
  // note above; synchronously on their thread, the same HYPOTHESIS), so the
  // pillar's Trigger02 crosses once, as the call; inside the platform's own
  // call on this thread, no handler (the graph hook's reason above)
  if (PlatformCall::Active() || ScriptNatives::InScriptAction()) {
    return;
  }
  auto refr = reinterpret_cast<RE::TESObjectREFR*>(ic->cpu_context->rcx);
  auto toSeq = reinterpret_cast<RE::NiControllerSequence*>(
    gum_invocation_context_get_nth_argument(ic, 2));
  // the graph hook's pairing (thuum tracker #129): the leave below reaches
  // SendAnimationEventLeave after these returns too
  if (!refr || !toSeq) {
    EventsApi::SkipAnimationEventEnter();
    return;
  }
  const char* name = toSeq->name.c_str();
  if (!name || !*name) {
    EventsApi::SkipAnimationEventEnter();
    return;
  }
  std::string str = name;
  EventsApi::SendAnimationEventEnter(refr->formID, str);
}

void OnPlayAnimationLeave(GumInvocationContext* ic)
{
  if (PlatformCall::Active() || ScriptNatives::InScriptAction()) {
    return;
  }
  EventsApi::SendAnimationEventLeave(true);
}

void InstallPlayAnimationHook()
{
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::HOOK_PLAY_ANIMATION,
    Offsets::Hooks::PlayAnimation.address(),
    std::make_shared<Frida::Hook>(OnPlayAnimationEnter, OnPlayAnimationLeave));
}

/**
 * PerformAction hook (thuum docs/verbs/cast-action.md): every BGSAction the
 * player's actor performs, as the engine performs it through its one action
 * entry, ActorMediator::PerformAction (the hand's attack, ready, release and
 * interrupt a cast is made of, the dual and voice ones, the melee ones), with
 * the idle the engine chose, the graph event it sent, and the actor's
 * master-behavior variables as they were when the graph was asked. Filtered
 * to the player here (rule 9), raised to JS as actionPerformed on the next
 * tick; a figure in another game replays it through performAction.
 *
 * The entry is (ActorMediator*, TESActionData*) -> bool (the data is
 * CommonLibSSE-NG's, include/RE/T/TESActionData.h; the entry read on sky-re
 * 2026-10-14, HYPOTHESIS until the lab's trace): the data comes in with the
 * event empty and leaves with the chosen idle's event in it, so the fields
 * are read at the leave. A call with flags bit 0 set only validates (the
 * engine's dry run, answered true with nothing performed), and a false
 * return played nothing; neither is reported. The mediator re-enters itself
 * for a paired action's target, so each thread keeps a stack, not a slot.
 */
struct PerformedAction
{
  RE::TESActionData* data = nullptr;
  // the source actor's form id (0x14 the player's) and whether it is the
  // player's; the actor's mark in the hosted table (Hosted::IsPerforming)
  // is on from the entry to the leave
  uint32_t sourceId = 0;
  bool isPlayer = false;
  AnimationGraphMasterBehaviourDescriptor::AnimationVariables variables;
};
thread_local std::vector<PerformedAction> g_performedActions;

// the player's pending cast starts, one per hand (0 left, 1 right): the
// time its start action was performed and no cast event has claimed
// (Frida::TakeCastStart); written on the game thread, read there too
std::atomic<uint64_t> g_pendingCastStartMs[2]{ 0, 0 };

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

void NoteCastHand(const std::string& eventName)
{
  if (eventName.size() < 4) {
    return;
  }
  std::string lower = eventName;
  for (auto& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  int32_t hand = -1;
  if (lower.rfind("mrh_", 0) == 0) {
    hand = 1;
  } else if (lower.rfind("mlh_", 0) == 0 ||
             lower.rfind("dualmagic_", 0) == 0) {
    hand = 0;
  }
  if (hand < 0) {
    return;
  }
  // a start opens the hand's pending cast; its Equipped exit (the Interrupt
  // action, a cancelled charge too) closes it; ready and release change
  // nothing (a fire-and-forget spell's cast event comes at its release)
  const bool isStart =
    lower.size() >= 5 && lower.compare(lower.size() - 5, 5, "start") == 0;
  const bool isExit = lower.find("equipped") != std::string::npos;
  if (isStart) {
    g_pendingCastStartMs[hand] = NowMs();
  } else if (isExit) {
    g_pendingCastStartMs[hand] = 0;
  }
}

void OnPerformActionEnter(GumInvocationContext* ic)
{
  PerformedAction entry;
  auto* data = reinterpret_cast<RE::TESActionData*>(ic->cpu_context->rdx);
  auto* player = RE::PlayerCharacter::GetSingleton();
  if (data && player && data->action && (data->flags & 1) == 0) {
    // thuum docs/verbs/npc-stream.md (ADR-050): an actor this game hosts
    // (Hosted.h, the server's word mirrored by setHosted) is admitted beside
    // the player: its actions travel as UpdateAction under its own id and
    // the puppets perform them through the engine's same entry
    auto* source = data->source.get();
    RE::Actor* actor = nullptr;
    if (source == player) {
      actor = player;
      entry.isPlayer = true;
    } else if (source && Hosted::Is(source->formID)) {
      actor = source->As<RE::Actor>();
    }
    if (actor) {
      // the variables before the action changes them: the want variable the
      // caster set a few instructions earlier is among them. A graph that is
      // not the master behaviour's (a creature's, slice 1 v2) gives none,
      // and such an actor is not admitted: its actions are not reported and
      // it is not marked, so its events ride the relay as before
      auto variables =
        AnimationGraphMasterBehaviourDescriptor{ *actor }.GetVariables();
      if (entry.isPlayer || !variables.IsEmpty()) {
        entry.data = data;
        entry.sourceId = actor->formID;
        entry.variables = std::move(variables);
        // the mark the relay's source reads while the engine sends this
        // action's graph event inside the call (Hosted::IsPerforming): the
        // event is the action's own and travels as the action
        Hosted::BeginPerforming(actor->formID);
      } else {
        entry.isPlayer = false;
      }
    }
  }
  g_performedActions.push_back(std::move(entry));
}

void OnPerformActionLeave(GumInvocationContext* ic)
{
  if (g_performedActions.empty()) {
    return;
  }
  PerformedAction entry = std::move(g_performedActions.back());
  g_performedActions.pop_back();
  auto* data = entry.data;
  if (!data) {
    return;
  }
  // the mark off before any return: the graph event, if any, went inside
  Hosted::EndPerforming(entry.sourceId);
  const bool taken = !!gum_invocation_context_get_return_value(ic);
  if (!taken) {
    return;
  }
  const uint32_t actionId = data->action ? data->action->formID : 0;
  const uint32_t idleId = data->animObjIdle ? data->animObjIdle->formID : 0;
  const std::string eventName =
    data->animEvent.c_str() ? data->animEvent.c_str() : "";
  const uint32_t type = data->unk20;
  const uint32_t flags = data->flags;
  const uint32_t result = static_cast<uint32_t>(data->result);
  // the player's pending cast starts alone: a hosted actor's casts are the
  // stream's, not the player's cast events'
  if (entry.isPlayer) {
    NoteCastHand(eventName);
  }

  SkyrimPlatform::GetSingleton()->AddTickTask(
    [actionId, idleId, eventName, type, flags, result,
     sourceId = entry.sourceId, isPlayer = entry.isPlayer,
     variables = std::move(entry.variables)](Napi::Env env) {
      auto obj = Napi::Object::New(env);
      // the player's own form as before; a hosted actor goes by its id
      // alone (no form lookup on this thread: PuppetApi.cpp's reason, the
      // forms map's lock against a cell load), actor null
      if (isPlayer) {
        auto* player = RE::PlayerCharacter::GetSingleton();
        AddObjProperty(&obj, "actor", static_cast<RE::TESForm*>(player),
                       "Actor");
      } else {
        obj.Set("actor", env.Null());
      }
      AddObjProperty(&obj, "actorId", sourceId);
      AddObjProperty(&obj, "action", actionId);
      AddObjProperty(&obj, "idle", idleId);
      AddObjProperty(&obj, "eventName", eventName.c_str());
      AddObjProperty(&obj, "type", type);
      AddObjProperty(&obj, "flags", flags);
      AddObjProperty(&obj, "result", result);
      obj.Set("variables", MagicApi::AnimationVariablesToJs(env, variables));
      EventsApi::SendEvent("actionPerformed", { obj });
    });
}

int32_t Frida::TakeCastStart(uint64_t withinMs)
{
  const uint64_t now = NowMs();
  int32_t oldest = -1;
  uint64_t oldestAt = 0;
  for (int32_t hand = 0; hand < 2; ++hand) {
    const uint64_t at = g_pendingCastStartMs[hand].load();
    if (at == 0 || now < at || now - at > withinMs) {
      continue;
    }
    if (oldest < 0 || at < oldestAt) {
      oldest = hand;
      oldestAt = at;
    }
  }
  if (oldest >= 0) {
    g_pendingCastStartMs[oldest] = 0;
  }
  return oldest;
}

/**
 * Every projectile launch (thuum docs/verbs/marksman.md: the arrow's path
 * identical on both screens; ADR-032's shape for the effect). The engine
 * launches every arrow and spell projectile through Projectile::Launch with
 * a LaunchData the caller filled (CommonLibSSE-NG include/RE/P/Projectile.h:
 * 78-107: origin, projectileBase, shooter, weaponSource, ammoSource, angleZ,
 * angleX, spell, castingSource, power, useOrigin). Read on entry, before the
 * launch, and raised to JS as projectileLaunched on the next tick: the
 * shooter's client sends its own arrow's origin and angles with the shot so
 * the observer launches the figure's arrow at the same point in the same
 * direction instead of from the figure's node along its aim; the lab's
 * recorder keeps every launch for the parity check. Only the player's and
 * the figures' cross into JS (rule 9).
 *
 * A figure's own arrow is dropped here. A figure wears its quiver, so its
 * bow releasing in this game launches an arrow of its own, beside the one
 * TESModPlatform launches for the shot the server relayed (x-double-arrow2
 * 20261012-022726: two launches for one shot). An arrow a figure launches
 * outside a RelayedArrowScope enters with no projectile (LaunchData's
 * projectileBase, Projectile.h:83), so Launch builds none (HYPOTHESIS, the
 * re-analyst's read of 44108 pending; the probe x-quiver counts the arrows),
 * and leaves with it put back for its caller. It is not reported: nothing
 * flew.
 */
struct LaunchSeen
{
  uint32_t shooter = 0;
  uint32_t projectile = 0;
  uint32_t weapon = 0;
  uint32_t ammo = 0;
  uint32_t spell = 0;
  int32_t castingSource = -1;
  float originX = 0, originY = 0, originZ = 0;
  float angleX = 0, angleZ = 0;
  float power = 0;
  bool useOrigin = false;
  // thuum docs/verbs/spell-cast.md: LaunchData's contactNormal (the pick's
  // normal a target-location launch carries, Projectile.h:82)
  float contactNormalX = 0, contactNormalY = 0, contactNormalZ = 0;
};

// one entry per Launch on the thread, pushed at the enter and popped at the
// leave; data is set when the enter took the projectile away
struct DroppedArrow
{
  RE::Projectile::LaunchData* data = nullptr;
  RE::BGSProjectile* projectile = nullptr;
};
thread_local std::vector<DroppedArrow> g_droppedArrows;
thread_local uint32_t g_relayedArrowDepth = 0;

Frida::RelayedArrowScope::RelayedArrowScope()
{
  ++g_relayedArrowDepth;
}

Frida::RelayedArrowScope::~RelayedArrowScope()
{
  --g_relayedArrowDepth;
}

void OnProjectileLaunchEnter(GumInvocationContext* ic)
{
  auto* data =
    reinterpret_cast<RE::Projectile::LaunchData*>(ic->cpu_context->rdx);
  g_droppedArrows.push_back({});
  if (!data || !data->shooter) {
    return;
  }
  // filtered in native (rule 9): the player's own launches and a hosted
  // actor's (the shooter's side of the sync: this game simulates both), the
  // figures' and the puppets' (the observer's side, which the recorder
  // compares); any other NPC's arrows and spells stay out of JS
  const uint32_t shooterId = data->shooter->formID;
  const bool player = shooterId == 0x14;
  const bool hosted = !player && Hosted::Is(shooterId);
  const bool puppet = !player && !hosted && Puppets::Is(shooterId);
  const bool figure = !player && !hosted && !puppet && shooterId >= 0xFF000000;
  if (!player && !hosted && !puppet && !figure) {
    return;
  }
  // thuum docs/verbs/marksman.md and npc-stream.md (train 53): an arrow this
  // game did not shoot is dropped unless a relayed shot launches it
  // (RelayedArrowScope): a figure's (its quiver is worn, so its bow's
  // release launches one of its own beside the relayed one) and a puppet's
  // (the replayed release may launch one; its host's shot arrives as
  // ArrowShot). A hosted actor's arrow flies and reaches JS as the player's
  // does: before train 53 a server-made actor this game hosted was dropped
  // here by its 0xff id (x-npc-bow 221512: the host's own archer shot
  // nothing on any screen)
  if ((figure || puppet) && data->ammoSource && data->projectileBase &&
      g_relayedArrowDepth == 0) {
    g_droppedArrows.back() = { data, data->projectileBase };
    data->projectileBase = nullptr;
    logger::info("{} own arrow dropped: shooter {:x}, ammo {:x}",
                 puppet ? "a puppet's" : "a figure's", shooterId,
                 data->ammoSource->formID);
    return;
  }
  LaunchSeen seen;
  seen.shooter = data->shooter->formID;
  seen.projectile = data->projectileBase ? data->projectileBase->formID : 0;
  seen.weapon = data->weaponSource ? data->weaponSource->formID : 0;
  seen.ammo = data->ammoSource ? data->ammoSource->formID : 0;
  seen.spell = data->spell ? data->spell->formID : 0;
  seen.castingSource = static_cast<int32_t>(data->castingSource);
  seen.originX = data->origin.x;
  seen.originY = data->origin.y;
  seen.originZ = data->origin.z;
  seen.angleX = data->angleX;
  seen.angleZ = data->angleZ;
  seen.power = data->power;
  seen.useOrigin = data->useOrigin;
  seen.contactNormalX = data->contactNormal.x;
  seen.contactNormalY = data->contactNormal.y;
  seen.contactNormalZ = data->contactNormal.z;
  SkyrimPlatform::GetSingleton()->AddTickTask([seen](Napi::Env env) {
    auto obj = Napi::Object::New(env);
    AddObjProperty(&obj, "shooter", seen.shooter);
    AddObjProperty(&obj, "projectile", seen.projectile);
    AddObjProperty(&obj, "weapon", seen.weapon);
    AddObjProperty(&obj, "ammo", seen.ammo);
    AddObjProperty(&obj, "spell", seen.spell);
    AddObjProperty(&obj, "castingSource", seen.castingSource);
    obj.Set("originX",
            Napi::Number::New(env, static_cast<double>(seen.originX)));
    obj.Set("originY",
            Napi::Number::New(env, static_cast<double>(seen.originY)));
    obj.Set("originZ",
            Napi::Number::New(env, static_cast<double>(seen.originZ)));
    obj.Set("angleX",
            Napi::Number::New(env, static_cast<double>(seen.angleX)));
    obj.Set("angleZ",
            Napi::Number::New(env, static_cast<double>(seen.angleZ)));
    obj.Set("power", Napi::Number::New(env, static_cast<double>(seen.power)));
    AddObjProperty(&obj, "useOrigin", seen.useOrigin);
    obj.Set("contactNormalX",
            Napi::Number::New(env, static_cast<double>(seen.contactNormalX)));
    obj.Set("contactNormalY",
            Napi::Number::New(env, static_cast<double>(seen.contactNormalY)));
    obj.Set("contactNormalZ",
            Napi::Number::New(env, static_cast<double>(seen.contactNormalZ)));
    EventsApi::SendEvent("projectileLaunched", { obj });
  });
}

void OnProjectileLaunchLeave(GumInvocationContext* ic)
{
  if (g_droppedArrows.empty()) {
    return;
  }
  const DroppedArrow dropped = g_droppedArrows.back();
  g_droppedArrows.pop_back();
  if (dropped.data) {
    dropped.data->projectileBase = dropped.projectile;
  }
}

/**
 * An arrow's flight as it starts (thuum docs/verbs/marksman.md): the
 * engine sets a missile's velocity on its first update from its angles at
 * that moment (InitVelocity, game/Offsets.h), and for the player's arrow
 * those are the angles after the engine's auto-aim, which Launch's LaunchData
 * does not carry (the re-analyst, 2026-10-16). Read on entry for arrows
 * (an ammo source) and, since train-30, for a spell's projectiles (a spell
 * source, the ammo 0: docs/verbs/spell-cast.md, Scattershock's heading on
 * the observer) whose shooter is the player or a figure (rule 9), raised as
 * projectileFlight on the next tick: the shooter's client sends an arrow's
 * angles with the shot, the recorder keeps both games' for the parity check.
 */
void OnProjectileInitVelocityEnter(GumInvocationContext* ic)
{
  auto* projectile = reinterpret_cast<RE::Projectile*>(ic->cpu_context->rcx);
  if (!projectile) {
    return;
  }
  auto& runtime = projectile->GetProjectileRuntimeData();
  if (!runtime.ammoSource && !runtime.spell) {
    return;
  }
  const auto shooter = runtime.shooter.get();
  if (!shooter) {
    return;
  }
  const uint32_t shooterId = shooter->formID;
  if (shooterId != 0x14 && shooterId < 0xFF000000) {
    return;
  }
  LaunchSeen seen;
  seen.shooter = shooterId;
  seen.projectile =
    projectile->GetBaseObject() ? projectile->GetBaseObject()->formID : 0;
  seen.weapon = runtime.weaponSource ? runtime.weaponSource->formID : 0;
  seen.ammo = runtime.ammoSource ? runtime.ammoSource->formID : 0;
  seen.spell = runtime.spell ? runtime.spell->formID : 0;
  seen.originX = projectile->data.location.x;
  seen.originY = projectile->data.location.y;
  seen.originZ = projectile->data.location.z;
  seen.angleX = projectile->data.angle.x;
  seen.angleZ = projectile->data.angle.z;
  seen.power = runtime.power;
  SkyrimPlatform::GetSingleton()->AddTickTask([seen](Napi::Env env) {
    auto obj = Napi::Object::New(env);
    AddObjProperty(&obj, "shooter", seen.shooter);
    AddObjProperty(&obj, "projectile", seen.projectile);
    AddObjProperty(&obj, "weapon", seen.weapon);
    AddObjProperty(&obj, "ammo", seen.ammo);
    AddObjProperty(&obj, "spell", seen.spell);
    obj.Set("x", Napi::Number::New(env, static_cast<double>(seen.originX)));
    obj.Set("y", Napi::Number::New(env, static_cast<double>(seen.originY)));
    obj.Set("z", Napi::Number::New(env, static_cast<double>(seen.originZ)));
    obj.Set("angleX",
            Napi::Number::New(env, static_cast<double>(seen.angleX)));
    obj.Set("angleZ",
            Napi::Number::New(env, static_cast<double>(seen.angleZ)));
    obj.Set("power", Napi::Number::New(env, static_cast<double>(seen.power)));
    EventsApi::SendEvent("projectileFlight", { obj });
  });
}

void InstallProjectileInitVelocityHook()
{
  if (!REL::Module::IsAE()) {
    return;
  }
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::HOOK_PROJECTILE_INIT_VELOCITY,
    Offsets::Hooks::ProjectileInitVelocity.address(),
    std::make_shared<Frida::Hook>(OnProjectileInitVelocityEnter, nullptr));
}

/**
 * The point a player's target-location cast places its explosion at (thuum
 * docs/research/apocalypse/target-location.md, the class pass): FindTargets
 * calls the engine's one placer (game/Offsets.h PlaceAreaEffect, 34413) with
 * the caster, the final point after the pick and the navmesh snap, the cell,
 * the spell and its largest-area effect. Read on entry for the player's
 * caster alone (rule 9; ActorMagicCaster's actor, CommonLibSSE-NG
 * include/RE/A/ActorMagicCaster.h:82) and raised to JS as areaEffectPlaced
 * on the next tick: the caster's client sends the point with its cast as a
 * launch, and an observer places the same explosion there with the figure's
 * caster (MagicApi placeAreaEffect). SKSE publishes no event for it (ADR-034's
 * exception). HYPOTHESIS until x-class-location's rerun: the id is the
 * re-analyst's static read.
 */
struct AreaEffectSeen
{
  uint32_t spell = 0;
  uint32_t effect = 0;
  float x = 0, y = 0, z = 0;
  bool dual = false;
};

void OnPlaceAreaEffectEnter(GumInvocationContext* ic)
{
  auto* caster = reinterpret_cast<RE::ActorMagicCaster*>(ic->cpu_context->rcx);
  auto* point = reinterpret_cast<RE::NiPoint3*>(ic->cpu_context->rdx);
  auto* spell = reinterpret_cast<RE::MagicItem*>(ic->cpu_context->r9);
  auto* effect = reinterpret_cast<RE::Effect*>(
    gum_invocation_context_get_nth_argument(ic, 4));
  if (!caster || !point || !spell) {
    return;
  }
  auto* player = RE::PlayerCharacter::GetSingleton();
  if (!player || caster->actor != player) {
    return;
  }
  AreaEffectSeen seen;
  seen.spell = spell->formID;
  seen.effect = effect && effect->baseEffect ? effect->baseEffect->formID : 0;
  seen.x = point->x;
  seen.y = point->y;
  seen.z = point->z;
  seen.dual = caster->GetIsDualCasting();
  SkyrimPlatform::GetSingleton()->AddTickTask([seen](Napi::Env env) {
    auto obj = Napi::Object::New(env);
    AddObjProperty(&obj, "spell", seen.spell);
    AddObjProperty(&obj, "effect", seen.effect);
    obj.Set("x", Napi::Number::New(env, static_cast<double>(seen.x)));
    obj.Set("y", Napi::Number::New(env, static_cast<double>(seen.y)));
    obj.Set("z", Napi::Number::New(env, static_cast<double>(seen.z)));
    AddObjProperty(&obj, "dual", seen.dual);
    EventsApi::SendEvent("areaEffectPlaced", { obj });
  });
}

void InstallPlaceAreaEffectHook()
{
  // AE only: the id is 1.6.1170's (game/Offsets.h)
  if (!REL::Module::IsAE()) {
    return;
  }
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::HOOK_PLACE_AREA_EFFECT,
    Offsets::Hooks::PlaceAreaEffect.address(),
    std::make_shared<Frida::Hook>(OnPlaceAreaEffectEnter, nullptr));
}

/**
 * thuum docs/verbs/spell-cast.md (the rune's glyph on the observer): the
 * engine's decal placement (game/Offsets.h RuneDecal, Address Library 44186),
 * which the magic caster's target-location launch 34452 calls right after
 * Projectile::Launch for a grenade projectile with (the projectile, &origin,
 * &normal scaled to 69.99, the material id from 44190, 1, 0, the decal group,
 * 1): the re-analyst's read, CONFIRMED on the caster's seat by the lab's
 * trace (lab/frida/rune-decal-trace.js, x-rune-decal-frida 20261020-102818:
 * one call per cast, the rune PGRE, the pick's position, a normal of length
 * 69.99). Read at the entry for the player's own projectile alone (rule 9:
 * the shooter handle against the player) and raised to JS as runeDecal on
 * the next tick with the projectile's base form, the origin, the unit normal
 * and the material id; magicSyncService sends them with the cast
 * (SpellCastData.decal), and each observer's game hands them to the same
 * placement after its relayed launch (MagicApi PlaceRelayedDecal). Nothing
 * here changes the call.
 */
struct RuneDecalSeen
{
  uint32_t projectile = 0;
  float originX = 0, originY = 0, originZ = 0;
  float normalX = 0, normalY = 0, normalZ = 0;
  uint32_t materialId = 0;
};

void OnRuneDecalEnter(GumInvocationContext* ic)
{
  auto* projectile = static_cast<RE::Projectile*>(
    gum_invocation_context_get_nth_argument(ic, 0));
  auto* origin = static_cast<const RE::NiPoint3*>(
    gum_invocation_context_get_nth_argument(ic, 1));
  auto* normal = static_cast<const RE::NiPoint3*>(
    gum_invocation_context_get_nth_argument(ic, 2));
  const auto materialId = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
    gum_invocation_context_get_nth_argument(ic, 3)));
  if (!projectile || !origin || !normal) {
    return;
  }
  // the player's own projectile alone (Projectile.h's runtime data: the
  // shooter's handle at 0x120)
  const auto shooter = projectile->GetProjectileRuntimeData().shooter.get();
  if (!shooter || shooter->formID != 0x14) {
    return;
  }
  RuneDecalSeen seen;
  seen.projectile =
    projectile->GetBaseObject() ? projectile->GetBaseObject()->formID : 0;
  seen.originX = origin->x;
  seen.originY = origin->y;
  seen.originZ = origin->z;
  const float length = normal->Length();
  if (length > 0.f && std::isfinite(length)) {
    seen.normalX = normal->x / length;
    seen.normalY = normal->y / length;
    seen.normalZ = normal->z / length;
  }
  seen.materialId = materialId;
  SkyrimPlatform::GetSingleton()->AddTickTask([seen](Napi::Env env) {
    auto obj = Napi::Object::New(env);
    AddObjProperty(&obj, "projectile", seen.projectile);
    obj.Set("originX",
            Napi::Number::New(env, static_cast<double>(seen.originX)));
    obj.Set("originY",
            Napi::Number::New(env, static_cast<double>(seen.originY)));
    obj.Set("originZ",
            Napi::Number::New(env, static_cast<double>(seen.originZ)));
    obj.Set("normalX",
            Napi::Number::New(env, static_cast<double>(seen.normalX)));
    obj.Set("normalY",
            Napi::Number::New(env, static_cast<double>(seen.normalY)));
    obj.Set("normalZ",
            Napi::Number::New(env, static_cast<double>(seen.normalZ)));
    AddObjProperty(&obj, "materialId", seen.materialId);
    EventsApi::SendEvent("runeDecal", { obj });
  });
}

void InstallRuneDecalHook()
{
  // AE only: the id is 1.6.1170's (game/Offsets.h)
  if (!REL::Module::IsAE()) {
    return;
  }
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::HOOK_RUNE_DECAL, Offsets::Hooks::RuneDecal.address(),
    std::make_shared<Frida::Hook>(OnRuneDecalEnter, nullptr));
  logger::info("[verb:spell-cast] rune decal hook installed: the decal "
               "placement (Address Library 44186), the player's own launches");
}

void InstallProjectileLaunchHook()
{
  // AE only: the id is 1.6.1170's (game/Offsets.h)
  if (!REL::Module::IsAE()) {
    return;
  }
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::HOOK_PROJECTILE_LAUNCH,
    Offsets::Hooks::ProjectileLaunch.address(),
    std::make_shared<Frida::Hook>(OnProjectileLaunchEnter,
                                  OnProjectileLaunchLeave));
}

void InstallPerformActionHook()
{
  // AE only: the id is 1.6.1170's (game/Offsets.h)
  if (!REL::Module::IsAE()) {
    return;
  }
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::HOOK_PERFORM_ACTION,
    Offsets::Hooks::PerformAction.address(),
    std::make_shared<Frida::Hook>(OnPerformActionEnter, OnPerformActionLeave));
}

/**
 * The graph's answers to a cast request (thuum docs/verbs/cast-action.md):
 * the store behind castReady. The hooks that fill it are typed vtable
 * swaps on the three spell-cast response handlers (Hooks.cpp,
 * InstallSpellCastResponseHooks), not Frida interceptors: the first build
 * with interceptors on those functions (34bdf0c3) died at the first answer
 * on the lab (x-cast-human 20261015-224514), before anything was logged.
 * Written where the graph event is handled, read on the game thread.
 */
std::mutex g_castReadyMutex;
std::unordered_map<uint32_t, uint32_t> g_castReady; // form id -> hand bits

void Frida::NoteCastReady(uint32_t actorFormId, uint32_t handBit)
{
  std::lock_guard<std::mutex> lock(g_castReadyMutex);
  g_castReady[actorFormId] |= handBit;
}

bool Frida::TakeCastReady(uint32_t actorFormId, int32_t hand)
{
  const uint32_t bit = hand == 0 ? 1 : hand == 1 ? 2 : hand == 2 ? 4 : 0;
  bool taken = false;
  {
    std::lock_guard<std::mutex> lock(g_castReadyMutex);
    auto it = g_castReady.find(actorFormId);
    if (it != g_castReady.end() && (it->second & bit) != 0) {
      it->second &= ~bit;
      taken = true;
    }
  }
  if (taken) {
    logger::info("[verb:cast-action] castReady on {:x}: hand {}", actorFormId,
                 hand);
  }
  return taken;
}

/**
 * Queue Ninode Update hook
 */
thread_local uint32_t g_queueNiNodeActorId = 0;

void OnQueueNinodeUpdateEnter(GumInvocationContext* ic)
{
  auto refr = ic->cpu_context->rcx ? (RE::TESObjectREFR*)(ic->cpu_context->rcx)
                                   : nullptr;

  uint32_t id = refr ? refr->formID : 0;

  g_queueNiNodeActorId = id;
}

void InstallQueueNinodeUpdateHook()
{
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::QUEUE_NINODE_UPDATE,
    Offsets::Hooks::QueueNinodeUpdate.address(),
    std::make_shared<Frida::Hook>(OnQueueNinodeUpdateEnter, nullptr));
}

/**
 * Apply Masks To Render Targets hook
 */
void OnApplyMasksToRenderTargetsEnter(GumInvocationContext* ic)
{
  if (g_queueNiNodeActorId > 0) {
    auto tints = TESModPlatform::GetTintsFor(g_queueNiNodeActorId);
    if (tints) {
      gum_invocation_context_replace_nth_argument(ic, 0, tints.get());
    }
  }

  g_queueNiNodeActorId = 0;
}

void InstallApplyMasksToRenderTargetsHook()
{
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::APPLY_MASKS_TO_RENDER_TARGET,
    Offsets::Hooks::ApplyMasksToRenderTargets.address(),
    std::make_shared<Frida::Hook>(OnApplyMasksToRenderTargetsEnter, nullptr));
}

/**
 * HasOverlays on a figure's base, and on the player's own (thuum
 * docs/verbs/appearance.md; the fault
 * x-vampire-stage recorded at run 20261111-035133: after c1's own player
 * turned vampire, NordRace 0x13746 to NordRaceVampire 0x88794, c1 drew c2's
 * figure with no head, a hollow helmet, while c2's view of c1 stayed right).
 * The third hook here for a figure posing as the player, beside the two
 * above. CONFIRMED-STATIC: a figure's base reads the player's form id 7 from
 * before its first 3D load (SpawnProcess.enable calls applyTints before it
 * enables the reference, skymp5-client src/view/spawnProcess.ts:21-32;
 * applyTints gives the base the player's base id through SetFormIdUnsafe,
 * src/sync/appearance.ts:126-129, which writes TESForm::formID alone,
 * PapyrusTESModPlatform.cpp:530-537). CONFIRMED-STATIC (data): the vampire
 * race carries the RACE flag kOverlayHeadPartList (CommonLibSSE-NG
 * include/RE/T/TESRace.h:77; set in NordRaceVampire 0x88794's DATA flags by
 * thuum lab/esm.py's race_flags over Skyrim.esm, clear in NordRace 0x13746
 * and OrcRace 0x13747).
 *
 * HYPOTHESIS (decompiler reads only, rule 2: a review's read of 1.6.1170 and
 * thuum ghidra/notes/playercharacter-tints-1-7-104.md on 1.7.104; the body
 * was not reread for this hook): the engine's HasOverlays (game/Offsets.h)
 * branches on form id 7 and there reads the local player's charGenRace and
 * an overlay-map entry (the map GetBaseOverlays 24791 and GetNumBaseOverlays
 * 24792 read), so the local player's turn flips the answer for every figure.
 * Once it is true, a type's current head part comes from the overlay list
 * alone (CommonLib's reading, src/RE/T/TESNPC.cpp:94-97); a figure's base
 * has no overlay array (:111-123), so no Face part and no head; and the
 * client rebuilds a figure's head each time it comes on screen
 * (skymp5-client src/view/formView.ts:1052-1087), so the head built before
 * the turn goes at the next rebuild. HYPOTHESIS too: the engine's head build
 * asks this function rather than an inlined copy of its test. The lab read
 * that settles both is x-vampire-figure-head (thuum lab/.cache/drafts/rung5).
 *
 * Replaced through Gum (the DamageActor shape): a base that reads form id 7
 * and is not the player's own (the player character's base object, compared
 * by pointer) gets false where the engine said true, so the engine builds
 * the figure's head from its own parts, as a seat whose player is no vampire
 * does; every other NPC gets the engine's answer. The player's own base
 * keeps the engine's true only while its race carries a head part list
 * flag, kOverlayHeadPartList or kOverrideHeadPartList (TESRace.h:77-78):
 * the engine's form id 7 branch reads neither (HYPOTHESIS, a static read of
 * 24790 in the unpacked 1.6.1170 exe: the chargen race against the base's
 * race, TESNPC+0x158, and an overlay-map entry), so a race with neither
 * laid the player's pre-switch parts over its own: x-rotfern-vampire-eyes
 * 20261114-224641 read rotfern's own face keeping RotfernEyes over the
 * RotfernEyesVampire her script set (Eli, 2026-11-14: "fix the eyes. fix
 * the overlay flag"). Paired with thuum D-E1 A'' (D-E2 B): the layer's
 * lab/layer/plugin-patches.tsv row "race-flags 02019D08 clear 04000000"
 * clears rotfernRaceVampire's kOverlayHeadPartList at build; laid without
 * that row, her vampire race carries the flag and her own face keeps the
 * engine's overlay copy. Read 2026-11-14 over the seats' engine order
 * (sky-srv's persist, 101 plugins, apocrypha's race_winners): every
 * PlayableVampireList race's winning record carries kOverlayHeadPartList
 * (0x54A08942, USSEP's or Dawnguard.esm's), so a vanilla vampire's own face
 * keeps the engine's answer; WerewolfBeastRace 0xCDD84 (0x08288940) and
 * Dawnguard.esm's DLC1VampireBeastRace 0x283A (0x48688940) carry
 * kOverrideHeadPartList, so a beast form's own face keeps it too; the ten
 * playable races (0x50A08943) carry neither, so after a switch away from
 * the chargen race (alignRace's reconnect SetRace, skymp5-client
 * raceMenuService.ts) the player's own face takes the base's parts, the
 * record every other screen shows (probably benign, unmeasured).
 * Filtered in native (rule 9): the form id compare comes first, so any other
 * NPC costs Gum's replace (its thread-local bookkeeping and a stack push,
 * FridaHookHandler.cpp's gum_interceptor_replace) and one compare beside the
 * engine's own; the call volume is unread. Nothing crosses into JS. Each
 * base's first override prints one line, and the count prints every ten
 * seconds while it moves (thuum ADR-069's note: a fix that skips work logs its
 * count), so a run can count the overrides.
 */
namespace {
using HasOverlaysFn = bool (*)(RE::TESNPC* npc);
HasOverlaysFn g_hasOverlaysOriginal = nullptr;

// the player's base form id, the one applyTints gives a figure's base (run
// 20261111-035133's figure-spells reads: base 7)
constexpr uint32_t kPlayerBaseFormId = 0x7;

// the overrides since launch, and the bases they came from: the replacement
// counts, the reporter below reads
std::atomic<uint64_t> g_hasOverlaysOverrides{ 0 };
std::atomic<uint32_t> g_hasOverlaysBases{ 0 };
// the player's own base given false since launch: its race carries neither
// head part list flag (thuum D-E1 A''); the reporter prints its line before
// the figures' line
std::atomic<uint64_t> g_hasOverlaysOwnOverrides{ 0 };
// the race the own-base line last named, so a new race prints a new line
std::atomic<uint32_t> g_hasOverlaysOwnLastRace{ 0 };

// the bases given their first line, by pointer. HasOverlays' thread is
// unread (a 3D load may run on a job thread), so a mutex, taken on the
// override's branch alone (rule 9); the set is cleared past 1024 bases, as
// TriggerLineDue's map
std::mutex g_hasOverlaysSeenMutex;
std::unordered_set<std::uintptr_t> g_hasOverlaysSeen;

bool HasOverlaysFirstOverride(const RE::TESNPC* npc)
{
  const auto key = reinterpret_cast<std::uintptr_t>(npc);
  std::lock_guard<std::mutex> lock(g_hasOverlaysSeenMutex);
  if (g_hasOverlaysSeen.size() > 1024 && g_hasOverlaysSeen.count(key) == 0) {
    g_hasOverlaysSeen.clear();
  }
  return g_hasOverlaysSeen.insert(key).second;
}

bool HasOverlaysReplacement(RE::TESNPC* npc)
{
  // the cheap test first (rule 9): any NPC but the player's base and the
  // figures' bases reads another form id and keeps the engine's answer
  if (!npc || npc->formID != kPlayerBaseFormId) {
    return g_hasOverlaysOriginal(npc);
  }
  const bool engine = g_hasOverlaysOriginal(npc);
  if (!engine) {
    return engine;
  }
  // with no player or base to compare against, nothing is overridden
  auto* player = RE::PlayerCharacter::GetSingleton();
  const RE::TESNPC* playerBase = player ? player->GetActorBase() : nullptr;
  if (!playerBase) {
    return engine;
  }
  if (npc == playerBase) {
    // the player's own base keeps the engine's true (its own overlay parts)
    // while its race carries a head part list flag; the engine's form id 7
    // branch reads neither, so this hook gives a race with neither no
    // overlay on the player (thuum D-E1 A'', above)
    const RE::TESRace* race = npc->GetRace();
    if (!race ||
        race->data.flags.any(RE::RACE_DATA::Flag::kOverlayHeadPartList,
                             RE::RACE_DATA::Flag::kOverrideHeadPartList)) {
      return engine;
    }
    if (g_hasOverlaysOwnLastRace.exchange(
          race->formID, std::memory_order_relaxed) != race->formID) {
      // GetRaceData's slot is 1.6.1170's layout; on 1.7.104 it reads wrong,
      // so the chargen race printed there is no evidence (the log alone)
      const RE::TESRace* chargenRace = player->GetRaceData().charGenRace;
      logger::info("[verb:appearance] HasOverlays on the player's own base: "
                   "its race {:x} carries no Overlay Head Part List flag "
                   "nor an override list, so the engine's true returned "
                   "false (its chargen race {:x}; thread {})",
                   race->formID, chargenRace ? chargenRace->formID : 0u,
                   GetCurrentThreadId());
    }
    g_hasOverlaysOwnOverrides.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // the base first, then the override, so the reporter's read between the
  // two never shows an override from a base it has not counted
  const bool first = HasOverlaysFirstOverride(npc);
  if (first) {
    g_hasOverlaysBases.fetch_add(1, std::memory_order_relaxed);
  }
  g_hasOverlaysOverrides.fetch_add(1, std::memory_order_relaxed);
  if (first) {
    // the races the HYPOTHESIS names (the base's, the player's and its
    // chargen race, PlayerCharacter.h:823-831) and the thread, for the lab
    const RE::TESRace* race = npc->GetRace();
    const RE::TESRace* playerRace = player->GetRace();
    const RE::TESRace* chargenRace = player->GetRaceData().charGenRace;
    logger::info("[verb:appearance] HasOverlays on a figure's base (form id "
                 "7, not the player's): the engine's true returned false "
                 "(base {:#x}, its race {:x}; the player's race {:x}, its "
                 "chargen race {:x}; thread {})",
                 reinterpret_cast<std::uintptr_t>(npc),
                 race ? race->formID : 0u,
                 playerRace ? playerRace->formID : 0u,
                 chargenRace ? chargenRace->formID : 0u, GetCurrentThreadId());
  }
  return false;
}

// the count, every ten seconds while it moves (thuum ADR-069's note: a
// fault's absence alone does not show that the fix fired); Rotfern's Fixes'
// reporter shape (thuum lab/rotferns-fixes/src/main.cpp StartSkipReporter),
// a detached thread as LoadGame.cpp's
void StartHasOverlaysReporter()
{
  std::thread([] {
    uint64_t reported = 0;
    uint64_t reportedOwn = 0;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      const uint64_t own =
        g_hasOverlaysOwnOverrides.load(std::memory_order_relaxed);
      if (own != reportedOwn) {
        logger::info("[verb:appearance] HasOverlays on the player's own "
                     "base: the engine's true returned false {} time(s) so "
                     "far (its race carries neither head part list flag)",
                     own);
        reportedOwn = own;
      }
      const uint64_t now =
        g_hasOverlaysOverrides.load(std::memory_order_relaxed);
      if (now != reported) {
        logger::info("[verb:appearance] HasOverlays on figures' bases: the "
                     "engine's true returned false {} time(s) so far, on {} "
                     "base(s)",
                     now, g_hasOverlaysBases.load(std::memory_order_relaxed));
        reported = now;
      }
    }
  }).detach();
}
}

void InstallHasOverlaysHook()
{
  // an id the Address Library lacks stops the plugin at static
  // initialization (the port's patch 05) before this runs, so this line
  // catches a zero address alone
  const std::uintptr_t address = Offsets::Hooks::HasOverlays.address();
  if (!address) {
    logger::error("[verb:appearance] HasOverlays hook not installed: "
                  "Offset::TESNPC::HasOverlays names no address on this "
                  "runtime");
    return;
  }
  if (!Frida::HookHandler::GetSingleton()->Replace(
        address, reinterpret_cast<void*>(&HasOverlaysReplacement),
        reinterpret_cast<void**>(&g_hasOverlaysOriginal))) {
    logger::error("[verb:appearance] Gum refused the HasOverlays "
                  "replacement: a figure's base and the player's own "
                  "keep the engine's answer");
    return;
  }
  StartHasOverlaysReporter();
  // the lab tells a build with the hook by this line, and its address by
  // `just addr` (1.6.1170: 24790 at +3c1b40)
  logger::info("[verb:appearance] HasOverlays hook installed: "
               "TESNPC::HasOverlays (CommonLibSSE-NG Offset::TESNPC::"
               "HasOverlays, Address Library {} at +{:x}); a figure's base "
               "gets false where the engine says true, and so does the "
               "player's own when its race carries neither head part list "
               "flag",
               RE::Offset::TESNPC::HasOverlays.id(),
               address - REL::Module::get().base());
}

/**
 * Render Cursor Menu hook
 */
bool g_allowHideCursorMenu = true;

void OnRenderCursorMenuEnter(GumInvocationContext* ic)
{
  auto menu = FridaHooksUtils::GetMenuByName(RE::CursorMenu::MENU_NAME);
  auto this_ = reinterpret_cast<int64_t*>(ic->cpu_context->rcx);
  if (!this_ || !g_allowHideCursorMenu || this_ != menu) {
    return;
  }

  auto& visibleFlag = CEFUtils::DX11RenderHandler::Visible();
  auto& focusFlag = CEFUtils::DInputHook::ChromeFocus();
  if (visibleFlag && focusFlag) {
    FridaHooksUtils::SetMenuNumberVariable(RE::CursorMenu::MENU_NAME,
                                           "_root.mc_Cursor._alpha", 0);
  } else {
    FridaHooksUtils::SetMenuNumberVariable(RE::CursorMenu::MENU_NAME,
                                           "_root.mc_Cursor._alpha", 100);
  }

  auto strings = {
    R"(_root.MenuHolder.Menu_mc.MainListHolder.List_mc._alpha)"
  };

  if (visibleFlag && focusFlag) {
    for (auto string : strings) {
      FridaHooksUtils::SetMenuNumberVariable(RE::MainMenu::MENU_NAME, string,
                                             0);
    }
  } else {
    for (auto string : strings) {
      FridaHooksUtils::SetMenuNumberVariable(RE::MainMenu::MENU_NAME, string,
                                             100);
    }
  }
}

void InstallRenderCursorMenuHook()
{
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::RENDER_CURSOR_MENU,
    Offsets::Hooks::RenderCursorMenu.address(),
    std::make_shared<Frida::Hook>(OnRenderCursorMenuEnter, nullptr));
}

// void OnFunctionRegistered(RE::BSScript::NativeFunction* func)
// {
//         // GetNativeFunctionAddr::Result result =
//         GetNativeFunctionAddr::Run(*func);

//         // auto offset = ((uint64_t)func->_callback) -
//         ((uint64_t)REL::Module::get().base());

//         const char *className = func->GetClassName()->data();
//         const char *name = func->GetName()->data();

//         // uint64_t returnType = ~0;
//         // returnType = (uint64_t)func->GetReturnType(&returnType);
//         // auto t = GetCppTypeName(returnType);

//         // std::string params;
//         // uint64_t numParams = func->GetNumParams();
//         // for (uint64_t i = 0; i < numParams; i++) {
//         //     RE::BSFixedString a_nameOut;
//         //     uint64_t a_typeOut;
//         //     a_typeOut = (uint64_t)func->GetParam(i, &a_nameOut,
//         &a_typeOut);

//         //     params += GetCppTypeName(a_typeOut);
//         //     params += ' ';
//         //     params += a_nameOut.data();
//         //     if (i != numParams - 1) {
//         //         params += ", ";
//         //     }
//         // }

//         // file << std::hex << "static REL::Relocation<" << t << " (*)(" <<
//         params << ")> " << className << '_' << name << "{ REL::Offset(0x" <<
//         offset << ") };" << std::endl;
// }

void InstallNativeFunctionCtorHook() noexcept
{
  // using func_t =
  // std::add_pointer_t<RE::BSScript::NativeFunction*(RE::BSScript::NativeFunction*,
  // const char*, const char*, bool, std::uint32_t)>; REL::Relocation<func_t>
  // nativeFunctionCtor{ REL::Offset(0x03076DE8) };

  // struct State
  // {
  //     uint64_t thisarg = 0;
  // };
  // std::shared_ptr<State> state(new State);
  // Frida::Hook            hook3{
  //     [state](GumInvocationContext* context) {
  //         auto self      =
  //         (RE::BSScript::NativeFunction*)context->cpu_context->rcx;
  //         state->thisarg = (uint64_t)self;
  //     },
  //     [state](GumInvocationContext* context) {
  //         auto self = (RE::BSScript::NativeFunction*)state->thisarg;
  //         OnFunctionRegistered(self);
  //     }
  // };

  // Frida::HookHandler::GetSingleton()->Install(Frida::HookID::NATIVE_FUNCTION_CTOR,
  // nativeFunctionCtor.address(), std::make_shared<Frida::Hook>(hook3));
}

/**
 * A figure's bar is the server's alone (thuum docs/verbs/figure-values.md,
 * train-30). A figure (another player's actor in this game) carries its
 * player's real maxima, so this game's own damage to it, the player's melee
 * and arrows, an NPC's hit, a trap, would dent its bar before the server's
 * percentage arrives. The HandleHealthDamage vfunc the train-28 guard
 * dropped is reached from inside DamageActor after the write (the Frida
 * read of 2026-10-20), so the gate is DamageActor itself: replaced through
 * Gum, the replacement answers false for a figure victim, the engine's own
 * answer for an invulnerable one (nothing written, not dead), and calls the
 * original for every other actor. The hit event, the impact, the blood and
 * the stagger come earlier in the chain and are untouched. A figure is
 * known by the mark skymp5-client puts on every one at spawn,
 * attackDamageMult 0 (view/formView.ts), on a dynamic form id. HYPOTHESIS
 * until b-figure-values' hit steps: the replacement's calling convention is
 * the function's (Windows x64: this in RCX, the float second argument in
 * XMM1, R8, R9B), and the figure's own hit reactions stay.
 */
namespace {
bool IsFigureVictim(RE::Actor* actor)
{
  if (!actor || actor->formID < 0xFF000000) {
    return false;
  }
  auto owner = actor->AsActorValueOwner();
  return owner &&
    owner->GetActorValue(RE::ActorValue::kAttackDamageMult) == 0.f;
}

using DamageActorFn = bool (*)(RE::Actor* victim, float damage,
                               RE::Actor* attacker, bool killMove);
DamageActorFn g_damageActorOriginal = nullptr;
std::atomic<bool> g_engineDamageCut{ true };
// whether the replacement is in: the player's cut of another actor's damage
// rests on it, so the client keeps its multipliers at 0 without it (D-T4's
// review, gap 1)
std::atomic<bool> g_damageActorReplaced{ false };

bool DamageActorReplacement(RE::Actor* victim, float damage,
                            RE::Actor* attacker, bool killMove)
{
  if (IsFigureVictim(victim)) {
    return false;
  }
  // thuum docs/DECISIONS.md ADR-028 and D-T4 (docs/research/
  // trap-slice-d-s3-2026-11-05.md): the damage another actor deals the
  // player is the server's (R0: its ChangeValues carry the number), so this
  // game applies none of it, whoever the attacker (a figure's hand or spell,
  // an NPC this game simulates or not). The world's damage, a hit with no
  // attacker (a trap's) or the player's own, stays the engine's, scaled by
  // the client's world multiplier (the six fDiffMultHPToPC*, skymp5-client
  // RagdollService). Until D-T4 the cut was that multiplier at 0, which
  // zeroed a trap's damage too (x-trap-hit-premise 20261105-154820: a spike
  // wall's hit cost the player nothing, 60 at 1.0). A fall read unzeroed
  // under that 0 (b-push), so its path is HYPOTHESIS, as are the other
  // attacker-less paths (37336's environmental damage, a hazard a figure's
  // spell placed, an owner that no longer resolves; docs/verbs/traps.md).
  // Not under the NPC rules' lever below: the player's cut always held
  if (victim && victim->formID == 0x14 && attacker && attacker != victim) {
    return false;
  }
  // thuum docs/verbs/npc-fate.md (ADR-050 decision 2, deaths by the engine):
  // a puppet here takes no engine damage (the record's values come as
  // ChangeValues); an NPC this game simulates takes none from the player's
  // or a figure's hand (the server's number arrives as a cause through
  // Frida::DamageActorOriginal, the one number); an NPC's hand on an NPC
  // stays the engine's (the host reports the hit and the absolute cause
  // corrects it). The player's own actor is cut above, for another actor's
  // damage alone.
  if (g_engineDamageCut.load(std::memory_order_relaxed) && victim &&
      victim->formID != 0x14) {
    if (Puppets::Is(victim->formID)) {
      return false;
    }
    if (attacker && (attacker->formID == 0x14 || IsFigureVictim(attacker))) {
      return false;
    }
  }
  return g_damageActorOriginal(victim, damage, attacker, killMove);
}
}

bool Frida::IsDamageActorReplaced()
{
  return g_damageActorReplaced.load(std::memory_order_relaxed);
}

bool Frida::DamageActorOriginal(RE::Actor* victim, float damage,
                                RE::Actor* attacker, bool killMove)
{
  if (!g_damageActorOriginal || !victim) {
    return false;
  }
  return g_damageActorOriginal(victim, damage, attacker, killMove);
}

void Frida::SetEngineDamageCut(bool on)
{
  g_engineDamageCut.store(on, std::memory_order_relaxed);
}

void InstallDamageActorHook()
{
  if (!REL::Module::IsAE()) {
    return;
  }
  const bool replaced = Frida::HookHandler::GetSingleton()->Replace(
    Offsets::Hooks::DamageActor.address(),
    reinterpret_cast<void*>(&DamageActorReplacement),
    reinterpret_cast<void**>(&g_damageActorOriginal));
  g_damageActorReplaced.store(replaced, std::memory_order_relaxed);
}

/**
 * thuum tracker #178 and #175 (Eli's third look of playtest 56's seat loop,
 * 2026-11-19: "vampiric grip MOSTLY works, even if a little jumpy";
 * "fusrohdah-ing another player is still suuuuuper janky and desynced. THAT
 * one needs to be one to one"): the get-up hold, the shout design's step 3.
 * A follower's copy of another seat's knocked actor is knocked in place and
 * its parts driven to the stream (PropService, PuppetApi's imposeBodies), and
 * its own engine got it up mid-flight, so the client knocked it again: c2's
 * copy of the gripped 39fe2 six times in 18 s, c1's of a shouted player three.
 * The re-analyst's static read (pyghidra-mcp on sky-re, CONFIRMED BY STATIC
 * READ, high): the knock step (39896, once a frame) calls the get-up 40150
 * (Actor*, bool playAction) -> void (Ghidra 0x140723110, `just addr 40150`
 * 0x723110 on 1.6.1170), directly or, when the task queue is in use, through
 * a queued task that calls it later (Ghidra FUN_1407117f0; the review of the
 * one-throw build), either way through this replacement, from knock state 1
 * or 3 when the actor is alive, not
 * underwater, not the player's grabbedObject, and its rest test (37689, the
 * root's and a part's squared linear velocity under 0.025) passes or its 15
 * s timer runs out; it zeroes that timer after the call, so a get-up that
 * returns at once is asked again on the next frame and the knock state
 * stands. Replaced through Gum as DamageActor is: a held form's get-up
 * returns before any of its body runs (37336's fall damage, the controller
 * probe, the ActionGetUp, knock state 5), every other actor's is the
 * original. HYPOTHESIS the read raises (not traced): a drive that writes
 * poses without velocities read "at rest" at once. The Frida plan (39896,
 * 37689, 37336; lab/frida/shout-getup-trace.js) is the next read on Eli's
 * word. Declared limits (the review): the knock step zeroes its own timer
 * after a refused call, so a hold lifted with no owner's get-up behind it (a
 * quiet stream, an untrack, a take-over) stands the copy up on its next
 * frame; and a placing release's task may lift a hold a newer follow stored
 * before it ran, the next store holding it again
 */
namespace {
using ActorGetUpFn = void (*)(RE::Actor* actor, bool playAction);
ActorGetUpFn g_actorGetUpOriginal = nullptr;

// a store refreshes a form's hold; past this without one it lapses (a dead
// stream, a follow that ended with no release): the follow's frames store
// every update while its stream lives, as knockState's followedDownMs
constexpr uint64_t kGetUpHoldMs = 2000;
// forms held at once before the lapsed ones are swept (imposeBodies holds 64)
constexpr std::size_t kGetUpHeldMax = 128;

struct GetUpHold
{
  uint64_t storedMs = 0;
  uint32_t refused = 0;
};

std::mutex g_getUpMutex;
std::unordered_map<uint32_t, GetUpHold> g_getUpHeld;

void ActorGetUpReplacement(RE::Actor* actor, bool playAction)
{
  if (actor) {
    const uint32_t formId = actor->formID;
    bool held = false;
    uint32_t refused = 0;
    {
      std::lock_guard<std::mutex> lock(g_getUpMutex);
      auto it = g_getUpHeld.find(formId);
      if (it != g_getUpHeld.end()) {
        if (NowMs() - it->second.storedMs <= kGetUpHoldMs) {
          held = true;
          refused = ++it->second.refused;
        } else {
          g_getUpHeld.erase(it);
        }
      }
    }
    if (held) {
      if (refused == 1) {
        logger::info("[verb:prop-physics] get-up of {:x} held: the drive "
                     "imposes another seat's stream on it (thuum tracker "
                     "#178)",
                     formId);
      }
      return;
    }
  }
  g_actorGetUpOriginal(actor, playAction);
}
}

void Frida::HoldGetUp(uint32_t formId)
{
  // the player's own get-up is held too since thuum tracker #179 (its body
  // its gripper's for a grip's hold, PuppetApi's ImposeBodies): the player's
  // knock step and get-up are the same 39896 and 40150 (the re-analyst's
  // static read of 2026-11-19). HYPOTHESIS: while held, its game keeps the
  // knock's bleedout camera and its 0.8 time slowdown, which the first get-up
  // 40150 runs clears
  if (formId == 0) {
    return;
  }
  const uint64_t now = NowMs();
  std::lock_guard<std::mutex> lock(g_getUpMutex);
  if (g_getUpHeld.count(formId) == 0 && g_getUpHeld.size() >= kGetUpHeldMax) {
    for (auto it = g_getUpHeld.begin(); it != g_getUpHeld.end();) {
      if (now - it->second.storedMs > kGetUpHoldMs) {
        it = g_getUpHeld.erase(it);
      } else {
        ++it;
      }
    }
    if (g_getUpHeld.size() >= kGetUpHeldMax) {
      return;
    }
  }
  g_getUpHeld[formId].storedMs = now;
}

void Frida::ReleaseGetUp(uint32_t formId)
{
  uint32_t refused = 0;
  bool was = false;
  {
    std::lock_guard<std::mutex> lock(g_getUpMutex);
    auto it = g_getUpHeld.find(formId);
    if (it != g_getUpHeld.end()) {
      was = true;
      refused = it->second.refused;
      g_getUpHeld.erase(it);
    }
  }
  if (was && refused > 0) {
    logger::info("[verb:prop-physics] get-up of {:x} released after {} "
                 "refusal(s): its engine's next knock step gets it up",
                 formId, refused);
  }
}

void InstallActorGetUpHook()
{
  if (!REL::Module::IsAE()) {
    return;
  }
  const bool replaced = Frida::HookHandler::GetSingleton()->Replace(
    Offsets::Hooks::ActorGetUp.address(),
    reinterpret_cast<void*>(&ActorGetUpReplacement),
    reinterpret_cast<void**>(&g_actorGetUpOriginal));
  logger::info("[verb:prop-physics] get-up hold (40150) {}",
               replaced ? "installed"
                        : "NOT installed: a held copy gets up "
                          "by its own engine");
}

/**
 * thuum docs/verbs/guest-witness.md (tracker #193): a puppet's detection of
 * this game's player is its host's. The engine's detection reader,
 * Actor::RequestDetectionLevel (CommonLibSSE-NG include/RE/A/Actor.h:619,
 * Offset::Actor::RequestDetectionLevel), answers from the detector's own
 * knowledge of the target, which the detector's AI fills; a puppet's AI is
 * off here, so a theft in a puppet's full view finds no witness in the
 * thief's StealAlarm (37422), whose owner and witness tests are this reader.
 * Replaced through Gum: for this game's player as the target and a puppet as
 * the detector, the level its host relayed (setPuppetDetection,
 * PuppetDetection.h); the original for every other call. Runs on the
 * engine's job threads: the player compare first, then one registry read
 * under its lock. HYPOTHESIS until a look reads a guest's theft charged
 */
namespace {
using RequestDetectionLevelFn = std::int32_t (*)(
  RE::Actor* self, RE::Actor* target, RE::DETECTION_PRIORITY priority);
RequestDetectionLevelFn g_requestDetectionLevelOriginal = nullptr;
std::atomic<uint64_t> g_guestWitnessAnswers{ 0 };

std::int32_t RequestDetectionLevelReplacement(RE::Actor* self,
                                              RE::Actor* target,
                                              RE::DETECTION_PRIORITY priority)
{
  if (self && target && target == RE::PlayerCharacter::GetSingleton()) {
    const auto read = Puppets::ReadDetection(self->formID);
    if (const auto answer =
          PuppetDetection::Answer(true, read.isPuppet, read.relayed)) {
      const uint64_t n = ++g_guestWitnessAnswers;
      if (n <= 5 || n % 1000 == 0) {
        logger::info("[verb:guest-witness] puppet {:x}'s detection of this "
                     "game's player answered by its host: level {} (read {})",
                     self->formID, *answer, n);
      }
      return *answer;
    }
  }
  return g_requestDetectionLevelOriginal
    ? g_requestDetectionLevelOriginal(self, target, priority)
    : PuppetDetection::kUndetected;
}
}

void InstallGuestWitnessHook()
{
  if (!REL::Module::IsAE()) {
    logger::info(
      "[verb:guest-witness] the detection hook installs on AE only");
    return;
  }
  const bool replaced = Frida::HookHandler::GetSingleton()->Replace(
    RE::Offset::Actor::RequestDetectionLevel.address(),
    reinterpret_cast<void*>(&RequestDetectionLevelReplacement),
    reinterpret_cast<void**>(&g_requestDetectionLevelOriginal));
  logger::info("[verb:guest-witness] detection hook (37764) {}",
               replaced ? "installed: a puppet's detection of this game's "
                          "player answers its host's relayed level"
                        : "NOT installed: a theft in a puppet's view finds "
                          "no witness here");
}

/**
 * thuum docs/verbs/leveling.md (M5): the player's skill experience is the
 * server's. The engine's one funnel for it is PlayerCharacter::UseSkill
 * (game/Offsets.h UseSkill, 40488), called once per landed hit, cast or
 * craft with the skill, the action's base amount (an iron sword's 7, the
 * weapon's base damage) and the source form, the skill's use mult applied
 * inside (x-skill-use-read v3, 20261031-094735). Replaced through Gum: the
 * replacement raises `skillUse` to JS on the next tick (the skill as its
 * ActorValue, the points, the source's form id) and calls the original, whose
 * write is nothing under skymp5-client's zeroed mults
 * (DisableSkillAdvanceService); skillUseService batches a second's uses into
 * SkillUse (MsgType 63) for the server's formulas (wire-rules leveling).
 */
namespace {
using UseSkillFn = void (*)(RE::PlayerCharacter* self, RE::ActorValue skill,
                            float points, RE::TESForm* source);
UseSkillFn g_useSkillOriginal = nullptr;

void UseSkillReplacement(RE::PlayerCharacter* self, RE::ActorValue skill,
                         float points, RE::TESForm* source)
{
  const uint32_t skillId = static_cast<uint32_t>(skill);
  const uint32_t sourceId = source ? source->formID : 0;
  SkyrimPlatform::GetSingleton()->AddTickTask(
    [skillId, points, sourceId](Napi::Env env) {
      auto obj = Napi::Object::New(env);
      AddObjProperty(&obj, "skill", skillId);
      AddObjProperty(&obj, "points", points);
      AddObjProperty(&obj, "source", sourceId);
      EventsApi::SendEvent("skillUse", { obj });
    });
  if (g_useSkillOriginal) {
    g_useSkillOriginal(self, skill, points, source);
  }
}
}

void InstallUseSkillHook()
{
  // AE only: the id is 1.6.1170's (game/Offsets.h)
  if (!REL::Module::IsAE()) {
    logger::info("[verb:leveling] UseSkill hook not installed: not AE");
    return;
  }
  Frida::HookHandler::GetSingleton()->Replace(
    Offsets::Hooks::UseSkill.address(),
    reinterpret_cast<void*>(&UseSkillReplacement),
    reinterpret_cast<void**>(&g_useSkillOriginal));
  // the lab tells one platform build from another by these install lines
  // (x-skill-use v1's read, 2026-10-31: two seats compared by them)
  logger::info("[verb:leveling] UseSkill hook installed: "
               "PlayerCharacter::AddSkillExperience (Address Library 40488), "
               "one skillUse per call");
}

// thuum tracker #123 (playtest 55, the spoken-line read; FridaHooks.h
// SpokenLine): the rows by speaker. Written from whatever thread calls the
// engine's 35225 (the VM thread inside our sayTopicInfo, the AI and scene
// updates on a host, QueueActorAnimation's inside DriveVoice) and from
// SayTopicInfo's game-thread task; read on the JavaScript thread
// (PuppetApi.cpp SpokenLines). Under one mutex, which guards the table alone
namespace {
constexpr size_t kSpokenRowsMax = 64;
constexpr size_t kSpokenSpeakersMax = 256;
// the most responses walked for an item's current one (ResponseIndexOf)
constexpr std::int32_t kSpokenResponsesMax = 64;

struct SpokenSpeaker
{
  std::deque<Frida::SpokenLine> rows;
  // the table's write count at this speaker's last row (the eviction's
  // order: the least recently written goes first)
  std::uint64_t lastWrite = 0;
};

struct SpokenTable
{
  std::mutex m;
  std::unordered_map<std::uint32_t, SpokenSpeaker> speakers;
  std::uint64_t writes = 0;
};

SpokenTable& Spoken()
{
  static SpokenTable table;
  return table;
}

void KeepSpokenLine(std::uint32_t speakerId, const Frida::SpokenLine& row)
{
  auto& t = Spoken();
  std::lock_guard<std::mutex> lock(t.m);
  auto it = t.speakers.find(speakerId);
  if (it == t.speakers.end()) {
    if (t.speakers.size() >= kSpokenSpeakersMax) {
      const auto oldest =
        std::min_element(t.speakers.begin(), t.speakers.end(),
                         [](const auto& a, const auto& b) {
                           return a.second.lastWrite < b.second.lastWrite;
                         });
      t.speakers.erase(oldest);
    }
    it = t.speakers.try_emplace(speakerId).first;
  }
  auto& speaker = it->second;
  if (speaker.rows.size() >= kSpokenRowsMax) {
    speaker.rows.pop_front();
  }
  speaker.rows.push_back(row);
  speaker.lastWrite = ++t.writes;
}

// the item's current response's place in its responses, -1 when none:
// BSSimpleList's head node and its iterator's node are protected
// (CommonLibSSE-NG include/RE/B/BSTList.h:150-153, :333), so the list is
// walked through the public iterator (each ++ follows the node's next) and
// each node's item address is compared with the current node's
// (Node::item is public, :77; DialogueItem.h:54-55); at most
// kSpokenResponsesMax steps
std::int32_t ResponseIndexOf(RE::DialogueItem& item)
{
  const auto* current = item.currentResponse;
  if (!current) {
    return -1;
  }
  std::int32_t index = 0;
  for (auto it = item.responses.begin();
       it != item.responses.end() && index < kSpokenResponsesMax;
       ++it, ++index) {
    if (&(*it) == &current->item) {
      return index;
    }
  }
  return -1;
}
}

void Frida::RecordSpokenLine(RE::Actor* speaker, RE::DialogueItem* item,
                             std::uint32_t type, bool gated) noexcept
{
  // inside the engine's own frames (the 35225 hook): nothing may unwind
  try {
    if (!speaker) {
      return;
    }
    SpokenLine row;
    row.epochMs = static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count());
    row.type = type;
    row.gated = gated ? 1u : 0u;
    if (item) {
      row.infoId = item->info ? item->info->formID : 0u;
      row.topicId = item->topic ? item->topic->formID : 0u;
      row.responseIndex = ResponseIndexOf(*item);
    }
    // field reads only, no relocation call (BSSoundHandle::IsValid and
    // IsPlaying are the engine's): the voice handle's sound id
    // (HighProcessData.h:282, BSSoundHandle.h:51) and the Actor runtime
    // voiceTimer DriveVoice reads (Actor.h:656). The id can outlive its
    // sound (nothing read here resets it after a line), hence the timer
    // beside it
    auto& runtime = speaker->GetActorRuntimeData();
    row.voiceTimerSec = runtime.voiceTimer;
    auto* high =
      runtime.currentProcess ? runtime.currentProcess->high : nullptr;
    row.handleValid =
      high && high->soundHandles[0].soundID != RE::BSSoundHandle::kInvalidID
      ? 1u
      : 0u;
    KeepSpokenLine(speaker->formID, row);
  } catch (...) {
  }
}

std::vector<Frida::SpokenLine> Frida::ReadSpokenLines(std::uint32_t speakerId,
                                                      std::uint64_t sinceMs)
{
  std::vector<SpokenLine> out;
  auto& t = Spoken();
  std::lock_guard<std::mutex> lock(t.m);
  const auto it = t.speakers.find(speakerId);
  if (it == t.speakers.end()) {
    return out;
  }
  for (const auto& row : it->second.rows) {
    if (row.epochMs >= sinceMs) {
      out.push_back(row);
    }
  }
  return out;
}

// thuum docs/verbs/npc-stream.md (the voice slice), playtest 37 (2026-10-28,
// Eli: "if i ask 'what do you have for sale' the trading menue is opened for
// both people"): a relayed line a puppet says here runs its info's script
// fragments on this game too, because the engine's only sender of the
// TESTopicInfoEvent, DialogueItem::SendTopicInfoEvent, tests nothing about
// the speaker's seat (the re-analyst's read, docs/research/m3/
// re-puppet-head-lips.md A: the BEGIN from 39643 inside our sayTopicInfo,
// the END from UpdateSoundCallBack inside our voice pair). Replaced through
// Gum: when the item's speaker is a puppet here and not in dialogue with
// this player (HighProcessData.h:341 talkingToPC), the event is not sent and
// the fragment never runs on the watcher; the host's own NPCs are not
// puppets there and keep every fragment. HYPOTHESIS until the probe
// (x-npc-vendor: the host asks a merchant's wares, the watcher's BarterMenu
// stays shut) or the Frida plan in the read (35225, 25616, 53997).
namespace {
using SendTopicInfoEventFn = std::uint64_t (*)(RE::DialogueItem* item,
                                               std::uint32_t type, bool force);
SendTopicInfoEventFn g_sendTopicInfoEventOriginal = nullptr;

std::uint64_t SendTopicInfoEventReplacement(RE::DialogueItem* item,
                                            std::uint32_t type, bool force)
{
  if (item && item->speaker) {
    auto* speaker = item->speaker->As<RE::Actor>();
    // the gate's verdict first, then the line recorded (thuum tracker #123,
    // Frida::RecordSpokenLine) before either path: a dropped event's row
    // carries gated 1
    bool gated = false;
    if (speaker && Puppets::Is(speaker->formID)) {
      auto* process = speaker->GetActorRuntimeData().currentProcess;
      auto* high = process ? process->high : nullptr;
      gated = !(high && high->talkingToPC);
    }
    if (speaker) {
      Frida::RecordSpokenLine(speaker, item, type, gated);
    }
    if (speaker && gated) {
      // one info line a minute per speaker, the rest counted (the game
      // thread's own path, no lock)
      static std::unordered_map<
        std::uint32_t,
        std::pair<std::chrono::steady_clock::time_point, std::uint32_t>>
        dropped;
      auto& [since, count] = dropped[speaker->formID];
      ++count;
      const auto now = std::chrono::steady_clock::now();
      if (since == std::chrono::steady_clock::time_point() ||
          now - since >= std::chrono::minutes(1)) {
        logger::info("[verb:npc-stream] fragment gate: info {:x} type {} "
                     "on puppet {:x} not sent to the VM (the watcher's "
                     "say); {} dropped since the last line",
                     item->info ? item->info->formID : 0u, type,
                     speaker->formID, count);
        since = now;
        count = 0;
      }
      return 0;
    }
  }
  return g_sendTopicInfoEventOriginal(item, type, force);
}
}

void InstallSendTopicInfoEventHook()
{
  if (!REL::Module::IsAE()) {
    return;
  }
  Frida::HookHandler::GetSingleton()->Replace(
    Offsets::Hooks::SendTopicInfoEvent.address(),
    reinterpret_cast<void*>(&SendTopicInfoEventReplacement),
    reinterpret_cast<void**>(&g_sendTopicInfoEventOriginal));
}

/**
 * The VM's quest stage sink (thuum docs/verbs/quest-stage.md, slice two;
 * ADR-056's known limit: a world-shared stage the server relays is applied
 * on this seat through Quest.SetCurrentStageID, and the stage's Papyrus
 * fragment, the QF_ script's Fragment_N, ran here too, so a reward was given
 * twice). The engine dispatches the fragment from SkyrimVM::ProcessEvent
 * (const TESQuestStageEvent*, BSTEventSource<TESQuestStageEvent>*):
 * CommonLibSSE-NG declares the sink sub-object at SkyrimVM+0xF0
 * (include/RE/S/SkyrimVM.h:116, the 31st base, so its vtable is the one
 * Offsets_VTABLE.h:4307 lists as VTABLE_SkyrimVM's entry 30, AE 217115),
 * the virtual (BSTEvent.h:146), the event (TESQuestStageEvent.h:5-14:
 * finishedCallback +0, formID +8, stage +0xC, unk0E +0xE, the stage's log
 * entry index per the read) and the source (ScriptEventSourceHolder.h:95),
 * and no body. The body is the re-analyst's Ghidra read on 1.6.1170
 * (docs/research/m3/re-quest-fragment.md, 14:08 PDT 2026-10-29): the
 * FragmentSystem (SkyrimVM.h:228) is asked for the stage's script and
 * fragment names by (formID, stage, entry); found, the VM call is dispatched
 * with a functor that fires the callback's virtual slot 1 when the stack
 * ends; not found, slot 1 fires inline; kContinue either way. Slot 1 (the
 * StageItemFinishedCallback's, whose vtable CommonLib names at
 * Offsets_VTABLE.h:939 and whose class it does not declare) sends the
 * TESQuestStageItemDoneEvent that clears the StoryTeller's wait and returns
 * the latent SetStage.
 *
 * Replaced through Gum (the DamageActor shape; Address Library 1.6.1170
 * 53987, just addr 2026-10-29, offset 0x9c6b70): when the event's (formID,
 * stage) is a pair skymp5-client's questStageService is applying from the
 * server's relay (TESModPlatform.SuppressQuestStageFragment before its
 * SetStage: one mark per apply), the mark is consumed and the callback's
 * slot 1 fires if the callback is non-null and kContinue returns with no
 * lookup and no dispatch, the engine's own no-fragment branch: the done bit,
 * currentStage, the journal entry and its message, the complete and fail
 * bits and the StoryTeller's bookkeeping are the setter's before the event
 * and stay, the latent return comes from slot 1 as before; only the
 * fragment is skipped. Any other pair goes to the original. Keyed by pairs,
 * never a flag window: the VM may queue a stage on an unstarted quest
 * (53711) and set it later (53713), which is why the mark is the sink's to
 * consume at the pair's first event and not the client's to take back at
 * the promise's answer (the review of m3-pass-train-2, finding 4: the
 * answer can come before the queued set, and a released mark would miss the
 * event). ReleaseQuestStageFragment is the client's safety, 10 s after the
 * apply: it takes back one mark if the pair still holds one (a refused
 * stage, a quest never started), so no mark leaks; two applies of one pair
 * are two marks and two consumes. One-shot per event is a HYPOTHESIS of its
 * own: the static read has the setter send one event per log entry of the
 * stage (25260), so a stage with several passing entries would run its
 * later entries' fragments; lab/frida/quest-stage-chain.js counts the
 * events per stage (53987) and decides whether the consume moves to the
 * last entry (53716's count). HYPOTHESIS until that trace reads it (the
 * falsifiers in the verb doc: a 54240 for a suppressed pair, a 56684 with
 * no 53763).
 */
namespace {
using QuestStageSinkFn = RE::BSEventNotifyControl (*)(
  void* sink, const RE::TESQuestStageEvent* event,
  RE::BSTEventSource<RE::TESQuestStageEvent>* source);
QuestStageSinkFn g_questStageSinkOriginal = nullptr;

// the pairs under suppression, (quest form id, stage) -> the marks the pair
// holds: one per apply in flight, each consumed by the pair's next event
// (or taken back by the client's safety). The natives write on the VM's
// thread; the sink reads on the setter's (Quest.SetCurrentStageID sends the
// event from inside its own native call, a console SetStage from the game
// thread), so a lock, the castReady shape above; never held across the
// callback or the original
std::mutex g_questStageSuppressMutex;
std::unordered_map<std::uint64_t, std::uint32_t> g_questStageSuppressed;

std::uint64_t QuestStageKey(std::uint32_t questFormId, std::uint16_t stage)
{
  return (static_cast<std::uint64_t>(questFormId) << 16) | stage;
}

// The stage callback the event carries, called through its vtable.
// HYPOTHESIS (docs/research/m3/re-quest-fragment.md, lines 13 to 15): a
// StageItemFinishedCallback is 0x18 bytes, its vtable at +0 (CommonLib's
// VTABLE___StageItemFinishedCallback, 1.6.1170 id 195903, names the table
// and no slot), formID at +0x10, stage at +0x14, entry at +0x16; its slot 1
// is 25234, `void (this)`, the sender of the TESQuestStageItemDoneEvent,
// which the engine's own no-fragment branch calls inline with no other
// argument. No CommonLib header declares the class, so the slot is read
// here, and it runs only for a suppressed pair, never for a stage of this
// game's own.
struct StageItemFinishedCallbackView
{
  std::uintptr_t* vtbl;
};
using StageItemFinishedSlot1Fn = void (*)(void* self);

void FireStageItemFinished(void* callback)
{
  auto* view = static_cast<StageItemFinishedCallbackView*>(callback);
  if (!view || !view->vtbl) {
    return;
  }
  reinterpret_cast<StageItemFinishedSlot1Fn>(view->vtbl[1])(callback);
}

RE::BSEventNotifyControl QuestStageSinkReplacement(
  void* sink, const RE::TESQuestStageEvent* event,
  RE::BSTEventSource<RE::TESQuestStageEvent>* source)
{
  if (event) {
    bool suppressed = false;
    std::uint32_t left = 0;
    {
      std::lock_guard<std::mutex> lock(g_questStageSuppressMutex);
      auto it = g_questStageSuppressed.find(
        QuestStageKey(event->formID, event->stage));
      if (it != g_questStageSuppressed.end()) {
        // one-shot: this event consumes one mark; the pair leaves the map
        // with its last
        suppressed = true;
        left = --it->second;
        if (left == 0) {
          g_questStageSuppressed.erase(it);
        }
      }
    }
    if (suppressed) {
      // thuum ADR-068 rung 5, D-Q3 A: a pair holding a suppress mark (the
      // login replay's, a load) and an acting request too skips its
      // fragment: suppress wins, and the request is taken here, so it arms
      // nothing later
      const bool dropped =
        ActingScope::Take(event->formID, event->stage).has_value();
      logger::warn("[verb:quest-stage] fragment of {:x} stage {} entry {} "
                   "skipped: the stage is the world's, applied from the "
                   "relay; {} mark(s) left for the pair",
                   event->formID, event->stage,
                   static_cast<unsigned>(event->unk0E), left);
      if (dropped) {
        logger::info("[verb:quest-stage] acting request for {:x} stage {} "
                     "dropped: its fragment is skipped (suppress wins, "
                     "D-Q3 A)",
                     event->formID, event->stage);
      }
      FireStageItemFinished(event->finishedCallback);
      return RE::BSEventNotifyControl::kContinue;
    }
    // thuum ADR-068 rung 5 (the acting scope, ActingScope.h; docs/research/
    // rung5-quests-2026-11-09.md 1(c)): the pair's acting request, a relayed
    // stage the client applies for another player (setActingPlayer) or a
    // stage a scoped stack set (the seam's propagation), is taken at its
    // event and arms the quest's scope for ActingScope::kArmMs before the
    // original dispatches the fragment, so the fragment's stack joins the
    // scope at its first native call (H-AP1) and runs for the acting player.
    // One atomic read for every other stage event
    if (const auto taken = ActingScope::Take(event->formID, event->stage)) {
      // the scope's root as the request carried it: kRelayed for a stage
      // relayed from another seat, its parent's for a chained one (V-scene).
      // One join, the fragment's own stack at its first native (H-AP1), so
      // a stage this seat's own engine sets on the quest inside the window
      // runs unscoped, its scene started here (HYPOTHESIS H-AP3; train 53's
      // review, correctness-2: playtest 52's c2 Fragment_9, run by c2's own
      // 195 1.57 s after the relayed 185, joined 185's arm and chained
      // rotfern's scope to 200)
      ActingScope::Arm(event->formID, taken->player, ActingScope::kArmMs,
                       taken->root, 1, event->stage);
      // a relay's scope with no acting player (setRelayedScope's) named as
      // such, never "acting player 0" (train 53's review, contracts-6)
      logger::info(
        "[verb:quest-stage] fragment of {:x} stage {} runs for "
        "{} ({})",
        event->formID, event->stage,
        taken->player.serverId != 0
          ? fmt::format("acting player {:x}", taken->player.serverId)
          : std::string("no acting player"),
        taken->origin == ActingScope::Origin::kChained ? "chained"
                                                       : "relayed");
    } else if (ActingScope::Widen(event->formID, event->stage)) {
      // another item of the stage that armed the quest (25252 runs every
      // item of a stage, 25260 sends this event for each whose conditions
      // pass; HYPOTHESIS H-AP8, a static read of 1.6.1170 in train 53's
      // review): its fragment's stack joins the arm too
      logger::info("[verb:quest-stage] fragment of {:x} stage {} entry {} "
                   "joins the stage's acting scope (another item)",
                   event->formID, event->stage,
                   static_cast<unsigned>(event->unk0E));
    }
    // thuum ADR-068 rung 5, D-Q4 C (QuestCause.h): the stage's verdict (its
    // setter's mark, the start or stop it rides, or none: a stage the engine
    // set by itself, the seat's own) arms the quest before the original
    // dispatches the fragment, so the fragment's stack carries the verdict
    // of whoever set its stage (H-QC1). A suppressed pair runs no fragment
    // and arms nothing (above)
    QuestCause::Arm(event->formID,
                    QuestCause::OfStage(event->formID, event->stage)
                      .value_or(QuestCause::Verdict{}));
  }
  return g_questStageSinkOriginal(sink, event, source);
}
}

void Frida::SuppressQuestStageFragment(uint32_t questFormId, uint16_t stage)
{
  std::uint32_t marks = 0;
  {
    std::lock_guard<std::mutex> lock(g_questStageSuppressMutex);
    marks = ++g_questStageSuppressed[QuestStageKey(questFormId, stage)];
  }
  logger::trace("[verb:quest-stage] fragment of {:x} stage {} suppressed: "
                "the relay's apply begins ({} mark(s) for the pair)",
                questFormId, stage, marks);
}

void Frida::ReleaseQuestStageFragment(uint32_t questFormId, uint16_t stage)
{
  // the client's safety, 10 s after the apply: one mark back if the pair
  // still holds one, so a stage whose event never came (refused by the
  // engine, or queued on a quest that never started) leaks none. A mark the
  // event consumed is gone already and this is a no-op.
  std::uint32_t left = 0;
  bool had = false;
  {
    std::lock_guard<std::mutex> lock(g_questStageSuppressMutex);
    auto it = g_questStageSuppressed.find(QuestStageKey(questFormId, stage));
    if (it != g_questStageSuppressed.end()) {
      had = true;
      left = --it->second;
      if (left == 0) {
        g_questStageSuppressed.erase(it);
      }
    }
  }
  if (had) {
    logger::warn("[verb:quest-stage] fragment mark of {:x} stage {} taken "
                 "back by the safety: no stage event claimed it within the "
                 "window (the stage was refused, or waits on a quest not "
                 "yet started; a later set runs its fragment); {} mark(s) "
                 "left",
                 questFormId, stage, left);
  } else {
    logger::trace("[verb:quest-stage] fragment mark of {:x} stage {}: "
                  "nothing to release, the stage's event consumed it",
                  questFormId, stage);
  }
}

void InstallQuestStageSinkHook()
{
  // AE only: the id is 1.6.1170's (game/Offsets.h)
  if (!REL::Module::IsAE()) {
    return;
  }
  if (!Frida::HookHandler::GetSingleton()->Replace(
        Offsets::Hooks::QuestStageSink.address(),
        reinterpret_cast<void*>(&QuestStageSinkReplacement),
        reinterpret_cast<void**>(&g_questStageSinkOriginal))) {
    logger::error("[verb:quest-stage] Gum refused the quest stage sink's "
                  "replacement: a relayed stage runs its fragment here");
  }
}

/**
 * The engine's reanimation of a hosted body (thuum docs/verbs/npc-fate.md,
 * playtest 37's finding 54: a thrall raised by a hosted necromancer stood on
 * the host's screen with no AI, "stands but gets no npc"). On its host a
 * dead actor keeps its AI off (PuppetApi.cpp Promote, H-54e), and the
 * Reanimate effect never passes the Character vtable's Resurrect slot that
 * Hooks.cpp HostedResurrect holds: ReanimateEffect::Start (Address Library
 * 34750) heals the health damage modifier, stores the caster as commanding
 * actor and then calls the reanimation proper, 38820, either directly (no
 * live animation graph) or from the GetUpEnd handler (42880) when the body's
 * kActionGetUp ends; Actor::MoveToMiddleHigh (37624) calls 38820 too for a
 * dead actor still carrying a commanding actor (the post-load re-entry).
 * 38820 is `void (Actor* this)`: SetLifeState(kReanimate) through 37490 into
 * 37612 (Actor::SetLifeState, CommonLibSSE-NG src/RE/A/Actor.cpp:1034),
 * actorState2.reanimating cleared, the caster's kApplyReanimateSpell perk
 * entry point, the commanding actor cleared. The read is
 * docs/research/m3/re-resurrect-hook.md (the sky-re Ghidra project,
 * 1.6.1170), HYPOTHESIS throughout until a trace; this hook's own log line
 * is the first.
 *
 * Here, at 38820's entry, a hosted body with its AI off (Hosted.h; not a
 * puppet, Puppets.h: a watcher's copy follows the server's word) gets its AI
 * enabled (Actor::EnableAI, include/RE/A/Actor.h:521, the kProcessMe bit of
 * Actor.h:160 that Actor::IsAIEnabled reads, Actor.h:579) and keeps it: the
 * dead state clears inside the call, and a thrall needs its AI to follow and
 * fight. The life state is read before and after through CommonLib's
 * accessor (Actor::AsActorState, Actor.h:736; ActorState::GetLifeState,
 * include/RE/A/ActorState.h:158; the enum at ActorState.h:7-18: kAlive 0,
 * kDying 1, kDead 2, kUnconcious 3, kReanimate 4, kRecycle 5). A reanimated
 * body lives at kReanimate for the effect's whole life, and Actor::IsDead
 * (Actor.h:345) reads actorState1 alone, dead at kDying, kDead and kRecycle,
 * so kReanimate reads alive. HYPOTHESIS, the lab's to confirm: the thrall's
 * AI stays on and it follows the commanding actor's figure on the host; the
 * host's sampler (skymp5-client hostedStateService) reads kReanimate as
 * alive and the server records the flip; nothing else turns the AI off
 * before the sampler's next poll; 38820 is a plain function entry Gum
 * attaches to (Attach logs OK). The entry keeps one record per call on the
 * thread, the DroppedArrow shape above.
 */
struct ReanimateSeen
{
  // null when the entry took nothing: not hosted, a puppet, or the AI
  // already on
  RE::Actor* actor = nullptr;
  uint32_t formId = 0;
  uint32_t lifeStateBefore = 0;
};
thread_local std::vector<ReanimateSeen> g_reanimations;

void OnReanimateEnter(GumInvocationContext* ic)
{
  g_reanimations.push_back({});
  // the one argument, this (rcx on Windows x64); TESForm::formID
  // (include/RE/T/TESForm.h:354)
  auto* actor = reinterpret_cast<RE::Actor*>(
    gum_invocation_context_get_nth_argument(ic, 0));
  if (!actor) {
    return;
  }
  const uint32_t formId = actor->formID;
  if (!Hosted::SimulatedHere(formId) || actor->IsAIEnabled()) {
    return;
  }
  auto& seen = g_reanimations.back();
  seen.actor = actor;
  seen.formId = formId;
  seen.lifeStateBefore =
    static_cast<uint32_t>(actor->AsActorState()->GetLifeState());
  actor->EnableAI(true);
}

void OnReanimateLeave(GumInvocationContext* ic)
{
  if (g_reanimations.empty()) {
    return;
  }
  const ReanimateSeen seen = g_reanimations.back();
  g_reanimations.pop_back();
  if (!seen.actor) {
    return;
  }
  // the actor outlives its own member call; the AI stays on (see above)
  const auto after =
    static_cast<uint32_t>(seen.actor->AsActorState()->GetLifeState());
  logger::info("[verb:npc-fate] reanimate {:x}: hosted body, AI enabled for "
               "the engine's reanimation (life state before {}, after {})",
               seen.formId, seen.lifeStateBefore, after);
}

void InstallReanimateHook()
{
  // AE only: the id is 1.6.1170's (game/Offsets.h)
  if (!REL::Module::IsAE()) {
    return;
  }
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::HOOK_REANIMATE, Offsets::Hooks::Reanimate.address(),
    std::make_shared<Frida::Hook>(OnReanimateEnter, OnReanimateLeave));
}

/**
 * The player's touch of a prop (thuum docs/verbs/prop-physics.md, ADR-058:
 * the seat whose player touched a prop it does not simulate claims the
 * simulation, PropClaim touch 0). The one place the engine sees a Keyframed
 * body's contact with the player is the player's proxy character
 * controller's hkpCharacterProxyListener slot 2,
 * bhkCharProxyController::ContactPointAddedCallback (CommonLibSSE-NG
 * include/RE/H/hkpCharacterProxyListener.h:49,
 * include/RE/B/bhkCharProxyController.h:22): hkpCharacterProxy::
 * UpdateManifold calls it for every manifold point added with no
 * motion-type filter, while the surface-interaction pass that fires slot 5
 * and applies the player's bump skips bodies of motion type 4 and 5, so on
 * the seat that does not simulate the prop (its body Keyframed, ADR-058
 * decision 3) nothing but this callback ever says the player touched it,
 * and polling the controller's bumpedBody (bhkCharacterController.h:148)
 * cannot replace it (the re-analyst's read, docs/research/m3/
 * re-havok-contact.md, 2026-10-29; HYPOTHESIS throughout until
 * lab/frida/prop-contact.js). The engine's own body reacts to layer 30
 * (other character controllers) alone; the detour reads, and the engine's
 * body runs unchanged after it.
 *
 * On entry: this (rcx) is the listener subobject, the proxy controller's
 * first base at +0 (bhkCharProxyController.h:12); the point (r8) is a
 * reference. Filtered here (rule 9), in this order since thuum ADR-071 rung
 * 4 (D-PH9 A; the build spec's C3). The body first: the other collidable
 * (hkpRootCdPoint.h:16 rootCollidableB) and its owner when its broadphase
 * type is an entity (hkpTypedBroadPhaseHandle.h:15; hkpWorldObject.h:39-47
 * kEntity; hkpCollidable.h:57-66 GetOwner by ownerOffset), its motion type at
 * hkpEntity.h:99 motion, hkpMotion.h:77 type (hkpMotion.h:26-38: 1 Dynamic,
 * 4 Keyframed, 5 Fixed). Then the layer and the motion: a layer in
 * {kClutter 4, kProps 10} (hkpCollidable.h:52-55 GetCollisionLayer,
 * collisionFilterInfo & 0x7F; include/RE/C/CollisionLayers.h:11, :17) at
 * any motion, which keeps E27's Keyframed follower copy (rung 3's filter),
 * or, while the world-body switch is on (Frida::SetWorldBodyContacts, off by
 * default), a dynamic motion type on any layer (PropBodies::IsDynamicMotion:
 * a world body's part on its own layer, kClutterLarge 29 at :36, a corpse's
 * kDeadBip 32 at :39; HYPOTHESIS that the proxy's callback fires for them,
 * the spec's F5, E58), so the ground, the walls and every Keyframed body
 * outside those two layers stop here, before any JS. HYPOTHESIS (the build
 * spec's F5, F13 and F19): that a world body's part reaches this callback by
 * its dynamic motion on its own layer, that FindCollidableRef names the
 * part's reference, and that a living biped's bodies read Keyframed, so they
 * stop here too. With the switch on, an item's body on a layer other than
 * those two passes as well wherever this seat's copy reads a dynamic type (a
 * non-owner's resting copy reads 2 or 3, thuum docs/engine/havok-props.md
 * E26), and propService.ts claimContacts claims it by touch 0, which rung 3
 * never did: a weapon's, if its body lies on kWeapon 5 (:12; unread, thuum
 * docs/verbs/prop-physics.md's drop reads owed). Then the player's own
 * controller: the listener's enclosing proxy controller, upcast to its
 * bhkCharacterController base (bhkCharProxyController.h:13; the casts apply
 * the header's layout), against Actor::GetCharController (Actor.h:531;
 * src/RE/A/Actor.cpp:324-328 walks currentProcess->middleHigh->
 * charController). Then the reference by TESHavokUtilities::FindCollidableRef
 * (TESHavokUtilities.h:15, bound by CommonLib at Offsets.h:524, Address
 * Library 26003, just addr 2026-10-29), never the player's own (0x14). The
 * client's kind of the reference is the one filter past these
 * (skymp5-client propService.ts).
 * One event per reference per 250 ms (a manifold point is added each time
 * the player's capsule meets the body anew, several a second while leaning
 * on it), kept in a map under a mutex because the callback's thread is
 * unread (the research note: inside the proxy's integrate, 62646); the hop
 * to JS is a tick task, a locked queue (viet/include/TaskQueue.h), so the
 * event reaches propContact the same way from any thread. The trace line
 * carries the thread id for the Dynamic plan's question. The JS side
 * (skymp5-client propService.ts onPropContact) claims the prop when it is
 * tracked and this seat does not simulate it.
 */
namespace {
constexpr uint64_t kPropContactEveryMs = 250;
// the world-body switch (Frida::SetWorldBodyContacts), off by default
std::atomic<bool> g_worldBodyContacts{ false };
std::mutex g_propContactMutex;
// reference form id -> NowMs() of the last event sent for it
std::unordered_map<uint32_t, uint64_t> g_propContactLastMs;

void OnPropContactEnter(GumInvocationContext* ic)
{
  auto* listener = reinterpret_cast<RE::hkpCharacterProxyListener*>(
    gum_invocation_context_get_nth_argument(ic, 0));
  const auto* point = reinterpret_cast<const RE::hkpRootCdPoint*>(
    gum_invocation_context_get_nth_argument(ic, 2));
  if (!listener || !point) {
    return;
  }
  const RE::hkpCollidable* collidable = point->rootCollidableB;
  if (!collidable) {
    return;
  }
  // the body, an entity's
  if (collidable->broadPhaseHandle.type !=
      static_cast<std::int8_t>(RE::hkpWorldObject::BroadPhaseType::kEntity)) {
    return;
  }
  auto* body = collidable->GetOwner<RE::hkpEntity>();
  if (!body) {
    return;
  }
  // the layer and the motion: the ground and the walls (Fixed) are most of
  // the points, and a Keyframed body passes on the two prop layers alone
  const auto type = body->motion.type.get();
  const auto layer = collidable->GetCollisionLayer();
  const bool propLayer =
    layer == RE::COL_LAYER::kClutter || layer == RE::COL_LAYER::kProps;
  if (!propLayer &&
      !(g_worldBodyContacts.load(std::memory_order_relaxed) &&
        PropBodies::IsDynamicMotion(type))) {
    return;
  }
  // the player's controller alone
  auto* player = RE::PlayerCharacter::GetSingleton();
  auto* playerController = player ? player->GetCharController() : nullptr;
  if (!playerController) {
    return;
  }
  auto* proxyController = static_cast<RE::bhkCharProxyController*>(listener);
  if (static_cast<RE::bhkCharacterController*>(proxyController) !=
      playerController) {
    return;
  }
  const uint32_t motionType = body->motion.type.underlying();
  auto* refr = RE::TESHavokUtilities::FindCollidableRef(*collidable);
  if (!refr) {
    return;
  }
  const uint32_t refrId = refr->formID;
  if (refrId == 0x14) {
    return;
  }
  const uint64_t now = NowMs();
  {
    std::lock_guard<std::mutex> lock(g_propContactMutex);
    auto it = g_propContactLastMs.find(refrId);
    if (it != g_propContactLastMs.end() &&
        now - it->second < kPropContactEveryMs) {
      return;
    }
    if (g_propContactLastMs.size() > 1024) {
      g_propContactLastMs.clear();
    }
    g_propContactLastMs[refrId] = now;
  }
  const uint32_t layerNumber = static_cast<uint32_t>(layer);
  logger::trace(
    "[verb:prop-physics] contact {:x} layer {} motion {} (thread {})", refrId,
    layerNumber, motionType, GetCurrentThreadId());
  SkyrimPlatform::GetSingleton()->AddTickTask(
    [refrId, layerNumber, motionType](Napi::Env env) {
      auto obj = Napi::Object::New(env);
      AddObjProperty(&obj, "refrId", refrId);
      AddObjProperty(&obj, "layer", layerNumber);
      AddObjProperty(&obj, "motionType", motionType);
      EventsApi::SendEvent("propContact", { obj });
    });
}
}

void InstallPropContactHook()
{
  // AE only: the id is 1.6.1170's (game/Offsets.h)
  if (!REL::Module::IsAE()) {
    return;
  }
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::HOOK_PROP_CONTACT,
    Offsets::Hooks::CharProxyContactPointAdded.address(),
    std::make_shared<Frida::Hook>(OnPropContactEnter, nullptr));
  logger::info("[verb:prop-physics] contact hook installed: the proxy "
               "controller's ContactPointAddedCallback (Address Library "
               "79131), one propContact per reference per {} ms",
               kPropContactEveryMs);
}

void Frida::SetWorldBodyContacts(bool on)
{
  g_worldBodyContacts.store(on, std::memory_order_relaxed);
  logger::info("[verb:prop-physics] world-body contacts {}",
               on ? "on" : "off");
}

/**
 * An impulse's cause (thuum docs/DECISIONS.md ADR-071 decision 4, "caster
 * only"; lab/.cache/drafts/physics/rung3-plan.md B3 and its 2026-11-12
 * addendum, from the reads in read-rung3-premises-080520-080858.md; the
 * trace lab/frida/prop-impulse.js v1.2 on thuum main is the map of the sites
 * and brackets rebuilt here). Every seat's engine runs every cast and shout
 * it sees, its own and its copies of another seat's, so the watcher's copy of
 * a remote cast pushes the watcher's own bodies with the caster's numbers
 * (H3: CONFIRMED by 20261111-223256, and by 20261112-080520 on c2, the
 * Riverwood Trader sign's board and a static's two bodies under the figure's
 * cones, a dead chicken's ragdoll through 26005). The engine that owns a body
 * simulates it and the others render it (ADR-063; Fiedler, "Networked Physics
 * in Virtual Reality", GDC 2018; Delaney, Ward and McLoone, Presence 15(2) and
 * 15(4), 2006): an impulse whose cause another seat owns is zeroed here, and
 * one whose cause this seat owns applies and names the references it pushed,
 * for the client's claim.
 *
 * The two sites, replaced through Gum (the DamageActor shape), each with
 * CommonLib's own signature (the decltype of its declaration, checked by a
 * static_assert at its replacement):
 * - ScaleGameplayImpulseForce (Address Library 26004,
 *   T/TESHavokUtilities.h:18), the force a gameplay impulse gives one body.
 *   Every prop push on both seats went through it (223256's H2). The
 *   explosions' and the cone's bhkRigidBody::SetLinearImpulse (78091,
 *   B/bhkRigidBody.h:54) applied its result over the world scale (223256's 58
 *   pairs, 080520's 14), and the arrow's hkpSphereMotion::ApplyPointImpulse
 *   (slot 0x14, H/hkpMotion.h:60) took it whole, its length the result to four
 *   places (x-prop-arrow 20261112-080858, c1:L22-L23). Zeroed: 0.0f, the
 *   original not called (HYPOTHESIS: it computes and writes nothing).
 * - AddExplosionImpulse (26005, TESHavokUtilities.h:14), an explosion's push
 *   of a node tree and a ragdoll's one push (080520: no 26004 inside any 26005
 *   on either seat). It recurses through the tree, and a replacement sees its
 *   outermost call alone (223256's replace saw 3 outer calls where 080520's
 *   attach saw 110; HYPOTHESIS, Gum routing a call from inside the original to
 *   the original). Zeroed: returned without calling the original, which skips
 *   the whole tree (HYPOTHESIS until a zero reads, and that its caller needs
 *   nothing else done inside it).
 *
 * The cause is never an argument (HitData's aggressor never appeared,
 * 223256): it is the innermost bracket open on the impulse's own thread, a
 * frame three engine functions push at their enter and pop at their leave
 * (HookHandler's attach, a HookID each), on a thread_local stack (the idiom
 * of g_relayedArrowDepth, above):
 * - Explosion::Update, slot 0xA3 (E/Explosion.h:53) of VTABLE_Explosion[0]
 *   (include/RE/Offsets_VTABLE.h:2779, Address Library 209940): its cause the
 *   explosion's actorOwner (Explosion.h:72, through GetExplosionRuntimeData's
 *   1.6.629 shift, :89-97), the caster on its own seat and the caster's figure
 *   on the watcher's (223256);
 * - Projectile::ProcessImpacts, slot 0xAC (P/Projectile.h:208) of each
 *   projectile class's vtable (Offsets_VTABLE.h:2771-2788: Arrow 209891,
 *   Barrier 209901, Beam 209914, Cone 209925, Flame 209953, Grenade 209962,
 *   Missile 209980 and Projectile itself 210006), one attach per distinct
 *   address (five in 1.6.1170's exe, lab/.cache/game/exe-1.6.1170 read
 *   2026-11-12: a class that does not override shares its parent's): its
 *   cause the projectile's shooter (Projectile.h:254, through
 *   GetProjectileRuntimeData's shift, :291-299). The arrow's 26004 ran at
 *   depth 3, inside the Arrow, Missile and Projectile chain on one object
 *   (080858);
 * - ConeProjectile::HandleHits, slot 0xBE (P/Projectile.h:226,
 *   C/ConeProjectile.h:43) of VTABLE_ConeProjectile[0] (Offsets_VTABLE.h:2777,
 *   209925): all 14 cone pushes of 080520 (8 on c1, 6 on c2) ran inside it
 *   under UpdateImpl, and its shooter named the cause, 0x14 on c1 and c1's
 *   figure on c2 (CONFIRMED by 20261112-080520 on both seats). UpdateImpl
 *   encloses it alone and the cone's own ProcessImpacts pushed nothing, so the
 *   cone has no other bracket of its own.
 * Each slot is read from its vtable at install, through CommonLib's
 * Relocation as write_vfunc reads the one it replaces (REL/Relocation.h:
 * 392-399), and its install line says whether it lies in SkyrimSE.exe's code
 * (Core Impact Framework hooks slot 0xBE of ArrowProjectile and
 * MissileProjectile, never ConeProjectile's: c1-CoreImpactFramework.log:22-23
 * in both reads).
 *
 * The rule (ADR-071 W5), in order, all reads and no engine write of its own:
 * 1. The switch off (the default): the original, nothing counted.
 * 2. No bracket open on this thread: the original, counted as outside any
 *    bracket (no 26004 ran unbracketed in either read; the caster's own 26005
 *    calls on its NPC copies ran outside every bracket, 080520's 110 on c1,
 * and pass, as caster only wants).
 * 3. The cause, read once a frame at its first impulse (the handle resolved by
 *    BSPointerHandle::get, B/BSPointerHandle.h:155, on the impulse's worker
 *    thread, as prop-impulse.js resolved it with no fault, and released at
 *    once), judged by OwnerOf (above). An explosion no actor owns (a trap's,
 *    the world's) or a projectile whose shooter is no actor (none, a trap's
 *    dart, a caster activator) is the world's, its cell host's: this seat's
 *    when this seat hosts the bracket object's cell, else undecided.
 * 4. Another seat's: zeroed, counted. This seat's: the original, the body's
 *    reference joining the frame's impact set (26004's by its rigid body's
 *    collidable, hkpWorldObject::GetUserData, H/hkpWorldObject.h:62, which is
 *    TESHavokUtilities::FindCollidableRef, E26's read safe inside Havok's
 *    callbacks; 26005's by its node's owner, NiAVObject::GetUserData,
 *    N/NiAVObject.h:125, src/RE/N/NiAVObject.cpp:102-114). Undecided (a live
 *    NPC no seat's word reached this seat for, in a cell this seat does not
 *    host; the world's in such a cell): the original, counted, no claim.
 * Every physics body takes the rule (Eli's D-PH5, "Every physics body",
 * 2026-11-12): items, statics and ragdolls alike, so a remote cast moves the
 * watcher's sign and ragdolls no more until rungs 4 and 5 stream them. The
 * player's own reference and a body with no reference join no impact set.
 *
 * At a frame's leave (rule 9: counted and said in native code; nothing
 * crosses for a zero): one line under the verb's tag, "impulse zeroed: N
 * bodies, cause C (kind S), another seat's", "impulse applied: N bodies,
 * cause C (kind S), this seat's: claim raised" (or "no reference to claim")
 * or "impulse undecided: ...", each (outcome, bracket object)'s first 16 and
 * every hundredth after; and for this seat's cause with a reference pushed,
 * propImpulse on the next tick (the propContact idiom): { impact, kind,
 * cause, refrs }, refrs at most kImpulseRefsPerEvent, a larger set as several
 * events with one impact number. Calls outside every bracket print once a
 * second while they come, and every count by kind and outcome every ten
 * seconds while one moves.
 *
 * Cost (rule 9; the plan's R7, read against an untraced run): 26004 and 26005
 * run at impacts alone (no call in the minutes between 223256's legs), Gum's
 * replace adding its thread-local bookkeeping and a stack push to each call;
 * the brackets run at every live projectile's ProcessImpacts and every
 * explosion's Update, a thread_local push and pop each, no lock and no lookup.
 *
 * HYPOTHESIS until the rung 3 build's reads (x-prop-impact v1; the reads'
 * section 7): that a zero at 26004 zeroes the push (the cone's 78091 vectors;
 * the arrow's ApplyPointImpulse, or the helper's early return on a zero
 * result, unread: the reads' section 3); that HandleHits' angular impulse
 * (78092, its call inside HandleHits never attached in 080520: section 7
 * item 2) leaves no spin; that an explosion a projectile's impact spawns
 * updates in its own frame, never nested inside the projectile's (the plan's
 * R4); how a zeroed 26005 looks on a live NPC the watcher hosts (item 4);
 * that the zero vector a zeroed 26004 still hands 78091 activates no sleeping
 * island (unread: a far host's client would track and read such bodies awake,
 * propService.ts wakeFar); and that the skipped 26005 original writes nothing
 * through a_pos (its hkVector4&, non-const, TESHavokUtilities.h:14) that its
 * caller reads
 */
namespace {
// the two sites as CommonLib declares them (TESHavokUtilities.h:14, :18)
using ScaleImpulseFn =
  decltype(&RE::TESHavokUtilities::ScaleGameplayImpulseForce);
using ExplosionImpulseFn =
  decltype(&RE::TESHavokUtilities::AddExplosionImpulse);
ScaleImpulseFn g_scaleImpulseOriginal = nullptr;
ExplosionImpulseFn g_explosionImpulseOriginal = nullptr;
// whether each replacement went in at install
std::atomic<bool> g_scaleImpulseReplaced{ false };
std::atomic<bool> g_explosionImpulseReplaced{ false };

// the rule's switch (Frida::SetImpulseCauseRule), off by default
std::atomic<bool> g_impulseCauseRule{ false };

// a bracket's kind, kNone the calls outside every bracket
enum class ImpulseBracket : uint32_t
{
  kNone = 0,
  kExplosion = 1,
  kImpacts = 2,
  kCone = 3
};
constexpr uint32_t kImpulseBracketKinds = 4;

// the kind's name in propImpulse and the lines (prop-impulse.js's names)
const char* ImpulseBracketText(ImpulseBracket kind)
{
  switch (kind) {
    case ImpulseBracket::kExplosion:
      return "explosion";
    case ImpulseBracket::kImpacts:
      return "impacts";
    case ImpulseBracket::kCone:
      return "cone";
    default:
      return "none";
  }
}

// an impulse call's outcome, by its cause's owner
enum class ImpulseOutcome : uint32_t
{
  kApplied = 0,
  kZeroed = 1,
  kUndecided = 2
};
constexpr uint32_t kImpulseOutcomes = 3;

ImpulseOutcome OutcomeOf(CauseOwner owner)
{
  switch (owner) {
    case CauseOwner::kOther:
      return ImpulseOutcome::kZeroed;
    case CauseOwner::kUndecided:
      return ImpulseOutcome::kUndecided;
    default:
      return ImpulseOutcome::kApplied;
  }
}

// the counted impulse calls since launch, by bracket kind and outcome (rule
// 9: counted in native code; the reporter below says them)
std::atomic<uint64_t> g_impulseCalls[kImpulseBracketKinds][kImpulseOutcomes];
// the impacts raised since launch, each propImpulse's number
std::atomic<uint32_t> g_impulseImpacts{ 0 };

void CountImpulseCall(ImpulseBracket kind, ImpulseOutcome outcome)
{
  g_impulseCalls[static_cast<uint32_t>(kind)][static_cast<uint32_t>(outcome)]
    .fetch_add(1, std::memory_order_relaxed);
}

// one bracket open on a thread; trivially constructible, so the
// thread_local array below needs no dynamic initialization, and every field
// is set at the enter
struct ImpulseFrame
{
  ImpulseBracket kind;
  // the hooked function's this: an Explosion for kExplosion, a Projectile for
  // the other two (each a TESObjectREFR at offset 0, the primary vtable's)
  void* site;
  // the cause, read and judged at the frame's first counted impulse, with
  // the site's form id read then (inside the site's own call)
  bool judged;
  CauseOwner owner;
  uint32_t causeId;
  uint32_t siteId;
  // the counted calls inside the frame, 26004's and 26005's
  uint32_t sized;
  uint32_t trees;
  // the frame's references in g_impulseRefs, from refStart, once it has one
  bool hasRefs;
  size_t refStart;
};

// at most this many brackets nest on a thread (the arrow's chain is three,
// 080858); a deeper enter is counted so the pops stay paired, and an impulse
// past it goes to the deepest frame kept
constexpr uint32_t kImpulseDepth = 8;
thread_local ImpulseFrame g_impulseFrames[kImpulseDepth];
thread_local uint32_t g_impulseDepth = 0;

// a reference with a Havok body, as propImpulse and readBodies' island name
// one: its form id and its base's form type (RE::FormType, F/FormTypes.h:138,
// whose numbers Skyrim Platform's FormType keeps)
struct BodyRef
{
  uint32_t refrId;
  uint32_t formType;
};
// this thread's impact sets, each open frame's from its refStart in the
// frames' order, cut back at each frame's leave (its capacity alone outlives
// an impact)
thread_local std::vector<BodyRef> g_impulseRefs;

// at most this many references a propImpulse (PropClaim v2's, the rung 2
// plan's R2-2)
constexpr size_t kImpulseRefsPerEvent = 16;

void ImpulseBracketEnter(ImpulseBracket kind, GumInvocationContext* ic)
{
  const uint32_t depth = g_impulseDepth++;
  if (depth >= kImpulseDepth) {
    return;
  }
  ImpulseFrame& frame = g_impulseFrames[depth];
  frame.kind = kind;
  frame.site = gum_invocation_context_get_nth_argument(ic, 0);
  frame.judged = false;
  frame.owner = CauseOwner::kUndecided;
  frame.causeId = 0;
  frame.siteId = 0;
  frame.sized = 0;
  frame.trees = 0;
  frame.hasRefs = false;
  frame.refStart = 0;
}

void OnImpulseExplosionEnter(GumInvocationContext* ic)
{
  ImpulseBracketEnter(ImpulseBracket::kExplosion, ic);
}

void OnImpulseImpactsEnter(GumInvocationContext* ic)
{
  ImpulseBracketEnter(ImpulseBracket::kImpacts, ic);
}

void OnImpulseConeEnter(GumInvocationContext* ic)
{
  ImpulseBracketEnter(ImpulseBracket::kCone, ic);
}

// the innermost frame open on this thread, or null for none
ImpulseFrame* InnermostImpulseFrame()
{
  const uint32_t depth = g_impulseDepth;
  if (depth == 0) {
    return nullptr;
  }
  return &g_impulseFrames[std::min(depth, kImpulseDepth) - 1];
}

// the world's cause (no actor named): the bracket object's cell's host
CauseOwner WorldCauseOwner(const RE::TESObjectREFR* site)
{
  const RE::TESObjectCELL* cell = site ? site->GetParentCell() : nullptr;
  return cell && Frida::IsCellHosted(cell->GetFormID())
    ? CauseOwner::kThisSeat
    : CauseOwner::kUndecided;
}

// the frame's cause, once: the explosion's owner or the projectile's
// shooter, each resolved and released here, judged by OwnerOf
void JudgeImpulseCause(ImpulseFrame& frame)
{
  if (frame.judged) {
    return;
  }
  frame.judged = true;
  frame.causeId = 0;
  if (!frame.site) {
    frame.owner = CauseOwner::kUndecided;
    return;
  }
  if (frame.kind == ImpulseBracket::kExplosion) {
    auto* explosion = static_cast<RE::Explosion*>(frame.site);
    frame.siteId = explosion->formID;
    const RE::NiPointer<RE::Actor> owner =
      explosion->GetExplosionRuntimeData().actorOwner.get();
    if (owner) {
      frame.causeId = owner->formID;
      frame.owner = OwnerOf(owner.get());
    } else {
      frame.owner = WorldCauseOwner(explosion);
    }
    return;
  }
  auto* projectile = static_cast<RE::Projectile*>(frame.site);
  frame.siteId = projectile->formID;
  const RE::NiPointer<RE::TESObjectREFR> shooter =
    projectile->GetProjectileRuntimeData().shooter.get();
  if (shooter && shooter->GetFormType() == RE::FormType::ActorCharacter) {
    frame.causeId = shooter->formID;
    frame.owner = OwnerOf(shooter.get());
  } else {
    // no actor named (none, a trap's dart, a caster activator): the world's,
    // its cell host's, as an explosion no actor owns
    frame.owner = WorldCauseOwner(projectile);
  }
}

// a counted call inside the frame: the cause judged first, then the call
// counted by the frame's kind and its outcome
void CountFrameImpulse(ImpulseFrame& frame, bool tree)
{
  JudgeImpulseCause(frame);
  if (tree) {
    ++frame.trees;
  } else {
    ++frame.sized;
  }
  CountImpulseCall(frame.kind, OutcomeOf(frame.owner));
}

// a body this seat's cause pushed: its reference joins the frame's impact
// set once, with its base's form type; the player's own and none skipped
void NoteImpulseRef(ImpulseFrame& frame, const RE::TESObjectREFR* refr)
{
  if (!refr) {
    return;
  }
  const uint32_t refrId = refr->formID;
  if (refrId == 0 || refrId == 0x14) {
    return;
  }
  if (!frame.hasRefs) {
    frame.hasRefs = true;
    frame.refStart = g_impulseRefs.size();
  }
  const auto from =
    g_impulseRefs.begin() + static_cast<std::ptrdiff_t>(frame.refStart);
  if (std::any_of(from, g_impulseRefs.end(), [refrId](const BodyRef& ref) {
        return ref.refrId == refrId;
      })) {
    return;
  }
  const RE::TESBoundObject* base = refr->GetBaseObject();
  g_impulseRefs.push_back(
    { refrId, base ? static_cast<uint32_t>(base->GetFormType()) : 0u });
}

// A living NPC is thrown by the engine that simulates it, and every other
// screen follows that host's stream; props and world objects keep the rule.
// So two exceptions to it, by the pushed body's reference, read where the
// call names it (26004's rigid body, 26005's node): another seat's push on a
// living actor this seat simulates (Hosted.h SimulatedHere) is applied, its
// throw this seat's engine's; this seat's own push on a living puppet copy
// (Puppets.h, another seat's NPC) is zeroed, the copy following its host's
// stream. Each is counted, its line at most once a second
const RE::Actor* LivingActorOf(const RE::TESObjectREFR* refr)
{
  if (!refr || refr->GetFormType() != RE::FormType::ActorCharacter) {
    return nullptr;
  }
  const auto* actor = static_cast<const RE::Actor*>(refr);
  return actor->IsDead() ? nullptr : actor;
}

std::atomic<uint64_t> g_hostedPushesTaken{ 0 };
std::atomic<uint64_t> g_puppetPushesZeroed{ 0 };
std::atomic<int64_t> g_pushExceptionLineMs{ 0 };

void SayPushException(bool taken, uint32_t refrId, uint32_t causeId)
{
  const auto n = taken ? ++g_hostedPushesTaken : ++g_puppetPushesZeroed;
  const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (nowMs - g_pushExceptionLineMs.exchange(nowMs) < 1000) {
    return;
  }
  if (taken) {
    logger::info("[verb:prop-physics] impulse applied on {:x}, a living NPC "
                 "this seat hosts, though cause {:x} is another seat's: its "
                 "throw is its host's (D-PH5's amendment, tracker #175; {} so "
                 "far)",
                 refrId, causeId, n);
  } else {
    logger::info("[verb:prop-physics] impulse zeroed on {:x}, a puppet copy "
                 "of another seat's NPC, though cause {:x} is this seat's: "
                 "its host throws it, this copy follows (tracker #175; {} so "
                 "far)",
                 refrId, causeId, n);
  }
}

float ScaleImpulseReplacement(float a_inputForce, RE::bhkRigidBody* a_body,
                              bool a_factorMass)
{
  if (!g_impulseCauseRule.load(std::memory_order_relaxed)) {
    return g_scaleImpulseOriginal(a_inputForce, a_body, a_factorMass);
  }
  ImpulseFrame* frame = InnermostImpulseFrame();
  if (!frame) {
    CountImpulseCall(ImpulseBracket::kNone, ImpulseOutcome::kApplied);
    return g_scaleImpulseOriginal(a_inputForce, a_body, a_factorMass);
  }
  CountFrameImpulse(*frame, false);
  // the rigid body bhkRefObject refers to (B/bhkRefObject.h:26), an
  // hkpEntity whose collidable names its reference
  const RE::TESObjectREFR* bodyRef = nullptr;
  if (a_body) {
    const auto* havok =
      static_cast<const RE::hkpRigidBody*>(a_body->referencedObject.get());
    bodyRef = havok ? havok->GetUserData() : nullptr;
  }
  const RE::Actor* living = LivingActorOf(bodyRef);
  if (frame->owner == CauseOwner::kOther) {
    if (!living || !Hosted::SimulatedHere(living->formID)) {
      return 0.f;
    }
    SayPushException(true, living->formID, frame->causeId);
    return g_scaleImpulseOriginal(a_inputForce, a_body, a_factorMass);
  }
  if (frame->owner == CauseOwner::kThisSeat && living &&
      Puppets::Is(living->formID)) {
    SayPushException(false, living->formID, frame->causeId);
    return 0.f;
  }
  const float force =
    g_scaleImpulseOriginal(a_inputForce, a_body, a_factorMass);
  if (frame->owner == CauseOwner::kThisSeat) {
    NoteImpulseRef(*frame, bodyRef);
  }
  return force;
}
static_assert(
  std::is_same_v<decltype(&ScaleImpulseReplacement), ScaleImpulseFn>,
  "26004's replacement keeps CommonLib's signature");

void ExplosionImpulseReplacement(RE::NiAVObject* a_obj3D, RE::hkVector4& a_pos,
                                 float a_force, const RE::HitData* a_hitData)
{
  if (!g_impulseCauseRule.load(std::memory_order_relaxed)) {
    g_explosionImpulseOriginal(a_obj3D, a_pos, a_force, a_hitData);
    return;
  }
  ImpulseFrame* frame = InnermostImpulseFrame();
  if (!frame) {
    CountImpulseCall(ImpulseBracket::kNone, ImpulseOutcome::kApplied);
    g_explosionImpulseOriginal(a_obj3D, a_pos, a_force, a_hitData);
    return;
  }
  CountFrameImpulse(*frame, true);
  const RE::TESObjectREFR* bodyRef =
    a_obj3D ? a_obj3D->GetUserData() : nullptr;
  const RE::Actor* living = LivingActorOf(bodyRef);
  if (frame->owner == CauseOwner::kOther) {
    if (!living || !Hosted::SimulatedHere(living->formID)) {
      return;
    }
    SayPushException(true, living->formID, frame->causeId);
    g_explosionImpulseOriginal(a_obj3D, a_pos, a_force, a_hitData);
    return;
  }
  if (frame->owner == CauseOwner::kThisSeat && living &&
      Puppets::Is(living->formID)) {
    SayPushException(false, living->formID, frame->causeId);
    return;
  }
  g_explosionImpulseOriginal(a_obj3D, a_pos, a_force, a_hitData);
  if (frame->owner == CauseOwner::kThisSeat) {
    NoteImpulseRef(*frame, bodyRef);
  }
}
static_assert(
  std::is_same_v<decltype(&ExplosionImpulseReplacement), ExplosionImpulseFn>,
  "26005's replacement keeps CommonLib's signature");

// the frame's lines are due per (outcome, bracket object): each one's first
// 16 and every hundredth after (a flame's or a beam's stream on a pile would
// raise one a frame, unread), TriggerLineDue's shape; the lock is taken at a
// leave that counted an impulse alone
std::mutex g_impulseLinesMutex;
std::unordered_map<uint64_t, uint64_t> g_impulseLines;

bool ImpulseLineDue(ImpulseOutcome outcome, uint32_t siteId)
{
  const uint64_t key =
    (static_cast<uint64_t>(static_cast<uint32_t>(outcome)) << 32) | siteId;
  std::lock_guard<std::mutex> lock(g_impulseLinesMutex);
  if (g_impulseLines.size() > 1024 && g_impulseLines.count(key) == 0) {
    g_impulseLines.clear();
  }
  const uint64_t n = ++g_impulseLines[key];
  return n <= 16 || n % 100 == 0;
}

// propImpulse on the next tick, kImpulseRefsPerEvent references an event,
// one impact number for every part
void RaiseImpulseEvents(uint32_t impact, const char* kind, uint32_t causeId,
                        const std::vector<BodyRef>& refs)
{
  for (size_t at = 0; at < refs.size(); at += kImpulseRefsPerEvent) {
    const size_t end = std::min(refs.size(), at + kImpulseRefsPerEvent);
    std::vector<BodyRef> part(refs.begin() + static_cast<std::ptrdiff_t>(at),
                              refs.begin() + static_cast<std::ptrdiff_t>(end));
    SkyrimPlatform::GetSingleton()->AddTickTask(
      [impact, kind, causeId, part](Napi::Env env) {
        auto obj = Napi::Object::New(env);
        AddObjProperty(&obj, "impact", impact);
        AddObjProperty(&obj, "kind", kind);
        AddObjProperty(&obj, "cause", causeId);
        auto refrs = Napi::Array::New(env, part.size());
        for (uint32_t i = 0; i < part.size(); ++i) {
          auto ref = Napi::Object::New(env);
          AddObjProperty(&ref, "refrId", part[i].refrId);
          AddObjProperty(&ref, "formType", part[i].formType);
          refrs.Set(i, ref);
        }
        obj.Set("refrs", refrs);
        EventsApi::SendEvent("propImpulse", { obj });
      });
  }
}

// a frame's leave with a counted impulse: its impact set taken off the
// thread's, the event raised for this seat's cause, and its line
void FinishImpulseFrame(const ImpulseFrame& frame)
{
  std::vector<BodyRef> refs;
  if (frame.hasRefs && frame.refStart <= g_impulseRefs.size()) {
    const auto from =
      g_impulseRefs.begin() + static_cast<std::ptrdiff_t>(frame.refStart);
    refs.assign(from, g_impulseRefs.end());
    g_impulseRefs.erase(from, g_impulseRefs.end());
  }
  const ImpulseOutcome outcome = OutcomeOf(frame.owner);
  const char* kind = ImpulseBracketText(frame.kind);
  uint32_t impact = 0;
  if (outcome == ImpulseOutcome::kApplied && !refs.empty()) {
    impact = g_impulseImpacts.fetch_add(1, std::memory_order_relaxed) + 1;
    RaiseImpulseEvents(impact, kind, frame.causeId, refs);
  }
  if (!ImpulseLineDue(outcome, frame.siteId)) {
    return;
  }
  const uint32_t bodies = frame.sized + frame.trees;
  const auto thread = GetCurrentThreadId();
  if (outcome == ImpulseOutcome::kZeroed) {
    logger::info("[verb:prop-physics] impulse zeroed: {} bodies, cause {:x} "
                 "({} {:x}), another seat's (26004 {}, 26005 {}; thread {})",
                 bodies, frame.causeId, kind, frame.siteId, frame.sized,
                 frame.trees, thread);
    return;
  }
  if (outcome == ImpulseOutcome::kUndecided) {
    logger::info("[verb:prop-physics] impulse undecided: {} bodies, cause "
                 "{:x} ({} {:x}), no seat's word on it here: applied, no "
                 "claim (26004 {}, 26005 {}; thread {})",
                 bodies, frame.causeId, kind, frame.siteId, frame.sized,
                 frame.trees, thread);
    return;
  }
  if (refs.empty()) {
    logger::info("[verb:prop-physics] impulse applied: {} bodies, cause {:x} "
                 "({} {:x}), this seat's: no reference to claim (26004 {}, "
                 "26005 {}; thread {})",
                 bodies, frame.causeId, kind, frame.siteId, frame.sized,
                 frame.trees, thread);
    return;
  }
  std::string list;
  for (const auto& ref : refs) {
    list += fmt::format("{}{:x}", list.empty() ? "" : " ", ref.refrId);
  }
  logger::info("[verb:prop-physics] impulse applied: {} bodies, cause {:x} "
               "({} {:x}), this seat's: claim raised, impact {}, {} "
               "references {} (26004 {}, 26005 {}; thread {})",
               bodies, frame.causeId, kind, frame.siteId, impact, refs.size(),
               list, frame.sized, frame.trees, thread);
}

void OnImpulseBracketLeave(GumInvocationContext* ic)
{
  if (g_impulseDepth == 0) {
    return;
  }
  const uint32_t depth = --g_impulseDepth;
  if (depth >= kImpulseDepth) {
    return;
  }
  const ImpulseFrame& frame = g_impulseFrames[depth];
  if (frame.sized == 0 && frame.trees == 0) {
    return;
  }
  FinishImpulseFrame(frame);
}

// the calls outside every bracket once a second while they come, and every
// count by kind and outcome every ten seconds while one moves (thuum
// ADR-069's note: a count shows that the rule fired); HasOverlays' reporter
// shape, a detached thread
void StartImpulseReporter()
{
  std::thread([] {
    uint64_t outsideSaid = 0;
    uint64_t totalSaid = 0;
    for (uint64_t second = 1;; ++second) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
      const uint64_t outside =
        g_impulseCalls[0][0].load(std::memory_order_relaxed);
      if (outside != outsideSaid) {
        logger::info("[verb:prop-physics] impulse outside any bracket: {} "
                     "calls, applied ({} so far)",
                     outside - outsideSaid, outside);
        outsideSaid = outside;
      }
      if (second % 10 != 0) {
        continue;
      }
      uint64_t n[kImpulseBracketKinds][kImpulseOutcomes];
      uint64_t total = 0;
      for (uint32_t k = 0; k < kImpulseBracketKinds; ++k) {
        for (uint32_t o = 0; o < kImpulseOutcomes; ++o) {
          n[k][o] = g_impulseCalls[k][o].load(std::memory_order_relaxed);
          total += n[k][o];
        }
      }
      if (total == totalSaid) {
        continue;
      }
      totalSaid = total;
      logger::info(
        "[verb:prop-physics] impulse calls so far (applied, zeroed, "
        "undecided): explosion {}, {}, {}; impacts {}, {}, {}; cone {}, {}, "
        "{}; outside any bracket {} applied; {} impacts raised",
        n[1][0], n[1][1], n[1][2], n[2][0], n[2][1], n[2][2], n[3][0], n[3][1],
        n[3][2], n[0][0], g_impulseImpacts.load(std::memory_order_relaxed));
    }
  }).detach();
}

// a vtable's slot, read as REL::Relocation::write_vfunc reads the one it
// replaces (REL/Relocation.h:392-399), the vtable through Relocation's
// VariantID constructor (:258-260); 0 for a vtable at 0
std::uintptr_t VtableSlot(const REL::VariantID& vtable, std::size_t slot)
{
  const REL::Relocation<std::uintptr_t> table{ vtable };
  if (table.address() == 0) {
    return 0;
  }
  return *reinterpret_cast<const std::uintptr_t*>(table.address() +
                                                  sizeof(void*) * slot);
}

// where a bracket's slot points, for its install line: in SkyrimSE.exe's
// code (REL::Module's textx segment, REL/Module.h:209) or outside it, where
// another plugin's thunk would sit
std::string SlotPlace(std::uintptr_t address)
{
  const auto& exe = REL::Module::get();
  const auto text = exe.segment(REL::Segment::textx);
  if (address >= text.address() && address < text.address() + text.size()) {
    return fmt::format("+{:x}, in SkyrimSE.exe's code", address - exe.base());
  }
  return fmt::format("{:x}, outside SkyrimSE.exe's code (another plugin's "
                     "thunk)",
                     address);
}
}

bool Frida::SetImpulseCauseRule(bool on)
{
  g_impulseCauseRule.store(on, std::memory_order_relaxed);
  const bool sized = g_scaleImpulseReplaced.load(std::memory_order_relaxed);
  const bool trees =
    g_explosionImpulseReplaced.load(std::memory_order_relaxed);
  logger::info("[verb:prop-physics] setImpulseCauseRule({}): {} "
               "(ScaleGameplayImpulseForce {}, AddExplosionImpulse {})",
               on,
               on ? "another seat's cause is zeroed on every physics body, "
                    "this seat's raises propImpulse"
                  : "every impulse applies as the engine's own, nothing "
                    "counted",
               sized ? "replaced" : "not replaced",
               trees ? "replaced" : "not replaced");
  return sized;
}

void InstallImpulseHooks()
{
  // AE only: the reads that place every site and bracket are 1.6.1170's
  // (game/Offsets.h)
  if (!REL::Module::IsAE()) {
    return;
  }
  auto* handler = Frida::HookHandler::GetSingleton();
  const std::uintptr_t base = REL::Module::get().base();
  const std::uintptr_t sizedAt =
    Offsets::Hooks::ScaleGameplayImpulseForce.address();
  const std::uintptr_t treesAt = Offsets::Hooks::AddExplosionImpulse.address();
  const bool sized = sizedAt &&
    handler->Replace(sizedAt,
                     reinterpret_cast<void*>(&ScaleImpulseReplacement),
                     reinterpret_cast<void**>(&g_scaleImpulseOriginal));
  const bool trees = treesAt &&
    handler->Replace(treesAt,
                     reinterpret_cast<void*>(&ExplosionImpulseReplacement),
                     reinterpret_cast<void**>(&g_explosionImpulseOriginal));
  g_scaleImpulseReplaced.store(sized, std::memory_order_relaxed);
  g_explosionImpulseReplaced.store(trees, std::memory_order_relaxed);
  if (!sized && !trees) {
    logger::error("[verb:prop-physics] impulse hooks not installed: Gum "
                  "refused ScaleGameplayImpulseForce (26004) and "
                  "AddExplosionImpulse (26005), so every impulse stays the "
                  "engine's own on every seat");
    return;
  }

  // the brackets, a HookID each (FridaHookHandler.h), one shape for the
  // three: the hook registered at plugin load, before any hook reads the map
  // (HookHandler::Register's note), then attached at each slot read now, so
  // every attach answers (Install's does not) and ProcessImpacts' one id
  // takes every distinct address
  handler->Register(Frida::HookID::HOOK_IMPULSE_EXPLOSION,
                    std::make_shared<Frida::Hook>(OnImpulseExplosionEnter,
                                                  OnImpulseBracketLeave));
  handler->Register(
    Frida::HookID::HOOK_IMPULSE_CONE,
    std::make_shared<Frida::Hook>(OnImpulseConeEnter, OnImpulseBracketLeave));
  handler->Register(Frida::HookID::HOOK_IMPULSE_IMPACTS,
                    std::make_shared<Frida::Hook>(OnImpulseImpactsEnter,
                                                  OnImpulseBracketLeave));
  const std::uintptr_t updateAt = VtableSlot(RE::VTABLE_Explosion[0], 0xA3);
  const bool update = updateAt &&
    handler->Attach(Frida::HookID::HOOK_IMPULSE_EXPLOSION, updateAt);
  if (updateAt) {
    logger::info("[verb:prop-physics] impulse bracket Explosion::Update "
                 "(VTABLE_Explosion[0] slot 0xa3): {}, {}",
                 SlotPlace(updateAt), update ? "attached" : "NOT attached");
  }
  const std::uintptr_t hitsAt = VtableSlot(RE::VTABLE_ConeProjectile[0], 0xBE);
  const bool hits =
    hitsAt && handler->Attach(Frida::HookID::HOOK_IMPULSE_CONE, hitsAt);
  if (hitsAt) {
    logger::info("[verb:prop-physics] impulse bracket "
                 "ConeProjectile::HandleHits (VTABLE_ConeProjectile[0] slot "
                 "0xbe): {}, {}",
                 SlotPlace(hitsAt), hits ? "attached" : "NOT attached");
  }
  const std::pair<const char*, REL::VariantID> projectiles[] = {
    { "Projectile", RE::VTABLE_Projectile[0] },
    { "ArrowProjectile", RE::VTABLE_ArrowProjectile[0] },
    { "BarrierProjectile", RE::VTABLE_BarrierProjectile[0] },
    { "BeamProjectile", RE::VTABLE_BeamProjectile[0] },
    { "ConeProjectile", RE::VTABLE_ConeProjectile[0] },
    { "FlameProjectile", RE::VTABLE_FlameProjectile[0] },
    { "GrenadeProjectile", RE::VTABLE_GrenadeProjectile[0] },
    { "MissileProjectile", RE::VTABLE_MissileProjectile[0] }
  };
  std::vector<std::uintptr_t> impactsAt;
  for (const auto& [name, vtable] : projectiles) {
    const std::uintptr_t at = VtableSlot(vtable, 0xAC);
    if (!at) {
      continue;
    }
    if (std::find(impactsAt.begin(), impactsAt.end(), at) != impactsAt.end()) {
      logger::info("[verb:prop-physics] impulse bracket {}::ProcessImpacts "
                   "(slot 0xac): {}, attached already",
                   name, SlotPlace(at));
      continue;
    }
    const bool attached =
      handler->Attach(Frida::HookID::HOOK_IMPULSE_IMPACTS, at);
    if (attached) {
      impactsAt.push_back(at);
    }
    logger::info("[verb:prop-physics] impulse bracket {}::ProcessImpacts "
                 "(slot 0xac): {}, {}",
                 name, SlotPlace(at), attached ? "attached" : "NOT attached");
  }
  StartImpulseReporter();
  // the lab tells a build with the rule by this line (prop-impulse.js v1.2
  // runs ATTACH_ONLY beside it: one replacement a function a process)
  logger::info("[verb:prop-physics] impulse hooks installed: "
               "ScaleGameplayImpulseForce (Address Library 26004 at +{:x}) "
               "{}, AddExplosionImpulse (26005 at +{:x}) {}; brackets "
               "Explosion::Update {}, ConeProjectile::HandleHits {}, "
               "ProcessImpacts at {} addresses; inert until "
               "setImpulseCauseRule(true)",
               sizedAt - base, sized ? "replaced" : "NOT replaced",
               treesAt - base, trees ? "replaced" : "NOT replaced",
               update ? "attached" : "NOT attached",
               hits ? "attached" : "NOT attached", impactsAt.size());
}

void Frida::InstallHooks()
{
  LoadAdmittedScripts();
  InstallSendEventHook();
  InstallDrawSheatheWeaponPcHook();
  InstallDrawSheatheWeaponActorHook();
  InstallSendAnimationEventHook();
  InstallPlayAnimationHook();
  InstallPerformActionHook();
  InstallProjectileLaunchHook();
  InstallRuneDecalHook();
  InstallProjectileInitVelocityHook();
  InstallPlaceAreaEffectHook();
  InstallDamageActorHook();
  // thuum tracker #178 and #175: a followed copy's get-up held while the
  // drive imposes its stream
  InstallActorGetUpHook();
  // thuum docs/verbs/leveling.md: the player's skill uses to JS
  InstallUseSkillHook();
  InstallGuestWitnessHook();
  InstallSendTopicInfoEventHook();
  // thuum docs/verbs/quest-stage.md, slice two: a relayed stage's fragment
  InstallQuestStageSinkHook();
  // thuum docs/verbs/npc-fate.md: a hosted body's reanimation by the engine
  InstallReanimateHook();
  // thuum docs/verbs/prop-physics.md: the player's touch of a prop's body
  InstallPropContactHook();
  // and an impulse's cause (ADR-071 rung 3), inert until
  // setImpulseCauseRule(true)
  InstallImpulseHooks();
  InstallQueueNinodeUpdateHook();
  InstallRenderCursorMenuHook();
#ifdef ENABLE_SKYRIM_AE
  InstallApplyMasksToRenderTargetsHook();
#endif
  // thuum docs/verbs/appearance.md: a figure's base, posing as the player,
  // takes no overlay head parts (the headless figure on a vampire's seat)
  InstallHasOverlaysHook();
  InstallNativeFunctionCtorHook();
  // thuum docs/verbs/npc-puppet.md: the puppet shape's two detours
  Puppets::InstallHooks();
  // thuum docs/verbs/npc-fate.md: the roll's two detours, inert until
  // setActorFateEnabled(true)
  Fates::InstallHooks();
  // thuum docs/verbs/change-form.md (ADR-065): the id census on 35928 and
  // the address proof line of the change-form natives
  ChangeForm::InstallHooks();

  logger::info("Frida hooks installed.");
}
