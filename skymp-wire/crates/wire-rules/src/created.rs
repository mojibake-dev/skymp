//! The spell channel's first slice (thuum ADR-073 decision 13, accepted
//! 2026-11-14: "every spell's scripted placement and animation reaches every
//! screen through one typed channel, owned by the caster's game", with no
//! per-spell entry; decision 6: a created reference is its creator's; tracker
//! #54, Raise Wall). A script on a seat places a reference with PlaceAtMe on
//! that seat's own player, or on a reference the seat created before, and
//! then moves, turns, shows, hides and deletes it with the engine's own
//! natives. The seat reports each call through the one seam (ScriptCall 75)
//! as its engine ran it, naming the created reference by its own local id
//! (0xff000000 and up; a creation's `placed`), and the server maps that id to
//! its copy.
//!
//! The read behind it: x-spell-calls 20261115-034842, c1's native-frame trace
//! of one Raise Wall cast (WB_FabricateWall_Script.OnEffectStart): PlaceAtMe
//! of the wall 0x1a098529 disabled on the player, MoveTo on the wall 192
//! ahead and 288 below the player, SetAngle, Enable(false), TranslateTo to
//! the risen pose at 512 units a second, EffectShader.Play of 0x1a0735e7 on
//! the wall; OnEffectFinish's Disable(true). Each ran on a VM worker thread
//! and named its created subject by the caster's local id.
//!
//! The rule, R2 (the creator's engine computes; the server records the
//! copy's pose and state and validates consistency only, ADR-063):
//! - A creation's self is the reporter's own player or one of the reporter's
//!   own created references; any other self is the natives table's (W3's
//!   script set), never this channel's (`E_CREATED_SELF`).
//! - A base the server places as a reference of its own (an activator, a
//!   moving static, a static, a light: [`COPIED`]) gets a copy, named to
//!   every other seat as the reporter's placement (placedBy). A base whose
//!   placement is its own simulation on each engine (an explosion, a hazard:
//!   [`ONE_SHOT`]) gets none: the call itself is relayed to the self's
//!   listeners, whose engines place their own. Any other kind (an actor, an
//!   item, a container) stays on the creator's seat (`E_CREATED_KIND`), a
//!   known limit of this slice.
//! - Every later call on a created reference is its creator's alone
//!   (`E_CREATED_OWNER` for another seat's id, `E_CREATED_UNKNOWN` for an id
//!   the reporter never placed), relayed as the same native to the copy's
//!   listeners with the copy's next sequence, so each receiver's engine runs
//!   the rise at the script's own speed.
//! - The bounds are the world's, never a spell's: finite numbers, a pose
//!   inside the world, a MoveTo offset within [`MAX_OFFSET`], a TranslateTo
//!   speed within [`MAX_SPEED`], [`MAX_PER_CREATOR`] references a creator
//!   holds at once and [`MAX_CREATED`] world-wide, and a creator's budget of
//!   [`BURST`] calls then [`PER_SECOND`] a second, spent by every creation
//!   and call that reaches the rule (`E_CREATED_BOUND`).

use std::collections::HashMap;

/// The farthest a MoveTo may set a created reference from its target, on
/// any axis, units: a cell's side (4096) twice over. Raise Wall's is 288.
pub const MAX_OFFSET: f32 = 8192.0;
/// The fastest a TranslateTo may move a created reference, units a second:
/// sixteen times Raise Wall's 512, a world bound against a forged speed.
pub const MAX_SPEED: f32 = 8192.0;
/// The created references one creator holds at once: Raise Wall's cast
/// holds four (the target box, the wall, the halo, and the box's explosion
/// gets none), and a script reuses them on its next cast.
pub const MAX_PER_CREATOR: usize = 32;
/// The created references the world holds at once.
pub const MAX_CREATED: usize = 512;
/// A creator's calls in one burst: Raise Wall's cast makes fourteen in its
/// OnEffectStart (x-spell-calls 20261115-034842), four casts' worth.
pub const BURST: f64 = 64.0;
/// A creator's calls a second once its burst is spent.
pub const PER_SECOND: f64 = 32.0;
/// A position component's world bound, units (wire-validate's
/// WORLD_ABS_MAX).
pub const WORLD_ABS_MAX: f32 = 1.0e6;
/// The first local id a seat's engine hands a reference it creates at run
/// time.
pub const LOCAL_FIRST: u32 = 0xff00_0000;
/// The id the wire gives the sender's own player.
pub const OWN_PLAYER: u32 = 0x14;

