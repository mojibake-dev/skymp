#include "InventoryApi.h"
#include "CallNativeApi.h"
#include "NullPointerException.h"
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

extern CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

namespace {

Napi::Value ToJsValue(Napi::Env env, RE::ExtraHealth& extra)
{
  auto res = Napi::Object::New(env);
  res.Set("health", Napi::Number::New(env, extra.health));
  res.Set("type", Napi::String::New(env, "Health"));
  return res;
}

Napi::Value ToJsValue(Napi::Env env, RE::ExtraCount& extra)
{
  auto res = Napi::Object::New(env);
  res.Set("count", Napi::Number::New(env, extra.count));
  res.Set("type", Napi::String::New(env, "Count"));
  return res;
}

Napi::Value ToJsValue(Napi::Env env, RE::ExtraEnchantment& extra)
{
  auto res = Napi::Object::New(env);
  res.Set("enchantmentId",
          Napi::Number::New(
            env, (extra.enchantment ? extra.enchantment->formID : 0)));
  res.Set("maxCharge", extra.charge);
  res.Set("type", Napi::String::New(env, "Enchantment"));
  return res;
}

Napi::Value ToJsValue(Napi::Env env, RE::ExtraCharge& extra)
{
  auto res = Napi::Object::New(env);
  res.Set("charge", Napi::Number::New(env, extra.charge));
  res.Set("type", Napi::String::New(env, "Charge"));
  return res;
}

Napi::Value ToJsValue(Napi::Env env, RE::ExtraTextDisplayData& extra)
{
  auto res = Napi::Object::New(env);
  res.Set("name", Napi::String::New(env, extra.displayName.c_str()));
  res.Set("type", Napi::String::New(env, "TextDisplayData"));
  return res;
}

Napi::Value ToJsValue(Napi::Env env, RE::ExtraSoul& extra)
{
  auto res = Napi::Object::New(env);
  res.Set("soul", Napi::Number::New(env, static_cast<int>(extra.GetType())));
  res.Set("type", Napi::String::New(env, "Soul"));
  return res;
}

Napi::Value ToJsValue(Napi::Env env, RE::ExtraPoison& extra)
{
  auto res = Napi::Object::New(env);
  res.Set("poisonId",
          Napi::Number::New(env, (extra.poison ? extra.poison->formID : 0)));
  res.Set(
    "count",
    Napi::Number::New(env, (reinterpret_cast<RE::ExtraPoison&>(extra).count)));
  res.Set("type", Napi::String::New(env, "Poison"));
  return res;
}

Napi::Value ToJsValue(Napi::Env env, RE::ExtraWorn& extra)
{
  auto res = Napi::Object::New(env);
  res.Set("type", Napi::String::New(env, "Worn"));
  return res;
}

Napi::Value ToJsValue(Napi::Env env, RE::ExtraWornLeft& extra)
{
  auto res = Napi::Object::New(env);
  res.Set("type", Napi::String::New(env, "WornLeft"));
  return res;
}

Napi::Value ToJsValue(Napi::Env env, RE::BSExtraData* extraData)
{
  /**
   * ExtraDataList has GetByType<T> which can be used to avoid casts *
   */
  if (extraData) {
    switch (extraData->GetType()) {
      case RE::ExtraDataType::kHealth:
        return ToJsValue(env, *reinterpret_cast<RE::ExtraHealth*>(extraData));
      case RE::ExtraDataType::kCount:
        return ToJsValue(env, *reinterpret_cast<RE::ExtraCount*>(extraData));
      case RE::ExtraDataType::kEnchantment:
        return ToJsValue(env,
                         *reinterpret_cast<RE::ExtraEnchantment*>(extraData));
      case RE::ExtraDataType::kCharge:
        return ToJsValue(env, *reinterpret_cast<RE::ExtraCharge*>(extraData));
      case RE::ExtraDataType::kTextDisplayData:
        return ToJsValue(
          env, *reinterpret_cast<RE::ExtraTextDisplayData*>(extraData));
      case RE::ExtraDataType::kSoul:
        return ToJsValue(env, *reinterpret_cast<RE::ExtraSoul*>(extraData));
      case RE::ExtraDataType::kPoison:
        return ToJsValue(env, *reinterpret_cast<RE::ExtraPoison*>(extraData));
      case RE::ExtraDataType::kWorn:
        return ToJsValue(env, *reinterpret_cast<RE::ExtraWorn*>(extraData));
      case RE::ExtraDataType::kWornLeft:
        return ToJsValue(env,
                         *reinterpret_cast<RE::ExtraWornLeft*>(extraData));
    }
  }
  return env.Undefined();
}

Napi::Value ToJsValue(Napi::Env env, RE::ExtraDataList* extraList)
{
  if (!extraList) {
    return env.Null();
  }

  std::vector<Napi::Value> jData;

  RE::BSReadLockGuard lock(extraList->GetLock());

  for (auto it = extraList->begin(); it != extraList->end(); ++it) {
    auto extra = ToJsValue(env, &(*it));
    if (!extra.IsUndefined()) {
      jData.push_back(extra);
    }
  }

  auto resultArray = Napi::Array::New(env, jData.size());
  for (uint32_t i = 0; i < static_cast<uint32_t>(jData.size()); i++) {
    resultArray.Set(i, jData[i]);
  }
  return resultArray;
}

Napi::Value ToJsValue(Napi::Env env,
                      RE::BSSimpleList<RE::ExtraDataList*>* extendDataList)
{
  if (!extendDataList) {
    return env.Null();
  }

  auto first = extendDataList->begin();
  auto last = extendDataList->end();

  std::vector<Napi::Value> arr;
  for (auto it = first; it != last; ++it) {
    arr.push_back(ToJsValue(env, *it));
  }

  auto resultArray = Napi::Array::New(env, arr.size());

  for (uint32_t i = 0; i < static_cast<uint32_t>(arr.size()); i++) {
    resultArray.Set(i, arr[i]);
  }

  return resultArray;
}
}

