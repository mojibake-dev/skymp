#include "TestUtils.hpp"
#include "libespm/GroupUtils.h"
#include "libespm/REFR.h"
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

// Riverwood's Sigrid 0x13483 (thuum docs/verbs/npc-puppet.md): placed in
// RiverwoodAlvorsHouse (CELL 0x133c8, interior coordinates) and walked
// outside by her schedule, like every named resident. The server loads a
// game-file actor by its placement square, so a player landing at Riverwood
// (b-npc-puppet's c1, (17225, -47204)) loads no resident, and no word for
// one can reach a host (b-npc-puppet 20261022-081631 and 090227; the probe
// x-npc-sigrid-ref 091354 read her record in 133c8). The host's roster
// (ActorSightings, CellHostTest) is the fix this case is the reason for.
TEST_CASE("Riverwood's Sigrid 0x13483 is placed in her house, not at "
          "Riverwood's squares, so a landing there loads no resident",
          "[NpcExists][espm]")
{
  constexpr uint32_t kSigrid = 0x13483;
  constexpr uint32_t kAlvorsHouse = 0x133c8;
  constexpr uint32_t kTamriel = 0x3c;
  auto& partOne = GetPartOne();
  partOne.worldState.npcEnabled = true;
  auto& br = partOne.worldState.GetEspm().GetBrowser();

  // her record's cell is the house, and the index files her nowhere around
  // Riverwood under Tamriel
  const auto rec = br.LookupById(kSigrid);
  REQUIRE(rec.rec);
  REQUIRE(rec.rec->GetType() == "ACHR");
  REQUIRE(espm::GetWorldOrCell(br, rec.rec) == kAlvorsHouse);
  int filedAt = 0;
  for (int16_t x = 3; x <= 6; ++x) {
    for (int16_t y = -13; y <= -10; ++y) {
      for (auto vec : br.GetRecordsAtPos(kTamriel, x, y)) {
        for (auto r : *vec) {
          if (r->GetId() == kSigrid) {
            ++filedAt;
          }
        }
      }
    }
  }
  REQUIRE(filedAt == 0);

  // the landing's chunk pass loads nothing of her
  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 17225.f, -47204.f, -128.f }, 0, kTamriel);
  partOne.SetUserActor(0, 0xff000000);
  auto& player = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  player.SetPos(player.GetPos());
  REQUIRE(partOne.worldState.LookupFormByIdNoLoad(kSigrid) == nullptr);

  partOne.DestroyActor(0xff000000);
  DoDisconnect(partOne, 0);
}

// Riverwood's E3 demo thief 0xe93d2 (base NPC_ 0xe93d3 E3demoThief) is placed
// at (14184.8, -51974.4, 631.1) under the enable parent 0x93638, a marker the
// files leave initially disabled (XESP flags 0: the child follows it), so no
// game started from the files ever shows him. Playtest 31 (2026-10-22): the
// server loaded him at the landing's chunk pass, both seats held a puppet
// with no 3D, and his hosted word flapped with the cell. A host's sighting,
// its engine having him attached and enabled, loads him regardless.
TEST_CASE("Riverwood's E3 demo thief 0xe93d2, enabled by a disabled marker, "
          "stays out of the landing's chunk pass; a host's sighting loads him",
          "[NpcExists][espm]")
{
  constexpr uint32_t kThief = 0xe93d2;
  constexpr uint32_t kMarker = 0x93638;
  constexpr uint32_t kTamriel = 0x3c;
  const NiPoint3 kThiefPos{ 14184.8f, -51974.4f, 631.1f };
  auto& partOne = GetPartOne();
  partOne.worldState.npcEnabled = true;
  auto& br = partOne.worldState.GetEspm().GetBrowser();
  auto& cache = partOne.worldState.GetEspmCache();

  // the record's enable parent and the marker's flag, as the files say
  const auto rec = br.LookupById(kThief);
  REQUIRE(rec.rec);
  REQUIRE(rec.rec->GetType() == "ACHR");
  const auto data =
    reinterpret_cast<const espm::REFR*>(rec.rec)->GetData(cache);
  REQUIRE(data.enableParentId == kMarker);
  REQUIRE((data.enableParentFlags & 1) == 0);
  const auto marker = br.LookupById(kMarker);
  REQUIRE(marker.rec);
  REQUIRE((marker.rec->GetFlags() & 0x800) != 0);

  // the landing's chunk pass holds his square and loads him no more
  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 17225.f, -47204.f, -128.f }, 0, kTamriel);
  partOne.SetUserActor(0, 0xff000000);
  auto& player = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  player.SetPos(player.GetPos());
  REQUIRE(partOne.worldState.LookupFormByIdNoLoad(kThief) == nullptr);

  // the host's sighting (its engine shows him, a quest's doing) loads him
  // where the engine has him
  const auto cell = partOne.worldState.CellOf(kTamriel, kThiefPos);
  REQUIRE(cell.has_value());
  partOne.OnCellLoaded(player, *cell, true);
  REQUIRE(partOne.GetCellHost(*cell).player == 0xff000000);
  partOne.OnActorSighting(player, kThief, kTamriel, kThiefPos,
                          NiPoint3{ 0.f, 0.f, 90.f });
  const auto& form = partOne.worldState.LookupFormByIdNoLoad(kThief);
  REQUIRE(form != nullptr);
  REQUIRE(form->AsActor() != nullptr);
  REQUIRE(partOne.worldState.CellOf(*form->AsActor()) == cell);

  partOne.DestroyActor(0xff000000);
}
