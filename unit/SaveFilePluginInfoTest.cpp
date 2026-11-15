#include "savefile/SFReader.h"
#include "savefile/SFStructure.h"
#include "savefile/SFWriter.h"
#include <catch2/catch_all.hpp>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

// thuum: every seat logs in through one template save, Skyrim Platform's
// assets/template.ess, whose plugin list is 00 Skyrim, 01 Update, 02
// Dragonborn, 03 Dawnguard and 04 HearthFires; the deployed load order is 00
// Skyrim, 01 Update, 02 Dawnguard, 03 HearthFires, 04 Dragonborn. Written over
// by position, the list left every form id in the template under its old
// index, so VC01's DLC1VampireChange (0x0300283B) named HearthFires.esm and
// reached the scripts as None (playtest 52). The login save keeps the
// template's entries and appends the seat's others (LoadGame's
// ModifyPluginInfo), and the game matches the list to its plugins by name
// (HYPOTHESIS: Ghidra on sky-re, 1.6.1170, 0x140606df0 and 0x1406057e0, until
// a lab run).

namespace {
std::shared_ptr<SaveFile_::SaveFile> ReadTemplate()
{
  std::ifstream in(TEMPLATE_ESS_PATH, std::ios::binary);
  REQUIRE(in.is_open());
  const std::vector<uint8_t> bytes{ std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>() };
  return SaveFile_::Reader(bytes.data(), bytes.size()).GetStructure();
}

uint32_t NamesSize(const std::vector<std::string>& names)
{
  uint32_t size = 0;
  for (auto& name : names) {
    size += static_cast<uint32_t>(2 + name.size());
  }
  return size;
}

const std::vector<std::string> kTemplatePlugins = { "Skyrim.esm", "Update.esm",
                                                    "Dragonborn.esm",
                                                    "Dawnguard.esm",
                                                    "HearthFires.esm" };

const std::vector<std::string> kAppended = {
  "unofficial skyrim special edition patch.esp", "GeneralDisplays.esm",
  "ApachiiHair.esm"
};
}

