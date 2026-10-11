#pragma once
#include "FormDesc.h"
#include <tuple>
#include <vector>

// What a player knows of shouts (thuum docs/verbs/shouts.md), kept as the
// engine keeps it: the shouts in its list (Actor.AddShout) and the words of
// power it knows and has unlocked, each a flag on the word's form in the
// engine (TESForm's kKnown and kUnlocked, CommonLibSSE-NG
// include/RE/T/TESForm.h)
struct ShoutRecord
{
  std::vector<FormDesc> shouts;
  std::vector<FormDesc> wordsKnown;
  std::vector<FormDesc> wordsUnlocked;

  bool Empty() const
  {
    return shouts.empty() && wordsKnown.empty() && wordsUnlocked.empty();
  }

  auto ToTuple() const
  {
    return std::make_tuple(shouts, wordsKnown, wordsUnlocked);
  }

  friend bool operator==(const ShoutRecord& lhs, const ShoutRecord& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }

  friend bool operator<(const ShoutRecord& lhs, const ShoutRecord& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};
