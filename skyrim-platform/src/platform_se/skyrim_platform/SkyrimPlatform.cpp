#include "SkyrimPlatform.h"
#include "BrowserApi.h"    // APIs for register in CommonExecutionListener
#include "CallNativeApi.h" // CallNativeApi::NativeCallRequirements
#include "CameraApi.h"
#include "ConsoleApi.h" // CommonExecutionListener
#include "ConstEnumApi.h"
#include "DevApi.h"
#include "DirectoryMonitor.h"
#include "EncodingApi.h"
#include "EventsApi.h"
#include "ExceptionPrinter.h"
#include "FateApi.h"
#include "FileInfoApi.h"
#include "FileUtils.h"
#include "HandOff.h"
#include "HttpClient.h"
#include "HttpClientApi.h"
#include "InventoryApi.h"
#include "LoadGameApi.h"
#include "MagicApi.h"
#include "MpClientPluginApi.h"
#include "ObjectReferenceApi.h"
#include "PlatformCall.h"
#include "PuppetApi.h"
#include "ScriptNatives.h"
#include "Sp3Api.h"
#include "TextApi.h"
#include "ThreadPoolWrapper.h"
#include "Win32Api.h"

#include "NapiHelper.h"

#include "IPC.h" // IPC::Call

CallNativeApi::NativeCallRequirements g_nativeCallRequirements;

namespace {
bool EndsWith(const std::wstring& value, const std::wstring& ending)
{
  return ending.size() <= value.size() &&
    std::equal(ending.rbegin(), ending.rend(), value.rbegin());
}

// thuum tracker #129 (HandOff.h): true on the JavaScript thread, the pool's
// one thread, from its first task on; nothing else runs the pool's tasks
thread_local bool t_jsThread = false;

// the JavaScript thread alone (PushToWorkerAndWait): the first hand-off it
// serves inside each native's wait, the lab's read of which path ran (a COC
// reads "while it waited on Debug.CenterOnCell"). Keyed by the native's
// function, which the VM keeps for its life, so a served ask costs one set
// lookup and the name is formatted only at its first
void SayServed(const RE::BSScript::IFunction* fn,
               const HandOff::Ticket& ticket)
{
  static std::unordered_set<const RE::BSScript::IFunction*> said;
  if (!said.insert(fn).second) {
    return;
  }
  const std::string native = fn
    ? fmt::format("{}.{}", fn->GetObjectTypeName().c_str(),
                  fn->GetName().c_str())
    : std::string("an unnamed native");
  logger::info("[hand-off] the JavaScript thread served '{}' for thread {} "
               "while it waited on {} (thuum tracker #129; once per native)",
               ticket.label, ticket.thread, native);
}
}

namespace {
class TickListener
{
public:
  virtual ~TickListener() = default;
  virtual void Tick() = 0;
  virtual void Update() = 0;
};

class HelloTickListener : public TickListener
{
public:
  void Tick() override
  {
    if (auto console = RE::ConsoleLog::GetSingleton()) {
      if (!helloSaid) {
        helloSaid = true;
        console->Print("Hello SE");
      }
    }
  }

  void Update() override {}

private:
  bool helloSaid = false;
};

class CommonExecutionListener : public TickListener
{
  friend class SkyrimPlatform;

public:
  CommonExecutionListener()
    : nativeCallRequirements(g_nativeCallRequirements)
  {
  }

  void Tick() override
  {
    try {
      GetJsEngine()->AcquireEnvAndCall(
        [this](Napi::Env env) { TickImpl(env); });
    } catch (const std::exception& e) {
      ExceptionPrinter::Print(e);
    }
  }

