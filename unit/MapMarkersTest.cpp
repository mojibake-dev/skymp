#include "ActionListener.h"
#include "MapMarkerDiscoveredMessage.h"
#include "Messages.h"
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

// thuum ADR-073 step 4's S6 (Eli's D-NT4 A; D-PO1 B and D-G6 A): a quest
// script's AddToMap. Skyrim.esm by lab/esm.py, 2026-11-15:
// FreeformRiverwood01 0x74a83 attaches QF_FreeformRiverwood01_00074A83
// (DNAM main flags 0x00); MS13CamillaStage10Scene, SCEN 0x383a3 (PNAM MS13
// 0x39645), attaches SF_MS13CamillaStage10Scene_000383A3, which names
// BleakFallsMapMarker 0x16232; Riverwood's rumor, INFO 0x24528 (DIAL
// 0x24390, QNAM DA01Intro 0x92e25, a Start Game Enabled quest, DNAM main
// flags 0x11), attaches TIF__00024528, which names MapMarker 0x92496, the
// Shrine of Azura's; 0x16232 and 0x92496 are REFRs of the MapMarker STAT
// 0x10. MS10MiscObjPointer 0x10d3bb attaches QF_MS10MiscObjPointer_0010D3BB
// (DNAM main flags 0x00, so no start of its own); the ore vein 0x10e6c5 is
// an ACTI's REFR, no marker
constexpr uint32_t kOtherActor = 0xff000abe;
constexpr uint32_t kBleakFalls = 0x00016232;
constexpr uint32_t kAzura = 0x00092496;
constexpr uint32_t kOreVein = 0x0010e6c5;

// a second player beside the first, user 1
MpActor& OtherAt(PartOne& p, const NiPoint3& pos)
{
  DoConnect(p, 1);
  p.CreateActor(kOtherActor, pos, 0, 0x3c);
  p.SetUserActor(1, kOtherActor);
  p.Messages().clear();
  return p.worldState.GetFormAt<MpActor>(kOtherActor);
}

// `user`'s game reports the script call ObjectReference.AddToMap(travel) on
// `marker` from `caller`, its fragment
void ReportAddToMap(PartOne& p, Networking::UserId user, uint32_t marker,
                    const char* caller, bool travel)
{
  ScriptCallMessage msg;
  msg.epoch = 0;
  msg.selfId = marker;
  msg.cls = "ObjectReference";
  msg.fn = "AddToMap";
  msg.args.push_back(
    std::optional<
      std::variant<bool, double, std::string, SpSnippetObjectArgument>>(
      travel));
  msg.caller = caller;
  msg.handler = "Fragment_0";
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnScriptCall(raw, msg);
}

// `user`'s own movement report, standing at `pos` (Stand's, by the actor's
// own idx)
void StandAs(PartOne& p, Networking::UserId user, MpActor& actor,
             const NiPoint3& pos)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateMovementMessage msg;
  msg.idx = actor.GetIdx();
  msg.data.pos = { pos.x, pos.y, pos.z };
  msg.data.rot = { 0, 0, 0 };
  msg.data.isInJumpState = false;
  msg.data.isWeapDrawn = false;
  msg.data.isBlocking = false;
  msg.data.worldOrCell = 0x3c;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
}

// the AddToMap snippets `user` received since the last clear, as (marker,
// canTravel), in order
std::vector<std::pair<uint32_t, bool>> ShownTo(PartOne& p,
                                               Networking::UserId user)
{
  p.Tick(); // a login's snippets are deferred, a relay's sent at once
  std::vector<std::pair<uint32_t, bool>> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j["t"] == MsgType::SpSnippet &&
        m.j["class"] == "ObjectReference" && m.j["function"] == "AddToMap") {
      out.emplace_back(m.j["selfId"].get<uint32_t>(),
                       m.j["arguments"].at(0).get<bool>());
    }
  }
  return out;
}

MapMarker Marker(PartOne& p, uint32_t refr, bool canTravel)
{
  return MapMarker{ FormDesc::FromFormId(refr, p.worldState.espmFiles),
                    canTravel };
}

