//! A player's attribute percentages (thuum docs/verbs/attributes.md): the
//! client reports Health, Magicka and Stamina as it regenerates them (R1,
//! cropped to the regeneration model), and the server sets them itself
//! (R0: a heal it applied, a drink, an owner's or the lab's set). A value
//! the server RAISED holds against a report that does not reach it yet,
//! since the client's next report may predate the server's message and
//! would read as a fall (the snap-back of x-class-target2, 2026-10-18); the
//! hold ends with the first report at or above the value (the client
//! applied it and may have regenerated since), or after [`HOLD_MS`] (the
//! arrival's wait, movement.rs) for a client that never reaches it. A value
//! the server lowered needs no hold: a stale higher report is cropped to
//! the regeneration since.

/// How far below a held value a report may sit and still count as the value
/// applied: a float's round trip through the engine.
pub const APPLIED_WITHIN: f32 = 2.0e-3;
/// How long a hold stands without a report reaching it.
pub const HOLD_MS: u64 = 10_000;

/// A percentage the server raised, awaiting the client's report of it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Hold {
    /// The actor value: Health 24, Magicka 25, Stamina 26.
    pub av: u8,
    /// The percentage the server set, 0 to 1.
    pub value: f32,
    /// When the hold lapses, Unix milliseconds.
    pub until_ms: u64,
}

/// Arms a hold on `av` at `value`: a standing hold on the same value is
/// replaced. A value that is not finite arms nothing.
pub fn hold(holds: &mut Vec<Hold>, av: u8, value: f32, now_ms: u64) {
    if !value.is_finite() {
        return;
    }
    holds.retain(|h| h.av != av);
    holds.push(Hold { av, value, until_ms: now_ms.saturating_add(HOLD_MS) });
}

/// Whether a report of `reported` on `av` is taken. Lapsed holds go first.
/// With no hold on `av` it is taken. A report at or above the held value
/// ends the hold and is taken; one below it is a stale report, dropped.
pub fn report(holds: &mut Vec<Hold>, av: u8, reported: f32, now_ms: u64) -> bool {
    holds.retain(|h| h.until_ms > now_ms);
    let Some(held) = holds.iter().find(|h| h.av == av).map(|h| h.value) else {
        return true;
    };
    if !reported.is_finite() {
        return false;
    }
    if reported + APPLIED_WITHIN >= held {
        holds.retain(|h| h.av != av);
        return true;
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_raised_value_holds_until_a_report_reaches_it() {
        let mut holds = Vec::new();
        hold(&mut holds, 24, 1.0, 1_000);
        // the client's stale report from before the heal: dropped
        assert!(!report(&mut holds, 24, 0.6, 1_500));
        assert_eq!(holds.len(), 1);
        // another value is free
        assert!(report(&mut holds, 26, 0.3, 1_500));
        // the report that carries the heal ends the hold
        assert!(report(&mut holds, 24, 0.999, 2_500));
        assert!(holds.is_empty());
        // a later real fall is taken
        assert!(report(&mut holds, 24, 0.9, 3_000));
    }

    #[test]
    fn a_report_above_the_held_value_counts_as_applied() {
        let mut holds = Vec::new();
        hold(&mut holds, 25, 0.8, 0);
        assert!(report(&mut holds, 25, 0.83, 1_000));
        assert!(holds.is_empty());
    }

    #[test]
    fn a_hold_lapses_and_a_new_hold_replaces_the_old() {
        let mut holds = Vec::new();
        hold(&mut holds, 24, 1.0, 0);
        hold(&mut holds, 24, 0.7, 100);
        assert_eq!(holds.len(), 1);
        assert_eq!(holds.first().map(|h| h.value), Some(0.7));
        assert!(!report(&mut holds, 24, 0.5, 5_000));
        assert!(report(&mut holds, 24, 0.5, 100 + HOLD_MS));
        assert!(holds.is_empty());
        hold(&mut holds, 24, f32::NAN, 0);
        assert!(holds.is_empty());
        hold(&mut holds, 24, 1.0, 0);
        assert!(!report(&mut holds, 24, f32::NAN, 10));
    }
}
