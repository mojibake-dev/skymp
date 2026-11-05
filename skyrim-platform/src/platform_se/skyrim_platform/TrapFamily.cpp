#include "TrapFamily.h"
#include <RE/B/BSAtomic.h>
#include <RE/I/IObjectHandlePolicy.h>
#include <RE/O/Object.h>
#include <RE/O/ObjectTypeInfo.h>
#include <RE/T/TESBoundObject.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>
#include <RE/V/Variable.h>
#include <RE/V/VirtualMachine.h>
#include <atomic>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace {
// ==== THE ONE PLACE THE FAMILY IS NAMED ====
// The trap family as ADR-068's D-S3 accepted it (point 1) and Eli's D-T1
// widened it (2026-11-05: "all traps should be equally triggerable by both
// guest and host"), measured over every script in the lab's load order
// (thuum docs/research/trap-prefix-census-2026-11-05.md; USSEP's copies win
// where it ships one). A script class is a trap class when its parent chain
// reaches one of the roots, or when its own name begins with the prefix:
// - The roots. TrapBase, TrapTriggerBase and TrapHitBase, D-S3's chains
//   (docs/research/trap-census-2026-11-05.md): PressurePlate, Tripwire,
//   SoundTrap and TrapLever under TrapTriggerBase; MovingTrap
//   (TrapSwingingWall, BladeTrap), DartTrap, MagicTrap (the soul gem trap's
//   caster), PhysicsTrap and TrapRiggedRockfall under TrapBase;
//   PhysicsTrapHit under TrapHitBase. And HazardBase, a trap's hit script
//   outside them (ProcessTrapHit from its OnTrapHitStart), alone on 34 spike
//   beds and beside TrapNorPlatform on 15 Nordic platforms.
// - The prefix. 32 scripts' names begin with "trap" in some case, four in
//   lower case (trapRockfallEffects and trapSoulGemController among them);
//   19 reach the three chains and 13 do not: on references the oil pool's
//   TrapOilPool, the bear trap's TrapBear (beside TrapHitBase), the oil
//   lamp's TrapFallingOilLampColLayer, TrapExplosiveGas, trapRockfallEffects,
//   TrapPoisonGas and the soul gem's trapSoulGemController; a magic effect's
//   TrapPoisonGasCough; five on nothing. The prefix is read on the class's
//   own name and never inherited through a parent that is not a root:
//   TG02Beehive and TGTQ04BannerBurnScript extend TrapExplosiveGas to carry
//   quest state (TG02's HiveGone, SetStage(TGTQ04, 65)), so their nine
//   references stay with the cell's host.
// Which references that makes is IsFamily's (a pickup never is). Measured on
// sky-srv over the load order's 33,142 scripted references, 4,887 against
// the three chains' 3,622: TrapOilPool's 793, TrapBear's 179,
// TrapFallingOilLampColLayer's 145, TrapExplosiveGas's 49, HazardBase's 48
// (one platform, 0xea269, carries USSEP's
// usleep_disablelinkedrefonactivation too), trapRockfallEffects' 37 and
// TrapPoisonGas's 14 join; no reference of the three chains' family leaves;
// the 52 soul gems carrying trapSoulGemController stay out as pickups, and
// 950 references with a trap class beside another script stay mixed (the
// design's F2: the levers and pull chains, five DLC1VC_EmbersToggleable)
constexpr const char* kFamilyRoots[] = { "TrapBase", "TrapTriggerBase",
                                         "TrapHitBase", "HazardBase" };
constexpr const char kFamilyPrefix[] = "trap";

// one class's own name against the roots, without case (Papyrus's rule)
bool IsRootName(const char* name)
{
  if (!name) {
    return false;
  }
  for (const char* root : kFamilyRoots) {
    if (_stricmp(name, root) == 0) {
      return true;
    }
  }
  return false;
}

// one class's own name begins with the prefix, without case
bool HasFamilyPrefix(const char* name)
{
  return name &&
    _strnicmp(name, kFamilyPrefix, sizeof(kFamilyPrefix) - 1) == 0;
}
// ==== END OF THE ONE PLACE ====

// the family's verdict per class, the chain walked at a class's first sight
// (rule 9: a hash lookup per attached script after that). The key is the
// class's ObjectTypeInfo, which the cache pins with a reference of its own,
// taken once and never released (ObjectTypeInfo is BSIntrusiveRefCounted,
// CommonLibSSE-NG include/RE/O/ObjectTypeInfo.h:16; IncRef at
// include/RE/B/BSIntrusiveRefCounted.h:13): a class the VM drops is then
// never freed, so its address can never come to name another class
struct
{
  std::unordered_map<const RE::BSScript::ObjectTypeInfo*, bool> family;
  std::mutex m;
} g_classes;

