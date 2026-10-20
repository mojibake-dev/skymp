#include "FormDesc.h"
#include "MpChangeForms.h"
#include "database_drivers/FileDatabase.h"
#include "database_drivers/LoadOrderFile.h"
#include "database_drivers/ZipDatabase.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <filesystem>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <simdjson.h>
#include <spdlog/spdlog.h>

// thuum docs/DECISIONS.md ADR-043: the world stores every form as
// "<hex>:<plugin>", and a load order change migrates a world instead of
// undressing it. The orders here have the light-plugin fixtures' shape
// (unit/LightPluginsTest.cpp: full and light plugins numbered apart); the
// conversion looks no record up, so no game files are needed.

namespace {

// The order a world was saved under
EspmFileList OrderA()
{
  return EspmFileList({ "Skyrim.esm", "Update.esm", "rotfern.esp",
                        "Apocalypse.esp", "Light.esp", "Small.esl" },
                      { false, false, false, false, true, true });
}

// A layer put Essentials.esp before rotfern and Apocalypse (slots 02 to 03
// and 03 to 04) and swapped the two light plugins (FE000 and FE001)
EspmFileList OrderB()
{
  return EspmFileList({ "Skyrim.esm", "Update.esm", "Essentials.esp",
                        "rotfern.esp", "Apocalypse.esp", "Small.esl",
                        "Light.esp" },
                      { false, false, false, false, false, true, true });
}

// A without Apocalypse.esp
EspmFileList OrderC()
{
  return EspmFileList(
    { "Skyrim.esm", "Update.esm", "rotfern.esp", "Light.esp", "Small.esl" },
    { false, false, false, true, true });
}

// A player record with every one of the twelve raw fields set, under A
MpChangeForm Record()
{
  MpChangeForm res;
  res.recType = MpChangeForm::ACHR;
  res.formDesc = FormDesc::FromString("abc");
  res.baseDesc = FormDesc::FromString("7:Skyrim.esm");
  res.profileId = 1;

  Appearance appearance;
  appearance.isFemale = true;
  appearance.raceId = 0x02000d62; // rotfern.esp
  // rotfern.esp, Skyrim.esm, Light.esp
  appearance.headpartIds = { 0x02000a01, 0x00051a00, 0xfe000800 };
  appearance.headTextureSetId = 0x02000b00; // rotfern.esp
  appearance.weight = 0.1f;
  appearance.name = "Rotfern";
  res.appearanceDump = appearance.ToJson();

  res.inv.AddItem(0x02000c00, 2); // rotfern.esp
  Inventory::ExtraData extra;
  extra.enchantmentId = 0x0004e6e1; // Skyrim.esm
  extra.poisonId = 0xfe001800;      // Small.esl
  extra.poisonCount = 1;
  res.inv.AddItems({ Inventory::Entry(0x03000d00, 1, extra) }); // Apocalypse

  Inventory::ExtraData worn;
  worn.worn_ = true;
  res.equipment.inv.AddItems({ Inventory::Entry(0x02000c00, 1, worn) });
  res.equipment.leftSpell = 0x03001234;    // Apocalypse.esp
  res.equipment.rightSpell = 0x0001c789;   // Skyrim.esm
  res.equipment.voiceSpell = 0x00013e07;   // Skyrim.esm
  res.equipment.instantSpell = 0x03004321; // Apocalypse.esp
  res.equipment.numChanges = 3;

  res.learnedSpells.LearnSpell(0x03001234); // Apocalypse.esp
  res.learnedSpells.LearnSpell(0x0001c789); // Skyrim.esm
  res.learnedSpells.LearnSpell(0xfe000900); // Light.esp
  return res;
}

MpChangeForm Load(const nlohmann::json& j, const EspmFileList* files,
                  const EspmFileList* rawOrder = nullptr)
{
  simdjson::dom::parser parser;
  auto element = parser.parse(j.dump()).value();
  return MpChangeForm::JsonToChangeForm(element, files, rawOrder);
}

Appearance AppearanceOf(const MpChangeForm& changeForm)
{
  return Appearance::FromJson(
    nlohmann::json::parse(changeForm.appearanceDump));
}

std::vector<std::string> UnresolvedOf(const MpChangeForm& changeForm)
{
  std::vector<std::string> res;
  for (const auto& entry : changeForm.unresolved) {
    res.push_back(entry.field + "=" + entry.form.ToString());
  }
  std::sort(res.begin(), res.end());
  return res;
}

}

