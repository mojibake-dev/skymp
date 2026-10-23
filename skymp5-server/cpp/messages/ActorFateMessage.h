#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <type_traits>
#include <vector>

// thuum docs/verbs/npc-fate.md (ADR-046 decision 1, ADR-049): the server's
// roll for the game-file actors of the cells a player's engine is about to
// attach, sent once per square per player ahead of the engine's own roll at
// the reference's load (PartOne::SendFates), and again at a respawn. Server
// to client, reliable, R0: the client pins the engine's roll from it (the
// leveled base and the worn list); a fate that arrives after the engine
// rolled goes through the engine's own re-roll. At most 64 entries and 32
// worn items each (wire-validate).
struct ActorFateMessage : public MessageBase<ActorFateMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ActorFate)>{};

  struct Fate
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("refrId", refrId)
        .Serialize("baseId", baseId)
        .Serialize("level", level)
        .Serialize("worn", worn);
    }

    friend bool operator==(const Fate& lhs, const Fate& rhs)
    {
      return lhs.refrId == rhs.refrId && lhs.baseId == rhs.baseId &&
        lhs.level == rhs.level && lhs.worn == rhs.worn;
    }

    uint32_t refrId = 0; // the placement's own form id (under 0xff000000)
    uint32_t baseId = 0; // the NPC_ the roll chose; 0 when it chose none
    uint32_t level = 0;  // the level the roll used (0: the old rule's roll)
    std::vector<uint32_t> worn; // the outfit roll's items, by form id
  };

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType).Serialize("fates", fates);
  }

  std::vector<Fate> fates;
};
