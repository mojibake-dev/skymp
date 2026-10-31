//! Perks as transactions (thuum docs/verbs/perks.md, M5): a player's game
//! reports the perk its Skills menu took, and the server judges the pick as
//! the menu did, by the perk record's own conditions (the CTDAs before the
//! first PRKE section: HasPerk for the tree's lines and the rank before,
//! GetBaseActorValue for the skill's level; UESP "Skyrim Mod:Mod File
//! Format/PERK", read in Skyrim.esm 2026-10-31: Armsman20 0x79343 wants
//! GetBaseActorValue(OneHanded) >= 20 and HasPerk(Armsman00) == 1) and by
//! the points left, one a level over the perks held. The record is the
//! server's (R0); the pick is the player's report (R1). A condition the
//! server cannot judge refuses the pick (rule 5), named by its function id.
//! Function ids are CommonLibSSE-NG's FUNCTION_DATA::FunctionID
//! (include/RE/T/TESCondition.h:48 GetActorValue 14, :120 GetLevel 80, :351
//! GetBaseActorValue 277, :566 HasPerk 448); actor value indexes are
//! include/RE/A/ActorValues.h (the skills 6 to 23).

use std::collections::HashMap;

/// The most perks a record holds (Skyrim's trees have 251 ranks in all).
pub const MAX_PERKS: usize = 512;
/// The condition functions the rule judges.
pub const FN_GET_ACTOR_VALUE: u16 = 14;
/// GetLevel.
pub const FN_GET_LEVEL: u16 = 80;
/// GetBaseActorValue.
pub const FN_GET_BASE_ACTOR_VALUE: u16 = 277;
/// HasPerk.
pub const FN_HAS_PERK: u16 = 448;
/// The CTDA flag bit for a comparison against a global (not judged here).
pub const FLAG_USE_GLOBAL: u8 = 0x04;
/// The CTDA flag bit for a swapped subject (not judged here).
pub const FLAG_SWAP_SUBJECT: u8 = 0x10;
/// The highest actor value index (CommonLibSSE-NG ActorValues.h, kTotal 164).
pub const MAX_ACTOR_VALUE: u32 = 163;

/// A CTDA comparison operator (the top three bits of its first byte).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Op {
    /// Equal.
    Eq,
    /// Not equal.
    Ne,
    /// Greater than.
    Gt,
    /// Greater than or equal.
    Ge,
    /// Less than.
    Lt,
    /// Less than or equal.
    Le,
}

impl Op {
    /// The operator in a CTDA's first byte (libespm CTDA.h GetOperator).
    pub fn from_ctda(byte0: u8) -> Option<Op> {
        match byte0.wrapping_shr(5) {
            0 => Some(Op::Eq),
            1 => Some(Op::Ne),
            2 => Some(Op::Gt),
            3 => Some(Op::Ge),
            4 => Some(Op::Lt),
            5 => Some(Op::Le),
            _ => None,
        }
    }

    /// Whether `lhs op rhs` holds.
    pub fn holds(self, lhs: f32, rhs: f32) -> bool {
        match self {
            Op::Eq => (lhs - rhs).abs() < f32::EPSILON,
            Op::Ne => (lhs - rhs).abs() >= f32::EPSILON,
            Op::Gt => lhs > rhs,
            Op::Ge => lhs >= rhs,
            Op::Lt => lhs < rhs,
            Op::Le => lhs <= rhs,
        }
    }
}

/// One of a perk's own conditions, as the rule judges it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Condition {
    /// HasPerk(perk) == 1 (`wanted` true) or == 0.
    HasPerk {
        /// The perk's form id.
        perk: u32,
        /// Held (true) or not held (false).
        wanted: bool,
    },
    /// GetBaseActorValue or GetActorValue of `av` against `value`: a skill's
    /// recorded level for 6 to 23, the recorded base otherwise.
    ActorValue {
        /// The actor value index.
        av: u8,
        /// The comparison.
        op: Op,
        /// The value compared against.
        value: f32,
    },
    /// GetLevel against `value`.
    Level {
        /// The comparison.
        op: Op,
        /// The value compared against.
        value: f32,
    },
    /// A function, flag or shape the rule does not judge; refuses the pick.
    Unknown {
        /// The CTDA function id.
        function: u16,
    },
}

