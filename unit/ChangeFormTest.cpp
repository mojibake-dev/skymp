#include "ActionListener.h"
#include "CellLoadedMessage.h"
#include "ChangeFormAckMessage.h"
#include "ChangeFormApplyMessage.h"
#include "ChangeFormMessage.h"
#include "CreateActorMessage.h"
#include "MsgType.h"
#include "TestUtils.hpp"
#include "wire_bridge_cxx/rules.h"
#include <catch2/catch_all.hpp>
#include <chrono>
#include <simdjson.h>

PartOne& GetPartOne();

// thuum docs/verbs/change-form.md (ADR-065 system 1): a host's change form
// for an actor it simulates is recorded on the actor, acked by its hash,
// served at once to the other game listening and again to a game that
// subscribes later, and it survives a save and a load of the actor's change
// form. The stage is ActorDeathTest's: the forest's predator 0x85547 in
// Tamriel's cell 0x9810, the players beside it. The wire's own refusals (the
// header, the hash, the rows) are wire-validate's tests; here the server's:
// a non-owner and a stale epoch.

namespace {
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kForestCell = 0x9810;
constexpr uint32_t kHost = 0xff000e21;
constexpr uint32_t kOther = 0xff000e22;
constexpr uint32_t kLate = 0xff000e23;
constexpr uint32_t kPredator = 0x85547;
const NiPoint3 kForest{ 123000.f, -80000.f, 10705.f };

// a packed ACHR record of four data bytes: the header byte (type 1, size
// code 0), the version 0x4e, length1 4, length2 0, then a kind-0 id (index
// 5) and a pad byte; its hex as the native spells it
const std::string kRecordHex = "014e0400"
                               "000005"
                               "7f";

uint32_t Fnv1a(const std::string& hex)
{
  uint32_t h = 0x811c9dc5u;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    const uint8_t b =
      static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16));
    h ^= b;
    h *= 0x01000193u;
  }
  return h;
}

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

MpActor& Arrive(PartOne& p, Networking::UserId user, uint32_t id)
{
  DoConnect(p, user);
  p.CreateActor(id, kForest, 0, kTamriel);
  p.SetUserActor(user, id);
  auto& actor = p.worldState.GetFormAt<MpActor>(id);
  actor.ForceSubscriptionsUpdate();
  p.TickCellHosts(NowMs());
  return actor;
}

MpActor& Standing(PartOne& p, uint32_t refrId)
{
  auto& form = p.worldState.LookupFormByIdNoLoad(refrId);
  REQUIRE(form);
  MpActor* actor = form->AsActor();
  REQUIRE(actor);
  if (actor->IsDead()) {
    actor->Respawn(false);
  }
  p.worldState.fateRespawnDue.erase(refrId);
  return *actor;
}

void Host(PartOne& p, Networking::UserId user, uint32_t cell)
{
  const auto past = NowMs() + skymp::rules::host_grace_ms() + 2'000;
  p.TickCellHosts(past);
  RawMessageData raw;
  raw.userId = user;
  CellLoadedMessage loaded;
  loaded.cell = cell;
  loaded.loaded = true;
  p.GetActionListener().OnCellLoaded(raw, loaded);
  p.TickCellHosts(past);
}

ChangeFormMessage Record(uint32_t epoch, uint32_t formId,
                         const std::string& hex)
{
  ChangeFormMessage msg;
  msg.epoch = epoch;
  msg.formId = formId;
  msg.formType = 1;
  msg.flags = 0x04000427;
  msg.version = 0x4e;
  msg.body = hex;
  msg.fnv1a = Fnv1a(hex);
  ChangeFormRemapRow row;
  row.offset = 4;
  row.formId = 0x0200abcd;
  row.index = 5;
  msg.remap.push_back(row);
  return msg;
}

void Report(PartOne& p, Networking::UserId user, const ChangeFormMessage& msg)
{
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnChangeForm(raw, msg);
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
// for which `pick` holds, or -1
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

void Leave(PartOne& p, Networking::UserId user, uint32_t id)
{
  DoDisconnect(p, user);
  p.DestroyActor(id);
}
}

