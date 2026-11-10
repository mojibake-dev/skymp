#include "MpObjectReference.h"
#include "ChangeFormGuard.h"
#include "EvaluateTemplate.h"
#include "FormCallbacks.h"
#include "GetWeightFromRecord.h"
#include "Inventory.h"
#include "LeveledListUtils.h"
#include "MathUtils.h"
#include "MessageBase.h"
#include "MpActor.h"
#include "MpChangeForms.h"
#include "MsgType.h"
#include "Primitive.h"
#include "ScriptVariablesHolder.h"
#include "UpdatePropertyMessage.h"
#include "WorldState.h"
#include "gamemode_events/ActivateEvent.h"
#include "gamemode_events/PutItemEvent.h"
#include "gamemode_events/TakeItemEvent.h"
#include "libespm/CompressedFieldsCache.h"
#include "libespm/Convert.h"
#include "libespm/FURN.h"
#include "libespm/GroupUtils.h"
#include "libespm/Utils.h"
#include "papyrus-vm/Reader.h"
#include "papyrus-vm/Utils.h" // stricmp
#include "papyrus-vm/VirtualMachine.h"
#include "script_objects/EspmGameObject.h"
#include "script_storages/IScriptStorage.h"
#include "wire_bridge_cxx/rules.h"
#include <ScopedTask.h>
#include <TimeUtils.h>
#include <algorithm>
#include <antigo/Context.h>
#include <antigo/ResolvedContext.h>
#include <cctype>
#include <map>
#include <numeric>
#include <optional>

#include "LockPromptMessage.h"
#include "OpenContainerMessage.h"
#include "SetInventoryMessage.h"
#include "SpSnippet.h"
#include "TeleportMessage.h"

#include "script_classes/PapyrusObjectReference.h" // kOriginalNameExpression

constexpr uint32_t kPlayerCharacterLevel = 1;

UpdatePropertyMessage MpObjectReference::CreatePropertyMessage_(
  MpObjectReference* self, const char* name, const std::string& valueDump)
{
  return PreparePropertyMessage_(self, name, valueDump);
}

UpdatePropertyMessage MpObjectReference::PreparePropertyMessage_(
  MpObjectReference* self, const char* name, const std::string& valueDump)
{
  UpdatePropertyMessage res;

  std::string baseRecordType;

  auto& loader = self->GetParent()->GetEspm();
  auto base = loader.GetBrowser().LookupById(GetBaseId());
  if (base.rec) {
    baseRecordType = base.rec->GetType().ToString();
  }

  res.idx = self->GetIdx();
  res.propName = name;
  res.refrId = self->GetFormId();
  res.dataDump = valueDump;

  // See 'perf: improve game framerate #1186'
  // Client needs to know if it is DOOR or not. thuum docs/verbs/npc-puppet.md:
  // and whether a game-file id is an actor's (NPC_), whose properties reach
  // its form model like a copy's; without the type the client takes the id
  // for a plain engine reference and keeps only inventory, isOpen,
  // isHarvested and disabled, so the hosted word of a handoff never arrived
  // (b-npc-puppet 20261022-104032, step 29: c2 granted Riverwood, Sigrid's
  // AI off on every screen). A server-made actor's id (0xff...) needs no
  // type: the client binds every such id to its model (RespawnTest's shape
  // kept for players)
  if (baseRecordType == "DOOR" ||
      (baseRecordType == "NPC_" && self->GetFormId() < 0xff000000)) {
    res.baseRecordType = baseRecordType;
  }

  return res;
}

// TODO: de-duplicate code of
// OccupantDisableEventSink/OccupantDisableEventSink, the only difference is
// base classes

class OccupantDestroyEventSink : public MpActor::DestroyEventSink
{
public:
  OccupantDestroyEventSink(WorldState& wst_,
                           MpObjectReference* untrustedRefPtr_)
    : wst(wst_)
    , untrustedRefPtr(untrustedRefPtr_)
    , refId(untrustedRefPtr_->GetFormId())
  {
  }

  void BeforeDestroy(MpActor& actor) override
  {
    if (!RefStillValid()) {
      return;
    }

    if (untrustedRefPtr->occupant == &actor) {
      untrustedRefPtr->SetOpen(false);
      untrustedRefPtr->occupant = nullptr;
    }
    // memo D52: the set alone here; the sink's own removal waits for a
    // normal way out, so nothing frees this object under its callback
    if (untrustedRefPtr->containerOccupants.erase(&actor) &&
        untrustedRefPtr->containerOccupants.empty()) {
      untrustedRefPtr->SetOpen(false);
    }
  }

private:
  bool RefStillValid() const
  {
    return untrustedRefPtr == wst.LookupFormById(refId).get();
  }

  WorldState& wst;
  MpObjectReference* const untrustedRefPtr;
  const uint32_t refId;
};

class OccupantDisableEventSink : public MpActor::DisableEventSink
{
public:
  OccupantDisableEventSink(WorldState& wst_,
                           MpObjectReference* untrustedRefPtr_)
    : wst(wst_)
    , untrustedRefPtr(untrustedRefPtr_)
    , refId(untrustedRefPtr_->GetFormId())
  {
  }

  void BeforeDisable(MpActor& actor) override
  {
    if (!RefStillValid()) {
      return;
    }

    if (untrustedRefPtr->occupant == &actor) {
      untrustedRefPtr->SetOpen(false);
      untrustedRefPtr->occupant = nullptr;
    }
    if (untrustedRefPtr->containerOccupants.erase(&actor) &&
        untrustedRefPtr->containerOccupants.empty()) {
      untrustedRefPtr->SetOpen(false);
    }
  }

private:
  bool RefStillValid() const
  {
    return untrustedRefPtr == wst.LookupFormById(refId).get();
  }

  WorldState& wst;
  MpObjectReference* const untrustedRefPtr;
  const uint32_t refId;
};

namespace {
std::pair<int16_t, int16_t> GetGridPos(const NiPoint3& pos) noexcept
{
  return { int16_t(pos.x / 4096), int16_t(pos.y / 4096) };
}
}

struct AnimGraphHolder
{
  AnimGraphHolder() { boolVariables.fill(false); }

  std::array<bool, static_cast<size_t>(AnimationVariableBool::kNumVariables)>
    boolVariables;
};

struct ScriptState
{
  std::map<std::string, std::shared_ptr<ScriptVariablesHolder>> varHolders;
};

struct PrimitiveData
{
  NiPoint3 boundsDiv2;
  GeoProc::GeoPolygonProc polygonProc;
  // libespm REFR XPRM type: 2 is a sphere, judged by distance; the rest by
  // the box the vertices make (thuum docs/verbs/word-wall.md: a rotated
  // box of the sphere's extents reaches 2,920 units on its diagonals where
  // the engine's sphere fires at 2,065)
  uint32_t type = 0;
};

struct MpObjectReference::Impl
{
public:
  bool onInitEventSent = false;
  bool scriptsInited = false;
  std::unique_ptr<ScriptState> scriptState;
  AnimGraphHolder animGraphHolder;
  std::optional<PrimitiveData> primitive;
  bool teleportFlag = false;
  bool setPropertyCalled = false;
};

namespace {
ChangeFormGuard::Mode MakeMode(bool isLocationSaveNeeded,
                               SetPosMode setPosMode)
{
  if (isLocationSaveNeeded) {
    return ChangeFormGuard::Mode::RequestSave;
  }

  switch (setPosMode) {
    case SetPosMode::CalledByUpdateMovement:
      return ChangeFormGuard::Mode::Deferred;
    case SetPosMode::Other:
      return ChangeFormGuard::Mode::RequestSave;
    default:
      spdlog::critical("Invalid SetPosMode");
      std::terminate();
      return ChangeFormGuard::Mode::RequestSave;
  }
}

MpChangeForm MakeChangeForm(const LocationalData& locationalData)
{
  MpChangeForm changeForm;
  changeForm.position = locationalData.pos;
  changeForm.angle = locationalData.rot;
  changeForm.worldOrCellDesc = locationalData.cellOrWorldDesc;
  return changeForm;
}
}

MpObjectReference::MpObjectReference(
  const LocationalData& locationalData_, const FormCallbacks& callbacks_,
  uint32_t baseId_, std::string baseType_,
  std::optional<NiPoint3> primitiveBoundsDiv2, uint32_t primitiveType)
  : callbacks(new FormCallbacks(callbacks_))
  , baseId(baseId_)
  , baseType(baseType_)
  , ChangeFormGuard(MakeChangeForm(locationalData_), this)
{
  pImpl.reset(new Impl);
  asObjectReference = this;

  if (primitiveBoundsDiv2)
    SetPrimitive(*primitiveBoundsDiv2, primitiveType);
}

const NiPoint3& MpObjectReference::GetPos() const
{
  return ChangeForm().position;
}

const NiPoint3& MpObjectReference::GetAngle() const
{
  return ChangeForm().angle;
}

const FormDesc& MpObjectReference::GetCellOrWorld() const
{
  return ChangeForm().worldOrCellDesc;
}

const uint32_t& MpObjectReference::GetBaseId() const
{
  return baseId;
}

const std::string& MpObjectReference::GetBaseType() const
{
  return baseType;
}

const Inventory& MpObjectReference::GetInventory() const
{
  return ChangeForm().inv;
}

bool MpObjectReference::HasRecordedInventory() const
{
  // baseContainerAdded is set by every write of the record (SetInventory,
  // AddItem, AddItems, EnsureBaseContainerAdded), so it is the record's mark
  return GetBaseType() == "CONT" && ChangeForm().baseContainerAdded;
}

const bool& MpObjectReference::IsHarvested() const
{
  return ChangeForm().isHarvested;
}

const bool& MpObjectReference::IsOpen() const
{
  return ChangeForm().isOpen;
}

const bool& MpObjectReference::IsDisabled() const
{
  return ChangeForm().isDisabled;
}

const bool& MpObjectReference::IsDeleted() const
{
  return ChangeForm().isDeleted;
}

const uint32_t& MpObjectReference::GetCount() const
{
  return ChangeForm().count;
}

std::chrono::system_clock::duration MpObjectReference::GetRelootTime() const
{
  if (relootTimeOverride) {
    return *relootTimeOverride;
  }

  if (auto time = GetParent()->GetRelootTime(baseType)) {
    return *time;
  }

  if (!std::strcmp(baseType.data(), "FLOR") ||
      !std::strcmp(baseType.data(), "TREE")) {
    return std::chrono::hours(1);
  } else if (!std::strcmp(baseType.data(), "DOOR")) {
    return std::chrono::seconds(3);
  } else if (espm::utils::IsItem(espm::Type{ baseType.data() })) {
    return std::chrono::hours(1);
  } else if (!std::strcmp(baseType.data(), "CONT")) {
    return std::chrono::hours(1);
  }

  return std::chrono::hours(0);
}

bool MpObjectReference::GetAnimationVariableBool(const char* name) const
{
  AnimationVariableBool variable = AnimationVariableBool::kInvalidVariable;

  if (!Utils::stricmp(name, "bInJumpState")) {
    variable = AnimationVariableBool::kVariable_bInJumpState;
  } else if (!Utils::stricmp(name, "_skymp_isWeapDrawn")) {
    variable = AnimationVariableBool::kVariable__skymp_isWeapDrawn;
  } else if (!Utils::stricmp(name, "IsBlocking")) {
    variable = AnimationVariableBool::kVariable_IsBlocking;
  } else if (!Utils::stricmp(name, "IsSneaking")) {
    variable = AnimationVariableBool::kVariable_IsSneaking;
  }

  if (variable == AnimationVariableBool::kInvalidVariable) {
    spdlog::warn("MpObjectReference::GetAnimationVariableBool - unknown "
                 "variable name: {}",
                 name);
    return false;
  }

  return pImpl->animGraphHolder.boolVariables[static_cast<size_t>(variable)];
}

bool MpObjectReference::IsPointInsidePrimitive(const NiPoint3& point) const
{
  if (pImpl->primitive) {
    if (pImpl->primitive->type == 2) {
      return (point - GetPos()).Length() <= pImpl->primitive->boundsDiv2.x;
    }
    return Primitive::IsInside(point, pImpl->primitive->polygonProc);
  }
  return false;
}

bool MpObjectReference::HasPrimitive() const
{
  return pImpl->primitive.has_value();
}

FormCallbacks MpObjectReference::GetCallbacks() const
{
  if (!callbacks)
    return FormCallbacks::DoNothing();
  return *callbacks;
}

bool MpObjectReference::HasScript(const char* name) const
{
  return ToGameObject()->HasScript(name);
}

bool MpObjectReference::IsActivationBlocked() const
{
  return activationBlocked;
}

bool MpObjectReference::GetTeleportFlag() const
{
  return pImpl->teleportFlag;
}

