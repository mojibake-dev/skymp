//! A quest's run state (thuum ADR-068 rung 5; docs/research/
//! rung5-quests-2026-11-09.md section 1(b), read 5). A quest is nobody's
//! (D-F1 C): every seat runs it, so a start or a stop one seat's game makes
//! is reported as QuestStage 57's word (`word` 1 a start, 2 a stop) and
//! becomes the world's: the server records the quest running or stopped on
//! the world's journal (PartOne::WorldJournal's questRuns), relays the word
//! to every other seat and serves the record at a login, the starts ahead of
//! the stages and the stops behind them. A stage-0 report is the start
//! word's, never a stage ([`Report::of`]): the engine's own start of a quest
//! the game starts by itself arrives that way, and memo D62's rule for it is
//! retired. Since 2026-11-10 so is a report of one of the quest's start-up
//! stages (QUST INDX), which the engine sets as it starts the quest. A word
//! equal to the record is folded (every seat that runs the quest reports the
//! same start), so nothing is written or relayed for it.
//!
//! D-Q3 A at the login: a quest recorded stopped whose stages the login
//! replays is served its start too, ahead of them, so per quest the replay
//! reads as its history, start, stages, stop ([`QuestRuns::at_login`]).
//!
//! Two controllers restart themselves in bursts on every seat
//! (DLC1VCController 0x0201a428 thirty-two times about a minute apart, a
//! layer quest at 0x30002859 about 1.3 times a second; read 5), so a seat
//! whose own words for one quest flip more than [`RUN_FLIPS`] times inside
//! [`RUN_WINDOW_MS`] is the quest's churn (`E_QUEST_CHURN`, the server's
//! backstop behind the client's own hold): the quest is held out of the
//! record for the server's run, its entry dropped ([`without`]) and its
//! later words refused, so no replay starts or stops it. Both numbers are
//! UNMEASURED: x-quest-runs v1 sets them. The record is bounded at
//! [`RUN_RECORD_CAP`] quests (`E_QUEST_RUNS_FULL`, E_QUEST_FULL's sibling)
//! and the churn's own counters at [`RUN_TRACK_CAP`] pairs (rule 12's
//! resource bounds: the seats' reports feed both). R0 record, R1 feed.
//!
//! D-Q4 C (Eli, 2026-11-10, "Rung 5's stage-eve words"): a run word counts
//! as the world's only when a script's call with a WORLD cause makes it, not
//! every engine start and stop (the template save's load, start-game
//! housekeeping, the Story Manager's per-seat starts). The seat's platform
//! reads the root of the script stack that called Quest.Start, Quest.Stop or
//! SetStage and carries its verdict onto the stages and words the call sets;
//! the seat reports a run word only with a world cause and names the cause
//! on every report (QuestStage `cause`, since SCHEMA_VERSION 43: the seat's
//! own word, R2). Here a run word, or a report judged a start, whose cause
//! is the seat's own is refused ([`Code::Cause`], `E_QUEST_CAUSE`): the
//! seat cannot tell a start-up stage from any other, the record's INDX
//! flags can. And a quest a Story Manager quest node names (SMQN NNAM, the
//! core's read of the game files; CommonLibSSE-NG BGSStoryManagerQuestNode.h
//! keeps them as `quests`, NNAM) keeps its runs per seat, start and stop,
//! under the setting `questStoryInstances` "seat" (the default, PENDING
//! ELI'S WORD; [`Code::Instance`], `E_QUEST_INSTANCE`), since a relayed stop
//! would cut another seat's own instance; its stages still cross. "world"
//! judges such a quest by its cause like any other.

use std::collections::{BTreeSet, HashMap, VecDeque};

use crate::clock::SettingsError;
use crate::forms::FormDesc;
use crate::quest_stages::{SharedQuests, StageDone};

/// How many times one seat's words for one quest may flip (a start after
/// its own stop, or a stop after its own start) inside [`RUN_WINDOW_MS`]
/// before the quest is its churn. UNMEASURED (x-quest-runs v1 reads each
/// quest's flips per minute per seat on an idle seat).
pub const RUN_FLIPS: usize = 6;

/// The churn's window, ms. UNMEASURED, as [`RUN_FLIPS`].
pub const RUN_WINDOW_MS: u64 = 60_000;

/// How many quests the world's run record holds at most: a bound, not a
/// measurement (the quests a world's playthroughs start, a few hundred of
/// Skyrim.esm's 1811). A word of a quest the record lacks is refused past it
/// (`E_QUEST_RUNS_FULL`) and the record stays.
pub const RUN_RECORD_CAP: usize = 2048;

/// How many (quest, seat) pairs the churn counter keeps; past it the pair
/// flipped longest ago is forgotten (its next word counts from none).
pub const RUN_TRACK_CAP: usize = 4096;

/// A run word.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Word {
    /// The quest started (Quest.Start, the engine's questStart event, or a
    /// report of stage 0 or of a start-up stage).
    Start,
    /// The quest stopped (Quest.Stop, the questStop event).
    Stop,
}

impl Word {
    /// The wire's value (QuestStage `word`): 1 a start, 2 a stop.
    #[must_use]
    pub fn wire(self) -> u8 {
        match self {
            Word::Start => 1,
            Word::Stop => 2,
        }
    }

    /// The run state the word leaves: running after a start.
    #[must_use]
    pub fn running(self) -> bool {
        self == Word::Start
    }
}

