#include "PapyrusActor.h"

#include "MpActor.h"
#include "SpSnippetFunctionGen.h"
#include "script_objects/EspmGameObject.h"
#include "script_objects/MpFormGameObject.h"

#include "EvaluateTemplate.h"
#include "papyrus-vm/CIString.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <set>
#include <string>

namespace {
espm::ActorValue ConvertToAV(CIString actorValueName)
{
  if (!actorValueName.compare("health")) {
    return espm::ActorValue::Health;
  }
  if (!actorValueName.compare("stamina")) {
    return espm::ActorValue::Stamina;
  }
  if (!actorValueName.compare("magicka")) {
    return espm::ActorValue::Magicka;
  }
  return espm::ActorValue::None;
}

// thuum docs/verbs/actor-values.md: an actor value's index from its Papyrus
// name as the lab confirmed it (wire-rules actor_values::NAMES); -1 for any
// other name
int ActorValueIndex(const VarValue& name)
{
  const char* s = static_cast<const char*>(name);
  return skymp::rules::actor_value_index(rust::Str(s ? s : ""));
}

// CommonLibSSE-NG include/RE/A/ActorValues.h: kHealth 24, kMagicka 25,
// kStamina 26, the three the server keeps as percentages of a maximum
espm::ActorValue Attribute(int index)
{
  switch (index) {
    case 24:
      return espm::ActorValue::Health;
    case 25:
      return espm::ActorValue::Magicka;
    case 26:
      return espm::ActorValue::Stamina;
    default:
      return espm::ActorValue::None;
  }
}

float AttributeOf(const ActorValues& values, espm::ActorValue av)
{
  switch (av) {
    case espm::ActorValue::Health:
      return values.health;
    case espm::ActorValue::Magicka:
      return values.magicka;
    case espm::ActorValue::Stamina:
      return values.stamina;
    default:
      return 0.f;
  }
}

float PercentageOf(const ActorValues& values, espm::ActorValue av)
{
  switch (av) {
    case espm::ActorValue::Health:
      return values.healthPercentage;
    case espm::ActorValue::Magicka:
      return values.magickaPercentage;
    case espm::ActorValue::Stamina:
      return values.staminaPercentage;
    default:
      return 0.f;
  }
}

// a base the server does not know: none recorded yet for a player, or any
// value but the three attributes on another actor. 0, logged once per name
// and native
VarValue Unknown(const char* native, const VarValue& name)
{
  static std::set<std::string> logged;
  const char* s = static_cast<const char*>(name);
  std::string key = std::string(native) + ":" + (s ? s : "");
  if (logged.insert(key).second) {
    spdlog::info("Actor.{}: the server holds no value for '{}' on this "
                 "actor; 0 (thuum docs/verbs/actor-values.md)",
                 native, s ? s : "");
  }
  return VarValue(0.f);
}

// the base the server knows for an actor value on an actor: a player's
// record (or a value the server set and holds), else Health, Magicka or
// Stamina from the race's and the base NPC's values
std::optional<float> KnownBase(MpActor& actor, int index)
{
  if (index < 0) {
    return std::nullopt;
  }
  if (actor.IsCreatedAsPlayer()) {
    if (auto base =
          actor.GetRecordedActorValueBase(static_cast<uint8_t>(index))) {
      return base;
    }
  }
  const auto attribute = Attribute(index);
  if (attribute != espm::ActorValue::None) {
    return AttributeOf(actor.GetBaseValues(), attribute);
  }
  return std::nullopt;
}
}

VarValue PapyrusActor::DrawWeapon(VarValue self,
                                  const std::vector<VarValue>& arguments)
{
  // TODO: consider making this SpSnippetMode::kNoReturnResult
  return ExecuteSpSnippetAndGetPromise(GetName(), "DrawWeapon",
                                       compatibilityPolicy, self, arguments,
                                       true, SpSnippetMode::kReturnResult);
}

VarValue PapyrusActor::UnequipAll(VarValue self,
                                  const std::vector<VarValue>& arguments)
{
  // TODO: consider making this SpSnippetMode::kNoReturnResult
  return ExecuteSpSnippetAndGetPromise(GetName(), "UnequipAll",
                                       compatibilityPolicy, self, arguments,
                                       true, SpSnippetMode::kReturnResult);
}

VarValue PapyrusActor::PlayIdle(VarValue self,
                                const std::vector<VarValue>& arguments)
{
  // TODO: consider making this SpSnippetMode::kNoReturnResult
  return ExecuteSpSnippetAndGetPromise(GetName(), "PlayIdle",
                                       compatibilityPolicy, self, arguments,
                                       true, SpSnippetMode::kReturnResult);
}

