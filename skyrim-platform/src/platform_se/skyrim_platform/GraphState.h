#pragma once

#include <string>
#include <string_view>

namespace RE {
class TESObjectREFR;
}

// thuum docs/verbs/traps.md "The graph state" (ADR-065 decision 2, the save
// is the state; ADR-073 decision 10's observe point): a world object's
// animation graph state as the engine itself keeps it across a save and a
// cell reload, read on its owner and restored on every other seat through
// the engine's own restore of a 3D load, for any graph (a pillar, a lever, a
// portcullis, a door's or a mechanism's), with no event replayed. The bytes
// are TESObjectREFR::SaveAnimation's (Address Library 1.6.1170 20098: the
// holder's behavior graphs by name, each state machine's active state, the
// variables and the clips' times, or a NiControllerManager's sequence and
// time; the biped's two flags), in the frame BGSLoadGameSubBuffer keeps them
// in for ExtraSavedAnimation (a version byte, the data's size as the
// engine's variable-width value, the data). Two TESModPlatform natives
// (PapyrusTESModPlatform.cpp; declared in TESModPlatform.psc and the
// committed .pex), each answering a JSON string, since callNative returns no
// object. Each runs inside TESModPlatform.Add's Call on the OnUpdate thread
// (callNative's direct path), and no C++ exception leaves one. The engine
// functions are named in GraphState.cpp with their evidence; every one is
// HYPOTHESIS (rule 2) until x-graph-state reads a capture restored on another
// seat. AE runtimes alone (the ids are 1.6.1170's; CommonLibSSE-NG names
// none of them).
namespace GraphState {

// SerializeGraphState(refr): the reference's graph state as its engine saves
// it at a 3D unload (BGSLoadGameSubBuffer::SaveAnimation, 36015), read only
// while its 3D is loaded (with none the engine's own unload save stands in
// its ExtraSavedAnimation and the graph has not moved since). A JSON object
// as a string:
//   { formId, graph: the frame as lowercase hex, bytes } or
//   { formId, refused: reason } for an actor (its animation is the stream's),
//   a runtime reference (0xff000000 and up), no 3D, nothing animated (no
//   behavior graph and no NiControllerManager), a frame past 8192 bytes (the
//   wire's cap::GRAPH_STATE), a frame the engine did not write whole, or a
//   runtime other than AE. Each reason's first refusal is logged as
//   `[verb:traps] SerializeGraphState refused <id>: <reason>`, the rest
//   counted (ADR-073 decision 7).
std::string Serialize(RE::TESObjectREFR* refr);

// ApplyGraphState(refr, graphHex): a served graph state restored on this
// seat's copy. With its 3D loaded, through BGSLoadGameSubBuffer::
// LoadAnimation (36012), the restore TESObjectREFR::Load3D runs for an
// ExtraSavedAnimation; with none, left as the reference's
// ExtraSavedAnimation (ExtraDataList::SetSavedAnimation, 11973), so its next
// 3D load restores it through the engine's own path. A JSON object as a
// string:
//   { formId, applied: true, how: "restored" | "held for its 3D load",
//     bytes } or
//   { formId, applied: false, retry, reason }: retry true for a moment's
//   refusal (a 3D whose behavior graph is not built yet, no memory), false
//   for one that cannot apply here (an actor, a runtime reference, bytes that
//   are not the engine's frame, a runtime other than AE).
std::string Apply(RE::TESObjectREFR* refr, std::string_view graphHex);

}
