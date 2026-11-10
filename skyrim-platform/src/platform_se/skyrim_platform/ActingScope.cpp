#include "ActingScope.h"
#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace {
struct RequestEntry
{
  ActingScope::Player player;
  ActingScope::Origin origin = ActingScope::Origin::kRelayed;
  uint64_t createdMs = 0;
  uint64_t untilMs = 0;
  bool taken = false;
};

struct ArmEntry
{
  ActingScope::Scope scope;
  uint64_t createdMs = 0;
  uint64_t untilMs = 0;
  uint32_t joins = 0;
};

struct JoinedEntry
{
  ActingScope::Scope scope;
  uint64_t joinedMs = 0;
  uint64_t untilMs = 0;
};

// the three tables under one mutex (ActingScope.h), the sizes kept beside
// them for a read with no lock: `live` the arms and the joined stacks
// together (Any), `armed` the arms alone, `requested` the requests
struct
{
  std::unordered_map<uint64_t, RequestEntry> requests;
  std::unordered_map<uint32_t, ArmEntry> arms;
  std::unordered_map<RE::VMStackID, JoinedEntry> joined;
  // the lines' counts per (scope, kind) (LineDue), and each kind's total
  std::unordered_map<uint64_t, uint32_t> lines;
  std::array<uint64_t, 8> lineTotals{};
  uint64_t nextSeq = 1;
  std::mutex m;
  std::atomic<size_t> live{ 0 };
  std::atomic<size_t> armed{ 0 };
  std::atomic<size_t> requested{ 0 };
} g_scope;

// rung 5's two per-player sets (ActingScope.h, PlayerGlobals and
// PlayerQuests), Hosted.h's idiom
struct IdSet
{
  std::unordered_set<uint32_t> ids;
  std::mutex m;
  std::atomic<size_t> size{ 0 };
};

IdSet g_playerGlobals;
IdSet g_playerQuests;

// a set replaced whole, 0 dropped
void ReplaceIds(IdSet& set, const std::vector<uint32_t>& ids)
{
  std::lock_guard l(set.m);
  set.ids.clear();
  for (const uint32_t id : ids) {
    if (id != 0) {
      set.ids.insert(id);
    }
  }
  set.size.store(set.ids.size());
}

bool HoldsId(IdSet& set, uint32_t id)
{
  if (set.size.load(std::memory_order_relaxed) == 0) {
    return false;
  }
  std::lock_guard l(set.m);
  return set.ids.count(id) != 0;
}

// the pair's key: the subject's form id high, the stage (a word or an entry
// below 0, as its 32 bits) low
uint64_t KeyOf(uint32_t subject, int32_t stage)
{
  return (static_cast<uint64_t>(subject) << 32) | static_cast<uint32_t>(stage);
}

// the sizes stored after a change, the lock held
void StoreSizes()
{
  g_scope.live.store(g_scope.arms.size() + g_scope.joined.size());
  g_scope.armed.store(g_scope.arms.size());
  g_scope.requested.store(g_scope.requests.size());
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
}

uint64_t ActingScope::NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

void ActingScope::Request(uint32_t subject, int32_t stage, Player player,
                          uint64_t ttlMs, Origin origin)
{
  const uint64_t now = NowMs();
  const uint64_t key = KeyOf(subject, stage);
  std::lock_guard l(g_scope.m);
  if (g_scope.requests.count(key) == 0 &&
      g_scope.requests.size() >= kMaxRequests) {
    EraseExpired(g_scope.requests, now);
    DropOldest(g_scope.requests, kMaxRequests,
               [](const RequestEntry& e) { return e.createdMs; });
  }
  RequestEntry& entry = g_scope.requests[key];
  entry.player = player;
  entry.origin = origin;
  entry.createdMs = now;
  entry.untilMs = now + ttlMs;
  entry.taken = false;
  StoreSizes();
}

std::optional<ActingScope::Taken> ActingScope::Take(uint32_t subject,
                                                    int32_t stage)
{
  if (!Requested()) {
    return std::nullopt;
  }
  const uint64_t now = NowMs();
  std::lock_guard l(g_scope.m);
  const auto it = g_scope.requests.find(KeyOf(subject, stage));
  if (it == g_scope.requests.end()) {
    return std::nullopt;
  }
  if (now >= it->second.untilMs) {
    g_scope.requests.erase(it);
    StoreSizes();
    return std::nullopt;
  }
  if (it->second.taken) {
    return std::nullopt;
  }
  it->second.taken = true;
  return Taken{ it->second.player, it->second.origin };
}

uint32_t ActingScope::ServerIdOf(uint32_t subject, int32_t stage)
{
  if (!Requested()) {
    return 0;
  }
  const uint64_t now = NowMs();
  std::lock_guard l(g_scope.m);
  const auto it = g_scope.requests.find(KeyOf(subject, stage));
  if (it == g_scope.requests.end()) {
    return 0;
  }
  if (now >= it->second.untilMs) {
    g_scope.requests.erase(it);
    StoreSizes();
    return 0;
  }
  return it->second.player.serverId;
}

bool ActingScope::Requested()
{
  return g_scope.requested.load(std::memory_order_relaxed) != 0;
}

