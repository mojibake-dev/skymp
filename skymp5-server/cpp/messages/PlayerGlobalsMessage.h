#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>
#include <vector>

// A player's own global variables (thuum docs/verbs/player-globals.md,
// ADR-029's A2). Both directions, reliable. Server to client, after a login:
// the globals the server keeps per player, to watch, and the player's
// recorded values, which the client's game sets. Client to server: the
// watched globals whose values changed since its last report (R2: recorded
// unvalidated), naming none to watch. The latest value wins per global.
struct PlayerGlobalsMessage : public MessageBase<PlayerGlobalsMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PlayerGlobals)>{};

  struct Entry
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("global", global).Serialize("value", value);
    }

    friend bool operator==(const Entry& lhs, const Entry& rhs)
    {
      return lhs.global == rhs.global && lhs.value == rhs.value;
    }

    uint32_t global = 0; // its form id in the load order both ends share
    float value = 0.f;
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("watch", watch)
      .Serialize("values", values);
  }

  std::vector<uint32_t> watch; // server to client only
  std::vector<Entry> values;
};
