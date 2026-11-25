#include "ActionListener.h"
#include "MpChangeForms.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <functional>
#include <optional>
#include <simdjson.h>

PartOne& GetPartOne();

// A player's bounty per hold and its jail term, the server's record; an
// arrest the arrested player's own game reports. Falkreath's records
// (Skyrim.esm): the crime faction CrimeFactionFalkreath 0x28170 names its
// jail marker 0x3ef17 (JAIL), a REFR in Tamriel 0x3c whose XTEL lands by door
// 0x3ef07, its stolen-goods chest 0xef436 (STOL), its prisoner-belongings
// chest 0xef437 (PLCN) and the jail outfit BeggarOutfit 0x28b60 (JOUT). The
// chest stands in FalkreathBarracksJail 0xef324, the cell the jail's
// teleport loads, and the outfit's items are ClothesBeggarRobes 0x13105 and
// ClothesBeggarBoots 0x13106.
namespace {
constexpr uint32_t kPrisoner = 0xff000f97;
constexpr uint32_t kFalkreath = 0x28170;
constexpr uint32_t kMarker = 0x3ef17;
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kJailCell = 0xef324;
constexpr uint32_t kStolenGoods = 0xef436;
constexpr uint32_t kBelongings = 0xef437;
constexpr uint32_t kRobes = 0x13105;
constexpr uint32_t kBoots = 0x13106;
// IronDagger, an item to strip
constexpr uint32_t kDagger = 0x1397e;
// PlayerFaction 0xdb1: a faction with no crime data, so no jail
constexpr uint32_t kPlayerFaction = 0xdb1;
// Falkreath's street, outside the barracks
const NiPoint3 kInFalkreath{ -28000.f, -48000.f, -2000.f };

MpActor& Arrive(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kPrisoner, kInFalkreath, 0, kTamriel);
  p.SetUserActor(0, kPrisoner);
  p.Messages().clear();
  return p.worldState.GetFormAt<MpActor>(kPrisoner);
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  p.DestroyActor(kPrisoner);
}

// the shared clock pinned at a day for the test's length
struct PinnedClock
{
  PinnedClock(PartOne& p_, float day)
    : p(p_)
    , old(p_.worldState.gameTime)
  {
    Set(day);
  }
  ~PinnedClock() { p.worldState.gameTime = old; }
  void Set(float day)
  {
    p.worldState.gameTime = [day] {
      GameTimeNow t;
      t.daysPassed = day;
      t.timeScale = 20.f;
      return t;
    };
  }
  PartOne& p;
  std::function<GameTimeNow()> old;
};
}

TEST_CASE("The change form keeps a player's bounty and jail term, and older "
          "records read as none",
          "[Jail]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;
  CrimeRecord record;
  record.Set(FormDesc::FromString("28170:Skyrim.esm"), 1000, 40);
  record.Set(FormDesc::FromString("267ea:Skyrim.esm"), 0, 5);
  JailTerm term;
  term.faction = FormDesc::FromString("28170:Skyrim.esm");
  term.days = 7;
  term.releaseAt = 12.5;
  term.belongings.AddItems({ Inventory::Entry(kDagger, 2) });
  term.arrived = true;
  record.jail = term;
  changeForm.crimeRecord = record;

  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(element).crimeRecord ==
          changeForm.crimeRecord);

  // the migration: a record from before the key reads as none, and a record
  // with none writes no key
  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("crimeRecord"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement).crimeRecord);
}

TEST_CASE("A hold's bounty is set as the game holds it, and a paid one leaves "
          "the record",
          "[Jail]")
{
  CrimeRecord record;
  const auto falkreath = FormDesc::FromString("28170:Skyrim.esm");
  record.Set(falkreath, 1000, 0);
  REQUIRE(record.Find(falkreath)->violent == 1000);
  record.Set(falkreath, 1000, 25);
  REQUIRE(record.crimeGold.size() == 1);
  REQUIRE(record.Find(falkreath)->nonViolent == 25);
  record.Set(falkreath, 0, 0);
  REQUIRE(record.crimeGold.empty());
  REQUIRE(record.Empty());
  // a zero bounty for a hold with none is no entry
  record.Set(falkreath, 0, 0);
  REQUIRE(record.Empty());
}

