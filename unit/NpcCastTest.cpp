#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "CellLoadedMessage.h"
#include "HitMessage.h"
#include "MsgType.h"
#include "PlayerBowShotMessage.h"
#include "SpellCastMessage.h"
#include "TestUtils.hpp"
#include "UpdateEquipmentMessage.h"
#include "libespm/Loader.h"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>

PartOne& GetPartOne();

// thuum docs/verbs/npc-stream.md (ADR-050, train 53): a hosted NPC's spells.
// Its host's game reports its hands, its casts and its hits under the NPC's
// own id. The server records what it cannot know (the engine's spell list:
// templates, leveled lists; R2, rule 5) and validates what it can (the cast
// behind a hit, R1, as a player's). The forest's leveled predator
// (NpcFateTest's id) stands in Tamriel's cell 0x9810; the shared world is
// left as found (the fixture persists actors across cases).

namespace {
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kHost = 0xff000d20;
constexpr uint32_t kOther = 0xff000d21;
constexpr uint32_t kPredator = 0x85547;
constexpr uint32_t kForestCell = 0x9810;
// Skyrim.esm (SpellCastTest's ids): Firebolt, fire-and-forget, 25 against a
// recorded 250 leaves 0.9; dunHonningbrewSkeeverPoison, delivered by touch
constexpr uint32_t kFirebolt = 0x00012fd0;
constexpr uint32_t kTouch = 0x0010fc22;
constexpr uint32_t kIronSword = 0x00012eb7;
// MarksmanTest's: LongBow 0x3B562, IronArrow 0x1397D
constexpr uint32_t kLongBow = 0x0003b562;
constexpr uint32_t kIronArrow = 0x0001397d;
const NiPoint3 kForest{ 123000.f, -80000.f, 10705.f };

const auto kWorn = [] {
  Inventory::ExtraData extra;
  extra.worn_ = true;
  return extra;
}();

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// `user` arrives at the forest as `id`; the fate pass loads the ring
MpActor& Arrive(PartOne& p, Networking::UserId user, uint32_t id)
{
  DoConnect(p, user);
  p.CreateActor(id, kForest, 0, kTamriel);
  p.SetUserActor(user, id);
  p.TickCellHosts(NowMs());
  return p.worldState.GetFormAt<MpActor>(id);
}

// the predator, alive and with no deadline, as the earlier cases may have
// left it
MpActor& Standing(PartOne& p, uint32_t refrId)
{
  auto& form = p.worldState.LookupFormByIdNoLoad(refrId);
  REQUIRE(form);
  MpActor* actor = form->AsActor();
  REQUIRE(actor);
  if (actor->IsDead()) {
    actor->Respawn(false);
  }
  p.worldState.fateRespawnDue.erase(refrId);
  return *actor;
}

// `user` hosts `cell`: any earlier host left at its case's end, so its grace
// passes first, then the loaded report grants the cell
void Host(PartOne& p, Networking::UserId user, uint32_t cell)
{
  const auto past = NowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  RawMessageData raw;
  raw.userId = user;
  CellLoadedMessage loaded;
  loaded.cell = cell;
  loaded.loaded = true;
  p.GetActionListener().OnCellLoaded(raw, loaded);
  p.TickCellHosts(past);
}

// the host (user 0) and the other player (user 1) at the forest, the host
// granted the cell, the other with a recorded base health of 250, the
// predator standing with empty hands in its record
MpActor& Gather(PartOne& p)
{
  auto& host = Arrive(p, 0, kHost);
  auto& other = Arrive(p, 1, kOther);
  auto& predator = Standing(p, kPredator);
  // beside the other player (the same square), so a cast or a shot of its
  // can have reached it
  predator.SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  Host(p, 0, kForestCell);
  REQUIRE(p.worldState.HostOf(predator) == &host);
  ActorValueRecord record;
  record.bases = { { 24, 250.f } };
  REQUIRE(other.SetActorValueRecord(record));
  predator.SetEquipment(Equipment{});
  p.Messages().clear();
  return predator;
}

// the raw bytes a relayed report carries (SendToNeighbours forwards them to
// the listeners as they came, so the fixture reads them as the message's
// type; CellHostTest's Wear idiom with the type named)
uint8_t kUnparsed[] = {
  Networking::MinPacketId, '{', '"', 't', '"', ':', '5', '}'
};

RawMessageData HostRaw()
{
  RawMessageData raw;
  raw.userId = 0;
  raw.unparsed = kUnparsed;
  raw.unparsedLength = sizeof(kUnparsed);
  return raw;
}

// the host's report of the predator's hands: one spell in its right hand
void Hands(PartOne& p, MpActor& predator, uint32_t rightSpell,
           uint32_t leftSpell = 0)
{
  UpdateEquipmentMessage msg;
  msg.idx = predator.GetIdx();
  if (rightSpell) {
    msg.data.rightSpell = rightSpell;
  }
  if (leftSpell) {
    msg.data.leftSpell = leftSpell;
  }
  const RawMessageData raw = HostRaw();
  p.GetActionListener().OnUpdateEquipment(raw, msg);
}

// the host's report of the predator's cast at the other player, through the
// packet path (DoMessage), as its client's message arrives
void Cast(PartOne& p, uint32_t spell)
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", kPredator },
                        { "target", kOther },
                        { "spell", spell },
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

// the host's report of the predator's spell hit on the other player
void SpellHits(PartOne& p, uint32_t spell)
{
  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.aggressor = kPredator;
  hit.data.target = kOther;
  hit.data.source = spell;
  p.GetActionListener().OnHit(raw, hit);
}

// the messages of one type sent to `user` since the last clear
std::vector<nlohmann::json> SentTo(PartOne& p, Networking::UserId user,
                                   MsgType type)
{
  std::vector<nlohmann::json> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(type)) {
      out.push_back(m.j);
    }
  }
  return out;
}

