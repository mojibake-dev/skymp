#include "HasMagicEffect.h"
#include "MpActor.h"
#include <algorithm>

const char* ConditionFunctions::HasMagicEffect::GetName() const
{
  return "HasMagicEffect";
}

uint16_t ConditionFunctions::HasMagicEffect::GetFunctionIndex() const
{
  return 214;
}

float ConditionFunctions::HasMagicEffect::Execute(
  MpActor& actor, uint32_t parameter1, [[maybe_unused]] uint32_t parameter2,
  const ConditionEvaluatorContext&)
{
  // The running effects are the server's own applications with a duration
  // (wire-rules magic.rs: an instant effect gets no entry), keyed by the
  // MGEF (RunningEffect.h), read here by runtime id. HYPOTHESIS
  // (docs/verbs/spell-cast.md, Hailstone): the engine's ActiveEffect of an
  // instant effect lives for the hit it belongs to, so a same-hit check
  // against an instant effect reads false here as it does there.
  if (parameter1 == 0) {
    return 0.f;
  }
  const auto running = actor.GetRunningEffects();
  return std::any_of(running.begin(), running.end(),
                     [&](const MpActor::RunningEffectView& r) {
                       return r.effect == parameter1;
                     })
    ? 1.f
    : 0.f;
}