impl Condition {
    /// A condition from a CTDA's fields: the function id, the first byte, the
    /// comparison value, the first parameter (a form id made global by the
    /// caller for HasPerk, an actor value index otherwise) and whether it runs
    /// on the subject with a plain comparison (`run_on` 0, no global, no swap).
    pub fn from_ctda(function: u16, byte0: u8, value: f32, param1: u32, run_on: u32) -> Condition {
        let flags = byte0 & 0x1f;
        if run_on != 0 || flags & (FLAG_USE_GLOBAL | FLAG_SWAP_SUBJECT) != 0 {
            return Condition::Unknown { function };
        }
        let Some(op) = Op::from_ctda(byte0) else {
            return Condition::Unknown { function };
        };
        match function {
            FN_HAS_PERK => match op {
                Op::Eq => Condition::HasPerk { perk: param1, wanted: value >= 0.5 },
                Op::Ne => Condition::HasPerk { perk: param1, wanted: value < 0.5 },
                _ => Condition::Unknown { function },
            },
            FN_GET_ACTOR_VALUE | FN_GET_BASE_ACTOR_VALUE => match u8::try_from(param1) {
                Ok(av) if param1 <= MAX_ACTOR_VALUE => Condition::ActorValue { av, op, value },
                _ => Condition::Unknown { function },
            },
            FN_GET_LEVEL => Condition::Level { op, value },
            other => Condition::Unknown { function: other },
        }
    }
}

/// A perk record's parts the rule reads.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Entry {
    /// DATA's isPlayable byte.
    pub playable: bool,
    /// DATA's isHidden byte.
    pub hidden: bool,
    /// The conditions before the first PRKE section, in order (all must hold:
    /// the OR flag is not honoured, which only refuses more).
    pub conditions: Vec<Condition>,
}

/// Every perk of the load order the rule knows.
#[derive(Debug, Default)]
pub struct Table {
    entries: HashMap<u32, Entry>,
}

impl Table {
    /// A perk with its DATA; conditions come after through `add_condition`.
    /// A repeated id replaces the entry (a later plugin's override).
    pub fn add_perk(&mut self, perk: u32, playable: bool, hidden: bool) {
        if perk == 0 {
            return;
        }
        self.entries.insert(perk, Entry { playable, hidden, conditions: Vec::new() });
    }

    /// A condition of `perk` (added before); false for a perk not added.
    pub fn add_condition(&mut self, perk: u32, condition: Condition) -> bool {
        match self.entries.get_mut(&perk) {
            Some(e) => {
                e.conditions.push(condition);
                true
            }
            None => false,
        }
    }

    /// The entry of `perk`.
    pub fn get(&self, perk: u32) -> Option<&Entry> {
        self.entries.get(&perk)
    }

    /// How many perks the table knows.
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Whether the table is empty.
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }
}

/// What the rule reads of a player's record.
#[derive(Debug, Clone, Copy)]
pub struct Record<'a> {
    /// The character level (1 at the start).
    pub level: u16,
    /// The perks held.
    pub perks: &'a [u32],
    /// Actor values the conditions may read: (index, the value a condition
    /// compares: a skill's level for 6 to 23, the base otherwise).
    pub values: &'a [(u8, f32)],
}

impl Record<'_> {
    fn value(&self, av: u8) -> f32 {
        self.values.iter().find(|(a, _)| *a == av).map_or(0.0, |(_, v)| *v)
    }
}

/// Why a pick is refused; each has a reason code for the log.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Refusal {
    /// No such perk in the game files.
    Unknown,
    /// Not playable, or hidden.
    Hidden,
    /// Already held.
    Owned,
    /// The record holds MAX_PERKS.
    Full,
    /// No point left.
    NoPoints,
    /// A HasPerk condition fails; the perk it names.
    Prerequisite(u32),
    /// An actor value condition fails; the actor value.
    Value(u8),
    /// A GetLevel condition fails.
    Level,
    /// A condition the rule does not judge; its function id.
    Condition(u16),
}

