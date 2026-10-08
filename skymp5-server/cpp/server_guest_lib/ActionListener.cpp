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
#include "libespm/SPEL.h"
#include "libespm/Utils.h"
#include "libespm/WEAP.h"
#include "script_objects/EspmGameObject.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <limits>
#include <map>
#include <optional>
#include <spdlog/spdlog.h>
#include <unordered_set>

#include "CustomPacketMessage.h"
#include "HostStartMessage.h"
#include "HostStopMessage.h"
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
                                          Networking::UserId userId)
{
  MpActor* myActor = partOne.serverState.ActorByUser(userId);
  // The old behavior is doing nothing in that case. This is covered by tests
  if (!myActor) {
    spdlog::warn("SendToNeighbours - No actor assigned to user");
    return nullptr;
  }

  MpForm* form = partOne.worldState.LookupFormByIdx(idx);
  MpActor* actor = form ? form->AsActor() : nullptr;
  if (!actor) {
    spdlog::error("SendToNeighbours - Target actor doesn't exist");
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
      partOne.SendHostStop(userId, *actor);

      partOne.worldState.hosters.erase(actor->GetFormId());
      return nullptr;
    }

    auto it = partOne.worldState.hosters.find(actor->GetFormId());
    if (it == partOne.worldState.hosters.end() ||
        it->second != myActor->GetFormId()) {
      if (idx == 0) {
        spdlog::warn("SendToNeighbours - idx=0, <Message>::ReadJson or "
                     "similar is probably incorrect");
      }
      spdlog::error(
        "SendToNeighbours - No permission to update actor {:x} (not a hoster)",
        actor->GetFormId());
      partOne.SendHostStop(userId, *actor);
      return nullptr;
    }
  }

  return actor;
}

void ActionListener::RelayToListeners(MpActor& actor,
                                      Networking::PacketData data,
                                      size_t length, bool reliable)
{
  for (auto listener : actor.GetActorListeners()) {
    auto targetuserId = partOne.serverState.UserByActor(listener);
    if (targetuserId != Networking::InvalidUserId) {
      partOne.GetSendTarget().Send(targetuserId, data, length, reliable);
    }
  }
}

MpActor* ActionListener::SendToNeighbours(uint32_t idx,
                                          Networking::UserId userId,
                                          Networking::PacketData data,
                                          size_t length, bool reliable)
{
  MpActor* actor = ActorUpdatableBy(idx, userId);
  if (actor) {
    RelayToListeners(*actor, data, length, reliable);
  }
  return actor;
}

MpActor* ActionListener::SendToNeighbours(uint32_t idx,
                                          const RawMessageData& rawMsgData,
                                          bool reliable)
{
  return SendToNeighbours(idx, rawMsgData.userId, rawMsgData.unparsed,
                          rawMsgData.unparsedLength, reliable);
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
  auto actor = ActorUpdatableBy(msg.idx, rawMsgData.userId);
  if (actor) {
    bool teleportFlag = actor->GetTeleportFlag();
    actor->SetTeleportFlag(false);

    static const NiPoint3 kInfinityPos = {
      std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::infinity()
    };

    auto& espmFiles = actor->GetParent()->espmFiles;

    const auto& currentPos = actor->GetPos();
    const auto& currentRot = actor->GetAngle();
    const auto& currentCellOrWorld = actor->GetCellOrWorld();

    if (!MovementValidation::Validate(
          partOne, currentPos, currentRot, currentCellOrWorld,
          teleportFlag
            ? kInfinityPos
            : NiPoint3{ msg.data.pos[0], msg.data.pos[1], msg.data.pos[2] },
          FormDesc::FromFormId(msg.data.worldOrCell, espmFiles),
          rawMsgData.userId, actor, espmFiles)) {
      return;
    }

    RelayToListeners(*actor, rawMsgData.unparsed, rawMsgData.unparsedLength,
                     false);

    // thuum docs/verbs/map-markers.md: the first movement after a login
    // means the player's own world is up, so its map can be shown, what it
    // learned of its ingredients taught again
    // (docs/verbs/learned-effects.md), its favorites marked again
    // (docs/verbs/favorites.md), its RaceMenu look applied again
    // (docs/verbs/racemenu-sync.md), and its actor values and progress
    // (docs/verbs/actor-values.md)
    if (actor == partOne.serverState.ActorByUser(rawMsgData.userId) &&
        actor->TakeMapMarkersPending()) {
      SendMapMarkers(*actor);
      SendIngredientEffects(*actor);
      SendFavorites(*actor);
      SendRaceMenuPreset(*actor);
      SendActorValues(*actor);
    }

    if (!msg.data.isBlocking) {
      actor->IncreaseBlockCount();
    } else {
      actor->ResetBlockCount();
    }

    actor->SetPos(
      NiPoint3{ msg.data.pos[0], msg.data.pos[1], msg.data.pos[2] },
      SetPosMode::CalledByUpdateMovement);
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

  auto targetActor = SendToNeighbours(msg.idx, rawMsgData);

  if (!targetActor) {
    return;
  }

  // Only process animation system and set last anim event for player's actor
  if (targetActor != myActor) {
    return;
  }

  partOne.animationSystem.Process(targetActor, msg.data);
  targetActor->SetLastAnimEvent(msg.data);
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
    actor->SetAppearance(&msg.data.value());
    SendToNeighbours(msg.idx, rawMsgData, true);
  }

  UpdateAppearanceAttemptEvent updateAppearanceAttemptEvent(
    actor, msg.data.value(), isAllowed);
  updateAppearanceAttemptEvent.Fire(actor->GetParent());
}

