#include "ActionListener.h"
#include "ActorValuesMessage.h"
#include "ConsoleCommandMessage.h"
#include "MpObjectReference.h"
#include "MsgType.h"
#include "OnEquipMessage.h"
#include "TestUtils.hpp"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <cmath>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

PartOne& GetPartOne();

// thuum docs/verbs/leveling.md (M5 Progression): a player's skill uses run
// the game's formulas on the server. The constants come from the load
// order's AVIF records (lab/esm.py, 2026-10-31: One-handed 6.3, 0, 2, 0 in
// Skyrim.esm; Smithing 160, 0, 0.25, 300 in Skyrim.esm, overridden by
// Update.esm to 1, 0, 0.25, 300, which the browser returns and UESP's table
// shows), the step from level 15 costs 2 * 15^1.95 (about 392.6) for
// One-handed, a use of a base of 10 earns 63, so the seventh hit takes the
// skill to 16 and the character 16 toward its first 100.

namespace {
constexpr uint32_t kActor = 0xff000fd1;
constexpr uint32_t kActorNoRecord = 0xff000fd2;
constexpr uint32_t kTamriel = 0x0000003c;
constexpr uint8_t kOneHanded = 6;
constexpr uint8_t kSmithing = 10;

MpActor& Player(PartOne& p, uint32_t actorId)
{
  DoConnect(p, 0);
  p.CreateActor(actorId, { 0, 0, 0 }, 0, kTamriel);
  p.SetUserActor(0, actorId);
  p.Messages().clear();
  return p.worldState.GetFormAt<MpActor>(actorId);
}

// the player's first actor values report: One-handed at 15 with no progress
void FirstReport(PartOne& p)
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::ActorValues) },
                    { "bases",
                      nlohmann::json::array(
                        { { { "av", kOneHanded }, { "base", 15.f } } }) },
                    { "skills",
                      nlohmann::json::array({ { { "skill", 0 },
                                                { "level", 15.f },
                                                { "xp", 0.f },
                                                { "threshold", 392.6f } } }) },
                    { "xp", 0.f },
                    { "threshold", 100.f },
                    { "level", 1 },
                    { "legendary", nlohmann::json::array() } });
}

void Use(PartOne& p, uint8_t skill, float amount)
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SkillUse) },
                    { "uses",
                      nlohmann::json::array(
                        { { { "skill", skill }, { "amount", amount } } }) } });
}

std::vector<ActorValuesMessage> RecordsTo(PartOne& p, Networking::UserId user)
{
  std::vector<ActorValuesMessage> out;
  for (auto& m : p.Messages()) {
    auto values = dynamic_cast<ActorValuesMessage*>(m.message.get());
    if (m.userId == user && values) {
      out.push_back(*values);
    }
  }
  return out;
}

void Leave(PartOne& p, uint32_t actorId)
{
  p.SetLevelingEnabled(false);
  DoDisconnect(p, 0);
  p.DestroyActor(actorId);
}
}

TEST_CASE("The skill table is read from the game files at start",
          "[Leveling][espm]")
{
  PartOne& p = GetPartOne();
  REQUIRE(p.SkillUseXp(kOneHanded, 10.f) == Catch::Approx(63.f));
  // Update.esm's override of Smithing wins over Skyrim.esm's 160
  REQUIRE(p.SkillUseXp(kSmithing, 1.f) == Catch::Approx(1.f));
  REQUIRE(p.SkillLevelCost(kOneHanded, 15) ==
          Catch::Approx(2.f * std::pow(15.f, 1.95f)).margin(0.01));
  REQUIRE(p.SkillLevelCost(kSmithing, 15) ==
          Catch::Approx(0.25f * std::pow(15.f, 1.95f) + 300.f).margin(0.01));
  // no skill: nothing
  REQUIRE(p.SkillUseXp(5, 10.f) == 0.f);
}

