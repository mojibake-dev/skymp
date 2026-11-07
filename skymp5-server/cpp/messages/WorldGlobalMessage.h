#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

// thuum ADR-068 rung 4 (WorldGlobal; amendment B): the GLOBs every seat's
// scripts write, streamed by one seat at an epoch (wire-rules
// world_streamer). Both directions, reliable. Client to server: the
// streamer's once-a-second diff of the GLOB array, under the epoch its
// stream word named (no stream, served false). Server to client: the values
// that moved the world's record relayed to every other seat (served false),
// the record at login (served true), and the stream word at each election
// (stream true to the new streamer, false to the old; entries empty). The
// contract is skymp-wire's (wire-schema skymp.rs, WorldGlobal, MsgType 80,
// wire id 88).
struct WorldGlobalMessage : public MessageBase<WorldGlobalMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::WorldGlobal)>{};

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

    uint32_t global = 0; // the GLOB, a form of the shared load order
    float value = 0.f;
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("epoch", epoch)
      .Serialize("stream", stream)
      .Serialize("entries", entries)
      .Serialize("served", served);
  }

  uint32_t epoch = 0;         // the streamer's epoch
  std::optional<bool> stream; // the stream word alone
  std::vector<Entry> entries; // each global once, 512 at most
  bool served = false;        // the record served at login
};
