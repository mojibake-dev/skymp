#include "QuestCause.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <initializer_list>
#include <mutex>
#include <unordered_map>

namespace {
struct MarkEntry
{
  QuestCause::Verdict verdict;
  uint64_t createdMs = 0;
  uint64_t untilMs = 0;
  // when the word's event took it, 0 while untaken
  uint64_t takenMs = 0;
};

struct ArmEntry
{
  QuestCause::Verdict verdict;
  uint64_t createdMs = 0;
  uint64_t untilMs = 0;
};

struct JoinedEntry
{
  QuestCause::Verdict verdict;
  uint64_t joinedMs = 0;
  uint64_t untilMs = 0;
};

// the three tables under one mutex (QuestCause.h), their size beside them
// for a read with no lock, and the census's counts
struct
{
  std::unordered_map<uint64_t, MarkEntry> marks;
  std::unordered_map<uint32_t, ArmEntry> arms;
  std::unordered_map<RE::VMStackID, JoinedEntry> joined;
  std::mutex m;
  std::atomic<size_t> live{ 0 };
  std::atomic<uint32_t> world{ 0 };
  std::atomic<uint32_t> seat{ 0 };
  std::atomic<uint32_t> carried{ 0 };
} g_cause;

// the pair's key: the quest's form id high, the stage (a word below 0, as its
// 32 bits) low
uint64_t KeyOf(uint32_t quest, int32_t stage)
{
  return (static_cast<uint64_t>(quest) << 32) | static_cast<uint32_t>(stage);
}

// the size stored after a change, the lock held
void StoreSize()
{
  g_cause.live.store(g_cause.marks.size() + g_cause.arms.size() +
                     g_cause.joined.size());
}

// a table's expired entries erased, the lock held
template <class Map>
void EraseExpired(Map& map, uint64_t now)
{
  for (auto it = map.begin(); it != map.end();) {
    if (now >= it->second.untilMs) {
      it = map.erase(it);
    } else {
      ++it;
    }
  }
}

// a table past its bound loses its oldest entry (by `field`), the lock held
template <class Map, class Field>
void DropOldest(Map& map, size_t bound, Field field)
{
  while (map.size() >= bound && !map.empty()) {
    auto oldest = map.begin();
    for (auto it = map.begin(); it != map.end(); ++it) {
      if (field(it->second) < field(oldest->second)) {
        oldest = it;
      }
    }
    map.erase(oldest);
  }
}

bool Any()
{
  return g_cause.live.load(std::memory_order_relaxed) != 0;
}

// Papyrus's names compare without case
bool EqualsNoCase(std::string_view a, std::string_view b)
{
  return a.size() == b.size() &&
    std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) ==
             std::tolower(static_cast<unsigned char>(y));
         });
}

bool StartsWithNoCase(std::string_view text, std::string_view prefix)
{
  return text.size() >= prefix.size() &&
    EqualsNoCase(text.substr(0, prefix.size()), prefix);
}

// the seat-own roots by name (the run-cause read's report; the deployed load
// order's scripts by its index, idx/pex.json): lifecycle; cell load (the
// read's five, and OnAttachedToCell and OnDetachedFromCell, which 6 and 13
// deployed scripts handle); a magic effect's own events; the player's race
// switch (13 deployed scripts handle it); SKSE's input, menu and camera
// events (Form.psc of SKSE 2.2.6: OnKeyDown :168, OnKeyUp :171,
// OnControlDown :180, OnControlUp :183, OnMenuOpen :193, OnMenuClose :196,
// OnPlayerCameraState :220, OnCrosshairRefChange :228); and SkyUI's MCM
// (SKI_ConfigBase and SKI_QuestBase in the deployed order: its page, reload,
// version and dialog events and the state options' ST events; its OnConfig
// and OnOption events by prefix below)
constexpr std::string_view kSeatRoots[] = { "OnInit",
                                            "OnPlayerLoadGame",
                                            "OnReset",
                                            "OnLoad",
                                            "OnUnload",
                                            "OnCellLoad",
                                            "OnCellAttach",
                                            "OnCellDetach",
                                            "OnAttachedToCell",
                                            "OnDetachedFromCell",
                                            "OnEffectStart",
                                            "OnEffectFinish",
                                            "OnRaceSwitchComplete",
                                            "OnKeyDown",
                                            "OnKeyUp",
                                            "OnControlDown",
                                            "OnControlUp",
                                            "OnMenuOpen",
                                            "OnMenuClose",
                                            "OnPlayerCameraState",
                                            "OnCrosshairRefChange",
                                            "OnPageReset",
                                            "OnGameReload",
                                            "OnVersionUpdate",
                                            "OnVersionUpdateBase",
                                            "OnMessageDialogClose",
                                            "OnSelectST",
                                            "OnSliderOpenST",
                                            "OnSliderAcceptST",
                                            "OnMenuOpenST",
                                            "OnMenuAcceptST",
                                            "OnColorOpenST",
                                            "OnColorAcceptST",
                                            "OnKeyMapChangeST",
                                            "OnDefaultST",
                                            "OnHighlightST",
                                            "OnInputOpenST",
                                            "OnInputAcceptST" };

