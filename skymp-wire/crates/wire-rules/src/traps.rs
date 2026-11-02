//! A trap's harm on another player's body (thuum docs/verbs/traps.md, ADR-059
//! decision 4). The cell's host's engine runs the trap's scripts: TrapBase's
//! fireTrap resolves MovingTrap's leveled pick (LvlDamage1 to LvlDamage6 by
//! the encounter level against LvlThreshold1 to LvlThreshold5, the engine's
//! CalculateEncounterLevel on the host) into TrapHitBase's `damage`, and
//! TrapHitBase.OnTrapHitStart hands it to the engine's ProcessTrapHit. The
//! host reports the number its engine dealt a guest's figure; the server owns
//! the guest's health, so it checks the number against the trap's own table
//! (every value its scripts could have picked, read from the game files by
//! the core) and the hit against the trap's reach, then applies the table's
//! value. R1: the host computes, the server validates. The encounter level
//! itself (the zone, the player's level, the trap's difficulty) is the
//! engine's system and is not recomputed here.

/// How far from a trap's origin a hit body may stand (units). The swinging
/// wall 0x54b15 sweeps an arc over its plate; the server measures from the
/// trap reference's origin to the target's recorded feet. UNMEASURED: the
/// death path's hazard radius, until the probe x-trap-hit reads the real
/// distance.
pub const REACH: f32 = 512.0;

/// How close a reported number must come to a table value: the scripts deal
/// the table's integers, the report is a float.
pub const TOLERANCE: f32 = 0.5;

/// Why a trap hit report is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Refusal {
    /// The reference carries no trap script with a damage table.
    Table,
    /// The number is not finite or below zero.
    Range,
    /// The target stands farther than [`REACH`] from the trap.
    Reach,
    /// The number is none the trap's scripts could pick.
    Damage,
}

/// Judge a host's trap hit report: `table` the damages the trap's scripts
/// could pick (MovingTrap's six levels, TrapHitBase's own `damage` when the
/// record sets it), `reported` the number the host's engine dealt, `distance`
/// from the trap's origin to the target's recorded position. Returns the
/// table's value to apply.
pub fn hit(table: &[f32], reported: f32, distance: f32) -> Result<f32, Refusal> {
    if table.is_empty() {
        return Err(Refusal::Table);
    }
    if !reported.is_finite() || reported < 0.0 {
        return Err(Refusal::Range);
    }
    if !distance.is_finite() || distance > REACH {
        return Err(Refusal::Reach);
    }
    table
        .iter()
        .copied()
        .filter(|v| v.is_finite() && *v >= 0.0)
        .find(|v| (v - reported).abs() <= TOLERANCE)
        .ok_or(Refusal::Damage)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// TrapWallWood 0xc6440's TrapSwingingWall table (Skyrim.esm VMAD).
    const WALL: [f32; 6] = [75.0, 100.0, 125.0, 150.0, 175.0, 200.0];

    #[test]
    fn the_tables_values_pass_within_reach_and_nothing_else_does() {
        assert_eq!(hit(&WALL, 75.0, 100.0), Ok(75.0));
        assert_eq!(hit(&WALL, 200.2, REACH), Ok(200.0));
        assert_eq!(hit(&WALL, 76.0, 100.0), Err(Refusal::Damage));
        assert_eq!(hit(&WALL, 0.0, 100.0), Err(Refusal::Damage));
        assert_eq!(hit(&WALL, 75.0, 513.0), Err(Refusal::Reach));
        assert_eq!(hit(&WALL, 75.0, f32::NAN), Err(Refusal::Reach));
        assert_eq!(hit(&WALL, f32::NAN, 100.0), Err(Refusal::Range));
        assert_eq!(hit(&WALL, -75.0, 100.0), Err(Refusal::Range));
        assert_eq!(hit(&[], 75.0, 100.0), Err(Refusal::Table));
    }

    #[test]
    fn a_traps_own_damage_property_counts_as_its_table() {
        // a bear trap or a spike wall without a leveled pick: TrapHitBase's
        // damage alone, which may be zero (TrapBase's fallback)
        assert_eq!(hit(&[30.0], 30.0, 10.0), Ok(30.0));
        assert_eq!(hit(&[0.0], 0.0, 10.0), Ok(0.0));
        assert_eq!(hit(&[f32::NAN, 30.0], 30.0, 10.0), Ok(30.0));
    }
}
