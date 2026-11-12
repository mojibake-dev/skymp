#pragma once
#include "AnimationSystem.h"
#include "FormDesc.h"
#include "GamemodeApi.h"
#include "HitData.h"
#include "MpActor.h"
#include "MpChangeForms.h"
#include "NiPoint3.h"
#include "PartOneListener.h"
#include "RaceOverlay.h"
#include "ServerState.h"
#include "SpellCastData.h"
#include "WorldState.h"
#include "formulas/IDamageFormula.h"
#include "libespm/Loader.h"
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <save_storages/ISaveStorage.h>
#include <set>
#include <simdjson.h>
#include <spdlog/logger.h>

namespace skymp::rules {
struct RestSettings;    // wire-bridge's (thuum ADR-020), opaque here
struct Fights;          // wire-bridge's (thuum ADR-023), opaque here
struct BondBreak;       // wire-bridge's (thuum shadowbond.md), opaque here
struct CellHost;        // wire-bridge's (thuum cell-host.md), opaque here
struct HostDecision;    // wire-bridge's (thuum cell-host.md), opaque here
struct PlacementRow;    // wire-bridge's (thuum spell-placed-object.md), opaque
                        // here
struct ScriptCalls;     // wire-bridge's (thuum ADR-068), opaque here
struct ScriptStates;    // wire-bridge's (thuum ADR-068 rung 3), opaque here
struct TriggerEntries;  // wire-bridge's (thuum ADR-068 D-C2), opaque here
struct QuestRuns;       // wire-bridge's (thuum ADR-068 rung 5), opaque here
struct ScriptVariables; // wire-bridge's (thuum ADR-068 rung 5), opaque here
struct PlayerOverlays;  // wire-bridge's (thuum ADR-068 rung 5), opaque here
}

using ProfileId = int32_t;
class ActionListener;
class MessageSerializer;

class PartOneSendTargetWrapper : public Networking::ISendTarget
{
public:
  explicit PartOneSendTargetWrapper(
    Networking::ISendTarget& underlyingSendTarget_);

  void Send(Networking::UserId targetUserId, Networking::PacketData data,
            size_t length, bool reliable) override;

  void Send(Networking::UserId targetUserId, const IMessageBase& message,
            bool reliable);

private:
  Networking::ISendTarget& underlyingSendTarget;
};

class PartOne
{
public:
  struct Message
  {
    nlohmann::json j;
    std::shared_ptr<IMessageBase> message;
    Networking::UserId userId = Networking::InvalidUserId;
    bool reliable = false;
  };

  using Listener = PartOneListener;

  PartOne(Networking::ISendTarget* sendTarget = nullptr);
  PartOne(std::shared_ptr<Listener> listener,
          Networking::ISendTarget* sendTarget = nullptr);
  ~PartOne();

  void SetSendTarget(Networking::ISendTarget* sendTarget);
  void SetDamageFormula(std::unique_ptr<IDamageFormula> dmgFormula);
  void AddListener(std::shared_ptr<Listener> listener);
  bool IsConnected(Networking::UserId userId) const;
  void Tick();
  FormCallbacks CreateFormCallbacks();
  ActionListener& GetActionListener();
  const std::vector<std::shared_ptr<Listener>>& GetListeners() const;
  std::vector<Message>& Messages();

  // API
  uint32_t CreateActor(uint32_t formId, const NiPoint3& pos, float angleZ,
                       uint32_t cellOrWorld, ProfileId profileId = -1);
  void SetUserActor(Networking::UserId userId, uint32_t actorFormId);
  uint32_t GetUserActor(Networking::UserId userId);
  std::string GetUserGuid(Networking::UserId userId);
  Networking::UserId GetUserByActor(uint32_t formId);
  void DestroyActor(uint32_t actorFormId);
  void SetRaceMenuOpen(uint32_t formId, bool open);
  void SendCustomPacket(Networking::UserId userId,
                        const std::string& jContent);
  std::string GetActorName(uint32_t actorFormId);
  NiPoint3 GetActorPos(uint32_t actorFormId);
  uint32_t GetActorCellOrWorld(uint32_t actorFormId);
  const std::set<uint32_t>& GetActorsByProfileId(ProfileId profileId);
  void SetEnabled(uint32_t actorFormId, bool enabled);

  using OnActorStreamIn = std::function<void(const MpActor& emitter,
                                             const MpObjectReference& listener,
                                             CreateActorMessage& message)>;
  void SetOnActorStreamIn(OnActorStreamIn callback);

  void AttachEspm(espm::Loader* espm);
  // thuum docs/verbs/cell-host.md: the records flushed at a stop (the Node
  // entry's SIGTERM and SIGINT handlers call it before the exit); true when
  // the save storage drained within the budget
  bool FlushSaves(std::chrono::milliseconds budget);
  void AttachSaveStorage(
    std::shared_ptr<
      Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>
      saveStorage);
  espm::Loader& GetEspm() const;
  bool HasEspm() const;
  void AttachLogger(std::shared_ptr<spdlog::logger> logger);
  spdlog::logger& GetLogger();

  static void HandlePacket(void* partOneInstance, Networking::UserId userId,
                           Networking::PacketType packetType,
                           Networking::PacketData data, size_t length);

  WorldState worldState;
  ServerState serverState;
  AnimationSystem animationSystem;

  PartOneSendTargetWrapper& GetSendTarget() const;

  float CalculateDamage(const MpActor& aggressor, const MpActor& target,
                        const HitData& hitData) const;

  // Charge a player's actor a move of `ground` units over the ground; true
  // when its budget covers it (thuum docs/verbs/movement-speed.md)
  bool SpendMovementBudget(uint32_t actorFormId, float ground);

  // thuum docs/verbs/console-commands.md, COC: let a player's actor make one
  // jump into a cell the server named, within a minute: an interior cell,
  // or an exterior one by its worldspace and grid square
  void PermitJump(uint32_t actorFormId, uint32_t cellId);
  void PermitJump(uint32_t actorFormId, uint32_t worldId, int16_t gridX,
                  int16_t gridY);

  // What a move the bounds refuse (another cell, or a jump) means for the
  // actor's permitted jump; the rule is Rust's (wire-rules movement)
  enum class JumpCheck
  {
    NoPermit, // none: judged as any other move
    Landed,   // the jump: it passes, once
    Waiting   // one waits and this is not it: dropped, no snap back
  };
  JumpCheck CheckJump(uint32_t actorFormId, uint32_t cellOrWorld, float x,
                      float y);

  // thuum docs/verbs/movement-speed.md, arrivals: the server moved a player's
  // actor itself (MpActor::Teleport), to (x, y) in cellOrWorld; until its game
  // reports from there, or ten seconds pass, its reports from elsewhere were
  // sent before the move. CheckArrival judges any report against that.
  void ExpectArrival(uint32_t actorFormId, uint32_t cellOrWorld, float x,
                     float y);
  // thuum docs/verbs/shadowbond.md: hold an actor where the server put it for
  // holdMs; a report from elsewhere meanwhile is sent back (the mod's own
  // swap script moving the caster a second time)
  void PinArrival(uint32_t actorFormId, uint32_t cellOrWorld,
                  const NiPoint3& at, float radius, uint64_t holdMs);
  bool PinnedAway(uint32_t actorFormId, uint32_t cellOrWorld,
                  const NiPoint3& at);
  JumpCheck CheckArrival(uint32_t actorFormId, uint32_t cellOrWorld, float x,
                         float y);

  // thuum docs/verbs/cast-teleport.md: the spells server-settings.json's
  // `castTeleports` names as teleports (a JSON list, "[]" for none; a bad
  // entry stops the server here, named), each spell's reach (negative for a
  // spell the server names no teleport; 0 for a return to where it was
  // cast, docs/verbs/ghostwalk.md), its wait (ms, 0 when unnamed) and
  // whether its cast is relayed, and the one jump a player's actor may make
  // after casting one: to within `reach` of `from`, where it stood, in
  // cellOrWorld, for `waitMs`; CheckCastJump judges a move the bounds refuse
  // against it (Landed or NoPermit; the rule is Rust's)
  void SetCastTeleportSettings(const std::string& castTeleportsJson);

