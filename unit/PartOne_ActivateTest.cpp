#include "TestUtils.hpp"
#include "script_storages/DirectoryScriptStorage.h"

#include "Messages.h"

using Catch::Matchers::ContainsSubstring;

extern espm::Loader& GetEspmLoader();

// The reloot tests below harvest or empty a reference, check it stays so,
// then wait for the reloot. The reloot runs on a wall-clock timer, so the
// window must outlast everything the test does between the harvest and its
// check: 25 to 30 ms did not under the Windows coverage run (every line
// instrumented, the suite 14 times slower), where the timer fired inside the
// test's own Tick and "Activate torch" failed at its first IsHarvested check
// (thuum, fork Windows runs 37728917244 and 37743653008, 2026-10-08).
constexpr auto kRelootWindow = std::chrono::milliseconds(1000);
constexpr auto kRelootWait = kRelootWindow + std::chrono::milliseconds(250);

class FakeDamageFormula : public IDamageFormula
{
public:
  [[nodiscard]] float CalculateDamage(const MpActor&, const MpActor&,
                                      const HitData&) const override
  {
    return 25.f;
  }

  [[nodiscard]] float CalculateDamage(const MpActor&, const MpActor&,
                                      const SpellCastData&) const override
  {
    return 25.f;
  }
};

// Actually, there are a few utils
espm::CompressedFieldsCache g_dummyCache;
PartOne& GetPartOne()
{
  auto instance = std::make_shared<PartOne>();
  instance->SetDamageFormula(std::make_unique<FakeDamageFormula>());

  instance->worldState.AttachScriptStorage(
    std::make_shared<DirectoryScriptStorage>(TEST_PEX_DIR));
  instance->AttachEspm(&GetEspmLoader());

  static std::vector<std::shared_ptr<PartOne>> g_partOneInstances;
  g_partOneInstances.push_back(instance);
  return *g_partOneInstances.back();
}

constexpr auto barrelInWhiterun = 0x4cc2d;

TEST_CASE("Activate without espm attached", "[PartOne][espm]")
{
  PartOne partOne;
  DoConnect(partOne, 0);

  REQUIRE_THROWS_WITH(
    DoMessage(partOne, 0,
              nlohmann::json{ { "t", MsgType::Activate },
                              { "data",
                                { { "caster", 0x15 },
                                  { "target", 0 },
                                  { "isSecondActivation", false } } } }),
    ContainsSubstring("No loaded esm or esp files are found"));
}

TEST_CASE("Activate without Actor attached", "[PartOne][espm]")
{
  auto& partOne = GetPartOne();

  DoConnect(partOne, 0);

  REQUIRE_THROWS_WITH(
    DoMessage(partOne, 0,
              nlohmann::json{ { "t", MsgType::Activate },
                              { "data",
                                { { "caster", 0x15 },
                                  { "target", 0 },
                                  { "isSecondActivation", false } } } }),
    ContainsSubstring("Can't do this without Actor attached"));

  DoDisconnect(partOne, 0);
}

TEST_CASE("Activate with bad hoster", "[PartOne][espm]")
{
  auto& partOne = GetPartOne();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  partOne.SetUserActor(0, 0xff000000);

  REQUIRE_THROWS_WITH(
    DoMessage(partOne, 0,
              nlohmann::json{ { "t", MsgType::Activate },
                              { "data",
                                { { "caster", 0x15 },
                                  { "target", 0 },
                                  { "isSecondActivation", false } } } }),
    ContainsSubstring(
      "Bad hoster is attached to caster 0x15, expected 0xff000000, but "
      "found 0x0"));

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
}

TEST_CASE("Activate with incorrect WorldSpace", "[PartOne][espm]")
{
  auto& partOne = GetPartOne();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  partOne.SetUserActor(0, 0xff000000);

  REQUIRE_THROWS_WITH(
    DoMessage(partOne, 0,
              nlohmann::json{ { "t", MsgType::Activate },
                              { "data",
                                { { "caster", 0x14 },
                                  { "target", barrelInWhiterun },
                                  { "isSecondActivation", false } } } }),
    ContainsSubstring(
      "WorldSpace doesn't match: caster is in Tamriel (0x3c), target "
      "is in WhiterunWorld (0x1a26f)"));

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
}

TEST_CASE("Activation of an unexisting ref doesn't throw", "[PartOne][espm]")
{
  auto& partOne = GetPartOne();
  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 22572, -8634, -3597 }, 0, 0x1a26f);
  partOne.SetUserActor(0, 0xff000000);

  DoMessage(partOne, 0,
            nlohmann::json{ { "t", MsgType::Activate },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", 0xdeadbeef },
                                { "isSecondActivation", false } } } });

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
}

TEST_CASE("See harvested PurpleMountainFlower in Whiterun", "[PartOne][espm]")
{
  auto& partOne = GetPartOne();
  partOne.Messages().clear();

  const auto refrId = 0x0100122a;
  auto& refr = partOne.worldState.GetFormAt<MpObjectReference>(refrId);

  refr.SetHarvested(true);

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 22572, -8634, -3597 }, 0, 0x1a26f);
  partOne.SetUserActor(0, 0xff000000);

  auto res = FindRefrMessage<CreateActorMessage>(partOne, refrId);
  REQUIRE(res.filteredMessages.size() == 1);
  REQUIRE(res.filteredMessages[0].props.isHarvested.has_value());
  REQUIRE(*res.filteredMessages[0].props.isHarvested == true);

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
  refr.SetHarvested(false);
}

TEST_CASE("See open DisplayCaseSmFlat01 in Whiterun", "[PartOne][espm]")
{
  auto& partOne = GetPartOne();
  partOne.Messages().clear();

  const auto refrId = 0x72080;
  auto& refr = partOne.worldState.GetFormAt<MpObjectReference>(refrId);

  refr.SetOpen(true);

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 25217.0293, -7373.9536, -3317.6880 }, 0,
                      0x1a26f);
  partOne.SetUserActor(0, 0xff000000);

  auto res = FindRefrMessage<CreateActorMessage>(partOne, refrId);
  REQUIRE(res.filteredMessages.size() == 1);
  REQUIRE(res.filteredMessages[0].props.isOpen.has_value());
  REQUIRE(*res.filteredMessages[0].props.isOpen == true);

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
  refr.SetOpen(false);
}

