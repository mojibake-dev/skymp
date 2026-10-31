#include "PartOne.h"
#include "ActorFateMessage.h"
#include "LocationalData.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "CreateActorMessage.h"
#include "CustomPacketMessage.h"
#include "DestroyActorMessage.h"
#include "HostGrantMessage.h"
#include "HostReleaseMessage.h"
#include "PlacedRequestMessage.h"
#include "PropOwnerMessage.h"
#include "RaceMenuPresetMessage.h"
#include "SetGameTimeMessage.h"
#include "SetRaceMenuOpenMessage.h"
#include "SpellCastMessage.h"
#include "UpdateGameModeDataMessage.h"

#include "ActionListener.h"
#include "FormCallbacks.h"
#include "MessageSerializerFactory.h"
#include "OpenSSLSigner.h"
#include "PacketParser.h"
#include "SpSnippet.h"
#include "libespm/ARMO.h"
#include "libespm/CELL.h"
#include "libespm/MGEF.h"
#include "libespm/PROJ.h"
#include "libespm/SPEL.h"
#include "libespm/ScriptData.h"
#include "papyrus-vm/Utils.h"
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

namespace {
// The wall clock, Unix milliseconds: the game clock is a function of it
int64_t UnixNowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::system_clock::now().time_since_epoch())
    .count();
}

// A monotonic clock in milliseconds, for the movement rule
uint64_t SteadyNowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// how often the placements' clock runs from Tick (thuum
// docs/verbs/spell-placed-object.md): a listed cast's 250 ms wait lands
// within a tick of it
constexpr std::chrono::milliseconds kPlacementsTick{ 50 };

// thuum docs/verbs/npc-fate.md (ADR-049 decision 3): one actor's fate as the
// wire carries it: the placement, the NPC_ the roll is pinned to (the
// chain's first leveled pick, else the placed base), the roll's level (0 for
// the old rule's) and the worn list, at most the wire's 32 entries
constexpr size_t kFatesPerMessage = 64;
constexpr size_t kFateWornMax = 32;

ActorFateMessage::Fate FateOf(const WorldState& worldState,
                              const MpActor& actor)
{
  ActorFateMessage::Fate fate;
  fate.refrId = actor.GetFormId();
  fate.baseId = actor.GetFateBaseId();
  fate.level = actor.GetFateLevel().value_or(0);
  for (const auto& entry : actor.GetEquipment().inv.entries) {
    if (entry.GetWorn() != Inventory::Worn::None &&
        fate.worn.size() < kFateWornMax) {
      fate.worn.push_back(entry.baseId);
    }
  }
  return fate;
}

SetGameTimeMessage ToMessage(const skymp::rules::GameTime& t)
{
  SetGameTimeMessage message;
  message.year = t.year;
  message.month = t.month;
  message.day = t.day;
  message.hour = t.hour;
  message.daysPassed = t.days_passed;
  message.timeScale = t.time_scale;
  return message;
}
}

PartOneSendTargetWrapper::PartOneSendTargetWrapper(
  Networking::ISendTarget& underlyingSendTarget_)
  : underlyingSendTarget(underlyingSendTarget_)
{
}

void PartOneSendTargetWrapper::Send(Networking::UserId targetUserId,
                                    Networking::PacketData data, size_t length,
                                    bool reliable)
{
  return underlyingSendTarget.Send(targetUserId, data, length, reliable);
}

void PartOneSendTargetWrapper::Send(Networking::UserId targetUserId,
                                    const IMessageBase& message, bool reliable)
{
  std::string stream;

  PartOne::GetMessageSerializerInstance().Serialize(message, stream);

  Send(targetUserId, reinterpret_cast<Networking::PacketData>(stream.data()),
       stream.size(), reliable);
}

class FakeSendTarget : public Networking::ISendTarget
{
public:
  void Send(Networking::UserId targetUserId, Networking::PacketData data,
            size_t length, bool reliable) override
  {
    std::shared_ptr<IMessageBase> message;

    auto deserializeResult =
      PartOne::GetMessageSerializerInstance().Deserialize(data, length);
    nlohmann::json j;
    if (deserializeResult) {
      deserializeResult->message->WriteJson(j);
      message = std::move(deserializeResult->message);
    } else {
      std::string s(reinterpret_cast<const char*>(data + 1), length - 1);
      j = nlohmann::json::parse(s);
    }

    messages.push_back(PartOne::Message{ j, message, targetUserId, reliable });
  }

  std::vector<PartOne::Message> messages;
};

struct PartOne::Impl
{
  simdjson::dom::parser parser;

  // thuum docs/verbs/npc-puppet.md (finding 66): sightings dropped because
  // their cell had no host yet, per cell and one per (reporter, reference),
  // until the grant replays the host's own (PartOne::ReplayHeldSightings),
  // kHeldSightingMs passes (SweepHeldSightings), the reporter leaves
  // (ForgetHeldSightings) or the total cap makes room (HoldSighting)
  struct HeldSighting
  {
    uint32_t reporter = 0;
    uint32_t refrId = 0;
    uint32_t worldOrCell = 0;
    NiPoint3 pos;
    NiPoint3 rot;
    uint64_t heldAtMs = 0;
  };
  std::unordered_map<uint32_t, std::vector<HeldSighting>> heldSightings;

  // every player actor's ground speed budget (thuum
  // docs/verbs/movement-speed.md; the rule is Rust's, ADR-020)
  rust::Box<skymp::rules::MovementBudgets> movementBudgets =
    skymp::rules::new_movement_budgets();

  // every player actor's recent bow and crossbow shots (thuum
  // docs/verbs/marksman.md; Rust's, ADR-020)
  rust::Box<skymp::rules::RangedShots> rangedShots =
    skymp::rules::new_ranged_shots();

  // every player actor's recent spell casts and open streams (thuum
  // docs/verbs/spell-cast.md; Rust's, ADR-020)
  rust::Box<skymp::rules::SpellCasts> spellCasts =
    skymp::rules::new_spell_casts();

  // every player actor's voice recovery (thuum docs/verbs/shouts.md; Rust's,
  // ADR-020)
  rust::Box<skymp::rules::Voices> voices = skymp::rules::new_voices();

  // the server's game clock (thuum docs/verbs/time.md; Rust's, ADR-020 and
  // ADR-021); players hear it once SetGameTimeSettings has run
  rust::Box<skymp::rules::GameClock> gameClock =
    skymp::rules::new_game_clock("{}");
  bool gameTimeBroadcast = false;

  // TES3MP's rest switches (thuum docs/verbs/rest.md): server-settings.json's
  // `rest` block, both on unless a server turns one off
  rust::Box<skymp::rules::RestSettings> restSettings =
    skymp::rules::new_rest_settings("{}");

  // the spells the server names as teleports (thuum
  // docs/verbs/cast-teleport.md): server-settings.json's `castTeleports`
  rust::Box<skymp::rules::CastTeleports> castTeleports =
    skymp::rules::new_cast_teleports("[]");

  // the headshot rule (thuum docs/verbs/headshot.md): server-settings.json's
  // `headshot` block and every shooter's recent claimed bow hits
  rust::Box<skymp::rules::Headshots> headshots =
    skymp::rules::new_headshots("{}");

  // the scripted effects the server runs itself (thuum
  // docs/verbs/scripted-effects.md): server-settings.json's
  // `scriptedEffects` list
  rust::Box<skymp::rules::ScriptedEffects> scriptedEffects =
    skymp::rules::new_scripted_effects("[]");

  // the bonds (thuum docs/verbs/shadowbond.md): server-settings.json's
  // `bonds` list and the bonds standing between actors
  rust::Box<skymp::rules::Bonds> bonds = skymp::rules::new_bonds("[]");
  std::chrono::steady_clock::time_point lastBondsTick;
  // per caster, bumped at every bond change: a proc cast still waiting on
  // its timer when the bond ended casts nothing (shadowbond.md, train-28)
  std::unordered_map<uint32_t, uint64_t> bondEpochs;

  // a teleport's look, kept from its cast for its landing (thuum
  // docs/verbs/cast-teleport.md, the look on other screens), by the caster's
  // form id, until the jump's wait runs out (SteadyNowMs)
  struct KeptTeleportLook
  {
    PartOne::TeleportLook look;
    uint64_t untilMs = 0;
  };
  std::unordered_map<uint32_t, KeptTeleportLook> teleportLooks;

  // the globals the server keeps per player (thuum
  // docs/verbs/player-globals.md): server-settings.json's `playerGlobals`
  rust::Box<skymp::rules::PlayerGlobalsList> playerGlobals =
    skymp::rules::new_player_globals("[]");

  // the quests whose stages are the world's (thuum
  // docs/verbs/quest-stage.md, ADR-056): server-settings.json's
  // `sharedQuests`
  rust::Box<skymp::rules::SharedQuests> sharedQuests =
    skymp::rules::new_shared_quests("[]");

  // the objects the server places at an accepted cast of a listed spell,
  // and every set standing (thuum docs/verbs/spell-placed-object.md,
  // ADR-057): server-settings.json's `spellPlacedObjects`; the rule is
  // Rust's. Per set its forms, by the rule's set id; per placed form its
  // caster, which every CreateActor of the form names (placedBy)
  rust::Box<skymp::rules::Placements> placements =
    skymp::rules::new_placements("[]");
  std::unordered_map<uint64_t, std::vector<uint32_t>> placedSets;
  std::unordered_map<uint32_t, uint32_t> placedCasterByForm;
  std::chrono::steady_clock::time_point lastPlacementsTick;
  // the food effects that lower the needs the server keeps (thuum
  // docs/verbs/player-globals.md, the needs): server-settings.json's
  // `survivalFood`, its switch and its table
  std::optional<FormDesc> survivalFoodSwitch;
  std::vector<PartOne::SurvivalFoodEffect> survivalFood;
  // the spells the server cures on every player (memo D56 A):
  // server-settings.json's `cureSpells`
  std::vector<FormDesc> cureSpells;

  // the fights between players (thuum docs/verbs/hostility-sync.md, ADR-023)
  rust::Box<skymp::rules::Fights> fights = skymp::rules::new_fights();
  std::chrono::steady_clock::time_point lastFightsTick;
  std::chrono::steady_clock::time_point lastFateRespawnsTick;

  // the cell host table (thuum docs/verbs/cell-host.md, ADR-035) and each
  // player's last grid square (worldOrCell, floor x, floor y), so its reach
  // is recomputed only when the square moves
  rust::Box<skymp::rules::Cells> cells = skymp::rules::new_cells();
  // the prop owner table (thuum docs/verbs/prop-physics.md, ADR-058): which
  // seat simulates each physics prop, the cell's host by default
  rust::Box<skymp::rules::Props> props = skymp::rules::new_props();
  std::chrono::steady_clock::time_point lastCellsTick;
  using Square = std::tuple<uint32_t, int32_t, int32_t>;
  std::unordered_map<uint32_t, Square> lastSquareByPlayer;
  std::function<int64_t(Networking::UserId)> roundTripSource;
  // the quitting host (docs/verbs/cell-host.md): each connection's silence
  // and the users the table holds stale for it
  std::function<int64_t(Networking::UserId)> silenceSource;
  std::unordered_set<Networking::UserId> staleUsers;
  // the squares each player has been sent fates for (npc-fate, ADR-049)
  std::unordered_map<uint32_t, std::set<Square>> fatedSquares;
  // the follow (docs/verbs/npc-handoff.md D6), off by the cellFollow setting
  bool followEnabled = true;

  espm::Loader* espm = nullptr;

  std::function<void(PartOneSendTargetWrapper* sendTarget,
                     MpObjectReference* emitter, MpObjectReference* listener)>
    onSubscribe, onUnsubscribe;

  espm::CompressedFieldsCache compressedFieldsCache;

  std::shared_ptr<PacketParser> packetParser;
  std::shared_ptr<ActionListener> actionListener;

  std::shared_ptr<spdlog::logger> logger;

  std::unique_ptr<PartOneSendTargetWrapper> sendTarget;
  std::unique_ptr<IDamageFormula> damageFormula{};
  FakeSendTarget fakeSendTarget;

  GamemodeApi::State gamemodeApiState;
  std::vector<uint8_t> updateGamemodeDataMsg;

  std::shared_ptr<OpenSSLSigner> sslSigner; // nullptr if no private key set
  std::string sslSignerKeyAlias;            // empty string
  bool enableGamemodeDataUpdatesBroadcast = false;

  PartOne::OnActorStreamIn onActorStreamIn;
};

PartOne::PartOne(Networking::ISendTarget* sendTarget)
{
  Init();
  SetSendTarget(sendTarget);
}

PartOne::PartOne(std::shared_ptr<Listener> listener,
                 Networking::ISendTarget* sendTarget)
{
  Init();
  AddListener(listener);
  SetSendTarget(sendTarget);
}

PartOne::~PartOne()
{
  // worldState may depend on serverState (actorsMap), we should reset it first
  worldState.Clear();
  serverState = {};
}

void PartOne::SetSendTarget(Networking::ISendTarget* sendTarget)
{
  Networking::ISendTarget* underlyingSendTargetToSet =
    sendTarget ? sendTarget : &pImpl->fakeSendTarget;

  pImpl->sendTarget.reset(
    new PartOneSendTargetWrapper(*underlyingSendTargetToSet));
}

void PartOne::SetDamageFormula(std::unique_ptr<IDamageFormula> dmgFormula)
{
  pImpl->damageFormula = std::move(dmgFormula);
}

void PartOne::AddListener(std::shared_ptr<Listener> listener)
{
  worldState.listeners.push_back(listener);
}

bool PartOne::IsConnected(Networking::UserId userId) const
{
  return serverState.IsConnected(userId);
}

void PartOne::Tick()
{
  TickPacketHistoryPlaybacks();
  TickDeferredMessages();
  TickGameTime();
  TickFightsEverySecond();
  TickBondsEverySecond();
  TickPlacementsOften();
  TickCellHostsEverySecond();
  TickFateRespawnsEverySecond();
  worldState.Tick();
}

uint32_t PartOne::CreateActor(uint32_t formId, const NiPoint3& pos,
                              float angleZ, uint32_t cellOrWorld,
                              ProfileId profileId)
{
  if (!formId) {
    formId = worldState.GenerateFormId();
  }
  worldState.AddForm(
    std::unique_ptr<MpActor>(
      new MpActor({ pos,
                    { 0, 0, angleZ },
                    FormDesc::FromFormId(cellOrWorld, worldState.espmFiles) },
                  CreateFormCallbacks())),
    formId);
  if (profileId >= 0) {
    auto& ac = worldState.GetFormAt<MpActor>(formId);
    ac.RegisterProfileId(profileId);
  }

  return formId;
}

void PartOne::SetUserActor(Networking::UserId userId, uint32_t actorFormId)
{
  serverState.EnsureUserExists(userId);

  if (actorFormId > 0) {
    auto& actor = worldState.GetFormAt<MpActor>(actorFormId);

    if (actor.IsDisabled()) {
      std::stringstream ss;
      ss << "Actor with id " << std::hex << actorFormId << " is disabled";
      throw std::runtime_error(ss.str());
    }

    // thuum docs/verbs/cell-host.md: the player's reach is recomputed at
    // its first report (the square forgotten); a user's own actor is never
    // hosted (ActionListener::ActorUpdatableBy), the "players link to each
    // other" fix of old
    pImpl->lastSquareByPlayer.erase(actor.GetFormId());
    pImpl->fatedSquares.erase(actor.GetFormId());

    // Both functions are required here, but it is NOT covered by unit tests
    // properly. If you do something wrong here, players will not be able to
    // interact with items in the same cell after reconnecting.
    actor.UnsubscribeFromAll();
    actor.RemoveFromGridAndUnsubscribeAll();

    serverState.actorsMap.Set(userId, &actor);

    // The clock first, so the client knows the time before its own
    // CreateActor loads the save (thuum docs/verbs/time.md)
    if (pImpl->gameTimeBroadcast) {
      pImpl->sendTarget->Send(
        userId, ToMessage(pImpl->gameClock->login(userId, UnixNowMs())), true);
    }

    actor.ForceSubscriptionsUpdate();

    // thuum docs/verbs/map-markers.md: show the player's discovered markers
    // once its client's world is up (the first movement after this)
    actor.SetMapMarkersPending(true);

    // thuum docs/verbs/player-globals.md (train-30's early send): the
    // player's own globals go out at the login itself, not at the first
    // movement report with the other login sends. The client's service
    // holds the message until its first in-game update, which comes a few
    // seconds before the Survival Mode Creation's 5 s check; sent at the
    // first movement (4.2 s after the connect in playtest twenty-seven's
    // test 2 relaunch) the restore lost that race once in two and the
    // Creation's first-login box asked again (x-survival-restore-timing
    // 20261020-135028 won it; 18:23Z lost it).
    GetActionListener().SendPlayerGlobals(actor);

    // thuum docs/verbs/quest-stage.md (ADR-056 decision 3): the world's
    // quest stages follow the globals, the shared list first, then every
    // recorded stage ascending per quest
    GetActionListener().SendQuestStages(actor);

    // thuum memo D56 A: the listed cure spells leave the player's game at the
    // login and every minute after (the engine's own RemoveSpell)
    GetActionListener().SendCures(actor);

    // thuum docs/verbs/actor-values.md (train-31; playtest twenty-eight's
    // test 2 relaunch): the login's record holds from the login itself, not
    // from its send at the first movement, so a fresh session's report that
    // lands first is refused instead of recorded over the player's skills
    // (probe x-relaunch-reports 20261021-015132 took one); the client's
    // echo of the applied record ends the hold
    if (actor.GetActorValueRecord()) {
      actor.SetActorValuesLoginPending(true);
    }

    // We do the same in MpActor::ApplyChangeForm for non-player characters
    if (actor.IsDead() && !actor.IsRespawning()) {
      spdlog::info("PartOne::SetUserActor {} {:x} - respawning dead actor",
                   userId, actorFormId);
      actor.RespawnWithDelay();
    }

    // This is not currently saved client-side, so reset
    actor.SetLastAnimEvent(std::nullopt);

  } else {
    serverState.actorsMap.Erase(userId);
  }
}

uint32_t PartOne::GetUserActor(Networking::UserId userId)
{
  serverState.EnsureUserExists(userId);

  auto actor = serverState.ActorByUser(userId);
  if (!actor) {
    return 0;
  }
  return actor->GetFormId();
}

std::string PartOne::GetUserGuid(Networking::UserId userId)
{
  serverState.EnsureUserExists(userId);
  return serverState.UserGuid(userId);
}

Networking::UserId PartOne::GetUserByActor(uint32_t formId)
{
  auto& form = worldState.LookupFormById(formId);
  if (form) {
    if (auto ac = form.get()->AsActor()) {
      return serverState.UserByActor(ac);
    }
  }
  return Networking::InvalidUserId;
}

void PartOne::DestroyActor(uint32_t actorFormId)
{
  std::shared_ptr<MpActor> destroyedForm;
  worldState.DestroyForm<MpActor>(actorFormId, &destroyedForm);

  serverState.actorsMap.Erase(destroyedForm.get());
}

void PartOne::SetRaceMenuOpen(uint32_t actorFormId, bool open)
{
  auto& actor = worldState.GetFormAt<MpActor>(actorFormId);

  if (actor.IsRaceMenuOpen() == open) {
    return;
  }

  actor.SetRaceMenuOpen(open);

  auto userId = serverState.UserByActor(&actor);
  if (userId == Networking::InvalidUserId) {
    spdlog::warn(
      "PartOne::SetRaceMenuOpen {:x} - actor is not attached to any of users",
      actorFormId);
    return;
  }

  SetRaceMenuOpenMessage message;
  message.open = open;
  pImpl->sendTarget->Send(userId, message, true);
}

void PartOne::SendCustomPacket(Networking::UserId userId,
                               const std::string& jContent)
{
  CustomPacketMessage message;
  message.contentJsonDump = jContent;
  pImpl->sendTarget->Send(userId, message, true);
}

std::string PartOne::GetActorName(uint32_t actorFormId)
{
  auto& ac = worldState.GetFormAt<MpActor>(actorFormId);
  return ac.GetAppearance() ? ac.GetAppearance()->name : "Prisoner";
}

NiPoint3 PartOne::GetActorPos(uint32_t actorFormId)
{
  auto& ac = worldState.GetFormAt<MpActor>(actorFormId);
  return ac.GetPos();
}

