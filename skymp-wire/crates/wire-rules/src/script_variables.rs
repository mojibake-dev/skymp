//! The condition-read script variables (thuum ADR-068 rung 5, D-V1 B and
//! D-T2 B; docs/research/rung5-quests-2026-11-09.md sections 1(b), 7 and
//! 8(b)). A dialogue's conditions read quest and reference scripts'
//! variables (CTDA functions 629 GetVMQuestVariable and 630
//! GetVMScriptVariable, CommonLibSSE-NG include/RE/T/TESCondition.h:
//! 762-763), and the census found 458 of the 770 keys written by a topic
//! info's fragment, which runs on the talking seat alone (H-CV1 fails). So
//! no one seat streams them (D-F1 C's word, rung 4's election): every seat
//! reports a listed key its game changed (ScriptVariables 81), the latest
//! write wins per key in the server's order, AliasFill 79's shape, an equal
//! value folded, and the record keeps each key's value with the server's
//! sequence for it, one per key rising at each recorded write, which the
//! relay and the login serve carry so a receiver keeps the newest. The
//! world's record lives on the journal (PartOne::WorldJournal's
//! scriptVariables), relayed to every other seat and served at login.
//!
//! The listed keys are the setting `conditionVariables`, (form, script,
//! variable) triples the census emits (lab/tools/ctda-vars.py): the form a
//! QUST (629) or a REFR (630) by FormDesc, the script the form's attached
//! script whose chain declares the variable, the variable by the name the
//! condition's CIS2 carries (`::vampirestatus_var`, an auto property's
//! backing variable). Names compare without case, as Papyrus's do.
//!
//! The player cut (D-V1 B's own consequence, D-T2 B): a key whose form is a
//! player-class quest (wire-rules quest_scope; composed in by
//! [`ScriptVariables::set_player`] while the scope acts) is its player's
//! own. Such an entry travels flagged `own`, is recorded on the reporter's
//! own player's change form (rule 6), never relayed to another seat, and
//! served at login to that player alone, so PlayerVampireQuest's
//! VampireStatus, LastFeedTime and FeedTimer survive a relaunch without
//! reaching anyone else. A world key reported `own`, or a player's key
//! reported as the world's, is `E_SCRIPTVAR_PLAYER`.
//!
//! Refusals, logged by name: `E_SCRIPTVAR_UNLISTED` (a key the setting does
//! not list), `E_SCRIPTVAR_PLAYER` (the cut), `E_SCRIPTVAR_KIND` (a kind
//! other than the one the record holds for the key: a variable keeps its
//! declared type), `E_SCRIPTVAR_FULL` (a new key past
//! [`VARIABLE_RECORD_CAP`], rule 12's bound). R2, rule 5: the reporting
//! engine's value, recorded, never judged.

use crate::clock::SettingsError;
use crate::forms::FormDesc;

/// How many keys one record holds at most (the world's, or one player's
/// own): a bound over the census's 770 vanilla keys and the lab's mods
/// (UNMEASURED past vanilla). The setting may list no more, so a record of
/// listed keys never reaches it; a record an older setting left can.
pub const VARIABLE_RECORD_CAP: usize = 1024;

/// The longest script or variable name a key carries: the wire's
/// PAPYRUS_NAME (wire-schema cap), so every listed key fits the message.
pub const NAME_MAX: usize = 128;

/// One condition-read variable: its form, its script and its variable.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Key {
    /// The QUST or REFR, as the change form names it.
    pub form: FormDesc,
    /// The attached script whose chain declares the variable.
    pub script: String,
    /// The variable, as the condition names it.
    pub var: String,
}

impl Key {
    /// Whether this is `other`'s key: the same form (its plugin without
    /// case) and the same names without case.
    #[must_use]
    pub fn is(&self, other: &Key) -> bool {
        self.form.is(other.form.short_id, &other.form.file) && self.script.eq_ignore_ascii_case(&other.script) && self.var.eq_ignore_ascii_case(&other.var)
    }
}

