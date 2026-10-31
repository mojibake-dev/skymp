#pragma once
#include "ActorValueRecord.h"
#include "AnimationData.h"
#include "Appearance.h"
#include "Equipment.h"
#include "Favorite.h"
#include "GetBaseActorValues.h"
#include "IngredientEffects.h"
#include "MapMarker.h"
#include "MpObjectReference.h"
#include "PlayerGlobal.h"
#include "ShoutRecord.h"
#include "UpdateActionMessage.h"
#include "libespm/espm.h"
#include <array>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <set>

#include "DeathStateContainerMessage.h"

class WorldState;
struct ActorValues;
class RespawnEvent;
class ActiveMagicEffectsMap;

class MpActor : public MpObjectReference
{
public:
  friend class RespawnEvent;

  static const char* Type() { return "Actor"; }
  const char* GetFormType() const override { return "Actor"; }

  MpActor(const LocationalData& locationalData_,
          const FormCallbacks& calbacks_, uint32_t optBaseId = 0);

  const bool& IsRaceMenuOpen() const;
  const bool& IsDead() const;
  const bool& IsRespawning() const;
  // thuum docs/verbs/npc-fate.md (playtest 38's finding 65): a placement
  // that starts dead in the game files, loaded as the server's corpse: dead
  // on the record with no death event, no death item and no respawn (the
  // engine's own rule for it), its loot the corpse path's one roll
  void MarkPlacedDead();
  bool IsPlacedDead() const;

  bool IsSpellLearned(uint32_t spellId) const; // including from base
  bool IsSpellLearnedFromBase(uint32_t spellId) const;
  // thuum docs/verbs/npc-stream.md (train 53; playtest 37's flame atronach):
  // the record's own spell set as global ids: the base's SPLO list, the
  // template the chain names for spells (NPC_'s UseSpelllist) and the race's
  // list. The abilities a creature bears from its birth (its cloak) are
  // among them, which no cast event ever names
  std::vector<uint32_t> KnownBaseSpells() const;
  std::vector<uint32_t> GetSpellList() const;

  std::unique_ptr<const Appearance> GetAppearance() const;
  const std::string& GetAppearanceAsJson();
  std::string GetLastAnimEventAsJson() const;
  const Equipment& GetEquipment() const;
  std::array<std::optional<Inventory::Entry>, 2> GetEquippedWeapon() const;
  std::array<std::optional<Inventory::Entry>, 2> GetEquippedScroll() const;
  std::array<std::optional<Inventory::Entry>, 2> GetEquippedLight() const;
  std::array<std::optional<Inventory::Entry>, 2> GetEquippedShield() const;
  uint32_t GetRaceId() const;
  bool IsWeaponDrawn() const;
  espm::ObjectBounds GetBounds() const;
  const std::vector<FormDesc>& GetTemplateChain() const;
  // thuum docs/verbs/npc-fate.md (ADR-049): the level the server's roll of
  // this actor used (none for a record rolled before the rule), and the
  // NPC_ an engine's leveled roll is pinned to: the chain's first leveled
  // pick, else the placed base
  std::optional<uint16_t> GetFateLevel() const;
  uint32_t GetFateBaseId() const;
  // An actor this process rolled without a fate (loaded by the grid's own
  // 3x3 before any fate pass reached it) is rolled anew at `level`, chain
  // and outfit, and stamped; one loaded from a save keeps its roll (ADR-049
  // decision 1: a record rolled before the rule keeps it), as does a dead
  // one and one already fated. Returns whether it rolled.
  bool RollFateIfUnrolled(uint16_t level);
  bool IsCreatedAsPlayer() const;
  const ActorValues& GetActorValues() const;
  const ActiveMagicEffectsMap& GetActiveMagicEffects() const;
  int32_t GetProfileId() const;

  float GetHealthRespawnPercentage() const;
  float GetMagickaRespawnPercentage() const;
  float GetStaminaRespawnPercentage() const;

  // thuum docs/verbs/attributes.md: a percentage the server RAISED holds
  // against the client's reports until one reaches it (the client's next
  // report may predate the server's message and read as a fall); false
  // from TakeReportedPercentage means the report is stale and dropped
  bool TakeReportedPercentage(espm::ActorValue av, float reported);