  void Update() override
  {
    try {
      GetJsEngine()->AcquireEnvAndCall(
        [this](Napi::Env env) { UpdateImpl(env); });
    } catch (const std::exception& e) {
      ExceptionPrinter::Print(e);
    }
  }

private:
  void TickImpl(Napi::Env env)
  {
    auto& fileDirs = GetFileDirs();

    if (monitors.empty()) {
      for (auto& fileDir : fileDirs) {
        monitors.push_back(std::make_shared<DirectoryMonitor>(fileDir));
      }
    }

    ++tickId;

    std::vector<std::filesystem::path> pathsToLoad;

    bool hotReload = false;

    for (auto& monitor : monitors) {
      if (monitor->Updated()) {
        hotReload = true;
        // Do not break here. monitor->Updated has to be called for all
        // monitors. See method implementation
      }
      monitor->ThrowOnceIfHasError();
    }

    const bool startupLoad = tickId == 1;
    const bool loadNeeded = startupLoad || hotReload;
    if (loadNeeded) {
      ClearState();
      for (auto& fileDir : fileDirs) {
        LoadFiles(env, GetPathsToLoad(fileDir));
      }
    }

    HttpClientApi::GetHttpClient().ExecuteQueuedCallbacks(env);

    EventsApi::SendEvent("tick", {});
  }

  void UpdateImpl(Napi::Env env)
  {
    taskQueue.Update(env);
    nativeCallRequirements.jsThrQ->Update(env);
    jsPromiseTaskQueue.Update(env);
    TextApi::OnUpdate();
    EventsApi::SendEvent("update", {});
  }

  const std::vector<std::filesystem::path>& GetFileDirs() const
  {
    if (!pluginFolders) {
      try {
        pluginFolders = Settings::GetPlatformSettings()->GetPluginFolders();
      } catch (std::exception& e) {
        pluginFolders = std::make_unique<std::vector<std::filesystem::path>>();
        throw;
      }
    }

    return *pluginFolders;
  }

  void LoadFiles(Napi::Env env,
                 const std::vector<std::filesystem::path>& pathsToLoad)
  {
    for (auto& path : pathsToLoad) {
      // otherwise SkyrimPlatform tries to interpret
      // skymp5-client-settings.txt.txt as a JavaScript code
      if (EndsWith(path.wstring(), L".txt.txt")) {
        logger::error("Found file with double extension: {}", path.string());
        continue;
      }
      if (EndsWith(path.wstring(), L"-settings.txt")) {
        LoadSettingsFile(path);
        continue;
      }
      if (EndsWith(path.wstring(), L".js")) {
        LoadPluginFile(env, path);
        continue;
      }
      logger::warn("Found unprocessed file: {}", path.string());
    }
  }

  void LoadSettingsFile(const std::filesystem::path& path)
  {
    auto s = path.filename().wstring();
    s.resize(s.size() - strlen("-settings.txt"));

    auto pluginName = std::filesystem::path(s).string();
    logger::info("Found settings file {} for plugin {}.", path.string(),
                 pluginName);

    settingsByPluginName[pluginName] = Viet::ReadFileIntoString(path);
    settingsByPluginNameCache.reset();
  }

  static void OnLoadPluginFileCallback(CommonExecutionListener& self,
                                       const char* patchedSourceCode,
                                       uint32_t patchedSourceCodeLength)
  {
    self.tmpPatchedPluginSources =
      std::string{ patchedSourceCode,
                   patchedSourceCode + patchedSourceCodeLength };
  }

  std::string PatchPluginFile(const std::filesystem::path& path,
                              const std::string& scriptSrc)
  {
#pragma pack(push, 1)
    struct Msg
    {
      void* onLoadPluginFileCallback = nullptr;
      void* self = nullptr;
      const char* pluginPathUtf8 = nullptr;
      const char* scriptSrc = nullptr;
    };
#pragma pack(pop)
    static_assert(sizeof(Msg) == 32);

    auto s = path.u8string();
    std::string pluginPathUtf8(s.begin(), s.end());

    Msg msg;
    msg.onLoadPluginFileCallback = OnLoadPluginFileCallback;
    msg.self = this;
    msg.pluginPathUtf8 = pluginPathUtf8.data();
    msg.scriptSrc = scriptSrc.data();

    tmpPatchedPluginSources = std::nullopt;

    IPC::Call("SkyrimPlatform_OnLoadPluginFile",
              reinterpret_cast<uint8_t*>(&msg), sizeof(Msg));

    if (tmpPatchedPluginSources != std::nullopt) {
      return *tmpPatchedPluginSources;
    }

    return scriptSrc;
  }

