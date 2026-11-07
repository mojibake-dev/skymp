#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "save_storages/AsyncSaveStorage.h"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

PartOne& GetPartOne();

// SaveStorageTest.cpp's helpers: a file-backed save storage under unit/data
// and a synchronous upsert into it (DropItemTest's declarations)
std::shared_ptr<
  Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>
MakeSaveStorageFile();
void UpsertSync(
  Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>& st,
  std::vector<std::optional<MpChangeForm>> changeForms);

// thuum ADR-068 rung 4 (the state rows, amendment B; docs/research/
// rung4-state-rows-2026-11-07.md section 1(c), RefState): the owner of a
// reference reports its script's Enable, EnableNoWait, Disable,
// DisableNoWait or Delete as a batched state row (ScriptCall 75); the server
// records the reference's engine state (isDisabled, isDeleted and
// refStateSeq, the object's sequence), relays it at once as DestructionStage
// 78's disabled and deleted words to the other listeners, serves it behind a
// create word, and attaches a reference the loader never held by its record.
// The stage is ScriptCallTest's, Bleak Falls Temple 0x371de: the portcullis
// 0x3b9f1 (an ACTI) at (4992, -1408, -768); and Skyrim.esm's records as
// lab/esm.py read them on sky-srv (2026-11-06; no later master overrides
// them): the XMarker 0x17037 (a REFR of the STAT 0x3b, record flags 0xc00,
// initially disabled) at (6867, -5874, -1687), which the encounter trigger
// 0x17038 enables, and the ambush draugr 0x380ba (an ACHR of the NPC_
// 0x38922, the same flags) at (8080, -7444, -2312), which 0x4e421's script
// enables (docs/research/trap-census-2026-11-05.md).
namespace {
using Arg = std::optional<
  std::variant<bool, double, std::string, SpSnippetObjectArgument>>;

constexpr uint32_t kHostActor = 0xff000c60;
constexpr uint32_t kGuestActor = 0xff000c61;
constexpr uint32_t kLateActor = 0xff000c62;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kPortcullis = 0x3b9f1;
constexpr uint32_t kXMarker = 0x17037;
constexpr uint32_t kAmbushDraugr = 0x380ba;
const NiPoint3 kByThePortcullis{ 4992.f, -1508.f, -768.f };
const NiPoint3 kByTheXMarker{ 6867.f, -5774.f, -1687.f };
const NiPoint3 kByTheDraugr{ 8080.f, -7344.f, -2312.f };

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

void Loaded(PartOne& p, Networking::UserId user, uint32_t cell, bool loaded)
{
  RawMessageData raw;
  raw.userId = user;
  CellLoadedMessage msg;
  msg.cell = cell;
  msg.loaded = loaded;
  p.GetActionListener().OnCellLoaded(raw, msg);
}

void Host(PartOne& p, Networking::UserId user, uint32_t cell)
{
  const auto past = NowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  Loaded(p, user, cell, true);
  p.TickCellHosts(past);
}

// the host (user 0) and the guest (user 1) at `where` in the temple, the host
// hosting it; returns the grant's epoch
uint32_t Enter(PartOne& p, const NiPoint3& where)
{
  DoConnect(p, 0);
  p.CreateActor(kHostActor, where, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, where, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  for (uint32_t id : { kHostActor, kGuestActor }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(where);
    actor.ForceSubscriptionsUpdate();
  }
  Host(p, 0, kTemple);
  REQUIRE(p.GetCellHost(kTemple).hosted);
  REQUIRE(p.GetCellHost(kTemple).player == kHostActor);
  p.Messages().clear();
  return p.GetCellHost(kTemple).epoch;
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHostActor);
  p.DestroyActor(kGuestActor);
}

void Call(PartOne& p, Networking::UserId user, uint32_t epoch, uint32_t selfId,
          const char* fn, std::vector<Arg> args)
{
  ScriptCallMessage msg;
  msg.epoch = epoch;
  msg.selfId = selfId;
  msg.cls = "ObjectReference";
  msg.fn = fn;
  msg.args = std::move(args);
  msg.caller = "defaultEnableEncLinkedRef";
  msg.handler = "onTriggerEnter";
  msg.latent = std::string(fn) == "Enable" || std::string(fn) == "Disable" ||
    std::string(fn) == "Delete";
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnScriptCall(raw, msg);
}

std::vector<DestructionStageMessage> WordsTo(PartOne& p,
                                             Networking::UserId user,
                                             uint32_t refr)
{
  std::vector<DestructionStageMessage> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<DestructionStageMessage*>(m.message.get());
    if (m.userId == user && typed && typed->refr == refr) {
      out.push_back(*typed);
    }
  }
  return out;
}