Napi::Value InventoryApi::GetExtraContainerChanges(
  const Napi::CallbackInfo& info)
{
  auto objectReferenceId =
    NapiHelper::ExtractUInt32(info[0], "objectReferenceId");

  auto refr = RE::TESForm::LookupByID<RE::TESObjectREFR>(objectReferenceId);
  if (!refr ||
      (refr->formType != RE::FormType::ActorCharacter &&
       refr->formType != RE::FormType::Reference)) {
    return info.Env().Null();
  }

  auto cntChanges = refr->extraList.GetByType<RE::ExtraContainerChanges>();

  if (!cntChanges || !cntChanges->changes || !cntChanges->changes->entryList) {
    return info.Env().Null();
  }

  auto first = cntChanges->changes->entryList->begin();
  auto last = cntChanges->changes->entryList->end();

  std::vector<Napi::Value> res;

  for (auto it = first; it != last; ++it) {
    const auto baseID = (*it)->object ? (*it)->object->formID : 0;

    auto jEntry = Napi::Object::New(info.Env());
    jEntry.Set("countDelta", (*it)->countDelta);
    jEntry.Set("baseId", Napi::Number::New(info.Env(), baseID));
    jEntry.Set("extendDataList", ToJsValue(info.Env(), (*it)->extraLists));

    res.push_back(jEntry);
  }

  auto resultArray = Napi::Array::New(info.Env(), res.size());

  for (uint32_t i = 0; i < static_cast<uint32_t>(res.size()); i++) {
    resultArray.Set(i, res[i]);
  }

  return resultArray;
}

Napi::Value InventoryApi::GetContainer(const Napi::CallbackInfo& info)
{
  auto formID = NapiHelper::ExtractUInt32(info[0], "formId");
  auto form = RE::TESForm::LookupByID<RE::TESObjectREFR>(formID);

  if (!form) {
    return Napi::Array::New(info.Env(), 0);
  }

  auto pContainer = form->As<RE::TESContainer>();

  if (!pContainer) {
    return Napi::Array::New(info.Env(), 0);
  }

  auto res = Napi::Array::New(info.Env(), pContainer->numContainerObjects);

  for (uint32_t i = 0; i < pContainer->numContainerObjects; ++i) {
    auto e = pContainer->containerObjects[i];
    auto jEntry = Napi::Object::New(info.Env());
    jEntry.Set("count", Napi::Number::New(info.Env(), e->count));
    jEntry.Set("baseId",
               Napi::Number::New(info.Env(), e->obj ? e->obj->formID : 0));
    res.Set(i, jEntry);
  }

  return res;
}

