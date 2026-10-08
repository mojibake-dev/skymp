#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ConsoleCommandMessage.h"
#include "MpChangeForms.h"
#include "PacketParser.h"
#include "UpdateMovementMessage.h"
#include <simdjson.h>

using Catch::Matchers::ContainsSubstring;

PartOne& GetPartOne();

namespace {
// the ConsoleOutput lines user 0 received since the last clear
std::vector<std::pair<std::string, bool>> Lines(PartOne& p)
{
  std::vector<std::pair<std::string, bool>> out;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::ConsoleOutput) {
      out.emplace_back(m.j["text"].get<std::string>(),
                       m.j["refused"].get<bool>());
    }
  }
  return out;
}

// every other message, in order
std::vector<nlohmann::json> Others(PartOne& p)
{
  std::vector<nlohmann::json> out;
  for (auto& m : p.Messages()) {
    if (m.j["t"] != MsgType::ConsoleOutput) {
      out.push_back(m.j);
    }
  }
  return out;
}

void Send(PartOne& p, const std::string& name,
          std::vector<std::variant<int64_t, std::string>> args)
{
  RawMessageData msgData;
  msgData.userId = 0;
  ConsoleCommandMessage msg;
  msg.data.commandName = name;
  msg.data.args = std::move(args);
  p.GetActionListener().OnConsoleCommand(msgData, msg);
}

// The player's own movement report from a cell or worldspace
void Report(PartOne& p, MpActor& me, uint32_t worldOrCell, const NiPoint3& pos)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = 0;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateMovementMessage msg;
  msg.idx = me.GetIdx();
  msg.data.pos = { pos.x, pos.y, pos.z };
  msg.data.rot = { 0, 0, 0 };
  msg.data.isInJumpState = false;
  msg.data.isWeapDrawn = false;
  msg.data.isBlocking = false;
  msg.data.worldOrCell = worldOrCell;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
}
}

TEST_CASE("ConsoleCommand packet is parsed", "[ConsoleCommand]")
{
  class MyActionListener : public ActionListener
  {
  public:
    MyActionListener()
      : ActionListener(GetPartOne())
    {
    }

    void OnConsoleCommand(const RawMessageData& rawMsgData_,
                          const ConsoleCommandMessage& msg_) override
    {
      rawMsgData = rawMsgData_;
      commandName = msg_.data.commandName;
      args = msg_.data.args;
    }

    RawMessageData rawMsgData;
    std::string commandName;
    std::vector<std::variant<int64_t, std::string>> args;
  };

  nlohmann::json j{ { "t", MsgType::ConsoleCommand },
                    { "data",
                      { { "commandName", "additem" },
                        { "args", { 0x14, 0x12eb7, 0x1 } } } } };

  auto msg = MakeMessage(j);

  MyActionListener listener;

  PacketParser p;
  p.TransformPacketIntoAction(
    122, reinterpret_cast<Networking::PacketData>(msg.data()), msg.size(),
    listener);

  REQUIRE(listener.args ==
          std::vector<std::variant<int64_t, std::string>>{
            int64_t(0x14), int64_t(0x12eb7), int64_t(0x1) });
  REQUIRE(listener.commandName == "additem");
  REQUIRE(listener.rawMsgData.userId == 122);
}

TEST_CASE("AddItem doesn't execute for non-privilleged users",
          "[ConsoleCommand]")
{
  PartOne& p = GetPartOne();

  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);

  RawMessageData msgData;
  msgData.userId = 0;

  ConsoleCommandMessage msg;
  msg.data.commandName = "additem";
  msg.data.args = { int64_t(0x14), int64_t(0x12eb7), int64_t(0x108) };
  // thuum docs/verbs/console-commands.md: the refusal is the caller's
  // console line, not an exception, and nothing is added (the shared test
  // world may hand this actor items an earlier test left: compare)
  const auto before = ac.GetInventory().GetItemCount(0x12eb7);
  p.Messages().clear();
  p.GetActionListener().OnConsoleCommand(msgData, msg);
  p.Tick();
  REQUIRE(Lines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "Not enough permissions to use this command", true } });
  REQUIRE(ac.GetInventory().GetItemCount(0x12eb7) == before);

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

