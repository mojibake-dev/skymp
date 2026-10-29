#include "MagicApi.h"
#include "CallNativeApi.h"
#include "FridaHooks.h"
#include "JsUtils.h"
#include "SkyrimPlatform.h"
#include "game/Offsets.h"

#include "Magic/AnimationGraphMasterBehaviourDescriptor.h"

#include <RE/B/BSTArray.h>
#include <RE/E/Effect.h>
#include <RE/E/EffectSetting.h>
#include <RE/F/FixedStrings.h>
#include <RE/M/MagicItemDataCollector.h>
#include <RE/T/TES.h>
#include <RE/T/TESObjectCELL.h>
#include <mutex>
#include <optional>
#include <unordered_map>

extern CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

namespace skymp::magic::details {

AnimationGraphMasterBehaviourDescriptor::AnimationVariables
GetAnimationVariablesFromJSArg(const Napi::Object& argObj)
{
  using AnimVarInitializer =
    AnimationGraphMasterBehaviourDescriptor::AnimationVariables::InitData;

  auto booleanVarsValue = NapiHelper::ExtractUInt8Array(
    argObj.Get("booleans"), "animationVariables.booleans");

  const auto booleanVars =
    AnimVarInitializer{ static_cast<uint8_t*>(booleanVarsValue.Data()),
                        booleanVarsValue.ByteLength() };

  auto floatsVarsValue = NapiHelper::ExtractUInt8Array(
    argObj.Get("floats"), "animationVariables.floats");

  const auto floatsVars =
    AnimVarInitializer{ static_cast<uint8_t*>(floatsVarsValue.Data()),
                        floatsVarsValue.ByteLength() };

  auto integersVarsValue = NapiHelper::ExtractUInt8Array(
    argObj.Get("integers"), "animationVariables.integers");

  const auto integersVars =
    AnimVarInitializer{ static_cast<uint8_t*>(integersVarsValue.Data()),
                        integersVarsValue.ByteLength() };

  const auto variables =
    AnimationGraphMasterBehaviourDescriptor::AnimationVariables{
      booleanVars, floatsVars, integersVars
    };

  return variables;
}

// thuum docs/verbs/spell-cast.md: a relayed cast's launch, where the
// caster's engine launched the spell and with which angles (pitch x and
// heading z, radians)
struct RelayedLaunch
{
  RE::NiPoint3 origin;
  RE::Projectile::ProjectileRot angles;
};

// The launch a relayed cast carries, as the caster's engine made it
// (Projectile::Launch's LaunchData, read in the caster's game by its
// projectileLaunched event): castSpellImmediate's arguments from `first` on,
// hasLaunch, originX, originY, originZ, launchAngleX and launchAngleZ. None
// when a caller passes fewer, when hasLaunch is false, or when a number is
// not finite
std::optional<RelayedLaunch> GetLaunchFromJSArgs(
  const Napi::CallbackInfo& info, size_t first)
{
  if (info.Length() < first + 6 ||
      !NapiHelper::ExtractBoolean(info[first], "hasLaunch")) {
    return std::nullopt;
  }
  const RelayedLaunch launch{
    RE::NiPoint3{ NapiHelper::ExtractFloat(info[first + 1], "originX"),
                  NapiHelper::ExtractFloat(info[first + 2], "originY"),
                  NapiHelper::ExtractFloat(info[first + 3], "originZ") },
    RE::Projectile::ProjectileRot{
      NapiHelper::ExtractFloat(info[first + 4], "launchAngleX"),
      NapiHelper::ExtractFloat(info[first + 5], "launchAngleZ") }
  };
  if (!std::isfinite(launch.origin.x) || !std::isfinite(launch.origin.y) ||
      !std::isfinite(launch.origin.z) || !std::isfinite(launch.angles.x) ||
      !std::isfinite(launch.angles.z)) {
    return std::nullopt;
  }
  return launch;
}

// thuum docs/verbs/cast-action.md: a BGSAction performed on an actor with
// the event and idle the caster's engine chose, through the mediator's entry
// that performs a TESActionData as given (game/Offsets.h Actions; read on
// sky-re 2026-10-14, HYPOTHESIS until the lab's trace: it sends the output's
// event to the actor's graph and keeps the engine's books for it, with no
// validation and no idle selection, where the first entry would choose the
// idle again from this actor's own conditions). The data is CommonLibSSE-NG's
// (include/RE/T/TESActionData.h: ActionInput's source, target, action and
// unk20, ActionOutput's animEvent and animObjIdle), built on this stack as
// the engine's own SourceActionMap::DoAction builds its, the vtable in place
// for the retry the entry makes through it, its references released below.
// The master-behavior variables the graph needs first are the caller's
// (applyAnimationVariablesToActor, before this call).
bool PerformActionOnActor(RE::Actor& actor, RE::BGSAction& action,
                          RE::TESIdleForm* idle, const std::string& eventName,
                          uint32_t type)
{
  // AE only (the ids are 1.6.1170's); an empty event would send an empty
  // event to the graph, since this entry selects none
  if (!REL::Module::IsAE() || eventName.empty()) {
    return false;
  }
  using PerformChosen_t = uint8_t (*)(void*, RE::TESActionData*);
  REL::Relocation<PerformChosen_t> performChosen{
    Offsets::Actions::PerformChosenAction
  };
  REL::Relocation<void**> mediator{ Offsets::Actions::ActorMediatorSingleton };
  void* self = *mediator;
  if (!self) {
    return false;
  }

  alignas(16) std::uint8_t storage[sizeof(RE::TESActionData)] = {};
  auto* data = reinterpret_cast<RE::TESActionData*>(storage);
  RE::stl::emplace_vtable<RE::TESActionData>(data);
  data->source = RE::NiPointer<RE::TESObjectREFR>(&actor);
  data->target = RE::NiPointer<RE::TESObjectREFR>(nullptr);
  data->action = &action;
  data->unk20 = type;
  data->animEvent = RE::BSFixedString(eventName.c_str());
  data->animObjIdle = idle;

  const uint8_t taken = performChosen(self, data);

  logger::info("[verb:cast-action] performAction on {:x}: action {:x} idle "
               "{:x} event '{}' type {} -> taken {} result {}",
               actor.formID, action.formID, idle ? idle->formID : 0, eventName,
               type, taken, data->result);
  data->source.reset();
  data->target.reset();
  data->animEvent = RE::BSFixedString();
  data->targetAnimEvent = RE::BSFixedString();
  return taken != 0;
}

// Whether the actor's active graph is the humanoid master behaviour's, whose
// variables the descriptor reads and writes: its own set reads non-empty
// (AnimationGraphMasterBehaviourDescriptor admits that project alone). An
// actor whose 3D is not loaded reads empty too and keeps the old refusal:
// only a loaded creature's graph is the case the cast goes on without
bool HasMasterGraph(const RE::Actor& actor)
{
  if (!actor.Is3DLoaded()) {
    return true;
  }
  return !AnimationGraphMasterBehaviourDescriptor{ actor }
            .GetVariables()
            .IsEmpty();
}

} // namespace skymp::magic::details