/// The record types a creation copies (the effect helpers' bases of train
/// 52's Fix 2: an activator, a moving static, a static, a light).
pub const COPIED: [&str; 4] = ["ACTI", "MSTT", "STAT", "LIGH"];
/// The record types whose placement is each engine's own one-shot.
pub const ONE_SHOT: [&str; 2] = ["EXPL", "HAZD"];

/// Why a created call is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// A creation's self is no player's own and no created reference of the
    /// reporter's.
    SelfKind,
    /// A creation's base is a kind this slice does not carry.
    Kind,
    /// The subject or an argument names a created reference of another
    /// creator.
    Owner,
    /// The subject or an argument names a local id the reporter never
    /// placed, or a creation names one already placed.
    Unknown,
    /// A number past a world bound, or a creator or the world at its count.
    Bound,
}

impl Code {
    /// The refusal's name in the server's log.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::SelfKind => "E_CREATED_SELF",
            Code::Kind => "E_CREATED_KIND",
            Code::Owner => "E_CREATED_OWNER",
            Code::Unknown => "E_CREATED_UNKNOWN",
            Code::Bound => "E_CREATED_BOUND",
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

/// What a creation's self is, as the core finds it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SelfKind {
    /// The reporter's own player (0x14).
    OwnPlayer,
    /// A local id of 0xff000000 and up (the reporter's created reference, or
    /// none).
    Local(u32),
    /// Anything else.
    Other,
}

/// What a creation does.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Creation {
    /// The core places its copy of the base at its self and binds it
    /// ([`Created::bind`]).
    Copy {
        /// The self's copy, 0 for the reporter's own player (its actor).
        self_copy: u32,
    },
    /// No copy: the call is relayed to the self's listeners as itself, its
    /// self the copy named.
    OneShot {
        /// The self's copy, 0 for the reporter's own player (its actor).
        self_copy: u32,
    },
}

/// What a call on a created reference does to the copy's record, beside its
/// relay (R2: the creator's engine's word).
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Keep {
    /// Nothing kept.
    Nothing,
    /// MoveTo: the copy stands at its target's pose plus the offset (its
    /// angle the target's when the call's fifth argument, abMatchRotation,
    /// is true, which the core reads).
    MoveTo {
        /// The offset from the target, units.
        offset: [f32; 3],
    },
    /// SetPosition: the copy stands there.
    Position([f32; 3]),
    /// SetAngle: the copy's angle, degrees.
    Angle([f32; 3]),
    /// TranslateTo: the copy's pose at the move's end, its angle degrees.
    Translate {
        /// Where the move ends.
        pos: [f32; 3],
        /// The angle at the move's end, degrees.
        rot: [f32; 3],
    },
    /// Enable: the copy is enabled.
    Enable,
    /// Disable: the copy is disabled.
    Disable,
    /// Delete: the copy is deleted and its id unbound.
    Delete,
}

/// An accepted call on a created reference.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Accepted {
    /// The copy the call is on.
    pub copy: u32,
    /// The copy's next sequence, the receivers' order.
    pub seq: u32,
    /// What the copy's record keeps.
    pub keep: Keep,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Entry {
    creator: u32,
    local: u32,
    copy: u32,
    seq: u32,
}

/// The created references the server holds, by their creator's local id,
/// and each creator's budget (its tokens and when they were last filled, on
/// the core's monotonic clock).
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Created {
    entries: Vec<Entry>,
    by_copy: HashMap<u32, usize>,
    budgets: HashMap<u32, (f64, u64)>,
}

fn finite_in_world(v: &[f32]) -> bool {
    v.iter().all(|x| x.is_finite() && x.abs() <= WORLD_ABS_MAX)
}

/// A call's numbers: a native's numeric arguments in order, as the engine's
/// Papyrus floats (32 bits; the wire carries JSON's doubles).
#[allow(clippy::as_conversions)]
fn numbers(args: &[Option<f64>]) -> Vec<f32> {
    args.iter().map(|a| a.map_or(f32::NAN, |v| v as f32)).collect()
}

impl Created {
    /// An empty table.
    #[must_use]
    pub fn new() -> Self {
        Self::default()
    }

    /// The references the table holds.
    #[must_use]
    pub fn len(&self) -> usize {
        self.entries.len()
    }

    /// Whether the table holds none.
    #[must_use]
    pub fn is_empty(&self) -> bool {
        self.entries.is_empty()
    }