/// What a QuestStage report asks of the record.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Report {
    /// A run word (this module's record).
    Word(Word),
    /// A stage above 0 that is no start-up stage (wire-rules quest_stages'
    /// record).
    Stage(u16),
}

impl Report {
    /// A report's `word` (none for a stage report), `stage` and `start_up`,
    /// whether the stage is one the quest's start runs (the core reads it
    /// from the record, QUST INDX: stage 0 and the start-up stages, the
    /// stages a start word's `wordStages` name): word 1 a start, word 2 a
    /// stop, no word and stage 0 a start (read 5: the stage-0 report is the
    /// start word's), no word and a start-up stage a start too, no word and
    /// any other stage that stage; none for a word of another value, which
    /// the wire refuses first (wire-validate check_quest_stage).
    ///
    /// Read 5 generalized (2026-11-10): the engine sets a quest's start-up
    /// stages as it starts the quest (FreeformHeartwoodMill 0xe3ea5's
    /// flagged stage 10 read done on both seats by the quest's own start,
    /// x-quest-rumor v1), so a report of one is the start's, never a world
    /// stage, as canon's retired QuestStageIsEngineStart held for stage 0
    /// and the start-up stages alike. Judged a stage, a seat's report of
    /// e3ea5's stage 10 would be recorded and relayed as the world's,
    /// against b-quest-rumor's HeartwoodMill claim (no "set e3ea5:Skyrim.esm
    /// stage 10", kept by its v2).
    #[must_use]
    pub fn of(word: Option<u8>, stage: u16, start_up: bool) -> Option<Self> {
        match word {
            Some(1) => Some(Report::Word(Word::Start)),
            Some(2) => Some(Report::Word(Word::Stop)),
            Some(_) => None,
            None if stage == 0 || start_up => Some(Report::Word(Word::Start)),
            None => Some(Report::Stage(stage)),
        }
    }
}

/// What made a report (QuestStage `cause`, since SCHEMA_VERSION 43; D-Q4
/// C): the verdict of the script stack that called Quest.Start, Quest.Stop
/// or SetStage, as the seat's platform read its root frame. The seat's own
/// word, unvalidated (R2): judged here, logged by the core, never trusted
/// past the report it rides.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Cause {
    /// A world cause (the wire's 1): a call from a dialogue, scene, package
    /// or perk fragment, an interaction event, or a stage fragment whose
    /// stage a world cause or a timer set.
    World,
    /// The seat's own (the wire's 2): lifecycle, cell load, the Story
    /// Manager's events, a direct timer, a magic effect, the UI, an engine
    /// start's start-up fragment, a stage set inside such a chain, or no
    /// script call at all.
    Seat,
}

impl Cause {
    /// The wire's value: none for an older client's report, which names no
    /// cause, and for a value the wire refuses first (wire-validate
    /// check_quest_stage).
    #[must_use]
    pub fn of(wire: Option<u8>) -> Option<Self> {
        match wire {
            Some(1) => Some(Cause::World),
            Some(2) => Some(Cause::Seat),
            _ => None,
        }
    }

    /// The word the log names.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Cause::World => "world",
            Cause::Seat => "seat",
        }
    }
}

/// Whether a quest a Story Manager quest node names keeps its runs per seat
/// (`questStoryInstances`, PENDING ELI'S WORD): a one-line flip of the
/// server's settings either way.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum StoryInstances {
    /// "seat", the default (the read's recommendation): every run word of
    /// such a quest is refused (`E_QUEST_INSTANCE`) and the login serves
    /// none, so a relayed stop never cuts another seat's own instance (a WI,
    /// WE or ADIA quest, about 35 CLOC dungeon quests, DA10, the CW
    /// missions, the Favors); its stages still cross.
    #[default]
    Seat,
    /// "world": such a quest is judged by its cause like any other.
    World,
}

impl StoryInstances {
    /// The setting from its JSON text, the string "seat" or "world"; the
    /// core passes `"seat"` for a key the file lacks. The error names what
    /// is wrong.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        match serde_json::from_str::<serde_json::Value>(text).map_err(|e| SettingsError(format!("questStoryInstances: {e}")))? {
            serde_json::Value::String(m) if m == "seat" => Ok(StoryInstances::Seat),
            serde_json::Value::String(m) if m == "world" => Ok(StoryInstances::World),
            other => Err(SettingsError(format!("questStoryInstances: {other} is neither \"seat\" nor \"world\""))),
        }
    }

    /// The setting's word, as the server's settings spell it.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            StoryInstances::Seat => "seat",
            StoryInstances::World => "world",
        }
    }
}

/// One quest's run on the world's record.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QuestRun {
    /// The quest, as the change form names it.
    pub quest: FormDesc,
    /// Recorded running (its last word a start).
    pub running: bool,
    /// How many words the record took for it, 1 at its first: the record's
    /// sequence for the quest, which labState shows.
    pub seq: u32,
}

/// Why a word is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// The quest is a seat's churn, held out of the record for the run.
    Churn,
    /// The record holds RUN_RECORD_CAP quests and not this one.
    Full,
    /// D-Q4 C: a run word, or a report judged a start, whose cause is the
    /// seat's own.
    Cause,
    /// A Story Manager instance's run word under `questStoryInstances`
    /// "seat": each seat's own.
    Instance,
}

impl Code {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::Churn => "E_QUEST_CHURN",
            Code::Full => "E_QUEST_RUNS_FULL",
            Code::Cause => "E_QUEST_CAUSE",
            Code::Instance => "E_QUEST_INSTANCE",
        }
    }
}

