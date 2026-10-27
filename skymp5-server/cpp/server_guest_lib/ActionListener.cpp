#include "ActionListener.h"
#include "AnimationSystem.h"
#include "ConditionsEvaluator.h"
#include "ConsoleCommands.h"
#include "CropRegeneration.h"
#include "Exceptions.h"
#include "GetBaseActorValues.h"
#include "HitData.h"
#include "MathUtils.h"
#include "MovementValidation.h"
#include "MpObjectReference.h"
#include "MsgType.h"
#include "Overloaded.h"
#include "ShoutSnippets.h"
#include "TeleportMessage.h"
#include "WorldState.h"
#include "gamemode_events/CustomEvent.h"
#include "gamemode_events/EatItemEvent.h"
#include "gamemode_events/UpdateAppearanceAttemptEvent.h"
#include "gamemode_events/UpdateEquipmentAttemptEvent.h"
#include "libespm/Convert.h"
#include "libespm/GMST.h"
#include "libespm/ObjectBounds.h"
#include "libespm/RACE.h"
#include "libespm/REFR.h"
#include "libespm/SHOU.h"
#include "libespm/SPEL.h"
#include "libespm/Utils.h"
#include "libespm/WEAP.h"
#include "script_objects/EspmGameObject.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <spdlog/spdlog.h>
#include <unordered_map>
#include <unordered_set>

#include "ArrowShotMessage.h"
#include "CustomPacketMessage.h"
#include "SpSnippet.h"
#include "UpdateAnimVariablesMessage.h"
#include "UpdateEquipmentMessage.h"

namespace FormIdCasts {
uint32_t LongToNormal(uint64_t longFormId)
{
  return static_cast<uint32_t>(longFormId % 0x100000000);
}
}

MpActor* ActionListener::ActorUpdatableBy(uint32_t idx,
                                          Networking::UserId userId,
                                          MsgType msgType)
{
  // slice 4's counters: the row this outcome lands in
  const auto count = [&](bool accepted, bool foreign) {
    if (msgType != MsgType::Invalid) {
      partOne.serverState.CountStream(userId, static_cast<uint8_t>(msgType),
                                      accepted, foreign);
    }
  };
  MpActor* myActor = partOne.serverState.ActorByUser(userId);
  // The old behavior is doing nothing in that case. This is covered by tests
  if (!myActor) {
    spdlog::warn("SendToNeighbours - No actor assigned to user");
    count(false, false);
    return nullptr;
  }

  MpForm* form = partOne.worldState.LookupFormByIdx(idx);
  MpActor* actor = form ? form->AsActor() : nullptr;
  if (!actor) {
    spdlog::error("SendToNeighbours - Target actor doesn't exist");
    count(false, false);
    return nullptr;
  }

  if (idx != myActor->GetIdx()) {
    // Possible fix for "players link to each other" bug
    // See also PartOne::SetUserActor
    Networking::UserId actorsOwningUserId =
      partOne.serverState.UserByActor(actor);
    if (actorsOwningUserId != Networking::InvalidUserId) {
      spdlog::error("SendToNeighbours - No permission to update actor {:x} "
                    "(already owned by user {})",
                    actor->GetFormId(), actorsOwningUserId);
      count(false, true);
      return nullptr;
    }

    // thuum docs/verbs/cell-host.md (ADR-035): an actor no user plays is
    // the host's of its cell; a report from any other game is dropped and
    // nothing is sent back (the client's HostService knows its grants)
    if (partOne.worldState.HostOf(*actor) != myActor) {
      if (idx == 0) {
        spdlog::warn("SendToNeighbours - idx=0, <Message>::ReadJson or "
                     "similar is probably incorrect");
      }
      spdlog::error("SendToNeighbours - No permission to update actor {:x} "
                    "(not the host of its cell)",
                    actor->GetFormId());
      count(false, true);
      return nullptr;
    }
  }

  count(true, false);
  return actor;
}

void ActionListener::RelayToListeners(MpActor& actor,
                                      Networking::PacketData data,
                                      size_t length, bool reliable,
                                      Networking::UserId sender)
{
  // thuum docs/verbs/cell-host.md: the host's own game has the truth of an
  // actor it hosts (its engine moved it), so its report goes to every other
  // listener and not back to it: the echo replayed the actor's own turn and
  // movement events on the actor and spun it (playtest thirty, 2026-10-21,
  // Eli: "the wolves are spinning around in place", on the host's screen
  // too). A player's own actor keeps the echo it always had.
  MpActor* senderActor = sender == Networking::InvalidUserId
    ? nullptr
    : partOne.serverState.ActorByUser(sender);
  const bool hosted = senderActor && senderActor != &actor;
  for (auto listener : actor.GetActorListeners()) {
    if (hosted && listener == senderActor) {
      continue;
    }
    auto targetuserId = partOne.serverState.UserByActor(listener);
    if (targetuserId != Networking::InvalidUserId) {
      partOne.GetSendTarget().Send(targetuserId, data, length, reliable);
    }
  }
}

MpActor* ActionListener::SendToNeighbours(uint32_t idx,
                                          Networking::UserId userId,
                                          Networking::PacketData data,
                                          size_t length, bool reliable,
                                          MsgType msgType)
{
  MpActor* actor = ActorUpdatableBy(idx, userId, msgType);
  if (actor) {
    RelayToListeners(*actor, data, length, reliable, userId);
  }
  return actor;
}

MpActor* ActionListener::SendToNeighbours(uint32_t idx,
                                          const RawMessageData& rawMsgData,
                                          bool reliable, MsgType msgType)
{
  return SendToNeighbours(idx, rawMsgData.userId, rawMsgData.unparsed,
                          rawMsgData.unparsedLength, reliable, msgType);
}

void ActionListener::RelayCast(MpActor& caster, const MpActor& sender,
                               const SpellCastData& data)
{
  SpellCastMessage relay;
  relay.data = data;
  for (auto listener : caster.GetActorListeners()) {
    if (listener != &sender) {
      listener->GetActorToSendTo().SendToUser(relay, true);
    }
  }
}

void ActionListener::OnCustomPacket(const RawMessageData& rawMsgData,
                                    const CustomPacketMessage& msg)
{
  simdjson::dom::parser parser;
  auto content = parser.parse(msg.contentJsonDump).value();
  for (auto& listener : partOne.GetListeners()) {
    listener->OnCustomPacket(rawMsgData.userId, content);
  }
}

void ActionListener::OnUpdateMovement(const RawMessageData& rawMsgData,
                                      const UpdateMovementMessage& msg)
{
  // Validate, then relay: a move the server rejects must not reach the
  // neighbours (thuum docs/verbs/validation.md, Movement)
  MpActor* actor = nullptr;
  bool hostedCellChange = false;
  bool passed = false;
  {
    // thuum docs/verbs/cell-host.md, the cell-change rule: a hosted actor's
    // report is its cell's host's; one naming another cell is accepted when
    // the sender hosts the destination and the cell it leaves is unhosted or
    // the sender's too (npc-handoff's row otherwise: dropped, logged)
    MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);
    MpForm* form = partOne.worldState.LookupFormByIdx(msg.idx);
    MpActor* moved = form ? form->AsActor() : nullptr;
    if (myActor && moved && moved != myActor &&
        partOne.serverState.UserByActor(moved) == Networking::InvalidUserId) {
      const NiPoint3 destPos{ msg.data.pos[0], msg.data.pos[1],
                              msg.data.pos[2] };
      const auto destCell =
        partOne.worldState.CellOf(msg.data.worldOrCell, destPos);
      const auto verdict =
        partOne.MayMoveActor(*myActor, *moved, msg.data.worldOrCell, destPos);
      if (verdict == PartOne::MoveVerdict::Refuse) {
        // one warn line a minute per actor, the rest counted (npc-handoff
        // (e): Stump's 751 lines in five minutes, playtest 32)
        static std::unordered_map<
          uint32_t, std::pair<std::chrono::steady_clock::time_point, uint32_t>>
          refusals;
        auto& [since, count] = refusals[moved->GetFormId()];
        const auto now = std::chrono::steady_clock::now();
        ++count;
        if (since.time_since_epoch().count() != 0 &&
            now - since < std::chrono::minutes(1)) {
          partOne.serverState.CountStream(
            rawMsgData.userId, static_cast<uint8_t>(MsgType::UpdateMovement),
            false);
          return;
        }
        spdlog::warn("ActionListener::OnUpdateMovement - cell-host: {:x} may "
                     "not move {:x} to cell {:x} (it simulates the actor "
                     "nowhere it may take it: not its cell, not a cell it "
                     "hosts, not a follow it can vouch for); dropped, {} "
                     "refusal(s) since the last line",
                     myActor->GetFormId(), moved->GetFormId(),
                     destCell.value_or(0), count);
        since = now;
        count = 0;
        partOne.serverState.CountStream(
          rawMsgData.userId, static_cast<uint8_t>(MsgType::UpdateMovement),
          false);
        return;
      }
      hostedCellChange =
        destCell.has_value() && destCell != partOne.worldState.CellOf(*moved);
      passed = verdict == PartOne::MoveVerdict::Pass;
      if (hostedCellChange) {
        spdlog::info("ActionListener::OnUpdateMovement - cell-host: {:x} {} "
                     "{:x} into cell {:x}, which {}",
                     myActor->GetFormId(),
                     verdict == PartOne::MoveVerdict::Follow ? "follows"
                       : passed                              ? "passes"
                                                             : "moves",
                     moved->GetFormId(), *destCell,
                     verdict == PartOne::MoveVerdict::Follow
                       ? (partOne.GetCellHost(*destCell).hosted
                            ? "another game hosts, within the pass margin "
                              "(npc-handoff D22: the crossing host keeps it)"
                            : "nobody hosts (npc-handoff D6)")
                       : passed ? "another game hosts (npc-handoff D9: the "
                                  "simulation passes with it)"
                                : "it hosts");
      }
      if (hostedCellChange) {
        // thuum docs/verbs/npc-handoff.md D6, the viewer's half (x-handoff-
        // door 20261024-001915: the host's engine walked Alvor into the inn
        // and the other seat's engine kept him at the door): the record's
        // move re-subscribes it on the destination's grid, so every viewer
        // outside it is dropped at once and a dropped view moves nothing.
        // Before that, each listening game but the host's gets a Teleport
        // for the actor, the reliable channel ahead of the DestroyActor, so
        // its engine moves its own reference into the record's cell
        // (remoteServer's onTeleportMessage, moveRefrToPosition).
        TeleportMessage teleport;
        teleport.idx = moved->GetIdx();
        teleport.pos = { destPos.x, destPos.y, destPos.z };
        teleport.rot = { msg.data.rot[0], msg.data.rot[1], msg.data.rot[2] };
        teleport.worldOrCell = msg.data.worldOrCell;
        // only the games the move unsubscribes (outside the destination's
        // 3x3): a border crossed inside both seats' grids rides the relay as
        // every other step, no snap (x-handoff-door 20261024-013234 read
        // about thirty such lines in four minutes for Riverwood's walkers)
        const auto& staying = partOne.worldState.GetNeighborsByPosition(
          msg.data.worldOrCell, static_cast<int16_t>(destPos.x / 4096),
          static_cast<int16_t>(destPos.y / 4096));
        size_t told = 0;
        for (MpObjectReference* listener : moved->GetListeners()) {
          MpActor* viewer = listener ? listener->AsActor() : nullptr;
          if (!viewer || viewer == myActor || staying.count(viewer)) {
            continue;
          }
          const auto viewerUser = partOne.serverState.UserByActor(viewer);
          if (viewerUser == Networking::InvalidUserId) {
            continue;
          }
          partOne.GetSendTarget().Send(viewerUser, teleport, true);
          ++told;
        }
        if (told) {
          spdlog::info(
            "ActionListener::OnUpdateMovement - npc-handoff: {:x}'s "
            "move into {:x} told to {} viewing game(s) as a "
            "Teleport ahead of their unsubscribe",
            moved->GetFormId(), destCell.value_or(0), told);
        }
      }
      actor = moved;
    } else {
      actor =
        ActorUpdatableBy(msg.idx, rawMsgData.userId, MsgType::UpdateMovement);
    }
  }
  if (actor) {
    // a teleport the server made (MpActor::Teleport sets the flag): the
    // player's reports from anywhere else were sent before it, so the
    // movement rule waits for one from where the server put it (thuum
    // docs/verbs/movement-speed.md, arrivals). Upstream sent the next report
    // back with a snap back (kInfinityPos) and a second stale report then
    // took the record back: a server teleport landed a little, or not at
    // all (Eli's playtest twelve, MoveTo). A hosted actor keeps upstream's.
    const bool teleportFlag = actor->GetTeleportFlag();
    actor->SetTeleportFlag(false);
    const bool isPlayersOwn =
      partOne.serverState.ActorByUser(rawMsgData.userId) == actor;

    static const NiPoint3 kInfinityPos = {
      std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::infinity()
    };

    auto& espmFiles = actor->GetParent()->espmFiles;

    const auto& currentPos = actor->GetPos();
    const auto& currentRot = actor->GetAngle();
    const auto& currentCellOrWorld = actor->GetCellOrWorld();

    if (teleportFlag && isPlayersOwn) {
      partOne.ExpectArrival(actor->GetFormId(),
                            currentCellOrWorld.ToFormId(espmFiles),
                            currentPos.x, currentPos.y);
    }

    bool placed = false;
    if (!MovementValidation::Validate(
          partOne, currentPos, currentRot, currentCellOrWorld,
          teleportFlag && !isPlayersOwn
            ? kInfinityPos
            : NiPoint3{ msg.data.pos[0], msg.data.pos[1], msg.data.pos[2] },
          FormDesc::FromFormId(msg.data.worldOrCell, espmFiles),
          rawMsgData.userId, actor, espmFiles, &placed, hostedCellChange)) {
      return;
    }

    // thuum docs/verbs/cast-teleport.md (the teleport snap, playtest
    // twenty-three: a jump under 1024 units slid on every other screen): a
    // move the server vouches for as a jump tells the other games to place
    // the figure at the report that follows, reliably and before it
    if (placed) {
      ActorJumpMessage jump;
      jump.idx = msg.idx;
      for (auto listener : actor->GetActorListeners()) {
        if (listener != actor) {
          listener->GetActorToSendTo().SendToUser(jump, true);
        }
      }
    }

    RelayToListeners(*actor, rawMsgData.unparsed, rawMsgData.unparsedLength,
                     false, rawMsgData.userId);

    // thuum docs/verbs/cell-host.md: the player's grid square may have
    // moved, and with it the cells the server's grid gives it (a cell
    // already loaded that the grid now reaches is granted here)
    if (isPlayersOwn) {
      partOne.RefreshReach(*actor);
    }

    // thuum docs/verbs/map-markers.md: the first movement after a login
    // means the player's own world is up, so its map can be shown, what it
    // learned of its ingredients taught again
    // (docs/verbs/learned-effects.md), its favorites marked again
    // (docs/verbs/favorites.md), its RaceMenu look applied again
    // (docs/verbs/racemenu-sync.md), its actor values and progress
    // (docs/verbs/actor-values.md), and its shouts and words of power
    // (docs/verbs/shouts.md). Its own globals go at the login itself
    // (PartOne::SetUserActor, docs/verbs/player-globals.md): the client
    // holds them until its first in-game update, ahead of the scripts
    // that read them at their first check.
    if (actor == partOne.serverState.ActorByUser(rawMsgData.userId) &&
        actor->TakeMapMarkersPending()) {
      SendMapMarkers(*actor);
      SendIngredientEffects(*actor);
      SendFavorites(*actor);
      SendRaceMenuPreset(*actor);
      SendActorValues(*actor);
      SendShoutRecord(*actor);
    }

    if (!msg.data.isBlocking) {
      actor->IncreaseBlockCount();
    } else {
      actor->ResetBlockCount();
    }

    // a jump the server permitted (thuum docs/verbs/console-commands.md,
    // COC) may land in another cell: the record moves there as a teleport
    // moves it, and SetPos below attaches it to that cell's grid
    const auto newCellOrWorld =
      FormDesc::FromFormId(msg.data.worldOrCell, espmFiles);
    if (newCellOrWorld != actor->GetCellOrWorld()) {
      actor->SetCellOrWorldObsolete(newCellOrWorld);
    }

    actor->SetPos(
      NiPoint3{ msg.data.pos[0], msg.data.pos[1], msg.data.pos[2] },
      SetPosMode::CalledByUpdateMovement);
    // thuum docs/verbs/npc-fate.md (ADR-048's B): a game-file actor's
    // heights from its host's samples, a fall report's evidence
    if (actor->IsFateRespawned()) {
      actor->RecordHeight(msg.data.pos[2], std::chrono::steady_clock::now());
    }
    // thuum docs/verbs/npc-handoff.md (d), ADR-047 D9: the record stands in
    // the destination now, so its simulator is that cell's host; the word
    // goes out per actor (UpdateHoster, npc-puppet.md): the new host's game
    // hears "not hosted by other" and goes live from the relayed point, every
    // other listener, the sender's game first, hears "hosted by other" and
    // demotes its copy. Playtest 34 (2026-10-25): without this the sender's
    // engine walked Sigrid live where the other game held her record.
    if (passed) {
      MpActor* newHost = partOne.worldState.HostOf(*actor);
      actor->UpdateHoster(newHost ? newHost->GetFormId() : 0);
      spdlog::info("ActionListener::OnUpdateMovement - npc-handoff D9: {:x} "
                   "now simulates {:x} in cell {:x}; the sender's game is "
                   "told to demote",
                   newHost ? newHost->GetFormId() : 0u, actor->GetFormId(),
                   partOne.worldState.CellOf(*actor).value_or(0));
    }
    actor->SetAngle(
      NiPoint3{ msg.data.rot[0], msg.data.rot[1], msg.data.rot[2] },
      SetAngleMode::CalledByUpdateMovement);
    actor->SetAnimationVariableBool(
      AnimationVariableBool::kVariable_bInJumpState, msg.data.isInJumpState);
    actor->SetAnimationVariableBool(
      AnimationVariableBool::kVariable__skymp_isWeapDrawn,
      msg.data.isWeapDrawn);
    actor->SetAnimationVariableBool(
      AnimationVariableBool::kVariable_IsBlocking, msg.data.isBlocking);
    actor->SetAnimationVariableBool(
      AnimationVariableBool::kVariable_IsSneaking, msg.data.isSneaking);

    if (actor->GetBlockCount() == 5) {
      actor->SetIsBlockActive(false);
      actor->ResetBlockCount();
    }

    if (msg.data.runMode != "Standing") {
      actor->SetLastAnimEvent(std::nullopt);
    }

    if (partOne.worldState.lastMovUpdateByIdx.size() <= msg.idx) {
      auto newSize = static_cast<size_t>(msg.idx) + 1;
      partOne.worldState.lastMovUpdateByIdx.resize(newSize);
    }
    partOne.worldState.lastMovUpdateByIdx[msg.idx] =
      std::chrono::system_clock::now();
  }
}

void ActionListener::OnUpdateAnimation(const RawMessageData& rawMsgData,
                                       const UpdateAnimationMessage& msg)
{
  MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!myActor) {
    return;
  }

  auto targetActor =
    SendToNeighbours(msg.idx, rawMsgData, false, MsgType::UpdateAnimation);

  if (!targetActor) {
    return;
  }

  // Only the player's own actor goes through the animation system (it spends
  // the player's stamina); a hosted actor keeps its last event too (thuum
  // docs/verbs/npc-stream.md: a later subscriber's CreateActor carries it,
  // as a player's does)
  if (targetActor != myActor) {
    targetActor->SetLastAnimEvent(msg.data);
    return;
  }

  partOne.animationSystem.Process(targetActor, msg.data);
  targetActor->SetLastAnimEvent(msg.data);
}

void ActionListener::OnUpdateAction(const RawMessageData& rawMsgData,
                                    const UpdateActionMessage& msg)
{
  MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!myActor) {
    return;
  }
  // the actor the sender may update (its own, or an NPC it hosts), else
  // nothing; the bytes go as they came (the wire validated them), reliably,
  // to every user listening to the actor but the sender, whose own game
  // performed the action already
  MpActor* actor =
    ActorUpdatableBy(msg.idx, rawMsgData.userId, MsgType::UpdateAction);
  if (!actor) {
    return;
  }
  for (auto listener : actor->GetActorListeners()) {
    if (listener == myActor) {
      continue;
    }
    auto targetUserId = partOne.serverState.UserByActor(listener);
    if (targetUserId != Networking::InvalidUserId) {
      partOne.GetSendTarget().Send(targetUserId, rawMsgData.unparsed,
                                   rawMsgData.unparsedLength, true);
    }
  }
}

// thuum docs/verbs/npc-stream.md (the voice slice, ADR-050): a spoken line
// begins on an actor the sender hosts. The sender's own actor is refused (a
// player's lines are its own game's), the gate is ActorUpdatableBy's (the
// host of the actor's cell), and the bytes go as they came, unreliable (a
// lost line is a lost line, TES3MP's shape), to every listener with a user
// but the host, whose puppet says the same info through the engine's own
// say. Nothing is recorded; one info line a minute per actor with the count.
void ActionListener::OnActorSpeech(const RawMessageData& rawMsgData,
                                   const ActorSpeechMessage& msg)
{
  MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!myActor) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (msg.idx == myActor->GetIdx()) {
    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point>
      lastOwn;
    auto& last = lastOwn[myActor->GetFormId()];
    if (last == std::chrono::steady_clock::time_point() ||
        now - last >= std::chrono::minutes(1)) {
      spdlog::warn("[verb:npc-stream] E_SPEECH_OWN: {:x} sent a spoken line "
                   "for its own actor (idx {}); dropped",
                   myActor->GetFormId(), msg.idx);
      last = now;
    }
    partOne.serverState.CountStream(
      rawMsgData.userId, static_cast<uint8_t>(MsgType::ActorSpeech), false);
    return;
  }
  MpActor* actor =
    ActorUpdatableBy(msg.idx, rawMsgData.userId, MsgType::ActorSpeech);
  if (!actor) {
    return;
  }
  size_t sent = 0;
  for (auto listener : actor->GetActorListeners()) {
    if (listener == myActor) {
      continue;
    }
    auto targetUserId = partOne.serverState.UserByActor(listener);
    if (targetUserId != Networking::InvalidUserId) {
      partOne.GetSendTarget().Send(targetUserId, rawMsgData.unparsed,
                                   rawMsgData.unparsedLength, false);
      ++sent;
    }
  }
  static std::unordered_map<
    uint32_t,
    std::tuple<std::chrono::steady_clock::time_point, uint32_t, uint32_t>>
    lines;
  auto& [since, count, relayed] = lines[actor->GetFormId()];
  ++count;
  relayed += static_cast<uint32_t>(sent);
  if (since == std::chrono::steady_clock::time_point() ||
      now - since >= std::chrono::minutes(1)) {
    spdlog::info("[verb:npc-stream] speech {:x} by {:x}: topic {:x} info "
                 "{:x} to {} listener(s); {} line(s), {} relays since the "
                 "last line",
                 actor->GetFormId(), myActor->GetFormId(), msg.topic, msg.info,
                 sent, count, relayed);
    since = now;
    count = 0;
    relayed = 0;
  }
}

// thuum docs/verbs/npc-talk.md (ADR-052): a player asks for an NPC another
// game simulates, for a dialogue, or gives it back. The gates: the actor an
// NPC (no user, not a player's), alive, not in combat by the host's last
// state, in the claimant's cell or world and within activation reach of its
// record (the activate rule's own bound), the claimant loaded in the actor's
// cell and nobody else's claim on it (wire-rules cells.rs). Granted, the
// claimant simulates the actor: the word names it on every seat (the old
// host's demotes, the claimant's promotes and opens the dialogue), for the
// lease or until the release.
namespace {
// defined with the activation rule below (the reach a dialogue claim shares
// with an activation, wire-rules activation)
float ActivationTargetSize(MpObjectReference& target);
}

