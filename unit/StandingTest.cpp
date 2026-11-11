#include "ActionListener.h"
#include "Messages.h"
#include "SpSnippet.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <map>
#include <set>
#include <simdjson.h>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>

PartOne& GetPartOne();

// thuum ADR-068 rung 4 (the state rows, W3; docs/research/
// rung4-state-rows-2026-11-07.md section 1(c), Standing): an NPC's simulator
// reports its scripts' SetFactionRank, ModFactionRank, RemoveFromFaction,
// RemoveFromAllFactions, SetCrimeFaction and SetRelationshipRank as batched
// state rows (ScriptCall 75); the server merges each into the NPC's record
// (its change form's factions, crimeFaction and relationships; wire-rules
// standing), relays the one applier that brings every other engine to the
// record's value at once with the object's sequence (SpSnippet::Relay), and
// serves a late seat the record's difference from the base behind the NPC's
// create word. The stage is Bleak Falls Temple 0x371de and its Arvel, as
// lab/esm.py read Skyrim.esm on sky-srv (2026-11-06; Update.esm and the three
// DLCs override neither record): the ACHR 0x39655 (MS13ArvelREF) at (3822,
// -3336, -1779), of the NPC_ 0x39646 (MS13Arvel, no template), whose SNAM
// rows are SpiderFaction 0x2997f and BanditAllyFaction 0xe0cd9, both at rank
// 0; the FACTs CrimeFactionImperial 0x28848 and CrimeFactionWhiterun
// 0x267ea; the ambush draugr 0x380b3, an ACHR (TrapKillTest's); and the
// portcullis 0x3b9f1, an ACTI (ScriptCallTest's), as an object that is no
// faction and no actor.
namespace {
using Arg = std::optional<
  std::variant<bool, double, std::string, SpSnippetObjectArgument>>;

constexpr uint32_t kHostActor = 0xff000c70;
constexpr uint32_t kGuestActor = 0xff000c71;
constexpr uint32_t kLateActor = 0xff000c72;
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kArvel = 0x39655;
constexpr uint32_t kSpider = 0x2997f;
constexpr uint32_t kBanditAlly = 0xe0cd9;
constexpr uint32_t kImperialCrime = 0x28848;
constexpr uint32_t kWhiterunCrime = 0x267ea;
constexpr uint32_t kDraugr = 0x380b3;
constexpr uint32_t kPortcullis = 0x3b9f1;
const NiPoint3 kByArvel{ 3822.f, -3236.f, -1779.f };

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// the host (user 0) and the guest (user 1) by Arvel, the host hosting the
// temple and both games reporting it loaded; NPCs on and Arvel loaded,
// simulated by the host (TrapKillTest's shape)
MpActor& Enter(PartOne& p)
{
  p.worldState.npcEnabled = true;
  DoConnect(p, 0);
  p.CreateActor(kHostActor, kByArvel, 0, kTemple);
  p.SetUserActor(0, kHostActor);
  DoConnect(p, 1);
  p.CreateActor(kGuestActor, kByArvel, 0, kTemple);
  p.SetUserActor(1, kGuestActor);
  for (uint32_t id : { kHostActor, kGuestActor }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos(kByArvel);
    actor.ForceSubscriptionsUpdate();
  }
  const auto past = NowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  for (Networking::UserId user : { 0u, 1u }) {
    RawMessageData raw;
    raw.userId = user;
    CellLoadedMessage loaded;
    loaded.cell = kTemple;
    loaded.loaded = true;
    p.GetActionListener().OnCellLoaded(raw, loaded);
  }
  p.TickCellHosts(past);
  REQUIRE(p.GetCellHost(kTemple).hosted);
  REQUIRE(p.GetCellHost(kTemple).player == kHostActor);
  auto& arvel = p.worldState.GetFormAt<MpActor>(kArvel);
  REQUIRE(p.SimulatorOf(arvel) == kHostActor);
  const auto& listeners = arvel.GetActorListeners();
  REQUIRE(std::count(listeners.begin(), listeners.end(),
                     &p.worldState.GetFormAt<MpActor>(kGuestActor)) == 1);
  p.Messages().clear();
  return arvel;
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kHostActor);
  p.DestroyActor(kGuestActor);
}

