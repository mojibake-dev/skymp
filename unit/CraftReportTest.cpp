#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "CraftReportMessage.h"
#include "DropItemMessage.h"
#include "MpChangeForms.h"
#include "OnEquipMessage.h"
#include "PacketParser.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <simdjson.h>

PartOne& GetPartOne();

// thuum tracker #98 (crafting; Eli's decision of 2026-11-16: relay the
// crafter's record). A sitting at a crafting bench is reported as its
// engine read the player's inventory at the menu's open and close: the
// server takes the lost stacks when its record holds every one, adds the
// gained ones as reported (a created form by its definition), keeps the
// enchantments a disenchant taught and teaches them again after a login.
// Ids from Skyrim.esm as the repo's own tests read them: Wheat 0x4B0BA
// (LearnedEffectsTest.cpp), RestoreHealth01 0x3EADD and its heal MGEF
// 0x3EB15 (MagicEffectsTest.cpp, EspmTest.cpp), the Iron Dagger 0x1397E
// (PoisonTest.cpp), the Steel Plate Boots with fire resist 0x1B401 and their
// enchantment EnchArmorResistFire03 0xAD484 (EspmTest.cpp); the second
// ingredient 0x34CDD is the one CraftTest.cpp's Dragonborn recipe takes.

namespace {
constexpr uint32_t kActor = 0xff000c98;
constexpr uint32_t kBench = 0x000be9a1;
constexpr uint32_t kWheat = 0x0004b0ba;
constexpr uint32_t kOther = 0x00034cdd;
constexpr uint32_t kPotionTemplate = 0x0003eadd;
constexpr uint32_t kHealMgef = 0x0003eb15;
constexpr uint32_t kDagger = 0x0001397e;
constexpr uint32_t kBoots = 0x0001b401;
constexpr uint32_t kBootsEnchantment = 0x000ad484;
constexpr uint32_t kSoulGem = 0x0002e4e3;

// A player (user 0) at the origin holding nothing: a new character's base
// container gives it a kit (five RestoreHealth01 0x3EADD and an Iron Dagger
// among it, CI pipeline 6420's three red cases), which these cases would
// count beside their own stacks, so the record is emptied first
// (SetInventory marks the base container laid, so the kit never comes back)
MpActor& Player(PartOne& p)
{
  DoConnect(p, 0);
  p.CreateActor(kActor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kActor);
  auto& ac = p.worldState.GetFormAt<MpActor>(kActor);
  ac.SetInventory(Inventory());
  REQUIRE(ac.GetInventory().entries.empty());
  p.Messages().clear();
  return ac;
}

void Leave(PartOne& p)
{
  p.DestroyActor(kActor);
  DoDisconnect(p, 0);
}

CreatedForm Heal(float magnitude, float cost = 12.5f)
{
  CreatedForm form;
  form.kind = CreatedForm::kPotion;
  form.effects = { CreatedEffect{ kHealMgef, magnitude, 0, 0, cost } };
  return form;
}

Inventory::Entry Potion(const CreatedForm& form, uint32_t count = 1)
{
  Inventory::Entry e(kPotionTemplate, count);
  e.createdBase = form;
  return e;
}

void Report(PartOne& p, std::vector<Inventory::Entry> lost,
            std::vector<Inventory::Entry> gained,
            std::vector<uint32_t> learned = {}, uint32_t seq = 1)
{
  RawMessageData raw;
  raw.userId = 0;
  CraftReportMessage msg;
  msg.workbench = kBench;
  msg.seq = seq;
  msg.lost = std::move(lost);
  msg.gained = std::move(gained);
  msg.learnedEnchantments = std::move(learned);
  p.GetActionListener().OnCraftReport(raw, msg);
}

// The record's stacks of `baseId`
std::vector<Inventory::Entry> Stacks(MpActor& ac, uint32_t baseId)
{
  std::vector<Inventory::Entry> out;
  for (const auto& e : ac.GetInventory().entries) {
    if (e.baseId == baseId) {
      out.push_back(e);
    }
  }
  return out;
}

// The answers to report `seq` user 0 received since the last clear: a
// SetInventory naming it (craftAck, SCHEMA_VERSION 62), sent at once
size_t AnswersSent(PartOne& p, uint32_t seq)
{
  return std::count_if(
    p.Messages().begin(), p.Messages().end(), [&](PartOne::Message& m) {
      return m.userId == 0 && m.j["t"] == MsgType::SetInventory &&
        m.j.contains("craftAck") && m.j["craftAck"] == seq;
    });
}

// The player's own movement report, standing at the origin
// (LearnedEffectsTest.cpp's)
void Stand(PartOne& p)
{
  static uint8_t unparsed[] = { Networking::MinPacketId, '{', '}' };
  RawMessageData raw;
  raw.userId = 0;
  raw.unparsed = unparsed;
  raw.unparsedLength = sizeof(unparsed);
  UpdateMovementMessage msg;
  msg.idx = 0;
  msg.data.pos = { 0, 0, 0 };
  msg.data.rot = { 0, 0, 0 };
  msg.data.isInJumpState = false;
  msg.data.isWeapDrawn = false;
  msg.data.isBlocking = false;
  msg.data.worldOrCell = 0x3c;
  msg.data.runMode = "Standing";
  p.GetActionListener().OnUpdateMovement(raw, msg);
}

// The Form.SetPlayerKnows snippets user 0 received since the last clear, by
// self
std::vector<uint32_t> Known(PartOne& p)
{
  p.Tick(); // snippets are deferred
  std::vector<uint32_t> out;
  for (auto& m : p.Messages()) {
    if (m.userId == 0 && m.j["t"] == MsgType::SpSnippet &&
        m.j["class"] == "Form" && m.j["function"] == "SetPlayerKnows") {
      out.push_back(m.j["selfId"].get<uint32_t>());
    }
  }
  return out;
}
}