void ActionListener::OnActorClaim(const RawMessageData& rawMsgData,
                                  const ActorClaimMessage& msg)
{
  MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!myActor) {
    return;
  }
  MpForm* form = partOne.worldState.LookupFormByIdx(msg.idx);
  MpActor* actor = form ? form->AsActor() : nullptr;
  const auto refuse = [&](const char* code, const std::string& why) {
    spdlog::warn("[verb:npc-talk] {}: {:x} {} idx {} ({:x}); {}", code,
                 myActor->GetFormId(), msg.claim ? "claims" : "releases",
                 msg.idx, actor ? actor->GetFormId() : 0u, why);
  };
  if (!actor) {
    refuse("E_CLAIM_NO_ACTOR", "no such actor");
    return;
  }
  if (!msg.claim) {
    if (!partOne.ReleaseClaim(*myActor, *actor)) {
      refuse("E_CLAIM_NONE", "it holds no claim on it");
      return;
    }
    spdlog::info("[verb:npc-talk] {:x} releases {:x}; the cell's rule names "
                 "{:x}",
                 myActor->GetFormId(), actor->GetFormId(),
                 partOne.SimulatorOf(*actor));
    return;
  }
  if (actor->IsCreatedAsPlayer() ||
      partOne.serverState.UserByActor(actor) != Networking::InvalidUserId) {
    refuse("E_CLAIM_NPC", "a player's actor is nobody's to claim");
    return;
  }
  if (actor->IsDead()) {
    refuse("E_CLAIM_DEAD", "the actor is dead");
    return;
  }
  if (actor->GetHostedCombatTarget() != 0) {
    refuse("E_CLAIM_COMBAT", "the actor fights (its host's last state)");
    return;
  }
  if (myActor->GetCellOrWorld() != actor->GetCellOrWorld()) {
    refuse("E_CLAIM_REACH", "another cell or world");
    return;
  }
  const float distance = (myActor->GetPos() - actor->GetPos()).Length();
  const auto reach = skymp::rules::activation_within_reach(
    distance, ActivationTargetSize(*actor));
  if (!reach.allowed) {
    refuse("E_CLAIM_REACH",
           fmt::format("{:.0f} units from its record, reach {:.0f}", distance,
                       reach.bound));
    return;
  }
  const auto verdict = partOne.ClaimActor(*myActor, *actor);
  switch (verdict) {
    case PartOne::ClaimVerdict::Granted:
    case PartOne::ClaimVerdict::Reclaimed:
      spdlog::info("[verb:npc-talk] {:x} claims {:x} in cell {:x} for a "
                   "dialogue{}; it simulates the actor for the lease",
                   myActor->GetFormId(), actor->GetFormId(),
                   partOne.worldState.CellOf(*actor).value_or(0),
                   verdict == PartOne::ClaimVerdict::Reclaimed ? " (renewed)"
                                                               : "");
      break;
    case PartOne::ClaimVerdict::Held:
      refuse("E_CLAIM_HELD", "another player holds a claim on it");
      break;
    case PartOne::ClaimVerdict::Unloaded:
      refuse("E_CLAIM_UNLOADED", "the claimant has not loaded its cell");
      break;
  }
}

// thuum docs/verbs/npc-stream.md (ADR-050 decision 1, slice 2): a host's
// state for an actor it simulates. The gate is ActorUpdatableBy's (the host
// of the actor's cell); the sender's own actor is refused (a player's values
// go as ChangeValues, with the regeneration crop). The core gathers the facts
// (the record, the hold table's answer per value, the time since the server
// last lowered the health by its own number: the last hit it numbered) and
// wire-rules hosted_state judges: a report that would undo a value the server
// lowered or raised lately is dropped, an equal one changes nothing, a 0 on a
// living record is floored at one point (until ADR-050's deaths-by-the-engine
// train), a dead or respawning record drops the report whole (npc-fate owns
// the transition). The changed values are written as judged (no kill, no
// respawn check) and go as ChangeValues to every listener with a user but
// the host (Rule A: the host's engine has them). The target and the package
// are recorded in memory. One info line a minute per actor with the counts,
// the first state at once (the lab's read).
void ActionListener::OnHostedActorState(const RawMessageData& rawMsgData,
                                        const HostedActorStateMessage& msg)
{
  MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!myActor) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (msg.idx == myActor->GetIdx()) {
    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point>
      lastOwn;
    auto& last = lastOwn[myActor->GetFormId()];
    if (last == std::chrono::steady_clock::time_point() ||
        now - last >= std::chrono::minutes(1)) {
      spdlog::warn("[verb:npc-stream] E_STATE_OWN: {:x} sent a hosted state "
                   "for its own actor (idx {}); a player's values go as "
                   "ChangeValues; dropped",
                   myActor->GetFormId(), msg.idx);
      last = now;
    }
    partOne.serverState.CountStream(
      rawMsgData.userId, static_cast<uint8_t>(MsgType::HostedActorState),
      false);
    return;
  }
  MpActor* actor =
    ActorUpdatableBy(msg.idx, rawMsgData.userId, MsgType::HostedActorState);
  if (!actor) {
    return;
  }

  const ActorValues& recorded = actor->GetActorValues();
  skymp::rules::HostedStateFacts facts{};
  facts.health.reported = msg.health;
  facts.health.recorded = recorded.healthPercentage;
  facts.magicka.reported = msg.magicka;
  facts.magicka.recorded = recorded.magickaPercentage;
  facts.stamina.reported = msg.stamina;
  facts.stamina.recorded = recorded.staminaPercentage;
  // the hold table (docs/verbs/attributes.md): a value the server raised
  // that this host has not reported yet refuses the report; the call that
  // reaches the held value ends the hold, as the players' path has it
  facts.health.held =
    !actor->TakeReportedPercentage(espm::ActorValue::Health, msg.health);
  facts.magicka.held =
    !actor->TakeReportedPercentage(espm::ActorValue::Magicka, msg.magicka);
  facts.stamina.held =
    !actor->TakeReportedPercentage(espm::ActorValue::Stamina, msg.stamina);
  // the server lowers an NPC's health by its own number at a hit it judged
  // (OnWeaponHit stamps the time); magicka and stamina have no such path
  const auto lastHit = actor->GetLastHitTakenTime();
  facts.health.lowered = lastHit.time_since_epoch().count() != 0;
  facts.health.lowered_ago_ms = facts.health.lowered
    ? static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHit)
          .count())
    : 0;
  facts.magicka.lowered = false;
  facts.magicka.lowered_ago_ms = 0;
  facts.stamina.lowered = false;
  facts.stamina.lowered_ago_ms = 0;
  facts.alive = !actor->IsDead() && !actor->IsRespawning();
  facts.one_point = 1.f / std::max(actor->GetMaximumValues().health, 1.f);

  const skymp::rules::HostedStateVerdict verdict =
    skymp::rules::hosted_state_judge(facts);

  static std::unordered_map<
    uint32_t,
    std::tuple<std::chrono::steady_clock::time_point, uint32_t, uint32_t>>
    lines;
  auto& [since, taken, dropped] = lines[actor->GetFormId()];
  if (verdict.dead) {
    ++dropped;
  } else {
    ++taken;
    actor->SetJudgedPercentages(
      verdict.health.changed ? std::optional<float>(verdict.health.record)
                             : std::nullopt,
      verdict.magicka.changed ? std::optional<float>(verdict.magicka.record)
                              : std::nullopt,
      verdict.stamina.changed ? std::optional<float>(verdict.stamina.record)
                              : std::nullopt);
    actor->SetHostedState(msg.combatTarget, msg.package);
    // slice 5 (the gaze and the face): recorded, and the message itself
    // relayed unreliable to every listener with a user but the host, whose
    // puppets aim and make the face (the percentages stay the judged
    // ChangeValues below; a puppet seat ignores the message's values)
    actor->SetHostedGaze(msg.gazeTarget, msg.gazeSlot, msg.emotionType,
                         msg.emotionValue);
    {
      HostedActorStateMessage relay = msg;
      relay.idx = actor->GetIdx();
      for (auto listener : actor->GetActorListeners()) {
        if (listener == myActor ||
            listener->GetUserId() == Networking::InvalidUserId) {
          continue;
        }
        listener->SendToUser(relay, false);
      }
    }
    if (verdict.health.changed || verdict.magicka.changed ||
        verdict.stamina.changed) {
      ChangeValuesMessage out;
      out.idx = actor->GetIdx();
      if (verdict.health.changed) {
        out.data.health = verdict.health.record;
      }
      if (verdict.magicka.changed) {
        out.data.magicka = verdict.magicka.record;
      }
      if (verdict.stamina.changed) {
        out.data.stamina = verdict.stamina.record;
      }
      // every listener with a user but the host (UpdateHoster's loop: an
      // offline listener's GetActorToSendTo would hand it to its host)
      for (auto listener : actor->GetActorListeners()) {
        if (listener == myActor ||
            listener->GetUserId() == Networking::InvalidUserId) {
          continue;
        }
        listener->SendToUser(out, true);
      }
    }
  }
  if (since == std::chrono::steady_clock::time_point() ||
      now - since >= std::chrono::minutes(1)) {
    spdlog::info(
      "[verb:npc-stream] state {:x} by {:x}: health {:.3f}{} magicka {:.3f}{} "
      "stamina {:.3f}{} target {:x} package {:x}{}{}; {} taken, {} dropped "
      "whole since the last line",
      actor->GetFormId(), myActor->GetFormId(), verdict.health.record,
      verdict.health.dropped ? " (report dropped)" : "",
      verdict.magicka.record,
      verdict.magicka.dropped ? " (report dropped)" : "",
      verdict.stamina.record,
      verdict.stamina.dropped ? " (report dropped)" : "", msg.combatTarget,
      msg.package, verdict.floored ? "; floored at one point" : "",
      verdict.dead ? "; the record is dead, the report dropped" : "", taken,
      dropped);
    since = now;
    taken = 0;
    dropped = 0;
  }
}

namespace {
// thuum docs/verbs/character-creation.md: the facts the race rule needs; the
// rule is Rust's (skymp-wire wire-rules appearance, ADR-020)
bool IsAllowedRace(PartOne& partOne, const MpActor& actor, uint32_t raceId)
{
  skymp::rules::RaceFacts facts{};
  facts.has_game_files = partOne.HasEspm();
  if (facts.has_game_files) {
    auto recorded = actor.GetAppearance();
    facts.is_recorded_race = recorded && recorded->raceId == raceId;
    auto race = espm::Convert<espm::RACE>(
      partOne.GetEspm().GetBrowser().LookupById(raceId).rec);
    facts.is_playable_race = race &&
      (race->GetData(partOne.worldState.GetEspmCache()).flags &
       espm::RACE::kPlayable);
  }
  return skymp::rules::race_allowed(facts);
}
}

namespace {
// thuum ADR-026: a RaceMenu look is the one record of a character's face; the
// appearance's head parts, hair colour, weight and face texture set follow
// it. Race, sex, skin and tints stay the appearance's: a look carries no
// race, and RaceMenu's save carries only the tint layers it set itself.
enum class LookVerdict
{
  kDerived,   // the appearance the look implies
  kRefused,   // a part the server lacks or the race may not wear
  kNotJudged, // no game files, or no appearance to derive into
};

// "<plugin>|<id>" to a form id in the server's load order, light plugins
// numbered as the engine numbers them (thuum docs/verbs/light-plugins.md);
// 0 for a plugin the server does not load
uint32_t LookFormId(const PartOne& partOne, const skymp::rules::LookRef& ref)
{
  const std::string plugin(ref.plugin);
  const auto& files = partOne.worldState.espmFiles;
  for (size_t i = 0; i < files.size(); ++i) {
    if (files[i].size() == plugin.size() &&
        std::equal(files[i].begin(), files[i].end(), plugin.begin(),
                   [](char a, char b) {
                     return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
                   })) {
      return FormDesc(ref.id & 0x00ffffff, files[i]).ToFormId(files);
    }
  }
  return 0;
}

skymp::rules::LookPartFacts HeadPartFacts(PartOne& partOne, uint32_t formId,
                                          uint32_t raceId)
{
  skymp::rules::LookPartFacts facts{};
  facts.form = 0;
  facts.valid_for_race = false;
  if (!formId) {
    return facts;
  }
  auto& browser = partOne.GetEspm().GetBrowser();
  auto& cache = partOne.worldState.GetEspmCache();
  const auto part = browser.LookupById(formId);
  const auto hdpt = espm::Convert<espm::HDPT>(part.rec);
  if (!hdpt) {
    return facts;
  }
  const auto data = hdpt->GetData(cache);
  facts.form = formId;
  for (uint32_t extra : data.extraParts) {
    facts.extras.push_back(part.ToGlobalId(extra));
  }
  if (!data.validRaces) {
    facts.valid_for_race = true; // the record gives no answer
    return facts;
  }
  const auto list = browser.LookupById(part.ToGlobalId(data.validRaces));
  if (const auto flst = espm::Convert<espm::FLST>(list.rec)) {
    for (uint32_t race : flst->GetData(cache).formIds) {
      if (list.ToGlobalId(race) == raceId) {
        facts.valid_for_race = true;
        break;
      }
    }
  }
  return facts;
}

LookVerdict AppearanceFromLook(PartOne& partOne, const Appearance& current,
                               const std::string& look, Appearance& out)
{
  if (!partOne.HasEspm()) {
    return LookVerdict::kNotJudged;
  }
  const auto facts = skymp::rules::racemenu_look_facts(rust::Str(look));
  if (!facts.ok) {
    return LookVerdict::kRefused;
  }
  std::vector<skymp::rules::LookPartFacts> parts;
  for (const auto& ref : facts.parts) {
    parts.push_back(
      HeadPartFacts(partOne, LookFormId(partOne, ref), current.raceId));
  }
  const auto derived = skymp::rules::racemenu_derived_head_parts(
    rust::Slice<const skymp::rules::LookPartFacts>(parts.data(),
                                                   parts.size()));
  if (!derived.ok) {
    return LookVerdict::kRefused;
  }
  out = current;
  if (!parts.empty()) {
    out.headpartIds.assign(derived.parts.begin(), derived.parts.end());
  }
  if (facts.has_hair_color) {
    out.hairColor = static_cast<int32_t>(facts.hair_color);
  }
  if (facts.has_weight) {
    out.weight = facts.weight;
  }
  if (facts.has_head_texture) {
    if (const uint32_t texture = LookFormId(partOne, facts.head_texture);
        texture && partOne.GetEspm().GetBrowser().LookupById(texture).rec &&
        partOne.GetEspm().GetBrowser().LookupById(texture).rec->GetType() ==
          "TXST") {
      out.headTextureSetId = texture;
    }
  }
  return LookVerdict::kDerived;
}

// the player's derived appearance to everyone who sees it but the player,
// whose own game already wears it
void SendAppearanceToOthers(PartOne& partOne, MpActor& actor,
                            const Appearance& appearance)
{
  UpdateAppearanceMessage out;
  out.idx = actor.GetIdx();
  out.data = appearance;
  for (auto listener : actor.GetActorListeners()) {
    if (listener == &actor ||
        partOne.serverState.UserByActor(listener) ==
          Networking::InvalidUserId) {
      continue;
    }
    listener->SendToUser(out, true);
  }
}

bool SameAppearanceParts(const Appearance& a, const Appearance& b)
{
  return a.headpartIds == b.headpartIds && a.hairColor == b.hairColor &&
    a.weight == b.weight && a.headTextureSetId == b.headTextureSetId;
}
}

void ActionListener::OnUpdateAppearance(const RawMessageData& rawMsgData,
                                        const UpdateAppearanceMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor || !msg.data.has_value()) {
    return;
  }

  const bool isRaceMenuOpen = actor->IsRaceMenuOpen();
  const bool isAllowed =
    isRaceMenuOpen && IsAllowedRace(partOne, *actor, msg.data->raceId);

  if (isRaceMenuOpen && !isAllowed) {
    spdlog::warn("ActionListener::OnUpdateAppearance - E_APPEARANCE_RACE: "
                 "{:x} chose race {:x}, which the race menu does not offer; "
                 "refused",
                 actor->GetFormId(), msg.data->raceId);
  }

  if (isAllowed) {
    actor->SetRaceMenuOpen(false);
    // thuum ADR-026: the appearance follows the player's recorded look,
    // whichever of the two a menu's close sends first
    Appearance derived;
    const std::string look = actor->GetRaceMenuPreset();
    if (!look.empty() &&
        AppearanceFromLook(partOne, msg.data.value(), look, derived) ==
          LookVerdict::kDerived &&
        !SameAppearanceParts(derived, msg.data.value())) {
      actor->SetAppearance(&derived);
      SendAppearanceToOthers(partOne, *actor, derived);
      spdlog::info("RaceMenu: actor {:x} appearance follows its look "
                   "(ADR-026): {} head parts",
                   actor->GetFormId(), derived.headpartIds.size());
    } else {
      actor->SetAppearance(&msg.data.value());
      SendToNeighbours(msg.idx, rawMsgData, true);
    }
  }

  UpdateAppearanceAttemptEvent updateAppearanceAttemptEvent(
    actor, msg.data.value(), isAllowed);
  updateAppearanceAttemptEvent.Fire(actor->GetParent());
}

namespace {
// thuum docs/verbs/shouts.md: the shout in a voice slot, if it holds one (a
// SHOU; the slot can also hold a power, a SPEL)
std::optional<uint32_t> VoiceShout(const Equipment& equipment,
                                   WorldState& worldState)
{
  if (!equipment.voiceSpell || !*equipment.voiceSpell) {
    return std::nullopt;
  }
  const auto res =
    worldState.GetEspm().GetBrowser().LookupById(*equipment.voiceSpell);
  if (!res.rec || res.rec->GetType() != espm::SHOU::kType) {
    return std::nullopt;
  }
  return *equipment.voiceSpell;
}

// What the server knows of a shout an actor's game reported (wire-rules
// shouts): the shout recorded, the spell's place among its variations
// (SHOU's SNAM), the words the record unlocked, and the variation's
// recovery with ShoutRecoveryMult, the actor value 86 (CommonLibSSE-NG
// include/RE/A/ActorValues.h:94) as the actor-values record and the
// running effects have it, 1 without either
PartOne::VoiceFacts VoiceFactsOf(const MpActor& actor, uint32_t shout,
                                 uint32_t spell, WorldState& worldState)
{
  constexpr uint8_t kNone = 3;
  PartOne::VoiceFacts f;
  f.shoutEquipped = actor.HasShoutEntry(MpActor::ShoutEntry::Shout, shout);
  f.variation = kNone;
  const auto res = worldState.GetEspm().GetBrowser().LookupById(shout);
  const auto data = espm::GetData<espm::SHOU>(shout, &worldState);
  for (uint32_t i = 0; i < data.count; ++i) {
    const auto& variation = data.variations[i];
    if (actor.HasShoutEntry(MpActor::ShoutEntry::WordUnlocked,
                            res.ToGlobalId(variation.word))) {
      f.wordsUnlocked |= static_cast<uint8_t>(1u << i);
    }
    if (f.variation == kNone && res.ToGlobalId(variation.spell) == spell) {
      f.variation = static_cast<uint8_t>(i);
      f.recoverySeconds = variation.recovery;
    }
  }
  f.recoveryMult = actor
                     .GetRecordedActorValueBase(static_cast<uint8_t>(
                       espm::ActorValue::ShoutRecoveryMult))
                     .value_or(1.f) +
    actor.GetEffectModifier(espm::ActorValue::ShoutRecoveryMult);
  // thuum docs/verbs/god-mode.md: TGM is "no shout cooldown" in the game
  // (UESP's Console page), so a god-mode player's shout starts no recovery
  // here either; its game sets none, and its shouts came refused
  if (actor.IsGodMode()) {
    f.recoveryMult = 0.f;
  }
  return f;
}

// The archetype a cast spell carries that its own cast spares (the engine's
// exception: Invisibility before Etherealize, none otherwise)
std::optional<espm::MGEF::EffectType> ConcealmentExceptionOf(
  WorldState* worldState, uint32_t spell)
{
  if (!spell) {
    return std::nullopt;
  }
  bool invisibility = false;
  bool etherealize = false;
  for (const auto& effect : MpActor::EffectsOf(worldState, spell)) {
    try {
      const auto type =
        espm::GetData<espm::MGEF>(effect.effectId, worldState).data.effectType;
      invisibility |= type == espm::MGEF::EffectType::Invisibility;
      etherealize |= type == espm::MGEF::EffectType::Etherealize;
    } catch (std::exception& e) {
      spdlog::warn("ConcealmentExceptionOf - effect {:x} of {:x}: {}",
                   effect.effectId, spell, e.what());
    }
  }
  if (invisibility) {
    return espm::MGEF::EffectType::Invisibility;
  }
  if (etherealize) {
    return espm::MGEF::EffectType::Etherealize;
  }
  return std::nullopt;
}

// thuum docs/verbs/self-cast.md: an invisible or ethereal actor's own act
// (an attack, a cast) ends that state on the server, as the engine ends it in
// its game: Actor::DispelAlteredStates (38819 on 1.6.1170), which the
// caster's SpellCast calls at the fire with one exception, the archetype the
// cast spell itself carries, so a recast of Invisibility replaces the running
// copy instead of ending it (the engine's own replace pass); a hit or an
// activation passes none (the re-analyst on sky-re, 2026-10-16; HYPOTHESIS
// until x-invis-recast). Without the exception the server ended the state
// and told every game at once, and the observer's dispel, landing after its
// figure's fresh cast, ended the new copy: playtest nineteen's "if i REcast
// it i lose invisibility on the other players screen but not my own".
void EndConcealment(PartOne& partOne, MpActor& actor, uint32_t castSpell = 0)
{
  const auto exception = ConcealmentExceptionOf(actor.GetParent(), castSpell);
  bool ended = false;
  if (exception == espm::MGEF::EffectType::Invisibility) {
    ended =
      actor.EndRunningEffectsOfTypes({ espm::MGEF::EffectType::Etherealize });
  } else if (exception == espm::MGEF::EffectType::Etherealize) {
    ended =
      actor.EndRunningEffectsOfTypes({ espm::MGEF::EffectType::Invisibility });
  } else {
    ended =
      actor.EndRunningEffectsOfTypes({ espm::MGEF::EffectType::Invisibility,
                                       espm::MGEF::EffectType::Etherealize });
  }
  if (ended) {
    spdlog::info("ActionListener - {:x} acts: its invisibility and ethereal "
                 "state end{}",
                 actor.GetFormId(),
                 exception ? " (but the one its own spell carries)" : "");
  }
  // thuum docs/verbs/shadowbond.md: a bonded actor's act is the mod's trigger
  // for the swap, whether or not the server holds its concealment (train-27
  // held none: the invisibility was the mod's script's, run in the caster's
  // game alone, and the gate on `ended` never opened for the caster's own
  // attack, x-shadowbond 20261019-081315 on 9fb0732f; train-28 records the
  // bond's proc on both, so `ended` opens too, and the break is the same).
  // The bond spell's own cast is not an act against it: the bond is
  // recorded at that cast's hit, after this
  if (!partOne.IsBondSpell(castSpell)) {
    partOne.BreakBondsOf(actor.GetFormId());
  }
}

// thuum docs/verbs/self-cast.md: a fire-and-forget spell of Self delivery
// (a shout's included) runs its effects on its caster on the server, as a
// drink's do: the server holds them and their time, every game sees them
void RunSelfSpell(WorldState& worldState, MpActor& caster, uint32_t spellId)
{
  try {
    const auto spell = espm::GetData<espm::SPEL>(spellId, &worldState);
    if (!spell.spellItem ||
        spell.spellItem->delivery != espm::SPEL::Delivery::Self) {
      return;
    }
    const bool concentration =
      spell.spellItem->castType == espm::SPEL::CastType::Concentration;
    if (!concentration &&
        spell.spellItem->castType != espm::SPEL::CastType::FireAndForget) {
      return;
    }
    // thuum docs/verbs/self-cast.md (r-shock-nova 20261018-013824, the
    // engine alone: two Shock Novas left their caster's health whole while
    // the bandit beside it took every effect): a Self burst's hostile area
    // effects are for those around the caster, claimed as its hits, never
    // for the caster itself
    auto effects = MpActor::EffectsOf(&worldState, spellId);
    effects.erase(std::remove_if(effects.begin(), effects.end(),
                                 [&](const espm::Effects::Effect& effect) {
                                   return MpActor::IsHostileArea(&worldState,
                                                                 effect);
                                 }),
                  effects.end());
    if (concentration) {
      // thuum docs/verbs/self-cast.md (train-31; playtest twenty-eight's
      // heals): a Self concentration spell (vanilla Healing) runs on the
      // server as a stream at the record's magnitude a second for as long
      // as the cast holds: its end message ends it (EndSelfStream), or the
      // cap when that message is lost. A recast restarts it
      caster.EndRunningEffectsOfSource(spellId);
      caster.ApplyEffects(spellId, effects, 1.f, caster.GetFormId(), false,
                          false, skymp::rules::self_stream_cap_seconds());
      spdlog::info("ActionListener - {:x} casts {:x} on itself as a stream: "
                   "its effects run on the server until the cast ends",
                   caster.GetFormId(), spellId);
      return;
    }
    caster.ApplyEffects(spellId, effects, 1.f, caster.GetFormId(), false);
    spdlog::info("ActionListener - {:x} casts {:x} on itself: its effects run "
                 "on the server",
                 caster.GetFormId(), spellId);
  } catch (std::exception& e) {
    spdlog::warn("ActionListener - self spell {:x}: {}", spellId, e.what());
  }
}

// the end of a Self concentration cast ends its stream on the server
// (RunSelfSpell); a fire-and-forget Self spell's end message (its cast
// animation's) ends nothing, its effects run their recorded time
void EndSelfStream(WorldState& worldState, MpActor& caster, uint32_t spellId)
{
  try {
    const auto spell = espm::GetData<espm::SPEL>(spellId, &worldState);
    if (!spell.spellItem ||
        spell.spellItem->delivery != espm::SPEL::Delivery::Self ||
        spell.spellItem->castType != espm::SPEL::CastType::Concentration) {
      return;
    }
    if (caster.EndRunningEffectsOfSource(spellId)) {
      spdlog::info("ActionListener - {:x} ends {:x} on itself: its stream on "
                   "the server ends with the cast",
                   caster.GetFormId(), spellId);
    }
  } catch (std::exception& e) {
    spdlog::warn("ActionListener - self stream's end {:x}: {}", spellId,
                 e.what());
  }
}

// thuum docs/verbs/marksman.md, spell-cast.md: how far from where the server
// has an actor its own launch may leave from (a shooter's bow, a caster's
// magic node): the node's height above the recorded position and that
// position's age
constexpr float kMaxLaunchOffsetUnits = 512.f;

// thuum docs/verbs/spell-cast.md: how far from its magic node a player's
// target-location cast reaches: its game picks the point with a ray along
// the caster's aim as long as the spell's range (SPIT) x 22 units, 5000 for
// a range of 0 (the re-analyst on sky-re, 2026-10-16, 1.6.1170; HYPOTHESIS
// until a lab run); none for a spell without a record to read it from
std::optional<float> LaunchRay(WorldState& worldState, uint32_t spellId)
{
  constexpr float kUnitsPerRange = 22.f;
  constexpr float kRayWithoutRange = 5000.f;
  try {
    const auto spell = espm::GetData<espm::SPEL>(spellId, &worldState);
    if (!spell.spellItem || !std::isfinite(spell.spellItem->range)) {
      return std::nullopt;
    }
    return spell.spellItem->range > 0.f
      ? spell.spellItem->range * kUnitsPerRange
      : kRayWithoutRange;
  } catch (std::exception& e) {
    spdlog::warn("ActionListener - launch of spell {:x}: {}", spellId,
                 e.what());
    return std::nullopt;
  }
}

// Whether `spell` is a variation of the shout in the voice slot
bool IsVoiceVariation(const Equipment& equipment, uint32_t spell,
                      WorldState& worldState)
{
  const auto shout = VoiceShout(equipment, worldState);
  if (!shout) {
    return false;
  }
  const auto res = worldState.GetEspm().GetBrowser().LookupById(*shout);
  const auto data = espm::GetData<espm::SHOU>(*shout, &worldState);
  for (uint32_t i = 0; i < data.count; ++i) {
    if (res.ToGlobalId(data.variations[i].spell) == spell) {
      return true;
    }
  }
  return false;
}

const char* VoiceRefusalCode(PartOne::VoiceCheck check)
{
  switch (check) {
    case PartOne::VoiceCheck::NoShout:
      return "E_SHOUT_UNKNOWN";
    case PartOne::VoiceCheck::NotAVariation:
      return "E_SHOUT_SPELL";
    case PartOne::VoiceCheck::WordLocked:
      return "E_SHOUT_WORD";
    case PartOne::VoiceCheck::Recovering:
      return "E_SHOUT_RECOVERY";
    default:
      return "accepted";
  }
}
}