Arg ObjectArg(uint32_t formId, const char* type)
{
  return Arg(SpSnippetObjectArgument{ formId, type });
}

// a standing native the NPC's script called on Arvel, reported by `user`'s
// game (the epoch is read for no actor's row)
void Call(PartOne& p, Networking::UserId user, const char* fn,
          std::vector<Arg> args)
{
  ScriptCallMessage msg;
  msg.epoch = 0;
  msg.selfId = kArvel;
  msg.cls = "Actor";
  msg.fn = fn;
  msg.args = std::move(args);
  msg.caller = "StandingTest";
  msg.handler = "onInit";
  msg.latent = false;
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnScriptCall(raw, msg);
}

// the standing appliers on Arvel that went to `user` since the last clear
std::vector<SpSnippetMessage> AppliersTo(PartOne& p, Networking::UserId user)
{
  static const std::set<std::string> kAppliers = {
    "SetFactionRank", "RemoveFromFaction", "RemoveFromAllFactions",
    "SetCrimeFaction", "SetRelationshipRank"
  };
  const uint64_t self = SpSnippet::MakeLongFormId(&p.worldState, kArvel);
  std::vector<SpSnippetMessage> out;
  for (auto& m : p.Messages()) {
    auto typed = dynamic_cast<SpSnippetMessage*>(m.message.get());
    if (m.userId == user && typed && typed->selfId == self &&
        typed->class_ == "Actor" && kAppliers.count(typed->function)) {
      out.push_back(*typed);
    }
  }
  return out;
}

// an applier's object argument (`at`) and its number, as the receiver reads
// them
uint64_t ObjectOf(const SpSnippetMessage& m, size_t at)
{
  REQUIRE(m.arguments.size() > at);
  REQUIRE(m.arguments[at].has_value());
  const auto* object = std::get_if<SpSnippetObjectArgument>(&*m.arguments[at]);
  REQUIRE(object);
  return object->formId;
}

double NumberOf(const SpSnippetMessage& m, size_t at)
{
  REQUIRE(m.arguments.size() > at);
  REQUIRE(m.arguments[at].has_value());
  const auto* number = std::get_if<double>(&*m.arguments[at]);
  REQUIRE(number);
  return *number;
}

// the record's factions by form id
std::map<uint32_t, int> Ranks(PartOne& p, MpActor& npc)
{
  std::map<uint32_t, int> out;
  for (const auto& faction : npc.GetFactionRanks()) {
    out[faction.formDesc.ToFormId(p.worldState.espmFiles)] = faction.rank;
  }
  return out;
}

