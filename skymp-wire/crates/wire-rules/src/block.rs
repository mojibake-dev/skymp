//! Blocking (thuum docs/verbs/blocking.md, M2). Whether a hit was blocked,
//! and what share of its damage the block stops, decided by the server from
//! its own records (R0): the target's recorded block (its blockStart and
//! blockStop, R2), its heading against the attacker, a shield for an arrow
//! or a bolt, and the engine's own formula. The attacker's game's verdict
//! (OnHit's isHitBlocked) is not read here: it saw the target's block only
//! through a relayed animation, a round trip late.
//!
//! The formula as the engine computes it (thuum ghidra/notes/block-formula.md,
//! Address Library ID 26425, HYPOTHESIS by thuum rule 2), confirmed in the
//! lab where measured (x-block-probe2 20261011-031501: an Iron Shield, rating
//! 20, stopped 50.0 percent at Block 15 to 20, the formula 49.9 to 50.2):
//! S = fArmorRatingBase + (fArmorRatingMax - fArmorRatingBase) x Block / 100
//! (Skyrim.esm 1.0 and 2.5, so UESP's "1 + Block x 1.5 / 100"); with a
//! shield, fShieldBaseFactor + (S + ShieldPerks) x rating x
//! fShieldScalingFactor / 100; without one, fBlockWeaponBase + S x the
//! attacker's weapon damage x fBlockWeaponScaling / 100; times
//! fBlockPowerAttackMult on a power attack; at most fPlayerMaxResistance /
//! 100. fBlockSkillMult and fBlockMax are read nowhere in the engine. The
//! perk entry point kModPercentBlocked waits for perks as server state (M5).

use crate::melee::HIT_CONE_ANGLE;

/// The game's block settings: Skyrim.esm's GMSTs as the caller read them,
/// and the executable's default where the masters set none.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BlockSettings {
    /// fShieldBaseFactor: a shield's share before its rating, 0 to 1.
    pub shield_base: f32,
    /// fShieldScalingFactor: percent per armor point of the shield.
    pub shield_scaling: f32,
    /// fBlockWeaponBase: a weapon's share before the attacker's weapon.
    pub weapon_base: f32,
    /// fBlockWeaponScaling: percent per point of the attacker's weapon damage.
    pub weapon_scaling: f32,
    /// fArmorRatingBase: the skill's factor at Block 0.
    pub rating_base: f32,
    /// fArmorRatingMax: the skill's factor at Block 100.
    pub rating_max: f32,
    /// fBlockPowerAttackMult: the share against a power attack, scaled.
    pub power_attack_mult: f32,
    /// fPlayerMaxResistance over 100: the most a block stops (the
    /// executable's 85; the masters set none).
    pub max_share: f32,
}

/// What the server knows about a hit on a target that may be blocking.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BlockFacts {
    /// The target's recorded block is up.
    pub blocking: bool,
    /// The target's heading, degrees (its yaw, as its movement reports it).
    pub heading: f32,
    /// The attacker's offset from the target, x.
    pub dx: f32,
    /// The attacker's offset from the target, y.
    pub dy: f32,
    /// The hit is an arrow or a bolt.
    pub ranged: bool,
    /// The target holds a shield (an ARMO with the shield's biped slot).
    pub shield: bool,
    /// The shield record's armor rating.
    pub shield_rating: f32,
    /// The target's ShieldPerks (actor value 66), the engine's term beside S.
    pub shield_perks: f32,
    /// The attacker's weapon's base damage (for a block without a shield).
    pub weapon_damage: f32,
    /// The target's Block skill.
    pub block_skill: f32,
    /// A power attack, as damage-flags backed it.
    pub power: bool,
}

/// Whether the attacker stands inside the cone the target's block covers:
/// fCombatHitConeAngle (the melee rule's HIT_CONE_ANGLE, 35) each side of
/// the target's heading, yaw only, as the engine tests a block (ID 47297,
/// called with a multiplier of 1). No slack: the server's record of the
/// target's heading is the report the attacker's game draws the figure from,
/// and x-block-probe2 saw no block at 45 degrees. A target on top of its
/// attacker, or a heading that is not a number, covers nothing.
#[must_use]
pub fn within_block_cone(f: &BlockFacts) -> bool {
    if !(f.heading.is_finite() && f.dx.is_finite() && f.dy.is_finite()) || (f.dx == 0.0 && f.dy == 0.0) {
        return false;
    }
    let bearing = f.dx.atan2(f.dy).to_degrees();
    let off = (f.heading - bearing).rem_euclid(360.0);
    off.min(360.0 - off) <= HIT_CONE_ANGLE
}