void ActionListener::OnUpdateEquipment(const RawMessageData& rawMsgData,
                                       const UpdateEquipmentMessage& msg)
{
  MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!myActor) {
    return;
  }
  // thuum docs/verbs/npc-stream.md (ADR-050 decision 1, slice 3): an idx that
  // is not the sender's own names an actor it hosts (ActorUpdatableBy's
  // gate): its equipment is the host's engine's word, checked against the
  // actor's own record (the items it wears present in its inventory, the
  // spells its base knows) and written to the actor's record (not the
  // sender's: an idx was resolved to the sender's own actor here, the latent
  // corruption cell-host noted). A refusal drops the report with one warn a
  // minute per actor and sends the host nothing (Rule A: nothing writes to
  // an actor the host's engine runs; SetInventory has no idx and would
  // replace the host player's own inventory, and a snippet on an NPC runs
  // nowhere)
  const bool hosted = msg.idx != myActor->GetIdx();
  MpActor* actor = hosted
    ? ActorUpdatableBy(msg.idx, rawMsgData.userId, MsgType::UpdateEquipment)
    : myActor;
  if (!actor) {
    return;
  }

  bool isAllowed = true;
  const auto actorFormId = actor->GetFormId();
  const Equipment& data = msg.data;
  const Inventory& equipmentInv = data.inv;
  uint32_t leftSpell = data.leftSpell.value_or(0);
  uint32_t rightSpell = data.rightSpell.value_or(0);
  uint32_t voiceSpell = data.voiceSpell.value_or(0);
  uint32_t instantSpell = data.instantSpell.value_or(0);

  enum class SpellSlotId : size_t
  {
    Left = 0,
    Right,
    Voice,
    Instant,
    kCount
  };

  std::array<uint32_t, static_cast<size_t>(SpellSlotId::kCount)>
    spellIdsToRemove = {};

  if (leftSpell > 0 && !actor->IsSpellLearned(leftSpell)) {
    spdlog::warn("ActionListener::OnUpdateEquipment {:x} - rejected equipment "
                 "update: spell {:x} is not learned",
                 actorFormId, leftSpell);
    isAllowed = false;
    spellIdsToRemove[static_cast<size_t>(SpellSlotId::Left)] = leftSpell;
  }

  if (rightSpell > 0 && !actor->IsSpellLearned(rightSpell)) {
    spdlog::warn("ActionListener::OnUpdateEquipment {:x} - rejected equipment "
                 "update: spell {:x} is not learned",
                 actorFormId, rightSpell);
    isAllowed = false;
    spellIdsToRemove[static_cast<size_t>(SpellSlotId::Right)] = rightSpell;
  }

  // thuum docs/verbs/shouts.md: the voice slot holds a power or a shout; a
  // shout is the player's when the server's record has it (the console's
  // AddShout for an admin), and one it lacks leaves the player's game
  const bool voiceIsShout = VoiceShout(data, partOne.worldState).has_value();
  if (voiceSpell > 0 &&
      !(voiceIsShout
          ? actor->HasShoutEntry(MpActor::ShoutEntry::Shout, voiceSpell)
          : actor->IsSpellLearned(voiceSpell))) {
    spdlog::warn("ActionListener::OnUpdateEquipment {:x} - rejected equipment "
                 "update: {} {:x} is not learned",
                 actorFormId, voiceIsShout ? "shout" : "spell", voiceSpell);
    isAllowed = false;
    spellIdsToRemove[static_cast<size_t>(SpellSlotId::Voice)] = voiceSpell;
  }

  if (instantSpell > 0 && !actor->IsSpellLearned(instantSpell)) {
    spdlog::warn("ActionListener::OnUpdateEquipment {:x} - rejected equipment "
                 "update: spell {:x} is not learned",
                 actorFormId, instantSpell);
    isAllowed = false;
    spellIdsToRemove[static_cast<size_t>(SpellSlotId::Instant)] = instantSpell;
  }

  std::vector<uint32_t> itemIdsToUnequip;

  const auto& inventory = actor->GetInventory();
  for (auto& entry : equipmentInv.entries) {
    // a hosted actor's report names what it wears; its engine's whole
    // inventory is npc-fate's record, not this message's
    if (hosted && entry.GetWorn() == Inventory::Worn::None) {
      continue;
    }
    if (!inventory.HasItem(entry.baseId)) {
      spdlog::warn(
        "ActionListener::OnUpdateEquipment {:x} - rejected equipment "
        "update: inventory does not contain item {:x}",
        actorFormId, entry.baseId);
      isAllowed = false;
      break;
    }
  }

  if (isAllowed) {
    auto worldState = actor->GetParent();
    if (worldState) {
      uint32_t occupiedSlots = 0;
      // Track which item owns each bit so we can report conflicts
      std::array<uint32_t, 32> slotOwner = {};
      for (auto& entry : equipmentInv.entries) {
        if (entry.GetWorn() == Inventory::Worn::None) {
          continue;
        }
        auto lookupRes =
          worldState->GetEspm().GetBrowser().LookupById(entry.baseId);
        if (!lookupRes.rec || lookupRes.rec->GetType() != espm::ARMO::kType) {
          continue;
        }
        auto armoData = espm::GetData<espm::ARMO>(entry.baseId, worldState);
        uint32_t bodyPartFlags = 0;
        if (armoData.bod2.present) {
          bodyPartFlags = armoData.bod2.bodyPartFlags;
        } else if (armoData.bodt.present) {
          bodyPartFlags = armoData.bodt.bodyPartFlags;
        }
        if (bodyPartFlags == 0) {
          continue;
        }
        uint32_t overlap = occupiedSlots & bodyPartFlags;
        if (overlap) {
          // Collect all conflicting item IDs from slotOwner
          std::unordered_set<uint32_t> conflictingItems;
          conflictingItems.insert(entry.baseId);
          for (int bit = 0; bit < 32; ++bit) {
            if (overlap & (1u << bit)) {
              conflictingItems.insert(slotOwner[bit]);
            }
          }
          std::string conflictList;
          for (uint32_t id : conflictingItems) {
            if (!conflictList.empty()) {
              conflictList += ", ";
            }
            conflictList += fmt::format("{:x}", id);
          }
          std::string binaryStr(32, '0');
          for (int bit = 31; bit >= 0; --bit) {
            if (overlap & (1u << (31 - bit))) {
              binaryStr[bit] = '1';
            }
          }
          spdlog::warn(
            "ActionListener::OnUpdateEquipment {:x} - rejected equipment "
            "update: items [{}] share armor slot flags {:x} (0b{})",
            actorFormId, conflictList, overlap, binaryStr);
          isAllowed = false;
          for (uint32_t id : conflictingItems) {
            itemIdsToUnequip.push_back(id);
          }
          break;
        }
        for (int bit = 0; bit < 32; ++bit) {
          if (bodyPartFlags & (1u << bit)) {
            slotOwner[bit] = entry.baseId;
          }
        }
        occupiedSlots |= bodyPartFlags;
      }
    }
  }

  if (isAllowed) {
    SendToNeighbours(msg.idx, rawMsgData, true);
    actor->SetEquipment(data);
    if (hosted) {
      uint32_t right = 0;
      uint32_t left = 0;
      size_t worn = 0;
      for (auto& entry : equipmentInv.entries) {
        const auto w = entry.GetWorn();
        if (w == Inventory::Worn::None) {
          continue;
        }
        ++worn;
        if (w == Inventory::Worn::Right) {
          right = entry.baseId;
        } else {
          left = entry.baseId;
        }
      }
      spdlog::info("[verb:npc-stream] equipment {:x} by {:x}: {} worn, "
                   "right {:x} left {:x}, spells {:x} {:x}, numChanges {}",
                   actorFormId, myActor->GetFormId(), worn, right, left,
                   rightSpell, leftSpell, data.numChanges);
    }
  } else if (hosted) {
    static std::unordered_map<
      uint32_t, std::pair<std::chrono::steady_clock::time_point, uint32_t>>
      refusals;
    const auto now = std::chrono::steady_clock::now();
    auto& [since, count] = refusals[actorFormId];
    ++count;
    if (since == std::chrono::steady_clock::time_point() ||
        now - since >= std::chrono::minutes(1)) {
      spdlog::warn("[verb:npc-stream] E_EQUIP_REFUSED: {:x}'s equipment by "
                   "{:x} refused (the lines above say why); the record "
                   "stands, nothing sent to the host; {} since the last line",
                   actorFormId, myActor->GetFormId(), count);
      partOne.serverState.CountStream(
        rawMsgData.userId, static_cast<uint8_t>(MsgType::UpdateEquipment),
        false);
      since = now;
      count = 0;
    }
  } else {
    actor->SendInventoryUpdate();

    for (size_t slot = 0; slot < spellIdsToRemove.size(); ++slot) {
      const uint32_t spellId = spellIdsToRemove[slot];
      if (spellId == 0) {
        continue;
      }
      const bool shout =
        slot == static_cast<size_t>(SpellSlotId::Voice) && voiceIsShout;
      SpSnippetObjectArgument spellArg;
      spellArg.formId = spellId;
      spellArg.type = shout ? "Shout" : "Spell";
      std::vector<std::optional<
        std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
        args;
      args.push_back(spellArg);
      SpSnippet("Actor", shout ? "RemoveShout" : "RemoveSpell", args,
                actor->GetFormId())
        .Execute(actor, SpSnippetMode::kNoReturnResult);
    }

    // Calculate diff between current (server) equipment and new (rejected)
    // equipment. Items worn in the new set but not in the current set need
    // to be unequipped on the client to revert the unauthorized change.
    {
      const auto& currentEquip = actor->GetEquipment().inv;

      std::unordered_set<uint32_t> currentWornIds;
      for (const auto& entry : currentEquip.entries) {
        if (entry.GetWorn() != Inventory::Worn::None) {
          currentWornIds.insert(entry.baseId);
        }
      }

      for (const auto& entry : equipmentInv.entries) {
        if (entry.GetWorn() != Inventory::Worn::None &&
            currentWornIds.find(entry.baseId) == currentWornIds.end()) {
          spdlog::info(
            "ActionListener::OnUpdateEquipment {:x} - unequipping item {:x} "
            "(not in current equipment, unauthorized change)",
            actorFormId, entry.baseId);
          itemIdsToUnequip.push_back(entry.baseId);
        }
      }

      // TODO: consider doing EquipItem for items worn in current equipment
      // but not worn in the new equipment (client tried to unequip them)
    }

    for (uint32_t itemId : itemIdsToUnequip) {
      SpSnippetObjectArgument itemArg;
      itemArg.formId = itemId;
      itemArg.type = "Form";
      std::vector<std::optional<
        std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
        args;
      args.push_back(itemArg);
      args.push_back(false);
      args.push_back(true);
      SpSnippet("Actor", "UnequipItem", args, actor->GetFormId())
        .Execute(actor, SpSnippetMode::kNoReturnResult);
    }
  }

  UpdateEquipmentAttemptEvent updateEquipmentAttemptEvent(actor, data,
                                                          isAllowed);
  updateEquipmentAttemptEvent.Fire(actor->GetParent());
}

namespace {
// thuum docs/verbs/activation-reach.md: the target's size, for the reach rule
// in Rust (skymp-wire wire-rules activation, ADR-020): the farthest point of
// its base record's bounds, times the reference's scale
float ActivationTargetSize(MpObjectReference& target)
{
  float size = 0.f;
  if (auto worldState = target.GetParent();
      worldState && worldState->HasEspm()) {
    auto& browser = worldState->GetEspm().GetBrowser();
    if (auto base = browser.LookupById(target.GetBaseId()).rec) {
      if (auto bounds =
            espm::GetObjectBounds(base, worldState->GetEspmCache())) {
        size = espm::BoundsRadius(*bounds);
      }
    }
    if (target.IsEspmForm() && !target.AsActor()) {
      size *= espm::GetData<espm::REFR>(target.GetFormId(), worldState).scale;
    }
  }
  return size;
}
}

void ActionListener::OnActivate(const RawMessageData& rawMsgData,
                                const ActivateMessage& msg)
{
  if (!partOne.HasEspm())
    throw std::runtime_error("No loaded esm or esp files are found");

  const auto ac = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!ac)
    throw std::runtime_error("Can't do this without Actor attached");

  // thuum docs/verbs/npc-fate.md (ADR-048's B, the hazard evidence): a
  // reference's activation of itself is a trap trigger's firing (every
  // vanilla trigger script activates itself, and the engine forwards it to
  // the trap with the plate as the activator; the stepping actor is never
  // the action reference, so no activation names the victim). The game
  // hosting the trigger's cell reports it; it is noted where and when as a
  // hazard death's evidence (R2) and nothing is activated here.
  if (msg.data.caster == msg.data.target && msg.data.caster != 0x14) {
    auto trigger = std::dynamic_pointer_cast<MpObjectReference>(
      partOne.worldState.LookupFormById(
        static_cast<uint32_t>(msg.data.target)));
    if (!trigger) {
      return;
    }
    const auto cell = partOne.worldState.CellOf(
      trigger->GetCellOrWorld().ToFormId(partOne.worldState.espmFiles),
      trigger->GetPos());
    if (!cell) {
      return;
    }
    const auto host = partOne.GetCellHost(*cell);
    if (!host.hosted || host.player != ac->GetFormId()) {
      spdlog::debug("ActionListener::OnActivate - trigger {:x} fired itself, "
                    "reported by {:x}, which does not host cell {:x}; dropped",
                    trigger->GetFormId(), ac->GetFormId(), *cell);
      return;
    }
    NoteActivation(*trigger, std::chrono::steady_clock::now());
    spdlog::info("[verb:npc-fate] trigger {:x} fired itself in cell {:x} (its "
                 "host {:x} reports): noted as a hazard death's evidence",
                 trigger->GetFormId(), *cell, ac->GetFormId());
    return;
  }

  // thuum docs/verbs/cell-host.md: a hosted caster is its cell's host's;
  // an actor a user plays is nobody's to report for
  uint32_t hosterId = 0;
  if (msg.data.caster != 0x14) {
    const auto& casterForm = partOne.worldState.LookupFormById(
      static_cast<uint32_t>(msg.data.caster));
    MpActor* casterActor = casterForm ? casterForm->AsActor() : nullptr;
    const bool played = casterActor &&
      partOne.serverState.UserByActor(casterActor) !=
        Networking::InvalidUserId;
    MpActor* host = casterActor && !played
      ? partOne.worldState.HostOf(*casterActor)
      : nullptr;
    hosterId = host ? host->GetFormId() : 0;
    if (hosterId != ac->GetFormId()) {
      std::stringstream ss;
      ss << std::hex << "Bad hoster is attached to caster 0x"
         << msg.data.caster << ", expected 0x" << ac->GetFormId()
         << ", but found 0x" << hosterId;
      throw std::runtime_error(ss.str());
    }
  }

  auto targetPtr = std::dynamic_pointer_cast<MpObjectReference>(
    partOne.worldState.LookupFormById(static_cast<uint32_t>(msg.data.target)));
  if (!targetPtr)
    return;

  MpObjectReference& caster = msg.data.caster == 0x14
    ? *ac
    : partOne.worldState.GetFormAt<MpObjectReference>(
        static_cast<uint32_t>(msg.data.caster));

  // A client's first activation must be within reach; closing a container
  // (the second activation) is not checked, so nobody is left holding one.
  // Across cells or worlds a distance means nothing: Activate's own check
  // refuses that, with its own error.
  if (!msg.data.isSecondActivation &&
      caster.GetCellOrWorld() == targetPtr->GetCellOrWorld()) {
    const float distance = (caster.GetPos() - targetPtr->GetPos()).Length();
    const auto verdict = skymp::rules::activation_within_reach(
      distance, ActivationTargetSize(*targetPtr));
    if (!verdict.allowed) {
      spdlog::warn("ActionListener::OnActivate - E_ACTIVATE_REACH: {:x} "
                   "activates {:x} from {:.0f} units, reach {:.0f}; refused",
                   caster.GetFormId(), targetPtr->GetFormId(), distance,
                   verdict.bound);
      return;
    }
  }

  // thuum docs/verbs/npc-fate.md (ADR-048's B): where things were set off
  // lately, a hazard death's evidence
  if (!msg.data.isSecondActivation) {
    NoteActivation(*targetPtr, std::chrono::steady_clock::now());
  }

  constexpr bool kDefaultProcessingOnlyFalse = false;
  // a hosted caster's activation is its host's report (npc-handoff D6)
  const bool reportedByHost = msg.data.caster != 0x14;
  targetPtr->Activate(caster, kDefaultProcessingOnlyFalse,
                      msg.data.isSecondActivation, reportedByHost);
  // thuum docs/verbs/self-cast.md: an actor's activation ends its
  // concealment (TESObjectREFR::ActivateRef 19796 calls DispelAlteredStates
  // for an actor activator, with no exception; pick-up goes the same way;
  // HYPOTHESIS until a lab run); the player's own here, a hosted actor's in
  // its host's game
  if (msg.data.caster == 0x14) {
    EndConcealment(partOne, *ac);
  }
  if (hosterId) {
    auto actor =
      std::dynamic_pointer_cast<MpActor>(partOne.worldState.LookupFormById(
        static_cast<uint32_t>(msg.data.caster)));
    if (actor) {
      actor->EquipBestWeapon();
    }
  }
}

void ActionListener::OnPutItem(const RawMessageData& rawMsgData,
                               const PutItemMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return;
  }

  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(msg.target);

  auto worldState = actor->GetParent();
  if (!worldState) {
    return spdlog::error("No WorldState attached");
  }

  if (worldState->HasKeyword(msg.baseId, "SweetCantDrop")) {
    return spdlog::error("Attempt to put SweetCantDrop item {:x}",
                         actor->GetFormId());
  }

  Inventory::Entry entry;
  entry.baseId = msg.baseId;
  entry.count = msg.count;
  static_cast<Inventory::ExtraData&>(entry) =
    static_cast<const Inventory::ExtraData&>(msg);
  entry.SetWorn(Inventory::Worn::None);

  ref.PutItem(*actor, entry);
}

void ActionListener::OnTakeItem(const RawMessageData& rawMsgData,
                                const TakeItemMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return;
  }

  auto& ref = partOne.worldState.GetFormAt<MpObjectReference>(msg.target);

  auto worldState = actor->GetParent();
  if (!worldState) {
    return spdlog::error("No WorldState attached");
  }

  if (worldState->HasKeyword(msg.baseId, "SweetCantDrop")) {
    return spdlog::error("Attempt to take SweetCantDrop item {:x}",
                         actor->GetFormId());
  }

  Inventory::Entry entry;
  entry.baseId = msg.baseId;
  entry.count = msg.count;
  static_cast<Inventory::ExtraData&>(entry) =
    static_cast<const Inventory::ExtraData&>(msg);

  ref.TakeItem(*actor, entry);
}

namespace {
const char* RestRefusalName(skymp::rules::RestRefusal refusal)
{
  switch (refusal) {
    case skymp::rules::RestRefusal::Hours:
      return "E_REST_HOURS";
    case skymp::rules::RestRefusal::Off:
      return "E_REST_OFF";
    case skymp::rules::RestRefusal::Dead:
      return "E_REST_DEAD";
    case skymp::rules::RestRefusal::Fighting:
      return "E_REST_FIGHTING";
    default:
      return "E_REST";
  }
}
}

