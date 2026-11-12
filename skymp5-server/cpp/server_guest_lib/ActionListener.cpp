#include "ActionListener.h"
#include "AnimationSystem.h"
#include "ConditionsEvaluator.h"
#include "ConsoleCommands.h"
#include "CropRegeneration.h"
#include "Exceptions.h"
#include "GetBaseActorValues.h"
#include "HitData.h"
#include "LocationalDataUtils.h"
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
#include "libespm/ACHR.h"
#include "libespm/ALCH.h"
#include "libespm/Convert.h"
#include "libespm/GLOB.h"
#include "libespm/GMST.h"
#include "libespm/HDPT.h"
#include "libespm/LVLI.h"
#include "libespm/ObjectBounds.h"
#include "libespm/QUST.h"
#include "libespm/RACE.h"
#include "libespm/REFR.h"
#include "libespm/SHOU.h"
#include "libespm/SPEL.h"
#include "libespm/ScriptData.h"
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
#include <set>
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

namespace {
// thuum docs/verbs/npc-fate.md ("Reanimation is any player's, on any body";
// Eli, playtest 37: "anyone should be able to reanimate any corpse, that's
// Just Gameplay"): the effect among `effects` whose archetype is Reanimate,
// by its MGEF record (libespm MGEF::DATA::effectType, DATA 0x40;
// EffectType::Reanimate is 22, the value CommonLibSSE-NG names kReanimate in
// include/RE/E/EffectArchetypes.h:32), or none. An effect whose record the
// server cannot read is no reanimation.
std::optional<uint32_t> ReanimateEffectOf(
  WorldState* worldState, const std::vector<espm::Effects::Effect>& effects)
{
  for (const auto& effect : effects) {
    try {
      if (espm::GetData<espm::MGEF>(effect.effectId, worldState)
            .data.effectType == espm::MGEF::EffectType::Reanimate) {
        return effect.effectId;
      }
    } catch (std::exception& e) {
      spdlog::warn("ReanimateEffectOf - effect {:x}: {}", effect.effectId,
                   e.what());
    }
  }
  return std::nullopt;
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
  // thuum docs/verbs/npc-fate.md (ADR-060): a reanimate at a dead body
  // reaches every other game as any cast's look; the body's host used to be
  // skipped for the server's own Spell.Cast snippet there, retired with
  // the commanded claim (the caster's own engine runs the effect, the body
  // is its seat's from the hit; the former host's copy is a puppet and
  // stands by the server's live word, HYPOTHESIS H1 of ADR-060)
  for (auto listener : caster.GetActorListeners()) {
    // A relay is one game's report for the other games, so only a listener
    // with a user hears it, and directly. GetActorToSendTo hands an offline
    // listener's copy (a game-file or server-made NPC in reach) to its
    // cell's host, and when the caster is hosted that host is the very game
    // that reported the cast: it received one SpellCast per NPC it hosted
    // near the caster (ten to thirteen in playtest 37) and cast each copy on
    // its live atronach, a real bolt every time (finding 53, 2026-10-28).
    // UpdateHoster, CastBondProc and PlayTeleportVanish skip such listeners
    // the same way. The sender's own actor is skipped as before.
    if (listener == &sender ||
        partOne.serverState.UserByActor(listener) ==
          Networking::InvalidUserId) {
      continue;
    }
    listener->SendToUser(relay, true);
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
  // thuum docs/verbs/npc-fate.md ("The body's rest", ADR-067): the report is
  // a dead NPC's, from the body's owner, inside its settle window
  bool bodySample = false;
  const uint64_t nowMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
  {
    // thuum docs/verbs/cell-host.md, the cell-change rule: a hosted actor's
    // report is its cell's host's; one naming another cell is accepted when
    // the sender hosts the destination and the cell it leaves is unhosted or
    // the sender's too (npc-handoff's row otherwise: dropped, logged)
    MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);
    MpForm* form = partOne.worldState.LookupFormByIdx(msg.idx);
    MpActor* moved = form ? form->AsActor() : nullptr;
    // thuum ADR-068 rung 4 (WorldGlobal 80): a player's own movement report
    // (its first, past the login's load block) makes its seat eligible to
    // stream the world's globals
    if (myActor && moved == myActor) {
      partOne.WorldStreamerMoved(*myActor);
    }
    if (myActor && moved && moved != myActor &&
        partOne.serverState.UserByActor(moved) == Networking::InvalidUserId) {
      const NiPoint3 destPos{ msg.data.pos[0], msg.data.pos[1],
                              msg.data.pos[2] };
      const auto destCell =
        partOne.worldState.CellOf(msg.data.worldOrCell, destPos);
      // thuum docs/verbs/npc-fate.md ("The body's rest", ADR-067 decision
      // 4): a dead NPC's movement report is a BODY sample, never a
      // stand-up: the message's healthPercentage and isDead are not read
      // on this path, and a stand-up is OnHostedActorState's alone, from
      // the health the host samples. The corpse is a physics object under
      // the prop rule: its owner, the engine that simulated the actor at
      // its death, moves the record with its ragdoll for the settle window
      // (WorldState::bodySettleMs) and the record keeps the pose the
      // ragdoll rests at (its samples still for the dwell), so a watcher
      // places its corpse there and the loot reach measures from it
      // (playtest 42's F4 and F6: a watcher's body 1,700 units down Bleak
      // Falls' slope while the record kept the death point). Every other
      // seat's report is refused, and once at rest so is the owner's, until
      // a later claim (a drag or a push: the prop claim, not this slice).
      // R2 (rule 5): the owner's ragdoll is recorded, not judged, inside
      // the movement bounds below. Each refusal is said once a minute per
      // sender and body, the rest counted. A body dead since before this
      // process or placed dead has no window here and keeps the host's
      // samples as before (TODO: its rest at load, with the prop claim)
      if (moved->IsDead() && moved->IsFateRespawned() &&
          moved->HasBodySettle()) {
        const auto refuseBody = [&](const char* code, const std::string& why) {
          // the throttle per code, sender and body: a NOT_OWNER line must
          // not silence an AT_REST line seconds later, nor mix their counts
          static std::map<
            std::tuple<std::string, uint32_t, uint32_t>,
            std::pair<std::chrono::steady_clock::time_point, uint32_t>>
            refusals;
          auto& [since, count] = refusals[{
            std::string(code), myActor->GetFormId(), moved->GetFormId() }];
          const auto now = std::chrono::steady_clock::now();
          ++count;
          if (since.time_since_epoch().count() == 0 ||
              now - since >= std::chrono::minutes(1)) {
            spdlog::warn("[verb:npc-fate] {}: {:x} moves body {:x} to "
                         "({:.0f}, {:.0f}, {:.0f}); {}; dropped, {} "
                         "refusal(s) since the last line",
                         code, myActor->GetFormId(), moved->GetFormId(),
                         destPos.x, destPos.y, destPos.z, why, count);
            since = now;
            count = 0;
          }
          // a foreign refusal in the stream counters, as ActorUpdatableBy's
          // "not the host" is
          partOne.serverState.CountStream(
            rawMsgData.userId, static_cast<uint8_t>(MsgType::UpdateMovement),
            false, true);
        };
        // the window may have ended since the last sweep
        moved->SettleBodyIfDue(nowMs);
        if (moved->IsBodyAtRest()) {
          const auto& rest = moved->GetPos();
          refuseBody(
            "E_BODY_AT_REST",
            fmt::format("the body is at rest at ({:.0f}, {:.0f}, {:.0f}) "
                        "since {} ms; a later claim (a drag, a push: the "
                        "prop claim) wakes it",
                        rest.x, rest.y, rest.z,
                        nowMs >= moved->BodyRestAtMs()
                          ? nowMs - moved->BodyRestAtMs()
                          : 0u));
          return;
        }
        if (moved->BodyOwnerId() == 0 &&
            partOne.SimulatorOf(*moved) == myActor->GetFormId()) {
          moved->ClaimBodyOwner(myActor->GetFormId());
        }
        if (moved->BodyOwnerId() != myActor->GetFormId()) {
          refuseBody("E_BODY_NOT_OWNER",
                     moved->BodyOwnerId()
                       ? fmt::format("{:x} owns the body (the engine that "
                                     "simulated it at its death)",
                                     moved->BodyOwnerId())
                       : std::string("nobody simulated it at its death and "
                                     "the sender does not simulate it now"));
          return;
        }
        bodySample = true;
      }
      auto verdict =
        partOne.MayMoveActor(*myActor, *moved, msg.data.worldOrCell, destPos);
      if (verdict == PartOne::MoveVerdict::Refuse && bodySample &&
          destCell.has_value() &&
          destCell == partOne.worldState.CellOf(*moved)) {
        // the body's owner inside the record's cell: the cell-host rule
        // stands aside for the ragdoll's owner within the window (ADR-063,
        // ownership, not permission; a host change mid-ragdoll leaves the
        // body with its mover); a roll across a cell border keeps the
        // cell-change rule
        verdict = PartOne::MoveVerdict::Accept;
      }
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

    // thuum docs/verbs/npc-fate.md ("The body's rest", ADR-067): the sample
    // that puts a body at rest is the record's final one, so it goes to
    // the other games reliably, as a prop's does at sleep (ADR-058); the
    // ragdoll's samples before it ride the unreliable relay as any step
    const NiPoint3 reportedPos{ msg.data.pos[0], msg.data.pos[1],
                                msg.data.pos[2] };
    const NiPoint3 reportedRot{ msg.data.rot[0], msg.data.rot[1],
                                msg.data.rot[2] };
    const bool bodyRests =
      bodySample && actor->BodySampleRests(reportedPos, reportedRot, nowMs);
    RelayToListeners(*actor, rawMsgData.unparsed, rawMsgData.unparsedLength,
                     bodyRests, rawMsgData.userId);

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
    // npc-handoff (d) through a door (ADR-047's note of 2026-10-28): a pass
    // into a cell whose grid the sender does not share (an interior from
    // outside, or the way back) unsubscribes the sender at the move below,
    // so its word goes first, while it still holds the view: its engine
    // demotes the copy it walked through instead of running it on indoors
    if (passed && hostedCellChange) {
      if (MpActor* sender =
            partOne.serverState.ActorByUser(rawMsgData.userId)) {
        actor->TellHostedByOther(*sender);
      }
    }
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
      // thuum docs/verbs/npc-handoff.md D9's settle (playtest 41, 2026-11-03,
      // Hod 0x1348a at the Sleeping Giant's door): the passing report itself
      // goes to the new host's game reliably, ahead of the word on the same
      // channel, so its promote places the copy at the door's far side (the
      // puppet's last sample, puppetService) and not where the copy stood;
      // the unreliable relay of the same report may land after the word, on
      // a live actor that ignores it, or not at all
      const auto newHostUser = newHost
        ? partOne.serverState.UserByActor(newHost)
        : Networking::InvalidUserId;
      if (newHostUser != Networking::InvalidUserId && rawMsgData.unparsed &&
          rawMsgData.unparsedLength > 0) {
        partOne.GetSendTarget().Send(newHostUser, rawMsgData.unparsed,
                                     rawMsgData.unparsedLength, true);
      }
      actor->UpdateHoster(newHost ? newHost->GetFormId() : 0);
      spdlog::info("ActionListener::OnUpdateMovement - npc-handoff D9: {:x} "
                   "now simulates {:x} in cell {:x} from the passing report, "
                   "sent to it reliably ahead of the word; the sender's game "
                   "is told to demote",
                   newHost ? newHost->GetFormId() : 0u, actor->GetFormId(),
                   partOne.worldState.CellOf(*actor).value_or(0));
    }
    actor->SetAngle(
      NiPoint3{ msg.data.rot[0], msg.data.rot[1], msg.data.rot[2] },
      SetAngleMode::CalledByUpdateMovement);
    // the body's sample, after the record took it: the rest, when the
    // samples held still for the dwell, saves the record at once and ends
    // the window
    if (bodySample) {
      actor->NoteBodySample(reportedPos, reportedRot, nowMs);
    }
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
  // thuum docs/verbs/npc-stream.md (playtest 35, Hilde's tanning missing on
  // a seat that joined two minutes into it): a hosted NPC's last action is
  // kept on the record (R2, memory), and its CreateActor carries it to a
  // late joiner, whose puppet replays it once
  if (actor != myActor) {
    actor->SetLastAction(msg);
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
  const float onePoint = 1.f / std::max(actor->GetMaximumValues().health, 1.f);
  // thuum docs/verbs/npc-fate.md (playtest 37's finding 48, 2026-10-29: a
  // necromancer's Reanimate stood a dead skeever up on its host's engine,
  // where it fought, while the record and the other seat kept the body): a
  // hosted NPC the record holds dead whose host samples it alive, above one
  // point of health, has been resurrected by its host's engine, which runs
  // the spell that did it; the server records the result (R2, rule 5): the
  // death cleared through the artificial respawn (MpActor::Respawn without a
  // teleport: the inventory kept, the body's day and its reset due ended),
  // the live word to every listener (RespawnEvent's isDead false, the
  // puppets' Resurrect), then this sample judged as any live one. Guards: an
  // NPC (a player's death is the server's own numbers), and the death older
  // than MpActor::kPendingDeathMs on the server's clock: a sample the host
  // sent before the server's kill word reached it is in flight for a round
  // trip, not a resurrection. A death this process never stamped (DiedAgo
  // nothing: RecordDeathAt runs at SendAndSetDeathState alone and a body
  // loaded dead gets none; playtest 37's finding 54, the body dead across a
  // restart that could never be stood up) has no kill word of this process
  // in flight, so it counts as past the grace
  if (actor->IsDead() && actor->IsFateRespawned() && msg.health > onePoint) {
    // thuum docs/verbs/npc-fate.md (finding 65): a placement that starts
    // dead has no death of this process to grace (DiedAgo nothing) and its
    // host's engine spawned it as a corpse, so one live sample must not
    // stand it (the train's review, 2026-10-29: the no-stamp reading above
    // took the first sample above one point as a resurrection). Its
    // stand-up is the cast's rule alone: a reanimate hit the server accepted
    // on it within MpActor::kReanimateWindowMs (NoteReanimateAccepted at
    // OnHit, ahead of ClaimReanimated). Any other live sample of it
    // stays a sample of a dead record, judged dead below; said once in ten
    // seconds per body, at debug
    const auto reanimatedAgo = actor->ReanimateAcceptedAgo(now);
    const bool reanimated = reanimatedAgo &&
      reanimatedAgo->count() <=
        static_cast<int64_t>(MpActor::kReanimateWindowMs);
    const auto stampGap = actor->DeadStampGap(myActor->GetFormId(),
                                              myActor->LoginGeneration(), now);
    if (!actor->IsPlacedDead() && !reanimated && stampGap) {
      // thuum docs/verbs/npc-fate.md (playtest 40's F9, and
      // x-npc-death-reentry v4's run 20261103-100946): a host's engine that
      // has not seen the death loads the body alive (a corpse's loading frame
      // before the kill word lands; a fresh process after a relaunch; a later
      // joiner that hosts), and its first live sample read here as a
      // resurrection ("35534 stands again with health 1.000 on ff000000's
      // sample, 171 s after its death"). A live sample stands a body only
      // after ITS HOST has sampled it dead once (the engine's own respawn or a
      // reanimation follows a corpse), or within a reanimate's window; the
      // stamp names its WITNESS (the host's id and login generation) and its
      // time (MpActor::DeadStampGap), so a relaunch, a reconnect or
      // another host is a new witness and a bleedout's old zero is not a
      // death's. Said once in ten seconds per body, at info: the lab reads it
      static std::unordered_map<uint32_t,
                                std::chrono::steady_clock::time_point>
        unstampedLines;
      auto& last = unstampedLines[actor->GetFormId()];
      if (last == std::chrono::steady_clock::time_point{} ||
          now - last >= std::chrono::seconds(10)) {
        last = now;
        spdlog::info("[verb:npc-fate] {:x}'s live sample (health {:.3f}) of "
                     "{:x} stands nothing: {}; the record stays dead",
                     myActor->GetFormId(), msg.health, actor->GetFormId(),
                     *stampGap);
      }
    } else if (actor->IsPlacedDead() && !reanimated) {
      static std::unordered_map<uint32_t,
                                std::chrono::steady_clock::time_point>
        placedLines;
      auto& last = placedLines[actor->GetFormId()];
      if (last == std::chrono::steady_clock::time_point{} ||
          now - last >= std::chrono::seconds(10)) {
        last = now;
        spdlog::debug("[verb:npc-fate] {:x} is a placed corpse: {:x}'s live "
                      "sample (health {:.3f}) stands nothing; a reanimation "
                      "is the cast's rule (finding 65)",
                      actor->GetFormId(), myActor->GetFormId(), msg.health);
      }
    } else {
      const auto diedAgo = actor->DiedAgo(now);
      if (!diedAgo ||
          diedAgo->count() >= static_cast<int64_t>(MpActor::kPendingDeathMs)) {
        spdlog::info(
          "[verb:npc-fate] resurrection by its host's engine: {:x} "
          "stands again with health {:.3f} on {:x}'s sample, {}; "
          "the record's death cleared, the live word to every "
          "listener",
          actor->GetFormId(), msg.health, myActor->GetFormId(),
          diedAgo
            ? fmt::format("{} s after its death", diedAgo->count() / 1000)
            : actor->IsPlacedDead()
            ? fmt::format("a placed corpse, the reanimate accepted {} ms ago",
                          reanimatedAgo->count())
            : std::string("its death before this process"));
        actor->ClearHostSampledDead();
        actor->NoteHostStoodUp(now);
        actor->Respawn(false);
      }
    }
  } else if (actor->IsFateRespawned() && msg.health <= onePoint) {
    // the host has this body down (playtest 40's F9): a later live sample of
    // its may stand it. The record may still be alive here: under ADR-050's
    // order the engine's death comes first and the host's zero reaches the
    // record before the server's word, floored, and that zero is the host's
    // stamp too, so a later stand-up with no claimed hit (a script's
    // Resurrect, the console) is not refused (numidium's review of round
    // six, item 3)
    actor->NoteHostSampledDead(myActor->GetFormId(),
                               myActor->LoginGeneration(), now);
  }
  facts.alive = !actor->IsDead() && !actor->IsRespawning();
  facts.one_point = onePoint;

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
    // the permit: a restricted race passes for a listed profile alone; the
    // profile is the login's, never the client's word
    const auto restricted = partOne.worldState.RestrictedRaceIds();
    facts.is_restricted = std::find(restricted.begin(), restricted.end(),
                                    raceId) != restricted.end();
    const auto permitted =
      partOne.worldState.PermittedRaceIds(actor.GetProfileId());
    facts.is_permitted =
      std::find(permitted.begin(), permitted.end(), raceId) != permitted.end();
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
  return partOne.LookFormIdOf(std::string(ref.plugin), ref.id);
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
// whose own game already wears it; with the race overlay it wears beyond its
// record, if any (thuum ADR-068 rung 5, D-T1 A: the figure's look and its
// overlay travel together, an UpdateAppearance without one ending it)
void SendAppearanceToOthers(PartOne& partOne, MpActor& actor,
                            const Appearance& appearance)
{
  UpdateAppearanceMessage out;
  out.idx = actor.GetIdx();
  out.data = appearance;
  out.overlay = partOne.PlayerOverlayOf(actor.GetFormId());
  for (auto listener : actor.GetActorListeners()) {
    if (listener == &actor ||
        partOne.serverState.UserByActor(listener) ==
          Networking::InvalidUserId) {
      continue;
    }
    listener->SendToUser(out, true);
  }
}

// the player's stored RaceMenu look to everyone who sees it but the player,
// whose own game wears it already (its race menu's close sent it, or its
// script's own ChangeHeadPart ran there); how many games heard it
size_t SendRaceMenuPresetToOthers(PartOne& partOne, MpActor& actor)
{
  RaceMenuPresetMessage out;
  out.actor = actor.GetFormId();
  out.preset = actor.GetRaceMenuPreset();
  size_t sent = 0;
  for (auto listener : actor.GetActorListeners()) {
    if (listener == &actor ||
        partOne.serverState.UserByActor(listener) ==
          Networking::InvalidUserId) {
      continue;
    }
    listener->SendToUser(out, true);
    ++sent;
  }
  return sent;
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
                 "{:x} chose race {:x}, which the race menu does not offer "
                 "or profile {} may not wear; refused",
                 actor->GetFormId(), msg.data->raceId, actor->GetProfileId());
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
    } else if (partOne.PlayerOverlayOf(actor->GetFormId())) {
      // thuum ADR-068 rung 5 (D-T1 A): the seat's look relayed as is would
      // drop the figure's overlay on every other screen; the server's word
      // carries both
      actor->SetAppearance(&msg.data.value());
      SendAppearanceToOthers(partOne, *actor, msg.data.value());
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

  // thuum docs/verbs/marksman.md (playtest 37's finding 45, 2026-10-29): a
  // player's report with one entry the server cannot take (an item its
  // inventory lacks, a spell or a shout it has not learned, armor sharing a
  // slot) was refused whole, so the hands it also carried (the bow just
  // drawn) never reached the record and every shot from them was refused
  // (E_SHOT_EQUIPMENT, 32 arrows that did nothing). Each such entry is now
  // dropped on its own, with the correction a refusal always sent (the item
  // unequipped, the spell removed, the inventory re-sent), and the rest
  // stands as the record. A hosted actor's report keeps npc-stream's shape
  // (slice 3): one entry the record cannot take refuses it whole, and the
  // host hears nothing (Rule A)
  const auto actorFormId = actor->GetFormId();
  const Equipment& data = msg.data;
  // the report less the entries dropped below
  Equipment kept = data;
  bool dropped = false;
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

  // thuum ADR-068 rung 5 (D-T1 A): what a player's race overlay holds (the
  // beast form's level abilities, its howl, WolfSkinFXArmor) passes the gate
  // as its record's would, and is relayed as reported, but never reaches the
  // change form (the fresh review's note 5: recorded below without it); a
  // hosted actor has none
  const auto overlay = hosted ? std::optional<RaceOverlay>()
                              : partOne.PlayerOverlayOf(actor->GetFormId());
  const auto inOverlay = [&](const std::vector<uint32_t> RaceOverlay::* list,
                             uint32_t formId) {
    return overlay &&
      std::find(((*overlay).*list).begin(), ((*overlay).*list).end(),
                formId) != ((*overlay).*list).end();
  };
  const auto learned = [&](uint32_t spellId) {
    return actor->IsSpellLearned(spellId) ||
      inOverlay(&RaceOverlay::spells, spellId);
  };

  // thuum docs/verbs/npc-stream.md (ADR-050, train 53): the known set is a
  // player's (the learned spells, the base's and its race's lists, the
  // template's where the chain is evaluated); a hosted actor's hands are its
  // host's engine's, which draws on templates and leveled spell lists the
  // server does not walk, so a spell or a shout the set lacks in a hosted
  // report is recorded as unvalidated (R2, rule 5) with one line, when the
  // form is a spell or a shout at all; anything else refuses the report
  auto recordedUnknown = [&](const char* slot, uint32_t formId,
                             bool shout) -> bool {
    if (!hosted || !partOne.HasEspm()) {
      return false;
    }
    const auto rec = partOne.GetEspm().GetBrowser().LookupById(formId).rec;
    const bool spellForm = rec &&
      (rec->GetType() == espm::SPEL::kType ||
       (shout && rec->GetType() == espm::SHOU::kType));
    if (!spellForm) {
      return false;
    }
    spdlog::info("[verb:npc-stream] E_EQUIP_UNKNOWN_SPELL: {:x}'s {} slot by "
                 "{:x} holds {} {:x}, which its record's known set lacks; "
                 "recorded unvalidated (the host's engine's list)",
                 actorFormId, slot, myActor->GetFormId(),
                 shout ? "shout" : "spell", formId);
    return true;
  };

  if (leftSpell > 0 && !learned(leftSpell) &&
      !recordedUnknown("left", leftSpell, false)) {
    spdlog::warn("ActionListener::OnUpdateEquipment {:x} - dropped from the "
                 "equipment update: spell {:x} is not learned",
                 actorFormId, leftSpell);
    kept.leftSpell.reset();
    dropped = true;
    spellIdsToRemove[static_cast<size_t>(SpellSlotId::Left)] = leftSpell;
  }

  if (rightSpell > 0 && !learned(rightSpell) &&
      !recordedUnknown("right", rightSpell, false)) {
    spdlog::warn("ActionListener::OnUpdateEquipment {:x} - dropped from the "
                 "equipment update: spell {:x} is not learned",
                 actorFormId, rightSpell);
    kept.rightSpell.reset();
    dropped = true;
    spellIdsToRemove[static_cast<size_t>(SpellSlotId::Right)] = rightSpell;
  }

  // thuum docs/verbs/shouts.md: the voice slot holds a power or a shout; a
  // shout is the player's when the server's record has it (the console's
  // AddShout for an admin), and one it lacks leaves the player's game
  const bool voiceIsShout = VoiceShout(data, partOne.worldState).has_value();
  if (voiceSpell > 0 &&
      !(voiceIsShout
          ? (actor->HasShoutEntry(MpActor::ShoutEntry::Shout, voiceSpell) ||
             inOverlay(&RaceOverlay::shouts, voiceSpell))
          : learned(voiceSpell)) &&
      !recordedUnknown("voice", voiceSpell, voiceIsShout)) {
    spdlog::warn("ActionListener::OnUpdateEquipment {:x} - dropped from the "
                 "equipment update: {} {:x} is not learned",
                 actorFormId, voiceIsShout ? "shout" : "spell", voiceSpell);
    kept.voiceSpell.reset();
    dropped = true;
    spellIdsToRemove[static_cast<size_t>(SpellSlotId::Voice)] = voiceSpell;
  }

  if (instantSpell > 0 && !learned(instantSpell) &&
      !recordedUnknown("instant", instantSpell, false)) {
    spdlog::warn("ActionListener::OnUpdateEquipment {:x} - dropped from the "
                 "equipment update: spell {:x} is not learned",
                 actorFormId, instantSpell);
    kept.instantSpell.reset();
    dropped = true;
    spellIdsToRemove[static_cast<size_t>(SpellSlotId::Instant)] = instantSpell;
  }

  std::vector<uint32_t> itemIdsToUnequip;

  // thuum docs/verbs/npc-fate.md (playtest 37, 2026-10-28, findings 43 and
  // 46): a hosted actor's engine wields what its own inventory roll gave it
  // when the fate reached it late (an interior's actor, a login's or a
  // teleport's neighbours): the late path redresses the outfit from the
  // server's list and leaves the engine's rolled weapon beside the server's
  // (FateApi.cpp RedressOnGameThread; re-npc-roll.md section 11, the dresser
  // picks by score), so the host's hands report named a weapon the record's
  // inventory lacked (39fe4's steel war axe, 39fe2's hunting bow), the whole
  // report was refused and every swing fell at E_HIT_WEAPON. A worn WEAP the
  // record lacks is the engine's own roll: recorded, R2 (rule 5), the item
  // added to the record's inventory (the body holds what it swung) with one
  // line, so the record's hands hold what the engine wields and the hit gate
  // stands as it is. Anything else the record lacks refuses the report as
  // before
  std::vector<uint32_t> engineWeapons;
  const auto recordTypeIs = [&](uint32_t formId, const char* type) -> bool {
    if (!partOne.HasEspm()) {
      return false;
    }
    const auto rec = partOne.GetEspm().GetBrowser().LookupById(formId).rec;
    return rec && rec->GetType() == type;
  };
  // thuum docs/verbs/npc-fate.md (Eli's word of 2026-11-04 after oracle v4
  // 190713's Torch01): a hosted actor's HANDS are its host's engine's, a
  // weapon or a light its AI took up (a torch lit in a dark hall), recorded
  // R2 as reported; its DRESS is the server's roll, the armor the fate
  // dresses every copy from
  const auto isHandRecord = [&](uint32_t formId) -> bool {
    return recordTypeIs(formId, espm::WEAP::kType) ||
      recordTypeIs(formId, espm::LIGH::kType);
  };
  const auto isArmorRecord = [&](uint32_t formId) -> bool {
    return recordTypeIs(formId, espm::ARMO::kType);
  };

  const auto& inventory = actor->GetInventory();
  {
    auto& entries = kept.inv.entries;
    for (auto it = entries.begin(); it != entries.end();) {
      // a hosted actor's report names what it wears; its engine's whole
      // inventory is npc-fate's record, not this message's
      if ((hosted && it->GetWorn() == Inventory::Worn::None) ||
          inventory.HasItem(it->baseId) ||
          inOverlay(&RaceOverlay::items, it->baseId)) {
        ++it;
        continue;
      }
      if (hosted && isHandRecord(it->baseId)) {
        // the engine's own roll in a hosted actor's hands (findings 43 and
        // 46), a light its AI lit the same: the entry stays in the report
        // and is recorded below
        engineWeapons.push_back(it->baseId);
        ++it;
        continue;
      }
      spdlog::warn("ActionListener::OnUpdateEquipment {:x} - dropped from the "
                   "equipment update: inventory does not contain item {:x}",
                   actorFormId, it->baseId);
      if (it->GetWorn() != Inventory::Worn::None) {
        itemIdsToUnequip.push_back(it->baseId);
      }
      it = entries.erase(it);
      dropped = true;
    }
  }

  if (auto worldState = actor->GetParent()) {
    uint32_t occupiedSlots = 0;
    // Track which item owns each bit so we can report conflicts
    std::array<uint32_t, 32> slotOwner = {};
    // the items sharing a slot, if any: they leave the report together and
    // the player's game is told to take them off (as the refusal told it)
    std::unordered_set<uint32_t> conflictingItems;
    for (auto& entry : kept.inv.entries) {
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
          "ActionListener::OnUpdateEquipment {:x} - dropped from the "
          "equipment update: items [{}] share armor slot flags {:x} (0b{})",
          actorFormId, conflictList, overlap, binaryStr);
        break;
      }
      for (int bit = 0; bit < 32; ++bit) {
        if (bodyPartFlags & (1u << bit)) {
          slotOwner[bit] = entry.baseId;
        }
      }
      occupiedSlots |= bodyPartFlags;
    }
    if (!conflictingItems.empty()) {
      auto& entries = kept.inv.entries;
      entries.erase(std::remove_if(entries.begin(), entries.end(),
                                   [&](const Inventory::Entry& entry) {
                                     return conflictingItems.count(
                                              entry.baseId) > 0;
                                   }),
                    entries.end());
      for (uint32_t id : conflictingItems) {
        itemIdsToUnequip.push_back(id);
      }
      dropped = true;
    }
  }

  if (dropped && hosted) {
    // the refusal as npc-stream's slice 3 has it: the record stands, nothing
    // is sent to the host (SetInventory has no idx and would replace the
    // host player's own inventory, a snippet on an NPC runs nowhere), one
    // warn a minute per actor
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
    UpdateEquipmentAttemptEvent refusedEvent(actor, data, false);
    refusedEvent.Fire(actor->GetParent());
    return;
  }

  // what stands is the record and the neighbours' picture: the bytes as
  // they came when nothing was dropped (SendToNeighbours), else the kept
  // report to the same audience (RelayToListeners': every listener with a
  // user, the player's own echo kept)
  if (dropped) {
    UpdateEquipmentMessage relay;
    relay.idx = msg.idx;
    relay.data = kept;
    for (auto listener : actor->GetActorListeners()) {
      if (listener->GetUserId() == Networking::InvalidUserId) {
        continue;
      }
      listener->SendToUser(relay, true);
    }
  } else {
    SendToNeighbours(msg.idx, rawMsgData, true);
  }
  if (hosted) {
    for (const auto weaponId : engineWeapons) {
      // the add's SetInventory goes to the actor's own user, which a hosted
      // actor has none of (PartOne's sendToUserDeferred drops it); nothing
      // reaches the host's game
      actor->AddItem(weaponId, 1);
      spdlog::info("[verb:npc-stream] E_EQUIP_UNKNOWN_WEAPON: {:x}'s hands by "
                   "{:x} hold {:x} (a weapon or a light), which its record's "
                   "inventory lacks "
                   "(the engine's own roll); recorded unvalidated, the item "
                   "added to the record",
                   actorFormId, myActor->GetFormId(), weaponId);
    }
    // finding 46 (the naked bandit): the record's worn set is the fate's
    // worn list (PartOne's FateOf), the outfit every seat's engine dresses
    // the copy from, and this report replaced it whole: a copy polled before
    // its dressing (nothing worn yet) or one already undressed reported no
    // outfit, the record kept none, and the next fate redressed the copy
    // naked on every seat (29600, "1 worn, right 1397e left 0" accepted).
    // The outfit is the server's roll (R0): a worn ARMOR entry of the record
    // that the report lacks stays; everything else worn (a weapon, a light,
    // the ammo) is the host's engine's hands and follows the report, so a
    // torch the AI put away leaves the record's hands as it left the host's
    // (oracle v4 190713 kept Torch01 worn in the record 25 s after the
    // re-roll while the host's actor held nothing)
    size_t keptOutfit = 0;
    for (const auto& entry : actor->GetEquipment().inv.entries) {
      if (entry.GetWorn() == Inventory::Worn::None ||
          kept.inv.HasItem(entry.baseId) || !isArmorRecord(entry.baseId)) {
        continue;
      }
      kept.inv.AddItems({ entry });
      ++keptOutfit;
    }
    if (keptOutfit > 0) {
      spdlog::info("[verb:npc-stream] outfit kept for {:x}: {} worn item(s) "
                   "of the server's roll the host's report lacked (a copy "
                   "before its dressing, or undressed)",
                   actorFormId, keptOutfit);
    }
  }
  // D-T1 A (the fresh review's note 5): an entry that passed by the overlay
  // alone is the figure's, relayed above as reported, and never the
  // record's, which nothing would strip at the overlay's end
  Equipment recorded = kept;
  if (overlay) {
    const auto overlaySpell = [&](std::optional<uint32_t>& slot) {
      if (slot && !actor->IsSpellLearned(*slot) &&
          inOverlay(&RaceOverlay::spells, *slot)) {
        slot.reset();
      }
    };
    overlaySpell(recorded.leftSpell);
    overlaySpell(recorded.rightSpell);
    overlaySpell(recorded.instantSpell);
    if (recorded.voiceSpell && voiceIsShout) {
      if (!actor->HasShoutEntry(MpActor::ShoutEntry::Shout,
                                *recorded.voiceSpell) &&
          inOverlay(&RaceOverlay::shouts, *recorded.voiceSpell)) {
        recorded.voiceSpell.reset();
      }
    } else {
      overlaySpell(recorded.voiceSpell);
    }
    auto& entries = recorded.inv.entries;
    entries.erase(std::remove_if(entries.begin(), entries.end(),
                                 [&](const Inventory::Entry& entry) {
                                   return !inventory.HasItem(entry.baseId) &&
                                     inOverlay(&RaceOverlay::items,
                                               entry.baseId);
                                 }),
                  entries.end());
  }
  actor->SetEquipment(recorded);
  if (hosted) {
    uint32_t right = 0;
    uint32_t left = 0;
    size_t worn = 0;
    for (auto& entry : kept.inv.entries) {
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

  if (dropped) {
    // the corrections a refusal sent, now for the dropped entries alone:
    // the server's inventory again, each spell removed, each item taken off
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

    for (uint32_t itemId : itemIdsToUnequip) {
      spdlog::info("ActionListener::OnUpdateEquipment {:x} - unequipping item "
                   "{:x} (dropped from the equipment update, unauthorized "
                   "change)",
                   actorFormId, itemId);
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

  UpdateEquipmentAttemptEvent updateEquipmentAttemptEvent(actor, kept, true);
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

// thuum ADR-068 D-W1 (docs/research/fifty-fixes-2026-11-07.md section 1(c),
// wire-rules trigger_entry's firing_source): the facts of a trap trigger's
// firing `reporter`'s game reports, judged in `cell`: its host, then, for a
// reporter that is not the host, whether its game has the cell loaded and
// how far the nearest of its actors stands from `trigger` (-1 for none, and
// for a trigger the server never loads). Its actors are its own player and
// the actors the world names it the simulator of (WorldState::HostOf) among
// the trigger's listeners (the actors of the grid squares around it). One
// inside the trigger's primitive stands at it, the engine's own condition
// for a trigger volume as OnWordWall reads it (a trigger box's bounds are
// its primitive, which its base's bounds do not give); any other is measured
// from the trigger's position.
skymp::rules::TriggerFiringFacts GatherFiringFacts(PartOne& partOne,
                                                   MpActor& reporter,
                                                   MpObjectReference* trigger,
                                                   uint32_t cell)
{
  skymp::rules::TriggerFiringFacts facts{};
  facts.reporter = reporter.GetFormId();
  facts.cell = cell;
  const auto host = partOne.GetCellHost(cell);
  facts.host = host.hosted ? host.player : 0;
  facts.nearest = -1.f;
  if (facts.host != 0 && facts.host == facts.reporter) {
    // the host's word is taken first: nothing more to gather
    return facts;
  }
  const auto loaders = partOne.GetCellLoaders(cell);
  facts.loader = std::find(loaders.begin(), loaders.end(),
                           reporter.GetFormId()) != loaders.end();
  if (!trigger) {
    return facts;
  }
  facts.trigger_size = ActivationTargetSize(*trigger);
  const auto measure = [&](const MpActor& actor) {
    if (actor.GetCellOrWorld() != trigger->GetCellOrWorld()) {
      return;
    }
    const float distance = trigger->HasPrimitive() &&
        trigger->IsPointInsidePrimitive(actor.GetPos())
      ? 0.f
      : (actor.GetPos() - trigger->GetPos()).Length();
    if (facts.nearest < 0.f || distance < facts.nearest) {
      facts.nearest = distance;
    }
  };
  measure(reporter);
  for (MpActor* listener : trigger->GetActorListeners()) {
    if (!listener || listener == &reporter ||
        partOne.serverState.UserByActor(listener) !=
          Networking::InvalidUserId) {
      // another seat's player is that seat's
      continue;
    }
    if (partOne.worldState.HostOf(*listener) == &reporter) {
      measure(*listener);
    }
  }
  return facts;
}

// thuum docs/verbs/containers.md (M4's row, "a distance check on PutItem and
// TakeItem"): a move in or out of a container is taken from a player in the
// container's cell or world and within the activation reach of it (the rule
// an open passes, wire-rules activation); the occupant set is pruned at
// opens alone, so a player who walked off with the menu open, or a forged
// message, could otherwise move items across the cell (R1). A refusal
// resyncs the mover's and the container's inventories, as a refused move
// does (memo D52), since the engine moved the item locally already.
bool ContainerMoveWithinReach(MpActor& actor, MpObjectReference& container,
                              const char* what)
{
  if (actor.GetCellOrWorld() != container.GetCellOrWorld()) {
    spdlog::warn("[verb:containers] E_CONTAINER_CELL: {:x} {}s {:x} from "
                 "another cell or world; refused",
                 actor.GetFormId(), what, container.GetFormId());
    container.ResyncAfterRefusedMove(actor);
    return false;
  }
  const float distance = (actor.GetPos() - container.GetPos()).Length();
  const auto verdict = skymp::rules::activation_within_reach(
    distance, ActivationTargetSize(container));
  if (!verdict.allowed) {
    spdlog::warn("[verb:containers] E_CONTAINER_REACH: {:x} {}s {:x} from "
                 "{:.0f} units, reach {:.0f}; refused",
                 actor.GetFormId(), what, container.GetFormId(), distance,
                 verdict.bound);
    container.ResyncAfterRefusedMove(actor);
    return false;
  }
  return true;
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

  // thuum D-C2 (SCHEMA_VERSION 40): an Activate carrying `entry` is a
  // trigger entry, never an activation; its own path, before every branch
  // below (the edge checked its shape, wire-validate check_activate)
  if (msg.data.entry) {
    OnTriggerEntry(*ac, msg);
    return;
  }

  // thuum docs/verbs/npc-fate.md (ADR-048's B, the hazard evidence): a
  // reference's activation of itself is a trap trigger's firing (every
  // vanilla trigger script activates itself, and the engine forwards it to
  // the trap with the plate as the activator; the stepping actor is never
  // the action reference, so no activation names the victim). The firing's
  // owner reports it (thuum ADR-068 D-W1: the seat that owns the trigger's
  // presser, judged by wire-rules trigger_entry's firing_source: the cell's
  // host, or a game with the cell loaded one of whose actors stands by the
  // trigger); it is noted where and when as a hazard death's evidence (R2)
  // and nothing is activated here.
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
    const auto firing = skymp::rules::trigger_firing_source(
      GatherFiringFacts(partOne, *ac, trigger.get(), *cell));
    if (firing.code != skymp::rules::TriggerFiringCode::Host &&
        firing.code != skymp::rules::TriggerFiringCode::Loader) {
      spdlog::info("[verb:npc-fate] {}: {:x} reports trigger {:x} firing "
                   "itself in cell {:x}, {}; dropped",
                   std::string(firing.name), ac->GetFormId(),
                   trigger->GetFormId(), *cell, std::string(firing.why));
      return;
    }
    NoteTrapFiring(*trigger, std::chrono::steady_clock::now());
    if (firing.code == skymp::rules::TriggerFiringCode::Loader) {
      spdlog::info("[verb:npc-fate] trigger {:x} fired itself in cell {:x} "
                   "({:x} reports with the cell loaded, its firing): noted as "
                   "a hazard death's evidence",
                   trigger->GetFormId(), *cell, ac->GetFormId());
      return;
    }
    spdlog::info("[verb:npc-fate] trigger {:x} fired itself in cell {:x} (its "
                 "host {:x} reports): noted as a hazard death's evidence",
                 trigger->GetFormId(), *cell, ac->GetFormId());
    return;
  }

  // thuum docs/verbs/traps.md (M4): a reference that is no actor activating
  // another (a plate its spike wall, a lever its portcullis) is a linked
  // activation the cell's host reports; its own path, before the hosted
  // caster's rule below throws on it
  if (msg.data.caster != 0x14) {
    const auto casterId = static_cast<uint32_t>(msg.data.caster);
    const auto& casterForm = partOne.worldState.LookupFormById(casterId);
    MpObjectReference* casterRef =
      casterForm ? casterForm->AsObjectReference() : nullptr;
    if (casterRef && !casterRef->AsActor()) {
      OnLinkedActivation(rawMsgData, *ac, casterId, casterRef,
                         static_cast<uint32_t>(msg.data.target));
      return;
    }
    // ADR-059 decision 2: a played actor other than the reporter's own as the
    // caster is the host's engine echoing the relay below (the guest's figure
    // activated the object there); nothing more to do with it
    if (casterRef && casterRef->AsActor() &&
        partOne.serverState.UserByActor(casterRef->AsActor()) !=
          Networking::InvalidUserId &&
        casterRef->GetFormId() != ac->GetFormId()) {
      spdlog::debug(
        "ActionListener::OnActivate - {:x} reports {:x} activating "
        "{:x}: a relayed activation's echo",
        ac->GetFormId(), casterId, static_cast<uint32_t>(msg.data.target));
      return;
    }
    // ADR-059 decision 2: a caster the server never loads as a form (the
    // loader skips STAT bases: Bleak Falls Temple's puzzle marker 0x3e728,
    // whose activation parents open the portcullis 0x3b9f1) still names a
    // link in the game files; judged by its id alone, with no server-side
    // activation of the target through it
    if (!casterForm && partOne.HasEspm()) {
      const auto rec = partOne.GetEspm().GetBrowser().LookupById(casterId).rec;
      if (rec && rec->GetType() == espm::REFR::kType) {
        OnLinkedActivation(rawMsgData, *ac, casterId, nullptr,
                           static_cast<uint32_t>(msg.data.target));
        return;
      }
      // a caster that is no form and no record falls to the hosted caster
      // rule below, whose throw the dispatcher logs (PartOne_ActivateTest's
      // "Activate with bad hoster" keeps that shape)
    }
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
  if (msg.data.caster == 0x14 && !msg.data.isSecondActivation) {
    RelayActivationToHost(*ac, *targetPtr);
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

  if (!ContainerMoveWithinReach(*actor, ref, "put")) {
    return;
  }
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
  // as the put strips it (the review of 7c3c9f79, finding 7): the record's
  // row matches by every extra but the count, and the taker's worn flag is
  // its own engine's, never the container's
  entry.SetWorn(Inventory::Worn::None);

  if (!ContainerMoveWithinReach(*actor, ref, "take")) {
    return;
  }
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
  const size_t sent = SendRaceMenuPresetToOthers(partOne, *actor);
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

// the eighteen skills' actor values (CommonLibSSE-NG include/RE/A/
// ActorValues.h: kOneHanded 6 to kEnchanting 23), the ones leveling owns
bool IsSkillActorValue(uint8_t av)
{
  return av >= 6 && av <= 23;
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
  auto next = ToRecord(merged.record);
  if (record) {
    // thuum docs/verbs/perks.md: the perks are the server's (R0) and ride no
    // report, so the merged record carries the ones held
    next.perks = record->perks;
    next.perkPicks = record->perkPicks; // the picks ride no report either
    // thuum docs/verbs/leveling.md: with leveling on, the skills, the
    // experience and the level are the server's (R0); the report's numbers
    // for them are its echo at best and stay out of the record, so a pick's
    // conditions (perks.md) read what the server wrote. The bases of the
    // skills' actor values go the same way: the leveling path mirrors a
    // skill's level into them, and a pick reads the bases first.
    if (partOne.LevelingEnabled()) {
      next.skills = record->skills;
      next.xp = record->xp;
      next.threshold = record->threshold;
      next.level = record->level;
      next.legendary = record->legendary;
      next.bases.erase(std::remove_if(next.bases.begin(), next.bases.end(),
                                      [](const std::pair<uint8_t, float>& b) {
                                        return IsSkillActorValue(b.first);
                                      }),
                       next.bases.end());
      for (const auto& base : record->bases) {
        if (IsSkillActorValue(base.first)) {
          next.bases.push_back(base);
        }
      }
    }
  }
  const bool changed = actor->SetActorValueRecord(std::move(next));
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
  const auto merged =
    PartOne::MergePlayerGlobals(actor->GetPlayerGlobals(), report);
  const bool changed = actor->SetPlayerGlobals(merged);
  spdlog::info("PlayerGlobals: user {} actor {:x} recorded {} values{}",
               rawMsgData.userId, actor->GetFormId(), report.size(),
               changed ? "" : ", unchanged");
  // thuum memo D51 B (2026-10-28, Eli: "that sounds good"; docs/verbs/
  // player-globals.md): a death from recorded state is the server's word
  // (R0). A listed global may carry a killAt in the settings (Survival's
  // cold need 81b at 1000, the mod's own CheckIfMaxCold threshold); when the
  // record reaches it the server kills the player through its own death
  // path, the respawn following as for any death, and the client's own
  // Kill stays untrusted input (rule 5; the seat's deferred kill holds it)
  KillByPlayerGlobals(*actor, merged);
}

namespace {
// a monotonic clock in ms, defined with the lock rules below
uint64_t MonotonicMs();

// Rule 5 (the review of m3-pass-train-2, finding 2): the stage index is the
// record's to check. A quest's stages are its INDX fields (libespm QUST.h
// QuestStage::JournalIndex, QUST.cpp GetQuestStages), int16 in the record
// and u16 on the wire, compared at the wire's width. A stage the quest does
// not define is refused (E_QUEST_STAGE) instead of recorded for every seat's
// engine to park in its journal.
bool QuestHasStage(PartOne& partOne, const espm::RecordHeader* record,
                   uint16_t stage)
{
  const auto qust = espm::Convert<espm::QUST>(record);
  if (!qust) {
    return false;
  }
  for (const auto& questStage :
       qust->GetQuestStages(partOne.worldState.GetEspmCache())) {
    if (questStage.journalIndex &&
        static_cast<uint16_t>(questStage.journalIndex->index) == stage) {
      return true;
    }
  }
  return false;
}

// the wire's cap on a run word's engine stages (wire-schema
// cap::WORD_STAGES)
constexpr size_t kWordStagesPerWord = 16;

// thuum ADR-068 rung 5 (D-Q3 A, the fresh review's note 7): the stages whose
// fragments the engine runs with a run word, read from the quest's record as
// canon read them (QuestStageIsEngineStart, memo D69): for a start (word 1)
// stage 0 and the record's start-up stages (QUST INDX,
// JournalIndex::StartUpStage), for a stop (word 2) its shut-down stages
// (ShutDownStage); each once in the record's order, int16 in the record and
// u16 on the wire as QuestHasStage compares them, at most the wire's cap.
// Every start and stop word the server sends carries them (`wordStages`), so
// the receiver of a served word suppresses the fragments its acting seat ran.
// A start's list also judges a seat's report with no word (OnQuestStage):
// one of its stages is the start word's (read 5, generalized 2026-11-10)
std::vector<uint16_t> WordStages(PartOne& partOne,
                                 const espm::RecordHeader* record,
                                 uint32_t word)
{
  std::vector<uint16_t> stages;
  const auto qust = espm::Convert<espm::QUST>(record);
  if (!qust || (word != 1 && word != 2)) {
    return stages;
  }
  using Flags = espm::QUST::QuestStage::JournalIndex;
  for (const auto& questStage :
       qust->GetQuestStages(partOne.worldState.GetEspmCache())) {
    if (!questStage.journalIndex) {
      continue;
    }
    const auto index = static_cast<uint16_t>(questStage.journalIndex->index);
    const auto flags = questStage.journalIndex->flags;
    const bool runs = word == 1
      ? (index == 0 || (flags & Flags::StartUpStage) != 0)
      : (flags & Flags::ShutDownStage) != 0;
    if (runs && stages.size() < kWordStagesPerWord &&
        std::find(stages.begin(), stages.end(), index) == stages.end()) {
      stages.push_back(index);
    }
  }
  return stages;
}

// a run word's `wordStages`, set only when the record flags any
void StampWordStages(PartOne& partOne, QuestStageMessage& message)
{
  if (!message.word || !partOne.HasEspm()) {
    return;
  }
  auto stages = WordStages(
    partOne, partOne.GetEspm().GetBrowser().LookupById(message.quest).rec,
    *message.word);
  if (!stages.empty()) {
    message.wordStages = std::move(stages);
  }
}

// thuum ADR-068 rung 5: whether a line of `name` is due for `reporter`, one a
// minute per seat, the rest counted into it (RefuseStateRow's shape for a
// line that is no refusal of a row)
bool QuestLineDue(
  std::map<std::pair<uint32_t, std::string>,
           std::pair<std::chrono::steady_clock::time_point, uint32_t>>& lines,
  uint32_t reporter, const std::string& name, uint32_t& since)
{
  const auto now = std::chrono::steady_clock::now();
  auto& [said, count] = lines[{ reporter, name }];
  ++count;
  if (said.time_since_epoch().count() != 0 &&
      now - said < std::chrono::minutes(1)) {
    return false;
  }
  since = count;
  said = now;
  count = 0;
  return true;
}

// the actor a report's `acting` names when it is a connected player's
// (thuum ADR-068 rung 5, read 7: a scope's player), else none
MpActor* ConnectedPlayer(PartOne& partOne, uint32_t actorId)
{
  if (actorId < 0xff000000) {
    return nullptr;
  }
  const auto& form = partOne.worldState.LookupFormByIdNoLoad(actorId);
  MpActor* actor = form ? form->AsActor() : nullptr;
  if (!actor ||
      partOne.serverState.UserByActor(actor) == Networking::InvalidUserId) {
    return nullptr;
  }
  return actor;
}

std::vector<skymp::rules::QuestRunRow> QuestRunRows(
  const std::vector<MpChangeFormREFR::QuestRun>& runs)
{
  std::vector<skymp::rules::QuestRunRow> rows;
  rows.reserve(runs.size());
  for (const auto& run : runs) {
    rows.push_back({ run.quest.shortFormId, rust::String(run.quest.file),
                     run.running, run.seq });
  }
  return rows;
}

std::vector<MpChangeFormREFR::QuestRun> QuestRunsOf(
  const rust::Vec<skymp::rules::QuestRunRow>& rows)
{
  std::vector<MpChangeFormREFR::QuestRun> runs;
  runs.reserve(rows.size());
  for (const auto& row : rows) {
    runs.push_back(
      { FormDesc(row.short_id, std::string(row.file)), row.running, row.seq });
  }
  return runs;
}
}

// thuum docs/verbs/quest-stage.md (ADR-056; memo D58 B; thuum ADR-068 rung
// 5, docs/research/rung5-quests-2026-11-09.md sections 1(b) and 8(a)): a
// player's game reports what its engine did to a quest: set a stage, or
// started or stopped it (`word` 1 or 2; a report with no word of stage 0 or
// of a start-up stage is the start word's, read 5, generalized 2026-11-10
// for b-quest-rumor's claim). The server checks the quest (a QUST of the load
// order, the world's by the sharing mode: listed in mode list, not denied in
// mode all, and never a player-class quest while the quest scope acts,
// E_QUEST_PLAYER; R1), stamps the acting player (the report's when it names
// a connected player, the scope's player of read 7, else the reporter,
// E_QUEST_ACTING once a minute per seat; the reported `acting` is R2) and
// records it as the world's (R0): a stage the record has is a no-op, a word
// equal to the quest's run is folded, a seat flipping one quest's run past
// the churn bound is held out of the record (E_QUEST_CHURN, wire-rules
// quest_runs), and each record is bounded (QUEST_RECORD_CAP, E_QUEST_FULL;
// RUN_RECORD_CAP, E_QUEST_RUNS_FULL): rule 12's explicit resource bounds on
// a client-fed table. What moved the record is relayed at once to every
// other connected player with the acting player, its fragment run there
// under that player's scope (D-Q3 A), a stage whose fragment calls a
// player-class quest's script relayed served instead (PQ-3: applied as a
// load, so only its acting seat runs it). A client names no list, no mode,
// no quest 0 and no replay: those are the login's, server to client. The
// memos D62 and D69 rules (a start-game-enabled quest's own start each
// seat's) retired with the start word, which records that start once.
// D-Q4 C (SCHEMA_VERSION 43): a report names its cause and its stack's root
// (R2, logged). A run word whose cause is the seat's own is refused
// (E_QUEST_CAUSE: a seat whose rule held it never sends one), one line a
// minute per seat and quest. Two designed paths are not recorded either, each
// in a plain line with no E_QUEST_ code: a run word of a quest a Story Manager
// quest node names while questStoryInstances is "seat", whatever its cause
// (each seat's own instance: a world-caused completion's Stop comes here, as
// A Lovely Letter's Fragment_22 does after a typed setstage), the node named,
// one line a minute per seat and quest; and a stage report the record reads
// as a start (a start-up stage: the seat cannot tell one, the record can)
// with the seat's own cause, one line a minute per seat. Any other stage
// crosses whatever its cause. D-Q9 B (Eli 2026-11-11; train 52): under
// questPlayerAliasCause "seat", the default, a world cause rooted at a
// player alias's OnLocationChange is judged the seat's own, its line naming
// the re-judgment.
void ActionListener::OnQuestStage(const RawMessageData& rawMsgData,
                                  const QuestStageMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return spdlog::warn("OnQuestStage - no actor for user {}",
                        rawMsgData.userId);
  }
  if (!msg.shared.empty() || msg.mode != 0 || msg.quest == 0 ||
      msg.served.value_or(false)) {
    return spdlog::info("QuestStage: user {} actor {:x} report refused (a "
                        "client names no shared list, no mode, no quest 0 "
                        "and no replay)",
                        rawMsgData.userId, actor->GetFormId());
  }
  if (!partOne.HasEspm()) {
    return spdlog::warn("OnQuestStage - no master files");
  }
  // whether the reported stage is one the quest's start runs (WordStages for
  // a start: stage 0 and the record's start-up stages), the fact wire-rules
  // quest_runs judges a report with no word by: a start-up stage's report is
  // the start word's, as a stage-0 report is (read 5, generalized
  // 2026-11-10); a form that is no quest names none and is refused below
  const auto lookup = partOne.GetEspm().GetBrowser().LookupById(msg.quest);
  const auto startStages = WordStages(partOne, lookup.rec, 1);
  const bool startUp = std::find(startStages.begin(), startStages.end(),
                                 msg.stage) != startStages.end();
  // 0 a stage, 1 a start, 2 a stop; 3 a word the wire refuses first
  const uint8_t what =
    skymp::rules::quest_report_word(msg.word.value_or(0), msg.stage, startUp);
  if (what == 3) {
    return spdlog::info("QuestStage: user {} actor {:x} report refused (word "
                        "{} with stage {})",
                        rawMsgData.userId, actor->GetFormId(),
                        msg.word.value_or(0), msg.stage);
  }
  const char* const verb = what == 1 ? "start" : what == 2 ? "stop" : "stage";
  FormDesc quest;
  try {
    quest = FormDesc::FromFormId(msg.quest, partOne.worldState.espmFiles);
  } catch (const std::exception&) {
    return spdlog::info("E_QUEST_UNKNOWN: user {} actor {:x} reported {} {} "
                        "of {:x}, which is in no loaded plugin",
                        rawMsgData.userId, actor->GetFormId(), verb, msg.stage,
                        msg.quest);
  }
  if (!lookup.rec || lookup.rec->GetType() != "QUST") {
    return spdlog::info("E_QUEST_UNKNOWN: user {} actor {:x} reported {} {} "
                        "of {:x}, which is no quest",
                        rawMsgData.userId, actor->GetFormId(), verb, msg.stage,
                        msg.quest);
  }
  if (what == 0 && !QuestHasStage(partOne, lookup.rec, msg.stage)) {
    return spdlog::info("E_QUEST_STAGE: user {} actor {:x} reported stage {} "
                        "of {}, a stage the quest's record does not define "
                        "(QUST INDX)",
                        rawMsgData.userId, actor->GetFormId(), msg.stage,
                        quest.ToString());
  }
  // the mode's own refusal first (the review of m3-pass-train-2, finding
  // 5), the setting's word, then the player class's
  if (const uint8_t refusal = partOne.CheckSharedQuest(quest)) {
    if (refusal == 2) {
      return spdlog::info("E_QUEST_DENIED: user {} actor {:x} reported {} {} "
                          "of {}, a quest the world denies (memo D58 B)",
                          rawMsgData.userId, actor->GetFormId(), verb,
                          msg.stage, quest.ToString());
    }
    if (refusal == 3) {
      // every seat runs its own player's quest and reports it at the
      // engine's rate until its client takes the login's list: one line a
      // minute per seat, the rest counted
      uint32_t since = 0;
      if (QuestLineDue(questLines, actor->GetFormId(), "E_QUEST_PLAYER",
                       since)) {
        spdlog::info("E_QUEST_PLAYER: user {} actor {:x} reported {} {} of "
                     "{}, a quest of the player class, each player's own "
                     "(thuum ADR-068 rung 5); {} such report(s) since the "
                     "last line",
                     rawMsgData.userId, actor->GetFormId(), verb, msg.stage,
                     quest.ToString(), since);
      }
      return;
    }
    return spdlog::info("E_QUEST_UNLISTED: user {} actor {:x} reported {} {} "
                        "of {}, a quest the world does not share ({})",
                        rawMsgData.userId, actor->GetFormId(), verb, msg.stage,
                        quest.ToString(), refusal);
  }

  // the acting player: the report's when it names a connected player (a
  // relayed fragment's chained set on this seat, read 7), else the reporter
  uint32_t acting = actor->GetFormId();
  if (msg.acting && *msg.acting != actor->GetFormId()) {
    if (MpActor* scoped = ConnectedPlayer(partOne, *msg.acting)) {
      acting = scoped->GetFormId();
    } else {
      uint32_t since = 0;
      if (QuestLineDue(questLines, actor->GetFormId(), "E_QUEST_ACTING",
                       since)) {
        spdlog::info("E_QUEST_ACTING: user {} actor {:x} reported {} {} of "
                     "{} acting for {:x}, no connected player's actor: "
                     "stamped as the reporter; {} such report(s) since the "
                     "last line",
                     rawMsgData.userId, actor->GetFormId(), verb, msg.stage,
                     quest.ToString(), *msg.acting, since);
      }
    }
  }

  const auto relayToOthers = [&](const QuestStageMessage& relay) {
    size_t relayed = 0;
    for (size_t i = 0, n = partOne.serverState.maxConnectedId; i <= n; ++i) {
      const auto userId = static_cast<Networking::UserId>(i);
      if (userId == rawMsgData.userId ||
          !partOne.serverState.ActorByUser(userId)) {
        continue;
      }
      partOne.GetSendTarget().Send(userId, relay, true);
      ++relayed;
    }
    return relayed;
  };

  // thuum ADR-068 rung 5 (D-Q4 C, SCHEMA_VERSION 43): what the seat says
  // made the report, 1 a world cause, 2 its own, 0 none (an older client;
  // the wire refuses any other value first), and its stack's root; both the
  // seat's word (R2), judged by wire-rules quest_runs and named in the lines
  uint8_t cause =
    msg.cause && *msg.cause <= 2 ? static_cast<uint8_t>(*msg.cause) : 0;
  std::string causeWords = cause == 0
    ? std::string()
    : fmt::format("cause {}, root {}, R2", cause == 1 ? "world" : "seat",
                  msg.root && !msg.root->empty() ? *msg.root : "none");
  // D-Q9 B (Eli 2026-11-11; train 52): a player alias's OnLocationChange is
  // its seat's own cause, the event being about that seat's player, so
  // under questPlayerAliasCause "seat" (the default) a world-caused report
  // whose root, split at its last dot, is such an event (wire-rules
  // quest_runs::root_is_seat_own: the function one of PLAYER_ALIAS_EVENTS,
  // the script one the game files attach to player aliases alone) is judged
  // with the seat's own cause: a run word refused (E_QUEST_CAUSE), a report
  // judged a start not recorded, a plain stage crossing as before. Only ever
  // toward the seat's own, so a forged root gains nothing (R2)
  if (cause == 1 && msg.root && partOne.QuestPlayerAliasCauseSeat()) {
    const auto dot = msg.root->rfind('.');
    if (dot != std::string::npos) {
      const std::string script = msg.root->substr(0, dot);
      const std::string function = msg.root->substr(dot + 1);
      if (skymp::rules::quest_root_is_seat_own(
            function, partOne.ScriptRidesPlayerAliasesOnly(script))) {
        cause = 2;
        causeWords = fmt::format("cause world re-judged the seat's own: {} is "
                                 "a player alias's {}, D-Q9 B",
                                 *msg.root, function);
      }
    }
  }
  const std::string causeText =
    causeWords.empty() ? std::string() : " (" + causeWords + ")";

  if (what != 0) {
    // a run word (wire-rules quest_runs): its cause (D-Q4 C), a Story
    // Manager instance, the churn, the fold, the cap
    const auto rows = QuestRunRows(partOne.GetQuestRuns());
    const rust::Slice<const skymp::rules::QuestRunRow> record(rows.data(),
                                                              rows.size());
    const auto verdict = partOne.GetQuestRunRules().quest_runs_judge(
      record, quest.shortFormId, quest.file, what, cause, actor->GetFormId(),
      MonotonicMs());
    if (verdict.code == 4 && !msg.word) {
      // D-Q4 C: a stage report the record judges a start (a start-up stage;
      // the seat holds its stage-0 reports itself) with the seat's own cause,
      // from the login's load or an engine start: the designed path, since
      // the seat cannot tell a start-up stage and the record can, so no
      // E_QUEST_ code. Nothing written or relayed; one line a minute per
      // seat, the rest counted into it
      uint32_t since = 0;
      if (QuestLineDue(questLines, actor->GetFormId(), "seat-own start-up",
                       since)) {
        spdlog::info("QuestStage: user {} actor {:x} reported stage {} of "
                     "{}, which the record reads as its start{}: the seat's "
                     "own, not the world's (D-Q4 C), nothing recorded; {} "
                     "such report(s) since the last line",
                     rawMsgData.userId, actor->GetFormId(), msg.stage,
                     quest.ToString(), causeText, since);
      }
      return;
    }
    if (verdict.code == 5) {
      // D-Q4 C: a Story Manager instance's word or start under
      // questStoryInstances "seat", whatever its cause (wire-rules
      // quest_runs' Code::Instance): the designed path, since each seat's
      // engine runs its own instance and its fragments' Start and Stop come
      // here with a world cause (A Lovely Letter's Fragment_22 after a typed
      // setstage), so no E_QUEST_ code (81527867's shape for a start-up
      // stage). Nothing written, relayed or counted; one line a minute per
      // seat and quest, the rest counted into it, every fact the refusal's
      // line had
      uint32_t since = 0;
      if (QuestLineDue(questLines, actor->GetFormId(),
                       "story instance " + quest.ToString(), since)) {
        const std::string nodes = partOne.StoryNodesOf(quest);
        spdlog::info("[verb:quest-stage] the {} of {} by {:x} kept to its "
                     "seat: a Story Manager instance (questStoryInstances "
                     "seat{}){}, its stages still crossing; user {}'s report "
                     "not recorded, nothing relayed; {} such report(s) of "
                     "the quest since the last line",
                     verb, quest.ToString(), actor->GetFormId(),
                     causeWords.empty() ? std::string() : "; " + causeWords,
                     nodes.empty() ? std::string()
                                   : fmt::format(", named by {}", nodes),
                     rawMsgData.userId, since);
      }
      return;
    }
    if (verdict.code == 4) {
      // D-Q4 C: a run word with the seat's own cause (a seat whose rule held
      // it never sends one): nothing written, relayed or counted; one line a
      // minute per seat and quest, the rest counted into it
      const std::string name = std::string(verdict.name);
      uint32_t since = 0;
      if (QuestLineDue(questLines, actor->GetFormId(),
                       name + " " + quest.ToString(), since)) {
        spdlog::info("{}: user {} actor {:x} reported the {} of {}{}: {}; "
                     "not recorded, nothing relayed; {} such report(s) of "
                     "the quest since the last line",
                     name, rawMsgData.userId, actor->GetFormId(), verb,
                     quest.ToString(), causeText, std::string(verdict.why),
                     since);
      }
      return;
    }
    if (verdict.code == 1) {
      return spdlog::debug("QuestStage: user {} actor {:x} reported the {} "
                           "of {}, already the world's",
                           rawMsgData.userId, actor->GetFormId(), verb,
                           quest.ToString());
    }
    if (verdict.code == 2) {
      if (verdict.first) {
        // held for the run: no replay starts or stops it
        partOne.SetQuestRuns(QuestRunsOf(skymp::rules::quest_runs_without(
          record, quest.shortFormId, quest.file)));
      }
      uint32_t since = 0;
      const bool due =
        QuestLineDue(questLines, actor->GetFormId(), "E_QUEST_CHURN", since);
      if (verdict.first || due) {
        spdlog::info(
          "E_QUEST_CHURN: user {} actor {:x} reported the {} of "
          "{}: {}; held out of the world's run record for the "
          "server's run, nothing relayed",
          rawMsgData.userId, actor->GetFormId(), verb, quest.ToString(),
          verdict.first ? std::string("its run flipped past the churn bound "
                                      "on this seat (wire-rules quest_runs, "
                                      "UNMEASURED)")
                        : fmt::format("a held quest ({} such report(s) since "
                                      "the last line)",
                                      since));
      }
      return;
    }
    if (verdict.code == 3) {
      static std::chrono::steady_clock::time_point lastFull;
      const auto now = std::chrono::steady_clock::now();
      if (lastFull == std::chrono::steady_clock::time_point() ||
          now - lastFull >= std::chrono::seconds(10)) {
        spdlog::warn("{}: user {} actor {:x} reported the {} of {}: {}, "
                     "refused (one line per 10 s)",
                     std::string(verdict.name), rawMsgData.userId,
                     actor->GetFormId(), verb, quest.ToString(),
                     std::string(verdict.why));
        lastFull = now;
      }
      return;
    }
    partOne.SetQuestRuns(QuestRunsOf(skymp::rules::quest_runs_merge(
      record, quest.shortFormId, quest.file, what)));
    QuestStageMessage relay;
    relay.quest = msg.quest;
    relay.stage = 0;
    relay.word = what;
    relay.acting = acting;
    StampWordStages(partOne, relay);
    const size_t relayed = relayToOthers(relay);
    spdlog::info("[verb:quest-stage] {:x} {} {}: recorded, relayed to {}{}{}",
                 actor->GetFormId(), what == 1 ? "starts" : "stops",
                 quest.ToString(), relayed,
                 relay.wordStages
                   ? fmt::format(" with its engine stages {}",
                                 fmt::join(*relay.wordStages, ", "))
                   : std::string(),
                 causeText);
    return;
  }

  const QuestStageDone done{ quest, msg.stage };
  if (partOne.QuestStageDoneAlready(done)) {
    return spdlog::debug("QuestStage: user {} actor {:x} reported stage {} "
                         "of {}, already the world's",
                         rawMsgData.userId, actor->GetFormId(), msg.stage,
                         quest.ToString());
  }
  if (partOne.QuestRecordFull()) {
    // the record is the world's, not a seat's, and every report past the
    // cap says the same thing: one warn line per 10 s
    static std::chrono::steady_clock::time_point lastFull;
    const auto now = std::chrono::steady_clock::now();
    if (lastFull == std::chrono::steady_clock::time_point() ||
        now - lastFull >= std::chrono::seconds(10)) {
      spdlog::warn("E_QUEST_FULL: user {} actor {:x} reported stage {} of {}: "
                   "the world's quest record holds its cap of {} stages, "
                   "refused (one line per 10 s)",
                   rawMsgData.userId, actor->GetFormId(), msg.stage,
                   quest.ToString(), partOne.GetQuestStages().size());
      lastFull = now;
    }
    return;
  }
  const auto before = partOne.GetQuestStages();
  const bool firstStage =
    std::none_of(before.begin(), before.end(),
                 [&](const auto& stage) { return stage.quest == quest; });
  partOne.SetQuestStages(partOne.MergeQuestStage(done));
  // thuum ADR-068 rung 4 (amendment B, read 8): a quest the world records
  // for the first time has filled its aliases, so the items its aliases
  // create go into their containers already attached, once each
  if (firstStage) {
    partOne.AddCreatedItemsOfQuest(msg.quest);
  }

  QuestStageMessage relay;
  relay.quest = msg.quest;
  relay.stage = msg.stage;
  relay.acting = acting;
  // PQ-3 (section 8(a), read 18): a stage whose fragment calls a
  // player-class quest's script (VC01's cure, DLC1VQ03's progression) is
  // applied as a load on the other seats, so it never turns or cures their
  // own players; only its acting seat ran its fragment
  if (partOne.IsServedStage(quest, msg.stage)) {
    relay.served = true;
  }
  const size_t relayed = relayToOthers(relay);
  spdlog::info("[verb:quest-stage] {:x} set {} stage {}: recorded, relayed "
               "to {}{}",
               actor->GetFormId(), quest.ToString(), msg.stage, relayed,
               causeText);
}

namespace {
// thuum docs/verbs/prop-physics.md (ADR-058, slice one): a prop is an item
// reference (libespm's item types, the client's FormTypeEx.isItem set), from
// the game files or server-made (a drop); a door, a container, an activator
// or an actor is no prop, whatever a seat claims or streams
bool IsPropKind(MpObjectReference& refr)
{
  return !refr.AsActor() &&
    espm::utils::IsItem(espm::Type(refr.GetBaseType().c_str()));
}

// thuum D-S3 (traps follow the interaction, point 4): a trap's world body,
// a world object a seat's own script sets moving (Embershard's falling
// rocks and its tripwire, ACTIs: thuum docs/research/trap-census-2026-11-05.md
// rows 18 and the tripwire's), joins the prop stream by the impulse touch
// alone; an actor, a door, a container or furniture never does, and an item
// is a prop by IsPropKind. Point 3: a world body is also the one kind whose
// destruction a seat's report records (OnDestructionStage, E_DESTRUCTION_KIND)
bool IsWorldBodyKind(MpObjectReference& refr)
{
  if (refr.AsActor() || IsPropKind(refr)) {
    return false;
  }
  const auto& baseType = refr.GetBaseType();
  return baseType != "DOOR" && baseType != "CONT" && baseType != "FURN" &&
    baseType != "NPC_";
}

// how far a touch reaches, in units from the claimant's record to the prop's:
// the controller's contact and the grab at arm's length, a drop at the
// player's feet with the client's drop match slack, an impulse (a shout, a
// spell) across a room. UNMEASURED budgets (the stage sizes them).
constexpr float kPropTouchReach = 256.f;
constexpr float kPropDropReach = 512.f;
constexpr float kPropImpulseReach = 2048.f;

// thuum D-S3 point 4 (the review of 9ff1895e, finding 2): how far a world
// body's sample may stand from the body's game-file position. "A trap's
// world body" admits almost every world object (IsWorldBodyKind), and a
// crafted seat could claim one by the impulse and carry it off for good at
// the step budget's pace; the tether keeps every one within a fall of where
// the game files place it. UNMEASURED: a fall's extent (x-trap-boulders
// reads Embershard's rocks)
constexpr float kWorldBodyTether = 2048.f;
}

// thuum docs/verbs/prop-physics.md (ADR-058 decision 1): a player's game
// touched a physics prop it does not simulate. The gates: the reference a
// prop the server holds (not an actor), the claimant standing in its cell
// (the cell the record's position names, WorldState::CellOf); then the rule
// (wire-rules props): the newest touch wins, one epoch per prop, unless the
// claimant simulates it already or another player holds it grabbed. Granted,
// the word (PropOwner) goes reliable to every listener of the prop with a
// user, the claimant among them (it learns its epoch); the old simulator's
// seat goes Keyframed on it, the claimant's Dynamic.
void ActionListener::OnPropClaim(const RawMessageData& rawMsgData,
                                 const PropClaimMessage& msg)
{
  MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!myActor) {
    return;
  }
  // every touch names a reference the server holds already (NoLoad: a
  // claim never loads the id a seat chose, the review of 9ff1895e's finding
  // 4): an item, or under an impulse (thuum D-S3 point 4) a trap's world
  // body, which stands loaded with the chunks the claimant's own grid pass
  // loads around it (GetNeighborsByPosition, the 3x3 squares of 4096 units,
  // so every body within kPropImpulseReach of the claimant)
  const bool impulse = msg.touch == 3;
  const auto& form = partOne.worldState.LookupFormByIdNoLoad(msg.refr);
  MpObjectReference* prop = form ? form->AsObjectReference() : nullptr;
  const auto refuse = [&](const char* code, const char* why) {
    spdlog::warn(
      "[verb:prop-physics] {}: {:x} claims prop {:x} by touch {}; {}", code,
      myActor->GetFormId(), msg.refr, msg.touch, why);
  };
  if (!prop || prop->AsActor()) {
    return refuse("E_PROP_UNKNOWN", "no such reference, or an actor");
  }
  const bool worldBody = IsWorldBodyKind(*prop);
  if (!IsPropKind(*prop) && !worldBody) {
    return refuse("E_PROP_KIND", "not an item reference");
  }
  if (worldBody && !impulse) {
    return refuse("E_PROP_KIND", "a world body, which only an impulse claims");
  }
  if (msg.touch > 3) {
    return refuse("E_PROP_RANGE", "a touch the contract does not name");
  }
  const auto cell = partOne.worldState.CellOf(*prop);
  if (!cell || partOne.worldState.CellOf(*myActor) != cell) {
    return refuse("E_PROP_CELL", "the claimant is not in the prop's cell");
  }
  const float reach = msg.touch == 3 ? kPropImpulseReach
    : msg.touch == 2                 ? kPropDropReach
                                     : kPropTouchReach;
  if ((myActor->GetPos() - prop->GetPos()).Length() > reach) {
    return refuse("E_PROP_REACH", "the claimant is too far from the prop");
  }
  const auto verdict = partOne.ClaimProp(
    *myActor, *prop, *cell, static_cast<uint8_t>(msg.touch), worldBody);
  switch (verdict) {
    case PartOne::PropClaimVerdict::Granted: {
      PropOwnerMessage word;
      word.refr = prop->GetFormId();
      word.player = myActor->GetFormId();
      word.epoch = partOne.PropEpochOf(*prop);
      size_t told = 0;
      for (auto listener : prop->GetActorListeners()) {
        const auto targetUserId = partOne.serverState.UserByActor(listener);
        if (targetUserId != Networking::InvalidUserId) {
          partOne.GetSendTarget().Send(targetUserId, word, true);
          ++told;
        }
      }
      spdlog::info("[verb:prop-physics] {:x} claims prop {:x} by touch {}: "
                   "granted, epoch {}; {} listener(s) told",
                   myActor->GetFormId(), prop->GetFormId(), msg.touch,
                   word.epoch, told);
      break;
    }
    case PartOne::PropClaimVerdict::Held:
      refuse("E_PROP_HELD", "another player holds it grabbed");
      break;
    case PartOne::PropClaimVerdict::Same:
      spdlog::debug("[verb:prop-physics] {:x} claims prop {:x} by touch {}: "
                    "it simulates it already",
                    myActor->GetFormId(), prop->GetFormId(), msg.touch);
      break;
    case PartOne::PropClaimVerdict::Moving:
      // thuum D-S3 point 4: the claimant heard the first firing's owner word
      // at its grant, so its seat follows the body until the owner's rest
      refuse("E_PROP_MOVING",
             "another seat's firing set the body moving, the first firing "
             "wins");
      break;
  }
}

// thuum docs/verbs/spell-placed-object.md (ADR-057 shape A, D70): the
// caster's answer to the server's PlacedRequest for one base. The sender
// must be a player with an actor; the rest (the set, the base, the band) is
// the rule's, judged and logged in PartOne::ReportPlacedObject, which places
// the object for every other seat when the report stands.
void ActionListener::OnPlacedReport(const RawMessageData& rawMsgData,
                                    const PlacedReportMessage& msg)
{
  MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!myActor) {
    return;
  }
  partOne.ReportPlacedObject(*myActor, msg.set, msg.base,
                             NiPoint3{ msg.pos[0], msg.pos[1], msg.pos[2] },
                             msg.yaw);
}

// thuum docs/verbs/leveling.md (M5): the uses a player's engine made in the
// last second. Dropped whole while the server's `leveling` switch is off
// (the client's own advance is off as before, so nothing moves); otherwise
// each use runs the formulas into the record (PartOne::ApplySkillUse), the
// Rust edge having bounded the shape and the rate.
void ActionListener::OnSkillUse(const RawMessageData& rawMsgData,
                                const SkillUseMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return;
  }
  if (!partOne.LevelingEnabled()) {
    spdlog::debug("[verb:leveling] {:x} reports {} use(s) with leveling off; "
                  "dropped",
                  actor->GetFormId(), msg.uses.size());
    return;
  }
  for (const auto& use : msg.uses) {
    partOne.ApplySkillUse(*actor, use.skill, use.amount);
  }
}

namespace {
// the Lockpick, Skyrim.esm MISC 0xa (lab/esm.py, 2026-10-31)
constexpr uint32_t kLockpick = 0xa;

uint64_t MonotonicMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// thuum ADR-068: a reported argument as the rule reads it, its kind and an
// object's form id (wire-bridge's ScriptArg: 0 null, 1 bool, 2 number, 3
// string, 4 object); its value travels as it came
skymp::rules::ScriptArg ScriptArgOf(
  const std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>& arg)
{
  skymp::rules::ScriptArg out{};
  if (!arg) {
    out.kind = 0;
  } else if (std::holds_alternative<bool>(*arg)) {
    out.kind = 1;
  } else if (std::holds_alternative<double>(*arg)) {
    out.kind = 2;
  } else if (std::holds_alternative<std::string>(*arg)) {
    out.kind = 3;
  } else if (auto object = std::get_if<SpSnippetObjectArgument>(&*arg)) {
    out.kind = 4;
    out.form_id = object->formId;
  }
  return out;
}

// thuum ADR-068 (the hall door's FX, rung 2's merged-head review): a
// game-file placement the server holds no reference for, read from its
// record as AttachEspmRecord reads one it attaches: a REFR alone (an ACHR is
// an actor's, the streams'), whether its base is a door or a container, the
// world or cell its group names (the load order's id) and its position; none
// for another record, for no record, or for one with no base or no place.
// The loader attaches no moving static (MSTT), so Bleak Falls Sanctum
// 0x371dd's door FX 0x5256c, which HallofStoriesKeyholeScript.openDoor
// plays FXDoorOpen on, is no reference of the server's (lab/esm.py,
// 2026-11-05: base 0x52172 RuinsPuzzleDoorFX, at (128, -320, -256)).
// `actors` (thuum ADR-068 rung 4, a record row's subject): an ACHR is
// placed too, by its record's cell, so an actor the loader never held (an
// initially disabled ambush draugr, Bleak Falls Temple 0x371de's 0x380ba,
// which the trigger 0x4e421's script enables) is judged by that cell's host
struct RecordPlacement
{
  bool doorOrContainer = false;
  bool actor = false;
  uint32_t worldOrCell = 0;
  NiPoint3 pos;
};

std::optional<RecordPlacement> PlacementOfRecord(WorldState& worldState,
                                                 uint32_t formId,
                                                 bool actors = false)
{
  if (!worldState.HasEspm()) {
    return std::nullopt;
  }
  const auto& browser = worldState.GetEspm().GetBrowser();
  const auto lookup = browser.LookupById(formId);
  const auto* achr = actors ? espm::Convert<espm::ACHR>(lookup.rec) : nullptr;
  const auto* refr = espm::Convert<espm::REFR>(lookup.rec);
  if (!refr && !achr) {
    return std::nullopt;
  }
  // an ACHR's data is a REFR's (libespm ACHR::Data derives from it)
  const espm::REFR::Data data = achr
    ? static_cast<espm::REFR::Data>(achr->GetData(worldState.GetEspmCache()))
    : refr->GetData(worldState.GetEspmCache());
  const auto base = browser.LookupById(lookup.ToGlobalId(data.baseId));
  const uint32_t worldOrCell =
    LocationalDataUtils::GetWorldOrCell(browser, lookup);
  if (!base.rec || !data.loc || worldOrCell == 0) {
    return std::nullopt;
  }
  const auto baseType = base.rec->GetType();
  RecordPlacement out;
  out.actor = achr != nullptr;
  out.doorOrContainer =
    !out.actor && (baseType == "DOOR" || baseType == "CONT");
  out.worldOrCell = worldOrCell;
  out.pos = LocationalDataUtils::GetPos(data.loc);
  return out;
}
}

// thuum docs/verbs/perks.md (M5): a player's perk pick
void ActionListener::OnPerkPick(const RawMessageData& rawMsgData,
                                const PerkPickMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return;
  }
  if (!partOne.PerksEnabled()) {
    spdlog::debug("[verb:perks] {:x} picks {:x} with perks off; dropped",
                  actor->GetFormId(), msg.perk);
    return;
  }
  partOne.PickPerk(*actor, msg.perk);
}

// thuum docs/verbs/word-wall.md (M4): a player's game reports the word wall
// trigger its engine fired. The wall's word is the trigger reference's own
// (PartOne::WordOfWall), the player must stand within the activation reach of
// it in the same cell, and the word goes into the shout record once (R0), the
// seat told as the console's TeachWord tells it; every refusal is logged by
// name and nothing of a refused report lands.
void ActionListener::OnWordWall(const RawMessageData& rawMsgData,
                                const WordWallMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return;
  }
  const auto& form = partOne.worldState.LookupFormById(msg.wall);
  MpObjectReference* wall = form ? form->AsObjectReference() : nullptr;
  if (!wall) {
    spdlog::info("[verb:word-wall] E_WORDWALL_NO_REF: {:x} reports {:x}, no "
                 "such reference",
                 actor->GetFormId(), msg.wall);
    return;
  }
  const auto wordWall = partOne.WordOfWall(*wall);
  if (!wordWall) {
    spdlog::info("[verb:word-wall] E_WORDWALL_NOT_A_WALL: {:x} reports {:x}, "
                 "which teaches no word",
                 actor->GetFormId(), msg.wall);
    return;
  }
  const uint32_t word = wordWall->word;
  // the reach is measured to the wall the trigger's script names when it is
  // loaded (the trigger's own bounds are the chamber), else to the trigger
  MpObjectReference* near = wall;
  if (wordWall->wall != 0) {
    const auto& wallForm = partOne.worldState.LookupFormById(wordWall->wall);
    if (auto* wallRef = wallForm ? wallForm->AsObjectReference() : nullptr) {
      near = wallRef;
    }
  }
  if (actor->GetCellOrWorld() != near->GetCellOrWorld()) {
    spdlog::info("[verb:word-wall] E_WORDWALL_CELL: {:x} reports {:x} from "
                 "another cell",
                 actor->GetFormId(), msg.wall);
    return;
  }
  // the engine fires the trigger's OnTrigger for a player anywhere inside its
  // primitive (Bleak Falls Sanctum's is a sphere of radius 2,065 around the
  // trigger, lab/esm.py's XPRM; the read x-word-wall-read 20261101-131832),
  // and the seat reports once per session, so the server's condition is the
  // engine's: inside the trigger's primitive (the box the loader keeps for
  // it, MpObjectReference::IsPointInsidePrimitive) when the trigger has one,
  // else within the activation reach of the wall
  const float distance = (actor->GetPos() - near->GetPos()).Length();
  const bool insideTrigger =
    wall->HasPrimitive() && wall->IsPointInsidePrimitive(actor->GetPos());
  if (!insideTrigger) {
    const auto reach = skymp::rules::activation_within_reach(
      distance, ActivationTargetSize(*near));
    if (!reach.allowed) {
      spdlog::info("[verb:word-wall] E_WORDWALL_REACH: {:x} reports {:x} from "
                   "{:.0f} units off the wall {:x}, the bound {:.0f}, outside "
                   "the trigger's primitive",
                   actor->GetFormId(), msg.wall, distance, near->GetFormId(),
                   reach.bound);
      return;
    }
  }
  const bool added = actor->RecordShoutEntry(
    MpActor::ShoutEntry::WordKnown,
    FormDesc::FromFormId(word, partOne.worldState.espmFiles));
  if (!added) {
    spdlog::info("[verb:word-wall] {:x} at wall {:x}: word {:x} known already",
                 actor->GetFormId(), msg.wall, word);
    return;
  }
  SendShoutEntry(*actor, MpActor::ShoutEntry::WordKnown, word);
  spdlog::info("[verb:word-wall] {:x} learns word {:x} at wall {:x} ({:.0f} "
               "units off the wall {:x})",
               actor->GetFormId(), word, msg.wall, distance,
               near->GetFormId());
}

void ActionListener::OnObjectAnimation(const RawMessageData& rawMsgData,
                                       const ObjectAnimationMessage& msg)
{
  MpActor* reporter = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!reporter) {
    return;
  }
  if (msg.refr == 0 || msg.event.empty()) {
    spdlog::info("[verb:traps] E_OBJANIM_RANGE: {:x} reports an animation "
                 "with refr {:x} and event '{}'",
                 reporter->GetFormId(), msg.refr, msg.event);
    return;
  }
  if (msg.refr >= 0xff000000) {
    // a server-made copy's animation is the server's Papyrus's
    // (PlayAnimation), never a host's report
    spdlog::info("[verb:traps] E_OBJANIM_RANGE: {:x} reports '{}' on {:x}, "
                 "not a game-file reference",
                 reporter->GetFormId(), msg.event, msg.refr);
    return;
  }
  auto refrPtr = std::dynamic_pointer_cast<MpObjectReference>(
    partOne.worldState.LookupFormById(msg.refr));
  if (!refrPtr) {
    spdlog::info("[verb:traps] E_OBJANIM_NO_REF: {:x} reports '{}' on {:x}, "
                 "no such reference",
                 reporter->GetFormId(), msg.event, msg.refr);
    return;
  }
  if (dynamic_cast<MpActor*>(refrPtr.get())) {
    // an actor's animations are UpdateAnimation's and the stream's
    spdlog::info("[verb:traps] E_OBJANIM_ACTOR: {:x} reports '{}' on {:x}, "
                 "an actor",
                 reporter->GetFormId(), msg.event, msg.refr);
    return;
  }
  if (refrPtr->GetBaseType() == "DOOR" || refrPtr->GetBaseType() == "CONT") {
    // a door's open is isOpen's (Activate's path), not an animation; so is a
    // container's lid (thuum playtest 40's F8: the host's chest Close relayed
    // onto the guest's chest under its open ContainerMenu lost the menu its
    // target)
    return;
  }
  const auto cell = partOne.worldState.CellOf(
    refrPtr->GetCellOrWorld().ToFormId(partOne.worldState.espmFiles),
    refrPtr->GetPos());
  if (!cell) {
    spdlog::info("[verb:traps] E_OBJANIM_NO_CELL: {:x} reports '{}' on {:x}, "
                 "whose cell is unknown",
                 reporter->GetFormId(), msg.event, msg.refr);
    return;
  }
  // the fifty fixes' furniture rule (wire-rules object_animation; thuum
  // docs/research/fifty-fixes-2026-11-07.md section 1(c)): the owner of a
  // world object's graph event is the engine that drives its graph, a
  // furniture's recorded occupant's owner while it is used (a player's own
  // seat, an NPC's simulator), else the cell's host as before; a
  // furniture's event is relayed and kept nowhere, every other kept as the
  // reference's lastAnimation (R2: the owner's engine computed it)
  skymp::rules::ObjectAnimationFacts facts{};
  facts.reporter = reporter->GetFormId();
  facts.furniture = refrPtr->GetBaseType() == "FURN";
  MpActor* occupant = facts.furniture ? refrPtr->GetOccupant() : nullptr;
  if (occupant) {
    facts.occupant = occupant->GetFormId();
    if (partOne.serverState.UserByActor(occupant) !=
        Networking::InvalidUserId) {
      facts.occupant_owner = occupant->GetFormId();
    } else if (MpActor* simulator = partOne.worldState.HostOf(*occupant)) {
      facts.occupant_owner = simulator->GetFormId();
    }
  }
  facts.cell = *cell;
  const auto host = partOne.GetCellHost(*cell);
  facts.host = host.hosted ? host.player : 0;
  const auto verdict = skymp::rules::object_animation_owner(facts);
  const bool byUser = verdict.code == skymp::rules::ObjectAnimationCode::User;
  if (!byUser && verdict.code != skymp::rules::ObjectAnimationCode::Host) {
    spdlog::info("[verb:traps] {}: {:x} reports '{}' on {:x} in cell {:x}, {}",
                 std::string(verdict.name), reporter->GetFormId(), msg.event,
                 msg.refr, *cell, std::string(verdict.why));
    return;
  }
  const uint64_t nowMs = MonotonicMs();
  auto& window = objectAnimationWindows[msg.refr];
  if (nowMs - window.startMs >= 1000) {
    window.startMs = nowMs;
    window.count = 0;
  }
  if (window.count >= 8) {
    spdlog::info("[verb:traps] E_OBJANIM_RATE: {:x} reports '{}' on {:x}, "
                 "the ninth in a second on that reference; dropped",
                 reporter->GetFormId(), msg.event, msg.refr);
    return;
  }
  ++window.count;
  if (verdict.keep) {
    refrPtr->SetLastAnimation(msg.event);
  }
  ObjectAnimationMessage relay;
  relay.refr = msg.refr;
  relay.event = msg.event;
  size_t relayed = 0;
  for (auto listener : refrPtr->GetActorListeners()) {
    if (listener == reporter ||
        partOne.serverState.UserByActor(listener) ==
          Networking::InvalidUserId) {
      continue;
    }
    listener->SendToUser(relay, true);
    ++relayed;
  }
  // every line keeps today's "<refr> animates <refr>" head, which
  // x-lever-furn's claim 5 counts
  if (byUser && occupant && occupant != reporter) {
    spdlog::info("[verb:traps] {:x} animates {:x} '{}' in cell {:x} (its user "
                 "{:x}'s simulator {:x} reports): relayed to {} game(s), not "
                 "kept",
                 msg.refr, msg.refr, msg.event, *cell, occupant->GetFormId(),
                 reporter->GetFormId(), relayed);
    return;
  }
  if (byUser) {
    spdlog::info("[verb:traps] {:x} animates {:x} '{}' in cell {:x} (its user "
                 "{:x} reports): relayed to {} game(s), not kept",
                 msg.refr, msg.refr, msg.event, *cell, reporter->GetFormId(),
                 relayed);
    return;
  }
  if (!verdict.keep) {
    spdlog::info("[verb:traps] {:x} animates {:x} '{}' in cell {:x} (its host "
                 "{:x} reports): relayed to {} game(s), not kept",
                 msg.refr, msg.refr, msg.event, *cell, reporter->GetFormId(),
                 relayed);
    return;
  }
  spdlog::info("[verb:traps] {:x} animates {:x} '{}' in cell {:x} (its host "
               "{:x} reports): recorded, relayed to {} game(s)",
               msg.refr, msg.refr, msg.event, *cell, reporter->GetFormId(),
               relayed);
}

// thuum ADR-068 (the one seam; docs/research/scripts-one-system-2026-11-05.md
// 6.4 and 6.11 items 1, 8 and 13): the rule judges the shape alone, in two
// steps. Its shape step comes first, before the server reads anything of the
// subject: the reporter's budget (a token for every call that reaches it,
// refused or not), the row, the arguments and the subject's form, so a call
// it refuses costs no lookup (the hall door's FX review, finding 1: a lookup
// of a subject the server does not hold tries LoadForm, then reads its
// record). For a shaped call the core gathers the subject's facts (the
// reference the server holds, loaded as OnObjectAnimation loads it: the
// server loads references by chunk around players, so a far gate a lever
// turns may not be loaded yet; rung 2's review, finding 3), its kind, the
// host of its cell and that host's epoch, and the rule's owner step judges
// them.
namespace {
// thuum ADR-068 rung 4: a refusal's line, once a minute per reporter and
// code, the rest counted (OnScriptCall's shape)
void RefuseStateRow(
  std::map<std::pair<uint32_t, std::string>,
           std::pair<std::chrono::steady_clock::time_point, uint32_t>>&
    refusals,
  uint32_t reporter, const std::string& name, const std::string& line)
{
  const auto now = std::chrono::steady_clock::now();
  auto& [since, count] = refusals[{ reporter, name }];
  ++count;
  if (since.time_since_epoch().count() != 0 &&
      now - since < std::chrono::minutes(1)) {
    return;
  }
  spdlog::info("{}; refused, {} since the last line", line, count);
  since = now;
  count = 0;
}

// a journal fill's clear: the empty desc, which names no plugin's form (a
// fill never names a server form: wire-validate refuses one)
bool IsClearedFill(const FormDesc& refr)
{
  return refr.shortFormId == 0 && refr.file.empty();
}

// kConstant, a GLOB record's flag (CommonLibSSE-NG include/RE/T/
// TESGlobal.h:28, 1 << 6): its value is the game files' on every seat
constexpr uint32_t kGlobalConstantFlag = 0x40;

// the messages a WorldGlobal holds at most (wire-schema cap::WORLD_GLOBALS)
constexpr size_t kWorldGlobalsPerMessage = 512;
}

namespace {
// thuum ADR-068 rung 4 (amendment B; docs/research/
// rung4-state-rows-2026-11-07.md section 1(c), RefState): the owner's
// Enable, EnableNoWait, Disable, DisableNoWait or Delete of a reference,
// recorded. A reference the loader never held is attached by its record
// first (WorldState::AttachForRecord, starting as the game files start it),
// then the rule (wire-rules ref_state) moves the record; a repeat is folded
// (nothing written or relayed; the sequence the owner step took stays
// unused, a gap no receiver reads). The record (isDisabled, isDeleted and
// refStateSeq, the call's sequence) is written through SetRefState and the
// word, DestructionStage with disabled, deleted and seq, goes at once to the
// listeners the reference had before the change but the reporter (a seat the
// change subscribed hears the record behind its create word instead); an
// actor's disable is relayed before MpActor::Disable unsubscribes its
// listeners. R2, rule 5: the owner's engine ran the call; recorded as
// reported.
void RecordRefState(PartOne& partOne, MpActor& reporter,
                    const ScriptCallMessage& msg,
                    const skymp::rules::ScriptCallVerdict& verdict,
                    MpObjectReference* subject)
{
  const uint32_t id = verdict.subject;
  if (!subject && partOne.worldState.AttachForRecord(id)) {
    const auto& form = partOne.worldState.LookupFormByIdNoLoad(id);
    subject = form ? form->AsObjectReference() : nullptr;
  }
  if (!subject) {
    spdlog::info("[verb:scripts] {:x} reports {}.{} on {:x} (from {}.{}): no "
                 "reference attaches by its record; not recorded",
                 reporter.GetFormId(), msg.cls, msg.fn, id, msg.caller,
                 msg.handler);
    return;
  }
  const auto rule = skymp::rules::ref_state_apply(
    subject->IsDisabled(), subject->IsDeleted(), msg.fn);
  const char* state =
    rule.deleted ? "deleted" : (rule.disabled ? "disabled" : "enabled");
  if (rule.code == 2) {
    return; // the table's row names no ref state native
  }
  if (rule.code == 1) {
    spdlog::debug("[verb:scripts] {:x} sets {:x} {} ({}.{} from {}.{}): here "
                  "already, nothing recorded",
                  reporter.GetFormId(), id, state, msg.cls, msg.fn, msg.caller,
                  msg.handler);
    return;
  }
  const std::vector<MpActor*> listeners(subject->GetActorListeners().begin(),
                                        subject->GetActorListeners().end());
  DestructionStageMessage word;
  word.refr = id;
  if (const auto destruction = subject->GetDestruction()) {
    word.stage = destruction->stage;
    word.destroyed = destruction->destroyed;
  }
  word.disabled = rule.disabled;
  word.deleted = rule.deleted;
  word.seq = verdict.seq;
  word.served = false;
  size_t relayed = 0;
  const auto relay = [&] {
    for (MpActor* listener : listeners) {
      if (listener == &reporter ||
          partOne.serverState.UserByActor(listener) ==
            Networking::InvalidUserId) {
        continue;
      }
      listener->SendToUser(word, true);
      ++relayed;
    }
  };
  if (subject->AsActor() && rule.disabled) {
    relay();
    subject->SetRefState(rule.disabled, rule.deleted, verdict.seq);
  } else {
    subject->SetRefState(rule.disabled, rule.deleted, verdict.seq);
    relay();
  }
  spdlog::info("[verb:scripts] {:x} sets {:x} {} ({}.{} from {}.{}, seq {}): "
               "recorded, relayed to {} game(s)",
               reporter.GetFormId(), id, state, msg.cls, msg.fn, msg.caller,
               msg.handler, verdict.seq, relayed);
}

// a call's object argument and number, as the standing's rule reads them (a
// None object as 0, the first number as an integer)
std::pair<uint32_t, int64_t> StandingArgs(const ScriptCallMessage& msg)
{
  uint32_t object = 0;
  int64_t number = 0;
  bool objectRead = false;
  bool numberRead = false;
  for (const auto& arg : msg.args) {
    if (!arg) {
      objectRead = true; // a None object
      continue;
    }
    if (auto o = std::get_if<SpSnippetObjectArgument>(&*arg);
        o && !objectRead) {
      object = static_cast<uint32_t>(o->formId);
      objectRead = true;
    } else if (auto d = std::get_if<double>(&*arg); d && !numberRead) {
      // finite by the wire's own check; a Papyrus Int, read whole
      number =
        static_cast<int64_t>(std::clamp(*d, -2147483648.0, 2147483647.0));
      numberRead = true;
    }
  }
  return { object, number };
}

// thuum ADR-068 rung 4 (the standing, W3; docs/research/
// rung4-state-rows-2026-11-07.md section 1(c)): an NPC's simulator's
// SetFactionRank, ModFactionRank, RemoveFromFaction, RemoveFromAllFactions,
// SetCrimeFaction or SetRelationshipRank, merged into the NPC's record by
// the rule (wire-rules standing) and relayed at once to the NPC's other
// listeners as the one applier that brings each engine to the record's value
// (a step lands as the rank it reached), with the call's sequence
// (SpSnippet::Relay). libespm reads no CRIF, so while the record keeps no
// crime faction the base's is unread and no SetCrimeFaction folds against
// it. Refusals once a minute per reporter and code (E_STANDING_FACTION,
// _RANK, _OTHER). R2, rule 5: the simulator's engine ran the call; its
// numbers are bounded, never judged.
void RecordStanding(
  PartOne& partOne, MpActor& reporter, const ScriptCallMessage& msg,
  const skymp::rules::ScriptCallVerdict& verdict, MpObjectReference* subject,
  std::map<std::pair<uint32_t, std::string>,
           std::pair<std::chrono::steady_clock::time_point, uint32_t>>&
    refusals)
{
  MpActor* npc = subject ? subject->AsActor() : nullptr;
  if (!npc || !partOne.HasEspm()) {
    return;
  }
  const auto& files = partOne.worldState.espmFiles;
  const auto& browser = partOne.GetEspm().GetBrowser();
  const auto [object, number] = StandingArgs(msg);
  const auto* objectRec = object ? browser.LookupById(object).rec : nullptr;
  const bool factionIsFact = objectRec && objectRec->GetType() == "FACT";
  bool otherIsActor = objectRec && objectRec->GetType() == "ACHR";
  if (object != 0 && !otherIsActor) {
    const auto& form = partOne.worldState.LookupFormByIdNoLoad(object);
    otherIsActor = form && form->AsActor();
  }
  std::vector<skymp::rules::StandingFaction> factions;
  for (const auto& faction : npc->GetFactionRanks()) {
    try {
      factions.push_back({ faction.formDesc.ToFormId(files), faction.rank });
    } catch (const std::exception&) {
    }
  }
  uint32_t crime = 0xffffffffu; // the base's, unread
  if (const auto recorded = npc->GetCrimeFactionRecord()) {
    crime = 0;
    if (recorded->shortFormId != 0 || !recorded->file.empty()) {
      try {
        crime = recorded->ToFormId(files);
      } catch (const std::exception&) {
      }
    }
  }
  std::vector<skymp::rules::StandingRelation> relationships;
  for (const auto& relationship : npc->GetRelationships()) {
    try {
      relationships.push_back(
        { relationship.other.ToFormId(files), relationship.rank });
    } catch (const std::exception&) {
    }
  }
  const auto rule = skymp::rules::standing_judge(
    msg.fn, object, number, factionIsFact, otherIsActor,
    rust::Slice<const skymp::rules::StandingFaction>(factions.data(),
                                                     factions.size()),
    crime,
    rust::Slice<const skymp::rules::StandingRelation>(relationships.data(),
                                                      relationships.size()));
  if (rule.code == 3) {
    return;
  }
  if (rule.code == 2) {
    RefuseStateRow(refusals, reporter.GetFormId(), std::string(rule.name),
                   fmt::format("[verb:scripts] {}: {:x} reports {}.{} on {:x} "
                               "(from {}.{}): {}",
                               std::string(rule.name), reporter.GetFormId(),
                               msg.cls, msg.fn, npc->GetFormId(), msg.caller,
                               msg.handler, std::string(rule.why)));
    return;
  }
  if (rule.code == 1) {
    spdlog::debug("[verb:scripts] {:x} reports {}.{} on {:x}: its standing "
                  "holds it already",
                  reporter.GetFormId(), msg.cls, msg.fn, npc->GetFormId());
    return;
  }
  FormDesc form;
  if (rule.form != 0) {
    try {
      form = FormDesc::FromFormId(rule.form, files);
    } catch (const std::exception&) {
      return;
    }
  }
  const char* applier = "";
  std::vector<std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
    args;
  // an object argument numbered as SerializeArguments numbers one (an actor
  // the server holds by its long id, SpSnippet::MakeLongFormId; a player's
  // actor as 0x14 to that player's own game, SpSnippet::Relay's MakeMessage)
  const auto object_ = [&](const char* type) {
    if (rule.form == 0) {
      args.push_back(std::nullopt);
    } else {
      args.push_back(SpSnippetObjectArgument{
        SpSnippet::MakeLongFormId(&partOne.worldState, rule.form), type });
    }
  };
  switch (rule.change) {
    case 1:
      npc->SetFactionRank(form, rule.rank);
      applier = "SetFactionRank";
      object_("Faction");
      args.push_back(static_cast<double>(rule.rank));
      break;
    case 2:
      npc->RemoveFromFaction(form);
      applier = "RemoveFromFaction";
      object_("Faction");
      break;
    case 3:
      npc->RemoveFromAllFactions();
      applier = "RemoveFromAllFactions";
      break;
    case 4:
      npc->SetCrimeFactionRecord(form);
      applier = "SetCrimeFaction";
      object_("Faction");
      break;
    case 5:
      npc->SetRelationshipRank(form, rule.rank);
      applier = "SetRelationshipRank";
      object_("Actor");
      args.push_back(static_cast<double>(rule.rank));
      break;
    default:
      return;
  }
  SpSnippet relay("Actor", applier, args, npc->GetFormId());
  size_t relayed = 0;
  for (MpActor* listener : npc->GetActorListeners()) {
    if (!listener || listener == &reporter ||
        partOne.serverState.UserByActor(listener) ==
          Networking::InvalidUserId) {
      continue;
    }
    relay.Relay(listener, verdict.seq);
    ++relayed;
  }
  spdlog::info("[verb:scripts] {:x} reports {}.{} on {:x} (from {}.{}): its "
               "standing recorded as Actor.{}, seq {}, relayed to {} game(s)",
               reporter.GetFormId(), msg.cls, msg.fn, npc->GetFormId(),
               msg.caller, msg.handler, applier, verdict.seq, relayed);
}

// a call's argument `i` as an object's form id (0 for None, an argument that
// is no object, or none at `i`)
uint32_t ObjectArgAt(const ScriptCallMessage& msg, size_t i)
{
  if (i >= msg.args.size() || !msg.args[i]) {
    return 0;
  }
  const auto* object = std::get_if<SpSnippetObjectArgument>(&*msg.args[i]);
  return object && object->formId <= std::numeric_limits<uint32_t>::max()
    ? static_cast<uint32_t>(object->formId)
    : 0;
}

// the row's form, its first argument's object (0 for None or an argument
// that is no object)
uint32_t OwnRowForm(const ScriptCallMessage& msg)
{
  return ObjectArgAt(msg, 0);
}

// a call's argument `i` as a number (0 for none or another kind): AddItem's
// and RemoveItem's aiCount, which wire-rules player_own::item_count judges
// (wire-validate refuses a number that is not finite first)
double NumberArgAt(const ScriptCallMessage& msg, size_t i)
{
  if (i >= msg.args.size() || !msg.args[i]) {
    return 0;
  }
  const auto* number = std::get_if<double>(&*msg.args[i]);
  return number ? *number : 0;
}

// thuum docs/DECISIONS.md D-R1 C: the stacks a removal of `count` of `form`
// takes from `inventory`, its plain stack first (no extra data: the stack a
// script's AddItem and a pickup join), then the form's other stacks in the
// record's order, each as the record holds it with the count taken from it,
// so Inventory::RemoveItems, which matches a stack by its extra data, finds
// every one and never throws; never more than the record holds (wire-rules
// player_own clamps the count first). Which stack the engine itself took is
// its own, unread
std::vector<Inventory::Entry> StacksToRemove(const Inventory& inventory,
                                             uint32_t form, uint32_t count)
{
  std::vector<Inventory::Entry> stacks;
  uint32_t left = count;
  const Inventory::Entry plain(form, 0);
  for (const bool plainPass : { true, false }) {
    for (const auto& entry : inventory.entries) {
      if (left == 0) {
        return stacks;
      }
      if (entry.baseId != form || entry.count == 0 ||
          entry.EqualExceptCount(plain) != plainPass) {
        continue;
      }
      Inventory::Entry taken = entry;
      taken.count = std::min(left, entry.count);
      left -= taken.count;
      stacks.push_back(taken);
    }
  }
  return stacks;
}

bool NamesFunc(const std::string& fn, const char* name)
{
  return fn.size() == std::strlen(name) &&
    std::equal(fn.begin(), fn.end(), name, [](char a, char b) {
           return std::tolower(static_cast<unsigned char>(a)) ==
             std::tolower(static_cast<unsigned char>(b));
         });
}

// thuum D-R1 C's leveled gap (train 52, plan 1.5): a leveled AddItem on the
// reporter's own player whose report carries its roll (ScriptCall's
// `landed`, SCHEMA_VERSION 44: what the engine's roll put in the player's
// inventory, read inside the call's AddItemFunctor on the seat). The call's
// form must be a leveled item list (an LVLI), else its landings land on no
// list (E_SCRIPTCALL_ARGS). Each landing is judged by wire-rules player_own
// as a plain add of its base (its record type, the count the record holds,
// the stacks and whether the base needs a new one, the u32 room), refused by
// name first when the base is none the list can roll (D-R2 B,
// E_SCRIPTCALL_ROLL: PartOne::LeveledReach, the list's entries with nested
// lists followed), and the accepted ones are written together as plain
// grants (MpObjectReference::AddPlainItems: one record edit, save request and
// inventory update for the call, whose update the player's own game alone
// hears at the tick, and one OnItemAdded per landing). The call's work is
// one plain AddItem's plus a constant per landing, since the record budget
// spends one token per call (wire-rules natives' BURST_RECORD, at most 255
// landings each; the fresh review of the whole branch, SHOULD 1). One line
// names the landings recorded (the first eight, the rest counted), or that
// the roll landed nothing; each refusal is RefuseStateRow's, one line a
// minute per seat and code. R2: the seat's word that its engine's roll
// landed these bases (rule 5), checked against the list's reach and never
// rolled again here
void RecordLandings(
  PartOne& partOne, MpActor& reporter, const ScriptCallMessage& msg,
  uint32_t list, const std::string& listType,
  std::map<std::pair<uint32_t, std::string>,
           std::pair<std::chrono::steady_clock::time_point, uint32_t>>&
    refusals)
{
  const auto& landed = *msg.landed;
  const auto said = [&](const std::string& what) {
    spdlog::info("[verb:quest-stage] {:x}'s own {}.{} {:x} (from {}.{}): {}",
                 reporter.GetFormId(), msg.cls, msg.fn, list, msg.caller,
                 msg.handler, what);
  };
  if (listType != espm::LVLI::kType) {
    return RefuseStateRow(
      refusals, reporter.GetFormId(), "E_SCRIPTCALL_ARGS",
      fmt::format("[verb:quest-stage] E_SCRIPTCALL_ARGS: {:x}'s own {}.{} "
                  "{:x} (from {}.{}): {:x} is no leveled list (LVLI), so its "
                  "{} landing(s) land on no list: nothing is written",
                  reporter.GetFormId(), msg.cls, msg.fn, list, msg.caller,
                  msg.handler, list, landed.size()));
  }
  if (landed.empty()) {
    return said("its roll landed nothing");
  }
  const auto& reach = partOne.LeveledReach(list); // sorted
  const auto& browser = partOne.GetEspm().GetBrowser();
  const std::vector<skymp::rules::ScriptPropertyRow> none;
  // the record's facts for each landed base, read in one pass over its
  // stacks (the count it holds over them, and whether it has the plain stack
  // a plain add joins), then carried forward as the call's accepted landings
  // move them, so each landing is judged as a plain AddItem would be after
  // the ones before it (Fix B's 4096 stacks counted landing by landing),
  // while the stacks are read once for the call, not once per landing (the
  // fresh review of the whole branch, SHOULD 1)
  struct Held
  {
    uint32_t count = 0;
    bool plain = false;
  };
  std::unordered_map<uint32_t, Held> held;
  for (const auto& landing : landed) {
    held.emplace(landing.item, Held());
  }
  const auto& before = reporter.GetInventory();
  for (const auto& entry : before.entries) {
    const auto at = held.find(entry.baseId);
    if (at == held.end()) {
      continue;
    }
    at->second.count += entry.count; // as Inventory::GetItemCount sums
    if (!at->second.plain &&
        entry.EqualExceptCount(Inventory::Entry(entry.baseId, 0))) {
      at->second.plain = true;
    }
  }
  size_t stacks = before.entries.size();
  std::vector<Inventory::Entry> recorded;
  for (const auto& landing : landed) {
    const uint32_t base = landing.item;
    const auto rec = browser.LookupById(base).rec;
    const std::string type = rec ? rec->GetType().ToString() : std::string();
    auto& record = held[base];
    skymp::rules::PlayerOwnFacts facts{};
    facts.kind =
      !type.empty() && espm::utils::IsItem(espm::Type(type.c_str()));
    facts.leveled = type == espm::LVLI::kType;
    facts.held_count = record.count;
    facts.count = static_cast<double>(landing.count);
    facts.stacks = stacks;
    facts.new_stack = !record.plain;
    facts.landing_of = list;
    facts.in_reach = std::binary_search(reach.begin(), reach.end(), base);
    const auto verdict = partOne.GetPlayerOverlays().player_own_judge(
      reporter.GetFormId(), msg.cls, msg.fn, base,
      rust::Slice<const skymp::rules::ScriptPropertyRow>(none.data(),
                                                         none.size()),
      facts);
    if (verdict.code == 0) {
      // written below with the call's other accepted landings
      recorded.push_back(Inventory::Entry(base, verdict.count));
      record.count += verdict.count;
      if (!record.plain) {
        record.plain = true; // the add appends the base's plain stack
        ++stacks;
      }
      continue;
    }
    if (verdict.code == 3) {
      // no room left below the record's count bound: nothing moves
      spdlog::debug("[verb:quest-stage] {:x}'s own {}.{} {:x}: its landing "
                    "{:x} folded",
                    reporter.GetFormId(), msg.cls, msg.fn, list, base);
      continue;
    }
    RefuseStateRow(refusals, reporter.GetFormId(), std::string(verdict.name),
                   fmt::format("[verb:quest-stage] {}: {:x}'s own {}.{} {:x} "
                               "(from {}.{}): {}",
                               std::string(verdict.name), reporter.GetFormId(),
                               msg.cls, msg.fn, list, msg.caller, msg.handler,
                               std::string(verdict.why)));
  }
  if (recorded.empty()) {
    return;
  }
  // one record edit, one save request and one inventory update for the
  // call, then OnItemAdded once per landing, as AddItem raises it
  reporter.AddPlainItems(recorded);
  // the line names the first kNamedLandings landings and counts the rest,
  // each landed base's count read again in one pass after the call
  constexpr size_t kNamedLandings = 8;
  std::unordered_map<uint32_t, uint32_t> now;
  for (size_t i = 0; i < recorded.size() && i < kNamedLandings; ++i) {
    now.emplace(recorded[i].baseId, 0);
  }
  for (const auto& entry : reporter.GetInventory().entries) {
    if (const auto at = now.find(entry.baseId); at != now.end()) {
      at->second += entry.count;
    }
  }
  std::string landings;
  std::string heldNow;
  for (size_t i = 0; i < recorded.size() && i < kNamedLandings; ++i) {
    const uint32_t base = recorded[i].baseId;
    landings += fmt::format("{}{:x} x{}", landings.empty() ? "" : ", ", base,
                            recorded[i].count);
    heldNow += fmt::format("{}{} of {:x}", heldNow.empty() ? "" : ", ",
                           now[base], base);
  }
  if (recorded.size() > kNamedLandings) {
    landings +=
      fmt::format(" and {} more base(s)", recorded.size() - kNamedLandings);
  }
  said(fmt::format("its roll landed {}, added to its inventory as its seat "
                   "reported it (D-R1 C, R2; each a base the list can roll, "
                   "D-R2 B), {} held now",
                   landings, heldNow));
}

// thuum ADR-068 rung 5 (the two transformations, D-T1 A; docs/research/
// rung5-quests-2026-11-09.md section 8(b)): a per-player quest's script
// changed its own player through one of the seven rows (Actor.SetRace,
// AddSpell, RemoveSpell, AddShout, RemoveShout, EquipItem and, Eli's D-E1 A,
// ChangeHeadPart, on 0x14, the owner step's record 4; the head part valid
// for the recorded race by its HDPT's RNAM list, swapped on the look in
// place of the part of its HDPT type). wire-rules player_own judges it over
// the facts gathered here (the form's record type, the bound over the VMAD
// properties of the caller's attachments live for this player, the twin
// rule over their FormLists, the recorded race, the RACE's Playable flag,
// the permit, what the record holds): recorded on the change form (R1) and
// the look sent to every game that sees the player (the twin, the base
// after a cure, the swapped head part), held in the player's race overlay
// instead (never on the change form; its figure's CreateActor and
// UpdateAppearance carry it, the equipment gate counts it), the overlay
// ended at the race's return, folded, or refused by name
// (E_SCRIPTCALL_ARGS, E_SCRIPTCALL_UNNAMED, E_SCRIPTCALL_RACE), one line a
// minute per seat and code. The calling
// class is the seat's own word (`caller`), so an attachment counts only
// while it is live for the reporter (the fresh review's note 3): a QUST's
// while the quest scope classes the quest the player's or the world's
// journal records it running or staged (PartOne::QuestAttachmentLive), an
// MGEF's while the player's record or overlay holds a SPEL that carries it;
// the rest is R1 on the seat's word that its script ran (rule 5).
// Eli's D-R1 C adds the player's inventory in the same shape: a script's
// ObjectReference.AddItem or RemoveItem on its own player (a quest's
// reward, A Lovely Letter's 25 gold) is recorded in the player's change form
// as the seat reported it, R2 (rule 5's "record it as unvalidated"), with no
// bound and so no attachment read; the facts are the form's record type (an
// item, or a leveled list refused by name, E_SCRIPTCALL_LEVELED), the count
// the record holds, the call's own count, the stacks the record holds and
// whether an add needs a new one (refused by name once the record holds the
// wire's stacks in one inventory, E_SCRIPTCALL_FULL: the fresh review of the
// second head, FIX FIRST 2) and, for a removal, whether its other container
// is one the player holds open (that container's PutItem moves the item, so
// the row folds). The record's change sends the player its inventory
// (MpObjectReference::SendInventoryUpdate, deferred to the tick), which its
// client's 5 s apply reads from then on. Since train 52 (D-R1 C's leveled
// gap) a leveled AddItem whose report carries `landed` records its roll
// (RecordLandings above); one that carries none is refused by name
// (E_SCRIPTCALL_LEVELED, the seat's scriptLeveledLandings off).
void RecordPlayerOwn(
  PartOne& partOne, MpActor& reporter, const ScriptCallMessage& msg,
  std::map<std::pair<uint32_t, std::string>,
           std::pair<std::chrono::steady_clock::time_point, uint32_t>>&
    refusals)
{
  const uint32_t form = OwnRowForm(msg);
  // Eli's D-R1 C: the inventory's two rows, which read no attachment
  const bool addItem = NamesFunc(msg.fn, "AddItem");
  const bool itemRow = addItem || NamesFunc(msg.fn, "RemoveItem");
  if (!partOne.HasEspm()) {
    const char* const code =
      itemRow ? "E_SCRIPTCALL_ARGS" : "E_SCRIPTCALL_UNNAMED";
    return RefuseStateRow(
      refusals, reporter.GetFormId(), code,
      fmt::format("[verb:quest-stage] {}: {:x}'s own {}.{} {:x} (from {}.{}): "
                  "the server holds no game files to read {} from",
                  code, reporter.GetFormId(), msg.cls, msg.fn, form,
                  msg.caller, msg.handler,
                  itemRow ? "the form's record type"
                          : "the calling script's attachments"));
  }
  // D-R1 C's leveled gap (train 52): a leveled AddItem that carries its
  // roll's landings records them, each a plain add of its base
  if (addItem && msg.landed) {
    const auto rec = partOne.GetEspm().GetBrowser().LookupById(form).rec;
    return RecordLandings(partOne, reporter, msg, form,
                          rec ? rec->GetType().ToString() : std::string(),
                          refusals);
  }

  // the attachments live for this player, and what they name, each once;
  // none for the inventory's rows, which have no bound (D-R1 C)
  size_t attached = 0;
  size_t live = 0;
  std::vector<skymp::rules::ScriptPropertyRow> properties;
  if (!itemRow) {
    const auto& attachments = partOne.ScriptAttachmentsOf(msg.caller);
    attached = attachments.size();
    const auto overlayNow = partOne.PlayerOverlayOf(reporter.GetFormId());
    std::optional<std::vector<uint32_t>> held;
    const auto holds = [&](uint32_t spell) {
      if (!held) {
        held = reporter.GetSpellList();
        const auto base = reporter.KnownBaseSpells();
        held->insert(held->end(), base.begin(), base.end());
        if (overlayNow) {
          held->insert(held->end(), overlayNow->spells.begin(),
                       overlayNow->spells.end());
        }
      }
      return std::find(held->begin(), held->end(), spell) != held->end();
    };
    for (const auto& attachment : attachments) {
      const bool counts = attachment.quest
        ? partOne.QuestAttachmentLive(attachment.form)
        : std::any_of(attachment.spells.begin(), attachment.spells.end(),
                      holds);
      if (!counts) {
        continue;
      }
      ++live;
      for (const auto& n : attachment.named) {
        if (std::any_of(properties.begin(), properties.end(),
                        [&](const auto& row) { return row.form == n.form; })) {
          continue;
        }
        skymp::rules::ScriptPropertyRow row;
        row.form = n.form;
        for (uint32_t member : n.members) {
          row.members.push_back(member);
        }
        properties.push_back(std::move(row));
      }
    }
  }

  skymp::rules::PlayerOwnFacts facts{};
  const auto look = reporter.GetAppearance();
  facts.recorded_race = look ? look->raceId : 0;
  const bool setRace = NamesFunc(msg.fn, "SetRace");
  // the form's record type, the row's own: a non-race written to the look
  // drops the player's whole change form at the next start (WorldState
  // skips a record whose appearance names no RACE)
  const auto typeOf = [&]() -> std::string {
    const auto rec = partOne.GetEspm().GetBrowser().LookupById(form).rec;
    return rec ? rec->GetType().ToString() : std::string();
  };
  const std::string type = form != 0 ? typeOf() : std::string();
  // Eli's D-E1 A: the seventh row, a head part swapped on the look
  const bool headRow = NamesFunc(msg.fn, "ChangeHeadPart");
  facts.kind = setRace ? type == espm::RACE::kType
    : (NamesFunc(msg.fn, "AddSpell") || NamesFunc(msg.fn, "RemoveSpell"))
    ? type == espm::SPEL::kType
    : (NamesFunc(msg.fn, "AddShout") || NamesFunc(msg.fn, "RemoveShout"))
    ? type == espm::SHOU::kType
    : headRow ? type == espm::HDPT::kType
              : (itemRow || NamesFunc(msg.fn, "EquipItem")) && !type.empty() &&
      espm::utils::IsItem(espm::Type(type.c_str()));
  if (itemRow) {
    // D-R1 C: a leveled list (its roll is the engine's), what the record
    // holds over all the form's stacks, the call's own count (aiCount) and
    // a removal's other container (akOtherContainer), held open by the
    // player or not
    facts.leveled = type == espm::LVLI::kType;
    facts.held_count =
      form != 0 ? reporter.GetInventory().GetItemCount(form) : 0;
    facts.count = NumberArgAt(msg, 1);
    // the bound on the stacks an add mints (wire-rules
    // player_own::INVENTORY_STACKS, the wire's stacks in one inventory): how
    // many the record holds, and whether a plain add of the form joins none
    // of them (Inventory::AddItems appends an item that matches no stack,
    // its extra data and worn state compared)
    const auto& inventory = reporter.GetInventory();
    facts.stacks = inventory.entries.size();
    const Inventory::Entry plain(form, 0);
    facts.new_stack =
      std::none_of(inventory.entries.begin(), inventory.entries.end(),
                   [&](const Inventory::Entry& entry) {
                     return entry.EqualExceptCount(plain);
                   });
    if (const uint32_t other = addItem ? 0 : ObjectArgAt(msg, 3); other != 0) {
      const auto& otherForm = partOne.worldState.LookupFormByIdNoLoad(other);
      const MpObjectReference* container =
        otherForm ? otherForm->AsObjectReference() : nullptr;
      facts.into_held_open =
        container && container->IsContainerOccupant(&reporter);
    }
  }
  if (headRow && form != 0) {
    facts.race_valid = partOne.HeadPartFitsRace(form, facts.recorded_race);
    facts.held = look &&
      std::find(look->headpartIds.begin(), look->headpartIds.end(), form) !=
        look->headpartIds.end();
  }
  if (setRace && form != 0) {
    const auto race = espm::Convert<espm::RACE>(
      partOne.GetEspm().GetBrowser().LookupById(form).rec);
    facts.playable = race &&
      (race->GetData(partOne.worldState.GetEspmCache()).flags &
       espm::RACE::kPlayable);
    const auto restricted = partOne.worldState.RestrictedRaceIds();
    facts.restricted = std::find(restricted.begin(), restricted.end(), form) !=
      restricted.end();
    const auto permitted =
      partOne.worldState.PermittedRaceIds(reporter.GetProfileId());
    facts.permitted =
      std::find(permitted.begin(), permitted.end(), form) != permitted.end();
  }
  const bool spellRow =
    NamesFunc(msg.fn, "AddSpell") || NamesFunc(msg.fn, "RemoveSpell");
  const bool shoutRow =
    NamesFunc(msg.fn, "AddShout") || NamesFunc(msg.fn, "RemoveShout");
  const bool equipRow = NamesFunc(msg.fn, "EquipItem");
  if (form != 0) {
    if (spellRow) {
      facts.held = reporter.IsSpellLearned(form);
    } else if (shoutRow) {
      facts.held = reporter.HasShoutEntry(MpActor::ShoutEntry::Shout, form);
    } else if (equipRow) {
      facts.held = reporter.GetInventory().HasItem(form);
    }
  }

  const auto verdict = partOne.GetPlayerOverlays().player_own_judge(
    reporter.GetFormId(), msg.cls, msg.fn, form,
    rust::Slice<const skymp::rules::ScriptPropertyRow>(properties.data(),
                                                       properties.size()),
    facts);
  const auto said = [&](const std::string& what) {
    spdlog::info("[verb:quest-stage] {:x}'s own {}.{} {:x} (from {}.{}): {}",
                 reporter.GetFormId(), msg.cls, msg.fn, form, msg.caller,
                 msg.handler, what);
  };
  if (itemRow && verdict.code == 0) {
    // D-R1 C, R2: the seat's grant added whole, its removal at most what
    // the record holds (the rule clamped it), the plain stack first. Either
    // way the change form's inventory moves and the player's own game hears
    // it (MpObjectReference::SendInventoryUpdate, deferred to the tick);
    // nobody else hears a player's inventory
    if (addItem) {
      reporter.AddItem(form, verdict.count);
    } else {
      reporter.RemoveItems(
        StacksToRemove(reporter.GetInventory(), form, verdict.count), nullptr);
    }
    return said(fmt::format("{} {} its inventory as its seat reported it "
                            "(D-R1 C, R2), {} held now",
                            verdict.count,
                            addItem ? "added to" : "removed from",
                            reporter.GetInventory().GetItemCount(form)));
  }
  if (itemRow && verdict.code == 1) {
    // a removal of an item the open overlay holds (the beast form's skin,
    // which its EquipItem put there): taken from the overlay, the record
    // untouched, and every game that sees the player hears its look without
    // it
    if (look) {
      SendAppearanceToOthers(partOne, reporter, *look);
    }
    return said(fmt::format("taken from its race overlay {:x} (D-T1 A), {} "
                            "item(s) left in it, its record untouched",
                            verdict.overlay.race,
                            verdict.overlay.items.size()));
  }
  switch (verdict.code) {
    case 0: {
      // the change form's, R1
      std::string what = "recorded on its change form";
      if (setRace) {
        if (!look) {
          return said("no recorded look to wear the race, nothing written");
        }
        Appearance appearance = *look;
        appearance.raceId = form;
        reporter.SetAppearance(&appearance);
        SendAppearanceToOthers(partOne, reporter, appearance);
        what += fmt::format(", its look's race {:x} sent to every game that "
                            "sees it",
                            form);
      } else if (NamesFunc(msg.fn, "AddSpell")) {
        reporter.AddSpell(form);
      } else if (NamesFunc(msg.fn, "RemoveSpell")) {
        reporter.RemoveSpell(form);
      } else if (shoutRow) {
        FormDesc shout;
        try {
          shout = FormDesc::FromFormId(form, partOne.worldState.espmFiles);
        } catch (const std::exception&) {
          return said("a shout in no loaded plugin, nothing written");
        }
        if (NamesFunc(msg.fn, "AddShout")) {
          reporter.RecordShoutEntry(MpActor::ShoutEntry::Shout, shout);
        } else {
          reporter.ForgetShoutEntry(MpActor::ShoutEntry::Shout, shout);
        }
      } else if (equipRow) {
        // no row mints an item (the fresh review's note 4): outside an
        // overlay the rule folds an item the record holds and refuses one
        // it lacks, so nothing reaches here to write
        return said("nothing written (EquipItem moves nothing into the "
                    "record)");
      } else if (headRow) {
        // D-E1 A: the part of the same HDPT type swapped on the look and on
        // the stored RaceMenu look (PartOne::RecordHeadPart), the stored
        // look sent first to every game that sees the player, then the
        // look: the look's figure is built again at the appearance and
        // takes the stored look its game holds then, whose head parts
        // replace the base's by type (thuum docs/verbs/racemenu-sync.md)
        if (!look) {
          return said("no recorded look to wear the head part, nothing "
                      "written");
        }
        const auto recorded = partOne.RecordHeadPart(reporter, form);
        if (recorded.preset) {
          what += fmt::format(", its RaceMenu look's part of that type "
                              "rewritten and sent to {} game(s)",
                              SendRaceMenuPresetToOthers(partOne, reporter));
        }
        if (!recorded.look) {
          return said(recorded.preset
                        ? what
                        : std::string("the look wears it already, nothing "
                                      "written"));
        }
        if (const auto now = reporter.GetAppearance()) {
          SendAppearanceToOthers(partOne, reporter, *now);
        }
        what += fmt::format(", its look's head part {:x} sent to every game "
                            "that sees it",
                            form);
      }
      if (verdict.ended.open) {
        what += fmt::format("; its race overlay {:x} ended with it",
                            verdict.ended.race);
      }
      return said(what);
    }
    case 1:
    case 2: {
      // the overlay's (D-T1 A): every game that sees the player hears its
      // look with the overlay now held, or with none once it ended
      if (look) {
        SendAppearanceToOthers(partOne, reporter, *look);
      }
      const auto& o = verdict.code == 1 ? verdict.overlay : verdict.ended;
      return said(fmt::format(
        "{} race overlay {:x} over {:x} (D-T1 A): {} spell(s), {} shout(s), "
        "{} item(s), never on its change form",
        verdict.code == 1 ? "held in its" : "ended its", o.race, o.base,
        o.spells.size(), o.shouts.size(), o.items.size()));
    }
    case 3:
      // a head part the look wears already: the stored RaceMenu look that
      // still names another of its type is brought to it (ADR-026's one
      // authority), and every game that sees the player hears it
      if (headRow && partOne.RecordHeadPart(reporter, form).preset) {
        return said(
          fmt::format("folded on the look; its RaceMenu look's part "
                      "of that type rewritten to it and sent to {} "
                      "game(s)",
                      SendRaceMenuPresetToOthers(partOne, reporter)));
      }
      return spdlog::debug("[verb:quest-stage] {:x}'s own {}.{} {:x}: folded",
                           reporter.GetFormId(), msg.cls, msg.fn, form);
    default: {
      // the inventory's rows read no attachment (D-R1 C: no bound), so
      // their line names none
      const std::string reads = itemRow
        ? std::string()
        : fmt::format("; the class has {} attachment(s), {} live for this "
                      "player",
                      attached, live);
      RefuseStateRow(refusals, reporter.GetFormId(), std::string(verdict.name),
                     fmt::format("[verb:quest-stage] {}: {:x}'s own {}.{} "
                                 "{:x} (from {}.{}): {}{}",
                                 std::string(verdict.name),
                                 reporter.GetFormId(), msg.cls, msg.fn, form,
                                 msg.caller, msg.handler,
                                 std::string(verdict.why), reads));
      return;
    }
  }
}

// the fifty fixes' D-C3 A (PENDING ELI): a SetAnimationVariable call's
// variable kept on its subject; defined with the view's bound below
void KeepScriptVariable(MpObjectReference& subject,
                        const ScriptCallMessage& msg, uint8_t kind,
                        uint32_t seq);
}

// A game-file placement the loader never attaches (a moving static: the hall
// door's FX, rung 2's merged-head review) is judged by its record instead:
// its record's cell, its listeners the games that report that cell loaded,
// and nothing kept, since no reference of the server's carries it.
// An accepted action goes at once (SpSnippet::Relay, never the deferred queue)
// to the subject's listeners but the reporter as the row's relay native
// (PlayAnimationAndWait as PlayAnimation), with the subject's sequence; the
// PlayAnimation pair's event stays the reference's lastAnimation until rung 7,
// a SetAnimationVariable call's variable is kept on the reference with the
// call's sequence (the fifty fixes' D-C3 A, PENDING ELI), and a call relayed
// as a graph event writes ObjectAnimation's "animates" line byte for byte
// (b-puzzle-relay claims it) before the call's own line. A trap family
// trigger's own call during a firing is its firing seat's look (thuum
// ADR-068 D-W2 A, wire-rules natives' look channel): the cell's host at the
// epoch, or a game with the cell loaded one of whose actors stands by the
// trigger (the firing's own rule over the subject's gathered facts), relayed
// to the subject's other listeners and kept nowhere, with its own line.
void ActionListener::OnScriptCall(const RawMessageData& rawMsgData,
                                  const ScriptCallMessage& msg)
{
  MpActor* reporter = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!reporter) {
    return;
  }
  std::vector<skymp::rules::ScriptArg> args;
  args.reserve(msg.args.size());
  for (const auto& arg : msg.args) {
    args.push_back(ScriptArgOf(arg));
  }
  const rust::Slice<const skymp::rules::ScriptArg> argSlice(args.data(),
                                                            args.size());
  const auto at =
    skymp::rules::script_call_subject(msg.cls, msg.fn, msg.selfId, argSlice);