    /// Spend one of `creator`'s tokens at `now_ms`.
    ///
    /// # Errors
    /// `E_CREATED_BOUND` when its budget is spent.
    pub fn spend(&mut self, creator: u32, now_ms: u64) -> Result<(), Refused> {
        let (tokens, at) = self.budgets.entry(creator).or_insert((BURST, now_ms));
        #[allow(clippy::as_conversions)]
        let filled = (*tokens + now_ms.saturating_sub(*at) as f64 / 1000.0 * PER_SECOND).min(BURST);
        *at = now_ms;
        if filled < 1.0 {
            *tokens = filled;
            return refuse(Code::Bound, format!("{creator:x}'s budget of {BURST} calls then {PER_SECOND} a second is spent"));
        }
        *tokens = filled - 1.0;
        Ok(())
    }

    fn find(&self, creator: u32, local: u32) -> Option<usize> {
        self.entries.iter().position(|e| e.creator == creator && e.local == local)
    }

    /// The copy of `creator`'s local id, if it placed one.
    #[must_use]
    pub fn resolve(&self, creator: u32, local: u32) -> Option<u32> {
        self.find(creator, local).and_then(|i| self.entries.get(i)).map(|e| e.copy)
    }

    /// Whose a copy is: its creator, if the table holds it.
    #[must_use]
    pub fn creator_of(&self, copy: u32) -> Option<u32> {
        self.by_copy.get(&copy).and_then(|&i| self.entries.get(i)).map(|e| e.creator)
    }

    /// A local id named by a call: the reporter's own reference's copy, or a
    /// refusal naming why.
    ///
    /// # Errors
    /// `E_CREATED_UNKNOWN` for an id the reporter never placed.
    pub fn local(&self, creator: u32, local: u32) -> Result<u32, Refused> {
        match self.resolve(creator, local) {
            Some(copy) => Ok(copy),
            None => refuse(Code::Unknown, format!("{local:x} is no reference {creator:x}'s game placed through this channel")),
        }
    }

    /// The judgment of a creation: `creator`'s PlaceAtMe or PlaceActorAtMe on
    /// `self_kind` of a base of `record_type`, returning `placed`.
    ///
    /// # Errors
    /// `E_CREATED_SELF`, `E_CREATED_KIND`, `E_CREATED_UNKNOWN` (a `placed`
    /// already bound, or one below the local range), `E_CREATED_BOUND`.
    pub fn create(&self, creator: u32, self_kind: SelfKind, record_type: &str, placed: u32) -> Result<Creation, Refused> {
        let self_copy = match self_kind {
            SelfKind::OwnPlayer => 0,
            SelfKind::Local(local) => self.local(creator, local)?,
            SelfKind::Other => {
                return refuse(
                    Code::SelfKind,
                    "the self is neither the reporter's own player nor a reference its game created: a placement on another object is the table's (W3)".to_owned(),
                );
            }
        };
        if placed < LOCAL_FIRST {
            return refuse(Code::Unknown, format!("the result {placed:x} is below the created range"));
        }
        if self.find(creator, placed).is_some() {
            return refuse(Code::Unknown, format!("{placed:x} is placed already"));
        }
        if ONE_SHOT.contains(&record_type) {
            return Ok(Creation::OneShot { self_copy });
        }
        if !COPIED.contains(&record_type) {
            return refuse(
                Code::Kind,
                format!("a {record_type} is no activator, moving static, static or light, nor an explosion or a hazard: it stays on its creator's seat in this slice"),
            );
        }
        if self.entries.iter().filter(|e| e.creator == creator).count() >= MAX_PER_CREATOR {
            return refuse(Code::Bound, format!("{creator:x} holds {MAX_PER_CREATOR} created references already"));
        }
        if self.entries.len() >= MAX_CREATED {
            return refuse(Code::Bound, format!("the world holds {MAX_CREATED} created references already"));
        }
        Ok(Creation::Copy { self_copy })
    }

    /// The core placed `copy` for `creator`'s `local`: later calls naming
    /// `local` are on `copy`. False for a pair or a copy bound already.
    pub fn bind(&mut self, creator: u32, local: u32, copy: u32) -> bool {
        if copy == 0 || self.find(creator, local).is_some() || self.by_copy.contains_key(&copy) {
            return false;
        }
        self.by_copy.insert(copy, self.entries.len());
        self.entries.push(Entry { creator, local, copy, seq: 0 });
        true
    }

