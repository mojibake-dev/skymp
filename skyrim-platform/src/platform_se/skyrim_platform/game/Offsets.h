#pragma once

/**
 * Idea is to keep offsets in one place so it would be easier to control these
 * in case of changes etc.
 */
namespace Offsets {

const uintptr_t BaseAddress = Offsets::BaseAddress;

namespace Hooks {
inline REL::Relocation<std::uintptr_t> VPrint{
  RELOCATION_ID(50180, 51110), REL::VariantOffset(0x163, 0x300, 0x163)
};
inline REL::Relocation<std::uintptr_t> FrameUpdate{
  RELOCATION_ID(35565, 36564), REL::VariantOffset(0x53, 0x6e, 0x68)
};
// thuum docs/verbs/cell-host.md (the quitting host, ADR-047's D12 B): the
// call WinMain (36544) makes to the engine's shutdown routine (36551) at
// WinMain+0x1ae, the first instruction after Main::quitGame (Main.h:77) is
// seen true. AE only: 1.6.1170's ids by `just addr` (0x63e970, 0x641c60),
// the call site by REA on sky-re 2026-10-24 (CONFIRMED-STATIC); the SE id
// and offset are unread, and the hook installs on AE runtimes only.
inline REL::Relocation<std::uintptr_t> QuitShutdownCall{ REL::ID(36544),
                                                         0x1ae };
inline REL::Relocation<std::uintptr_t> SendEvent{ RELOCATION_ID(98077,
                                                                104800) };
inline REL::Relocation<std::uintptr_t> DrawSheatheWeaponPC{ RELOCATION_ID(
  40232, 41235) };
inline REL::Relocation<std::uintptr_t> DrawSheatheWeaponActor{ RELOCATION_ID(
  36289, 37279) };
inline REL::Relocation<std::uintptr_t> SendAnimation{ RELOCATION_ID(37020,
                                                                    38048) };
// thuum docs/verbs/cast-action.md: the engine's action entry,
// ActorMediator::PerformAction, (ActorMediator*, TESActionData*) -> bool. AE
// only: 1.6.1170's id by lab/addr.py, the function SourceActionMap::DoAction
// (34202) reaches through TESActionData::Process (41557), read on sky-re
// 2026-10-14; the SE id is unread, and the hook installs on AE runtimes only
// (ADR-025's 1.6.1170).
inline REL::Relocation<std::uintptr_t> PerformAction{ REL::ID(38949) };
// thuum docs/verbs/figure-values.md: the engine's damage writer for an
// actor, (Actor* this, float damage in XMM1, Actor* attacker, bool killMove)
// -> bool (this->IsDead), reached by every damage path (DamageActor, Address
// Library 37335 on 1.6.1170: lab/frida/figure-dent-trace.js measured the
// write inside it before the HandleHealthDamage vfunc, 2026-10-20;
// docs/research/re-37335-and-rune-landing.md). Not in CommonLibSSE-NG, the
// SE id unread: AE only, as PerformAction.
inline REL::Relocation<std::uintptr_t> DamageActor{ REL::ID(37335) };
// Projectile::Launch, (ProjectileHandle*, LaunchData&) -> ProjectileHandle*:
// the engine's one launch of every projectile, an arrow's or a spell's
// (CommonLibSSE-NG src/RE/P/Projectile.cpp:255-258, RELOCATION_ID(42928,
// 44108); include/RE/P/Projectile.h:78-107 the LaunchData). AE id; thuum
// docs/verbs/marksman.md, the arrow's path identical on both screens.
inline REL::Relocation<std::uintptr_t> ProjectileLaunch{ REL::ID(44108) };
// A missile's velocity set on its first update, Projectile vfunc called as
// (Projectile*): the angles here are its final ones, the player's arrow's
// after the engine's auto-aim rewrote them (41248), which Launch's entry does
// not yet carry. Not in CommonLibSSE-NG; the re-analyst on sky-re
// (2026-10-16, Ghidra, 1.6.1170: InitVelocity, AE id 44184, RVA 0x7e99a0 by
// lab/addr.py), HYPOTHESIS until x-arrow-path. thuum docs/verbs/marksman.md.
inline REL::Relocation<std::uintptr_t> ProjectileInitVelocity{ REL::ID(
  44184) };
// The place of a target-location effect that is not Aimed: (MagicCaster*,
// const NiPoint3* point, TESObjectCELL*, MagicItem*, Effect* largestArea,
// BSScrapArray<NiPointer<TESObjectREFR>>* targets) -> void, FindTargets
// 34410's one call after the pick and the snap; it creates the effect's
// explosion at the point, owned by the caster's actor, and applies no spell
// number itself (the re-analyst on sky-re, 2026-10-17, 1.6.1170, signature
// CONFIRMED-STATIC; thuum docs/research/apocalypse/target-location.md). AE
// id only: CommonLibSSE-NG binds SE 34413 to DialogueItem::Ctor.
inline REL::Relocation<std::uintptr_t> PlaceAreaEffect{ REL::ID(34413) };
inline REL::Relocation<std::uintptr_t> QueueNinodeUpdate{ RELOCATION_ID(
  39181, 40255) };
// thuum docs/verbs/npc-puppet.md: the two detours of the puppet shape and
// the engine's own job and push they reuse. AE only, 1.6.1170's ids by
// lab/addr.py, read by the re-analyst on sky-re (REA over Ghidra,
// 2026-10-21, CONFIRMED-STATIC for the prototypes; HYPOTHESIS names):
// 41370 the "Actor animation" stage's queueing (ProcessLists*, JobList*),
// 41453 the per-actor animation job (a 32-bit ActorHandle), 69349 the job
// list's push (jobs, fn, arg, an unused fourth), 37356 the per-frame
// movement step (Actor*, float delta).
inline REL::Relocation<std::uintptr_t> QueueActorAnimation{ REL::ID(41370) };
inline REL::Relocation<std::uintptr_t> ActorAnimationJob{ REL::ID(41453) };
inline REL::ID JobListPush(69349);
inline REL::Relocation<std::uintptr_t> ActorMovementStep{ REL::ID(37356) };
// thuum docs/verbs/npc-fate.md (ADR-046 decision 1, ADR-049): the two
// detours of the roll pushed to both (FateHooks.cpp). AE only, 1.6.1170's
// ids by `just addr` on 2026-10-23, the re-analyst's REA read on sky-re
// (docs/research/m3/re-npc-roll.md sections 6 and 8.5): 14752 the
// per-reference roll wrapper (TESLeveledList*, TESObjectREFR*,
// BSScrapArray<CALCED_OBJECT>*), 14374's one call for the LVLN pick at
// InitItemImpl (HYPOTHESIS name; its two lab observations held three times
// on 2026-10-23); 16155 the outfit-array resolver (BSTArray<TESForm*>*
// a_items, OutfitInitCtx* a_ctx), unnamed in CommonLibSSE-NG, the one
// function every outfit resolution passes through: InitLeveledItems' two
// calls at the inventory's init and InitOutfitItems' (16072) late one from
// the outfit equipper (callers and registers CONFIRMED-STATIC, sections 8.1
// and 8.2). Section 8 replaces the 16072 site: the lab read found it never
// entered at the load.
inline REL::Relocation<std::uintptr_t> LeveledRoll{ REL::ID(14752) };
inline REL::Relocation<std::uintptr_t> OutfitResolve{ REL::ID(16155) };
// thuum docs/verbs/npc-fate.md, the hands: the container-path adder every
// CNTO leveled roll passes through (re-npc-roll.md section 11, REA on
// sky-re 2026-10-24; `just addr 16128` 0x236890 on 1.6.1170), `void
// (InventoryChanges*, BSScrapArray<CALCED_OBJECT>*, uint32 ordinal,
// BGSOutfit*)`, outfit null on the container path
inline REL::Relocation<std::uintptr_t> ContainerAdd{ REL::ID(16128) };
inline REL::Relocation<std::uintptr_t> ApplyMasksToRenderTargets{
  RELOCATION_ID(26454, 27040)
};
inline REL::Relocation<std::uintptr_t> RenderCursorMenu{ RELOCATION_ID(
  32867, 33632) };
}

// thuum docs/verbs/cast-action.md: the replay side of the action entry. AE
// only, 1.6.1170's ids by lab/addr.py, read on sky-re 2026-10-14 (HYPOTHESIS
// until the lab's trace): PerformChosenAction is ActorMediator's second
// entry, the tail of PerformAction that performs a TESActionData as given
// (the output's event sent to the actor's graph, the engine's books kept),
// with no validation and no idle selection; ActorMediatorSingleton holds the
// ActorMediator* its constructor stores. The post step the first entry and
// this one both call (39004, the actor-state bookkeeping keyed by the event
// name) is not called again here: it is not the master-behavior variables,
// which applyAnimationVariablesToActor writes before the replay.
namespace Actions {
inline REL::ID PerformChosenAction(38953);
inline REL::ID ActorMediatorSingleton(403567);
}

// thuum docs/verbs/npc-stream.md (the voice slice): the engine's say and the
// first half of the pair that ends a line. The re-analyst's REA read of
// 1.6.1170 on sky-re (2026-10-26, CONFIRMED-STATIC by instruction; the ids
// by lab/addr.py): SayTopic 39162 (asm 0x1406d71e0), `bool (AIProcess*,
// Actor* speaker, TESTopic*, TESTopicInfo* info, bool unused, bool flag6,
// bool flag7, bool keepExtra)`, the function Papyrus Say (56220) calls for a
// living actor with (process, actor, topic, 0, 0, 0, 1, 1); a given info
// skips the condition pick (25541 to 25544), the topic must be its parent.
// UpdateVoiceTimer 37752 (asm 0x14068f4e0), `void (Actor*, bool force)`,
// which the AI update (37347) runs right before Actor::UpdateSoundCallBack
// (CommonLibSSE-NG Actor.h:287, vtable slot 0x40) for a live actor and
// skips for an AI-off one. AE only, as the rest of this file.
namespace Voice {
inline REL::ID SayTopic(39162);
inline REL::ID UpdateVoiceTimer(37752);
}

// thuum docs/verbs/npc-puppet.md: Actor::ClearKeepOffsetFromActor, not in
// CommonLibSSE-NG; `void (RE::Actor*)`, the Papyrus native's body (REA on
// sky-re 2026-10-21, CONFIRMED-STATIC). AE only, 1.6.1170's id by lab/addr.py.
namespace Puppet {
inline REL::ID ClearKeepOffsetFromActor(37895);
}

// thuum docs/verbs/npc-fate.md: the engine calls the fate natives make
// (FateApi.cpp, FateHooks.cpp). AE only, 1.6.1170's ids by `just addr` on
// 2026-10-23 (docs/research/m3/re-npc-roll.md sections 6 and 8.5).
namespace Fate {
// the re-roll of a leveled actor `(RE::Actor*, int a_arg)`, -1 skipping its
// level check; it restores the placed base, drops the clone and the 3D and
// re-resolves through 14374, hence through Hooks::LeveledRoll (section 1 and
// section 3 (b), HYPOTHESIS)
inline REL::ID ActorReroll(37323);
// the ExtraOutfitItem setter `(ExtraDataList*, BGSOutfit*)` (section 8.2,
// CONFIRMED-STATIC)
inline REL::ID SetOutfitItemTag(11647);
// the entry adder `(InventoryChanges*, TESForm*, ExtraDataList*, int32
// count, TESObjectREFR* fromRefr)`, 16155's own add (sections 8.2 and 8.5,
// HYPOTHESIS)
inline REL::ID AddEntryWithList(16053);
// Actor::RemoveOutfitItems `(Actor*, BGSOutfit*)` (section 6, CommonLib
// name; the pin binds it at src/RE/A/Actor.cpp:1300-1305, so the member is
// what is called and this id is the record)
inline REL::ID RemoveOutfitItems(19690);
// Actor::AddWornOutfit `(Actor*, BGSOutfit*, bool a_forceUpdate)` (section
// 6, CommonLib name; bound at src/RE/A/Actor.cpp:1293-1298, called through
// the member; section 8.4's open question: it counted zero in the lab read)
inline REL::ID AddWornOutfit(19692);
}

namespace EventSource {
inline REL::ID ActorKill(REL::Relocate(37390, 38338));
inline REL::ID BooksRead(REL::Relocate(17470, 17865));
inline REL::ID CriticalHit(REL::Relocate(37726, 38671));
inline REL::ID DisarmedEvent(REL::Relocate(37392, 38340));
inline REL::ID DragonSoulsGained(REL::Relocate(37571, 38520));
inline REL::ID ItemHarvested(REL::Relocate(14704, 14875));
inline REL::ID LevelIncrease(REL::Relocate(39247, 40319));
inline REL::ID LocationDiscovery(REL::Relocate(40056, 41067));
// AE only: 1.6.1170's id by lab/addr.py (thuum docs/verbs/poison.md); the
// SE id is unread, and PoisonedWeaponSource checks the runtime first
inline REL::ID PoisonedWeapon(41069);
inline REL::ID ShoutAttack(REL::Relocate(40060, 41071));
inline REL::ID SkillIncrease(REL::Relocate(39248, 40320));
inline REL::ID SoulsTrapped(REL::Relocate(37916, 38873));
inline REL::ID SpellsLearned(REL::Relocate(37917, 38874));
}

namespace BSRenderManager {
inline REL::ID Singleton(REL::Relocate(524907, 411393));
}

namespace MenuScreenData {
inline REL::ID Singleton(REL::Relocate(517043, 403551));
}

inline REL::ID WinMain(REL::Relocate(35545, 36544));

inline RE::ObjectRefHandle GetInvalidRefHandle()
{
  REL::Relocation<RE::ObjectRefHandle*> handle{ REL::ID(
    REL::Relocate(514164, 400312)) };
  return *handle;
}

/**
 * This is called from CallNative::CallNativeSafe
 * no idea what it does, should be renamed.
 */
inline float Unknown(void* unk1, void* unk2, RE::TESObjectREFR* obj)
{
  using func_t = decltype(&Unknown); // 55622
  REL::Relocation<func_t> func{ REL::ID(REL::Relocate(55622, 56151)) };
  return func(unk1, unk2, obj);
}

inline void PushActorAway(void* vm, StackID stackId, RE::Actor* self,
                          RE::Actor* targetActor, float magnitude)
{
  using func_t = decltype(&PushActorAway);
  REL::Relocation<func_t> func{ REL::ID(REL::Relocate(55682, 56213)) };
  func(vm, stackId, self, targetActor, magnitude);
}

}

class TESDataHandlerExtension
{
public:
  RE::ObjectRefHandle CreateReferenceAtLocationImpl(
    RE::TESBoundObject* a_base, const RE::NiPoint3& a_location,
    const RE::NiPoint3& a_rotation, RE::TESObjectCELL* a_targetCell,
    RE::TESWorldSpace* a_selfWorldSpace,
    RE::TESObjectREFR* a_alreadyCreatedRef, RE::BGSPrimitive* a_primitive,
    const RE::ObjectRefHandle& a_linkedRoomRefHandle, bool a_forcePersist,
    bool a_arg11)
  {
    using func_t =
      decltype(&TESDataHandlerExtension::CreateReferenceAtLocationImpl);

    int id = REL::Relocate(13625, 13723);

    REL::Relocation<func_t> func{ REL::ID(id) };
    return func(this, a_base, a_location, a_rotation, a_targetCell,
                a_selfWorldSpace, a_alreadyCreatedRef, a_primitive,
                a_linkedRoomRefHandle, a_forcePersist, a_arg11);
  }
};
