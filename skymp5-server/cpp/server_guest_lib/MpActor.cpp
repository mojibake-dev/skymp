#include "MpActor.h"
#include "ActiveMagicEffectsMap.h"
#include "ActorValues.h"
#include "ActorValuesMessage.h"
#include "ChangeFormGuard.h"
#include "ChangeValuesMessage.h"
#include "Condition.h"
#include "ConditionsEvaluator.h"
#include "CropRegeneration.h"
#include "DamageCauseMessage.h"
#include "EvaluateTemplate.h"
#include "FormCallbacks.h"
#include "GetBaseActorValues.h"
#include "LeveledListUtils.h"
#include "LocationalDataUtils.h"
#include "MathUtils.h"
#include "MpChangeForms.h"
#include "MsgType.h"
#include "ServerState.h"
#include "SpSnippet.h"
#include "SpSnippetFunctionGen.h"
#include "WorldState.h"
#include "gamemode_events/DeathEvent.h"
#include "gamemode_events/DropItemEvent.h"
#include "gamemode_events/EatItemEvent.h"
#include "gamemode_events/ReadBookEvent.h"
#include "gamemode_events/RespawnEvent.h"
#include "libespm/NPC_.h"
#include "libespm/espm.h"
#include "papyrus-vm/Utils.h"
#include "script_objects/EspmGameObject.h"
#include "wire_bridge_cxx/rules.h"
#include <NiPoint3.h>
#include <TimeUtils.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <nlohmann/json.hpp>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <utility>

#include "ChangeValuesMessage.h"
#include "MagicEffectsMessage.h"
#include "TeleportMessage.h"
#include "UpdateEquipmentMessage.h"

// for PlaceAtMe used in MpActor::DropItem
#include "script_classes/PapyrusObjectReference.h"
#include "script_objects/MpFormGameObject.h"

struct MpActor::Impl
{
  // TODO: consider optimizing data structure and/or general refactoring
  std::set<std::shared_ptr<DestroyEventSink>> destroyEventSinks;
  std::set<std::shared_ptr<DisableEventSink>> disableEventSinks;

  std::map<uint32_t, Viet::Promise<VarValue>> snippetPromises;
  uint32_t snippetIndex = 0;
  uint32_t respawnTimerIndex = 0;
  bool isRespawning = false;
  // ADR-050 decision 2: the server's number reached zero, the host's report
  // awaited (MpActor::BeginPendingDeath)
  bool pendingDeath = false;
  uint32_t pendingDeathAggressor = 0;
  uint64_t pendingDeathSinceMs = 0;
  // npc-fate.md finding 48: the last death's time on the server's clock
  std::chrono::steady_clock::time_point diedAt{};
  // npc-fate.md H-52b: the killer credited at the last Kill (the read)
  std::optional<uint32_t> lastKillerId;
  std::optional<UpdateActionMessage> lastAction;
  // slice 5: the host's reading of the gaze and the face
  uint32_t hostedGazeTarget = 0;
  uint32_t hostedGazeSlot = 0;
  uint32_t hostedEmotionType = 0;
  uint32_t hostedEmotionValue = 0;
  bool isBlockActive = false;
  std::chrono::steady_clock::time_point lastAttributesUpdateTimePoint;
  std::vector<std::pair<uint32_t, std::chrono::steady_clock::time_point>>
    lastHitTimesLRU;
  std::chrono::steady_clock::time_point lastHitTakenTimePoint;
  // thuum docs/verbs/sleep.md: the last bed this player activated (0 when
  // none since its last rest) and when, and the Rested bonus's grant
  uint32_t lastBedId = 0;
  std::chrono::steady_clock::time_point lastBedTimePoint;
  uint64_t restedGrant = 0;
  // thuum docs/verbs/map-markers.md: the login's markers not sent yet
  bool mapMarkersPending = false;
  // thuum docs/verbs/racemenu-sync.md: the server opened the race menu and
  // the RaceMenu look from it has not come yet
  bool raceMenuLookDue = false;
  // thuum docs/verbs/learned-effects.md: the last ingredient eaten and when
  uint32_t lastEatenId = 0;
  std::chrono::steady_clock::time_point lastEatenTimePoint;
  // thuum docs/verbs/actor-values.md: values the server set that the
  // client has not reported yet, and whether the record a login sent waits
  // for a report that shows it applied
  std::vector<std::pair<uint8_t, float>> heldActorValues;
  // thuum docs/verbs/npc-fate.md (ADR-049): whether this actor came from a
  // saved change form (its roll kept) or was rolled by this process
  bool fromSave = false;
  bool actorValuesLoginPending = false;
  using RestorationTimePoints =
    std::unordered_map<espm::ActorValue,
                       std::chrono::steady_clock::time_point>;
  RestorationTimePoints restorationTimePoints = {
    { espm::ActorValue::Health, std::chrono::steady_clock::time_point{} },
    { espm::ActorValue::Stamina, std::chrono::steady_clock::time_point{} },
    { espm::ActorValue::Magicka, std::chrono::steady_clock::time_point{} },
    { espm::ActorValue::HealRate, std::chrono::steady_clock::time_point{} },
    { espm::ActorValue::MagickaRate, std::chrono::steady_clock::time_point{} },
    { espm::ActorValue::StaminaRate, std::chrono::steady_clock::time_point{} },
    { espm::ActorValue::HealRateMult_or_CombatHealthRegenMultMod,
      std::chrono::steady_clock::time_point{} },
    { espm::ActorValue::MagickaRateMult_or_CombatHealthRegenMultPowerMod,
      std::chrono::steady_clock::time_point{} },
    { espm::ActorValue::StaminaRateMult,
      std::chrono::steady_clock::time_point{} },
  };
  uint32_t blockActiveCount = 0;
  std::vector<std::pair<uint32_t, MpObjectReference*>> droppedItemsQueue;
  std::optional<AnimationData> animationData;

  // thuum docs/verbs/npc-stream.md (ADR-050): the host's reading of this
  // actor's combat target and running package, 0 for none
  uint32_t hostedCombatTarget = 0;
  uint32_t hostedPackage = 0;

  // thuum docs/verbs/god-mode.md: TGM and TIM for the session
  bool godMode = false;
  bool immortal = false;

  // thuum docs/verbs/npc-fate.md (ADR-048's B): the hits this actor took,
  // by aggressor, newest last, and its host's height samples for it within
  // MpActor::kHeightWindow; a death report's evidence
  std::vector<std::pair<uint32_t, std::chrono::steady_clock::time_point>>
    hitsTaken;
  std::deque<std::pair<std::chrono::steady_clock::time_point, float>> heights;

  // thuum docs/verbs/attributes.md: the percentages the server raised that
  // the client has not reported yet (wire-rules attributes); the former
  // 5 s blanket drop of reports after a drink lives here now, per value
  rust::Vec<skymp::rules::PercentHold> heldPercentages;
};

namespace {

// defined below with the running effects (the file's one wall clock)
uint64_t WallNowMs();

void RestoreActorValuePatched(MpActor* actor, espm::ActorValue actorValue,
                              float value)
{
  actor->RestoreActorValue(actorValue, value);
}

}

bool MpActor::TakeReportedPercentage(espm::ActorValue av, float reported)
{
  return skymp::rules::percentages_report(
    pImpl->heldPercentages, static_cast<uint8_t>(av), reported, WallNowMs());
}

MpActor::MpActor(const LocationalData& locationalData_,
                 const FormCallbacks& callbacks_, uint32_t optBaseId)
  : MpObjectReference(locationalData_, callbacks_,
                      optBaseId == 0 ? 0x7 : optBaseId, "NPC_")
{
  pImpl.reset(new Impl);
  asActor = this;
}

void MpActor::IncreaseBlockCount() noexcept
{
  ++pImpl->blockActiveCount;
}

void MpActor::ResetBlockCount() noexcept
{
  pImpl->blockActiveCount = 0;
}

uint32_t MpActor::GetBlockCount() const noexcept
{
  return pImpl->blockActiveCount;
}

bool MpActor::GetConsoleCommandsAllowedFlag() const
{
  return GetChangeForm().consoleCommandsAllowed;
}

void MpActor::SetConsoleCommandsAllowedFlag(bool newValue)
{
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.consoleCommandsAllowed = newValue;
  });
}

uint8_t MpActor::GetStaffRank() const
{
  const auto& changeForm = ChangeForm();
  if (changeForm.staffRank.has_value()) {
    return *changeForm.staffRank;
  }
  return changeForm.consoleCommandsAllowed ? 2 : 0;
}

std::optional<uint8_t> MpActor::GetRecordedStaffRank() const
{
  return ChangeForm().staffRank;
}

void MpActor::SetStaffRank(uint8_t rank)
{
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.staffRank = static_cast<uint8_t>(rank > 3 ? 3 : rank);
  });
}

void MpActor::EquipBestWeapon()
{
  if (!GetParent()->HasEspm()) {
    return;
  }

  auto& loader = GetParent()->GetEspm();
  auto& cache = GetParent()->GetEspmCache();

  const Equipment eq = GetEquipment();

  Equipment newEq;
  newEq.numChanges = eq.numChanges + 1;
  for (auto& entry : eq.inv.entries) {
    bool isEquipped = entry.GetWorn() != Inventory::Worn::None;
    bool isWeap =
      espm::GetRecordType(entry.baseId, GetParent()) == espm::WEAP::kType;
    if (isEquipped && isWeap) {
      continue;
    }
    newEq.inv.AddItems({ entry });
  }

  const Inventory& inv = GetInventory();
  Inventory::Entry bestEntry;
  int16_t bestDamage = -1;
  for (auto& entry : inv.entries) {
    if (entry.baseId) {
      auto lookupRes = loader.GetBrowser().LookupById(entry.baseId);
      if (auto weap = espm::Convert<espm::WEAP>(lookupRes.rec)) {
        if (!bestEntry.count ||
            weap->GetData(cache).weapData->damage > bestDamage) {
          bestEntry = entry;
          bestDamage = weap->GetData(cache).weapData->damage;
        }
      }
    }
  }

  if (bestEntry.count > 0) {
    bestEntry.SetWorn(Inventory::Worn::Right);
    newEq.inv.AddItems({ bestEntry });
  }

  SetEquipment(newEq);

  UpdateEquipmentMessage msg;
  msg.data = newEq;
  msg.idx = GetIdx();
  for (auto listener : GetActorListeners()) {
    listener->GetActorToSendTo().SendToUser(msg, true);
  }
}

void MpActor::AddSpell(const uint32_t spellId)
{
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.learnedSpells.LearnSpell(spellId);
  });
}

void MpActor::RemoveSpell(const uint32_t spellId)
{
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.learnedSpells.ForgetSpell(spellId);
  });
}

void MpActor::SetLastAnimEvent(
  const std::optional<AnimationData>& animationData)
{
  pImpl->animationData = animationData;
}

std::optional<AnimationData> MpActor::GetLastAnimEvent() const
{
  return pImpl->animationData;
}

void MpActor::SetJudgedPercentages(std::optional<float> health,
                                   std::optional<float> magicka,
                                   std::optional<float> stamina)
{
  if (!health && !magicka && !stamina) {
    return;
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    if (health) {
      changeForm.actorValues.healthPercentage = *health;
    }
    if (magicka) {
      changeForm.actorValues.magickaPercentage = *magicka;
    }
    if (stamina) {
      changeForm.actorValues.staminaPercentage = *stamina;
    }
  });
  SetLastAttributesPercentagesUpdate(std::chrono::steady_clock::now());
}

void MpActor::SetHostedState(uint32_t combatTarget, uint32_t package)
{
  pImpl->hostedCombatTarget = combatTarget;
  pImpl->hostedPackage = package;
}

void MpActor::SetHostedGaze(uint32_t target, uint32_t slot,
                            uint32_t emotionType, uint32_t emotionValue)
{
  pImpl->hostedGazeTarget = target;
  pImpl->hostedGazeSlot = slot;
  pImpl->hostedEmotionType = emotionType;
  pImpl->hostedEmotionValue = emotionValue;
}

uint32_t MpActor::GetHostedGazeTarget() const
{
  return pImpl->hostedGazeTarget;
}

uint32_t MpActor::GetHostedGazeSlot() const
{
  return pImpl->hostedGazeSlot;
}

uint32_t MpActor::GetHostedEmotionType() const
{
  return pImpl->hostedEmotionType;
}

uint32_t MpActor::GetHostedEmotionValue() const
{
  return pImpl->hostedEmotionValue;
}

uint32_t MpActor::GetHostedCombatTarget() const
{
  return pImpl->hostedCombatTarget;
}

uint32_t MpActor::GetHostedPackage() const
{
  return pImpl->hostedPackage;
}

void MpActor::SetRaceMenuOpen(bool isOpen)
{
  if (isOpen) {
    pImpl->raceMenuLookDue = true;
  }
  EditChangeForm(
    [&](MpChangeForm& changeForm) { changeForm.isRaceMenuOpen = isOpen; });
}

void MpActor::SetAppearance(const Appearance* newAppearance)
{
  EditChangeForm([&](MpChangeForm& changeForm) {
    if (newAppearance)
      changeForm.appearanceDump = newAppearance->ToJson();
    else
      changeForm.appearanceDump.clear();
  });
}

void MpActor::SetEquipment(const Equipment& newEquipment)
{
  EditChangeForm(
    [&](MpChangeForm& changeForm) { changeForm.equipment = newEquipment; });
}

void MpActor::SetHealthRespawnPercentage(float percentage)
{
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.healthRespawnPercentage = percentage;
  });
}

void MpActor::SetMagickaRespawnPercentage(float percentage)
{
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.magickaRespawnPercentage = percentage;
  });
}

void MpActor::SetStaminaRespawnPercentage(float percentage)
{
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.staminaRespawnPercentage = percentage;
  });
}

void MpActor::AddToFaction(Faction faction, bool lazyLoad)
{
  if (factionsLoaded == false && lazyLoad)
    LoadFactions();

  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    if (!changeForm.factions.has_value()) {
      changeForm.factions = std::vector<Faction>();
      changeForm.factions.value().push_back(faction);
    } else {
      for (const auto& fact : changeForm.factions.value()) {
        if (faction.formDesc == fact.formDesc) {
          return;
        }
      }
      changeForm.factions.value().push_back(faction);
    }
  });
}

bool MpActor::IsInFaction(FormDesc factionForm, bool lazyLoad)
{
  if (factionsLoaded == false && lazyLoad)
    LoadFactions();

  const auto& factions = GetChangeForm().factions;

  if (!factions.has_value()) {
    return false;
  }

  for (const auto& faction : factions.value()) {
    if (faction.formDesc == factionForm) {
      return true;
    }
  }
  return false;
}

std::vector<Faction> MpActor::GetFactions(int minFactionRank,
                                          int maxFactionRank, bool lazyLoad)
{
  if (factionsLoaded == false && lazyLoad)
    LoadFactions();

  std::vector<Faction> result = std::vector<Faction>();

  if (minFactionRank < -128 || minFactionRank > 127 || maxFactionRank < -128 ||
      maxFactionRank > 127 || minFactionRank > maxFactionRank) {
    spdlog::warn(
      "Actor.GetFactions - minRank > maxRank or out of range (-128/127)");
    return result;
  }

  const auto& factions = GetChangeForm().factions;

  if (!factions.has_value()) {
    return result;
  }

  for (const auto& faction : factions.value()) {
    if (faction.rank >= minFactionRank && faction.rank <= maxFactionRank) {
      result.push_back(faction);
    }
  }

  return result;
}

void MpActor::RemoveFromFaction(FormDesc factionForm, bool lazyLoad)
{
  if (factionsLoaded == false && lazyLoad)
    LoadFactions();

  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    if (!changeForm.factions.has_value()) {
      return;
    }

    auto& factions = changeForm.factions.value();
    factions.erase(std::remove_if(factions.begin(), factions.end(),
                                  [&](const Faction& faction) {
                                    return faction.formDesc == factionForm;
                                  }),
                   factions.end());
  });
}

void MpActor::VisitProperties(CreateActorMessage& message,
                              VisitPropertiesMode mode)
{
  const auto baseId = GetBaseId();
  const uint32_t raceId = GetAppearance() ? GetAppearance()->raceId : 0;

  BaseActorValues baseActorValues;
  WorldState* worldState = GetParent();
  // this "if" is needed for unit testing: tests can call VisitProperties
  // without espm attached, which will cause tests to fail
  if (worldState && worldState->HasEspm()) {
    baseActorValues = GetBaseActorValues(worldState, baseId, raceId,
                                         ChangeForm().templateChain);
  }

  MpChangeForm changeForm = GetChangeForm();

  MpObjectReference::VisitProperties(message, mode);

  if (mode == VisitPropertiesMode::All && IsRaceMenuOpen()) {
    message.props.isRaceMenuOpen = true;
  }

  if (mode == VisitPropertiesMode::All) {
    baseActorValues.VisitBaseActorValuesAndPercentages(baseActorValues,
                                                       changeForm, message);
  }

  // thuum docs/verbs/figure-values.md: another game's figure of this player
  // carries the player's recorded maxima as its base values, instead of the
  // million the client used to set, so a script there reads Skyrim's number
  // (Finger of Death's threshold, Nature's Balance's swap). Nothing travels
  // before the first record: the figure keeps the engine's defaults
  if (mode == VisitPropertiesMode::OnlyPublic) {
    if (const auto maxima = RecordedMaxima()) {
      message.props.health = maxima->health;
      message.props.magicka = maxima->magicka;
      message.props.stamina = maxima->stamina;
    }
  }

  message.props.learnedSpells = changeForm.learnedSpells.GetLearnedSpells();

  if (!changeForm.templateChain.empty()) {
    std::vector<uint32_t> templateChain;
    templateChain.reserve(changeForm.templateChain.size());

    for (auto& element : changeForm.templateChain) {
      templateChain.push_back(element.ToFormId(GetParent()->espmFiles));
    }

    message.props.templateChain = std::move(templateChain);
  }

  // thuum docs/verbs/npc-stream.md (playtest 35, Hilde's tanning missing on
  // a seat that joined two minutes into it): a hosted NPC's last action
  // rides to a late joiner as the CreateActor's custom property "lastAction"
  // (the record's own JSON-valued property channel, as isHostedByOther
  // travels; no wire row, so the legacy capture and SCHEMA_VERSION stand),
  // and its puppet replays it once as the view's first action. No player
  // guard: OnUpdateAction records an action for a hosted actor alone, never
  // for the sender's own, and a created actor's base of 0 (the lab's and the
  // tests' placed NPCs) reads as a player's to IsCreatedAsPlayer
  if (pImpl->lastAction) {
    const auto& last = *pImpl->lastAction;
    nlohmann::json value{
      { "action", last.action },
      { "idle", last.idle },
      { "eventName", last.eventName },
      { "type", last.type },
      { "actorAnimationVariables",
        { { "booleans", last.actorAnimationVariables.booleans },
          { "floats", last.actorAnimationVariables.floats },
          { "integers", last.actorAnimationVariables.integers } } }
    };
    CustomPropsEntry entry;
    entry.propName = "lastAction";
    entry.propValueJsonDump = value.dump();
    message.customPropsJsonDumps.push_back(std::move(entry));
  }
}

