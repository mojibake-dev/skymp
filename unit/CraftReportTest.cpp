#include "ActionListener.h"
#include "ActorValueRecord.h"
#include "CraftReportMessage.h"
#include "DropItemMessage.h"
#include "HitMessage.h"
#include "MpChangeForms.h"
#include "OnEquipMessage.h"
#include "PacketParser.h"
#include "SpellCastMessage.h"
#include "TestUtils.hpp"
#include "UpdateMovementMessage.h"
#include "save_storages/AsyncSaveStorage.h"
#include <algorithm>
#include <catch2/catch_all.hpp>
#include <simdjson.h>

PartOne& GetPartOne();

// A sitting at a crafting bench is reported as its engine read the player's
// inventory at the menu's open and close: the server takes the lost stacks
// when its record holds every one, adds the gained ones as reported (a
// created form by its definition), keeps the enchantments a disenchant taught
// and teaches them again after a login. Ids from Skyrim.esm as the repo's own
// tests read them: Wheat 0x4B0BA (LearnedEffectsTest.cpp), RestoreHealth01
// 0x3EADD and its heal MGEF 0x3EB15 (MagicEffectsTest.cpp, EspmTest.cpp), the
// Iron Dagger 0x1397E (PoisonTest.cpp), the Steel Plate Boots with fire
// resist 0x1B401 and their enchantment EnchArmorResistFire03 0xAD484
// (EspmTest.cpp); the second ingredient 0x34CDD is the one CraftTest.cpp's
// Dragonborn recipe takes; a stolen stack's owner is the crime faction
// CrimeFactionFalkreath 0x28170 (ChangeFormMergedTest.cpp).

// SaveStorageTest.cpp's helpers: a file-backed save storage under unit/data
// and a synchronous upsert into it
std::shared_ptr<
  Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>>
MakeSaveStorageFile();
void UpsertSync(
  Viet::ISaveStorage<MpChangeForm, FormDesc, std::vector<FormDesc>>& st,
  std::vector<std::optional<MpChangeForm>> changeForms);

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
constexpr uint32_t kOwner = 0x00028170;

// A player (user 0) at the origin holding nothing: a new character's base
// container gives it a kit (five RestoreHealth01 0x3EADD and an Iron Dagger
// among it), which these cases would count beside their own stacks, so the
// record is emptied first (SetInventory marks the base container laid, so the
// kit never comes back)
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
// SetInventory naming it (craftAck), sent at once
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
  // the stack is the same
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

  // a created stack holds no item of its template: the plain counts pass it
  // by, and a plain equip of the template finds nothing to drink
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
  // reported worn flag never reaches the record
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
  // keeps a reported one zeroed (CraftService::NormalizeCreated)
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
  // FormDescWorldTest.cpp's orders: Apocalypse.esp at 03, then at 04 with
  // Essentials.esp before it, then gone
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
// the reference the last drop placed: its CreateActor names the base
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

// A drop names its stack with its extras and created forms, the record's own
// row for it is the one taken, the dropped copy holds it, and the pickup
// gives it back whole
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

