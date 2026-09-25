// fakeclient: a headless client for the legacy SkyMP protocol, Linux-capable.
//
// Reuses mp_common's Networking::CreateClient (the same SLikeNet client the
// game's MpClientPlugin wraps) and the MessageSerializer, so every byte on
// the wire is what the real client would send. Two modes:
//   smoke (default): connect, log in with a profile id, wait for our actor,
//     send a few movement updates near the spawn, AddItem through a console
//     command, answer SpSnippets, and exit 0.
//   --script FILE: after the login handshake, replay JSON lines, one per step,
//     each with "at_ms" (milliseconds since the script started) and one of:
//       "move": {"dx", "dy", "dz", "runMode"}   an UpdateMovement at spawn + offset
//       "send": {...legacy message json...}      sent as is; the strings
//                "{{idx}}" and "{{worldOrCell}}" become this client's numbers
//     plus an optional "reliable" (default true). difftest's legacy driver
//     writes these; the events on stdout are its evidence.
// Output: one JSON object per line on stdout, {"event": ...}. Exit 0 on
// success, 1 on any failure or timeout. thuum Track W step 8 (docs/WIRE.md).
#include "Config.h"
#include "MessageSerializerFactory.h"
#include "MsgType.h"
#include "Networking.h"

#include <nlohmann/json.hpp>
#include <slikenet/BitStream.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;

struct Options
{
  std::string host = "127.0.0.1";
  uint16_t port = 7777;
  std::string password; // the server's "password" setting; the protocol
                        // prefix is added, as MpClientPlugin does
  int profileId = 1;
  int timeoutMs = 20000; // per wait
  int moves = 5;
  int settleMs = 2000;
  uint32_t addItemBase = 0x00012EB7; // IronSword, Skyrim.esm
  int addItemCount = 1;
  std::string script;
};

void Emit(const nlohmann::json& j)
{
  std::printf("%s\n", j.dump().c_str());
  std::fflush(stdout);
}

struct Client
{
  std::shared_ptr<Networking::IClient> cl;
  std::shared_ptr<MessageSerializer> serializer =
    MessageSerializerFactory::CreateMessageSerializer();
  bool failed = false;
  std::string failure;
  std::optional<uint32_t> myIdx;
  uint32_t worldOrCell = 0;
  std::array<float, 3> pos{ 0.f, 0.f, 0.f };
  std::array<float, 3> rot{ 0.f, 0.f, 0.f };
  size_t received = 0;
  std::vector<nlohmann::json> pendingSnippets;

  void Send(const nlohmann::json& j, bool reliable)
  {
    SLNet::BitStream stream;
    const std::string dump = j.dump();
    serializer->Serialize(dump.c_str(), stream);
    cl->Send(stream.GetData(), stream.GetNumberOfBytesUsed(), reliable);
    Emit({ { "event", "sent" }, { "msg", j } });
  }

  void OnMessage(Networking::PacketData data, size_t length)
  {
    nlohmann::json msg;
    if (auto result = serializer->Deserialize(data, length)) {
      result->message->WriteJson(msg);
    } else if (length > 1) {
      // JSON text after the packet id, as MpClientPlugin falls back to
      std::string raw(reinterpret_cast<const char*>(data) + 1, length - 1);
      msg = nlohmann::json::parse(raw, nullptr, false);
      if (msg.is_discarded()) {
        msg = nlohmann::json{ { "raw", raw } };
      }
    }
    ++received;
    Emit({ { "event", "message" }, { "msg", msg } });
    const int t = msg.is_object() ? msg.value("t", -1) : -1;
    if (t == static_cast<int>(MsgType::CreateActor) &&
        msg.value("isMe", false)) {
      myIdx = msg.value("idx", 0u);
      if (msg.contains("transform")) {
        const auto& tr = msg["transform"];
        worldOrCell = tr.value("worldOrCell", 0u);
        for (size_t i = 0; i < 3; ++i) {
          pos[i] = tr["pos"][i].get<float>();
          rot[i] = tr["rot"][i].get<float>();
        }
      }
      Emit({ { "event", "actor" },
             { "idx", *myIdx },
             { "worldOrCell", worldOrCell },
             { "pos", pos },
             { "rot", rot } });
    } else if (t == static_cast<int>(MsgType::SpSnippet)) {
      pendingSnippets.push_back(msg);
    }
  }