  // thuum docs/verbs/player-globals.md: the globals server-settings.json's
  // `playerGlobals` names (a JSON list of FormDescs, "[]" for none; a bad
  // entry stops the server here, named), kept per player. Watch: their form
  // ids in this load order (one whose plugin is not loaded left out).
  // CheckPlayerGlobals: 0 taken, else the rule's refusal (wire-rules
  // player_globals). AtLogin: the record's values still listed. Merge: the
  // latest value wins.
  void SetPlayerGlobalsSettings(const std::string& playerGlobalsJson);
  std::vector<uint32_t> PlayerGlobalsWatch() const;
  uint8_t CheckPlayerGlobals(const std::vector<PlayerGlobal>& report) const;
  std::vector<PlayerGlobal> PlayerGlobalsAtLogin(
    const std::vector<PlayerGlobal>& record) const;
  // memo D51 B (2026-10-28, Eli: "that sounds good"): the recorded globals at
  // or past their settings' killAt, the server's word that the player dies
  std::vector<PlayerGlobal> LethalPlayerGlobals(
    const std::vector<PlayerGlobal>& record) const;
  static std::vector<PlayerGlobal> MergePlayerGlobals(
    const std::vector<PlayerGlobal>& record,
    const std::vector<PlayerGlobal>& report);

  // thuum docs/verbs/quest-stage.md (ADR-056; memo D58 B): the quests whose
  // stages are the world's, server-settings.json's `sharedQuests`: a JSON
  // list of FormDescs ("[]" for none) is mode list, the object {"mode":
  // "all", "denied": [FormDescs], "allowed": [FormDescs]} mode all; a bad
  // entry or shape stops the server here, named (thuum ADR-068 rung 5, read
  // 4: `allowed` is a retired key, read, said once and ignored).
  // SharedQuestsMode: 0 list, 1 all, as the wire carries it.
  // CheckSharedQuest: 0 the world's, else the rule's refusal (wire-rules
  // quest_stages: 1 unlisted in mode list, 2 denied in mode all, 3 a
  // player-class quest while the quest scope acts). Listed: what the login
  // names as FormDescs, settings order (the list in mode list less the
  // player class; in mode all the denied quests, then the player class).
  // AtLogin: the world's recorded stages of quests still the
  // world's, ascending per quest. DoneAlready: the world's record holds the
  // stage. Merge: the record with the stage added once, ascending within
  // its quest (unchanged when the record is full). RecordFull: the record
  // is at the rule's cap (wire-rules quest_stages QUEST_RECORD_CAP, 4096
  // pairs), so a new stage is refused (E_QUEST_FULL).
  void SetSharedQuestsSettings(const std::string& sharedQuestsJson);
  uint8_t SharedQuestsMode() const;
  uint8_t CheckSharedQuest(const FormDesc& quest) const;
  std::vector<FormDesc> SharedQuestsListed() const;
  std::vector<QuestStageDone> SharedQuestsAtLogin();
  bool QuestStageDoneAlready(const QuestStageDone& done);
  std::vector<QuestStageDone> MergeQuestStage(const QuestStageDone& done);

  // thuum docs/verbs/character-creation.md (the permit, M5): the races the
  // race menu offers only to listed profiles, from server-settings.json's
  // `restrictedRaces` ({"<shortid>:<plugin>": [profile ids]}). The table is
  // the world's (WorldState::restrictedRaces), resolved against the load
  // order when a race is judged or a player's own CreateActor is written,
  // since the settings are read before the game files load
  void SetRestrictedRacesSettings(const std::string& restrictedRacesJson);
  bool QuestRecordFull();
  // The world's journal: a server-made reference at kWorldJournalRefId (far
  // above WorldState::GenerateFormId's walk up from 0xff000000) whose change
  // form carries the world's quest stages, so the record saves and loads
  // with the world's other server-made references: WorldState::
  // LoadChangeForm recreates any reference above 0xff000000 from its
  // record's baseDesc and applies the record whole. Made at the first call.
  // GetQuestStages: the world's stages, empty when none. SetQuestStages:
  // false when unchanged.
  static constexpr uint32_t kWorldJournalRefId = 0xffffff00u;
  MpObjectReference& WorldJournal();
  std::vector<QuestStageDone> GetQuestStages();
  bool SetQuestStages(std::vector<QuestStageDone> stages);

  // thuum ADR-068 rung 4 (amendment B, read 8; docs/research/
  // rung4-state-rows-2026-11-07.md section 1(c), the created half): an alias
  // that creates its reference in another alias's inventory (QUST ALCO, ALCA
  // "in") puts one item in that container when its quest fills, which no
  // seat reports, so the server adds it, R0 from the game files: the
  // container resolved by the target alias's forced reference (ALFR) or
  // unique actor (ALUA), each item added once (the journal's aliasFills
  // marks), so a take, a second load or a restart never adds it again
  // (MS13's GoldenClaw 0x39647 and ArvelsJournal 0x39654 in Arvel 0x39655).
  // AddCreatedItems: a game-file reference just attached (WorldState's
  // onAttach), for a start-game-enabled quest's creation, or one whose quest
  // has a recorded stage. AddCreatedItemsOfQuest: a quest's first recorded
  // stage, for its creations' containers already attached. Each returns the
  // items added.
  size_t AddCreatedItems(MpObjectReference& container);
  size_t AddCreatedItemsOfQuest(uint32_t questId);
  std::vector<MpChangeFormREFR::AliasFill> GetAliasFills();
  bool SetAliasFills(std::vector<MpChangeFormREFR::AliasFill> fills);

  // thuum ADR-068 rung 4 (WorldGlobal 80; amendment B): the world's globals
  // the world streamer's game reports, on the journal; the streamer elected
  // from what ADR-066 decision 6 has built (wire-rules world_streamer: past
  // a player's first movement report, not stale, not dead, the lowest id at
  // each election) and told by its stream word (WorldGlobal with `stream`
  // true; false to the seat whose stream ends), every send at once.
  // WorldStreamerMoved: a player's movement report (its first makes it
  // eligible). JudgeWorldGlobalReport: the streamer's epoch first, 0
  // Current, 1 StaleEpoch, 2 FutureEpoch, 3 NotOwner. WorldStreamer: the
  // streamer now and the epoch.
  // The switch is server-settings.json's `worldGlobals` (false, the
  // default: nobody is elected, no stream word goes out and no record is
  // served, the games' globals stay each seat's own as before).
  void SetWorldGlobalsEnabled(bool on);
  bool WorldGlobalsEnabled() const;
  void WorldStreamerMoved(MpActor& actor);
  uint8_t JudgeWorldGlobalReport(uint32_t sender, uint32_t epoch) const;
  std::pair<uint32_t, uint32_t> WorldStreamer() const;
  std::vector<PlayerGlobal> GetWorldGlobals();
  bool SetWorldGlobals(std::vector<PlayerGlobal> globals);

  // thuum ADR-068 rung 5 (docs/research/rung5-quests-2026-11-09.md section
  // 1(b), rule 6): the world's quest runs (QuestStage 57's words) and the
  // world's condition-read script variables (ScriptVariables 81's world
  // keys), on the journal beside its stages; a read makes no journal
  // (GetAliasFills says why), the first write does
  std::vector<MpChangeFormREFR::QuestRun> GetQuestRuns();
  bool SetQuestRuns(std::vector<MpChangeFormREFR::QuestRun> runs);
  std::vector<MpChangeFormREFR::ScriptVariable> GetScriptVariables();
  bool SetScriptVariables(
    std::vector<MpChangeFormREFR::ScriptVariable> variables);

