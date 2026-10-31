#include "ActionListener.h"
#include "CreateActorMessage.h"
#include "DestroyActorMessage.h"
#include "MpChangeForms.h"
#include "MsgType.h"
#include "PlacedRequestMessage.h"
#include "TestUtils.hpp"
#include "libespm/Loader.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <chrono>
#include <simdjson.h>
#include <string_view>
#include <vector>

PartOne& GetPartOne();

// thuum docs/verbs/spell-placed-object.md (ADR-057, shape A since D70): a
// listed spell's accepted cast makes the server ask the caster's game where
// its engine stands the table's bases (PlacedRequest, the caster alone); each
// PlacedReport the rule accepts (the set this caster's and open, the base
// asked for once, the pose 150 to 260 units from where the caster stood on
// the ground plane and within 256 of its height) places that base as a
// server form at the reported pose; every subscriber's CreateActor names the
// caster (placedBy) and the base's type; the set falls lingerS after the
// stream's end or the caster's leaving; no report within the window places
// nothing; a restart sweeps every such form. The table here names
// Skyrim.esm's own records, since the test's five-file load order lacks
// Apocalypse: Flames 0x12FCD (casting type 2, concentration; SpellCastTest's)
// is the listed spell, Firebolt 0x12FD0 a spell off the list, and the bases
// are two of Skyrim.esm's own records read with lab/esm.py (`find <esm> MSTT
// Barrel`, `find <esm> ACTI FXEmptyActivator`, 2026-10-29): MSTT 0xEEE0D
// BFXSmashedBarrelWithOil and ACTI 0xB79FF FXEmptyActivator (the activator
// Apocalypse's own wall script places as its target box), each checked
// against the record's type at use. The reported poses are the engine's for
// Raise Wall (x-raise-wall-pitch v1, 2026-10-30): the wall 206.25 ahead and
// 48 below, the halo 196.875 ahead at the caster's height.

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

constexpr uint32_t kMsttBase =
  0x000eee0d; // Skyrim.esm BFXSmashedBarrelWithOil
constexpr uint32_t kActiBase = 0x000b79ff; // Skyrim.esm FXEmptyActivator

// Skyrim.esm's record of `type` the table names, as the settings write a
// form; the record is looked up by id and its type checked, since libespm's
// GetRecordsByType indexes no MSTT or ACTI (the first T0 of the overnight
// train, pipeline 3761, threw on it)
FormDesc FirstOfType(PartOne& p, const char* type)
{
  const uint32_t id = std::string_view(type) == "MSTT" ? kMsttBase : kActiBase;
  const auto lookup = p.GetEspm().GetBrowser().LookupById(id);
  REQUIRE(lookup.rec);
  REQUIRE(lookup.rec->GetType() == type);
  return FormDesc::FromFormId(id, p.worldState.espmFiles);
}

// the Raise Wall entry's shape on Skyrim.esm's records: the wall and the
// halo, no offsets (the pose is the engine's since shape A)
std::string Table(PartOne& p, int perCaster, int lingerS)
{
  const nlohmann::json table = nlohmann::json::array(
    { { { "spell", kFlamesDesc },
        { "anchor", "caster" },
        { "objects",
          nlohmann::json::array(
            { { { "base", FirstOfType(p, "MSTT").ToString() },
                { "type", "MSTT" } },
              { { "base", FirstOfType(p, "ACTI").ToString() },
                { "type", "ACTI" } } }) },
        { "lifetime",
          { { "whileHeld", true }, { "lingerS", lingerS }, { "maxS", 120 } } },
        { "perCaster", perCaster } } });
  return table.dump();
}

// the caster (user 0) at the origin facing east (yaw 90) with Flames in its
// right hand and Firebolt in its left, and the watcher (user 1) 300 units
// north, both in Tamriel
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

// a game's report of where its engine stands `base` for `set`, through the
// packet path from `user`
void Report(PartOne& p, Networking::UserId user, uint64_t set, uint32_t base,
            float x, float y, float z, float yaw)
{
  DoMessage(p, user,
            nlohmann::json{ { "t", static_cast<int>(MsgType::PlacedReport) },
                            { "set", set },
                            { "base", base },
                            { "pos", nlohmann::json::array({ x, y, z }) },
                            { "yaw", yaw } });
}

