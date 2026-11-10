#include "ActionListener.h"
#include "Messages.h"
#include "TestUtils.hpp"
#include "libespm/Loader.h"
#include "libespm/ScriptData.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <spdlog/sinks/ostream_sink.h>
#include <spdlog/spdlog.h>
#include <sstream>
#include <string>
#include <vector>

PartOne& GetPartOne();

// thuum ADR-068 rung 5 (the two transformations, D-T1 A; docs/research/
// rung5-quests-2026-11-09.md section 8(b)): a per-player quest's script
// changes its own player through six rows (ScriptCall 75 on 0x14, the owner
// step's record 4), judged by wire-rules player_own over the calling
// script's VMAD properties, read across the QUSTs and MGEFs that attach the
// class. Skyrim.esm (lab/esm.py on sky-srv, 2026-11-09): NordRace 0x13746
// and BretonRace 0x13741 are Playable, NordRaceVampire 0x88794 and
// WerewolfBeastRace 0xcdd84 are not; PlayerVampireQuest 0xeafd5's
// PlayerVampireQuestScript names every race and its vampire twin as Race
// properties (no FormList: the twin rule's RaceCompatibility lists are no
// master's, so the twin is wire-rules player_own's test); PlayerWerewolfQuest
// 0x2ba16's PlayerWerewolfChangeScript names WerewolfBeastRace, the level
// abilities and WolfSkinFXArmor, read here from its VMAD by name.
namespace {
using Arg = std::optional<
  std::variant<bool, double, std::string, SpSnippetObjectArgument>>;

constexpr uint32_t kTurned = 0xff000c71;
constexpr uint32_t kWatcher = 0xff000c72;
constexpr uint32_t kNordRace = 0x13746;
constexpr uint32_t kBretonRace = 0x13741;
constexpr uint32_t kNordRaceVampire = 0x88794;
constexpr uint32_t kWerewolfBeastRace = 0xcdd84;
constexpr uint32_t kWerewolfQuest = 0x2ba16;

Arg FormArg(uint32_t formId, const char* type)
{
  SpSnippetObjectArgument object;
  object.formId = formId;
  object.type = type;
  return Arg(object);
}

void Own(PartOne& p, const char* fn, std::vector<Arg> args, const char* caller,
         const char* handler)
{
  ScriptCallMessage msg;
  msg.epoch = 0; // a guest seat's cell epoch: the own rows read none
  msg.selfId = 0x14;
  msg.cls = "Actor";
  msg.fn = fn;
  msg.args = std::move(args);
  msg.caller = caller;
  msg.handler = handler;
  RawMessageData raw;
  raw.userId = 0;
  p.GetActionListener().OnScriptCall(raw, msg);
}

void SetRace(PartOne& p, uint32_t race, const char* caller)
{
  Own(p, "SetRace", { FormArg(race, "Race") }, caller, "VampireChange");
}

// the form a property of `script` on `quest`'s VMAD names, 0 for none
uint32_t PropertyOf(PartOne& p, uint32_t quest, const char* script,
                    const char* property)
{
  const auto lookup = p.GetEspm().GetBrowser().LookupById(quest);
  espm::ScriptData data;
  lookup.rec->GetScriptData(&data, p.worldState.GetEspmCache());
  for (const auto& s : data.scripts) {
    if (s.scriptName != script) {
      continue;
    }
    for (const auto& prop : s.properties) {
      if (prop.name == property && prop.type == espm::Property::Type::Object) {
        return lookup.ToGlobalId(prop.value.formId);
      }
    }
  }
  return 0;
}

// the two players side by side, the turned one wearing NordRace
void Enter(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kTurned, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kTurned);
  DoConnect(p, 1);
  p.CreateActor(kWatcher, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(1, kWatcher);
  for (uint32_t id : { kTurned, kWatcher }) {
    auto& actor = p.worldState.GetFormAt<MpActor>(id);
    actor.SetPos({ 0, 0, 0 });
    actor.ForceSubscriptionsUpdate();
  }
  Appearance look;
  look.raceId = kNordRace;
  p.worldState.GetFormAt<MpActor>(kTurned).SetAppearance(&look);
  p.Messages().clear();
}

void Leave(PartOne& p)
{
  DoDisconnect(p, 0);
  DoDisconnect(p, 1);
  p.DestroyActor(kTurned);
  p.DestroyActor(kWatcher);
}

// the UpdateAppearance messages the watcher heard
std::vector<nlohmann::json> LooksTo(PartOne& p, Networking::UserId user)
{
  std::vector<nlohmann::json> out;
  for (auto& m : p.Messages()) {
    if (m.userId == user && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::UpdateAppearance)) {
      out.push_back(m.j);
    }
  }
  return out;
}

struct OwnLog
{
  std::shared_ptr<spdlog::logger> old = spdlog::default_logger();
  std::ostringstream oss;
  OwnLog()
  {
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(oss);
    auto logger = std::make_shared<spdlog::logger>("player-own-test", sink);
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
  }
  ~OwnLog() { spdlog::set_default_logger(old); }
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

TEST_CASE("A player's own SetRace to a Playable race its script names is "
          "recorded on its change form and its look sent to the other game; "
          "an unnamed form and a restricted race are refused by name",
          "[PlayerOwn][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  OwnLog log;
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);

  SetRace(p, kBretonRace, "PlayerVampireQuestScript");
  REQUIRE(turned.GetAppearance()->raceId == kBretonRace);
  auto looks = LooksTo(p, 1);
  REQUIRE(looks.size() == 1);
  REQUIRE(looks[0]["data"]["raceId"] == kBretonRace);
  REQUIRE(!looks[0].contains("overlay"));
  REQUIRE(LooksTo(p, 0).empty());

