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
//! player moves the player and starts scenes. The record itself is bounded
//! at QUEST_RECORD_CAP pairs (rule 12's resource bounds: a client's reports
//! feed it), and the stage index of a report is the record's to check (QUST
//! INDX, read beside this rule in the handler).
//!
//! Rung 5 (thuum ADR-068, docs/research/rung5-quests-2026-11-09.md section
//! 1(b) and 8(a)) retires memo D62's start-game-enabled rule: a quest the
//! game starts by itself crosses like any other, its stage 0 and its
//! start-up stages the start word's (wire-rules quest_runs), so mode all's
//! `allowed` names nothing any more. It stays a recognized key of the mode
//! block, read and ignored, and
//! [`SharedQuests::retired`] gives the core the one line it logs at load
//! (read 4: every image and T2's copy of the deployed settings carry it, and
//! an unknown key would refuse the whole setting). What stays each seat's
//! own beside the denied list is the player class (wire-rules quest_scope,
//! D-Q1's derivation): composed in by [`SharedQuests::set_player`] when the
//! scope acts, a player-class quest is never the world's in either mode, the
//! login names it among what a client never reports, and its report is
//! refused (`E_QUEST_PLAYER` in the handler, after the mode's own refusal).

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
    /// The player-class quests the scope composed in ([`SharedQuests::set_player`]),
    /// in the order given: never the world's, in either mode.
    player: Vec<FormDesc>,
    /// How many quests the block's retired `allowed` key named (rung 5,
    /// read 4), None when the block carries no such key.
    retired_allowed: Option<usize>,
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
    /// A player-class quest (rung 5, wire-rules quest_scope): each seat's
    /// own, in either mode.
    Player,
}

impl Refusal {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Refusal::Unlisted => "E_QUEST_UNLISTED",
            Refusal::Denied => "E_QUEST_DENIED",
            Refusal::Player => "E_QUEST_PLAYER",
        }
    }
}

impl SharedQuests {
    /// The setting from the `sharedQuests` block's JSON text: a list of
    /// "<hex id>:<plugin>" strings (`[]` for none) is mode list; the object
    /// `{"mode": "all", "denied": [...]}` (the list optional, the same
    /// strings) is mode all. The block may still carry rung 5's retired key
    /// `allowed` (memo D62's start-game-enabled quests that crossed): it is
    /// read and ignored, whatever it holds, and [`SharedQuests::retired`]
    /// says so. The error names what is wrong: a denied entry that is not
    /// such a string, a quest named twice, a mode other than "all", a field
    /// other than the three, or a block of another shape.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let block: serde_json::Value = serde_json::from_str(text).map_err(|e| SettingsError(format!("sharedQuests: {e}")))?;
        match block {
            serde_json::Value::Array(entries) => Ok(Self { mode: Mode::List, listed: parse_quests(&entries, "sharedQuests")?, ..Self::default() }),
            serde_json::Value::Object(fields) => {
                if let Some(key) = fields.keys().find(|k| *k != "mode" && *k != "denied" && *k != "allowed") {
                    return Err(SettingsError(format!("sharedQuests: {key:?} is no field of the mode block (mode, denied, and the retired allowed)")));
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
                // rung 5 (read 4): a recognized key, retired; its entries are
                // counted for the load's line and never read as quests
                let retired_allowed = fields.get("allowed").map(|v| v.as_array().map_or(0, Vec::len));
                Ok(Self { mode: Mode::All, denied, retired_allowed, ..Self::default() })
            }
            other => Err(SettingsError(format!("sharedQuests: {other} is neither a list of quests nor {{\"mode\": \"all\", \"denied\": [...]}}"))),
        }
    }

    /// The sharing mode.
    #[must_use]
    pub fn mode(&self) -> Mode {
        self.mode
    }

    /// The line the core logs once at load when the block still carries the
    /// retired `allowed` key (rung 5, read 4; the deploy files drop it in a
    /// later thuum commit), None when it does not.
    #[must_use]
    pub fn retired(&self) -> Option<String> {
        self.retired_allowed.map(|n| {
            format!(
                "sharedQuests.allowed is retired (thuum ADR-068 rung 5): its {n} quest(s) are ignored; a quest the game starts by itself crosses like any other, its stage 0 the start word's"
            )
        })
    }