// the PlacedRequests that went to `user` since the last clear
std::vector<PlacedRequestMessage> RequestsTo(PartOne& p,
                                             Networking::UserId user)
{
  std::vector<PlacedRequestMessage> out;
  for (auto& m : p.Messages()) {
    auto ask = dynamic_cast<PlacedRequestMessage*>(m.message.get());
    if (m.userId == user && ask) {
      out.push_back(*ask);
    }
  }
  return out;
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

// the caster's cast of Flames from its right hand, the server's request read
// back, and both reports where the engine stands Raise Wall's objects: one
// standing set of two forms, their ids returned
std::vector<uint32_t> Stand(PartOne& p)
{
  const FormDesc mstt = FirstOfType(p, "MSTT");
  const FormDesc acti = FirstOfType(p, "ACTI");
  Cast(p, kFlames, 1, false);
  const auto asked = RequestsTo(p, 0);
  REQUIRE(asked.size() == 1);
  const uint64_t set = asked[0].set;
  Report(p, 0, set, mstt.ToFormId(p.worldState.espmFiles), 206.25f, 0.f, -48.f,
         90.f);
  Report(p, 0, set, acti.ToFormId(p.worldState.espmFiles), 196.875f, 0.f, 0.f,
         90.f);
  const auto created = PlacedTo(p, 1);
  REQUIRE(created.size() == 2);
  std::vector<uint32_t> placed;
  for (const auto& m : created) {
    placed.push_back(FormIdOf(m));
  }
  return placed;
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

TEST_CASE("A listed spell's accepted cast asks the caster alone for the "
          "table's bases, and each report places that base where the "
          "engine stands it, named to every subscriber, the caster's own "
          "game included",
          "[SpellPlaced][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  const FormDesc mstt = FirstOfType(p, "MSTT");
  const FormDesc acti = FirstOfType(p, "ACTI");
  const uint32_t msttId = mstt.ToFormId(p.worldState.espmFiles);
  const uint32_t actiId = acti.ToFormId(p.worldState.espmFiles);

  const uint64_t t0 = NowMs();
  Cast(p, kFlames, 1, false);

  // the request: the caster alone, both bases, the rule's window
  const auto asked = RequestsTo(p, 0);
  REQUIRE(asked.size() == 1);
  REQUIRE(asked[0].bases == std::vector<uint32_t>{ msttId, actiId });
  REQUIRE(asked[0].windowMs == 6000);
  REQUIRE(RequestsTo(p, 1).empty());
  const uint64_t set = asked[0].set;

  // nothing stands before a report
  p.TickPlacements(t0 + 500);
  REQUIRE(PlacedTo(p, 1).empty());
  REQUIRE(p.SpellPlacedSets() == 0);

  // the wall, where the engine put it: 206.25 east, 48 below, its own yaw
  Report(p, 0, set, msttId, 206.25f, 0.f, -48.f, 97.f);
  REQUIRE(p.SpellPlacedSets() == 1);
  auto toWatcher = PlacedTo(p, 1);
  REQUIRE(toWatcher.size() == 1);
  const auto wall = toWatcher[0];
  REQUIRE(wall.props.placedBy == kCaster);
  REQUIRE(wall.baseRecordType == "MSTT");
  REQUIRE(wall.baseId == msttId);
  REQUIRE(wall.transform.worldOrCell == kTamriel);
  REQUIRE(wall.transform.pos[0] == Catch::Approx(206.25f).margin(0.01));
  REQUIRE(wall.transform.pos[1] == Catch::Approx(0.f).margin(0.01));
  REQUIRE(wall.transform.pos[2] == Catch::Approx(-48.f).margin(0.01));
  REQUIRE(wall.transform.rot[2] == Catch::Approx(97.f).margin(0.01));
  REQUIRE(!wall.isMe);

  // the halo, later, into the same set
  Report(p, 0, set, actiId, 0.f, -196.875f, 0.f, 97.f);
  REQUIRE(p.SpellPlacedSets() == 1);
  toWatcher = PlacedTo(p, 1);
  REQUIRE(toWatcher.size() == 2);
  const auto halo = toWatcher[1];
  REQUIRE(halo.props.placedBy == kCaster);
  REQUIRE(halo.baseRecordType == "ACTI");
  REQUIRE(halo.baseId == actiId);
  REQUIRE(halo.transform.pos[0] == Catch::Approx(0.f).margin(0.01));
  REQUIRE(halo.transform.pos[1] == Catch::Approx(-196.875f).margin(0.01));
  REQUIRE(halo.transform.pos[2] == Catch::Approx(0.f).margin(0.01));
  REQUIRE(halo.transform.rot[2] == Catch::Approx(97.f).margin(0.01));

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

  // placed once: the request is complete, the clock adds nothing
  p.Messages().clear();
  p.TickPlacements(t0 + 5'000);
  REQUIRE(PlacedTo(p, 1).empty());
  REQUIRE(p.SpellPlacedSets() == 1);
  Leave(p, placed);
}

TEST_CASE("A spell off the list asks nothing; a listed cast nobody reports "
          "inside the window places nothing; a report outside the rule "
          "places nothing and a late halo leaves the wall standing alone",
          "[SpellPlaced][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  const uint32_t msttId =
    FirstOfType(p, "MSTT").ToFormId(p.worldState.espmFiles);

  Cast(p, kFirebolt, 0, false);
  REQUIRE(RequestsTo(p, 0).empty());

  // no report at all: the window closes on nothing
  uint64_t t0 = NowMs();
  Cast(p, kFlames, 1, false);
  REQUIRE(RequestsTo(p, 0).size() == 1);
  p.TickPlacements(t0 + 3'100);
  REQUIRE(PlacedTo(p, 1).empty());
  REQUIRE(p.SpellPlacedSets() == 0);

  // the next cast in the hand (the old request dropped) and the refusals:
  // another's set, another player's report, a base the entry does not name,
  // too near, too far, too high
  p.Messages().clear();
  t0 = NowMs();
  Cast(p, kFlames, 1, false);
  const auto asked = RequestsTo(p, 0);
  REQUIRE(asked.size() == 1);
  const uint64_t set = asked[0].set;
  Report(p, 0, set + 1000, msttId, 200.f, 0.f, 0.f, 90.f);
  Report(p, 1, set, msttId, 200.f, 0.f, 0.f, 90.f);
  Report(p, 0, set, kFlames, 200.f, 0.f, 0.f, 90.f);
  Report(p, 0, set, msttId, 149.f, 0.f, 0.f, 90.f);
  Report(p, 0, set, msttId, 0.f, 261.f, 0.f, 90.f);
  Report(p, 0, set, msttId, 200.f, 0.f, 300.f, 90.f);
  REQUIRE(PlacedTo(p, 1).empty());
  REQUIRE(p.SpellPlacedSets() == 0);

  // then the wall at the band's edge, and the same base again
  Report(p, 0, set, msttId, 150.f, 0.f, -256.f, 90.f);
  REQUIRE(PlacedTo(p, 1).size() == 1);
  REQUIRE(p.SpellPlacedSets() == 1);
  Report(p, 0, set, msttId, 150.f, 0.f, -256.f, 90.f);
  REQUIRE(PlacedTo(p, 1).size() == 1);
  std::vector<uint32_t> placed{ FormIdOf(PlacedTo(p, 1)[0]) };

  // the window closes without the halo: the wall stands alone
  p.TickPlacements(t0 + 3'100);
  REQUIRE(PlacedTo(p, 1).size() == 1);
  REQUIRE(p.SpellPlacedSets() == 1);
  Leave(p, placed);
}

TEST_CASE("A listed cast that ends inside the window still takes its "
          "reports, and the set lingers from the stream's end",
          "[SpellPlaced][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, 2, 1);
  const uint32_t msttId =
    FirstOfType(p, "MSTT").ToFormId(p.worldState.espmFiles);

  const uint64_t t0 = NowMs();
  Cast(p, kFlames, 1, false);
  const auto asked = RequestsTo(p, 0);
  REQUIRE(asked.size() == 1);
  Cast(p, kFlames, 1, true);
  Report(p, 0, asked[0].set, msttId, 206.25f, 0.f, -48.f, 90.f);
  const auto created = PlacedTo(p, 1);
  REQUIRE(created.size() == 1);
  REQUIRE(p.SpellPlacedSets() == 1);
  std::vector<uint32_t> placed{ FormIdOf(created[0]) };

  // the linger of one second runs from the end, which came before the report
  p.Messages().clear();
  p.TickPlacements(t0 + 3'000);
  REQUIRE(p.SpellPlacedSets() == 0);
  const auto destroyed = DestroyedTo(p, 1);
  REQUIRE(std::find(destroyed.begin(), destroyed.end(), created[0].idx) !=
          destroyed.end());
  REQUIRE(p.worldState.GetFormAt<MpObjectReference>(placed[0]).IsDeleted());
  Leave(p, placed);
}

TEST_CASE("A caster at its perCaster cap is asked nothing further",
          "[SpellPlaced][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, 1);

  const auto placed = Stand(p);
  REQUIRE(p.SpellPlacedSets() == 1);

  // a second stream from the instant slot while the first holds
  p.Messages().clear();
  Cast(p, kFlames, 3, false);
  REQUIRE(RequestsTo(p, 0).empty());
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

  const auto placed = Stand(p);
  const auto created = PlacedTo(p, 1);
  REQUIRE(created.size() == 2);

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

  const auto placed = Stand(p);
  const auto created = PlacedTo(p, 1);
  REQUIRE(created.size() == 2);

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
