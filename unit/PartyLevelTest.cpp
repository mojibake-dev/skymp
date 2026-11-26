#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActionListener.h"
#include "GetBaseActorValues.h"
#include "Messages.h"
#include "wire_bridge_cxx/rules.h"
#include <chrono>
#include <fmt/format.h>
#include <string>
#include <vector>

PartOne& GetPartOne();

// A PC-level-mult NPC's level is its player's level times its mult, clamped
// to its calc levels (TESActorBaseData's GetLevel, id 14384). With several
// players in one place, the player's level is the party's: the mean of their
// levels leaned toward the highest (npcScaleLean, 0.75 by default), cut to a
// whole level. The party is the hosting game's player and every player whose
// cell that game hosts. Ahtar 0x1325f (Skyrim.esm: auto-calc, PC level mult
// 1000, calc levels 6 to 30, no offset, his class weighing health 4, magicka
// 0, stamina 2, his race's start 50), placed as 0x198b0 in cell 0x16a05, has
// 108 health at level 6, 155 at 10, 201 at 14, 248 at 18, 260 at 19 and 353
// at 27.
namespace {
constexpr uint32_t kPartyHost = 0xff000f9a;
constexpr uint32_t kPartyGuest = 0xff000f9b;
constexpr uint32_t kAhtar = 0x198b0;
constexpr uint32_t kAhtarCell = 0x16a05;
constexpr int kPartyLevel = 90;

uint64_t NowMsForParty()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

void SetPlayerLevel(MpActor& player, uint16_t level)
{
  ActorValueRecord record =
    player.GetActorValueRecord().value_or(ActorValueRecord{});
  record.level = level;
  player.SetActorValueRecord(record);
}

// a player at `level` joins Ahtar's cell as user `userId` and reports it
// loaded; the first to load it hosts it
MpActor& JoinAhtarsCell(PartOne& p, Networking::UserId userId, uint32_t id,
                        uint16_t level, uint64_t& clock)
{
  auto& ahtar = p.worldState.GetFormAt<MpActor>(kAhtar);
  DoConnect(p, userId);
  p.CreateActor(id, ahtar.GetPos() + NiPoint3{ 50.f, 50.f * userId, 0.f }, 0,
                kAhtarCell);
  p.SetUserActor(userId, id);
  auto& player = p.worldState.GetFormAt<MpActor>(id);
  SetPlayerLevel(player, level);
  p.TickCellHosts(clock += 1'000);
  RawMessageData raw;
  raw.userId = userId;
  CellLoadedMessage loaded;
  loaded.cell = kAhtarCell;
  loaded.loaded = true;
  p.GetActionListener().OnCellLoaded(raw, loaded);
  p.TickCellHosts(clock += 1'000);
  return player;
}

std::vector<std::string> PartyLevelsSent(PartOne& p)
{
  std::vector<std::string> out;
  for (auto& m : p.Messages()) {
    if (m.j["t"] == kPartyLevel) {
      out.push_back(
        fmt::format("user {} level {}", m.userId, m.j["level"].get<int>()));
    }
  }
  return out;
}
}

TEST_CASE("A PC-level-mult NPC's base values follow its party's level, "
          "the mean leaned toward the highest",
          "[PartyLevel][espm]")
{
  PartOne& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& ahtar = p.worldState.GetFormAt<MpActor>(kAhtar);
  uint64_t clock = NowMsForParty() + skymp::rules::host_grace_ms() + 2'000;

  // a level 10 player alone hosts his cell: his own level, 10
  auto& host = JoinAhtarsCell(p, 0, kPartyHost, 10, clock);
  REQUIRE(p.GetCellHost(kAhtarCell).hosted);
  REQUIRE(p.GetCellHost(kAhtarCell).player == kPartyHost);
  REQUIRE(ahtar.GetBaseValues().health == 155.f);

  // a level 30 player joins: 20 + 0.75 x 10, level 27
  auto& guest = JoinAhtarsCell(p, 1, kPartyGuest, 30, clock);
  REQUIRE(p.GetCellHost(kAhtarCell).player == kPartyHost);
  REQUIRE(ahtar.GetBaseValues().health == 353.f);

  // levels 18 and 20: 19 + 0.75 x 1, level 19
  SetPlayerLevel(host, 18);
  SetPlayerLevel(guest, 20);
  REQUIRE(ahtar.GetBaseValues().health == 260.f);

  // the guest leaves: the host's 18
  DoDisconnect(p, 1);
  p.DestroyActor(kPartyGuest);
  p.TickCellHosts(clock += 1'000);
  REQUIRE(ahtar.GetBaseValues().health == 248.f);

  // nobody runs him: level 1, his calc minimum 6
  DoDisconnect(p, 0);
  p.DestroyActor(kPartyHost);
  p.TickCellHosts(clock += skymp::rules::host_grace_ms() + 2'000);
  REQUIRE(!p.GetCellHost(kAhtarCell).hosted);
  REQUIRE(ahtar.GetBaseValues().health == 108.f);
}

TEST_CASE("Each game is told its party's level when a player joins, levels "
          "up or leaves",
          "[PartyLevel][espm]")
{
  PartOne& p = GetPartOne();
  p.worldState.npcEnabled = true;
  uint64_t clock = NowMsForParty() + skymp::rules::host_grace_ms() + 2'000;

  // a level 10 player alone hosts his cell: told his own 10
  p.Messages().clear();
  auto& host = JoinAhtarsCell(p, 0, kPartyHost, 10, clock);
  REQUIRE(PartyLevelsSent(p) == std::vector<std::string>{ "user 0 level 10" });

  // a level 30 player joins: both games are told 27
  p.Messages().clear();
  JoinAhtarsCell(p, 1, kPartyGuest, 30, clock);
  REQUIRE(PartyLevelsSent(p) ==
          std::vector<std::string>{ "user 0 level 27", "user 1 level 27" });

  // the host levels up to 14: 22 + 0.75 x 8, both told 28
  p.Messages().clear();
  SetPlayerLevel(host, 14);
  p.TickCellHosts(clock += 1'000);
  REQUIRE(PartyLevelsSent(p) ==
          std::vector<std::string>{ "user 0 level 28", "user 1 level 28" });

  // nothing changed: nothing is sent
  p.Messages().clear();
  p.TickCellHosts(clock += 1'000);
  REQUIRE(PartyLevelsSent(p).empty());

  // the guest leaves: the host is told its own 14
  DoDisconnect(p, 1);
  p.DestroyActor(kPartyGuest);
  p.TickCellHosts(clock += 1'000);
  REQUIRE(PartyLevelsSent(p) == std::vector<std::string>{ "user 0 level 14" });

  DoDisconnect(p, 0);
  p.DestroyActor(kPartyHost);
  p.TickCellHosts(clock += skymp::rules::host_grace_ms() + 2'000);
}