TEST_CASE("A hold's jail is read from its crime faction's records",
          "[Jail][espm]")
{
  PartOne& p = GetPartOne();
  const auto jail = p.JailOf(kFalkreath);
  REQUIRE(jail.has_value());
  REQUIRE(jail->marker == kMarker);
  REQUIRE(jail->markerWorldOrCell == kTamriel);
  REQUIRE(jail->landingWorldOrCell == kJailCell);
  REQUIRE(jail->landingInterior);
  // the release's spot: the far side of the jail's door, door 0x3ef07's own
  // XTEL landing beside the marker in Tamriel at (-29493.3, -87831.3,
  // -2979.68), the point itself as the engine's release fills it (the
  // player's own move raises it), not the marker's own spot (-29478.1,
  // -87742.2, -2961.7)
  REQUIRE(jail->releaseWorldOrCell == kTamriel);
  REQUIRE(
    (jail->releasePos - NiPoint3{ -29493.3f, -87831.3f, -2979.68f }).Length() <
    1.f);
  REQUIRE((jail->releasePos - jail->markerPos).Length() > 50.f);
  REQUIRE(jail->evidenceChest == kStolenGoods);
  REQUIRE(jail->belongingsChest == kBelongings);
  REQUIRE(std::find(jail->outfit.begin(), jail->outfit.end(), kRobes) !=
          jail->outfit.end());
  REQUIRE(std::find(jail->outfit.begin(), jail->outfit.end(), kBoots) !=
          jail->outfit.end());

  // a faction with no crime data names no jail
  REQUIRE(!p.JailOf(kPlayerFaction).has_value());
  // nor does a form that is no faction
  REQUIRE(!p.JailOf(kDagger).has_value());
}

namespace {
void Arrest(PartOne& p, uint32_t faction, uint32_t days)
{
  DoMessage(p, 0,
            nlohmann::json{ { "t", MsgType::Arrest },
                            { "faction", faction },
                            { "days", days } });
}

void Put(PartOne& p, uint32_t baseId, uint32_t count, uint32_t target,
         std::optional<float> health = std::nullopt,
         std::optional<uint32_t> owner = std::nullopt)
{
  nlohmann::json j{ { "t", MsgType::PutItem },
                    { "baseId", baseId },
                    { "count", count },
                    { "target", target } };
  if (health) {
    j["health"] = *health;
  }
  if (owner) {
    j["ownerId"] = *owner;
  }
  DoMessage(p, 0, j);
}

void Take(PartOne& p, uint32_t baseId, uint32_t count, uint32_t target)
{
  DoMessage(p, 0,
            nlohmann::json{ { "t", MsgType::TakeItem },
                            { "baseId", baseId },
                            { "count", count },
                            { "target", target } });
}

// the prisoner's game's movement report from a place (ConsoleCommandTest's
// Report)
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

// the arrest's window closed early (its 60 s are real time), as after the
// prisoner's arrival in the jail
void CloseWindow(MpActor& prisoner)
{
  MpActor::ArrestWindow closed;
  closed.until = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  prisoner.OpenArrestWindow(closed);
}
}