VarValue PapyrusActor::GetSitState(VarValue self,
                                   const std::vector<VarValue>& arguments)
{
  // TODO: make this non-latent
  return ExecuteSpSnippetAndGetPromise(GetName(), "GetSitState",
                                       compatibilityPolicy, self, arguments,
                                       true, SpSnippetMode::kReturnResult);
}

VarValue PapyrusActor::IsWeaponDrawn(VarValue self,
                                     const std::vector<VarValue>& arguments)
{
  if (auto actor = GetFormPtr<MpActor>(self)) {
    return VarValue(actor->IsWeaponDrawn());
  }
  return VarValue(false);
}

VarValue PapyrusActor::RestoreActorValue(
  VarValue self, const std::vector<VarValue>& arguments)
{
  espm::ActorValue attributeName =
    ConvertToAV(static_cast<const char*>(arguments[0]));
  float modifier = static_cast<double>(arguments[1]);
  if (auto actor = GetFormPtr<MpActor>(self)) {
    actor->RestoreActorValue(attributeName, modifier);
  }
  return VarValue();
}

namespace {
// SetActorValue's path before the actor-values verb, kept for every actor
// but a player and for names the lab confirmed no index for: the host's game
// runs the native (R2, its result not recorded). SpSnippet helps scripted
// draugrs attack (their Aggression), nothing more.
void DelegateToHost(const char* className, const char* function,
                    MpActor& actor, const std::vector<VarValue>& arguments)
{
  spdlog::warn("{} executes locally at this moment. Results will not "
               "affect server calculations",
               function);
  auto it = actor.GetParent()->hosters.find(actor.GetFormId());
  auto serializedArgs =
    SpSnippetFunctionGen::SerializeArguments(arguments, actor.GetParent());
  // spsnippet don't support auto sending to host. so determining current
  // hoster explicitly
  SpSnippet(className, function, serializedArgs, actor.GetFormId())
    .Execute(it == actor.GetParent()->hosters.end()
               ? &actor
               : &actor.GetParent()->GetFormAt<MpActor>(it->second),
             SpSnippetMode::kNoReturnResult);
}

void RequireArguments(const char* native,
                      const std::vector<VarValue>& arguments, size_t count)
{
  if (arguments.size() < count) {
    throw std::runtime_error(fmt::format(
      "Papyrus Actor.{}: wrong argument count", native));
  }
}
}

// thuum docs/verbs/actor-values.md: on a player the server sets the base
// (R0): recorded, held against the player's stale reports, sent to its game.
// The Creation Kit wiki's SetActorValue sets the base (HYPOTHESIS until the
// a-actor-values scenario reads it back through the game's own natives).
VarValue PapyrusActor::SetActorValue(VarValue self,
                                     const std::vector<VarValue>& arguments)
{
  RequireArguments("SetActorValue", arguments, 2);
  if (auto actor = GetFormPtr<MpActor>(self)) {
    const int index = ActorValueIndex(arguments[0]);
    const float value = static_cast<double>(arguments[1]);
    if (actor->IsCreatedAsPlayer() && index >= 0) {
      if (!actor->SetActorValueBaseByServer(static_cast<uint8_t>(index),
                                            value)) {
        spdlog::info("Actor.SetActorValue: {} for actor value {} of {:x} "
                     "refused (wire-rules actor_values_set_ok)",
                     value, index, actor->GetFormId());
      }
      return VarValue();
    }
    DelegateToHost(GetName(), "SetActorValue", *actor, arguments);
  }
  return VarValue();
}

// On a player, the base plus the change (R0); the Creation Kit wiki's
// ModActorValue changes the base the same way (HYPOTHESIS until the
// scenario reads it back). Before the player's first report the server knows
// no base for a skill and changes nothing.
VarValue PapyrusActor::ModActorValue(VarValue self,
                                     const std::vector<VarValue>& arguments)
{
  RequireArguments("ModActorValue", arguments, 2);
  if (auto actor = GetFormPtr<MpActor>(self)) {
    const int index = ActorValueIndex(arguments[0]);
    const float delta = static_cast<double>(arguments[1]);
    if (actor->IsCreatedAsPlayer() && index >= 0) {
      auto base = KnownBase(*actor, index);
      if (!base) {
        Unknown("ModActorValue", arguments[0]);
        return VarValue();
      }
      actor->SetActorValueBaseByServer(static_cast<uint8_t>(index),
                                       *base + delta);
      return VarValue();
    }
    DelegateToHost(GetName(), "ModActorValue", *actor, arguments);
  }
  return VarValue();
}

