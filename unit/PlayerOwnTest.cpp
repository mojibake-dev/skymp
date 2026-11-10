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
// abilities and WolfSkinFXArmor, read here from its VMAD by name. The bound
// reads an attachment only while it is live for the player (the fresh
// review's note 3): a quest the world's journal records running or staged,
// or one the quest scope classes the player's; an effect a SPEL the player
// holds carries (WerewolfChangeFXEffect 0xf8209's WerewolfTransformVisual
// names WerewolfRace 0xcdd84 and WolfSkinFXArmor 0xf6002, and only
// WerewolfChangeFX 0xf8208 carries it; Dragonborn's Frostmoon werewolf and
// werebear effects attach the same class: lab/esm.py on sky-srv,
// 2026-11-10).
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
constexpr uint32_t kVampireQuest = 0xeafd5;
constexpr uint32_t kWolfSkin = 0xf6002;
constexpr uint32_t kWerewolfChangeFx = 0xf8208;
constexpr uint32_t kWerewolfChangeFxEffect = 0xf8209;

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

// the two players side by side, the turned one wearing NordRace; the two
// quests' scripts live through the world's journal (both recorded running)
// unless `live` is false
void Enter(PartOne& p, bool live = true)
{
  if (live) {
    REQUIRE(p.SetQuestRuns(
      { { FormDesc::FromString("eafd5:Skyrim.esm"), true, 1 },
        { FormDesc::FromString("2ba16:Skyrim.esm"), true, 1 } }));
  }
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

  // the equipment gate passes the skin the overlay holds and relays the
  // report as the seat sent it (its bytes are the message's), but the
  // change form records it without the skin (the fresh review's note 5)
  UpdateEquipmentMessage equip;
  equip.idx = turned.GetIdx();
  equip.data.numChanges = 3;
  Inventory::ExtraData worn;
  worn.worn_ = true;
  equip.data.inv.entries.push_back(Inventory::Entry(skin, 1, worn));
  nlohmann::json report;
  equip.WriteJson(report);
  p.Messages().clear();
  DoMessage(p, 0, report);
  const auto& entries = turned.GetEquipment().inv.entries;
  REQUIRE(std::none_of(entries.begin(), entries.end(),
                       [&](const auto& e) { return e.baseId == skin; }));
  REQUIRE(turned.GetEquipment().numChanges == 3);
  REQUIRE(!turned.GetChangeForm().equipment.inv.HasItem(skin));
  size_t relayed = 0;
  for (auto& m : p.Messages()) {
    if (m.userId == 1 && m.j.is_object() &&
        m.j.value("t", -1) == static_cast<int>(MsgType::UpdateEquipment)) {
      REQUIRE(m.j["data"] == report["data"]);
      ++relayed;
    }
  }
  REQUIRE(relayed == 1);

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

TEST_CASE("A script's attachment names nothing until it is live for the "
          "player: the quest's run, a stage of it, or the scope's class",
          "[PlayerOwn][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p, false);
  OwnLog log;
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);

  // neither in the journal nor classed the player's: nothing named
  SetRace(p, kBretonRace, "PlayerVampireQuestScript");
  REQUIRE(log.Count("E_SCRIPTCALL_UNNAMED") == 1);
  REQUIRE(log.Count("live attachments") == 1);
  REQUIRE(log.Count(", 0 live for this player") == 1);
  REQUIRE(turned.GetAppearance()->raceId == kNordRace);
  REQUIRE(LooksTo(p, 1).empty());

  // a stage the journal records makes the quest's attachment live
  p.SetQuestStages({ { FormDesc::FromString("eafd5:Skyrim.esm"), 10 } });
  SetRace(p, kBretonRace, "PlayerVampireQuestScript");
  REQUIRE(turned.GetAppearance()->raceId == kBretonRace);
  p.SetQuestStages({});
  REQUIRE(!p.QuestAttachmentLive(kVampireQuest));

  // so does the scope's class while it acts (the playerQuests list)
  p.SetQuestScopeSettings(R"("derived")", R"(["eafd5:Skyrim.esm"])");
  REQUIRE(p.QuestAttachmentLive(kVampireQuest));
  SetRace(p, kNordRace, "PlayerVampireQuestScript");
  REQUIRE(turned.GetAppearance()->raceId == kNordRace);
  p.SetQuestScopeSettings(R"("off")", "[]");
  REQUIRE(!p.QuestAttachmentLive(kVampireQuest));
  Leave(p);
}