TEST_CASE("A host's change form for an actor it simulates is recorded on the "
          "actor, acked by its hash and served at once to the other game "
          "listening",
          "[ChangeForm][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& host = Arrive(p, 0, kHost);
  Arrive(p, 1, kOther);
  auto& actor = Standing(p, kPredator);
  actor.SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  actor.SetChangeFormRecord(std::nullopt);
  Host(p, 0, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &host);
  const auto cellHost = p.GetCellHost(kForestCell);
  REQUIRE(cellHost.hosted);
  REQUIRE(cellHost.player == kHost);
  p.Messages().clear();

  const auto msg = Record(cellHost.epoch, kPredator, kRecordHex);
  Report(p, 0, msg);

  const auto record = actor.GetChangeFormRecord();
  REQUIRE(record.has_value());
  REQUIRE(record->bodyHex == kRecordHex);
  REQUIRE(record->fnv1a == Fnv1a(kRecordHex));
  REQUIRE(record->epoch == cellHost.epoch);
  REQUIRE(record->flags == 0x04000427);
  REQUIRE(record->formType == 1);
  REQUIRE(record->version == 0x4e);
  REQUIRE(record->sender == kHost);
  REQUIRE(record->atMs > 0);
  REQUIRE(record->remap.size() == 1);
  REQUIRE(record->remap[0].offset == 4);
  REQUIRE(record->remap[0].formId == 0x0200abcd);
  REQUIRE(record->remap[0].index == 5);

  // the receipt to the owner alone, the hash it compares against
  const auto acks = SentTo<ChangeFormAckMessage>(p, 0);
  REQUIRE(acks.size() == 1);
  REQUIRE(acks[0].formId == kPredator);
  REQUIRE(acks[0].fnv1a == Fnv1a(kRecordHex));
  REQUIRE(SentTo<ChangeFormAckMessage>(p, 1).empty());

  // the record at once to the other game listening, never back to the owner
  const auto applies = SentTo<ChangeFormApplyMessage>(p, 1);
  REQUIRE(applies.size() == 1);
  REQUIRE(applies[0].formId == kPredator);
  REQUIRE(applies[0].body == kRecordHex);
  REQUIRE(applies[0].formType == 1);
  REQUIRE(applies[0].flags == 0x04000427);
  REQUIRE(applies[0].version == 0x4e);
  REQUIRE(applies[0].remap.size() == 1);
  REQUIRE(applies[0].remap[0] == msg.remap[0]);
  REQUIRE(SentTo<ChangeFormApplyMessage>(p, 0).empty());

  // the next record replaces the first and goes out again
  p.Messages().clear();
  const std::string second = "014e0400"
                             "000006"
                             "7e";
  Report(p, 0, Record(cellHost.epoch, kPredator, second));
  REQUIRE(actor.GetChangeFormRecord()->bodyHex == second);
  REQUIRE(SentTo<ChangeFormAckMessage>(p, 0).size() == 1);
  REQUIRE(SentTo<ChangeFormAckMessage>(p, 0)[0].fnv1a == Fnv1a(second));
  REQUIRE(SentTo<ChangeFormApplyMessage>(p, 1).size() == 1);

  actor.SetChangeFormRecord(std::nullopt);
  Leave(p, 0, kHost);
  Leave(p, 1, kOther);
}

TEST_CASE("A change form from a game that does not host the actor's cell, or "
          "at a stale epoch, is refused: no record, no ack, nothing served",
          "[ChangeForm][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& host = Arrive(p, 0, kHost);
  Arrive(p, 1, kOther);
  auto& actor = Standing(p, kPredator);
  actor.SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  actor.SetChangeFormRecord(std::nullopt);
  Host(p, 0, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &host);
  const auto cellHost = p.GetCellHost(kForestCell);
  p.Messages().clear();

  // the other game is a visitor, not the host
  Report(p, 1, Record(cellHost.epoch, kPredator, kRecordHex));
  REQUIRE(!actor.GetChangeFormRecord().has_value());
  REQUIRE(SentTo<ChangeFormAckMessage>(p, 1).empty());
  REQUIRE(SentTo<ChangeFormApplyMessage>(p, 0).empty());

  // the host at an epoch the cell has left behind, and at one ahead
  Report(p, 0, Record(cellHost.epoch - 1, kPredator, kRecordHex));
  Report(p, 0, Record(cellHost.epoch + 1, kPredator, kRecordHex));
  REQUIRE(!actor.GetChangeFormRecord().has_value());
  REQUIRE(SentTo<ChangeFormAckMessage>(p, 0).empty());
  REQUIRE(SentTo<ChangeFormApplyMessage>(p, 1).empty());

  // a player's actor, and an actor the server never held
  Report(p, 0, Record(cellHost.epoch, kOther, kRecordHex));
  Report(p, 0, Record(cellHost.epoch, 0x3e728, kRecordHex));
  REQUIRE(SentTo<ChangeFormAckMessage>(p, 0).empty());

  // the host at the standing epoch is taken, so the refusals above were the
  // premises and not the stage
  Report(p, 0, Record(cellHost.epoch, kPredator, kRecordHex));
  REQUIRE(actor.GetChangeFormRecord().has_value());
  REQUIRE(SentTo<ChangeFormAckMessage>(p, 0).size() == 1);

  actor.SetChangeFormRecord(std::nullopt);
  Leave(p, 0, kHost);
  Leave(p, 1, kOther);
}

