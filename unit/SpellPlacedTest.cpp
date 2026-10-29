#include "ActionListener.h"
#include "CreateActorMessage.h"
#include "DestroyActorMessage.h"
#include "MpChangeForms.h"
#include "MsgType.h"
#include "TestUtils.hpp"
#include "libespm/Loader.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <simdjson.h>
#include <vector>

PartOne& GetPartOne();

// thuum docs/verbs/spell-placed-object.md (ADR-057): a listed spell's
// accepted cast makes the server place the table's objects itself, posed
// from the caster, 250 ms into the open stream; every subscriber's
// CreateActor names the caster (placedBy) and the base's type; the set falls
// lingerS after the stream's end or the caster's leaving; a restart sweeps
// every such form. The table here names Skyrim.esm's own records, since the
// test's five-file load order lacks Apocalypse: Flames 0x12FCD (casting type
// 2, concentration; SpellCastTest's) is the listed spell, Firebolt 0x12FD0 a
// spell off the list, and the bases are the load order's first MSTT and ACTI,
// found by type so no id is guessed.

namespace {
constexpr uint32_t kTamriel = 0x3c;
constexpr uint32_t kCaster = 0xff000e02;
constexpr uint32_t kWatcher = 0xff000e03;
constexpr uint32_t kSwept = 0xff000e04;
constexpr uint32_t kKept = 0xff000e05;
constexpr uint32_t kFlames = 0x00012fcd;
constexpr uint32_t kFirebolt = 0x00012fd0;
const char* const kFlamesDesc = "12fcd:Skyrim.esm";

uint64_t NowMs()
{
  return static_cast<uint64_t>(
    std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch())
      .count());
}

// the load order's first record of `type`, as the settings write a form
FormDesc FirstOfType(PartOne& p, const char* type)
{
  const auto records = p.GetEspm().GetBrowser().GetDistinctRecordsByType(type);
  REQUIRE(!records.empty());
  const auto& first = records.front();
  REQUIRE(first.rec);
  return FormDesc::FromFormId(first.ToGlobalId(first.rec->GetId()),
                              p.worldState.espmFiles);
}

// the Raise Wall entry's shape on Skyrim.esm's records: the wall 192 ahead
// and 48 below, the halo 192 ahead at the caster's height
std::string Table(PartOne& p, int perCaster, int lingerS)
{
  const nlohmann::json table = nlohmann::json::array(
    { { { "spell", kFlamesDesc },
        { "anchor", "caster" },
        { "objects",
          nlohmann::json::array(
            { { { "base", FirstOfType(p, "MSTT").ToString() },
                { "type", "MSTT" },
                { "forward", 192 },
                { "dz", -48 } },
              { { "base", FirstOfType(p, "ACTI").ToString() },
                { "type", "ACTI" },
                { "forward", 192 },
                { "dz", 0 } } }) },
        { "lifetime",
          { { "whileHeld", true }, { "lingerS", lingerS }, { "maxS", 120 } } },
        { "perCaster", perCaster } } });
  return table.dump();
}

// the caster (user 0) at the origin facing east (yaw 90, so the wall goes
// to x + 192) with Flames in its right hand and Firebolt in its left, and the
// watcher (user 1) 300 units north, both in Tamriel
void Enter(PartOne& p, int perCaster = 2, int lingerS = 1)
{
  p.SetSpellPlacedObjectsSettings(Table(p, perCaster, lingerS));
  DoConnect(p, 0);
  p.CreateActor(kCaster, { 0, 0, 0 }, 90.f, kTamriel);
  p.SetUserActor(0, kCaster);
  DoConnect(p, 1);
  p.CreateActor(kWatcher, { 0, 300, 0 }, 0, kTamriel);
  p.SetUserActor(1, kWatcher);
  Equipment eq;
  eq.rightSpell = kFlames;
  eq.leftSpell = kFirebolt;
  p.worldState.GetFormAt<MpActor>(kCaster).SetEquipment(eq);
  p.Messages().clear();
}