void MpObjectReference::VisitProperties(CreateActorMessage& message,
                                        VisitPropertiesMode mode)
{
  if (IsHarvested()) {
    message.props.isHarvested = true;
  }

  if (IsOpen()) {
    message.props.isOpen = true;
  }

  // thuum docs/verbs/locks.md: the lock the client applies to its reference
  if (const auto lock = GetLock()) {
    LockProp prop;
    prop.level = lock->level;
    prop.key = lock->key;
    prop.locked = lock->locked;
    message.props.lock = prop;
  }

  if (auto actor = AsActor(); actor && actor->IsDead()) {
    message.props.isDead = true;
  } else if (actor && !actor->IsCreatedAsPlayer()) {
    // thuum docs/verbs/npc-fate.md (the deaths): an NPC's life is
    // said explicitly, so a seat that binds a corpse the server has stood up
    // (a reconnect after the sweep) resurrects it (formView's two-second
    // read); a copy's or a player's absent isDead keeps its meaning
    message.props.isDead = false;
  }

  // thuum (playtest 37, 2026-10-28, Eli: "shouldn't be that hard to change
  // that to on cell load right?"): an actor's inventory is its owner's alone
  // (mode All); a container's record is public, so every player whose grid
  // loads the container hears it in its CreateActor at the subscribe, and the
  // engine copy and its crosshair label are the server's before any open
  // (upstream's ROADMAP: "Incorrect 'empty' label on a container"). An
  // emptied record travels as an empty list, an untouched container not at
  // all (both engines hold the base's default). The opener's and the
  // occupants' sends (memo D52) stay as they are.
  if ((mode == VisitPropertiesMode::All && !GetInventory().IsEmpty()) ||
      HasRecordedInventory()) {
    message.props.inventory = GetInventory();
  }

  if (IsEspmForm() && IsDisabled()) {
    message.props.isDisabled = true;
  }

  if (ChangeForm().lastAnimation.has_value()) {
    message.props.lastAnimation = *ChangeForm().lastAnimation;
  }

  if (ChangeForm().setNodeScale.has_value()) {
    std::vector<SetNodeScaleEntry> setNodeScale;
    for (auto& [nodeName, scale] : *ChangeForm().setNodeScale) {
      SetNodeScaleEntry setNodeScaleEntry;
      setNodeScaleEntry.nodeName = nodeName;
      setNodeScaleEntry.scale = scale;
      setNodeScale.push_back(setNodeScaleEntry);
    }
    message.props.setNodeScale = std::move(setNodeScale);
  }

  if (ChangeForm().setNodeTextureSet.has_value()) {
    std::vector<SetNodeTextureSetEntry> setNodeTextureSet;
    for (auto& [nodeName, textureSetId] : *ChangeForm().setNodeTextureSet) {
      SetNodeTextureSetEntry setNodeTextureSetEntry;
      setNodeTextureSetEntry.nodeName = nodeName;
      setNodeTextureSetEntry.textureSetId =
        FormDesc::FromString(textureSetId).ToFormId(GetParent()->espmFiles);
      setNodeTextureSet.push_back(setNodeTextureSetEntry);
    }
    message.props.setNodeTextureSet = std::move(setNodeTextureSet);
  }

  if (ChangeForm().displayName.has_value()) {
    const std::string& raw = *ChangeForm().displayName;
    if (raw != PapyrusObjectReference::kOriginalNameExpression) {
      message.props.displayName = raw;
    }
  }

  // Property flags (isVisibleByOwner, isVisibleByNeighbor) are expected to be
  // checked by a caller (PartOne.cpp in this case)
  ChangeForm().dynamicFields.ForEachValueDump(
    [&](const std::string& propName, const std::string& valueDump) {
      CustomPropsEntry customPropsEntry;
      customPropsEntry.propName = propName;
      customPropsEntry.propValueJsonDump = valueDump;
      message.customPropsJsonDumps.push_back(customPropsEntry);
    });
}

void MpObjectReference::Activate(MpObjectReference& activationSource,
                                 bool defaultProcessingOnly,
                                 bool isSecondActivation, bool reportedByHost)
{
  if (spdlog::should_log(spdlog::level::trace)) {
    for (auto& script : ListActivePexInstances()) {
      spdlog::trace("MpObjectReference::Activate {:x} - found script {}",
                    GetFormId(), script->GetSourcePexName());
    }
  }

  if (auto worldState = activationSource.GetParent(); worldState->HasEspm()) {
    CheckInteractionAbility(activationSource);

    // Pillars puzzle Bleak Falls Barrow
    bool workaroundBypassParentsCheck = &activationSource == this;

    // Block if only activation parents can activate this
    auto refrId = GetFormId();
    if (!workaroundBypassParentsCheck && IsEspmForm() && !AsActor()) {
      auto lookupRes = worldState->GetEspm().GetBrowser().LookupById(refrId);
      auto data = espm::GetData<espm::REFR>(refrId, worldState);
      auto it = std::find_if(
        data.activationParents.begin(), data.activationParents.end(),
        [&](const espm::REFR::ActivationParentInfo& info) {
          return lookupRes.ToGlobalId(info.refrId) ==
            activationSource.GetFormId();
        });
      if (it == data.activationParents.end()) {
        if (data.isParentActivationOnly) {
          throw std::runtime_error(
            "Only activation parents can activate this object");
        }
      }
    }
  }

  if (isSecondActivation) {
    bool processed = ProcessActivateSecond(activationSource);
    if (processed) {
      auto arg = activationSource.ToVarValue();
      SendPapyrusEvent("SkympOnActivateClose", &arg, 1);
    }
  } else {
    ActivateEvent activateEvent(GetFormId(), activationSource.GetFormId());

    bool activationBlockedByMpApi = !activateEvent.Fire(GetParent());

    if (!activationBlockedByMpApi &&
        (!activationBlocked || defaultProcessingOnly)) {
      ProcessActivateNormal(activationSource, reportedByHost);
      ActivateChilds();
    } else {
      spdlog::trace(
        "Activation of form {:#x} has been blocked. Reasons: "
        "blocked by MpApi={}, form is blocked={}, defaultProcessingOnly={}",
        GetFormId(), activationBlockedByMpApi, activationBlocked,
        defaultProcessingOnly);
    }

    if (!defaultProcessingOnly && !activationBlockedByMpApi) {
      auto arg = activationSource.ToVarValue();
      SendPapyrusEvent("OnActivate", &arg, 1);
    }
  }
}

namespace {
// thuum ADR-068 rung 4: a game-file world object's Enable or Disable stamps
// its ref state recorded, whatever the cause (the server's own VM, the
// console, labState), at the object's sequence now (WorldState's
// objectSeqOf, the script calls' one sequence), so the state is attached,
// saved and served like a script's; an actor's grid is its own
void StampRefState(MpObjectReference& refr, ChangeFormGuard& guard)
{
  auto* worldState = refr.GetParent();
  if (!refr.IsEspmForm() || refr.AsActor() || !worldState) {
    return;
  }
  const uint32_t seq =
    worldState->objectSeqOf ? worldState->objectSeqOf(refr.GetFormId()) : 0;
  guard.EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.refStateSeq = std::max(changeForm.refStateSeq.value_or(0), seq);
  });
}
}

void MpObjectReference::Disable()
{
  if (ChangeForm().isDisabled) {
    return;
  }

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.isDisabled = true; });
  StampRefState(*this, *this);

  if (!IsEspmForm() || AsActor()) {
    RemoveFromGridAndUnsubscribeAll();
  }
}

void MpObjectReference::Enable()
{
  if (!ChangeForm().isDisabled) {
    return;
  }

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.isDisabled = false; });
  StampRefState(*this, *this);

  if (!IsEspmForm() || AsActor()) {
    ForceSubscriptionsUpdate();
  }
}

void MpObjectReference::SetPos(const NiPoint3& newPos, SetPosMode setPosMode)
{
  auto oldGridPos = GetGridPos(ChangeForm().position);
  auto newGridPos = GetGridPos(newPos);

  EditChangeForm(
    [&newPos](MpChangeFormREFR& changeForm) { changeForm.position = newPos; },
    MakeMode(IsLocationSavingNeeded(), setPosMode));

  if (oldGridPos != newGridPos || !everSubscribedOrListened) {
    // thuum docs/verbs/npc-fate.md (ADR-049): a player's fate pass before
    // the grid loads its 3x3, so the ring rolls at the player's level
    if (auto* worldState = GetParent();
        worldState && worldState->onSquareChange) {
      if (MpActor* actor = AsActor()) {
        worldState->onSquareChange(*actor);
      }
    }
    ForceSubscriptionsUpdate();
  }

  if (!IsDisabled()) {
    if (emittersWithPrimitives) {
      if (!primitivesWeAreInside) {
        primitivesWeAreInside.reset(new std::set<MpObjectReference*>);
      }

      for (auto& [emitterRefr, wasInside] : *emittersWithPrimitives) {
        bool inside = emitterRefr->IsPointInsidePrimitive(newPos);
        if (wasInside != inside) {
          wasInside = inside;
          auto me = ToVarValue();

          auto wst = GetParent();
          auto id = emitterRefr->GetFormId();
          auto myId = GetFormId();
          wst->SetTimer(std::chrono::seconds(0))
            .Then([wst, id, inside, me, myId, this](Viet::Void) {
              if (wst->LookupFormById(myId).get() != this) {
                wst->logger->error("Refr pointer expired", id);
                return;
              }
              auto& emitter = wst->LookupFormById(id);
              MpObjectReference* emitterRefr =
                emitter ? emitter->AsObjectReference() : nullptr;
              if (!emitterRefr) {
                wst->logger->error("Emitter not found in timer ({0:x})", id);
                return;
              }
              emitterRefr->SendPapyrusEvent(
                inside ? "OnTriggerEnter" : "OnTriggerLeave", &me, 1);
            });

          if (inside) {
            primitivesWeAreInside->insert(emitterRefr);
          } else {
            primitivesWeAreInside->erase(emitterRefr);
          }
        }
      }
    }

    if (primitivesWeAreInside) {
      auto me = ToVarValue();

      // May be modified inside loop, so copying
      const auto set = *primitivesWeAreInside;

      for (MpObjectReference* emitterRefr : set) {
        emitterRefr->SendPapyrusEvent("OnTrigger", &me, 1);
      }
    }
  }
}

void MpObjectReference::SetAngle(const NiPoint3& newAngle,
                                 SetAngleMode setAngleMode)
{
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.angle = newAngle; },
    MakeMode(IsLocationSavingNeeded(), setAngleMode));
}

void MpObjectReference::SetHarvested(bool harvested)
{
  if (harvested != ChangeForm().isHarvested) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.isHarvested = harvested;
    });
    SendMessageToActorListeners(
      CreatePropertyMessage_(this, "isHarvested",
                             harvested ? "true" : "false"),
      true);
  }
}

void MpObjectReference::SetOpen(bool open)
{
  if (open != ChangeForm().isOpen) {
    EditChangeForm(
      [&](MpChangeFormREFR& changeForm) { changeForm.isOpen = open; });
    SendMessageToActorListeners(
      CreatePropertyMessage_(this, "isOpen", open ? "true" : "false"), true);
    const bool container = baseType == "CONT" || IsCorpse();
    // thuum docs/verbs/containers.md (ADR-067 decision 1, Eli after playtest
    // 42: "fuck live updates of containers"): a container's contents sync at
    // its open (the record to the opener, ProcessActivateNormal) and at its
    // close: the record to every listener of the reference, the same
    // property the open sends, so a watcher's engine copy and the next
    // opener's crosshair hold the record without a reload. The shut is here
    // and not in RemoveContainerOccupant since a holder's destroy or
    // disable (the sinks above), a reloot and a script shut it here too.
    // Never while open: no two seats see one container's contents change
    // under them (decision 6 holds the menu to one seat). An untouched
    // container (no record, baseContainerAdded unset: every engine holds
    // the base's own roll) sends none, as the subscribe-time send does
    // (HasRecordedInventory), else a script's shut would blank the copies.
    const bool synced = container && !open && ChangeForm().baseContainerAdded;
    if (synced) {
      SendMessageToActorListeners(
        CreatePropertyMessage_(this, "inventory",
                               GetInventory().ToJson().dump()),
        true);
    }
    // thuum docs/verbs/containers.md: the lab reads a container's state here,
    // a chest's and a corpse's alike (x-container-hold v2 002233: the body's
    // open never printed, the claim could not hold)
    if (container) {
      if (open) {
        spdlog::info("[verb:containers] {:x} open with {} occupant(s)",
                     GetFormId(), containerOccupants.size());
      } else {
        spdlog::info("[verb:containers] {:x} shut with {} occupant(s); "
                     "inventory sent to {} listener(s)",
                     GetFormId(), containerOccupants.size(),
                     synced ? GetActorListeners().size() : 0);
      }
    }
  }
}

std::optional<MpChangeFormREFR::Lock> MpObjectReference::GetLock() const
{
  auto worldState = GetParent();
  if (!worldState || !worldState->locksEnabled) {
    return std::nullopt;
  }
  if (ChangeForm().lock.has_value()) {
    return ChangeForm().lock;
  }
  if (!worldState->HasEspm() || !IsEspmForm()) {
    return std::nullopt;
  }
  const auto lookupRes =
    worldState->GetEspm().GetBrowser().LookupById(GetFormId());
  const auto refr = espm::Convert<espm::REFR>(lookupRes.rec);
  if (!refr) {
    return std::nullopt;
  }
  const auto data = refr->GetData(worldState->GetEspmCache());
  if (!data.hasLock) {
    return std::nullopt;
  }
  MpChangeFormREFR::Lock lock;
  lock.level = data.lockLevel;
  lock.key = data.lockKey ? lookupRes.ToGlobalId(data.lockKey) : 0;
  lock.locked = true;
  return lock;
}