void MpActor::Enable()
{
  if (!ChangeForm().isDisabled) {
    return;
  }
  MpObjectReference::Enable();
  // thuum docs/research/m3/decisions-after-playtest-34.md D21: a dead actor
  // back in the world resumes its return. ScheduleFateRespawn stamps the
  // day when the death left none (it died disabled: the clock runs from
  // now) and keeps the death's own stamp otherwise (disabled while the body
  // lay: the sweep dropped it, the hours still count from the death)
  if (IsDead() && IsFateRespawned()) {
    ScheduleFateRespawn();
    spdlog::info("[verb:npc-fate] {:x} enabled dead: its return scheduled "
                 "(D21)",
                 GetFormId());
  }
}

void MpActor::Disable()
{
  if (ChangeForm().isDisabled) {
    return;
  }

  for (auto& sink : pImpl->disableEventSinks) {
    sink->BeforeDisable(*this);
  }

  MpObjectReference::Disable();

  for (auto [snippetIdx, promise] : pImpl->snippetPromises) {
    spdlog::warn("Disabling actor {:x} with pending snippet promise",
                 GetFormId());
    try {
      promise.Resolve(VarValue::None());
    } catch (std::exception& e) {
      // Not sure if this is possible, but better safe than sorry
      spdlog::error("Exception while resolving pending snippet promise: {}",
                    e.what());
    }
  }

  pImpl->snippetPromises.clear();
}

void MpActor::SendToUser(const IMessageBase& message, bool reliable)
{
  if (callbacks->sendToUser) {
    callbacks->sendToUser(this, message, reliable);
  } else {
    throw std::runtime_error("sendToUser is nullptr");
  }
}

void MpActor::SendToUserDeferred(const IMessageBase& message, bool reliable,
                                 int deferredChannelId,
                                 bool overwritePreviousChannelMessages)
{
  if (callbacks->sendToUserDeferred) {
    callbacks->sendToUserDeferred(this, message, reliable, deferredChannelId,
                                  overwritePreviousChannelMessages);
  } else {
    throw std::runtime_error("sendToUserDeferred is nullptr");
  }
}

Networking::UserId MpActor::GetUserId() const
{
  if (callbacks->getUserId) {
    return callbacks->getUserId(const_cast<MpActor*>(this));
  } else {
    throw std::runtime_error("getUserId is nullptr");
  }
}

MpActor& MpActor::GetActorToSendTo()
{
  // Only send to hoster if actor is offline (no active user)
  // This fixes December 2023 Update "invisible chat" bug
  bool isOffline = GetUserId() == Networking::InvalidUserId;

  auto worldState = GetParent();

  if (isOffline && worldState) {
    // thuum docs/verbs/cell-host.md: the game hosting this actor's cell
    if (MpActor* host = worldState->HostOf(*this)) {
      // Send messages such as Teleport, ChangeValues to our host
      return *host;
    }
  }

  return *this;
}

bool MpActor::OnEquip(uint32_t baseId)
{
  const auto& espm = GetParent()->GetEspm();
  auto lookupRes = espm.GetBrowser().LookupById(baseId);

  if (!lookupRes.rec) {
    return false;
  }

  const auto recordType = lookupRes.rec->GetType();

  const bool isSpell = recordType == "SPEL";
  const bool isBook = recordType == "BOOK";
  const bool isIngredient = recordType == "INGR";
  const bool isPotion = recordType == "ALCH";

  bool isTorch = false;
  if (espm::utils::Is<espm::LIGH>(recordType)) {
    auto& compressedFieldsCache = GetParent()->GetEspmCache();
    auto light = espm::Convert<espm::LIGH>(lookupRes.rec);
    auto res = light->GetData(compressedFieldsCache);
    isTorch = res.data.flags & espm::LIGH::Flags::CanBeCarried;
  }

  const bool isScroll = recordType == "SCRL";
  const bool isWeapon = recordType == "WEAP";
  const bool isArmor = recordType == "ARMO";
  const bool isAmmo = recordType == "AMMO";

  if (!(isSpell || isIngredient || isPotion || isBook || isTorch || isScroll ||
        isWeapon || isArmor || isAmmo)) {
    return false;
  }

  const bool hasItem = isSpell
    ? ChangeForm().learnedSpells.IsSpellLearned(baseId)
    : GetInventory().GetItemCount(baseId) > 0;

  if (!hasItem) {
    return false;
  }

  // thuum docs/verbs/poison.md: a poison's OnEquip is its player putting it
  // on the right-hand weapon, never a drink. The game's box is its only use
  // of a poison for its player; the engine sends no equip event for it, and
  // the client reports the game's PoisonedWeapon event as this.
  if (isPotion) {
    bool isPoison = false;
    try {
      isPoison = espm::GetData<espm::ALCH>(baseId, GetParent()).isPoison;
    } catch (std::exception& e) {
      spdlog::warn("MpActor::OnEquip {:x} - {:x}: {}", GetFormId(), baseId,
                   e.what());
    }
    if (isPoison) {
      return ApplyPoison(baseId);
    }
  }

  bool spellLearned = false;
  if (isIngredient || isPotion) {
    // thuum docs/verbs/magic-effects.md: observers see a drink's effects
    // through MagicEffects, as every effect, and no longer through the
    // drinker's figure drinking it in their games (which also had a poison
    // "drunk" there)
    EatItem(baseId, recordType);
  } else if (isBook) {
    spellLearned = ReadBook(baseId);
  }

  if (isIngredient || isPotion || spellLearned) {
    RemoveItem(baseId, 1, nullptr);
  }

  const VarValue args[] = {
    VarValue(std::make_shared<EspmGameObject>(lookupRes)), VarValue::None()
  };

  SendPapyrusEvent("OnObjectEquipped", args, std::size(args));

  return true;
}

namespace {
// thuum docs/verbs/poison.md: the doses a poison gives a weapon without
// perks, the game's 1.0 floored (the re-analyst's read of the box's callback,
// 2026-10-12; CONFIRMED by x-poison-probe3 20261013-001004). Perks change
// it through the kModPoisonDoseCount entry point, which waits for M5.
constexpr uint32_t kPoisonDoses = 1;

bool TakesPoison(WorldState& worldState, uint32_t weaponId)
{
  const auto lookup = worldState.GetEspm().GetBrowser().LookupById(weaponId);
  if (!lookup.rec || lookup.rec->GetType() != espm::WEAP::kType) {
    return false;
  }
  const auto weap = espm::GetData<espm::WEAP>(weaponId, &worldState);
  return weap.weapDNAM &&
    weap.weapDNAM->animType != espm::WEAP::AnimType::Other &&
    weap.weapDNAM->animType != espm::WEAP::AnimType::Staff;
}
}

bool MpActor::ApplyPoison(uint32_t poisonId)
{
  auto* worldState = GetParent();
  if (!worldState || GetInventory().GetItemCount(poisonId) == 0) {
    spdlog::warn("MpActor::ApplyPoison {:x} - E_POISON_NOT_HELD: {:x}",
                 GetFormId(), poisonId);
    return false;
  }

  // the weapon worn in the right hand, as the player's game reported it
  std::optional<uint32_t> weaponId;
  for (const auto& entry : GetEquipment().inv.entries) {
    if (entry.GetWorn() == Inventory::Worn::Right &&
        TakesPoison(*worldState, entry.baseId)) {
      weaponId = entry.baseId;
      break;
    }
  }
  if (!weaponId) {
    spdlog::warn("MpActor::ApplyPoison {:x} - E_POISON_NO_WEAPON: {:x} with "
                 "no weapon that takes it in the right hand",
                 GetFormId(), poisonId);
    return false;
  }

  Inventory inv = GetInventory();
  const auto isWeapon = [&](const Inventory::Entry& e) {
    return e.baseId == *weaponId && e.count > 0;
  };
  if (std::any_of(inv.entries.begin(), inv.entries.end(),
                  [&](const Inventory::Entry& e) {
                    return isWeapon(e) && e.poisonId.has_value();
                  })) {
    spdlog::warn("MpActor::ApplyPoison {:x} - E_POISON_ALREADY: {:x} carries "
                 "a poison",
                 GetFormId(), *weaponId);
    return false;
  }
  auto it = std::find_if(inv.entries.begin(), inv.entries.end(), isWeapon);
  if (it == inv.entries.end()) {
    spdlog::warn("MpActor::ApplyPoison {:x} - E_POISON_NO_WEAPON: {:x} is "
                 "not in the server's inventory",
                 GetFormId(), *weaponId);
    return false;
  }

  // one copy takes the mark: the game poisons the worn copy only. A copy
  // alone takes it where it stands, so the inventory keeps its order (as the
  // legacy stack's drink leaves it, which difftest compares); a stack gives
  // one copy to an entry of its own
  Inventory::Entry marked = *it;
  marked.count = 1;
  marked.poisonId = poisonId;
  marked.poisonCount = kPoisonDoses;
  if (it->count == 1) {
    *it = marked;
  } else {
    it->count -= 1;
    inv.AddItems({ marked });
  }
  inv.RemoveItems({ Inventory::Entry(poisonId, 1) });
  SetInventory(inv);

  spdlog::info("MpActor::ApplyPoison {:x} - poison {:x} on {:x}, {} dose(s)",
               GetFormId(), poisonId, *weaponId, kPoisonDoses);
  return true;
}

std::optional<uint32_t> MpActor::SpendPoisonDose(uint32_t weaponId)
{
  Inventory inv = GetInventory();
  auto it = std::find_if(
    inv.entries.begin(), inv.entries.end(), [&](const Inventory::Entry& e) {
      return e.baseId == weaponId && e.count > 0 && e.poisonId.has_value() &&
        e.poisonCount.value_or(0) > 0;
    });
  if (it == inv.entries.end()) {
    return std::nullopt;
  }

  const uint32_t poisonId = *it->poisonId;
  Inventory::Entry spent = *it;
  spent.count = 1;
  const uint32_t left = *it->poisonCount - 1;
  if (left > 0) {
    spent.poisonCount = left;
  } else {
    spent.poisonId.reset();
    spent.poisonCount.reset();
  }
  // a copy alone changes where it stands unless it rejoins a stack of its
  // own kind, so the inventory keeps its order (see ApplyPoison)
  const bool joinsStack = std::any_of(
    inv.entries.begin(), inv.entries.end(),
    [&](const Inventory::Entry& e) { return e.EqualExceptCount(spent); });
  if (it->count == 1 && !joinsStack) {
    *it = spent;
  } else {
    if (it->count > 1) {
      it->count -= 1;
    } else {
      inv.entries.erase(it);
    }
    inv.AddItems({ spent });
  }
  SetInventory(inv);
  return poisonId;
}

void MpActor::AddEventSink(std::shared_ptr<DestroyEventSink> sink)
{
  pImpl->destroyEventSinks.insert(sink);
}

void MpActor::RemoveEventSink(std::shared_ptr<DestroyEventSink> sink)
{
  pImpl->destroyEventSinks.erase(sink);
}

void MpActor::AddEventSink(std::shared_ptr<DisableEventSink> sink)
{
  pImpl->disableEventSinks.insert(sink);
}

void MpActor::RemoveEventSink(std::shared_ptr<DisableEventSink> sink)
{
  pImpl->disableEventSinks.erase(sink);
}

MpChangeForm MpActor::GetChangeForm() const
{
  auto res = MpObjectReference::GetChangeForm();
  res.recType = MpChangeForm::ACHR;
  return res;
}

void MpActor::ApplyChangeForm(const MpChangeForm& newChangeForm)
{
  if (newChangeForm.recType != MpChangeForm::ACHR) {
    throw std::runtime_error(
      "Expected record type to be ACHR, but found REFR");
  }
  MpObjectReference::ApplyChangeForm(newChangeForm);
  EditChangeForm(
    [&](MpChangeForm& changeForm) {
      changeForm = static_cast<const MpChangeForm&>(newChangeForm);

      // Actor without appearance would not be visible so we force player to
      // choose appearance
      if (changeForm.appearanceDump.empty()) {
        changeForm.isRaceMenuOpen = true;
      }

      // ActorValues does not refelect real base actor values set in esp/esm
      // game files since new update
      // this check is added only for test as a workaround. It is to be redone
      // in the nearest future. TODO
      if (GetParent() && GetParent()->HasEspm()) {
        EnsureTemplateChainEvaluated(GetParent()->GetEspm(),
                                     Mode::NoRequestSave);
        // The game files give the base values; the percentages are the
        // actor's own state and come from the record, or every restart
        // heals every actor (thuum docs/verbs/attributes.md)
        const ActorValues recorded = changeForm.actorValues;
        changeForm.actorValues = GetBaseActorValues(
          GetParent(), GetBaseId(), GetRaceId(), changeForm.templateChain);
        changeForm.actorValues.healthPercentage = recorded.healthPercentage;
        changeForm.actorValues.magickaPercentage = recorded.magickaPercentage;
        changeForm.actorValues.staminaPercentage = recorded.staminaPercentage;
      }
    },
    Mode::NoRequestSave);
  ReapplyMagicEffects();

  // a race menu the record keeps open is shown again at the login
  if (ChangeForm().isRaceMenuOpen) {
    pImpl->raceMenuLookDue = true;
  }

  // We do the same in PartOne::SetUserActor for player characters
  if (IsDead() && !IsRespawning()) {
    spdlog::info("MpActor::ApplyChangeForm {:x} - respawning dead actor",
                 GetFormId());
    RespawnWithDelay();
  }

  // Mirrors MpObjectReference impl
  // Perform all required grid operations
  newChangeForm.isDisabled ? Disable() : Enable();
  SetCellOrWorldObsolete(newChangeForm.worldOrCellDesc);
  SetPos(newChangeForm.position);
}

uint32_t MpActor::NextSnippetIndex(
  std::optional<Viet::Promise<VarValue>> promise)
{
  auto res = pImpl->snippetIndex++;
  if (promise) {
    pImpl->snippetPromises[res] = *promise;
  }
  return res;
}

void MpActor::ResolveSnippet(uint32_t snippetIdx, VarValue v)
{
  auto it = pImpl->snippetPromises.find(snippetIdx);
  if (it != pImpl->snippetPromises.end()) {
    auto& promise = it->second;
    promise.Resolve(v);
    pImpl->snippetPromises.erase(it);
  }
}

void MpActor::SetPercentage(espm::ActorValue av, float percentage)
{
  if (IsDead() || pImpl->isRespawning) {
    return;
  }
  if (av == espm::ActorValue::Health && percentage <= 0.f) {
    if (pImpl->immortal || !MayDie(nullptr)) {
      // thuum docs/verbs/god-mode.md: TIM, one point left instead of death;
      // the same point for an essential or protected base (npc-fate.md)
      percentage = 1.f / std::max(GetMaximumValues().health, 1.f);
    } else {
      Kill(nullptr);
      return;
    }
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    switch (av) {
      case espm::ActorValue::Health:
        changeForm.actorValues.healthPercentage = percentage;
        break;
      case espm::ActorValue::Magicka:
        changeForm.actorValues.magickaPercentage = percentage;
        break;
      case espm::ActorValue::Stamina:
        changeForm.actorValues.staminaPercentage = percentage;
        break;
      default:
        break;
    }
  });

  // Updating timestamp. Note: calling this multiple times for different AVs
  // is fine but might be slightly inefficient if batched.
  SetLastAttributesPercentagesUpdate(std::chrono::steady_clock::now());
}

void MpActor::SetPercentages(const ActorValues& actorValues,
                             MpActor* aggressor)
{
  if (IsDead() || pImpl->isRespawning) {
    return;
  }
  float health = ImmortalHealth(actorValues.healthPercentage);
  if (health == 0.f) {
    if (MayDie(aggressor)) {
      // thuum docs/verbs/npc-fate.md (ADR-050 decision 2): a hosted NPC's
      // death is its host's engine's, from the cause this number sent; the
      // record reads zero now and the kill waits for the report (or the
      // window, PartOne::TickDeaths)
      auto worldState = GetParent();
      if (worldState && worldState->npcEngineDeaths && HostedBy()) {
        BeginPendingDeath(aggressor);
      } else {
        Kill(aggressor);
        return;
      }
    } else {
      // essential, or protected and not a player's hand: one point left
      health = 1.f / std::max(GetMaximumValues().health, 1.f);
    }
  }
  // a value the server raises holds against the client's reports until one
  // reaches it (thuum docs/verbs/attributes.md); a lowered one needs no
  // hold, the crop takes a stale higher report
  const ActorValues before = GetChangeForm().actorValues;
  const auto nowMs = WallNowMs();
  const auto raise = [&](espm::ActorValue av, float was, float is) {
    if (is > was + skymp::rules::attributes_applied_within()) {
      skymp::rules::percentages_hold(pImpl->heldPercentages,
                                     static_cast<uint8_t>(av), is, nowMs);
    }
  };
  raise(espm::ActorValue::Health, before.healthPercentage, health);
  raise(espm::ActorValue::Magicka, before.magickaPercentage,
        actorValues.magickaPercentage);
  raise(espm::ActorValue::Stamina, before.staminaPercentage,
        actorValues.staminaPercentage);
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.actorValues.healthPercentage = health;
    changeForm.actorValues.magickaPercentage = actorValues.magickaPercentage;
    changeForm.actorValues.staminaPercentage = actorValues.staminaPercentage;
  });
  SetLastAttributesPercentagesUpdate(std::chrono::steady_clock::now());
}

