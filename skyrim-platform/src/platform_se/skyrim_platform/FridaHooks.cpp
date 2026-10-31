#include "FridaHooks.h"
#include "EventsApi.h"
#include "Fates.h"
#include "FridaHookHandler.h"
#include "FridaHooksUtils.h"
#include "Hosted.h"
#include "JsUtils.h"
#include "MagicApi.h"
#include "Override.h"
#include "PapyrusTESModPlatform.h"
#include "Puppets.h"
#include "SkyrimPlatform.h"
#include "StringHolder.h"

#include <RE/A/AIProcess.h>
#include <RE/A/Actor.h>
#include <RE/A/ActorMagicCaster.h>
#include <RE/A/ActorValues.h>
#include <RE/B/BSTEvent.h>
#include <RE/B/bhkCharProxyController.h>
#include <RE/D/DialogueItem.h>
#include <RE/E/Effect.h>
#include <RE/E/EffectSetting.h>
#include <RE/H/HighProcessData.h>
#include <RE/H/hkpEntity.h>
#include <RE/H/hkpRootCdPoint.h>
#include <RE/T/TESHavokUtilities.h>
#include <RE/T/TESQuestStageEvent.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <fmt/format.h>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

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
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    auto colon = line.find(": ");
    if (colon == std::string::npos) {
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
    "plugin line(s) in Data/Platform/papyrus-scripts.txt",
    g_admittedScripts.size(), plugins);
}

// A script passes if it, or any class it extends, is SkyUI's (SKI_*) or is
// named by an admitted plugin's line. The chain is ObjectTypeInfo's
// parentTypeInfo (CommonLibSSE-NG include/RE/O/ObjectTypeInfo.h:127-128,
// :178); it ends at a null parent, and the walk stops at 16 either way.
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
}

/**
 * Send Event hook
 */

