//! Damage flags (thuum docs/verbs/damage-flags.md). The client says whether
//! its hit was a power attack (twice the damage) and a sneak attack (1.3
//! times); a player keeps a flag only when the server saw it: a power
//! attack's start among its animation events within the last three seconds,
//! and its own sneaking state from its movement. A flag the server cannot
//! back is dropped and the hit lands as a plain one.

/// How long after a power attack's start its hit may come.
pub const POWER_ATTACK_WINDOW_MS: u64 = 3_000;

/// What the server knows about a player's hit and its claims.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FlagFacts {
    /// The client says power attack.
    pub claims_power: bool,
    /// The client says sneak attack.
    pub claims_sneak: bool,
    /// Milliseconds since the attacker's last power attack start, if any.
    pub since_power_start_ms: Option<u64>,
    /// The server holds the attacker sneaking.
    pub is_sneaking: bool,
}

/// The flags the server keeps.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Flags {
    /// Power attack.
    pub power: bool,
    /// Sneak attack.
    pub sneak: bool,
}

/// The flags a hit keeps.
pub fn backed_flags(f: FlagFacts) -> Flags {
    let saw_power = matches!(f.since_power_start_ms, Some(ms) if ms <= POWER_ATTACK_WINDOW_MS);
    Flags { power: f.claims_power && saw_power, sneak: f.claims_sneak && f.is_sneaking }
}

#[cfg(test)]
mod tests {
    use super::*;

    const fn facts(power: bool, sneak: bool, since: Option<u64>, sneaking: bool) -> FlagFacts {
        FlagFacts { claims_power: power, claims_sneak: sneak, since_power_start_ms: since, is_sneaking: sneaking }
    }

    #[test]
    fn a_power_flag_needs_a_recent_power_attack_start() {
        assert!(!backed_flags(facts(true, false, None, false)).power);
        assert!(backed_flags(facts(true, false, Some(800), false)).power);
        assert!(!backed_flags(facts(true, false, Some(3_001), false)).power);
        assert!(!backed_flags(facts(false, false, Some(10), false)).power);
    }

    #[test]
    fn a_sneak_flag_needs_sneaking() {
        assert!(!backed_flags(facts(false, true, None, false)).sneak);
        assert!(backed_flags(facts(false, true, None, true)).sneak);
        assert!(!backed_flags(facts(false, false, None, true)).sneak);
    }
}
