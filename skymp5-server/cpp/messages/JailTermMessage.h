#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// A player's jail term as the server's record holds it (R0), told to that
// player's own game alone: at the arrest's record, at every login while the
// term stands, and once when the shared clock passes its end. The end is a
// time on the shared clock, which runs while the player is away, so the game
// reads the time left from this rather than from its own clock, and a served
// term starts the release on that game. Server to client, reliable. The
// contract is skymp-wire's (wire-schema skymp.rs, JailTerm, MsgType 88, wire
// id 96).
struct JailTermMessage : public MessageBase<JailTermMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::JailTerm)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("faction", faction)
      .Serialize("releaseAt", releaseAt)
      .Serialize("served", served);
  }

  uint32_t faction = 0;  // the crime faction that jailed the player; never 0
  float releaseAt = 0.f; // the shared clock's days passed at the end; >= 0
  bool served = false;   // the shared clock has passed the end
};
