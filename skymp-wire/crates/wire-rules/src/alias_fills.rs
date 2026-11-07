//! Quest alias fills (thuum ADR-068 rung 4 and its amendment B, read 8;
//! docs/research/rung4-state-rows-2026-11-07.md section 1(c)). A quest's
//! reference alias is nobody's (D-F1 C): every seat runs the quest's scripts,
//! so a script's ReferenceAlias.ForceRefTo or Clear runs on every seat and
//! each reports it (AliasFill 79, the ref half). The server keeps one fill
//! per (quest, alias) on the journal (PartOne::WorldJournal's aliasFills),
//! the latest report winning and an equal one folded, relays a fill that
//! moved the record to the other seats and serves the record at login.
//! Refusals, logged by name: `E_ALIASFILL_NO_QUEST` (no QUST record),
//! `E_ALIASFILL_NO_ALIAS` (no reference alias of that id in the quest's
//! record), `E_ALIASFILL_REF` (the fill's form is no reference). R2, rule 5:
//! recorded as reported.
//!
//! The created half is the server's own, R0 from the game files (amendment
//! B, read 8): an alias that creates its reference (ALCO, the base) in
//! another alias's inventory (ALCA with its top bit set, the low half the
//! alias's id; MS13's GoldenClaw and ArvelsJournal both read 0x80000001, in
//! alias 1, lab/esm.py on Skyrim.esm 2026-11-06) puts one item in that
//! container when the quest fills, which no seat reports (a template-running
//! quest fills without ForceRefTo). The server resolves the container from
//! the target alias's own fill type, a forced reference (ALFR) or a unique
//! actor (ALUA, the one ACHR of that base), and adds the item once,
//! [`due`] reading the journal's marks so a take, a second load or a
//! restart never adds it again. A container the rule cannot resolve (a fill
//! by conditions, a creation at an alias rather than in it, an alias the
//! quest lacks) is `E_ALIASFILL_UNRESOLVED`, logged once by the core, and
//! that item stays each seat's own.

/// One reference alias's fill, by form ids in the load order both ends
/// share.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Fill {
    /// The QUST.
    pub quest: u32,
    /// The alias's id in its quest (ALST).
    pub alias: u32,
    /// The reference filling it, 0 when cleared.
    pub refr: u32,
}

/// What the core knows of a reported fill.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Facts {
    /// The quest is a QUST record.
    pub is_quest: bool,
    /// The quest's record has a reference alias (ALST) of that id.
    pub has_alias: bool,
    /// The fill's form is a reference (a placement or a form the server
    /// made); true for a clear.
    pub refr_is_ref: bool,
}

/// Why a fill is refused, or a creation's container unresolved.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// No QUST record.
    NoQuest,
    /// No reference alias of that id in the quest.
    NoAlias,
    /// The fill's form is no reference.
    Ref,
    /// A created alias whose container the rule cannot resolve.
    Unresolved,
}

impl Code {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::NoQuest => "E_ALIASFILL_NO_QUEST",
            Code::NoAlias => "E_ALIASFILL_NO_ALIAS",
            Code::Ref => "E_ALIASFILL_REF",
            Code::Unresolved => "E_ALIASFILL_UNRESOLVED",
        }
    }
}

/// The rule's word on a reported fill.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    /// The fill moved the record: merge it ([`merge`]) and relay it.
    Recorded,
    /// The record holds the same fill: nothing is written or relayed.
    Folded,
    /// Refused, logged by the code's name with why.
    Refuse(Code, String),
}

/// One reported fill against the record, over the facts the core gathered.
#[must_use]
pub fn judge(record: &[Fill], fill: Fill, facts: Facts) -> Verdict {
    if !facts.is_quest {
        return Verdict::Refuse(Code::NoQuest, format!("{:x} is no QUST record", fill.quest));
    }
    if !facts.has_alias {
        return Verdict::Refuse(Code::NoAlias, format!("quest {:x} has no reference alias {}", fill.quest, fill.alias));
    }
    if fill.refr != 0 && !facts.refr_is_ref {
        return Verdict::Refuse(Code::Ref, format!("{:x} is no reference", fill.refr));
    }
    if record.iter().any(|r| r.quest == fill.quest && r.alias == fill.alias && r.refr == fill.refr) {
        Verdict::Folded
    } else {
        Verdict::Recorded
    }
}

