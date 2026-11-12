//! Destruction (thuum D-S3, "traps follow the interaction", ADR-068's
//! DestructionStage row; docs/research/trap-slice-d-s3-2026-11-05.md point
//! 3): a single-use result recorded once. Every seat runs the trap family
//! itself (D-S2 B's cosmetic copies), so the same oil lamp falls, or the same
//! tripwire snaps, on several engines; what must agree is the result the
//! world keeps: a reference's destruction stage and its destroyed flag. The
//! seat whose engine reached a step first reports it (the stage the engine's
//! destruction raised, or SetDestroyed); the server records it (R2: the
//! seat's engine computed it) and tells the reference's other listeners, and
//! a seat that subscribes later, or after a restart, is served the record.
//!
//! The record only moves forward, the engine's own shape: a destruction
//! stage only rises (DamageObject never repairs) and the destroyed flag only
//! goes true for a spent trap. So the first report of each step wins; a
//! report equal to the record, or behind it, changes nothing, which is how
//! the second seat's copy of the same firing is dropped. The core gathers
//! the record (the game files' state when the reference has none: stage 0,
//! not destroyed) and asks here; the reference, its kind and the reporter's
//! loaded cell are the core's checks.

/// The highest stage a report may name: wire-validate's bound (a stage past
/// it is refused at the edge, `E_VAL_RANGE`), kept here for a report that
/// reaches the core another way. A destructible's stages are a handful
/// (Skyrim.esm's oil lamp has two, OilTrapDestroyed.nif and FXEmptyObject.nif;
/// thuum docs/research/trap-census-2026-11-05.md).
pub const MAX_STAGE: u8 = 127;

/// A reference's destruction as the server records it.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Destruction {
    /// The destruction stage the engine reached, 0 for none.
    pub stage: u8,
    /// The reference is destroyed (a spent trap: SetDestroyed(true)).
    pub destroyed: bool,
}

/// The answer to one report.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReportVerdict {
    /// The report moved the record a step (the stage rose, or the reference
    /// went destroyed): the record after, to write and to relay.
    Recorded(Destruction),
    /// The report equals the record or lies behind it: the record stands.
    Stands(Destruction),
    /// A stage past [`MAX_STAGE`]: nothing changes.
    Range,
}

/// A report of `stage` and `destroyed` against `record`: the stage only
/// rises, destroyed only goes true, and a report that moves neither changes
/// nothing (the first report of each step wins).
#[must_use]
pub fn report(record: Destruction, stage: u8, destroyed: bool) -> ReportVerdict {
    if stage > MAX_STAGE {
        return ReportVerdict::Range;
    }
    let after = Destruction { stage: record.stage.max(stage), destroyed: record.destroyed || destroyed };
    if after == record {
        ReportVerdict::Stands(record)
    } else {
        ReportVerdict::Recorded(after)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    const NONE: Destruction = Destruction { stage: 0, destroyed: false };

    #[test]
    fn the_first_report_of_a_stage_rise_is_recorded() {
        // the lamp's first shot: stage 1 over the game files' state
        assert_eq!(report(NONE, 1, false), ReportVerdict::Recorded(Destruction { stage: 1, destroyed: false }));
        // its fall's end, stage 2
        let one = Destruction { stage: 1, destroyed: false };
        assert_eq!(report(one, 2, false), ReportVerdict::Recorded(Destruction { stage: 2, destroyed: false }));
    }

    #[test]
    fn an_equal_report_changes_nothing() {
        // the second seat's copy of the same firing
        let one = Destruction { stage: 1, destroyed: false };
        assert_eq!(report(one, 1, false), ReportVerdict::Stands(one));
        // the game files' state reported as it is
        assert_eq!(report(NONE, 0, false), ReportVerdict::Stands(NONE));
        let spent = Destruction { stage: 0, destroyed: true };
        assert_eq!(report(spent, 0, true), ReportVerdict::Stands(spent));
    }

    #[test]
    fn a_lower_report_changes_nothing() {
        let two = Destruction { stage: 2, destroyed: false };
        assert_eq!(report(two, 1, false), ReportVerdict::Stands(two));
        assert_eq!(report(two, 0, false), ReportVerdict::Stands(two));
    }

    #[test]
    fn destroyed_only_goes_true() {
        // the tripwire's SetDestroyed(true) after its firing
        assert_eq!(report(NONE, 0, true), ReportVerdict::Recorded(Destruction { stage: 0, destroyed: true }));
        // a later report of a whole copy never brings it back
        let spent = Destruction { stage: 0, destroyed: true };
        assert_eq!(report(spent, 0, false), ReportVerdict::Stands(spent));
    }

    #[test]
    fn each_step_counts_on_its_own() {
        // a destroyed report behind the stage keeps the stage and records the
        // flag; a stage report past a destroyed record keeps the flag
        let two = Destruction { stage: 2, destroyed: false };
        assert_eq!(report(two, 1, true), ReportVerdict::Recorded(Destruction { stage: 2, destroyed: true }));
        let spent = Destruction { stage: 0, destroyed: true };
        assert_eq!(report(spent, 3, false), ReportVerdict::Recorded(Destruction { stage: 3, destroyed: true }));
    }

    #[test]
    fn a_stage_past_the_bound_changes_nothing() {
        assert_eq!(report(NONE, MAX_STAGE, false), ReportVerdict::Recorded(Destruction { stage: MAX_STAGE, destroyed: false }));
        assert_eq!(report(NONE, MAX_STAGE.saturating_add(1), false), ReportVerdict::Range);
        assert_eq!(report(NONE, u8::MAX, true), ReportVerdict::Range);
    }

    proptest! {
        #[test]
        fn the_record_never_moves_back_and_a_report_lands_once(
            stage in any::<u8>(), destroyed in any::<bool>(),
            record_stage in 0..=MAX_STAGE, record_destroyed in any::<bool>(),
        ) {
            let record = Destruction { stage: record_stage, destroyed: record_destroyed };
            match report(record, stage, destroyed) {
                ReportVerdict::Recorded(after) => {
                    prop_assert!(after.stage >= record.stage);
                    prop_assert!(after.destroyed || !record.destroyed);
                    prop_assert!(after != record);
                    // the same report again: the first one won
                    prop_assert_eq!(report(after, stage, destroyed), ReportVerdict::Stands(after));
                }
                ReportVerdict::Stands(kept) => {
                    prop_assert_eq!(kept, record);
                    prop_assert!(stage <= record.stage && (!destroyed || record.destroyed));
                }
                ReportVerdict::Range => prop_assert!(stage > MAX_STAGE),
            }
        }
    }
}
