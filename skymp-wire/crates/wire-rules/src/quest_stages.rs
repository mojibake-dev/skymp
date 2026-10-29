//! The world's quest stages (thuum docs/verbs/quest-stage.md, ADR-056). A
//! quest stage set in one player's game (a dialogue fragment, a script, the
//! lab's console) is reported by that game and becomes the world's: the
//! server records it once, relays it to every other seat and replays the
//! record after a login, and each seat applies it through the engine's own
//! SetStage, so topics, conditions and the journal read one quest state. The
//! records cannot say which quests are the world's, so server-settings.json
//! says, `sharedQuests`, in one of two modes (memo D58 B, 2026-10-29): a
//! list of FormDescs ("<hex form id>:<plugin>") is mode list, Riverwood's
//! quests first, and a quest off the list stays each seat's own; the object
//! `{"mode": "all", "denied": [FormDesc, ...]}` is mode all, TES3MP's shared
//! journal: every quest a seat reports is the world's unless denied. The
//! denied list holds what must stay a seat's own (the main quest's intro
//! chain, the DLC starts), because replaying their stages onto a fresh
//! player moves the player and starts scenes.

use crate::clock::SettingsError;
use crate::forms::FormDesc;

/// How the server decides which quests are the world's.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum Mode {
    /// A quest is the world's when `sharedQuests` lists it.
    #[default]
    List,
    /// Every quest is the world's unless `denied` names it.
    All,
}

impl Mode {
    /// The wire's value (QuestStage `mode`): 0 list, 1 all.
    #[must_use]
    pub fn wire(self) -> u8 {
        match self {
            Self::List => 0,
            Self::All => 1,
        }
    }
}

/// The quests whose stages are the world's.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct SharedQuests {
    mode: Mode,
    /// Mode list's quests, in the settings' order.
    listed: Vec<FormDesc>,
    /// Mode all's exceptions, in the settings' order.
    denied: Vec<FormDesc>,
}

/// One stage done on a shared quest.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct StageDone {
    /// The quest, as the change form names it.
    pub quest: FormDesc,
    /// The stage index the engine set (Quest.SetStage's, a u16 in the
    /// engine).
    pub stage: u16,
}

/// Why a report is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Refusal {
    /// A quest the server does not share (mode list).
    Unlisted,
    /// A quest the server denies (mode all).
    Denied,
}