// The current value. Health, Magicka and Stamina: the server's percentage
// moved to the value within the maximum, as Restore and Damage move it, on
// any actor (R0). Any other value on a player: the server records bases
// only, so the base is set (HYPOTHESIS: the game's ForceActorValue leaves
// the base; until the scenario reads it, the record keeps one number).
VarValue PapyrusActor::ForceActorValue(VarValue self,
                                       const std::vector<VarValue>& arguments)
{
  RequireArguments("ForceActorValue", arguments, 2);
  if (auto actor = GetFormPtr<MpActor>(self)) {
    const int index = ActorValueIndex(arguments[0]);
    const float value = static_cast<double>(arguments[1]);
    const auto attribute = Attribute(index);
    if (attribute != espm::ActorValue::None) {
      const float maximum = AttributeOf(actor->GetMaximumValues(), attribute);
      const float current =
        maximum * PercentageOf(actor->GetChangeForm().actorValues, attribute);
      if (value > current) {
        actor->RestoreActorValue(attribute, value - current);
      } else if (value < current) {
        actor->DamageActorValue(attribute, current - value);
      }
      return VarValue();
    }
    if (actor->IsCreatedAsPlayer() && index >= 0) {
      actor->SetActorValueBaseByServer(static_cast<uint8_t>(index), value);
      return VarValue();
    }
    DelegateToHost(GetName(), "ForceActorValue", *actor, arguments);
  }
  return VarValue();
}

// The current value: Health, Magicka and Stamina as the server's percentage
// of the maximum; any other value, the base the server knows (it records
// no modifiers).
VarValue PapyrusActor::GetActorValue(VarValue self,
                                     const std::vector<VarValue>& arguments)
{
  RequireArguments("GetActorValue", arguments, 1);
  if (auto actor = GetFormPtr<MpActor>(self)) {
    const int index = ActorValueIndex(arguments[0]);
    const auto attribute = Attribute(index);
    if (attribute != espm::ActorValue::None) {
      return VarValue(
        AttributeOf(actor->GetMaximumValues(), attribute) *
        PercentageOf(actor->GetChangeForm().actorValues, attribute));
    }
    if (auto base = KnownBase(*actor, index)) {
      return VarValue(*base);
    }
    return Unknown("GetActorValue", arguments[0]);
  }
  return VarValue(0.f);
}

VarValue PapyrusActor::GetBaseActorValue(
  VarValue self, const std::vector<VarValue>& arguments)
{
  RequireArguments("GetBaseActorValue", arguments, 1);
  if (auto actor = GetFormPtr<MpActor>(self)) {
    if (auto base = KnownBase(*actor, ActorValueIndex(arguments[0]))) {
      return VarValue(*base);
    }
    return Unknown("GetBaseActorValue", arguments[0]);
  }
  return VarValue(0.f);
}

// Health, Magicka and Stamina: the maximum their percentages count against
// (a player's recorded base); any other value, its base.
VarValue PapyrusActor::GetActorValueMax(
  VarValue self, const std::vector<VarValue>& arguments)
{
  RequireArguments("GetActorValueMax", arguments, 1);
  if (auto actor = GetFormPtr<MpActor>(self)) {
    const int index = ActorValueIndex(arguments[0]);
    const auto attribute = Attribute(index);
    if (attribute != espm::ActorValue::None) {
      return VarValue(AttributeOf(actor->GetMaximumValues(), attribute));
    }
    if (auto base = KnownBase(*actor, index)) {
      return VarValue(*base);
    }
    return Unknown("GetActorValueMax", arguments[0]);
  }
  return VarValue(0.f);
}

VarValue PapyrusActor::DamageActorValue(VarValue self,
                                        const std::vector<VarValue>& arguments)
{
  espm::ActorValue attributeName =
    ConvertToAV(static_cast<const char*>(arguments[0]));
  float modifier = static_cast<double>(arguments[1]);
  if (auto actor = GetFormPtr<MpActor>(self)) {
    actor->DamageActorValue(attributeName, modifier);
  }
  return VarValue();
}

VarValue PapyrusActor::IsEquipped(VarValue self,
                                  const std::vector<VarValue>& arguments)
{
  if (arguments.size() < 1) {
    throw std::runtime_error("Papyrus Actor IsEquipped: wrong argument count");
  }

  auto selfRefr = GetFormPtr<MpForm>(self);
  auto actor = GetFormPtr<MpActor>(self);
  auto form = GetRecordPtr(arguments[0]);

  if (!form.rec) {
    return VarValue(false);
  }

  std::vector<uint32_t> formIds;

  if (auto formlist = espm::Convert<espm::FLST>(form.rec)) {
    formIds =
      espm::GetData<espm::FLST>(formlist->GetId(), selfRefr->GetParent())
        .formIds;
  } else {
    formIds.emplace_back(form.ToGlobalId(form.rec->GetId()));
  }

  auto equipment = actor->GetEquipment().inv;
  // Enum entries of equipment
  for (auto& entry : equipment.entries) {
    // Filter out non-worn (in current implementation it is possible)
    if (entry.GetWorn() == Inventory::Worn::Right ||
        entry.GetWorn() == Inventory::Worn::Left) {
      // Enum entries of form list
      for (const auto& formId : formIds) {
        // If one of equipment entries matches one of formlist entries, then
        // return true
        if (entry.baseId == formId) {
          return VarValue(true);
        }
      }
    }
  }

  return VarValue(false);
}