// the index in the send log of the first message of `Message` to `user`
// for which `pick` holds, or -1 (DestructionStageTest's shape)
template <class Message, class Pick>
int IndexOf(PartOne& p, Networking::UserId user, Pick pick)
{
  int i = 0;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<Message*>(m.message.get());
    if (m.userId == user && typed && pick(*typed)) {
      return i;
    }
    ++i;
  }
  return -1;
}

struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("ref-state-test", sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
  }
  ~LogCapture() { spdlog::set_default_logger(old); }
  size_t Count(const std::string& line) const
  {
    const std::string log = oss.str();
    size_t n = 0;
    for (size_t at = log.find(line); at != std::string::npos;
         at = log.find(line, at + line.size())) {
      ++n;
    }
    return n;
  }
};
}

TEST_CASE("The cell's host's Disable of a world object is recorded with the "
          "object's sequence and relayed as the ref state's word; a repeat "
          "is folded, a guest's refused",
          "[RefState][espm]")
{
  auto& p = GetPartOne();
  auto& portcullis = p.worldState.GetFormAt<MpObjectReference>(kPortcullis);
  const uint32_t epoch = Enter(p, kByThePortcullis);
  REQUIRE(!portcullis.GetRefStateSeq());

  {
    LogCapture log;
    Call(p, 0, epoch, kPortcullis, "Disable", { Arg(false) });
    REQUIRE(log.Count("[verb:scripts] ff000c60 sets 3b9f1 disabled "
                      "(ObjectReference.Disable from "
                      "defaultEnableEncLinkedRef.onTriggerEnter, seq 1): "
                      "recorded, relayed to 1 game(s)") == 1);
  }
  REQUIRE(portcullis.IsDisabled());
  REQUIRE(!portcullis.IsDeleted());
  REQUIRE(portcullis.GetRefStateSeq() == 1u);
  auto words = WordsTo(p, 1, kPortcullis);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].disabled == true);
  REQUIRE(words[0].deleted == false);
  REQUIRE(words[0].seq == 1);
  REQUIRE(!words[0].served);
  REQUIRE(WordsTo(p, 0, kPortcullis).empty());
  // a disabled world object whose state is recorded keeps its listeners
  REQUIRE(portcullis.GetActorListeners().size() >= 2);

  // the guest's engine's copy of the same Disable: folded (no relay); its
  // own report of another state is no owner's
  p.Messages().clear();
  Call(p, 0, epoch, kPortcullis, "DisableNoWait", { Arg(false) });
  REQUIRE(WordsTo(p, 1, kPortcullis).empty());
  {
    LogCapture log;
    Call(p, 1, epoch, kPortcullis, "Enable", { Arg(true) });
    REQUIRE(log.Count("[verb:scripts] E_SCRIPTCALL_OWNER: ff000c61 reports "
                      "ObjectReference.Enable on 3b9f1") == 1);
  }
  REQUIRE(portcullis.IsDisabled());
  REQUIRE(WordsTo(p, 0, kPortcullis).empty());

  // the host's Enable: recorded at the object's next number (the fold spent
  // its own, a gap no receiver reads)
  Call(p, 0, epoch, kPortcullis, "EnableNoWait", { Arg(false) });
  REQUIRE(!portcullis.IsDisabled());
  REQUIRE(portcullis.GetRefStateSeq() == 3u);
  words = WordsTo(p, 1, kPortcullis);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].disabled == false);
  REQUIRE(words[0].seq == 3);
  Leave(p);
}

