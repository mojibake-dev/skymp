//! Scripted effects the server runs itself (thuum docs/verbs/scripted-effects.md,
//! ADR-029's allowlist, train-31). An effect of archetype Script does its work
//! in a Papyrus script that the caster's game runs; the records alone let the
//! server apply none of it. The ones that change another player's state are
//! named in server-settings.json's `scriptedEffects`, a list of
//! `{"spell": "<hex form id>:<plugin>", "kill": "fingerOfDeath", "percentage": 125}`,
//! the spell as the change forms write a FormDesc so a change of load order
//! keeps it, and the server runs the script's arithmetic from the claimed hit
//! once the effect's own conditions held (ADR-039), so the victim dies in
//! every game (playtest twenty-eight, Eli: "if it kills them by effect of the
//! spell, yes kill them. why on earth would i want them dead in only one
//! game").
//!
//! The first script, Apocalypse's Finger of Death
//! (WB_FingerOfDeath_Script.OnEffectStart, disassembled from the mod's
//! archive; docs/research/train-31-reads.md section 5): the target's Health is
//! the blast and the caster takes `percentage` percent of it. When that would
//! leave the caster alive (the self damage under the caster's Health less one)
//! the target loses its whole Health and dies by the caster's hand; else the
//! caster is left at one point and the target loses that much scaled back by
//! the percentage (0.8 of it at the mod's 125), and nobody dies. `percentage`
//! is the script's global as the mod ships it (WB_Restoration_FingerOfDeath_
//! Global_Percentage, 125) unless the entry says otherwise. HYPOTHESIS, in the
//! verb doc: the script's WB_IsResist default, which would scale the blast by
//! the target's magic resistance; the arithmetic here takes the Health as it
//! stands.

use crate::clock::SettingsError;
use crate::teleports::{form_spec, FormSpec};
use serde::Deserialize;

/// The script's percentage global as Apocalypse ships it.
pub const FINGER_OF_DEATH_PERCENTAGE: f32 = 125.0;

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Entry {
    spell: String,
    kill: String,
    percentage: Option<f32>,
}

/// The scripts the server runs.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KillScript {
    /// Finger of Death's blast, `finger_of_death`.
    FingerOfDeath,
}

#[derive(Debug, Clone, PartialEq)]
struct Kill {
    spell: FormSpec,
    script: KillScript,
    percentage: f32,
}

/// The `scriptedEffects` list as parsed.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct ScriptedEffects {
    kills: Vec<Kill>,
}

impl ScriptedEffects {
    /// The list from server-settings.json as JSON text; `[]` or an empty
    /// string names nothing. The error names the bad entry.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let trimmed = text.trim();
        let entries: Vec<Entry> = if trimmed.is_empty() || trimmed == "null" {
            Vec::new()
        } else {
            serde_json::from_str(trimmed).map_err(|e| SettingsError(format!("scriptedEffects: {e}")))?
        };
        let mut kills = Vec::with_capacity(entries.len());
        for entry in &entries {
            let spell = form_spec(&entry.spell)?;
            let script = match entry.kill.as_str() {
                "fingerOfDeath" => KillScript::FingerOfDeath,
                other => {
                    return Err(SettingsError(format!(
                        "scriptedEffects: {} names a kill script the server does not run: {other:?}",
                        entry.spell
                    )))
                }
            };
            let percentage = entry.percentage.unwrap_or(FINGER_OF_DEATH_PERCENTAGE);
            if !percentage.is_finite() || percentage <= 0.0 {
                return Err(SettingsError(format!(
                    "scriptedEffects: {} percentage {percentage} is not a share of the target's Health",
                    entry.spell
                )));
            }
            kills.push(Kill { spell, script, percentage });
        }
        Ok(Self { kills })
    }

    fn entry(&self, short_id: u32, file: &str) -> Option<&Kill> {
        self.kills.iter().find(|k| k.spell.short_id == short_id && k.spell.file == file)
    }

    /// Whether the spell `short_id` in `file` is a scripted kill.
    pub fn is_kill_spell(&self, short_id: u32, file: &str) -> bool {
        self.entry(short_id, file).is_some()
    }

    /// The script the spell's entry names, when the server names it a kill.
    pub fn kill_script(&self, short_id: u32, file: &str) -> Option<KillScript> {
        self.entry(short_id, file).map(|k| k.script)
    }

    /// The percentage the spell's entry carries (the mod's own when the
    /// server names it no kill).
    pub fn percentage(&self, short_id: u32, file: &str) -> f32 {
        self.entry(short_id, file).map_or(FINGER_OF_DEATH_PERCENTAGE, |k| k.percentage)
    }
}

/// What Finger of Death does to its two actors.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct DeathBlast {
    /// Health the target loses.
    pub target_damage: f32,
    /// Health the caster loses.
    pub caster_damage: f32,
    /// The target dies by the caster's hand.
    pub kill: bool,
}