namespace {
struct BoundObject
{
  double baseId;
  int count;
  RE::BGSEquipSlot* slot;
};
enum EquipSlot
{
  BothHands = 0x13f45,
  LeftHand = 0x13f43,
  RightHand = 0x13f42
};
}

Napi::Value InventoryApi::SetInventory(const Napi::CallbackInfo& info)
{
  double formId = NapiHelper::ExtractDouble(info[0], "formId");
  Napi::Object inventory = NapiHelper::ExtractObject(info[1], "inv");

  Napi::Array entries =
    NapiHelper::ExtractArray(inventory.Get("entries"), "inv.entries");

  const int size = entries.Length();
  RE::Actor* pActor = RE::TESForm::LookupByID<RE::Actor>(formId);

  if (!pActor) {
    throw NullPointerException("pActor");
  }

  std::vector<BoundObject> objects;

  objects.reserve(size);

  for (int i = 0; i < size; ++i) {
    std::string comment = fmt::format("inv.entries[{}]", i);
    std::string comment1 = fmt::format("inv.entries[{}].baseId", i);
    std::string comment2 = fmt::format("inv.entries[{}].count", i);
    std::string comment3 = fmt::format("inv.entries[{}].worn", i);
    std::string comment4 = fmt::format("inv.entries[{}].wornLeft", i);

    Napi::Object entry =
      NapiHelper::ExtractObject(entries.Get(i), comment.data());
    double baseId =
      NapiHelper::ExtractDouble(entry.Get("baseId"), comment1.data());

    const RE::TESBoundObject* pBoundObject =
      RE::TESForm::LookupByID<RE::TESBoundObject>(baseId);

    if (!pBoundObject) {
      continue;
    }

    int count = NapiHelper::ExtractInt32(entry.Get("count"), comment2.data());

    const bool worn = (!entry.Get("worn").IsUndefined())
      ? NapiHelper::ExtractBoolean(entry.Get("worn"), comment3.data())
      : false;

    const bool wornLeft = (!entry.Get("wornLeft").IsUndefined())
      ? NapiHelper::ExtractBoolean(entry.Get("wornLeft"), comment4.data())
      : false;

    RE::BGSEquipSlot* slot = nullptr;

    if (worn || wornLeft) {
      slot = worn ? static_cast<RE::BGSEquipSlot*>(
                      RE::TESForm::LookupByID(EquipSlot::RightHand))
                  : static_cast<RE::BGSEquipSlot*>(
                      RE::TESForm::LookupByID(EquipSlot::LeftHand));
    }

    objects.push_back({ baseId, count, slot });
  }

  g_nativeCallRequirements.gameThrQ->AddTask([formId, objects](Viet::Void) {
    RE::Actor* pActor = RE::TESForm::LookupByID<RE::Actor>(formId);

    if (!pActor) {
      return;
    }

    for (auto& object : objects) {
      RE::TESBoundObject* pBoundObject =
        RE::TESForm::LookupByID<RE::TESBoundObject>(object.baseId);

      if (!pBoundObject) {
        continue;
      }

      pActor->AddObjectToContainer(pBoundObject, nullptr, object.count,
                                   nullptr);
      if (object.slot) {
        RE::ActorEquipManager* manager = RE::ActorEquipManager::GetSingleton();
        bool forceEquip = pActor->GetFormID() != 0x14;
        manager->EquipObject(pActor, pBoundObject, nullptr, 1, object.slot,
                             false, forceEquip, false, false);
      }
    }
  });
  return info.Env().Undefined();
}

