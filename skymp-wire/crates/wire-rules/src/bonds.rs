//! Bonds (thuum docs/verbs/shadowbond.md, train-27). Apocalypse's Shadowbond
//! turns its caster and its target invisible and, the first time either
//! loses the invisibility, swaps their places; the mod's script runs on the
//! target's effect and moves each game's own player (apocrypha's read of
//! WB_InvisProcTeleport_Script, 2026-10-18). The server owns the two
//! positions: it records the bond when the spell's hit is claimed, ends it
//! at the first act that ends either actor's concealment (or at the spell's
//! duration), and at the break permits each actor one jump to where the
//! other stands, for a short window. The jumps themselves are the movement
//! rule's cast-jump permits; this is the bookkeeping.
use crate::clock::SettingsError;
use crate::forms::FormDesc;
use serde::Deserialize;

/// How long a bond stands when its entry names no `durationMs`: the
/// spell's 20 s (Shadowbond's SPEL, apocrypha's read).
pub const BOND_DURATION_MS: u64 = 20_000;
/// How long after the break each landing is permitted when the entry names
/// no `windowMs`: the script polls every 0.2 s, the engine's MoveTo is
/// instant, a report every 130 ms, and the two games break on their own
/// polls.
pub const BOND_WINDOW_MS: u64 = 1_500;
/// How far from the other's recorded position a landing may fall when the
/// entry names no `slackUnits`: the two games see the other where its last
/// report put it.
pub const BOND_SLACK_UNITS: f32 = 256.0;
/// Bonds kept per actor as caster; a new bond by the same caster replaces
/// its old one.
pub const MAX_BONDS: usize = 64;

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Entry {
    spell: String,
    duration_ms: Option<u64>,
    window_ms: Option<u64>,
    slack_units: Option<f32>,
}

/// One spell the server names as a bond.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BondSpell {
    /// How long the bond stands after the hit, ms.
    pub duration_ms: u64,
    /// How long after the break each landing is permitted, ms.
    pub window_ms: u64,
    /// How far from the other's position a landing may fall, units.
    pub slack_units: f32,
}

#[derive(Debug, Clone, PartialEq)]
struct Named {
    short_id: u32,
    file: String,
    spell: BondSpell,
}

/// A bond standing between two actors.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Bond {
    /// Who cast the spell.
    pub caster: u32,
    /// Whom it landed on.
    pub target: u32,
    /// When the bond ends by itself (a monotonic clock, ms).
    pub until_ms: u64,
    /// How long after the break each landing is permitted, ms.
    pub window_ms: u64,
    /// How far from the other's position a landing may fall, units.
    pub slack_units: f32,
}

/// The spells the server names as bonds, and the bonds standing.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Bonds {
    spells: Vec<Named>,
    bonds: Vec<Bond>,
}