Napi::Value MagicApi::CastSpellImmediate(const Napi::CallbackInfo& info)
{
  const auto actorFormId =
    NapiHelper::ExtractUInt32(info[0], "actorCasterFormId");

  const auto castingSource = static_cast<RE::MagicSystem::CastingSource>(
    NapiHelper::ExtractInt32(info[1], "castingSource"));

  const auto spellFormId = NapiHelper::ExtractUInt32(info[2], "formIdSpell");

  const auto magicTargetFormId =
    NapiHelper::ExtractUInt32(info[3], "formIdTarget");

  const auto aimAngle = NapiHelper::ExtractFloat(info[4], "aimAngle");
  const auto aimHeading = NapiHelper::ExtractFloat(info[5], "aimHeading");
  const RE::Projectile::ProjectileRot projectileAngles{ aimAngle, aimHeading };

  // thuum docs/verbs/spell-cast.md: the caster's own launch, after the
  // animation variables, when the cast carries one
  const auto launch = skymp::magic::details::GetLaunchFromJSArgs(info, 7);

  g_nativeCallRequirements.gameThrQ->AddTask(
    [spellFormId, actorFormId, castingSource, magicTargetFormId,
     projectileAngles, launch,

     animVars = skymp::magic::details::GetAnimationVariablesFromJSArg(
       NapiHelper::ExtractObject(info[6], "animationVariables"))](Viet::Void) {
      auto* pSpell = RE::TESForm::LookupByID<RE::SpellItem>(spellFormId);

      auto* pActor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);

      if (!pSpell || !pActor) {
        return;
      }

      const auto t = pSpell->GetFormType();

      const bool isValidSpellType = t == RE::FormType::Spell ||
        t == RE::FormType::Scroll || t == RE::FormType::Ingredient ||
        t == RE::FormType::AlchemyItem || t == RE::FormType::Enchantment;

      if (!isValidSpellType) {
        return;
      }

      // thuum docs/verbs/npc-stream.md (train 53; playtest 37's flame
      // atronach): the variables are the humanoid master behaviour's; a
      // creature puppet's graph (its project not DefaultMale or
      // DefaultFemale) takes none and gave none, so the relayed cast
      // returned here and drew nothing on the observer. On such a graph,
      // once its 3D is loaded, the cast goes without them: the engine's own
      // caster needs no graph variable to launch (HYPOTHESIS until a lab run
      // reads the bolt on the observer); a humanoid's keeps the apply and
      // its refusal
      if (skymp::magic::details::HasMasterGraph(*pActor)) {
        const bool isAnimationVariablesApplied =
          AnimationGraphMasterBehaviourDescriptor{ std::move(animVars) }
            .ApplyVariablesToActor(*pActor);
        if (!isAnimationVariablesApplied) {
          return;
        }
      } else {
        logger::info("[verb:npc-stream] cast {:x} on {:x} without graph "
                     "variables: its graph is not the master behaviour's",
                     spellFormId, actorFormId);
      }

      const auto magicCaster = pActor->GetMagicCaster(castingSource);

      if (!magicCaster) {
        return;
      }

      auto* magicTarget =
        RE::TESForm::LookupByID<RE::TESObjectREFR>(magicTargetFormId);

      // thuum docs/verbs/magic-effects.md: a spell on its caster (Self
      // delivery, CommonLibSSE-NG MagicSystem.h:41) is cast on the actor
      // itself, so its look shows on that actor in this game, as it did on
      // the caster in the caster's (x-self-visuals: Invisibility, Oakflesh
      // and Candlelight drew nothing on the figure, the cast's target being
      // none); what it does to values stays the server's
      if (pSpell->data.delivery == RE::MagicSystem::Delivery::kSelf) {
        magicCaster->CastSpellImmediate(pSpell, false, pActor, 1.0f, false,
                                        0.0f, pActor);
        return;
      }

      if (pSpell->GetCastingType() ==
          RE::MagicSystem::CastingType::kConcentration) {
        // thuum docs/verbs/spell-cast.md (Apocalypse's Scattershock, playtest
        // twenty-one: "the particle effect is running the wrong way on
        // player 2's screen"): an aimed stream's missiles are launched at
        // the caster's desired target whenever it resolves and the caster is
        // no player outside combat (MagicCaster's launch, ID 34450, the
        // re-analyst on sky-re, 2026-10-16), and the relayed target is the
        // figure itself, so each missile left for the figure's own chest.
        // An aimed stream goes with no target and the caster's desired
        // target cleared (CommonLibSSE-NG include/RE/M/MagicCaster.h:87), so
        // its missiles follow the figure's aim, which SetActorAimPitch sets
        // every update; a flame or a beam re-aims from that aim every frame
        // either way. HYPOTHESIS until x-stream-aim
        const bool aimed =
          pSpell->data.delivery == RE::MagicSystem::Delivery::kAimed;
        if (aimed) {
          magicCaster->desiredTarget.reset();
        }
        magicCaster->CastSpellImmediate(pSpell, false,
                                        aimed ? nullptr : magicTarget, 1.0f,
                                        false, 0.0f, pActor);

        return;
      }

      RE::ProjectileHandle pProjectile{};

      const auto magicNode = magicCaster->GetMagicNode();

      RE::NiPoint3 origin =
        magicNode ? magicNode->world.translate : pActor->GetPosition();

      if (!magicNode) {
        const auto boundMax = pActor->GetBoundMax();
        const auto boundMin = pActor->GetBoundMin();
        origin.z += (boundMax.z - boundMin.z) * 0.7f;
      }

      RE::Projectile::ProjectileRot angles = projectileAngles;

      if (launch) {
        // the caster's own launch, when the cast carries one, for any
        // projectile (thuum docs/verbs/spell-cast.md, train-31's aim parity
        // row; playtest twenty-eight: a bolt landed in different places on
        // the two screens while the arrows landed alike): its origin, the
        // caster's own magic node, which the lab measured about 65 units
        // from this figure's third-person node at equal angles
        // (docs/research/train-31-reads.md), and the angles its engine
        // launched with, as LaunchArrowAt takes an arrow's. For a
        // target-location cast the launch is the point the caster's game
        // picked along its aim (the re-analyst on sky-re, 2026-10-16): a
        // figure has no aim to pick one with, and the engine's own
        // CastSpellImmediate places no target-location projectile on it
        origin = launch->origin;
        angles = launch->angles;
      } else if (pSpell->data.delivery ==
                 RE::MagicSystem::Delivery::kTargetLocation) {
        // TODO we need recalculate origin, cast ray from head to crosshair
        auto rotation = pActor->Get3D2()->world.rotate.entry;
        auto viewDirection =
          NiPoint3{ rotation[0][1], rotation[1][1], rotation[2][1] };
        viewDirection.Unitize();

        auto offset = viewDirection * 200.f;

        origin.x += offset.x;
        origin.y += offset.y;
        origin.z = pActor->GetPositionZ() + 10.f;
      }

      RE::Projectile::LaunchData launchData(pActor, origin, angles, pSpell);

      // thuum docs/verbs/spell-cast.md, target location (train-29):
      // CommonLib's spell constructor takes the AV effect's projectile
      // (src/RE/P/Projectile.cpp:213-222, GetAVEffect()->data.projectileBase).
      // Lightning Strike's AV effect has none; its lobber (the Grenade PROJ
      // whose explosion places the bolt) sits on its second effect, so the
      // observer's launch recorded projectile 0 and nothing flew
      // (x-lightning-strike 20261019-220431). Take the first effect that
      // carries a projectile (include/RE/E/Effect.h:39 baseEffect,
      // include/RE/E/EffectSetting.h:90 projectileBase); area stays as the
      // constructor set it, the explosion comes from the PROJ when area is 0.
      if (!launchData.projectileBase) {
        for (auto* effect : pSpell->effects) {
          if (effect && effect->baseEffect &&
              effect->baseEffect->data.projectileBase) {
            launchData.projectileBase =
              effect->baseEffect->data.projectileBase;
            break;
          }
        }
      }

      launchData.castingSource = castingSource;
      launchData.desiredTarget = magicTarget;
      launchData.contactNormal = RE::NiPoint3{ 0.f, 0.f, 1.0f };

      RE::Projectile::Launch(&pProjectile, launchData);
    });

  return info.Env().Undefined();
}