void MpObjectReference::SetLock(const MpChangeFormREFR::Lock& lock)
{
  const auto before = GetLock();
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.lock = lock; });
  if (!before || !(*before == lock)) {
    const nlohmann::json value = { { "level", lock.level },
                                   { "key", lock.key },
                                   { "locked", lock.locked } };
    SendMessageToActorListeners(
      CreatePropertyMessage_(this, "lock", value.dump()), true);
    spdlog::info("[verb:locks] {:x} {} (level {}, key {:x}, {} broken)",
                 GetFormId(), lock.locked ? "locked" : "unlocked", lock.level,
                 lock.key, lock.broken);
  }
}

void MpObjectReference::SendLockPrompt(MpActor& to)
{
  LockPromptMessage prompt;
  prompt.refr = GetFormId();
  to.SendToUser(prompt, true);
}

std::optional<MpChangeFormREFR::Destruction>
MpObjectReference::GetDestruction() const
{
  return ChangeForm().destruction;
}

void MpObjectReference::SetDestruction(
  const MpChangeFormREFR::Destruction& destruction)
{
  if (ChangeForm().destruction == destruction) {
    return;
  }
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.destruction = destruction;
  });
}

const std::optional<ScriptStateRecord>& MpObjectReference::GetScriptState()
  const
{
  return ChangeForm().scriptState;
}

void MpObjectReference::SetScriptState(std::optional<ScriptStateRecord> record)
{
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.scriptState = std::move(record);
  });
}

namespace {
// thuum D-C3 A (PENDING Eli): the wire's bounds on a serve's graph variables
// (wire-schema cap::GRAPH_VARS and a name's cap::ANIM_EVENT bytes)
constexpr size_t kGraphVarsServed = 16;
constexpr size_t kGraphVarNameBytes = 128;
}

bool MpObjectReference::BuildScriptStateApply(
  ScriptStateApplyMessage& out) const
{
  const auto& record = ChangeForm().scriptState;
  if (!record) {
    return false;
  }
  out.formId = GetFormId();
  out.fnv1a = record->fnv1a;
  out.seq = record->seq;
  out.event = record->event;
  out.scripts = record->scripts;
  // thuum D-C3 A (PENDING Eli; docs/research/fifty-fixes-2026-11-07.md read
  // 7): the reference's graph variables as kept now, at the serve, each with
  // its own sequence, never stamped at the record's take (a stack whose only
  // act sets a variable takes no new record). A name comes from a script
  // call's string argument, which the wire lets run to 16 KiB where a graph
  // variable's stops at 128 bytes, so a variable the serve could not carry
  // (a name empty, past 128 bytes or cut by a NUL; past the sixteenth) is
  // left out rather than make the whole apply unservable (OnScriptState's
  // guard on the view)
  out.graphVars.reset();
  if (const auto& kept = ChangeForm().graphVariables) {
    for (const auto& v : *kept) {
      if (out.graphVars && out.graphVars->size() >= kGraphVarsServed) {
        break;
      }
      if (v.name.empty() || v.name.size() > kGraphVarNameBytes ||
          v.name.find('\0') != std::string::npos) {
        continue;
      }
      ScriptStateGraphVar served;
      served.name = v.name;
      served.k = v.k;
      served.i = v.i;
      served.f = v.f;
      served.b = v.b;
      served.seq = v.seq;
      if (!out.graphVars) {
        out.graphVars.emplace();
      }
      out.graphVars->push_back(std::move(served));
    }
  }
  return true;
}

namespace {
// thuum docs/verbs/containers.md (ADR-067 decision 6): a move in or out of a
// container is its holder's alone. A mover not in it (its hold released by
// its close or the prune, a corpse's loot menu its engine opened before the
// server's word, a forged message) is refused, and as every refused move
// (memo D52) it hears both inventories again, since its engine moved the
// item already. A refusal and not a throw: the throw reached the tick's
// catch (ScampServer::Tick) as an error line and left the mover's engine
// holding the phantom.
bool ContainerMoveByOccupant(MpActor& actor, MpObjectReference& container)
{
  if (container.IsContainerOccupant(&actor)) {
    return true;
  }
  spdlog::warn("[verb:containers] E_CONTAINER_OCCUPANT: {:x} doesn't occupy "
               "{:x}; the move refused, the mover resynced",
               actor.GetFormId(), container.GetFormId());
  container.ResyncAfterRefusedMove(actor);
  return false;
}
}

void MpObjectReference::PutItem(MpActor& ac, const Inventory::Entry& e)
{
  CheckInteractionAbility(ac);
  if (!ContainerMoveByOccupant(ac, *this)) {
    return;
  }
  // the take's twin (npc-fate.md 1c): a put of what the mover's record does
  // not hold is refused by name, no throw, the mover resynced
  const auto held = ac.GetInventory().GetItemCount(e.baseId);
  if (held < e.count) {
    spdlog::warn("[verb:containers] E_PUT_NOT_HELD: {:x} puts {} of {:x} "
                 "into {:x} but its record holds {}; refused, the mover "
                 "resynced",
                 ac.GetFormId(), e.count, e.baseId, GetFormId(), held);
    ResyncAfterRefusedMove(ac);
    return;
  }

  PutItemEvent putItemEvent(&ac, this, e);
  try {
    putItemEvent.Fire(GetParent());
  } catch (...) {
    ResyncAfterRefusedMove(ac);
    throw;
  }
}

void MpObjectReference::TakeItem(MpActor& ac, const Inventory::Entry& e)
{
  CheckInteractionAbility(ac);
  if (!ContainerMoveByOccupant(ac, *this)) {
    return;
  }
  // thuum docs/verbs/npc-fate.md (playtest 44's 1c, the longbow): the
  // mover's engine moved an item the server's record never had (a
  // late-fated actor's own container roll in its loot window), RemoveItems
  // threw into the tick's catch with no code and the engine kept the
  // phantom. The record is the truth: a take the record cannot cover is
  // refused by name and the mover hears both inventories again (rule 5)
  const auto held = GetInventory().GetItemCount(e.baseId);
  if (held < e.count) {
    spdlog::warn("[verb:containers] E_TAKE_NOT_IN_RECORD: {:x} takes {} of "
                 "{:x} from {:x}, whose record holds {}; refused, the mover "
                 "resynced",
                 ac.GetFormId(), e.count, e.baseId, GetFormId(), held);
    ResyncAfterRefusedMove(ac);
    return;
  }

  TakeItemEvent takeItemEvent(&ac, this, e);
  try {
    takeItemEvent.Fire(GetParent());
  } catch (...) {
    ResyncAfterRefusedMove(ac);
    throw;
  }
}

// thuum memo D52 (playtest 37, 23:32Z): two players on one stack, the loser's
// menu a redraw behind: its take of a row the server's chest no longer has
// is refused here, but the engine's own take already moved the item into
// its hand, a phantom it then equips and the server rejects (rotfern's bow
// 3b562, then her scripts stopped). The refused mover hears the truth at
// once: its own inventory (SetInventory) and the container's, so its engine
// reverts both before anything equips
void MpObjectReference::ResyncAfterRefusedMove(MpActor& ac)
{
  // sent at once, not deferred: the engine's own move is already done and
  // the next frame may equip the phantom (pipeline 3580's T0 read the
  // deferred SetInventory nowhere)
  SetInventoryMessage own;
  own.inventory = ac.GetInventory();
  ac.SendToUser(own, true);
  // a living actor's inventory is its owner's alone (VisitProperties sends
  // it in mode All only); only a container's or a corpse's record is the
  // mover's to hear, else a forged move naming another player within reach
  // would read that player's inventory (rule 5; the hold's fresh review)
  if (baseType != "CONT" && !IsCorpse()) {
    return;
  }
  ac.GetActorToSendTo().SendToUser(
    CreatePropertyMessage_(this, "inventory", GetInventory().ToJson().dump()),
    true);
}

// thuum docs/verbs/containers.md (ADR-067 decision 6, Eli after playtest 43:
// "wrap the exclusive container hold"): a container or a corpse holds the one
// player who opened it, and every other player's activation is refused
// while it does (ProcessActivateNormal). The set is memo D52's (playtest 37's
// shared view, every opener in it, the record redrawn under the others'
// menus), kept as the set it was and holding one; its sinks and the prune
// are the hold's releases beside the holder's own close.
bool MpObjectReference::IsContainerOccupant(const MpActor* actor) const
{
  return actor && containerOccupants.count(const_cast<MpActor*>(actor)) > 0;
}

void MpObjectReference::AddContainerOccupant(MpActor& actor)
{
  if (!containerOccupants.insert(&actor).second) {
    return;
  }
  auto destroySink =
    std::make_shared<OccupantDestroyEventSink>(*GetParent(), this);
  auto disableSink =
    std::make_shared<OccupantDisableEventSink>(*GetParent(), this);
  actor.AddEventSink(destroySink);
  actor.AddEventSink(disableSink);
  containerOccupantSinks[&actor] = { destroySink, disableSink };
  actor.MarkContainerOccupied(this, true);
}

void MpObjectReference::RemoveContainerOccupant(MpActor& actor)
{
  containerOccupants.erase(&actor);
  actor.MarkContainerOccupied(this, false);
  const auto it = containerOccupantSinks.find(&actor);
  if (it != containerOccupantSinks.end()) {
    actor.RemoveEventSink(it->second.first);
    actor.RemoveEventSink(it->second.second);
    containerOccupantSinks.erase(it);
  }
  if (containerOccupants.empty()) {
    SetOpen(false);
  }
}

// an occupant disabled, gone to another cell or out of reach has no menu
// open here any more (the engine closes it as the player walks away)
void MpObjectReference::PruneContainerOccupants(float reach)
{
  std::vector<MpActor*> gone;
  for (auto* a : containerOccupants) {
    const bool far = (a->GetPos() - GetPos()).SqrLength() > reach * reach;
    if (a->IsDisabled() || a->GetCellOrWorld() != GetCellOrWorld() || far) {
      gone.push_back(a);
    }
  }
  for (auto* a : gone) {
    RemoveContainerOccupant(*a);
  }
}

void MpObjectReference::SetRelootTime(
  std::chrono::system_clock::duration newRelootTime)
{
  relootTimeOverride = newRelootTime;
}

void MpObjectReference::SetChanceNoneOverride(uint8_t newChanceNone)
{
  chanceNoneOverride.reset(new uint8_t(newChanceNone));
}

void MpObjectReference::SetCellOrWorld(const FormDesc& newWorldOrCell)
{
  SetCellOrWorldObsolete(newWorldOrCell);
  ForceSubscriptionsUpdate();
}

void MpObjectReference::SetActivationBlocked(bool blocked)
{
  // TODO: Save
  activationBlocked = blocked;
}

void MpObjectReference::ForceSubscriptionsUpdate()
{
  auto worldState = GetParent();
  if (!worldState || (IsDisabled() && !KeepsGridWhileDisabled())) {
    return;
  }
  InitListenersAndEmitters();

  auto worldOrCell = GetCellOrWorld().ToFormId(worldState->espmFiles);

  auto& gridInfo = worldState->grids[worldOrCell];
  MoveOnGrid(*gridInfo.grid);

  auto& was = *this->listeners;
  auto pos = GetGridPos(GetPos());
  const auto& neighbors =
    worldState->GetNeighborsByPosition(worldOrCell, pos.first, pos.second);
  // thuum docs/verbs/npc-handoff.md D6: an actor's simulator stays its
  // listener where the grid would drop it (a follower two squares away, or
  // outside the interior its actor walked into), else the follow's host
  // would get a DestroyActor for the actor its engine runs
  std::set<MpObjectReference*> now = neighbors;
  if (AsActor()) {
    if (MpActor* simulator = worldState->HostOf(*this)) {
      now.insert(simulator);
    }
  }

  std::vector<MpObjectReference*> toRemove;
  std::set_difference(was.begin(), was.end(), now.begin(), now.end(),
                      std::inserter(toRemove, toRemove.begin()));
  for (auto listener : toRemove) {
    Unsubscribe(this, listener);
    // Unsubscribe from self is NEEDED. See comment below
    if (this != listener)
      Unsubscribe(listener, this);
  }

  std::vector<MpObjectReference*> toAdd;
  std::set_difference(now.begin(), now.end(), was.begin(), was.end(),
                      std::inserter(toAdd, toAdd.begin()));
  for (auto listener : toAdd) {
    Subscribe(this, listener);
    // Note: Self-subscription is OK this check is performed as we don't want
    // to self-subscribe twice! We have already been subscribed to self in
    // the last line of code
    if (this != listener)
      Subscribe(listener, this);
  }

  everSubscribedOrListened = true;
}

void MpObjectReference::SetPrimitive(const NiPoint3& boundsDiv2,
                                     uint32_t primitiveType)
{
  auto vertices = Primitive::GetVertices(GetPos(), GetAngle(), boundsDiv2);
  pImpl->primitive =
    PrimitiveData{ boundsDiv2, Primitive::CreateGeoPolygonProc(vertices),
                   primitiveType };
}