FormDesc Desc(PartOne& p, uint32_t formId)
{
  return FormDesc::FromFormId(formId, p.worldState.espmFiles);
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
    auto logger = std::make_shared<spdlog::logger>("standing-test", sink);
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

TEST_CASE("An NPC's simulator's SetFactionRank is recorded in its standing "
          "and relayed at once as the one applier with the object's "
          "sequence; a repeat folds, a guest's is no owner's",
          "[Standing][espm]")
{
  auto& p = GetPartOne();
  auto& arvel = Enter(p);
  // the base's rows, read at the first look
  REQUIRE(Ranks(p, arvel) ==
          std::map<uint32_t, int>{ { kSpider, 0 }, { kBanditAlly, 0 } });

  {
    LogCapture log;
    Call(p, 0, "SetFactionRank",
         { ObjectArg(kImperialCrime, "Faction"), Arg(2.0) });
    REQUIRE(log.Count("[verb:scripts] ff000c70 reports Actor.SetFactionRank "
                      "on 39655 (from StandingTest.onInit): its standing "
                      "recorded as Actor.SetFactionRank, seq 1, relayed to 1 "
                      "game(s)") == 1);
  }
  REQUIRE(Ranks(p, arvel) ==
          std::map<uint32_t, int>{
            { kSpider, 0 }, { kBanditAlly, 0 }, { kImperialCrime, 2 } });
  auto relayed = AppliersTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0].function == "SetFactionRank");
  REQUIRE(relayed[0].seq == std::optional<uint32_t>(1));
  REQUIRE(relayed[0].snippetIdx == int64_t{ 0xffffffff });
  REQUIRE(relayed[0].arguments.size() == 2);
  REQUIRE(ObjectOf(relayed[0], 0) == kImperialCrime);
  REQUIRE(std::get<SpSnippetObjectArgument>(*relayed[0].arguments[0]).type ==
          "Faction");
  REQUIRE(NumberOf(relayed[0], 1) == 2.0);
  // the reporter's own engine ran it
  REQUIRE(AppliersTo(p, 0).empty());

  // the same rank again: folded, nothing relayed
  p.Messages().clear();
  Call(p, 0, "SetFactionRank",
       { ObjectArg(kImperialCrime, "Faction"), Arg(2.0) });
  REQUIRE(AppliersTo(p, 1).empty());

  // the guest's engine does not simulate Arvel
  {
    LogCapture log;
    Call(p, 1, "SetFactionRank",
         { ObjectArg(kImperialCrime, "Faction"), Arg(4.0) });
    REQUIRE(log.Count("[verb:scripts] E_SCRIPTCALL_OWNER: ff000c71 reports "
                      "Actor.SetFactionRank on 39655") == 1);
  }
  REQUIRE(Ranks(p, arvel).at(kImperialCrime) == 2);
  REQUIRE(AppliersTo(p, 0).empty());
  Leave(p);
}

TEST_CASE("ModFactionRank moves a held rank within the int8 and relays the "
          "rank it reached; a step on a faction the record lacks folds; a "
          "rank outside -128 to 127 and an object that is no FACT are "
          "refused by name",
          "[Standing][espm]")
{
  auto& p = GetPartOne();
  auto& arvel = Enter(p);

  Call(p, 0, "ModFactionRank", { ObjectArg(kSpider, "Faction"), Arg(3.0) });
  auto relayed = AppliersTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0].function == "SetFactionRank");
  REQUIRE(ObjectOf(relayed[0], 0) == kSpider);
  REQUIRE(NumberOf(relayed[0], 1) == 3.0);
  REQUIRE(relayed[0].seq == std::optional<uint32_t>(1));

  // a step past the int8 stops at its end
  p.Messages().clear();
  Call(p, 0, "ModFactionRank", { ObjectArg(kSpider, "Faction"), Arg(127.0) });
  relayed = AppliersTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(NumberOf(relayed[0], 1) == 127.0);
  REQUIRE(relayed[0].seq == std::optional<uint32_t>(2));
  REQUIRE(Ranks(p, arvel).at(kSpider) == 127);

  // a faction the record lacks: HYPOTHESIS H-ST2, the engine leaves a
  // non-member out as the record does
  p.Messages().clear();
  Call(p, 0, "ModFactionRank",
       { ObjectArg(kWhiterunCrime, "Faction"), Arg(1.0) });
  REQUIRE(AppliersTo(p, 1).empty());
  REQUIRE(Ranks(p, arvel).count(kWhiterunCrime) == 0);

  {
    LogCapture log;
    Call(p, 0, "SetFactionRank",
         { ObjectArg(kSpider, "Faction"), Arg(200.0) });
    Call(p, 0, "SetFactionRank",
         { ObjectArg(kPortcullis, "Faction"), Arg(1.0) });
    REQUIRE(log.Count("[verb:scripts] E_STANDING_RANK: ff000c70 reports "
                      "Actor.SetFactionRank on 39655 (from "
                      "StandingTest.onInit): the rank 200 is outside -128 to "
                      "127; refused") == 1);
    REQUIRE(log.Count("[verb:scripts] E_STANDING_FACTION: ff000c70 reports "
                      "Actor.SetFactionRank on 39655 (from "
                      "StandingTest.onInit): 3b9f1 is no FACT record; "
                      "refused") == 1);
  }
  REQUIRE(AppliersTo(p, 1).empty());
  REQUIRE(Ranks(p, arvel).at(kSpider) == 127);
  REQUIRE(Ranks(p, arvel).count(kPortcullis) == 0);
  Leave(p);
}