// A player waited or slept (thuum docs/verbs/rest.md, ADR-021 decision 2):
// the shared clock does not move; the server checks the rest (R1) and gives
// the player its recovery (R0), each attribute's regeneration over the rested
// hours at the rates CropRegeneration judges by.
void ActionListener::OnRestIntent(const RawMessageData& rawMsgData,
                                  const RestIntentMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return spdlog::warn("OnRestIntent - no actor for user {}",
                        rawMsgData.userId);
  }

  // the last hit the player dealt or took, as the server saw it
  const auto now = std::chrono::steady_clock::now();
  const auto lastHit = std::max(actor->GetLastHitTime(std::nullopt),
                                actor->GetLastHitTakenTime());
  const bool hasHit = lastHit != std::chrono::steady_clock::time_point{};

  // thuum docs/verbs/sleep.md: a sleep is the server's call, from the bed
  // the player last activated; the client's flag comes from a furniture test
  // vanilla beds never pass
  const auto [bedId, bedAt] = actor->GetLastBed();
  std::shared_ptr<MpObjectReference> bed;
  if (bedId) {
    bed = std::dynamic_pointer_cast<MpObjectReference>(
      partOne.worldState.LookupFormById(bedId));
  }
  skymp::rules::BedFacts bedFacts{};
  bedFacts.has_bed = bed != nullptr;
  bedFacts.since_bed_activation_ms = bed
    ? static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - bedAt)
          .count())
    : 0;
  bedFacts.distance_to_bed =
    bed && bed->GetCellOrWorld() == actor->GetCellOrWorld()
    ? (bed->GetPos() - actor->GetPos()).Length()
    : std::numeric_limits<float>::infinity();
  const bool slept = skymp::rules::rest_slept(bedFacts);

  skymp::rules::RestFacts facts{};
  facts.hours = msg.hours;
  facts.sleep = slept;
  facts.is_dead = actor->IsDead();
  facts.has_hit = hasHit;
  facts.since_last_hit_ms = hasHit
    ? static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(now - lastHit)
          .count())
    : 0;
  // thuum ADR-023: no rest in a fight with another player, however long ago
  // its last hit, until it ends (a minute quiet, or walked apart)
  facts.in_fight = partOne.GetFights().in_fight(actor->GetFormId());
  const auto refusal =
    skymp::rules::rest_check(partOne.GetRestSettings(), facts);
  if (refusal != skymp::rules::RestRefusal::Allowed) {
    return spdlog::info("{}: user {} actor {:x} rest of {} h refused",
                        RestRefusalName(refusal), rawMsgData.userId,
                        actor->GetFormId(), msg.hours);
  }

  // thuum docs/verbs/self-cast.md: the rest runs the player's magic effects
  // for its time, so one of a minute ends in any rest, as in the player's
  // game (playtest fifteen's Invisibility); first, so a buff's end moves the
  // maximum before the rest's percentages
  actor->AdvanceEffects(skymp::rules::rest_effect_seconds(msg.hours));

  const auto after = [&](float percentage, RegenRate r) {
    skymp::rules::Regen regen{};
    regen.percentage = percentage;
    regen.rate = r.rate;
    regen.rate_mult = r.rateMult;
    return skymp::rules::rest_after(regen, msg.hours);
  };
  ActorValues values = actor->GetActorValues();
  values.healthPercentage =
    after(values.healthPercentage, GetHealthRegenRate(actor));
  values.magickaPercentage =
    after(values.magickaPercentage, GetMagickaRegenRate(actor));
  values.staminaPercentage =
    after(values.staminaPercentage, GetStaminaRegenRate(actor));
  actor->NetSetPercentages(values, nullptr, std::nullopt);

  // the bed's use is over: forget it
  actor->SetLastBed(0, {});
  const uint64_t restedMs =
    skymp::rules::rested_ms(slept, partOne.GetGameTime().timeScale);
  if (restedMs > 0) {
    GrantRested(*actor, restedMs);
  }

  spdlog::info("Rest: user {} actor {:x} {} {} h, percentages now {} {} {}{}",
               rawMsgData.userId, actor->GetFormId(),
               slept ? "slept" : "waited", msg.hours, values.healthPercentage,
               values.magickaPercentage, values.staminaPercentage,
               restedMs > 0 ? fmt::format(", Rested for {} s", restedMs / 1000)
                            : std::string());
}

namespace {
void SendSpellSnippet(MpActor& actor, const char* function, uint32_t spellId)
{
  SpSnippetObjectArgument spell;
  spell.formId = spellId;
  spell.type = "Spell";
  std::vector<std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
    args{ spell };
  if (std::string_view(function) == "AddSpell") {
    args.push_back(false); // abVerbose
  }
  SpSnippet("Actor", function, args, actor.GetFormId())
    .Execute(&actor, SpSnippetMode::kNoReturnResult);
}
}

namespace {
// The STAT every map marker reference places (Skyrim.esm 0x00000010
// "MapMarker", lab/esm.py, 2026-10-05).
constexpr uint32_t kMapMarkerBase = 0x00000010;

// A record header's Deleted flag (CommonLibSSE-NG include/RE/T/TESForm.h:62,
// RecordFlags kDeleted = 1 << 5; TESObjectREFR.h:171 the same for
// references; UESP, "Skyrim Mod:Mod File Format", record flags).
constexpr uint32_t kRecordDeleted = 0x00000020;

// The master files' map markers of a type around the player, by reference
// id; a later file's record of the same reference replaces an earlier one's.
// libespm keys references by position over 4096, truncated (Browser.cpp), so
// the scan covers the cells the discovery range reaches, plus one.
std::vector<skymp::rules::MarkerCandidate> MapMarkersNear(
  WorldState& worldState, const MpActor& actor, uint16_t markerType)
{
  const auto& br = worldState.GetEspm().GetBrowser();
  auto& cache = worldState.GetEspmCache();
  const uint32_t world = actor.GetCellOrWorld().ToFormId(worldState.espmFiles);
  const NiPoint3& pos = actor.GetPos();
  const auto cell = [](float v) { return static_cast<int16_t>(v / 4096); };
  const int16_t reach =
    static_cast<int16_t>(skymp::rules::map_marker_range() / 4096) + 1;
  const int16_t cx = cell(pos.x);
  const int16_t cy = cell(pos.y);

  std::map<uint32_t, skymp::rules::MarkerCandidate> found;
  for (size_t i = 0; i < worldState.espmFiles.size(); ++i) {
    const auto* combMapping = br.GetCombMapping(i);
    const auto* rawMapping = br.GetRawMapping(i);
    const uint32_t localWorld = espm::utils::GetMappedId(world, *rawMapping);
    for (int16_t x = cx - reach; x <= cx + reach; ++x) {
      for (int16_t y = cy - reach; y <= cy + reach; ++y) {
        auto records = br.GetRecordsAtPos(localWorld, x, y);
        for (const auto* rec : *records[i]) {
          if (rec->GetType() != "REFR") {
            continue;
          }
          const auto* refr = reinterpret_cast<const espm::REFR*>(rec);
          const auto data = refr->GetData(cache);
          if (!data.loc ||
              espm::utils::GetMappedId(data.baseId, *combMapping) !=
                kMapMarkerBase) {
            continue;
          }
          const uint32_t id =
            espm::utils::GetMappedId(rec->GetId(), *combMapping);
          if ((rec->GetFlags() & kRecordDeleted) ||
              static_cast<uint16_t>(data.mapMarkerType) != markerType) {
            found.erase(id);
            continue;
          }
          skymp::rules::MarkerCandidate c{};
          c.refr_id = id;
          c.x = data.loc->pos[0];
          c.y = data.loc->pos[1];
          c.z = data.loc->pos[2];
          found[id] = c;
        }
      }
    }
  }

  std::vector<skymp::rules::MarkerCandidate> res;
  res.reserve(found.size());
  for (const auto& [id, c] : found) {
    res.push_back(c);
  }
  return res;
}
}

// thuum docs/verbs/map-markers.md: the client says only that its engine
// discovered a location of a type. The server finds which marker that was
// from the master files and the player's position, and records it on the
// player; a refusal logs the nearest candidate's distance, which is how the
// lab measures the engine's discovery range.
void ActionListener::OnMapMarkerDiscovered(
  const RawMessageData& rawMsgData, const MapMarkerDiscoveredMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return spdlog::warn("OnMapMarkerDiscovered - no actor for user {}",
                        rawMsgData.userId);
  }
  WorldState& worldState = partOne.worldState;
  if (!worldState.HasEspm()) {
    return spdlog::warn("OnMapMarkerDiscovered - no master files");
  }

  const auto candidates = MapMarkersNear(worldState, *actor, msg.markerType);
  const NiPoint3& pos = actor->GetPos();
  const auto choice = skymp::rules::map_marker_discovered(
    pos.x, pos.y, pos.z,
    rust::Slice<const skymp::rules::MarkerCandidate>(candidates.data(),
                                                     candidates.size()));
  if (!choice.found) {
    return spdlog::info(
      "E_MARKER_NONE: user {} actor {:x} discovered a location of type {}, "
      "no marker of it in range (nearest {:x} at {} units)",
      rawMsgData.userId, actor->GetFormId(), msg.markerType, choice.refr_id,
      choice.distance);
  }

  const bool changed = actor->RecordMapMarker(
    FormDesc::FromFormId(choice.refr_id, worldState.espmFiles), msg.canTravel);
  spdlog::info("MapMarker: user {} actor {:x} discovered {:x} (type {}, {} "
               "units{}){}",
               rawMsgData.userId, actor->GetFormId(), choice.refr_id,
               msg.markerType, choice.distance,
               msg.canTravel ? ", fast travel" : "",
               changed ? "" : ", already recorded");
}

// thuum docs/verbs/map-markers.md: show the player's recorded markers on its
// client's map, Papyrus ObjectReference.AddToMap with each marker as self
void ActionListener::SendMapMarkers(MpActor& actor)
{
  const auto markers = actor.GetMapMarkers();
  for (const auto& marker : markers) {
    std::vector<std::optional<
      std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
      args{ marker.canTravel };
    SpSnippet("ObjectReference", "AddToMap", args,
              marker.refr.ToFormId(partOne.worldState.espmFiles))
      .Execute(&actor, SpSnippetMode::kNoReturnResult);
  }
  if (!markers.empty()) {
    spdlog::info("MapMarker: actor {:x} shown its {} markers after a login",
                 actor.GetFormId(), markers.size());
  }
}

// thuum docs/verbs/learned-effects.md: after the player ate an ingredient,
// its client reports the effects its engine now knows of it. The server keeps
// the report only when it saw that player eat that ingredient just before,
// and the record only grows.
void ActionListener::OnIngredientEffectsKnown(
  const RawMessageData& rawMsgData, const IngredientEffectsKnownMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return spdlog::warn("OnIngredientEffectsKnown - no actor for user {}",
                        rawMsgData.userId);
  }
  WorldState& worldState = partOne.worldState;
  if (!worldState.HasEspm()) {
    return spdlog::warn("OnIngredientEffectsKnown - no master files");
  }
  const auto lookup =
    worldState.GetEspm().GetBrowser().LookupById(msg.ingredient);
  if (!lookup.rec || lookup.rec->GetType() != "INGR") {
    return spdlog::info("E_EFFECTS_FORM: user {} actor {:x} reported "
                        "effects of {:x}, which is no ingredient",
                        rawMsgData.userId, actor->GetFormId(), msg.ingredient);
  }

  const auto [eatenId, eatenAt] = actor->GetLastEaten();
  const bool hasEat = eatenId != 0;
  const uint64_t sinceEatMs = hasEat
    ? static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - eatenAt)
          .count())
    : 0;
  if (!skymp::rules::ingredient_effects_kept(eatenId == msg.ingredient, hasEat,
                                             sinceEatMs)) {
    return spdlog::info("E_EFFECTS_NO_EAT: user {} actor {:x} reported "
                        "effects {:#x} of {:x}, last ate {:x} {} ms ago",
                        rawMsgData.userId, actor->GetFormId(), msg.mask,
                        msg.ingredient, eatenId, sinceEatMs);
  }

  const bool changed = actor->RecordIngredientEffects(
    FormDesc::FromFormId(msg.ingredient, worldState.espmFiles), msg.mask);
  spdlog::info("IngredientEffects: user {} actor {:x} knows {:#x} of {:x}{}",
               rawMsgData.userId, actor->GetFormId(), msg.mask, msg.ingredient,
               changed ? "" : ", already recorded");
}

// thuum docs/verbs/learned-effects.md: teach the player's recorded effects to
// its client's engine, Papyrus Ingredient.LearnEffect with the ingredient as
// self, one per effect
void ActionListener::SendIngredientEffects(MpActor& actor)
{
  size_t sent = 0;
  for (const auto& entry : actor.GetIngredientEffects()) {
    const uint32_t ingredientId =
      entry.ingredient.ToFormId(partOne.worldState.espmFiles);
    for (int i = 0; i < 4; ++i) {
      if (!(entry.mask & (1 << i))) {
        continue;
      }
      std::vector<std::optional<
        std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
        args{ static_cast<double>(i) };
      SpSnippet("Ingredient", "LearnEffect", args, ingredientId)
        .Execute(&actor, SpSnippetMode::kNoReturnResult);
      ++sent;
    }
  }
  if (sent > 0) {
    spdlog::info("IngredientEffects: actor {:x} taught its {} effects after "
                 "a login",
                 actor.GetFormId(), sent);
  }
}

namespace {
// thuum docs/verbs/favorites.md: what the server knows of a favorite's form.
// Magic is a SPEL or SHOU in the master files; an item counts while the
// player holds it; a form made in a session (the FF range) or without a
// record is nothing to keep.
skymp::rules::FavoriteKind FavoriteKindOf(WorldState& worldState,
                                          MpActor& actor, uint32_t formId)
{
  using Kind = skymp::rules::FavoriteKind;
  if (formId >= 0xff000000) {
    return Kind::Other;
  }
  const auto lookup = worldState.GetEspm().GetBrowser().LookupById(formId);
  if (!lookup.rec) {
    return Kind::Other;
  }
  const auto type = lookup.rec->GetType();
  if (type == "SPEL" || type == "SHOU") {
    return Kind::Magic;
  }
  return actor.GetInventory().GetItemCount(formId) > 0 ? Kind::HeldItem
                                                       : Kind::MissingItem;
}

// The favorites the rule keeps of `entries`, as (form id, hotkey)
std::vector<skymp::rules::FavoriteEntry> KeptFavorites(
  WorldState& worldState, MpActor& actor,
  const std::vector<std::pair<uint32_t, int8_t>>& entries)
{
  std::vector<skymp::rules::FavoriteFacts> facts;
  facts.reserve(entries.size());
  for (const auto& [formId, hotkey] : entries) {
    facts.push_back(skymp::rules::FavoriteFacts{
      formId, hotkey, FavoriteKindOf(worldState, actor, formId) });
  }
  const auto kept = skymp::rules::favorites_kept(
    rust::Slice<const skymp::rules::FavoriteFacts>(facts.data(),
                                                   facts.size()));
  return std::vector<skymp::rules::FavoriteEntry>(kept.begin(), kept.end());
}
}

// thuum docs/verbs/favorites.md: the player's favorites after a menu where
// they change closed. Items count while the player holds them; magic is
// recorded as reported (the server's spell list lacks the starting spells,
// race powers and shouts its engine knows, and the client marks magic only
// when its engine knows it); anything else is dropped. A kept report
// replaces the record.
void ActionListener::OnFavorites(const RawMessageData& rawMsgData,
                                 const FavoritesMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return spdlog::warn("OnFavorites - no actor for user {}",
                        rawMsgData.userId);
  }
  WorldState& worldState = partOne.worldState;
  if (!worldState.HasEspm()) {
    return spdlog::warn("OnFavorites - no master files");
  }

  std::vector<std::pair<uint32_t, int8_t>> reported;
  reported.reserve(msg.entries.size());
  for (const auto& entry : msg.entries) {
    reported.emplace_back(entry.form, entry.hotkey);
  }
  std::vector<Favorite> favorites;
  for (const auto& entry : KeptFavorites(worldState, *actor, reported)) {
    favorites.push_back(Favorite{
      FormDesc::FromFormId(entry.form, worldState.espmFiles), entry.hotkey });
  }
  const size_t keptCount = favorites.size();
  const bool changed = actor->SetFavorites(std::move(favorites));
  spdlog::info("Favorites: user {} actor {:x} keeps {} of {} reported{}",
               rawMsgData.userId, actor->GetFormId(), keptCount,
               msg.entries.size(), changed ? "" : ", unchanged");
}

// thuum docs/verbs/favorites.md: the record, through the same rule (an item
// sold since is left out), for the client's engine to mark
void ActionListener::SendFavorites(MpActor& actor)
{
  WorldState& worldState = partOne.worldState;
  const auto recorded = actor.GetFavorites();
  if (recorded.empty() || !worldState.HasEspm()) {
    return;
  }
  std::vector<std::pair<uint32_t, int8_t>> entries;
  entries.reserve(recorded.size());
  for (const auto& favorite : recorded) {
    entries.emplace_back(favorite.form.ToFormId(worldState.espmFiles),
                         favorite.hotkey);
  }
  FavoritesMessage message;
  for (const auto& entry : KeptFavorites(worldState, actor, entries)) {
    message.entries.push_back(
      FavoritesMessage::Entry{ entry.form, entry.hotkey });
  }
  actor.SendToUser(message, true);
  spdlog::info("Favorites: actor {:x} sent {} of {} recorded after a login",
               actor.GetFormId(), message.entries.size(), recorded.size());
}

// thuum docs/verbs/racemenu-sync.md: the player's RaceMenu look after the
// race menu closed. It is taken only from a race menu the server opened, as
// SkyMP takes an appearance (OnUpdateAppearance), so a look and its hair
// color change together or not at all. The server cannot check a look
// against the engine, so it records it bounded (a JSON object, within the
// wire's capacity) and hands it to every client that shows the player; a new
// client gets it with the player's figure (PartOne's onSubscribe).
void ActionListener::OnRaceMenuPreset(const RawMessageData& rawMsgData,
                                      const RaceMenuPresetMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return spdlog::warn("OnRaceMenuPreset - no actor for user {}",
                        rawMsgData.userId);
  }
  if (!skymp::rules::racemenu_preset_ok(rust::Str(msg.preset))) {
    return spdlog::info("RaceMenu: user {} actor {:x} preset of {} bytes "
                        "refused (not a bounded JSON object)",
                        rawMsgData.userId, actor->GetFormId(),
                        msg.preset.size());
  }
  // thuum ADR-026: a look whose head parts the server lacks or the race may
  // not wear is no look either, and does not use up the opening
  Appearance derived;
  LookVerdict verdict = LookVerdict::kNotJudged;
  if (auto current = actor->GetAppearance()) {
    verdict = AppearanceFromLook(partOne, *current, msg.preset, derived);
  }
  if (verdict == LookVerdict::kRefused) {
    return spdlog::info("RaceMenu: user {} actor {:x} preset of {} bytes "
                        "refused (E_RACEMENU_PARTS: a head part the server "
                        "lacks or the race may not wear, ADR-026)",
                        rawMsgData.userId, actor->GetFormId(),
                        msg.preset.size());
  }
  // a look that is no look does not use up the opening
  if (!actor->TakeRaceMenuLookDue()) {
    return spdlog::info("RaceMenu: user {} actor {:x} preset of {} bytes "
                        "refused (E_RACEMENU_CLOSED: the server did not open "
                        "the race menu)",
                        rawMsgData.userId, actor->GetFormId(),
                        msg.preset.size());
  }
  if (!actor->SetRaceMenuPreset(msg.preset)) {
    return spdlog::info("RaceMenu: user {} actor {:x} preset unchanged",
                        rawMsgData.userId, actor->GetFormId());
  }
  RaceMenuPresetMessage out;
  out.actor = actor->GetFormId();
  out.preset = msg.preset;
  size_t sent = 0;
  for (auto listener : actor->GetActorListeners()) {
    if (listener == actor ||
        partOne.serverState.UserByActor(listener) ==
          Networking::InvalidUserId) {
      continue;
    }
    listener->SendToUser(out, true);
    ++sent;
  }
  spdlog::info("RaceMenu: user {} actor {:x} recorded a preset of {} bytes, "
               "sent to {} other players",
               rawMsgData.userId, actor->GetFormId(), msg.preset.size(), sent);
  if (verdict == LookVerdict::kDerived) {
    auto current = actor->GetAppearance();
    if (current && !SameAppearanceParts(derived, *current)) {
      actor->SetAppearance(&derived);
      SendAppearanceToOthers(partOne, *actor, derived);
      spdlog::info("RaceMenu: actor {:x} appearance follows its look "
                   "(ADR-026): {} head parts",
                   actor->GetFormId(), derived.headpartIds.size());
    }
  }
}

void ActionListener::SendRaceMenuPreset(MpActor& actor)
{
  auto preset = actor.GetRaceMenuPreset();
  if (preset.empty()) {
    return;
  }
  RaceMenuPresetMessage message;
  message.actor = actor.GetFormId();
  message.preset = std::move(preset);
  actor.SendToUser(message, true);
  spdlog::info("RaceMenu: actor {:x} sent its preset of {} bytes after a "
               "login",
               actor.GetFormId(), message.preset.size());
}

namespace {
// thuum docs/verbs/actor-values.md: the record and the message as the rule
// takes them
skymp::rules::AvSnapshot ToAvSnapshot(const ActorValueRecord& record)
{
  skymp::rules::AvSnapshot out{};
  for (const auto& [av, base] : record.bases) {
    out.bases.push_back(skymp::rules::AvBase{ av, base });
  }
  for (const auto& skill : record.skills) {
    out.skills.push_back(skymp::rules::AvSkill{ skill.skill, skill.level,
                                                skill.xp, skill.threshold });
  }
  out.xp = record.xp;
  out.threshold = record.threshold;
  out.level = record.level;
  for (const auto& [skill, count] : record.legendary) {
    out.legendary.push_back(skymp::rules::AvLegendary{ skill, count });
  }
  return out;
}

ActorValueRecord ToRecord(const ActorValuesMessage& msg)
{
  ActorValueRecord out;
  for (const auto& base : msg.bases) {
    out.bases.emplace_back(base.av, base.base);
  }
  for (const auto& skill : msg.skills) {
    out.skills.push_back(ActorValueRecord::Skill{ skill.skill, skill.level,
                                                  skill.xp, skill.threshold });
  }
  out.xp = msg.xp;
  out.threshold = msg.threshold;
  out.level = msg.level;
  for (const auto& legendary : msg.legendary) {
    out.legendary.emplace_back(legendary.skill, legendary.count);
  }
  return out;
}

ActorValueRecord ToRecord(const skymp::rules::AvSnapshot& snapshot)
{
  ActorValueRecord out;
  for (const auto& base : snapshot.bases) {
    out.bases.emplace_back(base.av, base.base);
  }
  for (const auto& skill : snapshot.skills) {
    out.skills.push_back(ActorValueRecord::Skill{ skill.skill, skill.level,
                                                  skill.xp, skill.threshold });
  }
  out.xp = snapshot.xp;
  out.threshold = snapshot.threshold;
  out.level = snapshot.level;
  for (const auto& legendary : snapshot.legendary) {
    out.legendary.emplace_back(legendary.skill, legendary.count);
  }
  return out;
}

std::vector<skymp::rules::AvBase> ToAvBases(
  const std::vector<std::pair<uint8_t, float>>& values)
{
  std::vector<skymp::rules::AvBase> out;
  out.reserve(values.size());
  for (const auto& [av, base] : values) {
    out.push_back(skymp::rules::AvBase{ av, base });
  }
  return out;
}
}

