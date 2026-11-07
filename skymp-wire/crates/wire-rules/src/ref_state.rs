//! A reference's engine state (thuum ADR-068 rung 4, the state rows, and its
//! amendment B; docs/research/rung4-state-rows-2026-11-07.md section 1(c)).
//! The engine that owns a reference reports its script's Enable,
//! EnableNoWait, Disable, DisableNoWait or Delete through the seam as a
//! batched state row (ScriptCall 75, wire-rules natives `Keep::Record`); the
//! owner step judged the reporter, and this rule moves the record the server
//! keeps on the reference's change form (MpChangeFormREFR's isDisabled and
//! isDeleted, which the server already saves; amendment B's read 1), which
//! the core then serves and relays as DestructionStage 78's widened word.
//!
//! The record's shape is the engine's: Enable and EnableNoWait clear the
//! disabled flag, Disable and DisableNoWait set it, Delete sets the deleted
//! flag and the disabled one (a deleted game-file reference is disabled on
//! every seat; the engine keeps no deleted reference standing), and the
//! deleted flag never clears, so an Enable after a Delete changes nothing.
//! A call that leaves the record as it was is folded: nothing is written,
//! stamped or relayed (a repeat, the second seat's copy of one lifecycle
//! Disable). The fade the natives take (abFadeIn, abFadeOut) is the
//! receiver's own to show and never recorded. R2, rule 5: the owner's
//! engine ran the call; the value is recorded, never judged.

/// A reference's engine state as the server records it: the game files'
/// state is neither.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct RefState {
    /// The reference is disabled.
    pub disabled: bool,
    /// The reference is deleted (and disabled with it).
    pub deleted: bool,
}

/// What one of the five natives does to the record.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Call {
    /// Enable, EnableNoWait.
    Enable,
    /// Disable, DisableNoWait.
    Disable,
    /// Delete.
    Delete,
}

impl Call {
    /// The call a native of the ref state's rows makes, by its Papyrus name
    /// (compared without case, as Papyrus's); none for any other native.
    #[must_use]
    pub fn of(func: &str) -> Option<Call> {
        let is = |name: &str| func.eq_ignore_ascii_case(name);
        if is("Enable") || is("EnableNoWait") {
            Some(Call::Enable)
        } else if is("Disable") || is("DisableNoWait") {
            Some(Call::Disable)
        } else if is("Delete") {
            Some(Call::Delete)
        } else {
            None
        }
    }
}

/// The rule's word on one call.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Verdict {
    /// The call moved the record: the record after, to write, to stamp with
    /// the call's sequence and to relay.
    Recorded(RefState),
    /// The record stands as it was: nothing is written or relayed.
    Folded(RefState),
}

/// `call` against `record`: the record after, or the record folded.
#[must_use]
pub fn apply(record: RefState, call: Call) -> Verdict {
    let after = match call {
        // a deleted reference stays deleted and disabled: the engine never
        // brings one back
        Call::Enable if record.deleted => record,
        Call::Enable => RefState { disabled: false, deleted: false },
        Call::Disable => RefState { disabled: true, ..record },
        Call::Delete => RefState { disabled: true, deleted: true },
    };
    if after == record {
        Verdict::Folded(record)
    } else {
        Verdict::Recorded(after)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    const ENABLED: RefState = RefState { disabled: false, deleted: false };
    const DISABLED: RefState = RefState { disabled: true, deleted: false };
    const DELETED: RefState = RefState { disabled: true, deleted: true };

    #[test]
    fn the_five_natives_name_their_calls() {
        assert_eq!(Call::of("Enable"), Some(Call::Enable));
        assert_eq!(Call::of("EnableNoWait"), Some(Call::Enable));
        assert_eq!(Call::of("Disable"), Some(Call::Disable));
        assert_eq!(Call::of("disablenowait"), Some(Call::Disable));
        assert_eq!(Call::of("DELETE"), Some(Call::Delete));
        assert_eq!(Call::of("PlayAnimation"), None);
        assert_eq!(Call::of("EnableLinkChain"), None);
    }

    #[test]
    fn a_disable_and_an_enable_move_the_record() {
        // Bleak Falls Temple's cobweb disabled by its host's script, then a
        // quest's Enable of it
        assert_eq!(apply(ENABLED, Call::Disable), Verdict::Recorded(DISABLED));
        assert_eq!(apply(DISABLED, Call::Enable), Verdict::Recorded(ENABLED));
    }

    #[test]
    fn a_repeat_is_folded() {
        // the second seat's copy of one lifecycle Disable
        assert_eq!(apply(DISABLED, Call::Disable), Verdict::Folded(DISABLED));
        assert_eq!(apply(ENABLED, Call::Enable), Verdict::Folded(ENABLED));
        assert_eq!(apply(DELETED, Call::Delete), Verdict::Folded(DELETED));
    }

    #[test]
    fn a_delete_disables_and_never_clears() {
        assert_eq!(apply(ENABLED, Call::Delete), Verdict::Recorded(DELETED));
        assert_eq!(apply(DISABLED, Call::Delete), Verdict::Recorded(DELETED));
        // an Enable or a Disable after it changes nothing
        assert_eq!(apply(DELETED, Call::Enable), Verdict::Folded(DELETED));
        assert_eq!(apply(DELETED, Call::Disable), Verdict::Folded(DELETED));
    }

    proptest! {
        #[test]
        fn deleted_never_clears_and_a_call_lands_once(disabled in any::<bool>(), deleted in any::<bool>(), call in 0u8..3) {
            let record = RefState { disabled, deleted };
            let call = match call {
                0 => Call::Enable,
                1 => Call::Disable,
                _ => Call::Delete,
            };
            match apply(record, call) {
                Verdict::Recorded(after) => {
                    prop_assert!(after != record);
                    prop_assert!(after.deleted || !record.deleted);
                    // a deleted reference is disabled
                    prop_assert!(!after.deleted || after.disabled);
                    // the same call again is folded
                    prop_assert_eq!(apply(after, call), Verdict::Folded(after));
                }
                Verdict::Folded(kept) => prop_assert_eq!(kept, record),
            }
        }
    }
}