VarValue PapyrusActor::GetActorValuePercentage(
  VarValue self, const std::vector<VarValue>& arguments)
{
  if (arguments.size() < 1) {
    throw std::runtime_error(
      "Papyrus Actor.GetActorValuePercentage: wrong argument count");
  }

  if (auto actor = GetFormPtr<MpActor>(self)) {
    espm::ActorValue attrID =
      ConvertToAV(static_cast<const char*>(arguments[0]));

    auto form = actor->GetChangeForm();
    if (attrID == espm::ActorValue::Health) {
      return VarValue(form.actorValues.healthPercentage);
    } else if (attrID == espm::ActorValue::Stamina) {
      return VarValue(form.actorValues.staminaPercentage);
    } else if (attrID == espm::ActorValue::Magicka) {
      return VarValue(form.actorValues.magickaPercentage);
    } else {
      return VarValue(0.0f);
    }
  }
  return VarValue(0.0f);
}

VarValue PapyrusActor::SetAlpha(VarValue self,
                                const std::vector<VarValue>& arguments)
{
  if (auto selfRefr = GetFormPtr<MpActor>(self)) {
    if (arguments.size() < 1) {
      throw std::runtime_error("SetAlpha requires at least one argument");
    }
    // TODO: Make normal sync for this. For now using workaround to inform
    // neigbours by sending papyrus functions to them.
    auto funcName = "SetAlpha";
    auto serializedArgs = SpSnippetFunctionGen::SerializeArguments(
      arguments, selfRefr->GetParent());
    for (auto listener : selfRefr->GetActorListeners()) {
      SpSnippet(GetName(), funcName, serializedArgs, selfRefr->GetFormId())
        .Execute(listener, SpSnippetMode::kNoReturnResult);
    }
  }
  return VarValue::None();
}

namespace {
bool ValidateItemEquipability(const char* papyrusMethodName,
                              WorldState* worldState,
                              const espm::LookupResult& lookupRes)
{
  if (!lookupRes.rec) {
    spdlog::error("{} - invalid form", papyrusMethodName);
    return false;
  }

  if (!espm::utils::IsItem(lookupRes.rec->GetType())) {
    spdlog::error("{} - form is not an item", papyrusMethodName);
    return false;
  }

  if (espm::utils::Is<espm::LIGH>(lookupRes.rec->GetType())) {
    auto res = espm::Convert<espm::LIGH>(lookupRes.rec)
                 ->GetData(worldState->GetEspmCache());
    bool isTorch = res.data.flags & espm::LIGH::Flags::CanBeCarried;
    if (!isTorch) {
      spdlog::error("{} - form is LIGH without CanBeCarried flag",
                    papyrusMethodName);
      return false;
    }
  }

  return true;
}

void AddItemIfNotPresent(MpActor* actor, const espm::LookupResult& lookupRes)
{
  // If no such item in inventory, add one (this is standard behavior)
  auto baseId = lookupRes.ToGlobalId(lookupRes.rec->GetId());
  if (actor->GetInventory().GetItemCount(baseId) == 0) {
    actor->AddItem(baseId, 1);
  }
}
}

VarValue PapyrusActor::EquipItem(VarValue self,
                                 const std::vector<VarValue>& arguments)
{
  if (auto actor = GetFormPtr<MpActor>(self)) {
    auto worldState = actor->GetParent();
    if (!worldState) {
      spdlog::error("EquipItem - no WorldState attached");
      return VarValue::None();
    }

    if (arguments.size() < 1) {
      spdlog::error("EquipItem - invalid argument count");
      return VarValue::None();
    }

    auto lookupRes = GetRecordPtr(arguments[0]);

    if (!ValidateItemEquipability("EquipItem", worldState, lookupRes)) {
      return VarValue::None();
    }

    AddItemIfNotPresent(actor, lookupRes);

    SpSnippet(
      GetName(), "EquipItem",
      SpSnippetFunctionGen::SerializeArguments(arguments, actor->GetParent()),
      actor->GetFormId())
      .Execute(actor, SpSnippetMode::kNoReturnResult);
  } else {
    spdlog::error("EquipItem - invalid actor");
  }
  return VarValue::None();
}