  // a refusal's line, either step's: one a minute per reporter and code, the
  // rest counted (OnChangeForm's shape): a script repeats its call at every
  // turn
  const auto refused = [&](const skymp::rules::ScriptCallVerdict& verdict) {
    const auto now = std::chrono::steady_clock::now();
    auto& [since, count] = scriptCallRefusals[{
      reporter->GetFormId(), static_cast<uint8_t>(verdict.code) }];
    ++count;
    if (since.time_since_epoch().count() != 0 &&
        now - since < std::chrono::minutes(1)) {
      return;
    }
    spdlog::info("[verb:scripts] {}: {:x} reports {}.{} on {:x} (epoch {}, "
                 "from {}.{}): {}; refused, {} since the last line",
                 std::string(verdict.name), reporter->GetFormId(), msg.cls,
                 msg.fn, msg.selfId, msg.epoch, msg.caller, msg.handler,
                 std::string(verdict.why), count);
    since = now;
    count = 0;
  };

  // the shape step, before anything of the subject is read: a spent budget,
  // a row or arguments the table refuses, a subject the reporter may not
  // name; a refusal here costs no lookup
  auto& scriptCalls = partOne.GetScriptCalls();
  const auto shape = scriptCalls.shape(at.row, argSlice, reporter->GetFormId(),
                                       msg.selfId, MonotonicMs());
  if (shape.code != skymp::rules::ScriptCallCode::Accepted) {
    refused(shape);
    return;
  }

