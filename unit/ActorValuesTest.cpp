#include "ActionListener.h"
#include "ActorValuesMessage.h"
#include "MpChangeForms.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include "script_classes/PapyrusActor.h"
#include <catch2/catch_all.hpp>
#include <simdjson.h>

PartOne& GetPartOne();

// thuum docs/verbs/actor-values.md: a player's client reports its actor
// values and progress after a skill or level increase. The server records a
// report within bounds, holds a value it set until a report carries it, sends
// the record after every login, and holds that record until a report shows
// it applied.

namespace {
// one actor per test case: the test server is shared
constexpr uint32_t kActor = 0xff000fc1;
constexpr uint32_t kActorLogin = 0xff000fc2;
constexpr uint32_t kActorHeld = 0xff000fc3;
constexpr uint32_t kActorNative = 0xff000fc4;
constexpr uint32_t kActorFresh = 0xff000fc5;
constexpr uint32_t kTamriel = 0x0000003c;

// the engine's actor values (CommonLibSSE-NG include/RE/A/ActorValues.h)
constexpr uint8_t kOneHanded = 6;
constexpr uint8_t kTwoHanded = 7;
constexpr uint8_t kHealth = 24;
// Archery: its Papyrus name is Marksman (wire-rules actor_values::NAMES)
constexpr uint8_t kArchery = 8;

MpActor& Player(PartOne& p, uint32_t actorId)
{
  DoConnect(p, 0);
  p.CreateActor(actorId, { 0, 0, 0 }, 0, kTamriel);
  p.SetUserActor(0, actorId);
  p.Messages().clear();
  return p.worldState.GetFormAt<MpActor>(actorId);
}

ActorValuesMessage Snapshot(std::vector<std::pair<uint8_t, float>> bases,
                            uint16_t level)
{
  ActorValuesMessage msg;
  for (const auto& [av, base] : bases) {
    msg.bases.push_back(ActorValuesMessage::Base{ av, base });
  }
  msg.skills.push_back(ActorValuesMessage::Skill{ 0, 15.f, 2.f, 10.f });
  msg.xp = 10.f;
  msg.threshold = 75.f;
  msg.level = level;
  return msg;
}

void Report(PartOne& p, const ActorValuesMessage& msg)
{
  RawMessageData raw;
  raw.userId = 0;
  p.GetActionListener().OnActorValues(raw, msg);
}

std::vector<std::pair<uint8_t, float>> RecordedBases(MpActor& ac)
{
  const auto record = ac.GetActorValueRecord();
  return record ? record->bases : std::vector<std::pair<uint8_t, float>>();
}

// The player's own movement report, standing at the origin
void Stand(PartOne& p)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = 0;
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

// The ActorValues messages user 0 received since the last clear, each as
// its level
// the Papyrus functions the server sent user 0 to run in its own game
std::vector<std::string> Snippets(PartOne& p)
{
  std::vector<std::string> out;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::SpSnippet) {
      out.push_back(m.j["function"].get<std::string>());
    }
  }
  return out;
}

std::vector<uint16_t> SentLevels(PartOne& p)
{
  std::vector<uint16_t> out;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::ActorValues) {
      out.push_back(m.j["level"].get<uint16_t>());
    }
  }
  return out;
}

void Leave(PartOne& p, uint32_t actorId)
{
  p.DestroyActor(actorId);
  DoDisconnect(p, 0);
}
}

TEST_CASE("A report within bounds is recorded, one out of bounds is not",
          "[ActorValues]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p, kActor);

  Report(p, Snapshot({ { kOneHanded, 16.f }, { kHealth, 110.f } }, 2));
  REQUIRE(RecordedBases(ac) ==
          std::vector<std::pair<uint8_t, float>>{ { kOneHanded, 16.f },
                                                  { kHealth, 110.f } });
  REQUIRE(ac.GetActorValueRecord()->level == 2);

  // a skill past play's ceiling, and a level of 0
  Report(p, Snapshot({ { kOneHanded, 101.f } }, 2));
  Report(p, Snapshot({ { kOneHanded, 17.f } }, 0));
  REQUIRE(RecordedBases(ac) ==
          std::vector<std::pair<uint8_t, float>>{ { kOneHanded, 16.f },
                                                  { kHealth, 110.f } });

  Leave(p, kActor);
}

