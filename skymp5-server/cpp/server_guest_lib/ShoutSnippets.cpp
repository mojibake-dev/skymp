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
  for (const auto& word : record.wordsKnown) {
    SendShoutEntry(actor, MpActor::ShoutEntry::WordKnown,
                   word.ToFormId(files));
  }
  for (const auto& word : record.wordsUnlocked) {
    SendShoutEntry(actor, MpActor::ShoutEntry::WordUnlocked,
                   word.ToFormId(files));
  }
}
