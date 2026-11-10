#pragma once

#include <RE/B/BSCoreTypes.h>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// thuum docs/DECISIONS.md ADR-068 rung 5, D-Q4 C (Eli, 2026-11-10, "Rung 5's
// stage-eve words"; the run-cause read's rule): a quest's run word, a start or
// a stop, counts as the world's only when a script's call with a WORLD cause
// makes it, not every engine start and stop. The ROOT of the stack that
// called Quest.Start, Quest.Stop or SetCurrentStageID decides (the seam,
// ScriptNatives.cpp OnCall, reads it), and the verdict rides onto the stages
// and words the call sets, as the acting scope rides PropagateScope: the
// quest stage sink (FridaHooks.cpp) arms the quest with its stage's verdict
// before the original dispatches the fragment, so a fragment's stack carries
// the verdict of whoever set its stage; the quest events (EventHandler.cpp)
// read the pair's mark and raise its cause and root to JS (questStart,
// questStop, questStage). ActingScope.h's idiom: mutex-guarded tables written
// with no game call, the sizes kept for a read with no lock. Three tables:
// - marks, (quest, stage or word) to a verdict for kMarkMs: written by the
//   seam at a Quest row; read by the stage's event and the sink; taken once
//   by the word's event (a later event of the word reads none)
// - arms, a quest to its latest stage's verdict for kArmMs: written by the
//   sink at every stage event, the engine's own (no mark) arming the seat's
//   own; a stack whose root frame is a fragment of the quest joins it at its
//   first Quest row (lazily: only a Quest row's verdict matters)
// - joined stacks, a stack id to its verdict for kJoinedMs; cleared at a game
//   load (a stack id names nothing in the new world), while marks and arms,
//   keyed by form ids, expire on their own
// The verdict classes (Classify, the read's root_kind with its report's
// lists): seat-own roots are lifecycle (OnInit, OnPlayerLoadGame, OnReset),
// cell load (OnLoad, OnUnload, OnCellLoad, OnCellAttach, OnCellDetach,
// OnAttachedToCell, OnDetachedFromCell), the Story Manager's (OnStory*), a
// direct timer (OnUpdate, OnUpdateGameTime), a magic effect (OnEffectStart,
// OnEffectFinish), the UI (SKSE's Form.psc input and menu events, SkyUI's MCM
// events), a race switch, an engine start's start-up fragment (a stage
// fragment no mark arms) and any stage set inside a seat-own chain; world
// roots are dialogue, scene, package and perk fragments, every other On*
// event (an interaction), and a stage fragment whose stage a world cause or a
// timer set (a timer's stage is the world's progress: TG02B, MGR22,
// TutorialAlchemy time their completion). A root no list names (a plain
// function a plugin dispatched, a fragment of no kind read) is the seat's
// own: the world's needs a positive read. HYPOTHESIS H-QC1 (the read's
// unverified list): a script-set stage's fragment runs on its own stack
// rooted at the fragment (x-acting-frame showed it for a platform-applied
// stage only); H-QC2: a word's event lands inside its mark's window
// (x-quest-cause v1 reads both)
namespace QuestCause {

// a pair's stage beside a quest stage's 0 to 65535 (ActingScope's spelling)
constexpr int32_t kStart = -1;
constexpr int32_t kStop = -2;

// a mark's life: the acting scope's chained 10 s, so a stage the VM queues on
// an unstarted quest still finds it (UNMEASURED)
constexpr uint64_t kMarkMs = 10000;
// how long a taken start or stop still lends its verdict to the start-up or
// shut-down stages the engine sets with it (their events may come after the
// word's; UNMEASURED)
constexpr uint64_t kRideMs = 2000;
// an arm's life: the join is lazy (a fragment's first Quest row), so it
// outlives a fragment's short waits (UNMEASURED)
constexpr uint64_t kArmMs = 30000;
// a joined stack keeps its verdict this long at most
constexpr uint64_t kJoinedMs = 120000;
// the bounds, each table's oldest entry dropped past them
constexpr size_t kMaxMarks = 1024;
constexpr size_t kMaxArms = 256;
constexpr size_t kMaxJoined = 512;

// a call's verdict: a world cause or the seat's own, and the root frame of
// the stack that made it, "<Script>.<Function>" (empty for no script frame)
struct Verdict
{
  bool world = false;
  std::string root;
};

// a root frame's class
enum class Root : uint8_t
{
  kWorld,
  kTimer,
  kSeat,
  kStageFragment,
  kUnknown
};

// a Fragment_ root's self, by its form type
enum class Self : uint8_t
{
  kNone,
  kQuest,
  kInfo,
  kScene,
  kPackage,
  kPerk
};

// the root frame's class from its script's and function's names (Papyrus's,
// compared without case) and its self's kind
Root Classify(std::string_view script, std::string_view function, Self self);

// the steady clock in milliseconds
uint64_t NowMs();

// a Quest row's verdict for the pair it sets, replacing an older mark
void Mark(uint32_t quest, int32_t stage, const Verdict& verdict);
// the word's event (kStart or kStop): the pair's mark, taken once; nothing for
// none, a taken or an expired one
std::optional<Verdict> TakeWord(uint32_t quest, int32_t word);
// a stage's event and the sink: the stage's own mark; else the quest's start
// or stop mark (a start-up or shut-down stage rides its word) while untaken or
// within kRideMs of its take, the newer of the two; nothing for none
std::optional<Verdict> OfStage(uint32_t quest, uint16_t stage);
// the sink: the quest's fragment verdict for kArmMs, replacing an older arm
void Arm(uint32_t quest, const Verdict& verdict);
// a joined stack's verdict, nothing for one never joined or expired
std::optional<Verdict> Of(RE::VMStackID stackId);
// the stack joined to the quest's live arm, its root kept as named; nothing
// for no live arm (the caller reads the seat's own)
std::optional<Verdict> Join(RE::VMStackID stackId, uint32_t quest,
                            const std::string& root);

// the expired entries dropped, from the platform's per-frame tick; two atomic
// reads when every table is empty
void Sweep();
// the joined stacks dropped (a game load)
void ClearStacks();

// the census's window: the marks made since the last read, world and seat,
// and how many of them a fragment's carried verdict decided; read and zeroed
struct Counts
{
  uint32_t world = 0;
  uint32_t seat = 0;
  uint32_t carried = 0;
};
void Count(bool world, bool carried);
Counts TakeCounts();
}