TEST_CASE("Seven One-handed uses of a base of 10 take the record from 15 to "
          "16, the base with it, and the record goes to the player's game; "
          "with leveling off a use changes nothing",
          "[Leveling][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p, kActor);
  FirstReport(p);
  REQUIRE(ac.GetActorValueRecord().has_value());
  p.SetLevelingEnabled(true);
  p.Messages().clear();

  for (int hit = 1; hit <= 6; ++hit) {
    Use(p, kOneHanded, 10.f);
  }
  auto record = ac.GetActorValueRecord();
  REQUIRE(record.has_value());
  REQUIRE(record->skills.size() == 1);
  REQUIRE(record->skills[0].level == 15.f);
  REQUIRE(record->skills[0].xp == Catch::Approx(378.f));
  REQUIRE(record->level == 1);
  // no level moved yet: nothing sent
  REQUIRE(RecordsTo(p, 0).empty());

  Use(p, kOneHanded, 10.f);
  record = ac.GetActorValueRecord();
  REQUIRE(record->skills[0].level == 16.f);
  REQUIRE(record->skills[0].xp ==
          Catch::Approx(441.f - 2.f * std::pow(15.f, 1.95f)).margin(0.01));
  REQUIRE(record->skills[0].threshold ==
          Catch::Approx(2.f * std::pow(16.f, 1.95f)).margin(0.01));
  REQUIRE(record->bases.size() == 1);
  REQUIRE(record->bases[0].first == kOneHanded);
  REQUIRE(record->bases[0].second == 16.f);
  REQUIRE(record->level == 1);
  REQUIRE(record->xp == Catch::Approx(16.f));
  REQUIRE(record->threshold == Catch::Approx(100.f));
  const auto sent = RecordsTo(p, 0);
  REQUIRE(sent.size() == 1);
  REQUIRE(sent[0].skills.size() == 1);
  REQUIRE(sent[0].skills[0].level == 16.f);
  REQUIRE(sent[0].bases.size() == 1);
  REQUIRE(sent[0].bases[0].base == 16.f);

  // a refused use (no skill at ActorValue 5) changes nothing
  p.Messages().clear();
  Use(p, 5, 10.f);
  REQUIRE(ac.GetActorValueRecord() == record);

  // the switch off: dropped whole
  p.SetLevelingEnabled(false);
  Use(p, kOneHanded, 10.f);
  REQUIRE(ac.GetActorValueRecord() == record);
  REQUIRE(RecordsTo(p, 0).empty());
  Leave(p, kActor);
}

TEST_CASE("A use before the player's first actor values report is dropped",
          "[Leveling][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p, kActorNoRecord);
  p.SetLevelingEnabled(true);
  Use(p, kOneHanded, 10.f);
  REQUIRE(!ac.GetActorValueRecord().has_value());
  REQUIRE(RecordsTo(p, 0).empty());
  Leave(p, kActorNoRecord);
}

TEST_CASE("A server set of a skill (the console's SetAV) moves the progress "
          "entry with the base, and the skill's next use continues from the "
          "set level instead of snapping back",
          "[Leveling][espm]")
{
  PartOne& p = GetPartOne();
  p.worldState.enableConsoleCommandsForAll = true;
  auto& ac = Player(p, kActor);
  FirstReport(p);
  p.SetLevelingEnabled(true);
  p.Messages().clear();

  RawMessageData msgData;
  msgData.userId = 0;
  ConsoleCommandMessage msg;
  msg.data.commandName = "setav";
  msg.data.args = { int64_t(0x14), std::string("OneHanded"), int64_t(50) };
  p.GetActionListener().OnConsoleCommand(msgData, msg);

  auto record = ac.GetActorValueRecord();
  REQUIRE(record.has_value());
  REQUIRE(record->skills.size() == 1);
  REQUIRE(record->skills[0].skill == 0);
  REQUIRE(record->skills[0].level == 50.f);
  REQUIRE(record->skills[0].xp == 0.f);
  REQUIRE(record->skills[0].threshold ==
          Catch::Approx(2.f * std::pow(50.f, 1.95f)).margin(0.01));
  REQUIRE(record->bases ==
          std::vector<std::pair<uint8_t, float>>{ { kOneHanded, 50.f } });
  // the record went to the player's game whole: the base and the progress
  const auto sent = RecordsTo(p, 0);
  REQUIRE(sent.size() == 1);
  REQUIRE(sent[0].skills.size() == 1);
  REQUIRE(sent[0].skills[0].level == 50.f);
  REQUIRE(sent[0].skills[0].xp == 0.f);
  REQUIRE(sent[0].bases.size() == 1);
  REQUIRE(sent[0].bases[0].base == 50.f);

  // the next use continues from 50: a base of 10 earns 63 toward the step
  // from 50 (about 4,088), the level and the base stay 50 (before this the
  // use recomputed the base from the progress at 15 and the set snapped back)
  p.Messages().clear();
  Use(p, kOneHanded, 10.f);
  record = ac.GetActorValueRecord();
  REQUIRE(record->skills[0].level == 50.f);
  REQUIRE(record->skills[0].xp == Catch::Approx(63.f));
  REQUIRE(ac.GetRecordedActorValueBase(kOneHanded) ==
          std::optional<float>(50.f));
  REQUIRE(RecordsTo(p, 0).empty());

  p.worldState.enableConsoleCommandsForAll = false;
  Leave(p, kActor);
}

