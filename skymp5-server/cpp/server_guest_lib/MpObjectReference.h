#pragma once
#include "ChangeFormGuard.h"
#include "FormIndex.h"
#include "Grid.h"
#include "Inventory.h"
#include "JsonUtils.h"
#include "LocationalData.h"
#include "MessageBase.h"
#include "MpChangeForms.h"
#include "MpForm.h"
#include "libespm/Loader.h"
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <set>
#include <simdjson.h>
#include <string>
#include <tuple>
#include <vector>

#include "CreateActorMessage.h"
#include "UpdatePropertyMessage.h"

struct GridPosInfo
{
  uint32_t worldOrCell = 0;
  int16_t x = 0;
  int16_t y = 0;

  friend bool operator==(const GridPosInfo& lhs, const GridPosInfo& rhs)
  {
    return std::make_tuple(lhs.worldOrCell, lhs.x, lhs.y) ==
      std::make_tuple(rhs.worldOrCell, rhs.x, rhs.y);
  }
};

class MpActor;
class WorldState;
class OccupantDestroyEventSink;
class OccupantDisableEventSink;

class FormCallbacks;

class FormCallbacks;

enum class VisitPropertiesMode
{
  OnlyPublic,
  All
};

enum class SetPosMode
{
  // Will save pos from time to time
  CalledByUpdateMovement,

  // Will save pos immediately
  Other
};

// Corresponding string is a constant name without "kVariable_" prefix
// Keep in sync with MpObjectReference::GetAnimationVariableBool
enum class AnimationVariableBool
{
  kInvalidVariable,
  kVariable_bInJumpState,
  kVariable__skymp_isWeapDrawn,
  kVariable_IsBlocking,
  kVariable_IsSneaking,
  kNumVariables
};

using SetAngleMode = SetPosMode;

// thuum docs/verbs/npc-fate.md: the level a container's leveled item entry
// rolls at when no actor's fate level says otherwise
constexpr uint32_t kPlayerCharacterLevelDefault = 1;

