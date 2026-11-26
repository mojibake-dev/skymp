#include "ActionListener.h"
#include "ActorClaimMessage.h"
#include "ActorSpeechMessage.h"
#include "AliasFillMessage.h"
#include "ChangeValuesMessage.h"
#include "ConsoleCommandMessage.h"
#include "CreateActorMessage.h"
#include "DestroyActorMessage.h"
#include "HitMessage.h"
#include "HostGrantMessage.h"
#include "HostReleaseMessage.h"
#include "HostedActorStateMessage.h"
#include "OpenContainerMessage.h"
#include "TeleportMessage.h"
#include "TestUtils.hpp"
#include "UpdateActionMessage.h"
#include "UpdateAnimationMessage.h"
#include "UpdateEquipmentMessage.h"
#include "UpdateMovementMessage.h"
#include "UpdatePropertyMessage.h"
#include "libespm/Loader.h"
#include "libespm/WEAP.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <cmath>
#include <map>
#include <optional>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <vector>

PartOne& GetPartOne();

// thuum docs/verbs/cell-host.md (ADR-035): one player's game runs a cell's
// NPCs and the server knows which. The first game to have the cell loaded
// while the server's grid holds it hosts it; a second gets nothing; the
// host's reports for the cell's actors pass and anyone else's are dropped;
// a hosted actor crosses into another cell only under its host; the host's
// departure starts a grace, at whose end the visitor with the cell loaded
// takes it at the next epoch.

namespace {
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kC1 = 0xff000c01;
constexpr uint32_t kC2 = 0xff000c02;

// a worn inventory entry's extra, as HitTest arms its aggressor (the helper
// is that file's own, so it is spelled out here too)
const auto kWornExtra = [] {
  Inventory::ExtraData extra;
  extra.worn_ = true;
  return extra;
}();
constexpr uint32_t kWolf = 0xff000c03;
constexpr uint32_t kC3 = 0xff000c04;
// Whiterun's cow, an actor from Skyrim.esm (unit/NpcExists.cpp), in the
// Whiterun worldspace
constexpr uint32_t kCow = 0x10ebaf;
const uint32_t kWhiterun = ToUnderlying(Constants::kWhiterunCell);

// the exterior grid square (2, 0) of Tamriel: x in [8192, 12288); and the
// next square east, 4500 units on, beyond the 4096 a movement report may
// move an actor
const NiPoint3 kHere{ 9000.f, 500.f, 0.f };
const NiPoint3 kEast{ 13500.f, 500.f, 0.f };

// the server's log lines while it lives (AliasFillTest's shape), restored
// at its end
struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("cell-host-test", sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
  }
  ~LogCapture() { spdlog::set_default_logger(old); }
  size_t Count(const std::string& line) const
  {
    const std::string log = oss.str();
    size_t n = 0;
    for (size_t at = log.find(line); at != std::string::npos;
         at = log.find(line, at + line.size())) {
      ++n;
    }
    return n;
  }
};

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// The grace is stamped by the server's own clock reading inside the
// disconnect or the movement report, a few milliseconds apart from a reading
// the test takes around it (on Windows the gap reached past a millisecond,
// GitHub run 39618997497: a tick 1 ms inside the deadline fired, one 1 ms
// past it did not). So a test reads its clock before the act that arms the
// grace, ticks well inside the deadline and well past it.
constexpr uint64_t kInsideMs = 2'500;
constexpr uint64_t kPastMs = 2'000;

MpActor& Player(PartOne& p, Networking::UserId user, uint32_t id,
                const NiPoint3& pos)
{
  DoConnect(p, user);
  p.CreateActor(id, pos, 0, kTamriel);
  p.SetUserActor(user, id);
  return p.worldState.GetFormAt<MpActor>(id);
}

void CellLoaded(PartOne& p, Networking::UserId user, uint32_t cell,
                bool loaded = true)
{
  DoMessage(p, user,
            nlohmann::json{ { "t", MsgType::CellLoaded },
                            { "cell", cell },
                            { "loaded", loaded } });
}

// `user`'s movement report for `actorId`, standing at `pos`, its weapon
// drawn when `weapDrawn`
void Move(PartOne& p, Networking::UserId user, uint32_t actorId,
          const NiPoint3& pos, uint32_t worldOrCell = kTamriel,
          bool weapDrawn = false)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateMovementMessage msg;
  msg.idx = p.worldState.GetFormAt<MpActor>(actorId).GetIdx();
  msg.data.pos = { pos.x, pos.y, pos.z };
  msg.data.rot = { 0, 0, 0 };
  msg.data.isInJumpState = false;
  msg.data.isWeapDrawn = weapDrawn;
  msg.data.isBlocking = false;
  msg.data.worldOrCell = worldOrCell;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
}

// `user`'s action report for `actorId`: the message the host's hook raises
// for an action the actor performed (thuum docs/verbs/npc-stream.md); any
// action id serves, the wire validates the real ones
void Act(PartOne& p, Networking::UserId user, uint32_t actorId)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateActionMessage msg;
  msg.idx = p.worldState.GetFormAt<MpActor>(actorId).GetIdx();
  msg.action = 1;
  msg.eventName = "attackStart";
  msg.type = 2;
  p.GetActionListener().OnUpdateAction(raw, msg);
}

// `user`'s state report for `actorId` (thuum docs/verbs/npc-stream.md): the
// three percentages, the combat target and the package as the host's engine
// has them
void State(PartOne& p, Networking::UserId user, uint32_t actorId, float health,
           float magicka, float stamina, uint32_t target, uint32_t package,
           uint32_t gazeTarget = 0, uint32_t gazeSlot = 0,
           uint32_t emotionType = 0, uint32_t emotionValue = 0,
           std::optional<std::vector<uint32_t>> effects = std::nullopt,
           std::optional<std::vector<HostedActorStateMessage::PlayerDetection>>
             detects = std::nullopt)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  HostedActorStateMessage msg;
  msg.idx = p.worldState.GetFormAt<MpActor>(actorId).GetIdx();
  msg.health = health;
  msg.magicka = magicka;
  msg.stamina = stamina;
  msg.combatTarget = target;
  msg.package = package;
  msg.gazeTarget = gazeTarget;
  msg.gazeSlot = gazeSlot;
  msg.emotionType = emotionType;
  msg.emotionValue = emotionValue;
  msg.effects = std::move(effects);
  msg.detects = std::move(detects);
  p.GetActionListener().OnHostedActorState(raw, msg);
}

// every UpdateProperty (t 7) sent to `user`, as the JSON the wire carries
std::vector<nlohmann::json> PropertiesSentTo(PartOne& p,
                                             Networking::UserId user)
{
  std::vector<nlohmann::json> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(MsgType::UpdateProperty)) {
      out.push_back(m.j);
    }
  }
  return out;
}

// the last isHostedByOther `user` was sent for `actor` ("true", "false"),
// or "" when none was: the word a game keeps. Matched by the actor's form
// id (the cases share one PartOne, so an idx can be reused by a later
// actor), and counted: the host hears exactly one word per grant
std::string LastHostedWord(PartOne& p, Networking::UserId user, MpActor& actor,
                           size_t* count = nullptr,
                           std::string* baseRecordType = nullptr)
{
  std::string word;
  size_t n = 0;
  for (const auto& j : PropertiesSentTo(p, user)) {
    if (j.value("propName", std::string()) == "isHostedByOther" &&
        j.value("refrId", 0u) == actor.GetFormId()) {
      word = j.value("dataDump", std::string());
      if (baseRecordType) {
        *baseRecordType = j.value("baseRecordType", std::string());
      }
      ++n;
    }
  }
  if (count) {
    *count = n;
  }
  return word;
}

// every message sent to `user`, one line each, for a failing case's report
std::string MessagesSentTo(PartOne& p, Networking::UserId user)
{
  std::string out;
  for (auto& m : p.Messages()) {
    if (m.userId == user) {
      out += m.j.dump().substr(0, 160) + "\n";
    }
  }
  return out;
}

template <class Message>
std::vector<Message> SentTo(PartOne& p, Networking::UserId user)
{
  std::vector<Message> out;
  for (auto& m : p.Messages()) {
    if (m.userId != user) {
      continue;
    }
    if (auto typed = dynamic_cast<Message*>(m.message.get())) {
      out.push_back(*typed);
    }
  }
  return out;
}
}

TEST_CASE("The first loader in reach hosts the cell and alone moves its "
          "actors; the host's departure passes the cell on after the grace",
          "[CellHost]")
{
  auto& p = GetPartOne();
  Player(p, 0, kC1, kHere);
  Player(p, 1, kC2, kHere + NiPoint3{ 100.f, 0.f, 0.f });
  p.CreateActor(kWolf, kHere + NiPoint3{ 50.f, 50.f, 0.f }, 0, kTamriel);
  auto& wolf = p.worldState.GetFormAt<MpActor>(kWolf);
  // a server-made actor joins the grid and subscribes at its own first
  // SetPos (ForceSubscriptionsUpdate while nothing has subscribed it;
  // PartOne::CreateActor writes no position of its own, and a standing
  // player's report runs no pass: pipelines 2557 and 2560, "actor listeners
  // 0"); in the live game the host's first report for it does this
  wolf.SetPos(wolf.GetPos());
  REQUIRE(wolf.GetActorListeners().size() >= 2);
  p.Messages().clear();

  const auto cell = p.worldState.CellOf(wolf);
  REQUIRE(cell.has_value());
  REQUIRE(*cell != 0);
  REQUIRE_FALSE(p.GetCellHost(*cell).hosted);
  // the cell the 3x3 grid gives both players, so a loaded report grants it
  REQUIRE(p.worldState.CellOf(kTamriel, kHere) == cell);

  // c1 loads it first: the grant, epoch 1
  CellLoaded(p, 0, *cell);
  auto host = p.GetCellHost(*cell);
  REQUIRE(host.hosted);
  REQUIRE(host.player == kC1);
  REQUIRE(host.epoch == 1);
  auto grants = SentTo<HostGrantMessage>(p, 0);
  REQUIRE(grants.size() == 1);
  REQUIRE(grants.front().cell == *cell);
  REQUIRE(grants.front().epoch == 1);
  REQUIRE(p.worldState.HostOf(wolf) == &p.worldState.GetFormAt<MpActor>(kC1));
  // a server-made actor (a 0xff id, a copy on every client) is told to its
  // host as nobody else's (docs/verbs/npc-puppet.md; a game-file actor the
  // same, the case below). The LAST word is read: pipeline 2594's first
  // server-build (job 10667) found an earlier "true" in c1's stream once in
  // four runs, an ordering the client resolves the same way (task #21 reads
  // where it comes from)
  {
    UNSCOPED_INFO(
      "after the grant, to c1: "
      << MessagesSentTo(p, 0) << "wolf idx " << wolf.GetIdx()
      << ", actor listeners " << wolf.GetActorListeners().size()
      << ", c1 emitters "
      << p.worldState.GetFormAt<MpActor>(kC1).GetEmitters().size());
    size_t words = 0;
    REQUIRE(LastHostedWord(p, 0, wolf, &words) == "false");
    // one word, c1's own: an offline neighbour's word is nobody's to hear
    // (UpdateHoster sends only to listeners with a user)
    REQUIRE(words == 1);
  }

  // c2 loads it too: nothing
  CellLoaded(p, 1, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  REQUIRE(p.GetCellHost(*cell).epoch == 1);
  REQUIRE(SentTo<HostGrantMessage>(p, 1).empty());

  // the host moves the wolf; the other's report is dropped
  Move(p, 0, kWolf, kHere + NiPoint3{ 60.f, 50.f, 0.f });
  REQUIRE(wolf.GetPos().x == Catch::Approx(kHere.x + 60.f));
  Move(p, 1, kWolf, kHere + NiPoint3{ 70.f, 50.f, 0.f });
  REQUIRE(wolf.GetPos().x == Catch::Approx(kHere.x + 60.f));

  // the host's report reaches the other game and is not echoed to the host
  // (the echo replayed the wolf's own events on it and spun it, playtest
  // thirty); a raw relay lands in the fake send target as an empty object
  p.Messages().clear();
  Move(p, 0, kWolf, kHere + NiPoint3{ 60.f, 50.f, 0.f }); // standing still
  const auto relays = [&](Networking::UserId user) {
    size_t n = 0;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.empty()) {
        ++n;
      }
    }
    return n;
  };
  REQUIRE(relays(1) == 1);
  REQUIRE(relays(0) == 0);

  // the cell-change rule: the wolf into the next cell east, which nobody
  // hosts: dropped; once c1 has it loaded (and so hosts it), the same move
  // passes, 4500 units in one report
  const auto east = p.worldState.CellOf(kTamriel, kEast);
  REQUIRE(east.has_value());
  REQUIRE(east != cell);
  Move(p, 0, kWolf, kEast);
  REQUIRE(wolf.GetPos().x == Catch::Approx(kHere.x + 60.f));
  CellLoaded(p, 0, *east);
  REQUIRE(p.GetCellHost(*east).player == kC1);
  Move(p, 0, kWolf, kEast);
  REQUIRE(wolf.GetPos().x == Catch::Approx(kEast.x));
  REQUIRE(p.worldState.CellOf(wolf) == east);
  // and back west, hosted at both ends
  Move(p, 0, kWolf, kHere + NiPoint3{ 50.f, 50.f, 0.f });
  REQUIRE(p.worldState.CellOf(wolf) == cell);

  // c1 leaves: the grace runs; at its end c2, with the cell loaded and in
  // reach, takes it at epoch 2 and is told; the east cell, loaded by
  // nobody else, goes unhosted with its epoch kept
  p.Messages().clear();
  const auto now = NowMs();
  DoDisconnect(p, 0);
  p.TickCellHosts(now + kInsideMs);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  p.TickCellHosts(now + skymp::rules::host_grace_ms() + kPastMs);
  host = p.GetCellHost(*cell);
  REQUIRE(host.hosted);
  REQUIRE(host.player == kC2);
  REQUIRE(host.epoch == 2);
  grants = SentTo<HostGrantMessage>(p, 1);
  REQUIRE(grants.size() == 1);
  REQUIRE(grants.front().epoch == 2);
  REQUIRE_FALSE(p.GetCellHost(*east).hosted);
  REQUIRE(p.GetCellHost(*east).epoch == 1);
  REQUIRE(p.worldState.HostOf(wolf) == &p.worldState.GetFormAt<MpActor>(kC2));

  // the new host moves the wolf
  Move(p, 1, kWolf, kHere + NiPoint3{ 80.f, 50.f, 0.f });
  REQUIRE(wolf.GetPos().x == Catch::Approx(kHere.x + 80.f));

  DoDisconnect(p, 1);
  p.DestroyActor(kWolf);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

TEST_CASE("A cell loaded before the grid reaches it is granted when the "
          "player walks in, and the grid leaving arms the grace",
          "[CellHost]")
{
  auto& p = GetPartOne();
  auto& c1 = Player(p, 0, kC1, kHere);
  p.Messages().clear();

  // two squares east: loaded by the game's 5x5, outside the server's 3x3
  const NiPoint3 far{ kHere.x + 2 * 4096.f, kHere.y, kHere.z };
  const auto farCell = p.worldState.CellOf(kTamriel, far);
  REQUIRE(farCell.has_value());
  CellLoaded(p, 0, *farCell);
  REQUIRE_FALSE(p.GetCellHost(*farCell).hosted);
  REQUIRE(SentTo<HostGrantMessage>(p, 0).empty());

  // the player one square east (placed by the server, as its teleports
  // do; a 4000-unit step would exceed the ground budget) and its report
  // from there: the grid reaches the cell, the grant follows
  const NiPoint3 east{ kHere.x + 4000.f, kHere.y, kHere.z };
  c1.SetPos(east);
  Move(p, 0, kC1, east);
  REQUIRE(c1.GetPos().x == Catch::Approx(east.x));
  auto host = p.GetCellHost(*farCell);
  REQUIRE(host.hosted);
  REQUIRE(host.player == kC1);
  REQUIRE(host.epoch == 1);
  REQUIRE(SentTo<HostGrantMessage>(p, 0).size() == 1);

  // back west: the grid drops the cell (still loaded), the grace arms and,
  // with no visitor, the cell goes unhosted at its end, the host told
  const auto now = NowMs();
  c1.SetPos(kHere);
  Move(p, 0, kC1, kHere);
  REQUIRE(p.GetCellHost(*farCell).player == kC1);
  p.TickCellHosts(now + kInsideMs);
  REQUIRE(p.GetCellHost(*farCell).player == kC1);
  p.TickCellHosts(now + skymp::rules::host_grace_ms() + kPastMs);
  REQUIRE_FALSE(p.GetCellHost(*farCell).hosted);
  REQUIRE(p.GetCellHost(*farCell).epoch == 1);
  auto releases = SentTo<HostReleaseMessage>(p, 0);
  REQUIRE(releases.size() == 1);
  REQUIRE(releases.front().cell == *farCell);
  REQUIRE(releases.front().epoch == 1);

  DoDisconnect(p, 0);
  p.DestroyActor(kC1);
}

TEST_CASE("At the handoff the visitor with the lowest round trip takes the "
          "cell, read from the server's connections",
          "[CellHost]")
{
  auto& p = GetPartOne();
  Player(p, 0, kC1, kHere);
  Player(p, 1, kC2, kHere + NiPoint3{ 100.f, 0.f, 0.f });
  Player(p, 2, kC3, kHere + NiPoint3{ 200.f, 0.f, 0.f });
  // user 1 is the far one, user 2 the near one; the host's does not matter
  p.SetRoundTripSource([](Networking::UserId userId) -> int64_t {
    switch (userId) {
      case 1:
        return 90;
      case 2:
        return 30;
      default:
        return -1;
    }
  });
  const auto cell = p.worldState.CellOf(kTamriel, kHere);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  CellLoaded(p, 1, *cell);
  CellLoaded(p, 2, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  const auto now = NowMs();
  // a tick reads the round trips in
  p.TickCellHosts(now);
  p.Messages().clear();

  DoDisconnect(p, 0);
  p.TickCellHosts(now + kInsideMs);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  p.TickCellHosts(now + skymp::rules::host_grace_ms() + kPastMs);
  const auto host = p.GetCellHost(*cell);
  REQUIRE(host.hosted);
  REQUIRE(host.player == kC3);
  REQUIRE(host.epoch == 2);
  REQUIRE(SentTo<HostGrantMessage>(p, 2).size() == 1);
  REQUIRE(SentTo<HostGrantMessage>(p, 1).empty());

  DoDisconnect(p, 1);
  DoDisconnect(p, 2);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kC3);
}