  // thuum ADR-068 rung 5 (D-Q1's derivation whole; docs/research/
  // rung5-quests-2026-11-09.md section 8(a)): the quest scope, from
  // server-settings.json's `questScope` ("off", the default, measures;
  // "derived" acts) and `playerQuests` (the census's list half), each as its
  // value's JSON text; `conditionVariables`, the condition-read keys (D-V1
  // B); and the census's per-quest companions as JSON maps keyed by the
  // quest's "<hex id>:<plugin>": `playerQuestGlobals` (to the GLOBs its
  // scripts write, composed into the player globals, so WorldGlobal refuses
  // them), `playerQuestVariables` (to its scripts' scalar variables as
  // "<script>::<var>", D-T2 B's own keys) and `playerQuestCallers` (keyed by
  // a world quest instead: to its stages whose fragment calls a player-class
  // quest's script, PQ-3's, relayed served). A bad
  // value stops the server here, named. With the game files every QUST is
  // classed (wire-rules quest_scope over its aliases) and the measurement
  // logged; while the scope acts the class is composed into the shared
  // quests, the player globals and the script variables (each with the two
  // names of the quest's game-time registration). Every setter recomposes,
  // as AttachEspm does.
  void SetQuestScopeSettings(const std::string& modeJson,
                             const std::string& playerQuestsJson);
  void SetScriptVariablesSettings(const std::string& conditionVariablesJson);
  void SetPlayerQuestSettings(const std::string& globalsJson,
                              const std::string& variablesJson,
                              const std::string& callersJson);
  // thuum ADR-068 rung 5 (D-Q4 C, PENDING ELI'S WORD on the setting):
  // server-settings.json's `questStoryInstances`, its value's JSON text
  // ("seat", the default, or "world"); a bad value stops the server here,
  // named. With the game files every quest a Story Manager quest node names
  // (libespm SMQN, its NNAMs) goes to wire-rules quest_runs, whose judge
  // keeps such a quest's runs per seat under "seat" (its Code::Instance, a
  // plain line in OnQuestStage) and whose login serves none of them; its
  // stages still cross. One line names the nodes and quests read
  void SetQuestStoryInstancesSettings(const std::string& settingJson);
  // thuum D-Q9 B (Eli 2026-11-11, docs/DECISIONS.md at main 7ced7f5a; train
  // 52): server-settings.json's `questPlayerAliasCause`, its value's JSON
  // text ("seat", the default, D-Q9 B, or "world", D-Q9 A); a bad value
  // stops the server here, named. Under "seat" a report whose world cause a
  // player alias's event roots (the root's function one of wire-rules
  // quest_runs' PLAYER_ALIAS_EVENTS, its script one the index below marks)
  // is judged with the seat's own cause (OnQuestStage)
  void SetQuestPlayerAliasCauseSettings(const std::string& settingJson);
  bool QuestPlayerAliasCauseSeat() const;
  // D-Q9 B: whether the script class `script` (compared without case) rides
  // at least one QUST alias of the game files and every alias it rides, over
  // every quest, is the player's (wire-rules quest_scope::is_players over the
  // quest's aliases); false without the game files. Indexed at AttachEspm
  bool ScriptRidesPlayerAliasesOnly(const std::string& script) const;
  // the Story Manager quest nodes naming the quest, "<editor id> <hex id>"
  // (the first, and how many more), empty for none or without the game
  // files: the nodes the line keeping such a quest's run to its seat names
  std::string StoryNodesOf(const FormDesc& quest);
  // the quests the class marks the player's, measured in either mode; and
  // the ones composed in (empty while the scope does not act)
  const std::vector<FormDesc>& QuestScopeMarked() const;
  const std::vector<FormDesc>& PlayerClassQuests() const;
  // PQ-3: a stage of a world quest whose fragment calls a player-class
  // quest's script is relayed served (applied as a load), so only its
  // acting seat runs it; false while the scope does not act
  bool IsServedStage(const FormDesc& quest, uint16_t stage) const;
  // the rules' state (wire-rules quest_runs' churn counter and held quests,
  // script_variables' keys, player_own's overlays), for the handlers
  skymp::rules::QuestRuns& GetQuestRunRules();
  skymp::rules::ScriptVariables& GetScriptVariableRules();
  skymp::rules::PlayerOverlays& GetPlayerOverlays();
  // the quests a login starts ahead of the stages and stops behind them, in
  // the run record's order, each still the world's and not held; a quest
  // recorded stopped whose stages the login replays is among the starts too
  // (D-Q3 A: its served start names its start-up stages)
  std::pair<std::vector<FormDesc>, std::vector<FormDesc>> QuestRunsAtLogin();
  // D-T1 A: the race overlay `actorId` wears beyond its record, as its
  // figure's CreateActor and UpdateAppearance carry it; none while it wears
  // its record
  std::optional<RaceOverlay> PlayerOverlayOf(uint32_t actorId) const;

  // thuum ADR-068 rung 5 (the R1 bound of a player's own rows, wire-rules
  // player_own; the fresh review's note 3): one QUST or MGEF of the game
  // files that attaches a script class (its VMAD, libespm's
  // RecordHeader::GetScriptData, the winning record of each form; a QUST's
  // alias scripts too, each the quest's attachment), with what
  // its properties name: an Object property's form, each element of an
  // ObjectArray one's, and a FormList's entries in order beside it
  // (FLST::GetData); and for an MGEF the SPELs whose effects carry it
  // (SPEL::GetData). Indexed once per set of game files at AttachEspm, by
  // the class's name without case
  struct ScriptAttachment
  {
    struct Named
    {
      uint32_t form = 0;
      std::vector<uint32_t> members;
    };
    uint32_t form = 0;            // the QUST's or MGEF's id in the load order
    bool quest = false;           // a QUST's attachment, else an MGEF's
    std::vector<uint32_t> spells; // an MGEF's: the SPELs carrying it
    std::vector<Named> named;     // each form once, in the VMAD's order
  };
  // the attachments of `scriptClass`, none for a class no QUST or MGEF
  // attaches or without the game files
  const std::vector<ScriptAttachment>& ScriptAttachmentsOf(
    const std::string& scriptClass) const;
  // whether a QUST attachment counts for the bound: the quest scope classes
  // the quest a player's (PlayerClassQuests, while the scope acts) or the
  // world's journal records it running (questRuns) or staged (questStages)
  bool QuestAttachmentLive(uint32_t questId);
  // Eli's D-E1 A (ChangeHeadPart, the seventh own row): whether `race` may
  // wear head part `part`, its HDPT's valid-race FormList (RNAM, libespm
  // HDPT::Data::validRaces) holding the race; and the look wearing `part` in
  // place of every part of the same HDPT type (PNAM: eyes replace eyes),
  // false when it wears it already or `part` is no HDPT
  bool HeadPartFitsRace(uint32_t part, uint32_t race);
  bool WearHeadPart(Appearance& look, uint32_t part);
  // thuum D-R1 C's leveled gap (train 52, D-R2 B): the bases the leveled
  // list `list` can roll (LeveledListUtils::Reach over the game files: its
  // entries, nested lists followed), read once per list and kept until the
  // next AttachEspm, sorted ascending; empty for a form that is no leveled
  // list or without the game files. A landing of a seat's reported roll must
  // be one of them
  const std::vector<uint32_t>& LeveledReach(uint32_t list);
  // a RaceMenu look's "<plugin>|<id>" as a form id in the server's load
  // order, light plugins numbered as the engine numbers them (thuum
  // docs/verbs/light-plugins.md); 0 for a plugin the server does not load
  uint32_t LookFormIdOf(const std::string& plugin, uint32_t id) const;
  // thuum ADR-026 (one authority over a face): the stored RaceMenu look
  // `look` naming `part` in place of its head part of the same HDPT type
  // (wire-rules racemenu::with_head_part, each listed part's type read from
  // its record), false when it names `part` already, names no part of that
  // type, is no look, or `part` is no HDPT
  bool WearHeadPartInLook(std::string& look, uint32_t part);
  // Eli's D-E1 A, a recorded ChangeHeadPart: `actor`'s look wears `part`
  // (WearHeadPart) and its stored RaceMenu look follows (WearHeadPartInLook),
  // since that look's load on a figure, or on the player after a login,
  // replaces the base's head parts by type and would put the old part back
  // (thuum docs/verbs/racemenu-sync.md); what each record took. The words to
  // the other games are the caller's
  struct HeadPartRecord
  {
    bool look = false;
    bool preset = false;
  };
  HeadPartRecord RecordHeadPart(MpActor& actor, uint32_t part);

