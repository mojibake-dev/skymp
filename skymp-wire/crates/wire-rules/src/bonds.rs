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
//!
//! Train-28: the server owns the two concealments too. The script runs in
//! the caster's game alone and casts its proc (the entry's `proc`, a Self
//! spell of archetype Invisibility) through Papyrus, which no client reports
//! (playtest twenty-four: the proc reached no log from either game, and the
//! relayed fire-and-forget cast applies nothing to the target's own player).
//! So a bond carries its proc, and the server records it on both at the
//! claim and casts it where no script does; a bond that ran out is returned
//! once by `expired`, so the server ends what it imposed.
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
    /// The concealment the mod's script casts on both, "<hex form id>:<plugin>".
    proc: Option<String>,
    duration_ms: Option<u64>,
    window_ms: Option<u64>,
    slack_units: Option<f32>,
}

/// One spell the server names as a bond.
#[derive(Debug, Clone, PartialEq)]
pub struct BondSpell {
    /// How long the bond stands after the hit, ms.
    pub duration_ms: u64,
    /// How long after the break each landing is permitted, ms.
    pub window_ms: u64,
    /// How far from the other's position a landing may fall, units.
    pub slack_units: f32,
    /// The Self spell the mod's script casts on both in the caster's game
    /// (archetype Invisibility): the server records it on both for the
    /// bond's time and casts it in every other game, if the entry names it.
    pub proc: Option<FormDesc>,
}

#[derive(Debug, Clone, PartialEq)]
struct Named {
    short_id: u32,
    file: String,
    spell: BondSpell,
}

/// A bond standing between two actors.
#[derive(Debug, Clone, PartialEq)]
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
    /// How long it stands from its claim, ms: the concealment records' time.
    pub duration_ms: u64,
    /// The concealment the server records and casts for it, if named.
    pub proc: Option<FormDesc>,
}

/// The spells the server names as bonds, and the bonds standing.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Bonds {
    spells: Vec<Named>,
    bonds: Vec<Bond>,
    /// Bonds that ran out, kept until `expired` returns them once.
    lapsed: Vec<Bond>,
}