    /// The judgment of `creator`'s call `func` on its local id `local` with
    /// the call's numeric arguments in order (`None` for an argument that is
    /// no number).
    ///
    /// # Errors
    /// `E_CREATED_UNKNOWN`, `E_CREATED_BOUND` (a number past a world bound).
    pub fn call(&mut self, creator: u32, local: u32, func: &str, nums: &[Option<f64>]) -> Result<Accepted, Refused> {
        let keep = keep_of(func, nums)?;
        let Some(entry) = self.entries.iter_mut().find(|e| e.creator == creator && e.local == local) else {
            return refuse(Code::Unknown, format!("{local:x} is no reference {creator:x}'s game placed through this channel"));
        };
        entry.seq = entry.seq.wrapping_add(1);
        Ok(Accepted { copy: entry.copy, seq: entry.seq, keep })
    }

    /// The copy fell (a Delete, the core's own removal): its id unbound.
    pub fn forget_copy(&mut self, copy: u32) -> bool {
        let Some(i) = self.by_copy.remove(&copy) else {
            return false;
        };
        self.entries.swap_remove(i);
        if let Some(moved) = self.entries.get(i) {
            self.by_copy.insert(moved.copy, i);
        }
        true
    }

    /// The creator left: every copy it holds, unbound, for the core to
    /// delete (its game's references fell with it).
    pub fn forget_creator(&mut self, creator: u32) -> Vec<u32> {
        self.budgets.remove(&creator);
        let copies: Vec<u32> = self.entries.iter().filter(|e| e.creator == creator).map(|e| e.copy).collect();
        for &copy in &copies {
            self.forget_copy(copy);
        }
        copies
    }
}