  // thuum docs/verbs/spell-placed-object.md (ADR-057): the objects the
  // server places at an accepted cast of a listed spell,
  // server-settings.json's `spellPlacedObjects` (a JSON list, "[]" for
  // none; a bad entry stops the server here, named). The rule is Rust's
  // (wire-rules placements), shape A since D70: a listed spell's accepted
  // cast asks the caster's game where its engine stands the entry's bases
  // (PlacedRequest, the caster alone, SendPlacementRequests); each
  // PlacedReport the rule accepts (ReportPlacedObject: the set this
  // caster's and open, the base asked for once, the pose 150 to 260 units
  // from where the caster stood) stands as a server-made reference (MSTT or
  // ACTI) at the reported pose, every subscriber's CreateActor naming the
  // caster (placedBy) and the base's type; the set falls lingerS after the
  // stream's end or the caster's leaving, or at maxS; no report within the
  // window places nothing. TickPlacements runs that clock (Tick calls it
  // every kPlacementsTick). SweepSpellPlacedAtLoad deletes a loaded record's
  // form when the record carries placedBySpell (no cast survives a restart),
  // true when it did. SpellPlacedSets: the sets standing.
  void SetSpellPlacedObjectsSettings(
    const std::string& spellPlacedObjectsJson);
  // thuum docs/verbs/leveling.md (M5): skills rise by use and the character
  // levels by skill gains, computed here from the game's formulas (wire-rules
  // leveling) with the eighteen skills' constants read from the game files'
  // AVIF records at AttachEspm (LoadSkillTable). ApplySkillUse takes one
  // reported use (R1: the engine's base amount, bounded by the rule) into the
  // player's actor value record and sends the record to its game when a
  // level moves. The switch is server-settings.json's `leveling` (false:
  // reports are dropped, the client's own advance stays off as before).
  void SetLevelingEnabled(bool on);
  bool LevelingEnabled() const;
  // thuum docs/verbs/prop-physics.md (ADR-058): whether dropped items persist
  // as props (the loader keeps 0xff items, the drop deletes nothing); the
  // server setting propsPersist, true by default
  void SetPropsPersist(bool on);
  bool PropsPersist() const;
  float SkillUseXp(uint8_t skillAv, float base) const;
  float SkillLevelCost(uint8_t skillAv, uint8_t level) const;

  // thuum docs/verbs/weather.md (M4): one weather per worldspace, picked
  // here on the game clock from the worldspace's pool (the game files'
  // weather regions, read at AttachEspm, or server-settings.json's
  // `weather.pools`) and told to each player in it: at login, when it
  // enters a place, and at each change (SetWeather). An interior hears a
  // release. The switch is `weatherSync` (false: nothing is sent, every
  // game's own sky stands). SetWeatherSettings takes the `weather` block
  // as JSON text ("{}" for every default) and throws naming a bad key.
  void SetWeatherEnabled(bool on);
  bool WeatherEnabled() const;
  void SetWeatherSettings(const std::string& weatherSettingsJson);

  // thuum docs/verbs/perks.md (M5): a player's perk pick judged by the perk
  // record's own conditions and the points left (wire-rules perks, the table
  // read from every PERK of the load order at AttachEspm), written to the
  // record and answered with PerkState; a refused pick is undone by that
  // answer. The switch is server-settings.json's `perks` (false: picks are
  // dropped and no state is sent, the game's perks stay local as before).
  void SetPerksEnabled(bool on);
  bool PerksEnabled() const;

  // thuum docs/verbs/word-wall.md (M4): what a wall trigger reference's
  // WordWallTriggerScript names (the reference's own properties over its
  // base's): the word it teaches (myWord01) and the wall itself (wordWall01,
  // the reference the reach is measured to; 0 when the script names none)
  struct WordWall
  {
    uint32_t word = 0;
    uint32_t wall = 0;
  };
  // none for a reference that is no word wall trigger, or whose word is no
  // WOOP
  std::optional<WordWall> WordOfWall(const MpObjectReference& trigger);
  bool PickPerk(MpActor& actor, uint32_t perk);
  bool ApplySkillUse(MpActor& actor, uint8_t skillAv, float amount);
  // a skill book's read (leveling.md, skill books): one level for the skill
  // (R0: the rule's apply_book), the record sent whatever moved so the
  // seat's own increment is replaced by this number; once per book per
  // character is the caller's (ReadBookEvent and the record's readBooks).
  // False with leveling off or before the first actor values report
  bool ApplySkillBook(MpActor& actor, uint8_t skillAv);
  void TickPlacements(uint64_t nowMs);
  bool ReportPlacedObject(MpActor& caster, uint64_t set, uint32_t baseId,
                          const NiPoint3& pos, float yaw);
  bool SweepSpellPlacedAtLoad(const MpChangeForm& record);
  size_t SpellPlacedSets() const;
  // thuum ADR-068 rung 4 (the script set, W3; wire-rules placements): a
  // script's PlaceAtMe or PlaceActorAtMe on `selfId`, which stands at
  // `anchor` in `worldOrCell`, of `baseId`, its owner `reporter`: a set
  // opened for the one base (a placeable kind, else E_PLACED_KIND) and the
  // PlacedRequest sent to the reporter at once; its report (OnPlacedReport,
  // judged through the set) places the server's copy at the reported pose,
  // naming the reporter (placedBy) and recording placedByScript. False when
  // the set was refused.
  bool OpenScriptSet(MpActor& reporter, uint32_t selfId, uint32_t worldOrCell,
                     const NiPoint3& anchor, uint32_t baseId);
  // thuum docs/verbs/player-globals.md, the needs (playtest 37, 2026-10-28,
  // Eli: "food doesnt replenish hunger"): server-settings.json's
  // `survivalFood`, as data: the effects a food record carries whose
  // Creation script would lower a need in the eater's game
  // (Survival_HungerRestoreEffectScript.OnEffectStart calls
  // Hunger.DecreaseHungerBuffered(AmountToRestore.GetValue()), and
  // Survival_NeedBase.DecrementNeed clamps the result to a floor), each
  // with the need's global, the amount and the floor, plus the switch the
  // mode runs by (a JSON object, "{}" for none; a bad entry stops the
  // server here, named). SurvivalFoodOf: the entry for an effect's form id
  // in this load order, none for an effect the table does not name.
  struct SurvivalFoodEffect
  {
    FormDesc effect;
    FormDesc need;
    float amount = 0.f;
    float floor = 0.f;
  };
  void SetSurvivalFoodSettings(const std::string& survivalFoodJson);
  std::optional<SurvivalFoodEffect> SurvivalFoodOf(
    uint32_t effectFormId) const;
  std::optional<FormDesc> SurvivalFoodSwitch() const;

  // thuum memo D56 A (Eli, 2026-10-28: "A for both"): the spells the server
  // cures on every player until their verb is built, server-settings.json's
  // `cureSpells` (a JSON list of "<hex id>:<plugin>" strings, "[]" for none;
  // a bad entry stops the server here, named): Sanguinare Vampiris 0xb8780
  // first, which a seat's engine applies on a vampire's hit and whose script
  // turns the player three game days later client-side with no server
  // record (apocrypha's vampirism scout, ADR-055 D8's note). CureSpells: the
  // list as FormDescs, settings order; ActionListener::SendCures sends the
  // engine's own RemoveSpell for each at a login and every minute after.
  void SetCureSpellsSettings(const std::string& cureSpellsJson);
  const std::vector<FormDesc>& CureSpells() const;

  float TeleportReach(uint32_t spellFormId) const;

