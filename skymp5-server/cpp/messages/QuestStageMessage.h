#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>
#include <vector>

// thuum docs/verbs/quest-stage.md (ADR-056): a quest stage the world shares.
// Both directions, reliable. Client to server: the seat's own game set
// `stage` on `quest` (the engine's questStage event; R1: the server checks
// that the quest is a QUST of the load order and on its `sharedQuests`
// list, then records the stage as done world-wide, R0). Server to client:
// the relay of a recorded stage to every other seat, and the login replay,
// ascending per quest, opened by one message of quest 0 whose `shared`
// names the listed quests' form ids, so the seat knows which quests to
// report; `shared` is empty in every other message, and a client naming it
// is refused. The contract is skymp-wire's (wire-schema skymp.rs,
// QuestStage, MsgType 57).
struct QuestStageMessage : public MessageBase<QuestStageMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::QuestStage)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("quest", quest)
      .Serialize("stage", stage)
      .Serialize("shared", shared);
  }

  uint32_t quest = 0; // the QUST's form id in the load order both ends share
  uint16_t stage = 0; // the engine's stage index
  std::vector<uint32_t> shared; // server to client only, at a login
};