uint32_t PartOne::GetActorCellOrWorld(uint32_t actorFormId)
{
  auto& ac = worldState.GetFormAt<MpActor>(actorFormId);
  return ac.GetCellOrWorld().ToFormId(worldState.espmFiles);
}

const std::set<uint32_t>& PartOne::GetActorsByProfileId(ProfileId profileId)
{
  return worldState.GetActorsByProfileId(profileId);
}

void PartOne::SetEnabled(uint32_t actorFormId, bool enabled)
{
  auto& ac = worldState.GetFormAt<MpActor>(actorFormId);
  enabled ? ac.Enable() : ac.Disable();
}

void PartOne::SetOnActorStreamIn(OnActorStreamIn callback)
{
  pImpl->onActorStreamIn = callback;
}

void PartOne::AttachEspm(espm::Loader* espm)
{
  pImpl->espm = espm;
  worldState.AttachEspm(espm, [this] { return CreateFormCallbacks(); });
}

void PartOne::AttachSaveStorage(
  std::shared_ptr<
    Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>
    saveStorage)
{
  worldState.AttachSaveStorage(saveStorage);

  auto start = std::chrono::steady_clock::now();

  int n = 0;
  int numPlayerCharacters = 0;
  saveStorage->IterateSync([&](MpChangeForm changeForm) {
    // Do not let players become NPCs
    if (changeForm.profileId != -1 && !changeForm.isDisabled) {
      changeForm.isDisabled = true;
    }

    if (changeForm.isDeleted) {
      pImpl->logger->info(
        "Skipping deleted form {}, will likely overwrite at some point",
        changeForm.formDesc.ToString());
      return;
    }

    bool isFF = changeForm.formDesc.file.empty();

    if (isFF) {
      auto baseId = changeForm.baseDesc.ToFormId(worldState.espmFiles);
      auto lookupRes = GetEspm().GetBrowser().LookupById(baseId);

      if (lookupRes.rec && espm::utils::IsItem(lookupRes.rec->GetType())) {
        pImpl->logger->info("Skipping FF item {} (type is {}), will likely "
                            "overwrite at some point",
                            changeForm.formDesc.ToString(),
                            lookupRes.rec->GetType().ToString());
        return;
      }
    }

    n++;
    worldState.LoadChangeForm(changeForm, CreateFormCallbacks());
    // thuum docs/verbs/spell-placed-object.md (ADR-057): a spell-placed
    // form never outlives the server that placed it
    SweepSpellPlacedAtLoad(changeForm);
    if (changeForm.profileId >= 0) {
      ++numPlayerCharacters;
    }

    if (n % 25 == 0) {
      pImpl->logger->info("Loaded {} ChangeForms", n);
    }
  });

  auto end = std::chrono::steady_clock::now();
  auto duration =
    std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

  pImpl->logger->info("AttachSaveStorage took {} seconds and {} milliseconds, "
                      "loaded {} ChangeForms (Including {} player characters)",
                      duration.count() / 1000, duration.count() % 1000, n,
                      numPlayerCharacters);

  // thuum docs/verbs/npc-puppet.md (playtest 33): the saved game-file actors
  // onto the grid now, not at the first lookup that happens to name them
  worldState.LoadDeferredActors();
}

espm::Loader& PartOne::GetEspm() const
{
  return worldState.GetEspm();
}

bool PartOne::HasEspm() const
{
  return !worldState.espmFiles.empty();
}

void PartOne::AttachLogger(std::shared_ptr<spdlog::logger> logger)
{
  pImpl->logger = logger;
  worldState.logger = logger;
}

spdlog::logger& PartOne::GetLogger()
{
  if (!pImpl->logger) {
    throw std::runtime_error("no logger attached");
  }
  return *pImpl->logger;
}

namespace {
class ScopedTask
{
public:
  ScopedTask(std::function<void()> f_)
    : f(f_)
  {
  }
  ~ScopedTask() { f(); }

private:
  const std::function<void()> f;
};
}

void PartOne::HandlePacket(void* partOneInstance, Networking::UserId userId,
                           Networking::PacketType packetType,
                           Networking::PacketData data, size_t length)
{
  auto this_ = reinterpret_cast<PartOne*>(partOneInstance);

  constexpr size_t kMaxSafeGuid = 1024;

  switch (packetType) {
    case Networking::PacketType::ServerSideUserConnect: {
      // Length is trustworthy here because ServerSideUserConnect contents is
      // generated by us at the server side. However, we double-check it to
      // stay protected if the mechanism changes in the future.
      if (length > kMaxSafeGuid) {
        spdlog::error(
          "PartOne::HandlePacket - ServerSideUserConnect packet with "
          "excessive length: {}, truncating",
          length);
        length = kMaxSafeGuid;
      }
      std::string guid(reinterpret_cast<const char*>(data), length);
      return this_->AddUser(userId, UserType::User, guid);
    }
    case Networking::PacketType::ServerSideUserDisconnect: {
      ScopedTask t([userId, this_] {
        if (auto actor = this_->serverState.ActorByUser(userId)) {
          // TODO: apply dependency inversion here: connection handling code
          // should not depend on animation system
          this_->animationSystem.ClearInfo(actor);
          // a departing caster's streams end with it (thuum
          // docs/verbs/spell-cast.md)
          this_->pImpl->spellCasts->forget(actor->GetFormId());
          // and the sets its casts placed begin their linger (thuum
          // docs/verbs/spell-placed-object.md)
          this_->pImpl->placements->caster_gone(actor->GetFormId(),
                                                SteadyNowMs());
          // and a teleport's look waiting for its landing
          // (docs/verbs/cast-teleport.md)
          this_->pImpl->teleportLooks.erase(actor->GetFormId());
          // TGM and TIM are the session's (thuum docs/verbs/god-mode.md)
          actor->SetGodMode(false);
          actor->SetImmortal(false);
          // every cell it hosts starts its grace (thuum
          // docs/verbs/cell-host.md); the clock passes them on
          this_->pImpl->cells->gone(actor->GetFormId(), SteadyNowMs());
          // and its props fall back to their cells' hosts (thuum
          // docs/verbs/prop-physics.md)
          this_->pImpl->props->props_gone(actor->GetFormId());
          // and the sightings it held for a grant that never came (thuum
          // docs/verbs/npc-puppet.md, finding 66's hold)
          this_->ForgetHeldSightings(actor->GetFormId());
          this_->pImpl->lastSquareByPlayer.erase(actor->GetFormId());
          this_->pImpl->fatedSquares.erase(actor->GetFormId());
        }
        this_->pImpl->staleUsers.erase(userId);
        this_->serverState.Disconnect(userId);
        this_->serverState.disconnectingUserId = Networking::InvalidUserId;
        this_->pImpl->gameClock->forget(userId);
      });

      this_->serverState.disconnectingUserId = userId;
      for (auto& listener : this_->worldState.listeners)
        listener->OnDisconnect(userId);
      return;
    }
    case Networking::PacketType::Message:
      return this_->HandleMessagePacket(userId, data, length);
    default:
      spdlog::error("PartOne::HandlePacket - unexpected PacketType: {}",
                    static_cast<int>(packetType));
  }
}

PartOneSendTargetWrapper& PartOne::GetSendTarget() const
{
  if (!pImpl->sendTarget) {
    throw std::runtime_error("No send target found");
  }
  return *pImpl->sendTarget;
}

float PartOne::CalculateDamage(const MpActor& aggressor, const MpActor& target,
                               const HitData& hitData) const
{
  if (!pImpl->damageFormula) {
    throw std::runtime_error("no damage formula");
  }
  return pImpl->damageFormula->CalculateDamage(aggressor, target, hitData);
}

float PartOne::CalculateDamage(const MpActor& aggressor, const MpActor& target,
                               const SpellCastData& spellCastData) const
{
  if (!pImpl->damageFormula) {
    throw std::runtime_error("no damage formula");
  }
  return pImpl->damageFormula->CalculateDamage(aggressor, target,
                                               spellCastData);
}

void PartOne::NotifyGamemodeApiStateChanged(
  const GamemodeApi::State& newState) noexcept
{
  UpdateGameModeDataMessage msg;

  msg.eventSources.reserve(newState.createdEventSources.size());
  msg.updateOwnerFunctions.reserve(newState.createdProperties.size());
  msg.updateNeighborFunctions.reserve(newState.createdProperties.size());

  for (auto [eventName, eventSourceInfo] : newState.createdEventSources) {
    msg.eventSources.push_back(
      { eventName, SignJavaScriptSources(eventSourceInfo.functionBody) });
  }

  for (auto [propertyName, propertyInfo] : newState.createdProperties) {
    GamemodeValuePair updateOwnerFunctionsEntry;
    updateOwnerFunctionsEntry.name = propertyName;
    updateOwnerFunctionsEntry.content = SignJavaScriptSources(
      propertyInfo.isVisibleByOwner ? propertyInfo.updateOwner : "");
    msg.updateOwnerFunctions.push_back(updateOwnerFunctionsEntry);

    //  From docs: isVisibleByNeighbors considered to be always false for
    //  properties with `isVisibleByOwner == false`, in that case, actual
    //  flag value is ignored.

    const bool actuallyVisibleByNeighbor =
      propertyInfo.isVisibleByNeighbors && propertyInfo.isVisibleByOwner;

    GamemodeValuePair updateNeighborFunctionsEntry;
    updateNeighborFunctionsEntry.name = propertyName;
    updateNeighborFunctionsEntry.content = SignJavaScriptSources(
      actuallyVisibleByNeighbor ? propertyInfo.updateNeighbor : "");
    msg.updateNeighborFunctions.push_back(updateNeighborFunctionsEntry);
  }

  std::string stream;
  GetMessageSerializerInstance().Serialize(msg, stream);

  if (pImpl->enableGamemodeDataUpdatesBroadcast) {
    spdlog::info("PartOne::NotifyGamemodeApiStateChanged - sending gamemode "
                 "data update to all connected users");
    auto& currentSendTarget = GetSendTarget();
    for (size_t i = 0, n = serverState.maxConnectedId; i <= n; ++i) {
      Networking::UserId userId = static_cast<Networking::UserId>(i);
      if (serverState.IsConnected(userId)) {
        currentSendTarget.Send(
          userId, reinterpret_cast<Networking::PacketData>(stream.data()),
          stream.size(), true);
      }
    }
  } else {
    // Intentionally skipped to avoid client instability. See
    // 'enableGamemodeDataUpdatesBroadcast' in server docs.
    spdlog::info("PartOne::NotifyGamemodeApiStateChanged - skipping gamemode "
                 "data update send, clientside hot-reload is disabled");
  }

  pImpl->gamemodeApiState = newState;
  pImpl->updateGamemodeDataMsg.resize(stream.size());
  std::copy(stream.data(), stream.data() + stream.size(),
            pImpl->updateGamemodeDataMsg.begin());
}

void PartOne::SetPrivateKey(const std::string& keyAlias,
                            const std::string& pkeyPem)
{
  auto pkey = std::make_shared<OpenSSLPrivateKey>(pkeyPem);
  pImpl->sslSigner = std::make_shared<OpenSSLSigner>(pkey);
  pImpl->sslSignerKeyAlias = keyAlias;
}

void PartOne::EnableGamemodeDataUpdatesBroadcast(bool enable)
{
  pImpl->enableGamemodeDataUpdatesBroadcast = enable;
}

std::string PartOne::SignJavaScriptSources(const std::string& src) const
{
  if (src.empty()) {
    return src;
  }

  if (!pImpl->sslSigner) {
    return src + "\n// skymp:sig:n/a";
  }

  std::string signature = pImpl->sslSigner->SignB64(
    reinterpret_cast<const unsigned char*>(src.c_str()), src.length());
  return src +
    fmt::format("\n// skymp:sig:y:CPP{}:{}", pImpl->sslSignerKeyAlias,
                signature);
}

void PartOne::SetPacketHistoryRecording(Networking::UserId userId, bool enable)
{
  if (userId < serverState.userInfo.size() && serverState.userInfo[userId]) {
    if (!serverState.userInfo[userId]->packetHistoryStartTime) {
      serverState.userInfo[userId]->packetHistoryStartTime =
        std::chrono::steady_clock::now();
    }
    serverState.userInfo[userId]->isPacketHistoryRecording = enable;
  } else {
    throw std::runtime_error("Invalid user id " + std::to_string(userId));
  }
}

PacketHistory PartOne::GetPacketHistory(Networking::UserId userId)
{
  if (userId < serverState.userInfo.size() && serverState.userInfo[userId]) {
    return serverState.userInfo[userId]->packetHistory;
  } else {
    throw std::runtime_error("Invalid user id " + std::to_string(userId));
  }
}

void PartOne::ClearPacketHistory(Networking::UserId userId)
{
  if (userId < serverState.userInfo.size() && serverState.userInfo[userId]) {
    serverState.userInfo[userId]->packetHistory = std::move(PacketHistory{});
    serverState.userInfo[userId]->packetHistoryStartTime = std::nullopt;
  } else {
    throw std::runtime_error("Invalid user id " + std::to_string(userId));
  }
}

void PartOne::RequestPacketHistoryPlayback(Networking::UserId userId,
                                           const PacketHistory& history)
{
  if (userId < serverState.userInfo.size() && serverState.userInfo[userId]) {
    serverState.requestedPlaybacks[userId] =
      Playback{ history, std::chrono::steady_clock::now() };
  } else {
    throw std::runtime_error("Invalid user id " + std::to_string(userId));
  }
}

FormCallbacks PartOne::CreateFormCallbacks()
{
  auto st = &serverState;

  FormCallbacks::SubscribeCallback
    subscribe =
      [this](MpObjectReference* emitter, MpObjectReference* listener) {
        return pImpl->onSubscribe(pImpl->sendTarget.get(), emitter, listener);
      },
    unsubscribe = [this](MpObjectReference* emitter,
                         MpObjectReference* listener) {
      return pImpl->onUnsubscribe(pImpl->sendTarget.get(), emitter, listener);
    };

  FormCallbacks::SendToUserFn sendToUser =
    [this, st](MpActor* actor, const IMessageBase& message, bool reliable) {
      std::string stream;
      GetMessageSerializerInstance().Serialize(message, stream);

      auto targetuserId = st->UserByActor(actor);
      if (targetuserId != Networking::InvalidUserId &&
          st->disconnectingUserId != targetuserId) {
        pImpl->sendTarget->Send(
          targetuserId,
          reinterpret_cast<Networking::PacketData>(stream.data()),
          stream.size(), reliable);
      }
    };

  FormCallbacks::SendToUserDeferredFn sendToUserDeferred =
    [this, st](MpActor* actor, const IMessageBase& message, bool reliable,
               int deferredChannelId, bool overwritePreviousChannelMessages) {
      std::string stream;
      GetMessageSerializerInstance().Serialize(message, stream);

      if (deferredChannelId < 0 || deferredChannelId >= 100) {
        return spdlog::error(
          "sendToUserDeferred - invalid deferredChannelId {}",
          deferredChannelId);
      }

      auto targetuserId = st->UserByActor(actor);
      if (targetuserId == Networking::InvalidUserId ||
          st->disconnectingUserId == targetuserId) {
        // It's ok, it happens
        return;
      }

      auto& userInfo = st->userInfo[targetuserId];
      if (!userInfo) {
        return spdlog::error("sendToUserDeferred - null userInfo for user {}",
                             targetuserId);
      }

      DeferredMessage deferredMessage;
      deferredMessage.packetData = {
        reinterpret_cast<const Networking::PacketData>(stream.data()),
        reinterpret_cast<const Networking::PacketData>(stream.data()) +
          stream.size()
      };
      deferredMessage.packetReliable = reliable;
      deferredMessage.actorIdExpected = actor->GetFormId();

      if (userInfo->deferredChannels.size() <= deferredChannelId) {
        userInfo->deferredChannels.resize(deferredChannelId + 1);
      }

      if (overwritePreviousChannelMessages) {
        userInfo->deferredChannels[deferredChannelId] = { deferredMessage };
      } else {
        userInfo->deferredChannels[deferredChannelId].push_back(
          deferredMessage);
      }
    };

  FormCallbacks::GetUserIdFn getUserId =
    [this, st](MpActor* actor) -> Networking::UserId {
    return st->UserByActor(actor);
  };

  return { subscribe, unsubscribe, sendToUser, sendToUserDeferred, getUserId };
}

ActionListener& PartOne::GetActionListener()
{
  InitActionListener();
  return *pImpl->actionListener;
}

const std::vector<std::shared_ptr<PartOne::Listener>>& PartOne::GetListeners()
  const
{
  return worldState.listeners;
}

std::vector<PartOne::Message>& PartOne::Messages()
{
  return pImpl->fakeSendTarget.messages;
}

