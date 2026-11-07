#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <fmt/format.h>
#include <optional>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

PartOne& GetPartOne();

// thuum ADR-068 rung 3 (the script record, decision 5 and its amendment A;
// docs/research/rung3-script-record-2026-11-06.md section 1(c)): the owner of
// a reference, the host of its cell, reports the scripts attached to it as
// its engine read them at a quiescent point (ScriptState); the server judges
// the epoch and the owner (wire-rules script_state), keeps the last record on
// the reference with the object's sequence and its view (the reference's
// lastAnimation then) and relays it to nobody; it serves the record right
// behind the reference's create word to a game that does not host the cell
// and right ahead of HostGrant to the cell's new host; a release leaves the
// old host's release read open while nobody hosts the cell, and the next
// grant closes it. The stage is TrapRelayTest's puzzle room in Bleak Falls
// Temple 0x371de: the first pillar NorDefaultPuzzlePillar01 0xaa8cb at (5656,
// -896, -768), defaultPuzzlePillarScript on the REFR and on the base, its
// record the census's five variables (raw types 3, 3, 5, a class, 5;
// docs/verbs/papyrus-events.md), the class one the puzzle lever 0xaa8c3's
// plain form handle; the pillar's turns are ScriptCallTest's
// PlayAnimationAndWait, reported by the cell's host; the temple's draugr
// 0x380b3 (an ACHR, DestructionStageTest's) and the puzzle door's XMarker
// 0x3e728 (a STAT the server never loads, TrapRelayTest's). The players
// stand by the pillar where the loaded reference stands, never a typed
// position.
namespace {
using Arg = std::optional<
  std::variant<bool, double, std::string, SpSnippetObjectArgument>>;

constexpr uint32_t kHostActor = 0xff000fe8;
constexpr uint32_t kGuestActor = 0xff000fe9;
constexpr uint32_t kLateActor = 0xff000fea;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kPillar = 0xaa8cb;
constexpr uint32_t kLever = 0xaa8c3;
constexpr uint32_t kDraugr = 0x380b3;
constexpr uint32_t kMarker = 0x3e728;
// the owners' hashes of two reads of the pillar (the server keeps them and
// never recomputes them)
constexpr uint32_t kFnvPosition01 = 0x2b7e1516;
constexpr uint32_t kFnvPosition02 = 0x5c1a7e01;
// a tick well past a grace's deadline (CellHostTest's margin: the grace is
// stamped by the server's own clock reading inside the unload report)
constexpr uint64_t kPastMs = 2'000;

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

// the pillar loaded, then the host (user 0) and the guest (user 1) beside it
// in the temple, the host hosting the temple and, with `guestLoaded`, the
// guest's game reporting it loaded too; returns the grant's epoch
uint32_t Enter(PartOne& p, bool guestLoaded = true)
{
  auto& pillar = p.worldState.GetFormAt<MpObjectReference>(kPillar);
  const NiPoint3 by = pillar.GetPos() + NiPoint3{ 100.f, 0.f, 0.f };
  DoConnect(p, 0);
  p.CreateActor(kHostActor, by, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, by, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  for (uint32_t id : { kHostActor, kGuestActor }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(by);
    actor.ForceSubscriptionsUpdate();
  }
  Host(p, 0, kTemple);
  if (guestLoaded) {
    Loaded(p, 1, kTemple, true);
  }
  REQUIRE(p.GetCellHost(kTemple).hosted);
  REQUIRE(p.GetCellHost(kTemple).player == kHostActor);
  REQUIRE(p.worldState.CellOf(pillar) == std::optional<uint32_t>(kTemple));
  REQUIRE(pillar.GetActorListeners().size() >= 2);
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

// a variable of kind `k`, its field set by the caller
ScriptStateVar Var(uint32_t k)
{
  ScriptStateVar v;
  v.k = k;
  return v;
}

// a record of `formId` at `epoch`: one defaultPuzzlePillarScript in
// `state`, the census's five variables, the lever's handle among them
ScriptStateMessage Record(uint32_t epoch, uint32_t formId = kPillar,
                          const char* state = "position02",
                          uint32_t fnv1a = kFnvPosition02)
{
  ScriptStateHandle lever;
  lever.kind = 0;
  lever.mid = 0xffff;
  lever.form = kLever;
  lever.type = "defaultPillarPuzzleLever";
  ScriptStateObject script;
  script.type = "defaultPuzzlePillarScript";
  script.state = state;
  script.flags = 0x0b;
  script.vars.push_back(Var(3));
  script.vars.back().i = 1;
  script.vars.push_back(Var(3));
  script.vars.back().i = 3;
  script.vars.push_back(Var(5));
  script.vars.back().b = true;
  script.vars.push_back(Var(1));
  script.vars.back().o = lever;
  script.vars.push_back(Var(5));
  script.vars.back().b = false;
  ScriptStateMessage msg;
  msg.epoch = epoch;
  msg.formId = formId;
  msg.fnv1a = fnv1a;
  msg.scripts.push_back(script);
  return msg;
}

void Report(PartOne& p, Networking::UserId user, const ScriptStateMessage& msg)
{
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnScriptState(raw, msg);
}

// the pillar's turn as `user`'s engine reports it (ScriptCallTest's
// PlayAnimationAndWait, relayed as PlayAnimation with the pillar's sequence,
// its event the pillar's lastAnimation)
void Turn(PartOne& p, Networking::UserId user, uint32_t epoch,
          const char* trigger, const char* turned)
{
  ScriptCallMessage msg;
  msg.epoch = epoch;
  msg.selfId = kPillar;
  msg.cls = "ObjectReference";
  msg.fn = "PlayAnimationAndWait";
  msg.args = { Arg(std::string(trigger)), Arg(std::string(turned)) };
  msg.caller = "defaultPuzzlePillarScript";
  msg.handler = "onActivate";
  msg.latent = true;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnScriptCall(raw, msg);
}

// a stored record as a restart loads it: taken at the pillar's sequence 5,
// its view Trigger02
ScriptStateRecord Saved(uint32_t epoch)
{
  ScriptStateRecord saved;
  saved.epoch = epoch;
  saved.fnv1a = kFnvPosition02;
  saved.sender = kHostActor;
  saved.atMs = 1762440000123ull;
  saved.seq = 5;
  saved.event = "Trigger02";
  saved.scripts = Record(epoch).scripts;
  return saved;
}

template <class Message>
std::vector<Message> SentTo(PartOne& p, Networking::UserId user)
{
  std::vector<Message> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<Message*>(m.message.get());
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

// the snippets sent to `user`, as the fake send target recorded them (JSON)
std::vector<nlohmann::json> SnippetsTo(PartOne& p, Networking::UserId user)
{
  std::vector<nlohmann::json> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j["t"] == static_cast<int>(MsgType::SpSnippet)) {
      out.push_back(m.j);
    }
  }
  return out;
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
    auto logger = std::make_shared<spdlog::logger>("script-state-test", sink);
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

TEST_CASE("A host's script record is kept on the reference, stamped with the "
          "object's sequence and its view of that moment, and relayed to "
          "nobody",
          "[ScriptState][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = Enter(p);
  auto& pillar = p.worldState.GetFormAt<MpObjectReference>(kPillar);
  REQUIRE(!pillar.GetScriptState().has_value());

  // the host turns the pillar to position01: the call's relay takes the
  // pillar's sequence 1 and its event is the pillar's lastAnimation
  Turn(p, 0, epoch, "Trigger01", "Turned01");
  REQUIRE(pillar.GetLastAnimation() ==
          std::optional<std::string>("Trigger01"));
  p.Messages().clear();

  // its engine's read at the turn's end
  const auto msg = Record(epoch, kPillar, "position01", kFnvPosition01);
  {
    LogCapture log;
    Report(p, 0, msg);
    REQUIRE(log.Count(fmt::format(
              "[verb:scripts] script state aa8cb recorded from ff000fe8: 1 "
              "script(s), fnv 2b7e1516, epoch {}, seq 1, view 'Trigger01'",
              epoch)) == 1);
  }
  REQUIRE(pillar.GetScriptState().has_value());
  REQUIRE(pillar.GetScriptState()->epoch == epoch);
  REQUIRE(pillar.GetScriptState()->fnv1a == kFnvPosition01);
  REQUIRE(pillar.GetScriptState()->sender == kHostActor);
  REQUIRE(pillar.GetScriptState()->atMs > 0);
  REQUIRE(pillar.GetScriptState()->seq == 1);
  REQUIRE(pillar.GetScriptState()->event ==
          std::optional<std::string>("Trigger01"));
  REQUIRE(pillar.GetScriptState()->scripts == msg.scripts);
  // never relayed live, not even to the guest beside the pillar
  REQUIRE(SentTo<ScriptStateApplyMessage>(p, 0).empty());
  REQUIRE(SentTo<ScriptStateApplyMessage>(p, 1).empty());

  // the next turn moves the pillar's sequence and its lastAnimation on; the
  // record keeps the ones it was taken at
  Turn(p, 0, epoch, "Trigger02", "Turned02");
  REQUIRE(pillar.GetLastAnimation() ==
          std::optional<std::string>("Trigger02"));
  REQUIRE(pillar.GetScriptState()->seq == 1);
  REQUIRE(pillar.GetScriptState()->event ==
          std::optional<std::string>("Trigger01"));

  // the host's read after that turn replaces the record, stamped anew
  Report(p, 0, Record(epoch));
  REQUIRE(pillar.GetScriptState()->fnv1a == kFnvPosition02);
  REQUIRE(pillar.GetScriptState()->seq == 2);
  REQUIRE(pillar.GetScriptState()->event ==
          std::optional<std::string>("Trigger02"));
  REQUIRE(pillar.GetScriptState()->scripts.front().state == "position02");
  REQUIRE(SentTo<ScriptStateApplyMessage>(p, 1).empty());
  Leave(p);
}

TEST_CASE("A script record from a game that does not host the cell, under "
          "another epoch, of an actor or of no reference is refused and "
          "kept nowhere",
          "[ScriptState][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = Enter(p);
  auto& pillar = p.worldState.GetFormAt<MpObjectReference>(kPillar);
  {
    LogCapture log;
    // the guest names the cell's epoch but does not host the cell
    Report(p, 1, Record(epoch));
    REQUIRE(log.Count(fmt::format(
              "[verb:scripts] E_SCRIPTSTATE_OWNER: ff000fe9 reports the "
              "script state of aa8cb (1 script(s), fnv 5c1a7e01, epoch {}): "
              "ff000fe8 hosts cell 371de, not the reporter; refused",
              epoch)) == 1);
    // the host under an epoch ahead of the cell's, and the guest under the
    // 0 it holds for a cell it does not host: the epoch is judged first
    Report(p, 0, Record(epoch + 1));
    REQUIRE(log.Count(fmt::format(
              "[verb:scripts] E_SCRIPTSTATE_EPOCH: ff000fe8 reports the "
              "script state of aa8cb (1 script(s), fnv 5c1a7e01, epoch {}): "
              "cell 371de stands at epoch {}, the record names {}; refused",
              epoch + 1, epoch, epoch + 1)) == 1);
    Report(p, 1, Record(0));
    REQUIRE(log.Count(fmt::format(
              "[verb:scripts] E_SCRIPTSTATE_EPOCH: ff000fe9 reports the "
              "script state of aa8cb (1 script(s), fnv 5c1a7e01, epoch 0): "
              "cell 371de stands at epoch {}, the record names 0; refused",
              epoch)) == 1);
    // a reference the server names, and one it never loads
    Report(p, 0, Record(epoch, 0xff000dff));
    REQUIRE(log.Count(fmt::format(
              "[verb:scripts] E_SCRIPTSTATE_NO_REF: ff000fe8 reports the "
              "script state of ff000dff (1 script(s), fnv 5c1a7e01, epoch "
              "{}): not a game-file reference; refused",
              epoch)) == 1);
    Report(p, 0, Record(epoch, kMarker));
    REQUIRE(log.Count(fmt::format(
              "[verb:scripts] E_SCRIPTSTATE_NO_REF: ff000fe8 reports the "
              "script state of 3e728 (1 script(s), fnv 5c1a7e01, epoch {}): "
              "the server holds no such reference; refused",
              epoch)) == 1);
    REQUIRE(log.Count("recorded") == 0);
  }
  REQUIRE(!pillar.GetScriptState().has_value());
  REQUIRE(SentTo<ScriptStateApplyMessage>(p, 0).empty());
  REQUIRE(SentTo<ScriptStateApplyMessage>(p, 1).empty());

  // an actor the server never loaded, with NPCs on: refused by its record's
  // type and left unloaded (OnDestructionStage's idiom)
  p.worldState.npcEnabled = true;
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kDraugr) == nullptr);
  {
    LogCapture log;
    Report(p, 0, Record(epoch, kDraugr));
    REQUIRE(log.Count(fmt::format(
              "[verb:scripts] E_SCRIPTSTATE_KIND: ff000fe8 reports the "
              "script state of 380b3 (1 script(s), fnv 5c1a7e01, epoch {}): "
              "an actor: its scripts are its simulator's, a later rung's; "
              "refused",
              epoch)) == 1);
  }
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kDraugr) == nullptr);

  // the host at the cell's epoch is recorded, so the refusals above were
  // the premises and not the stage
  Report(p, 0, Record(epoch));
  REQUIRE(pillar.GetScriptState().has_value());
  REQUIRE(pillar.GetScriptState()->sender == kHostActor);
  Leave(p);
}

