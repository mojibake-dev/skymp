//! The trigger entry channel (thuum docs/DECISIONS.md ADR-068's D-W1 A and
//! D-C2 A, Eli 2026-11-06; docs/research/fifty-fixes-2026-11-07.md section
//! 1(c)): a trigger volume's entry counts once, on its presser's owner (the
//! seat whose player it is, an NPC's simulator, a body's cell host), and
//! travels to the trigger script's owner. The core gathers the facts and asks
//! one of two judgments here.
//!
//! [`firing_source`] judges a trap trigger's firing: its own activation (a
//! vanilla trigger script activates itself, and the engine forwards it to the
//! trap with the trigger as the activator) or its first link (the trigger
//! activating a reference whose activation parents name it). Under D-S3 the
//! cell's host alone reported one; under D-W1 the trigger fires on the seat
//! that owns its presser, which need not host the cell. The server cannot
//! tell a trap family trigger's pair from a mechanism's: the family is the
//! platform's walk of the VM's class chain (TrapFamily.cpp), and the server
//! reads no script archive in the lab (a BSA is added only from a settings
//! `archives` key). So a firing a game that does not host the cell reports is
//! R2, the reporter's word that its engine fired the trigger, bounded by the
//! game files' link and the budget (the core's checks), the loader test and a
//! reach here: one of the reporter's actors (its player, or an actor it
//! simulates) stands within the trigger's activation reach plus
//! [`FIRING_SLACK`]. The cell's host is taken as before.
//!
//! [`entry`] judges a presser's owner's report that its actor entered or left
//! a trigger whose world script runs on the cell's host (D-C2): the trigger a
//! world object the server holds or places, the presser the reporter's own,
//! the reporter a game with the cell loaded, a host to send it to, and the
//! presser within the trigger's activation reach. Accepted, the entry goes to
//! the host alone, whose engine sends the event to the trigger's scripts with
//! the presser as akActionRef. R2: the presser's owner's engine computed the
//! entry; the server relays it and records nothing but the pairing and the
//! hold below.
//!
//! The host's own report goes back to the host (C-entry, train 53; thuum
//! lab/.cache/drafts/reads-53/r-c1-c2-c4-claw.md, R-C1). The platform's gate
//! tells an entry only when it blanked it, in a cell its seat does not host
//! (FridaHooks.cpp's presser rule), so a report from the cell's host is one
//! whose grant came while the report was in flight, and its engine never ran
//! the script. Playtest 51: rotfern's player entered MS13ArgueTriggerREF
//! 0x78b9a 430 ms before its seat hosted cell 0x133c9, the report reached the
//! server 33 ms after the grant, and the rule's old premise ("its own engine
//! ran the trigger's script", `E_ENTRY_HOST`) refused it, so the argument's
//! stage (0x38386 stage 10) waited for the next player. Every other check
//! stays.
//!
//! An enter refused only because nobody hosts its cell (`E_ENTRY_NO_HOST`:
//! before the cell's first grant, or while its host's game is gone) and that
//! passes the reach is held for the cell's next grant ([`Entries::granted`]),
//! when the core judges it again over that moment's facts; the same read: a
//! player who walks into an unhosted cell standing in a world trigger lost
//! the enter whichever message came first. The hold is bounded per reporter
//! ([`HELD_PER_REPORTER`]), per cell ([`HELD_PER_CELL`]), in total
//! ([`HELD_TOTAL`]) and in time ([`HELD_MS`],
//! [`Entries::sweep`]); a leave of a held enter drops both; a reporter's
//! disconnect drops its own ([`Entries::forget`]). Each drop has a code the
//! core logs once a minute per reporter and code (`E_ENTRY_HELD_FULL`,
//! `E_ENTRY_HELD_LEFT`, `E_ENTRY_HELD_EXPIRED`). The read's rejected
//! alternative, D-W3 B's client store keeping the owned presser's blanked
//! enter, cannot know whether the relay was taken elsewhere, so it could play
//! twice; the gate tells only this seat's own pressers and D-W3 B holds only
//! undecided ones, so this hold and that store never hold the same enter.
//!
//! A leave pairs with its enter ([`Entries`]; the builder's call on the
//! probe review of D-C2, 2026-11-07): a guest who leaves a world trigger by
//! a teleport or a door reports its OnTriggerLeave after the move, 1,500 to
//! 2,000 units off the trigger, so a leave judged by the enter's reach would
//! be refused and the host's copy would keep the presence for good (every
//! occupancy-tracking world trigger stuck). So the server keeps each
//! presser's accepted enters it has not seen left, per trigger, and a leave
//! of one of them is taken without the reach or the loader (its accepted
//! enter passed both, and a door into another cell may unload the trigger's
//! for the presser before its leave arrives), every other check kept; a
//! leave of none is `E_ENTRY_UNPAIRED`, before any other judgment. Rule 5:
//! the leave is validated by its pairing (only a presser's own accepted
//! entry can be ended), R2 beyond that. An enter keeps every check.

use std::collections::HashMap;

use crate::activation;
use crate::cells;
use crate::script_state::RefKind;
use crate::Verdict;

/// How far past the trigger's activation reach a game that does not host the
/// cell may stand from a trigger it reports fired (UNMEASURED): the
/// presser's position the server holds trails its engine's by the movement
/// reports in flight, and a link fires after its trigger by the link's delay
/// (Embershard's tripwire 0xb6d3f activates its linker after 0.5 s, XAPR;
/// thuum docs/research/trap-census-2026-11-05.md row 16).
pub const FIRING_SLACK: f32 = 512.0;

/// A refused report: the code and why, in the facts' words, for the log.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Refusal<C> {
    /// The code.
    pub code: C,
    /// Why.
    pub why: String,
}

/// What the core knows of a reported firing.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FiringFacts {
    /// The reporter's actor.
    pub reporter: u32,
    /// The cell the firing is judged in (a linked pair's target's, a
    /// trigger's own for its own activation), 0 when the server cannot place
    /// it.
    pub cell: u32,
    /// That cell's host, 0 while nobody hosts it.
    pub host: u32,
    /// The reporter's game reports the cell loaded (PartOne::GetCellLoaders).
    pub loader: bool,
    /// How far the nearest of the reporter's actors stands from the trigger,
    /// units: its player, or an actor it simulates (WorldState::HostOf).
    /// None when none stands in the trigger's cell or world, or when the
    /// server places no trigger (a marker it never loads); a negative or
    /// non-finite distance reads as none.
    pub nearest: Option<f32>,
    /// The trigger's size: the farthest point of its bounds, times its scale
    /// (the activation rule's target size).
    pub trigger_size: f32,
}

/// Whose word a taken firing is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FiringSource {
    /// The cell's host's, as before D-W1.
    Host,
    /// A game with the cell loaded, one of whose actors stands by the trigger:
    /// the presser's owner's (D-W1).
    Loader,
}

/// Why a reported firing is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FiringCode {
    /// Neither the cell's host nor a game with the cell loaded (the code D-S3
    /// used for every guest, kept for the reporter that is neither).
    NotHost,
    /// A game with the cell loaded, none of whose actors stands by the
    /// trigger.
    Far,
}

impl FiringCode {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            FiringCode::NotHost => "E_TRAP_NOT_HOST",
            FiringCode::Far => "E_TRAP_FAR",
        }
    }
}

/// The rule's word on a reported firing.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum FiringVerdict {
    /// Taken: the core records it and relays it as before.
    Taken(FiringSource),
    /// Refused, logged by the code's name.
    Refuse(Refusal<FiringCode>),
}

/// What a firing's reporter that does not host the cell is measured against:
/// the trigger's activation reach (wire-rules activation) plus
/// [`FIRING_SLACK`]. A distance that is not a finite number is outside it.
#[must_use]
pub fn firing_reach(distance: f32, trigger_size: f32) -> Verdict {
    let bound = activation::within_reach(0.0, trigger_size).bound + FIRING_SLACK;
    Verdict { allowed: distance.is_finite() && distance <= bound, bound }
}

/// A distance the core measured, or none: a negative or non-finite one is
/// none.
fn measured(distance: Option<f32>) -> Option<f32> {
    distance.filter(|d| d.is_finite() && *d >= 0.0)
}