TEST_CASE("Activate DisplayCaseSmFlat01 in Whiterun", "[PartOne][espm]")
{

  auto& partOne = GetPartOne();

  partOne.Messages().clear();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 25217.0293, -7373.9536, -3317.6880 }, 0,
                      0x1a26f);
  partOne.SetUserActor(0, 0xff000000);

  const auto refrId = 0x72080;
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);

  auto res = FindRefrMessage<CreateActorMessage>(partOne, refrId);
  REQUIRE(res.filteredMessages.size() == 1);

  partOne.Messages().clear();

  REQUIRE(!ref.IsOpen());
  DoMessage(partOne, 0,
            nlohmann::json{ { "t", MsgType::Activate },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", refrId },
                                { "isSecondActivation", false } } } });
  REQUIRE(ref.IsOpen());
  DoMessage(partOne, 0,
            nlohmann::json{ { "t", MsgType::Activate },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", refrId },
                                { "isSecondActivation", false } } } });
  REQUIRE(!ref.IsOpen());

  REQUIRE(partOne.Messages().size() == 2);
  REQUIRE(partOne.Messages()[0].j["dataDump"] == "true");
  REQUIRE(partOne.Messages()[0].j["idx"] == ref.GetIdx());
  REQUIRE(partOne.Messages()[0].j["propName"] == "isOpen");
  REQUIRE(partOne.Messages()[0].j["t"] == MsgType::UpdateProperty);
  REQUIRE(partOne.Messages()[1].j["dataDump"] == "false");
  REQUIRE(partOne.Messages()[1].j["idx"] == ref.GetIdx());
  REQUIRE(partOne.Messages()[1].j["propName"] == "isOpen");
  REQUIRE(partOne.Messages()[1].j["t"] == MsgType::UpdateProperty);

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
}

TEST_CASE("Activate WRDoorMainGate01 in Whiterun", "[PartOne][espm]")
{
  auto& partOne = GetPartOne();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 19367.3379, -7433.0698, -3547.4492 }, 0,
                      0x1a26f);
  partOne.SetUserActor(0, 0xff000000);

  partOne.Messages().clear();
  auto refrId = 0x1b1f3;
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);
  ref.SetRelootTime(kRelootWindow);
  DoMessage(partOne, 0,
            nlohmann::json{ { "t", MsgType::Activate },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", refrId },
                                { "isSecondActivation", false } } } });
  REQUIRE(partOne.Messages().size() >= 1);
  REQUIRE(partOne.Messages()[0].j["dataDump"] == "true");
  REQUIRE(partOne.Messages()[0].j["idx"] == ref.GetIdx());
  REQUIRE(partOne.Messages()[0].j["propName"] == "isOpen");
  REQUIRE(partOne.Messages()[0].j["t"] == MsgType::UpdateProperty);

  REQUIRE(partOne.Messages().size() >= 2);
  REQUIRE(partOne.Messages()[1].j["t"] == MsgType::Teleport);
  REQUIRE(partOne.Messages()[1].j["pos"].dump() ==
          nlohmann::json{ 19243.53515625, -7427.3427734375, -3595.4052734375 }
            .dump());
  REQUIRE(partOne.Messages()[1].j["rot"].dump() ==
          nlohmann::json{ 0.0, -0.0, -89.99922180175781 }.dump());
  REQUIRE(partOne.Messages()[1].j["worldOrCell"] == 0x3c);

  auto& ac = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  REQUIRE(ac.GetCellOrWorld() == FormDesc::Tamriel());

  partOne.Messages().clear();
  std::this_thread::sleep_for(kRelootWait);
  partOne.Tick();

  REQUIRE(ref.IsOpen() == false);

  REQUIRE((ac.GetPos() -
           NiPoint3(19243.53515625, -7427.3427734375, -3595.4052734375))
            .Length() < 5);
  REQUIRE(ac.GetAngle() == NiPoint3{ 0.0, -0.0, -89.99922180175781 });

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
}

TEST_CASE("Activate PurpleMountainFlower in Whiterun", "[PartOne][espm]")
{
  auto& partOne = GetPartOne();

  partOne.Messages().clear();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 22572, -8634, -3597 }, 0, 0x1a26f);
  partOne.SetUserActor(0, 0xff000000);
  auto& ac = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  ac.RemoveAllItems();

  const auto refrId = 0x0100122a;
  const auto MountainFlower01Purple = 0x77e1e;

  auto res = FindRefrMessage<CreateActorMessage>(partOne, refrId);
  REQUIRE(res.filteredMessages.size() == 1);

  partOne.Messages().clear();

  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);
  ref.SetRelootTime(kRelootWindow);

  REQUIRE(!ref.IsHarvested());

  DoMessage(partOne, 0,
            nlohmann::json{ { "t", MsgType::Activate },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", refrId },
                                { "isSecondActivation", false } } } });

  partOne.Tick(); // send deferred inventory update messages

  REQUIRE(partOne.Messages().size() >= 3);
  REQUIRE(partOne.Messages()[0].j["idx"] == ref.GetIdx());
  REQUIRE(partOne.Messages()[0].j["t"] == MsgType::UpdateProperty);
  REQUIRE(partOne.Messages()[0].j["dataDump"] == "true");
  REQUIRE(partOne.Messages()[0].j["propName"] == "isHarvested");
  REQUIRE(partOne.Messages()[1].j["t"] == MsgType::OpenContainer);
  REQUIRE(partOne.Messages()[1].j["target"] == ref.GetFormId());
  REQUIRE(partOne.Messages()[2].j["t"] == MsgType::SetInventory);
  REQUIRE(partOne.Messages()[2].j["inventory"].dump() ==
          nlohmann::json({ { "entries",
                             { { { "baseId", MountainFlower01Purple },
                                 { "count", 1 } } } } })
            .dump());

  REQUIRE(ref.IsHarvested());

  // We should not be able to harvest already harvested items
  REQUIRE(ac.GetInventory().GetTotalItemCount() == 1);
  ref.Activate(ac);
  REQUIRE(ac.GetInventory().GetTotalItemCount() == 1);

  partOne.Tick();
  REQUIRE(ref.IsHarvested());

  partOne.Messages().clear();
  std::this_thread::sleep_for(kRelootWait);

  partOne.Tick();
  REQUIRE(!ref.IsHarvested());
  REQUIRE(partOne.Messages().size() == 1);
  REQUIRE(partOne.Messages()[0].j["t"] == MsgType::UpdateProperty);
  REQUIRE(partOne.Messages()[0].j["dataDump"] == "false");
  REQUIRE(partOne.Messages()[0].j["propName"] == "isHarvested");

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
}