void PartOne::Init()
{
  pImpl.reset(new Impl);
  pImpl->logger.reset(new spdlog::logger{ "empty logger" });

  worldState.gameTime = [this] { return GetGameTime(); };
  worldState.fightBetween = [this](uint32_t a, uint32_t b) {
    return pImpl->fights->between(a, b);
  };
  // and who simulates an actor where it stands: the host, or its follower
  // in a cell nobody hosts (npc-handoff D6); the word, the routing and the
  // sighting gate read this one
  worldState.simulatorOf = [this](uint32_t actor, uint32_t cell) {
    return pImpl->cells->simulator_of(actor, cell);
  };
  // and a hosted NPC's door (npc-handoff D6, and D9's pass through a door
  // since 2026-10-28): the crossing judged by the move verdict for its
  // simulator before the host's engine walks it
  worldState.npcDoorVerdict = [this](MpActor& actor, uint32_t destWorldOrCell,
                                     const NiPoint3& destPos) {
    // a judgment, not a move: nothing recorded until the host's report
    // (x-handoff-door 20261024-001915 read a follow removed at the exit
    // door before the host lookup, hence a false "no host")
    MpActor* host = worldState.HostOf(actor);
    return host &&
      WouldMoveActor(*host, actor, destWorldOrCell, destPos) !=
      MoveVerdict::Refuse;
  };
  // thuum docs/verbs/npc-fate.md (ADR-049 decision 2): a fate rolled anew
  // at a respawn goes to every user and the fate pass at a player's square
  // change, ahead of the grid's own loader (decision 3's "ahead of the
  // engine" begins with ahead of ours)
  worldState.onSquareChange = [this](MpActor& actor) {
    if (serverState.UserByActor(&actor) == Networking::InvalidUserId) {
      return;
    }
    SendFates(actor, actor.GetCellOrWorld().ToFormId(worldState.espmFiles),
              actor.GetPos());
  };
  worldState.onDeathState = [this](MpActor& actor, bool isDead) {
    OnDeathState(actor, isDead);
  };
  worldState.onActorFate = [this](MpActor& actor) {
    ActorFateMessage msg;
    msg.fates.push_back(FateOf(worldState, actor));
    for (size_t i = 0, n = serverState.maxConnectedId; i <= n; ++i) {
      const auto userId = static_cast<Networking::UserId>(i);
      if (serverState.ActorByUser(userId)) {
        GetSendTarget().Send(userId, msg, true);
      }
    }
  };
  // thuum ADR-050 decision 3 (train 51): a server-made NPC's roll at its
  // placement, at the level the rule gives the placer (a player's console
  // or the gamemode's reference), else the first player online, else 1
  worldState.fateRollLevelAt = [this](MpObjectReference& at) -> uint16_t {
    MpActor* loader = at.AsActor();
    if (!loader ||
        serverState.UserByActor(loader) == Networking::InvalidUserId) {
      loader = nullptr;
      for (Networking::UserId userId = 0; userId <= serverState.maxConnectedId;
           ++userId) {
        if (MpActor* actor = serverState.ActorByUser(userId)) {
          loader = actor;
          break;
        }
      }
    }
    if (!loader) {
      return 1;
    }
    const NiPoint3& pos = at.GetPos();
    return FateRollLevel(
      *loader, at.GetCellOrWorld().ToFormId(worldState.espmFiles),
      static_cast<int16_t>(pos.x / 4096), static_cast<int16_t>(pos.y / 4096));
  };
  worldState.cellHostOf = [this](uint32_t cell) {
    const auto host = pImpl->cells->host_of(cell);
    return host.hosted ? host.player : 0u;
  };

  pImpl->onSubscribe = [this](PartOneSendTargetWrapper* sendTarget,
                              MpObjectReference* emitter,
                              MpObjectReference* listener) {
    if (!emitter) {
      throw std::runtime_error("nullptr emitter in onSubscribe");
    }

    MpActor* listenerAsActor = listener->AsActor();
    if (!listenerAsActor) {
      return;
    }

    auto listenerUserId = serverState.UserByActor(listenerAsActor);
    if (listenerUserId == Networking::InvalidUserId) {
      return;
    }

    auto& emitterPos = emitter->GetPos();
    auto& emitterRot = emitter->GetAngle();

    bool isMe = emitter == listener;

    MpActor* emitterAsActor = emitter->AsActor();

    CreateActorMessage message;

    std::string jAnimation;

    if (emitterAsActor) {
      auto appearance = emitterAsActor->GetAppearance();
      message.appearance = appearance
        ? std::optional<Appearance>(*appearance)
        : std::optional<Appearance>(std::nullopt);
      if (pImpl->onActorStreamIn) {
        pImpl->onActorStreamIn(*emitterAsActor, *listener, message);
      }
    }

    if (emitterAsActor) {
      message.equipment = emitterAsActor->GetEquipment();
    }

    if (emitterAsActor) {
      message.animation = emitterAsActor->GetLastAnimEvent();
    }

    // thuum docs/verbs/npc-puppet.md: an actor from the game files travels
    // under its own form id (the client binds the engine's own reference
    // and runs it or puppets it by the cell's grant); upstream's
    // id + 0x100000000 made the client place a copy of its base
    message.refrId = emitter->GetFormId();

    if (emitter->GetBaseId() != 0x00000000 &&
        emitter->GetBaseId() != 0x00000007) {
      message.baseId = emitter->GetBaseId();
    }

    if (emitterAsActor && emitterAsActor->IsDead()) {
      message.isDead = true;
    }

    const bool isOwner = emitter == listener;

    auto mode = VisitPropertiesMode::OnlyPublic;
    if (isOwner) {
      mode = VisitPropertiesMode::All;
    }

    emitter->VisitProperties(message, mode);

    auto isFilteredOut = [&](const CustomPropsEntry& customPropsEntry) {
      auto it = pImpl->gamemodeApiState.createdProperties.find(
        customPropsEntry.propName);
      if (it != pImpl->gamemodeApiState.createdProperties.end()) {
        if (!it->second.isVisibleByOwner) {
          //  From docs: isVisibleByNeighbors is considered to be always
          //  false for properties with `isVisibleByOwner == false`, in
          //  that case, actual flag value is ignored.
          return true;
        }
        if (!it->second.isVisibleByNeighbors && !isOwner) {
          return true;
        }
      }
      return false;
    };

    message.customPropsJsonDumps.erase(
      std::remove_if(message.customPropsJsonDumps.begin(),
                     message.customPropsJsonDumps.end(), isFilteredOut),
      message.customPropsJsonDumps.end());

    const bool hasUser = emitterAsActor &&
      serverState.UserByActor(emitterAsActor) != Networking::InvalidUserId;

    // thuum docs/verbs/cell-host.md and npc-puppet.md (ADR-045 point 2 as
    // revised 2026-10-22): a player's figure is always hosted by other;
    // every other actor, a server-made copy (a 0xff id) and an actor from
    // the game files under its own id alike, is hosted by other unless
    // this listener hosts its cell. The server's word, explicit either
    // way: the client cannot derive a persistent reference's exterior cell
    // (Papyrus GetParentCell answers the worldspace's persistent cell), so
    // Riverwood's named residents stayed puppets on their host while the
    // grant's cell walk promoted only the cell's own references
    // (b-npc-puppet 20261022-081631). The client binds an engine reference
    // to a game-file actor's model, so the flag under the raw id reaches
    // it now.
    const bool fromGameFiles =
      emitterAsActor && emitter->GetFormId() < 0xff000000;
    if (emitterAsActor) {
      message.props.isHostedByOther =
        hasUser || worldState.HostOf(*emitter) != listener;
    }

    uint32_t worldOrCell =
      emitter->GetCellOrWorld().ToFormId(worldState.espmFiles);

    // See 'perf: improve game framerate #1186'
    // Client needs to know if it is DOOR or not; thuum docs/verbs/
    // npc-puppet.md: and whether an engine reference is an actor (NPC_),
    // which it binds a view to instead of placing a copy
    if (const std::string& baseType = emitter->GetBaseType();
        baseType == "DOOR" || (fromGameFiles && baseType == "NPC_")) {
      message.baseRecordType = baseType;
    }

    // thuum docs/verbs/spell-placed-object.md (ADR-057): a form the server
    // placed at a caster's accepted cast names the caster and its base's
    // type (MSTT or ACTI; the validator holds the pair), so the caster's own
    // game skips it while its cast runs and every other game places it
    if (const auto placed =
          pImpl->placedCasterByForm.find(emitter->GetFormId());
        placed != pImpl->placedCasterByForm.end()) {
      message.props.placedBy = placed->second;
      message.baseRecordType = emitter->GetBaseType();
    }

    message.idx = emitter->GetIdx();
    message.isMe = isMe;
    message.transform.pos = { emitterPos.x, emitterPos.y, emitterPos.z };
    message.transform.rot = { emitterRot.x, emitterRot.y, emitterRot.z };
    message.transform.worldOrCell = worldOrCell;

    // thuum ADR-050 decision 3 (train 51): a server-made NPC's fate goes
    // ahead of its create word to each subscriber (a game-file actor's rides
    // the ring pass at the square change, SendFates), so the copy's engine
    // rolls the server's roll inside its placement
    if (emitterAsActor && emitter->GetFormId() >= 0xff000000 &&
        !emitterAsActor->IsCreatedAsPlayer() &&
        emitterAsActor->GetFateLevel().has_value()) {
      ActorFateMessage fateMessage;
      fateMessage.fates.push_back(FateOf(worldState, *emitterAsActor));
      sendTarget->Send(listenerUserId, fateMessage, true);
    }

    sendTarget->Send(listenerUserId, message, true);

    // thuum docs/verbs/prop-physics.md (ADR-058): a listener arriving while
    // a seat simulates this prop hears who, after the create word, so it
    // neither simulates the prop as the host's default nor drops the stream
    // as echoes; with no live row the listener's own fallback (the cell's
    // host) is the answer
    if (!emitterAsActor) {
      if (const uint32_t liveOwner = PropLiveOwner(*emitter); liveOwner != 0) {
        PropOwnerMessage word;
        word.refr = emitter->GetFormId();
        word.player = liveOwner;
        word.epoch = PropEpochOf(*emitter);
        sendTarget->Send(listenerUserId, word, true);
      }
    }

    // thuum docs/verbs/racemenu-sync.md: a player's RaceMenu look comes
    // with its figure, after CreateActor on the same ordered channel; its
    // own client gets it after its login instead (ActionListener)
    if (emitterAsActor && !isMe && hasUser) {
      if (auto preset = emitterAsActor->GetRaceMenuPreset(); !preset.empty()) {
        RaceMenuPresetMessage presetMessage;
        presetMessage.actor = emitterAsActor->GetFormId();
        presetMessage.preset = std::move(preset);
        sendTarget->Send(listenerUserId, presetMessage, true);
      }
    }
  };

  pImpl->onUnsubscribe = [this](PartOneSendTargetWrapper* sendTarget,
                                MpObjectReference* emitter,
                                MpObjectReference* listener) {
    MpActor* listenerAsActor = listener->AsActor();
    if (!listenerAsActor) {
      return;
    }

    auto listenerUserId = serverState.UserByActor(listenerAsActor);
    if (listenerUserId != Networking::InvalidUserId &&
        listenerUserId != serverState.disconnectingUserId) {
      DestroyActorMessage message;
      message.idx = emitter->GetIdx();
      sendTarget->Send(listenerUserId, message, true);
    }
  };
}

void PartOne::AddUser(Networking::UserId userId, UserType type,
                      const std::string& guid)
{
  serverState.Connect(userId, guid);
  for (auto& listener : worldState.listeners)
    listener->OnConnect(userId);

  // Save CPU time by not serializing UpdateGamemodeDataMessage each time
  if (!pImpl->updateGamemodeDataMsg.empty()) {
    GetSendTarget().Send(userId,
                         reinterpret_cast<Networking::PacketData>(
                           pImpl->updateGamemodeDataMsg.data()),
                         pImpl->updateGamemodeDataMsg.size(), true);
  }
}

void PartOne::HandleMessagePacket(Networking::UserId userId,
                                  Networking::PacketData data, size_t length)
{
  if (!serverState.IsConnected(userId)) {
    spdlog::error("PartOne::HandleMessagePacket - received Message packet "
                  "from non-existing user {}, ignoring",
                  userId);
    return;
  }

  if (!pImpl->packetParser) {
    pImpl->packetParser = std::make_shared<PacketParser>();
  }

  InitActionListener();

  auto& userInfo = serverState.userInfo[userId];
  if (userInfo && userInfo->isPacketHistoryRecording) {
    if (!userInfo->packetHistoryStartTime) {
      spdlog::error("Expected packetHistoryStartTime to present, probably "
                    "incorrect code");
    } else {
      size_t offset = userInfo->packetHistory.buffer.size();

      userInfo->packetHistory.buffer.resize(offset + length);
      std::copy(data, data + length,
                userInfo->packetHistory.buffer.data() + offset);

      auto duration =
        std::chrono::steady_clock::now() - *userInfo->packetHistoryStartTime;
      auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(duration);
      auto timeMs = milliseconds.count();

      userInfo->packetHistory.packets.push_back(
        { offset, length, static_cast<uint64_t>(timeMs) });
    }
  }

  if (serverState.activePlaybacks.count(userId) > 0) {
    return;
  }

  pImpl->packetParser->TransformPacketIntoAction(userId, data, length,
                                                 *pImpl->actionListener);
}

void PartOne::InitActionListener()
{
  if (!pImpl->actionListener) {
    pImpl->actionListener = std::make_shared<ActionListener>(*this);
  }
}

void PartOne::TickPacketHistoryPlaybacks()
{
  for (auto& [userId, playback] : serverState.requestedPlaybacks) {
    serverState.activePlaybacks[userId] = std::move(playback);
  }
  serverState.requestedPlaybacks.clear();

  for (auto& [userId, playback] : serverState.activePlaybacks) {
    auto& packetHistory = playback.history;

    while (
      !packetHistory.packets.empty() &&
      playback.startTime +
          std::chrono::milliseconds(packetHistory.packets.front().timeMs) <=
        std::chrono::steady_clock::now()) {
      auto& packet = packetHistory.packets.front();

      if (packetHistory.buffer.size() < packet.offset + packet.length) {
        spdlog::error("Packet history buffer is corrupted");
      } else {
        pImpl->packetParser->TransformPacketIntoAction(
          userId, &packetHistory.buffer[packet.offset], packet.length,
          *pImpl->actionListener);
      }
      packetHistory.packets.pop_front();
    }
  }

  // delete playback if packetHistory.packets is empty
  for (auto it = serverState.activePlaybacks.begin();
       it != serverState.activePlaybacks.end();) {
    if (it->second.history.packets.empty()) {
      it = serverState.activePlaybacks.erase(it);
    } else {
      ++it;
    }
  }
}

void PartOne::TickDeferredMessages()
{
  for (size_t i = 0, n = serverState.maxConnectedId; i <= n; ++i) {
    Networking::UserId userId = static_cast<Networking::UserId>(i);
    auto& userInfo = serverState.userInfo[userId];
    if (!userInfo) {
      continue;
    }
    for (auto& channel : userInfo->deferredChannels) {
      for (auto& message : channel) {
        auto actor = serverState.ActorByUser(userId);
        if (!actor) {
          continue;
        }

        if (message.actorIdExpected != actor->GetFormId()) {
          continue;
        }

        pImpl->sendTarget->Send(
          userId,
          reinterpret_cast<Networking::PacketData>(message.packetData.data()),
          message.packetData.size(), message.packetReliable);
      }
      channel.clear();
    }
  }
}

MessageSerializer& PartOne::GetMessageSerializerInstance()
{
  static auto g_serializer =
    MessageSerializerFactory::CreateMessageSerializer();
  return *g_serializer;
}

void PartOne::SetGameTimeSettings(const std::string& timeSettingsJson)
{
  pImpl->gameClock = skymp::rules::new_game_clock(timeSettingsJson);
  pImpl->gameTimeBroadcast = true;
  auto t = GetGameTime();
  spdlog::info("PartOne::SetGameTimeSettings - game time now year {}, month "
               "{}, day {}, hour {:.3f}, days passed {:.3f}, time scale {}",
               t.year, t.month, t.day, t.hour, t.daysPassed, t.timeScale);
}

void PartOne::SetRestSettings(const std::string& restSettingsJson)
{
  pImpl->restSettings = skymp::rules::new_rest_settings(restSettingsJson);
  spdlog::info("PartOne::SetRestSettings - {}", restSettingsJson);
}

const skymp::rules::RestSettings& PartOne::GetRestSettings() const
{
  return *pImpl->restSettings;
}

GameTimeNow PartOne::GetGameTime() const
{
  auto t = pImpl->gameClock->now(UnixNowMs());
  return { t.year, t.month, t.day, t.hour, t.days_passed, t.time_scale };
}

void PartOne::TickGameTime()
{
  if (!pImpl->gameTimeBroadcast) {
    return;
  }
  const auto nowMs = UnixNowMs();
  for (size_t i = 0, n = serverState.maxConnectedId; i <= n; ++i) {
    const auto userId = static_cast<Networking::UserId>(i);
    if (serverState.ActorByUser(userId) &&
        pImpl->gameClock->resync_due(userId, nowMs)) {
      pImpl->sendTarget->Send(userId, ToMessage(pImpl->gameClock->now(nowMs)),
                              true);
    }
  }
}

skymp::rules::Fights& PartOne::GetFights()
{
  return *pImpl->fights;
}

void PartOne::TickFightsEverySecond()
{
  const auto now = std::chrono::steady_clock::now();
  if (now - pImpl->lastFightsTick < std::chrono::seconds(1)) {
    return;
  }
  pImpl->lastFightsTick = now;
  TickFights(static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch())
      .count()));
}

namespace {
// `client`'s game ends its figure's combat (thuum ADR-023): Papyrus
// Actor.StopCombatAlarm on the figure stops its combat, its alarm and its
// anger at the player (thuum ghidra/notes/hostility-1-7-104.md)
void StopCombatOnClient(MpActor& client, uint32_t figureId)
{
  const std::vector<std::optional<
    std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
    noArgs;
  SpSnippet("Actor", "StopCombatAlarm", noArgs, figureId)
    .Execute(&client, SpSnippetMode::kNoReturnResult);
}
}

void PartOne::TickFights(uint64_t nowMs)
{
  // a party of a pair, while the server can name it: a user plays it, or a
  // host's game simulates it (a hosted NPC, thuum docs/verbs/npc-stream.md
  // H-53h); a pair that lost either is forgotten, an unhosted cell's NPC
  // with it
  const auto played = [&](MpActor& actor) {
    return serverState.UserByActor(&actor) != Networking::InvalidUserId;
  };
  const auto party = [&](uint32_t formId) -> MpActor* {
    const auto& form = worldState.LookupFormById(formId);
    MpActor* actor = form ? form->AsActor() : nullptr;
    return actor && (played(*actor) || worldState.HostOf(*actor) != nullptr)
      ? actor
      : nullptr;
  };
  for (const auto& pair : pImpl->fights->pairs()) {
    MpActor* a = party(pair.a);
    MpActor* b = party(pair.b);
    if (!a || !b) {
      pImpl->fights->forget(a ? pair.b : pair.a);
      continue;
    }
    bool apart = true;
    if (a->GetCellOrWorld() == b->GetCellOrWorld()) {
      const auto cell = worldState.GetEspm().GetBrowser().LookupById(
        a->GetCellOrWorld().ToFormId(worldState.espmFiles));
      const bool interior =
        cell.rec && cell.rec->GetType() == espm::CELL::kType;
      apart = skymp::rules::hostility_apart(
        (a->GetPos() - b->GetPos()).Length(), interior);
    }
    if (pImpl->fights->check(pair.a, pair.b, apart, nowMs)) {
      // a user's game is told the end; a hosted NPC's host engine ends its
      // own combat when it loses its target
      if (played(*a)) {
        StopCombatOnClient(*a, pair.b);
      }
      if (played(*b)) {
        StopCombatOnClient(*b, pair.a);
      }
      spdlog::info("PartOne::TickFights - hostility: the fight between {:x} "
                   "and {:x} is over ({}); the players' games are told",
                   pair.a, pair.b, apart ? "apart" : "a minute quiet");
    }
  }
}

namespace {
// thuum docs/verbs/cell-host.md: the cells the server's grid gives a player
// at `pos`: the 3x3 of exterior grid cells around it (4096 units a side, the
// squares with no CELL in any file skipped), or its interior alone
// each reached cell with its ring: 0 the player's own square, 1 a
// neighbour (the nearness the cold start ranks by, npc-handoff D10); an
// interior is its own square
std::vector<std::pair<uint32_t, uint8_t>> ReachCells(
  const WorldState& worldState, uint32_t cellOrWorld, const NiPoint3& pos)
{
  std::vector<std::pair<uint32_t, uint8_t>> cells;
  for (int dx = -1; dx <= 1; ++dx) {
    for (int dy = -1; dy <= 1; ++dy) {
      const NiPoint3 at{ pos.x + 4096.f * static_cast<float>(dx),
                         pos.y + 4096.f * static_cast<float>(dy), pos.z };
      if (const auto cell = worldState.CellOf(cellOrWorld, at)) {
        const uint8_t ring =
          static_cast<uint8_t>(std::max(std::abs(dx), std::abs(dy)));
        const auto it =
          std::find_if(cells.begin(), cells.end(),
                       [&](const auto& c) { return c.first == *cell; });
        if (it == cells.end()) {
          cells.emplace_back(*cell, ring);
        } else if (ring < it->second) {
          it->second = ring;
        }
      }
    }
  }
  return cells;
}

// the player actor `formId` while a user plays it
MpActor* PlayerActor(PartOne& partOne, uint32_t formId)
{
  const auto& form = partOne.worldState.LookupFormByIdNoLoad(formId);
  MpActor* actor = form ? form->AsActor() : nullptr;
  return actor &&
      partOne.serverState.UserByActor(actor) != Networking::InvalidUserId
    ? actor
    : nullptr;
}

// the actors of `cell` that `viewer`'s game sees flip their hosted flag:
// the host's own from now, hosted by other on every other screen (and on
// every screen while `host` is nobody)
void FlipHostedFlags(PartOne& partOne, MpActor& viewer, uint32_t cell,
                     MpActor* host)
{
  for (MpObjectReference* emitter : viewer.GetEmitters()) {
    MpActor* actor = emitter ? emitter->AsActor() : nullptr;
    if (!actor ||
        partOne.serverState.UserByActor(actor) != Networking::InvalidUserId) {
      continue;
    }
    if (partOne.worldState.CellOf(*actor) != cell) {
      continue;
    }
    // the per-actor rows survive a grant or a release they do not concern
    // (ADR-047's follows, D22's margin hold, ADR-052's claims): the word
    // names the simulator the table answers, which is the host for an actor
    // with no row (a grant into the cell ends the rows into it)
    (void)host;
    actor->UpdateHoster(partOne.SimulatorOf(*actor));
  }
}

void ApplyHostDecisions(PartOne& partOne,
                        const rust::Vec<skymp::rules::HostDecision>& decisions)
{
  for (const auto& d : decisions) {
    switch (d.kind) {
      case skymp::rules::HostKind::Grant: {
        spdlog::info("PartOne - cell-host: {:x} hosts cell {:x} at epoch {}",
                     d.player, d.cell, d.epoch);
        // the cell's props nobody holds live are the new host's (thuum
        // docs/verbs/prop-physics.md, ADR-058 decision 1)
        partOne.PropsHostChanged(d.cell, d.player);
        MpActor* host = PlayerActor(partOne, d.player);
        if (!host) {
          break;
        }
        HostGrantMessage grant;
        grant.cell = d.cell;
        grant.epoch = d.epoch;
        partOne.GetSendTarget().Send(partOne.serverState.UserByActor(host),
                                     grant, true);
        FlipHostedFlags(partOne, *host, d.cell, host);
        partOne.ReplayHeldSightings(d.cell, *host);
        break;
      }
      case skymp::rules::HostKind::Release: {
        spdlog::info(
          "PartOne - cell-host: {:x} stops hosting cell {:x} (epoch {})",
          d.player, d.cell, d.epoch);
        MpActor* old = PlayerActor(partOne, d.player);
        if (!old) {
          break;
        }
        HostReleaseMessage release;
        release.cell = d.cell;
        release.epoch = d.epoch;
        partOne.GetSendTarget().Send(partOne.serverState.UserByActor(old),
                                     release, true);
        FlipHostedFlags(partOne, *old, d.cell, nullptr);
        break;
      }
      case skymp::rules::HostKind::Unhosted:
        spdlog::info("PartOne - cell-host: cell {:x} is unhosted, frozen at "
                     "its record (epoch {} kept)",
                     d.cell, d.epoch);
        break;
      case skymp::rules::HostKind::ClaimEnded: {
        // thuum docs/verbs/npc-talk.md (ADR-052): a claim's lease ran out;
        // the cell's rule names the simulator again and the word follows
        auto& form = partOne.worldState.LookupFormByIdNoLoad(d.actor);
        MpActor* actor = form ? form->AsActor() : nullptr;
        spdlog::info("[verb:npc-talk] the claim on {:x} in cell {:x} ended "
                     "with its lease; the cell's rule stands",
                     d.actor, d.cell);
        if (actor) {
          actor->UpdateHoster(partOne.SimulatorOf(*actor));
        }
        break;
      }
      default:
        break;
    }
  }
}
}

