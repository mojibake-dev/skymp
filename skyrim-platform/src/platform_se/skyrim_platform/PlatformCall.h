#pragma once
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// thuum docs/DECISIONS.md ADR-062 decision 2 (slice 1b's reviews, 2026-11-03):
// the platform's own native calls reach the same entries the script hooks
// watch, so a hook must tell the platform's call from a script frame's.
// Two marks, one per path:
// - the direct path (callNative's worker inside OnUpdate, every non-latent
//   SpSnippet, the client applying the server's word through setOpen, lock,
//   activate, kill, setValue): the native body runs inside fPtr->Call on the
//   calling thread, so a thread-local depth (Scope) marks it;
// - the latent path (VmCall::Run, DispatchMethodCall2): the VM only enqueues
//   the call and runs the body later on its own thread under a new stack id,
//   so no thread or stack mark can see it; the call is noted here by (row,
//   self) before the dispatch and consumed by the hook at the entry
//   (NoteLatent, TakeLatent); the note is forgotten when the dispatch is
//   refused or when the call's stack returns (ForgetLatent), and a note
//   older than kLatentExpiryMs is dropped at the next look, the backstop.
//   Known limit: a note carries no call id, so a script calling the same
//   native on the same self inside the window takes the platform's note and
//   the platform's call goes out live (the counts are right, the attribution
//   swapped once).
namespace PlatformCall {
inline thread_local int t_depth = 0;

struct Scope
{
  Scope() { ++t_depth; }
  ~Scope() { --t_depth; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

inline bool Active()
{
  return t_depth > 0;
}

constexpr uint64_t kLatentExpiryMs = 15'000;

struct LatentNote
{
  uint64_t token;
  std::string key;
  const void* self;
  uint64_t atMs;
};

inline std::mutex& LatentMutex()
{
  static std::mutex m;
  return m;
}

inline std::vector<LatentNote>& LatentNotes()
{
  static std::vector<LatentNote> notes;
  return notes;
}

// the lower-case "Class.Function" keys the script hooks watch (ScriptNatives
// registers its rows at plugin load); a latent call outside them is never
// noted, so Utility.wait and kin cost nothing here
inline std::vector<std::string>& WatchedKeys()
{
  static std::vector<std::string> keys;
  return keys;
}

inline uint64_t LatentNowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// "Class.Function", lower case: the JS side spells the function in camel
// case and the VM's binding in its own
inline std::string LatentKey(const char* cls, const char* func)
{
  std::string key = std::string(cls ? cls : "") + "." + (func ? func : "");
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return key;
}

inline void WatchKey(const std::string& lowerKey)
{
  std::lock_guard<std::mutex> lock(LatentMutex());
  auto& keys = WatchedKeys();
  if (std::find(keys.begin(), keys.end(), lowerKey) == keys.end()) {
    keys.push_back(lowerKey);
  }
}

// the platform is about to dispatch a latent native of `cls.func` (the
// resolved function's own names) on `self`: the note's token, or 0 when the
// row is not watched or the self is null (nothing to take at the entry)
inline uint64_t NoteLatent(const char* cls, const char* func, const void* self)
{
  if (!self) {
    return 0;
  }
  const auto key = LatentKey(cls, func);
  const auto now = LatentNowMs();
  std::lock_guard<std::mutex> lock(LatentMutex());
  auto& keys = WatchedKeys();
  if (std::find(keys.begin(), keys.end(), key) == keys.end()) {
    return 0;
  }
  static uint64_t nextToken = 1;
  auto& notes = LatentNotes();
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [now](const LatentNote& n) {
                               return now - n.atMs > kLatentExpiryMs;
                             }),
              notes.end());
  const auto token = nextToken++;
  notes.push_back(LatentNote{ token, key, self, now });
  return token;
}

// the dispatch failed or the call's stack returned: the note is spent
inline void ForgetLatent(uint64_t token)
{
  if (token == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(LatentMutex());
  auto& notes = LatentNotes();
  notes.erase(
    std::remove_if(notes.begin(), notes.end(),
                   [token](const LatentNote& n) { return n.token == token; }),
    notes.end());
}

// the hook at the entry of `lowerKey` on `self`: the oldest live note for
// that pair, taken once (an aged note is dropped, not taken: the backstop)
inline bool TakeLatent(const std::string& lowerKey, const void* self)
{
  const auto now = LatentNowMs();
  std::lock_guard<std::mutex> lock(LatentMutex());
  auto& notes = LatentNotes();
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [now](const LatentNote& n) {
                               return now - n.atMs > kLatentExpiryMs;
                             }),
              notes.end());
  auto it = std::find_if(notes.begin(), notes.end(), [&](const LatentNote& n) {
    return n.self == self && n.key == lowerKey;
  });
  if (it == notes.end()) {
    return false;
  }
  notes.erase(it);
  return true;
}
}
