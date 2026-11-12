//! World globals (thuum ADR-068 rung 4 and its amendment B; docs/research/
//! rung4-state-rows-2026-11-07.md section 1(c)). A GLOB that every seat's
//! scripts write (a dungeon's state, a quest's counter: nobody's under D-F1
//! C) is streamed by one seat, the world streamer (wire-rules
//! world_streamer): its game diffs the GLOB array once a second against its
//! last report (TESModPlatform.DiffWorldGlobals) and sends the values that
//! moved as WorldGlobal 80 under the streamer's epoch. The server keeps the
//! world's record on the journal (PartOne::WorldJournal's worldGlobals),
//! relays what changed to every other seat, and serves the record at login
//! and after a restart.
//!
//! The message is judged whole first, by the streamer's epoch (ADR-066
//! decision 4): `E_WORLDGLOBAL_EPOCH` for a stale or a future epoch,
//! `E_WORLDGLOBAL_OWNER` for the epoch from a seat that does not stream.
//! Then each entry on its own, a refused entry dropping alone (the core
//! gathers the facts from the game files and the settings):
//! `E_WORLDGLOBAL_CLOCK` for the clock's six (0x35 GameYear to 0x3a
//! TimeScale in Skyrim.esm, skymp5-client timeService.ts:9-14: the server's
//! own clock, wire-rules clock, writes them), `E_WORLDGLOBAL_NO_GLOB` for a
//! form that is no GLOB, `E_WORLDGLOBAL_CONSTANT` for a constant one (its
//! record's flag, kConstant 0x40, CommonLibSSE-NG include/RE/T/TESGlobal.h:
//! 28: the game files' value on every seat), `E_WORLDGLOBAL_PLAYER` for one on the
//! playerGlobals list (each player's own record, wire-rules player_globals).
//! The record merges the accepted entries: the latest value wins and an equal
//! value is folded (nothing relayed). R2, rule 5: the streamer's engine
//! computed the values; they are recorded, never judged.

/// The clock's first GLOB, GameYear (Skyrim.esm; thuum lab/esm.py,
/// 2026-10-03, as timeService.ts names them).
pub const CLOCK_FIRST: u32 = 0x35;

/// The clock's last GLOB, TimeScale.
pub const CLOCK_LAST: u32 = 0x3a;

/// Why a message or an entry is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// One of the clock's six globals.
    Clock,
    /// A global on the playerGlobals list.
    Player,
    /// A constant global.
    Constant,
    /// No GLOB record.
    NoGlob,
    /// The message's epoch from a seat that does not stream.
    Owner,
    /// The message's epoch is not the streamer's.
    Epoch,
}

impl Code {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::Clock => "E_WORLDGLOBAL_CLOCK",
            Code::Player => "E_WORLDGLOBAL_PLAYER",
            Code::Constant => "E_WORLDGLOBAL_CONSTANT",
            Code::NoGlob => "E_WORLDGLOBAL_NO_GLOB",
            Code::Owner => "E_WORLDGLOBAL_OWNER",
            Code::Epoch => "E_WORLDGLOBAL_EPOCH",
        }
    }
}

/// What the core knows of one entry's global.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct EntryFacts {
    /// The form is a GLOB record.
    pub is_glob: bool,
    /// The GLOB's record flags it constant.
    pub constant: bool,
    /// The global is on the playerGlobals list.
    pub player_listed: bool,
}

/// One entry, `global` its form id: accepted, or refused by its code. The
/// clock is read by number first (no lookup), then the record's facts.
pub fn entry(global: u32, facts: EntryFacts) -> Result<(), Code> {
    if (CLOCK_FIRST..=CLOCK_LAST).contains(&global) {
        return Err(Code::Clock);
    }
    if !facts.is_glob {
        return Err(Code::NoGlob);
    }
    if facts.constant {
        return Err(Code::Constant);
    }
    if facts.player_listed {
        return Err(Code::Player);
    }
    Ok(())
}

/// One global's value, by its form id.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Value {
    /// The GLOB's form id.
    pub global: u32,
    /// Its value (a TESGlobal holds a float whatever its type,
    /// CommonLibSSE-NG include/RE/T/TESGlobal.h:45).
    pub value: f32,
}

/// The record after `accepted`, the entries a report's judgment passed:
/// each global's latest value replaces its recorded one (added when new),
/// and the entries that moved the record, in the report's order, to relay.
/// An entry equal to the record is folded.
#[must_use]
pub fn merge(record: &[Value], accepted: &[Value]) -> (Vec<Value>, Vec<Value>) {
    let mut out = record.to_vec();
    let mut changed = Vec::new();
    for a in accepted {
        match out.iter_mut().find(|r| r.global == a.global) {
            Some(r) if r.value == a.value => {}
            Some(r) => {
                r.value = a.value;
                changed.push(*a);
            }
            None => {
                out.push(*a);
                changed.push(*a);
            }
        }
    }
    (out, changed)
}

#[cfg(test)]
mod tests {
    use super::*;

    const OPEN: EntryFacts = EntryFacts { is_glob: true, constant: false, player_listed: false };

    #[test]
    fn the_clock_constants_listed_globals_and_other_forms_are_refused() {
        for clock in CLOCK_FIRST..=CLOCK_LAST {
            assert_eq!(entry(clock, OPEN), Err(Code::Clock), "{clock:x}");
        }
        assert_eq!(entry(0x34, OPEN), Ok(()));
        assert_eq!(entry(0x3b, OPEN), Ok(()));
        assert_eq!(entry(0x12345, EntryFacts { is_glob: false, ..OPEN }), Err(Code::NoGlob));
        assert_eq!(entry(0x12345, EntryFacts { constant: true, ..OPEN }), Err(Code::Constant));
        assert_eq!(entry(0x12345, EntryFacts { player_listed: true, ..OPEN }), Err(Code::Player));
        // the clock is read before the record: a clock id is the clock's
        // whatever the facts say
        assert_eq!(entry(0x38, EntryFacts { is_glob: false, ..OPEN }), Err(Code::Clock));
        assert_eq!(Code::Clock.name(), "E_WORLDGLOBAL_CLOCK");
        assert_eq!(Code::Player.name(), "E_WORLDGLOBAL_PLAYER");
        assert_eq!(Code::Constant.name(), "E_WORLDGLOBAL_CONSTANT");
        assert_eq!(Code::NoGlob.name(), "E_WORLDGLOBAL_NO_GLOB");
        assert_eq!(Code::Owner.name(), "E_WORLDGLOBAL_OWNER");
        assert_eq!(Code::Epoch.name(), "E_WORLDGLOBAL_EPOCH");
    }

    #[test]
    fn the_latest_value_wins_and_an_equal_one_is_folded() {
        let g = |global, value| Value { global, value };
        let record = [g(0x1000, 1.0), g(0x2000, 0.0)];
        let (after, changed) = merge(&record, &[g(0x1000, 7.0), g(0x2000, 0.0), g(0x3000, 2.5)]);
        assert_eq!(after, vec![g(0x1000, 7.0), g(0x2000, 0.0), g(0x3000, 2.5)]);
        assert_eq!(changed, vec![g(0x1000, 7.0), g(0x3000, 2.5)]);
        // the same report again moves nothing
        let (again, none) = merge(&after, &[g(0x1000, 7.0), g(0x3000, 2.5)]);
        assert_eq!(again, after);
        assert!(none.is_empty());
        // an empty record takes every entry
        let (fresh, all) = merge(&[], &[g(0x1000, 9.0)]);
        assert_eq!((fresh.len(), all.len()), (1, 1));
    }
}
