#include "ActionListener.h"
#include "ActorFateMessage.h"
#include "ActorValueRecord.h"
#include "CellLoadedMessage.h"
#include "ConsoleCommandMessage.h"
#include "HitMessage.h"
#include "MsgType.h"
#include "PlayerBowShotMessage.h"
#include "SpSnippet.h"
#include "SpellCastMessage.h"
#include "SummonMessage.h"
#include "TestUtils.hpp"
#include "UpdateEquipmentMessage.h"
#include "libespm/Loader.h"
#include "libespm/SPEL.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <fmt/format.h>
#include <spdlog/sinks/ostream_sink.h>
#include <sstream>

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
// two server-made NPCs with no user, bystanders of the relay case
constexpr uint32_t kBystanderA = 0xff000d22;
constexpr uint32_t kBystanderB = 0xff000d23;
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
// Skyrim.esm (lab/esm.py on sky-srv, 2026-10-29): EncAtronachFlame, whose
// race AtronachFlameRace 0x131f5 bears the one ability AbFlameAtronach
// 0x2f3b5; its effect AbAtronachCloakFire 0x64459 (archetype Cloak) reaches
// crAtronachCloakFlameDmg, a concentration spell of 10 a second
constexpr uint32_t kAtronach = 0x00023aa6;
constexpr uint32_t kAtronachCloakDmg = 0x0006445b;
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

// the raw bytes a relayed report carries: the message as the host's client
// would send it (SendToNeighbours forwards them as they came, and the
// fixture types the relay by them, so a bare type without its fields threw
// NO_SUCH_FIELD at the fixture's read); kept in a static for the call
RawMessageData HostRaw(const UpdateEquipmentMessage& msg)
{
  static std::string bytes;
  nlohmann::json j;
  msg.WriteJson(j);
  bytes.clear();
  bytes.push_back(static_cast<char>(Networking::MinPacketId));
  bytes += j.dump();
  RawMessageData raw;
  raw.userId = 0;
  raw.unparsed = reinterpret_cast<Networking::PacketData>(bytes.data());
  raw.unparsedLength = bytes.size();
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
  const RawMessageData raw = HostRaw(msg);
  p.GetActionListener().OnUpdateEquipment(raw, msg);
}

// the host's report of the predator's (or `caster`'s) cast at the other
// player, through the packet path (DoMessage), as its client's message
// arrives
void Cast(PartOne& p, uint32_t spell, uint32_t caster = kPredator)
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", caster },
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

// the host's report of the predator's (or `aggressor`'s) spell hit on the
// other player (or `target`)
// the default logger's lines at info and above while the guard lives
// (MarksmanTest's shape), restored at its end
struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("npc-cast-test", sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
  }
  ~LogCapture() { spdlog::set_default_logger(old); }
  size_t Count(const std::string& line) const
  {
    const std::string log = oss.str();
    size_t n = 0;
    for (size_t at = log.find(line); at != std::string::npos;
         at = log.find(line, at + line.size())) {
      ++n;
    }
    return n;
  }
};

void SpellHits(PartOne& p, uint32_t spell, uint32_t aggressor = kPredator,
               uint32_t target = kOther)
{
  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.aggressor = aggressor;
  hit.data.target = target;
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

float Health(PartOne& p, uint32_t actor = kOther)
{
  return p.worldState.GetFormAt<MpActor>(actor)
    .GetChangeForm()
    .actorValues.healthPercentage;
}

void Leave(PartOne& p)
{
  p.worldState.GetFormAt<MpActor>(kPredator).SetEquipment(Equipment{});
  // a hosted actor's hit on the other player begins a fight since
  // 2026-10-28 (hostility.rs `counted`); the pair would outlive the actor
  // in the shared world until a TickFights, so it ends here
  p.GetFights().forget(kOther);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHost);
  p.DestroyActor(kOther);
}