Napi::Value MagicApi::InterruptCast(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");

  const auto castingSource = static_cast<RE::MagicSystem::CastingSource>(
    NapiHelper::ExtractInt32(info[1], "castingSource"));

  g_nativeCallRequirements.gameThrQ->AddTask(
    [actorFormId, castingSource,
     animVars = skymp::magic::details::GetAnimationVariablesFromJSArg(
       NapiHelper::ExtractObject(info[2], "animationVariables"))](Viet::Void) {
      const auto pActor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
      if (!pActor) {
        return;
      }

      // a creature puppet's end goes without the master's variables too
      // (CastSpellImmediate's rule above)
      if (skymp::magic::details::HasMasterGraph(*pActor)) {
        const bool isAnimationVariablesApplied =
          AnimationGraphMasterBehaviourDescriptor{ std::move(animVars) }
            .ApplyVariablesToActor(*pActor);
        if (!isAnimationVariablesApplied) {
          return;
        }
      }

      if (auto* caster = pActor->GetMagicCaster(castingSource)) {
        caster->FinishCast();
      } else {
        pActor->InterruptCast(false);
      }
    });

  return info.Env().Undefined();
}

Napi::Value MagicApi::GetAnimationVariablesFromActor(
  const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");

  const auto pActor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);

  if (!pActor) {
    return info.Env().Undefined();
  }

  const auto animVariables =
    AnimationGraphMasterBehaviourDescriptor{ *pActor }.GetVariables();

  return AnimationVariablesToJs(info.Env(), animVariables);
}

