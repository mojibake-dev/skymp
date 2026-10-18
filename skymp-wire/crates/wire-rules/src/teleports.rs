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
//!
//! The mod's script plays the teleport's effects in the caster's game alone,
//! so an entry may name its `look` in the other players' games (thuum
//! docs/verbs/cast-teleport.md, the look on other screens): `vanish`, a
//! VisualEffect played on the caster's figure from the cast until the
//! landing; `appear`, one played there at the landing for `appearSeconds`
//! (APPEAR_SECONDS when absent); `sound`, a Sound played on the figure at the
//! landing. Each is a form written as `spell` is, and a look names at least
//! one of the three.

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
    look: Option<LookEntry>,
}

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct LookEntry {
    vanish: Option<String>,
    appear: Option<String>,
    appear_seconds: Option<f32>,
    sound: Option<String>,
}

/// How long a look's appear plays when its entry names no `appearSeconds`,
/// seconds: Lost Grimoire's marker script plays its appear for 1.75 s (thuum
/// docs/verbs/cast-teleport.md).
pub const APPEAR_SECONDS: f32 = 1.75;
/// The longest appear a look may name, seconds.
pub const MAX_APPEAR_SECONDS: f32 = 10.0;
/// How far one movement report must step from the last, over the ground, for
/// a teleport's look to take the move as its landing when the movement bounds
/// take it on the ground budget alone, which most landings are (movement's
/// BURST covers them: the cast jump's permit then never sees them, thuum
/// docs/verbs/cast-teleport.md, playtest fourteen): about three reports' run
/// at movement::MAX_SPEED, reports going every 130 ms. HYPOTHESIS until a
/// scenario reads the steps of the landings and of running.
pub const LOOK_LANDING_STEP: f32 = 256.0;

/// A form a setting names: its id within its plugin (a FormDesc's short id)
/// and the plugin, as the setting writes it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FormSpec {
    /// The form id within its plugin.
    pub short_id: u32,
    /// The plugin's file name.
    pub file: String,
}

/// What the other players' games play on a teleport's caster's figure.
#[derive(Debug, Clone, PartialEq)]
pub struct TeleportLook {
    /// A VisualEffect played from the cast until the landing.
    pub vanish: Option<FormSpec>,
    /// A VisualEffect played at the landing.
    pub appear: Option<FormSpec>,
    /// How long the appear plays, seconds (0 to MAX_APPEAR_SECONDS).
    pub appear_seconds: f32,
    /// A Sound played at the landing.
    pub sound: Option<FormSpec>,
}

