//! A host's state report for an actor it simulates (thuum docs/verbs/
//! npc-stream.md, ADR-050 decision 1, slice 2). The host's engine runs the
//! actor's regeneration and its fights, so its percentages are the record's
//! source (R2), bounded by the server's own numbers: a report that would
//! undo a value the server lowered lately (a hit it numbered that the host's
//! engine has not applied yet) is dropped, as is one below a value the
//! server raised that the host has not reported yet (attributes.rs's hold,
//! the players' rule); a report equal to the record changes nothing; a
//! health that would leave a living record with less than one point is
//! floored at that point (god-mode's TIM number) until ADR-050's deaths-by-
//! the-engine train takes a death from the host's word; a report for a dead
//! or respawning record is dropped whole (npc-fate owns the transition). The
//! core gathers the facts (the record, the hold table's answers, the time
//! since its last lowering) and asks here.

/// How long a value the server lowered holds against a higher report: the
/// hold table's own window (attributes.rs HOLD_MS).
pub const LOWERED_HOLD_MS: u64 = 10_000;

/// Two percentages that agree within this are the same value (the core's
/// IsNearlyEqual margin, 1/1024).
pub const SAME_WITHIN: f32 = 1.0 / 1024.0;

/// What the core knows about one value.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ValueFacts {
    /// The host's percentage, 0 to 1 (the validator bounded it).
    pub reported: f32,
    /// The record's percentage.
    pub recorded: f32,
    /// The hold table refuses this report: the server raised the value and
    /// the host has not reported it yet (attributes.rs report answered false).
    pub held: bool,
    /// Milliseconds since the server last lowered this value by its own
    /// number; `None` when it never did.
    pub lowered_ago_ms: Option<u64>,
}

/// The facts for one report.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Facts {
    /// Health.
    pub health: ValueFacts,
    /// Magicka.
    pub magicka: ValueFacts,
    /// Stamina.
    pub stamina: ValueFacts,
    /// The record is alive and not respawning.
    pub alive: bool,
    /// One point of health as a percentage of the actor's maximum (the floor).
    pub one_point: f32,
}

/// The verdict for one value.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ValueVerdict {
    /// The percentage to record.
    pub record: f32,
    /// Whether the record changes (a changed value travels to the viewers).
    pub changed: bool,
    /// Whether the report was dropped against the server's number.
    pub dropped: bool,
}

/// The verdict for one report.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Verdict {
    /// Health.
    pub health: ValueVerdict,
    /// Magicka.
    pub magicka: ValueVerdict,
    /// Stamina.
    pub stamina: ValueVerdict,
    /// Whether the health was floored at one point.
    pub floored: bool,
    /// Whether the whole report was dropped (a dead or respawning record).
    pub dead: bool,
}

fn judge_value(f: ValueFacts) -> ValueVerdict {
    let kept = ValueVerdict { record: f.recorded, changed: false, dropped: false };
    if !f.reported.is_finite() {
        return ValueVerdict { dropped: true, ..kept };
    }
    if f.held {
        return ValueVerdict { dropped: true, ..kept };
    }
    let lowered_lately = f.lowered_ago_ms.is_some_and(|ago| ago < LOWERED_HOLD_MS);
    if f.reported > f.recorded + SAME_WITHIN && lowered_lately {
        return ValueVerdict { dropped: true, ..kept };
    }
    if (f.reported - f.recorded).abs() <= SAME_WITHIN {
        return kept;
    }
    ValueVerdict { record: f.reported, changed: true, dropped: false }
}

