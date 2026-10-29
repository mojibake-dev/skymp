//! The world's quest stages (thuum docs/verbs/quest-stage.md, ADR-056). A
//! quest stage set in one player's game (a dialogue fragment, a script, the
//! lab's console) is reported by that game and becomes the world's: the
//! server records it once, relays it to every other seat and replays the
//! record after a login, and each seat applies it through the engine's own
//! SetStage, so topics, conditions and the journal read one quest state. The
//! records cannot say which quests are the world's, so server-settings.json
//! names them: `sharedQuests`, a list of FormDescs ("<hex form id>:<plugin>"),
//! Riverwood's quests first; a quest off the list stays each seat's own, and
//! the list is what keeps the whole world's quest state off a fresh player
//! (the main quest's stages move the player and start scenes).

use crate::clock::SettingsError;
use crate::forms::FormDesc;

/// The quests whose stages are the world's.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct SharedQuests {
    listed: Vec<FormDesc>,
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
    /// A quest the server does not share.
    Unlisted,
}

impl SharedQuests {
    /// The list from the `sharedQuests` block's JSON text (`[]` for none):
    /// each entry a "<hex id>:<plugin>" string. The error names a bad entry:
    /// one that is not such a string, or a quest listed twice.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let entries: Vec<serde_json::Value> = serde_json::from_str(text).map_err(|e| SettingsError(format!("sharedQuests: {e}")))?;
        let mut listed: Vec<FormDesc> = Vec::with_capacity(entries.len());
        for e in entries {
            let serde_json::Value::String(name) = &e else {
                return Err(SettingsError(format!("sharedQuests: {e} is not \"<hex id>:<plugin>\"")));
            };
            let d = FormDesc::parse(name).map_err(|why| SettingsError(format!("sharedQuests: {why}")))?;
            if listed.iter().any(|l| l.is(d.short_id, &d.file)) {
                return Err(SettingsError(format!("sharedQuests: {name:?} is listed twice")));
            }
            listed.push(d);
        }
        Ok(Self { listed })
    }

    /// Whether the quest `short_id` in `file` is the world's.
    #[must_use]
    pub fn is_listed(&self, short_id: u32, file: &str) -> bool {
        self.listed.iter().any(|l| l.is(short_id, file))
    }

    /// The listed quests, in the settings' order: what a client reports.
    #[must_use]
    pub fn listed(&self) -> &[FormDesc] {
        &self.listed
    }

    /// Judge a report's quest: listed, or refused.
    pub fn check(&self, quest: &FormDesc) -> Result<(), Refusal> {
        if self.is_listed(quest.short_id, &quest.file) {
            Ok(())
        } else {
            Err(Refusal::Unlisted)
        }
    }

    /// What the login sends: the recorded stages of quests still listed (a
    /// record made under a longer list keeps the rest, unsent), in the
    /// record's quest order and ascending within a quest, so each SetStage
    /// on the seat sees the stages before it.
    #[must_use]
    pub fn at_login(&self, record: &[StageDone]) -> Vec<StageDone> {
        let mut out: Vec<StageDone> = Vec::with_capacity(record.len());
        for r in record {
            if !self.is_listed(r.quest.short_id, &r.quest.file) || out.iter().any(|o| o.quest == r.quest && o.stage == r.stage) {
                continue;
            }
            // insert after the last stage of the same quest that is below it,
            // keeping the first quest's block first
            let block_end = out.iter().rposition(|o| o.quest == r.quest).map_or(out.len(), |i| i.saturating_add(1));
            let block_start = out.iter().position(|o| o.quest == r.quest).unwrap_or(block_end);
            let at = out[block_start..block_end].iter().position(|o| o.stage > r.stage).map_or(block_end, |i| block_start.saturating_add(i));
            out.insert(at, r.clone());
        }
        out
    }
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
    let block_end = out.iter().rposition(|o| o.quest == stage.quest).map_or(out.len(), |i| i.saturating_add(1));
    let block_start = out.iter().position(|o| o.quest == stage.quest).unwrap_or(block_end);
    let at = out[block_start..block_end].iter().position(|o| o.stage > stage.stage).map_or(block_end, |i| block_start.saturating_add(i));
    out.insert(at, stage.clone());
    out
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
        assert!(list.as_ref().is_ok_and(|l| l.listed().len() == 2 && l.is_listed(0x74a83, "Skyrim.esm") && !l.is_listed(0x74a84, "Skyrim.esm")));
        assert!(SharedQuests::from_json(r#"[{"quest": "074a83:Skyrim.esm"}]"#).is_err());
        assert!(SharedQuests::from_json(r#"["074a83"]"#).is_err());
        assert!(SharedQuests::from_json(r#"["074a83:Skyrim.esm", "74a83:Skyrim.esm"]"#).is_err());
        assert!(SharedQuests::from_json("[]").is_ok_and(|l| l.listed().is_empty()));
    }

    #[test]
    fn a_listed_quest_passes_and_an_unlisted_one_is_refused() {
        let list = SharedQuests::from_json(r#"["074a83:Skyrim.esm"]"#).unwrap_or_default();
        assert_eq!(list.check(&quest(0x74a83)), Ok(()));
        assert_eq!(list.check(&quest(0x74a84)), Err(Refusal::Unlisted));
        assert_eq!(list.check(&FormDesc { short_id: 0x74a83, file: "Update.esm".to_owned() }), Err(Refusal::Unlisted));
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
}
