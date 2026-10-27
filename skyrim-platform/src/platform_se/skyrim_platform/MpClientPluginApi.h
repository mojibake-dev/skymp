#pragma once
#include "NapiHelper.h"

namespace MpClientPluginApi {
Napi::Value GetVersion(const Napi::CallbackInfo& info);
Napi::Value CreateClient(const Napi::CallbackInfo& info);
Napi::Value DestroyClient(const Napi::CallbackInfo& info);
Napi::Value IsConnected(const Napi::CallbackInfo& info);
Napi::Value Tick(const Napi::CallbackInfo& info);
Napi::Value Send(const Napi::CallbackInfo& info);
Napi::Value SendRaw(const Napi::CallbackInfo& args);

// thuum docs/verbs/cell-host.md (the quitting host, ADR-047's D12 B): the
// client torn down from the game's own quit path (Hooks.cpp,
// OnQuitShutdown) ahead of the engine's shutdown, so the server hears the
// disconnect at the quit and not at netcode's timeout. False when no client
// was ever created in this process: nothing to tear down, the plugin DLL
// left alone.
bool DestroyClientAtQuit();

// thuum docs/research/m3/decisions-after-playtest-34.md D24 (B): whether the
// quit thunk ends the process after the client's teardown instead of running
// the engine's shutdown, which crashes at every quit in the Scaleform heap
// teardown (two SKSE plugins hold the Loading Menu's IMenu in statics that
// nothing clears, cell-host.md's quitting-host row). Set by skymp5-client
// from its settings (quitFastExit, default on); off until it says so
Napi::Value SetQuitFastExit(const Napi::CallbackInfo& info);
bool QuitFastExit();

inline void Register(Napi::Env env, Napi::Object& exports)
{
  auto mpClientPlugin = Napi::Object::New(env);
  mpClientPlugin.Set(
    "getVersion",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(GetVersion)));
  mpClientPlugin.Set(
    "createClient",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(CreateClient)));
  mpClientPlugin.Set(
    "destroyClient",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(DestroyClient)));
  mpClientPlugin.Set(
    "isConnected",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(IsConnected)));
  mpClientPlugin.Set(
    "tick", Napi::Function::New(env, NapiHelper::WrapCppExceptions(Tick)));
  mpClientPlugin.Set(
    "send", Napi::Function::New(env, NapiHelper::WrapCppExceptions(Send)));
  mpClientPlugin.Set(
    "sendRaw",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SendRaw)));
  mpClientPlugin.Set(
    "setQuitFastExit",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(SetQuitFastExit)));
  exports.Set("mpClientPlugin", mpClientPlugin);
}
}
