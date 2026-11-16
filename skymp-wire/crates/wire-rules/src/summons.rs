//! A player's conjured summon as an owned actor shared between games (thuum
//! tracker #84, the verb `summon`; ADR-063: one owner, the caster's row;
//! ADR-073 decision 6: a created reference is its creator's, its ends
//! declared). The caster's engine spawns its summon later than the cast
//! (MagicCaster's Update, no event names the actor), so its game polls its
//! player's commanded actors and reports each new summon once (Summon,
//! MsgType 83, `ended` false) and its end once (`ended` true). The server
//! places its own actor of the reported base at the reported place, records
//! it as the caster's (cells.rs `FollowKind::Summon`: the caster's game
//! simulates it in any cell, every other game draws a puppet), and dispels it
//! at the first of its four ends: the caster's end report (the effect's
//! timer, or the engine's Finish after a death), a backstop past the
//! reported time left, the caster's departure (cells.rs `SummonGone`) and a
//! restart (swept at load).
//!
//! The read behind it: x-summon-baseline 20261116-083744 (c1 cast
//! ConjureFlameAtronachRightHand 0x100e76; its engine placed 0xff0008fd of
//! base 0x4e940, disabled and deleted by 12 s; c2 drew nothing). The spell
//! carries two summon effects gated on the Elemental Potency perk (MGEF
//! 0x1ceaa summons NPC_ 0x204c0, MGEF 0x4e945 NPC_ 0x4e940; Skyrim.esm, thuum
//! lab/esm.py on sky-srv, 2026-11-16: archetype 18 and those associated forms
//! in each MGEF's DATA, each EFIT 60 s), so nothing keys on one base: the
//! rule keys on the summon archetype and takes the base from the caster's
//! report, checked against that effect's associated form.
//!
//! The rule, R2 for the summon's existence, place and life (the caster's
//! engine computes; the server records within bounds, rule 5). A start is
//! refused, by name, when it breaks the first of these:
//! - the reporter plays an actor, which is the caster (the message names no
//!   other actor) (`E_SUMMON_CASTER`), and its local id is not one of its
//!   live summons already (`E_SUMMON_TWICE`);
//! - the spell is one the caster's record holds (`E_SUMMON_SPELL`), the
//!   effect is one of that spell's effects (`E_SUMMON_EFFECT`), its
//!   archetype is the summon's ([`SUMMON_ARCHETYPE`], `E_SUMMON_ARCHETYPE`),
//!   and its associated form is the reported base, an NPC_
//!   (`E_SUMMON_BASE`);
//! - the place is in the caster's record's worldspace or cell, within
//!   [`MAX_DISTANCE`] of it (`E_SUMMON_FAR`), in a cell the server knows
//!   (`E_SUMMON_CELL`);
//! - the time left is no more than [`DURATION_SLACK`] times the effect's
//!   record duration on the spell (`E_SUMMON_LONG`);
//! - the caster holds fewer than [`MAX_LIVE`] live summons
//!   (`E_SUMMON_FULL`).
//!
//! An end names one of the caster's own live summons by its local id, else
//! `E_SUMMON_UNKNOWN`. The table here holds each live summon until its
//! dispel's fade ends; the core asks [`Summons::tick`] for the backstops due
//! and the fades done.

use std::collections::HashMap;

/// The summon archetype: EffectSetting::Archetype::kSummonCreature
/// (CommonLibSSE-NG include/RE/E/EffectArchetypes.h:28), libespm
/// MGEF::EffectType::SummonCreature; both summon effects of
/// ConjureFlameAtronachRightHand carry it in their DATA (Skyrim.esm, lab/esm.py).
pub const SUMMON_ARCHETYPE: u32 = 18;
/// How many live summons one caster holds at once: the engine's own ceiling
/// with the Twin Souls perk (the design's bound; a third start is refused).
pub const MAX_LIVE: usize = 2;
/// The farthest a reported summon may stand from its caster's record,
/// units: two exterior cells' side, a bound that breaks nothing.
pub const MAX_DISTANCE: f32 = 8192.0;
/// How many times the effect's record duration the reported time left may
/// be: perks and dual casting lengthen the engine's own timer past the
/// record's, and four times is a bound that breaks nothing.
pub const DURATION_SLACK: f32 = 4.0;
/// How long past the reported time left the server dispels a summon whose
/// end report never came, ms.
pub const BACKSTOP_MS: u64 = 5_000;
/// The first local id a seat's engine hands a reference it creates at run
/// time.
pub const LOCAL_FIRST: u32 = 0xff00_0000;

