#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/weather.md (M4): the server's weather for the place a
// player is in: at login, when it enters a place, and at each change of the
// server's pick for its worldspace. The game forces the weather (a cut) or
// blends into it (transition); a weather of 0 releases the game's override
// (an interior, a worldspace without a pool). Server to client, reliable, one
// player. The contract is skymp-wire's (wire-schema skymp.rs, SetWeather,
// MsgType 66, wire id 74).
struct SetWeatherMessage : public MessageBase<SetWeatherMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::SetWeather)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("place", place)
      .Serialize("weather", weather)
      .Serialize("transition", transition);
  }

  uint32_t place = 0;      // the worldspace, or the interior cell; never 0
  uint32_t weather = 0;    // the WTHR form id; 0 releases the override
  bool transition = false; // blend into it (true) or cut to it (false)
};
