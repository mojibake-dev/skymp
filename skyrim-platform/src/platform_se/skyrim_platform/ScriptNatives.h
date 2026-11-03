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
void InstallHooks();
}