ActingScope::Scope ActingScope::Arm(uint32_t subject, Player player,
                                    uint64_t ttlMs)
{
  const uint64_t now = NowMs();
  std::lock_guard l(g_scope.m);
  if (g_scope.arms.count(subject) == 0 && g_scope.arms.size() >= kMaxArms) {
    EraseExpired(g_scope.arms, now);
    DropOldest(g_scope.arms, kMaxArms,
               [](const ArmEntry& e) { return e.createdMs; });
  }
  ArmEntry& entry = g_scope.arms[subject];
  entry.scope.player = player;
  entry.scope.seq = g_scope.nextSeq++;
  entry.createdMs = now;
  entry.untilMs = now + ttlMs;
  entry.joins = 0;
  StoreSizes();
  return entry.scope;
}

bool ActingScope::Armed()
{
  return g_scope.armed.load(std::memory_order_relaxed) != 0;
}

bool ActingScope::Any()
{
  return g_scope.live.load(std::memory_order_relaxed) != 0;
}

std::optional<ActingScope::Scope> ActingScope::Of(RE::VMStackID stackId)
{
  if (!Any()) {
    return std::nullopt;
  }
  const uint64_t now = NowMs();
  std::lock_guard l(g_scope.m);
  const auto it = g_scope.joined.find(stackId);
  if (it == g_scope.joined.end()) {
    return std::nullopt;
  }
  if (now >= it->second.untilMs) {
    g_scope.joined.erase(it);
    StoreSizes();
    return std::nullopt;
  }
  return it->second.scope;
}

std::optional<ActingScope::Joined> ActingScope::Join(RE::VMStackID stackId,
                                                     uint32_t rootSubject)
{
  if (!Armed() || rootSubject == 0) {
    return std::nullopt;
  }
  const uint64_t now = NowMs();
  std::lock_guard l(g_scope.m);
  const auto arm = g_scope.arms.find(rootSubject);
  if (arm == g_scope.arms.end()) {
    return std::nullopt;
  }
  if (now >= arm->second.untilMs) {
    g_scope.arms.erase(arm);
    StoreSizes();
    return std::nullopt;
  }
  if (g_scope.joined.count(stackId) == 0 &&
      g_scope.joined.size() >= kMaxJoined) {
    EraseExpired(g_scope.joined, now);
    DropOldest(g_scope.joined, kMaxJoined,
               [](const JoinedEntry& e) { return e.joinedMs; });
  }
  JoinedEntry& entry = g_scope.joined[stackId];
  entry.scope = arm->second.scope;
  entry.joinedMs = now;
  entry.untilMs = now + kJoinedMs;
  const uint32_t joins = ++arm->second.joins;
  StoreSizes();
  return Joined{ entry.scope, joins };
}

bool ActingScope::LineDue(uint64_t seq, Line line)
{
  const auto kind = static_cast<size_t>(line) & 7;
  const uint64_t key = (seq << 3) | kind;
  std::lock_guard l(g_scope.m);
  if (g_scope.lines.size() > 1024 && g_scope.lines.count(key) == 0) {
    g_scope.lines.clear();
  }
  const uint32_t n = ++g_scope.lines[key];
  const uint64_t total = ++g_scope.lineTotals[kind];
  return n <= 3 || total % 100 == 0;
}

void ActingScope::Sweep()
{
  if (!Any() && !Requested()) {
    return;
  }
  const uint64_t now = NowMs();
  std::lock_guard l(g_scope.m);
  EraseExpired(g_scope.requests, now);
  EraseExpired(g_scope.arms, now);
  EraseExpired(g_scope.joined, now);
  StoreSizes();
}

void ActingScope::Clear()
{
  std::lock_guard l(g_scope.m);
  g_scope.requests.clear();
  g_scope.arms.clear();
  g_scope.joined.clear();
  g_scope.lines.clear();
  StoreSizes();
}

ActingScope::Counts ActingScope::Count()
{
  std::lock_guard l(g_scope.m);
  Counts counts;
  counts.requests = g_scope.requests.size();
  counts.arms = g_scope.arms.size();
  counts.joined = g_scope.joined.size();
  return counts;
}

void ActingScope::PlayerGlobals::Set(
  const std::vector<uint32_t>& globalFormIds)
{
  ReplaceIds(g_playerGlobals, globalFormIds);
}

bool ActingScope::PlayerGlobals::Is(uint32_t globalFormId)
{
  return HoldsId(g_playerGlobals, globalFormId);
}

size_t ActingScope::PlayerGlobals::Count()
{
  return g_playerGlobals.size.load(std::memory_order_relaxed);
}

void ActingScope::PlayerQuests::Set(const std::vector<uint32_t>& questFormIds)
{
  ReplaceIds(g_playerQuests, questFormIds);
}

bool ActingScope::PlayerQuests::Is(uint32_t questFormId)
{
  return HoldsId(g_playerQuests, questFormId);
}

bool ActingScope::PlayerQuests::Any()
{
  return g_playerQuests.size.load(std::memory_order_relaxed) != 0;
}

size_t ActingScope::PlayerQuests::Count()
{
  return g_playerQuests.size.load(std::memory_order_relaxed);
}