// A stolen copy and a clean one of the same item are told apart by their
// owner: a drop or a craft takes the row with the reported stack's owner,
// the dropped copy keeps that owner for its pickup, and a stack whose owner
// the record does not hold is still taken from the rows it does
TEST_CASE("A drop or a craft takes the stolen copy or the clean one it "
          "names, and a dropped stolen copy comes back stolen at its pickup",
          "[CraftReport][DropItemTest][espm]")
{
  PartOne& p = GetPartOne();
  auto& ac = Player(p);
  Inventory::Entry stolenDagger(kDagger, 1);
  stolenDagger.ownerId = kOwner;
  ac.AddItems({ Inventory::Entry(kDagger, 1), stolenDagger });
  REQUIRE(Stacks(ac, kDagger).size() == 2);
  REQUIRE(!Stacks(ac, kDagger)[0].ownerId);

  // the stolen dagger dropped: the clean one stays, the copy holds the
  // stolen row
  RawMessageData raw;
  raw.userId = 0;
  DropItemMessage stolenDrop;
  stolenDrop.baseId = kDagger;
  stolenDrop.count = 1;
  stolenDrop.ownerId = kOwner;
  p.Messages().clear();
  p.GetActionListener().OnDropItem(raw, stolenDrop);
  REQUIRE(Stacks(ac, kDagger).size() == 1);
  REQUIRE(!Stacks(ac, kDagger)[0].ownerId);
  const uint32_t copyId = DroppedCopy(p, kDagger);
  REQUIRE(copyId != 0);
  auto& copy = p.worldState.GetFormAt<MpObjectReference>(copyId);
  REQUIRE(copy.GetInventory().entries.size() == 1);
  REQUIRE(copy.GetInventory().entries[0].ownerId == kOwner);

  // its pickup gives it back stolen, beside the clean one
  copy.Activate(ac);
  auto daggers = Stacks(ac, kDagger);
  REQUIRE(daggers.size() == 2);
  REQUIRE(!daggers[0].ownerId);
  REQUIRE(daggers[1].ownerId == kOwner);

  // the clean dagger dropped: the stolen one stays
  DropItemMessage cleanDrop;
  cleanDrop.baseId = kDagger;
  cleanDrop.count = 1;
  p.Messages().clear();
  p.GetActionListener().OnDropItem(raw, cleanDrop);
  daggers = Stacks(ac, kDagger);
  REQUIRE(daggers.size() == 1);
  REQUIRE(daggers[0].ownerId == kOwner);

  // a clean drop where the record holds only the stolen copy still takes it
  p.Messages().clear();
  p.GetActionListener().OnDropItem(raw, cleanDrop);
  REQUIRE(Stacks(ac, kDagger).empty());
  REQUIRE(DroppedCopy(p, kDagger) != 0);

  // a brew that used the stolen wheat leaves the clean wheat
  Inventory::Entry stolenWheat(kWheat, 1);
  stolenWheat.ownerId = kOwner;
  ac.AddItems({ Inventory::Entry(kWheat, 1), stolenWheat });
  Report(p, { stolenWheat }, { Potion(Heal(25.f)) });
  const auto wheat = Stacks(ac, kWheat);
  REQUIRE(wheat.size() == 1);
  REQUIRE(wheat[0].count == 1);
  REQUIRE(!wheat[0].ownerId);

  Leave(p);
}

// An equipment report's brewed potion is found by its definition, never by
// the plain template base
TEST_CASE("A record's brewed stack is held by its definition",
          "[CraftReport][espm]")
{
  Inventory inv;
  inv.AddItems({ Potion(Heal(30.f), 1) });
  REQUIRE(!inv.HasItem(kPotionTemplate));
  REQUIRE(inv.HasCreatedStack(kPotionTemplate, Heal(30.f)));
  REQUIRE(!inv.HasCreatedStack(kPotionTemplate, Heal(31.f)));
}