// the caster's cast through the packet path (DoMessage), as its client's
// message arrives: `castingSource` the hand (0 left, 1 right, 3 instant),
// `end` the stream's end
void Cast(PartOne& p, uint32_t spell, int castingSource, bool end)
{
  DoMessage(
    p, 0,
    nlohmann::json{ { "t", static_cast<int>(MsgType::SpellCast) },
                    { "data",
                      { { "caster", 0x14 },
                        { "target", kWatcher },
                        { "spell", spell },
                        { "isDualCasting", false },
                        { "interruptCast", end },
                        { "castingSource", castingSource },
                        { "aimAngle", 0.f },
                        { "aimHeading", 0.f },
                        { "hasLaunch", false },
                        { "originX", 0.f },
                        { "originY", 0.f },
                        { "originZ", 0.f },
                        { "launchAngleX", 0.f },
                        { "launchAngleZ", 0.f },
                        { "actorAnimationVariables",
                          { { "booleans", nlohmann::json::array() },
                            { "floats", nlohmann::json::array() },
                            { "integers", nlohmann::json::array() } } } } } });
}

// the CreateActors naming a placer that went to `user` since the last clear
std::vector<CreateActorMessage> PlacedTo(PartOne& p, Networking::UserId user)
{
  std::vector<CreateActorMessage> out;
  for (auto& m : p.Messages()) {
    if (m.userId != user) {
      continue;
    }
    auto create = dynamic_cast<CreateActorMessage*>(m.message.get());
    if (create && create->props.placedBy.has_value()) {
      out.push_back(*create);
    }
  }
  return out;
}

// the DestroyActors that went to `user` since the last clear, by idx
std::vector<uint32_t> DestroyedTo(PartOne& p, Networking::UserId user)
{
  std::vector<uint32_t> out;
  for (auto& m : p.Messages()) {
    auto destroy = dynamic_cast<DestroyActorMessage*>(m.message.get());
    if (m.userId == user && destroy) {
      out.push_back(destroy->idx);
    }
  }
  return out;
}

uint32_t FormIdOf(const CreateActorMessage& create)
{
  REQUIRE(create.refrId.has_value());
  return static_cast<uint32_t>(*create.refrId);
}

// the shared world left as found: the caster gone (its set lingers and the
// clock runs it out), every placed form, deleted or standing, destroyed, the
// table emptied
void Leave(PartOne& p, const std::vector<uint32_t>& placed,
           bool casterGone = false)
{
  if (!casterGone) {
    DoDisconnect(p, 0);
  }
  p.TickPlacements(NowMs() + 600'000);
  REQUIRE(p.SpellPlacedSets() == 0);
  p.SetSpellPlacedObjectsSettings("[]");
  for (const uint32_t formId : placed) {
    if (p.worldState.LookupFormByIdNoLoad(formId)) {
      p.worldState.DestroyForm(formId);
    }
  }
  DoDisconnect(p, 1);
  p.DestroyActor(kCaster);
  p.DestroyActor(kWatcher);
}
}

TEST_CASE("The change form keeps the spell that placed a form, and older "
          "records read as none",
          "[SpellPlaced]")
{
  MpChangeForm changeForm;
  changeForm.placedBySpell = FormDesc::FromString(kFlamesDesc);

  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  REQUIRE(MpChangeForm::JsonToChangeForm(element).placedBySpell ==
          changeForm.placedBySpell);

  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("placedBySpell"));
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  REQUIRE(!MpChangeForm::JsonToChangeForm(olderElement).placedBySpell);
}

