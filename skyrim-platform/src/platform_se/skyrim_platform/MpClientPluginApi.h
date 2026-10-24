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
  exports.Set("mpClientPlugin", mpClientPlugin);
}
}
