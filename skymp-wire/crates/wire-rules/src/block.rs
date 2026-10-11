//! Blocking (thuum docs/verbs/blocking.md, M2). Whether a hit was blocked,
//! and what share of its damage the block stops, decided by the server from
//! its own records (R0): the target's recorded block (its blockStart and
//! blockStop, R2), its facing against the attacker's position, a shield in
//! its hands for an arrow or a bolt, and the game's formula. The attacker's
//! own verdict (OnHit's isHitBlocked) is not read here: its game sees the
//! target's block only through a relayed animation, a round trip late.
//!
//! The formula, UESP "Skyrim:Block" (read 2026-10-10): with a shield,
//! blocked percent = 45 + 0.2 x BaseShieldArmorRating x (1 + BlockSkill x
//! 1.5 / 100); with a weapon, 30 + 0.2 x EnemyWeaponBaseDamage x (the same);
//! times fBlockPowerAttackMult against a power attack. The base and scaling
//! are the masters' fShieldBaseFactor (0.45), fShieldScalingFactor (0.2),
//! fBlockWeaponBase (0.3) and fBlockWeaponScaling (0.2), read by the caller;
//! the skill's factor (UESP's 1.5 against Skyrim.esm's fBlockSkillMult 2.0)
//! and the cap (fBlockMax 0.7 or UESP's 85 percent) are HYPOTHESIS until the
//! engine's own code or x-block-probe2 settles them, so both are settings.

/// The game's block settings, from the masters' GMSTs (thuum lab/esm.py,
/// 2026-10-10), and the two the verb doc tags HYPOTHESIS.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BlockSettings {
    /// fShieldBaseFactor: a shield's share before its rating, 0 to 1.
    pub shield_base: f32,
    /// fShieldScalingFactor: percent per armor point of the shield.
    pub shield_scaling: f32,
    /// fBlockWeaponBase: a weapon's share before the attacker's weapon, 0 to 1.
    pub weapon_base: f32,
    /// fBlockWeaponScaling: percent per point of the attacker's weapon damage.
    pub weapon_scaling: f32,
    /// The skill's factor, percent per Block point over 100 (UESP's 1.5).
    pub skill_factor: f32,
    /// fBlockPowerAttackMult: the share against a power attack, scaled.
    pub power_attack_mult: f32,
    /// The most a block stops, 0 to 1.
    pub max_share: f32,
}

/// The cone a block covers, radians from the blocker's view to the
/// attacker: the server's 1 radian as it stood (ActionListener
/// ShouldBeBlocked); HYPOTHESIS until x-block-probe2 measures the engine's.
pub const BLOCK_CONE_RAD: f32 = 1.0;

/// What the server knows about a hit on a target that may be blocking.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BlockFacts {
    /// The target's recorded block is up.
    pub blocking: bool,
    /// Radians between the target's view and the direction to the attacker
    /// (0 head on, pi behind); not a number when either position is unknown.
    pub facing_angle: f32,
    /// The hit is an arrow or a bolt.
    pub ranged: bool,
    /// The target holds a shield (an ARMO with the shield's biped slot).
    pub shield: bool,
    /// The shield's base armor rating.
    pub shield_rating: f32,
    /// The attacker's weapon's base damage (for a block with a weapon).
    pub weapon_damage: f32,
    /// The target's Block skill.
    pub block_skill: f32,
    /// A power attack (damage-flags' backed flag).
    pub power: bool,
}

