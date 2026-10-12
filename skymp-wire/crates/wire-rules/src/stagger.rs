//! The stagger (thuum docs/verbs/stagger.md, M2). Whether a hit staggers its
//! victim, and how hard, decided by the server from its own records (R0) by
//! the engine's own formula (thuum ghidra/notes/block-formula.md, HYPOTHESIS
//! by thuum rule 2): the hit's stagger (HitData.stagger, set by Address
//! Library ID 44009) is an attack's (ID 26428) or a bash's (ID 26422), less a
//! block's reduction (ID 26426) against a blocking victim, and the victim's
//! hit reaction (ID 38586) gates it. The victim's game then staggers its
//! player at the server's word through the engine's own start (ID 37710),
//! which clamps the magnitude to 1 and applies the perk entry points (perks
//! wait for M5).

/// The game's stagger settings: Skyrim.esm's GMSTs as the caller read them,
/// and the executable's defaults where the masters set none (the notes'
/// table).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct StaggerSettings {
    /// fStaggerMassMult (the executable's 1).
    pub mass_mult: f32,
    /// fStaggerMassBase (the executable's 0).
    pub mass_base: f32,
    /// fStaggerMassOffsetBase (Skyrim.esm 2.1).
    pub mass_offset_base: f32,
    /// fStaggerMassOffsetMult (the executable's 0.15).
    pub mass_offset_mult: f32,
    /// fStaggerPlayerMassMult (Skyrim.esm 2.5): a player victim's mass, scaled,
    /// in an attack's stagger.
    pub player_mass_mult: f32,
    /// fStaggerAttackBase (the executable's 0.5): a bash's base.
    pub bash_base: f32,
    /// fStaggerAttackMult (the executable's 0.01): a bash's per point of the
    /// attacker's Block.
    pub bash_mult: f32,
    /// fStaggerShieldMult (the executable's 1): a shield's bash.
    pub shield_mult: f32,
    /// fStagger1WeapMult (the executable's 0.2): a one-handed weapon's bash.
    pub one_hand_mult: f32,
    /// fStagger2WeapMult (the executable's 0.5): a two-handed weapon's or a
    /// bow's bash.
    pub two_hand_mult: f32,
    /// fStaggerRunningMult (the executable's 1.5): a bash on a running victim.
    pub running_mult: f32,
    /// fStaggerRecoilingMult (the executable's 1.5): a bash on a recoiling one.
    pub recoiling_mult: f32,
    /// fStaggerBlockingMult (Skyrim.esm 0): a bash on a blocking one.
    pub blocking_mult: f32,
    /// fStaggerBlockBase (Skyrim.esm 0.001).
    pub block_base: f32,
    /// fStaggerBlockMult (Skyrim.esm 0.005): per point of the victim's Block.
    pub block_mult: f32,
    /// fStaggerBlockAttackShieldBase (Skyrim.esm 0.01).
    pub block_shield_base: f32,
    /// fStaggerBlockAttackShieldMult (Skyrim.esm 0.001): per point of the
    /// victim's shield rating, or of its drawn weapon's type value.
    pub block_shield_mult: f32,
    /// fStaggerMin (Skyrim.esm 0): a blocked attack's or a bash's floor.
    pub min: f32,
    /// iStaggerAttackChance (Skyrim.esm 50): percent, for the hits nothing
    /// else makes stagger.
    pub attack_chance: u8,
    /// bPlayerAlwaysStaggered:Combat (an INI bool, true by default in the
    /// game): the server's own setting, since a player's INI is its own.
    pub player_always_staggered: bool,
}

