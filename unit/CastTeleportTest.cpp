#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

#include "ActionListener.h"
#include "MsgType.h"
#include "UpdateMovementMessage.h"

PartOne& GetPartOne();

// thuum docs/verbs/cast-teleport.md: a spell the server names as a teleport
// (server-settings.json castTeleports) permits its caster one jump within its
// reach of where it stood at the cast, and its cast reaches no other game.
// The entry's look plays in the other games on the caster's figure: its
// vanish at the cast; the figure placed, the vanish stopped, the appear and
// the sound at the landing (SpSnippets). Skyrim.esm's Firebolt (0x12FD0,
// thuum lab/esm.py) stands in for Lost Grimoire's Teleportation, which the
// test's load order lacks.
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
                        { "hasLaunch", false },
                        { "originX", 0.f },
                        { "originY", 0.f },
                        { "originZ", 0.f },
                        { "launchAngleX", 0.f },
                        { "launchAngleZ", 0.f },
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

// The look on other screens: Lost Grimoire's marker script's forms (thuum
// docs/verbs/cast-teleport.md: Skyrim.esm's MGTeleportOutEffect and
// MGTeleportInEffect, Dawnguard.esm's MAGPowersSupernaturalReflexesInMarker)
// in the test's load order, Dawnguard.esm third
constexpr uint32_t kVanish = 0x00102731;
constexpr uint32_t kAppear = 0x00102730;
constexpr uint32_t kSound = 0x02010ec4;
constexpr auto kLooked =
  R"([{"spell": "12fd0:Skyrim.esm", "reach": 3000, "look": {"vanish": )"
  R"("102731:Skyrim.esm", "appear": "102730:Skyrim.esm", "appearSeconds": )"
  R"(1.75, "sound": "010ec4:Dawnguard.esm"}}])";

// an object argument: the caster's figure, or form 0 for None
nlohmann::json Ref(uint32_t formId)
{
  return nlohmann::json{ { "formId", formId }, { "type", "ObjectReference" } };
}

// the look's snippets a user's game was sent, in order (VisualEffect, Sound,
// and ObjectReference.SetPosition)
std::vector<nlohmann::json> LookSnippets(PartOne& p, Networking::UserId user)
{
  p.Tick(); // snippets are deferred
  std::vector<nlohmann::json> out;
  for (auto& m : p.Messages()) {
    if (m.userId != user || !m.j.contains("t") ||
        m.j["t"] != MsgType::SpSnippet) {
      continue;
    }
    const auto cl = m.j["class"].get<std::string>();
    if (cl == "VisualEffect" || cl == "Sound" ||
        (cl == "ObjectReference" && m.j["function"] == "SetPosition")) {
      out.push_back(m.j);
    }
  }
  return out;
}

