#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

// thuum docs/verbs/quest-stage.md (ADR-056; memo D58 B): a quest stage the
// world shares. Both directions, reliable. Client to server: the seat's own
// game set `stage` on `quest` (the engine's questStage event; R1: the server
// checks that the quest is a QUST of the load order and the world's by its
// `sharedQuests` mode, then records the stage as done world-wide, R0).
// Server to client: the relay of a recorded stage to every other seat, and
// the login replay, ascending per quest, opened by one message of quest 0
// whose `mode` names the sharing mode and whose `shared` names, in mode 0
// (list), the quests the seat reports and, in mode 1 (all), the DENIED
// quests it never reports (empty when none is); `shared` is empty and
// `mode` 0 in every other message, and a client naming either is refused.
// Since SCHEMA_VERSION 41 (thuum ADR-068 rung 5): `word` 1 a start or 2 a
// stop with stage 0 (a quest's run, wire-rules quest_runs; a stage-0 report
// with no word is a start too), `acting` the acting player's actor (the
// server's stamp; a seat's scope's player, absent meaning the sender) and
// `served` true on the login replay (applied as a load), each absent unless
// set, so a stage alone reads and writes as before. The contract is
// skymp-wire's (wire-schema skymp.rs, QuestStage, MsgType 57).
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
      .Serialize("shared", shared)
      .Serialize("mode", mode)
      .Serialize("word", word)
      .Serialize("acting", acting)
      .Serialize("served", served);
  }

  uint32_t quest = 0; // the QUST's form id in the load order both ends share
  uint16_t stage = 0; // the engine's stage index
  // server to client only, at a login: the listed quests in mode 0, the
  // denied quests in mode 1
  std::vector<uint32_t> shared;
  uint8_t mode = 0; // server to client only, at a login: 0 list, 1 all
  // thuum ADR-068 rung 5: the run word, 1 a start or 2 a stop with stage 0
  // (a u8 on the wire); absent on a stage
  std::optional<uint32_t> word;
  // the acting player's actor (0xff000000 and up); absent means the sender
  std::optional<uint32_t> acting;
  // server to client only: true on the login replay
  std::optional<bool> served;
};