TEST_CASE("A login sends the record and holds it until a report shows it "
          "applied",
          "[ActorValues]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p, kActorLogin);
  Report(p, Snapshot({ { kOneHanded, 40.f } }, 12));

  p.SetUserActor(0, kActorLogin);
  p.Messages().clear();
  REQUIRE(SentLevels(p).empty());
  Stand(p);
  REQUIRE(SentLevels(p) == std::vector<uint16_t>{ 12 });

  // a fresh session's values, reported before the record was applied
  Report(p, Snapshot({ { kOneHanded, 15.f } }, 1));
  REQUIRE(RecordedBases(ac) ==
          std::vector<std::pair<uint8_t, float>>{ { kOneHanded, 40.f } });

  // applied, and One-Handed raised since by play
  Report(p, Snapshot({ { kOneHanded, 41.f } }, 12));
  REQUIRE(RecordedBases(ac) ==
          std::vector<std::pair<uint8_t, float>>{ { kOneHanded, 41.f } });

  Leave(p, kActorLogin);
}

TEST_CASE("A value the server set holds until a report carries it",
          "[ActorValues]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p, kActorHeld);
  Report(p, Snapshot({ { kTwoHanded, 15.f } }, 1));

  // the server sets Two-Handed to 50 (as a native would)
  ac.SetHeldActorValues({ { kTwoHanded, 50.f } });
  Report(p, Snapshot({ { kTwoHanded, 15.f } }, 1));
  REQUIRE(RecordedBases(ac) ==
          std::vector<std::pair<uint8_t, float>>{ { kTwoHanded, 50.f } });
  REQUIRE(!ac.GetHeldActorValues().empty());

  Report(p, Snapshot({ { kTwoHanded, 50.f } }, 1));
  REQUIRE(ac.GetHeldActorValues().empty());
  REQUIRE(RecordedBases(ac) ==
          std::vector<std::pair<uint8_t, float>>{ { kTwoHanded, 50.f } });

  Leave(p, kActorHeld);
}

TEST_CASE("The change form keeps a player's actor values, and older records "
          "read as none",
          "[ActorValues]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;
  ActorValueRecord record;
  record.bases = { { kOneHanded, 40.f }, { kHealth, 150.5f } };
  record.skills = { ActorValueRecord::Skill{ 0, 40.f, 3.5f, 120.f } };
  record.xp = 42.f;
  record.threshold = 300.f;
  record.level = 12;
  record.legendary = { { 2, 1 } };
  changeForm.actorValueRecord = record;

  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(element).actorValueRecord ==
          changeForm.actorValueRecord);

  const std::string older = MpChangeForm::ToJson(MpChangeForm()).dump();
  REQUIRE(older.find("actorValueRecord") == std::string::npos);
  auto olderElement = parser.parse(older).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement)
             .actorValueRecord.has_value());
}