// thuum docs/verbs/cell-host.md, the quitting host (playtest 32, 2026-10-22):
// a game killed, crashed or quit sends netcode no disconnect; the connection
// stood for the transport's timeout (about 18 s) before the grace began,
// and the village belonged to a gone game for about 24 s. The server now
// reads each connection's silence itself (IServer::GetSilenceMs) at every
// tick: a host silent past kStaleHostMs starts its grace at once and is no
// candidate; heard again inside the grace it keeps its cell at the same
// epoch; heard again after the cell passed it is a candidate again.
TEST_CASE("A host whose connection falls silent passes its cell at the "
          "grace without a disconnect, and keeps it when heard again inside",
          "[CellHost]")
{
  auto& p = GetPartOne();
  Player(p, 0, kC1, kHere);
  Player(p, 1, kC2, kHere + NiPoint3{ 100.f, 0.f, 0.f });
  // the silence each connection reports, by user (-1 unknown); static, the
  // source outlives the test's frame until it is cleared below
  static std::map<Networking::UserId, int64_t> silence;
  silence = { { 0, 0 }, { 1, 0 } };
  p.SetSilenceSource([](Networking::UserId userId) -> int64_t {
    const auto it = silence.find(userId);
    return it == silence.end() ? -1 : it->second;
  });
  const auto cell = p.worldState.CellOf(kTamriel, kHere);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  CellLoaded(p, 1, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  const auto now = NowMs();
  p.TickCellHosts(now);
  p.Messages().clear();

  // the host's game stops: its silence grows past the threshold and the
  // grace starts at that tick, with no disconnect
  silence[0] = PartOne::kStaleHostMs;
  p.TickCellHosts(now);
  p.TickCellHosts(now + kInsideMs);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  REQUIRE(SentTo<HostReleaseMessage>(p, 0).empty());
  p.TickCellHosts(now + skymp::rules::host_grace_ms() + kPastMs);
  const auto passed = p.GetCellHost(*cell);
  REQUIRE(passed.hosted);
  REQUIRE(passed.player == kC2);
  REQUIRE(passed.epoch == 2);
  REQUIRE(SentTo<HostReleaseMessage>(p, 0).size() == 1);
  REQUIRE(SentTo<HostGrantMessage>(p, 1).size() == 1);

  // the old host is heard again: a candidate again, with nothing to take
  silence[0] = 0;
  const auto revivedAt = now + skymp::rules::host_grace_ms() + kPastMs + 1'000;
  p.TickCellHosts(revivedAt);
  REQUIRE(p.GetCellHost(*cell).player == kC2);
  p.Messages().clear();

  // the new host falls silent and is heard again inside its grace: it keeps
  // the cell at the same epoch and nobody is told anything
  silence[1] = PartOne::kStaleHostMs;
  p.TickCellHosts(revivedAt + 1'000);
  silence[1] = 0;
  p.TickCellHosts(revivedAt + 1'000 + kInsideMs);
  p.TickCellHosts(revivedAt + 1'000 + skymp::rules::host_grace_ms() + kPastMs);
  const auto kept = p.GetCellHost(*cell);
  REQUIRE(kept.hosted);
  REQUIRE(kept.player == kC2);
  REQUIRE(kept.epoch == 2);
  REQUIRE(SentTo<HostReleaseMessage>(p, 1).empty());
  REQUIRE(SentTo<HostGrantMessage>(p, 0).empty());

  p.SetSilenceSource(nullptr);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

// thuum docs/verbs/npc-handoff.md D6 (ADR-047): a host's actor walks into a
// cell nobody hosts that the host's game has loaded (Stump into Gerdur's
// house was refused 751 times in five minutes, playtest 32). The move is a
// follow: accepted, the record moves, the host simulates the actor there and
// stays its listener two squares away; a visitor's report for it is
// dropped; a grant of the cell to the visitor ends the follow and the old
// follower's reports are dropped from then on.
TEST_CASE("A host follows its actor into an unhosted cell its game loads, "
          "and a grant of that cell ends the follow",
          "[CellHost]")
{
  auto& p = GetPartOne();
  Player(p, 0, kC1, kHere);
  // square 4 of Tamriel: loaded by c1's game, two squares from c1 (its
  // reach is squares 1 to 3), adjacent to kEast's square 3
  const NiPoint3 kFar{ 17500.f, 500.f, 0.f };
  const auto here = p.worldState.CellOf(kTamriel, kHere);
  const auto east1 = p.worldState.CellOf(kTamriel, kEast);
  const auto east2 = p.worldState.CellOf(kTamriel, kFar);
  REQUIRE(here.has_value());
  REQUIRE(east1.has_value());
  REQUIRE(east2.has_value());
  CellLoaded(p, 0, *here);
  CellLoaded(p, 0, *east1);
  CellLoaded(p, 0, *east2);
  REQUIRE(p.GetCellHost(*here).player == kC1);
  REQUIRE(p.GetCellHost(*east1).player == kC1);
  REQUIRE_FALSE(p.GetCellHost(*east2).hosted);
  p.CreateActor(kWolf, kEast, 0, kTamriel);
  auto& wolf = p.worldState.GetFormAt<MpActor>(kWolf);
  wolf.SetPos(kEast);
  REQUIRE(p.worldState.CellOf(wolf) == east1);
  REQUIRE(p.SimulatorOf(wolf) == kC1);
  p.Messages().clear();

  // the host moves it one square on, into the loaded cell nobody hosts
  Move(p, 0, kWolf, kFar);
  REQUIRE(p.worldState.CellOf(wolf) == east2);
  REQUIRE(p.SimulatorOf(wolf) == kC1);
  const auto follows = p.GetFollows();
  REQUIRE(follows.size() == 1);
  REQUIRE(follows[0].actor == kWolf);
  REQUIRE(follows[0].player == kC1);
  REQUIRE(follows[0].cell == *east2);
  auto& c1 = p.worldState.GetFormAt<MpActor>(kC1);
  REQUIRE(wolf.GetListeners().count(&c1) == 1);

  // inside the followed cell the follower's reports pass and a visitor's
  // are dropped
  Move(p, 0, kWolf, kFar + NiPoint3{ 50.f, 0.f, 0.f });
  REQUIRE(std::abs(wolf.GetPos().x - 17550.f) < 1.f);
  Player(p, 1, kC2, kFar + NiPoint3{ 0.f, 300.f, 0.f });
  Move(p, 1, kWolf, kFar + NiPoint3{ 500.f, 0.f, 0.f });
  REQUIRE(std::abs(wolf.GetPos().x - 17550.f) < 1.f);

  // the visitor loads the cell: granted, the follow ends, and the old
  // follower's report is dropped
  CellLoaded(p, 1, *east2);
  REQUIRE(p.GetCellHost(*east2).player == kC2);
  REQUIRE(p.SimulatorOf(wolf) == kC2);
  REQUIRE(p.GetFollows().empty());
  Move(p, 0, kWolf, kFar + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(std::abs(wolf.GetPos().x - 17550.f) < 1.f);

  // the facts of the game files around the door rule: the Sleeping Giant
  // Inn is an interior, Riverwood's exterior cell is not; Hod's placed
  // reference is persistent and a server-made actor has no record, a fact
  // the door rule stopped asking at memo D54 B (the non-persistent door
  // cases below)
  REQUIRE(p.worldState.IsInteriorCell(0x133c6));
  REQUIRE_FALSE(p.worldState.IsInteriorCell(0x9732));
  REQUIRE(p.worldState.IsPersistentReference(0x1348a));
  REQUIRE_FALSE(p.worldState.IsPersistentReference(kWolf));

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kWolf);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

// thuum tracker #162 (playtest 55, Eli: "weird scripting shit with draugr not
// standing up", "npc ai is soooo jank rn"): the grid serves a game the 3x3
// squares around its own player, and a cell's host runs every actor of its
// cell. Bleak Falls Temple 0x371de as Ariadne's game hosted it in 55 (both
// seats' logs and c2's recorder): she stood at (3900, -3365), square (0, 0),
// two squares west of the draugr 0x380b3 at its placement (8657, -5215,
// -2055), square (2, -1) (TrapRelayTest's stage, lab/esm.py), while rotfern
// pressed the trigger beside it, and her game held no form for the draugr,
// so the wake-up met an AI-off puppet. The host's roster names an actor its
// game holds no form for, and the server subscribes the host to it now
// (PartOne::OnActorSighting); the host's own walk across a square line keeps
// it (ForceSubscriptionsUpdate, D6's keep on the simulator's edge too),
// which ends the second hall's flicker. The other player, beside the
// draugr, is the host's guest: its own step subscribes the host (D6's
// insert, HostOf naming the cell's host for a player), and the host's steps
// keep it by the same rule, so the draugr the host's engine runs keep a
// figure of the guest to fight
TEST_CASE("A host two squares from an actor in its cell gets the actor's "
          "CreateActor at one roster and keeps it, and a guest of its cell "
          "its step subscribed, across its own square changes",
          "[CellHost][espm]")
{
  constexpr uint32_t kTemple = 0x371de;
  constexpr uint32_t kDraugr = 0x380b3;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.IsInteriorCell(kTemple));

  // the draugr loaded by its id (TrapRelayTest's shape) before any grant, as
  // the record stood before Ariadne's game took the temple
  auto& draugr = p.worldState.GetFormAt<MpActor>(kDraugr);
  const NiPoint3 seat = draugr.GetPos();
  // a grid square as the grid counts it (MpObjectReference.cpp's GetGridPos,
  // truncated toward zero), and whether two points stand farther apart than
  // the 3x3
  const auto square = [](const NiPoint3& pos) {
    return std::pair<int, int>{ static_cast<int16_t>(pos.x / 4096),
                                static_cast<int16_t>(pos.y / 4096) };
  };
  const auto apart = [&](const NiPoint3& a, const NiPoint3& b) {
    const auto sa = square(a);
    const auto sb = square(b);
    return std::abs(sa.first - sb.first) > 1 ||
      std::abs(sa.second - sb.second) > 1;
  };
  REQUIRE(square(seat) == std::pair<int, int>{ 2, -1 });
  // Ariadne's stand, square (0, 0); a step south across the y line, square
  // (0, -1); and one square from the draugr, square (1, 0)
  const NiPoint3 stand{ 3900.f, -3365.f, seat.z };
  const NiPoint3 south{ 3900.f, -4300.f, seat.z };
  const NiPoint3 oneOff{ 5000.f, -3365.f, seat.z };
  REQUIRE(apart(stand, seat));
  REQUIRE(apart(south, seat));
  REQUIRE(square(south) != square(stand));
  REQUIRE_FALSE(apart(oneOff, seat));
  REQUIRE(square(oneOff) != square(stand));

  // c1 at Ariadne's stand, c2 beside the draugr; c1's load grants it the
  // temple, c2's gets nothing
  DoConnect(p, 0);
  p.CreateActor(kC1, stand, 0, kTemple);
  p.SetUserActor(0, kC1);
  auto& c1 = p.worldState.GetFormAt<MpActor>(kC1);
  c1.ForceSubscriptionsUpdate();
  DoConnect(p, 1);
  p.CreateActor(kC2, seat + NiPoint3{ 80.f, 0.f, 0.f }, 0, kTemple);
  p.SetUserActor(1, kC2);
  auto& c2 = p.worldState.GetFormAt<MpActor>(kC2);
  c2.ForceSubscriptionsUpdate();
  CellLoaded(p, 0, kTemple);
  CellLoaded(p, 1, kTemple);
  REQUIRE(p.GetCellHost(kTemple).player == kC1);
  REQUIRE(p.worldState.HostOf(draugr) == &c1);
  REQUIRE(draugr.GetListeners().count(&c2) == 1);

  // the draugr's CreateActors to `user` since the last clear, and its
  // DestroyActors
  const auto creates = [&](Networking::UserId user) {
    std::vector<CreateActorMessage> out;
    for (const auto& m : SentTo<CreateActorMessage>(p, user)) {
      if (m.refrId == kDraugr) {
        out.push_back(m);
      }
    }
    return out;
  };
  const auto destroys = [&](Networking::UserId user) {
    size_t n = 0;
    for (const auto& m : SentTo<DestroyActorMessage>(p, user)) {
      if (m.idx == draugr.GetIdx()) {
        ++n;
      }
    }
    return n;
  };

  SECTION("the first hall: the host's roster takes the draugr two squares "
          "off, and the host's steps across a square line keep it")
  {
    // the grant's word went to the actors c1's game hears (FlipHostedFlags),
    // never the draugr: c1's game holds no form for it
    REQUIRE(draugr.GetListeners().count(&c1) == 0);
    const std::set<MpObjectReference*> c2Was = c2.GetListeners();
    const auto roster = [&] {
      DoMessage(p, 0,
                nlohmann::json{ { "t", MsgType::ActorSightings },
                                { "sightings",
                                  nlohmann::json::array({ nlohmann::json{
                                    { "refrId", kDraugr },
                                    { "worldOrCell", kTemple },
                                    { "pos", { seat.x, seat.y, seat.z } },
                                    { "rot", { 0.f, 0.f, 0.f } } } }) } });
    };

    // the roster names it where the record stands: c1 is subscribed and its
    // game gets the CreateActor with the word false; the record stays where
    // it stood and c2 hears nothing new
    p.Messages().clear();
    roster();
    REQUIRE(draugr.GetListeners().count(&c1) == 1);
    const auto toHost = creates(0);
    REQUIRE(toHost.size() == 1);
    REQUIRE(toHost[0].props.isHostedByOther == false);
    REQUIRE((draugr.GetPos() - seat).Length() < 1.f);
    REQUIRE(creates(1).empty());
    REQUIRE(c2.GetListeners() == c2Was);

    // a late roster that still names it: nothing more
    p.Messages().clear();
    roster();
    REQUIRE(creates(0).empty());

    // c1 steps south across the y line and back, the draugr two squares off
    // at each stand: c1's game keeps it, with no DestroyActor and no second
    // CreateActor, and c2's listeners are as the grid had them
    p.Messages().clear();
    c1.SetPos(south);
    REQUIRE(draugr.GetListeners().count(&c1) == 1);
    c1.SetPos(stand);
    REQUIRE(draugr.GetListeners().count(&c1) == 1);
    REQUIRE(destroys(0) == 0);
    REQUIRE(creates(0).empty());
    REQUIRE(c2.GetListeners() == c2Was);
  }

  SECTION("the second hall: a host one square off steps across a square "
          "line to two squares off and back, and keeps the draugr its engine "
          "runs")
  {
    // c1 one square from the draugr: the grid subscribes it, the word false
    p.Messages().clear();
    c1.SetPos(oneOff);
    REQUIRE(draugr.GetListeners().count(&c1) == 1);
    const auto toHost = creates(0);
    REQUIRE(toHost.size() == 1);
    REQUIRE(toHost[0].props.isHostedByOther == false);
    // across the x line, two squares off, and back: kept, with no
    // DestroyActor (each demote of the hall's flicker on the host followed
    // one) and no second CreateActor (each promote followed one)
    p.Messages().clear();
    c1.SetPos(stand);
    REQUIRE(draugr.GetListeners().count(&c1) == 1);
    REQUIRE(destroys(0) == 0);
    c1.SetPos(oneOff);
    REQUIRE(draugr.GetListeners().count(&c1) == 1);
    REQUIRE(creates(0).empty());
    // the other player keeps the draugr by the grid, as before
    REQUIRE(draugr.GetListeners().count(&c2) == 1);
    REQUIRE(destroys(1) == 0);
  }

  // the review of tracker #162's first build: the guest's pass kept the
  // host (D6's insert, HostOf) and the host's pass, asking HostedBy, dropped
  // the guest at its next step, a DestroyActor each way, while the host's
  // engine ran the draugr fighting beside the guest
  SECTION("a guest beyond the host's grid: its own step subscribes the "
          "host, and the host's steps across a square line keep it")
  {
    // the guest's step, square (2, -1) to (3, -1), the draugr still in its
    // 3x3 and the host three squares off
    const NiPoint3 guestStep{ 12400.f, seat.y, seat.z };
    REQUIRE(square(guestStep) == std::pair<int, int>{ 3, -1 });
    REQUIRE(apart(guestStep, stand));
    REQUIRE(apart(guestStep, south));
    REQUIRE(p.worldState.HostOf(c2) == &c1);
    REQUIRE(c2.GetListeners().count(&c1) == 0);
    c2.SetPos(guestStep);
    REQUIRE(c2.GetListeners().count(&c1) == 1);
    REQUIRE(c1.GetListeners().count(&c2) == 1);
    // the DestroyActors for `actor` that went to `user` since the last clear
    const auto destroyed = [&](Networking::UserId user, MpActor& actor) {
      size_t n = 0;
      for (const auto& m : SentTo<DestroyActorMessage>(p, user)) {
        if (m.idx == actor.GetIdx()) {
          ++n;
        }
      }
      return n;
    };
    p.Messages().clear();
    c1.SetPos(south);
    REQUIRE(c2.GetListeners().count(&c1) == 1);
    REQUIRE(c1.GetListeners().count(&c2) == 1);
    c1.SetPos(stand);
    REQUIRE(c2.GetListeners().count(&c1) == 1);
    REQUIRE(c1.GetListeners().count(&c2) == 1);
    REQUIRE(destroyed(0, c2) == 0);
    REQUIRE(destroyed(1, c1) == 0);
  }

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

// thuum docs/verbs/npc-handoff.md (d), ADR-047 D9 (playtest 34, 2026-10-25:
// a restart's cold start gave Sigrid's two cells to different players and
// her shuttle between them was refused at every crossing, seven a second,
// one engine walking her live where the other held the record): a hosted
// actor its host walks into an adjacent cell another live host holds passes
// to that host at once; the record moves, the new host's game hears "not
// hosted by other" and the old host's "hosted by other", the old host's
// further reports are dropped and the new host's pass; a crossing back
// passes it the same way.
TEST_CASE("A late joiner's CreateActor carries the hosted actor's last "
          "action, a player's never",
          "[CellHost]")
{
  // thuum docs/verbs/npc-stream.md (playtest 35, 2026-10-27): Hilde's tanning
  // began two minutes before the second seat reached Riverwood, and its
  // CreateActor carried the last animation event, not the action, so the
  // puppet stood idle until the host's engine restarted the chore. The
  // host's action on an NPC it hosts is kept on the record and rides the
  // CreateActor a later arrival gets; a player's own actions never do
  auto& p = GetPartOne();
  Player(p, 0, kC1, kHere);
  const auto here = p.worldState.CellOf(kTamriel, kHere);
  REQUIRE(here.has_value());
  CellLoaded(p, 0, *here);
  REQUIRE(p.GetCellHost(*here).player == kC1);
  p.CreateActor(kWolf, kHere, 0, kTamriel);
  auto& wolf = p.worldState.GetFormAt<MpActor>(kWolf);
  wolf.SetPos(kHere);
  REQUIRE(p.SimulatorOf(wolf) == kC1);
  REQUIRE_FALSE(wolf.GetLastAction().has_value());

  // the host performs an action on its wolf: kept
  Act(p, 0, kWolf);
  REQUIRE(wolf.GetLastAction().has_value());
  REQUIRE(wolf.GetLastAction()->eventName == "attackStart");

  // the second player arrives: its CreateActor for the wolf carries it
  p.Messages().clear();
  Player(p, 1, kC2, kHere);
  // the custom property "lastAction" (the record's JSON-valued property
  // channel; no wire row) on the wolf's CreateActor, on no player's
  const auto lastActionOf =
    [](const CreateActorMessage& m) -> std::optional<nlohmann::json> {
    for (const auto& entry : m.customPropsJsonDumps) {
      if (entry.propName == "lastAction") {
        return nlohmann::json::parse(entry.propValueJsonDump);
      }
    }
    return std::nullopt;
  };
  bool carried = false;
  for (const auto& m : SentTo<CreateActorMessage>(p, 1)) {
    if (m.refrId == kWolf) {
      const auto last = lastActionOf(m);
      REQUIRE(last.has_value());
      REQUIRE(last->at("eventName").get<std::string>() == "attackStart");
      REQUIRE(last->at("action").get<uint32_t>() == 1);
      REQUIRE(last->at("type").get<uint32_t>() == 2);
      REQUIRE(last->at("actorAnimationVariables").is_object());
      carried = true;
    }
    if (m.refrId == kC1) {
      // a player's actor: its actions are its own game's, never carried
      REQUIRE_FALSE(lastActionOf(m).has_value());
    }
  }
  REQUIRE(carried);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kWolf);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

TEST_CASE("A new listener's CreateActor carries the recorded drawn weapon of "
          "a player and a hosted actor, a sheathed one's never",
          "[CellHost]")
{
  // thuum tracker #172 (playtest 55): rotfern, her bow out, and the draugr
  // she fought came back into Ariadne's view sheathed on Ariadne's screen.
  // The record keeps the drawn flag from every movement sample; the create
  // a new listener gets carries it as the custom property "isWeapDrawn"
  auto& p = GetPartOne();
  Player(p, 0, kC1, kHere);
  const auto here = p.worldState.CellOf(kTamriel, kHere);
  REQUIRE(here.has_value());
  CellLoaded(p, 0, *here);
  REQUIRE(p.GetCellHost(*here).player == kC1);
  p.CreateActor(kWolf, kHere, 0, kTamriel);
  auto& wolf = p.worldState.GetFormAt<MpActor>(kWolf);
  wolf.SetPos(kHere);
  REQUIRE(p.SimulatorOf(wolf) == kC1);

  // the player and the wolf its game hosts both draw: the record takes it
  Move(p, 0, kC1, kHere, kTamriel, true);
  Move(p, 0, kWolf, kHere, kTamriel, true);
  REQUIRE(p.worldState.GetFormAt<MpActor>(kC1).IsWeaponDrawn());
  REQUIRE(wolf.IsWeaponDrawn());

  const auto drawnOf =
    [](const CreateActorMessage& m) -> std::optional<nlohmann::json> {
    for (const auto& entry : m.customPropsJsonDumps) {
      if (entry.propName == "isWeapDrawn") {
        return nlohmann::json::parse(entry.propValueJsonDump);
      }
    }
    return std::nullopt;
  };

  // the second player arrives: its creates of both say drawn
  p.Messages().clear();
  Player(p, 1, kC2, kHere);
  bool player = false, hosted = false;
  for (const auto& m : SentTo<CreateActorMessage>(p, 1)) {
    if (m.refrId == kC1 || m.refrId == kWolf) {
      const auto drawn = drawnOf(m);
      REQUIRE(drawn.has_value());
      REQUIRE(*drawn == true);
      if (m.refrId == kC1) {
        player = true;
      } else {
        hosted = true;
      }
    }
    if (m.refrId == kC2) {
      // its own create: its own game's word, never carried
      REQUIRE_FALSE(drawnOf(m).has_value());
    }
  }
  REQUIRE(player);
  REQUIRE(hosted);
  // the first player's create of the newcomer, sheathed: none
  bool newcomer = false;
  for (const auto& m : SentTo<CreateActorMessage>(p, 0)) {
    if (m.refrId == kC2) {
      REQUIRE_FALSE(drawnOf(m).has_value());
      newcomer = true;
    }
  }
  REQUIRE(newcomer);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kWolf);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

TEST_CASE("A hosted actor crossing into another live host's cell passes to "
          "that host, whose word flips both games",
          "[CellHost]")
{
  auto& p = GetPartOne();
  Player(p, 0, kC1, kHere);
  Player(p, 1, kC2, kEast);
  const auto here = p.worldState.CellOf(kTamriel, kHere);
  const auto east = p.worldState.CellOf(kTamriel, kEast);
  REQUIRE(here.has_value());
  REQUIRE(east.has_value());
  CellLoaded(p, 0, *here);
  CellLoaded(p, 1, *east);
  REQUIRE(p.GetCellHost(*here).player == kC1);
  REQUIRE(p.GetCellHost(*east).player == kC2);
  p.CreateActor(kWolf, kHere, 0, kTamriel);
  auto& wolf = p.worldState.GetFormAt<MpActor>(kWolf);
  wolf.SetPos(kHere);
  REQUIRE(p.SimulatorOf(wolf) == kC1);
  p.Messages().clear();

  // c1 walks it across the border (the next square begins at x 12288; a
  // report may move an actor 4096 at most): within the pass margin (D22's
  // brake, 256 units) the record moves and c1 keeps the actor as a follow
  // into c2's cell; a step the margin deep passes it, no follow left behind
  const NiPoint3 kAcross{ 12400.f, 500.f, 0.f };
  Move(p, 0, kWolf, kAcross);
  REQUIRE(p.worldState.CellOf(wolf) == east);
  REQUIRE(p.SimulatorOf(wolf) == kC1);
  REQUIRE_FALSE(p.GetFollows().empty());
  const NiPoint3 kDeep{ 12600.f, 500.f, 0.f };
  Move(p, 0, kWolf, kDeep);
  REQUIRE(p.worldState.CellOf(wolf) == east);
  REQUIRE(p.SimulatorOf(wolf) == kC2);
  REQUIRE(p.GetFollows().empty());
  bool newHostLive = false;
  for (const auto& m : SentTo<UpdatePropertyMessage>(p, 1)) {
    if (m.idx == wolf.GetIdx() && m.propName == "isHostedByOther" &&
        m.dataDump == "false") {
      newHostLive = true;
    }
  }
  bool oldHostDemoted = false;
  for (const auto& m : SentTo<UpdatePropertyMessage>(p, 0)) {
    if (m.idx == wolf.GetIdx() && m.propName == "isHostedByOther" &&
        m.dataDump == "true") {
      oldHostDemoted = true;
    }
  }
  REQUIRE(newHostLive);
  REQUIRE(oldHostDemoted);

  // the old host's next report is dropped, the new host's passes
  Move(p, 0, kWolf, kDeep + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(std::abs(wolf.GetPos().x - kDeep.x) < 1.f);
  Move(p, 1, kWolf, kDeep + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(std::abs(wolf.GetPos().x - (kDeep.x + 100.f)) < 1.f);

  // and back across, the margin deep on the other side: passed to c1 the
  // same way
  const NiPoint3 kBack{ 11900.f, 500.f, 0.f };
  Move(p, 1, kWolf, kBack);
  REQUIRE(p.worldState.CellOf(wolf) == here);
  REQUIRE(p.SimulatorOf(wolf) == kC1);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kWolf);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

// A hold in another host's cell (D22's margin) that ends with its holder's
// unload of the cell, or its departure, names the cell's host again, and the
// word reaches it: the host's game runs the actor instead of keeping a puppet
// nobody simulates, which a dialogue's claim could never reach (the claim
// moves no simulator, so no word answers it)
TEST_CASE("A hold in another host's cell ended by its holder's unload or "
          "departure hands the actor to that host, whose game is told",
          "[CellHost]")
{
  auto& p = GetPartOne();
  Player(p, 0, kC1, kHere);
  Player(p, 1, kC2, kEast);
  const auto here = p.worldState.CellOf(kTamriel, kHere);
  const auto east = p.worldState.CellOf(kTamriel, kEast);
  REQUIRE(here.has_value());
  REQUIRE(east.has_value());
  CellLoaded(p, 0, *here);
  CellLoaded(p, 1, *east);
  REQUIRE(p.GetCellHost(*east).player == kC2);
  p.CreateActor(kWolf, kHere, 0, kTamriel);
  auto& wolf = p.worldState.GetFormAt<MpActor>(kWolf);
  wolf.SetPos(kHere);
  REQUIRE(p.SimulatorOf(wolf) == kC1);

  // c1 walks it over the border within the pass margin: c1 holds it in c2's
  // cell
  const NiPoint3 kAcross{ 12400.f, 500.f, 0.f };
  Move(p, 0, kWolf, kAcross);
  REQUIRE(p.worldState.CellOf(wolf) == east);
  REQUIRE(p.SimulatorOf(wolf) == kC1);
  REQUIRE_FALSE(p.GetFollows().empty());

  bool c1Left = false;
  SECTION("its holder unloads the cell")
  {
    p.Messages().clear();
    CellLoaded(p, 0, *east, false);
  }
  SECTION("its holder leaves the server")
  {
    p.Messages().clear();
    DoDisconnect(p, 0);
    c1Left = true;
  }
  REQUIRE(p.GetFollows().empty());
  REQUIRE(p.SimulatorOf(wolf) == kC2);
  INFO(MessagesSentTo(p, 1));
  REQUIRE(LastHostedWord(p, 1, wolf) == "false");

  if (!c1Left) {
    DoDisconnect(p, 0);
  }
  DoDisconnect(p, 1);
  p.DestroyActor(kWolf);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

// thuum docs/verbs/npc-handoff.md (d) through a door, ADR-047's note of
// 2026-10-28 (playtest 37: Embry followed his package to the Sleeping
// Giant's door while Prisoner hosted the inn from inside; the server refused
// the crossing, "may not cross into 133c6", and his host's engine walked him
// back to the door every few seconds; Eli: "let embry cross and hand him to
// the inn's host. that's a good rule to have"): a persistent actor's door
// into a cell another live host holds is admitted at the door's pre-check
// and the host's report from indoors passes the actor to that host, whose
// word flips both games; the exit by schedule passes it back the same way.
TEST_CASE("A hosted actor's door into a cell another live host holds passes "
          "it to that host, in and back out",
          "[CellHost][espm]")
{
  // Hod 0x1348a, Riverwood's persistent mill worker (the door fact the
  // follow case reads), stood by the server in c1's square; the Sleeping
  // Giant 0x133c6 is the interior c2 stands in and so hosts
  constexpr uint32_t kHod = 0x1348a;
  constexpr uint32_t kInn = 0x133c6;
  const NiPoint3 kInside{ -250.f, -360.f, 0.f };
  const NiPoint3 kOutside = kHere + NiPoint3{ 300.f, 0.f, 0.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.IsInteriorCell(kInn));
  REQUIRE(p.worldState.IsPersistentReference(kHod));

  Player(p, 0, kC1, kHere);
  DoConnect(p, 1);
  p.CreateActor(kC2, kInside, 0, kInn);
  p.SetUserActor(1, kC2);
  const auto here = p.worldState.CellOf(kTamriel, kHere);
  REQUIRE(here.has_value());
  CellLoaded(p, 0, *here);
  CellLoaded(p, 1, kInn);
  REQUIRE(p.GetCellHost(*here).player == kC1);
  REQUIRE(p.GetCellHost(kInn).player == kC2);

  auto& form = p.worldState.LookupFormById(kHod);
  REQUIRE(form);
  MpActor* hod = form->AsActor();
  REQUIRE(hod);
  hod->SetCellOrWorldObsolete(FormDesc::Tamriel());
  hod->SetPos(kOutside);
  REQUIRE(p.worldState.CellOf(*hod) == here);
  REQUIRE(p.SimulatorOf(*hod) == kC1);
  p.Messages().clear();

  // the door's pre-check admits the crossing (the refusal of playtest 37)
  REQUIRE(p.worldState.npcDoorVerdict);
  REQUIRE(p.worldState.npcDoorVerdict(p.worldState.GetFormAt<MpActor>(kC1),
                                      *hod, kInn, kInside));

  // c1's engine walked him through; its report from indoors passes him
  Move(p, 0, kHod, kInside, kInn);
  REQUIRE(p.worldState.CellOf(*hod) == kInn);
  REQUIRE(p.SimulatorOf(*hod) == kC2);
  REQUIRE(p.GetFollows().empty());
  bool newHostLive = false;
  for (const auto& m : SentTo<UpdatePropertyMessage>(p, 1)) {
    if (m.idx == hod->GetIdx() && m.propName == "isHostedByOther" &&
        m.dataDump == "false") {
      newHostLive = true;
    }
  }
  bool oldHostDemoted = false;
  for (const auto& m : SentTo<UpdatePropertyMessage>(p, 0)) {
    if (m.idx == hod->GetIdx() && m.propName == "isHostedByOther" &&
        m.dataDump == "true") {
      oldHostDemoted = true;
    }
  }
  REQUIRE(newHostLive);
  REQUIRE(oldHostDemoted);
  // the passing report reached the new host's game reliably, ahead of the
  // word (the settle's half (a), 019921fb, on m3-system1 without the
  // window: its promote places the copy at the door's far side, not where
  // the copy stood); the test's raw report is the empty object the harness
  // parses it to
  {
    const auto& sent = p.Messages();
    size_t reportAt = sent.size();
    size_t wordAt = sent.size();
    for (size_t i = 0; i < sent.size(); ++i) {
      const auto& m = sent[i];
      if (m.userId != 1 || !m.j.is_object()) {
        continue;
      }
      if (reportAt == sent.size() && m.reliable && m.j.empty()) {
        reportAt = i;
      }
      if (wordAt == sent.size() &&
          m.j.value("propName", std::string()) == "isHostedByOther" &&
          m.j.value("dataDump", std::string()) == "false" &&
          m.j.value("refrId", 0u) == kHod) {
        wordAt = i;
      }
    }
    REQUIRE(reportAt < sent.size());
    REQUIRE(wordAt < sent.size());
    REQUIRE(reportAt < wordAt);
  }

  // the old host's next report from indoors is a stranger's, the inn
  // host's passes
  Move(p, 0, kHod, kInside + NiPoint3{ 100.f, 0.f, 0.f }, kInn);
  REQUIRE(std::abs(hod->GetPos().x - kInside.x) < 1.f);
  Move(p, 1, kHod, kInside + NiPoint3{ 100.f, 0.f, 0.f }, kInn);
  REQUIRE(std::abs(hod->GetPos().x - (kInside.x + 100.f)) < 1.f);

  // the exit by schedule: the inn's host walks him out into c1's square,
  // deep inside it, and c1 takes him back
  REQUIRE(p.worldState.npcDoorVerdict(p.worldState.GetFormAt<MpActor>(kC2),
                                      *hod, kTamriel, kOutside));
  Move(p, 1, kHod, kOutside);
  REQUIRE(p.worldState.CellOf(*hod) == here);
  REQUIRE(p.SimulatorOf(*hod) == kC1);
  REQUIRE(p.GetFollows().empty());

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kHod);
}

// A load door between two exterior worldspaces is a door too: Whiterun's
// main gate (WRDoorMainGate01, 0x1b1f1 in Tamriel 0x3c and 0x1b1f3 in
// Whiterun 0x1a26f, Skyrim.esm by lab/esm.py) has an exterior at both ends,
// and a crossing through it is neither adjacent (two worldspaces) nor an
// interior's door, so the judge refused every report of it unless the
// reporting game hosted the far side
TEST_CASE("A hosted actor through a load door between two worldspaces passes "
          "it to the far side's host",
          "[CellHost][espm]")
{
  constexpr uint32_t kHod = 0x1348a;
  constexpr uint32_t kWhiterunWorld = 0x1a26f;
  const NiPoint3 kOutsideGate{ 19312.f, -7700.f, -3520.f };
  const NiPoint3 kInsideGate{ 19367.f, -7300.f, -3547.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(!p.worldState.IsInteriorCell(kTamriel));
  REQUIRE(!p.worldState.IsInteriorCell(kWhiterunWorld));

  Player(p, 0, kC1, kOutsideGate + NiPoint3{ 0.f, -200.f, 0.f });
  DoConnect(p, 1);
  p.CreateActor(kC2, kInsideGate + NiPoint3{ 0.f, 200.f, 0.f }, 0,
                kWhiterunWorld);
  p.SetUserActor(1, kC2);
  const auto outside = p.worldState.CellOf(kTamriel, kOutsideGate);
  const auto inside = p.worldState.CellOf(kWhiterunWorld, kInsideGate);
  REQUIRE(outside.has_value());
  REQUIRE(inside.has_value());
  CellLoaded(p, 0, *outside);
  CellLoaded(p, 1, *inside);
  REQUIRE(p.GetCellHost(*outside).player == kC1);
  REQUIRE(p.GetCellHost(*inside).player == kC2);

  auto& form = p.worldState.LookupFormById(kHod);
  REQUIRE(form);
  MpActor* hod = form->AsActor();
  REQUIRE(hod);
  hod->SetCellOrWorldObsolete(FormDesc::Tamriel());
  hod->SetPos(kOutsideGate);
  REQUIRE(p.worldState.CellOf(*hod) == outside);
  REQUIRE(p.SimulatorOf(*hod) == kC1);

  // his simulator's report of the gate's crossing passes
  REQUIRE(p.worldState.npcDoorVerdict);
  REQUIRE(p.worldState.npcDoorVerdict(p.worldState.GetFormAt<MpActor>(kC1),
                                      *hod, kWhiterunWorld, kInsideGate));

  // and a refusal says who reported it and who simulates the walker: the
  // far side's host reporting a walker c1 simulates
  {
    LogCapture log;
    REQUIRE_FALSE(
      p.worldState.npcDoorVerdict(p.worldState.GetFormAt<MpActor>(kC2), *hod,
                                  kWhiterunWorld, kInsideGate));
    REQUIRE(log.Count(fmt::format(
              "door verdict for {:x} reported by {:x}: from cell {:x} (its "
              "simulator {:x}) to cell {:x} (its host {:x}); refused",
              kHod, kC2, *outside, kC1, *inside, kC2)) == 1);
  }

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kHod);
}

// thuum memo D54 (docs/research/m3/decisions-after-npc-stream.md; Eli's B,
// 2026-10-28): the door fact asked the actor's persistence, so Hilde
// 0x35534, Riverwood's one resident placed without the flag (her ACHR's
// header flags are 0 where Hod's and Embry's carry 0x400; Skyrim.esm by
// lab/esm.py; base NPC_ 0x35533 Hilde), was refused at her own door 0x1cbb0
// into RiverwoodSvensHouse 0x1cb84 and her host's engine walked her back to
// it for minutes. Every actor's door is judged alike now: into an interior
// nobody hosts the record follows the host's report in and stands there,
// frozen for every other game (a stranger's report dropped, the viewer
// outside unsubscribed), her host's engine holding her unloaded where the
// door put her, until a player loads the house and takes her with the
// grant.
TEST_CASE("A hosted non-persistent resident's door into an unhosted interior "
          "moves the record there and leaves it frozen",
          "[CellHost][espm]")
{
  constexpr uint32_t kHilde = 0x35534;
  constexpr uint32_t kHouse = 0x1cb84;
  // her placement's point in the house (lab/esm.py ref 0x35534)
  const NiPoint3 kInside{ -482.f, 31.f, -15.f };
  const NiPoint3 kOutside = kHere + NiPoint3{ 300.f, 0.f, 0.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.IsInteriorCell(kHouse));
  REQUIRE_FALSE(p.worldState.IsPersistentReference(kHilde));

  auto& c1 = Player(p, 0, kC1, kHere);
  auto& c2 = Player(p, 1, kC2, kHere + NiPoint3{ 600.f, 0.f, 0.f });
  const auto here = p.worldState.CellOf(kTamriel, kHere);
  REQUIRE(here.has_value());
  CellLoaded(p, 0, *here);
  REQUIRE(p.GetCellHost(*here).player == kC1);
  REQUIRE_FALSE(p.GetCellHost(kHouse).hosted);

  auto& form = p.worldState.LookupFormById(kHilde);
  REQUIRE(form);
  MpActor* hilde = form->AsActor();
  REQUIRE(hilde);
  hilde->SetCellOrWorldObsolete(FormDesc::Tamriel());
  hilde->SetPos(kOutside);
  REQUIRE(p.worldState.CellOf(*hilde) == here);
  REQUIRE(p.SimulatorOf(*hilde) == kC1);
  REQUIRE(hilde->GetListeners().count(&c1) == 1);
  REQUIRE(hilde->GetListeners().count(&c2) == 1);
  p.Messages().clear();

  // the door's pre-check admits her crossing into a house nobody hosts (the
  // refusal of D54), and records nothing
  REQUIRE(p.worldState.npcDoorVerdict);
  REQUIRE(p.worldState.npcDoorVerdict(c1, *hilde, kHouse, kInside));
  REQUIRE(p.GetFollows().empty());

  // c1's engine walked her through; its report from indoors moves the
  // record into the house as c1's follow, the house still nobody's
  Move(p, 0, kHilde, kInside, kHouse);
  REQUIRE(p.worldState.CellOf(*hilde) == kHouse);
  REQUIRE(p.SimulatorOf(*hilde) == kC1);
  const auto follows = p.GetFollows();
  REQUIRE(follows.size() == 1);
  REQUIRE(follows[0].actor == kHilde);
  REQUIRE(follows[0].player == kC1);
  REQUIRE(follows[0].cell == kHouse);
  REQUIRE_FALSE(p.GetCellHost(kHouse).hosted);

  // the viewer outside was told the move as a Teleport ahead of its
  // unsubscribe and is dropped by the house's grid; her host stays her
  // listener (the follow's pin)
  bool viewerTold = false;
  for (const auto& m : SentTo<TeleportMessage>(p, 1)) {
    if (m.idx == hilde->GetIdx() && m.worldOrCell == kHouse) {
      viewerTold = true;
    }
  }
  REQUIRE(viewerTold);
  REQUIRE(hilde->GetListeners().count(&c1) == 1);
  REQUIRE(hilde->GetListeners().count(&c2) == 0);

  // frozen for every other game: a stranger's report from the house is
  // dropped; her host's low-process sample passes
  Move(p, 1, kHilde, kInside + NiPoint3{ 100.f, 0.f, 0.f }, kHouse);
  REQUIRE(std::abs(hilde->GetPos().x - kInside.x) < 1.f);
  Move(p, 0, kHilde, kInside + NiPoint3{ 20.f, 0.f, 0.f }, kHouse);
  REQUIRE(std::abs(hilde->GetPos().x - (kInside.x + 20.f)) < 1.f);

  // a player loads the house: granted, the follow ends, the new host hears
  // "not hosted by other" for her and the old follower's report is dropped
  DoConnect(p, 2);
  p.CreateActor(kC3, kInside, 0, kHouse);
  p.SetUserActor(2, kC3);
  CellLoaded(p, 2, kHouse);
  REQUIRE(p.GetCellHost(kHouse).player == kC3);
  REQUIRE(p.SimulatorOf(*hilde) == kC3);
  REQUIRE(p.GetFollows().empty());
  bool newHostLive = false;
  for (const auto& m : SentTo<UpdatePropertyMessage>(p, 2)) {
    if (m.idx == hilde->GetIdx() && m.propName == "isHostedByOther" &&
        m.dataDump == "false") {
      newHostLive = true;
    }
  }
  REQUIRE(newHostLive);
  Move(p, 0, kHilde, kInside, kHouse);
  REQUIRE(std::abs(hilde->GetPos().x - (kInside.x + 20.f)) < 1.f);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  DoDisconnect(p, 2);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kC3);
  p.DestroyActor(kHilde);
}

// the same door while another seat hosts the house from inside (the Hod
// case's shape, ADR-047's door pass of 2026-10-28): the pass asks no
// persistence either
TEST_CASE("A hosted non-persistent resident's door into a cell another live "
          "host holds passes it to that host",
          "[CellHost][espm]")
{
  constexpr uint32_t kHilde = 0x35534;
  constexpr uint32_t kHouse = 0x1cb84;
  const NiPoint3 kInside{ -482.f, 31.f, -15.f };
  const NiPoint3 kOutside = kHere + NiPoint3{ 300.f, 0.f, 0.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE_FALSE(p.worldState.IsPersistentReference(kHilde));

  Player(p, 0, kC1, kHere);
  DoConnect(p, 1);
  p.CreateActor(kC2, kInside, 0, kHouse);
  p.SetUserActor(1, kC2);
  const auto here = p.worldState.CellOf(kTamriel, kHere);
  REQUIRE(here.has_value());
  CellLoaded(p, 0, *here);
  CellLoaded(p, 1, kHouse);
  REQUIRE(p.GetCellHost(*here).player == kC1);
  REQUIRE(p.GetCellHost(kHouse).player == kC2);

  auto& form = p.worldState.LookupFormById(kHilde);
  REQUIRE(form);
  MpActor* hilde = form->AsActor();
  REQUIRE(hilde);
  hilde->SetCellOrWorldObsolete(FormDesc::Tamriel());
  hilde->SetPos(kOutside);
  REQUIRE(p.worldState.CellOf(*hilde) == here);
  REQUIRE(p.SimulatorOf(*hilde) == kC1);
  p.Messages().clear();

  // the door's pre-check admits her crossing into the hosted house
  REQUIRE(p.worldState.npcDoorVerdict(p.worldState.GetFormAt<MpActor>(kC1),
                                      *hilde, kHouse, kInside));

  // c1's engine walked her through; its report from indoors passes her to
  // the house's host: the record moves, no follow, the word both ways
  Move(p, 0, kHilde, kInside, kHouse);
  REQUIRE(p.worldState.CellOf(*hilde) == kHouse);
  REQUIRE(p.SimulatorOf(*hilde) == kC2);
  REQUIRE(p.GetFollows().empty());
  bool newHostLive = false;
  for (const auto& m : SentTo<UpdatePropertyMessage>(p, 1)) {
    if (m.idx == hilde->GetIdx() && m.propName == "isHostedByOther" &&
        m.dataDump == "false") {
      newHostLive = true;
    }
  }
  bool oldHostDemoted = false;
  for (const auto& m : SentTo<UpdatePropertyMessage>(p, 0)) {
    if (m.idx == hilde->GetIdx() && m.propName == "isHostedByOther" &&
        m.dataDump == "true") {
      oldHostDemoted = true;
    }
  }
  REQUIRE(newHostLive);
  REQUIRE(oldHostDemoted);

  // the old host's next report from indoors is a stranger's, the house
  // host's passes
  Move(p, 0, kHilde, kInside + NiPoint3{ 100.f, 0.f, 0.f }, kHouse);
  REQUIRE(std::abs(hilde->GetPos().x - kInside.x) < 1.f);
  Move(p, 1, kHilde, kInside + NiPoint3{ 100.f, 0.f, 0.f }, kHouse);
  REQUIRE(std::abs(hilde->GetPos().x - (kInside.x + 100.f)) < 1.f);

  // her morning: the house's host walks her out into c1's square, deep
  // inside it, and c1 takes her back
  REQUIRE(p.worldState.npcDoorVerdict(p.worldState.GetFormAt<MpActor>(kC2),
                                      *hilde, kTamriel, kOutside));
  Move(p, 1, kHilde, kOutside);
  REQUIRE(p.worldState.CellOf(*hilde) == here);
  REQUIRE(p.SimulatorOf(*hilde) == kC1);
  REQUIRE(p.GetFollows().empty());

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kHilde);
}

// thuum docs/verbs/npc-puppet.md (ActorSightings, TES3MP's actor list): a
// named resident is placed in her house and walked outside by her schedule;
// the server loads actors by placement, so her host's engine has her where
// the server has nothing. The host's roster names her where it has her, the
// server loads the record on demand and places it there, and from then on
// the normal path runs: CreateActor with the word, the host's reports.
TEST_CASE("A host's sighting places an interior-placed resident where its "
          "engine has her; a non-host's and an unhosted cell's are dropped",
          "[CellHost][espm]")
{
  constexpr uint32_t kSigrid = 0x13483;
  constexpr uint32_t kDorthe = 0x13484;
  const NiPoint3 sigridPos{ 24063.7f, -45204.2f, -40.6f };
  const NiPoint3 landing{ 17225.f, -47204.f, -128.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSigrid) == nullptr);

  // c1 lands at Riverwood and loads Sigrid's square; c2 stands far away
  Player(p, 0, kC1, landing);
  Player(p, 1, kC2, landing + NiPoint3{ 0.f, -40000.f, 0.f });
  const auto cell = p.worldState.CellOf(kTamriel, sigridPos);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  p.Messages().clear();

  const auto sighting = [&](Networking::UserId user, uint32_t refrId,
                            const NiPoint3& pos) {
    DoMessage(p, user,
              nlohmann::json{ { "t", MsgType::ActorSightings },
                              { "sightings",
                                nlohmann::json::array({ nlohmann::json{
                                  { "refrId", refrId },
                                  { "worldOrCell", kTamriel },
                                  { "pos", { pos.x, pos.y, pos.z } },
                                  { "rot", { 0.f, 0.f, 165.3f } } } }) } });
  };

  // a non-host's sighting (c2 hosts nothing): dropped, no record
  sighting(1, kSigrid, sigridPos);
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSigrid) == nullptr);

  // the host's sighting of a position in a cell it does not host: dropped
  sighting(0, kDorthe, landing + NiPoint3{ 0.f, -40000.f, 0.f });
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kDorthe) == nullptr);

  // the host's sighting: the record loaded and placed where the engine has
  // her, in Tamriel, and told to c1 as its own
  sighting(0, kSigrid, sigridPos);
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSigrid) != nullptr);
  auto& sigrid = p.worldState.GetFormAt<MpActor>(kSigrid);
  REQUIRE(sigrid.GetCellOrWorld().ToFormId(p.worldState.espmFiles) ==
          kTamriel);
  REQUIRE(sigrid.GetPos().x == Catch::Approx(sigridPos.x));
  REQUIRE(sigrid.GetPos().y == Catch::Approx(sigridPos.y));
  REQUIRE(p.worldState.CellOf(sigrid) == cell);
  REQUIRE(p.worldState.HostOf(sigrid) ==
          &p.worldState.GetFormAt<MpActor>(kC1));
  {
    bool created = false;
    for (auto& m : SentTo<CreateActorMessage>(p, 0)) {
      if (m.refrId == kSigrid) {
        created = true;
        REQUIRE(m.props.isHostedByOther == false);
        REQUIRE(m.baseRecordType == "NPC_");
      }
    }
    REQUIRE(created);
  }

  // the same sighting again: nothing moves, nothing new is sent
  p.Messages().clear();
  sighting(0, kSigrid, sigridPos);
  REQUIRE(SentTo<CreateActorMessage>(p, 0).empty());

  // playtest 37 (finding 38, "we have 2 embrys"): a game whose engine has
  // the resident in a cell it hosts, while her record stands in another
  // game's cell, is told to MOVE its copy to the record and the record
  // stays (ADR-067 decision 5, playtest 42's F1: the Disable snippet this
  // block once sent never reached the copy, its long selfId needing a view
  // the sighter no longer had; the move travels as a static call with the
  // copy as an argument, the record's cell-or-world typed as both the Cell
  // and the WorldSpace argument)
  const NiPoint3 far = landing + NiPoint3{ 0.f, -40000.f, 0.f };
  const auto farCell = p.worldState.CellOf(kTamriel, far);
  REQUIRE(farCell.has_value());
  CellLoaded(p, 1, *farCell);
  REQUIRE(p.GetCellHost(*farCell).player == kC2);
  p.Messages().clear();
  sighting(1, kSigrid, far);
  REQUIRE(p.worldState.CellOf(sigrid) == cell);
  p.Tick(); // snippets are deferred (SpSnippet.cpp; pipeline 3613's red)
  bool moved = false;
  bool disabled = false;
  for (auto& m : p.Messages()) {
    if (!(m.userId == 1 && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::SpSnippet) &&
          m.j.contains("class") && m.j.contains("function"))) {
      continue;
    }
    if (m.j["class"] == "ObjectReference" && m.j["function"] == "Disable") {
      disabled = true;
    }
    if (m.j["class"] == "TESModPlatform" &&
        m.j["function"] == "moveRefrToPosition") {
      moved = true;
      const auto& args = m.j["arguments"];
      REQUIRE(args.is_array());
      REQUIRE(args.size() == 9);
      REQUIRE(args[0]["formId"] == kSigrid);
      REQUIRE(args[0]["type"] == "ObjectReference");
      REQUIRE(args[1]["type"] == "Cell");
      REQUIRE(args[2]["type"] == "WorldSpace");
      REQUIRE(args[1]["formId"] == args[2]["formId"]);
      REQUIRE(std::abs(args[3].get<double>() - sigridPos.x) < 1.0);
      REQUIRE(std::abs(args[4].get<double>() - sigridPos.y) < 1.0);
      REQUIRE(std::abs(args[5].get<double>() - sigridPos.z) < 1.0);
    }
  }
  REQUIRE(moved);
  REQUIRE_FALSE(disabled);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kSigrid);
}