// the four snippets of a landing at `at`, in their order
void RequireLanding(const std::vector<nlohmann::json>& s, const NiPoint3& at)
{
  REQUIRE(s.size() == 4);
  REQUIRE(s[0]["class"] == "ObjectReference");
  REQUIRE(s[0]["function"] == "SetPosition");
  REQUIRE(s[0]["selfId"] == kCaster);
  REQUIRE(s[0]["arguments"] ==
          nlohmann::json::array({ static_cast<double>(at.x),
                                  static_cast<double>(at.y),
                                  static_cast<double>(at.z) }));
  REQUIRE(s[1]["class"] == "VisualEffect");
  REQUIRE(s[1]["function"] == "Stop");
  REQUIRE(s[1]["selfId"] == kVanish);
  REQUIRE(s[1]["arguments"] == nlohmann::json::array({ Ref(kCaster) }));
  REQUIRE(s[2]["class"] == "VisualEffect");
  REQUIRE(s[2]["function"] == "Play");
  REQUIRE(s[2]["selfId"] == kAppear);
  REQUIRE(s[2]["arguments"] ==
          nlohmann::json::array({ Ref(kCaster), 1.75, Ref(0) }));
  REQUIRE(s[3]["class"] == "Sound");
  REQUIRE(s[3]["function"] == "Play");
  REQUIRE(s[3]["selfId"] == kSound);
  REQUIRE(s[3]["arguments"] == nlohmann::json::array({ Ref(kCaster) }));
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

TEST_CASE("A return (reach 0, relayed) is taken back where it was cast, "
          "and its cast is relayed",
          "[CastTeleport][espm]")
{
  // thuum docs/verbs/ghostwalk.md: Apocalypse's Ghostwalk, 10 s invisible
  // and then the mod's script moves the caster back where it cast
  PartOne& p = GetPartOne();
  p.SetCastTeleportSettings(
    R"([{"spell": "12fd0:Skyrim.esm", "reach": 0, "waitMs": 15400, )"
    R"("relay": true}])");
  REQUIRE(p.TeleportReach(kFirebolt) == Catch::Approx(0.f));
  REQUIRE(p.TeleportWaitMs(kFirebolt) == 15400);
  REQUIRE(p.TeleportRelays(kFirebolt));
  auto& caster = Pair(p);
  p.Messages().clear();

  Cast(p);
  REQUIRE(Relayed(p));

  // walked 1500 units east: the ground budget's burst (2048) takes it
  Report(p, caster, { 1500, 0, 0 });
  REQUIRE(SentBack(p) == 0);
  // back to the cast point: past what the burst has left, the permit's
  Report(p, caster, { 0, 0, 0 });
  REQUIRE(caster.GetPos() == NiPoint3{ 0, 0, 0 });
  REQUIRE(SentBack(p) == 0);

  // that was the one return: the next jump is sent back
  Report(p, caster, { 1500, 0, 0 });
  REQUIRE(SentBack(p) == 1);
  REQUIRE(caster.GetPos() == NiPoint3{ 0, 0, 0 });
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

TEST_CASE("A cast teleport's look plays its vanish on the caster's figure in "
          "the other games, and nothing in the caster's",
          "[CastTeleport][espm]")
{
  PartOne& p = GetPartOne();
  p.SetCastTeleportSettings(kLooked);
  const auto look = p.GetTeleportLook(kFirebolt);
  REQUIRE(look.has_value());
  REQUIRE(look->vanish == kVanish);
  REQUIRE(look->appear == kAppear);
  REQUIRE(look->appearSeconds == Catch::Approx(1.75f));
  REQUIRE(look->sound == kSound);
  Pair(p);
  p.Tick();
  p.Messages().clear();

  Cast(p);
  REQUIRE(!Relayed(p));
  const auto vanish = LookSnippets(p, 1);
  REQUIRE(vanish.size() == 1);
  REQUIRE(vanish[0]["class"] == "VisualEffect");
  REQUIRE(vanish[0]["function"] == "Play");
  REQUIRE(vanish[0]["selfId"] == kVanish);
  // on the figure for the jump's wait (the default 5 s), facing nothing
  REQUIRE(vanish[0]["arguments"] ==
          nlohmann::json::array({ Ref(kCaster), 5.0, Ref(0) }));
  REQUIRE(LookSnippets(p, 0).empty());
  Leave(p);
}

TEST_CASE("A cast teleport's landing places the figure and plays the look's "
          "appear and sound in the other games, once",
          "[CastTeleport][espm]")
{
  PartOne& p = GetPartOne();
  p.SetCastTeleportSettings(kLooked);
  auto& caster = Pair(p);
  Cast(p);
  p.Tick();
  p.Messages().clear();

  // 2500 units east: past the ground budget's burst, the permitted jump
  Report(p, caster, { 2500, 0, 0 });
  REQUIRE(SentBack(p) == 0);
  REQUIRE(caster.GetPos() == NiPoint3{ 2500, 0, 0 });
  RequireLanding(LookSnippets(p, 1), { 2500, 0, 0 });
  REQUIRE(LookSnippets(p, 0).empty());

  // the look is used: another step no run makes plays nothing
  p.Messages().clear();
  Report(p, caster, { 3100, 0, 0 });
  REQUIRE(SentBack(p) == 0);
  REQUIRE(LookSnippets(p, 1).empty());
  Leave(p);
}

// thuum docs/verbs/cast-teleport.md (the teleport snap, train-27): a landing
// the server vouches for reaches the other game as a placement (MsgType 46)
// before the report's relay, so the figure is placed, not glided
TEST_CASE("A cast teleport's landing tells the other game to place the figure "
          "before the report's relay",
          "[CastTeleport][espm]")
{
  PartOne& p = GetPartOne();
  p.SetCastTeleportSettings(kLooked);
  auto& caster = Pair(p);
  Cast(p);
  p.Tick();
  p.Messages().clear();

  Report(p, caster, { 2500, 0, 0 });
  REQUIRE(SentBack(p) == 0);
  int jumpAt = -1;
  int relayAt = -1;
  int i = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == 1 && m.j.contains("t")) {
      if (m.j["t"] == static_cast<int>(MsgType::ActorJump) && jumpAt < 0) {
        REQUIRE(m.reliable);
        REQUIRE(m.j["idx"].get<uint32_t>() == caster.idx);
        jumpAt = i;
      }
      if (m.j["t"] == static_cast<int>(MsgType::UpdateMovement) &&
          relayAt < 0) {
        relayAt = i;
      }
    }
    ++i;
  }
  REQUIRE(jumpAt >= 0);
  REQUIRE(relayAt >= 0);
  REQUIRE(jumpAt < relayAt);
  // the caster's own game is told nothing
  for (auto& m : p.Messages()) {
    REQUIRE_FALSE(m.userId == 0 && m.j.contains("t") &&
                  m.j["t"] == static_cast<int>(MsgType::ActorJump));
  }

  // a plain step after it: no placement
  p.Messages().clear();
  Report(p, caster, { 2600, 0, 0 });
  for (auto& m : p.Messages()) {
    REQUIRE_FALSE(m.j.contains("t") &&
                  m.j["t"] == static_cast<int>(MsgType::ActorJump));
  }
  Leave(p);
}