/// A variable's value, rung 3's folded kinds a condition compares: 3 an
/// int, 4 a float, 5 a bool.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Value {
    /// An Int (kind 3).
    Int(i32),
    /// A Float (kind 4).
    Float(f32),
    /// A Bool (kind 5).
    Bool(bool),
}

impl Value {
    /// Its folded kind: 3, 4 or 5.
    #[must_use]
    pub fn kind(self) -> u8 {
        match self {
            Value::Int(_) => 3,
            Value::Float(_) => 4,
            Value::Bool(_) => 5,
        }
    }

    /// The value a kind and its payload name: kind 3 with `i`, 4 with `f`,
    /// 5 with `b`; none for any other kind (the wire refuses those first).
    #[must_use]
    pub fn of(k: u8, i: i32, f: f32, b: bool) -> Option<Self> {
        match k {
            3 => Some(Value::Int(i)),
            4 => Some(Value::Float(f)),
            5 => Some(Value::Bool(b)),
            _ => None,
        }
    }

    /// Whether two values are one: the same kind and the same payload
    /// (floats by `==`, so 0.0 and -0.0 fold; the wire carries no NaN).
    fn same(self, other: Value) -> bool {
        match (self, other) {
            (Value::Int(a), Value::Int(b)) => a == b,
            (Value::Float(a), Value::Float(b)) => a == b,
            (Value::Bool(a), Value::Bool(b)) => a == b,
            _ => false,
        }
    }
}

/// One recorded variable: its key, its value and the server's sequence for
/// the key (1 at its first write).
#[derive(Debug, Clone, PartialEq)]
pub struct Entry {
    /// The key.
    pub key: Key,
    /// The latest value.
    pub value: Value,
    /// The record's sequence for the key.
    pub seq: u32,
}

/// Why an entry is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// A key the setting does not list.
    Unlisted,
    /// A player's key reported as the world's, or a world key as own.
    Player,
    /// A kind other than the record's for the key.
    Kind,
    /// A new key past the record's cap.
    Full,
}

impl Code {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::Unlisted => "E_SCRIPTVAR_UNLISTED",
            Code::Player => "E_SCRIPTVAR_PLAYER",
            Code::Kind => "E_SCRIPTVAR_KIND",
            Code::Full => "E_SCRIPTVAR_FULL",
        }
    }
}

/// The rule's word on a reported entry.
#[derive(Debug, Clone, PartialEq)]
pub enum Verdict {
    /// The entry moved the record: merge it ([`merge`]) and relay it (a
    /// world key) or keep it (an own key).
    Recorded,
    /// The record holds the same value: nothing is written or relayed.
    Folded,
    /// Refused, logged by the code's name with why.
    Refuse(Code, String),
}

/// The listed keys and the player class composed in.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct ScriptVariables {
    keys: Vec<Key>,
    player: Vec<FormDesc>,
}

/// A name a key may carry: not empty, no NUL (the receiver's BSFixedString
/// reads a C string), at most [`NAME_MAX`] bytes.
fn name_fits(name: &str) -> bool {
    !name.is_empty() && !name.contains('\0') && name.len() <= NAME_MAX
}

