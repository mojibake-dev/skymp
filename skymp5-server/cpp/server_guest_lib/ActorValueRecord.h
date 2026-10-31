#pragma once
#include <cstdint>
#include <tuple>
#include <utility>
#include <vector>

// A player's actor values and progress as the server records them (thuum
// docs/verbs/actor-values.md): each actor value's base (0 to 163,
// CommonLibSSE-NG's RE::ActorValue), each skill's progress (0 to 17,
// PlayerSkills::Skills), the character's experience, threshold and level,
// how many times each skill was made legendary, and the perks held (thuum
// docs/verbs/perks.md: sorted form ids; the points left are one a level past
// the first less these).
struct ActorValueRecord
{
  struct Skill
  {
    uint8_t skill = 0;
    float level = 0.f;
    float xp = 0.f;
    float threshold = 0.f;

    auto ToTuple() const
    {
      return std::make_tuple(skill, level, xp, threshold);
    }

    friend bool operator==(const Skill& lhs, const Skill& rhs)
    {
      return lhs.ToTuple() == rhs.ToTuple();
    }

    friend bool operator<(const Skill& lhs, const Skill& rhs)
    {
      return lhs.ToTuple() < rhs.ToTuple();
    }
  };

  std::vector<std::pair<uint8_t, float>> bases;
  std::vector<Skill> skills;
  float xp = 0.f;
  float threshold = 0.f;
  uint16_t level = 1;
  std::vector<std::pair<uint8_t, uint16_t>> legendary;
  std::vector<uint32_t> perks;

  auto ToTuple() const
  {
    return std::make_tuple(bases, skills, xp, threshold, level, legendary,
                           perks);
  }

  friend bool operator==(const ActorValueRecord& lhs,
                         const ActorValueRecord& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }

  // the change form compares its fields as a tuple
  friend bool operator<(const ActorValueRecord& lhs,
                        const ActorValueRecord& rhs)
  {
    return lhs.ToTuple() < rhs.ToTuple();
  }
};