TEST_CASE("An activation from beyond reach harvests nothing",
          "[PartOne][espm]")
{
  // thuum docs/verbs/activation-reach.md: the same flower as above, first
  // from 2500 units away (refused), then from beside it (harvested). The
  // actor starts beside it so the reference is loaded either way.
  auto& partOne = GetPartOne();
  partOne.Messages().clear();

  DoConnect(partOne, 0);
  const NiPoint3 beside = { 22572, -8634, -3597 };
  partOne.CreateActor(0xff000000, beside, 0, 0x1a26f);
  partOne.SetUserActor(0, 0xff000000);
  auto& ac = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  ac.RemoveAllItems();

  const auto refrId = 0x0100122a;
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);
  REQUIRE(!ref.IsHarvested());

  const auto activate = [&] {
    DoMessage(partOne, 0,
              nlohmann::json{ { "t", MsgType::Activate },
                              { "data",
                                { { "caster", 0x14 },
                                  { "target", refrId },
                                  { "isSecondActivation", false } } } });
    partOne.Tick();
  };

  ac.SetPos(beside + NiPoint3{ 2500, 0, 0 });
  activate();
  REQUIRE(!ref.IsHarvested());
  REQUIRE(ac.GetInventory().GetTotalItemCount() == 0);

  ac.SetPos(beside);
  activate();
  REQUIRE(ref.IsHarvested());
  REQUIRE(ac.GetInventory().GetTotalItemCount() == 1);

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
}

// thuum docs/verbs/containers.md (ADR-067 decisions 1 and 6, Eli after
// playtest 43: "wrap the exclusive container hold"): a container holds the
// one player who opened it. A second player's activation is refused while
// it does: E_CONTAINER_BUSY, no OpenContainer and no inventory to the
// refused seat, a notification naming the holder (its appearance's name);
// the holder hears nothing of it and its moves stay its own, with no live
// redraw to anyone. A move by the refused player moves nothing and raises
// no exception; the mover hears both inventories again. The holder's close
// shuts the chest and frees it for the second.
TEST_CASE("One player holds a container: a second's activation is refused "
          "with the holder named, the holder's view and moves untouched, "
          "the refused player's move resynced, and the second admitted once "
          "the holder leaves",
          "[PartOne][espm]")
{
  auto& partOne = GetPartOne();
  auto refrId = 0x20570;
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);
  ref.SetRelootTime(kRelootWindow);
  ref.SetChanceNoneOverride(0);

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 21272.0000, -7816.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.SetUserActor(0, 0xff000000);
  DoConnect(partOne, 1);
  partOne.CreateActor(0xff000001, { 21300.0000, -7816.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.SetUserActor(1, 0xff000001);
  auto& first = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  auto& second = partOne.worldState.GetFormAt<MpActor>(0xff000001);
  first.RemoveAllItems();
  second.RemoveAllItems();
  // the holder has a name (its appearance's): the refused player hears it
  Appearance named;
  named.name = "Rotfern";
  first.SetAppearance(&named);

  auto& messages = partOne.Messages();
  const auto sentTo = [&](Networking::UserId user, MsgType t) {
    return std::any_of(
      messages.begin(), messages.end(), [&](PartOne::Message& m) {
        return m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(t);
      });
  };
  const auto chestInventoryTo = [&](Networking::UserId user) {
    return std::any_of(
      messages.begin(), messages.end(), [&](PartOne::Message& m) {
        return m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::UpdateProperty) &&
          m.j.contains("propName") && m.j["propName"] == "inventory" &&
          m.j["idx"] == ref.GetIdx();
      });
  };
  const auto notifiedOf = [&](Networking::UserId user, const char* text) {
    return std::any_of(
      messages.begin(), messages.end(), [&](PartOne::Message& m) {
        return m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::SpSnippet) &&
          m.j["class"] == "Debug" && m.j["function"] == "Notification" &&
          m.j["arguments"][0].get<std::string>() == text;
      });
  };

  ref.Activate(first);
  REQUIRE(ref.IsOpen());
  REQUIRE(ref.IsContainerOccupant(&first));
  REQUIRE(ref.GetInventory().GetTotalItemCount() > 0);

  // the second is refused: no OpenContainer and no inventory to it, the
  // notification with the holder's name, no occupancy; the holder hears
  // nothing of it
  partOne.Messages().clear();
  ref.Activate(second);
  partOne.Tick(); // snippets are deferred (SpSnippet.cpp)
  REQUIRE(ref.IsOpen());
  REQUIRE_FALSE(sentTo(1, MsgType::OpenContainer));
  REQUIRE_FALSE(chestInventoryTo(1));
  REQUIRE(notifiedOf(1, "Rotfern is searching this"));
  REQUIRE_FALSE(ref.IsContainerOccupant(&second));
  REQUIRE(ref.IsContainerOccupant(&first));
  REQUIRE_FALSE(sentTo(0, MsgType::OpenContainer));
  REQUIRE_FALSE(chestInventoryTo(0));
  REQUIRE_FALSE(notifiedOf(0, "Rotfern is searching this"));

  // the refused player's take: refused with no exception, nothing moves, it
  // hears its own inventory and the chest's at once
  const auto count = ref.GetInventory().GetTotalItemCount();
  const auto wanted = ref.GetInventory().entries.at(0).baseId;
  partOne.Messages().clear();
  REQUIRE_NOTHROW(ref.TakeItem(second, { wanted, 1 }));
  REQUIRE(ref.GetInventory().GetTotalItemCount() == count);
  REQUIRE(second.GetInventory().GetTotalItemCount() == 0);
  REQUIRE(sentTo(1, MsgType::SetInventory));
  REQUIRE(chestInventoryTo(1));

  // the holder's take: the item moves, no live redraw goes anywhere
  // (decision 1: the contents sync at the open and at the close alone)
  partOne.Messages().clear();
  REQUIRE_NOTHROW(ref.TakeItem(first, { wanted, 1 }));
  partOne.Tick(); // the holder's own SetInventory is deferred
  REQUIRE(first.GetInventory().GetItemCount(wanted) == 1);
  REQUIRE(ref.GetInventory().GetTotalItemCount() == count - 1);
  REQUIRE_FALSE(chestInventoryTo(0));
  REQUIRE_FALSE(chestInventoryTo(1));
  REQUIRE_FALSE(sentTo(1, MsgType::SetInventory));

  // the holder leaves: shut, the hold released; the second's activation now
  // opens it
  ref.Activate(first, false, true);
  REQUIRE_FALSE(ref.IsOpen());
  REQUIRE_FALSE(ref.IsContainerOccupant(&first));
  partOne.Messages().clear();
  ref.Activate(second);
  REQUIRE(ref.IsOpen());
  REQUIRE(ref.IsContainerOccupant(&second));
  REQUIRE(sentTo(1, MsgType::OpenContainer));
  REQUIRE(chestInventoryTo(1));
  ref.Activate(second, false, true);
  REQUIRE_FALSE(ref.IsOpen());

  DoDisconnect(partOne, 0);
  DoDisconnect(partOne, 1);
  partOne.DestroyActor(0xff000000);
  partOne.DestroyActor(0xff000001);
}