  void LoadPluginFile(Napi::Env env, const std::filesystem::path& path)
  {
    auto scriptSrc = Viet::ReadFileIntoString(path);

    scriptSrc = PatchPluginFile(path, scriptSrc);

    getSettings = [this](const Napi::CallbackInfo& info) {
      if (!settingsByPluginNameCache) {
        auto result = Napi::Object::New(info.Env());
        for (const auto& [pluginName, settings] : settingsByPluginName) {
          auto parsedSettings = NapiHelper::ParseJson(info.Env(), settings);
          result.Set(pluginName, parsedSettings);
        }
        settingsByPluginNameCache.reset(
          new Napi::Reference<Napi::Object>(Napi::Persistent(result)));
      }
      return (*settingsByPluginNameCache).Value();
    };

    setSettings = [](const Napi::CallbackInfo& info) -> Napi::Value {
      throw std::runtime_error("settings setter not implemented");
      return info.Env().Undefined();
    };

    // We will be able to use require()
    auto devApi = Napi::Object::New(env);

    DevApi::NativeExportsMap nativeExportsMap;

    nativeExportsMap["skyrimPlatform"] = [this](Napi::Object e) {
      // thuum tracker #129 (HandOff.h, the rule's second half): inside a
      // task an engine thread waits on (a hook's hand-off, the tick, a
      // console command), the requirements carry no VM, so callNative
      // refuses "can't be called in this context" as it does outside the
      // update: a native there would wait on the worker while an engine
      // thread waits on this thread, the cycle's shape. Outside the update's
      // window the VM is unset already (it is set only inside main.cpp's
      // OnUpdate) and those tasks meet that refusal as they always did; only
      // a VM withheld here is marked, with the task it was withheld in, for
      // callNative's line (CallNativeApi.cpp); events.md's "Script functions
      // are not available inside the enter and leave handlers"
      auto getNativeCallRequirements =
        [this]() -> CallNativeApi::NativeCallRequirements {
        if (HandOff::Serving() && nativeCallRequirements.vm) {
          CallNativeApi::NativeCallRequirements requirements =
            nativeCallRequirements;
          requirements.vm = nullptr;
          requirements.withheldInside = HandOff::ServingWhat();
          return requirements;
        }
        return nativeCallRequirements;
      };
      auto env = e.Env();
      auto engine = GetJsEngine();
      EncodingApi::Register(env, e);
      LoadGameApi::Register(env, e);
      CameraApi::Register(env, e);
      MpClientPluginApi::Register(env, e);
      ObjectReferenceApi::Register(env, e);
      HttpClientApi::Register(env, e);
      ConsoleApi::Register(env, e);
      DevApi::Register(env, e, {}, GetFileDirs());
      EventsApi::Register(env, e);
      BrowserApi::Register(env, e);
      Win32Api::Register(env, e);
      FileInfoApi::Register(env, e);
      TextApi::Register(env, e);
      InventoryApi::Register(env, e);
      MagicApi::Register(env, e);
      PuppetApi::Register(env, e);
      FateApi::Register(env, e);
      ConstEnumApi::Register(env, e);
      CallNativeApi::Register(env, e, getNativeCallRequirements);
      Sp3Api::Register(env, e, getNativeCallRequirements);

      auto getter = NapiHelper::WrapCppExceptions(getSettings);
      auto setter = NapiHelper::WrapCppExceptions(setSettings);

      Napi::PropertyDescriptor settingsProperty =
        Napi::PropertyDescriptor::Accessor("settings", getter, setter);
      e.DefineProperty(settingsProperty);

      return e;
    };

    auto engine = GetJsEngine();
    DevApi::Register(env, devApi, nativeExportsMap, GetFileDirs());

    Napi::Object consoleApi = Napi::Object::New(env);
    ConsoleApi::Register(env, consoleApi);

    // I commented out require, because NodeJS has its own require function
    // I think it's the part of systemjs polyfill and it should be removed
    for (auto f : { /*"require", */ "addNativeExports" }) {
      env.Global().Set(f, devApi.Get(f));
    }
    env.Global().Set("log", consoleApi.Get("printConsole"));

    engine->RunScript(
      env, "skyrimPlatform = addNativeExports('skyrimPlatform', {})", "");
    engine->RunScript(env, scriptSrc, path.filename().string()).ToString();
  }