/// The record after an accepted fill: its (quest, alias)'s fill replaced, or
/// added; a clear is kept as refr 0, so a late seat is served it.
#[must_use]
pub fn merge(record: &[Fill], fill: Fill) -> Vec<Fill> {
    let mut out = record.to_vec();
    match out.iter_mut().find(|r| r.quest == fill.quest && r.alias == fill.alias) {
        Some(r) => r.refr = fill.refr,
        None => out.push(fill),
    }
    out
}

/// ALCA's top bit: the created reference goes into the target alias's
/// inventory, not at its place.
pub const CREATE_IN: u32 = 0x8000_0000;

/// One reference alias of a quest as the game files hold it (libespm QUST's
/// ALST, ALCO, ALCA, ALFR and ALUA), the form ids in the load order.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Alias {
    /// The alias's id (ALST).
    pub id: u32,
    /// The base the alias creates (ALCO), 0 for none.
    pub created: u32,
    /// Where it is created (ALCA): [`CREATE_IN`] and the target alias's id
    /// in the low half.
    pub create_at: u32,
    /// The forced reference (ALFR), 0 for none.
    pub forced: u32,
    /// The unique actor's base (ALUA), 0 for none.
    pub unique_actor: u32,
}

/// Where a created item goes, as the target alias's fill type names it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Container {
    /// A forced reference (ALFR): the reference itself.
    Forced(u32),
    /// A unique actor (ALUA): the one ACHR of this base, the core's to find.
    UniqueActor(u32),
    /// The rule cannot resolve it: `E_ALIASFILL_UNRESOLVED`, the words why.
    Unresolved(&'static str),
}

/// A created alias's item and the container it goes into.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Creation {
    /// The created alias's id.
    pub alias: u32,
    /// The base it creates.
    pub base: u32,
    /// The target alias's id (ALCA's low half).
    pub target: u32,
    /// Where the item goes.
    pub container: Container,
}

/// Every created alias of one quest's `aliases` and where its item goes.
#[must_use]
pub fn creations(aliases: &[Alias]) -> Vec<Creation> {
    aliases
        .iter()
        .filter(|a| a.created != 0)
        .map(|a| {
            let target = a.create_at & 0xFFFF;
            let container = if a.create_at & CREATE_IN == 0 {
                Container::Unresolved("created at its target alias, not in its inventory")
            } else {
                match aliases.iter().find(|t| t.id == target) {
                    None => Container::Unresolved("the quest has no alias of the target's id"),
                    Some(t) if t.forced != 0 => Container::Forced(t.forced),
                    Some(t) if t.unique_actor != 0 => Container::UniqueActor(t.unique_actor),
                    Some(_) => Container::Unresolved("the target alias fills by conditions"),
                }
            };
            Creation { alias: a.id, base: a.created, target, container }
        })
        .collect()
}

/// The journal's mark of a created alias's item: added once.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Mark {
    /// The QUST.
    pub quest: u32,
    /// The created alias's id.
    pub alias: u32,
    /// The item was added.
    pub added: bool,
}

/// Whether the created alias `alias` of `quest` still owes its item: no
/// mark says it was added.
#[must_use]
pub fn due(marks: &[Mark], quest: u32, alias: u32) -> bool {
    !marks.iter().any(|m| m.quest == quest && m.alias == alias && m.added)
}

#[cfg(test)]
mod tests {
    use super::*;

    // Bleak Falls MS13 0x39645's aliases as Skyrim.esm holds them (lab/esm.py
    // on sky-srv, 2026-11-06): Arvel 1 the unique actor 0x39646, ArvelsJournal
    // 16 and GoldenClaw 11 created in alias 1, ArvelWeb 10 forced to the web
    // 0x1fc43
    const MS13: u32 = 0x39645;
    const CLAW: u32 = 0x39647;
    const JOURNAL: u32 = 0x39654;
    const ARVEL_BASE: u32 = 0x39646;
    const WEB: u32 = 0x1fc43;

    fn ms13() -> Vec<Alias> {
        vec![
            Alias { id: 1, unique_actor: ARVEL_BASE, ..Alias::default() },
            Alias { id: 16, created: JOURNAL, create_at: 0x8000_0001, ..Alias::default() },
            Alias { id: 10, forced: WEB, ..Alias::default() },
            Alias { id: 11, created: CLAW, create_at: 0x8000_0001, ..Alias::default() },
        ]
    }

