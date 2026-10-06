//! A player's favorites (thuum docs/verbs/favorites.md): the items and magic
//! it marked in its menus, each with the hotkey bound to it. Its client
//! reports the whole list after a menu where favorites change closes. The
//! server keeps the items the player holds; it records magic as reported,
//! since its spell list holds only spells learned through the server, not the
//! starting spells, race powers or shouts a player's engine knows, and the
//! client marks magic only when its engine knows it; anything else is
//! dropped. A kept report replaces the record, so unmarking sticks.

/// Favorites one player keeps (wire-schema `cap::FAVORITES`).
pub const MAX: usize = 128;

/// The keys a favorite can be bound to, 0 to 7 for the keys 1 to 8; -1 is
/// none (CommonLibSSE-NG include/RE/E/ExtraHotkey.h).
pub const KEYS: usize = 8;

/// What the server knows of a reported favorite's form.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    /// An item the player holds in the server's inventory.
    HeldItem,
    /// An item the player does not hold.
    MissingItem,
    /// A spell or a shout in the master files.
    Magic,
    /// Anything else: no such form, another record type, or a form made in
    /// a session.
    Other,
}

/// One favorite.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Entry {
    /// The form id.
    pub form: u32,
    /// -1 for none, 0 to 7.
    pub hotkey: i8,
}

/// The favorites kept from a report, in its order: held items and magic,
/// the first entry per form, the first claim per key (a later claim keeps
/// its entry without the key), at most [`MAX`].
#[must_use]
pub fn kept(report: &[(Entry, Kind)]) -> Vec<Entry> {
    let mut out: Vec<Entry> = Vec::new();
    let mut taken = [false; KEYS];
    for &(e, kind) in report {
        if out.len() >= MAX {
            break;
        }
        if !matches!(kind, Kind::HeldItem | Kind::Magic) || out.iter().any(|o| o.form == e.form) {
            continue;
        }
        let hotkey = match usize::try_from(e.hotkey).ok().and_then(|k| taken.get_mut(k)) {
            Some(t) if !*t => {
                *t = true;
                e.hotkey
            }
            _ => -1,
        };
        out.push(Entry { form: e.form, hotkey });
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn e(form: u32, hotkey: i8) -> Entry {
        Entry { form, hotkey }
    }

    #[test]
    fn held_items_and_magic_are_kept_in_order() {
        let report = [(e(1, 2), Kind::HeldItem), (e(2, -1), Kind::Magic), (e(3, 0), Kind::Magic)];
        assert_eq!(kept(&report), vec![e(1, 2), e(2, -1), e(3, 0)]);
    }

    #[test]
    fn missing_items_and_other_forms_are_dropped_and_free_their_key() {
        let report = [(e(1, 4), Kind::MissingItem), (e(2, 5), Kind::Other), (e(3, 4), Kind::HeldItem)];
        assert_eq!(kept(&report), vec![e(3, 4)]);
    }

    #[test]
    fn the_first_entry_per_form_and_the_first_claim_per_key_win() {
        let report = [(e(1, 3), Kind::HeldItem), (e(1, 6), Kind::HeldItem), (e(2, 3), Kind::Magic)];
        assert_eq!(kept(&report), vec![e(1, 3), e(2, -1)]);
    }

    #[test]
    fn a_key_out_of_range_is_dropped_not_the_entry() {
        let report = [(e(1, 8), Kind::HeldItem), (e(2, i8::MIN), Kind::Magic), (e(3, 7), Kind::Magic)];
        assert_eq!(kept(&report), vec![e(1, -1), e(2, -1), e(3, 7)]);
    }

    #[test]
    fn at_most_max_are_kept() {
        let report: Vec<(Entry, Kind)> = (0..300u32).map(|f| (e(f, -1), Kind::HeldItem)).collect();
        let got = kept(&report);
        assert_eq!(got.len(), MAX);
        assert_eq!(got.last().map(|x| x.form), u32::try_from(MAX - 1).ok());
    }
}