  // thuum docs/verbs/shadowbond.md: server-settings.json's `bonds` (a JSON
  // list, "[]" for none; a bad entry stops the server here, named): the
  // spells whose claimed hit bonds caster and target for the spell's
  // duration. The first act that ends either's concealment breaks the bond
  // and permits each one jump to where the other stands (the cast-jump
  // permit, for the entry's window and slack); the rule is Rust's
  // (wire-rules bonds)
  void SetBondSettings(const std::string& bondsJson);
  bool IsBondSpell(uint32_t spellFormId) const;
  // Train-28: the bond's concealment is the server's too. The entry's `proc`
  // (the Self spell the mod's script casts on both in the caster's game
  // alone, archetype Invisibility) is recorded on both for the bond's time
  // at the claim and cast, by SpSnippet, in every game but the caster's own:
  // the target's own player in its game, the figures elsewhere (R0; no
  // client reports the script's cast, Skyrim Platform drops a Papyrus cast
  // of an unequipped spell, and the relayed fire-and-forget cast applies
  // nothing to the target's player). The break, a recast at another target
  // and the bond running out end the records and dispel the target's copy
  void RecordBond(uint32_t casterFormId, uint32_t targetFormId,
                  uint32_t spellFormId);
  // true when the actor was in a standing bond: it ended, both are swapped
  // and held, and the concealment the server imposed ends
  bool BreakBondsOf(uint32_t actorFormId);
  // Ends the bonds that ran out by `nowMs` (the bonds rule's clock,
  // SteadyNowMs): their concealments end as at a break, without the swap
  void TickBonds(uint64_t nowMs);

  // thuum docs/verbs/headshot.md: server-settings.json's `headshot` block
  // (the spells whose hit reports a headshot, the victim races the mod's
  // rule names, the shooter's base Archery floor; "{}" for none; a bad
  // block refuses to start)
  void SetHeadshotSettings(const std::string& headshotJson);
  bool IsHeadshotSpell(uint32_t spellFormId) const;
  // a bow hit by the actor on the target the server claimed (OnHit): a
  // headshot may follow it within the rule's window, once
  void NoteBowHit(uint32_t actorFormId, uint32_t targetFormId);
  enum class HeadshotCheck
  {
    Killed,   // everything holds: the target died
    NoShot,   // no claimed bow hit on the target within the window
    Helmeted, // the target wears something in the head or hair slot
    Race,     // the target's race is not one the rule names
    Archery   // the shooter's base Archery is under the floor
  };
  // a headshot the actor's game reports on the target: the facts from the
  // records (the target's worn head slots by ARMO BOD2, its race, the
  // actor's base Archery) against the rule (wire-rules headshots); Killed
  // kills the target
  HeadshotCheck ClaimHeadshot(MpActor& actor, MpActor& target);
  // thuum docs/verbs/scripted-effects.md: server-settings.json's
  // `scriptedEffects` list (the spells whose script's work the server does
  // itself from the claimed hit; "[]" for none; a bad list refuses to
  // start)
  void SetScriptedEffectSettings(const std::string& scriptedEffectsJson);
  bool IsScriptedKillSpell(uint32_t spellFormId) const;
  // the claimed hit of a scripted kill by the actor on the target: once an
  // effect of the spell holds its conditions for the target (ADR-039), the
  // script's arithmetic over the two actors' current Health (wire-rules
  // scripted_effects), the target's loss by the actor's hand; whether the
  // target died
  bool ClaimScriptedKill(MpActor& actor, MpActor& target, uint32_t spellFormId,
                         const std::vector<espm::Effects::Effect>& effects);
  uint64_t TeleportWaitMs(uint32_t spellFormId) const;
  bool TeleportRelays(uint32_t spellFormId) const;
  void PermitCastJump(uint32_t actorFormId, uint32_t cellOrWorld,
                      const NiPoint3& from, float reach, uint64_t waitMs);
  JumpCheck CheckCastJump(uint32_t actorFormId, uint32_t cellOrWorld,
                          const NiPoint3& at);
  // Whether a move the bounds took, to `at` in cellOrWorld, lands within
  // the cast jump that waits for the actor; the jump stands. The other games
  // place the figure there (thuum docs/verbs/cast-teleport.md, the teleport
  // snap; docs/verbs/shadowbond.md, the swap's landings within the budget)
  bool CastJumpLands(uint32_t actorFormId, uint32_t cellOrWorld,
                     const NiPoint3& at) const;

  // thuum docs/verbs/cast-teleport.md, the look on other screens: what a
  // teleport's castTeleports entry names for the other players' games to
  // play on its caster's figure, as load-order form ids (0 for a part it
  // names none of, or one in a plugin the server does not load); none for a
  // spell the server names no teleport, or whose entry names no look
  struct TeleportLook
  {
    uint32_t vanish = 0;       // VisualEffect: from the cast to the landing
    uint32_t appear = 0;       // VisualEffect: at the landing
    float appearSeconds = 0.f; // how long the appear plays
    uint32_t sound = 0;        // Sound: at the landing
  };
  std::optional<TeleportLook> GetTeleportLook(uint32_t spellFormId) const;
  // The caster's teleport cast, its jump permitted for `waitMs`: every other
  // player's game that sees the caster plays the look's vanish on its figure
  // for that wait (so a fizzled teleport's ends by itself), and the look is
  // kept for the landing as long; a later teleport cast replaces it, one
  // without a look drops it
  void PlayTeleportVanish(MpActor& caster, uint32_t spellFormId,
                          uint64_t waitMs);
  // The caster landed at `at`: those games place the figure there at once
  // (no glide), stop the vanish, play the appear and the sound. Once per
  // kept look; nothing when none is kept or its wait ran out
  void PlayTeleportLanding(MpActor& caster, const NiPoint3& at);
  // Whether a move of `step` units over the ground that the movement bounds
  // took is a teleport's landing for its look: a landing within the ground
  // budget's burst never reaches the cast jump's permit (wire-rules
  // teleports::look_lands)
  static bool TeleportLookLands(float step);

  // thuum docs/verbs/marksman.md: a player loosed an arrow from `weapon` at
  // draw `power`, standing at `from`; a later hit with that weapon claims
  // it, once, if its arrow can have reached the target since (wire-rules
  // ranged), and counts at the shot's power
  void RecordShot(uint32_t actorFormId, uint32_t weapon, float power,
                  const NiPoint3& from);
  enum class ShotCheck
  {
    Claimed, // a recorded shot covers the hit, and is used up
    NoShot,  // no unused shot of that weapon in the window
    TooFar   // shots wait, but none can have reached the target yet
  };
  struct ShotClaim
  {
    ShotCheck check = ShotCheck::NoShot;
    float power = 0.f; // the claimed shot's draw power, 0 to 1
  };
  ShotClaim ClaimShot(uint32_t actorFormId, uint32_t weapon,
                      const NiPoint3& target);

  // thuum docs/verbs/spell-cast.md: an actor began casting `spell` from
  // `hand` (0 left, 1 right, 2 voice, 3 instant), standing at `from`: a
  // fire-and-forget cast waits for its hit, a concentration one opens in
  // that hand (wire-rules casts); its kind, area and reach come from the
  // records
  // true: the cast's relay waits for its first hit (every effect delivered
  // to its target; wire-rules casts::CastFacts::lands_on_target)
  // `dual`: the caster's game reported a dual cast (EffectWasDualCast 724)
  bool StartCast(uint32_t actorFormId, uint32_t spell, uint8_t hand,
                 const NiPoint3& from, bool dual = false);
  // the stream in `hand` ended
  void EndCast(uint32_t actorFormId, uint8_t hand);
  enum class CastCheck
  {
    Claimed,  // a recorded cast covers the hit
    NoCast,   // no cast of that spell covers it
    TooFar,   // casts wait, but the target is beyond their reach
    Throttled // a reached spell's hit on a target it claimed within the tick
  };
  struct CastClaim
  {
    CastCheck check = CastCheck::NoCast;
    float scale = 0.f;  // the factor on the spell's magnitude
    bool relay = false; // the first hit of a cast whose relay waited for it
    bool dual = false;  // the claimed cast was dual cast (EffectWasDualCast)
  };
  // a spell hit by the actor on `target` standing at `at`
  CastClaim ClaimCast(uint32_t actorFormId, uint32_t spell, uint32_t target,
                      const NiPoint3& at);
  // thuum docs/verbs/second-hand.md: a hit by a spell none of the actor's
  // casts is of, claimed against a recorded cast whose records reach it
  // (a cloak's spell, a spell its effects' scripts name), judged from where
  // the caster stands now (`casterAt`)
  CastClaim ClaimReached(uint32_t actorFormId, uint32_t spell, uint32_t target,
                         const NiPoint3& at, const NiPoint3& casterAt);
  // thuum docs/verbs/npc-stream.md (train 53; playtest 37's flame atronach):
  // the ability among the actor's record's spells (MpActor::KnownBaseSpells;
  // a constant effect the engine runs from the actor's birth and never
  // casts) whose records reach `spell` (a Cloak effect's associated spell,
  // ReachedSpells), or nothing. Its standing cast is what a hosted
  // creature's cloak hit claims (ActionListener::OnHit)
  std::optional<uint32_t> AbilityReaching(const MpActor& actor,
                                          uint32_t spell);

