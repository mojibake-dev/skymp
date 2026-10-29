#pragma once
#include "ActiveMagicEffectsMap.h"
#include "ActorValueRecord.h"
#include "ActorValues.h"
#include "Appearance.h"
#include "DynamicFields.h"
#include "Equipment.h"
#include "Faction.h"
#include "Favorite.h"
#include "FormDesc.h"
#include "IngredientEffects.h"
#include "Inventory.h"
#include "LocationalData.h"
#include "MapMarker.h"
#include "NiPoint3.h"
#include "PlayerGlobal.h"
#include "Quest.h"
#include "QuestStageDone.h"
#include "RunningEffect.h"
#include "ShoutRecord.h"
#include "UnresolvedForm.h"
#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <set>
#include <string>
#include <tuple>

class MpObjectReference;
class WorldState;

struct LearnedSpells
{
  using Data = std::set<uint32_t>;

  void LearnSpell(Data::key_type baseId);

  void ForgetSpell(Data::key_type baseId);

  [[nodiscard]] size_t Count() const noexcept;

  [[nodiscard]] bool IsSpellLearned(Data::key_type baseId) const;

  std::vector<Data::key_type> GetLearnedSpells() const;

  friend bool operator==(const LearnedSpells& lhs, const LearnedSpells& rhs)
  {
    return lhs._learnedSpellIds == rhs._learnedSpellIds;
  }

  friend bool operator!=(const LearnedSpells& lhs, const LearnedSpells& rhs)
  {
    return !(lhs == rhs);
  }

  friend bool operator<(const LearnedSpells& lhs, const LearnedSpells& rhs)
  {
    return lhs._learnedSpellIds < rhs._learnedSpellIds;
  }

private:
  Data _learnedSpellIds{};
};

class MpChangeFormREFR
{
public:
  enum RecType
  {
    REFR = 0,
    ACHR = 1,
  };

  int recType = RecType::REFR;
  FormDesc formDesc;
  FormDesc baseDesc;
  NiPoint3 position = { 0, 0, 0 };
  NiPoint3 angle = { 0, 0, 0 };
  FormDesc worldOrCellDesc;
  Inventory inv;
  LearnedSpells learnedSpells;

  bool isHarvested = false;
  bool isOpen = false;
  bool baseContainerAdded = false;
  uint64_t nextRelootDatetime = 0;
  bool isDisabled = false;
  int32_t profileId = -1;
  bool isDeleted = false;
  uint32_t count = 0;

  bool isRaceMenuOpen = false;
  bool isDead = false;
  ActiveMagicEffectsMap activeMagicEffects;
  bool consoleCommandsAllowed = false;

  // 'appearanceDump' can be empty. it means nullopt.
  // "unexisting" appearance and appearance with zero entries are different
  // values in skymp due to poor design
  std::string appearanceDump;

  Equipment equipment;

  // TODO: inconsistency! save not only percentages but whole structure OR
  // have only percentages in change form struct
  ActorValues actorValues;

  float healthRespawnPercentage = 1.f;
  float magickaRespawnPercentage = 1.f;
  float staminaRespawnPercentage = 1.f;

  // Used only for player characters. See GetSpawnPoint
  LocationalData spawnPoint = { { 133857, -61130, 14662 },
                                { 0.f, 0.f, 72.f },
                                FormDesc::Tamriel() };

  float spawnDelay = 25.0f;

  std::vector<FormDesc> templateChain;

  // thuum docs/verbs/npc-fate.md (ADR-049 decision 4): the level the
  // server's roll of this actor used, present once rolled under the first
  // loader's level; absent for a record rolled before the rule (read as the
  // old roll, level 0 on the wire). An optional field, formVersion kept.
  std::optional<uint16_t> fateLevel;

  // thuum docs/verbs/npc-fate.md (the deaths): the game day a game-file
  // actor's body fell, from the server's clock; the respawn is due
  // WorldState::FateRespawnHours() later and a restart recomputes the
  // deadline from it. Absent while alive. An optional field, formVersion kept.
  std::optional<float> fateDiedAt;

  // Used for PlayAnimation (object reference)
  std::optional<std::string> lastAnimation;

  // Used for SetNodeTextureSet (node, texture set desc)
  std::optional<std::map<std::string, std::string>> setNodeTextureSet;

  // Used for SetNodeScale (node, scale value)
  std::optional<std::map<std::string, float>> setNodeScale;

  // Used for SetDisplayName (object reference)
  std::optional<std::string> displayName;

  // Used for Faction (FACT) synchronization
  std::optional<std::vector<Faction>> factions;

  // Used for Quest (QUST) synchronization
  std::optional<std::vector<Quest>> quests;

  // Map markers the player discovered (thuum docs/verbs/map-markers.md)
  std::optional<std::vector<MapMarker>> mapMarkers;

  // Ingredient effects the player learned (thuum
  // docs/verbs/learned-effects.md)
  std::optional<std::vector<IngredientEffects>> ingredientEffects;

  // The player's favorites (thuum docs/verbs/favorites.md)
  std::optional<std::vector<Favorite>> favorites;