impl Bonds {
    /// The `bonds` block as JSON text, a list of entries (`[]` for none):
    /// each a spell written "<hex form id>:<plugin>" and, optionally, its
    /// `proc` (the same form), `durationMs`, `windowMs` and `slackUnits`;
    /// the error names the bad entry.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let entries: Vec<Entry> =
            serde_json::from_str(text).map_err(|why| SettingsError(format!("bonds: {why}")))?;
        let mut spells = Vec::with_capacity(entries.len());
        for entry in entries {
            let FormDesc { short_id, file } = FormDesc::parse(&entry.spell)
                .map_err(|why| SettingsError(format!("bonds: {why}")))?;
            let proc = match &entry.proc {
                Some(text) => Some(FormDesc::parse(text).map_err(|why| {
                    SettingsError(format!("bonds: {}: proc: {why}", entry.spell))
                })?),
                None => None,
            };
            let slack_units = entry.slack_units.unwrap_or(BOND_SLACK_UNITS);
            if !slack_units.is_finite() || slack_units < 0.0 {
                return Err(SettingsError(format!(
                    "bonds: {}: slackUnits must be a finite non-negative number",
                    entry.spell
                )));
            }
            spells.push(Named {
                short_id,
                file,
                spell: BondSpell {
                    duration_ms: entry.duration_ms.unwrap_or(BOND_DURATION_MS),
                    window_ms: entry.window_ms.unwrap_or(BOND_WINDOW_MS),
                    slack_units,
                    proc,
                },
            });
        }
        Ok(Self {
            spells,
            bonds: Vec::new(),
            lapsed: Vec::new(),
        })
    }

    /// The bond spell `short_id` in `file` (compared without case), if the
    /// server names one.
    pub fn named(&self, short_id: u32, file: &str) -> Option<&BondSpell> {
        self.spells
            .iter()
            .find(|n| n.short_id == short_id && n.file.eq_ignore_ascii_case(file))
            .map(|n| &n.spell)
    }

    /// `caster`'s cast of `spell` landed on `target` at `now_ms`: a bond
    /// stands for the spell's duration, replacing the caster's older one
    /// (`supersede` first, to end what the server imposed for that one);
    /// the bond as recorded.
    pub fn record(&mut self, caster: u32, target: u32, spell: &BondSpell, now_ms: u64) -> Bond {
        self.prune(now_ms);
        self.bonds.retain(|b| b.caster != caster);
        if self.bonds.len() >= MAX_BONDS {
            self.bonds.remove(0);
        }
        let bond = Bond {
            caster,
            target,
            until_ms: now_ms.saturating_add(spell.duration_ms),
            window_ms: spell.window_ms,
            slack_units: spell.slack_units,
            duration_ms: spell.duration_ms,
            proc: spell.proc.clone(),
        };
        self.bonds.push(bond.clone());
        bond
    }

    /// The standing bond `caster` cast, removed and returned: a recast
    /// replaces it, and the server ends the concealment it imposed for it
    /// first; none when the caster has none standing.
    pub fn supersede(&mut self, caster: u32, now_ms: u64) -> Option<Bond> {
        self.prune(now_ms);
        let at = self.bonds.iter().position(|b| b.caster == caster)?;
        Some(self.bonds.remove(at))
    }

    /// `actor` acted at `now_ms` (its concealment ended): the standing bond
    /// it is in, as caster or target, ends and is returned; none when it is
    /// in none, or its bond ran out.
    pub fn break_of(&mut self, actor: u32, now_ms: u64) -> Option<Bond> {
        self.prune(now_ms);
        let at = self
            .bonds
            .iter()
            .position(|b| b.caster == actor || b.target == actor)?;
        Some(self.bonds.remove(at))
    }

    /// The bonds that ran out by `now_ms`, each returned once (no other
    /// method drops a lapsed bond: it waits here), for the server to end
    /// the concealments it imposed for them.
    pub fn expired(&mut self, now_ms: u64) -> Vec<Bond> {
        self.prune(now_ms);
        std::mem::take(&mut self.lapsed)
    }

    /// The actor is gone: its standing bonds with it (a lapsed one still
    /// comes back from `expired`, for the other actor's sake).
    pub fn forget(&mut self, actor: u32) {
        self.bonds
            .retain(|b| b.caster != actor && b.target != actor);
    }

    /// How many bonds stand at `now_ms`.
    pub fn standing(&mut self, now_ms: u64) -> usize {
        self.prune(now_ms);
        self.bonds.len()
    }

    /// Moves the bonds that ran out by `now_ms` to the lapsed list, which is
    /// bounded should nobody drain it.
    fn prune(&mut self, now_ms: u64) {
        let (lapsed, standing): (Vec<Bond>, Vec<Bond>) =
            self.bonds.drain(..).partition(|b| now_ms > b.until_ms);
        self.bonds = standing;
        self.lapsed.extend(lapsed);
        let excess = self.lapsed.len().saturating_sub(MAX_BONDS);
        if excess > 0 {
            self.lapsed.drain(..excess);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SHADOWBOND: &str = r#"[{"spell": "31C4D:Apocalypse - Magic of Skyrim.esp"}]"#;
    const DEFAULTS: BondSpell = BondSpell {
        duration_ms: BOND_DURATION_MS,
        window_ms: BOND_WINDOW_MS,
        slack_units: BOND_SLACK_UNITS,
        proc: None,
    };

    #[test]
    fn an_entry_names_a_spell_with_the_defaults_and_its_own_numbers() {
        let b = Bonds::from_json(SHADOWBOND).unwrap_or_default();
        assert_eq!(
            b.named(0x31c4d, "apocalypse - magic of skyrim.esp"),
            Some(&BondSpell {
                duration_ms: BOND_DURATION_MS,
                window_ms: BOND_WINDOW_MS,
                slack_units: BOND_SLACK_UNITS,
                proc: None
            })
        );
        assert!(b.named(0x31c4d, "Skyrim.esm").is_none());
        let b = Bonds::from_json(
            r#"[{"spell": "31C4D:A.esp", "durationMs": 5000, "windowMs": 800, "slackUnits": 100}]"#,
        )
        .unwrap_or_default();
        assert_eq!(
            b.named(0x31c4d, "A.esp"),
            Some(&BondSpell {
                duration_ms: 5_000,
                window_ms: 800,
                slack_units: 100.0,
                proc: None
            })
        );
        assert!(Bonds::from_json(r#"[{"spell": "31C4D:A.esp", "slackUnits": -1}]"#).is_err());
        assert!(Bonds::from_json(r#"[{"spell": "31C4D:A.esp", "reach": 3}]"#).is_err());
        assert!(Bonds::from_json("[]")
            .unwrap_or_default()
            .named(1, "A.esp")
            .is_none());
    }

    #[test]
    fn an_entry_may_name_the_concealment_the_server_imposes() {
        let b = Bonds::from_json(r#"[{"spell": "31C4D:A.esp", "proc": "55B32:A.esp"}]"#)
            .unwrap_or_default();
        assert_eq!(
            b.named(0x31c4d, "A.esp").and_then(|s| s.proc.clone()),
            Some(FormDesc {
                short_id: 0x55b32,
                file: "A.esp".to_string()
            })
        );
        // a bond without one is the swap alone, as before
        let b = Bonds::from_json(SHADOWBOND).unwrap_or_default();
        assert_eq!(
            b.named(0x31c4d, "Apocalypse - Magic of Skyrim.esp")
                .and_then(|s| s.proc.clone()),
            None
        );
        assert!(Bonds::from_json(r#"[{"spell": "31C4D:A.esp", "proc": "nonsense"}]"#).is_err());
    }

    #[test]
    fn a_bond_breaks_once_at_either_actors_act_and_runs_out_by_itself() {
        let mut b = Bonds::from_json(SHADOWBOND).unwrap_or_default();
        let s = DEFAULTS;
        b.record(1, 2, &s, 1_000);
        // a bystander's act breaks nothing
        assert_eq!(b.break_of(3, 2_000), None);
        // the target's act breaks it, once
        assert_eq!(
            b.break_of(2, 2_000)
                .map(|x| (x.caster, x.target, x.window_ms)),
            Some((1, 2, BOND_WINDOW_MS))
        );
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
    fn a_bond_that_ran_out_comes_back_once_and_a_broken_one_never() {
        let mut b = Bonds::from_json(SHADOWBOND).unwrap_or_default();
        let s = DEFAULTS;
        let recorded = b.record(1, 2, &s, 1_000);
        assert_eq!(
            (
                recorded.caster,
                recorded.target,
                recorded.duration_ms,
                recorded.proc
            ),
            (1, 2, BOND_DURATION_MS, None)
        );
        // not yet
        assert!(b.expired(1_000 + BOND_DURATION_MS).is_empty());
        // another method noticing the lapse keeps it for `expired`
        assert_eq!(b.break_of(2, 1_001 + BOND_DURATION_MS), None);
        let lapsed = b.expired(1_001 + BOND_DURATION_MS);
        assert_eq!(
            lapsed
                .iter()
                .map(|x| (x.caster, x.target))
                .collect::<Vec<_>>(),
            vec![(1, 2)]
        );
        assert!(b.expired(5_000 + BOND_DURATION_MS).is_empty());
        // a broken bond never runs out
        b.record(1, 2, &s, 10_000);
        assert!(b.break_of(1, 10_500).is_some());
        assert!(b.expired(10_001 + BOND_DURATION_MS).is_empty());
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

    #[test]
    fn a_recast_supersedes_the_casters_standing_bond_alone() {
        let mut b = Bonds::from_json(SHADOWBOND).unwrap_or_default();
        let s = DEFAULTS;
        b.record(1, 2, &s, 0);
        // the target casting one of its own supersedes nothing: it is no caster yet
        assert_eq!(b.supersede(2, 100), None);
        assert_eq!(
            b.supersede(1, 100).map(|x| (x.caster, x.target)),
            Some((1, 2))
        );
        assert_eq!(b.supersede(1, 100), None);
        assert_eq!(b.standing(100), 0);
        // a superseded bond did not run out
        assert!(b.expired(100 + BOND_DURATION_MS).is_empty());
    }
}