TEST_CASE("A game that subscribes to the actor after the record exists hears "
          "the record behind the actor's create word, and again when it "
          "reports the cell loaded",
          "[ChangeForm][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& host = Arrive(p, 0, kHost);
  auto& actor = Standing(p, kPredator);
  actor.SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  actor.SetChangeFormRecord(std::nullopt);
  Host(p, 0, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &host);
  const auto cellHost = p.GetCellHost(kForestCell);
  Report(p, 0, Record(cellHost.epoch, kPredator, kRecordHex));
  REQUIRE(actor.GetChangeFormRecord().has_value());
  p.Messages().clear();

  // the late game arrives beside the predator: its create word, then the
  // record on the same ordered channel
  Arrive(p, 2, kLate);
  const int created = IndexOf<CreateActorMessage>(
    p, 2, [](const CreateActorMessage& m) { return m.refrId == kPredator; });
  const int applied =
    IndexOf<ChangeFormApplyMessage>(p, 2, [](const ChangeFormApplyMessage& m) {
      return m.formId == kPredator;
    });
  REQUIRE(created >= 0);
  REQUIRE(applied > created);
  const auto applies = SentTo<ChangeFormApplyMessage>(p, 2);
  REQUIRE(applies.size() == 1);
  REQUIRE(applies[0].body == kRecordHex);
  REQUIRE(applies[0].remap.size() == 1);
  // the host hears nothing of its own record
  REQUIRE(SentTo<ChangeFormApplyMessage>(p, 0).empty());

  // the late game's engine attaches the cell: the cell's records once more
  p.Messages().clear();
  RawMessageData raw;
  raw.userId = 2;
  CellLoadedMessage loaded;
  loaded.cell = kForestCell;
  loaded.loaded = true;
  p.GetActionListener().OnCellLoaded(raw, loaded);
  const auto again = SentTo<ChangeFormApplyMessage>(p, 2);
  REQUIRE(again.size() == 1);
  REQUIRE(again[0].body == kRecordHex);
  // an unload serves nothing
  p.Messages().clear();
  loaded.loaded = false;
  p.GetActionListener().OnCellLoaded(raw, loaded);
  REQUIRE(SentTo<ChangeFormApplyMessage>(p, 2).empty());

  actor.SetChangeFormRecord(std::nullopt);
  Leave(p, 0, kHost);
  Leave(p, 2, kLate);
}

TEST_CASE("A change-form record survives a save and a load of the actor's "
          "change form, and an older record without one reads as none",
          "[ChangeForm]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;
  ChangeFormRecord record;
  record.epoch = 7;
  record.flags = 0x04000427;
  record.version = 0x4e;
  record.formType = 1;
  record.bodyHex = kRecordHex;
  record.fnv1a = Fnv1a(kRecordHex);
  ChangeFormRemapRow row;
  row.offset = 4;
  row.formId = 0x0200abcd;
  row.index = 5;
  record.remap.push_back(row);
  record.sender = kHost;
  record.atMs = 1762200000123ull;
  changeForm.changeFormRecord = record;

  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  const auto loaded = MpChangeForm::JsonToChangeForm(element);
  REQUIRE(loaded.changeFormRecord.has_value());
  REQUIRE(*loaded.changeFormRecord == record);

  // the record travels with the actor's change form as a whole
  MpActor actor(LocationalData(), FormCallbacks::DoNothing(), 0xff000000);
  actor.ApplyChangeForm(loaded);
  REQUIRE(actor.GetChangeFormRecord().has_value());
  REQUIRE(*actor.GetChangeFormRecord() == record);
  REQUIRE(actor.GetChangeFormRecord()->BodyLength() == 8);

  // a record written before the field: no key, read as none
  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("changeFormRecord"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement).changeFormRecord);
}

