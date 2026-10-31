//! Locks (thuum docs/verbs/locks.md, M4). A locked door or container is
//! locked for every player until the server says otherwise: the lock's level
//! and key come from the game files (the REFR's XLOC), a key in the player's
//! record opens it (R0), a lockpicking success reported by the player's game
//! opens it after the server's check (R1, bounded), a broken pick costs a
//! Lockpick from the record, and the state is the record's. The minigame
//! itself is the seat's (R3): its outcome cannot be recomputed here, so it is
//! judged by what the lock allows and what the player holds.
//!
//! The tiers are the game's (UESP, "Skyrim Mod:Mod File Format/REFR" XLOC: the
//! level byte is 0 or 1 Novice, 25 Apprentice, 50 Adept, 75 Expert, 100
//! Master, and -1 (255) requires its key; Skyrim.esm's Bleak Falls Temple
//! chests read 0, 25 and 0, lab/esm.py 2026-10-31). The bands between the
//! Creation Kit's values follow the engine's LOCK_LEVEL (CommonLibSSE-NG
//! ExtraLock.h:9-18): HYPOTHESIS on where exactly a value between two of them
//! falls (a leveled lock, flag 0x04, writes such a value at runtime).

/// A lock's tier, as the engine names it (CommonLibSSE-NG ExtraLock.h:9-18,
/// LOCK_LEVEL).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Tier {
    /// kUnlocked: the state says unlocked, whatever the level byte.
    Unlocked,
    /// kVeryEasy: Novice, the byte 0 to 24 (the Creation Kit writes 0 or 1).
    Novice,
    /// kEasy: Apprentice, 25 to 49.
    Apprentice,
    /// kAverage: Adept, 50 to 74.
    Adept,
    /// kHard: Expert, 75 to 99.
    Expert,
    /// kVeryHard: Master, 100 to 254.
    Master,
    /// kRequiresKey: 255 (the Creation Kit's -1); no pick opens it.
    RequiresKey,
}

/// The XLOC level byte for a lock that needs its key: the Creation Kit's -1
/// (UESP's REFR page).
pub const REQUIRES_KEY_LEVEL: u8 = 255;
/// The shortest gap between two reports on one lock from one player,
/// milliseconds (UNMEASURED: a pick attempt takes longer than this).
pub const REPORT_GAP_MS: u64 = 500;
/// The most picks one report may say broke.
pub const MAX_BROKEN_PER_REPORT: u8 = 8;

impl Tier {
    /// The tier of a lock whose XLOC level byte is `level`.
    #[must_use]
    pub fn of_level(level: u8) -> Self {
        match level {
            0..=24 => Self::Novice,
            25..=49 => Self::Apprentice,
            50..=74 => Self::Adept,
            75..=99 => Self::Expert,
            100..=254 => Self::Master,
            REQUIRES_KEY_LEVEL => Self::RequiresKey,
        }
    }

    /// The engine's LOCK_LEVEL value, -1 to 5.
    #[must_use]
    pub fn engine_value(self) -> i8 {
        match self {
            Self::Unlocked => -1,
            Self::Novice => 0,
            Self::Apprentice => 1,
            Self::Adept => 2,
            Self::Expert => 3,
            Self::Master => 4,
            Self::RequiresKey => 5,
        }
    }
}

/// A reference's lock as the record holds it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct LockState {
    /// The XLOC level byte (0 none).
    pub level: u8,
    /// The key's base form id, 0 for none.
    pub key: u32,
    /// Locked now.
    pub locked: bool,
    /// Picks broken on it so far (for the record and the log).
    pub broken: u32,
}

impl LockState {
    /// A lock from the game files' XLOC: level, key and whether it stands
    /// locked (a reference with an XLOC starts locked).
    #[must_use]
    pub fn from_xloc(level: u8, key: u32, locked: bool) -> Self {
        Self { level, key, locked, broken: 0 }
    }

    /// The lock's tier.
    #[must_use]
    pub fn tier(self) -> Tier {
        if self.locked {
            Tier::of_level(self.level)
        } else {
            Tier::Unlocked
        }
    }
}

