#include "ActionListener.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <chrono>

#include "HitMessage.h"
#include "MpChangeForms.h"
#include <simdjson.h>

PartOne& GetPartOne();
using namespace std::chrono_literals;

// thuum docs/verbs/poison.md: a poison's OnEquip puts it on the weapon worn
// in the right hand (the game's box; the client reports the game's
// PoisonedWeapon event as the poison's OnEquip): one poison spent and the
// weapon marked with the server's dose count, never a drink; a hit with the
// marked weapon runs the poison's effects on its target and spends the
// dose. Skyrim.esm (thuum lab/esm.py): DB03Poison 0x58CFB, playtest
// thirteen's; the Iron Dagger 0x1397E, HitTest's. The player poisons its
// own dagger and hits itself, as HitTest's first case hits itself.
namespace {
constexpr uint32_t kActor = 0xff000a01;
constexpr uint32_t kPoison = 0x00058cfb;
constexpr uint32_t kDagger = 0x0001397e;
// gold, after the dagger: a dagger entry moved to the end shows
constexpr uint32_t kGold = 0x0000000f;

Inventory::ExtraData Worn(bool right)
{
  Inventory::ExtraData extra;
  if (right) {
    extra.worn_ = true;
  } else {
    extra.wornLeft = true;
  }
  return extra;
}

// A player holding the dagger (worn in the right hand, or the left) and one
// poison
MpActor& Poisoner(PartOne& p, bool daggerInRight)
{
  DoConnect(p, 0);
  p.CreateActor(kActor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kActor);
  auto& ac = p.worldState.GetFormAt<MpActor>(kActor);
  ac.RemoveAllItems();
  ac.AddItem(kDagger, 1);
  ac.AddItem(kGold, 10);
  ac.AddItem(kPoison, 1);
  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(kDagger, 1, Worn(daggerInRight)));
  ac.SetEquipment(eq);
  p.Messages().clear();
  return ac;
}

const Inventory::Entry* DaggerEntry(const MpActor& ac)
{
  for (const auto& entry : ac.GetInventory().entries) {
    if (entry.baseId == kDagger && entry.count > 0) {
      return &entry;
    }
  }
  return nullptr;
}

// where the dagger stands among the entries: marking and spending keep it
// there (the legacy stack's drink does, and difftest compares the order)
size_t DaggerIndex(const MpActor& ac)
{
  const auto& entries = ac.GetInventory().entries;
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].baseId == kDagger && entries[i].count > 0) {
      return i;
    }
  }
  return entries.size();
}

bool PoisonRuns(const MpActor& ac)
{
  for (const auto& effect : ac.GetRunningEffects()) {
    if (effect.source == kPoison) {
      return true;
    }
  }
  return false;
}

// the player hits itself with the dagger
void HitSelf(PartOne& p, MpActor& ac)
{
  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.target = 0x14;
  hit.data.aggressor = 0x14;
  hit.data.source = kDagger;
  ac.SetLastHitTime(kActor, std::chrono::steady_clock::now() - 10s);
  p.GetActionListener().OnHit(raw, hit);
}

void Leave(PartOne& p)
{
  p.DestroyActor(kActor);
  DoDisconnect(p, 0);
}
}

TEST_CASE("A poison's OnEquip marks the right-hand weapon and spends the "
          "poison",
          "[Poison][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Poisoner(p, true);
  const auto index = DaggerIndex(ac);

  REQUIRE(ac.OnEquip(kPoison));
  REQUIRE(ac.GetInventory().GetItemCount(kPoison) == 0);
  REQUIRE(DaggerIndex(ac) == index);
  const auto* dagger = DaggerEntry(ac);
  REQUIRE(dagger);
  REQUIRE(dagger->poisonId == kPoison);
  REQUIRE(dagger->poisonCount == 1u);
  // never a drink
  REQUIRE(!PoisonRuns(ac));

  // a second poison on the poisoned dagger: refused, and kept
  ac.AddItem(kPoison, 1);
  REQUIRE(!ac.OnEquip(kPoison));
  REQUIRE(ac.GetInventory().GetItemCount(kPoison) == 1);
  Leave(p);
}

TEST_CASE("Without a weapon in the right hand a poison changes nothing",
          "[Poison][espm]")
{
  PartOne& p = GetPartOne();
  // the dagger in the left hand only, as the game refuses it
  auto& ac = Poisoner(p, false);
  REQUIRE(!ac.OnEquip(kPoison));
  REQUIRE(ac.GetInventory().GetItemCount(kPoison) == 1);
  REQUIRE(DaggerEntry(ac));
  REQUIRE(!DaggerEntry(ac)->poisonId);

  // nothing worn
  ac.SetEquipment(Equipment());
  REQUIRE(!ac.OnEquip(kPoison));
  REQUIRE(ac.GetInventory().GetItemCount(kPoison) == 1);

  // the dagger in the right hand, the poison not in the server's inventory
  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(kDagger, 1, Worn(true)));
  ac.SetEquipment(eq);
  ac.RemoveItem(kPoison, 1, nullptr);
  REQUIRE(!ac.OnEquip(kPoison));
  REQUIRE(!DaggerEntry(ac)->poisonId);
  Leave(p);
}

TEST_CASE("A poisoned weapon's hit runs the poison on its target and spends "
          "the dose",
          "[Poison][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Poisoner(p, true);
  const auto index = DaggerIndex(ac);
  REQUIRE(ac.OnEquip(kPoison));

  HitSelf(p, ac);
  REQUIRE(PoisonRuns(ac));
  REQUIRE(DaggerEntry(ac));
  REQUIRE(!DaggerEntry(ac)->poisonId);
  REQUIRE(DaggerIndex(ac) == index);
  // one dose: nothing left to spend
  REQUIRE(!ac.SpendPoisonDose(kDagger));
  Leave(p);
}

TEST_CASE("The change form keeps a weapon's poison and doses", "[Poison]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;
  Inventory::ExtraData extra;
  extra.poisonId = kPoison;
  extra.poisonCount = 1;
  changeForm.inv.AddItems({ Inventory::Entry(kDagger, 1, extra) });

  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  const auto loaded = MpChangeForm::JsonToChangeForm(element);
  REQUIRE(loaded.inv.entries.size() == 1);
  REQUIRE(loaded.inv.entries[0].poisonId == kPoison);
  REQUIRE(loaded.inv.entries[0].poisonCount == 1u);
}