// thuum docs/verbs/containers.md (ADR-067 decision 1, Eli after playtest 42:
// "fuck live updates of containers"): a container's contents sync at its
// open (the record to the opener) and at its close (the record to every
// listener of the reference: a watcher's engine copy and crosshair, the next
// opener's included), never while it is open
TEST_CASE("A container's record reaches every listener at the close and no "
          "one while it is open",
          "[PartOne][espm]")
{
  auto& partOne = GetPartOne();
  auto refrId = 0x20570;
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);
  ref.SetRelootTime(kRelootWindow);
  ref.SetChanceNoneOverride(0);

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 21272.0000, -7816.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.SetUserActor(0, 0xff000000);
  DoConnect(partOne, 1);
  partOne.CreateActor(0xff000001, { 21300.0000, -7816.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.SetUserActor(1, 0xff000001);
  auto& first = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  first.RemoveAllItems();

  auto& messages = partOne.Messages();
  const auto chestInventoryTo = [&](Networking::UserId user) {
    return std::find_if(
      messages.begin(), messages.end(), [&](PartOne::Message& m) {
        return m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::UpdateProperty) &&
          m.j.contains("propName") && m.j["propName"] == "inventory" &&
          m.j["idx"] == ref.GetIdx();
      });
  };

  // the open: the record to the opener alone
  partOne.Messages().clear();
  ref.Activate(first);
  REQUIRE(ref.IsOpen());
  REQUIRE(ref.GetInventory().GetTotalItemCount() > 0);
  REQUIRE(chestInventoryTo(0) != messages.end());
  REQUIRE(chestInventoryTo(1) == messages.end());

  // the holder's take: the record changes, no listener hears of it yet
  const auto taken = ref.GetInventory().entries.at(0).baseId;
  partOne.Messages().clear();
  ref.TakeItem(first, { taken, 1 });
  partOne.Tick();
  REQUIRE(chestInventoryTo(0) == messages.end());
  REQUIRE(chestInventoryTo(1) == messages.end());

  // the close: the record as it stands to both players, the watcher included
  partOne.Messages().clear();
  ref.Activate(first, false, true);
  REQUIRE_FALSE(ref.IsOpen());
  const auto toWatcher = chestInventoryTo(1);
  REQUIRE(toWatcher != messages.end());
  REQUIRE(toWatcher->j["dataDump"].get<std::string>() ==
          ref.GetInventory().ToJson().dump());
  REQUIRE(chestInventoryTo(0) != messages.end());

  DoDisconnect(partOne, 0);
  DoDisconnect(partOne, 1);
  partOne.DestroyActor(0xff000000);
  partOne.DestroyActor(0xff000001);
}

// thuum docs/verbs/containers.md (ADR-067 decision 6): a put or a take by a
// player not in the container (its hold released, a corpse's loot menu its
// engine opened on its own, a forged message) moves nothing and raises no
// exception: E_CONTAINER_OCCUPANT is logged and the mover hears its own
// inventory and the container's at once, as every refused move does
TEST_CASE("A move by a player not in the container is refused without an "
          "exception, moves nothing, and resyncs the mover",
          "[PartOne][espm]")
{
  auto& partOne = GetPartOne();
  auto refrId = 0x20570;
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);
  ref.SetRelootTime(kRelootWindow);
  ref.SetChanceNoneOverride(0);

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 21272.0000, -7816.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.SetUserActor(0, 0xff000000);
  auto& actor = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  actor.RemoveAllItems();

  // the record exists: opened and closed once, the hold released
  ref.Activate(actor);
  ref.Activate(actor, false, true);
  REQUIRE_FALSE(ref.IsOpen());
  REQUIRE_FALSE(ref.IsContainerOccupant(&actor));
  const auto before = ref.GetInventory().GetTotalItemCount();
  REQUIRE(before > 0);
  const auto wanted = ref.GetInventory().entries.at(0).baseId;

  auto& messages = partOne.Messages();
  const auto resynced = [&]() {
    const bool own =
      std::any_of(messages.begin(), messages.end(), [&](PartOne::Message& m) {
        return m.userId == 0 && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::SetInventory);
      });
    const bool chest =
      std::any_of(messages.begin(), messages.end(), [&](PartOne::Message& m) {
        return m.userId == 0 && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::UpdateProperty) &&
          m.j.contains("propName") && m.j["propName"] == "inventory" &&
          m.j["idx"] == ref.GetIdx();
      });
    return own && chest;
  };

  // a take
  partOne.Tick(); // the open's and close's deferred messages flushed
  partOne.Messages().clear();
  REQUIRE_NOTHROW(ref.TakeItem(actor, { wanted, 1 }));
  REQUIRE(ref.GetInventory().GetTotalItemCount() == before);
  REQUIRE(actor.GetInventory().GetTotalItemCount() == 0);
  REQUIRE(resynced());

  // a put
  actor.AddItem(0x12eb7, 1);
  const auto chestHad = ref.GetInventory().GetItemCount(0x12eb7);
  partOne.Tick(); // the AddItem's own deferred SetInventory flushed
  partOne.Messages().clear();
  REQUIRE_NOTHROW(ref.PutItem(actor, { 0x12eb7, 1 }));
  REQUIRE(ref.GetInventory().GetItemCount(0x12eb7) == chestHad);
  REQUIRE(actor.GetInventory().GetItemCount(0x12eb7) == 1);
  REQUIRE(resynced());

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
}

