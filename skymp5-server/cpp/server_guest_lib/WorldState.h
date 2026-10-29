#pragma once
#include "ConditionsEvaluator.h" // ConditionsEvaluatorSettings
#include "FormIndex.h"
#include "Grid.h"
#include "GridElement.h"
#include "MpChangeForms.h"
#include "MpForm.h"
#include "MpObjectReference.h"
#include "NiPoint3.h"
#include "PartOneListener.h"
#include "condition_functions/ConditionFunctionMap.h"
#include "libespm/Loader.h"
#include "papyrus-vm/VirtualMachine.h"
#include "script_objects/MpFormGameObject.h"
#include <MakeID.h>
#include <Timer.h>
#include <algorithm>
#include <chrono>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <spdlog/spdlog.h>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#ifdef AddForm
#  undef AddForm
#endif

class MpActor;
class FormCallbacks;
class MpChangeForm;
namespace Viet {
template <typename T, typename FormDescType, typename FilterType>
class ISaveStorage;
}
class IScriptStorage;
class GameModeEvent;

// The game clock at one instant, as the engine's six time globals hold it
// (thuum docs/verbs/time.md). PartOne computes it in Rust.
struct GameTimeNow
{
  uint32_t year = 0;
  uint32_t month = 0; // from 0, Morning Star
  uint32_t day = 1;   // from 1
  float hour = 0.f;
  float daysPassed = 0.f;
  float timeScale = 0.f;
};

class WorldState
{
  friend class MpObjectReference;
  friend class MpActor;

public:
  using FormCallbacksFactory = std::function<FormCallbacks()>;

  struct NpcSettingsEntry
  {
    bool spawnInInterior = false;
    bool spawnInExterior = false;
    bool overriden = false;
  };

public:
  WorldState();
  WorldState(const WorldState&) = delete;
  WorldState& operator=(const WorldState&) = delete;

  void Clear();

  const std::chrono::steady_clock::time_point& GetStartPoint() const;

  void AttachEspm(espm::Loader* espm,
                  const FormCallbacksFactory& formCallbacksFactory);
  void AttachSaveStorage(
    std::shared_ptr<
      Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>
      saveStorage);
  void AttachScriptStorage(std::shared_ptr<IScriptStorage> scriptStorage);

  void AddForm(std::unique_ptr<MpForm> form, uint32_t formId,
               bool skipChecks = false,
               const MpChangeForm* optionalChangeFormToApply = nullptr);

  // thuum docs/verbs/npc-puppet.md (playtest 33, 2026-10-23): every game-file
  // actor whose ChangeForm the save holds, loaded onto the grid at its saved
  // position as the save attaches (a deferred record is invisible to every
  // seat until something loads it)
  void LoadDeferredActors();

  void LoadChangeForm(const MpChangeForm& changeForm,
                      const FormCallbacks& callbacks);

  void Tick();

  // thuum docs/verbs/magic-effects.md: an actor whose effects run is advanced
  // on every Tick until none is left (MpActor::AdvanceEffects)
  void TrackEffects(uint32_t actorFormId);

  void RequestReloot(MpObjectReference& ref,
                     std::chrono::system_clock::duration time);

  void RequestSave(MpObjectReference& ref);
  bool HasEspmFile(std::string_view filename) const noexcept;

  template <typename T>
  Viet::Promise<Viet::Void> SetTimer(T&& duration,
                                     uint32_t* outTimerId = nullptr)
  {
    return timerRegular.SetTimer(std::forward<T>(duration), outTimerId);
  }

  template <typename T>
  Viet::Promise<Viet::Void> SetEffectTimer(T&& duration,
                                           uint32_t* outTimerId = nullptr)
  {
    return timerEffects.SetTimer(std::forward<T>(duration), outTimerId);
  }

  bool RemoveTimer(uint32_t timerId);
  Viet::Promise<Viet::Void> SetTimer(
    std::reference_wrapper<const std::chrono::system_clock::time_point>
      wrapper);
  Viet::Promise<Viet::Void> SetEffectTimer(
    std::reference_wrapper<const std::chrono::system_clock::time_point>
      wrapper);
  bool RemoveEffectTimer(uint32_t timerId);

  // Loads a requested form, likely resulting in whole chunk
  // loading if not yet loaded. `sighted`: a host's engine has the actor
  // attached and enabled (thuum docs/verbs/npc-puppet.md, the roster), so
  // the loader's disabled filters (initially disabled, an enable parent
  // that disables) do not apply to it
  const std::shared_ptr<MpForm>& LookupFormById(
    uint32_t formId, std::stringstream* optionalOutTrace = nullptr,
    bool sighted = false);

  // No loading
  MpForm* LookupFormByIdx(int idx);