TEST_CASE("AddItem executes", "[ConsoleCommand][espm]")
{
  PartOne& p = GetPartOne();

  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);
  ac.SetConsoleCommandsAllowedFlag(true);
  ac.RemoveAllItems();

  RawMessageData msgData;
  msgData.userId = 0;

  p.Messages().clear();
  ConsoleCommandMessage msg;
  msg.data.commandName = "additem";
  msg.data.args = { int64_t(0x14), int64_t(0x12eb7), int64_t(0x108) };
  p.GetActionListener().OnConsoleCommand(msgData, msg);

  p.Tick(); // send deferred messages

  nlohmann::json expectedInv{
    { "entries", { { { "baseId", 0x12eb7 }, { "count", 0x108 } } } }
  };
  // the inventory and the snippet as before, and the caller's console line
  REQUIRE(
    Others(p) ==
    std::vector<nlohmann::json>{
      nlohmann::json::parse(
        R"({"inventory":{"entries":[{"baseId":77495,"count":264}]},"t":28})"),
      nlohmann::json::parse(
        R"({"arguments":[{"formId":77495,"type":"weapon"},264,false],"class":"SkympHacks","function":"AddItem","selfId":0,"snippetIdx":4294967295,"t":30})") });
  REQUIRE(
    Lines(p) ==
    std::vector<std::pair<std::string, bool>>{ { "additem done", false } });

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

TEST_CASE("PlaceAtMe executes", "[ConsoleCommand][espm]")
{
  enum
  {
    EncGiant01 = 0x00023aae
  };

  PartOne& p = GetPartOne();

  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);
  ac.SetConsoleCommandsAllowedFlag(true);

  RawMessageData msgData;
  msgData.userId = 0;

  p.Messages().clear();
  ConsoleCommandMessage msg2;
  msg2.data.commandName = "placeatme";
  msg2.data.args = { int64_t(0x14), int64_t(EncGiant01) };
  p.GetActionListener().OnConsoleCommand(msgData, msg2);

  auto& refr = p.worldState.GetFormAt<MpActor>(0xff000001);
  REQUIRE(refr.GetBaseId() == EncGiant01);
  REQUIRE(refr.GetPos() == ac.GetPos());
  REQUIRE(refr.GetAngle() == NiPoint3(0, 0, 0));
  REQUIRE(refr.GetCellOrWorld() == ac.GetCellOrWorld());
  p.worldState.DestroyForm(0xff000001);

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

// thuum docs/verbs/console-commands.md: the server's table (wire-rules
// console) decides by the caller's staff rank; the record keeps the rank
TEST_CASE("Console commands follow the caller's staff rank",
          "[ConsoleCommand][espm]")
{
  PartOne& p = GetPartOne();
  p.worldState.enableConsoleCommandsForAll = false;
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);
  ac.RemoveAllItems();

  // older records: consoleCommandsAllowed reads as admin, else player
  REQUIRE(ac.GetStaffRank() == 0);
  ac.SetConsoleCommandsAllowedFlag(true);
  REQUIRE(ac.GetStaffRank() == 2);

  // a moderator may not add items
  ac.SetStaffRank(1);
  p.Messages().clear();
  Send(p, "additem", { int64_t(0x14), int64_t(0x12eb7), int64_t(1) });
  REQUIRE(Lines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "Not enough permissions to use this command", true } });

  // an admin may; a save is nobody's; SetLevel is listed, not run yet; a
  // name the table does not know is unknown
  ac.SetStaffRank(2);
  p.Messages().clear();
  Send(p, "additem", { int64_t(0x14), int64_t(0x12eb7), int64_t(1) });
  Send(p, "save", { int64_t(0), std::string("x") });
  Send(p, "setlevel", { int64_t(0), int64_t(5) });
  Send(p, "nosuchcommand", {});
  p.Tick();
  REQUIRE(
    Lines(p) ==
    std::vector<std::pair<std::string, bool>>{
      { "additem done", false },
      { "The server owns the world: this command is not available", true },
      { "The server does not run this command yet", true },
      { "Unknown command", true } });
  REQUIRE(ac.GetInventory().GetItemCount(0x12eb7) == 1);

  // with every player an owner by the server's setting, a rank the
  // gamemode recorded still wins
  p.worldState.enableConsoleCommandsForAll = true;
  ac.SetStaffRank(0);
  p.Messages().clear();
  Send(p, "additem", { int64_t(0x14), int64_t(0x12eb7), int64_t(1) });
  REQUIRE(Lines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "Not enough permissions to use this command", true } });
  REQUIRE(ac.GetInventory().GetItemCount(0x12eb7) == 1);
  ac.SetStaffRank(2);

  // the rank survives in the change form
  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(ac.GetChangeForm()).dump();
  auto element = parser.parse(dump).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(element).staffRank ==
          std::optional<uint8_t>(2));

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