TEST_CASE("CraftReport packet is parsed", "[CraftReport][espm]")
{
  class MyActionListener : public ActionListener
  {
  public:
    MyActionListener()
      : ActionListener(GetPartOne())
    {
    }

    void OnCraftReport(const RawMessageData& rawMsgData_,
                       const CraftReportMessage& msg_) override
    {
      userId = rawMsgData_.userId;
      msg = msg_;
    }

    Networking::UserId userId = Networking::InvalidUserId;
    CraftReportMessage msg;
  };

  const nlohmann::json potion =
    Inventory().AddItems({ Potion(Heal(25.f)) }).ToJson()["entries"][0];
  nlohmann::json j{ { "t", MsgType::CraftReport },
                    { "workbench", kBench },
                    { "seq", 7 },
                    { "lost",
                      { { { "baseId", kWheat }, { "count", 1 } },
                        { { "baseId", kOther }, { "count", 1 } } } },
                    { "gained", { potion } },
                    { "learnedEnchantments", nlohmann::json::array() } };
  auto packet = MakeMessage(j);

  MyActionListener listener;
  PacketParser parser;
  parser.TransformPacketIntoAction(
    122, reinterpret_cast<Networking::PacketData>(packet.data()),
    packet.size(), listener);

  REQUIRE(listener.userId == 122);
  REQUIRE(listener.msg.workbench == kBench);
  REQUIRE(listener.msg.seq == 7);
  REQUIRE(listener.msg.lost.size() == 2);
  REQUIRE(listener.msg.gained.size() == 1);
  REQUIRE(listener.msg.gained[0].createdBase == Heal(25.f));
}

