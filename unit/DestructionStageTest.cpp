#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

PartOne& GetPartOne();

// thuum D-S3 (traps follow the interaction; ADR-068's DestructionStage row;
// docs/research/trap-slice-d-s3-2026-11-05.md point 3): every seat runs the
// trap family itself, so one firing's result is reported by more than one
// seat; the server records the first report of each step (wire-rules
// destruction: the stage only rises, destroyed only goes true), relays it to
// the reference's other listeners and serves the record behind the
// reference's create word to a seat that subscribes later. The stage, from
// the census (docs/research/trap-census-2026-11-05.md, lab/esm.py's reads):
// Bleak Falls Temple 0x371de's oil lamp 0x9b0fb (TrapFallingOilLamp01
// 0x90efc, an ACTI, destructible: OilTrapDestroyed.nif, then
// FXEmptyObject.nif); Embershard Mine 0xb6be6's tripwire 0xb6d3f
// (TrapTripwire01 0x22f54, an ACTI, SetDestroyed(true) once fired); the
// temple's draugr 0x380b3 (an ACHR, TrapRelayTest's); the temple's bandit
// chest TreasBanditChest 0x371e9 at (-3420, 732, 575), a CONT (LockTest's,
// lab/esm.py 2026-10-31). The players stand by each reference where the
// loaded reference stands, never a typed position.
namespace {
constexpr uint32_t kHostActor = 0xff000fe5;
constexpr uint32_t kGuestActor = 0xff000fe6;
constexpr uint32_t kLateActor = 0xff000fe7;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kLamp = 0x9b0fb;
constexpr uint32_t kEmbershard = 0xb6be6;
constexpr uint32_t kTripwire = 0xb6d3f;
constexpr uint32_t kDraugr = 0x380b3;
constexpr uint32_t kBanditChest = 0x371e9;

using Destruction = MpChangeFormREFR::Destruction;

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// `user`'s game reports `cell` loaded or unloaded (CellLoaded)
void Loaded(PartOne& p, Networking::UserId user, uint32_t cell, bool loaded)
{
  RawMessageData raw;
  raw.userId = user;
  CellLoadedMessage msg;
  msg.cell = cell;
  msg.loaded = loaded;
  p.GetActionListener().OnCellLoaded(raw, msg);
}

// `user` hosts `cell` (TrapRelayTest's shape)
void Host(PartOne& p, Networking::UserId user, uint32_t cell)
{
  const auto past = NowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  Loaded(p, user, cell, true);
  p.TickCellHosts(past);
}

// the reference `refrId` loaded, then the host (user 0) and the guest (user
// 1) beside it in the interior `cell`, the host hosting the cell and, with
// `guestLoaded`, the guest's game reporting it loaded too
MpObjectReference& Enter(PartOne& p, uint32_t cell, uint32_t refrId,
                         bool guestLoaded = true)
{
  auto& refr = p.worldState.GetFormAt<MpObjectReference>(refrId);
  const NiPoint3 by = refr.GetPos() + NiPoint3{ 100.f, 0.f, 0.f };
  DoConnect(p, 0);
  p.CreateActor(kHostActor, by, 0, cell);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, by, 0, cell);
  p.SetUserActor(1, kGuestActor);
  for (uint32_t id : { kHostActor, kGuestActor }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(by);
    actor.ForceSubscriptionsUpdate();
  }
  Host(p, 0, cell);
  if (guestLoaded) {
    Loaded(p, 1, cell, true);
  }
  REQUIRE(p.GetCellHost(cell).hosted);
  REQUIRE(p.GetCellHost(cell).player == kHostActor);
  REQUIRE(p.worldState.CellOf(refr) == std::optional<uint32_t>(cell));
  REQUIRE(refr.GetActorListeners().size() >= 2);
  p.Messages().clear();
  return refr;
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHostActor);
  p.DestroyActor(kGuestActor);
}

// `user`'s game reports `refr` at `stage`, `destroyed` (a seat's report is
// never served)
void Report(PartOne& p, Networking::UserId user, uint32_t refr, uint32_t stage,
            bool destroyed)
{
  DestructionStageMessage msg;
  msg.refr = refr;
  msg.stage = stage;
  msg.destroyed = destroyed;
  msg.served = false;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnDestructionStage(raw, msg);
}

std::vector<DestructionStageMessage> SentTo(PartOne& p,
                                            Networking::UserId user)
{
  std::vector<DestructionStageMessage> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<DestructionStageMessage*>(m.message.get());
    if (m.userId == user && typed) {
      out.push_back(*typed);
    }
  }
  return out;
}

// the index in the send log of the first message of `Message` to `user`
// for which `pick` holds, or -1 (ChangeFormTest's shape)
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

// the default logger's lines at info and above while the guard lives
// (TrapRelayTest's shape), restored at its end
struct LogCapture
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  LogCapture()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger =
      std::make_shared<spdlog::logger>("destruction-stage-test", sink);
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

