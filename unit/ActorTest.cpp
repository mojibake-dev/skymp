#include "GetBaseActorValues.h"
#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

PartOne& GetPartOne();

TEST_CASE(
  "Actor appearance, equipment and isRaceMenuOpen properties should present "
  "in changeForm",
  "[Actor]")
{
  MpActor actor(LocationalData(), FormCallbacks::DoNothing());
  Appearance appearance;
  appearance.raceId = 0x123;
  actor.SetAppearance(&appearance);
  actor.SetEquipment(Equipment());
  actor.SetRaceMenuOpen(true);

  REQUIRE(actor.GetChangeForm().appearanceDump == appearance.ToJson());
  REQUIRE(actor.GetChangeForm().equipment == Equipment());
  REQUIRE(actor.GetChangeForm().isRaceMenuOpen == true);
}

TEST_CASE("Actor should load be able to load appearance, equipment, "
          "isRaceMenuOpen and other properties from changeform",
          "[Actor]")
{
  const auto kExtraWornTrue = [] {
    Inventory::ExtraData extra;
    extra.worn_ = true;
    return extra;
  }();

  Equipment eq;
  eq.inv.entries.push_back(Inventory::Entry(0x12eb7, 1, kExtraWornTrue));

  MpChangeForm changeForm;
  changeForm.isRaceMenuOpen = true;
  changeForm.equipment = eq;
  changeForm.appearanceDump = Appearance().ToJson();
  changeForm.recType = MpChangeForm::ACHR;
  changeForm.actorValues.healthPercentage = 1.0f;
  changeForm.actorValues.magickaPercentage = 0.9f;
  changeForm.actorValues.staminaPercentage = 0.0f;
  changeForm.isDead = true;
  changeForm.spawnPoint.cellOrWorldDesc.file = "yay";
  changeForm.spawnPoint.cellOrWorldDesc.shortFormId = 0xDEAD;
  changeForm.spawnPoint.pos = { 1, 2, 3 };
  changeForm.spawnPoint.rot = { 1, 2, 4 };
  changeForm.spawnDelay = 8.0f;
  changeForm.consoleCommandsAllowed = true;

  MpActor actor(LocationalData(), FormCallbacks::DoNothing(), 0xff000000);
  actor.ApplyChangeForm(changeForm);

  REQUIRE(actor.GetChangeForm().isRaceMenuOpen == true);
  REQUIRE(actor.GetChangeForm().equipment == eq);
  REQUIRE(actor.GetChangeForm().appearanceDump == Appearance().ToJson());
  REQUIRE(actor.GetChangeForm().actorValues.healthPercentage == 1.0f);
  REQUIRE(actor.GetChangeForm().actorValues.magickaPercentage == 0.9f);
  REQUIRE(actor.GetChangeForm().actorValues.staminaPercentage == 0.0f);
  REQUIRE(actor.GetChangeForm().isDead == true);
  REQUIRE(actor.GetChangeForm().spawnPoint.cellOrWorldDesc.file == "yay");
  REQUIRE(actor.GetChangeForm().spawnPoint.cellOrWorldDesc.shortFormId ==
          0xDEAD);
  REQUIRE(actor.GetChangeForm().spawnPoint.pos == NiPoint3{ 1, 2, 3 });
  REQUIRE(actor.GetChangeForm().spawnPoint.rot == NiPoint3{ 1, 2, 4 });
  REQUIRE(actor.GetChangeForm().spawnDelay == 8.0f);
  REQUIRE(actor.GetChangeForm().consoleCommandsAllowed == true);
}

TEST_CASE("Attribute percentages survive a reload with the game files loaded",
          "[Actor]")
{
  // thuum docs/verbs/attributes.md: with the master files loaded,
  // ApplyChangeForm refreshes the base values from them; the percentages
  // are the actor's state and must come from the record, or every server
  // restart heals every actor.
  PartOne& running = GetPartOne();
  REQUIRE(running.worldState.HasEspm());
  running.CreateActor(0xff000000, { 0, 0, 0 }, 0, 0x3c);
  auto& actor = running.worldState.GetFormAt<MpActor>(0xff000000);
  actor.SetPercentages({ 0.5f, 0.25f, 0.75f });
  const MpChangeForm saved = actor.GetChangeForm();
  REQUIRE(saved.actorValues.healthPercentage == 0.5f);

  // The restarted server: a fresh world reading the saved record, as
  // AttachSaveStorage does for each player character at start
  PartOne& restarted = GetPartOne();
  restarted.worldState.LoadChangeForm(saved, restarted.CreateFormCallbacks());
  auto& loaded = restarted.worldState.GetFormAt<MpActor>(0xff000000);
  const ActorValues values = loaded.GetChangeForm().actorValues;
  REQUIRE(values.healthPercentage == 0.5f);
  REQUIRE(values.magickaPercentage == 0.25f);
  REQUIRE(values.staminaPercentage == 0.75f);

  // The base values still come from the game files
  const BaseActorValues base =
    GetBaseActorValues(&restarted.worldState, loaded.GetBaseId(),
                       loaded.GetRaceId(), loaded.GetTemplateChain());
  REQUIRE(values.health == base.health);
  REQUIRE(values.healRate == base.healRate);
}

TEST_CASE("Actor factions in changeForm", "[Actor]")
{
  PartOne p;
  p.worldState.espmFiles = { "Skyrim.esm" };

  MpActor actor(LocationalData(), FormCallbacks::DoNothing());

  Faction faction = Faction();
  faction.formDesc = FormDesc::FromFormId(0x000123, p.worldState.espmFiles);
  faction.rank = 0;

  actor.AddToFaction(faction, false);
  // Second time should be ignored
  actor.AddToFaction(faction, false);

  REQUIRE(actor.GetChangeForm().factions.has_value());
  REQUIRE(actor.GetChangeForm().factions.value().size() == 1);
  REQUIRE(actor.GetChangeForm().factions.value()[0].formDesc.shortFormId ==
          0x000123);

  REQUIRE(
    actor.IsInFaction(FormDesc::FromFormId(0x000223, p.worldState.espmFiles),
                      false) == false);
  REQUIRE(actor.IsInFaction(
    FormDesc::FromFormId(0x000123, p.worldState.espmFiles), false));

  actor.RemoveFromFaction(
    FormDesc::FromFormId(0x000003, p.worldState.espmFiles), false);
  REQUIRE(actor.GetChangeForm().factions.value().size() == 1);

  actor.RemoveFromFaction(
    FormDesc::FromFormId(0x000123, p.worldState.espmFiles), false);
  REQUIRE(actor.GetChangeForm().factions.value().size() == 0);
}