impl SharedQuests {
    /// The setting from the `sharedQuests` block's JSON text: a list of
    /// "<hex id>:<plugin>" strings (`[]` for none) is mode list; the object
    /// `{"mode": "all", "denied": [...]}` (the list optional, the same
    /// strings) is mode all. The error names what is wrong: an entry that is
    /// not such a string, a quest named twice, a mode other than "all", a
    /// field other than the two, or a block of another shape.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let block: serde_json::Value = serde_json::from_str(text).map_err(|e| SettingsError(format!("sharedQuests: {e}")))?;
        match block {
            serde_json::Value::Array(entries) => Ok(Self { mode: Mode::List, listed: parse_quests(&entries, "sharedQuests")?, denied: Vec::new() }),
            serde_json::Value::Object(fields) => {
                if let Some(key) = fields.keys().find(|k| *k != "mode" && *k != "denied") {
                    return Err(SettingsError(format!("sharedQuests: {key:?} is no field of the mode block (mode, denied)")));
                }
                match fields.get("mode") {
                    Some(serde_json::Value::String(mode)) if mode == "all" => {}
                    Some(other) => return Err(SettingsError(format!("sharedQuests: mode {other} is not \"all\" (a list of quests is mode list)"))),
                    None => return Err(SettingsError("sharedQuests: the mode block names no \"mode\"".to_owned())),
                }
                let denied = match fields.get("denied") {
                    None => Vec::new(),
                    Some(serde_json::Value::Array(entries)) => parse_quests(entries, "sharedQuests.denied")?,
                    Some(other) => return Err(SettingsError(format!("sharedQuests: denied {other} is not a list of \"<hex id>:<plugin>\""))),
                };
                Ok(Self { mode: Mode::All, listed: Vec::new(), denied })
            }
            other => Err(SettingsError(format!("sharedQuests: {other} is neither a list of quests nor {{\"mode\": \"all\", \"denied\": [...]}}"))),
        }
    }

    /// The sharing mode.
    #[must_use]
    pub fn mode(&self) -> Mode {
        self.mode
    }

    /// Whether the quest `short_id` in `file` is the world's: listed in mode
    /// list, not denied in mode all.
    #[must_use]
    pub fn is_shared(&self, short_id: u32, file: &str) -> bool {
        match self.mode {
            Mode::List => self.listed.iter().any(|l| l.is(short_id, file)),
            Mode::All => !self.is_denied(short_id, file),
        }
    }

    /// Whether mode all denies the quest `short_id` in `file`.
    #[must_use]
    pub fn is_denied(&self, short_id: u32, file: &str) -> bool {
        self.denied.iter().any(|d| d.is(short_id, file))
    }

    /// What the login names, in the settings' order: the shared quests in
    /// mode list (what a client reports), the denied quests in mode all
    /// (what a client never reports).
    #[must_use]
    pub fn listed(&self) -> &[FormDesc] {
        match self.mode {
            Mode::List => &self.listed,
            Mode::All => &self.denied,
        }
    }

    /// Judge a report's quest: the world's, or refused by the mode's rule.
    pub fn check(&self, quest: &FormDesc) -> Result<(), Refusal> {
        match self.mode {
            Mode::List if self.is_shared(quest.short_id, &quest.file) => Ok(()),
            Mode::List => Err(Refusal::Unlisted),
            Mode::All if self.is_denied(quest.short_id, &quest.file) => Err(Refusal::Denied),
            Mode::All => Ok(()),
        }
    }

    /// What the login sends: the recorded stages of quests still the world's
    /// (a record made under another setting keeps the rest, unsent), in the
    /// record's quest order and ascending within a quest, so each SetStage
    /// on the seat sees the stages before it.
    #[must_use]
    pub fn at_login(&self, record: &[StageDone]) -> Vec<StageDone> {
        let mut out: Vec<StageDone> = Vec::with_capacity(record.len());
        for r in record {
            if !self.is_shared(r.quest.short_id, &r.quest.file) || out.iter().any(|o| o.quest == r.quest && o.stage == r.stage) {
                continue;
            }
            // insert after the last stage of the same quest that is below it,
            // keeping the first quest's block first
            out.insert(insertion_point(&out, r), r.clone());
        }
        out
    }
}

/// A list of "<hex id>:<plugin>" strings as FormDescs, each quest once;
/// `what` names the block in the error.
fn parse_quests(entries: &[serde_json::Value], what: &str) -> Result<Vec<FormDesc>, SettingsError> {
    let mut quests: Vec<FormDesc> = Vec::with_capacity(entries.len());
    for e in entries {
        let serde_json::Value::String(name) = e else {
            return Err(SettingsError(format!("{what}: {e} is not \"<hex id>:<plugin>\"")));
        };
        let d = FormDesc::parse(name).map_err(|why| SettingsError(format!("{what}: {why}")))?;
        if quests.iter().any(|q| q.is(d.short_id, &d.file)) {
            return Err(SettingsError(format!("{what}: {name:?} is listed twice")));
        }
        quests.push(d);
    }
    Ok(quests)
}

/// Whether the record holds the stage already (a report of it is a no-op).
#[must_use]
pub fn done(record: &[StageDone], stage: &StageDone) -> bool {
    record.iter().any(|r| r.quest == stage.quest && r.stage == stage.stage)
}

/// The record after a checked report: the stage added once, after the
/// stages of its quest below it (a new quest's first stage goes last), the
/// others untouched.
#[must_use]
pub fn merge(record: &[StageDone], stage: &StageDone) -> Vec<StageDone> {
    let mut out = record.to_vec();
    if done(&out, stage) {
        return out;
    }
    out.insert(insertion_point(&out, stage), stage.clone());
    out
}