  void SetRaceMenuOpen(bool isOpen);
  void SetAppearance(const Appearance* newAppearance);
  void SetEquipment(const Equipment& newEquipment);

  void SetHealthRespawnPercentage(float percentage);
  void SetMagickaRespawnPercentage(float percentage);
  void SetStaminaRespawnPercentage(float percentage);

  void AddToFaction(Faction faction, bool lazyLoad = true);
  bool IsInFaction(FormDesc factionForm, bool lazyLoad = true);
  std::vector<Faction> GetFactions(int minFactionID, int maxFactionID,
                                   bool lazyLoad = true);
  void RemoveFromFaction(FormDesc factionForm, bool lazyLoad = true);

  void VisitProperties(CreateActorMessage& message,
                       VisitPropertiesMode mode) override;
  void Disable() override;
  // thuum docs/research/m3/decisions-after-playtest-34.md D21: a dead actor
  // enabled again resumes its return (the fated clock from the enable when
  // it died out of the world, from its death otherwise)
  void Enable() override;

  void SendToUser(const IMessageBase& message, bool reliable);
  void SendToUserDeferred(const IMessageBase& message, bool reliable,
                          int deferredChannelId,
                          bool overwritePreviousChannelMessages);

  Networking::UserId GetUserId() const;

  // Returns the actor to send messages to: the host of this actor's cell
  // while no user plays it (thuum docs/verbs/cell-host.md).
  // If this actor is offline and has a hoster, returns the hoster.
  // Otherwise returns this actor.
  MpActor& GetActorToSendTo();

  [[nodiscard]] bool OnEquip(uint32_t baseId);
  // thuum docs/verbs/poison.md: the poison `poisonId` put on the weapon
  // worn in the player's right hand (the game's box, reported as the
  // poison's OnEquip): one poison spent and one copy of that weapon marked
  // with it and the server's dose count; false, nothing changed, without
  // the poison, without such a weapon (bare hands and staves take none, as
  // the game refuses them), or with that weapon already poisoned
  bool ApplyPoison(uint32_t poisonId);
  // The poison a copy of `weaponId` carries, one dose spent (the mark
  // cleared at none); nullopt when no copy carries one
  std::optional<uint32_t> SpendPoisonDose(uint32_t weaponId);

  // TODO: consider removing the entire DestroyEventSink feature because it's
  // only used in unit tests
  class DestroyEventSink
  {
  public:
    virtual ~DestroyEventSink() = default;
    virtual void BeforeDestroy(MpActor& actor) = 0;
  };

  void AddEventSink(std::shared_ptr<DestroyEventSink> sink);
  void RemoveEventSink(std::shared_ptr<DestroyEventSink> sink);

  class DisableEventSink
  {
  public:
    virtual ~DisableEventSink() = default;
    virtual void BeforeDisable(MpActor& actor) = 0;
  };

  void AddEventSink(std::shared_ptr<DisableEventSink> sink);
  void RemoveEventSink(std::shared_ptr<DisableEventSink> sink);

  MpChangeForm GetChangeForm() const override;
  void ApplyChangeForm(const MpChangeForm& changeForm) override;

  uint32_t NextSnippetIndex(
    std::optional<Viet::Promise<VarValue>> promise = std::nullopt);

  void ResolveSnippet(uint32_t snippetIdx, VarValue v);
  void SetPercentages(const ActorValues& actorValues,
                      MpActor* aggressor = nullptr);
  void SetPercentage(espm::ActorValue av, float percentage);
  void NetSendChangeValues(
    const ActorValues& actorValues,
    const std::optional<std::vector<espm::ActorValue>>& avFilter);
  void NetSetPercentages(
    const ActorValues& actorValues, MpActor* aggressor,
    const std::optional<std::vector<espm::ActorValue>>& avFilter);