  // thuum docs/verbs/shouts.md: what the server knows of a shout a player's
  // game reported, and its verdict (wire-rules shouts); an accepted shout
  // starts its recovery on the server's clock, which a reconnect does not
  // end
  struct VoiceFacts
  {
    bool shoutEquipped = false; // the voice slot holds a shout it recorded
    uint8_t variation = 0;      // the spell's place among the variations
    uint8_t wordsUnlocked = 0;  // bit i: the record unlocked word i
    float recoverySeconds = 0.f;
    float recoveryMult = 1.f;
  };
  enum class VoiceCheck
  {
    Accepted,
    NoShout,       // the voice slot holds no shout the record has
    NotAVariation, // the spell is none of the shout's
    WordLocked,    // a word up to the variation is not unlocked
    Recovering     // the last shout's recovery runs
  };
  VoiceCheck Shout(uint32_t actorFormId, const VoiceFacts& facts);
  // how long the actor's voice recovery runs, ms (0 when none)
  uint64_t VoiceRecoveryLeftMs(uint32_t actorFormId) const;

  // The server's game clock (thuum docs/verbs/time.md, ADR-021) from
  // server-settings.json's `time` block as JSON text ("{}" for every
  // default); throws naming the bad key. From then on each player hears the
  // clock at login, ahead of their own CreateActor, and every 60 s after;
  // ScampServer always calls it, as it switches on gamemode data broadcasts.
  void SetGameTimeSettings(const std::string& timeSettingsJson);
  GameTimeNow GetGameTime() const;

  // TES3MP's rest switches (thuum docs/verbs/rest.md) from
  // server-settings.json's `rest` block as JSON text ("{}" for both on);
  // throws naming the bad key. The rest rule asks them for every rest.
  void SetRestSettings(const std::string& restSettingsJson);
  const skymp::rules::RestSettings& GetRestSettings() const;

  // The fights going on (thuum docs/verbs/hostility-sync.md, ADR-023; the
  // rule and its state are Rust's, ADR-020): between two players, or since
  // 2026-10-28 a player and a hosted NPC (docs/verbs/npc-stream.md, H-53h).
  // A counted hit begins or keeps one; TickFights ends those that went a
  // minute without a hit or whose parties walked apart, and tells the
  // players' games.
  skymp::rules::Fights& GetFights();
  void TickFights(uint64_t nowMs);

  // thuum ADR-068 (the one seam): every reporter's script-call budget and
  // every subject's sequence (the rule and its state are Rust's, wire-rules
  // natives); ActionListener::OnScriptCall asks it for each reported call
  skymp::rules::ScriptCalls& GetScriptCalls();

  // thuum ADR-068 rung 3 (the script record): the release each cell last saw
  // until its next grant (wire-rules script_state; ApplyHostDecisions notes
  // the release and the grant); ActionListener::OnScriptState asks it for
  // each reported record
  skymp::rules::ScriptStates& GetScriptStates();

  // thuum ADR-068 D-C2 (the trigger entry channel): each presser's accepted
  // enters it has not left, by trigger (wire-rules trigger_entry::Entries;
  // the disconnect forgets a player's); ActionListener::OnTriggerEntry asks
  // it for each reported entry, so a leave pairs with its enter
  skymp::rules::TriggerEntries& GetTriggerEntries();