// thuum (playtest 37, 2026-10-28, Eli: "shouldn't be that hard to change that
// to on cell load right?"): a container's recorded inventory reaches every
// player whose grid loads it, inside its CreateActor at the subscribe, so the
// engine copy and its crosshair label are the server's before any open; an
// untouched container (no record: both engines hold the base's default)
// sends none
TEST_CASE("A container's recorded inventory reaches a player who never opened "
          "it at the subscribe; an untouched container sends none",
          "[PartOne][espm]")
{
  auto& partOne = GetPartOne();
  auto refrId = 0x20570;
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);
  ref.SetRelootTime(kRelootWindow);
  ref.SetChanceNoneOverride(0);

  auto& messages = partOne.Messages();
  const auto createdFor = [&](Networking::UserId userId) {
    return std::find_if(
      messages.begin(), messages.end(), [&](PartOne::Message& m) {
        return m.userId == userId && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::CreateActor) &&
          m.j.contains("refrId") && m.j["refrId"] == refrId;
      });
  };

  // untouched: the first player's word for the barrel carries no inventory
  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 21272.0000, -7816.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.Messages().clear();
  partOne.SetUserActor(0, 0xff000000);
  auto& first = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  REQUIRE_FALSE(ref.HasRecordedInventory());
  auto untouched = createdFor(0);
  REQUIRE(untouched != messages.end());
  REQUIRE_FALSE(untouched->j["props"].contains("inventory"));

  // the first opens it and leaves: the base's roll is the record now
  ref.Activate(first);
  REQUIRE(ref.IsOpen());
  REQUIRE(ref.GetInventory().GetTotalItemCount() > 0);
  ref.Activate(first, false, true);
  REQUIRE_FALSE(ref.IsOpen());
  REQUIRE(ref.HasRecordedInventory());

  // the second player never opened it: its CreateActor for the barrel
  // carries the record
  DoConnect(partOne, 1);
  partOne.CreateActor(0xff000001, { 21300.0000, -7816.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.Messages().clear();
  partOne.SetUserActor(1, 0xff000001);
  auto recorded = createdFor(1);
  REQUIRE(recorded != messages.end());
  REQUIRE(recorded->j["props"].contains("inventory"));
  REQUIRE(recorded->j["props"]["inventory"] == ref.GetInventory().ToJson());

  DoDisconnect(partOne, 0);
  DoDisconnect(partOne, 1);
  partOne.DestroyActor(0xff000000);
  partOne.DestroyActor(0xff000001);
}

TEST_CASE("BarrelFood01 PutItem/TakeItem", "[PartOne][espm]")
{
  auto& partOne = GetPartOne();
  auto refrId = 0x20570;
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);
  ref.SetRelootTime(kRelootWindow);

  REQUIRE(ref.GetInventory().IsEmpty());

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 21272.0000, -7816.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.SetUserActor(0, 0xff000000);

  auto& actor = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  actor.RemoveAllItems();

  // not in it: the put is refused (E_CONTAINER_OCCUPANT, ADR-067 decision
  // 6) with no exception and nothing moves
  REQUIRE_NOTHROW(ref.PutItem(actor, { 0x12eb7, 2 }));
  REQUIRE(ref.GetInventory().IsEmpty());

  // Activation forces base container to be added
  partOne.Messages().clear();
  REQUIRE(ref.GetInventory().GetTotalItemCount() == 0);
  ref.SetChanceNoneOverride(0); // LeveledItems will never produce zero items
  ref.Activate(actor);
  REQUIRE(ref.GetInventory().GetTotalItemCount() > 0);

  REQUIRE(partOne.Messages().size() >= 1);
  REQUIRE(partOne.Messages()[0].j["t"] == MsgType::UpdateProperty);
  REQUIRE(partOne.Messages()[0].j["propName"] == "isOpen");
  REQUIRE(partOne.Messages()[0].j["idx"] == ref.GetIdx());
  REQUIRE(partOne.Messages()[0].j["dataDump"] == "true");
  REQUIRE(partOne.Messages().size() >= 2);
  REQUIRE(partOne.Messages()[1].j["t"] == MsgType::UpdateProperty);
  REQUIRE(partOne.Messages()[1].j["propName"] == "inventory");
  REQUIRE(partOne.Messages()[1].j["idx"] == ref.GetIdx());
  REQUIRE(partOne.Messages().size() == 3);
  REQUIRE(partOne.Messages()[2].j["t"] == MsgType::OpenContainer);
  REQUIRE(partOne.Messages()[2].j["target"] == ref.GetFormId());

  // thuum docs/verbs/npc-fate.md (playtest 44's 1c): a put of what the
  // mover's record does not hold is refused by name (E_PUT_NOT_HELD), no
  // throw, the mover resynced; nothing moves
  // ... and the mover hears both inventories again (its own SetInventory
  // and the chest's "inventory" property: a CONT)
  partOne.Messages().clear();
  REQUIRE_NOTHROW(ref.PutItem(actor, { 0x12eb7, 2 }));
  REQUIRE(ref.GetInventory().GetItemCount(0x12eb7) == 0);
  {
    bool ownSent = false;
    bool chestSent = false;
    for (const auto& m : partOne.Messages()) {
      if (m.j["t"] == MsgType::SetInventory) {
        ownSent = true;
      }
      if (m.j["t"] == MsgType::UpdateProperty &&
          m.j["propName"] == "inventory" && m.j["idx"] == ref.GetIdx()) {
        chestSent = true;
      }
    }
    REQUIRE(ownSent);
    REQUIRE(chestSent);
  }

  actor.AddItem(0x12eb7, 1);
  REQUIRE_NOTHROW(ref.PutItem(actor, { 0x12eb7, 2 }));
  REQUIRE(ref.GetInventory().GetItemCount(0x12eb7) == 0);
  REQUIRE(actor.GetInventory().GetItemCount(0x12eb7) == 1);

  actor.AddItem(0x12eb7, 1);

  // On the first PutItem, we use DoMessage to ensure our messages are parsed
  // and processed successfully.
  // "Worn" state may be sent by the real skymp client. Ignoring then.
  partOne.Messages().clear();
  DoMessage(partOne, 0,
            { { "t", MsgType::PutItem },
              { "baseId", 0x12eb7 },
              { "count", 2 },
              { "worn", true },
              { "target", refrId } });
  partOne.Tick(); // send deferred inventory update messages
  REQUIRE(partOne.Messages().size() == 1);
  REQUIRE(partOne.Messages()[0].j["t"] == MsgType::SetInventory);

  REQUIRE(actor.GetInventory().IsEmpty());
  REQUIRE(ref.GetInventory().GetItemCount(0x12eb7) == 2);

  // Keeps items from base container
  auto copy = ref.GetInventory();
  copy.RemoveItems({ { 0x12eb7, 2 } });
  REQUIRE(copy.GetTotalItemCount() > 0);

  partOne.Messages().clear();
  DoMessage(partOne, 0,
            { { "t", MsgType::TakeItem },
              { "baseId", 0x12eb7 },
              { "count", 1 },
              { "target", refrId } });
  partOne.Tick(); // send deferred inventory update messages
  REQUIRE(partOne.Messages().size() == 1);
  REQUIRE(partOne.Messages()[0].j["t"] == MsgType::SetInventory);
  REQUIRE(ref.GetInventory().GetItemCount(0x12eb7) == 1);
  REQUIRE(actor.GetInventory().GetItemCount(0x12eb7) == 1);

  // Reloot check
  {
    // Take all items from the barrel. This action forces reloot.
    for (auto e : ref.GetInventory().entries)
      ref.TakeItem(actor, e);

    REQUIRE(ref.GetInventory().IsEmpty() == true);

    std::this_thread::sleep_for(kRelootWait);
    partOne.Tick();

    REQUIRE(ref.GetInventory().IsEmpty() == false);
  }

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
}