TEST_CASE("An arrest records the term on the shared clock and takes its "
          "game's strip and outfit, its move into the hold's jail its own",
          "[Jail][espm]")
{
  PartOne& p = GetPartOne();
  PinnedClock clock(p, 10.f);
  auto& prisoner = Arrive(p);
  // the player's base may bring daggers of its own, so the counts are read
  // against the start
  prisoner.AddItem(kDagger, 3);
  const uint32_t held = prisoner.GetInventory().GetItemCount(kDagger);
  REQUIRE(held >= 3);

  // the sentence's bounds: the game's 1 to 7 days
  REQUIRE(p.OnArrest(prisoner, kFalkreath, 0) ==
          PartOne::ArrestCheck::BadSentence);
  REQUIRE(p.OnArrest(prisoner, kFalkreath, 8) ==
          PartOne::ArrestCheck::BadSentence);
  REQUIRE(p.OnArrest(prisoner, kPlayerFaction, 3) ==
          PartOne::ArrestCheck::NoJail);
  REQUIRE(!prisoner.GetCrimeRecord().jail);
  REQUIRE(!prisoner.GetArrestWindow());

  // a put into the stolen-goods chest from the street before any arrest is
  // out of reach, as any put
  auto& chest = p.worldState.GetFormAt<MpObjectReference>(kBelongings);
  const uint32_t inChest = chest.GetInventory().GetItemCount(kDagger);
  Put(p, kDagger, 1, kStolenGoods);
  REQUIRE(prisoner.GetInventory().GetItemCount(kDagger) == held);

  // the arrest's report, through the message
  Arrest(p, kFalkreath, 7);
  const auto term = prisoner.GetCrimeRecord().jail;
  REQUIRE(term.has_value());
  REQUIRE(term->faction.ToFormId(p.worldState.espmFiles) == kFalkreath);
  REQUIRE(term->days == 7);
  REQUIRE(term->releaseAt == Catch::Approx(17.0));

  // a second report while the term runs keeps the term
  REQUIRE(p.OnArrest(prisoner, kFalkreath, 2) ==
          PartOne::ArrestCheck::AlreadyJailed);
  REQUIRE(prisoner.GetCrimeRecord().jail->days == 7);

  // the engine's move into the jail is the prisoner's own game's, recorded
  // as any: the arrest permits no jump of its own
  REQUIRE(p.CheckJump(kPrisoner, kJailCell, 0.f, 0.f) ==
          PartOne::JumpCheck::NoPermit);

  // the strip from the street, named to the stolen-goods chest as the
  // client's diff may name it: into the belongings chest, recorded as the
  // prisoner's own, no reach asked
  Put(p, kDagger, 2, kStolenGoods);
  REQUIRE(prisoner.GetInventory().GetItemCount(kDagger) == held - 2);
  REQUIRE(chest.GetInventory().GetItemCount(kDagger) == inChest + 2);
  REQUIRE(prisoner.GetCrimeRecord().jail->belongings.GetItemCount(kDagger) ==
          2);
  // more than the record holds moves nothing
  Put(p, kDagger, 50, kBelongings);
  REQUIRE(prisoner.GetInventory().GetItemCount(kDagger) == held - 2);

  // the outfit the engine made, seen as a take from the chest: created on the
  // record once, never taken from the chest; anything else is not moved
  Take(p, kRobes, 1, kStolenGoods);
  REQUIRE(prisoner.GetInventory().GetItemCount(kRobes) == 1);
  Take(p, kRobes, 1, kStolenGoods);
  REQUIRE(prisoner.GetInventory().GetItemCount(kRobes) == 1);
  Take(p, kDagger, 1, kBelongings);
  REQUIRE(prisoner.GetInventory().GetItemCount(kDagger) == held - 2);
  // the outfit is never part of the strip, and is created once a window:
  // put back, or disposed of, it is not made again
  Put(p, kRobes, 1, kStolenGoods);
  REQUIRE(prisoner.GetInventory().GetItemCount(kRobes) == 1);
  prisoner.RemoveItem(kRobes, 1, nullptr);
  Take(p, kRobes, 1, kStolenGoods);
  REQUIRE(prisoner.GetInventory().GetItemCount(kRobes) == 0);

  Leave(p);
}

namespace {
// MsgType::JailTerm: a player's jail term as the server's record holds it,
// told to that player's own game
constexpr int kJailTermType = 88;

// the jail terms the server told the prisoner's game (user 0) since the
// messages were last cleared
std::vector<nlohmann::json> JailTermsTold(PartOne& p)
{
  std::vector<nlohmann::json> told;
  for (const auto& m : p.Messages()) {
    if (m.userId == 0 && m.j.is_object() && m.j.contains("t") &&
        m.j["t"] == kJailTermType) {
      told.push_back(m.j);
    }
  }
  return told;
}

// the prisoner's game's report from its jail's cell, the arrest's jump, then
// the arrest's window closed and its arrival noted, as the jail's tick does
void ServeInCell(PartOne& p, MpActor& prisoner,
                 const PartOne::JailOfFaction& jail)
{
  Report(p, prisoner, jail.landingWorldOrCell, jail.landingPos);
  CloseWindow(prisoner);
  REQUIRE(!p.EndTermIfEscaped(prisoner));
  REQUIRE(prisoner.GetCrimeRecord().jail);
  REQUIRE(prisoner.GetCrimeRecord().jail->arrived);
}
}