/// The rule's word on a run word.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    /// The word moved the record: merge it ([`merge`]) and relay it.
    Recorded,
    /// The record holds the same run state: nothing is written or relayed.
    Folded,
    /// The quest is held, a seat's churn: nothing is written or relayed.
    /// `first` the moment this word made it so: the core drops the quest's
    /// entry ([`without`]) and logs `E_QUEST_CHURN`; false for every later
    /// word of a held quest.
    Held {
        /// This word made the quest held.
        first: bool,
    },
    /// Refused, with why: by the record's bound (`E_QUEST_RUNS_FULL`), by
    /// its cause (`E_QUEST_CAUSE`) or as a Story Manager instance
    /// (`E_QUEST_INSTANCE`). Nothing is written, relayed or counted.
    Refuse(Code, String),
}

/// One seat's words for one quest, as the churn counter reads them.
#[derive(Debug, Clone)]
struct Track {
    /// The seat's last word for the quest.
    last: Word,
    /// Its flips' times inside the window, oldest first, at most RUN_FLIPS
    /// plus one.
    flips: VecDeque<u64>,
    /// When the seat last sent a word for the quest, for the cap's eviction.
    touched_ms: u64,
}

/// The churn counter and the quests held for the run, beside the Story
/// Manager's quests and the setting that says whether they are each seat's
/// own. Runtime only: a restart forgets the first two, and a held quest's
/// entry is gone from the record by then ([`without`]); the core sets the
/// last two from the settings and the game files at every start.
#[derive(Debug, Default)]
pub struct QuestRuns {
    tracks: HashMap<(FormDesc, u32), Track>,
    held: BTreeSet<(u32, String)>,
    story: BTreeSet<(u32, String)>,
    instances: StoryInstances,
}

/// A held quest's key: its id and its plugin without case, as FormDesc's
/// `is` compares.
fn held_key(quest: &FormDesc) -> (u32, String) {
    (quest.short_id, quest.file.to_ascii_lowercase())
}

impl QuestRuns {
    /// Whether `quest` is held out of the record for the run.
    #[must_use]
    pub fn held(&self, quest: &FormDesc) -> bool {
        self.held.contains(&held_key(quest))
    }

    /// How many quests are held.
    #[must_use]
    pub fn held_count(&self) -> usize {
        self.held.len()
    }

    /// The Story Manager's quests (every quest an SMQN node of the game
    /// files names, the core's read) and `questStoryInstances`, both
    /// replaced whole.
    pub fn set_story(&mut self, quests: &[FormDesc], instances: StoryInstances) {
        self.story = quests.iter().map(held_key).collect();
        self.instances = instances;
    }

    /// Whether a Story Manager quest node names `quest`, whatever the
    /// setting says.
    #[must_use]
    pub fn story_named(&self, quest: &FormDesc) -> bool {
        self.story.contains(&held_key(quest))
    }

    /// Whether `quest` keeps its runs per seat: a Story Manager quest node
    /// names it and `questStoryInstances` is "seat".
    #[must_use]
    pub fn story_instance(&self, quest: &FormDesc) -> bool {
        self.instances == StoryInstances::Seat && self.story_named(quest)
    }

    /// How many quests the Story Manager's nodes name.
    #[must_use]
    pub fn story_count(&self) -> usize {
        self.story.len()
    }

    /// The setting in force.
    #[must_use]
    pub fn instances(&self) -> StoryInstances {
        self.instances
    }

    /// One run word of `quest` with the report's `cause` (none from an
    /// older client): a seat-caused word is refused first (D-Q4 C,
    /// `E_QUEST_CAUSE`), touching neither the record nor the churn counter;
    /// any other is [`QuestRuns::judge`]d.
    pub fn judge_caused(&mut self, record: &[QuestRun], quest: &FormDesc, word: Word, cause: Option<Cause>, seat: u32, now_ms: u64) -> Verdict {
        if cause == Some(Cause::Seat) {
            let what = if word == Word::Start { "start" } else { "stop" };
            return Verdict::Refuse(Code::Cause, format!("the {what} of {:x}:{} has the seat's own cause: a run word is the world's only when a script's call with a world cause makes it (D-Q4 C)", quest.short_id, quest.file));
        }
        self.judge(record, quest, word, seat, now_ms)
    }

    /// One run word of `quest` from `seat` (the reporter's actor) at
    /// `now_ms` on a monotonic clock, against the world's `record`: a Story
    /// Manager instance's word is refused under "seat" (`E_QUEST_INSTANCE`),
    /// touching neither the record nor the churn counter; a held quest's
    /// word is held; then the seat's flip, which may make the quest held;
    /// then the fold, and the record's bound for a quest it lacks.
    pub fn judge(&mut self, record: &[QuestRun], quest: &FormDesc, word: Word, seat: u32, now_ms: u64) -> Verdict {
        if self.story_instance(quest) {
            return Verdict::Refuse(Code::Instance, format!("{:x}:{} is a quest a Story Manager quest node names: its runs are each seat's own under questStoryInstances \"seat\", its stages still cross", quest.short_id, quest.file));
        }
        if self.held(quest) {
            return Verdict::Held { first: false };
        }
        if self.flipped_past_the_bound(quest, word, seat, now_ms) {
            self.held.insert(held_key(quest));
            self.tracks.retain(|(q, _), _| q.short_id != quest.short_id || !q.file.eq_ignore_ascii_case(&quest.file));
            return Verdict::Held { first: true };
        }
        match record.iter().find(|r| r.quest.is(quest.short_id, &quest.file)) {
            Some(r) if r.running == word.running() => Verdict::Folded,
            Some(_) => Verdict::Recorded,
            None if record.len() >= RUN_RECORD_CAP => Verdict::Refuse(Code::Full, format!("the world's run record holds its cap of {RUN_RECORD_CAP} quests, {:x}:{} not among them", quest.short_id, quest.file)),
            None => Verdict::Recorded,
        }
    }