impl Refusal {
    /// The reason code.
    pub fn code(self) -> &'static str {
        match self {
            Refusal::Unknown => "E_PERK_UNKNOWN",
            Refusal::Hidden => "E_PERK_HIDDEN",
            Refusal::Owned => "E_PERK_OWNED",
            Refusal::Full => "E_PERK_FULL",
            Refusal::NoPoints => "E_PERK_POINTS",
            Refusal::Prerequisite(_) => "E_PERK_PREREQ",
            Refusal::Value(_) => "E_PERK_SKILL",
            Refusal::Level => "E_PERK_LEVEL",
            Refusal::Condition(_) => "E_PERK_CONDITION",
        }
    }

    /// The detail: the perk, the actor value or the function the code names,
    /// 0 for the rest.
    pub fn detail(self) -> u32 {
        match self {
            Refusal::Prerequisite(p) => p,
            Refusal::Value(av) => u32::from(av),
            Refusal::Condition(f) => u32::from(f),
            _ => 0,
        }
    }
}

/// The perk points left: one a level past the first, less the perks held.
pub fn points(level: u16, held: usize) -> u16 {
    let earned = usize::from(level).saturating_sub(1);
    u16::try_from(earned.saturating_sub(held)).unwrap_or(u16::MAX)
}

/// The judgement of `perk` picked by the player whose record this is.
pub fn pick(table: &Table, record: &Record<'_>, perk: u32) -> Result<(), Refusal> {
    let entry = table.get(perk).ok_or(Refusal::Unknown)?;
    if !entry.playable || entry.hidden {
        return Err(Refusal::Hidden);
    }
    if record.perks.contains(&perk) {
        return Err(Refusal::Owned);
    }
    if record.perks.len() >= MAX_PERKS {
        return Err(Refusal::Full);
    }
    if points(record.level, record.perks.len()) == 0 {
        return Err(Refusal::NoPoints);
    }
    for condition in &entry.conditions {
        match *condition {
            Condition::HasPerk { perk: wanted_perk, wanted } => {
                if record.perks.contains(&wanted_perk) != wanted {
                    return Err(Refusal::Prerequisite(wanted_perk));
                }
            }
            Condition::ActorValue { av, op, value } => {
                if !op.holds(record.value(av), value) {
                    return Err(Refusal::Value(av));
                }
            }
            Condition::Level { op, value } => {
                if !op.holds(f32::from(record.level), value) {
                    return Err(Refusal::Level);
                }
            }
            Condition::Unknown { function } => return Err(Refusal::Condition(function)),
        }
    }
    Ok(())
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::arithmetic_side_effects, clippy::indexing_slicing, clippy::float_cmp)]
mod tests {
    use super::*;

    const ARMSMAN: u32 = 0xbabe4;
    const ARMSMAN20: u32 = 0x79343;
    const ARMSMAN40: u32 = 0x79342;
    const FIGHTING_STANCE: u32 = 0x52d50;
    const HIDDEN: u32 = 0x1234;
    const ONE_HANDED: u8 = 6;
    // the CTDA first byte for >= is 0x60 (operator 3 in the top three bits),
    // for == 0x00: Skyrim.esm's Armsman20 and FightingStance as read
    const GE: u8 = 0x60;
    const EQ: u8 = 0x00;