VarValue PapyrusActor::EquipItemEx(VarValue self,
                                   const std::vector<VarValue>& arguments)
{
  if (arguments.size() < 4) {
    spdlog::error("EquipItemEx requires at least 4 arguments");
    return VarValue::None();
  }

  auto actor = GetFormPtr<MpActor>(self);
  if (!actor) {
    spdlog::error("EquipItemEx - invalid actor");
    return VarValue::None();
  }

  auto worldState = actor->GetParent();
  if (!worldState) {
    spdlog::error("EquipItemEx - no WorldState attached");
    return VarValue::None();
  }

  auto lookupRes = GetRecordPtr(arguments[0]);

  if (!ValidateItemEquipability("EquipItemEx", worldState, lookupRes)) {
    return VarValue::None();
  }

  AddItemIfNotPresent(actor, lookupRes);

  SpSnippet(
    GetName(), "EquipItemEx",
    SpSnippetFunctionGen::SerializeArguments(arguments, actor->GetParent()),
    actor->GetFormId())
    .Execute(actor, SpSnippetMode::kNoReturnResult);
  return VarValue::None();
}

VarValue PapyrusActor::EquipSpell(VarValue self,
                                  const std::vector<VarValue>& arguments)
{
  if (arguments.size() < 2) {
    spdlog::error("EquipSpell requires at least 2 arguments");
    return VarValue::None();
  }

  auto lookupRes = GetRecordPtr(arguments[0]);
  if (!lookupRes.rec) {
    spdlog::error("EquipSpell - invalid form");
    return VarValue::None();
  }

  if (lookupRes.rec->GetType() != espm::SPEL::kType) {
    spdlog::error("EquipSpell - form is not a spell");
    return VarValue::None();
  }

  if (auto actor = GetFormPtr<MpActor>(self)) {
    // If no such spell in spell list, add one (this is standard behavior)
    auto baseId = lookupRes.ToGlobalId(lookupRes.rec->GetId());
    if (!actor->IsSpellLearned(baseId)) {
      actor->AddSpell(baseId);
    }

    SpSnippet(
      GetName(), "EquipSpell",
      SpSnippetFunctionGen::SerializeArguments(arguments, actor->GetParent()),
      actor->GetFormId())
      .Execute(actor, SpSnippetMode::kNoReturnResult);
  }

  return VarValue::None();
}

VarValue PapyrusActor::UnequipItem(VarValue self,
                                   const std::vector<VarValue>& arguments)
{
  if (arguments.size() < 3) {
    spdlog::error("UnequipItem requires at least 3 arguments");
    return VarValue::None();
  }

  if (auto actor = GetFormPtr<MpActor>(self)) {
    SpSnippet(
      GetName(), "UnequipItem",
      SpSnippetFunctionGen::SerializeArguments(arguments, actor->GetParent()),
      actor->GetFormId())
      .Execute(actor, SpSnippetMode::kNoReturnResult);
  }
  return VarValue::None();
}

VarValue PapyrusActor::SetDontMove(VarValue self,
                                   const std::vector<VarValue>& arguments)
{
  if (auto actor = GetFormPtr<MpActor>(self)) {
    if (arguments.size() < 1) {
      throw std::runtime_error("SetDontMove requires at least one argument");
    }
    SpSnippet(
      GetName(), "SetDontMove",
      SpSnippetFunctionGen::SerializeArguments(arguments, actor->GetParent()),
      actor->GetFormId())
      .Execute(actor, SpSnippetMode::kNoReturnResult);
  }
  return VarValue::None();
}

VarValue PapyrusActor::IsDead(
  VarValue self, const std::vector<VarValue>& arguments) const noexcept
{
  if (auto _this = GetFormPtr<MpActor>(self)) {
    return VarValue(_this->IsDead());
  }
  return VarValue::None();
}

VarValue PapyrusActor::WornHasKeyword(VarValue self,
                                      const std::vector<VarValue>& arguments)
{
  if (auto actor = GetFormPtr<MpActor>(self)) {
    if (arguments.size() < 1) {
      throw std::runtime_error(
        "Actor.WornHasKeyword requires at least one argument");
    }

    const auto& keywordRec = GetRecordPtr(arguments[0]);
    if (!keywordRec.rec) {
      spdlog::error("Actor.WornHasKeyword - invalid keyword form");
      return VarValue(false);
    }

    const std::vector<Inventory::Entry>& entries =
      actor->GetEquipment().inv.entries;
    WorldState* worldState = actor->GetParent();
    for (const auto& entry : entries) {
      if (entry.GetWorn() != Inventory::Worn::None) {
        const espm::LookupResult res =
          worldState->GetEspm().GetBrowser().LookupById(entry.baseId);
        if (!res.rec) {
          return VarValue::None();
        }
        const auto keywordIds =
          res.rec->GetKeywordIds(worldState->GetEspmCache());
        if (std::any_of(keywordIds.begin(), keywordIds.end(),
                        [&](uint32_t keywordId) {
                          return res.ToGlobalId(keywordId) ==
                            keywordRec.ToGlobalId(keywordRec.rec->GetId());
                        })) {
          return VarValue(true);
        }
      }
    }
  }
  return VarValue(false);
}