// thuum docs/verbs/npc-puppet.md (playtest 38's finding 66): the host's
// roster that reaches the server before its cell's grant (the same
// millisecond, live) is held and replayed at the grant, so the cell's
// actors are recorded within a round trip instead of a roster later
TEST_CASE("A sighting that arrives before its cell's grant is held and "
          "replayed for the grant's host (finding 66)",
          "[CellHost][espm]")
{
  constexpr uint32_t kSigrid = 0x13483;
  const NiPoint3 sigridPos{ 24063.7f, -45204.2f, -40.6f };
  const NiPoint3 landing{ 17225.f, -47204.f, -128.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSigrid) == nullptr);

  Player(p, 0, kC1, landing);
  const auto cell = p.worldState.CellOf(kTamriel, sigridPos);
  REQUIRE(cell.has_value());
  REQUIRE_FALSE(p.GetCellHost(*cell).hosted);
  p.Messages().clear();

  // the roster first: nobody's cell yet, so dropped and held
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", MsgType::ActorSightings },
                    { "sightings",
                      nlohmann::json::array({ nlohmann::json{
                        { "refrId", kSigrid },
                        { "worldOrCell", kTamriel },
                        { "pos", { sigridPos.x, sigridPos.y, sigridPos.z } },
                        { "rot", { 0.f, 0.f, 165.3f } } } }) } });
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSigrid) == nullptr);

  // the grant: the held sighting replays for the new host at once
  CellLoaded(p, 0, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSigrid) != nullptr);
  auto& sigrid = p.worldState.GetFormAt<MpActor>(kSigrid);
  REQUIRE(sigrid.GetPos().x == Catch::Approx(sigridPos.x));
  REQUIRE(sigrid.GetPos().y == Catch::Approx(sigridPos.y));
  REQUIRE(p.worldState.HostOf(sigrid) ==
          &p.worldState.GetFormAt<MpActor>(kC1));

  // a second grant has nothing left to replay
  p.Messages().clear();
  p.ReplayHeldSightings(*cell, p.worldState.GetFormAt<MpActor>(kC1));
  REQUIRE(SentTo<CreateActorMessage>(p, 0).empty());

  DoDisconnect(p, 0);
  p.DestroyActor(kC1);
  p.DestroyActor(kSigrid);
}

// the train's review (2026-10-29): the hold keyed by reference alone let a
// second game's roster of the same bandit, landing last, evict the first's
// entry, and the grant replayed the host's own entries only, so with c2's
// roster last and c1's grant nothing replayed. One entry per (reporter,
// reference); the grant replays the new host's own and keeps the other
// reporter's for its own grant while they last
TEST_CASE("Held sightings are the pair's: a second reporter's roster of the "
          "same reference keeps the first's entry, the grant replays the "
          "host's own and keeps the other's for its own grant (finding 66)",
          "[CellHost][espm]")
{
  constexpr uint32_t kSigrid = 0x13483;
  constexpr uint32_t kHod = 0x1348a;
  const NiPoint3 sigridPos{ 24063.7f, -45204.2f, -40.6f };
  const NiPoint3 hodPos = sigridPos + NiPoint3{ 60.f, 0.f, 0.f };
  const NiPoint3 landing{ 17225.f, -47204.f, -128.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSigrid) == nullptr);
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kHod) == nullptr);
  // an earlier case's leftovers expire first
  p.SweepHeldSightings(NowMs() + PartOne::kHeldSightingMs + 1);
  REQUIRE(p.HeldSightingsCount() == 0);

  Player(p, 0, kC1, landing);
  Player(p, 1, kC2, landing + NiPoint3{ 50.f, 0.f, 0.f });
  const auto cell = p.worldState.CellOf(kTamriel, sigridPos);
  REQUIRE(cell.has_value());
  REQUIRE_FALSE(p.GetCellHost(*cell).hosted);
  const auto sights = [&](Networking::UserId user, uint32_t refrId,
                          const NiPoint3& at) {
    DoMessage(p, user,
              nlohmann::json{ { "t", MsgType::ActorSightings },
                              { "sightings",
                                nlohmann::json::array({ nlohmann::json{
                                  { "refrId", refrId },
                                  { "worldOrCell", kTamriel },
                                  { "pos", { at.x, at.y, at.z } },
                                  { "rot", { 0.f, 0.f, 0.f } } } }) } });
  };

  // c1's roster, twice (one entry); c2's of the same reference last (its
  // own entry beside c1's, not over it); c2's of Hod
  sights(0, kSigrid, sigridPos);
  sights(0, kSigrid, sigridPos);
  REQUIRE(p.HeldSightingsCount(*cell) == 1);
  sights(1, kSigrid, sigridPos);
  REQUIRE(p.HeldSightingsCount(*cell) == 2);
  sights(1, kHod, hodPos);
  REQUIRE(p.HeldSightingsCount(*cell) == 3);
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSigrid) == nullptr);

  // c1's grant: its own entry replays (Sigrid stands), c2's two stay held
  CellLoaded(p, 0, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSigrid) != nullptr);
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kHod) == nullptr);
  REQUIRE(p.HeldSightingsCount(*cell) == 2);

  // c1 leaves; past the grace c2, with the cell loaded, takes it and its own
  // entries replay: Hod stands where c2's engine had it, the hold is empty
  const uint64_t now = NowMs();
  DoDisconnect(p, 0);
  CellLoaded(p, 1, *cell);
  p.TickCellHosts(now + skymp::rules::host_grace_ms() + kPastMs);
  REQUIRE(p.GetCellHost(*cell).player == kC2);
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kHod) != nullptr);
  auto& hod = p.worldState.GetFormAt<MpActor>(kHod);
  REQUIRE(hod.GetPos().x == Catch::Approx(hodPos.x));
  REQUIRE(p.HeldSightingsCount(*cell) == 0);

  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kSigrid);
  p.DestroyActor(kHod);
}