// A hit with a weapon worn with a made enchantment runs that enchantment's
// effects on the target through the effect rule, and the struck player's own
// game is told to cast it (SpellCast, the instant slot, spell 0, the
// definition as createdSpell); the attacker's own game is not told
TEST_CASE("An enchanted weapon's hit runs its made enchantment on the target "
          "and tells the struck player's game",
          "[CraftReport][espm]")
{
  constexpr uint32_t kAggressor = 0xff000c90;
  constexpr uint32_t kTarget = 0xff000c91;
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kAggressor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kAggressor);
  p.CreateActor(kTarget, { 0, 100, 0 }, 0, 0x3c);
  p.SetUserActor(1, kTarget);
  auto& aggressor = p.worldState.GetFormAt<MpActor>(kAggressor);
  auto& target = p.worldState.GetFormAt<MpActor>(kTarget);
  target.SetAngle({ 0.f, 0.f, 180.f });

  CreatedForm made;
  made.kind = CreatedForm::kWeaponEnchantment;
  // EnchFireDamageFFContact 0x4605a, EnchWeaponFireDamage01's own effect:
  // instant fire damage the effect rule numbers
  constexpr uint32_t kFireDamageMgef = 0x0004605a;
  made.effects = { CreatedEffect{ kFireDamageMgef, 10.f, 0, 0, 30.f } };
  Inventory::ExtraData extras;
  extras.createdEnchantment = made;
  extras.maxCharge = 2400.f;
  aggressor.SetInventory(Inventory());
  aggressor.AddItems({ Inventory::Entry(kDagger, 1, extras) });
  Inventory::ExtraData worn = extras;
  worn.worn_ = true;
  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(kDagger, 1, worn));
  aggressor.SetEquipment(eq);
  p.Messages().clear();

  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = kTarget;
  hit.data.source = kDagger;
  p.GetActionListener().OnHit(raw, hit);

  // the blade's own 25 on a base of 100 leaves 0.75; the enchantment's fire,
  // run by the effect rule, takes more
  REQUIRE(target.GetChangeForm().actorValues.healthPercentage < 0.75f);

  int toTarget = 0;
  int toAggressor = 0;
  for (auto& m : p.Messages()) {
    auto* cast = dynamic_cast<SpellCastMessage*>(m.message.get());
    if (!cast || !cast->data.createdSpell) {
      continue;
    }
    if (m.userId == 1) {
      ++toTarget;
      REQUIRE(cast->data.spell == 0u);
      REQUIRE(cast->data.caster == kAggressor);
      REQUIRE(cast->data.target == kTarget);
      REQUIRE(cast->data.castingSource == 3);
      REQUIRE(cast->data.createdSpell->SameAs(made));
    } else if (m.userId == 0) {
      ++toAggressor;
    }
  }
  REQUIRE(toTarget == 1);
  REQUIRE(toAggressor == 0);

  p.DestroyActor(kTarget);
  p.DestroyActor(kAggressor);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}

// A game-file enchanted weapon's hit relays its record's own enchantment
// (WEAP EITM: EnchIronSwordFire01 0x49BB8 names 0x49BB7, EspmTest's WEAP
// case) to the struck player's game by its id
TEST_CASE("A game-file enchanted weapon's hit relays its record's own "
          "enchantment to the struck player's game",
          "[CraftReport][espm]")
{
  constexpr uint32_t kAggressor = 0xff000c92;
  constexpr uint32_t kTarget = 0xff000c93;
  constexpr uint32_t kBurningSword = 0x00049bb8;
  constexpr uint32_t kItsEnchantment = 0x00049bb7;
  PartOne& p = GetPartOne();
  DoConnect(p, 0);
  DoConnect(p, 1);
  p.CreateActor(kAggressor, { 0, 0, 0 }, 0, 0x3c);
  p.SetUserActor(0, kAggressor);
  p.CreateActor(kTarget, { 0, 100, 0 }, 0, 0x3c);
  p.SetUserActor(1, kTarget);
  auto& aggressor = p.worldState.GetFormAt<MpActor>(kAggressor);
  auto& target = p.worldState.GetFormAt<MpActor>(kTarget);
  target.SetAngle({ 0.f, 0.f, 180.f });
  aggressor.SetInventory(Inventory());
  aggressor.AddItems({ Inventory::Entry(kBurningSword, 1) });
  Inventory::ExtraData worn;
  worn.worn_ = true;
  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(kBurningSword, 1, worn));
  aggressor.SetEquipment(eq);
  p.Messages().clear();

  RawMessageData raw;
  raw.userId = 0;
  HitMessage hit;
  hit.data.aggressor = 0x14;
  hit.data.target = kTarget;
  hit.data.source = kBurningSword;
  p.GetActionListener().OnHit(raw, hit);

  int toTarget = 0;
  for (auto& m : p.Messages()) {
    auto* cast = dynamic_cast<SpellCastMessage*>(m.message.get());
    if (cast && m.userId == 1 && cast->data.spell == kItsEnchantment) {
      ++toTarget;
      REQUIRE(!cast->data.createdSpell);
      REQUIRE(cast->data.caster == kAggressor);
    }
  }
  REQUIRE(toTarget == 1);

  p.DestroyActor(kTarget);
  p.DestroyActor(kAggressor);
  DoDisconnect(p, 1);
  DoDisconnect(p, 0);
}

