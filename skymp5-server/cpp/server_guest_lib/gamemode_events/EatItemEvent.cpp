#include "EatItemEvent.h"

#include "MpActor.h"
#include "WorldState.h"
#include <chrono>
#include <vector>

EatItemEvent::EatItemEvent(MpActor* actor_, uint32_t baseId_,
                           bool isIngredient_, bool isAlchemyItem_)
  : actor(actor_)
  , baseId(baseId_)
  , isIngredient(isIngredient_)
  , isAlchemyItem(isAlchemyItem_)
{
}

const char* EatItemEvent::GetName() const
{
  return "onEatItem";
}

std::string EatItemEvent::GetArgumentsJsonArray() const
{
  std::string result;
  result += "[";
  result += std::to_string(actor->GetFormId());
  result += ",";
  result += std::to_string(baseId);
  result += "]";
  return result;
}

void EatItemEvent::OnFireSuccess(WorldState* worldState)
{
  if (!isAlchemyItem) {
    // an ingredient's effects are a later slice (thuum
    // docs/verbs/magic-effects.md; learned-effects records what it teaches)
    return;
  }
  const auto data = espm::GetData<espm::ALCH>(baseId, worldState);
  if (data.isPoison) {
    // a poison goes on a weapon, not into the one who holds it: its server
    // side is a later step of thuum docs/verbs/magic-effects.md (the legacy
    // path restored the drinker by the poison's magnitude)
    return;
  }
  // thuum docs/verbs/magic-effects.md: a drink's effects run by their
  // records through the effect rule. The drinker's own game drinks it too;
  // a value the drink raised holds against the drinker's stale reports
  // until one reaches it (docs/verbs/attributes.md, in SetPercentages)
  actor->ApplyEffects(baseId, MpActor::EffectsOf(worldState, baseId), 1.f, 0,
                      false);
}