    /// Compose the player class in (rung 5, wire-rules quest_scope): the
    /// quests the scope's derivation marks Player, each once, replacing any
    /// composed before. The core calls it at load, and only while the scope
    /// acts (`questScope` "derived"), so "off" leaves the setting as written.
    pub fn set_player(&mut self, quests: &[FormDesc]) {
        self.player.clear();
        for q in quests {
            if !self.is_player(q.short_id, &q.file) {
                self.player.push(q.clone());
            }
        }
    }

    /// Whether the quest `short_id` in `file` is of the composed player class.
    #[must_use]
    pub fn is_player(&self, short_id: u32, file: &str) -> bool {
        self.player.iter().any(|p| p.is(short_id, file))
    }

    /// Whether the quest `short_id` in `file` is the world's: listed in mode
    /// list, not denied in mode all, and in either mode not of the player
    /// class.
    #[must_use]
    pub fn is_shared(&self, short_id: u32, file: &str) -> bool {
        let by_mode = match self.mode {
            Mode::List => self.listed.iter().any(|l| l.is(short_id, file)),
            Mode::All => !self.is_denied(short_id, file),
        };
        by_mode && !self.is_player(short_id, file)
    }

    /// Whether mode all denies the quest `short_id` in `file`.
    #[must_use]
    pub fn is_denied(&self, short_id: u32, file: &str) -> bool {
        self.denied.iter().any(|d| d.is(short_id, file))
    }

    /// What the login names, in the settings' order: in mode list the
    /// shared quests (what a client reports), the player class left out; in
    /// mode all the denied quests and then the player class's not denied
    /// already, each once (what a client never reports: denied ∪ player).
    #[must_use]
    pub fn listed(&self) -> Vec<FormDesc> {
        match self.mode {
            Mode::List => self.listed.iter().filter(|l| !self.is_player(l.short_id, &l.file)).cloned().collect(),
            Mode::All => self.denied.iter().chain(self.player.iter().filter(|p| !self.is_denied(p.short_id, &p.file))).cloned().collect(),
        }
    }