/// The rule on one reported firing over the `facts` the core gathered: the
/// cell's host first, as before; then a game with the cell loaded, one of
/// whose actors stands within [`firing_reach`] of the trigger.
#[must_use]
pub fn firing_source(facts: &FiringFacts) -> FiringVerdict {
    let refuse = |code, why: String| FiringVerdict::Refuse(Refusal { code, why });
    if facts.host != 0 && facts.host == facts.reporter {
        return FiringVerdict::Taken(FiringSource::Host);
    }
    if facts.cell == 0 {
        return refuse(FiringCode::NotHost, "whose cell is unknown to the server".to_owned());
    }
    if !facts.loader {
        return refuse(FiringCode::NotHost, "which it neither hosts nor has loaded".to_owned());
    }
    let Some(nearest) = measured(facts.nearest) else {
        return refuse(FiringCode::Far, "none of its actors stands where the trigger does".to_owned());
    };
    let reach = firing_reach(nearest, facts.trigger_size);
    if !reach.allowed {
        return refuse(FiringCode::Far, format!("its nearest actor {nearest:.0} units off the trigger, the bound {:.0}", reach.bound));
    }
    FiringVerdict::Taken(FiringSource::Loader)
}

/// What the core knows of a presser's owner's report of a trigger entry.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct EntryFacts {
    /// The reporter's actor.
    pub reporter: u32,
    /// What the server holds or places by its record at the trigger.
    pub trigger: RefKind,
    /// The presser: the reporter's own actor for its player (the wire's
    /// 0x14), or another actor.
    pub presser: u32,
    /// The presser's owner: its seat's actor for a player's actor, an NPC's
    /// simulator (WorldState::HostOf), 0 for nobody (no actor the server
    /// holds, or one nobody simulates).
    pub presser_owner: u32,
    /// The trigger's cell, 0 when the server cannot place it.
    pub cell: u32,
    /// That cell's host, 0 while nobody hosts it.
    pub host: u32,
    /// The reporter's game reports the cell loaded (PartOne::GetCellLoaders).
    pub loader: bool,
    /// How far the presser stands from the trigger, units; none when it
    /// stands in another cell or world. A negative or non-finite distance
    /// reads as none.
    pub distance: Option<f32>,
    /// The trigger's size: the farthest point of its bounds, times its scale.
    pub trigger_size: f32,
    /// The trigger's form id, the pairing's key beside the presser's.
    pub trigger_id: u32,
    /// The report is a leave (OnTriggerLeave, the wire's entry 2), an enter
    /// otherwise.
    pub leave: bool,
}

/// Why a reported entry is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EntryCode {
    /// No world object the server holds or places by its record, or one it
    /// cannot place in a cell.
    NoRef,
    /// The trigger is an actor.
    Actor,
    /// The presser is not the reporter's own actor nor one it simulates.
    Presser,
    /// The reporter's game has not reported the trigger's cell loaded.
    NotLoader,
    /// Nobody hosts the cell: no engine runs the trigger's script (a leave;
    /// an enter is held instead, [`EntryVerdict::Held`]).
    NoHost,
    /// The presser stands outside the trigger's activation reach.
    Reach,
    /// A leave with no accepted enter of the same presser into the same
    /// trigger to end.
    Unpaired,
    /// An enter nobody hosts the cell for, not held: the cell holds
    /// [`HELD_PER_CELL`] already, every cell together [`HELD_TOTAL`], or its
    /// reporter [`HELD_PER_REPORTER`] over every cell.
    HeldFull,
    /// A leave of an enter held for its cell's grant: both go, neither having
    /// reached an engine.
    HeldLeft,
    /// A held enter whose cell had no grant within [`HELD_MS`], dropped
    /// ([`Entries::sweep`], [`Entries::granted`]); never a judgment's word.
    HeldExpired,
}

impl EntryCode {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            EntryCode::NoRef => "E_ENTRY_NO_REF",
            EntryCode::Actor => "E_ENTRY_ACTOR",
            EntryCode::Presser => "E_ENTRY_PRESSER",
            EntryCode::NotLoader => "E_ENTRY_NOT_LOADER",
            EntryCode::NoHost => "E_ENTRY_NO_HOST",
            EntryCode::Reach => "E_ENTRY_REACH",
            EntryCode::Unpaired => "E_ENTRY_UNPAIRED",
            EntryCode::HeldFull => "E_ENTRY_HELD_FULL",
            EntryCode::HeldLeft => "E_ENTRY_HELD_LEFT",
            EntryCode::HeldExpired => "E_ENTRY_HELD_EXPIRED",
        }
    }
}

/// The rule's word on a reported entry.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum EntryVerdict {
    /// Relayed to the cell's host alone, its actor named: the reporter's own
    /// when it hosts the cell (its gate blanked the entry).
    Relay {
        /// The host's actor.
        host: u32,
    },
    /// An enter nobody hosts the cell for, held for its grant
    /// ([`Entries::granted`]).
    Held {
        /// The trigger's cell.
        cell: u32,
    },
    /// Refused, logged by the code's name.
    Refuse(Refusal<EntryCode>),
}

/// The rule on one reported entry over the `facts` the core gathered, in
/// this order: the trigger, the presser, the loader, the host, the reach.
/// An enter's judgment, and a leave's as an enter's; [`Entries::judge`]
/// pairs a leave with its enter instead.
#[must_use]
pub fn entry(facts: &EntryFacts) -> EntryVerdict {
    match admit(facts, true) {
        Ok(host) => EntryVerdict::Relay { host },
        Err(refused) => EntryVerdict::Refuse(refused),
    }
}

/// The entry's checks in their order: the cell's host to relay to, or the
/// first refusal. An enter's are every check; a paired leave's skip the
/// loader and the reach (`enter` false), since its pairing proves the
/// presser was inside: its accepted enter passed both. A host that is the
/// reporter is relayed to like any other (C-entry: its gate blanked the
/// entry, so its engine never ran the script).
fn admit(facts: &EntryFacts, enter: bool) -> Result<u32, Refusal<EntryCode>> {
    let refuse = |code, why: String| Err(Refusal { code, why });
    match facts.trigger {
        RefKind::WorldObject => {}
        RefKind::Unknown => return refuse(EntryCode::NoRef, "no world object the server holds or places".to_owned()),
        RefKind::Actor => return refuse(EntryCode::Actor, "the trigger is an actor".to_owned()),
    }
    if facts.cell == 0 {
        return refuse(EntryCode::NoRef, "the trigger's cell is unknown to the server".to_owned());
    }
    if facts.reporter == 0 || facts.presser_owner != facts.reporter {
        let owner = if facts.presser_owner == 0 { "nobody's".to_owned() } else { format!("{:x}'s", facts.presser_owner) };
        return refuse(EntryCode::Presser, format!("the presser {:x} is {owner}, not the reporter's", facts.presser));
    }
    if enter && !facts.loader {
        return refuse(EntryCode::NotLoader, format!("its game has not loaded cell {:x}", facts.cell));
    }
    if facts.host == 0 {
        return refuse(EntryCode::NoHost, format!("nobody hosts cell {:x}: no engine runs the trigger's script", facts.cell));
    }
    if !enter {
        return Ok(facts.host);
    }
    reach(facts)?;
    Ok(facts.host)
}

/// An enter's last check: the presser within the trigger's activation reach.
fn reach(facts: &EntryFacts) -> Result<(), Refusal<EntryCode>> {
    let refuse = |why: String| Err(Refusal { code: EntryCode::Reach, why });
    let Some(distance) = measured(facts.distance) else {
        return refuse("the presser stands in another cell or world".to_owned());
    };
    let within = activation::within_reach(distance, facts.trigger_size);
    if !within.allowed {
        return refuse(format!("the presser {distance:.0} units off the trigger, the reach {:.0}", within.bound));
    }
    Ok(())
}

/// How many accepted enters one presser keeps open (UNMEASURED): a new one
/// past them drops the oldest, whose leave is then `E_ENTRY_UNPAIRED`.
pub const ENTRIES_KEPT: usize = 64;

/// How long an enter held for its cell's grant waits (UNMEASURED): twice the
/// cells rule's grace ([`cells::GRACE_MS`]), the longest a loaded cell goes
/// without a host by design (its host's game gone, the grace before the cell
/// passes on). A first grant follows the loaded report at once: playtest
/// 51's enter was told 430 ms before its seat's grant.
pub const HELD_MS: u64 = cells::GRACE_MS.saturating_mul(2);

