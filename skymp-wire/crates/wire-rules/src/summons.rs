//! A player's conjured summon as an owned actor shared between games (thuum
//! tracker #84, the verb `summon`; ADR-052 decision 3 as amended 2026-11-16;
//! ADR-063: the caster's row owns it). The caster's engine spawns its summon
//! later than the cast (no event names the actor), so its game reports each
//! new summon once (Summon, MsgType 83, `ended` false) and its end once
//! (`ended` true). The server places its own actor of the reported base at
//! the reported place, records it as the caster's (cells.rs
//! `FollowKind::Summon`: the caster's game simulates it in any cell, every
//! other game draws a puppet), and dispels it at the first of its four ends:
//! the caster's end report (the effect's timer, or the engine's Finish after
//! a death), a backstop past the reported time left, the caster's departure
//! (cells.rs `SummonGone`) and a restart (swept at load).
//!
//! R2, recorded unvalidated (Eli, 2026-11-16 about 2:45 AM PST: "'The server
//! checks that the caster has learned the spell' This is an example of an
//! unnecessary check. This is not a competitive game."; CLAUDE.md rule 5's
//! R2 branch, ADR-063: the server validates consistency only): the caster's
//! engine computes the summon's base, its place, its count and its life; the
//! server records what it is told. A start is refused, by name, only when it
//! breaks the record's consistency:
//! - the reporter plays an actor, which is the caster (the message names no
//!   other actor) (`E_SUMMON_CASTER`), and its local id names no summon the
//!   record already holds (`E_SUMMON_TWICE`);
//! - the base is an actor record the files hold, the only thing the server
//!   can place (`E_SUMMON_BASE`);
//! - the place is in a cell the server knows (`E_SUMMON_CELL`).
//!
//! How many summons one caster holds, and for how long, its engine decides
//! (a scroll's, a staff's, a mod's cast is recorded like a learned spell's:
//! SCRL 0xa44a1 ConjureStormAtronachScroll, lab/esm.py on sky-srv,
//! 2026-11-16); the reported time left sets the backstop timer alone. The
//! end report of a local id the record does not hold folds, nothing named.

use std::collections::HashMap;

/// How long past the reported time left the server dispels a summon whose
/// end report never came, ms.
pub const BACKSTOP_MS: u64 = 5_000;

/// How long after its record a summon's create word names its effect to
/// the listeners that are not its caster (`summon_effect`, thuum tracker
/// #111), ms: the record's own subscription pass, which the core runs right
/// after the record (PartOne::OnSummonStart), with room for a listener the
/// next grid pass adds. A watcher's copy created inside it plays the
/// effect's arrival art; a later loader's copy stands plain, the summon
/// having arrived before that game saw it. A policy bound, not an engine
/// number.
pub const ARRIVAL_WORD_MS: u64 = 3_000;

/// Why a summon report is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// The reporter plays no actor.
    Caster,
    /// The local id is one of the caster's live summons already.
    Twice,
    /// The base is no actor record the files hold.
    Base,
    /// The place is in no cell the server knows.
    Cell,
}

impl Code {
    /// The refusal's name in the server's log.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::Caster => "E_SUMMON_CASTER",
            Code::Twice => "E_SUMMON_TWICE",
            Code::Base => "E_SUMMON_BASE",
            Code::Cell => "E_SUMMON_CELL",
        }
    }
}

/// A refusal: its code and the reason for the log.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Refused {
    /// The code.
    pub code: Code,
    /// Why, for the log.
    pub why: String,
}

fn refuse<T>(code: Code, why: String) -> Result<T, Refused> {
    Err(Refused { code, why })
}

/// A start report and the facts the core read for it: what the record holds
/// after it (the row's base, effect and spell are the report's own words,
/// recorded as told).
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct StartFacts {
    /// The reporter's actor, the caster; 0 when it plays none.
    pub caster: u32,
    /// The summon's id in the caster's engine (0xff000000 and up).
    pub local: u32,
    /// The reported base.
    pub base: u32,
    /// The reported summon effect, recorded for the log and the row.
    pub effect: u32,
    /// The reported spell (a learned one, a scroll's, a staff's), recorded
    /// for the log and the row.
    pub spell: u32,
    /// The base is an NPC_ record the files hold.
    pub base_is_npc: bool,
    /// The time the caster's engine reports left, seconds; the backstop
    /// timer's alone.
    pub remaining_s: f32,
    /// The reported place is in a cell the server knows.
    pub in_cell: bool,
}

