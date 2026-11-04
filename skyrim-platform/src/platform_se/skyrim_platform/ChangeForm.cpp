#include "ChangeForm.h"
#include "FridaHookHandler.h"
#include "game/Offsets.h"

#include <RE/A/AIProcess.h>
#include <RE/A/Actor.h>
#include <RE/A/ActorState.h>
#include <RE/B/BGSNumericIDIndex.h>
#include <RE/B/BGSSaveFormBuffer.h>
#include <RE/B/BGSSaveLoadGame.h>
#include <RE/B/BGSSaveLoadManager.h>
#include <RE/H/HighProcessData.h>
#include <RE/M/Main.h>
#include <RE/M/MemoryManager.h>
#include <RE/P/PlayerCharacter.h>
#include <RE/T/TESDataHandler.h>
#include <RE/T/TESForm.h>
#include <RE/T/TESObjectREFR.h>
#include <REL/Relocation.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fmt/format.h>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/**
 * thuum docs/verbs/change-form.md (ADR-065, system 1; ADR-067's corpse first):
 * the engine's own change form, serialized on demand on the owner and applied
 * on a watcher, the C++ of lab/frida/serialize-on-demand-actor.js v2.2 and
 * lab/frida/apply-on-demand.js v2.3. The facts: docs/research/
 * adr065-serialize-on-demand-2026-11-03.md (the id tables; Q1 the buffer's
 * layout and the body; Q2 the globals and the form-id map; Q3 the entry and
 * the scratch route; Q4 the recipe; the actor class Q1 to Q7; the (c) read
 * Q1 to Q5; D6 the ids across seats and the REMAP step) and docs/engine/
 * change-forms.md C4 to C6. Every id below is game/Offsets.h's ChangeForm
 * block (Address Library 1.6.1170, `just addr` 2026-11-03) or
 * CommonLibSSE-NG's own header, cited at its use. AE only, as every id of the
 * fork's thuum hooks (ADR-025's 1.6.1170).
 *
 * The ids used, with their sources:
 *   36035 BGSSaveFormBuffer ctor (buf) -> buf        note, the id table row 5
 *   36037 the nine-byte header (buf, id, flags, formType, version)  row 5
 *   36044 BGSSaveFormBuffer destroy (frees +0x08)     row 5
 *   35715 the initial data and the Havok block (buf, form, flags) -> kind
 *         row 10
 *   36081 the pack, buffer to record on the engine heap (out, buf)   row 7
 *   35740 the change form version byte (0x4E)         row 15
 *   35503 the changes map's read (map, id, Value*) -> bool    row 14
 *   35509 the changes map's write (map, id, const Value*)     row 14
 *   35568 the apply's stack-buffer wrapper (mgr, form) -> bool   row 13
 *   35567 the finish pass (mgr, fromFullLoad)          row 13
 *   35928 BGSNumericIDIndex::SetNumericID: CommonLibSSE-NG's RELOCATION_ID
 *         (35027, 35928) at include/RE/B/BGSNumericIDIndex.h:28; the census
 *         hook's address, and the REMAP encode through the header's inline
 *   19527 TESObjectREFR::CheckSaveGame, slot 0x0D of every actor (the actor
 *         section Q3; CONFIRMED-STATIC); 37649 Actor::SaveGame and 40266
 *         Character::SaveGame, slot 0x0E (Q7); 37650 and 40267 the LoadGame
 *         slots 0x0F ((c) id rows): the vtable premises, compared by address
 *   the virtual slots themselves are called through CommonLibSSE-NG's
 *         TESForm::CheckSaveGame and SaveGame (include/RE/T/TESForm.h:136-137)
 *   MemoryManager::GetSingleton, Allocate, Deallocate: CommonLibSSE-NG
 *         include/RE/M/MemoryManager.h:29-47 (ids 11141, 68115, 68117)
 *   TESForm::LookupByID: the header's inline over GetAllForms under its
 *         read lock (TESForm.h:190-220), what the engine's 14617 does
 *   BGSSaveLoadGame::GetSingleton (403330, BGSSaveLoadGame.h:71), its flags
 *         (+0x340, :61-63), the changes map (+0x330, :94), the pending array
 *         (+0x318, :93); BGSSaveLoadManager::GetSingleton (403340) and its
 *         +0x120 byte (BGSSaveLoadManager.h:114, unk120); Main::GetSingleton
 *         (403449) with resetGame (+0x11) and threadID (+0x28, Main.h:78,
 *         :87); TESDataHandler::GetSingleton and its clearingData
 *         (TESDataHandler.h:76-88 from the runtime block at 0xDA0)
 *
 * HYPOTHESIS (what only the lab proves; rule 2):
 *   H1 the thread. The Frida scripts ran the recipe on Main.threadID because
 *      their trigger (a kill's mark) fired on the engine's death-path threads
 *      with the main thread live. A TESModPlatform native runs on Skyrim
 *      Platform's OnUpdate thread (the memory note skyrim-platform-thread-
 *      shape: a Papyrus native's thread, with the main thread parked in the
 *      VM's update), the thread on which the fork already drops and rebuilds
 *      an actor's 3D (FateApi rerollActor's 37323, npc-fate at T3). So the
 *      Main.threadID premise is REPORTED (thread, mainThread, onMainThread in
 *      every result) and not enforced; kRequireMainThread flips it. The
 *      first lab call decides: a refusal there means the deferral design
 *      (a main-thread task and a result event) is the next slice.
 *   H2 the census hook sees the recipe's own writes. frida-gum bypasses every
 *      hook entered while one of its listeners runs on the thread (the
 *      scripts' guard section); a native called from JS through callNative
 *      stands in no Gum listener, so the replacement fires. If it did not,
 *      the census would read zero rows while the header write (36037) always
 *      makes one: Serialize then refuses with "census blind" rather than
 *      send a record whose type-0 ids nobody can remap.
 *   H3 the census offset. 36047 and 36048 call 35928 before appending the
 *      three bytes, so the buffer position at the call is the landing
 *      offset (the scripts' note; run 210713's REMAP at offset 2097 held).
 *   H4 the entry's Value is 16 bytes {flags, u32, record} (the note's Q3,
 *      change-forms.md D5; every transplant script used it; no dump yet).
 *   H5 the premise offsets +0xDA5 (clearingData) and +0x120 (the dry-run
 *      byte) name what the static read says; both read zero in every run,
 *      which a wrong offset would also show.
 *   H6 35928's insert into this seat's form-id map on a miss (the REMAP
 *      encode and a live serialize) is safe outside a save: the engine's
 *      save path does it for every referenced form.
 *   H7 the apply on a LOADED copy from this thread: run 193005 (a fixed base,
 *      the 3D kept) and 174905 (a leveled actor, the 3D released inside 35568
 *      and rebuilt, the record's block parked by the finish pass) ran it on
 *      Main.threadID; here it runs on H1's thread.
 *   H8 the Havok block's framing (kind 4/5/6 fixed part 27/31/34, a varint
 *      length, the 19534 block) read from the decompile; used here only to
 *      refuse a remap row that lands inside the block and for diagnostics.
 *
 * The premises (any failure refuses, the failed names in the log and, for
 * the apply, in the result): BGSSaveLoadGame and BGSSaveLoadManager present;
 * kLoading and kSaving clear (the apply: kInitingForms too); the dry-run byte
 * zero; clearingData zero; resetGame zero; the form an ACHR, not kTemporary,
 * not kWantsDelete or kRefPermanentlyDeleted, not the player; the far skin
 * absent; slot 0x0D 19527 and slot 0x0E 37649 or 40266 (the apply: slot 0x0F
 * 37650 or 40267, the 3D present, the actor dead, the pending array empty);
 * the entry present with a null record (an engine record is never touched:
 * the owner sends that one as is, D7's rule). The apply adds: the record
 * parses as ACHR, raw (length2 0), the engine's version, the lengths
 * consistent; every REMAP row decodes to kind 0 with its index and names a
 * form this seat has; no row inside the Havok block. Both natives write
 * nothing into the engine before every check has passed.
 */
namespace {

// The thread premise's switch (H1). false: reported; true: a refusal.
constexpr bool kRequireMainThread = false;
// the wire's body cap (docs/verbs/change-form.md, the message contract)
constexpr std::size_t kRecordCap = 65536;
// census rows kept per recipe (the scripts' CENSUS_CAP was 400 emitted rows;
// an actor record carries about 180 ids)
constexpr std::size_t kCensusCap = 4096;
// the wire's remap cap
constexpr std::size_t kRemapCap = 256;

// The changes map's Value (H4): flags, a u32 the scripts call mid, the
// record pointer
struct ChangeValue
{
  std::uint32_t flags = 0;
  std::uint32_t mid = 0;
  void* record = nullptr;
};
static_assert(sizeof(ChangeValue) == 0x10);

// BGSSaveFormBuffer (BGSSaveFormBuffer.h:7-25, 0x30 bytes): the data at
// +0x08, the position at +0x14, the form at +0x28 (CommonLibSSE-NG's members
// buffer, bufferPosition and form). The nine-byte header inside it (the
// note's Q1 layout): the id at +0x18 (3 bytes), the flags at +0x1b, the type
// at +0x1f, the version at +0x20; CommonLibSSE-NG's unk18 and unk20 cover
// them, so the flags are read by offset.
static_assert(sizeof(RE::BGSSaveFormBuffer) == 0x30);
constexpr std::size_t kHeaderFlagsOffset = 0x1b;

// The engine functions by id (the signatures the Frida scripts proved in
// their runs, the note's Q4 "The sequence" and the (c) read's last row)
using BufCtorFn = RE::BGSSaveFormBuffer* (*)(RE::BGSSaveFormBuffer*);
using BufInitFn = void (*)(RE::BGSSaveFormBuffer*, std::uint32_t id,
                           std::uint32_t flags, std::uint32_t formType,
                           std::uint8_t version);
using BufDestroyFn = void (*)(RE::BGSSaveFormBuffer*);
using InitialDataFn = std::uint32_t (*)(RE::BGSSaveFormBuffer*, RE::TESForm*,
                                        std::uint32_t flags);
using PackFn = void (*)(void** out, RE::BGSSaveFormBuffer*);
using SaveVersionFn = std::uint8_t (*)();
using ChangesReadFn = bool (*)(RE::BGSSaveLoadChangesMap*, RE::FormID,
                               ChangeValue*);
using ChangesWriteFn = void (*)(RE::BGSSaveLoadChangesMap*, RE::FormID,
                                const ChangeValue*);
using ApplyLoadedFn = std::uint8_t (*)(RE::BGSSaveLoadGame*, RE::TESForm*);
using FinishPassFn = void (*)(RE::BGSSaveLoadGame*, std::uint8_t fromFullLoad);
using SetNumericIdFn = void (*)(RE::BGSNumericIDIndex*, RE::FormID);

struct Engine
{
  REL::Relocation<BufCtorFn> bufCtor{ ::Offsets::ChangeForm::BufferCtor };
  REL::Relocation<BufInitFn> bufInit{ ::Offsets::ChangeForm::BufferInit };
  REL::Relocation<BufDestroyFn> bufDestroy{
    ::Offsets::ChangeForm::BufferDestroy
  };
  REL::Relocation<InitialDataFn> initialData{
    ::Offsets::ChangeForm::InitialData
  };
  REL::Relocation<PackFn> pack{ ::Offsets::ChangeForm::Pack };
  REL::Relocation<SaveVersionFn> saveVersion{
    ::Offsets::ChangeForm::SaveVersion
  };
  REL::Relocation<ChangesReadFn> changesRead{
    ::Offsets::ChangeForm::ChangesRead
  };
  REL::Relocation<ChangesWriteFn> changesWrite{
    ::Offsets::ChangeForm::ChangesWrite
  };
  REL::Relocation<ApplyLoadedFn> applyLoaded{
    ::Offsets::ChangeForm::ApplyLoaded
  };
  REL::Relocation<FinishPassFn> finishPass{
    ::Offsets::ChangeForm::FinishPass
  };
  // the vtable premises' addresses
  REL::Relocation<std::uintptr_t> refrCheckSaveGame{
    ::Offsets::ChangeForm::RefrCheckSaveGame
  };
  REL::Relocation<std::uintptr_t> actorSaveGame{
    ::Offsets::ChangeForm::ActorSaveGame
  };
  REL::Relocation<std::uintptr_t> characterSaveGame{
    ::Offsets::ChangeForm::CharacterSaveGame
  };
  REL::Relocation<std::uintptr_t> actorLoadGame{
    ::Offsets::ChangeForm::ActorLoadGame
  };
  REL::Relocation<std::uintptr_t> characterLoadGame{
    ::Offsets::ChangeForm::CharacterLoadGame
  };
};

const Engine& Eng()
{
  static const Engine e;
  return e;
}

std::uint32_t Fnv1a(const std::uint8_t* data, std::size_t size)
{
  std::uint32_t h = 0x811c9dc5u;
  for (std::size_t i = 0; i < size; ++i) {
    h ^= data[i];
    h *= 0x01000193u;
  }
  return h;
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

int HexDigit(char c)
{
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

bool FromHex(std::string_view hex, std::vector<std::uint8_t>& out)
{
  if (hex.size() % 2 != 0) {
    return false;
  }
  out.clear();
  out.reserve(hex.size() / 2);
  for (std::size_t i = 0; i < hex.size(); i += 2) {
    const int hi = HexDigit(hex[i]);
    const int lo = HexDigit(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out.push_back(static_cast<std::uint8_t>((hi << 4) | lo));
  }
  return true;
}

std::string Hex32(std::uint32_t v)
{
  return fmt::format("{:#x}", v);
}

std::string PtrStr(const void* p)
{
  return p ? fmt::format("{:#x}", reinterpret_cast<std::uintptr_t>(p))
           : std::string("null");
}

std::string RvaStr(std::uintptr_t address)
{
  const auto base = REL::Module::get().base();
  return address >= base ? fmt::format("{:#x}", address - base)
                         : fmt::format("outside the module: {:#x}", address);
}

std::uint32_t ThreadId()
{
  return static_cast<std::uint32_t>(::GetCurrentThreadId());
}

std::uint32_t HeaderFlagsOf(const RE::BGSSaveFormBuffer* buf)
{
  std::uint32_t v = 0;
  std::memcpy(&v,
              reinterpret_cast<const std::uint8_t*>(buf) + kHeaderFlagsOffset,
              sizeof(v));
  return v;
}

// A record as the pack (36081) frames it: byte 0 the save-type index (low six
// bits) and the size code (top two), byte 1 the version, two lengths of the
// code's width (1, 2 or 4 bytes), the data (the note's Q3 "Evidence (the
// entry)"; length2 nonzero means deflated)
struct RecordHeader
{
  bool ok = false;
  std::uint8_t typeIndex = 0;
  std::uint8_t sizeCode = 0;
  std::uint8_t version = 0;
  std::uint32_t length1 = 0;
  std::uint32_t length2 = 0;
  std::size_t bodyStart = 0;
  std::size_t total = 0;
};

RecordHeader ParseRecordHeader(const std::uint8_t* b, std::size_t avail)
{
  RecordHeader h;
  if (avail < 2) {
    return h;
  }
  const std::uint8_t b0 = b[0];
  const std::uint8_t s = static_cast<std::uint8_t>(b0 >> 6);
  if (s == 3) {
    return h;
  }
  const std::size_t n = s == 0 ? 1 : (s == 1 ? 2 : 4);
  const std::size_t need = 2 + 2 * n;
  if (avail < need) {
    return h;
  }
  auto le = [&](std::size_t o) {
    std::uint32_t v = 0;
    for (std::size_t k = n; k > 0; --k) {
      v = (v << 8) | b[o + k - 1];
    }
    return v;
  };
  h.typeIndex = static_cast<std::uint8_t>(b0 & 0x3f);
  h.sizeCode = s;
  h.version = b[1];
  h.length1 = le(2);
  h.length2 = le(2 + n);
  h.bodyStart = need;
  h.total = need + h.length1;
  h.ok = true;
  return h;
}

// 36052's variable-width integer: value << 2 | width, one byte under 0x40,
// two under 0x4000, four otherwise (the note's Q1)
bool ReadVarint(const std::uint8_t* b, std::size_t len, std::size_t o,
                std::uint32_t& value, std::size_t& width)
{
  if (o >= len) {
    return false;
  }
  const std::uint8_t w = static_cast<std::uint8_t>(b[o] & 3);
  if (w == 0) {
    value = static_cast<std::uint32_t>(b[o] >> 2);
    width = 1;
    return true;
  }
  if (w == 1) {
    if (o + 1 >= len) {
      return false;
    }
    value = static_cast<std::uint32_t>(((b[o + 1] << 8) | b[o]) >> 2);
    width = 2;
    return true;
  }
  if (o + 3 >= len) {
    return false;
  }
  const std::uint32_t raw = static_cast<std::uint32_t>(b[o]) |
    (static_cast<std::uint32_t>(b[o + 1]) << 8) |
    (static_cast<std::uint32_t>(b[o + 2]) << 16) |
    (static_cast<std::uint32_t>(b[o + 3]) << 24);
  value = raw >> 2;
  width = 4;
  return true;
}

// The initial data's Havok block (H8): the kind from the flags and the id
// (35708: created 5, kCellChanged or kPromoted 6, kMoved or kHavokMoved 4),
// the fixed part 27/31/34 bytes, then, when the flags carry kHavokMoved, a
// varint length and the 19534 block (u8 flags, varint body count, ...)
struct HavokInfo
{
  std::uint32_t kind = 0;
  std::size_t fixed = 0;
  bool present = false;
  std::size_t blockStart = 0;
  std::size_t blockLen = 0;
  std::uint32_t count = 0;
  std::string error;
};

HavokInfo ParseHavok(const std::uint8_t* body, std::size_t len,
                     std::uint32_t flags, RE::FormID id)
{
  HavokInfo r;
  r.kind = id >= 0xff000000u
    ? 5
    : ((flags & 0x02000008u) != 0 ? 6 : ((flags & 6u) != 0 ? 4 : 0));
  r.fixed = r.kind == 4 ? 27 : (r.kind == 5 ? 31 : (r.kind == 6 ? 34 : 0));
  r.present = (flags & 4u) != 0 && r.kind != 0;
  if (!r.present) {
    return r;
  }
  std::uint32_t blockLen = 0;
  std::size_t width = 0;
  if (!ReadVarint(body, len, r.fixed, blockLen, width)) {
    r.error = "body too short for the block";
    return r;
  }
  r.blockStart = r.fixed + width;
  r.blockLen = blockLen;
  if (blockLen < 2 || r.blockStart + blockLen > len) {
    r.error = fmt::format("block length {} does not fit the body", blockLen);
    return r;
  }
  std::uint32_t count = 0;
  std::size_t cw = 0;
  if (ReadVarint(body, len, r.blockStart + 1, count, cw)) {
    r.count = count;
  }
  return r;
}

// One premise, named as the Frida scripts name theirs
struct Check
{
  std::string name;
  std::string value;
  bool ok;
};

struct Premises
{
  bool ok = false;
  std::vector<Check> checks;
  RE::BGSSaveLoadGame* game = nullptr;
  RE::BGSSaveLoadChangesMap* map = nullptr;
  bool entryFound = false;
  ChangeValue entry;
  RE::Actor* actor = nullptr;
  bool has3D = false;
  std::uint32_t lifeState = 0xffffffffu;
  std::uint32_t pending = 0;
  std::uint32_t thread = 0;
  std::uint32_t mainThread = 0;
  bool onMainThread = false;
};

std::string FailedNames(const Premises& p)
{
  std::string out;
  for (const auto& c : p.checks) {
    if (!c.ok) {
      if (!out.empty()) {
        out += "; ";
      }
      out += c.name + " (" + c.value + ")";
    }
  }
  return out;
}

Premises CheckPremises(RE::TESObjectREFR* refr, bool forApply)
{
  Premises p;
  auto add = [&](std::string name, std::string value, bool ok) {
    p.checks.push_back(Check{ std::move(name), std::move(value), ok });
  };
  p.thread = ThreadId();

  auto game = RE::BGSSaveLoadGame::GetSingleton();
  auto manager = RE::BGSSaveLoadManager::GetSingleton();
  auto main = RE::Main::GetSingleton();
  auto handler = RE::TESDataHandler::GetSingleton();
  p.game = game;

  add("BGSSaveLoadGame", PtrStr(game), game != nullptr);
  add("BGSSaveLoadManager", PtrStr(manager), manager != nullptr);
  const std::uint32_t mflags = game ? game->flags.underlying() : 0xffffffffu;
  const std::uint32_t mask = forApply ? 0xEu : 0x6u;
  add(forApply ? "kLoading, kSaving, kInitingForms clear"
               : "kLoading, kSaving clear",
      Hex32(mflags), game != nullptr && (mflags & mask) == 0);
  // the dry-run byte (BGSSaveLoadManager.h:114 unk120's low byte, H5)
  const int dry =
    manager ? *reinterpret_cast<const std::uint8_t*>(&manager->unk120) : -1;
  add("dry-run byte zero (+0x120)", std::to_string(dry), dry == 0);
  const int clearing =
    handler ? (handler->GetGeometryRuntimeData().clearingData ? 1 : 0) : -1;
  add("clearingData zero (+0xDA5, HYPOTHESIS)", std::to_string(clearing),
      clearing == 0);
  const int reset = main ? (main->resetGame ? 1 : 0) : -1;
  add("resetGame zero (+0x11)", std::to_string(reset), reset == 0);
  p.mainThread = main ? main->threadID : 0;
  p.onMainThread = main != nullptr && main->threadID == p.thread;
  add(kRequireMainThread ? "on Main.threadID"
                         : "on Main.threadID (reported, H1: not enforced)",
      fmt::format("{} vs {}", p.mainThread, p.thread),
      !kRequireMainThread || p.onMainThread);

  add("reference", refr ? Hex32(refr->formID) : std::string("null"),
      refr != nullptr);
  if (!refr) {
    p.ok = false;
    return p;
  }

  const std::uint32_t fflags = refr->formFlags;
  add("not kTemporary (bit 14)", Hex32(fflags), ((fflags >> 14) & 1u) == 0);
  const std::uint32_t igf = refr->inGameFormFlags.underlying();
  add("inGameFormFlags bits 0, 7 clear", Hex32(igf), (igf & 0x81u) == 0);
  const bool isAchr = refr->GetFormType() == RE::FormType::ActorCharacter;
  add("form type ACHR 0x3E",
      Hex32(static_cast<std::uint32_t>(refr->GetFormType())), isAchr);
  auto player = RE::PlayerCharacter::GetSingleton();
  add("not the player (id 0x14, the singleton)",
      fmt::format("{} vs {}", Hex32(refr->formID), PtrStr(player)),
      refr->formID != 0x14 && player != nullptr &&
        static_cast<RE::TESObjectREFR*>(player) != refr);
  p.actor = isAchr ? refr->As<RE::Actor>() : nullptr;

  // the far skin (HighProcessData.h:357 farGeometry; Actor.h:651
  // currentProcess behind the runtime-data accessor): a set flag makes 0x0E
  // strip it by RemoveItem and mark kInventory (the actor section Q2)
  std::string farSkin = "not an actor";
  bool farSkinOk = false;
  if (p.actor) {
    auto process = p.actor->GetActorRuntimeData().currentProcess;
    auto high = process ? process->high : nullptr;
    if (!high) {
      farSkin = "no high process";
      farSkinOk = true;
    } else {
      farSkin = std::to_string(high->farGeometry ? 1 : 0);
      farSkinOk = !high->farGeometry;
    }
  }
  add("far skin absent (a set farGeometry would make 0x0E strip it and mark "
      "kInventory)",
      farSkin, farSkinOk);

  // the vtable premises, by address (TESForm.h:136-138 slots 0x0D to 0x0F)
  const auto& e = Eng();
  const auto vtbl = *reinterpret_cast<std::uintptr_t* const*>(refr);
  const std::uintptr_t slotC = vtbl[0x0D];
  const std::uintptr_t slotS = vtbl[0x0E];
  const std::uintptr_t slotL = vtbl[0x0F];
  add("slot 0x0D is 19527", RvaStr(slotC),
      slotC == e.refrCheckSaveGame.address());
  add("slot 0x0E is Actor::SaveGame 37649 or Character's 40266", RvaStr(slotS),
      slotS == e.actorSaveGame.address() ||
        slotS == e.characterSaveGame.address());
  if (forApply) {
    add("slot 0x0F is Actor::LoadGame 37650 or Character's 40267",
        RvaStr(slotL),
        slotL == e.actorLoadGame.address() ||
          slotL == e.characterLoadGame.address());
  }

  p.has3D = refr->Get3D2() != nullptr;
  if (forApply) {
    add("3D present (Get3D2)", p.has3D ? "yes" : "null", p.has3D);
  }
  if (p.actor) {
    p.lifeState =
      static_cast<std::uint32_t>(p.actor->AsActorState()->GetLifeState());
  }
  if (forApply) {
    add("actor dead (lifeState 2)", std::to_string(p.lifeState),
        p.actor != nullptr &&
          p.lifeState ==
            static_cast<std::uint32_t>(RE::ACTOR_LIFE_STATE::kDead));
  }
  p.pending = game ? static_cast<std::uint32_t>(game->unk318.size()) : 0;
  if (forApply) {
    add("pending array empty (+0x328)", std::to_string(p.pending),
        game != nullptr && p.pending == 0);
  }

  // the entry (35503 on the changes map at +0x330)
  p.map = game ? game->saveLoadChanges : nullptr;
  if (p.map) {
    ChangeValue v;
    p.entryFound = e.changesRead(p.map, refr->formID, &v);
    p.entry = v;
  }
  add("entry present", p.entryFound ? Hex32(p.entry.flags) : "none",
      p.entryFound);
  add("entry record null (an engine record is never touched)",
      PtrStr(p.entry.record), p.entryFound && p.entry.record == nullptr);

  p.ok = true;
  for (const auto& c : p.checks) {
    if (!c.ok) {
      p.ok = false;
      break;
    }
  }
  return p;
}

// The id census (D6): one row per BGSNumericIDIndex::SetNumericID call on the
// recipe's thread while a serialize runs, read back from the three bytes the
// engine wrote (the top two bits the kind: 1 a load-index-00 form, 2 a
// created form, 0 an index into this seat's form-id map, null at index 0)
struct CensusRow
{
  std::uint8_t phase;   // 1 the header (36037), 2 the initial data, 3 the body
  std::uint32_t offset; // the buffer position at the call (H3)
  RE::FormID id;
  std::uint8_t kind;
  std::uint32_t value;
};

struct Census
{
  RE::BGSSaveFormBuffer* buf = nullptr;
  std::uint8_t phase = 0;
  std::vector<CensusRow> rows;
  std::uint32_t dropped = 0;
};

thread_local Census* tl_census = nullptr;
SetNumericIdFn g_setNumericIdOriginal = nullptr;

void SetNumericIdReplacement(RE::BGSNumericIDIndex* self, RE::FormID id)
{
  g_setNumericIdOriginal(self, id);
  auto census = tl_census;
  if (!census || !self || !census->buf) {
    return;
  }
  if (census->rows.size() >= kCensusCap) {
    census->dropped += 1;
    return;
  }
  const std::uint8_t b0 = self->data[0];
  const std::uint8_t b1 = self->data[1];
  const std::uint8_t b2 = self->data[2];
  CensusRow row;
  row.phase = census->phase;
  row.offset = census->buf->bufferPosition;
  row.id = id;
  row.kind = static_cast<std::uint8_t>(b0 >> 6);
  row.value = (static_cast<std::uint32_t>(b0 & 0x3f) << 16) |
    (static_cast<std::uint32_t>(b1) << 8) | static_cast<std::uint32_t>(b2);
  census->rows.push_back(row);
}

struct CensusGuard
{
  explicit CensusGuard(Census* c) { tl_census = c; }
  ~CensusGuard() { tl_census = nullptr; }
  CensusGuard(const CensusGuard&) = delete;
  CensusGuard& operator=(const CensusGuard&) = delete;
};

// One REMAP row (apply-on-demand.js v2.3's parseRemap): offset the RECORD
// offset, formId the sender's full runtime id, index the sender's 22-bit
// index, formType optional
struct RemapRow
{
  std::size_t offset = 0;
  RE::FormID formId = 0;
  std::uint32_t index = 0;
  int formType = -1;
  std::uint32_t fromIndex = 0;
  std::uint32_t toIndex = 0;
};

std::string Trim(std::string_view s)
{
  std::size_t a = 0;
  std::size_t b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) {
    ++a;
  }
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) {
    --b;
  }
  return std::string(s.substr(a, b - a));
}

bool ParseNumber(const std::string& s, std::uint64_t& out)
{
  if (s.empty() || s[0] == '-') {
    return false;
  }
  // decimal or 0x-prefixed hex only: strtoull's base 0 would read a leading
  // zero as octal ("010" as 8) where the script's num() read 10
  const int base =
    (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) ? 16 : 10;
  char* end = nullptr;
  errno = 0;
  const unsigned long long v = std::strtoull(s.c_str(), &end, base);
  if (errno != 0 || end == s.c_str() || *end != '\0') {
    return false;
  }
  out = v;
  return true;
}

std::vector<std::string> Split(std::string_view text, char sep)
{
  std::vector<std::string> parts;
  std::size_t start = 0;
  while (true) {
    const std::size_t at = text.find(sep, start);
    if (at == std::string_view::npos) {
      parts.push_back(Trim(text.substr(start)));
      break;
    }
    parts.push_back(Trim(text.substr(start, at - start)));
    start = at + 1;
  }
  return parts;
}

// the rows' shape; their bytes and forms are checked in the apply
std::string ParseRemap(std::string_view text, std::vector<RemapRow>& rows)
{
  rows.clear();
  const std::string trimmed = Trim(text);
  if (trimmed.empty()) {
    return "";
  }
  std::string problems;
  auto problem = [&](const std::string& what) {
    if (!problems.empty()) {
      problems += "; ";
    }
    problems += what;
  };
  const auto parts = Split(trimmed, ',');
  for (std::size_t i = 0; i < parts.size(); ++i) {
    const std::string where = fmt::format("row {} \"{}\"", i + 1, parts[i]);
    if (parts[i].empty()) {
      problem(where + " is empty");
      continue;
    }
    const auto f = Split(parts[i], ':');
    if (f.size() != 3 && f.size() != 4) {
      problem(fmt::format(
        "{} has {} fields, offset:formId:index[:formType] expected", where,
        f.size()));
      continue;
    }
    std::uint64_t offset = 0;
    std::uint64_t formId = 0;
    std::uint64_t index = 0;
    std::uint64_t formType = 0;
    if (!ParseNumber(f[0], offset)) {
      problem(where + ": offset is not a non-negative integer");
    }
    if (!ParseNumber(f[1], formId) || formId == 0 || formId > 0xffffffffu) {
      problem(where + ": formId is not a form id");
    } else if ((formId >> 24) == 0 || (formId >> 24) == 0xff) {
      problem(fmt::format(
        "{}: form {:#x} encodes as kind {} on every seat (35928): a type-0 "
        "row cannot name it",
        where, formId, (formId >> 24) == 0 ? 1 : 2));
    }
    if (!ParseNumber(f[2], index) || index > 0x3fffffu) {
      problem(where + ": index is not a 22-bit integer");
    }
    if (f.size() == 4 && (!ParseNumber(f[3], formType) || formType > 0xff)) {
      problem(where + ": formType is not a byte");
    }
    RemapRow row;
    row.offset = static_cast<std::size_t>(offset);
    row.formId = static_cast<RE::FormID>(formId);
    row.index = static_cast<std::uint32_t>(index);
    row.formType = f.size() == 4 ? static_cast<int>(formType) : -1;
    rows.push_back(row);
  }
  for (std::size_t a = 0; a < rows.size(); ++a) {
    for (std::size_t b = a + 1; b < rows.size(); ++b) {
      const std::size_t lo =
        rows[a].offset < rows[b].offset ? rows[a].offset : rows[b].offset;
      const std::size_t hi =
        rows[a].offset < rows[b].offset ? rows[b].offset : rows[a].offset;
      if (hi - lo < 3) {
        problem(fmt::format("rows {} and {} overlap at offset {}", a + 1,
                            b + 1, rows[a].offset));
      }
    }
  }
  if (rows.size() > kRemapCap) {
    problem(fmt::format("{} rows, the cap is {}", rows.size(), kRemapCap));
  }
  return problems;
}

// at info for this slice (one call per corpse, about twenty rows): the seat
// runs at info by default (main.cpp's LogLevel) and the first lab runs read
// the premise rows from the log, the refusal line carrying the failed ones
void LogChecks(const char* native, RE::FormID formId, const Premises& p)
{
  for (const auto& c : p.checks) {
    logger::info("[verb:change-form] {} {:#x} check {}: {} {}", native, formId,
                 c.name, c.value, c.ok ? "ok" : "FAILED");
  }
}

// What the address proof line reads: each id, its RVA and first four bytes
// against the unpacked 1.6.1170 image's (pyghidra read_bytes on sky-re,
// 2026-11-03; HYPOTHESIS by rule 2, the running game decides). A mismatch is
// logged, never a refusal: another hook on the prologue would show as one.
struct Proof
{
  const char* name;
  std::uint64_t id;
  const char* expected; // eight hex digits, or nullptr for a data symbol
};

constexpr Proof kProof[] = {
  { "36035 BGSSaveFormBuffer ctor", 36035, "40534883" },
  { "36037 header init", 36037, "48895c24" },
  { "36044 buffer destroy", 36044, "40574883" },
  { "35715 initial data", 35715, "48894c24" },
  { "36081 pack", 36081, "40555657" },
  { "35740 save version", 35740, "0fb605c5" },
  { "35503 changes read", 35503, "89542410" },
  { "35509 changes write", 35509, "89542410" },
  { "35568 apply wrapper", 35568, "40574881" },
  { "35567 finish pass", 35567, "488bc455" },
  { "35928 BGSNumericIDIndex::SetNumericID", 35928, "40534883" },
  { "19527 TESObjectREFR::CheckSaveGame", 19527, "48895c24" },
  { "37649 Actor::SaveGame", 37649, "40555356" },
  { "40266 Character::SaveGame", 40266, "e9cbe9f5" },
  { "37650 Actor::LoadGame", 37650, "40555356" },
  { "40267 Character::LoadGame", 40267, "e98bf0f5" },
  { "11141 MemoryManager::GetSingleton", 11141, "40534883" },
  { "68115 MemoryManager::Allocate", 68115, "48895c24" },
  { "68117 MemoryManager::Deallocate", 68117, "4885d20f" },
  { "403330 BGSSaveLoadGame**", 403330, nullptr },
  { "403340 BGSSaveLoadManager**", 403340, nullptr },
  { "403449 Main**", 403449, nullptr },
};

bool g_censusInstalled = false;
}

void ChangeForm::InstallHooks()
{
  if (!REL::Module::IsAE()) {
    logger::info("[verb:change-form] not an AE runtime: the natives refuse");
    return;
  }
  // the proof line, read before the replacement patches 35928's prologue
  const auto base = REL::Module::get().base();
  std::size_t matched = 0;
  std::size_t mismatched = 0;
  for (const auto& row : kProof) {
    const std::uintptr_t address = REL::ID(row.id).address();
    if (!row.expected) {
      logger::info("[verb:change-form] address {}: rva {:#x} (data)", row.name,
                   address - base);
      continue;
    }
    std::uint8_t bytes[4] = { 0, 0, 0, 0 };
    std::memcpy(bytes, reinterpret_cast<const void*>(address), sizeof(bytes));
    const std::string got = ToHex(bytes, sizeof(bytes));
    const bool same = got == row.expected;
    if (same) {
      matched += 1;
    } else {
      mismatched += 1;
    }
    const std::string verdict = same
      ? std::string("ok")
      : fmt::format("MISMATCH (image {})", row.expected);
    logger::info("[verb:change-form] address {}: rva {:#x} bytes {} {}",
                 row.name, address - base, got, verdict);
  }
  logger::info("[verb:change-form] addresses: {} resolved, {} matched, {} "
               "mismatched the unpacked image's first bytes",
               std::size(kProof), matched, mismatched);

  // the census: a Gum replacement of 35928 that forwards every call and
  // records the ones made while a serialize runs on the calling thread
  g_censusInstalled = Frida::HookHandler::GetSingleton()->Replace(
    ::Offsets::ChangeForm::SetNumericID.address(),
    reinterpret_cast<void*>(&SetNumericIdReplacement),
    reinterpret_cast<void**>(&g_setNumericIdOriginal));
  logger::info("[verb:change-form] census hook on 35928: {}",
               g_censusInstalled ? "installed" : "NOT installed");
  // the apply's thread (H1): every ApplyChangeForm runs its engine steps as
  // an SKSE task on the main thread; the interface is queried again at each
  // call, this line says what it was at install
  logger::info("[verb:change-form] apply deferral at install: SKSE task "
               "interface {}",
               SKSE::GetTaskInterface() ? "present" : "absent");
}

namespace {

// the body of Serialize; the public wrapper at the end catches what it throws
std::string SerializeImpl(RE::TESObjectREFR* refr)
{
  const auto t0 = std::chrono::steady_clock::now();
  const RE::FormID formId = refr ? refr->formID : 0;
  auto refuse = [&](const std::string& reason) {
    logger::info("[verb:change-form] SerializeChangeForm refused {:#x}: {}",
                 formId, reason);
    return std::string();
  };
  if (!REL::Module::IsAE()) {
    return refuse("not an AE runtime");
  }
  if (!g_censusInstalled || !g_setNumericIdOriginal) {
    return refuse("census hook not installed (35928): no remap rows possible");
  }
  Premises p = CheckPremises(refr, false);
  LogChecks("SerializeChangeForm", formId, p);
  if (!p.ok) {
    return refuse("premises: " + FailedNames(p));
  }

  const auto& e = Eng();
  // the scratch buffer: ours on the stack, the engine owns only the data at
  // +0x08 (36044 frees it)
  alignas(16) std::uint8_t raw[sizeof(RE::BGSSaveFormBuffer)];
  std::memset(raw, 0, sizeof(raw));
  auto buf = reinterpret_cast<RE::BGSSaveFormBuffer*>(raw);
  e.bufCtor(buf);

  Census census;
  census.buf = buf;
  census.phase = 1;
  census.rows.reserve(512); // an actor record carries about 180 ids
  nlohmann::json result = nlohmann::json::object();
  void* record = nullptr;
  std::vector<std::uint8_t> recordBytes; // the pack's bytes, read back below
  std::string refusal;
  {
    CensusGuard guard(&census);
    // the header: the entry's flags, the live form type, the engine's version
    e.bufInit(buf, formId, p.entry.flags,
              static_cast<std::uint32_t>(refr->GetFormType()),
              e.saveVersion());
    buf->form = refr;
    // slot 0x0D (19527): may clear header bits; 0 means nothing to save
    const bool keep = refr->CheckSaveGame(buf);
    if (!keep) {
      refusal = "nothing to save (0x0D returned 0; the entry is left alone)";
    } else {
      const std::uint32_t flags2 = HeaderFlagsOf(buf);
      census.phase = 2;
      const std::uint32_t kind = e.initialData(buf, refr, flags2);
      census.phase = 3;
      // slot 0x0E (37649 or 40266): the body
      refr->SaveGame(buf);
      const std::uint32_t plainLen = buf->bufferPosition;
      const auto plain = static_cast<const std::uint8_t*>(buf->buffer);
      const std::uint32_t plainFnv = plain ? Fnv1a(plain, plainLen) : 0;
      // the pack: the record on the engine heap, ours to free
      void* out[2] = { nullptr, nullptr };
      e.pack(out, buf);
      record = out[0];
      if (!record) {
        refusal = "the pack gave no record";
      } else {
        const auto rec = static_cast<const std::uint8_t*>(record);
        const RecordHeader h = ParseRecordHeader(rec, 10);
        if (!h.ok) {
          refusal = "the record's header does not parse";
        } else if (h.total > kRecordCap) {
          refusal = fmt::format("record of {} bytes over the cap {}", h.total,
                                kRecordCap);
        } else if (h.length2 != 0) {
          refusal = fmt::format(
            "the record is deflated (length2 {}): its offsets would name "
            "compressed bytes, no remap possible",
            h.length2);
        } else {
          result["formId"] = formId;
          result["formType"] = static_cast<std::uint32_t>(refr->GetFormType());
          result["recordType"] = h.typeIndex;
          result["sizeCode"] = h.sizeCode;
          result["flags"] = flags2;
          result["entryFlags"] = p.entry.flags;
          result["dropped"] = (p.entry.flags & ~flags2);
          result["version"] = h.version;
          result["kind"] = kind;
          result["bodyHex"] = ToHex(rec, h.total);
          result["bytes"] = h.total;
          result["fnv1a"] = Fnv1a(rec, h.total);
          result["plainBytes"] = plainLen;
          result["plainFnv1a"] = plainFnv;
          result["deflated"] = false;
          result["bodyStart"] = h.bodyStart;
          recordBytes.assign(rec, rec + h.total);
        }
      }
    }
  }
  // free the record (MemoryManager.h:43) and the buffer's data (36044),
  // whatever happened above
  if (record) {
    RE::MemoryManager::GetSingleton()->Deallocate(record, false);
  }
  e.bufDestroy(buf);

  if (!refusal.empty()) {
    return refuse(refusal);
  }
  // the census: blind means the hook did not fire for our own calls (H2)
  if (census.rows.empty()) {
    return refuse("census blind: 35928's hook saw none of the recipe's writes "
                  "(the header write alone makes one row)");
  }
  nlohmann::json remap = nlohmann::json::array();
  std::uint32_t byKind[4] = { 0, 0, 0, 0 };
  std::uint32_t nulls = 0;
  int headerKind = -1;
  bool truncated = false;
  const std::size_t bodyStart = result["bodyStart"].get<std::size_t>();
  for (const auto& row : census.rows) {
    byKind[row.kind] += 1;
    if (row.phase == 1) {
      headerKind = row.kind;
      continue;
    }
    if (row.kind == 0 && row.id == 0 && row.value == 0) {
      nulls += 1;
      continue;
    }
    if (row.kind != 0 || row.id == 0) {
      continue;
    }
    // the read-back (H3): a varint placeholder patched after an id write
    // grows by a byte and 36057 shifts the tail, so a row written between
    // the placeholder and its patch carries an offset one short; the
    // record's own bytes there must decode to kind 0 with the census's
    // index, else the record would never land (the server's
    // E_CHANGEFORM_REMAP or the receiver's check) with the owner none the
    // wiser: refused here, the reason in the owner's log
    const std::size_t at = bodyStart + row.offset;
    const bool inRecord = at + 3 <= recordBytes.size();
    const std::uint32_t back = inRecord
      ? ((static_cast<std::uint32_t>(recordBytes[at] & 0x3f) << 16) |
         (static_cast<std::uint32_t>(recordBytes[at + 1]) << 8) |
         static_cast<std::uint32_t>(recordBytes[at + 2]))
      : 0xffffffffu;
    if (!inRecord || (recordBytes[at] >> 6) != 0 || back != row.value) {
      return refuse(fmt::format(
        "remap row {:#x} at record offset {} reads back kind {} index {}, "
        "census said index {} (a varint patch shifted the tail, H3)",
        row.id, at, inRecord ? recordBytes[at] >> 6 : 9, back, row.value));
    }
    if (remap.size() >= kRemapCap) {
      truncated = true;
      continue;
    }
    nlohmann::json r = nlohmann::json::object();
    r["offset"] = at;
    r["formId"] = row.id;
    r["index"] = row.value;
    remap.push_back(r);
  }
  result.erase("bodyStart");
  result["remap"] = remap;
  result["remapTruncated"] = truncated;
  nlohmann::json c = nlohmann::json::object();
  c["rows"] = census.rows.size();
  c["type0"] = byKind[0];
  c["type1"] = byKind[1];
  c["type2"] = byKind[2];
  c["type3"] = byKind[3];
  c["nulls"] = nulls;
  c["headerKind"] = headerKind;
  c["dropped"] = census.dropped;
  result["census"] = c;

  // the entry after: untouched means flags unchanged and the record still
  // null (the scripts' entryUntouched)
  ChangeValue after;
  const bool found = e.changesRead(p.map, formId, &after);
  result["entryUntouched"] =
    found && after.flags == p.entry.flags && after.record == nullptr;
  result["thread"] = p.thread;
  result["mainThread"] = p.mainThread;
  result["onMainThread"] = p.onMainThread;
  result["ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
                   .count();
  logger::info(
    "[verb:change-form] SerializeChangeForm {:#x}: {} bytes fnv "
    "{:#x} flags {:#x} kind {} remap rows {} census {} ids (type0 "
    "{}, type2 {}) thread {} main {}",
    formId, result["bytes"].get<std::size_t>(),
    result["fnv1a"].get<std::uint32_t>(), result["flags"].get<std::uint32_t>(),
    result["kind"].get<std::uint32_t>(), remap.size(), census.rows.size(),
    byKind[0], byKind[2], p.thread, p.mainThread);
  return result.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

// the body of Apply; the public wrapper at the end catches what it throws
std::string ApplyImpl(RE::TESObjectREFR* refr, std::string_view bodyHex,
                      std::uint32_t flags, std::uint32_t version,
                      std::string_view remapRows)
{
  const auto t0 = std::chrono::steady_clock::now();
  const RE::FormID formId = refr ? refr->formID : 0;
  nlohmann::json result = nlohmann::json::object();
  result["applied"] = false;
  result["reason"] = "";
  result["fnvPatched"] = 0;
  result["formId"] = formId;
  auto finish = [&]() {
    result["ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0)
                     .count();
    return result.dump(-1, ' ', false,
                       nlohmann::json::error_handler_t::replace);
  };
  auto refuse = [&](const std::string& step, const std::string& reason) {
    result["reason"] = "at " + step + ": " + reason;
    logger::info("[verb:change-form] ApplyChangeForm refused {:#x} at {}: {}",
                 formId, step, reason);
    return finish();
  };
  if (!REL::Module::IsAE()) {
    return refuse("runtime", "not an AE runtime");
  }

  // the REMAP rows' shape first (a malformed row refuses before any read)
  std::vector<RemapRow> rows;
  const std::string shape = ParseRemap(remapRows, rows);
  if (!shape.empty()) {
    return refuse("remap", shape);
  }
  result["remapRows"] = rows.size();

  Premises p = CheckPremises(refr, true);
  LogChecks("ApplyChangeForm", formId, p);
  result["thread"] = p.thread;
  result["mainThread"] = p.mainThread;
  result["onMainThread"] = p.onMainThread;
  if (!p.ok) {
    return refuse("premises", FailedNames(p));
  }
  result["rootBefore"] = p.has3D;
  result["lifeStateBefore"] = p.lifeState;

  // the record
  std::vector<std::uint8_t> bytes;
  if (!FromHex(bodyHex, bytes)) {
    return refuse("RECORD", "bodyHex is not an even run of hex digits");
  }
  if (bytes.empty() || bytes.size() > kRecordCap) {
    return refuse(
      "RECORD",
      fmt::format("{} bytes, the cap is {}", bytes.size(), kRecordCap));
  }
  const auto& e = Eng();
  const RecordHeader h = ParseRecordHeader(bytes.data(), bytes.size());
  if (!h.ok) {
    return refuse("RECORD", "the record's header does not parse");
  }
  const std::uint8_t engineVersion = e.saveVersion();
  std::string problems;
  auto problem = [&](const std::string& what) {
    if (!problems.empty()) {
      problems += "; ";
    }
    problems += what;
  };
  if (h.typeIndex != 1) {
    problem(fmt::format("type {} is not ACHR (1)", h.typeIndex));
  }
  if (h.version != engineVersion) {
    problem(fmt::format("version {}, the engine writes {}", h.version,
                        engineVersion));
  }
  if (version != h.version) {
    problem(fmt::format("version argument {} disagrees with the record's {}",
                        version, h.version));
  }
  if (h.total != bytes.size()) {
    problem(fmt::format("length1 {} disagrees with {} bytes", h.length1,
                        bytes.size()));
  }
  if (h.length2 != 0) {
    problem(fmt::format("deflated (length2 {}): refused", h.length2));
  }
  if (!problems.empty()) {
    return refuse("RECORD", problems);
  }
  result["record"] = { { "type", h.typeIndex },
                       { "sizeCode", h.sizeCode },
                       { "version", h.version },
                       { "length1", h.length1 },
                       { "length2", h.length2 },
                       { "total", h.total },
                       { "fnv1a", Fnv1a(bytes.data(), bytes.size()) } };

  // REMAP check: (a) the bytes, (b) the form here, for EVERY row before any
  // engine-state change
  for (auto& row : rows) {
    if (row.offset < h.bodyStart || row.offset + 3 > h.total) {
      return refuse("REMAP check",
                    fmt::format("offset {} is outside the record body "
                                "({}..{}); the census's offset is a body "
                                "offset, add the header's {}",
                                row.offset, h.bodyStart, h.total - 3,
                                h.bodyStart));
    }
    const std::uint8_t b0 = bytes[row.offset];
    const std::uint8_t kind = static_cast<std::uint8_t>(b0 >> 6);
    const std::uint32_t value = (static_cast<std::uint32_t>(b0 & 0x3f) << 16) |
      (static_cast<std::uint32_t>(bytes[row.offset + 1]) << 8) |
      static_cast<std::uint32_t>(bytes[row.offset + 2]);
    if (kind != 0) {
      return refuse(
        "REMAP check",
        fmt::format("bytes at {} decode to kind {} index {}, row says {}: "
                    "nothing to remap ({})",
                    row.offset, kind, value, row.index,
                    kind == 1 ? "a load-index-00 form, portable"
                              : (kind == 2 ? "a created form"
                                           : "kind 3 is aliasing or a wrong "
                                             "offset")));
    }
    if (value != row.index) {
      return refuse("REMAP check",
                    fmt::format("bytes at {} decode to kind 0 index {}, row "
                                "says {}",
                                row.offset, value, row.index));
    }
    row.fromIndex = value;
    auto form = RE::TESForm::LookupByID(row.formId);
    if (!form) {
      return refuse("REMAP check",
                    fmt::format("form {:#x} is not on this seat", row.formId));
    }
    if (form->formID != row.formId) {
      return refuse("REMAP check",
                    fmt::format("LookupByID returned a form with id {:#x} "
                                "for {:#x}",
                                form->formID, row.formId));
    }
    const int type = static_cast<int>(form->GetFormType());
    if (row.formType >= 0 && type != row.formType) {
      return refuse("REMAP check",
                    fmt::format("form {:#x} is type {:#x} on this seat, row "
                                "says {:#x}",
                                row.formId, type, row.formType));
    }
    row.formType = type;
  }

  // the Havok block (H8): a remap row inside it names no form id
  const HavokInfo havok = ParseHavok(
    bytes.data() + h.bodyStart, bytes.size() - h.bodyStart, flags, formId);
  result["havok"] = { { "present", havok.present },
                      { "kind", havok.kind },
                      { "count", havok.count },
                      { "blockStart", havok.blockStart },
                      { "blockLen", havok.blockLen } };
  if (!havok.error.empty()) {
    return refuse("body count", "Havok block: " + havok.error);
  }
  if (havok.present) {
    const std::size_t lo = h.bodyStart + havok.blockStart;
    const std::size_t hi = lo + havok.blockLen;
    for (const auto& row : rows) {
      if (row.offset + 3 > lo && row.offset < hi) {
        return refuse("REMAP check",
                      fmt::format("offset {} lies inside the Havok block "
                                  "({}..{}): no form id is written there",
                                  row.offset, lo, hi - 1));
      }
    }
  }

  // REMAP encode: the engine's own encoder per row on a scratch
  // (BGSNumericIDIndex.h:28), the three bytes patched into our copy
  nlohmann::json patched = nlohmann::json::array();
  for (auto& row : rows) {
    RE::BGSNumericIDIndex scratch;
    scratch.SetNumericID(row.formId);
    const std::uint8_t kind = static_cast<std::uint8_t>(scratch.data[0] >> 6);
    const std::uint32_t value =
      (static_cast<std::uint32_t>(scratch.data[0] & 0x3f) << 16) |
      (static_cast<std::uint32_t>(scratch.data[1]) << 8) |
      static_cast<std::uint32_t>(scratch.data[2]);
    if (kind != 0) {
      return refuse("REMAP encode 35928",
                    fmt::format("35928 encoded form {:#x} as kind {} ({}), "
                                "not a type-0 index",
                                row.formId, kind, ToHex(scratch.data, 3)));
    }
    bytes[row.offset] = scratch.data[0];
    bytes[row.offset + 1] = scratch.data[1];
    bytes[row.offset + 2] = scratch.data[2];
    row.toIndex = value;
    patched.push_back({ { "offset", row.offset },
                        { "formId", row.formId },
                        { "fromIndex", row.fromIndex },
                        { "toIndex", row.toIndex },
                        { "formType", row.formType } });
  }
  result["rows"] = patched;
  const std::uint32_t fnvPatched = Fnv1a(bytes.data(), bytes.size());

  // the heap copy (68115, the pack's own allocator), then THE engine-state
  // write (35509): the record pointer is the engine's from the apply on
  auto heap = RE::MemoryManager::GetSingleton();
  if (!heap) {
    return refuse("allocate 68115", "no MemoryManager");
  }
  void* rec = heap->Allocate(bytes.size(), 0, false);
  if (!rec) {
    return refuse("allocate 68115", "the engine heap gave no memory");
  }
  std::memcpy(rec, bytes.data(), bytes.size());

  ChangeValue write;
  write.flags = flags;
  write.mid = p.entry.mid;
  write.record = rec;
  e.changesWrite(p.map, formId, &write);
  ChangeValue written;
  const bool heldOurs =
    e.changesRead(p.map, formId, &written) && written.record == rec;
  if (!heldOurs) {
    heap->Deallocate(rec, false);
    return refuse("write 35509",
                  "the entry does not hold our record after the write");
  }
  result["fnvPatched"] = fnvPatched;
  result["written"] = { { "flags", written.flags }, { "mid", written.mid } };
  logger::info("[verb:change-form] ApplyChangeForm {:#x}: {} bytes fnv {:#x} "
               "flags {:#x} remap rows {} -> 35568",
               formId, bytes.size(), fnvPatched, flags, rows.size());

  // the apply: decode, the initial data, LoadGame, the pending array
  const std::uint8_t applyReturn = e.applyLoaded(p.game, refr);
  result["applyReturn"] = applyReturn;
  ChangeValue afterApply;
  const bool foundAfter = e.changesRead(p.map, formId, &afterApply);
  if (applyReturn == 0) {
    if (foundAfter && afterApply.record == rec) {
      ChangeValue restore;
      restore.flags = p.entry.flags;
      restore.mid = p.entry.mid;
      restore.record = nullptr;
      e.changesWrite(p.map, formId, &restore);
      heap->Deallocate(rec, false);
      result["restored"] = "the entry put back with its old flags and a null "
                           "record, our record freed";
    }
    return refuse("apply 35568",
                  "35568 returned 0 (no entry, no record, a type mismatch or "
                  "kLoading): read the FORMS log");
  }
  if (foundAfter && afterApply.record == rec) {
    // the engine took the record without consuming the pointer: nulled, not
    // freed (the script's dangling case)
    ChangeValue clear = afterApply;
    clear.record = nullptr;
    e.changesWrite(p.map, formId, &clear);
    result["dangling"] = "the entry still held our record after a 1: nulled, "
                         "not freed";
  }
  result["entryAfterApply"] = { { "found", foundAfter },
                                { "flags", afterApply.flags },
                                { "recordNull",
                                  afterApply.record == nullptr } };
  result["pendingAfterApply"] =
    static_cast<std::uint32_t>(p.game->unk318.size());
  result["rootAfterApply"] = refr->Get3D2() != nullptr;

  // the finish pass: InitLoadGame, the flush of the queued Havok block,
  // FinishLoadGame; bare, as the engine's promotion batch runs it
  e.finishPass(p.game, 0);
  ChangeValue afterFinish;
  const bool foundFinish = e.changesRead(p.map, formId, &afterFinish);
  result["entryAfterFinish"] = { { "found", foundFinish },
                                 { "flags", afterFinish.flags },
                                 { "recordNull",
                                   afterFinish.record == nullptr } };
  result["pendingAfterFinish"] =
    static_cast<std::uint32_t>(p.game->unk318.size());
  result["rootAfterFinish"] = refr->Get3D2() != nullptr;
  result["lifeStateAfter"] = p.actor
    ? static_cast<std::uint32_t>(p.actor->AsActorState()->GetLifeState())
    : 0xffffffffu;
  result["applied"] = true;
  logger::info(
    "[verb:change-form] ApplyChangeForm {:#x}: applied (35568 {}, "
    "pending {} then {}, root {} then {}, lifeState {} then {}) "
    "thread {} main {}",
    formId, applyReturn, result["pendingAfterApply"].get<std::uint32_t>(),
    result["pendingAfterFinish"].get<std::uint32_t>(),
    result["rootAfterApply"].get<bool>(),
    result["rootAfterFinish"].get<bool>(), p.lifeState,
    result["lifeStateAfter"].get<std::uint32_t>(), p.thread, p.mainThread);
  return finish();
}

} // namespace

// The public natives: the bodies above under a catch. The dispatch between
// callNative and TESModPlatform::Add has none (SkyrimPlatform.cpp's
// RunInIOContext calls the function bare and StartWorker is a bare
// ioContext.run()), so a throw out of a native lands in Add's catch with the
// worker's completion never set and the JS pool thread waits forever in
// PushToWorkerAndWait: a frozen seat. The realistic throws here are few
// (nlohmann's dump on a non-UTF-8 byte in a refusal text, bad_alloc); a
// throw inside the serialize's recipe window leaks the scratch buffer's data
// and the packed record (no RAII holder yet), which beats the freeze.
std::string ChangeForm::Serialize(RE::TESObjectREFR* refr)
{
  try {
    return SerializeImpl(refr);
  } catch (const std::exception& e) {
    logger::error("[verb:change-form] SerializeChangeForm {:#x} threw: {}",
                  refr ? refr->formID : 0u, e.what());
    return std::string();
  }
}

namespace {

// Playtest 44 (2026-11-03, thuum docs/verbs/change-form.md FALSIFIED further,
// Dynamic read 6): the apply deadlocked three times on the thread callNative
// runs a native on (Skyrim Platform's OnUpdate thread, H1), twice on a dying
// copy and once on a DEAD copy re-served at a reconnect, so the engine steps
// run where the engine loads its own change forms: the game's main thread,
// through SKSE's task interface (the fork's TickHandler idiom). The native
// queues and answers {queued}; the task's answer waits here for
// ApplyChangeFormResult, read once; one in-flight apply per form.
std::mutex g_applyMutex;
std::unordered_set<RE::FormID> g_applyInFlight;
std::unordered_map<RE::FormID, std::string> g_applyAnswers;

std::string ApplyRefusal(RE::FormID formId, const std::string& reason)
{
  nlohmann::json r = nlohmann::json::object();
  r["applied"] = false;
  r["queued"] = false;
  r["reason"] = reason;
  r["fnvPatched"] = 0;
  r["formId"] = formId;
  r["thread"] = ThreadId();
  const auto* main = RE::Main::GetSingleton();
  r["mainThread"] = main ? main->threadID : 0u;
  return r.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

} // namespace

std::string ChangeForm::Apply(RE::TESObjectREFR* refr,
                              std::string_view bodyHex, std::uint32_t flags,
                              std::uint32_t version,
                              std::string_view remapRows)
{
  const RE::FormID formId = refr ? refr->formID : 0;
  if (!refr) {
    return ApplyRefusal(formId, "at queue: no reference");
  }
  const auto* tasks = SKSE::GetTaskInterface();
  if (!tasks) {
    return ApplyRefusal(formId, "at queue: no SKSE task interface");
  }
  {
    std::lock_guard<std::mutex> lock(g_applyMutex);
    if (g_applyInFlight.count(formId) != 0) {
      return ApplyRefusal(formId,
                          "at queue: an apply is in flight for this form");
    }
    if (g_applyAnswers.count(formId) != 0) {
      return ApplyRefusal(formId,
                          "at queue: an unread answer waits for this form "
                          "(ApplyChangeFormResult first)");
    }
    g_applyInFlight.insert(formId);
  }
  const std::uint32_t from = ThreadId();
  tasks->AddTask([formId, body = std::string(bodyHex), flags, version,
                  rows = std::string(remapRows), from]() {
    std::string answer;
    try {
      // the reference looked up on the thread that runs the apply
      // (FateApi's shape: a lookup under the forms map's lock belongs where
      // the engine takes that lock itself)
      auto* again = RE::TESForm::LookupByID<RE::TESObjectREFR>(formId);
      answer = again ? ApplyImpl(again, body, flags, version, rows)
                     : ApplyRefusal(formId, "at task: the reference is gone");
    } catch (const std::exception& e) {
      logger::error("[verb:change-form] ApplyChangeForm {:#x} threw: {}",
                    formId, e.what());
      answer = ApplyRefusal(formId, std::string("exception: ") + e.what());
    }
    const auto* main = RE::Main::GetSingleton();
    const std::uint32_t here = ThreadId();
    const bool onMain = main != nullptr && main->threadID == here;
    logger::info("[verb:change-form] ApplyChangeForm {:#x} answered on {} "
                 "(thread {} main {}, queued from {})",
                 formId, onMain ? "the main thread" : "another thread", here,
                 main ? main->threadID : 0u, from);
    std::lock_guard<std::mutex> lock(g_applyMutex);
    g_applyInFlight.erase(formId);
    g_applyAnswers[formId] = std::move(answer);
  });
  logger::info("[verb:change-form] ApplyChangeForm {:#x} queued to the main "
               "thread ({} hex chars, {} remap chars) from thread {}",
               formId, bodyHex.size(), remapRows.size(), from);
  nlohmann::json r = nlohmann::json::object();
  r["applied"] = false;
  r["queued"] = true;
  r["reason"] = "";
  r["fnvPatched"] = 0;
  r["formId"] = formId;
  r["thread"] = from;
  return r.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string ChangeForm::ApplyResult(RE::TESObjectREFR* refr)
{
  const RE::FormID formId = refr ? refr->formID : 0;
  std::lock_guard<std::mutex> lock(g_applyMutex);
  auto it = g_applyAnswers.find(formId);
  if (it == g_applyAnswers.end()) {
    return std::string();
  }
  std::string out = std::move(it->second);
  g_applyAnswers.erase(it);
  return out;
}
