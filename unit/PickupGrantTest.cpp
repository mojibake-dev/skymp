#include "ActionListener.h"
#include "ActivateMessage.h"
#include "SetInventoryMessage.h"
#include "SpSnippetMessage.h"
#include "TakeItemMessage.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <optional>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

#include "MpActor.h"
#include "MpObjectReference.h"

PartOne& GetPartOne();

namespace {

// Riverwood, outside Alvor's forge (Skyrim.esm, Tamriel 0x3c): display items
// owned by the faction ServicesRiverwoodAlvor 0x5a666 (each REFR's XOWN), none
// with an XCNT, so each holds one item. The dagger 0x35516 at (20505.9,
// -44999.3, -22.5) is a SteelDagger 0x13986 with no leveled list; the sword
// 0x35514 at (20983.7, -45144.9, 20.0) is a leveled placement, its NAME the
// placeholder DummySword 0x6a0b9 and its XLIB LItemWeaponSwordTown 0x165bc,
// whose level-one entries are all IronSword 0x12eb7; the mace 0x355ac at
// (20475.8, -44983.3, -19.1) names IronMace 0x13982 with XLIB
// LItemWeaponMaceTown 0x165bb, whose entries are maces only
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kAlvorsFaction = 0x5a666;
constexpr uint32_t kDagger = 0x35516;
constexpr uint32_t kSteelDagger = 0x13986;
constexpr uint32_t kSword = 0x35514;
constexpr uint32_t kDummySword = 0x6a0b9;
constexpr uint32_t kIronSword = 0x12eb7;
constexpr uint32_t kMace = 0x355ac;
constexpr uint32_t kIronMace = 0x13982;

// Morthal's Moorside Inn 0x138ce (Skyrim.esm): REFR 0x10544b of BOOK 0xed03d
// Book2CommonSongOfReturnV19 at (938, -739, 77), DATA flags 0x00, a book to
// read, 145 units from the rug the book tests stand on
constexpr uint32_t kInn = 0x138ce;
constexpr uint32_t kSong = 0x10544b;
constexpr uint32_t kSongBase = 0xed03d;
const NiPoint3 kOnTheRug{ 1060.f, -722.f, 0.f };

// REFR 0x636b9 in cell 0x169fe (Skyrim.esm) at (-568.3, -570.8, 22.3): a
// leveled bundle of arrows, its NAME the placeholder DummyArrow 0x6a0bf, its
// XLIB LItemArrowsAll 0x68839, no XCNT, XOWN 0xb2037. LItemArrowsAll (LVLF
// 0x03, no UseAll) gives 12 of one nested list per entry: BaseArrowIron75
// 0x37c0d, LVLF 0x02, one entry of IronArrow 0x1397d, so one roll gives 12
// iron arrows at most. No override of these in Update.esm or the DLCs
constexpr uint32_t kArrowCell = 0x169fe;
constexpr uint32_t kArrows = 0x636b9;
constexpr uint32_t kDummyArrow = 0x6a0bf;
constexpr uint32_t kIronArrow = 0x1397d;
constexpr uint32_t kArrowsOwner = 0xb2037;
const NiPoint3 kBesideTheArrows{ -520.f, -571.f, 0.f };

constexpr uint32_t kTaker = 0xff000e01;
constexpr uint32_t kOther = 0xff000e02;

// within the activation reach of all three
const NiPoint3 kBeside{ 20700.f, -45100.f, -80.f };

uint8_t g_unparsed[] = { Networking::MinPacketId, '{', '}' };

RawMessageData Raw(Networking::UserId user)
{
  RawMessageData raw;
  raw.userId = user;
  raw.unparsed = g_unparsed;
  raw.unparsedLength = sizeof(g_unparsed);
  return raw;
}

MpActor& Player(PartOne& p, Networking::UserId user, uint32_t id,
                const NiPoint3& pos)
{
  DoConnect(p, user);
  p.CreateActor(id, pos, 0, kTamriel);
  p.SetUserActor(user, id);
  auto& actor = p.worldState.GetFormAt<MpActor>(id);
  actor.RemoveAllItems();
  return actor;
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kTaker);
  p.DestroyActor(kOther);
}

// `user`'s own player activates `refr`, as its E key does
void PickUp(PartOne& p, Networking::UserId user, uint32_t refr)
{
  ActivateMessage msg;
  msg.data.caster = 0x14;
  msg.data.target = refr;
  msg.data.isSecondActivation = false;
  p.GetActionListener().OnActivate(Raw(user), msg);
}