TEST_CASE("An effect script's attachment is live while the player holds a "
          "SPEL that carries the effect",
          "[PlayerOwn][espm]")
{
  PartOne& p = GetPartOne();
  // the class's name without case; Skyrim.esm's effect and Dragonborn's two
  const auto& attachments = p.ScriptAttachmentsOf("werewolftransformvisual");
  REQUIRE(attachments.size() == 3);
  const auto fx =
    std::find_if(attachments.begin(), attachments.end(), [](const auto& a) {
      return a.form == kWerewolfChangeFxEffect;
    });
  REQUIRE(fx != attachments.end());
  REQUIRE(!fx->quest);
  REQUIRE(fx->spells == std::vector<uint32_t>{ kWerewolfChangeFx });
  REQUIRE(std::any_of(fx->named.begin(), fx->named.end(), [](const auto& n) {
    return n.form == kWerewolfBeastRace;
  }));
  Enter(p, false);
  OwnLog log;
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);
  const char* const script = "WerewolfTransformVisual";

  SetRace(p, kWerewolfBeastRace, script);
  REQUIRE(log.Count("E_SCRIPTCALL_UNNAMED") == 1);
  REQUIRE(!p.PlayerOverlayOf(kTurned).has_value());

  // the record holds the carrying spell: the effect's attachment names the
  // beast race, which opens the overlay
  turned.AddSpell(kWerewolfChangeFx);
  SetRace(p, kWerewolfBeastRace, script);
  const auto overlay = p.PlayerOverlayOf(kTurned);
  REQUIRE(overlay);
  REQUIRE(overlay->race == kWerewolfBeastRace);
  REQUIRE(turned.GetAppearance()->raceId == kNordRace);
  Leave(p);
}

TEST_CASE("A form of another kind than the row's is refused by name "
          "(E_SCRIPTCALL_ARGS), named or not, and writes nothing",
          "[PlayerOwn][espm]")
{
  PartOne& p = GetPartOne();
  const uint32_t ability =
    PropertyOf(p, kVampireQuest, "PlayerVampireQuestScript", "AbVampire01");
  REQUIRE(ability != 0);
  Enter(p);
  OwnLog log;
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);

  // a SPEL the script names, as the race: the look would name no RACE and
  // the next start would drop the whole change form
  SetRace(p, ability, "PlayerVampireQuestScript");
  REQUIRE(log.Count("E_SCRIPTCALL_ARGS") == 1);
  REQUIRE(log.Count("is no RACE record") == 1);
  REQUIRE(turned.GetAppearance()->raceId == kNordRace);
  REQUIRE(LooksTo(p, 1).empty());

  // a RACE as a spell
  Own(p, "AddSpell", { FormArg(kNordRaceVampire, "Spell"), Arg(false) },
      "PlayerVampireQuestScript", "VampireProgression");
  REQUIRE(!turned.IsSpellLearned(kNordRaceVampire));
  Leave(p);
}

TEST_CASE("Outside an overlay EquipItem folds an item the record holds and "
          "refuses one it lacks (E_SCRIPTCALL_UNNAMED): no row mints an item",
          "[PlayerOwn][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  OwnLog log;
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);
  const char* const script = "PlayerWerewolfChangeScript";

  Own(p, "EquipItem", { FormArg(kWolfSkin, "Armor"), Arg(false), Arg(true) },
      script, "StartTracking");
  REQUIRE(log.Count("E_SCRIPTCALL_UNNAMED") == 1);
  REQUIRE(log.Count("is no item the player's record holds") == 1);
  REQUIRE(!turned.GetInventory().HasItem(kWolfSkin));

  turned.AddItem(kWolfSkin, 1);
  p.Messages().clear();
  Own(p, "EquipItem", { FormArg(kWolfSkin, "Armor"), Arg(false), Arg(true) },
      script, "StartTracking");
  REQUIRE(turned.GetInventory().GetItemCount(kWolfSkin) == 1);
  REQUIRE(LooksTo(p, 1).empty());
  Leave(p);
}

