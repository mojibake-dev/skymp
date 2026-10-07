#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>
#include <vector>

// A player's actor values and progress, the whole snapshot (thuum
// docs/verbs/actor-values.md). Client to server after a skill or level
// increase; server to client after a login and after a value the server set.
struct ActorValuesMessage : public MessageBase<ActorValuesMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ActorValues)>{};

  struct Base
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("av", av).Serialize("base", base);
    }

    friend bool operator==(const Base& lhs, const Base& rhs)
    {
      return lhs.av == rhs.av && lhs.base == rhs.base;
    }

    uint8_t av = 0; // 0 to 163, CommonLibSSE-NG's RE::ActorValue
    float base = 0.f;
  };

  struct Skill
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("skill", skill)
        .Serialize("level", level)
        .Serialize("xp", xp)
        .Serialize("threshold", threshold);
    }

    friend bool operator==(const Skill& lhs, const Skill& rhs)
    {
      return lhs.skill == rhs.skill && lhs.level == rhs.level &&
        lhs.xp == rhs.xp && lhs.threshold == rhs.threshold;
    }

    uint8_t skill = 0; // 0 to 17, PlayerSkills::Skills
    float level = 0.f;
    float xp = 0.f;
    float threshold = 0.f;
  };

  struct Legendary
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("skill", skill).Serialize("count", count);
    }

    friend bool operator==(const Legendary& lhs, const Legendary& rhs)
    {
      return lhs.skill == rhs.skill && lhs.count == rhs.count;
    }

    uint8_t skill = 0;
    uint16_t count = 0;
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("bases", bases)
      .Serialize("skills", skills)
      .Serialize("xp", xp)
      .Serialize("threshold", threshold)
      .Serialize("level", level)
      .Serialize("legendary", legendary);
  }

  std::vector<Base> bases;
  std::vector<Skill> skills;
  float xp = 0.f;
  float threshold = 0.f;
  uint16_t level = 1;
  std::vector<Legendary> legendary;
};