  void ClearState()
  {
    ConsoleApi::Clear();
    EventsApi::Clear();
    taskQueue.Clear();
    jsPromiseTaskQueue.Clear();
    nativeCallRequirements.jsThrQ->Clear();
    settingsByPluginName.clear();
    settingsByPluginNameCache.reset();
  }

  JsEngine* GetJsEngine() { return JsEngine::GetSingleton(); }

  std::vector<std::filesystem::path> GetPathsToLoad(
    const std::filesystem::path& directory)
  {
    std::vector<std::filesystem::path> paths;
    if (std::filesystem::exists(directory)) {
      for (auto& it : std::filesystem::directory_iterator(directory)) {
        if (!it.is_directory()) {
          paths.push_back(it.path());
        }
      }
    }
    return paths;
  }

  std::vector<std::shared_ptr<DirectoryMonitor>> monitors;
  uint32_t tickId = 0;
  Viet::TaskQueue<Napi::Env> taskQueue;
  Viet::TaskQueue<Napi::Env> jsPromiseTaskQueue;
  CallNativeApi::NativeCallRequirements& nativeCallRequirements;
  std::unordered_map<std::string, std::string> settingsByPluginName;
  std::unique_ptr<Napi::Reference<Napi::Object>> settingsByPluginNameCache;
  std::function<Napi::Value(const Napi::CallbackInfo& info)> getSettings,
    setSettings;
  mutable std::unique_ptr<std::vector<std::filesystem::path>> pluginFolders;
  std::optional<std::string> tmpPatchedPluginSources;
};
}

typedef asio::executor_work_guard<asio::io_context::executor_type> SPWorkGuard;

struct SkyrimPlatform::Impl
{
  std::shared_ptr<CommonExecutionListener> commonExecutionListener;
  std::vector<std::shared_ptr<TickListener>> tickListeners;
  Viet::TaskQueue<Napi::Env> tickTasks, updateTasks;
  ThreadPoolWrapper pool;

  // thuum tracker #129: the hooks' asks of the JavaScript thread
  // (HandOffAndWait), served in the pool's turn or by the JavaScript thread
  // while it waits on the worker (PushToWorkerAndWait); the worker's
  // complete flag lives under its lock
  HandOff::Desk handOffs;

  // Stuff needed to push functions from js to game thread
  asio::io_context ioContext;
  bool complete;
  std::shared_ptr<SPWorkGuard> workGuard;
  void RunInIOContext(RE::BSTSmartPointer<RE::BSScript::IFunction> fPtr,
                      const RE::BSTSmartPointer<RE::BSScript::Stack>& stack,
                      RE::BSScript::ErrorLogger* logger,
                      RE::BSScript::Internal::VirtualMachine* vm,
                      RE::BSScript::IFunction::CallResult* ret)
  {
    {
      // the platform's own call, marked for the seam (ScriptNatives.cpp;
      // thuum ADR-062 decision 2, ADR-068) and counted in its census here:
      // made inside the OnUpdate native's own Call, it never reaches the
      // seam's replacement (Gum runs a nested call straight to the original)
      ScriptNatives::CountPlatformCall(fPtr.get());
      PlatformCall::Scope platformCall;
      *ret = fPtr->Call(stack, logger, vm, false);
    }
    // the JavaScript thread waits on this flag, serving the hooks' asks
    // meanwhile (PushToWorkerAndWait)
    handOffs.Signal([this] { complete = true; });
  }
};

