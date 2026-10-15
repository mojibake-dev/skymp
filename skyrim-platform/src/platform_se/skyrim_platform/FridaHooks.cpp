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

#include <atomic>
#include <cctype>
#include <chrono>
#include <fmt/format.h>
#include <mutex>
#include <set>
#include <unordered_map>

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

  if (blockEvents && strcmp(*eventName, "OnUpdate") != 0 && vm) {
    vm->attachedScriptsLock.Lock();
    auto it = vm->attachedScripts.find(handle);

    if (it != vm->attachedScripts.end()) {
      auto& scripts = it->second;

      for (size_t i = 0; i < scripts.size(); i++) {
        auto script = scripts[i].get();
        auto info = script->GetTypeInfo();
        auto name = info->GetName();

        const char* skyui_name = "SKI_"; // start skyui object name
        if (strlen(name) >= 4 && name[0] == skyui_name[0] &&
            name[1] == skyui_name[1] && name[2] == skyui_name[2] &&
            name[3] == skyui_name[3]) {
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
 * The graph's answers to a cast request (thuum docs/verbs/cast-action.md).
 * The engine's caster sets the hand's want variable and performs the Attack
 * action only when the graph answers with the BeginCastRight (BeginCastLeft,
 * BeginCastVoice) graph event, handled by the hand's functor:
 * RightHandSpellCastHandler and kin (CommonLibSSE-NG
 * include/RE/Offsets_VTABLE.h:2392-2394), whose ExecuteHandler is the one
 * virtual after the destructor (include/RE/I/IHandlerFunctor.h:16-19), called
 * as (this, Actor&, const BSFixedString& tag). A figure's graph answers the
 * same way to the want variable the view writes (x-cast-gap
 * 20261015-213755: 34 of 34 asks within 7 ms of an answer, all taken), and
 * later while the figure moves, which is where a start one update after its
 * variables was refused (playtest nineteen, about half). The hand is read
 * from the tag when it names one, else it is the handler's. The player's own
 * answers are the engine's business and are not recorded.
 */
std::mutex g_castReadyMutex;
std::unordered_map<uint32_t, uint32_t> g_castReady; // form id -> hand bits

uint32_t HandBitOfTag(const RE::BSFixedString* tag, uint32_t fallback)
{
  const char* s = tag ? tag->c_str() : nullptr;
  if (!s) {
    return fallback;
  }
  std::string lower = s;
  for (auto& c : lower) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (lower.find("right") != std::string::npos) {
    return 2;
  }
  if (lower.find("left") != std::string::npos) {
    return 1;
  }
  if (lower.find("voice") != std::string::npos) {
    return 4;
  }
  return fallback;
}

void NoteCastReady(GumInvocationContext* ic, uint32_t fallbackBit)
{
  auto* actor = reinterpret_cast<RE::Actor*>(ic->cpu_context->rdx);
  if (!actor || actor == RE::PlayerCharacter::GetSingleton()) {
    return;
  }
  const auto* tag =
    reinterpret_cast<const RE::BSFixedString*>(ic->cpu_context->r8);
  const uint32_t bit = HandBitOfTag(tag, fallbackBit);
  {
    std::lock_guard<std::mutex> lock(g_castReadyMutex);
    g_castReady[actor->formID] |= bit;
  }
  logger::info("[verb:cast-action] castReady on {:x}: hand bit {}",
               actor->formID, bit);
}

void OnSpellCastResponseLeftEnter(GumInvocationContext* ic)
{
  NoteCastReady(ic, 1);
}

void OnSpellCastResponseRightEnter(GumInvocationContext* ic)
{
  NoteCastReady(ic, 2);
}

void OnSpellCastResponseVoiceEnter(GumInvocationContext* ic)
{
  NoteCastReady(ic, 4);
}

bool Frida::TakeCastReady(uint32_t actorFormId, int32_t hand)
{
  const uint32_t bit = hand == 0 ? 1 : hand == 1 ? 2 : hand == 2 ? 4 : 0;
  std::lock_guard<std::mutex> lock(g_castReadyMutex);
  auto it = g_castReady.find(actorFormId);
  if (it == g_castReady.end() || (it->second & bit) == 0) {
    return false;
  }
  it->second &= ~bit;
  return true;
}

void InstallSpellCastResponseHooks()
{
  if (!REL::Module::IsAE()) {
    return;
  }
  // ExecuteHandler: slot 1 of each handler's vtable (the destructor is 0)
  const auto executeHandlerOf = [](const REL::VariantID& vtable) {
    REL::Relocation<std::uintptr_t> vtbl{ vtable };
    return *reinterpret_cast<std::uintptr_t*>(vtbl.address() +
                                              sizeof(std::uintptr_t));
  };
  struct Entry
  {
    Frida::HookID id;
    const REL::VariantID& vtable;
    void (*enter)(GumInvocationContext*);
  };
  const Entry entries[] = {
    { Frida::HookID::HOOK_SPELL_CAST_RESPONSE_LEFT,
      RE::VTABLE_LeftHandSpellCastHandler[0], OnSpellCastResponseLeftEnter },
    { Frida::HookID::HOOK_SPELL_CAST_RESPONSE_RIGHT,
      RE::VTABLE_RightHandSpellCastHandler[0], OnSpellCastResponseRightEnter },
    { Frida::HookID::HOOK_SPELL_CAST_RESPONSE_VOICE,
      RE::VTABLE_VoiceSpellCastHandler[0], OnSpellCastResponseVoiceEnter },
  };
  // three handlers may share one implementation; one hook per address, the
  // tag then names the hand
  std::set<std::uintptr_t> hooked;
  for (const auto& e : entries) {
    const auto address = executeHandlerOf(e.vtable);
    if (!address || !hooked.insert(address).second) {
      continue;
    }
    Frida::HookHandler::GetSingleton()->Install(
      e.id, address, std::make_shared<Frida::Hook>(e.enter, nullptr));
    logger::info("[verb:cast-action] spell cast response hook at {:x}",
                 address);
  }
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
  InstallSendEventHook();
  InstallDrawSheatheWeaponPcHook();
  InstallDrawSheatheWeaponActorHook();
  InstallSendAnimationEventHook();
  InstallPerformActionHook();
  InstallSpellCastResponseHooks();
  InstallQueueNinodeUpdateHook();
  InstallRenderCursorMenuHook();
#ifdef ENABLE_SKYRIM_AE
  InstallApplyMasksToRenderTargetsHook();
#endif
  InstallNativeFunctionCtorHook();

  logger::info("Frida hooks installed.");
}
