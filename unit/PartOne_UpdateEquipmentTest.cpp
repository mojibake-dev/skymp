#include "TestUtils.hpp"

#include "ActionListener.h"
#include "UpdateEquipmentMessage.h"
#include <catch2/catch_all.hpp>
#include <string>
#include <vector>

PartOne& GetPartOne();

TEST_CASE("UpdateEquipment", "[PartOne]")
{

  PartOne partOne;

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000ABC, { 1.f, 2.f, 3.f }, 180.f, 0x3c);
  partOne.SetUserActor(0, 0xff000ABC);
  DoMessage(partOne, 0, jEquipment);
  partOne.Messages().clear();

  DoConnect(partOne, 1);
  partOne.CreateActor(0xffABCABC, { 11.f, 22.f, 33.f }, 180.f, 0x3c);
  partOne.SetUserActor(1, 0xffABCABC);

  // createActor should contain equipment
  auto res = FindRefrMessageIdx<CreateActorMessage>(partOne, 0);
  REQUIRE(res.filteredMessages.size() == 1);
  REQUIRE(res.filteredMessages[0].equipment.has_value());
  REQUIRE(res.filteredMessages[0].equipment->ToJson().dump() ==
          jEquipment["data"].dump());

  partOne.Messages().clear();

  DoMessage(partOne, 0, jEquipment);

  REQUIRE(partOne.Messages().size() == 2);
  REQUIRE(std::find_if(partOne.Messages().begin(), partOne.Messages().end(),
                       [&](auto m) {
                         return m.j["t"] == MsgType::UpdateEquipment &&
                           m.j["idx"] == 0 && m.reliable && m.userId == 1 &&
                           m.j["data"] == jEquipment["data"];
                       }) != partOne.Messages().end());
  REQUIRE(std::find_if(partOne.Messages().begin(), partOne.Messages().end(),
                       [&](auto m) {
                         return m.j["t"] == MsgType::UpdateEquipment &&
                           m.j["idx"] == 0 && m.reliable && m.userId == 0 &&
                           m.j["data"] == jEquipment["data"];
                       }) != partOne.Messages().end());
}

// thuum docs/verbs/marksman.md (playtest 37's finding 45): a player's
// equipment report with one entry the server cannot take is taken entry by
// entry: the bad one dropped with its correction, the rest the record.
// Skyrim.esm: LongBow 0x3B562 (MarksmanTest's), IronSword 0x12EB7
// (CellHostTest's), Flames 0x12FCD, a starting spell of the Player NPC_
// (FavoritesTest's, lab/esm.py), Firebolt 0x12FD0, which a fresh actor has
// not learned (ConsoleCommandTest's).
namespace {
constexpr uint32_t kLongBow = 0x0003b562;
constexpr uint32_t kIronSword = 0x00012eb7;
constexpr uint32_t kFlames = 0x00012fcd;
constexpr uint32_t kFirebolt = 0x00012fd0;
constexpr uint32_t kOwner = 0xff000e30;
constexpr uint32_t kWatcher = 0xff000e31;

const auto kWornRight = [] {
  Inventory::ExtraData extra;
  extra.worn_ = true;
  return extra;
}();

// the report's bytes beside the struct, as the client's packet carries them
// (SendToNeighbours relays them as they came; NpcCastTest's HostRaw)
RawMessageData OwnRaw(const UpdateEquipmentMessage& msg)
{
  static std::string bytes;
  nlohmann::json j;
  msg.WriteJson(j);
  bytes.clear();
  bytes.push_back(static_cast<char>(Networking::MinPacketId));
  bytes += j.dump();
  RawMessageData raw;
  raw.userId = 0;
  raw.unparsed = reinterpret_cast<Networking::PacketData>(bytes.data());
  raw.unparsedLength = bytes.size();
  return raw;
}

// the owner (user 0) at the origin and a watcher (user 1) 300 units north,
// both in Tamriel; the owner holds the long bow and has learned Flames
MpActor& Pair(PartOne& p)
{
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kOwner, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kOwner);
  p.CreateActor(kWatcher, { 0, 300, 0 }, 0, 0x3c);
  p.SetUserActor(1, kWatcher);
  auto& owner = p.worldState.GetFormAt<MpActor>(kOwner);
  owner.AddItem(kLongBow, 1);
  owner.AddSpell(kFlames);
  return owner;
}

// the owner's report, then a tick for the deferred snippets
void Report(PartOne& p, const UpdateEquipmentMessage& msg)
{
  p.GetActionListener().OnUpdateEquipment(OwnRaw(msg), msg);
  p.Tick();
}

std::vector<nlohmann::json> Sent(PartOne& p, Networking::UserId user,
                                 MsgType type)
{
  std::vector<nlohmann::json> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(type)) {
      out.push_back(m.j);
    }
  }
  return out;
}