  // thuum ADR-068 rung 5: inside an acting scope a seat's call names the
  // acting player's figure where its script named the player, by the
  // player's actor id; such an argument is taken only when it names a
  // connected player's actor (wire-rules natives::figures), never any other
  // server form. The list is gathered only for a call that names one
  const bool namesServerForm =
    std::any_of(args.begin(), args.end(), [](const auto& arg) {
      return arg.kind == 4 && arg.form_id >= 0xff000000;
    });
  if (namesServerForm) {
    std::vector<uint32_t> players;
    for (size_t i = 0, n = partOne.serverState.maxConnectedId; i <= n; ++i) {
      if (MpActor* player = partOne.serverState.ActorByUser(
            static_cast<Networking::UserId>(i))) {
        players.push_back(player->GetFormId());
      }
    }
    const auto figures = skymp::rules::script_call_figures(
      argSlice, rust::Slice<const uint32_t>(players.data(), players.size()));
    if (figures.code != skymp::rules::ScriptCallCode::Accepted) {
      refused(figures);
      return;
    }
  }

  // the shaped subject's facts: subject_kind 0 no reference, 1 a world
  // object, 2 a door or a container, 3 an actor, 4 a world object the server
  // holds no reference for, placed by its game-file record; the owner and
  // its epoch are the host's of the cell the subject stands in
  skymp::rules::ScriptCallFacts facts{};
  facts.sender = reporter->GetFormId();
  facts.self_id = msg.selfId;
  facts.epoch = msg.epoch;
  MpObjectReference* subject = nullptr;
  // a world object's world or cell and position, the reference's or its
  // record's
  std::optional<std::pair<uint32_t, NiPoint3>> place;
  // thuum ADR-068 rung 4 (the state rows): a record row's subject is placed
  // whatever its kind, a door's and a container's too (its enabled state is
  // the record's, never Activate's), an actor by its cell (or its ACHR's
  // when the server holds none) for the cell's host that owns it when
  // nobody simulates it
  const bool recordRow = at.record != 0;
  std::optional<std::pair<uint32_t, NiPoint3>> actorPlace;
  if (shape.subject == 0x14) {
    // the reporter's own player, an actor: its animations are the streams'
    facts.subject_kind = 3;
  } else {
    // a form the reporter may name (the shape step's: never 0, never one
    // the server names)
    const auto& form = partOne.worldState.LookupFormById(shape.subject);
    subject = form ? form->AsObjectReference() : nullptr;
    if (!subject) {
      // no reference of the server's: a game-file placement the loader
      // never attaches is judged by its record (its base's kind as a held
      // reference's, its record's cell), anything else stays unknown; a
      // record row's ACHR is placed too
      const auto record =
        PlacementOfRecord(partOne.worldState, shape.subject, recordRow);
      if (!record) {
        facts.subject_kind = 0;
      } else if (record->actor) {
        facts.subject_kind = 3;
        actorPlace.emplace(record->worldOrCell, record->pos);
      } else if (record->doorOrContainer) {
        facts.subject_kind = 2;
        if (recordRow) {
          place.emplace(record->worldOrCell, record->pos);
        }
      } else {
        facts.subject_kind = 4;
        place.emplace(record->worldOrCell, record->pos);
      }
    } else if (MpActor* actor = subject->AsActor()) {
      // an actor's owner is its simulator (ADR-063 decision 1; the death
      // report's own E_DEATH_NOT_HOST rule), which a row kept as evidence
      // reads (wire-rules natives Keep::Evidence: ProcessTrapHit's); an
      // action row on an actor is the streams' and refused by its kind
      facts.subject_kind = 3;
      facts.owner = partOne.SimulatorOf(*actor);
      if (const auto cell = partOne.worldState.CellOf(*actor)) {
        facts.cell = *cell;
        if (recordRow) {
          const auto host = partOne.GetCellHost(*cell);
          if (host.hosted) {
            facts.host = host.player;
            facts.host_epoch = host.epoch;
          }
        }
      }
    } else if (subject->GetBaseType() == "DOOR" ||
               subject->GetBaseType() == "CONT") {
      facts.subject_kind = 2;
      if (recordRow) {
        place.emplace(
          subject->GetCellOrWorld().ToFormId(partOne.worldState.espmFiles),
          subject->GetPos());
      }
    } else {
      facts.subject_kind = 1;
      place.emplace(
        subject->GetCellOrWorld().ToFormId(partOne.worldState.espmFiles),
        subject->GetPos());
    }
  }
  if (place) {
    const auto cell = partOne.worldState.CellOf(place->first, place->second);
    if (cell) {
      facts.cell = *cell;
      const auto host = partOne.GetCellHost(*cell);
      if (host.hosted) {
        facts.owner = host.player;
        facts.owner_epoch = host.epoch;
      }
    }
  }
  if (actorPlace) {
    // an actor the server holds no reference for: nobody simulates it, its
    // ACHR's cell's host owns its record rows at the epoch
    const auto cell =
      partOne.worldState.CellOf(actorPlace->first, actorPlace->second);
    if (cell) {
      facts.cell = *cell;
      const auto host = partOne.GetCellHost(*cell);
      if (host.hosted) {
        facts.host = host.player;
        facts.host_epoch = host.epoch;
      }
    }
  }

