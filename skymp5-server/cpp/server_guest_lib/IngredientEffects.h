#pragma once
#include "FormDesc.h"
#include <cstdint>
#include <tuple>

// The effects a player has learned of an ingredient (thuum
// docs/verbs/learned-effects.md): bit i set when effect i is known.
struct IngredientEffects
{
  FormDesc ingredient;
  uint8_t mask = 0;

  auto ToTuple() const { return std::make_tuple(ingredient, mask); }

  friend bool operator==(const IngredientEffects& lhs,
                         const IngredientEffects& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }

  friend bool operator<(const IngredientEffects& lhs,
                        const IngredientEffects& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};
