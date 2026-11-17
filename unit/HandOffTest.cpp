#include "../skyrim-platform/src/platform_se/skyrim_platform/HandOff.h"
#include <atomic>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

// thuum tracker #129 (playtest 54's COC freeze): the JavaScript thread waited
// on the worker's Debug.CenterOnCell, the worker's cell load waited on the
// background process thread, and that thread's PlayAnimation hook waited on
// the JavaScript thread. HandOff.h's desk breaks the cycle the way the
// worker already serves the JavaScript thread while it waits on the update:
// the JavaScript thread, while it waits on an engine thread, serves every
// ask queued meanwhile. These cases need no game: the cycle's three threads
// as std::threads, the desk's turn order, and the mark the platform's native
// refusal reads. The header is std only, so the server's T0 build reaches it
// by path, as PlatformCallTest.cpp reaches PlatformCall.h
namespace {
// the scenario's threads are detached and own their state, so a regression
// fails the case by its timeout instead of hanging the run
constexpr auto kTimeout = std::chrono::seconds(10);

struct Cycle
{
  HandOff::Desk desk;
  // the worker's flag, read and written under the desk's lock
  bool workerDone = false;
  std::atomic<bool> loaderAnswered{ false };
  std::atomic<bool> servedOnJs{ false };
  std::promise<void> jsReturned;
};
}

TEST_CASE("A JavaScript thread waiting on the engine serves the ask an engine "
          "thread waits on (#129's cycle)",
          "[HandOff]")
{
  auto cycle = std::make_shared<Cycle>();
  auto jsReturned = cycle->jsReturned.get_future();

  // the JavaScript thread: callNative's wait on the worker
  // (SkyrimPlatform::PushToWorkerAndWait)
  std::thread js([cycle] {
    const auto jsId = std::this_thread::get_id();
    std::thread loader([cycle, jsId] {
      // the background process thread's hook: its ask, then its wait
      auto ticket = cycle->desk.Ask(
        [cycle, jsId] {
          cycle->servedOnJs = std::this_thread::get_id() == jsId;
        },
        "sendAnimationEvent enter", 9528);
      cycle->desk.Wait(ticket);
      cycle->loaderAnswered = true;
    });
    std::thread worker([cycle] {
      // the cell load polls until the loader's task is through, then the
      // native returns (RunInIOContext's Signal)
      while (!cycle->loaderAnswered) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      cycle->desk.Signal([&] { cycle->workerDone = true; });
    });
    loader.detach();
    worker.detach();
    cycle->desk.WaitServing([&] { return cycle->workerDone; },
                            [](const HandOff::Ticket&) {});
    cycle->jsReturned.set_value();
  });
  js.detach();

  REQUIRE(jsReturned.wait_for(kTimeout) == std::future_status::ready);
  REQUIRE(cycle->loaderAnswered.load());
  REQUIRE(cycle->servedOnJs.load());
}

TEST_CASE("An ask the waiting JavaScript thread served is never run again in "
          "the pool's turn",
          "[HandOff]")
{
  struct State
  {
    HandOff::Desk desk;
    std::atomic<int> runs{ 0 };
    std::vector<std::string> named;
    std::promise<void> waited;
  };
  auto state = std::make_shared<State>();
  auto waited = state->waited.get_future();
  auto ticket =
    state->desk.Ask([state] { ++state->runs; }, "sendAnimationEvent leave", 1);

  // the JavaScript thread's wait ends once the ask has run, as a worker
  // whose native needed it would
  std::thread js([state, ticket] {
    state->desk.WaitServing(
      [&] { return ticket->done; },
      [&](const HandOff::Ticket& t) { state->named.push_back(t.label); });
    state->waited.set_value();
  });
  js.detach();
  REQUIRE(waited.wait_for(kTimeout) == std::future_status::ready);
  REQUIRE(state->runs.load() == 1);
  REQUIRE(state->named ==
          std::vector<std::string>{ "sendAnimationEvent leave" });

  // the ask's own turn in the pool finds nothing left to run
  REQUIRE_FALSE(state->desk.ServeOne());
  REQUIRE(state->runs.load() == 1);
}

TEST_CASE("The pool's turns serve the asks oldest first", "[HandOff]")
{
  HandOff::Desk desk;
  std::vector<int> order;
  for (int i = 1; i <= 3; ++i) {
    desk.Ask([&order, i] { order.push_back(i); }, "ask", 0);
  }
  REQUIRE(desk.ServeOne());
  REQUIRE(desk.ServeOne());
  REQUIRE(desk.ServeOne());
  REQUIRE_FALSE(desk.ServeOne());
  REQUIRE(order == std::vector<int>{ 1, 2, 3 });
}

TEST_CASE("A task that throws still answers its asker and never escapes the "
          "desk",
          "[HandOff]")
{
  HandOff::Desk desk;
  auto ticket =
    desk.Ask([] { throw std::runtime_error("handler failed"); }, "ask", 0);
  REQUIRE_NOTHROW(desk.ServeOne());
  REQUIRE(ticket->done);
  // the asker's wait returns at once
  desk.Wait(ticket);
}

TEST_CASE("The serving mark stands inside a served task alone, on its thread",
          "[HandOff]")
{
  HandOff::Desk desk;
  bool inside = false;
  std::atomic<bool> otherThread{ true };
  desk.Ask(
    [&] {
      inside = HandOff::Serving();
      std::thread other([&otherThread] { otherThread = HandOff::Serving(); });
      other.join();
    },
    "ask", 0);
  REQUIRE_FALSE(HandOff::Serving());
  REQUIRE(desk.ServeOne());
  REQUIRE(inside);
  REQUIRE_FALSE(otherThread.load());
  REQUIRE_FALSE(HandOff::Serving());

  // the platform's other waited tasks (the tick, a console command) raise
  // the same mark (SkyrimPlatform::PushAndWait)
  {
    HandOff::ServingScope serving;
    REQUIRE(HandOff::Serving());
  }
  REQUIRE_FALSE(HandOff::Serving());
}
