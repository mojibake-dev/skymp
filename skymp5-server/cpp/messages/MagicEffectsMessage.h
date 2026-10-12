#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>
#include <vector>

// The effects running on an actor (thuum docs/verbs/magic-effects.md). Server
// to client, reliable: whenever the set changes, and in full when a client
// first sees the actor, for each game to show them (the effect's hit shader
// and art on that actor for the seconds left), and the sums the actor's
// running buffs put on its actor values, which its own game holds. The
// effects are the server's (R0); each message replaces the actor's set.
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

  // What the actor's running buffs add to one actor value: the sum the
  // actor's own game holds on it while they run (its engine applies no drink
  // of its own)
  struct Modifier
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("av", av).Serialize("amount", amount);
    }

    friend bool operator==(const Modifier& lhs, const Modifier& rhs)
    {
      return lhs.av == rhs.av && lhs.amount == rhs.amount;
    }

    uint32_t av = 0;
    float amount = 0.f;
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("idx", idx)
      .Serialize("effects", effects)
      .Serialize("modifiers", modifiers);
  }

  uint32_t idx = 0;
  std::vector<Effect> effects;
  // one per actor value the running buffs leave away from 0, sent before
  // the ChangeValues of the same change, so a game moves its maximum before
  // it takes the percentage
  std::vector<Modifier> modifiers;
};