// Forms the engine makes at runtime, read for the wire and made from it.
// Assumed from a static read of the engine: the four creators of
// BGSCreatedObjectManager (CommonLibSSE-NG include/RE/B/
// BGSCreatedObjectManager.h; AddWeaponEnchantment and AddArmorEnchantment are
// CommonLib's own, src/RE/B/BGSCreatedObjectManager.cpp:11-23; AddPotion
// 36167 and AddPoison 36168 are not in CommonLib) each lock the manager and
// hand back an existing form whose effects match (MGEF, magnitude, area,
// duration; the cost not compared), else copy the default template into a new
// form above 0xFEFFFFFF. The engine counts a created form's references
// (CreatedMagicItemData::refCount) and deletes it at none: its own console
// path (PlayerCreatePotion 22506) adds one per inventory unit and releases
// the creator's slot, and a drink releases one. Units added through
// TESModPlatform.addItemEx count none, so a drink could take the count to
// none with units still held: every form this native makes or reads is
// pinned for the session instead (kCreatedPin references added once, on the
// main thread, under the manager's lock), more than a session's drinks
// spend. The creators run on the main thread (SKSE's UI task queue, as
// ChangeForm.cpp's apply), never on the JS thread.
namespace {
constexpr uint32_t kCreatedWeaponEnchantment = 0;
constexpr uint32_t kCreatedArmorEnchantment = 1;
constexpr uint32_t kCreatedPotion = 2;
constexpr uint32_t kCreatedPoison = 3;
constexpr size_t kCreatedEffectsMax = 8; // wire-schema cap::CREATED_EFFECTS
constexpr uint32_t kCreatedPin = 0x10000;

struct CreatedEffectDef
{
  uint32_t mgef = 0;
  float magnitude = 0.f;
  uint32_t area = 0;
  uint32_t duration = 0;
  float cost = 0.f;
};

struct CreatedDef
{
  uint32_t kind = 0;
  std::vector<CreatedEffectDef> effects;
};

// The engine's effect compare (0x1401430f0, assumed from a static read)
// skips the magnitude of an MGEF flagged kNoMagnitude, the area of one
// flagged kNoArea and the duration of one flagged kNoDuration
// (CommonLibSSE-NG include/RE/E/EffectSetting.h:54-56), and compares
// conditions (a created form has none) but not the cost. So a definition is
// normalized before any key is built, on the way out (ReadCreatedForm) and
// on the way in (CreateCreatedForm): each skipped field zeroed, so two
// definitions the engine takes for one form are one on the wire, in the
// record and in every cache. The MGEF is a game-file form, read on the
// calling thread (LookupByID takes no lock at this CommonLib pin; assumes a
// game-file MGEF never moves under it); false when this game holds none
bool NormalizeCreatedEffect(CreatedEffectDef& e,
                            const RE::EffectSetting* setting)
{
  if (!setting) {
    return false;
  }
  using Flag = RE::EffectSetting::EffectSettingData::Flag;
  if (setting->data.flags.any(Flag::kNoMagnitude)) {
    e.magnitude = 0.f;
  }
  if (setting->data.flags.any(Flag::kNoArea)) {
    e.area = 0;
  }
  if (setting->data.flags.any(Flag::kNoDuration)) {
    e.duration = 0;
  }
  return true;
}

// a definition's identity, the creators' own match: the kind and each
// effect's MGEF, magnitude (its bits), area and duration, in order, after
// NormalizeCreatedEffect
std::string CreatedKey(const CreatedDef& def)
{
  std::string key = std::to_string(def.kind);
  for (const auto& e : def.effects) {
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(e.magnitude));
    std::memcpy(&bits, &e.magnitude, sizeof(bits));
    key += fmt::format("|{:x},{:x},{},{}", e.mgef, bits, e.area, e.duration);
  }
  return key;
}

std::mutex g_createdMutex;
// definition key -> the local form made or read for it. Unbounded: a
// session holds one entry per distinct created form its inventories name (a
// few dozen), and each costs a key and an id. A cached form the engine no
// longer holds (a load game clears its created forms) is dropped at its next
// ask and made again
std::unordered_map<std::string, RE::FormID> g_createdForms;
std::unordered_set<std::string> g_createdQueued;
std::unordered_set<RE::FormID> g_createdPinned;
// definition key -> when its creator last made none: asked again after
// kCreatedRetryMs, never refused for the session
std::unordered_map<std::string, std::chrono::steady_clock::time_point>
  g_createdFailed;
constexpr auto kCreatedRetryMs = std::chrono::milliseconds(5000);