    /// Count the seat's flip, if the word is one, and say whether its flips
    /// inside the window passed [`RUN_FLIPS`].
    fn flipped_past_the_bound(&mut self, quest: &FormDesc, word: Word, seat: u32, now_ms: u64) -> bool {
        let key = (quest.clone(), seat);
        if !self.tracks.contains_key(&key) && self.tracks.len() >= RUN_TRACK_CAP {
            let oldest = self.tracks.iter().min_by_key(|(_, t)| t.touched_ms).map(|(k, _)| k.clone());
            if let Some(oldest) = oldest {
                self.tracks.remove(&oldest);
            }
        }
        let track = self.tracks.entry(key).or_insert_with(|| Track { last: word, flips: VecDeque::new(), touched_ms: now_ms });
        track.touched_ms = now_ms;
        if track.last != word {
            track.last = word;
            track.flips.push_back(now_ms);
        }
        let since = now_ms.saturating_sub(RUN_WINDOW_MS);
        while track.flips.front().is_some_and(|&t| t < since) {
            track.flips.pop_front();
        }
        track.flips.len() > RUN_FLIPS
    }

    /// `seat` left: its counters go (a reconnect counts from none).
    pub fn gone(&mut self, seat: u32) {
        self.tracks.retain(|(_, s), _| *s != seat);
    }

    /// What the login sends around the stages, in the record's order: the
    /// quests recorded running (served start words, ahead of the stages)
    /// and those recorded stopped (served stop words, behind them), each
    /// still the world's by `shared` (a denied or player-class quest, or one
    /// a mode list no longer names, stays unsent), not held, and no Story
    /// Manager instance under "seat" (a run recorded before, or under
    /// "world", stays in the record and goes unsent while the setting says
    /// "seat").
    ///
    /// D-Q3 A (a served replay's fragments are the world's history and never
    /// run again on a seat): a quest recorded stopped whose stages the login
    /// replays (`stages`, the world's stage record, of which
    /// SharedQuests::at_login replays every stage of a quest still the
    /// world's) is served a start word too, among the starts. A served stage
    /// of a quest not running on a seat makes the seat start the quest
    /// first, and that start runs the fragments of the quest's start-up
    /// stages unless a start word named them; the served start names them
    /// (the core stamps its `wordStages`, as on every start it sends), so
    /// per quest the login reads as its history: its start, its stages
    /// ascending, its stop. A quest recorded stopped with no stage to replay
    /// gets its stop alone, as before.
    #[must_use]
    pub fn at_login<'a>(&self, record: &'a [QuestRun], shared: &SharedQuests, stages: &[StageDone]) -> (Vec<&'a QuestRun>, Vec<&'a QuestRun>) {
        let sent = |r: &&QuestRun| shared.is_shared(r.quest.short_id, &r.quest.file) && !self.held(&r.quest) && !self.story_instance(&r.quest);
        let staged: BTreeSet<(u32, String)> = stages.iter().filter(|s| shared.is_shared(s.quest.short_id, &s.quest.file)).map(|s| held_key(&s.quest)).collect();
        let starts = record.iter().filter(sent).filter(|r| r.running || staged.contains(&held_key(&r.quest))).collect();
        let stops = record.iter().filter(sent).filter(|r| !r.running).collect();
        (starts, stops)
    }
}

/// The record after a recorded word: the quest's run set and its sequence
/// raised, or the quest added last at sequence 1. A quest the record lacks
/// is not added past RUN_RECORD_CAP (the judgment refuses it first).
#[must_use]
pub fn merge(record: &[QuestRun], quest: &FormDesc, word: Word) -> Vec<QuestRun> {
    let mut out = record.to_vec();
    let room = out.len() < RUN_RECORD_CAP;
    match out.iter_mut().find(|r| r.quest.is(quest.short_id, &quest.file)) {
        Some(r) => {
            r.running = word.running();
            r.seq = r.seq.saturating_add(1);
        }
        None if room => out.push(QuestRun { quest: quest.clone(), running: word.running(), seq: 1 }),
        None => {}
    }
    out
}

/// The record without `quest`: a held quest's entry leaves it, so no replay
/// starts or stops it.
#[must_use]
pub fn without(record: &[QuestRun], quest: &FormDesc) -> Vec<QuestRun> {
    record.iter().filter(|r| !r.quest.is(quest.short_id, &quest.file)).cloned().collect()
}

#[cfg(test)]
#[allow(clippy::arithmetic_side_effects, clippy::panic)] // a test's clock and failure messages, never a packet's path
mod tests {
    use super::*;

    // the plan's x-quest-words quest 0x10d3bb (a script-started quest),
    // the vampire castle's controller (Dawnguard.esm 0x1a428, read 5) and
    // two seats
    const LETTER: u32 = 0x10d3bb;
    const CONTROLLER: u32 = 0x1a428;
    const C1: u32 = 0xff00_0000;
    const C2: u32 = 0xff00_0001;

