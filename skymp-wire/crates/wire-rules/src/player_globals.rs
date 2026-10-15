//! A player's own global variables (thuum docs/verbs/player-globals.md,
//! ADR-029's A2). The globals a player's game writes for its player
//! (Apocalypse's progress and its MCM settings) are kept per player, R2:
//! recorded unvalidated, since checking a write would need the script that
//! made it to run on the server. The player's game reports the listed
//! globals' values as they change, and the server sets the recorded ones in
//! its game at login. The records cannot say whose a global is, so
//! server-settings.json names them: `playerGlobals`, a list of FormDescs
//! ("<hex form id>:<plugin>").

use crate::clock::SettingsError;
use crate::forms::FormDesc;

/// The globals the server keeps per player.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct PlayerGlobals {
    listed: Vec<FormDesc>,
}

/// One global's value.
#[derive(Debug, Clone, PartialEq)]
pub struct GlobalValue {
    /// The global, as the change form names it.
    pub global: FormDesc,
    /// Its value (a TESGlobal holds a float whatever its type,
    /// CommonLibSSE-NG include/RE/T/TESGlobal.h:45).
    pub value: f32,
}

/// Why a report is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Refusal {
    /// A global the server does not keep per player.
    Unlisted,
    /// A value that is not a finite number.
    NotFinite,
    /// One global twice in a report.
    Duplicate,
    /// More entries than the list holds.
    TooMany,
}

impl PlayerGlobals {
    /// The list from the `playerGlobals` block's JSON text (`[]` for none).
    /// The error names a bad entry: one that is not "<hex id>:<plugin>", or
    /// a global listed twice.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let entries: Vec<String> = serde_json::from_str(text).map_err(|e| SettingsError(format!("playerGlobals: {e}")))?;
        let mut listed: Vec<FormDesc> = Vec::with_capacity(entries.len());
        for e in entries {
            let d = FormDesc::parse(&e).map_err(|why| SettingsError(format!("playerGlobals: {why}")))?;
            if listed.iter().any(|l| l.is(d.short_id, &d.file)) {
                return Err(SettingsError(format!("playerGlobals: {e:?} is listed twice")));
            }
            listed.push(d);
        }
        Ok(Self { listed })
    }

    /// Whether the server keeps the global `short_id` in `file` per player.
    #[must_use]
    pub fn is_listed(&self, short_id: u32, file: &str) -> bool {
        self.listed.iter().any(|l| l.is(short_id, file))
    }

    /// The listed globals, in the settings' order: what a client watches.
    #[must_use]
    pub fn listed(&self) -> &[FormDesc] {
        &self.listed
    }

    /// Judge a player's report: every global listed, once, its value
    /// finite, no more entries than the list holds.
    pub fn check(&self, report: &[GlobalValue]) -> Result<(), Refusal> {
        if report.len() > self.listed.len() {
            return Err(Refusal::TooMany);
        }
        for (i, r) in report.iter().enumerate() {
            if !self.is_listed(r.global.short_id, &r.global.file) {
                return Err(Refusal::Unlisted);
            }
            if !r.value.is_finite() {
                return Err(Refusal::NotFinite);
            }
            if report.iter().take(i).any(|p| p.global.is(r.global.short_id, &r.global.file)) {
                return Err(Refusal::Duplicate);
            }
        }
        Ok(())
    }

    /// What the login sends: the recorded values of globals still listed (a
    /// record made under a longer list keeps the rest, unsent).
    #[must_use]
    pub fn at_login(&self, record: &[GlobalValue]) -> Vec<GlobalValue> {
        record.iter().filter(|r| self.is_listed(r.global.short_id, &r.global.file)).cloned().collect()
    }
}

/// The record after a checked report: a reported global's value replaces
/// its recorded one, a global reported for the first time is added, the
/// others stay.
#[must_use]
pub fn merge(record: &[GlobalValue], report: &[GlobalValue]) -> Vec<GlobalValue> {
    let mut out = record.to_vec();
    for r in report {
        match out.iter_mut().find(|o| o.global.is(r.global.short_id, &r.global.file)) {
            Some(o) => o.value = r.value,
            None => out.push(r.clone()),
        }
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    const APOC: &str = "Apocalypse - Magic of Skyrim.esp";

    fn g(short_id: u32, value: f32) -> GlobalValue {
        GlobalValue { global: FormDesc { short_id, file: APOC.to_owned() }, value }
    }

    // the Craftlord's unspent points and health multiplier, and the MCM's
    // general XP multiplier (the globals survey, 4d)
    fn list() -> PlayerGlobals {
        PlayerGlobals::from_json(
            r#"["0b1c19:Apocalypse - Magic of Skyrim.esp", "0b1c1b:Apocalypse - Magic of Skyrim.esp",
                "043cef:Apocalypse - Magic of Skyrim.esp"]"#,
        )
        .unwrap_or_default()
    }

    #[test]
    fn the_list_names_globals_once_each() {
        let l = list();
        assert_eq!(l.listed().len(), 3);
        assert!(l.is_listed(0xb1c19, "apocalypse - magic of skyrim.esp"));
        assert!(!l.is_listed(0xb1c19, "Skyrim.esm"));
        assert!(PlayerGlobals::from_json("[]").is_ok_and(|l| l.listed().is_empty()));
        for bad in [
            r#"["0b1c19"]"#,
            r#"["0b1c19:Apocalypse - Magic of Skyrim.esp", "0x0B1C19:apocalypse - magic of skyrim.esp"]"#,
            r#"[3]"#,
            r#"{"g": "0b1c19:x.esp"}"#,
            "",
        ] {
            assert!(PlayerGlobals::from_json(bad).is_err(), "{bad}");
        }
    }

    #[test]
    fn a_report_of_listed_finite_values_once_each_is_taken() {
        let l = list();
        assert_eq!(l.check(&[g(0xb1c19, 3.0), g(0x43cef, 1.5)]), Ok(()));
        assert_eq!(l.check(&[]), Ok(()));
        assert_eq!(l.check(&[g(0xb1c1a, 1.0)]), Err(Refusal::Unlisted));
        assert_eq!(l.check(&[g(0xb1c19, f32::NAN)]), Err(Refusal::NotFinite));
        assert_eq!(l.check(&[g(0xb1c19, f32::INFINITY)]), Err(Refusal::NotFinite));
        assert_eq!(l.check(&[g(0xb1c19, 1.0), g(0xb1c19, 2.0)]), Err(Refusal::Duplicate));
        let four = [g(0xb1c19, 1.0), g(0xb1c1b, 1.0), g(0x43cef, 1.0), g(0x43cef, 1.0)];
        assert_eq!(l.check(&four), Err(Refusal::TooMany));
    }

    #[test]
    fn the_latest_value_wins_and_the_rest_stay() {
        let record = merge(&[], &[g(0xb1c19, 3.0)]);
        let record = merge(&record, &[g(0x43cef, 2.0)]);
        let record = merge(&record, &[g(0xb1c19, 1.0)]);
        assert_eq!(record, vec![g(0xb1c19, 1.0), g(0x43cef, 2.0)]);
    }

    #[test]
    fn the_login_sends_what_is_still_listed() {
        let record = vec![g(0xb1c19, 1.0), g(0x99999, 7.0)];
        assert_eq!(list().at_login(&record), vec![g(0xb1c19, 1.0)]);
        assert!(PlayerGlobals::default().at_login(&record).is_empty());
    }
}