Napi::Object MagicApi::AnimationVariablesToJs(
  Napi::Env env,
  const AnimationGraphMasterBehaviourDescriptor::AnimationVariables&
    animVariables)
{
  auto obj = Napi::Object::New(env);

  AddObjProperty(
    &obj, "booleans",
    reinterpret_cast<const uint8_t*>(animVariables.booleans.data()),
    animVariables.SizeBooleansInBytes());

  AddObjProperty(&obj, "floats",
                 reinterpret_cast<const uint8_t*>(animVariables.floats.data()),
                 animVariables.SizeFloatsInBytes());

  AddObjProperty(
    &obj, "integers",
    reinterpret_cast<const uint8_t*>(animVariables.integers.data()),
    animVariables.SizeIntegersInBytes());

  return obj;
}

// thuum docs/verbs/cast-action.md: the figure plays the caster's actions.
// A BGSAction (its form id), the idle the caster's engine chose (a
// TESIdleForm's id, 0 when the caster's action had none), the graph event it
// sent and the input type bits, as an actionPerformed event reports them,
// performed on the actor through the entry the engine's own casters reach
// for the Attack, Ready, Release and Interrupt actions (TESActionData,
// CommonLibSSE-NG include/RE/T/TESActionData.h; ActionInput's source, target
// and action, ActionOutput's animEvent and animObjIdle). Queued to the game
// thread's task queue, which SkyrimPlatform drains right after the JS tick of
// the same frame (main.cpp), never inside the JS call: performed inside it
// (0ce34196) the action's graph event reached Skyrim Platform's
// sendAnimationEvent hook, whose call back into JS from within a running JS
// call hung the game at the first replay (x-cast-gap 20261015-211105 and
// x-cast-human 211518 on the lab: no performAction logged, the seat's
// heartbeat gone). The graph's answer goes to performActionResult, read on
// the next update, so a refused action is retried; a queued task answering
// "queued" (abe44a55) had the view's retry never fire (playtest nineteen's
// second session: concentration starts taken 8 of 15 on the figure).
namespace {
std::mutex g_actionResultMutex;
// the graph's answer to the last performAction on each actor: 1 taken, 0
// refused, -1 queued and not yet performed
std::unordered_map<uint32_t, int> g_actionResult;
}

