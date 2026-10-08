#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ConsoleCommandMessage.h"
#include "MpChangeForms.h"
#include "PacketParser.h"
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
  // console line, not an exception, and nothing is added
  p.Messages().clear();
  p.GetActionListener().OnConsoleCommand(msgData, msg);
  p.Tick();
  REQUIRE(Lines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "Not enough permissions to use this command", true } });
  REQUIRE(ac.GetInventory().GetItemCount(0x12eb7) == 0);

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

  // an admin may; a save is nobody's; COC is listed, not run yet; a name
  // the table does not know is unknown
  ac.SetStaffRank(2);
  p.Messages().clear();
  Send(p, "additem", { int64_t(0x14), int64_t(0x12eb7), int64_t(1) });
  Send(p, "save", { int64_t(0), std::string("x") });
  Send(p, "coc", { int64_t(0), int64_t(0x3c) });
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
  REQUIRE(ac.GetRecordedActorValueBase(8) == std::optional<float>(47.5f));
  REQUIRE(Lines(p) ==
          std::vector<std::pair<std::string, bool>>{
            { "setav done", false }, { "modav done", false } });

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
  REQUIRE(ac.GetRecordedActorValueBase(8) == std::optional<float>(47.5f));

  p.DestroyActor(0xff000000);
  DoDisconnect(p, 0);
}