float Health(PartOne& p)
{
  return p.worldState.GetFormAt<MpActor>(kOther)
    .GetChangeForm()
    .actorValues.healthPercentage;
}

void Leave(PartOne& p)
{
  p.worldState.GetFormAt<MpActor>(kPredator).SetEquipment(Equipment{});
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHost);
  p.DestroyActor(kOther);
}
}

TEST_CASE("A hosted actor's hands with a spell its known set lacks are "
          "recorded and relayed; a form that is no spell refuses the report",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& predator = Gather(p);

  // a wolf's record knows no Firebolt: the host's engine drew it from a list
  // the server does not walk, and the report stands (R2)
  Hands(p, predator, kFirebolt);
  REQUIRE(predator.GetEquipment().rightSpell.value_or(0) == kFirebolt);
  REQUIRE(SentTo(p, 1, MsgType::UpdateEquipment).size() == 1);

  // a sword is no spell: the report is refused whole, the record stands
  p.Messages().clear();
  Hands(p, predator, kFirebolt, kIronSword);
  REQUIRE(predator.GetEquipment().leftSpell.value_or(0) == 0);
  REQUIRE(predator.GetEquipment().rightSpell.value_or(0) == kFirebolt);
  REQUIRE(SentTo(p, 1, MsgType::UpdateEquipment).empty());
  Leave(p);
}

TEST_CASE("A hosted caster's cast is recorded under its own id and reaches "
          "the other game; its hit is claimed once against that cast",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& predator = Gather(p);
  Hands(p, predator, kFirebolt);
  p.Messages().clear();

  Cast(p, kFirebolt);
  const auto relays = SentTo(p, 1, MsgType::SpellCast);
  REQUIRE(relays.size() == 1);
  REQUIRE(relays[0]["data"]["caster"].get<uint32_t>() == kPredator);
  REQUIRE(relays[0]["data"]["target"].get<uint32_t>() == kOther);

  SpellHits(p, kFirebolt);
  REQUIRE(Health(p) == Catch::Approx(0.9f));
  // the same cast again: nothing (a fire-and-forget cast covers one hit)
  SpellHits(p, kFirebolt);
  REQUIRE(Health(p) == Catch::Approx(0.9f));
  Leave(p);
}