  static void OnPacket(void* state, Networking::PacketType type,
                       Networking::PacketData data, size_t length,
                       const char* error)
  {
    auto* self = static_cast<Client*>(state);
    switch (type) {
      case Networking::PacketType::Message:
        self->OnMessage(data, length);
        break;
      case Networking::PacketType::ClientSideConnectionAccepted:
        Emit({ { "event", "connected" } });
        break;
      case Networking::PacketType::ClientSideConnectionFailed:
      case Networking::PacketType::ClientSideConnectionDenied:
      case Networking::PacketType::ClientSideDisconnect:
        self->failed = true;
        self->failure = error ? error : "connection lost";
        Emit({ { "event", "error" },
               { "error", self->failure },
               { "packetType", static_cast<int>(type) } });
        break;
      default:
        break;
    }
  }

  void AnswerSnippets()
  {
    auto snippets = std::move(pendingSnippets);
    pendingSnippets.clear();
    for (auto& s : snippets) {
      const auto idx = s.value("snippetIdx", int64_t{ -1 });
      // 0xFFFFFFFF means the server wants no result (SpSnippet.cpp)
      if (idx >= 0 && idx != static_cast<int64_t>(0xFFFFFFFF)) {
        Send({ { "t", static_cast<int>(MsgType::FinishSpSnippet) },
               { "returnValue", nullptr },
               { "snippetIdx", idx } },
             true);
      }
    }
  }

  void Tick()
  {
    cl->Tick(&Client::OnPacket, this);
    AnswerSnippets();
  }