TEST_CASE("SetAV, ModAV and ForceAV run through the server's actor value "
          "natives",
          "[ConsoleCommand][espm]")
{
  PartOne& p = GetPartOne();
  p.worldState.enableConsoleCommandsForAll = true;
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);

  // the player's record (actor-values verb) before the commands
  ActorValueRecord record;
  record.bases = { { 8, 15.f } }; // Marksman (Archery)
  record.level = 1;
  record.threshold = 75.f;
  ac.SetActorValueRecord(record);

  p.Messages().clear();
  Send(p, "setav", { int64_t(0x14), std::string("Marksman"), int64_t(45) });
  Send(p, "modav",
       { int64_t(0x14), std::string("marksman"), std::string("2.5") });
  p.Tick();
  // SetAV sets the record's base (R0); ModAV changes a permanent modifier,
  // which the player's own game keeps (docs/verbs/actor-values.md), so the
  // base stays and the native runs there
  REQUIRE(ac.GetRecordedActorValueBase(8) == std::optional<float>(45.f));
  REQUIRE(Lines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "setav done", false }, { "modav done", false } });
  std::vector<std::string> snippets;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::SpSnippet) {
      snippets.push_back(m.j["function"].get<std::string>());
    }
  }
  REQUIRE(snippets == std::vector<std::string>{ "ModActorValue" });

  // a value that is no number, or not a finite one, fails with a line and
  // changes nothing
  p.Messages().clear();
  Send(p, "setav",
       { int64_t(0x14), std::string("Marksman"), std::string("lots") });
  Send(p, "forceav",
       { int64_t(0x14), std::string("Marksman"), std::string("nan") });
  Send(p, "modav",
       { int64_t(0x14), std::string("Marksman"), std::string("inf") });
  Send(p, "setav",
       { int64_t(0x14), std::string("Marksman"), std::string("12abc") });
  p.Tick();
  REQUIRE(Lines(p) ==
          std::vector<std::pair<std::string, bool>>(
            4, { "Failed: the value is not a number", true }));
  REQUIRE(ac.GetRecordedActorValueBase(8) == std::optional<float>(45.f));

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

TEST_CASE("RemoveItem, Enable, Kill, Resurrect, SetPos, SetAngle and MoveTo "
          "run through the server's own paths",
          "[ConsoleCommand][espm]")
{
  PartOne& p = GetPartOne();
  p.worldState.enableConsoleCommandsForAll = true;
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);
  p.CreateActor(0xff000001, { 500, 600, 70 }, 90, 0x3c);
  auto& other = p.worldState.GetFormAt<MpActor>(0xff000001);
  ac.RemoveAllItems();

  // RemoveItem takes back part of what AddItem gave
  p.Messages().clear();
  Send(p, "additem", { int64_t(0x14), int64_t(0x12eb7), int64_t(5) });
  Send(p, "removeitem", { int64_t(0x14), int64_t(0x12eb7), int64_t(3) });
  p.Tick();
  REQUIRE(ac.GetInventory().GetItemCount(0x12eb7) == 2);
  REQUIRE(Lines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "additem done", false }, { "removeitem done", false } });

  // SetPos and SetAngle change one axis of the caller, through a teleport
  p.Messages().clear();
  Send(p, "setpos", { int64_t(0x14), std::string("X"), int64_t(100) });
  Send(p, "setangle",
       { int64_t(0x14), std::string("z"), std::string("45.5") });
  REQUIRE(ac.GetPos().x == 100.f);
  REQUIRE(ac.GetPos().y == 0.f);
  REQUIRE(ac.GetAngle().z == 45.5f);
  Send(p, "setpos", { int64_t(0x14), std::string("w"), int64_t(1) });
  p.Tick();
  REQUIRE(Lines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "setpos done", false },
            { "setangle done", false },
            { "Failed: the axis is X, Y or Z", true } });
  REQUIRE(ac.GetPos().x == 100.f);

  // MoveTo goes to the named reference
  Send(p, "moveto", { int64_t(0x14), int64_t(0xff000001) });
  REQUIRE(ac.GetPos().x == 500.f);
  REQUIRE(ac.GetPos().y == 600.f);
  REQUIRE(ac.GetPos().z == 70.f);

  // Kill, then Resurrect where it fell; each refuses the state it needs
  p.Messages().clear();
  Send(p, "kill", { int64_t(0xff000001) });
  REQUIRE(other.IsDead());
  Send(p, "kill", { int64_t(0xff000001) });
  Send(p, "resurrect", { int64_t(0xff000001) });
  REQUIRE(!other.IsDead());
  Send(p, "resurrect", { int64_t(0xff000001) });
  p.Tick();
  REQUIRE(Lines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "kill done", false },
            { "Failed: already dead", true },
            { "resurrect done", false },
            { "Failed: not dead", true } });

  // Enable after Disable, on a reference the server made
  Send(p, "disable", { int64_t(0xff000001) });
  REQUIRE(other.IsDisabled());
  Send(p, "enable", { int64_t(0xff000001) });
  REQUIRE(!other.IsDisabled());

  p.DestroyActor(0xff000001);
  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}

