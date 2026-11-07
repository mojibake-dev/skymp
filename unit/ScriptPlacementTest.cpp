#include "ActionListener.h"
#include "CreateActorMessage.h"
#include "Messages.h"
#include "MsgType.h"
#include "PlacedRequestMessage.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

PartOne& GetPartOne();

// thuum ADR-068 rung 4 (the script set, W3; docs/research/
// rung4-state-rows-2026-11-07.md section 1(c), placements): the owner of a
// reference reports its script's PlaceAtMe or PlaceActorAtMe as a batched
// state row (ScriptCall 75); the server opens a script set anchored at the
// self (wire-rules placements, Anchor::SelfRef), asks the reporter alone
// where its engine stood the base (PlacedRequest 61, the spells' message),
// and places its own copy at the reported pose (PlacedReport 62) within the
// band's farthest reach of the self, no floor: a server form whose create
// word names the reporter (placedBy) and whose record names the self and the
// base (placedByScript). A base that is no kind a script set places is
// E_PLACED_KIND; a window with no report places nothing; the set has no
// lifetime. The stage is ScriptCallTest's, Bleak Falls Temple 0x371de's
// portcullis 0x3b9f1 (an ACTI) at (4992, -1408, -768); the bases are
// Skyrim.esm's as lab/esm.py read them on sky-srv (2026-11-06): the STAT
// 0x3b XMarker, and the EXPL 0x10e4ae dunCGTrapFireTriggerExplosion01, a
// trap's explosion.
namespace {
using Arg = std::optional<
  std::variant<bool, double, std::string, SpSnippetObjectArgument>>;

constexpr uint32_t kHostActor = 0xff000c80;
constexpr uint32_t kGuestActor = 0xff000c81;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kPortcullis = 0x3b9f1;
constexpr uint32_t kXMarker = 0x3b;
constexpr uint32_t kTrapExplosion = 0x10e4ae;
const NiPoint3 kByThePortcullis{ 4992.f, -1508.f, -768.f };

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// the host (user 0) and the guest (user 1) by the portcullis, the host
// hosting the temple (RefStateTest's shape); returns the grant's epoch
uint32_t Enter(PartOne& p)
{
  p.worldState.GetFormAt<MpObjectReference>(kPortcullis);
  DoConnect(p, 0);
  p.CreateActor(kHostActor, kByThePortcullis, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, kByThePortcullis, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  for (uint32_t id : { kHostActor, kGuestActor }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(kByThePortcullis);
    actor.ForceSubscriptionsUpdate();
  }
  const auto past = NowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  RawMessageData raw;
  raw.userId = 0;
  CellLoadedMessage loaded;
  loaded.cell = kTemple;
  loaded.loaded = true;
  p.GetActionListener().OnCellLoaded(raw, loaded);
  p.TickCellHosts(past);
  REQUIRE(p.GetCellHost(kTemple).hosted);
  REQUIRE(p.GetCellHost(kTemple).player == kHostActor);
  p.Messages().clear();
  return p.GetCellHost(kTemple).epoch;
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHostActor);
  p.DestroyActor(kGuestActor);
}

// the portcullis's script's PlaceAtMe of `base`, reported by `user`'s game
void PlaceAtMe(PartOne& p, Networking::UserId user, uint32_t epoch,
               uint32_t base)
{
  ScriptCallMessage msg;
  msg.epoch = epoch;
  msg.selfId = kPortcullis;
  msg.cls = "ObjectReference";
  msg.fn = "PlaceAtMe";
  msg.args = { Arg(SpSnippetObjectArgument{ base, "Form" }), Arg(1.0),
               Arg(false), Arg(false) };
  msg.caller = "ScriptPlacementTest";
  msg.handler = "onActivate";
  msg.latent = false;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnScriptCall(raw, msg);
}

// a game's report of where its engine stands `base` for `set`, through the
// packet path from `user` (SpellPlacedTest's shape)
void Report(PartOne& p, Networking::UserId user, uint64_t set, uint32_t base,
            const NiPoint3& at, float yaw)
{
  DoMessage(
    p, user,
    nlohmann::json{ { "t", static_cast<int>(MsgType::PlacedReport) },
                    { "set", set },
                    { "base", base },
                    { "pos", nlohmann::json::array({ at.x, at.y, at.z }) },
                    { "yaw", yaw } });
}

std::vector<PlacedRequestMessage> RequestsTo(PartOne& p,
                                             Networking::UserId user)
{
  std::vector<PlacedRequestMessage> out;
  for (auto& m : p.Messages()) {
    auto ask = dynamic_cast<PlacedRequestMessage*>(m.message.get());
    if (m.userId == user && ask) {
      out.push_back(*ask);
    }
  }
  return out;
}

// the CreateActors naming a placer that went to `user` since the last clear
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
    auto logger =
      std::make_shared<spdlog::logger>("script-placement-test", sink);
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

TEST_CASE("A host's scripted PlaceAtMe asks the reporter alone where its "
          "engine stood the base, and the report places the server's copy "
          "there, named to every subscriber with its placer and recorded "
          "with its self and base",
          "[ScriptPlacement][espm]")
{
  auto& p = GetPartOne();
  const uint32_t epoch = Enter(p);
  const auto& portcullis =
    p.worldState.GetFormAt<MpObjectReference>(kPortcullis);
  const NiPoint3 anchor = portcullis.GetPos();

  {
    LogCapture log;
    PlaceAtMe(p, 0, epoch, kXMarker);
    REQUIRE(log.Count("[verb:spell-placed] ff000c80's script placed "
                      "3b:Skyrim.esm (STAT) at 3b9f1: set ") == 1);
  }
  const auto asked = RequestsTo(p, 0);
  REQUIRE(asked.size() == 1);
  REQUIRE(asked[0].bases == std::vector<uint32_t>{ kXMarker });
  REQUIRE(asked[0].windowMs == 6000);
  REQUIRE(RequestsTo(p, 1).empty());
  // nothing stands before the report
  REQUIRE(PlacedTo(p, 1).empty());

  // the engine stood it 100 units east of the self, at its height, yaw 90
  p.Messages().clear();
  const NiPoint3 at = anchor + NiPoint3{ 100.f, 0.f, 0.f };
  {
    LogCapture log;
    Report(p, 0, asked[0].set, kXMarker, at, 90.f);
    REQUIRE(log.Count("[verb:spell-placed] ff000c80's script placed "
                      "3b:Skyrim.esm (STAT) at 3b9f1: ff") == 1);
  }
  const auto toGuest = PlacedTo(p, 1);
  REQUIRE(toGuest.size() == 1);
  const auto& create = toGuest[0];
  REQUIRE(create.props.placedBy == kHostActor);
  REQUIRE(create.baseId == kXMarker);
  REQUIRE(create.baseRecordType == "STAT");
  REQUIRE(create.transform.worldOrCell == kTemple);
  REQUIRE(create.transform.pos[0] == Catch::Approx(at.x).margin(0.01));
  REQUIRE(create.transform.pos[1] == Catch::Approx(at.y).margin(0.01));
  REQUIRE(create.transform.pos[2] == Catch::Approx(at.z).margin(0.01));
  REQUIRE(create.transform.rot[2] == Catch::Approx(90.f).margin(0.01));
  // the reporter's own game hears the same create word, and skips it itself
  const auto toHost = PlacedTo(p, 0);
  REQUIRE(toHost.size() == 1);
  REQUIRE(toHost[0].refrId == create.refrId);

  REQUIRE(create.refrId.has_value());
  const auto formId = static_cast<uint32_t>(*create.refrId);
  REQUIRE(formId >= 0xff000000);
  auto& placed = p.worldState.GetFormAt<MpObjectReference>(formId);
  REQUIRE(placed.GetBaseType() == "STAT");
  const auto record = placed.GetPlacedByScript();
  REQUIRE(record.has_value());
  REQUIRE(record->self ==
          FormDesc::FromFormId(kPortcullis, p.worldState.espmFiles));
  REQUIRE(record->base ==
          FormDesc::FromFormId(kXMarker, p.worldState.espmFiles));
  REQUIRE(!placed.GetPlacedBySpell());

  // the set is done: a second report of it is no set's
  p.Messages().clear();
  Report(p, 0, asked[0].set, kXMarker, at, 90.f);
  REQUIRE(PlacedTo(p, 1).empty());
  Leave(p);
}

TEST_CASE("A script set refuses a base of no kind it places and a guest's "
          "call, and a window that closes with no report in the band places "
          "nothing",
          "[ScriptPlacement][espm]")
{
  auto& p = GetPartOne();
  const uint32_t epoch = Enter(p);
  const NiPoint3 anchor =
    p.worldState.GetFormAt<MpObjectReference>(kPortcullis).GetPos();

  {
    LogCapture log;
    PlaceAtMe(p, 0, epoch, kTrapExplosion);
    REQUIRE(log.Count("[verb:spell-placed] E_PLACED_KIND: ff000c80's script "
                      "placed 10e4ae:Skyrim.esm (EXPL) at 3b9f1; nothing "
                      "placed") == 1);
  }
  REQUIRE(RequestsTo(p, 0).empty());

  // the guest's engine owns nothing in the host's cell
  {
    LogCapture log;
    PlaceAtMe(p, 1, epoch, kXMarker);
    REQUIRE(log.Count("[verb:scripts] E_SCRIPTCALL_OWNER: ff000c81 reports "
                      "ObjectReference.PlaceAtMe on 3b9f1") == 1);
  }
  REQUIRE(RequestsTo(p, 1).empty());

  // a report past the band's farthest reach (320 on the ground plane) is
  // refused, and the window then closes with nothing placed
  const uint64_t t0 = NowMs();
  PlaceAtMe(p, 0, epoch, kXMarker);
  const auto asked = RequestsTo(p, 0);
  REQUIRE(asked.size() == 1);
  p.Messages().clear();
  {
    LogCapture log;
    Report(p, 0, asked[0].set, kXMarker, anchor + NiPoint3{ 400.f, 0.f, 0.f },
           0.f);
    REQUIRE(log.Count("[verb:spell-placed] E_PLACED_RANGE: ff000c80 reports "
                      "set ") == 1);
  }
  REQUIRE(PlacedTo(p, 1).empty());
  {
    LogCapture log;
    p.TickPlacements(t0 + 7'000);
    REQUIRE(log.Count("[verb:spell-placed] ff000c80's script placed "
                      "3b:Skyrim.esm at 3b9f1: nothing placed, its game "
                      "reported no base within the window") == 1);
  }
  REQUIRE(PlacedTo(p, 0).empty());
  REQUIRE(PlacedTo(p, 1).empty());
  Leave(p);
}
