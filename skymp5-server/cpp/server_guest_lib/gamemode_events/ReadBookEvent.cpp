#include "ReadBookEvent.h"

#include "MpActor.h"
#include "SpSnippetFunctionGen.h"
#include "WorldState.h"
#include "script_objects/EspmGameObject.h"
#include <string>
#include <unordered_set>
#include <vector>

namespace {
// the skills' ActorValues, kOneHanded 6 to kEnchanting 23 (CommonLibSSE-NG
// include/RE/A/ActorValues.h)
constexpr uint32_t kFirstSkillAv = 6;
constexpr uint32_t kLastSkillAv = 23;
}

ReadBookEvent::ReadBookEvent(MpActor* actor_, uint32_t baseId_)
  : actor(actor_)
  , baseId(baseId_)
{
}

const char* ReadBookEvent::GetName() const
{
  return "onReadBook";
}

std::string ReadBookEvent::GetArgumentsJsonArray() const
{
  std::string result;
  result += "[";
  result += std::to_string(actor->GetFormId());
  result += ",";
  result += std::to_string(baseId);
  result += "]";
  return result;
}

bool ReadBookEvent::SpellLearned() const
{
  return spellLearned;
}

void ReadBookEvent::OnFireSuccess(WorldState* worldState)
{
  auto& loader = actor->GetParent()->GetEspm();
  auto bookLookupResult = loader.GetBrowser().LookupById(baseId);

  if (!bookLookupResult.rec) {
    spdlog::error("ReadBookEvent::OnFireSuccess - Actor {:x} reading book: No "
                  "book form {:x}",
                  actor->GetFormId(), baseId);
    return;
  }

  const auto bookData = espm::GetData<espm::BOOK>(baseId, actor->GetParent());
  const auto spellOrSkillFormId =
    bookLookupResult.ToGlobalId(bookData.spellOrSkillFormId);

  if (bookData.IsFlagSet(espm::BOOK::Flags::TeachesSpell)) {
    if (actor->IsSpellLearned(spellOrSkillFormId)) {
      spdlog::info("ReadBookEvent::OnFireSuccess - Actor {:x} reading book: "
                   "Spell already learned "
                   "{:x}, not spending the book",
                   actor->GetFormId(), spellOrSkillFormId);
      return;
    }
    actor->AddSpell(spellOrSkillFormId);
    spellLearned = true;
    return;
  } else if (bookData.IsFlagSet(espm::BOOK::Flags::TeachesSkill)) {
    // thuum docs/verbs/leveling.md (skill books): the DATA's teaches field
    // of a skill book is the skill's ActorValue, not a form (Skyrim.esm's
    // SkillOneHanded1 0x1afe3 carries 6, SkillSpeechcraft2 0x1b00e 17;
    // lab/esm.py, 2026-11-02), so it resolves through no plugin. One level
    // for the skill, once per book per character (the record's readBooks);
    // the increment is the server's, the record's send the seat's number
    const uint32_t skillAv = bookData.spellOrSkillFormId;
    if (skillAv < kFirstSkillAv || skillAv > kLastSkillAv) {
      spdlog::warn("[verb:leveling] E_BOOK_SKILL: {:x} reads skill book {:x} "
                   "teaching ActorValue {}, no skill; dropped",
                   actor->GetFormId(), baseId, skillAv);
      return;
    }
    if (actor->IsBookRead(baseId)) {
      spdlog::info("[verb:leveling] {:x} reads skill book {:x} again (skill "
                   "{}): read before, nothing; the record goes back to its "
                   "game",
                   actor->GetFormId(), baseId, skillAv);
      actor->SendActorValueRecord();
      return;
    }
    if (!worldState->applySkillBook) {
      spdlog::info("[verb:leveling] {:x} reads skill book {:x} (skill {}): "
                   "no leveling installed; the read stands nowhere",
                   actor->GetFormId(), baseId, skillAv);
      return;
    }
    if (worldState->applySkillBook(*actor, static_cast<uint8_t>(skillAv))) {
      actor->MarkBookRead(baseId);
      spdlog::info("[verb:leveling] {:x} reads skill book {:x}: skill {} one "
                   "level up",
                   actor->GetFormId(), baseId, skillAv);
    }
    return;
  }
}

void ReadBookEvent::OnFireBlocked(WorldState* worldState)
{
  auto& loader = actor->GetParent()->GetEspm();
  auto bookLookupResult = loader.GetBrowser().LookupById(baseId);

  const auto bookData = espm::GetData<espm::BOOK>(baseId, actor->GetParent());
  const auto spellOrSkillFormId =
    bookLookupResult.ToGlobalId(bookData.spellOrSkillFormId);

  if (!bookData.IsFlagSet(espm::BOOK::Flags::TeachesSpell)) {
    if (bookData.IsFlagSet(espm::BOOK::Flags::TeachesSkill)) {
      // the seat's own engine raised the skill when the book opened; the
      // record goes back so its number is the server's (leveling.md)
      actor->SendActorValueRecord();
    }
    return;
  }

  auto aSpell = VarValue(std::make_shared<EspmGameObject>(
    loader.GetBrowser().LookupById(spellOrSkillFormId)));

  std::vector<VarValue> arguments = { aSpell };

  if (actor->IsSpellLearned(spellOrSkillFormId)) {
    spdlog::info(
      "ReadBookEvent::OnFireBlocked - Actor {:x} reading book {:x}: "
      "Spell already learned {:x}, not executing RemoveSpell SpSnippet",
      actor->GetFormId(), baseId, spellOrSkillFormId);
    return;
  }

  SpSnippet("Actor", "RemoveSpell",
            SpSnippetFunctionGen::SerializeArguments(arguments, worldState),
            actor->GetFormId())
    .Execute(actor, SpSnippetMode::kNoReturnResult);
}