TEST_CASE("A game that subscribes to a recorded reference hears the record, "
          "its sequence and its view right behind the reference's create "
          "word",
          "[ScriptState][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = Enter(p);
  auto& pillar = p.worldState.GetFormAt<MpObjectReference>(kPillar);
  Turn(p, 0, epoch, "Trigger01", "Turned01");
  Report(p, 0, Record(epoch, kPillar, "position01", kFnvPosition01));
  // a turn after the read: the pillar's lastAnimation moves on, the
  // record's view stays the one it was taken with
  Turn(p, 0, epoch, "Trigger02", "Turned02");
  p.Messages().clear();

  // a third game arrives by the pillar: the pillar's create word, then the
  // record on the same ordered channel
  {
    LogCapture log;
    DoConnect(p, 2);
    p.CreateActor(kLateActor, pillar.GetPos() + NiPoint3{ 100.f, 0.f, 0.f }, 0,
                  kTemple);
    p.SetUserActor(2, kLateActor);
    REQUIRE(log.Count("[verb:scripts] script state aa8cb served to ff000fea "
                      "after its create word: 1 script(s), seq 1, view "
                      "'Trigger01'") == 1);
  }
  const int created = IndexOf<CreateActorMessage>(
    p, 2, [](const CreateActorMessage& m) { return m.refrId == kPillar; });
  const int served = IndexOf<ScriptStateApplyMessage>(
    p, 2,
    [](const ScriptStateApplyMessage& m) { return m.formId == kPillar; });
  REQUIRE(created >= 0);
  REQUIRE(served == created + 1);
  const auto applies = SentTo<ScriptStateApplyMessage>(p, 2);
  REQUIRE(applies.size() == 1);
  REQUIRE(applies[0].seq == 1);
  REQUIRE(applies[0].event == std::optional<std::string>("Trigger01"));
  REQUIRE(applies[0].fnv1a == kFnvPosition01);
  REQUIRE(applies[0].scripts == pillar.GetScriptState()->scripts);
  // the games listening when it was recorded hear nothing of it
  REQUIRE(SentTo<ScriptStateApplyMessage>(p, 0).empty());
  REQUIRE(SentTo<ScriptStateApplyMessage>(p, 1).empty());

  DoDisconnect(p, 2);
  p.DestroyActor(kLateActor);
  Leave(p);
}

