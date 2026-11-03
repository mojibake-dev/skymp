#include "RespawnFatedBinding.h"

Napi::Value RespawnFatedBinding::Get(Napi::Env env, ScampServer& scampServer,
                                     uint32_t formId)
{
  auto& partOne = scampServer.GetPartOne();

  auto& actor = partOne->worldState.GetFormAt<MpActor>(formId);
  return Napi::Boolean::New(env, actor.IsDead());
}

void RespawnFatedBinding::Set(Napi::Env env, ScampServer& scampServer,
                              uint32_t formId, Napi::Value newValue)
{
  auto& partOne = scampServer.GetPartOne();

  auto& actor = partOne->worldState.GetFormAt<MpActor>(formId);
  if (newValue.As<Napi::Boolean>().Value()) {
    // RespawnFated returns at once for a living actor
    actor.RespawnFated();
  }
}
