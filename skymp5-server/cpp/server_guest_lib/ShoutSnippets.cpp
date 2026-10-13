#include "ShoutSnippets.h"
#include "SpSnippet.h"
#include "WorldState.h"
#include <optional>
#include <string>
#include <variant>
#include <vector>

void SendShoutEntry(MpActor& actor, MpActor::ShoutEntry kind, uint32_t formId)
{
  SpSnippetObjectArgument form;
  form.formId = formId;
  form.type = kind == MpActor::ShoutEntry::Shout ? "Shout" : "WordOfPower";
  const std::vector<std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
    args{ form };
  if (kind == MpActor::ShoutEntry::Shout) {
    SpSnippet("Actor", "AddShout", args, actor.GetFormId())
      .Execute(&actor, SpSnippetMode::kNoReturnResult);
    return;
  }
  SpSnippet(
    "Game",
    kind == MpActor::ShoutEntry::WordKnown ? "TeachWord" : "UnlockWord", args)
    .Execute(&actor, SpSnippetMode::kNoReturnResult);
}

void SendShoutRecord(MpActor& actor)
{
  const auto record = actor.GetShoutRecord();
  if (record.Empty()) {
    return;
  }
  const auto& files = actor.GetParent()->espmFiles;
  for (const auto& shout : record.shouts) {
    SendShoutEntry(actor, MpActor::ShoutEntry::Shout, shout.ToFormId(files));
  }
  // a word known comes back silently: Game.TeachWord raises the game's big
  // word-of-power message (x-reteach-probe 20261013-061753: HUD type 21,
  // "Fire Breath", a second after the teachword; AddShout and UnlockWord
  // raised none), shown again at each login (playtest fourteen). SKSE's
  // Form.SetPlayerKnows sets the form's known flag alone (0x40,
  // CommonLibSSE-NG TESForm.h kKnown)
  const std::vector<std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
    knows{ true };
  for (const auto& word : record.wordsKnown) {
    SpSnippet("Form", "SetPlayerKnows", knows, word.ToFormId(files))
      .Execute(&actor, SpSnippetMode::kNoReturnResult);
  }
  for (const auto& word : record.wordsUnlocked) {
    SendShoutEntry(actor, MpActor::ShoutEntry::WordUnlocked,
                   word.ToFormId(files));
  }
}