// thuum docs/verbs/cell-host.md: the host's own game sees this actor as its
// own; every other game, and every game while `newHosterId` is nobody (0, an
// unhosted cell, frozen at its record), sees it hosted by other
void MpObjectReference::UpdateHoster(uint32_t newHosterId)
{
  // thuum docs/verbs/npc-puppet.md: an actor from the game files travels
  // under its own id, and since the client binds a view to it the property
  // under that id reaches it (2026-10-22; it did not reach the copy of
  // playtest thirty, which is why the flip was once withheld here)
  auto hostedMsg = CreatePropertyMessage_(this, "isHostedByOther", "true");
  auto notHostedMsg = CreatePropertyMessage_(this, "isHostedByOther", "false");
  for (auto listener : this->GetActorListeners()) {
    // the word is a game's, so only a listener with a user hears it, and
    // directly: GetActorToSendTo would hand an offline listener's word (a
    // game-file or server-made NPC in reach, a leftover actor; never the
    // hoster, so "true") to its cell's host, telling the host's own game
    // "hosted by other" about an actor it runs, in whatever order the
    // listener set iterates (unit CellHostTest's stray "true", pipelines
    // 2594 and 2602; thuum docs/verbs/npc-puppet.md)
    if (listener->GetUserId() == Networking::InvalidUserId) {
      continue;
    }
    if (newHosterId != 0 && newHosterId == listener->GetFormId()) {
      listener->SendToUser(notHostedMsg, true);
    } else {
      listener->SendToUser(hostedMsg, true);
    }
  }
}

// thuum docs/verbs/npc-handoff.md (d) through a door (ADR-047's note of
// 2026-10-28): the word for a game the record's move is about to unsubscribe
// (the old host outside the interior its actor just entered), sent before
// the grid forgets it so its engine demotes the copy it walked through;
// UpdateHoster reaches the listeners the move leaves (CellHostTest's door
// case, pipeline 3532: the old host never heard "hosted by other")
void MpObjectReference::TellHostedByOther(MpActor& game)
{
  if (game.GetUserId() == Networking::InvalidUserId) {
    return;
  }
  game.SendToUser(CreatePropertyMessage_(this, "isHostedByOther", "true"),
                  true);
}

void MpObjectReference::SetPropertyValueDump(const std::string& propertyName,
                                             const std::string& valueDump,
                                             bool isVisibleByOwner,
                                             bool isVisibleByNeighbor)
{
  auto msg = CreatePropertyMessage_(this, propertyName.c_str(), valueDump);
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.dynamicFields.SetValueDump(propertyName, valueDump);
  });
  if (isVisibleByNeighbor) {
    SendMessageToActorListeners(msg, true);
  } else if (isVisibleByOwner) {
    if (auto ac = AsActor()) {
      ac->GetActorToSendTo().SendToUser(msg, true);
    }
  }
  pImpl->setPropertyCalled = true;
}

void MpObjectReference::SetTeleportFlag(bool value)
{
  pImpl->teleportFlag = value;
}

void MpObjectReference::SetPosAndAngleSilent(const NiPoint3& pos,
                                             const NiPoint3& rot)
{
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) {
      changeForm.position = pos;
      changeForm.angle = rot;
    },
    Mode::NoRequestSave);
}

void MpObjectReference::Delete()
{
  if (IsEspmForm()) {
    spdlog::warn("MpObjectReference::Delete {:x} - Attempt to delete non-FF "
                 "object, ignoring",
                 GetFormId());
    return;
  }

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.isDeleted = true; });
  RemoveFromGridAndUnsubscribeAll();
}

void MpObjectReference::SetCount(uint32_t newCount)
{
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.count = newCount; });
}

void MpObjectReference::SetAnimationVariableBool(
  AnimationVariableBool animationVariableBool, bool value)
{
  auto i = static_cast<size_t>(animationVariableBool);
  pImpl->animGraphHolder.boolVariables[i] = value;
}

void MpObjectReference::SetInventory(const Inventory& inv)
{
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.baseContainerAdded = true;
    changeForm.inv = inv;
  });
  SendInventoryUpdate();
}

void MpObjectReference::AddItem(uint32_t baseId, uint32_t count)
{
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.baseContainerAdded = true;
    changeForm.inv.AddItem(baseId, count);
  });
  SendInventoryUpdate();

  if (auto worldState = GetParent(); worldState->HasEspm()) {
    espm::LookupResult lookupRes =
      worldState->GetEspm().GetBrowser().LookupById(baseId);

    if (lookupRes.rec) {
      auto baseItem = VarValue(std::make_shared<EspmGameObject>(lookupRes));
      auto itemCount = VarValue(static_cast<int32_t>(count));
      auto itemReference =
        VarValue(static_cast<IGameObject*>(nullptr)); // TODO
      auto sourceContainer =
        VarValue(static_cast<IGameObject*>(nullptr)); // TODO
      VarValue args[4] = { baseItem, itemCount, itemReference,
                           sourceContainer };
      SendPapyrusEvent("OnItemAdded", args, 4);
    } else {
      spdlog::warn("MpObjectReference::AddItem - failed to lookup item {:x}",
                   baseId);
    }
  }
}

void MpObjectReference::AddItems(const std::vector<Inventory::Entry>& entries)
{
  if (entries.size() > 0) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.baseContainerAdded = true;
      changeForm.inv.AddItems(entries);
    });
    SendInventoryUpdate();
  }

  // for (const auto& entri : entries) {
  //   auto baseItem = VarValue(static_cast<int32_t>(entri.baseId));
  //   auto itemCount = VarValue(static_cast<int32_t>(entri.count));
  //   auto itemReference = VarValue((IGameObject*)nullptr);
  //   auto sourceContainer = VarValue((IGameObject*)nullptr);
  //   VarValue args[4] = { baseItem, itemCount, itemReference,
  //   sourceContainer
  //   }; SendPapyrusEvent("OnItemAdded", args, 4);
  // }
}

void MpObjectReference::RemoveItem(uint32_t baseId, uint32_t count,
                                   MpObjectReference* target)
{
  RemoveItems({ { baseId, count } }, target);
}

void MpObjectReference::RemoveItems(
  const std::vector<Inventory::Entry>& entries, MpObjectReference* target)
{
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.inv.RemoveItems(entries);
  });

  if (target)
    target->AddItems(entries);

  SendInventoryUpdate();

  if (GetBaseType() == "CONT") {
    if (GetInventory().IsEmpty()) {
      spdlog::info("MpObjectReference::RemoveItems - {:x} requesting reloot",
                   this->GetFormId());
      RequestReloot();
    }
  }
}

void MpObjectReference::RemoveAllItems(MpObjectReference* target)
{
  auto prevInv = GetInventory();
  RemoveItems(prevInv.entries, target);
}

void MpObjectReference::RelootContainer()
{
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) {
      changeForm.baseContainerAdded = false;
    },
    Mode::NoRequestSave);
  EnsureBaseContainerAdded(*GetParent()->espm);
}

void MpObjectReference::RegisterProfileId(int32_t profileId)
{
  auto worldState = GetParent();
  if (!worldState) {
    throw std::runtime_error("Not attached to WorldState");
  }

  if (profileId < 0) {
    throw std::runtime_error(
      "Negative profileId passed to RegisterProfileId, should be >= 0");
  }

  auto currentProfileId = ChangeForm().profileId;
  auto formId = GetFormId();

  if (currentProfileId > 0) {
    worldState->actorIdByProfileId[currentProfileId].erase(formId);
  }

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.profileId = profileId; });

  if (profileId > 0) {
    worldState->actorIdByProfileId[profileId].insert(formId);
  }
}

void MpObjectReference::RegisterPrivateIndexedProperty(
  const std::string& propertyName, const std::string& propertyValueStringified)
{
  auto worldState = GetParent();
  if (!worldState) {
    throw std::runtime_error("Not attached to WorldState");
  }

  bool (*isNull)(const std::string&) = [](const std::string& s) {
    return s == "null" || s == "undefined" || s == "" || s == "''" ||
      s == "\"\"";
  };

  auto currentValueStringified =
    ChangeForm().dynamicFields.GetValueDump(propertyName);
  auto formId = GetFormId();
  if (!isNull(currentValueStringified)) {
    auto key = worldState->MakePrivateIndexedPropertyMapKey(
      propertyName, currentValueStringified);
    worldState->actorIdByPrivateIndexedProperty[key].erase(formId);
    spdlog::trace("MpObjectReference::RegisterPrivateIndexedProperty {:x} - "
                  "unregister {}",
                  formId, key);
  }

  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.dynamicFields.SetValueDump(propertyName,
                                          propertyValueStringified);
  });

  if (!isNull(propertyValueStringified)) {
    auto key = worldState->MakePrivateIndexedPropertyMapKey(
      propertyName, propertyValueStringified);
    worldState->actorIdByPrivateIndexedProperty[key].insert(formId);
    spdlog::trace(
      "MpObjectReference::RegisterPrivateIndexedProperty {:x} - register {}",
      formId, key);
  }
}

void MpObjectReference::Subscribe(MpObjectReference* emitter,
                                  MpObjectReference* listener)
{
  auto actorEmitter = emitter->AsActor();
  auto actorListener = listener->AsActor();
  if (!actorEmitter && !actorListener) {
    return;
  }

  if (!emitter->pImpl->onInitEventSent &&
      listener->GetChangeForm().profileId != -1) {
    emitter->pImpl->onInitEventSent = true;
    emitter->SendPapyrusEvent("OnInit");
    emitter->SendPapyrusEvent("OnCellLoad");
    emitter->SendPapyrusEvent("OnLoad");
  }

  const bool hasPrimitive = emitter->HasPrimitive();

  emitter->InitListenersAndEmitters();
  listener->InitListenersAndEmitters();

  auto [it, inserted] = emitter->listeners->insert(listener);

  if (actorListener && inserted) {
    emitter->actorListenerArray.push_back(actorListener);
  }

  listener->emitters->insert(emitter);

  if (!hasPrimitive) {
    emitter->callbacks->subscribe(emitter, listener);
    // thuum docs/verbs/magic-effects.md: a game that first sees an actor
    // hears the effects already running on it, after the actor's creation
    if (actorEmitter && actorListener && actorEmitter != actorListener) {
      actorEmitter->SendMagicEffectsTo(*actorListener);
    }
  }

  if (hasPrimitive) {
    if (!listener->emittersWithPrimitives) {
      listener->emittersWithPrimitives.reset(
        new std::map<MpObjectReference*, bool>);
    }
    listener->emittersWithPrimitives->insert({ emitter, false });
    // thuum ADR-068 rung 4 (b-ref-state-restart 20261107-211516, step 53): a
    // primitive gets no create word, so the records a create word carries
    // (its ref state, its destruction, its scripts' record) are served at
    // its new subscription instead (PartOne, WorldState's
    // onPrimitiveSubscribe): the Temple's setStage trigger 0x3ac73, disabled
    // with refStateSeq 1, reached no late joiner
    if (auto* worldState = emitter->GetParent(); worldState &&
        worldState->onPrimitiveSubscribe && inserted && actorListener &&
        !actorEmitter) {
      worldState->onPrimitiveSubscribe(*emitter, *actorListener);
    }
  }
}

void MpObjectReference::Unsubscribe(MpObjectReference* emitter,
                                    MpObjectReference* listener)
{
  auto actorEmitter = emitter->AsActor();
  auto actorListener = listener->AsActor();
  bool bothNonActors = !actorEmitter && !actorListener;
  if (bothNonActors) {
    return;
  }

  const bool hasPrimitive = emitter->HasPrimitive();

  if (!hasPrimitive) {
    emitter->callbacks->unsubscribe(emitter, listener);
  }

  size_t numElementsErased = emitter->listeners->erase(listener);

  if (actorListener && numElementsErased > 0) {
    emitter->actorListenerArray.erase(
      std::remove(emitter->actorListenerArray.begin(),
                  emitter->actorListenerArray.end(), actorListener),
      emitter->actorListenerArray.end());
  }

  listener->emitters->erase(emitter);

  if (listener->emittersWithPrimitives && hasPrimitive) {
    listener->emittersWithPrimitives->erase(emitter);
  }
}

void MpObjectReference::SetLastAnimation(const std::string& lastAnimation)
{
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.lastAnimation = lastAnimation;
  });
}

std::optional<std::string> MpObjectReference::GetLastAnimation() const
{
  return ChangeForm().lastAnimation;
}

std::vector<MpChangeFormREFR::GraphVariable>
MpObjectReference::GetGraphVariables() const
{
  const auto& variables = ChangeForm().graphVariables;
  return variables ? *variables
                   : std::vector<MpChangeFormREFR::GraphVariable>();
}

namespace {
// one graph variable's name against another's without case, as the engine's
// BSFixedString compares a string (CommonLibSSE-NG
// include/RE/B/BSFixedString.h: its strncmp over _strnicmp), so the engine's
// one variable is kept once whatever case each call spelled it in
bool SameVariableName(const std::string& lhs, const std::string& rhs)
{
  return lhs.size() == rhs.size() &&
    std::equal(lhs.begin(), lhs.end(), rhs.begin(), [](char a, char b) {
           return std::tolower(static_cast<unsigned char>(a)) ==
             std::tolower(static_cast<unsigned char>(b));
         });
}
}

bool MpObjectReference::KeepGraphVariable(
  MpChangeFormREFR::GraphVariable variable)
{
  auto kept = GetGraphVariables();
  std::vector<skymp::rules::GraphVariableKept> slots;
  slots.reserve(kept.size());
  for (const auto& entry : kept) {
    slots.push_back(
      { SameVariableName(entry.name, variable.name), entry.seq });
  }
  const auto slot = skymp::rules::graph_variable_slot(
    rust::Slice<const skymp::rules::GraphVariableKept>(slots.data(),
                                                       slots.size()),
    variable.seq);
  if (slot.code == 0 && slot.index < kept.size()) {
    kept[slot.index] = std::move(variable);
  } else if (slot.code == 1) {
    kept.push_back(std::move(variable));
  } else {
    return false;
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.graphVariables = std::move(kept);
  });
  return true;
}