// Eli's D-E1 A: the seventh own row, Actor.ChangeHeadPart, rotfern's eyes in
// the lab (rotfern.esp's RotfernEyes 0x014C06 and RotfernEyesMora 0x023F0E,
// both valid for race rotfern 0xAA00). The unit world loads the five
// masters, not rotfern.esp, and no vanilla QUST or MGEF script names an HDPT
// (lab/esm.py on sky-srv, 2026-11-10, every script of the five read), so
// the call is taken here to the bound's refusal, and the race list and the
// swap are the core's own functions over vanilla eye parts: Skyrim.esm's
// MaleEyesHumanBrown 0x51632 and MaleEyesHumanIceBlue 0x24244 (Eyes, their
// RNAM HeadPartsHuman 0xa8026, NordRace among its races), MaleEyesArgonian
// 0x5150a (HeadPartsArgonian 0xa8021, ArgonianRace 0x13740 alone) and
// HairMaleNord01 0x51507 (Hair)
TEST_CASE("A head part is one the recorded race may wear by its HDPT's "
          "race list, swapped on the look for the part of its type; the "
          "call is the bound's and the kind's like every own row",
          "[PlayerOwn][espm]")
{
  constexpr uint32_t kEyesBrown = 0x51632;
  constexpr uint32_t kEyesIceBlue = 0x24244;
  constexpr uint32_t kEyesArgonian = 0x5150a;
  constexpr uint32_t kHairNord01 = 0x51507;
  constexpr uint32_t kArgonianRace = 0x13740;
  PartOne& p = GetPartOne();
  REQUIRE(p.HeadPartFitsRace(kEyesIceBlue, kNordRace));
  REQUIRE(p.HeadPartFitsRace(kEyesArgonian, kArgonianRace));
  REQUIRE(!p.HeadPartFitsRace(kEyesArgonian, kNordRace));
  REQUIRE(!p.HeadPartFitsRace(kNordRace, kNordRace)); // no HDPT at all

  // eyes replace eyes, the hair stays; a part worn already moves nothing
  Appearance look;
  look.raceId = kNordRace;
  look.headpartIds = { kEyesBrown, kHairNord01 };
  REQUIRE(p.WearHeadPart(look, kEyesIceBlue));
  REQUIRE(look.headpartIds ==
          std::vector<uint32_t>{ kHairNord01, kEyesIceBlue });
  REQUIRE(!p.WearHeadPart(look, kEyesIceBlue));
  REQUIRE(!p.WearHeadPart(look, kNordRace));
  REQUIRE(look.headpartIds ==
          std::vector<uint32_t>{ kHairNord01, kEyesIceBlue });

  // the call from a live script no VMAD of which names a head part: the
  // bound refuses it; a RACE named as the part: the kind refuses it
  Enter(p);
  OwnLog log;
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);
  Own(p, "ChangeHeadPart", { FormArg(kEyesIceBlue, "HeadPart") },
      "PlayerVampireQuestScript", "VampireChange");
  REQUIRE(log.Count("E_SCRIPTCALL_UNNAMED") == 1);
  Own(p, "ChangeHeadPart", { FormArg(kNordRace, "HeadPart") },
      "PlayerVampireQuestScript", "VampireChange");
  REQUIRE(log.Count("is no HDPT record") == 1);
  REQUIRE(turned.GetAppearance()->headpartIds.empty());
  REQUIRE(LooksTo(p, 1).empty());
  Leave(p);
}

