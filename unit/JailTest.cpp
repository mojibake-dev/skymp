#include "MpChangeForms.h"
#include "TestUtils.hpp"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <functional>
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

TEST_CASE("An arrest records the term on the shared clock, permits the jump "
          "to the hold's jail and takes the strip and the outfit",
          "[Jail][espm]")
{
  PartOne& p = GetPartOne();
  PinnedClock clock(p, 10.f);
  auto& prisoner = Arrive(p);
  prisoner.AddItem(kDagger, 3);

  // the sentence's bounds: the game's 1 to 7 days
  REQUIRE(p.OnArrest(prisoner, kFalkreath, 0) ==
          PartOne::ArrestCheck::BadSentence);
  REQUIRE(p.OnArrest(prisoner, kFalkreath, 8) ==
          PartOne::ArrestCheck::BadSentence);
  REQUIRE(p.OnArrest(prisoner, kPlayerFaction, 3) ==
          PartOne::ArrestCheck::NoJail);
  REQUIRE(!prisoner.GetCrimeRecord().jail);
  REQUIRE(!prisoner.GetArrestWindow());

  REQUIRE(p.OnArrest(prisoner, kFalkreath, 7) ==
          PartOne::ArrestCheck::Recorded);
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

  // the strip: into the faction's belongings chest from the street, no
  // chest open, recorded as the prisoner's own; a chest of another's is
  // still out of reach
  REQUIRE(prisoner.ArrestStripTakes(kBelongings));
  REQUIRE(prisoner.ArrestStripTakes(kStolenGoods));
  REQUIRE(!prisoner.ArrestStripTakes(kMarker));
  auto& chest = p.worldState.GetFormAt<MpObjectReference>(kBelongings);
  chest.PutItem(prisoner, Inventory::Entry(kDagger, 2));
  REQUIRE(prisoner.GetInventory().GetItemCount(kDagger) == 1);
  REQUIRE(chest.GetInventory().GetItemCount(kDagger) >= 2);
  REQUIRE(prisoner.GetCrimeRecord().jail->belongings.GetItemCount(kDagger) ==
          2);

  // the outfit: its items, none else
  REQUIRE(prisoner.ArrestOutfitTakes(kRobes));
  REQUIRE(prisoner.ArrestOutfitTakes(kBoots));
  REQUIRE(!prisoner.ArrestOutfitTakes(kDagger));

  Leave(p);
}

TEST_CASE("A sentence ends when the shared clock passes it, never before, and "
          "gives back the prisoner's own belongings",
          "[Jail][espm]")
{
  PartOne& p = GetPartOne();
  PinnedClock clock(p, 20.f);
  auto& prisoner = Arrive(p);
  prisoner.AddItem(kDagger, 2);
  REQUIRE(p.OnCrimeGold(prisoner, kFalkreath, 100, 0));
  // the same bounty again changes nothing
  REQUIRE(!p.OnCrimeGold(prisoner, kFalkreath, 100, 0));
  // a form that is no faction holds no bounty
  REQUIRE(!p.OnCrimeGold(prisoner, kDagger, 100, 0));
  REQUIRE(p.OnArrest(prisoner, kFalkreath, 1) ==
          PartOne::ArrestCheck::Recorded);
  auto& chest = p.worldState.GetFormAt<MpObjectReference>(kBelongings);
  const uint32_t inChestBefore = chest.GetInventory().GetItemCount(kDagger);
  chest.PutItem(prisoner, Inventory::Entry(kDagger, 2));
  REQUIRE(prisoner.GetInventory().GetItemCount(kDagger) == 0);

  // half a day in: still serving
  clock.Set(20.5f);
  REQUIRE(!p.ReleaseIfServed(prisoner));
  REQUIRE(prisoner.GetCrimeRecord().jail);

  // the day passed on the shared clock: released, its two daggers back and
  // no more, the hold's bounty cleared, placed at the jail marker
  clock.Set(21.01f);
  REQUIRE(p.ReleaseIfServed(prisoner));
  REQUIRE(!prisoner.GetCrimeRecord().jail);
  REQUIRE(!prisoner.GetCrimeRecord().Find(
    FormDesc::FromFormId(kFalkreath, p.worldState.espmFiles)));
  REQUIRE(prisoner.GetInventory().GetItemCount(kDagger) == 2);
  REQUIRE(chest.GetInventory().GetItemCount(kDagger) == inChestBefore);
  REQUIRE(prisoner.GetCellOrWorld().ToFormId(p.worldState.espmFiles) ==
          kTamriel);

  // nothing left to release
  REQUIRE(!p.ReleaseIfServed(prisoner));
  Leave(p);
}