TEST_CASE("A brew's ingredients leave the record and its potion arrives by "
          "its definition; the same brew stacks, another does not",
          "[CraftReport][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p);
  ac.AddItem(kWheat, 3);
  ac.AddItem(kOther, 3);

  p.Messages().clear();
  Report(p, { Inventory::Entry(kWheat, 1), Inventory::Entry(kOther, 1) },
         { Potion(Heal(25.f)) }, {}, 3);
  REQUIRE(AnswersSent(p, 3) == 1);
  REQUIRE(ac.GetInventory().GetItemCount(kWheat) == 2);
  REQUIRE(ac.GetInventory().GetItemCount(kOther) == 2);
  REQUIRE(Stacks(ac, kPotionTemplate).size() == 1);
  REQUIRE(Stacks(ac, kPotionTemplate)[0].createdBase == Heal(25.f));

  // the same effects at another cost are the same potion: the engine's
  // creator hands back one form for both (CreatedForm::SameAs)
  Report(p, { Inventory::Entry(kWheat, 1), Inventory::Entry(kOther, 1) },
         { Potion(Heal(25.f, 13.f)) });
  REQUIRE(Stacks(ac, kPotionTemplate).size() == 1);
  REQUIRE(Stacks(ac, kPotionTemplate)[0].count == 2);

  // the heal MGEF's record flags NoDuration (EspmTest.cpp), so a duration
  // reported on it is no part of the potion: the record keeps it zeroed and
  // the stack is the same (the review's finding 4)
  CreatedForm lasting = Heal(25.f);
  lasting.effects[0].duration = 30;
  Report(p, { Inventory::Entry(kWheat, 1), Inventory::Entry(kOther, 1) },
         { Potion(lasting) }, {}, 4);
  ac.AddItem(kWheat, 1);
  ac.AddItem(kOther, 1);
  REQUIRE(Stacks(ac, kPotionTemplate).size() == 1);
  REQUIRE(Stacks(ac, kPotionTemplate)[0].count == 3);
  REQUIRE(Stacks(ac, kPotionTemplate)[0].createdBase->effects[0].duration ==
          0);

  // another magnitude is another potion, and never a plain template
  Report(p, { Inventory::Entry(kWheat, 1), Inventory::Entry(kOther, 1) },
         { Potion(Heal(30.f)) });
  const auto stacks = Stacks(ac, kPotionTemplate);
  REQUIRE(stacks.size() == 2);
  REQUIRE(stacks[1].createdBase == Heal(30.f));
  REQUIRE(stacks[1].count == 1);
  REQUIRE(ac.GetInventory().GetItemCount(kWheat) == 0);
  REQUIRE(!ac.GetInventory().entries.empty());

  // a created stack holds no item of its template (the review's finding 9):
  // the plain counts pass it by, and a plain equip of the template finds
  // nothing to drink
  REQUIRE(ac.GetInventory().GetItemCount(kPotionTemplate) == 0);
  REQUIRE(!ac.GetInventory().HasItem(kPotionTemplate));
  RawMessageData raw;
  raw.userId = 0;
  OnEquipMessage plain;
  plain.baseId = kPotionTemplate;
  p.GetActionListener().OnEquip(raw, plain);
  REQUIRE(Stacks(ac, kPotionTemplate) == stacks);

  Leave(p);
}

TEST_CASE("A temper and an enchant replace the plain item with the stack its "
          "engine made",
          "[CraftReport][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p);
  ac.AddItem(kDagger, 2);
  Inventory::ExtraData filled;
  filled.soul = 3;
  ac.AddItems({ Inventory::Entry(kSoulGem, 1, filled) });

  // a temper: the plain dagger out, the dagger with its health in; the
  // seat's worn flag never reaches the record
  Inventory::Entry worn(kDagger, 1);
  worn.SetWorn(Inventory::Worn::Right);
  Inventory::Entry tempered = worn;
  tempered.health = 1.25f;
  Report(p, { worn }, { tempered });
  auto daggers = Stacks(ac, kDagger);
  REQUIRE(daggers.size() == 2);
  REQUIRE(daggers[0].count == 1);
  REQUIRE(!daggers[0].health);
  REQUIRE(daggers[1].health == 1.25f);
  REQUIRE(daggers[1].GetWorn() == Inventory::Worn::None);

  // an enchant: the plain dagger and the filled gem out, the dagger with its
  // made enchantment, its charge and its name in
  CreatedForm enchantment;
  enchantment.kind = CreatedForm::kWeaponEnchantment;
  // no duration: the heal MGEF's record flags NoDuration, so the record
  // keeps a reported one zeroed (CraftService::NormalizeCreated, 7c5aaa3f0)
  enchantment.effects = { CreatedEffect{ kHealMgef, 10.f, 0, 0, 32.f } };
  Inventory::Entry enchanted(kDagger, 1);
  enchanted.createdEnchantment = enchantment;
  enchanted.maxCharge = 1500.f;
  enchanted.chargePercent = 100.f;
  enchanted.name = "Dagger of Mending";
  Report(
    p, { Inventory::Entry(kDagger, 1), Inventory::Entry(kSoulGem, 1, filled) },
    { enchanted });
  daggers = Stacks(ac, kDagger);
  REQUIRE(daggers.size() == 2);
  REQUIRE(daggers[0].health == 1.25f);
  REQUIRE(daggers[1].createdEnchantment == enchantment);
  REQUIRE(!daggers[1].enchantmentId);
  REQUIRE(daggers[1].name == "Dagger of Mending");
  REQUIRE(ac.GetInventory().GetItemCount(kSoulGem) == 0);

  // the enchanted dagger is lost again by its definition, whatever charge
  // its engine reads now
  Inventory::Entry used = enchanted;
  used.chargePercent = 40.f;
  REQUIRE(CraftService::HeldRows(ac.GetInventory(), { used }).has_value());

  Leave(p);
}

