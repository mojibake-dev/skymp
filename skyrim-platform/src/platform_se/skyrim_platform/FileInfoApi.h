#pragma once

#include "NapiHelper.h"

namespace FileInfoApi {

Napi::Value FileInfo(const Napi::CallbackInfo& info);

inline void Register(Napi::Env env, Napi::Object& exports)
{
  // wrapped like every other callback: a C++ exception that leaves a Napi
  // callback unwinds through V8's frames and leaves the engine's handler
  // chain pointing at a dead frame (thuum docs/verbs/npc-puppet.md, the
  // landing hang of 2026-10-21 and 2026-10-22: this function's refusal of a
  // plugin name was that exception, on every login)
  exports.Set(
    "getFileInfo",
    Napi::Function::New(env, NapiHelper::WrapCppExceptions(FileInfo)));
}

}