void ActionListener::OnUpdateEquipment(const RawMessageData& rawMsgData,
                                       const UpdateEquipmentMessage& msg)
{
  MpActor* actor = partOne.serverState.ActorByUser(rawMsgData.userId);
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

  if (voiceSpell > 0 && !actor->IsSpellLearned(voiceSpell)) {
    spdlog::warn("ActionListener::OnUpdateEquipment {:x} - rejected equipment "
                 "update: spell {:x} is not learned",
                 actorFormId, voiceSpell);
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
  } else {
    actor->SendInventoryUpdate();

    for (uint32_t spellId : spellIdsToRemove) {
      if (spellId == 0) {
        continue;
      }
      SpSnippetObjectArgument spellArg;
      spellArg.formId = spellId;
      spellArg.type = "Spell";
      std::vector<std::optional<
        std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
        args;
      args.push_back(spellArg);
      SpSnippet("Actor", "RemoveSpell", args, actor->GetFormId())
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

  auto it =
    partOne.worldState.hosters.find(static_cast<uint32_t>(msg.data.caster));
  auto hosterId = it == partOne.worldState.hosters.end() ? 0 : it->second;

  if (msg.data.caster != 0x14) {
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

  constexpr bool kDefaultProcessingOnlyFalse = false;
  targetPtr->Activate(caster, kDefaultProcessingOnlyFalse,
                      msg.data.isSecondActivation);
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
      return spdlog::info("ActorValues: user {} actor {:x} report before "
                          "the login's record was applied, not taken",
                          rawMsgData.userId, actor->GetFormId());
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
  if (holding) {
    // values the server set that this report does not carry: the record
    // goes back so the player's game applies them
    actor->SendActorValueRecord();
  }
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

  ac->RemoveItem(msg.ammoId, 1, nullptr);
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
    ConsoleCommands::Execute(*me, msg.data.commandName, consoleArgs);
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
  uint32_t remoteId = FormIdCasts::LongToNormal(msg.remoteId);

  MpActor* me = partOne.serverState.ActorByUser(rawMsgData.userId);
  if (!me) {
    throw std::runtime_error("Unable to host without actor attached");
  }

  auto& remote = partOne.worldState.GetFormAt<MpObjectReference>(remoteId);

  auto user = partOne.serverState.UserByActor(remote.AsActor());
  if (user != Networking::InvalidUserId) {
    return;
  }

  auto& hoster = partOne.worldState.hosters[remoteId];
  const uint32_t prevHoster = hoster;

  auto remoteIdx = remote.GetIdx();

  std::optional<std::chrono::system_clock::time_point> lastRemoteUpdate;
  if (partOne.worldState.lastMovUpdateByIdx.size() > remoteIdx) {
    lastRemoteUpdate = partOne.worldState.lastMovUpdateByIdx[remoteIdx];
  }

  const auto hostResetTimeout = std::chrono::seconds(2);

  if (hoster == 0 || !lastRemoteUpdate ||
      std::chrono::system_clock::now() - *lastRemoteUpdate >
        hostResetTimeout) {
    partOne.GetLogger().info("Hoster changed from {0:x} to {0:x}", prevHoster,
                             me->GetFormId());
    hoster = me->GetFormId();
    remote.UpdateHoster(hoster);

    // Prevents too fast host switch
    partOne.worldState.lastMovUpdateByIdx[remoteIdx] =
      std::chrono::system_clock::now();

    auto remoteAsActor = remote.AsActor();
    if (remoteAsActor) {
      remoteAsActor->EquipBestWeapon();
    }

    uint64_t longFormId = remote.GetFormId();
    if (remoteAsActor && longFormId < 0xff000000) {
      longFormId += 0x100000000;
    }

    HostStartMessage message;
    message.target = longFormId;
    partOne.GetSendTarget().Send(rawMsgData.userId, message, true);

    // Otherwise, health percentage would remain unsynced until someone hits
    // npc
    auto formId = remote.GetFormId();
    partOne.worldState.SetTimer(std::chrono::seconds(1))
      .Then([this, formId](Viet::Void) {
        // Check if form is still here
        auto& remote = partOne.worldState.GetFormAt<MpActor>(formId);

        auto changeForm = remote.GetChangeForm();

        ChangeValuesMessage msg;
        msg.idx = remote.GetIdx();
        msg.data.health = changeForm.actorValues.healthPercentage;
        msg.data.magicka = changeForm.actorValues.magickaPercentage;
        msg.data.stamina = changeForm.actorValues.staminaPercentage;
        remote.GetActorToSendTo().SendToUser(msg, true);
      });

    auto& prevHosterForm = partOne.worldState.LookupFormById(prevHoster);
    if (MpActor* prevHosterActor =
          prevHosterForm ? prevHosterForm->AsActor() : nullptr) {
      auto prevHosterUser = partOne.serverState.UserByActor(prevHosterActor);
      if (prevHosterUser != Networking::InvalidUserId &&
          prevHosterUser != rawMsgData.userId) {
        HostStopMessage message;
        message.target = longFormId;
        partOne.GetSendTarget().Send(prevHosterUser, message, true);
      }
    }
  }
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

  if (actor->ShouldSkipRestoration()) {
    return;
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

  const float baseHealth =
    GetBaseActorValues(espmProvider, baseId, raceId, actor.GetTemplateChain())
      .health;

  if (outBaseHealth) {
    *outBaseHealth = baseHealth;
  }

  const float damagePercentage = damage / baseHealth;
  const float currentHealthPercentage = healthPercentage - damagePercentage;

  /// TODO add check for nan and inf!
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

bool ShouldBeBlocked(const MpActor& aggressor, const MpActor& target)
{
  NiPoint3 targetViewDirection = target.GetViewDirection();
  NiPoint3 aggressorDirection = aggressor.GetPos() - target.GetPos();
  if (targetViewDirection * aggressorDirection <= 0) {
    return false;
  }
  float angle =
    std::acos((targetViewDirection * aggressorDirection) /
              (targetViewDirection.Length() * aggressorDirection.Length()));
  return angle < 1;
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
    auto it = partOne.worldState.hosters.find(hitData.aggressor);
    if (it == partOne.worldState.hosters.end() ||
        it->second != myActor->GetFormId()) {
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
    spdlog::error("ActionListener::OnHit - MpObjectReference not found for "
                  "hitData.target {:x}",
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
    spdlog::debug(fmt::format("{:x} actor is dead and can't attack. "
                              "requesting respawn in order to fix death state",
                              aggressor->GetFormId()));
    aggressor->RespawnWithDelay(true);
    return;
  }

  auto sourceInEspm =
    partOne.GetEspm().GetBrowser().LookupById(hitData.source);

  const bool isSourceSpell =
    sourceInEspm.rec && sourceInEspm.rec->GetType() == espm::SPEL::kType;

  const auto equipment = aggressor->GetEquipment();

  if (isSourceSpell && equipment.IsSpellEquipped(hitData.source)) {
    OnSpellHit(aggressor, targetRef, hitData);
    return;
  }

  const bool isUnarmed = IsUnarmedAttack(hitData.source);

  if (equipment.inv.HasItem(hitData.source) || isUnarmed) {
    // thuum docs/verbs/damage-flags.md: the facts the flag rule needs; the
    // rule is Rust's (skymp-wire wire-rules damage, ADR-020)
    if (aggressor == myActor &&
        (hitData.isPowerAttack || hitData.isSneakAttack)) {
      const auto lastPowerStart =
        partOne.animationSystem.GetLastPowerAttackStartTime(*aggressor);
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
      hitData.isPowerAttack = kept.power;
      hitData.isSneakAttack = kept.sneak;
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
    OnWeaponHit(aggressor, targetRef, hitData, isUnarmed);
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
    const auto it = partOne.worldState.hosters.find(spellCastData.caster);

    if (it == partOne.worldState.hosters.end() ||
        it->second != myActor->GetFormId()) {
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
    spdlog::info(fmt::format("{:x} actor is dead and can't spell cast. "
                             "requesting respawn in order to fix death state",
                             caster->GetFormId()));
    caster->RespawnWithDelay(true);
    return;
  }

  const auto equipment = caster->GetEquipment();

  if (equipment.IsSpellEquipped(spellCastData.spell) == false) {
    spdlog::info("ActionListener::OnSpellCast - spell {0:x} not "
                 "found in equipment",
                 spellCastData.spell);
    return;
  }

  SendToNeighbours(myActor->idx, rawMsgData);

  if (spellCastData.interruptCast) {
    return;
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
                                const HitData& hitData)
{
  SendPapyrusOnHitEvent(aggressor, targetRef, hitData);

  auto* targetActorPtr = targetRef ? targetRef->AsActor() : nullptr;
  if (!targetActorPtr) {
    return; // Not an actor, damage calculation is not needed
  }

  auto targetActorValues = targetActorPtr->GetChangeForm().actorValues;

  SpellCastData spellCastData{ aggressor->GetFormId(),
                               targetActorPtr->GetFormId(),
                               hitData.source,
                               false,
                               false,
                               SpellType::Left };

  float damage =
    partOne.CalculateDamage(*aggressor, *targetActorPtr, spellCastData);
  damage = damage <= 0.f ? 0.f : damage;

  targetActorValues.healthPercentage = CalculateCurrentHealthPercentage(
    *targetActorPtr, damage, targetActorValues.healthPercentage, nullptr);

  static const auto kHealthAvFilter =
    std::vector<espm::ActorValue>{ espm::ActorValue::Health };

  targetActorPtr->NetSetPercentages(targetActorValues, aggressor,
                                    kHealthAvFilter);

  spdlog::info("OnSpellHit - Target {0:x} is hit by {1:x} spell on {2} "
               "damage. By caster: {3:x})",
               spellCastData.target, spellCastData.spell, damage,
               spellCastData.caster);
}

void ActionListener::OnWeaponHit(MpActor* aggressor,
                                 MpObjectReference* targetRef, HitData hitData,
                                 [[maybe_unused]] bool isUnarmed)
{
  const auto currentHitTime = std::chrono::steady_clock::now();

  SendPapyrusOnHitEvent(aggressor, targetRef, hitData);

  auto* targetActorPtr = targetRef ? targetRef->AsActor() : nullptr;
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

  if (targetActor.IsBlockActive()) {
    if (ShouldBeBlocked(*aggressor, targetActor)) {
      bool isRemoteBowAttack = false;

      auto sourceLookupResult =
        targetActor.GetParent()->GetEspm().GetBrowser().LookupById(
          hitData.source);
      if (sourceLookupResult.rec &&
          sourceLookupResult.rec->GetType() == espm::WEAP::kType) {
        auto weapData =
          espm::GetData<espm::WEAP>(hitData.source, targetActor.GetParent());
        if (weapData.weapDNAM) {
          if (weapData.weapDNAM->animType == espm::WEAP::AnimType::Bow ||
              weapData.weapDNAM->animType == espm::WEAP::AnimType::Crossbow) {
            if (!hitData.isBashAttack) {
              isRemoteBowAttack = true;
            }
          }
        }
      }

      bool isBlockingByShield = false;

      auto targetActorEquipmentEntries =
        targetActor.GetEquipment().inv.entries;
      for (auto& entry : targetActorEquipmentEntries) {
        if (entry.GetWorn() != Inventory::Worn::None) {
          auto res =
            targetActor.GetParent()->GetEspm().GetBrowser().LookupById(
              entry.baseId);
          if (res.rec && res.rec->GetType() == espm::ARMO::kType) {
            auto data =
              espm::GetData<espm::ARMO>(entry.baseId, targetActor.GetParent());
            bool isShield = data.equipSlotId > 0;
            if (isShield) {
              isBlockingByShield = isShield;
            }
          }
        }
      }

      if (!isRemoteBowAttack || isBlockingByShield) {
        hitData.isHitBlocked = true;
      }
    }
  }

  float damage = partOne.CalculateDamage(*aggressor, targetActor, hitData);
  damage = damage < 0.f ? 0.f : damage;
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

  spdlog::debug(
    "OnWeaponHit - Target {0:x} is hit by {1} damage. Percentage was: {3}, "
    "percentage now: {2}, base health: {4})",
    hitData.target, damage, currentActorValues.healthPercentage,
    healthPercentage, outBaseHealth);

  NotifyHostility(*aggressor, targetActor, currentHitTime);
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