// thuum D-S3 (traps follow the interaction; ADR-068's DestructionStage row):
// a reference's destruction record (the stage its engine reached, the
// destroyed flag, from the first seat to report each step) survives a save
// and a load of the reference's change form and travels with it as a whole;
// a record written before the field (no key) reads as none, the game files'
// state (rule 6's migration)
TEST_CASE("A destruction record survives a save and a load of the "
          "reference's change form, and an older record without one reads "
          "as the game files' state",
          "[ChangeForm][Traps]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::REFR;
  MpChangeFormREFR::Destruction destruction;
  destruction.stage = 2;
  destruction.destroyed = true;
  changeForm.destruction = destruction;

  simdjson::dom::parser parser;
  const nlohmann::json saved = MpChangeForm::ToJson(changeForm);
  REQUIRE(saved.contains("destruction"));
  REQUIRE(saved["destruction"]["stage"] == 2);
  REQUIRE(saved["destruction"]["destroyed"] == true);
  const std::string dump = saved.dump();
  auto element = parser.parse(dump).value();
  const auto loaded = MpChangeForm::JsonToChangeForm(element);
  REQUIRE(loaded.destruction.has_value());
  REQUIRE(*loaded.destruction == destruction);

  // the record travels with the reference's change form as a whole
  MpObjectReference refr(LocationalData(), FormCallbacks::DoNothing(), 0,
                         "ACTI");
  REQUIRE(!refr.GetDestruction().has_value());
  refr.ApplyChangeForm(loaded);
  REQUIRE(refr.GetDestruction().has_value());
  REQUIRE(*refr.GetDestruction() == destruction);

  // a stage alone, not destroyed, keeps both fields
  MpChangeForm staged;
  staged.recType = MpChangeForm::REFR;
  staged.destruction = MpChangeFormREFR::Destruction{ 1, false };
  const std::string stagedDump = MpChangeForm::ToJson(staged).dump();
  auto stagedElement = parser.parse(stagedDump).value();
  const auto stagedLoaded = MpChangeForm::JsonToChangeForm(stagedElement);
  REQUIRE(stagedLoaded.destruction == staged.destruction);

  // a record written before the field: no key, read as none
  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("destruction"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement).destruction);
}

// the fifty fixes' D-C3 A (PENDING ELI; thuum docs/research/
// fifty-fixes-2026-11-07.md section 1(c), the record): a world object's graph
// variables survive a save and a load of its change form, each with its kind,
// its one value and its sequence, an optional key (rule 6's migration: a
// record written before it reads as none). fToggleBlend is the cobweb
// 0x3758c's (FXtriggerAmbFX's tear, playtest 49); the int and the bool are
// illustrative
TEST_CASE("A world object's graph variables survive a save and a load of its "
          "change form, and an older record without them reads as none",
          "[ChangeForm][ScriptCall]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::REFR;
  MpChangeFormREFR::GraphVariable blend;
  blend.name = "fToggleBlend";
  blend.k = 4;
  blend.f = 1.f;
  blend.seq = 1;
  MpChangeFormREFR::GraphVariable state;
  state.name = "iState";
  state.k = 3;
  state.i = -2;
  state.seq = 3;
  MpChangeFormREFR::GraphVariable open;
  open.name = "bOpen";
  open.k = 5;
  open.b = true;
  open.seq = 2;
  changeForm.graphVariables =
    std::vector<MpChangeFormREFR::GraphVariable>{ blend, state, open };

  simdjson::dom::parser parser;
  const nlohmann::json saved = MpChangeForm::ToJson(changeForm);
  REQUIRE(saved.contains("graphVariables"));
  const auto& entries = saved["graphVariables"]["entries"];
  REQUIRE(entries.size() == 3);
  REQUIRE(entries[0]["name"] == "fToggleBlend");
  REQUIRE(entries[0]["k"] == 4);
  REQUIRE(entries[0]["f"] == 1.0);
  REQUIRE(entries[0]["seq"] == 1);
  // the one value its kind carries, no other
  REQUIRE(!entries[0].contains("i"));
  REQUIRE(!entries[0].contains("b"));
  REQUIRE(entries[1]["i"] == -2);
  REQUIRE(entries[2]["b"] == true);
  const std::string dump = saved.dump();
  auto element = parser.parse(dump).value();
  const auto loaded = MpChangeForm::JsonToChangeForm(element);
  REQUIRE(loaded.graphVariables == changeForm.graphVariables);

  // the record travels with the reference's change form as a whole
  MpObjectReference refr(LocationalData(), FormCallbacks::DoNothing(), 0,
                         "ACTI");
  REQUIRE(refr.GetGraphVariables().empty());
  refr.ApplyChangeForm(loaded);
  REQUIRE(refr.GetGraphVariables() == *changeForm.graphVariables);

  // a record written before the field: no key, read as none
  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("graphVariables"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement).graphVariables);
}

