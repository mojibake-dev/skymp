#include "EventsApi.h"
#include "EventManager.h"
#include "ExceptionPrinter.h"
#include "Handler.h"
#include "Hook.h"
#include "HookPattern.h"
#include "IPC.h"
#include "InvalidArgumentException.h"
#include "JsUtils.h"
#include "NullPointerException.h"
#include "ScopedTask.h"
#include "SkyrimPlatform.h"
#include "ThreadPoolWrapper.h"
#include <sstream>
#include <string>

class EventsGlobalState
{
public:
  EventsGlobalState()
  {
    sendAnimationEvent.reset(
      new Hook("sendAnimationEvent", "animEventName", "animationSucceeded"));
    sendPapyrusEvent.reset(
      new Hook("sendPapyrusEvent", "papyrusEventName", std::nullopt));
  }

  std::shared_ptr<Hook> sendAnimationEvent;
  std::shared_ptr<Hook> sendPapyrusEvent;
} g;

namespace {
// thuum #134 (playtest 54: "on('update'): Expected 'objectTypeInfo' to not
// to be nullptr", 9,603 times in c1.log): a handler's error line names the
// handler, so an error that repeats every frame says whose code it is.
// skymp5-client's services all register through one shared controller whose
// on is sp.on itself (spApiInteractor.ts), mostly as the same
// `() => this.onUpdate()`, so a handler's name or source tells them apart
// poorly; the JavaScript error's own stack does (V8 records it where the
// Error is made, inside the native for a native's throw; the client bundle
// is webpack's development mode, unminified, so its frames carry the class
// and method). All of it is read after a handler threw: nothing is spent
// when nothing throws, and nothing is suppressed or rate-limited (the count
// of repeats is itself the read)

// keeps maxLen characters of text, each run of whitespace read as one space
std::string CompactText(const std::string& text, size_t maxLen)
{
  std::string out;
  bool space = false;
  for (const char c : text) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      space = !out.empty();
      continue;
    }
    if (out.size() >= maxLen) {
      out += "...";
      break;
    }
    if (space) {
      out += ' ';
      space = false;
    }
    out += c;
  }
  return out;
}

// a V8 frame "X.y (C:\\...\\skymp5-client.js:12:3)" reads
// "X.y (skymp5-client.js:12:3)", an anonymous one "skymp5-client.js:12:3"
std::string ShortStackFrame(const std::string& frame)
{
  std::string name;
  std::string location = frame;
  const size_t open = frame.find(" (");
  if (open != std::string::npos && frame.back() == ')') {
    name = frame.substr(0, open);
    location = frame.substr(open + 2, frame.size() - open - 3);
  }
  const size_t slash = location.find_last_of("\\/");
  if (slash != std::string::npos) {
    location = location.substr(slash + 1);
  }
  return name.empty() ? location : name + " (" + location + ")";
}

// the error's first maxFrames frames, innermost first, each joined to its
// caller by " < "
std::string JsStackFrames(Napi::Error& e, size_t maxFrames)
{
  try {
    const Napi::Value stack = e.Get("stack");
    if (!stack.IsString()) {
      return "none";
    }
    std::istringstream lines(stack.As<Napi::String>().Utf8Value());
    std::string line;
    std::string out;
    size_t n = 0;
    while (n < maxFrames && std::getline(lines, line)) {
      const size_t at = line.find_first_not_of(" \t");
      if (at == std::string::npos || line.compare(at, 3, "at ") != 0) {
        continue;
      }
      std::string frame = line.substr(at + 3);
      if (!frame.empty() && frame.back() == '\r') {
        frame.pop_back();
      }
      if (frame.empty()) {
        continue;
      }
      out += (n++ ? " < " : "") + ShortStackFrame(frame);
    }
    return out.empty() ? "none" : out;
  } catch (const std::exception& readError) {
    return std::string("unread: ") + readError.what();
  }
}

// the handler's own name, else the start of its source
std::string HandlerLabel(const CallbackObject& cb)
{
  try {
    const Napi::Function fn = cb.callback->Value().As<Napi::Function>();
    const Napi::Value name = fn.Get("name");
    if (name.IsString()) {
      std::string s = name.As<Napi::String>().Utf8Value();
      if (!s.empty()) {
        return s;
      }
    }
    return CompactText(fn.ToString().Utf8Value(), 48);
  } catch (const std::exception& readError) {
    return std::string("unread: ") + readError.what();
  }
}