  // The player's RaceMenu look, its preset JSON as RaceMenu saved it (thuum
  // docs/verbs/racemenu-sync.md)
  std::optional<std::string> raceMenuPreset;

  // The player's actor values and progress (thuum
  // docs/verbs/actor-values.md)
  std::optional<ActorValueRecord> actorValueRecord;

  // The player's staff rank for console commands (thuum
  // docs/verbs/console-commands.md; TES3MP's: 0 player, 1 moderator, 2
  // admin, 3 owner); absent in older records, where consoleCommandsAllowed
  // reads as admin and anything else as player
  std::optional<uint8_t> staffRank;

  // The effects running on the actor, each with the seconds it has run
  // (thuum docs/verbs/magic-effects.md); absent in older records, which keep
  // their legacy per-actor-value effects in activeMagicEffects
  std::optional<std::vector<RunningEffect>> runningEffects;

  // The shouts the player has and the words of power it knows and has
  // unlocked (thuum docs/verbs/shouts.md); absent in older records, read as
  // none
  std::optional<ShoutRecord> shoutRecord;

  // The global variables the player's game wrote for its player (thuum
  // docs/verbs/player-globals.md); absent in older records, read as none
  std::optional<std::vector<PlayerGlobal>> playerGlobals;

  // The world's quest stages (thuum docs/verbs/quest-stage.md, ADR-056):
  // per quest the stages done, world-wide; absent in older records, read as
  // none. Lives on the world's journal reference (PartOne::WorldJournal, a
  // server-made reference at a fixed id), never on a player's actor
  std::optional<std::vector<QuestStageDone>> questStages;

  // The spell whose accepted cast made the server place this reference
  // (thuum docs/verbs/spell-placed-object.md, ADR-057), so a restart sweeps
  // it (no cast ledger survives one); absent on every other form and in
  // older records, read as none
  std::optional<FormDesc> placedBySpell;

  // Please update 'ActorTest.cpp' when adding new Actor-related rows

  DynamicFields dynamicFields;

  // The shape of the record this change form was read from (thuum
  // docs/DECISIONS.md ADR-043 decision 2): 1 is the raw shape, every form a
  // runtime id of the order it was saved under; 2 stores every form as
  // "<hex>:<plugin>". A record without the key reads as 1; a form built in
  // memory is current. Not part of the record's equality.
  static constexpr int32_t kFormVersion = 2;
  int32_t formVersion = kFormVersion;

  // The stored forms the running order could not resolve, kept for the next
  // save (ADR-043 decision 1)
  std::vector<UnresolvedForm> unresolved;

  auto ToTuple() const
  {
    return std::make_tuple(
      recType, formDesc, baseDesc, position.x, position.y, position.z, angle.x,
      angle.y, angle.z, worldOrCellDesc, inv.ToJson(), isHarvested, isOpen,
      baseContainerAdded, nextRelootDatetime, isDisabled, profileId, isDeleted,
      count, isRaceMenuOpen, isDead, consoleCommandsAllowed, appearanceDump,
      equipment.ToJson(), actorValues.ToTuple(), healthRespawnPercentage,
      magickaRespawnPercentage, staminaRespawnPercentage, spawnPoint,
      dynamicFields, spawnDelay, learnedSpells, templateChain, fateLevel,
      fateDiedAt, lastAnimation, setNodeTextureSet, setNodeScale, displayName,
      mapMarkers, ingredientEffects, favorites, raceMenuPreset,
      actorValueRecord, staffRank, runningEffects, shoutRecord, playerGlobals,
      questStages, placedBySpell, unresolved);
  }

  // The record boundary (thuum docs/DECISIONS.md ADR-043; the map
  // docs/research/formdesc-world-map.md section 6, shape b). With `files`,
  // the running load order, ToJson writes every stored form as
  // "<hex>:<plugin>" and marks the record kFormVersion; JsonToChangeForm
  // resolves them back to runtime ids, keeping what does not resolve in
  // `unresolved`. A raw-shape record (no mark) read with `files` is re-based
  // from `rawOrder`, the order it was saved under, or from `files` when none
  // is known, and comes back with formVersion 1 so the caller writes it
  // again. Without `files` both keep today's raw shape (the tests, the
  // legacy path); a plugin-relative record cannot be read that way.
  static nlohmann::json ToJson(const MpChangeFormREFR& changeForm,
                               const EspmFileList* files = nullptr);
  static MpChangeFormREFR JsonToChangeForm(
    simdjson::dom::element& element, const EspmFileList* files = nullptr,
    const EspmFileList* rawOrder = nullptr);
};

#define MpChangeForm MpChangeFormREFR

inline bool operator==(const MpChangeFormREFR& lhs,
                       const MpChangeFormREFR& rhs)
{
  return lhs.ToTuple() == rhs.ToTuple();
}

inline bool operator!=(const MpChangeFormREFR& lhs,
                       const MpChangeFormREFR& rhs)
{
  return !(lhs == rhs);
}

inline bool operator<(const MpChangeFormREFR& lhs, const MpChangeFormREFR& rhs)
{
  return lhs.ToTuple() < rhs.ToTuple();
}
