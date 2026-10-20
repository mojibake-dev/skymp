#pragma once
#include "AnimationSystem.h"
#include "FormDesc.h"
#include "GamemodeApi.h"
#include "HitData.h"
#include "MpActor.h"
#include "MpChangeForms.h"
#include "NiPoint3.h"
#include "PartOneListener.h"
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
struct RestSettings; // wire-bridge's (thuum ADR-020), opaque here
struct Fights;       // wire-bridge's (thuum ADR-023), opaque here
struct BondBreak;    // wire-bridge's (thuum shadowbond.md), opaque here
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
  static std::vector<PlayerGlobal> MergePlayerGlobals(
    const std::vector<PlayerGlobal>& record,
    const std::vector<PlayerGlobal>& report);
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
  bool StartCast(uint32_t actorFormId, uint32_t spell, uint8_t hand,
                 const NiPoint3& from);
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

  // The fights between players going on (thuum docs/verbs/hostility-sync.md,
  // ADR-023; the rule and its state are Rust's, ADR-020). A hit between two
  // players begins or keeps one; TickFights ends those that went a minute
  // without a hit or whose players walked apart, and tells both games.
  skymp::rules::Fights& GetFights();
  void TickFights(uint64_t nowMs);

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

  void SendHostStop(Networking::UserId badHosterUserId,
                    MpObjectReference& remote);

  static MessageSerializer& GetMessageSerializerInstance();

private:
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
  void TickBondsEverySecond();
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
