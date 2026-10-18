//! Headshots (thuum docs/verbs/headshot.md, M2): Headshot Kills - CIF works
//! between players. The shooter's game decides that its arrow struck a bare
//! head (Core Impact Framework, which the server cannot see into) and casts
//! the mod's instant death spell at the victim; the server kills only when
//! everything around that word holds: a bow hit it claimed on that target
//! moments before, the victim's head bare by the equipment record, the
//! victim's race one the mod's rule names, the shooter's base Archery at the
//! rule's floor. The head hit itself is the shooter's game's word (R1 with
//! that one unvalidated fact, Eli 2026-10-12: "it's ok to trust the client a
//! little"). One kill per claimed bow hit.

use std::collections::HashMap;

use crate::clock::SettingsError;
use crate::teleports::{form_spec, FormSpec};
use serde::Deserialize;

/// How long after a claimed bow hit on a target the headshot's report may
/// follow it, milliseconds. CIF judges the arrow's impact at the victim's
/// next hit event, a frame later, and the shooter's game sends both on one
/// reliable channel in order; the window covers a slow frame and the
/// server's own scheduling. A chosen bound.
pub const HEADSHOT_WINDOW_MS: u64 = 1_500;

/// Bow hits kept per shooter; older ones go first.
pub const MAX_BOW_HITS: usize = 16;

#[derive(Debug, Clone, PartialEq, Deserialize, Default)]
#[serde(rename_all = "camelCase", deny_unknown_fields, default)]
struct Settings {
    /// The spells whose hit reports a headshot (the CIF rule's spell), as
    /// `<hex form id>:<plugin>`.
    spells: Vec<String>,
    /// The victim races the rule applies to, the same way.
    races: Vec<String>,
    /// The shooter's base Archery at least this.
    min_archery: f32,
}

/// What the server knows at a headshot's report.
#[derive(Debug, Clone, PartialEq)]
pub struct HeadshotFacts {
    /// Nothing worn in the head and hair slots (ARMO BOD2 slots 30 and 31)
    /// by the victim's equipment record.
    pub head_bare: bool,
    /// The victim's race, as a FormDesc's parts: its id within its plugin.
    pub race_short: u32,
    /// The victim's race's plugin.
    pub race_file: String,
    /// The shooter's base Archery by the actor-values record.
    pub archery: f32,
}

/// The server's answer to a headshot report.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum HeadshotCheck {
    /// Everything holds: the victim dies.
    Killed,
    /// No claimed bow hit on that target within the window, or its headshot
    /// is spent.
    NoShot,
    /// The victim wears something in the head or hair slot.
    Helmeted,
    /// The victim's race is not one the rule names.
    Race,
    /// The shooter's Archery is under the floor, or not a number.
    Archery,
}

#[derive(Debug, Clone, Copy, PartialEq)]
struct BowHit {
    target: u32,
    at_ms: u64,
    spent: bool,
}

/// The rule's settings and every shooter's recent bow hits.
#[derive(Debug, Clone, Default)]
pub struct Headshots {
    spells: Vec<FormSpec>,
    races: Vec<FormSpec>,
    min_archery: f32,
    hits: HashMap<u32, Vec<BowHit>>,
}