// the record's slot rule (wire-rules natives::variable_slot) as the
// reference keeps a variable: its own name in place (without case, the
// engine's BSFixedString compare), an older call left, a new name appended
// up to sixteen and in the oldest's place after
TEST_CASE("A reference keeps one entry per graph variable and at most "
          "sixteen, a new name past them in the oldest's place",
          "[ChangeForm][ScriptCall]")
{
  MpObjectReference refr(LocationalData(), FormCallbacks::DoNothing(), 0,
                         "ACTI");
  const auto variable = [](const std::string& name, float value,
                           uint32_t seq) {
    MpChangeFormREFR::GraphVariable out;
    out.name = name;
    out.k = 4;
    out.f = value;
    out.seq = seq;
    return out;
  };
  REQUIRE(refr.KeepGraphVariable(variable("fToggleBlend", 1.f, 1)));
  // the same variable in another case, later: in its place
  REQUIRE(refr.KeepGraphVariable(variable("FTOGGLEBLEND", 0.5f, 2)));
  REQUIRE(refr.GetGraphVariables().size() == 1);
  REQUIRE(refr.GetGraphVariables()[0].name == "FTOGGLEBLEND");
  REQUIRE(refr.GetGraphVariables()[0].f == std::optional<float>(0.5f));
  // an older call of it changes nothing
  REQUIRE_FALSE(refr.KeepGraphVariable(variable("fToggleBlend", 0.f, 1)));
  REQUIRE(refr.GetGraphVariables()[0].seq == 2);
  // fifteen more names fill the record, the seventeenth takes the oldest's
  // place, the first entry's (sequence 2)
  for (uint32_t n = 0; n < 15; ++n) {
    REQUIRE(refr.KeepGraphVariable(
      variable("fVar" + std::to_string(n), 1.f, 3 + n)));
  }
  REQUIRE(refr.GetGraphVariables().size() == 16);
  REQUIRE(refr.KeepGraphVariable(variable("fLast", 1.f, 40)));
  const auto kept = refr.GetGraphVariables();
  REQUIRE(kept.size() == 16);
  REQUIRE(kept[0].name == "fLast");
  REQUIRE(kept[0].seq == 40);
  REQUIRE(kept[1].name == "fVar0");
}