void MpObjectReference::SetNodeTextureSet(const std::string& node,
                                          const espm::LookupResult& textureSet,
                                          bool firstPerson)
{
  // This only changes var in database, SpSnippet is being sent in
  // PapyrusNetImmerse.cpp
  EditChangeForm([&](MpChangeForm& changeForm) {
    if (changeForm.setNodeTextureSet == std::nullopt) {
      changeForm.setNodeTextureSet = std::map<std::string, std::string>();
    }

    uint32_t textureSetId = textureSet.ToGlobalId(textureSet.rec->GetId());

    FormDesc textureSetFormDesc =
      FormDesc::FromFormId(textureSetId, GetParent()->espmFiles);

    changeForm.setNodeTextureSet->insert_or_assign(
      node, textureSetFormDesc.ToString());
  });
}

void MpObjectReference::SetNodeScale(const std::string& node, float scale,
                                     bool firstPerson)
{
  // This only changes var in database, SpSnippet is being sent in
  // PapyrusNetImmerse.cpp
  EditChangeForm([&](MpChangeForm& changeForm) {
    if (changeForm.setNodeScale == std::nullopt) {
      changeForm.setNodeScale = std::map<std::string, float>();
    }
    changeForm.setNodeScale->insert_or_assign(node, scale);
  });
}

void MpObjectReference::SetDisplayName(
  const std::optional<std::string>& newName)
{
  EditChangeForm(
    [&](MpChangeForm& changeForm) { changeForm.displayName = newName; });
}

const std::set<MpObjectReference*>& MpObjectReference::GetListeners() const
{
  static const std::set<MpObjectReference*> kEmptyListeners;
  return listeners ? *listeners : kEmptyListeners;
}

const std::vector<MpActor*>& MpObjectReference::GetActorListeners()
  const noexcept
{
  return actorListenerArray;
}

const std::set<MpObjectReference*>& MpObjectReference::GetEmitters() const
{
  static const std::set<MpObjectReference*> kEmptyEmitters;
  return emitters ? *emitters : kEmptyEmitters;
}

void MpObjectReference::RequestReloot(
  std::optional<std::chrono::system_clock::duration> time)
{
  if (!IsEspmForm()) {
    return;
  }

  if (GetParent()->IsRelootForbidden(baseType)) {
    return;
  }

  if (!time)
    time = GetRelootTime();

  if (!ChangeForm().nextRelootDatetime) {
    // thuum docs/verbs/containers.md: the record keeps the moment asked for,
    // not a full window from now; ApplyChangeForm asks for the time left at
    // a restart, and the full window here pushed every reloot a window later
    // per restart (M4's row, "the reloot timer that each restart pushes
    // later")
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.nextRelootDatetime = std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now() + *time);
    });

    GetParent()->RequestReloot(*this, *time);
  }
}

void MpObjectReference::DoReloot()
{
  if (ChangeForm().nextRelootDatetime) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.nextRelootDatetime = 0;
    });
    SetOpen(false);
    SetHarvested(false);
    RelootContainer();
  }
}

std::shared_ptr<std::chrono::time_point<std::chrono::system_clock>>
MpObjectReference::GetNextRelootMoment() const
{
  std::shared_ptr<std::chrono::time_point<std::chrono::system_clock>> res;
  if (ChangeForm().nextRelootDatetime) {
    res.reset(new std::chrono::time_point<std::chrono::system_clock>(
      std::chrono::system_clock::from_time_t(
        ChangeForm().nextRelootDatetime)));
  }
  return res;
}

MpChangeForm MpObjectReference::GetChangeForm() const
{
  MpChangeForm res = ChangeForm();

  if (GetParent() && !GetParent()->espmFiles.empty()) {
    res.formDesc = FormDesc::FromFormId(GetFormId(), GetParent()->espmFiles);
    res.baseDesc = FormDesc::FromFormId(GetBaseId(), GetParent()->espmFiles);
  } else {
    res.formDesc = res.baseDesc = FormDesc(GetFormId(), "");
  }

  return res;
}

void MpObjectReference::ApplyChangeForm(const MpChangeForm& changeForm)
{
  ANTIGO_CONTEXT_INIT(ctx);

  if (pImpl->setPropertyCalled) {
    GetParent()->logger->critical(
      "ApplyChangeForm called after SetProperty\n{}",
      ctx.Resolve().ToString());
    // std::terminate();
  }

  blockSaving = true;
  Viet::ScopedTask<MpObjectReference> unblockTask(
    [](MpObjectReference& self) { self.blockSaving = false; }, *this);

  const auto currentBaseId = GetBaseId();
  const auto newBaseId = changeForm.baseDesc.ToFormId(GetParent()->espmFiles);
  if (currentBaseId != newBaseId) {
    spdlog::error("Anomaly, baseId should never change ({:x} => {:x})",
                  currentBaseId, newBaseId);
  }

  if (ChangeForm().formDesc != changeForm.formDesc) {
    throw std::runtime_error("Expected formDesc to be " +
                             ChangeForm().formDesc.ToString() +
                             ", but found " + changeForm.formDesc.ToString());
  }

  if (changeForm.profileId >= 0) {
    RegisterProfileId(changeForm.profileId);
  }

  changeForm.dynamicFields.ForEachValueDump(
    [&](const std::string& propertyName, const std::string& valueDump) {
      static const std::string kPrefix = GetPropertyPrefixPrivateIndexed();
      bool startsWith = propertyName.compare(0, kPrefix.size(), kPrefix) == 0;
      if (startsWith) {
        RegisterPrivateIndexedProperty(propertyName, valueDump);
      }
    });

  // See https://github.com/skyrim-multiplayer/issue-tracker/issues/42
  EditChangeForm(
    [&](MpChangeFormREFR& f) {
      f = changeForm;

      // Fix: RequestReloot doesn't work with non-zero 'nextRelootDatetime'
      f.nextRelootDatetime = 0;

      // thuum docs/verbs/containers.md: a container saved open has nobody in
      // it after a load (the occupant set is the session's), so it comes
      // back shut; a door keeps the state its record says
      if (f.isOpen && baseType == "CONT") {
        f.isOpen = false;
        spdlog::info("[verb:containers] {:x} saved open came back shut at "
                     "load",
                     GetFormId());
      }
    },
    Mode::NoRequestSave);
  if (changeForm.nextRelootDatetime) {
    auto tp =
      std::chrono::system_clock::from_time_t(changeForm.nextRelootDatetime);

    // Fix: Handle situations when the server is stopped at the 'tp' moment
    if (tp < std::chrono::system_clock::now()) {
      tp = std::chrono::system_clock::now() + std::chrono::milliseconds(1);
    }

    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      tp - std::chrono::system_clock::now());
    RequestReloot(ms);
  }

  // Perform all required grid operations
  // Mirrors MpActor impl
  if (!AsActor()) {
    changeForm.isDisabled ? Disable() : Enable();
    SetCellOrWorldObsolete(changeForm.worldOrCellDesc);
    SetPos(changeForm.position);
  }
}

const DynamicFields& MpObjectReference::GetDynamicFields() const
{
  return ChangeForm().dynamicFields;
}

std::vector<QuestStageDone> MpObjectReference::GetQuestStages() const
{
  const auto& stages = ChangeForm().questStages;
  return stages ? *stages : std::vector<QuestStageDone>();
}

bool MpObjectReference::SetQuestStages(std::vector<QuestStageDone> stages)
{
  if (stages == GetQuestStages()) {
    return false;
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.questStages = std::move(stages);
  });
  return true;
}

std::vector<MpChangeFormREFR::AliasFill> MpObjectReference::GetAliasFills()
  const
{
  const auto& fills = ChangeForm().aliasFills;
  return fills ? *fills : std::vector<MpChangeFormREFR::AliasFill>();
}

bool MpObjectReference::SetAliasFills(
  std::vector<MpChangeFormREFR::AliasFill> fills)
{
  if (fills == GetAliasFills()) {
    return false;
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.aliasFills = std::move(fills);
  });
  return true;
}

std::vector<PlayerGlobal> MpObjectReference::GetWorldGlobals() const
{
  const auto& globals = ChangeForm().worldGlobals;
  return globals ? *globals : std::vector<PlayerGlobal>();
}

bool MpObjectReference::SetWorldGlobals(std::vector<PlayerGlobal> globals)
{
  if (globals == GetWorldGlobals()) {
    return false;
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.worldGlobals = std::move(globals);
  });
  return true;
}

std::vector<MpChangeFormREFR::QuestRun> MpObjectReference::GetQuestRuns() const
{
  const auto& runs = ChangeForm().questRuns;
  return runs ? *runs : std::vector<MpChangeFormREFR::QuestRun>();
}

bool MpObjectReference::SetQuestRuns(
  std::vector<MpChangeFormREFR::QuestRun> runs)
{
  if (runs == GetQuestRuns()) {
    return false;
  }
  EditChangeForm(
    [&](MpChangeForm& changeForm) { changeForm.questRuns = std::move(runs); });
  return true;
}

std::vector<MpChangeFormREFR::ScriptVariable>
MpObjectReference::GetScriptVariables() const
{
  const auto& variables = ChangeForm().scriptVariables;
  return variables ? *variables
                   : std::vector<MpChangeFormREFR::ScriptVariable>();
}

bool MpObjectReference::SetScriptVariables(
  std::vector<MpChangeFormREFR::ScriptVariable> variables)
{
  if (variables == GetScriptVariables()) {
    return false;
  }
  EditChangeForm([&](MpChangeForm& changeForm) {
    changeForm.scriptVariables = std::move(variables);
  });
  return true;
}

std::optional<FormDesc> MpObjectReference::GetPlacedBySpell() const
{
  return ChangeForm().placedBySpell;
}

std::optional<uint32_t> MpObjectReference::GetRefStateSeq() const
{
  return ChangeForm().refStateSeq;
}

void MpObjectReference::SetRefState(bool disabled, bool deleted, uint32_t seq)
{
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.isDeleted = deleted;
    changeForm.refStateSeq = seq;
  });
  if (disabled) {
    Disable();
  } else {
    Enable();
  }
  // the stamp above may name the sequence now; the record's is the call's
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.refStateSeq = seq; });
  if (!AsActor()) {
    ForceSubscriptionsUpdate();
  }
}

bool MpObjectReference::KeepsGridWhileDisabled() const
{
  return IsEspmForm() && !AsActor() && ChangeForm().refStateSeq.has_value();
}

std::optional<MpChangeFormREFR::PlacedByScript>
MpObjectReference::GetPlacedByScript() const
{
  return ChangeForm().placedByScript;
}

void MpObjectReference::SetPlacedByScript(const FormDesc& self,
                                          const FormDesc& base)
{
  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.placedByScript = MpChangeFormREFR::PlacedByScript{ self, base };
  });
}

void MpObjectReference::MarkDisabledByFiles()
{
  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) { changeForm.isDisabled = true; },
    Mode::NoRequestSave);
}

void MpObjectReference::SetPlacedBySpell(const FormDesc& spell)
{
  if (ChangeForm().placedBySpell == spell) {
    return;
  }
  EditChangeForm(
    [&](MpChangeForm& changeForm) { changeForm.placedBySpell = spell; });
}

void MpObjectReference::SetCellOrWorldObsolete(const FormDesc& newWorldOrCell)
{
  auto worldState = GetParent();
  if (!worldState) {
    return;
  }

  auto worldOrCell =
    ChangeForm().worldOrCellDesc.ToFormId(worldState->espmFiles);

  everSubscribedOrListened = false;
  auto gridIterator = worldState->grids.find(worldOrCell);
  if (gridIterator != worldState->grids.end()) {
    gridIterator->second.grid->Forget(this);
  }

  EditChangeForm([&](MpChangeFormREFR& changeForm) {
    changeForm.worldOrCellDesc = newWorldOrCell;
  });
}

void MpObjectReference::VisitNeighbours(const Visitor& visitor)
{
  if (IsDisabled()) {
    return;
  }

  auto worldState = GetParent();
  if (!worldState) {
    return;
  }

  auto worldOrCell =
    ChangeForm().worldOrCellDesc.ToFormId(worldState->espmFiles);

  auto gridIterator = worldState->grids.find(worldOrCell);
  if (gridIterator == worldState->grids.end()) {
    return;
  }

  auto& grid = gridIterator->second;
  auto pos = GetGridPos(GetPos());
  auto& neighbours =
    worldState->GetNeighborsByPosition(worldOrCell, pos.first, pos.second);
  for (auto neighbour : neighbours) {
    visitor(neighbour);
  }
}

void MpObjectReference::SendPapyrusEvent(const char* eventName,
                                         const VarValue* arguments,
                                         size_t argumentsCount)
{
  if (!pImpl->scriptsInited) {
    InitScripts();
    pImpl->scriptsInited = true;
  }
  return MpForm::SendPapyrusEvent(eventName, arguments, argumentsCount);
}