TEST_CASE("An arrest's record tells the prisoner's own game its term, its "
          "end on the shared clock, and a login while it stands tells it "
          "again",
          "[Jail][espm]")
{
  PartOne& p = GetPartOne();
  PinnedClock clock(p, 20.f);
  auto& prisoner = Arrive(p);
  Arrest(p, kFalkreath, 3);
  REQUIRE(prisoner.GetCrimeRecord().jail);
  auto told = JailTermsTold(p);
  REQUIRE(told.size() == 1);
  REQUIRE(told[0]["faction"] == kFalkreath);
  REQUIRE(told[0]["releaseAt"].get<double>() == Catch::Approx(23.0));
  REQUIRE(told[0]["served"] == false);

  // a login a day in, its first report from the cell: told again, still
  // serving
  const auto jail = p.JailOf(kFalkreath);
  REQUIRE(jail.has_value());
  clock.Set(21.f);
  prisoner.SetMapMarkersPending(true);
  p.Messages().clear();
  Report(p, prisoner, jail->landingWorldOrCell, jail->landingPos);
  told = JailTermsTold(p);
  REQUIRE(told.size() == 1);
  REQUIRE(told[0]["releaseAt"].get<double>() == Catch::Approx(23.0));
  REQUIRE(told[0]["served"] == false);
  REQUIRE(prisoner.GetCrimeRecord().jail);
  Leave(p);
}

TEST_CASE("A served term is its prisoner's own game's to release: told "
          "served once while the prisoner stays in its cell, the server "
          "moving nothing and giving nothing back",
          "[Jail][espm]")
{
  PartOne& p = GetPartOne();
  PinnedClock clock(p, 20.f);
  auto& prisoner = Arrive(p);
  prisoner.AddItem(kDagger, 1);
  Arrest(p, kFalkreath, 1);
  const auto jail = p.JailOf(kFalkreath);
  REQUIRE(jail.has_value());
  auto& chest = p.worldState.GetFormAt<MpObjectReference>(kBelongings);
  Put(p, kDagger, 1, kBelongings);
  const uint32_t inChest = chest.GetInventory().GetItemCount(kDagger);
  ServeInCell(p, prisoner, *jail);

  // half a day in: still serving, nothing told
  clock.Set(20.5f);
  p.Messages().clear();
  REQUIRE(!p.ReleaseIfServed(prisoner));
  REQUIRE(JailTermsTold(p).empty());

  // the day passed on the shared clock: its game is told the term is
  // served, and the record keeps it where its game has it, the term
  // standing and its belongings in the chest
  clock.Set(21.01f);
  REQUIRE(p.ReleaseIfServed(prisoner));
  const auto told = JailTermsTold(p);
  REQUIRE(told.size() == 1);
  REQUIRE(told[0]["faction"] == kFalkreath);
  REQUIRE(told[0]["releaseAt"].get<double>() == Catch::Approx(21.0));
  REQUIRE(told[0]["served"] == true);
  REQUIRE(prisoner.GetCrimeRecord().jail);
  REQUIRE(prisoner.GetCellOrWorld().ToFormId(p.worldState.espmFiles) ==
          jail->landingWorldOrCell);
  REQUIRE((prisoner.GetPos() - jail->landingPos).Length() < 1.f);
  REQUIRE(prisoner.GetInventory().GetItemCount(kDagger) == 0);
  REQUIRE(chest.GetInventory().GetItemCount(kDagger) == inChest);

  // told once: the tick's next pass tells nothing more
  p.Messages().clear();
  REQUIRE(!p.ReleaseIfServed(prisoner));
  REQUIRE(JailTermsTold(p).empty());
  Leave(p);
}

