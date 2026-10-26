#include "ActionListener.h"
#include "ChangeValuesMessage.h"
#include "CreateActorMessage.h"
#include "HostGrantMessage.h"
#include "HostReleaseMessage.h"
#include "HostedActorStateMessage.h"
#include "OpenContainerMessage.h"
#include "TestUtils.hpp"
#include "UpdateActionMessage.h"
#include "UpdateAnimationMessage.h"
#include "UpdateEquipmentMessage.h"
#include "UpdateMovementMessage.h"
#include "UpdatePropertyMessage.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <cmath>
#include <map>

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

// `user`'s movement report for `actorId`, standing at `pos`
void Move(PartOne& p, Networking::UserId user, uint32_t actorId,
          const NiPoint3& pos, uint32_t worldOrCell = kTamriel)
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
  msg.data.isWeapDrawn = false;
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
           float magicka, float stamina, uint32_t target, uint32_t package)
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

  // the facts the door rule reads from the game files: the Sleeping Giant
  // Inn is an interior, Riverwood's exterior cell is not; Hod's placed
  // reference is persistent, a server-made actor has no record
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

// thuum docs/verbs/npc-handoff.md (d), ADR-047 D9 (playtest 34, 2026-10-25:
// a restart's cold start gave Sigrid's two cells to different players and
// her shuttle between them was refused at every crossing, seven a second,
// one engine walking her live where the other held the record): a hosted
// actor its host walks into an adjacent cell another live host holds passes
// to that host at once; the record moves, the new host's game hears "not
// hosted by other" and the old host's "hosted by other", the old host's
// further reports are dropped and the new host's pass; a crossing back
// passes it the same way.
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
  // report may move an actor 4096 at most): the record moves, the
  // simulation passes, no follow is left behind
  const NiPoint3 kAcross{ 12400.f, 500.f, 0.f };
  Move(p, 0, kWolf, kAcross);
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
  Move(p, 0, kWolf, kAcross + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(std::abs(wolf.GetPos().x - kAcross.x) < 1.f);
  Move(p, 1, kWolf, kAcross + NiPoint3{ 100.f, 0.f, 0.f });
  REQUIRE(std::abs(wolf.GetPos().x - (kAcross.x + 100.f)) < 1.f);

  // and back across: passed to c1 the same way
  Move(p, 1, kWolf, kHere);
  REQUIRE(p.worldState.CellOf(wolf) == here);
  REQUIRE(p.SimulatorOf(wolf) == kC1);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kWolf);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
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

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kC1);
  p.DestroyActor(kC2);
  p.DestroyActor(kSigrid);
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
                            { "package", 0 } });
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
          "item the record lacks is refused with nothing sent to the host",
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
  constexpr uint32_t kElsewhere = 0x13989;
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
  p.Messages().clear();
  Wear(p, 0, kActor, kIronSword, 1);
  REQUIRE(actor.GetEquipment().inv.entries.size() == 1);
  REQUIRE(actor.GetEquipment().inv.entries.front().baseId == kIronSword);
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

  // an item the actor's record lacks: refused, the record stands, and the
  // host is sent neither an inventory (it has no idx: the player's own
  // would be replaced) nor an unequip snippet (nothing writes to a hosted
  // actor's engine)
  p.Messages().clear();
  Wear(p, 0, kActor, kElsewhere, 3);
  p.Tick();
  REQUIRE(actor.GetEquipment().inv.entries.front().baseId == kIronSword);
  REQUIRE(actor.GetEquipment().numChanges == 1);
  REQUIRE(relays(1) == 0);
  REQUIRE(sentOfType(0, MsgType::SetInventory) == 0);
  REQUIRE(sentOfType(0, MsgType::SpSnippet) == 0);

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
