//! Cast teleports (thuum docs/verbs/cast-teleport.md). A spell that moves its
//! caster to where its cast lands does so in the caster's own game: the engine
//! places the spell's marker where the target-location cast lands and a mod's
//! script moves the player onto it (Lost Grimoire's Teleportation, thuum
//! docs/MODS.md). The server takes that jump when it lands within the spell's
//! reach of the cast it recorded (movement's cast jump). The records cannot
//! say a spell teleports (its effect, Spawn Scripted Ref, also places runes
//! and anchors), so server-settings.json names them: `castTeleports`, a list
//! of `{"spell": "<hex form id>:<plugin>", "reach": <units>}`, the spell as the
//! server's change forms write a FormDesc, so a change of load order keeps it.

use crate::clock::SettingsError;
use serde::Deserialize;

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Entry {
    spell: String,
    reach: f32,
}

/// One spell the server names as a teleport.
#[derive(Debug, Clone, PartialEq)]
struct Named {
    /// The form id within its plugin (a FormDesc's short id).
    short_id: u32,
    /// The plugin, as the load order names it, compared without case.
    file: String,
    /// How far from the caster its cast may land, units.
    reach: f32,
}

/// The spells the server names as teleports, each with its reach.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct CastTeleports {
    spells: Vec<Named>,
}

impl CastTeleports {
    /// The list from the `castTeleports` block's JSON text (`[]` for none).
    /// The error names a bad entry: a spell that is not "<hex id>:<plugin>",
    /// a reach that is not a finite positive number, an unknown key.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let entries: Vec<Entry> = serde_json::from_str(text).map_err(|e| SettingsError(format!("castTeleports: {e}")))?;
        let mut spells = Vec::with_capacity(entries.len());
        for e in entries {
            let Some((id, file)) = e.spell.split_once(':') else {
                return Err(SettingsError(format!("castTeleports: {:?} is not <hex form id>:<plugin>", e.spell)));
            };
            let digits = id.strip_prefix("0x").unwrap_or(id);
            let Ok(short_id) = u32::from_str_radix(digits, 16) else {
                return Err(SettingsError(format!("castTeleports: {:?} has no hex form id", e.spell)));
            };
            if file.is_empty() {
                return Err(SettingsError(format!("castTeleports: {:?} names no plugin", e.spell)));
            }
            if !(e.reach.is_finite() && e.reach > 0.0) {
                return Err(SettingsError(format!("castTeleports: {:?} has a reach that is not a positive number", e.spell)));
            }
            spells.push(Named { short_id, file: file.to_owned(), reach: e.reach });
        }
        Ok(Self { spells })
    }

    /// The reach of the spell `short_id` in `file` (a FormDesc's parts), if
    /// the server names it as a teleport.
    #[must_use]
    pub fn reach_of(&self, short_id: u32, file: &str) -> Option<f32> {
        self.spells
            .iter()
            .find(|s| s.short_id == short_id && s.file.eq_ignore_ascii_case(file))
            .map(|s| s.reach)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn named_spells_and_their_reach() {
        let t = CastTeleports::from_json(r#"[{"spell": "a86df:LostGrimoire.esp", "reach": 3000}]"#)
            .unwrap_or_default();
        assert_eq!(t.reach_of(0xa86df, "LostGrimoire.esp"), Some(3000.0));
        assert_eq!(t.reach_of(0xa86df, "lostgrimoire.esp"), Some(3000.0));
        assert_eq!(t.reach_of(0xa86df, "Skyrim.esm"), None);
        assert_eq!(t.reach_of(0x12fd0, "LostGrimoire.esp"), None);
        let hex = CastTeleports::from_json(r#"[{"spell": "0x0A86DF:LostGrimoire.esp", "reach": 1}]"#).unwrap_or_default();
        assert_eq!(hex.reach_of(0xa86df, "LostGrimoire.esp"), Some(1.0));
    }

    #[test]
    fn none_named_by_default_and_bad_entries_refused() {
        assert_eq!(CastTeleports::from_json("[]").unwrap_or_default().reach_of(0xa86df, "LostGrimoire.esp"), None);
        for bad in [
            r#"[{"spell": "a86df", "reach": 3000}]"#,
            r#"[{"spell": "zz:LostGrimoire.esp", "reach": 3000}]"#,
            r#"[{"spell": "a86df:", "reach": 3000}]"#,
            r#"[{"spell": "a86df:LostGrimoire.esp", "reach": 0}]"#,
            r#"[{"spell": "a86df:LostGrimoire.esp", "reach": -5}]"#,
            r#"[{"spell": "a86df:LostGrimoire.esp", "reach": 3000, "speed": 1}]"#,
            r#"[{"spell": "a86df:LostGrimoire.esp"}]"#,
            r#"{"spell": "a86df:LostGrimoire.esp", "reach": 3000}"#,
            "",
        ] {
            assert!(CastTeleports::from_json(bad).is_err(), "{bad}");
        }
    }
}
