#include "WeaponAnimGuard.h"
#include "CallNativeApi.h"
#include "ChangeForm.h"

#include <RE/A/AIProcess.h>
#include <RE/A/Actor.h>
#include <RE/M/MiddleHighProcessData.h>
#include <REL/Relocation.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <fmt/format.h>
#include <mutex>
#include <string>
#include <unordered_set>

extern CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

namespace {
// the layout the guard reads, CommonLibSSE-NG's offsets held to the engine's
// own reads: AIProcess::middleHigh (include/RE/A/AIProcess.h:196; 39296 reads
// [rcx+0x8]), MiddleHighProcessData::unk1B8 and unk1F0 (include/RE/M/
// MiddleHighProcessData.h:163, :166; 39295, 39296 and 39432 read [mh+0x1b8]
// and [mh+0x1f0])
static_assert(offsetof(RE::AIProcess, middleHigh) == 0x8);
static_assert(offsetof(RE::MiddleHighProcessData, unk1B8) == 0x1B8);
static_assert(offsetof(RE::MiddleHighProcessData, unk1F0) == 0x1F0);
// the request in the record unk1B8 points to (1.6.1170, the ghidra MCP on
// sky-re: 39295 stores 39778's return at [record+0x40] under the lock; when
// the weapons differ from +0x1F8/+0x1FA it cancels the one there (63971)
// only if the loaded set's size at [record+0x58] is 0, and otherwise
// releases its listener at unk1F0 uncancelled for its own (40109's); 40110
// zeroes the handle at 0x1407217dd; 39296 zeroes it and releases unk1F0):
// HYPOTHESIS until the trace
constexpr std::size_t kRecordRequest = 0x40;

using WeaponAnimGuard::Decision;
using WeaponAnimGuard::Kind;

constexpr std::size_t kKinds = static_cast<std::size_t>(Kind::kCount);

struct Counts
{
  std::uint64_t deferred = 0; // works deferred at their first ask
  std::uint64_t ticks = 0;    // deferrals in all, every tick counted
  std::uint64_t ranAfter = 0; // deferred works that then ran
  std::uint64_t folded = 0;   // asks folded into a waiting work
  std::uint64_t dropped = 0;  // works dropped at kMaxTicks
  std::uint64_t held = 0;     // works a dead copy's record apply took
};

struct State
{
  std::mutex m;
  std::array<Counts, kKinds> counts{};
  std::unordered_set<std::uint64_t> waiting;
  std::chrono::steady_clock::time_point flushed{};
  bool changed = false;
};

State& S()
{
  static State s;
  return s;
}

const char* Tag(Kind kind)
{
  return kind == Kind::kLoadGame ? "[verb:change-form]" : "[verb:npc-fate]";
}

const char* Name(Kind kind)
{
  switch (kind) {
    case Kind::kReroll:
      return "the re-roll";
    case Kind::kRedress:
      return "the redress";
    case Kind::kBody:
      return "the body's dress";
    case Kind::kResurrect:
      return "the stand-up";
    case Kind::kLoadGame:
      return "the record's apply (Actor::LoadGame)";
    default:
      return "the work";
  }
}

// what a dropped work leaves, said in its line
const char* Left(Kind kind)
{
  switch (kind) {
    case Kind::kReroll:
    case Kind::kRedress:
      return "the copy keeps its dress and inventory until its next load or "
             "fate";
    case Kind::kBody:
      return "the body's record armor stays unworn until its next apply";
    case Kind::kResurrect:
      return "the copy stays down until the server's next respawn";
    case Kind::kLoadGame:
      return "the apply is answered refused";
    default:
      return "nothing done";
  }
}

std::uint64_t Key(Kind kind, std::uint32_t refrId)
{
  return (static_cast<std::uint64_t>(kind) << 32) | refrId;
}

// ADR-073 decision 7: every event is counted, and the counts are flushed in
// one line, at most every 30 s, on the guard's next event that changed one
// (the platform has no periodic summary of its own)
void MaybeFlush(State& s)
{
  const auto now = std::chrono::steady_clock::now();
  if (!s.changed || now - s.flushed < std::chrono::seconds(30)) {
    return;
  }
  s.flushed = now;
  s.changed = false;
  std::string line;
  for (std::size_t i = 0; i < kKinds; ++i) {
    const auto& c = s.counts[i];
    line += fmt::format("{}{} {} {} {} {} {} {}", i == 0 ? "" : "; ",
                        Name(static_cast<Kind>(i)), c.deferred, c.ticks,
                        c.ranAfter, c.folded, c.dropped, c.held);
  }
  logger::info("[verb:npc-fate] weapon-animation guard, counts since the "
               "start (deferred, ticks waited, ran after waiting, folded, "
               "dropped, held for the apply): {}",
               line);
}
}

