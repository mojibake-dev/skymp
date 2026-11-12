#include "TestUtils.hpp"
#include <catch2/catch_all.hpp>

PartOne& GetPartOne();

// thuum ADR-068 rung 4 (amendment B, read 2; docs/research/
// rung4-state-rows-2026-11-07.md section 1(c), Loader): most subjects of a
// script's Enable or Disable are references the server's loader never
// attaches (a marker, a collision box, an initially disabled activator or
// actor), so a reference with a recorded ref state is attached by its record:
// a saved record carrying refStateSeq lets LoadForm's type and disabled
// filters yield, and AttachForRecord does the same for a reference's first
// record, starting it as the game files start it. The subjects, read with
// lab/esm.py on sky-srv (2026-11-06; no later master overrides them): Bleak
// Falls Temple 0x371de's XMarkers 0x17037 at (6867, -5874, -1687) and
// 0x108a5c at (-598, -4954, -3312), each a REFR of the STAT 0x3b with record
// flags 0xc00 (initially disabled, persistent), which the encounter
// triggers 0x17038 and 0xf4a32 enable (docs/research/
// trap-census-2026-11-05.md), and its ambush draugr 0x380ba, an ACHR of the
// NPC_ 0x38922 with the same flags, which 0x4e421's script enables.

namespace {
constexpr uint32_t kTemple = 0x371de;
constexpr uint32_t kXMarker = 0x17037;
constexpr uint32_t kOtherXMarker = 0x108a5c;
constexpr uint32_t kXMarkerBase = 0x3b;
constexpr uint32_t kAmbushDraugr = 0x380ba;
const NiPoint3 kXMarkerPos{ 6867.f, -5874.f, -1687.f };

MpChangeForm SavedRecord(PartOne& p, std::optional<uint32_t> refStateSeq,
                         bool disabled)
{
  MpChangeForm changeForm;
  changeForm.recType = MpChangeForm::REFR;
  changeForm.formDesc = FormDesc::FromFormId(kXMarker, p.worldState.espmFiles);
  changeForm.baseDesc =
    FormDesc::FromFormId(kXMarkerBase, p.worldState.espmFiles);
  changeForm.worldOrCellDesc =
    FormDesc::FromFormId(kTemple, p.worldState.espmFiles);
  changeForm.position = kXMarkerPos;
  changeForm.isDisabled = disabled;
  changeForm.refStateSeq = refStateSeq;
  return changeForm;
}
}

TEST_CASE("A saved record carrying a recorded ref state attaches its "
          "reference by its record: an initially disabled STAT the loader "
          "skips",
          "[RefState][espm]")
{
  // no mark: the loader skips the XMarker (a STAT, and initially disabled)
  {
    auto& p = GetPartOne();
    p.worldState.LoadChangeForm(SavedRecord(p, std::nullopt, false),
                                p.CreateFormCallbacks());
    REQUIRE(!p.worldState.LookupFormById(kXMarker));
  }
  // the mark: attached, the saved record's state over the files' (a host's
  // Enable recorded at sequence 4)
  {
    auto& p = GetPartOne();
    p.worldState.LoadChangeForm(SavedRecord(p, 4, false),
                                p.CreateFormCallbacks());
    const auto& form = p.worldState.LookupFormById(kXMarker);
    REQUIRE(form);
    MpObjectReference* marker = form->AsObjectReference();
    REQUIRE(marker);
    REQUIRE(!marker->AsActor());
    REQUIRE(marker->GetBaseType() == "STAT");
    REQUIRE(!marker->IsDisabled());
    REQUIRE(marker->GetRefStateSeq() == 4u);
    REQUIRE(marker->GetCellOrWorld() ==
            FormDesc::FromFormId(kTemple, p.worldState.espmFiles));
  }
  // a recorded Disable stays disabled
  {
    auto& p = GetPartOne();
    p.worldState.LoadChangeForm(SavedRecord(p, 9, true),
                                p.CreateFormCallbacks());
    const auto& form = p.worldState.LookupFormById(kXMarker);
    REQUIRE(form);
    REQUIRE(form->AsObjectReference()->IsDisabled());
    REQUIRE(form->AsObjectReference()->GetRefStateSeq() == 9u);
  }
}

TEST_CASE("A reference's first recorded ref state attaches it by its record, "
          "starting as the game files start it",
          "[RefState][espm]")
{
  auto& p = GetPartOne();
  p.worldState.npcEnabled = true;
  // the plain lookups skip all three
  REQUIRE(!p.worldState.LookupFormById(kOtherXMarker));
  REQUIRE(!p.worldState.LookupFormById(kAmbushDraugr));

  // a world object: attached, disabled as the files place it, no record yet
  REQUIRE(p.worldState.AttachForRecord(kOtherXMarker));
  const auto& markerForm = p.worldState.LookupFormByIdNoLoad(kOtherXMarker);
  REQUIRE(markerForm);
  MpObjectReference* marker = markerForm->AsObjectReference();
  REQUIRE(marker);
  REQUIRE(marker->GetBaseType() == "STAT");
  REQUIRE(marker->IsDisabled());
  REQUIRE(!marker->GetRefStateSeq());

  // an actor: attached as an actor, disabled, off the grid until its Enable
  REQUIRE(p.worldState.AttachForRecord(kAmbushDraugr));
  const auto& draugrForm = p.worldState.LookupFormByIdNoLoad(kAmbushDraugr);
  REQUIRE(draugrForm);
  MpActor* draugr = draugrForm->AsActor();
  REQUIRE(draugr);
  REQUIRE(draugr->IsDisabled());
  REQUIRE(draugr->GetListeners().empty());

  // attached already: true, the same form
  REQUIRE(p.worldState.AttachForRecord(kAmbushDraugr));
  REQUIRE(p.worldState.LookupFormByIdNoLoad(kAmbushDraugr).get() == draugr);
  // no record attaches a server form, or a form no file holds
  REQUIRE(!p.worldState.AttachForRecord(0xff000123));
  REQUIRE(!p.worldState.AttachForRecord(0x00fffff0));
}
