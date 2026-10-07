#include "ActionListener.h"
#include "MpChangeForms.h"
#include "RaceMenuPresetMessage.h"
#include "TestUtils.hpp"
#include "UpdateAppearanceMessage.h"
#include "UpdateMovementMessage.h"
#include <catch2/catch_all.hpp>
#include <simdjson.h>

PartOne& GetPartOne();

// thuum docs/verbs/racemenu-sync.md: a player's client reports its RaceMenu
// look, the preset RaceMenu saved, after the race menu closed. The server
// takes one look per race menu it opened (as it takes an appearance), records
// it when it is a bounded JSON object, hands it to every other player that
// shows the player, sends it with the player's figure to a client that gets
// the figure later, and sends the player its own after every login.

namespace {
// one actor per test case: the test server is shared
constexpr uint32_t kActor = 0xff000fb1;
constexpr uint32_t kActorOther = 0xff000fb2;
constexpr uint32_t kActorLogin = 0xff000fb3;
constexpr uint32_t kActorLate = 0xff000fb4;
constexpr uint32_t kActorLateOther = 0xff000fb5;
constexpr uint32_t kActorGate = 0xff000fb6;
constexpr uint32_t kActorGateOther = 0xff000fb7;
constexpr uint32_t kTamriel = 0x0000003c;

const std::string kPreset =
  R"({"version": {"formatVersion": 3}, "headParts": [], "actor": {}})";

// A player in the race menu the server opens for a new character
MpActor& Player(PartOne& p, Networking::UserId user, uint32_t actorId)
{
  DoConnect(p, user);
  p.CreateActor(actorId, { 0, 0, 0 }, 0, kTamriel);
  p.SetUserActor(user, actorId);
  p.SetRaceMenuOpen(actorId, true);
  p.Messages().clear();
  return p.worldState.GetFormAt<MpActor>(actorId);
}

// The player's appearance report, which closes the race menu (the raw
// packet it forwards to the neighbours is an empty object, as Stand's)
void Dress(PartOne& p, Networking::UserId user)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateAppearanceMessage msg;
  msg.idx = 0;
  msg.data = Appearance();
  msg.data->raceId = 0x00013746; // NordRace
  p.GetActionListener().OnUpdateAppearance(raw, msg);
}

void Report(PartOne& p, Networking::UserId user, const std::string& preset)
{
  RawMessageData raw;
  raw.userId = user;
  RaceMenuPresetMessage msg;
  msg.preset = preset;
  p.GetActionListener().OnRaceMenuPreset(raw, msg);
}

// The player's own movement report, standing at the origin
void Stand(PartOne& p, Networking::UserId user)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateMovementMessage msg;
  msg.idx = 0;
  msg.data.pos = { 0, 0, 0 };
  msg.data.rot = { 0, 0, 0 };
  msg.data.isInJumpState = false;
  msg.data.isWeapDrawn = false;
  msg.data.isBlocking = false;
  msg.data.worldOrCell = kTamriel;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
}

// The RaceMenuPreset messages `user` received since the last clear, each as
// (actor, preset)
std::vector<std::pair<uint32_t, std::string>> Sent(PartOne& p,
                                                   Networking::UserId user)
{
  std::vector<std::pair<uint32_t, std::string>> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j["t"] == MsgType::RaceMenuPreset) {
      out.emplace_back(m.j["actor"].get<uint32_t>(),
                       m.j["preset"].get<std::string>());
    }
  }
  return out;
}

void Leave(PartOne& p, Networking::UserId user, uint32_t actorId)
{
  p.DestroyActor(actorId);
  DoDisconnect(p, user);
}
}

TEST_CASE("A preset is recorded when it is a bounded JSON object and goes "
          "to the other players who see the player",
          "[RaceMenuPreset]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p, 0, kActor);
  Player(p, 1, kActorOther);

  Report(p, 0, kPreset);
  REQUIRE(ac.GetRaceMenuPreset() == kPreset);
  REQUIRE(
    Sent(p, 1) ==
    std::vector<std::pair<uint32_t, std::string>>{ { kActor, kPreset } });
  REQUIRE(Sent(p, 0).empty()); // not echoed to its own client

  // the same look again is not sent again
  p.Messages().clear();
  Report(p, 0, kPreset);
  REQUIRE(Sent(p, 1).empty());

  // anything but a JSON object leaves the record as it was
  for (const char* refused : { "[]", "1", "{", "\"x\"" }) {
    Report(p, 0, refused);
    REQUIRE(ac.GetRaceMenuPreset() == kPreset);
  }
  REQUIRE(Sent(p, 1).empty());

  Leave(p, 1, kActorOther);
  Leave(p, 0, kActor);
}