/// How many enters one cell holds for its grant (UNMEASURED): one per
/// reporter, presser and trigger, the seats' own players and the actors they
/// simulate there; past it a new one is `E_ENTRY_HELD_FULL`.
pub const HELD_PER_CELL: usize = 16;

/// How many enters every cell together holds (UNMEASURED).
pub const HELD_TOTAL: usize = 256;

/// How many enters one reporter holds over every cell (UNMEASURED): its own
/// player and the actors it simulates, half a cell's [`HELD_PER_CELL`], so
/// one game never fills a cell's hold for the others (.claude/rules/rust.md's
/// per-connection cap); past it a new one is `E_ENTRY_HELD_FULL`.
pub const HELD_PER_REPORTER: usize = HELD_PER_CELL / 2;

/// An enter held for its cell's grant: who reported which presser into which
/// trigger in which cell, and when (the core's monotonic clock, ms).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Held {
    /// The reporter's actor.
    pub reporter: u32,
    /// The presser: the reporter's own actor for its player, or another.
    pub presser: u32,
    /// The trigger's form id.
    pub trigger_id: u32,
    /// The trigger's cell.
    pub cell: u32,
    /// When it was held.
    pub at_ms: u64,
}

impl Held {
    /// How long it has been held at `now_ms`.
    #[must_use]
    pub fn age_ms(&self, now_ms: u64) -> u64 {
        now_ms.saturating_sub(self.at_ms)
    }

    fn expired(&self, now_ms: u64) -> bool {
        self.age_ms(now_ms) > HELD_MS
    }
}

/// What a cell's grant takes out of the hold.
#[derive(Debug, Default, Clone, PartialEq, Eq)]
pub struct Granted {
    /// The enters the core judges again over the grant's facts, oldest
    /// first.
    pub replay: Vec<Held>,
    /// The enters held past [`HELD_MS`], dropped (`E_ENTRY_HELD_EXPIRED`).
    pub expired: Vec<Held>,
}

/// Each presser's accepted enters it has not left, by trigger, oldest first:
/// what a leave pairs with; and the enters held for their cell's grant, by
/// cell, oldest first. Runtime only: a restart forgets both, and a leave
/// after one is unpaired.
#[derive(Debug, Default)]
pub struct Entries {
    open: HashMap<u32, Vec<u32>>,
    held: HashMap<u32, Vec<Held>>,
}

impl Entries {
    /// The rule on one reported entry with its pairing, at `now_ms`: an enter
    /// judged by [`entry`], and kept open when relayed (a second enter of the
    /// same trigger keeps one, the newest), or held for its cell's grant
    /// when nobody hosts the cell and it passes the reach; a leave first by
    /// its pairing, `E_ENTRY_UNPAIRED` before any other judgment when it ends
    /// no open enter of its presser's into its trigger (`E_ENTRY_HELD_LEFT`
    /// when it ends its reporter's held one instead, which goes), then by
    /// every check but the loader and the reach (a presser that left by a
    /// door into another cell, whose unload the server heard first, still
    /// ends its presence on the host), relayed and closed.
    pub fn judge(&mut self, facts: &EntryFacts, now_ms: u64) -> EntryVerdict {
        if !facts.leave {
            return match entry(facts) {
                EntryVerdict::Relay { host } => {
                    self.opened(facts.presser, facts.trigger_id);
                    EntryVerdict::Relay { host }
                }
                EntryVerdict::Refuse(r) if r.code == EntryCode::NoHost => match reach(facts) {
                    Ok(()) => self.hold(facts, now_ms),
                    Err(refused) => EntryVerdict::Refuse(refused),
                },
                verdict => verdict,
            };
        }
        if !self.is_open(facts.presser, facts.trigger_id) {
            if let Some(held) = self.unhold(facts.cell, facts.reporter, facts.presser, facts.trigger_id) {
                return EntryVerdict::Refuse(Refusal {
                    code: EntryCode::HeldLeft,
                    why: format!("its enter, held {} ms for cell {:x}'s grant, goes with it", held.age_ms(now_ms), held.cell),
                });
            }
            return EntryVerdict::Refuse(Refusal {
                code: EntryCode::Unpaired,
                why: format!("no accepted entry of {:x} into {:x} to end", facts.presser, facts.trigger_id),
            });
        }
        match admit(facts, false) {
            Ok(host) => {
                self.closed(facts.presser, facts.trigger_id);
                EntryVerdict::Relay { host }
            }
            Err(refused) => EntryVerdict::Refuse(refused),
        }
    }

    /// `cell` was granted, to any seat, at `now_ms`: its held enters leave
    /// the hold, those within [`HELD_MS`] for the core to judge again over
    /// the grant's facts (a new host's own goes back to it, another's to it
    /// as a guest's), the rest dropped.
    pub fn granted(&mut self, cell: u32, now_ms: u64) -> Granted {
        let mut out = Granted::default();
        for held in self.held.remove(&cell).unwrap_or_default() {
            if held.expired(now_ms) {
                out.expired.push(held);
            } else {
                out.replay.push(held);
            }
        }
        out
    }

    /// The held enters past [`HELD_MS`] at `now_ms` go, each given back for
    /// its line (the core's once-a-second tick).
    pub fn sweep(&mut self, now_ms: u64) -> Vec<Held> {
        let mut gone = Vec::new();
        self.held.retain(|_, held| {
            let (old, kept): (Vec<Held>, Vec<Held>) = held.drain(..).partition(|h| h.expired(now_ms));
            gone.extend(old);
            *held = kept;
            !held.is_empty()
        });
        gone
    }

    /// The presser's game left (its disconnect): its open enters go, and
    /// every held enter it reported or pressed; how many held ones went.
    pub fn forget(&mut self, presser: u32) -> usize {
        self.open.remove(&presser);
        let mut forgotten = 0usize;
        self.held.retain(|_, held| {
            let before = held.len();
            held.retain(|h| h.reporter != presser && h.presser != presser);
            forgotten = forgotten.saturating_add(before.saturating_sub(held.len()));
            !held.is_empty()
        });
        forgotten
    }

    /// Whether the presser has an open enter into the trigger.
    #[must_use]
    pub fn is_open(&self, presser: u32, trigger: u32) -> bool {
        self.open.get(&presser).is_some_and(|t| t.contains(&trigger))
    }

    /// How many enters `cell` holds for its grant.
    #[must_use]
    pub fn held_in(&self, cell: u32) -> usize {
        self.held.get(&cell).map_or(0, Vec::len)
    }

    /// How many enters `reporter` holds for their grants, over every cell.
    #[must_use]
    pub fn held_by(&self, reporter: u32) -> usize {
        self.held.values().flatten().filter(|h| h.reporter == reporter).count()
    }

    /// The enter held, a second of the same reporter, presser and trigger
    /// keeping one, the newest; refused when the cell's hold, the whole
    /// hold or the reporter's share is full.
    fn hold(&mut self, facts: &EntryFacts, now_ms: u64) -> EntryVerdict {
        let full = |why: String| EntryVerdict::Refuse(Refusal { code: EntryCode::HeldFull, why });
        // the same enter again gives way to this one, so the counts below
        // always have its room
        let _ = self.unhold(facts.cell, facts.reporter, facts.presser, facts.trigger_id);
        let total = self.held.values().map(Vec::len).fold(0usize, usize::saturating_add);
        if total >= HELD_TOTAL {
            return full(format!("nobody hosts cell {:x}, and {HELD_TOTAL} enters wait for their grants already", facts.cell));
        }
        if self.held_in(facts.cell) >= HELD_PER_CELL {
            return full(format!("nobody hosts cell {:x}, which holds {HELD_PER_CELL} enters already", facts.cell));
        }
        if self.held_by(facts.reporter) >= HELD_PER_REPORTER {
            return full(format!("nobody hosts cell {:x}, and its reporter holds {HELD_PER_REPORTER} enters already", facts.cell));
        }
        self.held.entry(facts.cell).or_default().push(Held {
            reporter: facts.reporter,
            presser: facts.presser,
            trigger_id: facts.trigger_id,
            cell: facts.cell,
            at_ms: now_ms,
        });
        EntryVerdict::Held { cell: facts.cell }
    }

    fn unhold(&mut self, cell: u32, reporter: u32, presser: u32, trigger: u32) -> Option<Held> {
        let held = self.held.get_mut(&cell)?;
        let at = held.iter().position(|h| h.reporter == reporter && h.presser == presser && h.trigger_id == trigger)?;
        let taken = held.remove(at);
        if held.is_empty() {
            self.held.remove(&cell);
        }
        Some(taken)
    }

