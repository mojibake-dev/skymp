#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>

// thuum tracker #129 (playtest 54's COC freeze; the dump read of
// 2026-11-17, tracker note 3189, HYPOTHESIS until a second dump or a Frida
// read of PlayAnimation's thread agrees): a wait cycle through the platform.
// The JavaScript thread called Debug.CenterOnCell and waited for the worker
// (SkyrimPlatform::PushToWorkerAndWait); the worker, the Papyrus thread
// inside the VM's job, loaded the cell and polled the IO manager until
// nothing was pending; the background process thread, loading a
// container's 3D, played its "Close" sequence through
// TESObjectREFR::PlayAnimation, whose hook (FridaHooks.cpp,
// OnPlayAnimationEnter) handed the event to the JavaScript thread and
// waited (Hook.cpp, the pool's PushAndWait). Each waited on the next.
//
// The platform already breaks the mirror of this cycle: the worker waits on
// the JavaScript thread for the whole update (main.cpp's OnUpdate runs the
// io_context until the update stops it) and serves the JavaScript thread's
// native calls while it waits. This desk is the same shape the other way:
// an engine thread that must wait on the JavaScript thread asks here and
// waits; the JavaScript thread's own queue serves the ask in its turn
// (ServeOne), and the JavaScript thread, while it waits on the worker
// (WaitServing), serves every ask queued meanwhile. So no engine thread
// waits on the JavaScript thread while the JavaScript thread waits on the
// worker: the waiting JavaScript thread runs the ask. The other half of the
// rule is the platform's (SkyrimPlatform.cpp): a task an engine thread
// waits on calls no native (Serving() below), so a served task never waits
// on the engine itself.
//
// Two waits of the JavaScript thread on the engine stay outside the desk.
// (a) An engine call made on the JavaScript thread itself: callNative's
// direct addItem and removeItem (CallNative.cpp's AddObjectToContainer and
// RemoveItem) and the platform's APIs that call the engine there. No ask is
// served inside one, so one that waited on a thread whose hook asks the
// JavaScript thread would close the cycle as before. HYPOTHESIS: none of
// them waits on another engine thread (no instance known; it predates
// #129). (b) The tick's and a console command's waits on the JavaScript
// thread keep the pool's order (SkyrimPlatform::PushAndWait, its own
// HYPOTHESIS).
//
// Std only, so the server's T0 build reaches it by path
// (unit/HandOffTest.cpp), as PlatformCall.h is.
namespace HandOff {

// one engine thread's ask: the task the JavaScript thread runs for it, the
// ask's name and the asking thread's id for the platform's lines, whether it
// ran (under the desk's lock), and the asker's own wake, so an ask's end
// wakes its asker alone. The label is a view: it is read only while its
// asker waits in Wait, inside the frame that holds the string
struct Ticket
{
  std::function<void()> task;
  std::string_view label;
  std::uint32_t thread = 0;
  bool done = false;
  std::condition_variable cv;
};

// the depth of tasks an engine thread waits on, on the thread running them
// (the JavaScript thread), and what the innermost one is (for the platform's
// refusal line): raised around a served ask (Desk::Run) and by the platform
// around its other waited tasks (SkyrimPlatform::PushAndWait)
inline thread_local int t_serving = 0;
inline thread_local const char* t_servingWhat = nullptr;

inline bool Serving()
{
  return t_serving > 0;
}

// what the innermost waited task is ("a hook's handler", "the tick or a
// console command"), or null outside one
inline const char* ServingWhat()
{
  return t_servingWhat;
}

struct ServingScope
{
  explicit ServingScope(const char* what)
    : outerWhat(t_servingWhat)
  {
    ++t_serving;
    t_servingWhat = what;
  }
  ~ServingScope()
  {
    --t_serving;
    t_servingWhat = outerWhat;
  }
  ServingScope(const ServingScope&) = delete;
  ServingScope& operator=(const ServingScope&) = delete;

private:
  const char* const outerWhat;
};

class Desk
{
public:
  // an engine thread's side: the ask, queued, and a JavaScript thread that
  // waits on the worker woken to serve it. The caller then gives the
  // JavaScript thread's own queue a turn (ServeOne) and waits (Wait)
  std::shared_ptr<Ticket> Ask(std::function<void()> task,
                              std::string_view label, std::uint32_t thread)
  {
    auto ticket = std::make_shared<Ticket>();
    ticket->task = std::move(task);
    ticket->label = label;
    ticket->thread = thread;
    {
      std::lock_guard<std::mutex> lock(m);
      queue.push_back(ticket);
    }
    // the desk's own wake is the JavaScript thread's (WaitServing, WaitFor);
    // no asker waits on it
    cv.notify_all();
    return ticket;
  }

  // the asking thread, until its task has run, on its ticket's own wake
  void Wait(const std::shared_ptr<Ticket>& ticket)
  {
    std::unique_lock<std::mutex> lock(m);
    ticket->cv.wait(lock, [&] { return ticket->done; });
  }

  // the JavaScript thread's own turn for an ask (one turn per ask): the
  // oldest waiting ask, run, or none when a waiting JavaScript thread ran it
  // already
  bool ServeOne()
  {
    std::shared_ptr<Ticket> ticket;
    {
      std::lock_guard<std::mutex> lock(m);
      if (queue.empty()) {
        return false;
      }
      ticket = std::move(queue.front());
      queue.pop_front();
    }
    Run(*ticket);
    return true;
  }

  // the JavaScript thread's wait on the worker: until finished() holds (read
  // under the desk's lock; the worker changes it through Signal), every ask
  // queued meanwhile is run here, oldest first, each named to served()
  // first. It returns only once finished() holds, since the worker it waits
  // on may still write to the caller's frame
  template <class Finished, class Served>
  void WaitServing(Finished finished, Served served)
  {
    std::unique_lock<std::mutex> lock(m);
    for (;;) {
      cv.wait(lock, [&] { return finished() || !queue.empty(); });
      if (finished()) {
        return;
      }
      std::shared_ptr<Ticket> ticket = std::move(queue.front());
      queue.pop_front();
      lock.unlock();
      try {
        served(*ticket);
      } catch (...) {
        // a line that failed never strands the ask
      }
      Run(*ticket);
      lock.lock();
    }
  }

  // a wait on the worker that serves nothing (the platform's caller that is
  // not the JavaScript thread, whose asks it must not run): until
  // finished() holds, read and changed as WaitServing's is
  template <class Finished>
  void WaitFor(Finished finished)
  {
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return finished(); });
  }

  // the worker's side of a wait made here: the state finished() reads,
  // changed under the desk's lock, and the waiter woken
  template <class Change>
  void Signal(Change change)
  {
    {
      std::lock_guard<std::mutex> lock(m);
      change();
    }
    cv.notify_all();
  }

private:
  // a task never throws out of here: the platform's task logs its own
  // failure, and a throw must neither leave its asker waiting nor unwind a
  // WaitServing whose worker still runs. The caller holds the ticket's
  // shared_ptr, so the wake after the lock is released reaches a live ticket
  void Run(Ticket& ticket)
  {
    try {
      ServingScope serving("a hook's handler");
      ticket.task();
    } catch (...) {
    }
    {
      std::lock_guard<std::mutex> lock(m);
      ticket.done = true;
    }
    ticket.cv.notify_one();
  }

  std::mutex m;
  std::condition_variable cv;
  std::deque<std::shared_ptr<Ticket>> queue;
};
}