/// Finger of Death's arithmetic over the target's and the caster's current
/// Health and the script's percentage. Inputs that are not numbers, or a
/// percentage that is no share, do nothing.
pub fn finger_of_death(target_health: f32, caster_health: f32, percentage: f32) -> DeathBlast {
    let nothing = DeathBlast { target_damage: 0.0, caster_damage: 0.0, kill: false };
    if !(target_health.is_finite() && caster_health.is_finite() && percentage.is_finite()) || percentage <= 0.0 {
        return nothing;
    }
    let target_health = target_health.max(0.0);
    let caster_health = caster_health.max(0.0);
    // SelfDamage = TargetHealth * Percentage / 100
    let self_damage = target_health * percentage / 100.0;
    if self_damage < caster_health - 1.0 {
        // the kill branch: DamageActorValue(target, TargetHealth),
        // DamageActorValue(caster, SelfDamage), target.Kill(caster)
        return DeathBlast { target_damage: target_health, caster_damage: self_damage, kill: true };
    }
    // the fail branch: SelfDamage = casterHealth - 1; the target takes
    // SelfDamage * 100 / Percentage, the caster SelfDamage; no kill
    let self_damage = (caster_health - 1.0).max(0.0);
    DeathBlast { target_damage: self_damage * 100.0 / percentage, caster_damage: self_damage, kill: false }
}

#[cfg(test)]
mod tests {
    use super::*;

    const SETTINGS: &str = r#"[{"spell": "35452:Apocalypse - Magic of Skyrim.esp", "kill": "fingerOfDeath"}]"#;

    fn near(a: f32, b: f32) -> bool {
        (a - b).abs() < 1e-3
    }

    #[test]
    fn the_settings_name_the_spell_and_its_script() {
        let s = ScriptedEffects::from_json(SETTINGS).unwrap_or_default();
        assert!(s.is_kill_spell(0x35452, "Apocalypse - Magic of Skyrim.esp"));
        assert_eq!(s.kill_script(0x35452, "Apocalypse - Magic of Skyrim.esp"), Some(KillScript::FingerOfDeath));
        assert!(near(s.percentage(0x35452, "Apocalypse - Magic of Skyrim.esp"), 125.0));
        assert!(!s.is_kill_spell(0x35452, "Skyrim.esm"));
        assert_eq!(s.kill_script(0x12fd0, "Skyrim.esm"), None);
        let tuned = ScriptedEffects::from_json(
            r#"[{"spell": "35452:Apocalypse - Magic of Skyrim.esp", "kill": "fingerOfDeath", "percentage": 50}]"#,
        )
        .unwrap_or_default();
        assert!(near(tuned.percentage(0x35452, "Apocalypse - Magic of Skyrim.esp"), 50.0));
        assert!(ScriptedEffects::from_json("").is_ok());
        assert!(ScriptedEffects::from_json("[]").is_ok());
        assert!(ScriptedEffects::from_json("null").is_ok());
        assert!(ScriptedEffects::from_json(r#"[{"spell": "nonsense", "kill": "fingerOfDeath"}]"#).is_err());
        assert!(ScriptedEffects::from_json(r#"[{"spell": "35452:A.esp", "kill": "ashPile"}]"#).is_err());
        assert!(ScriptedEffects::from_json(r#"[{"spell": "35452:A.esp", "kill": "fingerOfDeath", "percentage": 0}]"#).is_err());
        assert!(ScriptedEffects::from_json(r#"[{"spell": "35452:A.esp", "kill": "fingerOfDeath", "ash": true}]"#).is_err());
    }

    #[test]
    fn a_blast_the_caster_survives_kills_the_target() {
        // the script's numbers: a target at 50 Health costs the caster 62.5,
        // under the caster's 99 (100 less one): the target dies
        let b = finger_of_death(50.0, 100.0, 125.0);
        assert!(b.kill);
        assert!(near(b.target_damage, 50.0));
        assert!(near(b.caster_damage, 62.5));
        // the bound is strict: a self damage equal to the caster's Health
        // less one is the fail branch
        let edge = finger_of_death(79.2, 100.0, 125.0);
        assert!(!edge.kill);
        let under = finger_of_death(79.0, 100.0, 125.0);
        assert!(under.kill);
    }

    #[test]
    fn a_blast_that_would_kill_the_caster_leaves_it_at_one_point() {
        // a target at 100 would cost the caster 125: the caster drops to one
        // point (99 damage) and the target takes 99 * 100 / 125
        let b = finger_of_death(100.0, 100.0, 125.0);
        assert!(!b.kill);
        assert!(near(b.caster_damage, 99.0));
        assert!(near(b.target_damage, 79.2));
        // a caster at one point or less has nothing to give: no damage at all
        let spent = finger_of_death(100.0, 1.0, 125.0);
        assert!(!spent.kill);
        assert!(near(spent.caster_damage, 0.0));
        assert!(near(spent.target_damage, 0.0));
    }

    #[test]
    fn nonsense_does_nothing() {
        for b in [
            finger_of_death(f32::NAN, 100.0, 125.0),
            finger_of_death(50.0, f32::INFINITY, 125.0),
            finger_of_death(50.0, 100.0, 0.0),
            finger_of_death(50.0, 100.0, -125.0),
        ] {
            assert!(!b.kill);
            assert!(near(b.target_damage, 0.0));
            assert!(near(b.caster_damage, 0.0));
        }
        // a negative health reads as none
        let b = finger_of_death(-5.0, 100.0, 125.0);
        assert!(b.kill);
        assert!(near(b.target_damage, 0.0));
    }
}