/// One summon the server holds, for the core's log and its create word.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Row {
    /// The server's actor.
    pub actor: u32,
    /// Its caster, the owner.
    pub caster: u32,
    /// Its id in the caster's engine.
    pub local: u32,
    /// Its base, as reported.
    pub base: u32,
    /// Its summon effect, as reported.
    pub effect: u32,
    /// Its spell, as reported.
    pub spell: u32,
    /// When the backstop dispels it, on the core's monotonic clock, ms.
    pub backstop_ms: u64,
    /// While its dispel fades: when the fade ends and the core deletes it.
    pub fading_until_ms: Option<u64>,
    /// When the core recorded it, on its monotonic clock, ms: the start of
    /// its arrival's word (`ARRIVAL_WORD_MS`).
    pub recorded_ms: u64,
}

/// What the clock found due.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Due {
    /// Live summons whose backstop passed, by actor id: the core dispels each.
    pub backstops: Vec<u32>,
    /// Summons whose dispel's fade ended, by actor id, gone from the table:
    /// the core deletes each.
    pub faded: Vec<u32>,
}

/// The summons the server holds, by their server actor.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Summons {
    rows: HashMap<u32, Row>,
}

/// Seconds as whole milliseconds, saturating; a negative or non-finite
/// number is 0.
#[allow(clippy::as_conversions)]
fn ms_of(seconds: f32) -> u64 {
    if seconds.is_finite() && seconds > 0.0 {
        (f64::from(seconds) * 1000.0) as u64
    } else {
        0
    }
}

impl Summons {
    /// An empty table.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The summons held, live and fading.
    #[must_use]
    pub fn len(&self) -> usize {
        self.rows.len()
    }