TEST_CASE("RemoveFromFaction drops a base faction, and a seat that arrives "
          "later is served the record's difference from the base behind "
          "Arvel's create word",
          "[Standing][espm]")
{
  auto& p = GetPartOne();
  auto& arvel = Enter(p);

  Call(p, 0, "RemoveFromFaction", { ObjectArg(kBanditAlly, "Faction") });
  auto relayed = AppliersTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0].function == "RemoveFromFaction");
  REQUIRE(relayed[0].arguments.size() == 1);
  REQUIRE(ObjectOf(relayed[0], 0) == kBanditAlly);
  Call(p, 0, "SetFactionRank",
       { ObjectArg(kImperialCrime, "Faction"), Arg(2.0) });
  // a faction the record does not hold: folded
  p.Messages().clear();
  Call(p, 0, "RemoveFromFaction", { ObjectArg(kWhiterunCrime, "Faction") });
  REQUIRE(AppliersTo(p, 1).empty());
  REQUIRE(Ranks(p, arvel) ==
          std::map<uint32_t, int>{ { kSpider, 0 }, { kImperialCrime, 2 } });

  // the late seat: Arvel's create word, then the two appliers that bring its
  // engine's copy from the base to the record, with no sequence
  {
    LogCapture log;
    DoConnect(p, 2);
    p.CreateActor(kLateActor, kByArvel, 0, kTemple);
    p.SetUserActor(2, kLateActor);
    REQUIRE(log.Count("[verb:scripts] standing of 39655 served to ff000c72 "
                      "after its create word: 2 applier(s)") == 1);
  }
  const int created = IndexOf<CreateActorMessage>(
    p, 2, [](const CreateActorMessage& m) { return m.refrId == kArvel; });
  REQUIRE(created >= 0);
  const uint64_t arvelLong = SpSnippet::MakeLongFormId(&p.worldState, kArvel);
  const int firstServed =
    IndexOf<SpSnippetMessage>(p, 2, [&](const SpSnippetMessage& m) {
      return m.selfId == arvelLong &&
        (m.function == "SetFactionRank" || m.function == "RemoveFromFaction");
    });
  REQUIRE(firstServed > created);
  const auto served = AppliersTo(p, 2);
  REQUIRE(served.size() == 2);
  REQUIRE(served[0].function == "SetFactionRank");
  REQUIRE(ObjectOf(served[0], 0) == kImperialCrime);
  REQUIRE(NumberOf(served[0], 1) == 2.0);
  REQUIRE(!served[0].seq.has_value());
  REQUIRE(served[1].function == "RemoveFromFaction");
  REQUIRE(ObjectOf(served[1], 0) == kBanditAlly);
  REQUIRE(!served[1].seq.has_value());
  DoDisconnect(p, 2);
  p.DestroyActor(kLateActor);
  Leave(p);
}