    #[test]
    fn the_claw_and_the_journal_go_into_arvel() {
        assert_eq!(
            creations(&ms13()),
            vec![
                Creation { alias: 16, base: JOURNAL, target: 1, container: Container::UniqueActor(ARVEL_BASE) },
                Creation { alias: 11, base: CLAW, target: 1, container: Container::UniqueActor(ARVEL_BASE) },
            ]
        );
    }

    #[test]
    fn a_container_the_rule_cannot_resolve_is_named() {
        let aliases = [
            Alias { id: 1, ..Alias::default() },
            Alias { id: 2, forced: WEB, ..Alias::default() },
            Alias { id: 3, created: CLAW, create_at: 0x8000_0001, ..Alias::default() },
            Alias { id: 4, created: CLAW, create_at: 0x0000_0002, ..Alias::default() },
            Alias { id: 5, created: CLAW, create_at: 0x8000_0009, ..Alias::default() },
            Alias { id: 6, created: JOURNAL, create_at: 0x8000_0002, ..Alias::default() },
        ];
        let made = creations(&aliases);
        let container = |alias: u32| made.iter().find(|c| c.alias == alias).map(|c| c.container);
        assert_eq!(container(3), Some(Container::Unresolved("the target alias fills by conditions")));
        assert_eq!(container(4), Some(Container::Unresolved("created at its target alias, not in its inventory")));
        assert_eq!(container(5), Some(Container::Unresolved("the quest has no alias of the target's id")));
        assert_eq!(container(6), Some(Container::Forced(WEB)));
        assert_eq!(made.len(), 4, "an alias that creates nothing is no creation");
    }

    #[test]
    fn a_creation_is_due_until_its_mark() {
        let marks = [Mark { quest: MS13, alias: 11, added: true }, Mark { quest: MS13, alias: 16, added: false }];
        assert!(!due(&marks, MS13, 11));
        assert!(due(&marks, MS13, 16));
        assert!(due(&[], MS13, 11));
        assert!(due(&marks, 0x12345, 11));
    }

    #[test]
    fn the_latest_fill_wins_and_an_equal_one_is_folded() {
        let ok = Facts { is_quest: true, has_alias: true, refr_is_ref: true };
        let first = Fill { quest: MS13, alias: 10, refr: WEB };
        assert_eq!(judge(&[], first, ok), Verdict::Recorded);
        let record = merge(&[], first);
        assert_eq!(judge(&record, first, ok), Verdict::Folded);
        // a clear moves it, kept as 0
        let cleared = Fill { refr: 0, ..first };
        assert_eq!(judge(&record, cleared, ok), Verdict::Recorded);
        let record = merge(&record, cleared);
        assert_eq!(record, vec![cleared]);
        // another alias is its own entry
        let record = merge(&record, Fill { alias: 3, ..first });
        assert_eq!(record.len(), 2);
    }

    #[test]
    fn the_refusals_have_their_names() {
        let ok = Facts { is_quest: true, has_alias: true, refr_is_ref: true };
        let fill = Fill { quest: MS13, alias: 10, refr: WEB };
        let refused = |v: Verdict| match v {
            Verdict::Refuse(code, why) => (code.name(), why),
            other => (if other == Verdict::Recorded { "recorded" } else { "folded" }, String::new()),
        };
        assert_eq!(refused(judge(&[], fill, Facts { is_quest: false, ..ok })), ("E_ALIASFILL_NO_QUEST", "39645 is no QUST record".to_owned()));
        assert_eq!(refused(judge(&[], fill, Facts { has_alias: false, ..ok })), ("E_ALIASFILL_NO_ALIAS", "quest 39645 has no reference alias 10".to_owned()));
        assert_eq!(refused(judge(&[], fill, Facts { refr_is_ref: false, ..ok })), ("E_ALIASFILL_REF", "1fc43 is no reference".to_owned()));
        // a clear names no reference
        assert_eq!(judge(&[], Fill { refr: 0, ..fill }, Facts { refr_is_ref: false, ..ok }), Verdict::Recorded);
        assert_eq!(Code::Unresolved.name(), "E_ALIASFILL_UNRESOLVED");
    }
}