// thuum docs/verbs/actor-values.md: the player's actor values and progress
// after a skill or level increase. Recorded within bounds (R2; wire-rules
// actor_values), except a value the server set that the report does not
// carry yet; after a login, not until a report shows the login's record
// applied.
void ActionListener::OnActorValues(const RawMessageData& rawMsgData,
                                   const ActorValuesMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return spdlog::warn("OnActorValues - no actor for user {}",
                        rawMsgData.userId);
  }
  const auto report = ToAvSnapshot(ToRecord(msg));
  const auto held = ToAvBases(actor->GetHeldActorValues());
  const rust::Slice<const skymp::rules::AvBase> heldSlice(held.data(),
                                                          held.size());
  if (!skymp::rules::actor_values_report_ok(report, heldSlice)) {
    return spdlog::info("ActorValues: user {} actor {:x} report refused "
                        "(out of bounds)",
                        rawMsgData.userId, actor->GetFormId());
  }
  const auto record = actor->GetActorValueRecord();
  if (actor->IsActorValuesLoginPending()) {
    if (record &&
        !skymp::rules::actor_values_login_applied(ToAvSnapshot(*record),
                                                  report)) {
      // the values it fell under, named (thuum docs/verbs/actor-values.md,
      // train-31: a relaunch's reports were refused for a whole session
      // with no line saying on what)
      std::string under;
      for (const auto& base : skymp::rules::actor_values_login_shortfall(
             ToAvSnapshot(*record), report)) {
        under += fmt::format(" {}:{:.4g}", base.av, base.base);
      }
      return spdlog::info("ActorValues: user {} actor {:x} report before "
                          "the login's record was applied, not taken (level "
                          "{} against the record's {}; under the record:{})",
                          rawMsgData.userId, actor->GetFormId(), msg.level,
                          record->level, under.empty() ? " nothing" : under);
    }
    actor->SetActorValuesLoginPending(false);
  }
  const auto merged = skymp::rules::actor_values_merge(
    ToAvSnapshot(record.value_or(ActorValueRecord{})), report, heldSlice);
  std::vector<std::pair<uint8_t, float>> stillHeld;
  for (const auto& h : merged.held) {
    stillHeld.emplace_back(h.av, h.base);
  }
  const bool holding = !stillHeld.empty();
  actor->SetHeldActorValues(std::move(stillHeld));
  const bool changed = actor->SetActorValueRecord(ToRecord(merged.record));
  spdlog::info("ActorValues: user {} actor {:x} recorded {} bases, level "
               "{}{}",
               rawMsgData.userId, actor->GetFormId(), msg.bases.size(),
               msg.level, changed ? "" : ", unchanged");
  if (changed) {
    // thuum docs/verbs/figure-values.md: the other games' figures of this
    // player take the new maxima
    actor->SendMaximaToListeners();
  }
  if (holding) {
    // values the server set that this report does not carry: the record
    // goes back so the player's game applies them
    actor->SendActorValueRecord();
  }
}

// thuum docs/verbs/player-globals.md (ADR-029's A2): a player's game reports
// the watched globals whose values changed; the server keeps them per player
// (R2, recorded unvalidated) and sets them in its game after a login. A
// global named by a form id no loaded plugin holds reads as unlisted.
void ActionListener::OnPlayerGlobals(const RawMessageData& rawMsgData,
                                     const PlayerGlobalsMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return spdlog::warn("OnPlayerGlobals - no actor for user {}",
                        rawMsgData.userId);
  }
  std::vector<PlayerGlobal> report;
  report.reserve(msg.values.size());
  for (const auto& entry : msg.values) {
    try {
      report.push_back(
        { FormDesc::FromFormId(entry.global, partOne.worldState.espmFiles),
          entry.value });
    } catch (const std::exception&) {
      return spdlog::info("PlayerGlobals: user {} actor {:x} report refused "
                          "(global {:x} is in no loaded plugin)",
                          rawMsgData.userId, actor->GetFormId(), entry.global);
    }
  }
  if (const uint8_t refusal = partOne.CheckPlayerGlobals(report)) {
    return spdlog::info("PlayerGlobals: user {} actor {:x} report refused "
                        "({})",
                        rawMsgData.userId, actor->GetFormId(), refusal);
  }
  const bool changed = actor->SetPlayerGlobals(
    PartOne::MergePlayerGlobals(actor->GetPlayerGlobals(), report));
  spdlog::info("PlayerGlobals: user {} actor {:x} recorded {} values{}",
               rawMsgData.userId, actor->GetFormId(), report.size(),
               changed ? "" : ", unchanged");
}

void ActionListener::OnCellLoaded(const RawMessageData& rawMsgData,
                                  const CellLoadedMessage& msg)
{
  // thuum docs/verbs/cell-host.md: a game's loaded set, the fact the
  // election runs on (wire-rules cells, through PartOne)
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    spdlog::warn("ActionListener::OnCellLoaded - user {} has no actor; cell "
                 "{:x} ignored",
                 rawMsgData.userId, msg.cell);
    return;
  }
  spdlog::info("ActionListener::OnCellLoaded - {:x} {} cell {:x}",
               actor->GetFormId(), msg.loaded ? "loaded" : "unloaded",
               msg.cell);
  partOne.OnCellLoaded(*actor, msg.cell, msg.loaded);
}

void ActionListener::OnActorSightings(const RawMessageData& rawMsgData,
                                      const ActorSightingsMessage& msg)
{
  MpActor* reporter = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!reporter) {
    spdlog::warn("ActionListener::OnActorSightings - user {} has no actor; "
                 "{} sighting(s) ignored",
                 rawMsgData.userId, msg.sightings.size());
    return;
  }
  // the roster's arrival itself, once in ten seconds per reporter: every
  // entry may be dropped silently (already standing where its host has it)
  static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point>
    lastRosterLog;
  const auto now = std::chrono::steady_clock::now();
  auto& last = lastRosterLog[reporter->GetFormId()];
  if (last == std::chrono::steady_clock::time_point{} ||
      now - last >= std::chrono::seconds(10)) {
    last = now;
    // every id, so a resident the roster never names can be told from one
    // the server drops silently (playtest 33, 2026-10-23: the host's rosters
    // named the animals and not the residents, and only the first id showed)
    std::string ids;
    for (const auto& s : msg.sightings) {
      ids += fmt::format("{}{:x}", ids.empty() ? "" : " ", s.refrId);
    }
    spdlog::info(
      "ActionListener::OnActorSightings - roster from {:x}: {} "
      "sighting(s), first {:x} in {:x}; ids [{}]",
      reporter->GetFormId(), msg.sightings.size(),
      msg.sightings.empty() ? 0u : msg.sightings.front().refrId,
      msg.sightings.empty() ? 0u : msg.sightings.front().worldOrCell, ids);
  }
  for (const auto& s : msg.sightings) {
    partOne.OnActorSighting(*reporter, s.refrId, s.worldOrCell,
                            NiPoint3{ s.pos[0], s.pos[1], s.pos[2] },
                            NiPoint3{ s.rot[0], s.rot[1], s.rot[2] });
  }
}

namespace {
// thuum docs/verbs/npc-fate.md (ADR-048's B): the bounds a death report is
// checked within. The windows are the host's reporting lags with room: a
// hit the server numbered within kDeathHitWindow of the report, a drop of
// kDeathFallUnits in the host's samples within MpActor::kHeightWindow, an
// activation within kDeathHazardUnits of the body within
// kDeathHazardWindow. A body farther than kDeathBodyUnits from the record
// is refused (a ragdoll's flight fits; another cell does not). The fall
// bound is a plausibility floor, not the engine's damage rule (that ran on
// the host): HYPOTHESIS until the lab's fall run reads the drop the engine
// kills at.
constexpr auto kDeathHitWindow = std::chrono::milliseconds(3000);
constexpr auto kDeathHazardWindow = std::chrono::milliseconds(5000);
constexpr float kDeathFallUnits = 200.f;
constexpr float kDeathHazardUnits = 512.f;
constexpr float kDeathBodyUnits = 2048.f;

const char* DeathCauseName(uint32_t cause)
{
  switch (cause) {
    case 0:
      return "unknown";
    case 1:
      return "a fall";
    case 2:
      return "a hazard";
    case 3:
      return "a script";
    case 4:
      return "an actor's hit";
    default:
      return "a code the contract does not name";
  }
}
}

void ActionListener::NoteActivation(const MpObjectReference& target,
                                    std::chrono::steady_clock::time_point now)
{
  constexpr size_t kActivationsKept = 64;
  recentActivations.push_back(
    RecentActivation{ now, target.GetCellOrWorld(), target.GetPos() });
  while (recentActivations.size() > kActivationsKept ||
         (!recentActivations.empty() &&
          now - recentActivations.front().at > kDeathHazardWindow)) {
    recentActivations.pop_front();
  }
}

bool ActionListener::ActivationSeenNear(
  const MpObjectReference& where, const NiPoint3& pos,
  std::chrono::steady_clock::time_point now) const
{
  for (const auto& activation : recentActivations) {
    if (now - activation.at > kDeathHazardWindow) {
      continue;
    }
    if (activation.cellOrWorld != where.GetCellOrWorld()) {
      continue;
    }
    if ((activation.pos - pos).Length() <= kDeathHazardUnits) {
      return true;
    }
  }
  return false;
}

void ActionListener::OnActorDeath(const RawMessageData& rawMsgData,
                                  const ActorDeathMessage& msg)
{
  MpActor* reporter = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!reporter) {
    spdlog::warn("ActionListener::OnActorDeath - user {} has no actor; its "
                 "report for {:x} ignored",
                 rawMsgData.userId, msg.refrId);
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const NiPoint3 pos{ msg.pos[0], msg.pos[1], msg.pos[2] };
  // one warn line a minute per actor, the rest counted (OnUpdateMovement's
  // refusals' shape): a host reports a death once, but a wrong premise
  // would repeat it at every death the engine deals
  const auto refuse = [&](const char* code, const std::string& why) {
    static std::unordered_map<
      uint32_t, std::pair<std::chrono::steady_clock::time_point, uint32_t>>
      refusals;
    auto& [since, count] = refusals[msg.refrId];
    ++count;
    if (since.time_since_epoch().count() != 0 &&
        now - since < std::chrono::minutes(1)) {
      return;
    }
    spdlog::warn("[verb:npc-fate] death report {}: {:x} says {:x} died of {} "
                 "(cause {}, killer {:x}) at ({:.0f}, {:.0f}, {:.0f}): {}; "
                 "refused, {} since the last line",
                 code, reporter->GetFormId(), msg.refrId,
                 DeathCauseName(msg.cause), msg.cause, msg.killer, pos.x,
                 pos.y, pos.z, why, count);
    since = now;
    count = 0;
  };
  if (!partOne.worldState.npcDeathReports) {
    refuse("E_DEATH_OFF", "npcDeathReports is false (ADR-048's A)");
    return;
  }
  // no load: an actor the server never held is nobody's to report dead
  auto& form = partOne.worldState.LookupFormByIdNoLoad(msg.refrId);
  MpActor* actor = form ? form->AsActor() : nullptr;
  if (!actor) {
    refuse("E_DEATH_NO_ACTOR", "the server holds no such actor");
    return;
  }
  if (!actor->IsFateRespawned()) {
    refuse("E_DEATH_PLAYER",
           "a player's actor: its deaths are the server's own numbers");
    return;
  }
  const uint32_t simulator = partOne.SimulatorOf(*actor);
  if (simulator != reporter->GetFormId()) {
    refuse("E_DEATH_NOT_HOST",
           fmt::format("{:x} simulates it, not the reporter", simulator));
    return;
  }
  if (actor->IsDead()) {
    // the server's own kill reached the host first, or a second report:
    // nothing to do and no warning, the race is the expected one
    spdlog::info("[verb:npc-fate] death report: {:x} says {:x} died of {}; "
                 "already dead here, nothing to do",
                 reporter->GetFormId(), msg.refrId, DeathCauseName(msg.cause));
    return;
  }
  MpActor* killer = nullptr;
  if (msg.killer != 0) {
    auto& killerForm = partOne.worldState.LookupFormByIdNoLoad(msg.killer);
    killer = killerForm ? killerForm->AsActor() : nullptr;
  }
  if (!actor->MayDie(killer)) {
    refuse("E_DEATH_IMMORTAL",
           "an essential or protected base (the engine's rule, "
           "MpActor::MayDie)");
    return;
  }
  const float away = (pos - actor->GetPos()).Length();
  if (!(away <= kDeathBodyUnits)) {
    refuse("E_DEATH_FAR",
           fmt::format("the body {:.0f} units from the record (bound {:.0f})",
                       away, kDeathBodyUnits));
    return;
  }

  // the evidence: the cause named is checked against what the server saw
  const auto hitAgo = actor->HitByAgo(killer ? killer->GetFormId() : 0u, now);
  const bool hitSeen = hitAgo && *hitAgo <= kDeathHitWindow;
  const auto drop = actor->DropTo(std::min(pos.z, actor->GetPos().z), now);
  const bool fallSeen = drop && *drop >= kDeathFallUnits;
  const bool hazardSeen = ActivationSeenNear(*actor, pos, now);
  std::string evidence;
  switch (msg.cause) {
    case 4:
      if (!killer) {
        refuse("E_DEATH_NO_KILLER",
               "the killer named is no actor the server holds");
        return;
      }
      if (!hitSeen) {
        refuse("E_DEATH_UNCHECKED",
               fmt::format("the server saw no hit by {:x} within {} ms",
                           killer->GetFormId(), kDeathHitWindow.count()));
        return;
      }
      evidence = fmt::format("{:x} hit it {} ms before (the engine's number "
                             "outran the server's)",
                             killer->GetFormId(), hitAgo->count());
      break;
    case 1:
      if (!fallSeen) {
        refuse("E_DEATH_UNCHECKED",
               fmt::format("the host's samples show {} of a drop, {:.0f} "
                           "units needed",
                           drop ? fmt::format("{:.0f} units", *drop)
                                : std::string("nothing"),
                           kDeathFallUnits));
        return;
      }
      evidence =
        fmt::format("a drop of {:.0f} units in the host's samples", *drop);
      break;
    case 2:
      if (!hazardSeen) {
        refuse("E_DEATH_UNCHECKED",
               fmt::format("no activation within {:.0f} units in {} ms",
                           kDeathHazardUnits, kDeathHazardWindow.count()));
        return;
      }
      evidence = "an activation beside the body";
      break;
    case 0:
      if (hitSeen) {
        evidence = fmt::format("a hit the server numbered {} ms before",
                               hitAgo->count());
        // thuum docs/verbs/npc-fate.md H-52b (train 54): after a lethal
        // single blow the engine's deathStart names no killer and the host
        // reports cause 0 (52's gate's rest, 153601 and 160510); the
        // server's own number knows who struck, so the latest hitter it
        // recorded within the window is credited (R0: the server computes
        // the death, the report is its evidence)
        if (!killer) {
          if (const auto by = actor->LastHitBy(now, kDeathHitWindow)) {
            auto& byForm = partOne.worldState.LookupFormByIdNoLoad(*by);
            killer = byForm ? byForm->AsActor() : nullptr;
            if (killer) {
              evidence += fmt::format(", the killer {:x} credited from the "
                                      "server's own number",
                                      killer->GetFormId());
            }
          }
        }
      } else if (fallSeen) {
        evidence =
          fmt::format("a drop of {:.0f} units in the host's samples", *drop);
      } else if (hazardSeen) {
        evidence = "an activation beside the body";
      } else {
        refuse("E_DEATH_UNCHECKED",
               "no hit, drop or activation the server saw");
        return;
      }
      break;
    case 3:
      refuse("E_DEATH_SCRIPT",
             "a script's kill is the server's to run (scripted-effects.md), "
             "not a host's to report");
      return;
    default:
      refuse("E_DEATH_CAUSE", "a cause the contract does not name");
      return;
  }
  spdlog::info("[verb:npc-fate] death report: {:x} died of {} at ({:.0f}, "
               "{:.0f}, {:.0f}), host {:x}, killer {:x}: {}; the server "
               "records the death",
               msg.refrId, DeathCauseName(msg.cause), pos.x, pos.y, pos.z,
               reporter->GetFormId(), killer ? killer->GetFormId() : 0u,
               evidence);
  actor->Kill(killer);
}

void ActionListener::SendPlayerGlobals(MpActor& actor)
{
  PlayerGlobalsMessage message;
  message.watch = partOne.PlayerGlobalsWatch();
  if (message.watch.empty()) {
    return;
  }
  for (const auto& entry :
       partOne.PlayerGlobalsAtLogin(actor.GetPlayerGlobals())) {
    try {
      message.values.push_back(
        { entry.global.ToFormId(partOne.worldState.espmFiles), entry.value });
    } catch (const std::exception&) {
      // a recorded global whose plugin this load order lacks
    }
  }
  actor.SendToUser(message, true);
  spdlog::info("PlayerGlobals: actor {:x} sent {} to watch, {} values after "
               "a login",
               actor.GetFormId(), message.watch.size(), message.values.size());
}

void ActionListener::SendActorValues(MpActor& actor)
{
  const auto record = actor.GetActorValueRecord();
  if (!record) {
    return;
  }
  actor.SetActorValuesLoginPending(true);
  actor.SendActorValueRecord();
  spdlog::info("ActorValues: actor {:x} sent {} bases, level {} after a "
               "login",
               actor.GetFormId(), record->bases.size(), record->level);
}

// thuum docs/verbs/sleep.md: SkyMP's client blocks the game's own Papyrus
// events, so the script that grants a sleep's bonus never runs. The server
// grants Rested, and takes it back after its eight game hours unless a later
// sleep granted it again.
void ActionListener::GrantRested(MpActor& actor, uint64_t durationMs)
{
  const uint64_t grant = actor.NextRestedGrant();
  SendSpellSnippet(actor, "AddSpell", espm::SPEL::kRested);
  WorldState& worldState = partOne.worldState;
  const uint32_t actorId = actor.GetFormId();
  worldState.SetTimer(std::chrono::milliseconds(durationMs))
    .Then([&worldState, actorId, grant](Viet::Void) {
      const auto& form = worldState.LookupFormById(actorId);
      MpActor* target = form ? form->AsActor() : nullptr;
      if (!target || target->GetRestedGrant() != grant) {
        return;
      }
      SendSpellSnippet(*target, "RemoveSpell", espm::SPEL::kRested);
    });
}

void ActionListener::OnDropItem(const RawMessageData& rawMsgData,
                                const DropItemMessage& msg)
{
  uint32_t baseId = FormIdCasts::LongToNormal(msg.baseId);
  MpActor* ac = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!ac) {
    return spdlog::error("Unable to drop an item from user with id: {}.",
                         rawMsgData.userId);
  }

  auto worldState = ac->GetParent();
  if (!worldState) {
    return spdlog::error("No WorldState attached");
  }

  if (worldState->HasKeyword(baseId, "SweetCantDrop")) {
    return spdlog::error("Attempt to drop SweetCantDrop item {:x}",
                         ac->GetFormId());
  }

  Inventory::Entry entry;
  entry.baseId = baseId;
  entry.count = msg.count;

  ac->DropItem(baseId, entry);
}

void ActionListener::OnPlayerBowShot(const RawMessageData& rawMsgData,
                                     const PlayerBowShotMessage& msg)
{
  MpActor* ac = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!ac) {
    return spdlog::error("Unable to shot from user with id: {}.",
                         rawMsgData.userId);
  }

  auto worldState = ac->GetParent();
  if (!worldState) {
    return;
  }

  auto ammoLookupRes =
    worldState->GetEspm().GetBrowser().LookupById(msg.ammoId);
  if (!ammoLookupRes.rec) {
    return spdlog::error("ActionListener::OnPlayerBowShot {:x} - unable to "
                         "find espm record for {:x}",
                         ac->GetFormId(), msg.ammoId);
  }

  if (ammoLookupRes.rec->GetType().ToString() != "AMMO") {
    return spdlog::error(
      "ActionListener::OnPlayerBowShot {:x} - unable to shot not an ammo {:x}",
      ac->GetFormId(), msg.ammoId);
  }

  // thuum docs/verbs/marksman.md: a shot from the bow the player holds with
  // an arrow it has equipped, as the server records its equipment; anything
  // else changes nothing
  const auto worn = [&](uint32_t baseId) {
    for (auto& entry : ac->GetEquipment().inv.entries) {
      if (entry.baseId == baseId && entry.GetWorn() != Inventory::Worn::None) {
        return true;
      }
    }
    return false;
  };
  if (!worn(msg.weaponId) || !worn(msg.ammoId)) {
    return spdlog::warn("ActionListener::OnPlayerBowShot - E_SHOT_EQUIPMENT: "
                        "{:x} shot {:x} from {:x}, which its equipment does "
                        "not hold; refused",
                        ac->GetFormId(), msg.ammoId, msg.weaponId);
  }

  // thuum docs/verbs/marksman.md: the launch each neighbour's game replays
  // leaves from the shooter's bow, near where the server has the shooter;
  // one from farther off changes nothing, as a shot from a bow it does not
  // hold
  if (msg.hasLaunch) {
    const float distance =
      (NiPoint3(msg.originX, msg.originY, msg.originZ) - ac->GetPos())
        .Length();
    if (distance > kMaxLaunchOffsetUnits) {
      return spdlog::warn("ActionListener::OnPlayerBowShot - E_SHOT_ORIGIN: "
                          "{:x} launched {:.0f} units from its position; "
                          "refused",
                          ac->GetFormId(), distance);
    }
  }

  if (!ac->IsGodMode()) {
    // thuum docs/verbs/god-mode.md: TGM, unlimited arrows
    ac->RemoveItem(msg.ammoId, 1, nullptr);
  }

  // the shot waits for its hit (one), and each neighbour's game draws the
  // arrow from the shooter's figure
  partOne.RecordShot(ac->GetFormId(), msg.weaponId, msg.power, ac->GetPos());
  // thuum docs/verbs/self-cast.md: the engine's bow release ends its
  // shooter's concealment after the shot (ArrowReleaseHandler 42859 calls
  // DispelAlteredStates with no exception; HYPOTHESIS until a lab run)
  EndConcealment(partOne, *ac);
  ArrowShotMessage relay;
  relay.idx = ac->GetIdx();
  relay.weaponId = msg.weaponId;
  relay.ammoId = msg.ammoId;
  relay.power = msg.power;
  relay.aimAngle = msg.aimAngle;
  relay.aimHeading = msg.aimHeading;
  relay.hasLaunch = msg.hasLaunch;
  relay.originX = msg.originX;
  relay.originY = msg.originY;
  relay.originZ = msg.originZ;
  relay.launchAngleX = msg.launchAngleX;
  relay.launchAngleZ = msg.launchAngleZ;
  for (auto listener : ac->GetActorListeners()) {
    if (listener != ac) {
      listener->GetActorToSendTo().SendToUser(relay, true);
    }
  }
  spdlog::info("ActionListener::OnPlayerBowShot - {:x} shot {:x} from {:x}, "
               "power {:.2f}, aim {:.3f} heading {:.3f}",
               ac->GetFormId(), msg.ammoId, msg.weaponId, msg.power,
               msg.aimAngle, msg.aimHeading);
}

void ActionListener::OnFinishSpSnippet(const RawMessageData& rawMsgData,
                                       const FinishSpSnippetMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    throw std::runtime_error(
      "Unable to finish SpSnippet: No Actor found for user " +
      std::to_string(rawMsgData.userId));
  }

  actor->ResolveSnippet(
    static_cast<uint32_t>(msg.snippetIdx),
    SpSnippet::VarValueFromSpSnippetReturnValue(msg.returnValue));
}

void ActionListener::OnEquip(const RawMessageData& rawMsgData,
                             const OnEquipMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    throw std::runtime_error(
      "Unable to finish SpSnippet: No Actor found for user " +
      std::to_string(rawMsgData.userId));
  }

  std::ignore = actor->OnEquip(msg.baseId);
}

void ActionListener::OnConsoleCommand(const RawMessageData& rawMsgData,
                                      const ConsoleCommandMessage& msg)
{
  MpActor* me = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (me) {
    std::vector<ConsoleCommands::Argument> consoleArgs;
    consoleArgs.resize(msg.data.args.size());
    for (size_t i = 0; i < msg.data.args.size(); i++) {
      consoleArgs[i] = ConsoleCommands::Argument(msg.data.args[i]);
    }
    ConsoleCommands::Execute(partOne, *me, msg.data.commandName, consoleArgs);
  }
}

void ActionListener::OnCraftItem(const RawMessageData& rawMsgData,
                                 const CraftItemMessage& msg)
{
  craftService->OnCraftItem(rawMsgData, msg.data.craftInputObjects,
                            msg.data.workbench, msg.data.resultObjectId);
}

void ActionListener::OnHostAttempt(const RawMessageData& rawMsgData,
                                   const HostMessage& msg)
{
  // thuum docs/verbs/cell-host.md (ADR-035): the server elects a cell's host
  // from the games' CellLoaded reports; a game's own claim on one actor (the
  // M0 shape, Host 14) is read and ignored, nothing sent back
  spdlog::debug("ActionListener::OnHostAttempt - Host for {:x} from user {} "
                "ignored: cells are hosted by the server's election",
                msg.remoteId, rawMsgData.userId);
}

void ActionListener::OnCustomEvent(const RawMessageData& rawMsgData,
                                   const CustomEventMessage& msg)
{
  auto ac = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!ac) {
    return;
  }
  if (msg.eventName.empty() || msg.eventName[0] != '_') {
    return;
  }

  nlohmann::json jsonArray = nlohmann::json::array();

  for (auto& arg : msg.argsJsonDumps) {
    jsonArray.push_back(nlohmann::json::parse(arg));
  }

  const std::string jsonArrayDump = jsonArray.dump();

  for (auto& listener : partOne.GetListeners()) {
    CustomEvent customEvent(ac->GetFormId(), msg.eventName, jsonArrayDump);
    listener->OnMpApiEvent(customEvent);
  }
}

