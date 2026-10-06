#pragma once
#include "FormDesc.h"
#include <cstdint>
#include <tuple>

// One of a player's favorites (thuum docs/verbs/favorites.md): an item or a
// spell or shout, and its hotkey, -1 for none or 0 to 7 for the keys 1 to 8.
struct Favorite
{
  FormDesc form;
  int8_t hotkey = -1;

  auto ToTuple() const { return std::make_tuple(form, hotkey); }

  friend bool operator==(const Favorite& lhs, const Favorite& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }

  friend bool operator<(const Favorite& lhs, const Favorite& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};
