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
/// The CTDA flag bit joining a condition to the next by OR: a run of flagged
/// conditions and the one after them is a group that holds when any member
/// holds; the groups are ANDed (the engine's reading, which the perk trees
/// use for a perk with two parents).
pub const FLAG_OR: u8 = 0x01;
/// HasKeyword (CommonLibSSE-NG TESCondition.h:690): a perk effect's subject
/// tab reads it against the weapon's or the armor piece's keywords.
pub const FN_HAS_KEYWORD: u16 = 560;
/// The entry points the server's formulas take (CommonLibSSE-NG
/// include/RE/B/BGSEntryPoint.h:13-106): kModAttackDamage.
pub const ENTRY_MOD_ATTACK_DAMAGE: u8 = 35;
/// kModArmorRating.
pub const ENTRY_MOD_ARMOR_RATING: u8 = 85;
/// The entry functions the rule applies (BGSEntryPointPerkEntry.h:20-37):
/// SetValue.
pub const EFFECT_SET_VALUE: u8 = 1;
/// AddValue.
pub const EFFECT_ADD_VALUE: u8 = 2;
/// MultiplyValue.
pub const EFFECT_MULTIPLY_VALUE: u8 = 3;
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
    /// HasKeyword(keyword) == 1 (`wanted` true) or == 0, judged against a
    /// subject's keywords (an effect's weapon or armor piece); a perk's own
    /// condition on it refuses the pick (the record carries no keywords).
    HasKeyword {
        /// The keyword's form id.
        keyword: u32,
        /// Carried (true) or not (false).
        wanted: bool,
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
            FN_HAS_KEYWORD => match op {
                Op::Eq => Condition::HasKeyword { keyword: param1, wanted: value >= 0.5 },
                Op::Ne => Condition::HasKeyword { keyword: param1, wanted: value < 0.5 },
                _ => Condition::Unknown { function },
            },
            other => Condition::Unknown { function: other },
        }
    }
}

/// One condition of a perk with its place in the OR grouping.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Clause {
    /// The condition.
    pub condition: Condition,
    /// Whether the CTDA's OR flag joins it to the next clause.
    pub or_next: bool,
}

/// One of a perk's entry point effects (a PRKE section with a one-float
/// value): what the server's formulas apply when the perk is held.
#[derive(Debug, Clone, PartialEq)]
pub struct Effect {
    /// The entry point (BGSEntryPoint.h).
    pub entry_point: u8,
    /// The function (SetValue, AddValue, MultiplyValue judged; others not).
    pub function: u8,
    /// EPFD's value.
    pub value: f32,
    /// PRKE's rank.
    pub rank: u8,
    /// PRKE's priority: the order the effects apply in.
    pub priority: u8,
    /// The first tab's conditions, on the perk owner (the pick's judge).
    pub owner: Vec<Clause>,
    /// The second tab's conditions, on the subject (keywords).
    pub subject: Vec<Clause>,
    /// Whether the formulas judge it: a judged function and no condition on a
    /// third tab.
    pub judged: bool,
}

/// A perk record's parts the rule reads.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Entry {
    /// DATA's isPlayable byte.
    pub playable: bool,
    /// DATA's isHidden byte.
    pub hidden: bool,
    /// The conditions before the first PRKE section, in order, grouped by the
    /// OR flag ([`FLAG_OR`]): every group must hold.
    pub conditions: Vec<Clause>,
    /// The entry point effects, in the record's order.
    pub effects: Vec<Effect>,
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
        self.entries.insert(perk, Entry { playable, hidden, conditions: Vec::new(), effects: Vec::new() });
    }

    /// A condition of `perk` (added before) standing on its own (no OR to the
    /// next); false for a perk not added.
    pub fn add_condition(&mut self, perk: u32, condition: Condition) -> bool {
        self.add_clause(perk, condition, false)
    }

    /// A condition of `perk` (added before) with its OR flag to the next
    /// clause; false for a perk not added.
    pub fn add_clause(&mut self, perk: u32, condition: Condition, or_next: bool) -> bool {
        match self.entries.get_mut(&perk) {
            Some(e) => {
                e.conditions.push(Clause { condition, or_next });
                true
            }
            None => false,
        }
    }

    /// An entry point effect of `perk` (added before): its index among the
    /// perk's effects; None for a perk not added.
    pub fn add_effect(&mut self, perk: u32, entry_point: u8, function: u8, value: f32, rank: u8, priority: u8) -> Option<usize> {
        let e = self.entries.get_mut(&perk)?;
        let judged = matches!(function, EFFECT_SET_VALUE | EFFECT_ADD_VALUE | EFFECT_MULTIPLY_VALUE);
        e.effects.push(Effect { entry_point, function, value, rank, priority, owner: Vec::new(), subject: Vec::new(), judged });
        Some(e.effects.len().saturating_sub(1))
    }

    /// A condition of that effect on `tab` (0 the perk owner, 1 the subject)
    /// with its OR flag to the next; false when it lands on a third tab,
    /// which leaves the effect out, or names no effect.
    pub fn add_effect_condition(&mut self, perk: u32, effect: usize, tab: u8, condition: Condition, or_next: bool) -> bool {
        let Some(e) = self.entries.get_mut(&perk).and_then(|e| e.effects.get_mut(effect)) else {
            return false;
        };
        match tab {
            0 => e.owner.push(Clause { condition, or_next }),
            1 => e.subject.push(Clause { condition, or_next }),
            _ => {
                e.judged = false;
                return false;
            }
        }
        true
    }

    /// How many effects the formulas judge.
    pub fn judged_effects(&self) -> usize {
        self.entries.values().flat_map(|e| e.effects.iter()).filter(|f| f.judged).count()
    }

    /// How many effects are left out (another function, a third tab).
    pub fn unjudged_effects(&self) -> usize {
        self.entries.values().flat_map(|e| e.effects.iter()).filter(|f| !f.judged).count()
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
    groups_hold(&entry.conditions, |condition| judge(record, condition))
}