// handler and frames come from the caller: only the Napi::Error branch reads
// them from V8 (HandlerLabel, JsStackFrames); this function makes no V8 call
void LogHandlerError(const CallbackObject& cb, const char* eventName,
                     const std::exception& e, const std::string& handler,
                     const std::string& frames)
{
  const char* method = cb.runOnce ? "once" : "on";
  std::string what = e.what();
  logger::error("{}('{}'): {} [handler {}; stack {}]", method, eventName, what,
                handler, frames);

  // We still write to the game console as we were doing before spdlog
  // integration. When I write something wrong when coding skymp5-client I
  // expect to see errors in game console
  ExceptionPrinter::Print(e);
}
}

void EventsApi::SendEvent(const char* eventName,
                          const std::vector<Napi::Value>& arguments)
{
  auto manager = EventManager::GetSingleton();

  const CallbackObjMap& cbObjMap = manager->GetCallbackObjMap(eventName);

  // TODO: use container with pre-allocated space
  // Global vars aren't helpful, because SendEvent can be made recursive in the
  // future.
  std::vector<uintptr_t> callbacksToUnsubscribe;
  std::vector<CallbackObject> callbacksToCall;

  // 1. Collect all callbacks and remember "runOnce" callbacks
  for (const auto& [uid, cb] : cbObjMap) {
    callbacksToCall.push_back(cb);
    if (cb.runOnce) {
      callbacksToUnsubscribe.push_back(uid);
    }
  }

  // 2. Make sure that "runOnce" callbacks will never be called again
  for (uintptr_t uid : callbacksToUnsubscribe) {
    manager->Unsubscribe(uid, eventName);
  }

  // 3. Finally, call the callbacks
  for (CallbackObject& cb : callbacksToCall) {
    try {
      Napi::Function callback = cb.callback->Value().As<Napi::Function>();
      callback.Call(arguments);
    } catch (Napi::Error& e) {
      // a JavaScript throw (a native's C++ throw reaches here as one,
      // NapiHelper::WrapCppExceptions): its stack names the code
      LogHandlerError(cb, eventName, e, HandlerLabel(cb), JsStackFrames(e, 6));
    } catch (const std::exception& e) {
      // a C++ exception other than Napi::Error that reached here may have
      // unwound through V8's frames and left the isolate's handler chain on a
      // dead frame (the 2026-10-22 landing hang: a later JavaScript throw's
      // unwind walked it forever), so this branch makes no V8 call at all:
      // no handler name or source, no stack
      LogHandlerError(cb, eventName, e, "unread (a C++ exception crossed V8)",
                      "none");
    }
  }

  // You may want to optimize it. Feel free to. But remember that changing
  // order of those three steps may and WILL break user code like:
  //
  // sp.on("tick", () => {
  //   sp.once("tick", () => {
  //     sp.once("tick", () => {
  // // ...
}

void EventsApi::Clear()
{
  g = {};

  EventManager::GetSingleton()->ClearCallbacks();
}

void EventsApi::SendAnimationEventEnter(uint32_t selfId,
                                        std::string& animEventName) noexcept
{
  g.sendAnimationEvent->Enter(selfId, animEventName);
}

void EventsApi::SendAnimationEventLeave(bool animationSucceeded) noexcept
{
  g.sendAnimationEvent->Leave(animationSucceeded);
}

void EventsApi::SkipAnimationEventEnter() noexcept
{
  g.sendAnimationEvent->SkipEnter();
}

void EventsApi::SendPapyrusEventEnter(uint32_t selfId,
                                      std::string& papyrusEventName) noexcept
{
  g.sendPapyrusEvent->Enter(selfId, papyrusEventName);
}

void EventsApi::SendPapyrusEventLeave() noexcept
{
  g.sendPapyrusEvent->Leave(true);
}

