#pragma once
#include "MinPacketId.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace Networking {

using UserId = unsigned short;

constexpr UserId InvalidUserId = static_cast<UserId>(~0);

// First byte must represent id of the packet (>= MinPacketId)
using PacketData = const unsigned char*;

enum class PacketType
{
  Invalid = -1,
  Message,
  ClientSideDisconnect,
  ClientSideConnectionAccepted,
  ClientSideConnectionFailed,
  ClientSideConnectionDenied,
  ServerSideUserConnect,
  ServerSideUserDisconnect
};

class IClient
{
public:
  using OnPacket = void (*)(void* state, PacketType packetType,
                            PacketData data, size_t length, const char* error);

  virtual ~IClient() = default;

  virtual void Send(PacketData data, size_t length, bool reliable) = 0;
  virtual void Tick(OnPacket onPacket, void* state) = 0;
  virtual bool IsConnected() const = 0;
};

class ISendTarget
{
public:
  virtual ~ISendTarget() = default;

  virtual void Send(UserId targetUserId, PacketData data, size_t length,
                    bool reliable) = 0;
};

class IServer : public ISendTarget
{
public:
  typedef void (*OnPacket)(void* state, UserId userId, PacketType packetType,
                           PacketData data, size_t length);

  virtual ~IServer() = default;

  virtual void Tick(OnPacket onPacket, void* state) = 0;

  virtual std::string GetIp(UserId userId) const = 0;

  // The connection's smoothed round trip in milliseconds, or -1 while
  // unknown (a transport that measures none, a client not connected);
  // thuum docs/verbs/cell-host.md ranks a cell's visitors by it.
  virtual int64_t GetRttMs(UserId) const { return -1; }
  // thuum docs/verbs/cell-host.md, the quitting host: milliseconds since
  // the user's connection last delivered a message, -1 while unknown (the
  // wire server measures it; a game killed, crashed or quit sends netcode
  // no disconnect, and its silence is the server's only early word)
  virtual int64_t GetSilenceMs(UserId) const { return -1; }

  virtual void CloseConnection(UserId userId) = 0;
};
}
