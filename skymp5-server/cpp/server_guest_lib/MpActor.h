#pragma once
#include "AnimationData.h"
#include "Appearance.h"
#include "Equipment.h"
#include "Favorite.h"
#include "GetBaseActorValues.h"
#include "IngredientEffects.h"
#include "MapMarker.h"
#include "MpObjectReference.h"
#include "libespm/espm.h"
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

  bool IsSpellLearned(uint32_t spellId) const; // including from base
  bool IsSpellLearnedFromBase(uint32_t spellId) const;
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
  bool IsCreatedAsPlayer() const;
  const ActorValues& GetActorValues() const;
  const ActiveMagicEffectsMap& GetActiveMagicEffects() const;
  int32_t GetProfileId() const;

  float GetHealthRespawnPercentage() const;
  float GetMagickaRespawnPercentage() const;
  float GetStaminaRespawnPercentage() const;

  bool ShouldSkipRestoration() const noexcept;
  void UpdateNextRestorationTime(std::chrono::seconds duration) noexcept;

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

  void SendToUser(const IMessageBase& message, bool reliable);
  void SendToUserDeferred(const IMessageBase& message, bool reliable,
                          int deferredChannelId,
                          bool overwritePreviousChannelMessages);

  Networking::UserId GetUserId() const;

  // Returns the actor to send messages to, considering hosters.
  // If this actor is offline and has a hoster, returns the hoster.
  // Otherwise returns this actor.
  MpActor& GetActorToSendTo();

  [[nodiscard]] bool OnEquip(uint32_t baseId);

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
  std::pair<uint32_t, std::chrono::steady_clock::time_point> GetLastEaten()
    const;
  // thuum docs/verbs/favorites.md: the player's favorites, kept in its change
  // form; a kept report replaces them (returns whether anything changed)
  std::vector<Favorite> GetFavorites() const;
  bool SetFavorites(std::vector<Favorite> favorites);
  // thuum docs/verbs/racemenu-sync.md: the player's RaceMenu look, its preset
  // JSON, kept in its change form; empty when none (returns whether anything
  // changed)
  std::string GetRaceMenuPreset() const;
  bool SetRaceMenuPreset(std::string preset);
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
  void Respawn(bool shouldTeleport = true);
  void RespawnWithDelay(bool shouldTeleport = true);
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

  bool GetConsoleCommandsAllowedFlag() const;
  void SetConsoleCommandsAllowedFlag(bool newValue);

  void EquipBestWeapon();

  void AddSpell(uint32_t spellId);
  void RemoveSpell(uint32_t spellId);

  void SetLastAnimEvent(const std::optional<AnimationData>& animationData);
  std::optional<AnimationData> GetLastAnimEvent() const;

private:
  struct Impl;
  std::shared_ptr<Impl> pImpl;
  bool factionsLoaded = false;

  void SendAndSetDeathState(bool isDead, bool shouldTeleport);

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
