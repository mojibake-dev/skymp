#pragma once
#include "AnimationData.h"
#include "ConsoleCommands.h"
#include "CraftService.h"
#include "Messages.h"
#include "MpActor.h"
#include "PartOne.h"
#include "RawMessageData.h"
#include "SpellCastData.h"
#include "SweetHidePlayerNamesService.h"
#include "libespm/Loader.h"
#include <deque>
#include <map>
#include <memory>
#include <utility>

class ServerState;
class WorldState;
struct ActorValues;

class ActionListener
{
public:
  ActionListener(PartOne& partOne_)
    : partOne(partOne_)
  {
    craftService = std::make_shared<CraftService>(partOne_);
    sweetHidePlayerNamesService =
      std::make_shared<SweetHidePlayerNamesService>(partOne_);
  }

  virtual void OnCustomPacket(const RawMessageData& rawMsgData,
                              const CustomPacketMessage& msg);

  virtual void OnUpdateMovement(const RawMessageData& rawMsgData,
                                const UpdateMovementMessage& msg);

  virtual void OnUpdateAnimation(const RawMessageData& rawMsgData,
                                 const UpdateAnimationMessage& msg);
  // thuum docs/verbs/cast-action.md: an action the player's actor performed,
  // relayed as received to the actor's neighbours but the sender, reliably
  virtual void OnUpdateAction(const RawMessageData& rawMsgData,
                              const UpdateActionMessage& msg);
  virtual void OnUpdateAppearance(const RawMessageData& rawMsgData,
                                  const UpdateAppearanceMessage& msg);
  virtual void OnUpdateEquipment(const RawMessageData& rawMsgData,
                                 const UpdateEquipmentMessage& msg);

  virtual void OnActivate(const RawMessageData& rawMsgData,
                          const ActivateMessage& msg);

  virtual void OnPutItem(const RawMessageData& rawMsgData,
                         const PutItemMessage& msg);
  virtual void OnTakeItem(const RawMessageData& rawMsgData,
                          const TakeItemMessage& msg);
  virtual void OnDropItem(const RawMessageData& rawMsgData,
                          const DropItemMessage& msg);

  virtual void OnPlayerBowShot(const RawMessageData& rawMsgData,
                               const PlayerBowShotMessage& msg);

  virtual void OnRestIntent(const RawMessageData& rawMsgData,
                            const RestIntentMessage& msg);

  // thuum docs/verbs/map-markers.md
  virtual void OnMapMarkerDiscovered(const RawMessageData& rawMsgData,
                                     const MapMarkerDiscoveredMessage& msg);

  // thuum docs/verbs/learned-effects.md
  virtual void OnIngredientEffectsKnown(
    const RawMessageData& rawMsgData,
    const IngredientEffectsKnownMessage& msg);

  // thuum docs/verbs/favorites.md
  virtual void OnFavorites(const RawMessageData& rawMsgData,
                           const FavoritesMessage& msg);

  // thuum docs/verbs/racemenu-sync.md
  virtual void OnRaceMenuPreset(const RawMessageData& rawMsgData,
                                const RaceMenuPresetMessage& msg);

  // thuum docs/verbs/actor-values.md
  virtual void OnActorValues(const RawMessageData& rawMsgData,
                             const ActorValuesMessage& msg);

  // thuum docs/verbs/player-globals.md
  virtual void OnPlayerGlobals(const RawMessageData& rawMsgData,
                               const PlayerGlobalsMessage& msg);

  // thuum docs/verbs/cell-host.md: a game's cell loaded or left its loaded
  // set; the server's election follows (wire-rules cells)
  virtual void OnCellLoaded(const RawMessageData& rawMsgData,
                            const CellLoadedMessage& msg);

  // thuum docs/verbs/npc-puppet.md: a host's roster of game-file actors the
  // server does not hold where its engine has them; each entry judged by
  // PartOne::OnActorSighting
  virtual void OnActorSightings(const RawMessageData& rawMsgData,
                                const ActorSightingsMessage& msg);

  // thuum docs/verbs/npc-fate.md (ADR-048's B): a host's report that a
  // game-file actor it simulates died of something the server has no
  // number for; taken when the server can check the cause against what it
  // saw (a hit it numbered, a drop in the host's samples, an activation
  // beside the body), refused and logged otherwise
  virtual void OnActorDeath(const RawMessageData& rawMsgData,
                            const ActorDeathMessage& msg);

