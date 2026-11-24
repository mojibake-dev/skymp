#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <nlohmann/json.hpp>
#include <vector>

PartOne& GetPartOne();

namespace {
// a host and a guest in Bleak Falls Temple, the host's game hosting it; the
// bandits' chest there stands for a word wall's look marker (a reference in
// a cell the host's game hosts)
constexpr uint32_t kLookHost = 0xff000fe7;
constexpr uint32_t kLookGuest = 0xff000fe8;
constexpr uint32_t kLookTemple = 0x371de;
constexpr uint32_t kLookChest = 0x371e9;
const NiPoint3 kLookByTheChest{ -3420.f, 732.f, 575.f };

uint64_t LookNowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

void LookEnter(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kLookHost, kLookByTheChest, 0, kLookTemple);
  p.SetUserActor(0, kLookHost);
  DoConnect(p, 1);
  p.CreateActor(kLookGuest, kLookByTheChest, 0, kLookTemple);
  p.SetUserActor(1, kLookGuest);
  p.worldState.GetFormAt<MpObjectReference>(kLookChest);
  for (uint32_t id : { kLookHost, kLookGuest }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(kLookByTheChest);
    actor.ForceSubscriptionsUpdate();
  }
  const auto past = LookNowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  RawMessageData raw;
  raw.userId = 0;
  CellLoadedMessage loaded;
  loaded.cell = kLookTemple;
  loaded.loaded = true;
  p.GetActionListener().OnCellLoaded(raw, loaded);
  p.TickCellHosts(past);
  REQUIRE(p.GetCellHost(kLookTemple).hosted);
  REQUIRE(p.GetCellHost(kLookTemple).player == kLookHost);
  p.Messages().clear();
}

void LookLeave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kLookHost);
  p.DestroyActor(kLookGuest);
}

void Ask(PartOne& p, Networking::UserId user, uint32_t player, uint32_t target)
{
  PlayerLookAskMessage msg;
  msg.player = player;
  msg.target = target;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnPlayerLookAsk(raw, msg);
}

void Look(PartOne& p, Networking::UserId user, uint32_t player,
          uint32_t target, bool los)
{
  PlayerLookMessage msg;
  msg.player = player;
  msg.target = target;
  msg.los = los;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnPlayerLook(raw, msg);
}

std::vector<nlohmann::json> Sent(PartOne& p, Networking::UserId user,
                                 MsgType type)
{
  std::vector<nlohmann::json> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j["t"] == static_cast<int>(type)) {
      out.push_back(m.j);
    }
  }
  return out;
}
}

TEST_CASE("The host's script's look ask of the guest goes to the guest's own "
          "game alone",
          "[PlayerLook][espm]")
{
  PartOne& p = GetPartOne();
  LookEnter(p);
  Ask(p, 0, kLookGuest, kLookChest);
  const auto toGuest = Sent(p, 1, MsgType::PlayerLookAsk);
  REQUIRE(toGuest.size() == 1);
  REQUIRE(toGuest[0]["player"] == kLookGuest);
  REQUIRE(toGuest[0]["target"] == kLookChest);
  REQUIRE(Sent(p, 0, MsgType::PlayerLookAsk).empty());
  LookLeave(p);
}

TEST_CASE("A look ask naming the asker itself or no connected player goes "
          "nowhere",
          "[PlayerLook][espm]")
{
  PartOne& p = GetPartOne();
  LookEnter(p);
  Ask(p, 0, kLookHost, kLookChest);
  Ask(p, 0, 0xff00fff0, kLookChest);
  REQUIRE(Sent(p, 0, MsgType::PlayerLookAsk).empty());
  REQUIRE(Sent(p, 1, MsgType::PlayerLookAsk).empty());
  LookLeave(p);
}

TEST_CASE("The guest's own look goes to the host of the target's cell, and a "
          "look naming another player goes nowhere",
          "[PlayerLook][espm]")
{
  PartOne& p = GetPartOne();
  LookEnter(p);
  Look(p, 1, kLookGuest, kLookChest, true);
  const auto toHost = Sent(p, 0, MsgType::PlayerLook);
  REQUIRE(toHost.size() == 1);
  REQUIRE(toHost[0]["player"] == kLookGuest);
  REQUIRE(toHost[0]["target"] == kLookChest);
  REQUIRE(toHost[0]["los"] == true);
  REQUIRE(Sent(p, 1, MsgType::PlayerLook).empty());
  p.Messages().clear();
  // a player's look is its own game's: the guest naming the host is dropped
  Look(p, 1, kLookHost, kLookChest, true);
  REQUIRE(Sent(p, 0, MsgType::PlayerLook).empty());
  // and the host's own look at a target its own game hosts goes nowhere
  Look(p, 0, kLookHost, kLookChest, false);
  REQUIRE(Sent(p, 0, MsgType::PlayerLook).empty());
  REQUIRE(Sent(p, 1, MsgType::PlayerLook).empty());
  LookLeave(p);
}