  std::chrono::steady_clock::time_point GetLastAttributesPercentagesUpdate();
  std::chrono::steady_clock::time_point GetLastHitTime(
    std::optional<uint32_t> targetId) const;
  // The last hit this actor took, as the server saw it (an epoch time point
  // when none): with the hits it dealt, the rest rule's sign of a fight.
  std::chrono::steady_clock::time_point GetLastHitTakenTime() const;
  void SetLastHitTakenTime(std::chrono::steady_clock::time_point timePoint);
  // thuum docs/verbs/sleep.md: the last bed this player activated and when
  // (a bed id of 0 when none since its last rest), and the Rested bonus's
  // grant, which its timed removal checks
  void SetLastBed(uint32_t bedId,
                  std::chrono::steady_clock::time_point timePoint);
  std::pair<uint32_t, std::chrono::steady_clock::time_point> GetLastBed()
    const;
  uint64_t NextRestedGrant();
  uint64_t GetRestedGrant() const;
  // thuum docs/verbs/map-markers.md: the map markers this player discovered,
  // kept in its change form; recording one again keeps the wider travel
  // flag, and returns whether anything changed. The pending flag is set at
  // login and taken by the first movement after it, when the client's world
  // is up to show them.
  std::vector<MapMarker> GetMapMarkers() const;
  bool RecordMapMarker(const FormDesc& refr, bool canTravel);
  void SetMapMarkersPending(bool pending);
  bool TakeMapMarkersPending();
  // thuum docs/verbs/learned-effects.md: the ingredient effects this player
  // learned, kept in its change form (recording only adds bits, and returns
  // whether anything changed), and the last ingredient it ate and when (0
  // when none), which a report must follow
  std::vector<IngredientEffects> GetIngredientEffects() const;
  bool RecordIngredientEffects(const FormDesc& ingredient, uint8_t mask);

  // thuum docs/verbs/shouts.md: what the player knows of shouts, the
  // server's record (R0)
  enum class ShoutEntry
  {
    Shout,
    WordKnown,
    WordUnlocked
  };
  ShoutRecord GetShoutRecord() const;
  // Adds the form to the record's list; false when it was there already
  bool RecordShoutEntry(ShoutEntry kind, const FormDesc& form);
  // Whether the record's list holds the form (a form id in the server's
  // load order)
  bool HasShoutEntry(ShoutEntry kind, uint32_t formId) const;
  std::pair<uint32_t, std::chrono::steady_clock::time_point> GetLastEaten()
    const;
  // thuum docs/verbs/favorites.md: the player's favorites, kept in its change
  // form; a kept report replaces them (returns whether anything changed)
  std::vector<Favorite> GetFavorites() const;
  bool SetFavorites(std::vector<Favorite> favorites);

  // thuum docs/verbs/player-globals.md: the globals the player's game wrote
  // for its player, as its change form keeps them; false when unchanged
  std::vector<PlayerGlobal> GetPlayerGlobals() const;
  bool SetPlayerGlobals(std::vector<PlayerGlobal> globals);
  // thuum docs/verbs/racemenu-sync.md: the player's RaceMenu look, its preset
  // JSON, kept in its change form; empty when none (returns whether anything
  // changed)
  std::string GetRaceMenuPreset() const;
  bool SetRaceMenuPreset(std::string preset);
  // thuum docs/verbs/actor-values.md: the player's actor values and
  // progress, kept in its change form (returns whether anything changed);
  // the values the server set that its client has not reported yet; and
  // whether the record a login sent waits for a report that shows it applied
  std::optional<ActorValueRecord> GetActorValueRecord() const;
  bool SetActorValueRecord(ActorValueRecord record);
  const std::vector<std::pair<uint8_t, float>>& GetHeldActorValues() const;
  void SetHeldActorValues(std::vector<std::pair<uint8_t, float>> held);
  bool IsActorValuesLoginPending() const;
  void SetActorValuesLoginPending(bool pending);
  // A base the server sets (R0: a Papyrus native, the gamemode): recorded,
  // held until the player's report carries it, and sent to the player.
  // Before the player's first report there is no record to send: the value
  // waits in the hold and enters the record with that report, which is then
  // sent back. False when wire-rules refuses the value (actor_values_set_ok).
  bool SetActorValueBaseByServer(uint8_t av, float value);
  // The base the server knows for an actor value: the record's, else a value
  // it set and holds; none before either
  std::optional<float> GetRecordedActorValueBase(uint8_t av) const;
  // The record to the player, as after a login but without waiting for a
  // report to show it applied
  void SendActorValueRecord();
  // The server takes a RaceMenu look only from a race menu it opened, the
  // rule SkyMP keeps for an appearance (ActionListener::OnUpdateAppearance):
  // one look per opening, whether it comes before or after the appearance
  // that closes the menu, and one per login while the record keeps the menu
  // open. Answers whether one is due and takes it.
  bool TakeRaceMenuLookDue();
  size_t CountRecentHits(std::chrono::duration<float> timeWindow) const;

