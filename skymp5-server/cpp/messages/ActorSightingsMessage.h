#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <array>
#include <cstdint>
#include <type_traits>
#include <vector>

// thuum docs/verbs/npc-puppet.md (ADR-045's revision of 2026-10-22; TES3MP's
// actor list): a host's roster of the game-file actors its engine has
// attached that the server holds no record of, with where the engine has
// them. Client to server, reliable, R1: the server loads each record on
// demand and places it there only if the sender hosts the cell of that
// position (PartOne::OnActorSighting); a sighting of a cell hosted by
// another game, of a player's actor or of a record standing in a cell
// another game hosts is dropped. At most 64 entries (wire-validate), every
// 2 s while any remain (HostService.reportSightings).
struct ActorSightingsMessage : public MessageBase<ActorSightingsMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ActorSightings)>{};

  struct Sighting
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("refrId", refrId)
        .Serialize("worldOrCell", worldOrCell)
        .Serialize("pos", pos)
        .Serialize("rot", rot);
    }

    friend bool operator==(const Sighting& lhs, const Sighting& rhs)
    {
      return lhs.refrId == rhs.refrId && lhs.worldOrCell == rhs.worldOrCell &&
        lhs.pos == rhs.pos && lhs.rot == rhs.rot;
    }

    uint32_t refrId = 0;      // the actor's own form id (under 0xff000000)
    uint32_t worldOrCell = 0; // the worldspace, or the interior cell
    std::array<float, 3> pos = { 0.f, 0.f, 0.f };
    std::array<float, 3> rot = { 0.f, 0.f, 0.f };
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType).Serialize("sightings", sightings);
  }

  std::vector<Sighting> sightings;
};