    fn skyrim() -> Table {
        let mut t = Table::default();
        t.add_perk(ARMSMAN, true, false);
        t.add_perk(ARMSMAN20, true, false);
        t.add_condition(ARMSMAN20, Condition::from_ctda(FN_GET_BASE_ACTOR_VALUE, GE, 20.0, u32::from(ONE_HANDED), 0));
        t.add_condition(ARMSMAN20, Condition::from_ctda(FN_HAS_PERK, EQ, 1.0, ARMSMAN, 0));
        t.add_perk(ARMSMAN40, true, false);
        t.add_condition(ARMSMAN40, Condition::from_ctda(FN_GET_BASE_ACTOR_VALUE, GE, 40.0, u32::from(ONE_HANDED), 0));
        t.add_condition(ARMSMAN40, Condition::from_ctda(FN_HAS_PERK, EQ, 1.0, ARMSMAN20, 0));
        t.add_perk(FIGHTING_STANCE, true, false);
        t.add_condition(FIGHTING_STANCE, Condition::from_ctda(FN_HAS_PERK, EQ, 1.0, ARMSMAN, 0));
        t.add_condition(FIGHTING_STANCE, Condition::from_ctda(FN_GET_BASE_ACTOR_VALUE, GE, 20.0, u32::from(ONE_HANDED), 0));
        t.add_perk(HIDDEN, true, true);
        t
    }

    fn record<'a>(level: u16, perks: &'a [u32], values: &'a [(u8, f32)]) -> Record<'a> {
        Record { level, perks, values }
    }

    #[test]
    fn conditions_read_from_the_record_s_fields() {
        assert_eq!(
            Condition::from_ctda(FN_GET_BASE_ACTOR_VALUE, GE, 20.0, 6, 0),
            Condition::ActorValue { av: 6, op: Op::Ge, value: 20.0 }
        );
        assert_eq!(Condition::from_ctda(FN_HAS_PERK, EQ, 1.0, ARMSMAN, 0), Condition::HasPerk { perk: ARMSMAN, wanted: true });
        assert_eq!(Condition::from_ctda(FN_HAS_PERK, EQ, 0.0, ARMSMAN, 0), Condition::HasPerk { perk: ARMSMAN, wanted: false });
        assert_eq!(Condition::from_ctda(FN_HAS_PERK, 0x20, 1.0, ARMSMAN, 0), Condition::HasPerk { perk: ARMSMAN, wanted: false });
        assert_eq!(Condition::from_ctda(FN_GET_LEVEL, GE, 10.0, 0, 0), Condition::Level { op: Op::Ge, value: 10.0 });
        // not judged: a global, a swapped subject, another run-on, an odd function, an actor value past the table
        assert_eq!(Condition::from_ctda(FN_GET_BASE_ACTOR_VALUE, GE | FLAG_USE_GLOBAL, 20.0, 6, 0), Condition::Unknown { function: 277 });
        assert_eq!(Condition::from_ctda(FN_GET_BASE_ACTOR_VALUE, GE | FLAG_SWAP_SUBJECT, 20.0, 6, 0), Condition::Unknown { function: 277 });
        assert_eq!(Condition::from_ctda(FN_GET_BASE_ACTOR_VALUE, GE, 20.0, 6, 1), Condition::Unknown { function: 277 });
        assert_eq!(Condition::from_ctda(74, GE, 1.0, 0, 0), Condition::Unknown { function: 74 });
        assert_eq!(Condition::from_ctda(FN_GET_BASE_ACTOR_VALUE, GE, 20.0, 200, 0), Condition::Unknown { function: 277 });
        assert_eq!(Condition::from_ctda(FN_HAS_PERK, GE, 1.0, ARMSMAN, 0), Condition::Unknown { function: 448 });
        assert_eq!(Op::from_ctda(0xa0), Some(Op::Le));
        assert_eq!(Op::from_ctda(0xc0), None);
        assert!(Op::Ge.holds(20.0, 20.0) && !Op::Gt.holds(20.0, 20.0) && Op::Ne.holds(1.0, 0.0) && Op::Eq.holds(1.0, 1.0));
    }

    #[test]
    fn points_are_one_a_level_over_the_perks_held() {
        assert_eq!(points(1, 0), 0);
        assert_eq!(points(2, 0), 1);
        assert_eq!(points(5, 2), 2);
        assert_eq!(points(5, 9), 0);
        assert_eq!(points(0, 0), 0);
    }

