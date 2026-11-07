#pragma once
#include "SpSnippetMessage.h" // SpSnippetObjectArgument
#include "papyrus-vm/Structures.h"
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <optional>
#include <variant>

class MpActor;
class WorldState;

enum class SpSnippetMode
{
  kNoReturnResult,
  kReturnResult,
};

class SpSnippet
{
public:
  SpSnippet(const char* cl_, const char* func_,
            const std::vector<std::optional<std::variant<
              bool, double, std::string, SpSnippetObjectArgument>>>& args_,
            uint32_t selfId_ = 0 /*zero indicates static function call*/);

  // actorExecutor will be used to detect userId to execute SpSnippet on
  Viet::Promise<VarValue> Execute(MpActor* actorExecutor, SpSnippetMode mode);

  // thuum ADR-068 (the one seam): a script call the subject's owner reported,
  // relayed to one listener's game: no return value, the subject's sequence
  // `seq` on the message, sent at once (MpActor::SendToUser, reliable),
  // never through the deferred queue Execute's snippets take (6.11 item 1: a
  // relay must not land behind the object's later record or relay)
  void Relay(MpActor* listener, uint32_t seq);

  // thuum ADR-068 rung 4 (the standing's serve): a recorded state's applier
  // to one listener's game behind a create word, at once, no answer and no
  // sequence (several appliers serve one subject, and a receiver drops a
  // seq at or below the last it took)
  void Serve(MpActor* listener);

  static VarValue VarValueFromSpSnippetReturnValue(
    const std::optional<std::variant<bool, double, std::string>>& returnValue);

  static uint64_t MakeLongFormId(WorldState* worldState, uint32_t formId);

private:
  // the message to `actorExecutor`'s game, its self as that engine numbers it
  SpSnippetMessage MakeMessage(MpActor* actorExecutor,
                               uint32_t snippetIdx) const;

  const char* const cl;
  const char* const func;
  const std::vector<std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>>& args;
  const uint32_t selfId;
};
