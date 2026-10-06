#include "ActionListener.h"
#include "IngredientEffectsKnownMessage.h"
#include "MpChangeForms.h"
#include "OnEquipMessage.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include <catch2/catch_all.hpp>
#include <simdjson.h>

PartOne& GetPartOne();

// thuum docs/verbs/learned-effects.md: after a player eats an ingredient, its
// client reports the effects its engine now knows of it; the server keeps
// the report only after an eat it saw, the record only grows, and after every
// login the server teaches the recorded effects again (Ingredient.LearnEffect
// snippets, on the first movement).

namespace {
constexpr uint32_t kActor = 0xff000abe;

// Skyrim.esm INGR 0x0004B0BA "Wheat" (lab/esm.py, 2026-10-05)
constexpr uint32_t kWheat = 0x0004b0ba;

MpActor& Player(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kActor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kActor);
  p.Messages().clear();
  return p.worldState.GetFormAt<MpActor>(kActor);
}

void Eat(PartOne& p, MpActor& ac, uint32_t baseId)
{
  ac.AddItem(baseId, 1);
  RawMessageData raw;
  raw.userId = 0;
  OnEquipMessage msg;
  msg.baseId = baseId;
  p.GetActionListener().OnEquip(raw, msg);
}

void Report(PartOne& p, uint32_t ingredient, uint8_t mask)
{
  RawMessageData raw;
  raw.userId = 0;
  IngredientEffectsKnownMessage msg;
  msg.ingredient = ingredient;
  msg.mask = mask;
  p.GetActionListener().OnIngredientEffectsKnown(raw, msg);
}

uint8_t Recorded(PartOne& p, MpActor& ac, uint32_t ingredient)
{
  for (const auto& entry : ac.GetIngredientEffects()) {
    if (entry.ingredient.ToFormId(p.worldState.espmFiles) == ingredient) {
      return entry.mask;
    }
  }
  return 0;
}

// The player's own movement report, standing at the origin
void Stand(PartOne& p)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = 0;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateMovementMessage msg;
  msg.idx = 0;
  msg.data.pos = { 0, 0, 0 };
  msg.data.rot = { 0, 0, 0 };
  msg.data.isInJumpState = false;
  msg.data.isWeapDrawn = false;
  msg.data.isBlocking = false;
  msg.data.worldOrCell = 0x3c;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
}

// The LearnEffect snippets user 0 received since the last clear, as
// (ingredient, effect index)
std::vector<std::pair<uint32_t, int>> Taught(PartOne& p)
{
  p.Tick(); // snippets are deferred
  std::vector<std::pair<uint32_t, int>> out;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::SpSnippet &&
        m.j["class"] == "Ingredient" && m.j["function"] == "LearnEffect") {
      out.emplace_back(m.j["selfId"].get<uint32_t>(),
                       static_cast<int>(m.j["arguments"].at(0).get<double>()));
    }
  }
  return out;
}

void Leave(PartOne& p)
{
  p.DestroyActor(kActor);
  DoDisconnect(p, 0);
}
}

TEST_CASE("A report after eating an ingredient records its known effects, "
          "and every login teaches them again",
          "[LearnedEffects]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p);

  Eat(p, ac, kWheat);
  Report(p, kWheat, 0b0001);
  REQUIRE(Recorded(p, ac, kWheat) == 0b0001);

  // a later eat that teaches the second effect adds it
  Eat(p, ac, kWheat);
  Report(p, kWheat, 0b0011);
  REQUIRE(Recorded(p, ac, kWheat) == 0b0011);

  // nothing is taught until a login, and a login waits for the first
  // movement
  REQUIRE(Taught(p).empty());
  p.SetUserActor(0, kActor);
  p.Messages().clear();
  REQUIRE(Taught(p).empty());
  Stand(p);
  REQUIRE(
    Taught(p) ==
    std::vector<std::pair<uint32_t, int>>{ { kWheat, 0 }, { kWheat, 1 } });

  Leave(p);
}

TEST_CASE("A report without an eat of that ingredient records nothing, and "
          "the record never shrinks",
          "[LearnedEffects]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p);

  // no eat at all
  Report(p, kWheat, 0b0001);
  REQUIRE(Recorded(p, ac, kWheat) == 0);

  // a report about another form than the one eaten, and one that is no
  // ingredient
  Eat(p, ac, kWheat);
  Report(p, kWheat + 1, 0b0001);
  REQUIRE(ac.GetIngredientEffects().empty());

  // a report with fewer effects than recorded changes nothing
  Report(p, kWheat, 0b0011);
  REQUIRE(Recorded(p, ac, kWheat) == 0b0011);
  Eat(p, ac, kWheat);
  Report(p, kWheat, 0b0001);
  REQUIRE(Recorded(p, ac, kWheat) == 0b0011);

  Leave(p);
}

TEST_CASE("The change form keeps a player's ingredient effects, and older "
          "records read as none",
          "[LearnedEffects]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;
  changeForm.ingredientEffects = std::vector<IngredientEffects>{
    { FormDesc::FromString("4b0ba:Skyrim.esm"), 0b0101 },
    { FormDesc::FromString("1234:Dawnguard.esm"), 0b1000 }
  };

  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(element).ingredientEffects ==
          changeForm.ingredientEffects);

  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("ingredientEffects"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement).ingredientEffects);
}