  // thuum ADR-068 D-W2 A (wire-rules natives' look channel): a row with a
  // look channel on a world object gathers its subject's firing facts as
  // D-W1 gathers a trigger's (GatherFiringFacts: the cell's host, then
  // whether the reporter's game has the cell loaded and how far its nearest
  // actor stands from the subject); a placement by its record has no
  // reference to measure from, so only its host's word is taken
  if (at.look != 0 && facts.cell != 0 &&
      (facts.subject_kind == 1 || facts.subject_kind == 4)) {
    facts.firing = GatherFiringFacts(partOne, *reporter, subject, facts.cell);
  }

  // thuum ADR-068 rung 3: a subject with a stored record seeds its sequence
  // first (a record saved before a restart, which started every sequence at
  // 0; ScriptCalls::seed, a max), so a relay never takes a number at or
  // below the one the record was served with; rung 4: so does its recorded
  // ref state's; the fifty fixes' D-C3 A: and each kept graph variable's, so
  // a later call's variable is never taken as older than a kept one's
  if (subject) {
    if (const auto& record = subject->GetScriptState()) {
      scriptCalls.seed(shape.subject, record->seq);
    }
    if (const auto refStateSeq = subject->GetRefStateSeq()) {
      scriptCalls.seed(shape.subject, *refStateSeq);
    }
    for (const auto& variable : subject->GetGraphVariables()) {
      scriptCalls.seed(shape.subject, variable.seq);
    }
  }

