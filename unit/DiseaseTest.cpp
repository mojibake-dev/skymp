#include "ActionListener.h"
#include "HostedActorStateMessage.h"
#include "MpChangeForms.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <functional>
#include <map>
#include <simdjson.h>

PartOne& GetPartOne();

// A disease a player's own engine caught is the player's (R2): its game names
// the effects with a look its engine runs on its player (HostedActorState's
// effects on its own idx), and an effect of a disease spell among them means
// the disease took there. The server records the disease on the player and
// the shared clock's day it first ran, so a login or a restart can bring the
// disease back with its course from that day: a disease's take is its magic
// effect's own condition, which the engine checks once, when the spell is
// applied. Skyrim.esm: DiseaseSanguinareVampiris SPEL 0xb8780 (a Disease, one
// effect) and its effect DisDamageHealthVampire MGEF 0xb8778; the vampire
// drain's effect crVampireAbsorbHealthConcAimed MGEF 0x8d5c5 belongs to no
// disease.
namespace {
constexpr uint32_t kPlayer = 0xff000a71;
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kSanguinare = 0xb8780;
constexpr uint32_t kSanguinareEffect = 0xb8778;
constexpr uint32_t kDrainEffect = 0x8d5c5;
const NiPoint3 kRiverwood{ 17225.f, -47204.f, -128.f };

MpActor& Arrive(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kPlayer, kRiverwood, 0, kTamriel);
  p.SetUserActor(0, kPlayer);
  p.Messages().clear();
  return p.worldState.GetFormAt<MpActor>(kPlayer);
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  p.DestroyActor(kPlayer);
}

// the shared clock pinned at a day for the test's length
struct PinnedClock
{
  PinnedClock(PartOne& p_, float day)
    : p(p_)
    , old(p_.worldState.gameTime)
  {
    Set(day);
  }
  ~PinnedClock() { p.worldState.gameTime = old; }
  void Set(float day)
  {
    p.worldState.gameTime = [day] {
      GameTimeNow t;
      t.daysPassed = day;
      t.timeScale = 20.f;
      return t;
    };
  }
  PartOne& p;
  std::function<GameTimeNow()> old;
};

// the player's own game's report of the effects with a look its engine runs
// on its player
void OwnEffects(PartOne& p, MpActor& me, std::vector<uint32_t> effects)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = 0;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  HostedActorStateMessage msg;
  msg.idx = me.GetIdx();
  msg.health = 1.f;
  msg.magicka = 1.f;
  msg.stamina = 1.f;
  msg.effects = std::move(effects);
  p.GetActionListener().OnHostedActorState(raw, msg);
}

// the record's caught diseases as its JSON holds them: spell id to the day
std::map<uint32_t, double> CaughtOf(const nlohmann::json& record)
{
  std::map<uint32_t, double> caught;
  if (!record.contains("caughtDiseases")) {
    return caught;
  }
  for (const auto& d : record["caughtDiseases"]) {
    caught[d["spell"].get<uint32_t>()] = d["caughtAt"].get<double>();
  }
  return caught;
}
}

TEST_CASE("A disease's effect running on a player's own game records the "
          "disease, caught on the shared clock's day; a later report keeps "
          "that day, and the player's own removal of the spell ends it",
          "[Disease][espm]")
{
  PartOne& p = GetPartOne();
  PinnedClock clock(p, 20.f);
  auto& me = Arrive(p);
  REQUIRE(!me.IsSpellLearned(kSanguinare));

  // the drain's own effect is no disease's: nothing recorded
  OwnEffects(p, me, { kDrainEffect });
  REQUIRE(!me.IsSpellLearned(kSanguinare));
  REQUIRE(CaughtOf(MpChangeForm::ToJson(me.GetChangeForm())).empty());

  // the disease's effect runs: the disease is the player's, caught today
  OwnEffects(p, me, { kDrainEffect, kSanguinareEffect });
  REQUIRE(me.IsSpellLearned(kSanguinare));
  auto caught = CaughtOf(MpChangeForm::ToJson(me.GetChangeForm()));
  REQUIRE(caught.size() == 1);
  REQUIRE(caught[kSanguinare] == Catch::Approx(20.0));

  // a day on, the same report keeps the first day
  clock.Set(21.f);
  OwnEffects(p, me, { kSanguinareEffect });
  caught = CaughtOf(MpChangeForm::ToJson(me.GetChangeForm()));
  REQUIRE(caught.size() == 1);
  REQUIRE(caught[kSanguinare] == Catch::Approx(20.0));

  // a report without it (a relaunch's game before any restore) ends nothing
  OwnEffects(p, me, {});
  REQUIRE(me.IsSpellLearned(kSanguinare));
  REQUIRE(CaughtOf(MpChangeForm::ToJson(me.GetChangeForm())).size() == 1);

  // the player's own game removed the spell (the turn's VampireChange, a
  // cure): the record ends with it
  me.RemoveSpell(kSanguinare);
  REQUIRE(!me.IsSpellLearned(kSanguinare));
  REQUIRE(CaughtOf(MpChangeForm::ToJson(me.GetChangeForm())).empty());
  Leave(p);
}

TEST_CASE("A caught disease and its day survive the change form's JSON, the "
          "plugin-relative record a restart reads included, and an older "
          "record reads none",
          "[Disease][espm]")
{
  PartOne& p = GetPartOne();
  const auto& files = p.worldState.espmFiles;
  simdjson::dom::parser parser;

  nlohmann::json raw = MpChangeForm::ToJson(MpChangeForm());
  raw["recType"] = MpChangeForm::ACHR;
  raw["caughtDiseases"] = nlohmann::json::array(
    { { { "spell", kSanguinare }, { "caughtAt", 20.5 } } });
  const std::string rawDump = raw.dump();
  auto rawElement = parser.parse(rawDump).value();
  const MpChangeForm read = MpChangeForm::JsonToChangeForm(rawElement);
  auto caught = CaughtOf(MpChangeForm::ToJson(read));
  REQUIRE(caught.size() == 1);
  REQUIRE(caught[kSanguinare] == Catch::Approx(20.5));

  // the record a save writes names the spell by its plugin, and reads back
  const nlohmann::json saved = MpChangeForm::ToJson(read, &files);
  REQUIRE(saved.contains("caughtDiseases"));
  REQUIRE(saved["caughtDiseases"][0]["spell"] == "b8780:Skyrim.esm");
  const std::string savedDump = saved.dump();
  auto savedElement = parser.parse(savedDump).value();
  caught = CaughtOf(MpChangeForm::ToJson(
    MpChangeForm::JsonToChangeForm(savedElement, &files)));
  REQUIRE(caught.size() == 1);
  REQUIRE(caught[kSanguinare] == Catch::Approx(20.5));

  // the migration: a record from before the key reads as none, and a record
  // with none writes no key
  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("caughtDiseases"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::ToJson(MpChangeForm::JsonToChangeForm(olderElement))
             .contains("caughtDiseases"));
}