namespace {
Napi::Value CreateHookApi(Napi::Env env, std::shared_ptr<Hook> hookInfo)
{
  auto hook = Napi::Object::New(env);
  // thuum docs/verbs/npc-puppet.md: both callbacks wrapped, so a failure
  // (a handler added inside a handler) reaches JavaScript as its error and
  // never as a C++ exception through V8's frames (the landing hang)
  hook.Set("add",
           Napi::Function::New(
             env,
             NapiHelper::WrapCppExceptions(
               [hookInfo](const Napi::CallbackInfo& info) {
                 auto handlerObj =
                   NapiHelper::ExtractObject(info[0], "handlerObj");

                 std::optional<double> minSelfId;
                 if (info[1].IsNumber()) {
                   minSelfId = NapiHelper::ExtractDouble(info[1], "minSelfId");
                 }

                 std::optional<double> maxSelfId;
                 if (info[2].IsNumber()) {
                   maxSelfId = NapiHelper::ExtractDouble(info[2], "maxSelfId");
                 }

                 std::optional<HookPattern> pattern;
                 if (info[3].IsString()) {
                   auto s = NapiHelper::ExtractString(info[3], "pattern");
                   pattern = HookPattern::Parse(s);
                 }

                 auto handler = std::make_shared<Handler>(
                   handlerObj, minSelfId, maxSelfId, pattern);
                 uint32_t id = hookInfo->AddHandler(handler);

                 return Napi::Value(Napi::Number::New(info.Env(), id));
               })));

  hook.Set("remove",
           Napi::Function::New(env,
                               NapiHelper::WrapCppExceptions(
                                 [hookInfo](const Napi::CallbackInfo& info) {
                                   uint32_t toRemove =
                                     NapiHelper::ExtractUInt32(info[0],
                                                               "toRemove");
                                   hookInfo->RemoveHandler(toRemove);
                                   return Napi::Value(info.Env().Undefined());
                                 })));
  return hook;
}
}

Napi::Value EventsApi::GetHooks(Napi::Env env)
{
  auto res = Napi::Object::New(env);
  for (auto& hook : { g.sendAnimationEvent, g.sendPapyrusEvent }) {
    res.Set(hook->GetName(), CreateHookApi(env, hook));
  }
  return res;
}

namespace {
Napi::Value Subscribe(const Napi::CallbackInfo& info, bool runOnce = false)
{
  auto eventName = NapiHelper::ExtractString(info[0], "eventName");
  auto callback = NapiHelper::ExtractFunction(info[1], "callback");

  std::shared_ptr<Napi::Reference<Napi::Function>> callbackRef;
  callbackRef.reset(
    new Napi::Reference<Napi::Function>(Napi::Persistent(callback)));

  auto handle =
    EventManager::GetSingleton()->Subscribe(eventName, callbackRef, runOnce);

  auto obj = Napi::Object::New(info.Env());
  AddObjProperty(&obj, "uid", handle->uid);
  AddObjProperty(&obj, "eventName", handle->eventName);

  return obj;
}
}

Napi::Value EventsApi::On(const Napi::CallbackInfo& info)
{
  return Subscribe(info);
}

Napi::Value EventsApi::Once(const Napi::CallbackInfo& info)
{
  return Subscribe(info, true);
}

Napi::Value EventsApi::Unsubscribe(const Napi::CallbackInfo& info)
{
  auto obj = NapiHelper::ExtractObject(info[0], "obj");
  auto eventName =
    NapiHelper::ExtractString(obj.Get("eventName"), "obj.eventName");
  auto uid = NapiHelper::ExtractUInt32(obj.Get("uid"), "obj.uid");
  EventManager::GetSingleton()->Unsubscribe(uid, eventName);
  return info.Env().Undefined();
}

Napi::Value EventsApi::SendIpcMessage(const Napi::CallbackInfo& info)
{
  Napi::Env env = info.Env();

  std::string targetSystemName =
    NapiHelper::ExtractString(info[0], "targetSystemName");
  Napi::ArrayBuffer messageBuffer =
    NapiHelper::ExtractArrayBuffer(info[1], "message");

  void* message = messageBuffer.Data();
  size_t messageLength = messageBuffer.ByteLength();

  if (!message) {
    throw std::runtime_error(
      "sendIpcMessage expects a valid ArrayBuffer instance");
  }

  if (messageLength == 0) {
    throw std::runtime_error(
      "sendIpcMessage expects an ArrayBuffer of length > 0");
  }

  IPC::Call(targetSystemName, reinterpret_cast<uint8_t*>(message),
            messageLength);

  return env.Undefined();
}