void ActionListener::OnChangeValues(const RawMessageData& rawMsgData,
                                    const ChangeValuesMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return spdlog::error(
      "ActionListener::OnChangeValues - no Actor attached to userId {}",
      rawMsgData.userId);
  }

  const auto now = std::chrono::steady_clock::now();
  const float timeAfterRegeneration = CropPeriodAfterLastRegen(
    actor->GetDurationOfAttributesPercentagesUpdate(now).count());

  const auto& currentValues = actor->GetActorValues();

  ChangeValuesMessage outMsg;
  outMsg.idx = actor->GetIdx();
  bool sendOutMsg = false;

  auto process = [&](espm::ActorValue av, std::optional<float> inputVal,
                     float currentVal, std::optional<float>& outVal) {
    if (!inputVal.has_value()) {
      return;
    }

    // a report below a value the server raised predates it: dropped until
    // one reaches the value (thuum docs/verbs/attributes.md). Asked before
    // the equality below: the report that carries the raise equals the
    // server's value and must still end the hold
    if (!actor->TakeReportedPercentage(av, *inputVal)) {
      spdlog::debug("OnChangeValues - {:x} reported {} {:.3f} below a value "
                    "the server raised; dropped",
                    actor->GetFormId(), static_cast<int>(av), *inputVal);
      return;
    }

    if (MathUtils::IsNearlyEqual(currentVal, *inputVal)) {
      return;
    }

    float newVal = *inputVal;

    if (av == espm::ActorValue::Health) {
      newVal = CropHealthRegeneration(newVal, timeAfterRegeneration, actor);
    } else if (av == espm::ActorValue::Magicka) {
      newVal = CropMagickaRegeneration(newVal, timeAfterRegeneration, actor);
    }

    if (!MathUtils::IsNearlyEqual(newVal, *inputVal)) {
      outVal = newVal;
      sendOutMsg = true;
    }

    actor->SetPercentage(av, newVal);
  };

  process(espm::ActorValue::Health, msg.data.health,
          currentValues.healthPercentage, outMsg.data.health);
  process(espm::ActorValue::Magicka, msg.data.magicka,
          currentValues.magickaPercentage, outMsg.data.magicka);
  process(espm::ActorValue::Stamina, msg.data.stamina,
          currentValues.staminaPercentage, outMsg.data.stamina);

  if (sendOutMsg) {
    actor->SendToUser(outMsg, true);
  }
}

namespace {

bool IsUnarmedAttack(const uint32_t sourceFormId)
{
  return sourceFormId == 0x1f4;
}

float CalculateCurrentHealthPercentage(const MpActor& actor, float damage,
                                       float healthPercentage,
                                       float* outBaseHealth)
{
  const uint32_t baseId = actor.GetBaseId();
  const uint32_t raceId = actor.GetRaceId();
  WorldState* espmProvider = actor.GetParent();

  // the actor's recorded maximum when it has one (thuum
  // docs/verbs/actor-values.md: a player's base health, raised by level-ups),
  // the race's and the base NPC's otherwise
  float baseHealth =
    GetBaseActorValues(espmProvider, baseId, raceId, actor.GetTemplateChain())
      .health;
  if (auto recorded = actor.GetRecordedActorValueBase(24);
      recorded && std::isfinite(*recorded) && *recorded > 0.f) {
    baseHealth = *recorded;
  }

  if (outBaseHealth) {
    *outBaseHealth = baseHealth;
  }

  if (!std::isfinite(baseHealth) || baseHealth <= 0.f ||
      !std::isfinite(damage)) {
    return healthPercentage;
  }
  const float damagePercentage = damage / baseHealth;
  const float currentHealthPercentage = healthPercentage - damagePercentage;

  return currentHealthPercentage <= 0.f ? 0.f : currentHealthPercentage;
}

float GetReach(const MpActor& actor, const uint32_t source,
               float reachHotfixMult)
{
  auto espmProvider = actor.GetParent();
  if (IsUnarmedAttack(source)) {
    uint32_t raceId = actor.GetRaceId();
    return reachHotfixMult *
      espm::GetData<espm::RACE>(raceId, espmProvider).unarmedReach;
  }
  auto weapDNAM = espm::GetData<espm::WEAP>(source, espmProvider).weapDNAM;
  float fCombatDistance =
    espm::GetData<espm::GMST>(espm::GMST::kFCombatDistance, espmProvider)
      .value;
  float weaponReach = weapDNAM ? weapDNAM->reach : 0;
  return reachHotfixMult * weaponReach * fCombatDistance;
}

NiPoint3 RotateZ(const NiPoint3& point, float angle)
{
  static const float kPi = std::acos(-1.f);
  static const float kAngleToRadians = kPi / 180.f;
  float cos = std::cos(angle * kAngleToRadians);
  float sin = std::sin(angle * kAngleToRadians);

  return { point.x * cos - point.y * sin, point.x * sin + point.y * cos,
           point.z };
}

float GetSqrDistanceToBounds(const MpActor& actor, const MpActor& target)
{
  // TODO(#491): Figure out where to take the missing reach component
  constexpr float kPatch = 15.f;

  auto bounds = actor.GetBounds();
  auto targetBounds = target.GetBounds();

  // "Y" is "face" of character
  const float angleZ = 90.f - target.GetAngle().z;
  float direction = actor.GetAngle().z;

  // vector from target to the actor
  NiPoint3 position = actor.GetPos() - target.GetPos();
  position += RotateZ(
    NiPoint3(kPatch + bounds.pos2[1], 0.f, 0.f + bounds.pos2[2]), direction);

  NiPoint3 pos = RotateZ(position, angleZ);

  bool isProjectionInside[3] = {
    (targetBounds.pos1[0] <= pos.x && pos.x <= targetBounds.pos2[0]),
    (targetBounds.pos1[1] <= pos.y && pos.y <= targetBounds.pos2[1]),
    (targetBounds.pos1[2] <= pos.z && pos.z <= targetBounds.pos2[2])
  };

  NiPoint3 nearestCorner = {
    pos[0] > 0 ? 0.f + targetBounds.pos2[0] : 0.f + targetBounds.pos1[0],
    pos[1] > 0 ? 0.f + targetBounds.pos2[1] : 0.f + targetBounds.pos1[1],
    pos[2] > 0 ? 0.f + targetBounds.pos2[2] : 0.f + targetBounds.pos1[2],
  };

  return NiPoint3(isProjectionInside[0] ? 0.f : pos.x - nearestCorner.x,
                  isProjectionInside[1] ? 0.f : pos.y - nearestCorner.y,
                  isProjectionInside[2] ? 0.f : pos.z - nearestCorner.z)
    .SqrLength();
}

bool IsBowOrCrossbowShot(const HitData& hitData, WorldState* worldState)
{
  if (!worldState || !worldState->HasEspm()) {
    return false;
  }

  if (hitData.isBashAttack) {
    return false;
  }

  auto sourceLookupRes =
    worldState->GetEspm().GetBrowser().LookupById(hitData.source);
  if (!sourceLookupRes.rec) {
    return false;
  }

  auto source = espm::Convert<espm::WEAP>(sourceLookupRes.rec);
  if (!source) {
    return false;
  }

  auto weapDNAM = source->GetData(worldState->GetEspmCache()).weapDNAM;

  if (weapDNAM->animType != espm::WEAP::AnimType::Bow &&
      weapDNAM->animType != espm::WEAP::AnimType::Crossbow) {
    return false;
  }

  return true;
}

bool IsDistanceValid(const MpActor& actor, const MpActor& targetActor,
                     const HitData& hitData)
{
  float sqrDistance = GetSqrDistanceToBounds(actor, targetActor);

  // TODO: fix bounding boxes for creatures such as chicken, mudcrab, etc
  float reachPveHotfixMult =
    (actor.GetBaseId() <= 0x7 && targetActor.GetBaseId() <= 0x7)
    ? 1.f
    : std::numeric_limits<float>::infinity();

  float reach = GetReach(actor, hitData.source, reachPveHotfixMult);

  // For bow/crossbow shots we don't want to check melee radius
  if (IsBowOrCrossbowShot(hitData, actor.GetParent())) {
    constexpr float kExteriorCellWidthUnits = 4096.f;
    reach = kExteriorCellWidthUnits * 2;
  }

  return reach * reach > sqrDistance;
}

bool CanHit(const MpActor& actor, const HitData& hitData,
            const std::chrono::duration<float>& timePassed)
{
  WorldState* espmProvider = actor.GetParent();
  auto weapDNAM =
    espm::GetData<espm::WEAP>(hitData.source, espmProvider).weapDNAM;

  if (weapDNAM) {
    float speedMult = weapDNAM->speed;
    return timePassed.count() >= (1.1 * (1 / speedMult)) -
      (1.1 * (1 / speedMult) * (speedMult <= 0.75 ? 0.45 : 0.3));
  }

  throw std::runtime_error(
    fmt::format("Cannot get weapon speed from source: {0:x}", hitData.source));
}

// thuum docs/verbs/blocking.md: the game's block settings as the engine
// reads them (thuum ghidra/notes/block-formula.md, ID 26425): Skyrim.esm's
// GMSTs, and fPlayerMaxResistance's executable default, 85, which the
// masters do not set (lab/esm.py, 2026-10-10)
skymp::rules::BlockSettings BlockSettingsOf(WorldState& worldState)
{
  const auto setting = [&](uint32_t id) {
    return espm::GetData<espm::GMST>(id, &worldState).value;
  };
  constexpr float kPlayerMaxResistance = 85.f;
  skymp::rules::BlockSettings s{};
  s.shield_base = setting(espm::GMST::kFShieldBaseFactor);
  s.shield_scaling = setting(espm::GMST::kFShieldScalingFactor);
  s.weapon_base = setting(espm::GMST::kFBlockWeaponBase);
  s.weapon_scaling = setting(espm::GMST::kFBlockWeaponScaling);
  s.rating_base = setting(espm::GMST::kFArmorRatingBase);
  s.rating_max = setting(espm::GMST::kFArmorRatingMax);
  s.power_attack_mult = setting(espm::GMST::kFBlockPowerAttackMult);
  s.max_share = kPlayerMaxResistance / 100.f;
  return s;
}

// The base armor rating of the shield the target holds, if it holds one: a
// worn ARMO with the shield's biped slot (BGSBipedObjectForm::
// BipedObjectSlot::kShield, 1 << 9, CommonLibSSE-NG
// include/RE/B/BGSBipedObjectForm.h:22; BOD2, else BODT). The record's
// equip slot is no shield test (thuum scratchpad m2-survey.md).
std::optional<float> HeldShieldRating(const MpActor& target,
                                      WorldState& worldState)
{
  constexpr uint32_t kShieldSlot = 1u << 9;
  auto& browser = worldState.GetEspm().GetBrowser();
  for (const auto& entry : target.GetEquipment().inv.entries) {
    if (entry.GetWorn() == Inventory::Worn::None) {
      continue;
    }
    const auto res = browser.LookupById(entry.baseId);
    if (!res.rec || res.rec->GetType() != espm::ARMO::kType) {
      continue;
    }
    const auto data = espm::GetData<espm::ARMO>(entry.baseId, &worldState);
    const uint32_t slots =
      data.bod2.present ? data.bod2.bodyPartFlags : data.bodt.bodyPartFlags;
    if (slots & kShieldSlot) {
      return static_cast<float>(data.baseRatingX100) / 100.f;
    }
  }
  return std::nullopt;
}

// thuum docs/verbs/blocking.md: the share of the hit's damage the target's
// block stops, from the server's own records (R0, wire-rules block): its
// recorded block, its heading against the attacker's position (the engine
// tests the block's cone in yaw only), an arrow or a bolt (a bow's bash is
// a melee hit), a shield, the attacker's weapon damage, the target's Block
// and ShieldPerks (the actor-values record; 0 without one), the power
// attack damage-flags backed
float BlockedShare(const MpActor& aggressor, const MpActor& target,
                   const HitData& hitData, WorldState& worldState)
{
  skymp::rules::BlockFacts f{};
  f.blocking = target.IsBlockActive();
  if (!f.blocking) {
    return 0.f;
  }
  f.heading = target.GetAngle().z;
  f.dx = aggressor.GetPos().x - target.GetPos().x;
  f.dy = aggressor.GetPos().y - target.GetPos().y;
  const auto source =
    worldState.GetEspm().GetBrowser().LookupById(hitData.source);
  if (source.rec && source.rec->GetType() == espm::WEAP::kType) {
    const auto weap = espm::GetData<espm::WEAP>(hitData.source, &worldState);
    if (weap.weapDNAM) {
      const auto anim = weap.weapDNAM->animType;
      f.ranged = (anim == espm::WEAP::AnimType::Bow ||
                  anim == espm::WEAP::AnimType::Crossbow) &&
        !hitData.isBashAttack;
    }
    if (weap.weapData) {
      f.weapon_damage = static_cast<float>(weap.weapData->damage);
    }
  }
  if (const auto rating = HeldShieldRating(target, worldState)) {
    f.shield = true;
    f.shield_rating = *rating;
  }
  f.block_skill =
    target
      .GetRecordedActorValueBase(static_cast<uint8_t>(espm::ActorValue::Block))
      .value_or(0.f);
  // ShieldPerks, actor value 66 (CommonLibSSE-NG
  // include/RE/A/ActorValues.h:74), the engine's term beside the skill
  constexpr uint8_t kShieldPerks = 66;
  f.shield_perks =
    target.GetRecordedActorValueBase(kShieldPerks).value_or(0.f);
  f.power = hitData.isPowerAttack;
  return skymp::rules::blocked_share(BlockSettingsOf(worldState), f);
}

// thuum docs/verbs/stagger.md: the game's stagger settings as the engine
// reads them (thuum ghidra/notes/block-formula.md): Skyrim.esm's GMSTs, and
// the executable's defaults where the masters set none (the notes' table:
// fStaggerMassMult 1, fStaggerMassBase 0, fStaggerMassOffsetMult 0.15,
// fStaggerAttackBase 0.5, fStaggerAttackMult 0.01, fStaggerShieldMult 1,
// fStagger1WeapMult 0.2, fStagger2WeapMult 0.5, fStaggerRunningMult 1.5,
// fStaggerRecoilingMult 1.5); bPlayerAlwaysStaggered:Combat is an INI
// setting, true by default, and the server's here
skymp::rules::StaggerSettings StaggerSettingsOf(WorldState& worldState)
{
  const auto setting = [&](uint32_t id) {
    return espm::GetData<espm::GMST>(id, &worldState);
  };
  skymp::rules::StaggerSettings s{};
  s.mass_mult = 1.f;
  s.mass_base = 0.f;
  s.mass_offset_base = setting(espm::GMST::kFStaggerMassOffsetBase).value;
  s.mass_offset_mult = 0.15f;
  s.player_mass_mult = setting(espm::GMST::kFStaggerPlayerMassMult).value;
  s.bash_base = 0.5f;
  s.bash_mult = 0.01f;
  s.shield_mult = 1.f;
  s.one_hand_mult = 0.2f;
  s.two_hand_mult = 0.5f;
  s.running_mult = 1.5f;
  s.recoiling_mult = 1.5f;
  s.blocking_mult = setting(espm::GMST::kFStaggerBlockingMult).value;
  s.block_base = setting(espm::GMST::kFStaggerBlockBase).value;
  s.block_mult = setting(espm::GMST::kFStaggerBlockMult).value;
  s.block_shield_base =
    setting(espm::GMST::kFStaggerBlockAttackShieldBase).value;
  s.block_shield_mult =
    setting(espm::GMST::kFStaggerBlockAttackShieldMult).value;
  s.min = setting(espm::GMST::kFStaggerMin).value;
  s.attack_chance = static_cast<uint8_t>(
    std::clamp(setting(espm::GMST::kIStaggerAttackChance).intValue, 0, 100));
  s.player_always_staggered = true;
  return s;
}

// The weapon worn in the actor's right hand, if any
std::optional<espm::WEAP::Data> RightHandWeapon(const MpActor& actor,
                                                WorldState& worldState)
{
  auto& browser = worldState.GetEspm().GetBrowser();
  for (const auto& entry : actor.GetEquipment().inv.entries) {
    if (entry.GetWorn() != Inventory::Worn::Right) {
      continue;
    }
    const auto res = browser.LookupById(entry.baseId);
    if (res.rec && res.rec->GetType() == espm::WEAP::kType) {
      return espm::GetData<espm::WEAP>(entry.baseId, &worldState);
    }
  }
  return std::nullopt;
}

// A drawn weapon's value against a stagger when its holder blocks without a
// shield (ID 18093, the executable's defaults: fStagger1WeapAR 10 for a
// one-handed sword, axe or mace, fStaggerDaggerAR 5, fStagger2WeapAR 15,
// fStaggerBowAR 5 for a bow or a crossbow, else 0)
float DrawnWeaponStaggerValue(const MpActor& actor, WorldState& worldState)
{
  if (!actor.GetAnimationVariableBool("_skymp_isWeapDrawn")) {
    return 0.f;
  }
  const auto weapon = RightHandWeapon(actor, worldState);
  if (!weapon || !weapon->weapDNAM) {
    return 0.f;
  }
  switch (weapon->weapDNAM->animType) {
    case espm::WEAP::AnimType::OneHandSword:
    case espm::WEAP::AnimType::OneHandAxe:
    case espm::WEAP::AnimType::OneHandMace:
      return 10.f;
    case espm::WEAP::AnimType::OneHandDagger:
      return 5.f;
    case espm::WEAP::AnimType::TwoHandSword:
    case espm::WEAP::AnimType::TwoHandAxe:
      return 15.f;
    case espm::WEAP::AnimType::Bow:
    case espm::WEAP::AnimType::Crossbow:
      return 5.f;
    default:
      return 0.f;
  }
}

// thuum docs/verbs/stagger.md: the facts the stagger rule needs, from the
// server's records: the attacker's weapon and the attack it started (its
// race's ATKD by name), the victim's race's mass, block and Block (the
// actor-values record; 0 without one, as blocking's), whether it is a
// player. The victim's running and recoiling are not recorded (false).
skymp::rules::StaggerFacts StaggerFactsOf(
  const MpActor& aggressor, const MpActor& target, const HitData& hitData,
  const std::string& attackName, bool targetIsPlayer, bool dealtDamage,
  WorldState& worldState)
{
  skymp::rules::StaggerFacts f{};
  f.bash = hitData.isBashAttack;
  f.weapon_stagger = 1.f;
  bool twoHanded = false;
  bool armed = false;
  auto& browser = worldState.GetEspm().GetBrowser();
  const auto source = browser.LookupById(hitData.source);
  if (source.rec && source.rec->GetType() == espm::WEAP::kType) {
    const auto weap = espm::GetData<espm::WEAP>(hitData.source, &worldState);
    f.weapon_stagger = weap.stagger;
    if (weap.weapDNAM) {
      const auto anim = weap.weapDNAM->animType;
      armed = anim != espm::WEAP::AnimType::Other;
      twoHanded = anim == espm::WEAP::AnimType::TwoHandSword ||
        anim == espm::WEAP::AnimType::TwoHandAxe ||
        anim == espm::WEAP::AnimType::Bow ||
        anim == espm::WEAP::AnimType::Crossbow;
      f.ranged = (anim == espm::WEAP::AnimType::Bow ||
                  anim == espm::WEAP::AnimType::Crossbow) &&
        !hitData.isBashAttack;
    }
  }
  const auto race =
    espm::GetData<espm::RACE>(aggressor.GetRaceId(), &worldState);
  const auto offset = race.attackStaggerOffsets.find(attackName);
  f.stagger_offset =
    offset == race.attackStaggerOffsets.end() ? 0.f : offset->second;
  // BowStaggerBonus, actor value 87 (CommonLibSSE-NG
  // include/RE/A/ActorValues.h:95)
  constexpr uint8_t kBowStaggerBonus = 87;
  f.bow_stagger_bonus =
    aggressor.GetRecordedActorValueBase(kBowStaggerBonus).value_or(0.f);
  f.attacker_block =
    aggressor
      .GetRecordedActorValueBase(static_cast<uint8_t>(espm::ActorValue::Block))
      .value_or(0.f);
  f.attacker_shield = HeldShieldRating(aggressor, worldState).has_value();
  f.attacker_two_handed = twoHanded;
  f.attacker_unarmed = !armed && !f.attacker_shield;

  // Mass, actor value 36 (include/RE/A/ActorValues.h:44): the record's,
  // else the race's base
  constexpr uint8_t kMass = 36;
  f.victim_mass = target.GetRecordedActorValueBase(kMass).value_or(
    espm::GetData<espm::RACE>(target.GetRaceId(), &worldState).baseMass);
  f.victim_is_player = targetIsPlayer;
  f.victim_running = false;
  f.victim_recoiling = false;
  f.victim_blocking = target.IsBlockActive();
  f.victim_block =
    target
      .GetRecordedActorValueBase(static_cast<uint8_t>(espm::ActorValue::Block))
      .value_or(0.f);
  if (const auto rating = HeldShieldRating(target, worldState)) {
    f.victim_block_rating = *rating;
  } else {
    f.victim_block_rating = DrawnWeaponStaggerValue(target, worldState);
  }
  f.blocked = hitData.isHitBlocked;
  f.dealt_damage = dealtDamage;
  return f;
}
}

namespace {
// thuum docs/verbs/melee-reach.md: the facts the melee reach rule needs; the
// rule is Rust's (skymp-wire wire-rules melee, ADR-020). A player's scale is
// its race's height for its sex (refScale and the base record's height are 1
// for player characters).
float PlayerScale(const MpActor& actor, WorldState& worldState)
{
  auto appearance = actor.GetAppearance();
  const bool isFemale = appearance && appearance->isFemale;
  return espm::GetData<espm::RACE>(actor.GetRaceId(), &worldState)
    .height[isFemale ? 1 : 0];
}

std::optional<skymp::rules::MeleeFacts> MeleeFacts(const MpActor& aggressor,
                                                   const MpActor& target,
                                                   WorldState& worldState)
{
  try {
    auto& browser = worldState.GetEspm().GetBrowser();
    auto& cache = worldState.GetEspmCache();
    skymp::rules::MeleeFacts facts{};
    for (const auto& entry : aggressor.GetEquipment().inv.entries) {
      if (entry.GetWorn() == Inventory::Worn::None) {
        continue;
      }
      auto weapon =
        espm::Convert<espm::WEAP>(browser.LookupById(entry.baseId).rec);
      auto dnam = weapon ? weapon->GetData(cache).weapDNAM : nullptr;
      if (dnam) {
        facts.weapon_reach = std::max(facts.weapon_reach, dnam->reach);
      }
    }
    facts.distance = (aggressor.GetPos() - target.GetPos()).Length();
    facts.combat_distance =
      espm::GetData<espm::GMST>(espm::GMST::kFCombatDistance, &worldState)
        .value;
    facts.bash_reach =
      espm::GetData<espm::GMST>(espm::GMST::kFCombatBashReach, &worldState)
        .value;
    facts.unarmed_reach =
      espm::GetData<espm::RACE>(aggressor.GetRaceId(), &worldState)
        .unarmedReach;
    facts.aggressor_scale = PlayerScale(aggressor, worldState);
    facts.target_scale = PlayerScale(target, worldState);
    return facts;
  } catch (std::exception& e) {
    spdlog::warn("MeleeFacts - {:x} on {:x}: {}; reach not checked",
                 aggressor.GetFormId(), target.GetFormId(), e.what());
    return std::nullopt;
  }
}

// thuum docs/verbs/hit-cone.md: the facts the hit cone rule needs; the rule
// is Rust's (skymp-wire wire-rules melee, ADR-020). Headings are degrees, as
// the core records them.
skymp::rules::ConeFacts ConeFacts(const MpActor& aggressor,
                                  const MpActor& target, bool power,
                                  WorldState& worldState)
{
  skymp::rules::ConeFacts facts{};
  facts.heading = aggressor.GetAngle().z;
  const NiPoint3 offset = target.GetPos() - aggressor.GetPos();
  facts.dx = offset.x;
  facts.dy = offset.y;
  facts.power = power;
  facts.target_dead = target.IsDead();
  try {
    facts.widest_strike_angle =
      espm::GetData<espm::RACE>(aggressor.GetRaceId(), &worldState)
        .widestStrikeAngle;
  } catch (std::exception& e) {
    spdlog::warn("ConeFacts - {:x}: {}; the race's attacks not read",
                 aggressor.GetFormId(), e.what());
  }
  return facts;
}
}