TEST_CASE("Server creates and destroys an object for user correcly",
          "[PartOne][espm]")
{
  auto& partOne = GetPartOne();
  partOne.Messages().clear();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000ABC, { 16230, -8377, -4564 }, 180.f, 0x3c);
  partOne.SetUserActor(0, 0xff000ABC);

  auto refId = 0x01000f69;

  auto res = FindRefrMessage<CreateActorMessage>(partOne, refId);
  REQUIRE(res.filteredMessages.size() == 1);

  auto& ac = partOne.worldState.GetFormAt<MpActor>(0xff000ABC);
  ac.SetPos({ 0, 0, 0 });

  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refId);

  auto res2 = FindRefrMessageIdx<DestroyActorMessage>(partOne, ref.GetIdx());
  REQUIRE(res2.filteredMessages.size() == 1);

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000ABC);
}

TEST_CASE("Activate BarrelFood01 in Whiterun (open/close)", "[PartOne][espm]")
{
  auto& partOne = GetPartOne();

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 21272.0000, -7816.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.SetUserActor(0, 0xff000000);

  auto refrId = 0x20570;
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);

  // Open/close container
  DoMessage(partOne, 0,
            nlohmann::json{ { "t", MsgType::Activate },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", refrId },
                                { "isSecondActivation", false } } } });
  REQUIRE(ref.IsOpen());
  DoMessage(partOne, 0,
            nlohmann::json{ { "t", MsgType::Activate },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", refrId },
                                { "isSecondActivation", true } } } });
  REQUIRE(!ref.IsOpen());

  DoMessage(partOne, 0,
            nlohmann::json{ { "t", MsgType::Activate },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", refrId },
                                { "isSecondActivation", false } } } });
  REQUIRE(ref.IsOpen());

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);

  // Actor destruction forces contaner close
  REQUIRE(!ref.IsOpen());
}

TEST_CASE("Activate torch", "[espm][PartOne]")
{
  auto& partOne = GetPartOne();
  partOne.Messages().clear();
  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 6507.0527, -5283.5664, -3263.1714 }, 0,
                      0x56C1B);
  partOne.SetUserActor(0, 0xff000000);
  auto& ac = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  ac.RemoveAllItems();

  const auto refrId = 0x671a8;
  const auto torchBaseId = 0x1d4ec;

  auto res = FindRefrMessage<CreateActorMessage>(partOne, refrId);
  REQUIRE(res.filteredMessages.size() == 1);

  partOne.Messages().clear();

  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);
  ref.SetRelootTime(kRelootWindow);

  REQUIRE(!ref.IsHarvested());

  DoMessage(partOne, 0,
            nlohmann::json{ { "t", MsgType::Activate },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", refrId },
                                { "isSecondActivation", false } } } });

  partOne.Tick(); // send deferred inventory update messages

  REQUIRE(partOne.Messages().size() >= 2);
  REQUIRE(partOne.Messages()[0].j["idx"] == ref.GetIdx());
  REQUIRE(partOne.Messages()[0].j["t"] == MsgType::UpdateProperty);
  REQUIRE(partOne.Messages()[0].j["dataDump"] == "true");
  REQUIRE(partOne.Messages()[0].j["propName"] == "isHarvested");
  REQUIRE(partOne.Messages()[1].j["t"] == MsgType::SetInventory);
  REQUIRE(
    partOne.Messages()[1].j["inventory"].dump() ==
    nlohmann::json(
      { { "entries", { { { "baseId", torchBaseId }, { "count", 1 } } } } })
      .dump());

  REQUIRE(ref.IsHarvested());

  // We should not be able to harvest already harvested items
  REQUIRE(ac.GetInventory().GetTotalItemCount() == 1);
  ref.Activate(ac);
  REQUIRE(ac.GetInventory().GetTotalItemCount() == 1);

  partOne.Tick();
  REQUIRE(ref.IsHarvested());

  partOne.Messages().clear();
  std::this_thread::sleep_for(kRelootWait);

  partOne.Tick();
  REQUIRE(!ref.IsHarvested());
  REQUIRE(partOne.Messages().size() == 1);
  REQUIRE(partOne.Messages()[0].j["t"] == MsgType::UpdateProperty);
  REQUIRE(partOne.Messages()[0].j["dataDump"] == "false");
  REQUIRE(partOne.Messages()[0].j["propName"] == "isHarvested");

  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
}