// the shared server's quest records and the world's markers back to none,
// and both players gone
void LeaveBoth(PartOne& p)
{
  p.SetWorldMapMarkers({});
  p.SetQuestStages({});
  p.SetQuestRuns({});
  p.SetSharedQuestsSettings("[]");
  p.DestroyActor(kOtherActor);
  DoDisconnect(p, 1);
  Leave(p);
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

TEST_CASE("A world quest's AddToMap is recorded for every player, relayed "
          "live once and served at a later login",
          "[MapMarkers]")
{
  PartOne& p = GetPartOne();
  // no quest a player's class (QuestScopeTest's last setting), the two
  // listed quests the world's
  p.SetQuestScopeSettings(R"("off")", "[]", "[]");
  p.SetSharedQuestsSettings(R"(["74a83:Skyrim.esm", "39645:Skyrim.esm"])");
  p.SetQuestStages({ { FormDesc::FromString("74a83:Skyrim.esm"), 10 },
                     { FormDesc::FromString("39645:Skyrim.esm"), 10 } });
  p.SetWorldMapMarkers({});
  auto& reporter = PlayerAt(p, kNearMarker);
  auto& other = OtherAt(p, kNearMarker);

  // the quest's fragment on the clearing's marker: the world's record, the
  // other player shown it at once, the reporter (whose engine ran it) not
  ReportAddToMap(p, 0, kMarker, "QF_FreeformRiverwood01_00074A83", true);
  REQUIRE(p.GetWorldMapMarkers() ==
          std::vector<MapMarker>{ Marker(p, kMarker, true) });
  REQUIRE(reporter.GetMapMarkers().empty());
  REQUIRE(ShownTo(p, 1) ==
          std::vector<std::pair<uint32_t, bool>>{ { kMarker, true } });
  REQUIRE(ShownTo(p, 0).empty());

  // the same report again, or another seat's run of the fragment: folded,
  // relayed to no one
  p.Messages().clear();
  ReportAddToMap(p, 0, kMarker, "QF_FreeformRiverwood01_00074A83", true);
  ReportAddToMap(p, 1, kMarker, "QF_FreeformRiverwood01_00074A83", false);
  REQUIRE(p.GetWorldMapMarkers() ==
          std::vector<MapMarker>{ Marker(p, kMarker, true) });
  REQUIRE(ShownTo(p, 0).empty());
  REQUIRE(ShownTo(p, 1).empty());

  // a scene's fragment (MS13's Camilla scene, MS13 staged): the scene's
  // quest's marker, from the other seat, relayed to the first
  ReportAddToMap(p, 1, kBleakFalls, "SF_MS13CamillaStage10Scene_000383A3",
                 false);
  REQUIRE(p.GetWorldMapMarkers() ==
          std::vector<MapMarker>{ Marker(p, kMarker, true),
                                  Marker(p, kBleakFalls, false) });
  REQUIRE(ShownTo(p, 0) ==
          std::vector<std::pair<uint32_t, bool>>{ { kBleakFalls, false } });
  REQUIRE(other.GetMapMarkers().empty());

  // a later login is shown the world's markers beside its own, each once
  // with the wider travel flag
  REQUIRE(reporter.RecordMapMarker(Marker(p, kBleakFalls, true).refr, true));
  p.Messages().clear();
  p.SetUserActor(0, kActor);
  StandAs(p, 0, reporter, kNearMarker);
  REQUIRE(ShownTo(p, 0) ==
          std::vector<std::pair<uint32_t, bool>>{ { kBleakFalls, true },
                                                  { kMarker, true } });

  LeaveBoth(p);
}

TEST_CASE("A quest marker needs a map marker, a cause and a live quest, and "
          "a quest each seat keeps goes on its reporter's map alone",
          "[MapMarkers]")
{
  PartOne& p = GetPartOne();
  // mode list with nothing listed and no quest a player's class: every
  // quest each seat's own, and live only by the journal or its start
  p.SetQuestScopeSettings(R"("off")", "[]", "[]");
  p.SetSharedQuestsSettings("[]");
  p.SetQuestStages({});
  p.SetQuestRuns({});
  p.SetWorldMapMarkers({});
  auto& reporter = PlayerAt(p, kNearMarker);
  auto& other = OtherAt(p, kNearMarker);

  // no map marker by the master files (E_SCRIPTCALL_ROW), no cause
  // (E_SCRIPTCALL_OWNER: a class no QUST, INFO or SCEN attaches), a quest
  // the server holds no run, stage or start of (E_SCRIPTCALL_OWNER)
  ReportAddToMap(p, 0, kOreVein, "TIF__00024528", true);
  ReportAddToMap(p, 0, kAzura, "defaultPuzzlePillarScript", true);
  ReportAddToMap(p, 0, kBleakFalls, "QF_MS10MiscObjPointer_0010D3BB", true);
  REQUIRE(reporter.GetMapMarkers().empty());
  REQUIRE(p.GetWorldMapMarkers().empty());
  REQUIRE(ShownTo(p, 1).empty());

  // Riverwood's rumor: a topic info's fragment, its topic's quest Start Game
  // Enabled and each seat's own here, so the reporter's own map alone
  ReportAddToMap(p, 0, kAzura, "TIF__00024528", true);
  REQUIRE(reporter.GetMapMarkers() ==
          std::vector<MapMarker>{ Marker(p, kAzura, true) });
  REQUIRE(p.GetWorldMapMarkers().empty());
  REQUIRE(ShownTo(p, 1).empty());

  // the journal's stop of that quest ends its start's life
  REQUIRE(p.SetQuestRuns(
    { { FormDesc::FromString("92e25:Skyrim.esm"), false, 1 } }));
  ReportAddToMap(p, 1, kAzura, "TIF__00024528", true);
  REQUIRE(other.GetMapMarkers().empty());

  LeaveBoth(p);
}

TEST_CASE("The world journal keeps a world quest's markers in its change "
          "form",
          "[MapMarkers]")
{
  PartOne& p = GetPartOne();
  p.SetWorldMapMarkers({});
  REQUIRE(p.RecordWorldMapMarker(Marker(p, kAzura, false).refr, false));
  REQUIRE(p.RecordWorldMapMarker(Marker(p, kAzura, true).refr, true));
  REQUIRE(!p.RecordWorldMapMarker(Marker(p, kAzura, false).refr, false));
  REQUIRE(p.GetWorldMapMarkers() ==
          std::vector<MapMarker>{ Marker(p, kAzura, true) });

  // what a restart reads back: the journal's change form, through its JSON
  const MpChangeForm saved = p.WorldJournal().GetChangeForm();
  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(saved).dump();
  auto element = parser.parse(dump).value();
  const MpChangeForm restored = MpChangeForm::JsonToChangeForm(element);
  REQUIRE(restored.mapMarkers ==
          std::optional<std::vector<MapMarker>>(
            std::vector<MapMarker>{ Marker(p, kAzura, true) }));

  REQUIRE(p.SetWorldMapMarkers({}));
  REQUIRE(p.GetWorldMapMarkers().empty());
}
