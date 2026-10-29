#include "ActionListener.h"
#include "ActorFateMessage.h"
#include "ActorValueRecord.h"
#include "CellLoadedMessage.h"
#include "ConsoleCommandMessage.h"
#include "HitMessage.h"
#include "MsgType.h"
#include "PlayerBowShotMessage.h"
#include "SpellCastMessage.h"
#include "TestUtils.hpp"
#include "UpdateEquipmentMessage.h"
#include "libespm/Loader.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
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
