#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// The world's game clock as the engine's six time globals hold it (thuum
// docs/verbs/time.md, ADR-021). Sent at login ahead of the player's own
// CreateActor, and every 60 s after.
struct SetGameTimeMessage : public MessageBase<SetGameTimeMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::SetGameTime)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("year", year)
      .Serialize("month", month)
      .Serialize("day", day)
      .Serialize("hour", hour)
      .Serialize("daysPassed", daysPassed)
      .Serialize("timeScale", timeScale);
  }

  uint32_t year = 0;
  uint32_t month = 0; // from 0, Morning Star
  uint32_t day = 1;   // from 1
  float hour = 0.f;
  float daysPassed = 0.f;
  float timeScale = 0.f;
};
