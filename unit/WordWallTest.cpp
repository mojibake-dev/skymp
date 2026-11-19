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
// nothing; the trigger's primitive is a sphere of radius 2,065 around it
// (XPRM type 2, lab/esm.py 2026-11-01), and the engine sends OnTrigger to
// a player anywhere inside it (x-word-wall-read 20261101-131832), so a
// report from inside the sphere is accepted however far the wall is, and
// one from outside it falls to the activation reach of the wall.
// thuum tracker #166: an accepted report teaches only under the script's
// innerRadius (300) of its lookTargetWord01, the marker 0x709d4 at (-7876,
// 188, 652) (lab/esm.py, 2026-11-17); one from farther out arms the wall,
// and the player's own movement update under that radius teaches.
namespace {
constexpr uint32_t kReader = 0xff000fd1;
constexpr uint32_t kSanctum = 0x371dd;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kTrigger = 0x709d2;
constexpr uint32_t kWallItself = 0x709d1;
constexpr uint32_t kWord = 0x13e22;
constexpr uint32_t kMark = 0x709d4; // the trigger's lookTargetWord01
const NiPoint3 kByTheTrigger{ -7601.f, -254.f, 674.f }; // 100 units off
// about 520 units off the word's mark: inside the sphere, short of the word
const NiPoint3 kShortOfTheWord = kByTheTrigger;
// about 100 units off the mark and 90 off the wall
const NiPoint3 kByTheWord{ -7800.f, 150.f, 600.f };
// about 180 units off the wall but 314 off the mark: the mark is measured
const NiPoint3 kByTheWallNotTheWord{ -7650.f, 0.f, 540.f };
// 250 units off the mark, south of it
const NiPoint3 kUnderTheRadius{ -7876.f, -62.f, 652.f };
// 1,500 units south of the trigger: inside its sphere, about 1,860 off the
// mark and 1,770 off the wall, far past the activation reach
const NiPoint3 kFarInTheSphere{ -7601.f, -154.f - 1500.f, 674.f };
// 2,400 units north of the trigger: outside its sphere
const NiPoint3 kOutOfTheSphere{ -7601.f, -254.f + 2500.f, 674.f };

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

// the reader's own movement update at `pos` in the sanctum, the record put
// there first so the movement rules spend nothing on the step
void WalkTo(PartOne& p, const NiPoint3& pos)
{
  auto& reader = p.worldState.GetFormAt<MpActor>(kReader);
  reader.SetPos(pos);
  auto j = jMovement;
  j["idx"] = reader.GetIdx();
  j["data"]["worldOrCell"] = kSanctum;
  j["data"]["pos"] = nlohmann::json::array({ pos.x, pos.y, pos.z });
  DoMessage(p, 0, j);
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
  Enter(p, kByTheWord, kSanctum);
  auto& reader = p.worldState.GetFormAt<MpActor>(kReader);
  REQUIRE_FALSE(reader.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  ReportWall(p, kTrigger);
  REQUIRE(reader.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  REQUIRE(reader.GetShoutRecord().wordsUnlocked.empty());
  REQUIRE(TeachWordsTold(p) == 1);
  // thuum tracker #166: the trigger's script names its mark, and the
  // record sets no radius, so the script's own 300 stands
  const auto& triggerForm = p.worldState.LookupFormById(kTrigger);
  REQUIRE(triggerForm);
  REQUIRE(triggerForm->AsObjectReference());
  const auto wordWall = p.WordOfWall(*triggerForm->AsObjectReference());
  REQUIRE(wordWall);
  REQUIRE(wordWall->wall == kWallItself);
  REQUIRE(wordWall->lookTarget == kMark);
  REQUIRE(wordWall->innerRadius == 300);
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
          "the word, arms the wall; the player's movement under the "
          "script's innerRadius of the word's mark teaches it once",
          "[WordWall][espm]")
{
  // thuum tracker #166: playtest 55 taught both seats about 2,000 units
  // out, where the game's own script only starts the chant
  PartOne& p = GetPartOne();
  Enter(p, kFarInTheSphere, kSanctum);
  auto& reader = p.worldState.GetFormAt<MpActor>(kReader);
  ReportWall(p, kTrigger);
  REQUIRE_FALSE(reader.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  REQUIRE(TeachWordsTold(p) == 0);
  // steps still short of the word: armed, nothing taught
  WalkTo(p, kShortOfTheWord);
  WalkTo(p, kByTheWallNotTheWord);
  REQUIRE_FALSE(reader.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  REQUIRE(TeachWordsTold(p) == 0);
  // under the radius: the word, once
  WalkTo(p, kUnderTheRadius);
  REQUIRE(reader.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  REQUIRE(TeachWordsTold(p) == 1);
  // the arm is spent: another step there tells the game nothing more
  WalkTo(p, kByTheWord);
  REQUIRE(reader.GetShoutRecord().wordsKnown.size() == 1);
  REQUIRE(TeachWordsTold(p) == 1);
  Leave(p);
}

TEST_CASE("A word wall's arm ends when the player leaves the trigger's "
          "sphere or disconnects; coming close then teaches nothing",
          "[WordWall][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, kFarInTheSphere, kSanctum);
  auto& reader = p.worldState.GetFormAt<MpActor>(kReader);
  ReportWall(p, kTrigger);
  WalkTo(p, kOutOfTheSphere);
  WalkTo(p, kUnderTheRadius);
  REQUIRE_FALSE(reader.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  REQUIRE(TeachWordsTold(p) == 0);
  Leave(p);
  // armed again, then gone with the connection
  Enter(p, kFarInTheSphere, kSanctum);
  ReportWall(p, kTrigger);
  Leave(p);
  Enter(p, kFarInTheSphere, kSanctum);
  auto& back = p.worldState.GetFormAt<MpActor>(kReader);
  WalkTo(p, kUnderTheRadius);
  REQUIRE_FALSE(back.HasShoutEntry(MpActor::ShoutEntry::WordKnown, kWord));
  REQUIRE(TeachWordsTold(p) == 0);
  Leave(p);
}

TEST_CASE("A word wall report from afar, from another cell, naming the wall "
          "itself or no reference teaches nothing",
          "[WordWall][espm]")
{
  PartOne& p = GetPartOne();
  // outside the trigger's sphere (a box of the same extents would still
  // hold it on this heading, which is why the shape is read), and far past
  // the wall's reach
  Enter(p, kOutOfTheSphere, kSanctum);
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
