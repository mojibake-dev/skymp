#include "PuppetApi.h"
#include "ActingScope.h"
#include "CallNativeApi.h"
#include "FridaHooks.h"
#include "Hosted.h"
#include "ItemMenuRefresh.h"
#include "PropBodies.h"
#include "Puppets.h"
#include "QuestCause.h"
#include "ScriptNatives.h"
#include "ScriptState.h"
#include "TrapFamily.h"
#include "game/Offsets.h"
#include <RE/A/ActivateHandler.h>
#include <RE/A/ActiveEffect.h>
#include <RE/B/BSXFlags.h>
#include <RE/C/Calendar.h>
#include <RE/C/ContainerMenu.h>
#include <RE/C/CrosshairPickData.h>
#include <RE/D/DialogueMenu.h>
#include <RE/E/EffectSetting.h>
#include <RE/E/ExtraTeleport.h>
#include <RE/I/InterfaceStrings.h>
#include <RE/M/MagicSystem.h>
#include <RE/M/MagicTarget.h>
#include <RE/M/Main.h>
#include <RE/M/MenuTopicManager.h>
#include <RE/P/PlayerControls.h>
#include <RE/T/TESFaction.h>
#include <RE/T/TESObjectCELL.h>
#include <RE/T/TESTopic.h>
#include <RE/T/TESTopicInfo.h>
#include <RE/T/TESWorldSpace.h>
#include <RE/U/UI.h>
#include <RE/U/UIMessage.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <fmt/format.h>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <xmmintrin.h>

extern CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