// Eli's D-R1 C (thuum docs/DECISIONS.md, rung 5's stage-eve words): a
// script's AddItem and RemoveItem on its own player are that player's own
// inventory record, R2, with no bound (ScriptCall 75 on 0x14, the owner
// step's record 4, ObjectReference's rows). x-acting-frame 20261110-072506's
// fragment is A Lovely Letter's stage 310 or 320,
// `Game.GetPlayer().AddItem(Gold, 25, False)` (Gold a MiscObject property,
// 0xf; thuum docs/research/rung5-quests-2026-11-09.md), reported by the
// acting seat alone: another seat runs it on the acting player's figure,
// whose call is no own row. A fresh actor carries a kit of its own, so every
// count below is the change a call made. LItemFoodCabbage75 0x10bf6a is
// unit/LeveledListUtilsTest.cpp's leveled list, and the chest 0x20570 by
// (21272, -7816, -3608) in 0x1a26f is PartOne_ActivateTest's; each record's
// type is read from the game files here
namespace {
constexpr uint32_t kGold = 0xf;
constexpr uint32_t kLeveledCabbage = 0x10bf6a;
constexpr uint32_t kChest = 0x20570;
constexpr uint32_t kArvelRef = 0x39655; // StandingTest's MS13ArvelREF, an ACHR

void OwnItem(PartOne& p, Networking::UserId user, uint32_t selfId,
             const char* fn, std::vector<Arg> args)
{
  ScriptCallMessage msg;
  msg.epoch = 0;
  msg.selfId = selfId;
  msg.cls = "ObjectReference";
  msg.fn = fn;
  msg.args = std::move(args);
  msg.caller = "LovelyLetterFragments"; // illustrative: no bound reads it
  msg.handler = "Fragment_22";
  RawMessageData raw;
  raw.userId = user;
  p.GetActionListener().OnScriptCall(raw, msg);
}

std::string RecordType(PartOne& p, uint32_t formId)
{
  const auto rec = p.GetEspm().GetBrowser().LookupById(formId).rec;
  return rec ? rec->GetType().ToString() : std::string();
}

// the gold counts of the SetInventory messages `user` heard, in order
std::vector<uint32_t> GoldSentTo(PartOne& p, Networking::UserId user)
{
  std::vector<uint32_t> out;
  for (auto& m : p.Messages()) {
    if (m.userId != user || !m.j.is_object() ||
        m.j.value("t", -1) != static_cast<int>(MsgType::SetInventory)) {
      continue;
    }
    uint32_t gold = 0;
    for (const auto& entry : m.j["inventory"]["entries"]) {
      if (entry.value("baseId", 0u) == kGold) {
        gold += entry.value("count", 0u);
      }
    }
    out.push_back(gold);
  }
  return out;
}
}

TEST_CASE("A script's AddItem of gold on the reporter's own player is "
          "recorded once in its inventory as reported, with no bound, and "
          "its game hears its inventory; a RemoveItem takes at most what the "
          "record holds",
          "[PlayerOwn][espm]")
{
  PartOne& p = GetPartOne();
  REQUIRE(RecordType(p, kGold) == "MISC");
  Enter(p);
  OwnLog log;
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);
  auto& watcher = p.worldState.GetFormAt<MpActor>(kWatcher);
  const uint32_t kit = turned.GetInventory().GetItemCount(kGold);
  const uint32_t watcherKit = watcher.GetInventory().GetItemCount(kGold);

  // A Lovely Letter's 25 gold: no attachment of the caller names it, none
  // needs to; recorded once, on the change form, and the player's own game
  // hears its inventory with it (deferred to the tick), the other none
  OwnItem(p, 0, 0x14, "AddItem",
          { FormArg(kGold, "Form"), Arg(25.0), Arg(false) });
  p.Tick();
  REQUIRE(turned.GetInventory().GetItemCount(kGold) == kit + 25);
  REQUIRE(turned.GetChangeForm().inv.GetItemCount(kGold) == kit + 25);
  REQUIRE(log.Count("25 added to its inventory as its seat reported it "
                    "(D-R1 C, R2)") == 1);
  const auto heard = GoldSentTo(p, 0);
  REQUIRE(!heard.empty());
  REQUIRE(heard.back() == kit + 25);
  REQUIRE(GoldSentTo(p, 1).empty());
  REQUIRE(watcher.GetInventory().GetItemCount(kGold) == watcherKit);

  // a quest taking 10 back, its other container None
  p.Messages().clear();
  OwnItem(p, 0, 0x14, "RemoveItem",
          { FormArg(kGold, "Form"), Arg(10.0), Arg(true), Arg() });
  p.Tick();
  REQUIRE(turned.GetInventory().GetItemCount(kGold) == kit + 15);
  REQUIRE(log.Count("10 removed from its inventory as its seat reported "
                    "it (D-R1 C, R2)") == 1);
  REQUIRE(!GoldSentTo(p, 0).empty());
  REQUIRE(GoldSentTo(p, 0).back() == kit + 15);

  // never below zero: a removal past the count takes what the record
  // holds, and one of what it lacks folds, nothing thrown
  REQUIRE_NOTHROW(
    OwnItem(p, 0, 0x14, "RemoveItem",
            { FormArg(kGold, "Form"), Arg(100000.0), Arg(true), Arg() }));
  REQUIRE(turned.GetInventory().GetItemCount(kGold) == 0);
  REQUIRE(log.Count("removed from its inventory as its seat reported it "
                    "(D-R1 C, R2), 0 held now") == 1);
  REQUIRE_NOTHROW(
    OwnItem(p, 0, 0x14, "RemoveItem",
            { FormArg(kGold, "Form"), Arg(5.0), Arg(true), Arg() }));
  REQUIRE(turned.GetInventory().GetItemCount(kGold) == 0);
  REQUIRE(watcher.GetInventory().GetItemCount(kGold) == watcherKit);
  Leave(p);
}

