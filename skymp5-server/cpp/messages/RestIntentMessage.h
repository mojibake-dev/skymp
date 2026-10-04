#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <type_traits>

// The player waited or slept, for the game hours the engine counted across
// the Sleep/Wait menu (thuum docs/verbs/rest.md, ADR-021 decision 2). The
// server checks it and computes the recovery; the shared clock does not move.
struct RestIntentMessage : public MessageBase<RestIntentMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::RestIntent)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("hours", hours)
      .Serialize("sleep", sleep);
  }

  float hours = 0.f;
  bool sleep = false; // a sleep in a bed rather than a wait
};