namespace {
// Skyrim.esm's One-handed skill books (lab/esm.py find BOOK SkillOneHanded,
// 2026-11-02: DATA flags 0x01 TeachesSkill, teaches 6 the ActorValue)
constexpr uint32_t kOneHandedBook = 0x1afe3;  // SkillOneHanded1
constexpr uint32_t kOneHandedBook2 = 0x1afd9; // SkillOneHanded2

// the seat's read of a book from its inventory arrives as the equip report
// (MpActor::OnEquip, ReadBookEvent), as a spell tome's does
void Read(PartOne& p, uint32_t bookId)
{
  RawMessageData raw;
  raw.userId = 0;
  OnEquipMessage msg;
  msg.baseId = bookId;
  p.GetActionListener().OnEquip(raw, msg);
}
}

TEST_CASE("A skill book read takes its skill one level and the base, carries "
          "the bar's share, credits the character, keeps the book and sends "
          "the record; the same book read again moves nothing and re-sends "
          "the record; the read rides the change form; with leveling off a "
          "read is dropped and the book is not spent",
          "[Leveling][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p, kActor);
  FirstReport(p);
  p.SetLevelingEnabled(true);
  // six hits first: 378 of the 392.6 the step from 15 costs, so the book's
  // step carries the bar's share (378 / 392.6 of the step from 16)
  for (int hit = 1; hit <= 6; ++hit) {
    Use(p, kOneHanded, 10.f);
  }
  ac.AddItem(kOneHandedBook, 1);
  p.Messages().clear();

  Read(p, kOneHandedBook);
  auto record = ac.GetActorValueRecord();
  REQUIRE(record.has_value());
  REQUIRE(record->skills.size() == 1);
  REQUIRE(record->skills[0].level == 16.f);
  const float cost15 = p.SkillLevelCost(kOneHanded, 15);
  const float cost16 = p.SkillLevelCost(kOneHanded, 16);
  REQUIRE(record->skills[0].xp ==
          Catch::Approx(378.f / cost15 * cost16).margin(0.05));
  REQUIRE(record->skills[0].threshold == Catch::Approx(cost16).margin(0.01));
  REQUIRE(record->bases ==
          std::vector<std::pair<uint8_t, float>>{ { kOneHanded, 16.f } });
  REQUIRE(record->level == 1);
  REQUIRE(record->xp == Catch::Approx(16.f));
  REQUIRE(ac.IsBookRead(kOneHandedBook));
  // the book stays: a spell tome goes, a skill book is read in place
  REQUIRE(ac.GetInventory().GetItemCount(kOneHandedBook) == 1);
  // the record went to the player's game whole
  const auto sent = RecordsTo(p, 0);
  REQUIRE(sent.size() == 1);
  REQUIRE(sent[0].bases.size() == 1);
  REQUIRE(sent[0].bases[0].base == 16.f);
  REQUIRE(sent[0].skills.size() == 1);
  REQUIRE(sent[0].skills[0].level == 16.f);

  // the same book again: nothing moves, the record goes back to the game
  // (its engine raised the skill on its own when the book opened)
  p.Messages().clear();
  Read(p, kOneHandedBook);
  record = ac.GetActorValueRecord();
  REQUIRE(record->skills[0].level == 16.f);
  REQUIRE(record->xp == Catch::Approx(16.f));
  REQUIRE(RecordsTo(p, 0).size() == 1);
  REQUIRE(ac.GetInventory().GetItemCount(kOneHandedBook) == 1);

  // the change form carries the read (readBooks, persisted with the record)
  REQUIRE(ac.GetChangeForm().readBooks.GetReadBooks() ==
          std::vector<uint32_t>{ kOneHandedBook });

  // leveling off: another book's read is dropped and that book is not spent
  p.SetLevelingEnabled(false);
  ac.AddItem(kOneHandedBook2, 1);
  p.Messages().clear();
  Read(p, kOneHandedBook2);
  record = ac.GetActorValueRecord();
  REQUIRE(record->skills[0].level == 16.f);
  REQUIRE(!ac.IsBookRead(kOneHandedBook2));
  REQUIRE(RecordsTo(p, 0).empty());

  Leave(p, kActor);
}

