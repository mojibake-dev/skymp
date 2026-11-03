#pragma once

// thuum docs/DECISIONS.md ADR-062 decision 2 (slice 1b's review, 2026-11-03):
// the platform's own native calls (callNative's direct path on the worker,
// the latent path through VmCall::Run, every SpSnippet the server asks a
// seat to run, the client applying the server's word through setOpen, lock,
// activate, kill, setCurrentStageID, setValue) reach the same entries the
// script hooks watch. This thread-local depth marks them, so a hook can tell
// the platform's call from a script frame's and count it apart (rule 9: the
// decision stays in native).
namespace PlatformCall {
inline thread_local int t_depth = 0;

struct Scope
{
  Scope() { ++t_depth; }
  ~Scope() { --t_depth; }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;
};

inline bool Active()
{
  return t_depth > 0;
}
}
