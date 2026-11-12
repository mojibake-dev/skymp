#include "PartOne.h"
#include "ActorFateMessage.h"
#include "LocationalData.h"
#include "libespm/Property.h"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "CreateActorMessage.h"
#include "CustomPacketMessage.h"
#include "DestroyActorMessage.h"
#include "DestructionStageMessage.h"
#include "HostGrantMessage.h"
#include "HostReleaseMessage.h"
#include "PerkStateMessage.h"
#include "PlacedRequestMessage.h"
#include "PropOwnerMessage.h"
#include "RaceMenuPresetMessage.h"
#include "ScriptStateApplyMessage.h"
#include "SetGameTimeMessage.h"
#include "SetRaceMenuOpenMessage.h"
#include "SetWeatherMessage.h"
#include "SpellCastMessage.h"
#include "UpdateGameModeDataMessage.h"
#include "UpdateMovementMessage.h"
#include "WorldGlobalMessage.h"

#include "ActionListener.h"
#include "FormCallbacks.h"
#include "LeveledListUtils.h"
#include "MessageSerializerFactory.h"
#include "OpenSSLSigner.h"
#include "PacketParser.h"
#include "SpSnippet.h"
#include "libespm/ARMO.h"
#include "libespm/AVIF.h"
#include "libespm/CELL.h"
#include "libespm/FLST.h"
#include "libespm/HDPT.h"
#include "libespm/MGEF.h"
#include "libespm/PROJ.h"
#include "libespm/QUST.h"
#include "libespm/SMQN.h"
#include "libespm/SPEL.h"
#include "libespm/ScriptData.h"
#include "papyrus-vm/Utils.h"
#include "script_classes/PapyrusObjectReference.h"
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
constexpr size_t kFateInvMax = 32; // wire-schema cap::FATE_INV