  // the owner step on the shaped call, over its subject's facts
  const auto verdict = scriptCalls.owner(facts);
  if (verdict.code == skymp::rules::ScriptCallCode::Skipped) {
    // a door's open is isOpen's (Activate's path), not an animation; so is a
    // container's lid (OnObjectAnimation's rule, thuum playtest 40's F8)
    return;
  }
  if (verdict.code != skymp::rules::ScriptCallCode::Accepted) {
    refused(verdict);
    return;
  }
  // thuum docs/verbs/npc-fate.md (playtest 48): a call the table keeps as
  // evidence, relayed to no seat and taking no sequence: ProcessTrapHit's,
  // the simulator's word that a trap (its first argument, TrapHitBase's
  // `akTarget.ProcessTrapHit(self, ...)`) hit an actor it simulates, noted
  // as that actor's death evidence (NoteTrapHit; R2, recorded, the numbers
  // never judged)
  if (verdict.keep_evidence) {
    MpActor* hit = subject ? subject->AsActor() : nullptr;
    if (!hit) {
      // the rule keeps an actor's alone (the reporter's own player is
      // refused before)
      return;
    }
    uint32_t trapId = 0;
    if (!msg.args.empty() && msg.args.front()) {
      if (auto trap =
            std::get_if<SpSnippetObjectArgument>(&*msg.args.front())) {
        // below 0xff000000 by the wire's own check (wire-validate
        // check_script_call)
        trapId = static_cast<uint32_t>(trap->formId);
      }
    }
    NoteTrapHit(*hit, trapId, std::chrono::steady_clock::now());
    spdlog::info("[verb:npc-fate] {:x} reports {}.{} on {:x} in cell {:x} "
                 "(from {}.{}): the trap {:x} hit it, noted as its death's "
                 "evidence",
                 reporter->GetFormId(), msg.cls, msg.fn, verdict.subject,
                 facts.cell, msg.caller, msg.handler, trapId);
    return;
  }
  // thuum ADR-068 rung 4 (the state rows): a call the table keeps as its
  // subject's record (wire-rules natives Keep::Record) moves the record and
  // is carried as the record's own word, never relayed as an action
  if (verdict.record == 1) {
    return RecordRefState(partOne, *reporter, msg, verdict, subject);
  }
  if (verdict.record == 2) {
    return RecordStanding(partOne, *reporter, msg, verdict, subject,
                          stateRowRefusals);
  }
  // thuum ADR-068 rung 5: a player's own row, its subject the reporter's own
  // player at any epoch (the owner step reads none), judged by player_own
  if (verdict.record == 4) {
    return RecordPlayerOwn(partOne, *reporter, msg, stateRowRefusals);
  }
  if (verdict.record == 3) {
    uint32_t worldOrCell = 0;
    NiPoint3 anchor;
    if (subject) {
      worldOrCell =
        subject->GetCellOrWorld().ToFormId(partOne.worldState.espmFiles);
      anchor = subject->GetPos();
    } else if (place) {
      worldOrCell = place->first;
      anchor = place->second;
    } else if (actorPlace) {
      worldOrCell = actorPlace->first;
      anchor = actorPlace->second;
    }
    uint32_t baseId = 0;
    if (!msg.args.empty() && msg.args.front()) {
      if (auto base =
            std::get_if<SpSnippetObjectArgument>(&*msg.args.front())) {
        baseId = static_cast<uint32_t>(base->formId);
      }
    }
    partOne.OpenScriptSet(*reporter, verdict.subject, worldOrCell, anchor,
                          baseId);
    return;
  }
  if (!subject && facts.subject_kind != 4) {
    // the rule accepts a world object alone, held or placed by its record
    return;
  }

  // the call's behavior graph event, the argument the row names
  std::string event;
  if (verdict.event_arg >= 0 &&
      static_cast<size_t>(verdict.event_arg) < msg.args.size()) {
    const auto& arg = msg.args[static_cast<size_t>(verdict.event_arg)];
    if (arg) {
      if (auto text = std::get_if<std::string>(&*arg)) {
        event = *text;
      }
    }
  }
  // a placement by its record keeps nothing: no reference of the server's
  // carries a lastAnimation to a late joiner (the hall door's FX is a
  // one-shot; the door it dresses, 0x4e2b8, is an ACTI the server holds)
  if (subject && verdict.keep_last && !event.empty()) {
    subject->SetLastAnimation(event);
  }
  // the fifty fixes' D-C3 A (PENDING ELI; wire-rules natives Keep::Variable):
  // the variable a SetAnimationVariable call set is kept on the reference
  // with the call's sequence and served with its script record; a placement
  // by its record keeps nothing, as lastAnimation's rule above
  if (subject && verdict.keep_var != 0) {
    KeepScriptVariable(*subject, msg, verdict.keep_var, verdict.seq);
  }

  std::vector<std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
    relayArgs;
  relayArgs.reserve(verdict.relay_args.size());
  for (uint8_t i : verdict.relay_args) {
    if (i < msg.args.size()) {
      relayArgs.push_back(msg.args[i]);
    }
  }
  // a look names the reporter's own player as its figure, the reporter's
  // actor, which the receiving seat holds as the reporter's copy (thuum
  // ADR-068 D-W2 A: CreateDetectionEvent's akOwner, the bone alarm's walker)
  if (verdict.figure_args) {
    for (auto& arg : relayArgs) {
      if (!arg) {
        continue;
      }
      if (auto object = std::get_if<SpSnippetObjectArgument>(&*arg);
          object && object->formId == 0x14) {
        object->formId = reporter->GetFormId();
      }
    }
  }
  const std::string relayCls(verdict.relay_cls);
  const std::string relayFn(verdict.relay_fn);
  SpSnippet relay(relayCls.c_str(), relayFn.c_str(), relayArgs,
                  verdict.relay_self);
  size_t relayed = 0;
  const auto relayTo = [&](MpActor* listener) {
    if (!listener || listener == reporter ||
        partOne.serverState.UserByActor(listener) ==
          Networking::InvalidUserId) {
      return;
    }
    // a look relayed with no sequence (Sound.Play's: its self the Sound,
    // shared by many triggers) is the same snippet without one
    if (verdict.look && verdict.seq == 0) {
      relay.Serve(listener);
    } else {
      relay.Relay(listener, verdict.seq);
    }
    ++relayed;
  };
  // audience 0: the subject's listeners (every action row's at rung 2); a
  // placement by its record has no reference of the server's to listen to,
  // so its listeners are the games that report its cell loaded
  if (verdict.audience == 0 && subject) {
    for (auto listener : subject->GetActorListeners()) {
      relayTo(listener);
    }
  } else if (verdict.audience == 0) {
    for (const uint32_t loader : partOne.GetCellLoaders(facts.cell)) {
      const auto& form = partOne.worldState.LookupFormByIdNoLoad(loader);
      relayTo(form ? form->AsActor() : nullptr);
    }
  } else if (verdict.audience == 3 && facts.owner != 0) {
    // the subject's cell's host alone (a look's CreateDetectionEvent: the
    // engine that simulates the cell's NPCs), never the reporter
    const auto& form = partOne.worldState.LookupFormByIdNoLoad(facts.owner);
    relayTo(form ? form->AsActor() : nullptr);
  }
  // thuum ADR-068 D-W2 A: a firing's look, its seat's word (the cell's
  // host's or a loader's by the trigger), relayed and kept nowhere, the
  // detection event to the cell's host alone
  if (verdict.look) {
    const std::string seqWord =
      verdict.seq ? fmt::format("seq {}", verdict.seq) : std::string("no seq");
    const char* whose =
      verdict.audience == 3 ? " (the cell's host alone)" : "";
    spdlog::info("[verb:traps] {:x} reports {}.{} on {:x} in cell {:x} "
                 "(epoch {}, from {}.{}): its firing seat's look, {}, relayed "
                 "to {} game(s){} as {}.{}, kept nowhere (D-W2)",
                 reporter->GetFormId(), msg.cls, msg.fn, verdict.subject,
                 facts.cell, msg.epoch, msg.caller, msg.handler, seqWord,
                 relayed, whose, relayCls, relayFn);
    return;
  }
  if (verdict.event_arg >= 0) {
    spdlog::info("[verb:traps] {:x} animates {:x} '{}' in cell {:x} (its host "
                 "{:x} reports): recorded, relayed to {} game(s)",
                 verdict.subject, verdict.subject, event, facts.cell,
                 reporter->GetFormId(), relayed);
  }
  spdlog::info("[verb:scripts] {:x} reports {}.{} on {:x} in cell {:x} "
               "(epoch {}, from {}.{}): seq {}, relayed to {} game(s) as "
               "{}.{}",
               reporter->GetFormId(), msg.cls, msg.fn, verdict.subject,
               facts.cell, msg.epoch, msg.caller, msg.handler, verdict.seq,
               relayed, relayCls, relayFn);
}

namespace {
// the longest behavior graph event the wire carries (wire-schema
// cap::ANIM_EVENT): a record's view past it would make its apply
// unrenderable, so such a view is not kept
constexpr size_t kAnimEventBytes = 128;

// the fifty fixes' D-C3 A (PENDING ELI): the variable a SetAnimationVariable
// call names (its first argument) and the value it sets (its second), of
// the row's folded `kind` (3 an int, 4 a float, 5 a bool), kept on `subject`
// with the call's `seq` where the record's slot rule puts it
// (MpObjectReference::KeepGraphVariable). R2: recorded as reported, its
// shape bounded: a name empty, carrying a NUL or past the wire's bound for
// an event (a served apply carrying it would be refused), or a value that
// is no finite number of its kind, is kept nowhere
void KeepScriptVariable(MpObjectReference& subject,
                        const ScriptCallMessage& msg, uint8_t kind,
                        uint32_t seq)
{
  if (msg.args.size() < 2 || !msg.args[0] || !msg.args[1]) {
    return;
  }
  const auto* name = std::get_if<std::string>(&*msg.args[0]);
  if (!name || name->empty() || name->size() > kAnimEventBytes ||
      name->find('\0') != std::string::npos) {
    return;
  }
  MpChangeFormREFR::GraphVariable variable;
  variable.name = *name;
  variable.k = kind;
  variable.seq = seq;
  const auto& value = *msg.args[1];
  if (kind == 5) {
    const auto* flag = std::get_if<bool>(&value);
    if (!flag) {
      return;
    }
    variable.b = *flag;
  } else {
    const auto* number = std::get_if<double>(&value);
    if (!number || !std::isfinite(*number)) {
      return;
    }
    if (kind == 3) {
      if (*number < static_cast<double>(std::numeric_limits<int32_t>::min()) ||
          *number > static_cast<double>(std::numeric_limits<int32_t>::max())) {
        return;
      }
      variable.i = static_cast<int32_t>(*number);
    } else if (kind == 4) {
      if (std::fabs(*number) >
          static_cast<double>(std::numeric_limits<float>::max())) {
        return;
      }
      variable.f = static_cast<float>(*number);
    } else {
      return;
    }
  }
  subject.KeepGraphVariable(std::move(variable));
}
}

// thuum ADR-068 rung 3 (the script record, decision 5 and its amendment A;
// docs/research/rung3-script-record-2026-11-06.md section 1(c)): a
// reference's owner reports the scripts attached to it as its engine read
// them at a quiescent point (no running or waiting stack on the object). The
// wire validated the shape (wire-validate check_script_state); here the
// reference is looked up as OnDestructionStage looks up a trap's lamp
// (LookupFormById, which loads it with its chunk; an actor's record is
// refused by its type before any load, that handler's idiom), its cell,
// that cell's host and its standing epoch gathered, and the rule (wire-rules
// script_state) judges them: the epoch first (ADR-066 decision 4), then the
// owner, and while nobody hosts the cell the release read of the host that
// just released it (ApplyHostDecisions noted the release). An accepted
// record is kept on the reference (saved, rule 6), stamped with the
// object's sequence (ScriptCalls' seq_of, the record it replaces seeding it
// first, so the sequence never steps back after a restart) and its view, the
// reference's lastAnimation now (amendment A: the Temple pillar's OnCellLoad
// turns the graph only once, so a late seat cannot render the record from
// the script). A record is never relayed live: PartOne serves it ahead of a
// grant and behind a create word, every send at once, never the deferred
// queue (OnChangeForm's shape, less its ack and its live serve).
void ActionListener::OnScriptState(const RawMessageData& rawMsgData,
                                   const ScriptStateMessage& msg)
{
  MpActor* sender = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!sender) {
    spdlog::warn("ActionListener::OnScriptState - user {} has no actor; its "
                 "record of {:x} ignored",
                 rawMsgData.userId, msg.formId);
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  // one line a minute per sender, form and code, the rest counted
  // (OnChangeForm's shape): an owner whose premise is wrong repeats its
  // record at every stack's end and every sweep
  const auto refuse = [&](const std::string& code, const std::string& why) {
    auto& [since, count] = scriptStateRefusals[std::make_tuple(
      sender->GetFormId(), msg.formId, code)];
    ++count;
    if (since.time_since_epoch().count() != 0 &&
        now - since < std::chrono::minutes(1)) {
      return;
    }
    spdlog::info("[verb:scripts] {}: {:x} reports the script state of {:x} "
                 "({} script(s), fnv {:08x}, epoch {}): {}; refused, {} since "
                 "the last line",
                 code, sender->GetFormId(), msg.formId, msg.scripts.size(),
                 msg.fnv1a, msg.epoch, why, count);
    since = now;
    count = 0;
  };
  if (msg.formId == 0 || msg.formId >= 0xff000000) {
    // a server-made reference's scripts are the server's own (the edge
    // refuses the id first, E_VAL_RANGE)
    return refuse("E_SCRIPTSTATE_NO_REF", "not a game-file reference");
  }

  // the reference's facts: ref_kind 0 no reference, 1 a reference that is
  // no actor, 2 an actor; the owner and its epoch are the host's of the
  // cell the reference stands in, the kept epoch while nobody hosts it
  skymp::rules::ScriptStateFacts facts{};
  facts.sender = sender->GetFormId();
  facts.epoch = msg.epoch;
  MpObjectReference* refr = nullptr;
  bool actorRecord = false;
  if (partOne.HasEspm()) {
    const auto record = partOne.GetEspm().GetBrowser().LookupById(msg.formId);
    actorRecord = record.rec && record.rec->GetType() == espm::ACHR::kType;
  }
  if (actorRecord) {
    facts.ref_kind = 2;
  } else {
    const auto& form = partOne.worldState.LookupFormById(msg.formId);
    refr = form ? form->AsObjectReference() : nullptr;
    if (refr) {
      facts.ref_kind = refr->AsActor() ? 2 : 1;
    }
  }
  if (refr && facts.ref_kind == 1) {
    if (const auto cell = partOne.worldState.CellOf(*refr)) {
      facts.cell = *cell;
      const auto host = partOne.GetCellHost(*cell);
      facts.owner = host.hosted ? host.player : 0;
      facts.owner_epoch = host.epoch;
    }
  }
  const auto verdict = partOne.GetScriptStates().judge(facts);
  const bool releaseRead =
    verdict.code == skymp::rules::ScriptStateCode::ReleaseRead;
  if (verdict.code != skymp::rules::ScriptStateCode::Accepted &&
      !releaseRead) {
    return refuse(std::string(verdict.name), std::string(verdict.why));
  }
  if (!refr) {
    // the rule records a reference that is no actor alone
    return;
  }

  // the record it replaces seeds the object's sequence first (a record saved
  // before a restart), so the record taken now never names a sequence below
  // the one the last record was served with
  auto& scriptCalls = partOne.GetScriptCalls();
  if (const auto& before = refr->GetScriptState()) {
    scriptCalls.seed(msg.formId, before->seq);
  }
  ScriptStateRecord record;
  record.epoch = msg.epoch;
  record.fnv1a = msg.fnv1a;
  record.sender = sender->GetFormId();
  record.atMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch())
      .count());
  record.seq = scriptCalls.seq_of(msg.formId);
  // the view, within the wire's bound for an event, or none
  if (auto event = refr->GetLastAnimation();
      event && event->size() <= kAnimEventBytes) {
    record.event = std::move(event);
  }
  record.scripts = msg.scripts;
  const uint32_t seq = record.seq;
  const std::string view = record.event.value_or("");
  refr->SetScriptState(std::move(record));

  // one info line a minute per form, the rest at debug: an owner reports at
  // every stack's end that moved the hash; a release read is said every
  // time (one per reference at a release)
  auto& said = scriptStateLines[msg.formId];
  if (releaseRead || said.time_since_epoch().count() == 0 ||
      now - said >= std::chrono::minutes(1)) {
    spdlog::info("[verb:scripts] script state {:x} recorded from {:x}: {} "
                 "script(s), fnv {:08x}, epoch {}, seq {}, view '{}'{}",
                 msg.formId, sender->GetFormId(), msg.scripts.size(),
                 msg.fnv1a, msg.epoch, seq, view,
                 releaseRead ? " (its release read)" : "");
    said = now;
  } else {
    spdlog::debug("[verb:scripts] script state {:x} recorded from {:x}: fnv "
                  "{:08x}, seq {}",
                  msg.formId, sender->GetFormId(), msg.fnv1a, seq);
  }
}

// thuum docs/DECISIONS.md ADR-059 decision 2: whether a reference's own
// record or its base carries a script (a puzzle pillar, a lever, a plate:
// the vanilla world's mechanisms are Papyrus); the base's answer is read
// once
bool ActionListener::IsScriptedWorldObject(const MpObjectReference& target)
{
  if (!partOne.HasEspm()) {
    return false;
  }
  auto& browser = partOne.GetEspm().GetBrowser();
  auto& cache = partOne.worldState.GetEspmCache();
  const auto scripted = [&](uint32_t formId) {
    const auto lookup = browser.LookupById(formId);
    if (!lookup.rec) {
      return false;
    }
    espm::ScriptData scriptData;
    lookup.rec->GetScriptData(&scriptData, cache);
    for (const auto& script : scriptData.scripts) {
      if (script.status != 3) {
        return true;
      }
    }
    return false;
  };
  const auto baseId = target.GetBaseId();
  auto it = scriptedBases.find(baseId);
  if (it == scriptedBases.end()) {
    it = scriptedBases.emplace(baseId, scripted(baseId)).first;
  }
  if (it->second) {
    return true;
  }
  return target.IsEspmForm() && scripted(target.GetFormId());
}

// thuum D-W1 and D-C2 (playtest 50; docs/research/fifty-fixes-2026-11-07.md
// 1(c)): a trigger volume's entry counts once, on its presser's owner, and a
// world trigger's script runs on the cell's host, so a seat that does not
// host the trigger's cell reports its own presser's entry (an Activate
// carrying `entry`, its shape the edge's: wire-validate check_activate). The
// facts are gathered here and the rule is Rust's (wire-rules trigger_entry's
// entry, through the bridge): the trigger a world object the server holds or
// places by its record, no actor (E_ENTRY_NO_REF, E_ENTRY_ACTOR); the
// presser the reporter's own actor or one its game simulates
// (E_ENTRY_PRESSER); the reporter a loader of the trigger's cell
// (E_ENTRY_NOT_LOADER) that does not host it (E_ENTRY_HOST: its own engine
// ran the script); a host there (E_ENTRY_NO_HOST: nothing runs the script);
// the presser within the trigger's activation reach (E_ENTRY_REACH). A leave
// pairs with its enter (wire-rules trigger_entry::Entries, PartOne's; the
// builder's call on the probe review of D-C2): one ending none is
// E_ENTRY_UNPAIRED before any other check; one ending an accepted enter of
// its presser's into its trigger is taken without the reach or the loader
// (a guest leaving by a teleport or a door reports it after the move, far
// off the trigger, its game perhaps with the cell unloaded), every other
// check kept. An
// accepted entry goes to the host's game alone as RelayedActivation {target:
// the trigger, caster: the presser, entry}, whose engine sends that event to
// the trigger's scripts with the presser as akActionRef; nothing is recorded
// but the pairing (R2: the presser's owner's engine saw the entry; rule 5,
// a leave validated by its pairing). A refusal sends nothing and is said by
// name, once a minute per reporter and code.
void ActionListener::OnTriggerEntry(MpActor& reporter,
                                    const ActivateMessage& msg)
{
  const uint32_t entry = msg.data.entry.value_or(0);
  const char* moving = entry == 2 ? "leaving" : "entering";
  // below 0xff000000 by the edge's check
  const auto triggerId = static_cast<uint32_t>(msg.data.target);

  // the presser: the reporter's own player for 0x14, else the actor it
  // names; a caster past 32 bits names no form (the edge refuses it,
  // wire-validate check_activate), so it reads as no presser here too
  MpActor* presser = nullptr;
  uint32_t presserId = 0;
  if (msg.data.caster == 0x14) {
    presser = &reporter;
    presserId = reporter.GetFormId();
  } else if (msg.data.caster <= std::numeric_limits<uint32_t>::max()) {
    presserId = static_cast<uint32_t>(msg.data.caster);
    const auto& form = partOne.worldState.LookupFormById(presserId);
    presser = form ? form->AsActor() : nullptr;
  }

  const auto refuse = [&](const std::string& code, const std::string& why) {
    const auto now = std::chrono::steady_clock::now();
    auto& [since, count] =
      triggerEntryRefusals[{ reporter.GetFormId(), code }];
    ++count;
    if (since.time_since_epoch().count() != 0 &&
        now - since < std::chrono::minutes(1)) {
      return;
    }
    spdlog::info("[verb:traps] {}: {:x} reports {:x} {} {:x}: {}; refused, "
                 "{} since the last line",
                 code, reporter.GetFormId(), presserId, moving, triggerId, why,
                 count);
    since = now;
    count = 0;
  };

  // the trigger's facts: trigger_kind 0 no reference, 1 a world object the
  // server holds or, holding no reference for it, places by its game-file
  // record, 2 an actor (an ACHR's record is one before any load,
  // OnScriptState's idiom); its cell, that cell's host and whether the
  // reporter's game has the cell loaded; the reach, measured from the
  // presser to the trigger in one world or cell (-1 across two, which no
  // reach takes)
  skymp::rules::TriggerEntryFacts facts{};
  facts.reporter = reporter.GetFormId();
  facts.presser = presserId;
  facts.distance = -1.f;
  std::optional<std::pair<uint32_t, NiPoint3>> place;
  float triggerSize = 0.f;
  bool actorRecord = false;
  if (partOne.HasEspm()) {
    const auto record = partOne.GetEspm().GetBrowser().LookupById(triggerId);
    actorRecord = record.rec && record.rec->GetType() == espm::ACHR::kType;
  }
  // the trigger the server holds, if it holds one: its primitive, when it
  // has one, is the engine's own volume for the entry (the reach below)
  MpObjectReference* held = nullptr;
  if (actorRecord) {
    facts.trigger_kind = 2;
  } else {
    const auto& form = partOne.worldState.LookupFormById(triggerId);
    held = form ? form->AsObjectReference() : nullptr;
    if (held && held->AsActor()) {
      facts.trigger_kind = 2;
    } else if (held) {
      facts.trigger_kind = 1;
      place.emplace(
        held->GetCellOrWorld().ToFormId(partOne.worldState.espmFiles),
        held->GetPos());
      triggerSize = ActivationTargetSize(*held);
    } else if (const auto record =
                 PlacementOfRecord(partOne.worldState, triggerId)) {
      facts.trigger_kind = 1;
      place.emplace(record->worldOrCell, record->pos);
    }
  }
  std::optional<uint32_t> cell;
  if (place) {
    cell = partOne.worldState.CellOf(place->first, place->second);
  }
  if (cell) {
    facts.cell = *cell;
    const auto loaders = partOne.GetCellLoaders(*cell);
    facts.loader = std::find(loaders.begin(), loaders.end(),
                             reporter.GetFormId()) != loaders.end();
    const auto host = partOne.GetCellHost(*cell);
    facts.host = host.hosted ? host.player : 0;
    // a host whose game is gone (its grace) runs no script: nobody, so the
    // rule refuses the entry before the pairing keeps an enter it never
    // relayed
    if (facts.host != 0) {
      const auto& hostForm = partOne.worldState.LookupFormById(facts.host);
      MpActor* hostActor = hostForm ? hostForm->AsActor() : nullptr;
      if (!hostActor ||
          partOne.serverState.UserByActor(hostActor) ==
            Networking::InvalidUserId) {
        facts.host = 0;
      }
    }
  }
  // the presser's owner: a played actor is its own player's, any other
  // actor its simulator's (WorldState::HostOf, OnActivate's hosted caster
  // rule); a presser that is no actor is nobody's
  if (presser) {
    const bool played =
      partOne.serverState.UserByActor(presser) != Networking::InvalidUserId;
    MpActor* simulator =
      played ? presser : partOne.worldState.HostOf(*presser);
    facts.presser_owner = simulator ? simulator->GetFormId() : 0;
    if (place &&
        presser->GetCellOrWorld().ToFormId(partOne.worldState.espmFiles) ==
          place->first) {
      // a presser inside the trigger's primitive (the box or sphere the
      // loader keeps for it) is on the trigger however far from its origin,
      // as the firing's measure and the word wall's condition take it
      facts.distance = held && held->HasPrimitive() &&
          held->IsPointInsidePrimitive(presser->GetPos())
        ? 0.f
        : (presser->GetPos() - place->second).Length();
    }
  }
  facts.trigger_size = triggerSize;
  facts.trigger_id = triggerId;
  facts.leave = entry == 2;

  // relayed, the verdict names the one game to send it to, the host's; a
  // leave pairs with its presser's accepted enter
  const auto verdict = partOne.GetTriggerEntries().judge(facts);
  if (verdict.code != skymp::rules::TriggerEntryCode::Relay) {
    return refuse(std::string(verdict.name), std::string(verdict.why));
  }
  const auto& hostForm = partOne.worldState.LookupFormById(verdict.host);
  MpActor* host = hostForm ? hostForm->AsActor() : nullptr;
  if (!host ||
      partOne.serverState.UserByActor(host) == Networking::InvalidUserId) {
    // the cell's host has no game connected: its engine cannot run the
    // script either, as the rule's own E_ENTRY_NO_HOST
    return refuse("E_ENTRY_NO_HOST", "the cell's host has no game connected");
  }
  RelayedActivationMessage relay;
  relay.target = triggerId;
  relay.caster = presserId;
  relay.entry = entry;
  host->SendToUser(relay, true);
  spdlog::info("[verb:traps] {:x} reports {:x} {} {:x} in cell {:x} (its "
               "host {:x}): relayed to the host",
               reporter.GetFormId(), presserId, moving, triggerId, facts.cell,
               verdict.host);
}

void ActionListener::RelayActivationToHost(MpActor& player,
                                           MpObjectReference& target)
{
  const auto& baseType = target.GetBaseType();
  if (baseType != "ACTI" && baseType != "FURN") {
    return;
  }
  if (!IsScriptedWorldObject(target)) {
    return;
  }
  const auto cell = partOne.worldState.CellOf(
    target.GetCellOrWorld().ToFormId(partOne.worldState.espmFiles),
    target.GetPos());
  if (!cell) {
    return;
  }
  const auto host = partOne.GetCellHost(*cell);
  if (!host.hosted) {
    return;
  }
  // every other game in the cell gets it: the host's, so the cell's
  // simulation counts the press, and the guests', whose engines run the same
  // script while the layer admits it everywhere (the hosted-cell gate's
  // train replaces the guests' share with the host's animations, ADR-059
  // decision 3); the sender's own engine ran it already
  RelayedActivationMessage relay;
  relay.target = target.GetFormId();
  relay.caster = player.GetFormId();
  size_t sent = 0;
  for (auto listener : target.GetActorListeners()) {
    if (listener == &player) {
      continue;
    }
    if (partOne.serverState.UserByActor(listener) !=
        Networking::InvalidUserId) {
      listener->SendToUser(relay, true);
      ++sent;
    }
  }
  spdlog::info(
    "[verb:traps] {:x} activates {:x} in cell {:x} (its host {:x}): "
    "relayed to {} other game(s)",
    player.GetFormId(), target.GetFormId(), *cell, host.player, sent);
}