void MpActor::NetSendChangeValues(
  const ActorValues& actorValues,
  const std::optional<std::vector<espm::ActorValue>>& avFilter)
{
  ChangeValuesMessage message;
  message.idx = GetIdx();

  static const std::vector<espm::ActorValue> kDefaultAvFilter = {
    espm::ActorValue::Health, espm::ActorValue::Magicka,
    espm::ActorValue::Stamina
  };

  const std::vector<espm::ActorValue>& avFilterRef =
    avFilter.has_value() ? *avFilter : kDefaultAvFilter;

  int numUpdatedValues = 0;

  if (avFilterRef.empty()) {
    // If no filter is provided, send all actor values
    message.data.health = actorValues.healthPercentage;
    message.data.magicka = actorValues.magickaPercentage;
    message.data.stamina = actorValues.staminaPercentage;
  } else {
    // Filter actor values based on the provided filter
    for (const auto& av : avFilterRef) {
      switch (av) {
        case espm::ActorValue::Health:
          message.data.health = actorValues.healthPercentage;
          numUpdatedValues++;
          break;
        case espm::ActorValue::Magicka:
          message.data.magicka = actorValues.magickaPercentage;
          numUpdatedValues++;
          break;
        case espm::ActorValue::Stamina:
          message.data.stamina = actorValues.staminaPercentage;
          numUpdatedValues++;
          break;
        default:
          break;
      }
    }
  }

  if (numUpdatedValues > 0) {
    GetActorToSendTo().SendToUser(message, true);
  }
}

void MpActor::NetSetPercentages(
  const ActorValues& actorValues, MpActor* aggressor,
  const std::optional<std::vector<espm::ActorValue>>& avFilter)
{
  // an immortal actor's game hears its one point, never nothing, which its
  // own game would take for a death (thuum docs/verbs/god-mode.md)
  ActorValues values = actorValues;
  values.healthPercentage = ImmortalHealth(values.healthPercentage);
  static const std::vector<espm::ActorValue> kAll = {
    espm::ActorValue::Health, espm::ActorValue::Magicka,
    espm::ActorValue::Stamina
  };
  const std::vector<espm::ActorValue>& touched =
    avFilter.has_value() && !avFilter->empty() ? *avFilter : kAll;
  const bool healthTouched =
    std::find(touched.begin(), touched.end(), espm::ActorValue::Health) !=
    touched.end();
  auto worldState = GetParent();
  MpActor* host = HostedBy();
  // thuum docs/verbs/npc-fate.md (ADR-050 decision 2, deaths by the engine):
  // a hosted NPC's health reaches its host as a CAUSE (the resulting
  // percentage, the aggressor), which the host's engine applies through its
  // own damage entry; its other values as before; every other listener
  // hears the resulting values at once (the attacker's bar moves within a
  // round trip, not at the host's next sample)
  if (host && healthTouched && worldState && worldState->npcEngineDeaths) {
    DamageCauseMessage cause;
    cause.idx = GetIdx();
    cause.attacker = aggressor ? aggressor->GetFormId() : 0;
    cause.health = values.healthPercentage;
    cause.killMove = false;
    host->SendToUser(cause, true);
    std::vector<espm::ActorValue> rest;
    for (const auto av : touched) {
      if (av != espm::ActorValue::Health) {
        rest.push_back(av);
      }
    }
    if (!rest.empty()) {
      NetSendChangeValues(values, rest);
    }
    SendValuesToOtherListeners(values, touched, host);
  } else {
    NetSendChangeValues(values, avFilter);
  }
  SetPercentages(values, aggressor);
}

MpActor* MpActor::HostedBy()
{
  if (IsCreatedAsPlayer() || GetUserId() != Networking::InvalidUserId) {
    return nullptr;
  }
  auto worldState = GetParent();
  if (!worldState) {
    return nullptr;
  }
  return worldState->HostOf(*this);
}

void MpActor::SendValuesToOtherListeners(
  const ActorValues& values, const std::vector<espm::ActorValue>& touched,
  MpActor* host)
{
  ChangeValuesMessage out;
  out.idx = GetIdx();
  for (const auto av : touched) {
    switch (av) {
      case espm::ActorValue::Health:
        out.data.health = values.healthPercentage;
        break;
      case espm::ActorValue::Magicka:
        out.data.magicka = values.magickaPercentage;
        break;
      case espm::ActorValue::Stamina:
        out.data.stamina = values.staminaPercentage;
        break;
      default:
        break;
    }
  }
  // every listener with a user but the host (OnHostedActorState's loop)
  for (auto listener : GetActorListeners()) {
    if (listener == host ||
        listener->GetUserId() == Networking::InvalidUserId) {
      continue;
    }
    listener->SendToUser(out, true);
  }
}

void MpActor::KillOrDefer(MpActor* killer, bool killMove)
{
  auto worldState = GetParent();
  MpActor* host = HostedBy();
  if (!host || !worldState || !worldState->npcEngineDeaths || IsDead()) {
    Kill(killer);
    return;
  }
  DamageCauseMessage cause;
  cause.idx = GetIdx();
  cause.attacker = killer ? killer->GetFormId() : 0;
  cause.health = 0.f;
  cause.killMove = killMove;
  host->SendToUser(cause, true);
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.actorValues.healthPercentage = 0.f;
  });
  SetLastAttributesPercentagesUpdate(std::chrono::steady_clock::now());
  BeginPendingDeath(killer);
}

void MpActor::BeginPendingDeath(MpActor* aggressor)
{
  if (pImpl->pendingDeath || IsDead()) {
    return;
  }
  pImpl->pendingDeath = true;
  pImpl->pendingDeathAggressor = aggressor ? aggressor->GetFormId() : 0;
  pImpl->pendingDeathSinceMs = static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
  if (auto worldState = GetParent()) {
    worldState->pendingDeaths.insert(GetFormId());
  }
  MpActor* host = HostedBy();
  spdlog::info("[verb:npc-fate] death pending on {:x}: the server's number "
               "reached zero (aggressor {:x}); its host {:x} reports within "
               "{} ms or the kill is the server's word",
               GetFormId(), pImpl->pendingDeathAggressor,
               host ? host->GetFormId() : 0u, kPendingDeathMs);
}

bool MpActor::HasPendingDeath() const
{
  return pImpl->pendingDeath;
}

uint64_t MpActor::PendingDeathSinceMs() const
{
  return pImpl->pendingDeathSinceMs;
}

uint32_t MpActor::PendingDeathAggressor() const
{
  return pImpl->pendingDeathAggressor;
}

float MpActor::ImmortalHealth(float healthPercentage)
{
  if (!pImpl->immortal || healthPercentage > 0.f) {
    return healthPercentage;
  }
  return 1.f / std::max(GetMaximumValues().health, 1.f);
}

std::chrono::steady_clock::time_point
MpActor::GetLastAttributesPercentagesUpdate()
{
  return pImpl->lastAttributesUpdateTimePoint;
}

std::chrono::steady_clock::time_point MpActor::GetLastHitTime(
  std::optional<uint32_t> targetId) const
{
  if (!targetId) {
    if (pImpl->lastHitTimesLRU.empty()) {
      return std::chrono::steady_clock::time_point();
    }
    return pImpl->lastHitTimesLRU.back().second;
  }

  for (const auto& entry : pImpl->lastHitTimesLRU) {
    if (entry.first == *targetId) {
      return entry.second;
    }
  }
  return std::chrono::steady_clock::time_point();
}

void MpActor::SetLastBed(uint32_t bedId,
                         std::chrono::steady_clock::time_point timePoint)
{
  pImpl->lastBedId = bedId;
  pImpl->lastBedTimePoint = timePoint;
}

std::pair<uint32_t, std::chrono::steady_clock::time_point>
MpActor::GetLastBed() const
{
  return { pImpl->lastBedId, pImpl->lastBedTimePoint };
}

uint64_t MpActor::NextRestedGrant()
{
  return ++pImpl->restedGrant;
}

uint64_t MpActor::GetRestedGrant() const
{
  return pImpl->restedGrant;
}

std::vector<MapMarker> MpActor::GetMapMarkers() const
{
  const auto& markers = ChangeForm().mapMarkers;
  return markers ? *markers : std::vector<MapMarker>();
}

bool MpActor::RecordMapMarker(const FormDesc& refr, bool canTravel)
{
  const auto& recorded = ChangeForm().mapMarkers;
  if (recorded) {
    for (const auto& marker : *recorded) {
      if (marker.refr == refr &&
          skymp::rules::map_marker_travel_after(marker.canTravel, canTravel) ==
            marker.canTravel) {
        return false;
      }
    }
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    if (!changeForm.mapMarkers) {
      changeForm.mapMarkers.emplace();
    }
    auto& markers = *changeForm.mapMarkers;
    for (auto& marker : markers) {
      if (marker.refr == refr) {
        marker.canTravel =
          skymp::rules::map_marker_travel_after(marker.canTravel, canTravel);
        return;
      }
    }
    markers.push_back(MapMarker{ refr, canTravel });
  });
  return true;
}

void MpActor::SetMapMarkersPending(bool pending)
{
  pImpl->mapMarkersPending = pending;
}

bool MpActor::TakeMapMarkersPending()
{
  return std::exchange(pImpl->mapMarkersPending, false);
}

std::vector<IngredientEffects> MpActor::GetIngredientEffects() const
{
  const auto& effects = ChangeForm().ingredientEffects;
  return effects ? *effects : std::vector<IngredientEffects>();
}

bool MpActor::RecordIngredientEffects(const FormDesc& ingredient, uint8_t mask)
{
  // what is recorded for it now (no entry counts as nothing known)
  uint8_t recordedMask = 0;
  if (const auto& recorded = ChangeForm().ingredientEffects) {
    for (const auto& entry : *recorded) {
      if (entry.ingredient == ingredient) {
        recordedMask = entry.mask;
      }
    }
  }
  const uint8_t newMask =
    skymp::rules::ingredient_effects_union(recordedMask, mask);
  if (newMask == recordedMask) {
    return false;
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    if (!changeForm.ingredientEffects) {
      changeForm.ingredientEffects.emplace();
    }
    auto& effects = *changeForm.ingredientEffects;
    for (auto& entry : effects) {
      if (entry.ingredient == ingredient) {
        entry.mask = newMask;
        return;
      }
    }
    effects.push_back(IngredientEffects{ ingredient, newMask });
  });
  return true;
}

ShoutRecord MpActor::GetShoutRecord() const
{
  const auto& record = ChangeForm().shoutRecord;
  return record ? *record : ShoutRecord();
}

namespace {
template <class Record>
auto& ShoutListOf(Record& record, MpActor::ShoutEntry kind)
{
  switch (kind) {
    case MpActor::ShoutEntry::Shout:
      return record.shouts;
    case MpActor::ShoutEntry::WordKnown:
      return record.wordsKnown;
    default:
      return record.wordsUnlocked;
  }
}
}

bool MpActor::RecordShoutEntry(ShoutEntry kind, const FormDesc& form)
{
  const ShoutRecord current = GetShoutRecord();
  const auto& list = ShoutListOf(current, kind);
  if (std::find(list.begin(), list.end(), form) != list.end()) {
    return false;
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    if (!changeForm.shoutRecord) {
      changeForm.shoutRecord.emplace();
    }
    ShoutListOf(*changeForm.shoutRecord, kind).push_back(form);
  });
  return true;
}

bool MpActor::HasShoutEntry(ShoutEntry kind, uint32_t formId) const
{
  const auto& record = ChangeForm().shoutRecord;
  auto* worldState = GetParent();
  if (!record || !worldState) {
    return false;
  }
  const auto& list = ShoutListOf(*record, kind);
  const auto form = FormDesc::FromFormId(formId, worldState->espmFiles);
  return std::find(list.begin(), list.end(), form) != list.end();
}

std::pair<uint32_t, std::chrono::steady_clock::time_point>
MpActor::GetLastEaten() const
{
  return { pImpl->lastEatenId, pImpl->lastEatenTimePoint };
}

std::vector<Favorite> MpActor::GetFavorites() const
{
  const auto& favorites = ChangeForm().favorites;
  return favorites ? *favorites : std::vector<Favorite>();
}

bool MpActor::SetFavorites(std::vector<Favorite> favorites)
{
  if (favorites == GetFavorites()) {
    return false;
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.favorites = std::move(favorites);
  });
  return true;
}

std::vector<PlayerGlobal> MpActor::GetPlayerGlobals() const
{
  const auto& globals = ChangeForm().playerGlobals;
  return globals ? *globals : std::vector<PlayerGlobal>();
}

bool MpActor::SetPlayerGlobals(std::vector<PlayerGlobal> globals)
{
  if (globals == GetPlayerGlobals()) {
    return false;
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.playerGlobals = std::move(globals);
  });
  return true;
}

std::optional<ActorValueRecord> MpActor::GetActorValueRecord() const
{
  return ChangeForm().actorValueRecord;
}

bool MpActor::SetActorValueRecord(ActorValueRecord record)
{
  if (GetActorValueRecord() == record) {
    return false;
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.actorValueRecord = std::move(record);
  });
  return true;
}

const std::vector<std::pair<uint8_t, float>>& MpActor::GetHeldActorValues()
  const
{
  return pImpl->heldActorValues;
}

void MpActor::SetHeldActorValues(std::vector<std::pair<uint8_t, float>> held)
{
  pImpl->heldActorValues = std::move(held);
}

bool MpActor::IsActorValuesLoginPending() const
{
  return pImpl->actorValuesLoginPending;
}

void MpActor::SetActorValuesLoginPending(bool pending)
{
  pImpl->actorValuesLoginPending = pending;
}

bool MpActor::SetActorValueBaseByServer(uint8_t av, float value)
{
  if (!skymp::rules::actor_values_set_ok(av, value)) {
    return false;
  }
  auto held = GetHeldActorValues();
  auto heldIt = std::find_if(held.begin(), held.end(),
                             [&](const auto& h) { return h.first == av; });
  if (heldIt != held.end()) {
    heldIt->second = value;
  } else {
    held.emplace_back(av, value);
  }
  SetHeldActorValues(std::move(held));

  auto record = GetActorValueRecord();
  if (!record) {
    return true; // enters the record with the player's first report
  }
  auto& bases = record->bases;
  auto baseIt = std::find_if(bases.begin(), bases.end(),
                             [&](const auto& b) { return b.first == av; });
  if (baseIt != bases.end()) {
    baseIt->second = value;
  } else {
    bases.emplace_back(av, value);
  }
  SetActorValueRecord(std::move(*record));
  SendActorValueRecord();
  return true;
}

std::optional<float> MpActor::GetRecordedActorValueBase(uint8_t av) const
{
  if (auto record = GetActorValueRecord()) {
    for (const auto& [a, base] : record->bases) {
      if (a == av) {
        return base;
      }
    }
  }
  for (const auto& [a, value] : GetHeldActorValues()) {
    if (a == av) {
      return value;
    }
  }
  return std::nullopt;
}

void MpActor::SendActorValueRecord()
{
  const auto record = GetActorValueRecord();
  if (!record) {
    return;
  }
  ActorValuesMessage message;
  for (const auto& [av, base] : record->bases) {
    message.bases.push_back(ActorValuesMessage::Base{ av, base });
  }
  for (const auto& skill : record->skills) {
    message.skills.push_back(ActorValuesMessage::Skill{
      skill.skill, skill.level, skill.xp, skill.threshold });
  }
  message.xp = record->xp;
  message.threshold = record->threshold;
  message.level = record->level;
  for (const auto& [skill, count] : record->legendary) {
    message.legendary.push_back(ActorValuesMessage::Legendary{ skill, count });
  }
  SendToUser(message, true);
}

std::string MpActor::GetRaceMenuPreset() const
{
  const auto& preset = ChangeForm().raceMenuPreset;
  return preset ? *preset : std::string();
}

bool MpActor::SetRaceMenuPreset(std::string preset)
{
  if (preset == GetRaceMenuPreset()) {
    return false;
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.raceMenuPreset = std::move(preset);
  });
  return true;
}

bool MpActor::TakeRaceMenuLookDue()
{
  return std::exchange(pImpl->raceMenuLookDue, false);
}

std::chrono::steady_clock::time_point MpActor::GetLastHitTakenTime() const
{
  return pImpl->lastHitTakenTimePoint;
}

void MpActor::SetLastHitTakenTime(
  std::chrono::steady_clock::time_point timePoint)
{
  pImpl->lastHitTakenTimePoint = timePoint;
}

void MpActor::SetLastAttributesPercentagesUpdate(
  std::chrono::steady_clock::time_point timePoint)
{
  pImpl->lastAttributesUpdateTimePoint = timePoint;
}

void MpActor::SetLastHitTime(uint32_t targetId,
                             std::chrono::steady_clock::time_point timePoint)
{
  constexpr size_t kMaxHitMemory = 16;

  auto& hits = pImpl->lastHitTimesLRU;

  auto it =
    std::find_if(hits.begin(), hits.end(), [targetId](const auto& entry) {
      return entry.first == targetId;
    });

  if (it != hits.end()) {
    it->second = timePoint;
    std::rotate(it, it + 1, hits.end());
    return;
  }

  if (kMaxHitMemory > 0) {
    if (pImpl->lastHitTimesLRU.size() >= kMaxHitMemory) {
      pImpl->lastHitTimesLRU.erase(pImpl->lastHitTimesLRU.begin());
    }
    pImpl->lastHitTimesLRU.push_back({ targetId, timePoint });
  }
}

size_t MpActor::CountRecentHits(std::chrono::duration<float> timeWindow) const
{
  size_t count = 0;
  if (pImpl->lastHitTimesLRU.size() > 0) {
    const auto now = std::chrono::steady_clock::now();
    for (const auto& entry : pImpl->lastHitTimesLRU) {
      if (now - entry.second < timeWindow) {
        count++;
      }
    }
  }
  return count;
}

std::chrono::duration<float> MpActor::GetDurationOfAttributesPercentagesUpdate(
  std::chrono::steady_clock::time_point now)
{
  std::chrono::duration<float> timeAfterRegeneration =
    now - pImpl->lastAttributesUpdateTimePoint;
  return timeAfterRegeneration;
}

const bool& MpActor::IsRaceMenuOpen() const
{
  return ChangeForm().isRaceMenuOpen;
}

const bool& MpActor::IsDead() const
{
  return ChangeForm().isDead;
}

const bool& MpActor::IsRespawning() const
{
  return pImpl->isRespawning;
}

bool MpActor::IsSpellLearned(const uint32_t spellId) const
{
  return ChangeForm().learnedSpells.IsSpellLearned(spellId) ||
    IsSpellLearnedFromBase(spellId);
}

bool MpActor::IsSpellLearnedFromBase(const uint32_t spellId) const
{
  const auto known = KnownBaseSpells();
  return std::find(known.begin(), known.end(), spellId) != known.end();
}

