#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

PartOne& GetPartOne();

// thuum ADR-068 rung 4 (WorldGlobal 80; amendment B; docs/research/
// rung4-state-rows-2026-11-07.md section 1(c)): the GLOBs every seat's
// scripts write are streamed by one seat at an epoch, elected from what
// ADR-066 decision 6 has built (past its first movement report, not stale,
// not dead, the lowest id); the server keeps the world's record on the
// journal, relays what moved and serves the record at a login. Skyrim.esm
// as lab/esm.py read it on sky-srv (2026-11-06; no later master overrides
// these): GLOB 0x17424 DA16ErandurCheckpoint (flags 0, 0.0), GLOB 0xe9d
// CrimeArrestOnSightNonViolentThreshold (flags 0x40, constant), GameHour
// 0x38 (the clock's), STAT 0x3b XMarker (no GLOB).
namespace {
constexpr uint32_t kFirst = 0xff000c50;
constexpr uint32_t kSecond = 0xff000c51;
constexpr uint32_t kLate = 0xff000c52;
constexpr uint32_t kCheckpoint = 0x17424;
constexpr uint32_t kConstant = 0xe9d;
constexpr uint32_t kGameHour = 0x38;
constexpr uint32_t kXMarker = 0x3b;

void Join(PartOne& p, Networking::UserId user, uint32_t actor)
{
  DoConnect(p, user);
  p.CreateActor(actor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(user, actor);
}

void Report(PartOne& p, Networking::UserId user, uint32_t epoch,
            std::vector<WorldGlobalMessage::Entry> entries)
{
  WorldGlobalMessage msg;
  msg.epoch = epoch;
  msg.entries = std::move(entries);
  msg.served = false;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnWorldGlobal(raw, msg);
}

std::vector<WorldGlobalMessage> SentTo(PartOne& p, Networking::UserId user)
{
  std::vector<WorldGlobalMessage> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<WorldGlobalMessage*>(m.message.get());
    if (m.userId == user && typed) {
      out.push_back(*typed);
    }
  }
  return out;
}

std::optional<float> Recorded(PartOne& p, uint32_t global)
{
  const auto desc = FormDesc::FromFormId(global, p.worldState.espmFiles);
  for (const auto& entry : p.GetWorldGlobals()) {
    if (entry.global == desc) {
      return entry.value;
    }
  }
  return std::nullopt;
}

struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("world-global-test", sink);
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

TEST_CASE("The first seat past its first movement report streams the world's "
          "globals, its report recorded and relayed, and the stream passes on "
          "at its leaving",
          "[WorldGlobal][espm]")
{
  auto& p = GetPartOne();
  p.SetWorldGlobalsEnabled(true);
  Join(p, 0, kFirst);
  Join(p, 1, kSecond);
  p.Messages().clear();

  // the first movement report elects its seat: the stream word at epoch 1
  DoUpdateMovement(p, kFirst, 0);
  auto words = SentTo(p, 0);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].epoch == 1);
  REQUIRE(words[0].stream == true);
  REQUIRE(words[0].entries.empty());
  REQUIRE(!words[0].served);
  REQUIRE(p.WorldStreamer() == std::make_pair(kFirst, 1u));
  // a second seat's movement elects nobody new
  DoUpdateMovement(p, kSecond, 1);
  REQUIRE(SentTo(p, 1).empty());
  REQUIRE(p.WorldStreamer() == std::make_pair(kFirst, 1u));

  // the streamer's report: recorded and relayed to the other seat
  p.Messages().clear();
  {
    LogCapture log;
    Report(p, 0, 1, { { kCheckpoint, 2.f } });
    REQUIRE(log.Count("[verb:world-globals] ff000c50 reports 1 global(s) at "
                      "epoch 1: 1 recorded, relayed to 1 game(s)") == 1);
  }
  REQUIRE(Recorded(p, kCheckpoint) == 2.f);
  auto relayed = SentTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0].epoch == 1);
  REQUIRE(!relayed[0].stream.has_value());
  REQUIRE(relayed[0].entries.size() == 1);
  REQUIRE(relayed[0].entries[0].global == kCheckpoint);
  REQUIRE(relayed[0].entries[0].value == 2.f);
  REQUIRE(SentTo(p, 0).empty());
  // the same value again: folded, nothing relayed
  p.Messages().clear();
  Report(p, 0, 1, { { kCheckpoint, 2.f } });
  REQUIRE(SentTo(p, 1).empty());

  // the streamer leaves: the other seat streams from epoch 2
  p.Messages().clear();
  DoDisconnect(p, 0);
  words = SentTo(p, 1);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].epoch == 2);
  REQUIRE(words[0].stream == true);
  REQUIRE(p.WorldStreamer() == std::make_pair(kSecond, 2u));
  DoDisconnect(p, 1);
}