// thuum docs/verbs/traps.md (M4): a game reports its engine's linked
// activation. The pair must be the game files' (the target's activation
// parents name the caster: WorldState's table, filled as the target loaded;
// Bleak Falls' spike wall names its plate and its portcullis names the
// lever, lab/esm.py 2026-11-01), the reporter must be its firing's owner
// (thuum ADR-068 D-W1, wire-rules trigger_entry's firing_source: the
// target's cell's host, or a game with the cell loaded one of whose actors
// stands by the trigger), and a game sends at most eight a second. The
// server records it through Activate's own path with the caster the engine
// named (a door's open, the hazard note, the target's own children) and
// relays it to the cell's other games, the host's among them, which play it
// with that caster (R2: the reporter's engine computed it). Every refusal is
// logged by name. An actor target is its
// simulator's (ADR-068, the rule below): dropped, or relayed to the one seat
// that simulates it when that seat is not the reporter.
void ActionListener::OnLinkedActivation(const RawMessageData& rawMsgData,
                                        MpActor& reporter, uint32_t casterId,
                                        MpObjectReference* casterRef,
                                        uint32_t targetId)
{
  auto targetPtr = std::dynamic_pointer_cast<MpObjectReference>(
    partOne.worldState.LookupFormById(targetId));
  // the linked activations the server relays for one game, at most eight in
  // any second, through the actor's relay or the cell's; a report a check
  // drops or refuses spends none (an E_TRAP_ACTIVATE, the server's own
  // activation failing after the budget, has spent it)
  const auto withinBudget = [&] {
    const uint64_t nowMs = MonotonicMs();
    auto& window = linkedActivationWindows[rawMsgData.userId];
    if (nowMs - window.startMs >= 1000) {
      window.startMs = nowMs;
      window.count = 0;
    }
    if (window.count >= 8) {
      spdlog::info(
        "[verb:traps] E_TRAP_RATE: {:x} reports {:x} activating {:x}, "
        "the ninth in a second; dropped",
        reporter.GetFormId(), casterId, targetId);
      return false;
    }
    ++window.count;
    return true;
  };
  // thuum docs/DECISIONS.md ADR-068 (an actor's script and AI run on the seat
  // that simulates it, and the NPC stream carries what follows): a linked
  // activation whose target is an ACTOR is that actor waking on its
  // simulator, an ambush's draugr roused by its trigger (Bleak Falls Temple's
  // 0x38098 and its four ACHR children 0x380b3, 0x380b4, 0x380b5, 0x380b9,
  // whose XAPR name it; x-bleak-traps-npcs 20261105-023048). The server does
  // not judge it and never relays it to the cell's other games: a relay there
  // would run the engine's activation on a watcher's puppet, which renders
  // and never simulates (ADR-063 decision 3). Who simulates a loaded actor is
  // the world's answer (WorldState::HostOf: the player a follow or a dialogue
  // claim names, else the cell's host, else nobody). The reporter, or nobody:
  // the report is dropped, since the reporter's engine ran the waking or no
  // engine runs the actor. Another seat (a dialogue's claimant, ADR-052; a
  // pass-margin follow into the reporter's cell; a thrall's commander once
  // ADR-060's row lands): the activation goes to that seat's game alone, the
  // one engine that runs the actor, within the reporter's budget. An exterior
  // ambush whose actor stands in a cell another seat hosts never reaches
  // here: the client reports a linked activation only when its seat hosts
  // the target's cell (activationService.ts), and the trigger's host does
  // not, so that case is open (docs/verbs/traps.md). A simulator with no
  // game connected counts as nobody, and an actor the server never loaded
  // has no simulator it knows of. Before this rule the pair read
  // E_TRAP_UNLINKED (the activation-parent table is filled for non-actor
  // references alone, MpObjectReference::Init) or E_TRAP_NO_REF (an actor
  // the server never loaded, judged here by its game-file record).
  MpActor* targetActor = targetPtr ? targetPtr->AsActor() : nullptr;
  const bool targetIsActor = targetPtr
    ? targetActor != nullptr
    : (partOne.HasEspm() &&
       partOne.GetEspm().GetBrowser().LookupById(targetId).rec &&
       partOne.GetEspm().GetBrowser().LookupById(targetId).rec->GetType() ==
         espm::ACHR::kType);
  if (targetIsActor) {
    MpActor* simulator =
      targetActor ? partOne.worldState.HostOf(*targetActor) : nullptr;
    if (simulator && simulator != &reporter &&
        partOne.serverState.UserByActor(simulator) !=
          Networking::InvalidUserId) {
      // the relay runs another seat's engine, so the report meets the cell
      // path's two consistency checks first (rule 5): the reporter is its
      // firing's owner in the cell its trigger stands in (thuum ADR-068
      // D-W1, wire-rules trigger_entry's firing_source: that cell's host, or
      // a game with it loaded one of whose actors stands by the trigger, so
      // a guest's press of a family trigger wakes the host's own actor and
      // any other guest's report is refused), and the actor's own
      // activation parents, read from its game-file record as
      // MpObjectReference::Init reads a non-actor's (its XAPR, which the
      // table skips for actors), name the trigger. A trigger the server
      // never loads (a marker, OnActivate's record path) has no cell the
      // server knows: refused by that name
      std::optional<uint32_t> casterCell;
      if (casterRef) {
        casterCell = partOne.worldState.CellOf(
          casterRef->GetCellOrWorld().ToFormId(partOne.worldState.espmFiles),
          casterRef->GetPos());
      }
      if (!casterCell) {
        spdlog::info("[verb:traps] E_TRAP_NO_CELL: {:x} reports {:x} "
                     "activating actor {:x}, its trigger's cell is unknown",
                     reporter.GetFormId(), casterId, targetId);
        return;
      }
      const auto firing = skymp::rules::trigger_firing_source(
        GatherFiringFacts(partOne, reporter, casterRef, *casterCell));
      if (firing.code != skymp::rules::TriggerFiringCode::Host &&
          firing.code != skymp::rules::TriggerFiringCode::Loader) {
        spdlog::info("[verb:traps] {}: {:x} reports {:x} activating actor "
                     "{:x}, its trigger in cell {:x}, {}",
                     std::string(firing.name), reporter.GetFormId(), casterId,
                     targetId, *casterCell, std::string(firing.why));
        return;
      }
      bool linked = false;
      if (partOne.HasEspm()) {
        try {
          const auto lookup =
            partOne.GetEspm().GetBrowser().LookupById(targetId);
          const auto data =
            espm::GetData<espm::ACHR>(targetId, &partOne.worldState);
          for (const auto& info : data.activationParents) {
            if (lookup.ToGlobalId(info.refrId) == casterId) {
              linked = true;
              break;
            }
          }
        } catch (std::exception&) {
          linked = false;
        }
      }
      if (!linked) {
        spdlog::info("[verb:traps] E_TRAP_UNLINKED: {:x} reports {:x} "
                     "activating actor {:x}, whose activation parents do not "
                     "name it",
                     reporter.GetFormId(), casterId, targetId);
        return;
      }
      if (!withinBudget()) {
        return;
      }
      RelayedActivationMessage relay;
      relay.target = targetActor->GetFormId();
      relay.caster = casterId;
      simulator->SendToUser(relay, true);
      spdlog::info("[verb:traps] {:x} reports {:x} activating actor {:x}: its "
                   "simulator {:x} is another seat's, relayed to it",
                   reporter.GetFormId(), casterId, targetId,
                   simulator->GetFormId());
      return;
    }
    spdlog::info("[verb:traps] {:x} reports {:x} activating actor {:x}: the "
                 "actor's simulator's (the NPC stream carries it), not "
                 "relayed",
                 reporter.GetFormId(), casterId, targetId);
    return;
  }
  if (!targetPtr) {
    // a target the server never loads (a marker) is no world object of the
    // server's; its own consequence arrives as its own linked activation
    spdlog::info("[verb:traps] E_TRAP_NO_REF: {:x} reports {:x} activating "
                 "{:x}, no such reference",
                 reporter.GetFormId(), casterId, targetId);
    return;
  }
  const auto cell = partOne.worldState.CellOf(
    targetPtr->GetCellOrWorld().ToFormId(partOne.worldState.espmFiles),
    targetPtr->GetPos());
  if (!cell) {
    spdlog::info("[verb:traps] E_TRAP_NO_CELL: {:x} reports {:x} activating "
                 "{:x}, whose cell is unknown",
                 reporter.GetFormId(), casterId, targetId);
    return;
  }
  // thuum ADR-068 D-W1 (wire-rules trigger_entry's firing_source): the pair
  // is its firing's owner's, the seat that owns the trigger's presser: the
  // cell's host as before, or a game with the cell loaded one of whose
  // actors (its player, or an actor it simulates) stands within the
  // trigger's activation reach plus the firing's slack. The server cannot
  // tell a trap family trigger's pair from a mechanism's (the family is the
  // platform's walk of the VM's class chain, and the server reads no script
  // archive in the lab), so a loader's pair is R2, the reporter's word that
  // its engine fired the trigger, bounded by the link below, the budget and
  // the reach, and every one taken is logged with its reporter. D-S3 phase
  // 2's player-only read of the trigger's VMAD retires into it: a
  // player-only trigger is a family trigger fired on its player's own seat.
  // A caster the server never loads (a marker) stands nowhere it can
  // measure, so only the host's word on it is taken
  const auto firing = skymp::rules::trigger_firing_source(
    GatherFiringFacts(partOne, reporter, casterRef, *cell));
  if (firing.code != skymp::rules::TriggerFiringCode::Host &&
      firing.code != skymp::rules::TriggerFiringCode::Loader) {
    spdlog::info("[verb:traps] {}: {:x} reports {:x} activating {:x} in cell "
                 "{:x}, {}",
                 std::string(firing.name), reporter.GetFormId(), casterId,
                 targetId, *cell, std::string(firing.why));
    return;
  }
  const auto children =
    partOne.worldState.activationChildsByActivationParent.find(casterId);
  if (children ==
        partOne.worldState.activationChildsByActivationParent.end() ||
      children->second.find(targetPtr->GetFormId()) ==
        children->second.end()) {
    spdlog::info("[verb:traps] E_TRAP_UNLINKED: {:x} reports {:x} activating "
                 "{:x}, whose activation parents do not name it",
                 reporter.GetFormId(), casterId, targetId);
    return;
  }
  if (!withinBudget()) {
    return;
  }
  NoteActivation(*targetPtr, std::chrono::steady_clock::now(), true);
  try {
    constexpr bool kDefaultProcessingOnlyFalse = false;
    constexpr bool kNotASecondActivation = false;
    constexpr bool kReportedByHost = true;
    if (casterRef) {
      targetPtr->Activate(*casterRef, kDefaultProcessingOnlyFalse,
                          kNotASecondActivation, kReportedByHost);
    }
  } catch (std::exception& e) {
    spdlog::info("[verb:traps] E_TRAP_ACTIVATE: {:x} reports {:x} activating "
                 "{:x}: {}",
                 reporter.GetFormId(), casterId, targetId, e.what());
    return;
  }
  RelayedActivationMessage relay;
  relay.target = targetPtr->GetFormId();
  relay.caster = casterId;
  size_t sent = 0;
  for (auto listener : targetPtr->GetActorListeners()) {
    if (listener == &reporter) {
      continue;
    }
    if (partOne.serverState.UserByActor(listener) !=
        Networking::InvalidUserId) {
      listener->SendToUser(relay, true);
      ++sent;
    }
  }
  if (firing.code == skymp::rules::TriggerFiringCode::Loader) {
    spdlog::info("[verb:traps] {:x} activates {:x} in cell {:x} ({:x} reports "
                 "with the cell loaded, its firing): relayed to {} game(s)",
                 casterId, targetPtr->GetFormId(), *cell, reporter.GetFormId(),
                 sent);
    return;
  }
  spdlog::info("[verb:traps] {:x} activates {:x} in cell {:x} (its host {:x} "
               "reports): relayed to {} game(s)",
               casterId, targetPtr->GetFormId(), *cell, reporter.GetFormId(),
               sent);
}

// thuum docs/verbs/locks.md (M4): a player's report on a lock. The rule
// (wire-rules locks) judges it against the record's lock, the activation
// reach (the rule an open passes), the Lockpicks the player holds and the
// gap since its last report on this lock; an accepted pick unlocks the
// record (the `lock` property to every listener), accepted broken picks
// leave the record's inventory, a refusal is logged by name.
void ActionListener::OnLockReport(const RawMessageData& rawMsgData,
                                  const LockReportMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!actor) {
    return;
  }
  const auto form = partOne.worldState.LookupFormByIdNoLoad(msg.refr);
  MpObjectReference* ref = form ? form->AsObjectReference() : nullptr;
  if (!ref) {
    spdlog::warn("[verb:locks] E_LOCK_NO_REF: {:x} reports on {:x}, no such "
                 "reference",
                 actor->GetFormId(), msg.refr);
    return;
  }
  const auto lock = ref->GetLock();
  if (!lock) {
    spdlog::info("[verb:locks] E_LOCK_OPEN: {:x} reports on {:x}, which has "
                 "no lock",
                 actor->GetFormId(), msg.refr);
    return;
  }
  bool withinReach = actor->GetCellOrWorld() == ref->GetCellOrWorld();
  if (withinReach) {
    const float distance = (actor->GetPos() - ref->GetPos()).Length();
    withinReach = skymp::rules::activation_within_reach(
                    distance, ActivationTargetSize(*ref))
                    .allowed;
  }
  const uint32_t picksHeld = actor->GetInventory().GetItemCount(kLockpick);
  const auto key = std::make_pair(actor->GetFormId(), ref->GetFormId());
  const uint64_t now = MonotonicMs();
  uint64_t sinceLast = UINT64_MAX;
  if (const auto it = lastLockReportMs.find(key);
      it != lastLockReportMs.end()) {
    sinceLast = now - it->second;
  }
  skymp::rules::LockStateRow row{};
  row.level = lock->level;
  row.key = lock->key;
  row.locked = lock->locked;
  row.broken = lock->broken;
  const auto verdict = skymp::rules::lock_report(
    row, static_cast<uint8_t>(std::min<uint32_t>(msg.outcome, 255)),
    static_cast<uint8_t>(std::min<uint32_t>(msg.broken, 255)), withinReach,
    picksHeld, sinceLast);
  if (!verdict.accepted) {
    static const char* const kWhy[] = { "accepted",       "E_LOCK_OPEN",
                                        "E_LOCK_REACH",   "E_LOCK_KEY",
                                        "E_LOCK_NO_PICK", "E_LOCK_RANGE",
                                        "E_LOCK_RATE" };
    spdlog::warn("[verb:locks] {}: {:x} reports outcome {} ({} broken) on "
                 "{:x} (level {}, {} picks held, reach {}); refused",
                 verdict.why < 7 ? kWhy[verdict.why] : "E_LOCK_UNKNOWN",
                 actor->GetFormId(), msg.outcome, msg.broken, ref->GetFormId(),
                 lock->level, picksHeld, withinReach);
    return;
  }
  lastLockReportMs[key] = now;
  MpChangeFormREFR::Lock after;
  after.level = verdict.state.level;
  after.key = verdict.state.key;
  after.locked = verdict.state.locked;
  after.broken = verdict.state.broken;
  switch (verdict.change) {
    case 1:
      spdlog::info("[verb:locks] {:x} picks {:x} open (level {})",
                   actor->GetFormId(), ref->GetFormId(), lock->level);
      ref->SetLock(after);
      break;
    case 2:
      spdlog::info("[verb:locks] {:x} breaks {} pick(s) on {:x} (level {}, "
                   "{} held)",
                   actor->GetFormId(), verdict.picks, ref->GetFormId(),
                   lock->level, picksHeld);
      actor->RemoveItem(kLockpick, verdict.picks, nullptr);
      ref->SetLock(after);
      break;
    default:
      spdlog::info("[verb:locks] {:x} leaves {:x} locked", actor->GetFormId(),
                   ref->GetFormId());
      break;
  }
}

// thuum D-S3 (traps follow the interaction; ADR-068's DestructionStage row;
// docs/research/trap-slice-d-s3-2026-11-05.md point 3): a seat's report of a
// reference's destruction stage rise or its destruction, which its engine
// caused or saw first. Every seat runs the trap family itself, so one firing
// is reported by more than one seat: the rule (wire-rules destruction) keeps
// the first report of each step (the stage only rises, destroyed only goes
// true) and an equal or a lower report changes nothing. Its checks first: a
// game-file reference the server holds or loads (LookupFormById: a trap's
// lamp may stand outside the chunks loaded around the players), no actor
// (an actor's death is the NPC fate's; an ACHR is refused by its record
// before the lookup could load it), a world body (IsWorldBodyKind: the
// lamp and the tripwire are ACTIs; a door, a container, furniture or an item
// is no trap's result, whatever a seat reports, the review of 9ff1895e's
// finding 1), and the reference's cell loaded in the reporter's game
// (PartOne::GetCellLoaders): the seat whose engine reached the step reports
// it, the cell's host or a guest alike (R2: the engine computed it, the
// server records it, nothing of it validated beyond these checks). A
// recorded step goes onto the reference's record (saved)
// and at once to its other listeners (served false); a late subscriber, or
// any after a restart, hears the record behind the reference's create word
// (PartOne's onSubscribe, served true).
void ActionListener::OnDestructionStage(const RawMessageData& rawMsgData,
                                        const DestructionStageMessage& msg)
{
  MpActor* reporter = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!reporter) {
    return;
  }
  const auto refuse = [&](const char* code, const std::string& why) {
    spdlog::info("[verb:traps] {}: {:x} reports {:x} at stage {}, destroyed "
                 "{}: {}",
                 code, reporter->GetFormId(), msg.refr, msg.stage,
                 msg.destroyed, why);
  };
  if (msg.refr == 0 || msg.refr >= 0xff000000) {
    // a server-made reference's destruction is the server's own (the edge
    // refuses the id first, E_VAL_RANGE)
    return refuse("E_DESTRUCTION_NO_REF", "not a game-file reference");
  }
  // an actor's record is refused by its type before any load (the review of
  // 9ff1895e, finding 4): the lookup below would load the NPC the id names,
  // one the server never held, for a report it then refuses
  // (OnLinkedActivation's targetIsActor reads the same type)
  if (partOne.HasEspm()) {
    const auto record = partOne.GetEspm().GetBrowser().LookupById(msg.refr);
    if (record.rec && record.rec->GetType() == espm::ACHR::kType) {
      return refuse("E_DESTRUCTION_ACTOR",
                    "an actor, whose death is the NPC fate's");
    }
  }
  auto refrPtr = std::dynamic_pointer_cast<MpObjectReference>(
    partOne.worldState.LookupFormById(msg.refr));
  if (!refrPtr) {
    return refuse("E_DESTRUCTION_NO_REF", "no such reference");
  }
  if (refrPtr->AsActor()) {
    return refuse("E_DESTRUCTION_ACTOR",
                  "an actor, whose death is the NPC fate's");
  }
  if (!IsWorldBodyKind(*refrPtr)) {
    return refuse("E_DESTRUCTION_KIND",
                  "not a trap or destructible world body");
  }
  const auto cell = partOne.worldState.CellOf(*refrPtr);
  if (!cell) {
    return refuse("E_DESTRUCTION_CELL", "the reference's cell is unknown");
  }
  const auto loaders = partOne.GetCellLoaders(*cell);
  if (std::find(loaders.begin(), loaders.end(), reporter->GetFormId()) ==
      loaders.end()) {
    return refuse("E_DESTRUCTION_CELL",
                  fmt::format("its game does not have the reference's cell "
                              "{:x} loaded",
                              *cell));
  }
  // the record, or the game files' state for a reference nobody reported
  const auto before =
    refrPtr->GetDestruction().value_or(MpChangeFormREFR::Destruction{});
  skymp::rules::DestructionRow record{};
  record.stage = before.stage;
  record.destroyed = before.destroyed;
  const auto verdict = skymp::rules::destruction_report(
    record, static_cast<uint8_t>(std::min<uint32_t>(msg.stage, 255)),
    msg.destroyed);
  if (verdict.verdict == 2) {
    // the edge refuses a stage past the bound first (E_VAL_RANGE)
    return refuse(
      "E_DESTRUCTION_RANGE",
      fmt::format("a stage past {}",
                  static_cast<int>(skymp::rules::destruction_max_stage())));
  }
  if (verdict.verdict != 0) {
    spdlog::info("[verb:traps] {:x} reports {:x} at stage {}, destroyed {}: "
                 "the record stands (stage {}, destroyed {}); dropped",
                 reporter->GetFormId(), msg.refr, msg.stage, msg.destroyed,
                 static_cast<int>(verdict.record.stage),
                 verdict.record.destroyed);
    return;
  }
  MpChangeFormREFR::Destruction after;
  after.stage = verdict.record.stage;
  after.destroyed = verdict.record.destroyed;
  refrPtr->SetDestruction(after);
  // thuum docs/verbs/traps.md (playtest 47): a recorded step of a trap or a
  // destructible (a lamp's fall, a pool's burn) is a hazard death's evidence
  // beside it, the trap's own record now that its pair is never reported
  NoteActivation(*refrPtr, std::chrono::steady_clock::now(), true);
  DestructionStageMessage relay;
  relay.refr = msg.refr;
  relay.stage = after.stage;
  relay.destroyed = after.destroyed;
  relay.served = false;
  size_t relayed = 0;
  for (auto listener : refrPtr->GetActorListeners()) {
    if (listener == reporter ||
        partOne.serverState.UserByActor(listener) ==
          Networking::InvalidUserId) {
      continue;
    }
    listener->SendToUser(relay, true);
    ++relayed;
  }
  spdlog::info("[verb:traps] {:x} reports {:x} at stage {}, destroyed {}: "
               "recorded (the first report), relayed to {} game(s)",
               reporter->GetFormId(), msg.refr, msg.stage, msg.destroyed,
               relayed);
}

// thuum ADR-068 rung 4 (AliasFill 79; amendment B, read 8; docs/research/
// rung4-state-rows-2026-11-07.md section 1(c)): a seat's own ForceRefTo or
// Clear of a quest's reference alias. The quest's record is read here (a
// QUST, a reference alias of that id among its ALST entries, libespm
// QUST::GetQuestAliases with ALLS set apart), the fill's form a REFR or an
// ACHR of the game files (or the clear), and the rule (wire-rules
// alias_fills) judges it against the journal's fills: the latest fill per
// (quest, alias) recorded, an equal one folded. A recorded fill goes at once
// to every other seat (served false), never the deferred queue.
void ActionListener::OnAliasFill(const RawMessageData& rawMsgData,
                                 const AliasFillMessage& msg)
{
  MpActor* reporter = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!reporter) {
    return;
  }
  if (!partOne.HasEspm()) {
    return spdlog::warn("OnAliasFill - no master files");
  }
  const auto& files = partOne.worldState.espmFiles;
  const auto& browser = partOne.GetEspm().GetBrowser();
  // thuum ADR-068 rung 5 (section 8(a)): a player-class quest's alias is its
  // player's own (the werewolf's Alias_Player), never the world's: refused
  // E_QUEST_PLAYER while the quest scope acts, nothing recorded or relayed
  try {
    if (partOne.CheckSharedQuest(FormDesc::FromFormId(msg.quest, files)) ==
        3) {
      RefuseStateRow(stateRowRefusals, reporter->GetFormId(), "E_QUEST_PLAYER",
                     fmt::format("[verb:quest-stage] E_QUEST_PLAYER: {:x} "
                                 "fills alias {:x}/{} with {:x}: a quest of "
                                 "the player class, each player's own",
                                 reporter->GetFormId(), msg.quest, msg.alias,
                                 msg.refr));
      return;
    }
  } catch (const std::exception&) {
    // a quest in no loaded plugin: the rule's E_ALIASFILL_ refusal says so
  }
  skymp::rules::AliasFillFacts facts{};
  const auto* qust =
    espm::Convert<espm::QUST>(browser.LookupById(msg.quest).rec);
  facts.is_quest = qust != nullptr;
  if (qust) {
    for (const auto& alias :
         qust->GetQuestAliases(partOne.worldState.GetEspmCache())) {
      if (!alias.locationAlias && alias.aliasID == msg.alias) {
        facts.has_alias = true;
        break;
      }
    }
  }
  if (msg.refr == 0) {
    facts.refr_is_ref = true;
  } else {
    const auto* refrRec = browser.LookupById(msg.refr).rec;
    facts.refr_is_ref = refrRec &&
      (refrRec->GetType() == "REFR" || refrRec->GetType() == "ACHR");
  }

  // the journal's fills as the rule reads them (the ref half's entries)
  auto fills = partOne.GetAliasFills();
  std::vector<skymp::rules::AliasFillEntry> record;
  for (const auto& fill : fills) {
    if (!fill.refr) {
      continue;
    }
    try {
      skymp::rules::AliasFillEntry entry{};
      entry.quest = fill.quest.ToFormId(files);
      entry.alias = fill.alias;
      entry.refr = IsClearedFill(*fill.refr) ? 0 : fill.refr->ToFormId(files);
      record.push_back(entry);
    } catch (const std::exception&) {
      // a fill whose plugin this load order lacks names nothing here
    }
  }
  skymp::rules::AliasFillEntry reported{};
  reported.quest = msg.quest;
  reported.alias = msg.alias;
  reported.refr = msg.refr;
  const auto verdict = skymp::rules::alias_fill_judge(
    rust::Slice<const skymp::rules::AliasFillEntry>(record.data(),
                                                    record.size()),
    reported, facts);
  if (verdict.code == 2) {
    RefuseStateRow(
      stateRowRefusals, reporter->GetFormId(), std::string(verdict.name),
      fmt::format("[verb:quest-stage] {}: {:x} fills alias "
                  "{:x}/{} with {:x}: {}",
                  std::string(verdict.name), reporter->GetFormId(), msg.quest,
                  msg.alias, msg.refr, std::string(verdict.why)));
    return;
  }
  if (verdict.code == 1) {
    return spdlog::debug("[verb:quest-stage] {:x} fills alias {:x}/{} with "
                         "{:x}: the record holds it already",
                         reporter->GetFormId(), msg.quest, msg.alias,
                         msg.refr);
  }

  // the journal's entry for (quest, alias) takes the fill, beside any
  // created item's mark it carries
  FormDesc questDesc;
  FormDesc refrDesc;
  try {
    questDesc = FormDesc::FromFormId(msg.quest, files);
    refrDesc = msg.refr ? FormDesc::FromFormId(msg.refr, files) : FormDesc();
  } catch (const std::exception& e) {
    return spdlog::warn("[verb:quest-stage] {:x} fills alias {:x}/{} with "
                        "{:x}: {}; not recorded",
                        reporter->GetFormId(), msg.quest, msg.alias, msg.refr,
                        e.what());
  }
  auto entry = std::find_if(fills.begin(), fills.end(), [&](const auto& f) {
    return f.quest == questDesc && f.alias == msg.alias;
  });
  if (entry == fills.end()) {
    MpChangeFormREFR::AliasFill fill;
    fill.quest = questDesc;
    fill.alias = msg.alias;
    fills.push_back(std::move(fill));
    entry = std::prev(fills.end());
  }
  entry->refr = refrDesc;
  partOne.SetAliasFills(std::move(fills));

  AliasFillMessage relay;
  relay.quest = msg.quest;
  relay.alias = msg.alias;
  relay.refr = msg.refr;
  relay.served = false;
  size_t relayed = 0;
  for (size_t i = 0, n = partOne.serverState.maxConnectedId; i <= n; ++i) {
    const auto userId = static_cast<Networking::UserId>(i);
    if (userId == rawMsgData.userId ||
        !partOne.serverState.ActorByUser(userId)) {
      continue;
    }
    partOne.GetSendTarget().Send(userId, relay, true);
    ++relayed;
  }
  spdlog::info("[verb:quest-stage] {:x} fills alias {:x}/{} with {:x}: "
               "recorded, relayed to {} game(s)",
               reporter->GetFormId(), msg.quest, msg.alias, msg.refr, relayed);
}

void ActionListener::SendAliasFills(MpActor& actor)
{
  const auto& files = partOne.worldState.espmFiles;
  size_t sent = 0;
  for (const auto& fill : partOne.GetAliasFills()) {
    // thuum ADR-068 rung 5: an older record's fill of a player-class quest
    // stays unsent, as its stages do
    if (!fill.refr || partOne.CheckSharedQuest(fill.quest) == 3) {
      continue;
    }
    AliasFillMessage message;
    try {
      message.quest = fill.quest.ToFormId(files);
      message.refr =
        IsClearedFill(*fill.refr) ? 0 : fill.refr->ToFormId(files);
    } catch (const std::exception&) {
      continue; // a fill whose plugin this load order lacks
    }
    message.alias = fill.alias;
    message.served = true;
    actor.SendToUser(message, true);
    ++sent;
  }
  if (sent > 0) {
    spdlog::info("[verb:quest-stage] {} alias fill(s) served to {:x} after "
                 "its login",
                 sent, actor.GetFormId());
  }
}

// thuum ADR-068 rung 4 (WorldGlobal 80; amendment B; docs/research/
// rung4-state-rows-2026-11-07.md section 1(c)): the world streamer's report.
// The whole message is the streamer's at its epoch or nothing (ADR-066
// decision 4, the epoch first); each entry is then judged alone against the
// game files (a GLOB, not constant) and the settings (not on the
// playerGlobals list), the clock's six by number (wire-rules world_globals),
// and the accepted entries merge into the journal's record, the latest value
// winning and an equal one folded; what moved goes at once to every other
// seat (served false).
void ActionListener::OnWorldGlobal(const RawMessageData& rawMsgData,
                                   const WorldGlobalMessage& msg)
{
  MpActor* reporter = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!reporter) {
    return;
  }
  const uint8_t judgement =
    partOne.JudgeWorldGlobalReport(reporter->GetFormId(), msg.epoch);
  if (judgement != 0) {
    const auto [streamer, epoch] = partOne.WorldStreamer();
    const uint8_t code = judgement == 3 ? 5 : 6;
    const std::string name(skymp::rules::world_global_code_name(code));
    std::string why;
    if (judgement == 1) {
      why = fmt::format("the stream stands at epoch {}, the report's is "
                        "behind it",
                        epoch);
    } else if (judgement == 2) {
      why = fmt::format("the stream stands at epoch {}, the report's is "
                        "ahead of it",
                        epoch);
    } else if (streamer == 0) {
      why = "nobody streams";
    } else {
      why = fmt::format("{:x} streams at epoch {}, not the reporter", streamer,
                        epoch);
    }
    RefuseStateRow(stateRowRefusals, reporter->GetFormId(), name,
                   fmt::format("[verb:world-globals] {}: {:x} reports {} "
                               "global(s) at epoch {}: {}",
                               name, reporter->GetFormId(), msg.entries.size(),
                               msg.epoch, why));
    return;
  }

  const auto watch = partOne.PlayerGlobalsWatch();
  std::vector<skymp::rules::WorldGlobalValue> accepted;
  std::map<std::string, size_t> refused;
  for (const auto& entry : msg.entries) {
    skymp::rules::WorldGlobalFacts facts{};
    facts.global = entry.global;
    if (partOne.HasEspm()) {
      const auto* rec =
        partOne.GetEspm().GetBrowser().LookupById(entry.global).rec;
      facts.is_glob = rec && rec->GetType() == "GLOB";
      facts.constant = rec && (rec->GetFlags() & kGlobalConstantFlag) != 0;
    }
    facts.player_listed =
      std::find(watch.begin(), watch.end(), entry.global) != watch.end();
    const uint8_t code = skymp::rules::world_global_entry(facts);
    if (code != 0) {
      ++refused[std::string(skymp::rules::world_global_code_name(code))];
      continue;
    }
    accepted.push_back({ entry.global, entry.value });
  }

  // the journal's record by form id; an entry whose plugin this load order
  // lacks stays as it is
  const auto& files = partOne.worldState.espmFiles;
  auto globals = partOne.GetWorldGlobals();
  std::vector<skymp::rules::WorldGlobalValue> record;
  for (const auto& global : globals) {
    try {
      record.push_back({ global.global.ToFormId(files), global.value });
    } catch (const std::exception&) {
    }
  }
  const auto merged = skymp::rules::world_globals_merge(
    rust::Slice<const skymp::rules::WorldGlobalValue>(record.data(),
                                                      record.size()),
    rust::Slice<const skymp::rules::WorldGlobalValue>(accepted.data(),
                                                      accepted.size()));
  WorldGlobalMessage relay;
  relay.epoch = msg.epoch;
  relay.served = false;
  for (const auto& moved : merged.changed) {
    FormDesc desc;
    try {
      desc = FormDesc::FromFormId(moved.global, files);
    } catch (const std::exception&) {
      continue;
    }
    auto it = std::find_if(globals.begin(), globals.end(),
                           [&](const auto& g) { return g.global == desc; });
    if (it == globals.end()) {
      globals.push_back({ desc, moved.value });
    } else {
      it->value = moved.value;
    }
    relay.entries.push_back({ moved.global, moved.value });
  }
  size_t relayed = 0;
  if (!relay.entries.empty()) {
    partOne.SetWorldGlobals(std::move(globals));
    for (size_t i = 0, n = partOne.serverState.maxConnectedId; i <= n; ++i) {
      const auto userId = static_cast<Networking::UserId>(i);
      if (userId == rawMsgData.userId ||
          !partOne.serverState.ActorByUser(userId)) {
        continue;
      }
      partOne.GetSendTarget().Send(userId, relay, true);
      ++relayed;
    }
  }
  std::string refusals;
  for (const auto& [name, count] : refused) {
    refusals += fmt::format(", {} x{}", name, count);
  }
  spdlog::info("[verb:world-globals] {:x} reports {} global(s) at epoch {}: "
               "{} recorded, relayed to {} game(s){}",
               reporter->GetFormId(), msg.entries.size(), msg.epoch,
               relay.entries.size(), relayed, refusals);
}

