#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

// thuum ADR-068 rung 5 (ScriptVariables; D-V1 B and D-T2 B; docs/research/
// rung5-quests-2026-11-09.md sections 1(a), 7 and 8(b)): the script
// variables a dialogue's conditions read (CTDA 629 GetVMQuestVariable, 630
// GetVMScriptVariable). A topic info's fragment runs on the talking seat
// alone, so every seat reports a listed key its game changed and the server
// keeps the latest write per key (wire-rules script_variables), each key
// with the server's own sequence. Both directions, reliable. Client to
// server: the changed entries (watch empty, served false, every seq 0), a
// player-class quest's keys flagged own (recorded on the reporter's own
// player's change form, never relayed). Server to client: a recorded world
// key relayed to every other seat (served false), and at a login the watch
// list (the keys to read, each own set, at most 256 a message) and the
// served values (served true: the world's record, and the player's own to
// it alone), each with its key's sequence. The contract is skymp-wire's
// (wire-schema skymp.rs, ScriptVariables, MsgType 81, wire id 89); a u8 on
// the wire is a uint32_t here, the messages' idiom.
struct ScriptVariablesMessage : public MessageBase<ScriptVariablesMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::ScriptVariables)>{};

  // one key: the QUST or REFR, the attached script whose chain declares the
  // variable, the variable as the condition names it, and whether it is its
  // player's own
  struct Key
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("form", form)
        .Serialize("script", script)
        .Serialize("var", var)
        .Serialize("own", own);
    }

    auto ToTuple() const { return std::make_tuple(form, script, var, own); }

    friend bool operator==(const Key& lhs, const Key& rhs)
    {
      return lhs.ToTuple() == rhs.ToTuple();
    }

    uint32_t form = 0;  // a form of the shared load order
    std::string script; // never empty, no NUL
    std::string var;    // never empty, no NUL
    bool own = false;   // its player's own key
  };

  // one value: the key's four fields, `k` 3 int, 4 float or 5 bool with the
  // one field that kind names, and the server's sequence for the key (0 on
  // a report, never 0 when served)
  struct Entry
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("form", form)
        .Serialize("script", script)
        .Serialize("var", var)
        .Serialize("own", own)
        .Serialize("k", k)
        .Serialize("i", i)
        .Serialize("f", f)
        .Serialize("b", b)
        .Serialize("seq", seq);
    }

    auto ToTuple() const
    {
      return std::make_tuple(form, script, var, own, k, i, f, b, seq);
    }

    friend bool operator==(const Entry& lhs, const Entry& rhs)
    {
      return lhs.ToTuple() == rhs.ToTuple();
    }

    uint32_t form = 0;
    std::string script;
    std::string var;
    bool own = false;
    uint32_t k = 0; // 3 int, 4 float, 5 bool (a u8 on the wire)
    std::optional<int32_t> i;
    std::optional<float> f;
    std::optional<bool> b;
    uint32_t seq = 0;
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("watch", watch)
      .Serialize("entries", entries)
      .Serialize("served", served);
  }

  std::vector<Key> watch;     // server to client only, at a login
  std::vector<Entry> entries; // each key once, 256 at most
  bool served = false;        // the record served at login
};
