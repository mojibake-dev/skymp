#include "FormDesc.h"
#include "libespm/Convert.h"
#include "libespm/FLST.h"
#include "libespm/Loader.h"
#include "libespm/Utils.h"
#include "libespm/espm.h"
#include <catch2/catch_all.hpp>
#include <filesystem>
#include <string>
#include <vector>

// thuum docs/verbs/light-plugins.md: libespm numbers full and light plugins
// apart, each in load order, as the engine does (CommonLibSSE-NG
// src/RE/T/TESFile.cpp:33-42, src/RE/T/TESDataHandler.cpp:40-87): a full
// plugin's forms at its index among full plugins << 24, a light plugin's at
// 0xFE000000 | its index among light plugins << 12. The fixtures are four
// tiny plugins this repo owns (fixtures/light_plugins/make_fixtures.py):
// Base.esm (full), Light.esp (light by its TES4 flag), After.esp (full,
// masters Base.esm and Light.esp), Small.esl (light by its extension).

namespace {
std::filesystem::path FixtureDir()
{
  return std::filesystem::path(__FILE__).parent_path() / "fixtures" /
    "light_plugins";
}

espm::Loader& Fixtures()
{
  static espm::Loader loader(
    FixtureDir(), { "Base.esm", "Light.esp", "After.esp", "Small.esl" });
  return loader;
}

std::string EditorIdAt(uint32_t formId)
{
  auto& loader = Fixtures();
  const auto res = loader.GetBrowser().LookupById(formId);
  if (!res.rec) {
    return "";
  }
  const char* editorId = res.rec->GetEditorId(loader.GetBrowser().GetCache());
  return editorId ? editorId : "";
}

std::vector<uint32_t> ListAt(uint32_t formId)
{
  auto& loader = Fixtures();
  const auto res = loader.GetBrowser().LookupById(formId);
  std::vector<uint32_t> out;
  if (const auto flst = espm::Convert<espm::FLST>(res.rec)) {
    for (uint32_t raw :
         flst->GetData(loader.GetBrowser().GetCache()).formIds) {
      out.push_back(res.ToGlobalId(raw));
    }
  }
  return out;
}
}

TEST_CASE("libespm numbers light plugins as the engine does", "[LightPlugins]")
{
  auto& loader = Fixtures();
  REQUIRE(loader.GetLightFlags() ==
          std::vector<bool>{ false, true, false, true });

  REQUIRE(EditorIdAt(0x00000800) == "BaseItem");
  REQUIRE(EditorIdAt(0xfe000800) == "LightItem");
  // the second full plugin, though third in the load order
  REQUIRE(EditorIdAt(0x01000800) == "AfterItem");
  // the second light plugin, light by its extension alone
  REQUIRE(EditorIdAt(0xfe001800) == "SmallItem");
  REQUIRE(EditorIdAt(0x02000800).empty());
  REQUIRE(EditorIdAt(0xfe002800).empty());
}

TEST_CASE("References into and out of a light plugin resolve both ways",
          "[LightPlugins]")
{
  // a light plugin naming its full master and itself
  REQUIRE(ListAt(0xfe000801) ==
          std::vector<uint32_t>{ 0x00000800, 0xfe000800 });
  // a full plugin naming its light master, its full master and itself
  REQUIRE(ListAt(0x01000801) ==
          std::vector<uint32_t>{ 0xfe000800, 0x00000800, 0x01000800 });

  // the combined numbering back to After.esp's own (load order position 2)
  const auto toRaw = Fixtures().GetBrowser().GetRawMapping(2);
  REQUIRE(toRaw);
  REQUIRE(espm::utils::GetMappedId(0xfe000800, *toRaw) == 0x01000800);
  REQUIRE(espm::utils::GetMappedId(0x01000800, *toRaw) == 0x02000800);
  REQUIRE(espm::utils::GetMappedId(0x00000800, *toRaw) == 0x00000800);
  // Small.esl is no master of After.esp
  REQUIRE(espm::utils::GetMappedId(0xfe001800, *toRaw) >= 0xff000000);
}

TEST_CASE("Form descriptors name light plugins' forms by file",
          "[LightPlugins][FormDesc]")
{
  auto& loader = Fixtures();
  const EspmFileList files(loader.GetFileNames(), loader.GetLightFlags());

  REQUIRE(FormDesc::FromFormId(0xfe000800, files) ==
          FormDesc(0x800, "Light.esp"));
  REQUIRE(FormDesc::FromFormId(0xfe001800, files) ==
          FormDesc(0x800, "Small.esl"));
  REQUIRE(FormDesc::FromFormId(0x01000800, files) ==
          FormDesc(0x800, "After.esp"));
  REQUIRE(FormDesc(0x800, "Light.esp").ToFormId(files) == 0xfe000800);
  REQUIRE(FormDesc(0x800, "Small.esl").ToFormId(files) == 0xfe001800);
  REQUIRE(FormDesc(0x800, "After.esp").ToFormId(files) == 0x01000800);
  REQUIRE(FormDesc::FilePosition(0xfe001800, files) == 3);
  REQUIRE(FormDesc::FilePosition(0xfe002800, files) == -1);
  REQUIRE_THROWS(FormDesc::FromFormId(0xfe002800, files));

  // without light flags every file is full, as before the verb
  const EspmFileList unflagged(loader.GetFileNames(), std::vector<bool>());
  REQUIRE(FormDesc::FromFormId(0x02000800, unflagged) ==
          FormDesc(0x800, "After.esp"));
  REQUIRE(FormDesc(0x800, "Small.esl").ToFormId(unflagged) == 0x03000800);
}