  // thuum docs/verbs/cell-host.md (ADR-035): the cell host table (wire-rules
  // cells, through the bridge). The facts: a game's CellLoaded report
  // (OnCellLoaded), the server's grid for the player (RefreshReach: its 3x3
  // of exterior cells, or its interior, recomputed when its grid square
  // moves, before a report is judged and every second), the clock
  // (TickCellHosts: a grace that ran out passes its cell on) and a departure
  // (the disconnect). The decisions go to the games as HostGrant and
  // HostRelease, and the actors of the cell flip their hosted flag.
  void OnCellLoaded(MpActor& player, uint32_t cell, bool loaded);
  // thuum docs/research/m3/decisions-after-playtest-34.md D18: a dead player
  // hosts nothing; its cells start their grace at the death and it is a
  // candidate again when it stands
  void OnDeathState(MpActor& actor, bool isDead);
  void RefreshReach(MpActor& player);
  void TickCellHosts(uint64_t nowMs);
  // thuum docs/verbs/npc-fate.md (ADR-050 decision 2): a hosted NPC whose
  // server number reached zero dies by the server's word when its host
  // reported nothing within MpActor::kPendingDeathMs
  void TickDeaths(uint64_t nowMs);
  // thuum docs/verbs/npc-fate.md ("The body's rest", ADR-067): a corpse
  // whose settle window passed with no two agreeing samples from its owner
  // is at rest where the record has it (MpActor::SettleBodyIfDue)
  void TickBodies(uint64_t nowMs);
  // thuum docs/verbs/npc-talk.md (ADR-052): a player's claim on an NPC for a
  // dialogue, the per-actor row beside the cell's host; the word follows
  enum class ClaimVerdict
  {
    Granted,
    Held,
    Unloaded,
    Reclaimed
  };
  ClaimVerdict ClaimActor(MpActor& claimant, MpActor& actor);
  bool ReleaseClaim(MpActor& claimant, MpActor& actor);
  // thuum docs/verbs/npc-fate.md (ADR-060): a commanded NPC is simulated by
  // its commander's game until that engine ends the effect, the thrall's
  // death on the engine's own timer, with no lease (Eli, 2026-11-12:
  // "summons and resurrections should obey the timers the level of
  // spell"). `commander` is the seat whose engine ran the spell (the caster
  // when a user plays it, else the caster's host). The word is UpdateHoster
  // to every listening game,
  // with no SendActorKeyframe ahead of it: the grant runs on a dead record
  // (the reanimate's hit), which a promote never places (a body is never
  // moved, D78 C; the client's dead hold), so the keyframe would place
  // nothing
  // `effectMs` is the record's duration (0 for a permanent effect), kept as
  // the thrall's estimate for its commander's departure alone (cells.rs's
  // orphan, HYPOTHESIS: the engine's own duration may be longer), never a
  // lease while the commander stays
  ClaimVerdict ClaimCommanded(MpActor& commander, MpActor& actor,
                              uint64_t effectMs);
  // the player commanding `actor` (ADR-060), 0 when none: whose report of
  // the thrall's death is its effect's end
  uint32_t CommanderOf(const MpActor& actor) const;
  // any claim on `actor` ends (its death): the claimant it had, 0 for none;
  // the cell's rule names the simulator again and the word follows, again
  // on a dead record and so with no keyframe
  uint32_t ReleaseAnyClaim(MpActor& actor);
  // thuum docs/engine/non-owning-seat.md section 3: the actor's record (its
  // transform and flags, as a movement message) to the game about to
  // simulate it, reliable and ahead of the hoster word on the same channel,
  // so its promote places the copy at the record and its first report comes
  // from there (the pass sends the passing report itself; a grant, a claim,
  // a release and a lease end send this). Nothing is sent to a game with no
  // user.
  void SendActorKeyframe(MpActor& actor, MpActor& to);
  // thuum docs/verbs/prop-physics.md (ADR-058): one simulating seat per
  // physics prop, the cell's host by default, the last toucher while its
  // lease holds; the owner table's "prop" rows (wire-rules props). The
  // handlers gather the facts (the reference, its cell, the claimant) and
  // ask here; the word (PropOwner) is the claim handler's.
  enum class PropClaimVerdict
  {
    Granted,
    Held,
    Same,
    Moving
  };
  // `worldBody`: the prop is a trap's world body (thuum D-S3 point 4: a
  // world object a seat's own script set moving, claimed by the impulse
  // alone), whose first firing wins: Moving while another seat's firing has
  // it awake (wire-rules props' claim_world_body); an item's claim keeps
  // ADR-058's newest touch wins
  PropClaimVerdict ClaimProp(const MpActor& claimant,
                             const MpObjectReference& prop, uint32_t cell,
                             uint8_t touch, bool worldBody);
  enum class PropTransformVerdict
  {
    Accepted,
    NotOwner,
    WrongCell,
    TooFast
  };
  // `sender`'s sample of `prop` landing in `cell` (0 for none), judged by
  // the host of `recordCell` (the cell the record stands in, read from the
  // host table), the body asleep or not, `moved` units from the record (the
  // rule's step budget)
  PropTransformVerdict PropTransform(const MpActor& sender,
                                     const MpObjectReference& prop,
                                     uint32_t cell, uint32_t recordCell,
                                     bool asleep, float moved);
  // The prop's live row's owner, else 0: the word a listener arriving while a
  // seat simulates the prop gets at its subscription (onSubscribe).
  uint32_t PropLiveOwner(const MpObjectReference& prop) const;
  // Who simulates `prop` where its record stands: the toucher while awake or
  // inside its lease, else its cell's host, else 0.
  uint32_t PropOwnerOf(const MpObjectReference& prop) const;
  // The prop's owner epoch, up at every grant; 0 before any.
  uint32_t PropEpochOf(const MpObjectReference& prop) const;
  // `host` now hosts `cell` (ApplyHostDecisions' grant): the props in it
  // nobody holds live name the host, their epochs kept.
  void PropsHostChanged(uint32_t cell, uint32_t host);
  // thuum docs/verbs/npc-fate.md (the deaths): the bodies whose deadline
  // (fateDiedAt plus WorldState::FateRespawnHours()) the installed game
  // clock has passed stand again; once a second from Tick, directly from a
  // test with the clock it set
  void TickFateRespawns();
  // thuum docs/verbs/npc-puppet.md (ActorSightings, TES3MP's actor list): a
  // host's engine has a game-file actor the server holds no record of, or
  // holds elsewhere (a resident placed in its house, walked outside by its
  // schedule). If `reporter` hosts the cell of the sighted position and no
  // other game hosts the actor where the record stands, the record is
  // loaded on demand and placed there; its CreateActor and word follow.
  void OnActorSighting(MpActor& reporter, uint32_t refrId,
                       uint32_t worldOrCell, const NiPoint3& pos,
                       const NiPoint3& rot);
  // thuum docs/verbs/npc-puppet.md (playtest 38's finding 66, "attempted to
  // kill a bandit outside bleakfalls. all damage was ignored"): the host's
  // roster naming the bandit reached the server in the grant's own
  // millisecond (20:00:51.189Z) and was dropped as nobody's; the next came
  // ten seconds later, after the player had died. A sighting dropped for a
  // missing host is held per cell, one entry per (reporter, reference), for
  // kHeldSightingMs; the grant replays the new host's own entries here
  // (ApplyHostDecisions calls it after the flags flip) and keeps another
  // reporter's for that reporter's own grant while they last. A sighting of
  // another game's cell is never held. The hold is bounded three ways (the
  // train's review, 2026-10-29: it grew with no total, no sweep and no
  // clear at a disconnect): per cell (kHeldSightingsPerCell), in total
  // (kHeldSightingsTotal; full, the oldest entry anywhere makes room, said
  // once in ten seconds) and in time (SweepHeldSightings, from the
  // once-a-second tick, drops the expired); a reporter's disconnect forgets
  // its entries (ForgetHeldSightings).
  void ReplayHeldSightings(uint32_t cell, MpActor& host);
  void SweepHeldSightings(uint64_t nowMs);
  void ForgetHeldSightings(uint32_t reporter);
  // every held entry, or `cell`'s alone (the tests' and the lab's reading)
  size_t HeldSightingsCount(std::optional<uint32_t> cell = std::nullopt) const;
  static constexpr uint64_t kHeldSightingMs = 30'000;
  static constexpr size_t kHeldSightingsPerCell = 64;
  static constexpr size_t kHeldSightingsTotal = 1024;
  // Where the players' round trips come from (IServer::GetRttMs, -1 while
  // unknown), read every tick for the table's ranking; none installed, no
  // round trip is known and the earliest loaded takes a cell.
  void SetRoundTripSource(std::function<int64_t(Networking::UserId)> source);
  // Where each connection's silence comes from (IServer::GetSilenceMs, the
  // milliseconds since its last message, -1 while unknown), read every
  // tick: a user silent for kStaleHostMs is stale (its hosted cells start
  // their grace, it is no candidate, its loaded set kept), and heard again
  // it is revived (a candidate again; a grace of its own still running is
  // disarmed). The quitting host of playtest 32: a game killed, crashed or
  // quit sends netcode no disconnect and held its village for the
  // transport's timeout plus the grace, about 24 s. None installed, only
  // the disconnect ends a host's tenure.
  void SetSilenceSource(std::function<int64_t(Networking::UserId)> source);
  // the silence that makes a host stale: four seconds is thirty missed
  // reports at the client's 130 ms cadence (x-host-kill 20261023-205837 and
  // 214420: a game's first loads after a login were silent 3.1 to 4.8 s,
  // before it hosted anything, so only a host is marked; no mid-session
  // loading screen was silent that long; the kill passed its cell 8.7 and
  // 9.4 s after the last report against 24 s before). The silence is the
  // smaller of netcode's time since any packet and the time since a message.
  static constexpr int64_t kStaleHostMs = 4'000;
  // Who hosts `cell` (hosted false while nobody; the epoch kept).
  skymp::rules::CellHost GetCellHost(uint32_t cell) const;
  // thuum docs/verbs/console-commands.md, `mp host`: how `cell`'s current
  // grant came, by the rule the lab reads the cell-host lines with
  // (lab/tools/playtest_setup.py last_grant): "fresh" when it came with
  // nobody hosting (the first since the server started, or after the cell
  // went unhosted), "inherited" when another host's stop came before it
  // (that seat loaded the cell as a guest first), "none" before any grant.
  // ApplyHostDecisions notes each grant, stop and unhosting of a cell here.
  enum class CellHostNote
  {
    Grant,
    Release,
    Unhosted
  };
  const char* GetCellGrantKind(uint32_t cell) const;
  void NoteCellHostDecision(CellHostNote note, uint32_t cell);
  // Every player actor whose game reports `cell` loaded (its CellLoaded
  // reports, OnCellLoaded), lowest id first: the games that show what
  // stands in it, the listeners of a script call's subject the server holds
  // no reference for (thuum ADR-068, ActionListener::OnScriptCall)
  std::vector<uint32_t> GetCellLoaders(uint32_t cell) const;
  // thuum docs/verbs/npc-fate.md (ADR-049 decision 3): the fates of the
  // game-file actors in the grid squares around `player`'s own, kFateRing
  // out (the engine attaches a 5x5 of cells; the grid's squares truncate
  // toward zero, so three squares cover it on the negative side too), sent
  // once per square per player as ActorFate ahead of the engine's own roll;
  // the chunks are loaded first with the player's level as the roll's.
  // Runs at every square change (RefreshReach).
  void SendFates(MpActor& player, uint32_t cellOrWorld, const NiPoint3& pos);
  static constexpr int16_t kFateRing = 3;
  // thuum docs/verbs/npc-fate.md (ADR-049 decision 1, the memo's D2): the
  // level the fate pass rolls `loader`'s fresh squares at, by the server
  // setting npcRollLevel (WorldState): "loader", the loader's own level (the
  // engine's input, option A); "party-mean" (the default), the mean, rounded
  // down, of the levels of the players within kFateRing squares of the
  // loader's square in the same world or cell, the loader among them, or of
  // every player online when no near player has a record; "party-min", the
  // lowest of the same; "fixed", npcRollLevelFixed for everyone (a designed
  // difficulty). Clamped at 1. The encounter zone's clamp is ADR-049's owed
  // read.
  uint16_t FateRollLevel(MpActor& loader, uint32_t cellOrWorld, int16_t sx,
                         int16_t sy);
  // thuum docs/verbs/npc-fate.md (playtest 51, b-npc-stream-equipment): the
  // roll belongs to the actor, not to the grid square. A game-file actor
  // with no roll yet (MpActor::CanRollFate) rolls at `loader`'s level
  // (FateRollLevel at the loader's square, the pass's own) where its record
  // first reaches a game: the pass's walk (SendFates, `walker` the game its
  // batch carries the fate to) and its create word (WorldState::
  // onActorSubscribe, ahead of the word). The fate rides ahead of each of
  // the actor's create words (onSubscribe), so this sends it to the games
  // whose pass has fated the actor's square and that get no create word now
  // (the ring beyond its 3x3), as that pass will not come there again; the
  // rest hear it at their own pass. A game can hear one roll again (a create
  // word, then its pass), which it takes for free while the fate is the
  // same. True when the actor rolled here
  bool EnsureFated(MpActor& actor, MpActor& loader,
                   const MpActor* walker = nullptr);
  // Every cell the table knows, by cell id, for the lab's reading
  // (mp.get(0, "cellHosts")).
  struct CellHostEntry
  {
    uint32_t cell = 0;
    bool hosted = false;
    uint32_t player = 0;
    uint32_t epoch = 0;
  };
  std::vector<CellHostEntry> GetCellHosts() const;
  // Every follow (thuum docs/verbs/npc-handoff.md D6, ADR-047): an actor a
  // host simulates in a cell nobody hosts, for the lab's reading.
  struct CellFollowEntry
  {
    uint32_t actor = 0;
    uint32_t player = 0;
    uint32_t cell = 0;
  };
  std::vector<CellFollowEntry> GetFollows() const;
  // The cell-change rule (thuum docs/verbs/cell-host.md; npc-handoff D6):
  // `sender`'s report moves `actor` (an actor no user plays) to `destPos`
  // in `destWorldOrCell` from the cell its record holds. Inside the cell
  // the sender simulates (as its host, or as the actor's follower), or
  // into a cell it hosts: Accept. Into a cell nobody hosts that the sender
  // vouches for (an adjacent exterior square its game has loaded, or a door:
  // an interior at either end, any actor's, memo D54 B): Follow, recorded in
  // the table; the sender simulates the actor there until the cell is
  // granted, it unloads the cell or leaves. Pass (ADR-047 D9, npc-handoff
  // (d)): the simulator's adjacent crossing into another live host's cell,
  // the record moving there and the simulation passing to that host at
  // once, the word flipped both ways (OnUpdateMovement). Anything else:
  // Refuse.
  enum class MoveVerdict : uint8_t
  {
    Refuse = 0,
    Accept = 1,
    Follow = 2,
    Pass = 3
  };
  MoveVerdict MayMoveActor(const MpActor& sender, const MpActor& actor,
                           uint32_t destWorldOrCell, const NiPoint3& destPos);
  // The same verdict without recording anything (no follow written or
  // ended): the door's pre-check, before the host's engine is asked to walk
  // the actor through; the follow is recorded by the host's report.
  MoveVerdict WouldMoveActor(const MpActor& sender, const MpActor& actor,
                             uint32_t destWorldOrCell,
                             const NiPoint3& destPos) const;
  // Who simulates `actor` where its record stands: the cell's host, else
  // its follower, else 0.
  uint32_t SimulatorOf(const MpActor& actor) const;
  // The follow switched off (server setting cellFollow false): a move into
  // an unhosted cell is refused as before train-40.
  void SetFollowEnabled(bool enabled);

