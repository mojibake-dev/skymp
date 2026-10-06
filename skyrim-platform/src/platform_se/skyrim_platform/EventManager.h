#pragma once
#include "EventHandler.h"
#include <map>

struct EventHandle
{
  EventHandle(uintptr_t uid_, const std::string& eventName_)
    : uid(uid_)
    , eventName(eventName_)
  {
  }

  const uintptr_t uid;
  const std::string eventName;
};

class SinkObject
{
public:
  SinkObject(const ::Sink* sink_,
             const std::vector<std::string>& linkedEvents_)
    : sink(sink_)
    , linkedEvents(linkedEvents_)
  {
  }

  const ::Sink* GetSink() const { return sink; }

  const std::vector<std::string>& GetLinkedEvents() const
  {
    return linkedEvents;
  }

private:
  const ::Sink* sink;
  std::vector<std::string> linkedEvents;
};

struct CallbackObject
{
  CallbackObject(
    const std::shared_ptr<Napi::Reference<Napi::Function>>& callback_,
    bool runOnce_)
    : callback(callback_)
    , runOnce(runOnce_)
  {
  }

  std::shared_ptr<Napi::Reference<Napi::Function>> callback;
  bool runOnce = false;
};

// Ordered by uid, which Subscribe hands out in increasing order, so an event
// calls its callbacks in the order they subscribed, as an EventEmitter does.
// A hash map called them in hash order: two once('update') callbacks queued
// in one frame could run newest first (thuum, run
// 20261006-183431-x-give-trace: skymp5-client applied the player's newer
// inventory, then the older one, and took back the item just given).
using CallbackObjMap = std::map<uintptr_t, CallbackObject>;

struct EventState
{
  explicit EventState(const std::optional<SinkObject>& sinkObj_)
    : sinkObj(sinkObj_)
  {
  }

  std::optional<SinkObject> sinkObj;
  CallbackObjMap callbacks;
};

using EventMap = robin_hood::unordered_map<std::string, EventState>;

class EventManager
{
public:
  [[nodiscard]] static EventManager* GetSingleton()
  {
    static EventManager singleton;
    return &singleton;
  }

  static void Init();
  static void InitCustom();

  std::unique_ptr<EventHandle> Subscribe(
    const std::string& eventName,
    const std::shared_ptr<Napi::Reference<Napi::Function>>& callback,
    bool runOnce);

  void Unsubscribe(uintptr_t uid, const std::string& eventName);

  void ClearCallbacks();

  const CallbackObjMap& GetCallbackObjMap(const char* eventName);

  void EmplaceEvent(const std::string& name,
                    const EventState& state = EventState{ std::nullopt });

  EventMap& GetEventMap();

private:
  void DeactivateEventSinkIfNeeded(EventState& state,
                                   const std::string& eventName);

  EventManager() { events.reserve(100); }
  EventManager(const EventManager&) = delete;
  EventManager(EventManager&&) = delete;

  ~EventManager() = default;

  EventMap events;

  uintptr_t nextUid = 0;
};