TEST_CASE("A report whose lost stack the record does not hold changes "
          "nothing and sends the record back",
          "[CraftReport][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p);
  ac.AddItem(kWheat, 1);
  const Inventory before = ac.GetInventory();
  p.Messages().clear();

  // two where one is held, and a potion the record never had: each answered
  // by its own seq, the record unchanged
  Report(p, { Inventory::Entry(kWheat, 2) }, { Potion(Heal(25.f)) }, {}, 7);
  REQUIRE(ac.GetInventory() == before);
  REQUIRE(AnswersSent(p, 7) == 1);

  p.Messages().clear();
  Report(p, { Potion(Heal(25.f)) }, { Inventory::Entry(kWheat, 5) }, {}, 8);
  REQUIRE(ac.GetInventory() == before);
  REQUIRE(AnswersSent(p, 8) == 1);
  REQUIRE(AnswersSent(p, 7) == 0);

  // the refusal names the first stack the record does not hold
  size_t unheld = 99;
  REQUIRE(!CraftService::HeldRows(
    ac.GetInventory(),
    { Inventory::Entry(kWheat, 1), Inventory::Entry(kWheat, 1) }, &unheld));
  REQUIRE(unheld == 1);

  Leave(p);
}

TEST_CASE("A disenchant's enchantment is kept and taught after every login; "
          "a form that is no enchantment, or a sitting that lost nothing "
          "enchantable, teaches nothing",
          "[CraftReport][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p);
  ac.AddItem(kBoots, 1);
  ac.AddItem(kWheat, 1);

  // an ingredient lost cannot have carried an enchantment
  Report(p, { Inventory::Entry(kWheat, 1) }, {}, { kBootsEnchantment });
  REQUIRE(ac.GetKnownEnchantments().empty());

  // the boots disenchanted: their enchantment learned; a potion's id named
  // beside it is no enchantment
  Report(p, { Inventory::Entry(kBoots, 1) }, {},
         { kBootsEnchantment, kPotionTemplate });
  REQUIRE(ac.GetInventory().GetItemCount(kBoots) == 0);
  REQUIRE(ac.GetKnownEnchantments() ==
          std::vector<FormDesc>{
            FormDesc::FromFormId(kBootsEnchantment, p.worldState.espmFiles) });

  // nothing is taught until a login, and a login waits for the first
  // movement
  REQUIRE(Known(p).empty());
  p.SetUserActor(0, kActor);
  p.Messages().clear();
  REQUIRE(Known(p).empty());
  Stand(p);
  REQUIRE(Known(p) == std::vector<uint32_t>{ kBootsEnchantment });

  Leave(p);
}

TEST_CASE("A brewed potion is drunk by its definition: one copy taken, its "
          "own effects run",
          "[CraftReport][MagicEffects][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p);
  ActorValueRecord record;
  record.bases = { { 24, 100.f } };
  REQUIRE(ac.SetActorValueRecord(record));
  ActorValues values = ac.GetActorValues();
  values.healthPercentage = 0.5f;
  ac.SetPercentages(values);
  ac.AddItems({ Potion(Heal(30.f), 2) });

  RawMessageData raw;
  raw.userId = 0;
  OnEquipMessage drink;
  drink.baseId = kPotionTemplate;
  drink.createdBase = Heal(30.f);
  p.GetActionListener().OnEquip(raw, drink);
  REQUIRE(Stacks(ac, kPotionTemplate).size() == 1);
  REQUIRE(Stacks(ac, kPotionTemplate)[0].count == 1);
  // its own 30, never the template's record (RestoreHealth01's 25)
  REQUIRE(ac.GetChangeForm().actorValues.healthPercentage ==
          Catch::Approx(0.8f));

  // a definition the record holds no stack of changes nothing
  drink.createdBase = Heal(31.f);
  p.GetActionListener().OnEquip(raw, drink);
  REQUIRE(Stacks(ac, kPotionTemplate)[0].count == 1);

  Leave(p);
}

