#include "TakeItemEvent.h"

#include "MpActor.h"
#include "MpObjectReference.h"

TakeItemEvent::TakeItemEvent(MpActor* actor_, MpObjectReference* sourceRefr_,
                             const Inventory::Entry& entry_)
  : actor(actor_)
  , sourceRefr(sourceRefr_)
  , entry(entry_)
{
}

const char* TakeItemEvent::GetName() const
{
  return "onTakeItem";
}

std::string TakeItemEvent::GetArgumentsJsonArray() const
{
  std::string result;
  result += "[";
  result += std::to_string(sourceRefr->GetFormId());
  result += ",";
  result += std::to_string(actor->GetFormId());
  result += ",";
  result += std::to_string(entry.baseId);
  result += ",";
  result += std::to_string(entry.count); // TODO: implement extra data
  result += "]";
  return result;
}

void TakeItemEvent::OnFireSuccess(WorldState*)
{
  sourceRefr->RemoveItems({ entry }, actor);
  // thuum docs/verbs/npc-fate.md (playtest 48, the loot strip): a take from a
  // body leaves no worn entry its inventory no longer covers, so the body's
  // fate never names the taken piece (H2: the reconcile adding it back)
  if (auto body = sourceRefr->AsActor()) {
    body->DropWornBeyondInventory(entry.baseId);
  }
}
