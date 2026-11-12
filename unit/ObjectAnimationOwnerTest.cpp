#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

PartOne& GetPartOne();

// the fifty fixes' furniture rule (thuum docs/research/
// fifty-fixes-2026-11-07.md section 1(c), wire-rules object_animation): the
// owner of a world object's graph event is the engine that drives its graph,
// a furniture's recorded occupant's owner while it is used (a player's own
// seat, an NPC's simulator), else the cell's host; a furniture's event is
// relayed and kept nowhere. Bleak Falls Temple 0x371de's puzzle lever
// 0xaa8c3 (FURN 0xb7db4 LeverPushAnimatingNoPlayer), the scene's bandit
// 0x39fe1 who pulls it (unit/FurnitureOccupantTest's), and the portcullis
// 0x3b9f1, an ACTI some four hundred units off the lever
// (unit/ObjectAnimationTest's). The players stand where the loaded lever
// stands, never a typed position.
namespace {
constexpr uint32_t kHostActor = 0xff000ff3;
constexpr uint32_t kGuestActor = 0xff000ff4;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kLever = 0xaa8c3;
constexpr uint32_t kBandit = 0x39fe1;
constexpr uint32_t kPortcullis = 0x3b9f1;

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// `user` hosts `cell` (TrapRelayTest's shape)
void Host(PartOne& p, Networking::UserId user, uint32_t cell)
{
  const auto past = NowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  RawMessageData raw;
  raw.userId = user;
  CellLoadedMessage loaded;
  loaded.cell = cell;
  loaded.loaded = true;
  p.GetActionListener().OnCellLoaded(raw, loaded);
  p.TickCellHosts(past);
}

// the lever and the portcullis loaded, both players beside the lever, the
// host's game hosting the temple
MpObjectReference& Enter(PartOne& p)
{
  auto& lever = p.worldState.GetFormAt<MpObjectReference>(kLever);
  const auto base =
    p.worldState.GetEspm().GetBrowser().LookupById(lever.GetBaseId());
  REQUIRE(base.rec);
  REQUIRE(base.rec->GetType() == "FURN");
  p.worldState.GetFormAt<MpObjectReference>(kPortcullis);
  const NiPoint3 at = lever.GetPos();
  DoConnect(p, 0);
  p.CreateActor(kHostActor, at + NiPoint3{ 60.f, 0.f, 0.f }, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, at + NiPoint3{ -60.f, 0.f, 0.f }, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  for (uint32_t id : { kHostActor, kGuestActor }) {
    p.worldState.GetFormAt<MpActor>(id).ForceSubscriptionsUpdate();
  }
  Host(p, 0, kTemple);
  p.Messages().clear();
  return lever;
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHostActor);
  p.DestroyActor(kGuestActor);
}

void Animate(PartOne& p, Networking::UserId user, uint32_t refr,
             const char* event)
{
  ObjectAnimationMessage msg;
  msg.refr = refr;
  msg.event = event;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnObjectAnimation(raw, msg);
}

std::vector<ObjectAnimationMessage> RelayedTo(PartOne& p,
                                              Networking::UserId user)
{
  std::vector<ObjectAnimationMessage> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<ObjectAnimationMessage*>(m.message.get());
    if (m.userId == user && typed) {
      out.push_back(*typed);
    }
  }
  return out;
}

std::optional<std::string> LastAnimation(PartOne& p, uint32_t refr)
{
  return p.worldState.GetFormAt<MpObjectReference>(refr)
    .GetChangeForm()
    .lastAnimation;
}

// the reply that makes a player's game use the furniture
// (FurnitureOccupantTest's)
bool UseSentTo(PartOne& p, Networking::UserId user, uint32_t target)
{
  return std::any_of(
    p.Messages().begin(), p.Messages().end(), [&](PartOne::Message& m) {
      return m.userId == user && m.j.is_object() && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(MsgType::OpenContainer) &&
        m.j.contains("target") && m.j["target"] == target;
    });
}

// the default logger's lines at info and above while the guard lives
// (TrapRelayTest's shape), restored at its end
struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger =
      std::make_shared<spdlog::logger>("object-animation-owner-test", sink);
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
}

