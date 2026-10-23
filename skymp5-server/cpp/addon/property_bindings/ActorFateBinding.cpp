#include "ActorFateBinding.h"
#include "MpActor.h"
#include "WorldState.h"
#include "libespm/NPC_.h"

Napi::Value ActorFateBinding::Get(Napi::Env env, ScampServer& scampServer,
                                  uint32_t formId)
{
  auto& worldState = scampServer.GetPartOne()->worldState;
  const auto& form = worldState.LookupFormByIdNoLoad(formId);
  MpActor* actor = form ? form->AsActor() : nullptr;
  if (!actor) {
    return env.Null();
  }
  auto res = Napi::Object::New(env);
  const uint32_t baseId = actor->GetFateBaseId();
  res.Set("baseId", Napi::Number::New(env, baseId));
  res.Set("level", Napi::Number::New(env, actor->GetFateLevel().value_or(0)));
  uint32_t raceId = 0;
  if (worldState.HasEspm() && baseId != 0) {
    const auto lookupRes =
      worldState.GetEspm().GetBrowser().LookupById(baseId);
    if (const auto* npc = espm::Convert<espm::NPC_>(lookupRes.rec)) {
      raceId =
        lookupRes.ToGlobalId(npc->GetData(worldState.GetEspmCache()).race);
    }
  }
  res.Set("raceId", Napi::Number::New(env, raceId));
  auto worn = Napi::Array::New(env);
  uint32_t k = 0;
  for (const auto& entry : actor->GetEquipment().inv.entries) {
    if (entry.GetWorn() != Inventory::Worn::None) {
      worn.Set(k++, Napi::Number::New(env, entry.baseId));
    }
  }
  res.Set("worn", worn);
  return res;
}