void PartOne::RefreshReach(MpActor& player)
{
  const uint32_t cellOrWorld =
    player.GetCellOrWorld().ToFormId(worldState.espmFiles);
  const NiPoint3& pos = player.GetPos();
  const Impl::Square square{
    cellOrWorld, static_cast<int32_t>(std::floor(pos.x / 4096.f)),
    static_cast<int32_t>(std::floor(pos.y / 4096.f))
  };
  const auto it = pImpl->lastSquareByPlayer.find(player.GetFormId());
  if (it != pImpl->lastSquareByPlayer.end() && it->second == square) {
    return;
  }
  pImpl->lastSquareByPlayer[player.GetFormId()] = square;
  SendFates(player, cellOrWorld, pos);
  const auto reached = ReachCells(worldState, cellOrWorld, pos);
  std::vector<uint32_t> cells;
  std::vector<uint8_t> rings;
  for (const auto& [cell, ring] : reached) {
    cells.push_back(cell);
    rings.push_back(ring);
  }
  ApplyHostDecisions(*this,
                     pImpl->cells->reach_ranked(
                       player.GetFormId(),
                       rust::Slice<const uint32_t>(cells.data(), cells.size()),
                       rust::Slice<const uint8_t>(rings.data(), rings.size()),
                       SteadyNowMs()));
}

uint16_t PartOne::FateRollLevel(MpActor& loader, uint32_t cellOrWorld,
                                int16_t sx, int16_t sy)
{
  const auto levelOf = [](MpActor& actor) -> std::optional<uint16_t> {
    const auto record = actor.GetActorValueRecord();
    if (!record) {
      return std::nullopt;
    }
    return std::max<uint16_t>(1, record->level);
  };
  const auto& rule = worldState.npcRollLevel;
  if (rule == "fixed") {
    return std::max<uint16_t>(1, worldState.npcRollLevelFixed);
  }
  if (rule == "loader") {
    return levelOf(loader).value_or(1);
  }
  // the party: every player online with a record; near, those within
  // kFateRing squares of the loader's square in its world or cell
  std::vector<uint16_t> near;
  std::vector<uint16_t> online;
  for (Networking::UserId userId = 0; userId <= serverState.maxConnectedId;
       ++userId) {
    MpActor* actor = serverState.ActorByUser(userId);
    if (!actor) {
      continue;
    }
    const auto level = levelOf(*actor);
    if (!level) {
      continue;
    }
    online.push_back(*level);
    if (actor->GetCellOrWorld().ToFormId(worldState.espmFiles) !=
        cellOrWorld) {
      continue;
    }
    const auto ax = static_cast<int16_t>(actor->GetPos().x / 4096);
    const auto ay = static_cast<int16_t>(actor->GetPos().y / 4096);
    if (std::abs(ax - sx) <= kFateRing && std::abs(ay - sy) <= kFateRing) {
      near.push_back(*level);
    }
  }
  const auto& party = near.empty() ? online : near;
  if (party.empty()) {
    return levelOf(loader).value_or(1);
  }
  if (rule == "party-min") {
    return *std::min_element(party.begin(), party.end());
  }
  // party-mean, and any rule word ScampServer did not recognize (it warns)
  uint32_t sum = 0;
  for (const auto level : party) {
    sum += level;
  }
  return std::max<uint16_t>(
    1, static_cast<uint16_t>(sum / static_cast<uint32_t>(party.size())));
}

void PartOne::SendFates(MpActor& player, uint32_t cellOrWorld,
                        const NiPoint3& pos)
{
  const auto userId = serverState.UserByActor(&player);
  if (userId == Networking::InvalidUserId || !worldState.HasEspm()) {
    return;
  }
  // the grid's own squares (MpObjectReference's GetGridPos truncates toward
  // zero), so the loaded chunks and the listeners' grid agree
  const auto sx = static_cast<int16_t>(pos.x / 4096);
  const auto sy = static_cast<int16_t>(pos.y / 4096);
  auto& fated = pImpl->fatedSquares[player.GetFormId()];
  std::vector<Impl::Square> fresh;
  for (int16_t dx = -kFateRing; dx <= kFateRing; ++dx) {
    for (int16_t dy = -kFateRing; dy <= kFateRing; ++dy) {
      const Impl::Square square{ cellOrWorld, sx + dx, sy + dy };
      if (fated.insert(square).second) {
        fresh.push_back(square);
      }
    }
  }
  if (fresh.empty()) {
    return;
  }
  // the ring's records loaded now, rolled at the level the setting's rule
  // gives (ADR-049 decision 1 with the memo's D2: the loader's own, the
  // party's mean or low end, or a number)
  const uint16_t level = FateRollLevel(player, cellOrWorld, sx, sy);
  worldState.fateLoaderLevel = level;
  worldState.LoadChunksAround(cellOrWorld, sx, sy, kFateRing);
  worldState.fateLoaderLevel.reset();

  auto& grid = *worldState.GetGrids()[cellOrWorld].grid;
  std::set<uint32_t> seen;
  ActorFateMessage msg;
  size_t sent = 0;
  const auto flush = [&] {
    if (!msg.fates.empty()) {
      GetSendTarget().Send(userId, msg, true);
      sent += msg.fates.size();
      msg.fates.clear();
    }
  };
  for (const auto& square : fresh) {
    const int32_t x = std::get<1>(square);
    const int32_t y = std::get<2>(square);
    for (MpObjectReference* ref : grid.GetNeighboursByPosition(
           static_cast<int16_t>(x), static_cast<int16_t>(y))) {
      MpActor* actor = ref ? ref->AsActor() : nullptr;
      if (!actor || actor->GetFormId() >= 0xff000000 ||
          serverState.UserByActor(actor) != Networking::InvalidUserId) {
        continue;
      }
      const NiPoint3& at = actor->GetPos();
      if (static_cast<int16_t>(at.x / 4096) != x ||
          static_cast<int16_t>(at.y / 4096) != y) {
        continue; // a neighbour of the square, listed under its own
      }
      if (!seen.insert(actor->GetFormId()).second) {
        continue;
      }
      // an actor this process rolled before any pass reached it (the grid's
      // 3x3 at a login, another square's ring) rolls at this level now
      actor->RollFateIfUnrolled(level);
      msg.fates.push_back(FateOf(worldState, *actor));
      if (msg.fates.size() >= kFatesPerMessage) {
        flush();
      }
    }
  }
  flush();
  if (sent) {
    spdlog::info("PartOne - npc-fate: {:x} (user {}) sent {} fate(s) for {} "
                 "new square(s) around ({}, {}) in {:x} at level {}",
                 player.GetFormId(), userId, sent, fresh.size(), sx, sy,
                 cellOrWorld, level);
  }
}

void PartOne::OnCellLoaded(MpActor& player, uint32_t cell, bool loaded)
{
  RefreshReach(player);
  const auto nowMs = SteadyNowMs();
  ApplyHostDecisions(
    *this,
    loaded ? pImpl->cells->loaded(player.GetFormId(), cell, nowMs)
           : pImpl->cells->unloaded(player.GetFormId(), cell, nowMs));
}

void PartOne::OnDeathState(MpActor& actor, bool isDead)
{
  // a player's actor alone: an NPC's death is npc-fate's and hosts nothing
  if (serverState.UserByActor(&actor) == Networking::InvalidUserId) {
    return;
  }
  const auto nowMs = SteadyNowMs();
  if (isDead) {
    spdlog::info("PartOne - cell-host: {:x} died: its cells start their "
                 "grace, no candidate until it stands (D18)",
                 actor.GetFormId());
    ApplyHostDecisions(*this, pImpl->cells->died(actor.GetFormId(), nowMs));
  } else {
    spdlog::info("PartOne - cell-host: {:x} stands again: a candidate again "
                 "(D18)",
                 actor.GetFormId());
    ApplyHostDecisions(*this,
                       pImpl->cells->respawned(actor.GetFormId(), nowMs));
  }
}

void PartOne::OnActorSighting(MpActor& reporter, uint32_t refrId,
                              uint32_t worldOrCell, const NiPoint3& pos,
                              const NiPoint3& rot)
{
  // each reason a sighting is dropped, once a minute per actor: the lab
  // reads the server log, and a roster that changes nothing must say why
  static std::unordered_map<uint32_t, uint64_t> lastDropLogMs;
  const auto drop = [&](const char* why) {
    const uint64_t now = SteadyNowMs();
    auto& last = lastDropLogMs[refrId];
    if (last != 0 && now - last < 60'000) {
      return;
    }
    last = now;
    spdlog::info("PartOne - npc-puppet: {:x}'s sighting of {:x} at ({:.0f}, "
                 "{:.0f}, {:.0f}) in {:x} dropped: {}",
                 reporter.GetFormId(), refrId, pos.x, pos.y, pos.z,
                 worldOrCell, why);
  };
  // a game-file actor alone (the validator's bound too); a player's actor
  // never
  if (refrId >= 0xff000000) {
    return drop("not a game-file id");
  }
  const auto cell = worldState.CellOf(worldOrCell, pos);
  if (!cell) {
    return drop("no cell at that position");
  }
  const auto host = GetCellHost(*cell);
  if (!host.hosted || host.player != reporter.GetFormId()) {
    // another game's cell, or nobody's yet: that host reports it, or the
    // grant comes first and replays what it held (finding 66)
    if (!host.hosted) {
      HoldSighting(reporter.GetFormId(), *cell, refrId, worldOrCell, pos, rot);
    }
    return drop(host.hosted ? "the cell is another game's"
                            : "the cell is nobody's yet");
  }
  // the record, loaded on demand where the game files place it; the host's
  // engine has it attached and enabled, so the loader's disabled filters
  // yield (an initially disabled placement a quest enabled, a child of a
  // marker that is on now)
  const auto& form = worldState.LookupFormById(refrId, nullptr, true);
  MpActor* actor = form ? form->AsActor() : nullptr;
  if (!actor) {
    return drop("no actor record loads for it");
  }
  if (serverState.UserByActor(actor) != Networking::InvalidUserId) {
    return drop("a player's actor");
  }
  const auto standing = worldState.CellOf(*actor);
  if (standing && *standing == *cell) {
    return; // already where the host's engine has it: the usual case, silent
  }
  if (MpActor* other = worldState.HostOf(*actor);
      other && other != &reporter) {
    // hosted by another game where the record stands: its word, not this
    // one's; the records meet at a handoff (npc-handoff). Playtest 37
    // (2026-10-28, finding 38, "we have 2 embrys"): after two fresh
    // launches the engines disagreed about where a resident stood, the
    // first sighting placed the record in the inn its sighter hosts, and the
    // other game's own reference outside stayed an orphan no record drives
    // (no promote, no demote, no move; frozen on its screen). The sighter's
    // engine is told to hide its copy (ObjectReference.Disable, no fade),
    // once a minute per sighter and actor; formView re-enables a disabled
    // reference when the record comes back into that game's grid
    static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point>
      hidden;
    const uint64_t key =
      (static_cast<uint64_t>(reporter.GetFormId()) << 32) | refrId;
    const auto now = std::chrono::steady_clock::now();
    auto& last = hidden[key];
    if (last == std::chrono::steady_clock::time_point{} ||
        now - last >= std::chrono::minutes(1)) {
      last = now;
      const std::vector<std::optional<
        std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
        noFade{ false };
      SpSnippet("ObjectReference", "Disable", noFade, refrId)
        .Execute(&reporter, SpSnippetMode::kNoReturnResult);
      spdlog::info("PartOne - npc-puppet: {:x}'s own copy of {:x} hidden (its "
                   "record stands in {:x}, which {:x} hosts)",
                   reporter.GetFormId(), refrId, standing.value_or(0),
                   other->GetFormId());
    }
    return drop("its record stands in a cell another game hosts");
  }
  spdlog::info("PartOne - npc-puppet: {:x} sights {:x} at ({:.0f}, {:.0f}, "
               "{:.0f}) in {:x}, cell {:x}; the record stood in {:x}",
               reporter.GetFormId(), refrId, pos.x, pos.y, pos.z, worldOrCell,
               *cell, standing.value_or(0));
  // the three sets of MpActor::Teleport without its message (the actor has
  // no game of its own): the new grid, then the position subscribes it and
  // CreateActor carries the word to every game in reach
  actor->SetCellOrWorldObsolete(
    FormDesc::FromFormId(worldOrCell, worldState.espmFiles));
  actor->SetPos(pos);
  actor->SetAngle(rot);
}

void PartOne::HoldSighting(uint32_t reporter, uint32_t cell, uint32_t refrId,
                           uint32_t worldOrCell, const NiPoint3& pos,
                           const NiPoint3& rot)
{
  const uint64_t now = SteadyNowMs();
  auto& held = pImpl->heldSightings[cell];
  // the reporter's own earlier entry for the reference gives way to this one
  // (another reporter's is its own: a roster landing last must not evict the
  // first's, or the first's grant replays nothing), and the cell's expired
  // entries go with it
  held.erase(std::remove_if(held.begin(), held.end(),
                            [&](const Impl::HeldSighting& h) {
                              return (h.reporter == reporter &&
                                      h.refrId == refrId) ||
                                now - h.heldAtMs > kHeldSightingMs;
                            }),
             held.end());
  if (held.size() >= kHeldSightingsPerCell) {
    return;
  }
  // the total across cells: full, the oldest entry anywhere makes room (the
  // newest sighting is the live one), said once in ten seconds
  if (HeldSightingsCount() >= kHeldSightingsTotal) {
    std::vector<Impl::HeldSighting>* oldestIn = nullptr;
    size_t oldestAt = 0;
    for (auto& entry : pImpl->heldSightings) {
      auto& entries = entry.second;
      for (size_t i = 0; i < entries.size(); ++i) {
        if (!oldestIn ||
            entries[i].heldAtMs < (*oldestIn)[oldestAt].heldAtMs) {
          oldestIn = &entries;
          oldestAt = i;
        }
      }
    }
    if (oldestIn) {
      oldestIn->erase(oldestIn->begin() +
                      static_cast<std::ptrdiff_t>(oldestAt));
    }
    static uint64_t lastFullLogMs = 0;
    if (lastFullLogMs == 0 || now - lastFullLogMs >= 10'000) {
      lastFullLogMs = now;
      spdlog::warn("PartOne - npc-puppet: {} sightings held across cells, "
                   "the cap: the oldest dropped for {:x}'s sighting of {:x} "
                   "in cell {:x}",
                   kHeldSightingsTotal, reporter, refrId, cell);
    }
  }
  held.push_back(
    Impl::HeldSighting{ reporter, refrId, worldOrCell, pos, rot, now });
}

void PartOne::ReplayHeldSightings(uint32_t cell, MpActor& host)
{
  auto it = pImpl->heldSightings.find(cell);
  if (it == pImpl->heldSightings.end()) {
    return;
  }
  const uint64_t now = SteadyNowMs();
  // the host's own entries leave the hold to replay; another reporter's stay
  // for its own grant while they last; the expired go
  std::vector<Impl::HeldSighting> mine;
  std::vector<Impl::HeldSighting> kept;
  for (const auto& h : it->second) {
    if (now - h.heldAtMs > kHeldSightingMs) {
      continue;
    }
    (h.reporter == host.GetFormId() ? mine : kept).push_back(h);
  }
  if (kept.empty()) {
    pImpl->heldSightings.erase(it);
  } else {
    it->second = std::move(kept);
  }
  for (const auto& h : mine) {
    OnActorSighting(host, h.refrId, h.worldOrCell, h.pos, h.rot);
  }
  if (!mine.empty()) {
    spdlog::info("PartOne - npc-puppet: {:x} hosts cell {:x}: {} sighting(s) "
                 "held before the grant replayed (finding 66)",
                 host.GetFormId(), cell, mine.size());
  }
}

void PartOne::SweepHeldSightings(uint64_t nowMs)
{
  for (auto it = pImpl->heldSightings.begin();
       it != pImpl->heldSightings.end();) {
    auto& held = it->second;
    held.erase(std::remove_if(held.begin(), held.end(),
                              [&](const Impl::HeldSighting& h) {
                                return nowMs - h.heldAtMs > kHeldSightingMs;
                              }),
               held.end());
    if (held.empty()) {
      it = pImpl->heldSightings.erase(it);
    } else {
      ++it;
    }
  }
}

void PartOne::ForgetHeldSightings(uint32_t reporter)
{
  size_t forgotten = 0;
  for (auto it = pImpl->heldSightings.begin();
       it != pImpl->heldSightings.end();) {
    auto& held = it->second;
    const size_t before = held.size();
    held.erase(std::remove_if(held.begin(), held.end(),
                              [&](const Impl::HeldSighting& h) {
                                return h.reporter == reporter;
                              }),
               held.end());
    forgotten += before - held.size();
    if (held.empty()) {
      it = pImpl->heldSightings.erase(it);
    } else {
      ++it;
    }
  }
  if (forgotten > 0) {
    spdlog::info("PartOne - npc-puppet: {:x} left: {} held sighting(s) of "
                 "its forgotten",
                 reporter, forgotten);
  }
}

size_t PartOne::HeldSightingsCount(std::optional<uint32_t> cell) const
{
  if (cell) {
    const auto it = pImpl->heldSightings.find(*cell);
    return it == pImpl->heldSightings.end() ? 0 : it->second.size();
  }
  size_t n = 0;
  for (const auto& entry : pImpl->heldSightings) {
    n += entry.second.size();
  }
  return n;
}

void PartOne::TickCellHostsEverySecond()
{
  const auto now = std::chrono::steady_clock::now();
  if (now - pImpl->lastCellsTick < std::chrono::seconds(1)) {
    return;
  }
  pImpl->lastCellsTick = now;
  TickCellHosts(SteadyNowMs());
  TickDeaths(SteadyNowMs());
  SweepHeldSightings(SteadyNowMs());
}

void PartOne::TickDeaths(uint64_t nowMs)
{
  if (worldState.pendingDeaths.empty()) {
    return;
  }
  const std::vector<uint32_t> ids(worldState.pendingDeaths.begin(),
                                  worldState.pendingDeaths.end());
  for (const auto id : ids) {
    auto& form = worldState.LookupFormByIdNoLoad(id);
    MpActor* actor = form ? form->AsActor() : nullptr;
    if (!actor || actor->IsDead() || !actor->HasPendingDeath()) {
      worldState.pendingDeaths.erase(id);
      continue;
    }
    if (nowMs < actor->PendingDeathSinceMs() + MpActor::kPendingDeathMs) {
      continue;
    }
    MpActor* aggressor = nullptr;
    if (const auto killerId = actor->PendingDeathAggressor()) {
      auto& killerForm = worldState.LookupFormByIdNoLoad(killerId);
      aggressor = killerForm ? killerForm->AsActor() : nullptr;
    }
    spdlog::info("[verb:npc-fate] death by the server's word on {:x}: no "
                 "report from its host within {} ms (aggressor {:x})",
                 id, MpActor::kPendingDeathMs, actor->PendingDeathAggressor());
    actor->Kill(aggressor);
  }
}

void PartOne::TickFateRespawnsEverySecond()
{
  const auto now = std::chrono::steady_clock::now();
  if (now - pImpl->lastFateRespawnsTick < std::chrono::seconds(1)) {
    return;
  }
  pImpl->lastFateRespawnsTick = now;
  TickFateRespawns();
}