TEST_CASE("A served prisoner its own game moved out of the jail's cell is "
          "released on the record: its own belongings back, extras and all, "
          "the hold's bounty cleared, and it stays where its game put it",
          "[Jail][espm]")
{
  PartOne& p = GetPartOne();
  PinnedClock clock(p, 20.f);
  auto& prisoner = Arrive(p);
  prisoner.AddItem(kDagger, 2);
  Inventory::Entry tempered(kDagger, 1);
  tempered.health = 1.1f;
  prisoner.AddItems({ tempered });
  const uint32_t held = prisoner.GetInventory().GetItemCount(kDagger);
  REQUIRE(p.OnCrimeGold(prisoner, kFalkreath, 100, 0));
  Arrest(p, kFalkreath, 1);
  const auto jail = p.JailOf(kFalkreath);
  REQUIRE(jail.has_value());
  auto& chest = p.worldState.GetFormAt<MpObjectReference>(kBelongings);
  const uint32_t inChestBefore = chest.GetInventory().GetItemCount(kDagger);
  Put(p, kDagger, 2, kBelongings);
  Put(p, kDagger, 1, kBelongings, 1.1f);
  REQUIRE(prisoner.GetInventory().GetItemCount(kDagger) == held - 3);
  ServeInCell(p, prisoner, *jail);
  clock.Set(21.01f);
  REQUIRE(p.ReleaseIfServed(prisoner));

  // its game's release: the engine's move to the far side of the jail's
  // door, its own move, a little off the door's own point as the engine's
  // settle leaves it. Out after the end is no escape
  const NiPoint3 outside = jail->releasePos + NiPoint3{ 40.f, 0.f, 0.f };
  Report(p, prisoner, jail->releaseWorldOrCell, outside);
  REQUIRE(!p.EndTermIfEscaped(prisoner));
  REQUIRE(prisoner.GetCrimeRecord().jail);

  // the release on the record: the term ends, its three daggers back, the
  // tempered one as it was, the hold's bounty cleared, the prisoner where
  // its own game moved it
  REQUIRE(p.ReleaseIfServed(prisoner));
  REQUIRE(!prisoner.GetCrimeRecord().jail);
  REQUIRE(!prisoner.GetCrimeRecord().Find(
    FormDesc::FromFormId(kFalkreath, p.worldState.espmFiles)));
  REQUIRE(prisoner.GetInventory().GetItemCount(kDagger) == held);
  bool temperedBack = false;
  for (const auto& e : prisoner.GetInventory().entries) {
    temperedBack |= e.EqualExceptCount(tempered);
  }
  REQUIRE(temperedBack);
  REQUIRE(chest.GetInventory().GetItemCount(kDagger) == inChestBefore);
  REQUIRE(prisoner.GetCellOrWorld().ToFormId(p.worldState.espmFiles) ==
          jail->releaseWorldOrCell);
  REQUIRE((prisoner.GetPos() - outside).Length() < 1.f);

  // nothing left to release
  REQUIRE(!p.ReleaseIfServed(prisoner));
  Leave(p);
}

// A prisoner whose sentence ended while it was away logs in where its record
// left it, in the jail's cell. Its own game runs the release once it is in
// the world, so the server tells it the term is served and moves nothing: a
// server placement at the login raced the login's own and lost.
TEST_CASE("A prisoner whose sentence ended while it was away is told so at "
          "its login and left where its login put it, its term standing "
          "until its own game releases it",
          "[Jail][espm]")
{
  PartOne& p = GetPartOne();
  PinnedClock clock(p, 20.f);
  auto& prisoner = Arrive(p);
  Arrest(p, kFalkreath, 1);
  const auto jail = p.JailOf(kFalkreath);
  REQUIRE(jail.has_value());
  ServeInCell(p, prisoner, *jail);

  // away past the sentence's end; the next login marks its first movement
  // pass as SetUserActor does, and its game reports from the cell
  clock.Set(21.01f);
  prisoner.SetMapMarkersPending(true);
  p.Messages().clear();
  const NiPoint3 inCell = jail->landingPos + NiPoint3{ 10.f, 0.f, 0.f };
  Report(p, prisoner, jail->landingWorldOrCell, inCell);
  const auto told = JailTermsTold(p);
  REQUIRE(told.size() == 1);
  REQUIRE(told[0]["faction"] == kFalkreath);
  REQUIRE(told[0]["served"] == true);
  REQUIRE(prisoner.GetCrimeRecord().jail);
  REQUIRE(prisoner.GetCellOrWorld().ToFormId(p.worldState.espmFiles) ==
          jail->landingWorldOrCell);
  REQUIRE((prisoner.GetPos() - inCell).Length() < 1.f);

  // told at the login, so the tick's pass tells nothing more
  p.Messages().clear();
  REQUIRE(!p.ReleaseIfServed(prisoner));
  REQUIRE(JailTermsTold(p).empty());
  Leave(p);
}

