#pragma once
#include <cstdint>

// thuum docs/DECISIONS.md ADR-062 decision 2 (slice 1b): the Papyrus natives
// through which a script changes server-owned state, hooked once each at
// their own entry and raised to JS as the event "scriptNative" for the
// reports the server records and relays (R2 by default, Eli: "trust should
// LARGELY be the default"). The entries are resolved at kDataLoaded from the
// VM's own bindings (Hooks.cpp BindNativeMethod's record, by class and name),
// never from a pinned offset (rule 1); the offsets found are logged for the
// reader to hold against docs/research/adr062/papyrus-native-entries.md.
namespace ScriptNatives {
// at plugin load: the hooks' callbacks registered with the handler (the
// entries' and the dispatch's; no address yet, so the map the live hooks
// read is never touched later)
void Register();
// at kDataLoaded: the table's rows (NativeRows.inc) bound from the VM's
// bindings and each native's entry attached (Hooks::GetBoundNatives), then
// the VM's one dispatch (NativeFunctionBase::Call) once the table's map is
// complete, where every tabled call is judged from its frame (thuum ADR-068,
// rung 2) and the platform's latent calls are marked (PlatformCall.h)
void InstallHooks();
// every frame from the platform's OnUpdate: the census flushed on time (a
// quiet seat emits its last window; the review's medium finding)
void Tick();
}
