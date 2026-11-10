#include "Inventory.h"
#include <catch2/catch_all.hpp>

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