/// What the server does with a named spell's cast.
#[derive(Debug, Clone, PartialEq)]
pub struct CastTeleport {
    /// How far from the caster its cast may land, units (0: back where it
    /// cast, within movement::CAST_JUMP_SLACK).
    pub reach: f32,
    /// How long after the cast the jump is permitted, ms.
    pub wait_ms: u64,
    /// Whether the cast is relayed to the neighbours.
    pub relay: bool,
    /// What the other players' games play on the caster's figure, when the
    /// entry names it.
    pub look: Option<TeleportLook>,
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

/// A form as an entry writes it, "<hex form id>:<plugin>" (the id may carry
/// "0x"), parsed as forms::FormDesc parses one; the error names the text.
pub(crate) fn form_spec(text: &str) -> Result<FormSpec, SettingsError> {
    let FormDesc { short_id, file } = FormDesc::parse(text).map_err(|why| SettingsError(format!("castTeleports: {why}")))?;
    Ok(FormSpec { short_id, file })
}

/// The look of the entry for `spell`, its forms written as the spell is.
fn parse_look(spell: &str, entry: LookEntry) -> Result<TeleportLook, SettingsError> {
    let spec = |text: Option<String>| text.as_deref().map(form_spec).transpose();
    let look = TeleportLook {
        vanish: spec(entry.vanish)?,
        appear: spec(entry.appear)?,
        appear_seconds: entry.appear_seconds.unwrap_or(APPEAR_SECONDS),
        sound: spec(entry.sound)?,
    };
    if look.vanish.is_none() && look.appear.is_none() && look.sound.is_none() {
        return Err(SettingsError(format!("castTeleports: {spell:?} has a look that names no vanish, appear or sound")));
    }
    if !(look.appear_seconds.is_finite() && (0.0..=MAX_APPEAR_SECONDS).contains(&look.appear_seconds)) {
        return Err(SettingsError(format!("castTeleports: {spell:?} has a look whose appearSeconds is not a number from 0 to 10")));
    }
    Ok(look)
}

impl CastTeleports {
    /// The list from the `castTeleports` block's JSON text (`[]` for none).
    /// The error names a bad entry: a spell, or a look's form, that is not
    /// "<hex id>:<plugin>", a reach that is not a finite number of 0 or
    /// more, a wait of 0, a look that names nothing or whose appearSeconds
    /// is not from 0 to 10, an unknown key.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let entries: Vec<Entry> = serde_json::from_str(text).map_err(|e| SettingsError(format!("castTeleports: {e}")))?;
        let mut spells = Vec::with_capacity(entries.len());
        for e in entries {
            let FormSpec { short_id, file } = form_spec(&e.spell)?;
            if !(e.reach.is_finite() && e.reach >= 0.0) {
                return Err(SettingsError(format!("castTeleports: {:?} has a reach that is not a number of 0 or more", e.spell)));
            }
            let wait_ms = e.wait_ms.unwrap_or(CAST_JUMP_WAIT_MS);
            if wait_ms == 0 {
                return Err(SettingsError(format!("castTeleports: {:?} has a wait of 0", e.spell)));
            }
            let look = e.look.map(|l| parse_look(&e.spell, l)).transpose()?;
            let teleport = CastTeleport { reach: e.reach, wait_ms, relay: e.relay.unwrap_or(false), look };
            spells.push(Named { short_id, file, teleport });
        }
        Ok(Self { spells })
    }

    /// The spell `short_id` in `file` (a FormDesc's parts), if the server
    /// names it as a teleport.
    #[must_use]
    pub fn named(&self, short_id: u32, file: &str) -> Option<&CastTeleport> {
        self.spells
            .iter()
            .find(|s| s.short_id == short_id && s.file.eq_ignore_ascii_case(file))
            .map(|s| &s.teleport)
    }

    /// The reach of the spell `short_id` in `file`, if the server names it
    /// as a teleport.
    #[must_use]
    pub fn reach_of(&self, short_id: u32, file: &str) -> Option<f32> {
        self.named(short_id, file).map(|t| t.reach)
    }
}