TEST_CASE("The login save keeps template.ess's plugins at their indices and "
          "appends the seat's others",
          "[SaveFile]")
{
  auto save = ReadTemplate();
  REQUIRE(save->pluginInfo.pluginsName == kTemplatePlugins);

  // VC01's DLC1VampireChange and PlayerVampireQuestScript's
  // TrapDiseasePorphyricHemophelia as the template holds them: Papyrus's
  // handles are RefIDs 0x001C8F and 0x0025F6, formIDArray entries 7311 and
  // 9718 counted from 1
  REQUIRE(save->formIDArray.at(7310) == 0x0300283B);
  REQUIRE(save->formIDArray.at(9717) == 0x030037E9);

  const auto before = *save;
  std::vector<std::string> deployed = { "Skyrim.esm", "Update.esm",
                                        "Dawnguard.esm", "HearthFires.esm",
                                        "Dragonborn.esm" };
  deployed.insert(deployed.end(), kAppended.begin(), kAppended.end());

  const auto missing = save->MergePluginInfo(deployed);
  REQUIRE(missing.empty());

  std::vector<std::string> expected = kTemplatePlugins;
  expected.insert(expected.end(), kAppended.begin(), kAppended.end());
  REQUIRE(save->pluginInfo.pluginsName == expected);
  REQUIRE(save->pluginInfo.numPlugins == expected.size());

  // both handles still name Dawnguard, and nothing in the formIDArray moved
  REQUIRE(save->pluginInfo.pluginsName[save->formIDArray[7310] >> 24] ==
          "Dawnguard.esm");
  REQUIRE(save->pluginInfo.pluginsName[save->formIDArray[9717] >> 24] ==
          "Dawnguard.esm");
  REQUIRE(save->formIDArray == before.formIDArray);
  REQUIRE(save->visitedWorldspaceArray == before.visitedWorldspaceArray);

  // the tables after the list move by the names added
  const uint32_t added = NamesSize(kAppended);
  REQUIRE(save->pluginInfoSize == before.pluginInfoSize + added);
  const auto& t = save->fileLocationTable;
  const auto& t0 = before.fileLocationTable;
  REQUIRE(t.formIDArrayCountOffset == t0.formIDArrayCountOffset + added);
  REQUIRE(t.unknownTable3Offset == t0.unknownTable3Offset + added);
  REQUIRE(t.globalDataTable1Offset == t0.globalDataTable1Offset + added);
  REQUIRE(t.globalDataTable2Offset == t0.globalDataTable2Offset + added);
  REQUIRE(t.changeFormsOffset == t0.changeFormsOffset + added);
  REQUIRE(t.globalDataTable3Offset == t0.globalDataTable3Offset + added);

  // a form id of the deployed order goes in under its plugin's index in the
  // save, found by name: Dawnguard's 0x020037E9 (PlayerVampireQuestScript's
  // TrapDiseasePorphyricHemophelia) as the template's 0x030037E9, and
  // Dragonborn's 04 as 02; the appended plugins keep their indices
  REQUIRE(save->FormIdUnderPlugin(deployed[0x02], 0x020037E9) == 0x030037E9);
  REQUIRE(save->FormIdUnderPlugin(deployed[0x04], 0x04017001) == 0x02017001);
  REQUIRE(save->FormIdUnderPlugin(deployed[0x03], 0x03000800) == 0x04000800);
  REQUIRE(save->FormIdUnderPlugin(deployed[0x06], 0x06000800) == 0x06000800);
  REQUIRE(save->FormIdUnderPlugin("NotLoaded.esp", 0x08000800) ==
          std::nullopt);
  REQUIRE(save->FindPluginIndex("DAWNGUARD.ESM") == 3);

  // and through CreateRefId it reuses the template's own entry: RefID 00 25
  // F6, the Papyrus handle's, and the formIDArray keeps its size
  const auto arraySize = save->formIDArray.size();
  const auto ref = SaveFile_::RefID::CreateRefId(*save, 0x030037E9);
  REQUIRE(ref.byte0 == 0x00);
  REQUIRE(ref.byte1 == 0x25);
  REQUIRE(ref.byte2 == 0xF6);
  REQUIRE(save->formIDArray.size() == arraySize);

  // the save written and read again holds the same list and ids
  const auto path =
    std::filesystem::temp_directory_path() / "thuum-plugin-merge-test.ess";
  REQUIRE(SaveFile_::Writer(save).CreateSaveFile(path));
  std::ifstream in(path, std::ios::binary);
  const std::vector<uint8_t> bytes{ std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>() };
  in.close();
  std::filesystem::remove(path);
  auto reread = SaveFile_::Reader(bytes.data(), bytes.size()).GetStructure();
  REQUIRE(reread->pluginInfo.pluginsName == expected);
  REQUIRE(reread->formIDArray == before.formIDArray);
  REQUIRE(reread->changeForms.size() == before.changeForms.size());
}

TEST_CASE("A template plugin the seat lacks keeps its index and is returned",
          "[SaveFile]")
{
  auto save = ReadTemplate();
  const auto before = save->pluginInfoSize;

  const auto missing = save->MergePluginInfo(
    { "skyrim.esm", "Update.esm", "Dawnguard.esm", "HearthFires.esm",
      "Dawnguard.esm", "", "Mod.esp" });
  REQUIRE(missing == std::vector<std::string>{ "Dragonborn.esm" });
  // matched in any case, each name once, an empty one skipped
  REQUIRE(save->pluginInfo.pluginsName ==
          std::vector<std::string>{ "Skyrim.esm", "Update.esm",
                                    "Dragonborn.esm", "Dawnguard.esm",
                                    "HearthFires.esm", "Mod.esp" });
  REQUIRE(save->pluginInfoSize == before + NamesSize({ "Mod.esp" }));
}

TEST_CASE("A plugin list past 254 entries throws and leaves the save's list "
          "as it was",
          "[SaveFile]")
{
  auto save = ReadTemplate();
  const auto before = save->pluginInfoSize;

  // the template's five, then 249 more: 254 entries, 0x00 to 0xFD
  std::vector<std::string> loaded = kTemplatePlugins;
  for (int i = 0; loaded.size() < 254; ++i) {
    loaded.push_back("Mod" + std::to_string(i) + ".esp");
  }
  auto atCap = ReadTemplate();
  REQUIRE_NOTHROW(atCap->MergePluginInfo(loaded));
  REQUIRE(atCap->pluginInfo.pluginsName.size() == 254);

  // one more would put a plugin at 0xFE, a light plugin's index
  loaded.push_back("OneTooMany.esp");
  REQUIRE_THROWS_AS(save->MergePluginInfo(loaded), std::runtime_error);
  REQUIRE(save->pluginInfo.pluginsName == kTemplatePlugins);
  REQUIRE(save->pluginInfoSize == before);
}