TEST_CASE("The twelve raw fields round-trip plugin-relative under one order",
          "[FormDescWorld]")
{
  const EspmFileList a = OrderA();
  const MpChangeForm record = Record();
  const nlohmann::json j = MpChangeForm::ToJson(record, &a);

  REQUIRE(j.at("formVersion") == MpChangeForm::kFormVersion);
  REQUIRE(j.at("appearanceForms").at("raceId") == "d62:rotfern.esp");
  REQUIRE(j.at("appearanceForms").at("headpartIds") ==
          nlohmann::json(
            { "a01:rotfern.esp", "51a00:Skyrim.esm", "800:Light.esp" }));
  REQUIRE(j.at("appearanceForms").at("headTextureSetId") == "b00:rotfern.esp");
  // the dump keeps its numbers for the legacy readers
  REQUIRE(j.at("appearanceDump").at("raceId") == 0x02000d62);
  REQUIRE(j.at("inv").at("entries").at(0).at("baseId") == "c00:rotfern.esp");
  REQUIRE(j.at("inv").at("entries").at(1).at("baseId") ==
          "d00:Apocalypse.esp");
  REQUIRE(j.at("inv").at("entries").at(1).at("enchantmentId") ==
          "4e6e1:Skyrim.esm");
  REQUIRE(j.at("inv").at("entries").at(1).at("poisonId") == "800:Small.esl");
  REQUIRE(j.at("equipmentDump").at("inv").at("entries").at(0).at("baseId") ==
          "c00:rotfern.esp");
  REQUIRE(j.at("equipmentDump").at("leftSpell") == "1234:Apocalypse.esp");
  REQUIRE(j.at("equipmentDump").at("rightSpell") == "1c789:Skyrim.esm");
  REQUIRE(j.at("equipmentDump").at("voiceSpell") == "13e07:Skyrim.esm");
  REQUIRE(j.at("equipmentDump").at("instantSpell") == "4321:Apocalypse.esp");
  REQUIRE(j.at("learnedSpells") ==
          nlohmann::json(
            { "1c789:Skyrim.esm", "1234:Apocalypse.esp", "900:Light.esp" }));

  const MpChangeForm loaded = Load(j, &a);
  REQUIRE(loaded.formVersion == MpChangeForm::kFormVersion);
  REQUIRE(loaded.unresolved.empty());
  REQUIRE(loaded.appearanceDump == record.appearanceDump);
  REQUIRE(loaded == record);
}

TEST_CASE("Without a load order the record keeps the raw shape",
          "[FormDescWorld]")
{
  const MpChangeForm record = Record();
  const nlohmann::json j = MpChangeForm::ToJson(record);
  REQUIRE(!j.contains("formVersion"));
  REQUIRE(!j.contains("appearanceForms"));
  REQUIRE(j.at("inv").at("entries").at(0).at("baseId") == 0x02000c00);
  REQUIRE(j.at("equipmentDump").at("leftSpell") == 0x03001234);
  REQUIRE(j.at("learnedSpells").at(0) == 0x0001c789);

  const MpChangeForm loaded = Load(j, nullptr);
  REQUIRE(loaded.formVersion == 1);
  REQUIRE(loaded == record);

  // a plugin-relative record needs the order to read
  const EspmFileList a = OrderA();
  REQUIRE_THROWS(Load(MpChangeForm::ToJson(record, &a), nullptr));
}

TEST_CASE("Saved under order A and loaded under order B, the runtime ids "
          "follow the slots and the forms stay",
          "[FormDescWorld]")
{
  const EspmFileList a = OrderA(), b = OrderB();
  const MpChangeForm record = Record();
  const nlohmann::json savedUnderA = MpChangeForm::ToJson(record, &a);

  const MpChangeForm loaded = Load(savedUnderA, &b);
  REQUIRE(loaded.formVersion == MpChangeForm::kFormVersion);
  REQUIRE(loaded.unresolved.empty());

  const Appearance appearance = AppearanceOf(loaded);
  REQUIRE(appearance.raceId == 0x03000d62);
  REQUIRE(appearance.headpartIds ==
          std::vector<uint32_t>{ 0x03000a01, 0x00051a00, 0xfe001800 });
  REQUIRE(appearance.headTextureSetId == 0x03000b00);
  REQUIRE(appearance.isFemale == true);
  REQUIRE(appearance.name == "Rotfern");
  REQUIRE(appearance.weight == 0.1f);

  REQUIRE(loaded.inv.entries.size() == 2);
  REQUIRE(loaded.inv.entries[0].baseId == 0x03000c00);
  REQUIRE(loaded.inv.entries[0].count == 2);
  REQUIRE(loaded.inv.entries[1].baseId == 0x04000d00);
  REQUIRE(loaded.inv.entries[1].count == 1);
  REQUIRE(loaded.inv.entries[1].enchantmentId == 0x0004e6e1);
  REQUIRE(loaded.inv.entries[1].poisonId == 0xfe000800);
  REQUIRE(loaded.inv.entries[1].poisonCount == 1);

  REQUIRE(loaded.equipment.inv.entries.size() == 1);
  REQUIRE(loaded.equipment.inv.entries[0].baseId == 0x03000c00);
  REQUIRE(loaded.equipment.inv.entries[0].GetWorn() == Inventory::Worn::Right);
  REQUIRE(loaded.equipment.leftSpell == 0x04001234);
  REQUIRE(loaded.equipment.rightSpell == 0x0001c789);
  REQUIRE(loaded.equipment.voiceSpell == 0x00013e07);
  REQUIRE(loaded.equipment.instantSpell == 0x04004321);
  REQUIRE(loaded.equipment.numChanges == 3);

  REQUIRE(loaded.learnedSpells.GetLearnedSpells() ==
          std::vector<uint32_t>{ 0x0001c789, 0x04001234, 0xfe001900 });

  // the same records: written back under B every stored form reads as it
  // did under A; only the dump's numbers are B's
  nlohmann::json savedUnderB = MpChangeForm::ToJson(loaded, &b);
  nlohmann::json expected = savedUnderA;
  REQUIRE(savedUnderB.at("appearanceDump").at("raceId") == 0x03000d62);
  savedUnderB.erase("appearanceDump");
  expected.erase("appearanceDump");
  REQUIRE(savedUnderB == expected);
}