/// The report's verdict: each value judged on its own, the health floored.
pub fn judge(f: &Facts) -> Verdict {
    let dropped = |v: ValueFacts| ValueVerdict { record: v.recorded, changed: false, dropped: true };
    if !f.alive {
        return Verdict {
            health: dropped(f.health),
            magicka: dropped(f.magicka),
            stamina: dropped(f.stamina),
            floored: false,
            dead: true,
        };
    }
    let mut health = judge_value(f.health);
    let mut floored = false;
    if !health.dropped && health.record < f.one_point {
        health.record = f.one_point;
        health.changed = (f.health.recorded - f.one_point).abs() > SAME_WITHIN;
        floored = true;
    }
    Verdict { health, magicka: judge_value(f.magicka), stamina: judge_value(f.stamina), floored, dead: false }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn value(reported: f32, recorded: f32) -> ValueFacts {
        ValueFacts { reported, recorded, held: false, lowered_ago_ms: None }
    }

    fn facts(health: ValueFacts) -> Facts {
        Facts { health, magicka: value(1.0, 1.0), stamina: value(1.0, 1.0), alive: true, one_point: 0.01 }
    }

    #[test]
    fn an_equal_report_changes_nothing_and_a_different_one_is_recorded() {
        let same = judge(&facts(value(0.5, 0.5)));
        assert!(!same.health.changed && !same.health.dropped);
        assert_eq!(same.health.record, 0.5);
        let fell = judge(&facts(value(0.4, 0.9)));
        assert!(fell.health.changed && !fell.health.dropped);
        assert_eq!(fell.health.record, 0.4);
        let rose = judge(&facts(value(0.95, 0.9)));
        assert!(rose.health.changed);
        assert_eq!(rose.health.record, 0.95);
        assert!(!same.dead && !fell.floored);
    }

    #[test]
    fn a_report_that_would_undo_a_recent_lowering_is_dropped_until_the_hold_lapses() {
        let hit = ValueFacts { reported: 0.9, recorded: 0.6, held: false, lowered_ago_ms: Some(400) };
        let v = judge(&facts(hit));
        assert!(v.health.dropped && !v.health.changed);
        assert_eq!(v.health.record, 0.6);
        // the host's lower reading after the hit is taken at once
        let applied = ValueFacts { reported: 0.55, recorded: 0.6, held: false, lowered_ago_ms: Some(400) };
        assert!(judge(&facts(applied)).health.changed);
        // past the hold, a higher report is the host's regeneration
        let later = ValueFacts { lowered_ago_ms: Some(LOWERED_HOLD_MS), ..hit };
        assert!(judge(&facts(later)).health.changed);
    }

    #[test]
    fn a_held_value_drops_the_report_and_other_values_are_free() {
        let f = Facts {
            health: ValueFacts { reported: 0.3, recorded: 1.0, held: true, lowered_ago_ms: None },
            magicka: value(0.2, 0.7),
            stamina: value(1.0, 1.0),
            alive: true,
            one_point: 0.01,
        };
        let v = judge(&f);
        assert!(v.health.dropped);
        assert_eq!(v.health.record, 1.0);
        assert!(v.magicka.changed);
        assert_eq!(v.magicka.record, 0.2);
        assert!(!v.stamina.changed && !v.stamina.dropped);
    }

    #[test]
    fn a_zero_on_a_living_record_is_floored_at_one_point() {
        let v = judge(&facts(value(0.0, 0.8)));
        assert!(v.floored && v.health.changed && !v.health.dropped);
        assert_eq!(v.health.record, 0.01);
        // already at the floor: floored again, nothing changes
        let again = judge(&facts(value(0.0, 0.01)));
        assert!(again.floored && !again.health.changed);
    }

    #[test]
    fn a_dead_record_drops_the_whole_report() {
        let f = Facts { alive: false, ..facts(value(0.5, 0.0)) };
        let v = judge(&f);
        assert!(v.dead && v.health.dropped && v.magicka.dropped && v.stamina.dropped);
        assert_eq!(v.health.record, 0.0);
    }

    #[test]
    fn a_report_that_is_not_a_number_is_dropped() {
        let v = judge(&facts(value(f32::NAN, 0.5)));
        assert!(v.health.dropped);
        assert_eq!(v.health.record, 0.5);
    }
}