void PartOne::TickFateRespawns()
{
  if (worldState.fateRespawnDue.empty()) {
    return;
  }
  const float day =
    worldState.gameTime ? worldState.gameTime().daysPassed : 0.f;
  const float hours = worldState.FateRespawnHours();
  std::vector<uint32_t> dropped;
  std::vector<uint32_t> due;
  for (const auto id : worldState.fateRespawnDue) {
    auto& form = worldState.LookupFormByIdNoLoad(id);
    MpActor* actor = form ? form->AsActor() : nullptr;
    if (!actor || !actor->IsDead() ||
        !actor->GetChangeForm().fateDiedAt.has_value()) {
      dropped.push_back(id);
      continue;
    }
    // thuum docs/research/m3/decisions-after-playtest-34.md D21: a body taken
    // out of the world while it lay waits for its enable (MpActor::Enable
    // puts it back on the clock, the death's stamp kept)
    if (actor->IsDisabled()) {
      spdlog::info("[verb:npc-fate] {:x} disabled while its body lay: its "
                   "return waits for its enable (D21)",
                   id);
      dropped.push_back(id);
      continue;
    }
    if (day >= *actor->GetChangeForm().fateDiedAt + hours / 24.f) {
      due.push_back(id);
    }
  }
  for (const auto id : dropped) {
    worldState.fateRespawnDue.erase(id);
  }
  for (const auto id : due) {
    worldState.fateRespawnDue.erase(id);
    auto& form = worldState.LookupFormByIdNoLoad(id);
    if (MpActor* actor = form ? form->AsActor() : nullptr) {
      actor->RespawnFated();
    }
  }
}

void PartOne::SetRoundTripSource(
  std::function<int64_t(Networking::UserId)> source)
{
  pImpl->roundTripSource = std::move(source);
}

void PartOne::SetSilenceSource(
  std::function<int64_t(Networking::UserId)> source)
{
  pImpl->silenceSource = std::move(source);
}

void PartOne::TickCellHosts(uint64_t nowMs)
{
  for (size_t i = 0, n = serverState.maxConnectedId; i <= n; ++i) {
    const auto userId = static_cast<Networking::UserId>(i);
    MpActor* actor = serverState.ActorByUser(userId);
    if (!actor) {
      continue;
    }
    RefreshReach(*actor);
    if (pImpl->roundTripSource) {
      const int64_t ms = pImpl->roundTripSource(userId);
      if (ms >= 0) {
        pImpl->cells->rtt(
          actor->GetFormId(),
          static_cast<uint32_t>(std::min<int64_t>(ms, UINT32_MAX)));
      }
    }
    // the quitting host: a connection silent past kStaleHostMs is stale
    // (its cells start their grace now, not at netcode's timeout), one
    // heard again is revived; -1 (unknown, the mock's users) changes
    // nothing
    if (pImpl->silenceSource) {
      const int64_t silenceMs = pImpl->silenceSource(userId);
      const bool wasStale = pImpl->staleUsers.count(userId) > 0;
      // only a host is marked: a silent visitor hosts nothing to pass on,
      // and a game's first loads after a login (3 to 5 s of silence in the
      // lab, x-host-kill 205837 and 214420) come before any grant
      const bool hostsSomething = [&] {
        for (const auto& row : pImpl->cells->hosts()) {
          if (row.hosted && row.player == actor->GetFormId()) {
            return true;
          }
        }
        return false;
      }();
      if (silenceMs >= kStaleHostMs && !wasStale && hostsSomething) {
        pImpl->staleUsers.insert(userId);
        spdlog::info("PartOne - cell-host: {:x} (user {}) silent {} ms, "
                     "stale: its cells start their grace",
                     actor->GetFormId(), userId, silenceMs);
        ApplyHostDecisions(*this,
                           pImpl->cells->stale(actor->GetFormId(), nowMs));
      } else if (silenceMs >= 0 && silenceMs < kStaleHostMs && wasStale) {
        pImpl->staleUsers.erase(userId);
        spdlog::info("PartOne - cell-host: {:x} (user {}) heard again after "
                     "its silence, revived",
                     actor->GetFormId(), userId);
        ApplyHostDecisions(*this,
                           pImpl->cells->revived(actor->GetFormId(), nowMs));
      }
    }
  }
  ApplyHostDecisions(*this, pImpl->cells->tick(nowMs));
}

skymp::rules::CellHost PartOne::GetCellHost(uint32_t cell) const
{
  return pImpl->cells->host_of(cell);
}

std::vector<PartOne::CellHostEntry> PartOne::GetCellHosts() const
{
  std::vector<CellHostEntry> out;
  for (const auto& row : pImpl->cells->hosts()) {
    out.push_back({ row.cell, row.hosted, row.player, row.epoch });
  }
  return out;
}

std::vector<PartOne::CellFollowEntry> PartOne::GetFollows() const
{
  std::vector<CellFollowEntry> out;
  for (const auto& row : pImpl->cells->follows()) {
    out.push_back({ row.actor, row.player, row.cell });
  }
  return out;
}

namespace {
// the facts the table judges a move by (npc-handoff D6): the origin and
// destination cells, two exterior squares touching in one worldspace, or a
// door (an interior at either end, any actor's: memo D54 B); none when the
// destination is no cell
std::optional<skymp::rules::CellMove> CellMoveFacts(WorldState& worldState,
                                                    const MpActor& sender,
                                                    const MpActor& actor,
                                                    uint32_t destWorldOrCell,
                                                    const NiPoint3& destPos)
{
  const auto origin = worldState.CellOf(actor);
  const auto dest = worldState.CellOf(destWorldOrCell, destPos);
  if (!dest) {
    return std::nullopt;
  }
  // the facts the table judges by (npc-handoff D6): two exterior squares
  // touching in one worldspace, or a door (an interior at either end). The
  // door asked the actor's persistence until 2026-10-28 (the engine keeps
  // simulating a persistent actor off screen, so its host could vouch for
  // it indoors), and Hilde 0x35534, Riverwood's one resident placed without
  // the flag, was refused at her own door 0x1cbb0 into RiverwoodSvensHouse
  // 0x1cb84 while her host's engine walked her back to it for minutes
  // (thuum memo D54, Eli's B: nothing in the engine or TES3MP treats her
  // door differently). Every actor's door counts now: into a cell nobody
  // hosts the record follows the host's report in and the host's engine
  // holds the actor where the door put it, unloaded, its low-process
  // samples the follow's; into another live host's cell the pass (D9)
  const uint32_t originWorldOrCell =
    actor.GetCellOrWorld().ToFormId(worldState.espmFiles);
  const bool destInterior = worldState.IsInteriorCell(destWorldOrCell);
  const bool originInterior = worldState.IsInteriorCell(originWorldOrCell);
  bool adjacent = false;
  if (origin && !destInterior && !originInterior &&
      originWorldOrCell == destWorldOrCell) {
    const NiPoint3& from = actor.GetPos();
    const int dx = std::abs(static_cast<int>(std::floor(from.x / 4096.f)) -
                            static_cast<int>(std::floor(destPos.x / 4096.f)));
    const int dy = std::abs(static_cast<int>(std::floor(from.y / 4096.f)) -
                            static_cast<int>(std::floor(destPos.y / 4096.f)));
    adjacent = std::max(dx, dy) <= 1;
  }
  const bool door = destInterior || originInterior;
  // the pass margin's fact (npc-handoff D22, 2026-10-26): how far inside the
  // destination's 4096-unit square the report stands, from its nearest edge;
  // an interior is deep by definition
  uint32_t edgeUnits = std::numeric_limits<uint32_t>::max();
  if (!destInterior) {
    const float fx = destPos.x - std::floor(destPos.x / 4096.f) * 4096.f;
    const float fy = destPos.y - std::floor(destPos.y / 4096.f) * 4096.f;
    const float edge = std::min({ fx, 4096.f - fx, fy, 4096.f - fy, 4096.f });
    edgeUnits = static_cast<uint32_t>(std::max(0.f, std::floor(edge)));
  }
  return skymp::rules::CellMove{ sender.GetFormId(), actor.GetFormId(),
                                 origin.value_or(0), *dest,
                                 adjacent,           door,
                                 edgeUnits };
}
}

PartOne::MoveVerdict PartOne::MayMoveActor(const MpActor& sender,
                                           const MpActor& actor,
                                           uint32_t destWorldOrCell,
                                           const NiPoint3& destPos)
{
  auto move =
    CellMoveFacts(worldState, sender, actor, destWorldOrCell, destPos);
  if (!move) {
    return MoveVerdict::Refuse;
  }
  if (!pImpl->followEnabled) {
    move->adjacent = false;
    move->door = false;
  }
  return static_cast<MoveVerdict>(
    pImpl->cells->may_move(*move, SteadyNowMs()));
}

PartOne::MoveVerdict PartOne::WouldMoveActor(const MpActor& sender,
                                             const MpActor& actor,
                                             uint32_t destWorldOrCell,
                                             const NiPoint3& destPos) const
{
  auto move = CellMoveFacts(const_cast<WorldState&>(worldState), sender, actor,
                            destWorldOrCell, destPos);
  if (!move) {
    return MoveVerdict::Refuse;
  }
  if (!pImpl->followEnabled) {
    move->adjacent = false;
    move->door = false;
  }
  return static_cast<MoveVerdict>(
    pImpl->cells->judge_move(*move, SteadyNowMs()));
}

uint32_t PartOne::SimulatorOf(const MpActor& actor) const
{
  const auto cell = worldState.CellOf(actor);
  return cell ? pImpl->cells->simulator_of(actor.GetFormId(), *cell) : 0u;
}

PartOne::ClaimVerdict PartOne::ClaimActor(MpActor& claimant, MpActor& actor)
{
  const auto cell = worldState.CellOf(actor);
  if (!cell) {
    return ClaimVerdict::Unloaded;
  }
  const auto code = pImpl->cells->claim_actor(
    actor.GetFormId(), claimant.GetFormId(), *cell, SteadyNowMs());
  const auto verdict = static_cast<ClaimVerdict>(code);
  if (verdict == ClaimVerdict::Granted || verdict == ClaimVerdict::Reclaimed) {
    // the word to every listening game: the claimant simulates it now
    actor.UpdateHoster(claimant.GetFormId());
  }
  return verdict;
}

bool PartOne::ReleaseClaim(MpActor& claimant, MpActor& actor)
{
  const bool released =
    pImpl->cells->release_claim(actor.GetFormId(), claimant.GetFormId());
  if (released) {
    actor.UpdateHoster(SimulatorOf(actor));
  }
  return released;
}

PartOne::PropClaimVerdict PartOne::ClaimProp(const MpActor& claimant,
                                             const MpObjectReference& prop,
                                             uint32_t cell, uint8_t touch)
{
  const auto code = pImpl->props->claim_prop(
    prop.GetFormId(), claimant.GetFormId(), cell, touch, SteadyNowMs());
  return static_cast<PropClaimVerdict>(code);
}

PartOne::PropTransformVerdict PartOne::PropTransform(
  const MpActor& sender, const MpObjectReference& prop, uint32_t cell,
  uint32_t recordCell, bool asleep, float moved)
{
  uint32_t cellHost = 0;
  if (recordCell != 0) {
    const auto host = pImpl->cells->host_of(recordCell);
    cellHost = host.hosted ? host.player : 0u;
  }
  skymp::rules::PropSampleFacts facts{};
  facts.refr = prop.GetFormId();
  facts.player = sender.GetFormId();
  facts.cell = cell;
  facts.cell_host = cellHost;
  facts.asleep = asleep;
  facts.moved = moved;
  facts.now_ms = SteadyNowMs();
  const auto code = pImpl->props->prop_transform(facts);
  return static_cast<PropTransformVerdict>(code);
}

uint32_t PartOne::PropLiveOwner(const MpObjectReference& prop) const
{
  return pImpl->props->prop_live_owner(prop.GetFormId(), SteadyNowMs());
}

uint32_t PartOne::PropOwnerOf(const MpObjectReference& prop) const
{
  const auto cell = worldState.CellOf(prop);
  uint32_t cellHost = 0;
  if (cell) {
    const auto host = pImpl->cells->host_of(*cell);
    cellHost = host.hosted ? host.player : 0u;
  }
  return pImpl->props->prop_owner_of(prop.GetFormId(), cellHost,
                                     SteadyNowMs());
}

uint32_t PartOne::PropEpochOf(const MpObjectReference& prop) const
{
  return pImpl->props->prop_epoch_of(prop.GetFormId());
}

void PartOne::PropsHostChanged(uint32_t cell, uint32_t host)
{
  pImpl->props->props_host_changed(cell, host, SteadyNowMs());
}

void PartOne::SetFollowEnabled(bool enabled)
{
  pImpl->followEnabled = enabled;
  spdlog::info("PartOne - cell-host: the follow is {}",
               enabled ? "on" : "off (cellFollow false)");
}

bool PartOne::SpendMovementBudget(uint32_t actorFormId, float ground)
{
  return pImpl->movementBudgets->spend(actorFormId, ground, SteadyNowMs());
}

void PartOne::PermitJump(uint32_t actorFormId, uint32_t cellId)
{
  pImpl->movementBudgets->permit_jump_interior(actorFormId, cellId,
                                               SteadyNowMs());
}

void PartOne::PermitJump(uint32_t actorFormId, uint32_t worldId, int16_t gridX,
                         int16_t gridY)
{
  pImpl->movementBudgets->permit_jump_exterior(actorFormId, worldId, gridX,
                                               gridY, SteadyNowMs());
}

namespace {
PartOne::JumpCheck ToJumpCheck(skymp::rules::JumpCheck c)
{
  switch (c) {
    case skymp::rules::JumpCheck::Landed:
      return PartOne::JumpCheck::Landed;
    case skymp::rules::JumpCheck::Waiting:
      return PartOne::JumpCheck::Waiting;
    default:
      return PartOne::JumpCheck::NoPermit;
  }
}
}

PartOne::JumpCheck PartOne::CheckJump(uint32_t actorFormId,
                                      uint32_t cellOrWorld, float x, float y)
{
  return ToJumpCheck(pImpl->movementBudgets->check_jump(
    actorFormId, cellOrWorld, x, y, SteadyNowMs()));
}

void PartOne::ExpectArrival(uint32_t actorFormId, uint32_t cellOrWorld,
                            float x, float y)
{
  pImpl->movementBudgets->expect_arrival(actorFormId, cellOrWorld, x, y,
                                         SteadyNowMs());
}

void PartOne::PinArrival(uint32_t actorFormId, uint32_t cellOrWorld,
                         const NiPoint3& at, float radius, uint64_t holdMs)
{
  pImpl->movementBudgets->pin(actorFormId, cellOrWorld, at.x, at.y, radius,
                              SteadyNowMs() + holdMs);
}

bool PartOne::PinnedAway(uint32_t actorFormId, uint32_t cellOrWorld,
                         const NiPoint3& at)
{
  return pImpl->movementBudgets->pinned_away(actorFormId, cellOrWorld, at.x,
                                             at.y, SteadyNowMs());
}

PartOne::JumpCheck PartOne::CheckArrival(uint32_t actorFormId,
                                         uint32_t cellOrWorld, float x,
                                         float y)
{
  return ToJumpCheck(pImpl->movementBudgets->check_arrival(
    actorFormId, cellOrWorld, x, y, SteadyNowMs()));
}

void PartOne::SetCastTeleportSettings(const std::string& castTeleportsJson)
{
  pImpl->castTeleports = skymp::rules::new_cast_teleports(castTeleportsJson);
  spdlog::info("PartOne::SetCastTeleportSettings - {}", castTeleportsJson);
}

void PartOne::SetBondSettings(const std::string& bondsJson)
{
  pImpl->bonds = skymp::rules::new_bonds(bondsJson);
  spdlog::info("PartOne::SetBondSettings - {}", bondsJson);
}

bool PartOne::IsBondSpell(uint32_t spellFormId) const
{
  const auto desc = FormDesc::FromFormId(spellFormId, worldState.espmFiles);
  return pImpl->bonds->bond_duration_ms(desc.shortFormId, desc.file) > 0;
}

void PartOne::RecordBond(uint32_t casterFormId, uint32_t targetFormId,
                         uint32_t spellFormId)
{
  const auto desc = FormDesc::FromFormId(spellFormId, worldState.espmFiles);
  const uint64_t now = SteadyNowMs();
  // a recast replaces the caster's standing bond: what the server imposed
  // for the old one ends first, unless the new bond is with the same target,
  // whose records the new claim refreshes
  const auto old = pImpl->bonds->supersede(casterFormId, now);
  if (old.found && old.target != targetFormId) {
    UnconcealBond(old, "replaced by the caster's new bond");
  }
  const auto bond = pImpl->bonds->record(casterFormId, targetFormId,
                                         desc.shortFormId, desc.file, now);
  spdlog::info("PartOne::RecordBond - {:x} bonds {:x} with spell {:x} (thuum "
               "docs/verbs/shadowbond.md)",
               casterFormId, targetFormId, spellFormId);
  if (bond.found) {
    ConcealBond(bond);
  }
}

namespace {
// thuum docs/verbs/shadowbond.md: how long the swap's places are held
// against the mod's own script move in the caster's game (its poll is 0.2 s;
// HYPOTHESIS that the second move comes within this, x-shadowbond reads it)
constexpr uint64_t kBondPinMs = 1'500;
// thuum docs/verbs/shadowbond.md (playtest twenty-five: the proc was cast in
// test 2's game, twice, without error, and test 2 saw both players visible):
// how long after the claim the proc is cast in the other games. The bond
// cast itself is relayed at the claim and runs on each game's next frame,
// and a cast ends an actor's invisibility unless it is the spell's own
// archetype (self-cast.md), so a proc that lands first is dispelled at once;
// the mod's own script waits 0.25 s after its OnEffectStart for the same
// reason. HYPOTHESIS until b-shadowbond's watch series shows the figure's
// av:Invisibility holding for the bond
constexpr uint64_t kBondProcCastDelayMs = 500;
}

bool PartOne::BreakBondsOf(uint32_t actorFormId)
{
  const auto broken = pImpl->bonds->break_of(actorFormId, SteadyNowMs());
  if (!broken.found) {
    return false;
  }
  // the acting actor's own concealment went with its act (EndConcealment);
  // the other's, and the target's own game's copy, go here
  UnconcealBond(broken, "an act of either");
  const auto caster = std::dynamic_pointer_cast<MpActor>(
    worldState.LookupFormById(broken.caster));
  const auto target = std::dynamic_pointer_cast<MpActor>(
    worldState.LookupFormById(broken.target));
  if (!caster || !target) {
    spdlog::warn("PartOne::BreakBondsOf - {:x} acts, but its bond's actors "
                 "{:x} and {:x} are not both here; nothing permitted",
                 actorFormId, broken.caster, broken.target);
    return false;
  }
  // the swap is a position change the server owns (R0): it moves both to
  // each other's places itself. The mod's script runs in the caster's game
  // only (ADR-029), so it moves the caster there too (the same spot, which
  // the arrival rule reads as the arrival) and only the FIGURE of the other
  // player, whose own game never moved: x-shadowbond 20261019-073758 on
  // 4c7e6998 swapped c1 alone. A report from elsewhere while the teleport
  // is on its way is dropped, not sent back (movement-speed.md, arrivals).
  // The window and slack the bond carries are its record's; the arrival
  // rule's radius and wait bound the landings now.
  LocationalData casterTo;
  casterTo.pos = target->GetPos();
  casterTo.rot = caster->GetAngle();
  casterTo.cellOrWorldDesc = target->GetCellOrWorld();
  LocationalData targetTo;
  targetTo.pos = caster->GetPos();
  targetTo.rot = target->GetAngle();
  targetTo.cellOrWorldDesc = caster->GetCellOrWorld();
  caster->Teleport(casterTo);
  target->Teleport(targetTo);
  // the mod's script in the caster's game moves the caster once more after
  // the swap, to where the target's figure now stands (its own spot), and
  // undid the swap (x-shadowbond 20261019-083537 on 13acbb35); a report
  // from away from the swap's place within the hold is sent back
  // the pin's radius is half the swap's own distance (clamped in the rule):
  // a swap shorter than the arrival radius slipped through a pin judged by
  // it (x-shadowbond 20261019-090907, the two 150 units apart)
  const float swapDistance = (casterTo.pos - targetTo.pos).Length();
  PinArrival(broken.caster,
             casterTo.cellOrWorldDesc.ToFormId(worldState.espmFiles),
             casterTo.pos, swapDistance / 2.f, kBondPinMs);
  PinArrival(broken.target,
             targetTo.cellOrWorldDesc.ToFormId(worldState.espmFiles),
             targetTo.pos, swapDistance / 2.f, kBondPinMs);
  spdlog::info("PartOne::BreakBondsOf - {:x} acts: its bond with {:x} breaks; "
               "the server swaps their places, {:x} to ({:.0f}, {:.0f}, "
               "{:.0f}) and {:x} to ({:.0f}, {:.0f}, {:.0f}) (thuum "
               "docs/verbs/shadowbond.md)",
               actorFormId,
               actorFormId == broken.caster ? broken.target : broken.caster,
               broken.caster, casterTo.pos.x, casterTo.pos.y, casterTo.pos.z,
               broken.target, targetTo.pos.x, targetTo.pos.y, targetTo.pos.z);
  return true;
}

