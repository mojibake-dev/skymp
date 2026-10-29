#include "PuppetApi.h"
#include "CallNativeApi.h"
#include "FridaHooks.h"
#include "Hosted.h"
#include "ItemMenuRefresh.h"
#include "Puppets.h"
#include "game/Offsets.h"
#include <RE/A/ActivateHandler.h>
#include <RE/C/Calendar.h>
#include <RE/C/ContainerMenu.h>
#include <RE/C/CrosshairPickData.h>
#include <RE/D/DialogueMenu.h>
#include <RE/I/InterfaceStrings.h>
#include <RE/M/MenuTopicManager.h>
#include <RE/P/PlayerControls.h>
#include <RE/T/TESFaction.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESTopic.h>
#include <RE/T/TESTopicInfo.h>
#include <RE/U/UI.h>
#include <RE/U/UIMessage.h>
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

extern CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

namespace {
struct
{
  std::unordered_set<uint32_t> ids;
  std::unordered_map<uint32_t, Puppets::Locomotion> locomotion;
  std::vector<uint32_t> highActors;
  std::recursive_mutex m;
} g_puppets;

// thuum docs/verbs/npc-stream.md (the voice slice): the engine's say with a
// given info (Offsets::Voice::SayTopic; the prototype the re-analyst read),
// and the answers of the asks, by actor
using SayTopicFn = bool (*)(RE::AIProcess*, RE::Actor*, RE::TESTopic*,
                            RE::TESTopicInfo*, bool, bool, bool, bool);
std::mutex g_sayResultMutex;
std::unordered_map<uint32_t, int> g_sayResult;

// Actor::ClearKeepOffsetFromActor is not in CommonLibSSE-NG (only the
// IMovementSetKeepOffsetFromActor vtable is). The engine's function (37895;
// REA on sky-re 2026-10-21, CONFIRMED-STATIC: `void (RE::Actor*)`, it flips
// the movement controller's keep-offset mode byte and nothing else)
using ClearKeepOffsetFn = void (*)(RE::Actor*);

// re-ai-puppet.md Q4, live to puppet: combat and alarm off, the keep-offset
// left, the AI bit cleared (EnableAI(false) also zeroes the character
// controller's velocity, src/RE/A/Actor.cpp:230-240); the actor stays in
// high process, where the detours animate it
void Demote(RE::Actor& actor)
{
  // an actor with no 3D (a persistent one its game has left behind, demoted
  // when the server took its form back, skymp5-client formView.destroy)
  // has no combat, no movement controller and no process to stop: the bit
  // alone, which EnableAI(false) writes without touching a controller
  // (src/RE/A/Actor.cpp:230-240, GetCharController null-safe)
  // the engine's activation (Talk, trade, pickpocket) is the host's alone:
  // blocked natively from the demote (TESObjectREFR::SetActivationBlocked,
  // CommonLibSSE-NG include/RE/T/TESObjectREFR.h:464), so a puppet that has
  // taken no relayed sample yet is blocked too (playtest 33: E on such a
  // puppet opened the engine's dialogue with an AI-off actor, which never
  // answered, and the Dialogue Menu stayed; no Papyrus call at the attach)
  if (!actor.Is3DLoaded()) {
    // thuum docs/verbs/npc-handoff.md D11 (b-npc-handoff-night 092208): an
    // actor with no 3D is simulated by nobody's engine, and clearing its AI
    // bit pinned it in an unhosted interior for good: no word reaches a
    // record nobody subscribes, so no promote ever came, and the morning
    // never brought the resident out (three of three on that seat, none on
    // the seat that had not been live for them). It keeps its AI and its
    // block untouched: the engine's own low-process schedule moves it, the
    // roster of whichever host it walks in on sights it and the record moves
    // to the sighter's cell; a copy that wandered this way is rebound and
    // placed by the server's word when its cell loads again.
    // the flag the export set ahead of this task comes off too: a flagged
    // actor has its movement step skipped by the detours once the engine
    // processes it (PuppetHooks.cpp), which would pin it the moment it loads
    Puppets::Set(actor.formID, false);
    logger::info("[verb:npc-puppet] {:x} lost with no 3D: left to its "
                 "schedule, AI, activation and flag untouched (npc-handoff "
                 "D11)",
                 actor.formID);
    return;
  }
  actor.SetActivationBlocked(true);
  if (auto* lists = RE::ProcessLists::GetSingleton()) {
    lists->StopCombatAndAlarmOnActor(&actor, false);
  }
  // the engine's clear reads the movement controller with no null test
  // (37895 to 41734(controller, 0), REA 2026-10-21); an actor whose process
  // has not started has none yet (GetActorRuntimeData().movementController,
  // Actor.h's runtime data), so the clear waits for a later call
  if (actor.GetActorRuntimeData().movementController.get() != nullptr) {
    REL::Relocation<ClearKeepOffsetFn> clearKeepOffset{
      Offsets::Puppet::ClearKeepOffsetFromActor
    };
    clearKeepOffset(&actor);
  }
  actor.EnableAI(false);
  logger::info("[verb:npc-puppet] puppet {:x}: AI off, combat stopped, "
               "keep-offset cleared, activation blocked",
               actor.formID);
}

// puppet to live: the AI bit set and the package evaluated on the first AI
// tick (EvaluatePackage(false, false), Papyrus's shape; an immediate one runs
// only in sit states 0, 3 and 7, re-ai-puppet.md:341-348). The relayed point
// and heading are written by skymp5-client before this call.
void Promote(RE::Actor& actor)
{
  // the puppet's block lifted with the AI: the host's residents can be
  // talked to (playtest 32's dead Talk prompts after a handoff)
  actor.SetActivationBlocked(false);
  // thuum docs/verbs/npc-fate.md H-54e (54's gate 20261027-190413): a corpse
  // promoted at its host's reconnect (the reconciler's flip, puppet then live
  // within a sample as the grant arrived) walked 1300 units at a wolf's trot
  // with no height change, and the record followed: EnableAI(true) and
  // EvaluatePackage on a dead actor hand the engine's package a body. A dead
  // actor keeps its AI off here (Actor::IsDead, CommonLibSSE-NG
  // include/RE/A/Actor.h:345); the block comes off for the loot, and the
  // AI comes on with the body's resurrection alone: the fate's resurrect
  // when the record stands again (FateApi.cpp resurrectActor), or the
  // engine's own Resurrect on this seat, a necromancer's reanimate, through
  // the Character vtable hook (Hooks.cpp HostedResurrect, finding 54)
  if (actor.IsDead()) {
    logger::info("[verb:npc-puppet] promote {:x}: dead, the block lifted "
                 "and the AI left off (npc-fate H-54e)",
                 actor.formID);
    return;
  }
  // thuum playtest 37 (2026-10-28 20:52Z, Alvor): a relayed line said here
  // through the sound descriptor path (a "played" result) left its item in
  // flight (greetingPlayer set, the handle live) past its end, and the AI's
  // first tick on the promoted actor drained it as greetings: four BEGINs in
  // 100 ms and the player's dialogue snapped shut three times in twelve
  // seconds. The item is cleared and the handle stopped before the AI runs.
  // HYPOTHESIS until the probe reads a promote after a "played" line.
  if (auto* process = actor.GetActorRuntimeData().currentProcess) {
    if (auto* high = process->high) {
      const bool handleLive = high->soundHandles[0].IsValid();
      if (high->greetingPlayer || handleLive) {
        if (handleLive) {
          high->soundHandles[0].Stop();
        }
        REL::Relocation<void (*)(RE::AIProcess*)> clearGreet{
          Offsets::Voice::ClearGreetTopic
        };
        clearGreet(process);
        logger::info("[verb:npc-puppet] promote {:x}: a puppet line's item "
                     "cleared before the AI (in flight {}, handle {})",
                     actor.formID, high->greetingPlayer,
                     handleLive ? "stopped" : "none");
      }
    }
  }
  actor.EnableAI(true);
  actor.EvaluatePackage(false, false);
  // thuum docs/research/m3/decisions-after-playtest-34.md D23: a live actor
  // with no model is the playtest 34 picture (on the compass, detecting,
  // invisible); said here so a lab read places the order of the fate's
  // reload and the promote
  logger::info("[verb:npc-puppet] live {:x}: AI on, package evaluated, "
               "activation unblocked (3D loaded {})",
               actor.formID, actor.Is3DLoaded());
}
}

