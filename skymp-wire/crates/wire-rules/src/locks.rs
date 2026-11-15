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
//!
//! The owner's write (thuum ADR-073 decision 12, step 3): every lock write a
//! game's engine makes by itself (a script's Lock or SetLockLevel, the owners'
//! door walk at a package's start, an NPC's key or pick) is that game's to
//! report, and the record takes it from the reference's owner, the host of
//! its cell or of its load door's other side (the pair shares one lock in the
//! engine; MpObjectReference::LockPairId). A write equal to the record folds,
//! whoever sends it; a different one from a game that owns neither side is
//! refused, and the server sends that game the record back, so a non-owner
//! renders the owner's lock (ADR-063).

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
    /// The record took the owner's engine write (ADR-073 step 3).
    Written,
    /// Nothing.
    Nothing,
}

/// A lock as a game's engine holds it after its own write (ADR-073 step 3):
/// locked or not, and the level byte (255 requires its key).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EngineLock {
    /// Locked now in that engine.
    pub locked: bool,
    /// The level byte in that engine.
    pub level: u8,
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
    /// An engine write from a game that owns neither the reference nor its
    /// load door's other side (E_LOCK_OWNER); the record goes back to it.
    NotOwner,
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

/// An engine's write of a lock (ADR-073 step 3): `record` the server's lock
/// (None for a reference the record and the game files give none), `written`
/// the reporting engine's lock after its write, `owns` whether the reporter
/// hosts the reference's cell or its load door's other side's. A write equal
/// to the record folds (Nothing), whoever sends it; a lockless reference's
/// unlocked one folds too. Any other write is the owner's to make: recorded
/// with the record's key and broken count kept (no native writes a key), a
/// new lock keyless; refused from a non-owner.
pub fn owner_write(record: Option<LockState>, written: EngineLock, owns: bool) -> Result<(Change, LockState), Refusal> {
    let before = record.unwrap_or(LockState { level: written.level, key: 0, locked: false, broken: 0 });
    let equal = match record {
        Some(state) => state.locked == written.locked && state.level == written.level,
        None => !written.locked,
    };
    if equal {
        return Ok((Change::Nothing, before));
    }
    if !owns {
        return Err(Refusal::NotOwner);
    }
    Ok((Change::Written, LockState { level: written.level, locked: written.locked, ..before }))
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

    #[test]
    fn an_owners_engine_write_is_recorded_and_a_non_owners_refused_unless_it_folds() {
        // thuum ADR-073 step 3: Alvor and Sigrid's house, the pair's lock at
        // level 0 with its key (0x133fb's XLOC, Skyrim.esm), locked at load
        let house = LockState::from_xloc(0, 0x36a3d, true);
        let unlocked = EngineLock { locked: false, level: 0 };
        // the host's morning unlock: recorded, the key and the count kept
        assert_eq!(owner_write(Some(house), unlocked, true), Ok((Change::Written, LockState { locked: false, ..house })));
        // the same write from a guest's engine: refused, the record goes back
        assert_eq!(owner_write(Some(house), unlocked, false), Err(Refusal::NotOwner));
        // a write equal to the record folds, from anyone (an apply's echo, a
        // world quest every seat ran)
        let locked = EngineLock { locked: true, level: 0 };
        assert_eq!(owner_write(Some(house), locked, false), Ok((Change::Nothing, house)));
        assert_eq!(owner_write(Some(house), locked, true), Ok((Change::Nothing, house)));
        // a level alone is a write (SetLockLevel on a standing lock)
        let releveled = EngineLock { locked: true, level: 50 };
        assert_eq!(owner_write(Some(house), releveled, true), Ok((Change::Written, LockState { level: 50, ..house })));
        // a broken count rides along untouched
        let worn = LockState { broken: 3, ..house };
        assert_eq!(owner_write(Some(worn), unlocked, true), Ok((Change::Written, LockState { locked: false, ..worn })));
    }

    #[test]
    fn a_lockless_reference_takes_a_new_keyless_lock_from_its_owner_and_an_unlocked_word_folds() {
        // a script's Lock on a reference the record and the game files give
        // no lock: the engine makes a new one (Papyrus Lock's GetLock or a new
        // REFR_LOCK, HYPOTHESIS by decompile), keyless
        let lock = EngineLock { locked: true, level: 25 };
        assert_eq!(owner_write(None, lock, true), Ok((Change::Written, LockState { level: 25, key: 0, locked: true, broken: 0 })));
        assert_eq!(owner_write(None, lock, false), Err(Refusal::NotOwner));
        // no lock and none written: nothing, from anyone
        let open = EngineLock { locked: false, level: 0 };
        assert_eq!(owner_write(None, open, false).map(|(c, _)| c), Ok(Change::Nothing));
        // a key lock's owner may open it by script (the gate's own fragment)
        let gate = LockState::from_xloc(REQUIRES_KEY_LEVEL, 0xd8f0c, true);
        let opened = EngineLock { locked: false, level: REQUIRES_KEY_LEVEL };
        assert_eq!(owner_write(Some(gate), opened, true), Ok((Change::Written, LockState { locked: false, ..gate })));
    }
}
