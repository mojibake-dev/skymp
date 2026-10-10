#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>
#include <vector>

// The effects running on an actor (thuum docs/verbs/magic-effects.md). Server
// to client, reliable: whenever the set changes, and in full when a client
// first sees the actor, for each game to show them (the effect's hit shader
// and art on that actor for the seconds left). The effects are the server's
// (R0); each message replaces the actor's set.
struct MagicEffectsMessage : public MessageBase<MagicEffectsMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::MagicEffects)>{};

  struct Effect
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("effect", effect)
        .Serialize("source", source)
        .Serialize("magnitude", magnitude)
        .Serialize("remaining", remaining);
    }

    friend bool operator==(const Effect& lhs, const Effect& rhs)
    {
      return lhs.effect == rhs.effect && lhs.source == rhs.source &&
        lhs.magnitude == rhs.magnitude && lhs.remaining == rhs.remaining;
    }

    uint32_t effect = 0; // the MGEF
    uint32_t source = 0; // the potion, poison, spell or enchantment
    float magnitude = 0.f;
    float remaining = 0.f; // seconds; 0 for an effect without duration
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("idx", idx)
      .Serialize("effects", effects);
  }

  uint32_t idx = 0;
  std::vector<Effect> effects;
};