    /// Judge a report's quest: the world's, or refused, the mode's own
    /// refusal first (the setting's word) and then the player class's.
    pub fn check(&self, quest: &FormDesc) -> Result<(), Refusal> {
        match self.mode {
            Mode::List if !self.listed.iter().any(|l| l.is(quest.short_id, &quest.file)) => return Err(Refusal::Unlisted),
            Mode::All if self.is_denied(quest.short_id, &quest.file) => return Err(Refusal::Denied),
            Mode::List | Mode::All => {}
        }
        if self.is_player(quest.short_id, &quest.file) {
            return Err(Refusal::Player);
        }
        Ok(())
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
    fn the_retired_allowlist_is_read_said_once_and_ignored() {
        // rung 5 (read 4): the deploy's block still names memo D62's four
        // allowed quests (MS13 0x39645 among them, Start Game Enabled); the
        // key is recognized, its quests change nothing, and the load's line
        // says how many it named
        let all = SharedQuests::from_json(r#"{"mode": "all", "denied": ["04e50d:Skyrim.esm"], "allowed": ["039645:Skyrim.esm", "0e3ea5:Skyrim.esm"]}"#);
        assert!(all.as_ref().is_ok_and(|a| a.mode() == Mode::All && a.listed() == [quest(0x4e50d)] && a.check(&quest(0x39645)) == Ok(()) && a.is_shared(0x39645, "Skyrim.esm")));
        assert_eq!(
            all.as_ref().ok().and_then(SharedQuests::retired).as_deref(),
            Some("sharedQuests.allowed is retired (thuum ADR-068 rung 5): its 2 quest(s) are ignored; a quest the game starts by itself crosses like any other, its stage 0 the start word's")
        );
        // whatever it holds: a quest also denied (memo D62's old conflict), a
        // malformed entry, a value of another shape; none refuses the block
        let both = SharedQuests::from_json(r#"{"mode": "all", "denied": ["039645:Skyrim.esm"], "allowed": ["39645:Skyrim.esm"]}"#);
        assert!(both.as_ref().is_ok_and(|a| a.check(&quest(0x39645)) == Err(Refusal::Denied)));
        assert!(SharedQuests::from_json(r#"{"mode": "all", "allowed": ["039645"]}"#).is_ok_and(|a| a.retired().is_some() && a.listed().is_empty()));
        assert!(SharedQuests::from_json(r#"{"mode": "all", "allowed": "039645:Skyrim.esm"}"#).is_ok_and(|a| a.retired().is_some_and(|line| line.contains("its 0 quest(s)"))));
        // no key, no line; mode list has none to carry
        assert!(SharedQuests::from_json(r#"{"mode": "all", "denied": ["04e50d:Skyrim.esm"]}"#).is_ok_and(|a| a.retired().is_none()));
        assert!(SharedQuests::from_json(r#"["039645:Skyrim.esm"]"#).is_ok_and(|l| l.retired().is_none()));
        // a stray field still refuses the block, the retired key's misspelling among them
        assert!(SharedQuests::from_json(r#"{"mode": "all", "allow": ["039645:Skyrim.esm"]}"#).is_err());
    }

    #[test]
    fn the_player_class_is_never_the_worlds_and_the_login_names_it_in_mode_all() {
        // rung 5 (D-Q1's derivation): PlayerWerewolfQuest 0x2ba16 and
        // PlayerVampireQuest 0xeafd5 marked Player (docs/research/
        // rung5-quests-2026-11-09.md section 7, H-QS1 and H-QS2), MQ102
        // 0x4e50d denied, MS13 0x39645 the world's
        let mut all = SharedQuests::from_json(r#"{"mode": "all", "denied": ["04e50d:Skyrim.esm"]}"#).unwrap_or_default();
        all.set_player(&[quest(0x2ba16), quest(0xeafd5), quest(0x2ba16)]);
        assert_eq!(all.listed(), vec![quest(0x4e50d), quest(0x2ba16), quest(0xeafd5)]);
        assert_eq!(all.check(&quest(0x2ba16)), Err(Refusal::Player));
        assert_eq!(all.check(&FormDesc { short_id: 0xeafd5, file: "skyrim.esm".to_owned() }), Err(Refusal::Player));
        assert_eq!(all.check(&quest(0x39645)), Ok(()));
        assert!(!all.is_shared(0xeafd5, "Skyrim.esm") && all.is_shared(0x39645, "Skyrim.esm"));
        // the mode's own refusal first: a quest both denied and marked is denied, named once in the list
        all.set_player(&[quest(0x4e50d), quest(0x2ba16)]);
        assert_eq!(all.check(&quest(0x4e50d)), Err(Refusal::Denied));
        assert_eq!(all.listed(), vec![quest(0x4e50d), quest(0x2ba16)]);
        // an older record's stage of a marked quest stays unsent
        assert_eq!(all.at_login(&[at(0x2ba16, 10), at(0x39645, 10)]), vec![at(0x39645, 10)]);
        // mode list: a marked quest on the list is refused and named nowhere
        let mut list = SharedQuests::from_json(r#"["02ba16:Skyrim.esm", "039645:Skyrim.esm"]"#).unwrap_or_default();
        list.set_player(&[quest(0x2ba16)]);
        assert_eq!(list.listed(), vec![quest(0x39645)]);
        assert_eq!(list.check(&quest(0x2ba16)), Err(Refusal::Player));
        assert_eq!(list.check(&quest(0xeafd5)), Err(Refusal::Unlisted));
        // nothing composed (questScope "off"): the setting as written
        list.set_player(&[]);
        assert_eq!(list.check(&quest(0x2ba16)), Ok(()));
        assert_eq!((Refusal::Unlisted.name(), Refusal::Denied.name(), Refusal::Player.name()), ("E_QUEST_UNLISTED", "E_QUEST_DENIED", "E_QUEST_PLAYER"));
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