  template <class Pred>
  bool WaitFor(int timeoutMs, Pred pred)
  {
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (Clock::now() < deadline) {
      Tick();
      if (failed) {
        return false;
      }
      if (pred()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
  }

  void Settle(int ms)
  {
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline) {
      Tick();
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
};

nlohmann::json LoginMessage(int profileId)
{
  nlohmann::json content = {
    { "customPacketType", "loginWithSkympIo" },
    { "gameData", { { "profileId", profileId } } },
  };
  return { { "t", static_cast<int>(MsgType::CustomPacket) },
           { "contentJsonDump", content.dump() } };
}

nlohmann::json MovementMessage(const Client& c, float dx, float dy = 0.f,
                               float dz = 0.f,
                               const std::string& runMode = "Walking")
{
  const std::array<float, 3> p{ c.pos[0] + dx, c.pos[1] + dy, c.pos[2] + dz };
  return { { "t", static_cast<int>(MsgType::UpdateMovement) },
           { "idx", *c.myIdx },
           { "data",
             { { "worldOrCell", c.worldOrCell },
               { "pos", p },
               { "rot", c.rot },
               { "direction", 0.0 },
               { "healthPercentage", 1.0 },
               { "speed", 0.0 },
               { "runMode", runMode },
               { "isInJumpState", false },
               { "isSneaking", false },
               { "isBlocking", false },
               { "isWeapDrawn", false },
               { "isDead", false } } } };
}

nlohmann::json AddItemMessage(uint32_t base, int count)
{
  return { { "t", static_cast<int>(MsgType::ConsoleCommand) },
           { "data",
             { { "commandName", "AddItem" },
               { "args",
                 { int64_t{ 0x14 }, static_cast<int64_t>(base),
                   int64_t{ count } } } } } };
}

bool ParseOptions(int argc, char** argv, Options& o)
{
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](std::string& out) {
      if (i + 1 >= argc) {
        return false;
      }
      out = argv[++i];
      return true;
    };
    std::string v;
    if (a == "--host" && next(v)) {
      o.host = v;
    } else if (a == "--port" && next(v)) {
      o.port = static_cast<uint16_t>(std::stoul(v));
    } else if (a == "--password" && next(v)) {
      o.password = v;
    } else if (a == "--profile-id" && next(v)) {
      o.profileId = std::stoi(v);
    } else if (a == "--timeout-ms" && next(v)) {
      o.timeoutMs = std::stoi(v);
    } else if (a == "--moves" && next(v)) {
      o.moves = std::stoi(v);
    } else if (a == "--settle-ms" && next(v)) {
      o.settleMs = std::stoi(v);
    } else if (a == "--add-item" && next(v)) {
      o.addItemBase = static_cast<uint32_t>(std::stoul(v, nullptr, 0));
    } else if (a == "--add-item-count" && next(v)) {
      o.addItemCount = std::stoi(v);
    } else if (a == "--script" && next(v)) {
      o.script = v;
    } else {
      std::fprintf(stderr, "unknown or incomplete option: %s\n", a.c_str());
      return false;
    }
  }
  return true;
}

void ReplaceAll(std::string& text, const std::string& from,
                const std::string& to)
{
  for (size_t pos = text.find(from); pos != std::string::npos;
       pos = text.find(from, pos + to.size())) {
    text.replace(pos, from.size(), to);
  }
}

int RunScript(Client& c, const Options& o)
{
  std::ifstream in(o.script);
  if (!in) {
    Emit({ { "event", "error" }, { "error", "cannot open script" } });
    return 1;
  }
  const auto start = Clock::now();
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    auto step = nlohmann::json::parse(line, nullptr, false);
    if (step.is_discarded() || !(step.contains("send") || step.contains("move"))) {
      Emit({ { "event", "error" }, { "error", "bad script line" } });
      return 1;
    }
    const auto atMs = step.value("at_ms", int64_t{ 0 });
    const auto due = start + std::chrono::milliseconds(atMs);
    while (Clock::now() < due) {
      c.Tick();
      if (c.failed) {
        return 1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool reliable = step.value("reliable", true);
    if (step.contains("move")) {
      const auto& m = step["move"];
      c.Send(MovementMessage(c, m.value("dx", 0.f), m.value("dy", 0.f),
                             m.value("dz", 0.f),
                             m.value("runMode", std::string("Walking"))),
             false);
      continue;
    }
    std::string raw = step["send"].dump();
    ReplaceAll(raw, "\"{{idx}}\"", std::to_string(*c.myIdx));
    ReplaceAll(raw, "\"{{worldOrCell}}\"", std::to_string(c.worldOrCell));
    auto msg = nlohmann::json::parse(raw, nullptr, false);
    if (msg.is_discarded()) {
      Emit({ { "event", "error" }, { "error", "bad send after substitution" } });
      return 1;
    }
    c.Send(msg, reliable);
  }
  c.Settle(o.settleMs);
  return c.failed ? 1 : 0;
}
} // namespace

int main(int argc, char** argv)
{
  Options o;
  if (!ParseOptions(argc, argv, o)) {
    return 2;
  }
  Client c;
  const std::string password = std::string(kNetworkingPasswordPrefix) + o.password;
  c.cl = Networking::CreateClient(o.host.c_str(), o.port, o.timeoutMs, password.c_str());

  if (!c.WaitFor(o.timeoutMs, [&] { return c.cl->IsConnected(); })) {
    Emit({ { "event", "error" }, { "error", "connect timed out" } });
    return 1;
  }
  c.Send(LoginMessage(o.profileId), true);
  if (!c.WaitFor(o.timeoutMs, [&] { return c.myIdx.has_value(); })) {
    Emit({ { "event", "error" }, { "error", "no CreateActor with isMe" } });
    return 1;
  }

  int rc = 0;
  if (!o.script.empty()) {
    rc = RunScript(c, o);
  } else {
    for (int i = 1; i <= o.moves; ++i) {
      c.Send(MovementMessage(c, 30.f * static_cast<float>(i)), false);
      c.Settle(130);
    }
    c.Send(AddItemMessage(o.addItemBase, o.addItemCount), true);
    c.Settle(o.settleMs);
    rc = c.failed ? 1 : 0;
  }
  Emit({ { "event", "done" }, { "received", c.received }, { "rc", rc } });
  return rc;
}