impl Bonds {
    /// The `bonds` block as JSON text, a list of entries (`[]` for none):
    /// each a spell written "<hex form id>:<plugin>" and, optionally, its
    /// `durationMs`, `windowMs` and `slackUnits`; the error names the bad
    /// entry.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let entries: Vec<Entry> = serde_json::from_str(text).map_err(|why| SettingsError(format!("bonds: {why}")))?;
        let mut spells = Vec::with_capacity(entries.len());
        for entry in entries {
            let FormDesc { short_id, file } =
                FormDesc::parse(&entry.spell).map_err(|why| SettingsError(format!("bonds: {why}")))?;
            let slack_units = entry.slack_units.unwrap_or(BOND_SLACK_UNITS);
            if !slack_units.is_finite() || slack_units < 0.0 {
                return Err(SettingsError(format!("bonds: {}: slackUnits must be a finite non-negative number", entry.spell)));
            }
            spells.push(Named {
                short_id,
                file,
                spell: BondSpell {
                    duration_ms: entry.duration_ms.unwrap_or(BOND_DURATION_MS),
                    window_ms: entry.window_ms.unwrap_or(BOND_WINDOW_MS),
                    slack_units,
                },
            });
        }
        Ok(Self { spells, bonds: Vec::new() })
    }

    /// The bond spell `short_id` in `file` (compared without case), if the
    /// server names one.
    pub fn named(&self, short_id: u32, file: &str) -> Option<&BondSpell> {
        self.spells.iter().find(|n| n.short_id == short_id && n.file.eq_ignore_ascii_case(file)).map(|n| &n.spell)
    }

    /// `caster`'s cast of `spell` landed on `target` at `now_ms`: a bond
    /// stands for the spell's duration, replacing the caster's older one.
    pub fn record(&mut self, caster: u32, target: u32, spell: &BondSpell, now_ms: u64) {
        self.bonds.retain(|b| b.caster != caster && now_ms <= b.until_ms);
        if self.bonds.len() >= MAX_BONDS {
            self.bonds.remove(0);
        }
        self.bonds.push(Bond {
            caster,
            target,
            until_ms: now_ms.saturating_add(spell.duration_ms),
            window_ms: spell.window_ms,
            slack_units: spell.slack_units,
        });
    }

    /// `actor` acted at `now_ms` (its concealment ended): the standing bond
    /// it is in, as caster or target, ends and is returned; none when it is
    /// in none, or its bond ran out.
    pub fn break_of(&mut self, actor: u32, now_ms: u64) -> Option<Bond> {
        self.bonds.retain(|b| now_ms <= b.until_ms);
        let at = self.bonds.iter().position(|b| b.caster == actor || b.target == actor)?;
        Some(self.bonds.remove(at))
    }

    /// The actor is gone: its bonds with it.
    pub fn forget(&mut self, actor: u32) {
        self.bonds.retain(|b| b.caster != actor && b.target != actor);
    }

    /// How many bonds stand at `now_ms`.
    pub fn standing(&mut self, now_ms: u64) -> usize {
        self.bonds.retain(|b| now_ms <= b.until_ms);
        self.bonds.len()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SHADOWBOND: &str = r#"[{"spell": "31C4D:Apocalypse - Magic of Skyrim.esp"}]"#;
    const DEFAULTS: BondSpell = BondSpell { duration_ms: BOND_DURATION_MS, window_ms: BOND_WINDOW_MS, slack_units: BOND_SLACK_UNITS };

    #[test]
    fn an_entry_names_a_spell_with_the_defaults_and_its_own_numbers() {
        let b = Bonds::from_json(SHADOWBOND).unwrap_or_default();
        assert_eq!(
            b.named(0x31c4d, "apocalypse - magic of skyrim.esp"),
            Some(&BondSpell { duration_ms: BOND_DURATION_MS, window_ms: BOND_WINDOW_MS, slack_units: BOND_SLACK_UNITS })
        );
        assert!(b.named(0x31c4d, "Skyrim.esm").is_none());
        let b = Bonds::from_json(r#"[{"spell": "31C4D:A.esp", "durationMs": 5000, "windowMs": 800, "slackUnits": 100}]"#).unwrap_or_default();
        assert_eq!(b.named(0x31c4d, "A.esp"), Some(&BondSpell { duration_ms: 5_000, window_ms: 800, slack_units: 100.0 }));
        assert!(Bonds::from_json(r#"[{"spell": "31C4D:A.esp", "slackUnits": -1}]"#).is_err());
        assert!(Bonds::from_json(r#"[{"spell": "31C4D:A.esp", "reach": 3}]"#).is_err());
        assert!(Bonds::from_json("[]").unwrap_or_default().named(1, "A.esp").is_none());
    }

    #[test]
    fn a_bond_breaks_once_at_either_actors_act_and_runs_out_by_itself() {
        let mut b = Bonds::from_json(SHADOWBOND).unwrap_or_default();
        let s = DEFAULTS;
        b.record(1, 2, &s, 1_000);
        // a bystander's act breaks nothing
        assert_eq!(b.break_of(3, 2_000), None);
        // the target's act breaks it, once
        assert_eq!(b.break_of(2, 2_000).map(|x| (x.caster, x.target, x.window_ms)), Some((1, 2, BOND_WINDOW_MS)));
        assert_eq!(b.break_of(1, 2_100), None);
        // the caster's act breaks a fresh one
        b.record(1, 2, &s, 3_000);
        assert!(b.break_of(1, 3_500).is_some());
        // a bond left alone runs out at the duration
        b.record(1, 2, &s, 10_000);
        assert_eq!(b.standing(10_000 + BOND_DURATION_MS), 1);
        assert_eq!(b.break_of(2, 10_001 + BOND_DURATION_MS), None);
    }

    #[test]
    fn a_casters_new_bond_replaces_its_old_one() {
        let mut b = Bonds::from_json(SHADOWBOND).unwrap_or_default();
        let s = DEFAULTS;
        b.record(1, 2, &s, 0);
        b.record(1, 3, &s, 100);
        assert_eq!(b.standing(100), 1);
        assert_eq!(b.break_of(2, 200), None);
        assert_eq!(b.break_of(3, 200).map(|x| x.target), Some(3));
        b.record(1, 2, &s, 300);
        b.forget(2);
        assert_eq!(b.standing(300), 0);
    }
}
