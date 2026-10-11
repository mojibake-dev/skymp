#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActionListener.h"
#include "MsgType.h"
#include "UpdateMovementMessage.h"

PartOne& GetPartOne();

// thuum docs/verbs/cast-teleport.md: a spell the server names as a teleport
// (server-settings.json castTeleports) permits its caster one jump within its
// reach of where it stood at the cast, and its cast reaches no other game.
// Skyrim.esm's Firebolt (0x12FD0, thuum lab/esm.py) stands in for Lost
// Grimoire's Teleportation, which the test's load order lacks.
namespace {
constexpr uint32_t kFirebolt = 0x00012fd0;
constexpr uint32_t kCaster = 0xff000000;
constexpr uint32_t kNeighbour = 0xff000001;

// The caster (user 0) at the origin with Firebolt in its right hand, and a
// neighbour (user 1) 300 units north, both in Tamriel
MpActor& Pair(PartOne& p)
{
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kCaster, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kCaster);
  p.CreateActor(kNeighbour, { 0, 300, 0 }, 0, 0x3c);
  p.SetUserActor(1, kNeighbour);
  auto& caster = p.worldState.GetFormAt<MpActor>(kCaster);
  Equipment eq;
  eq.rightSpell = kFirebolt;
  caster.SetEquipment(eq);
  return caster;
}

// the caster's game reports its cast (SpellCastTest's packet path)
void Cast(PartOne& p)
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", 0x14 },
                        { "target", 0 },
                        { "spell", kFirebolt },
                        { "isDualCasting", false },
                        { "interruptCast", false },
                        { "castingSource", 1 },
                        { "aimAngle", 0.f },
                        { "aimHeading", 0.f },
                        { "actorAnimationVariables",
                          { { "booleans", nlohmann::json::array() },
                            { "floats", nlohmann::json::array() },
                            { "integers", nlohmann::json::array() } } } } } });
}

// the caster's own movement report (ConsoleCommandTest's)
void Report(PartOne& p, MpActor& me, const NiPoint3& pos)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = 0;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateMovementMessage msg;
  msg.idx = me.GetIdx();
  msg.data.pos = { pos.x, pos.y, pos.z };
  msg.data.rot = { 0, 0, 0 };
  msg.data.isInJumpState = false;
  msg.data.isWeapDrawn = false;
  msg.data.isBlocking = false;
  msg.data.worldOrCell = 0x3c;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
}

// how many times the caster's game was sent back (Teleport2)
int SentBack(PartOne& p)
{
  int n = 0;
  for (auto& m : p.Messages()) {
    n += m.userId == 0 && m.j.contains("t") && m.j["t"] == MsgType::Teleport2;
  }
  return n;
}

// whether the neighbour's game heard the cast (SpellCast)
bool Relayed(PartOne& p)
{
  for (auto& m : p.Messages()) {
    if (m.userId == 1 && m.j.contains("t") && m.j["t"] == MsgType::SpellCast) {
      return true;
    }
  }
  return false;
}

void Leave(PartOne& p)
{
  p.SetCastTeleportSettings("[]");
  p.DestroyActor(kNeighbour);
  p.DestroyActor(kCaster);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}
}

TEST_CASE("A cast teleport's jump is taken once within its reach, and its "
          "cast reaches no other game",
          "[CastTeleport][espm]")
{
  PartOne& p = GetPartOne();
  p.SetCastTeleportSettings(
    R"([{"spell": "12fd0:Skyrim.esm", "reach": 3000}])");
  REQUIRE(p.TeleportReach(kFirebolt) == Catch::Approx(3000.f));
  auto& caster = Pair(p);
  p.Messages().clear();

  Cast(p);
  REQUIRE(!Relayed(p));

  // 2500 units east: past the ground budget's burst (2048), within the reach
  Report(p, caster, { 2500, 0, 0 });
  REQUIRE(caster.GetPos() == NiPoint3{ 2500, 0, 0 });
  REQUIRE(SentBack(p) == 0);

  // that was the one jump: the next is sent back
  Report(p, caster, { 5000, 0, 0 });
  REQUIRE(SentBack(p) == 1);
  REQUIRE(caster.GetPos() == NiPoint3{ 2500, 0, 0 });
  Leave(p);
}

TEST_CASE("A spell the server names no teleport permits no jump, and its "
          "cast is relayed",
          "[CastTeleport][espm]")
{
  PartOne& p = GetPartOne();
  p.SetCastTeleportSettings("[]");
  REQUIRE(p.TeleportReach(kFirebolt) < 0.f);
  auto& caster = Pair(p);
  p.Messages().clear();

  Cast(p);
  REQUIRE(Relayed(p));

  Report(p, caster, { 2500, 0, 0 });
  REQUIRE(SentBack(p) == 1);
  REQUIRE(caster.GetPos() == NiPoint3{ 0, 0, 0 });
  Leave(p);
}

TEST_CASE("A cast teleport's jump beyond its reach is sent back",
          "[CastTeleport][espm]")
{
  PartOne& p = GetPartOne();
  p.SetCastTeleportSettings(
    R"([{"spell": "12fd0:Skyrim.esm", "reach": 1000}])");
  auto& caster = Pair(p);
  p.Messages().clear();

  Cast(p);
  // 2500 units: beyond 1000 and the slack, and beyond the budget's burst
  Report(p, caster, { 2500, 0, 0 });
  REQUIRE(SentBack(p) == 1);
  REQUIRE(caster.GetPos() == NiPoint3{ 0, 0, 0 });
  Leave(p);
}