// the train's review (2026-10-29): the hold grew with no total cap, nothing
// swept it on a tick, and a departed reporter's entries stayed. The sweep
// runs from the once-a-second tick (TickCellHostsEverySecond); a reporter's
// disconnect forgets its own; kHeldSightingsTotal across cells, the oldest
// entry anywhere making room
TEST_CASE("The hold is bounded: the sweep expires held sightings, a "
          "reporter's disconnect forgets its own, and the total cap drops "
          "the oldest (finding 66)",
          "[CellHost][espm]")
{
  constexpr uint32_t kSigrid = 0x13483;
  const NiPoint3 sigridPos{ 24063.7f, -45204.2f, -40.6f };
  const NiPoint3 landing{ 17225.f, -47204.f, -128.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSigrid) == nullptr);
  p.SweepHeldSightings(NowMs() + PartOne::kHeldSightingMs + 1);
  REQUIRE(p.HeldSightingsCount() == 0);
  Player(p, 0, kC1, landing);
  Player(p, 1, kC2, landing + NiPoint3{ 50.f, 0.f, 0.f });
  const auto cell = p.worldState.CellOf(kTamriel, sigridPos);
  REQUIRE(cell.has_value());
  REQUIRE_FALSE(p.GetCellHost(*cell).hosted);
  const auto sights = [&](Networking::UserId user, uint32_t refrId,
                          const NiPoint3& at) {
    DoMessage(p, user,
              nlohmann::json{ { "t", MsgType::ActorSightings },
                              { "sightings",
                                nlohmann::json::array({ nlohmann::json{
                                  { "refrId", refrId },
                                  { "worldOrCell", kTamriel },
                                  { "pos", { at.x, at.y, at.z } },
                                  { "rot", { 0.f, 0.f, 0.f } } } }) } });
  };

  // the sweep: an entry past kHeldSightingMs goes
  sights(0, kSigrid, sigridPos);
  REQUIRE(p.HeldSightingsCount(*cell) == 1);
  p.SweepHeldSightings(NowMs() + PartOne::kHeldSightingMs + 1);
  REQUIRE(p.HeldSightingsCount(*cell) == 0);

  // the disconnect: c1's entry goes with it, and the grant to c2 that
  // follows finds nothing of c1's to replay
  sights(0, kSigrid, sigridPos);
  REQUIRE(p.HeldSightingsCount(*cell) == 1);
  DoDisconnect(p, 0);
  REQUIRE(p.HeldSightingsCount(*cell) == 0);
  CellLoaded(p, 1, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC2);
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSigrid) == nullptr);

  // the total cap: seventeen unhosted cells along Tamriel's x axis in the
  // grid row above the one the other cases host (an earlier case's departed
  // host may still hold its cell in grace, so every grace passes first), a
  // full roster of references the server never looks up for each (the hold
  // needs no record), kHeldSightingsPerCell apiece until the total is
  // reached; from there each new entry costs the oldest one anywhere, and
  // the hold is full, not closed
  p.TickCellHosts(NowMs() + skymp::rules::host_grace_ms() + kPastMs);
  constexpr size_t kCells = 17;
  static_assert(kCells * PartOne::kHeldSightingsPerCell >
                PartOne::kHeldSightingsTotal);
  const auto capCellPos = [](size_t c) {
    return NiPoint3{ 9000.f + 4096.f * static_cast<float>(c), 4600.f, 0.f };
  };
  for (size_t c = 0; c < kCells; ++c) {
    const NiPoint3 at = capCellPos(c);
    const auto capCell = p.worldState.CellOf(kTamriel, at);
    REQUIRE(capCell.has_value());
    REQUIRE_FALSE(p.GetCellHost(*capCell).hosted);
    nlohmann::json roster = nlohmann::json::array();
    for (size_t i = 0; i < PartOne::kHeldSightingsPerCell; ++i) {
      roster.push_back(
        nlohmann::json{ { "refrId", 0x00f00000 + c * 0x100 + i },
                        { "worldOrCell", kTamriel },
                        { "pos", { at.x, at.y, at.z } },
                        { "rot", { 0.f, 0.f, 0.f } } });
    }
    DoMessage(p, 1,
              nlohmann::json{ { "t", MsgType::ActorSightings },
                              { "sightings", roster } });
  }
  REQUIRE(p.HeldSightingsCount() == PartOne::kHeldSightingsTotal);
  sights(1, 0x00f0ffff, capCellPos(0));
  REQUIRE(p.HeldSightingsCount() == PartOne::kHeldSightingsTotal);

  p.SweepHeldSightings(NowMs() + PartOne::kHeldSightingMs + 1);
  REQUIRE(p.HeldSightingsCount() == 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

TEST_CASE("An actor from the game files travels under its own id with the "
          "server's explicit hosted word, the grant flips it, and only the "
          "host's reports move it",
          "[CellHost][espm]")
{
  // thuum docs/verbs/npc-puppet.md (ADR-045 point 2 as revised 2026-10-22):
  // the client binds the engine's own reference to such an actor and runs
  // it or puppets it by the server's word, isHostedByOther explicit either
  // way under the raw id (upstream's id + 0x100000000 made the flip miss the
  // copy: playtest thirty's spin; the client's own cell test misses a
  // persistent reference: b-npc-puppet 20261022-081631)
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& cow = p.worldState.GetFormAt<MpActor>(kCow);
  const NiPoint3 cowPos = cow.GetPos();
  const auto cell = p.worldState.CellOf(cow);
  REQUIRE(cell.has_value());
  REQUIRE(*cell != 0);

  // two players beside the cow: the grid subscribes both to it
  DoConnect(p, 0);
  p.CreateActor(kC1, cowPos + NiPoint3{ 100.f, 0.f, 0.f }, 0, kWhiterun);
  p.SetUserActor(0, kC1);
  DoConnect(p, 1);
  p.CreateActor(kC2, cowPos + NiPoint3{ 200.f, 0.f, 0.f }, 0, kWhiterun);
  p.SetUserActor(1, kC2);
  const auto cowCreated = [&](Networking::UserId user) {
    std::vector<CreateActorMessage> out;
    for (auto& m : SentTo<CreateActorMessage>(p, user)) {
      if (m.refrId == kCow) {
        out.push_back(m);
      }
    }
    return out;
  };
  // nobody hosts the cell yet: hosted by other on both screens, said so
  const auto toC1 = cowCreated(0);
  REQUIRE(toC1.size() == 1);
  REQUIRE(toC1.front().baseRecordType == "NPC_");
  REQUIRE(toC1.front().props.isHostedByOther == true);
  const auto toC2 = cowCreated(1);
  REQUIRE(toC2.size() == 1);
  REQUIRE(toC2.front().props.isHostedByOther == true);
  p.Messages().clear();

  // c1 hosts the cell: the grant, the cow's word flipped to c1 alone (the
  // last word each game hears is the one it keeps)
  CellLoaded(p, 0, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  REQUIRE(SentTo<HostGrantMessage>(p, 0).size() == 1);
  // the word names the record's type, or the client files it under a plain
  // engine reference and drops it (b-npc-puppet 20261022-104032's handoff)
  std::string wordType;
  REQUIRE(LastHostedWord(p, 0, cow, nullptr, &wordType) == "false");
  REQUIRE(wordType == "NPC_");
  REQUIRE(LastHostedWord(p, 1, cow) == "true");

  // the host's report moves the cow and reaches the other game once, not
  // the host; the other game's report is dropped
  p.Messages().clear();
  Move(p, 0, kCow, cowPos + NiPoint3{ 20.f, 0.f, 0.f }, kWhiterun);
  REQUIRE(cow.GetPos().x == Catch::Approx(cowPos.x + 20.f));
  Move(p, 1, kCow, cowPos + NiPoint3{ 40.f, 0.f, 0.f }, kWhiterun);
  REQUIRE(cow.GetPos().x == Catch::Approx(cowPos.x + 20.f));
  const auto relays = [&](Networking::UserId user) {
    size_t n = 0;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.empty()) {
        ++n;
      }
    }
    return n;
  };
  REQUIRE(relays(1) == 1);
  REQUIRE(relays(0) == 0);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kCow);
}

TEST_CASE("A hosted actor's actions travel from its host alone: the host's "
          "UpdateAction under the actor's idx reaches the other game once and "
          "never the host, and the other game's is dropped",
          "[CellHost][Action]")
{
  // thuum docs/verbs/npc-stream.md (ADR-050 decision 1): the host's engine
  // performs a hosted actor's actions through its one action entry, the
  // hook reports them under the actor's id, and the puppets perform the
  // same action through the same entry. The gate is ActorUpdatableBy's (the
  // host of the actor's cell, as for a movement report), the relay the
  // bytes as they came, reliable, to every listener but the sender
  auto& p = GetPartOne();
  constexpr uint32_t kActor = 0xff000c06;
  const NiPoint3 kAt{ 117786.f, -81400.f, 10997.f };
  Player(p, 0, kC1, kAt + NiPoint3{ 40.f, 0.f, 0.f });
  Player(p, 1, kC2, kAt + NiPoint3{ 60.f, 0.f, 0.f });
  p.CreateActor(kActor, kAt + NiPoint3{ 30.f, 0.f, 0.f }, 0, kTamriel);
  auto& actor = p.worldState.GetFormAt<MpActor>(kActor);
  actor.SetPos(actor.GetPos());
  const auto cell = p.worldState.CellOf(actor);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  REQUIRE(p.worldState.HostOf(actor) == &p.worldState.GetFormAt<MpActor>(kC1));
  const auto relays = [&](Networking::UserId user) {
    size_t n = 0;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.empty()) {
        ++n;
      }
    }
    return n;
  };

  // the host's report of its actor's action: relayed to the other game
  // once, never back to the host
  p.Messages().clear();
  Act(p, 0, kActor);
  REQUIRE(relays(1) == 1);
  REQUIRE(relays(0) == 0);

  // the other game's report about the same actor: dropped, nothing sent
  p.Messages().clear();
  Act(p, 1, kActor);
  REQUIRE(relays(0) == 0);
  REQUIRE(relays(1) == 0);

  // the player's own action under its own idx, as cast-action has it
  p.Messages().clear();
  Act(p, 0, kC1);
  REQUIRE(relays(1) == 1);
  REQUIRE(relays(0) == 0);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kActor);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

TEST_CASE("A hosted actor's state is its host's word bounded by the server's "
          "numbers: the host's values reach the record and the other game "
          "and never the host, a non-host's and the player's own idx are "
          "dropped, an equal report sends nothing, a zero is floored at one "
          "point, and a report that would undo the server's own lowering "
          "waits for the hold",
          "[CellHost][HostedState]")
{
  // thuum docs/verbs/npc-stream.md (ADR-050 decision 1, slice 2): the host's
  // engine runs the actor's regeneration and its fights, so its percentages
  // are the record's source (R2), bounded by the server's numbers
  // (wire-rules hosted_state); the target and the package are recorded; the
  // changed values travel as ChangeValues to the actor's other listeners
  auto& p = GetPartOne();
  constexpr uint32_t kActor = 0xff000c07;
  const NiPoint3 kAt{ 117786.f, -81400.f, 10997.f };
  Player(p, 0, kC1, kAt + NiPoint3{ 40.f, 0.f, 0.f });
  Player(p, 1, kC2, kAt + NiPoint3{ 60.f, 0.f, 0.f });
  p.CreateActor(kActor, kAt + NiPoint3{ 30.f, 0.f, 0.f }, 0, kTamriel);
  auto& actor = p.worldState.GetFormAt<MpActor>(kActor);
  actor.SetPos(actor.GetPos());
  const auto cell = p.worldState.CellOf(actor);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  auto& c1 = p.worldState.GetFormAt<MpActor>(kC1);
  REQUIRE(p.worldState.HostOf(actor) == &c1);
  const auto health = [&]() {
    return actor.GetActorValues().healthPercentage;
  };

  // the host's state: the record takes the values, the other game hears the
  // changed ones (magicka unchanged at 1), the host hears nothing
  p.Messages().clear();
  State(p, 0, kActor, 0.5f, 1.f, 0.75f, kC1, 0x40000);
  REQUIRE(health() == Catch::Approx(0.5f));
  REQUIRE(actor.GetActorValues().staminaPercentage == Catch::Approx(0.75f));
  REQUIRE(actor.GetActorValues().magickaPercentage == Catch::Approx(1.f));
  REQUIRE(actor.GetHostedCombatTarget() == kC1);
  REQUIRE(actor.GetHostedPackage() == 0x40000);
  const auto toC2 = SentTo<ChangeValuesMessage>(p, 1);
  REQUIRE(toC2.size() == 1);
  REQUIRE(toC2.front().idx == actor.GetIdx());
  REQUIRE(toC2.front().data.health.has_value());
  REQUIRE(*toC2.front().data.health == Catch::Approx(0.5f));
  REQUIRE(toC2.front().data.stamina.has_value());
  REQUIRE_FALSE(toC2.front().data.magicka.has_value());
  REQUIRE(SentTo<ChangeValuesMessage>(p, 0).empty());
  // slice 5: the state itself reaches the other game (the gaze and the face
  // ride it), never the host
  REQUIRE(SentTo<HostedActorStateMessage>(p, 1).size() == 1);
  REQUIRE(SentTo<HostedActorStateMessage>(p, 1).front().idx == actor.GetIdx());
  REQUIRE(SentTo<HostedActorStateMessage>(p, 0).empty());
  p.Messages().clear();
  State(p, 0, kActor, 0.5f, 1.f, 0.75f, kC1, 0x40000, kC2, 4, 5, 60);
  REQUIRE(actor.GetHostedGazeTarget() == kC2);
  REQUIRE(actor.GetHostedGazeSlot() == 4);
  REQUIRE(actor.GetHostedEmotionType() == 5);
  REQUIRE(actor.GetHostedEmotionValue() == 60);
  REQUIRE(SentTo<HostedActorStateMessage>(p, 1).size() == 1);
  REQUIRE(SentTo<HostedActorStateMessage>(p, 1).front().gazeTarget == kC2);
  REQUIRE(SentTo<HostedActorStateMessage>(p, 1).front().emotionValue == 60);
  // slice 4's counters: the host's states accepted under its row, a
  // non-host's dropped as foreign
  State(p, 1, kActor, 0.4f, 1.f, 1.f, 0, 0);
  bool hostRow = false;
  bool otherRow = false;
  for (const auto& row : p.serverState.StreamRows()) {
    if (row.msgType != static_cast<uint8_t>(MsgType::HostedActorState)) {
      continue;
    }
    if (row.user == 0 && row.accepted >= 2) {
      hostRow = true;
    }
    if (row.user == 1 && row.dropped >= 1 && row.foreign >= 1) {
      otherRow = true;
    }
  }
  REQUIRE(hostRow);
  REQUIRE(otherRow);

  // the other game's state about the same actor: dropped at the gate
  p.Messages().clear();
  State(p, 1, kActor, 0.1f, 0.1f, 0.1f, 0, 0);
  REQUIRE(health() == Catch::Approx(0.5f));
  REQUIRE(actor.GetHostedCombatTarget() == kC1);
  REQUIRE(SentTo<ChangeValuesMessage>(p, 0).empty());
  REQUIRE(SentTo<ChangeValuesMessage>(p, 1).empty());

  // the player's own idx: refused, its record untouched
  const float c1Health = c1.GetActorValues().healthPercentage;
  State(p, 0, kC1, 0.2f, 0.2f, 0.2f, 0, 0);
  REQUIRE(c1.GetActorValues().healthPercentage == Catch::Approx(c1Health));

  // the same values again: nothing changes, nothing is sent
  p.Messages().clear();
  State(p, 0, kActor, 0.5f, 1.f, 0.75f, kC1, 0x40000);
  REQUIRE(SentTo<ChangeValuesMessage>(p, 1).empty());

  // a zero on the living record: floored at one point, no death
  p.Messages().clear();
  State(p, 0, kActor, 0.f, 1.f, 0.75f, kC1, 0x40000);
  const float onePoint = 1.f / std::max(actor.GetMaximumValues().health, 1.f);
  REQUIRE(health() == Catch::Approx(onePoint));
  REQUIRE_FALSE(actor.IsDead());
  REQUIRE(SentTo<ChangeValuesMessage>(p, 1).size() == 1);

  // the wire's own path once: the JSON the client sends
  p.Messages().clear();
  DoMessage(p, 0,
            nlohmann::json{ { "t", MsgType::HostedActorState },
                            { "idx", actor.GetIdx() },
                            { "health", 0.45 },
                            { "magicka", 1.0 },
                            { "stamina", 0.75 },
                            { "combatTarget", 0 },
                            { "package", 0 },
                            { "gazeTarget", 0 },
                            { "gazeSlot", 0 },
                            { "emotionType", 0 },
                            { "emotionValue", 0 } });
  REQUIRE(health() == Catch::Approx(0.45f));
  REQUIRE(actor.GetHostedCombatTarget() == 0);

  // the server lowered the health by its own number a moment ago (a hit it
  // judged): the host's higher report is the engine before the hit, dropped;
  // its lower one is taken
  actor.SetPercentage(espm::ActorValue::Health, 0.3f);
  actor.SetLastHitTakenTime(std::chrono::steady_clock::now());
  State(p, 0, kActor, 0.9f, 1.f, 0.75f, kC1, 0x40000);
  REQUIRE(health() == Catch::Approx(0.3f));
  State(p, 0, kActor, 0.25f, 1.f, 0.75f, kC1, 0x40000);
  REQUIRE(health() == Catch::Approx(0.25f));

  // the host's animation for its actor is kept as the actor's last event (a
  // later subscriber's CreateActor carries it); the other game's is dropped
  // at the gate with the event untouched
  {
    static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
    RawMessageData raw;
    raw.userId = 0;
    raw.unparsed = unparsed;
    raw.unparsedLength = sizeof(unparsed);
    UpdateAnimationMessage anim;
    anim.idx = actor.GetIdx();
    anim.data.animEventName = "IdleWoodChopStart";
    anim.data.numChanges = 1;
    p.GetActionListener().OnUpdateAnimation(raw, anim);
    REQUIRE(actor.GetLastAnimEvent().has_value());
    REQUIRE(actor.GetLastAnimEvent()->animEventName == "IdleWoodChopStart");
    raw.userId = 1;
    anim.data.animEventName = "IdleStop";
    anim.data.numChanges = 2;
    p.GetActionListener().OnUpdateAnimation(raw, anim);
    REQUIRE(actor.GetLastAnimEvent()->animEventName == "IdleWoodChopStart");
  }

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kActor);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

// thuum docs/verbs/npc-puppet.md, Ownership (ADR-063, ADR-073 decision 1):
// the effects the host's engine runs on its actor with a look are that
// engine's; the server records the set the host names (R2), relays a report
// that carries it reliable to the actor's other games and never the host,
// keeps it through a report without it, takes it even when the values are
// dropped as a dead record's, serves it to a game that subscribes later, and
// drops a non-host's at the gate. Uncompiled where it was written (the
// server builds on sky-srv and in CI)
TEST_CASE("The host's running effects are recorded, relayed reliable to the "
          "actor's other games and served to a later one; a non-host's are "
          "dropped",
          "[CellHost][HostedState][HostedEffects]")
{
  auto& p = GetPartOne();
  constexpr uint32_t kActor = 0xff000c0a;
  // AbFXAtronachFlame and another load-order effect (any ids serve: the
  // record is the host's word, the wire checks the shape)
  constexpr uint32_t kFlames = 0x60da1;
  constexpr uint32_t kOther = 0x64459;
  const NiPoint3 kAt{ 117786.f, -81400.f, 10997.f };
  Player(p, 0, kC1, kAt + NiPoint3{ 40.f, 0.f, 0.f });
  Player(p, 1, kC2, kAt + NiPoint3{ 60.f, 0.f, 0.f });
  p.CreateActor(kActor, kAt + NiPoint3{ 30.f, 0.f, 0.f }, 0, kTamriel);
  auto& actor = p.worldState.GetFormAt<MpActor>(kActor);
  actor.SetPos(actor.GetPos());
  const auto cell = p.worldState.CellOf(actor);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  auto& c1 = p.worldState.GetFormAt<MpActor>(kC1);
  REQUIRE(p.worldState.HostOf(actor) == &c1);
  REQUIRE(actor.GetHostedEffects().empty());

  // the states a user was sent for the actor, with their reliability
  const auto statesTo = [&](Networking::UserId user) {
    std::vector<std::pair<HostedActorStateMessage, bool>> out;
    for (auto& m : p.Messages()) {
      if (m.userId != user) {
        continue;
      }
      if (auto typed = dynamic_cast<HostedActorStateMessage*>(m.message.get());
          typed && typed->idx == actor.GetIdx()) {
        out.emplace_back(*typed, m.reliable);
      }
    }
    return out;
  };

  // the host's first report with its engine's read: recorded, relayed
  // reliable to the other game with the set, never to the host
  p.Messages().clear();
  State(p, 0, kActor, 1.f, 1.f, 1.f, 0, 0, 0, 0, 0, 0,
        std::vector<uint32_t>{ kFlames, kOther });
  REQUIRE(actor.GetHostedEffects() ==
          std::vector<uint32_t>{ kFlames, kOther });
  {
    const auto toC2 = statesTo(1);
    REQUIRE(toC2.size() == 1);
    REQUIRE(toC2.front().second);
    REQUIRE(toC2.front().first.effects.has_value());
    REQUIRE(*toC2.front().first.effects ==
            std::vector<uint32_t>{ kFlames, kOther });
    REQUIRE(statesTo(0).empty());
  }

  // a report without the set: the record keeps it, the relay is unreliable
  // and carries none
  p.Messages().clear();
  State(p, 0, kActor, 0.9f, 1.f, 1.f, 0, 0);
  REQUIRE(actor.GetHostedEffects() ==
          std::vector<uint32_t>{ kFlames, kOther });
  {
    const auto toC2 = statesTo(1);
    REQUIRE(toC2.size() == 1);
    REQUIRE_FALSE(toC2.front().second);
    REQUIRE_FALSE(toC2.front().first.effects.has_value());
  }

  // the other game's report of the same actor: dropped at the gate, the
  // record untouched
  State(p, 1, kActor, 0.9f, 1.f, 1.f, 0, 0, 0, 0, 0, 0,
        std::vector<uint32_t>{});
  REQUIRE(actor.GetHostedEffects() ==
          std::vector<uint32_t>{ kFlames, kOther });

  // an effect left the set (its conditions went false on the host's
  // engine: the kInactive flag): the record and the relay follow
  p.Messages().clear();
  State(p, 0, kActor, 0.9f, 1.f, 1.f, 0, 0, 0, 0, 0, 0,
        std::vector<uint32_t>{ kFlames });
  REQUIRE(actor.GetHostedEffects() == std::vector<uint32_t>{ kFlames });
  REQUIRE(statesTo(1).size() == 1);
  REQUIRE(statesTo(1).front().second);

  // a game that subscribes later hears the record after the actor's
  // creation, reliable; the host is never served its own
  p.Messages().clear();
  Player(p, 2, kC3, kAt + NiPoint3{ 80.f, 0.f, 0.f });
  {
    const auto toC3 = statesTo(2);
    REQUIRE(toC3.size() == 1);
    REQUIRE(toC3.front().second);
    REQUIRE(toC3.front().first.effects.has_value());
    REQUIRE(*toC3.front().first.effects == std::vector<uint32_t>{ kFlames });
    REQUIRE(statesTo(0).empty());
  }

  // a dead record drops the values but takes the set the host's engine
  // keeps on the body, relayed reliable
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  p.Messages().clear();
  State(p, 0, kActor, 0.f, 0.f, 0.f, 0, 0, 0, 0, 0, 0,
        std::vector<uint32_t>{});
  REQUIRE(actor.GetHostedEffects().empty());
  REQUIRE(statesTo(1).size() == 1);
  REQUIRE(statesTo(1).front().second);
  // and an empty record serves nothing to a later game
  p.Messages().clear();
  actor.SendHostedEffectsTo(p.worldState.GetFormAt<MpActor>(kC2));
  REQUIRE(statesTo(1).empty());

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  DoDisconnect(p, 2);
  p.DestroyActor(kActor);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kC3);
}