  void SetLastAttributesPercentagesUpdate(
    std::chrono::steady_clock::time_point timePoint =
      std::chrono::steady_clock::now());
  void SetLastHitTime(uint32_t targetId,
                      std::chrono::steady_clock::time_point timePoint);

  std::chrono::duration<float> GetDurationOfAttributesPercentagesUpdate(
    std::chrono::steady_clock::time_point now);

  void Kill(MpActor* killer = nullptr, bool shouldTeleport = false);
  // the killer the server credited at this actor's last death (nothing
  // before any death; 0 for a death with none), the read of H-52b's credit
  std::optional<uint32_t> LastKillerId() const;
  // thuum docs/verbs/npc-fate.md (ADR-050 decision 2, deaths by the engine):
  // the game hosting this NPC (nullptr for a player's actor, an unhosted
  // actor or a player's own)
  MpActor* HostedBy();
  // a kill the server decided for a hosted NPC: the cause (health 0,
  // killMove) to the host and the kill deferred to its report, else Kill
  void KillOrDefer(MpActor* killer, bool killMove);
  // the server's number reached zero: the host's death report is awaited
  // for kPendingDeathMs, then the kill is the server's word (TickDeaths)
  void BeginPendingDeath(MpActor* aggressor);
  bool HasPendingDeath() const;
  void SendValuesToOtherListeners(const ActorValues& values,
                                  const std::vector<espm::ActorValue>& touched,
                                  MpActor* host);
  uint64_t PendingDeathSinceMs() const;
  uint32_t PendingDeathAggressor() const;
  static constexpr uint64_t kPendingDeathMs = 3000;
  void Respawn(bool shouldTeleport = true);
  void RespawnWithDelay(bool shouldTeleport = true);
  // thuum docs/verbs/npc-fate.md (the deaths): every actor that is not a
  // player's, a game-file actor (an id under 0xff000000) or a server-made
  // actor with an NPC base (playtest 34's finding), keeps to the engine's
  // reset clock: its body lies from fateDiedAt until
  // WorldState::FateRespawnHours() later, then stands at its placement (a
  // server-made actor at its spawn point) rolled anew (PartOne::
  // TickFateRespawns); a unique base stays dead, the engine's rule
  bool IsFateRespawned() const;
  void ScheduleFateRespawn();
  void RespawnFated();
  // the engine's rule for a game-file base: essential never dies, protected
  // dies to a player's own hand alone; a player's or a runtime actor may
  bool MayDie(MpActor* aggressor);
  // thuum docs/verbs/npc-fate.md (ADR-048's B, the host's checked death
  // report): what the server saw of this actor lately, the evidence a
  // report is checked against. The hits it took, by aggressor, from the
  // hit reports the server numbered (OnWeaponHit, OnSpellHit); its heights
  // from its host's movement samples, the last kHeightWindow of them
  void NoteHitBy(uint32_t aggressorId,
                 std::chrono::steady_clock::time_point now);
  // how long since `aggressorId` last hit this actor (any aggressor when
  // 0); nothing when the server saw no such hit
  std::optional<std::chrono::milliseconds> HitByAgo(
    uint32_t aggressorId, std::chrono::steady_clock::time_point now) const;
  // the aggressor of the latest hit the server numbered within `window`;
  // nothing when none (npc-fate.md H-52b, train 54: a death report naming
  // no killer after a lethal single blow takes its killer from the server's
  // own number)
  std::optional<uint32_t> LastHitBy(std::chrono::steady_clock::time_point now,
                                    std::chrono::milliseconds window) const;
  void RecordHeight(float z, std::chrono::steady_clock::time_point now);
  // the highest sample within the window minus `z`; nothing without samples
  std::optional<float> DropTo(float z,
                              std::chrono::steady_clock::time_point now) const;
  static constexpr std::chrono::seconds kHeightWindow{ 4 };
  // thuum docs/verbs/npc-fate.md (playtest 37's finding 48): when this actor
  // last died on the server's clock, the grace a host's live sample of a
  // dead record is read against (ActionListener::OnHostedActorState); every
  // death passes SendAndSetDeathState, which records it; tests backdate it
  void RecordDeathAt(std::chrono::steady_clock::time_point at);
  std::optional<std::chrono::milliseconds> DiedAgo(
    std::chrono::steady_clock::time_point now) const;
  // thuum docs/verbs/npc-fate.md ("Reanimation is any player's, on any
  // body"; finding 65's placed corpses): when the server last accepted a
  // reanimate hit on this body (ActionListener::OnHit, ahead of
  // CastReanimateOnHost). A placement that starts dead has no death to
  // grace and no engine stand-up of its own to record, so its host's live
  // sample stands it on the record only within kReanimateWindowMs of an
  // accepted reanimate: the cast's rule, not the sample's
  // (ActionListener::OnHostedActorState)
  void NoteReanimateAccepted(std::chrono::steady_clock::time_point at);
  std::optional<std::chrono::milliseconds> ReanimateAcceptedAgo(
    std::chrono::steady_clock::time_point now) const;
  // the host's round trip, the cast's flight on its engine and the sampler's
  // cadence, with room; a bound of the server's policy, not an engine number
  static constexpr uint64_t kReanimateWindowMs = 10'000;
  void Teleport(const LocationalData& position);
  void SetSpawnPoint(const LocationalData& position);
  LocationalData GetSpawnPoint() const;
  LocationalData GetEditorLocationalData() const;
  const float GetRespawnTime() const;
  void SetRespawnTime(float time);

