#include "ActionListener.h"
#include "MapMarkerDiscoveredMessage.h"
#include "MpChangeForms.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include "libespm/REFR.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <simdjson.h>

PartOne& GetPartOne();

// thuum docs/verbs/map-markers.md: a player's engine discovers a location
// and its client reports only the marker's type; the server finds which
// marker from the master files and the player's position, records it on the
// player (R0, in its change form), and shows the player's markers on its
// map again after every login (AddToMap snippets, once its world is up).

namespace {
constexpr uint32_t kActor = 0xff000abd;

// The map marker nearest the lab spawn: Skyrim.esm REFR 0x00016223 at
// (133093, -54772, 9103) in Tamriel (lab/esm.py near, 2026-10-05)
constexpr uint32_t kMarker = 0x00016223;
const NiPoint3 kNearMarker{ 133093, -55272, 9103 }; // 500 units south

// The marker's own type, TNAM in the master file
uint16_t MarkerType(PartOne& p)
{
  const auto res = p.worldState.GetEspm().GetBrowser().LookupById(kMarker);
  REQUIRE(res.rec);
  const auto* refr = reinterpret_cast<const espm::REFR*>(res.rec);
  return static_cast<uint16_t>(
    refr->GetData(p.worldState.GetEspmCache()).mapMarkerType);
}

MpActor& PlayerAt(PartOne& p, const NiPoint3& pos)
{
  DoConnect(p, 0);
  p.CreateActor(kActor, pos, 0, 0x3c);
  p.SetUserActor(0, kActor);
  p.Messages().clear();
  return p.worldState.GetFormAt<MpActor>(kActor);
}

void Discover(PartOne& p, uint16_t markerType, bool canTravel)
{
  RawMessageData raw;
  raw.userId = 0;
  MapMarkerDiscoveredMessage msg;
  msg.markerType = markerType;
  msg.canTravel = canTravel;
  p.GetActionListener().OnMapMarkerDiscovered(raw, msg);
}

// The player's own movement report, standing where it is
void Stand(PartOne& p, const NiPoint3& pos)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = 0;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateMovementMessage msg;
  msg.idx = 0;
  msg.data.pos = { pos.x, pos.y, pos.z };
  msg.data.rot = { 0, 0, 0 };
  msg.data.isInJumpState = false;
  msg.data.isWeapDrawn = false;
  msg.data.isBlocking = false;
  msg.data.worldOrCell = 0x3c;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
}

// The AddToMap snippets user 0 received since the last clear, as
// (marker, canTravel)
std::vector<std::pair<uint32_t, bool>> Shown(PartOne& p)
{
  p.Tick(); // snippets are deferred
  std::vector<std::pair<uint32_t, bool>> out;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::SpSnippet &&
        m.j["class"] == "ObjectReference" && m.j["function"] == "AddToMap") {
      out.emplace_back(m.j["selfId"].get<uint32_t>(),
                       m.j["arguments"].at(0).get<bool>());
    }
  }
  return out;
}

void Leave(PartOne& p)
{
  p.DestroyActor(kActor);
  DoDisconnect(p, 0);
}
}

TEST_CASE("A discovery near a marker records it, and every login shows it",
          "[MapMarkers]")
{
  PartOne& p = GetPartOne();
  auto& ac = PlayerAt(p, kNearMarker);
  REQUIRE(ac.GetMapMarkers().empty());

  Discover(p, MarkerType(p), true);
  const auto recorded = ac.GetMapMarkers();
  REQUIRE(recorded.size() == 1);
  REQUIRE(recorded[0].refr.ToFormId(p.worldState.espmFiles) == kMarker);
  REQUIRE(recorded[0].canTravel);

  // nothing is shown until a login, and a login waits for the first
  // movement, when the client's world is up
  REQUIRE(Shown(p).empty());
  p.SetUserActor(0, kActor);
  p.Messages().clear();
  REQUIRE(Shown(p).empty());
  Stand(p, kNearMarker);
  REQUIRE(Shown(p) ==
          std::vector<std::pair<uint32_t, bool>>{ { kMarker, true } });

  // once per login
  p.Messages().clear();
  Stand(p, kNearMarker);
  REQUIRE(Shown(p).empty());

  Leave(p);
}

TEST_CASE("A discovery records each marker once, keeping fast travel",
          "[MapMarkers]")
{
  PartOne& p = GetPartOne();
  auto& ac = PlayerAt(p, kNearMarker);

  Discover(p, MarkerType(p), false);
  REQUIRE(ac.GetMapMarkers().size() == 1);
  REQUIRE(!ac.GetMapMarkers()[0].canTravel);

  Discover(p, MarkerType(p), true);
  REQUIRE(ac.GetMapMarkers().size() == 1);
  REQUIRE(ac.GetMapMarkers()[0].canTravel);

  // a report without fast travel never takes it away
  Discover(p, MarkerType(p), false);
  REQUIRE(ac.GetMapMarkers().size() == 1);
  REQUIRE(ac.GetMapMarkers()[0].canTravel);

  Leave(p);
}

TEST_CASE("A discovery with no marker of its type in range records nothing",
          "[MapMarkers]")
{
  PartOne& p = GetPartOne();
  auto& ac = PlayerAt(p, kNearMarker);

  // kDLC02CastleKarstaag (59, CommonLibSSE-NG ExtraMapMarker.h): only on
  // Solstheim, never in Tamriel
  Discover(p, 59, true);
  REQUIRE(ac.GetMapMarkers().empty());

  // the marker's own type, but far beyond the discovery range
  Leave(p);
  auto& far = PlayerAt(p,
                       { kNearMarker.x + 4 * skymp::rules::map_marker_range(),
                         kNearMarker.y, kNearMarker.z });
  Discover(p, MarkerType(p), true);
  for (const auto& marker : far.GetMapMarkers()) {
    REQUIRE(marker.refr.ToFormId(p.worldState.espmFiles) != kMarker);
  }

  Leave(p);
}

TEST_CASE("The change form keeps a player's markers, and older records read "
          "as none",
          "[MapMarkers]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;
  changeForm.mapMarkers =
    std::vector<MapMarker>{ { FormDesc::FromString("16223:Skyrim.esm"), true },
                            { FormDesc::FromString("1234:Dawnguard.esm"),
                              false } };

  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  const MpChangeForm restored = MpChangeForm::JsonToChangeForm(element);
  REQUIRE(restored.mapMarkers == changeForm.mapMarkers);

  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("mapMarkers"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement).mapMarkers);
}