TEST_CASE("A prisoner out of its jail before its end escaped: the term ends, "
          "its belongings stay in the chest",
          "[Jail][espm]")
{
  PartOne& p = GetPartOne();
  PinnedClock clock(p, 30.f);
  auto& prisoner = Arrive(p);
  prisoner.AddItem(kDagger, 1);
  Arrest(p, kFalkreath, 3);
  auto& chest = p.worldState.GetFormAt<MpObjectReference>(kBelongings);
  Put(p, kDagger, 1, kBelongings);
  const uint32_t inChest = chest.GetInventory().GetItemCount(kDagger);
  CloseWindow(prisoner);

  // never seen in its jail yet (a slow load, a jump not made): not an
  // escape, the term runs
  REQUIRE(!p.EndTermIfEscaped(prisoner));
  REQUIRE(prisoner.GetCrimeRecord().jail);
  REQUIRE(!prisoner.GetCrimeRecord().jail->arrived);

  // in the jail's cell: serving, its arrival noted
  const auto jail = p.JailOf(kFalkreath);
  REQUIRE(jail.has_value());
  LocationalData inside;
  inside.pos = jail->landingPos;
  inside.rot = jail->landingRot;
  inside.cellOrWorldDesc =
    FormDesc::FromFormId(jail->landingWorldOrCell, p.worldState.espmFiles);
  prisoner.Teleport(inside);
  REQUIRE(!p.EndTermIfEscaped(prisoner));
  REQUIRE(prisoner.GetCrimeRecord().jail);
  REQUIRE(prisoner.GetCrimeRecord().jail->arrived);

  // out in Falkreath before its end: escaped
  LocationalData outside;
  outside.pos = kInFalkreath;
  outside.cellOrWorldDesc =
    FormDesc::FromFormId(kTamriel, p.worldState.espmFiles);
  prisoner.Teleport(outside);
  REQUIRE(p.EndTermIfEscaped(prisoner));
  REQUIRE(!prisoner.GetCrimeRecord().jail);
  REQUIRE(chest.GetInventory().GetItemCount(kDagger) == inChest);
  // and a later end of the shared clock releases nothing
  clock.Set(40.f);
  REQUIRE(!p.ReleaseIfServed(prisoner));
  Leave(p);
}

// A stack's owner is the engine's stolen mark, recorded from the player's own
// game. Its owner here is Falkreath's crime faction 0x28170 (an owner is an
// NPC or a faction)
TEST_CASE("A stack's owner keeps it a stack apart and survives the record's "
          "JSON, and an older record reads none",
          "[Jail]")
{
  Inventory inv;
  Inventory::Entry stolen(kDagger, 1);
  stolen.ownerId = kFalkreath;
  inv.AddItems({ Inventory::Entry(kDagger, 2), stolen });
  REQUIRE(inv.entries.size() == 2);

  simdjson::dom::parser parser;
  const std::string dump = inv.ToJson().dump();
  REQUIRE(Inventory::FromJson(parser.parse(dump).value()) == inv);
  // a stack with none writes no key
  Inventory plain;
  plain.AddItems({ Inventory::Entry(kDagger, 1) });
  REQUIRE(!plain.ToJson()["entries"][0].contains("ownerId"));

  // the migration: a record from before the key reads as none
  const nlohmann::json olderEntry{ { "baseId", kDagger }, { "count", 1 } };
  const nlohmann::json older{ { "entries",
                                nlohmann::json::array({ olderEntry }) } };
  const auto olderInv = Inventory::FromJson(older);
  REQUIRE(olderInv.entries.size() == 1);
  REQUIRE(!olderInv.entries[0].ownerId);
}

