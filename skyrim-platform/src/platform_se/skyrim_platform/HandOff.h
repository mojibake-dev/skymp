#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
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
// (ServeOne), and the JavaScript thread, while it waits on an engine thread
// (WaitServing), serves every ask queued meanwhile. So no engine thread
// waits on the JavaScript thread while the JavaScript thread waits on an
// engine thread: the waiting JavaScript thread runs the ask. The other half
// of the rule is the platform's (SkyrimPlatform.cpp): a task an engine
// thread waits on calls no native (Serving() below), so a served task never
// waits on the engine itself.
//
// Std only, so the server's T0 build reaches it by path
// (unit/HandOffTest.cpp), as PlatformCall.h is.
namespace HandOff {

// one engine thread's ask: the task the JavaScript thread runs for it, the
// ask's name and the asking thread's id for the platform's lines, and
// whether it ran (under the desk's lock)
struct Ticket
{
  std::function<void()> task;
  std::string label;
  std::uint32_t thread = 0;
  bool done = false;
};

// the depth of tasks an engine thread waits on, on the thread running them
// (the JavaScript thread): raised around a served ask (Desk::Run) and by the
// platform around its other waited tasks (SkyrimPlatform::PushAndWait)
inline thread_local int t_serving = 0;

inline bool Serving()
{
  return t_serving > 0;
}

struct ServingScope
{
  ServingScope() { ++t_serving; }
  ~ServingScope() { --t_serving; }
  ServingScope(const ServingScope&) = delete;
  ServingScope& operator=(const ServingScope&) = delete;
};

class Desk
{
public:
  // an engine thread's side: the ask, queued, and a JavaScript thread that
  // waits on the engine woken to serve it. The caller then gives the
  // JavaScript thread's own queue a turn (ServeOne) and waits (Wait)
  std::shared_ptr<Ticket> Ask(std::function<void()> task, std::string label,
                              std::uint32_t thread)
  {
    auto ticket = std::make_shared<Ticket>();
    ticket->task = std::move(task);
    ticket->label = std::move(label);
    ticket->thread = thread;
    {
      std::lock_guard<std::mutex> lock(m);
      queue.push_back(ticket);
    }
    cv.notify_all();
    return ticket;
  }

  // the asking thread, until its task has run
  void Wait(const std::shared_ptr<Ticket>& ticket)
  {
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return ticket->done; });
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

  // the JavaScript thread's wait on an engine thread: until finished() holds
  // (read under the desk's lock; the engine thread changes it through
  // Signal), every ask queued meanwhile is run here, oldest first, each
  // named to served() first. It returns only once finished() holds, since
  // the engine thread it waits on may still write to the caller's frame
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

  // the engine thread's side of a wait the JavaScript thread makes here:
  // the state finished() reads, changed under the desk's lock, and the
  // waiter woken
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
  // WaitServing whose engine thread still runs
  void Run(Ticket& ticket)
  {
    try {
      ServingScope serving;
      ticket.task();
    } catch (...) {
    }
    {
      std::lock_guard<std::mutex> lock(m);
      ticket.done = true;
    }
    cv.notify_all();
  }

  std::mutex m;
  std::condition_variable cv;
  std::deque<std::shared_ptr<Ticket>> queue;
};
}