namespace {
// thuum ADR-068 rung 5: ScriptVariables 81's rows (wire-rules
// script_variables through the bridge) and the change forms' records
skymp::rules::ScriptVarRow ScriptVarRowOf(
  const MpChangeFormREFR::ScriptVariable& v, bool own)
{
  skymp::rules::ScriptVarRow row{};
  row.short_id = v.form.shortFormId;
  row.file = rust::String(v.form.file);
  row.script = rust::String(v.script);
  row.var = rust::String(v.var);
  row.own = own;
  row.k = static_cast<uint8_t>(v.k);
  row.i = v.i.value_or(0);
  row.f = v.f.value_or(0.f);
  row.b = v.b.value_or(false);
  row.seq = v.seq;
  return row;
}

std::vector<skymp::rules::ScriptVarRow> ScriptVarRows(
  const std::vector<MpChangeFormREFR::ScriptVariable>& record, bool own)
{
  std::vector<skymp::rules::ScriptVarRow> rows;
  rows.reserve(record.size());
  for (const auto& v : record) {
    rows.push_back(ScriptVarRowOf(v, own));
  }
  return rows;
}

template <class Rows>
std::vector<MpChangeFormREFR::ScriptVariable> ScriptVariablesOf(
  const Rows& rows)
{
  std::vector<MpChangeFormREFR::ScriptVariable> record;
  record.reserve(rows.size());
  for (const auto& row : rows) {
    MpChangeFormREFR::ScriptVariable v;
    v.form = FormDesc(row.short_id, std::string(row.file));
    v.script = std::string(row.script);
    v.var = std::string(row.var);
    v.k = row.k;
    if (row.k == 3) {
      v.i = row.i;
    } else if (row.k == 4) {
      v.f = row.f;
    } else if (row.k == 5) {
      v.b = row.b;
    }
    v.seq = row.seq;
    record.push_back(std::move(v));
  }
  return record;
}

// a recorded row as the message's entry, its form in this load order
ScriptVariablesMessage::Entry EntryOfRow(const skymp::rules::ScriptVarRow& row,
                                         uint32_t formId)
{
  ScriptVariablesMessage::Entry e;
  e.form = formId;
  e.script = std::string(row.script);
  e.var = std::string(row.var);
  e.own = row.own;
  e.k = row.k;
  if (row.k == 3) {
    e.i = row.i;
  } else if (row.k == 4) {
    e.f = row.f;
  } else if (row.k == 5) {
    e.b = row.b;
  }
  e.seq = row.seq;
  return e;
}

// the wire's cap on one message's keys and its entries (wire-schema
// cap::SCRIPT_VARIABLES)
constexpr size_t kScriptVariablesPerMessage = 256;

// D-T2 B's two reserved names of a form's game-time registration (the
// platform's kGameTimeHours and kGameTimeRepeat), compared without case as
// the platform compares them
bool IsGameTimeName(const std::string& var)
{
  const auto same = [&](const char* name) {
    const std::string n(name);
    return var.size() == n.size() &&
      std::equal(var.begin(), var.end(), n.begin(), [](char a, char b) {
             return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
           });
  };
  return same("<OnUpdateGameTime>") || same("<OnUpdateGameTime:repeat>");
}

// the served entries in messages of at most kScriptVariablesPerMessage, in
// order, a form's two registration entries side by side and never split
// across two messages (the client reads them as one registration)
std::vector<std::vector<ScriptVariablesMessage::Entry>> ServedChunks(
  std::vector<ScriptVariablesMessage::Entry> entries)
{
  std::vector<ScriptVariablesMessage::Entry> ordered;
  ordered.reserve(entries.size());
  std::vector<bool> placed(entries.size(), false);
  for (size_t i = 0; i < entries.size(); ++i) {
    if (placed[i]) {
      continue;
    }
    placed[i] = true;
    ordered.push_back(entries[i]);
    if (!IsGameTimeName(entries[i].var)) {
      continue;
    }
    for (size_t j = i + 1; j < entries.size(); ++j) {
      if (!placed[j] && entries[j].form == entries[i].form &&
          IsGameTimeName(entries[j].var)) {
        placed[j] = true;
        ordered.push_back(entries[j]);
        break;
      }
    }
  }
  std::vector<std::vector<ScriptVariablesMessage::Entry>> chunks;
  for (size_t i = 0; i < ordered.size(); ++i) {
    const bool pairStarts = IsGameTimeName(ordered[i].var) &&
      i + 1 < ordered.size() && ordered[i + 1].form == ordered[i].form &&
      IsGameTimeName(ordered[i + 1].var);
    const size_t room = pairStarts ? 2 : 1;
    if (chunks.empty() ||
        chunks.back().size() + room > kScriptVariablesPerMessage) {
      chunks.emplace_back();
    }
    chunks.back().push_back(ordered[i]);
    if (pairStarts) {
      chunks.back().push_back(ordered[++i]);
    }
  }
  return chunks;
}
}

// thuum ADR-068 rung 5 (ScriptVariables 81; D-V1 B and D-T2 B; docs/
// research/rung5-quests-2026-11-09.md sections 1(b) and 8(b)): a seat's
// report of the condition-read variables its game changed. Any seat may
// report (a topic info's fragment runs on the talking seat alone): each
// entry is judged by wire-rules script_variables against its record, the
// journal's for a world key, the reporter's own player's change form for
// one flagged own (the player cut: a player-class quest's key, its scalar
// variables and its game-time registration). A world key's write is
// recorded as the latest by the server's order with the key's next
// sequence and relayed at once to every other seat (served false); an own
// key's is recorded on the player's change form and relayed to nobody. An
// equal value folds. R2: the reporting engine's value, recorded, never
// judged. Refusals by name: E_SCRIPTVAR_UNLISTED, _PLAYER, _KIND, _FULL, one
// line a minute per seat and code, every report's counts in its line.
void ActionListener::OnScriptVariables(const RawMessageData& rawMsgData,
                                       const ScriptVariablesMessage& msg)
{
  MpActor* reporter = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!reporter) {
    return;
  }
  if (!msg.watch.empty() || msg.served) {
    return spdlog::info("ScriptVariables: user {} actor {:x} report refused "
                        "(the watch and the served record are the server's)",
                        rawMsgData.userId, reporter->GetFormId());
  }
  auto& rules = partOne.GetScriptVariableRules();
  const auto& files = partOne.worldState.espmFiles;
  ScriptVariablesMessage relay;
  relay.served = false;
  size_t ownRecorded = 0;
  size_t folded = 0;
  std::map<std::string, size_t> refused;
  // the entries whose forms this load order names, judged whole in one
  // bridge call (the fresh review's note 13), each against its record as
  // the entries before it left it
  std::vector<skymp::rules::ScriptVarRow> rows;
  std::vector<size_t> rowEntry;
  rows.reserve(msg.entries.size());
  rowEntry.reserve(msg.entries.size());
  for (size_t i = 0; i < msg.entries.size(); ++i) {
    const auto& entry = msg.entries[i];
    skymp::rules::ScriptVarRow row{};
    try {
      const FormDesc form = FormDesc::FromFormId(entry.form, files);
      row.short_id = form.shortFormId;
      row.file = rust::String(form.file);
    } catch (const std::exception&) {
      ++refused["E_SCRIPTVAR_UNLISTED"]; // no plugin of this load order
      continue;
    }
    row.script = rust::String(entry.script);
    row.var = rust::String(entry.var);
    row.own = entry.own;
    row.k = static_cast<uint8_t>(entry.k);
    row.i = entry.i.value_or(0);
    row.f = entry.f.value_or(0.f);
    row.b = entry.b.value_or(false);
    row.seq = 0;
    rows.push_back(std::move(row));
    rowEntry.push_back(i);
  }
  const auto world = ScriptVarRows(partOne.GetScriptVariables(), false);
  const auto own = ScriptVarRows(reporter->GetScriptVariables(), true);
  const auto report = rules.script_variables_report(
    rust::Slice<const skymp::rules::ScriptVarRow>(world.data(), world.size()),
    rust::Slice<const skymp::rules::ScriptVarRow>(own.data(), own.size()),
    rust::Slice<const skymp::rules::ScriptVarRow>(rows.data(), rows.size()));
  for (size_t r = 0; r < rows.size() && r < report.verdicts.size(); ++r) {
    const auto& verdict = report.verdicts[r];
    const auto& entry = msg.entries[rowEntry[r]];
    if (verdict.code == 1) {
      ++folded;
      continue;
    }
    if (verdict.code != 0) {
      const std::string name(verdict.name);
      ++refused[name];
      uint32_t since = 0;
      if (QuestLineDue(questLines, reporter->GetFormId(), name, since)) {
        spdlog::info("{}: {:x} reports script variable {:x} {}.{}: {}; "
                     "refused, {} since the last line",
                     name, reporter->GetFormId(), entry.form, entry.script,
                     entry.var, std::string(verdict.why), since);
      }
      continue;
    }
    if (entry.own) {
      ++ownRecorded;
      continue;
    }
    auto out = entry;
    out.seq = r < report.seqs.size() ? report.seqs[r] : 0;
    relay.entries.push_back(std::move(out));
  }
  if (ownRecorded > 0) {
    reporter->SetScriptVariables(ScriptVariablesOf(report.own));
  }
  size_t relayed = 0;
  if (!relay.entries.empty()) {
    partOne.SetScriptVariables(ScriptVariablesOf(report.world));
    for (size_t i = 0, n = partOne.serverState.maxConnectedId; i <= n; ++i) {
      const auto userId = static_cast<Networking::UserId>(i);
      if (userId == rawMsgData.userId ||
          !partOne.serverState.ActorByUser(userId)) {
        continue;
      }
      partOne.GetSendTarget().Send(userId, relay, true);
      ++relayed;
    }
  }
  std::string refusals;
  for (const auto& [name, count] : refused) {
    refusals += fmt::format(", {} x{}", name, count);
  }
  spdlog::info("[verb:quest-stage] {:x} reports {} script variable(s): {} "
               "recorded, {} of its own, {} folded, relayed to {} game(s){}",
               reporter->GetFormId(), msg.entries.size(), relay.entries.size(),
               ownRecorded, folded, relayed, refusals);
}

void ActionListener::SendScriptVariables(MpActor& actor)
{
  auto& rules = partOne.GetScriptVariableRules();
  const auto& files = partOne.worldState.espmFiles;
  std::vector<ScriptVariablesMessage::Key> watch;
  size_t ownWatched = 0;
  for (const auto& key : rules.script_variables_watch()) {
    ScriptVariablesMessage::Key k;
    try {
      k.form = FormDesc(key.short_id, std::string(key.file)).ToFormId(files);
    } catch (const std::exception&) {
      continue; // a listed key whose plugin this load order lacks
    }
    k.script = std::string(key.script);
    k.var = std::string(key.var);
    k.own = key.own;
    ownWatched += key.own ? 1 : 0;
    watch.push_back(std::move(k));
  }
  const auto served = [&](const std::vector<skymp::rules::ScriptVarRow>& rows,
                          bool own) {
    std::vector<ScriptVariablesMessage::Entry> entries;
    for (const auto& row : rules.script_variables_at_login(
           rust::Slice<const skymp::rules::ScriptVarRow>(rows.data(),
                                                         rows.size()),
           own)) {
      try {
        entries.push_back(EntryOfRow(
          row, FormDesc(row.short_id, std::string(row.file)).ToFormId(files)));
      } catch (const std::exception&) {
        // a recorded key whose plugin this load order lacks
      }
    }
    return entries;
  };
  const auto world =
    served(ScriptVarRows(partOne.GetScriptVariables(), false), false);
  const auto own =
    served(ScriptVarRows(actor.GetScriptVariables(), true), true);
  if (watch.empty() && world.empty() && own.empty()) {
    return;
  }
  size_t messages = 0;
  for (size_t at = 0; at < watch.size(); at += kScriptVariablesPerMessage) {
    ScriptVariablesMessage message;
    message.served = true;
    const size_t end = std::min(watch.size(), at + kScriptVariablesPerMessage);
    message.watch.assign(watch.begin() + at, watch.begin() + end);
    actor.SendToUser(message, true);
    ++messages;
  }
  for (const auto* half : { &world, &own }) {
    for (auto& chunk : ServedChunks(*half)) {
      ScriptVariablesMessage message;
      message.served = true;
      message.entries = std::move(chunk);
      actor.SendToUser(message, true);
      ++messages;
    }
  }
  spdlog::info("[verb:quest-stage] script variables served to {:x} after its "
               "login: {} key(s) to watch ({} its own), {} world value(s), {} "
               "of its own, in {} message(s)",
               actor.GetFormId(), watch.size(), ownWatched, world.size(),
               own.size(), messages);
}

void ActionListener::SendWorldGlobals(MpActor& actor)
{
  if (!partOne.WorldGlobalsEnabled()) {
    return;
  }
  const auto& files = partOne.worldState.espmFiles;
  std::vector<WorldGlobalMessage::Entry> entries;
  for (const auto& global : partOne.GetWorldGlobals()) {
    try {
      entries.push_back({ global.global.ToFormId(files), global.value });
    } catch (const std::exception&) {
      // a recorded global whose plugin this load order lacks
    }
  }
  if (entries.empty()) {
    return;
  }
  const uint32_t epoch = partOne.WorldStreamer().second;
  for (size_t at = 0; at < entries.size(); at += kWorldGlobalsPerMessage) {
    WorldGlobalMessage message;
    message.epoch = epoch;
    message.served = true;
    const size_t end = std::min(entries.size(), at + kWorldGlobalsPerMessage);
    message.entries.assign(entries.begin() + at, entries.begin() + end);
    actor.SendToUser(message, true);
  }
  spdlog::info("[verb:world-globals] {} world global(s) served to {:x} after "
               "its login, epoch {}",
               entries.size(), actor.GetFormId(), epoch);
}

// thuum docs/verbs/prop-physics.md (ADR-058 decision 2): a simulator's
// sample of a prop. The record takes it from the prop's owner alone (the
// toucher while awake or inside its lease, else the host of the cell the
// RECORD stands in; wire-rules props), within the rule's step budget against
// the record (a seat cannot carry a prop across the world in one message),
// a trap's world body within kWorldBodyTether of its game-file position (nor
// carry one off over many: E_PROP_FAR, thuum D-S3 point 4's review),
// landing in the cell the sample's position names in the prop's own world or
// cell (HYPOTHESIS: a prop never changes world or interior by physics, so
// the record's worldOrCell stands; a kick through a load door is a later
// read); then the same bytes go to the prop's other listeners, unreliable
// while awake and reliable at sleep (the record's final sample). The write
// is the movement path's (OnUpdateMovement's SetPos and SetAngle): saved
// from time to time while the body moves, at once when it rests. A refusal
// is logged once per 10 s per seat: a seat that lost the lease keeps
// streaming until its own timer ends.
void ActionListener::OnPropTransform(const RawMessageData& rawMsgData,
                                     const PropTransformMessage& msg)
{
  MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!myActor) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  static std::unordered_map<Networking::UserId,
                            std::chrono::steady_clock::time_point>
    lastRefusal;
  const auto refuse = [&](const char* code, const char* why) {
    partOne.serverState.CountStream(
      rawMsgData.userId, static_cast<uint8_t>(MsgType::PropTransform), false);
    auto& last = lastRefusal[rawMsgData.userId];
    if (last == std::chrono::steady_clock::time_point() ||
        now - last >= std::chrono::seconds(10)) {
      spdlog::warn("[verb:prop-physics] {}: {:x} moves prop {:x}; {}", code,
                   myActor->GetFormId(), msg.refr, why);
      last = now;
    }
  };
  const auto& form = partOne.worldState.LookupFormByIdNoLoad(msg.refr);
  MpObjectReference* prop = form ? form->AsObjectReference() : nullptr;
  if (!prop || prop->AsActor()) {
    return refuse("E_PROP_UNKNOWN", "no such reference, or an actor");
  }
  const bool worldBody = IsWorldBodyKind(*prop);
  if (!IsPropKind(*prop) && !worldBody) {
    return refuse("E_PROP_KIND", "not an item reference");
  }
  // thuum D-S3 point 4: a trap's world body is on the stream once an impulse
  // claimed it (its epoch above 0: OnPropClaim admits no other touch on it,
  // and found it loaded), its samples then judged as an item's
  if (worldBody && partOne.PropEpochOf(*prop) == 0) {
    return refuse("E_PROP_KIND", "a world body no impulse claimed");
  }
  // the Rust edge refuses a non-finite transform (wire-validate); the legacy
  // path parses JSON, where 1e999 reads as infinity, so the check stands
  // here too before any arithmetic on it
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(msg.pos[i]) || !std::isfinite(msg.rot[i])) {
      return refuse("E_PROP_RANGE", "a transform that is not a number");
    }
  }
  const NiPoint3 pos{ msg.pos[0], msg.pos[1], msg.pos[2] };
  const NiPoint3 rot{ msg.rot[0], msg.rot[1], msg.rot[2] };
  // thuum D-S3 point 4 (the review of 9ff1895e, finding 2): a world body's
  // sample lands within kWorldBodyTether of the body's game-file position
  // (its REFR record's, PlacementOfRecord), whoever owns it, before the rule
  // judges or spends anything; one with no game-file placement (no REFR
  // record of the load order: a server-made reference) has nothing to be
  // held to and takes no sample
  if (worldBody) {
    const auto placement =
      PlacementOfRecord(partOne.worldState, prop->GetFormId());
    if (!placement) {
      return refuse("E_PROP_FAR", "a world body with no game-file position");
    }
    if ((pos - placement->pos).Length() > kWorldBodyTether) {
      return refuse("E_PROP_FAR",
                    "farther from its game-file position than the tether "
                    "allows");
    }
  }
  const uint32_t worldOrCell =
    prop->GetCellOrWorld().ToFormId(partOne.worldState.espmFiles);
  const auto cell = partOne.worldState.CellOf(worldOrCell, pos);
  const auto recordCell = partOne.worldState.CellOf(*prop);
  const float moved = (pos - prop->GetPos()).Length();
  const auto verdict =
    partOne.PropTransform(*myActor, *prop, cell.value_or(0),
                          recordCell.value_or(0), msg.asleep, moved);
  switch (verdict) {
    case PartOne::PropTransformVerdict::Accepted: {
      const auto mode =
        msg.asleep ? SetPosMode::Other : SetPosMode::CalledByUpdateMovement;
      prop->SetPos(pos, mode);
      prop->SetAngle(rot, mode);
      size_t sent = 0;
      for (auto listener : prop->GetActorListeners()) {
        if (listener == myActor) {
          continue;
        }
        const auto targetUserId = partOne.serverState.UserByActor(listener);
        if (targetUserId != Networking::InvalidUserId) {
          partOne.GetSendTarget().Send(targetUserId, rawMsgData.unparsed,
                                       rawMsgData.unparsedLength, msg.asleep);
          ++sent;
        }
      }
      partOne.serverState.CountStream(
        rawMsgData.userId, static_cast<uint8_t>(MsgType::PropTransform), true);
      spdlog::trace(
        "[verb:prop-physics] prop {:x} by {:x} at ({:.0f}, {:.0f}, "
        "{:.0f}){} in cell {:x}; relayed to {} listener(s)",
        prop->GetFormId(), myActor->GetFormId(), pos.x, pos.y, pos.z,
        msg.asleep ? ", asleep" : "", cell.value_or(0), sent);
      break;
    }
    case PartOne::PropTransformVerdict::NotOwner:
      refuse("E_PROP_NOT_OWNER", "another seat simulates it");
      break;
    case PartOne::PropTransformVerdict::WrongCell:
      refuse("E_PROP_CELL",
             "no cell at the point, or a cell nobody hosts and nobody holds");
      break;
    case PartOne::PropTransformVerdict::TooFast:
      refuse("E_PROP_STEP",
             "farther from the record than the step budget allows");
      break;
  }
}

void ActionListener::OnChangeForm(const RawMessageData& rawMsgData,
                                  const ChangeFormMessage& msg)
{
  MpActor* sender = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!sender) {
    spdlog::warn("ActionListener::OnChangeForm - user {} has no actor; its "
                 "record for {:x} ignored",
                 rawMsgData.userId, msg.formId);
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  // one warn line a minute per sender and form, the rest counted
  // (OnActorDeath's shape): a host reports a record at most once a frame per
  // actor, and a wrong premise would repeat it at every change
  const auto refuse = [&](const char* code, const std::string& why,
                          bool foreign) {
    static std::map<std::pair<uint32_t, uint32_t>,
                    std::pair<std::chrono::steady_clock::time_point, uint32_t>>
      refusals;
    auto& [since, count] = refusals[{ sender->GetFormId(), msg.formId }];
    ++count;
    partOne.serverState.CountStream(rawMsgData.userId,
                                    static_cast<uint8_t>(MsgType::ChangeForm),
                                    false, foreign);
    if (since.time_since_epoch().count() != 0 &&
        now - since < std::chrono::minutes(1)) {
      return;
    }
    spdlog::warn("[verb:change-form] {}: {:x} reports {:x} (type {}, flags "
                 "{:#x}, {} bytes, fnv {:08x}, epoch {}): {}; refused, {} "
                 "since the last line",
                 code, sender->GetFormId(), msg.formId, msg.formType,
                 msg.flags, msg.body.size() / 2, msg.fnv1a, msg.epoch, why,
                 count);
    since = now;
    count = 0;
  };
  // the wire refused these shapes already (wire-validate check_change_form);
  // the core keeps the two it stores by, so a record it holds is never a
  // player's or empty
  if (msg.formId == 0 || msg.formId >= 0xff000000) {
    refuse("E_CHANGEFORM_RANGE",
           "a form id of 0 or a player's: a change form names a game-file "
           "actor alone",
           false);
    return;
  }
  if (msg.body.size() < 8 || msg.body.size() % 2 != 0) {
    refuse("E_CHANGEFORM_SIZE", "a body too short for any record header",
           false);
    return;
  }
  // no load: an actor the server never held is nobody's to report
  auto& form = partOne.worldState.LookupFormByIdNoLoad(msg.formId);
  MpActor* actor = form ? form->AsActor() : nullptr;
  if (!actor) {
    refuse("E_CHANGEFORM_RANGE", "the server holds no such actor", false);
    return;
  }
  const auto cell = partOne.worldState.CellOf(*actor);
  if (!cell) {
    refuse("E_CHANGEFORM_OWNER", "the actor's cell is unknown to the server",
           true);
    return;
  }
  const auto host = partOne.GetCellHost(*cell);
  if (!host.hosted || host.player != sender->GetFormId()) {
    refuse("E_CHANGEFORM_OWNER",
           host.hosted ? fmt::format("{:x} hosts cell {:x}, not the reporter",
                                     host.player, *cell)
                       : fmt::format("nobody hosts cell {:x}", *cell),
           true);
    return;
  }
  if (msg.epoch != host.epoch) {
    refuse("E_CHANGEFORM_EPOCH",
           fmt::format("cell {:x} stands at epoch {}, the report names {}",
                       *cell, host.epoch, msg.epoch),
           true);
    return;
  }

  ChangeFormRecord record;
  record.epoch = msg.epoch;
  record.flags = msg.flags;
  record.version = static_cast<uint8_t>(msg.version);
  record.formType = static_cast<uint8_t>(msg.formType);
  record.bodyHex = msg.body;
  record.fnv1a = msg.fnv1a;
  record.remap = msg.remap;
  record.sender = sender->GetFormId();
  record.atMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch())
      .count());
  actor->SetChangeFormRecord(std::move(record));
  partOne.serverState.CountStream(
    rawMsgData.userId, static_cast<uint8_t>(MsgType::ChangeForm), true);

  // the receipt to the owner: its compare baseline (amendment A)
  ChangeFormAckMessage ack;
  ack.formId = msg.formId;
  ack.fnv1a = msg.fnv1a;
  sender->SendToUser(ack, true);

  // the record at once to every other game listening to the actor (the
  // watcher present at the death); a seat that subscribes or loads the cell
  // later hears it with the create word or at the load (PartOne)
  size_t served = 0;
  for (MpActor* listener : actor->GetActorListeners()) {
    if (listener == sender ||
        partOne.serverState.UserByActor(listener) ==
          Networking::InvalidUserId) {
      continue;
    }
    actor->SendChangeFormRecordTo(*listener);
    ++served;
  }
  // one info line a minute per form with the count, the rest at debug: a
  // moving actor's host reports at every changed frame
  static std::unordered_map<
    uint32_t, std::pair<std::chrono::steady_clock::time_point, uint32_t>>
    accepted;
  auto& [since, count] = accepted[msg.formId];
  ++count;
  if (since.time_since_epoch().count() == 0 ||
      now - since >= std::chrono::minutes(1)) {
    spdlog::info("[verb:change-form] recorded {:x} from {:x}: type {}, flags "
                 "{:#x}, {} bytes, fnv {:08x}, {} remap row(s), epoch {}, "
                 "served to {} listener(s); {} record(s) since the last line",
                 msg.formId, sender->GetFormId(), msg.formType, msg.flags,
                 msg.body.size() / 2, msg.fnv1a, msg.remap.size(), msg.epoch,
                 served, count);
    since = now;
    count = 0;
  } else {
    spdlog::debug("[verb:change-form] recorded {:x} from {:x}: flags {:#x}, "
                  "{} bytes, fnv {:08x}, served to {}",
                  msg.formId, sender->GetFormId(), msg.flags,
                  msg.body.size() / 2, msg.fnv1a, served);
  }
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
// thuum docs/verbs/traps.md (playtest 47, 2026-11-06): a trap stays deadly
// after its report (a swinging blade's loop, a pool's burn over its stages),
// and since ADR-068 D-S3 a family pair is fired on every seat and never
// reported, so a trap's evidence is its trigger's own firing, the trigger's
// activation children, a host's linked report or a destruction record, kept
// this long (UNMEASURED; 47's blade kill came 17.8 s after its trigger's
// report, E_DEATH_UNCHECKED on 9bcb9)
constexpr auto kDeathTrapWindow = std::chrono::milliseconds(30000);
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
                                    std::chrono::steady_clock::time_point now,
                                    bool trap)
{
  constexpr size_t kActivationsKept = 256;
  recentActivations.push_back(RecentActivation{
    now, target.GetCellOrWorld(), target.GetPos(), target.GetFormId(), trap });
  while (recentActivations.size() > kActivationsKept ||
         (!recentActivations.empty() &&
          now - recentActivations.front().at > kDeathTrapWindow)) {
    recentActivations.pop_front();
  }
}

// a trap's hit on an actor its simulator reports (OnScriptCall's kept
// ProcessTrapHit), kept beside the trap notes for the same kDeathTrapWindow
// and bounded as they are
void ActionListener::NoteTrapHit(const MpActor& actor, uint32_t trapId,
                                 std::chrono::steady_clock::time_point now)
{
  constexpr size_t kTrapHitsKept = 256;
  recentTrapHits.push_back(RecentTrapHit{ now, actor.GetFormId(), trapId });
  while (recentTrapHits.size() > kTrapHitsKept ||
         (!recentTrapHits.empty() &&
          now - recentTrapHits.front().at > kDeathTrapWindow)) {
    recentTrapHits.pop_front();
  }
}

std::optional<ActionListener::RecentTrapHit> ActionListener::TrapHitOn(
  const MpActor& actor, std::chrono::steady_clock::time_point now) const
{
  std::optional<RecentTrapHit> newest;
  for (const auto& hit : recentTrapHits) {
    if (hit.actorId != actor.GetFormId() || now - hit.at > kDeathTrapWindow) {
      continue;
    }
    if (!newest || hit.at > newest->at) {
      newest = hit;
    }
  }
  return newest;
}

// a trigger's own firing, and where it lands: the trigger's activation
// children and theirs (a plate's wall, a box's blades, a wire's linker and
// its rocks), the references the game files name with the trigger as their
// activate parent (WorldState::activationChildsByActivationParent, filled
// as the server loads them); two levels, bounded
void ActionListener::NoteTrapFiring(const MpObjectReference& trigger,
                                    std::chrono::steady_clock::time_point now)
{
  constexpr size_t kChildrenNoted = 32;
  NoteActivation(trigger, now, true);
  auto& children = partOne.worldState.activationChildsByActivationParent;
  std::vector<uint32_t> level{ trigger.GetFormId() };
  size_t noted = 0;
  for (int depth = 0; depth < 2 && noted < kChildrenNoted; ++depth) {
    std::vector<uint32_t> next;
    for (const uint32_t parent : level) {
      const auto it = children.find(parent);
      if (it == children.end()) {
        continue;
      }
      for (const auto& entry : it->second) {
        const uint32_t childId = entry.first;
        auto& childForm = partOne.worldState.LookupFormByIdNoLoad(childId);
        MpObjectReference* child =
          childForm ? childForm->AsObjectReference() : nullptr;
        if (!child || noted >= kChildrenNoted) {
          continue;
        }
        NoteActivation(*child, now, true);
        next.push_back(childId);
        ++noted;
      }
    }
    level = std::move(next);
  }
}

std::optional<ActionListener::RecentActivation>
ActionListener::ActivationSeenNear(
  const MpObjectReference& where, const NiPoint3& pos,
  std::chrono::steady_clock::time_point now) const
{
  std::optional<RecentActivation> newest;
  for (const auto& activation : recentActivations) {
    const auto window =
      activation.trap ? kDeathTrapWindow : kDeathHazardWindow;
    if (now - activation.at > window) {
      continue;
    }
    if (activation.cellOrWorld != where.GetCellOrWorld()) {
      continue;
    }
    if ((activation.pos - pos).Length() <= kDeathHazardUnits) {
      newest = activation;
    }
  }
  return newest;
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
  // the host's reliable report of the death is its dead-sample stamp too,
  // already dead here or not (the hosted state's zero is sent once,
  // unreliable, then suppressed as unchanged: numidium's review of round
  // seven, item 4b)
  actor->NoteHostSampledDead(reporter->GetFormId(),
                             reporter->LoginGeneration(),
                             std::chrono::steady_clock::now());
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
  // thuum docs/DECISIONS.md ADR-062 decision 2 and ADR-063 decision 4: a
  // host-run script's kill (cause 3) is the host's word, R2; the engine's
  // essential rule (KillEssential clears the flag before its Kill, the
  // server's base still reads it) and the body's distance to a record the
  // host itself streams are checks on the owner, not on consistency, so
  // they do not apply to it (npc-fate.md's Authority row)
  const bool scripted = msg.cause == 3;
  if (!scripted && !actor->MayDie(killer)) {
    refuse("E_DEATH_IMMORTAL",
           "an essential or protected base (the engine's rule, "
           "MpActor::MayDie)");
    return;
  }
  const float away = (pos - actor->GetPos()).Length();
  if (!scripted && !(away <= kDeathBodyUnits)) {
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
  const std::optional<RecentActivation> hazard =
    ActivationSeenNear(*actor, pos, now);
  const bool hazardSeen = hazard.has_value();
  // thuum docs/verbs/npc-fate.md (playtest 48): its host's word that a trap
  // hit this very actor within the trap window, whenever the trap was set
  // off (NoteTrapHit); read last of an unnamed cause's evidence (after the
  // place's notes and the reanimation's end), so a death today's rules
  // explain keeps their line. The trap it names is recorded as reported,
  // never checked against the body's place (R2, unvalidated)
  const std::optional<RecentTrapHit> trapHit = TrapHitOn(*actor, now);
  const auto trapHitEvidence = [&]() {
    const auto ago =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - trapHit->at)
        .count();
    return fmt::format("a trap's evidence beside the body ({:x} hit it {} ms "
                       "before, its host's word)",
                       trapHit->trapId, ago);
  };
  const auto hazardEvidence = [&]() {
    const auto ago =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - hazard->at)
        .count();
    if (hazard->trap) {
      return fmt::format("a trap's evidence beside the body ({:x}, {} ms "
                         "before)",
                         hazard->refId, ago);
    }
    return fmt::format("an activation beside the body ({:x}, {} ms before)",
                       hazard->refId, ago);
  };
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
      if (!hazardSeen && !trapHit) {
        refuse("E_DEATH_UNCHECKED",
               fmt::format("no activation within {:.0f} units in {} ms, nor "
                           "a trap's evidence in {} ms",
                           kDeathHazardUnits, kDeathHazardWindow.count(),
                           kDeathTrapWindow.count()));
        return;
      }
      evidence = hazardSeen ? hazardEvidence() : trapHitEvidence();
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
        evidence = hazardEvidence();
      } else if (partOne.CommanderOf(*actor) == reporter->GetFormId()) {
        // thuum docs/verbs/npc-fate.md (ADR-060; Eli, 2026-11-12: "summons
        // and resurrections should obey the timers the level of spell"): a
        // commanded thrall dies when its commander's engine ends the effect,
        // on that engine's own timer (dual casting, perks, the spell's
        // level), which no window of the server's bounds; its commander's
        // report of the death is the effect's end, at any time (R2,
        // recorded: the thrall is the commander's, ADR-063)
        evidence = "the command's end: its commander's engine ended the "
                   "effect (ADR-060)";
      } else if (const auto stoodUp = actor->HostStoodUpAgo(now); stoodUp &&
                 actor->ReanimateAcceptedAgo(now) &&
                 stoodUp->count() <=
                   static_cast<int64_t>(MpActor::kReanimatedEndMs)) {
        // thuum docs/verbs/npc-fate.md (playtest 40's F5): a reanimated body
        // dies again when its spell ends, the engine's own death with no hit,
        // drop or activation behind it; the server stood this body up on its
        // host's sample after an accepted reanimate, so the host's report
        // within the reanimation's span is the spell's end
        evidence = fmt::format("the reanimation's end: its host's engine "
                               "stood it up {} s ago after an accepted "
                               "reanimate",
                               stoodUp->count() / 1000);
      } else if (trapHit) {
        // the last evidence read (playtest 48): a death every earlier rule
        // explains keeps its line, the reanimation's end among them
        evidence = trapHitEvidence();
      } else {
        refuse("E_DEATH_UNCHECKED",
               "no hit, drop, activation or trap hit the server saw");
        return;
      }
      break;
    case 3:
      // thuum docs/DECISIONS.md ADR-062 decision 2 (Eli: "trust should
      // LARGELY be the default"): a host-run script's Kill, reported by the
      // host whose engine ran it (the platform's hook on Actor.Kill marks
      // the actor, DeathReportService names cause 3 within its window), is
      // the host's word, R2 in npc-fate.md's Authority table (recorded, not
      // validated); before ADR-062 it was refused as the server's to run,
      // and the server's own scripted kills (scripted-effects.md) stay its
      // own
      evidence = "a host-run script's kill (ADR-062, R2, unvalidated)";
      if (killer) {
        evidence += fmt::format(", the killer {:x} the script named",
                                killer->GetFormId());
      }
      break;
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

namespace {
// the wire's cap on the login list's player class (wire-schema
// cap::SHARED_QUESTS, the list's own)
constexpr size_t kPlayerClassPerList = 256;
}

