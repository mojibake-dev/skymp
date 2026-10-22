#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

PartOne& GetPartOne();

static constexpr uint32_t cowId = 0x10ebaf;
// thuum docs/verbs/npc-puppet.md: an actor from the game files travels under
// its own form id (upstream sent id + 0x100000000 for a client-side copy)
static constexpr uint64_t cowRefrId = 0x10ebaf;

TEST_CASE("Whiterun cow 0x10ebaf exists", "[NpcExists][espm]")
{
  auto& partOne = GetPartOne();
  partOne.worldState.npcEnabled = true;
  auto& cow = partOne.worldState.GetFormAt<MpActor>(cowId);
}

TEST_CASE("Whiterun cow 0x10ebaf is visible to players", "[NpcExists][espm]")
{
  uint32_t cell = ToUnderlying(Constants::kWhiterunCell);
  auto& partOne = GetPartOne();
  partOne.worldState.npcEnabled = true;
  auto& messages = partOne.Messages();

  auto& cow = partOne.worldState.GetFormAt<MpActor>(cowId);

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, cow.GetPos(), 0, cell);
  partOne.SetUserActor(0, 0xff000000);

  {
    auto it = std::find_if(
      messages.begin(), messages.end(),
      [&](PartOne::Message& msg) { return msg.j["refrId"] == cowRefrId; });

    REQUIRE(it != messages.end());
  }

  {
    auto it = std::find_if(messages.begin(), messages.end(),
                           [&](PartOne::Message& msg) {
                             // the flag sits under props, explicit for a
                             // game-file actor too: nobody hosts its cell
                             // here, so hosted by other (npc-puppet.md,
                             // ADR-045 point 2 as revised 2026-10-22)
                             return msg.j["refrId"] == cowRefrId &&
                               msg.j["props"]["isHostedByOther"] == true;
                           });
    REQUIRE(it != messages.end());
  }

  partOne.DestroyActor(0xff000000);
  partOne.DestroyActor(cowId);
  DoDisconnect(partOne, 0);
}

TEST_CASE("Whiterun cow is disallowed to be loaded because of overriden npc "
          "settings, provided enabled npcs",
          "[NpcExists][espm]")
{
  PartOne& partOne = GetPartOne();
  partOne.worldState.npcEnabled = true;
  std::unordered_map<std::string, WorldState::NpcSettingsEntry> settings = {
    { "DragonBorn.esm", { true, true } }
  };
  partOne.worldState.SetNpcSettings(std::move(settings));

  REQUIRE_THROWS_WITH(partOne.worldState.GetFormAt<MpActor>(cowId),
                      Catch::Matchers::ContainsSubstring("Form with id"));
}

TEST_CASE(
  "Whiterun cow is not loaded because only npcs within interior are allowed",
  "[NpcExists][espm]")
{
  PartOne& partOne = GetPartOne();
  partOne.worldState.npcEnabled = true;
  bool spawnInInterior = true, spawnInExterior = false;
  std::unordered_map<std::string, WorldState::NpcSettingsEntry> settings = {
    { "Skyrim.esm", { spawnInInterior, spawnInExterior } }
  };
  partOne.worldState.SetNpcSettings(std::move(settings));

  REQUIRE_THROWS_WITH(partOne.worldState.GetFormAt<MpActor>(cowId),
                      Catch::Matchers::ContainsSubstring("Form with id"));
}

TEST_CASE("Whiterun cow is successfully loaded, because only exterior npc "
          "from Skyrim.esm are allowed",
          "[NpcExists][espm]")
{
  PartOne& partOne = GetPartOne();
  partOne.worldState.npcEnabled = true;
  bool spawnInInterior = false, spawnInExterior = true;
  std::unordered_map<std::string, WorldState::NpcSettingsEntry> settings = {
    { "Skyrim.esm", { spawnInInterior, spawnInExterior } }
  };
  partOne.worldState.SetNpcSettings(std::move(settings));

  REQUIRE_NOTHROW(partOne.worldState.GetFormAt<MpActor>(cowId));
}

