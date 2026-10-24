#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <array>
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/npc-fate.md (ADR-048's B): a host's report that a
// game-file actor its engine runs died of something the server has no
// number for (a fall, a hazard, an actor's blow whose engine damage outran
// the server's). Client to server, reliable, once per death, R1: the server
// takes it only when it can check the cause against what it saw
// (ActionListener::OnActorDeath) and refuses the rest, logged. Causes: 0
// unknown, 1 fall, 2 hazard, 3 script, 4 an actor's hit (the killer named).
struct ActorDeathMessage : public MessageBase<ActorDeathMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ActorDeath)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("refrId", refrId)
      .Serialize("cause", cause)
      .Serialize("killer", killer)
      .Serialize("pos", pos);
  }

  uint32_t refrId = 0; // the dead actor's own form id (under 0xff000000)
  uint32_t cause = 0;  // the host's reading, 0 to 4 (wire-validate's bound)
  uint32_t killer = 0; // the killer's form id for cause 4, else 0
  std::array<float, 3> pos = { 0.f, 0.f, 0.f }; // where the body fell
};