    fn quest(short_id: u32) -> FormDesc {
        FormDesc { short_id, file: "Skyrim.esm".to_owned() }
    }

    #[test]
    fn a_stage_0_report_is_a_start_and_the_words_read_as_the_wire_spells_them() {
        assert_eq!(Report::of(None, 0, false), Some(Report::Word(Word::Start)));
        assert_eq!(Report::of(Some(1), 0, false), Some(Report::Word(Word::Start)));
        assert_eq!(Report::of(Some(2), 0, false), Some(Report::Word(Word::Stop)));
        assert_eq!(Report::of(None, 10, false), Some(Report::Stage(10)));
        assert_eq!(Report::of(Some(3), 0, false), None);
        assert_eq!((Word::Start.wire(), Word::Stop.wire()), (1, 2));
        assert!(Word::Start.running() && !Word::Stop.running());
        assert_eq!((Code::Churn.name(), Code::Full.name()), ("E_QUEST_CHURN", "E_QUEST_RUNS_FULL"));
    }

    #[test]
    fn a_start_up_stage_report_is_a_start_and_a_plain_stage_stays_a_stage() {
        // FreeformHeartwoodMill 0xe3ea5, b-quest-rumor's claim (Skyrim.esm,
        // lab/esm.py on sky-srv, 2026-11-10): INDX 10 flagged a start-up
        // stage, 20 unflagged, no stage 0
        const MILL: u32 = 0xe3ea5;
        assert_eq!(Report::of(None, 10, true), Some(Report::Word(Word::Start)));
        assert_eq!(Report::of(None, 20, false), Some(Report::Stage(20)));
        // stage 0 keeps its rule either way, and a word is its own whatever
        // the stage
        assert_eq!((Report::of(None, 0, false), Report::of(None, 0, true)), (Some(Report::Word(Word::Start)), Some(Report::Word(Word::Start))));
        assert_eq!((Report::of(Some(2), 0, true), Report::of(Some(3), 0, true)), (Some(Report::Word(Word::Stop)), None));
        // as the core judges it: one seat's flagged stage records the start
        // once, and the other seat's report of the same stage folds
        let Some(Report::Word(word)) = Report::of(None, 10, true) else { panic!("a start-up stage report is no word") };
        let mut runs = QuestRuns::default();
        let mill = quest(MILL);
        assert_eq!(runs.judge(&[], &mill, word, C1, 0), Verdict::Recorded);
        let record = merge(&[], &mill, word);
        assert_eq!(record, vec![QuestRun { quest: mill.clone(), running: true, seq: 1 }]);
        assert_eq!(runs.judge(&record, &mill, word, C2, 10), Verdict::Folded);
    }

    #[test]
    fn a_start_is_recorded_once_and_a_stop_after_it() {
        let mut runs = QuestRuns::default();
        let q = quest(LETTER);
        assert_eq!(runs.judge(&[], &q, Word::Start, C1, 0), Verdict::Recorded);
        let record = merge(&[], &q, Word::Start);
        assert_eq!(record, vec![QuestRun { quest: q.clone(), running: true, seq: 1 }]);
        // the other seat's start of the running quest folds, and so does the
        // first seat's again
        assert_eq!(runs.judge(&record, &q, Word::Start, C2, 10), Verdict::Folded);
        assert_eq!(runs.judge(&record, &q, Word::Start, C1, 20), Verdict::Folded);
        assert_eq!(runs.judge(&record, &q, Word::Stop, C1, 30), Verdict::Recorded);
        let record = merge(&record, &q, Word::Stop);
        assert_eq!(record, vec![QuestRun { quest: q.clone(), running: false, seq: 2 }]);
        // a stop of a stopped quest folds; the plugin compares without case
        assert_eq!(runs.judge(&record, &FormDesc { short_id: LETTER, file: "skyrim.esm".to_owned() }, Word::Stop, C2, 40), Verdict::Folded);
        // a stop of a quest the record lacks is recorded stopped
        assert_eq!(runs.judge(&record, &quest(0x74a83), Word::Stop, C2, 50), Verdict::Recorded);
        assert_eq!(merge(&record, &quest(0x74a83), Word::Stop).last().map(|r| (r.running, r.seq)), Some((false, 1)));
    }

