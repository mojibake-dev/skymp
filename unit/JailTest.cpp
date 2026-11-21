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

// thuum tracker #97, ADR-079 (Eli, 2026-11-20: "yes real jail time"): a
// player's bounty per hold and its jail term, the server's record; an
// arrest the arrested player's own game reports. Falkreath's records
// (Skyrim.esm through lab/esm.py on sky-srv, 2026-11-20): the crime faction
// CrimeFactionFalkreath 0x28170 names its jail marker 0x3ef17 (JAIL), a REFR
// in Tamriel 0x3c whose XTEL lands by door 0x3ef07, its stolen-goods chest
// 0xef436 (STOL), its prisoner-belongings chest 0xef437 (PLCN) and the jail
// outfit BeggarOutfit 0x28b60 (JOUT). Playtest 32's arrest there (tracker #97
// note 3702): the chest stands in FalkreathBarracksJail 0xef324, the cell the
// jail's teleport loaded, and the outfit put on was ClothesBeggarRobes
// 0x13105 and ClothesBeggarBoots 0x13106.
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

  // rule 6's migration: a record from before the key reads as none, and a
  // record with none writes no key
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
         std::optional<float> health = std::nullopt)
{
  nlohmann::json j{ { "t", MsgType::PutItem },
                    { "baseId", baseId },
                    { "count", count },
                    { "target", target } };
  if (health) {
    j["health"] = *health;
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

TEST_CASE("An arrest records the term on the shared clock, permits the jump "
          "to the hold's jail and takes its game's strip and outfit",
          "[Jail][espm]")
{
  PartOne& p = GetPartOne();
  PinnedClock clock(p, 10.f);
  auto& prisoner = Arrive(p);
  // the player's base may bring daggers of its own (pipeline 6414: one more
  // than added), so the counts are read against the start
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

  // the engine's move into the jail is the one jump permitted
  REQUIRE(p.CheckJump(kPrisoner, kJailCell, 0.f, 0.f) ==
          PartOne::JumpCheck::Landed);

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
  // put back, or disposed of, it is not made again (the second review of
  // cd890dbed, the mint loop)
  Put(p, kRobes, 1, kStolenGoods);
  REQUIRE(prisoner.GetInventory().GetItemCount(kRobes) == 1);
  prisoner.RemoveItem(kRobes, 1, nullptr);
  Take(p, kRobes, 1, kStolenGoods);
  REQUIRE(prisoner.GetInventory().GetItemCount(kRobes) == 0);

  Leave(p);
}

TEST_CASE("A sentence ends when the shared clock passes it, never before, and "
          "gives back the prisoner's own belongings, extras and all",
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
  // the same bounty again changes nothing
  REQUIRE(!p.OnCrimeGold(prisoner, kFalkreath, 100, 0));
  // a form that is no faction holds no bounty
  REQUIRE(!p.OnCrimeGold(prisoner, kDagger, 100, 0));
  Arrest(p, kFalkreath, 1);
  REQUIRE(prisoner.GetCrimeRecord().jail);
  auto& chest = p.worldState.GetFormAt<MpObjectReference>(kBelongings);
  const uint32_t inChestBefore = chest.GetInventory().GetItemCount(kDagger);
  Put(p, kDagger, 2, kBelongings);
  Put(p, kDagger, 1, kBelongings, 1.1f);
  REQUIRE(prisoner.GetInventory().GetItemCount(kDagger) == held - 3);

  // half a day in: still serving, and the tick's escape check leaves a
  // prisoner its window still holds
  clock.Set(20.5f);
  REQUIRE(!p.ReleaseIfServed(prisoner));
  REQUIRE(!p.EndTermIfEscaped(prisoner));
  REQUIRE(prisoner.GetCrimeRecord().jail);

  // the day passed on the shared clock: released, its three daggers back,
  // the tempered one as it was, the hold's bounty cleared, placed at the
  // jail marker
  clock.Set(21.01f);
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
  const auto jail = p.JailOf(kFalkreath);
  REQUIRE(jail.has_value());
  REQUIRE(prisoner.GetCellOrWorld().ToFormId(p.worldState.espmFiles) ==
          jail->markerWorldOrCell);
  REQUIRE((prisoner.GetPos() - jail->markerPos).Length() < 1.f);

  // nothing left to release
  REQUIRE(!p.ReleaseIfServed(prisoner));
  Leave(p);
}

// thuum tracker #97 (Eli's jail look, 2026-11-20): a prisoner whose sentence
// the shared clock ended while it was away is released at its first movement
// report after the login, and that report, sent from the jail's cell, must
// not put the record back there: rotfern was released into Whiterun and
// sent back into the jail's cell, the login pass's release run before the
// report's own write overwrote it
TEST_CASE("A sentence that ended while its prisoner was away releases it at "
          "its first report after the login, and the report from the cell "
          "leaves the record at the marker",
          "[Jail][espm]")
{
  PartOne& p = GetPartOne();
  PinnedClock clock(p, 20.f);
  auto& prisoner = Arrive(p);
  Arrest(p, kFalkreath, 1);
  REQUIRE(prisoner.GetCrimeRecord().jail);
  const auto jail = p.JailOf(kFalkreath);
  REQUIRE(jail.has_value());
  // the arrest's jump into the jail's cell (the login's first report, the
  // sentence not yet over)
  Report(p, prisoner, jail->landingWorldOrCell, jail->landingPos);
  REQUIRE(prisoner.GetCellOrWorld().ToFormId(p.worldState.espmFiles) ==
          jail->landingWorldOrCell);
  REQUIRE(prisoner.GetCrimeRecord().jail);

  // away past the sentence's end; the next login marks its first movement
  // pass as SetUserActor does, and its game reports from the cell
  clock.Set(21.01f);
  prisoner.SetMapMarkersPending(true);
  const NiPoint3 inCell = jail->landingPos + NiPoint3{ 10.f, 0.f, 0.f };
  Report(p, prisoner, jail->landingWorldOrCell, inCell);
  REQUIRE(!prisoner.GetCrimeRecord().jail);
  REQUIRE(prisoner.GetCellOrWorld().ToFormId(p.worldState.espmFiles) ==
          jail->markerWorldOrCell);
  REQUIRE((prisoner.GetPos() - jail->markerPos).Length() < 1.f);
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
