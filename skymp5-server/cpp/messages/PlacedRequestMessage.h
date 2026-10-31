#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>
#include <vector>

// thuum docs/verbs/spell-placed-object.md (ADR-057 shape A, D70): the server
// asks the caster's game where its engine stands the objects a listed
// spell's script placed. Server to client, reliable, to the caster alone at
// its accepted cast (PartOne::SendPlacementRequests, wire-rules placements):
// the set's id, the bases to find (form ids in the load order both ends
// share) and the window in milliseconds within which the reports are taken.
// The caster's game answers one PlacedReport per base once the reference
// has settled. The contract is skymp-wire's (wire-schema skymp.rs,
// PlacedRequest, MsgType 61, wire id 69).
struct PlacedRequestMessage : public MessageBase<PlacedRequestMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::PlacedRequest)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("set", set)
      .Serialize("bases", bases)
      .Serialize("windowMs", windowMs);
  }

  uint64_t set = 0;            // the set's id, which the reports name
  std::vector<uint32_t> bases; // the bases to find, one to eight, none 0
  uint32_t windowMs = 0;       // how long from the cast the reports are taken
};