TEST_CASE("Regress test for 'Record ff00b5de doesn't exist'",
          "[PartOne][espm]")
{
  auto& partOne = GetPartOne();
  partOne.worldState.npcEnabled = true;
  partOne.Messages().clear();

  const auto refrId = 0x000524f2;

  // 1: C:\skymp\unit\PartOne_ActivateTest.cpp(598): FAILED:
  // 1: due to unexpected exception with message:
  // 1:   Record ff00b5de doesn't exist

  auto& actor = partOne.worldState.GetFormAt<MpActor>(refrId);

  // REQUIRE(actor.GetInventory().ToJson().dump() == "");

  // TODO: Why 0???
  // REQUIRE(actor.GetInventory().GetItemCount(0x0000B5DE) == 1);
}

TEST_CASE("Regress: LvlGiant mustn't have Fox race health", "[PartOne][espm]")
{
  auto& partOne = GetPartOne();
  partOne.worldState.npcEnabled = true;
  partOne.Messages().clear();

  const auto refrId = 0x000ecf8a;

  auto& actor = partOne.worldState.GetFormAt<MpActor>(refrId);

  uint32_t baseId = actor.GetBaseId();
  uint32_t raceId = actor.GetRaceId();

  auto baseActorValues = GetBaseActorValues(&partOne.worldState, baseId,
                                            raceId, actor.GetTemplateChain());

  REQUIRE(baseActorValues.health == 250.f);
}

// thuum docs/verbs/containers.md (M4's row): the rest of the container row.
// The barrel is BarrelFood01 0x20570 at (21272, -7816, -3608) in Whiterun
// 0x1a26f, the test world's one container.

TEST_CASE("A put or a take from beyond the activation reach is refused and "
          "the mover resynced; within reach it is taken",
          "[PartOne][espm][Containers]")
{
  auto& partOne = GetPartOne();
  auto refrId = 0x20570;
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);
  ref.SetRelootTime(kRelootWindow);

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 21272.0000, -7816.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.SetUserActor(0, 0xff000000);
  auto& actor = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  actor.RemoveAllItems();
  ref.SetChanceNoneOverride(0);
  ref.Activate(actor);
  REQUIRE(ref.IsContainerOccupant(&actor));
  actor.AddItem(0x12eb7, 2);

  // the mover walked 900 units off with the menu open: the put is refused,
  // nothing moves, the mover hears both inventories again
  actor.SetPos({ 21272.0000 + 900.f, -7816.0000, -3608.0000 });
  partOne.Messages().clear();
  DoMessage(partOne, 0,
            { { "t", MsgType::PutItem },
              { "baseId", 0x12eb7 },
              { "count", 2 },
              { "target", refrId } });
  partOne.Tick();
  REQUIRE(actor.GetInventory().GetItemCount(0x12eb7) == 2);
  REQUIRE(ref.GetInventory().GetItemCount(0x12eb7) == 0);
  bool resynced = false;
  for (auto& m : partOne.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::SetInventory) {
      resynced = true;
    }
  }
  REQUIRE(resynced);

  // back at the barrel: the put is taken
  actor.SetPos({ 21272.0000, -7816.0000, -3608.0000 });
  partOne.Messages().clear();
  DoMessage(partOne, 0,
            { { "t", MsgType::PutItem },
              { "baseId", 0x12eb7 },
              { "count", 2 },
              { "target", refrId } });
  partOne.Tick();
  REQUIRE(actor.GetInventory().GetItemCount(0x12eb7) == 0);
  REQUIRE(ref.GetInventory().GetItemCount(0x12eb7) == 2);

  // and a take from afar is refused the same way
  actor.SetPos({ 21272.0000, -7816.0000 + 900.f, -3608.0000 });
  DoMessage(partOne, 0,
            { { "t", MsgType::TakeItem },
              { "baseId", 0x12eb7 },
              { "count", 2 },
              { "target", refrId } });
  partOne.Tick();
  REQUIRE(ref.GetInventory().GetItemCount(0x12eb7) == 2);
  REQUIRE(actor.GetInventory().GetItemCount(0x12eb7) == 0);

  // the barrel left as found
  actor.SetPos({ 21272.0000, -7816.0000, -3608.0000 });
  ref.TakeItem(actor, { 0x12eb7, 2 });
  actor.RemoveAllItems();
  ref.Activate(actor, false, true);
  DoDisconnect(partOne, 0);
  partOne.DestroyActor(0xff000000);
}