// thuum docs/verbs/books.md, S2 (fork m4-book-read 2255e2d4's merge
// condition): a player's activation of a world book that teaches no spell is
// answered with the open (BookActivationTest), and the open reads the book as
// the engine reads it at its Book Menu's open (TESObjectBOOK::Read at
// BookMenu::OpenBookMenu's end, docs/verbs/leveling.md, HYPOTHESIS): the
// inventory read's skill branch (ReadBookEvent), once a book a character
// (readBooks), its record held for the open's end (the close or the Take),
// since the reader's engine raises the skill itself after the open reply.
// The game files as lab/esm.py reads Skyrim.esm (2026-11-14 PST):
// DawnstarBrinasHouse 0x13a87 holds REFR 0x10859a of BOOK 0x1afe3
// SkillOneHanded1 (DATA flags 0x01 TeachesSkill, teaches 6) at (-1203.2,
// -450.8, 11.2), beside PeltFloor01 0x0e0463 at (-1095.4, -618.1, 0.5);
// MorthalMoorsideInn 0x138ce holds REFR 0x10544d of BOOK 0xed63f
// Book2CommonWatcherofStones (DATA flags 0x00) at (943.6, -723.3, 74.6),
// beside PeltFloor02 0x0e00f0 at (1061, -721, 1) (BookActivationTest's).
namespace {
constexpr uint32_t kBrinasHouse = 0x13a87;
constexpr uint32_t kWorldOneHandedBook = 0x10859a; // base kOneHandedBook
constexpr uint32_t kInn = 0x138ce;
constexpr uint32_t kWatcher = 0x10544d;
constexpr uint32_t kWatcherBase = 0xed63f;
// on the pelts, about 200 units from the skill book, and on the inn's rug,
// 116 to 140 from its books: inside the activation reach
const NiPoint3 kByTheSkillBook{ -1095.f, -618.f, 0.f };
const NiPoint3 kOnTheInnRug{ 1060.f, -722.f, 0.f };

// the player (user 0, kActor) in `cell` at `at`, its bag empty
MpActor& ReaderIn(PartOne& p, uint32_t cell, const NiPoint3& at)
{
  DoConnect(p, 0);
  p.CreateActor(kActor, at, 0, cell);
  p.SetUserActor(0, kActor);
  auto& actor = p.worldState.GetFormAt<MpActor>(kActor);
  actor.RemoveAllItems();
  p.Tick(); // RemoveAllItems' deferred SetInventory
  p.Messages().clear();
  return actor;
}

// the player's own activation of `target`, as its game sends it: the open
// or the Take (`close` false), the Book Menu's close (`close` true)
void ActivateBook(PartOne& p, uint32_t target, bool close = false)
{
  DoMessage(p, 0,
            nlohmann::json{ { "t", MsgType::Activate },
                            { "data",
                              { { "caster", 0x14 },
                                { "target", target },
                                { "isSecondActivation", close } } } });
  p.Tick(); // the deferred inventory
}

size_t OpensSent(PartOne& p, uint32_t target)
{
  return std::count_if(
    p.Messages().begin(), p.Messages().end(), [&](PartOne::Message& m) {
      return m.userId == 0 && m.j.is_object() && m.j.contains("t") &&
        m.j["t"] == static_cast<int>(MsgType::OpenContainer) &&
        m.j.contains("target") && m.j["target"] == target;
    });
}

// the default logger's lines at info and above while the guard lives
// (PropTest's shape), restored at its end
struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("leveling-test", sink);
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

const std::string kRaisedAtOpen =
  "[verb:leveling] ff000fd1 reads skill book 1afe3: skill 6 one level up; "
  "the record goes to its game at its open's end";
const std::string kAgainAtOpen =
  "[verb:leveling] ff000fd1 reads skill book 1afe3 again (skill 6): read "
  "before, nothing; the record goes back to its game at its open's end";
const std::string kRecordAtEnd = "[verb:books] ff000fd1 ends its open of "
                                 "book 10859a: the skill book's record goes "
                                 "to its game";
}