bool Puppets::Is(uint32_t formId)
{
  std::lock_guard l(g_puppets.m);
  return g_puppets.ids.count(formId) != 0;
}

void Puppets::Set(uint32_t formId, bool puppet)
{
  std::lock_guard l(g_puppets.m);
  if (puppet) {
    g_puppets.ids.insert(formId);
  } else {
    g_puppets.ids.erase(formId);
    g_puppets.locomotion.erase(formId);
  }
}

size_t Puppets::Count()
{
  std::lock_guard l(g_puppets.m);
  return g_puppets.ids.size();
}

uint64_t Puppets::NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

void Puppets::SetLocomotion(uint32_t formId, const Locomotion& sample)
{
  std::lock_guard l(g_puppets.m);
  Locomotion stamped = sample;
  stamped.stampMs = NowMs();
  g_puppets.locomotion[formId] = stamped;
}

bool Puppets::GetLocomotion(uint32_t formId, Locomotion& out)
{
  std::lock_guard l(g_puppets.m);
  const auto it = g_puppets.locomotion.find(formId);
  if (it == g_puppets.locomotion.end()) {
    return false;
  }
  out = it->second;
  return true;
}

void Puppets::SetHighActors(std::vector<uint32_t> ids)
{
  std::lock_guard l(g_puppets.m);
  g_puppets.highActors = std::move(ids);
}

std::vector<uint32_t> Puppets::GetHighActors()
{
  std::lock_guard l(g_puppets.m);
  return g_puppets.highActors;
}

