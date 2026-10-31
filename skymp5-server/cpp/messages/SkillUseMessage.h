#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>
#include <vector>

// thuum docs/verbs/leveling.md (M5 Progression): the skill uses a player's
// engine made in the last second, batched. Client to server, reliable, R1:
// the engine computes each action's base amount (its own skill experience is
// off), the server bounds it and runs the game's formulas (wire-rules
// leveling, ActionListener::OnSkillUse) into the record, answering with
// ActorValues when a level moves. The contract is skymp-wire's (wire-schema
// skymp.rs, SkillUse, MsgType 63, wire id 71).
struct SkillUseMessage : public MessageBase<SkillUseMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::SkillUse)>{};

  struct Use
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("skill", skill).Serialize("amount", amount);
    }
    friend bool operator==(const Use& lhs, const Use& rhs)
    {
      return lhs.skill == rhs.skill && lhs.amount == rhs.amount;
    }
    uint8_t skill = 0;  // the skill's ActorValue, 6 to 23
    float amount = 0.f; // the action's base amount, above 0
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType).Serialize("uses", uses);
  }

  std::vector<Use> uses; // each skill once, 1 to 18
};