std::vector<uint32_t> MpActor::KnownBaseSpells() const
{
  std::vector<uint32_t> known;
  const auto npcData = espm::GetData<espm::NPC_>(GetBaseId(), GetParent());
  const auto npc = GetParent()->GetEspm().GetBrowser().LookupById(GetBaseId());

  const uint32_t raceId = npc.ToGlobalId(npcData.race);

  const auto raceData = espm::GetData<espm::RACE>(raceId, GetParent());
  const auto race = GetParent()->GetEspm().GetBrowser().LookupById(raceId);

  for (auto npcSpellRaw : npcData.spells) {
    known.push_back(npc.ToGlobalId(npcSpellRaw));
  }

  // thuum docs/verbs/npc-stream.md (ADR-050, train 53): the list of the
  // template the chain names for spells (NPC_'s UseSpelllist flag) where the
  // server has evaluated the chain; a chain not evaluated, or one that walks
  // into a leveled list, answers nothing here and the base's own list stands
  // (a hosted actor's cast of a spell outside this set is recorded, not
  // refused: ActionListener::OnSpellCast)
  std::string chainError;
  EvaluateTemplateNoThrow<espm::NPC_::UseSpelllist>(
    GetParent(), GetBaseId(), GetTemplateChain(),
    [&known](const auto& lookup, const auto& data) {
      for (auto raw : data.spells) {
        known.push_back(lookup.ToGlobalId(raw));
      }
      return true;
    },
    &chainError);

  for (auto raceSpellRaw : raceData.spells) {
    known.push_back(race.ToGlobalId(raceSpellRaw));
  }

  return known;
}

std::vector<uint32_t> MpActor::GetSpellList() const
{
  return ChangeForm().learnedSpells.GetLearnedSpells();
}

std::unique_ptr<const Appearance> MpActor::GetAppearance() const
{
  auto& changeForm = ChangeForm();
  if (changeForm.appearanceDump.size() > 0) {
    simdjson::dom::parser p;
    auto doc = p.parse(changeForm.appearanceDump).value();

    std::unique_ptr<const Appearance> res;
    res.reset(new Appearance(Appearance::FromJson(doc)));
    return res;
  }
  return nullptr;
}

const std::string& MpActor::GetAppearanceAsJson()
{
  return ChangeForm().appearanceDump;
}

namespace {
bool IsValidAnimEventName(const std::string& eventName)
{
  return std::all_of(eventName.begin(), eventName.end(),
                     [](char c) { return std::isalnum(c) || (c == '_'); });
}
}

std::string MpActor::GetLastAnimEventAsJson() const
{
  std::optional<AnimationData> anim = GetLastAnimEvent();

  if (!anim) {
    return "";
  }

  // Don't want bad anims to break JSON syntax
  if (!IsValidAnimEventName(anim->animEventName)) {
    return "";
  }

  std::string res = R"({"animEventName":)";
  res += '"' + anim->animEventName + '"';
  res += R"(,"numChanges":)";
  res += std::to_string(anim->numChanges);
  res += "}";
  return res;
}

const Equipment& MpActor::GetEquipment() const
{
  return ChangeForm().equipment;
}

uint32_t MpActor::GetRaceId() const
{
  const auto appearance = GetAppearance();

  if (appearance) {
    return appearance->raceId;
  }

  return EvaluateTemplate<espm::NPC_::UseTraits>(
    GetParent(), GetBaseId(), GetTemplateChain(),
    [](const auto& npcLookupResult, const auto& npcData) {
      return npcLookupResult.ToGlobalId(npcData.race);
    });
}

bool MpActor::IsWeaponDrawn() const
{
  return GetAnimationVariableBool("_skymp_isWeapDrawn");
}

espm::ObjectBounds MpActor::GetBounds() const
{
  return espm::GetData<espm::NPC_>(GetBaseId(), GetParent()).objectBounds;
}

const std::vector<FormDesc>& MpActor::GetTemplateChain() const
{
  return ChangeForm().templateChain;
}

bool MpActor::IsCreatedAsPlayer() const
{
  return GetFormId() >= 0xff000000 && GetBaseId() <= 0x7;
}

const ActorValues& MpActor::GetActorValues() const
{
  return ChangeForm().actorValues;
}

const ActiveMagicEffectsMap& MpActor::GetActiveMagicEffects() const
{
  return ChangeForm().activeMagicEffects;
}

int32_t MpActor::GetProfileId() const
{
  return ChangeForm().profileId;
}

float MpActor::GetHealthRespawnPercentage() const
{
  return ChangeForm().healthRespawnPercentage;
}

float MpActor::GetMagickaRespawnPercentage() const
{
  return ChangeForm().magickaRespawnPercentage;
}

float MpActor::GetStaminaRespawnPercentage() const
{
  return ChangeForm().staminaRespawnPercentage;
}

void MpActor::SendAndSetDeathState(bool isDead, bool shouldTeleport)
{
  spdlog::trace(
    "MpActor::SendAndSetDeathState {:x} - isDead: {}, shouldTeleport: {}",
    GetFormId(), isDead, shouldTeleport);

  if (isDead) {
    RecordDeathAt(std::chrono::steady_clock::now());
  }

  auto position = GetSpawnPoint();

  auto respawnMsg = GetDeathStateMsg(position, isDead, shouldTeleport);
  GetActorToSendTo().SendToUser(respawnMsg, true);

  // thuum docs/verbs/npc-fate.md, ADR-048's seam (docs/research/m3/
  // death-path.md section 2): a game-file actor's death is the server's word
  // on every seat. The container above reaches the host alone; the other
  // games learned a death from the host's relayed sample, which a puppet
  // applied with no record behind it (rule 5). Now the word itself goes to
  // every listening game, as the respawn's "false" already does
  // (RespawnEvent), and the client takes a puppet's death from it alone.
  // ADR-050 decision 3 (train 51): a server-made NPC's copies hear it too;
  // only a player's actor keeps its own path (the NPC rule keyed once on
  // "not a player", IsCreatedAsPlayer).
  if (isDead && !IsCreatedAsPlayer()) {
    SendMessageToActorListeners(CreatePropertyMessage_(this, "isDead", "true"),
                                true);
  }

  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.isDead = isDead;
    changeForm.actorValues.healthPercentage =
      isDead ? 0.f : GetHealthRespawnPercentage();
    changeForm.actorValues.magickaPercentage =
      isDead ? 0.f : GetMagickaRespawnPercentage();
    changeForm.actorValues.staminaPercentage =
      isDead ? 0.f : GetStaminaRespawnPercentage();
  });

  if (shouldTeleport) {
    SetCellOrWorldObsolete(position.cellOrWorldDesc);
    SetPos(position.pos);
    SetAngle(position.rot);
  }

  // thuum docs/research/m3/decisions-after-playtest-34.md D18: the cell host
  // table hears a player's death and respawn here (PartOne installs it)
  if (auto worldState = GetParent(); worldState && worldState->onDeathState) {
    worldState->onDeathState(*this, isDead);
  }
}

DeathStateContainerMessage MpActor::GetDeathStateMsg(
  const LocationalData& position, bool isDead, bool shouldTeleport)
{
  DeathStateContainerMessage res;
  res.tIsDead =
    PreparePropertyMessage_(this, "isDead", isDead ? "true" : "false");

  if (shouldTeleport) {
    res.tTeleport = TeleportMessage();
    res.tTeleport->idx = GetIdx();
    std::copy(&position.pos[0], &position.pos[0] + 3,
              std::begin(res.tTeleport->pos));
    std::copy(&position.rot[0], &position.rot[0] + 3,
              std::begin(res.tTeleport->rot));
    res.tTeleport->worldOrCell =
      position.cellOrWorldDesc.ToFormId(GetParent()->espmFiles);
  }

  if (!isDead) {
    constexpr float kAttributePercentageFull = 1.f;
    res.tChangeValues = ChangeValuesMessage();
    res.tChangeValues->idx = GetIdx();
    res.tChangeValues->data.health = kAttributePercentageFull;
    res.tChangeValues->data.magicka = kAttributePercentageFull;
    res.tChangeValues->data.stamina = kAttributePercentageFull;
  }

  return res;
}

void MpActor::EatItem(uint32_t baseId, espm::Type t)
{
  bool isIngredient = t == "INGR";
  bool isAlchemyItem = t == "ALCH";

  // thuum docs/verbs/learned-effects.md: a report of what the eat taught
  // must follow it
  if (isIngredient) {
    pImpl->lastEatenId = baseId;
    pImpl->lastEatenTimePoint = std::chrono::steady_clock::now();
  }

  EatItemEvent eatItemEvent(this, baseId, isIngredient, isAlchemyItem);
  eatItemEvent.Fire(GetParent());
}

bool MpActor::ReadBook(const uint32_t baseId)
{
  ReadBookEvent readBookEvent(this, baseId);
  readBookEvent.Fire(GetParent());
  return readBookEvent.SpellLearned();
}

std::optional<uint16_t> MpActor::GetFateLevel() const
{
  return ChangeForm().fateLevel;
}

uint32_t MpActor::GetFateBaseId() const
{
  auto worldState = GetParent();
  if (!worldState || !worldState->HasEspm()) {
    return GetBaseId();
  }
  const auto& chain = ChangeForm().templateChain;
  std::vector<uint32_t> ids(chain.size());
  std::transform(chain.begin(), chain.end(), ids.begin(),
                 [&](const FormDesc& desc) {
                   return desc.ToFormId(worldState->espmFiles);
                 });
  const uint32_t pick = LeveledListUtils::FirstLeveledPick(
    worldState->GetEspm().GetBrowser(), ids);
  return pick ? pick : GetBaseId();
}

bool MpActor::RollFateIfUnrolled(uint16_t level)
{
  auto worldState = GetParent();
  if (!worldState || !worldState->HasEspm() || pImpl->fromSave || IsDead() ||
      ChangeForm().fateLevel.has_value()) {
    return false;
  }
  // thuum ADR-050 decision 3 (train 51): a server-made NPC, a placed base
  // under a 0xff id, is fated through the same roll as a game-file actor
  // (PapyrusObjectReference::PlaceAtMe rolls it before its create word);
  // only a player's actor (the player base) and a baseless one are refused
  const auto baseId = GetBaseId();
  if (baseId <= 0x7) {
    return false;
  }
  worldState->fateLoaderLevel = level;
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.templateChain.clear();
    changeForm.inv = Inventory();
    changeForm.baseContainerAdded = false;
  });
  EnsureTemplateChainEvaluated(worldState->GetEspm());
  EnsureBaseContainerAdded(worldState->GetEspm());
  EquipBestWeapon();
  worldState->fateLoaderLevel.reset();
  return true;
}

void MpActor::EnsureTemplateChainEvaluated(espm::Loader& loader,
                                           ChangeFormGuard::Mode mode)
{
  auto worldState = GetParent();
  if (!worldState) {
    return;
  }

  // thuum docs/verbs/npc-fate.md (ADR-049 decision 1): the level of the
  // player whose arrival loads the actor, set around the fate pass's chunk
  // loads; none, the old rule's roll (0: every entry allowed)
  const std::optional<uint16_t> fateLevel = worldState->fateLoaderLevel;
  const uint32_t kPcLevel = fateLevel.value_or(0);

  auto baseId = GetBaseId();
  if (baseId == 0x7 || baseId == 0) {
    return;
  }

  const auto& templateChain = ChangeForm().templateChain;

  if (!templateChain.empty()) {

    std::string errorTraits;
    std::string errorStats;
    std::string errorFactions;
    std::string errorSpelllist;
    std::string errorAIData;
    std::string errorAIPackages;
    std::string errorUnused;
    std::string errorBaseData;
    std::string errorInventory;
    std::string errorScript;
    std::string errorDefPackList;
    std::string errorAttackData;
    std::string errorKeywords;

    auto doNothing = [](const auto&, const auto&) { return 1; };

    EvaluateTemplateNoThrow<espm::NPC_::UseTraits>(
      worldState, baseId, templateChain, doNothing, &errorTraits);

    EvaluateTemplateNoThrow<espm::NPC_::UseStats>(
      worldState, baseId, templateChain, doNothing, &errorStats);

    EvaluateTemplateNoThrow<espm::NPC_::UseFactions>(
      worldState, baseId, templateChain, doNothing, &errorFactions);

    EvaluateTemplateNoThrow<espm::NPC_::UseSpelllist>(
      worldState, baseId, templateChain, doNothing, &errorSpelllist);

    EvaluateTemplateNoThrow<espm::NPC_::UseAIData>(
      worldState, baseId, templateChain, doNothing, &errorAIData);

    EvaluateTemplateNoThrow<espm::NPC_::UseAIPackages>(
      worldState, baseId, templateChain, doNothing, &errorAIPackages);

    EvaluateTemplateNoThrow<espm::NPC_::Unused>(
      worldState, baseId, templateChain, doNothing, &errorUnused);

    EvaluateTemplateNoThrow<espm::NPC_::UseBaseData>(
      worldState, baseId, templateChain, doNothing, &errorBaseData);

    EvaluateTemplateNoThrow<espm::NPC_::UseInventory>(
      worldState, baseId, templateChain, doNothing, &errorInventory);

    EvaluateTemplateNoThrow<espm::NPC_::UseScript>(
      worldState, baseId, templateChain, doNothing, &errorScript);

    EvaluateTemplateNoThrow<espm::NPC_::UseDefPackList>(
      worldState, baseId, templateChain, doNothing, &errorDefPackList);

    EvaluateTemplateNoThrow<espm::NPC_::UseAttackData>(
      worldState, baseId, templateChain, doNothing, &errorAttackData);

    EvaluateTemplateNoThrow<espm::NPC_::UseKeywords>(
      worldState, baseId, templateChain, doNothing, &errorKeywords);

    if (errorTraits.empty() && errorStats.empty() && errorFactions.empty() &&
        errorSpelllist.empty() && errorAIData.empty() &&
        errorAIPackages.empty() && errorUnused.empty() &&
        errorBaseData.empty() && errorInventory.empty() &&
        errorScript.empty() && errorDefPackList.empty() &&
        errorAttackData.empty() && errorKeywords.empty()) {
      return;
    }

    spdlog::info(
      "MpActor::EnsureTemplateChainEvaluate {:x} - One of EvaluateTemplate "
      "errored, forgetting previous template chain. Likely, an update on "
      "esp/esm side.",
      GetFormId());
  }

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) {
      auto headNpc = loader.GetBrowser().LookupById(baseId);
      std::vector<uint32_t> res = LeveledListUtils::EvaluateTemplateChain(
        loader.GetBrowser(), headNpc, kPcLevel);
      std::vector<FormDesc> templateChain(res.size());
      std::transform(
        res.begin(), res.end(), templateChain.begin(), [&](uint32_t formId) {
          return FormDesc::FromFormId(formId, worldState->espmFiles);
        });
      changeForm.templateChain = std::move(templateChain);
      changeForm.fateLevel = fateLevel;
    },
    mode);
}

void MpActor::AddDeathItem()
{
  auto map = EvaluateDeathItem();
  for (auto& p : map) {
    AddItem(p.first, p.second);
  }
}

void MpActor::LoadFactions()
{
  std::vector<Faction> factions = EvaluateTemplate<espm::NPC_::UseFactions>(
    GetParent(), GetBaseId(), GetTemplateChain(),
    [&](const auto& npcLookupResult, const auto& npcData) {
      std::vector<Faction> factions = std::vector<Faction>();
      for (auto npcFaction : npcData.factions) {
        Faction faction = Faction();
        faction.formDesc =
          FormDesc::FromFormId(npcLookupResult.ToGlobalId(npcFaction.formId),
                               GetParent()->espmFiles);
        faction.rank = npcFaction.rank;
        factions.push_back(faction);
      }
      return factions;
    });
  for (Faction faction : factions) {
    AddToFaction(faction, false);
  }
  factionsLoaded = true;
}

std::map<uint32_t, uint32_t> MpActor::EvaluateDeathItem()
{
  auto worldState = GetParent();
  if (!worldState) {
    return {};
  }

  constexpr int kPlayerCharacterLevel = 1;

  auto& loader = worldState->GetEspm();

  auto base = loader.GetBrowser().LookupById(GetBaseId());
  if (!base.rec) {
    spdlog::error("EvaluateDeathItem {:x} - No base form", GetFormId());
    return {};
  }

  auto npc = espm::Convert<espm::NPC_>(base.rec);
  if (!npc) {
    spdlog::error(
      "EvaluateDeathItem {:x} - Expected base type to be NPC_, but got {}",
      GetFormId(), base.rec->GetType().ToString());
    return {};
  }

  uint32_t baseId = base.ToGlobalId(base.rec->GetId());
  auto& templateChain = ChangeForm().templateChain;

  uint32_t deathItemId = EvaluateTemplate<espm::NPC_::UseTraits>(
    worldState, baseId, templateChain,
    [](const auto& npcLookupResult, const auto& npcData) {
      return npcLookupResult.ToGlobalId(npcData.deathItem);
    });

  if (deathItemId == 0) {
    spdlog::info("EvaluateDeathItem {:x} - No death item found, skipping add",
                 GetFormId());
    return {};
  }

  espm::LookupResult deathItemLookupRes =
    loader.GetBrowser().LookupById(deathItemId);
  if (!deathItemLookupRes.rec) {
    spdlog::error("EvaluateDeathItem {:x} - Death item {:x} not found in espm",
                  GetFormId(), deathItemId);
    return {};
  }

  auto deathItemLvli = espm::Convert<espm::LVLI>(deathItemLookupRes.rec);
  if (!deathItemLvli) {
    spdlog::error("EvaluateDeathItem {:x} - Expected death item type to be "
                  "LVLI, but got {}",
                  GetFormId(), deathItemLookupRes.rec->GetType().ToString());
    return {};
  }

  const auto kCountMult = 1;
  auto map = LeveledListUtils::EvaluateListRecurse(
    loader.GetBrowser(), deathItemLookupRes, kCountMult,
    kPlayerCharacterLevel);
  return map;
}

void MpActor::ModifyActorValuePercentage(espm::ActorValue av,
                                         float percentageDelta)
{
  ActorValues currentActorValues = GetChangeForm().actorValues;
  switch (av) {
    case espm::ActorValue::Health:
      currentActorValues.healthPercentage =
        CropValue(currentActorValues.healthPercentage + percentageDelta);
      break;
    case espm::ActorValue::Stamina:
      currentActorValues.staminaPercentage =
        CropValue(currentActorValues.staminaPercentage + percentageDelta);
      break;
    case espm::ActorValue::Magicka:
      currentActorValues.magickaPercentage =
        CropValue(currentActorValues.magickaPercentage + percentageDelta);
      break;
    default:
      return;
  }

  static MpActor* const kNullAggressor = nullptr;

  NetSetPercentages(currentActorValues, kNullAggressor,
                    std::vector<espm::ActorValue>{ av });
}

