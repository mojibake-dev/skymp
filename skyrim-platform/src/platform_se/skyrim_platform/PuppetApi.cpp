#include "PuppetApi.h"
#include "ActingScope.h"
#include "CallNativeApi.h"
#include "FridaHooks.h"
#include "Hosted.h"
#include "ItemMenuRefresh.h"
#include "Puppets.h"
#include "QuestCause.h"
#include "ScriptNatives.h"
#include "ScriptState.h"
#include "TrapFamily.h"
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
#include <fmt/format.h>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <xmmintrin.h>

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

// thuum docs/DECISIONS.md ADR-068 rung 5 (the acting scope, ActingScope.h;
// docs/research/rung5-quests-2026-11-09.md 1(c), read 12: an export, not a
// TESModPlatform native, so the message handler can write it): the acting
// player of a request the server relayed, written before skymp5-client
// applies it; setHosted's shape, the JS thread alone, no game call. A stage
// (0 to 65535) is a request the quest stage sink takes at the pair's event,
// arming the quest's scope; a start (-1) or a stop (-2) is requested and
// arms the quest's scope at once, for ActingScope::kArmMs; an entry (-3) is
// a relayed trigger entry's, questId the trigger, taken by
// TESModPlatform.SendTriggerEvent's arm for its server id. figureLocalId is
// the figure this seat holds for the acting player, 0 when it is absent here
// (Game.GetPlayer answers None in the scope then); serverId its actor id as
// the server names it; ttlMs the request's life. False, nothing stored, for
// a quest 0, a stage outside -3 to 65535, a serverId 0, the player's own id
// as the figure, or a ttl outside 1 to ActingScope::kMaxRequestMs. Logged
// for the lab
Napi::Value PuppetApi::SetActingPlayer(const Napi::CallbackInfo& info)
{
  const auto questId = NapiHelper::ExtractUInt32(info[0], "questId");
  const auto stage = NapiHelper::ExtractInt32(info[1], "stage");
  const auto figure = NapiHelper::ExtractUInt32(info[2], "figureLocalId");
  const auto serverId = NapiHelper::ExtractUInt32(info[3], "serverId");
  const auto ttlMs = NapiHelper::ExtractUInt32(info[4], "ttlMs");
  if (questId == 0 || stage < ActingScope::kEntry || stage > 0xFFFF ||
      serverId == 0 || figure == 0x14 || ttlMs == 0 ||
      ttlMs > ActingScope::kMaxRequestMs) {
    return Napi::Boolean::New(info.Env(), false);
  }
  ActingScope::Player player;
  player.figure = figure;
  player.serverId = serverId;
  ActingScope::Request(questId, stage, player, ttlMs,
                       ActingScope::Origin::kRelayed);
  const bool word =
    stage == ActingScope::kStart || stage == ActingScope::kStop;
  if (word) {
    ActingScope::Arm(questId, player, ActingScope::kArmMs);
  }
  const ActingScope::Counts counts = ActingScope::Count();
  logger::info(
    "[verb:quests] setActingPlayer({:x}, {}, {:x}, {:x}, {}): {}; "
    "{} request(s), {} arm(s), {} joined stack(s)",
    questId, stage, figure, serverId, ttlMs,
    word ? "requested and armed"
         : (figure == 0 ? "requested, the figure absent here" : "requested"),
    counts.requests, counts.arms, counts.joined);
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
// thuum ADR-071 rung 2 (the sample v2): a loaded reference's Havok body, by
// the walk CommonLib's own GetCollisionLayer makes (CommonLibSSE-NG
// src/RE/N/NiAVObject.cpp:61-75): the 3D root's collision object
// (include/RE/N/NiAVObject.h:156) as a bhkNiCollisionObject (NiObject.h:61),
// its body (bhkNiCollisionObject.h:56) as a bhkRigidBody (NiObject.h:64), and
// the hkpRigidBody it refers to (bhkRefObject.h:26; hkpRigidBody.h:7, an
// hkpEntity). Both null for a model whose root carries no rigid body
// (HYPOTHESIS: the lab's props, an IronIngot, a Basket01 and a FoodCabbage,
// carry theirs on the root; x-body-read's "body" lines read it)
struct PropBody
{
  RE::bhkRigidBody* rigid = nullptr;
  RE::hkpRigidBody* havok = nullptr;
};

PropBody BodyOf(RE::NiAVObject& root)
{
  PropBody res;
  auto* collision = root.collisionObject
    ? root.collisionObject->AsBhkNiCollisionObject()
    : nullptr;
  auto* rigid =
    collision && collision->body ? collision->body->AsBhkRigidBody() : nullptr;
  if (rigid && rigid->referencedObject) {
    res.rigid = rigid;
    res.havok = static_cast<RE::hkpRigidBody*>(rigid->referencedObject.get());
  }
  return res;
}

// hkpMotion::MotionType's dynamic values (CommonLibSSE-NG
// include/RE/H/hkpMotion.h:26-38): the lab's resting Dynamic props read 2, a
// basket, and 3, an ingot (docs/engine/havok-props.md E1, E13), never 1
bool IsDynamicMotion(RE::hkpMotion::MotionType type)
{
  using Type = RE::hkpMotion::MotionType;
  return type == Type::kDynamic || type == Type::kSphereInertia ||
    type == Type::kBoxInertia || type == Type::kThinBoxInertia;
}

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

struct BodyRead
{
  uint32_t formId = 0;
  bool awake = false;
  uint32_t motion = 0;
  RE::NiPoint3 pos;
  RE::NiPoint3 rot;
  RE::NiPoint3 lin;
  RE::NiPoint3 ang;
};

// one reference's body, false when the reference, its 3D
// (TESObjectREFR::Get3D, T/TESObjectREFR.h:375), its cell's bhkWorld
// (TESObjectCELL::GetbhkWorld, T/TESObjectCELL.h:197) or the rigid body is
// missing. Read on the JS thread under the world's lock taken as a reader
// (bhkWorld::worldLock, B/bhkWorld.h:71; BSReadLockGuard,
// B/BSAtomic.h:110-117). HYPOTHESIS: Havok's step and the node's update from
// the body write under that lock as writers, so a read is never mixed across
// two steps and the island it reads is never one the step frees; nothing in
// CommonLib takes the lock (bhkWorld.h:71 is its one mention), so who holds it
// is the exe's to show. HYPOTHESIS too: no writer of it waits for the JS tick
// while it holds it, setPuppet's deadlock shape (above); x-body-read and
// x-prop-follow read both as no hang and no step spike across reads every
// update. The fields: pos the root's world translate in game units and rot
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
// awake)
bool ReadBody(uint32_t formId, float toGameUnits, BodyRead& out)
{
  auto* refr = RE::TESForm::LookupByID<RE::TESObjectREFR>(formId);
  auto* root = refr ? refr->Get3D() : nullptr;
  auto* cell = refr ? refr->GetParentCell() : nullptr;
  auto* world = cell ? cell->GetbhkWorld() : nullptr;
  if (!root || !world) {
    return false;
  }
  RE::BSReadLockGuard reading{ world->worldLock };
  const PropBody body = BodyOf(*root);
  if (!body.havok) {
    return false;
  }
  const auto& motion = body.havok->motion;
  const auto* island = body.havok->simulationIsland;
  out.formId = formId;
  out.awake = island && island->isInActiveIslandsArray;
  out.motion = motion.type.underlying();
  out.pos = root->world.translate;
  out.rot = EulerDegreesXYZ(root->world.rotate);
  out.lin = Vec3(motion.linearVelocity, toGameUnits);
  out.ang = Vec3(motion.angularVelocity, 1.f);
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
    const bool found =
      formId != 0 && formId != 0x14 && ReadBody(formId, toGameUnits, read);
    if (all) {
      if (found) {
        answered.push_back(read);
      }
      continue;
    }
    std::lock_guard<std::mutex> lock(g_bodyAwakeMutex);
    if (found && read.awake) {
      if (g_bodyAwake.size() > kBodyAwakeMax) {
        g_bodyAwake.clear();
      }
      g_bodyAwake.insert(formId);
      answered.push_back(read);
      continue;
    }
    const bool fell = g_bodyAwake.erase(formId) != 0;
    if (fell && found) {
      answered.push_back(read);
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
// never waits for its task. The task looks the reference up again, walks to
// its rigid body (BodyOf), refuses a motion type that is not dynamic and
// calls CommonLib's bhkRigidBody::SetLinearVelocity (B/bhkRigidBody.h:55;
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
    const PropBody body = BodyOf(*root);
    if (!body.havok) {
      logger::warn("[verb:prop-physics] velocity not set on {:x}: no rigid "
                   "body on its 3D root",
                   formId);
      return;
    }
    const auto type = body.havok->motion.type;
    const uint32_t motion = type.underlying();
    if (!IsDynamicMotion(type.get())) {
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
          // the stopped line's END never runs either: its done bits restored
          // so this say passes the busy test (the same reason as the
          // inaudible branch below)
          runtime.boolBits.set(RE::Actor::BOOL_BITS::kSoundFileDone,
                               RE::Actor::BOOL_BITS::kVoiceFileDone);
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

Napi::Value PuppetApi::IsDamageActorReplaced(const Napi::CallbackInfo& info)
{
  return Napi::Boolean::New(info.Env(), Frida::IsDamageActorReplaced());
}

Napi::Value PuppetApi::SayTopicInfoResult(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  std::lock_guard<std::mutex> lock(g_sayResultMutex);
  const auto it = g_sayResult.find(actorFormId);
  return Napi::Number::New(info.Env(),
                           it == g_sayResult.end() ? -1 : it->second);
}
