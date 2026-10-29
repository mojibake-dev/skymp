#pragma once
#include "FormDesc.h"
#include <cstdint>
#include <tuple>

// One quest stage the world has done, as the server records it (thuum
// docs/verbs/quest-stage.md, ADR-056 decision 2: per quest the set of stages
// done, the engine's GetStageDone semantics, because a stage can be set out
// of order). The quest is named by its FormDesc, so a change of load order
// keeps it.
struct QuestStageDone
{
  FormDesc quest;
  uint16_t stage = 0;

  auto ToTuple() const { return std::make_tuple(quest, stage); }

  friend bool operator==(const QuestStageDone& lhs, const QuestStageDone& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }

  // the change form's own order compares its fields' (MpChangeForms.h)
  friend bool operator<(const QuestStageDone& lhs, const QuestStageDone& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};