/// The clauses in order: a run joined by the OR flag and the clause after it
/// is one group, held when any member holds; every group must hold, and a
/// failing group refuses with its first member's refusal.
fn groups_hold(clauses: &[Clause], mut judge_one: impl FnMut(Condition) -> Result<(), Refusal>) -> Result<(), Refusal> {
    let mut holds = false;
    let mut first_failure: Option<Refusal> = None;
    for clause in clauses {
        if !holds {
            match judge_one(clause.condition) {
                Ok(()) => holds = true,
                Err(refusal) => {
                    if first_failure.is_none() {
                        first_failure = Some(refusal);
                    }
                }
            }
        }
        if !clause.or_next {
            if !holds {
                if let Some(refusal) = first_failure {
                    return Err(refusal);
                }
            }
            holds = false;
            first_failure = None;
        }
    }
    // a last clause flagged OR with nothing after it closes its group here
    if !holds {
        if let Some(refusal) = first_failure {
            return Err(refusal);
        }
    }
    Ok(())
}

/// One condition against a subject's keywords (an effect's weapon or armor
/// piece): HasKeyword alone is judged there.
fn judge_subject(keywords: &[u32], condition: Condition) -> Result<(), Refusal> {
    match condition {
        Condition::HasKeyword { keyword, wanted } if keywords.contains(&keyword) == wanted => Ok(()),
        Condition::HasKeyword { .. } => Err(Refusal::Condition(FN_HAS_KEYWORD)),
        Condition::HasPerk { .. } => Err(Refusal::Condition(FN_HAS_PERK)),
        Condition::ActorValue { .. } => Err(Refusal::Condition(FN_GET_ACTOR_VALUE)),
        Condition::Level { .. } => Err(Refusal::Condition(FN_GET_LEVEL)),
        Condition::Unknown { function } => Err(Refusal::Condition(function)),
    }
}

/// `start` through the held perks' effects on `entry_point` whose owner
/// conditions hold for the record and whose subject conditions hold for
/// `subject_keywords`, in (priority, perk id) order: SetValue replaces,
/// AddValue adds, MultiplyValue multiplies; an effect the formulas do not
/// judge, or whose conditions do not hold, leaves the value alone (thuum
/// docs/verbs/perks.md, the second half).
#[must_use]
pub fn apply_entry_point(table: &Table, entry_point: u8, start: f32, record: &Record<'_>, subject_keywords: &[u32]) -> f32 {
    let mut applicable: Vec<(u8, u32, &Effect)> = Vec::new();
    for &perk in record.perks {
        let Some(entry) = table.get(perk) else {
            continue;
        };
        for effect in &entry.effects {
            if effect.entry_point != entry_point || !effect.judged {
                continue;
            }
            if groups_hold(&effect.owner, |c| judge(record, c)).is_err() {
                continue;
            }
            if groups_hold(&effect.subject, |c| judge_subject(subject_keywords, c)).is_err() {
                continue;
            }
            applicable.push((effect.priority, perk, effect));
        }
    }
    applicable.sort_by_key(|(priority, perk, _)| (*priority, *perk));
    let mut value = start;
    for (_, _, effect) in applicable {
        value = match effect.function {
            EFFECT_SET_VALUE => effect.value,
            EFFECT_ADD_VALUE => value + effect.value,
            EFFECT_MULTIPLY_VALUE => value * effect.value,
            _ => value,
        };
    }
    value
}