    /// Whether the table holds none.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.rows.is_empty()
    }

    fn live(&self) -> impl Iterator<Item = &Row> {
        self.rows.values().filter(|r| r.fading_until_ms.is_none())
    }

    /// `caster`'s live summons (a fading one counts no more: its engine
    /// ended it).
    #[must_use]
    pub fn live_of(&self, caster: u32) -> usize {
        self.live().filter(|r| r.caster == caster).count()
    }

    /// The live summon `caster`'s engine numbers `local`, its server actor.
    #[must_use]
    pub fn actor_of(&self, caster: u32, local: u32) -> Option<u32> {
        self.live().find(|r| r.caster == caster && r.local == local).map(|r| r.actor)
    }

    /// The row of `actor`, live or fading.
    #[must_use]
    pub fn row(&self, actor: u32) -> Option<Row> {
        self.rows.get(&actor).copied()
    }

    /// Whose summon `actor` is, 0 for none held.
    #[must_use]
    pub fn caster_of(&self, actor: u32) -> u32 {
        self.rows.get(&actor).map_or(0, |r| r.caster)
    }

    /// The create word's `ownLocal` for `listener`: the local id when it is
    /// the caster of the live summon `actor`, else 0 (none sent).
    #[must_use]
    pub fn own_local(&self, actor: u32, listener: u32) -> u32 {
        self.rows
            .get(&actor)
            .filter(|r| r.fading_until_ms.is_none() && r.caster == listener && listener != 0)
            .map_or(0, |r| r.local)
    }

    /// The create word's `summonEffect` for `listener` at `now_ms` (thuum
    /// tracker #111): the summon effect of the live summon `actor`, whose
    /// arrival art a watcher's copy plays, when `listener` is not its caster
    /// (whose word binds its engine's own summon, `own_local`, and whose
    /// engine draws the arrival itself) and the summon was recorded no more
    /// than [`ARRIVAL_WORD_MS`] before; else 0 (none sent).
    #[must_use]
    pub fn summon_effect(&self, actor: u32, listener: u32, now_ms: u64) -> u32 {
        self.rows
            .get(&actor)
            .filter(|r| {
                r.fading_until_ms.is_none()
                    && r.caster != listener
                    && now_ms.saturating_sub(r.recorded_ms) <= ARRIVAL_WORD_MS
            })
            .map_or(0, |r| r.effect)
    }

    /// The record's consistency of a start report, nothing recorded: the
    /// reporter's own player, its engine's id naming no summon held, a base
    /// the server could place, a cell it knows.
    ///
    /// # Errors
    /// The first consistency broken, by its code, with why.
    pub fn judge(&self, f: &StartFacts) -> Result<(), Refused> {
        if f.caster == 0 {
            return refuse(Code::Caster, "the sender plays no actor, so no caster stands behind the report".to_owned());
        }
        if let Some(actor) = self.actor_of(f.caster, f.local) {
            return refuse(Code::Twice, format!("{:x}'s engine reported its summon {:x} already, the server's {actor:x}", f.caster, f.local));
        }
        if !f.base_is_npc {
            return refuse(Code::Base, format!("the base {:x} is no actor record the files hold", f.base));
        }
        if !f.in_cell {
            return refuse(Code::Cell, "the place is in no cell the server's files hold".to_owned());
        }
        Ok(())
    }

    /// The core placed `actor` for the accepted start `f` at `now_ms`: held,
    /// its backstop at the reported time left plus [`BACKSTOP_MS`]. False,
    /// and nothing recorded, when the table holds `actor` or `f`'s local id
    /// for its caster already, or the start breaks a consistency (`judge`).
    pub fn record(&mut self, f: &StartFacts, actor: u32, now_ms: u64) -> bool {
        if actor == 0 || self.rows.contains_key(&actor) || self.judge(f).is_err() {
            return false;
        }
        let backstop_ms = now_ms.saturating_add(ms_of(f.remaining_s)).saturating_add(BACKSTOP_MS);
        self.rows.insert(
            actor,
            Row {
                actor,
                caster: f.caster,
                local: f.local,
                base: f.base,
                effect: f.effect,
                spell: f.spell,
                backstop_ms,
                fading_until_ms: None,
                recorded_ms: now_ms,
            },
        );
        true
    }

    /// `caster`'s end report of its summon `local`: the server's actor to
    /// dispel, `None` when its record holds no live summon by that id
    /// (never reported, or dispelled already), folded.
    #[must_use]
    pub fn end(&self, caster: u32, local: u32) -> Option<u32> {
        self.actor_of(caster, local)
    }

    /// The dispel of `actor` begins at `now_ms`: its row stays, fading,
    /// until `fade_ms` later, when [`Summons::tick`] hands it to the core
    /// to delete. False when no live summon is `actor` (none held, or its
    /// dispel runs already).
    pub fn dispel(&mut self, actor: u32, now_ms: u64, fade_ms: u64) -> bool {
        match self.rows.get_mut(&actor) {
            Some(r) if r.fading_until_ms.is_none() => {
                r.fading_until_ms = Some(now_ms.saturating_add(fade_ms));
                true
            }
            _ => false,
        }
    }

    /// `actor` is gone by another way (deleted, swept): its row goes. False
    /// when none was held.
    pub fn forget(&mut self, actor: u32) -> bool {
        self.rows.remove(&actor).is_some()
    }

    /// The clock at `now_ms`: the live summons whose backstop passed (still
    /// held: the core dispels each), and the fading ones whose fade ended
    /// (gone from the table: the core deletes each), both by actor id.
    pub fn tick(&mut self, now_ms: u64) -> Due {
        let mut due = Due::default();
        for r in self.rows.values() {
            match r.fading_until_ms {
                None if now_ms >= r.backstop_ms => due.backstops.push(r.actor),
                Some(until) if now_ms >= until => due.faded.push(r.actor),
                _ => {}
            }
        }
        due.backstops.sort_unstable();
        due.faded.sort_unstable();
        for actor in &due.faded {
            self.rows.remove(actor);
        }
        due
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const ROTFERN: u32 = 0xff00_0001;
    const ARIADNE: u32 = 0xff00_0002;
    // Skyrim.esm (lab/esm.py on sky-srv, 2026-11-16)
    const CONJURE_FLAME_ATRONACH: u32 = 0x100e76;
    const SUMMON_FLAME_ATRONACH_POTENT: u32 = 0x4e945;
    const ATRONACH_POTENT: u32 = 0x4e940;
    // SCRL 0xa44a1 ConjureStormAtronachScroll (lab/esm.py on sky-srv,
    // 2026-11-16): a scroll's cast is recorded like a learned spell's
    const CONJURE_STORM_ATRONACH_SCROLL: u32 = 0xa44a1;
    // x-summon-baseline 20261116-083744's engine id
    const LOCAL: u32 = 0xff00_08fd;

    /// The baseline's cast: rotfern's potent atronach 300 units ahead with
    /// 59.5 s of the effect's 60 left.
    fn start() -> StartFacts {
        StartFacts {
            caster: ROTFERN,
            local: LOCAL,
            base: ATRONACH_POTENT,
            effect: SUMMON_FLAME_ATRONACH_POTENT,
            spell: CONJURE_FLAME_ATRONACH,
            base_is_npc: true,
            remaining_s: 59.5,
            in_cell: true,
        }
    }

    fn code(r: Result<(), Refused>) -> Option<&'static str> {
        r.err().map(|e| e.code.name())
    }

    #[test]
    fn a_summon_within_the_consistencies_is_placed_and_held_as_its_casters() {
        let mut s = Summons::new();
        assert_eq!(s.judge(&start()), Ok(()));
        assert!(s.record(&start(), 0xff00_0c90, 1_000));
        assert_eq!(s.len(), 1);
        assert_eq!(s.live_of(ROTFERN), 1);
        assert_eq!(s.actor_of(ROTFERN, LOCAL), Some(0xff00_0c90));
        assert_eq!(s.caster_of(0xff00_0c90), ROTFERN);
        // the create word names the engine's reference to its caster alone
        assert_eq!(s.own_local(0xff00_0c90, ROTFERN), LOCAL);
        assert_eq!(s.own_local(0xff00_0c90, ARIADNE), 0);
        assert_eq!(s.own_local(0xff00_0c91, ROTFERN), 0);
        // and its effect to every other listener while it arrives (#111):
        // never to its caster, never for another actor, never past the
        // arrival's word
        assert_eq!(s.summon_effect(0xff00_0c90, ARIADNE, 1_000), SUMMON_FLAME_ATRONACH_POTENT);
        assert_eq!(s.summon_effect(0xff00_0c90, ARIADNE, 1_000 + ARRIVAL_WORD_MS), SUMMON_FLAME_ATRONACH_POTENT);
        assert_eq!(s.summon_effect(0xff00_0c90, ARIADNE, 1_000 + ARRIVAL_WORD_MS + 1), 0);
        assert_eq!(s.summon_effect(0xff00_0c90, ROTFERN, 1_000), 0);
        assert_eq!(s.summon_effect(0xff00_0c91, ARIADNE, 1_000), 0);
        let row = s.row(0xff00_0c90);
        assert_eq!(
            row.map(|r| (r.base, r.effect, r.spell, r.backstop_ms)),
            Some((ATRONACH_POTENT, SUMMON_FLAME_ATRONACH_POTENT, CONJURE_FLAME_ATRONACH, 1_000 + 59_500 + BACKSTOP_MS))
        );
        // a second actor for the same engine reference, or the same actor
        // twice, records nothing
        assert!(!s.record(&start(), 0xff00_0c91, 1_100));
        assert!(!s.record(&start(), 0xff00_0c90, 1_100));
        assert!(!s.record(&start(), 0, 1_100));
        // the other caster's numbers are its own
        let hers = StartFacts { caster: ARIADNE, ..start() };
        assert_eq!(s.judge(&hers), Ok(()));
    }

    #[test]
    fn a_scrolls_summon_is_recorded_like_a_learned_spells() {
        // R2 recorded unvalidated (Eli, 2026-11-16: "This is not a
        // competitive game"): the spell and the effect are the report's own
        // words, recorded for the log and the row, never judged; the storm
        // atronach scroll's cast places and binds like the learned one's
        let mut s = Summons::new();
        let scroll = StartFacts {
            spell: CONJURE_STORM_ATRONACH_SCROLL,
            effect: 0,
            ..start() };
        assert_eq!(s.judge(&scroll), Ok(()));
        assert!(s.record(&scroll, 0xff00_0c90, 1_000));
        let row = s.row(0xff00_0c90);
        assert_eq!(row.map(|r| (r.spell, r.effect)), Some((CONJURE_STORM_ATRONACH_SCROLL, 0)));
        assert_eq!(s.own_local(0xff00_0c90, ROTFERN), LOCAL);
        // a negative or non-finite time left still records: the backstop
        // clamps it (ms_of), the record holds what it was told
        let told = StartFacts { remaining_s: f32::NAN, local: LOCAL + 1, ..scroll };
        assert!(s.record(&told, 0xff00_0c91, 1_000));
        assert_eq!(s.row(0xff00_0c91).map(|r| r.backstop_ms), Some(1_000 + BACKSTOP_MS));
        let told = StartFacts { remaining_s: -3.0, local: LOCAL + 2, ..scroll };
        assert!(s.record(&told, 0xff00_0c92, 1_000));
        assert_eq!(s.row(0xff00_0c92).map(|r| r.backstop_ms), Some(1_000 + BACKSTOP_MS));
    }

    #[test]
    fn a_start_past_a_consistency_is_refused_by_the_first_it_breaks() {
        let mut s = Summons::new();
        let refused = |s: &Summons, f: StartFacts| code(s.judge(&f));
        assert_eq!(refused(&s, StartFacts { caster: 0, ..start() }), Some("E_SUMMON_CASTER"));
        assert_eq!(refused(&s, StartFacts { base_is_npc: false, ..start() }), Some("E_SUMMON_BASE"));
        assert_eq!(refused(&s, StartFacts { in_cell: false, ..start() }), Some("E_SUMMON_CELL"));
        // the order: the caster, then the engine's id, then the base, then
        // the cell
        assert_eq!(refused(&s, StartFacts { caster: 0, base_is_npc: false, in_cell: false, ..start() }), Some("E_SUMMON_CASTER"));
        assert!(s.record(&start(), 0xff00_0c90, 0));
        assert_eq!(refused(&s, start()), Some("E_SUMMON_TWICE"));
        assert_eq!(refused(&s, StartFacts { local: LOCAL + 1, base_is_npc: false, in_cell: false, ..start() }), Some("E_SUMMON_BASE"));
        // the why names the numbers
        let why = s.judge(&start()).err().map(|e| e.why);
        assert_eq!(why.as_deref(), Some("ff000001's engine reported its summon ff0008fd already, the server's ff000c90"));
        let why = Summons::new().judge(&StartFacts { base_is_npc: false, ..start() }).err().map(|e| e.why);
        assert_eq!(why.as_deref(), Some("the base 4e940 is no actor record the files hold"));
        // the spell, the effect, the distance and the time are never judged:
        // a far place, an unknown spell, a wild time all record
        let told = StartFacts { spell: 0, effect: 0, remaining_s: 9_999.0, ..StartFacts { local: LOCAL + 3, ..start() } };
        assert_eq!(s.judge(&told), Ok(()));
        assert!(s.record(&told, 0xff00_0c91, 0));
        assert_eq!(s.live_of(ROTFERN), 2);
    }

    #[test]
    fn an_end_dispels_the_casters_own_summon_and_the_fade_hands_it_to_the_core() {
        let mut s = Summons::new();
        assert!(s.record(&start(), 0xff00_0c90, 1_000));
        // another caster's end, or an id never reported, names nothing
        assert_eq!(s.end(ARIADNE, LOCAL), None);
        assert_eq!(s.end(ROTFERN, LOCAL + 1), None);
        assert_eq!(s.end(ROTFERN, LOCAL), Some(0xff00_0c90));
        // the dispel: fading for the GMST's 1.5 s, no longer live, no longer
        // the caster's to bind, a second dispel nothing
        assert!(s.dispel(0xff00_0c90, 2_000, 1_500));
        assert!(!s.dispel(0xff00_0c90, 2_100, 1_500));
        assert_eq!(s.live_of(ROTFERN), 0);
        assert_eq!(s.own_local(0xff00_0c90, ROTFERN), 0);
        // and a fading summon arrives for nobody (#111)
        assert_eq!(s.summon_effect(0xff00_0c90, ARIADNE, 2_100), 0);
        assert_eq!(s.end(ROTFERN, LOCAL), None);
        assert_eq!(s.caster_of(0xff00_0c90), ROTFERN);
        // the engine's next summon by the same number is a new one
        assert_eq!(s.judge(&start()), Ok(()));
        // the fade ends: the core deletes it, the row goes
        assert_eq!(s.tick(3_499), Due::default());
        assert_eq!(s.tick(3_500), Due { backstops: vec![], faded: vec![0xff00_0c90] });
        assert!(s.is_empty());
        assert_eq!(s.tick(3_600), Due::default());
        assert!(!s.dispel(0xff00_0c90, 3_600, 1_500));
    }

    #[test]
    fn a_summon_whose_end_never_comes_is_dispelled_by_its_backstop() {
        let mut s = Summons::new();
        assert!(s.record(&start(), 0xff00_0c90, 1_000));
        assert!(s.record(&StartFacts { local: LOCAL + 1, remaining_s: 10.0, ..start() }, 0xff00_0c91, 1_000));
        // 10 s left plus the backstop's 5 s
        assert_eq!(s.tick(15_999), Due::default());
        assert_eq!(s.tick(16_000), Due { backstops: vec![0xff00_0c91], faded: vec![] });
        // still live until the core dispels it; asked again it is due again
        assert_eq!(s.tick(16_100), Due { backstops: vec![0xff00_0c91], faded: vec![] });
        assert!(s.dispel(0xff00_0c91, 16_100, 1_500));
        assert_eq!(s.tick(17_600), Due { backstops: vec![], faded: vec![0xff00_0c91] });
        // 59.5 s left plus 5 s
        assert_eq!(s.tick(65_499), Due::default());
        assert_eq!(s.tick(65_500), Due { backstops: vec![0xff00_0c90], faded: vec![] });
        // a sweep forgets it outright
        assert!(s.forget(0xff00_0c90));
        assert!(!s.forget(0xff00_0c90));
        assert!(s.is_empty());
        assert_eq!(ms_of(f32::INFINITY), 0);
        assert_eq!(ms_of(-3.0), 0);
        assert_eq!(ms_of(1.5), 1_500);
    }
}