TEST_CASE("At a grant the cell's recorded references reach the new host "
          "ahead of its HostGrant, and a release read is taken while nobody "
          "hosts the cell, then refused after the next grant",
          "[ScriptState][espm]")
{
  PartOne& p = GetPartOne();
  // the guest's game has not reported the temple loaded, so the host's
  // release leaves the cell unhosted
  const uint32_t epoch = Enter(p, false);
  auto& pillar = p.worldState.GetFormAt<MpObjectReference>(kPillar);
  Turn(p, 0, epoch, "Trigger01", "Turned01");
  Report(p, 0, Record(epoch, kPillar, "position01", kFnvPosition01));
  REQUIRE(pillar.GetScriptState()->fnv1a == kFnvPosition01);

  // the host's game unloads the temple: the grace runs out with no other
  // game loading it, the host is told its release and the cell is
  // unhosted, its epoch kept
  const auto now = NowMs();
  Loaded(p, 0, kTemple, false);
  p.TickCellHosts(now + skymp::rules::host_grace_ms() + kPastMs);
  REQUIRE(!p.GetCellHost(kTemple).hosted);
  REQUIRE(p.GetCellHost(kTemple).epoch == epoch);
  REQUIRE(SentTo<HostReleaseMessage>(p, 0).size() == 1);

  // the host's release read of the pillar, at the epoch it released: taken
  {
    LogCapture log;
    Report(p, 0, Record(epoch, kPillar, "position02", kFnvPosition02));
    REQUIRE(log.Count(fmt::format(
              "[verb:scripts] script state aa8cb recorded from ff000fe8: 1 "
              "script(s), fnv 5c1a7e01, epoch {}, seq 1, view 'Trigger01' "
              "(its release read)",
              epoch)) == 1);
    // nobody else's: the guest's at the kept epoch
    Report(p, 1, Record(epoch));
    REQUIRE(log.Count(fmt::format(
              "[verb:scripts] E_SCRIPTSTATE_OWNER: ff000fe9 reports the "
              "script state of aa8cb (1 script(s), fnv 5c1a7e01, epoch {}): "
              "nobody hosts cell 371de; ff000fe8 released it at epoch {}; "
              "refused",
              epoch, epoch)) == 1);
  }
  REQUIRE(pillar.GetScriptState()->fnv1a == kFnvPosition02);
  REQUIRE(pillar.GetScriptState()->sender == kHostActor);

  // the guest's game loads the temple and is granted it at the next epoch:
  // the pillar's record reaches it right ahead of the grant
  p.Messages().clear();
  {
    LogCapture log;
    Loaded(p, 1, kTemple, true);
    REQUIRE(log.Count("[verb:scripts] script state aa8cb served to ff000fe9 "
                      "ahead of its grant of cell 371de: 1 script(s), seq 1, "
                      "view 'Trigger01'") == 1);
  }
  REQUIRE(p.GetCellHost(kTemple).hosted);
  REQUIRE(p.GetCellHost(kTemple).player == kGuestActor);
  const uint32_t next = p.GetCellHost(kTemple).epoch;
  REQUIRE(next == epoch + 1);
  const int served = IndexOf<ScriptStateApplyMessage>(
    p, 1,
    [](const ScriptStateApplyMessage& m) { return m.formId == kPillar; });
  const int granted = IndexOf<HostGrantMessage>(
    p, 1, [](const HostGrantMessage& m) { return m.cell == kTemple; });
  REQUIRE(served >= 0);
  REQUIRE(granted > served);
  const auto applies = SentTo<ScriptStateApplyMessage>(p, 1);
  REQUIRE(applies.size() == 1);
  REQUIRE(applies[0].fnv1a == kFnvPosition02);
  REQUIRE(applies[0].seq == 1);
  REQUIRE(applies[0].event == std::optional<std::string>("Trigger01"));
  // the old host hears nothing of it
  REQUIRE(SentTo<ScriptStateApplyMessage>(p, 0).empty());

  // the grant ended the release read: the old host's late read at its old
  // epoch stands behind the cell's, and the record stands
  {
    LogCapture log;
    Report(p, 0, Record(epoch, kPillar, "position01", kFnvPosition01));
    REQUIRE(log.Count(fmt::format(
              "[verb:scripts] E_SCRIPTSTATE_EPOCH: ff000fe8 reports the "
              "script state of aa8cb (1 script(s), fnv 2b7e1516, epoch {}): "
              "cell 371de stands at epoch {}, the record names {}; refused",
              epoch, next, epoch)) == 1);
  }
  REQUIRE(pillar.GetScriptState()->fnv1a == kFnvPosition02);
  // the new host's own read at its epoch is recorded
  Report(p, 1, Record(next, kPillar, "position01", kFnvPosition01));
  REQUIRE(pillar.GetScriptState()->sender == kGuestActor);
  REQUIRE(pillar.GetScriptState()->epoch == next);
  Leave(p);
}