Napi::Value MagicApi::PerformAction(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto actionFormId = NapiHelper::ExtractUInt32(info[1], "actionFormId");
  const auto idleFormId = NapiHelper::ExtractUInt32(info[2], "idleFormId");
  const auto eventName = NapiHelper::ExtractString(info[3], "eventName");
  const auto type = NapiHelper::ExtractUInt32(info[4], "type");

  {
    std::lock_guard<std::mutex> lock(g_actionResultMutex);
    g_actionResult[actorFormId] = -1;
  }

  g_nativeCallRequirements.gameThrQ->AddTask(
    [actorFormId, actionFormId, idleFormId, eventName, type](Viet::Void) {
      int result = 0;
      auto* pActor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
      auto* pAction = RE::TESForm::LookupByID<RE::BGSAction>(actionFormId);
      if (pActor && pAction) {
        auto* pIdle = idleFormId
          ? RE::TESForm::LookupByID<RE::TESIdleForm>(idleFormId)
          : nullptr;
        result = skymp::magic::details::PerformActionOnActor(
                   *pActor, *pAction, pIdle, eventName, type)
          ? 1
          : 0;
      }
      std::lock_guard<std::mutex> lock(g_actionResultMutex);
      g_actionResult[actorFormId] = result;
    });

  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value MagicApi::PerformActionResult(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  std::lock_guard<std::mutex> lock(g_actionResultMutex);
  const auto it = g_actionResult.find(actorFormId);
  return Napi::Number::New(info.Env(),
                           it == g_actionResult.end() ? -1 : it->second);
}

// thuum docs/verbs/spell-cast.md (playtest nineteen: Flames aimed straight
// up showed as a forward stream on the other screen): the engine aims an
// actor's stream at its launch and again every frame from Actor::GetAimAngle
// (37930 on 1.6.1170), which returns -AimPitchCurrent when the actor's graph
// has bAimActive, else the reference's angle.x (the re-analyst on sky-re,
// 2026-10-16; CommonLibSSE-NG src/RE/A/Actor.cpp:275-296). Nothing set either
// on a figure while it stands (movementApply passes only a heading offset),
// so its stream flew level. Both are written here, each update, from the
// caster's pitch (radians, down positive, as GetAimAngle): the reference's
// angle (include/RE/T/TESObjectREFR.h:495, OBJ_REFR's angle) and the graph
// float, so either branch reads the caster's aim. False for no actor or a
// pitch that is not a number.
Napi::Value MagicApi::SetActorAimPitch(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto radians = NapiHelper::ExtractFloat(info[1], "radians");
  auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
  if (!actor || !std::isfinite(radians)) {
    return Napi::Boolean::New(info.Env(), false);
  }
  actor->data.angle.x = radians;
  actor->SetGraphVariableFloat("AimPitchCurrent", -radians);
  return Napi::Boolean::New(info.Env(), true);
}

// thuum docs/verbs/spell-cast.md (x-stream-aim 20261017-135526: a figure's
// Scattershock missiles leave 2.75 degrees off the caster's own). The engine
// takes an aimed projectile's heading at every relaunch from
// Actor::GetAimHeading (37929 on 1.6.1170; MagicCaster 34450, then 34452
// into LaunchData's angleZ, CommonLibSSE-NG include/RE/P/Projectile.h:88),
// which returns GetHeading(false) minus the graph float AimHeadingCurrent
// (include/RE/F/FixedStrings.h:187; src/RE/A/Actor.cpp:288-296, no
// bAimActive branch); GetHeading(false) on a Character is the reference's
// angle.z (37206; angle.z plus angle.y only for a race flagged kImmobile,
// include/RE/T/TESRace.h:60), and the player's own override adds a seat's
// heading delta, zero on foot (the re-analyst on sky-re, 2026-10-17,
// CONFIRMED-STATIC). The body's heading is the movement's, so only the graph
// float is written, to the difference, and GetAimHeading returns the
// caster's heading without turning the figure; a figure's Flames and beams
// read the same float every frame. HYPOTHESIS until a lab run: the graph
// does not write the float back between this write and the caster's read.
// False for no actor or a heading that is not a number.
Napi::Value MagicApi::SetActorAimHeading(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto radians = NapiHelper::ExtractFloat(info[1], "radians");
  auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
  if (!actor || !std::isfinite(radians)) {
    return Napi::Boolean::New(info.Env(), false);
  }
  constexpr float kPi = 3.1415927f;
  float delta = actor->GetHeading(false) - radians;
  // 37929 subtracts without wrapping and its readers are periodic; a value
  // in [-pi, pi] keeps a graph reading it for pose from twisting the spine
  while (delta > kPi) {
    delta -= 2.0f * kPi;
  }
  while (delta < -kPi) {
    delta += 2.0f * kPi;
  }
  const bool ok = actor->SetGraphVariableFloat(
    RE::FixedStrings::GetSingleton()->aimHeadingCurrent, delta);
  return Napi::Boolean::New(info.Env(), ok);
}

// thuum docs/verbs/self-cast.md (playtest twenty, Eli: an invisible
// player's figure still showed the crosshair's "E Talk / test 1" prompt;
// "yes hide it"): the reference's ExtraFlags kBlockActivateText
// (CommonLibSSE-NG include/RE/E/ExtraFlags.h:20), set or cleared through its
// extra data list (include/RE/E/ExtraDataList.h:194), the way the engine's
// own BlockActivation sets kBlockActivate (src/RE/T/TESObjectREFR.cpp:843).
// HYPOTHESIS until a lab run: the crosshair's prompt reads the flag.
Napi::Value MagicApi::SetActivateTextHidden(const Napi::CallbackInfo& info)
{
  const auto refrFormId = NapiHelper::ExtractUInt32(info[0], "refrFormId");
  const auto hidden = NapiHelper::ExtractBoolean(info[1], "hidden");
  auto* refr = RE::TESForm::LookupByID<RE::TESObjectREFR>(refrFormId);
  if (!refr) {
    return Napi::Boolean::New(info.Env(), false);
  }
  refr->extraList.SetExtraFlags(RE::ExtraFlags::Flag::kBlockActivateText,
                                hidden);
  return Napi::Boolean::New(info.Env(), true);
}

// thuum docs/research/apocalypse/target-location.md (the class pass;
// x-class-location 20261018-013501: an observer's game launched nothing and
// showed nothing for Thrumming Stone, Lightning Strike and the rune). The
// engine places a target-location spell's explosion through one call after
// its pick (game/Offsets.h PlaceAreaEffect, 34413, the re-analyst's read on
// sky-re 2026-10-17: signature CONFIRMED-STATIC, behavior HYPOTHESIS until a
// run): the caster, the point, the point's cell, the spell, its largest-area
// effect (MagicItemDataCollector, CommonLibSSE-NG include/RE/M/
// MagicItemDataCollector.h:37) and a target list it may fill. Called here for
// a figure at the point its caster's game picked, on the game thread, when
// the cell is attached and the effect is not Aimed, the dual-cast flag as
// the caster's; it creates the explosion, owned by the figure, and applies
// no spell number itself (the explosion's own record may: target-location.md,
// ADR-028's rule to extend). False for a point that is not a number or a
// non-AE runtime; a missing actor, spell or cell places nothing.
Napi::Value MagicApi::PlaceAreaEffect(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto spellFormId = NapiHelper::ExtractUInt32(info[1], "spellFormId");
  const auto castingSource = static_cast<RE::MagicSystem::CastingSource>(
    NapiHelper::ExtractInt32(info[2], "castingSource"));
  const RE::NiPoint3 point{ NapiHelper::ExtractFloat(info[3], "x"),
                            NapiHelper::ExtractFloat(info[4], "y"),
                            NapiHelper::ExtractFloat(info[5], "z") };
  const bool dual = NapiHelper::ExtractBoolean(info[6], "dual");
  if (!std::isfinite(point.x) || !std::isfinite(point.y) ||
      !std::isfinite(point.z) || !REL::Module::IsAE()) {
    return Napi::Boolean::New(info.Env(), false);
  }
  g_nativeCallRequirements.gameThrQ->AddTask(
    [actorFormId, spellFormId, castingSource, point, dual](Viet::Void) {
      auto* actor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);
      auto* spell = RE::TESForm::LookupByID<RE::MagicItem>(spellFormId);
      if (!actor || !spell) {
        return;
      }
      auto* caster = actor->GetMagicCaster(castingSource);
      auto* tes = RE::TES::GetSingleton();
      auto* cell = tes ? tes->GetCell(point) : nullptr;
      if (!caster || !cell || !cell->IsAttached()) {
        return;
      }
      RE::MagicItemDataCollector collector(spell);
      spell->Traverse(collector);
      auto* effect = collector.largestAreaEffect;
      if (!effect || !effect->baseEffect ||
          effect->baseEffect->data.delivery ==
            RE::MagicSystem::Delivery::kAimed) {
        return;
      }
      caster->SetDualCasting(dual);
      using PlaceAreaEffect_t =
        void(RE::MagicCaster*, const RE::NiPoint3*, RE::TESObjectCELL*,
             RE::MagicItem*, RE::Effect*,
             RE::BSScrapArray<RE::NiPointer<RE::TESObjectREFR>>*);
      REL::Relocation<PlaceAreaEffect_t> place{
        Offsets::Hooks::PlaceAreaEffect.address()
      };
      RE::BSScrapArray<RE::NiPointer<RE::TESObjectREFR>> targets;
      place(caster, &point, cell, spell, effect, &targets);
    });
  return Napi::Boolean::New(info.Env(), true);
}

