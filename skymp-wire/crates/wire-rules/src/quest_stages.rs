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
//! player moves the player and starts scenes. In mode all a quest the game
//! starts by itself (QUST DNAM's Start Game Enabled: a layer's controller,
//! MCM, version-tracker and player-alias quests, the alternate start's own,
//! 330 of Skyrim.esm's 1811) is each seat's own unless `allowed` names it
//! (memo D62, 2026-10-29); the flag is the record's, so the server reads it
//! beside this rule and asks `is_allowed` here. The record itself is bounded
//! at QUEST_RECORD_CAP pairs (rule 12's resource bounds: a client's reports
//! feed it), and the stage index of a report is the record's to check (QUST
//! INDX, read beside this rule in the handler as the start flag is).

use crate::clock::SettingsError;
use crate::forms::FormDesc;
use std::collections::{BTreeSet, HashMap};

/// How many (quest, stage) pairs the world's record holds at most. The
/// record grows by the seats' reports (in mode all any quest a seat starts),
/// so it is bounded here as every client-fed table is (.claude/rules/rust.md:
/// resource bounds are explicit). 4096 is a bound, not a measurement: a few
/// stages for each of the few hundred quests one world's playthroughs reach
/// of Skyrim.esm's 1811 (the lab reads how far a playtest's record grows,
/// UNMEASURED), at 4096 comparisons per report in `done` and `merge`. A
/// report past it is refused by name (E_QUEST_FULL in the handler) and the
/// record's stages stay.
pub const QUEST_RECORD_CAP: usize = 4096;