TEST_CASE("A Delete keeps a game-file reference deleted and disabled, an "
          "Enable after it folded; the reporter's own player is no record "
          "row's",
          "[RefState][espm]")
{
  auto& p = GetPartOne();
  auto& portcullis = p.worldState.GetFormAt<MpObjectReference>(kPortcullis);
  const uint32_t epoch = Enter(p, kByThePortcullis);
  {
    LogCapture log;
    Call(p, 0, epoch, kPortcullis, "Delete", {});
    REQUIRE(log.Count("[verb:scripts] ff000c60 sets 3b9f1 deleted") == 1);
  }
  REQUIRE(portcullis.IsDeleted());
  REQUIRE(portcullis.IsDisabled());
  const auto words = WordsTo(p, 1, kPortcullis);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].deleted == true);
  REQUIRE(words[0].disabled == true);
  p.Messages().clear();
  Call(p, 0, epoch, kPortcullis, "Enable", { Arg(false) });
  REQUIRE(portcullis.IsDeleted());
  REQUIRE(portcullis.IsDisabled());
  REQUIRE(WordsTo(p, 1, kPortcullis).empty());
  {
    LogCapture log;
    Call(p, 0, epoch, 0x14, "Disable", { Arg(false) });
    REQUIRE(log.Count("[verb:scripts] E_SCRIPTCALL_ROW: ff000c60 reports "
                      "ObjectReference.Disable on 14") == 1);
  }
  Leave(p);
}

TEST_CASE("A reference the loader never held is attached by its record at "
          "its host's first recorded Enable, and a seat its subscription "
          "reaches is served its state",
          "[RefState][espm]")
{
  auto& p = GetPartOne();
  const uint32_t epoch = Enter(p, kByTheXMarker);
  REQUIRE(!p.worldState.LookupFormByIdNoLoad(kXMarker));
  Call(p, 0, epoch, kXMarker, "Enable", { Arg(false) });
  const auto& form = p.worldState.LookupFormByIdNoLoad(kXMarker);
  REQUIRE(form);
  MpObjectReference* marker = form->AsObjectReference();
  REQUIRE(marker);
  REQUIRE(marker->GetBaseType() == "STAT");
  REQUIRE(!marker->IsDisabled());
  REQUIRE(marker->GetRefStateSeq() == 1u);
  // its subscription reached the guest: the create word, then the served
  // state (a seat the change subscribed hears no relay)
  const auto words = WordsTo(p, 1, kXMarker);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].served);
  REQUIRE(words[0].disabled == false);
  REQUIRE(words[0].seq == 1);

  // its Disable after: relayed to the guest, the marker kept on the grid
  p.Messages().clear();
  Call(p, 0, epoch, kXMarker, "Disable", { Arg(false) });
  REQUIRE(marker->IsDisabled());
  const auto relayed = WordsTo(p, 1, kXMarker);
  REQUIRE(relayed.size() == 1);
  REQUIRE(!relayed[0].served);
  REQUIRE(relayed[0].disabled == true);
  REQUIRE(relayed[0].seq == 2);
  REQUIRE(!marker->GetActorListeners().empty());
  Leave(p);
}

TEST_CASE("An actor the server never held is enabled by its cell's host's "
          "record row and served to the seats it reaches",
          "[RefState][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  const uint32_t epoch = Enter(p, kByTheDraugr);
  REQUIRE(!p.worldState.LookupFormByIdNoLoad(kAmbushDraugr));
  {
    LogCapture log;
    Call(p, 0, epoch, kAmbushDraugr, "Enable", { Arg(false) });
    REQUIRE(log.Count("[verb:scripts] ff000c60 sets 380ba enabled") == 1);
  }
  const auto& form = p.worldState.LookupFormByIdNoLoad(kAmbushDraugr);
  REQUIRE(form);
  MpActor* draugr = form->AsActor();
  REQUIRE(draugr);
  REQUIRE(!draugr->IsDisabled());
  REQUIRE(draugr->GetRefStateSeq() == 1u);
  const auto words = WordsTo(p, 1, kAmbushDraugr);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].served);
  REQUIRE(words[0].disabled == false);
  Leave(p);
}