namespace {
struct
{
  std::unordered_set<uint32_t> ids;
  std::unordered_map<uint32_t, Puppets::Locomotion> locomotion;
  // tracker #111: each actor's locomotion as its movement step left it, the
  // engine's own for an actor this game simulates and the relay's feed for
  // a puppet (Puppets::SetStepLocomotion)
  std::unordered_map<uint32_t, Puppets::Locomotion> stepped;
  bool movementSampled = false;
  std::vector<uint32_t> highActors;
  // tracker #121: the package destination cells by actor (Puppets.h)
  std::unordered_map<uint32_t, std::vector<uint32_t>> packageCells;
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
  // engine's own revival on this seat: the console's or Papyrus's Resurrect
  // through the Character vtable hook (Hooks.cpp HostedResurrect) and a
  // necromancer's reanimation through the hook on its one function
  // (FridaHooks.cpp OnReanimateEnter, finding 54;
  // docs/research/m3/re-resurrect-hook.md)
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
        // and the done bits the cleared line's END would have restored, so
        // the live actor's own first say passes the busy test (37266)
        actor.GetActorRuntimeData().boolBits.set(
          RE::Actor::BOOL_BITS::kSoundFileDone,
          RE::Actor::BOOL_BITS::kVoiceFileDone);
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

void Puppets::SetStepLocomotion(uint32_t formId, const Locomotion& sample)
{
  std::lock_guard l(g_puppets.m);
  Locomotion stamped = sample;
  stamped.stampMs = NowMs();
  g_puppets.stepped[formId] = stamped;
}

bool Puppets::GetStepLocomotion(uint32_t formId, Locomotion& out)
{
  std::lock_guard l(g_puppets.m);
  const auto it = g_puppets.stepped.find(formId);
  if (it == g_puppets.stepped.end()) {
    return false;
  }
  out = it->second;
  return true;
}

void Puppets::SetMovementSampled(bool installed)
{
  std::lock_guard l(g_puppets.m);
  g_puppets.movementSampled = installed;
}

bool Puppets::MovementSampled()
{
  std::lock_guard l(g_puppets.m);
  return g_puppets.movementSampled;
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

void Puppets::SetPackageCells(
  std::unordered_map<uint32_t, std::vector<uint32_t>> cells)
{
  std::lock_guard l(g_puppets.m);
  g_puppets.packageCells = std::move(cells);
}

std::vector<uint32_t> Puppets::GetPackageCells(uint32_t formId)
{
  std::lock_guard l(g_puppets.m);
  const auto it = g_puppets.packageCells.find(formId);
  return it == g_puppets.packageCells.end() ? std::vector<uint32_t>{}
                                            : it->second;
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

// getPackageDestinationCells(actorFormId): the cells the actor's running
// package names as its location, as of the last "Actor animation" stage
// (Puppets::GetPackageCells), read on the JS thread with no game call; empty
// for an actor out of high process or a package that names none (tracker
// #121, skymp5-client PuppetService's demote)
Napi::Value PuppetApi::GetPackageDestinationCells(
  const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto cells = Puppets::GetPackageCells(actorFormId);
  auto res = Napi::Array::New(info.Env(), cells.size());
  for (size_t i = 0; i < cells.size(); ++i) {
    res.Set(static_cast<uint32_t>(i),
            Napi::Number::New(info.Env(), static_cast<double>(cells[i])));
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

// getActorLocomotion(actorFormId): tracker #111, the locomotion fields as the
// actor's last movement step left them (Puppets::GetStepLocomotion): an actor
// this game simulates, its engine's own; a puppet, the relay's feed as the
// detour wrote it. In setPuppetLocomotion's units, with ageMs since the
// sample; read on the JS thread from the registry, no game call. null when
// the detour is not installed or the actor was never sampled (neither hosted
// nor a puppet here, or no step since it became one)
Napi::Value PuppetApi::GetActorLocomotion(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  Puppets::Locomotion sample;
  if (!Puppets::MovementSampled() ||
      !Puppets::GetStepLocomotion(actorFormId, sample)) {
    return info.Env().Null();
  }
  const uint64_t now = Puppets::NowMs();
  const uint64_t age = now > sample.stampMs ? now - sample.stampMs : 0;
  auto res = Napi::Object::New(info.Env());
  res.Set("speed", Napi::Number::New(info.Env(), sample.speed));
  res.Set("directionDeg", Napi::Number::New(info.Env(), sample.directionDeg));
  res.Set("turnRateDeg", Napi::Number::New(info.Env(), sample.turnRateDeg));
  res.Set("ageMs", Napi::Number::New(info.Env(), static_cast<double>(age)));
  return res;
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

// thuum docs/DECISIONS.md ADR-068 rung 5 (the acting scope, ActingScope.h;
// docs/research/rung5-quests-2026-11-09.md 1(c), read 12: an export, not a
// TESModPlatform native, so the message handler can write it): the acting
// player of a request the server relayed, written before skymp5-client
// applies it; setHosted's shape, the JS thread alone, no game call. A stage
// (0 to 65535) is a request the quest stage sink takes at the pair's event,
// arming the quest's scope; a start (-1) or a stop (-2) is requested and
// arms the quest's scope at once, for ActingScope::kArmMs; an entry (-3) is
// a relayed trigger entry's, questId the trigger, taken by
// TESModPlatform.SendTriggerEvent's arm for its server id; an activation
// (-4) a relayed press's, questId the pressed reference, taken by
// TESModPlatform.ArmActivation's arm (tracker #117). figureLocalId is
// the figure this seat holds for the acting player, 0 when it is absent here
// (Game.GetPlayer answers None in the scope then); serverId its actor id as
// the server names it; ttlMs the request's life. False, nothing stored, for
// a quest 0, a stage outside -4 to 65535, a serverId 0, the player's own id
// as the figure, or a ttl outside 1 to ActingScope::kMaxRequestMs. Logged
// for the lab. Since tracker #117 (Eli's design B) an activation's request
// may carry a sixth argument, items: the acting player's inventory as the
// server records it (RelayedActivation's presserInventory), a flat array of
// base id and count pairs, at most kMaxItemStacks of them, each base and
// count above 0; the scope answers an item count on the figure from it. False,
// nothing stored, for items on any other stage or items of another shape
Napi::Value PuppetApi::SetActingPlayer(const Napi::CallbackInfo& info)
{
  const auto questId = NapiHelper::ExtractUInt32(info[0], "questId");
  const auto stage = NapiHelper::ExtractInt32(info[1], "stage");
  const auto figure = NapiHelper::ExtractUInt32(info[2], "figureLocalId");
  const auto serverId = NapiHelper::ExtractUInt32(info[3], "serverId");
  const auto ttlMs = NapiHelper::ExtractUInt32(info[4], "ttlMs");
  if (questId == 0 || stage < ActingScope::kActivation || stage > 0xFFFF ||
      serverId == 0 || figure == 0x14 || ttlMs == 0 ||
      ttlMs > ActingScope::kMaxRequestMs) {
    return Napi::Boolean::New(info.Env(), false);
  }
  ActingScope::Player player;
  player.figure = figure;
  player.serverId = serverId;
  // the press's record (#117): the wire's own cap on an inventory's stacks
  // (wire-schema cap::INVENTORY), the counts of one base summed as the
  // record's GetItemCount sums them
  constexpr uint32_t kMaxItemStacks = 4096;
  const bool hasItems =
    info.Length() > 5 && !info[5].IsUndefined() && !info[5].IsNull();
  if (hasItems) {
    if (stage != ActingScope::kActivation || !info[5].IsArray()) {
      return Napi::Boolean::New(info.Env(), false);
    }
    const auto pairs = info[5].As<Napi::Array>();
    const uint32_t n = pairs.Length();
    if (n % 2 != 0 || n / 2 > kMaxItemStacks) {
      return Napi::Boolean::New(info.Env(), false);
    }
    auto items = std::make_shared<std::unordered_map<uint32_t, uint32_t>>();
    for (uint32_t i = 0; i < n; i += 2) {
      const Napi::Value base = pairs.Get(i);
      const Napi::Value count = pairs.Get(i + 1);
      if (!base.IsNumber() || !count.IsNumber()) {
        return Napi::Boolean::New(info.Env(), false);
      }
      const uint32_t baseId = base.As<Napi::Number>().Uint32Value();
      const uint32_t stack = count.As<Napi::Number>().Uint32Value();
      if (baseId == 0 || stack == 0) {
        return Napi::Boolean::New(info.Env(), false);
      }
      uint32_t& held = (*items)[baseId];
      held = stack > UINT32_MAX - held ? UINT32_MAX : held + stack;
    }
    player.items = std::move(items);
  }
  // the scope's root (V-scene): a relayed stage or word is kRelayed, this
  // seat not its cause seat; an entry's arm is SendTriggerEvent's and an
  // activation's ArmActivation's, kTrigger (this seat runs the press for
  // its presser and is the cause seat of what its script sets)
  const bool pressed =
    stage == ActingScope::kEntry || stage == ActingScope::kActivation;
  ActingScope::Request(questId, stage, player, ttlMs,
                       ActingScope::Origin::kRelayed,
                       pressed ? ActingScope::RootKind::kTrigger
                               : ActingScope::RootKind::kRelayed);
  const bool word =
    stage == ActingScope::kStart || stage == ActingScope::kStop;
  if (word) {
    ActingScope::Arm(questId, player, ActingScope::kArmMs,
                     ActingScope::RootKind::kRelayed);
  }
  const ActingScope::Counts counts = ActingScope::Count();
  logger::info(
    "[verb:quests] setActingPlayer({:x}, {}, {:x}, {:x}, {}): {}{}; "
    "{} request(s), {} arm(s), {} joined stack(s)",
    questId, stage, figure, serverId, ttlMs,
    word ? "requested and armed"
         : (figure == 0 ? "requested, the figure absent here" : "requested"),
    player.items
      ? fmt::format(", the record's {} item(s) carried", player.items->size())
      : std::string(),
    counts.requests, counts.arms, counts.joined);
  return Napi::Boolean::New(info.Env(), true);
}

// train 53's V-scene (ADR-073 D-A2 C: the stage's cause seat owns a scene a
// fragment starts): every stage, start or stop this seat applies from a
// relay whose cause seat is elsewhere is scoped kRelayed, whoever its acting
// player is. setActingPlayer covers another player (the swap); this covers
// this seat's own player and an unnamed one, which setActingPlayer refuses
// (a serverId 0, the player's own id as the figure): the pair requested, and
// a start or a stop armed at once, with no acting player (ActingScope::
// Swaps false), so the fragment's frames and Game.GetPlayer stay this
// seat's, PQ-3 skips nothing, a scene start it makes is skipped (the cause
// seat starts it and claims its actors, ADR-073 step 5a's scene claim) and
// the stages it chains carry the root on, their reports
// naming no acting player as before (getActingPlayer answers 0). False,
// nothing stored, for a quest 0, a stage outside -2 to 65535 (an entry is a
// trigger's, setActingPlayer's -3), or a ttl outside 1 to
// ActingScope::kMaxRequestMs. Logged for the lab
Napi::Value PuppetApi::SetRelayedScope(const Napi::CallbackInfo& info)
{
  const auto questId = NapiHelper::ExtractUInt32(info[0], "questId");
  const auto stage = NapiHelper::ExtractInt32(info[1], "stage");
  const auto ttlMs = NapiHelper::ExtractUInt32(info[2], "ttlMs");
  if (questId == 0 || stage < ActingScope::kStop || stage > 0xFFFF ||
      ttlMs == 0 || ttlMs > ActingScope::kMaxRequestMs) {
    return Napi::Boolean::New(info.Env(), false);
  }
  const ActingScope::Player none{};
  ActingScope::Request(questId, stage, none, ttlMs,
                       ActingScope::Origin::kRelayed,
                       ActingScope::RootKind::kRelayed);
  const bool word =
    stage == ActingScope::kStart || stage == ActingScope::kStop;
  if (word) {
    ActingScope::Arm(questId, none, ActingScope::kArmMs,
                     ActingScope::RootKind::kRelayed);
  }
  const ActingScope::Counts counts = ActingScope::Count();
  logger::info("[verb:quests] setRelayedScope({:x}, {}, {}): {}, rooted at "
               "the relay with no acting player swapped; {} request(s), {} "
               "arm(s), {} joined stack(s)",
               questId, stage, ttlMs,
               word ? "requested and armed" : "requested", counts.requests,
               counts.arms, counts.joined);
  return Napi::Boolean::New(info.Env(), true);
}

// getActingPlayer(questId, stage): the server id of the acting player whose
// scope requested the pair on this seat, a relayed request or a stage, a
// start or a stop a scoped stack set (the seam's propagation), until the
// request expires, whether its event took it or not; 0 for none. The C2S
// QuestStage's `acting` for a stage this seat set inside a scope (read 7).
// A table read on the JS thread, no game call
Napi::Value PuppetApi::GetActingPlayer(const Napi::CallbackInfo& info)
{
  const auto questId = NapiHelper::ExtractUInt32(info[0], "questId");
  const auto stage = NapiHelper::ExtractInt32(info[1], "stage");
  return Napi::Number::New(info.Env(),
                           ActingScope::ServerIdOf(questId, stage));
}

// thuum docs/DECISIONS.md ADR-068 rung 5, D-Q4 C (QuestCause.h): a cause
// this seat's own act names for a pair before the engine makes it. Its one
// caller is skymp5-client's console service: Skyrim Platform runs a replaced
// command's JS execute before the engine's own handler, which runs only when
// execute answers true (ConsoleApi.cpp ConsoleComand_Execute), so a quest
// command the player typed is marked the world's before the engine starts,
// stops or stages the quest, and no script frame is on that path for the
// seam to read (ScriptNatives.cpp MarkQuestCause reads Papyrus rows). The
// word's event takes the mark once (EventHandler.cpp), the stage's event
// reads it, and the quest stage sink arms the stage's fragment with it, so
// what the fragment sets carries the world's cause too. ttlMs is the mark's
// life (the engine queues a typed start or stop to the story teller's next
// pass, so its event comes after the handler: a static read of the five
// commands' handlers, HYPOTHESIS).
// Since train 52 (Fix 3, the console arm's home) the rule of which word a
// request makes is the seam's own, QuestCause::WordMade, read here against
// the engine's flags: the quest looked up (TESForm::LookupByID<TESQuest>,
// under the form table's own lock) and its run and Start Game Enabled flag
// read (TESQuest::IsEnabled and StartsEnabled, CommonLibSSE-NG
// src/RE/T/TESQuest.cpp:42-45 and :112-115), on the JS thread as
// getTrapRole reads its reference (above), while the console's own thread
// waits for this execute in PushAndWait (ConsoleApi.cpp
// ConsoleComand_Execute), so the flags read are the ones the engine's
// handler reads next (HYPOTHESIS H-QR1: a VM thread's own Quest.Start in
// that window races, as the seam's own read races). No Papyrus call. A stage
// marks itself; a start (-1), a stop (-2) or a typed reset (-3,
// QuestCause::kReset) marks the word it makes, or nothing. The answer says
// what it did: { marked, running, startsEnabled }, marked the word marked
// (a stage, -1 or -2) or null when the request makes none; null, nothing
// stored, for a refused shape or a quest id no TESQuest has
Napi::Value PuppetApi::MarkQuestCause(const Napi::CallbackInfo& info)
{
  const auto questId = NapiHelper::ExtractUInt32(info[0], "questId");
  const auto stage = NapiHelper::ExtractInt32(info[1], "stage");
  const auto cause = NapiHelper::ExtractString(info[2], "cause");
  const auto root = NapiHelper::ExtractString(info[3], "root");
  const auto ttlMs = NapiHelper::ExtractUInt32(info[4], "ttlMs");
  bool shaped = root.size() <= QuestCause::kMaxRootBytes;
  for (const char c : root) {
    shaped = shaped &&
      ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
       (c >= '0' && c <= '9') || c == '_' || c == '.');
  }
  if (questId == 0 || stage < QuestCause::kReset || stage > 0xFFFF ||
      (cause != "world" && cause != "seat") || !shaped || ttlMs == 0 ||
      ttlMs > QuestCause::kMaxMarkMs) {
    return info.Env().Null();
  }
  const auto* quest = RE::TESForm::LookupByID<RE::TESQuest>(questId);
  if (!quest) {
    return info.Env().Null();
  }
  const bool running = quest->IsEnabled();
  const bool startsEnabled = quest->StartsEnabled();
  const std::optional<int32_t> made =
    QuestCause::WordMade(stage, running, startsEnabled);
  Napi::Value marked = info.Env().Null();
  if (made) {
    QuestCause::Verdict verdict;
    verdict.world = cause == "world";
    verdict.root = root;
    QuestCause::Mark(questId, *made, verdict, ttlMs);
    marked = Napi::Number::New(info.Env(), *made);
  }
  logger::trace(
    "[verb:quest-stage] markQuestCause({:x}, {}): {} (running {}, "
    "Start Game Enabled {}; cause {}, root {}, for {} ms)",
    questId, stage,
    made ? fmt::format("marked {}", *made) : std::string("nothing marked"),
    running, startsEnabled, cause, root.empty() ? "none" : root, ttlMs);
  auto res = Napi::Object::New(info.Env());
  res.Set("marked", marked);
  res.Set("running", Napi::Boolean::New(info.Env(), running));
  res.Set("startsEnabled", Napi::Boolean::New(info.Env(), startsEnabled));
  return res;
}

// thuum ADR-068 rung 5, PQ-3 (docs/research/rung5-quests-2026-11-09.md
// 8(a); ActingScope.h, PlayerGlobals): the globals each player keeps,
// skymp5-client's PlayerGlobals watch list (the server's playerGlobals, the
// marked quests' globals composed in), replaced whole at each call ([]
// clears it); inside another player's acting scope the seam skips a
// GlobalVariable.SetValue on one. setTriggerPresserExempt's shape: the JS
// thread alone, no game call, logged for the lab
Napi::Value PuppetApi::SetPlayerGlobals(const Napi::CallbackInfo& info)
{
  const auto globals = NapiHelper::ExtractArray(info[0], "globalFormIds");
  std::vector<uint32_t> ids;
  const uint32_t n = globals.Length();
  ids.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const std::string name = "globalFormIds[" + std::to_string(i) + "]";
    ids.push_back(NapiHelper::ExtractUInt32(globals.Get(i), name.c_str()));
  }
  ActingScope::PlayerGlobals::Set(ids);
  logger::info("[verb:quests] setPlayerGlobals: {} global(s) a scoped stack "
               "never writes here (PQ-3)",
               ActingScope::PlayerGlobals::Count());
  return Napi::Boolean::New(info.Env(), true);
}

// thuum ADR-068 rung 5's full build (Eli's D-Q2 word, "yes full
// transformation"; docs/research/rung5-quests-2026-11-09.md 8(a);
// ActingScope.h, PlayerQuests): the per-player quests, the server's derived
// playerQuests as skymp5-client receives them, replaced whole at each call
// ([] clears it); the Papyrus event gate passes an event to such a quest's
// own scripts and to its aliases' scripts on this seat, its own player's
// (FridaHooks.cpp). setPlayerGlobals' shape: the JS thread alone, no game
// call, logged for the lab
Napi::Value PuppetApi::SetPlayerQuests(const Napi::CallbackInfo& info)
{
  const auto quests = NapiHelper::ExtractArray(info[0], "questFormIds");
  std::vector<uint32_t> ids;
  const uint32_t n = quests.Length();
  ids.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const std::string name = "questFormIds[" + std::to_string(i) + "]";
    ids.push_back(NapiHelper::ExtractUInt32(quests.Get(i), name.c_str()));
  }
  ActingScope::PlayerQuests::Set(ids);
  logger::info("[verb:quests] setPlayerQuests: {} quest(s) whose own and "
               "alias scripts' events run here, this seat's own player's",
               ActingScope::PlayerQuests::Count());
  return Napi::Boolean::New(info.Env(), true);
}

// isPerformingAction(actorFormId): the hook's mark, on from the action
// entry's enter to its leave (FridaHooks.cpp OnPerformActionEnter)
Napi::Value PuppetApi::IsPerformingAction(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  return Napi::Boolean::New(info.Env(), Hosted::IsPerforming(actorFormId));
}

// thuum docs/DECISIONS.md ADR-068 D-S3 (TrapFamily.h): the role from the
// reads the gate and the seam make, here on the JS thread, as MagicApi's
// setActorAimPitch reads its actor (TESForm::LookupByID under the form
// table's own lock), with the attached scripts read under the VM's lock; no
// task queued, no Papyrus native called
Napi::Value PuppetApi::GetTrapRole(const Napi::CallbackInfo& info)
{
  const auto formId = NapiHelper::ExtractUInt32(info[0], "formId");
  const auto role = TrapFamily::RoleOf(formId);
  if (!role) {
    return info.Env().Null();
  }
  auto res = Napi::Object::New(info.Env());
  res.Set("family", Napi::Boolean::New(info.Env(), role->family));
  res.Set("playerOnly", Napi::Boolean::New(info.Env(), role->playerOnly));
  res.Set("destroyed", Napi::Boolean::New(info.Env(), role->destroyed));
  res.Set("trigger", Napi::Boolean::New(info.Env(), role->trigger));
  res.Set("exempt", Napi::Boolean::New(info.Env(), role->exempt));
  return res;
}

// thuum docs/DECISIONS.md ADR-068 D-W1 A and D-C2 A (the presser rule; thuum
// .claude/rules/native.md's kill switch): the Papyrus event gate's rule over
// a world object's OnTriggerEnter and OnTriggerLeave on or off
// (FridaHooks.cpp OnSendEventEnter), one atomic flag, off by default so a
// client that never sets it keeps D-S3; skymp5-client's setting
// triggerPresser (default true) turns it on. No game call; the setter logs
// the switch for the lab
Napi::Value PuppetApi::SetTriggerPresserRule(const Napi::CallbackInfo& info)
{
  const auto on = NapiHelper::ExtractBoolean(info[0], "on");
  Frida::SetTriggerPresserRule(on);
  return Napi::Boolean::New(info.Env(), true);
}

// thuum docs/research/fifty-fixes-2026-11-07.md 1(b), the furniture branch
// (thuum .claude/rules/native.md's kill switch): the animation hook's rule
// over a furniture's own graph event on or off (FridaHooks.cpp
// OnSendAnimationEventEnter), one atomic flag, off by default so a client
// that never sets it keeps every graph event on the sendAnimationEvent
// handlers; skymp5-client's setting furnitureAnimation (default true) turns
// it on. No game call; the setter logs the switch for the lab
Napi::Value PuppetApi::SetFurnitureAnimation(const Napi::CallbackInfo& info)
{
  const auto on = NapiHelper::ExtractBoolean(info[0], "on");
  Frida::SetFurnitureAnimation(on);
  return Napi::Boolean::New(info.Env(), true);
}

// thuum docs/DECISIONS.md ADR-068 D-W2 A (docs/research/
// fifty-fixes-2026-11-07.md section 7.2(a); thuum .claude/rules/native.md's
// kill switch): the look's switch (ScriptNatives.h), one atomic flag, off by
// default, acting only with the presser rule on; skymp5-client's setting
// triggerLook (default true) turns it on. No game call; logged for the lab
Napi::Value PuppetApi::SetTriggerLook(const Napi::CallbackInfo& info)
{
  const auto on = NapiHelper::ExtractBoolean(info[0], "on");
  ScriptNatives::SetTriggerLook(on);
  logger::info("[verb:traps] setTriggerLook({}): {}", on,
               on ? "a family trigger's own calls on itself in its firing "
                    "window cross as its firing seat's look (D-W2), with "
                    "the presser rule on"
                  : "a family trigger's own calls stay on each seat");
  return Napi::Boolean::New(info.Env(), true);
}

// D-W2 A's fallback lever (TrapFamily.h, Exempt): the class names, replacing
// the list whole ([] clears it); no game call; logged for the lab
Napi::Value PuppetApi::SetTriggerPresserExempt(const Napi::CallbackInfo& info)
{
  const auto classes = NapiHelper::ExtractArray(info[0], "classes");
  std::vector<std::string> names;
  const uint32_t n = classes.Length();
  names.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const std::string name = "classes[" + std::to_string(i) + "]";
    names.push_back(NapiHelper::ExtractString(classes.Get(i), name.c_str()));
  }
  TrapFamily::Exempt::Set(names);
  const std::string stored = TrapFamily::Exempt::Names();
  logger::info("[verb:traps] setTriggerPresserExempt: {}",
               stored.empty()
                 ? std::string("no class exempt, the presser rule whole")
                 : "these keep D-S3's per-seat firing: " + stored);
  return Napi::Boolean::New(info.Env(), true);
}

// thuum ADR-068 D-S3 point 4: the followed set the seam reads (TrapFamily.h),
// fed by skymp5-client's prop stream at a trap body's owner word and at the
// owner's at-rest sample; setHosted's shape. The count is logged for the lab
Napi::Value PuppetApi::SetPropFollowed(const Napi::CallbackInfo& info)
{
  const auto formId = NapiHelper::ExtractUInt32(info[0], "formId");
  const auto followed = NapiHelper::ExtractBoolean(info[1], "followed");
  if (formId == 0 || formId == 0x14) {
    return Napi::Boolean::New(info.Env(), false);
  }
  TrapFamily::Followed::Set(formId, followed);
  logger::info("[verb:traps] setPropFollowed({:x}, {}): followed set {}",
               formId, followed, TrapFamily::Followed::Count());
  return Napi::Boolean::New(info.Env(), true);
}

// thuum ADR-068 D-S3 point 3: the applying set the gate reads (TrapFamily.h),
// raised by skymp5-client's trap results around a served record's apply and
// lowered after it; setHosted's shape. The count is logged for the lab
Napi::Value PuppetApi::SetRecordApplying(const Napi::CallbackInfo& info)
{
  const auto formId = NapiHelper::ExtractUInt32(info[0], "formId");
  const auto applying = NapiHelper::ExtractBoolean(info[1], "applying");
  if (formId == 0 || formId == 0x14) {
    return Napi::Boolean::New(info.Env(), false);
  }
  TrapFamily::RecordApplying::Set(formId, applying);
  logger::info("[verb:traps] setRecordApplying({:x}, {}): applying set {}",
               formId, applying, TrapFamily::RecordApplying::Count());
  return Napi::Boolean::New(info.Env(), true);
}

namespace {
// thuum ADR-071 rung 4 (statics and a body's parts; the build spec's C3): a
// reference's bodies are found by PropBodies' one walk (PropBodies.h), which
// replaced rung 2's BodyOf (the 3D root's own body, else the first on the
// root's children, one level down) with CommonLib's whole scene-graph order
// (BSVisit::TraverseScenegraphCollision, src/RE/B/BSVisit.cpp:11-37). Where
// BodyOf found a body and the reference has no other, the walk's one body is
// the same one, so an item's and a trap rock's row is rung 3's; a body deeper
// than one level is found too. readBodies, its island, setBodyVelocity,
// getBodyClass and imposeBodies all read the same walk, so a write lands on
// the parts a read named

// a world rotation as Euler XYZ in degrees, the inverse of CommonLib's
// NiMatrix3::SetEulerAnglesXYZ (src/RE/N/NiMatrix3.cpp:90-109): its
// ToEulerAnglesXYZ formula (:36-56) with the standard arctangent, and the
// pitch of 90 degrees solved from SetEulerAnglesXYZ itself. The pin's
// NiFastATan2 (src/RE/N/NiMath.cpp:23-71) answers -atan(y / x) wherever |x|
// is at least |y| (:61 takes the series from an offset of 0), so through
// ToEulerAnglesXYZ a yaw of 30 degrees reads -30 and one of 60 reads 60, and
// three random rotations in four fail to rebuild their matrix (the pin's own
// bodies compiled and run on their own, 2026-11-12; this one rebuilds all
// 100000). CommonLib is never modified here, so the caller carries the fix,
// as with BSTSmallSharedArray::empty. x and z in -180 to 180, y in -90 to 90
RE::NiPoint3 EulerDegreesXYZ(const RE::NiMatrix3& m)
{
  // SetEulerAnglesXYZ's entry[0][2] is -sin(y)
  const float s = m.entry[0][2];
  float x = 0.f;
  float y = 0.f;
  float z = 0.f;
  if (s > -1.f && s < 1.f) {
    y = -std::asin(s);
    x = std::atan2(m.entry[1][2], m.entry[2][2]);
    z = std::atan2(m.entry[0][1], m.entry[0][0]);
  } else if (s <= -1.f) {
    // a pitch of +90 degrees: the matrix holds x - z alone, z taken as 0
    y = RE::NI_HALF_PI;
    x = std::atan2(m.entry[1][0], m.entry[1][1]);
  } else {
    // a pitch of -90 degrees: the matrix holds x + z alone
    y = -RE::NI_HALF_PI;
    x = std::atan2(-m.entry[1][0], m.entry[1][1]);
  }
  return { RE::rad_to_deg(x), RE::rad_to_deg(y), RE::rad_to_deg(z) };
}

// an hkVector4's three components (hkVector4.h:53, an __m128) times a scale
RE::NiPoint3 Vec3(const RE::hkVector4& v, float scale)
{
  float f[4];
  _mm_storeu_ps(f, v.quad);
  return { f[0] * scale, f[1] * scale, f[2] * scale };
}

// a reference with a Havok body, as readBodies' island and the propImpulse
// event name one (FridaHooks.cpp's BodyRef): its form id and its base's form
// type (RE::FormType, F/FormTypes.h:138, whose numbers Skyrim Platform's
// FormType keeps), so the client tells its rows with no game call
struct BodyRef
{
  uint32_t refrId = 0;
  uint32_t formType = 0;
};

struct BodyRead
{
  uint32_t formId = 0;
  bool awake = false;
  uint32_t motion = 0;
  RE::NiPoint3 pos;
  RE::NiPoint3 rot;
  RE::NiPoint3 lin;
  RE::NiPoint3 ang;
  // with islands: whether the body carries its island (a dynamic type,
  // awake), and the island's other members (ReadIslandOf, below)
  bool hasIsland = false;
  std::vector<BodyRef> island;
  // thuum ADR-071 rung 4 (D-PH10 A): a reference that is not whole and has a
  // part carries every part's pose in walk order, pos in game units and quat
  // as Havok keeps it (ReadBody); none for a whole reference, one with no
  // part, or a walk cut short
  bool hasBones = false;
  std::vector<PropBodies::Pose> bones;
};

// thuum tracker #178 (a shout on a player): the player's own body is read
// from its third-person 3D (TESObjectREFR::Get3D(false), CommonLibSSE-NG
// include/RE/T/TESObjectREFR.h:376), the skeleton a knock ragdolls, whatever
// the camera shows; every other reference's from its current 3D
RE::NiAVObject* PropRootOf(RE::TESObjectREFR& refr)
{
  return refr.formID == 0x14 ? refr.Get3D(false) : refr.Get3D();
}

// and it is read only while it is knocked (Actor::IsInRagdollState,
// include/RE/A/Actor.h:597) and alive: its own game's live-body stream
// (rung 5's live-body kind), never as a prop otherwise
bool OwnPlayerKnocked()
{
  auto* player = RE::PlayerCharacter::GetSingleton();
  return player && !player->IsDead() && player->IsInRagdollState();
}

// one reference's body, false when the reference, its 3D
// (TESObjectREFR::Get3D, T/TESObjectREFR.h:375), its cell's bhkWorld
// (TESObjectCELL::GetbhkWorld, T/TESObjectCELL.h:197) or the rigid body is
// missing. Read on the JS thread and under no lock, the rung 2 plan's
// section 4 order: HYPOTHESIS that the JS tick never overlaps Havok's step on
// its worker threads (docs/engine/havok-props.md E2), so the reads are whole;
// the fields are aligned floats, one pointer and a byte, so a read racing the
// step would mix at most one step's motion, never a value that is not a
// number, and the island pointer is the one dereference that could meet a
// freed island (HYPOTHESIS: Havok frees a merged island inside its own step).
// No lock is taken on the JS thread: Hook.cpp:86 sends every animation event
// to the one JS thread through PushAndWait (FridaHooks.cpp:1744), from
// whatever thread sent it, before any filter, so a JS thread waiting on
// bhkWorld::worldLock (B/bhkWorld.h:71) while that thread, or one it waits
// for, holds it as a writer would hang the game (the review of 3892415e,
// finding 1). x-body-read and x-prop-follow read for a torn read across reads
// every update (a crash, a step spike in the follower's largest step); if one
// shows, the read moves to the game thread in this file's poll shape
// (gameThrQ, as PollHostedGaze reads), never a lock on the JS thread. The
// fields: pos the root's world translate in game units and rot
// its world rotation (N/NiAVObject.h:158; N/NiTransform.h:25-26), the body's
// drawn place, CommonLib's own node-to-reference pair (MoveToNode,
// src/RE/T/TESObjectREFR.cpp:780-789; HYPOTHESIS that the engine's reference
// angle keeps the same convention, x-body-read's premise P0); lin and ang the
// entity's motion (H/hkpEntity.h:99; H/hkpMotion.h:84-85), lin times the
// world scale's inverse (B/bhkWorld.h:48-52) into game units a second, ang
// unscaled; motion its type (hkpMotion.h:77); awake the entity's island
// (hkpEntity.h:92) set and in the world's active list
// (H/hkpSimulationIsland.h:41, the bit lab/frida/prop-sleep.js reads;
// HYPOTHESIS until E10 reads under the 60 fps cap: in the active list means
// awake).
// Since thuum ADR-071 rung 4 (D-PH10 A; the build spec's C3) the bodies are
// PropBodies' walk (ReadWalk, its fault said as line N4 and no row this
// time), and the row is read three ways. A whole reference (one body, not
// Fixed: an item, a trap rock) answers rung 3's row from its one body, now
// also from a body deeper than one level (HYPOTHESIS, the build spec's F22
// beside F18's trap rock: every item is whole; the client routes an item by
// its form type before any class read, propService.ts kindOf, so an item of
// several bodies would answer its fastest part's motion and velocities, its
// bones dropped by its client, and have its take-over velocity refused as
// N5); one with no part (an all-Fixed static) answers its first body's row,
// motion 5, as BodyOf did. Otherwise, with a part (the Riverwood Trader
// sign's boards on their Fixed anchor):
// awake when any part's island is set and in the active list; motion, lin
// and ang the fastest part's (the largest linear speed), so the client's
// send priority needs no math of its own; and bones, every part's pose in
// walk order (PropBodies::PoseOf, pos times the world scale's inverse into
// game units, quat as read), none when the walk was cut. pos and rot stay the
// 3D root's either way (the sign's anchor; HYPOTHESIS for a whole body that
// hangs below its root that the root moves with it, rung 2's R9: the trap
// scenarios read it). The parts' fields are read after the guarded walk,
// under no lock, as the velocities always were
bool ReadBody(uint32_t formId, float toGameUnits, BodyRead& out)
{
  auto* refr = RE::TESForm::LookupByID<RE::TESObjectREFR>(formId);
  auto* root = refr ? PropRootOf(*refr) : nullptr;
  auto* cell = refr ? refr->GetParentCell() : nullptr;
  auto* world = cell ? cell->GetbhkWorld() : nullptr;
  if (!root || !world) {
    return false;
  }
  PropBodies::Walk walk;
  if (!PropBodies::ReadWalk(*root, formId, walk) || walk.bodies == 0) {
    return false;
  }
  out.formId = formId;
  out.pos = root->world.translate;
  out.rot = EulerDegreesXYZ(root->world.rotate);
  if (PropBodies::IsWhole(walk) || walk.nParts == 0) {
    const auto& motion = walk.first.havok->motion;
    const auto* island = walk.first.havok->simulationIsland;
    out.awake = island && island->isInActiveIslandsArray;
    out.motion = motion.type.underlying();
    out.lin = Vec3(motion.linearVelocity, toGameUnits);
    out.ang = Vec3(motion.angularVelocity, 1.f);
    return true;
  }
  const RE::hkpRigidBody* fastest = walk.parts[0].havok;
  float fastestSq = -1.f;
  bool awake = false;
  for (uint32_t i = 0; i < walk.nParts; ++i) {
    const RE::hkpRigidBody* havok = walk.parts[i].havok;
    const auto* island = havok->simulationIsland;
    awake = awake || (island && island->isInActiveIslandsArray);
    const RE::NiPoint3 v = Vec3(havok->motion.linearVelocity, 1.f);
    const float sq = v.x * v.x + v.y * v.y + v.z * v.z;
    if (sq > fastestSq) {
      fastestSq = sq;
      fastest = havok;
    }
  }
  out.awake = awake;
  out.motion = fastest->motion.type.underlying();
  out.lin = Vec3(fastest->motion.linearVelocity, toGameUnits);
  out.ang = Vec3(fastest->motion.angularVelocity, 1.f);
  if (walk.cut) {
    return true;
  }
  out.hasBones = true;
  out.bones.reserve(walk.nParts);
  for (uint32_t i = 0; i < walk.nParts; ++i) {
    PropBodies::Pose pose = PropBodies::PoseOf(*walk.parts[i].havok);
    for (float& f : pose.pos) {
      f *= toGameUnits;
    }
    out.bones.push_back(pose);
  }
  return true;
}

// thuum ADR-071 rung 3 (the island's claim; lab/.cache/drafts/physics/
// rung3-plan.md B4, on H4's facts, CONFIRMED by 20261111-223256 on c1: a
// resting pile is one sleeping island, a knocked neighbour joins it on the
// first moving frame, islands split in 2 to 9 frames and merge again on
// contact in flight, a Keyframed body a member that never moves): at most
// this many of an awake body's island's other members, by reference
constexpr size_t kIslandRefsMax = 16;
// at most this many of the island's entities looked at, so a size read
// mid-change still bounds the walk (UNMEASURED: 223256's pile held six
// cabbages)
constexpr int32_t kIslandEntitiesRead = 256;
// the walk's answer when a read faulted
constexpr size_t kIslandFault = static_cast<size_t>(-1);

// the walk itself, plain data alone so the frame below may guard it (MSVC
// C2712): the body's island (H/hkpEntity.h:92) unless it is its world's
// fixed island (hkpSimulationIsland::world, H/hkpSimulationIsland.h:31;
// hkpWorld::fixedIsland, H/hkpWorld.h:97; the cart's 1,710 statics in
// 223256), then its entities (hkpSimulationIsland.h:53, an hkInplaceArray,
// H/hkArray.h:196, its size and data read once) in the island's own order,
// each of a dynamic motion type (IsDynamicMotion: a Keyframed member is
// another seat's stream or an animated actor, a Fixed one the world) whose
// collidable names a reference (hkpWorldObject::GetUserData,
// H/hkpWorldObject.h:62, which is TESHavokUtilities::FindCollidableRef,
// src/RE/H/hkpWorldObject.cpp:23-27), each reference once with its base's
// form type; the player's own, the body's own (a reference of several
// bodies, the Riverwood Trader sign's seven) and a body with no reference
// left out. Fills out[0, max) and answers the count
size_t WalkIsland(const RE::hkpEntity& body, uint32_t self, BodyRef* out,
                  size_t max)
{
  const RE::hkpSimulationIsland* island = body.simulationIsland;
  if (!island) {
    return 0;
  }
  const RE::hkpWorld* world = island->world;
  if (world && world->fixedIsland == island) {
    return 0;
  }
  const RE::hkpEntity* const* entities = island->entities.data();
  const int32_t count =
    std::min<int32_t>(island->entities.size(), kIslandEntitiesRead);
  size_t n = 0;
  for (int32_t i = 0; entities && i < count && n < max; ++i) {
    const RE::hkpEntity* member = entities[i];
    if (!member || member == &body ||
        !PropBodies::IsDynamicMotion(member->motion.type.get())) {
      continue;
    }
    const RE::TESObjectREFR* refr = member->GetUserData();
    const uint32_t refrId = refr ? refr->formID : 0;
    if (refrId == 0 || refrId == 0x14 || refrId == self) {
      continue;
    }
    bool seen = false;
    for (size_t k = 0; k < n && !seen; ++k) {
      seen = out[k].refrId == refrId;
    }
    if (seen) {
      continue;
    }
    const RE::TESBoundObject* base = refr->GetBaseObject();
    out[n].refrId = refrId;
    out[n].formType = base ? static_cast<uint32_t>(base->GetFormType()) : 0u;
    ++n;
  }
  return n;
}

// the walk in a structured exception frame, the fork's GetNthVtableElement
// shape (PapyrusTESModPlatform.cpp:253-258). The walk runs on the JS thread
// with no lock (rung 2's fresh review, finding 1: Hook.cpp:86's PushAndWait
// lets an engine thread wait on the JS thread while it holds a world lock as
// a writer, so a lock taken here can hang the game; never one), so it reads
// the island while Havok's step may merge or split it: HYPOTHESIS that such a
// read is torn at worst. A member freed mid-read faults here instead of
// taking the game down, and answers kIslandFault
size_t GuardedWalkIsland(const RE::hkpEntity& body, uint32_t self,
                         BodyRef* out, size_t max)
{
  __try {
    return WalkIsland(body, self, out, max);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return kIslandFault;
  }
}

std::atomic<uint64_t> g_islandFaults{ 0 };

// the body whose island readBodies names (thuum ADR-071 rung 4): a whole
// reference's one body when its type is dynamic, rung 3's body; otherwise the
// first part in walk order of a dynamic type whose island is set and in the
// active list (H/hkpEntity.h:92; H/hkpSimulationIsland.h:41). Null for none
const RE::hkpRigidBody* IslandBodyOf(const PropBodies::Walk& walk)
{
  if (PropBodies::IsWhole(walk)) {
    const RE::hkpRigidBody* havok = walk.first.havok;
    return havok && PropBodies::IsDynamicMotion(havok->motion.type.get())
      ? havok
      : nullptr;
  }
  for (uint32_t i = 0; i < walk.nParts; ++i) {
    const RE::hkpRigidBody* havok = walk.parts[i].havok;
    const auto* island = havok->simulationIsland;
    if (PropBodies::IsDynamicMotion(havok->motion.type.get()) && island &&
        island->isInActiveIslandsArray) {
      return havok;
    }
  }
  return nullptr;
}

// one reference's island (readBodies with islands, for a body its read found
// awake and of a dynamic type): the bodies walked again as ReadBody walks
// them (PropBodies::ReadWalk) and the island's body taken (IslandBodyOf),
// then the guarded walk, the island itself read inside it; on the
// JS thread, never inside the step from the impulse replacement (R2-4's
// threading HYPOTHESIS), taking no engine lock. The other calls are a
// form-table read and field reads (TESForm::LookupByID,
// include/RE/T/TESForm.h:209-218, takes no lock at this pin) and
// FindCollidableRef in the walk: HYPOTHESIS that it takes none either (E26
// read it safe inside Havok's own callbacks). False with no island read: no
// body, no island body (IslandBodyOf), a faulted body walk (said as N4) or a
// faulted island read (said at its first and every hundredth: a torn read
// here moves the read to the game thread's queue in this file's poll shape,
// PollHostedGaze, never to a lock)
bool ReadIslandOf(uint32_t formId, std::vector<BodyRef>& out)
{
  auto* refr = RE::TESForm::LookupByID<RE::TESObjectREFR>(formId);
  auto* root = refr ? refr->Get3D() : nullptr;
  if (!root) {
    return false;
  }
  PropBodies::Walk walk;
  if (!PropBodies::ReadWalk(*root, formId, walk)) {
    return false;
  }
  const RE::hkpRigidBody* body = IslandBodyOf(walk);
  if (!body) {
    return false;
  }
  BodyRef found[kIslandRefsMax];
  const size_t n = GuardedWalkIsland(*body, formId, found, kIslandRefsMax);
  if (n == kIslandFault) {
    const uint64_t faults =
      g_islandFaults.fetch_add(1, std::memory_order_relaxed) + 1;
    if (faults == 1 || faults % 100 == 0) {
      logger::warn("[verb:prop-physics] island read of {:x} faulted: a "
                   "member changed under the lock-free read, no island this "
                   "read ({} so far)",
                   formId, faults);
    }
    return false;
  }
  out.assign(found, found + n);
  return true;
}

// readBodies' filter (rule 9): the ids its last read without all found
// awake, so a body is answered while awake and once more at the read where
// its awake bit fell, and a resting one never; an id left out (no body) is
// dropped from it. Bounded and cleared by size as the contact hook's map is
// (FridaHooks.cpp OnPropContactEnter)
constexpr size_t kBodyAwakeMax = 1024;
std::mutex g_bodyAwakeMutex;
std::unordered_set<uint32_t> g_bodyAwake;
} // namespace

// thuum docs/DECISIONS.md ADR-071 rung 2 (the sample v2): the owner's stream
// reads its simulated bodies every update with all false; a placement's test
// and the lab's "body" line read one id with all true, which leaves the
// filter alone so they never take the stream's fall. On the JS thread as
// getTrapRole reads the engine (above), since the engine's own reads are
// synchronous while its writes are latent (Skyrim Platform's typing: getMass
// a number, applyHavokImpulse and setMotionType promises); no task, no
// Papyrus call
Napi::Value PuppetApi::ReadBodies(const Napi::CallbackInfo& info)
{
  const auto ids = NapiHelper::ExtractArray(info[0], "formIds");
  const bool all = NapiHelper::ExtractBoolean(info[1], "all");
  // thuum ADR-071 rung 3 (the island's claim, rung3-plan.md B4): the third
  // argument, false when absent (rung 2's call), adds each awake dynamic
  // body's island (ReadIslandOf, lock-free on this thread)
  const bool islands = info.Length() >= 3 && !info[2].IsUndefined() &&
    !info[2].IsNull() && NapiHelper::ExtractBoolean(info[2], "islands");
  std::vector<uint32_t> formIds;
  const uint32_t n = ids.Length();
  formIds.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const std::string name = "formIds[" + std::to_string(i) + "]";
    formIds.push_back(NapiHelper::ExtractUInt32(ids.Get(i), name.c_str()));
  }
  const float toGameUnits = RE::bhkWorld::GetWorldScaleInverse();
  std::vector<BodyRead> answered;
  answered.reserve(formIds.size());
  for (const auto formId : formIds) {
    BodyRead read;
    const bool found = formId != 0 && (formId != 0x14 || OwnPlayerKnocked()) &&
      ReadBody(formId, toGameUnits, read);
    if (found && islands && read.awake &&
        PropBodies::IsDynamicMotion(
          static_cast<RE::hkpMotion::MotionType>(read.motion))) {
      read.hasIsland = ReadIslandOf(formId, read.island);
    }
    if (all) {
      if (found) {
        answered.push_back(std::move(read));
      }
      continue;
    }
    std::lock_guard<std::mutex> lock(g_bodyAwakeMutex);
    if (found && read.awake) {
      if (g_bodyAwake.size() > kBodyAwakeMax) {
        g_bodyAwake.clear();
      }
      g_bodyAwake.insert(formId);
      answered.push_back(std::move(read));
      continue;
    }
    const bool fell = g_bodyAwake.erase(formId) != 0;
    if (fell && found) {
      answered.push_back(std::move(read));
    }
  }
  const auto toArray = [&](const RE::NiPoint3& v) {
    auto arr = Napi::Array::New(info.Env(), 3);
    arr.Set(uint32_t(0), Napi::Number::New(info.Env(), v.x));
    arr.Set(uint32_t(1), Napi::Number::New(info.Env(), v.y));
    arr.Set(uint32_t(2), Napi::Number::New(info.Env(), v.z));
    return arr;
  };
  auto res = Napi::Array::New(info.Env(), answered.size());
  for (uint32_t i = 0; i < answered.size(); ++i) {
    const auto& read = answered[i];
    auto row = Napi::Object::New(info.Env());
    row.Set("formId", Napi::Number::New(info.Env(), read.formId));
    row.Set("awake", Napi::Boolean::New(info.Env(), read.awake));
    row.Set("motion", Napi::Number::New(info.Env(), read.motion));
    row.Set("pos", toArray(read.pos));
    row.Set("rot", toArray(read.rot));
    row.Set("lin", toArray(read.lin));
    row.Set("ang", toArray(read.ang));
    if (read.hasIsland) {
      auto island = Napi::Array::New(info.Env(), read.island.size());
      for (uint32_t k = 0; k < read.island.size(); ++k) {
        auto ref = Napi::Object::New(info.Env());
        ref.Set("refrId",
                Napi::Number::New(info.Env(), read.island[k].refrId));
        ref.Set("formType",
                Napi::Number::New(info.Env(), read.island[k].formType));
        island.Set(k, ref);
      }
      row.Set("island", island);
    }
    // thuum ADR-071 rung 4: a body of several parts' poses, { pos: [x, y, z]
    // in game units, quat: [x, y, z, w] } a part, in walk order
    if (read.hasBones) {
      auto bones = Napi::Array::New(info.Env(), read.bones.size());
      for (uint32_t k = 0; k < read.bones.size(); ++k) {
        const auto& pose = read.bones[k];
        auto pos = Napi::Array::New(info.Env(), 3);
        for (uint32_t c = 0; c < 3; ++c) {
          pos.Set(c, Napi::Number::New(info.Env(), pose.pos[c]));
        }
        auto quat = Napi::Array::New(info.Env(), 4);
        for (uint32_t c = 0; c < 4; ++c) {
          quat.Set(c, Napi::Number::New(info.Env(), pose.quat[c]));
        }
        auto bone = Napi::Object::New(info.Env());
        bone.Set("pos", pos);
        bone.Set("quat", quat);
        bones.Set(k, bone);
      }
      row.Set("bones", bones);
    }
    res.Set(i, row);
  }
  return res;
}

// thuum docs/DECISIONS.md ADR-071 rung 2 (the take-over: a Keyframed copy set
// Dynamic starts at rest, HYPOTHESIS, prop-physics.md:41-42): a write, so
// latent as the engine's own are, and on the main thread: SKSE's UI task
// queue (AddUITask, CommonLibSSE-NG include/SKSE/Interfaces.h:198), drained
// by skse64's hook in UI::ProcessMessages, which Main::Update calls every
// frame; never AddTask, whose tasks run on pool threads during play (run
// 20261104-043219; ChangeForm.cpp's apply, whose route this is). The native
// never waits for its task. The task looks the reference up again, walks its
// bodies (PropBodies::ReadWalk, rung 4: a faulted walk said as N4 and nothing
// set), refuses a reference of several bodies ("a body of several parts
// (imposeBodies drives it)", the build spec's line N5: a velocity names one
// body, and a sign's parts are each the follower's drive's; HYPOTHESIS F22
// that no item has several: the client hands an item's take-over velocity
// here and never imposes one, so N5 naming an item refutes it), refuses a
// motion type that is not dynamic and calls CommonLib's
// bhkRigidBody::SetLinearVelocity (B/bhkRigidBody.h:55;
// src/RE/B/bhkRigidBody.cpp:26-31, Address Library 78089) with lin times the
// world scale (bhkWorld::GetWorldScale, B/bhkWorld.h:42-46) and
// SetAngularVelocity (bhkRigidBody.h:53; .cpp:12-17, 78090) with ang
// unscaled, each as an hkVector4 (H/hkVector4.h:19-21). The engine's setters
// take whatever lock they need themselves (HYPOTHESIS), so the task takes
// none; HYPOTHESIS too that they wake a deactivated body, x-prop-follow's
// leg D reads the motion they carry
Napi::Value PuppetApi::SetBodyVelocity(const Napi::CallbackInfo& info)
{
  const auto formId = NapiHelper::ExtractUInt32(info[0], "formId");
  const auto lin = NapiHelper::ExtractNiPoint3(info[1], "lin");
  const auto ang = NapiHelper::ExtractNiPoint3(info[2], "ang");
  bool finite = true;
  for (size_t i = 0; i < 3; ++i) {
    finite = finite && std::isfinite(lin[i]) && std::isfinite(ang[i]);
  }
  const auto* tasks = SKSE::GetTaskInterface();
  if (formId == 0 || formId == 0x14 || !finite || !tasks) {
    return Napi::Boolean::New(info.Env(), false);
  }
  tasks->AddUITask([formId, lin, ang]() {
    auto* refr = RE::TESForm::LookupByID<RE::TESObjectREFR>(formId);
    auto* root = refr ? refr->Get3D() : nullptr;
    if (!root) {
      logger::warn("[verb:prop-physics] velocity not set on {:x}: no "
                   "reference with 3D",
                   formId);
      return;
    }
    PropBodies::Walk walk;
    if (!PropBodies::ReadWalk(*root, formId, walk)) {
      return;
    }
    if (walk.bodies == 0) {
      logger::warn("[verb:prop-physics] velocity not set on {:x}: no rigid "
                   "body in its 3D",
                   formId);
      return;
    }
    if (walk.bodies > 1) {
      logger::warn("[verb:prop-physics] velocity not set on {:x}: a body of "
                   "several parts (imposeBodies drives it)",
                   formId);
      return;
    }
    const PropBodies::Body& body = walk.first;
    const auto type = body.havok->motion.type;
    const uint32_t motion = type.underlying();
    if (!PropBodies::IsDynamicMotion(type.get())) {
      logger::warn("[verb:prop-physics] velocity not set on {:x}: motion {} "
                   "is not dynamic",
                   formId, motion);
      return;
    }
    const float toHavok = RE::bhkWorld::GetWorldScale();
    body.rigid->SetLinearVelocity(
      RE::hkVector4(lin.x * toHavok, lin.y * toHavok, lin.z * toHavok, 0.f));
    body.rigid->SetAngularVelocity(RE::hkVector4(ang.x, ang.y, ang.z, 0.f));
    logger::info("[verb:prop-physics] velocity set on {:x}: ({:.1f}, {:.1f}, "
                 "{:.1f}) units a second, spin ({:.2f}, {:.2f}, {:.2f}) "
                 "radians a second, motion {}",
                 formId, lin.x, lin.y, lin.z, ang.x, ang.y, ang.z, motion);
  });
  return Napi::Boolean::New(info.Env(), true);
}

// thuum docs/DECISIONS.md ADR-071 decision 4 (rung 3, an impulse's cause;
// thuum .claude/rules/native.md's kill switch): the impulse rule at the
// replaced ScaleGameplayImpulseForce and AddExplosionImpulse on or off
// (FridaHooks.cpp), one atomic flag, off by default so a client that never
// sets it keeps every engine's own push; skymp5-client's setting
// propImpulseCause (default true) turns it on. No game call; the setter logs
// the switch for the lab and answers whether ScaleGameplayImpulseForce's
// replacement went in at install
Napi::Value PuppetApi::SetImpulseCauseRule(const Napi::CallbackInfo& info)
{
  const auto on = NapiHelper::ExtractBoolean(info[0], "on");
  return Napi::Boolean::New(info.Env(), Frida::SetImpulseCauseRule(on));
}

// thuum docs/DECISIONS.md ADR-071 rung 4, D-PH9 A (each seat reads the class
// from its loaded 3D: the root's BSX kDynamic, through one export, cached per
// binding by the client): the reference's 3D root's BSX block, by the key
// built per call as CommonLib's own HasAnimation builds it
// (src/RE/N/NiAVObject.cpp:121-125; NiObjectNET::GetExtraData,
// N/NiObjectNET.h:40-41; no function-local static, whose destructor would
// release the engine's string at the DLL's unload, HYPOTHESIS), its flags
// (BSXFlags::GetFlags, B/BSXFlags.h:43) and kDynamic (B/BSXFlags.h:22), then
// PropBodies' walk. On the JS thread, as readBodies reads (a form-table read,
// TESForm::LookupByID, T/TESForm.h:210-227, under no lock at this pin;
// TESObjectREFR::Get3D, T/TESObjectREFR.h:375; field reads), no task, no
// Papyrus call, no log of its own (the client's class line carries it).
// HYPOTHESIS that the engine's string cache takes the per-call key off the
// main thread as it does from the engine's own threads
Napi::Value PuppetApi::GetBodyClass(const Napi::CallbackInfo& info)
{
  const auto formId = NapiHelper::ExtractUInt32(info[0], "formId");
  auto* refr =
    formId != 0 ? RE::TESForm::LookupByID<RE::TESObjectREFR>(formId) : nullptr;
  auto* root = refr ? PropRootOf(*refr) : nullptr;
  if (!root) {
    return info.Env().Null();
  }
  const auto* bsx = root->GetExtraData<RE::BSXFlags>("BSX");
  const uint32_t flags = bsx ? static_cast<uint32_t>(bsx->GetFlags()) : 0u;
  const bool dynamic =
    (flags & static_cast<uint32_t>(RE::BSXFlags::Flag::kDynamic)) != 0;
  PropBodies::Walk walk;
  if (!PropBodies::ReadWalk(*root, formId, walk)) {
    return info.Env().Null();
  }
  auto parts = Napi::Array::New(info.Env(), walk.nParts);
  for (uint32_t i = 0; i < walk.nParts; ++i) {
    const auto& part = walk.parts[i];
    auto row = Napi::Object::New(info.Env());
    // the part's node by its name (NiObjectNET::name, N/NiObjectNET.h:53),
    // "" for a node with none (BSFixedString::c_str answers "" for no
    // string, B/BSFixedString.h:116-123)
    row.Set(
      "node",
      Napi::String::New(info.Env(), part.node ? part.node->name.c_str() : ""));
    row.Set(
      "motion",
      Napi::Number::New(info.Env(), part.havok->motion.type.underlying()));
    parts.Set(i, row);
  }
  auto res = Napi::Object::New(info.Env());
  res.Set("bsx", Napi::Number::New(info.Env(), flags));
  res.Set("dynamic", Napi::Boolean::New(info.Env(), dynamic));
  // thuum tracker #175: an actor's bones among parts (the first ones, its
  // carried parts after them), and whether its engine holds it ragdolled now
  // (Actor::IsInRagdollState, CommonLibSSE-NG include/RE/A/Actor.h:597; the
  // knock of a living actor, rung 5's live-body kind)
  auto* actor = refr->As<RE::Actor>();
  res.Set("bones", Napi::Number::New(info.Env(), walk.bones));
  res.Set("ragdoll",
          Napi::Boolean::New(info.Env(), actor && actor->IsInRagdollState()));
  res.Set("bodies", Napi::Number::New(info.Env(), walk.bodies));
  res.Set("fixed", Napi::Number::New(info.Env(), walk.fixed));
  res.Set("parts", parts);
  res.Set("cut", Napi::Boolean::New(info.Env(), walk.cut));
  return res;
}

namespace {
// thuum docs/DECISIONS.md ADR-071 rung 4, the follower's impose (D-PH10 A; the
// route K3 under D-PH26 A; the build spec's C3 and section C): imposeBodies
// keeps the newest poses per form and one drain on the main thread applies
// them. At most this many forms held at once (UNMEASURED: one sign today, a
// field of corpses at rung 5; ChangeForm.cpp:1490-1492's per-form table
// shape)
constexpr size_t kImposeRefsMax = 64;
// a part farther than this from its target is placed outright, not driven
// (UNMEASURED, twice E24's 32: thuum docs/engine/havok-props.md)
constexpr float kBodySnapUnits = 64.f;
// the drive's step, the part's world's own clamped to these (the spec's
// F9: HYPOTHESIS until B5's read, 1/60 under decision 7's cap)
constexpr float kImposeDtMin = 1.f / 240.f;
constexpr float kImposeDtMax = 1.f / 30.f;
// the drive's horizon (ImposeHorizonOf) at most this (UNMEASURED: a quarter
// second, a hitch's frame; a longer gap is driven over this, which falls
// short of the target and never passes it, and the next drain closes it)
constexpr float kImposeHorizonMax = 0.25f;
// a pose's quaternion off unit length by at most this, wire-validate's
// PROP_QUAT_NORM_SLACK (skymp-wire crates/wire-validate/src/lib.rs:86)
constexpr float kImposeQuatSlack = 0.01f;
// the snap line at most once a second per form
constexpr uint64_t kImposeSnapSayEveryMs = 1000;
// the drain's said-once sets, cleared past this size
constexpr size_t kImposeSaidMax = 1024;
// an entry no store has refreshed for this long is dropped unapplied by the
// next drain (UNMEASURED: the follower stores every update, and a rest is
// laid by the drain its own store queues), so a rest the drain could not lay
// or a reloaded script's last poses never land on a later drain
constexpr uint64_t kImposeStaleMs = 1000;

// one form's newest poses, game units and unit quaternions, a part each in
// the follower's walk order; rest the stream's rest, placed outright and then
// dropped; seq the store's number, so the drain drops only what it applied
struct ImposeEntry
{
  std::vector<PropBodies::Pose> poses;
  bool rest = false;
  uint64_t seq = 0;
  // Puppets::NowMs() at the store (kImposeStaleMs)
  uint64_t storedMs = 0;
};

// written on the JS thread (imposeBodies, releaseBodies) and read by the
// drain on the main thread, under the one mutex, never held across an engine
// call
std::mutex g_imposeMutex;
std::unordered_map<uint32_t, ImposeEntry> g_impose;
uint64_t g_imposeSeq = 0;
// a drain queued and not yet begun; cleared at the drain's start, so a store
// during a drain queues the next one, and at most one waits in the queue
std::atomic<bool> g_imposeDrainQueued{ false };
// stores refused for a full table
std::atomic<uint64_t> g_imposeFull{ 0 };

// the drain's own memory, read and written on the main thread alone
bool g_imposeDrainSaid = false;
std::unordered_set<uint32_t> g_imposeNo3DSaid;
std::unordered_set<uint64_t> g_imposeSkipSaid;
std::unordered_map<uint32_t, uint64_t> g_imposeSnapSaidMs;
std::unordered_map<uint32_t, uint64_t> g_imposeHorizonSaidMs;

// a held form's world clock at the drain that last drove or placed it
// (ImposeClockOf), and Puppets::NowMs() then: the next drive's horizon is
// the world's time between the two drains (ImposeHorizonOf)
struct ImposeClock
{
  float clock = 0.f;
  uint64_t atMs = 0;
};
std::unordered_map<uint32_t, ImposeClock> g_imposeClock;

// a JS array of exactly n numbers into out; a value of another shape throws
// as NapiHelper's extractors throw (ExtractNiPoint3's shape)
void ExtractLanes(const Napi::Value& v, const std::string& argName, float* out,
                  uint32_t n)
{
  const auto arr = NapiHelper::ExtractArray(v, argName.c_str());
  if (arr.Length() != n) {
    throw std::runtime_error("Expected '" + argName +
                             "' to be array with length " + std::to_string(n));
  }
  for (uint32_t i = 0; i < n; ++i) {
    const std::string name = argName + "[" + std::to_string(i) + "]";
    out[i] = NapiHelper::ExtractFloat(arr.Get(i), name.c_str());
  }
}

// the part's drive step: its world's (hkpWorldObject::world,
// H/hkpWorldObject.h:65) dynamicsStepInfo.stepInfo.deltaTime
// (H/hkpWorld.h:193, H/hkStepInfo.h:12) clamped, or 0 (no write) with no
// world or a step that is not a positive number
float ImposeStepOf(const RE::hkpRigidBody& body)
{
  const RE::hkpWorld* world = body.world;
  if (!world) {
    return 0.f;
  }
  const float raw = world->dynamicsStepInfo.stepInfo.deltaTime;
  if (!std::isfinite(raw) || !(raw > 0.f)) {
    return 0.f;
  }
  return std::clamp(raw, kImposeDtMin, kImposeDtMax);
}

// the part's world clock: its world's dynamicsStepInfo.stepInfo.endTime
// (H/hkpWorld.h:193, H/hkStepInfo.h:11), the time the world's last step
// reached (HYPOTHESIS, rule 2: that the engine's step sets it from the
// world's running time, as Havok's own does); false with no world or a
// value that is not a number
bool ImposeClockOf(const RE::hkpRigidBody& body, float& out)
{
  const RE::hkpWorld* world = body.world;
  if (!world) {
    return false;
  }
  out = world->dynamicsStepInfo.stepInfo.endTime;
  return std::isfinite(out);
}

// thuum tracker #125 (playtest 54, the Riverwood Trader's sign jumping on
// the guest's screen): the drive's horizon. A drive sets a part's velocity
// once a drain, one drain a frame, and the world integrates it on every
// step until the next drain sets another, so the velocity that closes the
// gap is the gap over the world's time between two drains, not over one
// step: over one step, a frame of n steps carries the part n times the gap
// (HYPOTHESIS: 54's guest drained about 28 times a second, c2-console.log
// "908 frames imposed" over 07:21:48 to 07:22:20Z, against the 1/60 step
// thuum ADR-071 decision 7 names, unread on its seats; its parts passed 64
// units and were placed outright 12 or more times, c2.log 972 to 1009).
// The horizon line below reads the steps a drain (the spec's F9): a horizon
// equal to the step says the drive was never the cause.
// The world's advance since this form's last drain, at least the step and
// at most kImposeHorizonMax; the step alone (today's drive) on a form's
// first drain, a clock older than kImposeStaleMs, a clock that went back or
// stood (a paused world), or no clock
float ImposeHorizonOf(float step, bool known, const ImposeClock& then,
                      float clockNow, uint64_t nowMs)
{
  if (!(step > 0.f) || !known || nowMs - then.atMs > kImposeStaleMs) {
    return step;
  }
  const float advance = clockNow - then.clock;
  if (!std::isfinite(advance) || !(advance > step)) {
    return step;
  }
  return std::min(advance, kImposeHorizonMax);
}

// one form's poses applied on the main thread; true when the form leaves the
// table (no reference with 3D, its rest placed, or a rest it cannot place: a
// faulted or cut walk, another part count; the parts then rest as their own
// engine has them, DEF-4c), so the client's last store never lingers
bool ImposeOne(uint32_t formId, const ImposeEntry& entry, float toHavok,
               float toGameUnits)
{
  auto* refr = RE::TESForm::LookupByID<RE::TESObjectREFR>(formId);
  auto* root = refr ? refr->Get3D() : nullptr;
  if (!root) {
    if (g_imposeNo3DSaid.size() > kImposeSaidMax) {
      g_imposeNo3DSaid.clear();
    }
    if (g_imposeNo3DSaid.insert(formId).second) {
      logger::info("[verb:prop-physics] impose {:x}: no reference with 3D, "
                   "dropped",
                   formId);
    }
    return true;
  }
  PropBodies::Walk walk;
  if (!PropBodies::ReadWalk(*root, formId, walk)) {
    return entry.rest;
  }
  const auto n = static_cast<uint32_t>(entry.poses.size());
  // thuum tracker #175: an actor's poses may stop short of its walk's carried
  // parts (another seat's copy carries fewer, PropBodies.cpp's WalkBodies):
  // every bone and the carried parts both hold are imposed, the rest left to
  // this engine
  const bool prefix = walk.bones > 0 && n >= walk.bones && n < walk.nParts;
  if (walk.cut || (n != walk.nParts && !prefix)) {
    // a pose for each part, in each seat's walk order, or none (E55; the
    // spec's F2): the counts both at most 64, so one key a (form, n, m)
    const uint64_t key = (static_cast<uint64_t>(formId) << 16) |
      (static_cast<uint64_t>(n & 0xff) << 8) | (walk.nParts & 0xff);
    if (g_imposeSkipSaid.size() > kImposeSaidMax) {
      g_imposeSkipSaid.clear();
    }
    if (g_imposeSkipSaid.insert(key).second) {
      logger::info("[verb:prop-physics] impose {:x} skipped: {} poses against "
                   "{} parts here{}",
                   formId, n, walk.nParts, walk.cut ? ", its walk cut" : "");
    }
    return entry.rest;
  }
  PropBodies::Pose targets[PropBodies::kPartsMax];
  float farthest = 0.f;
  uint32_t farthestPart = 0;
  for (uint32_t i = 0; i < n; ++i) {
    targets[i] = entry.poses[i];
    for (float& f : targets[i].pos) {
      f *= toHavok;
    }
    const PropBodies::Pose now = PropBodies::PoseOf(*walk.parts[i].havok);
    float sq = 0.f;
    for (size_t c = 0; c < 3; ++c) {
      const float d = now.pos[c] - targets[i].pos[c];
      sq += d * d;
    }
    const float off = std::sqrt(sq) * toGameUnits;
    if (off > farthest) {
      farthest = off;
      farthestPart = i;
    }
  }
  // the form's world clock now and at its last drain (#125), kept for the
  // next drain's horizon whether this one places or drives
  float clockNow = 0.f;
  const bool ticking = ImposeClockOf(*walk.parts[0].havok, clockNow);
  const uint64_t clockMs = Puppets::NowMs();
  const auto last = g_imposeClock.find(formId);
  const bool known = ticking && last != g_imposeClock.end();
  const ImposeClock then = known ? last->second : ImposeClock{};
  if (ticking) {
    if (last == g_imposeClock.end() && g_imposeClock.size() > kImposeSaidMax) {
      g_imposeClock.clear();
    }
    g_imposeClock[formId] = ImposeClock{ clockNow, clockMs };
  } else if (last != g_imposeClock.end()) {
    g_imposeClock.erase(last);
  }
  const bool snap = farthest > kBodySnapUnits;
  if (entry.rest || snap) {
    for (uint32_t i = 0; i < n; ++i) {
      PropBodies::PlaceBody(walk.parts[i], targets[i]);
    }
    if (snap) {
      const uint64_t nowMs = Puppets::NowMs();
      if (g_imposeSnapSaidMs.size() > kImposeSaidMax) {
        g_imposeSnapSaidMs.clear();
      }
      auto it = g_imposeSnapSaidMs.find(formId);
      if (it == g_imposeSnapSaidMs.end() ||
          nowMs - it->second >= kImposeSnapSayEveryMs) {
        g_imposeSnapSaidMs[formId] = nowMs;
        logger::info("[verb:prop-physics] impose {:x} placed: part {} {:.1f} "
                     "units off",
                     formId, farthestPart, farthest);
      }
    }
    return entry.rest;
  }
  float firstStep = 0.f;
  float firstHorizon = 0.f;
  for (uint32_t i = 0; i < n; ++i) {
    const PropBodies::Body& part = walk.parts[i];
    const float step = ImposeStepOf(*part.havok);
    const float horizon =
      ImposeHorizonOf(step, known, then, clockNow, clockMs);
    if (i == 0) {
      firstStep = step;
      firstHorizon = horizon;
    }
    PropBodies::DriveBody(part, targets[i], horizon);
  }
  // the horizon line (#125's read of the spec's F9: the world's steps a
  // drain, horizon over step) at most once a second per form
  if (g_imposeHorizonSaidMs.size() > kImposeSaidMax) {
    g_imposeHorizonSaidMs.clear();
  }
  auto said = g_imposeHorizonSaidMs.find(formId);
  if (said == g_imposeHorizonSaidMs.end() ||
      clockMs - said->second >= kImposeSnapSayEveryMs) {
    g_imposeHorizonSaidMs[formId] = clockMs;
    logger::info("[verb:prop-physics] impose {:x} driven: horizon {:.4f} s, "
                 "the world's step {:.4f} s, {:.1f} units off",
                 formId, firstHorizon, firstStep, farthest);
  }
  return false;
}

// the drain, on the main thread (SKSE's UI task queue, below): every form's
// newest poses taken one at a time under the table's mutex and applied with
// it released, so releaseBodies between two forms is honoured; a form whose
// entry changed while it was applied keeps the newer entry. No
// bhkWorld::worldLock (B/bhkWorld.h:71): HYPOTHESIS (rule 2, the static read
// on sky-re the build spec's F7 records, of 78089 and 78090, which just addr
// placed on 2026-11-13, addrlib: 0xe931e0 and 0xe932b0 on 1.6.1170) that
// they take the body's world lock for write and activate its island, so a
// sleeping copy wakes with no other call and a driven part stays awake while
// it is driven, and that SetPosition and SetRotation (slots 0x35 and 0x36)
// lock as they need
void DrainImpose()
{
  g_imposeDrainQueued.store(false);
  if (!g_imposeDrainSaid) {
    g_imposeDrainSaid = true;
    // the spec's F8: Main's (M/Main.h:87 threadID) until this line reads
    // equal ids, HYPOTHESIS
    const auto* main = RE::Main::GetSingleton();
    logger::info("[verb:prop-physics] impose drain on thread {}, Main's {}",
                 static_cast<uint32_t>(::GetCurrentThreadId()),
                 main ? main->threadID : 0u);
  }
  std::vector<uint32_t> ids;
  {
    std::lock_guard<std::mutex> lock(g_imposeMutex);
    ids.reserve(g_impose.size());
    for (const auto& held : g_impose) {
      ids.push_back(held.first);
    }
  }
  const float toHavok = RE::bhkWorld::GetWorldScale();
  const float toGameUnits = RE::bhkWorld::GetWorldScaleInverse();
  for (const uint32_t formId : ids) {
    ImposeEntry entry;
    {
      std::lock_guard<std::mutex> lock(g_imposeMutex);
      const auto it = g_impose.find(formId);
      if (it == g_impose.end()) {
        continue;
      }
      entry = it->second;
    }
    const bool stale = Puppets::NowMs() - entry.storedMs > kImposeStaleMs;
    if (!stale && !ImposeOne(formId, entry, toHavok, toGameUnits)) {
      continue;
    }
    // the form leaves the drive: its clock goes with it (#125), so a later
    // follow's first drain drives over one step
    g_imposeClock.erase(formId);
    std::lock_guard<std::mutex> lock(g_imposeMutex);
    const auto it = g_impose.find(formId);
    if (it != g_impose.end() && it->second.seq == entry.seq) {
      g_impose.erase(it);
    }
  }
}
} // namespace

// thuum docs/DECISIONS.md ADR-071 rung 4 (D-PH10 A, the stream and impose of
// a body's parts; the route K3 under D-PH26 A): the follower's newest poses
// for a reference's parts, validated and stored on the JS thread, applied on
// the main thread by one drain queued on SKSE's UI task queue (AddUITask,
// include/SKSE/Interfaces.h:198), never AddTask (ChangeForm.cpp:1477-1489,
// run 20261104-043219), at most one drain waiting, and never waited for. The
// drain looks each form up again, walks it (PropBodies::ReadWalk) and lays
// pose i on part i: with rest set, or a part more than 64 units from its
// target, every part placed (PropBodies::PlaceBody; the snap said at most
// once a second a form, line N6, and a rest dropping the form); otherwise
// every part driven (PropBodies::DriveBody) over the world's time since the
// form's last drain (ImposeHorizonOf, #125; the horizon said at most once a
// second a form). A form with no 3D is dropped (N3, once a form); a pose
// count that is not the walk's part count is skipped (N2, once a (form,
// count, parts)). Refused, false and nothing stored: 0, the player's own
// id, no pose, more than 64, a value that is not a number, a quaternion off
// unit length by more than 0.01
// (wire-validate's PROP_QUAT_NORM_SLACK), no SKSE task interface, or a full
// table (64 forms, said at the first and every hundredth). A pose of another
// shape throws, as the platform's extractors throw
Napi::Value PuppetApi::ImposeBodies(const Napi::CallbackInfo& info)
{
  const auto formId = NapiHelper::ExtractUInt32(info[0], "formId");
  const auto bones = NapiHelper::ExtractArray(info[1], "bones");
  const bool rest = NapiHelper::ExtractBoolean(info[2], "rest");
  const uint32_t n = bones.Length();
  const auto* tasks = SKSE::GetTaskInterface();
  if (formId == 0 || formId == 0x14 || n == 0 || n > PropBodies::kPartsMax ||
      !tasks) {
    return Napi::Boolean::New(info.Env(), false);
  }
  std::vector<PropBodies::Pose> poses;
  poses.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const std::string name = "bones[" + std::to_string(i) + "]";
    auto bone = NapiHelper::ExtractObject(bones.Get(i), name.c_str());
    PropBodies::Pose pose;
    ExtractLanes(bone.Get("pos"), name + ".pos", pose.pos, 3);
    ExtractLanes(bone.Get("quat"), name + ".quat", pose.quat, 4);
    bool finite = true;
    for (float f : pose.pos) {
      finite = finite && std::isfinite(f);
    }
    float sq = 0.f;
    for (float f : pose.quat) {
      finite = finite && std::isfinite(f);
      sq += f * f;
    }
    const float len = std::sqrt(sq);
    if (!finite || !(std::abs(len - 1.f) <= kImposeQuatSlack)) {
      return Napi::Boolean::New(info.Env(), false);
    }
    for (float& f : pose.quat) {
      f /= len;
    }
    poses.push_back(pose);
  }
  {
    std::lock_guard<std::mutex> lock(g_imposeMutex);
    if (g_impose.count(formId) == 0 && g_impose.size() >= kImposeRefsMax) {
      const uint64_t full =
        g_imposeFull.fetch_add(1, std::memory_order_relaxed) + 1;
      if (full == 1 || full % 100 == 0) {
        logger::warn("[verb:prop-physics] impose {:x} refused: {} forms "
                     "already held ({} so far)",
                     formId, kImposeRefsMax, full);
      }
      return Napi::Boolean::New(info.Env(), false);
    }
    auto& entry = g_impose[formId];
    entry.poses = std::move(poses);
    entry.rest = rest;
    entry.seq = ++g_imposeSeq;
    entry.storedMs = Puppets::NowMs();
  }
  if (!g_imposeDrainQueued.exchange(true)) {
    tasks->AddUITask([]() {
      try {
        DrainImpose();
      } catch (const std::exception& e) {
        logger::error("[verb:prop-physics] impose drain threw on the main "
                      "thread: {}",
                      e.what());
      }
    });
  }
  return Napi::Boolean::New(info.Env(), true);
}

// thuum docs/DECISIONS.md ADR-071 rung 4: the form's stored poses dropped,
// so the drain lays nothing more on it (a take-over, a quiet stream, a
// follow's end). Under K3 nothing else: the parts were never set another
// motion type. A later K4 or K1 (the build spec's section C) restores each
// part's captured type here. True when poses were held; false for 0, the
// player's own id or none held. A table operation on the JS thread, no game
// call
Napi::Value PuppetApi::ReleaseBodies(const Napi::CallbackInfo& info)
{
  const auto formId = NapiHelper::ExtractUInt32(info[0], "formId");
  if (formId == 0 || formId == 0x14) {
    return Napi::Boolean::New(info.Env(), false);
  }
  bool held = false;
  {
    std::lock_guard<std::mutex> lock(g_imposeMutex);
    held = g_impose.erase(formId) != 0;
  }
  return Napi::Boolean::New(info.Env(), held);
}

// thuum docs/DECISIONS.md ADR-071 rung 4, D-PH9 A (thuum
// .claude/rules/native.md's kill switch): the contact hook's world-body
// switch (FridaHooks.cpp OnPropContactEnter), one atomic flag, off by default
// so a client that never sets it keeps rung 3's filter; skymp5-client's
// setting propWorldBodies turns it on. No game call; Frida's setter logs the
// switch for the lab ("[verb:prop-physics] world-body contacts on|off", the
// spec's line N7); true
Napi::Value PuppetApi::SetWorldBodyContacts(const Napi::CallbackInfo& info)
{
  const auto on = NapiHelper::ExtractBoolean(info[0], "on");
  Frida::SetWorldBodyContacts(on);
  return Napi::Boolean::New(info.Env(), true);
}

// thuum ADR-068 rung 3 (the script record, amendment A): the pending set the
// gate reads (ScriptState.h), raised by skymp5-client's ScriptStateService
// for a served record that carries its view and lowered when it gives the
// record up (an applied record lowers it in the native); setHosted's shape.
// The count is logged for the lab
Napi::Value PuppetApi::SetScriptRecordPending(const Napi::CallbackInfo& info)
{
  const auto formId = NapiHelper::ExtractUInt32(info[0], "formId");
  const auto pending = NapiHelper::ExtractBoolean(info[1], "pending");
  if (formId == 0 || formId == 0x14) {
    return Napi::Boolean::New(info.Env(), false);
  }
  ScriptState::Pending::Set(formId, pending);
  logger::info("[verb:scripts] setScriptRecordPending({:x}, {}): pending set "
               "{}",
               formId, pending, ScriptState::Pending::Count());
  return Napi::Boolean::New(info.Env(), true);
}

// thuum ADR-068 rung 3 (the review's FIX FIRST 2): the mark lowered with the
// names the gate blanked while it was up handed back (ScriptState.h's
// Pending::Release, under the set's own lock), where setScriptRecordPending's
// lowering drops them; the JS thread alone, no game call. The answer is
// logged for the lab
Napi::Value PuppetApi::ReleaseScriptRecordPending(
  const Napi::CallbackInfo& info)
{
  const auto formId = NapiHelper::ExtractUInt32(info[0], "formId");
  if (formId == 0 || formId == 0x14) {
    return Napi::String::New(info.Env(), "[]");
  }
  const std::string blanked = ScriptState::Pending::Release(formId);
  logger::info("[verb:scripts] releaseScriptRecordPending({:x}): blanked {}, "
               "pending set {}",
               formId, blanked, ScriptState::Pending::Count());
  return Napi::String::New(info.Env(), blanked);
}

// thuum ADR-068 rung 3 (the review's NOTE 10, thuum .claude/rules/native.md's
// kill switch): the seam's touch on or off (ScriptNatives.h), one atomic
// flag the seam reads first; logged for the lab
Napi::Value PuppetApi::SetScriptRecordTouches(const Napi::CallbackInfo& info)
{
  const auto enabled = NapiHelper::ExtractBoolean(info[0], "enabled");
  ScriptNatives::SetTouchesEnabled(enabled);
  logger::info("[verb:scripts] setScriptRecordTouches({}): {}", enabled,
               enabled ? "an owned state row's call crosses as a touch"
                       : "no touch is read or queued");
  return Napi::Boolean::New(info.Env(), true);
}

namespace {
// thuum tracker #123 (playtest 54, Eli: the Riverwood intro scene's voice
// lines on the host's game alone, its animations on both): the guest asked
// each of the scene's six relayed lines and this native answered 0 for every
// one (c2-console.log 07:16:29Z to 07:16:52Z, "say 35534 info 954cc -> no
// actor or info"; no "sayTopicInfo on" line in c2.log), while the same
// puppet's greeting 4547c started at 07:18:01Z. Each refused line's topic is
// a scene topic of DialogueRiverwoodIntroScene 0x954d9 (lab/esm.py: SNAM
// SCEN, QNAM 954d9), a quest the guest had stopped with its own copy of the
// scene. HYPOTHESIS (finding 34, playtest 37): a topic's infos are resident
// only on a game whose engine brought that quest's dialogue up, so
// LookupByID found no INFO here. The line is looked for in the form map
// first, then in the relayed topic's own info array (CommonLibSSE-NG
// TESTopic.h:171-172); a topic with no such info there is left to the
// engine's pick (SayTopicInfo). HYPOTHESIS until x-riverwood-scene-voice
// reads the guest's verdicts.
struct SayLine
{
  RE::TESTopic* topic = nullptr;
  RE::TESTopicInfo* info = nullptr;
  const char* through = "nothing";
};

SayLine ResolveSayLine(uint32_t infoFormId, uint32_t topicFormId)
{
  SayLine line;
  auto* found = RE::TESForm::LookupByID<RE::TESTopicInfo>(infoFormId);
  if (found && found->parentTopic) {
    line.topic = found->parentTopic;
    line.info = found;
    line.through = "the form map";
    return line;
  }
  line.topic =
    topicFormId ? RE::TESForm::LookupByID<RE::TESTopic>(topicFormId) : nullptr;
  if (!line.topic) {
    return line;
  }
  if (found) {
    line.info = found;
    line.through = "the form map, the relayed topic its parent";
    return line;
  }
  if (line.topic->topicInfos) {
    for (uint32_t i = 0; i < line.topic->numTopicInfos; ++i) {
      auto* candidate = line.topic->topicInfos[i];
      if (candidate && candidate->formID == infoFormId) {
        line.info = candidate;
        line.through = "the relayed topic's infos";
        return line;
      }
    }
  }
  line.through = "the engine's pick from the relayed topic";
  return line;
}

// what this game holds of a topic, for the lab's read of a line that did
// not resolve: the infos its array lists and its quest's state (IsRunning,
// TESQuest.cpp:47; the flag word, TESQuest.h:41 kHasDialogueData)
std::string DescribeTopicHere(const RE::TESTopic* topic)
{
  if (!topic) {
    return "the topic unresolved here";
  }
  uint32_t listed = 0;
  if (topic->topicInfos) {
    for (uint32_t i = 0; i < topic->numTopicInfos; ++i) {
      if (topic->topicInfos[i]) {
        ++listed;
      }
    }
  }
  const RE::TESQuest* quest = topic->ownerQuest;
  return fmt::format(
    "{} of {} info(s) listed{}, quest {:x} running {} flags {:x}", listed,
    topic->numTopicInfos, topic->topicInfos ? "" : " (no array)",
    quest ? quest->formID : 0u, quest ? quest->IsRunning() : false,
    quest ? quest->data.flags.underlying() : 0u);
}

// thuum tracker #123, the stage's read (x-riverwood-scene-voice v3 run
// 20261118-031740, the tracker's note 3335): on the guest the relayed topic
// listed no info (numTopicInfos 0, a null array) and its quest 0x954d9 was
// stopped (flags 0x100), so the engine's pick with a null info built no item
// (result 6). The re-analyst's pyghidra read of 1.6.1170 on sky-re and its
// skeptic pass (2026-11-17, HYPOTHESIS by rule 2): the engine destroys a
// stopped quest's infos (the story teller's update 32487, then 25087, then
// 25520 per topic) and loads them again only at the quest's start (25212,
// the last step of its QueuedPromoteQuestTask, calls 25086) or at a save load
// of a quest the story teller still lists (25084), so a guest whose copy of
// the quest stopped holds no info of it. Such a line's quest dialogue is
// loaded through the engine's own loader (Offsets::Voice::LoadQuestDialogue),
// and the line is resolved again and said with its info given, the condition
// pick skipped. The guard is the read's: the topic lists no info (the
// loader's own filter, 25085); its quest is not enabled, not waiting on a
// stage and does not start enabled (25088's mask 0x81, the engine's own test
// before an unload, as CommonLib's IsStopped, TESQuest.cpp:57-60; the loader
// returns at once on kStartsEnabled); and its promote task is null (the
// engine's own load for the quest may still be running). A load that left
// the topic empty is never tried again; one that listed infos may run again
// after the engine unloads them.
//
// The load runs on the MAIN thread through SKSE's UI task queue (the fork's
// route for engine work: SetBodyVelocity above, FateApi.cpp's
// QueueFateTask), which runs on Main.threadID (CONFIRMED by x-corpse-record
// v3 run 20261104-053639, ten applies of ten "thread 10472 main 10472",
// thuum docs/LOG.md 2026-11-04), and the say is posted back to gameThrQ,
// where it already works. gameThrQ drains inside the Papyrus OnUpdate native
// (main.cpp), and the skeptic read that a second file's override of an info
// the first file just made (USSEP's of 954d3 and 954c6) reloads it as an
// existing form, releasing its handle through the VM's handle policy: a call
// into the VM from a VM native, kept off that thread.
//
// The loader writes the engine's "topic being loaded" global (401555) with
// no lock, and 25531 grows a topic's array with no lock, so an engine load
// beside this one could take one of these infos, give one, or grow one
// array from two threads. The platform review's xref read (pyghidra-mcp on
// sky-re, 2026-11-17): 25519's code callers are 25084 (a save load) and
// 25086, whose one code caller is 25212, the promote task's run. 25212
// leaves the quest's promoteTask set (25086 is its last call), and 25083,
// reached from the task's 25194, clears it (TESQuest.h:281, its +0x248, at
// its end). So outside a save load every engine INFO load runs while some
// quest's promoteTask is set, and this load waits while any quest's is
// (PromotingQuest), asked again each tick up to kDialogueLoadMaxTicks; past
// that the line is said with no load. HYPOTHESIS: the task is set before it
// is queued (the skeptic's read of 25046), and no quest starts off the main
// thread during this load (a Papyrus Start on the VM thread would open that
// window); lab/frida/topic-load-trace.js's overlap rows read it. Every
// info's parent on the quest's topics is read after the load and a stray
// one said as an error.
//
// While an ask is on the main thread, every ask that names a topic follows
// it there, in the order asked, and the count drops on gameThrQ as each say
// runs, so a later line never plays before an earlier one and no topic's
// array is read on that thread under a load. An ask keeps its place in line
// until its say has run on gameThrQ, and a later ask waits on the main
// thread while an earlier one holds its place, so no load runs there beside
// an earlier line's say (thuum tracker #123, the docs review of e7e9da677:
// the place was given up when the say was posted, before it ran). A say
// with no actor or no process here, which runs at once on gameThrQ whatever
// is in line (QueueDialogueLoad), reads no topic (SayTopicInfo's speak).
//
// No info fragment runs on the guest: a puppet's line raises no
// TESTopicInfoEvent while it is not talking to this player, whatever its
// quest (FridaHooks.cpp's SendTopicInfoEvent replacement, the fragment gate;
// H-57a CONFIRMED at T4, playtest 37, thuum docs/LOG.md 2026-10-28), and
// this scene's six infos carry no VMAD (the skeptic's read of the plugins).
// An info of another quest that carries a VMAD, made on the main thread by
// this load, may bind its script at load (HYPOTHESIS, not this scene). The
// first main-thread load leaves the data handler's own TESFiles open (the
// platform review's read of 25519 and TESFile::Duplicate: the main thread
// gets the original file, opened with no close; HYPOTHESIS: harmless, as the
// main thread's later readers seek it themselves). HYPOTHESIS until
// x-riverwood-scene-voice v4 reads the guest's lines, and its trace the
// threads
std::mutex g_dialogueLoadMutex;
// the topics whose quest's dialogue this game loaded for a line and found
// still empty, never tried again
std::unordered_set<uint32_t> g_dialogueLoadTopics;
// the asks gone to the main thread whose say has not run yet (dropped on
// gameThrQ just before each say)
std::atomic<int> g_dialogueLoadsQueued{ 0 };
// each ask's place in line, and the places of the asks gone to the main
// thread whose say has not run yet (taken on gameThrQ with the place, given
// up there once the say has run, SayInLine)
uint64_t g_dialogueAskSeq = 0;
std::set<uint64_t> g_dialogueAsksInLine;

// the ticks an ask waits on the main thread, each one gameThrQ drain and
// then the main thread's UI drain, before its line is said with no load
// (WeaponAnimGuard.h:71's bound, and its route)
constexpr uint32_t kDialogueLoadMaxTicks = 120;

// the thread a load ran on beside Main's (CommonLibSSE-NG Main.h:87
// threadID), the witness the lab reads (FateApi.cpp's FateThreadNote)
std::string SayThreadNote()
{
  const auto* main = RE::Main::GetSingleton();
  return fmt::format("thread {} main {}",
                     static_cast<uint32_t>(::GetCurrentThreadId()),
                     main ? main->threadID : 0u);
}

// why a line's topic does not get its quest's dialogue loaded, or null when
// the guard holds (CommonLibSSE-NG TESTopic.h:170 ownerQuest, :172
// numTopicInfos; TESQuest.h:26, :30 and :33 the flags, :263 data, :281
// promoteTask)
const char* DialogueLoadRefusal(const RE::TESTopic& topic)
{
  if (topic.numTopicInfos != 0) {
    return "the topic lists infos";
  }
  const RE::TESQuest* quest = topic.ownerQuest;
  if (!quest) {
    return "the topic has no quest";
  }
  if (quest->data.flags.any(RE::QuestFlag::kEnabled, RE::QuestFlag::kStageWait,
                            RE::QuestFlag::kStartsEnabled)) {
    return "its quest is enabled, waits on a stage or starts enabled";
  }
  if (quest->promoteTask) {
    return "its quest's promote task is in flight";
  }
  return nullptr;
}

// a quest whose promote task is in flight, or null: while one is, the
// engine's own dialogue load (25212's call of 25086) may be running on
// another thread (the comment above g_dialogueLoadMutex; CommonLibSSE-NG
// TESDataHandler.h:70 GetFormArray, TESQuest.h:281 promoteTask)
const RE::TESQuest* PromotingQuest()
{
  auto* handler = RE::TESDataHandler::GetSingleton();
  if (!handler) {
    return nullptr;
  }
  for (const RE::TESQuest* quest : handler->GetFormArray<RE::TESQuest>()) {
    if (quest && quest->promoteTask) {
      return quest;
    }
  }
  return nullptr;
}

// a quest's topics and the infos they list, and how many of those name
// another topic their parent (the race above, seen): the topic lists of
// TESQuest.h:271-272, the branched ones by branch
struct QuestInfoCount
{
  uint32_t topics = 0;
  uint32_t infos = 0;
  uint32_t strays = 0;
};

QuestInfoCount CountQuestInfos(const RE::TESQuest& quest)
{
  QuestInfoCount count;
  const auto countTopic = [&count](const RE::TESTopic* topic) {
    if (!topic) {
      return;
    }
    ++count.topics;
    if (!topic->topicInfos) {
      return;
    }
    for (uint32_t i = 0; i < topic->numTopicInfos; ++i) {
      const RE::TESTopicInfo* listed = topic->topicInfos[i];
      if (!listed) {
        continue;
      }
      ++count.infos;
      if (listed->parentTopic != topic) {
        ++count.strays;
      }
    }
  };
  for (const auto& list : quest.topics) {
    for (const RE::TESTopic* topic : list) {
      countTopic(topic);
    }
  }
  for (const auto& branches : quest.branchedDialogue) {
    for (const auto& entry : branches) {
      if (entry.second) {
        for (const RE::TESTopic* topic : *entry.second) {
          countTopic(topic);
        }
      }
    }
  }
  return count;
}

// the line's topic when its quest's dialogue is to be loaded here, or null:
// its info still not in the form map, the topic never found empty by a load
// here and the guard holding; refusal is set when the guard failed
RE::TESTopic* TopicToLoad(uint32_t infoFormId, uint32_t topicFormId,
                          const char*& refusal)
{
  refusal = nullptr;
  auto* found = RE::TESForm::LookupByID<RE::TESTopicInfo>(infoFormId);
  if (found && found->parentTopic) {
    return nullptr;
  }
  auto* topic = RE::TESForm::LookupByID<RE::TESTopic>(topicFormId);
  if (!topic) {
    return nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(g_dialogueLoadMutex);
    if (g_dialogueLoadTopics.count(topicFormId) != 0) {
      return nullptr;
    }
  }
  refusal = DialogueLoadRefusal(*topic);
  return refusal ? nullptr : topic;
}

// the main thread's load for the line's topic, with one line before the
// call and one after; a topic the load left empty is never tried again
void LoadDialogueForLine(uint32_t actorFormId, uint32_t infoFormId,
                         RE::TESTopic& topic)
{
  RE::TESQuest* quest = topic.ownerQuest;
  logger::info("[verb:npc-stream] say {:x}: topic {:x} had 0 infos on this "
               "game, its quest {:x} stopped (flags {:x}); loading its "
               "dialogue ({}; tracker #123)",
               actorFormId, topic.formID, quest->formID,
               quest->data.flags.underlying(), SayThreadNote());
  const auto start = std::chrono::steady_clock::now();
  REL::Relocation<void (*)(RE::TESQuest*)> load{
    Offsets::Voice::LoadQuestDialogue
  };
  load(quest);
  const double ms = std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - start)
                      .count();
  if (topic.numTopicInfos == 0) {
    std::lock_guard<std::mutex> lock(g_dialogueLoadMutex);
    g_dialogueLoadTopics.insert(topic.formID);
  }
  bool listed = false;
  if (topic.topicInfos) {
    for (uint32_t i = 0; i < topic.numTopicInfos; ++i) {
      const RE::TESTopicInfo* candidate = topic.topicInfos[i];
      if (candidate && candidate->formID == infoFormId) {
        listed = true;
      }
    }
  }
  const auto* mapped = RE::TESForm::LookupByID<RE::TESTopicInfo>(infoFormId);
  const QuestInfoCount count = CountQuestInfos(*quest);
  const std::string said = fmt::format(
    "[verb:npc-stream] say {:x}: topic {:x} had 0 infos on this game, its "
    "quest {:x} stopped; loaded its dialogue: {} infos in {:.1f} ms (the "
    "line's info {:x} {} the form map and {} the topic's array; the quest's "
    "{} topic(s) list {} info(s), {} naming another topic their parent; "
    "tracker #123)",
    actorFormId, topic.formID, quest->formID, topic.numTopicInfos, ms,
    infoFormId, mapped ? "in" : "not in", listed ? "in" : "not in",
    count.topics, count.infos, count.strays);
  if (count.strays != 0) {
    logger::error("{}", said);
  } else {
    logger::info("{}", said);
  }
}

// an ask gone to the main thread: wanted is gameThrQ's guard passed (an ask
// there only behind another says no refusal), seq its place in line
struct DialogueAsk
{
  uint32_t actorFormId = 0;
  uint32_t infoFormId = 0;
  uint32_t topicFormId = 0;
  bool wanted = false;
  uint64_t seq = 0;
};

// an ask's say on gameThrQ: the ask's count dropped just before it, so an
// ask that thread takes after it sees it gone and one before it follows it
// through the main thread; its place in line given up once the say has run,
// a throw included, so no later ask's load runs on the main thread beside
// it (thuum tracker #123; CommonLibSSE-NG SKSE/Impl/PCH.h:156 scope_exit)
void SayInLine(uint64_t seq, const std::function<void()>& speak)
{
  g_dialogueLoadsQueued.fetch_sub(1);
  const stl::scope_exit leave{ [seq]() noexcept {
    std::lock_guard<std::mutex> lock(g_dialogueLoadMutex);
    g_dialogueAsksInLine.erase(seq);
  } };
  speak();
}

// the say posted back to gameThrQ
void PostSay(uint64_t seq, const std::function<void()>& speak)
{
  g_nativeCallRequirements.gameThrQ->AddTask(
    [seq, speak](Viet::Void) { SayInLine(seq, speak); });
}

// the main thread's task: the load when the line needs one, then the say
// posted back to gameThrQ with no second wait (the ask's answer, still -1,
// written there). The ask waits instead, asked again next tick, while a
// quest's promote task is in flight and its line needs a load, or while an
// earlier ask's say has not run; past kDialogueLoadMaxTicks it runs, with no
// load while either holds
void RunDialogueAsk(const DialogueAsk& ask, uint32_t tick,
                    const std::function<void()>& speak)
{
  try {
    const char* refusal = nullptr;
    RE::TESTopic* topic =
      TopicToLoad(ask.infoFormId, ask.topicFormId, refusal);
    const RE::TESQuest* promoting = topic ? PromotingQuest() : nullptr;
    bool earlier = false;
    {
      std::lock_guard<std::mutex> lock(g_dialogueLoadMutex);
      earlier = !g_dialogueAsksInLine.empty() &&
        *g_dialogueAsksInLine.begin() < ask.seq;
    }
    if ((promoting || earlier) && tick < kDialogueLoadMaxTicks) {
      if (tick == 0 && promoting) {
        logger::info("[verb:npc-stream] say {:x}: topic {:x} waits on the "
                     "main thread: quest {:x}'s promote task is in flight; "
                     "asked again each tick up to {} (tracker #123)",
                     ask.actorFormId, ask.topicFormId, promoting->formID,
                     kDialogueLoadMaxTicks);
      } else if (tick == 0) {
        logger::info("[verb:npc-stream] say {:x}: topic {:x} waits on the "
                     "main thread behind an earlier line; asked again each "
                     "tick up to {} (tracker #123)",
                     ask.actorFormId, ask.topicFormId, kDialogueLoadMaxTicks);
      }
      // Viet::TaskQueue::Update moves its tasks out before it runs them
      // (viet/include/TaskQueue.h), so this runs on gameThrQ's next drain
      // (WeaponAnimGuard.cpp's route), and from there the main thread's;
      // an earlier ask's say, posted before this, runs on that drain first
      g_nativeCallRequirements.gameThrQ->AddTask(
        [ask, tick, speak](Viet::Void) {
          const auto* tasks = SKSE::GetTaskInterface();
          if (!tasks) {
            SayInLine(ask.seq, speak);
            return;
          }
          tasks->AddUITask(
            [ask, tick, speak]() { RunDialogueAsk(ask, tick + 1, speak); });
        });
      return;
    }
    if (topic && (promoting || earlier)) {
      // past the bound: no load beside the engine's own or an earlier
      // line's say (thuum tracker #123)
      const std::string why = promoting
        ? fmt::format("quest {:x}'s promote task still in flight",
                      promoting->formID)
        : std::string("an earlier line's say has not run");
      logger::warn("[verb:npc-stream] say {:x}: topic {:x}'s dialogue left "
                   "unloaded: {} after {} tick(s); the line is said without "
                   "it (tracker #123)",
                   ask.actorFormId, ask.topicFormId, why, tick);
    } else {
      if (tick > 0) {
        logger::info("[verb:npc-stream] say {:x}: topic {:x} ran on the "
                     "main thread after {} tick(s) of waiting (tracker #123)",
                     ask.actorFormId, ask.topicFormId, tick);
      }
      if (topic) {
        LoadDialogueForLine(ask.actorFormId, ask.infoFormId, *topic);
      } else if (refusal && ask.wanted) {
        logger::info("[verb:npc-stream] say {:x}: topic {:x}'s dialogue not "
                     "loaded on the main thread: {} ({}; tracker #123)",
                     ask.actorFormId, ask.topicFormId, refusal,
                     DescribeTopicHere(RE::TESForm::LookupByID<RE::TESTopic>(
                       ask.topicFormId)));
      }
    }
  } catch (const std::exception& e) {
    logger::error("[verb:npc-stream] say {:x}: the dialogue load for topic "
                  "{:x} threw on the main thread: {} (tracker #123)",
                  ask.actorFormId, ask.topicFormId, e.what());
  } catch (...) {
    logger::error("[verb:npc-stream] say {:x}: the dialogue load for topic "
                  "{:x} threw on the main thread: not a std::exception "
                  "(tracker #123)",
                  ask.actorFormId, ask.topicFormId);
  }
  // here on a run and on a throw, before or after the wait's queue: the say
  // goes back, the ask holding its place in line until it has run there
  PostSay(ask.seq, speak);
}

// gameThrQ's half: true when the say goes to the main thread first (an
// actor with a process here, and an ask already there or the line's info
// not in the form map and its topic passing the guard, never found empty
// here); the ask's answer stays -1 until the say posted back writes it
// (voiceService.ts waits answerMs, 2000 ms); false when the say runs now
bool QueueDialogueLoad(uint32_t actorFormId, uint32_t infoFormId,
                       uint32_t topicFormId,
                       const std::function<void()>& speak)
{
  if (topicFormId == 0) {
    return false;
  }
  auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
  if (!actor || !actor->GetActorRuntimeData().currentProcess) {
    return false;
  }
  // an ask already on the main thread: this one follows it, whatever its
  // line (the count drops on this thread, SayInLine)
  const bool behind = g_dialogueLoadsQueued.load() != 0;
  if (!behind) {
    const char* refusal = nullptr;
    if (!TopicToLoad(infoFormId, topicFormId, refusal)) {
      return false;
    }
  }
  const auto* tasks = SKSE::GetTaskInterface();
  if (!tasks) {
    static bool said = false;
    if (!said) {
      said = true;
      logger::warn("[verb:npc-stream] say {:x}: no SKSE task interface, so "
                   "a stopped quest's dialogue is not loaded (tracker #123)",
                   actorFormId);
    }
    return false;
  }
  DialogueAsk ask;
  ask.actorFormId = actorFormId;
  ask.infoFormId = infoFormId;
  ask.topicFormId = topicFormId;
  ask.wanted = !behind;
  {
    std::lock_guard<std::mutex> lock(g_dialogueLoadMutex);
    ask.seq = ++g_dialogueAskSeq;
    g_dialogueAsksInLine.insert(ask.seq);
  }
  g_dialogueLoadsQueued.fetch_add(1);
  tasks->AddUITask([ask, speak]() { RunDialogueAsk(ask, 0, speak); });
  return true;
}
}

