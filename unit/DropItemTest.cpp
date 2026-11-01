#include "MpChangeForms.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "MpObjectReference.h"
#include "PartOne.h"
#include "save_storages/AsyncSaveStorage.h"

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

// SaveStorageTest.cpp's helpers: a file-backed save storage under unit/data
// and a synchronous upsert into it
std::shared_ptr<
  Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>
MakeSaveStorageFile();
void UpsertSync(
  Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>& st,
  std::vector<std::optional<MpChangeForm>> changeForms);

TEST_CASE("A dropped copy survives a restart: the save's loader keeps it and "
          "a user whose actor is set after the load hears its create word "
          "(x-prop-push v15 220439: upstream's loader skipped the ingot)",
          "[DropItemTest]")
{
  constexpr uint32_t ironDagger = 0x0001397E;
  PartOne& running = GetPartOne();
  DoConnect(running, 0);
  running.CreateActor(0xff000000, { 1, 2, 3 }, 0, 0x3c);
  running.SetUserActor(0, 0xff000000);
  MpActor& ac = running.worldState.GetFormAt<MpActor>(0xff000000);
  ac.RemoveAllItems();
  ac.AddItem(ironDagger, 1);
  DoMessage(running, 0,
            nlohmann::json{ { "t", MsgType::DropItem },
                            { "baseId", ironDagger },
                            { "count", 1 } });
  running.Tick();
  MpObjectReference& copy =
    running.worldState.GetFormAt<MpObjectReference>(0xff000001);
  REQUIRE(copy.GetBaseId() == ironDagger);
  // the record as the save writes it: the actor's and the copy's
  MpChangeForm savedActor = ac.GetChangeForm();
  savedActor.profileId = 7; // a player character, as the save marks one
  const MpChangeForm savedCopy = copy.GetChangeForm();
  REQUIRE(savedCopy.isDisabled == false);
  REQUIRE(savedCopy.position.y == 66.f);

  // the restarted server reads the save as AttachSaveStorage does
  const bool persist = GENERATE(true, false);
  auto storage = MakeSaveStorageFile();
  UpsertSync(*storage, { savedActor, savedCopy });
  PartOne& restarted = GetPartOne();
  restarted.SetPropsPersist(persist);
  restarted.AttachSaveStorage(storage);
  auto loadedForm = restarted.worldState.LookupFormById(0xff000001);
  if (!persist) {
    // upstream's clutter policy, kept behind the setting: the item is skipped
    REQUIRE(loadedForm == nullptr);
    return;
  }
  REQUIRE(loadedForm != nullptr);
  MpObjectReference& loaded =
    restarted.worldState.GetFormAt<MpObjectReference>(0xff000001);
  REQUIRE(loaded.GetPos().y == 66.f);
  REQUIRE(loaded.IsDisabled() == false);

  // the user logs in to its restored actor (the loader disables a player
  // character, the login enables it): the login's subscription pass must
  // send the copy's create word, as it does for a copy created while the
  // user is online
  MpActor& loadedActor = restarted.worldState.GetFormAt<MpActor>(0xff000000);
  loadedActor.Enable();
  DoConnect(restarted, 0);
  restarted.Messages().clear();
  restarted.SetUserActor(0, 0xff000000);
  restarted.Tick();
  int creates = 0;
  for (auto& m : restarted.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::CreateActor &&
        m.j["refrId"] == 0xff000001) {
      ++creates;
      REQUIRE(m.j["transform"]["pos"] ==
              nlohmann::json::array({ 1.f, 66.f, 51.f }));
    }
  }
  REQUIRE(creates == 1);
}
