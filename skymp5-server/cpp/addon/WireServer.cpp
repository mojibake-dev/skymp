#include "WireServer.h"

#include "MinPacketId.h"
#include "wire_bridge_cxx/lib.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
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
    , clientsByUser(maxConnections)
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
      LogRefusal(id, 0, std::string(), "not a SkyMP packet");
      return;
    }
    // The core writes 0x86 + JSON (MessageSerializer); Rust takes the JSON.
    const char* text = reinterpret_cast<const char*>(data + 1);
    const size_t textLength = length - 1;
    try {
      rust::Str json(text, textLength); // checks UTF-8, throws if not
      if (uint16_t code = server->send(client, json, reliable)) {
        LogRefusal(id, code,
                   std::string(skymp::wire::send_refused_name(json)),
                   std::string(text, std::min<size_t>(textLength, 160)));
      }
    } catch (const std::invalid_argument&) {
      LogRefusal(id, 400, std::string(), "not UTF-8");
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

  int64_t GetRttMs(Networking::UserId userId) const override
  {
    if (userId >= clientsByUser.size() || !clientsByUser[userId]) {
      return -1;
    }
    return server->client_rtt_ms(*clientsByUser[userId]);
  }

  int64_t GetSilenceMs(Networking::UserId userId) const override
  {
    if (userId >= clientsByUser.size() || !clientsByUser[userId]) {
      return -1;
    }
    return server->client_silence_ms(*clientsByUser[userId]);
  }

  void CloseConnection(Networking::UserId userId) override
  {
    server->disconnect(ClientOf(userId));
  }

private:
  void Dispatch(const skymp::wire::WireEvent& ev, OnPacket onPacket,
                void* state)
  {
    switch (ev.kind) {
      case skymp::wire::EventKind::Connected: {
        if (Find(ev.client) != Networking::InvalidUserId) {
          spdlog::error("WireServer: client {:x} connected twice", ev.client);
          return;
        }
        const Networking::UserId userId = Allocate(ev.client);
        if (userId == Networking::InvalidUserId) {
          spdlog::error("WireServer: no free user id for client {:x}",
                        ev.client);
          server->disconnect(ev.client);
          return;
        }
        // The guid the TypeScript login reads through getUserGuid
        const std::string guid = fmt::format("wire-{:016x}", ev.client);
        onPacket(state, userId, Networking::PacketType::ServerSideUserConnect,
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
        // The id stays valid during the callback (the core may still ask
        // for its ip) and is freed after it, even if the core throws.
        struct FreeAfter
        {
          WireServer* self;
          Networking::UserId userId;
          ~FreeAfter() { self->Free(userId); }
        } freeAfter{ this, userId };
        onPacket(state, userId,
                 Networking::PacketType::ServerSideUserDisconnect, nullptr, 0);
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
        // thuum ADR-073 decision 7: counted per (code, message), the
        // detail naming the message ("E_VAL_RATE (SpellCast)"), the first of
        // each in full and then at its 2nd, 4th, 8th and so on. Keyed by
        // the code alone and logged at the first and every thousandth, the
        // one E_VAL_RATE line of x-drain-watch 20261115-032536 named no
        // type and every later one was silent. The message is the bridge's
        // name for it, not its MsgType: a cast's end ("SpellCast end") is
        // counted apart from the casts' refused re-reports, which share
        // type 23, and the M0 family apart from each other, which share 0
        // (the review of m4-fix-cast-end, finding 1)
        auto& n = rejected[{ ev.reason, std::string(ev.refused) }];
        ++n;
        if ((n & (n - 1)) == 0) {
          const Networking::UserId userId = Find(ev.client);
          spdlog::warn("WireServer: rejected from client {:x}{}: {} (x{})",
                       ev.client,
                       userId == Networking::InvalidUserId
                         ? std::string()
                         : fmt::format(" (user {})", userId),
                       std::string(ev.detail), n);
        }
        return;
      }
    }
  }

  // thuum ADR-073 decision 7, the inbound refusals' shape: a send the wire
  // refused, counted per (code, message name, empty when the JSON is not a
  // message), said in full at each key's first and then at its 2nd, 4th,
  // 8th and so on, with the user and the message. Keyed by the code alone
  // and said at the first and every thousandth, a relayed end the wire
  // refused after its code's first refusal (E_TX_LIMIT on a full channel,
  // 304 for a client gone) was silent while the end's line still said
  // "relayed to 1 game(s)" (the review of m4-fix-cast-end, finding 2)
  void LogRefusal(Networking::UserId userId, uint16_t code,
                  const std::string& name, const std::string& what)
  {
    auto& n = refused[{ code, name }];
    ++n;
    if ((n & (n - 1)) == 0) {
      spdlog::error("WireServer: send to user {} refused with code {}{} "
                    "(x{}): {}",
                    userId, code,
                    name.empty() ? std::string() : fmt::format(" ({})", name),
                    n, what);
    }
  }

  // Any u64 is a valid netcode client id, 0 included: a slot is free when
  // it holds no id, never when it holds a particular one.
  Networking::UserId Allocate(uint64_t client)
  {
    for (size_t i = 0; i < clientsByUser.size(); ++i) {
      if (!clientsByUser[i]) {
        clientsByUser[i] = client;
        return static_cast<Networking::UserId>(i);
      }
    }
    return Networking::InvalidUserId;
  }

  Networking::UserId Find(uint64_t client) const
  {
    for (size_t i = 0; i < clientsByUser.size(); ++i) {
      if (clientsByUser[i] == client) { // an empty slot never matches
        return static_cast<Networking::UserId>(i);
      }
    }
    return Networking::InvalidUserId;
  }

  void Free(Networking::UserId userId)
  {
    if (userId < clientsByUser.size()) {
      clientsByUser[userId].reset();
    }
  }

  uint64_t ClientOf(Networking::UserId userId) const
  {
    if (userId >= clientsByUser.size() || !clientsByUser[userId]) {
      throw std::runtime_error("User with id " + std::to_string(userId) +
                               " doesn't exist");
    }
    return *clientsByUser[userId];
  }

  rust::Box<skymp::wire::Server> server;
  std::vector<std::optional<uint64_t>> clientsByUser;
  std::deque<skymp::wire::WireEvent> pending;
  std::string buffer;
  // by (reason code, the refused message's name, empty for none); the
  // names are a fixed set (the schema's variants and "SpellCast end")
  std::map<std::pair<uint16_t, std::string>, uint64_t> rejected;
  // by (reason code, the refused message's name, empty for none)
  std::map<std::pair<uint16_t, std::string>, uint64_t> refused;
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
