#include "FridaHookHandler.h"
#include "Puppets.h"
#include "game/Offsets.h"
#include <cmath>
#include <vector>

// thuum docs/verbs/npc-puppet.md, the two detours of the puppet shape (one AI
// bit plus two hooks, re-ai-puppet.md Q2). Every prototype below is the
// re-analyst's read of 1.6.1170 on sky-re (REA over Ghidra, 2026-10-21,
// CONFIRMED-STATIC by instruction; the ids resolved by `just addr`, RVAs
// supporting data): none is written from memory.
namespace {
// 41370 (HYPOTHESIS name ProcessLists_QueueActorAnimation): the "Actor
// animation" stage's queueing, `void (RE::ProcessLists* self, JobList*
// jobs)`; it partitions highActorHandles with 41466, whose one kProcessMe
// test drops every AI-off actor, and queues the animation job 41453 per
// admitted actor through the engine's push 69349. The detour calls the
// original and queues the same job for each flagged actor the gate dropped,
// so a puppet's graph ticks in the engine's own stage and thread model
// (hook 2a; the CK: an AI-off actor's model otherwise stays behind its
// collision).
using QueueActorAnimationFn = void (*)(RE::ProcessLists* lists, void* jobs);
QueueActorAnimationFn g_queueActorAnimationOriginal = nullptr;

// 69349 (HYPOTHESIS name JobList_AddJob): `void (JobList* jobs, void* fn,
// uint32_t arg, uint32_t unused)`; every caller zeroes the fourth argument
// and the function never reads it. 41453 takes the 32-bit ActorHandle value.
using JobListPushFn = void (*)(void* jobs, void* fn, uint32_t arg,
                               uint32_t unused);

// 37356 (HYPOTHESIS name Actor_MovementStep): `void (RE::Actor* self, float
// delta)`, the per-frame movement step (the controller's output moved
// through Actor::Move, SetRotationZ, the feedback speed and direction), no
// kProcessMe test in it or its callers 37350, 41444, 41306.
using MovementStepFn = void (*)(RE::Actor* actor, float delta);
MovementStepFn g_movementStepOriginal = nullptr;

// 41466's admission, without its kProcessMe term (the puppet's bit is clear,
// so the original queued nothing for it; an AI-on actor is already queued and
// a second job would tick its graph twice)
bool AnimatesAsPuppet(RE::Actor& actor)
{
  if (actor.IsAIEnabled() || !Puppets::Is(actor.formID)) {
    return false;
  }
  // CommonLibSSE-NG keeps Actor's runtime-dependent fields behind
  // GetActorRuntimeData() (Actor.h:651 and :647 are its members)
  auto& runtime = actor.GetActorRuntimeData();
  if (!runtime.currentProcess ||
      runtime.boolBits.any(RE::Actor::BOOL_BITS::kInTempChangeList) ||
      actor.IsDeleted() || actor.IsDisabled()) {
    return false;
  }
  auto* cell = actor.GetParentCell();
  if (!cell ||
      cell->cellState.get() != RE::TESObjectCELL::CellState::kAttached) {
    return false;
  }
  auto* node = actor.Get3D();
  return node && node->parent;
}

// thuum docs/verbs/npc-stream.md (the voice slice): a line on a puppet is
// ended by the engine's own pair, which the AI update runs for a live actor
// and skips for an AI-off one (37347 returns at kProcessMe before the voice
// timer and UpdateSoundCallBack, the re-analyst's read of 2026-10-26): left
// alone, a puppet's line plays its audio and lip sync, shows no subtitle and
// never ends, and the next line is refused while it stands (HYPOTHESIS until
// P7). So the pair runs here, once a frame on the game thread, for a puppet
// with a line in flight: the callback state 3 (just started: the subtitle
// and the timer are armed), the timer counting, or 1 and 2 (the timer ran
// out: the end event, the subtitle gone)
using UpdateVoiceTimerFn = void (*)(RE::Actor*, bool);

void DriveVoice(RE::Actor& actor)
{
  auto& runtime = actor.GetActorRuntimeData();
  const int state = runtime.soundCallBackSet;
  if (state != 1 && state != 2 && state != 3 && runtime.voiceTimer <= 0.f) {
    return;
  }
  REL::Relocation<UpdateVoiceTimerFn> updateVoiceTimer{
    Offsets::Voice::UpdateVoiceTimer
  };
  updateVoiceTimer(&actor, false);
  actor.UpdateSoundCallBack(false);
  const int after = runtime.soundCallBackSet;
  if ((state == 1 || state == 2) && after != state) {
    logger::info("[verb:npc-stream] line on {:x} ended (state {} -> {})",
                 actor.formID, state, after);
  }
}

void QueueActorAnimationReplacement(RE::ProcessLists* lists, void* jobs)
{
  g_queueActorAnimationOriginal(lists, jobs);
  if (!lists) {
    return;
  }
  // the high-process actors by form id, for the roster and the demote poll
  // (Puppets.h): read here on the game thread where the engine owns the
  // list, a few dozen handles a frame
  {
    std::vector<uint32_t> ids;
    ids.reserve(lists->highActorHandles.size());
    for (const auto& handle : lists->highActorHandles) {
      auto actor = handle.get();
      if (actor) {
        ids.push_back(actor->formID);
      }
    }
    Puppets::SetHighActors(std::move(ids));
  }
  if (!jobs || Puppets::Count() == 0) {
    return;
  }
  REL::Relocation<JobListPushFn> push{ Offsets::Hooks::JobListPush };
  auto* job =
    reinterpret_cast<void*>(Offsets::Hooks::ActorAnimationJob.address());
  for (const auto& handle : lists->highActorHandles) {
    auto actor = handle.get();
    if (actor && AnimatesAsPuppet(*actor)) {
      push(jobs, job, handle.native_handle(), 0);
      DriveVoice(*actor);
    }
  }
}

// Hook 3. A puppet's movement is the relay's, so the engine's step is not
// run for it. Skyrim Together's plain early return left the character
// controller's step time at zero (its #901: the rigid-body controller
// divides a velocity change by stepInfo.deltaTime in SetLinearVelocity and
// SetPosition, so every relayed warp divided by zero and nearby bodies
// glided). R1 (REA 2026-10-21): the step time is stamped only inside the
// controller's own simulate (78275), reached through Actor::Move (vtable
// 0xC8, 37507), which 37356 itself calls with a zero translation for
// ragdolled actors and sitting riders. So a puppet gets Move(delta, zero):
// the controller ticks and stamps its step at rest where the relay last
// warped it, gravity settles it to the ground, and the divisions stay
// finite; the step's own output, rotation and feedback are skipped. The
// precondition is the engine's: a controller and an attached cell (the
// not-attached path never Moves). HYPOTHESIS until probe P1's trace;
// fallback shape 4 in the verb doc.
void MovementStepReplacement(RE::Actor* actor, float delta)
{
  if (!actor || !Puppets::Is(actor->formID)) {
    g_movementStepOriginal(actor, delta);
    return;
  }
  auto* cell = actor->GetParentCell();
  auto* controller = actor->GetCharController();
  if (!actor->GetActorRuntimeData().currentProcess || !controller || !cell ||
      cell->cellState.get() != RE::TESObjectCELL::CellState::kAttached) {
    return;
  }
  if (actor->IsInRagdollState()) {
    controller->flags.set(RE::CHARACTER_FLAGS::kFollowRagdoll);
  }
  actor->Move(delta, RE::NiPoint3{ 0.f, 0.f, 0.f });

  // docs/research/m3/re-puppet-locomotion.md (R2, 2026-10-22): the graph's
  // Speed, Direction and TurnDelta are Actor data channels polled at every
  // graph tick from MiddleHighProcessData::currentMovementSpeed (:197),
  // rotation.z (:138, the move direction feedback in radians clockwise from
  // the facing) and rotationSpeed.z (:139), fields only the skipped step
  // and its feedback tail write; a Papyrus SetAnimationVariableFloat lives
  // less than a frame (the channel push rewrites the slot before the step).
  // So the host's relayed sample is written here, every frame, before the
  // animation stage polls (HYPOTHESIS on the stage order, probe F5); the
  // Speed channel's own threshold at 5.0 then raises moveStart and moveStop,
  // and the Direction channel the move direction actions, as for a live
  // actor (HYPOTHESIS, probe F2 and F3). unk2AC is TargetSpeed's source
  // (:198; HYPOTHESIS that the graph reads it).
  Puppets::Locomotion sample;
  if (Puppets::GetLocomotion(actor->formID, sample)) {
    if (auto* mh = actor->GetActorRuntimeData().currentProcess->middleHigh) {
      constexpr float kDegToRad = 0.017453292f;
      constexpr float kTwoPi = 6.2831853f;
      float direction = sample.directionDeg * kDegToRad;
      direction = std::fmod(direction, kTwoPi);
      if (direction < 0.f) {
        direction += kTwoPi;
      }
      // a stale sample (no newer one in kLocomotionStaleMs) is a standing
      // record: the host sends a changed sample every 130 ms while the actor
      // moves, so a walk never goes stale while it is one (HYPOTHESIS until
      // x-npc-talk reads a talker's puppet still at Speed 0)
      const bool stale =
        Puppets::NowMs() - sample.stampMs > Puppets::kLocomotionStaleMs;
      const float speed = stale ? 0.f : sample.speed;
      mh->currentMovementSpeed = speed;
      mh->unk2AC = speed;
      mh->rotation.z = direction;
      mh->rotationSpeed.z = -sample.turnRateDeg * kDegToRad;
    }
  }
}
}

void Puppets::InstallHooks()
{
  if (!REL::Module::IsAE()) {
    return;
  }
  auto* handler = Frida::HookHandler::GetSingleton();
  const bool animation =
    handler->Replace(Offsets::Hooks::QueueActorAnimation.address(),
                     reinterpret_cast<void*>(&QueueActorAnimationReplacement),
                     reinterpret_cast<void**>(&g_queueActorAnimationOriginal));
  const bool movement =
    handler->Replace(Offsets::Hooks::ActorMovementStep.address(),
                     reinterpret_cast<void*>(&MovementStepReplacement),
                     reinterpret_cast<void**>(&g_movementStepOriginal));
  logger::info("[verb:npc-puppet] detours installed: animation queue {}, "
               "movement step {}",
               animation, movement);
}
