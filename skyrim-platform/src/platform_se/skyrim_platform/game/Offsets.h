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
inline REL::Relocation<std::uintptr_t> ProjectileInitVelocity{ REL::ID(44184) };
inline REL::Relocation<std::uintptr_t> QueueNinodeUpdate{ RELOCATION_ID(
  39181, 40255) };
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