VarValue PapyrusActor::AddToFaction(VarValue self,
                                    const std::vector<VarValue>& arguments)
{
  if (auto actor = GetFormPtr<MpActor>(self)) {
    auto worldState = actor->GetParent();
    if (!worldState) {
      throw std::runtime_error("Actor.AddToFaction - no WorldState attached");
    }

    if (arguments.size() < 1) {
      throw std::runtime_error("Actor.AddToFaction requires one argument");
    }

    const auto& factionRec = GetRecordPtr(arguments[0]);
    if (!factionRec.rec) {
      spdlog::error("Actor.AddToFaction - invalid faction form");
      return VarValue();
    }

    Faction resultFaction = Faction();
    resultFaction.formDesc = FormDesc::FromFormId(
      factionRec.ToGlobalId(factionRec.rec->GetId()), worldState->espmFiles);
    resultFaction.rank = 0;

    actor->AddToFaction(resultFaction);
  }
  return VarValue();
}

VarValue PapyrusActor::IsInFaction(VarValue self,
                                   const std::vector<VarValue>& arguments)
{
  if (auto actor = GetFormPtr<MpActor>(self)) {
    auto worldState = actor->GetParent();
    if (!worldState) {
      throw std::runtime_error("Actor.IsInFaction - no WorldState attached");
    }

    if (arguments.size() < 1) {
      throw std::runtime_error("Actor.IsInFaction requires one argument");
    }

    const auto& factionRec = GetRecordPtr(arguments[0]);
    if (!factionRec.rec) {
      spdlog::error("Actor.IsInFaction - invalid faction form");
      return VarValue(false);
    }

    return VarValue(actor->IsInFaction(FormDesc::FromFormId(
      factionRec.ToGlobalId(factionRec.rec->GetId()), worldState->espmFiles)));
  }
  return VarValue(false);
}

VarValue PapyrusActor::GetFactions(VarValue self,
                                   const std::vector<VarValue>& arguments)
{
  VarValue result = VarValue((uint8_t)VarValue::kType_ObjectArray);
  result.pArray = std::make_shared<std::vector<VarValue>>();

  if (auto actor = GetFormPtr<MpActor>(self)) {
    auto worldState = actor->GetParent();
    if (!worldState) {
      throw std::runtime_error("Actor.GetFactions - no WorldState attached");
    }

    if (arguments.size() < 2) {
      throw std::runtime_error("Actor.GetFactions requires two arguments");
    }

    auto minFactionRank = static_cast<int>(arguments[0]);
    auto maxFactionRank = static_cast<int>(arguments[1]);

    auto factions = actor->GetFactions(minFactionRank, maxFactionRank);
    for (auto faction : factions) {
      result.pArray->push_back(VarValue(std::make_shared<EspmGameObject>(
        worldState->GetEspm().GetBrowser().LookupById(
          faction.formDesc.ToFormId(worldState->espmFiles)))));
    }
  }
  return result;
}

VarValue PapyrusActor::RemoveFromFaction(
  VarValue self, const std::vector<VarValue>& arguments)
{
  if (auto actor = GetFormPtr<MpActor>(self)) {
    auto worldState = actor->GetParent();
    if (!worldState) {
      throw std::runtime_error(
        "Actor.RemoveFromFaction - no WorldState attached");
    }

    if (arguments.size() < 1) {
      throw std::runtime_error(
        "Actor.RemoveFromFaction requires one argument");
    }

    const auto& factionRec = GetRecordPtr(arguments[0]);
    if (!factionRec.rec) {
      spdlog::error("Actor.RemoveFromFaction - invalid faction form");
      return VarValue();
    }

    const auto& factions = actor->GetChangeForm().factions;

    if (!factions.has_value()) {
      return VarValue();
    }

    actor->RemoveFromFaction(FormDesc::FromFormId(
      factionRec.ToGlobalId(factionRec.rec->GetId()), worldState->espmFiles));
  }
  return VarValue();
}