void MpActor::BeforeDestroy()
{
  for (auto& sink : pImpl->destroyEventSinks) {
    sink->BeforeDestroy(*this);
  }

  MpObjectReference::BeforeDestroy();

  UnsubscribeFromAll();
}

void MpActor::Init(WorldState* worldState, uint32_t formId, bool hasChangeForm)
{
  pImpl->fromSave = hasChangeForm;
  MpObjectReference::Init(worldState, formId, hasChangeForm);

  if (worldState->HasEspm()) {
    auto& espm = worldState->GetEspm();
    EnsureTemplateChainEvaluated(espm);
    EnsureBaseContainerAdded(espm); // template chain needed here

    // TODO: implement "gearedUpWeapons" flag
    EquipBestWeapon();
  }
}

void MpActor::Kill(MpActor* killer, bool shouldTeleport)
{
  spdlog::trace("MpActor::Kill {:x} - killer is {:x}", GetFormId(),
                killer ? killer->GetFormId() : 0);
  // ADR-050 decision 2: one death, one word (the host's report and the
  // pending death's timeout may both ask)
  if (IsDead()) {
    spdlog::trace("MpActor::Kill {:x} - already dead, nothing", GetFormId());
    return;
  }
  if (pImpl->pendingDeath) {
    pImpl->pendingDeath = false;
    if (auto worldState = GetParent()) {
      worldState->pendingDeaths.erase(GetFormId());
    }
  }
  pImpl->lastKillerId = killer ? killer->GetFormId() : 0u;

  auto& changeForm = ChangeForm();
  const float healthPercentageBeforeDeath =
    changeForm.actorValues.healthPercentage;
  const float magickaPercentageBeforeDeath =
    changeForm.actorValues.magickaPercentage;
  const float staminaPercentageBeforeDeath =
    changeForm.actorValues.staminaPercentage;

  // Keep in sync with MpActor::SetIsDead
  SendAndSetDeathState(true, shouldTeleport);

  DeathEvent deathEvent(this, killer, healthPercentageBeforeDeath,
                        magickaPercentageBeforeDeath,
                        staminaPercentageBeforeDeath);
  deathEvent.Fire(GetParent());

  AddDeathItem();
}

void MpActor::RespawnWithDelay(bool shouldTeleport)
{
  spdlog::trace("MpActor::RespawnWithDelay {:x} - isRespawning: {}",
                GetFormId(), pImpl->isRespawning);

  if (pImpl->isRespawning) {
    return;
  }
  // thuum docs/research/m3/decisions-after-playtest-34.md D21: a disabled
  // actor is out of the world by the server's own word (the gamemode's or
  // the lab's Disable), so its death schedules no return; playtest 34's ice
  // wolf ff000003, disabled at 23:00Z, was killed at 23:01:00 and stood
  // again enabled at 23:01:25. Its enable starts the clock (Enable below). A
  // player's actor is never taken out this way
  if (IsDisabled() && !IsCreatedAsPlayer()) {
    spdlog::info("[verb:npc-fate] {:x} died disabled: no respawn scheduled "
                 "(D21); its enable starts the clock",
                 GetFormId());
    return;
  }
  // thuum docs/verbs/npc-fate.md (the deaths): a game-file actor's body keeps
  // to the engine's reset clock in game hours, never a wall delay; the
  // deadline lives on the record and PartOne's sweep ends it
  if (IsFateRespawned()) {
    ScheduleFateRespawn();
    return;
  }
  pImpl->isRespawning = true;

  ++pImpl->respawnTimerIndex;
  auto respawnTimerIndex = pImpl->respawnTimerIndex;

  uint32_t formId = GetFormId();
  if (auto worldState = GetParent()) {
    float respawnTime = GetRespawnTime();
    auto time = Viet::TimeUtils::To<std::chrono::milliseconds>(respawnTime);
    worldState->SetTimer(time).Then([worldState, this, formId, shouldTeleport,
                                     respawnTimerIndex,
                                     respawnTime](Viet::Void) {
      if (worldState->LookupFormById(formId).get() == this) {
        bool isLatestRespawn = respawnTimerIndex == pImpl->respawnTimerIndex;
        if (isLatestRespawn) {
          // D21: taken out of the world while the timer ran; its enable
          // schedules the return anew
          if (IsDisabled() && !IsCreatedAsPlayer()) {
            spdlog::info("[verb:npc-fate] {:x} disabled before its respawn: "
                         "the respawn is dropped (D21)",
                         GetFormId());
            pImpl->isRespawning = false;
            return;
          }
          spdlog::info("MpActor::RespawnWithDelay {:x} - finally, respawn "
                       "after {} seconds",
                       GetFormId(), respawnTime);
          RebuildAndRespawn(shouldTeleport, false);
        }
      }
    });
  }
}

void MpActor::Respawn(bool shouldTeleport)
{
  if (IsDead() == false) {
    return;
  }
  pImpl->isRespawning = false;

  // thuum docs/verbs/npc-fate.md (the deaths): every respawn passes here,
  // the sweep's, the console's Resurrect, the lab's respawn-ref; the body's
  // stamp and its place in the due set end with it, so the next death
  // counts from its own day
  if (ChangeForm().fateDiedAt.has_value()) {
    EditChangeForm(
      [&](MpChangeForm& changeForm) { changeForm.fateDiedAt.reset(); },
      Mode::NoRequestSave);
  }
  if (auto worldState = GetParent()) {
    worldState->fateRespawnDue.erase(GetFormId());
  }

  RespawnEvent respawnEvent(this, shouldTeleport);
  respawnEvent.Fire(GetParent());
}

void MpActor::RebuildAndRespawn(bool shouldTeleport, bool rebuildAlways)
{
  auto worldState = GetParent();
  if (!worldState) {
    return;
  }
  // This is implemented here, not in MpActor::Respawn because we don't
  // want inventory to be re-added in case of artificial resurrect from
  // the gamemode

  // This will do nothing if death item resolved to no items
  auto deathItem = EvaluateDeathItem();
  if (deathItem.empty() && !rebuildAlways) {
    spdlog::info("MpActor::RespawnWithDelay {:x} - death item "
                 "resolved to no items, keeping inventory",
                 GetFormId());
  } else {
    spdlog::info("MpActor::RespawnWithDelay {:x} - death item "
                 "resolved to {} items, re-adding base inventory",
                 GetFormId(), deathItem.size());

    if (!worldState->HasEspm()) {
      spdlog::error("MpActor::RespawnWithDelay {:x} - no espm attached",
                    GetFormId());
    } else {
      Inventory inventory = GetInventory();
      Inventory inventoryToKeep;
      for (auto& entry : inventory.entries) {
        if (worldState->HasKeyword(entry.baseId, "SweetCantDrop")) {
          inventoryToKeep.entries.push_back(entry);
        }
      }
      // thuum docs/verbs/npc-fate.md (ADR-049 decision 2): the
      // respawn re-rolls the chain and the outfit at the record's
      // level, as the engine re-rolls at a cell reset, and the fate
      // is sent again
      const auto fateLevel = ChangeForm().fateLevel;
      EditChangeForm(
        [&](MpChangeForm& changeForm) {
          changeForm.inv = inventoryToKeep;
          changeForm.baseContainerAdded = false;
          if (fateLevel.has_value()) {
            changeForm.templateChain.clear();
          }
        },
        Mode::NoRequestSave);
      if (fateLevel.has_value()) {
        worldState->fateLoaderLevel = fateLevel;
        EnsureTemplateChainEvaluated(worldState->GetEspm(),
                                     Mode::NoRequestSave);
      }
      EnsureBaseContainerAdded(worldState->GetEspm());
      worldState->fateLoaderLevel.reset();
      if (fateLevel.has_value() && worldState->onActorFate) {
        worldState->onActorFate(*this);
      }
      spdlog::info("MpActor::RespawnWithDelay {:x} - {} inventory "
                   "entries with keyword kept",
                   GetFormId(), inventoryToKeep.entries.size());
    }
  }

  Respawn(shouldTeleport);
}

bool MpActor::IsFateRespawned() const
{
  // every actor that is not a player's: a game-file actor (its id under
  // 0xff000000) and a server-made actor with an NPC base alike. Playtest 34
  // (2026-10-25): two server-made bandits in Bleak Falls Temple (base
  // EncBandit01Melee2HBerserkNordM, profileId -1, the lab's placement)
  // died to headshots and stood again 25 s later by the players' rule,
  // while the game-file bandit beside them lay for the reset hours
  return !IsCreatedAsPlayer();
}

void MpActor::ScheduleFateRespawn()
{
  auto worldState = GetParent();
  if (!worldState) {
    return;
  }
  pImpl->isRespawning = true;
  if (!ChangeForm().fateDiedAt.has_value()) {
    const float day =
      worldState->gameTime ? worldState->gameTime().daysPassed : 0.f;
    EditChangeForm(
      [&](MpChangeForm& changeForm) { changeForm.fateDiedAt = day; });
  }
  const float diedAt = *ChangeForm().fateDiedAt;
  bool unique = false;
  if (worldState->HasEspm() && GetBaseType() == "NPC_") {
    unique = espm::GetData<espm::NPC_>(GetBaseId(), worldState).isUnique;
  }
  if (unique) {
    spdlog::info("[verb:npc-fate] {:x} died at day {:.4f}: a unique base, "
                 "the body stays (the engine's rule)",
                 GetFormId(), diedAt);
    return;
  }
  const float hours = worldState->FateRespawnHours();
  worldState->fateRespawnDue.insert(GetFormId());
  spdlog::info("[verb:npc-fate] {:x} died at day {:.4f}; the body lies until "
               "day {:.4f} ({} h)",
               GetFormId(), diedAt, diedAt + hours / 24.f, hours);
}

void MpActor::RespawnFated()
{
  if (!IsDead()) {
    return;
  }
  spdlog::info("[verb:npc-fate] {:x} stands again at its placement, rolled "
               "anew",
               GetFormId());
  RebuildAndRespawn(true, true);
}

void MpActor::RecordDeathAt(std::chrono::steady_clock::time_point at)
{
  pImpl->diedAt = at;
}

std::optional<std::chrono::milliseconds> MpActor::DiedAgo(
  std::chrono::steady_clock::time_point now) const
{
  if (pImpl->diedAt == std::chrono::steady_clock::time_point()) {
    return std::nullopt;
  }
  return std::chrono::duration_cast<std::chrono::milliseconds>(now -
                                                               pImpl->diedAt);
}

void MpActor::NoteHitBy(uint32_t aggressorId,
                        std::chrono::steady_clock::time_point now)
{
  constexpr size_t kHitsKept = 8;
  auto& hits = pImpl->hitsTaken;
  hits.erase(
    std::remove_if(hits.begin(), hits.end(),
                   [&](const auto& hit) { return hit.first == aggressorId; }),
    hits.end());
  hits.emplace_back(aggressorId, now);
  if (hits.size() > kHitsKept) {
    hits.erase(hits.begin());
  }
}

std::optional<std::chrono::milliseconds> MpActor::HitByAgo(
  uint32_t aggressorId, std::chrono::steady_clock::time_point now) const
{
  std::optional<std::chrono::milliseconds> ago;
  for (const auto& [by, at] : pImpl->hitsTaken) {
    if (aggressorId != 0 && by != aggressorId) {
      continue;
    }
    const auto since =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - at);
    if (!ago || since < *ago) {
      ago = since;
    }
  }
  return ago;
}

std::optional<uint32_t> MpActor::LastHitBy(
  std::chrono::steady_clock::time_point now,
  std::chrono::milliseconds window) const
{
  std::optional<uint32_t> by;
  std::optional<std::chrono::milliseconds> ago;
  for (const auto& [aggressor, at] : pImpl->hitsTaken) {
    const auto since =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - at);
    if (since > window) {
      continue;
    }
    if (!ago || since < *ago) {
      ago = since;
      by = aggressor;
    }
  }
  return by;
}

std::optional<uint32_t> MpActor::LastKillerId() const
{
  return pImpl->lastKillerId;
}

void MpActor::SetLastAction(const UpdateActionMessage& action)
{
  pImpl->lastAction = action;
}

std::optional<UpdateActionMessage> MpActor::GetLastAction() const
{
  return pImpl->lastAction;
}

void MpActor::RecordHeight(float z, std::chrono::steady_clock::time_point now)
{
  auto& heights = pImpl->heights;
  heights.emplace_back(now, z);
  while (!heights.empty() && now - heights.front().first > kHeightWindow) {
    heights.pop_front();
  }
}

std::optional<float> MpActor::DropTo(
  float z, std::chrono::steady_clock::time_point now) const
{
  std::optional<float> top;
  for (const auto& [at, height] : pImpl->heights) {
    if (now - at > kHeightWindow) {
      continue;
    }
    if (!top || height > *top) {
      top = height;
    }
  }
  if (!top) {
    return std::nullopt;
  }
  return *top - z;
}

bool MpActor::MayDie(MpActor* aggressor)
{
  if (!IsFateRespawned()) {
    return true;
  }
  auto worldState = GetParent();
  if (!worldState || !worldState->HasEspm() || GetBaseType() != "NPC_") {
    return true;
  }
  const auto npc = espm::GetData<espm::NPC_>(GetBaseId(), worldState);
  if (npc.isEssential) {
    return false;
  }
  if (npc.isProtected) {
    return aggressor && aggressor->IsCreatedAsPlayer();
  }
  return true;
}

void MpActor::Teleport(const LocationalData& position)
{
  TeleportMessage msg;
  msg.idx = GetIdx();
  std::copy(&position.pos[0], &position.pos[0] + 3, std::begin(msg.pos));
  std::copy(&position.rot[0], &position.rot[0] + 3, std::begin(msg.rot));
  msg.worldOrCell = position.cellOrWorldDesc.ToFormId(GetParent()->espmFiles);
  GetActorToSendTo().SendToUser(msg, true);

  SetCellOrWorldObsolete(position.cellOrWorldDesc);
  SetPos(position.pos);
  SetAngle(position.rot);

  // the movement reports its game sent before this arrive after it; the
  // next one tells the movement rule to wait for the arrival (thuum
  // docs/verbs/movement-speed.md, ActionListener::OnUpdateMovement)
  SetTeleportFlag(true);
}

void MpActor::SetSpawnPoint(const LocationalData& position)
{
  EditChangeForm(
    [&](MpChangeForm& changeForm) { changeForm.spawnPoint = position; });
}

LocationalData MpActor::GetSpawnPoint() const
{
  auto formId = GetFormId();

  if (!IsCreatedAsPlayer()) {
    if (formId < 0xff000000) {
      return GetEditorLocationalData();
    }
  }
  return ChangeForm().spawnPoint;
}

LocationalData MpActor::GetEditorLocationalData() const
{
  auto formId = GetFormId();
  auto worldState = GetParent();

  if (!worldState || !worldState->HasEspm()) {
    throw std::runtime_error("MpActor::GetEditorLocation can only be used "
                             "with actors attached to a valid world state");
  }

  auto data = espm::GetData<espm::ACHR>(formId, worldState);
  auto lookupRes = worldState->GetEspm().GetBrowser().LookupById(formId);

  // TODO: Angles are probably messed up here (radians/degrees)

  NiPoint3 pos;
  NiPoint3 rot;
  uint32_t worldOrCell = 0x0000003c;

  if (!lookupRes.rec) {
    spdlog::error("MpActor::GetEditorLocationalData {:x} - lookupRes.rec was "
                  "nullptr, using current location as spawn point",
                  formId);

    pos = this->GetPos();
    rot = this->GetAngle();
    worldOrCell = this->GetCellOrWorld().ToFormId(worldState->espmFiles);
  } else if (!data.loc) {
    spdlog::error("MpActor::GetEditorLocationalData {:x} - data.loc was "
                  "nullptr, using current location as spawn point",
                  formId);

    pos = this->GetPos();
    rot = this->GetAngle();
    worldOrCell = this->GetCellOrWorld().ToFormId(worldState->espmFiles);
  } else {
    pos = LocationalDataUtils::GetPos(data.loc);
    rot = LocationalDataUtils::GetRot(data.loc);
    worldOrCell = LocationalDataUtils::GetWorldOrCell(
      worldState->GetEspm().GetBrowser(), lookupRes);
  }

  return LocationalData{
    pos, rot, FormDesc::FromFormId(worldOrCell, worldState->espmFiles)
  };
}

const float MpActor::GetRespawnTime() const
{
  return ChangeForm().spawnDelay;
}

void MpActor::SetRespawnTime(float time)
{
  EditChangeForm(
    [&](MpChangeForm& changeForm) { changeForm.spawnDelay = time; });
}

void MpActor::SetIsDead(bool isDead)
{
  spdlog::trace("MpActor::SetIsDead {:x} - isDead: {}", GetFormId(), isDead);

  constexpr bool kShouldTeleport = false;

  if (isDead) {
    if (IsDead() == false) {
      auto& changeForm = ChangeForm();
      const float healthPercentageBeforeDeath =
        changeForm.actorValues.healthPercentage;
      const float magickaPercentageBeforeDeath =
        changeForm.actorValues.magickaPercentage;
      const float staminaPercentageBeforeDeath =
        changeForm.actorValues.staminaPercentage;

      // Keep in sync with MpActor::Kill
      SendAndSetDeathState(isDead, kShouldTeleport);

      DeathEvent deathEvent(this, nullptr, healthPercentageBeforeDeath,
                            magickaPercentageBeforeDeath,
                            staminaPercentageBeforeDeath);
      deathEvent.Fire(GetParent());

      AddDeathItem();

      spdlog::trace("MpActor::SetIsDead {:x} - actor is now dead",
                    GetFormId());
    } else {
      spdlog::trace("MpActor::SetIsDead {:x} - actor is already dead",
                    GetFormId());
    }
  } else {
    // same as SendAndSetDeathState but resets isRespawning flag
    Respawn(kShouldTeleport);
    spdlog::trace("MpActor::SetIsDead {:x} - actor is now alive", GetFormId());
  }
}

void MpActor::RestoreActorValue(espm::ActorValue av, float value)
{
  ModifyActorValuePercentage(
    av, std::abs(value) / GetMaximumValues().GetValue(av));
}

void MpActor::DamageActorValue(espm::ActorValue av, float value)
{
  ModifyActorValuePercentage(
    av, -std::abs(value) / GetMaximumValues().GetValue(av));
}

BaseActorValues MpActor::GetBaseValues()
{
  return GetBaseActorValues(GetParent(), GetBaseId(), GetRaceId(),
                            ChangeForm().templateChain);
}

