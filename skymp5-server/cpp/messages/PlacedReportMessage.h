#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <array>
#include <cstdint>
#include <type_traits>

// thuum docs/verbs/spell-placed-object.md (ADR-057 shape A, D70): where the
// caster's engine stands one base of a requested set. Client to server,
// reliable, once per base per set, R1: the caster's game computes (the
// script's placement), the server validates (the set its own request to
// this caster and still open, the base one the set asked for and not yet
// reported, the pose 150 to 260 units from where the caster stood at the
// cast on the ground plane and within 256 of its height; wire-rules
// placements, ActionListener::OnPlacedReport) and places its copy there for
// every other seat. The contract is skymp-wire's (wire-schema skymp.rs,
// PlacedReport, MsgType 62, wire id 70).
struct PlacedReportMessage : public MessageBase<PlacedReportMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PlacedReport)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("set", set)
      .Serialize("base", base)
      .Serialize("pos", pos)
      .Serialize("yaw", yaw);
  }

  uint64_t set = 0;  // the set named by the request
  uint32_t base = 0; // the base found, a form id, never 0
  std::array<float, 3> pos = { 0.f, 0.f,
                               0.f }; // where it stands, engine units
  float yaw = 0.f;                    // its Z angle, degrees
};
