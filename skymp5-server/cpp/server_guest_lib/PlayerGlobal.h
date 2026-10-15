#pragma once
#include "FormDesc.h"
#include <tuple>

// One global variable the player's game wrote for its player, as the server
// keeps it (thuum docs/verbs/player-globals.md, ADR-029's A2: R2, recorded
// unvalidated). The global is named by its FormDesc, so a change of load
// order keeps it.
struct PlayerGlobal
{
  FormDesc global;
  float value = 0.f;

  auto ToTuple() const { return std::make_tuple(global, value); }

  friend bool operator==(const PlayerGlobal& lhs, const PlayerGlobal& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }
};