void PartOne::SetPlayerGlobalsSettings(const std::string& playerGlobalsJson)
{
  pImpl->playerGlobals = skymp::rules::new_player_globals(playerGlobalsJson);
  spdlog::info("PartOne::SetPlayerGlobalsSettings - {}", playerGlobalsJson);
}

namespace {
std::vector<skymp::rules::PlayerGlobalEntry> ToEntries(
  const std::vector<PlayerGlobal>& globals)
{
  std::vector<skymp::rules::PlayerGlobalEntry> entries;
  entries.reserve(globals.size());
  for (const auto& g : globals) {
    entries.push_back(
      { g.global.shortFormId, rust::String(g.global.file), g.value });
  }
  return entries;
}

std::vector<PlayerGlobal> ToGlobals(
  const rust::Vec<skymp::rules::PlayerGlobalEntry>& entries)
{
  std::vector<PlayerGlobal> globals;
  globals.reserve(entries.size());
  for (const auto& e : entries) {
    globals.push_back({ FormDesc(e.short_id, std::string(e.file)), e.value });
  }
  return globals;
}
}

std::vector<uint32_t> PartOne::PlayerGlobalsWatch() const
{
  std::vector<uint32_t> watch;
  for (const auto& e : pImpl->playerGlobals->player_globals_listed()) {
    try {
      watch.push_back(FormDesc(e.short_id, std::string(e.file))
                        .ToFormId(worldState.espmFiles));
    } catch (const std::exception&) {
      // a listed global whose plugin this load order lacks
    }
  }
  return watch;
}

uint8_t PartOne::CheckPlayerGlobals(
  const std::vector<PlayerGlobal>& report) const
{
  const auto entries = ToEntries(report);
  return pImpl->playerGlobals->player_globals_check(
    rust::Slice<const skymp::rules::PlayerGlobalEntry>(entries.data(),
                                                       entries.size()));
}

std::vector<PlayerGlobal> PartOne::PlayerGlobalsAtLogin(
  const std::vector<PlayerGlobal>& record) const
{
  const auto entries = ToEntries(record);
  return ToGlobals(pImpl->playerGlobals->player_globals_at_login(
    rust::Slice<const skymp::rules::PlayerGlobalEntry>(entries.data(),
                                                       entries.size())));
}

std::vector<PlayerGlobal> PartOne::LethalPlayerGlobals(
  const std::vector<PlayerGlobal>& record) const
{
  const auto entries = ToEntries(record);
  return ToGlobals(pImpl->playerGlobals->player_globals_lethal(
    rust::Slice<const skymp::rules::PlayerGlobalEntry>(entries.data(),
                                                       entries.size())));
}

std::vector<PlayerGlobal> PartOne::MergePlayerGlobals(
  const std::vector<PlayerGlobal>& record,
  const std::vector<PlayerGlobal>& report)
{
  const auto recordEntries = ToEntries(record);
  const auto reportEntries = ToEntries(report);
  return ToGlobals(skymp::rules::player_globals_merge(
    rust::Slice<const skymp::rules::PlayerGlobalEntry>(recordEntries.data(),
                                                       recordEntries.size()),
    rust::Slice<const skymp::rules::PlayerGlobalEntry>(reportEntries.data(),
                                                       reportEntries.size())));
}

void PartOne::SetSharedQuestsSettings(const std::string& sharedQuestsJson)
{
  pImpl->sharedQuests = skymp::rules::new_shared_quests(sharedQuestsJson);
  spdlog::info("PartOne::SetSharedQuestsSettings - {}", sharedQuestsJson);
}

namespace {
std::vector<skymp::rules::QuestStageEntry> ToStageEntries(
  const std::vector<QuestStageDone>& stages)
{
  std::vector<skymp::rules::QuestStageEntry> entries;
  entries.reserve(stages.size());
  for (const auto& s : stages) {
    entries.push_back(
      { s.quest.shortFormId, rust::String(s.quest.file), s.stage });
  }
  return entries;
}

skymp::rules::QuestStageEntry ToStageEntry(const QuestStageDone& done)
{
  return { done.quest.shortFormId, rust::String(done.quest.file), done.stage };
}

std::vector<QuestStageDone> ToStages(
  const rust::Vec<skymp::rules::QuestStageEntry>& entries)
{
  std::vector<QuestStageDone> stages;
  stages.reserve(entries.size());
  for (const auto& e : entries) {
    stages.push_back({ FormDesc(e.short_id, std::string(e.file)), e.stage });
  }
  return stages;
}

// Skyrim.esm (thuum lab/esm.py on the 1.6.1170 master): STAT 0x3b XMarker,
// the journal's base, a marker with no model in the game, placed in WRLD
// 0x3c Tamriel (FormDesc::Tamriel) at the worldspace's far southwest, a
// cell no seat loads
constexpr uint32_t kWorldJournalBaseId = 0x3b;
constexpr float kWorldJournalX = -200000.f;
constexpr float kWorldJournalY = -200000.f;
}

uint8_t PartOne::SharedQuestsMode() const
{
  return pImpl->sharedQuests->shared_quests_mode();
}

uint8_t PartOne::CheckSharedQuest(const FormDesc& quest) const
{
  return pImpl->sharedQuests->shared_quests_check(quest.shortFormId,
                                                  quest.file);
}

bool PartOne::IsSharedQuestAllowed(const FormDesc& quest) const
{
  return pImpl->sharedQuests->shared_quests_allowed(quest.shortFormId,
                                                    quest.file);
}

std::vector<FormDesc> PartOne::SharedQuestsListed() const
{
  std::vector<FormDesc> listed;
  for (const auto& e : pImpl->sharedQuests->shared_quests_listed()) {
    listed.push_back(FormDesc(e.short_id, std::string(e.file)));
  }
  return listed;
}

std::vector<QuestStageDone> PartOne::SharedQuestsAtLogin()
{
  const auto entries = ToStageEntries(GetQuestStages());
  return ToStages(pImpl->sharedQuests->shared_quests_at_login(
    rust::Slice<const skymp::rules::QuestStageEntry>(entries.data(),
                                                     entries.size())));
}

bool PartOne::QuestStageDoneAlready(const QuestStageDone& done)
{
  const auto entries = ToStageEntries(GetQuestStages());
  return skymp::rules::quest_stage_done(
    rust::Slice<const skymp::rules::QuestStageEntry>(entries.data(),
                                                     entries.size()),
    ToStageEntry(done));
}

std::vector<QuestStageDone> PartOne::MergeQuestStage(
  const QuestStageDone& done)
{
  const auto entries = ToStageEntries(GetQuestStages());
  return ToStages(skymp::rules::quest_stages_merge(
    rust::Slice<const skymp::rules::QuestStageEntry>(entries.data(),
                                                     entries.size()),
    ToStageEntry(done)));
}

bool PartOne::QuestRecordFull()
{
  const auto entries = ToStageEntries(GetQuestStages());
  return skymp::rules::quest_record_full(
    rust::Slice<const skymp::rules::QuestStageEntry>(entries.data(),
                                                     entries.size()));
}

MpObjectReference& PartOne::WorldJournal()
{
  if (!worldState.LookupFormById(kWorldJournalRefId)) {
    // the constructor WorldState::LoadChangeForm uses to recreate a
    // server-made reference from its record (recType REFR, the base's type),
    // so a restart's form is this one; MpObjectReference::Init looks up no
    // record for an id above 0xff000000, and the base's scripts are read
    // lazily at the first Papyrus event (XMarker has none)
    worldState.AddForm(
      std::make_unique<MpObjectReference>(
        LocationalData{ { kWorldJournalX, kWorldJournalY, 0.f },
                        { 0.f, 0.f, 0.f },
                        FormDesc::Tamriel() },
        CreateFormCallbacks(), kWorldJournalBaseId, "STAT"),
      kWorldJournalRefId);
    spdlog::info("PartOne::WorldJournal - made the world's journal {:x} "
                 "(thuum docs/verbs/quest-stage.md)",
                 kWorldJournalRefId);
  }
  return worldState.GetFormAt<MpObjectReference>(kWorldJournalRefId);
}

std::vector<QuestStageDone> PartOne::GetQuestStages()
{
  return WorldJournal().GetQuestStages();
}

bool PartOne::SetQuestStages(std::vector<QuestStageDone> stages)
{
  return WorldJournal().SetQuestStages(std::move(stages));
}

void PartOne::SetSpellPlacedObjectsSettings(
  const std::string& spellPlacedObjectsJson)
{
  pImpl->placements = skymp::rules::new_placements(spellPlacedObjectsJson);
  spdlog::info("PartOne::SetSpellPlacedObjectsSettings - {}",
               spellPlacedObjectsJson);
}

namespace {
// the reasons as the rule numbers them (wire-rules placements RemovalWhy
// and Refusal)
const char* PlacementRemovalWhy(uint8_t why)
{
  return why == 0 ? "its linger ran out" : "it stood for its maxS";
}

const char* PlacementRefusalWhy(uint8_t why)
{
  switch (why) {
    case 0:
      return "the caster holds its perCaster sets";
    case 1:
      return "the world holds its 64 sets";
    default:
      return "the caster's game reported no base within the window";
  }
}

// the report verdicts as the rule numbers them (wire-rules placements
// ReportRefusal through the bridge's PlacementReportVerdict)
const char* PlacementReportCode(uint8_t why)
{
  switch (why) {
    case 1:
      return "E_PLACED_SET";
    case 2:
      return "E_PLACED_LATE";
    case 3:
      return "E_PLACED_BASE";
    case 4:
      return "E_PLACED_REPEAT";
    case 5:
      return "E_PLACED_NAN";
    case 6:
      return "E_PLACED_RANGE";
    default:
      return "E_PLACED_UNKNOWN";
  }
}
}

uint32_t PartOne::PlaceSpellObject(const skymp::rules::PlacementRow& row)
{
  const auto caster = std::dynamic_pointer_cast<MpObjectReference>(
    worldState.LookupFormByIdNoLoad(row.caster));
  if (!caster) {
    spdlog::warn("[verb:spell-placed] {:x} is gone; set {} places nothing",
                 row.caster, row.set);
    return 0;
  }
  const FormDesc baseDesc(row.short_id, std::string(row.file));
  uint32_t baseId = 0;
  try {
    baseId = baseDesc.ToFormId(worldState.espmFiles);
  } catch (std::exception& e) {
    spdlog::warn("[verb:spell-placed] base {} of set {}: {}; not placed",
                 baseDesc.ToString(), row.set, e.what());
    return 0;
  }
  // the table's word for the base's type, checked against the record when
  // the files are loaded: the validator refuses any other pair on the wire
  const std::string recordType(row.record_type);
  if (HasEspm()) {
    const auto rec = GetEspm().GetBrowser().LookupById(baseId).rec;
    if (!rec || rec->GetType().ToString() != recordType) {
      spdlog::warn("[verb:spell-placed] base {} of set {} is no {}; not "
                   "placed",
                   baseDesc.ToString(), row.set, recordType);
      return 0;
    }
  }
  // the constructor PlaceAtMe's body uses for a script's placement
  // (PapyrusObjectReference.cpp), the id the world hands out, the caster's
  // cell or world; the caster's id goes into every CreateActor the form
  // sends (onSubscribe reads placedCasterByForm), the spell into its record
  // for the sweep at load
  const uint32_t formId = worldState.GenerateFormId();
  worldState.AddForm(std::make_unique<MpObjectReference>(
                       LocationalData{ { row.x, row.y, row.z },
                                       { 0.f, 0.f, row.yaw },
                                       caster->GetCellOrWorld() },
                       CreateFormCallbacks(), baseId, recordType),
                     formId);
  auto& refr = worldState.GetFormAt<MpObjectReference>(formId);
  refr.SetPlacedBySpell(
    FormDesc(row.spell_short_id, std::string(row.spell_file)));
  pImpl->placedCasterByForm[formId] = row.caster;
  refr.ForceSubscriptionsUpdate();
  return formId;
}

void PartOne::RemoveSpellPlacedSet(uint64_t set, const char* why)
{
  const auto it = pImpl->placedSets.find(set);
  if (it == pImpl->placedSets.end()) {
    return;
  }
  size_t deleted = 0;
  for (const uint32_t formId : it->second) {
    pImpl->placedCasterByForm.erase(formId);
    const auto refr = std::dynamic_pointer_cast<MpObjectReference>(
      worldState.LookupFormByIdNoLoad(formId));
    if (refr && !refr->IsDeleted()) {
      // the existing destroy path: the record marked deleted, the form off
      // the grid, every subscriber's DestroyActor (onUnsubscribe); then the
      // reverse edges too, so no actor's listener set keeps naming a fallen
      // form (MpObjectReference.h's pairing; the T0 use-after-free of
      // 2026-10-29 when such a form was destroyed later)
      refr->Delete();
      refr->UnsubscribeFromAll();
      ++deleted;
    }
  }
  spdlog::info("[verb:spell-placed] set {} removed: {} of {} forms deleted, "
               "{}",
               set, deleted, it->second.size(), why);
  pImpl->placedSets.erase(it);
}