  // thuum docs/verbs/npc-stream.md (ADR-050 decision 1, slice 2): a host's
  // state for an actor it simulates: the values judged against the server's
  // own numbers (wire-rules hosted_state: a report that would undo a value
  // the server lowered or raised lately is dropped, a 0 on a living record
  // floored at one point), the combat target and the package recorded, the
  // changed values sent as ChangeValues to the actor's other listeners
  virtual void OnHostedActorState(const RawMessageData& rawMsgData,
                                  const HostedActorStateMessage& msg);

  // thuum docs/verbs/npc-stream.md (the voice slice): a spoken line begins
  // on an actor the sender hosts; the host gate, then the bytes relayed
  // unreliable to the actor's other listeners (the puppets say the line);
  // nothing recorded
  // thuum docs/verbs/npc-talk.md (ADR-052): a player's claim on an NPC for a
  // dialogue, or its release
  virtual void OnActorClaim(const RawMessageData& rawMsgData,
                            const ActorClaimMessage& msg);
  virtual void OnActorSpeech(const RawMessageData& rawMsgData,
                             const ActorSpeechMessage& msg);

  // the activations the server accepted lately, by place: a hazard death's
  // evidence (ADR-048's B; that a trap's trigger reaches the server as an
  // activation is a HYPOTHESIS until the lab's trap run)
  void NoteActivation(const MpObjectReference& target,
                      std::chrono::steady_clock::time_point now);
  bool ActivationSeenNear(const MpObjectReference& where, const NiPoint3& pos,
                          std::chrono::steady_clock::time_point now) const;

  virtual void OnFinishSpSnippet(const RawMessageData& rawMsgData,
                                 const FinishSpSnippetMessage& msg);

  virtual void OnEquip(const RawMessageData& rawMsgData,
                       const OnEquipMessage& msg);

  virtual void OnConsoleCommand(const RawMessageData& rawMsgData,
                                const ConsoleCommandMessage& msg);

  virtual void OnCraftItem(const RawMessageData& rawMsgData,
                           const CraftItemMessage& msg);

  virtual void OnHostAttempt(const RawMessageData& rawMsgData,
                             const HostMessage& msg);

  virtual void OnCustomEvent(const RawMessageData& rawMsgData,
                             const CustomEventMessage& msg);

  virtual void OnChangeValues(const RawMessageData& rawMsgData,
                              const ChangeValuesMessage& msg);

  virtual void OnHit(const RawMessageData& rawMsgData, const HitMessage& msg);

  virtual void OnUpdateAnimVariables(const RawMessageData& rawMsgData,
                                     const UpdateAnimVariablesMessage& msg);

  virtual void OnSpellCast(const RawMessageData& rawMsgData,
                           const SpellCastMessage& msg);

  virtual void OnUnknown(const RawMessageData& rawMsgData);

  // for CraftTest.cpp
  const std::shared_ptr<CraftService>& GetCraftService() noexcept
  {
    return craftService;
  }

  // thuum docs/verbs/player-globals.md: the globals the server keeps per
  // player, for its client to watch, and their recorded values (with the
  // settings' defaults where the record is silent), for its game to set
  // after a login. Public since train-30: PartOne::SetUserActor sends it at
  // the login itself, ahead of the other login sends.
  void SendPlayerGlobals(MpActor& actor);

private:
  // magnitudeScale: the claimed cast's factor on the spell's magnitude
  // (thuum docs/verbs/spell-cast.md): 1 for a fire-and-forget hit, seconds
  // for a stream's
  // dualCast: the claimed cast was dual cast (EffectWasDualCast 724)
  void OnSpellHit(MpActor* aggressor, MpObjectReference* targetRef,
                  const HitData& hitData, float magnitudeScale = 1.f,
                  bool dualCast = false);
  // damageMult: a claimed arrow's draw power (thuum docs/verbs/marksman.md),
  // 1 for every other hit
  void OnWeaponHit(MpActor* aggressor, MpObjectReference* targetRef,
                   HitData hitData, bool isUnarmed, float damageMult = 1.f);

