#include "MpChangeForms.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "MpObjectReference.h"
#include "PartOne.h"

PartOne& GetPartOne();

TEST_CASE("Dropping an item", "[DropItemTest]")
{
  auto& partOne = GetPartOne();
  // an iron dagger
  constexpr uint32_t ironDagger = 0x0001397E;
  constexpr uint32_t ironSword = 0x00012EB7;
  DoConnect(partOne, 0);

  partOne.CreateActor(0xff000000, { 1, 2, 3 }, 0, 0x3c);
  partOne.SetUserActor(0, 0xff000000);

  MpActor& ac = partOne.worldState.GetFormAt<MpActor>(0xff000000);

  ac.RemoveAllItems();
  REQUIRE(ac.GetInventory().GetTotalItemCount() == 0);
  ac.AddItem(ironDagger, 1);
  REQUIRE(ac.GetInventory().GetTotalItemCount() == 1);
  partOne.Messages().clear();
  REQUIRE(partOne.Messages().size() == 0);
  DoMessage(partOne, 0,
            nlohmann::json{ { "t", MsgType::DropItem },
                            { "baseId", ironDagger },
                            { "count", 1 } });
  // 1 message from here and another 1 is comming from actionListener
  partOne.Tick();
  REQUIRE(partOne.Messages().size() == 2);
  REQUIRE(ac.GetInventory().GetItemCount(ironDagger) == 0);
  MpObjectReference& refr =
    partOne.worldState.GetFormAt<MpObjectReference>(0xff000001);
  REQUIRE(refr.GetBaseId() == ironDagger);
  // a step ahead of the actor along its heading (angle z 0: north), at its
  // feet, never inside its capsule (thuum prop-physics, the drop's stream)
  REQUIRE(refr.GetPos().x == 1.f);
  REQUIRE(refr.GetPos().y == 66.f);
  REQUIRE(refr.GetPos().z ==
          51.f); // the feet's 3 plus the 48 lift (prop-physics.md)
  // and the create word carries that position: the copy is created at the
  // drop point, never moved there after (x-prop-launch v3 190643: a copy
  // created at the dropper spawned inside its capsule on its own seat)
  {
    int creates = 0;
    for (auto& m : partOne.Messages()) {
      if (m.j["t"] == MsgType::CreateActor &&
          m.j["refrId"] == refr.GetFormId()) {
        ++creates;
        REQUIRE(m.j["transform"]["pos"] ==
                nlohmann::json::array({ 1.f, 66.f, 51.f }));
      }
    }
    REQUIRE(creates == 1);
  }
  ac.AddItem(ironSword, 5);
  partOne.Messages().clear();
  REQUIRE(partOne.Messages().size() == 0);
  DoMessage(partOne, 0,
            nlohmann::json{ { "t", MsgType::DropItem },
                            { "baseId", ironSword },
                            { "count", 5 } });
  partOne.Tick();
  REQUIRE(partOne.Messages().size() == 2);
  REQUIRE(ac.GetInventory().GetItemCount(ironSword) == 0);
}
