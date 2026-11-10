#pragma once

#include <RE/B/BSCoreTypes.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

// thuum docs/DECISIONS.md ADR-068 rung 5 (D-F1 C: every seat's VM applies
// every stage; D-Q3 A: the live relay runs the fragment, a replay skips it)
// and docs/research/rung5-quests-2026-11-09.md 1(c): the acting scope. A
// stage, a start or a stop the server relays from another player's seat runs
// its fragment here for that player, the acting player: the seam
// (ScriptNatives.cpp OnCall, step (0)) reads the scope of the stack a native
// runs on, swaps the player's own actor (0x14) for the acting player's
// figure in the native's frame and Game.GetPlayer's result for it, and
// carries the scope to a stage, a start or a stop the stack sets (read 7's
// propagation). Hosted.h's idiom: mutex-guarded, written with no game call,
// gated by one atomic Any() the seam reads before anything else. Three
// tables:
// - requests, (subject, stage or word) to the acting player: written by
//   setActingPlayer (PuppetApi.cpp) before the client applies a relay, and
//   by the seam's propagation (chained); taken by the quest stage sink
//   (FridaHooks.cpp) at the pair's event and by SendTriggerEvent
//   (ScriptState.cpp) for a relayed entry; read by getActingPlayer until it
//   expires, so the seat's report of a stage set inside a scope names the
//   scope's player (the C2S QuestStage's `acting`)
// - arms, a subject (a quest, a trigger) to its scope for kArmMs: a stack
//   whose root frame's self is the subject joins the scope at its first
//   native call inside the window (the seam's Join)
// - joined stacks, a stack id to its scope, kept for the stack's life (at
//   most kJoinedMs)
// Cleared at a game load (PapyrusTESModPlatform.cpp's load sink): a stack id
// names nothing in the new world. Beside them, rung 5's two per-player sets
// (the plan's section 8(a)): the globals each player keeps, whose SetValue a
// scoped stack never runs here (PQ-3, PlayerGlobals below), and the quests
// each player keeps, whose own scripts' events the Papyrus event gate passes
// on every seat for its own player (PlayerQuests below).
namespace ActingScope {

// a request's stage beside a quest stage's 0 to 65535 (setActingPlayer's
// stage argument): a quest's start, its stop, and a relayed trigger entry
// (its subject the trigger, taken by SendTriggerEvent's arm)
constexpr int32_t kStart = -1;
constexpr int32_t kStop = -2;
constexpr int32_t kEntry = -3;

// ARM_MS, UNMEASURED (HYPOTHESIS H-AP3: the window joins only the relayed
// fragment's stacks; x-acting-player v1 records the joins per arm)
constexpr uint64_t kArmMs = 2000;
// JOINED_MS: a joined stack keeps its scope this long at most
constexpr uint64_t kJoinedMs = 120000;
// a chained request's life (the seam's propagation): the client's suppress
// safety's 10 s, so a stage the VM queues on an unstarted quest still finds
// it, and the seat's report of the stage still reads it
constexpr uint64_t kChainedMs = 10000;
// setActingPlayer's ttl, at most
constexpr uint64_t kMaxRequestMs = 60000;
// the bounds, each table's oldest entry dropped past them
constexpr size_t kMaxRequests = 256;
constexpr size_t kMaxArms = 64;
constexpr size_t kMaxJoined = 256;

// the acting player: the figure this seat holds for it (its local form id,
// 0 when it is absent here, finding 6: Game.GetPlayer then answers None in
// the scope) and its actor id as the server names it
struct Player
{
  uint32_t figure = 0;
  uint32_t serverId = 0;
};

// a scope: its player, and the arm that opened it (a sequence number, the
// lines' key: "the first three per scope")
struct Scope
{
  Player player;
  uint64_t seq = 0;
};

// where a request came from: setActingPlayer's relay, or the seam's
// propagation inside a scoped stack (the sink's line says which)
enum class Origin : uint8_t
{
  kRelayed,
  kChained
};

struct Taken
{
  Player player;
  Origin origin = Origin::kRelayed;
};

struct Joined
{
  Scope scope;
  // the stacks this arm joined so far (H-AP3's reading)
  uint32_t joins = 0;
};

// the steady clock in milliseconds (ScriptNatives.cpp's)
uint64_t NowMs();

// a request for the pair, replacing an older one, for ttlMs
void Request(uint32_t subject, int32_t stage, Player player, uint64_t ttlMs,
             Origin origin);
// the pair's request taken at its event: once (a taken request arms nothing
// again), nothing for none, a taken or an expired one
std::optional<Taken> Take(uint32_t subject, int32_t stage);
// getActingPlayer's read: the pair's request's server id until it expires,
// taken or not; 0 for none
uint32_t ServerIdOf(uint32_t subject, int32_t stage);
// any request, one atomic read: the sink asks it before it takes the lock
bool Requested();

// the subject armed for its player for ttlMs, replacing an older arm of the
// subject; the new scope answered for the caller's line
Scope Arm(uint32_t subject, Player player, uint64_t ttlMs);
// any arm, one atomic read
bool Armed();

// an arm or a joined stack, one relaxed load: OnCall's step (0) reads it for
// every native the game calls, and nothing more while it is false
bool Any();
// a joined stack's scope, nothing for a stack never joined or expired
std::optional<Scope> Of(RE::VMStackID stackId);
// the stack joined to the scope armed for its root frame's subject, nothing
// when no live arm names the subject
std::optional<Joined> Join(RE::VMStackID stackId, uint32_t rootSubject);

// the lines a scope makes, each kind throttled on its own, so a scope's
// joins and swaps never spend the first lines of its Game.GetPlayer returns
// (the line x-acting-player v1 claims)
enum class Line : uint8_t
{
  kJoin,
  kSwap,
  kReturn,
  kChain,
  kSkip
};
// a scope's line of that kind is due: its first three and every hundredth of
// the kind overall (the map of counts cleared past 1024 keys)
bool LineDue(uint64_t seq, Line line);

// the expired entries dropped, from the platform's per-frame tick
// (ScriptNatives::Tick); two atomic reads when every table is empty
void Sweep();
// every table emptied (a game load)
void Clear();

struct Counts
{
  size_t requests = 0;
  size_t arms = 0;
  size_t joined = 0;
};
// the tables' sizes, for the setter's line
Counts Count();

// PQ-3 (docs/research/rung5-quests-2026-11-09.md 8(a), read 18: world
// scripts call into the per-player quests' scripts, VC01's and C03Rampage's
// VampireCure among them): the globals each player keeps (PlayerGlobals
// 45's watch list, with the marked quests' globals composed into it:
// PlayerIsVampire, VampireFeedReady), replaced whole by setPlayerGlobals
// (PuppetApi.cpp). Inside another player's acting scope the seam skips a
// GlobalVariable.SetValue on one (ScriptNatives.cpp), so a relayed fragment
// that reaches a per-player script never writes this seat's own player's
// global. Mutex-guarded, the size kept for a read with no lock; empty by
// default
namespace PlayerGlobals {
void Set(const std::vector<uint32_t>& globalFormIds);
bool Is(uint32_t globalFormId);
size_t Count();
}

// rung 5's full build (Eli's D-Q2 word, "yes full transformation";
// docs/research/rung5-quests-2026-11-09.md 8(a)): the per-player quests, the
// server's derived playerQuests (PlayerVampireQuest, PlayerWerewolfQuest and
// the census's others), replaced whole by setPlayerQuests (PuppetApi.cpp).
// The rule is ownership: each seat runs its own player's per-player quests,
// so the Papyrus event gate (FridaHooks.cpp OnSendEventEnter) passes an
// event to such a quest's own scripts, or to its aliases' scripts, whatever
// their class (PlayerVampireQuestScript's OnUpdateGameTime,
// PlayerWerewolfChangeScript's timers), where the class-chain test would
// blank it; the server never relays such a quest's stages (E_QUEST_PLAYER),
// so no seat runs another player's copy. Mutex-guarded, the size kept for a
// read with no lock; empty by default
namespace PlayerQuests {
void Set(const std::vector<uint32_t>& questFormIds);
bool Is(uint32_t questFormId);
// any quest in the set, one atomic read: the gate asks it first
bool Any();
size_t Count();
}
}
