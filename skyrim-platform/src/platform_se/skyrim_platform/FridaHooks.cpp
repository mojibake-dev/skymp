#include "FridaHooks.h"
#include "EventsApi.h"
#include "FridaHookHandler.h"
#include "FridaHooksUtils.h"
#include "JsUtils.h"
#include "MagicApi.h"
#include "Override.h"
#include "PapyrusTESModPlatform.h"
#include "SkyrimPlatform.h"
#include "StringHolder.h"

#include <RE/A/ActorMagicCaster.h>
#include <RE/E/Effect.h>
#include <RE/E/EffectSetting.h>

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
  if (data && player && data->source.get() == player && data->action &&
      (data->flags & 1) == 0) {
    entry.data = data;
    // the variables before the action changes them: the want variable the
    // caster set a few instructions earlier is among them
    entry.variables =
      AnimationGraphMasterBehaviourDescriptor{ *player }.GetVariables();
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
  NoteCastHand(eventName);

  SkyrimPlatform::GetSingleton()->AddTickTask(
    [actionId, idleId, eventName, type, flags, result,
     variables = std::move(entry.variables)](Napi::Env env) {
      auto obj = Napi::Object::New(env);
      auto* player = RE::PlayerCharacter::GetSingleton();
      AddObjProperty(&obj, "actor", static_cast<RE::TESForm*>(player),
                     "Actor");
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
  // filtered in native (rule 9): the player's own launches (the shooter's
  // side of the sync) and the figures' (dynamic forms, the observer's side,
  // which the recorder compares); an NPC's arrows and spells stay out of JS
  const uint32_t shooterId = data->shooter->formID;
  if (shooterId != 0x14 && shooterId < 0xFF000000) {
    return;
  }
  if (shooterId >= 0xFF000000 && data->ammoSource && data->projectileBase &&
      g_relayedArrowDepth == 0) {
    g_droppedArrows.back() = { data, data->projectileBase };
    data->projectileBase = nullptr;
    logger::info("a figure's own arrow dropped: shooter {:x}, ammo {:x}",
                 shooterId, data->ammoSource->formID);
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
 * (an ammo source) whose shooter is the player or a figure (rule 9), raised
 * as projectileFlight on the next tick: the shooter's client sends these
 * angles with the shot, the recorder keeps both games' for the parity check.
 */
void OnProjectileInitVelocityEnter(GumInvocationContext* ic)
{
  auto* projectile = reinterpret_cast<RE::Projectile*>(ic->cpu_context->rcx);
  if (!projectile) {
    return;
  }
  auto& runtime = projectile->GetProjectileRuntimeData();
  if (!runtime.ammoSource) {
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
  seen.ammo = runtime.ammoSource->formID;
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
  InstallQueueNinodeUpdateHook();
  InstallRenderCursorMenuHook();
#ifdef ENABLE_SKYRIM_AE
  InstallApplyMasksToRenderTargetsHook();
#endif
  InstallNativeFunctionCtorHook();

  logger::info("Frida hooks installed.");
}
