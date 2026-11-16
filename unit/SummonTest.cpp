#include "ActionListener.h"
#include "CellLoadedMessage.h"
#include "Messages.h"
#include "MpActor.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <fmt/format.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <spdlog/sinks/ostream_sink.h>
#include <sstream>
#include <variant>

PartOne& GetPartOne();

// thuum tracker #84 (docs/verbs/summon.md, ADR-052 decision 3 as amended
// 2026-11-16, ADR-063: the caster's row owns its summon): a player's engine
// reports the summon its summon effect placed (Summon 83: the local id, the
// base, the effect and the spell, the place, the time left) and the server
// places its own actor, which the caster's game simulates through the create
// word's ownLocal bind while every other game draws a puppet. R2 recorded
// unvalidated (Eli, 2026-11-16: "'The server checks that the caster has
// learned the spell' This is an example of an unnecessary check. This is not
// a competitive game."): the base, the effect and the spell are the report's
// own words, recorded as told, and the rule keeps the record's consistencies
// alone (the caster, its engine's id, a base the files hold, a cell it
// knows). The forms are the baseline's (x-summon-baseline 20261116-083744,
// lab/esm.py on sky-srv, 2026-11-16): ConjureFlameAtronachRightHand 0x100e76,
// its potent effect MGEF 0x4e945 summoning NPC_ 0x4e940, the plain 0x1ceaa
// summoning NPC_ 0x204c0; SCRL 0xa44a1 ConjureStormAtronachScroll and MISC
// 0x12fe7 Basket01 by the same read. Tamriel is 0x3c; the two stand at its
// origin, the summon 300 units ahead.

namespace {
constexpr uint32_t kCaster = 0xff000c81;
constexpr uint32_t kWatcher = 0xff000c82;
constexpr uint32_t kConjure = 0x100e76;
constexpr uint32_t kPlainEffect = 0x1ceaa;
constexpr uint32_t kPlainAtronach = 0x204c0;
constexpr uint32_t kPotentEffect = 0x4e945;
constexpr uint32_t kPotentAtronach = 0x4e940;
constexpr uint32_t kLocal = 0xff0008fd;
constexpr uint32_t kStormScroll = 0xa44a1; // SCRL ConjureStormAtronachScroll
constexpr uint32_t kBasket = 0x12fe7;      // MISC Basket01

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

struct SummonLog
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  SummonLog()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("summon-test", sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
  }
  ~SummonLog() { spdlog::set_default_logger(old); }
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

SummonMessage Start(uint32_t local, uint32_t base, uint32_t effect,
                    float remainingS, float x)
{
  SummonMessage msg;
  msg.localId = local;
  msg.baseId = base;
  msg.effectId = effect;
  msg.spellId = kConjure;
  msg.pos = { x, 0.f, 0.f };
  msg.rot = { 0.f, 0.f, 90.f };
  msg.worldOrCell = 0x3c;
  msg.remainingS = remainingS;
  msg.ended = false;
  return msg;
}

void Report(PartOne& p, const SummonMessage& msg)
{
  RawMessageData raw;
  raw.userId = 0;
  p.GetActionListener().OnSummon(raw, msg);
}

// the two players at Tamriel's origin; the caster knows the spell
void Enter(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kCaster, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kCaster);
  DoConnect(p, 1);
  p.CreateActor(kWatcher, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(1, kWatcher);
  for (uint32_t id : { kCaster, kWatcher }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos({ 0, 0, 0 });
    actor.ForceSubscriptionsUpdate();
  }
  p.worldState.GetFormAt<MpActor>(kCaster).AddSpell(kConjure);
  p.Messages().clear();
}

// any fading summons finish their fade (the summons' clock is the cell
// host's), the departure dispels any still live, then the players go
void Leave(PartOne& p)
{
  p.TickCellHosts(NowMs() + 2000);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.TickCellHosts(NowMs() + 2000);
  p.DestroyActor(kCaster);
  p.DestroyActor(kWatcher);
}

// `user` hosts `cell` (ActorDeathTest's Host idiom): any earlier host's
// grace passes first, then the loaded report grants the cell
void HostCell(PartOne& p, Networking::UserId user, uint32_t cell)
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

// one listener's CreateActor words for the summons placed
std::vector<nlohmann::json> CreateWordsTo(PartOne& p, Networking::UserId user)
{
  std::vector<nlohmann::json> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::CreateActor)) {
      out.push_back(m.j);
    }
  }
  return out;
}

} // namespace