  void SetIsDead(bool isDead);

  void RestoreActorValue(espm::ActorValue av, float value);
  void DamageActorValue(espm::ActorValue av, float value);
  void SetActorValue(espm::ActorValue actorValue, float value);

  // TODO: only used in legacy MGEF implementation, remove when MGEF is
  // rewritten
  void SetActorValues(const ActorValues& actorValues);

  BaseActorValues GetBaseValues();
  BaseActorValues GetMaximumValues();
  // thuum docs/verbs/figure-values.md: the maxima the other games set as
  // this player's figure's base Health, Magicka and Stamina (the recorded
  // bases 24, 25, 26 with the running buffs: GetMaximumValues), present once
  // a record exists; before it the figure keeps the engine's defaults
  std::optional<BaseActorValues> RecordedMaxima();
  // ... and their trip to every game that sees the figure, when they change
  void SendMaximaToListeners();

  void DropItem(const uint32_t baseId, const Inventory::Entry& entry);
  void SetIsBlockActive(bool isBlockActive);
  bool IsBlockActive() const;
  NiPoint3 GetViewDirection() const;
  void IncreaseBlockCount() noexcept;
  void ResetBlockCount() noexcept;
  uint32_t GetBlockCount() const noexcept;
  void ApplyMagicEffect(espm::Effects::Effect& effect,
                        bool hasSweetpie = false,
                        bool durationOverriden = false);
  void ApplyMagicEffects(std::vector<espm::Effects::Effect>& effects,
                         bool hasSweetpie = false,
                         bool durationOverriden = false);
  void RemoveMagicEffect(const espm::ActorValue actorValue);
  void RemoveAllMagicEffects();
  void ReapplyMagicEffects();