TEST_CASE("Each seat's 0x14 is its own player: a call whose self is not the "
          "reporter's own player is no own row and writes nothing",
          "[PlayerOwn][espm]")
{
  PartOne& p = GetPartOne();
  Enter(p);
  OwnLog log;
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);
  auto& watcher = p.worldState.GetFormAt<MpActor>(kWatcher);
  const uint32_t kit = turned.GetInventory().GetItemCount(kGold);
  const uint32_t watcherKit = watcher.GetInventory().GetItemCount(kGold);

  // the other seat's own call lands on its own player alone
  OwnItem(p, 1, 0x14, "AddItem",
          { FormArg(kGold, "Form"), Arg(5.0), Arg(false) });
  REQUIRE(watcher.GetInventory().GetItemCount(kGold) == watcherKit + 5);
  REQUIRE(turned.GetInventory().GetItemCount(kGold) == kit);

  // the other player's actor named as the self: no subject the reporter
  // may name (a server form), refused at the shape
  OwnItem(p, 0, kWatcher, "AddItem",
          { FormArg(kGold, "Form"), Arg(25.0), Arg(false) });
  REQUIRE(log.Count("E_SCRIPTCALL_ARGS") == 1);
  // an NPC as the self (a script's AddItem on Arvel): the player's own row
  // has no channel on another subject
  OwnItem(p, 0, kArvelRef, "AddItem",
          { FormArg(kGold, "Form"), Arg(25.0), Arg(false) });
  REQUIRE(log.Count("E_SCRIPTCALL_ROW") == 1);
  REQUIRE(log.Count("a player's own row on another subject") == 1);
  REQUIRE(turned.GetInventory().GetItemCount(kGold) == kit);
  REQUIRE(watcher.GetInventory().GetItemCount(kGold) == watcherKit + 5);
  Leave(p);
}

TEST_CASE("A leveled list's AddItem is refused by name and writes nothing: "
          "what landed is the engine's roll, which the report does not carry",
          "[PlayerOwn][espm]")
{
  PartOne& p = GetPartOne();
  REQUIRE(RecordType(p, kLeveledCabbage) == "LVLI");
  Enter(p);
  OwnLog log;
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);
  const Inventory before = turned.GetInventory();
  OwnItem(p, 0, 0x14, "AddItem",
          { FormArg(kLeveledCabbage, "Form"), Arg(1.0), Arg(false) });
  REQUIRE(log.Count("E_SCRIPTCALL_LEVELED") == 1);
  REQUIRE(log.Count("is a leveled list (LVLI)") == 1);
  REQUIRE(turned.GetInventory() == before);
  REQUIRE(!turned.GetInventory().HasItem(kLeveledCabbage));
  // a count that lands nothing, and a form of no item's kind
  OwnItem(p, 0, 0x14, "AddItem",
          { FormArg(kGold, "Form"), Arg(0.0), Arg(false) });
  REQUIRE(log.Count("E_SCRIPTCALL_ARGS") == 1);
  REQUIRE(log.Count("is no whole number from 1 to 2147483647") == 1);
  REQUIRE(turned.GetInventory() == before);
  Leave(p);
}