/// Why a summon report is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// The reporter plays no actor.
    Caster,
    /// The local id is one of the caster's live summons already.
    Twice,
    /// The spell is not one the caster's record holds.
    Spell,
    /// The effect is not one of the spell's effects.
    Effect,
    /// The effect is no summon (its archetype is not [`SUMMON_ARCHETYPE`]).
    Archetype,
    /// The base is not the effect's associated form, or no NPC_.
    Base,
    /// The place is in another worldspace or cell than the caster's record,
    /// or farther than [`MAX_DISTANCE`] from it.
    Far,
    /// The place is in no cell the server knows.
    Cell,
    /// The time left is past [`DURATION_SLACK`] times the record duration.
    Long,
    /// The caster holds [`MAX_LIVE`] live summons already.
    Full,
    /// An end names no live summon of the caster's.
    Unknown,
}

impl Code {
    /// The refusal's name in the server's log.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::Caster => "E_SUMMON_CASTER",
            Code::Twice => "E_SUMMON_TWICE",
            Code::Spell => "E_SUMMON_SPELL",
            Code::Effect => "E_SUMMON_EFFECT",
            Code::Archetype => "E_SUMMON_ARCHETYPE",
            Code::Base => "E_SUMMON_BASE",
            Code::Far => "E_SUMMON_FAR",
            Code::Cell => "E_SUMMON_CELL",
            Code::Long => "E_SUMMON_LONG",
            Code::Full => "E_SUMMON_FULL",
            Code::Unknown => "E_SUMMON_UNKNOWN",
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

/// A start report and the facts the core read for it (rule 5's list).
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct StartFacts {
    /// The reporter's actor, the caster; 0 when it plays none.
    pub caster: u32,
    /// The summon's id in the caster's engine (0xff000000 and up).
    pub local: u32,
    /// The reported base.
    pub base: u32,
    /// The reported summon effect (an MGEF).
    pub effect: u32,
    /// The reported spell.
    pub spell: u32,
    /// The caster's record holds the spell (its learned spells, or its
    /// base's).
    pub spell_known: bool,
    /// The effect is one of the spell's effects.
    pub effect_on_spell: bool,
    /// The effect's archetype as its MGEF's DATA gives it; `u32::MAX` when
    /// the effect is no MGEF.
    pub archetype: u32,
    /// The effect's associated form (its MGEF's DATA), global; 0 for none.
    pub associated: u32,
    /// The base is an NPC_ record.
    pub base_is_npc: bool,
    /// The largest record duration of the spell's entries of the effect
    /// (EFIT), seconds.
    pub record_s: f32,
    /// The time the caster's engine reports left, seconds.
    pub remaining_s: f32,
    /// The reported worldspace or cell is the caster's record's.
    pub same_world: bool,
    /// The reported place's distance from the caster's record, units.
    pub distance: f32,
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
    /// Its base.
    pub base: u32,
    /// Its summon effect.
    pub effect: u32,
    /// Its spell.
    pub spell: u32,
    /// When the backstop dispels it, on the core's monotonic clock, ms.
    pub backstop_ms: u64,
    /// While its dispel fades: when the fade ends and the core deletes it.
    pub fading_until_ms: Option<u64>,
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

    /// Rule 5's checks of a start report, in the module's order; nothing is
    /// recorded.
    ///
    /// # Errors
    /// The first bound broken, by its code, with why.
    pub fn judge(&self, f: &StartFacts) -> Result<(), Refused> {
        if f.caster == 0 {
            return refuse(Code::Caster, "the sender plays no actor, so no caster stands behind the report".to_owned());
        }
        if let Some(actor) = self.actor_of(f.caster, f.local) {
            return refuse(Code::Twice, format!("{:x}'s engine reported its summon {:x} already, the server's {actor:x}", f.caster, f.local));
        }
        if !f.spell_known {
            return refuse(Code::Spell, format!("{:x}'s record holds no spell {:x}", f.caster, f.spell));
        }
        if !f.effect_on_spell {
            return refuse(Code::Effect, format!("{:x} is none of spell {:x}'s effects", f.effect, f.spell));
        }
        if f.archetype != SUMMON_ARCHETYPE {
            return refuse(
                Code::Archetype,
                if f.archetype == u32::MAX {
                    format!("{:x} is no magic effect the files hold", f.effect)
                } else {
                    format!("{:x}'s archetype is {}, not the summon's {SUMMON_ARCHETYPE}", f.effect, f.archetype)
                },
            );
        }
        if f.associated != f.base {
            return refuse(Code::Base, format!("{:x} summons {:x}, not the reported base {:x}", f.effect, f.associated, f.base));
        }
        if !f.base_is_npc {
            return refuse(Code::Base, format!("the base {:x} is no NPC_ record", f.base));
        }
        if !f.same_world {
            return refuse(Code::Far, format!("the place is in another worldspace or cell than {:x}'s record", f.caster));
        }
        if !(0.0..=MAX_DISTANCE).contains(&f.distance) {
            return refuse(Code::Far, format!("the place stands {:.0} units from {:x}'s record (bound {MAX_DISTANCE:.0})", f.distance, f.caster));
        }
        if !f.in_cell {
            return refuse(Code::Cell, "the place is in no cell the server's files hold".to_owned());
        }
        let bound = f.record_s * DURATION_SLACK;
        if !(f.remaining_s >= 0.0 && f.remaining_s <= bound) {
            return refuse(
                Code::Long,
                format!("{:.1} s left is past {DURATION_SLACK} times {:x}'s record duration on spell {:x} ({:.1} s)", f.remaining_s, f.effect, f.spell, f.record_s),
            );
        }
        let live = self.live_of(f.caster);
        if live >= MAX_LIVE {
            return refuse(Code::Full, format!("{:x} holds {live} live summons already (bound {MAX_LIVE})", f.caster));
        }
        Ok(())
    }

    /// The core placed `actor` for the accepted start `f` at `now_ms`: held,
    /// its backstop at the reported time left plus [`BACKSTOP_MS`]. False,
    /// and nothing recorded, when the table holds `actor` or `f`'s local id
    /// for its caster already, or the start breaks a bound (`judge`).
    pub fn record(&mut self, f: &StartFacts, actor: u32, now_ms: u64) -> bool {
        if actor == 0 || self.rows.contains_key(&actor) || self.judge(f).is_err() {
            return false;
        }
        let backstop_ms = now_ms.saturating_add(ms_of(f.remaining_s)).saturating_add(BACKSTOP_MS);
        self.rows.insert(
            actor,
            Row { actor, caster: f.caster, local: f.local, base: f.base, effect: f.effect, spell: f.spell, backstop_ms, fading_until_ms: None },
        );
        true
    }

    /// `caster`'s end report of its summon `local`: the server's actor to
    /// dispel.
    ///
    /// # Errors
    /// `E_SUMMON_UNKNOWN` when `caster` holds no live summon by that id (never
    /// reported, refused, or dispelled already).
    pub fn end(&self, caster: u32, local: u32) -> Result<u32, Refused> {
        match self.actor_of(caster, local) {
            Some(actor) => Ok(actor),
            None => refuse(Code::Unknown, format!("{:x} holds no live summon its engine numbers {local:x}", caster)),
        }
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
    const SUMMON_FLAME_ATRONACH: u32 = 0x1ceaa;
    const SUMMON_FLAME_ATRONACH_POTENT: u32 = 0x4e945;
    const ATRONACH: u32 = 0x204c0;
    const ATRONACH_POTENT: u32 = 0x4e940;
    // x-summon-baseline 20261116-083744's engine id
    const LOCAL: u32 = 0xff00_08fd;

    /// The baseline's cast, within every bound: rotfern's potent atronach
    /// 300 units ahead with 59.5 s of the effect's 60 left.
    fn start() -> StartFacts {
        StartFacts {
            caster: ROTFERN,
            local: LOCAL,
            base: ATRONACH_POTENT,
            effect: SUMMON_FLAME_ATRONACH_POTENT,
            spell: CONJURE_FLAME_ATRONACH,
            spell_known: true,
            effect_on_spell: true,
            archetype: SUMMON_ARCHETYPE,
            associated: ATRONACH_POTENT,
            base_is_npc: true,
            record_s: 60.0,
            remaining_s: 59.5,
            same_world: true,
            distance: 300.0,
            in_cell: true,
        }
    }

    fn code(r: Result<(), Refused>) -> Option<&'static str> {
        r.err().map(|e| e.code.name())
    }

    #[test]
    fn a_summon_within_every_bound_is_placed_and_held_as_its_casters() {
        let mut s = Summons::new();
        assert_eq!(s.judge(&start()), Ok(()));
        // the perk's other effect and its own base pass alike: the rule keys
        // on the archetype, never on one base
        let plain = StartFacts { base: ATRONACH, effect: SUMMON_FLAME_ATRONACH, associated: ATRONACH, local: LOCAL + 1, ..start() };
        assert_eq!(s.judge(&plain), Ok(()));
        assert!(s.record(&start(), 0xff00_0c90, 1_000));
        assert_eq!(s.len(), 1);
        assert_eq!(s.live_of(ROTFERN), 1);
        assert_eq!(s.actor_of(ROTFERN, LOCAL), Some(0xff00_0c90));
        assert_eq!(s.caster_of(0xff00_0c90), ROTFERN);
        // the create word names the engine's reference to its caster alone
        assert_eq!(s.own_local(0xff00_0c90, ROTFERN), LOCAL);
        assert_eq!(s.own_local(0xff00_0c90, ARIADNE), 0);
        assert_eq!(s.own_local(0xff00_0c91, ROTFERN), 0);
        let row = s.row(0xff00_0c90);
        assert_eq!(row.map(|r| (r.base, r.effect, r.spell, r.backstop_ms)), Some((ATRONACH_POTENT, SUMMON_FLAME_ATRONACH_POTENT, CONJURE_FLAME_ATRONACH, 1_000 + 59_500 + BACKSTOP_MS)));
        // a second actor for the same engine reference, or the same actor
        // twice, records nothing
        assert!(!s.record(&start(), 0xff00_0c91, 1_100));
        assert!(!s.record(&plain, 0xff00_0c90, 1_100));
        assert!(!s.record(&plain, 0, 1_100));
        // the Twin Souls pair
        assert!(s.record(&plain, 0xff00_0c91, 1_200));
        assert_eq!(s.live_of(ROTFERN), 2);
        // the other caster's numbers are its own
        let hers = StartFacts { caster: ARIADNE, ..start() };
        assert_eq!(s.judge(&hers), Ok(()));
    }

    #[test]
    fn a_start_past_a_bound_is_refused_by_the_first_it_breaks() {
        let mut s = Summons::new();
        let refused = |s: &Summons, f: StartFacts| code(s.judge(&f));
        assert_eq!(refused(&s, StartFacts { caster: 0, ..start() }), Some("E_SUMMON_CASTER"));
        assert_eq!(refused(&s, StartFacts { spell_known: false, ..start() }), Some("E_SUMMON_SPELL"));
        assert_eq!(refused(&s, StartFacts { effect_on_spell: false, ..start() }), Some("E_SUMMON_EFFECT"));
        // a Reanimate effect (archetype 22) and an effect no MGEF holds
        assert_eq!(refused(&s, StartFacts { archetype: 22, ..start() }), Some("E_SUMMON_ARCHETYPE"));
        assert_eq!(refused(&s, StartFacts { archetype: u32::MAX, ..start() }), Some("E_SUMMON_ARCHETYPE"));
        // the potent effect reported with the plain base, and a base that is
        // no actor
        assert_eq!(refused(&s, StartFacts { base: ATRONACH, ..start() }), Some("E_SUMMON_BASE"));
        assert_eq!(refused(&s, StartFacts { base_is_npc: false, ..start() }), Some("E_SUMMON_BASE"));
        assert_eq!(refused(&s, StartFacts { same_world: false, ..start() }), Some("E_SUMMON_FAR"));
        assert_eq!(refused(&s, StartFacts { distance: 8192.5, ..start() }), Some("E_SUMMON_FAR"));
        assert_eq!(refused(&s, StartFacts { distance: f32::NAN, ..start() }), Some("E_SUMMON_FAR"));
        assert_eq!(refused(&s, StartFacts { distance: 8192.0, ..start() }), None);
        assert_eq!(refused(&s, StartFacts { in_cell: false, ..start() }), Some("E_SUMMON_CELL"));
        // four times the record's 60 s, and no further
        assert_eq!(refused(&s, StartFacts { remaining_s: 240.0, ..start() }), None);
        assert_eq!(refused(&s, StartFacts { remaining_s: 240.5, ..start() }), Some("E_SUMMON_LONG"));
        assert_eq!(refused(&s, StartFacts { remaining_s: -1.0, ..start() }), Some("E_SUMMON_LONG"));
        assert_eq!(refused(&s, StartFacts { remaining_s: f32::NAN, ..start() }), Some("E_SUMMON_LONG"));
        assert_eq!(refused(&s, StartFacts { record_s: 0.0, remaining_s: 1.0, ..start() }), Some("E_SUMMON_LONG"));
        // the order: the spell before the place before the time
        assert_eq!(refused(&s, StartFacts { spell_known: false, distance: 9_000.0, remaining_s: 999.0, ..start() }), Some("E_SUMMON_SPELL"));
        assert_eq!(refused(&s, StartFacts { distance: 9_000.0, remaining_s: 999.0, ..start() }), Some("E_SUMMON_FAR"));
        // a third live summon, and the same engine reference twice
        assert!(s.record(&start(), 0xff00_0c90, 0));
        assert_eq!(refused(&s, start()), Some("E_SUMMON_TWICE"));
        assert!(s.record(&StartFacts { local: LOCAL + 1, ..start() }, 0xff00_0c91, 0));
        assert_eq!(refused(&s, StartFacts { local: LOCAL + 2, ..start() }), Some("E_SUMMON_FULL"));
        assert!(!s.record(&StartFacts { local: LOCAL + 2, ..start() }, 0xff00_0c92, 0));
        // the why names the numbers
        let why = s.judge(&StartFacts { local: LOCAL + 2, ..start() }).err().map(|e| e.why);
        assert_eq!(why.as_deref(), Some("ff000001 holds 2 live summons already (bound 2)"));
        let why = Summons::new().judge(&StartFacts { distance: 9_000.0, ..start() }).err().map(|e| e.why);
        assert_eq!(why.as_deref(), Some("the place stands 9000 units from ff000001's record (bound 8192)"));
        let why = Summons::new().judge(&StartFacts { base: ATRONACH, ..start() }).err().map(|e| e.why);
        assert_eq!(why.as_deref(), Some("4e945 summons 4e940, not the reported base 204c0"));
    }

    #[test]
    fn an_end_dispels_the_casters_own_summon_and_the_fade_hands_it_to_the_core() {
        let mut s = Summons::new();
        assert!(s.record(&start(), 0xff00_0c90, 1_000));
        // another caster's end, or an id never reported, names nothing
        assert_eq!(s.end(ARIADNE, LOCAL).err().map(|e| e.code), Some(Code::Unknown));
        assert_eq!(s.end(ROTFERN, LOCAL + 1).err().map(|e| e.code), Some(Code::Unknown));
        assert_eq!(s.end(ROTFERN, LOCAL), Ok(0xff00_0c90));
        // the dispel: fading for the GMST's 1.5 s, no longer live, no longer
        // the caster's to bind, a second dispel nothing
        assert!(s.dispel(0xff00_0c90, 2_000, 1_500));
        assert!(!s.dispel(0xff00_0c90, 2_100, 1_500));
        assert_eq!(s.live_of(ROTFERN), 0);
        assert_eq!(s.own_local(0xff00_0c90, ROTFERN), 0);
        assert_eq!(s.end(ROTFERN, LOCAL).err().map(|e| e.code), Some(Code::Unknown));
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
