#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <array>
#include <cstdint>
#include <type_traits>

// thuum tracker #84 (docs/verbs/summon.md, ADR-052 decision 3 as amended
// 2026-11-16, ADR-063: the caster's row owns its summon): the summon a
// player's engine placed at a summon effect's apply (ended false: its
// runtime id, its base, the summon effect and the spell, its place and the
// time the effect has left) and its end (ended true: the local id alone
// read, the engine's own Finish after a death as its timer). Client to
// server, reliable, R2: the message names no actor, so it is only ever the
// sender's own player's; ActionListener::OnSummon asks wire-rules summons'
// bounds (the spell the record holds, the effect on it, the archetype, the
// associated base, the place, the time, the live count, each refusal an
// E_SUMMON_*) and places the server's actor, which the caster's game
// simulates through its create word's ownLocal bind. The contract is
// skymp-wire's (wire-schema skymp.rs, Summon, MsgType 83, wire id 91).
struct SummonMessage : public MessageBase<SummonMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::Summon)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("localId", localId)
      .Serialize("baseId", baseId)
      .Serialize("effectId", effectId)
      .Serialize("spellId", spellId)
      .Serialize("pos", pos)
      .Serialize("rot", rot)
      .Serialize("worldOrCell", worldOrCell)
      .Serialize("remainingS", remainingS)
      .Serialize("ended", ended);
  }

  uint32_t localId = 0;  // the summon's id in the caster's engine, a
                         // runtime id 0xff000000 and up
  uint32_t baseId = 0;   // the summon's base, an NPC_ of the load order; a
                         // start's word
  uint32_t effectId = 0; // the summon effect (an MGEF) of the spell that
                         // cast it; a start's word
  uint32_t spellId = 0;  // the spell as the caster's engine holds it; a
                         // start's word
  std::array<float, 3> pos, rot; // where it stands, which way it faces (the
                                 // engine's GetAngle words, degrees); a
                                 // start's word
  uint32_t worldOrCell = 0;      // the worldspace or cell of the place; a
                                 // start's word
  float remainingS = 0.f;        // the seconds the effect has left, at
                                 // least 0; a start's word
  bool ended = false;            // false: the start of the summon localId
                                 // names. true: its end
};