BaseActorValues MpActor::GetMaximumValues()
{
  // thuum docs/verbs/actor-values.md: a player's recorded base health,
  // magicka and stamina (the engine's own, raised by level-ups) are the
  // maximum its percentages count against; the race's and the base NPC's
  // values before the first record. CommonLibSSE-NG include/RE/A/
  // ActorValues.h: kHealth 24, kMagicka 25, kStamina 26.
  BaseActorValues values = GetBaseValues();
  if (auto health = GetRecordedActorValueBase(24); health && *health > 0.f) {
    values.health = *health;
  }
  if (auto magicka = GetRecordedActorValueBase(25);
      magicka && *magicka > 0.f) {
    values.magicka = *magicka;
  }
  if (auto stamina = GetRecordedActorValueBase(26);
      stamina && *stamina > 0.f) {
    values.stamina = *stamina;
  }
  // thuum docs/verbs/magic-effects.md: and the running buffs on each (a
  // Fortify Health's 20 while it runs)
  values.health += GetEffectModifier(espm::ActorValue::Health);
  values.magicka += GetEffectModifier(espm::ActorValue::Magicka);
  values.stamina += GetEffectModifier(espm::ActorValue::Stamina);
  return values;
}

std::optional<BaseActorValues> MpActor::RecordedMaxima()
{
  const auto recorded = [this](uint8_t av) {
    const auto base = GetRecordedActorValueBase(av);
    return base && *base > 0.f;
  };
  if (!recorded(24) && !recorded(25) && !recorded(26)) {
    return std::nullopt;
  }
  return GetMaximumValues();
}

void MpActor::SendMaximaToListeners()
{
  const auto maxima = RecordedMaxima();
  if (!maxima) {
    return;
  }
  // the same three props CreateActor carries to an observer, as property
  // updates (the owner's own values travel by ChangeValues and its record)
  SendMessageToActorListeners(
    CreatePropertyMessage_(this, "health", fmt::format("{}", maxima->health)),
    true);
  SendMessageToActorListeners(
    CreatePropertyMessage_(this, "magicka",
                           fmt::format("{}", maxima->magicka)),
    true);
  SendMessageToActorListeners(
    CreatePropertyMessage_(this, "stamina",
                           fmt::format("{}", maxima->stamina)),
    true);
}

void MpActor::DropItem(const uint32_t baseId, const Inventory::Entry& entry)
{
  constexpr float kDeletionTimeSeconds = 2 * 60;
  constexpr size_t kDroppedItemsQueueMax = 10;

  static std::atomic<bool> g_dropItemDisabledGlobally = false;
  static std::atomic<int> g_numDrops = 0;

  if (g_dropItemDisabledGlobally) {
    return;
  }

  if (g_numDrops > 1'000'000) {
    spdlog::warn(
      "MpActor::DropItem - Too many item drops, server restart needed");
    return;
  }

  constexpr uint32_t kGold001 = 0x0000000f;
  if (baseId == kGold001) {
    spdlog::warn("MpActor::DropItem {:x} - Attempt to drop Gold001 by actor",
                 GetFormId());
    return;
  }

  const int count = entry.count;

  DropItemEvent dropEvent(GetFormId(), baseId, count);
  bool success = dropEvent.Fire(GetParent());

  if (!success) {
    spdlog::info("MpActor::DropItem {:x} - blocked by MpApiDropItem "
                 "(baseId={:x}, count={})",
                 GetFormId(), baseId, count);
    return;
  }

  auto worldState = GetParent();

  espm::LookupResult lookupRes =
    worldState->GetEspm().GetBrowser().LookupById(baseId);
  lookupRes.rec->GetId();

  std::string editorId =
    lookupRes.rec->GetEditorId(worldState->GetEspmCache());

  spdlog::trace("MpActor::DropItem - dropping {}", editorId);
  RemoveItems({ entry });

  PapyrusObjectReference papyrusObjectReference;
  auto baseForm = VarValue(std::make_shared<EspmGameObject>(lookupRes));
  auto aCount = VarValue(count);
  auto aForcePersist = VarValue(false);
  auto aInitiallyDisabled = VarValue(false);

  VarValue placedObjectWrap = papyrusObjectReference.PlaceAtMe(
    this->ToVarValue(),
    { baseForm, aCount, aForcePersist, aInitiallyDisabled });
  MpObjectReference* placedObject =
    GetFormPtr<MpObjectReference>(placedObjectWrap);

  if (!placedObject) {
    spdlog::error("MpActor::DropItem - placedObject was null, disabling "
                  "DropItem for all players");
    g_dropItemDisabledGlobally = true;
    return;
  }

  placedObject->SetCount(count);

  // thuum docs/verbs/prop-physics.md (the drop's stream at T3, 2026-10-30): a
  // copy placed at the actor's own position wakes Dynamic inside the dropper's
  // capsule on its seat and is thrown out of reach; a step ahead along the
  // actor's heading it lands at the feet, as the engine's own drop does
  {
    constexpr float kDropAheadUnits = 64.f;
    constexpr float kDegToRad = 3.14159265f / 180.f;
    const float yaw = GetAngle().z * kDegToRad;
    const NiPoint3 pos = GetPos();
    placedObject->SetPos({ pos.x + std::sin(yaw) * kDropAheadUnits,
                           pos.y + std::cos(yaw) * kDropAheadUnits, pos.z });
  }

  uint32_t droppedItemFormId = placedObject->GetFormId();

  // Filter our dropped items queue
  pImpl->droppedItemsQueue.erase(
    std::remove_if(
      pImpl->droppedItemsQueue.begin(), pImpl->droppedItemsQueue.end(),
      [worldState](const std::pair<uint32_t, MpObjectReference*>& pair) {
        auto [referenceFormId, reference] = pair;
        bool referenceAlive =
          reference == worldState->LookupFormById(referenceFormId).get();
        if (!referenceAlive) {
          return true;
        }
        if (reference->IsDeleted() || reference->IsHarvested()) {
          return true;
        }
        return false;
      }),
    pImpl->droppedItemsQueue.end());

  while (!pImpl->droppedItemsQueue.empty() &&
         pImpl->droppedItemsQueue.size() >= kDroppedItemsQueueMax) {
    auto [referenceFormId, reference] = pImpl->droppedItemsQueue.front();
    bool referenceAlive =
      reference == worldState->LookupFormById(referenceFormId).get();
    if (referenceAlive) {
      if (!reference->IsDeleted()) {
        spdlog::trace("MpActor::DropItem - deleting previously dropped {}",
                      editorId);
        reference->Delete();
      } else {
        spdlog::warn("MpActor::DropItem - reference in queue was deleted");
      }
    } else {
      spdlog::warn("MpActor::DropItem - reference in queue was invalidated");
    }
    pImpl->droppedItemsQueue.erase(pImpl->droppedItemsQueue.begin());
  }

  pImpl->droppedItemsQueue.push_back(
    std::make_pair(droppedItemFormId, placedObject));

  auto time =
    Viet::TimeUtils::To<std::chrono::milliseconds>(kDeletionTimeSeconds);

  uint32_t formId = GetFormId();

  // TODO: make timer group for better performance
  worldState->SetTimer(time).Then([placedObject, worldState, droppedItemFormId,
                                   editorId, this, formId](Viet::Void) {
    bool actorStillAlive = this == worldState->LookupFormById(formId).get();
    if (!actorStillAlive) {
      return;
    }
    bool formStillAlive =
      placedObject == worldState->LookupFormById(droppedItemFormId).get();
    if (!formStillAlive) {
      return;
    }

    if (placedObject->IsDeleted()) {
      return;
    }

    spdlog::trace("MpActor::DropItem - deleting previously dropped {}",
                  editorId);

    // Item deleted, not in queue anymore
    auto it = std::remove(this->pImpl->droppedItemsQueue.begin(),
                          this->pImpl->droppedItemsQueue.end(),
                          std::make_pair(droppedItemFormId, placedObject));
    this->pImpl->droppedItemsQueue.erase(it,
                                         this->pImpl->droppedItemsQueue.end());

    placedObject->Delete();
    ++g_numDrops;
  });
}

void MpActor::SetIsBlockActive(bool active)
{
  pImpl->isBlockActive = active;
}

bool MpActor::IsBlockActive() const
{
  return pImpl->isBlockActive;
}

const float kAngleToRadians = std::acos(-1.f) / 180.f;

NiPoint3 MpActor::GetViewDirection() const
{
  return { std::sin(GetAngle().z * kAngleToRadians),
           std::cos(GetAngle().z * kAngleToRadians), 0 };
}

void MpActor::SetActorValue(espm::ActorValue actorValue, float value)
{
  ActorValues currentActorValues = GetChangeForm().actorValues;
  switch (actorValue) {
    case espm::ActorValue::HealRate:
      currentActorValues.healRate = value;
      break;
    case espm::ActorValue::MagickaRate:
      currentActorValues.magickaRate = value;
      break;
    case espm::ActorValue::StaminaRate:
      currentActorValues.staminaRate = value;
      break;
    case espm::ActorValue::Health:
      currentActorValues.health = value;
      break;
    case espm::ActorValue::Magicka:
      currentActorValues.magicka = value;
      break;
    case espm::ActorValue::Stamina:
      currentActorValues.stamina = value;
      break;
    case espm::ActorValue::HealRateMult_or_CombatHealthRegenMultMod:
      currentActorValues.healRateMult = value;
      break;
    case espm::ActorValue::StaminaRateMult:
      currentActorValues.staminaRateMult = value;
      break;
    case espm::ActorValue::MagickaRateMult_or_CombatHealthRegenMultPowerMod:
      currentActorValues.magickaRateMult = value;
      break;
    default:
      break;
  }

  std::vector<espm::ActorValue> avFilter = { actorValue };
  NetSendChangeValues(currentActorValues, avFilter);
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.actorValues = currentActorValues;
  });
}

namespace {
// thuum docs/verbs/magic-effects.md: an item's or a spell's effect as the
// effect rule takes it, from its MGEF record (libespm MGEF: the archetype,
// the actor values, the second's weight, the flags) and the item's own
// magnitude and duration; none for an archetype the rule leaves alone
std::optional<skymp::rules::EffectFacts> EffectFactsOf(
  WorldState* worldState, uint32_t source, const espm::Effects::Effect& effect)
{
  espm::MGEF::DATA mgef;
  try {
    mgef = espm::GetData<espm::MGEF>(effect.effectId, worldState).data;
  } catch (std::exception& e) {
    spdlog::warn("EffectFactsOf - effect {:x} of {:x}: {}", effect.effectId,
                 source, e.what());
    return std::nullopt;
  }
  skymp::rules::EffectFacts facts{};
  switch (mgef.effectType) {
    case espm::MGEF::EffectType::ValueMod:
      facts.kind = skymp::rules::EffectKind::Value;
      break;
    case espm::MGEF::EffectType::PeakValueMod:
      facts.kind = skymp::rules::EffectKind::PeakValue;
      break;
    case espm::MGEF::EffectType::Dual:
      facts.kind = skymp::rules::EffectKind::DualValue;
      break;
    // thuum docs/verbs/self-cast.md: states that change no value and run
    // for their duration, which the server's rules read and every game shows
    case espm::MGEF::EffectType::Invisibility:
    case espm::MGEF::EffectType::Light:
    case espm::MGEF::EffectType::Etherealize:
      facts.kind = skymp::rules::EffectKind::State;
      break;
    default:
      return std::nullopt;
  }
  if (mgef.primaryAV == espm::ActorValue::None &&
      facts.kind != skymp::rules::EffectKind::State) {
    return std::nullopt;
  }
  constexpr uint32_t kNoSecond = std::numeric_limits<uint32_t>::max();
  facts.effect = effect.effectId;
  facts.source = source;
  facts.av = static_cast<uint32_t>(mgef.primaryAV);
  facts.second_av = facts.kind == skymp::rules::EffectKind::DualValue &&
      mgef.secondaryAV != espm::ActorValue::None
    ? static_cast<uint32_t>(mgef.secondaryAV)
    : kNoSecond;
  facts.second_weight = mgef.secondAVWeight;
  facts.magnitude = effect.magnitude;
  facts.duration_s = static_cast<float>(effect.duration);
  facts.recover = mgef.IsFlagSet(espm::MGEF::Flags::Recover);
  facts.detrimental = mgef.IsFlagSet(espm::MGEF::Flags::Detrimental);
  facts.no_duration = mgef.IsFlagSet(espm::MGEF::Flags::NoDuration);
  return facts;
}

bool IsNoForm(const FormDesc& desc)
{
  return desc.shortFormId == 0 && desc.file.empty();
}

// The change form's running effects as the rule's entries; one whose
// plugin is no longer loaded is dropped
rust::Vec<skymp::rules::EffectEntry> EffectEntriesOf(
  const MpChangeForm& changeForm, const std::vector<std::string>& files)
{
  rust::Vec<skymp::rules::EffectEntry> entries;
  if (!changeForm.runningEffects) {
    return entries;
  }
  constexpr uint32_t kNoSecond = std::numeric_limits<uint32_t>::max();
  for (const auto& r : *changeForm.runningEffects) {
    try {
      skymp::rules::EffectEntry e{};
      e.facts.effect = r.effect.ToFormId(files);
      e.facts.source = r.source.ToFormId(files);
      e.facts.kind = r.kind == 1 ? skymp::rules::EffectKind::PeakValue
        : r.kind == 2            ? skymp::rules::EffectKind::DualValue
        : r.kind == 3            ? skymp::rules::EffectKind::State
                                 : skymp::rules::EffectKind::Value;
      e.facts.av = static_cast<uint32_t>(r.av);
      e.facts.second_av =
        r.secondAv < 0 ? kNoSecond : static_cast<uint32_t>(r.secondAv);
      e.facts.second_weight = r.secondWeight;
      e.facts.magnitude = r.magnitude;
      e.facts.duration_s = r.durationS;
      e.facts.recover = r.recover;
      e.facts.detrimental = r.detrimental;
      e.facts.no_duration = r.noDuration;
      e.caster = IsNoForm(r.caster) ? 0 : r.caster.ToFormId(files);
      e.elapsed_s = r.elapsedS;
      entries.push_back(e);
    } catch (std::exception& ex) {
      spdlog::warn("EffectEntriesOf - dropping {}: {}", r.effect.ToString(),
                   ex.what());
    }
  }
  return entries;
}

// The server's wall clock in Unix milliseconds, what a running effect's
// asOfMs counts against: the world's clock (thuum ADR-021)
uint64_t WallNowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch())
      .count());
}

// The rule's entries as the change form keeps them, their seconds run
// counted as of `asOfMs`
std::optional<std::vector<RunningEffect>> RunningEffectsOf(
  const rust::Vec<skymp::rules::EffectEntry>& entries,
  const std::vector<std::string>& files, uint64_t asOfMs)
{
  if (entries.empty()) {
    return std::nullopt;
  }
  constexpr uint32_t kNoSecond = std::numeric_limits<uint32_t>::max();
  std::vector<RunningEffect> running;
  running.reserve(entries.size());
  for (const auto& e : entries) {
    RunningEffect r;
    r.effect = FormDesc::FromFormId(e.facts.effect, files);
    r.source = FormDesc::FromFormId(e.facts.source, files);
    r.caster = e.caster ? FormDesc::FromFormId(e.caster, files) : FormDesc();
    r.kind = e.facts.kind == skymp::rules::EffectKind::PeakValue ? 1
      : e.facts.kind == skymp::rules::EffectKind::DualValue      ? 2
      : e.facts.kind == skymp::rules::EffectKind::State          ? 3
                                                                 : 0;
    r.av = static_cast<int32_t>(e.facts.av);
    r.secondAv = e.facts.second_av == kNoSecond
      ? -1
      : static_cast<int32_t>(e.facts.second_av);
    r.secondWeight = e.facts.second_weight;
    r.magnitude = e.facts.magnitude;
    r.durationS = e.facts.duration_s;
    r.elapsedS = e.elapsed_s;
    r.asOfMs = asOfMs;
    r.recover = e.facts.recover;
    r.detrimental = e.facts.detrimental;
    r.noDuration = e.facts.no_duration;
    running.push_back(std::move(r));
  }
  return running;
}

// Each change's points by Health, Magicka and Stamina (CommonLibSSE-NG
// include/RE/A/ActorValues.h: kHealth 24, kMagicka 25, kStamina 26); a
// change of any other actor value counts through GetEffectModifier where
// it matters (the regeneration rates). A god actor (thuum
// docs/verbs/god-mode.md) loses nothing of a current value, while a buff's
// maximum still moves.
template <class Changes>
void SumAttributeChanges(const Changes& changes, std::array<float, 3>& deltas,
                         std::array<bool, 3>& touched, bool god)
{
  for (const auto& c : changes) {
    const int i = c.av == 24 ? 0 : c.av == 25 ? 1 : c.av == 26 ? 2 : -1;
    if (i < 0) {
      continue;
    }
    deltas[i] += (god && c.current < 0.f ? 0.f : c.current) + c.modifier;
    touched[i] = true;
  }
}

// The running effects as MagicEffects, and `shownOnce` (effects applied at
// once, 0 seconds left) beside them
// The sums the running buffs in `entries` put on each actor value they
// touch, by actor value, those at 0 left out (wire-rules magic::modifier)
std::vector<MagicEffectsMessage::Modifier> EffectModifiersOf(
  const rust::Vec<skymp::rules::EffectEntry>& entries)
{
  constexpr uint32_t kNoSecond = std::numeric_limits<uint32_t>::max();
  std::set<uint32_t> avs;
  for (const auto& e : entries) {
    avs.insert(e.facts.av);
    if (e.facts.second_av != kNoSecond) {
      avs.insert(e.facts.second_av);
    }
  }
  std::vector<MagicEffectsMessage::Modifier> res;
  for (uint32_t av : avs) {
    const float amount = skymp::rules::effect_modifier(entries, av);
    if (amount != 0.f) {
      MagicEffectsMessage::Modifier modifier;
      modifier.av = av;
      modifier.amount = amount;
      res.push_back(modifier);
    }
  }
  return res;
}

MagicEffectsMessage MagicEffectsOf(
  uint32_t idx, const rust::Vec<skymp::rules::EffectEntry>& entries,
  const std::vector<MagicEffectsMessage::Effect>& shownOnce)
{
  MagicEffectsMessage msg;
  msg.idx = idx;
  msg.modifiers = EffectModifiersOf(entries);
  for (const auto& e : entries) {
    MagicEffectsMessage::Effect effect;
    effect.effect = e.facts.effect;
    effect.source = e.facts.source;
    effect.magnitude = e.facts.magnitude;
    effect.remaining = std::max(e.facts.duration_s - e.elapsed_s, 0.f);
    msg.effects.push_back(effect);
  }
  for (const auto& effect : shownOnce) {
    msg.effects.push_back(effect);
  }
  return msg;
}
}