VarValue PapyrusActor::AddSpell(VarValue self,
                                const std::vector<VarValue>& arguments)
{
  // TODO: should we sync spell list in general? should we show spell add for
  // actor neighbors?

  if (auto actor = GetFormPtr<MpActor>(self)) {
    if (arguments.size() < 2) {
      throw std::runtime_error(
        "Actor.AddSpell requires at least two arguments");
    }

    const auto& spell = GetRecordPtr(arguments[0]);
    if (!spell.rec) {
      spdlog::error("Actor.AddSpell - invalid spell form");
      return VarValue(false);
    }

    if (spell.rec->GetType().ToString() != "SPEL") {
      spdlog::error("Actor.AddSpell - type expected to be SPEL, but it is {}",
                    spell.rec->GetType().ToString());
      return VarValue(false);
    }

    uint32_t spellId = spell.ToGlobalId(spell.rec->GetId());

    if (!actor->IsSpellLearned(spellId)) {
      actor->AddSpell(spellId);

      SpSnippet(GetName(), "AddSpell",
                SpSnippetFunctionGen::SerializeArguments(arguments,
                                                         actor->GetParent()),
                actor->GetFormId())
        .Execute(actor, SpSnippetMode::kNoReturnResult);

      return VarValue(true);
    }
  }

  return VarValue(false);
}

VarValue PapyrusActor::RemoveSpell(VarValue self,
                                   const std::vector<VarValue>& arguments)
{
  if (auto actor = GetFormPtr<MpActor>(self)) {
    if (arguments.size() < 1) {
      throw std::runtime_error(
        "Actor.RemoveSpell requires at least one argument");
    }

    const auto& spell = GetRecordPtr(arguments[0]);
    if (!spell.rec) {
      spdlog::error("Actor.RemoveSpell - invalid spell form");
      return VarValue(false);
    }

    if (spell.rec->GetType().ToString() != "SPEL") {
      spdlog::error(
        "Actor.RemoveSpell - type expected to be SPEL, but it is {}",
        spell.rec->GetType().ToString());
      return VarValue(false);
    }

    uint32_t spellId = spell.ToGlobalId(spell.rec->GetId());

    if (actor->IsSpellLearnedFromBase(spellId)) {
      spdlog::warn("Actor.RemoveSpell - can't remove spells inherited from "
                   "RACE/NPC_ records");
    } else if (actor->IsSpellLearned(spellId)) {
      actor->RemoveSpell(spellId);

      SpSnippet(GetName(), "RemoveSpell",
                SpSnippetFunctionGen::SerializeArguments(arguments,
                                                         actor->GetParent()),
                actor->GetFormId())
        .Execute(actor, SpSnippetMode::kNoReturnResult);

      return VarValue(true);
    }
  }
  return VarValue(false);
}

VarValue PapyrusActor::GetRace(VarValue self,
                               const std::vector<VarValue>& arguments)
{
  auto actor = GetFormPtr<MpActor>(self);
  if (!actor) {
    return VarValue::None();
  }

  uint32_t raceId = 0;

  if (auto appearance = actor->GetAppearance()) {
    raceId = appearance->raceId;
  } else {
    raceId = EvaluateTemplate<espm::NPC_::UseTraits>(
      actor->GetParent(), actor->GetBaseId(), actor->GetTemplateChain(),
      [](const auto& npcLookupResult, const auto& npcData) {
        return npcLookupResult.ToGlobalId(npcData.race);
      });
  }

  auto lookupRes =
    actor->GetParent()->GetEspm().GetBrowser().LookupById(raceId);

  if (!lookupRes.rec) {
    spdlog::error("Actor.GetRace - Race with id {:x} not found in espm",
                  raceId);
    return VarValue::None();
  }

  if (!(lookupRes.rec->GetType() == espm::RACE::kType)) {
    spdlog::error(
      "Actor.GetRace - Expected record {:x} to be RACE, but it is {}", raceId,
      lookupRes.rec->GetType().ToString());
    return VarValue::None();
  }

  return VarValue(std::make_shared<EspmGameObject>(lookupRes));
}

VarValue PapyrusActor::GetSpellCount(VarValue self,
                                     const std::vector<VarValue>& arguments)
{
  auto actor = GetFormPtr<MpActor>(self);
  if (!actor) {
    return VarValue(0);
  }

  std::vector<uint32_t> spellList = actor->GetSpellList();

  int countLearnedDuringGameplay = 0;
  for (auto spellId : spellList) {
    if (!actor->IsSpellLearnedFromBase(spellId)) {
      countLearnedDuringGameplay++;
    }
  }

  return VarValue(countLearnedDuringGameplay);
}