TEST_CASE("A recorded ref state survives a restart: a reference the loader "
          "skips is attached by its saved record and served behind its "
          "create word",
          "[RefState][espm]")
{
  MpChangeForm saved;
  {
    auto& p = GetPartOne();
    const uint32_t epoch = Enter(p, kByTheXMarker);
    Call(p, 0, epoch, kXMarker, "Enable", { Arg(false) });
    Call(p, 0, epoch, kXMarker, "Disable", { Arg(false) });
    const auto& form = p.worldState.LookupFormByIdNoLoad(kXMarker);
    REQUIRE(form);
    saved = form->AsObjectReference()->GetChangeForm();
    REQUIRE(saved.refStateSeq == 2u);
    REQUIRE(saved.isDisabled);
    Leave(p);
  }
  // the restart: a fresh world takes the saved record, and the lookup
  // attaches the marker by it, disabled and kept on the grid
  auto& p = GetPartOne();
  p.worldState.LoadChangeForm(saved, p.CreateFormCallbacks());
  const auto& form = p.worldState.LookupFormById(kXMarker);
  REQUIRE(form);
  MpObjectReference* marker = form->AsObjectReference();
  REQUIRE(marker);
  REQUIRE(marker->IsDisabled());
  REQUIRE(marker->GetRefStateSeq() == 2u);
  REQUIRE(marker->KeepsGridWhileDisabled());

  // a seat arrives by it: the marker's create word, then its state on the
  // same ordered channel, served
  {
    LogCapture log;
    DoConnect(p, 2);
    p.CreateActor(kLateActor, kByTheXMarker, 0, kTemple);
    p.SetUserActor(2, kLateActor);
    REQUIRE(log.Count("[verb:scripts] ref state 17037 (disabled true, "
                      "deleted false, seq 2) served to ff000c62 after its "
                      "create word") == 1);
  }
  const int created = IndexOf<CreateActorMessage>(
    p, 2, [](const CreateActorMessage& m) { return m.refrId == kXMarker; });
  const int served = IndexOf<DestructionStageMessage>(
    p, 2, [](const DestructionStageMessage& m) { return m.refr == kXMarker; });
  REQUIRE(created >= 0);
  REQUIRE(served == created + 1);
  const auto words = WordsTo(p, 2, kXMarker);
  REQUIRE(words.size() == 1);
  REQUIRE(words[0].served);
  REQUIRE(words[0].disabled == true);
  REQUIRE(words[0].deleted == false);
  REQUIRE(words[0].seq == 2);
  DoDisconnect(p, 2);
  p.DestroyActor(kLateActor);
}

TEST_CASE("A game-file reference a script deleted stays deleted across a "
          "restart: the save's loader keeps its record and the reference "
          "attaches by it",
          "[RefState][espm]")
{
  MpChangeForm saved;
  {
    auto& p = GetPartOne();
    auto& portcullis = p.worldState.GetFormAt<MpObjectReference>(kPortcullis);
    const uint32_t epoch = Enter(p, kByThePortcullis);
    Call(p, 0, epoch, kPortcullis, "Delete", {});
    REQUIRE(portcullis.IsDeleted());
    saved = portcullis.GetChangeForm();
    REQUIRE(saved.isDeleted);
    REQUIRE(saved.refStateSeq == 1u);
    Leave(p);
  }
  // the restarted server reads the save as at start (PartOne's loader
  // dropped every deleted record before rung 4, so the files' standing
  // portcullis came back)
  auto storage = MakeSaveStorageFile();
  UpsertSync(*storage, { saved });
  auto& p = GetPartOne();
  p.AttachSaveStorage(storage);
  const auto& form = p.worldState.LookupFormById(kPortcullis);
  REQUIRE(form);
  MpObjectReference* portcullis = form->AsObjectReference();
  REQUIRE(portcullis);
  REQUIRE(portcullis->IsDeleted());
  REQUIRE(portcullis->IsDisabled());
  REQUIRE(portcullis->GetRefStateSeq() == 1u);
  REQUIRE(portcullis->KeepsGridWhileDisabled());
}

TEST_CASE("The server's own Disable of a game-file world object stamps its "
          "ref state recorded",
          "[RefState][espm]")
{
  auto& p = GetPartOne();
  auto& portcullis = p.worldState.GetFormAt<MpObjectReference>(kPortcullis);
  REQUIRE(!portcullis.GetRefStateSeq());
  portcullis.Disable();
  REQUIRE(portcullis.GetRefStateSeq().has_value());
  REQUIRE(portcullis.KeepsGridWhileDisabled());
  portcullis.Enable();
  REQUIRE(!portcullis.IsDisabled());
}