TEST_CASE("A hosted spell hit with no cast behind it is refused (rule 5)",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& predator = Gather(p);
  Hands(p, predator, kFirebolt);

  SpellHits(p, kFirebolt);
  REQUIRE(Health(p) == 1.f);
  Leave(p);
}

TEST_CASE("A hosted caster's spell its record's hands lack is recorded, not "
          "refused, and its hit lands as a cast that reaches its target",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  Gather(p);

  // no hands reported: the record's slots are empty, the cast still counts
  Cast(p, kFirebolt);
  REQUIRE(SentTo(p, 1, MsgType::SpellCast).size() == 1);
  SpellHits(p, kFirebolt);
  REQUIRE(Health(p) == Catch::Approx(0.9f));
  Leave(p);
}

TEST_CASE("A hosted caster's touch cast reaches the other game at its first "
          "hit, on the target that hit names",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& predator = Gather(p);
  Hands(p, predator, kTouch);
  p.Messages().clear();

  Cast(p, kTouch);
  REQUIRE(SentTo(p, 1, MsgType::SpellCast).empty());

  SpellHits(p, kTouch);
  const auto relays = SentTo(p, 1, MsgType::SpellCast);
  REQUIRE(relays.size() == 1);
  REQUIRE(relays[0]["data"]["caster"].get<uint32_t>() == kPredator);
  REQUIRE(relays[0]["data"]["target"].get<uint32_t>() == kOther);
  Leave(p);
}

TEST_CASE("A hosted archer's shot is reported by its host under the actor's "
          "id: it spends the record's arrow, reaches the other game as "
          "ArrowShot, and its hit is claimed once at the shot's power; a "
          "shot by a player who is not the host changes nothing",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& predator = Gather(p);
  predator.AddItem(kLongBow, 1);
  predator.AddItem(kIronArrow, 3);
  // the host's hands report: the bow in the record's hands, no ammo (the
  // quiver is the engine's)
  UpdateEquipmentMessage hands;
  hands.idx = predator.GetIdx();
  hands.data.inv.entries.push_back(Inventory::Entry(kLongBow, 1, kWorn));
  const RawMessageData raw = HostRaw();
  p.GetActionListener().OnUpdateEquipment(raw, hands);
  REQUIRE(predator.GetEquipment().inv.HasItem(kLongBow));
  p.Messages().clear();

  PlayerBowShotMessage shot;
  shot.idx = predator.GetIdx();
  shot.weaponId = kLongBow;
  shot.ammoId = kIronArrow;
  shot.power = 0.5f;
  shot.aimAngle = 0.f;
  shot.aimHeading = 1.5f;
  p.GetActionListener().OnPlayerBowShot(raw, shot);
  REQUIRE(predator.GetInventory().GetItemCount(kIronArrow) == 2);
  const auto relays = SentTo(p, 1, MsgType::ArrowShot);
  REQUIRE(relays.size() == 1);
  REQUIRE(relays[0]["idx"].get<uint32_t>() == predator.GetIdx());
  REQUIRE(relays[0]["weaponId"].get<uint32_t>() == kLongBow);
  REQUIRE(SentTo(p, 0, MsgType::ArrowShot).empty());

  // the hit at the shot's power: the fixture's formula gives 25, halved,
  // against the other player's recorded 250
  HitMessage hit;
  hit.data.aggressor = kPredator;
  hit.data.target = kOther;
  hit.data.source = kLongBow;
  p.GetActionListener().OnHit(raw, hit);
  REQUIRE(Health(p) == Catch::Approx(0.95f));
  // the same shot again: no shot behind it
  p.GetActionListener().OnHit(raw, hit);
  REQUIRE(Health(p) == Catch::Approx(0.95f));

  // the other player reports a shot for the actor it does not host
  p.Messages().clear();
  RawMessageData other;
  other.userId = 1;
  p.GetActionListener().OnPlayerBowShot(other, shot);
  REQUIRE(predator.GetInventory().GetItemCount(kIronArrow) == 2);
  REQUIRE(SentTo(p, 0, MsgType::ArrowShot).empty());
  predator.RemoveItem(kLongBow, 1, nullptr);
  predator.RemoveItem(kIronArrow, 2, nullptr);
  Leave(p);
}
