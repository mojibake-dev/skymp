#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

// One variable a script override sets: named as the type's chain declares it
// (an auto property's backing variable, "::WordLearned_var"), matched without
// case, `k` rung 3's folded kinds an override sets (3 int, 4 float, 5 bool)
// and that kind's one field.
struct ScriptStateOverrideVar
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("name", name)
      .Serialize("k", k)
      .Serialize("i", i)
      .Serialize("f", f)
      .Serialize("b", b);
  }
  auto ToTuple() const { return std::make_tuple(name, k, i, f, b); }
  friend bool operator==(const ScriptStateOverrideVar& lhs,
                         const ScriptStateOverrideVar& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }
  std::string name; // never empty, no NUL
  uint32_t k = 0;   // 3, 4 or 5 (a u8 on the wire)
  std::optional<int32_t> i;
  std::optional<float> f;
  std::optional<bool> b;
};

// One attached script an override names: its type, the state it goes back to
// ("" the empty state) and the variables it sets
struct ScriptStateOverrideScript
{
  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("type", type)
      .Serialize("state", state)
      .Serialize("vars", vars);
  }
  auto ToTuple() const { return std::make_tuple(type, state, vars); }
  friend bool operator==(const ScriptStateOverrideScript& lhs,
                         const ScriptStateOverrideScript& rhs)
  {
    return lhs.ToTuple() == rhs.ToTuple();
  }
  std::string type;  // the script type, never empty
  std::string state; // the state to set, "" the empty state
  std::vector<ScriptStateOverrideVar> vars; // at most 16, each name once
};

// The state of a world object's scripts that a player's own record decides,
// put back by name on the copy the object's owner runs (R0: the server decides
// it from the player's record), ahead of the relayed entry that runs the
// scripts for that player on the same channel. Server to the owner's game
// alone, reliable. The contract is skymp-wire's (wire-schema skymp.rs,
// ScriptStateOverride, MsgType 89, wire id 97).
struct ScriptStateOverrideMessage
  : public MessageBase<ScriptStateOverrideMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::ScriptStateOverride)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("refrId", refrId)
      .Serialize("scripts", scripts);
  }

  uint32_t refrId = 0; // a reference of the shared load order, never 0
  std::vector<ScriptStateOverrideScript> scripts; // 1 to 16, each type once
};