/// What a native does to the copy's record, and its world bounds. The
/// natives are the engine's own (FunctionsDump.txt's ObjectReference rows),
/// never a spell's: any other call is relayed and keeps nothing.
fn keep_of(func: &str, nums: &[Option<f64>]) -> Result<Keep, Refused> {
    let f = func.to_ascii_lowercase();
    let n = numbers(nums);
    let at = |i: usize| n.get(i).copied().unwrap_or(f32::NAN);
    let bound = |what: &str| refuse(Code::Bound, format!("{func}'s {what} is past a world bound or no number"));
    match f.as_str() {
        // MoveTo(akTarget, afXOffset, afYOffset, afZOffset, abMatchRotation):
        // the target is no number, the offsets are 1 to 3
        "moveto" => {
            let offset = [at(1), at(2), at(3)];
            if !offset.iter().all(|v| v.is_finite() && v.abs() <= MAX_OFFSET) {
                return bound("offset");
            }
            Ok(Keep::MoveTo { offset })
        }
        "setposition" => {
            let pos = [at(0), at(1), at(2)];
            if !finite_in_world(&pos) {
                return bound("position");
            }
            Ok(Keep::Position(pos))
        }
        "setangle" => {
            let rot = [at(0), at(1), at(2)];
            if !rot.iter().all(|v| v.is_finite()) {
                return bound("angle");
            }
            Ok(Keep::Angle(rot))
        }
        // TranslateTo(afX, afY, afZ, afXAngle, afYAngle, afZAngle, afSpeed,
        // afMaxRotationSpeed)
        "translateto" => {
            let pos = [at(0), at(1), at(2)];
            let rot = [at(3), at(4), at(5)];
            let speed = at(6);
            let turn = n.get(7).copied().unwrap_or(0.0);
            if !finite_in_world(&pos) || !rot.iter().all(|v| v.is_finite()) {
                return bound("pose");
            }
            if !(speed.is_finite() && speed > 0.0 && speed <= MAX_SPEED && turn.is_finite()) {
                return bound("speed");
            }
            Ok(Keep::Translate { pos, rot })
        }
        "enable" | "enablenowait" => Ok(Keep::Enable),
        "disable" | "disablenowait" => Ok(Keep::Disable),
        "delete" => Ok(Keep::Delete),
        _ => {
            if !n.iter().all(|v| v.is_nan() || v.is_finite()) {
                return bound("number");
            }
            Ok(Keep::Nothing)
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const ROTFERN: u32 = 0xff00_0000;
    const ARIADNE: u32 = 0xff00_0001;
    // the caster's local ids in x-spell-calls 20261115-034842
    const WALL: u32 = 0xff00_08fa;
    const BOX: u32 = 0xff00_08a2;
    const HALO: u32 = 0xff00_0996;

    fn some(v: &[f64]) -> Vec<Option<f64>> {
        v.iter().copied().map(Some).collect()
    }

    #[test]
    fn raise_walls_cast_places_three_copies_and_its_explosion_on_each_engine() {
        let mut t = Created::new();
        // the target box and the wall on the player, the explosion and the
        // halo on the box (WB_FabricateWall_Script.OnEffectStart's order)
        assert_eq!(t.create(ROTFERN, SelfKind::OwnPlayer, "ACTI", BOX), Ok(Creation::Copy { self_copy: 0 }));
        assert!(t.bind(ROTFERN, BOX, 0xff00_0004));
        assert_eq!(t.create(ROTFERN, SelfKind::OwnPlayer, "MSTT", WALL), Ok(Creation::Copy { self_copy: 0 }));
        assert!(t.bind(ROTFERN, WALL, 0xff00_0005));
        assert_eq!(t.create(ROTFERN, SelfKind::Local(BOX), "EXPL", 0xff00_0990), Ok(Creation::OneShot { self_copy: 0xff00_0004 }));
        assert_eq!(t.create(ROTFERN, SelfKind::Local(BOX), "ACTI", HALO), Ok(Creation::Copy { self_copy: 0xff00_0004 }));
        assert!(t.bind(ROTFERN, HALO, 0xff00_0006));
        assert_eq!(t.len(), 3);
        assert_eq!(t.resolve(ROTFERN, WALL), Some(0xff00_0005));
        assert_eq!(t.creator_of(0xff00_0006), Some(ROTFERN));
    }

    #[test]
    fn the_walls_calls_keep_its_pose_and_take_its_sequence() {
        let mut t = Created::new();
        assert!(t.bind(ROTFERN, WALL, 0xff00_0005));
        let moved = t.call(ROTFERN, WALL, "MoveTo", &[None, Some(0.0), Some(192.0), Some(-288.0), None]);
        assert_eq!(moved.map(|a| (a.copy, a.seq, a.keep)), Ok((0xff00_0005, 1, Keep::MoveTo { offset: [0.0, 192.0, -288.0] })));
        assert_eq!(t.call(ROTFERN, WALL, "SetAngle", &some(&[0.0, 0.0, 0.0])).map(|a| (a.seq, a.keep)), Ok((2, Keep::Angle([0.0, 0.0, 0.0]))));
        assert_eq!(t.call(ROTFERN, WALL, "Enable", &[None]).map(|a| a.keep), Ok(Keep::Enable));
        let rise = t.call(ROTFERN, WALL, "TranslateTo", &some(&[133_855.4, -61_303.94, 14_628.6, 0.0, 0.0, 0.0, 512.0, 0.0]));
        assert!(matches!(rise, Ok(Accepted { seq: 4, keep: Keep::Translate { .. }, .. })));
        assert_eq!(t.call(ROTFERN, WALL, "Disable", &[None]).map(|a| a.keep), Ok(Keep::Disable));
        assert_eq!(t.call(ROTFERN, WALL, "Delete", &[]).map(|a| a.keep), Ok(Keep::Delete));
    }

    #[test]
    fn a_created_reference_is_its_creators_alone() {
        let mut t = Created::new();
        assert!(t.bind(ROTFERN, WALL, 0xff00_0005));
        // another seat naming the same local number names nothing of its own
        assert_eq!(t.call(ARIADNE, WALL, "Enable", &[None]).map_err(|r| r.code), Err(Code::Unknown));
        assert_eq!(t.local(ARIADNE, WALL).map_err(|r| r.code), Err(Code::Unknown));
        assert_eq!(t.create(ARIADNE, SelfKind::Local(WALL), "ACTI", HALO).map_err(|r| r.code), Err(Code::Unknown));
        // a self that is no player's own and no created reference
        assert_eq!(t.create(ROTFERN, SelfKind::Other, "ACTI", HALO).map_err(|r| r.code), Err(Code::SelfKind));
        // a result placed twice, or below the range
        assert_eq!(t.create(ROTFERN, SelfKind::OwnPlayer, "ACTI", WALL).map_err(|r| r.code), Err(Code::Unknown));
        assert_eq!(t.create(ROTFERN, SelfKind::OwnPlayer, "ACTI", 0x1a09_8529).map_err(|r| r.code), Err(Code::Unknown));
        assert!(!t.bind(ROTFERN, WALL, 0xff00_0009), "a pair bound once");
        assert!(!t.bind(ARIADNE, HALO, 0xff00_0005), "a copy bound once");
    }

    #[test]
    fn an_actor_or_an_item_stays_on_its_creators_seat() {
        let t = Created::new();
        for kind in ["NPC_", "MISC", "WEAP", "CONT", "DOOR"] {
            assert_eq!(t.create(ROTFERN, SelfKind::OwnPlayer, kind, WALL).map_err(|r| r.code), Err(Code::Kind), "{kind}");
        }
        assert_eq!(t.create(ROTFERN, SelfKind::OwnPlayer, "HAZD", WALL), Ok(Creation::OneShot { self_copy: 0 }));
    }

    #[test]
    fn the_bounds_are_the_worlds_never_a_spells() {
        let mut t = Created::new();
        assert!(t.bind(ROTFERN, WALL, 0xff00_0005));
        let code = |r: Result<Accepted, Refused>| r.map(|_| ()).map_err(|r| r.code);
        assert_eq!(code(t.call(ROTFERN, WALL, "MoveTo", &[None, Some(0.0), Some(9000.0), Some(0.0), None])), Err(Code::Bound));
        assert_eq!(code(t.call(ROTFERN, WALL, "MoveTo", &[None, Some(f64::NAN), Some(0.0), Some(0.0), None])), Err(Code::Bound));
        assert_eq!(code(t.call(ROTFERN, WALL, "TranslateTo", &some(&[0.0, 0.0, 2.0e6, 0.0, 0.0, 0.0, 512.0, 0.0]))), Err(Code::Bound));
        assert_eq!(code(t.call(ROTFERN, WALL, "TranslateTo", &some(&[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 9000.0, 0.0]))), Err(Code::Bound));
        assert_eq!(code(t.call(ROTFERN, WALL, "TranslateTo", &some(&[0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]))), Err(Code::Bound));
        assert_eq!(code(t.call(ROTFERN, WALL, "SetPosition", &some(&[0.0, 1.0e7, 0.0]))), Err(Code::Bound));
        assert_eq!(code(t.call(ROTFERN, WALL, "SetAngle", &some(&[f64::INFINITY, 0.0, 0.0]))), Err(Code::Bound));
        // a creator at its count, then the world's
        let mut full = Created::new();
        for i in 0..u32::try_from(MAX_PER_CREATOR).unwrap_or(0) {
            assert!(full.bind(ROTFERN, LOCAL_FIRST + i, 0xff10_0000 + i));
        }
        assert_eq!(full.len(), MAX_PER_CREATOR);
        assert_eq!(full.create(ROTFERN, SelfKind::OwnPlayer, "MSTT", 0xff20_0000).map_err(|r| r.code), Err(Code::Bound));
        assert_eq!(full.create(ARIADNE, SelfKind::OwnPlayer, "MSTT", 0xff20_0000), Ok(Creation::Copy { self_copy: 0 }));
    }

    #[test]
    fn a_creators_calls_spend_its_budget() {
        let mut t = Created::new();
        for _ in 0..64 {
            assert_eq!(t.spend(ROTFERN, 1_000), Ok(()));
        }
        assert_eq!(t.spend(ROTFERN, 1_000).map_err(|r| r.code), Err(Code::Bound));
        assert_eq!(t.spend(ARIADNE, 1_000), Ok(()), "a budget each");
        // 32 a second: a token back every 31.25 ms
        assert_eq!(t.spend(ROTFERN, 1_032), Ok(()));
        assert_eq!(t.spend(ROTFERN, 1_032).map_err(|r| r.code), Err(Code::Bound));
        // a departed creator's budget starts full again
        t.forget_creator(ROTFERN);
        assert_eq!(t.spend(ROTFERN, 1_032), Ok(()));
    }

    #[test]
    fn a_fallen_copy_and_a_departed_creator_unbind() {
        let mut t = Created::new();
        assert!(t.bind(ROTFERN, BOX, 0xff00_0004));
        assert!(t.bind(ROTFERN, WALL, 0xff00_0005));
        assert!(t.bind(ARIADNE, WALL, 0xff00_0007));
        assert!(t.forget_copy(0xff00_0004));
        assert!(!t.forget_copy(0xff00_0004));
        assert_eq!(t.resolve(ROTFERN, WALL), Some(0xff00_0005), "the moved entry is still found");
        assert_eq!(t.creator_of(0xff00_0005), Some(ROTFERN));
        assert_eq!(t.forget_creator(ROTFERN), vec![0xff00_0005]);
        assert_eq!((t.len(), t.resolve(ARIADNE, WALL)), (1, Some(0xff00_0007)));
    }
}