/// Whether a record of `pairs` entries is at QUEST_RECORD_CAP, so no new
/// stage can enter it (a stage already in it is a no-op as ever).
#[must_use]
pub const fn full(pairs: usize) -> bool {
    pairs >= QUEST_RECORD_CAP
}

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
    /// Mode all's start-game-enabled quests that cross all the same, in the
    /// settings' order (memo D62).
    allowed: Vec<FormDesc>,
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
    /// `{"mode": "all", "denied": [...], "allowed": [...]}` (both lists
    /// optional, the same strings; `allowed` the start-game-enabled quests
    /// that cross, memo D62) is mode all. The error names what is wrong: an
    /// entry that is not such a string, a quest named twice or both denied
    /// and allowed, a mode other than "all", a field other than the three,
    /// or a block of another shape.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let block: serde_json::Value = serde_json::from_str(text).map_err(|e| SettingsError(format!("sharedQuests: {e}")))?;
        match block {
            serde_json::Value::Array(entries) => Ok(Self { mode: Mode::List, listed: parse_quests(&entries, "sharedQuests")?, denied: Vec::new(), allowed: Vec::new() }),
            serde_json::Value::Object(fields) => {
                if let Some(key) = fields.keys().find(|k| *k != "mode" && *k != "denied" && *k != "allowed") {
                    return Err(SettingsError(format!("sharedQuests: {key:?} is no field of the mode block (mode, denied, allowed)")));
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
                let allowed = match fields.get("allowed") {
                    None => Vec::new(),
                    Some(serde_json::Value::Array(entries)) => parse_quests(entries, "sharedQuests.allowed")?,
                    Some(other) => return Err(SettingsError(format!("sharedQuests: allowed {other} is not a list of \"<hex id>:<plugin>\""))),
                };
                if let Some(both) = allowed.iter().find(|a| denied.iter().any(|d| d.is(a.short_id, &a.file))) {
                    return Err(SettingsError(format!("sharedQuests: {:x}:{} is both denied and allowed", both.short_id, both.file)));
                }
                Ok(Self { mode: Mode::All, listed: Vec::new(), denied, allowed })
            }
            other => Err(SettingsError(format!("sharedQuests: {other} is neither a list of quests nor {{\"mode\": \"all\", \"denied\": [...], \"allowed\": [...]}}"))),
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

    /// Whether mode all's `allowed` names the quest `short_id` in `file`: a
    /// quest the game starts by itself that crosses all the same (memo D62).
    /// The flag is the record's and the server reads it; this rule holds the
    /// list, empty in mode list, where the curated list names what crosses.
    #[must_use]
    pub fn is_allowed(&self, short_id: u32, file: &str) -> bool {
        self.allowed.iter().any(|a| a.is(short_id, file))
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
    /// on the seat sees the stages before it. One pass over the record, a
    /// set of stages per quest (a duplicate an older record holds folds
    /// in), so a record at QUEST_RECORD_CAP replays in its size's time, not
    /// its square.
    #[must_use]
    pub fn at_login(&self, record: &[StageDone]) -> Vec<StageDone> {
        let mut quests: Vec<&FormDesc> = Vec::new();
        let mut stages: HashMap<&FormDesc, BTreeSet<u16>> = HashMap::new();
        for r in record {
            if !self.is_shared(r.quest.short_id, &r.quest.file) {
                continue;
            }
            stages
                .entry(&r.quest)
                .or_insert_with(|| {
                    quests.push(&r.quest);
                    BTreeSet::new()
                })
                .insert(r.stage);
        }
        quests
            .iter()
            .flat_map(|q| stages.get(q).into_iter().flatten().map(|s| StageDone { quest: (*q).clone(), stage: *s }))
            .collect()
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
/// others untouched. A record at QUEST_RECORD_CAP takes no new stage and
/// comes back unchanged (the handler asks `full` first and refuses the
/// report by name); a stage already in it is a no-op either way.
#[must_use]
pub fn merge(record: &[StageDone], stage: &StageDone) -> Vec<StageDone> {
    let mut out = record.to_vec();
    if done(&out, stage) || full(out.len()) {
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
    fn the_mode_block_takes_an_allowlist_and_refuses_a_quest_both_denied_and_allowed() {
        // memo D62: MS13 0x39645 is Start Game Enabled (Skyrim.esm 1.6.1170, DNAM main flags 0x11, thuum lab/esm.py), MQ102 0x4e50d is not
        let all = SharedQuests::from_json(r#"{"mode": "all", "denied": ["04e50d:Skyrim.esm"], "allowed": ["039645:Skyrim.esm"]}"#);
        assert!(all.as_ref().is_ok_and(|a| a.mode() == Mode::All && a.is_allowed(0x39645, "Skyrim.esm") && !a.is_allowed(0x39645, "Update.esm") && !a.is_allowed(0x4e50d, "Skyrim.esm")));
        // the allowed list changes nothing else: the login names the denied alone, the check passes the quest as any other
        assert!(all.as_ref().is_ok_and(|a| a.listed() == [quest(0x4e50d)] && a.check(&quest(0x39645)) == Ok(()) && a.is_shared(0x39645, "Skyrim.esm")));
        assert!(SharedQuests::from_json(r#"{"mode": "all", "allowed": ["039645:Skyrim.esm"]}"#).is_ok_and(|a| a.is_allowed(0x39645, "Skyrim.esm") && a.listed().is_empty()));
        assert!(SharedQuests::from_json(r#"{"mode": "all", "allowed": []}"#).is_ok_and(|a| !a.is_allowed(0x39645, "Skyrim.esm")));
        assert!(SharedQuests::from_json(r#"{"mode": "all"}"#).is_ok_and(|a| !a.is_allowed(0x39645, "Skyrim.esm")));
        // mode list has no allowed list: the curated list names what crosses
        assert!(SharedQuests::from_json(r#"["039645:Skyrim.esm"]"#).is_ok_and(|l| !l.is_allowed(0x39645, "Skyrim.esm")));
        assert!(!SharedQuests::default().is_allowed(0x39645, "Skyrim.esm"));
        // the refusals: a quest both denied and allowed (however spelled), a list of another shape, a quest allowed twice, a bad entry, a stray field
        assert!(SharedQuests::from_json(r#"{"mode": "all", "denied": ["039645:Skyrim.esm"], "allowed": ["39645:Skyrim.esm"]}"#).is_err());
        assert!(SharedQuests::from_json(r#"{"mode": "all", "allowed": "039645:Skyrim.esm"}"#).is_err());
        assert!(SharedQuests::from_json(r#"{"mode": "all", "allowed": ["039645:Skyrim.esm", "39645:Skyrim.esm"]}"#).is_err());
        assert!(SharedQuests::from_json(r#"{"mode": "all", "allowed": ["039645"]}"#).is_err());
        assert!(SharedQuests::from_json(r#"{"mode": "all", "allow": ["039645:Skyrim.esm"]}"#).is_err());
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

    /// A record of exactly QUEST_RECORD_CAP pairs: quests 1 to 16, stages 0
    /// to 255 each, in that order.
    fn record_at_the_cap() -> Vec<StageDone> {
        let mut record = Vec::new();
        for q in 1..=16u32 {
            for s in 0..=255u16 {
                record.push(at(q, s));
            }
        }
        record
    }

    #[test]
    fn the_record_stops_at_the_cap_and_a_recorded_stage_is_still_done_there() {
        let record = record_at_the_cap();
        assert_eq!(record.len(), QUEST_RECORD_CAP);
        assert!(full(record.len()));
        // a new stage of a quest in the record, and one of a new quest: the
        // record unchanged either way (the handler names the refusal)
        assert_eq!(merge(&record, &at(1, 300)), record);
        assert_eq!(merge(&record, &at(99, 10)), record);
        // a stage already in it is done, a no-op as before the cap
        assert!(done(&record, &at(16, 255)));
        assert_eq!(merge(&record, &at(16, 255)), record);
        // one below the cap takes the one stage more, then is full
        let mut below = record.clone();
        below.pop();
        assert!(!full(below.len()));
        let filled = merge(&below, &at(16, 255));
        assert_eq!(filled, record);
        assert!(full(filled.len()));
        assert!(!full(0));
    }

    #[test]
    fn the_login_replays_a_record_at_the_cap_in_one_pass() {
        // twice the cap's pairs, each one twice (an older record's
        // duplicate), quests and stages in reverse; quest 8 denied
        let all = SharedQuests::from_json(r#"{"mode": "all", "denied": ["000008:Skyrim.esm"]}"#).unwrap_or_default();
        let mut record = Vec::new();
        for q in (1..=16u32).rev() {
            for s in (0..=255u16).rev() {
                record.push(at(q, s));
                record.push(at(q, s));
            }
        }
        // the quests in the record's order (16 first), ascending within each,
        // once each, the denied quest left out
        let mut expected = Vec::new();
        for q in (1..=16u32).rev() {
            if q == 8 {
                continue;
            }
            for s in 0..=255u16 {
                expected.push(at(q, s));
            }
        }
        assert_eq!(all.at_login(&record), expected);
        // the same shape under mode list, one quest listed
        let list = SharedQuests::from_json(r#"["000003:Skyrim.esm"]"#).unwrap_or_default();
        let listed: Vec<StageDone> = (0..=255u16).map(|s| at(3, s)).collect();
        assert_eq!(list.at_login(&record), listed);
    }
}
