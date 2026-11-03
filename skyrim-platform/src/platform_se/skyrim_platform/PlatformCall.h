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
//   (NoteLatent, TakeLatent). A note nobody consumes expires after
//   kLatentExpiryMs (a refused or failed dispatch).
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

// the platform is about to dispatch a latent native on `self`
inline void NoteLatent(const char* cls, const char* func, const void* self)
{
  const auto now = LatentNowMs();
  std::lock_guard<std::mutex> lock(LatentMutex());
  auto& notes = LatentNotes();
  notes.erase(std::remove_if(notes.begin(), notes.end(),
                             [now](const LatentNote& n) {
                               return now - n.atMs > kLatentExpiryMs;
                             }),
              notes.end());
  notes.push_back(LatentNote{ LatentKey(cls, func), self, now });
}

// the hook at the entry of `cls.func` on `self`: true once per note
inline bool TakeLatent(const std::string& lowerKey, const void* self)
{
  std::lock_guard<std::mutex> lock(LatentMutex());
  auto& notes = LatentNotes();
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