Napi::Value MagicApi::CastReady(const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");
  const auto hand = NapiHelper::ExtractInt32(info[1], "hand");
  return Napi::Boolean::New(info.Env(),
                            Frida::TakeCastReady(actorFormId, hand));
}

Napi::Value MagicApi::ApplyAnimationVariablesToActor(
  const Napi::CallbackInfo& info)
{
  const auto actorFormId = NapiHelper::ExtractUInt32(info[0], "actorFormId");

  const auto pActor = RE::TESForm::LookupByID<RE::Actor>(actorFormId);

  if (!pActor) {
    return Napi::Boolean::New(info.Env(), false);
  }

  const bool isAnimationVariablesApplied =
    AnimationGraphMasterBehaviourDescriptor{
      skymp::magic::details::GetAnimationVariablesFromJSArg(
        NapiHelper::ExtractObject(info[1], "animationVariables"))
    }
      .ApplyVariablesToActor(*pActor);

  return Napi::Boolean::New(info.Env(), isAnimationVariablesApplied);
}

void MagicApi::Register(Napi::Env env, Napi::Object& exports)
{
  exports.Set("castSpellImmediate",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(CastSpellImmediate)));
  exports.Set(
    "interruptCast",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(InterruptCast)));

  exports.Set(
    "performAction",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(PerformAction)));

  exports.Set("performActionResult",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(PerformActionResult)));

  exports.Set("setActorAimHeading",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetActorAimHeading)));

  exports.Set(
    "placeAreaEffect",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(PlaceAreaEffect)));

  exports.Set("setActivateTextHidden",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(SetActivateTextHidden)));

  exports.Set(
    "castReady",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(CastReady)));

  exports.Set(
    "setActorAimPitch",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetActorAimPitch)));

  exports.Set(
    "getAnimationVariablesFromActor",
    Napi::Function::New(
      env, NapiHelper::WrapCppExceptions(GetAnimationVariablesFromActor)));

  exports.Set(
    "applyAnimationVariablesToActor",
    Napi::Function::New(
      env, NapiHelper::WrapCppExceptions(ApplyAnimationVariablesToActor)));
}
