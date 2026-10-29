#include "MpChangeForms.h"
#include "JsonUtils.h"
#include <spdlog/spdlog.h>

namespace {
std::vector<std::string> ToStringArray(const std::vector<FormDesc>& formDescs)
{
  std::vector<std::string> res(formDescs.size());
  std::transform(formDescs.begin(), formDescs.end(), res.begin(),
                 [](const FormDesc& v) { return v.ToString(); });
  return res;
}

std::vector<FormDesc> ToFormDescsArray(const std::vector<std::string>& strings)
{
  std::vector<FormDesc> res(strings.size());
  std::transform(strings.begin(), strings.end(), res.begin(),
                 [](const std::string& v) { return FormDesc::FromString(v); });
  return res;
}

// The record's forms as "<hex>:<plugin>" (thuum docs/DECISIONS.md ADR-043;
// docs/research/formdesc-world-map.md section 6, shape b). The twelve raw
// fields are converted here, at the record boundary, so the wire and every
// runtime API keep their uint32: the inventory entries' baseId,
// enchantmentId and poisonId (under inv and under equipmentDump.inv), the
// equipment's four spell slots and learnedSpells in place; the appearance's
// race, head parts and texture set as the sibling `appearanceForms`, since
// appearanceDump is an Appearance's JSON as text, which MpActor and the
// legacy readers parse by the numbers: the dump keeps the numbers of the
// order it was saved under, the sibling carries the plugin-relative truth and
// overrides them on read.

// "<hex>:<plugin>" for a runtime id under an order; nullopt for an id no file
// of the order holds, which a record should not carry (the caller logs it)
std::optional<std::string> Describe(uint32_t formId, const EspmFileList& files)
{
  try {
    return FormDesc::FromFormId(formId, files).ToString();
  } catch (std::exception&) {
    return std::nullopt;
  }
}

// The runtime id of a stored form under an order; nullopt when its plugin is
// not in the order
std::optional<uint32_t> Resolve(const FormDesc& form,
                                const EspmFileList& files)
{
  try {
    return form.ToFormId(files);
  } catch (std::exception&) {
    return std::nullopt;
  }
}

const UnresolvedForm* FindUnresolved(
  const std::vector<UnresolvedForm>& unresolved, const char* field)
{
  for (const auto& entry : unresolved) {
    if (entry.field == field) {
      return &entry;
    }
  }
  return nullptr;
}

constexpr const char* kEntryForms[] = { "baseId", "enchantmentId",
                                        "poisonId" };
constexpr const char* kSpellSlots[] = { "leftSpell", "rightSpell",
                                        "voiceSpell", "instantSpell" };

// An inventory's JSON (Inventory::ToJson's shape) with its entries' forms
// described. An entry a file of the order does not hold is dropped with an
// error (a raw id in a record is a bug under ADR-043); the entries parked
// under `field` are written back after the live ones.
void DescribeInventory(nlohmann::json& inv, const EspmFileList& files,
                       const std::vector<UnresolvedForm>& unresolved,
                       const char* field, const std::string& record)
{
  auto kept = nlohmann::json::array();
  auto entries = inv.find("entries");
  if (entries != inv.end() && entries->is_array()) {
    for (auto& entry : *entries) {
      bool described = true;
      for (const char* key : kEntryForms) {
        auto value = entry.find(key);
        if (value == entry.end() || !value->is_number()) {
          continue;
        }
        const uint32_t formId = value->get<uint32_t>();
        const auto desc = Describe(formId, files);
        if (!desc) {
          spdlog::error("MpChangeForm::ToJson {} - {} entry's {} {:#x} "
                        "belongs to no loaded plugin, dropping the entry",
                        record, field, key, formId);
          described = false;
          break;
        }
        *value = *desc;
      }
      if (described) {
        kept.push_back(std::move(entry));
      }
    }
  }
  for (const auto& parked : unresolved) {
    if (parked.field == field) {
      kept.push_back(nlohmann::json::parse(parked.entry));
    }
  }
  inv["entries"] = std::move(kept);
}

// The inverse: a stored inventory's entries resolved under the running
// order. An entry whose base, enchantment or poison names a plugin the order
// lacks is parked whole under `field`, as written.
void ResolveInventory(nlohmann::json& inv, const EspmFileList& files,
                      std::vector<UnresolvedForm>& unresolved,
                      const char* field)
{
  auto entries = inv.find("entries");
  if (entries == inv.end() || !entries->is_array()) {
    return;
  }
  auto kept = nlohmann::json::array();
  for (const auto& entry : *entries) {
    nlohmann::json resolved = entry;
    std::optional<FormDesc> missing;
    for (const char* key : kEntryForms) {
      auto value = resolved.find(key);
      if (value == resolved.end() || !value->is_string()) {
        continue;
      }
      const FormDesc form = FormDesc::FromString(value->get<std::string>());
      const auto formId = Resolve(form, files);
      if (!formId) {
        missing = form;
        break;
      }
      *value = *formId;
    }
    if (missing) {
      unresolved.push_back(UnresolvedForm{ field, *missing, entry.dump() });
    } else {
      kept.push_back(std::move(resolved));
    }
  }
  *entries = std::move(kept);
}

// A scalar form field described: the runtime value when it holds one, else
// the form parked under `field` if any; absent (an empty spell slot) or
// unplaceable, the key is left out
void DescribeScalar(nlohmann::json& object, const char* field,
                    std::optional<uint32_t> value, const EspmFileList& files,
                    const std::vector<UnresolvedForm>& unresolved,
                    const std::string& record)
{
  if (!value || *value == 0) {
    if (const auto* parked = FindUnresolved(unresolved, field)) {
      object[field] = parked->form.ToString();
      return;
    }
    if (!value) {
      object.erase(field);
      return;
    }
  }
  if (const auto desc = Describe(*value, files)) {
    object[field] = *desc;
    return;
  }
  spdlog::error("MpChangeForm::ToJson {} - {} {:#x} belongs to no loaded "
                "plugin, writing none",
                record, field, *value);
  object.erase(field);
}

// A stored scalar form under the running order: its runtime id, or 0 with
// the form parked under `field`
uint32_t ResolveScalar(const nlohmann::json& object, const char* field,
                       const EspmFileList& files,
                       std::vector<UnresolvedForm>& unresolved)
{
  auto it = object.find(field);
  if (it == object.end() || !it->is_string()) {
    return 0;
  }
  const FormDesc form = FormDesc::FromString(it->get<std::string>());
  if (const auto formId = Resolve(form, files)) {
    return *formId;
  }
  unresolved.push_back(UnresolvedForm{ field, form, "" });
  return 0;
}

void DescribeAppearance(nlohmann::json& res, const MpChangeForm& changeForm,
                        const EspmFileList& files, const std::string& record)
{
  if (changeForm.appearanceDump.empty()) {
    return;
  }
  const nlohmann::json dump =
    nlohmann::json::parse(changeForm.appearanceDump, nullptr, false);
  if (!dump.is_object()) {
    return;
  }

  auto forms = nlohmann::json::object();
  for (const char* field : { "raceId", "headTextureSetId" }) {
    std::optional<uint32_t> value;
    if (auto it = dump.find(field); it != dump.end() && it->is_number()) {
      value = it->get<uint32_t>();
    }
    if (value) {
      DescribeScalar(forms, field, value, files, changeForm.unresolved,
                     record);
    } else if (const auto* parked =
                 FindUnresolved(changeForm.unresolved, field)) {
      forms[field] = parked->form.ToString();
    }
  }

  auto parts = nlohmann::json::array();
  if (auto it = dump.find("headpartIds"); it != dump.end() && it->is_array()) {
    for (const auto& part : *it) {
      if (!part.is_number()) {
        continue;
      }
      const uint32_t formId = part.get<uint32_t>();
      if (const auto desc = Describe(formId, files)) {
        parts.push_back(*desc);
      } else {
        spdlog::error("MpChangeForm::ToJson {} - head part {:#x} belongs to "
                      "no loaded plugin, dropping it",
                      record, formId);
      }
    }
  }
  for (const auto& parked : changeForm.unresolved) {
    if (parked.field == "headpartIds") {
      parts.push_back(parked.form.ToString());
    }
  }
  forms["headpartIds"] = std::move(parts);
  res["appearanceForms"] = std::move(forms);
}

// appearanceForms resolved under the running order into the dump's numbers;
// the dump is rewritten only when a number moves, so a record read under the
// order it was saved in keeps its text byte for byte
void ResolveAppearance(MpChangeForm& res, const nlohmann::json& forms,
                       const EspmFileList& files)
{
  if (res.appearanceDump.empty() || !forms.is_object()) {
    return;
  }
  nlohmann::json dump =
    nlohmann::json::parse(res.appearanceDump, nullptr, false);
  if (!dump.is_object()) {
    return;
  }
  const nlohmann::json before = dump;

  dump["raceId"] = ResolveScalar(forms, "raceId", files, res.unresolved);
  dump["headTextureSetId"] =
    ResolveScalar(forms, "headTextureSetId", files, res.unresolved);

  auto parts = nlohmann::json::array();
  if (auto it = forms.find("headpartIds");
      it != forms.end() && it->is_array()) {
    for (const auto& part : *it) {
      if (!part.is_string()) {
        continue;
      }
      const FormDesc form = FormDesc::FromString(part.get<std::string>());
      if (const auto formId = Resolve(form, files)) {
        parts.push_back(*formId);
      } else {
        res.unresolved.push_back(UnresolvedForm{ "headpartIds", form, "" });
      }
    }
  }
  dump["headpartIds"] = std::move(parts);

  if (dump != before) {
    res.appearanceDump = dump.dump();
  }
}

void DescribeLearnedSpells(nlohmann::json& res, const MpChangeForm& changeForm,
                           const EspmFileList& files,
                           const std::string& record)
{
  auto spells = nlohmann::json::array();
  for (const uint32_t formId : changeForm.learnedSpells.GetLearnedSpells()) {
    if (const auto desc = Describe(formId, files)) {
      spells.push_back(*desc);
    } else {
      spdlog::error("MpChangeForm::ToJson {} - learned spell {:#x} belongs "
                    "to no loaded plugin, dropping it",
                    record, formId);
    }
  }
  for (const auto& parked : changeForm.unresolved) {
    if (parked.field == "learnedSpells") {
      spells.push_back(parked.form.ToString());
    }
  }
  res["learnedSpells"] = std::move(spells);
}

// Every stored form of the written record as "<hex>:<plugin>" under `files`,
// and the version mark that says so
void DescribeForms(nlohmann::json& res, const MpChangeForm& changeForm,
                   const EspmFileList& files)
{
  const std::string record = changeForm.formDesc.ToString();
  res["formVersion"] = MpChangeForm::kFormVersion;

  DescribeInventory(res["inv"], files, changeForm.unresolved, "inv", record);

  nlohmann::json& equipment = res["equipmentDump"];
  DescribeInventory(equipment["inv"], files, changeForm.unresolved,
                    "equipment.inv", record);
  for (const char* slot : kSpellSlots) {
    std::optional<uint32_t> value;
    if (auto it = equipment.find(slot);
        it != equipment.end() && it->is_number()) {
      value = it->get<uint32_t>();
    }
    DescribeScalar(equipment, slot, value, files, changeForm.unresolved,
                   record);
  }

  DescribeLearnedSpells(res, changeForm, files, record);
  DescribeAppearance(res, changeForm, files, record);
}

// The stored equipment resolved under the running order: its inventory and
// its four spell slots; a slot naming a plugin the order lacks is parked and
// left empty
void ResolveEquipment(nlohmann::json& equipment, const EspmFileList& files,
                      std::vector<UnresolvedForm>& unresolved)
{
  if (auto it = equipment.find("inv");
      it != equipment.end() && it->is_object()) {
    ResolveInventory(*it, files, unresolved, "equipment.inv");
  }
  for (const char* slot : kSpellSlots) {
    auto it = equipment.find(slot);
    if (it == equipment.end() || !it->is_string()) {
      continue;
    }
    const FormDesc form = FormDesc::FromString(it->get<std::string>());
    if (const auto formId = Resolve(form, files)) {
      *it = *formId;
    } else {
      unresolved.push_back(UnresolvedForm{ slot, form, "" });
      equipment.erase(it);
    }
  }
}
}