void MpObjectReference::Init(WorldState* parent, uint32_t formId,
                             bool hasChangeForm)
{
  MpForm::Init(parent, formId, hasChangeForm);

  // We should queue created form for saving as soon as it is initialized
  const auto mode = (!hasChangeForm && !IsEspmForm()) ? Mode::RequestSave
                                                      : Mode::NoRequestSave;

  EditChangeForm(
    [&](MpChangeFormREFR& changeForm) {
      changeForm.formDesc =
        FormDesc::FromFormId(formId, GetParent()->espmFiles);
    },
    mode);

  auto refrId = GetFormId();
  if (parent->HasEspm() && IsEspmForm() && !AsActor()) {
    auto lookupRes = parent->GetEspm().GetBrowser().LookupById(refrId);
    auto data = espm::GetData<espm::REFR>(refrId, parent);
    for (auto& info : data.activationParents) {
      auto activationParent = lookupRes.ToGlobalId(info.refrId);

      // Using WorldState for that, because we don't want search (potentially
      // load) other references during OnInit
      parent->activationChildsByActivationParent[activationParent].insert(
        { refrId, info.delay });
    }
    // thuum docs/DECISIONS.md ADR-059 decision 2: a link a script carries as a
    // property joins the same table, from the parent's side and with no
    // delay (a script's Activate is immediate). Bleak Falls Temple's puzzle
    // lever 0xaa8c3 carries defaultPillarPuzzleLever: refActOnSuccess01 and
    // refActOnFailure01..04 are the portcullis and the dart traps it fires,
    // puzzleDoorActivator the marker whose activation parents open the gate
    // (lab/esm.py's VMAD decode, 2026-11-01). So the host's report of that
    // script's Activate on them is a linked activation the server knows
    // (ActionListener::OnLinkedActivation), not E_TRAP_UNLINKED.
    if (lookupRes.rec) {
      espm::ScriptData scriptData;
      lookupRes.rec->GetScriptData(&scriptData, parent->GetEspmCache());
      const auto lower = [](const std::string& text) {
        std::string out = text;
        for (auto& c : out) {
          c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return out;
      };
      for (const auto& script : scriptData.scripts) {
        if (script.status == 3 ||
            Utils::stricmp(script.scriptName.data(),
                           "defaultPillarPuzzleLever") != 0) {
          continue;
        }
        for (const auto& property : script.properties) {
          if (property.type != espm::Property::Type::Object ||
              !property.value.formId) {
            continue;
          }
          const std::string name = lower(property.name);
          if (name.rfind("refacton", 0) != 0 &&
              name != "puzzledooractivator") {
            continue;
          }
          const auto child = lookupRes.ToGlobalId(property.value.formId);
          parent->activationChildsByActivationParent[refrId].insert(
            { child, 0.f });
        }
      }
    }
  }
}

void MpObjectReference::FlushUnsavedEdit()
{
  if (!HasUnsavedEdit()) {
    return;
  }
  EditChangeForm([](MpChangeForm&) {}, Mode::RequestSave);
}

bool MpObjectReference::IsLocationSavingNeeded() const
{
  auto last = GetLastSaveRequestMoment();
  return !last ||
    std::chrono::system_clock::now() - *last > std::chrono::seconds(30);
}

void MpObjectReference::GivePickupItemsToActivationSource(
  MpObjectReference& activationSource, const espm::LookupResult& base)
{
  auto& loader = GetParent()->GetEspm();
  auto& compressedFieldsCache = GetParent()->GetEspmCache();
  auto t = base.rec->GetType();

  auto mapping = loader.GetBrowser().GetCombMapping(base.fileIdx);
  uint32_t resultItem = 0;
  if (espm::utils::Is<espm::TREE>(t)) {
    auto data =
      espm::Convert<espm::TREE>(base.rec)->GetData(compressedFieldsCache);
    resultItem = espm::utils::GetMappedId(data.resultItem, *mapping);
  }

  if (espm::utils::Is<espm::FLOR>(t)) {
    auto data =
      espm::Convert<espm::FLOR>(base.rec)->GetData(compressedFieldsCache);
    resultItem = espm::utils::GetMappedId(data.resultItem, *mapping);
  }

  if (espm::utils::Is<espm::LIGH>(t)) {
    auto res =
      espm::Convert<espm::LIGH>(base.rec)->GetData(compressedFieldsCache);
    bool isTorch = res.data.flags & espm::LIGH::Flags::CanBeCarried;
    if (!isTorch) {
      return;
    }
    resultItem = espm::utils::GetMappedId(base.rec->GetId(), *mapping);
  }

  if (resultItem == 0) {
    resultItem = espm::utils::GetMappedId(base.rec->GetId(), *mapping);
  }

  auto resultItemLookupRes = loader.GetBrowser().LookupById(resultItem);
  auto leveledItem = espm::Convert<espm::LVLI>(resultItemLookupRes.rec);
  if (leveledItem) {
    const auto kCountMult = 1;
    auto map = LeveledListUtils::EvaluateListRecurse(
      loader.GetBrowser(), resultItemLookupRes, kCountMult,
      kPlayerCharacterLevel, chanceNoneOverride.get());
    for (auto& p : map) {
      activationSource.AddItem(p.first, p.second);
    }
  } else {
    auto refrRecord = espm::Convert<espm::REFR>(
      loader.GetBrowser().LookupById(GetFormId()).rec);

    uint32_t countRecord =
      refrRecord ? refrRecord->GetData(compressedFieldsCache).count : 1;

    uint32_t countChangeForm = ChangeForm().count;

    constexpr uint32_t kCountDefault = 1;

    uint32_t resultingCount =
      std::max(kCountDefault, std::max(countRecord, countChangeForm));

    activationSource.AddItem(resultItem, resultingCount);
  }
}

bool MpObjectReference::IsCorpse()
{
  // thuum docs/verbs/npc-fate.md (playtest 37, finding 62): a server-made
  // NPC, a 0xff id with an NPC base, is a corpse to the server as a
  // game-file actor is. The id's range stood here, so rotfern's E on the
  // dead necromancer ff00000a (sent twice) matched no branch of
  // ProcessActivateNormal and was dropped with no line. A player's actor is
  // the one dead actor that is no corpse (its death takes the respawn path),
  // told apart as the NPC rule is everywhere else: IsCreatedAsPlayer, a 0xff
  // id with the player base or none (MpActor.cpp).
  auto actor = AsActor();
  return actor && actor->IsDead() && !actor->IsCreatedAsPlayer();
}

void MpObjectReference::ProcessActivateNormal(
  MpObjectReference& activationSource, bool reportedByHost)
{
  auto actorActivator = activationSource.AsActor();

  auto worldState = GetParent();
  auto& loader = GetParent()->GetEspm();
  auto& compressedFieldsCache = GetParent()->GetEspmCache();

  auto base = loader.GetBrowser().LookupById(GetBaseId());
  if (!base.rec || !GetBaseId()) {
    return spdlog::error("MpObjectReference::ProcessActivate {:x} - doesn't "
                         "have base form, activationSource is {:x}",
                         GetFormId(), activationSource.GetFormId());
  }

  if (IsDeleted()) {
    return spdlog::warn("MpObjectReference::ProcessActivate {:x} - deleted "
                        "object, activationSource is {:x}",
                        GetFormId(), activationSource.GetFormId());
  }

  if (IsDisabled()) {
    return spdlog::warn("MpObjectReference::ProcessActivate {:x} - disabled "
                        "object, activationSource is {:x}",
                        GetFormId(), activationSource.GetFormId());
  }

  auto t = base.rec->GetType();

  // thuum docs/verbs/locks.md: a player's activation of a locked door or
  // container opens for its key alone (R0); without it the player's game is
  // told to run the engine's own activation on its locked reference (the
  // lockpicking menu, the "requires key" box), and the unlock comes back as
  // a LockReport. A host's report or a non-actor activator (a script, a
  // parent) passes the lock as the engine's own linked activations do.
  if (actorActivator && !reportedByHost) {
    if (const auto lock = GetLock(); lock && lock->locked) {
      skymp::rules::LockStateRow row{};
      row.level = lock->level;
      row.key = lock->key;
      row.locked = lock->locked;
      row.broken = lock->broken;
      const bool hasKey = lock->key != 0 &&
        actorActivator->GetInventory().GetItemCount(lock->key) > 0;
      const auto verdict = skymp::rules::lock_activation(row, hasKey);
      if (verdict.verdict == 1) {
        MpChangeFormREFR::Lock opened = *lock;
        opened.locked = false;
        spdlog::info("[verb:locks] {:x} opens {:x} with its key {:x}",
                     actorActivator->GetFormId(), GetFormId(), lock->key);
        SetLock(opened);
      } else if (verdict.verdict == 2) {
        spdlog::info("[verb:locks] E_ACTIVATE_LOCKED: {:x} activates {:x} "
                     "(level {}, key {:x}); the player's game is prompted",
                     actorActivator->GetFormId(), GetFormId(), lock->level,
                     lock->key);
        SendLockPrompt(*actorActivator);
        return;
      }
    }
  }

  bool pickable = espm::utils::Is<espm::TREE>(t) ||
    espm::utils::Is<espm::FLOR>(t) || espm::utils::IsItem(t);
  if (pickable && !IsHarvested()) {
    GivePickupItemsToActivationSource(activationSource, base);
    SetHarvested(true);
    RequestReloot();

    if ((espm::utils::Is<espm::TREE>(t) || espm::utils::Is<espm::FLOR>(t)) &&
        actorActivator) {
      // SendOpenContainer being used to activate the object
      // TODO: rename SendOpenContainer to SendActivate
      activationSource.SendOpenContainer(GetFormId());
    }

    if (espm::utils::IsItem(t) && !IsEspmForm()) {
      spdlog::info("MpObjectReference::ProcessActivate - Deleting 0xff item");
      Delete();
    }
  } else if (t == espm::DOOR::kType) {
    auto lookupRes = loader.GetBrowser().LookupById(GetFormId());
    auto refrRecord = espm::Convert<espm::REFR>(lookupRes.rec);
    auto teleport = refrRecord->GetData(compressedFieldsCache).teleport;
    if (teleport) {
      if (!IsOpen()) {
        SetOpen(true);
        RequestReloot();
      }

      auto destinationId = lookupRes.ToGlobalId(teleport->destinationDoor);
      auto destination = loader.GetBrowser().LookupById(destinationId);
      auto destinationRecord = espm::Convert<espm::REFR>(destination.rec);
      if (!destinationRecord) {
        throw std::runtime_error(
          "No destination found for this teleport door");
      }

      auto teleportWorldOrCell = destination.ToGlobalId(
        GetWorldOrCell(loader.GetBrowser(), destinationRecord));

      static const auto kPi = std::acos(-1.f);
      const auto& pos = teleport->pos;
      const float rot[] = { teleport->rotRadians[0] / kPi * 180,
                            teleport->rotRadians[1] / kPi * 180,
                            teleport->rotRadians[2] / kPi * 180 };

      // a game-file actor a host reported for (never a runtime actor, a
      // script's activation or a player's: those keep the transit below;
      // unit/PapyrusCompatibilityTest's BrokenOarGrotto door among them)
      MpActor* npcActivator = reportedByHost && actorActivator &&
          actorActivator->GetUserId() == Networking::InvalidUserId &&
          actorActivator->GetFormId() < 0xff000000
        ? actorActivator
        : nullptr;
      if (npcActivator) {
        // thuum docs/verbs/npc-handoff.md D6, the door (x-handoff-door v3,
        // 2026-10-23): a hosted NPC's door is its host engine's to walk
        // through. The client blocks every door's activation and reports
        // it, so without this reply the engine stood at the door while
        // the record was teleported indoors by the branch below, the record
        // left the host's grid, both seats demoted the actor, the roster
        // brought the record back out and the loop ran every 4 s. Now the
        // server judges the crossing (the move verdict: a cell nobody
        // hosts is a follow; another live host's cell a pass since Eli's
        // call of 2026-10-28, playtest 37, "let embry cross and hand him to
        // the inn's host": the record moves with the host's report and the
        // word flips to the destination's host, npc-handoff D9; a stranger's
        // actor is refused; any actor's door since memo D54 B of the same
        // day, a non-persistent resident's into her own house too) and asks
        // the host's game to perform the activation for the actor
        // (ObjectReference.Activate with default processing, the engine's
        // own door transit); the record follows the host's next report
        // through MayMoveActor. The door still swings for everyone.
        const NiPoint3 destPos{ pos[0], pos[1], pos[2] };
        const bool allowed = !worldState->npcDoorVerdict ||
          worldState->npcDoorVerdict(*npcActivator, teleportWorldOrCell,
                                     destPos);
        MpActor* host = worldState->HostOf(*npcActivator);
        if (allowed && host && host->IsCreatedAsPlayer()) {
          SpSnippetObjectArgument walker;
          walker.formId = npcActivator->GetFormId();
          walker.type = "ObjectReference";
          const std::vector<std::optional<
            std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
            args{ walker, true };
          SpSnippet("ObjectReference", "Activate", args, GetFormId())
            .Execute(host, SpSnippetMode::kNoReturnResult);
          spdlog::info("MpObjectReference::ProcessActivateNormal - "
                       "npc-handoff: door {:x}: {:x}'s host {:x} walks it "
                       "through to {:x}; the record follows its report",
                       GetFormId(), npcActivator->GetFormId(),
                       host->GetFormId(), teleportWorldOrCell);
        } else {
          spdlog::info("MpObjectReference::ProcessActivateNormal - "
                       "npc-handoff: door {:x}: {:x} may not cross into {:x} "
                       "({}); the actor stays",
                       GetFormId(), npcActivator->GetFormId(),
                       teleportWorldOrCell,
                       allowed ? "no host to walk it" : "refused");
        }
      } else {
        TeleportMessage msg;
        msg.idx = activationSource.GetIdx();
        std::copy(std::begin(pos), std::end(pos), msg.pos.begin());
        std::copy(std::begin(rot), std::end(rot), msg.rot.begin());
        msg.worldOrCell = teleportWorldOrCell;

        if (actorActivator) {
          actorActivator->GetActorToSendTo().SendToUser(msg, true);
        }

        activationSource.SetCellOrWorldObsolete(
          FormDesc::FromFormId(teleportWorldOrCell, worldState->espmFiles));
        activationSource.SetPos({ pos[0], pos[1], pos[2] });
        activationSource.SetAngle({ rot[0], rot[1], rot[2] });
      }

    } else {
      SetOpen(!IsOpen());
    }
  } else if (actorActivator && (t == espm::CONT::kType || IsCorpse())) {
    // thuum docs/verbs/npc-fate.md (the deaths): a dead NPC's body, a
    // game-file actor's or a server-made copy's (finding 62), is a
    // container, the CONT branch's own body (occupancy within reach, the
    // record's inventory, OpenContainer; TakeItem and PutItem on the
    // server); a live actor's activation stays the host's engine's
    EnsureBaseContainerAdded(loader);

    constexpr float kOccupationReach = 512.f;

    // a holder gone out of reach, disabled or to another cell has no menu
    // open here any more and leaves first, so a stale hold never outlives
    // its seat's distance
    PruneContainerOccupants(kOccupationReach);
    // thuum docs/verbs/containers.md (ADR-067 decision 6, Eli after playtest
    // 43: "wrap the exclusive container hold"): one seat holds a container,
    // a chest's or a corpse's, at a time; another seat's activation is
    // refused while it is held, and the holder's close or its leaving reach
    // (the prune above) frees it. With decision 1 (the contents sync at the
    // open and at the close, SetOpen) no two seats see one container's
    // contents change under them. The refused seat hears who holds it: the
    // holder's name as PartOne::GetActorName reads it (the appearance's),
    // "Someone" when it has none.
    if (!containerOccupants.empty() && !IsContainerOccupant(actorActivator)) {
      const MpActor* holder = *containerOccupants.begin();
      spdlog::info("[verb:containers] E_CONTAINER_BUSY: {:x} activates {:x} "
                   "held by {:x}; refused",
                   actorActivator->GetFormId(), GetFormId(),
                   holder->GetFormId());
      const auto appearance = holder->GetAppearance();
      const std::string text = appearance && !appearance->name.empty()
        ? appearance->name + " is searching this"
        : std::string("Someone is using this");
      const std::vector<std::optional<
        std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
        args{ text };
      SpSnippet("Debug", "Notification", args, 0)
        .Execute(actorActivator, SpSnippetMode::kNoReturnResult);
      return;
    }
    AddContainerOccupant(*actorActivator);
    SetOpen(true);
    actorActivator->GetActorToSendTo().SendToUser(
      CreatePropertyMessage_(this, "inventory",
                             GetInventory().ToJson().dump()),
      true);
    activationSource.SendOpenContainer(GetFormId());
  } else if (t == espm::ACTI::kType && actorActivator) {
    // SendOpenContainer being used to activate the object
    // TODO: rename SendOpenContainer to SendActivate
    activationSource.SendOpenContainer(GetFormId());
  } else if (t == "FURN" && actorActivator && IsSleepFurniture()) {
    // thuum docs/verbs/sleep.md: a vanilla bed opens the sleep menu and never
    // seats the player, so there is nothing to occupy; two players may sleep
    // in one bed, each in its own game. The player's next rest is a sleep if
    // it comes soon, at this bed.
    // SendOpenContainer being used to activate the object
    // TODO: rename SendOpenContainer to SendActivate
    activationSource.SendOpenContainer(GetFormId());
    actorActivator->SetLastBed(GetFormId(), std::chrono::steady_clock::now());
  } else if (t == "FURN" && actorActivator) {

    constexpr float kOccupationReach = 256.f;

    if (CheckIfObjectCanStartOccupyThis(activationSource, kOccupationReach)) {
      if (this->occupant) {
        this->occupant->RemoveEventSink(this->occupantDestroySink);
        this->occupant->RemoveEventSink(this->occupantDisableSink);
      }

      // SendOpenContainer being used to activate the object
      // TODO: rename SendOpenContainer to SendActivate
      activationSource.SendOpenContainer(GetFormId());

      this->occupant = actorActivator;

      this->occupantDestroySink.reset(
        new OccupantDestroyEventSink(*GetParent(), this));
      this->occupant->AddEventSink(this->occupantDestroySink);

      this->occupantDisableSink.reset(
        new OccupantDisableEventSink(*GetParent(), this));
      this->occupant->AddEventSink(this->occupantDisableSink);
    }
  }
}

bool MpObjectReference::ProcessActivateSecond(
  MpObjectReference& activationSource)
{
  auto actorActivator = activationSource.AsActor();

  auto worldState = GetParent();
  auto& loader = GetParent()->GetEspm();
  auto& compressedFieldsCache = GetParent()->GetEspmCache();

  auto base = loader.GetBrowser().LookupById(GetBaseId());
  if (!base.rec || !GetBaseId()) {
    spdlog::error("MpObjectReference::ProcessActivate {:x} - doesn't "
                  "have base form, activationSource is {:x}",
                  GetFormId(), activationSource.GetFormId());
    return false;
  }

  auto t = base.rec->GetType();

  // thuum docs/verbs/npc-fate.md (playtest 34, section 5): a corpse shares
  // the CONT branch's open (ProcessActivateNormal) and must share its close,
  // else its looter's closing activation fell through here, the occupancy
  // was never released, and every other player was refused ("occupant is
  // another object and is nearby") until the looter stood 512 units away
  if ((t == espm::CONT::kType || IsCorpse()) && actorActivator) {
    // memo D52: this player leaves the container; it shuts with the last
    if (IsContainerOccupant(actorActivator)) {
      RemoveContainerOccupant(*actorActivator);
      return true;
    }
  } else if (t == "FURN" && actorActivator) {
    if (this->occupant == &activationSource) {
      this->occupant->RemoveEventSink(this->occupantDestroySink);
      this->occupant = nullptr;
      return true;
    }
  }

  return false;
}

namespace {
// a game-file reference whose base is a STAT (a marker): nothing for the
// server to activate
bool IsStaticMarker(WorldState& worldState, uint32_t refrId)
{
  if (!worldState.HasEspm()) {
    return false;
  }
  const auto& browser = worldState.GetEspm().GetBrowser();
  const auto lookup = browser.LookupById(refrId);
  const auto* refr = espm::Convert<espm::REFR>(lookup.rec);
  if (!refr) {
    return false;
  }
  const auto data = refr->GetData(worldState.GetEspmCache());
  const auto base = browser.LookupById(lookup.ToGlobalId(data.baseId));
  return base.rec && base.rec->GetType() == "STAT";
}
}

void MpObjectReference::ActivateChilds()
{
  auto worldState = GetParent();
  if (!worldState) {
    return;
  }

  auto myFormId = GetFormId();

  for (auto& pair : worldState->activationChildsByActivationParent[myFormId]) {
    auto childRefrId = pair.first;
    auto delay = pair.second;

    auto delayMs = Viet::TimeUtils::To<std::chrono::milliseconds>(delay);
    worldState->SetTimer(delayMs).Then([worldState, childRefrId,
                                        myFormId](Viet::Void) {
      auto& childForm = worldState->LookupFormById(childRefrId);
      MpObjectReference* childRefr =
        childForm ? childForm->AsObjectReference() : nullptr;
      if (!childRefr) {
        // thuum playtest 47: a static marker child (an XMarker, a scene's
        // spot) is no object the server models and has nothing to activate,
        // so its absence is told at info; any other missing child stays a
        // warning (the puzzle lever 0xaa8c3's 0x3e728 logged one per pull)
        if (IsStaticMarker(*worldState, childRefrId)) {
          spdlog::info("MpObjectReference::ActivateChilds {:x} - activation "
                       "child {:x} is a static marker, nothing to activate",
                       myFormId, childRefrId);
          return;
        }
        spdlog::warn("MpObjectReference::ActivateChilds {:x} - Bad/missing "
                     "activation child {:x}",
                     myFormId, childRefrId);
        return;
      }

      // Not sure about activationSource and defaultProcessingOnly in this
      // case I'll try to keep vanilla scripts working
      childRefr->Activate(worldState->GetFormAt<MpObjectReference>(myFormId));
    });
  }
}

bool MpObjectReference::CheckIfObjectCanStartOccupyThis(
  MpObjectReference& activationSource, float occupationReach)
{
  if (!this->occupant) {
    spdlog::info("MpObjectReference::ProcessActivate {:x} - no occupant "
                 "(activationSource = {:x})",
                 GetFormId(), activationSource.GetFormId());
    return true;
  }

  if (this->occupant->IsDisabled()) {
    spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is "
                 "disabled (activationSource = {:x})",
                 GetFormId(), activationSource.GetFormId());
    return true;
  }

  // thuum playtest 47 (2026-11-06): a dead or deleted occupant uses nothing.
  // A host's NPC takes a furniture by its reported activation, and nothing
  // reports its leaving (an NPC's furniture exit is no activation), so Bleak
  // Falls Temple's puzzle lever 0xaa8c3 stayed its scene's bandit 0x39fe1's
  // after the darts killed him beside it: every player's pull was refused
  // here ("occupant is another object and is nearby"), the OpenContainer
  // reply never came, and the lever never animated though its script ran
  if (this->occupant->IsDead() || this->occupant->IsDeleted()) {
    spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is "
                 "dead or deleted (activationSource = {:x})",
                 GetFormId(), activationSource.GetFormId());
    return true;
  }

  auto& occupantPos = this->occupant->GetPos();
  auto distanceToOccupantSqr = (occupantPos - GetPos()).SqrLength();
  if (distanceToOccupantSqr > occupationReach * occupationReach) {
    spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is too "
                 "far away (activationSource = {:x})",
                 GetFormId(), activationSource.GetFormId());
    return true;
  }

  auto& occupantCellOrWorld = this->occupant->GetCellOrWorld();
  if (occupantCellOrWorld != GetCellOrWorld()) {
    spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is in "
                 "another cell/world (activationSource = {:x})",
                 GetFormId(), activationSource.GetFormId());
    return true;
  }

  if (this->occupant == &activationSource) {
    auto& loader = GetParent()->GetEspm();
    auto base = loader.GetBrowser().LookupById(GetBaseId());
    auto t = base.rec->GetType();
    auto actorActivator = activationSource.AsActor();
    if (t == "FURN" && actorActivator) {
      spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is "
                   "already this object (activationSource = {:x}). Blocking "
                   "because it's FURN",
                   GetFormId(), activationSource.GetFormId());
      return false;
    } else {
      spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is "
                   "already this object (activationSource = {:x})",
                   GetFormId(), activationSource.GetFormId());
      return true;
    }
  }

  spdlog::info("MpObjectReference::ProcessActivate {:x} - occupant is "
               "another object and is nearby (activationSource = {:x})",
               GetFormId(), activationSource.GetFormId());
  return false;
}