  // a form no attachment of the class names: E_SCRIPTCALL_UNNAMED
  p.Messages().clear();
  Own(p, "AddSpell", { FormArg(0x92c48, "Spell"), Arg(false) },
      "PlayerVampireQuestScript", "VampireProgression");
  REQUIRE(log.Count("E_SCRIPTCALL_UNNAMED") == 1);
  REQUIRE(!turned.IsSpellLearned(0x92c48));

  // a race on restrictedRaces the profile is not permitted: refused, the
  // look untouched
  p.SetRestrictedRacesSettings(R"({"13746:Skyrim.esm": [99]})");
  SetRace(p, kNordRace, "PlayerVampireQuestScript");
  REQUIRE(log.Count("E_SCRIPTCALL_RACE") == 1);
  REQUIRE(turned.GetAppearance()->raceId == kBretonRace);
  REQUIRE(LooksTo(p, 1).empty());
  p.SetRestrictedRacesSettings("{}");
  Leave(p);
}

TEST_CASE("A race neither Playable nor a twin opens the player's overlay: "
          "its figure wears it, what its scripts add stays off the change "
          "form and counts at the equipment gate, and the race's return or "
          "a disconnect ends it",
          "[PlayerOwn][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t ability =
    PropertyOf(p, kWerewolfQuest, "PlayerWerewolfChangeScript",
               "PlayerWerewolfLvl10AndBelowAbility");
  const uint32_t skin = PropertyOf(
    p, kWerewolfQuest, "PlayerWerewolfChangeScript", "WolfSkinFXArmor");
  REQUIRE(ability != 0);
  REQUIRE(skin != 0);
  Enter(p);
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);
  const char* const script = "PlayerWerewolfChangeScript";

  SetRace(p, kWerewolfBeastRace, script);
  // the record keeps the player's race; the figure wears the beast
  REQUIRE(turned.GetAppearance()->raceId == kNordRace);
  auto looks = LooksTo(p, 1);
  REQUIRE(looks.size() == 1);
  REQUIRE(looks[0]["overlay"]["open"] == true);
  REQUIRE(looks[0]["overlay"]["race"] == kWerewolfBeastRace);
  REQUIRE(looks[0]["overlay"]["base"] == kNordRace);
  REQUIRE(p.PlayerOverlayOf(kTurned).has_value());

  // the level ability and the skin go into the overlay, never the record
  p.Messages().clear();
  Own(p, "AddSpell", { FormArg(ability, "Spell"), Arg(false) }, script,
      "InitialShift");
  Own(p, "EquipItem", { FormArg(skin, "Armor"), Arg(false), Arg(true) },
      script, "StartTracking");
  REQUIRE(!turned.GetChangeForm().learnedSpells.IsSpellLearned(ability));
  REQUIRE(!turned.GetInventory().HasItem(skin));
  const auto overlay = p.PlayerOverlayOf(kTurned);
  REQUIRE(overlay);
  REQUIRE(overlay->spells == std::vector<uint32_t>{ ability });
  REQUIRE(overlay->items == std::vector<uint32_t>{ skin });
  looks = LooksTo(p, 1);
  REQUIRE(looks.size() == 2);
  REQUIRE(looks[1]["overlay"]["items"] == nlohmann::json::array({ skin }));

  // the equipment gate keeps the skin the overlay holds (the report as the
  // seat sends it, so its relay's bytes are the message's)
  UpdateEquipmentMessage equip;
  equip.idx = turned.GetIdx();
  equip.data.numChanges = 3;
  Inventory::ExtraData worn;
  worn.worn_ = true;
  equip.data.inv.entries.push_back(Inventory::Entry(skin, 1, worn));
  nlohmann::json report;
  equip.WriteJson(report);
  DoMessage(p, 0, report);
  const auto& entries = turned.GetEquipment().inv.entries;
  REQUIRE(std::any_of(entries.begin(), entries.end(),
                      [&](const auto& e) { return e.baseId == skin; }));

  // a figure created now wears the overlay too
  // (CreateActor props.overlay, the player's own CreateActor never)
  // the race's return ends it: the figure's look without an overlay
  p.Messages().clear();
  SetRace(p, kNordRace, script);
  REQUIRE(!p.PlayerOverlayOf(kTurned).has_value());
  looks = LooksTo(p, 1);
  REQUIRE(looks.size() == 1);
  REQUIRE(!looks[0].contains("overlay"));
  REQUIRE(!turned.GetChangeForm().learnedSpells.IsSpellLearned(ability));

  // a disconnect mid-form ends it with the connection
  SetRace(p, kWerewolfBeastRace, script);
  REQUIRE(p.PlayerOverlayOf(kTurned).has_value());
  DoDisconnect(p, 0);
  REQUIRE(!p.PlayerOverlayOf(kTurned).has_value());
  REQUIRE(turned.GetAppearance()->raceId == kNordRace);
  DoDisconnect(p, 1);
  p.DestroyActor(kTurned);
  p.DestroyActor(kWatcher);
}

TEST_CASE("A vampire race its script names but no Playable flag or twin "
          "list holds opens an overlay over the recorded race",
          "[PlayerOwn][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  SetRace(p, kNordRaceVampire, "PlayerVampireQuestScript");
  const auto overlay = p.PlayerOverlayOf(kTurned);
  REQUIRE(overlay);
  REQUIRE(overlay->race == kNordRaceVampire);
  REQUIRE(overlay->base == kNordRace);
  REQUIRE(p.worldState.GetFormAt<MpActor>(kTurned).GetAppearance()->raceId ==
          kNordRace);
  Leave(p);
}
