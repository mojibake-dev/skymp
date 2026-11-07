#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

PartOne& GetPartOne();

// thuum ADR-068 rung 4 (AliasFill 79; amendment B, read 8; docs/research/
// rung4-state-rows-2026-11-07.md section 1(c)): a quest's reference alias is
// nobody's (D-F1 C), so every seat reports its own ForceRefTo or Clear; the
// server records the latest fill per (quest, alias) on the world's journal,
// folds an equal one, relays a recorded one to every other seat and serves
// the record at a login. Skyrim.esm as lab/esm.py read it on sky-srv
// (2026-11-06): QUST 0x39645 MS13, its reference aliases ArvelWeb 10 (forced
// to the web 0x1fc43, a REFR in Bleak Falls Temple) and ShoutPower 3; its
// location alias 4 (Riverwood) is no reference alias; STAT 0x3b XMarker is
// no quest; MISC 0x39647 MS13GoldenClaw is a base, no reference.
namespace {
constexpr uint32_t kReporter = 0xff000c40;
constexpr uint32_t kWatcher = 0xff000c41;
constexpr uint32_t kLate = 0xff000c42;
constexpr uint32_t kMs13 = 0x39645;
constexpr uint32_t kWeb = 0x1fc43;
constexpr uint32_t kXMarker = 0x3b;
constexpr uint32_t kClaw = 0x39647;

void Join(PartOne& p, Networking::UserId user, uint32_t actor)
{
  DoConnect(p, user);
  p.CreateActor(actor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(user, actor);
}

void Fill(PartOne& p, Networking::UserId user, uint32_t quest, uint32_t alias,
          uint32_t refr)
{
  AliasFillMessage msg;
  msg.quest = quest;
  msg.alias = alias;
  msg.refr = refr;
  msg.served = false;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnAliasFill(raw, msg);
}

std::vector<AliasFillMessage> SentTo(PartOne& p, Networking::UserId user)
{
  std::vector<AliasFillMessage> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<AliasFillMessage*>(m.message.get());
    if (m.userId == user && typed) {
      out.push_back(*typed);
    }
  }
  return out;
}

std::optional<FormDesc> RecordedRefr(PartOne& p, uint32_t alias)
{
  const auto quest = FormDesc::FromFormId(kMs13, p.worldState.espmFiles);
  for (const auto& fill : p.GetAliasFills()) {
    if (fill.quest == quest && fill.alias == alias) {
      return fill.refr;
    }
  }
  return std::nullopt;
}

// the default logger's lines at info and above while the guard lives
// (DestructionStageTest's shape), restored at its end
struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("alias-fill-test", sink);
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
}

TEST_CASE("A seat's alias fill is recorded on the journal and relayed to the "
          "other seats; an equal one is folded, a clear recorded",
          "[AliasFill][espm]")
{
  auto& p = GetPartOne();
  Join(p, 0, kReporter);
  Join(p, 1, kWatcher);
  p.Messages().clear();

  {
    LogCapture log;
    Fill(p, 0, kMs13, 10, kWeb);
    REQUIRE(log.Count("[verb:quest-stage] ff000c40 fills alias 39645/10 with "
                      "1fc43: recorded, relayed to 1 game(s)") == 1);
  }
  REQUIRE(RecordedRefr(p, 10) ==
          FormDesc::FromFormId(kWeb, p.worldState.espmFiles));
  auto relayed = SentTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0].quest == kMs13);
  REQUIRE(relayed[0].alias == 10);
  REQUIRE(relayed[0].refr == kWeb);
  REQUIRE(!relayed[0].served);
  REQUIRE(SentTo(p, 0).empty());

  // the other seat's copy of the same fill: folded, nothing sent
  p.Messages().clear();
  Fill(p, 1, kMs13, 10, kWeb);
  REQUIRE(SentTo(p, 0).empty());
  REQUIRE(SentTo(p, 1).empty());

  // a clear: recorded as the empty desc, relayed as refr 0
  Fill(p, 1, kMs13, 10, 0);
  REQUIRE(RecordedRefr(p, 10) == FormDesc());
  relayed = SentTo(p, 0);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0].refr == 0);

  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
}

TEST_CASE("An alias fill naming no quest, no reference alias of the quest or "
          "no reference is refused by name and relayed to nobody",
          "[AliasFill][espm]")
{
  auto& p = GetPartOne();
  Join(p, 0, kReporter);
  Join(p, 1, kWatcher);
  p.Messages().clear();
  LogCapture log;
  Fill(p, 0, kXMarker, 10, kWeb);
  Fill(p, 0, kMs13, 99, kWeb);
  // MS13's alias 4 is a location alias (ALLS), no reference alias
  Fill(p, 0, kMs13, 4, kWeb);
  Fill(p, 0, kMs13, 3, kClaw);
  REQUIRE(log.Count("[verb:quest-stage] E_ALIASFILL_NO_QUEST: ff000c40 fills "
                    "alias 3b/10 with 1fc43: 3b is no QUST record; refused, 1 "
                    "since the last line") == 1);
  REQUIRE(log.Count("[verb:quest-stage] E_ALIASFILL_NO_ALIAS: ff000c40 fills "
                    "alias 39645/99 with 1fc43: quest 39645 has no reference "
                    "alias 99; refused, 1 since the last line") == 1);
  // the second NO_ALIAS inside the minute is counted, not said
  REQUIRE(log.Count("E_ALIASFILL_NO_ALIAS") == 1);
  REQUIRE(log.Count("[verb:quest-stage] E_ALIASFILL_REF: ff000c40 fills "
                    "alias 39645/3 with 39647: 39647 is no reference; "
                    "refused, 1 since the last line") == 1);
  REQUIRE(SentTo(p, 1).empty());
  REQUIRE(!RecordedRefr(p, 3));
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
}

TEST_CASE("A seat that logs in is served the world's alias fills",
          "[AliasFill][espm]")
{
  auto& p = GetPartOne();
  Join(p, 0, kReporter);
  Fill(p, 0, kMs13, 10, kWeb);
  Fill(p, 0, kMs13, 3, 0);
  p.Messages().clear();
  Join(p, 2, kLate);
  const auto served = SentTo(p, 2);
  REQUIRE(served.size() == 2);
  for (const auto& fill : served) {
    REQUIRE(fill.served);
    REQUIRE(fill.quest == kMs13);
  }
  REQUIRE(std::count_if(served.begin(), served.end(), [](const auto& f) {
            return f.alias == 10 && f.refr == kWeb;
          }) == 1);
  REQUIRE(std::count_if(served.begin(), served.end(), [](const auto& f) {
            return f.alias == 3 && f.refr == 0;
          }) == 1);
  DoDisconnect(p, 0);
  DoDisconnect(p, 2);
}