// thuum tracker #119 (playtest 53, Eli: "ariadne just died to sunlight ... i
// didn't see gold fire effect on her"): a player's own game names the effects
// with a look its engine runs on its player, as a host does its actor's; the
// server records the set on the player, relays it reliable to the player's
// other games (never back), serves it to a game that subscribes later, keeps
// dropping a report without a set (E_STATE_OWN: the values are ChangeValues'),
// and starts every login with none. Uncompiled where it was written (the
// server builds on sky-srv and in CI)
TEST_CASE("A player's own running effects are recorded, relayed reliable to "
          "its other games and served to a later one; a new login starts "
          "with none",
          "[CellHost][HostedState][HostedEffects]")
{
  auto& p = GetPartOne();
  // SunAffectsNPCVampires' _SunBurningEffect at its light slot in the
  // session's load order (fe01f, its quest fe01f800 on playtest 53's seat),
  // and another (any ids serve: the record is the owner's word)
  constexpr uint32_t kBurn = 0xfe01f802;
  constexpr uint32_t kOther = 0x60da1;
  const NiPoint3 kAt{ 117786.f, -81400.f, 10997.f };
  auto& c1 = Player(p, 0, kC1, kAt + NiPoint3{ 40.f, 0.f, 0.f });
  auto& c2 = Player(p, 1, kC2, kAt + NiPoint3{ 60.f, 0.f, 0.f });
  REQUIRE(c2.GetHostedEffects().empty());

  // the states a user was sent for c2's player, with their reliability
  const auto statesTo = [&](Networking::UserId user) {
    std::vector<std::pair<HostedActorStateMessage, bool>> out;
    for (auto& m : p.Messages()) {
      if (m.userId != user) {
        continue;
      }
      if (auto typed = dynamic_cast<HostedActorStateMessage*>(m.message.get());
          typed && typed->idx == c2.GetIdx()) {
        out.emplace_back(*typed, m.reliable);
      }
    }
    return out;
  };

  // c2's own report with its engine's read: recorded on its player, relayed
  // reliable to c1 with the set, never back to c2
  p.Messages().clear();
  State(p, 1, kC2, 0.4f, 1.f, 1.f, 0, 0, 0, 0, 0, 0,
        std::vector<uint32_t>{ kOther, kBurn });
  REQUIRE(c2.GetHostedEffects() == std::vector<uint32_t>{ kOther, kBurn });
  {
    const auto toC1 = statesTo(0);
    REQUIRE(toC1.size() == 1);
    REQUIRE(toC1.front().second);
    REQUIRE(toC1.front().first.effects.has_value());
    REQUIRE(*toC1.front().first.effects ==
            std::vector<uint32_t>{ kOther, kBurn });
    REQUIRE(statesTo(1).empty());
  }

  // a report of its own with no set: dropped as before, the record kept,
  // nothing relayed
  p.Messages().clear();
  State(p, 1, kC2, 0.4f, 1.f, 1.f, 0, 0);
  REQUIRE(c2.GetHostedEffects() == std::vector<uint32_t>{ kOther, kBurn });
  REQUIRE(statesTo(0).empty());

  // the burn left the set (indoors, its conditions false on c2's engine):
  // the record and the relay follow
  p.Messages().clear();
  State(p, 1, kC2, 0.4f, 1.f, 1.f, 0, 0, 0, 0, 0, 0,
        std::vector<uint32_t>{ kOther });
  REQUIRE(c2.GetHostedEffects() == std::vector<uint32_t>{ kOther });
  REQUIRE(statesTo(0).size() == 1);
  REQUIRE(statesTo(0).front().second);

  // a game that subscribes later hears the record, reliable
  p.Messages().clear();
  Player(p, 2, kC3, kAt + NiPoint3{ 80.f, 0.f, 0.f });
  {
    const auto toC3 = statesTo(2);
    REQUIRE(toC3.size() == 1);
    REQUIRE(toC3.front().second);
    REQUIRE(toC3.front().first.effects.has_value());
    REQUIRE(*toC3.front().first.effects == std::vector<uint32_t>{ kOther });
    REQUIRE(statesTo(1).empty());
  }

  // c2's next login starts with none recorded: nothing is served until its
  // game's first read
  DoDisconnect(p, 1);
  DoConnect(p, 1);
  p.SetUserActor(1, kC2);
  REQUIRE(c2.GetHostedEffects().empty());
  REQUIRE(c1.GetHostedEffects().empty());

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  DoDisconnect(p, 2);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kC3);
}

// `user`'s equipment report for `actorId`: one worn item in the right hand
// (thuum docs/verbs/npc-stream.md slice 3), no spells
void Wear(PartOne& p, Networking::UserId user, uint32_t actorId,
          uint32_t baseId, uint32_t numChanges)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateEquipmentMessage msg;
  msg.idx = p.worldState.GetFormAt<MpActor>(actorId).GetIdx();
  msg.data.numChanges = numChanges;
  Inventory::Entry entry;
  entry.baseId = baseId;
  entry.count = 1;
  entry.SetWorn(Inventory::Worn::Right);
  msg.data.inv.entries.push_back(entry);
  p.GetActionListener().OnUpdateEquipment(raw, msg);
}

TEST_CASE("A hosted actor's equipment is its host's engine's word checked "
          "against the actor's own record: a worn item the record holds goes "
          "into the actor's record and to the other game alone, the host "
          "player's record stays, a non-host's report is dropped, and an "
          "item the record lacks is recorded with nothing sent to the host",
          "[CellHost][HostedEquipment][espm]")
{
  // thuum docs/verbs/npc-stream.md (ADR-050 decision 1, slice 3): the
  // record's hands are what the hit path judges a hosted actor's swing
  // against (E_HIT_WEAPON, playtest 34), so the host streams what its actor
  // wears; before this the handler resolved any idx to the sender's own actor
  auto& p = GetPartOne();
  constexpr uint32_t kActor = 0xff000c08;
  // Skyrim.esm's iron sword, a WEAP record (the slot rule looks it up and
  // passes it by)
  constexpr uint32_t kIronSword = 0x12eb7;
  // Skyrim.esm's steel sword, a WEAP the record lacks, and its steel helmet
  // ArmorSteelHelmetA, an ARMO the record lacks (lab/esm.py): the actor's
  // fate dresses it in the iron set, so the iron helmet 0x12e4d is already
  // its own (3b's first T0, "outfit kept ... 4 worn item(s)", right 12e4d).
  // Both helmets' BOD2 name the slots 0x1002 (lab/esm.py id, sky-srv,
  // 2026-11-14): the steel one takes the iron one's place
  constexpr uint32_t kElsewhere = 0x13989;
  constexpr uint32_t kSteelHelmet = 0x13954;
  // Skyrim.esm's Torch01, a LIGH record the record lacks
  constexpr uint32_t kTorch = 0x1d4ec;
  const NiPoint3 kAt{ 117786.f, -81400.f, 10997.f };
  Player(p, 0, kC1, kAt + NiPoint3{ 40.f, 0.f, 0.f });
  Player(p, 1, kC2, kAt + NiPoint3{ 60.f, 0.f, 0.f });
  p.CreateActor(kActor, kAt + NiPoint3{ 30.f, 0.f, 0.f }, 0, kTamriel);
  auto& actor = p.worldState.GetFormAt<MpActor>(kActor);
  actor.SetPos(actor.GetPos());
  actor.AddItem(kIronSword, 1);
  const auto cell = p.worldState.CellOf(actor);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  auto& c1 = p.worldState.GetFormAt<MpActor>(kC1);
  REQUIRE(p.worldState.HostOf(actor) == &c1);
  const auto relays = [&](Networking::UserId user) {
    size_t n = 0;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.empty()) {
        ++n;
      }
    }
    return n;
  };
  const auto sentOfType = [&](Networking::UserId user, MsgType type) {
    size_t n = 0;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(type)) {
        ++n;
      }
    }
    return n;
  };

  // the host's report: the actor's record takes it, the other game hears
  // the relay once, the host hears nothing, the host player's own record
  // stays as it was
  const auto c1Before = c1.GetEquipment();
  // the record's worn outfit before the report: what a first report never
  // shrinks (playtest 37's finding 46: the fate's worn list is the outfit
  // every seat dresses the copy from, and a host's first read can come
  // before its engine dressed the copy); fork m4-npc-worn: an entry the
  // record wears comes off when this host's own earlier report wore it, or
  // when a reported entry takes its hand or its armor slots
  const auto isArmor = [&](uint32_t baseId) {
    const auto rec = p.GetEspm().GetBrowser().LookupById(baseId).rec;
    return rec && rec->GetType() == "ARMO";
  };
  std::vector<uint32_t> outfitBefore;
  for (const auto& entry : actor.GetEquipment().inv.entries) {
    if (entry.GetWorn() != Inventory::Worn::None && isArmor(entry.baseId)) {
      outfitBefore.push_back(entry.baseId);
    }
  }
  p.Messages().clear();
  Wear(p, 0, kActor, kIronSword, 1);
  REQUIRE(actor.GetEquipment().inv.HasItem(kIronSword));
  for (const auto baseId : outfitBefore) {
    REQUIRE(actor.GetEquipment().inv.HasItem(baseId));
  }
  REQUIRE(actor.GetEquipment().inv.entries.size() == 1 + outfitBefore.size());
  REQUIRE(actor.GetEquipment().numChanges == 1);
  REQUIRE(c1.GetEquipment().inv.entries.size() == c1Before.inv.entries.size());
  REQUIRE(relays(1) == 1);
  REQUIRE(relays(0) == 0);

  // the other game's report about the same actor: dropped at the gate
  p.Messages().clear();
  Wear(p, 1, kActor, kIronSword, 2);
  REQUIRE(actor.GetEquipment().numChanges == 1);
  REQUIRE(relays(0) == 0);
  REQUIRE(relays(1) == 0);

  // a WEAPON the actor's record lacks: the engine's own roll in its hands
  // (fate-dress, finding 43: the late fate path leaves the engine's rolled
  // weapon beside the server's), recorded R2 into the record's inventory and
  // hands, relayed once, and the host is sent neither an inventory (it has
  // no idx: the player's own would be replaced) nor a snippet
  p.Messages().clear();
  Wear(p, 0, kActor, kElsewhere, 3);
  p.Tick();
  REQUIRE(actor.GetEquipment().inv.HasItem(kElsewhere));
  REQUIRE(actor.GetInventory().HasItem(kElsewhere));
  REQUIRE(actor.GetEquipment().numChanges == 3);
  REQUIRE(relays(1) == 1);
  REQUIRE(sentOfType(0, MsgType::SetInventory) == 0);
  REQUIRE(sentOfType(0, MsgType::SpSnippet) == 0);

  // an ARMOR the record lacks (a helmet the host's engine put on, a skin a
  // script equipped): recorded R2 like the weapon, into the record's
  // inventory and worn set, relayed once, nothing sent to the host; the
  // steel sword the host's last report wore and this one does not comes off
  // the record's worn set and stays in its inventory; the roll's iron
  // helmet, whose slots the steel one takes, comes off too, and the rest of
  // the roll's outfit stays (no report of this host wore it yet)
  const auto shareSlots = [&](uint32_t a, uint32_t b) {
    if (!isArmor(a) || !isArmor(b)) {
      return false;
    }
    const auto slots = [&](uint32_t id) {
      const auto data = espm::GetData<espm::ARMO>(id, &p.worldState);
      return data.bod2.present ? data.bod2.bodyPartFlags
                               : data.bodt.bodyPartFlags;
    };
    return (slots(a) & slots(b)) != 0;
  };
  std::vector<uint32_t> outfitLeft;
  for (const auto baseId : outfitBefore) {
    if (!shareSlots(baseId, kSteelHelmet)) {
      outfitLeft.push_back(baseId);
    }
  }
  p.Messages().clear();
  Wear(p, 0, kActor, kSteelHelmet, 4);
  p.Tick();
  REQUIRE(actor.GetEquipment().inv.HasItem(kSteelHelmet));
  REQUIRE(actor.GetInventory().HasItem(kSteelHelmet));
  REQUIRE_FALSE(actor.GetEquipment().inv.HasItem(kElsewhere));
  REQUIRE(actor.GetInventory().HasItem(kElsewhere));
  for (const auto baseId : outfitBefore) {
    REQUIRE(actor.GetEquipment().inv.HasItem(baseId) ==
            !shareSlots(baseId, kSteelHelmet));
  }
  REQUIRE(actor.GetEquipment().numChanges == 4);
  REQUIRE(relays(1) == 1);
  REQUIRE(sentOfType(0, MsgType::SetInventory) == 0);
  REQUIRE(sentOfType(0, MsgType::SpSnippet) == 0);

  // a LIGHT the record lacks, lit by the host's AI (Skyrim.esm Torch01
  // 0x1d4ec; thuum docs/verbs/npc-fate.md, Eli's word of 2026-11-04 after
  // oracle v4 190713: the AI's torch is an equip every seat shows, the hands
  // are the host's): recorded R2 like the engine's own weapon, relayed once;
  // the steel helmet the last report wore comes off the worn set
  p.Messages().clear();
  Wear(p, 0, kActor, kTorch, 5);
  p.Tick();
  REQUIRE(actor.GetEquipment().inv.HasItem(kTorch));
  REQUIRE(actor.GetInventory().HasItem(kTorch));
  REQUIRE_FALSE(actor.GetEquipment().inv.HasItem(kSteelHelmet));
  REQUIRE(actor.GetInventory().HasItem(kSteelHelmet));
  for (const auto baseId : outfitLeft) {
    REQUIRE(actor.GetEquipment().inv.HasItem(baseId));
  }
  REQUIRE(actor.GetEquipment().numChanges == 5);
  REQUIRE(relays(1) == 1);
  REQUIRE(sentOfType(0, MsgType::SetInventory) == 0);

  // the host's next report without it (the AI put the torch away): the
  // torch leaves the record's worn set and stays in its inventory, the
  // sword is worn again, and the roll's outfit no report of this host wore
  // stays
  p.Messages().clear();
  Wear(p, 0, kActor, kIronSword, 6);
  p.Tick();
  REQUIRE_FALSE(actor.GetEquipment().inv.HasItem(kTorch));
  REQUIRE(actor.GetInventory().HasItem(kTorch));
  REQUIRE(actor.GetEquipment().inv.HasItem(kIronSword));
  for (const auto baseId : outfitLeft) {
    REQUIRE(actor.GetEquipment().inv.HasItem(baseId));
  }
  REQUIRE(actor.GetEquipment().numChanges == 6);
  REQUIRE(relays(1) == 1);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kActor);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

// `user`'s spoken line for `actorId` (thuum docs/verbs/npc-stream.md, the
// voice slice)
void Speak(PartOne& p, Networking::UserId user, uint32_t actorId)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  ActorSpeechMessage msg;
  msg.idx = p.worldState.GetFormAt<MpActor>(actorId).GetIdx();
  msg.topic = 0x20000;
  msg.info = 0x20001;
  p.GetActionListener().OnActorSpeech(raw, msg);
}

TEST_CASE("A hosted actor's spoken line travels from its host alone to the "
          "other game and never back; the other game's and the player's own "
          "idx are dropped",
          "[CellHost][Speech]")
{
  // thuum docs/verbs/npc-stream.md (the voice slice): the host's engine
  // speaks the line, the hook reports it, the puppets say the same info
  auto& p = GetPartOne();
  constexpr uint32_t kActor = 0xff000c09;
  const NiPoint3 kAt{ 117786.f, -81400.f, 10997.f };
  Player(p, 0, kC1, kAt + NiPoint3{ 40.f, 0.f, 0.f });
  Player(p, 1, kC2, kAt + NiPoint3{ 60.f, 0.f, 0.f });
  p.CreateActor(kActor, kAt + NiPoint3{ 30.f, 0.f, 0.f }, 0, kTamriel);
  auto& actor = p.worldState.GetFormAt<MpActor>(kActor);
  actor.SetPos(actor.GetPos());
  const auto cell = p.worldState.CellOf(actor);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  REQUIRE(p.worldState.HostOf(actor) == &p.worldState.GetFormAt<MpActor>(kC1));
  const auto relays = [&](Networking::UserId user) {
    size_t n = 0;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() && m.j.empty()) {
        ++n;
      }
    }
    return n;
  };

  p.Messages().clear();
  Speak(p, 0, kActor);
  REQUIRE(relays(1) == 1);
  REQUIRE(relays(0) == 0);

  p.Messages().clear();
  Speak(p, 1, kActor);
  REQUIRE(relays(0) == 0);
  REQUIRE(relays(1) == 0);

  p.Messages().clear();
  Speak(p, 0, kC1);
  REQUIRE(relays(0) == 0);
  REQUIRE(relays(1) == 0);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kActor);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

TEST_CASE("A hosted actor's furniture use is its host's engine's: the server "
          "answers it no OpenContainer, the reply that performs a player's "
          "own activation in its game",
          "[CellHost][Activate]")
{
  // playtest 32 (2026-10-22): a resident sat on Riverwood's bench 2ad10 by
  // package in the host's game; the host's client reported the activation
  // as the resident's (cell-host's hosted caster), the server answered
  // OpenContainer through GetActorToSendTo, which hands an offline actor's
  // messages to its host, and the host's game activated the bench with its
  // own player: rotfern "sat down on a bench all on her own". The reply is
  // the activating player's alone.
  auto& p = GetPartOne();
  // unit/RestTest.cpp's bed, a FURN the server loads, in Tamriel
  constexpr uint32_t kBed = 0x000B3184;
  constexpr uint32_t kSitter = 0xff000c05;
  const NiPoint3 kAtBed{ 117786.f, -81400.f, 10997.f };
  Player(p, 0, kC1, kAtBed + NiPoint3{ 40.f, 0.f, 0.f });
  p.CreateActor(kSitter, kAtBed + NiPoint3{ 30.f, 0.f, 0.f }, 0, kTamriel);
  auto& sitter = p.worldState.GetFormAt<MpActor>(kSitter);
  sitter.SetPos(sitter.GetPos());
  const auto cell = p.worldState.CellOf(sitter);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  REQUIRE(p.worldState.HostOf(sitter) ==
          &p.worldState.GetFormAt<MpActor>(kC1));
  p.Messages().clear();

  // the host's report of its actor's activation: taken, never answered with
  // the player's activation
  DoMessage(p, 0,
            nlohmann::json{ { "t", MsgType::Activate },
                            { "data",
                              { { "caster", kSitter },
                                { "target", kBed },
                                { "isSecondActivation", false } } } });
  REQUIRE(SentTo<OpenContainerMessage>(p, 0).empty());

  // the player's own activation is answered as before
  p.Messages().clear();
  DoMessage(p, 0,
            nlohmann::json{ { "t", MsgType::Activate },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", kBed },
                                { "isSecondActivation", false } } } });
  REQUIRE(SentTo<OpenContainerMessage>(p, 0).size() == 1);

  DoDisconnect(p, 0);
  p.DestroyActor(kSitter);
  p.DestroyActor(kC1);
}

// the host's report of its NPC's activation leaves the NPC's equipment as
// its host's own report recorded it. An EquipBestWeapon on the activation
// would strip the worn weapon and wear the inventory's best in the right hand
// alone with numChanges one up, so each activation (a draugr rising from its
// alcove gives four) would send every copy a record its host never made
TEST_CASE("A hosted actor's activation leaves its equipment record as its "
          "host reported it",
          "[CellHost][Equipment][espm]")
{
  auto& p = GetPartOne();
  // the bench case's bed, a FURN the server loads, in Tamriel
  constexpr uint32_t kBed = 0x000B3184;
  constexpr uint32_t kSitter = 0xff000c05;
  // Skyrim.esm DraugrGreatsword (WEAP, ETYP BothHands 0x13f45) and
  // IronSword (the sword is this file's kIronSword elsewhere)
  constexpr uint32_t kGreatsword = 0x000236a5;
  constexpr uint32_t kSword = 0x00012eb7;
  const NiPoint3 kAtBed{ 117786.f, -81400.f, 10997.f };
  Player(p, 0, kC1, kAtBed + NiPoint3{ 40.f, 0.f, 0.f });
  p.CreateActor(kSitter, kAtBed + NiPoint3{ 30.f, 0.f, 0.f }, 0, kTamriel);
  auto& sitter = p.worldState.GetFormAt<MpActor>(kSitter);
  sitter.SetPos(sitter.GetPos());
  const auto cell = p.worldState.CellOf(sitter);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  REQUIRE(p.worldState.HostOf(sitter) ==
          &p.worldState.GetFormAt<MpActor>(kC1));

  // the host's report as its HostedStateService makes it: the greatsword,
  // read in both hands, one entry worn and wornLeft; a sword it also carries
  sitter.AddItem(kGreatsword, 1);
  sitter.AddItem(kSword, 1);
  Equipment reported;
  Inventory::Entry worn;
  worn.baseId = kGreatsword;
  worn.count = 1;
  worn.SetWorn(Inventory::Worn::Right);
  worn.wornLeft = true;
  reported.inv.AddItems({ worn });
  reported.numChanges = 6;
  sitter.SetEquipment(reported);
  const Equipment before = sitter.GetEquipment();
  REQUIRE(before.numChanges == 6);

  // four activations of the furniture, as a draugr's alcove gives them
  for (int i = 0; i < 4; ++i) {
    DoMessage(p, 0,
              nlohmann::json{ { "t", MsgType::Activate },
                              { "data",
                                { { "caster", kSitter },
                                  { "target", kBed },
                                  { "isSecondActivation", false } } } });
  }
  REQUIRE(sitter.GetEquipment() == before);
  REQUIRE(sitter.GetEquipment().numChanges == 6);

  DoDisconnect(p, 0);
  p.DestroyActor(kSitter);
  p.DestroyActor(kC1);
}

// a host reports a two-hander it holds as one entry worn and wornLeft (its
// engine reads it in both hands), which Inventory::Entry::GetWorn reads as
// Left; the server's right-hand reads take it by its record's ETYP
// (MpActor::WornHand: GetEquippedWeapon, which ActionListener's
// RightHandWeapon and GetEquippedItemType read), and a real left-hand weapon
// stays the left hand's
TEST_CASE("A host-reported two-hander is the right hand's weapon to the "
          "server's reads, and a left-hand one-hander stays left",
          "[CellHost][Equipment][espm]")
{
  auto& p = GetPartOne();
  constexpr uint32_t kHolder = 0xff000c05;
  // Skyrim.esm WEAP DraugrGreatsword (ETYP BothHands 0x13f45) and
  // IronDagger (FavoritesTest's kIronDagger)
  constexpr uint32_t kGreatsword = 0x000236a5;
  constexpr uint32_t kDagger = 0x0001397e;
  const NiPoint3 kAt{ 117786.f, -81400.f, 10997.f };
  p.CreateActor(kHolder, kAt, 0, kTamriel);
  auto& holder = p.worldState.GetFormAt<MpActor>(kHolder);
  holder.AddItem(kGreatsword, 1);
  holder.AddItem(kDagger, 1);

  // the host's report: the greatsword in both hands
  Equipment twoHanded;
  Inventory::Entry both;
  both.baseId = kGreatsword;
  both.count = 1;
  both.SetWorn(Inventory::Worn::Right);
  both.wornLeft = true;
  REQUIRE(both.GetWorn() == Inventory::Worn::Left);
  twoHanded.inv.AddItems({ both });
  holder.SetEquipment(twoHanded);
  REQUIRE(MpActor::WornHand(p.worldState, both) == Inventory::Worn::Right);
  auto weapons = holder.GetEquippedWeapon();
  REQUIRE(weapons[1].has_value());
  REQUIRE(weapons[1]->baseId == kGreatsword);
  REQUIRE_FALSE(weapons[0].has_value());
  // ActionListener's RightHandWeapon (the drawn-weapon stagger value's)
  // reads this right hand
  const auto right =
    espm::GetData<espm::WEAP>(weapons[1]->baseId, &p.worldState);
  REQUIRE(right.weapDNAM);
  REQUIRE(right.weapDNAM->animType == espm::WEAP::AnimType::TwoHandSword);

  // a dagger in the left hand alone: the left hand's, no right-hand weapon
  Equipment leftHand;
  Inventory::Entry left;
  left.baseId = kDagger;
  left.count = 1;
  left.SetWorn(Inventory::Worn::Left);
  leftHand.inv.AddItems({ left });
  holder.SetEquipment(leftHand);
  REQUIRE(MpActor::WornHand(p.worldState, left) == Inventory::Worn::Left);
  weapons = holder.GetEquippedWeapon();
  REQUIRE(weapons[0].has_value());
  REQUIRE(weapons[0]->baseId == kDagger);
  REQUIRE_FALSE(weapons[1].has_value());

  p.DestroyActor(kHolder);
}