TEST_CASE("A summon report places the server's actor, binds its caster and "
          "puppets the watcher; each broken consistency is refused by name, "
          "and a scroll's summon records like a learned spell's (tracker #84)",
          "[Summon][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  SummonLog log;

  // the consistencies, each by its first name, nothing placed: a base that
  // is no actor record the files hold (MISC 0x12fe7 Basket01, lab/esm.py on
  // sky-srv), and a place in no cell the server knows (the basket's own id,
  // which is no WRLD or CELL)
  Report(p, Start(kLocal + 1, kBasket, kPotentEffect, 59.5f, 300.f));
  {
    auto msg = Start(kLocal + 1, kPotentAtronach, kPotentEffect, 59.5f, 300.f);
    msg.worldOrCell = kBasket;
    Report(p, msg);
  }
  REQUIRE(log.Count("E_SUMMON_BASE: the base 12fe7 is no actor record the "
                    "files hold; nothing placed") == 1);
  REQUIRE(log.Count("E_SUMMON_CELL: the place is in no cell the server's "
                    "files hold; nothing placed") == 1);
  REQUIRE(CreateWordsTo(p, 0).empty());
  p.Messages().clear();

  // the learned spell's report and a scroll's record alike: R2 recorded
  // unvalidated, the spell and the effect are the report's own words
  // (SCRL 0xa44a1 ConjureStormAtronachScroll is never learned, and its
  // effect rides unjudged)
  Report(p, Start(kLocal, kPotentAtronach, kPotentEffect, 59.5f, 300.f));
  {
    auto msg = Start(kLocal + 1, kPlainAtronach, 0, 59.5f, 300.f);
    msg.spellId = kStormScroll;
    Report(p, msg);
  }
  const auto bound = CreateWordsTo(p, 0);
  const auto puppet = CreateWordsTo(p, 1);
  REQUIRE(bound.size() == 2);
  REQUIRE(puppet.size() == 2);
  for (size_t i = 0; i < 2; ++i) {
    REQUIRE(bound[i]["props"].value("ownLocal", 0) ==
            (i == 0 ? kLocal : kLocal + 1));
    REQUIRE(bound[i]["props"].value("isHostedByOther", true) == false);
    REQUIRE(bound[i].value("baseRecordType", "") == "NPC_");
    REQUIRE(bound[i].value("baseId", 0) ==
            (i == 0 ? kPotentAtronach : kPlainAtronach));
    REQUIRE(puppet[i]["props"].contains("ownLocal") == false);
    REQUIRE(puppet[i]["props"].value("isHostedByOther", false) == true);
    // the puppet's word names no record type, as no server-made actor's
    // does (the client's copy path keys on the base, never the type; the
    // caster's word alone carries NPC_, with the bind)
  }

  // the same engine reference twice: the record holds it
  Report(p, Start(kLocal, kPotentAtronach, kPotentEffect, 59.5f, 300.f));
  REQUIRE(log.Count("E_SUMMON_TWICE") == 1);

  // an end naming no live summon folds
  {
    SummonMessage end;
    end.localId = kLocal + 9;
    end.ended = true;
    Report(p, end);
  }
  REQUIRE(log.Count("ff000c81's engine reports its summon ff000906 ended; "
                    "the record holds none live by that id, folded") == 1);

  Leave(p);
}