VarValue PapyrusActor::GetNthSpell(VarValue self,
                                   const std::vector<VarValue>& arguments)
{
  auto actor = GetFormPtr<MpActor>(self);
  if (!actor) {
    return VarValue::None();
  }

  if (arguments.empty()) {
    spdlog::error("GetNthSpell - expected at least 1 argument");
    return VarValue::None();
  }
  int n = static_cast<int>(arguments[0]);
  if (n < 0) {
    return VarValue::None();
  }

  std::vector<uint32_t> spellList = actor->GetSpellList();

  std::vector<uint32_t> spellsLearnedDuringGameplay;
  spellsLearnedDuringGameplay.reserve(spellList.size());
  for (auto spellId : spellList) {
    if (!actor->IsSpellLearnedFromBase(spellId)) {
      spellsLearnedDuringGameplay.push_back(spellId);
    }
  }

  if (n >= static_cast<int>(spellsLearnedDuringGameplay.size())) {
    return VarValue::None();
  }

  uint32_t spellId = spellsLearnedDuringGameplay[n];

  auto lookupRes =
    actor->GetParent()->GetEspm().GetBrowser().LookupById(spellId);

  if (!lookupRes.rec) {
    spdlog::error("GetNthSpell - Spell with id {:x} not found in espm",
                  spellId);
    return VarValue::None();
  }

  if (!(lookupRes.rec->GetType() == espm::SPEL::kType)) {
    spdlog::error(
      "GetNthSpell - Expected record {:x} to be SPEL, but it is {}", spellId,
      lookupRes.rec->GetType().ToString());
    return VarValue::None();
  }

  return VarValue(std::make_shared<EspmGameObject>(lookupRes));
}

void PapyrusActor::Register(
  VirtualMachine& vm, std::shared_ptr<IPapyrusCompatibilityPolicy> policy)
{
  compatibilityPolicy = policy;

  AddMethod(vm, "IsWeaponDrawn", &PapyrusActor::IsWeaponDrawn);
  AddMethod(vm, "DrawWeapon", &PapyrusActor::DrawWeapon);
  AddMethod(vm, "UnequipAll", &PapyrusActor::UnequipAll);
  AddMethod(vm, "PlayIdle", &PapyrusActor::PlayIdle);
  AddMethod(vm, "GetSitState", &PapyrusActor::GetSitState);
  AddMethod(vm, "RestoreActorValue", &PapyrusActor::RestoreActorValue);
  AddMethod(vm, "SetActorValue", &PapyrusActor::SetActorValue);
  AddMethod(vm, "DamageActorValue", &PapyrusActor::DamageActorValue);
  AddMethod(vm, "GetActorValue", &PapyrusActor::GetActorValue);
  AddMethod(vm, "GetBaseActorValue", &PapyrusActor::GetBaseActorValue);
  AddMethod(vm, "GetActorValueMax", &PapyrusActor::GetActorValueMax);
  AddMethod(vm, "ModActorValue", &PapyrusActor::ModActorValue);
  AddMethod(vm, "ForceActorValue", &PapyrusActor::ForceActorValue);
  AddMethod(vm, "IsEquipped", &PapyrusActor::IsEquipped);
  AddMethod(vm, "GetActorValuePercentage",
            &PapyrusActor::GetActorValuePercentage);
  AddMethod(vm, "SetAlpha", &PapyrusActor::SetAlpha);
  AddMethod(vm, "EquipItem", &PapyrusActor::EquipItem);
  AddMethod(vm, "EquipItemEx", &PapyrusActor::EquipItemEx);
  AddMethod(vm, "EquipSpell", &PapyrusActor::EquipSpell);
  AddMethod(vm, "UnequipItem", &PapyrusActor::UnequipItem);
  AddMethod(vm, "SetDontMove", &PapyrusActor::SetDontMove);
  AddMethod(vm, "IsDead", &PapyrusActor::IsDead);
  AddMethod(vm, "WornHasKeyword", &PapyrusActor::WornHasKeyword);
  AddMethod(vm, "AddToFaction", &PapyrusActor::AddToFaction);
  AddMethod(vm, "IsInFaction", &PapyrusActor::IsInFaction);
  AddMethod(vm, "GetFactions", &PapyrusActor::GetFactions);
  AddMethod(vm, "RemoveFromFaction", &PapyrusActor::RemoveFromFaction);
  AddMethod(vm, "AddSpell", &PapyrusActor::AddSpell);
  AddMethod(vm, "RemoveSpell", &PapyrusActor::RemoveSpell);
  AddMethod(vm, "GetRace", &PapyrusActor::GetRace);
  AddMethod(vm, "GetSpellCount", &PapyrusActor::GetSpellCount);
  AddMethod(vm, "GetNthSpell", &PapyrusActor::GetNthSpell);
}
