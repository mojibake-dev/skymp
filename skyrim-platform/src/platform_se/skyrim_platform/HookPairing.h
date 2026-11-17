#pragma once
#include <cstddef>
#include <iterator>
#include <vector>

// thuum tracker #129: a hook whose leave crosses too (the animation hook)
// pairs each leave with its own enter on the hooked thread. Every enter that
// reaches the platform leaves one note, whether it crossed to the JavaScript
// thread: one that crossed, one the native match kept back, one whose
// hand-off was declined, and one its caller returned from before the match
// (FridaHooks.cpp's null reference or empty name, through Hook::SkipEnter),
// since that call's leave still comes. A leave takes the newest note of its
// hook and crosses only after an enter that did, so the JavaScript side's
// per-thread state (inProgressThreads, each handler's context) opens and
// closes in pairs, and a nested event's leave never takes the outer one's.
//
// Std only, so the server's T0 build reaches it by path
// (unit/HookPairingTest.cpp), as HandOff.h is.
namespace HookPairing {

class EnterNotes
{
public:
  // enough for any real nesting; past it the oldest note goes, an enter
  // whose leave never came (a throw through the hooked call)
  static constexpr std::size_t kCap = 64;

  // an enter's note: its hook, and whether it crossed
  void Note(const void* hook, bool crossed)
  {
    if (notes.size() >= kCap) {
      notes.erase(notes.begin());
    }
    notes.push_back(Entry{ hook, crossed });
  }

  // a leave's: the newest note of its hook, taken, and whether that enter
  // crossed; false with no note of the hook
  bool Take(const void* hook)
  {
    for (auto it = notes.rbegin(); it != notes.rend(); ++it) {
      if (it->hook == hook) {
        const bool crossed = it->crossed;
        notes.erase(std::next(it).base());
        return crossed;
      }
    }
    return false;
  }

  std::size_t Size() const { return notes.size(); }

private:
  struct Entry
  {
    const void* hook = nullptr;
    bool crossed = false;
  };
  std::vector<Entry> notes;
};
}