// the Story Manager's events (OnStoryScript, OnStoryKillActor, ...) and
// SkyUI's MCM config and option events
constexpr std::string_view kSeatPrefixes[] = { "OnStory", "OnConfig",
                                               "OnOption" };

// the direct timers (Form.psc :114 and :118)
constexpr std::string_view kTimerRoots[] = { "OnUpdate", "OnUpdateGameTime" };

// a fragment's kind by the Creation Kit's script name when its self's form
// type is unread: a topic info's, a scene's, a package's, a perk's (QF_, a
// quest's, needs its quest to carry a verdict, so it is no fallback)
constexpr std::string_view kWorldFragmentPrefixes[] = { "TIF_", "SF_", "PF_",
                                                        "PRKF_" };
}

uint64_t QuestCause::NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

QuestCause::Root QuestCause::Classify(std::string_view script,
                                      std::string_view function, Self self)
{
  if (StartsWithNoCase(function, "Fragment_")) {
    switch (self) {
      case Self::kQuest:
        return Root::kStageFragment;
      case Self::kInfo:
      case Self::kScene:
      case Self::kPackage:
      case Self::kPerk:
        return Root::kWorld;
      case Self::kNone:
        break;
    }
    for (const auto prefix : kWorldFragmentPrefixes) {
      if (StartsWithNoCase(script, prefix)) {
        return Root::kWorld;
      }
    }
    return Root::kUnknown;
  }
  for (const auto name : kTimerRoots) {
    if (EqualsNoCase(function, name)) {
      return Root::kTimer;
    }
  }
  for (const auto name : kSeatRoots) {
    if (EqualsNoCase(function, name)) {
      return Root::kSeat;
    }
  }
  for (const auto prefix : kSeatPrefixes) {
    if (StartsWithNoCase(function, prefix)) {
      return Root::kSeat;
    }
  }
  // every other event is an interaction (OnActivate, OnTriggerEnter, OnHit,
  // OnDeath, OnRead, OnLocationChange, ...)
  if (StartsWithNoCase(function, "On")) {
    return Root::kWorld;
  }
  return Root::kUnknown;
}

void QuestCause::Mark(uint32_t quest, int32_t stage, const Verdict& verdict,
                      uint64_t ttlMs)
{
  const uint64_t now = NowMs();
  const uint64_t key = KeyOf(quest, stage);
  std::lock_guard l(g_cause.m);
  if (g_cause.marks.count(key) == 0 && g_cause.marks.size() >= kMaxMarks) {
    EraseExpired(g_cause.marks, now);
    DropOldest(g_cause.marks, kMaxMarks,
               [](const MarkEntry& e) { return e.createdMs; });
  }
  MarkEntry& entry = g_cause.marks[key];
  entry.verdict = verdict;
  entry.createdMs = now;
  entry.untilMs = now + ttlMs;
  entry.takenMs = 0;
  StoreSize();
}

std::optional<QuestCause::Verdict> QuestCause::TakeWord(uint32_t quest,
                                                        int32_t word)
{
  if (!Any()) {
    return std::nullopt;
  }
  const uint64_t now = NowMs();
  std::lock_guard l(g_cause.m);
  const auto it = g_cause.marks.find(KeyOf(quest, word));
  if (it == g_cause.marks.end()) {
    return std::nullopt;
  }
  if (now >= it->second.untilMs) {
    g_cause.marks.erase(it);
    StoreSize();
    return std::nullopt;
  }
  if (it->second.takenMs != 0) {
    return std::nullopt;
  }
  it->second.takenMs = now;
  return it->second.verdict;
}