Napi::Value PuppetApi::SetPuppet(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto puppet = NapiHelper::ExtractBoolean(info[1], "puppet");
  if (actorFormId == 0 || actorFormId == 0x14) {
    return Napi::Boolean::New(info.Env(), false);
  }
  // the flag first: the detours read it from this frame on. The actor is
  // looked up on the game thread alone (performAction's shape): a lookup
  // here runs on the JS thread under the forms map's lock while the game
  // thread waits for this tick, and a cell load holding that lock would
  // hold both (the gate's hang at a landing, 2026-10-21, under reading)
  Puppets::Set(actorFormId, puppet);
  g_nativeCallRequirements.gameThrQ->AddTask(
    [actorFormId, puppet](Viet::Void) {
      auto* pActor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
      if (!pActor) {
        logger::warn("[verb:npc-puppet] setPuppet({:x}, {}): no such actor",
                     actorFormId, puppet);
        Puppets::Set(actorFormId, false);
        return;
      }
      if (puppet) {
        Demote(*pActor);
      } else {
        Promote(*pActor);
      }
    });
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value PuppetApi::ReleasePuppet(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  if (actorFormId == 0 || actorFormId == 0x14) {
    return Napi::Boolean::New(info.Env(), false);
  }
  // the flag off first: the detours stop skipping its step from this frame
  Puppets::Set(actorFormId, false);
  g_nativeCallRequirements.gameThrQ->AddTask([actorFormId](Viet::Void) {
    auto* pActor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
    if (!pActor) {
      logger::warn("[verb:npc-puppet] releasePuppet({:x}): no such actor",
                   actorFormId);
      return;
    }
    // thuum docs/verbs/npc-handoff.md D11: an unloaded puppet is simulated
    // by nobody, so its AI bit comes back on (the engine's own schedule
    // moves it; Actor::EnableAI writes the bit without a controller,
    // src/RE/A/Actor.cpp:230-240) and its block comes off; no package
    // evaluation on an actor with no process
    pActor->SetActivationBlocked(false);
    pActor->EnableAI(true);
    logger::info("[verb:npc-puppet] released {:x}: AI on, flag and block "
                 "off, left to its schedule (3D loaded {})",
                 actorFormId, pActor->Is3DLoaded());
  });
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value PuppetApi::IsPuppet(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  return Napi::Boolean::New(info.Env(), Puppets::Is(actorFormId));
}

// getHighProcessActors(): the form ids of the engine's high-process actors
// as of the last "Actor animation" stage (Puppets::GetHighActors), read on
// the JS thread with no game call; the roster's source (skymp5-client
// HostService) and the demote poll's (PuppetService)
Napi::Value PuppetApi::GetHighProcessActors(const Napi::CallbackInfo& info)
{
  const auto ids = Puppets::GetHighActors();
  auto res = Napi::Array::New(info.Env(), ids.size());
  for (size_t i = 0; i < ids.size(); ++i) {
    res.Set(static_cast<uint32_t>(i),
            Napi::Number::New(info.Env(), static_cast<double>(ids[i])));
  }
  return res;
}

// setPuppetLocomotion(actorFormId, speed, directionDeg, turnRateDeg): the
// relayed sample a puppet's graph is fed from, stored under the registry's
// lock on the JS thread (no game call); false when the actor is not a
// puppet (the sample is kept anyway for the next demote)
Napi::Value PuppetApi::SetPuppetLocomotion(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  Puppets::Locomotion sample;
  sample.speed = NapiHelper::ExtractFloat(info[1], "speed");
  sample.directionDeg = NapiHelper::ExtractFloat(info[2], "directionDeg");
  sample.turnRateDeg = NapiHelper::ExtractFloat(info[3], "turnRateDeg");
  Puppets::SetLocomotion(actorFormId, sample);
  return Napi::Boolean::New(info.Env(), Puppets::Is(actorFormId));
}

// thuum docs/verbs/npc-stream.md (ADR-050): the hosted set the action hook
// reads (Hosted.h), fed by skymp5-client's HostService at every flip of the
// server's word per actor; the JS thread alone, no game call. The count is
// logged for the lab ("hosted set n" in the client's log at a grant)
Napi::Value PuppetApi::SetHosted(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto hosted = NapiHelper::ExtractBoolean(info[1], "hosted");
  if (actorFormId == 0 || actorFormId == 0x14) {
    return Napi::Boolean::New(info.Env(), false);
  }
  Hosted::Set(actorFormId, hosted);
  logger::info("[verb:npc-stream] setHosted({:x}, {}): hosted set {}",
               actorFormId, hosted, Hosted::Count());
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value PuppetApi::IsHosted(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  return Napi::Boolean::New(info.Env(), Hosted::Is(actorFormId));
}

// isPerformingAction(actorFormId): the hook's mark, on from the action
// entry's enter to its leave (FridaHooks.cpp OnPerformActionEnter)
Napi::Value PuppetApi::IsPerformingAction(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  return Napi::Boolean::New(info.Env(), Hosted::IsPerforming(actorFormId));
}

Napi::Value PuppetApi::SayTopicInfo(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto infoFormId =
    NapiHelper::ExtractUInt32(info[1], "topicInfoFormId");
  if (actorFormId == 0 || actorFormId == 0x14 || infoFormId == 0) {
    return Napi::Boolean::New(info.Env(), false);
  }
  {
    std::lock_guard<std::mutex> lock(g_sayResultMutex);
    g_sayResult[actorFormId] = -1;
  }
  // the forms looked up on the game thread alone (setPuppet's reason)
  g_nativeCallRequirements.gameThrQ->AddTask([actorFormId,
                                              infoFormId](Viet::Void) {
    int result = 0;
    auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
    auto* topicInfo = RE::TESForm::LookupByID<RE::TESTopicInfo>(infoFormId);
    if (actor && topicInfo && topicInfo->parentTopic) {
      auto& runtime = actor->GetActorRuntimeData();
      if (runtime.currentProcess) {
        const auto bitsBefore = runtime.boolBits.underlying();
        const float timerBefore = runtime.voiceTimer;
        const int stateBefore = runtime.soundCallBackSet;
        REL::Relocation<SayTopicFn> say{ Offsets::Voice::SayTopic };
        // Say's shape with the info given: the engine's own books, lip
        // sync, talking idle and begin event; the end is the detour's
        // (PuppetHooks.cpp DriveVoice) on a puppet
        // the lip entry's load state before the call (voice v3; the
        // re-analyst's read of 2026-10-26: HighProcessData's +0x3B8 is the
        // current line's lip-sync resource entry, its +0xC word carrying
        // the load state in bits 28 to 30: 1 or 2 queued or pending, 3 or
        // 4 loaded, 0 or 5 absent or failed)
        const auto lipState = [](RE::HighProcessData* high) -> int {
          if (!high || !high->unk3B8) {
            return -1;
          }
          const auto word = *reinterpret_cast<const uint32_t*>(
            reinterpret_cast<const uint8_t*>(high->unk3B8) + 0xC);
          return static_cast<int>((word >> 28) & 7u);
        };
        const int lipBefore = lipState(runtime.currentProcess->high);
        // voice v4: the audibility terms SpeakSoundFunction tests before
        // its lip branch (the re-analyst's read of P7 v3, 2026-10-26: a 3D
        // on this seat, and the speaker within the dialogue output model's
        // max distance of the listener on every axis unless talking to the
        // player), read here as the player's distance per axis and the 3D
        const bool has3D = actor->Get3D() != nullptr;
        float axisDistance = -1.f;
        if (auto* player = RE::PlayerCharacter::GetSingleton()) {
          const auto d = actor->GetPosition() - player->GetPosition();
          axisDistance =
            std::max({ std::fabs(d.x), std::fabs(d.y), std::fabs(d.z) });
        }
        // voice v3: the seventh flag FALSE selects the SYNCHRONOUS lip-file
        // load inside SpeakSoundFunction (16256), so the line plays in this
        // call with its lip sync. Say's own shape passes true, the
        // asynchronous load, after which the engine plays nothing and
        // marks the say in flight (greetingPlayer) for a replay on its next
        // AI frame (39632 under UpdateAI), which an AI-off puppet never
        // runs: P7 v1 20261026-181119, the humanoid resident's line
        // refused while every creature's bark (no lip file, a synchronous
        // miss) started. HYPOTHESIS until P7 v3
        // voice v4: keepExtra false, the shape of the engine's own bark and
        // player-dialogue callers (44673, 37541; only Papyrus Say passes
        // true): no ExtraSayToTopicInfo is attached for a callback nothing
        // on this seat consumes
        // train 57: a relayed repeat of the item in progress is left to the
        // puppet's own continuation (PuppetHooks.cpp DriveVoice): a bark
        // raises a BEGIN per line on the host and the relay carries each, so
        // a say here would restart the item at its first line
        auto* highBefore = runtime.currentProcess->high;
        const bool repeat = highBefore && highBefore->greetingPlayer &&
          highBefore->greetTopic && highBefore->greetTopic->info == topicInfo;
        // thuum playtest 37 (2026-10-28, Faendal and Alvor): a line asked
        // while the last one still plays with no state byte (the sound
        // descriptor path: timer running, state -1) takes that path too and
        // plays with no lips (4540c, bccc6, 105384, each "played"); the host's
        // own engine cuts a line for the next, so the one in flight is stopped
        // and its item cleared here first, and this line starts through
        // SpeakSoundFunction's lip branch. HYPOTHESIS until the probe.
        // 2026-10-28 21:56Z (Eli: "on rotfern the rumors line doesnt make it
        // to prisoner"): the rumor ca6da asked 1.7 s after the greeting 45447,
        // a lip-path line still in flight (state 3), took the sound path too
        // ("played", quiet 3D, no lips), since the pre-clear skipped state 3.
        // The host's engine cuts any line for the next, so does this seat
        bool precleared = false;
        if (!repeat && highBefore && timerBefore > 0.f) {
          if (highBefore->soundHandles[0].IsValid()) {
            highBefore->soundHandles[0].Stop();
          }
          REL::Relocation<void (*)(RE::AIProcess*)> clearGreetBefore{
            Offsets::Voice::ClearGreetTopic
          };
          clearGreetBefore(runtime.currentProcess);
          precleared = true;
        }
        const bool returned = repeat
          ? false
          : say(runtime.currentProcess, actor, topicInfo->parentTopic,
                topicInfo, false, false, false, false);
        const int stateAfter = runtime.soundCallBackSet;
        // voice v2 (P7 v1 20261026-181119: the say started on every
        // creature puppet and was refused on a humanoid resident's with
        // the same pre-state): the facts the engine's answer leaves
        // behind, so the lab reads WHICH path refused or played. The state
        // byte 3 is the SpeakSoundFunction path's mark; a line through the
        // sound descriptor path (39771, the static read) sets no state
        // byte but leaves a live handle or a running timer: "played"
        auto* high = runtime.currentProcess->high;
        const bool handleLive = high && high->soundHandles[0].IsValid();
        const float timerAfter = runtime.voiceTimer;
        const bool inFlight = high && high->greetingPlayer;
        const int lipAfter = lipState(high);
        bool cleared = false;
        if (stateAfter == 3) {
          result = 1;
        } else if (handleLive || timerAfter > 0.f) {
          result = 3;
        } else if (inFlight) {
          // voice v4: the start left the item in progress with nothing
          // playing and no lip entry: SpeakSoundFunction's audibility gate
          // (no 3D here, or the speaker outside the dialogue output model's
          // range of this seat's listener) returned before the lip branch
          // (P7 v3 200538, the resident 13482 "deferred": lip -1 before and
          // after, the re-analyst's read). The engine's verdict for this
          // seat is accepted and the item cleared the way its END paths
          // clear it, so the mark never holds a later promote's greetings
          result = 5;
          if (high) {
            REL::Relocation<void (*)(RE::AIProcess*)> clearGreet{
              Offsets::Voice::ClearGreetTopic
            };
            clearGreet(runtime.currentProcess);
            cleared = true;
          }
        } else {
          result = 2;
        }
        if (repeat) {
          result = 3; // played: the item is in progress here, its later
                      // lines the continuation's
          logger::info("[verb:npc-stream] sayTopicInfo on {:x}: info {:x} is "
                       "the item in progress, left to its continuation",
                       actorFormId, infoFormId);
        }
        logger::info("[verb:npc-stream] sayTopicInfo on {:x}: info {:x} "
                     "topic {:x} -> {} (bits {:x}, timer {:.2f}, state {}, "
                     "lip {}, 3D {}, axis distance {:.0f} before; returned "
                     "{}, state {}, timer {:.2f}, handle {}, delay {:.2f}, in "
                     "flight {}, lip {} after{}{})",
                     actorFormId, infoFormId, topicInfo->parentTopic->formID,
                     result == 1     ? "started"
                       : result == 3 ? "played"
                       : result == 5 ? "inaudible"
                       : result == 4 ? "deferred"
                                     : "refused",
                     bitsBefore, timerBefore, stateBefore, lipBefore, has3D,
                     axisDistance, returned, stateAfter, timerAfter,
                     handleLive ? "live" : "none",
                     high ? high->soundDelay : -2.f, inFlight, lipAfter,
                     cleared ? ", item cleared" : "",
                     precleared ? "; the line in flight stopped first" : "");
      }
    }
    std::lock_guard<std::mutex> lock(g_sayResultMutex);
    g_sayResult[actorFormId] = result;
  });
  return Napi::Boolean::New(info.Env(), true);
}

namespace {
struct HostedGazeRead
{
  uint32_t target = 0;
  uint32_t slot = 0;
  uint32_t emotionType = 0;
  uint32_t emotionValue = 0;
  // the face as FaceGen animates it (Actor::GetFaceGenAnimationData,
  // CommonLibSSE-NG Actor.h:309; BSFaceGenAnimationData.h:39 the expression
  // keyframe, :81 the override flag): the strongest expression's index and
  // value, and whether an override holds it (the lab's read of a puppet's
  // face, and the host's for comparison)
  uint32_t exprOverride = 0;
  uint32_t exprIndex = 0;
  float exprValue = 0.f;
};
std::mutex g_gazeMutex;
std::unordered_map<uint32_t, HostedGazeRead> g_gaze;

// thuum playtest 37 (Eli: "do the probe first before any more head builds"):
// the face as FaceGen animates it, every keyframe value, for the lab's read
// of a puppet's mouth against the live NPC's on the host
// (BSFaceGenAnimationData's expression, modifier and phoneme keyframes,
// CommonLibSSE-NG BSFaceGenAnimationData.h:39-42; each a
// BSFaceGenKeyframeMultiple, values and count at
// BSFaceGenKeyframeMultiple.h:41-42)
struct FaceRead
{
  std::vector<float> expression;
  std::vector<float> modifier;
  std::vector<float> phoneme;
  bool exprOverride = false;
  // every keyframe set the header lays out, 0x20 to 0x180 (CommonLibSSE-NG
  // BSFaceGenAnimationData.h:39-50, twelve BSFaceGenKeyframeMultiple), in
  // that order: playtest 37's control (Eli, 2026-10-28: "lips move on the
  // initiater") read zero phonemes on a live NPC whose mouth moved, so the
  // lip sync writes a set the three named ones do not cover
  std::vector<std::vector<float>> sets;
};
std::mutex g_faceMutex;
std::unordered_map<uint32_t, FaceRead> g_face;
}

namespace {
// thuum docs/research/m3/re-claimed-vendor-topic.md (the re-analyst,
// 2026-10-28; playtest 37's finding 40: a claimant's promoted innkeeper
// offered no "What have you got for sale?"): the vendor topics' condition
// GetOffersServicesNow reads the actor's cached vendor faction
// (Actor::CanOfferServices over GetVendorFaction, CommonLib Actor.cpp:136),
// which the engine recomputes lazily and at most every 20 s of AI time, so
// a dialogue opened right after the claim's promote finds the cache empty
// (a run within the last 20 s kept nothing for an AI-off puppet) and builds
// its topic list without the services. Before the activation the cache is
// reset with the engine's own sentinel (the stamp at -FLT_MAX, Actor.h:671)
// and asked again; a vendor whose faction still does not resolve waits a
// frame, at most this many, then the dialogue opens anyway and the inputs
// the engine judged (the cell, the hour) are logged. HYPOTHESIS H-58b until
// x-npc-vendor-claim reads the topic on the claimant
constexpr int kVendorFrames = 30;

// the actor belongs to some faction flagged as a vendor
bool IsVendorActor(RE::Actor& actor)
{
  bool vendor = false;
  actor.VisitFactions([&vendor](RE::TESFaction* faction, std::int8_t) {
    if (faction && faction->IsVendor()) {
      vendor = true;
      return true;
    }
    return false;
  });
  return vendor;
}

void ActivatePickRefTask(int vendorFramesLeft)
{
  auto* player = RE::PlayerCharacter::GetSingleton();
  if (!player) {
    return;
  }
  // thuum docs/research/m3/re-claimed-dialogue-state.md (the re-analyst,
  // 2026-10-28; playtest 37's finding 19: a claimant's promoted partner
  // reads not-in-dialogue while the menu is open): a SECOND
  // TESNPC::Activate on an actor whose dialogue already runs clears
  // HighProcessData::talkingToPC (its SetTalkingToPC(1)/(0) pair before
  // the speaker test), and with the byte off the engine steps the
  // package, drops the look at the player and times the voice as for a
  // stranger: the wiggle, the missing head turn. One activation per
  // claim: nothing runs while the Dialogue Menu is open, while the
  // MenuTopicManager's speaker is the pick, or while the pick already
  // talks to the player (HighProcessData.h:341). H-55e CONFIRMED live
  // (playtest 37, 00:21Z).
  auto* ui = RE::UI::GetSingleton();
  if (ui && ui->IsMenuOpen(RE::DialogueMenu::MENU_NAME)) {
    logger::info("[verb:npc-talk] activatePickRef skipped: the Dialogue "
                 "Menu is open");
    return;
  }
  RE::NiPointer<RE::TESObjectREFR> pick;
  if (auto* pickData = RE::CrosshairPickData::GetSingleton()) {
    pick = pickData->target.get();
  }
  RE::Actor* actor = nullptr;
  if (pick) {
    if (auto* mtm = RE::MenuTopicManager::GetSingleton()) {
      auto speaker = mtm->speaker.get();
      if (speaker && speaker.get() == pick.get()) {
        logger::info("[verb:npc-talk] activatePickRef skipped: {:x} is "
                     "the dialogue's speaker already",
                     pick->formID);
        return;
      }
    }
    actor = pick->As<RE::Actor>();
    if (actor) {
      auto* process = actor->GetActorRuntimeData().currentProcess;
      auto* high = process ? process->high : nullptr;
      if (high && high->talkingToPC) {
        logger::info("[verb:npc-talk] activatePickRef skipped: {:x} "
                     "talks to the player already",
                     pick->formID);
        return;
      }
    }
  }
  if (actor) {
    // the engine's sentinel every frame of the wait: a run re-arms the 20 s
    // throttle, so each GetVendorFaction must find the stamp reset
    auto& rt = actor->GetActorRuntimeData();
    rt.calculateVendorFactionTimer.timeStamp = -FLT_MAX;
    rt.vendorFaction = nullptr;
    // the recompute, then the cache read AFTER it: the pin's GetVendorFaction
    // returns the value it read before recomputing (Actor.cpp:610, the local
    // taken first), so the first live run read null for all 30 frames while
    // the recompute filled the cache each frame and the menu found the shop
    // topic anyway (playtest 37, 03:40Z, "shop worked!")
    actor->GetVendorFaction();
    if (IsVendorActor(*actor) && rt.vendorFaction == nullptr) {
      auto* cell = actor->GetParentCell();
      auto* calendar = RE::Calendar::GetSingleton();
      if (vendorFramesLeft > 0) {
        if (vendorFramesLeft == kVendorFrames || vendorFramesLeft % 10 == 0) {
          logger::info(
            "[verb:npc-talk] activatePickRef waits: {:x} is a vendor with "
            "no vendor faction yet (cell {:x}, hour {:.2f}); {} frame(s) "
            "left",
            actor->formID, cell ? cell->formID : 0u,
            calendar ? calendar->GetHour() : -1.f, vendorFramesLeft);
        }
        g_nativeCallRequirements.gameThrQ->AddTask(
          [vendorFramesLeft](Viet::Void) {
            ActivatePickRefTask(vendorFramesLeft - 1);
          });
        return;
      }
      logger::warn("[verb:npc-talk] activatePickRef: {:x} is a vendor with "
                   "no vendor faction after {} frames (cell {:x}, hour "
                   "{:.2f}); the dialogue opens anyway (H-58b)",
                   actor->formID, kVendorFrames, cell ? cell->formID : 0u,
                   calendar ? calendar->GetHour() : -1.f);
    } else if (vendorFramesLeft < kVendorFrames) {
      logger::info("[verb:npc-talk] activatePickRef: {:x}'s vendor faction "
                   "resolved after {} frame(s)",
                   actor->formID, kVendorFrames - vendorFramesLeft);
    }
  }
  player->ActivatePickRef();
  logger::info("[verb:npc-talk] activatePickRef ran (pick {:x})",
               pick ? pick->formID : 0u);
}
} // namespace

Napi::Value PuppetApi::ActivatePickRef(const Napi::CallbackInfo& info)
{
  g_nativeCallRequirements.gameThrQ->AddTask(
    [](Viet::Void) { ActivatePickRefTask(kVendorFrames); });
  return Napi::Boolean::New(info.Env(), true);
}

// the same read: the engine's own release of the E that sent a claim lands
// after the promote lifted the block and activates the actor a second time;
// consumeActivatePress marks the press's held action done on the engine's
// ActivateHandler (ActivateHandler.h:18 SetHeldButtonActionSuccess), so its
// release activates nothing. HYPOTHESIS until the read: that the handler
// skips the tap on a successful held action.
Napi::Value PuppetApi::ConsumeActivatePress(const Napi::CallbackInfo& info)
{
  g_nativeCallRequirements.gameThrQ->AddTask([](Viet::Void) {
    auto* controls = RE::PlayerControls::GetSingleton();
    auto* handler = controls ? controls->GetActivateHandler() : nullptr;
    if (!handler) {
      return;
    }
    handler->SetHeldButtonActionSuccess(true);
    logger::info("[verb:npc-talk] consumeActivatePress: the engine's "
                 "release of this press activates nothing");
  });
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value PuppetApi::PollHostedGaze(const Napi::CallbackInfo& info)
{
  const auto ids = NapiHelper::ExtractArray(info[0], "actorFormIds");
  std::vector<uint32_t> actorIds;
  const uint32_t n = ids.Length();
  actorIds.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const std::string name = "actorFormIds[" + std::to_string(i) + "]";
    actorIds.push_back(NapiHelper::ExtractUInt32(ids.Get(i), name.c_str()));
  }
  // the engine's fields read on the game thread (setPuppet's reason); a few
  // field reads per hosted actor per poll (rule 9)
  g_nativeCallRequirements.gameThrQ->AddTask([actorIds](Viet::Void) {
    std::unordered_map<uint32_t, HostedGazeRead> reads;
    for (const auto id : actorIds) {
      auto* actor = RE::TESForm::LookupByID<RE::Actor>(id);
      if (!actor) {
        continue;
      }
      HostedGazeRead read;
      auto& runtime = actor->GetActorRuntimeData();
      auto* high =
        runtime.currentProcess ? runtime.currentProcess->high : nullptr;
      if (high) {
        // the highest set slot wins (HighProcessData.h:80-94: default,
        // action, script, combat, dialogue, procedure)
        for (int slot = RE::HighProcessData::HEAD_TRACK_TYPE::kTotal - 1;
             slot >= 0; --slot) {
          if (!high->headTracked[slot]) {
            continue;
          }
          auto target = high->headTrackTarget[slot].get();
          if (target) {
            read.target = target->formID;
            read.slot = static_cast<uint32_t>(slot);
            break;
          }
        }
      }
      read.emotionType = static_cast<uint32_t>(actor->GetEmotionType());
      read.emotionValue = std::min<uint32_t>(actor->GetEmotionValue(), 100u);
      if (auto* face = actor->GetFaceGenAnimationData()) {
        read.exprOverride = face->exprOverride ? 1u : 0u;
        const auto& frame = face->expressionKeyFrame;
        if (frame.values) {
          for (uint32_t i = 0; i < frame.count; ++i) {
            if (frame.values[i] > read.exprValue) {
              read.exprValue = frame.values[i];
              read.exprIndex = i;
            }
          }
        }
      }
      reads[id] = read;
    }
    std::lock_guard<std::mutex> lock(g_gazeMutex);
    g_gaze.swap(reads);
  });
  return info.Env().Undefined();
}

Napi::Value PuppetApi::HostedGaze(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  HostedGazeRead read;
  {
    std::lock_guard<std::mutex> lock(g_gazeMutex);
    const auto it = g_gaze.find(actorFormId);
    if (it == g_gaze.end()) {
      return info.Env().Null();
    }
    read = it->second;
  }
  auto res = Napi::Array::New(info.Env(), 7);
  res.Set(uint32_t(0), Napi::Number::New(info.Env(), read.target));
  res.Set(uint32_t(1), Napi::Number::New(info.Env(), read.slot));
  res.Set(uint32_t(2), Napi::Number::New(info.Env(), read.emotionType));
  res.Set(uint32_t(3), Napi::Number::New(info.Env(), read.emotionValue));
  res.Set(uint32_t(4), Napi::Number::New(info.Env(), read.exprOverride));
  res.Set(uint32_t(5), Napi::Number::New(info.Env(), read.exprIndex));
  res.Set(uint32_t(6), Napi::Number::New(info.Env(), read.exprValue));
  return res;
}

Napi::Value PuppetApi::PollFaceKeyframes(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  // the engine's fields read on the game thread (setPuppet's reason)
  g_nativeCallRequirements.gameThrQ->AddTask([actorFormId](Viet::Void) {
    auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
    if (!actor) {
      return;
    }
    FaceRead read;
    if (auto* face = actor->GetFaceGenAnimationData()) {
      read.exprOverride = face->exprOverride;
      const auto copy = [](const RE::BSFaceGenKeyframeMultiple& frame,
                           std::vector<float>& out) {
        if (!frame.values) {
          return;
        }
        out.assign(frame.values, frame.values + frame.count);
      };
      copy(face->expressionKeyFrame, read.expression);
      copy(face->modifierKeyFrame, read.modifier);
      copy(face->phenomeKeyFrame, read.phoneme);
      const RE::BSFaceGenKeyframeMultiple* all[] = { &face->expressionKeyFrame,
                                                     &face->unk040,
                                                     &face->modifierKeyFrame,
                                                     &face->phenomeKeyFrame,
                                                     &face->customKeyFrame,
                                                     &face->unk0C0,
                                                     &face->unk0E0,
                                                     &face->unk100,
                                                     &face->unk120,
                                                     &face->unk140,
                                                     &face->unk160,
                                                     &face->unk180 };
      for (const auto* frame : all) {
        std::vector<float> values;
        copy(*frame, values);
        read.sets.push_back(std::move(values));
      }
    }
    std::lock_guard<std::mutex> lock(g_faceMutex);
    g_face[actorFormId] = std::move(read);
  });
  return info.Env().Undefined();
}

Napi::Value PuppetApi::FaceKeyframes(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  FaceRead read;
  {
    std::lock_guard<std::mutex> lock(g_faceMutex);
    const auto it = g_face.find(actorFormId);
    if (it == g_face.end()) {
      return info.Env().Null();
    }
    read = it->second;
  }
  const auto toArray = [&](const std::vector<float>& v) {
    auto arr = Napi::Array::New(info.Env(), v.size());
    for (uint32_t i = 0; i < v.size(); ++i) {
      arr.Set(i, Napi::Number::New(info.Env(), v[i]));
    }
    return arr;
  };
  auto res = Napi::Array::New(info.Env(), 5);
  res.Set(uint32_t(0), toArray(read.expression));
  res.Set(uint32_t(1), toArray(read.modifier));
  res.Set(uint32_t(2), toArray(read.phoneme));
  res.Set(uint32_t(3), Napi::Boolean::New(info.Env(), read.exprOverride));
  auto sets = Napi::Array::New(info.Env(), read.sets.size());
  for (uint32_t i = 0; i < read.sets.size(); ++i) {
    sets.Set(i, toArray(read.sets[i]));
  }
  res.Set(uint32_t(4), sets);
  return res;
}

// thuum memo D52 B (playtest 37, 2026-10-28, Eli: "so both can open it but
// they dont see a shared live updating inventory / in morrowind they do";
// docs/research/m3/re-container-menu-refresh.md): refreshContainerMenu(refId)
// ends a batch of changes to a container whose ContainerMenu is open on this
// seat (ContainerMenu::GetTargetRefHandle): the engine's inventory-update
// message for that reference, posted twice from the game thread after the
// batch's tasks (Skyrim Souls' shape: the first redraws any rows the engine
// marked dirty, the second rebuilds both sides when none is). Each change
// addItemEx lands under the menu already posts one (ItemMenuRefresh); this
// is the batch's end for the paths that land otherwise (resetBase's
// RemoveAllItems). Never ItemList::Update: it blanks the menu's selection
// until a message is processed, playtest 37's "cannot place" (finding 30).
// HYPOTHESIS until the shared chest redraws in place on a seat
Napi::Value PuppetApi::RefreshContainerMenu(const Napi::CallbackInfo& info)
{
  const auto refId = NapiHelper::ExtractUInt32(info[0], "refId");
  g_nativeCallRequirements.gameThrQ->AddTask([refId](Viet::Void) {
    auto* ui = RE::UI::GetSingleton();
    auto menu = ui ? ui->GetMenu<RE::ContainerMenu>() : nullptr;
    if (!menu) {
      return;
    }
    const auto handle = RE::ContainerMenu::GetTargetRefHandle();
    RE::NiPointer<RE::TESObjectREFR> target;
    RE::TESObjectREFR::LookupByHandle(handle, target);
    if (!target || (refId != 0 && target->formID != refId)) {
      logger::info("[verb:containers] refreshContainerMenu {:x}: the open "
                   "menu is {:x}'s, nothing redrawn",
                   refId, target ? target->formID : 0u);
      return;
    }
    const auto first = ItemMenuRefresh::TryPost(*target);
    const auto second = first == ItemMenuRefresh::PostResult::kPosted
      ? ItemMenuRefresh::TryPost(*target)
      : first;
    if (second == ItemMenuRefresh::PostResult::kPosted) {
      logger::info("[verb:containers] refreshContainerMenu {:x}: the engine's "
                   "inventory update posted twice",
                   refId);
    } else {
      // the reason with both handles: a "not posted" with the menu open and
      // its target this reference has only the handle comparison or the
      // queue left to blame (docs/research/m3/re-corpse-menu-crash.md asks)
      logger::info("[verb:containers] refreshContainerMenu {:x}: the engine's "
                   "inventory update not posted: {} (the menu's target handle "
                   "{:x}, the reference's {:x})",
                   refId, ItemMenuRefresh::Name(second), handle,
                   target->CreateRefHandle().native_handle());
    }
  });
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value PuppetApi::DamageActor(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const float health = NapiHelper::ExtractFloat(info[1], "health");
  const auto attackerFormId =
    NapiHelper::ExtractUInt32(info[2], "attackerFormId");
  const bool killMove = NapiHelper::ExtractBoolean(info[3], "killMove");
  if (actorFormId == 0 || actorFormId == 0x14 || !std::isfinite(health)) {
    return Napi::Boolean::New(info.Env(), false);
  }
  // the forms looked up on the game thread alone (setPuppet's reason)
  g_nativeCallRequirements.gameThrQ->AddTask(
    [actorFormId, health, attackerFormId, killMove](Viet::Void) {
      auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
      if (!actor) {
        logger::warn("[verb:npc-fate] damageActor on {:x}: no such actor",
                     actorFormId);
        return;
      }
      auto* attacker = attackerFormId
        ? RE::TESForm::LookupByID<RE::Actor>(attackerFormId)
        : nullptr;
      auto* owner = actor->AsActorValueOwner();
      if (!owner) {
        return;
      }
      // the engine's current health against its maximum (the permanent value
      // plus the temporary modifier, CommonLibSSE-NG ActorValueOwner.h:15-16,
      // Actor.h:526), the resulting percentage the server sent, the delta
      const float current = owner->GetActorValue(RE::ActorValue::kHealth);
      const float maximum =
        owner->GetPermanentActorValue(RE::ActorValue::kHealth) +
        actor->GetActorValueModifier(RE::ACTOR_VALUE_MODIFIER::kTemporary,
                                     RE::ActorValue::kHealth);
      const float target =
        std::clamp(health, 0.f, 1.f) * std::max(maximum, 1.f);
      const float delta = current - target;
      const char* way = "nothing";
      float topped = 0.f;
      if (delta > 0.01f || (killMove && current > 0.f)) {
        Frida::DamageActorOriginal(actor, std::max(delta, 0.f), attacker,
                                   killMove);
        way = "damaged";
        // playtest 35 (2026-10-27, the campfire pair under rotfern's arrows):
        // the engine took a quarter of what it was handed (35 asked 15.3 and
        // moved 3.8; the difficulty's multiplier on a player's hit, the
        // attacker unmapped then), so the server's zero landed at a quarter
        // bar and the kill was the server's word three seconds later. The
        // value is read back and the remainder taken through the value owner
        // (Papyrus DamageActorValue's route, no multiplier): the server's
        // number is the engine's, the killer the first call's
        const float after = owner->GetActorValue(RE::ActorValue::kHealth);
        if (after - target > 0.5f) {
          topped = after - target;
          owner->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                   RE::ActorValue::kHealth, -topped);
        }
      } else if (delta < -0.01f) {
        owner->RestoreActorValue(RE::ACTOR_VALUE_MODIFIER::kDamage,
                                 RE::ActorValue::kHealth, -delta);
        way = "restored";
      }
      logger::info("[verb:npc-fate] cause on {:x}: health {:.3f} of {:.1f} "
                   "(engine {:.1f} -> {:.1f}, {}, topped up {:.1f}), attacker "
                   "{:x}, kill move {}",
                   actorFormId, health, maximum, current, target, way, topped,
                   attackerFormId, killMove);
    });
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value PuppetApi::SetEngineDamageCut(const Napi::CallbackInfo& info)
{
  const bool on = NapiHelper::ExtractBoolean(info[0], "on");
  Frida::SetEngineDamageCut(on);
  logger::info("[verb:npc-fate] the engine damage cut is {}",
               on ? "on" : "off");
  return info.Env().Undefined();
}

Napi::Value PuppetApi::SayTopicInfoResult(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  std::lock_guard<std::mutex> lock(g_sayResultMutex);
  const auto it = g_sayResult.find(actorFormId);
  return Napi::Number::New(info.Env(),
                           it == g_sayResult.end() ? -1 : it->second);
}