  float CalculateDamage(const MpActor& aggressor, const MpActor& target,
                        const SpellCastData& spellCastData) const;

  void NotifyGamemodeApiStateChanged(
    const GamemodeApi::State& newState) noexcept;

  void SetPrivateKey(const std::string& keyId, const std::string& pkeyPem);

  void EnableGamemodeDataUpdatesBroadcast(bool enable);

  void SetPacketHistoryRecording(Networking::UserId userId, bool value);
  PacketHistory GetPacketHistory(Networking::UserId userId);
  void ClearPacketHistory(Networking::UserId userId);
  void RequestPacketHistoryPlayback(Networking::UserId userId,
                                    const PacketHistory& history);

  static MessageSerializer& GetMessageSerializerInstance();

private:
  // the hold behind OnActorSighting: `reporter`'s sighting of `refrId` in
  // `cell`, which has no host yet (ReplayHeldSightings above)
  void HoldSighting(uint32_t reporter, uint32_t cell, uint32_t refrId,
                    uint32_t worldOrCell, const NiPoint3& pos,
                    const NiPoint3& rot);
  void Init();

  enum class UserType
  {
    User,
    Bot
  };

  void AddUser(Networking::UserId userId, UserType userType,
               const std::string& guid);

  void HandleMessagePacket(Networking::UserId userId,
                           Networking::PacketData data, size_t length);

  void InitActionListener();

  void TickPacketHistoryPlaybacks();
  void TickGameTime();
  void TickFightsEverySecond();
  void TickCellHostsEverySecond();
  void TickFateRespawnsEverySecond();
  void TickBondsEverySecond();
  // thuum docs/verbs/spell-placed-object.md: the placements' clock from
  // Tick; one object placed (0 when it could not be), a set's forms deleted
  void TickPlacementsOften();
  uint32_t PlaceSpellObject(const skymp::rules::PlacementRow& row);
  void RemoveSpellPlacedSet(uint64_t set, const char* why);
  // shape A: the requests the rule queued at an accepted cast, one message
  // a set, to the caster alone
  void SendPlacementRequests();
  // thuum docs/verbs/leveling.md: the skills' AVSK rows from the game files
  // (AVOneHanded 0x44c to AVEnchanting 0x45d, one per ActorValue 6 to 23)
  void LoadSkillTable();
  void LoadWeatherPools();
  bool ApplySettingsWeatherPools();
  void LoadPerkTable();
  // thuum ADR-068 rung 5: the quest scope's class over the game files, its
  // measurement line, and while it acts its composition into the shared
  // quests, the player globals and the script variables; the player globals
  // alone (the setting's, with the class's while it acts)
  void ComposeQuestScope();
  // D-Q4 C: the Story Manager's quests and the setting into quest_runs
  void ComposeStoryQuests();
  void ComposePlayerGlobals();
  void TickWeather();
  void SendWeatherWord(Networking::UserId userId);
  // thuum docs/verbs/shadowbond.md (train-28): the proc's records on both
  // and its cast in every game but the caster's; the reverse, with the
  // reason for the log
  void ConcealBond(const skymp::rules::BondBreak& bond);
  void UnconcealBond(const skymp::rules::BondBreak& bond, const char* why);
  // the proc's casts in every game but the caster's, sent by ConcealBond's
  // timer once the relayed bond cast has run there
  void CastBondProc(const skymp::rules::BondBreak& bond, uint32_t procId);
  // the proc's form id in this load order, 0 for none (or not loaded)
  uint32_t BondProcFormId(const skymp::rules::BondBreak& bond) const;
  void TickDeferredMessages();

  std::string SignJavaScriptSources(const std::string& src) const;

  struct Impl;
  std::shared_ptr<Impl> pImpl;
};
