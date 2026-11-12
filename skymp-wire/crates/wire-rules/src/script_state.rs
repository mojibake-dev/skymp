//! The script record (thuum ADR-068 rung 3, decision 5 and its amendment A;
//! docs/research/rung3-script-record-2026-11-06.md section 1(c)). The engine
//! that owns a reference reads each script attached to it at a quiescent
//! point (no running or waiting stack whose self is the object) and reports
//! the reads as ScriptState (MsgType 76); the server keeps the last record
//! beside the reference's change form, with the object's sequence and the
//! reference's last animation event at that moment (its view, amendment A),
//! and serves it as ScriptStateApply (MsgType 77) to the seat about to host
//! the reference's cell, ahead of its grant, and to a late joiner behind the
//! reference's create word. A record is never relayed live.
//!
//! This rule judges one reported record over the facts the core gathers: a
//! reference the server holds or loads (`E_SCRIPTSTATE_NO_REF`), no actor
//! (`E_SCRIPTSTATE_KIND`: an actor's scripts are its simulator's, decision
//! 6, a later rung's), and its owner, the host of the reference's cell at
//! the record's epoch. The epoch is judged first, the order ADR-066 decision
//! 4 gives every report: a stale or a future epoch is `E_SCRIPTSTATE_EPOCH`
//! whoever sends it, the current one from a seat that does not host the cell
//! `E_SCRIPTSTATE_OWNER`. One record passes while nobody hosts the cell: the
//! release read, the reads of the host that just released it, at the epoch
//! it released ([`ScriptStates::released`] notes the release, the cell's
//! next grant ends it, and from that grant the old host's epoch stands behind
//! the cell's, a stale epoch as in ADR-066). The shape (the scripts, their
//! kinds and caps) is wire-validate's; the reference's view and sequence are
//! the core's to stamp.

use std::collections::HashMap;

/// What the server holds at a reported record's reference.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RefKind {
    /// No reference the server holds or loads.
    Unknown,
    /// A reference that is no actor: a world object (a puzzle pillar, a
    /// lever), a door or a container alike.
    WorldObject,
    /// An actor, whose scripts are its simulator's (decision 6), a later
    /// rung's record.
    Actor,
}

/// What the core knows of a reported record.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Facts {
    /// The reporter's actor.
    pub sender: u32,
    /// The record's epoch: the cell's host epoch the reporter holds.
    pub epoch: u32,
    /// What the server holds at the reference.
    pub kind: RefKind,
    /// The reference's cell, 0 when the server cannot place it.
    pub cell: u32,
    /// The host of that cell, 0 while nobody hosts it.
    pub owner: u32,
    /// The cell's standing epoch: the grant's, the kept one while nobody
    /// hosts the cell.
    pub owner_epoch: u32,
}

/// Why a reported record is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// No reference the server holds or loads.
    NoRef,
    /// An actor's record.
    Kind,
    /// The reporter does not host the reference's cell, nor is its release
    /// read the one the cell's release left open.
    Owner,
    /// The record's epoch is not the cell's: behind it (a stale grant, the
    /// old host's after the next grant) or ahead of it.
    Epoch,
}

impl Code {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::NoRef => "E_SCRIPTSTATE_NO_REF",
            Code::Kind => "E_SCRIPTSTATE_KIND",
            Code::Owner => "E_SCRIPTSTATE_OWNER",
            Code::Epoch => "E_SCRIPTSTATE_EPOCH",
        }
    }
}

/// A refused record: the code and why, for the log.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Refusal {
    /// The code.
    pub code: Code,
    /// Why, in the facts' words.
    pub why: String,
}

/// The rule's word on a reported record.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    /// Recorded: the reporter hosts the reference's cell at the record's
    /// epoch.
    Accepted,
    /// Recorded: the release read, a read of the host that just released
    /// the cell, at the epoch it released, while nobody hosts the cell.
    ReleaseRead,
    /// Refused, logged by the code's name.
    Refuse(Refusal),
}

fn refuse(code: Code, why: String) -> Verdict {
    Verdict::Refuse(Refusal { code, why })
}

/// The rule's state: the release each cell last saw, until the cell's next
/// grant. Runtime only: a restart forgets every release, so a release read
/// that arrives after one is the old host's and refused, its record the last
/// one saved.
#[derive(Debug, Default)]
pub struct ScriptStates {
    /// The host that released each cell and the epoch it released.
    releases: HashMap<u32, (u32, u32)>,
}

impl ScriptStates {
    /// `player` released `cell` at `epoch` (the cells table's Release): its
    /// release read at that epoch is taken while nobody hosts the cell.
    pub fn released(&mut self, cell: u32, player: u32, epoch: u32) {
        self.releases.insert(cell, (player, epoch));
    }

    /// `cell` was granted, to any seat (the old host among them): the
    /// release read ends there, the old host's epoch behind the cell's.
    pub fn granted(&mut self, cell: u32) {
        self.releases.remove(&cell);
    }

