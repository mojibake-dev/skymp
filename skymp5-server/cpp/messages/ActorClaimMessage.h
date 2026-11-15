#pragma once
#include "MessageBase.h"
#include "MsgType.h"
#include <cstdint>
#include <optional>
#include <type_traits>

// thuum docs/verbs/npc-talk.md (ADR-052): a player asks for an NPC another
// player's game simulates, for a dialogue (claim true), or gives it back
// (claim false). Client to server, reliable, R0: the server grants when the
// claimant stands in the actor's cell within activation reach and the actor
// is an NPC, alive, not in combat and unclaimed
// (ActionListener::OnActorClaim); the word names the claimant as the simulator
// for the claim's lease. Since SCHEMA_VERSION 47 (thuum ADR-073 decision 6,
// step 5a) a claim or a release names its purpose: absent or 0 a talk as
// before, 1 a quest's lead (`quest` and `package`: the quest's own package
// running on the actor, which fills one of its aliases; D-53-4), 2 a scene
// the sender's seat started (`quest` and `scene`; D-L2 B), each absent unless
// set, so a talk reads and writes as before. A lead's release may say `why`
// (thuum D-53-5 B): 1 the sender's game started a package that is not one of
// the quest's own, 2 the actor is no longer loaded in the sender's game; its
// own report (R2). The contract is skymp-wire's (wire-schema skymp.rs,
// ActorClaim, MsgType 56, wire id 64).
struct ActorClaimMessage : public MessageBase<ActorClaimMessage>
{
  static constexpr auto kMsgType =
    std::integral_constant<char, static_cast<char>(MsgType::ActorClaim)>{};

  template <class Archive>
  void Serialize(Archive& archive)
  {
    archive.Serialize("t", kMsgType)
      .Serialize("idx", idx)
      .Serialize("claim", claim)
      .Serialize("purpose", purpose)
      .Serialize("quest", quest)
      .Serialize("package", package)
      .Serialize("scene", scene)
      .Serialize("why", why);
  }

  uint32_t idx = 0;
  bool claim = true;
  // since SCHEMA_VERSION 47: 0 a talk, 1 a lead, 2 a scene (a u8 on the
  // wire); absent means a talk
  std::optional<uint32_t> purpose;
  // a lead's or a scene's QUST, in the load order both ends share
  std::optional<uint32_t> quest;
  // a lead's PACK, the package the sender's engine runs on the actor
  std::optional<uint32_t> package;
  // a scene's SCEN
  std::optional<uint32_t> scene;
  // since SCHEMA_VERSION 47 (D-53-5 B), a lead's release only: 1 its game
  // left the quest's own packages, 2 its game unloaded the actor (a u8 on
  // the wire); absent or 0 a release
  std::optional<uint32_t> why;
};
