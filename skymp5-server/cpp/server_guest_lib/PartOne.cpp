#include "PartOne.h"
#include "wire_bridge_cxx/rules.h"
#include <array>
#include <cassert>
#include <chrono>
#include <string>
#include <vector>

#include "CreateActorMessage.h"
#include "CustomPacketMessage.h"
#include "DestroyActorMessage.h"
#include "HostStopMessage.h"
#include "RaceMenuPresetMessage.h"
#include "SetGameTimeMessage.h"
#include "SetRaceMenuOpenMessage.h"
#include "UpdateGameModeDataMessage.h"

#include "ActionListener.h"
#include "FormCallbacks.h"
#include "MessageSerializerFactory.h"
#include "OpenSSLSigner.h"
#include "PacketParser.h"
#include "SpSnippet.h"
#include "libespm/CELL.h"
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

  // every player actor's ground speed budget (thuum
  // docs/verbs/movement-speed.md; the rule is Rust's, ADR-020)
  rust::Box<skymp::rules::MovementBudgets> movementBudgets =
    skymp::rules::new_movement_budgets();

  // the server's game clock (thuum docs/verbs/time.md; Rust's, ADR-020 and
  // ADR-021); players hear it once SetGameTimeSettings has run
  rust::Box<skymp::rules::GameClock> gameClock =
    skymp::rules::new_game_clock("{}");
  bool gameTimeBroadcast = false;

  // TES3MP's rest switches (thuum docs/verbs/rest.md): server-settings.json's
  // `rest` block, both on unless a server turns one off
  rust::Box<skymp::rules::RestSettings> restSettings =
    skymp::rules::new_rest_settings("{}");

  // the fights between players (thuum docs/verbs/hostility-sync.md, ADR-023)
  rust::Box<skymp::rules::Fights> fights = skymp::rules::new_fights();
  std::chrono::steady_clock::time_point lastFightsTick;

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

    // Clear actor's hoster if any.
    // HostStop message will be sent on the next attempt to update actor's
    // movement
    // Possible fix for "players link to each other" bug
    // See also ActionListener::SendToNeighbours
    auto hosterActorIt = worldState.hosters.find(actor.GetFormId());
    if (hosterActorIt != worldState.hosters.end()) {
      worldState.hosters.erase(hosterActorIt);
    }

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
        }
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

void PartOne::SendHostStop(Networking::UserId badHosterUserId,
                           MpObjectReference& remote)
{
  auto remoteAsActor = remote.AsActor();

  uint64_t longFormId = remote.GetFormId();
  if (remoteAsActor && longFormId < 0xff000000) {
    longFormId += 0x100000000;
  }

  HostStopMessage message;
  message.target = longFormId;
  GetSendTarget().Send(badHosterUserId, message, true);
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

    uint64_t longFormId = emitter->GetFormId();
    if (emitterAsActor && longFormId < 0xff000000) {
      longFormId += 0x100000000;
    }
    message.refrId = longFormId;

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
          //  From docs: isVisibleByNeighbors is considered to be always false
          //  for properties with `isVisibleByOwner == false`, in that case,
          //  actual flag value is ignored.
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
    auto hosterIterator = worldState.hosters.find(emitter->GetFormId());

    if (hasUser ||
        (hosterIterator != worldState.hosters.end() &&
         hosterIterator->second != 0 &&
         hosterIterator->second != listener->GetFormId())) {
      message.props.isHostedByOther = true;
    }

    uint32_t worldOrCell =
      emitter->GetCellOrWorld().ToFormId(worldState.espmFiles);

    // See 'perf: improve game framerate #1186'
    // Client needs to know if it is DOOR or not
    if (const std::string& baseType = emitter->GetBaseType();
        baseType == "DOOR") {
      message.baseRecordType = "DOOR";
    }

    message.idx = emitter->GetIdx();
    message.isMe = isMe;
    message.transform.pos = { emitterPos.x, emitterPos.y, emitterPos.z };
    message.transform.rot = { emitterRot.x, emitterRot.y, emitterRot.z };
    message.transform.worldOrCell = worldOrCell;

    sendTarget->Send(listenerUserId, message, true);

    // thuum docs/verbs/racemenu-sync.md: a player's RaceMenu look comes with
    // its figure, after CreateActor on the same ordered channel; its own
    // client gets it after its login instead (ActionListener)
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
      spdlog::error(
        "Expected packetHistoryStartTime to present, probably incorrect code");
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
  // a player of a pair, while a user plays it
  const auto player = [&](uint32_t formId) -> MpActor* {
    const auto& form = worldState.LookupFormById(formId);
    MpActor* actor = form ? form->AsActor() : nullptr;
    return actor && serverState.UserByActor(actor) != Networking::InvalidUserId
      ? actor
      : nullptr;
  };
  for (const auto& pair : pImpl->fights->pairs()) {
    MpActor* a = player(pair.a);
    MpActor* b = player(pair.b);
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
      StopCombatOnClient(*a, pair.b);
      StopCombatOnClient(*b, pair.a);
      spdlog::info("PartOne::TickFights - hostility: the fight between {:x} "
                   "and {:x} is over ({}); both games are told",
                   pair.a, pair.b, apart ? "apart" : "a minute quiet");
    }
  }
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

PartOne::JumpCheck PartOne::CheckJump(uint32_t actorFormId,
                                      uint32_t cellOrWorld, float x, float y)
{
  switch (pImpl->movementBudgets->check_jump(actorFormId, cellOrWorld, x, y,
                                             SteadyNowMs())) {
    case skymp::rules::JumpCheck::Landed:
      return JumpCheck::Landed;
    case skymp::rules::JumpCheck::Waiting:
      return JumpCheck::Waiting;
    default:
      return JumpCheck::NoPermit;
  }
}
