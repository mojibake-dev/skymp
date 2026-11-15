#include "ActionListener.h"
#include "CreateActorMessage.h"
#include "Messages.h"
#include "MsgType.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

PartOne& GetPartOne();

// thuum ADR-073 decision 13, the spell channel's first slice (wire-rules
// created; docs/verbs/spell-placed-object.md; #54, Raise Wall): a script on
// a seat places a reference with PlaceAtMe on that seat's own player and
// moves, shows and deletes it with the engine's own natives, each reported
// as its engine ran it and naming the created reference by the seat's local
// id. The server places its copy (placedBy the creator), keeps its pose and
// state, and relays each call as the same native to the copy's listeners
// with the copy's sequence; another seat naming the id names nothing of its
// own. Raise Wall's order, as x-spell-calls 20261115-034842 read it on c1:
// PlaceAtMe disabled, MoveTo 192 ahead and 288 below the player, Enable,
// TranslateTo, and a placement on the created reference. The stage is
// ScriptPlacementTest's, Bleak Falls Temple 0x371de; the bases are
// Skyrim.esm's (lab/esm.py): the STAT 0x3b XMarker for the wall, the EXPL
// 0x10e4ae for the explosion each engine places itself.
namespace {
using Arg = std::optional<
  std::variant<bool, double, std::string, SpSnippetObjectArgument>>;

constexpr uint32_t kHostActor = 0xff000c80;
constexpr uint32_t kGuestActor = 0xff000c81;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kXMarker = 0x3b;
constexpr uint32_t kTrapExplosion = 0x10e4ae;
// the caster's local ids, as its engine numbers them
constexpr uint32_t kWallLocal = 0xff0008fa;
constexpr uint32_t kBoomLocal = 0xff000990;
const NiPoint3 kInTheTemple{ 4992.f, -1508.f, -768.f };

void Enter(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kHostActor, kInTheTemple, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, kInTheTemple, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  for (uint32_t id : { kHostActor, kGuestActor }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(kInTheTemple);
    actor.ForceSubscriptionsUpdate();
  }
  p.Messages().clear();
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHostActor);
  p.DestroyActor(kGuestActor);
}

void Report(PartOne& p, Networking::UserId user, uint32_t self, const char* fn,
            std::vector<Arg> args,
            std::optional<uint32_t> placed = std::nullopt,
            std::optional<uint32_t> localArgs = std::nullopt)
{
  ScriptCallMessage msg;
  msg.selfId = self;
  msg.cls = "ObjectReference";
  msg.fn = fn;
  msg.args = std::move(args);
  msg.caller = "WB_FabricateWall_Script";
  msg.handler = "OnEffectStart";
  msg.placed = placed;
  msg.localArgs = localArgs;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnScriptCall(raw, msg);
}

Arg ObjectArg(uint32_t id, const char* type)
{
  SpSnippetObjectArgument object;
  object.formId = id;
  object.type = type;
  return Arg(object);
}

std::vector<nlohmann::json> SnippetsTo(PartOne& p, Networking::UserId user)
{
  std::vector<nlohmann::json> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j["t"] == static_cast<int>(MsgType::SpSnippet)) {
      out.push_back(m.j);
    }
  }
  return out;
}

std::vector<CreateActorMessage> PlacedTo(PartOne& p, Networking::UserId user)
{
  std::vector<CreateActorMessage> out;
  for (auto& m : p.Messages()) {
    if (m.userId != user) {
      continue;
    }
    auto create = dynamic_cast<CreateActorMessage*>(m.message.get());
    if (create && create->props.placedBy.has_value()) {
      out.push_back(*create);
    }
  }
  return out;
}

struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("created-test", sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
  }
  ~LogCapture() { spdlog::set_default_logger(old); }
  bool Has(const std::string& line) const
  {
    return oss.str().find(line) != std::string::npos;
  }
};
}