    #[test]
    fn a_seats_flips_past_the_bound_hold_the_quest_for_the_run() {
        let mut runs = QuestRuns::default();
        let q = FormDesc { short_id: CONTROLLER, file: "Dawnguard.esm".to_owned() };
        let mut record = Vec::new();
        // the controller's burst on c1: start, stop, start... a flip each
        // after the first word; RUN_FLIPS flips inside the window pass
        let mut word = Word::Start;
        let mut t = 0;
        for _ in 0..=RUN_FLIPS {
            let v = runs.judge(&record, &q, word, C1, t);
            assert!(matches!(v, Verdict::Recorded | Verdict::Folded), "{v:?} at {t}");
            if v == Verdict::Recorded {
                record = merge(&record, &q, word);
            }
            word = if word == Word::Start { Word::Stop } else { Word::Start };
            t += 1_000;
        }
        // the seventh flip holds it: the record drops it, later words are held
        assert_eq!(runs.judge(&record, &q, word, C1, t), Verdict::Held { first: true });
        assert!(runs.held(&q) && runs.held(&FormDesc { short_id: CONTROLLER, file: "DAWNGUARD.ESM".to_owned() }));
        assert_eq!(runs.held_count(), 1);
        let record = without(&record, &q);
        assert!(record.is_empty());
        assert_eq!(runs.judge(&record, &q, Word::Start, C2, t + 1), Verdict::Held { first: false });
        // the login never names a held quest
        let all = SharedQuests::from_json(r#"{"mode": "all"}"#).unwrap_or_default();
        let stale = merge(&[], &q, Word::Start);
        assert!(runs.at_login(&stale, &all, &[]).0.is_empty());
    }

    #[test]
    fn flips_outside_the_window_and_on_two_seats_count_apart() {
        let mut runs = QuestRuns::default();
        let q = quest(LETTER);
        // one flip a window apart never piles up
        let mut word = Word::Start;
        for n in 0..(4 * RUN_FLIPS) {
            let t = u64::try_from(n).unwrap_or(0) * (RUN_WINDOW_MS / 2);
            assert!(!matches!(runs.judge(&[], &q, word, C1, t), Verdict::Held { .. }), "{n}");
            word = if word == Word::Start { Word::Stop } else { Word::Start };
        }
        // two seats each under the bound inside one window: no churn
        let mut runs = QuestRuns::default();
        for seat in [C1, C2] {
            let mut word = Word::Start;
            for n in 0..=RUN_FLIPS {
                assert!(!matches!(runs.judge(&[], &q, word, seat, u64::try_from(n).unwrap_or(0)), Verdict::Held { .. }));
                word = if word == Word::Start { Word::Stop } else { Word::Start };
            }
        }
        // a seat's departure forgets its counter: its flips count from none
        runs.gone(C1);
        assert!(!runs.held(&q));
    }

    #[test]
    fn the_record_stops_at_its_cap_and_the_counter_at_its_own() {
        let mut runs = QuestRuns::default();
        let record: Vec<QuestRun> = (1..=u32::try_from(RUN_RECORD_CAP).unwrap_or(0)).map(|q| QuestRun { quest: quest(q), running: true, seq: 1 }).collect();
        assert_eq!(record.len(), RUN_RECORD_CAP);
        let far = quest(0x0010_0000);
        match runs.judge(&record, &far, Word::Start, C1, 0) {
            Verdict::Refuse(code, why) => assert_eq!((code, why), (Code::Full, format!("the world's run record holds its cap of {RUN_RECORD_CAP} quests, 100000:Skyrim.esm not among them"))),
            other => panic!("not refused: {other:?}"),
        }
        assert_eq!(merge(&record, &far, Word::Start), record);
        // a quest the full record holds still moves
        assert_eq!(runs.judge(&record, &quest(1), Word::Stop, C1, 0), Verdict::Recorded);
        // the counter keeps at most RUN_TRACK_CAP pairs, the oldest forgotten
        let mut runs = QuestRuns::default();
        for n in 0..=u32::try_from(RUN_TRACK_CAP).unwrap_or(0) {
            let _ = runs.judge(&[], &quest(n + 1), Word::Start, C1, u64::from(n));
        }
        assert_eq!(runs.tracks.len(), RUN_TRACK_CAP);
        assert!(!runs.tracks.contains_key(&(quest(1), C1)));
    }

    #[test]
    fn the_login_sends_running_quests_then_stopped_ones_still_shared() {
        let runs = QuestRuns::default();
        let record = vec![
            QuestRun { quest: quest(LETTER), running: true, seq: 1 },
            QuestRun { quest: quest(0x4e50d), running: true, seq: 3 },
            QuestRun { quest: quest(0x74a83), running: false, seq: 2 },
            QuestRun { quest: quest(0x2ba16), running: true, seq: 1 },
        ];
        let mut all = SharedQuests::from_json(r#"{"mode": "all", "denied": ["04e50d:Skyrim.esm"]}"#).unwrap_or_default();
        all.set_player(&[quest(0x2ba16)]);
        let (starts, stops) = runs.at_login(&record, &all, &[]);
        assert_eq!(starts.iter().map(|r| r.quest.short_id).collect::<Vec<_>>(), [LETTER]);
        assert_eq!(stops.iter().map(|r| r.quest.short_id).collect::<Vec<_>>(), [0x74a83]);
        // mode list sends what it lists
        let list = SharedQuests::from_json(r#"["074a83:Skyrim.esm"]"#).unwrap_or_default();
        let (starts, stops) = runs.at_login(&record, &list, &[]);
        assert!(starts.is_empty());
        assert_eq!(stops.len(), 1);
    }

    // D-Q4 C (the read of 2026-11-10, its census of sky-srv's deployed load
    // order, qust-facts.tsv): FreeformRiverwood01 0x74a83 (A Lovely Letter)
    // and CR02 0x25185 are quests SMQN nodes name (FreeformRiverwoodNode
    // 0xb555d, CompanionsAelaNode 0xbfc77), dunEmbershardQST 0xb6fef one no
    // node names; all three Skyrim.esm's
    const LETTER_SM: u32 = 0x74a83;
    const CR02: u32 = 0x25185;
    const EMBERSHARD: u32 = 0xb6fef;

    fn flip(word: Word) -> Word {
        if word == Word::Start { Word::Stop } else { Word::Start }
    }

    fn ids(runs: &[&QuestRun]) -> Vec<u32> {
        runs.iter().map(|r| r.quest.short_id).collect()
    }

    #[test]
    fn a_report_names_its_cause_as_the_wire_spells_it() {
        assert_eq!((Cause::of(Some(1)), Cause::of(Some(2)), Cause::of(None)), (Some(Cause::World), Some(Cause::Seat), None));
        assert_eq!((Cause::of(Some(0)), Cause::of(Some(3)), Cause::of(Some(u8::MAX))), (None, None, None));
        assert_eq!((Cause::World.name(), Cause::Seat.name()), ("world", "seat"));
        assert_eq!((Code::Cause.name(), Code::Instance.name()), ("E_QUEST_CAUSE", "E_QUEST_INSTANCE"));
    }

    #[test]
    fn a_seat_caused_word_is_refused_before_the_record_and_the_churn_and_a_world_one_is_judged() {
        let mut runs = QuestRuns::default();
        let q = quest(EMBERSHARD);
        match runs.judge_caused(&[], &q, Word::Start, Some(Cause::Seat), C1, 0) {
            Verdict::Refuse(code, why) => assert_eq!((code, why.as_str()), (Code::Cause, "the start of b6fef:Skyrim.esm has the seat's own cause: a run word is the world's only when a script's call with a world cause makes it (D-Q4 C)")),
            other => panic!("not refused by its cause: {other:?}"),
        }
        // a world cause is judged as every word was, and so is an older
        // client's report, which names none
        assert_eq!(runs.judge_caused(&[], &q, Word::Start, Some(Cause::World), C1, 0), Verdict::Recorded);
        assert_eq!(runs.judge_caused(&[], &q, Word::Start, None, C2, 0), Verdict::Recorded);
        // a refused word counts no flip: the vampire castle's controller
        // restarting itself from its own timer (a seat-caused word each time)
        // never makes the quest held, so its first world-caused word is
        // recorded
        let controller = FormDesc { short_id: CONTROLLER, file: "Dawnguard.esm".to_owned() };
        let mut word = Word::Start;
        for n in 0..(4 * RUN_FLIPS) {
            let at = u64::try_from(n).unwrap_or(0);
            assert!(matches!(runs.judge_caused(&[], &controller, word, Some(Cause::Seat), C1, at), Verdict::Refuse(Code::Cause, _)), "{n}");
            word = flip(word);
        }
        assert!(!runs.held(&controller));
        assert_eq!(runs.judge_caused(&[], &controller, Word::Start, Some(Cause::World), C1, 100), Verdict::Recorded);
    }

    #[test]
    fn a_story_instance_keeps_its_runs_per_seat_unless_the_setting_says_world() {
        let mut runs = QuestRuns::default();
        assert_eq!((runs.story_count(), runs.instances()), (0, StoryInstances::Seat));
        runs.set_story(&[quest(LETTER_SM), quest(CR02)], StoryInstances::Seat);
        assert_eq!(runs.story_count(), 2);
        assert!(runs.story_named(&quest(LETTER_SM)) && runs.story_instance(&FormDesc { short_id: LETTER_SM, file: "SKYRIM.ESM".to_owned() }));
        assert!(!runs.story_named(&quest(EMBERSHARD)) && !runs.story_instance(&quest(EMBERSHARD)));
        // a world-caused start and stop of an instance: refused, nothing
        // written, relayed or counted
        for word in [Word::Start, Word::Stop] {
            match runs.judge_caused(&[], &quest(LETTER_SM), word, Some(Cause::World), C1, 0) {
                Verdict::Refuse(code, why) => assert_eq!((code, why.as_str()), (Code::Instance, "74a83:Skyrim.esm is a quest a Story Manager quest node names: its runs are each seat's own under questStoryInstances \"seat\", its stages still cross")),
                other => panic!("not refused as an instance: {other:?}"),
            }
        }
        // an older client's word too, and a seat-caused one is named by its
        // cause first
        assert!(matches!(runs.judge(&[], &quest(CR02), Word::Start, C2, 0), Verdict::Refuse(Code::Instance, _)));
        assert!(matches!(runs.judge_caused(&[], &quest(CR02), Word::Start, Some(Cause::Seat), C2, 0), Verdict::Refuse(Code::Cause, _)));
        // a quest no node names crosses
        assert_eq!(runs.judge_caused(&[], &quest(EMBERSHARD), Word::Start, Some(Cause::World), C1, 0), Verdict::Recorded);
        // the login serves no instance's run, one recorded before included
        let all = SharedQuests::from_json(r#"{"mode": "all"}"#).unwrap_or_default();
        let record = vec![
            QuestRun { quest: quest(LETTER_SM), running: true, seq: 1 },
            QuestRun { quest: quest(EMBERSHARD), running: true, seq: 1 },
            QuestRun { quest: quest(CR02), running: false, seq: 2 },
        ];
        let (starts, stops) = runs.at_login(&record, &all, &[]);
        assert_eq!((ids(&starts), ids(&stops)), (vec![EMBERSHARD], vec![]));
        // "world": such a quest is judged by its cause like any other and
        // served at the login
        runs.set_story(&[quest(LETTER_SM), quest(CR02)], StoryInstances::World);
        assert!(runs.story_named(&quest(LETTER_SM)) && !runs.story_instance(&quest(LETTER_SM)));
        assert_eq!(runs.judge_caused(&[], &quest(LETTER_SM), Word::Start, Some(Cause::World), C1, 10), Verdict::Recorded);
        assert!(matches!(runs.judge_caused(&[], &quest(LETTER_SM), Word::Stop, Some(Cause::Seat), C1, 10), Verdict::Refuse(Code::Cause, _)));
        let (starts, stops) = runs.at_login(&record, &all, &[]);
        assert_eq!((ids(&starts), ids(&stops)), (vec![LETTER_SM, EMBERSHARD], vec![CR02]));
    }

    #[test]
    fn the_story_setting_reads_seat_or_world_and_names_what_is_wrong() {
        assert_eq!(StoryInstances::from_json(r#""seat""#).ok(), Some(StoryInstances::Seat));
        assert_eq!(StoryInstances::from_json(r#""world""#).ok(), Some(StoryInstances::World));
        assert_eq!(StoryInstances::default(), StoryInstances::Seat);
        assert_eq!((StoryInstances::Seat.name(), StoryInstances::World.name()), ("seat", "world"));
        for bad in [r#""off""#, "1", "{}", r#"["seat"]"#, "seat"] {
            let why = StoryInstances::from_json(bad).err().map(|e| e.0);
            assert!(why.as_deref().is_some_and(|w| w.starts_with("questStoryInstances: ")), "{bad}: {why:?}");
        }
    }

    // D-Q3 A, the login's start of a stopped quest: MS13 0x39645 and its
    // plain stages 10 and 20 (the C++ QuestStageTest's listed quest; neither
    // flagged a start-up stage, both recorded as stages there) and
    // Embershard's stage 40 (its cause case's)
    const MS13: u32 = 0x39645;

    // the login as the core composes it (ActionListener::SendQuestStages):
    // the served starts, the stages the record replays, the served stops;
    // each its quest, its word (0 for a stage) and its stage
    fn replay(runs: &QuestRuns, record: &[QuestRun], shared: &SharedQuests, stages: &[StageDone]) -> Vec<(u32, u8, u16)> {
        let (starts, stops) = runs.at_login(record, shared, stages);
        let words = |list: &[&QuestRun], word: Word| list.iter().map(|r| (r.quest.short_id, word.wire(), 0)).collect::<Vec<(u32, u8, u16)>>();
        let mut out = words(&starts, Word::Start);
        out.extend(shared.at_login(stages).iter().map(|s| (s.quest.short_id, 0, s.stage)));
        out.extend(words(&stops, Word::Stop));
        out
    }

    #[test]
    fn the_login_starts_a_stopped_quest_ahead_of_the_stages_it_replays() {
        let mut runs = QuestRuns::default();
        runs.set_story(&[quest(LETTER_SM)], StoryInstances::Seat);
        let all = SharedQuests::from_json(r#"{"mode": "all"}"#).unwrap_or_default();
        // Embershard running; MS13 stopped with two stages; the letter quest
        // stopped with none; A Lovely Letter, a Story Manager instance,
        // stopped (a run recorded before) with its stage 10, which crosses
        let record = vec![
            QuestRun { quest: quest(EMBERSHARD), running: true, seq: 1 },
            QuestRun { quest: quest(MS13), running: false, seq: 2 },
            QuestRun { quest: quest(LETTER), running: false, seq: 2 },
            QuestRun { quest: quest(LETTER_SM), running: false, seq: 2 },
        ];
        let at = |short_id: u32, stage: u16| StageDone { quest: quest(short_id), stage };
        // an older record's order: MS13's 20 ahead of its 10
        let stages = vec![at(EMBERSHARD, 40), at(MS13, 20), at(MS13, 10), at(LETTER_SM, 10)];
        // the stopped quest with stages: its start among the starts, its
        // stages ascending, its stop behind them; the stopped quest with none
        // its stop alone; the running quest as before; the instance no word
        assert_eq!(
            replay(&runs, &record, &all, &stages),
            vec![(EMBERSHARD, 1, 0), (MS13, 1, 0), (EMBERSHARD, 0, 40), (MS13, 0, 10), (MS13, 0, 20), (LETTER_SM, 0, 10), (MS13, 2, 0), (LETTER, 2, 0)]
        );
        let (starts, stops) = runs.at_login(&record, &all, &stages);
        assert_eq!((ids(&starts), ids(&stops)), (vec![EMBERSHARD, MS13], vec![MS13, LETTER]));
        // no stage to replay: the login as before, the stopped quests' stops
        // alone
        let (starts, stops) = runs.at_login(&record, &all, &[]);
        assert_eq!((ids(&starts), ids(&stops)), (vec![EMBERSHARD], vec![MS13, LETTER]));
        // a stage names its quest without the plugin's case, as `is` compares
        let spelled = vec![StageDone { quest: FormDesc { short_id: MS13, file: "SKYRIM.ESM".to_owned() }, stage: 10 }];
        assert_eq!(ids(&runs.at_login(&record, &all, &spelled).0), vec![EMBERSHARD, MS13]);
        // a quest the world no longer shares: no word, and its stages unsent
        let denied = SharedQuests::from_json(r#"{"mode": "all", "denied": ["039645:Skyrim.esm"]}"#).unwrap_or_default();
        assert_eq!(replay(&runs, &record, &denied, &stages), vec![(EMBERSHARD, 1, 0), (EMBERSHARD, 0, 40), (LETTER_SM, 0, 10), (LETTER, 2, 0)]);
        // "world": the Story Manager's quest is a quest like any other, so
        // its stopped run with a stage is started ahead of it and stopped
        runs.set_story(&[quest(LETTER_SM)], StoryInstances::World);
        let (starts, stops) = runs.at_login(&record, &all, &stages);
        assert_eq!((ids(&starts), ids(&stops)), (vec![EMBERSHARD, MS13, LETTER_SM], vec![MS13, LETTER, LETTER_SM]));
    }
}