TEST_CASE("SetCrimeFaction records a crime faction and its removal, and "
          "SetRelationshipRank a rank toward another actor; the reporter's "
          "own player and an object that is no actor are refused",
          "[Standing][espm]")
{
  auto& p = GetPartOne();
  auto& arvel = Enter(p);

  Call(p, 0, "SetCrimeFaction", { ObjectArg(kWhiterunCrime, "Faction") });
  REQUIRE(arvel.GetCrimeFactionRecord() ==
          std::optional<FormDesc>(Desc(p, kWhiterunCrime)));
  auto relayed = AppliersTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0].function == "SetCrimeFaction");
  REQUIRE(ObjectOf(relayed[0], 0) == kWhiterunCrime);

  // None, as the seat sends it (an object of form id 0): the removal, kept
  // as the empty desc and relayed as None
  p.Messages().clear();
  Call(p, 0, "SetCrimeFaction", { ObjectArg(0, "Faction") });
  REQUIRE(arvel.GetCrimeFactionRecord() ==
          std::optional<FormDesc>(FormDesc()));
  relayed = AppliersTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0].arguments.size() == 1);
  REQUIRE(!relayed[0].arguments[0].has_value());
  REQUIRE(relayed[0].seq == std::optional<uint32_t>(2));
  // None again: folded
  p.Messages().clear();
  Call(p, 0, "SetCrimeFaction", { ObjectArg(0, "Faction") });
  REQUIRE(AppliersTo(p, 1).empty());

  Call(p, 0, "SetRelationshipRank",
       { ObjectArg(kDraugr, "Actor"), Arg(-2.0) });
  relayed = AppliersTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0].function == "SetRelationshipRank");
  REQUIRE(ObjectOf(relayed[0], 0) ==
          SpSnippet::MakeLongFormId(&p.worldState, kDraugr));
  REQUIRE(NumberOf(relayed[0], 1) == -2.0);
  // the fold spent its own number
  REQUIRE(relayed[0].seq == std::optional<uint32_t>(4));
  const auto relationships = arvel.GetRelationships();
  REQUIRE(relationships.size() == 1);
  REQUIRE(relationships[0].other == Desc(p, kDraugr));
  REQUIRE(relationships[0].rank == -2);

  {
    LogCapture log;
    p.Messages().clear();
    Call(p, 0, "SetRelationshipRank", { ObjectArg(0x14, "Actor"), Arg(1.0) });
    Call(p, 0, "SetRelationshipRank",
         { ObjectArg(kPortcullis, "Actor"), Arg(1.0) });
    REQUIRE(log.Count("[verb:scripts] E_STANDING_OTHER: ff000c70 reports "
                      "Actor.SetRelationshipRank on 39655 (from "
                      "StandingTest.onInit): the reporter's own player") == 1);
    // the second refusal of the same code inside the minute is counted
    REQUIRE(log.Count("E_STANDING_OTHER") == 1);
  }
  REQUIRE(AppliersTo(p, 1).empty());
  REQUIRE(arvel.GetRelationships().size() == 1);
  Leave(p);
}

