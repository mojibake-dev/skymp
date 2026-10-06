#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// After the player ate an ingredient, the effects its engine now knows of it
// (thuum docs/verbs/learned-effects.md): bit i set when effect i is known.
// The server keeps it only after an eat it saw, and only ever adds effects.
struct IngredientEffectsKnownMessage
  : public MessageBase<IngredientEffectsKnownMessage>
{
  static constexpr auto kMsgType = std::integral_constant<
    char, static_cast<char>(MsgType::IngredientEffectsKnown)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("ingredient", ingredient)
      .Serialize("mask", mask);
  }

  uint32_t ingredient = 0; // the INGR's form id
  uint8_t mask = 0;        // known effects, bits 0 to 3
};
