#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

PartOne& GetPartOne();

// thuum ADR-068 rung 4 (amendment B, read 8; docs/research/
// rung4-state-rows-2026-11-07.md section 1(c), the created half; H-AF0): an
// alias that creates its reference in another alias's inventory puts one
// item there when its quest fills, which no seat reports (a template-running
// quest fills without ForceRefTo), so the server adds it, R0 from the game
// files, once. Skyrim.esm as lab/esm.py read it on sky-srv (2026-11-06; no
// later master overrides these): QUST 0x39645 MS13, start-game-enabled (DNAM
// main flags 0x11), aliases GoldenClaw 11 (ALCO MISC 0x39647
// MS13GoldenClaw) and ArvelsJournal 16 (ALCO 0x39654), both ALCA 0x80000001
// (in alias 1), and alias 1 Arvel, ALUA NPC_ 0x39646 MS13Arvel, whose one
// ACHR is 0x39655 in Bleak Falls Temple 0x371de (record flags 0x400:
// persistent, enabled at start). So Arvel holds one claw and one journal at
// his first attach, and the journal's marks keep a second attach, a restart
// and a take from adding either again.

namespace {
constexpr uint32_t kMs13 = 0x39645;
constexpr uint32_t kArvel = 0x39655;
constexpr uint32_t kArvelBase = 0x39646;
constexpr uint32_t kClaw = 0x39647;
constexpr uint32_t kJournal = 0x39654;

MpActor& Attach(PartOne& p)
{
  const auto& form = p.worldState.LookupFormById(kArvel);
  REQUIRE(form);
  MpActor* arvel = form->AsActor();
  REQUIRE(arvel);
  REQUIRE(arvel->GetBaseId() == kArvelBase);
  return *arvel;
}

std::optional<MpChangeFormREFR::AliasFill> MarkOf(PartOne& p, uint32_t alias)
{
  for (const auto& fill : p.GetAliasFills()) {
    if (fill.quest == FormDesc::FromFormId(kMs13, p.worldState.espmFiles) &&
        fill.alias == alias) {
      return fill;
    }
  }
  return std::nullopt;
}

// a restart: the world's journal and Arvel's record loaded as the save holds
// them, Arvel attached again from the game files with his record applied
MpActor& Restart(PartOne& p, const MpChangeForm& journal,
                 const MpChangeForm& arvel)
{
  p.worldState.npcEnabled = true;
  p.worldState.LoadChangeForm(journal, p.CreateFormCallbacks());
  p.worldState.LoadChangeForm(arvel, p.CreateFormCallbacks());
  return Attach(p);
}
}

TEST_CASE("MS13's created aliases put the Golden Claw and Arvel's journal in "
          "Arvel once, across a second attach and a restart, and never again "
          "after a take",
          "[QuestStage][AliasFill][espm]")
{
  MpChangeForm journal;
  MpChangeForm saved;
  {
    auto& p = GetPartOne();
    p.worldState.npcEnabled = true;
    MpActor& arvel = Attach(p);
    REQUIRE(arvel.GetInventory().GetItemCount(kClaw) == 1);
    REQUIRE(arvel.GetInventory().GetItemCount(kJournal) == 1);
    // the journal's marks: the created base, the container, added
    const auto claw = MarkOf(p, 11);
    REQUIRE(claw.has_value());
    REQUIRE(claw->created ==
            FormDesc::FromFormId(kClaw, p.worldState.espmFiles));
    REQUIRE(claw->container ==
            FormDesc::FromFormId(kArvel, p.worldState.espmFiles));
    REQUIRE(claw->added);
    REQUIRE(!claw->refr.has_value());
    const auto book = MarkOf(p, 16);
    REQUIRE(book.has_value());
    REQUIRE(book->added);
    // a second attach of the same actor adds nothing
    REQUIRE(p.AddCreatedItems(arvel) == 0);
    REQUIRE(arvel.GetInventory().GetItemCount(kClaw) == 1);
    // nor does MS13's first recorded stage, its containers attached
    REQUIRE(p.AddCreatedItemsOfQuest(kMs13) == 0);
    REQUIRE(arvel.GetInventory().GetItemCount(kClaw) == 1);
    journal = p.WorldJournal().GetChangeForm();
    saved = arvel.GetChangeForm();
  }

  // a restart keeps one
  {
    auto& p = GetPartOne();
    MpActor& arvel = Restart(p, journal, saved);
    REQUIRE(arvel.GetInventory().GetItemCount(kClaw) == 1);
    REQUIRE(arvel.GetInventory().GetItemCount(kJournal) == 1);
    // the take: a player loots the claw
    arvel.RemoveItem(kClaw, 1, nullptr);
    REQUIRE(arvel.GetInventory().GetItemCount(kClaw) == 0);
    journal = p.WorldJournal().GetChangeForm();
    saved = arvel.GetChangeForm();
  }

  // and after the take, a restart adds none: the mark stands
  {
    auto& p = GetPartOne();
    MpActor& arvel = Restart(p, journal, saved);
    REQUIRE(arvel.GetInventory().GetItemCount(kClaw) == 0);
    REQUIRE(arvel.GetInventory().GetItemCount(kJournal) == 1);
    REQUIRE(p.AddCreatedItems(arvel) == 0);
    REQUIRE(arvel.GetInventory().GetItemCount(kClaw) == 0);
  }
}

TEST_CASE("A world with no journal mark for a created alias owes its item; "
          "one with the mark does not",
          "[QuestStage][AliasFill][espm]")
{
  // the journal marked before Arvel's attach: nothing is added
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  MpChangeFormREFR::AliasFill mark;
  mark.quest = FormDesc::FromFormId(kMs13, p.worldState.espmFiles);
  mark.alias = 11;
  mark.created = FormDesc::FromFormId(kClaw, p.worldState.espmFiles);
  mark.added = true;
  p.SetAliasFills({ mark });
  MpActor& arvel = Attach(p);
  REQUIRE(arvel.GetInventory().GetItemCount(kClaw) == 0);
  // the journal, unmarked, still owed
  REQUIRE(arvel.GetInventory().GetItemCount(kJournal) == 1);
  REQUIRE(MarkOf(p, 16).has_value());
}
