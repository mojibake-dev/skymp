#include "MagicApi.h"
#include "CallNativeApi.h"
#include "FridaHooks.h"
#include "JsUtils.h"
#include "SkyrimPlatform.h"
#include "game/Offsets.h"

#include "Magic/AnimationGraphMasterBehaviourDescriptor.h"

#include <mutex>
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

  g_nativeCallRequirements.gameThrQ->AddTask(
    [spellFormId, actorFormId, castingSource, magicTargetFormId,
     projectileAngles,

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

      const bool isAnimationVariablesApplied =
        AnimationGraphMasterBehaviourDescriptor{ std::move(animVars) }
          .ApplyVariablesToActor(*pActor);

      if (!isAnimationVariablesApplied) {
        return;
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

        magicCaster->CastSpellImmediate(pSpell, false, magicTarget, 1.0f,
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

      if (pSpell->data.delivery ==
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

      RE::Projectile::LaunchData launchData(pActor, origin, projectileAngles,
                                            pSpell);

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

      const bool isAnimationVariablesApplied =
        AnimationGraphMasterBehaviourDescriptor{ std::move(animVars) }
          .ApplyVariablesToActor(*pActor);

      if (!isAnimationVariablesApplied) {
        return;
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

  exports.Set(
    "castReady",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(CastReady)));

  exports.Set(
    "getAnimationVariablesFromActor",
    Napi::Function::New(
      env, NapiHelper::WrapCppExceptions(GetAnimationVariablesFromActor)));

  exports.Set(
    "applyAnimationVariablesToActor",
    Napi::Function::New(
      env, NapiHelper::WrapCppExceptions(ApplyAnimationVariablesToActor)));
}