TEST_CASE("The change form keeps created stacks and known enchantments, and "
          "older records read none",
          "[CraftReport]")
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;
  changeForm.inv.AddItems({ Potion(Heal(25.f), 2) });
  changeForm.knownEnchantments =
    std::vector<FormDesc>{ FormDesc::FromString("ad484:Skyrim.esm") };

  simdjson::dom::parser parser;
  const std::string dump = MpChangeForm::ToJson(changeForm).dump();
  auto element = parser.parse(dump).value();
  const auto loaded = MpChangeForm::JsonToChangeForm(element);
  REQUIRE(loaded.inv == changeForm.inv);
  REQUIRE(loaded.inv.entries[0].createdBase == Heal(25.f));
  REQUIRE(loaded.knownEnchantments == changeForm.knownEnchantments);

  nlohmann::json older = MpChangeForm::ToJson(MpChangeForm());
  REQUIRE(!older.contains("knownEnchantments"));
  older["inv"] = { { "entries",
                     { { { "baseId", kWheat }, { "count", 1 } } } } };
  const std::string olderDump = older.dump();
  auto olderElement = parser.parse(olderDump).value();
  const auto olderLoaded = MpChangeForm::JsonToChangeForm(olderElement);
  REQUIRE(!olderLoaded.knownEnchantments);
  REQUIRE(olderLoaded.inv.entries.size() == 1);
  REQUIRE(!olderLoaded.inv.entries[0].createdBase);
  REQUIRE(!olderLoaded.inv.entries[0].createdEnchantment);
  REQUIRE(!olderLoaded.inv.entries[0].createdPoison);
}

TEST_CASE("A created effect's MGEF is stored by its plugin and crosses a load "
          "order change (ADR-043)",
          "[CraftReport]")
{
  // FormDescWorldTest.cpp's orders: Apocalypse.esp at 03, then at 04 after a
  // layer put Essentials.esp before it, then gone
  const EspmFileList orderA(
    { "Skyrim.esm", "Update.esm", "rotfern.esp", "Apocalypse.esp" },
    { false, false, false, false });
  const EspmFileList orderB({ "Skyrim.esm", "Update.esm", "Essentials.esp",
                              "rotfern.esp", "Apocalypse.esp" },
                            { false, false, false, false, false });
  const EspmFileList orderC({ "Skyrim.esm", "Update.esm", "rotfern.esp" },
                            { false, false, false });

  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::ACHR;
  changeForm.formDesc = FormDesc::FromString("abc");
  CreatedForm modded = Heal(25.f);
  modded.effects.push_back(CreatedEffect{ 0x03000e00, 5.f, 0, 30, 2.5f });
  changeForm.inv.AddItems({ Potion(modded) });

  const nlohmann::json stored = MpChangeForm::ToJson(changeForm, &orderA);
  const auto& effects = stored["inv"]["entries"][0]["createdBase"]["effects"];
  REQUIRE(effects[0]["mgef"] == "3eb15:Skyrim.esm");
  REQUIRE(effects[1]["mgef"] == "e00:Apocalypse.esp");

  simdjson::dom::parser parser;
  const std::string dump = stored.dump();
  auto element = parser.parse(dump).value();
  const auto moved = MpChangeForm::JsonToChangeForm(element, &orderB);
  REQUIRE(moved.inv.entries.size() == 1);
  REQUIRE(moved.inv.entries[0].createdBase->effects[0].mgef == kHealMgef);
  REQUIRE(moved.inv.entries[0].createdBase->effects[1].mgef == 0x04000e00);

  auto again = parser.parse(dump).value();
  const auto gone = MpChangeForm::JsonToChangeForm(again, &orderC);
  REQUIRE(gone.inv.entries.empty());
  REQUIRE(gone.unresolved.size() == 1);
  REQUIRE(gone.unresolved[0].field == "inv");
}

namespace {
// the reference the last drop placed: its create word names the base
uint32_t DroppedCopy(PartOne& p, uint32_t baseId)
{
  uint32_t refrId = 0;
  for (auto& m : p.Messages()) {
    if (m.j["t"] == MsgType::CreateActor && m.j.contains("refrId") &&
        m.j.contains("baseId") && m.j["baseId"] == baseId) {
      refrId = m.j["refrId"].get<uint32_t>();
    }
  }
  return refrId;
}
}