TEST_CASE("A world skill book's open raises its skill once, the record going "
          "to the player's game at the menu's close; a second open raises "
          "nothing and sends the record again at its close",
          "[Leveling][Books][espm]")
{
  PartOne& p = GetPartOne();
  auto& book = p.worldState.GetFormAt<MpObjectReference>(kWorldOneHandedBook);
  REQUIRE(book.GetBaseId() == kOneHandedBook);
  auto& ac = ReaderIn(p, kBrinasHouse, kByTheSkillBook);
  FirstReport(p);
  p.SetLevelingEnabled(true);
  p.Messages().clear();
  LogCapture log;

  // the open: the skill one level up on the server, the book read, the
  // record held (its engine raises the skill after this reply)
  ActivateBook(p, kWorldOneHandedBook);
  REQUIRE(OpensSent(p, kWorldOneHandedBook) == 1);
  REQUIRE(ac.GetOpenBook() == kWorldOneHandedBook);
  auto record = ac.GetActorValueRecord();
  REQUIRE(record.has_value());
  REQUIRE(record->skills.size() == 1);
  REQUIRE(record->skills[0].level == 16.f);
  REQUIRE(record->skills[0].xp == Catch::Approx(0.f).margin(0.001));
  REQUIRE(record->bases ==
          std::vector<std::pair<uint8_t, float>>{ { kOneHanded, 16.f } });
  REQUIRE(record->level == 1);
  REQUIRE(record->xp == Catch::Approx(16.f));
  REQUIRE(ac.IsBookRead(kOneHandedBook));
  REQUIRE(RecordsTo(p, 0).empty());
  REQUIRE(log.Count(kRaisedAtOpen) == 1);
  REQUIRE(log.Count(kRecordAtEnd) == 0);
  // nothing taken
  REQUIRE(!book.IsHarvested());
  REQUIRE(ac.GetInventory().GetItemCount(kOneHandedBook) == 0);

  // the close: the record goes, the server's 16
  p.Messages().clear();
  ActivateBook(p, kWorldOneHandedBook, true);
  REQUIRE(ac.GetOpenBook() == 0);
  auto sent = RecordsTo(p, 0);
  REQUIRE(sent.size() == 1);
  REQUIRE(sent[0].bases.size() == 1);
  REQUIRE(sent[0].bases[0].base == 16.f);
  REQUIRE(sent[0].skills.size() == 1);
  REQUIRE(sent[0].skills[0].level == 16.f);
  REQUIRE(log.Count(kRecordAtEnd) == 1);

  // the second open: read before, nothing moves, the record held again
  p.Messages().clear();
  ActivateBook(p, kWorldOneHandedBook);
  REQUIRE(OpensSent(p, kWorldOneHandedBook) == 1);
  record = ac.GetActorValueRecord();
  REQUIRE(record->skills[0].level == 16.f);
  REQUIRE(record->xp == Catch::Approx(16.f));
  REQUIRE(RecordsTo(p, 0).empty());
  REQUIRE(log.Count(kRaisedAtOpen) == 1);
  REQUIRE(log.Count(kAgainAtOpen) == 1);

  // its close: the record again (a seat whose engine raised the skill once
  // more goes back to 16), still 16
  p.Messages().clear();
  ActivateBook(p, kWorldOneHandedBook, true);
  sent = RecordsTo(p, 0);
  REQUIRE(sent.size() == 1);
  REQUIRE(sent[0].skills[0].level == 16.f);
  REQUIRE(log.Count(kRecordAtEnd) == 2);
  REQUIRE(ac.GetChangeForm().readBooks.GetReadBooks() ==
          std::vector<uint32_t>{ kOneHandedBook });

  Leave(p, kActor);
}