// The server's Papyrus natives on a player (R0): read the record, set a
// base, hold it against a stale report and send the record to the player.
// Names resolve as the lab confirmed them (wire-rules actor_values::NAMES).
TEST_CASE("A player's actor value natives read and set the server's record",
          "[ActorValues][Papyrus]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p, kActorNative);
  PapyrusActor papyrus;
  Report(p, Snapshot({ { kOneHanded, 20.f }, { kHealth, 150.f } }, 3));

  REQUIRE(static_cast<double>(papyrus.GetBaseActorValue(
            ac.ToVarValue(), { VarValue("onehanded") })) == 20.0);
  REQUIRE(static_cast<double>(papyrus.GetActorValue(
            ac.ToVarValue(), { VarValue("OneHanded") })) == 20.0);
  // the recorded base health is the maximum the percentage counts against
  REQUIRE(static_cast<double>(papyrus.GetActorValueMax(
            ac.ToVarValue(), { VarValue("Health") })) == 150.0);

  // SetActorValue: recorded, held, sent
  p.Messages().clear();
  papyrus.SetActorValue(ac.ToVarValue(),
                        { VarValue("Marksman"), VarValue(45.f) });
  REQUIRE(static_cast<double>(papyrus.GetBaseActorValue(
            ac.ToVarValue(), { VarValue("Marksman") })) == 45.0);
  REQUIRE(ac.GetHeldActorValues() ==
          std::vector<std::pair<uint8_t, float>>{ { kArchery, 45.f } });
  REQUIRE(SentLevels(p) == std::vector<uint16_t>{ 3 });

  // a stale report does not take it back, and the record goes back again
  // (a report is the client's whole snapshot, Health included: wire-rules
  // actor_values::merge keeps nothing of the record but the holds)
  p.Messages().clear();
  Report(
    p,
    Snapshot({ { kOneHanded, 21.f }, { kArchery, 15.f }, { kHealth, 150.f } },
             3));
  REQUIRE(static_cast<double>(papyrus.GetBaseActorValue(
            ac.ToVarValue(), { VarValue("Marksman") })) == 45.0);
  REQUIRE(SentLevels(p) == std::vector<uint16_t>{ 3 });

  // the report that carries it ends the hold
  Report(
    p,
    Snapshot({ { kOneHanded, 21.f }, { kArchery, 45.f }, { kHealth, 150.f } },
             3));
  REQUIRE(ac.GetHeldActorValues().empty());

  // ModActorValue and ForceActorValue change a permanent modifier in the
  // game, never the base (x-av-probe 20261008-101643): on a player each runs
  // in the player's own game, and the record keeps its bases and the
  // server its percentages
  p.Messages().clear();
  papyrus.ModActorValue(ac.ToVarValue(),
                        { VarValue("Marksman"), VarValue(60.f) });
  papyrus.ForceActorValue(ac.ToVarValue(),
                          { VarValue("Health"), VarValue(75.f) });
  REQUIRE(static_cast<double>(papyrus.GetBaseActorValue(
            ac.ToVarValue(), { VarValue("Marksman") })) == 45.0);
  REQUIRE(ac.GetChangeForm().actorValues.healthPercentage == 1.f);
  p.Tick(); // snippets go out deferred
  REQUIRE(Snippets(p) ==
          std::vector<std::string>{ "ModActorValue", "ForceActorValue" });

  // a name the lab confirmed no index for changes nothing on the record
  const auto before = ac.GetActorValueRecord();
  papyrus.SetActorValue(ac.ToVarValue(),
                        { VarValue("Mysticism"), VarValue(10.f) });
  REQUIRE(ac.GetActorValueRecord() == before);
  REQUIRE(static_cast<double>(papyrus.GetBaseActorValue(
            ac.ToVarValue(), { VarValue("Mysticism") })) == 0.0);

  Leave(p, kActorNative);
}

TEST_CASE("A value set before the player's first report enters the record "
          "with that report",
          "[ActorValues][Papyrus]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p, kActorFresh);
  PapyrusActor papyrus;

  // no record yet: nothing to send, the value waits in the hold
  papyrus.SetActorValue(ac.ToVarValue(),
                        { VarValue("Marksman"), VarValue(30.f) });
  REQUIRE(!ac.GetActorValueRecord());
  REQUIRE(SentLevels(p).empty());
  REQUIRE(static_cast<double>(papyrus.GetBaseActorValue(
            ac.ToVarValue(), { VarValue("Marksman") })) == 30.0);
  // a skill the server knows no base for yet
  REQUIRE(static_cast<double>(papyrus.GetBaseActorValue(
            ac.ToVarValue(), { VarValue("Sneak") })) == 0.0);

  // the first report: recorded with the held value, and sent back
  Report(p, Snapshot({ { kArchery, 15.f }, { kOneHanded, 15.f } }, 1));
  REQUIRE(RecordedBases(ac) ==
          std::vector<std::pair<uint8_t, float>>{ { kArchery, 30.f },
                                                  { kOneHanded, 15.f } });
  REQUIRE(SentLevels(p) == std::vector<uint16_t>{ 1 });

  Leave(p, kActorFresh);
}
