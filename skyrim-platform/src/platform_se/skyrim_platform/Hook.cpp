#include "Hook.h"
#include "Handler.h"
#include "SkyrimPlatform.h"

#include <iterator>
#include <utility>
#include <vector>

namespace {
// thuum tracker #129: an enter of a hook whose leave crosses too (the
// animation hook) leaves one note on its thread, whether it crossed; the
// leave takes the newest note of its hook and crosses only after an enter
// that did. So the JavaScript side's per-thread state (inProgressThreads,
// each handler's context) opens and closes in pairs whatever an enter
// skipped: the native match's refusal, a hand-off declined, a caller's
// early return (FridaHooks.cpp's enters return before Enter on a null
// reference or an empty name, their leaves still call Leave)
thread_local std::vector<std::pair<const Hook*, bool>> t_enters;

void NoteEnter(const Hook* hook, bool crossed)
{
  // an enter whose leave never came (a throw through the hooked call)
  // never grows the notes without bound
  if (t_enters.size() >= 64) {
    t_enters.clear();
  }
  t_enters.emplace_back(hook, crossed);
}

bool TakeEnter(const Hook* hook)
{
  for (auto it = t_enters.rbegin(); it != t_enters.rend(); ++it) {
    if (it->first == hook) {
      const bool crossed = it->second;
      t_enters.erase(std::next(it).base());
      return crossed;
    }
  }
  return false;
}
}

Hook::Hook(std::string hookName_, std::string eventNameVariableName_,
           std::optional<std::string> succeededVariableName_)
  : hookName(hookName_)
  , eventNameVariableName(eventNameVariableName_)
  , succeededVariableName(succeededVariableName_)
{
}

// thuum docs/verbs/npc-puppet.md (the landing hang of 2026-10-21): the test
// that matters is re-entrancy on the JavaScript thread, a handler adding or
// removing a hook while HandleEnter or HandleLeave walks the map. The old
// test read addRemoveBlocker, which a game or job thread raises before it
// hands its Enter to the JavaScript thread, so an add made by ordinary
// JavaScript while any animation event was in flight anywhere threw a C++
// exception out of an unwrapped Napi callback, through V8's frames: the
// isolate's handler chain was left pointing at a dead frame and the next
// exception unwind spun forever (two dumps, the JS worker in
// StackFrameIterator::Advance). The npc-puppet build hosts a cell's
// game-file actors and adds one animation hook per actor at the grant while
// those actors animate from the job threads, so the collision was certain.
uint32_t Hook::AddHandler(const std::shared_ptr<Handler>& handler)
{
  if (handlerDepth > 0) {
    throw std::runtime_error("Trying to add hook inside hook context");
  }
  std::unique_lock<std::shared_mutex> lock(handlersMutex);
  handlers.emplace(hCounter, handler);
  return hCounter++;
}

void Hook::RemoveHandler(const uint32_t& id)
{
  if (handlerDepth > 0) {
    throw std::runtime_error("Trying to remove hook inside hook context");
  }
  std::unique_lock<std::shared_mutex> lock(handlersMutex);
  handlers.erase(id);
}

std::string Hook::GetName() const
{
  return hookName;
}

bool Hook::AnyHandlerMatches(uint32_t selfId,
                             const std::string& eventName) const
{
  std::shared_lock<std::shared_mutex> lock(handlersMutex);
  for (const auto& hp : handlers) {
    if (hp.second->Matches(selfId, eventName)) {
      return true;
    }
  }
  return false;
}

void Hook::Enter(uint32_t selfId, std::string& eventName)
{
  const bool papyrus = hookName == "sendPapyrusEvent";

  // thuum tracker #129 (hard rule 9; events.md's "No JS is triggered until
  // selfId in range found and event name matches"): the native match, made
  // here on the hooked thread before any crossing, for both hooks. The
  // Papyrus hook had it; the animation hook handed every event to the
  // JavaScript thread and waited there, a match or not, and HandleEnter
  // then called nobody. The test is HandleEnter's own (Handler::Matches:
  // the id range, then the pattern by bytes, case and all), on the name as
  // the engine sent it; a later handler sees a name an earlier one changed,
  // and only a handler that matched can change it, so no match here is no
  // match there. A handler added between this test and the hand-off sees
  // the event's next sending, as one added a moment later would
  if (!AnyHandlerMatches(selfId, eventName)) {
    if (!sparedSaid.exchange(true)) {
      logger::info("[hook] '{}': an event no handler's filter (minSelfId, "
                   "maxSelfId, eventPattern) takes stays in native and never "
                   "crosses to the JavaScript thread (rule 9, thuum tracker "
                   "#129); said once",
                   hookName);
    }
    if (!papyrus) {
      NoteEnter(this, false);
    }
    return;
  }

  DWORD owningThread = GetCurrentThreadId();

  if (papyrus) {
    return SkyrimPlatform::GetSingleton()->AddUpdateTask(
      [this, owningThread, selfId, eventName](Napi::Env env) {
        std::string s = eventName;
        HandleEnter(owningThread, selfId, s, env);
      });
  }

  addRemoveBlocker++;
  auto f = [&](Napi::Env env) {
    try {
      if (inProgressThreads.count(owningThread))
        throw std::runtime_error("'" + hookName + "' is already processing");
      inProgressThreads.insert(owningThread);
      HandleEnter(owningThread, selfId, eventName, env);
    } catch (std::exception& e) {
      auto err = std::string(e.what()) + " (while performing enter on '" +
        hookName + "')";
      SkyrimPlatform::GetSingleton()->AddUpdateTask(
        [err](Napi::Env) { throw std::runtime_error(err); });
    }
  };
  // thuum tracker #129: through the platform's hand-off, which the
  // JavaScript thread serves even while it waits on the worker
  // (SkyrimPlatform::HandOffAndWait, HandOff.h); declined only when asked on
  // the JavaScript thread itself, which would wait on itself
  NoteEnter(
    this,
    SkyrimPlatform::GetSingleton()->HandOffAndWait(hookName + " enter", f));
  addRemoveBlocker--;
}