impl Headshots {
    /// The `headshot` block of server-settings.json as JSON text; `{}` or an
    /// empty string names no spell, so nothing is a headshot.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let settings: Settings = if text.trim().is_empty() || text.trim() == "null" {
            Settings::default()
        } else {
            serde_json::from_str(text).map_err(|e| SettingsError(format!("headshot: {e}")))?
        };
        if !settings.min_archery.is_finite() || settings.min_archery < 0.0 {
            return Err(SettingsError(format!("headshot: minArchery {} is not a skill level", settings.min_archery)));
        }
        let spells = settings.spells.iter().map(|s| form_spec(s)).collect::<Result<Vec<_>, _>>()?;
        let races = settings.races.iter().map(|s| form_spec(s)).collect::<Result<Vec<_>, _>>()?;
        Ok(Self { spells, races, min_archery: settings.min_archery, hits: HashMap::new() })
    }

    /// Whether the spell `short_id` in `file` reports a headshot.
    pub fn is_headshot_spell(&self, short_id: u32, file: &str) -> bool {
        self.spells.iter().any(|s| s.short_id == short_id && s.file == file)
    }

    /// `actor`'s bow hit on `target` was claimed at `now_ms` (wire-rules
    /// ranged): a headshot may follow it within the window, once.
    pub fn note_bow_hit(&mut self, actor: u32, target: u32, now_ms: u64) {
        let hits = self.hits.entry(actor).or_default();
        hits.retain(|h| now_ms.saturating_sub(h.at_ms) <= HEADSHOT_WINDOW_MS);
        if hits.len() >= MAX_BOW_HITS {
            hits.remove(0);
        }
        hits.push(BowHit { target, at_ms: now_ms, spent: false });
    }

    /// `actor` reports a headshot on `target` at `now_ms` with what the
    /// server knows: the newest unspent bow hit on that target within the
    /// window is spent, and the victim dies when the head is bare, the race
    /// is named and the shooter's Archery reaches the floor. The checks run
    /// in that order and the first failure is the answer; a failed check
    /// spends nothing, so a later report of the same hit is judged afresh
    /// (the facts do not change between them).
    pub fn claim(&mut self, actor: u32, target: u32, facts: &HeadshotFacts, now_ms: u64) -> HeadshotCheck {
        let Some(hits) = self.hits.get_mut(&actor) else {
            return HeadshotCheck::NoShot;
        };
        hits.retain(|h| now_ms.saturating_sub(h.at_ms) <= HEADSHOT_WINDOW_MS);
        let Some(hit) = hits.iter_mut().rev().find(|h| h.target == target && !h.spent) else {
            return HeadshotCheck::NoShot;
        };
        if !facts.head_bare {
            return HeadshotCheck::Helmeted;
        }
        if !self.races.iter().any(|r| r.short_id == facts.race_short && r.file == facts.race_file) {
            return HeadshotCheck::Race;
        }
        if !(facts.archery.is_finite() && facts.archery >= self.min_archery) {
            return HeadshotCheck::Archery;
        }
        hit.spent = true;
        HeadshotCheck::Killed
    }

    /// The actor is gone: its bow hits with it.
    pub fn forget(&mut self, actor: u32) {
        self.hits.remove(&actor);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Headshot Kills.esp's hInstantDeathSpell is 0x801 in its file; the
    // ten playable races and ElderRace (Skyrim.esm, thuum lab/esm.py on
    // sky-srv, 2026-10-17: NordRace 0x13746, ElderRace 0x67CD8)
    const SETTINGS: &str = r#"{"spells": ["801:Headshot Kills.esp"], "races": ["13746:Skyrim.esm", "67cd8:Skyrim.esm"], "minArchery": 60}"#;

    // the settings as parsed; a parse failure gives the default, which the
    // first test's checks would catch
    fn settings() -> Headshots {
        Headshots::from_json(SETTINGS).unwrap_or_default()
    }

    fn nord(archery: f32, head_bare: bool) -> HeadshotFacts {
        HeadshotFacts { head_bare, race_short: 0x13746, race_file: "Skyrim.esm".into(), archery }
    }

    #[test]
    fn the_settings_name_the_spell_and_nothing_else() {
        let h = settings();
        assert!(h.is_headshot_spell(0x801, "Headshot Kills.esp"));
        assert!(!h.is_headshot_spell(0x801, "Skyrim.esm"));
        assert!(!h.is_headshot_spell(0x12fd0, "Skyrim.esm"));
        let none = Headshots::from_json("{}").unwrap_or_default();
        assert!(!none.is_headshot_spell(0x801, "Headshot Kills.esp"));
        assert!(Headshots::from_json("").is_ok());
        assert!(Headshots::from_json(r#"{"spells": ["nonsense"]}"#).is_err());
        assert!(Headshots::from_json(r#"{"minArchery": -1}"#).is_err());
        assert!(Headshots::from_json(r#"{"spell": []}"#).is_err());
    }

    #[test]
    fn a_headshot_rides_a_claimed_bow_hit_once() {
        let mut h = settings();
        assert_eq!(h.claim(1, 2, &nord(60.0, true), 100), HeadshotCheck::NoShot);
        h.note_bow_hit(1, 2, 100);
        assert_eq!(h.claim(1, 2, &nord(60.0, true), 150), HeadshotCheck::Killed);
        // the same hit again: spent
        assert_eq!(h.claim(1, 2, &nord(60.0, true), 200), HeadshotCheck::NoShot);
        // another target's hit is its own
        h.note_bow_hit(1, 3, 300);
        assert_eq!(h.claim(1, 2, &nord(60.0, true), 350), HeadshotCheck::NoShot);
        assert_eq!(h.claim(1, 3, &nord(60.0, true), 350), HeadshotCheck::Killed);
    }

    #[test]
    fn a_headshot_long_after_its_hit_is_refused() {
        let mut h = settings();
        h.note_bow_hit(1, 2, 1_000);
        assert_eq!(h.claim(1, 2, &nord(60.0, true), 1_000 + HEADSHOT_WINDOW_MS), HeadshotCheck::Killed);
        h.note_bow_hit(1, 2, 5_000);
        assert_eq!(h.claim(1, 2, &nord(60.0, true), 5_001 + HEADSHOT_WINDOW_MS), HeadshotCheck::NoShot);
    }

    #[test]
    fn a_helmet_a_foreign_race_or_a_weak_archer_takes_no_kill_and_spends_nothing() {
        let mut h = settings();
        h.note_bow_hit(1, 2, 100);
        assert_eq!(h.claim(1, 2, &nord(60.0, false), 150), HeadshotCheck::Helmeted);
        let orc_dog = HeadshotFacts { race_short: 0x13747, ..nord(60.0, true) };
        assert_eq!(h.claim(1, 2, &orc_dog, 150), HeadshotCheck::Race);
        assert_eq!(h.claim(1, 2, &nord(59.0, true), 150), HeadshotCheck::Archery);
        assert_eq!(h.claim(1, 2, &nord(f32::NAN, true), 150), HeadshotCheck::Archery);
        // the hit is still there for a report whose facts hold
        assert_eq!(h.claim(1, 2, &nord(60.0, true), 150), HeadshotCheck::Killed);
    }

    #[test]
    fn a_forgotten_shooter_has_no_hits() {
        let mut h = settings();
        h.note_bow_hit(1, 2, 100);
        h.forget(1);
        assert_eq!(h.claim(1, 2, &nord(60.0, true), 150), HeadshotCheck::NoShot);
    }
}
