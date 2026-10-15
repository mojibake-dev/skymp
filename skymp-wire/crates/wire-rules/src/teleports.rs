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
//!
//! A spell that returns its caster to where it cast is the same jump with a
//! reach of 0 (Apocalypse's Ghostwalk, thuum docs/verbs/ghostwalk.md: 10 s
//! invisible, then the mod's script moves the caster back), so it takes the
//! two optional fields: `waitMs`, how long after the cast the jump is
//! permitted (movement::CAST_JUMP_WAIT_MS when absent), and `relay`, whether
//! the cast is still relayed to the neighbours (false when absent: a relayed
//! teleport would place its marker in an observer's game and move the
//! observer; Ghostwalk's relayed Self cast only makes the figure invisible).

use crate::clock::SettingsError;
use crate::forms::FormDesc;
use crate::movement::CAST_JUMP_WAIT_MS;
use serde::Deserialize;

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Entry {
    spell: String,
    reach: f32,
    wait_ms: Option<u64>,
    relay: Option<bool>,
}

/// What the server does with a named spell's cast.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CastTeleport {
    /// How far from the caster its cast may land, units (0: back where it
    /// cast, within movement::CAST_JUMP_SLACK).
    pub reach: f32,
    /// How long after the cast the jump is permitted, ms.
    pub wait_ms: u64,
    /// Whether the cast is relayed to the neighbours.
    pub relay: bool,
}

/// One spell the server names as a teleport.
#[derive(Debug, Clone, PartialEq)]
struct Named {
    /// The form id within its plugin (a FormDesc's short id).
    short_id: u32,
    /// The plugin, as the load order names it, compared without case.
    file: String,
    teleport: CastTeleport,
}

/// The spells the server names as teleports, each with its reach.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct CastTeleports {
    spells: Vec<Named>,
}

impl CastTeleports {
    /// The list from the `castTeleports` block's JSON text (`[]` for none).
    /// The error names a bad entry: a spell that is not "<hex id>:<plugin>",
    /// a reach that is not a finite number of 0 or more, a wait of 0, an
    /// unknown key.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let entries: Vec<Entry> = serde_json::from_str(text).map_err(|e| SettingsError(format!("castTeleports: {e}")))?;
        let mut spells = Vec::with_capacity(entries.len());
        for e in entries {
            let FormDesc { short_id, file } = FormDesc::parse(&e.spell).map_err(|why| SettingsError(format!("castTeleports: {why}")))?;
            if !(e.reach.is_finite() && e.reach >= 0.0) {
                return Err(SettingsError(format!("castTeleports: {:?} has a reach that is not a number of 0 or more", e.spell)));
            }
            let wait_ms = e.wait_ms.unwrap_or(CAST_JUMP_WAIT_MS);
            if wait_ms == 0 {
                return Err(SettingsError(format!("castTeleports: {:?} has a wait of 0", e.spell)));
            }
            let teleport = CastTeleport { reach: e.reach, wait_ms, relay: e.relay.unwrap_or(false) };
            spells.push(Named { short_id, file, teleport });
        }
        Ok(Self { spells })
    }

    /// The spell `short_id` in `file` (a FormDesc's parts), if the server
    /// names it as a teleport.
    #[must_use]
    pub fn named(&self, short_id: u32, file: &str) -> Option<CastTeleport> {
        self.spells
            .iter()
            .find(|s| s.short_id == short_id && s.file.eq_ignore_ascii_case(file))
            .map(|s| s.teleport)
    }

    /// The reach of the spell `short_id` in `file`, if the server names it
    /// as a teleport.
    #[must_use]
    pub fn reach_of(&self, short_id: u32, file: &str) -> Option<f32> {
        self.named(short_id, file).map(|t| t.reach)
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
    fn a_teleport_waits_the_default_and_is_not_relayed_unless_named() {
        let t = CastTeleports::from_json(
            r#"[{"spell": "a86df:LostGrimoire.esp", "reach": 8000},
                {"spell": "aae8:Apocalypse - Magic of Skyrim.esp", "reach": 0, "waitMs": 15400, "relay": true}]"#,
        )
        .unwrap_or_default();
        assert_eq!(
            t.named(0xa86df, "LostGrimoire.esp"),
            Some(CastTeleport { reach: 8000.0, wait_ms: CAST_JUMP_WAIT_MS, relay: false })
        );
        assert_eq!(
            t.named(0xaae8, "apocalypse - magic of skyrim.esp"),
            Some(CastTeleport { reach: 0.0, wait_ms: 15_400, relay: true })
        );
        assert_eq!(t.named(0xaae8, "LostGrimoire.esp"), None);
    }

    #[test]
    fn none_named_by_default_and_bad_entries_refused() {
        assert_eq!(CastTeleports::from_json("[]").unwrap_or_default().reach_of(0xa86df, "LostGrimoire.esp"), None);
        for bad in [
            r#"[{"spell": "a86df", "reach": 3000}]"#,
            r#"[{"spell": "zz:LostGrimoire.esp", "reach": 3000}]"#,
            r#"[{"spell": "a86df:", "reach": 3000}]"#,
            r#"[{"spell": "a86df:LostGrimoire.esp", "reach": -5}]"#,
            r#"[{"spell": "a86df:LostGrimoire.esp", "reach": 3000, "waitMs": 0}]"#,
            r#"[{"spell": "a86df:LostGrimoire.esp", "reach": 3000, "waitMs": -1}]"#,
            r#"[{"spell": "a86df:LostGrimoire.esp", "reach": 3000, "relay": 1}]"#,
            r#"[{"spell": "a86df:LostGrimoire.esp", "reach": 3000, "speed": 1}]"#,
            r#"[{"spell": "a86df:LostGrimoire.esp"}]"#,
            r#"{"spell": "a86df:LostGrimoire.esp", "reach": 3000}"#,
            "",
        ] {
            assert!(CastTeleports::from_json(bad).is_err(), "{bad}");
        }
    }
}