Napi::Value PuppetApi::SayTopicInfo(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto infoFormId =
    NapiHelper::ExtractUInt32(info[1], "topicInfoFormId");
  // thuum tracker #123: the relayed line's topic (ActorSpeech's topic), the
  // key when the info does not resolve on this game; 0 when absent (a
  // two-argument caller)
  const uint32_t topicFormId =
    info.Length() >= 3 && !info[2].IsUndefined() && !info[2].IsNull()
    ? NapiHelper::ExtractUInt32(info[2], "topicFormId")
    : 0u;
  if (actorFormId == 0 || actorFormId == 0x14 || infoFormId == 0) {
    return Napi::Boolean::New(info.Env(), false);
  }
  {
    std::lock_guard<std::mutex> lock(g_sayResultMutex);
    g_sayResult[actorFormId] = -1;
  }
  // the forms looked up on the game thread alone (setPuppet's reason): the
  // say, run by the task below or posted back by the main thread after a
  // dialogue load (#123)
  const auto speak = [actorFormId, infoFormId, topicFormId]() {
    int result = 0;
    auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
    auto* processHere =
      actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
    // #123: the line through the form map, else the relayed topic (an info
    // found in its array, or none: the engine's pick below). With no actor
    // or no process here nothing is said (the answer 0) and no topic is
    // read: such a say runs at once on gameThrQ even while an ask's load
    // grows a topic's array on the main thread (QueueDialogueLoad; thuum
    // tracker #123, the docs review of e7e9da677)
    const SayLine line =
      processHere ? ResolveSayLine(infoFormId, topicFormId) : SayLine{};
    auto* topicInfo = line.info;
    auto* topic = line.topic;
    if (actor && processHere && !topic) {
      // no topic for the line here: the lab's read names what resolved
      // (#123)
      result = 6;
      logger::info("[verb:npc-stream] sayTopicInfo on {:x}: info {:x} topic "
                   "{:x} -> unresolved (no topic here: the info {}, the "
                   "relayed topic {})",
                   actorFormId, infoFormId, topicFormId,
                   topicInfo ? "found with no parent" : "not found",
                   topicFormId ? "not found" : "not given");
    }
    if (actor && topic) {
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
        const bool repeat = topicInfo && highBefore &&
          highBefore->greetingPlayer && highBefore->greetTopic &&
          highBefore->greetTopic->info == topicInfo;
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
          // thuum tracker #123: the line in flight ends here, not by the
          // engine's END, so the spoken-line read gets a CUT row for its
          // item (read before the stop and the clear)
          Frida::RecordSpokenLine(actor, highBefore->greetTopic.get(),
                                  Frida::kSpokenCut, false);
          if (highBefore->soundHandles[0].IsValid()) {
            highBefore->soundHandles[0].Stop();
          }
          REL::Relocation<void (*)(RE::AIProcess*)> clearGreetBefore{
            Offsets::Voice::ClearGreetTopic
          };
          clearGreetBefore(runtime.currentProcess);
          // the stopped line's END never runs either: its done bits restored
          // so this say passes the busy test (the same reason as the
          // inaudible branch below)
          runtime.boolBits.set(RE::Actor::BOOL_BITS::kSoundFileDone,
                               RE::Actor::BOOL_BITS::kVoiceFileDone);
          precleared = true;
        }
        // #123: with no info resolved here the say carries the relayed topic
        // and a null info, Papyrus Say's shape (the re-analyst's read,
        // docs/research/m3/re-puppet-head-lips.md A: 56220 calls 39162 with
        // (process, actor, topic, null, ...) and 39642 builds the item for
        // the topic), so the engine picks the info; the item it builds is
        // told from the one in place before the call. The stage's run
        // 20261118-031740 read that the pick loads nothing (a topic with no
        // info built no item, result 6), so a stopped quest's line has its
        // dialogue loaded first (QueueDialogueLoad above) and this is the
        // fallback for a topic the load left empty
        auto* itemBefore = highBefore ? highBefore->greetTopic.get() : nullptr;
        const bool returned = repeat
          ? false
          : say(runtime.currentProcess, actor, topic, topicInfo, false, false,
                false, false);
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
        // #123: the info the engine's pick took (no info given), read from
        // the item the say built (HighProcessData.h:298, DialogueItem.h:56)
        auto* itemAfter = high ? high->greetTopic.get() : nullptr;
        const RE::TESTopicInfo* picked =
          !topicInfo && itemAfter && itemAfter != itemBefore ? itemAfter->info
                                                             : nullptr;
        bool cleared = false;
        if (!topicInfo && !picked) {
          // the engine built no item for the topic: no line here
          result = 6;
        } else if (!topicInfo && picked->formID != infoFormId) {
          // another info of the topic: the host's line is not this one, so
          // it is stopped and its item cleared the pre-clear's way above
          result = 7;
          // thuum tracker #123: the picked line's item is cleared here by
          // ClearGreetTopic alone, which sends no END (the END paths 37544
          // and 37741 send it, then call the clear: thuum docs/research/m3/
          // re-puppet-head-lips.md:119-123): a CUT row for the spoken-line
          // read, or the BEGIN its say raised reads as voiced until the
          // speaker's next row (itemAfter is the picked item, read before
          // the stop and the clear)
          Frida::RecordSpokenLine(actor, itemAfter, Frida::kSpokenCut, false);
          if (high && high->soundHandles[0].IsValid()) {
            high->soundHandles[0].Stop();
          }
          REL::Relocation<void (*)(RE::AIProcess*)> clearPicked{
            Offsets::Voice::ClearGreetTopic
          };
          clearPicked(runtime.currentProcess);
          runtime.boolBits.set(RE::Actor::BOOL_BITS::kSoundFileDone,
                               RE::Actor::BOOL_BITS::kVoiceFileDone);
          cleared = true;
        } else if (stateAfter == 3) {
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
            // thuum tracker #123: the started item is cleared here, never
            // reaching the engine's END: a CUT row for the spoken-line read
            // (read before the clear)
            Frida::RecordSpokenLine(actor, high->greetTopic.get(),
                                    Frida::kSpokenCut, false);
            REL::Relocation<void (*)(RE::AIProcess*)> clearGreet{
              Offsets::Voice::ClearGreetTopic
            };
            clearGreet(runtime.currentProcess);
            cleared = true;
          }
          // The engine's END restores the done bits; an inaudible start never
          // reaches it: the voice-file start (39772) clears kVoiceFileDone
          // after SpeakSoundFunction returned at its audibility gate with the
          // timer at -1, and the item above is cleared by the engine's own
          // ClearGreetTopic, which leaves the bits alone. So the next say on
          // this puppet failed the busy test (37266: both done bits set and
          // the timer at or below 0) and every later line answered "refused":
          // one line heard, the rest never (playtests 43 and 44's F9; Eli,
          // 2026-11-03: voice lines after the first one worked historically
          // for everything but scripted scenes, and a scene's first line is
          // the one most often inaudible, the guest far). Restored here as END
          // would (Actor.h:166-167). HYPOTHESIS until say-verdict.js reads a
          // started or played say after an inaudible one on the same puppet.
          runtime.boolBits.set(RE::Actor::BOOL_BITS::kSoundFileDone,
                               RE::Actor::BOOL_BITS::kVoiceFileDone);
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
        // #123: where the line was found here; for one said through the
        // engine's pick, the info it took and what this game holds of the
        // topic (the lab's read of an unresolved line)
        const std::string through = topicInfo
          ? fmt::format("; through {}", line.through)
          : fmt::format("; through {}, picked {:x}; {}", line.through,
                        picked ? picked->formID : 0u,
                        DescribeTopicHere(topic));
        logger::info(
          "[verb:npc-stream] sayTopicInfo on {:x}: info {:x} "
          "topic {:x} -> {} (bits {:x}, timer {:.2f}, state {}, "
          "lip {}, 3D {}, axis distance {:.0f} before; returned "
          "{}, state {}, timer {:.2f}, handle {}, delay {:.2f}, in "
          "flight {}, lip {} after{}{}{})",
          actorFormId, infoFormId, topic->formID,
          result == 1     ? "started"
            : result == 3 ? "played"
            : result == 5 ? "inaudible"
            : result == 4 ? "deferred"
            : result == 6 ? "unresolved"
            : result == 7 ? "another info"
                          : "refused",
          bitsBefore, timerBefore, stateBefore, lipBefore, has3D, axisDistance,
          returned, stateAfter, timerAfter, handleLive ? "live" : "none",
          high ? high->soundDelay : -2.f, inFlight, lipAfter,
          cleared ? ", item cleared" : "",
          precleared ? "; the line in flight stopped first" : "", through);
      }
    }
    std::lock_guard<std::mutex> lock(g_sayResultMutex);
    g_sayResult[actorFormId] = result;
  };
  // #123: a line whose info this game does not hold, of a stopped quest's
  // topic, waits for that quest's dialogue to load on the main thread
  // (QueueDialogueLoad above), which posts the say back to gameThrQ
  g_nativeCallRequirements.gameThrQ->AddTask(
    [actorFormId, infoFormId, topicFormId, speak](Viet::Void) {
      if (!QueueDialogueLoad(actorFormId, infoFormId, topicFormId, speak)) {
        speak();
      }
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
  // thuum docs/verbs/npc-puppet.md, Ownership (ADR-063, ADR-073 decision
  // 1): the running effects whose look the engine draws on the actor and
  // keeps, by base effect form id, ascending, each once (ReadEffectLooks)
  std::vector<uint32_t> effects;
};
// Two tables (thuum tracker #56, 2026-11-15): the seat's own poll
// (HostedStateService, every hosted actor each 500 ms, passing own = true),
// whose table its sends read, and any other reader's (lab-driver's npc-state,
// which passes nothing). A poll replaces only its own table. With one shared
// table, each npc-state on a host left one entry, every other hosted actor's
// gaze and face read null at the seat's next poll and went out as 0, and the
// poll after sent them back: in b-npc-stream-face 20261111-082307 the host c1
// sent its eight residents zeroed at 08:26:30.0 and 32.0 and back at 30.5 and
// 32.5, and the watcher c2 logged "face: 1066df gaze cleared" each time, then
// aimed again. The lab's reader is the default so a deployed lab-driver,
// which calls with no flag, moves to its own table with no redeploy
std::mutex g_gazeMutex;
std::array<std::unordered_map<uint32_t, HostedGazeRead>, 2> g_gaze;

// the table the optional own flag at info[index] names: 1 for the seat's own
// poll (true), 0 for every other reader (absent, undefined or false)
size_t GazeTable(const Napi::CallbackInfo& info, size_t index)
{
  if (info.Length() <= index || info[index].IsUndefined()) {
    return 0;
  }
  return NapiHelper::ExtractBoolean(info[index], "own") ? 1 : 0;
}

// thuum docs/verbs/traps.md "The pillar's side" (x-puzzle-guest-view v1
// 20261104-220217): a named node's rotation on any loaded reference, read on
// the game thread as the gaze is (NetImmerse's Euler natives take out-arrays
// the platform cannot pass, and a BSTriShape on its node's origin moves no
// position when the node turns). pollNodeRotation(refrId, nodeNames) queues
// the read of the reference's 3D (TESObjectREFR::Get3D, NiAVObject::
// GetObjectByName, NiAVObject.h:106; the world and local transforms at :157
// and :158; NiMatrix3::ToEulerAnglesXYZ, NiMatrix3.h:28, radians);
// nodeRotation(refrId) answers the last poll, one row per name
struct NodeRotationRead
{
  std::string node;
  bool has = false;
  float world[3] = { 0.f, 0.f, 0.f };
  float local[3] = { 0.f, 0.f, 0.f };
};
std::mutex g_nodeRotationMutex;
std::unordered_map<uint32_t, std::vector<NodeRotationRead>> g_nodeRotation;

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
    // topic anyway (playtest 37, 03:40Z, "shop worked!"). The call is for
    // its recompute alone; its [[nodiscard]] return is the stale value, so
    // it is dropped on purpose (MSVC C4834 is an error in the client build)
    std::ignore = actor->GetVendorFaction();
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

namespace {
// thuum docs/verbs/npc-puppet.md, Ownership (ADR-063, ADR-073 decision 1):
// the host's engine runs the actor's effects and draws their looks, so the
// host reads which of them a watcher must draw on its puppet. Each entry of
// the actor's active effect list (Actor::AsMagicTarget, CommonLibSSE-NG
// include/RE/A/Actor.h:720; MagicTarget::GetActiveEffectList,
// include/RE/M/MagicTarget.h:84) that is neither inactive (its conditions
// false: ActiveEffect::Flag::kInactive, include/RE/A/ActiveEffect.h:50) nor
// dispelled (kDispelled, :51; the flags at :111), whose base effect
// (GetBaseObject, :91) has a look (EffectSettingData's effectShader, the hit
// shader, include/RE/E/EffectSetting.h:80, or its hitEffectArt, :96) the
// record does not turn off (Flag::kNoHitEffect, :64), and whose look stays
// while the effect runs: a constant effect (castingType, :92;
// MagicSystem::CastingType::kConstantEffect, include/RE/M/MagicSystem.h:33),
// an ability's, or one flagged FX Persist (Flag::kFXPersist, :57). A
// fire-and-forget look without FX Persist flashes once at the effect's start
// and is its cause's (the hit, the relayed cast), never a poll's. What
// kNoHitEffect and kFXPersist do is the Creation Kit's reading of the
// flags: HYPOTHESIS until a probe reads the two looks on a watcher
// (Skyrim.esm's AbFXAtronachFlame 0x60da1, a constant effect with a hit
// effect art and no FX Persist, DATA flags 0x00200802 by lab/esm.py on
// sky-srv, 2026-11-14, is drawn on the host for as long as it runs)
//
// thuum playtest 54 (Eli: "the swirling particle affect was applied to
// ariadne not the corpse"): an effect's look is drawn on its visuals target
// (ActiveEffect::GetVisualsTarget, vfunc 03, include/RE/A/ActiveEffect.h:65:
// the effect's own target's stats object), and a CommandEffect overrides the
// slot (include/RE/C/CommandEffect.h:15) for the actor it commands
// (commandedActor, :27). A raise's ReanimateEffect sits in its CASTER's
// list: c2-console 07:35:41.005 "effects: this seat's own player: 1 running
// with a look (96d0b)", gone at 07:36:43.631, the effect's 60 s; MGEF
// 0x96d0b ReanimateFFAimed25 draws EFSH 0x75272 ReanimateFXShader and ARTO
// 0x75271 ReanimateTargetFX (lab/esm.py). Read as the caster's, every other
// seat drew the swirl on the caster's figure (c1-console 07:35:40.691
// "effects: ff000e42 plays shader 75272", "plays art 75271"). So a look
// drawn on another reference goes to `elsewhere` under that reference's id,
// and the poll gives it to that actor's read (the body, which its commander's
// seat hosts from the grant). HYPOTHESIS until a watcher shows the swirl on
// the body: that the override answers the commanded actor (CommonLib names
// the slot, not its body)
std::vector<uint32_t> ReadEffectLooks(
  RE::Actor& actor,
  std::unordered_map<uint32_t, std::vector<uint32_t>>& elsewhere)
{
  std::vector<uint32_t> out;
  auto* target = actor.AsMagicTarget();
  auto* list = target ? target->GetActiveEffectList() : nullptr;
  if (!list) {
    return out;
  }
  using Flag = RE::ActiveEffect::Flag;
  using DataFlag = RE::EffectSetting::EffectSettingData::Flag;
  for (RE::ActiveEffect* effect : *list) {
    if (!effect || effect->flags.any(Flag::kInactive, Flag::kDispelled)) {
      continue;
    }
    const auto* base = effect->GetBaseObject();
    if (!base || base->data.flags.any(DataFlag::kNoHitEffect)) {
      continue;
    }
    if (!base->data.effectShader && !base->data.hitEffectArt) {
      continue;
    }
    const bool stays = base->data.castingType ==
        RE::MagicSystem::CastingType::kConstantEffect ||
      base->data.flags.any(DataFlag::kFXPersist);
    if (!stays) {
      continue;
    }
    const RE::TESObjectREFR* drawnOn = effect->GetVisualsTarget();
    if (drawnOn && drawnOn != &actor) {
      elsewhere[drawnOn->formID].push_back(base->formID);
      continue;
    }
    out.push_back(base->formID);
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}
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
  const size_t table = GazeTable(info, 1);
  // the engine's fields read on the game thread (setPuppet's reason); a few
  // field reads per hosted actor per poll (rule 9)
  g_nativeCallRequirements.gameThrQ->AddTask([actorIds, table](Viet::Void) {
    std::unordered_map<uint32_t, HostedGazeRead> reads;
    std::unordered_map<uint32_t, std::vector<uint32_t>> elsewhere;
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
      read.effects = ReadEffectLooks(*actor, elsewhere);
      reads[id] = std::move(read);
    }
    // the looks one polled actor's effects draw on another polled actor (a
    // raise's on its body) join that actor's read; a reference this poll
    // does not read is not this seat's to report
    for (auto& [drawnOnId, looks] : elsewhere) {
      const auto it = reads.find(drawnOnId);
      if (it == reads.end()) {
        // a look drawn on a reference this poll does not read (a body before
        // the grant reaches its commander's seat) is said once a (reference,
        // first look), so the lab tells a dropped look from one never
        // raised; at most 1024 said for the session. Said for the seat's own
        // poll only (table 1, every hosted actor, the read its sends carry):
        // another reader's poll (lab-driver's npc-state, one id into table
        // 0) leaves out by design a body the seat's own poll reads, so its
        // line would be false, and under the once-a-key rule it would hide
        // the seat's own drop of that key for the session. One thread: this
        // task runs on the game-thread queue alone. looks is never empty
        // (each entry is made by a push_back in ReadEffectLooks)
        static std::unordered_set<uint64_t> droppedSaid;
        if (table == 1) {
          const uint64_t key =
            (static_cast<uint64_t>(drawnOnId) << 32) | looks.front();
          if (droppedSaid.size() < 1024 && droppedSaid.insert(key).second) {
            logger::info("[verb:npc-puppet] effects drawn on {:x}, not "
                         "polled here: {} look(s), first {:x}, dropped",
                         drawnOnId, looks.size(), looks.front());
          }
        }
        continue;
      }
      auto& effects = it->second.effects;
      effects.insert(effects.end(), looks.begin(), looks.end());
      std::sort(effects.begin(), effects.end());
      effects.erase(std::unique(effects.begin(), effects.end()),
                    effects.end());
    }
    std::lock_guard<std::mutex> lock(g_gazeMutex);
    g_gaze[table].swap(reads);
  });
  return info.Env().Undefined();
}

Napi::Value PuppetApi::PollNodeRotation(const Napi::CallbackInfo& info)
{
  const auto refrId = NapiHelper::ExtractUInt32(info[0], "refrId");
  const auto names = NapiHelper::ExtractArray(info[1], "nodeNames");
  std::vector<std::string> nodeNames;
  const uint32_t n = names.Length();
  nodeNames.reserve(n);
  for (uint32_t i = 0; i < n; ++i) {
    const std::string name = "nodeNames[" + std::to_string(i) + "]";
    nodeNames.push_back(NapiHelper::ExtractString(names.Get(i), name.c_str()));
  }
  g_nativeCallRequirements.gameThrQ->AddTask([refrId, nodeNames](Viet::Void) {
    std::vector<NodeRotationRead> reads;
    reads.reserve(nodeNames.size());
    auto* refr = RE::TESForm::LookupByID<RE::TESObjectREFR>(refrId);
    RE::NiAVObject* root = refr ? refr->Get3D() : nullptr;
    for (const auto& name : nodeNames) {
      NodeRotationRead read;
      read.node = name;
      RE::NiAVObject* node = nullptr;
      if (root) {
        node = name.empty() ? root : root->GetObjectByName(name.c_str());
      }
      if (node) {
        read.has = true;
        node->world.rotate.ToEulerAnglesXYZ(read.world[0], read.world[1],
                                            read.world[2]);
        node->local.rotate.ToEulerAnglesXYZ(read.local[0], read.local[1],
                                            read.local[2]);
      }
      reads.push_back(std::move(read));
    }
    std::lock_guard<std::mutex> lock(g_nodeRotationMutex);
    g_nodeRotation[refrId] = std::move(reads);
  });
  return info.Env().Undefined();
}

Napi::Value PuppetApi::NodeRotation(const Napi::CallbackInfo& info)
{
  const auto refrId = NapiHelper::ExtractUInt32(info[0], "refrId");
  std::vector<NodeRotationRead> reads;
  {
    std::lock_guard<std::mutex> lock(g_nodeRotationMutex);
    const auto it = g_nodeRotation.find(refrId);
    if (it == g_nodeRotation.end()) {
      return info.Env().Null();
    }
    reads = it->second;
  }
  auto res = Napi::Array::New(info.Env(), reads.size());
  for (uint32_t i = 0; i < reads.size(); ++i) {
    const auto& read = reads[i];
    auto row = Napi::Object::New(info.Env());
    row.Set("node", Napi::String::New(info.Env(), read.node));
    row.Set("has", Napi::Boolean::New(info.Env(), read.has));
    auto world = Napi::Array::New(info.Env(), 3);
    auto local = Napi::Array::New(info.Env(), 3);
    for (uint32_t k = 0; k < 3; ++k) {
      world.Set(k, Napi::Number::New(info.Env(), read.world[k]));
      local.Set(k, Napi::Number::New(info.Env(), read.local[k]));
    }
    row.Set("world", world);
    row.Set("local", local);
    res.Set(i, row);
  }
  return res;
}

namespace {
// thuum D-53-8 B (train 53's V-live; lab/.cache/drafts/reads-53/
// stage53-reads.md, train53-greens): the head parts the player's own base
// draws, read on the game thread as the node rotations are. The list
// follows CommonLibSSE-NG's reading of a type's current part, taken over the
// whole list (TESNPC::GetCurrentHeadPartByType, src/RE/T/TESNPC.cpp:94-97:
// the overlay list while HasOverlays answers true, else the base list): the
// overlay list (TESNPC::GetBaseOverlays and GetNumBaseOverlays,
// include/RE/T/TESNPC.h:242 and :247) while TESNPC::HasOverlays (:255)
// answers true and the list holds parts, else the base list
// (TESNPC::headParts and numHeadParts, :285-286). One case departs from
// CommonLib's reading: HasOverlays true over an empty overlay list, where
// GetHeadPartOverlayByType (:111-123) names no part of any type and this
// read takes the base list; its answer says so (hasOverlays true,
// overlayCount 0, overlay false), and what the engine draws then is unread
// (HYPOTHESIS). HasOverlays is called
// through this platform's own replacement of it (FridaHooks.cpp,
// InstallHasOverlaysHook), so the answer is the one the engine's head build
// gets: the engine's own on the player's base while its race carries a head
// part list flag (TESRace.h:77-78), false otherwise. Train 53 read the two
// lists apart after Ariadne's turn (20261115-031800
// steps/184-c2-head-parts.json: her base list kept
// FemaleEyesKhajiitBlueNarrow 0xee878, her overlay list held
// FemaleEyesKhajiitVampire 0xee87f in its place), and her face drew the
// overlay's (steps 186 to 194). HYPOTHESIS (rule 2): that the engine's head
// build reads the list HasOverlays picks, as CommonLib's reading does
struct OwnHeadPartsRead
{
  uint32_t seq = 0;            // reads since launch, this one's
  uint32_t race = 0;           // the base's race (TESNPC::GetRace), 0 for none
  bool flagged = false;        // that race carries kOverlayHeadPartList or
                               // kOverrideHeadPartList (TESRace.h:77-78)
  bool hasOverlays = false;    // TESNPC::HasOverlays's answer
  uint32_t overlayCount = 0;   // TESNPC::GetNumBaseOverlays
  bool overlay = false;        // the parts are the overlay list
  std::vector<uint32_t> parts; // each once, in the list's order
};
std::mutex g_ownHeadPartsMutex;
std::optional<OwnHeadPartsRead> g_ownHeadParts;
uint32_t g_ownHeadPartsSeq = 0; // under the mutex
}

Napi::Value PuppetApi::PollOwnHeadParts(const Napi::CallbackInfo& info)
{
  g_nativeCallRequirements.gameThrQ->AddTask([](Viet::Void) {
    auto* player = RE::PlayerCharacter::GetSingleton();
    RE::TESNPC* base = player ? player->GetActorBase() : nullptr;
    if (!base) {
      return;
    }
    OwnHeadPartsRead read;
    const RE::TESRace* race = base->GetRace();
    read.race = race ? race->formID : 0u;
    read.flagged = race &&
      race->data.flags.any(RE::RACE_DATA::Flag::kOverlayHeadPartList,
                           RE::RACE_DATA::Flag::kOverrideHeadPartList);
    read.hasOverlays = base->HasOverlays();
    read.overlayCount = base->GetNumBaseOverlays();
    RE::BGSHeadPart** overlays =
      read.overlayCount != 0 ? base->GetBaseOverlays() : nullptr;
    read.overlay = read.hasOverlays && overlays != nullptr;
    const auto take = [&read](const RE::BGSHeadPart* part) {
      if (part &&
          std::find(read.parts.begin(), read.parts.end(), part->formID) ==
            read.parts.end()) {
        read.parts.push_back(part->formID);
      }
    };
    if (read.overlay) {
      for (uint32_t i = 0; i < read.overlayCount; ++i) {
        take(overlays[i]);
      }
    } else if (base->headParts) {
      for (std::int8_t i = 0; i < base->numHeadParts; ++i) {
        take(base->headParts[i]);
      }
    }
    std::lock_guard<std::mutex> lock(g_ownHeadPartsMutex);
    read.seq = ++g_ownHeadPartsSeq;
    g_ownHeadParts = std::move(read);
  });
  return info.Env().Undefined();
}

Napi::Value PuppetApi::OwnHeadParts(const Napi::CallbackInfo& info)
{
  OwnHeadPartsRead read;
  {
    std::lock_guard<std::mutex> lock(g_ownHeadPartsMutex);
    if (!g_ownHeadParts) {
      return info.Env().Null();
    }
    read = *g_ownHeadParts;
  }
  auto env = info.Env();
  auto res = Napi::Object::New(env);
  res.Set("seq", Napi::Number::New(env, read.seq));
  res.Set("race", Napi::Number::New(env, read.race));
  res.Set("flagged", Napi::Boolean::New(env, read.flagged));
  res.Set("hasOverlays", Napi::Boolean::New(env, read.hasOverlays));
  res.Set("overlayCount", Napi::Number::New(env, read.overlayCount));
  res.Set("overlay", Napi::Boolean::New(env, read.overlay));
  auto parts = Napi::Array::New(env, read.parts.size());
  for (uint32_t i = 0; i < static_cast<uint32_t>(read.parts.size()); ++i) {
    parts.Set(i, Napi::Number::New(env, read.parts[i]));
  }
  res.Set("parts", parts);
  return res;
}

Napi::Value PuppetApi::HostedGaze(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto& table = g_gaze[GazeTable(info, 1)];
  HostedGazeRead read;
  {
    std::lock_guard<std::mutex> lock(g_gazeMutex);
    const auto it = table.find(actorFormId);
    if (it == table.end()) {
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

Napi::Value PuppetApi::HostedEffects(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto& table = g_gaze[GazeTable(info, 1)];
  std::vector<uint32_t> effects;
  {
    std::lock_guard<std::mutex> lock(g_gazeMutex);
    const auto it = table.find(actorFormId);
    if (it == table.end()) {
      return info.Env().Null();
    }
    effects = it->second.effects;
  }
  auto res = Napi::Array::New(info.Env(), effects.size());
  for (uint32_t i = 0; i < effects.size(); ++i) {
    res.Set(i, Napi::Number::New(info.Env(), effects[i]));
  }
  return res;
}

Napi::Value PuppetApi::SpokenLines(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  // sinceMs: the system clock in ms (Date.now() on the same seat); absent,
  // null, negative or not finite reads as 0, every row
  std::uint64_t sinceMs = 0;
  if (info.Length() >= 2 && !info[1].IsUndefined() && !info[1].IsNull()) {
    const double since = NapiHelper::ExtractDouble(info[1], "sinceMs");
    if (std::isfinite(since) && since > 0.0) {
      sinceMs = static_cast<std::uint64_t>(std::min(since, 1.0e18));
    }
  }
  // a table read under its mutex, no engine call (FridaHooks.cpp)
  const std::vector<Frida::SpokenLine> rows =
    Frida::ReadSpokenLines(actorFormId, sinceMs);
  auto env = info.Env();
  auto res = Napi::Array::New(env, rows.size());
  for (uint32_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    auto cells = Napi::Array::New(env, 8);
    cells.Set(uint32_t(0),
              Napi::Number::New(env, static_cast<double>(row.epochMs)));
    cells.Set(uint32_t(1), Napi::Number::New(env, row.infoId));
    cells.Set(uint32_t(2), Napi::Number::New(env, row.topicId));
    cells.Set(uint32_t(3), Napi::Number::New(env, row.type));
    cells.Set(uint32_t(4), Napi::Number::New(env, row.gated));
    cells.Set(uint32_t(5), Napi::Number::New(env, row.handleValid));
    cells.Set(uint32_t(6), Napi::Number::New(env, row.responseIndex));
    cells.Set(uint32_t(7), Napi::Number::New(env, row.voiceTimerSec));
    res.Set(i, cells);
  }
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

Napi::Value PuppetApi::IsDamageActorReplaced(const Napi::CallbackInfo& info)
{
  return Napi::Boolean::New(info.Env(), Frida::IsDamageActorReplaced());
}

namespace {
// thuum #63: each read load door link by the door's form id (0: no load
// door), bounded; written on the game thread, read on the JS thread
std::mutex g_linkedDoorsMutex;
std::unordered_map<uint32_t, uint32_t> g_linkedDoors;
constexpr size_t kLinkedDoorsMax = 8192;
}

Napi::Value PuppetApi::PollLinkedDoors(const Napi::CallbackInfo& info)
{
  std::vector<uint32_t> ids;
  if (info[0].IsArray()) {
    const auto array = info[0].As<Napi::Array>();
    for (uint32_t i = 0; i < array.Length() && ids.size() < 256; ++i) {
      const Napi::Value v = array.Get(i);
      if (v.IsNumber()) {
        ids.push_back(v.As<Napi::Number>().Uint32Value());
      }
    }
  }
  if (ids.empty()) {
    return info.Env().Undefined();
  }
  g_nativeCallRequirements.gameThrQ->AddTask(
    [ids = std::move(ids)](Viet::Void) {
      std::vector<std::pair<uint32_t, uint32_t>> read;
      read.reserve(ids.size());
      for (const uint32_t id : ids) {
        uint32_t linked = 0;
        if (auto* refr = RE::TESForm::LookupByID<RE::TESObjectREFR>(id)) {
          if (const auto* teleport =
                refr->extraList.GetByType<RE::ExtraTeleport>();
              teleport && teleport->teleportData) {
            if (const auto door = teleport->teleportData->linkedDoor.get()) {
              linked = door->GetFormID();
            }
          }
        }
        read.emplace_back(id, linked);
      }
      std::lock_guard<std::mutex> lock(g_linkedDoorsMutex);
      if (g_linkedDoors.size() + read.size() > kLinkedDoorsMax) {
        g_linkedDoors.clear();
      }
      for (const auto& [id, linked] : read) {
        g_linkedDoors[id] = linked;
      }
    });
  return info.Env().Undefined();
}

namespace {
// thuum #63 and #47: each read cell's grid square and worldspace (world 0:
// an interior), bounded; written on the game thread, read on the JS thread
struct CellGrid
{
  uint32_t world = 0;
  int32_t x = 0;
  int32_t y = 0;
};
std::mutex g_cellGridsMutex;
std::unordered_map<uint32_t, CellGrid> g_cellGrids;
constexpr size_t kCellGridsMax = 4096;
}

Napi::Value PuppetApi::PollCellGrids(const Napi::CallbackInfo& info)
{
  std::vector<uint32_t> ids;
  if (info[0].IsArray()) {
    const auto array = info[0].As<Napi::Array>();
    for (uint32_t i = 0; i < array.Length() && ids.size() < 256; ++i) {
      const Napi::Value v = array.Get(i);
      if (v.IsNumber()) {
        ids.push_back(v.As<Napi::Number>().Uint32Value());
      }
    }
  }
  if (ids.empty()) {
    return info.Env().Undefined();
  }
  g_nativeCallRequirements.gameThrQ->AddTask(
    [ids = std::move(ids)](Viet::Void) {
      std::vector<std::pair<uint32_t, CellGrid>> read;
      read.reserve(ids.size());
      for (const uint32_t id : ids) {
        auto* cell = RE::TESForm::LookupByID<RE::TESObjectCELL>(id);
        if (!cell) {
          continue;
        }
        CellGrid grid;
        if (!cell->IsInteriorCell()) {
          if (const auto* coordinates = cell->GetCoordinates()) {
            // CommonLibSSE-NG keeps the cell's world space in its runtime data
            // (TESObjectCELL.h:222-249)
            const auto* world = cell->GetRuntimeData().worldSpace;
            grid.world = world ? world->GetFormID() : 0;
            grid.x = coordinates->cellX;
            grid.y = coordinates->cellY;
          }
        }
        read.emplace_back(id, grid);
      }
      std::lock_guard<std::mutex> lock(g_cellGridsMutex);
      if (g_cellGrids.size() + read.size() > kCellGridsMax) {
        g_cellGrids.clear();
      }
      for (const auto& [id, grid] : read) {
        g_cellGrids[id] = grid;
      }
    });
  return info.Env().Undefined();
}

Napi::Value PuppetApi::CellGridOf(const Napi::CallbackInfo& info)
{
  const auto id = NapiHelper::ExtractUInt32(info[0], "cellId");
  std::lock_guard<std::mutex> lock(g_cellGridsMutex);
  const auto it = g_cellGrids.find(id);
  if (it == g_cellGrids.end()) {
    return info.Env().Null();
  }
  auto obj = Napi::Object::New(info.Env());
  obj.Set("world", Napi::Number::New(info.Env(), it->second.world));
  if (it->second.world != 0) {
    obj.Set("x", Napi::Number::New(info.Env(), it->second.x));
    obj.Set("y", Napi::Number::New(info.Env(), it->second.y));
  }
  return obj;
}

Napi::Value PuppetApi::LinkedDoorOf(const Napi::CallbackInfo& info)
{
  const auto id = NapiHelper::ExtractUInt32(info[0], "refrId");
  std::lock_guard<std::mutex> lock(g_linkedDoorsMutex);
  const auto it = g_linkedDoors.find(id);
  if (it == g_linkedDoors.end()) {
    return info.Env().Null();
  }
  return Napi::Number::New(info.Env(), it->second);
}

Napi::Value PuppetApi::SayTopicInfoResult(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  std::lock_guard<std::mutex> lock(g_sayResultMutex);
  const auto it = g_sayResult.find(actorFormId);
  return Napi::Number::New(info.Env(),
                           it == g_sayResult.end() ? -1 : it->second);
}
