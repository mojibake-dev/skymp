#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/locks.md (M4): a player's game reports how its lockpicking
// attempt on a locked reference ended. Client to server, reliable, R1: the
// engine ran the minigame, the server judges the report (wire-rules locks,
// ActionListener::OnLockReport: the reference locked and within the
// activation reach, the lock's tier open to picks, a Lockpick held, a gap
// between reports) and writes the unlock or takes the broken picks from the
// record. The contract is skymp-wire's (wire-schema skymp.rs, LockReport,
// MsgType 64, wire id 72).
struct LockReportMessage : public MessageBase<LockReportMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::LockReport)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("refr", refr)
      .Serialize("outcome", outcome)
      .Serialize("broken", broken);
  }

  uint32_t refr = 0;    // the reference's form id, never 0
  uint32_t outcome = 0; // 0 picked, 1 picks broke, 2 the menu left (a u8)
  uint32_t broken = 0;  // picks broken in this attempt, 0 to 8 (a u8)
};