TEST_CASE("A login sends the player its own preset after the first "
          "movement",
          "[RaceMenuPreset]")
{
  PartOne& p = GetPartOne();
  Player(p, 0, kActorLogin);
  Report(p, 0, kPreset);

  p.SetUserActor(0, kActorLogin);
  p.Messages().clear();
  REQUIRE(Sent(p, 0).empty());
  Stand(p, 0);
  REQUIRE(
    Sent(p, 0) ==
    std::vector<std::pair<uint32_t, std::string>>{ { kActorLogin, kPreset } });

  Leave(p, 0, kActorLogin);
}

TEST_CASE("A client that gets a player's figure later gets its preset with "
          "it",
          "[RaceMenuPreset]")
{
  PartOne& p = GetPartOne();
  Player(p, 0, kActorLate);
  Report(p, 0, kPreset);

  // a second player arrives where the first stands: its client gets the
  // first's figure when it takes its actor
  DoConnect(p, 1);
  p.CreateActor(kActorLateOther, { 0, 0, 0 }, 0, kTamriel);
  p.Messages().clear();
  p.SetUserActor(1, kActorLateOther);
  auto sent = Sent(p, 1);
  REQUIRE(std::count(sent.begin(), sent.end(),
                     std::make_pair(kActorLate, kPreset)) == 1);

  Leave(p, 1, kActorLateOther);
  Leave(p, 0, kActorLate);
}

TEST_CASE("The change form keeps a player's preset, and older records read "
          "as none",
          "[RaceMenuPreset]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;
  changeForm.raceMenuPreset = kPreset;

  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(element).raceMenuPreset ==
          changeForm.raceMenuPreset);

  const std::string older = MpChangeForm::ToJson(MpChangeForm()).dump();
  REQUIRE(older.find("raceMenuPreset") == std::string::npos);
  auto olderElement = parser.parse(older).value();
  REQUIRE(
    !MpChangeForm::JsonToChangeForm(olderElement).raceMenuPreset.has_value());
}

TEST_CASE("A look is taken once per race menu the server opened, before or "
          "after the appearance that closes it, and not otherwise",
          "[RaceMenuPreset]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p, 0, kActorGate);
  Player(p, 1, kActorGateOther);
  const std::string second = R"({"version": {"formatVersion": 3}, "a": 1})";
  const std::string third = R"({"version": {"formatVersion": 3}, "a": 2})";

  // the look first, then the appearance
  Report(p, 0, kPreset);
  REQUIRE(ac.GetRaceMenuPreset() == kPreset);
  REQUIRE(ac.IsRaceMenuOpen());
  Dress(p, 0);
  REQUIRE(!ac.IsRaceMenuOpen());

  // a look from a race menu the player opened itself is refused
  p.Messages().clear();
  Report(p, 0, second);
  REQUIRE(ac.GetRaceMenuPreset() == kPreset);
  REQUIRE(Sent(p, 1).empty());

  // the appearance first, then the look
  p.SetRaceMenuOpen(kActorGate, true);
  Dress(p, 0);
  REQUIRE(!ac.IsRaceMenuOpen());
  p.Messages().clear();
  Report(p, 0, second);
  REQUIRE(ac.GetRaceMenuPreset() == second);
  REQUIRE(
    Sent(p, 1) ==
    std::vector<std::pair<uint32_t, std::string>>{ { kActorGate, second } });

  // one look per opening: a second one is refused
  p.Messages().clear();
  Report(p, 0, third);
  REQUIRE(ac.GetRaceMenuPreset() == second);
  REQUIRE(Sent(p, 1).empty());

  // a record that keeps the menu open shows it again at a login, so a look
  // is due again
  p.SetRaceMenuOpen(kActorGate, true);
  Report(p, 0, third);
  REQUIRE(ac.GetRaceMenuPreset() == third);
  auto changeForm = ac.GetChangeForm();
  REQUIRE(changeForm.isRaceMenuOpen);
  ac.ApplyChangeForm(changeForm);
  Report(p, 0, kPreset);
  REQUIRE(ac.GetRaceMenuPreset() == kPreset);

  Leave(p, 1, kActorGateOther);
  Leave(p, 0, kActorGate);
}
