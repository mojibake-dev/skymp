#include "../skyrim-platform/src/platform_se/skyrim_platform/PlatformCall.h"
#include <atomic>
#include <catch2/catch_all.hpp>
#include <cstdint>
#include <thread>

// thuum docs/DECISIONS.md D-S0-14 B (2026-11-14): the Papyrus event gate and
// the animation hooks tell the platform's own call by PlatformCall's
// thread-local mark (FridaHooks.cpp), in place of callNative's process-wide
// Override, and callNative's non-latent branch raises it on the JavaScript
// thread (CallNativeApi.cpp). The gate's verdict rests on three properties
// of the mark that need no game: another thread never reads this thread's
// call (read 1's H1a: an engine event on another thread passed while a call
// ran), the first of two overlapping calls to end leaves the other's mark
// standing (R11), and a dispatched call's level raises the mark only when
// the seam marks it the platform's (ScriptNatives.cpp's Call hook). The
// header is std only, so the server's T0 build reaches it by path
namespace {
bool ActiveOnAnotherThread()
{
  std::atomic<bool> seen{ true };
  std::thread other([&seen] { seen = PlatformCall::Active(); });
  other.join();
  return seen;
}
}

TEST_CASE("PlatformCall's mark is the calling thread's alone (D-S0-14 B)",
          "[PlatformCall]")
{
  REQUIRE_FALSE(PlatformCall::Active());
  {
    PlatformCall::Scope call;
    REQUIRE(PlatformCall::Active());
    REQUIRE_FALSE(ActiveOnAnotherThread());
  }
  REQUIRE_FALSE(PlatformCall::Active());
}

TEST_CASE("PlatformCall's mark outlives the first of two overlapping calls "
          "(R11)",
          "[PlatformCall]")
{
  // nested on one thread: callNative's Scope around a call whose body runs
  // on the same thread
  {
    PlatformCall::Scope outer;
    {
      PlatformCall::Scope inner;
      REQUIRE(PlatformCall::Active());
    }
    REQUIRE(PlatformCall::Active());
  }
  REQUIRE_FALSE(PlatformCall::Active());

  // overlapping on two threads: the other's call begins and ends inside
  // this one's, which Override's single flag would have cleared
  std::atomic<bool> theirsSeen{ false };
  {
    PlatformCall::Scope mine;
    std::thread other([&theirsSeen] {
      PlatformCall::Scope theirs;
      theirsSeen = PlatformCall::Active();
    });
    other.join();
    REQUIRE(theirsSeen);
    REQUIRE(PlatformCall::Active());
  }
  REQUIRE_FALSE(PlatformCall::Active());
}

TEST_CASE("A dispatched call's level raises the mark only when marked",
          "[PlatformCall]")
{
  int frame = 0;
  const auto sp = reinterpret_cast<std::uintptr_t>(&frame);
  {
    // a script's call: the seam raises nothing
    const PlatformCall::CallLevel script(false, false, sp);
    REQUIRE_FALSE(PlatformCall::Active());
  }
  {
    // the platform's latent call on its Papyrus thread
    const PlatformCall::CallLevel platform(true, false, sp);
    REQUIRE(PlatformCall::Active());
    REQUIRE_FALSE(ActiveOnAnotherThread());
  }
  REQUIRE_FALSE(PlatformCall::Active());
}