bool MpObjectReference::IsSleepFurniture() const
{
  auto worldState = GetParent();
  if (!worldState) {
    return false;
  }
  const auto base = worldState->GetEspm().GetBrowser().LookupById(GetBaseId());
  const auto furniture = espm::Convert<espm::FURN>(base.rec);
  return furniture &&
    (furniture->GetData(worldState->GetEspmCache()).activeMarkers &
     espm::FURN::kCanSleep);
}

void MpObjectReference::RemoveFromGridAndUnsubscribeAll()
{
  auto worldOrCell = GetCellOrWorld().ToFormId(GetParent()->espmFiles);
  auto gridIterator = GetParent()->grids.find(worldOrCell);
  if (gridIterator != GetParent()->grids.end()) {
    gridIterator->second.grid->Forget(this);
  }

  auto listenersCopy = GetListeners();
  for (auto listener : listenersCopy) {
    Unsubscribe(this, listener);
  }

  everSubscribedOrListened = false;
}

void MpObjectReference::UnsubscribeFromAll()
{
  auto emittersCopy = GetEmitters();
  for (auto emitter : emittersCopy)
    Unsubscribe(emitter, this);
}

void MpObjectReference::InitScripts()
{
  auto baseId = GetBaseId();
  if (!baseId || !GetParent()->espm) {
    return;
  }

  auto scriptStorage = GetParent()->GetScriptStorage();
  if (!scriptStorage) {
    return;
  }

  auto& compressedFieldsCache = GetParent()->GetEspmCache();

  std::vector<std::string> scriptNames;

  auto& br = GetParent()->espm->GetBrowser();
  auto base = br.LookupById(baseId);
  auto refr = br.LookupById(GetFormId());
  for (auto record : { base.rec, refr.rec }) {
    if (!record) {
      continue;
    }

    std::optional<espm::ScriptData> scriptData;

    if (record == base.rec && record->GetType() == "NPC_") {
      auto baseId = base.ToGlobalId(base.rec->GetId());
      if (auto actor = AsActor()) {
        auto& templateChain = actor->GetTemplateChain();
        scriptData = EvaluateTemplate<espm::NPC_::UseScript>(
          GetParent(), baseId, templateChain,
          [&compressedFieldsCache](const auto& npcLookupRes,
                                   const auto& npcData) {
            espm::ScriptData scriptData;
            npcLookupRes.rec->GetScriptData(&scriptData,
                                            compressedFieldsCache);
            return scriptData;
          });
      }
    }

    if (!scriptData) {
      scriptData = espm::ScriptData();
      record->GetScriptData(&*scriptData, compressedFieldsCache);
    }

    auto& scriptsInStorage =
      GetParent()->GetScriptStorage()->ListScripts(false);
    for (auto& script : scriptData->scripts) {
      if (scriptsInStorage.count(
            { script.scriptName.begin(), script.scriptName.end() })) {

        if (std::count(scriptNames.begin(), scriptNames.end(),
                       script.scriptName) == 0) {
          scriptNames.push_back(script.scriptName);
        }
      } else if (auto wst = GetParent())
        wst->logger->warn("Script '{}' not found in the script storage",
                          script.scriptName);
    }
  }

  // A hardcoded hack to remove all scripts except SweetPie scripts from
  // exterior objects
  // TODO: make is a server setting with proper conditions or an API
  if (GetParent() && GetParent()->disableVanillaScriptsInExterior &&
      GetFormId() < 0x05000000) {
    auto cellOrWorld = GetCellOrWorld().ToFormId(GetParent()->espmFiles);
    auto lookupRes =
      GetParent()->GetEspm().GetBrowser().LookupById(cellOrWorld);
    if (lookupRes.rec && lookupRes.rec->GetType() == "WRLD") {
      spdlog::info("Skipping non-Sweet scripts for exterior form {:x}",
                   cellOrWorld);
      scriptNames.erase(std::remove_if(scriptNames.begin(), scriptNames.end(),
                                       [](const std::string& val) {
                                         auto kPrefix = "Sweet";
                                         bool startsWith = val.size() >= 5 &&
                                           !memcmp(kPrefix, val.data(), 5);
                                         return !startsWith;
                                       }),
                        scriptNames.end());
    }
  }

  // A hardcoded hack to remove unsupported scripts
  // TODO: make is a server setting with proper conditions or an API
  auto isScriptEraseNeeded = [](const std::string& val) {
    // 1. GetStage in OnTrigger
    // 2. Unable to determine Actor for 'Game.GetPlayer' in 'OnLoad'
    const bool isRemoveNeeded =
      !Utils::stricmp(val.data(), "DA06PreRitualSceneTriggerScript") ||
      !Utils::stricmp(val.data(), "CritterSpawn");

    spdlog::info("Skipping script {}", val);

    return isRemoveNeeded;
  };
  scriptNames.erase(std::remove_if(scriptNames.begin(), scriptNames.end(),
                                   isScriptEraseNeeded),
                    scriptNames.end());

  if (!scriptNames.empty()) {
    pImpl->scriptState = std::make_unique<ScriptState>();

    std::vector<VirtualMachine::ScriptInfo> scriptInfo;
    for (auto& scriptName : scriptNames) {
      auto scriptVariablesHolder = std::make_shared<ScriptVariablesHolder>(
        scriptName, base, refr, base.parent, &compressedFieldsCache,
        GetParent());
      scriptInfo.push_back({ scriptName, std::move(scriptVariablesHolder) });
    }

    GetParent()->GetPapyrusVm().AddObject(ToGameObject(), scriptInfo);
  }
}