// The manager's table that holds `item`: its kind, or nullopt. The caller
// holds the manager's lock
std::optional<uint32_t> CreatedKindLocked(RE::BGSCreatedObjectManager& manager,
                                          const RE::MagicItem* item)
{
  for (const auto& data : manager.weaponEnchantments) {
    if (data.magicItem == item) {
      return kCreatedWeaponEnchantment;
    }
  }
  for (const auto& data : manager.armorEnchantments) {
    if (data.magicItem == item) {
      return kCreatedArmorEnchantment;
    }
  }
  for (const auto& entry : manager.potions) {
    if (entry.second.magicItem == item) {
      return kCreatedPotion;
    }
  }
  for (const auto& entry : manager.poisons) {
    if (entry.second.magicItem == item) {
      return kCreatedPoison;
    }
  }
  return std::nullopt;
}

RE::MagicItem* CreatedMagicItem(RE::TESForm* form)
{
  if (!form || form->GetFormID() <= 0xFEFFFFFF) {
    return nullptr;
  }
  if (auto* potion = form->As<RE::AlchemyItem>()) {
    return potion;
  }
  if (auto* enchantment = form->As<RE::EnchantmentItem>()) {
    return enchantment;
  }
  return nullptr;
}

// Main thread: kCreatedPin references on the manager's record of `item`
// (assumes a reference held here keeps the engine from deleting the form
// while this session runs)
void PinCreatedOnMain(RE::MagicItem* item)
{
  auto* manager = RE::BGSCreatedObjectManager::GetSingleton();
  if (!manager || !item) {
    return;
  }
  RE::BSSpinLockGuard guard(manager->lock);
  const auto pin = [&](RE::BGSCreatedObjectManager::CreatedMagicItemData& d) {
    if (d.magicItem == item) {
      d.refCount = d.refCount + kCreatedPin;
      return true;
    }
    return false;
  };
  for (auto& data : manager->weaponEnchantments) {
    if (pin(data)) {
      return;
    }
  }
  for (auto& data : manager->armorEnchantments) {
    if (pin(data)) {
      return;
    }
  }
  for (auto& entry : manager->potions) {
    if (pin(entry.second)) {
      return;
    }
  }
  for (auto& entry : manager->poisons) {
    if (pin(entry.second)) {
      return;
    }
  }
}

// Queues the pin of a form read on the JS thread, once a session
void QueuePinCreated(RE::FormID formId)
{
  {
    std::lock_guard lock(g_createdMutex);
    if (!g_createdPinned.insert(formId).second) {
      return;
    }
  }
  if (auto* tasks = SKSE::GetTaskInterface()) {
    tasks->AddUITask([formId]() {
      PinCreatedOnMain(CreatedMagicItem(RE::TESForm::LookupByID(formId)));
    });
  }
}

// AddPotion and AddPoison as a static read gives them (assumed): the
// manager, the slot the made form is written to (holding one reference) and
// the effect list; only the slot is read after the call, so a return value,
// if any, is never used
using AddAlchemyFn = void (*)(RE::BGSCreatedObjectManager*, RE::AlchemyItem**,
                              RE::BSTArray<RE::Effect>&);

// Main thread: the engine's creator of `def.kind` over `def`'s effects; the
// form, or null
RE::MagicItem* CreateOnMain(const CreatedDef& def)
{
  auto* manager = RE::BGSCreatedObjectManager::GetSingleton();
  if (!manager) {
    return nullptr;
  }
  RE::BSTArray<RE::Effect> effects;
  for (const auto& e : def.effects) {
    auto* setting = RE::TESForm::LookupByID<RE::EffectSetting>(e.mgef);
    if (!setting) {
      logger::warn("[verb:craft] createCreatedForm: {:x} is no magic effect "
                   "of this game",
                   e.mgef);
      return nullptr;
    }
    auto& effect = effects.emplace_back();
    effect.effectItem.magnitude = e.magnitude;
    effect.effectItem.area = e.area;
    effect.effectItem.duration = e.duration;
    effect.baseEffect = setting;
    effect.cost = e.cost;
  }
  switch (def.kind) {
    case kCreatedWeaponEnchantment:
      return manager->AddWeaponEnchantment(effects);
    case kCreatedArmorEnchantment:
      return manager->AddArmorEnchantment(effects);
    case kCreatedPotion:
    case kCreatedPoison: {
      RE::AlchemyItem* slot = nullptr;
      REL::Relocation<AddAlchemyFn> add{ REL::ID(
        def.kind == kCreatedPotion ? 36167 : 36168) };
      add(manager, &slot, effects);
      return slot;
    }
    default:
      return nullptr;
  }
}
}