TEST_CASE("A removal into a container the player holds open is that "
          "container's own move (PutItem) and folds; once it is shut the "
          "removal is the player's",
          "[PlayerOwn][espm]")
{
  PartOne& p = GetPartOne();
  REQUIRE(RecordType(p, kChest) == "REFR");
  auto& chest = p.worldState.GetFormAt<MpObjectReference>(kChest);
  DoConnect(p, 0);
  p.CreateActor(kTurned, { 21272.0000, -7816.0000, -3608.0000 }, 0, 0x1a26f);
  p.SetUserActor(0, kTurned);
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);
  OwnLog log;
  const uint32_t kit = turned.GetInventory().GetItemCount(kGold);
  OwnItem(p, 0, 0x14, "AddItem",
          { FormArg(kGold, "Form"), Arg(25.0), Arg(false) });
  REQUIRE(turned.GetInventory().GetItemCount(kGold) == kit + 25);

  chest.Activate(turned);
  REQUIRE(chest.IsContainerOccupant(&turned));
  OwnItem(p, 0, 0x14, "RemoveItem",
          { FormArg(kGold, "Form"), Arg(10.0), Arg(false),
            FormArg(kChest, "ObjectReference") });
  REQUIRE(turned.GetInventory().GetItemCount(kGold) == kit + 25);

  // shut: the chest's PutItem would be refused, so the player's side is
  // the record's (the chest's side is the named gap: nobody records it)
  chest.Activate(turned, false, true);
  REQUIRE(!chest.IsContainerOccupant(&turned));
  OwnItem(p, 0, 0x14, "RemoveItem",
          { FormArg(kGold, "Form"), Arg(10.0), Arg(false),
            FormArg(kChest, "ObjectReference") });
  REQUIRE(turned.GetInventory().GetItemCount(kGold) == kit + 15);
  REQUIRE(log.Count("10 removed from its inventory") == 1);
  DoDisconnect(p, 0);
  p.DestroyActor(kTurned);
}

// The fresh review of the second head (FIX FIRST 2): D-R1 C let a seat mint a
// stack of any item at the native's rate with no limit on their number, and
// the wire carries at most 4096 stacks in one inventory (wire-schema
// cap::INVENTORY, wire-rules player_own::INVENTORY_STACKS), so an add that
// needs a new stack is refused by name once the record holds that many. The
// iron dagger 0x1397e is DropItemTest's, the lockpick 0xa LockTest's; each
// record's type is read here first
TEST_CASE("An own AddItem that needs a new stack is refused once the record "
          "holds the wire's 4096 stacks (E_SCRIPTCALL_FULL); one that joins "
          "a stack the record holds is still recorded, and a removal frees "
          "a stack",
          "[PlayerOwn][espm]")
{
  PartOne& p = GetPartOne();
  constexpr uint32_t kIronDagger = 0x1397e;
  constexpr uint32_t kLockpick = 0xa;
  REQUIRE(RecordType(p, kIronDagger) == "WEAP");
  REQUIRE(RecordType(p, kLockpick) == "MISC");
  Enter(p);
  OwnLog log;
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);
  // the record at the cap: a plain stack of gold beside 4095 iron daggers,
  // each tempered apart (a stack per health), so a plain dagger, which the
  // record holds, needs a new stack as the lockpick it lacks does
  Inventory full;
  full.entries.push_back(Inventory::Entry(kGold, 10));
  for (uint32_t i = 1; i < 4096; ++i) {
    Inventory::ExtraData tempered;
    tempered.health = 1.0f + static_cast<float>(i) / 4096.0f;
    full.entries.push_back(Inventory::Entry(kIronDagger, 1, tempered));
  }
  REQUIRE(full.entries.size() == 4096);
  turned.SetInventory(full);

  OwnItem(p, 0, 0x14, "AddItem",
          { FormArg(kIronDagger, "Form"), Arg(1.0), Arg(false) });
  OwnItem(p, 0, 0x14, "AddItem",
          { FormArg(kLockpick, "Form"), Arg(1.0), Arg(false) });
  REQUIRE(turned.GetInventory() == full);
  // one line a minute per seat and code, the second counted into the next
  REQUIRE(log.Count("[verb:quest-stage] E_SCRIPTCALL_FULL: ff000c71's own "
                    "ObjectReference.AddItem 1397e (from "
                    "LovelyLetterFragments.Fragment_22): the player's record "
                    "holds its cap of 4096 stacks (the wire's in one "
                    "inventory) and 1397e would need a new one: nothing is "
                    "written; refused, 1 since the last line") == 1);
  REQUIRE(log.Count("E_SCRIPTCALL_FULL") == 1);

  // the gold joins its plain stack: recorded at the cap, no stack added
  OwnItem(p, 0, 0x14, "AddItem",
          { FormArg(kGold, "Form"), Arg(25.0), Arg(false) });
  REQUIRE(turned.GetInventory().entries.size() == 4096);
  REQUIRE(turned.GetInventory().GetItemCount(kGold) == 35);
  REQUIRE(log.Count("25 added to its inventory as its seat reported it") == 1);

  // a removal takes a tempered dagger's whole stack, and the next new stack
  // lands
  OwnItem(p, 0, 0x14, "RemoveItem",
          { FormArg(kIronDagger, "Form"), Arg(1.0), Arg(true), Arg() });
  REQUIRE(turned.GetInventory().entries.size() == 4095);
  OwnItem(p, 0, 0x14, "AddItem",
          { FormArg(kLockpick, "Form"), Arg(1.0), Arg(false) });
  REQUIRE(turned.GetInventory().entries.size() == 4096);
  REQUIRE(turned.GetInventory().GetItemCount(kLockpick) == 1);
  REQUIRE(log.Count("E_SCRIPTCALL_FULL") == 1);
  Leave(p);
}