/// One condition against the record.
fn judge(record: &Record<'_>, condition: Condition) -> Result<(), Refusal> {
    match condition {
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
        // the record carries no keywords: a perk's own HasKeyword refuses, as
        // the unjudged function it was before the effects read it
        Condition::HasKeyword { .. } => return Err(Refusal::Condition(FN_HAS_KEYWORD)),
        Condition::Unknown { function } => return Err(Refusal::Condition(function)),
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

    // a perk with two parents (Skyrim's Paralyzing Strike, Warmaster,
    // Bullseye, Locksmith, Purity...): HasPerk(A) OR HasPerk(B), AND a skill
    const TWO_PARENTS: u32 = 0x5555;
    const PARENT_A: u32 = 0x1111;
    const PARENT_B: u32 = 0x2222;

    fn two_parents() -> Table {
        let mut t = Table::default();
        t.add_perk(PARENT_A, true, false);
        t.add_perk(PARENT_B, true, false);
        t.add_perk(TWO_PARENTS, true, false);
        t.add_clause(TWO_PARENTS, Condition::from_ctda(FN_HAS_PERK, EQ | FLAG_OR, 1.0, PARENT_A, 0), true);
        t.add_clause(TWO_PARENTS, Condition::from_ctda(FN_HAS_PERK, EQ, 1.0, PARENT_B, 0), false);
        t.add_clause(TWO_PARENTS, Condition::from_ctda(FN_GET_BASE_ACTOR_VALUE, GE, 30.0, u32::from(ONE_HANDED), 0), false);
        t
    }

    #[test]
    fn an_or_group_holds_with_either_parent_and_the_groups_are_anded() {
        let t = two_parents();
        let thirty = [(ONE_HANDED, 30.0)];
        let ten = [(ONE_HANDED, 10.0)];
        assert_eq!(pick(&t, &Record { level: 10, perks: &[PARENT_A], values: &thirty }, TWO_PARENTS), Ok(()));
        assert_eq!(pick(&t, &Record { level: 10, perks: &[PARENT_B], values: &thirty }, TWO_PARENTS), Ok(()));
        assert_eq!(
            pick(&t, &Record { level: 10, perks: &[], values: &thirty }, TWO_PARENTS),
            Err(Refusal::Prerequisite(PARENT_A)),
            "neither parent: the group's first refusal"
        );
        assert_eq!(
            pick(&t, &Record { level: 10, perks: &[PARENT_A], values: &ten }, TWO_PARENTS),
            Err(Refusal::Value(ONE_HANDED)),
            "the parent group holds, the skill group does not"
        );
    }

    #[test]
    fn an_unknown_clause_inside_a_held_or_group_refuses_nothing() {
        let mut t = Table::default();
        t.add_perk(PARENT_A, true, false);
        t.add_perk(TWO_PARENTS, true, false);
        t.add_clause(TWO_PARENTS, Condition::from_ctda(FN_HAS_PERK, EQ | FLAG_OR, 1.0, PARENT_A, 0), true);
        t.add_clause(TWO_PARENTS, Condition::from_ctda(999, EQ, 1.0, 0, 0), false);
        assert_eq!(pick(&t, &Record { level: 10, perks: &[PARENT_A], values: &[] }, TWO_PARENTS), Ok(()));
        assert_eq!(pick(&t, &Record { level: 10, perks: &[], values: &[] }, TWO_PARENTS), Err(Refusal::Prerequisite(PARENT_A)));
    }

    #[test]
    fn a_trailing_or_flag_closes_its_group_at_the_end() {
        let mut t = Table::default();
        t.add_perk(PARENT_A, true, false);
        t.add_perk(TWO_PARENTS, true, false);
        t.add_clause(TWO_PARENTS, Condition::from_ctda(FN_HAS_PERK, EQ | FLAG_OR, 1.0, PARENT_A, 0), true);
        assert_eq!(pick(&t, &Record { level: 10, perks: &[PARENT_A], values: &[] }, TWO_PARENTS), Ok(()));
        assert_eq!(pick(&t, &Record { level: 10, perks: &[], values: &[] }, TWO_PARENTS), Err(Refusal::Prerequisite(PARENT_A)));
    }

    // the weapon school's effects as Skyrim.esm reads them (lab/esm.py,
    // 2026-11-01): Armsman00 multiplies by 1.2 on kModAttackDamage while
    // Armsman20 is not held, for a weapon carrying one of the one-handed
    // keywords (OR, the last flagged too); Armsman20 by 1.4 the same way
    const SWORD: u32 = 0x1e711;
    const DAGGER: u32 = 0x1e713;
    const BOW: u32 = 0x1e715;

    fn weapon_school() -> Table {
        let mut t = Table::default();
        t.add_perk(ARMSMAN, true, false);
        t.add_perk(ARMSMAN20, true, false);
        t.add_perk(ARMSMAN40, true, false);
        for (perk, next, value) in [(ARMSMAN, ARMSMAN20, 1.2), (ARMSMAN20, ARMSMAN40, 1.4)] {
            let e = t.add_effect(perk, ENTRY_MOD_ATTACK_DAMAGE, EFFECT_MULTIPLY_VALUE, value, 0, 0).unwrap();
            assert!(t.add_effect_condition(perk, e, 0, Condition::from_ctda(FN_HAS_PERK, EQ, 0.0, next, 0), false));
            assert!(t.add_effect_condition(perk, e, 1, Condition::from_ctda(FN_HAS_KEYWORD, EQ | FLAG_OR, 1.0, SWORD, 0), true));
            assert!(t.add_effect_condition(perk, e, 1, Condition::from_ctda(FN_HAS_KEYWORD, EQ | FLAG_OR, 1.0, DAGGER, 0), true));
        }
        t
    }

    #[test]
    fn a_weapon_perk_multiplies_a_hit_with_its_school_keyword() {
        let t = weapon_school();
        let held = Record { level: 5, perks: &[ARMSMAN], values: &[] };
        assert_eq!(apply_entry_point(&t, ENTRY_MOD_ATTACK_DAMAGE, 10.0, &held, &[SWORD, 0x1234]), 12.0);
        assert_eq!(apply_entry_point(&t, ENTRY_MOD_ATTACK_DAMAGE, 10.0, &held, &[DAGGER]), 12.0);
        assert_eq!(apply_entry_point(&t, ENTRY_MOD_ATTACK_DAMAGE, 10.0, &held, &[BOW]), 10.0, "not the school's weapon");
        assert_eq!(apply_entry_point(&t, ENTRY_MOD_ATTACK_DAMAGE, 10.0, &held, &[]), 10.0, "unarmed carries no keyword");
        assert_eq!(apply_entry_point(&t, ENTRY_MOD_ARMOR_RATING, 10.0, &held, &[SWORD]), 10.0, "another entry point");
        let none = Record { level: 5, perks: &[], values: &[] };
        assert_eq!(apply_entry_point(&t, ENTRY_MOD_ATTACK_DAMAGE, 10.0, &none, &[SWORD]), 10.0);
    }

    #[test]
    fn the_rank_held_above_excludes_the_rank_below() {
        let t = weapon_school();
        let both = Record { level: 5, perks: &[ARMSMAN20, ARMSMAN], values: &[] };
        assert_eq!(apply_entry_point(&t, ENTRY_MOD_ATTACK_DAMAGE, 10.0, &both, &[SWORD]), 14.0);
    }

    #[test]
    fn effects_apply_in_priority_order_and_an_unjudged_one_is_left_out() {
        let mut t = Table::default();
        t.add_perk(ARMSMAN, true, false);
        // priority 1 multiplies, priority 0 adds: 10 -> 12 -> 24
        let m = t.add_effect(ARMSMAN, ENTRY_MOD_ATTACK_DAMAGE, EFFECT_MULTIPLY_VALUE, 2.0, 0, 1).unwrap();
        let a = t.add_effect(ARMSMAN, ENTRY_MOD_ATTACK_DAMAGE, EFFECT_ADD_VALUE, 2.0, 0, 0).unwrap();
        // a third-tab condition leaves this one out, and so does a function the rule does not apply
        let third = t.add_effect(ARMSMAN, ENTRY_MOD_ATTACK_DAMAGE, EFFECT_SET_VALUE, 1.0, 0, 0).unwrap();
        assert!(!t.add_effect_condition(ARMSMAN, third, 2, Condition::from_ctda(FN_HAS_KEYWORD, EQ, 1.0, SWORD, 0), false));
        let _spell = t.add_effect(ARMSMAN, ENTRY_MOD_ATTACK_DAMAGE, 10, 0.0, 0, 0).unwrap();
        assert_ne!(m, a);
        assert_eq!((t.judged_effects(), t.unjudged_effects()), (2, 2));
        let held = Record { level: 5, perks: &[ARMSMAN], values: &[] };
        assert_eq!(apply_entry_point(&t, ENTRY_MOD_ATTACK_DAMAGE, 10.0, &held, &[]), 24.0);
        assert!(t.add_effect(0x9999, ENTRY_MOD_ATTACK_DAMAGE, EFFECT_ADD_VALUE, 1.0, 0, 0).is_none(), "a perk not added");
    }

    #[test]
    fn a_perks_own_has_keyword_still_refuses_the_pick() {
        let mut t = Table::default();
        t.add_perk(ARMSMAN, true, false);
        t.add_condition(ARMSMAN, Condition::from_ctda(FN_HAS_KEYWORD, EQ, 1.0, SWORD, 0));
        assert_eq!(pick(&t, &Record { level: 5, perks: &[], values: &[] }, ARMSMAN), Err(Refusal::Condition(FN_HAS_KEYWORD)));
    }
}
