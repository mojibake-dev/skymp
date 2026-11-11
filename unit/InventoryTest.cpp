#include "Inventory.h"
#include <catch2/catch_all.hpp>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

TEST_CASE(
  "RemoveItems in case of multiple entries with the same baseId/extras",
  "[Inventory]")
{
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(0xf, 10));
  inv.entries.push_back(Inventory::Entry(0xf, 10));

  inv.RemoveItems({ Inventory::Entry(0xf, 5) });

  REQUIRE(inv.entries.size() == 2);
  REQUIRE(inv.entries[0].count == 5);
  REQUIRE(inv.entries[1].count == 10);
}

TEST_CASE("Similar entries should be merged if needed to remove items",
          "[Inventory]")
{
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(0xf, 10));
  inv.entries.push_back(Inventory::Entry(0xf, 10));

  inv.RemoveItems({ Inventory::Entry(0xf, 15) });

  REQUIRE(inv.entries.size() == 1);
  REQUIRE(inv.entries[0].count == 5);
}

TEST_CASE("Remove all items from similar entries", "[Inventory]")
{
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(0xf, 10));
  inv.entries.push_back(Inventory::Entry(0xf, 10));

  inv.RemoveItems({ Inventory::Entry(0xf, 20) });

  REQUIRE(inv.entries.size() == 0);
}

TEST_CASE("Remove all items from a single entry", "[Inventory]")
{
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(0xf, 10));

  inv.RemoveItems({ Inventory::Entry(0xf, 10) });

  REQUIRE(inv.entries.size() == 0);
}

TEST_CASE("Not enough items to remove", "[Inventory]")
{
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(0xf, 10));
  inv.entries.push_back(Inventory::Entry(0xf, 9));

  REQUIRE_THROWS_AS(inv.RemoveItems({ Inventory::Entry(0xf, 20) }),
                    std::runtime_error);
}

// thuum playtest 50 (the undressed bandits): AddItems returned at the first
// item that merged into a stack and dropped every later item of the call.
// Skyrim.esm's Gold001 0xf and the iron helmet 0x12e4d and boots 0x12e4b (the
// ids TES5DamageFormulaTest's armor case reads); Inventory reads no records
namespace {
constexpr uint32_t kGold = 0xf;
constexpr uint32_t kIronHelmet = 0x12e4d;
constexpr uint32_t kIronBoots = 0x12e4b;
}

TEST_CASE("AddItems adds the items after one that merges into a stack",
          "[Inventory]")
{
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(kGold, 32));

  inv.AddItems(
    { Inventory::Entry(kGold, 8), Inventory::Entry(kIronHelmet, 1) });

  REQUIRE(inv.entries.size() == 2);
  REQUIRE(inv.entries[0] == Inventory::Entry(kGold, 40));
  REQUIRE(inv.entries[1] == Inventory::Entry(kIronHelmet, 1));
}

TEST_CASE("AddItems merges a middle item and appends the items around it in "
          "the call's order",
          "[Inventory]")
{
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(kGold, 32));

  inv.AddItems({ Inventory::Entry(kIronHelmet, 1), Inventory::Entry(kGold, 8),
                 Inventory::Entry(kIronBoots, 1) });

  REQUIRE(inv.entries.size() == 3);
  REQUIRE(inv.entries[0] == Inventory::Entry(kGold, 40));
  REQUIRE(inv.entries[1] == Inventory::Entry(kIronHelmet, 1));
  REQUIRE(inv.entries[2] == Inventory::Entry(kIronBoots, 1));
}

TEST_CASE("AddItems matches each item to a stack by its extra data too",
          "[Inventory]")
{
  Inventory::ExtraData worn;
  worn.worn_ = true;
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(kGold, 32));
  inv.entries.push_back(Inventory::Entry(kIronHelmet, 1));

  // the worn helmet matches no stack and is appended; the plain one after it
  // joins the plain stack
  inv.AddItems({ Inventory::Entry(kGold, 8),
                 Inventory::Entry(kIronHelmet, 1, worn),
                 Inventory::Entry(kIronHelmet, 1) });

  REQUIRE(inv.entries.size() == 3);
  REQUIRE(inv.entries[0] == Inventory::Entry(kGold, 40));
  REQUIRE(inv.entries[1] == Inventory::Entry(kIronHelmet, 2));
  REQUIRE(inv.entries[2] == Inventory::Entry(kIronHelmet, 1, worn));
}