    fn opened(&mut self, presser: u32, trigger: u32) {
        let triggers = self.open.entry(presser).or_default();
        triggers.retain(|&t| t != trigger);
        triggers.push(trigger);
        if triggers.len() > ENTRIES_KEPT {
            triggers.remove(0);
        }
    }

    fn closed(&mut self, presser: u32, trigger: u32) -> bool {
        let Some(triggers) = self.open.get_mut(&presser) else {
            return false;
        };
        let before = triggers.len();
        triggers.retain(|&t| t != trigger);
        let found = triggers.len() < before;
        if triggers.is_empty() {
            self.open.remove(&presser);
        }
        found
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    // Bleak Falls Temple 0x371de, its host's actor and a guest's; the plate
    // 0x567f2 (PressurePlate) whose spike wall 0x54b15 names it, and the
    // cobweb 0x3758c (FXtriggerAmbFX), whose tear the host's engine played in
    // playtest 49 (its server log: "SetAnimationVariableFloat on 3758c in
    // cell 371de"), the entry's trigger below. The sizes are stand-ins, not
    // the records' bounds.
    const TEMPLE: u32 = 0x371de;
    const HOST: u32 = 0xff00_0000;
    const GUEST: u32 = 0xff00_0001;
    const PLATE_SIZE: f32 = 64.0;
    // the entrance web 0x37281 (unit/TriggerEntryTest's), and the Temple's
    // second web 0x3758c
    const WEB: u32 = 0x37281;
    const OTHER_WEB: u32 = 0x3758c;
    const WEB_SIZE: f32 = 100.0;
    // a draugr of the temple, the guest's by a claim (ADR-052)
    const DRAUGR: u32 = 0x380b3;
    // the core's monotonic clock at a report, ms
    const NOW: u64 = 1_000_000;
    // playtest 51's argument trigger 0x78b9a (MS13ArgueTriggerREF) in the
    // Riverwood Trader 0x133c9, its host rotfern's seat (R-C1)
    const TRADER: u32 = 0x133c9;
    const ARGUE: u32 = 0x78b9a;

    fn guest_firing() -> FiringFacts {
        FiringFacts { reporter: GUEST, cell: TEMPLE, host: HOST, loader: true, nearest: Some(40.0), trigger_size: PLATE_SIZE }
    }

    fn firing_refusal(code: FiringCode, why: &str) -> FiringVerdict {
        FiringVerdict::Refuse(Refusal { code, why: why.to_owned() })
    }

    fn guest_entry() -> EntryFacts {
        EntryFacts {
            reporter: GUEST,
            trigger: RefKind::WorldObject,
            presser: GUEST,
            presser_owner: GUEST,
            cell: TEMPLE,
            host: HOST,
            loader: true,
            distance: Some(30.0),
            trigger_size: WEB_SIZE,
            trigger_id: WEB,
            leave: false,
        }
    }

    fn entry_refusal(code: EntryCode, why: &str) -> EntryVerdict {
        EntryVerdict::Refuse(Refusal { code, why: why.to_owned() })
    }

    #[test]
    fn the_reach_of_a_firing_is_the_activation_reach_plus_the_slack() {
        // 180 + 16 + 256 + the trigger's 64, then 512
        assert_eq!(firing_reach(0.0, PLATE_SIZE).bound, 1028.0);
        assert!(firing_reach(1028.0, PLATE_SIZE).allowed);
        assert!(!firing_reach(1029.0, PLATE_SIZE).allowed);
        // a meaningless size adds nothing, a meaningless distance is outside
        assert_eq!(firing_reach(0.0, f32::NAN).bound, 964.0);
        assert!(!firing_reach(f32::NAN, PLATE_SIZE).allowed);
        assert!(!firing_reach(f32::INFINITY, PLATE_SIZE).allowed);
    }

    #[test]
    fn the_cells_host_is_taken_as_before() {
        let host = FiringFacts { reporter: HOST, ..guest_firing() };
        assert_eq!(firing_source(&host), FiringVerdict::Taken(FiringSource::Host));
        // wherever its actors stand and whatever it reports loaded: its
        // engine simulates the cell
        assert_eq!(firing_source(&FiringFacts { nearest: None, loader: false, ..host }), FiringVerdict::Taken(FiringSource::Host));
    }

    #[test]
    fn a_loader_whose_actor_stands_by_the_trigger_fired_it() {
        // the guest pressed the plate in the host's cell (D-W1)
        assert_eq!(firing_source(&guest_firing()), FiringVerdict::Taken(FiringSource::Loader));
        // nobody hosts the cell: the presser's owner fired it all the same
        assert_eq!(firing_source(&FiringFacts { host: 0, ..guest_firing() }), FiringVerdict::Taken(FiringSource::Loader));
        // at the bound's edge
        assert_eq!(firing_source(&FiringFacts { nearest: Some(1028.0), ..guest_firing() }), FiringVerdict::Taken(FiringSource::Loader));
    }

    #[test]
    fn a_game_that_neither_hosts_nor_loads_the_cell_is_refused() {
        assert_eq!(firing_source(&FiringFacts { loader: false, ..guest_firing() }), firing_refusal(FiringCode::NotHost, "which it neither hosts nor has loaded"));
        assert_eq!(firing_source(&FiringFacts { cell: 0, host: 0, ..guest_firing() }), firing_refusal(FiringCode::NotHost, "whose cell is unknown to the server"));
        // nobody hosting is no host of the reporter's: an actor 0 is nobody
        assert_eq!(firing_source(&FiringFacts { reporter: 0, host: 0, loader: false, ..guest_firing() }), firing_refusal(FiringCode::NotHost, "which it neither hosts nor has loaded"));
    }

    #[test]
    fn a_loader_with_no_actor_by_the_trigger_is_far() {
        assert_eq!(firing_source(&FiringFacts { nearest: Some(1029.0), ..guest_firing() }), firing_refusal(FiringCode::Far, "its nearest actor 1029 units off the trigger, the bound 1028"));
        // the portcullis's side of the temple, some five thousand units off
        assert_eq!(firing_source(&FiringFacts { nearest: Some(5281.4), ..guest_firing() }), firing_refusal(FiringCode::Far, "its nearest actor 5281 units off the trigger, the bound 1028"));
        // none of its actors in the trigger's cell or world, or a trigger the
        // server never places (a marker), and a distance that means nothing
        for nearest in [None, Some(-1.0), Some(f32::NAN), Some(f32::INFINITY)] {
            assert_eq!(firing_source(&FiringFacts { nearest, ..guest_firing() }), firing_refusal(FiringCode::Far, "none of its actors stands where the trigger does"));
        }
        assert_eq!(FiringCode::Far.name(), "E_TRAP_FAR");
        assert_eq!(FiringCode::NotHost.name(), "E_TRAP_NOT_HOST");
    }

    #[test]
    fn a_guests_entry_into_a_hosted_world_trigger_goes_to_the_host() {
        // c2's own player walks into the web in the host's temple (D-C2)
        assert_eq!(entry(&guest_entry()), EntryVerdict::Relay { host: HOST });
        // an NPC the guest simulates presses it the same way
        let npc = EntryFacts { presser: DRAUGR, ..guest_entry() };
        assert_eq!(entry(&npc), EntryVerdict::Relay { host: HOST });
        // at the reach's edge: 180 + 16 + 256 + the web's 100
        assert_eq!(entry(&EntryFacts { distance: Some(552.0), ..guest_entry() }), EntryVerdict::Relay { host: HOST });
    }

    #[test]
    fn an_entry_is_refused_by_the_first_check_it_fails() {
        assert_eq!(entry(&EntryFacts { trigger: RefKind::Unknown, ..guest_entry() }), entry_refusal(EntryCode::NoRef, "no world object the server holds or places"));
        assert_eq!(entry(&EntryFacts { cell: 0, ..guest_entry() }), entry_refusal(EntryCode::NoRef, "the trigger's cell is unknown to the server"));
        assert_eq!(entry(&EntryFacts { trigger: RefKind::Actor, ..guest_entry() }), entry_refusal(EntryCode::Actor, "the trigger is an actor"));
        // the host's own draugr, or nobody's
        assert_eq!(entry(&EntryFacts { presser: DRAUGR, presser_owner: HOST, ..guest_entry() }), entry_refusal(EntryCode::Presser, "the presser 380b3 is ff000000's, not the reporter's"));
        assert_eq!(entry(&EntryFacts { presser: DRAUGR, presser_owner: 0, ..guest_entry() }), entry_refusal(EntryCode::Presser, "the presser 380b3 is nobody's, not the reporter's"));
        assert_eq!(entry(&EntryFacts { loader: false, ..guest_entry() }), entry_refusal(EntryCode::NotLoader, "its game has not loaded cell 371de"));
        assert_eq!(entry(&EntryFacts { host: 0, ..guest_entry() }), entry_refusal(EntryCode::NoHost, "nobody hosts cell 371de: no engine runs the trigger's script"));
        assert_eq!(entry(&EntryFacts { distance: Some(553.0), ..guest_entry() }), entry_refusal(EntryCode::Reach, "the presser 553 units off the trigger, the reach 552"));
        assert_eq!(entry(&EntryFacts { distance: None, ..guest_entry() }), entry_refusal(EntryCode::Reach, "the presser stands in another cell or world"));
        // the trigger is judged before the presser, the presser before the
        // loader, the loader before the host
        assert_eq!(entry(&EntryFacts { trigger: RefKind::Actor, presser_owner: 0, loader: false, ..guest_entry() }), entry_refusal(EntryCode::Actor, "the trigger is an actor"));
        let unloaded_npc = EntryFacts { presser: DRAUGR, presser_owner: HOST, loader: false, ..guest_entry() };
        assert_eq!(entry(&unloaded_npc), entry_refusal(EntryCode::Presser, "the presser 380b3 is ff000000's, not the reporter's"));
        assert_eq!(entry(&EntryFacts { loader: false, host: 0, ..guest_entry() }), entry_refusal(EntryCode::NotLoader, "its game has not loaded cell 371de"));
        // a reporter of 0 owns nothing, even a presser of nobody's
        assert_eq!(entry(&EntryFacts { reporter: 0, presser: 0, presser_owner: 0, ..guest_entry() }), entry_refusal(EntryCode::Presser, "the presser 0 is nobody's, not the reporter's"));
        // the names, in the rule's order, then the pairing's and the hold's
        let names: Vec<&str> = [EntryCode::NoRef, EntryCode::Actor, EntryCode::Presser, EntryCode::NotLoader, EntryCode::NoHost, EntryCode::Reach, EntryCode::Unpaired, EntryCode::HeldFull, EntryCode::HeldLeft, EntryCode::HeldExpired].iter().map(|c| c.name()).collect();
        assert_eq!(names, ["E_ENTRY_NO_REF", "E_ENTRY_ACTOR", "E_ENTRY_PRESSER", "E_ENTRY_NOT_LOADER", "E_ENTRY_NO_HOST", "E_ENTRY_REACH", "E_ENTRY_UNPAIRED", "E_ENTRY_HELD_FULL", "E_ENTRY_HELD_LEFT", "E_ENTRY_HELD_EXPIRED"]);
    }

    fn host_entry() -> EntryFacts {
        EntryFacts { reporter: HOST, presser: HOST, presser_owner: HOST, cell: TRADER, trigger_id: ARGUE, ..guest_entry() }
    }

    #[test]
    fn the_hosts_own_entry_is_one_its_gate_blanked_and_goes_back_to_it() {
        // playtest 51: rotfern's player entered the argument's trigger while
        // its seat did not host the trader yet; the report reached the
        // server just after the grant, so the reporter is the host
        assert_eq!(entry(&host_entry()), EntryVerdict::Relay { host: HOST });
        // an NPC its game simulates the same way
        assert_eq!(entry(&EntryFacts { presser: DRAUGR, ..host_entry() }), EntryVerdict::Relay { host: HOST });
        // every other check stays: the loader, the reach, the presser
        assert_eq!(entry(&EntryFacts { loader: false, ..host_entry() }), entry_refusal(EntryCode::NotLoader, "its game has not loaded cell 133c9"));
        assert_eq!(entry(&EntryFacts { distance: Some(553.0), ..host_entry() }), entry_refusal(EntryCode::Reach, "the presser 553 units off the trigger, the reach 552"));
        assert_eq!(entry(&EntryFacts { presser: GUEST, presser_owner: GUEST, ..host_entry() }), entry_refusal(EntryCode::Presser, "the presser ff000001 is ff000001's, not the reporter's"));
        // relayed back, it opens the pairing, and its leave goes back too
        let mut e = Entries::default();
        assert_eq!(e.judge(&host_entry(), NOW), EntryVerdict::Relay { host: HOST });
        assert!(e.is_open(HOST, ARGUE));
        assert_eq!(e.judge(&EntryFacts { leave: true, distance: Some(1800.0), ..host_entry() }, NOW), EntryVerdict::Relay { host: HOST });
        assert!(!e.is_open(HOST, ARGUE));
    }

    #[test]
    fn an_enter_before_any_grant_is_held_and_the_grant_gives_it_back() {
        // the same enter arriving before the grant: nobody hosts the trader
        let mut e = Entries::default();
        let early = EntryFacts { host: 0, ..host_entry() };
        assert_eq!(e.judge(&early, NOW), EntryVerdict::Held { cell: TRADER });
        assert_eq!(e.held_in(TRADER), 1);
        // held, not open: nothing reached an engine
        assert!(!e.is_open(HOST, ARGUE));
        // the same enter again keeps one, the newest
        assert_eq!(e.judge(&early, NOW + 200), EntryVerdict::Held { cell: TRADER });
        assert_eq!(e.held_in(TRADER), 1);
        // a guest's too, into the same trigger: its own
        assert_eq!(e.judge(&EntryFacts { host: 0, cell: TRADER, trigger_id: ARGUE, ..guest_entry() }, NOW + 300), EntryVerdict::Held { cell: TRADER });
        assert_eq!(e.held_in(TRADER), 2);
        // another cell's grant takes nothing
        assert_eq!(e.granted(TEMPLE, NOW + 400), Granted::default());
        // the trader's grant gives both back, oldest first, for the core to
        // judge again over the grant's facts
        let back = e.granted(TRADER, NOW + 430);
        let held = |reporter: u32, at_ms: u64| Held { reporter, presser: reporter, trigger_id: ARGUE, cell: TRADER, at_ms };
        assert_eq!(back, Granted { replay: vec![held(HOST, NOW + 200), held(GUEST, NOW + 300)], expired: vec![] });
        assert_eq!(e.held_in(TRADER), 0);
        assert_eq!(e.granted(TRADER, NOW + 500), Granted::default());
        // judged again with the grant's host, the host's own goes back to it
        // and the guest's to the host, and each opens its pairing
        assert_eq!(e.judge(&host_entry(), NOW + 430), EntryVerdict::Relay { host: HOST });
        assert_eq!(e.judge(&EntryFacts { cell: TRADER, trigger_id: ARGUE, ..guest_entry() }, NOW + 430), EntryVerdict::Relay { host: HOST });
        assert!(e.is_open(HOST, ARGUE) && e.is_open(GUEST, ARGUE));
    }

    #[test]
    fn only_an_enter_refused_for_want_of_a_host_alone_is_held() {
        let mut e = Entries::default();
        let early = EntryFacts { host: 0, ..host_entry() };
        // every check before the host's still refuses, and the reach after it
        assert_eq!(e.judge(&EntryFacts { loader: false, ..early }, NOW), entry_refusal(EntryCode::NotLoader, "its game has not loaded cell 133c9"));
        assert_eq!(e.judge(&EntryFacts { presser_owner: GUEST, ..early }, NOW), entry_refusal(EntryCode::Presser, "the presser ff000000 is ff000001's, not the reporter's"));
        assert_eq!(e.judge(&EntryFacts { distance: Some(553.0), ..early }, NOW), entry_refusal(EntryCode::Reach, "the presser 553 units off the trigger, the reach 552"));
        assert_eq!(e.judge(&EntryFacts { distance: None, ..early }, NOW), entry_refusal(EntryCode::Reach, "the presser stands in another cell or world"));
        assert_eq!(e.held_in(TRADER), 0);
        // a paired leave nobody hosts the cell for is refused as before: its
        // enter reached an engine that no longer runs the cell
        assert_eq!(e.judge(&guest_entry(), NOW), EntryVerdict::Relay { host: HOST });
        assert_eq!(e.judge(&EntryFacts { host: 0, ..leave_of(guest_entry()) }, NOW), entry_refusal(EntryCode::NoHost, "nobody hosts cell 371de: no engine runs the trigger's script"));
        assert_eq!(e.held_in(TEMPLE), 0);
    }

    #[test]
    fn a_leave_of_a_held_enter_drops_both() {
        let mut e = Entries::default();
        let early = EntryFacts { host: 0, ..host_entry() };
        assert_eq!(e.judge(&early, NOW), EntryVerdict::Held { cell: TRADER });
        // another game's leave of the same presser ends nothing of it
        let others = EntryFacts { reporter: GUEST, presser_owner: GUEST, leave: true, ..early };
        assert_eq!(e.judge(&others, NOW + 100), entry_refusal(EntryCode::Unpaired, "no accepted entry of ff000000 into 78b9a to end"));
        assert_eq!(e.held_in(TRADER), 1);
        // its own leave, from far off: both go, nothing relayed
        let leave = EntryFacts { leave: true, distance: Some(1800.0), ..early };
        assert_eq!(e.judge(&leave, NOW + 250), entry_refusal(EntryCode::HeldLeft, "its enter, held 250 ms for cell 133c9's grant, goes with it"));
        assert_eq!(e.held_in(TRADER), 0);
        assert_eq!(e.granted(TRADER, NOW + 400), Granted::default());
        // a second leave is unpaired
        assert_eq!(e.judge(&leave, NOW + 300), entry_refusal(EntryCode::Unpaired, "no accepted entry of ff000000 into 78b9a to end"));
    }

    #[test]
    fn the_hold_is_bounded_per_cell_in_total_and_in_time() {
        let mut e = Entries::default();
        // a reporter per slot (its own player the presser), so the reporter's
        // share never decides here
        let early = |cell: u32, trigger_id: u32| {
            let seat = 0xff00_1000u32.saturating_add(cell.saturating_mul(32)).saturating_add(trigger_id);
            EntryFacts { host: 0, cell, trigger_id, reporter: seat, presser: seat, presser_owner: seat, ..host_entry() }
        };
        // a cell's sixteen, then the seventeenth refused
        for t in 1..=u32::try_from(HELD_PER_CELL).unwrap_or(16) {
            assert_eq!(e.judge(&early(TRADER, t), NOW), EntryVerdict::Held { cell: TRADER });
        }
        assert_eq!(e.judge(&early(TRADER, 100), NOW), entry_refusal(EntryCode::HeldFull, "nobody hosts cell 133c9, which holds 16 enters already"));
        assert_eq!(e.held_in(TRADER), HELD_PER_CELL);
        // the same enter again still has its room
        assert_eq!(e.judge(&early(TRADER, 1), NOW), EntryVerdict::Held { cell: TRADER });
        // every cell together: 256, then refused in a cell with room
        let cells = u32::try_from(HELD_TOTAL / HELD_PER_CELL).unwrap_or(16);
        for cell in 1..cells {
            for t in 1..=u32::try_from(HELD_PER_CELL).unwrap_or(16) {
                assert_eq!(e.judge(&early(cell, t), NOW), EntryVerdict::Held { cell });
            }
        }
        assert_eq!(e.judge(&early(TEMPLE, 1), NOW), entry_refusal(EntryCode::HeldFull, "nobody hosts cell 371de, and 256 enters wait for their grants already"));
        assert_eq!(e.held_in(TEMPLE), 0);
        // in time: kept to the window's edge, gone past it
        assert!(e.sweep(NOW + HELD_MS).is_empty());
        let gone = e.sweep(NOW + HELD_MS + 1);
        assert_eq!(gone.len(), HELD_TOTAL);
        assert!(gone.iter().all(|h| h.age_ms(NOW + HELD_MS + 1) == HELD_MS + 1));
        assert_eq!(e.held_in(TRADER), 0);
        // a grant past the window gives the expired back as dropped
        assert_eq!(e.judge(&early(TRADER, 1), NOW), EntryVerdict::Held { cell: TRADER });
        let late = e.granted(TRADER, NOW + HELD_MS + 1);
        assert!(late.replay.is_empty());
        assert_eq!(late.expired.len(), 1);
        // the window: twice the cells rule's grace
        assert_eq!(HELD_MS, 10_000);
        assert_eq!((HELD_PER_CELL, HELD_TOTAL), (16, 256));
    }

    #[test]
    fn one_reporter_holds_half_a_cell_at_most_over_every_cell() {
        let mut e = Entries::default();
        let early = |cell: u32, trigger_id: u32| EntryFacts { host: 0, cell, trigger_id, ..host_entry() };
        // the host's game: eight in the trader's, then refused there and in
        // a cell with room
        for t in 1..=u32::try_from(HELD_PER_REPORTER).unwrap_or(8) {
            assert_eq!(e.judge(&early(TRADER, t), NOW), EntryVerdict::Held { cell: TRADER });
        }
        assert_eq!(e.held_by(HOST), HELD_PER_REPORTER);
        assert_eq!(e.judge(&early(TRADER, 100), NOW), entry_refusal(EntryCode::HeldFull, "nobody hosts cell 133c9, and its reporter holds 8 enters already"));
        assert_eq!(e.judge(&early(TEMPLE, 100), NOW), entry_refusal(EntryCode::HeldFull, "nobody hosts cell 371de, and its reporter holds 8 enters already"));
        // the same enter again still has its room
        assert_eq!(e.judge(&early(TRADER, 1), NOW), EntryVerdict::Held { cell: TRADER });
        // an NPC its game simulates is its share too
        assert_eq!(e.judge(&EntryFacts { presser: DRAUGR, ..early(TRADER, 100) }, NOW), entry_refusal(EntryCode::HeldFull, "nobody hosts cell 133c9, and its reporter holds 8 enters already"));
        // another game's enter into the same cell is held
        assert_eq!(e.judge(&EntryFacts { host: 0, cell: TRADER, trigger_id: ARGUE, ..guest_entry() }, NOW), EntryVerdict::Held { cell: TRADER });
        assert_eq!((e.held_in(TRADER), e.held_by(GUEST)), (HELD_PER_REPORTER.saturating_add(1), 1));
        // a grant gives the share back
        assert_eq!(e.granted(TRADER, NOW + 100).replay.len(), HELD_PER_REPORTER.saturating_add(1));
        assert_eq!(e.held_by(HOST), 0);
        assert_eq!(HELD_PER_REPORTER, 8);
    }

    #[test]
    fn a_reporters_disconnect_forgets_what_it_held() {
        let mut e = Entries::default();
        let early = EntryFacts { host: 0, ..host_entry() };
        assert_eq!(e.judge(&early, NOW), EntryVerdict::Held { cell: TRADER });
        // an NPC its game simulates, and a guest's own player
        assert_eq!(e.judge(&EntryFacts { presser: DRAUGR, ..early }, NOW), EntryVerdict::Held { cell: TRADER });
        assert_eq!(e.judge(&EntryFacts { host: 0, cell: TRADER, ..guest_entry() }, NOW), EntryVerdict::Held { cell: TRADER });
        assert_eq!(e.forget(HOST), 2);
        assert_eq!(e.held_in(TRADER), 1);
        assert_eq!(e.forget(HOST), 0);
        assert_eq!(e.forget(GUEST), 1);
        assert_eq!(e.held_in(TRADER), 0);
    }

    fn leave_of(facts: EntryFacts) -> EntryFacts {
        EntryFacts { leave: true, ..facts }
    }

    #[test]
    fn a_leave_after_an_accepted_enter_is_relayed_without_the_reach() {
        let mut e = Entries::default();
        // the guest's player walks into the web: relayed, kept open
        assert_eq!(e.judge(&guest_entry(), NOW), EntryVerdict::Relay { host: HOST });
        assert!(e.is_open(GUEST, WEB));
        // it leaves by a door, 1,800 units off when the leave arrives, and
        // in another world at worst: the pairing takes it, not the reach
        let far = EntryFacts { distance: Some(1800.0), ..leave_of(guest_entry()) };
        assert_eq!(e.judge(&far, NOW), EntryVerdict::Relay { host: HOST });
        assert!(!e.is_open(GUEST, WEB));
        assert_eq!(e.judge(&guest_entry(), NOW), EntryVerdict::Relay { host: HOST });
        assert_eq!(e.judge(&EntryFacts { distance: None, ..leave_of(guest_entry()) }, NOW), EntryVerdict::Relay { host: HOST });
    }

    #[test]
    fn a_leave_with_no_open_enter_is_unpaired_and_a_second_leave_too() {
        let mut e = Entries::default();
        let leave = leave_of(guest_entry());
        // in reach, but never entered
        assert_eq!(e.judge(&leave, NOW), entry_refusal(EntryCode::Unpaired, "no accepted entry of ff000001 into 37281 to end"));
        assert_eq!(EntryCode::Unpaired.name(), "E_ENTRY_UNPAIRED");
        // before any other judgment: another's presser, a game without the
        // cell, an actor for a trigger
        for unpaired in [EntryFacts { presser_owner: HOST, ..leave }, EntryFacts { loader: false, ..leave }, EntryFacts { trigger: RefKind::Actor, ..leave }] {
            assert_eq!(e.judge(&unpaired, NOW), entry_refusal(EntryCode::Unpaired, "no accepted entry of ff000001 into 37281 to end"));
        }
        // entered and left once: the second leave ends nothing
        assert_eq!(e.judge(&guest_entry(), NOW), EntryVerdict::Relay { host: HOST });
        assert_eq!(e.judge(&leave, NOW), EntryVerdict::Relay { host: HOST });
        assert_eq!(e.judge(&leave, NOW), entry_refusal(EntryCode::Unpaired, "no accepted entry of ff000001 into 37281 to end"));
        // an enter the rule refused opens nothing: its leave is unpaired
        let far_enter = EntryFacts { distance: Some(553.0), ..guest_entry() };
        assert_eq!(e.judge(&far_enter, NOW), entry_refusal(EntryCode::Reach, "the presser 553 units off the trigger, the reach 552"));
        assert_eq!(e.judge(&leave, NOW), entry_refusal(EntryCode::Unpaired, "no accepted entry of ff000001 into 37281 to end"));
        // another trigger's enter pairs with that trigger's leave alone
        assert_eq!(e.judge(&EntryFacts { trigger_id: OTHER_WEB, ..guest_entry() }, NOW), EntryVerdict::Relay { host: HOST });
        assert_eq!(e.judge(&leave, NOW), entry_refusal(EntryCode::Unpaired, "no accepted entry of ff000001 into 37281 to end"));
        assert_eq!(e.judge(&EntryFacts { trigger_id: OTHER_WEB, ..leave }, NOW), EntryVerdict::Relay { host: HOST });
    }

    #[test]
    fn a_paired_leave_keeps_every_check_but_the_loader_and_the_reach() {
        let mut e = Entries::default();
        assert_eq!(e.judge(&guest_entry(), NOW), EntryVerdict::Relay { host: HOST });
        let leave = EntryFacts { distance: Some(1800.0), ..leave_of(guest_entry()) };
        // another's presser is refused by the presser, its enter kept open
        assert_eq!(e.judge(&EntryFacts { presser_owner: HOST, ..leave }, NOW), entry_refusal(EntryCode::Presser, "the presser ff000001 is ff000000's, not the reporter's"));
        assert_eq!(e.judge(&EntryFacts { host: 0, ..leave }, NOW), entry_refusal(EntryCode::NoHost, "nobody hosts cell 371de: no engine runs the trigger's script"));
        assert_eq!(e.judge(&EntryFacts { trigger: RefKind::Actor, ..leave }, NOW), entry_refusal(EntryCode::Actor, "the trigger is an actor"));
        assert!(e.is_open(GUEST, WEB));
        // the guest became the host while its leave was in flight: its gate
        // blanked the leave, so it goes back to its own game and closes
        // (C-entry)
        let hosting = EntryFacts { host: GUEST, ..leave };
        assert_eq!(e.judge(&hosting, NOW), EntryVerdict::Relay { host: GUEST });
        assert!(!e.is_open(GUEST, WEB));
        assert_eq!(e.judge(&leave, NOW), entry_refusal(EntryCode::Unpaired, "no accepted entry of ff000001 into 37281 to end"));
    }

    #[test]
    fn a_leave_after_the_presser_left_the_cell_ends_its_presence_on_the_host() {
        // the guest's player enters the web, its game with the Temple
        // loaded; it goes through a door into another cell, whose game
        // reports the Temple unloaded before its leave arrives, the presser
        // in another world or cell by then: the leave still ends the
        // presence on the host
        let mut e = Entries::default();
        assert_eq!(e.judge(&guest_entry(), NOW), EntryVerdict::Relay { host: HOST });
        let gone = EntryFacts { loader: false, distance: None, ..leave_of(guest_entry()) };
        assert_eq!(e.judge(&gone, NOW), EntryVerdict::Relay { host: HOST });
        assert!(!e.is_open(GUEST, WEB));
        // an enter keeps the loader: from a game without the cell, refused,
        // nothing opened, so its leave is unpaired
        let unloaded_enter = EntryFacts { loader: false, ..guest_entry() };
        assert_eq!(e.judge(&unloaded_enter, NOW), entry_refusal(EntryCode::NotLoader, "its game has not loaded cell 371de"));
        assert_eq!(e.judge(&gone, NOW), entry_refusal(EntryCode::Unpaired, "no accepted entry of ff000001 into 37281 to end"));
    }

    #[test]
    fn a_pressers_open_enters_go_with_its_disconnect_and_past_the_bound() {
        let mut e = Entries::default();
        assert_eq!(e.judge(&guest_entry(), NOW), EntryVerdict::Relay { host: HOST });
        e.forget(GUEST);
        assert!(!e.is_open(GUEST, WEB));
        assert_eq!(e.judge(&leave_of(guest_entry()), NOW), entry_refusal(EntryCode::Unpaired, "no accepted entry of ff000001 into 37281 to end"));
        // sixty-four open at once; the sixty-fifth drops the oldest
        let enter = |t: u32| EntryFacts { trigger_id: t, ..guest_entry() };
        for t in 1..=u32::try_from(ENTRIES_KEPT).unwrap_or(64) {
            assert_eq!(e.judge(&enter(t), NOW), EntryVerdict::Relay { host: HOST });
        }
        assert!(e.is_open(GUEST, 1));
        assert_eq!(e.judge(&enter(65), NOW), EntryVerdict::Relay { host: HOST });
        assert!(!e.is_open(GUEST, 1));
        assert!(e.is_open(GUEST, 2) && e.is_open(GUEST, 65));
        // a second enter of an open trigger makes it the newest
        assert_eq!(e.judge(&enter(2), NOW), EntryVerdict::Relay { host: HOST });
        assert_eq!(e.judge(&enter(66), NOW), EntryVerdict::Relay { host: HOST });
        assert!(e.is_open(GUEST, 2) && !e.is_open(GUEST, 3));
        // another presser's are its own
        let npc = EntryFacts { presser: DRAUGR, ..guest_entry() };
        assert_eq!(e.judge(&npc, NOW), EntryVerdict::Relay { host: HOST });
        e.forget(GUEST);
        assert!(e.is_open(DRAUGR, WEB));
        assert_eq!(ENTRIES_KEPT, 64);
    }

    fn any_distance() -> impl Strategy<Value = Option<f32>> {
        prop_oneof![
            Just(None),
            (0.0f32..4000.0).prop_map(Some),
            any::<f32>().prop_map(Some),
            Just(Some(f32::NAN)),
            Just(Some(f32::INFINITY)),
            Just(Some(-1.0)),
        ]
    }

    fn any_kind() -> impl Strategy<Value = RefKind> {
        prop_oneof![Just(RefKind::Unknown), Just(RefKind::WorldObject), Just(RefKind::Actor)]
    }

    proptest! {
        #[test]
        fn a_firing_is_the_hosts_or_a_near_loaders_and_nothing_else(
            reporter in prop_oneof![Just(0u32), Just(HOST), Just(GUEST), any::<u32>()],
            host in prop_oneof![Just(0u32), Just(HOST), Just(GUEST), any::<u32>()],
            cell in prop_oneof![Just(0u32), Just(TEMPLE), any::<u32>()],
            loader in any::<bool>(),
            nearest in any_distance(),
            trigger_size in prop_oneof![(0.0f32..2000.0), any::<f32>()],
        ) {
            let facts = FiringFacts { reporter, cell, host, loader, nearest, trigger_size };
            let near = measured(nearest).is_some_and(|d| firing_reach(d, trigger_size).allowed);
            match firing_source(&facts) {
                FiringVerdict::Taken(FiringSource::Host) => prop_assert!(host != 0 && host == reporter),
                FiringVerdict::Taken(FiringSource::Loader) => {
                    prop_assert!(host == 0 || host != reporter);
                    prop_assert!(cell != 0 && loader && near);
                }
                FiringVerdict::Refuse(r) => {
                    prop_assert!(host == 0 || host != reporter);
                    match r.code {
                        FiringCode::NotHost => prop_assert!(cell == 0 || !loader),
                        FiringCode::Far => prop_assert!(cell != 0 && loader && !near),
                    }
                    prop_assert!(!r.why.is_empty());
                }
            }
        }

        #[test]
        fn an_entry_is_relayed_only_when_every_check_holds(
            reporter in prop_oneof![Just(0u32), Just(HOST), Just(GUEST), any::<u32>()],
            trigger in any_kind(),
            presser_owner in prop_oneof![Just(0u32), Just(HOST), Just(GUEST), any::<u32>()],
            cell in prop_oneof![Just(0u32), Just(TEMPLE), any::<u32>()],
            host in prop_oneof![Just(0u32), Just(HOST), Just(GUEST), any::<u32>()],
            loader in any::<bool>(),
            distance in any_distance(),
            trigger_size in prop_oneof![(0.0f32..2000.0), any::<f32>()],
        ) {
            let facts = EntryFacts { reporter, trigger, presser: presser_owner, presser_owner, cell, host, loader, distance, trigger_size, trigger_id: WEB, leave: false };
            let within = measured(distance).is_some_and(|d| activation::within_reach(d, trigger_size).allowed);
            let checks = [
                (EntryCode::NoRef, trigger != RefKind::Unknown),
                (EntryCode::Actor, trigger != RefKind::Actor),
                (EntryCode::NoRef, cell != 0),
                (EntryCode::Presser, reporter != 0 && presser_owner == reporter),
                (EntryCode::NotLoader, loader),
                (EntryCode::NoHost, host != 0),
                (EntryCode::Reach, within),
            ];
            let first_failed = checks.iter().find(|(_, holds)| !holds).map(|(code, _)| *code);
            match entry(&facts) {
                EntryVerdict::Relay { host: to } => {
                    prop_assert_eq!(first_failed, None);
                    prop_assert_eq!(to, host);
                    // the reporter's own game when it hosts the cell (C-entry)
                    prop_assert!(to != 0);
                }
                EntryVerdict::Held { .. } => prop_assert!(false, "entry holds nothing; Entries::judge does"),
                EntryVerdict::Refuse(r) => {
                    prop_assert_eq!(Some(r.code), first_failed);
                    prop_assert!(!r.why.is_empty());
                }
            }
        }

        #[test]
        fn an_enter_is_held_only_for_want_of_a_host_and_the_hold_stays_bounded(
            steps in proptest::collection::vec((0u32..4, 0u32..24, 0u32..3, any::<bool>(), any::<bool>(), 0u64..30_000, 0u8..8, 0u32..3), 0..400),
        ) {
            // the model: per cell, the held (reporter, presser, trigger)
            // keys; four cells, three games' reports, a game's actor and two
            // NPCs as pressers, a step's clock moving on
            let cells = [TRADER, TEMPLE, 0x9731, 0xbbb1];
            let pressers = [HOST, DRAUGR, 0x380b4];
            let reporters = [HOST, GUEST, 0xff00_0002];
            let mut e = Entries::default();
            let mut now = NOW;
            for (c, t, p, hosted, near, dt, op, r) in steps {
                now = now.saturating_add(dt);
                let cell = cells.get(usize::try_from(c).unwrap_or(0)).copied().unwrap_or(TRADER);
                match op {
                    // a grant: what it gives back is that cell's alone, each
                    // within the window or dropped past it
                    0 => {
                        let back = e.granted(cell, now);
                        prop_assert!(back.replay.iter().all(|h| h.cell == cell && h.age_ms(now) <= HELD_MS));
                        prop_assert!(back.expired.iter().all(|h| h.cell == cell && h.age_ms(now) > HELD_MS));
                        prop_assert_eq!(e.held_in(cell), 0);
                    }
                    // the tick's sweep: nothing past the window stays
                    1 => {
                        prop_assert!(e.sweep(now).iter().all(|h| h.age_ms(now) > HELD_MS));
                    }
                    _ => {
                        let presser = pressers.get(usize::try_from(p).unwrap_or(0)).copied().unwrap_or(HOST);
                        let reporter = reporters.get(usize::try_from(r).unwrap_or(0)).copied().unwrap_or(HOST);
                        let facts = EntryFacts {
                            reporter,
                            presser,
                            presser_owner: reporter,
                            cell,
                            trigger_id: t,
                            host: if hosted { HOST } else { 0 },
                            distance: Some(if near { 30.0 } else { 1800.0 }),
                            ..host_entry()
                        };
                        let before = e.held_in(cell);
                        match e.judge(&facts, now) {
                            EntryVerdict::Held { cell: at } => {
                                prop_assert!(!hosted && near);
                                prop_assert_eq!(at, cell);
                                prop_assert!(e.held_in(cell) >= 1 && e.held_in(cell) <= before.saturating_add(1));
                            }
                            EntryVerdict::Relay { host } => prop_assert!(hosted && near && host == HOST),
                            EntryVerdict::Refuse(r) => match r.code {
                                EntryCode::Reach => prop_assert!(!near),
                                EntryCode::HeldFull => prop_assert!(!hosted && near),
                                other => prop_assert!(false, "an enter refused {:?}", other),
                            },
                        }
                    }
                }
                prop_assert!(cells.iter().all(|&c| e.held_in(c) <= HELD_PER_CELL));
                prop_assert!(cells.iter().map(|&c| e.held_in(c)).sum::<usize>() <= HELD_TOTAL);
                prop_assert!(reporters.iter().all(|&r| e.held_by(r) <= HELD_PER_REPORTER));
            }
        }

        #[test]
        fn a_leave_is_relayed_only_after_its_own_accepted_enter(
            steps in proptest::collection::vec((any::<bool>(), 0u32..3, 0u32..3, any::<bool>(), any::<bool>(), any::<bool>()), 0..64),
        ) {
            // the model: the open (presser, trigger) pairs; the pressers are
            // the guest's own player and two NPCs it simulates
            let pressers = [GUEST, DRAUGR, 0x380b4];
            let mut e = Entries::default();
            let mut open: Vec<(u32, u32)> = Vec::new();
            for (leave, p, t, near, owned, loaded) in steps {
                let presser = pressers.get(usize::try_from(p).unwrap_or(0)).copied().unwrap_or(GUEST);
                let trigger = WEB.saturating_add(t);
                let facts = EntryFacts {
                    presser,
                    presser_owner: if owned { GUEST } else { HOST },
                    loader: loaded,
                    distance: Some(if near { 30.0 } else { 1800.0 }),
                    trigger_id: trigger,
                    leave,
                    ..guest_entry()
                };
                let was_open = open.contains(&(presser, trigger));
                match e.judge(&facts, NOW) {
                    EntryVerdict::Relay { host } => {
                        prop_assert_eq!(host, HOST);
                        prop_assert!(owned);
                        if leave {
                            // the loader and the reach unread for a leave
                            prop_assert!(was_open);
                            open.retain(|&o| o != (presser, trigger));
                        } else {
                            prop_assert!(near && loaded);
                            if !was_open {
                                open.push((presser, trigger));
                            }
                        }
                    }
                    EntryVerdict::Refuse(r) => {
                        // a leave's pairing before any other judgment; an
                        // enter's presser, loader, then reach
                        let expected = if leave && !was_open {
                            EntryCode::Unpaired
                        } else if !owned {
                            EntryCode::Presser
                        } else if !loaded {
                            EntryCode::NotLoader
                        } else {
                            EntryCode::Reach
                        };
                        prop_assert_eq!(r.code, expected);
                        prop_assert!(!(leave && owned && was_open));
                    }
                    // the host is always there: nothing waits for a grant
                    EntryVerdict::Held { .. } => prop_assert!(false, "held with a host"),
                }
                for &(op, ot) in &open {
                    prop_assert!(e.is_open(op, ot));
                }
                prop_assert_eq!(e.is_open(presser, trigger), open.contains(&(presser, trigger)));
            }
        }
    }
}
