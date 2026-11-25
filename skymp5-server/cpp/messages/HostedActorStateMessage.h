#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <optional>
#include <type_traits>
#include <vector>

// thuum docs/verbs/npc-stream.md (ADR-050 decision 1, slice 2): a host's
// state for an actor it simulates, as its engine has it: the three
// percentages, its combat target as the server knows it (0 for none) and
// the package its last package event named (0 for none). Client to server,
// under the actor's idx, unreliable (the first for an actor after a grant
// reliable), on a change at most twice a second. The server judges the
// values against its own numbers (wire-rules hosted_state) and records the
// rest; its output is ChangeValues to the actor's other listeners
// (ActionListener::OnHostedActorState). The running effects (thuum
// docs/verbs/npc-puppet.md, Ownership) ride it: present on a change, sent
// and relayed reliable, recorded on the actor (R2) and served to a later
// subscriber. The contract is skymp-wire's (wire-schema skymp.rs,
// HostedActorState, MsgType 53, wire id 61).
struct HostedActorStateMessage : public MessageBase<HostedActorStateMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char,
                           static_cast<char>(MsgType::HostedActorState)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("idx", idx)
      .Serialize("health", health)
      .Serialize("magicka", magicka)
      .Serialize("stamina", stamina)
      .Serialize("combatTarget", combatTarget)
      .Serialize("package", package)
      .Serialize("gazeTarget", gazeTarget)
      .Serialize("gazeSlot", gazeSlot)
      .Serialize("emotionType", emotionType)
      .Serialize("emotionValue", emotionValue)
      .Serialize("effects", effects)
      .Serialize("detects", detects);
  }

  // one player's detection by the actor: the player's actor as the server
  // knows it and the level the host's engine holds of its figure (-1000 for
  // none, seen above 0)
  struct PlayerDetection
  {
    template <class Archive>
    void Serialize(Archive& archive)
    {
      archive.Serialize("player", player).Serialize("level", level);
    }

    friend bool operator==(const PlayerDetection& lhs,
                           const PlayerDetection& rhs)
    {
      return lhs.player == rhs.player && lhs.level == rhs.level;
    }

    uint32_t player = 0;
    int32_t level = -1000;
  };

  uint32_t idx = 0;
  float health = 1.f;
  float magicka = 1.f;
  float stamina = 1.f;
  uint32_t combatTarget = 0;
  uint32_t package = 0;
  // slice 5 (the gaze and the face): the head-track target as the server
  // knows it (0 none), the engine's slot type (below 6), the emotion type
  // (at most 7) and strength (at most 100); the server records them and
  // relays the message to the actor's other listeners, whose puppets apply
  // them (the percentages stay the server's ChangeValues)
  uint32_t gazeTarget = 0;
  uint32_t gazeSlot = 0;
  uint32_t emotionType = 0;
  uint32_t emotionValue = 0;
  // the effects whose look the host's engine draws on the actor and keeps
  // (active, neither inactive nor dispelled, a hit shader or a hit effect
  // art, a constant effect or FX Persist): base effect form ids in the load
  // order, ascending, each once, at most 32 (the wire's check); present on
  // the first report with the engine's read and on a change, absent when
  // unchanged (the record keeps the last set)
  std::optional<std::vector<uint32_t>> effects;
  // the actor's detection of each player whose figure the host's engine holds
  // a level of, ascending by player, at most 16 (the wire's check); present
  // when an entry came or went or a level crossed into or out of sight or
  // moved by ten, absent otherwise (a receiver keeps the last); relayed
  // reliable to the actor's other listeners, not recorded
  std::optional<std::vector<PlayerDetection>> detects;
};