nlohmann::json MpChangeForm::ToJson(const MpChangeForm& changeForm,
                                    const EspmFileList* files)
{
  auto res = nlohmann::json::object();
  res["recType"] = static_cast<int>(changeForm.recType);
  res["formDesc"] = changeForm.formDesc.ToString();
  res["baseDesc"] = changeForm.baseDesc.ToString();
  res["position"] = { changeForm.position[0], changeForm.position[1],
                      changeForm.position[2] };
  res["angle"] = { changeForm.angle[0], changeForm.angle[1],
                   changeForm.angle[2] };
  res["worldOrCellDesc"] = changeForm.worldOrCellDesc.ToString();
  res["inv"] = changeForm.inv.ToJson();
  res["isHarvested"] = changeForm.isHarvested;
  res["isOpen"] = changeForm.isOpen;
  res["baseContainerAdded"] = changeForm.baseContainerAdded;
  res["nextRelootDatetime"] = changeForm.nextRelootDatetime;
  res["isDisabled"] = changeForm.isDisabled;
  res["profileId"] = changeForm.profileId;
  res["isDeleted"] = changeForm.isDeleted;
  res["count"] = changeForm.count;
  res["isRaceMenuOpen"] = changeForm.isRaceMenuOpen;
  res["dynamicFields"] = changeForm.dynamicFields.GetAsJson();

  if (changeForm.appearanceDump.empty()) {
    res["appearanceDump"] = nullptr;
  } else {
    res["appearanceDump"] = nlohmann::json::parse(changeForm.appearanceDump);
  }

  res["equipmentDump"] = changeForm.equipment.ToJson();

  res["learnedSpells"] = changeForm.learnedSpells.GetLearnedSpells();

  res["healthPercentage"] = changeForm.actorValues.healthPercentage;
  res["magickaPercentage"] = changeForm.actorValues.magickaPercentage;
  res["staminaPercentage"] = changeForm.actorValues.staminaPercentage;

  res["healthRespawnPercentage"] = changeForm.healthRespawnPercentage;
  res["magickaRespawnPercentage"] = changeForm.magickaRespawnPercentage;
  res["staminaRespawnPercentage"] = changeForm.staminaRespawnPercentage;

  res["isDead"] = changeForm.isDead;

  res["consoleCommandsAllowed"] = changeForm.consoleCommandsAllowed;

  res["spawnPoint_pos"] = { changeForm.spawnPoint.pos[0],
                            changeForm.spawnPoint.pos[1],
                            changeForm.spawnPoint.pos[2] };
  res["spawnPoint_rot"] = { changeForm.spawnPoint.rot[0],
                            changeForm.spawnPoint.rot[1],
                            changeForm.spawnPoint.rot[2] };
  res["spawnPoint_cellOrWorldDesc"] =
    changeForm.spawnPoint.cellOrWorldDesc.ToString();

  res["spawnDelay"] = changeForm.spawnDelay;
  res["effects"] = changeForm.activeMagicEffects.ToJson();

  // thuum docs/verbs/npc-fate.md (ADR-049); absent in older records
  if (changeForm.fateLevel.has_value()) {
    res["fateLevel"] = *changeForm.fateLevel;
  }
  if (changeForm.fateDiedAt.has_value()) {
    res["fateDiedAt"] = *changeForm.fateDiedAt;
  }

  if (!changeForm.templateChain.empty()) {
    res["templateChain"] = ToStringArray(changeForm.templateChain);
  }

  // TODO: uncomment when add script vars save feature
  // if (changeForm.lastAnimation.has_value()) {
  //   res["lastAnimation"] = *changeForm.lastAnimation;
  // }

  if (changeForm.setNodeTextureSet.has_value()) {
    res["setNodeTextureSet"] = *changeForm.setNodeTextureSet;
  }

  if (changeForm.setNodeScale.has_value()) {
    res["setNodeScale"] = *changeForm.setNodeScale;
  }

  if (changeForm.displayName.has_value()) {
    res["displayName"] = *changeForm.displayName;
  }

  if (changeForm.factions.has_value() &&
      !changeForm.factions.value().empty()) {
    auto factionsJson = nlohmann::json::array();
    for (int i = 0; i < static_cast<int>(changeForm.factions.value().size());
         ++i) {
      nlohmann::json obj = {
        { "formDesc", changeForm.factions.value()[i].formDesc.ToString() },
        { "rank", (uint32_t)changeForm.factions.value()[i].rank }
      };
      factionsJson.push_back(obj);
    }
    res["factions"] = { { "entries", factionsJson } };
  }

  // thuum docs/verbs/map-markers.md; absent in older records, read as none
  if (changeForm.mapMarkers.has_value() && !changeForm.mapMarkers->empty()) {
    auto mapMarkersJson = nlohmann::json::array();
    for (const auto& marker : *changeForm.mapMarkers) {
      mapMarkersJson.push_back({ { "formDesc", marker.refr.ToString() },
                                 { "canTravel", marker.canTravel } });
    }
    res["mapMarkers"] = { { "entries", mapMarkersJson } };
  }

  // thuum docs/verbs/learned-effects.md; absent in older records, read as
  // none
  if (changeForm.ingredientEffects.has_value() &&
      !changeForm.ingredientEffects->empty()) {
    auto effectsJson = nlohmann::json::array();
    for (const auto& entry : *changeForm.ingredientEffects) {
      effectsJson.push_back({ { "formDesc", entry.ingredient.ToString() },
                              { "mask", static_cast<uint32_t>(entry.mask) } });
    }
    res["ingredientEffects"] = { { "entries", effectsJson } };
  }

  // thuum docs/verbs/player-globals.md; absent in older records, read as
  // none
  if (changeForm.playerGlobals.has_value() &&
      !changeForm.playerGlobals->empty()) {
    auto globalsJson = nlohmann::json::array();
    for (const auto& entry : *changeForm.playerGlobals) {
      globalsJson.push_back(
        { { "formDesc", entry.global.ToString() }, { "value", entry.value } });
    }
    res["playerGlobals"] = { { "entries", globalsJson } };
  }

  // thuum docs/verbs/quest-stage.md (ADR-056); absent in older records, read
  // as none
  if (changeForm.questStages.has_value() && !changeForm.questStages->empty()) {
    auto stagesJson = nlohmann::json::array();
    for (const auto& entry : *changeForm.questStages) {
      stagesJson.push_back(
        { { "quest", entry.quest.ToString() },
          { "stage", static_cast<uint32_t>(entry.stage) } });
    }
    res["questStages"] = { { "entries", stagesJson } };
  }

  // thuum docs/verbs/spell-placed-object.md (ADR-057); absent in older
  // records, read as none
  if (changeForm.placedBySpell.has_value()) {
    res["placedBySpell"] = changeForm.placedBySpell->ToString();
  }

  // thuum docs/verbs/shouts.md; absent in older records, read as none
  if (changeForm.shoutRecord.has_value() && !changeForm.shoutRecord->Empty()) {
    res["shoutRecord"] = {
      { "shouts", ToStringArray(changeForm.shoutRecord->shouts) },
      { "wordsKnown", ToStringArray(changeForm.shoutRecord->wordsKnown) },
      { "wordsUnlocked", ToStringArray(changeForm.shoutRecord->wordsUnlocked) }
    };
  }

  // thuum docs/verbs/magic-effects.md; absent in older records, read as
  // none. A caster is written only when there is one: an empty FormDesc
  // would read back as 0xff000000
  if (changeForm.runningEffects.has_value() &&
      !changeForm.runningEffects->empty()) {
    auto entries = nlohmann::json::array();
    for (const auto& e : *changeForm.runningEffects) {
      nlohmann::json j = { { "effect", e.effect.ToString() },
                           { "source", e.source.ToString() },
                           { "kind", e.kind },
                           { "av", e.av },
                           { "secondAv", e.secondAv },
                           { "secondWeight", e.secondWeight },
                           { "magnitude", e.magnitude },
                           { "durationS", e.durationS },
                           { "elapsedS", e.elapsedS },
                           { "asOf", e.asOfMs },
                           { "recover", e.recover },
                           { "detrimental", e.detrimental },
                           { "noDuration", e.noDuration } };
      if (e.caster.shortFormId != 0 || !e.caster.file.empty()) {
        j["caster"] = e.caster.ToString();
      }
      entries.push_back(j);
    }
    res["runningEffects"] = { { "entries", entries } };
  }

  // thuum docs/verbs/favorites.md; absent in older records, read as none
  if (changeForm.favorites.has_value() && !changeForm.favorites->empty()) {
    auto favoritesJson = nlohmann::json::array();
    for (const auto& entry : *changeForm.favorites) {
      favoritesJson.push_back(
        { { "formDesc", entry.form.ToString() },
          { "hotkey", static_cast<int>(entry.hotkey) } });
    }
    res["favorites"] = { { "entries", favoritesJson } };
  }

  // thuum docs/verbs/console-commands.md; absent in older records
  if (changeForm.staffRank.has_value()) {
    res["staffRank"] = *changeForm.staffRank;
  }

  // thuum docs/verbs/racemenu-sync.md; absent in older records, read as none
  if (changeForm.raceMenuPreset.has_value() &&
      !changeForm.raceMenuPreset->empty()) {
    res["raceMenuPreset"] = *changeForm.raceMenuPreset;
  }

  // thuum docs/verbs/actor-values.md; absent in older records, read as none
  if (changeForm.actorValueRecord.has_value()) {
    const auto& record = *changeForm.actorValueRecord;
    auto bases = nlohmann::json::array();
    for (const auto& [av, base] : record.bases) {
      bases.push_back({ { "av", av }, { "base", base } });
    }
    auto skills = nlohmann::json::array();
    for (const auto& skill : record.skills) {
      skills.push_back({ { "skill", skill.skill },
                         { "level", skill.level },
                         { "xp", skill.xp },
                         { "threshold", skill.threshold } });
    }
    auto legendary = nlohmann::json::array();
    for (const auto& [skill, count] : record.legendary) {
      legendary.push_back({ { "skill", skill }, { "count", count } });
    }
    res["actorValueRecord"] = {
      { "bases", bases },        { "skills", skills },
      { "xp", record.xp },       { "threshold", record.threshold },
      { "level", record.level }, { "legendary", legendary }
    };
  }

  // ADR-043: with the running order every stored form goes plugin-relative
  if (files) {
    DescribeForms(res, changeForm, *files);
  }

  return res;
}