// TODO: only used in legacy MGEF implementation, remove when MGEF is rewritten
void MpActor::SetActorValues(const ActorValues& actorValues)
{
  NetSendChangeValues(actorValues, std::nullopt);
  EditChangeForm(
    [&](MpChangeForm& changeForm) { changeForm.actorValues = actorValues; });
}

void MpActor::ApplyMagicEffect(espm::Effects::Effect& effect, bool hasSweetpie,
                               bool durationOverriden)
{
  WorldState* worldState = GetParent();
  auto data = espm::GetData<espm::MGEF>(effect.effectId, worldState).data;

  if (data.effectType == espm::MGEF::EffectType::CureDisease) {
    spdlog::trace("Curing all diseases");
    auto spells = ChangeForm().learnedSpells.GetLearnedSpells();
    for (auto spellId : spells) {
      auto spellData = espm::GetData<espm::SPEL>(spellId, worldState);
      if (spellData.spellItem->type == espm::SPEL::SpellType::Disease) {
        spdlog::trace("Curing disease {:x}", spellId);
        RemoveSpell(spellId);
      }
    }
    return;
  }

  const espm::ActorValue av = data.primaryAV;
  const espm::MGEF::EffectType type = data.effectType;
  spdlog::trace("Actor value in ApplyMagicEffect(): {}",
                static_cast<std::underlying_type_t<espm::ActorValue>>(av));

  const bool isValue = av == espm::ActorValue::Health ||
    av == espm::ActorValue::Stamina || av == espm::ActorValue::Magicka;
  const bool isRate = av == espm::ActorValue::HealRate ||
    av == espm::ActorValue::StaminaRate || av == espm::ActorValue::MagickaRate;
  const bool isMult =
    av == espm::ActorValue::HealRateMult_or_CombatHealthRegenMultMod ||
    av == espm::ActorValue::StaminaRateMult ||
    av == espm::ActorValue::MagickaRateMult_or_CombatHealthRegenMultPowerMod;

  if (isValue) { // other types are unsupported
    if (hasSweetpie) {
      // this coefficient (workaround) has been added for sake of game
      // balance and because of disability to restrict players use potions
      // often on client side
      constexpr float kMagnitudeCoeff = 100.f;
      RestoreActorValuePatched(this, av, effect.magnitude * kMagnitudeCoeff);
    } else {
      RestoreActorValuePatched(this, av, effect.magnitude);
    }
  }

  if (isRate || isMult) {
    MpChangeForm changeForm = GetChangeForm();
    BaseActorValues baseValues = GetBaseActorValues(
      GetParent(), GetBaseId(), GetRaceId(), changeForm.templateChain);
    const ActiveMagicEffectsMap& activeEffects = changeForm.activeMagicEffects;
    const float baseValue = baseValues.GetValue(av);
    const uint32_t formId = GetFormId();
    auto now = std::chrono::system_clock::now();
    std::chrono::system_clock::time_point endTime;
    std::chrono::milliseconds duration;
    if (durationOverriden) {
      std::optional effect = GetChangeForm().activeMagicEffects.Get(av);
      if (!effect.has_value()) {
        spdlog::error(
          "MpActor with formId {:x} has no magic effect affecting "
          "actor value {}",
          GetFormId(),
          static_cast<std::underlying_type_t<espm::ActorValue>>(av));
        return;
      }
      endTime = effect.value().get().endTime;
      duration =
        std::chrono::duration_cast<std::chrono::milliseconds>(endTime - now);
    } else {
      endTime =
        now + Viet::TimeUtils::To<std::chrono::milliseconds>(effect.duration);
      duration =
        Viet::TimeUtils::To<std::chrono::milliseconds>(effect.duration);
    }
    uint32_t timerId;
    worldState->SetEffectTimer(duration, &timerId)
      .Then([formId, actorValue = av, worldState](Viet::Void) {
        auto& actor = worldState->GetFormAt<MpActor>(formId);
        actor.RemoveMagicEffect(actorValue);
      });

    ActiveMagicEffectsMap::Entry entry{ timerId, effect, endTime };
    if (activeEffects.Has(av)) {
      const ActiveMagicEffectsMap::Entry& entry =
        activeEffects.Get(av).value().get();
      worldState->RemoveEffectTimer(entry.timerId);
    }
    EditChangeForm([av, pEntry = &entry](MpChangeForm& changeForm) {
      changeForm.activeMagicEffects.Add(av, *pEntry);
    });
    if (isRate) {
      SetActorValue(av, effect.magnitude);
    } else {
      float mult = 1.f;
      if (type == espm::MGEF::EffectType::PeakValueMod) {
        mult = MathUtils::PercentToMultPos(effect.magnitude);
      }

      if (type == espm::MGEF::EffectType::ValueMod) {
        mult = MathUtils::PercentToMultNeg(effect.magnitude);
      }
      if (MathUtils::IsNearlyEqual(1.f, mult)) {
        spdlog::error(
          "Unknown espm::MGEF::EffectType: {}",
          static_cast<std::underlying_type_t<espm::MGEF::EffectType>>(type));
      }
      spdlog::trace("Final multiplicator is {}", mult);
      // TODO: proper fix (or effects system) instead of monkey-patching 4x
      // higher mult
      // https://github.com/skyrim-multiplayer/skymp/pull/1852
      spdlog::trace("The result of baseValue * mult is: {}*{}={}", baseValue,
                    mult, baseValue * (mult * 4));
      SetActorValue(av, baseValue * (mult * 4));
    }
  }
}

void MpActor::ApplyMagicEffects(std::vector<espm::Effects::Effect>& effects,
                                bool hasSweetpie, bool durationOverriden)
{
  for (auto& effect : effects) {
    ApplyMagicEffect(effect, hasSweetpie, durationOverriden);
  }
}

void MpActor::RemoveMagicEffect(const espm::ActorValue actorValue)
{
  try {
    const ActorValues baseActorValues = GetBaseActorValues(
      GetParent(), GetBaseId(), GetRaceId(), ChangeForm().templateChain);
    const float baseActorValue = baseActorValues.GetValue(actorValue);
    SetActorValue(actorValue, baseActorValue);
    EditChangeForm([actorValue](MpChangeForm& changeForm) {
      changeForm.activeMagicEffects.Remove(actorValue);
    });
  } catch (std::exception& e) {
    spdlog::error("MpActor::RemoveMagicEffect {:x} - {}", GetFormId(),
                  e.what());
  }
}

void MpActor::RemoveAllMagicEffects()
{
  try {
    const ActorValues baseActorValues = GetBaseActorValues(
      GetParent(), GetBaseId(), GetRaceId(), ChangeForm().templateChain);
    SetActorValues(baseActorValues);
    EditChangeForm(
      [](MpChangeForm& changeForm) { changeForm.activeMagicEffects.Clear(); });
  } catch (std::exception& e) {
    spdlog::error("MpActor::RemoveAllMagicEffects {:x} - {}", GetFormId(),
                  e.what());
  }
}

void MpActor::ReapplyMagicEffects()
{
  // thuum docs/verbs/magic-effects.md: an older record's legacy effects (one
  // per actor value: a regeneration potion's, with its end time) become
  // running effects with the time they have left; the rates and multipliers
  // the legacy path set go back to the game files' (the running buffs' sum
  // counts them now), and the legacy map is cleared. Then the actor's
  // running effects resume, after a restart as after a migration.
  WorldState* worldState = GetParent();
  if (!worldState) {
    return;
  }
  if (!ChangeForm().activeMagicEffects.Empty()) {
    const auto& files = worldState->espmFiles;
    auto entries = EffectEntriesOf(ChangeForm(), files);
    const auto now = std::chrono::system_clock::now();
    for (const auto& [av, legacy] :
         ChangeForm().activeMagicEffects.GetEntries()) {
      try {
        const espm::Effects::Effect& effect = legacy.data;
        const float remaining =
          std::chrono::duration<float>(legacy.endTime - now).count();
        if (remaining <= 0.f) {
          continue;
        }
        const auto facts = EffectFactsOf(worldState, effect.effectId, effect);
        if (!facts) {
          continue;
        }
        auto applied = skymp::rules::apply_effect(
          *facts, 1.f, 0, skymp::rules::EffectArrival::Once);
        if (!applied.has_entry) {
          continue;
        }
        applied.entry.elapsed_s =
          std::max(applied.entry.facts.duration_s - remaining, 0.f);
        // its changes are not made again: the legacy path made them, in its
        // own way, which the reset below undoes
        std::ignore = skymp::rules::admit_effect(entries, applied.entry);
      } catch (std::exception& e) {
        spdlog::warn("MpActor::ReapplyMagicEffects {:x} - {}", GetFormId(),
                     e.what());
      }
    }
    auto running = RunningEffectsOf(entries, files, WallNowMs());
    const BaseActorValues base = GetBaseValues();
    EditChangeForm([&](MpChangeForm& changeForm) {
      changeForm.runningEffects = running;
      changeForm.activeMagicEffects.Clear();
      changeForm.actorValues.healRate = base.healRate;
      changeForm.actorValues.magickaRate = base.magickaRate;
      changeForm.actorValues.staminaRate = base.staminaRate;
      changeForm.actorValues.healRateMult = base.healRateMult;
      changeForm.actorValues.magickaRateMult = base.magickaRateMult;
      changeForm.actorValues.staminaRateMult = base.staminaRateMult;
    });
  }
  if (!HasRunningEffects()) {
    return;
  }
  // The world's clock runs while the server is down (thuum ADR-021), and so
  // do the effects: they run on by the time since they were last counted
  // (an older record's, written before that time was kept, from now). An
  // effect that ended meanwhile gives back what it gave; a poison's course
  // lands at once, as it would have.
  uint64_t asOfMs = 0;
  for (const auto& r : *ChangeForm().runningEffects) {
    asOfMs = std::max(asOfMs, r.asOfMs);
  }
  const uint64_t nowMs = WallNowMs();
  if (asOfMs != 0 && nowMs > asOfMs) {
    AdvanceEffects(static_cast<float>(nowMs - asOfMs) / 1000.f);
  }
  if (HasRunningEffects()) {
    worldState->TrackEffects(GetFormId());
  }
}

std::vector<MpActor::RunningEffectView> MpActor::GetRunningEffects() const
{
  std::vector<RunningEffectView> res;
  WorldState* worldState = GetParent();
  if (!worldState || !HasRunningEffects()) {
    return res;
  }
  for (const auto& e : EffectEntriesOf(ChangeForm(), worldState->espmFiles)) {
    RunningEffectView view;
    view.effect = e.facts.effect;
    view.source = e.facts.source;
    view.magnitude = e.facts.magnitude;
    view.secondsLeft = std::max(e.facts.duration_s - e.elapsed_s, 0.f);
    res.push_back(view);
  }
  return res;
}

std::vector<espm::Effects::Effect> MpActor::EffectsOf(WorldState* worldState,
                                                      uint32_t source)
{
  if (!worldState || !worldState->HasEspm()) {
    return {};
  }
  const auto lookup = worldState->GetEspm().GetBrowser().LookupById(source);
  if (!lookup.rec) {
    return {};
  }
  auto effects =
    espm::Effects(lookup.rec).GetData(worldState->GetEspmCache()).effects;
  for (auto& effect : effects) {
    // an id inside a record counts against that file's masters
    effect.effectId = lookup.ToGlobalId(effect.effectId);
  }
  return effects;
}

bool MpActor::IsHostileArea(WorldState* worldState,
                            const espm::Effects::Effect& effect)
{
  if (effect.areaOfEffect == 0) {
    return false;
  }
  try {
    const auto mgef = espm::GetData<espm::MGEF>(effect.effectId, worldState);
    return mgef.data.IsFlagSet(espm::MGEF::Flags::Detrimental) ||
      mgef.data.IsFlagSet(espm::MGEF::Flags::Hostile);
  } catch (std::exception& e) {
    spdlog::warn("IsHostileArea - effect {:x}: {}", effect.effectId, e.what());
    return false;
  }
}

bool MpActor::IsConditionGated(WorldState* worldState,
                               const espm::Effects::Effect& effect)
{
  if (effect.hasConditions) {
    return true;
  }
  try {
    return espm::GetData<espm::MGEF>(effect.effectId, worldState).callsHasPerk;
  } catch (std::exception& e) {
    spdlog::warn("IsConditionGated - effect {:x}: {}", effect.effectId,
                 e.what());
    return false;
  }
}

bool MpActor::EffectConditionsHold(WorldState* worldState,
                                   const espm::Effects::Effect& effect,
                                   uint32_t source, const MpActor& victim,
                                   const MpActor& caster, bool dualCast)
{
  if (!worldState) {
    return false;
  }
  std::vector<Condition> conditions;
  const auto& map = worldState->conditionFunctionMap;
  auto& browser = worldState->GetEspm().GetBrowser();
  // A form parameter sits in the owning record's own index space (the
  // file's masters, then itself), as the record's EFIDs do: mapped here to
  // the global id through the record, for the functions that declare one
  // (ConditionFunction::IsFormParameter; thuum docs/verbs/spell-cast.md,
  // Hailstone's Shatter names its MGEF as 0x030B2C7F)
  const auto mapForms = [&](Condition& condition,
                            const espm::LookupResult& owner) {
    const auto& fn = map.GetConditionFunction(condition.functionIndex);
    if (!fn || !owner.rec) {
      return;
    }
    if (fn->IsFormParameter(1)) {
      const auto raw =
        ConditionsEvaluator::ExtractParameter(condition.parameter1);
      condition.parameter1 = fmt::format("0x{:X}", owner.ToGlobalId(raw));
    }
    if (fn->IsFormParameter(2)) {
      const auto raw =
        ConditionsEvaluator::ExtractParameter(condition.parameter2);
      condition.parameter2 = fmt::format("0x{:X}", owner.ToGlobalId(raw));
    }
  };
  const auto spellRecord = browser.LookupById(source);
  for (const auto& ctda : effect.conditions) {
    auto condition = Condition::FromCtda(ctda);
    mapForms(condition, spellRecord);
    conditions.push_back(std::move(condition));
  }
  try {
    const auto mgefRecord = browser.LookupById(effect.effectId);
    const auto mgef = espm::GetData<espm::MGEF>(effect.effectId, worldState);
    for (const auto& ctda : mgef.conditions) {
      auto condition = Condition::FromCtda(ctda);
      mapForms(condition, mgefRecord);
      conditions.push_back(std::move(condition));
    }
  } catch (std::exception& e) {
    spdlog::warn("EffectConditionsHold - effect {:x}: {}; refused",
                 effect.effectId, e.what());
    return false;
  }
  if (conditions.empty()) {
    return true;
  }
  for (const auto& condition : conditions) {
    if (!map.GetConditionFunction(condition.functionIndex)) {
      // the refusal stands per hit; the line is worth once per effect and
      // function (playtest twenty-four: 139 of these in an hour, two per
      // tick of a stream, for riders gated on HasPerk, which the server
      // refuses by design until it holds perks), then debug
      static std::set<std::pair<uint32_t, uint16_t>> seen;
      const bool first =
        seen.insert({ effect.effectId, condition.functionIndex }).second;
      const auto line = fmt::format(
        "MpActor::EffectConditionsHold - E_CONDITION_UNKNOWN: function {} "
        "({}) on effect {:x} of {:x}, cast by {:x} at {:x}; refused (thuum "
        "ADR-039){}",
        condition.function.empty() ? "?" : condition.function,
        condition.functionIndex, effect.effectId, source, caster.GetFormId(),
        victim.GetFormId(), first ? "; logged once, later ones at debug" : "");
      if (first) {
        spdlog::warn("{}", line);
      } else {
        spdlog::debug("{}", line);
      }
      return false;
    }
  }
  bool holds = false;
  ConditionEvaluatorContext context;
  context.damageSourceFormId = source;
  context.effectVictimFormId = victim.GetFormId();
  context.effectCasterFormId = caster.GetFormId();
  context.effectWasDualCast = dualCast;
  ConditionsEvaluator::EvaluateConditions(
    map, worldState->conditionsEvaluatorSettings,
    ConditionsEvaluatorCaller::kMagicEffect, conditions, victim, caster,
    [&](bool result, std::vector<std::string>&) { holds = result; }, context);
  if (!holds) {
    spdlog::info("MpActor::EffectConditionsHold - effect {:x} of {:x} by {:x} "
                 "at {:x}: its conditions fail; not applied (thuum ADR-039)",
                 effect.effectId, source, caster.GetFormId(),
                 victim.GetFormId());
  }
  return holds;
}