SkyrimPlatform::SkyrimPlatform()
{
  pImpl = std::make_shared<Impl>();

  pImpl->commonExecutionListener = std::make_shared<CommonExecutionListener>();

  pImpl->tickListeners.push_back(std::make_shared<HelloTickListener>());
  pImpl->tickListeners.push_back(pImpl->commonExecutionListener);
  pImpl->complete = false;
}

SkyrimPlatform* SkyrimPlatform::GetSingleton()
{
  static SkyrimPlatform g_skyrimPlatform;
  return &g_skyrimPlatform;
}

void SkyrimPlatform::JsTick(Napi::Env env, bool gameFunctionsAvailable)
{
  if (gameFunctionsAvailable) {
    pImpl->commonExecutionListener->GetJsEngine()->Tick();
  }

  for (auto& listener : pImpl->tickListeners) {
    gameFunctionsAvailable ? listener->Update() : listener->Tick();
  }

  try {
    (gameFunctionsAvailable ? pImpl->updateTasks : pImpl->tickTasks)
      .Update(env);
  } catch (const std::exception& e) {
    ExceptionPrinter::Print(e);
  }
}

void SkyrimPlatform::AddTickTask(const std::function<void(Napi::Env)>& f)
{
  pImpl->tickTasks.AddTask(f);
}

void SkyrimPlatform::AddUpdateTask(const std::function<void(Napi::Env)>& f)
{
  pImpl->updateTasks.AddTask(f);
}

// the tick (TickHandler.h) and a console command (ConsoleApi.cpp) wait here
// in the pool's order, never served inside the JavaScript thread's wait on
// the worker (thuum tracker #129, HandOff.h): the JavaScript thread waits on
// the engine only inside the update (a native needs the VM main.cpp's
// OnUpdate sets), wholly inside the VM's job, and HYPOTHESIS: no native the
// VM's job runs waits on either of those threads. The main thread waits on
// that job at its frame's join (the #129 dump's thread 6032, and the
// platform's rule that a native never waits on a main-thread task), and the
// tick's task runs in the frame's post-process stage (skse64 drains
// AddTask's queue there), after the job is joined. A hook's ask comes from
// any thread, a loader a native waits on among them (the dump's background
// process thread), and goes through HandOffAndWait. Both are tasks an engine
// thread waits on, so a native inside either is refused
// (getNativeCallRequirements)
void SkyrimPlatform::PushAndWait(const std::function<void(Napi::Env)>& f)
{
  pImpl->pool.PushAndWait([this, f] {
    t_jsThread = true;
    HandOff::ServingScope serving("the tick or a console command");
    auto engine = pImpl->commonExecutionListener->GetJsEngine();
    engine->AcquireEnvAndCall(f);
  });
}

void SkyrimPlatform::Push(const std::function<void(Napi::Env)>& f)
{
  pImpl->pool.Push([this, f] {
    t_jsThread = true;
    auto engine = pImpl->commonExecutionListener->GetJsEngine();
    engine->AcquireEnvAndCall(f);
  });
}