void Part(PartOne& p)
{
  p.DestroyActor(kWatcher);
  p.DestroyActor(kOwner);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}
}

TEST_CASE("An equipment report with one spell the player has not learned "
          "keeps its other entries: the spell is dropped and removed from "
          "the player's game, the rest is the record and the neighbours'",
          "[PartOne][UpdateEquipment][espm]")
{
  PartOne& p = GetPartOne();
  auto& owner = Pair(p);
  REQUIRE(owner.IsSpellLearned(kFlames));
  REQUIRE(!owner.IsSpellLearned(kFirebolt));
  p.Messages().clear();

  UpdateEquipmentMessage msg;
  msg.idx = owner.GetIdx();
  msg.data.numChanges = 7;
  msg.data.inv.entries.push_back(Inventory::Entry(kLongBow, 1, kWornRight));
  msg.data.leftSpell = kFlames;
  msg.data.rightSpell = kFirebolt;
  Report(p, msg);

  // the record: the bow and Flames, Firebolt dropped
  const auto& eq = owner.GetEquipment();
  REQUIRE(eq.numChanges == 7);
  REQUIRE(eq.inv.HasItem(kLongBow));
  REQUIRE(eq.leftSpell.value_or(0) == kFlames);
  REQUIRE(!eq.rightSpell.has_value());

  // the watcher hears the kept report (and the owner its echo)
  const auto relays = Sent(p, 1, MsgType::UpdateEquipment);
  REQUIRE(relays.size() == 1);
  REQUIRE(relays[0]["idx"] == owner.GetIdx());
  REQUIRE(relays[0]["data"]["leftSpell"] == kFlames);
  REQUIRE((!relays[0]["data"].contains("rightSpell") ||
           relays[0]["data"]["rightSpell"].is_null()));
  REQUIRE(relays[0]["data"]["inv"]["entries"].size() == 1);
  REQUIRE(relays[0]["data"]["inv"]["entries"][0]["baseId"] == kLongBow);
  REQUIRE(Sent(p, 0, MsgType::UpdateEquipment).size() == 1);

  // the corrections for the dropped spell alone: RemoveSpell and the
  // server's inventory again; nothing unequipped
  const auto snippets = Sent(p, 0, MsgType::SpSnippet);
  REQUIRE(snippets.size() == 1);
  REQUIRE(snippets[0]["class"] == "Actor");
  REQUIRE(snippets[0]["function"] == "RemoveSpell");
  REQUIRE(snippets[0]["arguments"][0]["formId"] == kFirebolt);
  REQUIRE(Sent(p, 0, MsgType::SetInventory).size() == 1);

  Part(p);
}

TEST_CASE("An equipment report with one item the player's inventory lacks "
          "keeps the rest: the item is dropped and taken off in the "
          "player's game, the rest is the record and the neighbours'",
          "[PartOne][UpdateEquipment][espm]")
{
  PartOne& p = GetPartOne();
  auto& owner = Pair(p);
  REQUIRE(owner.GetInventory().HasItem(kLongBow));
  REQUIRE(!owner.GetInventory().HasItem(kIronSword));
  p.Messages().clear();

  UpdateEquipmentMessage msg;
  msg.idx = owner.GetIdx();
  msg.data.numChanges = 8;
  msg.data.inv.entries.push_back(Inventory::Entry(kLongBow, 1, kWornRight));
  msg.data.inv.entries.push_back(Inventory::Entry(kIronSword, 1, kWornRight));
  msg.data.leftSpell = kFlames;
  Report(p, msg);

  // the record: the bow and Flames, the sword dropped
  const auto& eq = owner.GetEquipment();
  REQUIRE(eq.numChanges == 8);
  REQUIRE(eq.inv.HasItem(kLongBow));
  REQUIRE(!eq.inv.HasItem(kIronSword));
  REQUIRE(eq.leftSpell.value_or(0) == kFlames);

  // the watcher hears the kept report
  const auto relays = Sent(p, 1, MsgType::UpdateEquipment);
  REQUIRE(relays.size() == 1);
  REQUIRE(relays[0]["data"]["inv"]["entries"].size() == 1);
  REQUIRE(relays[0]["data"]["inv"]["entries"][0]["baseId"] == kLongBow);
  REQUIRE(relays[0]["data"]["leftSpell"] == kFlames);

  // the corrections for the dropped item alone: UnequipItem and the
  // server's inventory again; no spell removed
  const auto snippets = Sent(p, 0, MsgType::SpSnippet);
  REQUIRE(snippets.size() == 1);
  REQUIRE(snippets[0]["class"] == "Actor");
  REQUIRE(snippets[0]["function"] == "UnequipItem");
  REQUIRE(snippets[0]["arguments"][0]["formId"] == kIronSword);
  REQUIRE(Sent(p, 0, MsgType::SetInventory).size() == 1);

  Part(p);
}