/// What an activation of a locked reference meets.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Activation {
    /// Not locked: the activation proceeds.
    Open,
    /// The player holds the key: unlocked now, the activation proceeds.
    OpenedWithKey,
    /// Locked and no key: refused (E_ACTIVATE_LOCKED); the seat's own menu is
    /// the path.
    Locked,
}

/// The outcome a player's game reports for one attempt.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    /// The lock turned.
    Picked,
    /// Picks broke, this many, and the lock stands.
    Broken(u8),
    /// The menu was left with the lock standing.
    Left,
}

/// What an accepted report changes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Change {
    /// Unlocked now.
    Unlocked,
    /// This many Lockpicks leave the record (clamped to the count held).
    PicksBroken(u8),
    /// Nothing.
    Nothing,
}

/// Why a report changed nothing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Refusal {
    /// The reference is not locked (E_LOCK_OPEN).
    Open,
    /// The player is out of the activation reach (E_LOCK_REACH).
    Reach,
    /// A pick on a lock that needs its key (E_LOCK_KEY).
    Key,
    /// A pick with no Lockpick held (E_LOCK_NO_PICK).
    NoPick,
    /// A broken count of 0 or over the bound (E_LOCK_RANGE).
    Range,
    /// Two reports inside the gap (E_LOCK_RATE).
    Rate,
}

/// `player` activates a reference whose lock is `state`, holding the key or
/// not; the state after.
#[must_use]
pub fn activation(state: LockState, has_key: bool) -> (Activation, LockState) {
    if !state.locked {
        return (Activation::Open, state);
    }
    if state.key != 0 && has_key {
        return (Activation::OpenedWithKey, LockState { locked: false, ..state });
    }
    (Activation::Locked, state)
}