  // thuum docs/verbs/magic-effects.md: the effect rule (wire-rules magic)
  // over the actor's running effects, kept in its change form. `source`'s
  // effects (its record's list) apply at `scale` times their magnitudes,
  // by `caster` (0 for none); `stream` for a concentration spell's hit,
  // which counts at once. Archetypes other than value, peak value and dual
  // value modifiers are left alone. `streamSeconds` above 0 runs every
  // effect for that many seconds at its magnitude a second, the record's
  // own duration set aside: a Self concentration cast's stream (thuum
  // docs/verbs/self-cast.md, train-31)
  void ApplyEffects(uint32_t source,
                    const std::vector<espm::Effects::Effect>& effects,
                    float scale, uint32_t caster, bool stream,
                    bool dualCast = false, float streamSeconds = 0.f);
  // Runs the actor's effects for `dtSeconds` of the server's wall clock
  // (WorldState's effects tick; at a load, the time since the record's)
  void AdvanceEffects(float dtSeconds);
  bool HasRunningEffects() const;
  // One running effect as a gamemode reads it (mp.get(actor,
  // "runningEffects")): its MGEF, its source, its magnitude and the
  // seconds it has left
  struct RunningEffectView
  {
    uint32_t effect = 0;
    uint32_t source = 0;
    float magnitude = 0.f;
    float secondsLeft = 0.f;
  };
  std::vector<RunningEffectView> GetRunningEffects() const;
  // The running buffs' sum on `av`: the temporary modifier the maximum and
  // the regeneration rates count
  float GetEffectModifier(espm::ActorValue av) const;
  // The actor value as the engine's GetActorValue answers it: Health,
  // Magicka and Stamina as the maximum times the kept percentage; any
  // other as the player's recorded base (0 with no record) plus the
  // modifier above (thuum docs/verbs/spell-cast.md, train-30)
  float GetCurrentActorValue(espm::ActorValue av);
  // The share of a hostile effect's magnitude this actor takes, by its
  // resistances (thuum docs/verbs/magic-effects.md, train-30): 1 for an
  // effect that is not hostile, a spell that ignores resistance, or an
  // effect whose resist value this actor does not carry
  float ResistanceFactorOf(uint32_t source,
                           const espm::Effects::Effect& effect);
  // Whether a running effect's MGEF is of `type` (thuum
  // docs/verbs/self-cast.md: an ethereal actor takes no damage)
  bool HasRunningEffectOfType(espm::MGEF::EffectType type) const;
  // Ends the running effects whose MGEF is of one of `types` before their
  // time, a buff's modifier given back, every game told; whether any ended
  bool EndRunningEffectsOfTypes(
    std::initializer_list<espm::MGEF::EffectType> types);
  // Ends the running effects `source` (a spell, a potion) carries before
  // their time, the same way; whether any ended (thuum
  // docs/verbs/shadowbond.md: the bond's proc on the actor that did not
  // act, its own Invisibility spell spared)
  bool EndRunningEffectsOfSource(uint32_t source);
  // The effect list of a record that carries one (a potion, a poison, a
  // spell, an enchantment, a scroll, an ingredient: EFID, EFIT and their
  // CTDA), each MGEF id as the load order numbers it; empty for none
  // thuum docs/verbs/self-cast.md (r-shock-nova 20261018-013824): an effect
  // with an area whose record is detrimental, the hostile part of a Self
  // burst, which the engine keeps off the burst's own caster
  static bool IsHostileArea(WorldState* worldState,
                            const espm::Effects::Effect& effect);
  static std::vector<espm::Effects::Effect> EffectsOf(WorldState* worldState,
                                                      uint32_t source);
  // thuum docs/verbs/magic-effects.md: whether an effect waits on
  // conditions the server does not evaluate yet: the spell's own gate on
  // its entry, or a perk the effect record's conditions name, the way
  // vanilla gates its perks' riders (the Impact perk's stagger,
  // Disintegrate's 200 Health). The server keeps no perks before M5, so
  // such a rider stays out
  static bool IsConditionGated(WorldState* worldState,
                               const espm::Effects::Effect& effect);
  // thuum docs/DECISIONS.md ADR-039: whether the effect's conditions (the
  // spell's on its entry and the effect record's own) hold for `victim`
  // when `caster` applies it: Subject the actor the effect lands on, Target
  // the caster (the engine's reading for a magic effect's conditions,
  // HYPOTHESIS until a lab run shows a Target row; CK wiki "Condition
  // Functions"). A function the server cannot evaluate refuses the effect
  // and logs E_CONDITION_UNKNOWN; no conditions hold trivially. A refusal
  // names the rows that fail on their own (failedRow, the text its line
  // carries after "failing:"; the pile's "refusal line naming its row",
  // 2026-10-31)
  static bool EffectConditionsHold(WorldState* worldState,
                                   const espm::Effects::Effect& effect,
                                   uint32_t source, const MpActor& victim,
                                   const MpActor& caster,
                                   bool dualCast = false,
                                   std::string* failedRow = nullptr);
  // thuum docs/verbs/god-mode.md: TGM and TIM, the server's flags for the
  // player's session (cleared at its departure). A god actor takes no
  // damage from another actor and spends no arrow; an immortal actor's
  // health stops at one point instead of reaching nothing
  bool IsGodMode() const noexcept;
  void SetGodMode(bool on) noexcept;
  bool IsImmortal() const noexcept;
  void SetImmortal(bool on) noexcept;
  // MagicEffects to `listener`'s player: the effects running on this actor,
  // for a game that first sees it (MpObjectReference::Subscribe)
  void SendMagicEffectsTo(MpActor& listener);