// thuum tracker #98 (SCHEMA_VERSION 63, Eli's look 2 of 3e8ab5acb: "cannot
// drop" a brew and an enchanted item; note 4020): a drop names its stack with
// its extras and created forms, the record's own row for it is the one taken,
// the dropped copy holds it, and the pickup gives it back whole
TEST_CASE("A brewed potion and an enchanted dagger are dropped as their own "
          "stacks and come back whole at their pickup",
          "[CraftReport][DropItemTest][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p);
  Inventory::Entry dagger(kDagger, 1);
  dagger.enchantmentId = kBootsEnchantment;
  dagger.maxCharge = 2400.f;
  dagger.name = "test";
  ac.AddItems({ Potion(Heal(30.f), 2), dagger });

  RawMessageData raw;
  raw.userId = 0;
  DropItemMessage potionDrop;
  potionDrop.baseId = kPotionTemplate;
  potionDrop.count = 1;
  potionDrop.createdBase = Heal(30.f);
  p.Messages().clear();
  p.GetActionListener().OnDropItem(raw, potionDrop);
  REQUIRE(Stacks(ac, kPotionTemplate).size() == 1);
  REQUIRE(Stacks(ac, kPotionTemplate)[0].count == 1);
  const uint32_t potionCopyId = DroppedCopy(p, kPotionTemplate);
  REQUIRE(potionCopyId != 0);
  auto& potionCopy = p.worldState.GetFormAt<MpObjectReference>(potionCopyId);
  REQUIRE(potionCopy.GetInventory().entries.size() == 1);
  REQUIRE(potionCopy.GetInventory().entries[0].createdBase);
  REQUIRE(
    potionCopy.GetInventory().entries[0].createdBase->SameAs(Heal(30.f)));

  DropItemMessage daggerDrop;
  daggerDrop.baseId = kDagger;
  daggerDrop.count = 1;
  daggerDrop.enchantmentId = kBootsEnchantment;
  daggerDrop.maxCharge = 2400.f;
  daggerDrop.name = "test";
  p.Messages().clear();
  p.GetActionListener().OnDropItem(raw, daggerDrop);
  REQUIRE(Stacks(ac, kDagger).empty());
  const uint32_t daggerCopyId = DroppedCopy(p, kDagger);
  REQUIRE(daggerCopyId != 0);
  auto& daggerCopy = p.worldState.GetFormAt<MpObjectReference>(daggerCopyId);
  REQUIRE(daggerCopy.GetInventory().entries.size() == 1);
  REQUIRE(daggerCopy.GetInventory().entries[0].enchantmentId ==
          kBootsEnchantment);

  // the pickups give the rows back as they were dropped
  daggerCopy.Activate(ac);
  REQUIRE(Stacks(ac, kDagger).size() == 1);
  REQUIRE(Stacks(ac, kDagger)[0].enchantmentId == kBootsEnchantment);
  REQUIRE(Stacks(ac, kDagger)[0].maxCharge == 2400.f);
  potionCopy.Activate(ac);
  REQUIRE(Stacks(ac, kPotionTemplate).size() == 1);
  REQUIRE(Stacks(ac, kPotionTemplate)[0].count == 2);
  REQUIRE(Stacks(ac, kPotionTemplate)[0].createdBase->SameAs(Heal(30.f)));

  Leave(p);
}

TEST_CASE("A drop of extras the record does not hold is refused and changes "
          "nothing",
          "[CraftReport][DropItemTest][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p);
  ac.AddItems({ Inventory::Entry(kDagger, 1) });

  RawMessageData raw;
  raw.userId = 0;
  DropItemMessage drop;
  drop.baseId = kDagger;
  drop.count = 1;
  drop.enchantmentId = kBootsEnchantment;
  drop.maxCharge = 2400.f;
  p.Messages().clear();
  p.GetActionListener().OnDropItem(raw, drop);
  REQUIRE(Stacks(ac, kDagger).size() == 1);
  REQUIRE(!Stacks(ac, kDagger)[0].enchantmentId);
  REQUIRE(DroppedCopy(p, kDagger) == 0);

  // and a brewed potion the record holds no stack of
  DropItemMessage potionDrop;
  potionDrop.baseId = kPotionTemplate;
  potionDrop.count = 1;
  potionDrop.createdBase = Heal(31.f);
  p.GetActionListener().OnDropItem(raw, potionDrop);
  REQUIRE(DroppedCopy(p, kPotionTemplate) == 0);

  Leave(p);
}

// thuum tracker #98 (look 2, note 4020): an equipment report's brewed potion
// is found by its definition, never by the plain template base
TEST_CASE("A record's brewed stack is held by its definition",
          "[CraftReport][espm]")
{
  Inventory inv;
  inv.AddItems({ Potion(Heal(30.f), 1) });
  REQUIRE(!inv.HasItem(kPotionTemplate));
  REQUIRE(inv.HasCreatedStack(kPotionTemplate, Heal(30.f)));
  REQUIRE(!inv.HasCreatedStack(kPotionTemplate, Heal(31.f)));
}