// a host's report of a two-hander takes the place of what the record wore in
// either hand, the fate's right-hand pick, a left-hand piece and an outfit
// shield flagged worn alike, so no stale one-hander stays worn beside it for
// GetEquippedWeapon's right hand and no shield for a copy to dress over it
TEST_CASE("A host's report of a two-hander takes the place of the record's "
          "one-hander, its left-hand piece and its outfit shield",
          "[CellHost][HostedEquipment][espm]")
{
  auto& p = GetPartOne();
  constexpr uint32_t kHolder = 0xff000c05;
  // Skyrim.esm WEAP DraugrGreatsword (ETYP BothHands 0x13f45), IronSword
  // and IronDagger (this file's kIronSword and FavoritesTest's kIronDagger)
  constexpr uint32_t kGreatsword = 0x000236a5;
  constexpr uint32_t kSword = 0x00012eb7;
  constexpr uint32_t kDagger = 0x0001397e;
  // Skyrim.esm ARMO ArmorIronShield, BOD2 0x200 the shield slot
  constexpr uint32_t kShield = 0x00012eb6;
  const NiPoint3 kAtBed{ 117786.f, -81400.f, 10997.f };
  Player(p, 0, kC1, kAtBed + NiPoint3{ 40.f, 0.f, 0.f });
  p.CreateActor(kHolder, kAtBed + NiPoint3{ 30.f, 0.f, 0.f }, 0, kTamriel);
  auto& holder = p.worldState.GetFormAt<MpActor>(kHolder);
  holder.SetPos(holder.GetPos());
  const auto cell = p.worldState.CellOf(holder);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  REQUIRE(p.worldState.HostOf(holder) ==
          &p.worldState.GetFormAt<MpActor>(kC1));
  holder.AddItem(kGreatsword, 1);
  holder.AddItem(kSword, 1);
  holder.AddItem(kDagger, 1);
  holder.AddItem(kShield, 1);

  // the record before any report of this host's: the sword in the right
  // hand (the fate's pick), the dagger in the left, the outfit's shield
  // flagged worn as the outfit roll flags every piece
  Equipment recorded;
  Inventory::Entry sword;
  sword.baseId = kSword;
  sword.count = 1;
  sword.SetWorn(Inventory::Worn::Right);
  Inventory::Entry dagger;
  dagger.baseId = kDagger;
  dagger.count = 1;
  dagger.SetWorn(Inventory::Worn::Left);
  Inventory::Entry shield;
  shield.baseId = kShield;
  shield.count = 1;
  shield.SetWorn(Inventory::Worn::Right);
  recorded.inv.AddItems({ sword, dagger, shield });
  holder.SetEquipment(recorded);

  // the host's report: the greatsword in both hands, one entry worn and
  // wornLeft (its HostedStateService's shape)
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = 0;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateEquipmentMessage msg;
  msg.idx = holder.GetIdx();
  msg.data.numChanges = 1;
  Inventory::Entry both;
  both.baseId = kGreatsword;
  both.count = 1;
  both.SetWorn(Inventory::Worn::Right);
  both.wornLeft = true;
  msg.data.inv.entries.push_back(both);
  p.GetActionListener().OnUpdateEquipment(raw, msg);

  const auto weapons = holder.GetEquippedWeapon();
  REQUIRE(weapons[1].has_value());
  REQUIRE(weapons[1]->baseId == kGreatsword);
  REQUIRE_FALSE(weapons[0].has_value());
  for (const auto& entry : holder.GetEquipment().inv.entries) {
    if (entry.baseId == kSword || entry.baseId == kDagger ||
        entry.baseId == kShield) {
      REQUIRE(entry.GetWorn() == Inventory::Worn::None);
    }
  }

  DoDisconnect(p, 0);
  p.DestroyActor(kHolder);
  p.DestroyActor(kC1);
}

// thuum docs/verbs/npc-talk.md (ADR-052): a player's claim on an NPC another
// game simulates, for a dialogue
TEST_CASE("A guest's claim on a hosted NPC names the guest as the simulator "
          "on every seat, a far or unloaded claimant is refused, and the "
          "release names the host again",
          "[CellHost][Talk][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  // kCow, the game-file actor the host cases use, at its own place in its
  // own world (loaded by its id as the puppet case does; a Tamriel point put
  // the cow in no cell of Whiterun's world, pipeline 3248)
  MpActor* actor = &p.worldState.GetFormAt<MpActor>(kCow);
  REQUIRE(actor);
  const NiPoint3 kAt = actor->GetPos();
  const auto cell = p.worldState.CellOf(*actor);
  REQUIRE(cell.has_value());
  // two players beside the cow in its world, the claim's reach rule reading
  // the same world for both
  DoConnect(p, 0);
  p.CreateActor(kC1, kAt + NiPoint3{ 40.f, 0.f, 0.f }, 0, kWhiterun);
  p.SetUserActor(0, kC1);
  DoConnect(p, 1);
  p.CreateActor(kC2, kAt + NiPoint3{ 80.f, 0.f, 0.f }, 0, kWhiterun);
  p.SetUserActor(1, kC2);
  auto& c1 = p.worldState.GetFormAt<MpActor>(kC1);
  auto& c2 = p.worldState.GetFormAt<MpActor>(kC2);
  CellLoaded(p, 0, *cell);
  CellLoaded(p, 1, *cell);
  p.TickCellHosts(static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count()));
  REQUIRE(p.worldState.HostOf(*actor) == &c1);
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  const auto claim = [&](Networking::UserId user, bool want) {
    RawMessageData raw;
    raw.userId = user;
    raw.unparsed = unparsed;
    raw.unparsedLength = sizeof(unparsed);
    ActorClaimMessage msg;
    msg.idx = actor->GetIdx();
    msg.claim = want;
    p.GetActionListener().OnActorClaim(raw, msg);
  };

  // c2 claims: the word names c2 on both seats, c2 simulates
  p.Messages().clear();
  claim(1, true);
  REQUIRE(p.SimulatorOf(*actor) == kC2);
  REQUIRE(p.worldState.HostOf(*actor) == &c2);
  REQUIRE(LastHostedWord(p, 1, *actor) == "false");
  REQUIRE(LastHostedWord(p, 0, *actor) == "true");
  // c1's reports are refused, c2's pass the gate: a hosted hands report
  // under the actor's idx (the equipment case's shape; the gate itself is
  // the listener's own)
  constexpr uint32_t kIronSword = 0x12eb7; // the equipment case's
  actor->AddItem(kIronSword, 1);
  Wear(p, 0, actor->GetFormId(), kIronSword, 1);
  REQUIRE_FALSE(actor->GetEquipment().inv.HasItem(kIronSword));
  Wear(p, 1, actor->GetFormId(), kIronSword, 1);
  REQUIRE(actor->GetEquipment().inv.HasItem(kIronSword));
  actor->SetEquipment(Equipment{});
  actor->RemoveItem(kIronSword, 1, nullptr);

  // the release: the host again on both seats
  p.Messages().clear();
  claim(1, false);
  REQUIRE(p.SimulatorOf(*actor) == kC1);
  REQUIRE(LastHostedWord(p, 0, *actor) == "false");
  REQUIRE(LastHostedWord(p, 1, *actor) == "true");

  // a claimant far from the record is refused, nothing moves
  c2.SetPos(kAt + NiPoint3{ 4000.f, 0.f, 0.f });
  p.Messages().clear();
  claim(1, true);
  REQUIRE(p.SimulatorOf(*actor) == kC1);
  REQUIRE(PropertiesSentTo(p, 1).empty());
  c2.SetPos(kAt + NiPoint3{ 80.f, 0.f, 0.f });

  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

// thuum ADR-073 step 5a (decision 6, D-53-4; R-C2, playtest 52): the game
// running a quest's own package on the quest's alias actor claims her for
// the quest's lead, and a scene's starter its actors. Skyrim.esm as read on
// sky-srv 2026-11-14: Camilla Valerius 0x13488 (her NPC_ 0x1347b a unique
// actor) in RiverwoodTrader 0x133c9 fills MS13 0x39645's alias 9 by that
// unique actor; MS13CamillaTravelMarker00Package 0x2e4cc's QNAM is MS13,
// MS13CamillaWorkTrader 0x63b2a has none, XMarker 0x3b is no quest;
// MS13LucanCamillaScene 0x38386's scene 0x3839b has Lucan 0x13487 (its
// alias 0, his NPC_ 0x1347a) among its actors, and the ritual 0x5dba7 is
// VC01's. Read the same way 2026-11-14, for the refusals the fresh review of
// 4bf1e21c found untested (NIT 6): 0x38386's aliases 0 and 2 are the unique
// actors Lucan 0x1347a and Camilla 0x1347b, and Hod 0x1348a's NPC_ is
// 0x1347d, neither; four more quests whose own package names them (QNAM),
// each with a reference alias by no forced reference or unique actor:
// CRTwinsPostQuest 0x10fe25 (CRTwinsPostQuestHangOut 0x10fe26), CR12PostQuest
// 0x10b10f (CR12PostQuestAelaPray 0x10b110), DialogueGenericScene04 0x1b112
// (DGS04OgleHotLadies 0x1c56d) and DA07intro 0x94d89
// (DA07SerenTraveltoArguePackage 0x94d8d), none of the eight records
// overridden by Update.esm, the three DLC masters or USSEP
TEST_CASE("A host's lead on a quest's alias actor is its claim, outlives a "
          "grant to another seat and ends at the alias's refill and the "
          "actor's death; a scene's starter claims its actor until the "
          "quest stops; the wrong seat, package, quest or scene is refused",
          "[CellHost][Talk][espm]")
{
  constexpr uint32_t kCamilla = 0x13488;
  constexpr uint32_t kLucan = 0x13487;
  constexpr uint32_t kTrader = 0x133c9;
  constexpr uint32_t kMs13 = 0x39645;
  constexpr uint32_t kTravel = 0x2e4cc;
  constexpr uint32_t kWorkTrader = 0x63b2a;
  constexpr uint32_t kArgueQuest = 0x38386;
  constexpr uint32_t kArgument = 0x3839b;
  constexpr uint32_t kRitual = 0x5dba7;
  constexpr uint32_t kHod = 0x1348a;
  constexpr uint32_t kTwinsPost = 0x10fe25;
  constexpr uint32_t kTwinsHangOut = 0x10fe26;
  constexpr uint32_t kAelaPost = 0x10b10f;
  constexpr uint32_t kAelaPray = 0x10b110;
  constexpr uint32_t kOgle = 0x1b112;
  constexpr uint32_t kOgleLadies = 0x1c56d;
  constexpr uint32_t kSilusIntro = 0x94d89;
  constexpr uint32_t kSerenTravel = 0x94d8d;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.IsInteriorCell(kTrader));
  auto& camillaForm = p.worldState.LookupFormById(kCamilla);
  REQUIRE(camillaForm);
  MpActor* actor = camillaForm->AsActor();
  REQUIRE(actor);
  auto& lucanForm = p.worldState.LookupFormById(kLucan);
  REQUIRE(lucanForm);
  MpActor* lucan = lucanForm->AsActor();
  REQUIRE(lucan);
  REQUIRE(p.worldState.CellOf(*actor) == kTrader);
  REQUIRE(p.worldState.CellOf(*lucan) == kTrader);
  const NiPoint3 kAt = actor->GetPos();
  DoConnect(p, 0);
  p.CreateActor(kC1, kAt + NiPoint3{ 40.f, 0.f, 0.f }, 0, kTrader);
  p.SetUserActor(0, kC1);
  DoConnect(p, 1);
  p.CreateActor(kC2, kAt + NiPoint3{ 80.f, 0.f, 0.f }, 0, kTrader);
  p.SetUserActor(1, kC2);
  auto& c1 = p.worldState.GetFormAt<MpActor>(kC1);
  CellLoaded(p, 0, kTrader);
  CellLoaded(p, 1, kTrader);
  p.TickCellHosts(NowMs());
  REQUIRE(p.GetCellHost(kTrader).player == kC1);
  REQUIRE(p.worldState.HostOf(*actor) == &c1);

  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  const auto claim = [&](Networking::UserId user, MpActor& who,
                         uint32_t purpose, uint32_t quest, uint32_t form) {
    RawMessageData raw;
    raw.userId = user;
    raw.unparsed = unparsed;
    raw.unparsedLength = sizeof(unparsed);
    ActorClaimMessage msg;
    msg.idx = who.GetIdx();
    msg.claim = true;
    msg.purpose = purpose;
    msg.quest = quest;
    if (purpose == 1) {
      msg.package = form;
    } else {
      msg.scene = form;
    }
    p.GetActionListener().OnActorClaim(raw, msg);
  };
  // the quests a claim row's leads hold, and its scene
  const auto leadsOf = [&](uint32_t id) -> std::vector<uint32_t> {
    for (const auto& row : p.GetFollows()) {
      if (row.actor == id) {
        return row.leads;
      }
    }
    return {};
  };
  const auto sceneOf = [&](uint32_t id) -> uint32_t {
    for (const auto& row : p.GetFollows()) {
      if (row.actor == id) {
        return row.scene;
      }
    }
    return 0;
  };
  // whether any row (a follow, or a claim of any purpose) holds the actor
  const auto rowed = [&](uint32_t id) {
    for (const auto& row : p.GetFollows()) {
      if (row.actor == id) {
        return true;
      }
    }
    return false;
  };

  // another seat's lead (c1's game runs her: E_CLAIM_NOT_SIMULATOR), a
  // package MS13 does not own (E_CLAIM_PACKAGE), a form that is no quest
  // (E_CLAIM_QUEST), a purpose the contract names none of (E_CLAIM_PURPOSE,
  // past the validator): no row
  claim(1, *actor, 1, kMs13, kTravel);
  claim(0, *actor, 1, kMs13, kWorkTrader);
  claim(0, *actor, 1, 0x3b, kTravel);
  claim(0, *actor, 3, kMs13, kTravel);
  REQUIRE(leadsOf(kCamilla).empty());
  REQUIRE_FALSE(rowed(kCamilla));
  // the host's own lead from its travel package: its claim, and no word,
  // since its game already runs her
  p.Messages().clear();
  claim(0, *actor, 1, kMs13, kTravel);
  REQUIRE(leadsOf(kCamilla) == std::vector<uint32_t>{ kMs13 });
  REQUIRE(p.SimulatorOf(*actor) == kC1);
  REQUIRE(LastHostedWord(p, 0, *actor).empty());
  REQUIRE(LastHostedWord(p, 1, *actor).empty());

  // c1 unloads the trader and the grace passes it to c2 (R-C2's grant): the
  // lead outlives it, c1 still simulates her and both seats are told so (the
  // server's half: c1's own why-2 release, D-53-5 B, would end it, and this
  // case sends none)
  const auto now = NowMs();
  CellLoaded(p, 0, kTrader, false);
  p.TickCellHosts(now + skymp::rules::host_grace_ms() + kPastMs);
  REQUIRE(p.GetCellHost(kTrader).player == kC2);
  REQUIRE(leadsOf(kCamilla) == std::vector<uint32_t>{ kMs13 });
  REQUIRE(p.SimulatorOf(*actor) == kC1);
  REQUIRE(LastHostedWord(p, 0, *actor) == "false");
  REQUIRE(LastHostedWord(p, 1, *actor) == "true");

  // MS13's alias 9 refilled with Lucan (a seat's ForceRefTo): the quest
  // released her, the claim ends and the cell's host simulates her
  {
    AliasFillMessage fill;
    fill.quest = kMs13;
    fill.alias = 9;
    fill.refr = kLucan;
    fill.served = false;
    RawMessageData raw;
    raw.userId = 1;
    p.GetActionListener().OnAliasFill(raw, fill);
  }
  REQUIRE(leadsOf(kCamilla).empty());
  REQUIRE(p.SimulatorOf(*actor) == kC2);
  REQUIRE(LastHostedWord(p, 1, *actor) == "false");
  REQUIRE(LastHostedWord(p, 0, *actor) == "true");

  // the argument scene, started by c1 in the trader c2 hosts: before c1
  // loads the trader again it is refused (E_CLAIM_UNLOADED: a scene is its
  // cell's visitors'); a scene of another quest is refused (E_CLAIM_SCENE),
  // and so is an actor none of its actor aliases can hold (Hod:
  // E_CLAIM_ALIAS); the argument's takes Lucan to c1 until its quest stops
  claim(0, *lucan, 2, kArgueQuest, kArgument);
  REQUIRE(sceneOf(kLucan) == 0);
  REQUIRE(p.SimulatorOf(*lucan) == kC2);
  CellLoaded(p, 0, kTrader);
  claim(0, *lucan, 2, kArgueQuest, kRitual);
  REQUIRE(sceneOf(kLucan) == 0);
  REQUIRE(p.SimulatorOf(*lucan) == kC2);
  auto& hodForm = p.worldState.LookupFormById(kHod);
  REQUIRE(hodForm);
  MpActor* hod = hodForm->AsActor();
  REQUIRE(hod);
  claim(0, *hod, 2, kArgueQuest, kArgument);
  REQUIRE_FALSE(rowed(kHod));
  claim(0, *lucan, 2, kArgueQuest, kArgument);
  REQUIRE(sceneOf(kLucan) == kArgument);
  REQUIRE(p.SimulatorOf(*lucan) == kC1);
  REQUIRE(LastHostedWord(p, 0, *lucan) == "false");
  REQUIRE(LastHostedWord(p, 1, *lucan) == "true");
  p.QuestStopped(kArgueQuest);
  REQUIRE(sceneOf(kLucan) == 0);
  REQUIRE(p.SimulatorOf(*lucan) == kC2);
  REQUIRE(LastHostedWord(p, 1, *lucan) == "false");

  // the host's own lead again (alias 9 is Lucan's now, so the claim rides
  // MS13's aliases the server cannot read: unvalidated, R2), then her death
  // ends it
  claim(1, *actor, 1, kMs13, kTravel);
  REQUIRE(leadsOf(kCamilla) == std::vector<uint32_t>{ kMs13 });
  // three more quests' own packages fill her row's MAX_LEADS (each rides an
  // alias the server cannot read: unvalidated, R2); a fifth quest's is
  // refused (E_CLAIM_FULL) and nothing held is dropped
  claim(1, *actor, 1, kTwinsPost, kTwinsHangOut);
  claim(1, *actor, 1, kAelaPost, kAelaPray);
  claim(1, *actor, 1, kOgle, kOgleLadies);
  claim(1, *actor, 1, kSilusIntro, kSerenTravel);
  REQUIRE(leadsOf(kCamilla) ==
          std::vector<uint32_t>{ kMs13, kTwinsPost, kAelaPost, kOgle });
  // D-53-5 B: a lead's release saying its game stopped running her ends
  // that lead as a release does, the why the holder's own report (R2); a
  // why past 2, or on a talk's release, is refused (E_CLAIM_WHY, past the
  // validator) and ends nothing; Ogle's package left (why 1) ends its lead
  // alone, and the host still runs her
  const auto release = [&](Networking::UserId user, MpActor& who,
                           uint32_t purpose, uint32_t quest, uint32_t package,
                           uint32_t why) {
    RawMessageData raw;
    raw.userId = user;
    raw.unparsed = unparsed;
    raw.unparsedLength = sizeof(unparsed);
    ActorClaimMessage msg;
    msg.idx = who.GetIdx();
    msg.claim = false;
    msg.purpose = purpose;
    if (purpose == 1) {
      msg.quest = quest;
      msg.package = package;
    }
    msg.why = why;
    p.GetActionListener().OnActorClaim(raw, msg);
  };
  release(1, *actor, 1, kOgle, kOgleLadies, 3);
  release(1, *actor, 0, 0, 0, 2);
  REQUIRE(leadsOf(kCamilla) ==
          std::vector<uint32_t>{ kMs13, kTwinsPost, kAelaPost, kOgle });
  release(1, *actor, 1, kOgle, kOgleLadies, 1);
  REQUIRE(leadsOf(kCamilla) ==
          std::vector<uint32_t>{ kMs13, kTwinsPost, kAelaPost });
  REQUIRE(p.SimulatorOf(*actor) == kC2);
  actor->Kill(nullptr);
  REQUIRE(leadsOf(kCamilla).empty());

  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kHod);
}

// thuum docs/research/m3/decisions-after-playtest-34.md D18 (b-cell-host
// 20261025-115250: c1 took a cell at epoch 2 while dead): a dead player
// hosts nothing. Its death is the server's own number: the cells it hosts
// start their grace there, its own loaded report grants it nothing while it
// lies, and it is a candidate again when it stands.
TEST_CASE("A dead player hosts nothing: the grace starts at its death, its "
          "loaded report grants nothing, and it is a candidate again when it "
          "stands",
          "[CellHost][espm]")
{
  auto& p = GetPartOne();
  auto& c1 = Player(p, 0, kC1, kHere);
  Player(p, 1, kC2, kHere);
  const auto cell = p.worldState.CellOf(c1);
  REQUIRE(cell.has_value());
  const uint64_t t0 = NowMs();
  p.TickCellHosts(t0);
  CellLoaded(p, 0, *cell);
  CellLoaded(p, 1, *cell);
  p.TickCellHosts(t0 + 10);
  REQUIRE(p.GetCellHost(*cell).hosted);
  REQUIRE(p.GetCellHost(*cell).player == kC1);

  // the death arms the grace; the live visitor takes the cell at its end
  const uint64_t tDeath = NowMs();
  c1.Kill(nullptr);
  REQUIRE(c1.IsDead());
  p.TickCellHosts(tDeath + skymp::rules::host_grace_ms() - kInsideMs);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  p.TickCellHosts(tDeath + skymp::rules::host_grace_ms() + kPastMs);
  REQUIRE(p.GetCellHost(*cell).hosted);
  REQUIRE(p.GetCellHost(*cell).player == kC2);

  // the visitor leaves; the dead player, its cell still loaded, takes
  // nothing
  const uint64_t tLeave = NowMs();
  CellLoaded(p, 1, *cell, false);
  p.TickCellHosts(tLeave + skymp::rules::host_grace_ms() + kPastMs);
  REQUIRE_FALSE(p.GetCellHost(*cell).hosted);
  CellLoaded(p, 0, *cell);
  p.TickCellHosts(NowMs());
  REQUIRE_FALSE(p.GetCellHost(*cell).hosted);

  // it stands again: the free cell it has loaded is its
  c1.Respawn(false);
  REQUIRE_FALSE(c1.IsDead());
  REQUIRE(p.GetCellHost(*cell).hosted);
  REQUIRE(p.GetCellHost(*cell).player == kC1);

  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
  p.DestroyActor(kC2);
  p.DestroyActor(kC1);
}

// thuum docs/verbs/melee-reach.md (the weekend pass, M3's "NPC melee reach and
// hit cone"): a hosted humanoid NPC's melee hit, reported by its host, is
// validated by reach as a player's is (R1); beyond reach it is refused and
// the victim keeps its health; within reach it lands
TEST_CASE("A hosted resident's melee hit its host reports is gated by reach",
          "[CellHost][Hit][espm]")
{
  constexpr uint32_t kSigrid = 0x13483;
  const NiPoint3 sigridPos{ 24063.7f, -45204.2f, -40.6f };
  const NiPoint3 landing{ 17225.f, -47204.f, -128.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSigrid) == nullptr);
  Player(p, 0, kC1, landing);
  Player(p, 1, kC2, sigridPos + NiPoint3{ 0.f, 2000.f, 0.f });
  const auto cell = p.worldState.CellOf(kTamriel, sigridPos);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", MsgType::ActorSightings },
                    { "sightings",
                      nlohmann::json::array({ nlohmann::json{
                        { "refrId", kSigrid },
                        { "worldOrCell", kTamriel },
                        { "pos", { sigridPos.x, sigridPos.y, sigridPos.z } },
                        { "rot", { 0.f, 0.f, 0.f } } } }) } });
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kSigrid) != nullptr);
  auto& sigrid = p.worldState.GetFormAt<MpActor>(kSigrid);
  auto& victim = p.worldState.GetFormAt<MpActor>(kC2);
  // her hit names a weapon her record's hands hold (an unarmed 0 is dropped by
  // E_HIT_WEAPON before reach judges it, pipeline 3832's red): an iron sword
  // (reach 1.0), as HitTest arms its aggressor. Armed AFTER the victim's move:
  // the move sends the victim's new squares' fates and the roll re-dresses the
  // resident's record, which wiped the sword in pipeline 3849 ("absent in its
  // inventory")
  const uint32_t ironSword = 0x00012eb7;
  const auto arm = [&]() {
    if (!sigrid.GetInventory().HasItem(ironSword)) {
      sigrid.AddItem(ironSword, 1);
    }
    Equipment eq;
    eq.inv.entries.push_back(Inventory::Entry(ironSword, 1, kWornExtra));
    sigrid.SetEquipment(eq);
  };
  const auto hitFrom = [&](const NiPoint3& victimPos) {
    victim.SetPos(victimPos);
    arm();
    ActorValues full;
    full.healthPercentage = 1.f;
    full.magickaPercentage = 1.f;
    full.staminaPercentage = 1.f;
    victim.SetPercentages(full);
    sigrid.SetLastHitTime(
      kC2, std::chrono::steady_clock::now() - std::chrono::seconds(10));
    RawMessageData raw;
    raw.userId = 0;
    HitMessage hit;
    hit.data.aggressor = kSigrid;
    hit.data.target = kC2;
    hit.data.source = ironSword;
    p.GetActionListener().OnHit(raw, hit);
    return victim.GetChangeForm().actorValues.healthPercentage < 1.f;
  };
  // 2000 units north of her: beyond the sword's reach, refused (E_HIT_REACH)
  REQUIRE_FALSE(hitFrom(sigridPos + NiPoint3{ 0.f, 2000.f, 0.f }));
  // 60 units north, on her heading: lands
  REQUIRE(hitFrom(sigridPos + NiPoint3{ 0.f, 60.f, 0.f }));

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kSigrid);
}

