#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <cmath>
#include <nlohmann/json.hpp>

PartOne& GetPartOne();

namespace {
constexpr uint32_t kWordlessHost = 0xff000c31;
constexpr uint32_t kWordlessGuest = 0xff000c32;
constexpr uint32_t kTamrielWorld = 0x3c;
constexpr uint32_t kWhiterunWorld = 0x1a26f;
// a resident another seat's engine walked through Whiterun's gate: his record
// stands inside the city's own worldspace, while a game that hosts nothing
// holds its own copy of him outside the gate, placed there by its engine's
// schedule, with no word (his record stands beyond that game's grid)
constexpr uint32_t kResident = 0x1a6a4;
const NiPoint3 kOutsideTheGate{ 19262.f, -7201.f, -3595.f };
const NiPoint3 kInsideTheGate{ 1500.f, -2600.f, -3200.5f };
}

TEST_CASE("A game that hosts nothing sights its wordless copy of a resident "
          "whose record stands in another worldspace, in a cell another game "
          "hosts, and is told to move the copy to the record",
          "[CellHost][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  DoConnect(p, 0);
  p.CreateActor(kWordlessHost, kOutsideTheGate, 0, kTamrielWorld);
  p.SetUserActor(0, kWordlessHost);
  DoConnect(p, 1);
  p.CreateActor(kWordlessGuest, kOutsideTheGate, 0, kTamrielWorld);
  p.SetUserActor(1, kWordlessGuest);
  const auto gate = p.worldState.CellOf(kTamrielWorld, kOutsideTheGate);
  REQUIRE(gate.has_value());
  DoMessage(p, 0,
            nlohmann::json{ { "t", MsgType::CellLoaded },
                            { "cell", *gate },
                            { "loaded", true } });
  REQUIRE(p.GetCellHost(*gate).player == kWordlessHost);

  const auto& form = p.worldState.LookupFormById(kResident);
  REQUIRE(form != nullptr);
  MpActor* resident = form->AsActor();
  REQUIRE(resident != nullptr);
  resident->SetCellOrWorld(
    FormDesc::FromFormId(kWhiterunWorld, p.worldState.espmFiles));
  resident->SetPos(kInsideTheGate);
  p.Messages().clear();

  DoMessage(
    p, 1,
    nlohmann::json{
      { "t", MsgType::ActorSightings },
      { "sightings",
        nlohmann::json::array({ nlohmann::json{
          { "refrId", kResident },
          { "worldOrCell", kTamrielWorld },
          { "pos",
            { kOutsideTheGate.x, kOutsideTheGate.y, kOutsideTheGate.z } },
          { "rot", { 0.f, 0.f, 90.f } } } }) } });
  p.Tick(); // snippets are deferred (SpSnippet.cpp)

  bool moved = false;
  for (auto& m : p.Messages()) {
    if (!(m.j.is_object() && m.j.contains("t") &&
          m.j["t"] == static_cast<int>(MsgType::SpSnippet) &&
          m.j.contains("class") && m.j["class"] == "TESModPlatform" &&
          m.j["function"] == "moveRefrToPosition")) {
      continue;
    }
    REQUIRE(m.userId == 1);
    moved = true;
    const auto& args = m.j["arguments"];
    REQUIRE(args.size() == 9);
    REQUIRE(args[0]["formId"] == kResident);
    REQUIRE(args[2]["type"] == "WorldSpace");
    REQUIRE(args[2]["formId"] == kWhiterunWorld);
    REQUIRE(std::abs(args[3].get<double>() - kInsideTheGate.x) < 1.0);
    REQUIRE(std::abs(args[4].get<double>() - kInsideTheGate.y) < 1.0);
  }
  REQUIRE(moved);
  REQUIRE(p.worldState.CellOf(*resident) != gate);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kWordlessHost);
  p.DestroyActor(kWordlessGuest);
  p.DestroyActor(kResident);
  p.worldState.npcEnabled = false;
}