void PartOne::SendPlacementRequests()
{
  const auto rows = pImpl->placements->take_requests();
  for (size_t i = 0; i < rows.size();) {
    const skymp::rules::PlacementRequestRow& first = rows[i];
    const uint64_t set = first.set;
    PlacedRequestMessage ask;
    ask.set = set;
    ask.windowMs =
      static_cast<uint32_t>(std::min<uint64_t>(first.window_ms, 10'000));
    std::string names;
    for (; i < rows.size() && rows[i].set == set; ++i) {
      const FormDesc baseDesc(rows[i].short_id, std::string(rows[i].file));
      try {
        ask.bases.push_back(baseDesc.ToFormId(worldState.espmFiles));
        names += fmt::format(" {} ({})", baseDesc.ToString(),
                             std::string(rows[i].record_type));
      } catch (std::exception& e) {
        spdlog::warn("[verb:spell-placed] base {} of set {}: {}; not asked",
                     baseDesc.ToString(), set, e.what());
      }
    }
    const auto spell =
      FormDesc(first.spell_short_id, std::string(first.spell_file)).ToString();
    const auto caster = std::dynamic_pointer_cast<MpActor>(
      worldState.LookupFormByIdNoLoad(first.caster));
    const auto userId = caster ? serverState.UserByActor(caster.get())
                               : Networking::InvalidUserId;
    if (ask.bases.empty() || userId == Networking::InvalidUserId) {
      spdlog::warn("[verb:spell-placed] {:x} cast {}: set {} asks nothing "
                   "({} base(s), user {})",
                   first.caster, spell, set, ask.bases.size(),
                   static_cast<int64_t>(userId));
      continue;
    }
    GetSendTarget().Send(userId, ask, true);
    spdlog::info("[verb:spell-placed] {:x} cast {}: set {} requested, {} "
                 "base(s){} within {} ms",
                 first.caster, spell, set, ask.bases.size(), names,
                 ask.windowMs);
  }
}

bool PartOne::ReportPlacedObject(MpActor& caster, uint64_t set,
                                 uint32_t baseId, const NiPoint3& pos,
                                 float yaw)
{
  FormDesc baseDesc;
  try {
    baseDesc = FormDesc::FromFormId(baseId, worldState.espmFiles);
  } catch (std::exception& e) {
    spdlog::warn("[verb:spell-placed] E_PLACED_BASE: {:x} reports set {} "
                 "base {:x}: {}",
                 caster.GetFormId(), set, baseId, e.what());
    return false;
  }
  skymp::rules::PlacementReport report{};
  report.caster = caster.GetFormId();
  report.set = set;
  report.short_id = baseDesc.shortFormId;
  report.file = baseDesc.file;
  report.x = pos.x;
  report.y = pos.y;
  report.z = pos.z;
  report.yaw = yaw;
  const uint64_t nowMs = SteadyNowMs();
  const auto verdict = pImpl->placements->report_placed(report, nowMs);
  if (!verdict.accepted) {
    spdlog::warn("[verb:spell-placed] {}: {:x} reports set {} base {} at "
                 "({:.0f}, {:.0f}, {:.0f}) yaw {:.0f}; {:.0f} on the ground "
                 "and {:.0f} up from the cast",
                 PlacementReportCode(verdict.why), caster.GetFormId(), set,
                 baseDesc.ToString(), pos.x, pos.y, pos.z, yaw,
                 verdict.distance, verdict.dz);
    return false;
  }
  spdlog::info("[verb:spell-placed] {:x} reports set {} base {} at ({:.0f}, "
               "{:.0f}, {:.0f}) yaw {:.0f}: accepted",
               caster.GetFormId(), set, baseDesc.ToString(), pos.x, pos.y,
               pos.z, yaw);
  TickPlacements(nowMs);
  return true;
}

void PartOne::TickPlacements(uint64_t nowMs)
{
  for (const skymp::rules::PlacementRow& row :
       pImpl->placements->due_placements(nowMs)) {
    const uint32_t formId = PlaceSpellObject(row);
    if (formId) {
      pImpl->placedSets[row.set].push_back(formId);
    }
    spdlog::info(
      "[verb:spell-placed] {:x} cast {}: set {} placed {} form(s) {:x} ({}) "
      "at ({:.0f}, {:.0f}, {:.0f}) yaw {:.0f}, as reported",
      row.caster,
      FormDesc(row.spell_short_id, std::string(row.spell_file)).ToString(),
      row.set, formId ? 1 : 0, formId, std::string(row.record_type), row.x,
      row.y, row.z, row.yaw);
  }
  for (const auto& refused : pImpl->placements->take_refusals()) {
    spdlog::info(
      "[verb:spell-placed] {:x} cast {}: nothing placed, {}", refused.caster,
      FormDesc(refused.spell_short_id, std::string(refused.spell_file))
        .ToString(),
      PlacementRefusalWhy(refused.why));
  }
  for (const auto& removal : pImpl->placements->due_removals(nowMs)) {
    RemoveSpellPlacedSet(removal.set, PlacementRemovalWhy(removal.why));
  }
}

void PartOne::TickPlacementsOften()
{
  const auto now = std::chrono::steady_clock::now();
  if (now - pImpl->lastPlacementsTick < kPlacementsTick) {
    return;
  }
  pImpl->lastPlacementsTick = now;
  TickPlacements(SteadyNowMs());
}

size_t PartOne::SpellPlacedSets() const
{
  return pImpl->placements->live_sets();
}

bool PartOne::SweepSpellPlacedAtLoad(const MpChangeForm& record)
{
  if (!record.placedBySpell.has_value()) {
    return false;
  }
  uint32_t formId = 0;
  try {
    formId = record.formDesc.ToFormId(worldState.espmFiles);
  } catch (std::exception& e) {
    spdlog::warn("[verb:spell-placed] {} not swept: {}",
                 record.formDesc.ToString(), e.what());
    return false;
  }
  const auto refr = std::dynamic_pointer_cast<MpObjectReference>(
    worldState.LookupFormByIdNoLoad(formId));
  if (!refr || refr->IsDeleted()) {
    return false;
  }
  refr->Delete();
  spdlog::info("[verb:spell-placed] swept {:x} (base {}, spell {}) at load: "
               "no cast survives a restart",
               formId, record.baseDesc.ToString(),
               record.placedBySpell->ToString());
  return true;
}

void PartOne::SetSurvivalFoodSettings(const std::string& survivalFoodJson)
{
  const auto j = nlohmann::json::parse(survivalFoodJson);
  if (!j.is_object()) {
    throw std::runtime_error("survivalFood must be an object");
  }
  // a FormDesc names its plugin, or a load order change would move it
  const auto descOf = [](const nlohmann::json& text) {
    const FormDesc desc = FormDesc::FromString(text.get<std::string>());
    if (desc.file.empty()) {
      throw std::runtime_error(
        fmt::format("survivalFood: {} must name its plugin (<hex>:<plugin>)",
                    text.get<std::string>()));
    }
    return desc;
  };
  std::optional<FormDesc> switchGlobal;
  if (j.contains("switch")) {
    switchGlobal = descOf(j.at("switch"));
  }
  std::vector<SurvivalFoodEffect> effects;
  if (j.contains("effects")) {
    for (const auto& entry : j.at("effects")) {
      SurvivalFoodEffect effect;
      effect.effect = descOf(entry.at("effect"));
      effect.need = descOf(entry.at("need"));
      effect.amount = entry.at("amount").get<float>();
      effect.floor =
        entry.contains("floor") ? entry.at("floor").get<float>() : 0.f;
      if (!std::isfinite(effect.amount) || effect.amount <= 0.f ||
          !std::isfinite(effect.floor)) {
        throw std::runtime_error(
          fmt::format("survivalFood: {} needs a positive finite amount and "
                      "a finite floor",
                      effect.effect.ToString()));
      }
      effects.push_back(std::move(effect));
    }
  }
  pImpl->survivalFoodSwitch = std::move(switchGlobal);
  pImpl->survivalFood = std::move(effects);
  spdlog::info("PartOne::SetSurvivalFoodSettings - {}", survivalFoodJson);
}

std::optional<PartOne::SurvivalFoodEffect> PartOne::SurvivalFoodOf(
  uint32_t effectFormId) const
{
  for (const auto& entry : pImpl->survivalFood) {
    try {
      if (entry.effect.ToFormId(worldState.espmFiles) == effectFormId) {
        return entry;
      }
    } catch (const std::exception&) {
      // a listed effect whose plugin this load order lacks
    }
  }
  return std::nullopt;
}

std::optional<FormDesc> PartOne::SurvivalFoodSwitch() const
{
  return pImpl->survivalFoodSwitch;
}

void PartOne::SetCureSpellsSettings(const std::string& cureSpellsJson)
{
  const auto j = nlohmann::json::parse(cureSpellsJson);
  if (!j.is_array()) {
    throw std::runtime_error(
      "cureSpells: a JSON list of \"<hex id>:<plugin>\" strings was expected");
  }
  std::vector<FormDesc> list;
  for (const auto& e : j) {
    if (!e.is_string()) {
      throw std::runtime_error("cureSpells: " + e.dump() +
                               " is not \"<hex id>:<plugin>\"");
    }
    list.push_back(FormDesc::FromString(e.get<std::string>()));
  }
  pImpl->cureSpells = std::move(list);
  spdlog::info("PartOne::SetCureSpellsSettings - {}", cureSpellsJson);
}

const std::vector<FormDesc>& PartOne::CureSpells() const
{
  return pImpl->cureSpells;
}

void PartOne::SetScriptedEffectSettings(const std::string& scriptedEffectsJson)
{
  pImpl->scriptedEffects =
    skymp::rules::new_scripted_effects(scriptedEffectsJson);
  spdlog::info("PartOne::SetScriptedEffectSettings - {}", scriptedEffectsJson);
}

bool PartOne::IsScriptedKillSpell(uint32_t spellFormId) const
{
  const auto desc = FormDesc::FromFormId(spellFormId, worldState.espmFiles);
  return pImpl->scriptedEffects->is_kill_spell(desc.shortFormId, desc.file);
}

bool PartOne::ClaimScriptedKill(
  MpActor& actor, MpActor& target, uint32_t spellFormId,
  const std::vector<espm::Effects::Effect>& effects)
{
  if (target.GetChangeForm().isDead) {
    spdlog::info("PartOne::ClaimScriptedKill - {:x}'s {:x} on {:x}: the "
                 "target is dead already; nothing",
                 actor.GetFormId(), spellFormId, target.GetFormId());
    return false;
  }
  if (target.IsGodMode()) {
    spdlog::info("PartOne::ClaimScriptedKill - {:x}'s {:x} on {:x}: the "
                 "target is in god mode; nothing",
                 actor.GetFormId(), spellFormId, target.GetFormId());
    return false;
  }
  // the script runs only where its effect applies: an effect of the spell
  // whose conditions hold for the target (thuum ADR-039; Finger of Death's
  // seven, IsEssential among them)
  const bool held = std::any_of(
    effects.begin(), effects.end(), [&](const espm::Effects::Effect& effect) {
      return MpActor::EffectConditionsHold(&worldState, effect, spellFormId,
                                           target, actor);
    });
  if (!held) {
    spdlog::info("PartOne::ClaimScriptedKill - {:x}'s {:x} on {:x}: no "
                 "effect's conditions hold; nothing",
                 actor.GetFormId(), spellFormId, target.GetFormId());
    return false;
  }
  const auto desc = FormDesc::FromFormId(spellFormId, worldState.espmFiles);
  const float percentage =
    pImpl->scriptedEffects->kill_percentage(desc.shortFormId, desc.file);
  const float targetHealth =
    target.GetCurrentActorValue(espm::ActorValue::Health);
  const float casterHealth =
    actor.GetCurrentActorValue(espm::ActorValue::Health);
  const auto blast =
    skymp::rules::finger_of_death(targetHealth, casterHealth, percentage);
  // the caster's share: a player's own game runs the mod's script, which
  // damages the caster itself and reports the fall (b-fod-kill
  // 20261021-042635: the server's 64 and the script's 64 stacked to nothing
  // and the report of 0 killed the caster), so the server takes it from the
  // report as it takes every self-inflicted script damage (ADR-029's
  // default); an actor no game plays (M3's NPC casters) pays here. A god
  // caster pays nothing, as the engine's TGM takes no damage
  const bool casterGameRunsTheScript =
    serverState.UserByActor(&actor) != Networking::InvalidUserId;
  if (blast.caster_damage > 0.f && !actor.IsGodMode() &&
      !casterGameRunsTheScript) {
    actor.DamageActorValue(espm::ActorValue::Health, blast.caster_damage);
  }
  // the target's, by the caster's hand: a Health reaching nothing names
  // its killer (MpActor::SetPercentages), an immortal target keeps its one
  // point (NetSetPercentages)
  if (blast.target_damage > 0.f) {
    const float maximum = std::max(target.GetMaximumValues().health, 1.f);
    ActorValues values = target.GetChangeForm().actorValues;
    values.healthPercentage = blast.kill
      ? 0.f
      : std::max(0.f, values.healthPercentage - blast.target_damage / maximum);
    target.NetSetPercentages(
      values, &actor,
      std::vector<espm::ActorValue>{ espm::ActorValue::Health });
  }
  spdlog::info(
    "PartOne::ClaimScriptedKill - {:x}'s {:x} on {:x}: the "
    "target's {:.1f} Health against the caster's {:.1f} at {:.0f} "
    "percent: the target loses {:.1f}, the caster {:.1f}{}; {}",
    actor.GetFormId(), spellFormId, target.GetFormId(), targetHealth,
    casterHealth, percentage, blast.target_damage, blast.caster_damage,
    casterGameRunsTheScript ? " in its own game, reported" : " here",
    blast.kill ? "the target dies by the caster's hand" : "nobody dies");
  return blast.kill;
}

void PartOne::SetHeadshotSettings(const std::string& headshotJson)
{
  pImpl->headshots = skymp::rules::new_headshots(headshotJson);
  spdlog::info("PartOne::SetHeadshotSettings - {}", headshotJson);
}

bool PartOne::IsHeadshotSpell(uint32_t spellFormId) const
{
  const auto desc = FormDesc::FromFormId(spellFormId, worldState.espmFiles);
  return pImpl->headshots->is_headshot_spell(desc.shortFormId, desc.file);
}

void PartOne::NoteBowHit(uint32_t actorFormId, uint32_t targetFormId)
{
  pImpl->headshots->note_bow_hit(actorFormId, targetFormId, SteadyNowMs());
}

PartOne::HeadshotCheck PartOne::ClaimHeadshot(MpActor& actor, MpActor& target)
{
  skymp::rules::HeadshotFacts facts{};
  // the head and hair slots (ARMO BOD2 bits 0 and 1, biped slots 30 and 31:
  // CommonLibSSE-NG include/RE/B/BGSBipedObjectForm.h:13-14) of every item
  // the target's equipment record wears
  constexpr uint32_t kHeadSlots = 0x3;
  facts.head_bare = true;
  std::string worn;
  for (const auto& entry : target.GetEquipment().inv.entries) {
    if (entry.GetWorn() == Inventory::Worn::None) {
      continue;
    }
    worn += fmt::format(" {:x}", entry.baseId);
    const auto lookup =
      worldState.GetEspm().GetBrowser().LookupById(entry.baseId);
    if (!lookup.rec || lookup.rec->GetType() != espm::ARMO::kType) {
      continue;
    }
    const auto armo = espm::GetData<espm::ARMO>(entry.baseId, &worldState);
    const uint32_t flags = armo.bod2.present ? armo.bod2.bodyPartFlags
      : armo.bodt.present                    ? armo.bodt.bodyPartFlags
                                             : 0;
    worn += fmt::format("/{:x}", flags);
    if (flags & kHeadSlots) {
      facts.head_bare = false;
      break;
    }
  }
  // thuum docs/verbs/headshot.md (playtest 32, 2026-10-22): a player's
  // record carries its race in its appearance, an actor's from the game
  // files never does, and the rule read race 0 for every NPC and refused
  // each headshot on the race (E_HEADSHOT_RACE on 39fe2, a bare-headed
  // bandit). MpActor's own race read falls back through the NPC_ base's
  // template chain (UseTraits); a chain that cannot be evaluated reads 0, as
  // before
  uint32_t raceId = 0;
  try {
    raceId = target.GetRaceId();
  } catch (const std::exception& e) {
    spdlog::warn("ClaimHeadshot - race of {:x} unknown: {}",
                 target.GetFormId(), e.what());
  }
  const auto race = FormDesc::FromFormId(raceId, worldState.espmFiles);
  facts.race_short = race.shortFormId;
  facts.race_file = rust::String(race.file);
  // the shooter's base Archery (actor value 8, CommonLibSSE-NG
  // include/RE/A/ActorValues.h:16) by its record; none recorded reads 0
  facts.archery = 0.f;
  if (const auto record = actor.GetActorValueRecord()) {
    for (const auto& [av, value] : record->bases) {
      if (av == 8) {
        facts.archery = value;
      }
    }
  }
  const auto check = pImpl->headshots->claim(
    actor.GetFormId(), target.GetFormId(), facts, SteadyNowMs());
  spdlog::info("PartOne::ClaimHeadshot - {:x} on {:x}: head bare {} (worn{}), "
               "race {:x}:{}, archery {}",
               actor.GetFormId(), target.GetFormId(), facts.head_bare,
               worn.empty() ? " none" : worn, facts.race_short,
               std::string(facts.race_file), facts.archery);
  switch (check) {
    case skymp::rules::HeadshotCheck::Killed:
      // ADR-050 decision 2: a hosted NPC's headshot is a lethal cause to its
      // host (killMove), the kill deferred to the host's report
      target.KillOrDefer(&actor, true);
      return HeadshotCheck::Killed;
    case skymp::rules::HeadshotCheck::Helmeted:
      return HeadshotCheck::Helmeted;
    case skymp::rules::HeadshotCheck::Race:
      return HeadshotCheck::Race;
    case skymp::rules::HeadshotCheck::Archery:
      return HeadshotCheck::Archery;
    default:
      return HeadshotCheck::NoShot;
  }
}

float PartOne::TeleportReach(uint32_t spellFormId) const
{
  // a spell as the settings name it: its FormDesc (form id within its
  // plugin, and the plugin), so a change of load order keeps it
  const auto desc = FormDesc::FromFormId(spellFormId, worldState.espmFiles);
  return pImpl->castTeleports->teleport_reach(desc.shortFormId, desc.file);
}

uint64_t PartOne::TeleportWaitMs(uint32_t spellFormId) const
{
  const auto desc = FormDesc::FromFormId(spellFormId, worldState.espmFiles);
  return pImpl->castTeleports->teleport_wait_ms(desc.shortFormId, desc.file);
}

bool PartOne::TeleportRelays(uint32_t spellFormId) const
{
  const auto desc = FormDesc::FromFormId(spellFormId, worldState.espmFiles);
  return pImpl->castTeleports->teleport_relays(desc.shortFormId, desc.file);
}

void PartOne::PermitCastJump(uint32_t actorFormId, uint32_t cellOrWorld,
                             const NiPoint3& from, float reach,
                             uint64_t waitMs)
{
  pImpl->movementBudgets->permit_cast_jump(actorFormId, cellOrWorld, from.x,
                                           from.y, from.z, reach, waitMs,
                                           SteadyNowMs());
}

PartOne::JumpCheck PartOne::CheckCastJump(uint32_t actorFormId,
                                          uint32_t cellOrWorld,
                                          const NiPoint3& at)
{
  return ToJumpCheck(pImpl->movementBudgets->check_cast_jump(
    actorFormId, cellOrWorld, at.x, at.y, at.z, SteadyNowMs()));
}

bool PartOne::CastJumpLands(uint32_t actorFormId, uint32_t cellOrWorld,
                            const NiPoint3& at) const
{
  return pImpl->movementBudgets->cast_jump_lands(
    actorFormId, cellOrWorld, at.x, at.y, at.z, SteadyNowMs());
}

namespace {
// A form a teleport's look names, as its entry writes it, to its id in the
// server's load order, light plugins numbered as the engine numbers them
// (FormDesc::ToFormId) and the plugin compared without case, as the spell's
// is (wire-rules teleports); 0 when the look names none, or for a plugin the
// server does not load
uint32_t LookPartFormId(bool named, const skymp::rules::LookRef& ref,
                        const EspmFileList& files)
{
  if (!named) {
    return 0;
  }
  const std::string plugin(ref.plugin);
  for (const auto& file : files) {
    if (!Utils::stricmp(file.c_str(), plugin.c_str())) {
      return FormDesc(ref.id & 0x00ffffff, file).ToFormId(files);
    }
  }
  return 0;
}

// The arguments the look's snippets pass, as Skyrim Platform's dump declares
// the calls (skyrim-platform codegen FunctionsDump.txt: VisualEffect.Play
// (ObjectReference, Float, ObjectReference), VisualEffect.Stop
// (ObjectReference), Sound.Play(ObjectReference),
// ObjectReference.SetPosition (Float, Float, Float), latent); CallNative
// takes every parameter
using SnippetArgs = std::vector<std::optional<
  std::variant<bool, double, std::string, SpSnippetObjectArgument>>>;

// The caster's figure in a neighbour's game (spSnippetService resolves a
// server id to its figure)
SpSnippetObjectArgument FigureOf(const MpActor& caster)
{
  SpSnippetObjectArgument figure;
  figure.formId = caster.GetFormId();
  figure.type = "ObjectReference";
  return figure;
}

// None for an ObjectReference parameter, the facing object the mod's script
// leaves out: an object argument naming form 0, which the client's
// deserializeArg turns into null (Game.getFormEx(0) finds no form, and
// ObjectReference.from(null) is null). A JSON null would not do:
// deserializeArg reads formId off any argument whose typeof is "object"
SpSnippetObjectArgument NoReference()
{
  SpSnippetObjectArgument none;
  none.formId = 0;
  none.type = "ObjectReference";
  return none;
}
}

uint32_t PartOne::BondProcFormId(const skymp::rules::BondBreak& bond) const
{
  if (!bond.has_proc) {
    return 0;
  }
  try {
    return FormDesc(bond.proc_short_id, std::string(bond.proc_file))
      .ToFormId(worldState.espmFiles);
  } catch (std::exception& e) {
    spdlog::warn("PartOne - the bond's proc {}:{:x} is not in the load order "
                 "({}); the bond stands without its concealment",
                 std::string(bond.proc_file), bond.proc_short_id, e.what());
    return 0;
  }
}

void PartOne::ConcealBond(const skymp::rules::BondBreak& bond)
{
  const uint32_t procId = BondProcFormId(bond);
  if (!procId) {
    return;
  }
  const auto caster =
    std::dynamic_pointer_cast<MpActor>(worldState.LookupFormById(bond.caster));
  const auto target =
    std::dynamic_pointer_cast<MpActor>(worldState.LookupFormById(bond.target));
  if (!caster || !target) {
    return;
  }
  auto effects = MpActor::EffectsOf(&worldState, procId);
  if (effects.empty()) {
    spdlog::warn("PartOne::ConcealBond - proc {:x} carries no effects; the "
                 "bond stands without its concealment",
                 procId);
    return;
  }
  // the record is the bond's, not the proc's own 600 s: it ends by itself
  // with the bond, a restart included (running effects persist)
  const auto seconds = static_cast<uint32_t>(bond.duration_ms / 1000);
  for (auto& effect : effects) {
    effect.duration = seconds;
  }
  for (MpActor* actor : { caster.get(), target.get() }) {
    actor->ApplyEffects(procId, effects, 1.f, bond.caster, false);
  }
  spdlog::info("PartOne::ConcealBond - {:x} and {:x} concealed by {:x} for {} "
               "s; the casts follow in {} ms (thuum docs/verbs/shadowbond.md)",
               bond.caster, bond.target, procId, seconds,
               kBondProcCastDelayMs);
  // the cast itself comes after the relayed bond cast has run in the other
  // games (kBondProcCastDelayMs); a bond that ended meanwhile casts nothing
  const uint64_t epoch = ++pImpl->bondEpochs[bond.caster];
  worldState.SetTimer(std::chrono::milliseconds(kBondProcCastDelayMs))
    .Then([this, bond, procId, epoch](Viet::Void) {
      if (pImpl->bondEpochs[bond.caster] != epoch) {
        return;
      }
      CastBondProc(bond, procId);
    });
}

void PartOne::CastBondProc(const skymp::rules::BondBreak& bond,
                           uint32_t procId)
{
  const auto caster =
    std::dynamic_pointer_cast<MpActor>(worldState.LookupFormById(bond.caster));
  const auto target =
    std::dynamic_pointer_cast<MpActor>(worldState.LookupFormById(bond.target));
  if (!caster || !target) {
    return;
  }
  // the proc as a cast relay (SpellCast, the message every resolved cast
  // travels on): each bonded actor casts the proc on itself, through its
  // instant caster (castingSource 3), in every game that sees it but the
  // caster's own, where the mod's script cast both. An observer's game casts
  // it on the actor's figure through castSpellImmediate's Self branch, the
  // primitive b-invis-recast proves nightly; the target's own game reads a
  // caster that is its own player as 0x14 (remoteServer.ts). A Papyrus
  // Spell.Cast snippet completed in the target's game and applied nothing
  // (b-shadowbond 20261019-215816, playtest twenty-five), as the driver's
  // own Spell.Cast never applied anything either (push.md); no cast of the
  // proc comes back to the server (Skyrim Platform reports only a cast of an
  // equipped spell), so it is never an act against the bond
  int games = 0;
  for (MpActor* actor : { caster.get(), target.get() }) {
    SpellCastMessage cast;
    cast.data.caster = actor->GetFormId();
    cast.data.target = actor->GetFormId();
    cast.data.spell = procId;
    cast.data.castingSource = 3; // MagicSystem::CastingSource::kInstant
    for (auto* listener : actor->GetActorListeners()) {
      if (listener == caster.get() ||
          serverState.UserByActor(listener) == Networking::InvalidUserId) {
        continue;
      }
      listener->GetActorToSendTo().SendToUser(cast, true);
      ++games;
    }
  }
  spdlog::info("PartOne::CastBondProc - {:x} relayed as {:x}'s and {:x}'s own "
               "instant cast to {} game(s) (thuum docs/verbs/shadowbond.md)",
               procId, bond.caster, bond.target, games);
}

void PartOne::UnconcealBond(const skymp::rules::BondBreak& bond,
                            const char* why)
{
  // a proc cast still on its timer casts nothing after this
  ++pImpl->bondEpochs[bond.caster];
  const uint32_t procId = BondProcFormId(bond);
  if (!procId) {
    return;
  }
  int ended = 0;
  int games = 0;
  for (uint32_t formId : { bond.caster, bond.target }) {
    const auto actor =
      std::dynamic_pointer_cast<MpActor>(worldState.LookupFormById(formId));
    if (!actor) {
      continue;
    }
    // by source, so a player's own Invisibility spell is spared
    ended += actor->EndRunningEffectsOfSource(procId) ? 1 : 0;
    // the target's own copy was the server's cast, so the server dispels it;
    // the caster's is the script's, which dispels its own at the swap or at
    // its end; the figures' copies follow the records (dispelEnded)
    if (formId == bond.target &&
        serverState.UserByActor(actor.get()) != Networking::InvalidUserId) {
      SpSnippetObjectArgument spell;
      spell.formId = procId;
      spell.type = "Spell";
      const SnippetArgs dispel{ spell };
      SpSnippet("Actor", "DispelSpell", dispel, actor->GetFormId())
        .Execute(actor.get(), SpSnippetMode::kNoReturnResult);
      ++games;
    }
  }
  spdlog::info("PartOne::UnconcealBond - {:x} and {:x}'s concealment by {:x} "
               "ends ({}): {} record(s) ended, dispelled in {} game(s)",
               bond.caster, bond.target, procId, why, ended, games);
}

void PartOne::TickBonds(uint64_t nowMs)
{
  for (const auto& lapsed : pImpl->bonds->expired(nowMs)) {
    spdlog::info("PartOne::TickBonds - {:x}'s bond with {:x} ran out (thuum "
                 "docs/verbs/shadowbond.md)",
                 lapsed.caster, lapsed.target);
    UnconcealBond(lapsed, "the bond ran out");
  }
}

void PartOne::TickBondsEverySecond()
{
  const auto now = std::chrono::steady_clock::now();
  if (now - pImpl->lastBondsTick < std::chrono::seconds(1)) {
    return;
  }
  pImpl->lastBondsTick = now;
  TickBonds(SteadyNowMs());
}

std::optional<PartOne::TeleportLook> PartOne::GetTeleportLook(
  uint32_t spellFormId) const
{
  const auto desc = FormDesc::FromFormId(spellFormId, worldState.espmFiles);
  const auto spec =
    pImpl->castTeleports->teleport_look(desc.shortFormId, desc.file);
  if (!spec.has_vanish && !spec.has_appear && !spec.has_sound) {
    return std::nullopt;
  }
  const auto& files = worldState.espmFiles;
  TeleportLook look;
  look.vanish = LookPartFormId(spec.has_vanish, spec.vanish, files);
  look.appear = LookPartFormId(spec.has_appear, spec.appear, files);
  look.appearSeconds = spec.appear_seconds;
  look.sound = LookPartFormId(spec.has_sound, spec.sound, files);
  return look;
}

void PartOne::PlayTeleportVanish(MpActor& caster, uint32_t spellFormId,
                                 uint64_t waitMs)
{
  const auto look = GetTeleportLook(spellFormId);
  if (!look) {
    pImpl->teleportLooks.erase(caster.GetFormId());
    return;
  }
  const uint64_t now = SteadyNowMs();
  const uint64_t untilMs = waitMs > std::numeric_limits<uint64_t>::max() - now
    ? std::numeric_limits<uint64_t>::max()
    : now + waitMs;
  pImpl->teleportLooks[caster.GetFormId()] = { *look, untilMs };
  if (!look->vanish) {
    return;
  }

  const double waitSeconds = static_cast<double>(waitMs) / 1000.0;
  const SnippetArgs play{ FigureOf(caster), waitSeconds, NoReference() };
  int neighbours = 0;
  for (auto* listener : caster.GetActorListeners()) {
    if (listener == &caster ||
        serverState.UserByActor(listener) == Networking::InvalidUserId) {
      continue;
    }
    SpSnippet("VisualEffect", "Play", play, look->vanish)
      .Execute(listener, SpSnippetMode::kNoReturnResult);
    ++neighbours;
  }
  spdlog::info("PartOne::PlayTeleportVanish - {:x}'s teleport look at the "
               "cast: vanish {:x} for {:.1f} s in {} neighbour(s)' games",
               caster.GetFormId(), look->vanish, waitSeconds, neighbours);
}

void PartOne::PlayTeleportLanding(MpActor& caster, const NiPoint3& at)
{
  const auto it = pImpl->teleportLooks.find(caster.GetFormId());
  if (it == pImpl->teleportLooks.end()) {
    return;
  }
  const auto kept = it->second;
  pImpl->teleportLooks.erase(it);
  if (SteadyNowMs() > kept.untilMs) {
    return;
  }
  const auto& look = kept.look;

  const SnippetArgs place{ static_cast<double>(at.x),
                           static_cast<double>(at.y),
                           static_cast<double>(at.z) };
  const SnippetArgs onFigure{ FigureOf(caster) };
  const SnippetArgs appear{ FigureOf(caster),
                            static_cast<double>(look.appearSeconds),
                            NoReference() };
  int neighbours = 0;
  for (auto* listener : caster.GetActorListeners()) {
    if (listener == &caster ||
        serverState.UserByActor(listener) == Networking::InvalidUserId) {
      continue;
    }
    // the pop first: a near teleport's figure would glide there in 0.2 s
    // (skymp5-client sync/movementApply.ts translateTo)
    SpSnippet("ObjectReference", "SetPosition", place, caster.GetFormId())
      .Execute(listener, SpSnippetMode::kNoReturnResult);
    if (look.vanish) {
      SpSnippet("VisualEffect", "Stop", onFigure, look.vanish)
        .Execute(listener, SpSnippetMode::kNoReturnResult);
    }
    if (look.appear) {
      SpSnippet("VisualEffect", "Play", appear, look.appear)
        .Execute(listener, SpSnippetMode::kNoReturnResult);
    }
    if (look.sound) {
      SpSnippet("Sound", "Play", onFigure, look.sound)
        .Execute(listener, SpSnippetMode::kNoReturnResult);
    }
    ++neighbours;
  }
  spdlog::info("PartOne::PlayTeleportLanding - {:x}'s teleport look at the "
               "landing ({:.0f}, {:.0f}, {:.0f}): placed, vanish {:x} "
               "stopped, appear {:x} for {:.2f} s, sound {:x}, in {} "
               "neighbour(s)' games",
               caster.GetFormId(), at.x, at.y, at.z, look.vanish, look.appear,
               look.appearSeconds, look.sound, neighbours);
}

bool PartOne::TeleportLookLands(float step)
{
  return skymp::rules::teleport_look_lands(step);
}

void PartOne::RecordShot(uint32_t actorFormId, uint32_t weapon, float power,
                         const NiPoint3& from)
{
  skymp::rules::ShotFacts shot{};
  shot.weapon = weapon;
  shot.power = power;
  shot.x = from.x;
  shot.y = from.y;
  shot.z = from.z;
  pImpl->rangedShots->record(actorFormId, shot, SteadyNowMs());
}

namespace {
// thuum docs/verbs/spell-cast.md: what the records say of a cast: the
// spell's casting type (SPEL SPIT), whether any of its effects has an area,
// and its reach, the longest range among its effects' projectiles (MGEF DATA
// 0x48, PROJ DATA 0x0C). A spell that launches none (a touch, a self spell)
// reaches kTouchReach units, a chosen bound, unmeasured: in the five masters
// every hostile spell delivered by touch or at a target actor is, by its
// editor id, a creature's attack, a trap's, a perk's, an enchantment's or a
// daedra banishing's, none cast from a player's hand (thuum lab scan,
// 2026-10-10), so the bound is for mods'
constexpr float kTouchReach = 512.f;
// thuum docs/verbs/spell-cast.md (train-27): how long a placed trigger's
// cast (a rune) may claim the hit that trips it, milliseconds. The records
// give no lifetime (PROJ Lifetime is 0 on the runes), so the engine's own
// persistence is HYPOTHESIS; ten minutes bounds a rune a player walks into
// late without keeping casts forever
constexpr uint64_t kHazardWindowMs = 600'000;

// thuum docs/verbs/second-hand.md: the spells a cast of `spell` can make,
// read from the records, as global ids: a Cloak effect's associated spell
// (MGEF DATA 0x08) and every spell an Object property of a script on one of
// its effects names (VMAD), then the same for those, to kReachDepth; at most
// the bridge's eight. An id inside a record counts against that file's
// masters, so each is mapped through the record that names it
constexpr int kReachDepth = 3;

std::vector<uint32_t> ReachedSpells(WorldState& worldState, uint32_t spell)
{
  std::vector<uint32_t> reached;
  std::vector<uint32_t> frontier{ spell };
  std::vector<uint32_t> seen{ spell };
  auto& browser = worldState.GetEspm().GetBrowser();
  auto& cache = worldState.GetEspmCache();
  constexpr size_t kMaxReached = 8;
  for (int depth = 0; depth < kReachDepth && !frontier.empty(); ++depth) {
    std::vector<uint32_t> next;
    for (const uint32_t from : frontier) {
      const auto spellLookup = browser.LookupById(from);
      if (!spellLookup.rec ||
          spellLookup.rec->GetType() != espm::SPEL::kType) {
        continue;
      }
      const auto data = espm::GetData<espm::SPEL>(from, &worldState);
      for (const auto& effect : data.effects) {
        if (effect.effectFormId == 0) {
          continue;
        }
        const uint32_t mgefId = spellLookup.ToGlobalId(effect.effectFormId);
        const auto mgefLookup = browser.LookupById(mgefId);
        if (!mgefLookup.rec) {
          continue;
        }
        std::vector<uint32_t> named;
        const auto mgef = espm::GetData<espm::MGEF>(mgefId, &worldState);
        if (mgef.data.effectType == espm::MGEF::EffectType::Cloak &&
            mgef.data.associatedItem != 0) {
          named.push_back(mgefLookup.ToGlobalId(mgef.data.associatedItem));
        }
        espm::ScriptData scriptData;
        mgefLookup.rec->GetScriptData(&scriptData, cache);
        for (const auto& script : scriptData.scripts) {
          for (const auto& property : script.properties) {
            if (property.type == espm::Property::Type::Object &&
                property.value.formId != 0) {
              named.push_back(mgefLookup.ToGlobalId(property.value.formId));
            }
          }
        }
        for (const uint32_t id : named) {
          if (std::find(seen.begin(), seen.end(), id) != seen.end()) {
            continue;
          }
          seen.push_back(id);
          const auto lookup = browser.LookupById(id);
          if (!lookup.rec || lookup.rec->GetType() != espm::SPEL::kType) {
            continue;
          }
          if (reached.size() >= kMaxReached) {
            return reached;
          }
          reached.push_back(id);
          next.push_back(id);
        }
      }
    }
    frontier = std::move(next);
  }
  return reached;
}

skymp::rules::CastFacts CastFactsFor(WorldState& worldState, uint32_t spell,
                                     uint8_t hand, const NiPoint3& from,
                                     bool dual)
{
  skymp::rules::CastFacts facts{};
  facts.dual = dual;
  facts.spell = spell;
  facts.hand = hand;
  facts.kind = skymp::rules::CastKind::FireAndForget;
  facts.area = false;
  facts.reach = kTouchReach;
  facts.x = from.x;
  facts.y = from.y;
  facts.z = from.z;
  try {
    const auto data = espm::GetData<espm::SPEL>(spell, &worldState);
    if (data.spellItem &&
        data.spellItem->castType == espm::SPEL::CastType::Concentration) {
      facts.kind = skymp::rules::CastKind::Concentration;
    }
    // an id inside a record counts against that file's masters, so a
    // plugin's own effect and projectile are found through the record that
    // names them (thuum docs/verbs/spell-cast.md, playtest fourteen: Lost
    // Grimoire's effect 05xxxxxx in its file is 0B in the load order)
    auto& browser = worldState.GetEspm().GetBrowser();
    const auto spellLookup = browser.LookupById(spell);
    float reach = 0.f;
    uint32_t longestS = 0;
    // thuum docs/verbs/spell-cast.md (train-27): a spell whose every effect
    // is delivered to its target, by touch or at a target actor (MGEF
    // delivery 1 or 3, libespm MGEF::DATA::delivery), lands where no aim
    // decides; its relay waits for its first hit, which names the target
    bool landsOnTarget = !data.effects.empty();
    for (const auto& effect : data.effects) {
      if (effect.effectItem && effect.effectItem->areaOfEffect > 0) {
        facts.area = true;
      }
      if (effect.effectItem) {
        longestS = std::max(longestS, effect.effectItem->duration);
      }
      if (effect.effectFormId == 0) {
        continue;
      }
      const uint32_t mgefId = spellLookup.ToGlobalId(effect.effectFormId);
      const auto mgef = espm::GetData<espm::MGEF>(mgefId, &worldState);
      if (mgef.data.delivery != 1 && mgef.data.delivery != 3) {
        landsOnTarget = false;
      }
      if (mgef.data.projectile == 0) {
        continue;
      }
      const auto proj = espm::GetData<espm::PROJ>(
        browser.LookupById(mgefId).ToGlobalId(mgef.data.projectile),
        &worldState);
      reach = std::max(reach, proj.range);
      // thuum docs/verbs/spell-cast.md (train-27): a rune's projectile is a
      // placed trigger whose hit comes when an actor walks into it, minutes
      // after the cast (playtest twenty-three: the rune's hits refused as
      // E_SPELL_NO_CAST once the cast reached the server at all), so its
      // cast's claim lasts the hazard window instead of the bolt window
      if (proj.IsPlacedTrigger()) {
        facts.hazard_ms = kHazardWindowMs;
      }
    }
    if (reach > 0.f) {
      facts.reach = reach;
    }
    facts.lands_on_target = landsOnTarget;
    // thuum docs/verbs/second-hand.md: what the cast's records reach, and
    // for how long their hits may claim it (the wire-rules floor is the
    // bolt window; a concentration cast's open stream extends it)
    facts.linger_ms = static_cast<uint64_t>(longestS) * 1000;
    // and which of them are concentration spells, counted by their time
    // (a cloak's applied spell is one: Skyrim.esm's FlameCloakDmg 0x2B385)
    const auto reached = ReachedSpells(worldState, spell);
    for (size_t i = 0; i < reached.size() && i < facts.reached.size(); ++i) {
      facts.reached[i] = reached[i];
      const auto reachedData =
        espm::GetData<espm::SPEL>(reached[i], &worldState);
      if (reachedData.spellItem &&
          reachedData.spellItem->castType ==
            espm::SPEL::CastType::Concentration) {
        facts.reached_concentration |= static_cast<uint8_t>(1u << i);
      }
    }
  } catch (std::exception& e) {
    spdlog::warn("CastFactsFor - {:x}: {}; cast as fire-and-forget within {} "
                 "units",
                 spell, e.what(), kTouchReach);
  }
  return facts;
}
}

bool PartOne::StartCast(uint32_t actorFormId, uint32_t spell, uint8_t hand,
                        const NiPoint3& from, bool dual)
{
  const auto facts = CastFactsFor(worldState, spell, hand, from, dual);
  const uint64_t nowMs = SteadyNowMs();
  // thuum docs/verbs/spell-placed-object.md (ADR-057): a listed spell's
  // cast waits for its placement from where the caster stands and faces
  // (the recorded angle's Z, the yaw the script read)
  FormDesc spellDesc;
  try {
    spellDesc = FormDesc::FromFormId(spell, worldState.espmFiles);
  } catch (std::exception&) {
    // a spell of no loaded plugin names nothing in the table; the cast
    // itself is judged below as before
  }
  if (pImpl->placements->is_listed(spellDesc.shortFormId, spellDesc.file)) {
    const auto caster = std::dynamic_pointer_cast<MpObjectReference>(
      worldState.LookupFormByIdNoLoad(actorFormId));
    skymp::rules::PlacedCast cast{};
    cast.caster = actorFormId;
    cast.hand = hand;
    cast.x = from.x;
    cast.y = from.y;
    cast.z = from.z;
    cast.yaw = caster ? caster->GetAngle().z : 0.f;
    cast.concentration = facts.kind == skymp::rules::CastKind::Concentration;
    pImpl->placements->cast_started(spellDesc.shortFormId, spellDesc.file,
                                    cast, nowMs);
    SendPlacementRequests();
  }
  return pImpl->spellCasts->start(actorFormId, facts, nowMs);
}

void PartOne::EndCast(uint32_t actorFormId, uint8_t hand)
{
  pImpl->spellCasts->end(actorFormId, hand);
  pImpl->placements->cast_ended(actorFormId, hand, SteadyNowMs());
}

std::optional<uint32_t> PartOne::AbilityReaching(const MpActor& actor,
                                                 uint32_t spell)
{
  if (!HasEspm() || spell == 0) {
    return std::nullopt;
  }
  for (const uint32_t ability : actor.KnownBaseSpells()) {
    try {
      const auto data = espm::GetData<espm::SPEL>(ability, &worldState);
      if (!data.spellItem ||
          data.spellItem->castType != espm::SPEL::CastType::ConstantEffect) {
        continue;
      }
      const auto reached = ReachedSpells(worldState, ability);
      if (std::find(reached.begin(), reached.end(), spell) != reached.end()) {
        return ability;
      }
    } catch (std::exception& e) {
      spdlog::warn("PartOne::AbilityReaching - {:x} of {:x}: {}", ability,
                   actor.GetFormId(), e.what());
    }
  }
  return std::nullopt;
}

PartOne::CastClaim PartOne::ClaimCast(uint32_t actorFormId, uint32_t spell,
                                      uint32_t target, const NiPoint3& at)
{
  skymp::rules::CastHit hit{};
  hit.spell = spell;
  hit.target = target;
  hit.x = at.x;
  hit.y = at.y;
  hit.z = at.z;
  const auto claim = pImpl->spellCasts->claim(actorFormId, hit, SteadyNowMs());
  CastClaim res;
  res.scale = claim.scale;
  res.relay = claim.relay;
  res.dual = claim.dual;
  switch (claim.check) {
    case skymp::rules::CastCheck::Claimed:
      res.check = CastCheck::Claimed;
      break;
    case skymp::rules::CastCheck::TooFar:
      res.check = CastCheck::TooFar;
      break;
    case skymp::rules::CastCheck::Throttled:
      res.check = CastCheck::Throttled;
      break;
    default:
      res.check = CastCheck::NoCast;
      break;
  }
  return res;
}

PartOne::CastClaim PartOne::ClaimReached(uint32_t actorFormId, uint32_t spell,
                                         uint32_t target, const NiPoint3& at,
                                         const NiPoint3& casterAt)
{
  skymp::rules::CastHit hit{};
  hit.spell = spell;
  hit.target = target;
  hit.x = at.x;
  hit.y = at.y;
  hit.z = at.z;
  const auto claim = pImpl->spellCasts->claim_reached(
    actorFormId, hit, casterAt.x, casterAt.y, casterAt.z, SteadyNowMs());
  CastClaim res;
  res.scale = claim.scale;
  switch (claim.check) {
    case skymp::rules::CastCheck::Claimed:
      res.check = CastCheck::Claimed;
      break;
    case skymp::rules::CastCheck::TooFar:
      res.check = CastCheck::TooFar;
      break;
    case skymp::rules::CastCheck::Throttled:
      res.check = CastCheck::Throttled;
      break;
    default:
      res.check = CastCheck::NoCast;
      break;
  }
  return res;
}

PartOne::VoiceCheck PartOne::Shout(uint32_t actorFormId,
                                   const VoiceFacts& facts)
{
  skymp::rules::VoiceFacts f{};
  f.shout_equipped = facts.shoutEquipped;
  f.variation = facts.variation;
  f.words_unlocked = facts.wordsUnlocked;
  f.recovery_s = facts.recoverySeconds;
  f.recovery_mult = facts.recoveryMult;
  switch (pImpl->voices->shout(actorFormId, f, SteadyNowMs())) {
    case skymp::rules::VoiceRefusal::Accepted:
      return VoiceCheck::Accepted;
    case skymp::rules::VoiceRefusal::NotAVariation:
      return VoiceCheck::NotAVariation;
    case skymp::rules::VoiceRefusal::WordLocked:
      return VoiceCheck::WordLocked;
    case skymp::rules::VoiceRefusal::Recovering:
      return VoiceCheck::Recovering;
    default:
      return VoiceCheck::NoShout;
  }
}

uint64_t PartOne::VoiceRecoveryLeftMs(uint32_t actorFormId) const
{
  return pImpl->voices->recovery_left_ms(actorFormId, SteadyNowMs());
}

PartOne::ShotClaim PartOne::ClaimShot(uint32_t actorFormId, uint32_t weapon,
                                      const NiPoint3& target)
{
  const auto claim = pImpl->rangedShots->claim(
    actorFormId, weapon, target.x, target.y, target.z, SteadyNowMs());
  ShotClaim res;
  res.power = claim.power;
  switch (claim.check) {
    case skymp::rules::ShotCheck::Claimed:
      res.check = ShotCheck::Claimed;
      break;
    case skymp::rules::ShotCheck::TooFar:
      res.check = ShotCheck::TooFar;
      break;
    default:
      res.check = ShotCheck::NoShot;
      break;
  }
  return res;
}