TEST_CASE("A cast teleport's landing the ground budget takes lands its look",
          "[CastTeleport][espm]")
{
  // most landings (b-teleport's 550 units, every one of playtest fourteen)
  // stay within the budget's burst and never reach the jump's permit
  PartOne& p = GetPartOne();
  p.SetCastTeleportSettings(kLooked);
  auto& caster = Pair(p);
  Cast(p);
  p.Tick();
  p.Messages().clear();

  // a step a run makes between two reports is no landing
  Report(p, caster, { 50, 0, 0 });
  REQUIRE(LookSnippets(p, 1).empty());
  // 550 units on: the budget takes it, and the look lands
  Report(p, caster, { 600, 0, 0 });
  REQUIRE(SentBack(p) == 0);
  REQUIRE(caster.GetPos() == NiPoint3{ 600, 0, 0 });
  RequireLanding(LookSnippets(p, 1), { 600, 0, 0 });
  REQUIRE(LookSnippets(p, 0).empty());
  Leave(p);
}

TEST_CASE("A cast teleport whose entry names no look sends no look's snippet",
          "[CastTeleport][espm]")
{
  PartOne& p = GetPartOne();
  p.SetCastTeleportSettings(
    R"([{"spell": "12fd0:Skyrim.esm", "reach": 3000}])");
  REQUIRE(!p.GetTeleportLook(kFirebolt));
  auto& caster = Pair(p);
  p.Tick();
  p.Messages().clear();

  Cast(p);
  Report(p, caster, { 600, 0, 0 });
  Report(p, caster, { 3100, 0, 0 });
  REQUIRE(SentBack(p) == 0);
  REQUIRE(caster.GetPos() == NiPoint3{ 3100, 0, 0 });
  REQUIRE(LookSnippets(p, 1).empty());
  REQUIRE(LookSnippets(p, 0).empty());
  Leave(p);
}