// thuum ADR-068 rung 4 (the state rows; docs/research/
// rung4-state-rows-2026-11-07.md section 1(c), Records): the ref state's
// recorded mark, the standing's crime faction and relationships, a script
// set's placement and the journal's alias fills and world globals survive a
// save and a load of the change form, each an optional key (rule 6's
// migration: a record written before them reads as none). The forms are
// FormDescs already, so they are written as "<hex>:<plugin>" with or without
// the running order. The ids are MS13 0x39645's as Skyrim.esm holds them
// (lab/esm.py, 2026-11-06: the web 0x1fc43 its ArvelWeb alias 10, the claw
// 0x39647 its GoldenClaw alias 11, created in Arvel 0x39655) and the
// census's cobweb explosion 0xc0c55; the faction, the other actor and the
// global are illustrative ids
TEST_CASE("Rung 4's records survive a save and a load of the change form, "
          "and a record written before them reads as none",
          "[ChangeForm][RefState]")
{
  const std::string kEsm = "Skyrim.esm";
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::REFR;
  changeForm.isDisabled = true;
  changeForm.refStateSeq = 7;
  changeForm.crimeFaction = FormDesc(0x28170, kEsm);
  changeForm.relationships = std::vector<MpChangeFormREFR::Relationship>{
    { FormDesc(0x1347b, kEsm), -2 }, { FormDesc(0x39655, kEsm), 3 }
  };
  changeForm.placedByScript =
    MpChangeFormREFR::PlacedByScript{ FormDesc(0x1fc43, kEsm),
                                      FormDesc(0xc0c55, kEsm) };
  MpChangeFormREFR::AliasFill web;
  web.quest = FormDesc(0x39645, kEsm);
  web.alias = 10;
  web.refr = FormDesc(0x1fc43, kEsm);
  MpChangeFormREFR::AliasFill cleared;
  cleared.quest = FormDesc(0x39645, kEsm);
  cleared.alias = 3;
  cleared.refr = FormDesc();
  MpChangeFormREFR::AliasFill claw;
  claw.quest = FormDesc(0x39645, kEsm);
  claw.alias = 11;
  claw.created = FormDesc(0x39647, kEsm);
  claw.container = FormDesc(0x39655, kEsm);
  claw.added = true;
  changeForm.aliasFills =
    std::vector<MpChangeFormREFR::AliasFill>{ web, cleared, claw };
  changeForm.worldGlobals =
    std::vector<PlayerGlobal>{ { FormDesc(0x5e3bd, kEsm), 7.f } };
  // an emptied faction list is kept as empty, so a removal survives
  changeForm.factions = std::vector<Faction>();

  simdjson::dom::parser parser;
  const nlohmann::json saved = MpChangeForm::ToJson(changeForm);
  REQUIRE(saved["refStateSeq"] == 7);
  REQUIRE(saved["crimeFaction"] == "28170:Skyrim.esm");
  REQUIRE(saved["relationships"]["entries"].size() == 2);
  REQUIRE(saved["relationships"]["entries"][0]["rank"] == -2);
  REQUIRE(saved["placedByScript"]["self"] == "1fc43:Skyrim.esm");
  REQUIRE(saved["placedByScript"]["base"] == "c0c55:Skyrim.esm");
  REQUIRE(saved["aliasFills"]["entries"].size() == 3);
  REQUIRE(saved["aliasFills"]["entries"][0]["refr"] == "1fc43:Skyrim.esm");
  REQUIRE(saved["aliasFills"]["entries"][1]["refr"] == "0");
  REQUIRE(!saved["aliasFills"]["entries"][0].contains("created"));
  REQUIRE(saved["aliasFills"]["entries"][2]["created"] == "39647:Skyrim.esm");
  REQUIRE(saved["aliasFills"]["entries"][2]["added"] == true);
  REQUIRE(!saved["aliasFills"]["entries"][2].contains("refr"));
  REQUIRE(saved["worldGlobals"]["entries"][0]["formDesc"] ==
          "5e3bd:Skyrim.esm");
  REQUIRE(saved["factions"]["entries"].empty());
  const std::string dump = saved.dump();
  auto element = parser.parse(dump).value();
  const auto loaded = MpChangeForm::JsonToChangeForm(element);
  REQUIRE(loaded.isDisabled);
  REQUIRE(loaded.refStateSeq == 7u);
  REQUIRE(loaded.crimeFaction == changeForm.crimeFaction);
  REQUIRE(loaded.relationships == changeForm.relationships);
  REQUIRE(loaded.placedByScript == changeForm.placedByScript);
  REQUIRE(loaded.aliasFills == changeForm.aliasFills);
  REQUIRE(loaded.worldGlobals == changeForm.worldGlobals);
  REQUIRE(loaded.factions.has_value());
  REQUIRE(loaded.factions->empty());
  // a clear's empty desc reads back as the empty desc it was, no plugin's
  REQUIRE(loaded.aliasFills->at(1).refr->shortFormId == 0);
  REQUIRE(loaded.aliasFills->at(1).refr->file.empty());

  // the crime faction's removal: the empty desc, kept apart from none
  MpChangeForm removed;
  removed.recType = MpChangeForm::ACHR;
  removed.crimeFaction = FormDesc();
  const std::string removedDump = MpChangeForm::ToJson(removed).dump();
  auto removedElement = parser.parse(removedDump).value();
  const auto removedLoaded = MpChangeForm::JsonToChangeForm(removedElement);
  REQUIRE(removedLoaded.crimeFaction == FormDesc());

  // a record written before the fields: no key, each read as none
  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  for (const char* key :
       { "refStateSeq", "crimeFaction", "relationships", "placedByScript",
         "aliasFills", "worldGlobals", "factions" }) {
    REQUIRE(!older.contains(key));
  }
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  const auto olderLoaded = MpChangeForm::JsonToChangeForm(olderElement);
  REQUIRE(!olderLoaded.refStateSeq);
  REQUIRE(!olderLoaded.crimeFaction);
  REQUIRE(!olderLoaded.relationships);
  REQUIRE(!olderLoaded.placedByScript);
  REQUIRE(!olderLoaded.aliasFills);
  REQUIRE(!olderLoaded.worldGlobals);
  REQUIRE(!olderLoaded.factions);
}