impl ScriptVariables {
    /// The setting from the `conditionVariables` block's JSON text: a list
    /// of `{"form": "<hex id>:<plugin>", "script": "<name>", "var":
    /// "<name>"}` (`[]` for none), each key once, at most
    /// [`VARIABLE_RECORD_CAP`]. The error names the entry and what is wrong
    /// with it.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let block: serde_json::Value = serde_json::from_str(text).map_err(|e| SettingsError(format!("conditionVariables: {e}")))?;
        let serde_json::Value::Array(entries) = block else {
            return Err(SettingsError(format!("conditionVariables: {block} is not a list of {{\"form\", \"script\", \"var\"}}")));
        };
        if entries.len() > VARIABLE_RECORD_CAP {
            return Err(SettingsError(format!("conditionVariables: {} keys, past the record's cap of {VARIABLE_RECORD_CAP}", entries.len())));
        }
        let mut keys: Vec<Key> = Vec::with_capacity(entries.len());
        for e in &entries {
            let serde_json::Value::Object(fields) = e else {
                return Err(SettingsError(format!("conditionVariables: {e} is not {{\"form\", \"script\", \"var\"}}")));
            };
            if let Some(stray) = fields.keys().find(|k| !matches!(k.as_str(), "form" | "script" | "var")) {
                return Err(SettingsError(format!("conditionVariables: {stray:?} is no field of a key (form, script, var)")));
            }
            let text_of = |name: &str| match fields.get(name) {
                Some(serde_json::Value::String(s)) => Ok(s.clone()),
                _ => Err(SettingsError(format!("conditionVariables: {e} names no {name} string"))),
            };
            let form = FormDesc::parse(&text_of("form")?).map_err(|why| SettingsError(format!("conditionVariables: {why}")))?;
            let (script, var) = (text_of("script")?, text_of("var")?);
            if !name_fits(&script) || !name_fits(&var) {
                return Err(SettingsError(format!("conditionVariables: {e} has a name empty, past {NAME_MAX} bytes or carrying a NUL")));
            }
            let key = Key { form, script, var };
            if keys.iter().any(|k| k.is(&key)) {
                return Err(SettingsError(format!("conditionVariables: {e} is listed twice")));
            }
            keys.push(key);
        }
        Ok(Self { keys, player: Vec::new() })
    }

    /// Compose the player class in (wire-rules quest_scope, while the scope
    /// acts): a listed key whose form is one of `quests` is its player's
    /// own. Replaces any composed before.
    pub fn set_player(&mut self, quests: &[FormDesc]) {
        self.player = quests.to_vec();
    }

    /// The listed keys, in the setting's order.
    #[must_use]
    pub fn keys(&self) -> &[Key] {
        &self.keys
    }

    /// Whether the setting lists `key`.
    #[must_use]
    pub fn is_listed(&self, key: &Key) -> bool {
        self.keys.iter().any(|k| k.is(key))
    }

    /// Whether `key`'s form is of the composed player class: the key is its
    /// player's own.
    #[must_use]
    pub fn is_own(&self, key: &Key) -> bool {
        self.player.iter().any(|q| q.is(key.form.short_id, &key.form.file))
    }

    /// One reported entry, flagged `own` or not, against `record` (the
    /// world's for a world key, the reporter's player's for an own key): the
    /// list, then the player cut, then the record's kind for the key, the
    /// fold and the cap.
    #[must_use]
    pub fn judge(&self, record: &[Entry], key: &Key, value: Value, own: bool) -> Verdict {
        if !self.is_listed(key) {
            return Verdict::Refuse(Code::Unlisted, format!("{} is no key conditionVariables lists", describe(key)));
        }
        match (self.is_own(key), own) {
            (true, false) => return Verdict::Refuse(Code::Player, format!("{} is a player-class quest's, its player's own, reported as the world's", describe(key))),
            (false, true) => return Verdict::Refuse(Code::Player, format!("{} is the world's, reported as its player's own", describe(key))),
            _ => {}
        }
        match record.iter().find(|r| r.key.is(key)) {
            Some(r) if r.value.kind() != value.kind() => {
                Verdict::Refuse(Code::Kind, format!("{} holds kind {}, the report names kind {}", describe(key), r.value.kind(), value.kind()))
            }
            Some(r) if r.value.same(value) => Verdict::Folded,
            Some(_) => Verdict::Recorded,
            None if record.len() >= VARIABLE_RECORD_CAP => Verdict::Refuse(Code::Full, format!("the record holds its cap of {VARIABLE_RECORD_CAP} keys, {} not among them", describe(key))),
            None => Verdict::Recorded,
        }
    }

    /// What the login serves of `record`, in its order: the entries still
    /// listed, and of those the world's (`own` false, for the world's
    /// record) or the player's own (`own` true, for a player's record).
    #[must_use]
    pub fn at_login<'a>(&self, record: &'a [Entry], own: bool) -> Vec<&'a Entry> {
        record.iter().filter(|e| self.is_listed(&e.key) && self.is_own(&e.key) == own).collect()
    }
}

