#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <optional>
#include <type_traits>

// thuum docs/verbs/locks.md (M4): a player's game reports how its lockpicking
// attempt on a locked reference ended. Client to server, reliable, R1: the
// engine ran the minigame, the server judges the report (wire-rules locks,
// ActionListener::OnLockReport: the reference locked and within the
// activation reach, the lock's tier open to picks, a Lockpick held, a gap
// between reports) and writes the unlock or takes the broken picks from the
// record. Since thuum ADR-073 step 3 (decision 12) outcome 3 is the game's
// engine writing the lock by itself (a script's Lock or SetLockLevel, the
// owners' door walk, an NPC's key or pick), with `locked` and `level` its
// engine's lock after the write; the server records it from the reference's
// owner (ActionListener::OnLockReport). The contract is skymp-wire's
// (wire-schema skymp.rs, LockReport, MsgType 64, wire id 72).
struct LockReportMessage : public MessageBase<LockReportMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::LockReport)>{};

  // the engine's write (skymp-wire's LOCK_ENGINE_WRITE)
  static constexpr uint32_t kEngineWrite = 3;

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("refr", refr)
      .Serialize("outcome", outcome)
      .Serialize("broken", broken)
      .Serialize("locked", locked)
      .Serialize("level", level);
  }

  uint32_t refr = 0;    // the reference's form id, never 0
  uint32_t outcome = 0; // 0 picked, 1 picks broke, 2 the menu left, 3 the
                        // engine's write (a u8)
  uint32_t broken = 0;  // picks broken in this attempt, 0 to 8 (a u8)
  std::optional<bool> locked;    // an engine write's lock after it
  std::optional<uint32_t> level; // an engine write's level byte (a u8)
};