/// The share of a hit's damage the target's block stops: 0 when it is not
/// blocked (no block up, the attacker outside the cone, an arrow at a block
/// without a shield, or facts that are not numbers), else the engine's
/// formula, at most the cap. An arrow counts as blocked by a raised shield
/// in the cone: the engine wants the arrow to strike the shield's body
/// (ArrowProjectile::AddImpact, ID 43710), which the server cannot see, and
/// a shield raised toward the archer is where its arrow strikes (HYPOTHESIS).
#[must_use]
pub fn blocked_share(s: &BlockSettings, f: &BlockFacts) -> f32 {
    if !f.blocking || !within_block_cone(f) || (f.ranged && !f.shield) {
        return 0.0;
    }
    let skill = s.rating_base + (s.rating_max - s.rating_base) * f.block_skill.max(0.0) / 100.0;
    let share = if f.shield {
        s.shield_base + (skill + f.shield_perks.max(0.0)) * f.shield_rating.max(0.0) * s.shield_scaling / 100.0
    } else {
        s.weapon_base + skill * f.weapon_damage.max(0.0) * s.weapon_scaling / 100.0
    };
    let share = if f.power { share * s.power_attack_mult } else { share };
    if share.is_finite() {
        share.clamp(0.0, s.max_share.clamp(0.0, 1.0))
    } else {
        0.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Skyrim.esm (thuum lab/esm.py, 2026-10-10) and the executable's 85
    const SKYRIM: BlockSettings = BlockSettings {
        shield_base: 0.45,
        shield_scaling: 0.2,
        weapon_base: 0.3,
        weapon_scaling: 0.2,
        rating_base: 1.0,
        rating_max: 2.5,
        power_attack_mult: 0.66,
        max_share: 0.85,
    };

    // an Iron Shield (ArmorIronShield 0x12EB6, rating 20) at Block 15,
    // facing south at an attacker 100 units south, against an Iron Sword (7)
    fn iron_shield() -> BlockFacts {
        BlockFacts {
            blocking: true,
            heading: 180.0,
            dx: 0.0,
            dy: -100.0,
            ranged: false,
            shield: true,
            shield_rating: 20.0,
            shield_perks: 0.0,
            weapon_damage: 7.0,
            block_skill: 15.0,
            power: false,
        }
    }

    fn near(a: f32, b: f32) -> bool {
        (a - b).abs() < 1e-4
    }

    // the attacker at `degrees` off the shield's facing, 100 units away
    fn turned(degrees: f32) -> BlockFacts {
        let bearing = (180.0 + degrees).to_radians();
        BlockFacts { dx: 100.0 * bearing.sin(), dy: 100.0 * bearing.cos(), ..iron_shield() }
    }

    #[test]
    fn a_shield_stops_the_engine_s_share() {
        // 0.45 + (1 + 1.5 x 15 / 100) x 20 x 0.2 / 100 = 0.499 (the lab: 0.500)
        assert!(near(blocked_share(&SKYRIM, &iron_shield()), 0.499));
        // at Block 100: 0.45 + 2.5 x 0.04 = 0.55
        assert!(near(blocked_share(&SKYRIM, &BlockFacts { block_skill: 100.0, ..iron_shield() }), 0.55));
        // ShieldPerks adds beside S: 0.45 + (1.225 + 0.5) x 0.04 = 0.519
        assert!(near(blocked_share(&SKYRIM, &BlockFacts { shield_perks: 0.5, ..iron_shield() }), 0.519));
        // a power attack: times 0.66
        assert!(near(blocked_share(&SKYRIM, &BlockFacts { power: true, ..iron_shield() }), 0.499 * 0.66));
    }

    #[test]
    fn a_block_without_a_shield_stops_the_weapon_formula_s_share() {
        // 0.3 + 1.225 x 7 x 0.2 / 100 = 0.31715
        let sword = BlockFacts { shield: false, shield_rating: 0.0, ..iron_shield() };
        assert!(near(blocked_share(&SKYRIM, &sword), 0.31715));
    }

    #[test]
    fn the_cone_is_the_engine_s_35_degrees_each_side() {
        assert!(within_block_cone(&turned(0.0)));
        assert!(within_block_cone(&turned(30.0)));
        assert!(within_block_cone(&turned(-34.0)));
        // x-block-probe2: no block at 45, 90 or 135
        for off in [45.0, 90.0, 135.0, -45.0, 180.0] {
            assert!(!within_block_cone(&turned(off)), "{off}");
            assert!(near(blocked_share(&SKYRIM, &turned(off)), 0.0), "{off}");
        }
        // headings wrap
        assert!(within_block_cone(&BlockFacts { heading: 359.0, dx: 1.0, dy: 100.0, ..iron_shield() }));
        // on top of the attacker, or nonsense: no cone
        assert!(!within_block_cone(&BlockFacts { dx: 0.0, dy: 0.0, ..iron_shield() }));
        assert!(!within_block_cone(&BlockFacts { heading: f32::NAN, ..iron_shield() }));
    }

    #[test]
    fn no_block_without_one_and_an_arrow_needs_a_shield() {
        assert!(near(blocked_share(&SKYRIM, &BlockFacts { blocking: false, ..iron_shield() }), 0.0));
        let arrow = BlockFacts { ranged: true, ..iron_shield() };
        assert!(near(blocked_share(&SKYRIM, &arrow), 0.499));
        let at_a_sword = BlockFacts { ranged: true, shield: false, shield_rating: 0.0, ..iron_shield() };
        assert!(near(blocked_share(&SKYRIM, &at_a_sword), 0.0));
    }

    #[test]
    fn the_cap_holds_and_nonsense_stops_nothing() {
        let capped = BlockSettings { max_share: 0.4, ..SKYRIM };
        assert!(near(blocked_share(&capped, &iron_shield()), 0.4));
        assert!(near(blocked_share(&SKYRIM, &BlockFacts { shield_rating: f32::INFINITY, ..iron_shield() }), 0.0));
        let negative = BlockFacts { shield_rating: -50.0, block_skill: -10.0, shield_perks: -3.0, ..iron_shield() };
        assert!(near(blocked_share(&SKYRIM, &negative), 0.45));
    }
}
