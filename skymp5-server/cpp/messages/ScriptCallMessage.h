#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include "SpSnippetMessage.h" // SpSnippetObjectArgument
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

// thuum ADR-068 (the one seam; docs/research/scripts-one-system-2026-11-05.md
// 6.4): a tabled native a script ran on the engine that owns the call's
// subject, read at the native boundary (NativeFunctionBase::Call) from the
// call's typed frame and reported as it ran. The server judges the shape
// alone (wire-rules natives: the pair an action row or a state row kept as
// evidence, the arguments the row's count and kinds, the sender's budget,
// the subject's owner at the epoch; ActionListener::OnScriptCall) and relays
// an accepted action to the subject's other listeners as SpSnippet with the
// subject's sequence, or notes an evidence row's call for its subject
// (ProcessTrapHit's, a trap's hit on an NPC the sender simulates: the NPC's
// death evidence, thuum docs/verbs/npc-fate.md); a value is never judged.
// Client to server, reliable. The contract is skymp-wire's (wire-schema
// skymp.rs, ScriptCall, MsgType 75, wire id 83). Since SCHEMA_VERSION 44
// (thuum D-R1 C's leveled gap, train 52) a leveled AddItem on the sender's
// own player carries `landed`, what the engine's roll put in that player's
// inventory (each base once, its count summed; empty when the roll landed
// nothing), which the server records as plain grants of bases the list can
// roll (ActionListener's RecordPlayerOwn, wire-rules player_own).
struct ScriptCallMessage : public MessageBase<ScriptCallMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ScriptCall)>{};

  // one landing of a leveled roll: a base the roll put in the sender's own
  // player and how many (wire-schema ItemDelta; wire-validate holds the item
  // a load-order form below 0xff000000, the count above zero and each base
  // once)
  struct Landing
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("item", item).Serialize("count", count);
    }

    friend bool operator==(const Landing& lhs, const Landing& rhs)
    {
      return lhs.item == rhs.item && lhs.count == rhs.count;
    }

    uint32_t item = 0;
    int32_t count = 0;
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("epoch", epoch)
      .Serialize("selfId", selfId)
      .Serialize("cls", cls)
      .Serialize("fn", fn)
      .Serialize("args", args)
      .Serialize("caller", caller)
      .Serialize("handler", handler)
      .Serialize("latent", latent)
      .Serialize("landed", landed);
  }

  // the subject's owner epoch the sender holds (the cell's host epoch;
  // unread for an actor, whose owner is its simulator)
  uint32_t epoch = 0;
  // the call's subject: a method native's self, a static one's target (its
  // first argument); a load-order form, or 0x14 for the sender's own player
  uint32_t selfId = 0;
  std::string cls; // the native's Papyrus class
  std::string fn;  // the native's name
  // the call's arguments in the native's order; an unread one is empty
  std::vector<std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
    args;
  std::string caller;  // the calling script's type, for the log
  std::string handler; // the calling function, for the log
  bool latent = false; // the native is latent: a receiver runs its twin
  // since SCHEMA_VERSION 44: a leveled AddItem's roll as it landed on the
  // sender's own player, at most 255 (TESLeveledList's one-byte entry
  // count); absent on every other call
  std::optional<std::vector<Landing>> landed;
};