void MpObjectReference::MoveOnGrid(GridImpl<MpObjectReference*>& grid)
{
  auto newGridPos = GetGridPos(GetPos());
  grid.Move(this, newGridPos.first, newGridPos.second);
}

void MpObjectReference::InitListenersAndEmitters()
{
  if (!listeners) {
    listeners.reset(new std::set<MpObjectReference*>);
    emitters.reset(new std::set<MpObjectReference*>);
    actorListenerArray.clear();
  }
}

void MpObjectReference::SendInventoryUpdate()
{
  constexpr int kChannelSetInventory = 0;
  auto actor = AsActor();
  if (actor) {
    SetInventoryMessage message;
    message.inventory = actor->GetInventory();
    actor->SendToUserDeferred(message, true, kChannelSetInventory, true);
  }
}

void MpObjectReference::SendOpenContainer(uint32_t targetId)
{
  auto actor = AsActor();
  // thuum docs/verbs/cell-host.md: this reply performs a player's own
  // activation in its game (the client blocks it and waits for the word).
  // A hosted actor's activation already ran in its host's engine, and
  // GetActorToSendTo hands an offline actor's messages to its host, whose
  // game then activated the object with its own player: playtest 32
  // (2026-10-22), the host sat on the bench a resident had taken. The reply
  // is the activating player's alone.
  if (actor && actor->GetUserId() != Networking::InvalidUserId) {
    OpenContainerMessage msg;
    msg.target = targetId;
    actor->SendToUser(msg, true);
  }
}

std::vector<espm::CONT::ContainerObject> GetOutfitObjects(
  WorldState* worldState, const std::vector<FormDesc>& templateChain,
  const espm::LookupResult& lookupRes)
{
  auto& compressedFieldsCache = worldState->GetEspmCache();

  std::vector<espm::CONT::ContainerObject> res;

  if (auto baseNpc = espm::Convert<espm::NPC_>(lookupRes.rec)) {
    auto baseId = lookupRes.ToGlobalId(lookupRes.rec->GetId());
    auto outfitId = EvaluateTemplate<espm::NPC_::UseInventory>(
      worldState, baseId, templateChain,
      [](const auto& npcLookupRes, const auto& npcData) {
        return npcLookupRes.ToGlobalId(npcData.defaultOutfitId);
      });

    auto outfitLookupRes =
      worldState->GetEspm().GetBrowser().LookupById(outfitId);
    auto outfit = espm::Convert<espm::OTFT>(outfitLookupRes.rec);
    auto outfitData =
      outfit ? outfit->GetData(compressedFieldsCache) : espm::OTFT::Data();

    for (uint32_t i = 0; i != outfitData.count; ++i) {
      auto outfitElementId = outfitLookupRes.ToGlobalId(outfitData.formIds[i]);
      res.push_back({ outfitElementId, 1 });
    }
  }
  return res;
}

std::vector<espm::CONT::ContainerObject> GetInventoryObjects(
  WorldState* worldState, const std::vector<FormDesc>& templateChain,
  const espm::LookupResult& lookupRes)
{
  auto& compressedFieldsCache = worldState->GetEspmCache();

  auto baseContainer = espm::Convert<espm::CONT>(lookupRes.rec);
  if (baseContainer) {
    return baseContainer->GetData(compressedFieldsCache).objects;
  }

  auto baseNpc = espm::Convert<espm::NPC_>(lookupRes.rec);
  if (baseNpc) {
    auto baseId = lookupRes.ToGlobalId(lookupRes.rec->GetId());
    return EvaluateTemplate<espm::NPC_::UseInventory>(
      worldState, baseId, templateChain,
      [](const auto&, const auto& npcData) { return npcData.objects; });
  }

  return {};
}

void MpObjectReference::AddContainerObject(
  const espm::CONT::ContainerObject& entry,
  std::map<uint32_t, uint32_t>* itemsToAdd, uint32_t pcLevel)
{
  auto& espm = GetParent()->GetEspm();
  auto formLookupRes = espm.GetBrowser().LookupById(entry.formId);
  auto leveledItem = espm::Convert<espm::LVLI>(formLookupRes.rec);
  if (leveledItem) {
    constexpr uint32_t kCountMult = 1;
    auto map = LeveledListUtils::EvaluateListRecurse(
      espm.GetBrowser(), formLookupRes, kCountMult, pcLevel,
      chanceNoneOverride.get());
    for (auto& p : map) {
      (*itemsToAdd)[p.first] += p.second;
    }
  } else {
    (*itemsToAdd)[entry.formId] += entry.count;
  }
}

void MpObjectReference::EnsureBaseContainerAdded(espm::Loader& espm)
{
  if (ChangeForm().baseContainerAdded) {
    return;
  }

  auto worldState = GetParent();
  if (!worldState) {
    return;
  }

  auto actor = AsActor();
  const std::vector<FormDesc> kEmptyTemplateChain;
  const std::vector<FormDesc>& templateChain =
    actor ? actor->GetTemplateChain() : kEmptyTemplateChain;

  auto lookupRes = espm.GetBrowser().LookupById(GetBaseId());

  std::map<uint32_t, uint32_t> itemsToAdd, itemsToEquip;

  // thuum docs/verbs/npc-fate.md (ADR-049 decision 1): an actor's leveled
  // entries roll at its fate level (the first loader's), else the old 1
  const uint32_t pcLevel = actor && actor->GetFateLevel().has_value()
    ? std::max<uint32_t>(1, *actor->GetFateLevel())
    : kPlayerCharacterLevel;

  auto inventoryObjects =
    GetInventoryObjects(GetParent(), templateChain, lookupRes);
  for (auto& entry : inventoryObjects) {
    AddContainerObject(entry, &itemsToAdd, pcLevel);
  }

  // thuum docs/verbs/npc-fate.md, npc-stream.md (train 51): the outfit rolls
  // ONCE, and what the actor wears is what its inventory holds. Two rolls of
  // a leveled outfit entry (one for the worn set, one for the inventory) gave
  // a placed bandit's record a worn hide helmet its inventory lacked, so the
  // host's faithful hands report was refused ("inventory does not contain
  // item 13913", x-npc-copy-fate 20261027-041406), and every leveled
  // game-file outfit could do the same
  auto outfitObjects = GetOutfitObjects(GetParent(), templateChain, lookupRes);
  for (auto& entry : outfitObjects) {
    AddContainerObject(entry, &itemsToEquip, pcLevel);
  }
  for (auto& p : itemsToEquip) {
    itemsToAdd[p.first] += p.second;
  }
  if (actor) {
    Equipment eq;
    for (auto& p : itemsToEquip) {
      Inventory::Entry e;
      e.baseId = p.first;
      e.count = p.second;
      e.SetWorn(Inventory::Worn::Right);
      eq.inv.AddItems({ e });
    }
    actor->SetEquipment(eq);
  }

  std::vector<Inventory::Entry> entries;
  for (auto& p : itemsToAdd) {
    entries.push_back({ p.first, p.second });
  }
  AddItems(entries);

  if (!ChangeForm().baseContainerAdded) {
    EditChangeForm([&](MpChangeFormREFR& changeForm) {
      changeForm.baseContainerAdded = true;
    });
  }
}

void MpObjectReference::CheckInteractionAbility(MpObjectReference& refr)
{
  auto& loader = GetParent()->GetEspm();
  auto& compressedFieldsCache = GetParent()->GetEspmCache();

  auto casterWorldId = refr.GetCellOrWorld().ToFormId(GetParent()->espmFiles);
  auto targetWorldId = GetCellOrWorld().ToFormId(GetParent()->espmFiles);

  auto casterWorld = loader.GetBrowser().LookupById(casterWorldId).rec;
  auto targetWorld = loader.GetBrowser().LookupById(targetWorldId).rec;

  if (targetWorld != casterWorld) {
    const char* casterWorldName =
      casterWorld ? casterWorld->GetEditorId(compressedFieldsCache) : "<null>";

    const char* targetWorldName =
      targetWorld ? targetWorld->GetEditorId(compressedFieldsCache) : "<null>";

    throw std::runtime_error(fmt::format(
      "WorldSpace doesn't match: caster is in {} ({:#x}), target is in "
      "{} ({:#x})",
      casterWorldName, casterWorldId, targetWorldName, targetWorldId));
  }
}

void MpObjectReference::SendMessageToActorListeners(const IMessageBase& msg,
                                                    bool reliable) const
{
  for (auto listener : GetActorListeners()) {
    listener->GetActorToSendTo().SendToUser(msg, true);
  }
}

void MpObjectReference::BeforeDestroy()
{
  if (this->occupant && this->occupantDestroySink) {
    this->occupant->RemoveEventSink(this->occupantDestroySink);
  }

  // Move far far away calling OnTriggerExit, unsubscribing, etc
  SetPos({ -1'000'000'000, 0, 0 });

  MpForm::BeforeDestroy();

  // both directions, as PartOne::SetUserActor pairs them and this header
  // asks: RemoveFromGridAndUnsubscribeAll alone leaves the reverse edge (an
  // actor's listener set still naming this form) when the form was Delete()d
  // before its destroy, since Delete's unsubscribe emptied this form's own
  // set and the SetPos above then finds nothing to remove. The overnight
  // train's T0 (thuum docs/verbs/spell-placed-object.md: a fallen set's forms
  // deleted, then destroyed by the test's Leave) died with SIGSEGV in the
  // caster's own destroy, Unsubscribe reading the freed form (2026-10-29).
  UnsubscribeFromAll();
  RemoveFromGridAndUnsubscribeAll();
}

float MpObjectReference::GetTotalItemWeight() const
{
  const auto& entries = GetInventory().entries;
  const auto calculateWeight = [this](float sum,
                                      const Inventory::Entry& entry) {
    const auto& espm = GetParent()->GetEspm();
    const auto* record = espm.GetBrowser().LookupById(entry.baseId).rec;
    if (!record) {
      spdlog::warn(
        "MpObjectReference::GetTotalItemWeight of ({:x}): Record of form "
        "({}) is nullptr",
        GetFormId(), entry.baseId);
      return 0.f;
    }
    float weight = GetWeightFromRecord(record, GetParent()->GetEspmCache());
    if (!espm::utils::IsItem(record->GetType())) {
      spdlog::warn("Unsupported espm type {} has been detected, when "
                   "calculating overall weight.",
                   record->GetType().ToString());
    } else {
      spdlog::trace("Weight: {} for record of type {}", weight,
                    record->GetType().ToString());
    }
    return sum + entry.count * weight;
  };

  return std::accumulate(entries.begin(), entries.end(), 0.f, calculateWeight);
}