TEST_CASE("A grant sends the cell's actors' records to the new host ahead of "
          "its hoster words (docs/engine/non-owning-seat.md section 3: the "
          "promote places the copy at the record, not where the engine had "
          "it; the grant bounce of 20261103-055217 and 060106)",
          "[CellHost]")
{
  auto& p = GetPartOne();
  Player(p, 0, kC1, kHere);
  const auto here = p.worldState.CellOf(kTamriel, kHere);
  REQUIRE(here.has_value());
  // the wolf stands in a cell nobody hosts yet, frozen at its record
  p.CreateActor(kWolf, kHere + NiPoint3{ 300.f, 0.f, 0.f }, 0, kTamriel);
  auto& wolf = p.worldState.GetFormAt<MpActor>(kWolf);
  wolf.SetPos(kHere + NiPoint3{ 300.f, 0.f, 0.f });
  REQUIRE(p.SimulatorOf(wolf) == 0);
  p.Messages().clear();

  // c1's load grants it the cell: for the wolf, its record as a movement
  // message to c1 first, reliable, then the word
  CellLoaded(p, 0, *here);
  REQUIRE(p.SimulatorOf(wolf) == kC1);
  const auto& sent = p.Messages();
  size_t keyframeAt = sent.size();
  size_t wordAt = sent.size();
  for (size_t i = 0; i < sent.size(); ++i) {
    const auto& m = sent[i];
    if (m.userId != 0 || !m.j.is_object()) {
      continue;
    }
    if (keyframeAt == sent.size() && m.reliable &&
        m.j.value("t", -1) == static_cast<int>(MsgType::UpdateMovement) &&
        m.j.value("idx", 0u) == wolf.GetIdx()) {
      keyframeAt = i;
      REQUIRE(std::abs(m.j["data"]["pos"][0].get<float>() -
                       (kHere.x + 300.f)) < 1.f);
      REQUIRE(m.j["data"]["worldOrCell"].get<uint32_t>() == kTamriel);
    }
    if (wordAt == sent.size() &&
        m.j.value("propName", std::string()) == "isHostedByOther" &&
        m.j.value("dataDump", std::string()) == "false" &&
        m.j.value("refrId", 0u) == kWolf) {
      wordAt = i;
    }
  }
  REQUIRE(keyframeAt < sent.size());
  REQUIRE(wordAt < sent.size());
  REQUIRE(keyframeAt < wordAt);

  DoDisconnect(p, 0);
  p.DestroyActor(kWolf);
  p.DestroyActor(kC1);
}

TEST_CASE("A host swap (a nearer seat takes the cell) sends the new host each "
          "actor's record ahead of the Release's flip, which already names it "
          "(docs/engine/non-owning-seat.md section 3: the keyframe goes to "
          "whichever game the word names)",
          "[CellHost]")
{
  auto& p = GetPartOne();
  Player(p, 0, kC1, kHere);
  const auto east = p.worldState.CellOf(kTamriel, kEast);
  REQUIRE(east.has_value());
  // c1 hosts the east square from a ring away (its load report), with the
  // wolf in it
  CellLoaded(p, 0, *east);
  REQUIRE(p.GetCellHost(*east).player == kC1);
  p.CreateActor(kWolf, kEast, 0, kTamriel);
  auto& wolf = p.worldState.GetFormAt<MpActor>(kWolf);
  wolf.SetPos(kEast);
  REQUIRE(p.SimulatorOf(wolf) == kC1);
  p.Messages().clear();

  // c2 stands in the square itself: the strictly nearer ring takes it, as
  // [Release(c1), Grant(c2)]; for the wolf the Release's flip names c2
  // already, so c2 hears the record before that word
  Player(p, 1, kC2, kEast);
  CellLoaded(p, 1, *east);
  REQUIRE(p.GetCellHost(*east).player == kC2);
  REQUIRE(p.SimulatorOf(wolf) == kC2);
  const auto& sent = p.Messages();
  size_t keyframeAt = sent.size();
  size_t wordAt = sent.size();
  for (size_t i = 0; i < sent.size(); ++i) {
    const auto& m = sent[i];
    if (m.userId != 1 || !m.j.is_object()) {
      continue;
    }
    if (keyframeAt == sent.size() && m.reliable &&
        m.j.value("t", -1) == static_cast<int>(MsgType::UpdateMovement) &&
        m.j.value("idx", 0u) == wolf.GetIdx()) {
      keyframeAt = i;
    }
    if (wordAt == sent.size() &&
        m.j.value("propName", std::string()) == "isHostedByOther" &&
        m.j.value("dataDump", std::string()) == "false" &&
        m.j.value("refrId", 0u) == kWolf) {
      wordAt = i;
    }
  }
  REQUIRE(keyframeAt < sent.size());
  REQUIRE(wordAt < sent.size());
  REQUIRE(keyframeAt < wordAt);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kWolf);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

// thuum docs/verbs/console-commands.md, `mp host` (Eli in playtest 48,
// 2026-11-06: "who's hosting? (you should make a console command i can ask
// that with instead of you)"): any player asks who hosts the cell it stands
// in, the grant's epoch and how the grant came, fresh with nobody hosting
// before it or inherited from a host that stopped (the lab's last_grant
// rule), and its own part
TEST_CASE("mp host names the host of the caller's cell, its epoch and how "
          "its grant came",
          "[CellHost][ConsoleCommand]")
{
  auto& p = GetPartOne();
  auto& c1 = Player(p, 0, kC1, kHere);
  auto& c2 = Player(p, 1, kC2, kHere + NiPoint3{ 100.f, 0.f, 0.f });
  const auto cell = p.worldState.CellOf(kTamriel, kHere);
  REQUIRE(cell.has_value());
  REQUIRE(p.worldState.CellOf(c2) == cell);
  // the console lines `user` gets for its `mp host`
  const auto ask = [&](Networking::UserId user) {
    p.Messages().clear();
    RawMessageData raw;
    raw.userId = user;
    ConsoleCommandMessage msg;
    msg.data.commandName = "mp";
    msg.data.args = { int64_t(0), std::string("host") };
    p.GetActionListener().OnConsoleCommand(raw, msg);
    std::vector<std::string> lines;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j["t"] == MsgType::ConsoleOutput) {
        lines.push_back(m.j["text"].get<std::string>());
      }
    }
    return lines;
  };
  using Catch::Matchers::ContainsSubstring;
  const std::string c1Name =
    "(no name) (" + std::to_string(c1.GetProfileId()) + ")";
  const std::string c2Name =
    "(no name) (" + std::to_string(c2.GetProfileId()) + ")";

  // nobody loads it yet: nobody hosts it
  auto lines = ask(0);
  REQUIRE(lines.size() == 2);
  REQUIRE_THAT(lines[0],
               ContainsSubstring(
                 ": nobody hosts it (frozen at its record, epoch 0 kept)"));
  REQUIRE(lines[1] == "mp host done");

  // c1 loads it first: a fresh grant at epoch 1, c1 its host, c2 its guest
  CellLoaded(p, 0, *cell);
  CellLoaded(p, 1, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  lines = ask(0);
  REQUIRE(lines.size() == 2);
  REQUIRE_THAT(lines[0],
               ContainsSubstring(": host " + c1Name +
                                 ", epoch 1, its grant fresh; you host it"));
  lines = ask(1);
  REQUIRE(lines.size() == 2);
  REQUIRE_THAT(
    lines[0],
    ContainsSubstring(": host " + c1Name +
                      ", epoch 1, its grant fresh; you are its guest"));

  // c1 leaves: at the grace's end c2 takes the cell, an inherited grant
  const auto now = NowMs();
  DoDisconnect(p, 0);
  p.TickCellHosts(now + skymp::rules::host_grace_ms() + kPastMs);
  REQUIRE(p.GetCellHost(*cell).player == kC2);
  REQUIRE(p.GetCellHost(*cell).epoch == 2);
  lines = ask(1);
  REQUIRE(lines.size() == 2);
  REQUIRE_THAT(
    lines[0],
    ContainsSubstring(": host " + c2Name +
                      ", epoch 2, its grant inherited; you host it"));
  REQUIRE(lines[1] == "mp host done");

  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

// thuum #48 leg D (x-reanimate-claim v2.4 20261116-035932; ADR-060): a
// player's load door is the server's teleport, so the engine's door transit,
// which keeps a commanded actor with the player in single player, never ran
// and c1's game dropped Sven outside ("demote 13491: the engine had it at
// ... in 3c", c1 04:05:18.267), his record left in Tamriel. The door now
// takes the player's thralls near it through with it, the command kept: its
// row names the landing's cell and its simulator is the commander there. A
// thrall beyond the reach and another player's thrall stay. The door is
// Whiterun's gate WRDoorMainGate01 0x1b1f3 into Tamriel, the one
// PartOne_ActivateTest drives, where Riverwood's night locks do not reach;
// the thralls are game-file actors set beside it: Sven 0x13491 and Hilde
// 0x35534 (Riverwood's, moved here) and Whiterun's cow
TEST_CASE("A player's load door takes the thralls it commands near it "
          "through with it, the command kept; a far one, another "
          "player's and an NPC's raise it commands as its host stay",
          "[CellHost][espm]")
{
  constexpr uint32_t kGate = 0x1b1f3;
  constexpr uint32_t kSven = 0x13491;
  constexpr uint32_t kHilde = 0x35534;
  // tracker #106: a body an NPC raised, which c1 commands as its caster's
  // host (ClaimReanimated), and that NPC's id (the door's rule reads the
  // accepted caster alone, so no actor stands for it)
  constexpr uint32_t kNpcRaised = 0xff000c08;
  constexpr uint32_t kNecromancer = 0xff000c09;
  // Dead Thrall's record duration (a full thrall, ADR-060 as amended
  // 2026-11-12; since 2026-11-15 it dies at its commander's departure too)
  constexpr uint32_t kFullThrallS = 86'313'600;
  // the gate's side in Whiterun and its landing in Tamriel
  // (PartOne_ActivateTest, "Activate WRDoorMainGate01 in Whiterun")
  const NiPoint3 kAtGate{ 19367.3379f, -7433.0698f, -3547.4492f };
  const NiPoint3 kLanding{ 19243.53515625f, -7427.3427734375f,
                           -3595.4052734375f };
  auto& p = GetPartOne();
  const bool npcWas = p.worldState.npcEnabled;
  p.worldState.npcEnabled = true;

  DoConnect(p, 0);
  p.CreateActor(kC1, kAtGate, 0, kWhiterun);
  p.SetUserActor(0, kC1);
  auto& c1 = p.worldState.GetFormAt<MpActor>(kC1);
  DoConnect(p, 1);
  p.CreateActor(kC2, kAtGate + NiPoint3{ 200.f, 0.f, 0.f }, 0, kWhiterun);
  p.SetUserActor(1, kC2);
  auto& c2 = p.worldState.GetFormAt<MpActor>(kC2);

  // a game-file actor set at `pos` in Whiterun's worldspace
  const auto place = [&](uint32_t id, const NiPoint3& pos) -> MpActor& {
    const auto& form = p.worldState.LookupFormById(id);
    REQUIRE(form);
    MpActor* actor = form->AsActor();
    REQUIRE(actor);
    actor->SetCellOrWorldObsolete(
      FormDesc::FromFormId(kWhiterun, p.worldState.espmFiles));
    actor->SetPos(pos);
    return *actor;
  };
  auto& sven = place(kSven, kAtGate + NiPoint3{ 80.f, 60.f, 0.f });
  auto& hilde = place(kHilde, kAtGate + NiPoint3{ 5000.f, 0.f, 0.f });
  auto& cow = place(kCow, kAtGate + NiPoint3{ -80.f, 60.f, 0.f });
  REQUIRE_FALSE(sven.IsDead());
  const auto gateCell = p.worldState.CellOf(kWhiterun, kAtGate);
  const auto farCell = p.worldState.CellOf(hilde);
  REQUIRE(gateCell.has_value());
  REQUIRE(farCell.has_value());
  REQUIRE(*farCell != *gateCell);
  CellLoaded(p, 0, *gateCell);
  CellLoaded(p, 0, *farCell);
  CellLoaded(p, 1, *gateCell);
  REQUIRE(p.ClaimCommanded(c1, sven, kFullThrallS) ==
          PartOne::ClaimVerdict::Granted);
  REQUIRE(p.ClaimCommanded(c1, hilde, kFullThrallS) ==
          PartOne::ClaimVerdict::Granted);
  REQUIRE(p.ClaimCommanded(c2, cow, kFullThrallS) ==
          PartOne::ClaimVerdict::Granted);
  p.CreateActor(kNpcRaised, kAtGate + NiPoint3{ 60.f, -60.f, 0.f }, 0,
                kWhiterun);
  auto& npcRaised = p.worldState.GetFormAt<MpActor>(kNpcRaised);
  npcRaised.SetPos(npcRaised.GetPos());
  REQUIRE(p.worldState.CellOf(npcRaised) == gateCell);
  npcRaised.NoteReanimateAccepted(std::chrono::steady_clock::now(), 0,
                                  kNecromancer);
  REQUIRE(p.ClaimCommanded(c1, npcRaised, kFullThrallS) ==
          PartOne::ClaimVerdict::Granted);
  REQUIRE(p.CommanderOf(sven) == kC1);
  REQUIRE(sven.GetListeners().count(&c2) == 1);
  const NiPoint3 hildeWas = hilde.GetPos();
  const NiPoint3 cowWas = cow.GetPos();
  const NiPoint3 npcRaisedWas = npcRaised.GetPos();

  // c1 takes the gate
  p.Messages().clear();
  DoMessage(p, 0,
            nlohmann::json{ { "t", MsgType::Activate },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", kGate },
                                { "isSecondActivation", false } } } });
  REQUIRE(c1.GetCellOrWorld() == FormDesc::Tamriel());

  // the thrall beside it went through with it: the record at the landing,
  // still commanded by c1, which simulates it there; its row names the
  // landing's cell; c1's game sees it there and c2's no longer does
  REQUIRE(sven.GetCellOrWorld() == FormDesc::Tamriel());
  REQUIRE((sven.GetPos() - kLanding).Length() < 1.f);
  REQUIRE(p.CommanderOf(sven) == kC1);
  REQUIRE(p.SimulatorOf(sven) == kC1);
  const auto landingCell = p.worldState.CellOf(kTamriel, kLanding);
  REQUIRE(landingCell.has_value());
  bool rowMoved = false;
  for (const auto& row : p.GetFollows()) {
    if (row.actor == kSven) {
      rowMoved = row.player == kC1 && row.cell == *landingCell;
    }
  }
  REQUIRE(rowMoved);
  REQUIRE(sven.GetListeners().count(&c1) == 1);
  REQUIRE(sven.GetListeners().count(&c2) == 0);
  // thuum tracker #162 (the review of its first build): the door into
  // another world parts the pair, the keep's cellOrWorld guard, so c1's
  // game gets a DestroyActor for the thrall at the gate and then its
  // CreateActor at the landing, the word false (the thrall's own pass);
  // without the guard c1's pass would keep it and neither would come
  size_t destroyAt = 0;
  size_t createAt = 0;
  std::optional<CreateActorMessage> created;
  for (size_t i = 0; i < p.Messages().size(); ++i) {
    const auto& m = p.Messages()[i];
    if (m.userId != 0) {
      continue;
    }
    if (auto destroy = dynamic_cast<DestroyActorMessage*>(m.message.get());
        destroy && destroy->idx == sven.GetIdx() && destroyAt == 0) {
      destroyAt = i + 1;
    }
    if (auto create = dynamic_cast<CreateActorMessage*>(m.message.get());
        create && create->refrId.value_or(0) == kSven) {
      createAt = i + 1;
      created = *create;
    }
  }
  REQUIRE(destroyAt != 0);
  REQUIRE(createAt > destroyAt);
  REQUIRE(created.has_value());
  REQUIRE(created->transform.pos[0] == Catch::Approx(kLanding.x));
  REQUIRE(created->transform.pos[1] == Catch::Approx(kLanding.y));
  REQUIRE(created->props.isHostedByOther == false);

  // the thrall beyond the reach and c2's thrall stay where they stood,
  // their commands as they were
  REQUIRE(hilde.GetCellOrWorld().ToFormId(p.worldState.espmFiles) ==
          kWhiterun);
  REQUIRE((hilde.GetPos() - hildeWas).Length() < 1.f);
  REQUIRE(p.CommanderOf(hilde) == kC1);
  REQUIRE(cow.GetCellOrWorld().ToFormId(p.worldState.espmFiles) == kWhiterun);
  REQUIRE((cow.GetPos() - cowWas).Length() < 1.f);
  REQUIRE(p.CommanderOf(cow) == kC2);
  // the NPC's raise beside the gate stays with its caster, c1 still its
  // commander as the caster's host (tracker #106)
  REQUIRE(npcRaised.GetCellOrWorld().ToFormId(p.worldState.espmFiles) ==
          kWhiterun);
  REQUIRE((npcRaised.GetPos() - npcRaisedWas).Length() < 1.f);
  REQUIRE(p.CommanderOf(npcRaised) == kC1);

  // the shared world: the commands end first, so the commanders'
  // disconnects kill nobody (since ADR-060's amendment of 2026-11-15 every
  // thrall dies at its commander's departure, a full one as a timed one)
  p.ReleaseAnyClaim(sven);
  p.ReleaseAnyClaim(hilde);
  p.ReleaseAnyClaim(cow);
  p.ReleaseAnyClaim(npcRaised);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  REQUIRE_FALSE(sven.IsDead());
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kSven);
  p.DestroyActor(kHilde);
  p.DestroyActor(kCow);
  p.DestroyActor(kNpcRaised);
  p.worldState.npcEnabled = npcWas;
}

// thuum tracker #162 (the review of its first build): D6's keep holds a
// commander and its thrall a pair within one world or cell at any distance,
// so a load door whose landing lies in the world or cell it leaves parts
// nothing, and no create word would place the thrall at the landing in the
// commander's game, whose engine still runs it where it stood. The door
// sends the landing itself, a Teleport for the thrall's idx. The commander's
// own move and the door's call are made here as the door's path makes them
// (MpObjectReference's load door: the player's record first, then
// PartOne::TakeMinionsThroughDoor), five squares across Tamriel
TEST_CASE("A load door within one world sends the commander's game its "
          "thrall's landing, the pair held through it",
          "[CellHost][espm]")
{
  constexpr uint32_t kThrall = 0xff000c1d;
  // a command's record seconds, named for the lab alone (ClaimReanimated)
  constexpr uint32_t kRecordS = 60;
  const NiPoint3 kLanding = kHere + NiPoint3{ 20480.f, 0.f, 0.f };
  const NiPoint3 kFacing{ 0.f, 0.f, 90.f };
  auto& p = GetPartOne();
  const bool npcWas = p.worldState.npcEnabled;
  p.worldState.npcEnabled = true;
  auto& c1 = Player(p, 0, kC1, kHere);
  c1.SetPos(kHere);
  p.CreateActor(kThrall, kHere + NiPoint3{ 50.f, 50.f, 0.f }, 0, kTamriel);
  auto& thrall = p.worldState.GetFormAt<MpActor>(kThrall);
  thrall.SetPos(thrall.GetPos());
  const auto cell = p.worldState.CellOf(thrall);
  REQUIRE(cell.has_value());
  REQUIRE(p.worldState.CellOf(kTamriel, kLanding).has_value());
  CellLoaded(p, 0, *cell);
  // c1's raise, as OnHit stamps it ahead of ClaimReanimated
  thrall.NoteReanimateAccepted(std::chrono::steady_clock::now(), 0, kC1);
  REQUIRE(p.ClaimCommanded(c1, thrall, kRecordS) ==
          PartOne::ClaimVerdict::Granted);
  REQUIRE(p.SimulatorOf(thrall) == kC1);
  REQUIRE(thrall.GetListeners().count(&c1) == 1);

  // the player's record at the landing: the pair held, no DestroyActor
  p.Messages().clear();
  c1.SetPos(kLanding);
  REQUIRE(thrall.GetListeners().count(&c1) == 1);
  const auto taken = p.TakeMinionsThroughDoor(c1, 0, kTamriel, kHere, kTamriel,
                                              kLanding, kFacing);
  REQUIRE(taken == std::vector<uint32_t>{ kThrall });
  REQUIRE((thrall.GetPos() - kLanding).Length() < 1.f);
  REQUIRE(thrall.GetListeners().count(&c1) == 1);

  // c1's game: the landing for the thrall's idx, and no DestroyActor or
  // CreateActor for it
  const auto landings = SentTo<TeleportMessage>(p, 0);
  REQUIRE(landings.size() == 1);
  REQUIRE(landings[0].idx == thrall.GetIdx());
  REQUIRE(landings[0].pos[0] == Catch::Approx(kLanding.x));
  REQUIRE(landings[0].pos[1] == Catch::Approx(kLanding.y));
  REQUIRE(landings[0].rot[2] == Catch::Approx(kFacing.z));
  REQUIRE(landings[0].worldOrCell == kTamriel);
  for (const auto& m : SentTo<DestroyActorMessage>(p, 0)) {
    REQUIRE(m.idx != thrall.GetIdx());
  }
  for (const auto& m : SentTo<CreateActorMessage>(p, 0)) {
    REQUIRE(m.refrId.value_or(0) != kThrall);
  }

  p.ReleaseAnyClaim(thrall);
  DoDisconnect(p, 0);
  p.DestroyActor(kC1);
  p.DestroyActor(kThrall);
  p.worldState.npcEnabled = npcWas;
}

// thuum tracker #106 (docs/verbs/npc-fate.md, ADR-063): an NPC's raise is
// commanded by its caster's host (ClaimReanimated); once another game runs
// the caster, the thrall dies by the server's word, as at its commander's
// departure (PartOne::EndLostNpcThralls, MpActor::KillThrall), and its
// command ends with the death (OnDeathState). A player's own thrall keeps
// its command through the grant, its row outliving it (wire-rules cells.rs
// FollowKind::Commanded)
TEST_CASE("An NPC's thrall dies when another game runs its caster; the "
          "player's own thrall keeps its command",
          "[CellHost]")
{
  constexpr uint32_t kNecromancer = 0xff000c05;
  constexpr uint32_t kNpcThrall = 0xff000c06;
  constexpr uint32_t kOwnThrall = 0xff000c07;
  // a command's record seconds, named for the lab alone (ClaimReanimated)
  constexpr uint32_t kRecordS = 60;
  auto& p = GetPartOne();
  const bool npcWas = p.worldState.npcEnabled;
  p.worldState.npcEnabled = true;
  auto& c1 = Player(p, 0, kC1, kHere);
  auto& c2 = Player(p, 1, kC2, kHere + NiPoint3{ 100.f, 0.f, 0.f });
  // a server-made actor joins the grid at its own first SetPos (the first
  // case's note)
  const auto make = [&](uint32_t id, const NiPoint3& at) -> MpActor& {
    p.CreateActor(id, at, 0, kTamriel);
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(actor.GetPos());
    return actor;
  };
  auto& necromancer = make(kNecromancer, kHere + NiPoint3{ 50.f, 50.f, 0.f });
  auto& npcThrall = make(kNpcThrall, kHere + NiPoint3{ 80.f, 60.f, 0.f });
  auto& ownThrall = make(kOwnThrall, kHere + NiPoint3{ -80.f, 60.f, 0.f });
  const auto cell = p.worldState.CellOf(necromancer);
  REQUIRE(cell.has_value());
  REQUIRE(p.worldState.CellOf(npcThrall) == cell);
  REQUIRE(p.worldState.CellOf(ownThrall) == cell);
  CellLoaded(p, 0, *cell);
  CellLoaded(p, 1, *cell);
  REQUIRE(p.GetCellHost(*cell).player == kC1);
  REQUIRE(p.worldState.HostOf(necromancer) == &c1);

  // the raises as OnHit stamps them ahead of ClaimReanimated: the
  // necromancer's, commanded by its host c1; c1's own
  const auto at = std::chrono::steady_clock::now();
  npcThrall.NoteReanimateAccepted(at, 0, kNecromancer);
  ownThrall.NoteReanimateAccepted(at, 0, kC1);
  REQUIRE(p.ClaimCommanded(c1, npcThrall, kRecordS) ==
          PartOne::ClaimVerdict::Granted);
  REQUIRE(p.ClaimCommanded(c1, ownThrall, kRecordS) ==
          PartOne::ClaimVerdict::Granted);

  // c1 still runs the necromancer: nothing ends
  p.EndLostNpcThralls();
  REQUIRE_FALSE(npcThrall.IsDead());
  REQUIRE(p.CommanderOf(npcThrall) == kC1);

  // c1 unloads the cell, still connected, and the grace passes it to c2,
  // which runs the necromancer now: its thrall dies and the command ends;
  // c1's own thrall keeps its command
  const auto now = NowMs();
  CellLoaded(p, 0, *cell, false);
  p.TickCellHosts(now + skymp::rules::host_grace_ms() + kPastMs);
  REQUIRE(p.GetCellHost(*cell).player == kC2);
  REQUIRE(p.worldState.HostOf(necromancer) == &c2);
  p.EndLostNpcThralls();
  REQUIRE(npcThrall.IsDead());
  REQUIRE(p.CommanderOf(npcThrall) == 0);
  REQUIRE_FALSE(ownThrall.IsDead());
  REQUIRE(p.CommanderOf(ownThrall) == kC1);

  p.ReleaseAnyClaim(ownThrall);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kNecromancer);
  p.DestroyActor(kNpcThrall);
  p.DestroyActor(kOwnThrall);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.worldState.npcEnabled = npcWas;
}