Napi::Value InventoryApi::ReadCreatedForm(const Napi::CallbackInfo& info)
{
  const auto formId = NapiHelper::ExtractUInt32(info[0], "formId");
  // LookupByID reads the form map unlocked at this CommonLib pin (assumes a
  // created form this engine's inventory names is not deleted in the window,
  // the pin holding it once read); the manager's record and the form's effect
  // list are read under the manager's lock, which the engine's creators and
  // its reference steps take
  auto* item = CreatedMagicItem(RE::TESForm::LookupByID(formId));
  auto* manager = RE::BGSCreatedObjectManager::GetSingleton();
  if (!item || !manager) {
    return info.Env().Null();
  }
  CreatedDef def;
  std::vector<const RE::EffectSetting*> settings;
  {
    RE::BSSpinLockGuard guard(manager->lock);
    const auto kind = CreatedKindLocked(*manager, item);
    if (!kind) {
      return info.Env().Null();
    }
    def.kind = *kind;
    for (const auto* effect : item->effects) {
      if (!effect || !effect->baseEffect) {
        return info.Env().Null();
      }
      CreatedEffectDef e;
      e.mgef = effect->baseEffect->GetFormID();
      e.magnitude = effect->effectItem.magnitude;
      e.area = effect->effectItem.area;
      e.duration = effect->effectItem.duration;
      e.cost = effect->cost;
      def.effects.push_back(e);
      settings.push_back(effect->baseEffect);
    }
  }
  if (def.effects.empty()) {
    return info.Env().Null();
  }
  auto effects = Napi::Array::New(info.Env(), def.effects.size());
  for (uint32_t i = 0; i < def.effects.size(); ++i) {
    auto& e = def.effects[i];
    NormalizeCreatedEffect(e, settings[i]);
    auto jEffect = Napi::Object::New(info.Env());
    jEffect.Set("mgef", Napi::Number::New(info.Env(), e.mgef));
    jEffect.Set("magnitude", Napi::Number::New(info.Env(), e.magnitude));
    jEffect.Set("area", Napi::Number::New(info.Env(), e.area));
    jEffect.Set("duration", Napi::Number::New(info.Env(), e.duration));
    jEffect.Set("cost", Napi::Number::New(info.Env(), e.cost));
    effects.Set(i, jEffect);
  }
  {
    // the crafter's own form answers its definition from now on
    std::lock_guard lock(g_createdMutex);
    const std::string key = CreatedKey(def);
    g_createdForms.insert_or_assign(key, formId);
    g_createdFailed.erase(key);
  }
  QueuePinCreated(formId);
  auto res = Napi::Object::New(info.Env());
  res.Set("kind", Napi::Number::New(info.Env(), def.kind));
  res.Set("effects", effects);
  return res;
}

