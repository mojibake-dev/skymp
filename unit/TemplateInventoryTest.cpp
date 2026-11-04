#include "MsgType.h"
#include "ServerState.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <nlohmann/json.hpp>

PartOne& GetPartOne();

TEST_CASE("MS13BanditCampfire01 in BleakFalls should have inventory/equipment "
          "derived from NPC template",
          "[TemplateInventory]")
{
  PartOne& p = GetPartOne();
  p.worldState.npcSettings["Skyrim.esm"].spawnInInterior = true;
  p.worldState.npcEnabled = true;
  uint32_t actorId = 0x39fe4;
  auto& bandit = p.worldState.GetFormAt<MpActor>(actorId);

  // thuum docs/verbs/npc-fate.md (playtest 44's 1c): a fate-able actor's base
  // rolls once, at its fate, so the template chain's inventory stands after
  // the roll, not at Init (the fate pass calls this at the first loader's
  // level; NpcFateTest reads the pass itself)
  REQUIRE(bandit.GetEquipment().inv.entries.empty());
  REQUIRE(bandit.RollFateIfUnrolled(1));

  REQUIRE(bandit.GetEquipment().inv.ToJson().size() > 0);

  int numWeaps = 0;
  for (auto entry : bandit.GetEquipment().inv.entries) {
    auto lookupRes =
      p.worldState.GetEspm().GetBrowser().LookupById(entry.baseId);
    if (lookupRes.rec && lookupRes.rec->GetType() == "WEAP") {
      numWeaps++;
    }
  }

  REQUIRE(numWeaps == 1);
}