// thuum tracker #121 (ADR-035 point 8 as amended 2026-11-16, Eli's "go with
// b for 121"): the destination's host owns the arrival. A resident whose
// record stands frozen in an interior nobody hosts (playtest 53: Gorm 0x1aa69
// in Highmoon Hall 0x138cc while both players sat in the Moorside Inn
// 0x138ce) is taken by the host of the cell its engine walked it into, from
// that host's roster, the record following (the client keeps the walker's AI
// on while its package heads there, PuppetService). A sighting of it on the
// way, in a cell nobody hosts, is held for that cell's grant and the record
// stays frozen; no game reports a frozen cell
TEST_CASE("A resident frozen in an unhosted interior comes to the cell whose "
          "host sights it there; a sighting on the way leaves it frozen",
          "[CellHost][espm]")
{
  constexpr uint32_t kGorm = 0x1aa69;
  constexpr uint32_t kHall = 0x138cc;
  constexpr uint32_t kMoorside = 0x138ce;
  // lab/esm.py on sky-srv (2026-11-16): GormREF at (-89, 1297, 1) in the
  // hall; the inn's door landing (door 0x177af) and the supper package's
  // marker 0xe006e at (1822, 197, 0) in the inn; the hall's outer door
  // landing (door 0x173c6) on Morthal's street in Tamriel
  const NiPoint3 kInHall{ -89.f, 1297.f, 1.f };
  const NiPoint3 kInnLanding{ 1825.f, -533.f, 1.f };
  const NiPoint3 kAtSupper{ 1822.f, 197.f, 0.f };
  const NiPoint3 kStreet{ -38660.f, 63424.f, -13786.f };
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.IsInteriorCell(kHall));
  REQUIRE(p.worldState.IsInteriorCell(kMoorside));

  // c2 stands in the inn and hosts it; nobody hosts the hall or the street
  Player(p, 0, kC1, kHere);
  DoConnect(p, 1);
  p.CreateActor(kC2, kInnLanding, 0, kMoorside);
  p.SetUserActor(1, kC2);
  CellLoaded(p, 1, kMoorside);
  REQUIRE(p.GetCellHost(kMoorside).player == kC2);
  REQUIRE_FALSE(p.GetCellHost(kHall).hosted);
  const auto street = p.worldState.CellOf(kTamriel, kStreet);
  REQUIRE(street.has_value());
  REQUIRE_FALSE(p.GetCellHost(*street).hosted);

  // his record frozen in the hall
  auto& form = p.worldState.LookupFormById(kGorm);
  REQUIRE(form);
  MpActor* gorm = form->AsActor();
  REQUIRE(gorm);
  gorm->SetCellOrWorldObsolete(
    FormDesc::FromFormId(kHall, p.worldState.espmFiles));
  gorm->SetPos(kInHall);
  REQUIRE(p.worldState.CellOf(*gorm) == kHall);
  REQUIRE(p.worldState.HostOf(*gorm) == nullptr);
  p.Messages().clear();

  const auto sighting = [&](Networking::UserId user, uint32_t worldOrCell,
                            const NiPoint3& pos) {
    DoMessage(p, user,
              nlohmann::json{ { "t", MsgType::ActorSightings },
                              { "sightings",
                                nlohmann::json::array({ nlohmann::json{
                                  { "refrId", kGorm },
                                  { "worldOrCell", worldOrCell },
                                  { "pos", { pos.x, pos.y, pos.z } },
                                  { "rot", { 0.f, 0.f, 0.f } } } }) } });
  };

  // on the way, the street nobody hosts: held, the record frozen in the hall
  sighting(1, kTamriel, kStreet);
  REQUIRE(p.worldState.CellOf(*gorm) == kHall);
  REQUIRE(p.worldState.HostOf(*gorm) == nullptr);
  REQUIRE(SentTo<CreateActorMessage>(p, 1).empty());

  // at supper: the inn's host's roster has him in the inn; the record follows
  // and the inn's host simulates him, told so with its word
  sighting(1, kMoorside, kAtSupper);
  REQUIRE(p.worldState.CellOf(*gorm) == kMoorside);
  REQUIRE(gorm->GetPos().y == Catch::Approx(kAtSupper.y));
  REQUIRE(p.worldState.HostOf(*gorm) == &p.worldState.GetFormAt<MpActor>(kC2));
  bool told = false;
  for (auto& m : SentTo<CreateActorMessage>(p, 1)) {
    if (m.refrId == kGorm) {
      told = true;
      REQUIRE(m.props.isHostedByOther == false);
    }
  }
  REQUIRE(told);

  p.SweepHeldSightings(NowMs() + PartOne::kHeldSightingMs + 1);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
}

// thuum tracker #121 (playtest 55, Eli: "Wait for Gorm"; design B, the
// destination's host takes the arrival, read at the door): Gorm 0x1aa69's
// record stood frozen in Highmoon Hall 0x138cc, which nobody hosted, while c2
// hosted the Moorside Inn 0x138ce and its engine walked him in the dark to
// the inn's street door 0x177af, where the engine's ActivateRef refused on
// our client's activation block (confirmed by trace: tracker #121 note 3311,
// run 20261118-023818 on c2) and no game reported him.
// The inn's host reports him now, and the one door walk
// (MpObjectReference::WalkThroughDoor) asks its game to walk him through
// (D6's snippet, ObjectReference.Activate with default processing); nothing
// is recorded until its roster sights him inside. A game that does not host
// the inn is refused with a line, never the Bad hoster throw, before the
// sighting and after it (another game simulating him then); the inn host's
// second report before the sighting is refused, so a late duplicate never
// puts him back at the landing; and once the sighting names the inn's host
// his simulator, its report of the inn's door is D6's
TEST_CASE("A resident frozen in an unhosted interior walks through the street "
          "door into the inn on the inn host's report; another seat's report "
          "of the same door is refused; a hosted resident's door keeps D6's "
          "verdict",
          "[CellHost][espm][Activate]")
{
  constexpr uint32_t kGorm = 0x1aa69;
  constexpr uint32_t kHall = 0x138cc;
  constexpr uint32_t kMoorside = 0x138ce;
  // lab/esm.py door and ref on sky-srv (Skyrim.esm, 2026-11-17): the inn's
  // street door 0x177af stands in Tamriel at (-40095, 65547, -13802) and
  // teleports to the inn's door 0x1738c, at (1824, -576, 0) inside, with the
  // landing (1825, -533, 1); 0x1738c teleports back out to (-40054, 65484,
  // -13802); GormREF at (-89, 1297, 1) in the hall
  constexpr uint32_t kStreetDoor = 0x177af;
  constexpr uint32_t kInnDoor = 0x1738c;
  const NiPoint3 kInHall{ -89.f, 1297.f, 1.f };
  const NiPoint3 kInnLanding{ 1825.f, -533.f, 1.f };
  const NiPoint3 kStreetLanding{ -40054.f, 65484.f, -13802.f };
  const NiPoint3 kAtSupper{ 1822.f, 197.f, 0.f };
  auto& p = GetPartOne();
  const bool npcWas = p.worldState.npcEnabled;
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.IsInteriorCell(kHall));
  REQUIRE(p.worldState.IsInteriorCell(kMoorside));

  // the two doors as the game files place them
  auto& streetDoor = p.worldState.GetFormAt<MpObjectReference>(kStreetDoor);
  const auto into = streetDoor.TeleportDestination();
  REQUIRE(into.has_value());
  REQUIRE(into->worldOrCell == kMoorside);
  REQUIRE((into->pos - kInnLanding).Length() < 1.f);
  auto& innDoor = p.worldState.GetFormAt<MpObjectReference>(kInnDoor);
  const auto out = innDoor.TeleportDestination();
  REQUIRE(out.has_value());
  REQUIRE(out->worldOrCell == kTamriel);
  REQUIRE((out->pos - kStreetLanding).Length() < 1.f);
  REQUIRE_FALSE(streetDoor.IsOpen());

  // c2 stands in the inn and hosts it; nobody hosts the hall or the street
  Player(p, 0, kC1, kHere);
  DoConnect(p, 1);
  p.CreateActor(kC2, kInnLanding, 0, kMoorside);
  p.SetUserActor(1, kC2);
  CellLoaded(p, 1, kMoorside);
  REQUIRE(p.GetCellHost(kMoorside).player == kC2);
  REQUIRE_FALSE(p.GetCellHost(kHall).hosted);
  const auto street = p.worldState.CellOf(kTamriel, kStreetLanding);
  REQUIRE(street.has_value());
  REQUIRE_FALSE(p.GetCellHost(*street).hosted);

  // his record frozen in the hall, no game simulating him
  auto& form = p.worldState.LookupFormById(kGorm);
  REQUIRE(form);
  MpActor* gorm = form->AsActor();
  REQUIRE(gorm);
  gorm->SetCellOrWorldObsolete(
    FormDesc::FromFormId(kHall, p.worldState.espmFiles));
  gorm->SetPos(kInHall);
  REQUIRE(p.worldState.CellOf(*gorm) == kHall);
  REQUIRE(p.worldState.HostOf(*gorm) == nullptr);
  p.Messages().clear();

  // `user`'s report of Gorm activating `door`, as the client's door branch
  // sends it (MsgType 6, the bytes D6 already sends)
  const auto report = [&](Networking::UserId user, uint32_t door) {
    DoMessage(p, user,
              nlohmann::json{ { "t", MsgType::Activate },
                              { "data",
                                { { "caster", kGorm },
                                  { "target", door },
                                  { "isSecondActivation", false } } } });
  };
  // the door walks asked of `user`'s game since the last clear: D6's
  // snippet, Gorm the activator with default processing only (snippets are
  // deferred to the tick, SpSnippet.cpp)
  const auto walksAskedOf = [&](Networking::UserId user) {
    p.Tick();
    size_t n = 0;
    for (auto& m : p.Messages()) {
      if (m.userId != user || !m.j.is_object() || !m.j.contains("t") ||
          m.j["t"] != static_cast<int>(MsgType::SpSnippet) ||
          m.j.value("class", std::string()) != "ObjectReference" ||
          m.j.value("function", std::string()) != "Activate") {
        continue;
      }
      const auto& args = m.j["arguments"];
      REQUIRE(args.is_array());
      REQUIRE(args.size() == 2);
      REQUIRE(args[0]["formId"] == kGorm);
      REQUIRE(args[1] == true);
      ++n;
    }
    return n;
  };
  const auto followsGorm = [&] {
    for (const auto& row : p.GetFollows()) {
      if (row.actor == kGorm) {
        return true;
      }
    }
    return false;
  };
  const auto sighting = [&](Networking::UserId user, uint32_t worldOrCell,
                            const NiPoint3& pos) {
    DoMessage(p, user,
              nlohmann::json{ { "t", MsgType::ActorSightings },
                              { "sightings",
                                nlohmann::json::array({ nlohmann::json{
                                  { "refrId", kGorm },
                                  { "worldOrCell", worldOrCell },
                                  { "pos", { pos.x, pos.y, pos.z } },
                                  { "rot", { 0.f, 0.f, 0.f } } } }) } });
  };

  // c1, which does not host the inn, reports his copy at the street door:
  // refused by the verdict for c1 (the inn is c2's), a line and no throw;
  // nothing asked, the door shut, the record frozen
  REQUIRE_NOTHROW(report(0, kStreetDoor));
  REQUIRE(walksAskedOf(0) == 0);
  REQUIRE(walksAskedOf(1) == 0);
  REQUIRE_FALSE(streetDoor.IsOpen());
  REQUIRE(p.worldState.CellOf(*gorm) == kHall);

  // the inn's host reports him there: its game walks him through and the
  // door swings; nothing recorded at the door, the record frozen in the hall,
  // nobody's, no follow
  p.Messages().clear();
  REQUIRE_NOTHROW(report(1, kStreetDoor));
  REQUIRE(walksAskedOf(1) == 1);
  REQUIRE(walksAskedOf(0) == 0);
  REQUIRE(streetDoor.IsOpen());
  REQUIRE(p.worldState.CellOf(*gorm) == kHall);
  REQUIRE((gorm->GetPos() - kInHall).Length() < 1.f);
  REQUIRE(p.worldState.HostOf(*gorm) == nullptr);
  REQUIRE_FALSE(followsGorm());

  // its second report before the sighting (a late duplicate, or the event
  // of the snippet's own call): refused, so he is never put back at the
  // landing
  p.Messages().clear();
  REQUIRE_NOTHROW(report(1, kStreetDoor));
  REQUIRE(walksAskedOf(1) == 0);

  // c2's roster sights him inside: the record follows, c2 simulates him
  sighting(1, kMoorside, kAtSupper);
  REQUIRE(p.worldState.CellOf(*gorm) == kMoorside);
  REQUIRE(gorm->GetPos().y == Catch::Approx(kAtSupper.y));
  REQUIRE(p.worldState.HostOf(*gorm) == &p.worldState.GetFormAt<MpActor>(kC2));

  // c1's copy at the street door now: another game simulates him, so the
  // verdict refuses with a line, where this report met the Bad hoster throw
  // before; the record where the sighting put it
  p.Messages().clear();
  REQUIRE_NOTHROW(report(0, kStreetDoor));
  REQUIRE(walksAskedOf(0) == 0);
  REQUIRE(walksAskedOf(1) == 0);
  REQUIRE(p.worldState.CellOf(*gorm) == kMoorside);
  REQUIRE(gorm->GetPos().y == Catch::Approx(kAtSupper.y));

  // his simulator's report of the inn's door out to the street nobody
  // hosts: D6's verdict (a follow), its game walks him through; the record
  // stays inside until its report from the street
  Move(p, 1, kGorm, kInnLanding, kMoorside);
  REQUIRE((gorm->GetPos() - kInnLanding).Length() < 1.f);
  p.Messages().clear();
  REQUIRE_NOTHROW(report(1, kInnDoor));
  REQUIRE(walksAskedOf(1) == 1);
  REQUIRE(walksAskedOf(0) == 0);
  REQUIRE(p.worldState.CellOf(*gorm) == kMoorside);
  REQUIRE(p.worldState.HostOf(*gorm) == &p.worldState.GetFormAt<MpActor>(kC2));

  // Gorm's record is left in the inn and his accepted arrival stays noted on
  // him (MpActor::GetDoorArrival: the hall, for kDoorArrivalHoldMs) on this
  // shared PartOne; a later case that puts him back in the hall and has a
  // game report his arrival within that hold is refused by the guard
  p.SweepHeldSightings(NowMs() + PartOne::kHeldSightingMs + 1);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.worldState.npcEnabled = npcWas;
}

// thuum playtest 53 #113 (Eli's design A, 2026-11-17): a thrall fights the
// other player for as long as its commander's fight with that player stands
// on the record, then stands down with it. Its commander's game simulates it
// (ClaimCommanded), so that game alone is told, through the fight's own
// notices: Actor.StartCombat on the thrall aimed at the other player when the
// fight is seen standing and again while its host's last state names no
// target, Actor.StopCombat at the fight's end (PartOne::KeepThrallsInFight,
// StandDownThralls)
TEST_CASE("A thrall fights the other player while its commander's fight "
          "stands, is told again while its host names no target, and stands "
          "down with the fight",
          "[CellHost]")
{
  constexpr uint32_t kThrall = 0xff000c1b;
  // a command's record seconds, named for the lab alone (ClaimReanimated)
  constexpr uint32_t kRecordS = 60;
  auto& p = GetPartOne();
  const bool npcWas = p.worldState.npcEnabled;
  p.worldState.npcEnabled = true;
  auto& c1 = Player(p, 0, kC1, kHere);
  Player(p, 1, kC2, kHere + NiPoint3{ 100.f, 0.f, 0.f });
  p.CreateActor(kThrall, kHere + NiPoint3{ 50.f, 50.f, 0.f }, 0, kTamriel);
  auto& thrall = p.worldState.GetFormAt<MpActor>(kThrall);
  thrall.SetPos(thrall.GetPos());
  const auto cell = p.worldState.CellOf(thrall);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  CellLoaded(p, 1, *cell);
  // c1's raise, as OnHit stamps it ahead of ClaimReanimated
  thrall.NoteReanimateAccepted(std::chrono::steady_clock::now(), 0, kC1);
  REQUIRE(p.ClaimCommanded(c1, thrall, kRecordS) ==
          PartOne::ClaimVerdict::Granted);
  REQUIRE(p.CommanderOf(thrall) == kC1);
  REQUIRE(p.SimulatorOf(thrall) == kC1);

  // the snippets of `function` on the thrall `user` got since the last clear
  const auto toldTo = [&](Networking::UserId user, const char* function) {
    p.Tick(); // snippets are deferred
    std::vector<nlohmann::json> out;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() &&
          m.j.value("t", -1) == static_cast<int>(MsgType::SpSnippet) &&
          m.j.value("function", std::string()) == function &&
          m.j.value("selfId", uint64_t{ 0 }) == kThrall) {
        out.push_back(m.j);
      }
    }
    return out;
  };
  const auto tick = [&](uint64_t atMs) {
    p.Messages().clear();
    p.TickFights(atMs);
  };

  // no fight: nothing
  const uint64_t t0 = NowMs();
  tick(t0);
  REQUIRE(toldTo(0, "StartCombat").empty());

  // the players' fight begins (the record's hit, NotifyHostility): the
  // commander's game is told its thrall fights c2, the other game nothing
  REQUIRE(p.GetFights().hit(kC1, kC2, t0));
  tick(t0 + 1'000);
  const auto joined = toldTo(0, "StartCombat");
  REQUIRE(joined.size() == 1);
  REQUIRE(joined[0]["class"] == "Actor");
  REQUIRE(joined[0]["arguments"] ==
          nlohmann::json::array(
            { nlohmann::json{ { "formId", kC2 }, { "type", "Actor" } } }));
  REQUIRE(toldTo(1, "StartCombat").empty());

  // its host's state names c2: nothing again
  thrall.SetHostedState(kC2, 0);
  tick(t0 + 3'000);
  REQUIRE(toldTo(0, "StartCombat").empty());

  // its engine stood it down (its host's state names no target): told
  // again, then not within the period
  thrall.SetHostedState(0, 0);
  tick(t0 + 5'000);
  REQUIRE(toldTo(0, "StartCombat").size() == 1);
  tick(t0 + 5'500);
  REQUIRE(toldTo(0, "StartCombat").empty());

  // another enemy of its own: its engine's choice, nothing
  thrall.SetHostedState(kC1 + 0x100, 0);
  tick(t0 + 8'000);
  REQUIRE(toldTo(0, "StartCombat").empty());

  // a minute quiet: the fight ends, the players' games told as before and
  // the thrall stands down on its commander's game alone
  thrall.SetHostedState(kC2, 0);
  tick(t0 + 61'000);
  REQUIRE(!p.GetFights().between(kC1, kC2));
  REQUIRE(toldTo(0, "StopCombat").size() == 1);
  REQUIRE(toldTo(1, "StopCombat").empty());
  // and nothing after it
  tick(t0 + 63'000);
  REQUIRE(toldTo(0, "StartCombat").empty());
  REQUIRE(toldTo(0, "StopCombat").empty());

  // a player's fight with an NPC tells the thrall nothing, at its start or
  // its end: the thrall fighting that NPC is its engine's (R2)
  constexpr uint32_t kFoe = 0xff000c1c;
  p.CreateActor(kFoe, kHere + NiPoint3{ -60.f, 40.f, 0.f }, 0, kTamriel);
  auto& foe = p.worldState.GetFormAt<MpActor>(kFoe);
  foe.SetPos(foe.GetPos());
  REQUIRE(p.worldState.HostOf(foe) == &c1);
  thrall.SetHostedState(kFoe, 0);
  REQUIRE(p.GetFights().hit(kC1, kFoe, t0 + 63'000));
  tick(t0 + 64'000);
  REQUIRE(toldTo(0, "StartCombat").empty());
  tick(t0 + 125'000);
  REQUIRE(!p.GetFights().between(kC1, kFoe));
  REQUIRE(toldTo(0, "StopCombat").empty());

  // a dead thrall is never told
  REQUIRE(p.GetFights().hit(kC1, kC2, t0 + 126'000));
  thrall.Kill(nullptr);
  tick(t0 + 127'000);
  REQUIRE(toldTo(0, "StartCombat").empty());

  p.ReleaseAnyClaim(thrall);
  p.GetFights().forget(kC1);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kFoe);
  p.DestroyActor(kThrall);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.worldState.npcEnabled = npcWas;
}

// thuum docs/verbs/guest-witness.md: a hosted actor's detection of each player
// is its host's engine's; the server relays a report that carries it reliable
// to the actor's other games, its entries whole, and a report without it
// unreliable and without; it records nothing
TEST_CASE("A guest's theft in full view of an NPC the host's game runs "
          "reaches the guest's game: the host's detection of each player is "
          "relayed reliable to the actor's other games, its entries whole",
          "[CellHost][HostedState][HostedDetects]")
{
  auto& p = GetPartOne();
  constexpr uint32_t kActor = 0xff000c1d;
  const NiPoint3 kAt{ 117786.f, -81400.f, 10997.f };
  Player(p, 0, kC1, kAt + NiPoint3{ 40.f, 0.f, 0.f });
  Player(p, 1, kC2, kAt + NiPoint3{ 60.f, 0.f, 0.f });
  p.CreateActor(kActor, kAt + NiPoint3{ 30.f, 0.f, 0.f }, 0, kTamriel);
  auto& actor = p.worldState.GetFormAt<MpActor>(kActor);
  actor.SetPos(actor.GetPos());
  const auto cell = p.worldState.CellOf(actor);
  REQUIRE(cell.has_value());
  CellLoaded(p, 0, *cell);
  auto& c1 = p.worldState.GetFormAt<MpActor>(kC1);
  REQUIRE(p.worldState.HostOf(actor) == &c1);

  const auto statesTo = [&](Networking::UserId user) {
    std::vector<std::pair<HostedActorStateMessage, bool>> out;
    for (auto& m : p.Messages()) {
      if (m.userId != user) {
        continue;
      }
      if (auto typed = dynamic_cast<HostedActorStateMessage*>(m.message.get());
          typed && typed->idx == actor.GetIdx()) {
        out.emplace_back(*typed, m.reliable);
      }
    }
    return out;
  };

  // the host's NPC sees the guest (c2's actor) and not the host's own player
  const std::vector<HostedActorStateMessage::PlayerDetection> seen = {
    { kC1, -1000 }, { kC2, 57 }
  };
  p.Messages().clear();
  State(p, 0, kActor, 1.f, 1.f, 1.f, 0, 0, 0, 0, 0, 0, std::nullopt, seen);
  {
    const auto toC2 = statesTo(1);
    REQUIRE(toC2.size() == 1);
    REQUIRE(toC2.front().second);
    REQUIRE(toC2.front().first.detects.has_value());
    REQUIRE(*toC2.front().first.detects == seen);
    REQUIRE(statesTo(0).empty());
  }

  // a report without them: unreliable and without; the guest keeps the last
  p.Messages().clear();
  State(p, 0, kActor, 0.9f, 1.f, 1.f, 0, 0);
  {
    const auto toC2 = statesTo(1);
    REQUIRE(toC2.size() == 1);
    REQUIRE_FALSE(toC2.front().second);
    REQUIRE_FALSE(toC2.front().first.detects.has_value());
  }
}