// thuum ADR-068 rung 3 (the script record): a reference's script record (the
// scripts its owner read at a quiescent point, with the object's sequence
// and the view the server stamped then) survives a save and a load of the
// reference's change form and travels with it as a whole; a record written
// before the field (no key) reads as none (rule 6's migration). The scripts
// are the Temple pillar 0xaa8cb's defaultPuzzlePillarScript as the census
// read it (docs/verbs/papyrus-events.md: five variables, raw types 3, 3, 5,
// a class, 5, in its turned state position02; the class one the puzzle
// lever 0xaa8c3's plain form handle, kind 0, mid 0xFFFF), and one value of
// each other kind the record carries: a typed None, a float, a string and
// three arrays (an int array, an object array with its element type, a None
// string array)
TEST_CASE("A script record survives a save and a load of the reference's "
          "change form, and an older record without one reads as none",
          "[ChangeForm][ScriptState]")
{
  const auto value = [](uint32_t k) {
    ScriptStateVar v;
    v.k = k;
    return v;
  };
  ScriptStateHandle lever;
  lever.kind = 0;
  lever.mid = 0xffff;
  lever.form = 0xaa8c3;
  lever.type = "defaultPillarPuzzleLever";

  ScriptStateObject pillar;
  pillar.type = "defaultPuzzlePillarScript";
  pillar.state = "position02";
  pillar.flags = 0x0b;
  pillar.vars.push_back(value(3));
  pillar.vars.back().i = 1;
  pillar.vars.push_back(value(3));
  pillar.vars.back().i = 2;
  pillar.vars.push_back(value(5));
  pillar.vars.back().b = true;
  pillar.vars.push_back(value(1));
  pillar.vars.back().o = lever;
  pillar.vars.push_back(value(5));
  pillar.vars.back().b = false;
  // a typed None, a float, a string
  pillar.vars.push_back(value(1));
  pillar.vars.push_back(value(4));
  pillar.vars.back().f = 1.5f;
  pillar.vars.push_back(value(2));
  pillar.vars.back().s = "Turned02";
  // an int array, an object array of a handle and a None, a None array
  ScriptStateValue one;
  one.k = 3;
  one.i = 1;
  ScriptStateValue three;
  three.k = 3;
  three.i = 3;
  pillar.vars.push_back(value(13));
  pillar.vars.back().a = std::vector<ScriptStateValue>{ one, three };
  ScriptStateValue handle;
  handle.k = 1;
  handle.o = lever;
  ScriptStateValue none;
  none.k = 1;
  pillar.vars.push_back(value(11));
  pillar.vars.back().elem = "ObjectReference";
  pillar.vars.back().a = std::vector<ScriptStateValue>{ handle, none };
  pillar.vars.push_back(value(12));

  ScriptStateRecord record;
  record.epoch = 3;
  record.fnv1a = 0x5c1a7e01;
  record.sender = 0xff000fe5;
  record.atMs = 1762440000123ull;
  record.seq = 2;
  record.event = "Trigger02";
  record.scripts.push_back(pillar);

  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::REFR;
  changeForm.scriptState = record;

  simdjson::dom::parser parser;
  const nlohmann::json saved = MpChangeForm::ToJson(changeForm);
  REQUIRE(saved.contains("scriptState"));
  const nlohmann::json& kept = saved["scriptState"];
  REQUIRE(kept["epoch"] == 3);
  REQUIRE(kept["fnv1a"] == 0x5c1a7e01);
  REQUIRE(kept["sender"] == 0xff000fe5);
  REQUIRE(kept["at"] == 1762440000123ull);
  REQUIRE(kept["seq"] == 2);
  REQUIRE(kept["event"] == "Trigger02");
  REQUIRE(kept["scripts"].size() == 1);
  REQUIRE(kept["scripts"][0]["type"] == "defaultPuzzlePillarScript");
  REQUIRE(kept["scripts"][0]["state"] == "position02");
  REQUIRE(kept["scripts"][0]["flags"] == 0x0b);
  REQUIRE(kept["scripts"][0]["vars"].size() == 11);
  // an absent field writes no key, as the wire's JSON spells a variable
  REQUIRE(kept["scripts"][0]["vars"][0] ==
          nlohmann::json{ { "k", 3 }, { "i", 1 } });
  REQUIRE(kept["scripts"][0]["vars"][5] == nlohmann::json{ { "k", 1 } });
  REQUIRE(kept["scripts"][0]["vars"][3]["o"] ==
          nlohmann::json{ { "kind", 0 },
                          { "mid", 0xffff },
                          { "form", 0xaa8c3 },
                          { "type", "defaultPillarPuzzleLever" } });
  REQUIRE(kept["scripts"][0]["vars"][9]["elem"] == "ObjectReference");
  REQUIRE(kept["scripts"][0]["vars"][9]["a"].size() == 2);
  REQUIRE(kept["scripts"][0]["vars"][10] == nlohmann::json{ { "k", 12 } });

  const std::string dump = saved.dump();
  auto element = parser.parse(dump).value();
  const auto loaded = MpChangeForm::JsonToChangeForm(element);
  REQUIRE(loaded.scriptState.has_value());
  REQUIRE(*loaded.scriptState == record);

  // the record travels with the reference's change form as a whole
  MpObjectReference refr(LocationalData(), FormCallbacks::DoNothing(), 0,
                         "ACTI");
  REQUIRE(!refr.GetScriptState().has_value());
  refr.ApplyChangeForm(loaded);
  REQUIRE(refr.GetScriptState().has_value());
  REQUIRE(*refr.GetScriptState() == record);

  // a record taken before any animation reached the reference has no view,
  // and keeps none through the save
  MpChangeForm viewless;
  viewless.recType = MpChangeForm::REFR;
  ScriptStateRecord noView = record;
  noView.event.reset();
  viewless.scriptState = noView;
  const nlohmann::json savedViewless = MpChangeForm::ToJson(viewless);
  REQUIRE(!savedViewless["scriptState"].contains("event"));
  const std::string viewlessDump = savedViewless.dump();
  auto viewlessElement = parser.parse(viewlessDump).value();
  const auto viewlessLoaded = MpChangeForm::JsonToChangeForm(viewlessElement);
  REQUIRE(viewlessLoaded.scriptState ==
          std::optional<ScriptStateRecord>(noView));
  REQUIRE(!viewlessLoaded.scriptState->event.has_value());

  // a record written before the field: no key, read as none
  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("scriptState"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement).scriptState);
}

