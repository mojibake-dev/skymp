#pragma once
#include <cstdint>

// thuum docs/DECISIONS.md ADR-068 (scripts as one system; rung 2, the one
// seam, docs/research/scripts-one-system-2026-11-05.md 6.2): the VM's one
// native dispatch, BSScript::NF_util::NativeFunctionBase::Call
// (CommonLibSSE-NG include/RE/N/NativeFunctionBase.h:47, IFunction's slot
// 0x0F; Address Library 104651, game/Offsets.h NativeFunctionCall), hooked
// once for every tabled native (NativeRows.inc, thuum docs/NATIVES.md's
// rows): the call judged from its typed frame, the subject's owner tested
// (a world object's cell host, an actor's simulator, the player's own seat),
// an owned action row's call (and a script's Kill) queued into this frame's
// batch and raised to JS once a frame as the event "scriptCalls", every
// other tabled call counted and raised every 30 s as "scriptCensus". The
// rows' functions are resolved at kDataLoaded from the VM's own bindings
// (Hooks.cpp BindNativeMethod's record, by class and name), never from a
// pinned offset (rule 1). This replaced the flip's 24 entry hooks (ADR-062
// decision 2) and their per-call "scriptNative" event.
namespace ScriptNatives {
// at plugin load: the dispatch hook's callbacks registered with the handler
// (no address yet, so the map the live hooks read is never touched later)
void Register();
// at kDataLoaded: the table's rows bound from the VM's bindings
// (Hooks::GetBoundNatives), then the VM's one dispatch hooked once the
// table's map is complete, where every tabled call is judged from its frame
// and the platform's latent calls are marked (PlatformCall.h)
void InstallHooks();
// every frame from the platform's OnUpdate: the census flushed on time (a
// quiet seat emits its last window; the review's medium finding)
void Tick();
}