TEST_CASE("The first report of a reference's stage rise is recorded and "
          "relayed to its other listeners; an equal or a lower report "
          "changes nothing",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  auto& lamp = Enter(p, kTemple, kLamp);
  REQUIRE(!lamp.GetDestruction().has_value());

  // the guest's arrow brings the lamp down: its engine's stage rise is the
  // first report, recorded and relayed to the host, never back to the guest
  {
    LogCapture log;
    Report(p, 1, kLamp, 1, false);
    REQUIRE(log.Count("[verb:traps] ff000fe6 reports 9b0fb at stage 1, "
                      "destroyed false: recorded (the first report), "
                      "relayed to 1 game(s)") == 1);
  }
  REQUIRE(lamp.GetDestruction() ==
          std::optional<Destruction>(Destruction{ 1, false }));
  const auto toHost = SentTo(p, 0);
  REQUIRE(toHost.size() == 1);
  REQUIRE(toHost[0].refr == kLamp);
  REQUIRE(toHost[0].stage == 1);
  REQUIRE(!toHost[0].destroyed);
  REQUIRE(!toHost[0].served);
  REQUIRE(SentTo(p, 1).empty());

  // the host's engine fired the same lamp: its equal report is dropped, and
  // so is a lower one
  p.Messages().clear();
  {
    LogCapture log;
    Report(p, 0, kLamp, 1, false);
    Report(p, 0, kLamp, 0, false);
    REQUIRE(log.Count("[verb:traps] ff000fe5 reports 9b0fb at stage 1, "
                      "destroyed false: the record stands (stage 1, "
                      "destroyed false); dropped") == 1);
    REQUIRE(log.Count("[verb:traps] ff000fe5 reports 9b0fb at stage 0, "
                      "destroyed false: the record stands (stage 1, "
                      "destroyed false); dropped") == 1);
    REQUIRE(log.Count("recorded") == 0);
  }
  REQUIRE(SentTo(p, 0).empty());
  REQUIRE(SentTo(p, 1).empty());
  REQUIRE(lamp.GetDestruction() ==
          std::optional<Destruction>(Destruction{ 1, false }));

  // the fall's end, stage 2, first from the host: recorded, to the guest
  Report(p, 0, kLamp, 2, false);
  REQUIRE(lamp.GetDestruction() ==
          std::optional<Destruction>(Destruction{ 2, false }));
  const auto toGuest = SentTo(p, 1);
  REQUIRE(toGuest.size() == 1);
  REQUIRE(toGuest[0].stage == 2);
  REQUIRE(!toGuest[0].served);
  REQUIRE(SentTo(p, 0).empty());

  // a second shot records nothing
  p.Messages().clear();
  Report(p, 1, kLamp, 2, false);
  REQUIRE(SentTo(p, 0).empty());
  REQUIRE(SentTo(p, 1).empty());
  REQUIRE(lamp.GetDestruction() ==
          std::optional<Destruction>(Destruction{ 2, false }));
  Leave(p);
}

TEST_CASE("A spent tripwire's destruction is recorded once, and a whole "
          "copy's report never brings it back",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  auto& wire = Enter(p, kEmbershard, kTripwire);

  // the guest walks the wire: its engine's SetDestroyed(true) is the first
  {
    LogCapture log;
    Report(p, 1, kTripwire, 0, true);
    REQUIRE(log.Count("[verb:traps] ff000fe6 reports b6d3f at stage 0, "
                      "destroyed true: recorded (the first report), "
                      "relayed to 1 game(s)") == 1);
  }
  REQUIRE(wire.GetDestruction() ==
          std::optional<Destruction>(Destruction{ 0, true }));
  const auto toHost = SentTo(p, 0);
  REQUIRE(toHost.size() == 1);
  REQUIRE(toHost[0].refr == kTripwire);
  REQUIRE(toHost[0].stage == 0);
  REQUIRE(toHost[0].destroyed);
  REQUIRE(!toHost[0].served);

  // the host's copy, never fired there, reports it whole: the record stands
  p.Messages().clear();
  {
    LogCapture log;
    Report(p, 0, kTripwire, 0, false);
    REQUIRE(log.Count("[verb:traps] ff000fe5 reports b6d3f at stage 0, "
                      "destroyed false: the record stands (stage 0, "
                      "destroyed true); dropped") == 1);
  }
  REQUIRE(wire.GetDestruction() ==
          std::optional<Destruction>(Destruction{ 0, true }));
  REQUIRE(SentTo(p, 0).empty());
  REQUIRE(SentTo(p, 1).empty());
  Leave(p);
}

