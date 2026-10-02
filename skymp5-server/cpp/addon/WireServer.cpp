#include "WireServer.h"

#include "MinPacketId.h"
#include "wire_bridge_cxx/lib.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <fmt/format.h>
#include <prometheus/core.h>
#include <prometheus/gauge.h>
#include <spdlog/spdlog.h>

namespace {

rust::Box<skymp::wire::Server> Bind(const char* listenAddress,
                                    unsigned short port,
                                    unsigned short maxConnections,
                                    const char* password)
{
  skymp::wire::BindOptions options;
  options.listen_host = listenAddress;
  options.port = port;
  options.max_clients = maxConnections;
  options.password = password ? password : "";
  spdlog::info("WireServer: listening on {}:{} (netcode, unsecure tokens, "
               "{}), up to {} clients",
               listenAddress, port,
               options.password.empty() ? "no password" : "password set",
               maxConnections);
  // throws rust::Error (a std::exception) with E_BRIDGE_* on failure
  return skymp::wire::wire_server_bind(options);
}

class WireServer : public Networking::IServer
{
public:
  WireServer(const char* listenAddress, unsigned short port,
             unsigned short maxConnections, const char* password,
             std::shared_ptr<prometheus::Registry> promRegistry)
    : server(Bind(listenAddress, port, maxConnections, password))
    , clientsByUser(maxConnections, kNoClient)
    , connectedClientsGauge{
      promRegistry,
      "skymp_server_connected_clients_count",
      "Count of currently connected clients (as seen by the wire)",
    }
  {
  }

  void Send(Networking::UserId id, Networking::PacketData data, size_t length,
            bool reliable) override
  {
    const uint64_t client = ClientOf(id);
    if (length < 2 || data[0] != Networking::MinPacketId) {
      LogRefusal(0, "not a SkyMP packet");
      return;
    }
    // The core writes 0x86 + JSON (MessageSerializer); Rust takes the JSON.
    const char* text = reinterpret_cast<const char*>(data + 1);
    const size_t textLength = length - 1;
    try {
      rust::Str json(text, textLength); // checks UTF-8, throws if not
      if (uint16_t code = server->send(client, json, reliable)) {
        LogRefusal(code,
                   std::string(text, std::min<size_t>(textLength, 160)));
      }
    } catch (const std::invalid_argument&) {
      LogRefusal(400, "not UTF-8");
    }
  }

  void Tick(OnPacket onPacket, void* state) override
  {
    rust::Vec<skymp::wire::WireEvent> events;
    server->poll(events);
    for (auto& ev : events) {
      pending.push_back(std::move(ev));
    }
    // One event at a time, as RakNet's Receive loop did: if the core throws
    // on an event, the rest wait for the next Tick instead of being lost.
    while (!pending.empty()) {
      skymp::wire::WireEvent ev = std::move(pending.front());
      pending.pop_front();
      Dispatch(ev, onPacket, state);
    }

    const auto now = std::chrono::steady_clock::now();
    if (now - lastMetricsUpdate > std::chrono::seconds(3)) {
      lastMetricsUpdate = now;
      connectedClientsGauge.Set(static_cast<double>(server->client_count()));
    }
  }

  std::string GetIp(Networking::UserId userId) const override
  {
    return std::string(server->client_ip(ClientOf(userId)));
  }

  void CloseConnection(Networking::UserId userId) override
  {
    server->disconnect(ClientOf(userId));
  }

private:
  static constexpr uint64_t kNoClient = 0;

  void Dispatch(const skymp::wire::WireEvent& ev, OnPacket onPacket,
                void* state)
  {
    switch (ev.kind) {
      case skymp::wire::EventKind::Connected: {
        const Networking::UserId userId = Allocate(ev.client);
        if (userId == Networking::InvalidUserId) {
          spdlog::error("WireServer: no free user id for client {:x}",
                        ev.client);
          server->disconnect(ev.client);
          return;
        }
        // The guid the TypeScript login reads through getUserGuid
        const std::string guid = fmt::format("wire-{:016x}", ev.client);
        onPacket(state, userId,
                 Networking::PacketType::ServerSideUserConnect,
                 reinterpret_cast<Networking::PacketData>(guid.data()),
                 guid.size());
        return;
      }
      case skymp::wire::EventKind::Disconnected: {
        const Networking::UserId userId = Find(ev.client);
        if (userId == Networking::InvalidUserId) {
          return;
        }
        spdlog::info("WireServer: user {} left ({})", userId,
                     std::string(ev.detail));
        onPacket(state, userId,
                 Networking::PacketType::ServerSideUserDisconnect, nullptr,
                 0);
        Free(userId);
        return;
      }
      case skymp::wire::EventKind::Message: {
        const Networking::UserId userId = Find(ev.client);
        if (userId == Networking::InvalidUserId) {
          return;
        }
        buffer.clear();
        buffer.push_back(static_cast<char>(Networking::MinPacketId));
        buffer.append(ev.json.data(), ev.json.size());
        onPacket(state, userId, Networking::PacketType::Message,
                 reinterpret_cast<Networking::PacketData>(buffer.data()),
                 buffer.size());
        return;
      }
      case skymp::wire::EventKind::Rejected: {
        auto& n = rejected[ev.reason];
        ++n;
        // the first of each kind, then every thousandth
        if (n == 1 || n % 1000 == 0) {
          spdlog::warn("WireServer: rejected from client {:x}: {} (x{})",
                       ev.client, std::string(ev.detail), n);
        }
        return;
      }
    }
  }

  void LogRefusal(uint16_t code, const std::string& what)
  {
    auto& n = refused[code];
    ++n;
    if (n == 1 || n % 1000 == 0) {
      spdlog::error("WireServer: send refused with code {} (x{}): {}", code,
                    n, what);
    }
  }

  Networking::UserId Allocate(uint64_t client)
  {
    for (size_t i = 0; i < clientsByUser.size(); ++i) {
      if (clientsByUser[i] == kNoClient) {
        clientsByUser[i] = client;
        return static_cast<Networking::UserId>(i);
      }
    }
    return Networking::InvalidUserId;
  }

  Networking::UserId Find(uint64_t client) const
  {
    for (size_t i = 0; i < clientsByUser.size(); ++i) {
      if (clientsByUser[i] == client) {
        return static_cast<Networking::UserId>(i);
      }
    }
    return Networking::InvalidUserId;
  }

  void Free(Networking::UserId userId)
  {
    if (userId < clientsByUser.size()) {
      clientsByUser[userId] = kNoClient;
    }
  }

  uint64_t ClientOf(Networking::UserId userId) const
  {
    if (userId >= clientsByUser.size() ||
        clientsByUser[userId] == kNoClient) {
      throw std::runtime_error("User with id " + std::to_string(userId) +
                               " doesn't exist");
    }
    return clientsByUser[userId];
  }

  rust::Box<skymp::wire::Server> server;
  std::vector<uint64_t> clientsByUser;
  std::deque<skymp::wire::WireEvent> pending;
  std::string buffer;
  std::map<uint16_t, uint64_t> rejected;
  std::map<uint16_t, uint64_t> refused;
  std::chrono::time_point<std::chrono::steady_clock> lastMetricsUpdate;
  prometheus::Gauge<double&> connectedClientsGauge;
};

} // namespace

std::shared_ptr<Networking::IServer> CreateWireServer(
  const char* listenAddress, unsigned short port,
  unsigned short maxConnections, const char* password,
  std::shared_ptr<prometheus::Registry> promRegistry)
{
  return std::make_shared<WireServer>(listenAddress, port, maxConnections,
                                      password, promRegistry);
}