void ActionListener::OnHit(const RawMessageData& rawMsgData,
                           const HitMessage& msg)
{
  MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);

  if (!myActor) {
    return spdlog::error(
      "ActionListener::OnHit - no Actor attached to userId {}",
      rawMsgData.userId);
  }

  MpActor* aggressor = nullptr;

  HitData hitData = msg.data;
  if (hitData.aggressor == 0x14) {
    aggressor = myActor;
    hitData.aggressor = aggressor->GetFormId();
  } else {
    aggressor = &partOne.worldState.GetFormAt<MpActor>(hitData.aggressor);
    // thuum docs/verbs/cell-host.md: a hosted aggressor is its cell's
    // host's; an actor a user plays is nobody's to report for
    if (partOne.serverState.UserByActor(aggressor) !=
          Networking::InvalidUserId ||
        partOne.worldState.HostOf(*aggressor) != myActor) {
      spdlog::error("SendToNeighbours - No permission to send OnHit with "
                    "aggressor actor {:x}",
                    aggressor->GetFormId());
      return;
    }
  }

  if (hitData.target == 0x14) {
    hitData.target = myActor->GetFormId();
  }

  MpForm* targetForm = partOne.worldState.LookupFormById(hitData.target).get();
  MpObjectReference* targetRef =
    targetForm ? targetForm->AsObjectReference() : nullptr;
  if (!targetRef) {
    // a hit on nothing the server holds: the ground (target 0, every
    // teleport cast's own landing, playtest twenty-four) or a world
    // reference outside the server's forms (an arrow in a wall); nothing to
    // judge, nothing wrong
    spdlog::info("ActionListener::OnHit - MpObjectReference not found for "
                 "hitData.target {:x}: a hit on nothing the server holds",
                 hitData.target);
    return;
  }

  const FormDesc& aggressorCellOrWorld = aggressor->GetCellOrWorld();
  const FormDesc& targetCellOrWorld = targetRef->GetCellOrWorld();

  if (aggressorCellOrWorld != targetCellOrWorld) {
    const std::vector<std::string>& files = partOne.worldState.espmFiles;
    spdlog::error(
      "ActionListener::OnHit - aggressor and targetRef are in different cells "
      "or world. Aggressor: {:x}, targetRef: {:x}, cellOrWorld of aggressor: "
      "{:x}, cellOrWorld of targetRef: {:x}",
      aggressor->GetFormId(), targetRef->GetFormId(),
      aggressorCellOrWorld.ToFormId(files), targetCellOrWorld.ToFormId(files));
    return;
  }

  // TODO: repair IsDistanceValid instead
  if (!IsBowOrCrossbowShot(hitData, &partOne.worldState)) {
    const NiPoint3& aggressorPos = aggressor->GetPos();
    const NiPoint3& targetPos = targetRef->GetPos();
    constexpr float kExteriorCellWidthUnits = 4096.f;
    if ((aggressorPos - targetPos).SqrLength() >
        kExteriorCellWidthUnits * kExteriorCellWidthUnits) {
      spdlog::error("ActionListener::OnHit - aggressor and targetRef are too "
                    "distant. Aggressor: {:x}, targetRef: {:x}",
                    aggressor->GetFormId(), targetRef->GetFormId());
      return;
    }
  }

  if (aggressor->IsDead()) {
    // thuum ADR-050 decision 3 (train 51): the respawn that fixes a dead
    // player's state is a player's rule; a dead NPC's late swing is dropped
    // and its respawn stays the fate's clock
    spdlog::debug(fmt::format(
      "{:x} actor is dead and can't attack{}", aggressor->GetFormId(),
      aggressor->IsCreatedAsPlayer() ? ". requesting respawn in order to fix "
                                       "death state"
                                     : ""));
    if (aggressor->IsCreatedAsPlayer()) {
      aggressor->RespawnWithDelay(true);
    }
    return;
  }

  auto sourceInEspm =
    partOne.GetEspm().GetBrowser().LookupById(hitData.source);

  const bool isSourceSpell =
    sourceInEspm.rec && sourceInEspm.rec->GetType() == espm::SPEL::kType;

  const auto equipment = aggressor->GetEquipment();

  // a player's shout hits with its variation's spell (thuum
  // docs/verbs/shouts.md), claimed against the shout the server accepted
  if (isSourceSpell &&
      (equipment.IsSpellEquipped(hitData.source) ||
       (aggressor == myActor &&
        IsVoiceVariation(equipment, hitData.source, partOne.worldState)))) {
    // thuum docs/verbs/spell-cast.md: a player's spell hits only as a cast
    // the server recorded: a fire-and-forget cast once (an area spell's
    // targets once each), a stream by the time it held its target (a hosted
    // actor's casts are its host's game's, M3)
    float magnitudeScale = 1.f;
    bool dualCast = false;
    if (aggressor == myActor) {
      const auto claim =
        partOne.ClaimCast(aggressor->GetFormId(), hitData.source,
                          targetRef->GetFormId(), targetRef->GetPos());
      if (claim.check != PartOne::CastCheck::Claimed) {
        spdlog::warn(
          "ActionListener::OnHit - {}: {:x} hits {:x} with spell "
          "{:x} {:.0f} units away; refused",
          claim.check == PartOne::CastCheck::TooFar ? "E_SPELL_RANGE"
                                                    : "E_SPELL_NO_CAST",
          aggressor->GetFormId(), targetRef->GetFormId(), hitData.source,
          (targetRef->GetPos() - aggressor->GetPos()).Length());
        return;
      }
      magnitudeScale = claim.scale;
      dualCast = claim.dual;
      // thuum docs/verbs/shadowbond.md: a bond spell's claimed hit bonds
      // the two until either acts, or the spell's duration ends
      if (targetRef->GetFormId() != aggressor->GetFormId() &&
          partOne.IsBondSpell(hitData.source)) {
        partOne.RecordBond(aggressor->GetFormId(), targetRef->GetFormId(),
                           hitData.source);
      }
      if (claim.relay) {
        // the first hit of a cast delivered to its target: the cast goes
        // to the other games now, at the target this hit names
        for (auto it = pendingCastRelays.begin();
             it != pendingCastRelays.end(); ++it) {
          if (it->first.first == aggressor->GetFormId() &&
              it->second.spell == hitData.source) {
            SpellCastData relayed = it->second;
            relayed.target = targetRef->GetFormId();
            spdlog::info("ActionListener::OnHit - {:x} cast {:x} relayed at "
                         "its first hit, on {:x}",
                         aggressor->GetFormId(), hitData.source,
                         targetRef->GetFormId());
            RelayCast(*aggressor, *myActor, relayed);
            pendingCastRelays.erase(it);
            break;
          }
        }
      }
    }
    OnSpellHit(aggressor, targetRef, hitData, magnitudeScale, dualCast);
    return;
  }

  // thuum docs/verbs/second-hand.md: a player's hit by a spell it neither
  // holds nor shouted counts only as one that a cast the server recorded
  // for it reaches through the records (a cloak's spell, a proc its
  // effects' scripts cast), from where the server has the caster now; any
  // other is refused where the log shows it (it fell to the weapon path's
  // debug lines before)
  // thuum docs/verbs/headshot.md: the shooter's game reports a head hit as
  // the mod's instant death spell on the victim (Core Impact Framework casts
  // it from the shooter at the victim); the server kills when a bow hit it
  // claimed on that victim came moments before and the records agree (a
  // bare head, a named race, the shooter's Archery), once per hit. The head
  // hit itself is the shooter's game's word (R1, Eli 2026-10-12).
  if (isSourceSpell && aggressor == myActor &&
      partOne.IsHeadshotSpell(hitData.source)) {
    auto* victim = dynamic_cast<MpActor*>(targetRef);
    if (!victim) {
      return;
    }
    const auto check = partOne.ClaimHeadshot(*aggressor, *victim);
    if (check == PartOne::HeadshotCheck::Killed) {
      spdlog::info("ActionListener::OnHit - headshot: {:x} kills {:x} with "
                   "{:x} on a claimed bow hit",
                   aggressor->GetFormId(), victim->GetFormId(),
                   hitData.source);
      return;
    }
    const char* code = check == PartOne::HeadshotCheck::Helmeted
      ? "E_HEADSHOT_HELMET"
      : check == PartOne::HeadshotCheck::Race    ? "E_HEADSHOT_RACE"
      : check == PartOne::HeadshotCheck::Archery ? "E_HEADSHOT_ARCHERY"
                                                 : "E_HEADSHOT_NO_SHOT";
    spdlog::warn("ActionListener::OnHit - {}: {:x} reports a headshot on "
                 "{:x} with {:x}; refused",
                 code, aggressor->GetFormId(), victim->GetFormId(),
                 hitData.source);
    return;
  }

  if (isSourceSpell && aggressor == myActor) {
    const auto claim = partOne.ClaimReached(
      aggressor->GetFormId(), hitData.source, targetRef->GetFormId(),
      targetRef->GetPos(), aggressor->GetPos());
    if (claim.check != PartOne::CastCheck::Claimed) {
      const char* code = claim.check == PartOne::CastCheck::TooFar
        ? "E_SPELL_RANGE"
        : claim.check == PartOne::CastCheck::Throttled ? "E_SPELL_TICK"
                                                       : "E_SPELL_NO_CAST";
      spdlog::warn("ActionListener::OnHit - {}: {:x} hits {:x} with spell "
                   "{:x}, which it holds not and no cast of its reaches "
                   "within {:.0f} units; refused",
                   code, aggressor->GetFormId(), targetRef->GetFormId(),
                   hitData.source,
                   (targetRef->GetPos() - aggressor->GetPos()).Length());
      return;
    }
    spdlog::info("ActionListener::OnHit - {:x} hits {:x} with spell {:x}, "
                 "claimed as a spell its cast reaches",
                 aggressor->GetFormId(), targetRef->GetFormId(),
                 hitData.source);
    OnSpellHit(aggressor, targetRef, hitData, claim.scale, claim.dual);
    return;
  }

  const bool isUnarmed = IsUnarmedAttack(hitData.source);

  if (equipment.inv.HasItem(hitData.source) || isUnarmed) {
    // thuum docs/verbs/damage-flags.md: the facts the flag rule needs; the
    // rule is Rust's (skymp-wire wire-rules damage, ADR-020)
    if (aggressor == myActor &&
        (hitData.isPowerAttack || hitData.isSneakAttack ||
         hitData.isBashAttack)) {
      const auto lastPowerStart =
        partOne.animationSystem.GetLastPowerAttackStartTime(*aggressor);
      const auto lastBashStart =
        partOne.animationSystem.GetLastBashStartTime(*aggressor);
      const auto sinceMs = [](std::chrono::steady_clock::time_point t) {
        return static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t)
            .count());
      };
      skymp::rules::FlagFacts facts{};
      facts.claims_power = hitData.isPowerAttack;
      facts.claims_sneak = hitData.isSneakAttack;
      facts.saw_power_start =
        lastPowerStart != std::chrono::steady_clock::time_point();
      facts.since_power_start_ms = facts.saw_power_start
        ? static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - lastPowerStart)
              .count())
        : 0;
      facts.is_sneaking = aggressor->GetAnimationVariableBool("IsSneaking");
      // thuum docs/verbs/blocking.md: a bash as a power attack is, by its
      // start among the attacker's animation events
      facts.claims_bash = hitData.isBashAttack;
      facts.saw_bash_start =
        lastBashStart != std::chrono::steady_clock::time_point();
      facts.since_bash_start_ms =
        facts.saw_bash_start ? sinceMs(lastBashStart) : 0;
      const auto kept = skymp::rules::backed_flags(facts);
      if (hitData.isPowerAttack && !kept.power) {
        spdlog::warn("ActionListener::OnHit - E_HIT_POWER: {:x} claims a "
                     "power attack it did not start; hit as a plain one",
                     aggressor->GetFormId());
      }
      if (hitData.isSneakAttack && !kept.sneak) {
        spdlog::warn("ActionListener::OnHit - E_HIT_SNEAK: {:x} claims a "
                     "sneak attack while not sneaking; hit as a plain one",
                     aggressor->GetFormId());
      }
      if (hitData.isBashAttack && !kept.bash) {
        spdlog::warn("ActionListener::OnHit - E_HIT_BASH: {:x} claims a bash "
                     "it did not start; hit as a plain one",
                     aggressor->GetFormId());
      }
      hitData.isPowerAttack = kept.power;
      hitData.isSneakAttack = kept.sneak;
      hitData.isBashAttack = kept.bash;
    }

    // Player against player only: the bound's body extents are a humanoid's
    // (creatures' are larger), and a hosted NPC's scale is its own record's
    MpActor* targetActor = targetRef->AsActor();
    if (aggressor == myActor && targetActor &&
        partOne.serverState.UserByActor(targetActor) !=
          Networking::InvalidUserId &&
        !IsBowOrCrossbowShot(hitData, &partOne.worldState)) {
      if (auto facts =
            MeleeFacts(*aggressor, *targetActor, partOne.worldState)) {
        const auto verdict = skymp::rules::melee_within_reach(*facts);
        if (!verdict.allowed) {
          spdlog::warn("ActionListener::OnHit - E_HIT_REACH: {:x} hits {:x} "
                       "from {:.0f} units, reach {:.0f}; refused",
                       aggressor->GetFormId(), targetActor->GetFormId(),
                       facts->distance, verdict.bound);
          return;
        }
      }
      const auto cone = ConeFacts(*aggressor, *targetActor,
                                  hitData.isPowerAttack, partOne.worldState);
      const auto coneVerdict = skymp::rules::melee_within_cone(cone);
      if (!coneVerdict.allowed) {
        spdlog::warn("ActionListener::OnHit - E_HIT_CONE: {:x} hits {:x} "
                     "outside {:.0f} degrees of its heading {:.0f}; refused",
                     aggressor->GetFormId(), targetActor->GetFormId(),
                     coneVerdict.bound, cone.heading);
        return;
      }
    }
    // thuum docs/verbs/marksman.md: a player's arrow hits only as a shot the
    // server recorded, and once (a hosted actor's shots are its host's
    // game's, M3), at its draw power: the engine scales a player's arrow's
    // damage by the power once, at launch (the re-analyst's read of
    // Projectile::Launch, 2026-10-09)
    float damageMult = 1.f;
    if (aggressor == myActor &&
        IsBowOrCrossbowShot(hitData, &partOne.worldState)) {
      const auto claim = partOne.ClaimShot(
        aggressor->GetFormId(), hitData.source, targetRef->GetPos());
      if (claim.check != PartOne::ShotCheck::Claimed) {
        spdlog::warn(
          "ActionListener::OnHit - {}: {:x} hits {:x} with {:x} "
          "{:.0f} units away; refused",
          claim.check == PartOne::ShotCheck::TooFar ? "E_HIT_RANGE"
                                                    : "E_HIT_NO_SHOT",
          aggressor->GetFormId(), targetRef->GetFormId(), hitData.source,
          (targetRef->GetPos() - aggressor->GetPos()).Length());
        return;
      }
      damageMult = claim.power;
      // thuum docs/verbs/headshot.md: a headshot may ride this hit
      partOne.NoteBowHit(aggressor->GetFormId(), targetRef->GetFormId());
    }
    OnWeaponHit(aggressor, targetRef, hitData, isUnarmed, damageMult);
    return;
  }

  if (aggressor != myActor) {
    // thuum docs/verbs/npc-fate.md (playtest 34, 2026-10-25): a hosted
    // actor's hit with a weapon its record's hands lack ended here at debug
    // level, silent in the lab's log, and two server-made bandits in Bleak
    // Falls Temple never landed a hit on the server: the host's engine
    // dresses a server-made copy from the base's own roll (npc-puppet's Rule
    // A) while the record holds the server's, and nothing pins the two the
    // way a game-file actor's fate pins its hands. The hit stays dropped
    // (the server's number needs the server's weapon); the drop is said
    // once a minute per actor
    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point>
      lastWeaponDropLog;
    const auto now = std::chrono::steady_clock::now();
    auto& last = lastWeaponDropLog[aggressor->GetFormId()];
    if (last == std::chrono::steady_clock::time_point() ||
        now - last >= std::chrono::minutes(1)) {
      last = now;
      spdlog::info("ActionListener::OnHit - E_HIT_WEAPON: hosted {:x} hits "
                   "{:x} with {:x}, which its record's hands lack ({} in its "
                   "inventory); dropped",
                   aggressor->GetFormId(), hitData.target, hitData.source,
                   aggressor->GetInventory().HasItem(hitData.source)
                     ? "held unequipped"
                     : "absent");
    }
    return;
  }

  if (aggressor->GetInventory().HasItem(hitData.source) == false) {
    spdlog::debug("{:x} actor has no {:x} weapon and can't attack",
                  hitData.aggressor, hitData.source);
  }

  spdlog::debug("{:x} weapon is not equipped by {:x} actor and cannot be used",
                hitData.source, hitData.aggressor);
}

void ActionListener::OnUpdateAnimVariables(
  const RawMessageData& rawMsgData, const UpdateAnimVariablesMessage& msg)
{
  const MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!myActor) {
    throw std::runtime_error("Unable to change values without Actor attached");
  }

  SendToNeighbours(myActor->idx, rawMsgData);
}

void ActionListener::OnSpellCast(const RawMessageData& rawMsgData,
                                 const SpellCastMessage& msg)
{
  MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);

  if (!myActor) {
    throw std::runtime_error("Unable to change values without Actor attached");
  }

  MpActor* caster = nullptr;

  SpellCastData spellCastData = msg.data;

  if (spellCastData.caster == 0x14 ||
      spellCastData.caster == myActor->GetFormId()) {
    caster = myActor;
    spellCastData.caster = caster->GetFormId();
  } else {
    caster = &partOne.worldState.GetFormAt<MpActor>(spellCastData.caster);
    // thuum docs/verbs/cell-host.md: a hosted caster is its cell's host's;
    // an actor a user plays is nobody's to report for
    if (partOne.serverState.UserByActor(caster) != Networking::InvalidUserId ||
        partOne.worldState.HostOf(*caster) != myActor) {
      spdlog::error(
        "SendToNeighbours - No permission to send OnSpellCast with "
        "caster actor {:x}",
        caster->GetFormId());
      return;
    }
  }

  if (spellCastData.target == 0x14) {
    spellCastData.target = myActor->GetFormId();
  }

  if (caster->IsDead()) {
    // thuum ADR-050 decision 3 (train 51): a player's rule, as OnHit's
    spdlog::info(fmt::format(
      "{:x} actor is dead and can't spell cast{}", caster->GetFormId(),
      caster->IsCreatedAsPlayer() ? ". requesting respawn in order to fix "
                                    "death state"
                                  : ""));
    if (caster->IsCreatedAsPlayer()) {
      caster->RespawnWithDelay(true);
    }
    return;
  }

  // thuum docs/verbs/spell-cast.md: the launch a target-location cast
  // carries is the point the caster's game picked along its aim, which each
  // neighbour's game launches the spell at (R1). Its game picks no farther
  // than the spell's ray from the caster's magic node (LaunchRay), a node
  // within kMaxLaunchOffsetUnits of where the server has the caster, as a
  // bow's launch is; a point beyond the two goes to no other game. The cast
  // itself stands, and every relay below carries it without the launch, so
  // the neighbours place it as they did before casts carried one
  if (spellCastData.hasLaunch) {
    const auto ray = LaunchRay(partOne.worldState, spellCastData.spell);
    const float distance =
      (NiPoint3(spellCastData.originX, spellCastData.originY,
                spellCastData.originZ) -
       caster->GetPos())
        .Length();
    if (!ray || distance > *ray + kMaxLaunchOffsetUnits) {
      spdlog::warn("ActionListener::OnSpellCast - E_SPELL_ORIGIN: {:x} "
                   "launched {:x} {:.0f} units from its position, its ray "
                   "{:.0f}; relayed without the launch",
                   caster->GetFormId(), spellCastData.spell, distance,
                   ray.value_or(0.f));
      spellCastData.hasLaunch = false;
      spellCastData.originX = 0.f;
      spellCastData.originY = 0.f;
      spellCastData.originZ = 0.f;
      spellCastData.launchAngleX = 0.f;
      spellCastData.launchAngleZ = 0.f;
    }
  }

  // thuum docs/verbs/spell-cast.md: the hand the cast comes from (the
  // validator holds castingSource to 0 to 3)
  const auto hand =
    static_cast<uint8_t>(std::clamp(spellCastData.castingSource, 0, 3));

  // A cast's end ends that hand's stream whatever the hand holds by now (it
  // used to need the spell still equipped, so a changed hand lost the stop),
  // and reaches every other screen (reliable: a lost end left an observer's
  // figure casting, playtest six's Flames). Every relay below is the cast as
  // resolved (RelayCast): the client's bytes named their caster 0x14 when
  // the client chose, which each neighbour's game reads as its own player
  if (spellCastData.interruptCast) {
    EndSelfStream(partOne.worldState, *caster, spellCastData.spell);
    partOne.EndCast(caster->GetFormId(), hand);
    // a stream delivered to its target that ended before any hit: its start
    // never reached the other games, so neither does its end
    if (pendingCastRelays.erase({ caster->GetFormId(), hand }) > 0) {
      spdlog::info("ActionListener::OnSpellCast - {:x} ended {:x} in hand "
                   "{} before it hit anyone; not relayed",
                   caster->GetFormId(), spellCastData.spell, hand);
      return;
    }
    RelayCast(*caster, *myActor, spellCastData);
    return;
  }

  const auto equipment = caster->GetEquipment();

  // thuum docs/verbs/shouts.md: a player's shout comes from the voice with
  // a shout in the voice slot, and the shouts rule judges it against the
  // server's record. An accepted one starts its recovery and waits for its
  // hits as a cast does, and every screen draws it as a spell's cast: the
  // animation relay plays the figure's shoutStart, and the cast launches
  // the breath from the figure's voice (playtest fourteen: the shouter moved
  // as though shouting with no breath on the other screen). Its damage
  // stays the server's, from the shouter's own hits
  if (const auto shout = VoiceShout(equipment, partOne.worldState);
      shout && hand == 2 && caster == myActor) {
    const auto facts =
      VoiceFactsOf(*caster, *shout, spellCastData.spell, partOne.worldState);
    const auto check = partOne.Shout(caster->GetFormId(), facts);
    if (check != PartOne::VoiceCheck::Accepted) {
      spdlog::warn("ActionListener::OnSpellCast - {}: {:x} shouts {:x} "
                   "with spell {:x} (variation {}, words unlocked {:#b}); "
                   "refused",
                   VoiceRefusalCode(check), caster->GetFormId(), *shout,
                   spellCastData.spell, facts.variation, facts.wordsUnlocked);
      return;
    }
    partOne.StartCast(caster->GetFormId(), spellCastData.spell, hand,
                      caster->GetPos(), spellCastData.isDualCasting);
    spdlog::info("ActionListener::OnSpellCast - {:x} shouts {:x}, variation "
                 "{} (spell {:x}); recovers in {:.1f} s",
                 caster->GetFormId(), *shout, facts.variation,
                 spellCastData.spell,
                 partOne.VoiceRecoveryLeftMs(caster->GetFormId()) / 1000.0);
    EndConcealment(partOne, *caster, spellCastData.spell);
    RunSelfSpell(partOne.worldState, *caster, spellCastData.spell);
    RelayCast(*caster, *myActor, spellCastData);
    return;
  }

  if (equipment.IsSpellEquipped(spellCastData.spell) == false) {
    spdlog::info("ActionListener::OnSpellCast - spell {0:x} not "
                 "found in equipment",
                 spellCastData.spell);
    return;
  }

  // the cast waits for its hit (a stream stays open until its end), and
  // every screen draws it; one delivered to its target (touch, target
  // actor) is drawn there only once its first hit names the target (thuum
  // docs/verbs/spell-cast.md, playtest twenty-three: the caster's guess at
  // the target was 0 at every Shadowbond and Azure Reconstruction cast)
  const bool relayAtFirstHit =
    partOne.StartCast(caster->GetFormId(), spellCastData.spell, hand,
                      caster->GetPos(), spellCastData.isDualCasting);
  EndConcealment(partOne, *caster, spellCastData.spell);
  RunSelfSpell(partOne.worldState, *caster, spellCastData.spell);

  // thuum docs/verbs/cast-teleport.md: a spell the server names as a
  // teleport moves its caster in the caster's own game (the engine places
  // its marker, a mod's script moves the player), and the server takes that
  // one jump within its reach of where the caster stood, for the entry's
  // wait. Its cast reaches no other game unless the entry relays it: a
  // neighbour's figure would launch it there, and the marker's script would
  // move that game's own player. A return (docs/verbs/ghostwalk.md: reach 0,
  // back where it cast) is relayed: its Self cast makes the figure
  // invisible, and the figure's effect script is blanked there.
  const float teleportReach = partOne.TeleportReach(spellCastData.spell);
  const bool teleport = teleportReach >= 0.f;
  if (teleport && caster == myActor) {
    const uint64_t waitMs = partOne.TeleportWaitMs(spellCastData.spell);
    partOne.PermitCastJump(
      caster->GetFormId(),
      caster->GetCellOrWorld().ToFormId(partOne.worldState.espmFiles),
      caster->GetPos(), teleportReach, waitMs);
    // the look on other screens: the mod's script plays the teleport's
    // effects in the caster's game alone, so the entry's look plays its
    // vanish on the caster's figure in every other game until the landing
    // (MovementValidation, PartOne::PlayTeleportLanding)
    partOne.PlayTeleportVanish(*caster, spellCastData.spell, waitMs);
  }
  if (teleport && !partOne.TeleportRelays(spellCastData.spell)) {
    spdlog::info("ActionListener::OnSpellCast - {:x} cast teleport {:x}, "
                 "reach {}; not relayed",
                 caster->GetFormId(), spellCastData.spell, teleportReach);
  } else {
    if (teleport) {
      spdlog::info("ActionListener::OnSpellCast - {:x} cast teleport {:x}, "
                   "reach {}; relayed",
                   caster->GetFormId(), spellCastData.spell, teleportReach);
    }
    if (relayAtFirstHit) {
      pendingCastRelays[{ caster->GetFormId(), hand }] = spellCastData;
      spdlog::info("ActionListener::OnSpellCast - {:x} cast {:x} in hand {}, "
                   "delivered to its target; relay waits for its first hit",
                   caster->GetFormId(), spellCastData.spell, hand);
    } else {
      RelayCast(*caster, *myActor, spellCastData);
    }
  }

  auto& browser = partOne.worldState.GetEspm().GetBrowser();

  const std::array<VarValue, 1> args{ VarValue(
    std::make_shared<EspmGameObject>(
      browser.LookupById(spellCastData.spell))) };

  caster->SendPapyrusEvent("OnSpellCast", args.data(), args.size());

  const auto targetRef = std::dynamic_pointer_cast<MpObjectReference>(
    partOne.worldState.LookupFormById(spellCastData.target));

  if (!targetRef) {
    spdlog::info(
      "ActionListener::OnSpellCast - MpObjectReference not found for "
      "spellCastData.target {:x}",
      spellCastData.target);
    return;
  }

  // TODO: apply magic effects if this is not a fireball-like spell.
  // Previous attempt was not successful, so it was deleted.
}

