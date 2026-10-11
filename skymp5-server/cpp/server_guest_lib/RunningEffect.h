#pragma once
#include "FormDesc.h"
#include <cstdint>
#include <tuple>

// An effect running on an actor (thuum docs/verbs/magic-effects.md), as the
// effect rule (wire-rules magic, through the bridge's EffectEntry) keeps it:
// its facts from the records, its magnitude already scaled, and the seconds
// it has run as of a moment on the server's wall clock. The world's clock
// runs while the server is down (thuum ADR-021), so a record loaded later
// runs its effects on by the time since that moment. Form ids are kept as
// FormDesc, so a change of load order keeps them.
struct RunningEffect
{
  FormDesc effect;   // the MGEF
  FormDesc source;   // the potion, poison, spell or enchantment
  FormDesc caster;   // who applied it; empty for none
  uint32_t kind = 0; // 0 value, 1 peak value, 2 dual value modifier
  int32_t av = -1;
  int32_t secondAv = -1; // a dual modifier's second actor value, -1 for none
  float secondWeight = 0.f;
  float magnitude = 0.f;
  float durationS = 0.f;
  float elapsedS = 0.f;
  // when elapsedS was counted, Unix milliseconds; 0 in records written
  // before it was kept, which run on from their load
  uint64_t asOfMs = 0;
  bool recover = false;
  bool detrimental = false;
  bool noDuration = false;

  auto ToTuple() const
  {
    return std::make_tuple(effect, source, caster, kind, av, secondAv,
                           secondWeight, magnitude, durationS, elapsedS,
                           asOfMs, recover, detrimental, noDuration);
  }

  friend bool operator==(const RunningEffect& lhs, const RunningEffect& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }

  friend bool operator<(const RunningEffect& lhs, const RunningEffect& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};