TEST_CASE("A stored record's sequence seeds the object's at its serve, so "
          "after a restart the next relay and the next record take the "
          "number after the record's",
          "[ScriptState][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = Enter(p);
  auto& pillar = p.worldState.GetFormAt<MpObjectReference>(kPillar);
  // the record a restart loaded, while the fresh server's sequence for the
  // pillar is 0
  pillar.SetScriptState(Saved(epoch));
  REQUIRE(p.GetScriptCalls().seq_of(kPillar) == 0);

  // a late game is served the record as it was taken, and the serve seeds
  // the pillar's sequence
  DoConnect(p, 2);
  p.CreateActor(kLateActor, pillar.GetPos() + NiPoint3{ 100.f, 0.f, 0.f }, 0,
                kTemple);
  p.SetUserActor(2, kLateActor);
  const auto applies = SentTo<ScriptStateApplyMessage>(p, 2);
  REQUIRE(applies.size() == 1);
  REQUIRE(applies[0].seq == 5);
  REQUIRE(applies[0].event == std::optional<std::string>("Trigger02"));
  REQUIRE(p.GetScriptCalls().seq_of(kPillar) == 5);

  // the host's next turn is relayed with the number after the record's, and
  // its read after the turn is stamped with it and with the turn's event
  p.Messages().clear();
  Turn(p, 0, epoch, "Trigger03", "Turned03");
  const auto relayed = SnippetsTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["seq"] == 6);
  Report(p, 0, Record(epoch, kPillar, "position03"));
  REQUIRE(pillar.GetScriptState()->seq == 6);
  REQUIRE(pillar.GetScriptState()->event ==
          std::optional<std::string>("Trigger03"));

  DoDisconnect(p, 2);
  p.DestroyActor(kLateActor);
  Leave(p);
}

TEST_CASE("A relay seeds its subject's sequence from the stored record when "
          "no serve did, so a host's first turn after a restart is numbered "
          "above the record's",
          "[ScriptState][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t epoch = Enter(p);
  auto& pillar = p.worldState.GetFormAt<MpObjectReference>(kPillar);
  pillar.SetScriptState(Saved(epoch));
  REQUIRE(p.GetScriptCalls().seq_of(kPillar) == 0);
  Turn(p, 0, epoch, "Trigger03", "Turned03");
  const auto relayed = SnippetsTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0]["seq"] == 6);
  REQUIRE(p.GetScriptCalls().seq_of(kPillar) == 6);
  Leave(p);
}
