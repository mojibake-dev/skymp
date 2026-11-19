#include "ActionListener.h"
#include "MsgType.h"
#include "TestUtils.hpp"
#include "libespm/SHOU.h"
#include <catch2/catch_all.hpp>

PartOne& GetPartOne();

// thuum docs/verbs/word-wall.md (M4): a word wall's word into the shout
// record. Bleak Falls Sanctum 0x371dd (lab/esm.py cellrefs, 2026-10-31): the
// trigger 0x709d2 (ACTI 0x5095e WordWallTrigger, its script's myWord01 the
// WOOP WordFus 0x13e22, its wordWall01 the wall 0x709d1) at (-7601, -154,
// 674); the wall 0x709d1 itself (ACTI 0x7098e, at (-7802, 100, 521)) teaches
// nothing; the trigger's primitive is a sphere of radius 2,065 around it
// (XPRM type 2, lab/esm.py 2026-11-01), and the engine sends OnTrigger to
// a player anywhere inside it (x-word-wall-read 20261101-131832), so a
// report from inside the sphere is accepted however far the wall is, and
// one from outside it falls to the activation reach of the wall.
namespace {
constexpr uint32_t kReader = 0xff000fd1;
constexpr uint32_t kSanctum = 0x371dd;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kTrigger = 0x709d2;
constexpr uint32_t kWallItself = 0x709d1;
constexpr uint32_t kWord = 0x13e22;
// thuum tracker #138: the shout the word belongs to, UnrelentingForceShout
// SHOU 0x13e07, its first word WOOP 0x13e22 (ShoutsTest.cpp's lab/esm.py
// read of Skyrim.esm, 2026-10-10; read again with `lab/esm.py find
// Skyrim.esm SHOU UnrelentingForce` on sky-srv, each SNAM[12]'s first four
// bytes a word; EspmTest.cpp's "SHOU variations" reads the same SNAMs); the
// case below reads it through the test's own espm before it relies on it
constexpr uint32_t kShout = 0x13e07;
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

// the Actor.AddShout snippets naming the shout sent to the reader (thuum
// tracker #138)
int ShoutsAddedTold(PartOne& p, uint32_t shout)
{
  p.Tick();
  int n = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::SpSnippet &&
        m.j["class"] == "Actor" && m.j["function"] == "AddShout" &&
        m.j["arguments"].size() == 1 &&
        m.j["arguments"][0]["formId"] == shout) {
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

TEST_CASE("A word wall report from inside the trigger's sphere, far from "
          "the wall, teaches the word (the engine's own condition)",
          "[WordWall][espm]")
{
  PartOne& p = GetPartOne();
  // 1,500 units south of the trigger: inside its 2,065 sphere, about 1,770
  // off the wall, far past the activation reach
  Enter(p, { -7601.f, -154.f - 1500.f, 674.f }, kSanctum);
  auto& reader = p.worldState.GetFormAt<MpActor>(kReader);
  ReportWall(p, kTrigger);
  REQUIRE(reader.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  REQUIRE(TeachWordsTold(p) == 1);
  Leave(p);
}

TEST_CASE("A word wall report from afar, from another cell, naming the wall "
          "itself or no reference teaches nothing",
          "[WordWall][espm]")
{
  PartOne& p = GetPartOne();
  // 2,400 units north of the trigger: outside its sphere (a box of the
  // same extents would still hold it on this heading, which is why the
  // shape is read), and far past the wall's reach
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

TEST_CASE("A word learned at a wall records the shout it belongs to, and the "
          "game is told both (thuum tracker #138)",
          "[WordWall][espm]")
{
  PartOne& p = GetPartOne();
  // the premise, read through the test's espm: the shout's first variation
  // (its first SNAM) names the wall's word, and the index finds it there
  const auto shou = p.worldState.GetEspm().GetBrowser().LookupById(kShout);
  REQUIRE(shou.rec != nullptr);
  const auto data = espm::GetData<espm::SHOU>(kShout, &p.worldState);
  REQUIRE(data.count >= 1);
  REQUIRE(shou.ToGlobalId(data.variations[0].word) == kWord);
  REQUIRE(p.ShoutOfWord(kWord).value_or(0) == kShout);
  REQUIRE_FALSE(p.ShoutOfWord(kWallItself).has_value());

  Enter(p, kByTheTrigger, kSanctum);
  auto& reader = p.worldState.GetFormAt<MpActor>(kReader);
  REQUIRE_FALSE(reader.HasShoutEntry(MpActor::ShoutEntry::Shout, kShout));
  ReportWall(p, kTrigger);
  REQUIRE(reader.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  REQUIRE(reader.HasShoutEntry(MpActor::ShoutEntry::Shout, kShout));
  REQUIRE(reader.GetShoutRecord().shouts.size() == 1);
  REQUIRE(ShoutsAddedTold(p, kShout) == 1);
  REQUIRE(TeachWordsTold(p) == 1);
  // the record as saved holds both, so a relaunch gives back both
  REQUIRE(reader.GetChangeForm().shoutRecord.has_value());
  REQUIRE(reader.GetChangeForm().shoutRecord->shouts.size() == 1);
  // again: known already, nothing more recorded or told
  ReportWall(p, kTrigger);
  REQUIRE(reader.GetShoutRecord().shouts.size() == 1);
  REQUIRE(ShoutsAddedTold(p, kShout) == 1);
  Leave(p);
}
