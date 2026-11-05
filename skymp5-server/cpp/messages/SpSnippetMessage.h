#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <type_traits>

struct SpSnippetObjectArgument
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("formId", formId).Serialize("type", type);
  }

  uint64_t formId = 0;
  std::string type;
};

struct SpSnippetMessage : public MessageBase<SpSnippetMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::SpSnippet)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("class", class_)
      .Serialize("function", function)
      .Serialize("arguments", arguments)
      .Serialize("selfId", selfId)
      .Serialize("snippetIdx", snippetIdx)
      .Serialize("seq", seq);
  }

  std::string class_;
  std::string function;
  std::vector<std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
    arguments;
  uint64_t selfId = 0;
  int64_t snippetIdx = 0;
  // thuum ADR-068 (the one seam): the server's sequence for the subject of a
  // relayed script call (ScriptCall), up by one at every accepted call on
  // that object; empty, and no key, for every other snippet
  std::optional<uint32_t> seq;
};