// the VM's attached scripts of a handle, walked under the VM's own lock: the
// gate's walk (FridaHooks.cpp OnSendEventEnter), attachedScriptsLock and
// attachedScripts (CommonLibSSE-NG include/RE/V/VirtualMachine.h:219-220),
// the lock held by CommonLib's own guard (include/RE/B/BSAtomic.h:93-107).
// f sees each attached script object, possibly null. False when the handle
// has no attached script; the array's size is read, never its empty(), which
// answers inverted in the pinned headers (include/RE/B/BSTArray.h:749)
template <class F>
bool ForEachAttached(RE::VMHandle handle, F&& f)
{
  auto* vm = VM::GetSingleton();
  if (!vm) {
    return false;
  }
  RE::BSSpinLockGuard guard{ vm->attachedScriptsLock };
  const auto it = vm->attachedScripts.find(handle);
  if (it == vm->attachedScripts.end() || it->second.size() == 0) {
    return false;
  }
  auto& scripts = it->second;
  for (std::uint32_t i = 0; i < scripts.size(); ++i) {
    f(scripts[i].get());
  }
  return true;
}

// HYPOTHESIS H-PO1: a trigger box's playerOnlyTrigger (TrapTriggerBase's
// property, set true by the VMAD of the Temple's 0x86018 and the Sanctum's
// 0x4e42b, the census's table) is the value Object::GetProperty finds
// (CommonLibSSE-NG src/RE/O/Object.cpp:66-76) once the object's properties
// are initialized. GetProperty takes the declaring class's own auto-variable
// index with no offset for the classes above it, where GetVariable adds one
// (:84-112), so the read holds while ObjectReference and Form declare no
// variables. getTrapRole(0x86018).playerOnly reading true and
// getTrapRole(0x567f2).playerOnly (the Temple plate, unset) false settles it
bool PlayerOnlyScript(RE::BSScript::Object* script, const FixedString& name)
{
  if (!script) {
    return false;
  }
  const RE::BSScript::Variable* value = script->GetProperty(name);
  return value && value->IsBool() && value->GetBool();
}

// the property's name, built once (a BSFixedString is the engine's string
// cache's) and asked by every caller before it takes the VM's lock
const FixedString& PlayerOnlyName()
{
  static const auto fsPlayerOnly = new FixedString("playerOnlyTrigger");
  return *fsPlayerOnly;
}

// a set of form ids in Hosted.h's idiom (Hosted.cpp): mutex-guarded, written
// on the JS thread, read by the hooks; the size kept beside it for a read
// with no lock
struct FlagSet
{
  std::unordered_set<uint32_t> ids;
  std::mutex m;
  std::atomic<size_t> size{ 0 };
};

FlagSet g_followed;
FlagSet g_recordApplying;

bool Has(FlagSet& set, uint32_t formId)
{
  std::lock_guard l(set.m);
  return set.ids.count(formId) != 0;
}

void Put(FlagSet& set, uint32_t formId, bool on)
{
  std::lock_guard l(set.m);
  if (on) {
    set.ids.insert(formId);
  } else {
    set.ids.erase(formId);
  }
  set.size.store(set.ids.size());
}

size_t SizeOf(FlagSet& set)
{
  std::lock_guard l(set.m);
  return set.ids.size();
}

// a pickup: a reference whose base is an inventory object (CommonLibSSE-NG's
// TESForm::IsInventoryObject, include/RE/T/TESForm.h:329, its form types at
// src/RE/T/TESForm.cpp:192-214; the base by TESObjectREFR::GetBaseObject,
// include/RE/T/TESObjectREFR.h:384). Taking it is the inventory's (R0), one
// owner, whatever script rides it: the 52 soul gems whose
// trapSoulGemController hands the presser the gem and disables it (base SLGM
// DummySoulGem 0x6a0c2, Skyrim.esm) stay the cell host's, while the trap
// that guards one, MagicTrap under TrapBase, fires on every seat
bool IsPickup(const RE::TESObjectREFR* ref)
{
  const RE::TESBoundObject* base = ref ? ref->GetBaseObject() : nullptr;
  return base && base->IsInventoryObject();
}
}