  void GrantRested(MpActor& actor, uint64_t durationMs);
  // thuum docs/verbs/map-markers.md: AddToMap on its client for each marker
  // the player has discovered
  void SendMapMarkers(MpActor& actor);
  // thuum docs/verbs/learned-effects.md: Ingredient.LearnEffect on its client
  // for each effect the player has learned
  void SendIngredientEffects(MpActor& actor);
  // thuum docs/verbs/favorites.md: the player's favorites, for its client's
  // engine to mark
  void SendFavorites(MpActor& actor);
  // thuum docs/verbs/racemenu-sync.md: the player's RaceMenu look, for its
  // own client to apply after a login
  void SendRaceMenuPreset(MpActor& actor);
  // thuum docs/verbs/actor-values.md: the player's actor values and
  // progress, for its client's engine to apply after a login; the record is
  // held until a report shows it applied
  void SendActorValues(MpActor& actor);

  void NotifyHostility(MpActor& aggressor, MpActor& target,
                       std::chrono::steady_clock::time_point now);
  // thuum docs/verbs/stagger.md: after a hit's damage, the stagger the
  // engine's rule gives it, played in the victim's game
  void NotifyStagger(MpActor& aggressor, MpActor& target,
                     const HitData& hitData, bool dealtDamage);
  // the stagger of `magnitude` played in the target's game, the aggressor's
  // figure its source, and relayed to every game that sees the target
  // (thuum docs/verbs/stagger.md); `what` names the hit for the log
  void PlayStagger(MpActor& aggressor, MpActor& target, float magnitude,
                   const std::string& what);

  void SendPapyrusOnHitEvent(MpActor* aggressor, MpObjectReference* target,
                             const HitData& hitData);

  // The actor at idx if this user may update it (its own actor, or an NPC
  // it hosts); otherwise logs, sends HostStop where that applies, and
  // returns nullptr.
  // `msgType` names the stream row the outcome counts in (slice 4's
  // counters, ServerState::CountStream); Invalid counts nothing
  MpActor* ActorUpdatableBy(uint32_t idx, Networking::UserId userId,
                            MsgType msgType = MsgType::Invalid);

  struct RecentActivation
  {
    std::chrono::steady_clock::time_point at;
    FormDesc cellOrWorld;
    NiPoint3 pos;
  };
  std::deque<RecentActivation> recentActivations;

  // The message, as received, to every user listening to the actor. A
  // hosted actor's report is not echoed to the game that sent it (thuum
  // docs/verbs/cell-host.md: its engine moved the actor, and the echo
  // replayed the actor's own turn events on it, playtest thirty's spinning
  // wolves); a player's own actor keeps its echo as before.
  void RelayToListeners(MpActor& actor, Networking::PacketData data,
                        size_t length, bool reliable,
                        Networking::UserId sender = Networking::InvalidUserId);

  // ActorUpdatableBy, then RelayToListeners. Returns the actor or nullptr.
  MpActor* SendToNeighbours(uint32_t idx, Networking::UserId userId,
                            Networking::PacketData data, size_t length,
                            bool reliable, MsgType msgType = MsgType::Invalid);

  MpActor* SendToNeighbours(uint32_t idx, const RawMessageData& rawMsgData,
                            bool reliable = false,
                            MsgType msgType = MsgType::Invalid);

  // thuum docs/verbs/spell-cast.md: a cast as the server resolved it (its
  // caster a form id, never the sender's 0x14) to every user listening to
  // the caster but the sender, reliably, as a bow shot's relay
  void RelayCast(MpActor& caster, const MpActor& sender,
                 const SpellCastData& data);

  PartOne& partOne;
  // thuum docs/verbs/spell-cast.md (train-27): a cast delivered to its
  // target (touch, target actor) whose relay waits for its first hit, by
  // the caster's form id and hand; the hit's claim (CastClaim::relay)
  // sends it with the target the hit names, a new cast in the hand
  // replaces it, its stream's end drops it unsent
  std::map<std::pair<uint32_t, uint8_t>, SpellCastData> pendingCastRelays;

  // TODO: inverse dependency
  std::shared_ptr<CraftService> craftService;
  std::shared_ptr<SweetHidePlayerNamesService> sweetHidePlayerNamesService;
};