// thuum train 52 (D-R1 C's leveled gap; the fresh review of the whole branch,
// SHOULD 1): AddPlainItems writes a leveled roll's landings in one pass over
// the stacks and must give AddItems' result. The iron dagger 0x1397e is
// DropItemTest's; a tempered stack is one with its health set
namespace {
constexpr uint32_t kIronDagger = 0x1397e;

// a tempered dagger, then gold, then a plain dagger stack after the tempered
// one
Inventory DaggerRecord()
{
  Inventory::ExtraData tempered;
  tempered.health = 1.1f;
  Inventory inv;
  inv.entries.push_back(Inventory::Entry(kIronDagger, 1, tempered));
  inv.entries.push_back(Inventory::Entry(kGold, 32));
  inv.entries.push_back(Inventory::Entry(kIronDagger, 2));
  return inv;
}
}

TEST_CASE("AddPlainItems gives AddItems' result: a plain item joins the plain "
          "stack after a tempered one, a duplicate base joins the stack its "
          "first item made, and an item with extra data takes AddItems' own "
          "rule",
          "[Inventory]")
{
  Inventory::ExtraData worn;
  worn.worn_ = true;
  const std::vector<Inventory::Entry> toAdd = {
    // joins the plain dagger stack, never the tempered one before it
    Inventory::Entry(kIronDagger, 3),
    // a base the record lacks, appended, then its duplicate joining it
    Inventory::Entry(kIronHelmet, 1),
    Inventory::Entry(kIronHelmet, 4),
    // not plain: appended by AddItems' rule, and no plain stack for the
    // plain boots after it, which are appended too
    Inventory::Entry(kIronBoots, 1, worn),
    Inventory::Entry(kIronBoots, 2),
    Inventory::Entry(kGold, 8),
  };
  Inventory added = DaggerRecord();
  added.AddItems(toAdd);
  Inventory plain = DaggerRecord();
  plain.AddPlainItems(toAdd);
  REQUIRE(plain == added);

  Inventory::ExtraData tempered;
  tempered.health = 1.1f;
  REQUIRE(plain.entries.size() == 6);
  REQUIRE(plain.entries[0] == Inventory::Entry(kIronDagger, 1, tempered));
  REQUIRE(plain.entries[1] == Inventory::Entry(kGold, 40));
  REQUIRE(plain.entries[2] == Inventory::Entry(kIronDagger, 5));
  REQUIRE(plain.entries[3] == Inventory::Entry(kIronHelmet, 5));
  REQUIRE(plain.entries[4] == Inventory::Entry(kIronBoots, 1, worn));
  REQUIRE(plain.entries[5] == Inventory::Entry(kIronBoots, 2));
}

TEST_CASE("AddPlainItems gives AddItems' result on seeded random records and "
          "calls",
          "[Inventory]")
{
  // the equivalence a 400,000-case scratch harness read before fork
  // 1a70a16b, kept here in 2,000 seeded cases: up to twelve stacks and nine
  // items over four bases, a stack or an item tempered, named, worn or
  // logically plain (worn_ false), and half the calls plain items alone.
  // mt19937's output is the same on every standard library and each draw is
  // its own statement, so every platform runs the same cases
  std::mt19937 g(20261111);
  const auto draw = [&](uint32_t n) { return static_cast<uint32_t>(g() % n); };
  const auto entry = [&](bool plainOnly) {
    Inventory::ExtraData extra;
    const uint32_t kind = plainOnly ? 0 : draw(5);
    if (kind == 1) {
      extra.health = 1.5f;
    } else if (kind == 2) {
      extra.name = std::string("Ancient Nord Dagger");
    } else if (kind == 3) {
      extra.worn_ = true;
    } else if (kind == 4) {
      extra.worn_ = false;
    }
    const uint32_t base = kIronDagger + draw(4);
    const uint32_t count = 1 + draw(5);
    return Inventory::Entry(base, count, extra);
  };
  for (int i = 0; i < 2000; ++i) {
    INFO("case " << i);
    Inventory record;
    const uint32_t stacks = draw(13);
    for (uint32_t s = 0; s < stacks; ++s) {
      record.entries.push_back(entry(false));
    }
    const bool plainOnly = draw(2) == 0;
    std::vector<Inventory::Entry> toAdd;
    const uint32_t items = draw(10);
    for (uint32_t k = 0; k < items; ++k) {
      toAdd.push_back(entry(plainOnly));
    }
    Inventory added = record;
    added.AddItems(toAdd);
    Inventory plain = record;
    plain.AddPlainItems(toAdd);
    REQUIRE(plain == added);
  }
}