WeaponAnimGuard::InFlight WeaponAnimGuard::Peek(RE::Actor* actor)
{
  InFlight out;
  // the record's offset is 1.6.1170's (REA, the ghidra MCP), the fate
  // detours' runtime (Fates::InstallHooks)
  if (!actor || !REL::Module::IsAE()) {
    return out;
  }
  // Actor's currentProcess (include/RE/A/Actor.h:651)
  auto* process = actor->GetActorRuntimeData().currentProcess;
  auto* middleHigh = process ? process->middleHigh : nullptr;
  if (!middleHigh) {
    return out;
  }
  // the listener first: it outlives the record's handle (40110 zeroes the
  // handle before 39296 releases the listener), and it is the one word
  // 39432 reads
  out.listener = middleHigh->unk1F0;
  // unk1B8's first word, BSTArray's allocator data (include/RE/B/
  // BSTArray.h:140, the allocator its first base, :374-375), read as the
  // engine reads it: a pointer to the record
  const std::uint8_t* record = nullptr;
  std::memcpy(&record, &middleHigh->unk1B8, sizeof(record));
  if (record) {
    std::memcpy(&out.request, record + kRecordRequest, sizeof(out.request));
  }
  return out;
}

Decision WeaponAnimGuard::Check(Kind kind, RE::Actor* actor,
                                std::uint32_t refrId, std::uint32_t tick,
                                std::function<void(std::uint32_t)> again,
                                std::function<void()> owed)
{
  auto& s = S();
  const auto i = static_cast<std::size_t>(kind);
  const auto key = Key(kind, refrId);
  {
    std::lock_guard lock(s.m);
    if (tick == 0 && s.waiting.count(key) != 0) {
      ++s.counts[i].folded;
      s.changed = true;
      logger::info("{} weapon-animation guard: {} of {:x} folded into the "
                   "one already waiting, which reads the newest state when "
                   "it runs",
                   Tag(kind), Name(kind), refrId);
      MaybeFlush(s);
      return Decision::kFolded;
    }
  }
  // a dead copy's dress while its record's apply is queued or running: the
  // apply's LoadGame and FinishLoadGame lay the record's worn list (ADR-065),
  // so a second pass now would only make a request the finish pass meets
  // (051655's order). Dead is Actor::IsDead (include/RE/A/Actor.h:345), the
  // reconcile's own test; asked outside the guard's lock, since
  // HoldForApply takes ChangeForm's
  if (owed && actor && actor->IsDead() &&
      ChangeForm::HoldForApply(refrId, Name(kind), std::move(owed))) {
    std::lock_guard lock(s.m);
    s.waiting.erase(key);
    ++s.counts[i].held;
    s.changed = true;
    logger::info("{} weapon-animation guard: {} of {:x} held for the record's "
                 "apply of it, queued or running: the lay restores its worn "
                 "list and the client's reconcile after the lay is its one "
                 "pass; run again if the apply is refused{}",
                 Tag(kind), Name(kind), refrId,
                 tick > 0 ? fmt::format(" (after {} tick(s) of waiting)", tick)
                          : std::string());
    MaybeFlush(s);
    return Decision::kHeld;
  }
  const InFlight inFlight = Peek(actor);
  {
    std::lock_guard lock(s.m);
    if (!inFlight.Any()) {
      if (tick > 0) {
        s.waiting.erase(key);
        ++s.counts[i].ranAfter;
        s.changed = true;
        logger::info("{} weapon-animation guard: {} of {:x} runs after {} "
                     "tick(s) of waiting, the request and its listener gone",
                     Tag(kind), Name(kind), refrId, tick);
      }
      MaybeFlush(s);
      return Decision::kRun;
    }
    if (tick >= kMaxTicks || !g_nativeCallRequirements.gameThrQ) {
      s.waiting.erase(key);
      ++s.counts[i].dropped;
      s.changed = true;
      logger::warn("{} weapon-animation guard: {} of {:x} dropped, a "
                   "weapon-animation request outstanding after {} tick(s) "
                   "(the request {}, its listener {}){}: {}",
                   Tag(kind), Name(kind), refrId, tick,
                   fmt::ptr(inFlight.request), fmt::ptr(inFlight.listener),
                   g_nativeCallRequirements.gameThrQ ? "" : " (no tick queue)",
                   Left(kind));
      // a drop is flushed now, whatever the clock says
      s.flushed = {};
      MaybeFlush(s);
      return Decision::kDropped;
    }
    if (tick == 0) {
      ++s.counts[i].deferred;
      logger::info("{} weapon-animation guard: {} of {:x} deferred, a "
                   "weapon-animation request outstanding on its process (the "
                   "request {}, its listener {}); asked again each tick up "
                   "to {}",
                   Tag(kind), Name(kind), refrId, fmt::ptr(inFlight.request),
                   fmt::ptr(inFlight.listener), kMaxTicks);
    }
    ++s.counts[i].ticks;
    s.changed = true;
    s.waiting.insert(key);
    MaybeFlush(s);
  }
  // Viet::TaskQueue::Update moves its tasks out before it runs them
  // (viet/include/TaskQueue.h), so a task added here runs on the next
  // drain, never this one; added outside the guard's lock
  g_nativeCallRequirements.gameThrQ->AddTask(
    [again = std::move(again), tick](Viet::Void) { again(tick + 1); });
  return Decision::kDeferred;
}

void WeaponAnimGuard::Forget(Kind kind, std::uint32_t refrId)
{
  auto& s = S();
  std::lock_guard lock(s.m);
  s.waiting.erase(Key(kind, refrId));
}