// A crafted item lying on the ground at a restart: the dropped copy holds its
// stack's rows in its own change form, so the save keeps them
TEST_CASE("A tempered dagger's and a brewed potion's dropped copies survive a "
          "restart with their rows, and their pickups give the rows back",
          "[CraftReport][DropItemTest][espm]")
{
  PartOne& running = GetPartOne();
  auto& ac = Player(running);
  Inventory::Entry dagger(kDagger, 1);
  dagger.health = 1.1f;
  ac.AddItems({ dagger, Potion(Heal(30.f), 1) });

  RawMessageData raw;
  raw.userId = 0;
  DropItemMessage daggerDrop;
  daggerDrop.baseId = kDagger;
  daggerDrop.count = 1;
  daggerDrop.health = 1.1f;
  running.GetActionListener().OnDropItem(raw, daggerDrop);
  DropItemMessage potionDrop;
  potionDrop.baseId = kPotionTemplate;
  potionDrop.count = 1;
  potionDrop.createdBase = Heal(30.f);
  running.GetActionListener().OnDropItem(raw, potionDrop);
  REQUIRE(ac.GetInventory().entries.empty());
  const uint32_t daggerCopyId = DroppedCopy(running, kDagger);
  const uint32_t potionCopyId = DroppedCopy(running, kPotionTemplate);
  REQUIRE(daggerCopyId != 0);
  REQUIRE(potionCopyId != 0);

  // the record as the save writes it: the actor's and the copies'
  MpChangeForm savedActor = ac.GetChangeForm();
  savedActor.profileId = 7; // a player character, as the save marks one
  const MpChangeForm savedDagger =
    running.worldState.GetFormAt<MpObjectReference>(daggerCopyId)
      .GetChangeForm();
  const MpChangeForm savedPotion =
    running.worldState.GetFormAt<MpObjectReference>(potionCopyId)
      .GetChangeForm();
  Leave(running);

  // the restarted server reads the save as AttachSaveStorage does
  auto storage = MakeSaveStorageFile();
  UpsertSync(*storage, { savedActor, savedDagger, savedPotion });
  PartOne& restarted = GetPartOne();
  restarted.SetPropsPersist(true);
  restarted.AttachSaveStorage(storage);
  auto& daggerCopy =
    restarted.worldState.GetFormAt<MpObjectReference>(daggerCopyId);
  auto& potionCopy =
    restarted.worldState.GetFormAt<MpObjectReference>(potionCopyId);
  REQUIRE(daggerCopy.GetInventory().entries.size() == 1);
  REQUIRE(daggerCopy.GetInventory().entries[0].health == 1.1f);
  REQUIRE(potionCopy.GetInventory().entries.size() == 1);
  REQUIRE(potionCopy.GetInventory().entries[0].createdBase);
  REQUIRE(
    potionCopy.GetInventory().entries[0].createdBase->SameAs(Heal(30.f)));

  // the player logs in to its restored actor (the loader disables a player
  // character, the login enables it) and picks both up
  auto& loaded = restarted.worldState.GetFormAt<MpActor>(kActor);
  loaded.Enable();
  DoConnect(restarted, 0);
  restarted.SetUserActor(0, kActor);
  restarted.Tick();
  daggerCopy.Activate(loaded);
  potionCopy.Activate(loaded);
  REQUIRE(Stacks(loaded, kDagger).size() == 1);
  REQUIRE(Stacks(loaded, kDagger)[0].health == 1.1f);
  REQUIRE(Stacks(loaded, kPotionTemplate).size() == 1);
  REQUIRE(Stacks(loaded, kPotionTemplate)[0].createdBase);
  REQUIRE(Stacks(loaded, kPotionTemplate)[0].createdBase->SameAs(Heal(30.f)));

  restarted.DestroyActor(kActor);
  DoDisconnect(restarted, 0);
}