TEST_CASE("A world global report from a seat that does not stream, at another "
          "epoch, or naming the clock, a constant or no GLOB is refused",
          "[WorldGlobal][espm]")
{
  auto& p = GetPartOne();
  p.SetWorldGlobalsEnabled(true);
  Join(p, 0, kFirst);
  Join(p, 1, kSecond);
  DoUpdateMovement(p, kFirst, 0);
  DoUpdateMovement(p, kSecond, 1);
  p.Messages().clear();
  LogCapture log;
  Report(p, 1, 1, { { kCheckpoint, 5.f } });
  REQUIRE(log.Count("[verb:world-globals] E_WORLDGLOBAL_OWNER: ff000c51 "
                    "reports 1 global(s) at epoch 1: ff000c50 streams at "
                    "epoch 1, not the reporter; refused, 1 since the last "
                    "line") == 1);
  Report(p, 0, 2, { { kCheckpoint, 5.f } });
  REQUIRE(log.Count("[verb:world-globals] E_WORLDGLOBAL_EPOCH: ff000c50 "
                    "reports 1 global(s) at epoch 2: the stream stands at "
                    "epoch 1, the report's is ahead of it; refused, 1 since "
                    "the last line") == 1);
  REQUIRE(!Recorded(p, kCheckpoint));
  REQUIRE(SentTo(p, 0).empty());
  REQUIRE(SentTo(p, 1).empty());

  // each entry alone: the clock, a constant and a form that is no GLOB are
  // refused, the rest recorded
  Report(p, 0, 1,
         { { kGameHour, 9.f },
           { kConstant, 1.f },
           { kXMarker, 1.f },
           { kCheckpoint, 3.f } });
  REQUIRE(log.Count("[verb:world-globals] ff000c50 reports 4 global(s) at "
                    "epoch 1: 1 recorded, relayed to 1 game(s), "
                    "E_WORLDGLOBAL_CLOCK x1, E_WORLDGLOBAL_CONSTANT x1, "
                    "E_WORLDGLOBAL_NO_GLOB x1") == 1);
  REQUIRE(Recorded(p, kCheckpoint) == 3.f);
  REQUIRE(!Recorded(p, kGameHour));
  REQUIRE(!Recorded(p, kConstant));
  const auto relayed = SentTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0].entries.size() == 1);
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
}

TEST_CASE("A seat that logs in is served the world's globals; with "
          "worldGlobals off nobody streams",
          "[WorldGlobal][espm]")
{
  {
    auto& p = GetPartOne();
    p.SetWorldGlobalsEnabled(true);
    Join(p, 0, kFirst);
    DoUpdateMovement(p, kFirst, 0);
    Report(p, 0, 1, { { kCheckpoint, 4.f } });
    p.Messages().clear();
    Join(p, 2, kLate);
    const auto served = SentTo(p, 2);
    REQUIRE(served.size() == 1);
    REQUIRE(served[0].served);
    REQUIRE(served[0].epoch == 1);
    REQUIRE(!served[0].stream.has_value());
    REQUIRE(served[0].entries.size() == 1);
    REQUIRE(served[0].entries[0].global == kCheckpoint);
    REQUIRE(served[0].entries[0].value == 4.f);
    DoDisconnect(p, 0);
    DoDisconnect(p, 2);
  }
  {
    // the default: no stream word at a first movement, no serve at a login
    auto& p = GetPartOne();
    REQUIRE(!p.WorldGlobalsEnabled());
    Join(p, 0, kFirst);
    p.Messages().clear();
    DoUpdateMovement(p, kFirst, 0);
    REQUIRE(SentTo(p, 0).empty());
    REQUIRE(p.WorldStreamer() == std::make_pair(0u, 0u));
    DoDisconnect(p, 0);
  }
}
