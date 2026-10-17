#include "TestUtils.hpp"
#include "libespm/espm.h"

PartOne& GetPartOne();

// thuum docs/verbs/magic-effects.md: vanilla gates a perk's rider on the
// effect record's own conditions, not on the spell's entry. Firebolt
// (Skyrim.esm 0x12FD0) carries the Impact perk's stagger with no condition
// on the spell (the Apocalypse census of 2026-10-16, read on sky-srv), and
// the lab's server logs show 67 of 91 Firebolt hits staggered by it with no
// perk anywhere. The server keeps no perks before M5, so the rider stays
// out while the spell's fire damage still lands.
TEST_CASE("Firebolt's Impact stagger waits on its perk, its fire damage "
          "does not",
          "[MagicEffects][espm]")
{
  PartOne& p = GetPartOne();
  const auto effects = MpActor::EffectsOf(&p.worldState, 0x12fd0);
  REQUIRE_FALSE(effects.empty());
  int staggers = 0;
  int damages = 0;
  for (const auto& effect : effects) {
    const auto mgef =
      espm::GetData<espm::MGEF>(effect.effectId, &p.worldState);
    if (mgef.data.effectType == espm::MGEF::EffectType::Stagger) {
      ++staggers;
      REQUIRE(mgef.callsHasPerk);
      REQUIRE(MpActor::IsConditionGated(&p.worldState, effect));
    } else if (mgef.data.effectType == espm::MGEF::EffectType::ValueMod &&
               mgef.data.primaryAV == espm::ActorValue::Health) {
      ++damages;
      REQUIRE_FALSE(MpActor::IsConditionGated(&p.worldState, effect));
    }
  }
  REQUIRE(staggers >= 1);
  REQUIRE(damages >= 1);
}