/// Where a stage goes in a record: inside its quest's block (the stages of
/// one quest sit together, ascending), before the first stage above it; at
/// the block's end when none is above it; at the record's end for a quest
/// not yet in it. Never past the record's length, so the insert cannot panic.
fn insertion_point(record: &[StageDone], stage: &StageDone) -> usize {
    let block_end = record.iter().rposition(|o| o.quest == stage.quest).map_or(record.len(), |i| i.saturating_add(1));
    let block_start = record.iter().position(|o| o.quest == stage.quest).unwrap_or(block_end);
    record
        .iter()
        .enumerate()
        .skip(block_start)
        .take(block_end.saturating_sub(block_start))
        .find(|(_, o)| o.stage > stage.stage)
        .map_or(block_end, |(i, _)| i)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn quest(short_id: u32) -> FormDesc {
        FormDesc { short_id, file: "Skyrim.esm".to_owned() }
    }

    fn at(short_id: u32, stage: u16) -> StageDone {
        StageDone { quest: quest(short_id), stage }
    }

    #[test]
    fn the_list_parses_strings_and_refuses_the_rest() {
        let list = SharedQuests::from_json(r#"["074a83:Skyrim.esm", "0e0d2e:Skyrim.esm"]"#);
        assert!(list.as_ref().is_ok_and(|l| l.mode() == Mode::List && l.listed().len() == 2 && l.is_shared(0x74a83, "Skyrim.esm") && !l.is_shared(0x74a84, "Skyrim.esm")));
        assert!(SharedQuests::from_json(r#"[{"quest": "074a83:Skyrim.esm"}]"#).is_err());
        assert!(SharedQuests::from_json(r#"["074a83"]"#).is_err());
        assert!(SharedQuests::from_json(r#"["074a83:Skyrim.esm", "74a83:Skyrim.esm"]"#).is_err());
        assert!(SharedQuests::from_json("[]").is_ok_and(|l| l.mode() == Mode::List && l.listed().is_empty()));
        assert!(SharedQuests::from_json(r#""all""#).is_err());
        assert!(SharedQuests::from_json("7").is_err());
    }

    #[test]
    fn the_mode_block_is_all_with_a_denylist_and_refuses_the_rest() {
        // MQ102 0x4e50d denied (Skyrim.esm, thuum lab/esm.py)
        let all = SharedQuests::from_json(r#"{"mode": "all", "denied": ["04e50d:Skyrim.esm"]}"#);
        assert!(all.as_ref().is_ok_and(|a| a.mode() == Mode::All && a.listed() == [quest(0x4e50d)]));
        assert!(all.as_ref().is_ok_and(|a| a.is_shared(0x74a83, "Skyrim.esm") && !a.is_shared(0x4e50d, "Skyrim.esm") && a.is_denied(0x4e50d, "Skyrim.esm")));
        assert!(all.as_ref().is_ok_and(|a| a.is_shared(0x4e50d, "Update.esm")));
        // nothing denied: every quest is the world's, the login names none
        assert!(SharedQuests::from_json(r#"{"mode": "all"}"#).is_ok_and(|a| a.mode() == Mode::All && a.listed().is_empty() && a.is_shared(0x4e50d, "Skyrim.esm")));
        assert!(SharedQuests::from_json(r#"{"mode": "all", "denied": []}"#).is_ok_and(|a| a.listed().is_empty()));
        // the refusals, each named
        assert!(SharedQuests::from_json(r#"{"mode": "list"}"#).is_err());
        assert!(SharedQuests::from_json(r#"{"denied": ["04e50d:Skyrim.esm"]}"#).is_err());
        assert!(SharedQuests::from_json(r#"{"mode": "all", "deny": ["04e50d:Skyrim.esm"]}"#).is_err());
        assert!(SharedQuests::from_json(r#"{"mode": "all", "denied": "04e50d:Skyrim.esm"}"#).is_err());
        assert!(SharedQuests::from_json(r#"{"mode": "all", "denied": ["04e50d"]}"#).is_err());
        assert!(SharedQuests::from_json(r#"{"mode": "all", "denied": ["04e50d:Skyrim.esm", "4e50d:Skyrim.esm"]}"#).is_err());
        assert!(SharedQuests::from_json(r#"{"mode": 1}"#).is_err());
        assert_eq!(Mode::List.wire(), 0);
        assert_eq!(Mode::All.wire(), 1);
    }

    #[test]
    fn a_listed_quest_passes_and_an_unlisted_one_is_refused() {
        let list = SharedQuests::from_json(r#"["074a83:Skyrim.esm"]"#).unwrap_or_default();
        assert_eq!(list.check(&quest(0x74a83)), Ok(()));
        assert_eq!(list.check(&quest(0x74a84)), Err(Refusal::Unlisted));
        assert_eq!(list.check(&FormDesc { short_id: 0x74a83, file: "Update.esm".to_owned() }), Err(Refusal::Unlisted));
    }

    #[test]
    fn in_mode_all_every_quest_passes_but_a_denied_one() {
        let all = SharedQuests::from_json(r#"{"mode": "all", "denied": ["04e50d:Skyrim.esm"]}"#).unwrap_or_default();
        assert_eq!(all.check(&quest(0x74a83)), Ok(()));
        assert_eq!(all.check(&quest(0xb6031)), Ok(()));
        assert_eq!(all.check(&quest(0x4e50d)), Err(Refusal::Denied));
        assert_eq!(all.check(&FormDesc { short_id: 0x4e50d, file: "Update.esm".to_owned() }), Ok(()));
        // an unparsed setting falls back to the default: mode list, empty
        assert_eq!(SharedQuests::default().check(&quest(0x74a83)), Err(Refusal::Unlisted));
    }

    #[test]
    fn the_record_keeps_each_stage_once_ascending_within_its_quest() {
        let r = merge(&[], &at(1, 20));
        let r = merge(&r, &at(1, 10));
        let r = merge(&r, &at(2, 5));
        let r = merge(&r, &at(1, 30));
        let r = merge(&r, &at(1, 20));
        assert_eq!(r, vec![at(1, 10), at(1, 20), at(1, 30), at(2, 5)]);
        assert!(done(&r, &at(1, 20)));
        assert!(!done(&r, &at(1, 25)));
        assert!(!done(&r, &at(3, 20)));
    }

    #[test]
    fn the_login_sends_listed_quests_ascending_and_skips_the_rest() {
        let list = SharedQuests::from_json(r#"["000001:Skyrim.esm"]"#).unwrap_or_default();
        let record = vec![at(1, 30), at(2, 5), at(1, 10), at(1, 10), at(1, 20)];
        assert_eq!(list.at_login(&record), vec![at(1, 10), at(1, 20), at(1, 30)]);
        assert!(list.at_login(&[]).is_empty());
    }

    #[test]
    fn in_mode_all_the_login_sends_every_recorded_quest_but_the_denied() {
        let all = SharedQuests::from_json(r#"{"mode": "all", "denied": ["000002:Skyrim.esm"]}"#).unwrap_or_default();
        // a record made under mode list that listed quest 2 keeps it, unsent
        let record = vec![at(2, 5), at(1, 30), at(3, 10), at(1, 10), at(1, 10), at(1, 20)];
        assert_eq!(all.at_login(&record), vec![at(1, 10), at(1, 20), at(1, 30), at(3, 10)]);
        assert!(all.at_login(&[]).is_empty());
        let none_denied = SharedQuests::from_json(r#"{"mode": "all"}"#).unwrap_or_default();
        assert_eq!(none_denied.at_login(&record), vec![at(2, 5), at(1, 10), at(1, 20), at(1, 30), at(3, 10)]);
    }
}