TEST_CASE("A furniture's user reports its graph event, relayed and kept "
          "nowhere; the host's echo of it is E_OBJANIM_NOT_USER",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  auto& lever = Enter(p);
  auto& guest = p.worldState.GetFormAt<MpActor>(kGuestActor);

  // the guest pulls the lever: the server's use records it as the occupant
  // and answers with the use (ProcessActivateNormal's FURN branch)
  lever.Activate(guest);
  REQUIRE(UseSentTo(p, 1, kLever));
  REQUIRE(lever.GetOccupant() == &guest);
  p.Messages().clear();

  // its engine plays the lever's push: the user's word, relayed to the host
  // and kept nowhere, so no late joiner or reload pushes it again
  {
    LogCapture log;
    Animate(p, 1, kLever, "Lever_PlayerPush");
    REQUIRE(log.Count("[verb:traps] aa8c3 animates aa8c3 'Lever_PlayerPush' "
                      "in cell 371de (its user ff000ff4 reports): relayed to "
                      "1 game(s), not kept") == 1);
    REQUIRE(log.Count("E_OBJANIM_") == 0);
  }
  REQUIRE(RelayedTo(p, 0).size() == 1);
  REQUIRE(RelayedTo(p, 0)[0].refr == kLever);
  REQUIRE(RelayedTo(p, 0)[0].event == "Lever_PlayerPush");
  REQUIRE(RelayedTo(p, 1).empty());
  REQUIRE_FALSE(LastAnimation(p, kLever).has_value());

  // the host's engine plays the relay: a report of it is not the user's
  p.Messages().clear();
  {
    LogCapture log;
    Animate(p, 0, kLever, "Lever_PlayerPush");
    REQUIRE(log.Count("[verb:traps] E_OBJANIM_NOT_USER: ff000ff3 reports "
                      "'Lever_PlayerPush' on aa8c3 in cell 371de, used by "
                      "ff000ff4, whose owner is ff000ff4") == 1);
    REQUIRE(log.Count("animates aa8c3") == 0);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE(RelayedTo(p, 1).empty());
  REQUIRE_FALSE(LastAnimation(p, kLever).has_value());
  Leave(p);
}

TEST_CASE("A guest's graph event on a world object nobody uses is still "
          "E_OBJANIM_NOT_HOST, and the host's on a furniture nobody uses is "
          "relayed and kept nowhere",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  auto& lever = Enter(p);
  REQUIRE(lever.GetOccupant() == nullptr);

  {
    LogCapture log;
    Animate(p, 1, kPortcullis, "Open");
    REQUIRE(log.Count("[verb:traps] E_OBJANIM_NOT_HOST: ff000ff4 reports "
                      "'Open' on 3b9f1 in cell 371de, which it does not "
                      "host") == 1);
    Animate(p, 1, kLever, "Lever_PlayerPush");
    REQUIRE(log.Count("[verb:traps] E_OBJANIM_NOT_HOST: ff000ff4 reports "
                      "'Lever_PlayerPush' on aa8c3 in cell 371de, which it "
                      "does not host") == 1);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE_FALSE(LastAnimation(p, kPortcullis).has_value());

  // the host's: the portcullis recorded as before, the lever kept nowhere
  {
    LogCapture log;
    Animate(p, 0, kPortcullis, "Open");
    REQUIRE(log.Count("[verb:traps] 3b9f1 animates 3b9f1 'Open' in cell "
                      "371de (its host ff000ff3 reports): recorded, relayed "
                      "to 1 game(s)") == 1);
    Animate(p, 0, kLever, "Lever_PlayerPush");
    REQUIRE(log.Count("[verb:traps] aa8c3 animates aa8c3 'Lever_PlayerPush' "
                      "in cell 371de (its host ff000ff3 reports): relayed to "
                      "1 game(s), not kept") == 1);
  }
  REQUIRE(LastAnimation(p, kPortcullis) == std::optional<std::string>("Open"));
  REQUIRE_FALSE(LastAnimation(p, kLever).has_value());
  REQUIRE(RelayedTo(p, 1).size() == 2);
  Leave(p);
}

TEST_CASE("An NPC user's simulator reports its furniture's graph event; a "
          "guest's word on it is E_OBJANIM_NOT_USER",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  auto& lever = Enter(p);
  auto& host = p.worldState.GetFormAt<MpActor>(kHostActor);

  // the scene's bandit takes the lever by the host's report of its use, as
  // in playtest 47; the host simulates it
  p.worldState.npcSettings["Skyrim.esm"].spawnInInterior = true;
  p.worldState.npcEnabled = true;
  auto& bandit = p.worldState.GetFormAt<MpActor>(kBandit);
  bandit.SetPos(lever.GetPos() + NiPoint3{ 0.f, 40.f, 0.f });
  constexpr bool kDefaultProcessingOnly = false;
  constexpr bool kSecondActivation = false;
  constexpr bool kReportedByHost = true;
  lever.Activate(bandit, kDefaultProcessingOnly, kSecondActivation,
                 kReportedByHost);
  REQUIRE(lever.GetOccupant() == &bandit);
  REQUIRE(p.worldState.HostOf(bandit) == &host);
  p.Messages().clear();

  {
    LogCapture log;
    Animate(p, 0, kLever, "Lever_PlayerPush");
    REQUIRE(log.Count("[verb:traps] aa8c3 animates aa8c3 'Lever_PlayerPush' "
                      "in cell 371de (its user 39fe1's simulator ff000ff3 "
                      "reports): relayed to 1 game(s), not kept") == 1);
  }
  REQUIRE(RelayedTo(p, 1).size() == 1);
  REQUIRE(RelayedTo(p, 1)[0].event == "Lever_PlayerPush");

  p.Messages().clear();
  {
    LogCapture log;
    Animate(p, 1, kLever, "Lever_PlayerPush");
    REQUIRE(log.Count("[verb:traps] E_OBJANIM_NOT_USER: ff000ff4 reports "
                      "'Lever_PlayerPush' on aa8c3 in cell 371de, used by "
                      "39fe1, whose owner is ff000ff3") == 1);
  }
  REQUIRE(RelayedTo(p, 0).empty());
  REQUIRE_FALSE(LastAnimation(p, kLever).has_value());
  Leave(p);
}