ActorFateMessage::Fate FateOf(const WorldState& worldState,
                              const MpActor& actor)
{
  ActorFateMessage::Fate fate;
  fate.refrId = actor.GetFormId();
  fate.baseId = actor.GetFateBaseId();
  fate.level = actor.GetFateLevel().value_or(0);
  // thuum docs/verbs/npc-fate.md (playtest 48, the loot strip): a worn piece
  // is named only while the inventory still holds it. Since playtest 48 a
  // take drops the worn entry it empties (MpActor::DropWornBeyondInventory);
  // a record saved before keeps one (Embershard's bandit b6fdb in 48's world:
  // the shield 12eb6 worn in its record by the host's report accepted at
  // 20:23:28Z, gone from its inventory after its opens at 20:27Z, the next
  // host's report of it refused at 20:40:43Z as "inventory does not
  // contain"), and a seat's reconcile would add it back and dress it
  std::map<uint32_t, uint32_t> unnamed;
  for (const auto& entry : actor.GetInventory().entries) {
    if (entry.baseId != 0) {
      unnamed[entry.baseId] += entry.count;
    }
  }
  for (const auto& entry : actor.GetEquipment().inv.entries) {
    if (entry.GetWorn() != Inventory::Worn::None &&
        fate.worn.size() < kFateWornMax) {
      auto& left = unnamed[entry.baseId];
      if (left == 0) {
        spdlog::debug("PartOne - npc-fate: {:x}'s worn {:x} left out of its "
                      "fate, its inventory holds none",
                      actor.GetFormId(), entry.baseId);
        continue;
      }
      --left;
      fate.worn.push_back(entry.baseId);
    }
  }
  // thuum docs/verbs/npc-fate.md (playtest 44's 1c): the record's other rows,
  // the server's own container roll (EnsureBaseContainerAdded), so every
  // seat's copy carries them instead of its engine's roll. The worn set lives
  // in the Equipment record alone (the inventory's entries carry no worn flag
  // of their own: the review of 7c3c9f79, finding 1), so a base's row is its
  // inventory count less its worn count, and a base whose whole count is
  // worn makes no row (the validator refuses a count of 0)
  std::map<uint32_t, int64_t> rows;
  for (const auto& entry : actor.GetInventory().entries) {
    if (entry.baseId != 0) {
      rows[entry.baseId] += entry.count;
    }
  }
  for (const auto& entry : actor.GetEquipment().inv.entries) {
    if (entry.GetWorn() != Inventory::Worn::None && entry.baseId != 0) {
      rows[entry.baseId] -= entry.count;
    }
  }
  for (const auto& [baseId, net] : rows) {
    if (net <= 0) {
      continue;
    }
    if (fate.inv.size() >= kFateInvMax) {
      spdlog::debug("PartOne - npc-fate: {:x}'s fate carries {} of its {} "
                    "inventory rows (the wire's cap)",
                    actor.GetFormId(), kFateInvMax, rows.size());
      break;
    }
    ActorFateMessage::InvRow row;
    row.baseId = baseId;
    row.count = static_cast<uint32_t>(net);
    fate.inv.push_back(row);
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

  // the server's weather (thuum docs/verbs/weather.md; Rust's, wire-rules
  // weather): one per worldspace on the game clock; players hear it once
  // weatherEnabled is set. The settings' pools wait for the load order when
  // they name form descriptors (weatherPoolsApplied false until then)
  rust::Box<skymp::rules::WeatherSync> weatherSync =
    skymp::rules::new_weather_sync(120, 0);
  bool weatherEnabled = false;
  nlohmann::json weatherPools = nlohmann::json::object();
  bool weatherPoolsApplied = true;
  std::chrono::steady_clock::time_point lastWeatherTick{};

  // the perks' rule (thuum docs/verbs/perks.md; Rust's, wire-rules perks):
  // every PERK of the load order with its own conditions, read at AttachEspm
  rust::Box<skymp::rules::PerkTable> perkTable =
    skymp::rules::new_perk_table();
  bool perksEnabled = false;

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
  // the retired `allowed` of the mode block (thuum ADR-068 rung 5, read 4),
  // kept for the quest scope's measurement line alone
  std::vector<FormDesc> retiredAllowed;

  // thuum ADR-068 rung 5: the run record's churn counter and held quests
  // (wire-rules quest_runs, runtime only), the quest scope's settings
  // (questScope, playerQuests), the condition-read variables
  // (conditionVariables) and every player's race overlay (player_own)
  rust::Box<skymp::rules::QuestRuns> questRuns =
    skymp::rules::new_quest_runs();
  // D-Q4 C: `questStoryInstances` "world" (false: "seat", the default), the
  // Story Manager's quests composed into questRuns at AttachEspm
  bool questStoryWorld = false;
  // D-Q9 B (train 52): `questPlayerAliasCause` "world" (false: "seat", the
  // default, Eli's B), and the game files' alias scripts, indexed at
  // AttachEspm: whether each rides player aliases alone
  bool questPlayerAliasWorld = false;
  const std::unordered_map<std::string, bool>* playerAliasScripts = nullptr;
  rust::Box<skymp::rules::QuestScope> questScope =
    skymp::rules::new_quest_scope("\"off\"", "[]");
  rust::Box<skymp::rules::ScriptVariables> scriptVariables =
    skymp::rules::new_script_variables("[]");
  rust::Box<skymp::rules::PlayerOverlays> playerOverlays =
    skymp::rules::new_player_overlays();
  // the playerGlobals setting as written, recomposed with the player class's
  // globals (playerQuestGlobals) while the scope acts
  std::string playerGlobalsJson = "[]";
  // the census's per-quest companions, parsed (each key a quest)
  std::vector<std::pair<FormDesc, std::vector<FormDesc>>> playerQuestGlobals;
  std::vector<
    std::pair<FormDesc, std::vector<std::pair<std::string, std::string>>>>
    playerQuestVariables;
  // (playerQuestCallers is keyed by the world quest whose stages it names)
  std::vector<std::pair<FormDesc, std::vector<uint16_t>>> playerQuestCallers;
  // the class measured (either mode) and composed (acting only), and PQ-3's
  // stages relayed served (acting only)
  std::vector<FormDesc> questScopeMarked;
  std::vector<FormDesc> playerClassQuests;
  std::vector<std::pair<FormDesc, uint16_t>> servedStages;
  // the R1 bound's script attachments, indexed at AttachEspm (the fresh
  // review's note 3), by the class's name without case
  const std::unordered_map<
    std::string, std::vector<PartOne::ScriptAttachment>>* scriptAttachments =
    nullptr;
  // D-R1 C's leveled gap (train 52): each leveled list's reach as a landing
  // first asked for it, by the list's form id (only lists of the game files:
  // the caller checks the form is an LVLI first), cleared at AttachEspm
  std::unordered_map<uint32_t, std::vector<uint32_t>> leveledReach;

  // the objects the server places at an accepted cast of a listed spell,
  // and every set standing (thuum docs/verbs/spell-placed-object.md,
  // ADR-057): server-settings.json's `spellPlacedObjects`; the rule is
  // Rust's. Per set its forms, by the rule's set id; per placed form its
  // caster, which every CreateActor of the form names (placedBy)
  rust::Box<skymp::rules::Placements> placements =
    skymp::rules::new_placements("[]");
  std::unordered_map<uint64_t, std::vector<uint32_t>> placedSets;
  std::unordered_map<uint32_t, uint32_t> placedCasterByForm;

  // thuum ADR-068 rung 4 (the created half): a unique actor's attached
  // reference by its base (an ALUA container, noted at its attach), and the
  // created aliases whose unresolved container this world logged, once each
  std::unordered_map<uint32_t, uint32_t> uniqueActorRefs;
  std::set<std::pair<uint32_t, uint32_t>> unresolvedCreationsLogged;

  // thuum ADR-068 rung 4 (WorldGlobal 80): the world streamer, and the users
  // it marked stale for their silence (apart from the cell hosts' marks,
  // which only a host takes)
  rust::Box<skymp::rules::WorldStreamer> worldStreamer =
    skymp::rules::new_world_streamer();
  std::unordered_set<Networking::UserId> streamerStaleUsers;
  bool worldGlobalsEnabled = false;

  // thuum ADR-068 rung 4 (the script set): each open set's world or cell,
  // where its object is placed at its report
  std::unordered_map<uint64_t, uint32_t> scriptSetWorldOrCell;
  // thuum docs/verbs/leveling.md
  rust::Box<skymp::rules::LevelingTable> leveling =
    skymp::rules::new_leveling_table();
  bool levelingEnabled = false;
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
  // `mp host`'s note of how each cell's grant came (GetCellGrantKind): held
  // from a grant or a stop until the cell goes unhosted, so a grant while it
  // is held is a hand-off
  struct CellGrantNote
  {
    bool held = false;
    const char* kind = "none";
  };
  std::unordered_map<uint32_t, CellGrantNote> cellGrantNotes;
  // thuum docs/verbs/npc-fate.md (ADR-060): the actor standing again by its
  // simulator's engine for the length of ResurrectByHost's call, 0 for none:
  // the death state's listener keeps its claims through that stand-up alone
  uint32_t standingUpByHost = 0;
  // the prop owner table (thuum docs/verbs/prop-physics.md, ADR-058): which
  // seat simulates each physics prop, the cell's host by default
  rust::Box<skymp::rules::Props> props = skymp::rules::new_props();
  // the script calls' budgets and sequences (thuum ADR-068, wire-rules
  // natives): runtime only, every budget full and every sequence 0 at a start
  rust::Box<skymp::rules::ScriptCalls> scriptCalls =
    skymp::rules::new_script_calls();
  // the script records' release reads (thuum ADR-068 rung 3, wire-rules
  // script_state): runtime only, no release open at a start
  rust::Box<skymp::rules::ScriptStates> scriptStates =
    skymp::rules::new_script_states();
  // each presser's accepted trigger enters it has not left (thuum ADR-068
  // D-C2, wire-rules trigger_entry::Entries): runtime only, none open at a
  // start
  rust::Box<skymp::rules::TriggerEntries> triggerEntries =
    skymp::rules::new_trigger_entries();
  std::chrono::steady_clock::time_point lastCellsTick;
  using Square = std::tuple<uint32_t, int32_t, int32_t>;
  // one serial per login across the process: a dead-sample stamp's witness
  // names it, and a destroyed-and-recreated actor must never bring an old
  // witness back
  uint32_t loginSerial = 0;
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

namespace {
// the cell host table's decisions, applied (defined below, beside
// FlipHostedFlags); a disconnect's claims need them first (ADR-060)
void ApplyHostDecisions(
  PartOne& partOne, const rust::Vec<skymp::rules::HostDecision>& decisions);

// thuum ADR-068 rung 4: the stream words of one election, at once, never
// the deferred queue: false to the seat whose stream ends, true to the new
// streamer, each naming the election's epoch
void SendStreamWords(PartOne& partOne,
                     const skymp::rules::StreamerElection& election)
{
  if (!election.changed) {
    return;
  }
  const auto tell = [&](uint32_t actorId, bool streams) {
    if (actorId == 0) {
      return;
    }
    const auto& form = partOne.worldState.LookupFormByIdNoLoad(actorId);
    MpActor* actor = form ? form->AsActor() : nullptr;
    if (!actor ||
        partOne.serverState.UserByActor(actor) == Networking::InvalidUserId) {
      return;
    }
    WorldGlobalMessage word;
    word.epoch = election.epoch;
    word.stream = streams;
    actor->SendToUser(word, true);
  };
  tell(election.previous, false);
  tell(election.streamer, true);
  spdlog::info("[verb:world-globals] the world streamer is {:x} from epoch "
               "{} ({:x} before)",
               election.streamer, election.epoch, election.previous);
}
}

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
  TickWeather();
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
    // the login generation: a dead-sample stamp's witness names it, so a
    // relaunch or a reconnect is a new witness (MpActor::DeadStampGap)
    actor.SetLoginGeneration(++pImpl->loginSerial);

    // The clock first, so the client knows the time before its own
    // CreateActor loads the save (thuum docs/verbs/time.md)
    if (pImpl->gameTimeBroadcast) {
      pImpl->sendTarget->Send(
        userId, ToMessage(pImpl->gameClock->login(userId, UnixNowMs())), true);
    }

    actor.ForceSubscriptionsUpdate();

    // the sky of the place the player stands in (thuum docs/verbs/weather.md)
    SendWeatherWord(userId);

    // the perks held and the points left (thuum docs/verbs/perks.md)
    if (pImpl->perksEnabled) {
      actor.SendPerkState();
    }

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

    // thuum ADR-068 rung 4: the world's alias fills after its quest stages,
    // and its world globals after the player's own (the streamer applies
    // the serve before it re-baselines its diff)
    GetActionListener().SendAliasFills(actor);
    GetActionListener().SendWorldGlobals(actor);

    // thuum ADR-068 rung 5 (D-V1 B, D-T2 B): the condition-read variables'
    // watch and record after the world globals, the player's own with them
    GetActionListener().SendScriptVariables(actor);

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

namespace {
const std::unordered_map<std::string, std::vector<PartOne::ScriptAttachment>>&
ScriptAttachmentIndexOf(WorldState& worldState);
const std::unordered_map<std::string, bool>& PlayerAliasScriptIndexOf(
  WorldState& worldState);
}

void PartOne::AttachEspm(espm::Loader* espm)
{
  pImpl->espm = espm;
  worldState.AttachEspm(espm, [this] { return CreateFormCallbacks(); });
  LoadSkillTable();
  LoadWeatherPools();
  LoadPerkTable();
  // thuum ADR-068 rung 5: the quest scope's class needs every QUST's
  // aliases, read now that the game files are here (the settings came first)
  ComposeQuestScope();
  // and D-Q4 C's Story Manager quests, every SMQN node's NNAMs
  ComposeStoryQuests();
  // and the R1 bound's script attachments, read here and not at a seat's
  // first call (the fresh review's note 3)
  pImpl->scriptAttachments = &ScriptAttachmentIndexOf(worldState);
  // a leveled list's reach is read again from these game files
  pImpl->leveledReach.clear();
  // and D-Q9 B's alias scripts: which ride player aliases alone
  pImpl->playerAliasScripts = &PlayerAliasScriptIndexOf(worldState);
  size_t playerOnly = 0;
  for (const auto& entry : *pImpl->playerAliasScripts) {
    if (entry.second) {
      ++playerOnly;
    }
  }
  spdlog::info("[verb:quest-stage] the game files attach {} script class(es) "
               "to quest aliases, {} of them to player aliases alone "
               "(questPlayerAliasCause {}, D-Q9 B)",
               pImpl->playerAliasScripts->size(), playerOnly,
               pImpl->questPlayerAliasWorld ? "world" : "seat");
}

bool PartOne::FlushSaves(std::chrono::milliseconds budget)
{
  const auto start = std::chrono::steady_clock::now();
  size_t requested = 0;
  const bool drained = worldState.FlushSaveStorage(budget, &requested);
  const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::steady_clock::now() - start);
  pImpl->logger->info(
    "FlushSaves - {} unsaved record(s) requested, the storage "
    "{} in {} ms",
    requested, drained ? "drained" : "NOT drained (budget)", took.count());
  return drained;
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

    bool isFF = changeForm.formDesc.file.empty();

    // thuum ADR-068 rung 4 (amendment B, read 7): a game-file reference a
    // script deleted keeps its record, deleted and disabled at its ref
    // state's sequence, so the loader attaches it by that record and every
    // seat that loads its cell is served the deletion (skipped here, the
    // files' standing copy came back after every restart); a server form's
    // deleted record is still dropped (upstream's skip: its id is reused)
    if (changeForm.isDeleted && isFF) {
      pImpl->logger->info(
        "Skipping deleted form {}, will likely overwrite at some point",
        changeForm.formDesc.ToString());
      return;
    }

    // thuum docs/verbs/prop-physics.md (ADR-058): a dropped item's copy is a
    // prop the record keeps across a restart, so the skip below (upstream's,
    // for items its drop deletes two minutes later anyway) holds only when
    // props do not persist (x-prop-push v15 220439: the restored ingot
    // reached neither seat, "Skipping FF item 2 (type is MISC)")
    if (isFF && !worldState.propsPersist) {
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
          // and the trigger enters it has not left (thuum ADR-068 D-C2: a
          // leave pairs with its enter; its presence on the host's copy
          // goes with the figure)
          this_->pImpl->triggerEntries->forget(actor->GetFormId());
          // TGM and TIM are the session's (thuum docs/verbs/god-mode.md)
          actor->SetGodMode(false);
          actor->SetImmortal(false);
          // and the containers it stood in let it go (thuum
          // docs/verbs/containers.md: a disconnect neither destroys nor
          // disables the actor, so the occupant sinks never fired and the
          // chest stayed open for everyone else)
          for (auto* container : std::vector<MpObjectReference*>(
                 actor->OccupiedContainers().begin(),
                 actor->OccupiedContainers().end())) {
            spdlog::info("[verb:containers] {:x} released from {:x} by its "
                         "disconnect",
                         actor->GetFormId(), container->GetFormId());
            container->RemoveContainerOccupant(*actor);
          }
          // every cell it hosts starts its grace (thuum
          // docs/verbs/cell-host.md); the clock passes them on. And its
          // claims end with a word each, a thrall's command's and a
          // dialogue's (ADR-060): the cell's simulator takes each actor
          // back, never left a puppet nobody simulates; a timed thrall dies
          // by the server's word first, a full one stays alive there (ADR-060
          // as amended 2026-11-12). The decisions are applied under a try
          // that logs (thuum lab/.cache/drafts/reanimate/
          // read-stage-125536.md 4.2): this task runs in a ScopedTask's
          // destructor, where an exception ends the process, and the
          // transport has freed the leaver's user before it runs, so a
          // direct send to the leaver threw "User with id 2 doesn't exist"
          // and terminated the server (x-reanimate-claim 20261112-125536);
          // a throw now skips the decisions after it, logged, and the rest
          // of the departure below still runs
          try {
            ApplyHostDecisions(
              *this_,
              this_->pImpl->cells->gone(actor->GetFormId(), SteadyNowMs()));
          } catch (std::exception& e) {
            spdlog::error("[verb:cell-host] {:x}'s departure: a host "
                          "decision threw, the ones after it skipped: {}",
                          actor->GetFormId(), e.what());
          } catch (...) {
            spdlog::error("[verb:cell-host] {:x}'s departure: a host "
                          "decision threw, the ones after it skipped",
                          actor->GetFormId());
          }
          // and the world streamer hands its stream on (thuum ADR-068 rung
          // 4)
          SendStreamWords(
            *this_, this_->pImpl->worldStreamer->gone(actor->GetFormId()));
          this_->pImpl->streamerStaleUsers.erase(userId);
          // and its props fall back to their cells' hosts (thuum
          // docs/verbs/prop-physics.md)
          this_->pImpl->props->props_gone(actor->GetFormId());
          // and the sightings it held for a grant that never came (thuum
          // docs/verbs/npc-puppet.md, finding 66's hold)
          this_->ForgetHeldSightings(actor->GetFormId());
          this_->pImpl->lastSquareByPlayer.erase(actor->GetFormId());
          this_->pImpl->fatedSquares.erase(actor->GetFormId());
          // thuum ADR-068 rung 5: its quest words' churn counters go, and
          // its race overlay ends with the connection (D-T1 A: a relaunch
          // mid-form logs in as its own race)
          this_->pImpl->questRuns->quest_runs_gone(actor->GetFormId());
          const auto overlay =
            this_->pImpl->playerOverlays->player_overlay_gone(
              actor->GetFormId());
          if (overlay.open) {
            spdlog::info("[verb:quest-stage] {:x}'s race overlay {:x} ended "
                         "at its disconnect: {} spell(s), {} shout(s), {} "
                         "item(s) off its figure, its record's race {:x} "
                         "stands (D-T1 A)",
                         actor->GetFormId(), overlay.race,
                         overlay.spells.size(), overlay.shouts.size(),
                         overlay.items.size(), overlay.base);
          }
        }
        this_->pImpl->staleUsers.erase(userId);
        this_->serverState.Disconnect(userId);
        this_->serverState.disconnectingUserId = Networking::InvalidUserId;
        this_->pImpl->gameClock->forget(userId);
        this_->pImpl->weatherSync->forget(userId);
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

namespace {
// thuum D-S3 and ADR-068 rungs 3 and 4: a reference's records served to one
// listener's game on the ordered channel, at once, behind the reference's
// create word and, for a primitive (a trigger the loader keeps as one, which
// gets no create word), at its new subscription (b-ref-state-restart
// 20261107-211516, step 53: the Temple's setStage trigger 0x3ac73, disabled
// with refStateSeq 1, reached no late joiner); `when` ends each line.
// - Its recorded destruction (a spent tripwire, an oil lamp's fallen stage)
//   and its recorded ref state (its owner's Enable, Disable or Delete, or the
//   server's own: disabled and deleted with the object's sequence, an
//   actor's too), served, in one word, so the late seat applies the record
//   without replaying the firing; the word goes before ScriptStateApply,
//   whose sequence can be older (the receiver's served word raises nothing).
// - Its scripts' record, with the sequence and the view they were taken at,
//   to a game that does not host its cell (the host's engine runs the
//   scripts; a new host hears the record ahead of its grant instead,
//   ApplyHostDecisions), its sequence seeding the object's first
//   (ScriptCalls::seed, a max). After a restart each record is the saved one
void ServeReferenceRecords(PartOne& partOne,
                           PartOneSendTargetWrapper* sendTarget,
                           Networking::UserId listenerUserId,
                           MpObjectReference& emitter, MpActor& listener,
                           const char* when)
{
  MpActor* emitterAsActor = emitter.AsActor();
  const auto destruction = emitterAsActor
    ? std::optional<MpChangeFormREFR::Destruction>()
    : emitter.GetDestruction();
  const auto refStateSeq = emitter.GetRefStateSeq();
  if (destruction || refStateSeq) {
    DestructionStageMessage served;
    served.refr = emitter.GetFormId();
    if (destruction) {
      served.stage = destruction->stage;
      served.destroyed = destruction->destroyed;
    }
    if (refStateSeq) {
      served.disabled = emitter.IsDisabled();
      served.deleted = emitter.IsDeleted();
      served.seq = *refStateSeq;
    }
    served.served = true;
    sendTarget->Send(listenerUserId, served, true);
    if (destruction) {
      spdlog::info("[verb:traps] {:x}'s record (stage {}, destroyed {}) "
                   "served to {:x} {}",
                   emitter.GetFormId(), static_cast<int>(destruction->stage),
                   destruction->destroyed, listener.GetFormId(), when);
    }
    if (refStateSeq) {
      spdlog::info("[verb:scripts] ref state {:x} (disabled {}, deleted {}, "
                   "seq {}) served to {:x} {}",
                   emitter.GetFormId(), emitter.IsDisabled(),
                   emitter.IsDeleted(), *refStateSeq, listener.GetFormId(),
                   when);
    }
  }
  if (!emitterAsActor && emitter.GetScriptState()) {
    const auto cell = partOne.worldState.CellOf(emitter);
    const auto host =
      cell ? partOne.GetCellHost(*cell) : skymp::rules::CellHost{};
    ScriptStateApplyMessage apply;
    if ((!host.hosted || host.player != listener.GetFormId()) &&
        emitter.BuildScriptStateApply(apply)) {
      partOne.GetScriptCalls().seed(apply.formId, apply.seq);
      sendTarget->Send(listenerUserId, apply, true);
      // thuum D-C3 A: the graph variables served with it
      spdlog::info("[verb:scripts] script state {:x} served to {:x} {}: {} "
                   "script(s), seq {}, view '{}', {} graph variable(s)",
                   apply.formId, listener.GetFormId(), when,
                   apply.scripts.size(), apply.seq, apply.event.value_or(""),
                   apply.graphVars ? apply.graphVars->size() : 0);
    }
  }
}
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
  // thuum docs/verbs/leveling.md: the skill table's cost of a level, for a
  // server set of a skill (MpActor::SetActorValueBaseByServer moves the
  // progress entry with the base; x-lockpick v4's read, 2026-10-31)
  worldState.skillLevelCost = [this](uint8_t skillAv, uint8_t level) {
    return SkillLevelCost(skillAv, level);
  };
  // and a skill book's level (ReadBookEvent's skill branch): the increment
  // is the server's (leveling.md, skill books)
  worldState.applySkillBook = [this](MpActor& actor, uint8_t skillAv) {
    return ApplySkillBook(actor, skillAv);
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
  // thuum ADR-068 rung 4 (amendment B, read 8): the quest aliases' created
  // items a reference holds, put in at its attach, before its create word
  worldState.onAttach = [this](MpObjectReference& refr) {
    AddCreatedItems(refr);
  };
  // thuum ADR-068 rung 4: the object's sequence now, which a game-file world
  // object's Enable or Disable stamps as its recorded ref state
  worldState.objectSeqOf = [this](uint32_t refrId) {
    return GetScriptCalls().seq_of(refrId);
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
  // thuum ADR-068 rung 4 (b-ref-state-restart 20261107-211516, step 53): a
  // primitive's new subscription, which sends no create word, serves the
  // reference's records as a create word carries them
  worldState.onPrimitiveSubscribe = [this](MpObjectReference& primitive,
                                           MpActor& listener) {
    const auto userId = serverState.UserByActor(&listener);
    if (userId == Networking::InvalidUserId) {
      return;
    }
    ServeReferenceRecords(*this, pImpl->sendTarget.get(), userId, primitive,
                          listener, "at its subscribe (a primitive)");
  };
  // thuum docs/verbs/npc-fate.md (playtest 51, b-npc-stream-equipment): a
  // game's first sight of a game-file actor nothing rolled yet rolls it, at
  // that game's level, ahead of the create word: the npc-puppet sighting's
  // move of an indoor record into a square the host's pass had fated
  // (Faendal 0x1348c and nine more in 20261110-062127, served empty, every
  // worn piece of the host's first report refused, E_EQUIP_REFUSED), a
  // script's MoveTo, a lookup's load, a login's grid
  worldState.onActorSubscribe = [this](MpActor& actor, MpActor& listener) {
    if (serverState.UserByActor(&listener) == Networking::InvalidUserId) {
      return;
    }
    // the roll's loader: the game hosting the actor's cell when one does
    // (its engine runs the actor; the sighting's reporter), else the game
    // it is served to, the first with a user in the subscribe loop's order
    MpActor* host = worldState.HostOf(actor);
    MpActor& loader =
      host && serverState.UserByActor(host) != Networking::InvalidUserId
      ? *host
      : listener;
    try {
      EnsureFated(actor, loader);
    } catch (const std::exception& e) {
      // a roll that throws never unwinds the subscribe it runs in (a login's
      // ForceSubscriptionsUpdate, SetUserActor's sends behind it): the actor
      // is served as the roll left it, unrolled, one line per actor
      static std::unordered_set<uint32_t> warned;
      if (warned.insert(actor.GetFormId()).second) {
        spdlog::error("PartOne - npc-fate: {:x}'s roll at its create word "
                      "to {:x} threw, the actor served unrolled: {}",
                      actor.GetFormId(), listener.GetFormId(), e.what());
      }
    }
    // thuum docs/verbs/npc-fate.md C4 (ADR-060's return): a full thrall
    // served to its caster's game after that game reported the thrall's
    // cell loaded (the engine's 5x5 runs ahead of the server's 3x3, so the
    // load met no emitter) is commanded by it again here, ahead of the
    // create word, which then names the caster the simulator; the word
    // reaches the games already listening. Under the same rule as the roll:
    // a throw never unwinds the subscribe
    if (actor.HasRunningEffects()) {
      try {
        ReturnThralls(listener, { &actor });
      } catch (const std::exception& e) {
        spdlog::error("[verb:npc-fate] {:x}'s return to {:x} at its create "
                      "word threw: {}",
                      actor.GetFormId(), listener.GetFormId(), e.what());
      }
    }
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

    // thuum ADR-068 rung 5 (D-T1 A): another player's figure wears that
    // player's race overlay, which never touches its change form
    if (hasUser && !isMe) {
      message.props.overlay = PlayerOverlayOf(emitter->GetFormId());
    }

    message.idx = emitter->GetIdx();
    message.isMe = isMe;
    message.transform.pos = { emitterPos.x, emitterPos.y, emitterPos.z };
    message.transform.rot = { emitterRot.x, emitterRot.y, emitterRot.z };
    message.transform.worldOrCell = worldOrCell;

    // thuum ADR-050 decision 3 (train 51) and docs/verbs/npc-fate.md
    // (playtest 51): a fated NPC's fate goes ahead of each of its create
    // words, a server-made copy's (its engine rolls the server's roll inside
    // its placement) and a game-file actor's alike. The pass fates a square
    // once per game, so an actor rolled at one game's pass and moved into a
    // square another game had fated (a sighting, a MoveTo) reached that
    // game with its create word alone, and its engine kept its own roll. A
    // repeat costs the game nothing: its ActorFateService runs the late
    // path only when the fate's base, worn list or rows changed
    if (emitterAsActor && !emitterAsActor->IsCreatedAsPlayer() &&
        emitterAsActor->GetFateLevel().has_value()) {
      ActorFateMessage fateMessage;
      fateMessage.fates.push_back(FateOf(worldState, *emitterAsActor));
      sendTarget->Send(listenerUserId, fateMessage, true);
    }

    sendTarget->Send(listenerUserId, message, true);

    // thuum docs/verbs/change-form.md (ADR-065 system 1): the actor's
    // recorded change form rides behind its create word on the same ordered
    // channel, so the seat applies it before the actor's first frame there
    if (emitterAsActor && !isMe) {
      ChangeFormApplyMessage apply;
      if (emitterAsActor->BuildChangeFormApply(apply)) {
        sendTarget->Send(listenerUserId, apply, true);
      }
    }

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

    // the reference's records ride behind its create word on the same
    // ordered channel (ServeReferenceRecords: its destruction and ref state,
    // then its scripts' record for a game that does not host its cell)
    ServeReferenceRecords(*this, sendTarget, listenerUserId, *emitter,
                          *listenerAsActor, "after its create word");

    // thuum ADR-068 rung 4 (the standing, W3): an NPC's recorded standing
    // rides behind its create word as the appliers that bring a late
    // engine's copy to the record (the record's against its base's rows),
    // at once and with no sequence: several serve one subject. A
    // relationship toward the listener's own player reaches its game as
    // 0x14 (SpSnippet::Serve's MakeMessage, thuum train 52), after a relog
    // too
    if (emitterAsActor && !isMe && !hasUser &&
        emitter->GetFormId() < 0xff000000) {
      size_t appliers = 0;
      for (const auto& applier : emitterAsActor->StandingAppliers()) {
        uint32_t formId = 0;
        if (applier.form.shortFormId != 0 || !applier.form.file.empty()) {
          try {
            formId = applier.form.ToFormId(worldState.espmFiles);
          } catch (const std::exception&) {
            continue;
          }
        }
        std::vector<std::optional<
          std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
          args;
        const bool other =
          std::string(applier.function) == "SetRelationshipRank";
        if (formId != 0) {
          args.push_back(SpSnippetObjectArgument{
            SpSnippet::MakeLongFormId(&worldState, formId),
            other ? std::string("Actor") : std::string("Faction") });
        } else {
          args.push_back(std::nullopt);
        }
        if (applier.rank) {
          args.push_back(static_cast<double>(*applier.rank));
        }
        SpSnippet("Actor", applier.function, args, emitter->GetFormId())
          .Serve(listenerAsActor);
        ++appliers;
      }
      if (appliers > 0) {
        spdlog::info("[verb:scripts] standing of {:x} served to {:x} after "
                     "its create word: {} applier(s)",
                     emitter->GetFormId(), listenerAsActor->GetFormId(),
                     appliers);
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

skymp::rules::ScriptCalls& PartOne::GetScriptCalls()
{
  return *pImpl->scriptCalls;
}

skymp::rules::ScriptStates& PartOne::GetScriptStates()
{
  return *pImpl->scriptStates;
}

skymp::rules::TriggerEntries& PartOne::GetTriggerEntries()
{
  return *pImpl->triggerEntries;
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
    const uint32_t simulator = partOne.SimulatorOf(*actor);
    // the record ahead of EVERY word to the game the word names, the host's
    // or another's (non-owning-seat.md section 3; the grant bounce of 055217
    // and 060106; a host swap's Release runs after the grant has named the
    // new host, so its flip names the new host too and carries the keyframe)
    if (MpActor* sim = PlayerActor(partOne, simulator)) {
      partOne.SendActorKeyframe(*actor, *sim);
    }
    actor->UpdateHoster(simulator);
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
        partOne.NoteCellHostDecision(PartOne::CellHostNote::Grant, d.cell);
        // the cell's props nobody holds live are the new host's (thuum
        // docs/verbs/prop-physics.md, ADR-058 decision 1)
        partOne.PropsHostChanged(d.cell, d.player);
        // thuum ADR-068 rung 3: the grant ends the release read the cell's
        // last release left open; from here the old host's epoch stands
        // behind the cell's (wire-rules script_state)
        partOne.GetScriptStates().granted(d.cell);
        MpActor* host = PlayerActor(partOne, d.player);
        if (!host) {
          break;
        }
        // thuum ADR-068 rung 3 (the script record): the cell's recorded
        // references the new host's game sees (FlipHostedFlags' walk of its
        // emitters; an actor's scripts are its simulator's, a later rung's)
        // go to it right ahead of the grant on the same ordered channel, at
        // once, so its engine applies each record before its gate opens for
        // the cell; each record's sequence seeds the object's first
        // (ScriptCalls::seed, a max), so after a restart the next relay
        // takes a number above the one the record is stamped with
        for (MpObjectReference* emitter : host->GetEmitters()) {
          if (!emitter || emitter->AsActor() || !emitter->GetScriptState() ||
              partOne.worldState.CellOf(*emitter) != d.cell) {
            continue;
          }
          ScriptStateApplyMessage apply;
          if (!emitter->BuildScriptStateApply(apply)) {
            continue;
          }
          partOne.GetScriptCalls().seed(apply.formId, apply.seq);
          partOne.GetSendTarget().Send(partOne.serverState.UserByActor(host),
                                       apply, true);
          // thuum D-C3 A (PENDING Eli): the graph variables served with it
          spdlog::info("[verb:scripts] script state {:x} served to {:x} "
                       "ahead of its grant of cell {:x}: {} script(s), seq "
                       "{}, view '{}', {} graph variable(s)",
                       apply.formId, host->GetFormId(), d.cell,
                       apply.scripts.size(), apply.seq,
                       apply.event.value_or(""),
                       apply.graphVars ? apply.graphVars->size() : 0);
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
        partOne.NoteCellHostDecision(PartOne::CellHostNote::Release, d.cell);
        // thuum ADR-068 rung 3: the old host's release read of the cell's
        // references, at the epoch it released, is taken while nobody hosts
        // the cell (wire-rules script_state; the next grant ends it)
        partOne.GetScriptStates().released(d.cell, d.player, d.epoch);
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
        partOne.NoteCellHostDecision(PartOne::CellHostNote::Unhosted, d.cell);
        break;
      case skymp::rules::HostKind::ClaimEnded:
      case skymp::rules::HostKind::ClaimGone: {
        // thuum docs/verbs/npc-talk.md (ADR-052): a claim's lease ran out,
        // or (ADR-060) its claimant left the server, a thrall's commander or
        // a dialogue's claimant; the cell's rule names the simulator again
        // and the word follows, so no actor stays a puppet nobody simulates
        auto& form = partOne.worldState.LookupFormByIdNoLoad(d.actor);
        MpActor* actor = form ? form->AsActor() : nullptr;
        uint32_t simulator = actor ? partOne.SimulatorOf(*actor) : 0u;
        // thuum lab/.cache/drafts/reanimate/read-stage-125536.md 4.2: a
        // departing claimant that hosts the actor's cell, or was just
        // granted it, is the cell's rule's answer through its grace
        // (wire-rules cells.rs: gone() drops its follows and only starts its
        // grace as host, so simulator_of falls to host_of), and the leaver
        // gets no word: its user is gone from the transport, and the
        // keyframe below went to it and threw. The actor waits for its
        // cell's next host; UpdateHoster's word skips the departing user
        const bool leaverSimulates =
          d.kind == skymp::rules::HostKind::ClaimGone && simulator == d.player;
        if (leaverSimulates) {
          simulator = 0;
        }
        if (d.kind == skymp::rules::HostKind::ClaimEnded) {
          spdlog::info("[verb:npc-talk] the claim on {:x} in cell {:x} ended "
                       "with its lease; the cell's rule stands",
                       d.actor, d.cell);
        } else {
          spdlog::info("[verb:npc-talk] the claim on {:x} in cell {:x} ended "
                       "with its claimant {:x}'s departure; {:x} simulates "
                       "it (ADR-052, ADR-060){}",
                       d.actor, d.cell, d.player, simulator,
                       leaverSimulates ? ", the claimant hosting the cell: "
                                         "the word waits for its next host"
                                       : "");
        }
        if (actor) {
          if (MpActor* sim = PlayerActor(partOne, simulator)) {
            partOne.SendActorKeyframe(*actor, *sim);
          }
          actor->UpdateHoster(simulator);
        }
        break;
      }
      case skymp::rules::HostKind::ThrallExpired: {
        // thuum docs/verbs/npc-fate.md (ADR-060 as amended; Eli,
        // 2026-11-12: "'but as a plain resident with no undead look' should
        // simply die"): a timed thrall's commander left and no engine runs
        // its effect now, so the server's word ends it at once. It comes
        // ahead of the departure's ClaimGone for the same actor, so the
        // cell's simulator is handed a body and never promotes a living copy
        // no effect holds
        auto& form = partOne.worldState.LookupFormByIdNoLoad(d.actor);
        MpActor* actor = form ? form->AsActor() : nullptr;
        if (actor && !actor->IsDead()) {
          spdlog::info("[verb:npc-fate] {:x} dies at its commander {:x}'s "
                       "departure: a timed thrall, its effect no engine's "
                       "now, ends by the server's word (ADR-060)",
                       d.actor, d.player);
          actor->Kill(nullptr);
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

bool PartOne::EnsureFated(MpActor& actor, MpActor& loader,
                          const MpActor* walker)
{
  // a game-file actor alone, as the walk's: a server-made NPC rolls at its
  // placement (PlaceAtMe)
  if (actor.GetFormId() >= 0xff000000 || !actor.CanRollFate()) {
    return false;
  }
  const NiPoint3& loaderPos = loader.GetPos();
  const uint16_t level = FateRollLevel(
    loader, loader.GetCellOrWorld().ToFormId(worldState.espmFiles),
    static_cast<int16_t>(loaderPos.x / 4096),
    static_cast<int16_t>(loaderPos.y / 4096));
  if (!actor.RollFateIfUnrolled(level)) {
    return false;
  }
  // the actor's square as the pass fates it (the grid's own, truncated
  // toward zero). The games in its 3x3 get their create word now, the fate
  // ahead of it (onSubscribe), and the walker gets it in its batch; every
  // other game whose pass has fated the square, the ring beyond the 3x3,
  // hears it here, as that pass will not come there again
  const uint32_t cellOrWorld =
    actor.GetCellOrWorld().ToFormId(worldState.espmFiles);
  const NiPoint3& pos = actor.GetPos();
  const auto sx = static_cast<int16_t>(pos.x / 4096);
  const auto sy = static_cast<int16_t>(pos.y / 4096);
  const Impl::Square square{ cellOrWorld, sx, sy };
  const auto& inReach =
    worldState.GetGrids()[cellOrWorld].grid->GetNeighboursByPosition(sx, sy);
  ActorFateMessage msg;
  msg.fates.push_back(FateOf(worldState, actor));
  size_t told = 0;
  for (Networking::UserId userId = 0; userId <= serverState.maxConnectedId;
       ++userId) {
    MpActor* player = serverState.ActorByUser(userId);
    if (!player || player == walker || inReach.count(player)) {
      continue;
    }
    const auto fated = pImpl->fatedSquares.find(player->GetFormId());
    if (fated == pImpl->fatedSquares.end() || !fated->second.count(square)) {
      continue;
    }
    GetSendTarget().Send(userId, msg, true);
    ++told;
  }
  spdlog::info("PartOne - npc-fate: {:x} fated at level {} ({:x}'s) at its "
               "first serve; its fate sent to {} game(s) beyond its 3x3 "
               "whose pass fated its square ({}, {}) in {:x}",
               actor.GetFormId(), level, loader.GetFormId(), told, sx, sy,
               cellOrWorld);
  return true;
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
      // an actor loaded where no game had it in reach (a lookup, another
      // actor's square change), so served to none, rolls here at this pass's
      // level; its fate rides this batch, and EnsureFated tells the other
      // games whose pass has fated the square
      EnsureFated(*actor, player, &player);
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
  // thuum docs/verbs/change-form.md (ADR-065 system 1): the cell's actors
  // with a record are served again at the load, so a seat whose engine
  // attached the cell applies each record before the actor's first frame
  // there (the create word's copy may predate the record, or the engine's
  // 5x5 may run ahead of the server's 3x3); the receiver's hash compare
  // makes a repeat free
  std::vector<MpActor*> inCell;
  if (loaded) {
    size_t served = 0;
    for (MpObjectReference* emitter : player.GetEmitters()) {
      MpActor* actor = emitter ? emitter->AsActor() : nullptr;
      if (!actor || actor == &player ||
          serverState.UserByActor(actor) != Networking::InvalidUserId ||
          worldState.CellOf(*actor) != cell) {
        continue;
      }
      inCell.push_back(actor);
      ChangeFormApplyMessage apply;
      if (actor->BuildChangeFormApply(apply)) {
        player.SendToUser(apply, true);
        ++served;
      }
    }
    if (served > 0) {
      spdlog::info("[verb:change-form] cell {:x} loaded by {:x}: {} record(s) "
                   "served",
                   cell, player.GetFormId(), served);
    }
  }
  const auto nowMs = SteadyNowMs();
  ApplyHostDecisions(
    *this,
    loaded ? pImpl->cells->loaded(player.GetFormId(), cell, nowMs)
           : pImpl->cells->unloaded(player.GetFormId(), cell, nowMs));
  // thuum docs/verbs/npc-fate.md C4 (ADR-060's return): a full thrall its
  // record names this player the caster of is commanded by it again once
  // its game has the thrall's cell loaded (the cells table knows the load
  // only from here on); a thrall the grid serves it later is the
  // subscribe's (onActorSubscribe)
  if (loaded && !inCell.empty()) {
    ReturnThralls(player, inCell);
  }
}

void PartOne::OnDeathState(MpActor& actor, bool isDead)
{
  // thuum docs/verbs/npc-fate.md (ADR-060): a commanded actor's death ends
  // its commander's claim (the effect's end kills the thrall in the
  // commander's engine, vanilla's ash pile), a dialogue's claim with it; the
  // cell's rule simulates it
  if (isDead) {
    const uint32_t commander = CommanderOf(actor);
    if (ReleaseAnyClaim(actor) != 0 && commander != 0) {
      spdlog::info("[verb:npc-fate] {:x} died under {:x}'s command: the "
                   "claim ends, {:x} simulates it (ADR-060)",
                   actor.GetFormId(), commander, SimulatorOf(actor));
    }
  } else if (pImpl->standingUpByHost != actor.GetFormId()) {
    // a respawn the engine did not make starts a new life: its accepted
    // reanimate, spell and caster end (the server review of 3a31c05a, N2)
    actor.NoteReanimateAccepted({});
    // and a respawn its simulator's engine did not stand (the fate's sweep,
    // the console's Resurrect, the lab's respawn-ref) ends any claim on it:
    // a command whose engine never stood the body would name its commander
    // on the living actor in any cell, and that commander's cause-0 report
    // would kill it (the fresh review of f89115af, C2); the commander's own
    // stand-up (ResurrectByHost) keeps it
    if (const uint32_t former = ReleaseAnyClaim(actor); former != 0) {
      spdlog::info("[verb:npc-fate] {:x} stands again by a respawn its "
                   "simulator's engine did not make: {:x}'s claim ends, {:x} "
                   "simulates it (ADR-060)",
                   actor.GetFormId(), former, SimulatorOf(actor));
    }
  }
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
    SendStreamWords(*this,
                    pImpl->worldStreamer->dead(actor.GetFormId(), true));
  } else {
    spdlog::info("PartOne - cell-host: {:x} stands again: a candidate again "
                 "(D18)",
                 actor.GetFormId());
    ApplyHostDecisions(*this,
                       pImpl->cells->respawned(actor.GetFormId(), nowMs));
    SendStreamWords(*this,
                    pImpl->worldStreamer->dead(actor.GetFormId(), false));
  }
}

// degrees to radians for the snippet's rotation arguments (the client's
// moveRefrToPosition takes radians, as remoteServer's teleport converts)
static constexpr float kDegToRad = 3.14159265358979323846f / 180.0f;

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
  // playtest 42's F1 and F2 (2026-11-03): a sighter's own copy of an actor
  // standing where the record does not (the record in a cell another game
  // hosts, or the copy in another game's cell while the record stands
  // elsewhere) is told to MOVE to the record, cell and position, through the
  // native the client uses for a player's own cross-cell move
  // (TESModPlatform's moveRefrToPosition, the call formView's gate makes when
  // a view exists). Playtest 37's finding 38 ("we have 2 embrys") first met
  // this orphan and sent ObjectReference.Disable; playtest 42's F1 (c2's
  // Embry solid at the inn's porch while the record sat inside under c1, Hod
  // walking into it) showed the Disable never REACHED the copy: its selfId
  // went through MakeLongFormId's actor branch and the client resolves a
  // long id through a view this game no longer had (runMethod threw "Unable
  // to find form with id 0"), so the roster kept sighting the porch copy
  // every 10 s after each hide. Playtest 40's F2 (Hilde's ghost) is the
  // other shape: D11's release after a move indoors walking a copy back out.
  // The copy now travels as an ARGUMENT with its raw id and selfId 0 (a
  // static call), which the client maps to itself. A copy moved into an
  // interior this game has not loaded loses its 3D and its collision here and
  // stands at the record when that game walks in. The record's cell-or-world
  // form goes typed as both the Cell and the WorldSpace argument: the
  // client's deserializer casts and yields null for the one that does not
  // fit, so no null travels (a null argument would fault its deserializer).
  // Once a minute per sighter and actor. The move itself is npc-handoff's H5
  // (CONFIRMED at T3 for an engine reference moved into an unloaded interior
  // through this native). HYPOTHESIS until the lab: a copy with no view and
  // the puppet flag lands the same; and, after D11's release 4 s later, the
  // copy's own schedule may walk it off the record (Hod's porch record while
  // nobody hosts the street), the minute's cadence then showing as a pop.
  static std::unordered_map<uint64_t, std::chrono::steady_clock::time_point>
    moved;
  const auto tellToMoveToRecord = [&](MpActor& actor, uint32_t recordCell,
                                      const char* hostedBy) {
    // a body is never moved (D78 C): a dead record's copy is npc-fate's,
    // killed where it stands when its view arrives
    if (actor.IsDead()) {
      return;
    }
    const uint64_t key =
      (static_cast<uint64_t>(reporter.GetFormId()) << 32) | refrId;
    const auto now = std::chrono::steady_clock::now();
    auto& last = moved[key];
    if (last != std::chrono::steady_clock::time_point{} &&
        now - last < std::chrono::minutes(1)) {
      return;
    }
    last = now;
    const uint32_t recordCellOrWorld =
      actor.GetCellOrWorld().ToFormId(worldState.espmFiles);
    const NiPoint3 recordPos = actor.GetPos();
    const NiPoint3 recordRot = actor.GetAngle();
    SpSnippetObjectArgument copy;
    copy.formId = refrId;
    copy.type = "ObjectReference";
    SpSnippetObjectArgument asCell;
    asCell.formId = recordCellOrWorld;
    asCell.type = "Cell";
    SpSnippetObjectArgument asWorld;
    asWorld.formId = recordCellOrWorld;
    asWorld.type = "WorldSpace";
    const std::vector<std::optional<
      std::variant<bool, double, std::string, SpSnippetObjectArgument>>>
      toRecord{ copy,
                asCell,
                asWorld,
                static_cast<double>(recordPos.x),
                static_cast<double>(recordPos.y),
                static_cast<double>(recordPos.z),
                static_cast<double>(recordRot.x * kDegToRad),
                static_cast<double>(recordRot.y * kDegToRad),
                static_cast<double>(recordRot.z * kDegToRad) };
    SpSnippet("TESModPlatform", "moveRefrToPosition", toRecord, 0)
      .Execute(&reporter, SpSnippetMode::kNoReturnResult);
    spdlog::info(
      "PartOne - npc-puppet: {:x}'s own copy of {:x} told to move "
      "to the record at ({:.0f}, {:.0f}, {:.0f}) in {:x} (the "
      "record's cell {:x} is {}; the copy stood at ({:.0f}, {:.0f}, "
      "{:.0f}) in {:x} here)",
      reporter.GetFormId(), refrId, recordPos.x, recordPos.y, recordPos.z,
      recordCellOrWorld, recordCell, hostedBy, pos.x, pos.y, pos.z,
      worldOrCell);
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
    } else {
      // playtest 42's F2: c2's engine walked Hod through the inn's door after
      // c2's demote and sighted him inside c1's inn while the record stayed
      // at the porch outside; the copy follows the record, never the record
      // the copy
      // a record the server holds already; an actor with no record is its
      // host's roster's to create (the loader's disabled filters yield to the
      // host's engine alone, rule 5)
      const auto& sightedForm = worldState.LookupFormByIdNoLoad(refrId);
      MpActor* sighted = sightedForm ? sightedForm->AsActor() : nullptr;
      if (sighted &&
          serverState.UserByActor(sighted) == Networking::InvalidUserId) {
        const auto recordCell = worldState.CellOf(*sighted);
        if (recordCell && *recordCell != *cell) {
          const MpActor* recordHost = worldState.HostOf(*sighted);
          // never the record's own host: its engine is the truth (branch (a)'s
          // rule and the view's gate's, formView)
          if (recordHost != &reporter) {
            tellToMoveToRecord(*sighted, *recordCell,
                               recordHost ? "another game's"
                                          : "nobody's, frozen at the record");
          }
        }
      }
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
    // one's; the records meet at a handoff (npc-handoff); the sighter's copy
    // goes to the record (tellToMoveToRecord above)
    tellToMoveToRecord(*actor, standing.value_or(0), "another game's");
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
  TickBodies(SteadyNowMs());
  SweepHeldSightings(SteadyNowMs());
}

void PartOne::TickBodies(uint64_t nowMs)
{
  if (worldState.settlingBodies.empty()) {
    return;
  }
  const std::vector<uint32_t> ids(worldState.settlingBodies.begin(),
                                  worldState.settlingBodies.end());
  for (const auto id : ids) {
    auto& form = worldState.LookupFormByIdNoLoad(id);
    MpActor* actor = form ? form->AsActor() : nullptr;
    if (!actor || !actor->IsBodySettling()) {
      worldState.settlingBodies.erase(id);
      continue;
    }
    actor->SettleBodyIfDue(nowMs);
  }
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
      // thuum ADR-068 rung 4: the world streamer reads the same silence for
      // every seat (its own eligibility wants a movement report first, so a
      // login's load block is no election)
      const bool streamerStale = pImpl->streamerStaleUsers.count(userId) > 0;
      if (silenceMs >= kStaleHostMs && !streamerStale) {
        pImpl->streamerStaleUsers.insert(userId);
        SendStreamWords(*this,
                        pImpl->worldStreamer->stale(actor->GetFormId(), true));
      } else if (silenceMs >= 0 && silenceMs < kStaleHostMs && streamerStale) {
        pImpl->streamerStaleUsers.erase(userId);
        SendStreamWords(
          *this, pImpl->worldStreamer->stale(actor->GetFormId(), false));
      }
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

const char* PartOne::GetCellGrantKind(uint32_t cell) const
{
  const auto it = pImpl->cellGrantNotes.find(cell);
  return it == pImpl->cellGrantNotes.end() ? "none" : it->second.kind;
}

void PartOne::NoteCellHostDecision(CellHostNote note, uint32_t cell)
{
  auto& kept = pImpl->cellGrantNotes[cell];
  switch (note) {
    case CellHostNote::Grant:
      kept.kind = kept.held ? "inherited" : "fresh";
      kept.held = true;
      break;
    case CellHostNote::Release:
      // until the cell goes unhosted, the next grant is a hand-off
      kept.held = true;
      break;
    case CellHostNote::Unhosted:
      kept.held = false;
      break;
  }
}

std::vector<uint32_t> PartOne::GetCellLoaders(uint32_t cell) const
{
  std::vector<uint32_t> out;
  for (const uint32_t player : pImpl->cells->loaders_of(cell)) {
    out.push_back(player);
  }
  return out;
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
    // the record ahead of the word, then the word to every listening game:
    // the claimant simulates it now
    SendActorKeyframe(actor, claimant);
    actor.UpdateHoster(claimant.GetFormId());
  }
  return verdict;
}

bool PartOne::ReleaseClaim(MpActor& claimant, MpActor& actor)
{
  const bool released =
    pImpl->cells->release_claim(actor.GetFormId(), claimant.GetFormId());
  if (released) {
    const uint32_t simulator = SimulatorOf(actor);
    auto& form = worldState.LookupFormByIdNoLoad(simulator);
    if (MpActor* sim = form ? form->AsActor() : nullptr) {
      SendActorKeyframe(actor, *sim);
    }
    actor.UpdateHoster(simulator);
  }
  return released;
}

std::vector<uint32_t> PartOne::ReturnThralls(
  MpActor& player, const std::vector<MpActor*>& actors)
{
  std::vector<skymp::rules::ThrallRecord> records;
  std::unordered_map<uint32_t, MpActor*> byId;
  for (MpActor* actor : actors) {
    if (!actor || actor == &player || !actor->HasRunningEffects() ||
        serverState.UserByActor(actor) != Networking::InvalidUserId) {
      continue;
    }
    const auto reanimate = actor->GetReanimateCaster();
    if (reanimate.caster == 0 || reanimate.caster != player.GetFormId() ||
        !std::isfinite(reanimate.recordSeconds)) {
      continue;
    }
    const auto cell = worldState.CellOf(*actor);
    if (!cell) {
      continue;
    }
    skymp::rules::ThrallRecord record;
    record.actor = actor->GetFormId();
    record.cell = *cell;
    record.caster = reanimate.caster;
    record.record_s =
      static_cast<uint32_t>(std::clamp(reanimate.recordSeconds, 0.f, 4.0e9f));
    record.alive = !actor->IsDead() && !actor->IsRespawning();
    records.push_back(record);
    byId[record.actor] = actor;
  }
  if (records.empty()) {
    return {};
  }
  const auto claimed =
    pImpl->cells->returning(player.GetFormId(),
                            rust::Slice<const skymp::rules::ThrallRecord>(
                              records.data(), records.size()),
                            SteadyNowMs());
  std::vector<uint32_t> returned;
  for (const uint32_t id : claimed) {
    const auto it = byId.find(id);
    if (it == byId.end()) {
      continue;
    }
    MpActor& actor = *it->second;
    const auto cell = worldState.CellOf(actor);
    spdlog::info("[verb:npc-fate] {:x} returns to its caster {:x} in cell "
                 "{:x}: a full thrall its record names (the record's {:.0f} "
                 "s), commanded again (ADR-060)",
                 id, player.GetFormId(), cell.value_or(0u),
                 actor.GetReanimateCaster().recordSeconds);
    // ClaimGone's shape for a living actor: the record is the truth for
    // this one moment, so the new simulator's copy is placed at it first
    // (SendActorKeyframe sends only to a game that sees the actor)
    SendActorKeyframe(actor, player);
    actor.UpdateHoster(player.GetFormId());
    returned.push_back(id);
  }
  return returned;
}

PartOne::ClaimVerdict PartOne::ClaimCommanded(MpActor& commander,
                                              MpActor& actor,
                                              uint32_t recordSeconds)
{
  const auto cell = worldState.CellOf(actor);
  if (!cell) {
    return ClaimVerdict::Unloaded;
  }
  const auto code =
    pImpl->cells->claim_commanded(actor.GetFormId(), commander.GetFormId(),
                                  *cell, recordSeconds, SteadyNowMs());
  const auto verdict = static_cast<ClaimVerdict>(code);
  if (verdict == ClaimVerdict::Granted || verdict == ClaimVerdict::Reclaimed) {
    // the word to every listening game: the commander simulates it now
    actor.UpdateHoster(commander.GetFormId());
  }
  return verdict;
}

uint32_t PartOne::CommanderOf(const MpActor& actor) const
{
  return pImpl->cells->commander_of(actor.GetFormId());
}

uint32_t PartOne::ReleaseAnyClaim(MpActor& actor)
{
  const uint32_t former = pImpl->cells->release_any_claim(actor.GetFormId());
  if (former != 0) {
    actor.UpdateHoster(SimulatorOf(actor));
  }
  return former;
}

void PartOne::ResurrectByHost(MpActor& actor)
{
  // the mark lives for the call alone: a gamemode listener may block the
  // respawn (GameModeEvent::Fire), and then no death state reads it
  pImpl->standingUpByHost = actor.GetFormId();
  ScopedTask clear([this] { pImpl->standingUpByHost = 0; });
  actor.Respawn(false);
}

void PartOne::SendActorKeyframe(MpActor& actor, MpActor& to)
{
  const auto userId = serverState.UserByActor(&to);
  if (userId == Networking::InvalidUserId) {
    return;
  }
  // only to a game that sees the actor: a Release's flip walks the old
  // host's emitters, and a keyframe for a form the new host lacks is "Form
  // with idx N not found" there (numidium's review of 0b61fc80, item 7)
  if (to.GetEmitters().count(&actor) == 0) {
    return;
  }
  UpdateMovementMessage msg;
  msg.idx = actor.GetIdx();
  msg.data.worldOrCell = actor.GetCellOrWorld().ToFormId(worldState.espmFiles);
  const auto& pos = actor.GetPos();
  const auto& rot = actor.GetAngle();
  msg.data.pos = { pos.x, pos.y, pos.z };
  msg.data.rot = { rot.x, rot.y, rot.z };
  msg.data.runMode = "Standing";
  msg.data.isWeapDrawn = actor.IsWeaponDrawn();
  msg.data.isDead = actor.IsDead();
  // the edge validates the keyframe like any movement (wire-validate's
  // bounds), so a record's health is clamped to its range rather than
  // refused silently while the word still goes
  msg.data.healthPercentage =
    std::clamp(actor.GetChangeForm().actorValues.healthPercentage, 0.f, 1.f);
  GetSendTarget().Send(userId, msg, true);
}

PartOne::PropClaimVerdict PartOne::ClaimProp(const MpActor& claimant,
                                             const MpObjectReference& prop,
                                             uint32_t cell, uint8_t touch,
                                             bool worldBody)
{
  const auto code = worldBody
    ? pImpl->props->claim_prop_world_body(
        prop.GetFormId(), claimant.GetFormId(), cell, SteadyNowMs())
    : pImpl->props->claim_prop(prop.GetFormId(), claimant.GetFormId(), cell,
                               touch, SteadyNowMs());
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
  pImpl->playerGlobalsJson = playerGlobalsJson;
  spdlog::info("PartOne::SetPlayerGlobalsSettings - {}", playerGlobalsJson);
  // thuum ADR-068 rung 5: the player class's globals join the setting's
  ComposePlayerGlobals();
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
  // thuum ADR-068 rung 5 (read 4): the retired `allowed` is read and ignored,
  // said once here; its quests stay known to the quest scope's measurement
  // line alone (a mark on that list stops for Eli)
  if (const std::string retired(pImpl->sharedQuests->shared_quests_retired());
      !retired.empty()) {
    spdlog::warn("[verb:quest-stage] {}", retired);
  }
  pImpl->retiredAllowed.clear();
  const auto block = nlohmann::json::parse(sharedQuestsJson, nullptr, false);
  if (block.is_object() && block.contains("allowed") &&
      block["allowed"].is_array()) {
    for (const auto& entry : block["allowed"]) {
      if (entry.is_string()) {
        pImpl->retiredAllowed.push_back(
          FormDesc::FromString(entry.get<std::string>()));
      }
    }
  }
  ComposeQuestScope();
}

namespace {
// thuum ADR-068 rung 5: a FormDesc as the settings spell it,
// "<hex id>:<plugin>" (one to eight hex digits, a plugin named); throws
// naming the setting and the entry (FormDesc::FromString checks nothing)
FormDesc SettingFormDesc(const char* setting, const std::string& text)
{
  const auto colon = text.find(':');
  const std::string id =
    colon == std::string::npos ? std::string() : text.substr(0, colon);
  const bool hex = !id.empty() && id.size() <= 8 &&
    std::all_of(id.begin(), id.end(),
                [](unsigned char c) { return std::isxdigit(c) != 0; });
  if (!hex || colon + 1 >= text.size()) {
    throw std::runtime_error(std::string(setting) + ": \"" + text +
                             "\" is not \"<hex id>:<plugin>\"");
  }
  return FormDesc::FromString(text);
}

// the same form: the id, and the plugin compared without case, as the
// engine compares plugin names (the census spells USSEP in lower case)
bool SameForm(const FormDesc& a, const FormDesc& b)
{
  return a.shortFormId == b.shortFormId && a.file.size() == b.file.size() &&
    std::equal(a.file.begin(), a.file.end(), b.file.begin(),
               [](char x, char y) {
                 return std::tolower(static_cast<unsigned char>(x)) ==
                   std::tolower(static_cast<unsigned char>(y));
               });
}

bool HoldsForm(const std::vector<FormDesc>& list, const FormDesc& form)
{
  return std::any_of(list.begin(), list.end(),
                     [&](const FormDesc& f) { return SameForm(f, form); });
}

// one of the census's per-quest maps ({"<quest>": [values]}) as JSON text,
// each value read by `read`; throws naming the setting and the entry
template <class Value, class Read>
std::vector<std::pair<FormDesc, std::vector<Value>>> PlayerQuestMap(
  const char* setting, const std::string& text, Read read)
{
  const auto j = nlohmann::json::parse(text);
  if (!j.is_object()) {
    throw std::runtime_error(std::string(setting) +
                             ": an object {\"<hex id>:<plugin>\": [...]} "
                             "was expected");
  }
  std::vector<std::pair<FormDesc, std::vector<Value>>> out;
  for (const auto& [key, values] : j.items()) {
    if (!values.is_array()) {
      throw std::runtime_error(std::string(setting) + ": \"" + key +
                               "\" names no list");
    }
    std::vector<Value> parsed;
    for (const auto& value : values) {
      parsed.push_back(read(value));
    }
    out.emplace_back(SettingFormDesc(setting, key), std::move(parsed));
  }
  return out;
}

// the two reserved names of a form's queued OnUpdateGameTime registration
// (skyrim-platform ScriptState.cpp's kGameTimeHours and kGameTimeRepeat,
// read by ReadScriptVariables; wire-rules script_variables' GAME_TIME_HOURS
// and GAME_TIME_REPEAT): D-T2 B's re-issue at a login
constexpr const char* kGameTimeHours = "<OnUpdateGameTime>";
constexpr const char* kGameTimeRepeat = "<OnUpdateGameTime:repeat>";

// the wire's cap on a PlayerGlobals watch (wire-schema cap::PLAYER_GLOBALS)
constexpr size_t kPlayerGlobalsMax = 256;

// thuum ADR-068 rung 5 (D-Q1's alias half): one QUST's facts for its class,
// read once per set of game files (the tests' many PartOnes share them, as
// AliasCreationsOf's index): its aliases as libespm's QUST::GetQuestAliases
// reads them, the forms in the load order (an external fill's alias read
// one hop), and its first attached script, which names the key of its
// game-time registration
struct QuestScopeFacts
{
  FormDesc quest;
  std::vector<skymp::rules::QuestScopeAliasRow> aliases;
  std::string firstScript;
  // thuum D-Q9 B (train 52): each script its VMAD attaches to one of its
  // aliases (libespm ScriptData::aliases), with that alias's id
  std::vector<std::pair<uint32_t, std::string>> aliasScripts;
};

const std::vector<QuestScopeFacts>& QuestScopeFactsOf(WorldState& worldState)
{
  static std::map<const espm::Loader*, std::vector<QuestScopeFacts>> g_facts;
  auto it = g_facts.find(&worldState.GetEspm());
  if (it != g_facts.end()) {
    return it->second;
  }
  // built whole before it is kept, so a read that throws keeps nothing
  std::vector<QuestScopeFacts> facts;
  auto& browser = worldState.GetEspm().GetBrowser();
  auto& cache = worldState.GetEspmCache();
  const auto global = [](const espm::LookupResult& at, uint32_t raw) {
    return raw ? at.ToGlobalId(raw) : 0u;
  };
  for (const auto& lookup : browser.GetDistinctRecordsByType("QUST")) {
    const auto* qust = espm::Convert<espm::QUST>(lookup.rec);
    if (!qust) {
      continue;
    }
    QuestScopeFacts quest;
    try {
      quest.quest = FormDesc::FromFormId(
        lookup.ToGlobalId(lookup.rec->GetId()), worldState.espmFiles);
    } catch (const std::exception&) {
      continue;
    }
    for (const auto& alias : qust->GetQuestAliases(cache)) {
      skymp::rules::QuestScopeAliasRow row{};
      row.location = alias.locationAlias;
      row.id = alias.locationAlias ? alias.locationID : alias.aliasID;
      row.forced = global(lookup, alias.aliasForcedReference);
      row.unique_actor = global(lookup, alias.aliasUniqueActor);
      row.created = global(lookup, alias.aliasCreatedObject);
      row.create_at = alias.createAt;
      row.external = false;
      if (!alias.locationAlias && alias.externalAliasReference) {
        const auto other =
          browser.LookupById(global(lookup, alias.externalAliasReference));
        if (const auto* otherQust = espm::Convert<espm::QUST>(other.rec)) {
          for (const auto& theirs : otherQust->GetQuestAliases(cache)) {
            if (!theirs.locationAlias &&
                theirs.aliasID == alias.externalAlias) {
              row.external = true;
              row.external_forced = global(other, theirs.aliasForcedReference);
              row.external_unique_actor =
                global(other, theirs.aliasUniqueActor);
              break;
            }
          }
        }
      }
      quest.aliases.push_back(row);
    }
    espm::ScriptData scriptData;
    lookup.rec->GetScriptData(&scriptData, cache);
    for (const auto& script : scriptData.scripts) {
      if (script.status != 3 && !script.scriptName.empty()) {
        quest.firstScript = script.scriptName;
        break;
      }
    }
    for (const auto& alias : scriptData.aliases) {
      if (alias.aliasId < 0) {
        continue; // an alias object the section did not name
      }
      for (const auto& script : alias.scripts) {
        if (script.status != 3 && !script.scriptName.empty()) {
          quest.aliasScripts.emplace_back(static_cast<uint32_t>(alias.aliasId),
                                          script.scriptName);
        }
      }
    }
    facts.push_back(std::move(quest));
  }
  return g_facts.emplace(&worldState.GetEspm(), std::move(facts))
    .first->second;
}

std::string LowerScriptClass(std::string name)
{
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return name;
}

// thuum D-Q9 B (Eli 2026-11-11; train 52): every script class a QUST's VMAD
// attaches to an alias, by its name without case: true when every alias it
// rides, over every quest, is the player's (wire-rules
// quest_scope::is_players over that quest's aliases), false when one is not.
// Read once per set of game files over the quest scope's facts
// (QuestScopeFactsOf's shape: built whole before it is kept). A script also
// attached to a placed reference is read by its aliases alone (the QUST VMADs
// are what the index reads)
const std::unordered_map<std::string, bool>& PlayerAliasScriptIndexOf(
  WorldState& worldState)
{
  static std::map<const espm::Loader*, std::unordered_map<std::string, bool>>
    g_index;
  auto it = g_index.find(&worldState.GetEspm());
  if (it != g_index.end()) {
    return it->second;
  }
  std::unordered_map<std::string, bool> index;
  for (const auto& q : QuestScopeFactsOf(worldState)) {
    if (q.aliasScripts.empty()) {
      continue;
    }
    const auto players = skymp::rules::quest_scope_player_aliases(
      rust::Slice<const skymp::rules::QuestScopeAliasRow>(q.aliases.data(),
                                                          q.aliases.size()));
    for (const auto& [aliasId, script] : q.aliasScripts) {
      const bool player =
        std::find(players.begin(), players.end(), aliasId) != players.end();
      auto [at, added] = index.emplace(LowerScriptClass(script), player);
      if (!added) {
        at->second = at->second && player;
      }
    }
  }
  return g_index.emplace(&worldState.GetEspm(), std::move(index))
    .first->second;
}

// thuum ADR-068 rung 5 (the fresh review's note 3): every QUST's and MGEF's
// script attachments with what their properties name, a QUST's alias
// scripts among them (a review of the rotfern-eyes slot: an own row its
// alias script called found no attachment, E_SCRIPTCALL_UNNAMED with "the
// class has 0 attachment(s)"), and the SPELs that carry each MGEF, read once
// per set of game files (QuestScopeFactsOf's shape: built whole before it is
// kept, so a read that throws keeps nothing). A script a record marks
// removed (status 3) and a property so marked are skipped, as the core
// reads a VMAD elsewhere
const std::unordered_map<std::string, std::vector<PartOne::ScriptAttachment>>&
ScriptAttachmentIndexOf(WorldState& worldState)
{
  using Index =
    std::unordered_map<std::string, std::vector<PartOne::ScriptAttachment>>;
  static std::map<const espm::Loader*, Index> g_index;
  auto it = g_index.find(&worldState.GetEspm());
  if (it != g_index.end()) {
    return it->second;
  }
  Index index;
  auto& browser = worldState.GetEspm().GetBrowser();
  auto& cache = worldState.GetEspmCache();
  std::unordered_map<uint32_t, std::vector<uint32_t>> carriers;
  for (const auto& lookup :
       browser.GetDistinctRecordsByType(espm::SPEL::kType)) {
    const auto* spel = espm::Convert<espm::SPEL>(lookup.rec);
    if (!spel) {
      continue;
    }
    const uint32_t spell = lookup.ToGlobalId(lookup.rec->GetId());
    for (const auto& effect : spel->GetData(cache).effects) {
      if (!effect.effectFormId) {
        continue;
      }
      auto& spells = carriers[lookup.ToGlobalId(effect.effectFormId)];
      if (std::find(spells.begin(), spells.end(), spell) == spells.end()) {
        spells.push_back(spell);
      }
    }
  }
  for (const char* type : { espm::QUST::kType, espm::MGEF::kType }) {
    const bool quest = !std::strcmp(type, espm::QUST::kType);
    for (const auto& lookup : browser.GetDistinctRecordsByType(type)) {
      espm::ScriptData scriptData;
      lookup.rec->GetScriptData(&scriptData, cache);
      const uint32_t form = lookup.ToGlobalId(lookup.rec->GetId());
      // a QUST's alias scripts beside its own (libespm's ScriptData::
      // aliases, the VMAD's Alias Scripts Section): each an attachment of
      // the quest, live as the quest is, its properties named as the
      // quest's own scripts' are; RotfernEyes.esp carries its script on its
      // player alias alone (thuum D-E1 A), as eight quests of Skyrim.esm,
      // Dawnguard.esm and Dragonborn.esm do
      std::vector<const espm::Script*> scripts;
      for (const auto& script : scriptData.scripts) {
        scripts.push_back(&script);
      }
      for (const auto& alias : scriptData.aliases) {
        for (const auto& script : alias.scripts) {
          scripts.push_back(&script);
        }
      }
      for (const espm::Script* each : scripts) {
        const auto& script = *each;
        if (script.status == 3 || script.scriptName.empty()) {
          continue;
        }
        PartOne::ScriptAttachment attachment;
        attachment.form = form;
        attachment.quest = quest;
        if (!quest) {
          if (const auto c = carriers.find(form); c != carriers.end()) {
            attachment.spells = c->second;
          }
        }
        const auto add = [&](uint32_t named) {
          if (named == 0 ||
              std::any_of(attachment.named.begin(), attachment.named.end(),
                          [&](const auto& n) { return n.form == named; })) {
            return;
          }
          PartOne::ScriptAttachment::Named n;
          n.form = named;
          const auto list = browser.LookupById(named);
          if (const auto* flst = espm::Convert<espm::FLST>(list.rec)) {
            for (uint32_t member : flst->GetData(cache).formIds) {
              n.members.push_back(list.ToGlobalId(member));
            }
          }
          attachment.named.push_back(std::move(n));
        };
        for (const auto& property : script.properties) {
          if (property.status == 3) {
            continue;
          }
          if (property.type == espm::Property::Type::Object &&
              property.value.formId) {
            add(lookup.ToGlobalId(property.value.formId));
          } else if (property.type == espm::Property::Type::ObjectArray) {
            for (const auto& element : property.array) {
              if (element.formId) {
                add(lookup.ToGlobalId(element.formId));
              }
            }
          }
        }
        index[LowerScriptClass(script.scriptName)].push_back(
          std::move(attachment));
      }
    }
  }
  return g_index.emplace(&worldState.GetEspm(), std::move(index))
    .first->second;
}

std::vector<skymp::rules::QuestStageEntry> QuestEntries(
  const std::vector<FormDesc>& quests)
{
  std::vector<skymp::rules::QuestStageEntry> entries;
  entries.reserve(quests.size());
  for (const auto& q : quests) {
    entries.push_back({ q.shortFormId, rust::String(q.file), 0 });
  }
  return entries;
}

// thuum ADR-068 rung 5 (D-Q4 C): every quest a Story Manager quest node of
// the game files names (libespm SMQN.h, its NNAMs: CommonLibSSE-NG
// include/RE/B/BGSStoryManagerQuestNode.h:48), each with the first node that
// names it ("<editor id> <hex id>") and how many do, and the nodes read,
// once per set of game files (QuestScopeFactsOf's shape: built whole before
// it is kept). A node is read in the last file's version
// (GetDistinctRecordsByType), and one that file deletes or marks ignored
// names nothing (the node's RecordFlags, kDeleted 1 << 5 and kIgnored
// 1 << 12, BGSStoryManagerQuestNode.h:29-30)
struct StoryQuest
{
  FormDesc quest;
  std::string firstNode;
  size_t nodes = 0;
};

struct StoryIndex
{
  std::vector<StoryQuest> quests;
  size_t nodes = 0;
};

constexpr uint32_t kStoryNodeSkipped = (1u << 5) | (1u << 12);

const StoryIndex& StoryIndexOf(WorldState& worldState)
{
  static std::map<const espm::Loader*, StoryIndex> g_story;
  auto it = g_story.find(&worldState.GetEspm());
  if (it != g_story.end()) {
    return it->second;
  }
  StoryIndex index;
  auto& browser = worldState.GetEspm().GetBrowser();
  auto& cache = worldState.GetEspmCache();
  for (const auto& lookup :
       browser.GetDistinctRecordsByType(espm::SMQN::kType)) {
    const auto* node = espm::Convert<espm::SMQN>(lookup.rec);
    if (!node || (lookup.rec->GetFlags() & kStoryNodeSkipped) != 0) {
      continue;
    }
    ++index.nodes;
    const char* editorId = lookup.rec->GetEditorId(cache);
    const std::string name =
      fmt::format("{} {:x}", editorId ? editorId : "",
                  lookup.ToGlobalId(lookup.rec->GetId()));
    for (const uint32_t raw : node->GetData(cache).quests) {
      if (raw == 0) {
        continue;
      }
      FormDesc quest;
      try {
        quest =
          FormDesc::FromFormId(lookup.ToGlobalId(raw), worldState.espmFiles);
      } catch (const std::exception&) {
        continue;
      }
      const auto at = std::find_if(
        index.quests.begin(), index.quests.end(),
        [&](const StoryQuest& q) { return SameForm(q.quest, quest); });
      if (at == index.quests.end()) {
        index.quests.push_back({ quest, name, 1 });
      } else {
        ++at->nodes;
      }
    }
  }
  return g_story.emplace(&worldState.GetEspm(), std::move(index))
    .first->second;
}
}

void PartOne::SetQuestScopeSettings(const std::string& modeJson,
                                    const std::string& playerQuestsJson)
{
  pImpl->questScope =
    skymp::rules::new_quest_scope(modeJson, playerQuestsJson);
  spdlog::info("PartOne::SetQuestScopeSettings - questScope {}, playerQuests "
               "{}",
               modeJson, playerQuestsJson);
  ComposeQuestScope();
}

void PartOne::SetScriptVariablesSettings(
  const std::string& conditionVariablesJson)
{
  pImpl->scriptVariables =
    skymp::rules::new_script_variables(conditionVariablesJson);
  spdlog::info("PartOne::SetScriptVariablesSettings - {} key(s)",
               pImpl->scriptVariables->script_variables_watch().size());
  ComposeQuestScope();
}

void PartOne::SetPlayerQuestSettings(const std::string& globalsJson,
                                     const std::string& variablesJson,
                                     const std::string& callersJson)
{
  pImpl->playerQuestGlobals = PlayerQuestMap<FormDesc>(
    "playerQuestGlobals", globalsJson, [](const nlohmann::json& v) {
      if (!v.is_string()) {
        throw std::runtime_error("playerQuestGlobals: " + v.dump() +
                                 " is not \"<hex id>:<plugin>\"");
      }
      return SettingFormDesc("playerQuestGlobals", v.get<std::string>());
    });
  pImpl->playerQuestVariables =
    PlayerQuestMap<std::pair<std::string, std::string>>(
      "playerQuestVariables", variablesJson, [](const nlohmann::json& v) {
        // "<script>::<var>", split at the first "::" (the census's
        // ownVariables and the lab grammar's spelling: the variable of an
        // auto property is "::<name>_var")
        const std::string text = v.is_string() ? v.get<std::string>() : "";
        const auto at = text.find("::");
        if (at == std::string::npos || at == 0 || at + 2 >= text.size()) {
          throw std::runtime_error("playerQuestVariables: " + v.dump() +
                                   " is not \"<script>::<var>\"");
        }
        return std::make_pair(text.substr(0, at), text.substr(at + 2));
      });
  pImpl->playerQuestCallers = PlayerQuestMap<uint16_t>(
    "playerQuestCallers", callersJson, [](const nlohmann::json& v) {
      if (!v.is_number_unsigned() || v.get<uint64_t>() > 0xffff) {
        throw std::runtime_error("playerQuestCallers: " + v.dump() +
                                 " is no stage (0 to 65535)");
      }
      return static_cast<uint16_t>(v.get<uint64_t>());
    });
  spdlog::info("PartOne::SetPlayerQuestSettings - {} quest(s) with globals, "
               "{} with variables, {} calling one",
               pImpl->playerQuestGlobals.size(),
               pImpl->playerQuestVariables.size(),
               pImpl->playerQuestCallers.size());
  ComposeQuestScope();
}

void PartOne::SetQuestStoryInstancesSettings(const std::string& settingJson)
{
  pImpl->questStoryWorld =
    skymp::rules::quest_story_instances_world(settingJson);
  spdlog::info("PartOne::SetQuestStoryInstancesSettings - "
               "questStoryInstances {}",
               settingJson);
  ComposeStoryQuests();
}

void PartOne::SetQuestPlayerAliasCauseSettings(const std::string& settingJson)
{
  pImpl->questPlayerAliasWorld =
    skymp::rules::quest_player_alias_cause_world(settingJson);
  spdlog::info("PartOne::SetQuestPlayerAliasCauseSettings - "
               "questPlayerAliasCause {}",
               settingJson);
}

bool PartOne::QuestPlayerAliasCauseSeat() const
{
  return !pImpl->questPlayerAliasWorld;
}

bool PartOne::ScriptRidesPlayerAliasesOnly(const std::string& script) const
{
  if (!pImpl->playerAliasScripts || script.empty()) {
    return false;
  }
  const auto it = pImpl->playerAliasScripts->find(LowerScriptClass(script));
  return it != pImpl->playerAliasScripts->end() && it->second;
}

void PartOne::ComposeStoryQuests()
{
  if (!HasEspm()) {
    return; // AttachEspm composes once the game files are here
  }
  const auto& index = StoryIndexOf(worldState);
  std::vector<FormDesc> quests;
  quests.reserve(index.quests.size());
  for (const auto& q : index.quests) {
    quests.push_back(q.quest);
  }
  const auto entries = QuestEntries(quests);
  pImpl->questRuns->quest_runs_set_story(
    rust::Slice<const skymp::rules::QuestStageEntry>(entries.data(),
                                                     entries.size()),
    pImpl->questStoryWorld);
  spdlog::info("[verb:quest-stage] the Story Manager's {} quest node(s) "
               "name {} quest(s) (SMQN NNAM): {} (questStoryInstances {}, "
               "D-Q4 C)",
               index.nodes, quests.size(),
               pImpl->questStoryWorld
                 ? "judged by their cause like any other quest"
                 : "their runs are each seat's own, their stages cross",
               pImpl->questStoryWorld ? "world" : "seat");
}

std::string PartOne::StoryNodesOf(const FormDesc& quest)
{
  if (!HasEspm()) {
    return std::string();
  }
  for (const auto& q : StoryIndexOf(worldState).quests) {
    if (SameForm(q.quest, quest)) {
      return q.nodes > 1
        ? fmt::format("{} and {} more", q.firstNode, q.nodes - 1)
        : q.firstNode;
    }
  }
  return std::string();
}

void PartOne::ComposeQuestScope()
{
  if (!HasEspm()) {
    return; // AttachEspm composes once the game files are here
  }
  const auto& facts = QuestScopeFactsOf(worldState);
  const bool acting = pImpl->questScope->quest_scope_acting();
  std::vector<FormDesc> marked;
  std::vector<const QuestScopeFacts*> markedFacts;
  for (const auto& q : facts) {
    const rust::Slice<const skymp::rules::QuestScopeAliasRow> aliases(
      q.aliases.data(), q.aliases.size());
    if (pImpl->questScope->quest_scope_class(q.quest.shortFormId, q.quest.file,
                                             aliases) == 1) {
      marked.push_back(q.quest);
      markedFacts.push_back(&q);
    }
  }

  // the measurement (D-Q1's "measured against today's lists"): the class and
  // its overlap with the denied list and the retired allowed one, either
  // mode, before anything is composed
  size_t denied = 0;
  size_t allowed = 0;
  std::string ids;
  for (const auto& q : marked) {
    if (pImpl->sharedQuests->shared_quests_check(q.shortFormId, q.file) == 2) {
      ++denied;
    }
    if (HoldsForm(pImpl->retiredAllowed, q)) {
      ++allowed;
    }
    ids += (ids.empty() ? "" : ", ") + q.ToString();
  }
  spdlog::info("[verb:quest-stage] quest scope ({}): {} per player: {}; {} "
               "denied, {} allowed",
               acting ? "derived" : "off", marked.size(), ids, denied,
               allowed);
  pImpl->questScopeMarked = marked;

  // the composition, while the scope acts ("off" leaves the settings as
  // written)
  pImpl->playerClassQuests = acting ? marked : std::vector<FormDesc>();
  const auto entries = QuestEntries(pImpl->playerClassQuests);
  const rust::Slice<const skymp::rules::QuestStageEntry> entrySlice(
    entries.data(), entries.size());
  pImpl->sharedQuests->shared_quests_set_player(entrySlice);

  // D-T2 B's own keys: each composed quest's scalar variables (the census's
  // playerQuestVariables) and its game-time registration's two names, keyed
  // by the quest's first variable's script or, with none listed, its first
  // attached one (the platform reads a registration by the form, whatever
  // the script)
  std::vector<skymp::rules::ScriptVarKeyRow> own;
  if (acting) {
    for (const QuestScopeFacts* q : markedFacts) {
      std::string script;
      for (const auto& [quest, variables] : pImpl->playerQuestVariables) {
        if (!SameForm(quest, q->quest)) {
          continue;
        }
        for (const auto& [scriptName, var] : variables) {
          if (script.empty()) {
            script = scriptName;
          }
          own.push_back({ q->quest.shortFormId, rust::String(q->quest.file),
                          rust::String(scriptName), rust::String(var), true });
        }
      }
      if (script.empty()) {
        script = q->firstScript;
      }
      if (script.empty()) {
        continue; // no script attached: nothing registers for game time
      }
      for (const char* name : { kGameTimeHours, kGameTimeRepeat }) {
        own.push_back({ q->quest.shortFormId, rust::String(q->quest.file),
                        rust::String(script), rust::String(name), true });
      }
    }
  }
  const size_t ownKept = pImpl->scriptVariables->script_variables_set_player(
    entrySlice,
    rust::Slice<const skymp::rules::ScriptVarKeyRow>(own.data(), own.size()));

  // PQ-3's stages
  pImpl->servedStages.clear();
  if (acting) {
    for (const auto& [quest, stages] : pImpl->playerQuestCallers) {
      for (uint16_t stage : stages) {
        pImpl->servedStages.emplace_back(quest, stage);
      }
    }
  }

  ComposePlayerGlobals();
  if (acting) {
    spdlog::info("[verb:quest-stage] quest scope composed: {} quest(s) per "
                 "player, {} own key(s) of {} named, {} stage(s) relayed "
                 "served (PQ-3)",
                 pImpl->playerClassQuests.size(), ownKept, own.size(),
                 pImpl->servedStages.size());
  }
}

void PartOne::ComposePlayerGlobals()
{
  // the player class's globals (playerQuestGlobals) join the setting's, each
  // once, so the player keeps them and WorldGlobal refuses them; nothing
  // joins while the scope does not act
  std::vector<std::string> added;
  if (!pImpl->playerClassQuests.empty() &&
      !pImpl->playerQuestGlobals.empty()) {
    const auto listed =
      nlohmann::json::parse(pImpl->playerGlobalsJson, nullptr, false);
    std::vector<FormDesc> have;
    if (listed.is_array()) {
      for (const auto& e : listed) {
        if (e.is_string()) {
          have.push_back(FormDesc::FromString(e.get<std::string>()));
        } else if (e.is_object() && e.contains("global") &&
                   e["global"].is_string()) {
          have.push_back(FormDesc::FromString(e["global"].get<std::string>()));
        }
      }
    }
    for (const auto& quest : pImpl->playerClassQuests) {
      for (const auto& [owner, globals] : pImpl->playerQuestGlobals) {
        if (!SameForm(owner, quest)) {
          continue;
        }
        for (const auto& global : globals) {
          if (!HoldsForm(have, global) && have.size() < kPlayerGlobalsMax) {
            have.push_back(global);
            added.push_back(global.ToString());
          }
        }
      }
    }
  }
  if (added.empty()) {
    pImpl->playerGlobals =
      skymp::rules::new_player_globals(pImpl->playerGlobalsJson);
    return;
  }
  auto merged =
    nlohmann::json::parse(pImpl->playerGlobalsJson, nullptr, false);
  if (!merged.is_array()) {
    merged = nlohmann::json::array();
  }
  for (const auto& global : added) {
    merged.push_back(global);
  }
  try {
    pImpl->playerGlobals = skymp::rules::new_player_globals(merged.dump());
    spdlog::info("[verb:quest-stage] quest scope: {} global(s) of the player "
                 "class kept per player (playerQuestGlobals)",
                 added.size());
  } catch (const std::exception& e) {
    spdlog::error("[verb:quest-stage] quest scope: the player class's {} "
                  "global(s) refused by playerGlobals ({}); the setting's "
                  "list stands",
                  added.size(), e.what());
    pImpl->playerGlobals =
      skymp::rules::new_player_globals(pImpl->playerGlobalsJson);
  }
}

const std::vector<FormDesc>& PartOne::QuestScopeMarked() const
{
  return pImpl->questScopeMarked;
}

const std::vector<FormDesc>& PartOne::PlayerClassQuests() const
{
  return pImpl->playerClassQuests;
}

bool PartOne::IsServedStage(const FormDesc& quest, uint16_t stage) const
{
  return std::any_of(pImpl->servedStages.begin(), pImpl->servedStages.end(),
                     [&](const auto& served) {
                       return served.second == stage &&
                         SameForm(served.first, quest);
                     });
}

skymp::rules::QuestRuns& PartOne::GetQuestRunRules()
{
  return *pImpl->questRuns;
}

skymp::rules::ScriptVariables& PartOne::GetScriptVariableRules()
{
  return *pImpl->scriptVariables;
}

skymp::rules::PlayerOverlays& PartOne::GetPlayerOverlays()
{
  return *pImpl->playerOverlays;
}

std::pair<std::vector<FormDesc>, std::vector<FormDesc>>
PartOne::QuestRunsAtLogin()
{
  std::vector<skymp::rules::QuestRunRow> record;
  for (const auto& run : GetQuestRuns()) {
    record.push_back({ run.quest.shortFormId, rust::String(run.quest.file),
                       run.running, run.seq });
  }
  // the journal's stages: a quest recorded stopped whose stages the login
  // replays is started ahead of them too (D-Q3 A)
  std::vector<skymp::rules::QuestStageEntry> stages;
  for (const auto& done : GetQuestStages()) {
    stages.push_back(
      { done.quest.shortFormId, rust::String(done.quest.file), done.stage });
  }
  const auto login = pImpl->questRuns->quest_runs_at_login(
    rust::Slice<const skymp::rules::QuestRunRow>(record.data(), record.size()),
    *pImpl->sharedQuests,
    rust::Slice<const skymp::rules::QuestStageEntry>(stages.data(),
                                                     stages.size()));
  std::pair<std::vector<FormDesc>, std::vector<FormDesc>> out;
  for (const auto& e : login.starts) {
    out.first.push_back(FormDesc(e.short_id, std::string(e.file)));
  }
  for (const auto& e : login.stops) {
    out.second.push_back(FormDesc(e.short_id, std::string(e.file)));
  }
  return out;
}

std::optional<RaceOverlay> PartOne::PlayerOverlayOf(uint32_t actorId) const
{
  const auto row = pImpl->playerOverlays->player_overlay(actorId);
  // an overlay over no recorded race (a player with no look) is never sent:
  // the wire's overlay names both races (wire-validate check_race_overlay)
  if (!row.open || row.race == 0 || row.base == 0) {
    return std::nullopt;
  }
  RaceOverlay overlay;
  overlay.open = true;
  overlay.race = row.race;
  overlay.base = row.base;
  overlay.spells.assign(row.spells.begin(), row.spells.end());
  overlay.shouts.assign(row.shouts.begin(), row.shouts.end());
  overlay.items.assign(row.items.begin(), row.items.end());
  return overlay;
}

const std::vector<PartOne::ScriptAttachment>& PartOne::ScriptAttachmentsOf(
  const std::string& scriptClass) const
{
  static const std::vector<ScriptAttachment> kNone;
  if (!pImpl->scriptAttachments) {
    return kNone;
  }
  const auto it =
    pImpl->scriptAttachments->find(LowerScriptClass(scriptClass));
  return it == pImpl->scriptAttachments->end() ? kNone : it->second;
}

bool PartOne::QuestAttachmentLive(uint32_t questId)
{
  if (!HasEspm()) {
    return false;
  }
  FormDesc quest;
  try {
    quest = FormDesc::FromFormId(questId, worldState.espmFiles);
  } catch (const std::exception&) {
    return false;
  }
  if (HoldsForm(pImpl->playerClassQuests, quest)) {
    return true;
  }
  const auto runs = GetQuestRuns();
  if (std::any_of(runs.begin(), runs.end(), [&](const auto& run) {
        return run.running && SameForm(run.quest, quest);
      })) {
    return true;
  }
  const auto stages = GetQuestStages();
  return std::any_of(stages.begin(), stages.end(), [&](const auto& stage) {
    return SameForm(stage.quest, quest);
  });
}

const std::vector<uint32_t>& PartOne::LeveledReach(uint32_t list)
{
  static const std::vector<uint32_t> kNone;
  if (!HasEspm() || list == 0) {
    return kNone;
  }
  auto it = pImpl->leveledReach.find(list);
  if (it == pImpl->leveledReach.end()) {
    auto& browser = GetEspm().GetBrowser();
    auto reach = LeveledListUtils::Reach(browser, browser.LookupById(list));
    // sorted, so a landing's check is a binary search (a boss chest's list
    // reaches thousands of bases)
    std::sort(reach.begin(), reach.end());
    it = pImpl->leveledReach.emplace(list, std::move(reach)).first;
  }
  return it->second;
}

bool PartOne::HeadPartFitsRace(uint32_t part, uint32_t race)
{
  if (!HasEspm() || race == 0) {
    return false;
  }
  auto& browser = GetEspm().GetBrowser();
  auto& cache = worldState.GetEspmCache();
  const auto lookup = browser.LookupById(part);
  const auto* hdpt = espm::Convert<espm::HDPT>(lookup.rec);
  if (!hdpt) {
    return false;
  }
  const uint32_t validRaces = hdpt->GetData(cache).validRaces;
  if (validRaces == 0) {
    return false; // a part that lists no race is no race's
  }
  const auto list = browser.LookupById(lookup.ToGlobalId(validRaces));
  const auto* flst = espm::Convert<espm::FLST>(list.rec);
  if (!flst) {
    return false;
  }
  for (uint32_t member : flst->GetData(cache).formIds) {
    if (list.ToGlobalId(member) == race) {
      return true;
    }
  }
  return false;
}

bool PartOne::WearHeadPart(Appearance& look, uint32_t part)
{
  if (!HasEspm()) {
    return false;
  }
  auto& browser = GetEspm().GetBrowser();
  auto& cache = worldState.GetEspmCache();
  const auto typeOf = [&](uint32_t id) -> std::optional<uint32_t> {
    const auto* hdpt = espm::Convert<espm::HDPT>(browser.LookupById(id).rec);
    return hdpt ? std::optional<uint32_t>(hdpt->GetData(cache).type)
                : std::nullopt;
  };
  const auto type = typeOf(part);
  if (!type ||
      std::find(look.headpartIds.begin(), look.headpartIds.end(), part) !=
        look.headpartIds.end()) {
    return false;
  }
  auto& parts = look.headpartIds;
  parts.erase(
    std::remove_if(parts.begin(), parts.end(),
                   [&](uint32_t worn) { return typeOf(worn) == type; }),
    parts.end());
  parts.push_back(part);
  return true;
}

uint32_t PartOne::LookFormIdOf(const std::string& plugin, uint32_t id) const
{
  const auto& files = worldState.espmFiles;
  for (const auto& file : files) {
    if (file.size() == plugin.size() &&
        std::equal(file.begin(), file.end(), plugin.begin(),
                   [](char a, char b) {
                     return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
                   })) {
      return FormDesc(id & 0x00ffffff, file).ToFormId(files);
    }
  }
  return 0;
}

bool PartOne::WearHeadPartInLook(std::string& look, uint32_t part)
{
  if (!HasEspm() || look.empty()) {
    return false;
  }
  auto& browser = GetEspm().GetBrowser();
  auto& cache = worldState.GetEspmCache();
  const auto typeOf = [&](uint32_t id) -> std::optional<uint32_t> {
    const auto* hdpt = espm::Convert<espm::HDPT>(browser.LookupById(id).rec);
    return hdpt ? std::optional<uint32_t>(hdpt->GetData(cache).type)
                : std::nullopt;
  };
  const auto type = typeOf(part);
  if (!type) {
    return false;
  }
  const auto facts = skymp::rules::racemenu_look_facts(rust::Str(look));
  if (!facts.ok) {
    return false;
  }
  // one byte a listed part, in the look's order: of `part`'s type or not
  std::vector<uint8_t> sameType;
  sameType.reserve(facts.parts.size());
  for (const auto& ref : facts.parts) {
    const uint32_t listed = LookFormIdOf(std::string(ref.plugin), ref.id);
    sameType.push_back(listed != 0 && typeOf(listed) == type ? 1 : 0);
  }
  FormDesc desc;
  try {
    desc = FormDesc::FromFormId(part, worldState.espmFiles);
  } catch (const std::exception&) {
    return false; // a part in no loaded plugin
  }
  const auto rewritten = skymp::rules::racemenu_with_head_part(
    rust::Str(look),
    rust::Slice<const uint8_t>(sameType.data(), sameType.size()),
    rust::Str(desc.file), desc.shortFormId);
  if (rewritten.empty()) {
    return false;
  }
  look = std::string(rewritten);
  return true;
}

PartOne::HeadPartRecord PartOne::RecordHeadPart(MpActor& actor, uint32_t part)
{
  HeadPartRecord recorded;
  if (const auto current = actor.GetAppearance()) {
    Appearance look = *current;
    if (WearHeadPart(look, part)) {
      actor.SetAppearance(&look);
      recorded.look = true;
    }
  }
  std::string preset = actor.GetRaceMenuPreset();
  if (WearHeadPartInLook(preset, part) &&
      actor.SetRaceMenuPreset(std::move(preset))) {
    recorded.preset = true;
  }
  return recorded;
}

void PartOne::SetRestrictedRacesSettings(
  const std::string& restrictedRacesJson)
{
  const auto j = nlohmann::json::parse(restrictedRacesJson);
  if (!j.is_object()) {
    throw std::runtime_error("restrictedRaces must be an object: "
                             "{\"<shortid>:<plugin>\": [profile ids]}");
  }
  std::map<FormDesc, std::vector<int32_t>> table;
  for (const auto& [key, profiles] : j.items()) {
    // "<shortid>:<plugin>": one to six hex digits (FormDesc::FromString
    // checks nothing, so "zzz:x.esp" would read as id 0), then the plugin's
    // file name, kept in lower case so two spellings of one plugin are one
    // key (the engine compares names without case)
    const auto colon = key.find(':');
    const std::string idPart =
      colon == std::string::npos ? std::string() : key.substr(0, colon);
    std::string file =
      colon == std::string::npos ? std::string() : key.substr(colon + 1);
    const bool hexId = !idPart.empty() && idPart.size() <= 6 &&
      std::all_of(idPart.begin(), idPart.end(),
                  [](unsigned char c) { return std::isxdigit(c) != 0; });
    if (!hexId || file.empty() || !profiles.is_array()) {
      throw std::runtime_error("restrictedRaces: bad entry \"" + key +
                               "\" (want \"<shortid>:<plugin>\": [profile "
                               "ids], the short id one to six hex digits)");
    }
    const auto shortId =
      static_cast<uint32_t>(std::stoul(idPart, nullptr, 16));
    if (shortId == 0) {
      throw std::runtime_error("restrictedRaces: \"" + key +
                               "\" names no form (short id 0)");
    }
    std::transform(file.begin(), file.end(), file.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    auto& list = table[FormDesc(shortId, file)];
    for (const auto& profile : profiles) {
      if (!profile.is_number_integer()) {
        throw std::runtime_error("restrictedRaces: \"" + key +
                                 "\" lists a profile id that is no integer");
      }
      list.push_back(profile.get<int32_t>());
    }
  }
  worldState.restrictedRaces = std::move(table);
  spdlog::info("PartOne::SetRestrictedRacesSettings - {} race(s): {}",
               worldState.restrictedRaces.size(), restrictedRacesJson);
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

void PartOne::SetWorldGlobalsEnabled(bool on)
{
  pImpl->worldGlobalsEnabled = on;
  spdlog::info("[verb:world-globals] world globals {}", on ? "on" : "off");
}

bool PartOne::WorldGlobalsEnabled() const
{
  return pImpl->worldGlobalsEnabled;
}

void PartOne::WorldStreamerMoved(MpActor& actor)
{
  // off, nobody is ever eligible, so the seats' later events elect nobody
  if (!pImpl->worldGlobalsEnabled) {
    return;
  }
  SendStreamWords(*this, pImpl->worldStreamer->moved(actor.GetFormId()));
}

uint8_t PartOne::JudgeWorldGlobalReport(uint32_t sender, uint32_t epoch) const
{
  return pImpl->worldStreamer->judge(sender, epoch);
}

std::pair<uint32_t, uint32_t> PartOne::WorldStreamer() const
{
  const auto now = pImpl->worldStreamer->streamer_now();
  return { now.streamer, now.epoch };
}

std::vector<PlayerGlobal> PartOne::GetWorldGlobals()
{
  // a read makes no journal (GetAliasFills says why)
  if (!worldState.LookupFormById(kWorldJournalRefId)) {
    return {};
  }
  return WorldJournal().GetWorldGlobals();
}

bool PartOne::SetWorldGlobals(std::vector<PlayerGlobal> globals)
{
  return WorldJournal().SetWorldGlobals(std::move(globals));
}

std::vector<MpChangeFormREFR::QuestRun> PartOne::GetQuestRuns()
{
  if (!worldState.LookupFormById(kWorldJournalRefId)) {
    return {};
  }
  return WorldJournal().GetQuestRuns();
}

bool PartOne::SetQuestRuns(std::vector<MpChangeFormREFR::QuestRun> runs)
{
  if (runs.empty() && !worldState.LookupFormById(kWorldJournalRefId)) {
    return false; // nothing to write, and no journal made for it
  }
  return WorldJournal().SetQuestRuns(std::move(runs));
}

std::vector<MpChangeFormREFR::ScriptVariable> PartOne::GetScriptVariables()
{
  if (!worldState.LookupFormById(kWorldJournalRefId)) {
    return {};
  }
  return WorldJournal().GetScriptVariables();
}

bool PartOne::SetScriptVariables(
  std::vector<MpChangeFormREFR::ScriptVariable> variables)
{
  if (variables.empty() && !worldState.LookupFormById(kWorldJournalRefId)) {
    return false; // nothing to write, and no journal made for it
  }
  return WorldJournal().SetScriptVariables(std::move(variables));
}

std::vector<MpChangeFormREFR::AliasFill> PartOne::GetAliasFills()
{
  // a read makes no journal: the login's serve reads the fills on every
  // server, and a journal made there would take the idx of the actor the
  // next login creates (PartOne_MovementTest's idx 1, pipeline 5600); the
  // first write makes it
  if (!worldState.LookupFormById(kWorldJournalRefId)) {
    return {};
  }
  return WorldJournal().GetAliasFills();
}

bool PartOne::SetAliasFills(std::vector<MpChangeFormREFR::AliasFill> fills)
{
  return WorldJournal().SetAliasFills(std::move(fills));
}

namespace {
// thuum ADR-068 rung 4 (amendment B, read 8): one created alias's item as
// the game files give it, the forms in the load order; containerKind is the
// rules' (0 unresolved, 1 a forced reference, 2 a unique actor's base)
struct AliasCreation
{
  uint32_t quest = 0;
  uint32_t alias = 0;
  uint32_t base = 0;
  uint32_t target = 0;
  uint8_t containerKind = 0;
  uint32_t container = 0;
  bool startGameEnabled = false;
  std::string why;
};

// every quest's created aliases, read once per set of game files: a scan of
// the QUST records (their aliases, libespm QUST::GetQuestAliases; a quest's
// DNAM read only where an alias creates), the creation and its container
// the rule's (wire-rules alias_fills::creations). The index is the files',
// so the process keeps one per loader, which the tests' many PartOnes share
struct AliasCreationIndex
{
  std::unordered_multimap<uint32_t, AliasCreation> byForcedRef;
  std::unordered_multimap<uint32_t, AliasCreation> byUniqueActor;
  std::unordered_map<uint32_t, std::vector<AliasCreation>> byQuest;
};

const AliasCreationIndex& AliasCreationsOf(WorldState& worldState)
{
  static std::map<const espm::Loader*, std::unique_ptr<AliasCreationIndex>>
    g_indices;
  auto& slot = g_indices[&worldState.GetEspm()];
  if (slot) {
    return *slot;
  }
  slot = std::make_unique<AliasCreationIndex>();
  auto& index = *slot;
  auto& cache = worldState.GetEspmCache();
  size_t resolved = 0;
  size_t unresolved = 0;
  for (const auto& lookup :
       worldState.GetEspm().GetBrowser().GetDistinctRecordsByType("QUST")) {
    const auto* qust = espm::Convert<espm::QUST>(lookup.rec);
    if (!qust) {
      continue;
    }
    std::vector<skymp::rules::QuestAliasRow> rows;
    bool creates = false;
    for (const auto& alias : qust->GetQuestAliases(cache)) {
      if (alias.locationAlias) {
        continue;
      }
      skymp::rules::QuestAliasRow row{};
      row.id = alias.aliasID;
      row.created = alias.aliasCreatedObject
        ? lookup.ToGlobalId(alias.aliasCreatedObject)
        : 0;
      row.create_at = alias.createAt;
      row.forced = alias.aliasForcedReference
        ? lookup.ToGlobalId(alias.aliasForcedReference)
        : 0;
      row.unique_actor =
        alias.aliasUniqueActor ? lookup.ToGlobalId(alias.aliasUniqueActor) : 0;
      creates = creates || row.created != 0;
      rows.push_back(row);
    }
    if (!creates) {
      continue;
    }
    const auto made = skymp::rules::alias_creations(
      rust::Slice<const skymp::rules::QuestAliasRow>(rows.data(),
                                                     rows.size()));
    const uint32_t questId = lookup.ToGlobalId(lookup.rec->GetId());
    const auto data = qust->GetData(cache);
    const bool startGameEnabled = data.questData &&
      (data.questData->mainFlags & espm::QUST::QuestData::StartGameEnabled);
    for (const auto& row : made) {
      AliasCreation creation;
      creation.quest = questId;
      creation.alias = row.alias;
      creation.base = row.base;
      creation.target = row.target;
      creation.containerKind = row.container_kind;
      creation.container = row.container;
      creation.startGameEnabled = startGameEnabled;
      creation.why = std::string(row.why);
      if (creation.containerKind == 1) {
        index.byForcedRef.emplace(creation.container, creation);
        ++resolved;
      } else if (creation.containerKind == 2) {
        index.byUniqueActor.emplace(creation.container, creation);
        ++resolved;
      } else {
        ++unresolved;
      }
      index.byQuest[questId].push_back(std::move(creation));
    }
  }
  spdlog::info("[verb:quest-stage] the quests' created aliases: {} resolved "
               "to a container, {} unresolved (E_ALIASFILL_UNRESOLVED, each "
               "logged once when its quest fills)",
               resolved, unresolved);
  return index;
}

// the journal's marks as the rule reads them: an entry whose created item
// names a quest the running order resolves
std::vector<skymp::rules::AliasMarkRow> CreatedMarks(
  const std::vector<MpChangeFormREFR::AliasFill>& fills,
  const EspmFileList& files)
{
  std::vector<skymp::rules::AliasMarkRow> marks;
  for (const auto& fill : fills) {
    if (!fill.created) {
      continue;
    }
    try {
      marks.push_back({ fill.quest.ToFormId(files), fill.alias, fill.added });
    } catch (const std::exception&) {
      // a plugin the order lacks: its mark names no quest of this world
    }
  }
  return marks;
}
}

namespace {
bool AddCreatedItem(PartOne& partOne, MpObjectReference& container,
                    const AliasCreation& creation)
{
  auto fills = partOne.GetAliasFills();
  const auto marks = CreatedMarks(fills, partOne.worldState.espmFiles);
  if (!skymp::rules::alias_created_due(
        rust::Slice<const skymp::rules::AliasMarkRow>(marks.data(),
                                                      marks.size()),
        creation.quest, creation.alias)) {
    return false;
  }
  const auto& files = partOne.worldState.espmFiles;
  FormDesc quest;
  FormDesc created;
  FormDesc where;
  try {
    quest = FormDesc::FromFormId(creation.quest, files);
    created = FormDesc::FromFormId(creation.base, files);
    where = FormDesc::FromFormId(container.GetFormId(), files);
  } catch (const std::exception& e) {
    spdlog::warn("[verb:quest-stage] alias {:x}/{} creates {:x} in {:x}: {}; "
                 "not added",
                 creation.quest, creation.alias, creation.base,
                 container.GetFormId(), e.what());
    return false;
  }
  container.AddItem(creation.base, 1);
  auto entry = std::find_if(fills.begin(), fills.end(), [&](const auto& fill) {
    return fill.quest == quest && fill.alias == creation.alias;
  });
  if (entry == fills.end()) {
    MpChangeFormREFR::AliasFill mark;
    mark.quest = quest;
    mark.alias = creation.alias;
    fills.push_back(std::move(mark));
    entry = std::prev(fills.end());
  }
  entry->created = created;
  entry->container = where;
  entry->added = true;
  partOne.SetAliasFills(std::move(fills));
  spdlog::info("[verb:quest-stage] alias {:x}/{} creates {:x} in {:x}: "
               "added once",
               creation.quest, creation.alias, creation.base,
               container.GetFormId());
  return true;
}
}

size_t PartOne::AddCreatedItems(MpObjectReference& container)
{
  if (!HasEspm() || container.GetFormId() >= 0xff000000) {
    return 0;
  }
  const auto& index = AliasCreationsOf(worldState);
  std::vector<const AliasCreation*> found;
  const auto collect =
    [&](const std::unordered_multimap<uint32_t, AliasCreation>& map,
        uint32_t key) {
      const auto [begin, end] = map.equal_range(key);
      for (auto it = begin; it != end; ++it) {
        found.push_back(&it->second);
      }
    };
  collect(index.byForcedRef, container.GetFormId());
  if (container.AsActor()) {
    const size_t before = found.size();
    collect(index.byUniqueActor, container.GetBaseId());
    if (found.size() != before) {
      pImpl->uniqueActorRefs[container.GetBaseId()] = container.GetFormId();
    }
  }
  if (found.empty()) {
    return 0;
  }
  // a start-game-enabled quest fills from the game's start; any other once
  // the world records a stage of it (its start, as near as the record knows)
  const auto stages = GetQuestStages();
  size_t added = 0;
  for (const AliasCreation* creation : found) {
    if (!creation->startGameEnabled) {
      const bool started =
        std::any_of(stages.begin(), stages.end(), [&](const auto& stage) {
          try {
            return stage.quest.ToFormId(worldState.espmFiles) ==
              creation->quest;
          } catch (const std::exception&) {
            return false;
          }
        });
      if (!started) {
        continue;
      }
    }
    if (AddCreatedItem(*this, container, *creation)) {
      ++added;
    }
  }
  return added;
}

size_t PartOne::AddCreatedItemsOfQuest(uint32_t questId)
{
  if (!HasEspm()) {
    return 0;
  }
  const auto& index = AliasCreationsOf(worldState);
  const auto it = index.byQuest.find(questId);
  if (it == index.byQuest.end()) {
    return 0;
  }
  size_t added = 0;
  for (const auto& creation : it->second) {
    if (creation.containerKind == 0) {
      if (pImpl->unresolvedCreationsLogged
            .insert({ creation.quest, creation.alias })
            .second) {
        spdlog::info("[verb:quest-stage] E_ALIASFILL_UNRESOLVED: alias "
                     "{:x}/{} creates {:x} in alias {}: {}; the item stays "
                     "each seat's own",
                     creation.quest, creation.alias, creation.base,
                     creation.target, creation.why);
      }
      continue;
    }
    // the container already attached; one attached later takes its item at
    // its own attach (AddCreatedItems), the quest now recorded
    uint32_t containerId = creation.container;
    if (creation.containerKind == 2) {
      const auto ref = pImpl->uniqueActorRefs.find(creation.container);
      if (ref == pImpl->uniqueActorRefs.end()) {
        continue;
      }
      containerId = ref->second;
    }
    const auto& form = worldState.LookupFormByIdNoLoad(containerId);
    MpObjectReference* container = form ? form->AsObjectReference() : nullptr;
    if (container && AddCreatedItem(*this, *container, creation)) {
      ++added;
    }
  }
  return added;
}

namespace {
// the eighteen skills' AVIF records, one per ActorValue 6 to 23 (Skyrim.esm as
// lab/esm.py read it, 2026-10-31: AVOneHanded 0x44c through AVEnchanting
// 0x45d, in the engine's order)
constexpr uint32_t kFirstSkillAvif = 0x44c;
constexpr uint8_t kFirstSkillAv = 6;
constexpr uint8_t kSkillCount = 18;
}

void PartOne::LoadSkillTable()
{
  if (!HasEspm()) {
    return;
  }
  int fromFiles = 0;
  for (uint8_t i = 0; i < kSkillCount; ++i) {
    const uint32_t formId = kFirstSkillAvif + i;
    const uint8_t skillAv = static_cast<uint8_t>(kFirstSkillAv + i);
    const auto lookup = GetEspm().GetBrowser().LookupById(formId);
    if (!lookup.rec || lookup.rec->GetType() != "AVIF") {
      spdlog::warn("[verb:leveling] skill {}: no AVIF at {:x}; UESP's "
                   "constants stand",
                   skillAv, formId);
      continue;
    }
    const auto data = espm::GetData<espm::AVIF>(formId, &worldState);
    if (!data.hasSkill) {
      spdlog::warn("[verb:leveling] skill {}: AVIF {:x} ({}) carries no AVSK; "
                   "UESP's constants stand",
                   skillAv, formId, data.editorId);
      continue;
    }
    if (pImpl->leveling->set_skill(
          skillAv, data.skill.useMult, data.skill.useOffset,
          data.skill.improveMult, data.skill.improveOffset)) {
      ++fromFiles;
    }
  }
  spdlog::info("[verb:leveling] skill table: {} of {} skills from the game "
               "files",
               fromFiles, kSkillCount);
}

void PartOne::SetLevelingEnabled(bool on)
{
  pImpl->levelingEnabled = on;
  spdlog::info("[verb:leveling] leveling {}", on ? "on" : "off");
}

void PartOne::SetPropsPersist(bool on)
{
  worldState.propsPersist = on;
  spdlog::info("[verb:prop-physics] dropped items {}",
               on ? "persist as props" : "follow upstream's clutter policies");
}

bool PartOne::PropsPersist() const
{
  return worldState.propsPersist;
}

bool PartOne::LevelingEnabled() const
{
  return pImpl->levelingEnabled;
}

float PartOne::SkillUseXp(uint8_t skillAv, float base) const
{
  return pImpl->leveling->use_xp(skillAv, base);
}

float PartOne::SkillLevelCost(uint8_t skillAv, uint8_t level) const
{
  return pImpl->leveling->level_cost(skillAv, level);
}

namespace {
// the record's standing in the rule's shape: the skills it lists (by their
// ActorValue; one not listed stands at the floor) and the character
std::pair<std::vector<skymp::rules::LevelingSkill>,
          skymp::rules::LevelingCharacter>
LevelingStanding(const ActorValueRecord& record)
{
  std::vector<skymp::rules::LevelingSkill> skills;
  for (const auto& s : record.skills) {
    skymp::rules::LevelingSkill standing{};
    standing.skill_av = static_cast<uint8_t>(s.skill + kFirstSkillAv);
    standing.level = static_cast<uint8_t>(std::clamp(s.level, 0.f, 255.f));
    standing.xp = s.xp;
    skills.push_back(standing);
  }
  skymp::rules::LevelingCharacter character{};
  character.level = record.level;
  character.xp = record.xp;
  return { std::move(skills), character };
}

const char* LevelingRefusal(uint8_t why)
{
  static const char* const kWhy[] = { "accepted", "E_SKILL_NONE",
                                      "E_SKILL_NAN", "E_SKILL_RANGE" };
  return why < 4 ? kWhy[why] : "E_SKILL_UNKNOWN";
}

// the judged standing into the record: the skill's progress entry (by the
// engine's skill index) and its base (the level the game shows), the
// character; then the record to the actor and the events to the log. The
// record goes to the game when a level moved, or always with `sendAlways`
// (a skill book's read: the seat's own increment is replaced by this number)
void WriteLevelingOutcome(MpActor& actor, ActorValueRecord& record,
                          uint8_t skillAv,
                          const skymp::rules::LevelingOutcome& outcome,
                          const skymp::rules::LevelingTable& leveling,
                          bool perksEnabled, bool sendAlways)
{
  const uint8_t skillIndex = static_cast<uint8_t>(skillAv - kFirstSkillAv);
  for (const auto& s : outcome.skills) {
    if (s.skill_av != skillAv) {
      continue;
    }
    const float threshold = leveling.level_cost(s.skill_av, s.level);
    auto it = std::find_if(
      record.skills.begin(), record.skills.end(),
      [&](const ActorValueRecord::Skill& r) { return r.skill == skillIndex; });
    if (it == record.skills.end()) {
      record.skills.push_back(ActorValueRecord::Skill{
        skillIndex, static_cast<float>(s.level), s.xp, threshold });
    } else {
      it->level = static_cast<float>(s.level);
      it->xp = s.xp;
      it->threshold = threshold;
    }
    auto base = std::find_if(
      record.bases.begin(), record.bases.end(),
      [&](const std::pair<uint8_t, float>& b) { return b.first == skillAv; });
    if (base == record.bases.end()) {
      record.bases.emplace_back(skillAv, static_cast<float>(s.level));
    } else {
      base->second = static_cast<float>(s.level);
    }
  }
  record.level = outcome.character.level;
  record.xp = outcome.character.xp;
  record.threshold =
    skymp::rules::leveling_character_threshold(outcome.character.level);
  actor.SetActorValueRecord(record);
  for (const auto& e : outcome.events) {
    if (e.kind == 0) {
      spdlog::info("[verb:leveling] {:x} skill {} reaches level {}",
                   actor.GetFormId(), e.skill_av, e.level);
    } else {
      spdlog::info("[verb:leveling] {:x} reaches character level {}",
                   actor.GetFormId(), e.level);
    }
  }
  if (sendAlways || !outcome.events.empty()) {
    // the game shows the new level and the bar from the record (R0)
    actor.SendActorValueRecord();
    // a level moves the perk points (thuum docs/verbs/perks.md)
    if (perksEnabled && !outcome.events.empty()) {
      actor.SendPerkState();
    }
  }
}
}

bool PartOne::ApplySkillUse(MpActor& actor, uint8_t skillAv, float amount)
{
  auto record = actor.GetActorValueRecord();
  if (!record) {
    spdlog::info("[verb:leveling] E_SKILL_NO_RECORD: {:x} uses skill {} "
                 "before its first actor values report; dropped",
                 actor.GetFormId(), skillAv);
    return false;
  }
  const auto [skills, character] = LevelingStanding(*record);
  const rust::Slice<const skymp::rules::LevelingSkill> slice(skills.data(),
                                                             skills.size());
  const auto outcome =
    pImpl->leveling->apply_use(slice, character, skillAv, amount);
  if (!outcome.accepted) {
    spdlog::warn("[verb:leveling] {}: {:x} uses skill {} with {}; dropped",
                 LevelingRefusal(outcome.why), actor.GetFormId(), skillAv,
                 amount);
    return false;
  }
  WriteLevelingOutcome(actor, *record, skillAv, outcome, *pImpl->leveling,
                       pImpl->perksEnabled, false);
  return true;
}

bool PartOne::ApplySkillBook(MpActor& actor, uint8_t skillAv)
{
  if (!pImpl->levelingEnabled) {
    spdlog::debug("[verb:leveling] {:x} reads a skill book for skill {} with "
                  "leveling off; dropped",
                  actor.GetFormId(), skillAv);
    return false;
  }
  auto record = actor.GetActorValueRecord();
  if (!record) {
    spdlog::info("[verb:leveling] E_SKILL_NO_RECORD: {:x} reads a skill book "
                 "for skill {} before its first actor values report; dropped",
                 actor.GetFormId(), skillAv);
    return false;
  }
  const auto [skills, character] = LevelingStanding(*record);
  const rust::Slice<const skymp::rules::LevelingSkill> slice(skills.data(),
                                                             skills.size());
  const auto outcome = pImpl->leveling->apply_book(slice, character, skillAv);
  if (!outcome.accepted) {
    spdlog::warn("[verb:leveling] {}: {:x} reads a skill book for skill {}; "
                 "dropped",
                 LevelingRefusal(outcome.why), actor.GetFormId(), skillAv);
    return false;
  }
  if (outcome.events.empty()) {
    spdlog::info("[verb:leveling] {:x} reads a skill book for skill {} at the "
                 "cap: the book is spent, nothing moves",
                 actor.GetFormId(), skillAv);
  }
  WriteLevelingOutcome(actor, *record, skillAv, outcome, *pImpl->leveling,
                       pImpl->perksEnabled, true);
  return true;
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

bool PartOne::OpenScriptSet(MpActor& reporter, uint32_t selfId,
                            uint32_t worldOrCell, const NiPoint3& anchor,
                            uint32_t baseId)
{
  FormDesc baseDesc;
  try {
    baseDesc = FormDesc::FromFormId(baseId, worldState.espmFiles);
  } catch (const std::exception& e) {
    spdlog::info("[verb:spell-placed] E_PLACED_BASE: {:x}'s script placed "
                 "{:x} at {:x}: {}; nothing placed",
                 reporter.GetFormId(), baseId, selfId, e.what());
    return false;
  }
  std::string baseType;
  if (HasEspm()) {
    if (const auto* rec = GetEspm().GetBrowser().LookupById(baseId).rec) {
      baseType = rec->GetType().ToString();
    }
  }
  const auto opened = pImpl->placements->open_script_set(
    reporter.GetFormId(), selfId, anchor.x, anchor.y, anchor.z,
    baseDesc.shortFormId, baseDesc.file, baseType, SteadyNowMs());
  if (opened.refusal != 0) {
    const char* code = opened.refusal == 1 ? "E_PLACED_KIND"
      : opened.refusal == 2                ? "E_PLACED_FULL"
                                           : "E_PLACED_NAN";
    spdlog::info("[verb:spell-placed] {}: {:x}'s script placed {} ({}) at "
                 "{:x}; nothing placed",
                 code, reporter.GetFormId(), baseDesc.ToString(), baseType,
                 selfId);
    return false;
  }
  pImpl->scriptSetWorldOrCell[opened.set] = worldOrCell;
  for (const auto& row : pImpl->placements->take_script_requests()) {
    PlacedRequestMessage ask;
    ask.set = row.set;
    ask.windowMs =
      static_cast<uint32_t>(std::min<uint64_t>(row.window_ms, 10'000));
    const FormDesc base(row.short_id, std::string(row.file));
    try {
      ask.bases.push_back(base.ToFormId(worldState.espmFiles));
    } catch (const std::exception&) {
      continue;
    }
    const auto userId = serverState.UserByActor(&reporter);
    if (userId == Networking::InvalidUserId) {
      continue;
    }
    GetSendTarget().Send(userId, ask, true);
    spdlog::info("[verb:spell-placed] {:x}'s script placed {} ({}) at {:x}: "
                 "set {} requested within {} ms",
                 row.caster, base.ToString(), std::string(row.record_type),
                 row.self_id, row.set, ask.windowMs);
  }
  return true;
}

namespace {
// thuum ADR-068 rung 4 (the script set): the server's copy of a script's
// placement at its reporter's pose, through PlaceAt (an NPC fated before its
// create word), marked with its placer and its record before its first
// subscription so its create word names the reporter
uint32_t PlaceScriptObject(PartOne& partOne,
                           const skymp::rules::ScriptPlacementRow& row,
                           uint32_t worldOrCell,
                           std::unordered_map<uint32_t, uint32_t>& placers)
{
  auto& worldState = partOne.worldState;
  const auto& reporterForm = worldState.LookupFormByIdNoLoad(row.caster);
  MpActor* reporter = reporterForm ? reporterForm->AsActor() : nullptr;
  if (!reporter || !partOne.HasEspm() || worldOrCell == 0) {
    return 0;
  }
  const FormDesc baseDesc(row.short_id, std::string(row.file));
  uint32_t baseId = 0;
  FormDesc where;
  FormDesc self;
  try {
    baseId = baseDesc.ToFormId(worldState.espmFiles);
    where = FormDesc::FromFormId(worldOrCell, worldState.espmFiles);
    self = FormDesc::FromFormId(row.self_id, worldState.espmFiles);
  } catch (const std::exception&) {
    return 0;
  }
  const auto lookup = partOne.GetEspm().GetBrowser().LookupById(baseId);
  if (!lookup.rec) {
    return 0;
  }
  auto& refr = PapyrusObjectReference::PlaceAt(
    *reporter, lookup,
    LocationalData{ { row.x, row.y, row.z }, { 0.f, 0.f, row.yaw }, where },
    false);
  placers[refr.GetFormId()] = row.caster;
  refr.SetPlacedByScript(self, baseDesc);
  refr.ForceSubscriptionsUpdate();
  return refr.GetFormId();
}
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
  // thuum ADR-068 rung 4 (the script set): a report placed, or a window
  // closed with none
  for (const auto& row : pImpl->placements->due_script_placements(nowMs)) {
    const auto where = pImpl->scriptSetWorldOrCell.find(row.set);
    const uint32_t worldOrCell =
      where != pImpl->scriptSetWorldOrCell.end() ? where->second : 0;
    pImpl->scriptSetWorldOrCell.erase(row.set);
    const FormDesc base(row.short_id, std::string(row.file));
    if (!row.placed) {
      spdlog::info("[verb:spell-placed] {:x}'s script placed {} at {:x}: "
                   "nothing placed, its game reported no base within the "
                   "window",
                   row.caster, base.ToString(), row.self_id);
      continue;
    }
    const uint32_t formId =
      PlaceScriptObject(*this, row, worldOrCell, pImpl->placedCasterByForm);
    spdlog::info("[verb:spell-placed] {:x}'s script placed {} ({}) at {:x}: "
                 "{:x} at ({:.0f}, {:.0f}, {:.0f}) yaw {:.0f}, as reported",
                 row.caster, base.ToString(), std::string(row.record_type),
                 row.self_id, formId, row.x, row.y, row.z, row.yaw);
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

// thuum D-T4 (ADR-028; the review of fork e4c78077, its finding 2d): the proc
// relayed below as a seat's own self-cast passes both the client's refusal of
// an actor caster's Health, Magicka and Stamina effects (the caster is the
// seat's own player) and the platform's cut of another actor's damage (the
// attacker is the victim itself), so it must never carry a Health, Magicka or
// Stamina damage effect: concealment alone, today
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
  // instant caster (castingSource 3), in every game that sees it, the
  // caster's own included since the mod's script is blocked everywhere
  // (Skyrim Platform's never-admitted list, m2-shadowbond-script-block,
  // 2026-11-01: until then the caster's game ran the script, which cast both
  // procs itself, and the relay skipped it; b-shadowbond 20261102-023146 read
  // the caster's game never invisible once the script was gone). An
  // observer's game casts it on the actor's figure through
  // castSpellImmediate's Self branch, the primitive b-invis-recast proves
  // nightly; a game whose own player the cast names reads the caster as 0x14
  // (remoteServer.ts, casterIsMe). A Papyrus Spell.Cast snippet completed in
  // the target's game and applied nothing (b-shadowbond 20261019-215816,
  // playtest twenty-five), as the driver's own Spell.Cast never applied
  // anything either (push.md); no cast of the proc comes back to the server
  // (Skyrim Platform reports only a cast of an equipped spell), so it is
  // never an act against the bond
  int games = 0;
  for (MpActor* actor : { caster.get(), target.get() }) {
    SpellCastMessage cast;
    cast.data.caster = actor->GetFormId();
    cast.data.target = actor->GetFormId();
    cast.data.spell = procId;
    cast.data.castingSource = 3; // MagicSystem::CastingSource::kInstant
    for (auto* listener : actor->GetActorListeners()) {
      if (serverState.UserByActor(listener) == Networking::InvalidUserId) {
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

// thuum docs/verbs/weather.md (M4): the server's weather

namespace {
// A weather setting's form reference: a number (a global form id), a hex
// string ("0x81a", the tests' and the lab's shorthand for Skyrim.esm's own
// ids) or a form descriptor ("81a:Skyrim.esm"), which needs the load order
// (0 before the game files are attached; the caller waits for them).
uint32_t WeatherFormRef(const nlohmann::json& value,
                        const EspmFileList& espmFiles, const std::string& what)
{
  if (value.is_number_unsigned()) {
    return value.get<uint32_t>();
  }
  if (value.is_string()) {
    const auto text = value.get<std::string>();
    if (text.find(':') != std::string::npos) {
      return espmFiles.empty()
        ? 0
        : FormDesc::FromString(text).ToFormId(espmFiles);
    }
    try {
      return static_cast<uint32_t>(std::stoul(text, nullptr, 16));
    } catch (const std::exception&) {
    }
  }
  throw std::runtime_error(
    fmt::format("weather: {} must be a form id (a number, \"0x81a\" or "
                "\"81a:Skyrim.esm\"), got {}",
                what, value.dump()));
}

// Whether any pool names a form descriptor, which only the load order
// resolves.
bool WeatherPoolsNeedLoadOrder(const nlohmann::json& pools)
{
  for (const auto& [world, list] : pools.items()) {
    if (world.find(':') != std::string::npos) {
      return true;
    }
    for (const auto& entry : list) {
      const auto& weather = entry.at("weather");
      if (weather.is_string() &&
          weather.get<std::string>().find(':') != std::string::npos) {
        return true;
      }
    }
  }
  return false;
}
}

void PartOne::SetWeatherEnabled(bool on)
{
  pImpl->weatherEnabled = on;
  spdlog::info("[verb:weather] weather sync {}", on ? "on" : "off");
}

bool PartOne::WeatherEnabled() const
{
  return pImpl->weatherEnabled;
}

void PartOne::SetWeatherSettings(const std::string& weatherSettingsJson)
{
  const auto j = nlohmann::json::parse(weatherSettingsJson);
  if (!j.is_object()) {
    throw std::runtime_error(
      "weather: an object with changeGameMinutes, seed and pools");
  }
  for (const auto& [key, value] : j.items()) {
    if (key != "changeGameMinutes" && key != "seed" && key != "pools") {
      throw std::runtime_error("weather: unknown key '" + key + "'");
    }
  }
  uint32_t changeGameMinutes = 120;
  if (j.contains("changeGameMinutes")) {
    const auto& v = j.at("changeGameMinutes");
    if (!v.is_number_unsigned() || v.get<uint64_t>() == 0 ||
        v.get<uint64_t>() > 1440) {
      throw std::runtime_error("weather: changeGameMinutes must be a whole "
                               "number of game minutes, 1 to 1440");
    }
    changeGameMinutes = v.get<uint32_t>();
  }
  uint64_t seed = 0;
  if (j.contains("seed")) {
    const auto& v = j.at("seed");
    if (!v.is_number_unsigned()) {
      throw std::runtime_error("weather: seed must be a whole number");
    }
    seed = v.get<uint64_t>();
  }
  const auto pools = j.value("pools", nlohmann::json::object());
  if (!pools.is_object()) {
    throw std::runtime_error(
      "weather: pools must map a worldspace to its list of {weather, chance}");
  }
  for (const auto& [world, list] : pools.items()) {
    if (!list.is_array() || list.empty()) {
      throw std::runtime_error(fmt::format(
        "weather: pools.{} must be a list of {{weather, chance}}", world));
    }
    for (const auto& entry : list) {
      if (!entry.is_object() || !entry.contains("weather") ||
          !entry.contains("chance") ||
          !entry.at("chance").is_number_unsigned()) {
        throw std::runtime_error(
          fmt::format("weather: pools.{} entries are {{weather, chance}} with "
                      "a whole chance",
                      world));
      }
      for (const auto& [key, value] : entry.items()) {
        if (key != "weather" && key != "chance") {
          throw std::runtime_error(fmt::format(
            "weather: pools.{}: unknown key '{}' in an entry", world, key));
        }
      }
    }
  }
  pImpl->weatherSync = skymp::rules::new_weather_sync(changeGameMinutes, seed);
  pImpl->weatherPools = pools;
  pImpl->weatherPoolsApplied = false;
  const bool applied = ApplySettingsWeatherPools();
  spdlog::info("[verb:weather] settings: a slot of {} game minutes, seed {}, "
               "{} pinned pool(s){}",
               changeGameMinutes, seed, pools.size(),
               applied ? "" : " (resolved when the game files attach)");
}

bool PartOne::ApplySettingsWeatherPools()
{
  if (pImpl->weatherPoolsApplied) {
    return true;
  }
  if (WeatherPoolsNeedLoadOrder(pImpl->weatherPools) && !HasEspm()) {
    return false;
  }
  for (const auto& [world, list] : pImpl->weatherPools.items()) {
    const auto worldId = WeatherFormRef(nlohmann::json(world),
                                        worldState.espmFiles, "a pools key");
    std::vector<uint32_t> weathers;
    std::vector<uint32_t> chances;
    for (const auto& entry : list) {
      weathers.push_back(
        WeatherFormRef(entry.at("weather"), worldState.espmFiles,
                       fmt::format("pools.{}'s weather", world)));
      chances.push_back(entry.at("chance").get<uint32_t>());
    }
    const auto n = pImpl->weatherSync->set_pool(
      worldId, rust::Slice<const uint32_t>(weathers.data(), weathers.size()),
      rust::Slice<const uint32_t>(chances.data(), chances.size()), true);
    if (n == 0) {
      throw std::runtime_error(
        fmt::format("weather: pools.{} needs a weather with a chance above 0 "
                    "(and at most 64 weathers)",
                    world));
    }
    spdlog::info("[verb:weather] worldspace {:x}: {} weather(s) pinned by the "
                 "settings",
                 worldId, n);
  }
  pImpl->weatherPoolsApplied = true;
  return true;
}

void PartOne::LoadWeatherPools()
{
  if (!HasEspm()) {
    return;
  }
  size_t regions = 0;
  std::set<uint32_t> worlds;
  for (const auto& lookup :
       GetEspm().GetBrowser().GetDistinctRecordsByType("REGN")) {
    if (!lookup.rec) {
      continue;
    }
    const auto data = espm::GetData<espm::REGN>(
      lookup.ToGlobalId(lookup.rec->GetId()), &worldState);
    if (data.weathers.empty() || data.worldspace == 0) {
      continue;
    }
    const auto world = lookup.ToGlobalId(data.worldspace);
    std::vector<uint32_t> weathers;
    std::vector<uint32_t> chances;
    for (const auto& entry : data.weathers) {
      weathers.push_back(lookup.ToGlobalId(entry.weather));
      chances.push_back(entry.chance);
    }
    if (pImpl->weatherSync->set_pool(
          world, rust::Slice<const uint32_t>(weathers.data(), weathers.size()),
          rust::Slice<const uint32_t>(chances.data(), chances.size()),
          false) > 0) {
      ++regions;
      worlds.insert(world);
    }
  }
  spdlog::info(
    "[verb:weather] pools from the game files: {} weather "
    "region(s) over {} worldspace(s); Tamriel's holds {} weather(s)",
    regions, worlds.size(), pImpl->weatherSync->pool_len(0x3c));
  if (!ApplySettingsWeatherPools()) {
    spdlog::warn("[verb:weather] the settings' pools did not resolve against "
                 "the load order");
  }
}

void PartOne::SendWeatherWord(Networking::UserId userId)
{
  if (!pImpl->weatherEnabled) {
    return;
  }
  auto actor = serverState.ActorByUser(userId);
  if (!actor) {
    return;
  }
  const uint32_t place =
    actor->GetCellOrWorld().ToFormId(worldState.espmFiles);
  if (place == 0) {
    return;
  }
  bool isWorld = false;
  if (HasEspm()) {
    const auto lookup = GetEspm().GetBrowser().LookupById(place);
    isWorld = lookup.rec && lookup.rec->GetType() == "WRLD";
  } else {
    isWorld = pImpl->weatherSync->pool_len(place) > 0;
  }
  const double gameMinutes =
    static_cast<double>(GetGameTime().daysPassed) * 1440.0;
  const auto word =
    pImpl->weatherSync->due(userId, place, isWorld, gameMinutes);
  if (!word.some) {
    return;
  }
  SetWeatherMessage message;
  message.place = word.place;
  message.weather = word.weather;
  message.transition = word.transition;
  pImpl->sendTarget->Send(userId, message, true);
  spdlog::info("[verb:weather] user {} hears {:x} for {:x} slot {} ({})",
               userId, word.weather, word.place, word.slot,
               word.weather == 0 ? "release"
                                 : (word.transition ? "transition" : "cut"));
}

void PartOne::TickWeather()
{
  if (!pImpl->weatherEnabled) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - pImpl->lastWeatherTick < std::chrono::milliseconds(500)) {
    return;
  }
  pImpl->lastWeatherTick = now;
  for (size_t i = 0, n = serverState.maxConnectedId; i <= n; ++i) {
    SendWeatherWord(static_cast<Networking::UserId>(i));
  }
}

// thuum docs/verbs/perks.md (M5): perks as transactions

void PartOne::SetPerksEnabled(bool on)
{
  pImpl->perksEnabled = on;
  worldState.perksEnabled = on;
  spdlog::info("[verb:perks] perks {}", on ? "on" : "off");
}

bool PartOne::PerksEnabled() const
{
  return pImpl->perksEnabled;
}

// thuum docs/verbs/word-wall.md (M4): the word a wall trigger teaches, read
// from the WordWallTriggerScript's myWord property on the trigger (the
// reference's own value over its base ACTI's, as ScriptVariablesHolder reads
// a script's properties) and made global through that record's lookup; none
// for a reference that is no word wall trigger, or whose word is no WOOP
std::optional<PartOne::WordWall> PartOne::WordOfWall(
  const MpObjectReference& trigger)
{
  if (!worldState.HasEspm()) {
    return std::nullopt;
  }
  auto& browser = worldState.GetEspm().GetBrowser();
  auto& cache = worldState.GetEspmCache();
  std::optional<uint32_t> word;
  uint32_t wallRef = 0;
  const auto read = [&](const espm::LookupResult& lookup) {
    if (!lookup.rec) {
      return;
    }
    espm::ScriptData scriptData;
    lookup.rec->GetScriptData(&scriptData, cache);
    for (const auto& script : scriptData.scripts) {
      // the vanilla trigger script; a removed script (status 3) is no script
      if (Utils::stricmp(script.scriptName.data(), "WordWallTriggerScript") !=
            0 ||
          script.status == 3) {
        continue;
      }
      for (const auto& property : script.properties) {
        // the wall's word is the script's myWord01 (Skyrim.esm's trigger
        // references carry it with the wall's other eight properties, the
        // base ACTI's copy of the script the shared five: lab/esm.py's VMAD
        // decode of 0x709d2 and 0x5095e, 2026-11-01)
        if (property.type != espm::Property::Type::Object ||
            !property.value.formId) {
          continue;
        }
        if (!Utils::stricmp(property.name.data(), "myWord01") ||
            !Utils::stricmp(property.name.data(), "myWord")) {
          word = lookup.ToGlobalId(property.value.formId);
        } else if (!Utils::stricmp(property.name.data(), "wordWall01")) {
          // the wall the trigger stands for: the reach is measured to it, as
          // the trigger's own bounds are the chamber (WordWallTrigger's OBND
          // is a box of about 4,000 units a side)
          wallRef = lookup.ToGlobalId(property.value.formId);
        }
      }
    }
  };
  read(browser.LookupById(trigger.GetBaseId()));
  if (trigger.IsEspmForm()) {
    read(browser.LookupById(trigger.GetFormId()));
  }
  if (!word) {
    return std::nullopt;
  }
  const auto lookup = browser.LookupById(*word);
  if (!lookup.rec || !(lookup.rec->GetType() == "WOOP")) {
    return std::nullopt;
  }
  return WordWall{ *word, wallRef };
}

namespace {
// the values a perk's conditions read: the record's bases first (the
// server's own writes land there, SetActorValueBaseByServer, and the
// leveling path mirrors a skill's level into them), then a skill's level for
// a skill the bases do not name (the record's Skill.skill is PlayerSkills'
// index, 0 for One-handed: ActorValue 6 to 23)
void RecordConditionValues(const ActorValueRecord& record,
                           std::vector<uint8_t>& avs,
                           std::vector<float>& values)
{
  for (const auto& [av, base] : record.bases) {
    avs.push_back(av);
    values.push_back(base);
  }
  for (const auto& skill : record.skills) {
    const auto av = static_cast<uint8_t>(skill.skill + 6);
    if (std::find(avs.begin(), avs.end(), av) == avs.end()) {
      avs.push_back(av);
      values.push_back(skill.level);
    }
  }
}
}

void PartOne::LoadPerkTable()
{
  if (!HasEspm()) {
    return;
  }
  // HasPerk's parameter is a perk's form id (CommonLibSSE-NG TESCondition.h
  // FUNCTION_DATA::FunctionID::kHasPerk 448), the file's own: made global
  // through the record's lookup, as the perk's own id is
  constexpr uint16_t kHasPerk = 448;
  // HasKeyword's parameter is a keyword's form id (TESCondition.h
  // FunctION_DATA::FunctionID::kHasKeyword 560), made global the same way
  constexpr uint16_t kHasKeyword = 560;
  size_t playable = 0;
  size_t conditions = 0;
  size_t effects = 0;
  for (const auto& lookup :
       GetEspm().GetBrowser().GetDistinctRecordsByType("PERK")) {
    if (!lookup.rec) {
      continue;
    }
    const auto perkId = lookup.ToGlobalId(lookup.rec->GetId());
    const auto data = espm::GetData<espm::PERK>(perkId, &worldState);
    pImpl->perkTable->add_perk(perkId, data.isPlayable, data.isHidden);
    if (data.isPlayable && !data.isHidden) {
      ++playable;
    }
    for (const auto& ctda : data.conditions) {
      uint32_t param1 = ctda.GetDefaultData().firstParameter;
      if (ctda.functionIndex == kHasPerk) {
        param1 = lookup.ToGlobalId(param1);
      }
      if (pImpl->perkTable->add_condition(
            perkId, ctda.functionIndex, ctda.operatorFlag,
            ctda.comparisonValue, param1,
            static_cast<uint32_t>(ctda.runOnType))) {
        ++conditions;
      }
    }
    // the perk's effects (thuum docs/verbs/perks.md, the second half): each
    // entry point section with a one-float value, its conditions by tab
    for (const auto& entry : data.entries) {
      if (entry.type != 2 || entry.dataType != 1) {
        continue;
      }
      const int32_t effect =
        pImpl->perkTable->add_effect(perkId, entry.entryPoint, entry.function,
                                     entry.value, entry.rank, entry.priority);
      if (effect < 0) {
        continue;
      }
      ++effects;
      for (const auto& [tab, ctda] : entry.conditions) {
        uint32_t param1 = ctda.GetDefaultData().firstParameter;
        if (ctda.functionIndex == kHasPerk ||
            ctda.functionIndex == kHasKeyword) {
          param1 = lookup.ToGlobalId(param1);
        }
        pImpl->perkTable->add_effect_condition(
          perkId, effect, tab, ctda.functionIndex, ctda.operatorFlag,
          ctda.comparisonValue, param1, static_cast<uint32_t>(ctda.runOnType));
      }
    }
  }
  spdlog::info("[verb:perks] perk table: {} perk(s) from the game files, {} "
               "playable, {} condition(s), {} effect(s) ({} the formulas "
               "judge, {} left out: another function or a third tab)",
               pImpl->perkTable->perk_count(), playable, conditions, effects,
               pImpl->perkTable->judged_effects(),
               pImpl->perkTable->unjudged_effects());
  // the formulas' way in (TES5DamageFormula): a value through the owner's
  // held perks' effects on one entry point, perks off passing it unchanged
  worldState.applyPerkEntryPoint =
    [this](const MpActor& owner, uint8_t entryPoint, float value,
           const std::vector<uint32_t>& subjectKeywords) {
      if (!pImpl->perksEnabled) {
        return value;
      }
      const auto record = owner.GetActorValueRecord();
      if (!record || record->perks.empty()) {
        return value;
      }
      std::vector<uint8_t> avs;
      std::vector<float> values;
      RecordConditionValues(*record, avs, values);
      return pImpl->perkTable->apply_entry_point(
        entryPoint, value, record->level,
        rust::Slice<const uint32_t>(record->perks.data(),
                                    record->perks.size()),
        rust::Slice<const uint8_t>(avs.data(), avs.size()),
        rust::Slice<const float>(values.data(), values.size()),
        rust::Slice<const uint32_t>(subjectKeywords.data(),
                                    subjectKeywords.size()));
    };
}

bool PartOne::PickPerk(MpActor& actor, uint32_t perk)
{
  auto record = actor.GetActorValueRecord();
  if (!record) {
    spdlog::info("[verb:perks] E_PERK_NO_RECORD: {:x} picks {:x} before its "
                 "actor values are recorded; refused, its state resent",
                 actor.GetFormId(), perk);
    actor.SendPerkState(); // an empty set and no point: the game's pick undone
    return false;
  }
  std::vector<uint8_t> avs;
  std::vector<float> values;
  RecordConditionValues(*record, avs, values);
  const auto verdict = pImpl->perkTable->pick(
    record->level,
    rust::Slice<const uint32_t>(record->perks.data(), record->perks.size()),
    record->perkPicks, rust::Slice<const uint8_t>(avs.data(), avs.size()),
    rust::Slice<const float>(values.data(), values.size()), perk);
  if (!verdict.accepted) {
    spdlog::info(
      "[verb:perks] {}: {:x} picks {:x} at level {} with {} perk(s) and {} "
      "pick(s) spent (detail {:x}); refused, its state resent",
      std::string(verdict.code), actor.GetFormId(), perk, record->level,
      record->perks.size(), record->perkPicks, verdict.detail);
    actor.SendPerkState();
    return false;
  }
  actor.AddPerkByServer(perk, true); // the pick spends a point
  spdlog::info("[verb:perks] {:x} takes perk {:x} at level {}: {} perk(s) "
               "held, {} point(s) left",
               actor.GetFormId(), perk, record->level,
               record->perks.size() + 1, actor.PerkPoints());
  actor.SendPerkState();
  return true;
}