TEST_CASE("A reported stolen stack moves the record's unmarked copy when the "
          "record never heard its owner",
          "[Jail]")
{
  Inventory inv;
  inv.AddItems({ Inventory::Entry(kDagger, 2) });
  Inventory::Entry stolen(kDagger, 1);
  stolen.ownerId = kFalkreath;

  // a pickup's or a shop chest's take: the record holds it unmarked
  const auto unmarked = inv.RecordedCopyOf(stolen);
  REQUIRE(!unmarked.ownerId);
  REQUIRE(unmarked.count == 1);
  REQUIRE(unmarked.EqualExceptCount(Inventory::Entry(kDagger, 1)));

  // the record holds it marked: the marked stack
  inv.AddItems({ stolen });
  REQUIRE(inv.RecordedCopyOf(stolen).ownerId == kFalkreath);

  // a plain report is itself
  REQUIRE(inv.RecordedCopyOf(Inventory::Entry(kDagger, 1)) ==
          Inventory::Entry(kDagger, 1));

  // more than either holds: the report, whose removal throws
  Inventory::Entry many = stolen;
  many.count = 5;
  REQUIRE(inv.RecordedCopyOf(many) == many);
}

TEST_CASE("An arrest's strip puts a stolen stack into the evidence chest, "
          "whichever chest its diff names, and the release keeps it there",
          "[Jail][espm]")
{
  PartOne& p = GetPartOne();
  PinnedClock clock(p, 40.f);
  auto& prisoner = Arrive(p);
  const uint32_t held = prisoner.GetInventory().GetItemCount(kDagger);
  prisoner.AddItem(kDagger, 1);
  Inventory::Entry stolen(kDagger, 1);
  stolen.ownerId = kFalkreath;
  prisoner.AddItems({ stolen });
  auto& evidence = p.worldState.GetFormAt<MpObjectReference>(kStolenGoods);
  auto& belongings = p.worldState.GetFormAt<MpObjectReference>(kBelongings);
  const uint32_t inEvidence = evidence.GetInventory().GetItemCount(kDagger);
  const uint32_t inBelongings =
    belongings.GetInventory().GetItemCount(kDagger);

  Arrest(p, kFalkreath, 1);
  REQUIRE(prisoner.GetCrimeRecord().jail);
  // the strip's diff names one chest for all its moves: here the
  // belongings chest for both, the engine's walk (16120) sorting by owner
  Put(p, kDagger, 1, kBelongings, std::nullopt, kFalkreath);
  Put(p, kDagger, 1, kBelongings);
  REQUIRE(prisoner.GetInventory().GetItemCount(kDagger) == held);
  REQUIRE(evidence.GetInventory().GetItemCount(kDagger) == inEvidence + 1);
  REQUIRE(belongings.GetInventory().GetItemCount(kDagger) == inBelongings + 1);
  REQUIRE(prisoner.GetCrimeRecord().jail->belongings.GetItemCount(kDagger) ==
          1);
  bool marked = false;
  for (const auto& e : evidence.GetInventory().entries) {
    marked |= e.EqualExceptCount(stolen);
  }
  REQUIRE(marked);

  // served: the plain dagger back, the stolen one kept in the evidence chest
  clock.Set(41.01f);
  REQUIRE(p.ReleaseIfServed(prisoner));
  REQUIRE(prisoner.GetInventory().GetItemCount(kDagger) == held + 1);
  for (const auto& e : prisoner.GetInventory().entries) {
    REQUIRE(!e.ownerId);
  }
  REQUIRE(evidence.GetInventory().GetItemCount(kDagger) == inEvidence + 1);
  Leave(p);
}