/// A key as the log names it: "<hex id>:<plugin> <script>.<variable>".
fn describe(key: &Key) -> String {
    format!("{:x}:{} {}.{}", key.form.short_id, key.form.file, key.script, key.var)
}

/// The record after a recorded entry and the key's new sequence: its value
/// replaced and its sequence raised, or the key added last at sequence 1
/// (not past [`VARIABLE_RECORD_CAP`], which the judgment refuses first: the
/// record comes back unchanged, sequence 0).
#[must_use]
pub fn merge(record: &[Entry], key: &Key, value: Value) -> (Vec<Entry>, u32) {
    let mut out = record.to_vec();
    let room = out.len() < VARIABLE_RECORD_CAP;
    let seq = match out.iter_mut().find(|r| r.key.is(key)) {
        Some(r) => {
            r.value = value;
            r.seq = r.seq.saturating_add(1);
            r.seq
        }
        None if room => {
            out.push(Entry { key: key.clone(), value, seq: 1 });
            1
        }
        None => 0,
    };
    (out, seq)
}

#[cfg(test)]
#[allow(clippy::panic)] // a test's failure message, never a packet's path
mod tests {
    use super::*;
    use proptest::prelude::*;

    // the census's keys (lab/.cache/census/r5/ctda-vars.tsv): MS13
    // 0x39645's ::camillaescort_var (a topic info's and a scene's write),
    // MG01 0x1f251's ::niryaskillvar_var (topic infos alone) and
    // PlayerVampireQuest 0xeafd5's ::vampirestatus_var (its own script's,
    // the player cut's)
    fn key(short_id: u32, script: &str, var: &str) -> Key {
        Key { form: FormDesc { short_id, file: "Skyrim.esm".to_owned() }, script: script.to_owned(), var: var.to_owned() }
    }

    fn escort() -> Key {
        key(0x39645, "MS13QuestScript", "::camillaescort_var")
    }

    fn status() -> Key {
        key(0xeafd5, "PlayerVampireQuestScript", "::vampirestatus_var")
    }