std::optional<QuestCause::Verdict> QuestCause::OfStage(uint32_t quest,
                                                       uint16_t stage)
{
  if (!Any()) {
    return std::nullopt;
  }
  const uint64_t now = NowMs();
  std::lock_guard l(g_cause.m);
  const auto own = g_cause.marks.find(KeyOf(quest, stage));
  if (own != g_cause.marks.end() && now < own->second.untilMs) {
    return own->second.verdict;
  }
  const MarkEntry* rides = nullptr;
  for (const int32_t word : { kStart, kStop }) {
    const auto it = g_cause.marks.find(KeyOf(quest, word));
    if (it == g_cause.marks.end() || now >= it->second.untilMs) {
      continue;
    }
    if (it->second.takenMs != 0 && now - it->second.takenMs > kRideMs) {
      continue;
    }
    if (!rides || it->second.createdMs > rides->createdMs) {
      rides = &it->second;
    }
  }
  if (!rides) {
    return std::nullopt;
  }
  return rides->verdict;
}

void QuestCause::Arm(uint32_t quest, const Verdict& verdict)
{
  const uint64_t now = NowMs();
  std::lock_guard l(g_cause.m);
  if (g_cause.arms.count(quest) == 0 && g_cause.arms.size() >= kMaxArms) {
    EraseExpired(g_cause.arms, now);
    DropOldest(g_cause.arms, kMaxArms,
               [](const ArmEntry& e) { return e.createdMs; });
  }
  ArmEntry& entry = g_cause.arms[quest];
  entry.verdict = verdict;
  entry.createdMs = now;
  entry.untilMs = now + kArmMs;
  StoreSize();
}

std::optional<QuestCause::Verdict> QuestCause::Of(RE::VMStackID stackId)
{
  if (!Any()) {
    return std::nullopt;
  }
  const uint64_t now = NowMs();
  std::lock_guard l(g_cause.m);
  const auto it = g_cause.joined.find(stackId);
  if (it == g_cause.joined.end()) {
    return std::nullopt;
  }
  if (now >= it->second.untilMs) {
    g_cause.joined.erase(it);
    StoreSize();
    return std::nullopt;
  }
  return it->second.verdict;
}

std::optional<QuestCause::Verdict> QuestCause::Join(RE::VMStackID stackId,
                                                    uint32_t quest,
                                                    const std::string& root)
{
  if (quest == 0 || !Any()) {
    return std::nullopt;
  }
  const uint64_t now = NowMs();
  std::lock_guard l(g_cause.m);
  const auto arm = g_cause.arms.find(quest);
  if (arm == g_cause.arms.end()) {
    return std::nullopt;
  }
  if (now >= arm->second.untilMs) {
    g_cause.arms.erase(arm);
    StoreSize();
    return std::nullopt;
  }
  if (g_cause.joined.count(stackId) == 0 &&
      g_cause.joined.size() >= kMaxJoined) {
    EraseExpired(g_cause.joined, now);
    DropOldest(g_cause.joined, kMaxJoined,
               [](const JoinedEntry& e) { return e.joinedMs; });
  }
  JoinedEntry& entry = g_cause.joined[stackId];
  entry.verdict.world = arm->second.verdict.world;
  entry.verdict.root = root;
  entry.joinedMs = now;
  entry.untilMs = now + kJoinedMs;
  StoreSize();
  return entry.verdict;
}

void QuestCause::Sweep()
{
  if (!Any()) {
    return;
  }
  const uint64_t now = NowMs();
  std::lock_guard l(g_cause.m);
  EraseExpired(g_cause.marks, now);
  EraseExpired(g_cause.arms, now);
  EraseExpired(g_cause.joined, now);
  StoreSize();
}

void QuestCause::ClearStacks()
{
  std::lock_guard l(g_cause.m);
  g_cause.joined.clear();
  StoreSize();
}

void QuestCause::Count(bool world, bool carried)
{
  (world ? g_cause.world : g_cause.seat)
    .fetch_add(1, std::memory_order_relaxed);
  if (carried) {
    g_cause.carried.fetch_add(1, std::memory_order_relaxed);
  }
}

QuestCause::Counts QuestCause::TakeCounts()
{
  Counts counts;
  counts.world = g_cause.world.exchange(0);
  counts.seat = g_cause.seat.exchange(0);
  counts.carried = g_cause.carried.exchange(0);
  return counts;
}
