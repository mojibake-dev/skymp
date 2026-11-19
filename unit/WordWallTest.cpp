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
// thuum tracker #138: the shout the word belongs to, UnrelentingForceShout
// SHOU 0x13e07, its first word WOOP 0x13e22 (ShoutsTest.cpp's lab/esm.py
// read of Skyrim.esm, 2026-10-10; to read it again: `lab/esm.py find
// Skyrim.esm SHOU UnrelentingForce` on sky-srv, each SNAM[12]'s first four
// bytes a word; EspmTest.cpp's "SHOU variations" reads the same SNAMs); the
// case below reads it through the test's own espm before it relies on it
constexpr uint32_t kShout = 0x13e07;
// the other shouts playtest 55's records hold (tracker #138: profile 1's
// 3f9ea, profile 2's 32920): FireBreathShout 0x3f9ea (docs/verbs/shouts.md,
// lab/esm.py 2026-10-10) and BecomeEtherealShout 0x32920
// (docs/verbs/magic-effects.md's Become Ethereal row)
constexpr uint32_t kFireBreath = 0x3f9ea;
constexpr uint32_t kBecomeEthereal = 0x32920;
// a word two shouts name (read 2026-11-18 with lab/esm.py's walk over SHOU
// on the Mac's copies in lab/.cache/game; PartOne.cpp's ShoutOfWordIndexOf
// cites it): MarkedforDeathShout 0x7097c's first word 0x60297 is also the
// first word of Dawnguard's DLC1DragonDrainVitalityShout06, 0x02008442 in
// the test's load order (Dawnguard.esm third, unit/main.cpp)
constexpr uint32_t kMarkedForDeath = 0x7097c;
constexpr uint32_t kDrainVitality06 = 0x02008442;
constexpr uint32_t kSharedWord = 0x60297;
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
  // and every word of each shout playtest 55's records hold maps back to
  // that shout alone, so the login's pass records none but its own
  for (const uint32_t shout : { kShout, kFireBreath, kBecomeEthereal }) {
    CAPTURE(shout);
    const auto lookup = p.worldState.GetEspm().GetBrowser().LookupById(shout);
    REQUIRE(lookup.rec != nullptr);
    const auto words = espm::GetData<espm::SHOU>(shout, &p.worldState);
    REQUIRE(words.count == 3);
    for (uint32_t i = 0; i < words.count; ++i) {
      CAPTURE(i);
      REQUIRE(words.variations[i].word != 0);
      REQUIRE(p.ShoutOfWord(lookup.ToGlobalId(words.variations[i].word))
                .value_or(0) == shout);
    }
  }

  // by the word: a report from farther out only arms the wall (tracker #166)
  Enter(p, kByTheWord, kSanctum);
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

TEST_CASE("A word two shouts name maps to no shout (thuum tracker #138)",
          "[WordWall][espm]")
{
  PartOne& p = GetPartOne();
  // the premise, read through the test's espm: both shouts' first variation
  // names the same word
  for (const uint32_t shout : { kMarkedForDeath, kDrainVitality06 }) {
    CAPTURE(shout);
    const auto lookup = p.worldState.GetEspm().GetBrowser().LookupById(shout);
    REQUIRE(lookup.rec != nullptr);
    const auto words = espm::GetData<espm::SHOU>(shout, &p.worldState);
    REQUIRE(words.count >= 1);
    REQUIRE(lookup.ToGlobalId(words.variations[0].word) == kSharedWord);
  }
  // no record field tells the player's shout from a creature's, so the word
  // keeps none: a login's pass or a wall records nothing for it
  REQUIRE_FALSE(p.ShoutOfWord(kSharedWord).has_value());
}

TEST_CASE("A wall read of a word known without its shout records the shout "
          "and tells only the shout (thuum tracker #138)",
          "[WordWall][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, kByTheTrigger, kSanctum);
  auto& reader = p.worldState.GetFormAt<MpActor>(kReader);
  // the word known with no shout, as the console's teachword or a script's
  // Game.TeachWord records it during a session
  REQUIRE(reader.RecordShoutEntry(
    MpActor::ShoutEntry::WordKnown,
    FormDesc::FromFormId(kWord, p.worldState.espmFiles)));
  ReportWall(p, kTrigger);
  REQUIRE(reader.HasShoutEntry(MpActor::ShoutEntry::Shout, kShout));
  REQUIRE(reader.GetShoutRecord().wordsKnown.size() == 1);
  REQUIRE(reader.GetShoutRecord().shouts.size() == 1);
  REQUIRE(ShoutsAddedTold(p, kShout) == 1);
  // the word was known already: the game is not taught it again
  REQUIRE(TeachWordsTold(p) == 0);
  Leave(p);
}