// the host places `base` at its own actor through the server console
// (NpcFateTest's placed-actor idiom); the placed actor is named by the one
// fate with that base under a 0xff id
uint32_t Place(PartOne& p, MpActor& host, uint32_t base)
{
  host.SetConsoleCommandsAllowedFlag(true);
  p.Messages().clear();
  RawMessageData raw;
  raw.userId = 0;
  ConsoleCommandMessage cmd;
  cmd.data.commandName = "placeatme";
  cmd.data.args = { int64_t(0x14), int64_t(base) };
  p.GetActionListener().OnConsoleCommand(raw, cmd);
  uint32_t placedId = 0;
  for (auto& m : p.Messages()) {
    if (auto fate = dynamic_cast<ActorFateMessage*>(m.message.get())) {
      for (const auto& f : fate->fates) {
        if (f.refrId >= 0xff000000 && f.baseId == base) {
          placedId = f.refrId;
        }
      }
    }
  }
  REQUIRE(placedId >= 0xff000000);
  return placedId;
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

// thuum docs/verbs/npc-stream.md (train 53; playtest 37's flame atronach,
// 2026-10-29): a creature's cloak is a constant-effect ability of its race,
// never cast, so no cast of its lingered for the reach rule and every cloak
// hit was refused E_SPELL_NO_CAST ("which it holds not"). Its damage effect,
// MGEF crFireDamageConcAimed 0x10fdd1, carries one CTDA, function 719
// IsHostileToActor (H-53h), which the server answers from ADR-023's fights
// table; that table held players alone until 2026-10-28, so the cloak burnt
// no player server-side however long the two fought (hostility.rs `counted`)
TEST_CASE("A hosted creature's cloak hit is claimed against the ability its "
          "record holds, which stands as its cast; its damage waits on a "
          "fight between the two (IsHostileToActor) and lands once a hit of "
          "the creature's began one; a spell no ability of its reaches "
          "stays refused",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  Gather(p);
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  const uint32_t atronachId = Place(p, host, kAtronach);
  auto& atronach = p.worldState.GetFormAt<MpActor>(atronachId);
  REQUIRE(atronach.GetBaseId() == kAtronach);
  REQUIRE(p.worldState.HostOf(atronach) == &host);
  // the race's ability is in the record's own spell set
  const auto known = atronach.KnownBaseSpells();
  REQUIRE(std::find(known.begin(), known.end(), 0x2f3b5u) != known.end());
  REQUIRE(p.AbilityReaching(atronach, kAtronachCloakDmg) == 0x2f3b5u);
  REQUIRE_FALSE(p.AbilityReaching(atronach, kFirebolt).has_value());
  // beside both players, a stranger to both
  atronach.SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  p.Messages().clear();
  REQUIRE_FALSE(p.GetFights().in_fight(atronachId));
  REQUIRE(Health(p) == 1.f);

  // the cloak's spell with no cast of the atronach's, on its own host, who
  // stands in the cloak and never fought it (playtest 37's seat): the
  // ability stands as the cast and the hit is claimed, and the effect's
  // IsHostileToActor refuses it between strangers, the engine's own rule,
  // once in ApplyEffects and once in OnSpellHit's hostility pass, the
  // function known to the server. The host takes this hit and not the other
  // player because a claim writes its cadence slot on its target even when
  // the conditions then refuse it, and the other player's first report
  // below is to count a whole cadence
  const float hostHealthBefore = Health(p, kHost);
  {
    LogCapture log;
    SpellHits(p, kAtronachCloakDmg, atronachId, kHost);
    REQUIRE(log.Count("ability cloak: ") == 1);
    REQUIRE(log.Count("claimed as a spell its cast reaches") == 1);
    REQUIRE(log.Count("E_SPELL_NO_CAST") == 0);
    REQUIRE(log.Count("its conditions fail; not applied") == 2);
    REQUIRE(log.Count("E_CONDITION_UNKNOWN") == 0);
  }
  REQUIRE(Health(p, kHost) == Catch::Approx(hostHealthBefore));
  REQUIRE_FALSE(p.GetFights().in_fight(atronachId));
  // a spell no ability of the record reaches, with no cast behind it: refused
  // (rule 5)
  {
    LogCapture log;
    SpellHits(p, kFirebolt, atronachId);
    REQUIRE(log.Count("E_SPELL_NO_CAST") == 1);
    REQUIRE(log.Count("ability cloak: ") == 0);
  }
  REQUIRE(Health(p) == 1.f);

  // the atronach's Firebolt, cast and claimed as the predator's is: 25 of
  // the recorded 250, and a hosted creature's hit on a player begins a fight
  // as a player's does, with NO StartCombat notice (the notice stays between
  // players: the victim's copy of the creature is AI-off, the creature's own
  // host engine fights by itself, the server's rest refusal is what the
  // victim keeps); the pair outlives a TickFights, which admits a member
  // with a host
  p.Messages().clear();
  Cast(p, kFirebolt, atronachId);
  SpellHits(p, kFirebolt, atronachId);
  REQUIRE(Health(p) == Catch::Approx(0.9f));
  REQUIRE(p.GetFights().between(atronachId, kOther));
  REQUIRE_FALSE(p.GetFights().between(atronachId, kHost));
  p.Tick(); // snippets are deferred
  size_t startCombats = 0;
  for (const auto& j : SentTo(p, 1, MsgType::SpSnippet)) {
    startCombats += j.value("function", std::string{}) == "StartCombat";
  }
  REQUIRE(startCombats == 0);
  p.TickFights(NowMs());
  REQUIRE(p.GetFights().between(atronachId, kOther));

  // in the fight, the cloak's first report on the other player holds its
  // conditions and counts one cadence: 0.5 s (wire-rules casts
  // REACHED_TICK_MS) of 10 a second, 5 of 250
  {
    LogCapture log;
    SpellHits(p, kAtronachCloakDmg, atronachId);
    REQUIRE(log.Count("claimed as a spell its cast reaches") == 1);
    REQUIRE(log.Count("its conditions fail; not applied") == 0);
    REQUIRE(log.Count("E_CONDITION_UNKNOWN") == 0);
  }
  REQUIRE(Health(p) == Catch::Approx(0.88f));

  p.DestroyActor(atronachId);
  Leave(p);
}

TEST_CASE("A hosted archer's shot is reported by its host under the actor's "
          "id: it spends no record arrow (the engine spends none of an "
          "NPC's), reaches the other game as ArrowShot, and its hit is "
          "claimed once at the shot's power; a shot by a player who is not "
          "the host changes nothing",
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
  RawMessageData raw = HostRaw(hands);
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
  {
    LogCapture log;
    p.GetActionListener().OnPlayerBowShot(raw, shot);
    REQUIRE(log.Count("E_SHOT_QUIVER") == 1);
  }
  // the quiver as the engine keeps it: three before, three after (the
  // archer's body loots what the host's game shows, x-fate-inventory
  // 20261104-201910's refused take of 16 against a record of 15)
  REQUIRE(predator.GetInventory().GetItemCount(kIronArrow) == 3);
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
  REQUIRE(predator.GetInventory().GetItemCount(kIronArrow) == 3);
  REQUIRE(SentTo(p, 0, MsgType::ArrowShot).empty());
  predator.RemoveItem(kLongBow, 1, nullptr);
  predator.RemoveItem(kIronArrow, 3, nullptr);
  Leave(p);
}

// playtest 37, finding 53 (2026-10-28): the flame atronach on seat c1 fired
// ten to thirteen bolts a cast on its host's own screen. RelayCast sent one
// SpellCast per listener through GetActorToSendTo, which hands an offline
// listener's copy (an NPC in reach) to its cell's host, the very game that
// reported the cast, and the host's game cast every copy on its live
// atronach. A relay is for games: a listener with no user is skipped, as
// UpdateHoster, CastBondProc and PlayTeleportVanish skip it.
TEST_CASE("A hosted caster's cast relay reaches each other game once and "
          "never its own host's, whatever offline NPCs stand in reach",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& predator = Gather(p);
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  Hands(p, predator, kFirebolt);

  // two server-made NPCs beside the predator, in the host's cell and with no
  // user, so listeners of the predator's: a server-made actor joins the grid
  // at its first SetPos (CellHostTest's wolf), so the position is written
  // once after the creation and the premise is asserted before the cast
  for (uint32_t id : { kBystanderA, kBystanderB }) {
    p.CreateActor(id, kForest + NiPoint3{ 200.f, 0.f, 0.f }, 0, kTamriel);
    auto& bystander = p.worldState.GetFormAt<MpActor>(id);
    bystander.SetPos(bystander.GetPos());
    REQUIRE(bystander.GetUserId() == Networking::InvalidUserId);
    REQUIRE(p.worldState.HostOf(bystander) == &host);
    const auto& listeners = predator.GetActorListeners();
    REQUIRE(std::find(listeners.begin(), listeners.end(), &bystander) !=
            listeners.end());
  }
  p.Messages().clear();

  Cast(p, kFirebolt);
  // the other game hears the cast once; the host's game, which reported it,
  // not at all (it heard one copy per bystander before the fix)
  const auto relays = SentTo(p, 1, MsgType::SpellCast);
  REQUIRE(relays.size() == 1);
  REQUIRE(relays[0]["data"]["caster"].get<uint32_t>() == kPredator);
  REQUIRE(relays[0]["data"]["target"].get<uint32_t>() == kOther);
  REQUIRE(SentTo(p, 0, MsgType::SpellCast).empty());

  p.DestroyActor(kBystanderA);
  p.DestroyActor(kBystanderB);
  Leave(p);
}

// playtest 53's #112 (2026-11-17 00:12:34Z, server.log :2197 to :2206):
// Ariadne's two sword hits on rotfern's Reanimate Corpse thrall began a fight
// with no notice ("no notice, the pair is not two players"), and rotfern's
// engine, which runs the thrall, never saw a hit: it landed on Ariadne's own
// copy, a puppet. Eli: "doesn't respond to hits from other player but does
// aggro once someone assaults the owner". The host's game is told now
// (ActionListener::NotifyHostility): its NPC starts combat with its figure of
// the attacker, once a fight, as a victim player's game is (ADR-023)
TEST_CASE("A player's hit on an NPC another game hosts tells the host's game "
          "once a fight to start its NPC's combat with the attacker; the "
          "host's own hit tells nothing",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& predator = Gather(p);
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  auto& other = p.worldState.GetFormAt<MpActor>(kOther);
  // in front of both players, who stand at the forest's spot facing north
  // (HitTest's fight's frame), within a sword's reach
  predator.SetPos(kForest + NiPoint3{ 0.f, 100.f, 0.f });
  for (MpActor* player : { &host, &other }) {
    player->AddItem(kIronSword, 1);
    Equipment eq;
    eq.inv.entries.push_back(Inventory::Entry(kIronSword, 1, kWorn));
    player->SetEquipment(eq);
  }
  // `user`'s sword hit on the predator at full health, its cooldown past
  const auto swings = [&](Networking::UserId user, MpActor& player) {
    predator.SetPercentages(ActorValues{});
    player.SetLastHitTime(
      kPredator, std::chrono::steady_clock::now() - std::chrono::seconds(3));
    RawMessageData raw;
    raw.userId = user;
    HitMessage hit;
    hit.data.aggressor = 0x14;
    hit.data.target = kPredator;
    hit.data.source = kIronSword;
    p.GetActionListener().OnHit(raw, hit);
    p.Tick(); // snippets are deferred
  };
  // the StartCombat snippets `user` received since the last clear
  const auto startCombatsTo = [&](Networking::UserId user) {
    std::vector<nlohmann::json> out;
    for (const auto& j : SentTo(p, user, MsgType::SpSnippet)) {
      if (j.value("function", std::string{}) == "StartCombat") {
        out.push_back(j);
      }
    }
    return out;
  };

  // the other player's hit begins the fight: the host's game alone is told,
  // the predator its self (a game-file actor's long id) and the attacker's
  // server id its argument, which the host's client maps to its figure
  p.Messages().clear();
  {
    LogCapture log;
    swings(1, other);
    REQUIRE(log.Count("hit 85547, a fight begins; its host ff000d20's game "
                      "is told") == 1);
  }
  // D2 (pt53-112c): both ways, the predator aimed at the attacker's figure
  // and the figure (the attacker's server id its self) aimed at the predator
  const auto told = startCombatsTo(0);
  REQUIRE(told.size() == 2);
  REQUIRE(told[0]["class"] == "Actor");
  REQUIRE(told[0]["selfId"] ==
          SpSnippet::MakeLongFormId(&p.worldState, kPredator));
  REQUIRE(told[0]["arguments"] ==
          nlohmann::json::array(
            { nlohmann::json{ { "formId", kOther }, { "type", "Actor" } } }));
  REQUIRE(told[1]["class"] == "Actor");
  REQUIRE(told[1]["selfId"] == kOther);
  REQUIRE(told[1]["arguments"] ==
          nlohmann::json::array({ nlohmann::json{ { "formId", kPredator },
                                                  { "type", "Actor" } } }));
  REQUIRE(startCombatsTo(1).empty());
  REQUIRE(p.GetFights().between(kOther, kPredator));

  // D2: the fight's end, a minute quiet, reaches the host's game both ways:
  // StopCombatAlarm on its figure of the attacker, StopCombat on the
  // predator; the attacker's own game hears the players' end as before
  p.Messages().clear();
  {
    LogCapture log;
    p.TickFights(NowMs() + 61'000);
    REQUIRE(log.Count("85547's host ff000d20 is told the end of its fight "
                      "with ff000d21 (D2)") == 1);
  }
  p.Tick(); // snippets are deferred
  const auto endsTo = [&](Networking::UserId user, const char* function) {
    std::vector<nlohmann::json> out;
    for (const auto& j : SentTo(p, user, MsgType::SpSnippet)) {
      if (j.value("function", std::string{}) == function) {
        out.push_back(j);
      }
    }
    return out;
  };
  const auto alarms = endsTo(0, "StopCombatAlarm");
  REQUIRE(alarms.size() == 1);
  REQUIRE(alarms[0]["selfId"] == kOther);
  const auto stops = endsTo(0, "StopCombat");
  REQUIRE(stops.size() == 1);
  REQUIRE(stops[0]["selfId"] ==
          SpSnippet::MakeLongFormId(&p.worldState, kPredator));
  REQUIRE_FALSE(p.GetFights().between(kOther, kPredator));

  // a new fight, then its next hit tells nothing
  swings(1, other);
  p.Messages().clear();
  swings(1, other);
  REQUIRE(startCombatsTo(0).empty());
  REQUIRE(startCombatsTo(1).empty());

  // the host's own hit on its NPC is its engine's already
  p.GetFights().forget(kOther);
  p.Messages().clear();
  {
    LogCapture log;
    swings(0, host);
    REQUIRE(log.Count("no notice, its host's own engine saw the hit") == 1);
  }
  REQUIRE(startCombatsTo(0).empty());
  REQUIRE(startCombatsTo(1).empty());

  predator.SetPercentages(ActorValues{});
  p.GetFights().forget(kHost);
  Leave(p);
}

// thuum tracker #115 (playtest 53, Eli: "atronochs were shooting two
// projectiles"): an aimed fire-and-forget spell's relay flies its bolt at the
// victim in the victim's own game, which is the landing there, so the server
// serves that game no DoCombatSpellApply (the engine launched a second bolt
// from the caster's copy for it: playtest 53's c2-console.log:11154-11214
// against server.log:3366-3378); a stream's relayed missiles follow the
// copy's aim, so a stream keeps the delivery (playtest 51's drain)
TEST_CASE("A hosted caster's aimed bolt that hits a player gets no landing "
          "delivery in that player's game; a stream is never skipped",
          "[NpcCast][espm]")
{
  // Skyrim.esm (lab/esm.py on sky-srv, 2026-11-16): crAtronachFlameFirebolt,
  // the playtest's own bolt, SPIT fire-and-forget and aimed, its one effect
  // FireDamageFFAimed 0x12f03. Firebolt 0x12fd0 also carries
  // PerkImpactStaggerPushFFAimed 0x153d3 and PerkIntenseFlamesConfDownFFAimed
  // 0xf392d, which move no server value, so OnSpellHit never calls the
  // delivery for it (MovesServerValue over every effect): m4-turbo-53's
  // first T0 (pipeline 6186) read 0 skip lines with it
  constexpr uint32_t kAtronachBolt = 0x0007e882;
  // FlamesRightHand, SPIT concentration and aimed, one effect 0x13ca9
  constexpr uint32_t kFlames = 0x000c969a;
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& predator = Gather(p);
  // the record itself: the atronach's bolt is aimed and fire-and-forget,
  // Flames a stream
  const auto bolt = espm::GetData<espm::SPEL>(kAtronachBolt, &p.worldState);
  REQUIRE(bolt.spellItem);
  REQUIRE(bolt.spellItem->castType == espm::SPEL::CastType::FireAndForget);
  REQUIRE(bolt.spellItem->delivery == espm::SPEL::Delivery::Aimed);
  const auto flames = espm::GetData<espm::SPEL>(kFlames, &p.worldState);
  REQUIRE(flames.spellItem);
  REQUIRE(flames.spellItem->castType == espm::SPEL::CastType::Concentration);
  REQUIRE(flames.spellItem->delivery == espm::SPEL::Delivery::Aimed);
  // the landing snippets served to `user` since the last clear
  const auto landings = [&](Networking::UserId user) {
    size_t n = 0;
    for (auto& m : p.Messages()) {
      if (m.userId == user && m.j.is_object() &&
          m.j.value("t", -1) == static_cast<int>(MsgType::SpSnippet) &&
          m.j.value("class", std::string()) == "Actor" &&
          m.j.value("function", std::string()) == "DoCombatSpellApply") {
        ++n;
      }
    }
    return n;
  };

  SECTION("the bolt: its hit applies the record's numbers and serves no "
          "landing")
  {
    Hands(p, predator, kAtronachBolt);
    Cast(p, kAtronachBolt);
    p.Messages().clear();
    LogCapture log;
    SpellHits(p, kAtronachBolt);
    INFO(log.oss.str());
    REQUIRE(Health(p) < 1.f);
    REQUIRE(landings(1) == 0);
    REQUIRE(landings(0) == 0);
    REQUIRE(log.Count("85547's 7e882 landed on ff000d21: no delivery, its "
                      "relayed bolt is the landing in that game (tracker "
                      "#115)") == 1);
  }

  // the stream's own delivery is the playtest 51 rule's, unchanged here;
  // pipeline 6186's T0 read no landing for a hit just after the cast, which
  // this case does not explain (its captured lines below say why on a red),
  // so it claims only #115's part: the skip never takes a stream
  SECTION("the stream: #115's skip never takes it")
  {
    Hands(p, predator, kFlames);
    Cast(p, kFlames);
    p.Messages().clear();
    LogCapture log;
    SpellHits(p, kFlames);
    INFO(log.oss.str());
    REQUIRE(log.Count("c969a landed on ff000d21: no delivery") == 0);
    REQUIRE(landings(0) == 0);
  }
  Leave(p);
}

namespace {
// playtest 54 #126's cases: the predator commanded by the host (the raise
// as OnHit stamps it ahead of ClaimReanimated, CellHostTest's shape), in
// front of both players within a sword's reach, each player with a sword
MpActor& HostsThrall(PartOne& p)
{
  auto& predator = Gather(p);
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  auto& other = p.worldState.GetFormAt<MpActor>(kOther);
  predator.SetPos(kForest + NiPoint3{ 0.f, 100.f, 0.f });
  for (MpActor* player : { &host, &other }) {
    player->AddItem(kIronSword, 1);
    Equipment eq;
    eq.inv.entries.push_back(Inventory::Entry(kIronSword, 1, kWorn));
    player->SetEquipment(eq);
  }
  predator.NoteReanimateAccepted(std::chrono::steady_clock::now(), 0, kHost);
  REQUIRE(p.ClaimCommanded(host, predator, 60) ==
          PartOne::ClaimVerdict::Granted);
  REQUIRE(p.CommanderOf(predator) == kHost);
  REQUIRE(p.worldState.HostOf(predator) == &host);
  p.Messages().clear();
  return predator;
}

// the snippets of `function` sent to `user` since the last clear
std::vector<nlohmann::json> SnippetsTo(PartOne& p, Networking::UserId user,
                                       const char* function)
{
  std::vector<nlohmann::json> out;
  for (const auto& j : SentTo(p, user, MsgType::SpSnippet)) {
    if (j.value("function", std::string{}) == function) {
      out.push_back(j);
    }
  }
  return out;
}

void LeaveThrall(PartOne& p, MpActor& predator)
{
  predator.SetHostedState(0, 0);
  predator.SetPercentages(ActorValues{});
  p.ReleaseAnyClaim(predator);
  p.GetFights().forget(kPredator);
  p.GetFights().forget(kHost);
  Leave(p);
}
}

// playtest 54 #126 (2026-11-17 07:41Z): Ariadne's second atronach lost its
// target in her game while its fight with rotfern stood, and rotfern's later
// hits reached her game as damage alone, the pair already existing. A hit on
// a summon or thrall whose host's state names no target tells the host the
// fight again, both ways, at most once a second
TEST_CASE("A hit on a thrall another game simulates, whose host's state "
          "names no target, tells its host the fight again both ways, at "
          "most once a second",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& predator = HostsThrall(p);
  auto& other = p.worldState.GetFormAt<MpActor>(kOther);
  const auto swings = [&] {
    predator.SetPercentages(ActorValues{});
    other.SetLastHitTime(
      kPredator, std::chrono::steady_clock::now() - std::chrono::seconds(3));
    RawMessageData raw;
    raw.userId = 1;
    HitMessage hit;
    hit.data.aggressor = 0x14;
    hit.data.target = kPredator;
    hit.data.source = kIronSword;
    p.GetActionListener().OnHit(raw, hit);
    p.Tick(); // snippets are deferred
  };

  // the fight begins: the host is told both ways (D2), and, the thrall its
  // own, it hears the players' notice as the owner too (tracker #160,
  // ADR-077; #160's thrall case below reads which is which). The players'
  // pair stands from that hit, so the case's first fights tick (a new
  // PartOne's first Tick runs one) joins the thrall to its commander's
  // fight (#113's design A), a StartCombat queued for the next Tick
  {
    LogCapture log;
    swings();
    INFO(log.oss.str());
    REQUIRE(log.Count("thrall 85547 of ff000d20 joins its commander's fight "
                      "with ff000d21") == 1);
  }
  REQUIRE(SnippetsTo(p, 0, "StartCombat").size() == 3);
  // the join, drained: one StartCombat on the thrall aimed at the other
  // player's figure, to the commander's game alone
  p.Messages().clear();
  p.Tick(); // snippets are deferred
  const auto joined = SnippetsTo(p, 0, "StartCombat");
  REQUIRE(joined.size() == 1);
  REQUIRE(joined[0]["selfId"] ==
          SpSnippet::MakeLongFormId(&p.worldState, kPredator));
  REQUIRE(joined[0]["arguments"] ==
          nlohmann::json::array(
            { nlohmann::json{ { "formId", kOther }, { "type", "Actor" } } }));
  REQUIRE(SnippetsTo(p, 1, "StartCombat").empty());

  // the host's state names the attacker: the next hit tells nothing
  predator.SetHostedState(kOther, 0);
  p.Messages().clear();
  swings();
  REQUIRE(SnippetsTo(p, 0, "StartCombat").empty());

  // its state names no target: told again, both ways, the same two
  predator.SetHostedState(0, 0);
  p.Messages().clear();
  {
    LogCapture log;
    swings();
    REQUIRE(log.Count("hit 85547 again and its host ff000d20's state names "
                      "no target: told again, both ways") == 1);
  }
  const auto again = SnippetsTo(p, 0, "StartCombat");
  REQUIRE(again.size() == 2);
  REQUIRE(again[0]["selfId"] ==
          SpSnippet::MakeLongFormId(&p.worldState, kPredator));
  REQUIRE(again[1]["selfId"] == kOther);
  REQUIRE(SnippetsTo(p, 1, "StartCombat").empty());

  // a second hit within the second: nothing
  p.Messages().clear();
  swings();
  REQUIRE(SnippetsTo(p, 0, "StartCombat").empty());

  LeaveThrall(p, predator);
}

namespace {
// thuum tracker #160's summon (SummonTest's forms, lab/esm.py on sky-srv,
// 2026-11-16): ConjureFlameAtronachRightHand 0x100e76, its potent effect
// MGEF 0x4e945 summoning NPC_ 0x4e940, the Potent Flame Atronach of
// playtest 55's two fights (the seats' own engine read of its name,
// trace-c1-2 and trace-c2-2); its engine's id, a runtime id as SummonTest's
constexpr uint32_t kConjure = 0x00100e76;
constexpr uint32_t kPotentEffect = 0x0004e945;
constexpr uint32_t kPotentAtronach = 0x0004e940;
constexpr uint32_t kSummonLocal = 0xff0008fd;

// Gather's two players, each with a sword (HostsThrall's arming)
void Armed(PartOne& p)
{
  Gather(p);
  for (uint32_t id : { kHost, kOther }) {
    auto& player = p.worldState.GetFormAt<MpActor>(id);
    player.AddItem(kIronSword, 1);
    Equipment eq;
    eq.inv.entries.push_back(Inventory::Entry(kIronSword, 1, kWorn));
    player.SetEquipment(eq);
  }
}

// the host's game (user 0, or `user`'s) reports the summon its engine placed
// in front of both players, within a sword's reach (the #112 case's spot):
// its own player's, or with `npc` that of an NPC its game simulates
// (OnSummon's casterId, tracker #106). The server's actor, read from the
// bound create word (SummonTest's idiom)
uint32_t Summoned(PartOne& p, uint32_t npc = 0, Networking::UserId user = 0)
{
  SummonMessage msg;
  msg.localId = kSummonLocal;
  msg.baseId = kPotentAtronach;
  msg.effectId = kPotentEffect;
  msg.spellId = kConjure;
  const NiPoint3 at = kForest + NiPoint3{ 0.f, 100.f, 0.f };
  msg.pos = { at.x, at.y, at.z };
  msg.rot = { 0.f, 0.f, 0.f };
  msg.worldOrCell = kTamriel;
  msg.remainingS = 59.5f;
  msg.ended = false;
  if (npc != 0) {
    msg.casterId = npc;
  }
  p.Messages().clear();
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnSummon(raw, msg);
  uint32_t summon = 0;
  for (const auto& j : SentTo(p, user, MsgType::CreateActor)) {
    if (j.contains("props") &&
        j["props"].value("ownLocal", 0u) == kSummonLocal) {
      summon = static_cast<uint32_t>(j.value("refrId", 0ull));
    }
  }
  REQUIRE(summon != 0);
  REQUIRE(p.worldState.HostOf(p.worldState.GetFormAt<MpActor>(summon)) ==
          &p.worldState.GetFormAt<MpActor>(user == 0 ? kHost : kOther));
  p.Messages().clear();
  return summon;
}

// `user`'s sword hit on `target` at full health, its cooldown past (the
// #126 cases' swing), then the deferred snippets
void Swings(PartOne& p, Networking::UserId user, uint32_t target)
{
  p.worldState.GetFormAt<MpActor>(target).SetPercentages(ActorValues{});
  p.worldState.GetFormAt<MpActor>(user == 0 ? kHost : kOther)
    .SetLastHitTime(
      target, std::chrono::steady_clock::now() - std::chrono::seconds(3));
  RawMessageData raw;
  raw.userId = user;
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = target;
  hit.data.source = kIronSword;
  p.GetActionListener().OnHit(raw, hit);
  p.Tick(); // snippets are deferred
}

// `user`'s game reports the bare-handed hit (Skyrim.esm's Unarmed 0x1f4,
// HitTest's) of `aggressor`, an actor its game simulates, on `target`: the
// target at full health, the cooldown past, then the deferred snippets. The
// predator is a forest animal (LvlAnimalForestPredator, NpcFateTest's) and
// the summon a flame atronach, so no humanoid reach or cone applies
// (ActionListener's HumanoidNpc)
void Strikes(PartOne& p, Networking::UserId user, uint32_t aggressor,
             uint32_t target)
{
  p.worldState.GetFormAt<MpActor>(target).SetPercentages(ActorValues{});
  p.worldState.GetFormAt<MpActor>(aggressor).SetLastHitTime(
    target, std::chrono::steady_clock::now() - std::chrono::seconds(3));
  RawMessageData raw;
  raw.userId = user;
  HitMessage hit;
  hit.data.aggressor = aggressor;
  hit.data.target = target;
  hit.data.source = 0x1f4;
  p.GetActionListener().OnHit(raw, hit);
  p.Tick(); // snippets are deferred
}

// the snippets of `function` on `selfId` (a server id, its long form for a
// game file's actor) `user`'s game got since the last clear
size_t SnippetsOn(PartOne& p, Networking::UserId user, const char* function,
                  uint32_t selfId)
{
  const uint64_t self = SpSnippet::MakeLongFormId(&p.worldState, selfId);
  const auto all = SnippetsTo(p, user, function);
  return static_cast<size_t>(
    std::count_if(all.begin(), all.end(), [&](const nlohmann::json& j) {
      return j.value("selfId", uint64_t{ 0 }) == self;
    }));
}

// the players' notices (ADR-023) `user`'s game got since the last clear:
// StartCombat on a figure, aimed at that game's own player (0x14)
std::vector<nlohmann::json> PlayersNoticesTo(PartOne& p,
                                             Networking::UserId user)
{
  const auto toPlayer = nlohmann::json::array(
    { nlohmann::json{ { "formId", 0x14 }, { "type", "Actor" } } });
  std::vector<nlohmann::json> out;
  for (const auto& j : SnippetsTo(p, user, "StartCombat")) {
    if (j.contains("arguments") && j["arguments"] == toPlayer) {
      out.push_back(j);
    }
  }
  return out;
}

// the line of a hit between the other player's side and the host's that
// begins the two players' fight (ActionListener's HitAcrossSides, thuum
// ADR-077 decision 4), and the words every such line carries
constexpr const char* kOwnerTold =
  "between the sides of ff000d21 and ff000d20: a fight between the two "
  "players begins, and both games are told (thuum ADR-077 decision 4";
constexpr const char* kSidesLine = "between the sides of";
}

// thuum tracker #160 (playtest 55, Eli: "summoner wasn't 'looped in' to
// aggro"; ADR-077 decision 2: "if someone attacks your summon they should
// become hostile to you, and the summon should attack"): at 2:37 PM PST
// rotfern hit Ariadne's atronach and at 2:39 PM the other way round, and
// each time the server began the pair with the summon alone (server.log
// :2707, :2912) and told the summoner's game only "the summon and the
// attacker fight", so the summoner's player stayed out of combat. A
// player's hit on another player's summon now begins a fight with its owner
// too (PartOne::OwnerOf), and the owner's game hears the players' notice:
// StartCombat on its figure of the attacker, aimed at its own player
TEST_CASE("A player's hit on another player's summon begins a fight with its "
          "owner too, whose game hears the players' notice once a fight; a "
          "hit on one's own summon or an NPC's tells no owner (tracker #160, "
          "ADR-077)",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  Armed(p);

  SECTION("another player's summon: the owner's game is told once, the "
          "pair recorded beside the summon's own")
  {
    const uint32_t summon = Summoned(p);
    REQUIRE(p.OwnerOf(p.worldState.GetFormAt<MpActor>(summon)) == kHost);
    {
      LogCapture log;
      Swings(p, 1, summon);
      INFO(log.oss.str());
      REQUIRE(log.Count(fmt::format("ff000d21 hit {:x}, {}", summon,
                                    kOwnerTold)) == 1);
      // the summon's own fight, as #112's D2 has it
      REQUIRE(log.Count(fmt::format("ff000d21 hit {:x}, a fight begins; its "
                                    "host ff000d20's game is told",
                                    summon)) == 1);
    }
    const auto notices = PlayersNoticesTo(p, 0);
    REQUIRE(notices.size() == 1);
    REQUIRE(notices[0]["class"] == "Actor");
    REQUIRE(notices[0]["selfId"] == kOther);
    // the notice and D2's two (the summon at the attacker's figure, the
    // figure at the summon); the attacker's game hears the players' notice
    // of the owner, its figure of the owner at its own player, since
    // neither engine saw a hit between the two players
    REQUIRE(SnippetsTo(p, 0, "StartCombat").size() == 3);
    const auto toAttacker = PlayersNoticesTo(p, 1);
    REQUIRE(toAttacker.size() == 1);
    REQUIRE(toAttacker[0]["selfId"] == kHost);
    REQUIRE(SnippetsTo(p, 1, "StartCombat").size() == 1);
    REQUIRE(p.GetFights().between(kOther, kHost));
    REQUIRE(p.GetFights().between(kOther, summon));

    // the next hit, both fights going on: no notice again
    p.Messages().clear();
    {
      LogCapture log;
      Swings(p, 1, summon);
      REQUIRE(log.Count(kOwnerTold) == 0);
    }
    REQUIRE(PlayersNoticesTo(p, 0).empty());
    REQUIRE(SnippetsTo(p, 1, "StartCombat").empty());
  }

  // the pair is hit on every hit on the summon, so it keeps the summon's
  // own quiet clock: an owner's fight 59 s quiet, which a TickFights 2 s on
  // would end (hostility.rs FIGHT_QUIET_MS), stands after a hit on the
  // summon, and that hit tells the owner nothing new
  SECTION("another player's summon in the owner's standing fight: the hit "
          "keeps the owner's pair, with no second notice")
  {
    const uint32_t summon = Summoned(p);
    REQUIRE(p.GetFights().hit(kOther, kHost, NowMs() - 59'000));
    Swings(p, 1, summon);
    REQUIRE(PlayersNoticesTo(p, 0).empty());
    REQUIRE(p.GetFights().between(kOther, summon));
    p.TickFights(NowMs() + 2'000);
    REQUIRE(p.GetFights().between(kOther, kHost));
  }

  // the fights table keeps no sides, so the owner's strike back, inside
  // the standing pair, tells nobody: the attacker's game heard its notice
  // of the owner at the summon's hit, or would never hear it
  SECTION("another player's summon, then its owner strikes the attacker: "
          "the attacker's game heard the owner's notice at the summon's hit, "
          "and the strike tells nothing new")
  {
    const uint32_t summon = Summoned(p);
    Swings(p, 1, summon);
    const auto toAttacker = PlayersNoticesTo(p, 1);
    REQUIRE(toAttacker.size() == 1);
    REQUIRE(toAttacker[0]["selfId"] == kHost);
    p.Messages().clear();
    {
      LogCapture log;
      Swings(p, 0, kOther);
      INFO(log.oss.str());
      REQUIRE(log.Count("a fight begins") == 0);
    }
    REQUIRE(PlayersNoticesTo(p, 0).empty());
    REQUIRE(PlayersNoticesTo(p, 1).empty());
    REQUIRE(p.GetFights().between(kOther, kHost));
  }

  // a dead summon is nobody's minion (PartOne::OwnerOf): a late swing on
  // the body begins no fight with its owner, whose entry lasts to the end
  // report or the backstop
  SECTION("a dead summon: no owner's, no fight with its owner")
  {
    const uint32_t summon = Summoned(p);
    auto& atronach = p.worldState.GetFormAt<MpActor>(summon);
    atronach.Kill(nullptr);
    REQUIRE(atronach.IsDead());
    REQUIRE(p.OwnerOf(atronach) == 0);
    {
      LogCapture log;
      Swings(p, 1, summon);
      INFO(log.oss.str());
      REQUIRE(log.Count(kSidesLine) == 0);
    }
    REQUIRE(PlayersNoticesTo(p, 0).empty());
    REQUIRE(PlayersNoticesTo(p, 1).empty());
    REQUIRE_FALSE(p.GetFights().between(kOther, kHost));
  }

  SECTION("one's own summon: its own engine's, no notice")
  {
    const uint32_t summon = Summoned(p);
    {
      LogCapture log;
      Swings(p, 0, summon);
      INFO(log.oss.str());
      REQUIRE(log.Count("no notice, its host's own engine saw the hit") == 1);
      REQUIRE(log.Count(kSidesLine) == 0);
    }
    REQUIRE(SnippetsTo(p, 0, "StartCombat").empty());
    REQUIRE(SnippetsTo(p, 1, "StartCombat").empty());
  }

  // tracker #106: the predator's summon, its host's to report and simulate;
  // the summon row's caster is the host's player, who owns nothing of it
  SECTION("an NPC's summon: no player's, its host told the summon's fight "
          "alone")
  {
    const uint32_t summon = Summoned(p, kPredator);
    REQUIRE(p.OwnerOf(p.worldState.GetFormAt<MpActor>(summon)) == 0);
    {
      LogCapture log;
      Swings(p, 1, summon);
      INFO(log.oss.str());
      REQUIRE(log.Count(fmt::format("ff000d21 hit {:x}, a fight begins; its "
                                    "host ff000d20's game is told",
                                    summon)) == 1);
      REQUIRE(log.Count(kSidesLine) == 0);
    }
    REQUIRE(PlayersNoticesTo(p, 0).empty());
    REQUIRE(SnippetsTo(p, 0, "StartCombat").size() == 2);
    REQUIRE(SnippetsTo(p, 1, "StartCombat").empty());
    REQUIRE_FALSE(p.GetFights().between(kOther, kHost));
  }

  p.GetFights().forget(kHost);
  Leave(p);
}

// tracker #160 for a thrall (ADR-077 decision 2 names both): the predator
// commanded by the host (HostsThrall), its owner by its command row unless
// its raise's stamp names an NPC caster (tracker #106, ClaimReanimated: a
// hosted necromancer's raise makes its host's player the commander)
TEST_CASE("A player's hit on another player's thrall begins a fight with its "
          "commander too, whose game hears the players' notice; a hit on "
          "one's own thrall or an NPC's raise tells no owner (tracker #160, "
          "ADR-077)",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& predator = HostsThrall(p);

  SECTION("another player's thrall: its commander's game is told, and the "
          "thrall joins its commander's fight")
  {
    REQUIRE(p.OwnerOf(predator) == kHost);
    {
      LogCapture log;
      Swings(p, 1, kPredator);
      INFO(log.oss.str());
      REQUIRE(log.Count(fmt::format("ff000d21 hit 85547, {}", kOwnerTold)) ==
              1);
      // the players' pair stands from that hit, so the case's first fights
      // tick runs design A on it (#113)
      REQUIRE(log.Count("thrall 85547 of ff000d20 joins its commander's "
                        "fight with ff000d21") == 1);
    }
    const auto notices = PlayersNoticesTo(p, 0);
    REQUIRE(notices.size() == 1);
    REQUIRE(notices[0]["selfId"] == kOther);
    REQUIRE(SnippetsTo(p, 0, "StartCombat").size() == 3);
    const auto toAttacker = PlayersNoticesTo(p, 1);
    REQUIRE(toAttacker.size() == 1);
    REQUIRE(toAttacker[0]["selfId"] == kHost);
    REQUIRE(SnippetsTo(p, 1, "StartCombat").size() == 1);
    REQUIRE(p.GetFights().between(kOther, kHost));
    REQUIRE(p.GetFights().between(kOther, kPredator));
    // the join, drained: one StartCombat on the thrall aimed at the other
    // player's figure, to the commander's game alone
    p.Messages().clear();
    p.Tick(); // snippets are deferred
    const auto joined = SnippetsTo(p, 0, "StartCombat");
    REQUIRE(joined.size() == 1);
    REQUIRE(joined[0]["selfId"] ==
            SpSnippet::MakeLongFormId(&p.worldState, kPredator));
    REQUIRE(joined[0]["arguments"] ==
            nlohmann::json::array({ nlohmann::json{ { "formId", kOther },
                                                    { "type", "Actor" } } }));
    REQUIRE(SnippetsTo(p, 1, "StartCombat").empty());
  }

  SECTION("one's own thrall: its own engine's, no notice")
  {
    {
      LogCapture log;
      Swings(p, 0, kPredator);
      INFO(log.oss.str());
      REQUIRE(log.Count("no notice, its host's own engine saw the hit") == 1);
      REQUIRE(log.Count(kSidesLine) == 0);
    }
    REQUIRE(SnippetsTo(p, 0, "StartCombat").empty());
    REQUIRE(SnippetsTo(p, 1, "StartCombat").empty());
  }

  // the raise's stamp names an NPC caster (a bystander's server id; the
  // command stays the host's, as ClaimReanimated grants an NPC's raise)
  SECTION("an NPC's raise: its host's command, no player's thrall")
  {
    predator.NoteReanimateAccepted(std::chrono::steady_clock::now(), 0,
                                   kBystanderA);
    REQUIRE(p.CommanderOf(predator) == kHost);
    REQUIRE(p.OwnerOf(predator) == 0);
    {
      LogCapture log;
      Swings(p, 1, kPredator);
      INFO(log.oss.str());
      REQUIRE(log.Count("ff000d21 hit 85547, a fight begins; its host "
                        "ff000d20's game is told") == 1);
      REQUIRE(log.Count(kSidesLine) == 0);
    }
    REQUIRE(PlayersNoticesTo(p, 0).empty());
    REQUIRE(SnippetsTo(p, 0, "StartCombat").size() == 2);
    REQUIRE(SnippetsTo(p, 1, "StartCombat").empty());
    REQUIRE_FALSE(p.GetFights().between(kOther, kHost));
  }

  LeaveThrall(p, predator);
}

// thuum ADR-077 decision 4 (2026-11-18, Eli, playtest 55: "definitionally
// with a hostile thrall the fight COUDLN't end"; tracker #113, #131): a fight
// between two players is a fight between their two sides, each player with
// their summons and thralls (PartOne::SideOf). Every hit between the two
// sides keeps the two players' pair (ActionListener's HitAcrossSides), so it
// ends only when no member of either side has struck the other for the quiet
// window (wire-rules hostility.rs FIGHT_QUIET_MS, a minute), and the two
// sides stand apart only once no member's own fight with the other side
// stands. A thrall still fighting the other side keeps the whole fight on
// and stands down only at its end (#113's design A). Playtest 55's thrall
// 373a6 hit Ariadne 24 times past the players' end (server.log 22:49:23.544
// UTC, "stays in its own fight"): its swings kept its own pair alone then,
// and keep the players' fight now. The thrall's strikes are its host's
// reports (Strikes), the other player's its own game's (Swings)
TEST_CASE("A fight between two players is between their sides: a thrall's "
          "hits keep it on, it ends a minute after the last hit between the "
          "sides, and the thrall stands down only then (thuum ADR-077 "
          "decision 4)",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& predator = HostsThrall(p);
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  auto& other = p.worldState.GetFormAt<MpActor>(kOther);
  // the sides: each player its own, the thrall its commander's
  REQUIRE(p.SideOf(host) == &host);
  REQUIRE(p.SideOf(other) == &other);
  REQUIRE(p.SideOf(predator) == &host);

  // the players' fight with its last hit between the two of them 50 s back,
  // a minute quiet 10 s from now unless a hit between their sides keeps it
  SECTION("the thrall's own swings at the other player keep the players' "
          "fight on past the minute, and no game stands anything down")
  {
    const uint64_t t0 = NowMs();
    REQUIRE(p.GetFights().hit(kHost, kOther, t0 - 50'000));
    {
      LogCapture log;
      Strikes(p, 0, kPredator, kOther);
      INFO(log.oss.str());
      // inside the standing fight: nothing begins between the players
      REQUIRE(log.Count(kSidesLine) == 0);
    }
    REQUIRE(p.GetFights().between(kPredator, kOther));
    p.Tick(); // what the case's first fights tick queued
    p.Messages().clear();
    {
      LogCapture log;
      p.TickFights(t0 + 30'000);
      INFO(log.oss.str());
      REQUIRE(log.Count("is over") == 0);
    }
    p.Tick(); // snippets are deferred
    REQUIRE(p.GetFights().between(kHost, kOther));
    REQUIRE(SnippetsTo(p, 0, "StopCombatAlarm").empty());
    REQUIRE(SnippetsTo(p, 0, "StopCombat").empty());
    REQUIRE(SnippetsTo(p, 1, "StopCombatAlarm").empty());
  }

  SECTION("a thrall's strike on the other player begins the players' fight, "
          "and both players' games hear it")
  {
    {
      LogCapture log;
      Strikes(p, 0, kPredator, kOther);
      INFO(log.oss.str());
      REQUIRE(log.Count("85547 hit ff000d21, between the sides of ff000d20 "
                        "and ff000d21: a fight between the two players "
                        "begins") == 1);
      // the thrall's own pair, a hosted actor's hit on a player
      REQUIRE(log.Count("85547 hit ff000d21, a fight begins; no notice, the "
                        "pair is not two players") == 1);
    }
    REQUIRE(p.GetFights().between(kHost, kOther));
    REQUIRE(p.GetFights().between(kPredator, kOther));
    // each game's figure of the other player, aimed at its own player
    const auto toHost = PlayersNoticesTo(p, 0);
    REQUIRE(toHost.size() == 1);
    REQUIRE(toHost[0]["selfId"] == kOther);
    const auto toOther = PlayersNoticesTo(p, 1);
    REQUIRE(toOther.size() == 1);
    REQUIRE(toOther[0]["selfId"] == kHost);
  }

  SECTION("a minute with no hit between the sides: the fight ends, the "
          "thrall stands down with it, and every game hears the end")
  {
    const uint64_t before = NowMs();
    Strikes(p, 0, kPredator, kOther);
    const uint64_t struck = NowMs();
    p.Tick(); // the join the case's first fights tick queued
    // under a minute after the strike: the fight stands
    p.TickFights(before + 55'000);
    REQUIRE(p.GetFights().between(kHost, kOther));
    p.Tick(); // the rejoin that tick queued
    p.Messages().clear();
    {
      LogCapture log;
      p.TickFights(struck + 60'000);
      INFO(log.oss.str());
      REQUIRE(log.Count("the fight between ff000d20 and ff000d21 is over (a "
                        "minute quiet)") == 1);
      REQUIRE(log.Count("thrall 85547 of ff000d20 stands down with its "
                        "commander's fight with ff000d21") == 1);
      // the thrall's own pair, as quiet, ends in the same tick (D2)
      REQUIRE(log.Count("85547's host ff000d20 is told the end of its fight "
                        "with ff000d21 (D2)") == 1);
      REQUIRE(log.Count("stays in the fight") == 0);
      REQUIRE(log.Count("is told nothing") == 0);
    }
    p.Tick(); // snippets are deferred
    // the commander's game: its figure of the other player stands down and
    // the thrall stops, once at the players' end (design A) and once at the
    // thrall's own pair's (D2), in the same tick
    REQUIRE(SnippetsOn(p, 0, "StopCombatAlarm", kOther) == 2);
    REQUIRE(SnippetsOn(p, 0, "StopCombat", kPredator) == 2);
    // the other player's game: its figures of the commander and the thrall
    REQUIRE(SnippetsOn(p, 1, "StopCombatAlarm", kHost) == 1);
    REQUIRE(SnippetsOn(p, 1, "StopCombatAlarm", kPredator) == 1);
    REQUIRE(SnippetsTo(p, 1, "StopCombat").empty());
    REQUIRE_FALSE(p.GetFights().between(kHost, kOther));
    REQUIRE_FALSE(p.GetFights().between(kPredator, kOther));
  }

  // the other player's sword on the thrall begins the players' fight
  // through it (tracker #160) and the thrall's own; then the commander walks
  // 5000 units off (hostility.rs EXTERIOR_RANGE 3000) with the thrall beside
  // the other player. The two players stand apart; their sides do not
  SECTION("the commander walks off while its thrall fights beside the other "
          "player: the fight stays on until the thrall's own fight ends, "
          "then ends apart five seconds on (APART_MS)")
  {
    const uint64_t before = NowMs();
    Swings(p, 1, kPredator);
    REQUIRE(p.GetFights().between(kHost, kOther));
    REQUIRE(p.GetFights().between(kPredator, kOther));
    host.SetPos(kForest + NiPoint3{ 0.f, 5000.f, 0.f });
    {
      LogCapture log;
      p.TickFights(before + 1'000);
      p.TickFights(before + 7'000);
      INFO(log.oss.str());
      REQUIRE(log.Count("is over") == 0);
    }
    REQUIRE(p.GetFights().between(kHost, kOther));

    // the thrall follows its commander off: its own pair ends apart five
    // seconds on, told to no game while its side's fight stands
    predator.SetPos(kForest + NiPoint3{ 0.f, 5000.f, 0.f });
    p.Tick(); // what the earlier ticks queued
    p.Messages().clear();
    {
      LogCapture log;
      p.TickFights(before + 8'000);
      p.TickFights(before + 13'000);
      INFO(log.oss.str());
      REQUIRE(log.Count("the fight between 85547 and ff000d21 is over "
                        "(apart)") == 1);
      REQUIRE(log.Count("ff000d21's figure of 85547 stays in the fight: "
                        "ff000d21's side and 85547's still fight") == 1);
      REQUIRE(log.Count("85547's fight with ff000d21 is its side's, which "
                        "stands: its host ff000d20 is told nothing") == 1);
      REQUIRE(log.Count("the fight between ff000d20 and ff000d21 is over") ==
              0);
    }
    p.Tick(); // snippets are deferred
    REQUIRE(SnippetsTo(p, 0, "StopCombatAlarm").empty());
    REQUIRE(SnippetsTo(p, 0, "StopCombat").empty());
    REQUIRE(SnippetsTo(p, 1, "StopCombatAlarm").empty());
    REQUIRE_FALSE(p.GetFights().between(kPredator, kOther));
    REQUIRE(p.GetFights().between(kHost, kOther));

    // the sides now stand apart: five seconds on, the players' fight ends
    // apart and the thrall stands down with it
    p.Messages().clear();
    {
      LogCapture log;
      p.TickFights(before + 14'000);
      p.TickFights(before + 19'000);
      INFO(log.oss.str());
      REQUIRE(log.Count("the fight between ff000d20 and ff000d21 is over "
                        "(apart)") == 1);
      REQUIRE(log.Count("thrall 85547 of ff000d20 stands down with its "
                        "commander's fight with ff000d21") == 1);
    }
    p.Tick(); // snippets are deferred
    REQUIRE(SnippetsOn(p, 0, "StopCombatAlarm", kOther) == 1);
    REQUIRE(SnippetsOn(p, 0, "StopCombat", kPredator) == 1);
    REQUIRE(SnippetsOn(p, 1, "StopCombatAlarm", kHost) == 1);
    REQUIRE_FALSE(p.GetFights().between(kHost, kOther));
  }

  LeaveThrall(p, predator);
}

// thuum ADR-077 decision 4 for summons (playtest 54 #126, tracker #131): a
// summon fights in its side's fight as a thrall does. Its hits on the other
// player or on that player's minion keep its owner's fight with that player
// on, so its owner's figure of that player stays in the fight while it
// fights (#126's keep, which its own pair alone kept before), and a minute
// after the last hit between the sides the fight ends for every member. Two
// minions are two NPCs, no pair of their own in the fights table
// (hostility.rs counted): their hits keep the players' pair alone
TEST_CASE("A summon's hits on the other player or on that player's summon "
          "keep its owner's fight with that player on, and a first one "
          "begins it (thuum ADR-077 decision 4)",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  Armed(p);
  auto& host = p.worldState.GetFormAt<MpActor>(kHost);
  auto& other = p.worldState.GetFormAt<MpActor>(kOther);
  // the host's atronach, its game's report, and the other player's, its own
  const uint32_t mine = Summoned(p);
  const uint32_t theirs = Summoned(p, 0, 1);
  REQUIRE(p.SideOf(p.worldState.GetFormAt<MpActor>(mine)) == &host);
  REQUIRE(p.SideOf(p.worldState.GetFormAt<MpActor>(theirs)) == &other);

  SECTION("the host's summon strikes the other player inside the players' "
          "fight: it stays on past the minute, the host's figure of that "
          "player in it, and a minute after the strike it ends for both")
  {
    const uint64_t t0 = NowMs();
    REQUIRE(p.GetFights().hit(kHost, kOther, t0 - 50'000));
    Strikes(p, 0, mine, kOther);
    const uint64_t struck = NowMs();
    REQUIRE(p.GetFights().between(mine, kOther));
    p.Messages().clear();
    {
      LogCapture log;
      p.TickFights(t0 + 30'000);
      INFO(log.oss.str());
      REQUIRE(log.Count("is over") == 0);
    }
    p.Tick(); // snippets are deferred
    REQUIRE(p.GetFights().between(kHost, kOther));
    REQUIRE(SnippetsTo(p, 0, "StopCombatAlarm").empty());
    REQUIRE(SnippetsTo(p, 1, "StopCombatAlarm").empty());

    p.Messages().clear();
    {
      LogCapture log;
      p.TickFights(struck + 60'000);
      INFO(log.oss.str());
      REQUIRE(log.Count("the fight between ff000d20 and ff000d21 is over (a "
                        "minute quiet)") == 1);
      REQUIRE(log.Count("stays in the fight") == 0);
      // the summon's own pair, as quiet, ends in the same tick (D2)
      REQUIRE(log.Count(fmt::format("{:x}'s host ff000d20 is told the end of "
                                    "its fight with ff000d21 (D2)",
                                    mine)) == 1);
    }
    p.Tick(); // snippets are deferred
    // the owner's figure of the other player stands down at the players'
    // end and again at the summon's own (D2), and the summon stops (D2)
    REQUIRE(SnippetsOn(p, 0, "StopCombatAlarm", kOther) == 2);
    REQUIRE(SnippetsOn(p, 0, "StopCombat", mine) == 1);
    REQUIRE(SnippetsOn(p, 1, "StopCombatAlarm", kHost) == 1);
    REQUIRE(SnippetsOn(p, 1, "StopCombatAlarm", mine) == 1);
    REQUIRE_FALSE(p.GetFights().between(kHost, kOther));
    REQUIRE_FALSE(p.GetFights().between(mine, kOther));
  }

  SECTION("the host's summon strikes the other player's summon inside the "
          "players' fight: it stays on past the minute, and the two summons "
          "are no pair of their own")
  {
    const uint64_t t0 = NowMs();
    REQUIRE(p.GetFights().hit(kHost, kOther, t0 - 50'000));
    {
      LogCapture log;
      Strikes(p, 0, mine, theirs);
      INFO(log.oss.str());
      REQUIRE(log.Count(kSidesLine) == 0);
    }
    REQUIRE_FALSE(p.GetFights().between(mine, theirs));
    {
      LogCapture log;
      p.TickFights(t0 + 30'000);
      INFO(log.oss.str());
      REQUIRE(log.Count("is over") == 0);
    }
    REQUIRE(p.GetFights().between(kHost, kOther));
  }

  SECTION("the other player's summon strikes the host's with no players' "
          "fight: it begins, and both players' games hear it")
  {
    {
      LogCapture log;
      Strikes(p, 1, theirs, mine);
      INFO(log.oss.str());
      REQUIRE(log.Count(fmt::format("{:x} hit {:x}, between the sides of "
                                    "ff000d21 and ff000d20: a fight between "
                                    "the two players begins",
                                    theirs, mine)) == 1);
    }
    REQUIRE(p.GetFights().between(kHost, kOther));
    REQUIRE_FALSE(p.GetFights().between(mine, theirs));
    const auto toHost = PlayersNoticesTo(p, 0);
    REQUIRE(toHost.size() == 1);
    REQUIRE(toHost[0]["selfId"] == kOther);
    const auto toOther = PlayersNoticesTo(p, 1);
    REQUIRE(toOther.size() == 1);
    REQUIRE(toOther[0]["selfId"] == kHost);
  }

  p.GetFights().forget(mine);
  p.GetFights().forget(theirs);
  p.GetFights().forget(kHost);
  Leave(p);
}

// thuum ADR-077 decision 4: a player's own summons and thralls alone are on
// its side (PartOne::SideOf, OwnerOf). An NPC's summon or raise its host's
// game runs (tracker #106) is on no player's side: its hits on a player keep
// no players' fight, and #113's design A sends no NPC's raise into one. Its
// own fight with the other player still keeps its host's figure of that
// player at the players' end, so it keeps its target in that game (#126's
// keep, unchanged for a minion on no side)
TEST_CASE("An NPC's summon or raise is on no player's side: its hits keep no "
          "players' fight, and design A sends it into none (thuum ADR-077 "
          "decision 4)",
          "[NpcCast][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;

  SECTION("an NPC's summon: its hit on the other player keeps nothing, and "
          "its own fight keeps its host's figure of that player at the "
          "players' end")
  {
    Armed(p);
    const uint32_t summon = Summoned(p, kPredator);
    REQUIRE(p.SideOf(p.worldState.GetFormAt<MpActor>(summon)) == nullptr);
    const uint64_t t0 = NowMs();
    REQUIRE(p.GetFights().hit(kHost, kOther, t0 - 50'000));
    {
      LogCapture log;
      Strikes(p, 0, summon, kOther);
      INFO(log.oss.str());
      REQUIRE(log.Count(kSidesLine) == 0);
    }
    REQUIRE(p.GetFights().between(summon, kOther));
    p.Messages().clear();
    {
      LogCapture log;
      p.TickFights(t0 + 30'000);
      INFO(log.oss.str());
      REQUIRE(log.Count("the fight between ff000d20 and ff000d21 is over (a "
                        "minute quiet)") == 1);
      REQUIRE(log.Count("ff000d20's figure of ff000d21 stays in the fight: a "
                        "summon or thrall its game runs for an NPC still "
                        "fights it") == 1);
    }
    p.Tick(); // snippets are deferred
    REQUIRE(SnippetsOn(p, 0, "StopCombatAlarm", kOther) == 0);
    REQUIRE(SnippetsOn(p, 1, "StopCombatAlarm", kHost) == 1);
    REQUIRE_FALSE(p.GetFights().between(kHost, kOther));
    REQUIRE(p.GetFights().between(summon, kOther));
    p.GetFights().forget(summon);
    p.GetFights().forget(kHost);
    Leave(p);
  }

  // the raise's stamp names an NPC caster (a bystander's server id; the
  // command stays the host's, as ClaimReanimated grants an NPC's raise)
  SECTION("an NPC's raise its host's game commands: its hit on the other "
          "player keeps nothing, and design A neither joins nor stands it "
          "down")
  {
    auto& predator = HostsThrall(p);
    predator.NoteReanimateAccepted(std::chrono::steady_clock::now(), 0,
                                   kBystanderA);
    REQUIRE(p.CommanderOf(predator) == kHost);
    REQUIRE(p.SideOf(predator) == nullptr);
    const uint64_t t0 = NowMs();
    REQUIRE(p.GetFights().hit(kHost, kOther, t0 - 50'000));
    {
      LogCapture log;
      Strikes(p, 0, kPredator, kOther);
      INFO(log.oss.str());
      REQUIRE(log.Count(kSidesLine) == 0);
      // the case's first fights tick, the players' fight standing
      REQUIRE(log.Count("joins its commander's fight") == 0);
    }
    REQUIRE(p.GetFights().between(kPredator, kOther));
    p.Messages().clear();
    {
      LogCapture log;
      p.TickFights(t0 + 30'000);
      INFO(log.oss.str());
      REQUIRE(log.Count("the fight between ff000d20 and ff000d21 is over (a "
                        "minute quiet)") == 1);
      REQUIRE(log.Count("ff000d20's figure of ff000d21 stays in the fight: a "
                        "summon or thrall its game runs for an NPC still "
                        "fights it") == 1);
      REQUIRE(log.Count("stands down with its commander's fight") == 0);
    }
    p.Tick(); // snippets are deferred
    REQUIRE(SnippetsOn(p, 0, "StopCombat", kPredator) == 0);
    REQUIRE_FALSE(p.GetFights().between(kHost, kOther));
    LeaveThrall(p, predator);
  }
}