// (VMHandle handle, const BSFixedString& eventName, IFunctionArguments* args)
void OnSendEventEnter(GumInvocationContext* ic)
{
  if (Override::IsOverriden()) {
    return;
  }
  auto handle = (RE::VMHandle)gum_invocation_context_get_nth_argument(ic, 1);
  auto eventName = (char**)gum_invocation_context_get_nth_argument(ic, 2);

  auto vm = VM::GetSingleton();

  uint32_t selfId = 0;

  auto policy = vm->GetObjectHandlePolicy();
  if (policy) {
    if (auto actor =
          policy->GetObjectForHandle(RE::FormType::ActorCharacter, handle)) {
      selfId = actor->GetFormID();
    }
    if (auto refr =
          policy->GetObjectForHandle(RE::FormType::Reference, handle)) {
      selfId = refr->GetFormID();
    }
  }

  auto eventNameStr = std::string(*eventName);
  EventsApi::SendPapyrusEventEnter(selfId, eventNameStr);

  auto blockEvents = TESModPlatform::GetPapyrusEventsBlocked();

  // thuum docs/verbs/push.md (ADR-031; ADR-029's A1, test 1): an event on
  // an effect the player bears, or one the player cast, runs in this game,
  // so a consequence a record's script carries plays where it lands.
  // Unrelenting Force's throw is one: VoicePushEffectScript's OnEffectStart
  // calls Caster.PushActorAway(Target, PushForce). Borne, it throws the
  // player in the player's own game; cast, it throws the target's figure
  // in the shouter's game from the shouter's position, as the target's own
  // game throws it (the movement relay stands down while the figure is
  // down). ActiveEffect's VM type id, its caster and its target:
  // CommonLibSSE-NG include/RE/A/ActiveEffect.h:31, :100 and :104. The
  // target is the actor's MagicTarget base (+0xA0 on 1.6.1170, measured),
  // so it is compared with the player's AsMagicTarget
  // (include/RE/A/Actor.h:720), never through ActiveEffect::GetTargetActor,
  // whose reinterpret_cast skips that base; the caster is a handle
  // (GetCasterActor, src/RE/A/ActiveEffect.cpp:25-28). An effect between two
  // figures stays blanked (A1's bystanders, docs/apocalypse-options.md 6).
  if (blockEvents && policy &&
      policy->HandleIsType(RE::ActiveEffect::VMTYPEID, handle)) {
    auto effect = static_cast<RE::ActiveEffect*>(
      policy->GetObjectForHandle(RE::ActiveEffect::VMTYPEID, handle));
    auto player = RE::PlayerCharacter::GetSingleton();
    if (effect && player &&
        (effect->target == player->AsMagicTarget() ||
         effect->GetCasterActor().get() == player)) {
      blockEvents = false;
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

        if (AdmittedByChain(info)) {
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
}

void OnSendEventLeave(GumInvocationContext* ic)
{
  if (Override::IsOverriden()) {
    return;
  }
  EventsApi::SendPapyrusEventLeave();
}

void InstallSendEventHook()
{
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::SEND_EVENT, Offsets::Hooks::SendEvent.address(),
    std::make_shared<Frida::Hook>(OnSendEventEnter, OnSendEventLeave));
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
void OnSendAnimationEventEnter(GumInvocationContext* ic)
{
  if (Override::IsOverriden()) {
    return;
  }
  auto refr = ic->cpu_context->rcx
    ? (RE::TESObjectREFR*)(ic->cpu_context->rcx - 0x38)
    : nullptr;
  uint32_t formId = refr ? refr->formID : 0;

  constexpr int argIdx = 1;
  auto animEventName =
    (char**)gum_invocation_context_get_nth_argument(ic, argIdx);

  if (!refr || !animEventName)
    return;

  std::string str = *animEventName;
  if (str == "") {
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
  if (Override::IsOverriden()) {
    return;
  }
  bool res = !!gum_invocation_context_get_return_value(ic);
  EventsApi::SendAnimationEventLeave(res);
}

void InstallSendAnimationEventHook()
{
  Frida::HookHandler::GetSingleton()->Install(
    Frida::HookID::HOOK_SEND_ANIMATION_EVENT,
    Offsets::Hooks::SendAnimation.address(),
    std::make_shared<Frida::Hook>(OnSendAnimationEventEnter,
                                  OnSendAnimationEventLeave));
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

bool DamageActorReplacement(RE::Actor* victim, float damage,
                            RE::Actor* attacker, bool killMove)
{
  if (IsFigureVictim(victim)) {
    return false;
  }
  // thuum docs/verbs/npc-fate.md (ADR-050 decision 2, deaths by the engine):
  // a puppet here takes no engine damage (the record's values come as
  // ChangeValues); an NPC this game simulates takes none from the player's
  // or a figure's hand (the server's number arrives as a cause through
  // Frida::DamageActorOriginal, the one number); an NPC's hand on an NPC
  // stays the engine's (the host reports the hit and the absolute cause
  // corrects it). The player's own actor is never cut here.
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
  Frida::HookHandler::GetSingleton()->Replace(
    Offsets::Hooks::DamageActor.address(),
    reinterpret_cast<void*>(&DamageActorReplacement),
    reinterpret_cast<void**>(&g_damageActorOriginal));
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
    return;
  }
  Frida::HookHandler::GetSingleton()->Replace(
    Offsets::Hooks::UseSkill.address(),
    reinterpret_cast<void*>(&UseSkillReplacement),
    reinterpret_cast<void**>(&g_useSkillOriginal));
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
    if (speaker && Puppets::Is(speaker->formID)) {
      auto* process = speaker->GetActorRuntimeData().currentProcess;
      auto* high = process ? process->high : nullptr;
      const bool talking = high && high->talkingToPC;
      if (!talking) {
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
      logger::warn("[verb:quest-stage] fragment of {:x} stage {} entry {} "
                   "skipped: the stage is the world's, applied from the "
                   "relay; {} mark(s) left for the pair",
                   event->formID, event->stage,
                   static_cast<unsigned>(event->unk0E), left);
      FireStageItemFinished(event->finishedCallback);
      return RE::BSEventNotifyControl::kContinue;
    }
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
  if (!Hosted::Is(formId) || Puppets::Is(formId) || actor->IsAIEnabled()) {
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
 * reference. Filtered here (rule 9) to the other collidable's layer in
 * {kClutter 4, kProps 10} (hkpRootCdPoint.h:16 rootCollidableB;
 * hkpCollidable.h:52-55 GetCollisionLayer, collisionFilterInfo & 0x7F;
 * include/RE/C/CollisionLayers.h) and to the player's own controller: the
 * listener's enclosing proxy controller, upcast to its bhkCharacterController
 * base (bhkCharProxyController.h:13; the casts apply the header's layout),
 * against Actor::GetCharController (Actor.h:531; src/RE/A/Actor.cpp:324-328
 * walks currentProcess->middleHigh->charController). The body is the
 * collidable's owner when its broadphase type is an entity
 * (hkpTypedBroadPhaseHandle.h:15; hkpWorldObject.h:39-47 kEntity;
 * hkpCollidable.h:57-66 GetOwner by ownerOffset), its motion type at
 * hkpEntity.h:99 motion, hkpMotion.h:77 type (hkpMotion.h:26-38: 1 Dynamic,
 * 4 Keyframed, 5 Fixed); the reference by
 * TESHavokUtilities::FindCollidableRef (TESHavokUtilities.h:15, bound by
 * CommonLib at Offsets.h:524, Address Library 26003, just addr 2026-10-29).
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
  // the layer first: the ground and the walls are most of the points
  const auto layer = collidable->GetCollisionLayer();
  if (layer != RE::COL_LAYER::kClutter && layer != RE::COL_LAYER::kProps) {
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
  if (collidable->broadPhaseHandle.type !=
      static_cast<std::int8_t>(RE::hkpWorldObject::BroadPhaseType::kEntity)) {
    return;
  }
  auto* body = collidable->GetOwner<RE::hkpEntity>();
  if (!body) {
    return;
  }
  const uint32_t motionType = body->motion.type.underlying();
  auto* refr = RE::TESHavokUtilities::FindCollidableRef(*collidable);
  if (!refr) {
    return;
  }
  const uint32_t refrId = refr->formID;
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

void Frida::InstallHooks()
{
  LoadAdmittedScripts();
  InstallSendEventHook();
  InstallDrawSheatheWeaponPcHook();
  InstallDrawSheatheWeaponActorHook();
  InstallSendAnimationEventHook();
  InstallPerformActionHook();
  InstallProjectileLaunchHook();
  InstallProjectileInitVelocityHook();
  InstallPlaceAreaEffectHook();
  InstallDamageActorHook();
  // thuum docs/verbs/leveling.md: the player's skill uses to JS
  InstallUseSkillHook();
  InstallSendTopicInfoEventHook();
  // thuum docs/verbs/quest-stage.md, slice two: a relayed stage's fragment
  InstallQuestStageSinkHook();
  // thuum docs/verbs/npc-fate.md: a hosted body's reanimation by the engine
  InstallReanimateHook();
  // thuum docs/verbs/prop-physics.md: the player's touch of a prop's body
  InstallPropContactHook();
  InstallQueueNinodeUpdateHook();
  InstallRenderCursorMenuHook();
#ifdef ENABLE_SKYRIM_AE
  InstallApplyMasksToRenderTargetsHook();
#endif
  InstallNativeFunctionCtorHook();
  // thuum docs/verbs/npc-puppet.md: the puppet shape's two detours
  Puppets::InstallHooks();
  // thuum docs/verbs/npc-fate.md: the roll's two detours, inert until
  // setActorFateEnabled(true)
  Fates::InstallHooks();

  logger::info("Frida hooks installed.");
}