void Hook::Leave(bool succeeded)
{
  if (hookName == "sendPapyrusEvent") {
    return;
  }

  // thuum tracker #129: only the leave of an enter that crossed (the notes
  // above)
  if (!TakeEnter(this)) {
    return;
  }

  addRemoveBlocker++;
  DWORD owningThread = GetCurrentThreadId();

  auto f = [&](Napi::Env env) {
    try {
      if (!inProgressThreads.count(owningThread))
        throw std::runtime_error("'" + hookName + "' is not processing");
      inProgressThreads.erase(owningThread);
      HandleLeave(owningThread, succeeded, env);

    } catch (std::exception& e) {
      std::string what = e.what();
      SkyrimPlatform::GetSingleton()->AddUpdateTask([what](Napi::Env) {
        throw std::runtime_error(what + " (in SendAnimationEventLeave)");
      });
    }
  };
  // the enter crossed from this thread, so this is no JavaScript thread and
  // the hand-off is never declined; its answer is not needed
  SkyrimPlatform::GetSingleton()->HandOffAndWait(hookName + " leave", f);
  addRemoveBlocker--;
}

void Hook::HandleEnter(DWORD owningThread, uint32_t selfId,
                       std::string& eventName, const Napi::Env& env)
{
  // JavaScript thread only (the handlers run here); the depth is the
  // re-entrancy guard AddHandler and RemoveHandler read
  struct DepthScope
  {
    int& d;
    explicit DepthScope(int& d_)
      : d(d_)
    {
      ++d;
    }
    ~DepthScope() { --d; }
  } depthScope(handlerDepth);
  for (auto& hp : handlers) {
    Handler* h = hp.second.get();
    auto& perThread = h->perThread[owningThread];

    perThread.matchesCondition = h->Matches(selfId, eventName);
    if (!perThread.matchesCondition) {
      continue;
    }

    PrepareContext(perThread, env);
    ClearContextStorage(perThread, env);

    perThread.context->Value().As<Napi::Object>().Set(
      "selfId", Napi::Number::New(env, static_cast<double>(selfId)));
    perThread.context->Value().As<Napi::Object>().Set(
      eventNameVariableName, Napi::String::New(env, eventName));
    h->enter.Value().As<Napi::Function>().Call(env.Undefined(),
                                               { perThread.context->Value() });

    // Retrieve the updated eventName from the context
    Napi::Value updatedEventName =
      perThread.context->Value().As<Napi::Object>().Get(eventNameVariableName);
    eventName = updatedEventName.As<Napi::String>().Utf8Value();
  }
}

void Hook::PrepareContext(HandlerInfoPerThread& h, const Napi::Env& env)
{
  if (!h.context) {
    auto object = Napi::Object::New(env);
    h.context.reset(new Napi::Reference<Napi::Object>(
      Napi::Persistent<Napi::Object>(object)));
  }

  Napi::Value standardMap = env.Global().Get("Map");

  if (!h.storage) {
    Napi::Object mapInstance = standardMap.As<Napi::Function>().New({});
    h.storage.reset(
      new Napi::Reference<Napi::Object>(Napi::Persistent(mapInstance)));
    h.context->Value().As<Napi::Object>().Set("storage", h.storage->Value());
  }
}

void Hook::ClearContextStorage(HandlerInfoPerThread& h, Napi::Env env)
{
  if (!h.storage) {
    return;
  }

  Napi::Object global = env.Global();
  Napi::Object standardMap = global.Get("Map").As<Napi::Object>();
  Napi::Function clear = standardMap.Get("prototype")
                           .As<Napi::Object>()
                           .Get("clear")
                           .As<Napi::Function>();

  clear.Call(h.storage->Value(), {});
}

void Hook::HandleLeave(DWORD owningThread, bool succeeded, Napi::Env env)
{
  struct DepthScope
  {
    int& d;
    explicit DepthScope(int& d_)
      : d(d_)
    {
      ++d;
    }
    ~DepthScope() { --d; }
  } depthScope(handlerDepth);
  for (auto& hp : handlers) {
    Handler* h = hp.second.get();
    auto& perThread = h->perThread.at(owningThread);

    if (!perThread.matchesCondition) {
      continue;
    }

    PrepareContext(perThread, env);

    if (succeededVariableName.has_value()) {
      perThread.context->Value().As<Napi::Object>().Set(
        succeededVariableName.value(), Napi::Boolean::New(env, succeeded));
    }

    h->leave.Value().As<Napi::Function>().Call(env.Undefined(),
                                               { perThread.context->Value() });
    h->perThread.erase(owningThread);
  }
}