TEST_CASE("A listed spell's open stream places the table's objects 250 ms "
          "in, not before, posed from the caster and named to every "
          "subscriber, the caster's own game included",
          "[SpellPlaced][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  const FormDesc mstt = FirstOfType(p, "MSTT");
  const FormDesc acti = FirstOfType(p, "ACTI");

  const uint64_t t0 = NowMs();
  Cast(p, kFlames, 1, false);

  // short of the wait: nothing stands
  p.TickPlacements(t0 + 100);
  REQUIRE(PlacedTo(p, 1).empty());
  REQUIRE(p.SpellPlacedSets() == 0);

  // the wait passed with the stream open: the wall, then the halo
  p.TickPlacements(t0 + 2'000);
  REQUIRE(p.SpellPlacedSets() == 1);
  const auto toWatcher = PlacedTo(p, 1);
  REQUIRE(toWatcher.size() == 2);

  const auto& wall = toWatcher[0];
  REQUIRE(wall.props.placedBy == kCaster);
  REQUIRE(wall.baseRecordType == "MSTT");
  REQUIRE(wall.baseId == mstt.ToFormId(p.worldState.espmFiles));
  REQUIRE(wall.transform.worldOrCell == kTamriel);
  REQUIRE(wall.transform.pos[0] == Catch::Approx(192.f).margin(0.01));
  REQUIRE(wall.transform.pos[1] == Catch::Approx(0.f).margin(0.01));
  REQUIRE(wall.transform.pos[2] == Catch::Approx(-48.f).margin(0.01));
  REQUIRE(wall.transform.rot[2] == Catch::Approx(90.f).margin(0.01));
  REQUIRE(!wall.isMe);

  const auto& halo = toWatcher[1];
  REQUIRE(halo.props.placedBy == kCaster);
  REQUIRE(halo.baseRecordType == "ACTI");
  REQUIRE(halo.baseId == acti.ToFormId(p.worldState.espmFiles));
  REQUIRE(halo.transform.pos[0] == Catch::Approx(192.f).margin(0.01));
  REQUIRE(halo.transform.pos[1] == Catch::Approx(0.f).margin(0.01));
  REQUIRE(halo.transform.pos[2] == Catch::Approx(0.f).margin(0.01));
  REQUIRE(halo.transform.rot[2] == Catch::Approx(90.f).margin(0.01));

  // the caster's own game hears the same two, and skips them itself
  const auto toCaster = PlacedTo(p, 0);
  REQUIRE(toCaster.size() == 2);
  REQUIRE(toCaster[0].props.placedBy == kCaster);
  REQUIRE(FormIdOf(toCaster[0]) == FormIdOf(wall));
  REQUIRE(FormIdOf(toCaster[1]) == FormIdOf(halo));

  // the records carry the spell, for the sweep at load
  std::vector<uint32_t> placed;
  for (const auto& m : toWatcher) {
    placed.push_back(FormIdOf(m));
    auto& refr = p.worldState.GetFormAt<MpObjectReference>(placed.back());
    REQUIRE(refr.GetPlacedBySpell() == FormDesc::FromString(kFlamesDesc));
    REQUIRE(!refr.IsDeleted());
    REQUIRE(refr.GetFormId() >= 0xff000000);
  }
  REQUIRE(p.worldState.GetFormAt<MpObjectReference>(placed[0]).GetBaseType() ==
          "MSTT");
  REQUIRE(p.worldState.GetFormAt<MpObjectReference>(placed[1]).GetBaseType() ==
          "ACTI");

  // placed once
  p.Messages().clear();
  p.TickPlacements(t0 + 3'000);
  REQUIRE(PlacedTo(p, 1).empty());
  REQUIRE(p.SpellPlacedSets() == 1);
  Leave(p, placed);
}

TEST_CASE("A spell off the list places nothing, and a listed cast that ends "
          "inside the wait places nothing",
          "[SpellPlaced][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);

  const uint64_t t0 = NowMs();
  Cast(p, kFirebolt, 0, false);
  p.TickPlacements(t0 + 2'000);
  REQUIRE(PlacedTo(p, 1).empty());
  REQUIRE(p.SpellPlacedSets() == 0);

  Cast(p, kFlames, 1, false);
  Cast(p, kFlames, 1, true);
  p.TickPlacements(NowMs() + 2'000);
  REQUIRE(PlacedTo(p, 1).empty());
  REQUIRE(p.SpellPlacedSets() == 0);
  Leave(p, {});
}

TEST_CASE("A caster at its perCaster cap places no further set",
          "[SpellPlaced][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, 1);

  const uint64_t t0 = NowMs();
  Cast(p, kFlames, 1, false);
  p.TickPlacements(t0 + 2'000);
  REQUIRE(p.SpellPlacedSets() == 1);
  std::vector<uint32_t> placed;
  for (const auto& m : PlacedTo(p, 1)) {
    placed.push_back(FormIdOf(m));
  }
  REQUIRE(placed.size() == 2);

  // a second stream from the instant slot while the first holds
  p.Messages().clear();
  const uint64_t t1 = NowMs();
  Cast(p, kFlames, 3, false);
  p.TickPlacements(t1 + 2'000);
  REQUIRE(PlacedTo(p, 1).empty());
  REQUIRE(p.SpellPlacedSets() == 1);
  Leave(p, placed);
}

TEST_CASE("A set falls lingerS after its stream's end: every subscriber's "
          "DestroyActor, the forms deleted",
          "[SpellPlaced][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, 2, 1);

  const uint64_t t0 = NowMs();
  Cast(p, kFlames, 1, false);
  p.TickPlacements(t0 + 2'000);
  const auto created = PlacedTo(p, 1);
  REQUIRE(created.size() == 2);
  std::vector<uint32_t> placed;
  for (const auto& m : created) {
    placed.push_back(FormIdOf(m));
  }

  p.Messages().clear();
  const uint64_t t1 = NowMs();
  Cast(p, kFlames, 1, true);
  // inside the linger: standing
  p.TickPlacements(t1 + 400);
  REQUIRE(DestroyedTo(p, 1).empty());
  REQUIRE(p.SpellPlacedSets() == 1);
  REQUIRE(!p.worldState.GetFormAt<MpObjectReference>(placed[0]).IsDeleted());

  // past it: gone on every screen, the records deleted
  p.TickPlacements(t1 + 3'000);
  REQUIRE(p.SpellPlacedSets() == 0);
  const auto destroyedWatcher = DestroyedTo(p, 1);
  const auto destroyedCaster = DestroyedTo(p, 0);
  for (const auto& m : created) {
    REQUIRE(std::find(destroyedWatcher.begin(), destroyedWatcher.end(),
                      m.idx) != destroyedWatcher.end());
    REQUIRE(std::find(destroyedCaster.begin(), destroyedCaster.end(), m.idx) !=
            destroyedCaster.end());
  }
  for (const uint32_t formId : placed) {
    REQUIRE(p.worldState.GetFormAt<MpObjectReference>(formId).IsDeleted());
  }
  Leave(p, placed);
}

TEST_CASE("A caster that leaves takes its set with it after the linger",
          "[SpellPlaced][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, 2, 1);

  const uint64_t t0 = NowMs();
  Cast(p, kFlames, 1, false);
  p.TickPlacements(t0 + 2'000);
  const auto created = PlacedTo(p, 1);
  REQUIRE(created.size() == 2);
  std::vector<uint32_t> placed;
  for (const auto& m : created) {
    placed.push_back(FormIdOf(m));
  }

  p.Messages().clear();
  const uint64_t t1 = NowMs();
  DoDisconnect(p, 0);
  p.TickPlacements(t1 + 400);
  REQUIRE(p.SpellPlacedSets() == 1);
  p.TickPlacements(t1 + 3'000);
  REQUIRE(p.SpellPlacedSets() == 0);
  const auto destroyed = DestroyedTo(p, 1);
  for (const auto& m : created) {
    REQUIRE(std::find(destroyed.begin(), destroyed.end(), m.idx) !=
            destroyed.end());
  }
  Leave(p, placed, true);
}

TEST_CASE("A restart sweeps every spell-placed form: a record carrying "
          "placedBySpell is deleted at load, one without it is kept",
          "[SpellPlaced][espm]")
{
  PartOne& p = GetPartOne();

  // a record as a running server saved one: a REFR above 0xff000000 on an
  // MSTT base, carrying the spell; WorldState::LoadChangeForm recreates it
  // from the record as AttachSaveStorage does for each record at start
  MpChangeForm record;
  record.recType = MpChangeForm::REFR;
  record.formDesc = FormDesc::FromFormId(kSwept, p.worldState.espmFiles);
  record.baseDesc = FirstOfType(p, "MSTT");
  record.worldOrCellDesc = FormDesc::Tamriel();
  record.position = { 192, 0, -48 };
  record.placedBySpell = FormDesc::FromString(kFlamesDesc);
  p.worldState.LoadChangeForm(record, p.CreateFormCallbacks());
  auto& swept = p.worldState.GetFormAt<MpObjectReference>(kSwept);
  REQUIRE(swept.GetPlacedBySpell() == record.placedBySpell);
  REQUIRE(!swept.IsDeleted());
  REQUIRE(p.SweepSpellPlacedAtLoad(record));
  REQUIRE(swept.IsDeleted());
  // swept once
  REQUIRE(!p.SweepSpellPlacedAtLoad(record));

  MpChangeForm plain = record;
  plain.formDesc = FormDesc::FromFormId(kKept, p.worldState.espmFiles);
  plain.placedBySpell.reset();
  p.worldState.LoadChangeForm(plain, p.CreateFormCallbacks());
  REQUIRE(!p.SweepSpellPlacedAtLoad(plain));
  REQUIRE(!p.worldState.GetFormAt<MpObjectReference>(kKept).IsDeleted());

  p.worldState.DestroyForm(kSwept);
  p.worldState.DestroyForm(kKept);
}
