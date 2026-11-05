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
