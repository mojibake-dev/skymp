#include "ActionListener.h"
#include "ConsoleCommandMessage.h"
#include "MpChangeForms.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <fmt/format.h>
#include <simdjson.h>

PartOne& GetPartOne();

// thuum docs/verbs/shouts.md: what a player knows of shouts is the server's
// record (R0): an admin's TeachWord, UnlockWord and AddShout write it and
// tell the target's own game (Game.TeachWord, Game.UnlockWord,
// Actor.AddShout by SpSnippet), the record survives in the change form, and
// after every login the server teaches it again (on the first movement).
// Skyrim.esm (thuum lab/esm.py, 2026-10-10): UnrelentingForceShout SHOU
// 0x13E07, its first word WOOP 0x13E22.
namespace {
constexpr uint32_t kActor = 0xff000abe;
constexpr uint32_t kShout = 0x00013e07;
constexpr uint32_t kWord = 0x00013e22;

MpActor& Player(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kActor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kActor);
  p.Messages().clear();
  return p.worldState.GetFormAt<MpActor>(kActor);
}

// the player's console: `<name> <0x14> <form>`, as skymp5-client sends it
void Console(PartOne& p, const std::string& name, uint32_t form)
{
  RawMessageData raw;
  raw.userId = 0;
  ConsoleCommandMessage msg;
  msg.data.commandName = name;
  msg.data.args = { int64_t{ 0x14 }, int64_t{ form } };
  p.GetActionListener().OnConsoleCommand(raw, msg);
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
  msg.data.worldOrCell = 0x3c;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
}

// The snippets user 0 received since the last clear, as "Class.Function
// form"
std::vector<std::string> Told(PartOne& p)
{
  p.Tick(); // snippets are deferred
  std::vector<std::string> out;
  for (auto& m : p.Messages()) {
    if (m.userId != 0 || m.j["t"] != MsgType::SpSnippet) {
      continue;
    }
    const auto& args = m.j["arguments"];
    const uint32_t form = args.size() == 1 && args[0].contains("formId")
      ? args[0]["formId"].get<uint32_t>()
      : 0;
    out.push_back(m.j["class"].get<std::string>() + "." +
                  m.j["function"].get<std::string>() + " " +
                  fmt::format("{:x}", form));
  }
  return out;
}

bool Contains(const std::vector<std::string>& told, const std::string& line)
{
  return std::find(told.begin(), told.end(), line) != told.end();
}

void Leave(PartOne& p)
{
  p.DestroyActor(kActor);
  DoDisconnect(p, 0);
}
}

TEST_CASE("An admin's word and shout commands write the record and tell the "
          "player's game",
          "[Shouts][espm]")
{
  PartOne& p = GetPartOne();
  const bool forAll = p.worldState.enableConsoleCommandsForAll;
  p.worldState.enableConsoleCommandsForAll = false;
  auto& ac = Player(p);

  // a player's are refused, and nothing is recorded or told
  ac.SetStaffRank(0);
  Console(p, "teachword", kWord);
  REQUIRE(ac.GetShoutRecord().Empty());
  REQUIRE(!Contains(Told(p), "Game.TeachWord 13e22"));

  ac.SetStaffRank(2);
  p.Messages().clear();
  Console(p, "addshout", kShout);
  Console(p, "teachword", kWord);
  Console(p, "unlockword", kWord);
  const auto told = Told(p);
  REQUIRE(Contains(told, "Actor.AddShout 13e07"));
  REQUIRE(Contains(told, "Game.TeachWord 13e22"));
  REQUIRE(Contains(told, "Game.UnlockWord 13e22"));
  const auto record = ac.GetShoutRecord();
  REQUIRE(record.shouts.size() == 1);
  REQUIRE(record.shouts[0].ToFormId(p.worldState.espmFiles) == kShout);
  REQUIRE(record.wordsKnown.size() == 1);
  REQUIRE(record.wordsUnlocked.size() == 1);

  // again: the record does not grow
  Console(p, "teachword", kWord);
  REQUIRE(ac.GetShoutRecord().wordsKnown.size() == 1);

  // a form of the wrong kind is refused
  Console(p, "teachword", kShout);
  REQUIRE(ac.GetShoutRecord().wordsKnown.size() == 1);

  p.worldState.enableConsoleCommandsForAll = forAll;
  Leave(p);
}

TEST_CASE("After a login the server teaches the recorded shouts and words",
          "[Shouts][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p);
  REQUIRE(ac.RecordShoutEntry(MpActor::ShoutEntry::Shout,
                              FormDesc::FromString("13e07:Skyrim.esm")));
  REQUIRE(ac.RecordShoutEntry(MpActor::ShoutEntry::WordKnown,
                              FormDesc::FromString("13e22:Skyrim.esm")));
  REQUIRE(ac.RecordShoutEntry(MpActor::ShoutEntry::WordUnlocked,
                              FormDesc::FromString("13e22:Skyrim.esm")));
  REQUIRE(!ac.RecordShoutEntry(MpActor::ShoutEntry::Shout,
                               FormDesc::FromString("13e07:Skyrim.esm")));

  // the first movement after the login teaches the record
  ac.SetMapMarkersPending(true);
  p.Messages().clear();
  Stand(p);
  const auto told = Told(p);
  REQUIRE(Contains(told, "Actor.AddShout 13e07"));
  REQUIRE(Contains(told, "Game.TeachWord 13e22"));
  REQUIRE(Contains(told, "Game.UnlockWord 13e22"));
  Leave(p);
}

TEST_CASE("The change form keeps a player's shouts and words, and older "
          "records read as none",
          "[Shouts]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;
  ShoutRecord record;
  record.shouts = { FormDesc::FromString("13e07:Skyrim.esm") };
  record.wordsKnown = { FormDesc::FromString("13e22:Skyrim.esm"),
                        FormDesc::FromString("13e23:Skyrim.esm") };
  record.wordsUnlocked = { FormDesc::FromString("13e22:Skyrim.esm") };
  changeForm.shoutRecord = record;

  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(element).shoutRecord ==
          changeForm.shoutRecord);

  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("shoutRecord"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement).shoutRecord);
}