  // No loading version of LookupFormById
  const std::shared_ptr<MpForm>& LookupFormByIdNoLoad(uint32_t formId);

  void SendPapyrusEvent(MpForm* form, const char* eventName,
                        const VarValue* arguments, size_t argumentsCount);

  const std::set<MpObjectReference*>& GetNeighborsByPosition(
    uint32_t cellOrWorld, int16_t cellX, int16_t cellY);

  std::shared_ptr<std::vector<uint32_t>> GetAllForms(uint32_t modIndex);

  // See LookupFormById comment
  template <class F>
  F& GetFormAt(uint32_t formId)
  {
    const std::shared_ptr<MpForm>& form = LookupFormById(formId);
    if (!form) {
      throw std::runtime_error(
        fmt::format("Form with id {:#x} doesn't exist", formId));
    }

    F* typedForm = nullptr;

    if constexpr (std::is_same_v<F, MpActor>) {
      typedForm = form->AsActor();
    } else if constexpr (std::is_same_v<F, MpObjectReference>) {
      typedForm = form->AsObjectReference();
    } else {
      typedForm = dynamic_cast<F*>(form.get());
    }

    if (!typedForm) {
      if constexpr (std::is_same_v<F, MpActor>) {
        if (auto ref = std::dynamic_pointer_cast<MpObjectReference>(form)) {
          auto pos = ref->GetPos();
          spdlog::warn(
            "Specified Form is ObjectReference, but we tried to treat it as "
            "Actor, likely because of a client bug. formId={:#x}, "
            "baseId={:#x}, pos is {:.2f} {:.2f} {:.2f} at {}",
            formId, ref->GetBaseId(), pos.x, pos.y, pos.z,
            ref->GetCellOrWorld().ToString());
        }
      }
      throw std::runtime_error(
        fmt::format("Form with id {:#x} is not {} (actually it is {})", formId,
                    F::Type(), MpForm::GetFormType(&*form)));
    }

    return *typedForm;
  };

  template <class FormType = MpForm>
  void DestroyForm(uint32_t formId,
                   std::shared_ptr<FormType>* outDestroyedForm = nullptr)
  {
    auto it = forms.find(formId);
    if (it == forms.end()) {
      throw std::runtime_error(
        static_cast<const std::stringstream&>(std::stringstream()
                                              << "Form with id " << std::hex
                                              << formId << " doesn't exist")
          .str());
    }

    auto& form = it->second;
    if (!dynamic_cast<FormType*>(form.get())) {
      std::stringstream s;
      s << "Expected form " << std::hex << formId << " to be "
        << MpForm::GetFormType<FormType>() << ", but got "
        << MpForm::GetFormType(form.get());
      throw std::runtime_error(s.str());
    }

    if (outDestroyedForm)
      *outDestroyedForm = std::dynamic_pointer_cast<FormType>(it->second);

    it->second->BeforeDestroy();

    if (auto formIndex = dynamic_cast<FormIndex*>(form.get())) {
      if (formIdxManager && !formIdxManager->DestroyID(formIndex->idx))
        throw std::runtime_error("DestroyID failed");
    }

    forms.erase(it);
  };

  espm::Loader& GetEspm() const;
  bool HasEspm() const;
  espm::CompressedFieldsCache& GetEspmCache();
  IScriptStorage* GetScriptStorage() const;
  VirtualMachine& GetPapyrusVm();
  const std::set<uint32_t>& GetActorsByProfileId(int32_t profileId) const;
  const std::set<uint32_t>& GetActorsByPrivateIndexedProperty(
    const std::string& privateIndexedPropertyMapKey) const;
  std::string MakePrivateIndexedPropertyMapKey(
    const std::string& propertyName,
    const std::string& propertyValueStringified);
  uint32_t GenerateFormId();
  void SetRelootTime(const std::string& recordType,
                     std::chrono::system_clock::duration time);
  std::optional<std::chrono::system_clock::duration> GetRelootTime(
    const std::string& recordType) const;

  // Utility function to check if the provided baseId has the certain keyword
  bool HasKeyword(uint32_t baseId, const char* keyword);