/// The share of a hit's damage the target's block stops: 0 when it is not
/// blocked (no block up, the attacker outside the cone, an arrow at a block
/// without a shield, or facts that are not numbers), else the game's
/// formula, at most the cap.
#[must_use]
pub fn blocked_share(s: &BlockSettings, f: &BlockFacts) -> f32 {
    if !f.blocking || !f.facing_angle.is_finite() || f.facing_angle.abs() >= BLOCK_CONE_RAD {
        return 0.0;
    }
    if f.ranged && !f.shield {
        return 0.0;
    }
    let skill = 1.0 + f.block_skill.max(0.0) * s.skill_factor / 100.0;
    let share = if f.shield {
        s.shield_base + s.shield_scaling / 100.0 * f.shield_rating.max(0.0) * skill
    } else {
        s.weapon_base + s.weapon_scaling / 100.0 * f.weapon_damage.max(0.0) * skill
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

    // Skyrim.esm (thuum lab/esm.py, 2026-10-10) and UESP's skill factor;
    // a cap above every case here, so the formula shows
    const SKYRIM: BlockSettings = BlockSettings {
        shield_base: 0.45,
        shield_scaling: 0.2,
        weapon_base: 0.3,
        weapon_scaling: 0.2,
        skill_factor: 1.5,
        power_attack_mult: 0.66,
        max_share: 0.85,
    };

    // an Iron Shield (ArmorIronShield 0x12EB6, rating 20) at Block 15,
    // head on, against an Iron Sword (7)
    fn iron_shield() -> BlockFacts {
        BlockFacts {
            blocking: true,
            facing_angle: 0.0,
            ranged: false,
            shield: true,
            shield_rating: 20.0,
            weapon_damage: 7.0,
            block_skill: 15.0,
            power: false,
        }
    }

    fn near(a: f32, b: f32) -> bool {
        (a - b).abs() < 1e-4
    }

    #[test]
    fn a_shield_stops_the_game_s_share() {
        // 45 + 0.2 x 20 x (1 + 15 x 1.5 / 100) = 49.9 percent
        assert!(near(blocked_share(&SKYRIM, &iron_shield()), 0.499));
        // at Block 100: 45 + 0.2 x 20 x 2.5 = 55 percent
        let skilled = BlockFacts { block_skill: 100.0, ..iron_shield() };
        assert!(near(blocked_share(&SKYRIM, &skilled), 0.55));
        // a power attack: times 0.66
        let power = BlockFacts { power: true, ..iron_shield() };
        assert!(near(blocked_share(&SKYRIM, &power), 0.499 * 0.66));
    }

    #[test]
    fn a_weapon_stops_its_own_share() {
        // 30 + 0.2 x 7 x 1.225 = 31.715 percent
        let sword = BlockFacts { shield: false, shield_rating: 0.0, ..iron_shield() };
        assert!(near(blocked_share(&SKYRIM, &sword), 0.31715));
    }

    #[test]
    fn no_block_outside_the_cone_or_without_one() {
        assert!(near(blocked_share(&SKYRIM, &BlockFacts { blocking: false, ..iron_shield() }), 0.0));
        assert!(near(blocked_share(&SKYRIM, &BlockFacts { facing_angle: std::f32::consts::PI, ..iron_shield() }), 0.0));
        assert!(near(blocked_share(&SKYRIM, &BlockFacts { facing_angle: BLOCK_CONE_RAD, ..iron_shield() }), 0.0));
        assert!(blocked_share(&SKYRIM, &BlockFacts { facing_angle: BLOCK_CONE_RAD - 0.01, ..iron_shield() }) > 0.0);
        assert!(near(blocked_share(&SKYRIM, &BlockFacts { facing_angle: f32::NAN, ..iron_shield() }), 0.0));
    }

    #[test]
    fn an_arrow_is_stopped_by_a_shield_only() {
        let arrow = BlockFacts { ranged: true, ..iron_shield() };
        assert!(near(blocked_share(&SKYRIM, &arrow), 0.499));
        let at_a_sword = BlockFacts { ranged: true, shield: false, shield_rating: 0.0, ..iron_shield() };
        assert!(near(blocked_share(&SKYRIM, &at_a_sword), 0.0));
    }

    #[test]
    fn the_cap_holds_and_nonsense_stops_nothing() {
        let capped = BlockSettings { max_share: 0.4, ..SKYRIM };
        assert!(near(blocked_share(&capped, &iron_shield()), 0.4));
        let huge = BlockFacts { shield_rating: f32::INFINITY, ..iron_shield() };
        assert!(near(blocked_share(&SKYRIM, &huge), 0.0));
        let negative = BlockFacts { shield_rating: -50.0, block_skill: -10.0, ..iron_shield() };
        assert!(near(blocked_share(&SKYRIM, &negative), 0.45));
    }
}
