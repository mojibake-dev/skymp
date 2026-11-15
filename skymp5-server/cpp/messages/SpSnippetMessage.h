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
      .Serialize("seq", seq)
      .Serialize("keep", keep);
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
  // thuum #52 (wire-schema SpSnippet's `keep`, SCHEMA_VERSION 53): on a
  // relayed call the server keeps as its actor's look, the row's keep code
  // (wire-rules natives LookKeep::code: 1 EffectShader.Play, 2 its Stop, 3
  // VisualEffect.Play, 4 its Stop, 5 Actor.SetEyeTexture), so the receiver
  // keeps it for its copy's 3D loads by the row's own word; absent on every
  // other snippet, and only with `seq`
  std::optional<uint32_t> keep;
};