// thuum tracker #129 (HandOff.h): a hook's enter or leave, run on the
// JavaScript thread while the asking engine thread waits, in the pool's turn
// or earlier, by the JavaScript thread waiting on the worker
// (PushToWorkerAndWait). Asked on the JavaScript thread itself (an engine
// call made there outside callNative's mark sending the event) it is
// declined, false: the pool's one thread would wait on itself
bool SkyrimPlatform::HandOffAndWait(const std::string& label,
                                    const std::function<void(Napi::Env)>& f)
{
  if (t_jsThread) {
    static std::atomic<bool> said{ false };
    if (!said.exchange(true)) {
      logger::warn("[hand-off] '{}' asked on the JavaScript thread itself is "
                   "declined, no handler called: it would wait on itself "
                   "(thuum tracker #129); said once",
                   label);
    }
    return false;
  }
  auto ticket = pImpl->handOffs.Ask(
    [this, f] {
      // a served ask is no part of the native call the JavaScript thread
      // may be waiting on: callNative's Scope (CallNativeApi.cpp) holds the
      // platform's mark (PlatformCall.h) on this thread for that wait, so
      // the mark is lowered for the ask's extent and an event the handlers
      // cause meets D-S0-14 B's rules as any world event does
      struct OutsideCall
      {
        int saved = PlatformCall::t_depth;
        OutsideCall() { PlatformCall::t_depth = 0; }
        ~OutsideCall() { PlatformCall::t_depth = saved; }
      } outsideCall;
      try {
        pImpl->commonExecutionListener->GetJsEngine()->AcquireEnvAndCall(f);
      } catch (const std::exception& e) {
        logger::error("[hand-off] a hook's task failed: {}", e.what());
      }
    },
    label, static_cast<std::uint32_t>(GetCurrentThreadId()));
  // the pool's turn for the ask; when the JavaScript thread served it inside
  // a wait already, this turn finds nothing of its own to run
  pImpl->pool.Push([this] {
    t_jsThread = true;
    pImpl->handOffs.ServeOne();
  });
  pImpl->handOffs.Wait(ticket);
  return true;
}

void SkyrimPlatform::PushToWorkerAndWait(
  RE::BSTSmartPointer<RE::BSScript::IFunction> fPtr,
  const RE::BSTSmartPointer<RE::BSScript::Stack>& stack,
  RE::BSScript::ErrorLogger* logger,
  RE::BSScript::Internal::VirtualMachine* vm,
  RE::BSScript::IFunction::CallResult* ret)
{
  pImpl->handOffs.Signal([this] { pImpl->complete = false; });
  asio::post(
    pImpl->ioContext.get_executor(),
    std::bind(&Impl::RunInIOContext, pImpl, fPtr, stack, logger, vm, ret));
  // the asks are the JavaScript thread's to run: every caller today is a
  // Napi callback (CallNative.cpp, from callNative and the typed bindings),
  // but a C++ caller on another thread would run the hooks' JavaScript
  // there, so it waits without serving and is said once
  if (!t_jsThread) {
    static std::atomic<bool> said{ false };
    if (!said.load(std::memory_order_relaxed) && !said.exchange(true)) {
      spdlog::warn("[hand-off] {}.{} was asked of the worker from thread {}, "
                   "not the JavaScript thread: it waits without serving the "
                   "hooks' asks (thuum tracker #129); said once",
                   fPtr ? fPtr->GetObjectTypeName().c_str() : "",
                   fPtr ? fPtr->GetName().c_str() : "", GetCurrentThreadId());
    }
    pImpl->handOffs.WaitFor([this] { return pImpl->complete; });
    return;
  }
  // thuum tracker #129: the JavaScript thread waits on the worker and serves
  // every hook's ask queued meanwhile, as the worker serves this thread's
  // calls while it waits on the update (main.cpp's OnUpdate). The native may
  // wait on an engine thread whose hook asks this thread (a cell load's
  // background 3D load playing a container's Close, the #129 dump read,
  // HYPOTHESIS until a second dump or a Frida read of PlayAnimation's thread
  // agrees: HandOff.h), so a plain wait here closed the cycle. A served ask
  // runs inside this call, at the update's native call site, before the
  // rest of the update: the handlers keep to JavaScript state (they call no
  // native: getNativeCallRequirements), and state the update writes for a
  // handler to read is read in the next update, not in the handler
  // (docs/skyrim_platform/events.md)
  pImpl->handOffs.WaitServing(
    [this] { return pImpl->complete; },
    [&fPtr](const HandOff::Ticket& ticket) { SayServed(fPtr.get(), ticket); });
}

void SkyrimPlatform::PrepareWorker()
{
  if (pImpl->ioContext.stopped()) {
    pImpl->ioContext.restart();
  }
  pImpl->workGuard =
    std::make_shared<SPWorkGuard>(pImpl->ioContext.get_executor());
}

void SkyrimPlatform::StartWorker()
{
  pImpl->ioContext.run();
}

void SkyrimPlatform::StopWorker()
{
  pImpl->ioContext.stop();
}
