#include "PartOne.h"
#include "TestUtils.hpp"
#include "script_compatibility_policies/HeuristicPolicy.h"
#include <catch2/catch_all.hpp>

#include "papyrus-vm/Structures.h"
#include "script_classes/PapyrusGame.h"

PartOne& GetPartOne();

TEST_CASE("GetForm", "[Papyrus][Game][espm]")
{
  PartOne& partOne = GetPartOne();
  PapyrusGame game;
  std::shared_ptr<spdlog::logger> logger;
  game.compatibilityPolicy.reset(new HeuristicPolicy(&partOne.worldState));

  constexpr const uint32_t foodBarrel = 0x20570;
  const auto& refer =
    partOne.worldState.GetFormAt<MpObjectReference>(foodBarrel);
  const IGameObject* pForm =
    partOne.worldState.LookupFormById(refer.GetFormId())->ToGameObject().get();
  const IGameObject* pPapyrusForm =
    static_cast<const IGameObject*>(game.GetFormEx(
      VarValue::None(), { VarValue(static_cast<int32_t>(foodBarrel)) }));
  REQUIRE(pForm == pPapyrusForm);
}

TEST_CASE("GetFormEx", "[Papyrus][Game][espm]")
{
  PartOne& partOne = GetPartOne();
  PapyrusGame game;
  std::shared_ptr<spdlog::logger> logger;
  game.compatibilityPolicy.reset(new HeuristicPolicy(&partOne.worldState));
  DoConnect(partOne, 0);
  const uint32_t formId =
    partOne.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  partOne.SetUserActor(0, formId);
  const IGameObject* pForm =
    partOne.worldState.LookupFormById(formId)->ToGameObject().get();
  const IGameObject* pPapyrusForm =
    static_cast<const IGameObject*>(game.GetFormEx(
      VarValue::None(), { VarValue(static_cast<int32_t>(formId)) }));
  REQUIRE(pForm == pPapyrusForm);
}

TEST_CASE("IncrementStat counts on the player's own client", "[Papyrus][Game]")
{
  // thuum docs/NATIVES.md, decided 2026-10-04: a misc stat lives in the
  // player's save and stats menu, so the server hands it to that client
  PartOne p;
  {
    auto ac =
      std::make_unique<MpActor>(LocationalData(), p.CreateFormCallbacks());
    p.worldState.AddForm(std::move(ac), 0xff000000);
  }
  auto& ac = p.worldState.GetFormAt<MpActor>(0xff000000);
  auto policy = new HeuristicPolicy(&p.worldState);
  policy->SetDefaultActor(VarValue::AttachTestStackId().GetMetaStackId(), &ac);
  PapyrusGame game;
  game.compatibilityPolicy.reset(policy);
  DoConnect(p, 3);
  p.SetUserActor(3, 0xff000000);
  p.Messages().clear();

  game.IncrementStat(VarValue::AttachTestStackId(),
                     { VarValue("Locks Picked"), VarValue(1) });
  p.Tick(); // deferred messages

  REQUIRE(p.Messages().size() == 1);
  REQUIRE(p.Messages()[0].userId == 3);
  REQUIRE(p.Messages()[0].j ==
          nlohmann::json{ { "t", 30 },
                          { "snippetIdx", 4294967295 },
                          { "selfId", 0 },
                          { "class", "Game" },
                          { "function", "IncrementStat" },
                          { "arguments", { "Locks Picked", 1 } } });
}