void ActionListener::SendQuestStages(MpActor& actor)
{
  // the login's first message names the sharing mode and its list (memo D58
  // B): in mode list the shared quests, and nothing at all when none is
  // listed; in mode all the quests a seat never reports, the denied and,
  // while the quest scope acts, the player class (thuum ADR-068 rung 5), sent
  // even when none is, so the seat learns the mode
  const uint8_t mode = partOne.SharedQuestsMode();
  const auto listed = partOne.SharedQuestsListed();
  if (mode == 0 && listed.empty()) {
    return;
  }
  QuestStageMessage list;
  list.mode = mode;
  for (const auto& quest : listed) {
    try {
      list.shared.push_back(quest.ToFormId(partOne.worldState.espmFiles));
    } catch (const std::exception&) {
      // a listed quest whose plugin this load order lacks
    }
  }
  if (mode == 0 && list.shared.empty()) {
    return;
  }
  // thuum ADR-068 rung 5 (the ownership rule): the player-class quests, whose
  // scripts each seat runs for its own player (the platform admits their
  // events past its gate), named while the quest scope acts
  std::vector<uint32_t> player;
  for (const auto& quest : partOne.PlayerClassQuests()) {
    try {
      const uint32_t id = quest.ToFormId(partOne.worldState.espmFiles);
      if (player.size() < kPlayerClassPerList &&
          std::find(player.begin(), player.end(), id) == player.end()) {
        player.push_back(id);
      }
    } catch (const std::exception&) {
      // a marked quest whose plugin this load order lacks
    }
  }
  if (!player.empty()) {
    list.player = std::move(player);
  }
  actor.SendToUser(list, true);

  // thuum ADR-068 rung 5 (D-Q3 A: the replay is a load, served true): the
  // quests recorded running ahead of the stages, so a stage's quest runs
  // when its stage applies, and those recorded stopped behind them; a quest
  // the world no longer shares (denied, the player class, off a list) or a
  // seat's churn stays unsent, as its stages do. A quest recorded stopped
  // whose stages the login replays is started ahead of them too (wire-rules
  // quest_runs at_login): the seat would start it for its first stage
  // anyway, and only a start word names the start-up stages whose fragments
  // the seat must not run again, so each quest reads start, stages, stop
  const auto sendWord = [&](const FormDesc& quest, uint32_t word) {
    QuestStageMessage message;
    try {
      message.quest = quest.ToFormId(partOne.worldState.espmFiles);
    } catch (const std::exception&) {
      return false; // a recorded quest whose plugin this load order lacks
    }
    message.stage = 0;
    message.word = word;
    message.served = true;
    StampWordStages(partOne, message);
    actor.SendToUser(message, true);
    return true;
  };
  const auto [starts, stops] = partOne.QuestRunsAtLogin();
  // a start of a quest the stops name too is a stopped quest's, for its
  // stages
  const std::set<FormDesc> stopping(stops.begin(), stops.end());
  size_t running = 0;
  size_t staged = 0;
  for (const auto& quest : starts) {
    if (!sendWord(quest, 1)) {
      continue;
    }
    if (stopping.count(quest) != 0) {
      ++staged;
    } else {
      ++running;
    }
  }
  size_t stages = 0;
  for (const auto& entry : partOne.SharedQuestsAtLogin()) {
    QuestStageMessage message;
    try {
      message.quest = entry.quest.ToFormId(partOne.worldState.espmFiles);
    } catch (const std::exception&) {
      continue; // a recorded quest whose plugin this load order lacks
    }
    message.stage = entry.stage;
    message.served = true;
    actor.SendToUser(message, true);
    ++stages;
  }
  size_t stopped = 0;
  for (const auto& quest : stops) {
    stopped += sendWord(quest, 2) ? 1 : 0;
  }
  spdlog::info("QuestStage: actor {:x} sent mode {} with {} {} quests, {} "
               "stages after a login",
               actor.GetFormId(), mode, list.shared.size(),
               mode == 0 ? "shared" : "denied", stages);
  spdlog::info("QuestStage: actor {:x} sent {} running and {} stopped "
               "quest(s) around the stages after a login, {} of the stopped "
               "started ahead of their stages (D-Q3 A)",
               actor.GetFormId(), running, stopped, staged);
}

// thuum memo D56 A (Eli, 2026-10-28, "A for both"): the server cures the
// listed spells on a player at its login and every minute after, through the
// engine's own RemoveSpell (the shrine's cure in one line), so nobody turns
// from a seat-side disease until the vampirism slice is built (apocrypha's
// scout: Sanguinare Vampiris runs on the seat, its script turns the player
// three game days later with no server record). A spell the player's game
// does not carry is a no-op there. One info line per send.
void ActionListener::SendCures(MpActor& actor)
{
  const auto& spells = partOne.CureSpells();
  if (spells.empty()) {
    return;
  }
  const uint32_t actorId = actor.GetFormId();
  size_t sent = 0;
  for (const auto& desc : spells) {
    uint32_t formId = 0;
    try {
      formId = desc.ToFormId(partOne.worldState.espmFiles);
    } catch (const std::exception&) {
      continue; // a listed spell whose plugin this load order lacks
    }
    SendSpellSnippet(actor, "RemoveSpell", formId);
    ++sent;
  }
  if (sent == 0) {
    return;
  }
  spdlog::info("[verb:magic-effects] cure: actor {:x} told to drop {} listed "
               "spell(s) (memo D56 A)",
               actorId, sent);
  // the minute chain: one per login, the newer login's chain the one that
  // goes on (the epoch)
  const uint32_t epoch = ++cureEpochs[actorId];
  partOne.worldState.SetTimer(std::chrono::seconds(60))
    .Then([this, actorId, epoch](Viet::Void) {
      auto it = cureEpochs.find(actorId);
      if (it == cureEpochs.end() || it->second != epoch) {
        return;
      }
      const auto& form = partOne.worldState.LookupFormById(actorId);
      MpActor* target = form ? form->AsActor() : nullptr;
      if (!target ||
          partOne.serverState.UserByActor(target) ==
            Networking::InvalidUserId) {
        cureEpochs.erase(it);
        return;
      }
      // the same epoch goes on: SendCures bumps it, so the chain is one
      --it->second;
      SendCures(*target);
    });
}

void ActionListener::KillByPlayerGlobals(
  MpActor& actor, const std::vector<PlayerGlobal>& merged)
{
  if (actor.IsDead()) {
    return;
  }
  const auto lethal = partOne.LethalPlayerGlobals(merged);
  if (lethal.empty()) {
    return;
  }
  spdlog::info("PlayerGlobals: user {} actor {:x} dies by the server's "
               "word: global {} at {} reached its killAt (memo D51 B)",
               partOne.serverState.UserByActor(&actor), actor.GetFormId(),
               lethal.front().global.ToString(), lethal.front().value);
  actor.Kill(nullptr, false);
}

bool ActionListener::RecordPlayerGlobals(
  MpActor& actor, const std::vector<PlayerGlobal>& values, const char* why)
{
  const auto merged =
    PartOne::MergePlayerGlobals(actor.GetPlayerGlobals(), values);
  const bool changed = actor.SetPlayerGlobals(merged);
  PlayerGlobalsMessage message;
  message.watch = partOne.PlayerGlobalsWatch();
  for (const auto& entry : values) {
    try {
      message.values.push_back(
        { entry.global.ToFormId(partOne.worldState.espmFiles), entry.value });
    } catch (const std::exception&) {
      // a global whose plugin this load order lacks
    }
  }
  if (!message.values.empty()) {
    actor.SendToUser(message, true);
  }
  spdlog::info("PlayerGlobals: actor {:x} {}: {} values written by the "
               "server's word, {} sent to its game{}",
               actor.GetFormId(), why, values.size(), message.values.size(),
               changed ? "" : ", the record unchanged");
  KillByPlayerGlobals(actor, merged);
  return changed;
}

// thuum docs/verbs/player-globals.md, the needs (playtest 37, 2026-10-28,
// Eli: "food doesnt replenish hunger"; numidium's live read: the hunger need
// 81a stood at 1000 through 115 reads while Eli ate, 816 climbing). In the
// game the Survival Mode Creation (ccQDRSSE001-SurvivalMode.esl) overrides
// 115 food records with one of Update.esm's Survival_FoodRestoreHunger
// effects (2ee1 VerySmall to 2ee4 Large) and its twelve hot soups with
// Survival_FoodRestoreCold (2ee5) too; the esl's overrides of those effects
// carry Survival_HungerRestoreEffectScript or
// Survival_FoodColdRestoreEffectScript, whose OnEffectStart lowers the need
// by a GLOB's amount (Survival_HungerRestore{VerySmall,Small,Medium,Large}
// Amount 2, 18, 220, 380; Survival_ColdRestoreMediumAmount 200) through
// Survival_NeedBase.DecrementNeed, which clamps to a floor (hunger's
// NeedMinValue 0; cold's stage 1 value 50). That script never runs on a
// seat, so the server does the arithmetic from the record: the eaten
// record's effects against server-settings.json's `survivalFood`, while the
// mode's switch (828, the toggle) is recorded on, as the effects' own
// condition on 826 (GetGlobalValue == 1) gates them in the game. The game's
// next need tick reads the written value and recomputes the stage.
void ActionListener::ApplySurvivalFood(MpActor& actor, uint32_t baseId)
{
  WorldState& worldState = partOne.worldState;
  const auto lookup = worldState.GetEspm().GetBrowser().LookupById(baseId);
  if (!lookup.rec || !(lookup.rec->GetType() == "ALCH")) {
    return;
  }
  const auto effects = MpActor::EffectsOf(&worldState, baseId);
  if (effects.empty()) {
    return;
  }
  try {
    if (espm::GetData<espm::ALCH>(baseId, &worldState).isPoison) {
      return; // on a weapon, never eaten (docs/verbs/poison.md)
    }
  } catch (std::exception& e) {
    spdlog::warn("ApplySurvivalFood {:x} - {:x}: {}", actor.GetFormId(),
                 baseId, e.what());
    return;
  }
  auto& cache = worldState.GetEspmCache();
  const auto& files = worldState.espmFiles;
  // a listed global as the game holds it: the value written here, else the
  // record's (with the settings' default where the record is silent), else
  // the GLOB record's own, the game's start value
  const auto record = partOne.PlayerGlobalsAtLogin(actor.GetPlayerGlobals());
  std::vector<PlayerGlobal> written;
  const auto valueOf = [&](const FormDesc& global) -> std::optional<float> {
    for (const auto& g : written) {
      if (g.global == global) {
        return g.value;
      }
    }
    for (const auto& g : record) {
      if (g.global == global) {
        return g.value;
      }
    }
    try {
      return espm::GetData<espm::GLOB>(global.ToFormId(files), &worldState)
        .value;
    } catch (std::exception&) {
      return std::nullopt;
    }
  };
  if (const auto switchGlobal = partOne.SurvivalFoodSwitch()) {
    const auto on = valueOf(*switchGlobal);
    if (!on || *on != 1.f) {
      spdlog::debug("ApplySurvivalFood {:x} ate {:x} with the mode's switch "
                    "{} not on: nothing moves",
                    actor.GetFormId(), baseId, switchGlobal->ToString());
      return;
    }
  }
  const char* itemEditorId = lookup.rec->GetEditorId(cache);
  for (const auto& effect : effects) {
    const auto entry = partOne.SurvivalFoodOf(effect.effectId);
    if (!entry) {
      continue;
    }
    const auto before = valueOf(entry->need);
    if (!before) {
      spdlog::warn("ApplySurvivalFood {:x} ate {:x}: the need {} is in no "
                   "record and no loaded plugin",
                   actor.GetFormId(), baseId, entry->need.ToString());
      continue;
    }
    const float after = std::max(entry->floor, *before - entry->amount);
    std::string effectName = fmt::format("{:x}", effect.effectId);
    const auto effectLookup =
      worldState.GetEspm().GetBrowser().LookupById(effect.effectId);
    if (effectLookup.rec && effectLookup.rec->GetEditorId(cache)) {
      effectName = effectLookup.rec->GetEditorId(cache);
    }
    spdlog::info("[verb:player-globals] {:x} ate {:x} {} ({}): {} {} -> {}",
                 actor.GetFormId(), baseId, itemEditorId ? itemEditorId : "",
                 effectName, entry->need.ToString(), *before, after);
    if (after == *before) {
      continue; // at the floor already
    }
    bool replaced = false;
    for (auto& g : written) {
      if (g.global == entry->need) {
        g.value = after; // two effects on one need add up
        replaced = true;
      }
    }
    if (!replaced) {
      written.push_back({ entry->need, after });
    }
  }
  if (written.empty()) {
    return;
  }
  // the record holds listed globals only (wire-rules player_globals)
  if (const uint8_t refusal = partOne.CheckPlayerGlobals(written)) {
    spdlog::warn("ApplySurvivalFood {:x} ate {:x}: a need is no listed "
                 "player global ({})",
                 actor.GetFormId(), baseId, refusal);
    return;
  }
  RecordPlayerGlobals(actor, written, "ate");
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
  MpActor* myActor = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!myActor) {
    return spdlog::error("Unable to shot from user with id: {}.",
                         rawMsgData.userId);
  }
  // thuum docs/verbs/npc-stream.md (ADR-050, train 53): a hosted archer's
  // shot comes from its host's game under the actor's idx (the host's own
  // shot carries the host's own idx); the host rule gates it as it gates
  // every hosted report
  const bool hosted = msg.idx != myActor->GetIdx();
  MpActor* ac = hosted
    ? ActorUpdatableBy(msg.idx, rawMsgData.userId, MsgType::PlayerBowShot)
    : myActor;
  if (!ac) {
    return;
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
    return spdlog::error("ActionListener::OnPlayerBowShot {:x} - unable to "
                         "shot not an ammo {:x}",
                         ac->GetFormId(), msg.ammoId);
  }

  // thuum docs/verbs/marksman.md: a shot from the bow the player holds with
  // an arrow it has equipped, as the server records its equipment. The
  // record's hands are the client's last accepted report and lag it
  // (playtest 37's finding 45, 2026-10-29: a report refused for one entry,
  // the hands a login re-dresses; 32 arrows that did nothing), so a bow or
  // an arrow the hands lack is judged by the inventory, the server's own
  // (R0): held there, the shot is plausible and accepted, said once per
  // shooter per ten seconds; absent there, refused (E_SHOT_NO_BOW,
  // E_SHOT_NO_ARROW)
  const auto worn = [&](uint32_t baseId) {
    for (auto& entry : ac->GetEquipment().inv.entries) {
      if (entry.baseId == baseId && entry.GetWorn() != Inventory::Worn::None) {
        return true;
      }
    }
    return false;
  };
  // a hosted actor's quiver is its engine's (the host's hands report lists
  // no ammo, and an NPC's arrows come from lists the server does not walk):
  // its bow is checked against the record, its ammo recorded (R2)
  const bool bowInHands = worn(msg.weaponId);
  const bool arrowInHands = hosted || worn(msg.ammoId);
  if (!bowInHands || !arrowInHands) {
    const auto& inventory = ac->GetInventory();
    if (inventory.GetItemCount(msg.weaponId) < 1) {
      return spdlog::warn("ActionListener::OnPlayerBowShot - E_SHOT_NO_BOW: "
                          "{:x} shot {:x} from {:x}, which its inventory does "
                          "not hold; refused",
                          ac->GetFormId(), msg.ammoId, msg.weaponId);
    }
    if (!hosted && inventory.GetItemCount(msg.ammoId) < 1) {
      return spdlog::warn("ActionListener::OnPlayerBowShot - E_SHOT_NO_ARROW: "
                          "{:x} shot {:x} from {:x}, an arrow its inventory "
                          "does not hold; refused",
                          ac->GetFormId(), msg.ammoId, msg.weaponId);
    }
    static std::unordered_map<uint32_t, std::chrono::steady_clock::time_point>
      lastStaleHandsLog;
    const auto now = std::chrono::steady_clock::now();
    auto& last = lastStaleHandsLog[ac->GetFormId()];
    if (last == std::chrono::steady_clock::time_point() ||
        now - last >= std::chrono::seconds(10)) {
      last = now;
      spdlog::info("ActionListener::OnPlayerBowShot - hands stale: shot from "
                   "{:x} accepted on the inventory ({:x}'s record's hands "
                   "lack {})",
                   msg.weaponId, ac->GetFormId(),
                   bowInHands       ? "the arrow"
                     : arrowInHands ? "the bow"
                                    : "the bow and the arrow");
    }
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

  if (hosted) {
    // thuum docs/verbs/npc-fate.md 1c (x-fate-inventory 20261104-201910 on
    // the hands pair): the engine spends none of an NPC's arrows at its
    // shot (the host's copy of the archer's body held 16 after one shot
    // while this record had spent one to 15, and the take of 16 was
    // refused E_TAKE_NOT_IN_RECORD; UESP's Ammunition: a follower given one
    // arrow of a kind shoots it without end), so the record spends none
    // either, R2 as the engine, and the body's loot is what the host sees
    spdlog::info("[verb:npc-stream] E_SHOT_QUIVER: {:x}'s shot of {:x} by "
                 "{:x} spends no record arrow (the engine spends none of an "
                 "NPC's; its quiver is recorded, never depleted)",
                 ac->GetFormId(), msg.ammoId, myActor->GetFormId());
  } else if (!ac->IsGodMode()) {
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
  // every listener with a user but the sender (OnHostedActorState's loop):
  // an offline listener's GetActorToSendTo hands a message to its host, so
  // the loop before train 53 sent a host near a shot one ArrowShot per NPC
  // it hosted among the shooter's listeners (one arrow drawn per hosted
  // neighbour; found at train 53's read, never seen in a lab run, which
  // shot with no hosted cell near)
  for (auto listener : ac->GetActorListeners()) {
    if (listener == myActor ||
        listener->GetUserId() == Networking::InvalidUserId) {
      continue;
    }
    listener->SendToUser(relay, true);
  }
  spdlog::info("ActionListener::OnPlayerBowShot - {:x} shot {:x} from {:x}, "
               "power {:.2f}, aim {:.3f} heading {:.3f}{}",
               ac->GetFormId(), msg.ammoId, msg.weaponId, msg.power,
               msg.aimAngle, msg.aimHeading,
               hosted ? fmt::format(", by its host {:x}", myActor->GetFormId())
                      : std::string());
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

  if (actor->OnEquip(msg.baseId)) {
    ApplySurvivalFood(*actor, msg.baseId);
  }
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
  // docs/verbs/actor-values.md: a player's base health, raised by
  // level-ups), the race's and the base NPC's otherwise
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

// thuum docs/verbs/melee-reach.md and hit-cone.md (M3's "NPC melee reach and
// hit cone", the weekend pass of 2026-10-29): the reach and cone rules were
// players' alone because the bounds' body extents are a humanoid's and the
// facts' scale is a race's height. A humanoid NPC (its race playable, RACE
// DATA flag 0x1) has the same extents and the same height table, so it is
// gated as a player is, as a target and, when its host reports its hit, as
// an aggressor (R1: the host computes, the server validates). Creatures and
// the non-playable humanoid races (Elder, Dremora, the vampire races) keep
// the old bound: a named limit until their extents are read.
bool HumanoidNpc(const MpActor& actor, WorldState& worldState)
{
  if (actor.IsCreatedAsPlayer() || actor.GetRaceId() == 0) {
    return false;
  }
  try {
    return (espm::GetData<espm::RACE>(actor.GetRaceId(), &worldState).flags &
            espm::RACE::kPlayable) != 0;
  } catch (std::exception&) {
    return false;
  }
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
    spdlog::error("ActionListener::OnHit - aggressor and targetRef are in "
                  "different cells "
                  "or world. Aggressor: {:x}, targetRef: {:x}, cellOrWorld "
                  "of aggressor: "
                  "{:x}, cellOrWorld of targetRef: {:x}",
                  aggressor->GetFormId(), targetRef->GetFormId(),
                  aggressorCellOrWorld.ToFormId(files),
                  targetCellOrWorld.ToFormId(files));
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
  // thuum docs/verbs/npc-stream.md (train 53): a hosted actor's spell hit
  // claims its recorded cast whether or not its record's hands hold the
  // spell (the hands are the host's engine's, recorded; the cast is the
  // server's record); one its casts do not cover falls to the reach rule
  // below (a cloak's damage), which refuses it when nothing reaches
  const bool hostedSpellHit = isSourceSpell && aggressor != myActor;
  bool castClaimed = false;
  if (isSourceSpell &&
      (equipment.IsSpellEquipped(hitData.source) ||
       IsVoiceVariation(equipment, hitData.source, partOne.worldState) ||
       hostedSpellHit)) {
    // thuum docs/verbs/spell-cast.md: a spell hits only as a cast the server
    // recorded: a fire-and-forget cast once (an area spell's targets once
    // each), a stream by the time it held its target. A hosted actor's casts
    // are recorded under its own id from its host's reports (ADR-050, train
    // 53), so its hits are claimed and scaled the same way, and one with no
    // cast behind it is refused (rule 5)
    float magnitudeScale = 1.f;
    bool dualCast = false;
    {
      const auto claim =
        partOne.ClaimCast(aggressor->GetFormId(), hitData.source,
                          targetRef->GetFormId(), targetRef->GetPos());
      if (claim.check != PartOne::CastCheck::Claimed) {
        if (hostedSpellHit && claim.check == PartOne::CastCheck::NoCast &&
            !equipment.IsSpellEquipped(hitData.source)) {
          // nothing claimed, nothing spent: the reach rule below decides
        } else {
          spdlog::warn(
            "ActionListener::OnHit - {}: {:x} hits {:x} with spell "
            "{:x} {:.0f} units away; refused",
            claim.check == PartOne::CastCheck::TooFar ? "E_SPELL_RANGE"
                                                      : "E_SPELL_NO_CAST",
            aggressor->GetFormId(), targetRef->GetFormId(), hitData.source,
            (targetRef->GetPos() - aggressor->GetPos()).Length());
          return;
        }
      } else {
        castClaimed = true;
      }
      magnitudeScale = claim.scale;
      dualCast = claim.dual;
      // thuum docs/verbs/shadowbond.md: a bond spell's claimed hit bonds
      // the two until either acts, or the spell's duration ends
      if (castClaimed && targetRef->GetFormId() != aggressor->GetFormId() &&
          partOne.IsBondSpell(hitData.source)) {
        partOne.RecordBond(aggressor->GetFormId(), targetRef->GetFormId(),
                           hitData.source);
      }
      if (castClaimed && claim.relay) {
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
    if (castClaimed) {
      OnSpellHit(aggressor, targetRef, hitData, magnitudeScale, dualCast);
      return;
    }
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

  if (isSourceSpell) {
    auto claim = partOne.ClaimReached(
      aggressor->GetFormId(), hitData.source, targetRef->GetFormId(),
      targetRef->GetPos(), aggressor->GetPos());
    // thuum docs/verbs/npc-stream.md (train 53; playtest 37's flame
    // atronach, 2026-10-29): a creature's cloak is a constant-effect ability
    // of its base or race (Skyrim.esm's AbFlameAtronach 0x2f3b5,
    // AtronachFlameRace's one spell, whose effect AbAtronachCloakFire
    // 0x64459 reaches crAtronachCloakFlameDmg 0x6445b; lab/esm.py on
    // sky-srv), which its host's engine runs from the actor's birth and
    // never casts, so no cast of its lingered for the reach rule and every
    // cloak hit was refused
    // ("which it holds not"). The ability the record holds stands as a cast
    // of its own when its cloak's hit arrives (the records say the actor
    // bears it, R1; the distance bound and the cadence are the reach
    // rule's), and the hit claims it as a player's cloak hit claims the
    // cloak's cast; a spell no ability of the record reaches stays refused
    // (rule 5)
    if (hostedSpellHit && claim.check == PartOne::CastCheck::NoCast) {
      if (const auto ability =
            partOne.AbilityReaching(*aggressor, hitData.source)) {
        partOne.StartCast(aggressor->GetFormId(), *ability, uint8_t{ 3 },
                          aggressor->GetPos(), false);
        claim = partOne.ClaimReached(aggressor->GetFormId(), hitData.source,
                                     targetRef->GetFormId(),
                                     targetRef->GetPos(), aggressor->GetPos());
        spdlog::info("[verb:npc-stream] ability cloak: {:x} hits {:x} with "
                     "{:x}, the cloak of ability {:x} its record holds; the "
                     "ability stands as its cast{}",
                     aggressor->GetFormId(), targetRef->GetFormId(),
                     hitData.source, *ability,
                     claim.check == PartOne::CastCheck::Claimed
                       ? ""
                       : ", the hit outside its reach");
      }
    }
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
  // thuum docs/verbs/marksman.md (playtest 37's finding 45): an arrow's hit
  // is judged below by the shot the server recorded, not by the record's
  // hands, which lag the client's report; a bow the hands lack but the
  // inventory holds passes to that claim (OnPlayerBowShot's allowance, the
  // same shape), and the claim refuses what no recorded shot covers
  const bool bowShot = IsBowOrCrossbowShot(hitData, &partOne.worldState);
  const bool bowHeld =
    bowShot && aggressor->GetInventory().GetItemCount(hitData.source) > 0;

  if (equipment.inv.HasItem(hitData.source) || isUnarmed || bowHeld) {
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

    // The reach and cone rules: a player's hit on a player or a humanoid
    // NPC, and a humanoid NPC's hit its host reports (the bound's body
    // extents are a humanoid's; creatures keep the old bound, HumanoidNpc
    // above)
    MpActor* targetActor = targetRef->AsActor();
    const bool playerSwing = aggressor == myActor;
    const bool hostedSwing = !playerSwing &&
      partOne.worldState.HostOf(*aggressor) == myActor &&
      HumanoidNpc(*aggressor, partOne.worldState);
    const bool gatedTarget = targetActor &&
      (partOne.serverState.UserByActor(targetActor) !=
         Networking::InvalidUserId ||
       HumanoidNpc(*targetActor, partOne.worldState));
    if ((playerSwing || hostedSwing) && gatedTarget &&
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
    // thuum docs/verbs/marksman.md: an arrow hits only as a shot the server
    // recorded, and once, at its draw power (a hosted actor's shots are its
    // host's game's reports under the actor's own id, train 53, claimed the
    // same way): the engine scales a player's arrow's damage by the power
    // once, at launch (the re-analyst's read of Projectile::Launch,
    // 2026-10-09)
    float damageMult = 1.f;
    if (bowShot) {
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
      // the launch's surface goes with the launch (the rune's glyph)
      spellCastData.decal.reset();
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
    if (caster == myActor) {
      spdlog::info("ActionListener::OnSpellCast - spell {0:x} not "
                   "found in equipment",
                   spellCastData.spell);
      return;
    }
    // thuum docs/verbs/npc-stream.md (ADR-050, train 53): a hosted caster's
    // spell its record's hands lack is its host's engine's (a template's or
    // a leveled list's the server does not walk, or a power cast from no
    // slot): recorded unvalidated (R2, rule 5) when the form is a spell at
    // all, so its hits are claimed against this cast as any cast's are
    const auto rec = partOne.HasEspm()
      ? partOne.GetEspm().GetBrowser().LookupById(spellCastData.spell).rec
      : nullptr;
    if (!rec || rec->GetType() != espm::SPEL::kType) {
      spdlog::warn("ActionListener::OnSpellCast - E_CAST_FORM: {:x} by {:x} "
                   "casts {:x}, which is no spell; refused",
                   caster->GetFormId(), myActor->GetFormId(),
                   spellCastData.spell);
      return;
    }
    spdlog::info("[verb:npc-stream] E_CAST_UNKNOWN: {:x} by {:x} casts {:x}, "
                 "which its record's hands lack; recorded unvalidated (the "
                 "host's engine's list)",
                 caster->GetFormId(), myActor->GetFormId(),
                 spellCastData.spell);
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
      // the one info line for an accepted aimed or self cast relayed at
      // once; the target-delivered, teleport, shout and ended casts have
      // their own lines above. The lab counts a hosted caster's casts by it
      // (x-npc-creature-caster: a creature's casts were countable only from
      // the watcher's puppet casts before, 2026-10-28)
      spdlog::info("ActionListener::OnSpellCast - {:x} cast {:x} in hand {} "
                   "at {:x}{}; relayed",
                   caster->GetFormId(), spellCastData.spell, hand,
                   spellCastData.target,
                   caster == myActor ? "" : " (hosted, by its host's report)");
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
  // thuum docs/verbs/npc-fate.md ("Reanimation is any player's, on any
  // body"; ADR-060): a reanimate's accepted hit on a dead body hands the
  // body to the caster's seat, whose engine ran the spell and runs the
  // effect; its samples stand the record up (the hosted-state rule)
  if (targetActorPtr->IsDead() &&
      ReanimateEffectOf(&partOne.worldState, effects).has_value()) {
    // the stamp a placed corpse's stand-up is read against
    // (OnHostedActorState, finding 65)
    targetActorPtr->NoteReanimateAccepted(std::chrono::steady_clock::now());
    ClaimReanimated(*aggressor, *targetActorPtr, hitData.source, effects);
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
      "attack speed. Weapon: {1:x}. Elapsed time: {2}. Expected attack "
      "time: "
      "{3}",
      hitData.target, hitData.source, timePassedAnyTarget.count(),
      expectedAttackTime);
    return;
  }

  // if (IsDistanceValid(*aggressor, targetActor, hitData) == false) {
  //   float distance =
  //     std::sqrt(GetSqrDistanceToBounds(*aggressor, targetActor));

  //   // TODO: fix bounding boxes for creatures such as chicken, mudcrab,
  //   etc float reachPveHotfixMult =
  //     (aggressor->GetBaseId() <= 0x7 && targetActor.GetBaseId() <= 0x7)
  //     ? 1.f
  //     : std::numeric_limits<float>::infinity();

  //   float reach = GetReach(*aggressor, hitData.source,
  //   reachPveHotfixMult); uint32_t aggressorId = aggressor->GetFormId();
  //   uint32_t targetId = targetActor.GetFormId();
  //   spdlog::debug(
  //     fmt::format("{:x} actor can't reach {:x} target because distance {}
  //     is
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
// that hit. When a hit begins a fight (the Rust rule's fights table), the
// victim's game starts combat between its figure of the attacker and its own
// player (0x14 on that client). The engine then refuses the victim a wait or
// a sleep with the attacker near, as it does the attacker;
// PartOne::TickFights ends the fight on both games. Since 2026-10-28 (thuum
// docs/verbs/npc-stream.md, H-53h) a hosted NPC is a party too: the table is
// what IsHostileToActor reads for an effect between its two ends, and it
// never held a creature, so an atronach's cloak burnt no player server-side
// however long the two fought. The host's engine fights for its NPC by
// itself (its AI is the host's, R2), so a hosted victim gets no notice.
void ActionListener::NotifyHostility(MpActor& aggressor, MpActor& target,
                                     std::chrono::steady_clock::time_point now)
{
  skymp::rules::HostilityFacts facts{};
  facts.aggressor_is_player =
    partOne.serverState.UserByActor(&aggressor) != Networking::InvalidUserId;
  facts.aggressor_is_hosted = !facts.aggressor_is_player &&
    partOne.worldState.HostOf(aggressor) != nullptr;
  facts.target_is_player =
    partOne.serverState.UserByActor(&target) != Networking::InvalidUserId;
  facts.target_is_hosted =
    !facts.target_is_player && partOne.worldState.HostOf(target) != nullptr;
  facts.same_actor = &aggressor == &target;
  if (!skymp::rules::hostility_counted(facts)) {
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
  if (!facts.target_is_player || !facts.aggressor_is_player) {
    // the StartCombat notice stays between players (ADR-023): a hosted
    // NPC's victim is its host engine's to fight, and a player's copy of a
    // hosted creature has its AI off, so a StartCombat on it would do
    // nothing the engine means; the server's rest refusal (in_fight) is
    // what a player attacked by a hosted creature keeps
    spdlog::info("ActionListener::OnWeaponHit - hostility: {:x} hit {:x}, a "
                 "fight begins; no notice, the pair is not two players",
                 aggressor.GetFormId(), target.GetFormId());
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

// thuum docs/verbs/npc-fate.md ("Reanimation is any player's, on any body";
// Eli, playtest 37: "anyone should be able to reanimate any corpse, that's
// Just Gameplay"; ADR-060, playtest 41): the engine's Reanimate effect runs
// in the caster's own game, the one that knows what the commander wants, so
// the accepted hit makes that game the body's simulator until its engine
// ends the effect (the cells rule's commanded claim, no lease; UpdateHoster
// to every listening game): its engine stands the body and runs the get-up
// and the commanded packages, and its samples stand the record
// (OnHostedActorState: inside MpActor::kReanimateWindowMs of the hit, or
// after its own dead sample). The
// caster's game is its user's for a player and its host's for a hosted
// caster (a necromancer's cast at a body in another game's cell). The claim
// is R0, the thrall's behaviour R2 (ADR-060's H1 to H3 open until
// x-reanimate-claim reads them).
void ActionListener::ClaimReanimated(
  MpActor& caster, MpActor& body, uint32_t spell,
  const std::vector<espm::Effects::Effect>& effects)
{
  // the seat whose engine ran the spell: the caster when a user plays it,
  // else the caster's host (a hosted necromancer's thrall is its host's)
  MpActor* commander =
    partOne.serverState.UserByActor(&caster) != Networking::InvalidUserId
    ? &caster
    : partOne.worldState.HostOf(caster);
  if (!commander) {
    spdlog::info("[verb:npc-fate] reanimate {:x} by {:x} on {:x}: no seat "
                 "runs the caster; the body stays its cell's",
                 spell, caster.GetFormId(), body.GetFormId());
    return;
  }
  // no lease (Eli, 2026-11-12: "summons and resurrections should obey the
  // timers the level of spell"): the command lasts until the commander's
  // engine ends the effect, the thrall's death, which its seat reports
  // (OnActorDeath's command's end, OnDeathState's release). The record's
  // duration (its EFIT, seconds) is named for the lab and read for one line
  // alone, the commander's departure (wire-rules cells.rs gone, ADR-060 as
  // amended 2026-11-12): a full thrall's, a day and more (Dead Thrall's
  // 86,313,600 s), stays alive, a timed one dies; dual casting, perks and
  // the spell's level set the engine's own timer
  uint32_t recordSeconds = 0;
  const auto effectId = ReanimateEffectOf(&partOne.worldState, effects);
  for (const auto& effect : effects) {
    if (effectId && effect.effectId == *effectId) {
      recordSeconds = effect.duration;
    }
  }
  const auto verdict = partOne.ClaimCommanded(*commander, body, recordSeconds);
  switch (verdict) {
    case PartOne::ClaimVerdict::Granted:
    case PartOne::ClaimVerdict::Reclaimed:
      spdlog::info(
        "[verb:npc-fate] reanimate {:x} by {:x} on {:x}: {:x} "
        "commands it until its engine ends the effect (the record's {} s) "
        "(ADR-060){}",
        spell, caster.GetFormId(), body.GetFormId(), commander->GetFormId(),
        recordSeconds,
        verdict == PartOne::ClaimVerdict::Reclaimed ? ", renewed" : "");
      break;
    case PartOne::ClaimVerdict::Unloaded:
      spdlog::warn("[verb:npc-fate] E_COMMAND_UNLOADED: reanimate {:x} by "
                   "{:x} on {:x}: {:x} has not loaded the body's cell; the "
                   "body stays its cell's",
                   spell, caster.GetFormId(), body.GetFormId(),
                   commander->GetFormId());
      break;
    case PartOne::ClaimVerdict::Held:
      break;
  }
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
