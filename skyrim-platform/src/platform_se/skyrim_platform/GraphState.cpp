#include "GraphState.h"

#include <RE/B/BGSLoadGameSubBuffer.h>
#include <RE/B/BSAnimationGraphManager.h>
#include <RE/B/BSTSmartPointer.h>
#include <RE/E/ExtraDataList.h>
#include <RE/M/MemoryManager.h>
#include <RE/N/NiAVObject.h>
#include <RE/N/NiControllerManager.h>
#include <RE/N/NiRTTI.h>
#include <RE/N/NiTimeController.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>
#include <REL/Relocation.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fmt/format.h>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

/**
 * thuum docs/verbs/traps.md "The graph state". The engine keeps a world
 * object's animation across a cell reload and a save with one pair of
 * functions, read with Ghidra on sky-re (2026-11-14, program
 * SkyrimSE-1.6.1170.0.exe.unpacked.exe; Address Library ids by `just addr`,
 * none named by CommonLibSSE-NG; thuum CLAUDE.md rule 1). HYPOTHESIS (rule
 * 2) until x-graph-state v1 reads an owner's capture restored on another
 * seat:
 *   H-G1 36015 (0x1406258c0) is BGSLoadGameSubBuffer::SaveAnimation(sub,
 *        form): its own message names it ("Form '%s' %08X passed to
 *        BGSLoadGameSubBuffer::SaveAnimation() is not a reference"); it
 *        builds a scratch BGSSaveFormBuffer (0x140626a90, 0x140626ce0), runs
 *        TESObjectREFR::SaveAnimation 20098 (0x1402f5de0) into it, copies the
 *        written bytes into the sub-buffer as one engine allocation
 *        [version][size][data] (0x140625d00, the size by 0x140601a60's
 *        variable width) and frees the scratch. TESObjectREFR::Set3D calls it
 *        at a 3D unload (0x1402e4dad) and hands the sub-buffer to 11973.
 *   H-G2 36012 (0x140625630) is BGSLoadGameSubBuffer::LoadAnimation(sub,
 *        form): the sub-buffer read into a BGSLoadFormBuffer (0x140625b80,
 *        which frees the sub-buffer's memory with the default heap and nulls
 *        its pointer), then TESObjectREFR::LoadAnimation 20099 (0x1402f5fe0).
 *        ExtraDataList's restore 11975 (0x14016a930) calls it from
 *        TESObjectREFR::Load3D (0x1402e4136) for an ExtraSavedAnimation and
 *        removes the extra.
 *   H-G3 11973 (0x14016a7d0) is ExtraDataList::SetSavedAnimation(list, sub):
 *        under the list's write lock it adds an ExtraSavedAnimation
 *        (ExtraDataTypes.h:239, kSavedAnimation; ExtraSavedAnimation.h:20,
 *        its BGSLoadGameSubBuffer at 0x10) or replaces the one there (its
 *        message "SetSavedAnimation() is being called when there is already
 *        a saved animation.", the old buffer freed) and keeps the
 *        sub-buffer's pointer.
 *   H-G4 the data is seat-portable: 20098 writes the graph manager's graphs
 *        by project name, each with lists keyed by name (state machines and
 *        their active states, variables, clips and their times, active
 *        transitions; BShkbAnimationGraph's save 0x140bb3c50), strings as a
 *        u16 length and bytes (0x140627090), and the biped's two flags; no
 *        form id and no pointer. The load (0x140ba4800) restores a graph
 *        only where the count and the project name match, skipping any
 *        other by its name's registered reader.
 *   H-G5 the save and the restore are safe on this thread (the Papyrus VM's,
 *        where the platform's PlayAnimation and SetAnimationVariable* run
 *        too): both take the graph manager's own lock (0x1401949b0 on the
 *        manager at 0xa0).
 *   H-G6 the data's first byte is the holder's graph flag (0x14054b260's
 *        first write, a bool, slot 6 of AnimationStreamSaveGame): 1 when the
 *        reference had a behavior graph, so a restore of such bytes needs
 *        the graph built on this 3D (0x14054b390 reads into a scratch manager
 *        and drops it when the holder has none).
 * The engine's frame is the wire's (wire-validate graph_state_frame), and
 * nothing here walks the data: the receiver's engine reads it (R2, recorded
 * unvalidated, docs/verbs/traps.md's Known limits).
 */