bool TrapFamily::IsFamilyType(const RE::BSScript::ObjectTypeInfo* info)
{
  if (!info) {
    return false;
  }
  std::lock_guard l(g_classes.m);
  const auto it = g_classes.family.find(info);
  if (it != g_classes.family.end()) {
    return it->second;
  }
  // the prefix on the class's own name alone; then AdmittedByChain's walk
  // (FridaHooks.cpp) for a root: ObjectTypeInfo's parent chain
  // (CommonLibSSE-NG include/RE/O/ObjectTypeInfo.h:127-128, :178) to a null
  // parent, 16 classes at most
  bool family = HasFamilyPrefix(info->GetName());
  const RE::BSScript::ObjectTypeInfo* cls = info;
  for (int depth = 0; cls && depth < 16 && !family;
       ++depth, cls = cls->GetParent()) {
    family = IsRootName(cls->GetName());
  }
  info->IncRef();
  g_classes.family.emplace(info, family);
  return family;
}

bool TrapFamily::IsFamily(const RE::TESObjectREFR* ref, RE::VMHandle handle)
{
  if (!ref || IsPickup(ref)) {
    return false;
  }
  bool every = true;
  const bool any = ForEachAttached(handle, [&](RE::BSScript::Object* script) {
    every = every && script && IsFamilyType(script->GetTypeInfo());
  });
  return any && every;
}

// TESForm::RecordFlags::kDestroyed on formFlags (CommonLibSSE-NG
// include/RE/T/TESForm.h:91, the accessor at :287). ObjectReference's
// SetDestroyed native sets and clears that bit and nothing else of the
// reference (HYPOTHESIS by decompile, ADR-064 decision 1; sky-re's Ghidra on
// 1.6.1170, 2026-11-05: its body flips bit 23 of formFlags, then calls
// AddChange with kFlags,
// TESForm.h:46 and :133). HYPOTHESIS H-DF1: a destruction reaching a stage
// that destroys the object sets the same bit (unread; x-trap-lamp v1's shot
// lamp reading getTrapRole(0x9b0fb).destroyed true settles it)
bool TrapFamily::Destroyed(const RE::TESObjectREFR* ref)
{
  return ref &&
    (ref->GetFormFlags() & RE::TESForm::RecordFlags::kDestroyed) != 0;
}

// the reference by its id (TESForm::LookupByID, under the form table's own
// lock), its handle through the VM's handle policy as CommonLib's
// TESForm::HasVMAD asks it (src/RE/T/TESForm.cpp:122-136), then one walk of
// its attached scripts for the family and the trigger's property
std::optional<TrapFamily::Role> TrapFamily::RoleOf(uint32_t formId)
{
  RE::TESForm* form = formId != 0 ? RE::TESForm::LookupByID(formId) : nullptr;
  RE::TESObjectREFR* ref = form ? form->AsReference() : nullptr;
  if (!ref) {
    return std::nullopt;
  }
  auto* vm = VM::GetSingleton();
  auto* policy = vm ? vm->GetObjectHandlePolicy() : nullptr;
  if (!policy) {
    return std::nullopt;
  }
  const RE::VMHandle handle =
    policy->GetHandleForObject(ref->GetFormType(), ref);
  const FixedString& name = PlayerOnlyName();
  Role role;
  bool every = true;
  const bool any = ForEachAttached(handle, [&](RE::BSScript::Object* script) {
    every = every && script && IsFamilyType(script->GetTypeInfo());
    role.playerOnly = role.playerOnly || PlayerOnlyScript(script, name);
  });
  if (!any) {
    return std::nullopt;
  }
  // IsFamily's test, read in the same walk
  role.family = every && !IsPickup(ref);
  role.destroyed = Destroyed(ref);
  return role;
}

bool TrapFamily::Followed::Is(uint32_t formId)
{
  return Has(g_followed, formId);
}

void TrapFamily::Followed::Set(uint32_t formId, bool followed)
{
  Put(g_followed, formId, followed);
}

size_t TrapFamily::Followed::Count()
{
  return SizeOf(g_followed);
}

bool TrapFamily::RecordApplying::Is(uint32_t formId)
{
  return Has(g_recordApplying, formId);
}

void TrapFamily::RecordApplying::Set(uint32_t formId, bool applying)
{
  Put(g_recordApplying, formId, applying);
}

size_t TrapFamily::RecordApplying::Count()
{
  return SizeOf(g_recordApplying);
}

bool TrapFamily::RecordApplying::Any()
{
  return g_recordApplying.size.load(std::memory_order_relaxed) != 0;
}

void TrapFamily::LogReady()
{
  std::string roots;
  for (const char* root : kFamilyRoots) {
    if (!roots.empty()) {
      roots += ", ";
    }
    roots += root;
  }
  logger::info("[verb:traps] trap family ready: {} root(s) by class chain "
               "({}) and the class's own name's prefix '{}', a pickup never "
               "(TrapFamily.cpp, D-T1), read by the event gate and the "
               "native seam; 3 export(s): getTrapRole, setPropFollowed, "
               "setRecordApplying",
               std::size(kFamilyRoots), roots, kFamilyPrefix);
}