  // thuum docs/verbs/cell-host.md: the CELL a reference is in: the record's
  // worldOrCell for an interior; for an exterior, the CELL at the
  // reference's 4096-unit grid square (floor, as the engine's grid and
  // GetParentCell count it) from libespm's index of every exterior CELL;
  // none when no file has a CELL there (unhostable, never 0).
  std::optional<uint32_t> CellOf(const MpObjectReference& ref) const;
  std::optional<uint32_t> CellOf(uint32_t cellOrWorld,
                                 const NiPoint3& pos) const;
  // The player actor whose game simulates the reference where it stands:
  // its cell's host, or the actor's follower in a cell nobody hosts
  // (thuum docs/verbs/npc-handoff.md D6), or nullptr.
  MpActor* HostOf(const MpObjectReference& ref);
  // Whether `cellOrWorld` is an interior CELL record (libespm CELL::Interior).
  bool IsInteriorCell(uint32_t cellOrWorld);
  // Whether `formId` is a persistent placed reference in the game files
  // (the record header's PersistentReference flag); false for a server-made
  // form or an id no file has. The door rule asked it until memo D54 B
  // (2026-10-28, every actor's door alike); CellHostTest reads it as that
  // memo's premise.
  bool IsPersistentReference(uint32_t formId);
  // thuum docs/verbs/npc-fate.md (ADR-049): the game files' records at the
  // chunks around (x, y) of `cellOrWorld` (its grid squares, 4096 units),
  // loaded once each: the grid's own loader (GetNeighborsByPosition, a 3x3)
  // factored out for the fate pass, which loads the ring the engine is about
  // to attach so the actors there are rolled before any engine rolls them.
  void LoadChunksAround(uint32_t cellOrWorld, int16_t x, int16_t y,
                        int16_t radius);

  // Only for tests
  auto& GetGrids() { return grids; }

  void SetNpcSettings(
    std::unordered_map<std::string, NpcSettingsEntry>&& settings);
  void SetForbiddenRelootTypes(const std::set<std::string>& types);
  void SetEnableConsoleCommandsForAllSetting(bool enable);

public:
  EspmFileList espmFiles;
  std::unordered_map<int32_t, std::set<uint32_t>> actorIdByProfileId;
  std::unordered_map<std::string, std::set<uint32_t>>
    actorIdByPrivateIndexedProperty;
  std::shared_ptr<spdlog::logger> logger;
  std::vector<std::shared_ptr<PartOneListener>> listeners;
  // thuum docs/verbs/cell-host.md (ADR-035): who hosts a cell, installed by
  // PartOne from the cell host table (wire-rules cells): the player actor's
  // form id, or 0 while nobody does. The per-actor `hosters` map retired
  // with the Host (14) election; an actor no user plays is its cell's host's.
  std::function<uint32_t(uint32_t cell)> cellHostOf;
  // and who simulates an actor standing in a cell: the host, else the
  // actor's follower (npc-handoff D6), else 0; installed by PartOne; HostOf
  // reads this when installed and cellHostOf otherwise
  std::function<uint32_t(uint32_t actor, uint32_t cell)> simulatorOf;
  // thuum docs/verbs/npc-handoff.md D6, the door: whether a hosted NPC may
  // cross a teleport door into `destWorldOrCell` at `destPos` (PartOne's
  // follow verdict for its simulator), installed by PartOne; none installed,
  // every crossing is allowed
  std::function<bool(MpActor& actor, uint32_t destWorldOrCell,
                     const NiPoint3& destPos)>
    npcDoorVerdict;
  // thuum docs/verbs/npc-fate.md (ADR-049 decision 1): the level the next
  // game-file actors loaded roll their template chain and outfit at (the
  // player whose arrival loads them), set by PartOne around the fate pass's
  // chunk loads; none, the old rule's roll (level 0, outfit at 1)
  std::optional<uint16_t> fateLoaderLevel;
  // thuum docs/verbs/npc-fate.md (the memo's D2): the rule for the roll's
  // level, the server setting npcRollLevel: "loader", "party-mean" (the
  // default), "party-min", or "fixed" with npcRollLevelFixed the number
  // (PartOne::FateRollLevel)
  std::string npcRollLevel = "party-mean";
  uint16_t npcRollLevelFixed = 0;
  // a game-file actor's fate rolled anew (the respawn), installed by PartOne
  // to send the fate again
  std::function<void(MpActor&)> onActorFate;
  // an actor's death state written (MpActor::SendAndSetDeathState: the
  // death true, the respawn false), installed by PartOne for the cell host
  // table (thuum docs/research/m3/decisions-after-playtest-34.md D18)
  std::function<void(MpActor&, bool)> onDeathState;
  // an actor's grid square changed at a position write, before the grid's
  // own 3x3 loads around it (MpObjectReference::SetPos): PartOne's fate pass
  // runs here for a player, so the ring is rolled at that player's level
  // before any of it is loaded or subscribed
  std::function<void(MpActor&)> onSquareChange;
  // thuum ADR-050 decision 3 (train 51): the roll's level for a server-made
  // NPC placed at a reference (PlaceAtMe), by the same rule as the ring's
  // (PartOne::FateRollLevel with the placer as the loader)
  std::function<uint16_t(MpObjectReference&)> fateRollLevelAt;
  // thuum docs/verbs/npc-fate.md (the deaths): the game-file actors whose
  // bodies lie waiting for their deadline (MpActor::ScheduleFateRespawn);
  // PartOne::TickFateRespawns ends them against the game clock
  std::unordered_set<uint32_t> fateRespawnDue;
  // the hours a body lies before its respawn: the server setting
  // npcRespawnHours when set, else the master's GMST iHoursToRespawnCell
  float npcRespawnHours = 0.f;
  float FateRespawnHours();
  // thuum docs/verbs/npc-fate.md (ADR-048's B): a host's death report for a
  // game-file actor is taken when the server can check its cause
  // (ActionListener::OnActorDeath); false refuses every report (A: the
  // server's number alone kills)
  bool npcDeathReports = true;
  // thuum docs/verbs/npc-fate.md (ADR-050 decision 2, deaths by the engine):
  // the server's number for a hit on a hosted NPC goes to its host as a
  // cause (DamageCause) and the kill at zero waits for the host's report up
  // to kPendingDeathMs; false restores the absolute values and the kill at
  // once (a bisect switch, server setting npcEngineDeaths)
  bool npcEngineDeaths = true;
  // the sneak attack multiplier for bows, staffs and crossbows, which the
  // game names no setting for: measured against the engine (x-npc-sneak-shot)
  // and set here (server setting sneakRangedMult); 0 keeps SkyMP's 1.3
  float sneakRangedMult = 0.f;
  // the actors whose server number reached zero and whose host's report is
  // awaited (MpActor::BeginPendingDeath; PartOne::TickDeaths kills the ones
  // past the window)
  std::unordered_set<uint32_t> pendingDeaths;
  std::unordered_map<uint32_t, std::map<uint32_t, float>>
    activationChildsByActivationParent;
  std::vector<std::optional<std::chrono::system_clock::time_point>>
    lastMovUpdateByIdx;

