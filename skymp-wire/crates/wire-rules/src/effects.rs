//! Ingredient effects a player has learned (thuum docs/verbs/learned-effects.md).
//! The player's engine decides what eating an ingredient teaches (its first
//! unknown effect, more with the Experimenter perk, which only the client's
//! engine knows until M5), and its client reports the ingredient's known
//! effects afterwards. The server keeps a report only when it saw that player
//! eat that ingredient just before, and the record only ever grows: a report
//! never takes an effect away.

/// How long after an eat the server keeps a report of that ingredient's
/// effects: the client waits up to two seconds for its engine's mask to
/// settle; the rest is room for a slow update and the network.
pub const EAT_WINDOW_MS: u64 = 10_000;

/// An ingredient's four effects, one bit each (CommonLibSSE-NG
/// IngredientItem::GameData knownEffectFlags).
pub const EFFECT_BITS: u8 = 0x0f;

/// Whether a report is kept: the player ate that same ingredient, and
/// `since_eat_ms` ago at most [`EAT_WINDOW_MS`].
#[must_use]
pub fn kept(ate_same: bool, since_eat_ms: Option<u64>) -> bool {
    ate_same && since_eat_ms.is_some_and(|ms| ms <= EAT_WINDOW_MS)
}

/// The recorded mask after a kept report: the union of the two, four bits.
#[must_use]
pub const fn union(recorded: u8, reported: u8) -> u8 {
    (recorded | reported) & EFFECT_BITS
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_report_right_after_eating_that_ingredient_is_kept() {
        assert!(kept(true, Some(0)));
        assert!(kept(true, Some(EAT_WINDOW_MS)));
        assert!(!kept(true, Some(EAT_WINDOW_MS + 1)));
    }

    #[test]
    fn a_report_without_the_eat_is_not() {
        assert!(!kept(true, None));
        assert!(!kept(false, Some(0)));
    }

    #[test]
    fn the_record_only_grows_and_stays_four_bits() {
        assert_eq!(union(0b0001, 0b0010), 0b0011);
        assert_eq!(union(0b0111, 0b0001), 0b0111);
        assert_eq!(union(0, 0xff), EFFECT_BITS);
    }
}