TEST_CASE("A raw-shape record with a saved order A migrates under order B "
          "to the same forms",
          "[FormDescWorld]")
{
  const EspmFileList a = OrderA(), b = OrderB();
  const MpChangeForm record = Record();
  const nlohmann::json raw = MpChangeForm::ToJson(record); // version 1, A

  const MpChangeForm migrated = Load(raw, &b, &a);
  // read from the raw shape: the driver writes it back as version 2
  REQUIRE(migrated.formVersion == 1);
  REQUIRE(migrated.unresolved.empty());
  REQUIRE(migrated.equipment.leftSpell == 0x04001234);
  REQUIRE(AppearanceOf(migrated).raceId == 0x03000d62);
  // the same change form a version-2 record saved under A gives under B
  REQUIRE(migrated == Load(MpChangeForm::ToJson(record, &a), &b));
  REQUIRE(MpChangeForm::ToJson(migrated, &b).at("formVersion") ==
          MpChangeForm::kFormVersion);

  // no saved order: the raw ids are taken as the running order's
  const MpChangeForm asIs = Load(raw, &a);
  REQUIRE(asIs.formVersion == 1);
  REQUIRE(asIs == record);
}

TEST_CASE("The file driver reads a raw world by its saved order and writes "
          "it back migrated",
          "[FormDescWorld]")
{
  const EspmFileList a = OrderA(), b = OrderB();
  const std::filesystem::path directory = "unit/data/formdesc-world";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory / "changeForms");

  // a world saved by a server running A before the version mark: raw
  // records, the order file beside them
  const MpChangeForm record = Record();
  const auto recordPath =
    directory / "changeForms" / (record.formDesc.ToString('_') + ".json");
  {
    std::ofstream f(recordPath);
    f << MpChangeForm::ToJson(record).dump(2);
    std::ofstream order(directory / LoadOrderFile::kName);
    order << LoadOrderFile::ToJson(a.ToLoadOrder()).dump(2);
  }

  // the server now runs B
  FileDatabase db(directory.string(), spdlog::default_logger());
  REQUIRE(db.LoadLoadOrder() == a.ToLoadOrder());
  db.SetLoadOrder(std::make_shared<FileDatabase::LoadOrder>(b.ToLoadOrder()));

  std::vector<MpChangeForm> read;
  db.Iterate(
    [&](const MpChangeForm& changeForm) { read.push_back(changeForm); },
    std::nullopt);
  REQUIRE(read.size() == 1);
  REQUIRE(read[0].formVersion == 1);
  REQUIRE(read[0].equipment.leftSpell == 0x04001234);
  REQUIRE(AppearanceOf(read[0]).raceId == 0x03000d62);

  // migrated once: the record is version 2 on disk, the order file names B
  {
    std::ifstream in(recordPath);
    const nlohmann::json onDisk = nlohmann::json::parse(in);
    REQUIRE(onDisk.at("formVersion") == MpChangeForm::kFormVersion);
    REQUIRE(onDisk.at("equipmentDump").at("leftSpell") ==
            "1234:Apocalypse.esp");
    REQUIRE(onDisk.at("appearanceForms").at("raceId") == "d62:rotfern.esp");
  }
  REQUIRE(db.LoadLoadOrder() == b.ToLoadOrder());

  // the next load is plain
  read.clear();
  db.Iterate(
    [&](const MpChangeForm& changeForm) { read.push_back(changeForm); },
    std::nullopt);
  REQUIRE(read.size() == 1);
  REQUIRE(read[0].formVersion == MpChangeForm::kFormVersion);
  REQUIRE(read[0].equipment.leftSpell == 0x04001234);

  // a world without the file reads as an unknown order: the raw ids are the
  // running order's
  std::filesystem::remove(directory / LoadOrderFile::kName);
  {
    std::ofstream f(recordPath);
    f << MpChangeForm::ToJson(record).dump(2);
  }
  REQUIRE(!db.LoadLoadOrder().has_value());
  read.clear();
  db.Iterate(
    [&](const MpChangeForm& changeForm) { read.push_back(changeForm); },
    std::nullopt);
  REQUIRE(read.size() == 1);
  REQUIRE(read[0].formVersion == 1);
  REQUIRE(read[0].equipment.leftSpell == 0x03001234);
  REQUIRE(db.LoadLoadOrder() == b.ToLoadOrder());
}

