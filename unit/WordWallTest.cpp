#include "ActionListener.h"
#include "MsgType.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

PartOne& GetPartOne();

// thuum docs/verbs/word-wall.md (M4): a word wall's word into the shout
// record. Bleak Falls Sanctum 0x371dd (lab/esm.py cellrefs, 2026-10-31): the
// trigger 0x709d2 (ACTI 0x5095e WordWallTrigger, its script's myWord01 the
// WOOP WordFus 0x13e22, its wordWall01 the wall 0x709d1) at (-7601, -154,
// 674); the wall 0x709d1 itself (ACTI 0x7098e, at (-7802, 100, 521)) teaches
// nothing and is what the reach is measured to (the trigger's bounds are a
// 4,000-unit box, lab/esm.py 2026-11-01).
namespace {
constexpr uint32_t kReader = 0xff000fd1;
constexpr uint32_t kSanctum = 0x371dd;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kTrigger = 0x709d2;
constexpr uint32_t kWallItself = 0x709d1;
constexpr uint32_t kWord = 0x13e22;
const NiPoint3 kByTheTrigger{ -7601.f, -254.f, 674.f }; // 100 units off

void Enter(PartOne& p, const NiPoint3& pos, uint32_t cell)
{
  DoConnect(p, 0);
  p.CreateActor(kReader, pos, 0, cell);
  p.SetUserActor(0, kReader);
  p.Messages().clear();
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  p.DestroyActor(kReader);
}

void ReportWall(PartOne& p, uint32_t wall)
{
  DoMessage(p, 0,
            nlohmann::json{ { "t", static_cast<int>(MsgType::WordWall) },
                            { "wall", wall } });
}

// the Game.TeachWord snippets sent to the reader (deferred to a tick)
int TeachWordsTold(PartOne& p)
{
  p.Tick();
  int n = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::SpSnippet &&
        m.j["class"] == "Game" && m.j["function"] == "TeachWord") {
      ++n;
    }
  }
  return n;
}
}

TEST_CASE("A word wall's trigger teaches its word into the record once, "
          "within reach, and the game is told",
          "[WordWall][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, kByTheTrigger, kSanctum);
  auto& reader = p.worldState.GetFormAt<MpActor>(kReader);
  REQUIRE_FALSE(reader.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  ReportWall(p, kTrigger);
  REQUIRE(reader.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  REQUIRE(reader.GetShoutRecord().wordsUnlocked.empty());
  REQUIRE(TeachWordsTold(p) == 1);
  // again: known already, the record and the game left alone
  ReportWall(p, kTrigger);
  REQUIRE(reader.GetShoutRecord().wordsKnown.size() == 1);
  REQUIRE(TeachWordsTold(p) == 1);
  // the record outlives the actor's form as saved
  REQUIRE(reader.GetChangeForm().shoutRecord.has_value());
  REQUIRE(reader.GetChangeForm().shoutRecord->wordsKnown.size() == 1);
  Leave(p);
}

TEST_CASE("A word wall report from afar, from another cell, naming the wall "
          "itself or no reference teaches nothing",
          "[WordWall][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, { -7601.f, -254.f + 2500.f, 674.f }, kSanctum);
  auto& reader = p.worldState.GetFormAt<MpActor>(kReader);
  ReportWall(p, kTrigger); // afar
  REQUIRE_FALSE(reader.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  reader.SetPos(kByTheTrigger);
  ReportWall(p, kWallItself); // the wall, not its trigger: no word
  REQUIRE_FALSE(reader.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  ReportWall(p, 0x12345); // nothing there
  REQUIRE_FALSE(reader.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  REQUIRE(TeachWordsTold(p) == 0);
  Leave(p);
  // another cell at the trigger's coordinates: the temple above the sanctum
  Enter(p, { -7601.f, -154.f, 674.f }, kTemple);
  auto& visitor = p.worldState.GetFormAt<MpActor>(kReader);
  ReportWall(p, kTrigger);
  REQUIRE_FALSE(visitor.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  REQUIRE(TeachWordsTold(p) == 0);
  Leave(p);
}