// thuum train 52 (the client builder's read of fork m4-train-52-client
// 24448f7b): a standing applier names a player's actor when a relayed
// fragment ran under that player's acting scope on the NPC's simulator (A
// Lovely Letter's Fragment_22, `SetRelationshipRank(Game.GetPlayer(), 2)` on
// Faendal: the seat sends the acting player's figure by its actor id, rung
// 5). That player's own game holds no view of its own actor, so the id
// resolves to nothing there and the rank is never applied, after a relog
// included; its engine numbers its player 0x14, as SerializeArguments and a
// snippet's self already map it (SpSnippet::ClientFormId, now MakeMessage's
// for every object argument). Here the host, Arvel's simulator, reports the
// relationship toward the guest's actor
TEST_CASE("A standing applier naming a player's actor reaches that player's "
          "own game as 0x14 and every other game as the actor's id, "
          "relayed and served behind the create word alike, after a relog "
          "too",
          "[Standing][espm]")
{
  auto& p = GetPartOne();
  auto& arvel = Enter(p);
  REQUIRE(SpSnippet::ClientFormId(
            kGuestActor, &p.worldState.GetFormAt<MpActor>(kGuestActor)) ==
          0x14);
  REQUIRE(SpSnippet::ClientFormId(
            kGuestActor, &p.worldState.GetFormAt<MpActor>(kHostActor)) ==
          kGuestActor);
  REQUIRE(SpSnippet::ClientFormId(kGuestActor, nullptr) == kGuestActor);
  REQUIRE(SpSnippet::ClientFormId(
            kArvel, &p.worldState.GetFormAt<MpActor>(kGuestActor)) == kArvel);

  Call(p, 0, "SetRelationshipRank",
       { ObjectArg(kGuestActor, "Actor"), Arg(2.0) });
  const auto relationships = arvel.GetRelationships();
  REQUIRE(relationships.size() == 1);
  REQUIRE(relationships[0].other == Desc(p, kGuestActor));
  REQUIRE(relationships[0].rank == 2);
  // the relay: the guest's own game reads its player
  const auto relayed = AppliersTo(p, 1);
  REQUIRE(relayed.size() == 1);
  REQUIRE(relayed[0].function == "SetRelationshipRank");
  REQUIRE(ObjectOf(relayed[0], 0) == 0x14);
  REQUIRE(NumberOf(relayed[0], 1) == 2.0);

  // a late seat: served behind Arvel's create word, the guest's actor by
  // its id
  DoConnect(p, 2);
  p.CreateActor(kLateActor, kByArvel, 0, kTemple);
  p.SetUserActor(2, kLateActor);
  const auto late = AppliersTo(p, 2);
  REQUIRE(late.size() == 1);
  REQUIRE(late[0].function == "SetRelationshipRank");
  REQUIRE(ObjectOf(late[0], 0) == kGuestActor);
  REQUIRE(NumberOf(late[0], 1) == 2.0);

  // the guest logs in again (a second login of the same actor is what a
  // reconnect does): its own game is served the record, its player as 0x14
  p.Messages().clear();
  p.SetUserActor(1, kGuestActor);
  const auto own = AppliersTo(p, 1);
  REQUIRE(own.size() == 1);
  REQUIRE(own[0].function == "SetRelationshipRank");
  REQUIRE(ObjectOf(own[0], 0) == 0x14);
  REQUIRE(NumberOf(own[0], 1) == 2.0);
  REQUIRE(!own[0].seq.has_value());
  DoDisconnect(p, 2);
  p.DestroyActor(kLateActor);
  Leave(p);
}

TEST_CASE("RemoveFromAllFactions empties the standing and a second folds; a "
          "restart keeps the empty record, never the base's rows, and serves "
          "it",
          "[Standing][espm]")
{
  MpChangeForm saved;
  {
    auto& p = GetPartOne();
    auto& arvel = Enter(p);
    Call(p, 0, "RemoveFromAllFactions", {});
    auto relayed = AppliersTo(p, 1);
    REQUIRE(relayed.size() == 1);
    REQUIRE(relayed[0].function == "RemoveFromAllFactions");
    REQUIRE(relayed[0].arguments.empty());
    p.Messages().clear();
    Call(p, 0, "RemoveFromAllFactions", {});
    REQUIRE(AppliersTo(p, 1).empty());
    REQUIRE(arvel.GetFactionRanks().empty());
    saved = arvel.GetChangeForm();
    Leave(p);
  }
  // the record through the save's own JSON: an empty list stays a list
  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(saved).dump();
  auto element = parser.parse(dump).value();
  const MpChangeForm loaded = MpChangeForm::JsonToChangeForm(element);
  REQUIRE(loaded.factions.has_value());
  REQUIRE(loaded.factions->empty());

  // the restarted server: Arvel attaches with the saved record, whose empty
  // list the base's rows never refill (MpActor::LoadFactions)
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  p.worldState.LoadChangeForm(loaded, p.CreateFormCallbacks());
  auto& arvel = p.worldState.GetFormAt<MpActor>(kArvel);
  REQUIRE(arvel.GetFactionRanks().empty());
  const auto appliers = arvel.StandingAppliers();
  REQUIRE(appliers.size() == 2);
  std::set<uint32_t> removed;
  for (const auto& applier : appliers) {
    REQUIRE(std::string(applier.function) == "RemoveFromFaction");
    removed.insert(applier.form.ToFormId(p.worldState.espmFiles));
  }
  REQUIRE(removed == std::set<uint32_t>{ kSpider, kBanditAlly });
}