// `user`'s game reports a take from `refr`: what its engine's pickup added
void Take(PartOne& p, Networking::UserId user, uint32_t refr, uint32_t baseId,
          uint32_t count, std::optional<uint32_t> ownerId)
{
  TakeItemMessage msg;
  msg.baseId = baseId;
  msg.count = count;
  msg.target = refr;
  msg.ownerId = ownerId;
  p.GetActionListener().OnTakeItem(Raw(user), msg);
}

uint32_t OwnedCount(const MpActor& actor, uint32_t baseId, uint32_t owner)
{
  uint32_t n = 0;
  for (const auto& e : actor.GetInventory().entries) {
    if (e.baseId == baseId && e.ownerId == owner) {
      n += e.count;
    }
  }
  return n;
}

template <class Message>
std::vector<Message> SentTo(PartOne& p, Networking::UserId user)
{
  std::vector<Message> out;
  for (auto& m : p.Messages()) {
    if (m.userId != user) {
      continue;
    }
    if (auto typed = dynamic_cast<Message*>(m.message.get())) {
      out.push_back(*typed);
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
    auto logger = std::make_shared<spdlog::logger>("pickup-test", sink);
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

TEST_CASE("A player's pickup of a game-file item is granted to that taker's "
          "own engine and recorded from its report with the stack's owner; "
          "a second taker is refused before its engine moves anything, and "
          "a take nobody was granted is refused and resynced",
          "[PickupGrant][espm]")
{
  auto& p = GetPartOne();
  auto& taker = Player(p, 0, kTaker, kBeside);
  auto& other = Player(p, 1, kOther, kBeside + NiPoint3{ 30.f, 0.f, 0.f });
  auto& dagger = p.worldState.GetFormAt<MpObjectReference>(kDagger);
  REQUIRE(dagger.GetBaseId() == kSteelDagger);
  REQUIRE_FALSE(dagger.IsHarvested());
  p.Messages().clear();

  // the taker's activation: granted, and its game told to run the engine's
  // own pickup; nothing reaches the record yet
  {
    LogCapture log;
    PickUp(p, 0, kDagger);
    p.Tick();
    REQUIRE(log.Count("[verb:pickup] ff000e01 picks up 35516 (base 13986): "
                      "granted, its own engine's pickup runs; the record "
                      "waits for its report") == 1);
  }
  REQUIRE(dagger.GetPickupGrantee() == kTaker);
  REQUIRE_FALSE(dagger.IsHarvested());
  REQUIRE(taker.GetInventory().GetItemCount(kSteelDagger) == 0);
  const auto snippets = SentTo<SpSnippetMessage>(p, 0);
  REQUIRE(snippets.size() == 1);
  REQUIRE(snippets[0].class_ == "ObjectReference");
  REQUIRE(snippets[0].function == "Activate");
  REQUIRE(snippets[0].selfId == kDagger);
  REQUIRE(snippets[0].arguments.size() == 2);
  REQUIRE(snippets[0].arguments[1].has_value());
  REQUIRE(std::get<bool>(*snippets[0].arguments[1]) == true);

  // the taker's own activation again before its report: its game is not
  // told twice
  p.Messages().clear();
  {
    LogCapture log;
    PickUp(p, 0, kDagger);
    p.Tick();
    REQUIRE(log.Count("[verb:pickup] ff000e01 picks up 35516 again; its "
                      "grant stands, its report awaited") == 1);
  }
  REQUIRE(SentTo<SpSnippetMessage>(p, 0).empty());
  REQUIRE(dagger.GetPickupGrantee() == kTaker);

  // another player's activation while the grant stands: refused, and its
  // game is told nothing, so its engine never runs a pickup of it
  p.Messages().clear();
  {
    LogCapture log;
    PickUp(p, 1, kDagger);
    p.Tick();
    REQUIRE(log.Count("[verb:pickup] E_PICKUP_GRANTED: ff000e02 picks up "
                      "35516; it is granted to ff000e01, whose own engine's "
                      "pickup is under way; refused") == 1);
  }
  REQUIRE(SentTo<SpSnippetMessage>(p, 1).empty());
  REQUIRE(dagger.GetPickupGrantee() == kTaker);

  // a take from it by a player the grant is not to: refused, its game sent
  // its own inventory back
  p.Messages().clear();
  {
    LogCapture log;
    Take(p, 1, kDagger, kSteelDagger, 1, kAlvorsFaction);
    REQUIRE(log.Count("[verb:pickup] E_PICKUP_NOT_GRANTED: ff000e02 reports "
                      "its pickup of 35516 (1 of 13986); no grant of it to "
                      "this taker stands; refused, the taker resynced") == 1);
  }
  REQUIRE(SentTo<SetInventoryMessage>(p, 1).size() == 1);
  REQUIRE(other.GetInventory().GetItemCount(kSteelDagger) == 0);
  REQUIRE_FALSE(dagger.IsHarvested());

  // the taker's report: recorded with its owner, the engine's stolen mark,
  // and the reference taken for every seat
  {
    LogCapture log;
    Take(p, 0, kDagger, kSteelDagger, 1, kAlvorsFaction);
    REQUIRE(log.Count("[verb:pickup] ff000e01 picks up 35516: 1 of 13986, "
                      "owner 5a666 recorded from its own engine's pickup") ==
            1);
  }
  REQUIRE(OwnedCount(taker, kSteelDagger, kAlvorsFaction) == 1);
  REQUIRE(dagger.IsHarvested());
  REQUIRE(dagger.GetPickupGrantee() == 0);

  // the same report again: taken already
  {
    LogCapture log;
    Take(p, 0, kDagger, kSteelDagger, 1, kAlvorsFaction);
    REQUIRE(log.Count("[verb:pickup] E_PICKUP_TAKEN: ff000e01 reports its "
                      "pickup of 35516 (1 of 13986); the reference is taken "
                      "already; refused, the taker resynced") == 1);
  }
  REQUIRE(taker.GetInventory().GetItemCount(kSteelDagger) == 1);

  // and another activation of it grants nothing
  p.Messages().clear();
  PickUp(p, 1, kDagger);
  p.Tick();
  REQUIRE(dagger.GetPickupGrantee() == 0);
  REQUIRE(SentTo<SpSnippetMessage>(p, 1).empty());
  REQUIRE(other.GetInventory().GetItemCount(kSteelDagger) == 0);

  Leave(p);
}

TEST_CASE("A pickup's report names the reference's own base or a base its "
          "leveled list can roll; any other base, or more than the "
          "reference holds, is refused with the grant left standing",
          "[PickupGrant][espm]")
{
  auto& p = GetPartOne();
  auto& taker = Player(p, 0, kTaker, kBeside);
  Player(p, 1, kOther, kBeside + NiPoint3{ 30.f, 0.f, 0.f });

  // a leveled placement: the engine's roll, never the placeholder
  auto& sword = p.worldState.GetFormAt<MpObjectReference>(kSword);
  REQUIRE(sword.GetBaseId() == kDummySword);
  PickUp(p, 0, kSword);
  REQUIRE(sword.GetPickupGrantee() == kTaker);
  Take(p, 0, kSword, kIronSword, 1, kAlvorsFaction);
  REQUIRE(OwnedCount(taker, kIronSword, kAlvorsFaction) == 1);
  REQUIRE(taker.GetInventory().GetItemCount(kDummySword) == 0);
  REQUIRE(sword.IsHarvested());

  // a base the reference cannot be: refused, the grant standing
  auto& mace = p.worldState.GetFormAt<MpObjectReference>(kMace);
  REQUIRE(mace.GetBaseId() == kIronMace);
  PickUp(p, 0, kMace);
  REQUIRE(mace.GetPickupGrantee() == kTaker);
  {
    LogCapture log;
    Take(p, 0, kMace, kSteelDagger, 1, kAlvorsFaction);
    REQUIRE(log.Count("[verb:pickup] E_PICKUP_BASE: ff000e01 reports its "
                      "pickup of 355ac (1 of 13986); the reference stands "
                      "for 13982 or a roll of its leveled list; refused, the "
                      "taker resynced") == 1);
  }
  REQUIRE(taker.GetInventory().GetItemCount(kSteelDagger) == 0);
  REQUIRE_FALSE(mace.IsHarvested());
  REQUIRE(mace.GetPickupGrantee() == kTaker);

  // more than it holds: refused
  {
    LogCapture log;
    Take(p, 0, kMace, kIronMace, 2, kAlvorsFaction);
    REQUIRE(log.Count("[verb:pickup] E_PICKUP_COUNT: ff000e01 reports its "
                      "pickup of 355ac (2 of 13982); the reference holds 1; "
                      "refused, the taker resynced") == 1);
  }
  REQUIRE(taker.GetInventory().GetItemCount(kIronMace) == 0);
  REQUIRE_FALSE(mace.IsHarvested());

  // its own NAME base, one of it: recorded
  Take(p, 0, kMace, kIronMace, 1, kAlvorsFaction);
  REQUIRE(OwnedCount(taker, kIronMace, kAlvorsFaction) == 1);
  REQUIRE(mace.IsHarvested());

  Leave(p);
}

TEST_CASE("A book's Take, the Book Menu's own pickup, is recorded against the "
          "player's open of it with the stack's owner, and the closing "
          "activation finds it taken",
          "[PickupGrant][Books][espm]")
{
  auto& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(kTaker, kOnTheRug, 0, kInn);
  p.SetUserActor(0, kTaker);
  auto& reader = p.worldState.GetFormAt<MpActor>(kTaker);
  reader.RemoveAllItems();
  auto& book = p.worldState.GetFormAt<MpObjectReference>(kSong);
  REQUIRE(book.GetBaseId() == kSongBase);
  REQUIRE_FALSE(book.IsHarvested());

  // the open: the book's own path, no grant
  PickUp(p, 0, kSong);
  p.Tick();
  REQUIRE(reader.GetOpenBook() == kSong);
  REQUIRE(book.GetPickupGrantee() == 0);
  REQUIRE_FALSE(book.IsHarvested());

  // the engine's Take in the Book Menu, as the game reports it: recorded
  // against the open, the owner as reported
  Take(p, 0, kSong, kSongBase, 1, kAlvorsFaction);
  REQUIRE(OwnedCount(reader, kSongBase, kAlvorsFaction) == 1);
  REQUIRE(book.IsHarvested());

  // the menu's close sends the Take's activation: taken already, the open
  // ended, the inventory sent again, nothing added twice
  p.Messages().clear();
  PickUp(p, 0, kSong);
  p.Tick();
  REQUIRE(reader.GetOpenBook() == 0);
  REQUIRE(reader.GetInventory().GetItemCount(kSongBase) == 1);
  REQUIRE_FALSE(SentTo<SetInventoryMessage>(p, 0).empty());

  DoDisconnect(p, 0);
  p.DestroyActor(kTaker);
}

TEST_CASE("A book's Take reaching the server before its engine's report "
          "grants the book to its reader: nothing is given, and the report "
          "is recorded against the grant with its owner",
          "[PickupGrant][Books][espm]")
{
  auto& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(kTaker, kOnTheRug, 0, kInn);
  p.SetUserActor(0, kTaker);
  auto& reader = p.worldState.GetFormAt<MpActor>(kTaker);
  reader.RemoveAllItems();
  auto& book = p.worldState.GetFormAt<MpObjectReference>(kSong);
  REQUIRE_FALSE(book.IsHarvested());

  PickUp(p, 0, kSong);
  p.Tick();
  REQUIRE(reader.GetOpenBook() == kSong);

  // the menu's close sends the Take first: the open ends into a grant, no
  // copy given
  {
    LogCapture log;
    PickUp(p, 0, kSong);
    p.Tick();
    REQUIRE(log.Count("[verb:books] ff000e01 takes book 10544b (base ed03d) "
                      "from its Book Menu; granted to its own engine's "
                      "pickup, the record waits for its report") == 1);
  }
  REQUIRE(reader.GetOpenBook() == 0);
  REQUIRE(book.GetPickupGrantee() == kTaker);
  REQUIRE_FALSE(book.IsHarvested());
  REQUIRE(reader.GetInventory().GetItemCount(kSongBase) == 0);

  // the engine's report: recorded with the stack's owner, once
  Take(p, 0, kSong, kSongBase, 1, kAlvorsFaction);
  REQUIRE(OwnedCount(reader, kSongBase, kAlvorsFaction) == 1);
  REQUIRE(reader.GetInventory().GetItemCount(kSongBase) == 1);
  REQUIRE(book.IsHarvested());
  REQUIRE(book.GetPickupGrantee() == 0);

  DoDisconnect(p, 0);
  p.DestroyActor(kTaker);
}

TEST_CASE("Two readers of one book: the first Take wins; the other's Take "
          "sends its inventory back and its report, open or not, is refused",
          "[PickupGrant][Books][espm]")
{
  auto& p = GetPartOne();
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kTaker, kOnTheRug, 0, kInn);
  p.SetUserActor(0, kTaker);
  p.CreateActor(kOther, kOnTheRug + NiPoint3{ 0.f, -40.f, 0.f }, 0, kInn);
  p.SetUserActor(1, kOther);
  auto& reader = p.worldState.GetFormAt<MpActor>(kTaker);
  auto& other = p.worldState.GetFormAt<MpActor>(kOther);
  reader.RemoveAllItems();
  other.RemoveAllItems();
  auto& book = p.worldState.GetFormAt<MpObjectReference>(kSong);
  REQUIRE_FALSE(book.IsHarvested());

  PickUp(p, 0, kSong);
  PickUp(p, 1, kSong);
  p.Tick();
  REQUIRE(reader.GetOpenBook() == kSong);
  REQUIRE(other.GetOpenBook() == kSong);

  // the first Take: granted to the reader
  PickUp(p, 0, kSong);
  p.Tick();
  REQUIRE(book.GetPickupGrantee() == kTaker);

  // the other's report while its open stands: refused, the grant the
  // reader's
  {
    LogCapture log;
    Take(p, 1, kSong, kSongBase, 1, kAlvorsFaction);
    REQUIRE(log.Count("[verb:pickup] E_PICKUP_NOT_GRANTED: ff000e02 reports "
                      "its pickup of 10544b (1 of ed03d); no grant of it to "
                      "this taker stands; refused, the taker resynced") == 1);
  }
  REQUIRE(other.GetInventory().GetItemCount(kSongBase) == 0);
  REQUIRE(book.GetPickupGrantee() == kTaker);

  // the other's Take: its inventory sent back, the grant kept
  p.Messages().clear();
  {
    LogCapture log;
    PickUp(p, 1, kSong);
    p.Tick();
    REQUIRE(log.Count("[verb:books] ff000e02 takes book 10544b from its Book "
                      "Menu; it is granted to ff000e01, whose Take came "
                      "first; its inventory sent back") == 1);
  }
  REQUIRE_FALSE(SentTo<SetInventoryMessage>(p, 1).empty());
  REQUIRE(other.GetOpenBook() == 0);
  REQUIRE(book.GetPickupGrantee() == kTaker);
  REQUIRE_FALSE(book.IsHarvested());

  // the reader's report: recorded
  Take(p, 0, kSong, kSongBase, 1, kAlvorsFaction);
  REQUIRE(OwnedCount(reader, kSongBase, kAlvorsFaction) == 1);
  REQUIRE(other.GetInventory().GetItemCount(kSongBase) == 0);
  REQUIRE(book.IsHarvested());

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kTaker);
  p.DestroyActor(kOther);
}

TEST_CASE("A leveled bundle with no XCNT takes as many as one roll of its "
          "list can give: 12 iron arrows recorded, 13 refused",
          "[PickupGrant][espm]")
{
  auto& p = GetPartOne();
  DoConnect(p, 0);
  p.CreateActor(kTaker, kBesideTheArrows, 0, kArrowCell);
  p.SetUserActor(0, kTaker);
  auto& taker = p.worldState.GetFormAt<MpActor>(kTaker);
  taker.RemoveAllItems();
  auto& arrows = p.worldState.GetFormAt<MpObjectReference>(kArrows);
  REQUIRE(arrows.GetBaseId() == kDummyArrow);
  REQUIRE(arrows.PickupMost(kIronArrow) == 12);
  REQUIRE(arrows.PickupMost(kDummyArrow) == 1);

  PickUp(p, 0, kArrows);
  REQUIRE(arrows.GetPickupGrantee() == kTaker);

  // more than one roll can give: refused, the grant standing
  {
    LogCapture log;
    Take(p, 0, kArrows, kIronArrow, 13, kArrowsOwner);
    REQUIRE(log.Count("[verb:pickup] E_PICKUP_COUNT: ff000e01 reports its "
                      "pickup of 636b9 (13 of 1397d); the reference holds "
                      "12; refused, the taker resynced") == 1);
  }
  REQUIRE(taker.GetInventory().GetItemCount(kIronArrow) == 0);
  REQUIRE_FALSE(arrows.IsHarvested());
  REQUIRE(arrows.GetPickupGrantee() == kTaker);

  // the roll's most: recorded
  Take(p, 0, kArrows, kIronArrow, 12, kArrowsOwner);
  REQUIRE(OwnedCount(taker, kIronArrow, kArrowsOwner) == 12);
  REQUIRE(arrows.IsHarvested());

  DoDisconnect(p, 0);
  p.DestroyActor(kTaker);
}