// A review of the rotfern-eyes slot, fix 1: the server indexes a QUST's alias
// scripts as the quest's attachments (libespm ScriptData::aliases), so an own
// row an alias script calls finds its bound, RotfernEyes.esp's (its script on
// its player alias alone) among them. Dragonborn.esm's DLC2BloodskalBladeQST
// 27282 has that shape (no quest script, no fragment, one alias), its alias
// script DLC2BloodskalBladeScript naming DLC2BloodskalBladeSpellVert 33a27 and
// DLC2BloodskalBladeSpellHoriz 1aeae; PlayerWerewolfQuest's alias 0 carries
// PlayerWerewolfScript naming WerewolfBeastRace beside the quest's own scripts
// (lab/esm.py's walk of the lab's cached Skyrim.esm, Dawnguard.esm and
// Dragonborn.esm, 2026-11-10: 1,255 scripted QUSTs, 698 with alias scripts,
// eight alias scripts alone). Each record's type is read here first
TEST_CASE("A quest's alias script is that quest's attachment: an alias-only "
          "quest's own row is accepted while the quest runs, and so is one "
          "from an alias script beside the quest's own",
          "[PlayerOwn][espm]")
{
  PartOne& p = GetPartOne();
  const auto& files = p.worldState.espmFiles;
  const uint32_t bladeQuest =
    FormDesc::FromString("27282:Dragonborn.esm").ToFormId(files);
  const uint32_t spellVert =
    FormDesc::FromString("33a27:Dragonborn.esm").ToFormId(files);
  const uint32_t spellHoriz =
    FormDesc::FromString("1aeae:Dragonborn.esm").ToFormId(files);
  REQUIRE(RecordType(p, bladeQuest) == "QUST");
  REQUIRE(RecordType(p, spellVert) == "SPEL");
  REQUIRE(RecordType(p, spellHoriz) == "SPEL");

  // the index: each alias script the quest's attachment, live as it is
  const auto names = [](const PartOne::ScriptAttachment& a, uint32_t form) {
    return std::any_of(a.named.begin(), a.named.end(),
                       [&](const auto& n) { return n.form == form; });
  };
  const auto& blade = p.ScriptAttachmentsOf("DLC2BloodskalBladeScript");
  const auto at = std::find_if(blade.begin(), blade.end(), [&](const auto& a) {
    return a.form == bladeQuest;
  });
  REQUIRE(at != blade.end());
  REQUIRE(at->quest);
  REQUIRE(names(*at, spellVert));
  REQUIRE(names(*at, spellHoriz));
  const auto& werewolf = p.ScriptAttachmentsOf("PlayerWerewolfScript");
  REQUIRE(std::any_of(werewolf.begin(), werewolf.end(), [&](const auto& a) {
    return a.form == kWerewolfQuest && a.quest && names(a, kWerewolfBeastRace);
  }));

  Enter(p, false);
  OwnLog log;
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);
  // the quest neither runs nor is staged: its attachment names nothing yet
  Own(p, "AddSpell", { FormArg(spellVert, "Spell"), Arg(false) },
      "DLC2BloodskalBladeScript", "OnEquipped");
  REQUIRE(log.Count("E_SCRIPTCALL_UNNAMED") == 1);
  REQUIRE(log.Count(", 0 live for this player") == 1);
  REQUIRE(!turned.IsSpellLearned(spellVert));

  // its run in the world's journal: the alias script's spell is recorded
  REQUIRE(
    p.SetQuestRuns({ { FormDesc::FromString("27282:Dragonborn.esm"), true, 1 },
                     { FormDesc::FromString("2ba16:Skyrim.esm"), true, 1 } }));
  Own(p, "AddSpell", { FormArg(spellVert, "Spell"), Arg(false) },
      "DLC2BloodskalBladeScript", "OnEquipped");
  REQUIRE(turned.IsSpellLearned(spellVert));

  // the werewolf's alias script names the beast race: its overlay opens
  SetRace(p, kWerewolfBeastRace, "PlayerWerewolfScript");
  const auto overlay = p.PlayerOverlayOf(kTurned);
  REQUIRE(overlay);
  REQUIRE(overlay->race == kWerewolfBeastRace);
  Leave(p);
}

