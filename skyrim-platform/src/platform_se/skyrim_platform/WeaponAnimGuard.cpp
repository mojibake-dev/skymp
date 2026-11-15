#include "WeaponAnimGuard.h"
#include "CallNativeApi.h"

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
#include <unordered_set>

extern CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

namespace {
// the layout the guard reads, CommonLibSSE-NG's offsets held to the engine's
// own reads: AIProcess::middleHigh (include/RE/A/AIProcess.h:196; 39296 reads
// [rcx+0x8]), MiddleHighProcessData::unk1B8 and unk1F0 (include/RE/M/
// MiddleHighProcessData.h:163, :166; 39295 and 39296 read [mh+0x1b8] and
// [mh+0x1f0])
static_assert(offsetof(RE::AIProcess, middleHigh) == 0x8);
static_assert(offsetof(RE::MiddleHighProcessData, unk1B8) == 0x1B8);
static_assert(offsetof(RE::MiddleHighProcessData, unk1F0) == 0x1F0);
// the request in the record unk1B8 points to (REA, 1.6.1170: 39295 stores
// 39778's return at [record+0x40]; 40110, the callback the dispatcher calls,
// zeroes it at 0x1407217dd; 39296 zeroes it, cancelling only when the
// equipped ids differ from +0x1F8/+0x1FA and the loaded set's size at
// [record+0x58] (a BSTArray at +0x48, swapped with unk1D0 at 0x1406e043a,
// MiddleHighProcessData.h:164) is 0): HYPOTHESIS until the trace
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
};

// what the first line of each kind says, once per (kind, event)
enum class Event : std::uint8_t
{
  kDeferred,
  kRanAfter,
  kFolded,
  kDropped,
  kCount
};

struct State
{
  std::mutex m;
  std::array<Counts, kKinds> counts{};
  std::array<std::array<bool, static_cast<std::size_t>(Event::kCount)>, kKinds>
    said{};
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
      return "the copy keeps the engine's pick until its next load or fate";
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

// true the first time (kind, event) is met: the caller says it once
bool First(State& s, Kind kind, Event event)
{
  auto& said =
    s.said[static_cast<std::size_t>(kind)][static_cast<std::size_t>(event)];
  if (said) {
    return false;
  }
  said = true;
  return true;
}

// ADR-073 decision 7: after its first line each kind is counted, and the
// counts are flushed in one line, at most every 30 s, on the guard's next
// event that changed one (the platform has no periodic summary of its own)
void MaybeFlush(State& s)
{
  const auto now = std::chrono::steady_clock::now();
  if (!s.changed || now - s.flushed < std::chrono::seconds(30)) {
    return;
  }
  s.flushed = now;
  s.changed = false;
  const auto& r = s.counts[static_cast<std::size_t>(Kind::kReroll)];
  const auto& u = s.counts[static_cast<std::size_t>(Kind::kResurrect)];
  const auto& l = s.counts[static_cast<std::size_t>(Kind::kLoadGame)];
  logger::info("[verb:npc-fate] weapon-animation guard, counts since the "
               "start (deferred, ticks waited, ran after waiting, folded, "
               "dropped): re-roll {} {} {} {} {}; stand-up {} {} {} {} {}; "
               "apply {} {} {} {} {}",
               r.deferred, r.ticks, r.ranAfter, r.folded, r.dropped,
               u.deferred, u.ticks, u.ranAfter, u.folded, u.dropped,
               l.deferred, l.ticks, l.ranAfter, l.folded, l.dropped);
}
}

const void* WeaponAnimGuard::InFlightRequest(RE::Actor* actor,
                                             const void** callback)
{
  if (callback) {
    *callback = nullptr;
  }
  // the record's offset is 1.6.1170's (REA), the fate detours' runtime
  // (Fates::InstallHooks)
  if (!actor || !REL::Module::IsAE()) {
    return nullptr;
  }
  // Actor's currentProcess (include/RE/A/Actor.h:651)
  auto* process = actor->GetActorRuntimeData().currentProcess;
  auto* middleHigh = process ? process->middleHigh : nullptr;
  if (!middleHigh) {
    return nullptr;
  }
  // unk1B8's first word, BSTArray's allocator data (include/RE/B/
  // BSTArray.h:140, the allocator its first base, :374-375), read as the
  // engine reads it: a pointer to the record
  const std::uint8_t* record = nullptr;
  std::memcpy(&record, &middleHigh->unk1B8, sizeof(record));
  if (!record) {
    return nullptr;
  }
  if (callback) {
    *callback = middleHigh->unk1F0;
  }
  const void* request = nullptr;
  std::memcpy(&request, record + kRecordRequest, sizeof(request));
  return request;
}

Decision WeaponAnimGuard::Check(Kind kind, RE::Actor* actor,
                                std::uint32_t refrId, std::uint32_t tick,
                                std::function<void(std::uint32_t)> again)
{
  const void* callback = nullptr;
  const void* request = InFlightRequest(actor, &callback);
  auto& s = S();
  const auto i = static_cast<std::size_t>(kind);
  const auto key = Key(kind, refrId);
  {
    std::lock_guard lock(s.m);
    if (tick == 0 && s.waiting.count(key) != 0) {
      ++s.counts[i].folded;
      s.changed = true;
      if (First(s, kind, Event::kFolded)) {
        logger::info("{} weapon-animation guard: {} of {:x} folded into the "
                     "one already waiting, which reads the newest state when "
                     "it runs; the first of its kind, later ones counted",
                     Tag(kind), Name(kind), refrId);
      }
      MaybeFlush(s);
      return Decision::kFolded;
    }
    if (!request) {
      if (tick > 0) {
        s.waiting.erase(key);
        ++s.counts[i].ranAfter;
        s.changed = true;
        if (First(s, kind, Event::kRanAfter)) {
          logger::info("{} weapon-animation guard: {} of {:x} runs after {} "
                       "tick(s) of waiting, its request done; the first of "
                       "its kind, later ones counted",
                       Tag(kind), Name(kind), refrId, tick);
        }
      }
      MaybeFlush(s);
      return Decision::kRun;
    }
    if (tick >= kMaxTicks || !g_nativeCallRequirements.gameThrQ) {
      s.waiting.erase(key);
      ++s.counts[i].dropped;
      s.changed = true;
      if (First(s, kind, Event::kDropped)) {
        logger::warn("{} weapon-animation guard: {} of {:x} dropped, a "
                     "weapon-animation request {} still in flight after {} "
                     "tick(s){}: {}; the first of its kind, later ones "
                     "counted",
                     Tag(kind), Name(kind), refrId, fmt::ptr(request), tick,
                     g_nativeCallRequirements.gameThrQ ? ""
                                                       : " (no tick queue)",
                     Left(kind));
      }
      // a drop is flushed now, whatever the clock says
      s.flushed = {};
      MaybeFlush(s);
      return Decision::kDropped;
    }
    if (tick == 0) {
      ++s.counts[i].deferred;
    }
    ++s.counts[i].ticks;
    s.changed = true;
    s.waiting.insert(key);
    if (First(s, kind, Event::kDeferred)) {
      logger::info("{} weapon-animation guard: {} of {:x} deferred, a "
                   "weapon-animation request {} in flight on its process "
                   "(the callback {}); asked again each tick up to {} "
                   "(r-c3-temple-crash.md candidate A, HYPOTHESIS); the first "
                   "of its kind, later ones counted",
                   Tag(kind), Name(kind), refrId, fmt::ptr(request),
                   fmt::ptr(callback), kMaxTicks);
    }
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