TEST_CASE("A destruction report on no reference, on an actor or from a game "
          "without the reference's cell loaded is refused and records "
          "nothing",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  // the guest's game has not reported the temple loaded
  auto& lamp = Enter(p, kTemple, kLamp, false);
  {
    LogCapture log;
    Report(p, 1, kLamp, 1, false);
    REQUIRE(log.Count("[verb:traps] E_DESTRUCTION_CELL: ff000fe6 reports "
                      "9b0fb at stage 1, destroyed false: its game does not "
                      "have the reference's cell 371de loaded") == 1);
    // a reference the server names, and none at all
    Report(p, 0, 0xff000dff, 1, false);
    Report(p, 0, 0, 1, false);
    REQUIRE(log.Count("[verb:traps] E_DESTRUCTION_NO_REF: ff000fe5 reports "
                      "ff000dff at stage 1, destroyed false: not a game-file "
                      "reference") == 1);
    REQUIRE(log.Count("[verb:traps] E_DESTRUCTION_NO_REF: ff000fe5 reports "
                      "0 at stage 1, destroyed false: not a game-file "
                      "reference") == 1);
    REQUIRE(log.Count("recorded") == 0);
  }
  REQUIRE(!lamp.GetDestruction().has_value());
  REQUIRE(SentTo(p, 0).empty());
  REQUIRE(SentTo(p, 1).empty());

  // an actor: the temple's draugr, loaded with NPCs on
  p.worldState.npcEnabled = true;
  auto& draugr = p.worldState.GetFormAt<MpActor>(kDraugr);
  {
    LogCapture log;
    Report(p, 0, kDraugr, 1, true);
    REQUIRE(log.Count("[verb:traps] E_DESTRUCTION_ACTOR: ff000fe5 reports "
                      "380b3 at stage 1, destroyed true: an actor, whose "
                      "death is the NPC fate's") == 1);
  }
  REQUIRE(!draugr.GetDestruction().has_value());
  REQUIRE(SentTo(p, 0).empty());

  // the guest's game reports the temple loaded: its report is the first
  Loaded(p, 1, kTemple, true);
  Report(p, 1, kLamp, 1, false);
  REQUIRE(lamp.GetDestruction() ==
          std::optional<Destruction>(Destruction{ 1, false }));
  REQUIRE(SentTo(p, 0).size() == 1);
  Leave(p);
}

// the review of 9ff1895e (finding 1): before the kind check any reference
// that was no actor could be recorded destroyed, a container's or a door's
// as well as a trap's; IsWorldBodyKind refuses a door, a container,
// furniture and an item alike, and the temple's chest stands for them
TEST_CASE("A destruction report on a container, which is no world body, is "
          "refused and records nothing: a trap's result is a world body's",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  // both games have the temple loaded, so only the kind refuses
  auto& chest = Enter(p, kTemple, kBanditChest);
  REQUIRE(chest.GetBaseType() == "CONT");
  {
    LogCapture log;
    Report(p, 1, kBanditChest, 1, false);
    Report(p, 0, kBanditChest, 0, true);
    REQUIRE(log.Count("[verb:traps] E_DESTRUCTION_KIND: ff000fe6 reports "
                      "371e9 at stage 1, destroyed false: not a trap or "
                      "destructible world body") == 1);
    REQUIRE(log.Count("[verb:traps] E_DESTRUCTION_KIND: ff000fe5 reports "
                      "371e9 at stage 0, destroyed true: not a trap or "
                      "destructible world body") == 1);
    REQUIRE(log.Count("recorded") == 0);
  }
  REQUIRE(!chest.GetDestruction().has_value());
  REQUIRE(SentTo(p, 0).empty());
  REQUIRE(SentTo(p, 1).empty());
  Leave(p);
}

TEST_CASE("A game that subscribes to a reference after its destruction is "
          "recorded hears the record right after the reference's create "
          "word, served",
          "[Traps][espm]")
{
  PartOne& p = GetPartOne();
  auto& lamp = Enter(p, kTemple, kLamp);
  Report(p, 1, kLamp, 2, false);
  REQUIRE(lamp.GetDestruction() ==
          std::optional<Destruction>(Destruction{ 2, false }));
  p.Messages().clear();

  // a third game arrives by the lamp: the lamp's create word, then the
  // record on the same ordered channel, served (after a restart the same
  // path serves the saved record; ChangeFormTest's round trip keeps it)
  {
    LogCapture log;
    DoConnect(p, 2);
    p.CreateActor(kLateActor, lamp.GetPos() + NiPoint3{ 100.f, 0.f, 0.f }, 0,
                  kTemple);
    p.SetUserActor(2, kLateActor);
    REQUIRE(log.Count("[verb:traps] 9b0fb's record (stage 2, destroyed "
                      "false) served to ff000fe7 after its create word") == 1);
  }
  const int created = IndexOf<CreateActorMessage>(
    p, 2, [](const CreateActorMessage& m) { return m.refrId == kLamp; });
  const int served = IndexOf<DestructionStageMessage>(
    p, 2, [](const DestructionStageMessage& m) { return m.refr == kLamp; });
  REQUIRE(created >= 0);
  REQUIRE(served == created + 1);
  const auto toLate = SentTo(p, 2);
  REQUIRE(toLate.size() == 1);
  REQUIRE(toLate[0].stage == 2);
  REQUIRE(!toLate[0].destroyed);
  REQUIRE(toLate[0].served);
  // the games that heard the step when it was recorded hear nothing again
  REQUIRE(SentTo(p, 0).empty());
  REQUIRE(SentTo(p, 1).empty());

  // a reference nobody reported is served nothing
  REQUIRE(IndexOf<DestructionStageMessage>(
            p, 2, [](const DestructionStageMessage& m) {
              return m.refr != kLamp;
            }) == -1);

  DoDisconnect(p, 2);
  p.DestroyActor(kLateActor);
  Leave(p);
}