/// Whether a move of `step` units over the ground, which the movement bounds
/// took while a teleport's look waits for its landing, is that landing
/// (LOOK_LANDING_STEP).
#[must_use]
pub fn look_lands(step: f32) -> bool {
    step.is_finite() && step > LOOK_LANDING_STEP
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
            Some(&CastTeleport { reach: 8000.0, wait_ms: CAST_JUMP_WAIT_MS, relay: false, look: None })
        );
        assert_eq!(
            t.named(0xaae8, "apocalypse - magic of skyrim.esp"),
            Some(&CastTeleport { reach: 0.0, wait_ms: 15_400, relay: true, look: None })
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

    /// Lost Grimoire's marker script's forms (thuum docs/verbs/cast-teleport.md:
    /// MGTeleportOutEffect, MGTeleportInEffect, and Dawnguard's
    /// MAGPowersSupernaturalReflexesInMarker), as the lab's settings name them.
    const LOOK: &str = r#"{"vanish": "102731:Skyrim.esm", "appear": "102730:Skyrim.esm", "appearSeconds": 1.75, "sound": "010ec4:Dawnguard.esm"}"#;

    fn with_look(look: &str) -> Result<CastTeleports, SettingsError> {
        CastTeleports::from_json(&format!(r#"[{{"spell": "a86df:LostGrimoire.esp", "reach": 8000, "look": {look}}}]"#))
    }

    fn spec(short_id: u32, file: &str) -> Option<FormSpec> {
        Some(FormSpec { short_id, file: file.to_owned() })
    }

    #[test]
    fn a_look_names_its_forms_and_its_appear_time() {
        let t = with_look(LOOK).unwrap_or_default();
        assert_eq!(
            t.named(0xa86df, "LostGrimoire.esp").and_then(|c| c.look.clone()),
            Some(TeleportLook {
                vanish: spec(0x102731, "Skyrim.esm"),
                appear: spec(0x102730, "Skyrim.esm"),
                appear_seconds: 1.75,
                sound: spec(0x10ec4, "Dawnguard.esm"),
            })
        );
        // the rest of the entry as without one
        assert_eq!(t.reach_of(0xa86df, "LostGrimoire.esp"), Some(8000.0));
        assert_eq!(t.named(0xa86df, "LostGrimoire.esp").map(|c| (c.wait_ms, c.relay)), Some((CAST_JUMP_WAIT_MS, false)));
        // a part alone, its form written as a spell may be; the appear's
        // time defaults to the mod's
        let appear = with_look(r#"{"appear": "0x102730:Skyrim.esm"}"#).unwrap_or_default();
        assert_eq!(
            appear.named(0xa86df, "LostGrimoire.esp").and_then(|c| c.look.clone()),
            Some(TeleportLook { vanish: None, appear: spec(0x102730, "Skyrim.esm"), appear_seconds: APPEAR_SECONDS, sound: None })
        );
        let bounds = with_look(r#"{"sound": "010ec4:Dawnguard.esm", "appearSeconds": 0}"#).unwrap_or_default();
        assert_eq!(bounds.named(0xa86df, "LostGrimoire.esp").and_then(|c| c.look.as_ref()).map(|l| l.appear_seconds), Some(0.0));
        assert!(with_look(r#"{"appear": "102730:Skyrim.esm", "appearSeconds": 10}"#).is_ok());
    }

    #[test]
    fn bad_looks_refused() {
        for bad in [
            // a form that is not <hex id>:<plugin>
            r#"{"vanish": "102731"}"#,
            r#"{"appear": "zz:Skyrim.esm"}"#,
            r#"{"sound": "010ec4:"}"#,
            r#"{"vanish": 1058609}"#,
            // an appear time that is negative, past 10 s, past an f32, or no number
            r#"{"appear": "102730:Skyrim.esm", "appearSeconds": -1}"#,
            r#"{"appear": "102730:Skyrim.esm", "appearSeconds": 10.5}"#,
            r#"{"appear": "102730:Skyrim.esm", "appearSeconds": 1e39}"#,
            r#"{"appear": "102730:Skyrim.esm", "appearSeconds": "1.75"}"#,
            // an unknown key, a look that names nothing, one that is no object
            r#"{"appear": "102730:Skyrim.esm", "shader": "1:Skyrim.esm"}"#,
            "{}",
            r#"{"appearSeconds": 1.75}"#,
            r#""102730:Skyrim.esm""#,
        ] {
            assert!(with_look(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn an_entry_without_a_look_has_none() {
        let t = CastTeleports::from_json(r#"[{"spell": "a86df:LostGrimoire.esp", "reach": 8000}]"#).unwrap_or_default();
        assert_eq!(t.named(0xa86df, "LostGrimoire.esp").map(|c| c.look.is_none()), Some(true));
    }

    #[test]
    fn a_landing_is_a_step_no_run_covers() {
        // b-teleport's landing, about 550 units on (docs/verbs/cast-teleport.md)
        assert!(look_lands(549.0));
        assert!(look_lands(LOOK_LANDING_STEP + 1.0));
        // a report's run at the fastest movement, and one after a short stall
        assert!(!look_lands(86.0));
        assert!(!look_lands(LOOK_LANDING_STEP));
        assert!(!look_lands(f32::NAN));
        assert!(!look_lands(f32::INFINITY));
    }
}
