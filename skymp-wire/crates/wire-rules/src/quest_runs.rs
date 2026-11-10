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
//! retired. A word equal to the record is folded (every seat that runs the
//! quest reports the same start), so nothing is written or relayed for it.
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

use std::collections::{BTreeSet, HashMap, VecDeque};

use crate::forms::FormDesc;
use crate::quest_stages::SharedQuests;

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
    /// stage-0 report).
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
    /// A stage above 0 (wire-rules quest_stages' record).
    Stage(u16),
}

impl Report {
    /// A report's `word` (none for a stage report) and `stage`: word 1 a
    /// start, word 2 a stop, no word and stage 0 a start (read 5: the stage-0
    /// report is the start word's), no word and any other stage that stage;
    /// none for a word of another value, which the wire refuses first
    /// (wire-validate check_quest_stage).
    #[must_use]
    pub fn of(word: Option<u8>, stage: u16) -> Option<Self> {
        match word {
            Some(1) => Some(Report::Word(Word::Start)),
            Some(2) => Some(Report::Word(Word::Stop)),
            Some(_) => None,
            None if stage == 0 => Some(Report::Word(Word::Start)),
            None => Some(Report::Stage(stage)),
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
}

impl Code {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::Churn => "E_QUEST_CHURN",
            Code::Full => "E_QUEST_RUNS_FULL",
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
    /// Refused by the record's bound (`E_QUEST_RUNS_FULL`), with why.
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

/// The churn counter and the quests held for the run. Runtime only: a
/// restart forgets both, and a held quest's entry is gone from the record
/// by then ([`without`]).
#[derive(Debug, Default)]
pub struct QuestRuns {
    tracks: HashMap<(FormDesc, u32), Track>,
    held: BTreeSet<(u32, String)>,
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

    /// One run word of `quest` from `seat` (the reporter's actor) at
    /// `now_ms` on a monotonic clock, against the world's `record`: a held
    /// quest's word is held; then the seat's flip, which may make the quest
    /// held; then the fold, and the record's bound for a quest it lacks.
    pub fn judge(&mut self, record: &[QuestRun], quest: &FormDesc, word: Word, seat: u32, now_ms: u64) -> Verdict {
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
    /// a mode list no longer names, stays unsent) and not held.
    #[must_use]
    pub fn at_login<'a>(&self, record: &'a [QuestRun], shared: &SharedQuests) -> (Vec<&'a QuestRun>, Vec<&'a QuestRun>) {
        let sent = |r: &&QuestRun| shared.is_shared(r.quest.short_id, &r.quest.file) && !self.held(&r.quest);
        let starts = record.iter().filter(sent).filter(|r| r.running).collect();
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
        assert_eq!(Report::of(None, 0), Some(Report::Word(Word::Start)));
        assert_eq!(Report::of(Some(1), 0), Some(Report::Word(Word::Start)));
        assert_eq!(Report::of(Some(2), 0), Some(Report::Word(Word::Stop)));
        assert_eq!(Report::of(None, 10), Some(Report::Stage(10)));
        assert_eq!(Report::of(Some(3), 0), None);
        assert_eq!((Word::Start.wire(), Word::Stop.wire()), (1, 2));
        assert!(Word::Start.running() && !Word::Stop.running());
        assert_eq!((Code::Churn.name(), Code::Full.name()), ("E_QUEST_CHURN", "E_QUEST_RUNS_FULL"));
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
        assert!(runs.at_login(&stale, &all).0.is_empty());
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
        let (starts, stops) = runs.at_login(&record, &all);
        assert_eq!(starts.iter().map(|r| r.quest.short_id).collect::<Vec<_>>(), [LETTER]);
        assert_eq!(stops.iter().map(|r| r.quest.short_id).collect::<Vec<_>>(), [0x74a83]);
        // mode list sends what it lists
        let list = SharedQuests::from_json(r#"["074a83:Skyrim.esm"]"#).unwrap_or_default();
        let (starts, stops) = runs.at_login(&record, &list);
        assert!(starts.is_empty());
        assert_eq!(stops.len(), 1);
    }
}