TEST_CASE("A spell's wall is the caster's: placed, moved, shown, risen and "
          "deleted on every screen as the caster's script did it",
          "[CreatedChannel][espm]")
{
  auto& p = GetPartOne();
  Enter(p);
  LogCapture log;

  // PlaceAtMe(wall, 1, false, true) on the caster's own player: a copy,
  // off every screen until its first Enable
  Report(p, 0, 0x14, "PlaceAtMe",
         { ObjectArg(kXMarker, "Static"), Arg(1.0), Arg(false), Arg(true) },
         kWallLocal);
  const auto bound = p.GetCreatedRefs().created_local(kHostActor, kWallLocal);
  REQUIRE(bound.accepted);
  const uint32_t copy = bound.copy;
  REQUIRE(copy >= 0xff000000);
  REQUIRE(log.Has("ff000c80's script placed 3b (STAT) on 14"));
  REQUIRE(log.Has("off every screen until its first Enable"));
  REQUIRE(PlacedTo(p, 1).empty());

  // MoveTo(player, 0, 192, -288, true): the copy's record stands there
  Report(p, 0, kWallLocal, "MoveTo",
         { ObjectArg(0x14, "Actor"), Arg(0.0), Arg(192.0), Arg(-288.0),
           Arg(true) });
  const auto& wall = p.worldState.GetFormAt<MpObjectReference>(copy);
  REQUIRE(wall.GetPos().x == Catch::Approx(kInTheTemple.x));
  REQUIRE(wall.GetPos().y == Catch::Approx(kInTheTemple.y + 192.f));
  REQUIRE(wall.GetPos().z == Catch::Approx(kInTheTemple.z - 288.f));

  // Enable(false): the copy reaches the other seat, named as the caster's
  // (the caster's game hears it too and keeps its engine's own reference)
  p.Messages().clear();
  Report(p, 0, kWallLocal, "Enable", { Arg(false) });
  const auto created = PlacedTo(p, 1);
  REQUIRE(created.size() == 1);
  REQUIRE(*created[0].props.placedBy == kHostActor);

  // TranslateTo: the rise, relayed as itself on the copy with its sequence,
  // to the other seat alone
  p.Messages().clear();
  Report(p, 0, kWallLocal, "TranslateTo",
         { Arg(4992.0), Arg(-1316.0), Arg(-816.0), Arg(0.0), Arg(0.0),
           Arg(0.0), Arg(512.0), Arg(0.0) });
  const auto rise = SnippetsTo(p, 1);
  REQUIRE(rise.size() == 1);
  REQUIRE(rise[0]["function"] == "TranslateTo");
  REQUIRE(rise[0]["selfId"] == copy);
  REQUIRE(rise[0]["seq"] == 3);
  REQUIRE(SnippetsTo(p, 0).empty());
  REQUIRE(wall.GetPos().z == Catch::Approx(-816.f));

  // the other seat naming the same local number names nothing of its own
  p.Messages().clear();
  Report(p, 1, kWallLocal, "TranslateTo",
         { Arg(0.0), Arg(0.0), Arg(0.0), Arg(0.0), Arg(0.0), Arg(0.0),
           Arg(512.0), Arg(0.0) });
  // (the channel binds the id for its creator alone, so the table judges the
  // other seat's call and refuses it)
  REQUIRE(
    log.Has(": ff000c81 reports ObjectReference.TranslateTo on ff0008fa"));
  REQUIRE(log.Has("refused"));
  REQUIRE(SnippetsTo(p, 0).empty());

  // an explosion placed on the created reference: each engine places its
  // own, the call relayed on the copy
  p.Messages().clear();
  Report(p, 0, kWallLocal, "PlaceAtMe",
         { ObjectArg(kTrapExplosion, "Explosion"), Arg(1.0), Arg(false),
           Arg(false) },
         kBoomLocal);
  const auto boom = SnippetsTo(p, 1);
  REQUIRE(boom.size() == 1);
  REQUIRE(boom[0]["function"] == "PlaceAtMe");
  REQUIRE(boom[0]["selfId"] == copy);
  REQUIRE(log.Has("a one-shot, relayed to 1 game(s)"));

  // Delete: the copy falls on every screen, unbound
  Report(p, 0, kWallLocal, "Delete", {});
  REQUIRE(wall.IsDeleted());
  REQUIRE_FALSE(
    p.GetCreatedRefs().created_local(kHostActor, kWallLocal).accepted);

  Leave(p);
}

TEST_CASE("A creator's copies go with it", "[CreatedChannel][espm]")
{
  auto& p = GetPartOne();
  Enter(p);
  Report(p, 0, 0x14, "PlaceAtMe",
         { ObjectArg(kXMarker, "Static"), Arg(1.0), Arg(false), Arg(false) },
         kWallLocal);
  const auto bound = p.GetCreatedRefs().created_local(kHostActor, kWallLocal);
  REQUIRE(bound.accepted);
  const auto& wall = p.worldState.GetFormAt<MpObjectReference>(bound.copy);
  REQUIRE_FALSE(wall.IsDeleted());
  DoDisconnect(p, 0);
  REQUIRE(wall.IsDeleted());
  REQUIRE(p.GetCreatedRefs().created_len() == 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHostActor);
  p.DestroyActor(kGuestActor);
}