void MpActor::ApplyEffects(uint32_t source,
                           const std::vector<espm::Effects::Effect>& effects,
                           float scale, uint32_t caster, bool stream,
                           bool dualCast, float streamSeconds)
{
  WorldState* worldState = GetParent();
  if (!worldState || IsDead()) {
    return;
  }
  // thuum docs/DECISIONS.md ADR-039: another actor's effect is applied only
  // if its conditions hold for this actor; a self effect keeps the gate
  // below (ADR-029's default: the caster's own game runs it)
  std::shared_ptr<MpActor> casterActor;
  if (caster != 0 && caster != GetFormId()) {
    casterActor =
      std::dynamic_pointer_cast<MpActor>(worldState->LookupFormById(caster));
  }
  const auto& files = worldState->espmFiles;
  const BaseActorValues maxBefore = GetMaximumValues();
  auto entries = EffectEntriesOf(ChangeForm(), files);
  const size_t entriesBefore = entries.size();
  const auto modifiersBefore = EffectModifiersOf(entries);
  std::vector<MagicEffectsMessage::Effect> shownOnce;
  std::array<float, 3> deltas{};
  std::array<bool, 3> touched{};
  for (const auto& effect : effects) {
    if (casterActor) {
      if (!EffectConditionsHold(worldState, effect, source, *this,
                                *casterActor, dualCast)) {
        continue;
      }
    } else if (IsConditionGated(worldState, effect)) {
      // a self effect, until the server evaluates its conditions: a perk's
      // extras stay out, whether the spell gates them or their own record
      continue;
    }
    auto facts = EffectFactsOf(worldState, source, effect);
    if (!facts) {
      continue;
    }
    if (streamSeconds > 0.f) {
      // a Self concentration cast: the effect runs for the cast's span at
      // its magnitude a second (thuum docs/verbs/self-cast.md, train-31)
      facts->duration_s = streamSeconds;
      facts->no_duration = false;
    }
    // the target's resistances take their share of a hostile effect before
    // anything is applied (thuum docs/verbs/magic-effects.md, train-30)
    const float resisted = scale * ResistanceFactorOf(source, effect);
    const auto applied =
      skymp::rules::apply_effect(*facts, resisted, caster,
                                 stream ? skymp::rules::EffectArrival::Stream
                                        : skymp::rules::EffectArrival::Once);
    SumAttributeChanges(applied.changes, deltas, touched, pImpl->godMode);
    if (applied.has_entry) {
      SumAttributeChanges(skymp::rules::admit_effect(entries, applied.entry),
                          deltas, touched, pImpl->godMode);
    } else if (!applied.changes.empty() && !stream) {
      // an effect without duration: shown once (a stream's hits show
      // through the caster's figure's stream instead)
      MagicEffectsMessage::Effect once;
      once.effect = facts->effect;
      once.source = source;
      once.magnitude = facts->magnitude * resisted;
      shownOnce.push_back(once);
    }
  }
  // a buff replaced at the bound keeps the count but moves the sums
  const bool setChanged = entries.size() != entriesBefore ||
    EffectModifiersOf(entries) != modifiersBefore;
  auto running = RunningEffectsOf(entries, files, WallNowMs());
  EditChangeForm(
    [&](MpChangeForm& changeForm) { changeForm.runningEffects = running; });
  if (running) {
    worldState->TrackEffects(GetFormId());
  }
  // the set and its sums first, then the values: on the one ordered
  // channel, the actor's own game moves its maximum before it takes the
  // percentage, as the server did
  if (setChanged || !shownOnce.empty()) {
    SendMessageToActorListeners(MagicEffectsOf(GetIdx(), entries, shownOnce),
                                true);
  }
  ApplyAttributeDeltas(deltas, touched, maxBefore, caster);
}

void MpActor::AdvanceEffects(float dtSeconds)
{
  WorldState* worldState = GetParent();
  if (!worldState || !HasRunningEffects()) {
    return;
  }
  if (IsDead()) {
    // a death ends the effects; the respawn restores the attributes
    EditChangeForm(
      [&](MpChangeForm& changeForm) { changeForm.runningEffects.reset(); });
    return;
  }
  const auto& files = worldState->espmFiles;
  const BaseActorValues maxBefore = GetMaximumValues();
  auto entries = EffectEntriesOf(ChangeForm(), files);
  const size_t entriesBefore = entries.size();
  const auto changes = skymp::rules::advance_effects(entries, dtSeconds);
  auto running = RunningEffectsOf(entries, files, WallNowMs());
  EditChangeForm(
    [&](MpChangeForm& changeForm) { changeForm.runningEffects = running; });
  std::array<float, 3> deltas{};
  std::array<bool, 3> touched{};
  SumAttributeChanges(changes, deltas, touched, pImpl->godMode);
  if (entries.size() != entriesBefore) {
    // an effect ended: every game drops it (each counts the seconds down
    // itself, so a running one needs no message), before the values, as in
    // ApplyEffects
    SendMessageToActorListeners(MagicEffectsOf(GetIdx(), entries, {}), true);
  }
  ApplyAttributeDeltas(deltas, touched, maxBefore, 0);
}

void MpActor::SendMagicEffectsTo(MpActor& listener)
{
  WorldState* worldState = GetParent();
  if (!worldState || !HasRunningEffects()) {
    return;
  }
  const auto entries = EffectEntriesOf(ChangeForm(), worldState->espmFiles);
  listener.SendToUser(MagicEffectsOf(GetIdx(), entries, {}), true);
}

bool MpActor::IsGodMode() const noexcept
{
  return pImpl->godMode;
}

void MpActor::SetGodMode(bool on) noexcept
{
  pImpl->godMode = on;
}

bool MpActor::IsImmortal() const noexcept
{
  return pImpl->immortal;
}

void MpActor::SetImmortal(bool on) noexcept
{
  pImpl->immortal = on;
}

bool MpActor::HasRunningEffects() const
{
  const auto& running = ChangeForm().runningEffects;
  return running && !running->empty();
}

bool MpActor::HasRunningEffectOfType(espm::MGEF::EffectType type) const
{
  WorldState* worldState = GetParent();
  if (!worldState || !HasRunningEffects()) {
    return false;
  }
  for (const auto& e : EffectEntriesOf(ChangeForm(), worldState->espmFiles)) {
    try {
      if (espm::GetData<espm::MGEF>(e.facts.effect, worldState)
            .data.effectType == type) {
        return true;
      }
    } catch (std::exception&) {
      // a record no longer loaded is no such effect
    }
  }
  return false;
}

bool MpActor::EndRunningEffectsOfTypes(
  std::initializer_list<espm::MGEF::EffectType> types)
{
  WorldState* worldState = GetParent();
  if (!worldState || !HasRunningEffects()) {
    return false;
  }
  std::vector<uint32_t> ending;
  for (const auto& e : EffectEntriesOf(ChangeForm(), worldState->espmFiles)) {
    try {
      const auto type =
        espm::GetData<espm::MGEF>(e.facts.effect, worldState).data.effectType;
      if (std::find(types.begin(), types.end(), type) != types.end()) {
        ending.push_back(e.facts.effect);
      }
    } catch (std::exception&) {
    }
  }
  return EndRunningEffectIds(ending);
}

bool MpActor::EndRunningEffectsOfSource(uint32_t source)
{
  WorldState* worldState = GetParent();
  if (!worldState || !HasRunningEffects()) {
    return false;
  }
  std::vector<uint32_t> ending;
  for (const auto& e : EffectEntriesOf(ChangeForm(), worldState->espmFiles)) {
    if (e.facts.source == source) {
      ending.push_back(e.facts.effect);
    }
  }
  return EndRunningEffectIds(ending);
}

bool MpActor::EndRunningEffectIds(const std::vector<uint32_t>& ending)
{
  WorldState* worldState = GetParent();
  if (!worldState || ending.empty()) {
    return false;
  }
  const auto& files = worldState->espmFiles;
  auto entries = EffectEntriesOf(ChangeForm(), files);
  const BaseActorValues maxBefore = GetMaximumValues();
  const auto changes = skymp::rules::remove_effects(
    entries, rust::Slice<const uint32_t>(ending.data(), ending.size()));
  auto running = RunningEffectsOf(entries, files, WallNowMs());
  EditChangeForm(
    [&](MpChangeForm& changeForm) { changeForm.runningEffects = running; });
  std::array<float, 3> deltas{};
  std::array<bool, 3> touched{};
  SumAttributeChanges(changes, deltas, touched, pImpl->godMode);
  // the set first, then the values, as AdvanceEffects does
  SendMessageToActorListeners(MagicEffectsOf(GetIdx(), entries, {}), true);
  ApplyAttributeDeltas(deltas, touched, maxBefore, 0);
  return true;
}

namespace {
// The constant effects of a race's abilities (RACE SPLO: each an Ability
// spell of constant cast type, a Nord's Resist Frost 000AA020 above all)
// as the rule's facts, read once per race from the records: the engine
// keeps them on the actor from its first frame, the client's ActorValues
// report carries bases alone, and the server applied none of them (thuum
// docs/verbs/spell-cast.md, Hailstone, train-30). Empty for a race the
// records lack.
const rust::Vec<skymp::rules::EffectFacts>& RaceAbilityFactsOf(
  WorldState* worldState, uint32_t raceId)
{
  static std::unordered_map<uint32_t, rust::Vec<skymp::rules::EffectFacts>>
    cache;
  static const rust::Vec<skymp::rules::EffectFacts> none;
  if (!worldState || raceId == 0) {
    return none;
  }
  if (const auto it = cache.find(raceId); it != cache.end()) {
    return it->second;
  }
  rust::Vec<skymp::rules::EffectFacts> facts;
  try {
    const auto race = worldState->GetEspm().GetBrowser().LookupById(raceId);
    const auto raceData = espm::GetData<espm::RACE>(raceId, worldState);
    for (const auto rawSpell : raceData.spells) {
      const auto spellId = race.ToGlobalId(rawSpell);
      const auto spell = espm::GetData<espm::SPEL>(spellId, worldState);
      if (!spell.spellItem ||
          spell.spellItem->type != espm::SPEL::SpellType::Ability ||
          spell.spellItem->castType != espm::SPEL::CastType::ConstantEffect) {
        continue;
      }
      for (const auto& effect : MpActor::EffectsOf(worldState, spellId)) {
        if (auto f = EffectFactsOf(worldState, spellId, effect)) {
          facts.push_back(*f);
        }
      }
    }
  } catch (std::exception& e) {
    spdlog::warn("RaceAbilityFactsOf - race {:x}: {}", raceId, e.what());
  }
  return cache.emplace(raceId, std::move(facts)).first->second;
}
}

float MpActor::GetCurrentActorValue(espm::ActorValue av)
{
  if (av == espm::ActorValue::Health || av == espm::ActorValue::Magicka ||
      av == espm::ActorValue::Stamina) {
    const auto maximum = GetMaximumValues();
    const auto& values = GetActorValues();
    if (av == espm::ActorValue::Health) {
      return maximum.health * values.healthPercentage;
    }
    if (av == espm::ActorValue::Magicka) {
      return maximum.magicka * values.magickaPercentage;
    }
    return maximum.stamina * values.staminaPercentage;
  }
  const auto index = static_cast<int32_t>(av);
  if (index < 0 || index > 0xff) {
    return 0.f;
  }
  const auto base = GetRecordedActorValueBase(static_cast<uint8_t>(index));
  return base.value_or(0.f) + GetEffectModifier(av);
}

float MpActor::ResistanceFactorOf(uint32_t source,
                                  const espm::Effects::Effect& effect)
{
  // The engine's own resistance never runs on a player's Health, Magicka
  // or Stamina damage from another actor (the player's AddTarget refuses
  // those effects; the server's number is imposed, R0), so the server
  // applies it: only a Hostile effect is resisted (MGEF flag 0x1), a
  // spell flagged IgnoreResistance is not, Magic Resistance applies to a
  // spell's or an enchantment's effect and not to a poison's or a
  // potion's (an ALCH source), the element's own value is the MGEF's
  // ResistanceAV read on this actor, and the player's cap is 85
  // (fPlayerMaxResistance's executable default, which the masters do not
  // set: ActionListener BlockSettingsOf, thuum ghidra/notes/block-formula.md);
  // an NPC's cap is 100, immunity. HYPOTHESIS until x-hailstone: the
  // engine's order and cap for a dual value modifier's second value.
  WorldState* worldState = GetParent();
  if (!worldState) {
    return 1.f;
  }
  espm::MGEF::DATA mgef;
  try {
    mgef = espm::GetData<espm::MGEF>(effect.effectId, worldState).data;
  } catch (std::exception&) {
    return 1.f;
  }
  if (!mgef.IsFlagSet(espm::MGEF::Flags::Hostile)) {
    return 1.f;
  }
  bool magicResists = true;
  const auto sourceRecord =
    worldState->GetEspm().GetBrowser().LookupById(source);
  if (sourceRecord.rec && sourceRecord.rec->GetType() == "ALCH") {
    magicResists = false;
  }
  if (sourceRecord.rec && sourceRecord.rec->GetType() == "SPEL") {
    try {
      const auto spell = espm::GetData<espm::SPEL>(source, worldState);
      if (spell.spellItem &&
          (static_cast<uint32_t>(spell.spellItem->flags) &
           static_cast<uint32_t>(espm::SPEL::SpellFlags::IgnoreResistance)) !=
            0) {
        return 1.f;
      }
    } catch (std::exception&) {
    }
  }
  const float cap = IsCreatedAsPlayer() ? 85.f : 100.f;
  const float magic =
    magicResists ? GetCurrentActorValue(espm::ActorValue::MagicResist) : 0.f;
  const float element = mgef.resistValue >= 0
    ? GetCurrentActorValue(static_cast<espm::ActorValue>(mgef.resistValue))
    : 0.f;
  return skymp::rules::resistance_factor(element, magic, cap);
}

float MpActor::GetEffectModifier(espm::ActorValue av) const
{
  WorldState* worldState = GetParent();
  if (!worldState) {
    return 0.f;
  }
  const auto avIndex = static_cast<uint32_t>(av);
  // the race's constant abilities stand under every running effect
  float modifier = skymp::rules::ability_modifier(
    RaceAbilityFactsOf(worldState, GetRaceId()), avIndex);
  if (HasRunningEffects()) {
    const auto entries = EffectEntriesOf(ChangeForm(), worldState->espmFiles);
    modifier += skymp::rules::effect_modifier(entries, avIndex);
  }
  return modifier;
}

void MpActor::ApplyAttributeDeltas(const std::array<float, 3>& deltas,
                                   const std::array<bool, 3>& touched,
                                   const BaseActorValues& maxBefore,
                                   uint32_t caster)
{
  if (!touched[0] && !touched[1] && !touched[2]) {
    return;
  }
  const BaseActorValues maxAfter = GetMaximumValues();
  ActorValues values = GetChangeForm().actorValues;
  std::vector<espm::ActorValue> avs;
  // a buff's change moves the value with the maximum: the points against
  // the maximum before, the share against the maximum after
  auto move = [](float& percentage, float before, float after, float delta) {
    if (before <= 0.f || after <= 0.f) {
      return;
    }
    percentage = std::clamp((percentage * before + delta) / after, 0.f, 1.f);
  };
  if (touched[0]) {
    move(values.healthPercentage, maxBefore.health, maxAfter.health,
         deltas[0]);
    avs.push_back(espm::ActorValue::Health);
  }
  if (touched[1]) {
    move(values.magickaPercentage, maxBefore.magicka, maxAfter.magicka,
         deltas[1]);
    avs.push_back(espm::ActorValue::Magicka);
  }
  if (touched[2]) {
    move(values.staminaPercentage, maxBefore.stamina, maxAfter.stamina,
         deltas[2]);
    avs.push_back(espm::ActorValue::Stamina);
  }
  MpActor* aggressor = nullptr;
  if (caster) {
    if (const auto& form = GetParent()->LookupFormByIdNoLoad(caster)) {
      aggressor = form->AsActor();
    }
  }
  NetSetPercentages(values, aggressor, avs);
  // a buff that moved a maximum moves the figure's base in the other games
  // too (thuum docs/verbs/figure-values.md: c1 drinks, c2's figure follows)
  if (maxAfter.health != maxBefore.health ||
      maxAfter.magicka != maxBefore.magicka ||
      maxAfter.stamina != maxBefore.stamina) {
    SendMaximaToListeners();
  }
}

std::array<std::optional<Inventory::Entry>, 2> MpActor::GetEquippedWeapon()
  const
{
  std::array<std::optional<Inventory::Entry>, 2> wornEntries;
  // 0 -> left hand, 1 -> right hand
  auto& espmBrowser = GetParent()->GetEspm().GetBrowser();
  for (const auto& entry : GetEquipment().inv.entries) {
    if (entry.GetWorn() != Inventory::Worn::None) {
      espm::LookupResult res = espmBrowser.LookupById(entry.baseId);
      auto* record = espm::Convert<espm::WEAP>(res.rec);
      if (record) {
        if (entry.GetWorn() == Inventory::Worn::Left) {
          wornEntries[0] = std::move(entry);
        }
        if (entry.GetWorn() == Inventory::Worn::Right) {
          wornEntries[1] = std::move(entry);
        }
      }
    }
  }
  return wornEntries;
}

std::array<std::optional<Inventory::Entry>, 2> MpActor::GetEquippedScroll()
  const
{
  std::array<std::optional<Inventory::Entry>, 2> wornEntries;
  // 0 -> left hand, 1 -> right hand
  auto& espmBrowser = GetParent()->GetEspm().GetBrowser();
  for (const auto& entry : GetEquipment().inv.entries) {
    if (entry.GetWorn() != Inventory::Worn::None) {
      espm::LookupResult res = espmBrowser.LookupById(entry.baseId);
      auto* record = espm::Convert<espm::SCRL>(res.rec);
      if (record) {
        if (entry.GetWorn() == Inventory::Worn::Left) {
          wornEntries[0] = std::move(entry);
        }
        if (entry.GetWorn() == Inventory::Worn::Right) {
          wornEntries[1] = std::move(entry);
        }
      }
    }
  }
  return wornEntries;
}

std::array<std::optional<Inventory::Entry>, 2> MpActor::GetEquippedLight()
  const
{
  std::array<std::optional<Inventory::Entry>, 2> wornEntries;
  // 0 -> left hand, 1 -> right hand
  auto& espmBrowser = GetParent()->GetEspm().GetBrowser();
  for (const auto& entry : GetEquipment().inv.entries) {
    if (entry.GetWorn() != Inventory::Worn::None) {
      espm::LookupResult res = espmBrowser.LookupById(entry.baseId);
      auto* record = espm::Convert<espm::LIGH>(res.rec);
      if (record) {
        if (entry.GetWorn() == Inventory::Worn::Left) {
          wornEntries[0] = std::move(entry);
        }
        if (entry.GetWorn() == Inventory::Worn::Right) {
          wornEntries[1] = std::move(entry);
        }
      }
    }
  }
  return wornEntries;
}

std::array<std::optional<Inventory::Entry>, 2> MpActor::GetEquippedShield()
  const
{
  std::array<std::optional<Inventory::Entry>, 2> wornEntries;
  // 0 -> left hand, 1 -> right hand
  auto& espmBrowser = GetParent()->GetEspm().GetBrowser();
  auto& espmCache = GetParent()->GetEspmCache();
  constexpr uint32_t kBodShield = 0x00000200;

  for (const auto& entry : GetEquipment().inv.entries) {
    if (entry.GetWorn() != Inventory::Worn::None) {
      espm::LookupResult res = espmBrowser.LookupById(entry.baseId);
      auto* record = espm::Convert<espm::ARMO>(res.rec);
      if (record) {
        auto data = record->GetData(espmCache);
        bool isShield = false;

        if (data.bod2.present) {
          isShield = (data.bod2.bodyPartFlags & kBodShield) != 0;
        } else if (data.bodt.present) {
          isShield = (data.bodt.bodyPartFlags & kBodShield) != 0;
        } else {
          continue;
        }

        if (!isShield) {
          continue;
        }

        // In Skyrim, in the equipment data, the shield has the worn flag and
        // not the wornLeft, although it is actually in the left hand.
        if (entry.GetWorn() == Inventory::Worn::Right) {
          wornEntries[0] = std::move(entry);
        }
      }
    }
  }
  return wornEntries;
}