  bool GetConsoleCommandsAllowedFlag() const;
  void SetConsoleCommandsAllowedFlag(bool newValue);
  // thuum docs/verbs/console-commands.md: the player's staff rank (0 player,
  // 1 moderator, 2 admin, 3 owner): the recorded one, else admin when
  // consoleCommandsAllowed is set, else player
  uint8_t GetStaffRank() const;
  std::optional<uint8_t> GetRecordedStaffRank() const;
  void SetStaffRank(uint8_t rank);

  void EquipBestWeapon();

  void AddSpell(uint32_t spellId);
  void RemoveSpell(uint32_t spellId);

  void SetLastAnimEvent(const std::optional<AnimationData>& animationData);
  std::optional<AnimationData> GetLastAnimEvent() const;

  // thuum docs/verbs/npc-stream.md (playtest 35): the last action a host
  // performed on this NPC (UpdateAction's fields), memory by design, carried
  // by CreateActor to a seat that joins the cell after the action began
  void SetLastAction(const UpdateActionMessage& action);
  std::optional<UpdateActionMessage> GetLastAction() const;

  // thuum docs/verbs/npc-stream.md (ADR-050, slice 2): the percentages a
  // host's state report writes once wire-rules hosted_state judged them
  // (each present value written as given: no kill, no respawn check, the
  // judging done; the attributes stamp refreshed), and the host's reading of
  // the actor's combat target (a form id the server knows, 0 for none) and
  // running package (0 for none), memory by design (R2 records for
  // npc-handoff and the lab, forgotten at a restart)
  void SetJudgedPercentages(std::optional<float> health,
                            std::optional<float> magicka,
                            std::optional<float> stamina);
  void SetHostedState(uint32_t combatTarget, uint32_t package);
  uint32_t GetHostedCombatTarget() const;
  uint32_t GetHostedPackage() const;
  // slice 5: the host's reading of the actor's gaze and face, recorded (R2)
  void SetHostedGaze(uint32_t target, uint32_t slot, uint32_t emotionType,
                     uint32_t emotionValue);
  uint32_t GetHostedGazeTarget() const;
  uint32_t GetHostedGazeSlot() const;
  uint32_t GetHostedEmotionType() const;
  uint32_t GetHostedEmotionValue() const;

private:
  // the two EndRunningEffects above: removes the MGEFs named, gives a buff's
  // modifier back, tells every game; false for none
  bool EndRunningEffectIds(const std::vector<uint32_t>& ending);
  // thuum docs/verbs/god-mode.md: `healthPercentage`, or one point for an
  // immortal actor whose health would reach nothing
  float ImmortalHealth(float healthPercentage);

  // thuum docs/verbs/magic-effects.md: Health, Magicka and Stamina moved by
  // `deltas` (points, against the maximum before the change, `maxBefore`,
  // and the maximum after it), each only where `touched`; a kill, if any,
  // by `caster`
  void ApplyAttributeDeltas(const std::array<float, 3>& deltas,
                            const std::array<bool, 3>& touched,
                            const BaseActorValues& maxBefore, uint32_t caster);

  struct Impl;
  std::shared_ptr<Impl> pImpl;
  bool factionsLoaded = false;

  void SendAndSetDeathState(bool isDead, bool shouldTeleport);
  void RebuildAndRespawn(bool shouldTeleport, bool rebuildAlways);

  DeathStateContainerMessage GetDeathStateMsg(const LocationalData& position,
                                              bool isDead,
                                              bool shouldTeleport);

  void EatItem(uint32_t baseId, espm::Type t);

  bool ReadBook(uint32_t baseId);

  void ModifyActorValuePercentage(espm::ActorValue av, float percentageDelta);

  void EnsureTemplateChainEvaluated(
    espm::Loader& loader,
    ChangeFormGuard::Mode mode = ChangeFormGuard::Mode::RequestSave);

  std::map<uint32_t, uint32_t> EvaluateDeathItem();
  void AddDeathItem();
  void LoadFactions();

protected:
  void BeforeDestroy() override;
  void Init(WorldState* parent, uint32_t formId, bool hasChangeForm) override;
};