    #[test]
    fn the_first_perk_takes_a_point_and_the_tree_s_lines_hold() {
        let t = skyrim();
        let no_points = record(1, &[], &[(ONE_HANDED, 20.0)]);
        assert_eq!(pick(&t, &no_points, ARMSMAN), Err(Refusal::NoPoints));
        let fresh = record(2, &[], &[(ONE_HANDED, 15.0)]);
        assert_eq!(pick(&t, &fresh, ARMSMAN), Ok(()));
        // the second rank before the first: the prerequisite, even with the skill
        let skilled = record(2, &[], &[(ONE_HANDED, 25.0)]);
        assert!(matches!(pick(&t, &skilled, ARMSMAN20), Err(Refusal::Prerequisite(ARMSMAN))));
        // with Armsman held: the skill decides
        let held = [ARMSMAN];
        assert!(matches!(pick(&t, &record(3, &held, &[(ONE_HANDED, 15.0)]), ARMSMAN20), Err(Refusal::Value(ONE_HANDED))));
        assert_eq!(pick(&t, &record(3, &held, &[(ONE_HANDED, 20.0)]), ARMSMAN20), Ok(()));
        assert_eq!(pick(&t, &record(3, &held, &[(ONE_HANDED, 20.0)]), FIGHTING_STANCE), Ok(()));
        assert!(matches!(pick(&t, &record(3, &held, &[(ONE_HANDED, 19.0)]), FIGHTING_STANCE), Err(Refusal::Value(ONE_HANDED))));
        // held already, hidden, unknown
        assert_eq!(pick(&t, &record(3, &held, &[(ONE_HANDED, 20.0)]), ARMSMAN), Err(Refusal::Owned));
        assert_eq!(pick(&t, &record(3, &held, &[]), HIDDEN), Err(Refusal::Hidden));
        assert_eq!(pick(&t, &record(3, &held, &[]), 0xdead), Err(Refusal::Unknown));
        // a skill absent from the record reads 0
        assert!(matches!(pick(&t, &record(3, &held, &[]), ARMSMAN20), Err(Refusal::Value(ONE_HANDED))));
        // the points run out along the ranks
        let two = [ARMSMAN, ARMSMAN20];
        assert_eq!(pick(&t, &record(3, &two, &[(ONE_HANDED, 40.0)]), ARMSMAN40), Err(Refusal::NoPoints));
        assert_eq!(pick(&t, &record(4, &two, &[(ONE_HANDED, 40.0)]), ARMSMAN40), Ok(()));
    }

    #[test]
    fn a_condition_the_rule_cannot_judge_refuses_and_names_its_function() {
        let mut t = skyrim();
        t.add_perk(0x999, true, false);
        t.add_condition(0x999, Condition::from_ctda(74, GE, 1.0, 0, 0));
        let r = pick(&t, &record(5, &[], &[]), 0x999);
        assert_eq!(r, Err(Refusal::Condition(74)));
        assert_eq!(r.unwrap_err().code(), "E_PERK_CONDITION");
        assert_eq!(r.unwrap_err().detail(), 74);
        t.add_perk(0x998, true, false);
        t.add_condition(0x998, Condition::from_ctda(FN_GET_LEVEL, GE, 10.0, 0, 0));
        assert_eq!(pick(&t, &record(5, &[], &[]), 0x998), Err(Refusal::Level));
        assert_eq!(pick(&t, &record(10, &[], &[]), 0x998), Ok(()));
        assert_eq!(Refusal::Prerequisite(ARMSMAN).detail(), ARMSMAN);
        assert_eq!(Refusal::Value(6).code(), "E_PERK_SKILL");
        assert!(!t.add_condition(0x777, Condition::Level { op: Op::Ge, value: 1.0 }));
        assert_eq!(t.len(), 7);
        assert!(!t.is_empty());
    }

    #[test]
    fn a_full_record_takes_no_more() {
        let mut t = Table::default();
        t.add_perk(1, true, false);
        let full: Vec<u32> = (2..=u32::try_from(MAX_PERKS).unwrap() + 1).collect();
        assert_eq!(pick(&t, &record(u16::MAX, &full, &[]), 1), Err(Refusal::Full));
        // perk 0 is never an entry
        t.add_perk(0, true, false);
        assert_eq!(t.get(0), None);
    }
}
