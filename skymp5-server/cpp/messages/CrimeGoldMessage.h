#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>

// thuum tracker #97, ADR-079, D-G6 A (docs/verbs/jail.md): one hold's bounty
// as the player's own game holds it, sent when it changes. Client to server,
// reliable. The record is the server's (R0), given back at a login
// (PartOne::OnCrimeGold). The contract is skymp-wire's (wire-schema
// skymp.rs, CrimeGold, MsgType 85, wire id 93).
struct CrimeGoldMessage : public MessageBase<CrimeGoldMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::CrimeGold)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("faction", faction)
      .Serialize("violent", violent)
      .Serialize("nonViolent", nonViolent);
  }

  uint32_t faction = 0;    // the crime faction, never 0
  uint32_t violent = 0;    // its violent crime gold
  uint32_t nonViolent = 0; // its non-violent crime gold
};