void ActionListener::OnUnknown(const RawMessageData& rawMsgData)
{
  spdlog::warn("ActionListener::OnUnknown - Got unhandled message");
}

void ActionListener::OnSpellHit(MpActor* aggressor,
                                MpObjectReference* targetRef,
                                const HitData& hitData, float magnitudeScale,
                                bool dualCast)
{
  SendPapyrusOnHitEvent(aggressor, targetRef, hitData);

  auto* targetActorPtr = targetRef ? targetRef->AsActor() : nullptr;
  if (!targetActorPtr) {
    return; // Not an actor, damage calculation is not needed
  }

  // thuum docs/verbs/magic-effects.md: the spell's effects run through the
  // effect rule at the claim's scale: a stream's hit counts its seconds at
  // once (spell-cast's claim), a fire-and-forget spell's effects by their
  // records; every value they touch, not Health alone (shock's Magicka,
  // frost's Stamina), and the ones with a duration keep running
  std::vector<espm::Effects::Effect> effects;
  bool stream = false;
  try {
    const auto spell =
      espm::GetData<espm::SPEL>(hitData.source, &partOne.worldState);
    stream = spell.spellItem &&
      spell.spellItem->castType == espm::SPEL::CastType::Concentration;
    effects = MpActor::EffectsOf(&partOne.worldState, hitData.source);
  } catch (std::exception& e) {
    spdlog::warn("OnSpellHit - spell {:x}: {}", hitData.source, e.what());
    return;
  }
  const float scale = std::isfinite(magnitudeScale) && magnitudeScale > 0.f
    ? magnitudeScale
    : 0.f;
  // thuum docs/verbs/self-cast.md: an ethereal target takes nothing from
  // another's spell (a heal from another included, for now)
  if (aggressor != targetActorPtr &&
      targetActorPtr->HasRunningEffectOfType(
        espm::MGEF::EffectType::Etherealize)) {
    spdlog::info("OnSpellHit - {:x}'s {:x} reaches {:x}, which is ethereal: "
                 "nothing applied",
                 aggressor->GetFormId(), hitData.source,
                 targetActorPtr->GetFormId());
    return;
  }
  // thuum docs/verbs/scripted-effects.md (train-31, ADR-029's first
  // allowlist entry): a spell the server names as a scripted kill has its
  // script's arithmetic run here, from the claimed hit, before the record's
  // other effects, as the script runs at its effect's start with the
  // target's Health as it stands; the target dies in every game or in none
  if (aggressor != targetActorPtr &&
      partOne.IsScriptedKillSpell(hitData.source)) {
    partOne.ClaimScriptedKill(*aggressor, *targetActorPtr, hitData.source,
                              effects);
  }
  targetActorPtr->ApplyEffects(hitData.source, effects, scale,
                               aggressor->GetFormId(), stream, dualCast);
  // thuum docs/verbs/npc-fate.md (ADR-048's B): the hit the server
  // numbered, a death report's evidence (a self cast is no hit taken)
  if (aggressor != targetActorPtr) {
    targetActorPtr->NoteHitBy(aggressor->GetFormId(),
                              std::chrono::steady_clock::now());
  }

  // thuum docs/verbs/stagger.md: a spell's Stagger effect (Unrelenting
  // Force's push) staggers its target by the effect's magnitude, played in
  // the target's game as a weapon's stagger is, since the server decides a
  // player's staggers (playtest fifteen, Eli: "does unrelenting force
  // ragdoll"; HYPOTHESIS: the engine's stagger at Unrelenting Force's 1.5
  // knocks the target down, to read on the lab)
  float spellStagger = 0.f;
  bool anyHostileEffectHeld = false;
  for (const auto& effect : effects) {
    // a perk's rider (the Impact perk's stagger on every Firebolt, read in
    // the lab's logs: 67 of 91 hits staggered with no perk) is judged by its
    // conditions as ApplyEffects' effects are (thuum ADR-039: HasPerk is
    // unknown to the server before M5, so it refuses and logs)
    const bool gated = aggressor != targetActorPtr
      ? !MpActor::EffectConditionsHold(&partOne.worldState, effect,
                                       hitData.source, *targetActorPtr,
                                       *aggressor)
      : MpActor::IsConditionGated(&partOne.worldState, effect);
    if (gated) {
      continue;
    }
    // thuum docs/verbs/magic-effects.md (train-31; playtest twenty-eight,
    // Eli: "i should be able to cast healing hands on them"): a hit begins
    // a fight only through an effect whose record is hostile or
    // detrimental. A heal held on another player (Healing Hands) begins
    // nothing, as the engine's neighbours only remark on a non-hostile
    // spell (SayOnHitByMagicEffectScript's non-hostile topic). An effect
    // whose record the server cannot read counts as hostile, as every held
    // effect did before
    try {
      const auto mgef =
        espm::GetData<espm::MGEF>(effect.effectId, &partOne.worldState);
      if (mgef.data.IsFlagSet(espm::MGEF::Flags::Hostile) ||
          mgef.data.IsFlagSet(espm::MGEF::Flags::Detrimental)) {
        anyHostileEffectHeld = true;
      }
      if (mgef.data.effectType == espm::MGEF::EffectType::Stagger) {
        spellStagger = std::max(spellStagger, effect.magnitude * scale);
      }
    } catch (std::exception&) {
      anyHostileEffectHeld = true;
    }
  }
  if (spellStagger > 0.f && aggressor != targetActorPtr &&
      targetActorPtr->GetChangeForm().actorValues.healthPercentage > 0.f) {
    PlayStagger(*aggressor, *targetActorPtr, spellStagger,
                fmt::format("spell {:x}", hitData.source));
  }

  spdlog::info("OnSpellHit - Target {0:x} is hit by {1:x} spell at {2} times "
               "its magnitudes ({3}). By caster: {4:x}",
               targetActorPtr->GetFormId(), hitData.source, scale,
               stream ? "a stream's seconds" : "once", aggressor->GetFormId());

  // a spell hit begins a fight as a weapon's does (ADR-023), once a hostile
  // effect of it held: a hit whose every effect its conditions refuse (a
  // cloak's proc on a peaceful neighbour, which the engine itself never
  // reports) begins nothing (thuum ADR-039), and neither does a heal
  if (anyHostileEffectHeld) {
    NotifyHostility(*aggressor, *targetActorPtr,
                    std::chrono::steady_clock::now());
  }
}

void ActionListener::OnWeaponHit(MpActor* aggressor,
                                 MpObjectReference* targetRef, HitData hitData,
                                 [[maybe_unused]] bool isUnarmed,
                                 float damageMult)
{
  const auto currentHitTime = std::chrono::steady_clock::now();

  auto* targetActorPtr = targetRef ? targetRef->AsActor() : nullptr;

  // thuum docs/verbs/blocking.md: whether the hit was blocked is the
  // server's, from its own records (wire-rules block), never the attacker's
  // game's verdict, which saw the target's block a round trip late; scripts
  // hear the server's (OnHit's abHitBlocked)
  const float blockedShare = targetActorPtr
    ? BlockedShare(*aggressor, *targetActorPtr, hitData, partOne.worldState)
    : 0.f;
  if (hitData.isHitBlocked != (blockedShare > 0.f)) {
    spdlog::info("OnWeaponHit - {:x} hits {:x}: blocked {} in the attacker's "
                 "game, {} by the server's records",
                 aggressor->GetFormId(), hitData.target, hitData.isHitBlocked,
                 blockedShare > 0.f);
  }
  hitData.isHitBlocked = blockedShare > 0.f;

  SendPapyrusOnHitEvent(aggressor, targetRef, hitData);

  if (!targetActorPtr) {
    return; // Not an actor, damage calculation is not needed
  }

  auto& targetActor = *targetActorPtr;

  const auto lastHitTimeAnyTarget = aggressor->GetLastHitTime(std::nullopt);
  const std::chrono::duration<float> timePassedAnyTarget =
    currentHitTime - lastHitTimeAnyTarget;

  constexpr float kSplashTimeWindow = 0.1f;
  constexpr size_t kMaxSplashTargets = 4;

  // Splash attack detection. Non-vanilla feature, fixes anticheat-vs-mod
  // issues
  const bool isSplash = timePassedAnyTarget.count() < kSplashTimeWindow;

  if (isSplash) {
    spdlog::info("Splash attack detected from aggressor {:x} to target {:x}",
                 aggressor->GetFormId(), targetActor.GetFormId());

    // Check if THIS specific target was hit recently
    auto lastHitSpecific = aggressor->GetLastHitTime(targetActor.GetFormId());
    std::chrono::duration<float> timeSinceSpecific =
      currentHitTime - lastHitSpecific;

    // If the specific target was hit faster than the splash window
    if (timeSinceSpecific.count() < kSplashTimeWindow) {
      spdlog::warn("Splash attack from {:x} to {:x} ignored, target hit "
                   "too recently",
                   aggressor->GetFormId(), targetActor.GetFormId());
      return;
    }

    if (aggressor->CountRecentHits(std::chrono::duration<float>(
          kSplashTimeWindow)) >= kMaxSplashTargets) {
      spdlog::warn("Splash attack from {:x} to {:x} ignored, too many "
                   "targets hit recently",
                   aggressor->GetFormId(), targetActor.GetFormId());
      return;
    }
  } else if (!CanHit(*aggressor, hitData, timePassedAnyTarget)) {
    WorldState* espmProvider = targetActor.GetParent();
    auto weapDNAM =
      espm::GetData<espm::WEAP>(hitData.source, espmProvider).weapDNAM;
    float expectedAttackTime = (1.1 * (1 / weapDNAM->speed)) -
      (1.1 * (1 / weapDNAM->speed) * (weapDNAM->speed <= 0.75 ? 0.45 : 0.3));
    spdlog::debug(
      "OnWeaponHit - Target {0:x} is not available for attack due to fast "
      "attack speed. Weapon: {1:x}. Elapsed time: {2}. Expected attack time: "
      "{3}",
      hitData.target, hitData.source, timePassedAnyTarget.count(),
      expectedAttackTime);
    return;
  }

  // if (IsDistanceValid(*aggressor, targetActor, hitData) == false) {
  //   float distance =
  //     std::sqrt(GetSqrDistanceToBounds(*aggressor, targetActor));

  //   // TODO: fix bounding boxes for creatures such as chicken, mudcrab, etc
  //   float reachPveHotfixMult =
  //     (aggressor->GetBaseId() <= 0x7 && targetActor.GetBaseId() <= 0x7)
  //     ? 1.f
  //     : std::numeric_limits<float>::infinity();

  //   float reach = GetReach(*aggressor, hitData.source, reachPveHotfixMult);
  //   uint32_t aggressorId = aggressor->GetFormId();
  //   uint32_t targetId = targetActor.GetFormId();
  //   spdlog::debug(
  //     fmt::format("{:x} actor can't reach {:x} target because distance {} is
  //     "
  //                 "greater then first actor attack radius {}",
  //                 aggressorId, targetId, distance, reach));
  //   return;
  // }

  ActorValues currentActorValues = targetActor.GetChangeForm().actorValues;

  float healthPercentage = currentActorValues.healthPercentage;

  // a block's share is the rule's (the formula counts no block)
  float damage = partOne.CalculateDamage(*aggressor, targetActor, hitData) *
    damageMult * (1.f - blockedShare);
  damage = damage < 0.f || !std::isfinite(damage) ? 0.f : damage;
  if (targetActor.IsGodMode()) {
    // thuum docs/verbs/god-mode.md: TGM, no damage taken; the fight still
    // begins below
    damage = 0.f;
  }
  // thuum docs/verbs/self-cast.md: an attack ends its attacker's
  // invisibility and ethereal state, and an ethereal target takes nothing
  // (no damage, no poison's dose, no stagger)
  EndConcealment(partOne, *aggressor);
  const bool ethereal =
    targetActor.HasRunningEffectOfType(espm::MGEF::EffectType::Etherealize);
  if (ethereal) {
    spdlog::info("OnWeaponHit - {:x} hits {:x}, which is ethereal: no damage",
                 aggressor->GetFormId(), targetActor.GetFormId());
    damage = 0.f;
  }
  float outBaseHealth = 0.f;
  currentActorValues.healthPercentage = CalculateCurrentHealthPercentage(
    targetActor, damage, healthPercentage, &outBaseHealth);

  currentActorValues.healthPercentage =
    currentActorValues.healthPercentage < 0.f
    ? 0.f
    : currentActorValues.healthPercentage;

  targetActor.NetSetPercentages(
    currentActorValues, aggressor,
    std::vector<espm::ActorValue>{ espm::ActorValue::Health });
  aggressor->SetLastHitTime(targetActor.GetFormId(), currentHitTime);
  targetActor.SetLastHitTakenTime(currentHitTime);
  // thuum docs/verbs/npc-fate.md (ADR-048's B): the hit the server numbered,
  // the evidence a death report naming this aggressor is checked against
  targetActor.NoteHitBy(aggressor->GetFormId(), currentHitTime);

  // thuum docs/verbs/marksman.md: the server's own damage for every weapon
  // hit, at info as OnSpellHit's, so a lab run can tell it from what the
  // victim's game reports afterwards
  spdlog::info("OnWeaponHit - {:x} hits {:x} with {:x} for {} damage "
               "(blocked share {}): health {} -> {} of base {}",
               aggressor->GetFormId(), hitData.target, hitData.source, damage,
               blockedShare, healthPercentage,
               currentActorValues.healthPercentage, outBaseHealth);

  NotifyHostility(*aggressor, targetActor, currentHitTime);

  // thuum docs/verbs/poison.md: a poisoned weapon's hit runs the poison's
  // effects on the target through the effect rule (ADR-028), the attacker
  // as caster, and spends a dose; the attacker's game is told its inventory,
  // so its ExtraPoison agrees. HYPOTHESIS: the game spends a dose on a
  // blocked hit as well (unmeasured)
  if (const auto poison =
        ethereal ? std::nullopt : aggressor->SpendPoisonDose(hitData.source)) {
    const auto effects = MpActor::EffectsOf(&partOne.worldState, *poison);
    targetActor.ApplyEffects(*poison, effects, 1.f, aggressor->GetFormId(),
                             false);
    spdlog::info("OnWeaponHit - {:x}'s {:x} carried poison {:x}: its effects "
                 "run on {:x}, a dose spent",
                 aggressor->GetFormId(), hitData.source, *poison,
                 targetActor.GetFormId());
  }

  // the engine's reaction skips a hit that killed, and one on an ethereal
  // target
  if (currentActorValues.healthPercentage > 0.f && !ethereal) {
    NotifyStagger(*aggressor, targetActor, hitData, damage > 0.f);
  }
}

// thuum docs/verbs/stagger.md: the stagger the engine's own rule gives the
// hit (wire-rules stagger), played in the victim's game through the engine's
// start (TESModPlatform.Stagger, ID 37710) with the server's magnitude and
// the attacker's figure as its source; the victim's client relays the
// stagger's animation as any of its events. The victim's game never sees the
// hit, so it cannot stagger by itself.
void ActionListener::NotifyStagger(MpActor& aggressor, MpActor& target,
                                   const HitData& hitData, bool dealtDamage)
{
  const auto targetUser = partOne.serverState.UserByActor(&target);
  if (targetUser == Networking::InvalidUserId) {
    // thuum docs/verbs/npc-fate.md (ADR-050 decision 2): a hosted actor's
    // host game reacts to its own hits; a hit by another player's hand
    // reaches that game only as the server's cause, so the stagger is sent
    // to the host for the engine to play on the actor (HYPOTHESIS H-52s)
    MpActor* host = target.HostedBy();
    if (!host || host == &aggressor || !partOne.worldState.npcEngineDeaths) {
      return;
    }
  }
  const std::string attackName =
    partOne.animationSystem.GetLastAttackStartName(aggressor);
  thread_local std::mt19937 rng{ std::random_device{}() };
  const auto roll =
    static_cast<uint8_t>(std::uniform_int_distribution<int>(0, 99)(rng));
  const float magnitude = skymp::rules::stagger_magnitude(
    StaggerSettingsOf(partOne.worldState),
    StaggerFactsOf(aggressor, target, hitData, attackName, true, dealtDamage,
                   partOne.worldState),
    roll);
  if (magnitude <= 0.f) {
    return;
  }
  PlayStagger(aggressor, target, magnitude,
              attackName.empty() ? std::string("hit") : attackName);
}

void ActionListener::PlayStagger(MpActor& aggressor, MpActor& target,
                                 float magnitude, const std::string& what)
{
  // the game that plays it: the victim's own, or its host's for a hosted
  // NPC (ADR-050 decision 2; the snippet runs on a player's actor alone,
  // SpSnippet::Execute)
  MpActor* executor = &target;
  if (partOne.serverState.UserByActor(&target) == Networking::InvalidUserId) {
    executor = target.HostedBy();
    if (!executor || executor == &aggressor) {
      return; // a hosted actor's host game reacts to its own hits
    }
  }
  SpSnippetObjectArgument player;
  player.formId = executor == &target ? 0x14 : target.GetFormId();
  player.type = "Actor";
  SpSnippetObjectArgument source;
  source.formId = aggressor.GetFormId();
  source.type = "Actor";
  const std::vector<std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
    args{ player, static_cast<double>(magnitude), source };
  SpSnippet("TESModPlatform", "Stagger", args, 0)
    .Execute(executor, SpSnippetMode::kNoReturnResult);
  if (executor != &target) {
    // the host's engine plays it and its relay carries the event to the
    // puppets (npc-stream); no second relay from here
    return;
  }

  // every game that sees the victim plays the stagger on its figure: the
  // engine's stagger in the victim's own game is an action its client never
  // hears as an animation event (b-stagger 20261011-213039: IsStaggering in
  // that game, no staggerStart among its events), so the server relays the
  // event as the victim's own (deathService lets a relayed staggerStart
  // through; the victim's client draws no figure of itself). Its numChanges
  // stays clear of the victim's own counter, which a figure compares for
  // equality only (sync/animation.ts applyAnimation).
  static uint32_t serverAnimations = 0x40000000;
  UpdateAnimationMessage stagger;
  stagger.idx = target.GetIdx();
  stagger.data.animEventName = "staggerStart";
  stagger.data.numChanges = ++serverAnimations;
  for (auto* listener : target.GetActorListeners()) {
    listener->SendToUser(stagger, true);
  }
  spdlog::info("ActionListener::OnWeaponHit - stagger: {:x}'s {} on {:x} "
               "staggers it by {}",
               aggressor.GetFormId(), what, target.GetFormId(), magnitude);
}

// thuum docs/verbs/hostility-sync.md (ADR-023): the attacker's engine marks
// the victim an enemy when its hit lands, but the victim's engine never sees
// that hit. When a hit between two players begins a fight (the Rust rule's
// fights table), the victim's game starts combat between its figure of the
// attacker and its own player (0x14 on that client). The engine then refuses
// the victim a wait or a sleep with the attacker near, as it does the
// attacker; PartOne::TickFights ends the fight on both games.
void ActionListener::NotifyHostility(MpActor& aggressor, MpActor& target,
                                     std::chrono::steady_clock::time_point now)
{
  skymp::rules::HostilityFacts facts{};
  facts.aggressor_is_player =
    partOne.serverState.UserByActor(&aggressor) != Networking::InvalidUserId;
  facts.target_is_player =
    partOne.serverState.UserByActor(&target) != Networking::InvalidUserId;
  facts.same_actor = &aggressor == &target;
  if (!skymp::rules::hostility_between_players(facts)) {
    return;
  }
  const auto nowMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch())
      .count());
  if (!partOne.GetFights().hit(aggressor.GetFormId(), target.GetFormId(),
                               nowMs)) {
    return;
  }

  SpSnippetObjectArgument player;
  player.formId = 0x14;
  player.type = "Actor";
  const std::vector<std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
    args{ player };
  SpSnippet("Actor", "StartCombat", args, aggressor.GetFormId())
    .Execute(&target, SpSnippetMode::kNoReturnResult);
  spdlog::info("ActionListener::OnWeaponHit - hostility: {:x} hit {:x}, a "
               "fight begins; the victim's game is told",
               aggressor.GetFormId(), target.GetFormId());
}

void ActionListener::SendPapyrusOnHitEvent(MpActor* aggressor,
                                           MpObjectReference* target,
                                           const HitData& hitData)
{
  auto& browser = partOne.worldState.GetEspm().GetBrowser();
  std::array<VarValue, 7> args;
  args[0] = VarValue(aggressor->ToGameObject()); // akAggressor
  args[1] = VarValue(std::make_shared<EspmGameObject>(
    browser.LookupById(hitData.source)));    // akSource
  args[2] = VarValue::None();                // akProjectile
  args[3] = VarValue(hitData.isPowerAttack); // abPowerAttack
  args[4] = VarValue(hitData.isSneakAttack); // abSneakAttack
  args[5] = VarValue(hitData.isBashAttack);  // abBashAttack
  args[6] = VarValue(hitData.isHitBlocked);  // abHitBlocked
  target->SendPapyrusEvent("OnHit", args.data(), args.size());
}
