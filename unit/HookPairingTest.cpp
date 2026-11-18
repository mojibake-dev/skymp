#include "../skyrim-platform/src/platform_se/skyrim_platform/HookPairing.h"
#include <catch2/catch_all.hpp>
#include <cstddef>

// thuum tracker #129: Hook::Leave crosses to the JavaScript thread only
// after an enter of the same hook that crossed on its thread, so the
// JavaScript side's per-thread state opens and closes in pairs. The notes
// that pair them (HookPairing.h) need no game: an enter that crossed, one
// the native match kept back or whose hand-off was declined, one its caller
// returned from before the match (Hook::SkipEnter), nesting, and the cap.
// The header is std only, so the server's T0 build reaches it by path, as
// HandOffTest.cpp reaches HandOff.h
namespace {
// two hooks' identities, as Hook.cpp's `this` (distinct objects)
const int kAnimationHook = 1;
const int kOtherHook = 2;
}

TEST_CASE("A leave crosses after an enter that crossed, and after no other",
          "[HookPairing]")
{
  HookPairing::EnterNotes notes;
  // no enter at all: the leave stays in native
  REQUIRE_FALSE(notes.Take(&kAnimationHook));

  // the native match kept the enter back, or its hand-off was declined
  notes.Note(&kAnimationHook, false);
  REQUIRE_FALSE(notes.Take(&kAnimationHook));

  notes.Note(&kAnimationHook, true);
  REQUIRE(notes.Take(&kAnimationHook));
  REQUIRE(notes.Size() == 0);
}

TEST_CASE("A nested event whose enter returned early takes its own note, not "
          "the outer enter's",
          "[HookPairing]")
{
  HookPairing::EnterNotes notes;
  // the outer event crossed; inside its hooked call the engine sent a second
  // event, whose enter FridaHooks.cpp returned from (an empty name) through
  // SkipEnter
  notes.Note(&kAnimationHook, true);
  notes.Note(&kAnimationHook, false);
  // the inner leave stays in native, and the outer leave still crosses
  REQUIRE_FALSE(notes.Take(&kAnimationHook));
  REQUIRE(notes.Take(&kAnimationHook));
  REQUIRE(notes.Size() == 0);
}

TEST_CASE("Each hook takes its own notes", "[HookPairing]")
{
  HookPairing::EnterNotes notes;
  notes.Note(&kAnimationHook, true);
  notes.Note(&kOtherHook, false);
  REQUIRE(notes.Take(&kAnimationHook));
  REQUIRE_FALSE(notes.Take(&kOtherHook));
  REQUIRE(notes.Size() == 0);
}

TEST_CASE("Past the cap the oldest note goes and the live ones stay",
          "[HookPairing]")
{
  HookPairing::EnterNotes notes;
  // an enter whose leave never came (a throw through the hooked call)
  notes.Note(&kOtherHook, true);
  for (std::size_t i = 1; i < HookPairing::EnterNotes::kCap; ++i) {
    notes.Note(&kAnimationHook, false);
  }
  REQUIRE(notes.Size() == HookPairing::EnterNotes::kCap);
  // a live outer enter that crossed, noted at the cap
  notes.Note(&kAnimationHook, true);
  REQUIRE(notes.Size() == HookPairing::EnterNotes::kCap);
  REQUIRE(notes.Take(&kAnimationHook));
  // the stale note was the one dropped
  REQUIRE_FALSE(notes.Take(&kOtherHook));
  REQUIRE(notes.Size() == HookPairing::EnterNotes::kCap - 1);
}