// Fix 2: a recorded ChangeHeadPart writes the stored RaceMenu look's part of
// that HDPT type too (PartOne::RecordHeadPart, the write RecordPlayerOwn
// makes), since that look's load on a figure, and on the player after a
// login, replaces the base's head parts by type with its own (thuum
// docs/verbs/racemenu-sync.md, x-head-parts 20261007-133253) and would put
// the old eyes back. No vanilla script names an HDPT (805 HDPTs and 1,745
// scripted QUSTs and MGEFs read, their alias scripts and FormLists among
// them), so the bound never takes a vanilla call this far; the write is the
// core's and is read here over the head-part case's vanilla parts
TEST_CASE("A recorded head part swap brings the stored RaceMenu look's part "
          "of its type with it; a look that names it already or names no "
          "part of its type is left as it is",
          "[PlayerOwn][espm]")
{
  constexpr uint32_t kEyesBrown = 0x51632;
  constexpr uint32_t kEyesIceBlue = 0x24244;
  constexpr uint32_t kHairNord01 = 0x51507;
  const std::string kLook =
    R"({"actor": {"weight": 50},
  "headParts": [{"formIdentifier": "Skyrim.esm|051632", "type": 2},
                {"formIdentifier": "Skyrim.esm|051507", "type": 3}]})";
  PartOne& p = GetPartOne();
  Enter(p);
  auto& turned = p.worldState.GetFormAt<MpActor>(kTurned);
  Appearance worn = *turned.GetAppearance();
  worn.headpartIds = { kEyesBrown, kHairNord01 };
  turned.SetAppearance(&worn);
  REQUIRE(turned.SetRaceMenuPreset(kLook));

  const auto recorded = p.RecordHeadPart(turned, kEyesIceBlue);
  REQUIRE(recorded.look);
  REQUIRE(recorded.preset);
  REQUIRE(turned.GetAppearance()->headpartIds ==
          std::vector<uint32_t>{ kHairNord01, kEyesIceBlue });
  // the stored look names the new eyes in the old ones' place, its type and
  // the rest of the look kept
  const auto stored = nlohmann::json::parse(turned.GetRaceMenuPreset());
  REQUIRE(stored["headParts"].size() == 2);
  REQUIRE(stored["headParts"][0]["formIdentifier"] == "Skyrim.esm|024244");
  REQUIRE(stored["headParts"][0]["type"] == 2);
  REQUIRE(stored["headParts"][1]["formIdentifier"] == "Skyrim.esm|051507");
  REQUIRE(stored["actor"]["weight"] == 50);

  // the same swap again: both records hold it already, nothing moves
  const auto again = p.RecordHeadPart(turned, kEyesIceBlue);
  REQUIRE(!again.look);
  REQUIRE(!again.preset);
  // a stored look that still names the old eyes (one written before this
  // fix): the look folds, the stored look comes to it
  REQUIRE(turned.SetRaceMenuPreset(kLook));
  const auto healed = p.RecordHeadPart(turned, kEyesIceBlue);
  REQUIRE(!healed.look);
  REQUIRE(healed.preset);
  REQUIRE(turned.GetRaceMenuPreset().find("Skyrim.esm|024244") !=
          std::string::npos);

  // a look with no eyes, a part that is no HDPT, no look at all
  std::string noEyes =
    R"({"headParts": [{"formIdentifier": "Skyrim.esm|051507", "type": 3}]})";
  REQUIRE(!p.WearHeadPartInLook(noEyes, kEyesIceBlue));
  REQUIRE(!p.WearHeadPartInLook(noEyes, kNordRace));
  std::string none;
  REQUIRE(!p.WearHeadPartInLook(none, kEyesIceBlue));
  Leave(p);
}
