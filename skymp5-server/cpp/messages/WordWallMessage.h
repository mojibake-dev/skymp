#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/word-wall.md (M4): a player's game reports the word wall
// trigger its engine fired. Client to server, reliable, R1: the server reads
// the word from the trigger reference's own record, wants the player within
// the activation reach of it in the same cell, and writes the word into the
// shout record (R0), telling the seat as the console's TeachWord does
// (ActionListener::OnWordWall). The contract is skymp-wire's (wire-schema
// skymp.rs, WordWall, MsgType 69, wire id 77).
struct WordWallMessage : public MessageBase<WordWallMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::WordWall)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType).Serialize("wall", wall);
  }

  uint32_t wall = 0; // the wall trigger's reference id, never 0
};