  bool isPapyrusHotReloadEnabled = false;

  // The server's game clock now (thuum ADR-021), installed by PartOne;
  // Papyrus's time natives read it.
  std::function<GameTimeNow()> gameTime;

  // Whether two player actors are in a fight with each other (thuum
  // ADR-023's record, installed by PartOne); IsHostileToActor answers from
  // it for an effect between players (ADR-039)
  std::function<bool(uint32_t, uint32_t)> fightBetween;

  bool npcEnabled = false;
  std::unordered_map<std::string, NpcSettingsEntry> npcSettings;
  NpcSettingsEntry defaultSetting;
  bool enableConsoleCommandsForAll = false;

  bool disableVanillaScriptsInExterior = true;

  ConditionsEvaluatorSettings conditionsEvaluatorSettings;
  ConditionFunctionMap conditionFunctionMap;
  std::vector<GameModeEvent*> currentGameModeEventsStack;

private:
  bool AttachEspmRecord(const espm::CombineBrowser& br,
                        const espm::RecordHeader* record,
                        const espm::IdMapping& mapping,
                        std::stringstream* optionalOutTrace = nullptr,
                        bool sighted = false);

  bool LoadForm(uint32_t formId, std::stringstream* optionalOutTrace = nullptr,
                bool sighted = false);
  void TickSaveStorage(const std::chrono::system_clock::time_point& now);
  void TickTimers(const std::chrono::system_clock::time_point& now);
  void TickEffects(const std::chrono::system_clock::time_point& now);
  [[nodiscard]] bool NpcSourceFilesOverriden() const noexcept;
  [[nodiscard]] bool IsNpcAllowed(uint32_t refrId) const noexcept;
  [[nodiscard]] uint32_t GetFileIdx(uint32_t formId) const noexcept;
  [[nodiscard]] bool IsRelootForbidden(std::string type) const noexcept;

private:
  void LoadChunkIfNeeded(uint32_t cellOrWorld, int16_t x, int16_t y);

  struct GridInfo
  {
    std::shared_ptr<GridImpl<MpObjectReference*>> grid =
      std::make_shared<GridImpl<MpObjectReference*>>();
    std::map<int16_t, std::map<int16_t, bool>> loadedChunks;
  };

  std::unordered_map<uint32_t, std::shared_ptr<MpForm>> forms;
  std::unordered_map<std::string, size_t> loadOrderMap;
  std::unordered_map<uint32_t, GridInfo> grids;
  std::unique_ptr<MakeID> formIdxManager;
  std::vector<MpObjectReference*> refrByIdxUnreliable;
  espm::Loader* espm = nullptr;
  FormCallbacksFactory formCallbacksFactory;
  std::unique_ptr<espm::CompressedFieldsCache> espmCache;

  struct Impl;
  std::shared_ptr<Impl> pImpl;
  Viet::Timer timerEffects, timerRegular;
  std::chrono::steady_clock::time_point worldStartTime;
};