TEST_CASE("A reloot asked for a time keeps that moment in the record, so a "
          "restart's remaining time is not pushed a full window later",
          "[PartOne][espm][Containers]")
{
  auto& partOne = GetPartOne();
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(0x20570);
  ref.SetRelootTime(kRelootWindow);
  REQUIRE(!ref.GetNextRelootMoment());

  const auto before = std::chrono::system_clock::now();
  ref.RequestReloot(std::chrono::seconds(3));
  const auto moment = ref.GetNextRelootMoment();
  REQUIRE(moment);
  // the record says about 3 s from now, not the 1 s window (the record holds
  // whole seconds, so the bounds allow one each way)
  REQUIRE(*moment - before >= std::chrono::milliseconds(1'900));
  REQUIRE(*moment - before <= std::chrono::milliseconds(4'100));

  // a restart's load asks for the remaining time and keeps the moment
  MpChangeForm saved = ref.GetChangeForm();
  partOne.worldState.LoadChangeForm(saved, partOne.CreateFormCallbacks());
  const auto again = ref.GetNextRelootMoment();
  REQUIRE(again);
  REQUIRE(*again - before >= std::chrono::milliseconds(900));
  REQUIRE(*again - before <= std::chrono::milliseconds(4'100));

  // left as found: the reloot fired by hand clears the moment
  ref.DoReloot();
  REQUIRE(!ref.GetNextRelootMoment());
}

TEST_CASE("A container saved open comes back shut at load, with nobody in it",
          "[PartOne][espm][Containers]")
{
  auto& partOne = GetPartOne();
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(0x20570);
  MpChangeForm saved = ref.GetChangeForm();
  saved.isOpen = true;
  partOne.worldState.LoadChangeForm(saved, partOne.CreateFormCallbacks());
  REQUIRE(!ref.IsOpen());
}

TEST_CASE("A disconnect releases the player's container: the chest shuts "
          "for the others when it was the last in it",
          "[PartOne][espm][Containers]")
{
  auto& partOne = GetPartOne();
  auto refrId = 0x20570;
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);
  ref.SetRelootTime(kRelootWindow);

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 21272.0000, -7816.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.SetUserActor(0, 0xff000000);
  DoConnect(partOne, 1);
  partOne.CreateActor(0xff000001, { 21272.0000, -7716.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.SetUserActor(1, 0xff000001);
  auto& actor = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  ref.SetChanceNoneOverride(0);
  ref.Activate(actor);
  REQUIRE(ref.IsOpen());
  REQUIRE(ref.IsContainerOccupant(&actor));
  REQUIRE(actor.OccupiedContainers().count(&ref) == 1);

  partOne.Messages().clear();
  DoDisconnect(partOne, 0);
  REQUIRE(!ref.IsOpen());
  REQUIRE(!ref.IsContainerOccupant(&actor));
  REQUIRE(actor.OccupiedContainers().empty());
  bool told = false;
  for (auto& m : partOne.Messages()) {
    if (m.userId == 1 && m.j["t"] == MsgType::UpdateProperty &&
        m.j["propName"] == "isOpen" && m.j["dataDump"] == "false") {
      told = true;
    }
  }
  REQUIRE(told);

  DoDisconnect(partOne, 1);
  partOne.DestroyActor(0xff000000);
  partOne.DestroyActor(0xff000001);
}

// thuum docs/verbs/containers.md (ADR-067 decision 6, superseding ADR-061
// decision 3's switch): the hold is the one model and no setting; a second
// player's activation of a held container is refused with a notification on
// its seat ("Someone is using this" when the holder has no name); the
// holder's close frees the container, and the holder's own re-activation
// keeps its hold
TEST_CASE("The container hold is the one model: a second player is refused "
          "while the first is in it, told someone is using it when the "
          "holder has no name, and admitted once the first leaves",
          "[PartOne][espm]")
{
  auto& partOne = GetPartOne();
  auto refrId = 0x20570;
  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(refrId);
  ref.SetRelootTime(kRelootWindow);
  ref.SetChanceNoneOverride(0);

  DoConnect(partOne, 0);
  partOne.CreateActor(0xff000000, { 21272.0000, -7816.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.SetUserActor(0, 0xff000000);
  DoConnect(partOne, 1);
  partOne.CreateActor(0xff000001, { 21300.0000, -7816.0000, -3608.0000 }, 0,
                      0x1a26f);
  partOne.SetUserActor(1, 0xff000001);
  auto& first = partOne.worldState.GetFormAt<MpActor>(0xff000000);
  auto& second = partOne.worldState.GetFormAt<MpActor>(0xff000001);
  REQUIRE_FALSE(static_cast<bool>(first.GetAppearance())); // no name

  ref.Activate(first);
  REQUIRE(ref.IsOpen());
  REQUIRE(ref.IsContainerOccupant(&first));

  // the second is refused: no OpenContainer, the notification, no occupancy
  partOne.Messages().clear();
  ref.Activate(second);
  partOne.Tick(); // snippets are deferred (SpSnippet.cpp)
  auto& messages = partOne.Messages();
  const auto openedFor = [&](Networking::UserId user) {
    return std::any_of(
      messages.begin(), messages.end(), [&](PartOne::Message& m) {
        return m.userId == user && m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::OpenContainer);
      });
  };
  const auto notified =
    std::find_if(messages.begin(), messages.end(), [&](PartOne::Message& m) {
      return m.userId == 1 && m.j.is_object() && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(MsgType::SpSnippet) &&
        m.j["class"] == "Debug" && m.j["function"] == "Notification";
    });
  REQUIRE_FALSE(openedFor(1));
  REQUIRE(notified != messages.end());
  REQUIRE(notified->j["arguments"][0].get<std::string>() ==
          "Someone is using this");
  REQUIRE_FALSE(ref.IsContainerOccupant(&second));
  REQUIRE(ref.IsContainerOccupant(&first));

  // the first leaves: shut; the second's activation now opens it
  ref.Activate(first, false, true);
  REQUIRE_FALSE(ref.IsOpen());
  partOne.Messages().clear();
  ref.Activate(second);
  REQUIRE(ref.IsOpen());
  REQUIRE(openedFor(1));
  REQUIRE(ref.IsContainerOccupant(&second));
  ref.Activate(second, false, true);
  REQUIRE_FALSE(ref.IsOpen());

  // the holder's own re-activation keeps its hold, one occupant still
  ref.Activate(first);
  ref.Activate(first);
  REQUIRE(ref.IsOpen());
  REQUIRE(ref.IsContainerOccupant(&first));
  REQUIRE_FALSE(ref.IsContainerOccupant(&second));
  ref.Activate(first, false, true);
  REQUIRE_FALSE(ref.IsOpen());

  DoDisconnect(partOne, 0);
  DoDisconnect(partOne, 1);
  partOne.DestroyActor(0xff000000);
  partOne.DestroyActor(0xff000001);
}