/// A report on a lock: judged against the state, the player's reach, the
/// Lockpicks held and the gap since the player's last report on it; the
/// change and the state after.
pub fn report(
    state: LockState,
    outcome: Outcome,
    within_reach: bool,
    picks_held: u32,
    since_last_ms: Option<u64>,
) -> Result<(Change, LockState), Refusal> {
    if !state.locked {
        return Err(Refusal::Open);
    }
    if !within_reach {
        return Err(Refusal::Reach);
    }
    if since_last_ms.is_some_and(|gap| gap < REPORT_GAP_MS) {
        return Err(Refusal::Rate);
    }
    match outcome {
        Outcome::Picked => {
            if state.tier() == Tier::RequiresKey {
                return Err(Refusal::Key);
            }
            if picks_held == 0 {
                return Err(Refusal::NoPick);
            }
            Ok((Change::Unlocked, LockState { locked: false, ..state }))
        }
        Outcome::Broken(count) => {
            if count == 0 || count > MAX_BROKEN_PER_REPORT {
                return Err(Refusal::Range);
            }
            if state.tier() == Tier::RequiresKey {
                return Err(Refusal::Key);
            }
            let held = u8::try_from(picks_held).unwrap_or(u8::MAX);
            let taken = count.min(held);
            if taken == 0 {
                return Err(Refusal::NoPick);
            }
            Ok((Change::PicksBroken(taken), LockState { broken: state.broken.saturating_add(u32::from(taken)), ..state }))
        }
        Outcome::Left => Ok((Change::Nothing, state)),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const KEY: u32 = 0x1234;

    fn novice() -> LockState {
        LockState::from_xloc(10, 0, true)
    }

    #[test]
    fn the_tiers_follow_the_games_levels() {
        // the Creation Kit's values (UESP's REFR page), and Skyrim.esm's own
        // chests: Bleak Falls Temple's 0x371e9 and 0x54b11 at 0, 0x3fa69 at 25
        assert_eq!(Tier::of_level(0), Tier::Novice);
        assert_eq!(Tier::of_level(1), Tier::Novice);
        assert_eq!(Tier::of_level(24), Tier::Novice);
        assert_eq!(Tier::of_level(25), Tier::Apprentice);
        assert_eq!(Tier::of_level(49), Tier::Apprentice);
        assert_eq!(Tier::of_level(50), Tier::Adept);
        assert_eq!(Tier::of_level(74), Tier::Adept);
        assert_eq!(Tier::of_level(75), Tier::Expert);
        assert_eq!(Tier::of_level(99), Tier::Expert);
        assert_eq!(Tier::of_level(100), Tier::Master);
        assert_eq!(Tier::of_level(254), Tier::Master);
        assert_eq!(Tier::of_level(REQUIRES_KEY_LEVEL), Tier::RequiresKey);
        for (tier, value) in [(Tier::Unlocked, -1), (Tier::Novice, 0), (Tier::Master, 4), (Tier::RequiresKey, 5)] {
            assert_eq!(tier.engine_value(), value);
        }
        // an unlocked record reads unlocked whatever its level; a locked one
        // at 0 is a Novice lock
        assert_eq!(LockState::from_xloc(50, 0, false).tier(), Tier::Unlocked);
        assert_eq!(LockState::from_xloc(0, 0, true).tier(), Tier::Novice);
        assert_eq!(novice().tier(), Tier::Novice);
    }

    #[test]
    fn a_key_in_the_record_opens_and_nothing_else_does_at_the_activation() {
        let keyed = LockState::from_xloc(REQUIRES_KEY_LEVEL, KEY, true);
        assert_eq!(activation(keyed, false), (Activation::Locked, keyed));
        let (verdict, after) = activation(keyed, true);
        assert_eq!(verdict, Activation::OpenedWithKey);
        assert!(!after.locked);
        assert_eq!(after.tier(), Tier::Unlocked);
        // a lock with no key is not opened by "having the key"
        assert_eq!(activation(novice(), true), (Activation::Locked, novice()));
        // an open reference proceeds
        assert_eq!(activation(after, false), (Activation::Open, after));
    }

    #[test]
    fn a_pick_opens_a_lock_a_pick_can_open_and_is_refused_by_name_otherwise() {
        let (change, after) = report(novice(), Outcome::Picked, true, 3, None).unwrap_or((Change::Nothing, novice()));
        assert_eq!(change, Change::Unlocked);
        assert!(!after.locked);
        assert_eq!(report(after, Outcome::Picked, true, 3, None), Err(Refusal::Open));
        assert_eq!(report(novice(), Outcome::Picked, false, 3, None), Err(Refusal::Reach));
        assert_eq!(report(novice(), Outcome::Picked, true, 0, None), Err(Refusal::NoPick));
        let keyed = LockState::from_xloc(REQUIRES_KEY_LEVEL, KEY, true);
        assert_eq!(report(keyed, Outcome::Picked, true, 3, None), Err(Refusal::Key));
        assert_eq!(report(keyed, Outcome::Broken(1), true, 3, None), Err(Refusal::Key));
        // the gap
        assert_eq!(report(novice(), Outcome::Picked, true, 3, Some(REPORT_GAP_MS - 1)), Err(Refusal::Rate));
        assert!(report(novice(), Outcome::Picked, true, 3, Some(REPORT_GAP_MS)).is_ok());
        // a master lock is pickable
        let master = LockState::from_xloc(100, 0, true);
        assert!(report(master, Outcome::Picked, true, 1, None).is_ok());
    }

    #[test]
    fn broken_picks_cost_what_is_held_and_no_more() {
        let (change, after) = report(novice(), Outcome::Broken(3), true, 10, None).unwrap_or((Change::Nothing, novice()));
        assert_eq!(change, Change::PicksBroken(3));
        assert!(after.locked);
        assert_eq!(after.broken, 3);
        // clamped to the two held
        let (change, after) = report(after, Outcome::Broken(5), true, 2, None).unwrap_or((Change::Nothing, after));
        assert_eq!(change, Change::PicksBroken(2));
        assert_eq!(after.broken, 5);
        // none held: nothing to break
        assert_eq!(report(novice(), Outcome::Broken(1), true, 0, None), Err(Refusal::NoPick));
        // the bounds
        assert_eq!(report(novice(), Outcome::Broken(0), true, 10, None), Err(Refusal::Range));
        assert_eq!(report(novice(), Outcome::Broken(MAX_BROKEN_PER_REPORT + 1), true, 10, None), Err(Refusal::Range));
        // leaving changes nothing
        assert_eq!(report(novice(), Outcome::Left, true, 0, None), Ok((Change::Nothing, novice())));
    }
}