namespace {

using SubBufferAnimationFn = void(RE::BGSLoadGameSubBuffer*, RE::TESForm*);
using SetSavedAnimationFn = void(RE::ExtraDataList*,
                                 RE::BGSLoadGameSubBuffer*);

// the wire's cap::GRAPH_STATE: a frame past it is never sent
constexpr std::size_t kMaxFrame = 8192;
constexpr std::uint32_t kFirstRuntimeId = 0xff000000;

// the frame's header and data sizes, 0 for bytes that are no whole frame
// (the engine's own read of its size, 0x140601ab0: the low two bits the
// width code, 0 one byte, 1 two, 2 four, little-endian, the size the value
// shifted right by two; wire-validate graph_state_frame's rule). `n` is the
// bytes known to be readable, or 0 to read the frame's own total: only the
// header is read then (the engine's allocation holds the whole frame)
struct Frame
{
  std::size_t header = 0;
  std::size_t data = 0;
  std::size_t Total() const { return header + data; }
};

Frame ReadFrame(const std::uint8_t* p, std::size_t n)
{
  Frame f;
  if (!p || (n != 0 && n < 2)) {
    return f;
  }
  const std::uint8_t code = p[1] & 3;
  std::size_t width = 0;
  std::uint32_t raw = 0;
  if (code == 0) {
    width = 1;
    raw = p[1];
  } else if (code == 1) {
    if (n != 0 && n < 3) {
      return f;
    }
    width = 2;
    raw = static_cast<std::uint32_t>(p[1]) |
      (static_cast<std::uint32_t>(p[2]) << 8);
  } else if (code == 2) {
    if (n != 0 && n < 5) {
      return f;
    }
    width = 4;
    raw = static_cast<std::uint32_t>(p[1]) |
      (static_cast<std::uint32_t>(p[2]) << 8) |
      (static_cast<std::uint32_t>(p[3]) << 16) |
      (static_cast<std::uint32_t>(p[4]) << 24);
  } else {
    return f;
  }
  const std::size_t size = raw >> 2;
  if (size == 0 || (n != 0 && 1 + width + size != n)) {
    return f;
  }
  f.header = 1 + width;
  f.data = size;
  return f;
}

std::string ToHex(const std::uint8_t* data, std::size_t size)
{
  static const char* digits = "0123456789abcdef";
  std::string out;
  out.resize(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    out[2 * i] = digits[data[i] >> 4];
    out[2 * i + 1] = digits[data[i] & 0x0f];
  }
  return out;
}

// lowercase hex alone, the wire's spelling
bool FromHex(std::string_view hex, std::vector<std::uint8_t>& out)
{
  if (hex.size() % 2 != 0) {
    return false;
  }
  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    return -1;
  };
  out.clear();
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = digit(hex[i]);
    const int lo = digit(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return true;
}

// ADR-073 decision 7: each refusal reason's first line, the rest counted and
// flushed in one line at most every 30 s, on the next refusal (the platform
// has no periodic summary of its own; WeaponAnimGuard's shape)
struct Refusals
{
  std::mutex m;
  std::vector<std::pair<std::string, std::uint64_t>> counts;
  std::chrono::steady_clock::time_point flushed{};
};

Refusals& R()
{
  static Refusals r;
  return r;
}

void NoteRefusal(const char* native, std::uint32_t formId,
                 const std::string& reason)
{
  auto& r = R();
  std::lock_guard l(r.m);
  for (auto& [kind, n] : r.counts) {
    if (kind == reason) {
      ++n;
      const auto now = std::chrono::steady_clock::now();
      if (now - r.flushed >= std::chrono::seconds(30)) {
        r.flushed = now;
        std::string line;
        for (const auto& [k, c] : r.counts) {
          line += fmt::format("{}{}: {}", line.empty() ? "" : "; ", k, c);
        }
        logger::info("[verb:traps] graph state refusals since the start: {}",
                     line);
      }
      return;
    }
  }
  r.counts.emplace_back(reason, 1);
  logger::info("[verb:traps] {} refused {:x}: {} (the first of its kind; "
               "the rest counted)",
               native, formId, reason);
}

// the premises both natives share; "" when they hold
std::string Premise(RE::TESObjectREFR* refr)
{
  if (!REL::Module::IsAE()) {
    return "not the AE runtime (the ids are 1.6.1170's)";
  }
  if (!refr) {
    return "no reference";
  }
  if (refr->GetFormID() >= kFirstRuntimeId) {
    return "a runtime reference";
  }
  if (refr->GetFormType() == RE::FormType::ActorCharacter) {
    return "an actor (its animation is the stream's)";
  }
  return "";
}

bool HasBehaviorGraph(RE::TESObjectREFR* refr)
{
  RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
  return refr->GetAnimationGraphManager(manager) && manager;
}

bool HasSequences(RE::NiAVObject* root)
{
  const RE::NiTimeController* controllers =
    root ? root->GetControllers() : nullptr;
  return controllers &&
    netimmerse_cast<const RE::NiControllerManager*>(controllers);
}

std::string SerializeImpl(RE::TESObjectREFR* refr)
{
  const std::uint32_t formId = refr ? refr->GetFormID() : 0;
  nlohmann::json out{ { "formId", formId } };
  const auto refuse = [&](const std::string& reason) {
    NoteRefusal("SerializeGraphState", formId, reason);
    out["refused"] = reason;
    return out.dump();
  };
  if (auto premise = Premise(refr); !premise.empty()) {
    return refuse(premise);
  }
  RE::NiAVObject* root = refr->Get3D();
  if (!root) {
    return refuse("no 3D (its unload's own save stands)");
  }
  if (!HasBehaviorGraph(refr) && !HasSequences(root)) {
    return refuse("nothing animated (no behavior graph, no "
                  "NiControllerManager)");
  }
  RE::BGSLoadGameSubBuffer sub{};
  sub.buffer.buffer = nullptr;
  REL::Relocation<SubBufferAnimationFn> save{ REL::ID(36015) };
  save(&sub, refr);
  auto* bytes = reinterpret_cast<const std::uint8_t*>(sub.buffer.buffer);
  if (!bytes) {
    return refuse("the engine saved nothing");
  }
  const Frame frame = ReadFrame(bytes, 0);
  const std::size_t total = frame.Total();
  std::string hex;
  if (total != 0 && total <= kMaxFrame) {
    hex = ToHex(bytes, total);
  }
  RE::free(sub.buffer.buffer);
  sub.buffer.buffer = nullptr;
  if (total == 0) {
    return refuse("the engine's frame did not read whole");
  }
  if (total > kMaxFrame) {
    return refuse(fmt::format("a frame past the wire's {} bytes", kMaxFrame));
  }
  out["graph"] = std::move(hex);
  out["bytes"] = total;
  return out.dump();
}

std::string ApplyImpl(RE::TESObjectREFR* refr, std::string_view graphHex)
{
  const std::uint32_t formId = refr ? refr->GetFormID() : 0;
  nlohmann::json out{ { "formId", formId }, { "applied", false } };
  const auto refuse = [&](const std::string& reason, bool retry) {
    if (!retry) {
      NoteRefusal("ApplyGraphState", formId, reason);
    }
    out["retry"] = retry;
    out["reason"] = reason;
    return out.dump();
  };
  if (auto premise = Premise(refr); !premise.empty()) {
    return refuse(premise, false);
  }
  std::vector<std::uint8_t> bytes;
  if (!FromHex(graphHex, bytes) || bytes.size() > kMaxFrame) {
    return refuse("not lowercase hex within the wire's cap", false);
  }
  const Frame frame = ReadFrame(bytes.data(), bytes.size());
  if (frame.Total() == 0) {
    return refuse("not the engine's frame", false);
  }
  // H-G6: bytes of a reference with a behavior graph restore only into one
  // built on this 3D
  const bool graphData = bytes[frame.header] != 0;
  RE::NiAVObject* root = refr->Get3D();
  if (root && graphData && !HasBehaviorGraph(refr)) {
    return refuse("its 3D has no behavior graph yet", true);
  }
  void* memory = RE::malloc(bytes.size());
  if (!memory) {
    return refuse("no memory for the sub-buffer", true);
  }
  std::memcpy(memory, bytes.data(), bytes.size());
  RE::BGSLoadGameSubBuffer sub{};
  sub.buffer.buffer = static_cast<char*>(memory);
  if (root) {
    REL::Relocation<SubBufferAnimationFn> load{ REL::ID(36012) };
    load(&sub, refr);
    // H-G2: the load frees the sub-buffer and nulls it; never left here
    if (sub.buffer.buffer) {
      RE::free(sub.buffer.buffer);
      sub.buffer.buffer = nullptr;
    }
    out["how"] = "restored";
  } else {
    // H-G3: the list keeps the pointer; the next Load3D restores and frees it
    REL::Relocation<SetSavedAnimationFn> hold{ REL::ID(11973) };
    hold(&refr->extraList, &sub);
    out["how"] = "held for its 3D load";
  }
  out["applied"] = true;
  out["bytes"] = bytes.size();
  return out.dump();
}

}

// no C++ exception leaves a native: past TESModPlatform.Add a throw would
// freeze the seat (ChangeForm.cpp's note on the public natives)
std::string GraphState::Serialize(RE::TESObjectREFR* refr)
{
  try {
    return SerializeImpl(refr);
  } catch (const std::exception& e) {
    logger::error("[verb:traps] SerializeGraphState threw: {}", e.what());
  } catch (...) {
    logger::error("[verb:traps] SerializeGraphState threw");
  }
  return "";
}

std::string GraphState::Apply(RE::TESObjectREFR* refr,
                              std::string_view graphHex)
{
  try {
    return ApplyImpl(refr, graphHex);
  } catch (const std::exception& e) {
    logger::error("[verb:traps] ApplyGraphState threw: {}", e.what());
  } catch (...) {
    logger::error("[verb:traps] ApplyGraphState threw");
  }
  return "";
}