TEST_CASE("A stored form naming an unloaded plugin is kept and written back",
          "[FormDescWorld]")
{
  const EspmFileList a = OrderA(), c = OrderC();
  const MpChangeForm record = Record();
  const nlohmann::json savedUnderA = MpChangeForm::ToJson(record, &a);

  // Apocalypse.esp is not loaded: its forms resolve to nothing and are kept
  const MpChangeForm loaded = Load(savedUnderA, &c);
  REQUIRE(loaded.inv.entries.size() == 1);
  REQUIRE(loaded.inv.entries[0].baseId == 0x02000c00);
  REQUIRE(!loaded.equipment.leftSpell.has_value());
  REQUIRE(loaded.equipment.rightSpell == 0x0001c789);
  REQUIRE(loaded.equipment.voiceSpell == 0x00013e07);
  REQUIRE(!loaded.equipment.instantSpell.has_value());
  REQUIRE(loaded.learnedSpells.GetLearnedSpells() ==
          std::vector<uint32_t>{ 0x0001c789, 0xfe000900 });
  REQUIRE(UnresolvedOf(loaded) ==
          std::vector<std::string>{ "instantSpell=4321:Apocalypse.esp",
                                    "inv=d00:Apocalypse.esp",
                                    "learnedSpells=1234:Apocalypse.esp",
                                    "leftSpell=1234:Apocalypse.esp" });
  REQUIRE(AppearanceOf(loaded).raceId == 0x02000d62);

  // written back under C the record is what it was under A, the parked
  // entries after the live ones
  nlohmann::json savedUnderC = MpChangeForm::ToJson(loaded, &c);
  REQUIRE(savedUnderC.at("inv").at("entries").size() == 2);
  REQUIRE(savedUnderC.at("inv").at("entries").at(1).at("baseId") ==
          "d00:Apocalypse.esp");
  REQUIRE(savedUnderC.at("inv").at("entries").at(1).at("poisonId") ==
          "800:Small.esl");
  REQUIRE(savedUnderC.at("equipmentDump").at("leftSpell") ==
          "1234:Apocalypse.esp");
  REQUIRE(savedUnderC.at("equipmentDump").at("instantSpell") ==
          "4321:Apocalypse.esp");
  REQUIRE(savedUnderC.at("learnedSpells") ==
          nlohmann::json(
            { "1c789:Skyrim.esm", "900:Light.esp", "1234:Apocalypse.esp" }));
  nlohmann::json expected = savedUnderA;
  savedUnderC.erase("learnedSpells");
  expected.erase("learnedSpells");
  REQUIRE(savedUnderC == expected);

  // and the plugin's return restores it
  const MpChangeForm restored = Load(MpChangeForm::ToJson(loaded, &c), &a);
  REQUIRE(restored.unresolved.empty());
  REQUIRE(restored == record);
}

TEST_CASE("The zip driver keeps the load order beside its records",
          "[FormDescWorld]")
{
  const EspmFileList a = OrderA();
  const char* archivePath = "unit/data/formdesc-world.zip";
  std::filesystem::create_directories("unit/data");
  std::filesystem::remove(archivePath);

  ZipDatabase db(archivePath, spdlog::default_logger());
  REQUIRE(!db.LoadLoadOrder().has_value());
  db.UpsertLoadOrder(a.ToLoadOrder());
  REQUIRE(db.LoadLoadOrder() == a.ToLoadOrder());

  // the order entry is no record
  size_t n = 0;
  db.Iterate([&](const MpChangeForm&) { ++n; }, std::nullopt);
  REQUIRE(n == 0);
}
