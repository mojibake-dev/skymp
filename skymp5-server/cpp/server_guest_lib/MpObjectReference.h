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
#include "ScriptStateApplyMessage.h"
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

  MpObjectReference(const LocationalData& locationalData,
                    const FormCallbacks& callbacks, uint32_t baseId,
                    std::string baseType,
                    std::optional<NiPoint3> primitiveBoundsDiv2 = std::nullopt,
                    uint32_t primitiveType = 0);

  const NiPoint3& GetPos() const;
  const NiPoint3& GetAngle() const;
  const FormDesc& GetCellOrWorld() const;
  const uint32_t& GetBaseId() const;
  const std::string& GetBaseType() const;
  // thuum docs/verbs/npc-fate.md (the deaths): a dead NPC, a game-file actor
  // or a server-made copy alike (playtest 37, finding 62), a container to the
  // player beside it; a player's dead actor is none
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

  // thuum docs/verbs/cell-host.md: whether the record holds an edit the saving
  // policy skipped (the position moved inside IsLocationSavingNeeded's 30 s),
  // and the save request for it; WorldState::FlushSaveStorage asks every form
  // at a stop
  using ChangeFormGuard::HasUnsavedEdit;
  void FlushUnsavedEdit();
  void SetPos(const NiPoint3& newPos,
              SetPosMode setPosMode = SetPosMode::Other);
  void SetAngle(const NiPoint3& newAngle,
                SetAngleMode setAngleMode = SetAngleMode::Other);
  void SetHarvested(bool harvested);
  void SetOpen(bool open);
  // thuum docs/verbs/locks.md: the reference's lock as the server holds it
  // (the record's, else the game files' XLOC with the key made global; none
  // for a reference the files leave unlocked), its write (the record and the
  // `lock` property to every listener), and the word to a player whose
  // activation met the lock without its key (the engine's own activation on
  // the player's locked reference runs the minigame)
  std::optional<MpChangeFormREFR::Lock> GetLock() const;
  void SetLock(const MpChangeFormREFR::Lock& lock);
  void SendLockPrompt(MpActor& to);
  // thuum D-S3 (traps follow the interaction; ADR-068's DestructionStage
  // row): the reference's destruction as the server recorded it (the stage
  // the engine reached, the destroyed flag), none while nobody reported one
  // (the game files' state); the write is the record's alone (saved), the
  // relay and the late subscriber's serve are ActionListener's and
  // PartOne's
  std::optional<MpChangeFormREFR::Destruction> GetDestruction() const;
  void SetDestruction(const MpChangeFormREFR::Destruction& destruction);
  // thuum ADR-068 rung 3 (the script record): the last record the
  // reference's owner reported of the scripts attached to it, with the
  // sequence and the view the server stamped (ScriptStateRecord.h), none
  // while nobody reported one; the write is the record's alone (saved). The
  // read is the record in place, valid until the reference's next edit
  const std::optional<ScriptStateRecord>& GetScriptState() const;
  void SetScriptState(std::optional<ScriptStateRecord> record);
  // thuum ADR-068 rung 4 (the state rows): the object's sequence at the ref
  // state's last record (MpChangeFormREFR::refStateSeq), none while no
  // script moved its enabled or deleted state
  std::optional<uint32_t> GetRefStateSeq() const;
  // the ref state's record as its owner's call moved it (ActionListener's
  // record branch; wire-rules ref_state): the deleted flag and the object's
  // sequence written, then Enable or Disable as the record now says (an
  // actor's grid changes are MpActor's), and a world object put on the grid
  // (an attach by record, a re-enable), where a recorded one stays while
  // disabled so every seat that loads its cell is served its state
  void SetRefState(bool disabled, bool deleted, uint32_t seq);
  // a game-file world object whose ref state is recorded keeps its place on
  // the grid while disabled (ForceSubscriptionsUpdate): its create word and
  // its served state reach every seat that loads its cell
  bool KeepsGridWhileDisabled() const;
  // the reference starts disabled as the game files place it (initially
  // disabled, or under an enable parent that disables it), written when the
  // loader attaches it by its record (WorldState::AttachEspmRecord's
  // `recorded`), so its first recorded call applies to the engine's own
  // start; no save request, no grid change, no sequence
  void MarkDisabledByFiles();
  // the apply built from the record, stamped with the sequence and the view
  // the record was taken at (false without a record); served ahead of a
  // grant (ApplyHostDecisions) and behind the create word (onSubscribe)
  bool BuildScriptStateApply(ScriptStateApplyMessage& out) const;
  void PutItem(MpActor& actor, const Inventory::Entry& entry);
  void TakeItem(MpActor& actor, const Inventory::Entry& entry);
  // thuum docs/verbs/containers.md: a mover whose put or take the server
  // refused (a reach, a missing item, no hold on the container) hears both
  // inventories again
  void ResyncAfterRefusedMove(MpActor& ac);
  // the container's holder (ADR-067 decision 6: one at a time; memo D52's
  // set kept), read by the tests and released by a player's disconnect
  // (PartOne; docs/verbs/containers.md)
  bool IsContainerOccupant(const MpActor* actor) const;
  void RemoveContainerOccupant(MpActor& actor);
  // a furniture's recorded user, or nullptr: set at a use's activation
  // (ProcessActivateNormal's FURN branch), cleared by its closing
  // activation and by the occupant's destroy or disable; read by the owner
  // of the furniture's graph events (wire-rules object_animation)
  MpActor* GetOccupant() const noexcept { return occupant; }
  void SetRelootTime(std::chrono::system_clock::duration newRelootTime);
  void SetChanceNoneOverride(uint8_t chanceNone);
  void SetCellOrWorld(const FormDesc& worldOrCell);
  void SetAnimationVariableBool(AnimationVariableBool animationVariableBool,
                                bool value);
  void SetActivationBlocked(bool blocked);
  void ForceSubscriptionsUpdate();
  // the trigger volume's shape: 0 or 1 the box the vertices make, 2 a
  // sphere of the bounds' radius (libespm REFR XPRM type)
  void SetPrimitive(const NiPoint3& boundsDiv2, uint32_t primitiveType = 0);
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
  // thuum train 52 (D-R1 C's leveled gap; the fresh review of the whole
  // branch, SHOULD 1): AddItem for several plain items at once, as a leveled
  // roll lands them: one change-form edit (Inventory::AddPlainItems), so one
  // save request and one inventory update to the owner's game, then
  // OnItemAdded raised once per entry in their order, each as AddItem raises
  // it; never a record edit, a change-form copy and an inventory
  // serialization per item (the deferred channel keeps only the last of
  // them)
  void AddPlainItems(const std::vector<Inventory::Entry>& entries);
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
  // the reference's last behavior graph event as the record holds it (an
  // accepted script call's or ObjectAnimation's), none before any: the view
  // a script record is stamped with (thuum ADR-068 rung 3, amendment A)
  std::optional<std::string> GetLastAnimation() const;
  // the fifty fixes' D-C3 A (PENDING ELI): the graph variables a script set
  // on this world object as the record keeps them (MpChangeForm's
  // graphVariables), empty when none; and one more kept where the record's
  // slot rule puts it (wire-rules natives::variable_slot, the names
  // compared without case), false when nothing changed (a call older than
  // the one kept)
  std::vector<MpChangeFormREFR::GraphVariable> GetGraphVariables() const;
  bool KeepGraphVariable(MpChangeFormREFR::GraphVariable variable);
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

  // thuum ADR-068 rung 4: the world's alias fills and created items' marks
  // and its world globals, on the same journal; empty when none; false when
  // unchanged
  std::vector<MpChangeFormREFR::AliasFill> GetAliasFills() const;
  bool SetAliasFills(std::vector<MpChangeFormREFR::AliasFill> fills);
  std::vector<PlayerGlobal> GetWorldGlobals() const;
  bool SetWorldGlobals(std::vector<PlayerGlobal> globals);

  // thuum ADR-068 rung 5: the world's quest runs, on the same journal; and
  // the condition-read script variables, the journal's the world's keys, a
  // player's actor's its own; empty when none; false when unchanged
  std::vector<MpChangeFormREFR::QuestRun> GetQuestRuns() const;
  bool SetQuestRuns(std::vector<MpChangeFormREFR::QuestRun> runs);
  std::vector<MpChangeFormREFR::ScriptVariable> GetScriptVariables() const;
  bool SetScriptVariables(
    std::vector<MpChangeFormREFR::ScriptVariable> variables);

  // thuum docs/verbs/spell-placed-object.md (ADR-057): the spell whose
  // accepted cast made the server place this reference, none for every
  // other form; the record's mark for the sweep at load
  std::optional<FormDesc> GetPlacedBySpell() const;
  void SetPlacedBySpell(const FormDesc& spell);
  // thuum ADR-068 rung 4 (the script set, W3): the script's self and the base
  // whose placement made the server place this reference, none for every
  // other form
  std::optional<MpChangeFormREFR::PlacedByScript> GetPlacedByScript() const;
  void SetPlacedByScript(const FormDesc& self, const FormDesc& base);

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
  // thuum docs/verbs/containers.md (ADR-067 decision 6): a container's or a
  // corpse's holder, one at a time (memo D52's shared set of playtest 37,
  // kept as the set it was and holding one); furniture keeps the one
  // occupant below
  void AddContainerOccupant(MpActor& actor);
  void PruneContainerOccupants(float reach);
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
  // the container's holder (one at most, ADR-067 decision 6; memo D52's set
  // kept) and its sinks; the sinks leave with the actor on its normal way
  // out (the closing activation, the pruning), never from inside their own
  // callback
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
