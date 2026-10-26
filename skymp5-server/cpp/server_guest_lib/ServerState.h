#pragma once
#include "ActorsMap.h"
#include "Config.h"
#include <Networking.h>
#include <array>
#include <chrono>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <simdjson.h>
#include <unordered_map>

class MpActor;

struct PacketHistoryElement
{
  size_t offset = 0;
  size_t length = 0;
  uint64_t timeMs = 0;
};

struct PacketHistory
{
  std::vector<uint8_t> buffer;
  std::deque<PacketHistoryElement> packets;
};

struct Playback
{
  PacketHistory history;
  std::chrono::time_point<std::chrono::steady_clock> startTime;
};

struct DeferredMessage
{
  std::vector<uint8_t> packetData;
  bool packetReliable = false;
  uint32_t actorIdExpected = 0;
};

struct UserInfo
{
  bool isDisconnecting = false;

  bool isPacketHistoryRecording = false;
  PacketHistory packetHistory;
  std::optional<std::chrono::time_point<std::chrono::steady_clock>>
    packetHistoryStartTime;

  std::vector<std::vector<DeferredMessage>> deferredChannels;

  std::string guid;
};

class ServerState
{
public:
  ServerState() { userInfo.resize(kMaxPlayers); }

  std::vector<std::unique_ptr<UserInfo>> userInfo;
  Networking::UserId maxConnectedId = 0;
  ActorsMap actorsMap;
  Networking::UserId disconnectingUserId = Networking::InvalidUserId;

  std::map<Networking::UserId, Playback>
    activePlaybacks; // do not modify directly, use requestedPlaybacks
  std::map<Networking::UserId, Playback> requestedPlaybacks;

  // thuum docs/verbs/npc-stream.md (slice 4, ADR-050 decision 5): what each
  // connection's stream did, by SkyMP message type: reports accepted and
  // dropped (the host gate, the per-message refusals), and the reports about
  // actors the sender does not own (ActorUpdatableBy's refusals). Read by
  // the lab through mp.get(0, "streamCounters").
  struct StreamCount
  {
    uint32_t accepted = 0;
    uint32_t dropped = 0;
  };
  struct StreamRow
  {
    Networking::UserId user = 0;
    uint8_t msgType = 0;
    uint32_t accepted = 0;
    uint32_t dropped = 0;
    uint32_t foreign = 0;
  };
  void CountStream(Networking::UserId userId, uint8_t msgType, bool accepted,
                   bool foreign = false);
  std::vector<StreamRow> StreamRows() const;

  void Connect(Networking::UserId userId, const std::string& guid);
  void Disconnect(Networking::UserId userId) noexcept;
  bool IsConnected(Networking::UserId userId) const;
  MpActor* ActorByUser(Networking::UserId userId);
  const std::string& UserGuid(Networking::UserId userId);
  Networking::UserId UserByActor(MpActor* actor);
  void EnsureUserExists(Networking::UserId userId);

private:
  struct UserStream
  {
    std::map<uint8_t, StreamCount> byType;
    uint32_t foreign = 0;
  };
  std::map<Networking::UserId, UserStream> streamCounters;
};