/// What the server knows about a hit that may stagger its victim.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct StaggerFacts {
    /// The hit is a bash, as damage-flags backed it.
    pub bash: bool,
    /// The attacker's weapon's stagger (WEAP DNAM at 0x60); 1 without a
    /// weapon, as the engine has it.
    pub weapon_stagger: f32,
    /// The attack's staggerOffset (the attacker's race's ATKD for the attack
    /// it started); 0 for a plain swing in the masters.
    pub stagger_offset: f32,
    /// A bow or a crossbow shot (adds the BowStaggerBonus; such a hit
    /// always staggers when its stagger is above 0).
    pub ranged: bool,
    /// The attacker's BowStaggerBonus (actor value 87).
    pub bow_stagger_bonus: f32,
    /// The attacker's Block (a bash's skill).
    pub attacker_block: f32,
    /// The attacker bashes with a shield.
    pub attacker_shield: bool,
    /// The attacker's weapon is two-handed, a bow or a crossbow (a weapon
    /// bash's multiplier).
    pub attacker_two_handed: bool,
    /// The attacker bashes with no weapon and no shield.
    pub attacker_unarmed: bool,
    /// The victim's Mass (actor value 36; the race's base, 1 for the
    /// playable races).
    pub victim_mass: f32,
    /// The victim is a player (every victim in PvP).
    pub victim_is_player: bool,
    /// The victim runs (a bash's multiplier).
    pub victim_running: bool,
    /// The victim recoils (a bash's multiplier; not recorded yet).
    pub victim_recoiling: bool,
    /// The victim's recorded block is up (the engine tests IsBlocking, before
    /// and apart from the cone).
    pub victim_blocking: bool,
    /// The victim's Block.
    pub victim_block: f32,
    /// The victim's shield's rating, or its drawn weapon's type value
    /// (fStagger1WeapAR and the like), or 0.
    pub victim_block_rating: f32,
    /// The hit was blocked (blocking's verdict: a share above 0).
    pub blocked: bool,
    /// The hit dealt damage (the engine's reaction needs some).
    pub dealt_damage: bool,
}

/// The hit's stagger, HitData.stagger as ID 44009 sets it: a bash's or an
/// attack's, less the block's reduction against a blocking victim (floored
/// at fStaggerMin there). Not clamped: the engine clamps at the start.
#[must_use]
pub fn hit_stagger(s: &StaggerSettings, f: &StaggerFacts) -> f32 {
    let raw_mass = f.victim_mass.max(0.0);
    let stagger = if f.bash {
        // ID 26422: the attacker's Block over the victim's mass, times the
        // victim's state and the attacker's bash, times the bash's offset
        let mut mult = 1.0;
        if f.victim_recoiling {
            mult *= s.recoiling_mult;
        }
        if f.victim_running {
            mult *= s.running_mult;
        }
        if f.victim_blocking {
            mult *= s.blocking_mult;
        }
        let with = if f.attacker_shield {
            s.shield_mult
        } else if f.attacker_unarmed {
            1.0
        } else if f.attacker_two_handed {
            s.two_hand_mult
        } else {
            s.one_hand_mult
        };
        let denom = s.mass_mult * raw_mass + s.mass_base;
        let base = (f.attacker_block.max(0.0) * s.bash_mult + s.bash_base) / denom;
        (base * mult * with * f.stagger_offset).max(s.min)
    } else {
        // ID 26428: the weapon's stagger times the attack's offset over the
        // victim's mass (scaled for a player), less the mass's offset
        let mass = if f.victim_is_player { raw_mass * s.player_mass_mult } else { raw_mass };
        let denom = s.mass_mult * mass + s.mass_base;
        let base = f.weapon_stagger * f.stagger_offset;
        let base = if denom > 0.0001 { base / denom } else { base };
        let offset = ((mass - s.mass_offset_base) * s.mass_offset_mult).max(0.0);
        let bonus = if f.ranged { f.bow_stagger_bonus } else { 0.0 };
        base - offset + bonus
    };
    if !stagger.is_finite() {
        return 0.0;
    }
    if stagger > 0.0 && f.victim_blocking {
        // ID 26426, with the victim's own mass, unscaled
        let offset = ((raw_mass - s.mass_offset_base) * s.mass_offset_mult).max(0.0);
        let reduction = s.block_shield_mult * f.victim_block_rating.max(0.0)
            + s.block_shield_base
            + s.block_mult * f.victim_block.max(0.0)
            + s.block_base
            + offset;
        return (stagger - reduction).max(s.min);
    }
    stagger
}