// thuum docs/verbs/console-commands.md, COC (an admin's, Eli 2026-10-08):
// the caller's own game goes to a cell the server knows, and the server
// takes that one jump and records where the game landed. Skyrim.esm:
// Riverwood is Tamriel's square (4, -12), its inn the interior 0x133c6
// (thuum lab/esm.py)
TEST_CASE("COC sends an admin's game to a cell the server knows and takes "
          "that one jump",
          "[ConsoleCommand][espm]")
{
  PartOne& p = GetPartOne();
  const bool forAll = p.worldState.enableConsoleCommandsForAll;
  p.worldState.enableConsoleCommandsForAll = false;
  DoConnect(p, 0);
  p.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, 0xff000000);
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);
  const auto& files = p.worldState.espmFiles;

  // the cells user 0's game was told to go to, and the times it was sent
  // back
  const auto sentTo = [&] {
    std::vector<std::string> out;
    for (auto& m : p.Messages()) {
      if (m.userId == 0 && m.j["t"] == MsgType::SpSnippet &&
          m.j["class"] == "Debug" && m.j["function"] == "CenterOnCell" &&
          m.j["selfId"] == 0) {
        out.push_back(m.j["arguments"][0].get<std::string>());
      }
    }
    return out;
  };
  const auto sentBack = [&] {
    int n = 0;
    for (auto& m : p.Messages()) {
      n += m.userId == 0 && m.j["t"] == MsgType::Teleport2;
    }
    return n;
  };

  // a moderator may not; an admin's COC needs a cell the server knows
  ac.SetStaffRank(1);
  p.Messages().clear();
  Send(p, "coc", { int64_t(0), std::string("riverwood") });
  ac.SetStaffRank(2);
  Send(p, "coc", { int64_t(0), std::string("nosuchcell") });
  Send(p, "coc", { int64_t(0), int64_t(0x3c) });
  Send(p, "coc", { int64_t(0) });
  p.Tick();
  REQUIRE(Lines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "Not enough permissions to use this command", true },
            { "Failed: no cell named nosuchcell", true },
            { "Failed: no cell 3c", true },
            { "Failed: COC needs a cell's name", true } });
  REQUIRE(sentTo().empty());

  // Riverwood by its name, case aside: the game is told the name as the
  // file has it
  p.Messages().clear();
  Send(p, "coc", { int64_t(0), std::string("riverwood") });
  p.Tick();
  REQUIRE(Lines(p) ==
          std::vector<std::pair<std::string, bool>>{ { "coc done", false } });
  REQUIRE(sentTo() == std::vector<std::string>{ "Riverwood" });

  // a report from the loading screen (no cell) and one far from Riverwood
  // are dropped, the player not sent back; then the landing is taken
  p.Messages().clear();
  Report(p, ac, 0, { 0, 0, 0 });
  Report(p, ac, 0x3c, { 100000, 100000, 0 });
  REQUIRE(sentBack() == 0);
  REQUIRE(ac.GetPos() == NiPoint3{ 0, 0, 0 });
  Report(p, ac, 0x3c, { 18432, -47104, 500 });
  REQUIRE(ac.GetPos() == NiPoint3{ 18432, -47104, 500 });
  REQUIRE(ac.GetCellOrWorld() == FormDesc::Tamriel());
  REQUIRE(sentBack() == 0);

  // that was the one jump: the next is sent back
  Report(p, ac, 0x3c, { 0, 0, 0 });
  REQUIRE(sentBack() == 1);
  REQUIRE(ac.GetPos() == NiPoint3{ 18432, -47104, 500 });

  // the inn by the form id the game found for its name: the record moves
  // into the interior cell with the player
  p.Messages().clear();
  Send(p, "coc", { int64_t(0), int64_t(0x133c6) });
  p.Tick();
  REQUIRE(Lines(p) ==
          std::vector<std::pair<std::string, bool>>{ { "coc done", false } });
  REQUIRE(sentTo() == std::vector<std::string>{ "RiverwoodSleepingGiantInn" });
  Report(p, ac, 0x133c6, { 10, 20, 30 });
  REQUIRE(ac.GetCellOrWorld().ToFormId(files) == 0x133c6);
  REQUIRE(ac.GetPos() == NiPoint3{ 10, 20, 30 });
  REQUIRE(sentBack() == 0);

  p.worldState.enableConsoleCommandsForAll = forAll;
  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}