    /// The host that released `cell` and the epoch it released, while no
    /// grant has followed.
    #[must_use]
    pub fn release_of(&self, cell: u32) -> Option<(u32, u32)> {
        self.releases.get(&cell).copied()
    }

    /// The rule on one reported record over the `facts` the core gathered:
    /// the reference, its cell, then the epoch and the owner, in that order.
    #[must_use]
    pub fn judge(&self, facts: &Facts) -> Verdict {
        match facts.kind {
            RefKind::WorldObject => {}
            RefKind::Unknown => return refuse(Code::NoRef, "the server holds no such reference".to_owned()),
            RefKind::Actor => return refuse(Code::Kind, "an actor: its scripts are its simulator's, a later rung's".to_owned()),
        }
        if facts.cell == 0 {
            return refuse(Code::Owner, "the reference's cell is unknown to the server".to_owned());
        }
        // the epoch first (ADR-066 decision 4): a record made under another
        // grant is refused whoever sends it, so the old host's record after
        // the cell's next grant is a stale epoch, never a hand-off's
        if facts.epoch != facts.owner_epoch {
            return refuse(Code::Epoch, format!("cell {:x} stands at epoch {}, the record names {}", facts.cell, facts.owner_epoch, facts.epoch));
        }
        if facts.owner == 0 {
            return match self.releases.get(&facts.cell) {
                Some(&(player, epoch)) if player == facts.sender && epoch == facts.epoch => Verdict::ReleaseRead,
                Some(&(player, epoch)) => refuse(Code::Owner, format!("nobody hosts cell {:x}; {player:x} released it at epoch {epoch}", facts.cell)),
                None => refuse(Code::Owner, format!("nobody hosts cell {:x}", facts.cell)),
            };
        }
        if facts.owner != facts.sender {
            return refuse(Code::Owner, format!("{:x} hosts cell {:x}, not the reporter", facts.owner, facts.cell));
        }
        Verdict::Accepted
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    // Bleak Falls Temple 0x371de (its puzzle pillar 0xaa8cb's cell), its
    // host's actor and a guest's
    const TEMPLE: u32 = 0x371de;
    const HOST: u32 = 0xff00_0000;
    const GUEST: u32 = 0xff00_0001;

    fn hosted() -> Facts {
        Facts { sender: HOST, epoch: 3, kind: RefKind::WorldObject, cell: TEMPLE, owner: HOST, owner_epoch: 3 }
    }

    fn refusal(code: Code, why: &str) -> Verdict {
        Verdict::Refuse(Refusal { code, why: why.to_owned() })
    }

    #[test]
    fn the_cells_host_at_its_epoch_is_recorded() {
        let s = ScriptStates::default();
        assert_eq!(s.judge(&hosted()), Verdict::Accepted);
        // as often as its reads come (the owner's sweep, each stack's end)
        assert_eq!(s.judge(&hosted()), Verdict::Accepted);
    }

    #[test]
    fn a_guest_at_the_cells_epoch_is_not_the_owner() {
        let s = ScriptStates::default();
        assert_eq!(s.judge(&Facts { sender: GUEST, ..hosted() }), refusal(Code::Owner, "ff000000 hosts cell 371de, not the reporter"));
    }

    #[test]
    fn a_stale_or_a_future_epoch_is_refused_whoever_sends_it() {
        let s = ScriptStates::default();
        assert_eq!(s.judge(&Facts { epoch: 2, ..hosted() }), refusal(Code::Epoch, "cell 371de stands at epoch 3, the record names 2"));
        assert_eq!(s.judge(&Facts { epoch: 4, ..hosted() }), refusal(Code::Epoch, "cell 371de stands at epoch 3, the record names 4"));
        // a guest holds no grant of the cell and names 0 (the client's
        // epochForActor): the epoch is judged before the owner
        assert_eq!(s.judge(&Facts { sender: GUEST, epoch: 0, ..hosted() }), refusal(Code::Epoch, "cell 371de stands at epoch 3, the record names 0"));
    }

    #[test]
    fn an_actor_no_reference_and_an_unplaced_one_are_refused() {
        let s = ScriptStates::default();
        // the temple's draugr 0x380b3: an actor's scripts are its simulator's
        assert_eq!(s.judge(&Facts { kind: RefKind::Actor, ..hosted() }), refusal(Code::Kind, "an actor: its scripts are its simulator's, a later rung's"));
        assert_eq!(s.judge(&Facts { kind: RefKind::Unknown, ..hosted() }), refusal(Code::NoRef, "the server holds no such reference"));
        assert_eq!(s.judge(&Facts { cell: 0, owner: 0, ..hosted() }), refusal(Code::Owner, "the reference's cell is unknown to the server"));
        // the kind is judged before anything of the cell
        assert_eq!(s.judge(&Facts { kind: RefKind::Actor, epoch: 9, owner: 0, ..hosted() }), refusal(Code::Kind, "an actor: its scripts are its simulator's, a later rung's"));
    }

    #[test]
    fn the_release_read_is_taken_while_nobody_hosts_the_cell() {
        let mut s = ScriptStates::default();
        // the host released the temple at epoch 3 and nobody took it: the
        // kept epoch is still 3
        let unhosted = Facts { owner: 0, ..hosted() };
        assert_eq!(s.judge(&unhosted), refusal(Code::Owner, "nobody hosts cell 371de"));
        s.released(TEMPLE, HOST, 3);
        assert_eq!(s.release_of(TEMPLE), Some((HOST, 3)));
        assert_eq!(s.judge(&unhosted), Verdict::ReleaseRead);
        // as often as the host's reads come, for any reference of the cell
        assert_eq!(s.judge(&unhosted), Verdict::ReleaseRead);
        // nobody else's, and not under another epoch
        assert_eq!(s.judge(&Facts { sender: GUEST, ..unhosted }), refusal(Code::Owner, "nobody hosts cell 371de; ff000000 released it at epoch 3"));
        assert_eq!(s.judge(&Facts { epoch: 2, ..unhosted }), refusal(Code::Epoch, "cell 371de stands at epoch 3, the record names 2"));
        // another cell's release opens nothing here
        assert_eq!(s.release_of(0x371dd), None);
        assert_eq!(s.judge(&Facts { cell: 0x371dd, ..unhosted }), refusal(Code::Owner, "nobody hosts cell 371dd"));
    }

    #[test]
    fn a_grant_ends_the_release_read() {
        let mut s = ScriptStates::default();
        s.released(TEMPLE, HOST, 3);
        // the guest's game takes the temple at epoch 4
        s.granted(TEMPLE);
        assert_eq!(s.release_of(TEMPLE), None);
        let regranted = Facts { owner: GUEST, owner_epoch: 4, ..hosted() };
        // the old host's late read is behind the cell's epoch: a stale epoch
        assert_eq!(s.judge(&regranted), refusal(Code::Epoch, "cell 371de stands at epoch 4, the record names 3"));
        // and the new host's at its own epoch is recorded
        assert_eq!(s.judge(&Facts { sender: GUEST, epoch: 4, ..regranted }), Verdict::Accepted);
        // nobody hosts it again at 4 with no release noted: nothing is open
        assert_eq!(s.judge(&Facts { owner: 0, epoch: 4, owner_epoch: 4, ..hosted() }), refusal(Code::Owner, "nobody hosts cell 371de"));
        // a grant back to the old host ends it the same way
        s.released(TEMPLE, HOST, 4);
        s.granted(TEMPLE);
        assert_eq!(s.judge(&Facts { owner_epoch: 5, ..hosted() }), refusal(Code::Epoch, "cell 371de stands at epoch 5, the record names 3"));
        assert_eq!(s.judge(&Facts { epoch: 5, owner_epoch: 5, ..hosted() }), Verdict::Accepted);
    }

    #[test]
    fn the_codes_have_their_names() {
        assert_eq!(Code::NoRef.name(), "E_SCRIPTSTATE_NO_REF");
        assert_eq!(Code::Kind.name(), "E_SCRIPTSTATE_KIND");
        assert_eq!(Code::Owner.name(), "E_SCRIPTSTATE_OWNER");
        assert_eq!(Code::Epoch.name(), "E_SCRIPTSTATE_EPOCH");
    }

    proptest! {
        // whatever the facts and the release noted, a record is taken only
        // from the cell's host at the cell's epoch, or from the host that
        // released a cell nobody hosts, at the epoch it released
        #[test]
        fn a_record_is_taken_from_its_owner_alone(
            sender in 0..4u32, epoch in 0..4u32, kind in 0..3u8, cell in 0..3u32,
            owner in 0..4u32, owner_epoch in 0..4u32, release in proptest::option::of((0..4u32, 0..4u32)),
        ) {
            let kind = match kind { 0 => RefKind::Unknown, 1 => RefKind::WorldObject, _ => RefKind::Actor };
            let mut s = ScriptStates::default();
            if let Some((player, at)) = release {
                s.released(cell, player, at);
            }
            let facts = Facts { sender, epoch, kind, cell, owner, owner_epoch };
            match s.judge(&facts) {
                Verdict::Accepted => {
                    prop_assert_eq!(kind, RefKind::WorldObject);
                    prop_assert!(cell != 0 && owner != 0 && owner == sender && epoch == owner_epoch);
                }
                Verdict::ReleaseRead => {
                    prop_assert_eq!(kind, RefKind::WorldObject);
                    prop_assert!(cell != 0 && owner == 0 && epoch == owner_epoch);
                    prop_assert_eq!(release, Some((sender, epoch)));
                }
                Verdict::Refuse(r) => {
                    let code = r.code;
                    match kind {
                        RefKind::Unknown => prop_assert_eq!(code, Code::NoRef),
                        RefKind::Actor => prop_assert_eq!(code, Code::Kind),
                        RefKind::WorldObject if cell != 0 && epoch != owner_epoch => prop_assert_eq!(code, Code::Epoch),
                        RefKind::WorldObject => prop_assert_eq!(code, Code::Owner),
                    }
                }
            }
        }
    }
}