MpChangeForm MpChangeForm::JsonToChangeForm(simdjson::dom::element& element,
                                            const EspmFileList* files,
                                            const EspmFileList* rawOrder)
{
  static const JsonPointer recType("recType");
  static const JsonPointer formVersion("formVersion");
  static const JsonPointer appearanceForms("appearanceForms");
  static const JsonPointer formDesc("formDesc");
  static const JsonPointer baseDesc("baseDesc");
  static const JsonPointer position("position");
  static const JsonPointer angle("angle");
  static const JsonPointer worldOrCellDesc("worldOrCellDesc");
  static const JsonPointer inv("inv");
  static const JsonPointer isHarvested("isHarvested");
  static const JsonPointer isOpen("isOpen");
  static const JsonPointer baseContainerAdded("baseContainerAdded");
  static const JsonPointer nextRelootDatetime("nextRelootDatetime");
  static const JsonPointer isDisabled("isDisabled");
  static const JsonPointer profileId("profileId");
  static const JsonPointer isDeleted("isDeleted");
  static const JsonPointer count("count");
  static const JsonPointer isRaceMenuOpen("isRaceMenuOpen");
  static const JsonPointer appearanceDump("appearanceDump");
  static const JsonPointer equipmentDump("equipmentDump");
  static const JsonPointer learnedSpells("learnedSpells");
  static const JsonPointer dynamicFields("dynamicFields");
  static const JsonPointer healthPercentage("healthPercentage");
  static const JsonPointer magickaPercentage("magickaPercentage");
  static const JsonPointer staminaPercentage("staminaPercentage");
  static const JsonPointer isDead("isDead");
  static const JsonPointer consoleCommandsAllowed("consoleCommandsAllowed");
  static const JsonPointer spawnPointPos("spawnPoint_pos");
  static const JsonPointer spawnPointRot("spawnPoint_rot");
  static const JsonPointer spawnPointCellOrWorldDesc(
    "spawnPoint_cellOrWorldDesc");
  static const JsonPointer spawnDelay("spawnDelay");
  static const JsonPointer effects("effects");
  static const JsonPointer templateChain("templateChain");
  static const JsonPointer fateLevel("fateLevel");
  static const JsonPointer fateDiedAt("fateDiedAt");
  static const JsonPointer lastAnimation("lastAnimation");
  static const JsonPointer setNodeTextureSet("setNodeTextureSet");
  static const JsonPointer setNodeScale("setNodeScale");
  static const JsonPointer displayName("displayName");
  static const JsonPointer factions("factions");
  static const JsonPointer mapMarkers("mapMarkers");
  static const JsonPointer ingredientEffects("ingredientEffects");
  static const JsonPointer shoutRecord("shoutRecord");
  static const JsonPointer playerGlobals("playerGlobals");
  static const JsonPointer questStages("questStages");
  static const JsonPointer placedBySpell("placedBySpell");
  static const JsonPointer favorites("favorites");
  static const JsonPointer raceMenuPreset("raceMenuPreset");
  static const JsonPointer staffRank("staffRank");
  static const JsonPointer runningEffects("runningEffects");
  static const JsonPointer actorValueRecord("actorValueRecord");
  static const JsonPointer healthRespawnPercentage("healthRespawnPercentage");
  static const JsonPointer magickaRespawnPercentage(
    "magickaRespawnPercentage");
  static const JsonPointer staminaRespawnPercentage(
    "staminaRespawnPercentage");

  MpChangeForm res;
  ReadEx(element, recType, &res.recType);

  const char* tmp;
  simdjson::dom::element jTmp;

  ReadEx(element, formDesc, &tmp);
  res.formDesc = FormDesc::FromString(tmp);

  ReadEx(element, baseDesc, &tmp);
  res.baseDesc = FormDesc::FromString(tmp);

  // ADR-043 decision 2: the version mark; a record without it is the raw
  // shape, written before the mark existed
  res.formVersion = 1;
  if (element.at_pointer(formVersion.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, formVersion, &res.formVersion);
  }
  const bool pluginRelative = res.formVersion >= kFormVersion;
  if (pluginRelative && !files) {
    throw std::runtime_error(
      fmt::format("{} is plugin-relative (formVersion {}) and needs the load "
                  "order to read",
                  res.formDesc.ToString(), res.formVersion));
  }

  ReadEx(element, position, &jTmp);
  for (int i = 0; i < 3; ++i) {
    ReadEx(jTmp, i, &res.position[i]);
  }

  ReadEx(element, angle, &jTmp);
  for (int i = 0; i < 3; ++i) {
    ReadEx(jTmp, i, &res.angle[i]);
  }

  ReadEx(element, worldOrCellDesc, &tmp);
  res.worldOrCellDesc = FormDesc::FromString(tmp);

  ReadEx(element, inv, &jTmp);
  if (pluginRelative) {
    nlohmann::json invJson =
      nlohmann::json::parse(static_cast<std::string>(simdjson::minify(jTmp)));
    ResolveInventory(invJson, *files, res.unresolved, "inv");
    res.inv = Inventory::FromJson(invJson);
  } else {
    res.inv = Inventory::FromJson(jTmp);
  }

  ReadEx(element, isHarvested, &res.isHarvested);
  ReadEx(element, isOpen, &res.isOpen);
  ReadEx(element, baseContainerAdded, &res.baseContainerAdded);
  ReadEx(element, nextRelootDatetime, &res.nextRelootDatetime);
  ReadEx(element, isDisabled, &res.isDisabled);
  ReadEx(element, profileId, &res.profileId);

  if (element.at_pointer(isDeleted.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, isDeleted, &res.isDeleted);
  }

  if (element.at_pointer(count.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, count, &res.count);
  }

  ReadEx(element, isRaceMenuOpen, &res.isRaceMenuOpen);

  ReadEx(element, appearanceDump, &jTmp);
  res.appearanceDump = simdjson::minify(jTmp);
  if (res.appearanceDump == "null") {
    res.appearanceDump.clear();
  }

  if (pluginRelative &&
      element.at_pointer(appearanceForms.GetData()).error() ==
        simdjson::error_code::SUCCESS) {
    ReadEx(element, appearanceForms, &jTmp);
    const nlohmann::json forms =
      nlohmann::json::parse(static_cast<std::string>(simdjson::minify(jTmp)));
    ResolveAppearance(res, forms, *files);
  }

  ReadEx(element, equipmentDump, &jTmp);
  std::string eqDump = simdjson::minify(jTmp);
  if (eqDump == "null") {
    eqDump.clear();
  } else {
    auto equipment = nlohmann::json::parse(eqDump);
    if (!equipment.contains("numChanges")) {
      equipment["numChanges"] = 0;
      eqDump = equipment.dump();
      spdlog::info("MpChangeForm::JsonToChangeForm {} - Missing 'numChanges' "
                   "key, setting to 0",
                   res.formDesc.ToString());
    }
  }

  if (eqDump.size() > 0) {
    nlohmann::json equipmentJson = nlohmann::json::parse(eqDump);
    if (pluginRelative) {
      ResolveEquipment(equipmentJson, *files, res.unresolved);
    }
    res.equipment = Equipment::FromJson(equipmentJson);
  } else {
    res.equipment = Equipment();
  }

  if (element.at_pointer(learnedSpells.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    if (pluginRelative) {
      std::vector<std::string> stored;
      ReadVector(element, learnedSpells, &stored);
      for (const auto& text : stored) {
        const FormDesc form = FormDesc::FromString(text);
        if (const auto spellId = Resolve(form, *files)) {
          res.learnedSpells.LearnSpell(*spellId);
        } else {
          res.unresolved.push_back(
            UnresolvedForm{ "learnedSpells", form, "" });
        }
      }
    } else {
      std::vector<LearnedSpells::Data::key_type> learnedSpellsData;

      ReadVector(element, learnedSpells, &learnedSpellsData);

      for (const auto spellId : learnedSpellsData) {
        res.learnedSpells.LearnSpell(spellId);
      }
    }
  }

  ReadEx(element, healthPercentage, &res.actorValues.healthPercentage);
  ReadEx(element, magickaPercentage, &res.actorValues.magickaPercentage);
  ReadEx(element, staminaPercentage, &res.actorValues.staminaPercentage);

  if (element.at_pointer(healthRespawnPercentage.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, healthRespawnPercentage, &res.healthRespawnPercentage);
  }

  if (element.at_pointer(magickaRespawnPercentage.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, magickaRespawnPercentage, &res.magickaRespawnPercentage);
  }

  if (element.at_pointer(staminaRespawnPercentage.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, staminaRespawnPercentage, &res.staminaRespawnPercentage);
  }

  ReadEx(element, isDead, &res.isDead);

  if (element.at_pointer(consoleCommandsAllowed.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, consoleCommandsAllowed, &res.consoleCommandsAllowed);
  }

  simdjson::dom::element jDynamicFields;
  ReadEx(element, dynamicFields, &jDynamicFields);
  res.dynamicFields = DynamicFields::FromJson(nlohmann::json::parse(
    static_cast<std::string>(simdjson::minify(jDynamicFields))));

  ReadEx(element, spawnPointPos, &jTmp);
  for (int i = 0; i < 3; ++i) {
    ReadEx(jTmp, i, &res.spawnPoint.pos[i]);
  }

  ReadEx(element, spawnPointRot, &jTmp);
  for (int i = 0; i < 3; ++i) {
    ReadEx(jTmp, i, &res.spawnPoint.rot[i]);
  }

  ReadEx(element, spawnPointCellOrWorldDesc, &tmp);
  res.spawnPoint.cellOrWorldDesc = FormDesc::FromString(tmp);

  ReadEx(element, spawnDelay, &res.spawnDelay);

  if (element.at_pointer(effects.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, effects, &jTmp);
    res.activeMagicEffects = ActiveMagicEffectsMap::FromJson(jTmp);
  }

  if (element.at_pointer(fateLevel.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    uint64_t level = 0;
    ReadEx(element, fateLevel, &level);
    res.fateLevel = static_cast<uint16_t>(std::min<uint64_t>(level, 65535));
  }

  if (element.at_pointer(fateDiedAt.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    float day = 0.f;
    ReadEx(element, fateDiedAt, &day);
    res.fateDiedAt = day;
  }

  if (element.at_pointer(templateChain.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    std::vector<std::string> data;
    ReadVector(element, templateChain, &data);
    res.templateChain = ToFormDescsArray(data);
  }

  if (element.at_pointer(lastAnimation.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    const char* tmp;
    ReadEx(element, lastAnimation, &tmp);
    res.lastAnimation = tmp;
  }

  if (element.at_pointer(setNodeTextureSet.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    simdjson::dom::element data;
    ReadEx(element, setNodeTextureSet, &data);

    if (res.setNodeTextureSet == std::nullopt) {
      res.setNodeTextureSet = std::map<std::string, std::string>();
    }

    for (auto [key, value] : data.get_object()) {
      std::string keyStr = key.data();
      std::string valueStr = value.get_string().value().data();
      res.setNodeTextureSet->emplace(keyStr, valueStr);
    }
  }

  if (element.at_pointer(setNodeScale.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    simdjson::dom::element data;
    ReadEx(element, setNodeScale, &data);

    if (res.setNodeScale == std::nullopt) {
      res.setNodeScale = std::map<std::string, float>();
    }

    for (auto [key, value] : data.get_object()) {
      std::string keyStr = key.data();
      float valueFloat = static_cast<float>(value.get_double().value());
      res.setNodeScale->emplace(keyStr, valueFloat);
    }
  }

  if (element.at_pointer(displayName.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    const char* tmp;
    ReadEx(element, displayName, &tmp);
    res.displayName = tmp;
  }

  if (element.at_pointer(factions.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, factions, &jTmp);
    static const JsonPointer entries("entries");

    std::vector<simdjson::dom::element> parsedEntries;
    ReadVector(jTmp, entries, &parsedEntries);

    std::vector<Faction> factions = std::vector<Faction>(parsedEntries.size());

    for (size_t i = 0; i != parsedEntries.size(); ++i) {
      auto& jEntry = parsedEntries[i];

      static JsonPointer rank("rank");
      Faction fact = Faction();
      const char* tmp;
      ReadEx(jEntry, formDesc, &tmp);
      fact.formDesc = FormDesc::FromString(tmp);

      uint32_t rankTemp = 0;
      ReadEx(jEntry, rank, &rankTemp);
      fact.rank = rankTemp;

      factions[i] = fact;
    }

    res.factions = factions;
  }

  if (element.at_pointer(mapMarkers.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, mapMarkers, &jTmp);
    static const JsonPointer entries("entries");
    static const JsonPointer canTravel("canTravel");

    std::vector<simdjson::dom::element> parsedEntries;
    ReadVector(jTmp, entries, &parsedEntries);

    std::vector<MapMarker> markers(parsedEntries.size());
    for (size_t i = 0; i != parsedEntries.size(); ++i) {
      const char* tmp;
      ReadEx(parsedEntries[i], formDesc, &tmp);
      markers[i].refr = FormDesc::FromString(tmp);
      ReadEx(parsedEntries[i], canTravel, &markers[i].canTravel);
    }
    res.mapMarkers = markers;
  }

  if (element.at_pointer(ingredientEffects.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, ingredientEffects, &jTmp);
    static const JsonPointer entries("entries");
    static const JsonPointer mask("mask");

    std::vector<simdjson::dom::element> parsedEntries;
    ReadVector(jTmp, entries, &parsedEntries);

    std::vector<IngredientEffects> effects(parsedEntries.size());
    for (size_t i = 0; i != parsedEntries.size(); ++i) {
      const char* tmp;
      ReadEx(parsedEntries[i], formDesc, &tmp);
      effects[i].ingredient = FormDesc::FromString(tmp);
      uint32_t maskTmp = 0;
      ReadEx(parsedEntries[i], mask, &maskTmp);
      effects[i].mask = static_cast<uint8_t>(maskTmp & 0x0f);
    }
    res.ingredientEffects = effects;
  }

  if (element.at_pointer(playerGlobals.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, playerGlobals, &jTmp);
    static const JsonPointer entries("entries");
    static const JsonPointer value("value");

    std::vector<simdjson::dom::element> parsedEntries;
    ReadVector(jTmp, entries, &parsedEntries);

    std::vector<PlayerGlobal> globals(parsedEntries.size());
    for (size_t i = 0; i != parsedEntries.size(); ++i) {
      const char* tmp;
      ReadEx(parsedEntries[i], formDesc, &tmp);
      globals[i].global = FormDesc::FromString(tmp);
      ReadEx(parsedEntries[i], value, &globals[i].value);
    }
    res.playerGlobals = globals;
  }

  if (element.at_pointer(questStages.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, questStages, &jTmp);
    static const JsonPointer entries("entries");
    static const JsonPointer quest("quest");
    static const JsonPointer stage("stage");

    std::vector<simdjson::dom::element> parsedEntries;
    ReadVector(jTmp, entries, &parsedEntries);

    std::vector<QuestStageDone> stages(parsedEntries.size());
    for (size_t i = 0; i != parsedEntries.size(); ++i) {
      const char* tmp;
      ReadEx(parsedEntries[i], quest, &tmp);
      stages[i].quest = FormDesc::FromString(tmp);
      uint32_t stageTmp = 0;
      ReadEx(parsedEntries[i], stage, &stageTmp);
      stages[i].stage = static_cast<uint16_t>(stageTmp);
    }
    res.questStages = stages;
  }

  if (element.at_pointer(placedBySpell.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    const char* placedTmp;
    ReadEx(element, placedBySpell, &placedTmp);
    res.placedBySpell = FormDesc::FromString(placedTmp);
  }

  if (element.at_pointer(shoutRecord.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, shoutRecord, &jTmp);
    static const JsonPointer shouts("shouts"), wordsKnown("wordsKnown"),
      wordsUnlocked("wordsUnlocked");
    ShoutRecord record;
    std::vector<std::string> data;
    ReadVector(jTmp, shouts, &data);
    record.shouts = ToFormDescsArray(data);
    data.clear();
    ReadVector(jTmp, wordsKnown, &data);
    record.wordsKnown = ToFormDescsArray(data);
    data.clear();
    ReadVector(jTmp, wordsUnlocked, &data);
    record.wordsUnlocked = ToFormDescsArray(data);
    res.shoutRecord = record;
  }

  if (element.at_pointer(runningEffects.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, runningEffects, &jTmp);
    static const JsonPointer entries("entries"), effect("effect"),
      source("source"), caster("caster"), kind("kind"), av("av"),
      secondAv("secondAv"), secondWeight("secondWeight"),
      magnitude("magnitude"), durationS("durationS"), elapsedS("elapsedS"),
      asOf("asOf"), recover("recover"), detrimental("detrimental"),
      noDuration("noDuration");

    std::vector<simdjson::dom::element> parsedEntries;
    ReadVector(jTmp, entries, &parsedEntries);

    std::vector<RunningEffect> running(parsedEntries.size());
    for (size_t i = 0; i != parsedEntries.size(); ++i) {
      auto& e = running[i];
      const char* tmp;
      ReadEx(parsedEntries[i], effect, &tmp);
      e.effect = FormDesc::FromString(tmp);
      ReadEx(parsedEntries[i], source, &tmp);
      e.source = FormDesc::FromString(tmp);
      if (parsedEntries[i].at_pointer(caster.GetData()).error() ==
          simdjson::error_code::SUCCESS) {
        ReadEx(parsedEntries[i], caster, &tmp);
        e.caster = FormDesc::FromString(tmp);
      }
      ReadEx(parsedEntries[i], kind, &e.kind);
      ReadEx(parsedEntries[i], av, &e.av);
      ReadEx(parsedEntries[i], secondAv, &e.secondAv);
      ReadEx(parsedEntries[i], secondWeight, &e.secondWeight);
      ReadEx(parsedEntries[i], magnitude, &e.magnitude);
      ReadEx(parsedEntries[i], durationS, &e.durationS);
      ReadEx(parsedEntries[i], elapsedS, &e.elapsedS);
      // absent in records written before it was kept
      if (parsedEntries[i].at_pointer(asOf.GetData()).error() ==
          simdjson::error_code::SUCCESS) {
        ReadEx(parsedEntries[i], asOf, &e.asOfMs);
      }
      ReadEx(parsedEntries[i], recover, &e.recover);
      ReadEx(parsedEntries[i], detrimental, &e.detrimental);
      ReadEx(parsedEntries[i], noDuration, &e.noDuration);
    }
    res.runningEffects = running;
  }

  if (element.at_pointer(favorites.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, favorites, &jTmp);
    static const JsonPointer entries("entries");
    static const JsonPointer hotkey("hotkey");

    std::vector<simdjson::dom::element> parsedEntries;
    ReadVector(jTmp, entries, &parsedEntries);

    std::vector<Favorite> kept(parsedEntries.size());
    for (size_t i = 0; i != parsedEntries.size(); ++i) {
      const char* tmp;
      ReadEx(parsedEntries[i], formDesc, &tmp);
      kept[i].form = FormDesc::FromString(tmp);
      int32_t hotkeyTmp = -1;
      ReadEx(parsedEntries[i], hotkey, &hotkeyTmp);
      kept[i].hotkey =
        hotkeyTmp >= 0 && hotkeyTmp <= 7 ? static_cast<int8_t>(hotkeyTmp) : -1;
    }
    res.favorites = kept;
  }

  if (element.at_pointer(staffRank.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    uint32_t tmp = 0;
    ReadEx(element, staffRank, &tmp);
    res.staffRank = static_cast<uint8_t>(tmp > 3 ? 3 : tmp);
  }

  if (element.at_pointer(raceMenuPreset.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    const char* tmp;
    ReadEx(element, raceMenuPreset, &tmp);
    res.raceMenuPreset = tmp;
  }

  if (element.at_pointer(actorValueRecord.GetData()).error() ==
      simdjson::error_code::SUCCESS) {
    ReadEx(element, actorValueRecord, &jTmp);
    static const JsonPointer bases("bases"), skills("skills"),
      legendary("legendary"), av("av"), base("base"), skill("skill"),
      level("level"), xp("xp"), threshold("threshold"), count("count");
    ActorValueRecord record;

    std::vector<simdjson::dom::element> parsed;
    ReadVector(jTmp, bases, &parsed);
    for (auto& entry : parsed) {
      uint32_t avTmp = 0;
      double baseTmp = 0;
      ReadEx(entry, av, &avTmp);
      ReadEx(entry, base, &baseTmp);
      record.bases.emplace_back(static_cast<uint8_t>(avTmp),
                                static_cast<float>(baseTmp));
    }
    parsed.clear();
    ReadVector(jTmp, skills, &parsed);
    for (auto& entry : parsed) {
      uint32_t skillTmp = 0;
      double levelTmp = 0, xpTmp = 0, thresholdTmp = 0;
      ReadEx(entry, skill, &skillTmp);
      ReadEx(entry, level, &levelTmp);
      ReadEx(entry, xp, &xpTmp);
      ReadEx(entry, threshold, &thresholdTmp);
      record.skills.push_back(ActorValueRecord::Skill{
        static_cast<uint8_t>(skillTmp), static_cast<float>(levelTmp),
        static_cast<float>(xpTmp), static_cast<float>(thresholdTmp) });
    }
    parsed.clear();
    ReadVector(jTmp, legendary, &parsed);
    for (auto& entry : parsed) {
      uint32_t skillTmp = 0, countTmp = 0;
      ReadEx(entry, skill, &skillTmp);
      ReadEx(entry, count, &countTmp);
      record.legendary.emplace_back(static_cast<uint8_t>(skillTmp),
                                    static_cast<uint16_t>(countTmp));
    }
    double xpTmp = 0, thresholdTmp = 0;
    uint32_t levelTmp = 1;
    ReadEx(jTmp, xp, &xpTmp);
    ReadEx(jTmp, threshold, &thresholdTmp);
    ReadEx(jTmp, level, &levelTmp);
    record.xp = static_cast<float>(xpTmp);
    record.threshold = static_cast<float>(thresholdTmp);
    record.level = static_cast<uint16_t>(levelTmp);
    res.actorValueRecord = record;
  }

  // ADR-043 decision 2: a raw-shape record under a known order is written
  // plugin-relative under the order it was saved in (rawOrder, else the
  // running one) and read back under the running one. The migration is a
  // save and a load, the one conversion path for both; the result keeps
  // formVersion 1 so the driver writes it back as version 2.
  if (files && !pluginRelative) {
    const std::string described =
      ToJson(res, rawOrder ? rawOrder : files).dump();
    simdjson::dom::parser parser;
    simdjson::dom::element rebased = parser.parse(described).value();
    MpChangeForm migrated = JsonToChangeForm(rebased, files, nullptr);
    migrated.formVersion = 1;
    return migrated;
  }

  return res;
}

void LearnedSpells::LearnSpell(const Data::key_type spellId)
{
  _learnedSpellIds.emplace(spellId);
}

void LearnedSpells::ForgetSpell(const Data::key_type spellId)
{
  _learnedSpellIds.erase(spellId);
}

size_t LearnedSpells::Count() const noexcept
{
  return _learnedSpellIds.size();
}

bool LearnedSpells::IsSpellLearned(const Data::key_type spellId) const
{
  return _learnedSpellIds.count(spellId) != 0;
}

std::vector<LearnedSpells::Data::key_type> LearnedSpells::GetLearnedSpells()
  const
{
  return std::vector(_learnedSpellIds.begin(), _learnedSpellIds.end());
}