/// The stagger the server has the victim's game play: 0 for none, else the
/// hit's stagger clamped to 1 as the engine's start clamps it. ID 38586's
/// gate: only a hit that dealt damage (the caller does not ask for one that
/// killed), with a stagger above 0; a player victim always while
/// player_always_staggered holds, a blocked hit, a bash or a bow's or a
/// crossbow's shot always, others when `roll` (0 to 99, the caller's) falls
/// under iStaggerAttackChance. A timed bash (one landing during the victim's
/// own swing, which makes it recoil instead) is not told apart yet.
#[must_use]
pub fn stagger(s: &StaggerSettings, f: &StaggerFacts, roll: u8) -> f32 {
    if !f.dealt_damage {
        return 0.0;
    }
    let magnitude = hit_stagger(s, f);
    if magnitude.is_nan() || magnitude <= 0.0 {
        return 0.0;
    }
    let always = (f.victim_is_player && s.player_always_staggered) || f.blocked || f.bash || f.ranged;
    if always || roll < s.attack_chance {
        magnitude.min(1.0)
    } else {
        0.0
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Skyrim.esm (thuum lab/esm.py, 2026-10-10) and the executable's defaults
    const SKYRIM: StaggerSettings = StaggerSettings {
        mass_mult: 1.0,
        mass_base: 0.0,
        mass_offset_base: 2.1,
        mass_offset_mult: 0.15,
        player_mass_mult: 2.5,
        bash_base: 0.5,
        bash_mult: 0.01,
        shield_mult: 1.0,
        one_hand_mult: 0.2,
        two_hand_mult: 0.5,
        running_mult: 1.5,
        recoiling_mult: 1.5,
        blocking_mult: 0.0,
        block_base: 0.001,
        block_mult: 0.005,
        block_shield_base: 0.01,
        block_shield_mult: 0.001,
        min: 0.0,
        attack_chance: 50,
        player_always_staggered: true,
    };

    // a Nord's forward power attack (staggerOffset 1.0) with an Iron Sword
    // (stagger 0.75) on a player (Mass 1.0) who is not blocking
    fn power_attack() -> StaggerFacts {
        StaggerFacts {
            bash: false,
            weapon_stagger: 0.75,
            stagger_offset: 1.0,
            ranged: false,
            bow_stagger_bonus: 0.0,
            attacker_block: 15.0,
            attacker_shield: false,
            attacker_two_handed: false,
            attacker_unarmed: false,
            victim_mass: 1.0,
            victim_is_player: true,
            victim_running: false,
            victim_recoiling: false,
            victim_blocking: false,
            victim_block: 20.0,
            victim_block_rating: 0.0,
            blocked: false,
            dealt_damage: true,
        }
    }

    fn near(a: f32, b: f32) -> bool {
        (a - b).abs() < 1e-4
    }

    #[test]
    fn a_power_attack_staggers_a_player_by_the_engine_s_formula() {
        // 0.75 x 1.0 / (1 x 1.0 x 2.5 + 0) - (2.5 - 2.1) x 0.15 = 0.24
        assert!(near(hit_stagger(&SKYRIM, &power_attack()), 0.24));
        assert!(near(stagger(&SKYRIM, &power_attack(), 99), 0.24));
    }

    #[test]
    fn a_plain_swing_staggers_nobody() {
        // staggerOffset 0: 0 - 0.06, below 0
        let plain = StaggerFacts { stagger_offset: 0.0, ..power_attack() };
        assert!(hit_stagger(&SKYRIM, &plain) < 0.0);
        assert!(near(stagger(&SKYRIM, &plain, 0), 0.0));
    }

    #[test]
    fn a_block_takes_its_reduction_and_a_blocked_hit_always_staggers() {
        // an Iron Shield (20) at Block 20: 0.001 x 20 + 0.01 + 0.005 x 20 +
        // 0.001 + max(0, (1.0 - 2.1) x 0.15) = 0.131, so 0.24 - 0.131 = 0.109
        let blocked = StaggerFacts { victim_blocking: true, victim_block_rating: 20.0, blocked: true, ..power_attack() };
        assert!(near(hit_stagger(&SKYRIM, &blocked), 0.109));
        let not_always = StaggerSettings { player_always_staggered: false, attack_chance: 0, ..SKYRIM };
        assert!(near(stagger(&not_always, &blocked, 99), 0.109));
        // the reduction tests the block, not the cone: from behind as well
        let behind = StaggerFacts { blocked: false, ..blocked };
        assert!(near(hit_stagger(&SKYRIM, &behind), 0.109));
        // a reduction above the stagger leaves fStaggerMin, 0: none
        let wall = StaggerFacts { victim_block: 100.0, victim_block_rating: 100.0, ..blocked };
        assert!(near(stagger(&SKYRIM, &wall, 0), 0.0));
    }

    #[test]
    fn a_bash_staggers_unless_the_victim_blocks() {
        // a shield bash (bashStart, staggerOffset 0.5) at Block 15:
        // (15 x 0.01 + 0.5) / (1 x 1.0 + 0) x 1 x 0.5 = 0.325
        let bash = StaggerFacts { bash: true, stagger_offset: 0.5, attacker_shield: true, ..power_attack() };
        assert!(near(stagger(&SKYRIM, &bash, 99), 0.325));
        // a one-handed weapon's bash: x 0.2
        let pommel = StaggerFacts { attacker_shield: false, ..bash };
        assert!(near(stagger(&SKYRIM, &pommel, 99), 0.065));
        // on a running victim: x 1.5
        let running = StaggerFacts { victim_running: true, ..bash };
        assert!(near(stagger(&SKYRIM, &running, 99), 0.4875));
        // on a blocking victim: x fStaggerBlockingMult, 0
        let blocker = StaggerFacts { victim_blocking: true, victim_block_rating: 20.0, ..bash };
        assert!(near(stagger(&SKYRIM, &blocker, 0), 0.0));
    }

    #[test]
    fn the_gate_rolls_only_where_nothing_else_staggers() {
        let npc = StaggerFacts { victim_is_player: false, ..power_attack() };
        // an NPC victim: m 1.0, so 0.75 / 1.0 - 0 = 0.75, at the roll
        assert!(near(stagger(&SKYRIM, &npc, 49), 0.75));
        assert!(near(stagger(&SKYRIM, &npc, 50), 0.0));
        // a player while the setting holds: whatever the roll
        let off = StaggerSettings { player_always_staggered: false, ..SKYRIM };
        assert!(near(stagger(&off, &power_attack(), 50), 0.0));
        assert!(near(stagger(&SKYRIM, &power_attack(), 50), 0.24));
        // a bow's shot: always, with its bonus (the Long Bow's stagger is 0)
        let arrow = StaggerFacts { ranged: true, weapon_stagger: 0.0, bow_stagger_bonus: 0.3, ..npc };
        assert!(near(stagger(&SKYRIM, &arrow, 99), 0.3));
        let plain_arrow = StaggerFacts { bow_stagger_bonus: 0.0, ..arrow };
        assert!(near(stagger(&SKYRIM, &plain_arrow, 0), 0.0));
    }

    #[test]
    fn no_damage_no_stagger_and_the_clamp_holds() {
        let none = StaggerFacts { dealt_damage: false, ..power_attack() };
        assert!(near(stagger(&SKYRIM, &none, 0), 0.0));
        let huge = StaggerFacts { weapon_stagger: 20.0, ..power_attack() };
        assert!(near(stagger(&SKYRIM, &huge, 0), 1.0));
        let nonsense = StaggerFacts { weapon_stagger: f32::NAN, ..power_attack() };
        assert!(near(stagger(&SKYRIM, &nonsense, 0), 0.0));
        let massless = StaggerFacts { bash: true, stagger_offset: 0.5, victim_mass: 0.0, ..power_attack() };
        assert!(near(stagger(&SKYRIM, &massless, 0), 0.0));
    }
}