TEST_CASE("A fine's confiscation: an owned stack's put into a hold's evidence "
          "chest is taken from the street, marked on the record or not, and "
          "an unowned one is any put",
          "[Jail][espm]")
{
  PartOne& p = GetPartOne();
  auto& player = Arrive(p);
  // the evidence chests are read from the FACT records, no hold named
  REQUIRE(p.CrimeFactionOfEvidenceChest(kStolenGoods) == kFalkreath);
  REQUIRE(p.CrimeFactionOfEvidenceChest(kBelongings) == 0);
  REQUIRE(p.CrimeFactionOfEvidenceChest(kDagger) == 0);

  const uint32_t held = player.GetInventory().GetItemCount(kDagger);
  Inventory::Entry stolen(kDagger, 1);
  stolen.ownerId = kFalkreath;
  player.AddItems({ stolen });
  // a pickup's: its game marked it, the record never heard the owner
  player.AddItem(kDagger, 1);
  auto& evidence = p.worldState.GetFormAt<MpObjectReference>(kStolenGoods);
  const uint32_t inEvidence = evidence.GetInventory().GetItemCount(kDagger);

  // no arrest, no window: the engine's walk at a fine paid with removeStolen
  Put(p, kDagger, 1, kStolenGoods, std::nullopt, kFalkreath);
  Put(p, kDagger, 1, kStolenGoods, std::nullopt, kFalkreath);
  REQUIRE(player.GetInventory().GetItemCount(kDagger) == held);
  REQUIRE(evidence.GetInventory().GetItemCount(kDagger) == inEvidence + 2);
  for (const auto& e : evidence.GetInventory().entries) {
    if (e.baseId == kDagger && e.ownerId) {
      REQUIRE(*e.ownerId == kFalkreath);
    }
  }
  // more than the record holds moves nothing (conservation)
  Put(p, kDagger, 50, kStolenGoods, std::nullopt, kFalkreath);
  REQUIRE(player.GetInventory().GetItemCount(kDagger) == held);

  // an unowned put into the evidence chest from the street is any put: out
  // of reach
  player.AddItem(kDagger, 1);
  Put(p, kDagger, 1, kStolenGoods);
  REQUIRE(player.GetInventory().GetItemCount(kDagger) == held + 1);
  REQUIRE(evidence.GetInventory().GetItemCount(kDagger) == inEvidence + 2);
  Leave(p);
}

TEST_CASE("A fine's gold leaves the record: the gold the player's engine "
          "paid as it set the bounty, at most the bounty recorded before and "
          "the gold the record holds; a bribe pays part of it the same way, "
          "and more than either is refused with the bounty still set",
          "[Jail][espm]")
{
  // Skyrim.esm's MISC Gold001
  constexpr uint32_t kGold001 = 0x0000000f;
  PartOne& p = GetPartOne();
  auto& player = Arrive(p);
  const auto falkreath =
    FormDesc::FromFormId(kFalkreath, p.worldState.espmFiles);
  const auto gold = [&] {
    return player.GetInventory().GetItemCount(kGold001);
  };
  const auto bounty = [&] {
    const auto record = player.GetCrimeRecord();
    const CrimeGold* held = record.Find(falkreath);
    return held ? held->violent + held->nonViolent : 0u;
  };
  player.AddItem(kGold001, 140);
  const uint32_t start = gold();

  // a fine: the whole bounty, in the game's own message
  REQUIRE(p.OnCrimeGold(player, kFalkreath, 0, 40));
  REQUIRE(bounty() == 40);
  DoMessage(p, 0,
            nlohmann::json{ { "t", MsgType::CrimeGold },
                            { "faction", kFalkreath },
                            { "violent", 0 },
                            { "nonViolent", 0 },
                            { "paidGold", 40 } });
  REQUIRE(gold() == start - 40);
  REQUIRE(bounty() == 0);

  // a bribe: part of the bounty
  REQUIRE(p.OnCrimeGold(player, kFalkreath, 0, 40));
  p.OnCrimeGold(player, kFalkreath, 0, 0, 20);
  REQUIRE(gold() == start - 60);
  REQUIRE(bounty() == 0);

  // more than the bounty recorded before the report: refused, the bounty
  // the game reports still recorded
  REQUIRE(p.OnCrimeGold(player, kFalkreath, 0, 10));
  p.OnCrimeGold(player, kFalkreath, 0, 0, 11);
  REQUIRE(gold() == start - 60);
  REQUIRE(bounty() == 0);

  // more than the record's gold: refused
  REQUIRE(p.OnCrimeGold(player, kFalkreath, 1000, 0));
  p.OnCrimeGold(player, kFalkreath, 0, 0, start);
  REQUIRE(gold() == start - 60);
  REQUIRE(bounty() == 0);

  Leave(p);
}