// thuum docs/verbs/npc-fate.md ("canon-4's merge review", finding 2): the
// record a host serialized for a body is that body's; the server's stand-up
// (RespawnFated) ends the body, so the record goes with it and no later
// arrival is served a corpse's record over the living copy
TEST_CASE("A fated respawn drops the body's held change-form record",
          "[ChangeForm][NpcFate][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& host = Arrive(p, 0, kHost);
  auto& actor = Standing(p, kPredator);
  actor.SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  actor.SetChangeFormRecord(std::nullopt);
  Host(p, 0, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &host);
  const auto cellHost = p.GetCellHost(kForestCell);
  REQUIRE(cellHost.hosted);
  actor.Kill(nullptr);
  REQUIRE(actor.IsDead());
  p.Messages().clear();
  Report(p, 0, Record(cellHost.epoch, kPredator, kRecordHex));
  REQUIRE(actor.GetChangeFormRecord().has_value());

  actor.RespawnFated();
  REQUIRE_FALSE(actor.IsDead());
  REQUIRE_FALSE(actor.GetChangeFormRecord().has_value());

  Leave(p, 0, kHost);
}

// physics rung 5a (ADR-071 decision 5): a body is a row of the prop table,
// its death's simulator's, while the change-form rule stays the cell's (Eli's
// D-PH11 A moves a body's record to the word the sender holds with SCHEMA 47,
// rung 5's): the host's record of a body whose live row it holds is still
// recorded at the cell's epoch, the row's epoch apart from it
TEST_CASE("A host's change form for a body whose live row it holds is "
          "recorded at the cell's epoch",
          "[ChangeForm][NpcFate][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  auto& host = Arrive(p, 0, kHost);
  auto& actor = Standing(p, kPredator);
  actor.SetPos(kForest + NiPoint3{ 100.f, 0.f, 0.f });
  actor.SetChangeFormRecord(std::nullopt);
  Host(p, 0, kForestCell);
  REQUIRE(p.worldState.HostOf(actor) == &host);
  const auto cellHost = p.GetCellHost(kForestCell);
  REQUIRE(cellHost.hosted);
  actor.Kill(nullptr);
  // a stand-up and a second death move the row's epoch two on, so the
  // record's epoch is the cell's alone
  if (p.PropEpochOf(actor) == cellHost.epoch) {
    actor.Respawn(false);
    actor.Kill(nullptr);
  }
  REQUIRE(actor.IsDead());
  REQUIRE(p.PropLiveOwner(actor) == kHost);
  REQUIRE(p.PropEpochOf(actor) != cellHost.epoch);

  p.Messages().clear();
  Report(p, 0, Record(cellHost.epoch, kPredator, kRecordHex));
  const auto record = actor.GetChangeFormRecord();
  REQUIRE(record.has_value());
  REQUIRE(record->epoch == cellHost.epoch);
  REQUIRE(record->sender == kHost);
  REQUIRE(SentTo<ChangeFormAckMessage>(p, 0).size() == 1);

  actor.SetChangeFormRecord(std::nullopt);
  actor.Respawn(false);
  p.worldState.fateRespawnDue.erase(kPredator);
  Leave(p, 0, kHost);
}