TEST_CASE(
  "Whiterun cow is loaded because all npc are allowed by default setting",
  "[NpcExists][espm]")
{
  PartOne& partOne = GetPartOne();
  partOne.worldState.npcEnabled = true;
  partOne.worldState.defaultSetting.spawnInInterior = true;
  partOne.worldState.defaultSetting.spawnInExterior = true;
  partOne.worldState.defaultSetting.overriden = true;
  REQUIRE_NOTHROW(partOne.worldState.GetFormAt<MpActor>(cowId));
}

TEST_CASE("Riverwood's Alvor 0x13482, a unique Nord in an interior, loads "
          "under npcEnabled: the essential, unique, crime-faction and race "
          "bans are lifted (thuum docs/verbs/npc-puppet.md)",
          "[NpcExists][espm]")
{
  PartOne& partOne = GetPartOne();
  partOne.worldState.npcEnabled = true;
  REQUIRE_NOTHROW(partOne.worldState.GetFormAt<MpActor>(0x13482));
}

// Riverwood's Sigrid 0x13483 (thuum docs/verbs/npc-puppet.md): a persistent
// reference in Tamriel's persistent cell (Skyrim.esm 0xd74), placed at
// (24063, -45204). b-npc-puppet 20261022-081631 and 090227 saw her host demote
// her at the attach and never promote her: no word for her ever reached the
// host. The two questions this case asks the server alone: where libespm
// files her (the chunk loader asks GetRecordsAtPos at the truncated square of
// a player's position, GetGridPos's rounding), and whether a player landing
// at Riverwood (b-npc-puppet's c1, (17225, -47204)) loads her and is sent her
// CreateActor.
TEST_CASE("Riverwood's Sigrid 0x13483 is filed at her own square and loads "
          "for a player landing at Riverwood",
          "[NpcExists][espm]")
{
  constexpr uint32_t kSigrid = 0x13483;
  constexpr uint32_t kTamriel = 0x3c;
  auto& partOne = GetPartOne();
  partOne.worldState.npcEnabled = true;
  auto& br = partOne.worldState.GetEspm().GetBrowser();

  // the index: under Tamriel at the truncated square of her own position
  {
    int filedAt = 0;
    int16_t atX = 0, atY = 0;
    for (int16_t x = 3; x <= 6; ++x) {
      for (int16_t y = -13; y <= -10; ++y) {
        for (auto vec : br.GetRecordsAtPos(kTamriel, x, y)) {
          for (auto rec : *vec) {
            if (rec->GetId() == kSigrid) {
              ++filedAt;
              atX = x;
              atY = y;
            }
          }
        }
      }
    }
    INFO("Sigrid filed " << filedAt << " time(s) around Riverwood, last at ("
                         << atX << ", " << atY << ")");
    REQUIRE(filedAt == 1);
    REQUIRE(atX == 5);
    REQUIRE(atY == -11);
  }

  // the server: the player's landing loads the chunk and subscribes it
  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 17225.f, -47204.f, -128.f }, 0, kTamriel);
  partOne.SetUserActor(0, 0xff000000);
  auto& player = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  player.SetPos(player.GetPos());

  {
    std::stringstream trace;
    const bool loadedByTheLanding =
      partOne.worldState.LookupFormByIdNoLoad(kSigrid) != nullptr;
    if (!loadedByTheLanding) {
      // the loader's own reasons, for the failing case's report
      partOne.worldState.LookupFormById(kSigrid, &trace);
    }
    INFO("LookupFormById trace: " << trace.str());
    REQUIRE(loadedByTheLanding);
    auto& sigrid = partOne.worldState.GetFormAt<MpActor>(kSigrid);
    REQUIRE(sigrid.GetCellOrWorld().ToFormId(partOne.worldState.espmFiles) ==
            kTamriel);
    REQUIRE(sigrid.GetPos().x == Catch::Approx(24063.7f).margin(2.f));
  }

  {
    auto& messages = partOne.Messages();
    auto it = std::find_if(
      messages.begin(), messages.end(), [&](PartOne::Message& msg) {
        return msg.userId == 0 && msg.j["refrId"] == kSigrid &&
          msg.j["props"]["isHostedByOther"] == true;
      });
    REQUIRE(it != messages.end());
  }

  partOne.DestroyActor(0xff000000);
  partOne.DestroyActor(kSigrid);
  DoDisconnect(partOne, 0);
}