TEST_CASE("A summon's end dispels it: the kill reaches every game, the "
          "fade's snippet the watcher's alone, the fade's end deletes the "
          "actor (tracker #84)",
          "[Summon][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  SummonLog log;
  Report(p, Start(kLocal, kPotentAtronach, kPotentEffect, 59.5f, 300.f));
  // the word's idx is the world's change-form index, never the form id;
  // refrId carries the form id (PartOne.cpp's fill, npc-puppet's note)
  const uint32_t summon =
    static_cast<uint32_t>(CreateWordsTo(p, 0)[0].value("refrId", 0ull));
  REQUIRE(summon != 0);
  p.Messages().clear();

  {
    SummonMessage end;
    end.localId = kLocal;
    end.ended = true;
    Report(p, end);
  }
  REQUIRE(log.Count(fmt::format(
            "{:x} dispels (its effect's end, its engine's own word), its "
            "fade 1.5 s",
            summon)) == 1);
  REQUIRE(log.Count(fmt::format("{:x} died: no respawn scheduled", summon)) ==
          1);
  // the kill's death and the fade's snippet: the caster (the summon's
  // simulator, the host) hears the death state container, the watcher the
  // isDead property (ADR-048's seam: a game-file actor's death is the
  // server's word on every seat, the container reaching the host alone and
  // the other games the property), and the snippet the watcher's alone,
  // the caster's engine having run its own end
  bool deathToCaster = false, deathToWatcher = false, alphaToCaster = false,
       alphaToWatcher = false;
  for (auto& m : p.Messages()) {
    if (!m.j.is_object()) {
      continue;
    }
    if (m.j.value("t", -1) == static_cast<int>(MsgType::SpSnippet) &&
        m.j.value("function", "") == "SetAlpha") {
      (m.userId == 0 ? alphaToCaster : alphaToWatcher) = true;
    }
    if (m.j.value("t", -1) == static_cast<int>(MsgType::DeathStateContainer)) {
      deathToCaster = deathToCaster || m.userId == 0;
    }
    // both seats hear the property; accumulate, never assign, so the
    // listener set's order decides nothing
    if (m.j.value("propName", std::string()) == "isDead" &&
        m.j.value("refrId", 0ull) ==
          static_cast<decltype(m.j.value("refrId", 0ull))>(summon)) {
      deathToWatcher = deathToWatcher || m.userId == 1;
    }
  }
  REQUIRE(deathToCaster);
  REQUIRE(deathToWatcher);
  REQUIRE(!alphaToCaster);
  REQUIRE(alphaToWatcher);

  // the fade's end deletes the actor: DestroyActor to both, the record gone
  p.Messages().clear();
  p.TickCellHosts(NowMs() + 2000);
  bool destroyedCaster = false, destroyedWatcher = false;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::DestroyActor)) {
      destroyedCaster = true;
    }
    if (m.userId == 1 && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::DestroyActor)) {
      destroyedWatcher = true;
    }
  }
  REQUIRE(destroyedCaster);
  REQUIRE(destroyedWatcher);
  REQUIRE(log.Count(fmt::format("{:x} deleted, its fade ended", summon)) == 1);
  const auto& goneForm = p.worldState.LookupFormByIdNoLoad(summon);
  MpActor* gone = goneForm ? goneForm->AsActor() : nullptr;
  REQUIRE(gone);
  REQUIRE(gone->IsDeleted());

  Leave(p);
}

TEST_CASE("A summon's caster's departure dispels it, and the backstop "
          "dispels one whose end never came (tracker #84)",
          "[Summon][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  SummonLog log;
  Report(p, Start(kLocal, kPotentAtronach, kPotentEffect, 0.5f, 300.f));
  // the word's idx is the world's change-form index, never the form id;
  // refrId carries the form id (PartOne.cpp's fill, npc-puppet's note)
  const uint32_t summon =
    static_cast<uint32_t>(CreateWordsTo(p, 0)[0].value("refrId", 0ull));
  REQUIRE(summon != 0);

  // the backstop: 0.5 s left plus its 5 s, no end report
  p.TickCellHosts(NowMs() + 6000);
  REQUIRE(
    log.Count(fmt::format(
      "{:x} dispels (its backstop, its end report never came)", summon)) == 1);

  // the departure: a live summon dispels with its caster
  Report(p, Start(kLocal + 1, kPlainAtronach, kPlainEffect, 59.5f, 300.f));
  const uint32_t second =
    static_cast<uint32_t>(CreateWordsTo(p, 0)[1].value("refrId", 0ull));
  DoDisconnect(p, 0);
  REQUIRE(log.Count(fmt::format("{:x} dispels (its caster's departure)",
                                second)) == 1);
  // the leaver's own word is gone with it: the snippet served the
  // watcher's alone
  int snippets = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == 1 && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::SpSnippet) &&
        m.j.value("function", "") == "SetAlpha") {
      ++snippets;
    }
  }
  REQUIRE(snippets >= 1);
  DoConnect(p, 0);
  p.SetUserActor(0, kCaster);
  Leave(p);
}

