#include "MpChangeForms.h"
#include <catch2/catch_all.hpp>
#include <simdjson.h>

// A change form's optional keys from two features in one record: an
// inventory entry's created forms (createdBase, createdEnchantment,
// createdPoison) and the actor's knownEnchantments, beside an entry's ownerId
// and the actor's crimeRecord. Each is read only where present, so a record
// written before any of them reads all as none; a record carrying one set
// keeps the other none; a record carrying both keeps every field, a jailed
// player's belongings included.
namespace {
// Skyrim.esm: SteelSword 0x13989 (PartOne_UpdateEquipmentTest's),
// EnchFireDamageFFContact 0x4605a, the fire enchantment's MGEF
// (CraftReportTest's), and the crime faction CrimeFactionFalkreath 0x28170
// (JailTest's)
constexpr uint32_t kSteelSword = 0x00013989;
constexpr uint32_t kFireDamage = 0x0004605a;
constexpr uint32_t kOwnerFaction = 0x00028170;
// an ENCH of Skyrim.esm (CraftReportTest's kBootsEnchantment) as a learned
// enchantment, and CrimeFactionWhiterun 0x267ea (StandingTest's) as a second
// hold
const char* const kKnownEnchantment = "ad484:Skyrim.esm";
const char* const kFalkreathCrime = "28170:Skyrim.esm";
const char* const kWhiterunCrime = "267ea:Skyrim.esm";

CreatedForm FireEnchantment()
{
  CreatedForm form;
  form.kind = CreatedForm::kWeaponEnchantment;
  form.effects = { CreatedEffect{ kFireDamage, 10.f, 0, 0, 30.f } };
  return form;
}

Inventory::Entry EnchantedSword(std::optional<uint32_t> owner)
{
  Inventory::Entry e(kSteelSword, 1);
  e.createdEnchantment = FireEnchantment();
  e.maxCharge = 2400.f;
  e.ownerId = owner;
  return e;
}

CrimeRecord Bounty()
{
  CrimeRecord record;
  record.Set(FormDesc::FromString(kFalkreathCrime), 1000, 40);
  JailTerm term;
  term.faction = FormDesc::FromString(kFalkreathCrime);
  term.days = 7;
  term.releaseAt = 12.5;
  term.belongings.AddItems({ EnchantedSword(kOwnerFaction) });
  term.arrived = true;
  record.jail = term;
  return record;
}

MpChangeForm RoundTrip(const MpChangeForm& changeForm)
{
  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  return MpChangeForm::JsonToChangeForm(element);
}
}

TEST_CASE("A record from before the optional keys reads every one as none and "
          "writes none back",
          "[ChangeForm]")
{
  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("knownEnchantments"));
  REQUIRE(!older.contains("crimeRecord"));
  const nlohmann::json plainEntry = { { "baseId", kSteelSword },
                                      { "count", 1 } };
  older["inv"] = { { "entries", nlohmann::json::array({ plainEntry }) } };

  simdjson::dom::parser parser;
  const std::string dump = older.dump();
  auto element = parser.parse(dump).value();
  const auto loaded = MpChangeForm::JsonToChangeForm(element);
  REQUIRE(!loaded.knownEnchantments);
  REQUIRE(!loaded.crimeRecord);
  REQUIRE(loaded.inv.entries.size() == 1);
  const auto& entry = loaded.inv.entries[0];
  REQUIRE(!entry.createdBase);
  REQUIRE(!entry.createdEnchantment);
  REQUIRE(!entry.createdPoison);
  REQUIRE(!entry.ownerId);

  const nlohmann::json written = MpChangeForm::ToJson(loaded);
  REQUIRE(!written.contains("knownEnchantments"));
  REQUIRE(!written.contains("crimeRecord"));
  const auto& writtenEntry = written["inv"]["entries"][0];
  REQUIRE(!writtenEntry.contains("createdEnchantment"));
  REQUIRE(!writtenEntry.contains("ownerId"));
}

TEST_CASE("A record carrying one feature's keys keeps them and reads the "
          "other's as none",
          "[ChangeForm]")
{
  MpChangeForm crafted;
  crafted.recType = MpChangeForm::ACHR;
  crafted.inv.AddItems({ EnchantedSword(std::nullopt) });
  crafted.knownEnchantments =
    std::vector<FormDesc>{ FormDesc::FromString(kKnownEnchantment) };
  const auto craftedLoaded = RoundTrip(crafted);
  REQUIRE(craftedLoaded.inv == crafted.inv);
  REQUIRE(craftedLoaded.inv.entries[0].createdEnchantment ==
          FireEnchantment());
  REQUIRE(craftedLoaded.knownEnchantments == crafted.knownEnchantments);
  REQUIRE(!craftedLoaded.crimeRecord);
  REQUIRE(!craftedLoaded.inv.entries[0].ownerId);

  MpChangeForm jailed;
  jailed.recType = MpChangeForm::ACHR;
  Inventory::Entry owned(kSteelSword, 1);
  owned.ownerId = kOwnerFaction;
  jailed.inv.AddItems({ owned });
  CrimeRecord record;
  record.Set(FormDesc::FromString(kWhiterunCrime), 0, 5);
  jailed.crimeRecord = record;
  const auto jailedLoaded = RoundTrip(jailed);
  REQUIRE(jailedLoaded.inv == jailed.inv);
  REQUIRE(jailedLoaded.inv.entries[0].ownerId == kOwnerFaction);
  REQUIRE(jailedLoaded.crimeRecord == jailed.crimeRecord);
  REQUIRE(!jailedLoaded.knownEnchantments);
  REQUIRE(!jailedLoaded.inv.entries[0].createdEnchantment);
}

TEST_CASE("A record carrying both features' keys keeps every one, and an "
          "owned stack stays apart from an unowned one of the same made "
          "enchantment",
          "[ChangeForm]")
{
  MpChangeForm both;
  both.recType = MpChangeForm::ACHR;
  both.inv.AddItems(
    { EnchantedSword(kOwnerFaction), EnchantedSword(std::nullopt) });
  REQUIRE(both.inv.entries.size() == 2);
  both.knownEnchantments =
    std::vector<FormDesc>{ FormDesc::FromString(kKnownEnchantment) };
  both.crimeRecord = Bounty();

  const auto loaded = RoundTrip(both);
  REQUIRE(loaded.inv == both.inv);
  REQUIRE(loaded.inv.entries.size() == 2);
  REQUIRE(loaded.knownEnchantments == both.knownEnchantments);
  REQUIRE(loaded.crimeRecord == both.crimeRecord);
  REQUIRE(loaded.crimeRecord->jail->belongings.entries[0].createdEnchantment ==
          FireEnchantment());
  REQUIRE(loaded.crimeRecord->jail->belongings.entries[0].ownerId ==
          kOwnerFaction);
}