TEST_CASE("A skill book read from the inventory raises nothing when a world "
          "copy of it is opened; the copy's Take sends the record and takes "
          "the book",
          "[Leveling][Books][espm]")
{
  PartOne& p = GetPartOne();
  auto& book = p.worldState.GetFormAt<MpObjectReference>(kWorldOneHandedBook);
  auto& ac = ReaderIn(p, kBrinasHouse, kByTheSkillBook);
  FirstReport(p);
  p.SetLevelingEnabled(true);
  ac.AddItem(kOneHandedBook, 1);
  Read(p, kOneHandedBook);
  REQUIRE(ac.IsBookRead(kOneHandedBook));
  REQUIRE(ac.GetActorValueRecord()->skills[0].level == 16.f);
  p.Messages().clear();
  LogCapture log;

  ActivateBook(p, kWorldOneHandedBook);
  REQUIRE(OpensSent(p, kWorldOneHandedBook) == 1);
  auto record = ac.GetActorValueRecord();
  REQUIRE(record->skills[0].level == 16.f);
  REQUIRE(record->xp == Catch::Approx(16.f));
  REQUIRE(RecordsTo(p, 0).empty());
  REQUIRE(log.Count(kAgainAtOpen) == 1);
  REQUIRE(log.Count(kRaisedAtOpen) == 0);

  // the Take: the record at the open's end, the book into the bag
  p.Messages().clear();
  ActivateBook(p, kWorldOneHandedBook);
  REQUIRE(OpensSent(p, kWorldOneHandedBook) == 0);
  const auto sent = RecordsTo(p, 0);
  REQUIRE(sent.size() == 1);
  REQUIRE(sent[0].skills[0].level == 16.f);
  REQUIRE(log.Count(kRecordAtEnd) == 1);
  REQUIRE(book.IsHarvested());
  REQUIRE(ac.GetInventory().GetItemCount(kOneHandedBook) == 2);
  REQUIRE(ac.GetOpenBook() == 0);
  REQUIRE(ac.GetActorValueRecord()->skills[0].level == 16.f);

  Leave(p, kActor);
}

TEST_CASE("A world book that teaches no skill raises nothing at its open and "
          "sends no record at its close",
          "[Leveling][Books][espm]")
{
  PartOne& p = GetPartOne();
  auto& book = p.worldState.GetFormAt<MpObjectReference>(kWatcher);
  REQUIRE(book.GetBaseId() == kWatcherBase);
  auto& ac = ReaderIn(p, kInn, kOnTheInnRug);
  FirstReport(p);
  p.SetLevelingEnabled(true);
  p.Messages().clear();
  LogCapture log;

  ActivateBook(p, kWatcher);
  REQUIRE(OpensSent(p, kWatcher) == 1);
  REQUIRE(ac.GetOpenBook() == kWatcher);
  auto record = ac.GetActorValueRecord();
  REQUIRE(record->skills.size() == 1);
  REQUIRE(record->skills[0].level == 15.f);
  REQUIRE(record->xp == Catch::Approx(0.f).margin(0.001));
  REQUIRE(!ac.IsBookRead(kWatcherBase));
  REQUIRE(RecordsTo(p, 0).empty());
  REQUIRE(log.Count("[verb:books] ff000fd1 reads book ed63f: no skill book, "
                    "nothing to raise") == 1);

  p.Messages().clear();
  ActivateBook(p, kWatcher, true);
  REQUIRE(ac.GetOpenBook() == 0);
  REQUIRE(RecordsTo(p, 0).empty());
  REQUIRE(log.Count("ends its open of book") == 0);
  REQUIRE(ac.GetChangeForm().readBooks.GetReadBooks().empty());

  Leave(p, kActor);
}