    const LISTED: &str = r#"[
        {"form": "039645:Skyrim.esm", "script": "MS13QuestScript", "var": "::camillaescort_var"},
        {"form": "01f251:Skyrim.esm", "script": "MG01QuestScript", "var": "::niryaskillvar_var"},
        {"form": "0eafd5:Skyrim.esm", "script": "PlayerVampireQuestScript", "var": "::vampirestatus_var"}
    ]"#;

    fn listed() -> ScriptVariables {
        ScriptVariables::from_json(LISTED).unwrap_or_default()
    }

    #[test]
    fn the_setting_parses_keys_and_refuses_the_rest() {
        let list = listed();
        assert_eq!(list.keys().len(), 3);
        assert!(list.is_listed(&key(0x39645, "ms13questscript", "::CamillaEscort_var")), "names compare without case");
        assert!(!list.is_listed(&key(0x39646, "MS13QuestScript", "::camillaescort_var")));
        assert!(ScriptVariables::from_json("[]").is_ok_and(|l| l.keys().is_empty()));
        let long = "x".repeat(NAME_MAX + 1);
        for bad in [
            r#"{"form": "039645:Skyrim.esm"}"#.to_owned(),
            r#"[{"form": "039645", "script": "S", "var": "v"}]"#.to_owned(),
            r#"[{"form": "039645:Skyrim.esm", "script": "", "var": "v"}]"#.to_owned(),
            r#"[{"form": "039645:Skyrim.esm", "script": "S"}]"#.to_owned(),
            r#"[{"form": "039645:Skyrim.esm", "script": "S", "var": "v", "own": true}]"#.to_owned(),
            r#"[{"form": "039645:Skyrim.esm", "script": "S", "var": "v"}, {"form": "39645:skyrim.esm", "script": "s", "var": "V"}]"#.to_owned(),
            format!(r#"[{{"form": "039645:Skyrim.esm", "script": "S", "var": "{long}"}}]"#),
            r#"[{"form": "039645:Skyrim.esm", "script": "S\u0000", "var": "v"}]"#.to_owned(),
            r#"["039645:Skyrim.esm"]"#.to_owned(),
        ] {
            assert!(ScriptVariables::from_json(&bad).is_err(), "{bad}");
        }
        let past: Vec<String> = (0..=VARIABLE_RECORD_CAP).map(|n| format!(r#"{{"form": "039645:Skyrim.esm", "script": "S", "var": "v{n}"}}"#)).collect();
        assert!(ScriptVariables::from_json(&format!("[{}]", past.join(","))).is_err());
    }

    #[test]
    fn the_latest_write_wins_per_key_and_an_equal_one_folds() {
        let list = listed();
        assert_eq!(list.judge(&[], &escort(), Value::Int(1), false), Verdict::Recorded);
        let (record, seq) = merge(&[], &escort(), Value::Int(1));
        assert_eq!(seq, 1);
        // any seat's same write folds; another value moves the key's sequence
        assert_eq!(list.judge(&record, &escort(), Value::Int(1), false), Verdict::Folded);
        assert_eq!(list.judge(&record, &escort(), Value::Int(2), false), Verdict::Recorded);
        let (record, seq) = merge(&record, &escort(), Value::Int(2));
        assert_eq!((seq, record.len()), (2, 1));
        // another key starts at 1
        let skill = key(0x1f251, "MG01QuestScript", "::niryaskillvar_var");
        let (record, seq) = merge(&record, &skill, Value::Float(25.0));
        assert_eq!((seq, record.len()), (1, 2));
        assert_eq!(list.judge(&record, &skill, Value::Float(-0.0), false), Verdict::Recorded);
        assert_eq!(list.judge(&merge(&record, &skill, Value::Float(0.0)).0, &skill, Value::Float(-0.0), false), Verdict::Folded);
        // a kind other than the record's is refused
        assert!(matches!(list.judge(&record, &escort(), Value::Bool(true), false), Verdict::Refuse(Code::Kind, _)));
        // an unlisted key
        match list.judge(&record, &key(0x74a83, "FreeformRiverwood01Script", "::sent_var"), Value::Int(1), false) {
            Verdict::Refuse(code, why) => assert_eq!((code.name(), why), ("E_SCRIPTVAR_UNLISTED", "74a83:Skyrim.esm FreeformRiverwood01Script.::sent_var is no key conditionVariables lists".to_owned())),
            other => panic!("not refused: {other:?}"),
        }
    }

    #[test]
    fn a_player_class_quests_keys_are_its_players_own() {
        let mut list = listed();
        // the scope off: the vampire's status is a world key like any other
        assert_eq!(list.judge(&[], &status(), Value::Int(2), false), Verdict::Recorded);
        assert!(matches!(list.judge(&[], &status(), Value::Int(2), true), Verdict::Refuse(Code::Player, _)));
        // derived: its player's own, recorded on its record, the world's refused
        list.set_player(&[FormDesc { short_id: 0xeafd5, file: "Skyrim.esm".to_owned() }]);
        assert!(list.is_own(&status()) && !list.is_own(&escort()));
        assert_eq!(list.judge(&[], &status(), Value::Int(2), true), Verdict::Recorded);
        match list.judge(&[], &status(), Value::Int(2), false) {
            Verdict::Refuse(code, why) => assert_eq!((code.name(), why), ("E_SCRIPTVAR_PLAYER", "eafd5:Skyrim.esm PlayerVampireQuestScript.::vampirestatus_var is a player-class quest's, its player's own, reported as the world's".to_owned())),
            other => panic!("not refused: {other:?}"),
        }
        assert!(matches!(list.judge(&[], &escort(), Value::Int(1), true), Verdict::Refuse(Code::Player, _)));
        // the login serves each record's own half
        let (world, _) = merge(&[], &escort(), Value::Int(1));
        let (world, _) = merge(&world, &status(), Value::Int(2));
        assert_eq!(list.at_login(&world, false).iter().map(|e| e.key.var.as_str()).collect::<Vec<_>>(), ["::camillaescort_var"]);
        assert_eq!(list.at_login(&world, true).len(), 1);
        // an older record's unlisted key stays unsent
        let (stale, _) = merge(&world, &key(0x74a83, "S", "v"), Value::Int(1));
        assert_eq!(list.at_login(&stale, false).len(), 1);
    }

    #[test]
    fn the_record_stops_at_its_cap() {
        let mut keys: Vec<String> = Vec::new();
        let mut record = Vec::new();
        for n in 0..VARIABLE_RECORD_CAP {
            keys.push(format!(r#"{{"form": "039645:Skyrim.esm", "script": "S", "var": "v{n}"}}"#));
            record = merge(&record, &key(0x39645, "S", &format!("v{n}")), Value::Int(0)).0;
        }
        assert_eq!(record.len(), VARIABLE_RECORD_CAP);
        let mut list = ScriptVariables::from_json(&format!("[{}]", keys.join(","))).unwrap_or_default();
        list.keys.push(key(0x39645, "S", "extra"));
        assert!(matches!(list.judge(&record, &key(0x39645, "S", "extra"), Value::Int(1), false), Verdict::Refuse(Code::Full, _)));
        assert_eq!(merge(&record, &key(0x39645, "S", "extra"), Value::Int(1)), (record.clone(), 0));
        // a key the full record holds still moves
        assert_eq!(list.judge(&record, &key(0x39645, "S", "v0"), Value::Int(1), false), Verdict::Recorded);
        assert_eq!((Code::Kind.name(), Code::Full.name()), ("E_SCRIPTVAR_KIND", "E_SCRIPTVAR_FULL"));
        assert_eq!((Value::of(3, 7, 0.0, false), Value::of(4, 0, 0.5, false), Value::of(5, 0, 0.0, true), Value::of(2, 0, 0.0, false)), (Some(Value::Int(7)), Some(Value::Float(0.5)), Some(Value::Bool(true)), None));
    }

    proptest! {
        // any run of writes: a verdict, never a panic; the record never past
        // its cap, each key once, its sequence the count of its recorded
        // writes
        #[test]
        fn writes_never_panic_and_keep_one_entry_per_key(writes in proptest::collection::vec((0u8..4, 0u8..6, any::<i32>(), any::<bool>()), 0..64)) {
            let list = listed();
            let keys = [escort(), key(0x1f251, "MG01QuestScript", "::niryaskillvar_var"), status(), key(0x74a83, "S", "v")];
            let mut record: Vec<Entry> = Vec::new();
            let mut recorded = [0u32; 4];
            for (which, k, i, b) in writes {
                let n = usize::from(which);
                let Some(key) = keys.get(n) else { continue };
                let Some(value) = Value::of(k, i, 0.25, b) else { continue };
                if list.judge(&record, key, value, false) == Verdict::Recorded {
                    let (after, seq) = merge(&record, key, value);
                    record = after;
                    if let Some(count) = recorded.get_mut(n) {
                        *count = count.saturating_add(1);
                        prop_assert_eq!(seq, *count);
                    }
                }
                prop_assert!(record.len() <= keys.len());
            }
            for e in &record {
                prop_assert_eq!(record.iter().filter(|r| r.key.is(&e.key)).count(), 1);
            }
        }
    }
}
