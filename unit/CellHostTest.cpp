#include "ActionListener.h"
#include "HostGrantMessage.h"
#include "HostReleaseMessage.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>

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
          const NiPoint3& pos)
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
  msg.data.worldOrCell = kTamriel;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
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