// thuum tracker #106 (ADR-063: one engine owns each piece of the world): an
// NPC's summon is its caster's owner's. The game that simulates the NPC (its
// cell's host) reports it, naming the NPC by its server id, and simulates the
// summon through the bind as it would its player's own; another game's report
// of that NPC's summon would be a second owner, refused by name, as is a
// caster another user plays. The NPC is a server-made actor 200 units from the
// players, in the cell the caster's game loaded
TEST_CASE("An NPC's summon is its host's to report: the host's game binds it, "
          "any other game's report is refused by name, and its end is taken "
          "from the row's game (tracker #106)",
          "[Summon][espm]")
{
  PartOne& p = GetPartOne();
  const bool npcEnabled = p.worldState.npcEnabled;
  p.worldState.npcEnabled = true;
  Enter(p);
  constexpr uint32_t kNpc = 0xff000c90;
  p.CreateActor(kNpc, { 200, 0, 0 }, 0, 0x3c);
  auto& npc = p.worldState.GetFormAt<MpActor>(kNpc);
  npc.SetPos({ 200, 0, 0 });
  const auto cell = p.worldState.CellOf(npc);
  REQUIRE(cell.has_value());
  HostCell(p, 0, *cell);
  REQUIRE(p.worldState.HostOf(npc) ==
          &p.worldState.GetFormAt<MpActor>(kCaster));
  p.Messages().clear();
  SummonLog log;

  // the watcher's game does not simulate the NPC, and the watcher is a
  // caster another user plays: both refused by name, nothing placed
  {
    auto msg = Start(kLocal + 3, kPotentAtronach, kPotentEffect, 59.5f, 300.f);
    msg.casterId = kNpc;
    RawMessageData raw;
    raw.userId = 1;
    p.GetActionListener().OnSummon(raw, msg);
  }
  REQUIRE(log.Count("E_SUMMON_CASTER: ff000c82 reports summon ff000900 of "
                    "caster ff000c90, its host ff000c81; nothing placed") ==
          1);
  {
    auto msg = Start(kLocal + 3, kPotentAtronach, kPotentEffect, 59.5f, 300.f);
    msg.casterId = kWatcher;
    Report(p, msg);
  }
  REQUIRE(log.Count("E_SUMMON_CASTER: ff000c81 reports summon ff000900 of "
                    "caster ff000c82, an actor another user plays; nothing "
                    "placed") == 1);
  REQUIRE(CreateWordsTo(p, 0).empty());
  REQUIRE(CreateWordsTo(p, 1).empty());

  // the host's report stands: the NPC named, the host's game bound, the
  // watcher's a puppet
  {
    auto msg = Start(kLocal + 3, kPotentAtronach, kPotentEffect, 59.5f, 300.f);
    msg.casterId = kNpc;
    Report(p, msg);
  }
  REQUIRE(log.Count("ff000c90's summon by its host ff000c81's report ff000900 "
                    "(base 4e940") == 1);
  REQUIRE(log.Count("stands for 59.5 s, its owner ff000c81's game") == 1);
  const auto bound = CreateWordsTo(p, 0);
  const auto puppet = CreateWordsTo(p, 1);
  REQUIRE(bound.size() == 1);
  REQUIRE(puppet.size() == 1);
  REQUIRE(bound[0]["props"].value("ownLocal", 0) == kLocal + 3);
  REQUIRE(bound[0]["props"].value("isHostedByOther", true) == false);
  REQUIRE(puppet[0]["props"].contains("ownLocal") == false);

  // its end from the row's game, the NPC named, dispels it
  {
    SummonMessage end;
    end.localId = kLocal + 3;
    end.ended = true;
    end.casterId = kNpc;
    Report(p, end);
  }
  REQUIRE(log.Count("dispels (its effect's end, its engine's own word)") == 1);

  p.DestroyActor(kNpc);
  Leave(p);
  p.worldState.npcEnabled = npcEnabled;
}
