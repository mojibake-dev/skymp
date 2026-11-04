#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActionListener.h"
#include "MpActor.h"
#include "MsgType.h"

PartOne& GetPartOne();

// thuum docs/verbs/spell-cast.md (the rune's glyph on the observer, queued by
// Eli 2026-11-02): a target-location cast carries the surface its caster's
// engine placed the glyph on (SpellCastData.decal: the pick's unit normal
// and the material id), relayed with the launch to every other game, which
// hands it to the same placement after its relayed launch. The decal rides
// the launch alone: a launch the server refuses (its origin past the spell's
// ray from the caster) goes out stripped, and the decal with it.

namespace {
constexpr uint32_t kCaster = 0xff000d11;
constexpr uint32_t kNeighbour = 0xff000d12;
// Frost Rune, Skyrim.esm SPEL 0x6796f: delivery target location, SPIT range
// 20 (lab/esm.py 2026-11-02), so LaunchRay is 440 units
constexpr uint32_t kFrostRune = 0x6796f;
// the summit's ground in x-rune-decal-frida 20261020-102818
constexpr uint32_t kMaterial = 0xe4d39ca3;

// the caster's cast with its launch from `origin` and the surface's decal
void CastWithDecal(PartOne& p, const NiPoint3& origin)
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", 0x14 },
                        { "target", 0 },
                        { "spell", kFrostRune },
                        { "isDualCasting", false },
                        { "interruptCast", false },
                        { "castingSource", 1 },
                        { "aimAngle", -0.9f },
                        { "aimHeading", 0.f },
                        { "hasLaunch", true },
                        { "originX", origin.x },
                        { "originY", origin.y },
                        { "originZ", origin.z },
                        { "launchAngleX", -0.9f },
                        { "launchAngleZ", 0.f },
                        { "actorAnimationVariables",
                          { { "booleans", nlohmann::json::array() },
                            { "floats", nlohmann::json::array() },
                            { "integers", nlohmann::json::array() } } },
                        { "decal",
                          { { "normalX", 0.f },
                            { "normalY", 0.f },
                            { "normalZ", 1.f },
                            { "materialId", kMaterial } } } } } });
}

// the casts the neighbour's game heard, each its data
std::vector<nlohmann::json> RelaysTo(PartOne& p, Networking::UserId user)
{
  std::vector<nlohmann::json> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(MsgType::SpellCast)) {
      out.push_back(m.j["data"]);
    }
  }
  return out;
}
}

TEST_CASE("A target-location cast's decal rides its launch to the other game "
          "and goes with the launch when the server strips it",
          "[SpellCast][espm]")
{
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kCaster, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kCaster);
  p.CreateActor(kNeighbour, { 0, 300, 0 }, 0, 0x3c);
  p.SetUserActor(1, kNeighbour);
  auto& caster = p.worldState.GetFormAt<MpActor>(kCaster);
  Equipment eq;
  eq.rightSpell = kFrostRune;
  caster.SetEquipment(eq);
  p.Messages().clear();

  // a launch 200 units ahead, inside the ray: the relay carries the decal
  CastWithDecal(p, { 0.f, 200.f, 0.f });
  auto relays = RelaysTo(p, 1);
  REQUIRE(relays.size() == 1);
  REQUIRE(relays[0]["hasLaunch"] == true);
  REQUIRE(relays[0].contains("decal"));
  REQUIRE(!relays[0]["decal"].is_null());
  REQUIRE(relays[0]["decal"]["normalZ"] == 1.f);
  REQUIRE(relays[0]["decal"]["materialId"] == kMaterial);

  // a launch 5,000 units out, past the ray and the offset: the launch is
  // stripped (E_SPELL_ORIGIN) and the decal with it
  p.Messages().clear();
  CastWithDecal(p, { 0.f, 5000.f, 0.f });
  relays = RelaysTo(p, 1);
  REQUIRE(relays.size() == 1);
  REQUIRE(relays[0]["hasLaunch"] == false);
  REQUIRE((!relays[0].contains("decal") || relays[0]["decal"].is_null()));

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kCaster);
  p.DestroyActor(kNeighbour);
}
