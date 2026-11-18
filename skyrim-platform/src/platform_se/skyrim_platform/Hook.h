#pragma once
#include <atomic>
#include <shared_mutex>

class Handler;
struct HandlerInfoPerThread;

class Hook
{
public:
  Hook(std::string hookName_, std::string eventNameVariableName_,
       std::optional<std::string> succeededVariableName_);

  // javascript thread only
  uint32_t AddHandler(const std::shared_ptr<Handler>& handler);
  void RemoveHandler(const uint32_t& id);

  // Thread-safe, but it isn't too useful actually
  std::string GetName() const;

  // Hooks are set on game functions that are being called from multiple
  // threads. So Enter/Leave methods are thread-safe, but all private methods
  // are for Chakra thread only

  void Enter(uint32_t selfId, std::string& eventName);

  // thuum tracker #129 (HookPairing.h): an enter its caller returned from
  // before Enter (FridaHooks.cpp's null reference or empty name), whose
  // leave still calls Leave: its note, not crossed, so that leave takes its
  // own and never a nested outer enter's
  void SkipEnter();

  void Leave(bool succeeded);

private:
  // thuum tracker #129 (hard rule 9): whether any handler's filter
  // (minSelfId, maxSelfId, eventPattern) takes the event, by the very test
  // HandleEnter applies (Handler::Matches), on the hooked thread before any
  // crossing; any thread, under the shared lock
  bool AnyHandlerMatches(uint32_t selfId, const std::string& eventName) const;

  void HandleEnter(DWORD owningThread, uint32_t selfId, std::string& eventName,
                   const Napi::Env& env);

  void PrepareContext(HandlerInfoPerThread& h, const Napi::Env& env);

  void ClearContextStorage(HandlerInfoPerThread& h, Napi::Env env);

  void HandleLeave(DWORD owningThread, bool succeeded, Napi::Env env);

  const std::string hookName;
  // the hand-off's names for this hook's enter and leave
  // (SkyrimPlatform::HandOffAndWait), built once, not per event
  const std::string enterLabel;
  const std::string leaveLabel;
  const std::string eventNameVariableName;
  const std::optional<std::string> succeededVariableName;
  std::set<DWORD> inProgressThreads;
  std::map<uint32_t, std::shared_ptr<Handler>> handlers;
  // the handlers map's shape against the native match on the engine's
  // threads (AnyHandlerMatches, shared); AddHandler and RemoveHandler change
  // it under the unique lock on the JavaScript thread, whose own walks
  // (HandleEnter, HandleLeave) need none, since nothing else changes it
  mutable std::shared_mutex handlersMutex;
  uint32_t hCounter = 0;
  // raised by the hooked thread around its hand-off to the JavaScript thread
  // (kept for the Papyrus hook's early scan); not a guard for add or remove
  std::atomic<int> addRemoveBlocker = 0;
  // the JavaScript thread's depth inside HandleEnter or HandleLeave: the
  // guard AddHandler and RemoveHandler read (thuum docs/verbs/npc-puppet.md)
  int handlerDepth = 0;
  // the first event the native match kept from crossing, said once
  std::atomic<bool> sparedSaid{ false };
};