class MpObjectReference
  : public MpForm
  , public FormIndex
  , protected ChangeFormGuard
{
  friend class OccupantDestroyEventSink;
  friend class OccupantDisableEventSink;

public:
  static const char* Type() { return "ObjectReference"; }
  const char* GetFormType() const override { return "ObjectReference"; }

  MpObjectReference(
    const LocationalData& locationalData, const FormCallbacks& callbacks,
    uint32_t baseId, std::string baseType,
    std::optional<NiPoint3> primitiveBoundsDiv2 = std::nullopt);

  const NiPoint3& GetPos() const;
  const NiPoint3& GetAngle() const;
  const FormDesc& GetCellOrWorld() const;
  const uint32_t& GetBaseId() const;
  const std::string& GetBaseType() const;
  // thuum docs/verbs/npc-fate.md (the deaths): a dead game-file actor, a
  // container to the player beside it
  bool IsCorpse();
  const Inventory& GetInventory() const;
  // thuum (playtest 37, 2026-10-28, Eli: "shouldn't be that hard to change
  // that to on cell load right?"): a container whose inventory the server
  // has written (the base's roll at its first open, EnsureBaseContainerAdded;
  // a script's or a gamemode's AddItem or SetInventory before one). Every
  // player whose grid loads it hears that record at the subscribe
  // (VisitProperties); an untouched container sends nothing, since both
  // engines hold the base's default
  bool HasRecordedInventory() const;
  const bool& IsHarvested() const;
  const bool& IsOpen() const;
  const bool& IsDisabled() const;
  const bool& IsDeleted() const;
  const uint32_t& GetCount() const;
  float GetTotalItemWeight() const;
  std::chrono::system_clock::duration GetRelootTime() const;
  bool GetAnimationVariableBool(const char* name) const;
  bool IsPointInsidePrimitive(const NiPoint3& point) const;
  bool HasPrimitive() const;
  FormCallbacks GetCallbacks() const;
  bool HasScript(const char* name) const;
  bool IsActivationBlocked() const;
  bool GetTeleportFlag() const;

  virtual void VisitProperties(CreateActorMessage& message,
                               VisitPropertiesMode mode);
  // `reportedByHost`: the activation is a client's report for an actor it
  // simulates (thuum docs/verbs/npc-handoff.md D6, the door): a hosted
  // NPC's door is then its host engine's to walk through; a script's or a
  // runtime actor's activation keeps the server's own door transit
  virtual void Activate(MpObjectReference& activationSource,
                        bool defaultProcessingOnly = false,
                        bool isSecondActivation = false,
                        bool reportedByHost = false);
  // A FURN the engine lets the player sleep in (thuum docs/verbs/sleep.md)
  bool IsSleepFurniture() const;
  virtual void Disable();
  virtual void Enable();

  void SetPos(const NiPoint3& newPos,
              SetPosMode setPosMode = SetPosMode::Other);
  void SetAngle(const NiPoint3& newAngle,
                SetAngleMode setAngleMode = SetAngleMode::Other);
  void SetHarvested(bool harvested);
  void SetOpen(bool open);
  void PutItem(MpActor& actor, const Inventory::Entry& entry);
  void TakeItem(MpActor& actor, const Inventory::Entry& entry);
  void SetRelootTime(std::chrono::system_clock::duration newRelootTime);
  void SetChanceNoneOverride(uint8_t chanceNone);
  void SetCellOrWorld(const FormDesc& worldOrCell);
  void SetAnimationVariableBool(AnimationVariableBool animationVariableBool,
                                bool value);
  void SetActivationBlocked(bool blocked);
  void ForceSubscriptionsUpdate();
  void SetPrimitive(const NiPoint3& boundsDiv2);
  void UpdateHoster(uint32_t newHosterId);
  void TellHostedByOther(MpActor& game);
  void SetPropertyValueDump(const std::string& propertyName,
                            const std::string& valueDump,
                            bool isVisibleByOwner, bool isVisibleByNeighbor);
  void SetTeleportFlag(bool value);
  void SetPosAndAngleSilent(const NiPoint3& pos, const NiPoint3& rot);
  void Delete();
  void SetCount(uint32_t count);

  // If you want to completely remove ObjectReference from the grid you need
  // toUnsubscribeFromAll and then RemoveFromGridAndUnsubscribeAll. Do not use
  // any of these functions without another in new code if you have no good
  // reason for this.
  void UnsubscribeFromAll();
  void RemoveFromGridAndUnsubscribeAll();

  void SetInventory(const Inventory& inv);
  void AddItem(uint32_t baseId, uint32_t count);
  void AddItems(const std::vector<Inventory::Entry>& entries);
  void RemoveItem(uint32_t baseId, uint32_t count, MpObjectReference* target);
  void RemoveItems(const std::vector<Inventory::Entry>& entries,
                   MpObjectReference* target = nullptr);
  void RemoveAllItems(MpObjectReference* target = nullptr);
  void RelootContainer();
  void RegisterProfileId(int32_t profileId);
  void RegisterPrivateIndexedProperty(
    const std::string& propertyName,
    const std::string& propertyValueStringified);

  static void Subscribe(MpObjectReference* emitter,
                        MpObjectReference* listener);
  static void Unsubscribe(MpObjectReference* emitter,
                          MpObjectReference* listener);

  void SetLastAnimation(const std::string& lastAnimation);
  void SetNodeTextureSet(const std::string& node,
                         const espm::LookupResult& textureSet,
                         bool firstPerson);
  void SetNodeScale(const std::string& node, float scale, bool firstPerson);
  void SetDisplayName(const std::optional<std::string>& newName);

  const std::set<MpObjectReference*>& GetListeners() const;
  const std::set<MpObjectReference*>& GetEmitters() const;

  // uses default reloot time if nullopt passed
  void RequestReloot(
    std::optional<std::chrono::system_clock::duration> time = std::nullopt);
  void DoReloot();
  std::shared_ptr<std::chrono::time_point<std::chrono::system_clock>>
  GetNextRelootMoment() const;

  virtual MpChangeForm GetChangeForm() const;
  virtual void ApplyChangeForm(const MpChangeForm& changeForm);
  const DynamicFields& GetDynamicFields() const;

  // thuum docs/verbs/quest-stage.md (ADR-056): the world's quest stages, as
  // the world's journal reference keeps them in its change form
  // (PartOne::WorldJournal); empty when none; false when unchanged
  std::vector<QuestStageDone> GetQuestStages() const;
  bool SetQuestStages(std::vector<QuestStageDone> stages);

  // thuum docs/verbs/spell-placed-object.md (ADR-057): the spell whose
  // accepted cast made the server place this reference, none for every
  // other form; the record's mark for the sweep at load
  std::optional<FormDesc> GetPlacedBySpell() const;
  void SetPlacedBySpell(const FormDesc& spell);

  // This method removes ObjectReference from a current grid and doesn't attach
  // to another grid
  void SetCellOrWorldObsolete(const FormDesc& worldOrCell);

  using Visitor = std::function<void(MpObjectReference*)>;
  void VisitNeighbours(const Visitor& visitor);

  void SendInventoryUpdate();
  const std::vector<MpActor*>& GetActorListeners() const noexcept;

  static const char* GetPropertyPrefixPrivate() noexcept { return "private."; }
  static const char* GetPropertyPrefixPrivateIndexed() noexcept
  {
    return "private.indexed.";
  }

  void SendPapyrusEvent(const char* eventName,
                        const VarValue* arguments = nullptr,
                        size_t argumentsCount = 0) override;

protected:
  void Init(WorldState* parent, uint32_t formId, bool hasChangeForm) override;

  void EnsureBaseContainerAdded(espm::Loader& espm);

  void SendMessageToActorListeners(const IMessageBase& msg,
                                   bool reliable) const;

private:
  // thuum docs/verbs/npc-fate.md (ADR-049): `pcLevel` is the level a leveled
  // item entry rolls at (an actor's fate level, else 1)
  void AddContainerObject(const espm::CONT::ContainerObject& containerObject,
                          std::map<uint32_t, uint32_t>* itemsToAdd,
                          uint32_t pcLevel = kPlayerCharacterLevelDefault);
  void InitScripts();
  void MoveOnGrid(GridImpl<MpObjectReference*>& grid);
  void InitListenersAndEmitters();
  void SendOpenContainer(uint32_t refId);

  void CheckInteractionAbility(MpObjectReference& ac);
  bool IsLocationSavingNeeded() const;
  void ProcessActivateNormal(MpObjectReference& activationSource,
                             bool reportedByHost = false);
  bool ProcessActivateSecond(MpObjectReference& activationSource);
  void GivePickupItemsToActivationSource(MpObjectReference& activationSource,
                                         const espm::LookupResult& base);
  void ActivateChilds();
  // thuum memo D52 (playtest 37, Eli: "i WOULD like two people to be able to
  // be in one container at once"): a container's or a corpse's occupants, as
  // many as open it; furniture keeps the one occupant below
  bool IsContainerOccupant(const MpActor* actor) const;
  void AddContainerOccupant(MpActor& actor);
  void RemoveContainerOccupant(MpActor& actor);
  void PruneContainerOccupants(float reach);
  void SendInventoryToContainerOccupants(const MpActor* except);
  void ResyncAfterRefusedMove(MpActor& ac);
  bool CheckIfObjectCanStartOccupyThis(MpObjectReference& activationSource,
                                       float occupationReach);

  bool everSubscribedOrListened = false;
  std::unique_ptr<std::set<MpObjectReference*>> listeners;
  std::vector<MpActor*> actorListenerArray;

  // Should be empty for non-actor refs
  std::unique_ptr<std::set<MpObjectReference*>> emitters;

  // The following keys were originally formIds, but changed to pointers for
  // the sake of performance. Luckily, the server never releases objects, so
  // the pointers are always valid.
  // TODO: ensure primitivesWeAreInside is freed correctly
  std::unique_ptr<std::map<MpObjectReference*, bool /* wasInside */>>
    emittersWithPrimitives;
  std::unique_ptr<std::set<MpObjectReference*>> primitivesWeAreInside;

  std::string baseType;
  uint32_t baseId = 0;
  MpActor* occupant = nullptr;
  std::shared_ptr<OccupantDestroyEventSink> occupantDestroySink;
  std::shared_ptr<OccupantDisableEventSink> occupantDisableSink;
  // the container's occupants (memo D52) and each one's sinks; the sinks
  // leave with the actor on its normal way out (the closing activation, the
  // pruning), never from inside their own callback
  std::set<MpActor*> containerOccupants;
  std::map<MpActor*,
           std::pair<std::shared_ptr<OccupantDestroyEventSink>,
                     std::shared_ptr<OccupantDisableEventSink>>>
    containerOccupantSinks;
  std::optional<std::chrono::system_clock::duration> relootTimeOverride;
  std::unique_ptr<uint8_t> chanceNoneOverride;
  bool activationBlocked = false;

  struct Impl;
  std::shared_ptr<Impl> pImpl;

protected:
  void BeforeDestroy() override;
  UpdatePropertyMessage CreatePropertyMessage_(MpObjectReference* self,
                                               const char* name,
                                               const std::string& valueDump);
  UpdatePropertyMessage PreparePropertyMessage_(MpObjectReference* self,
                                                const char* name,
                                                const std::string& valueDump);

  const std::shared_ptr<FormCallbacks> callbacks;
};