Napi::Value InventoryApi::CreateCreatedForm(const Napi::CallbackInfo& info)
{
  CreatedDef def;
  def.kind = NapiHelper::ExtractUInt32(info[0], "kind");
  const Napi::Array effects = NapiHelper::ExtractArray(info[1], "effects");
  const uint32_t n = effects.Length();
  if (def.kind > kCreatedPoison || n == 0 || n > kCreatedEffectsMax) {
    return Napi::Number::New(info.Env(), -1);
  }
  for (uint32_t i = 0; i < n; ++i) {
    const std::string name = fmt::format("effects[{}]", i);
    const Napi::Object e =
      NapiHelper::ExtractObject(effects.Get(i), name.data());
    CreatedEffectDef effect;
    effect.mgef = NapiHelper::ExtractUInt32(e.Get("mgef"), "mgef");
    effect.magnitude =
      NapiHelper::ExtractFloat(e.Get("magnitude"), "magnitude");
    effect.area = NapiHelper::ExtractUInt32(e.Get("area"), "area");
    effect.duration = NapiHelper::ExtractUInt32(e.Get("duration"), "duration");
    effect.cost = NapiHelper::ExtractFloat(e.Get("cost"), "cost");
    if (effect.mgef == 0 || effect.mgef > 0xFEFFFFFF ||
        !std::isfinite(effect.magnitude) || !std::isfinite(effect.cost) ||
        !NormalizeCreatedEffect(
          effect, RE::TESForm::LookupByID<RE::EffectSetting>(effect.mgef))) {
      return Napi::Number::New(info.Env(), -1);
    }
    def.effects.push_back(effect);
  }
  const std::string key = CreatedKey(def);
  {
    std::lock_guard lock(g_createdMutex);
    if (auto it = g_createdForms.find(key); it != g_createdForms.end()) {
      // a form a load game took away is made again (assumes a load clears
      // the created-object manager's forms)
      if (CreatedMagicItem(RE::TESForm::LookupByID(it->second))) {
        return Napi::Number::New(info.Env(), static_cast<double>(it->second));
      }
      g_createdPinned.erase(it->second);
      g_createdForms.erase(it);
    }
    if (auto it = g_createdFailed.find(key); it != g_createdFailed.end()) {
      if (std::chrono::steady_clock::now() - it->second < kCreatedRetryMs) {
        return Napi::Number::New(info.Env(), -1);
      }
      g_createdFailed.erase(it);
    }
    if (!g_createdQueued.insert(key).second) {
      return Napi::Number::New(info.Env(), 0);
    }
  }
  auto* tasks = SKSE::GetTaskInterface();
  if (!tasks) {
    std::lock_guard lock(g_createdMutex);
    g_createdQueued.erase(key);
    return Napi::Number::New(info.Env(), -1);
  }
  tasks->AddUITask([def, key]() {
    RE::MagicItem* item = nullptr;
    try {
      item = CreateOnMain(def);
    } catch (const std::exception& e) {
      logger::error("[verb:craft] createCreatedForm threw on the main "
                    "thread: {}",
                    e.what());
    }
    if (item) {
      PinCreatedOnMain(item);
    }
    {
      std::lock_guard lock(g_createdMutex);
      g_createdQueued.erase(key);
      if (item) {
        g_createdForms.insert_or_assign(key, item->GetFormID());
        g_createdPinned.insert(item->GetFormID());
      } else {
        g_createdFailed.insert_or_assign(key,
                                         std::chrono::steady_clock::now());
      }
    }
    logger::info("[verb:craft] createCreatedForm kind {} with {} effect(s): "
                 "{}",
                 def.kind, def.effects.size(),
                 item ? fmt::format("{:x}", item->GetFormID())
                      : std::string("none made, asked again in 5 s"));
  });
  return Napi::Number::New(info.Env(), 0);
}

Napi::Value InventoryApi::CreatedFormTemplate(const Napi::CallbackInfo& info)
{
  const auto kind = NapiHelper::ExtractUInt32(info[0], "kind");
  auto* objects = RE::BGSDefaultObjectManager::GetSingleton();
  if (!objects || kind > kCreatedPoison) {
    return Napi::Number::New(info.Env(), 0);
  }
  constexpr RE::DEFAULT_OBJECT kTemplates[] = {
    RE::DEFAULT_OBJECTS::kBaseWeaponEnchantment,
    RE::DEFAULT_OBJECTS::kBaseArmorEnchantment,
    RE::DEFAULT_OBJECTS::kBasePotion, RE::DEFAULT_OBJECTS::kBasePoison
  };
  const RE::TESForm* form = objects->GetObject(kTemplates[kind]);
  return Napi::Number::New(info.Env(), form ? form->GetFormID() : 0);
}

void InventoryApi::Register(Napi::Env env, Napi::Object& exports)
{
  exports.Set("getExtraContainerChanges",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(GetExtraContainerChanges)));
  exports.Set(
    "getContainer",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(GetContainer)));
  exports.Set(
    "setInventory",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetInventory)));
  exports.Set(
    "readCreatedForm",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(ReadCreatedForm)));
  exports.Set("createCreatedForm",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(CreateCreatedForm)));
  exports.Set("createdFormTemplate",
              Napi::Function::New(
                env, NapiHelper::WrapCppExceptions(CreatedFormTemplate)));
}
