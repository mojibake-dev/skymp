//! A world object's graph event (ObjectAnimation, MsgType 71; thuum
//! docs/research/fifty-fixes-2026-11-07.md section 1(c), the puzzle lever's
//! motion, under ADR-063's ownership rule): who may report one on a
//! reference, and whether the server keeps it as the reference's
//! lastAnimation.
//!
//! The owner of a world object's graph event is the engine that drives its
//! graph. A furniture's (a FURN's) during a use is its user's: the user's
//! engine raises the furniture's own graph event (Bleak Falls Temple's puzzle
//! lever 0xaa8c3, a NoPlayer FURN, plays 'Lever_PlayerPush' on the puller's
//! game; HYPOTHESIS H-LF2 until x-lever-furn reads it), so the occupant the
//! server recorded at the use's activation names the owner: a player's own
//! seat, or an NPC's simulator. Anything else, and a furniture nobody uses,
//! is the cell's host's, as before (ADR-059 decision 3). R2: the owner's
//! engine computed the event; the server checks who reports it and relays.
//!
//! A furniture's use event is kept nowhere (the plan's read 8): the server
//! serves a reference's lastAnimation behind every late joiner's create
//! word, and a client re-plays a kept relay at each 3D load, so a lever back
//! at rest would push again at every join and every reload. Every other
//! world object's last event is kept as before. The core keeps the shape's
//! checks (an actor, a door, a container, the ranges) and the reference's
//! budget; this rule is the owner and the keep.

/// What the core knows of a reported graph event's reference.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Facts {
    /// The reporter's actor.
    pub reporter: u32,
    /// The reference's base is a furniture (FURN).
    pub furniture: bool,
    /// The furniture's recorded occupant, 0 for none (and for a reference
    /// that is no furniture).
    pub occupant: u32,
    /// The occupant's owner: the occupant itself for a player's actor, an
    /// NPC's simulator (WorldState::HostOf), 0 for nobody.
    pub occupant_owner: u32,
    /// The reference's cell.
    pub cell: u32,
    /// That cell's host, 0 while nobody hosts it.
    pub host: u32,
}

/// Whose word a taken event is.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Owner {
    /// The cell's host's.
    Host,
    /// A furniture's user's: its occupant's own seat, or its simulator.
    User,
}

/// Why a reported event is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// A reference no one uses, and the reporter does not host its cell.
    NotHost,
    /// A furniture in use, and the reporter is not its user's owner.
    NotUser,
}

impl Code {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::NotHost => "E_OBJANIM_NOT_HOST",
            Code::NotUser => "E_OBJANIM_NOT_USER",
        }
    }
}

/// A refused event: the code and why, for the log.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Refusal {
    /// The code.
    pub code: Code,
    /// Why, in the facts' words.
    pub why: String,
}

/// The rule's word on a reported event.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    /// Relayed to the reference's other listeners.
    Relay {
        /// Whose word it is.
        owner: Owner,
        /// Kept as the reference's lastAnimation: false for a furniture.
        keep: bool,
    },
    /// Refused, logged by the code's name.
    Refuse(Refusal),
}

/// The rule on one reported event over the `facts` the core gathered: a
/// furniture in use is its user's owner's, anything else the cell's host's.
#[must_use]
pub fn judge(facts: &Facts) -> Verdict {
    let refuse = |code, why: String| Verdict::Refuse(Refusal { code, why });
    if facts.furniture && facts.occupant != 0 {
        if facts.occupant_owner != 0 && facts.occupant_owner == facts.reporter {
            return Verdict::Relay { owner: Owner::User, keep: false };
        }
        let owner = if facts.occupant_owner == 0 { "nobody".to_owned() } else { format!("{:x}", facts.occupant_owner) };
        return refuse(Code::NotUser, format!("used by {:x}, whose owner is {owner}", facts.occupant));
    }
    if facts.host != 0 && facts.host == facts.reporter {
        return Verdict::Relay { owner: Owner::Host, keep: !facts.furniture };
    }
    refuse(Code::NotHost, "which it does not host".to_owned())
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    // Bleak Falls Temple 0x371de hosted by 0xff000000, a guest 0xff000001;
    // the puzzle lever 0xaa8c3 (FURN 0xb7db4 LeverPushAnimatingNoPlayer), the
    // scene's bandit 0x39fe1 who pulls it (unit/FurnitureOccupantTest), and
    // the portcullis 0x3b9f1, an ACTI
    const TEMPLE: u32 = 0x371de;
    const HOST: u32 = 0xff00_0000;
    const GUEST: u32 = 0xff00_0001;
    const BANDIT: u32 = 0x39fe1;

    fn guest_pull() -> Facts {
        Facts { reporter: GUEST, furniture: true, occupant: GUEST, occupant_owner: GUEST, cell: TEMPLE, host: HOST }
    }

    fn portcullis(reporter: u32) -> Facts {
        Facts { reporter, furniture: false, occupant: 0, occupant_owner: 0, cell: TEMPLE, host: HOST }
    }

    fn refusal(code: Code, why: &str) -> Verdict {
        Verdict::Refuse(Refusal { code, why: why.to_owned() })
    }

    #[test]
    fn a_furnitures_user_reports_its_graph_event_kept_nowhere() {
        // the guest pulls the lever in the host's temple: its own seat's
        assert_eq!(judge(&guest_pull()), Verdict::Relay { owner: Owner::User, keep: false });
        // the scene's bandit pulls it on its simulator, the host
        let bandit = Facts { reporter: HOST, occupant: BANDIT, occupant_owner: HOST, ..guest_pull() };
        assert_eq!(judge(&bandit), Verdict::Relay { owner: Owner::User, keep: false });
        // the guest's own NPC, in a cell nobody hosts
        let unhosted = Facts { occupant: BANDIT, host: 0, ..guest_pull() };
        assert_eq!(judge(&unhosted), Verdict::Relay { owner: Owner::User, keep: false });
    }

    #[test]
    fn a_furniture_in_use_is_not_the_hosts_nor_another_seats() {
        // the host's echo of the guest's pull
        assert_eq!(judge(&Facts { reporter: HOST, ..guest_pull() }), refusal(Code::NotUser, "used by ff000001, whose owner is ff000001"));
        // the guest's word on the bandit's use, and on a use nobody simulates
        let bandit = Facts { occupant: BANDIT, occupant_owner: HOST, ..guest_pull() };
        assert_eq!(judge(&bandit), refusal(Code::NotUser, "used by 39fe1, whose owner is ff000000"));
        let nobodys = Facts { occupant: BANDIT, occupant_owner: 0, ..guest_pull() };
        assert_eq!(judge(&nobodys), refusal(Code::NotUser, "used by 39fe1, whose owner is nobody"));
        assert_eq!(judge(&Facts { reporter: HOST, ..nobodys }), refusal(Code::NotUser, "used by 39fe1, whose owner is nobody"));
        // a reporter of 0 owns no use, even nobody's
        assert_eq!(judge(&Facts { reporter: 0, ..nobodys }), refusal(Code::NotUser, "used by 39fe1, whose owner is nobody"));
    }

    #[test]
    fn anything_else_is_the_cells_hosts_and_kept_but_a_furnitures() {
        assert_eq!(judge(&portcullis(HOST)), Verdict::Relay { owner: Owner::Host, keep: true });
        assert_eq!(judge(&portcullis(GUEST)), refusal(Code::NotHost, "which it does not host"));
        // a furniture nobody uses: the host's, kept nowhere
        let unused = Facts { reporter: HOST, occupant: 0, occupant_owner: 0, ..guest_pull() };
        assert_eq!(judge(&unused), Verdict::Relay { owner: Owner::Host, keep: false });
        assert_eq!(judge(&Facts { reporter: GUEST, ..unused }), refusal(Code::NotHost, "which it does not host"));
        // nobody hosting is no host of the reporter's
        assert_eq!(judge(&Facts { host: 0, ..portcullis(0) }), refusal(Code::NotHost, "which it does not host"));
        // an occupant on a reference that is no furniture (none is recorded,
        // but the facts may say so) is the host's case
        assert_eq!(judge(&Facts { occupant: GUEST, occupant_owner: GUEST, ..portcullis(HOST) }), Verdict::Relay { owner: Owner::Host, keep: true });
        assert_eq!(Code::NotHost.name(), "E_OBJANIM_NOT_HOST");
        assert_eq!(Code::NotUser.name(), "E_OBJANIM_NOT_USER");
    }

    proptest! {
        #[test]
        fn an_event_is_its_owners_and_a_furnitures_is_never_kept(
            reporter in prop_oneof![Just(0u32), Just(HOST), Just(GUEST), any::<u32>()],
            furniture in any::<bool>(),
            occupant in prop_oneof![Just(0u32), Just(GUEST), Just(BANDIT), any::<u32>()],
            occupant_owner in prop_oneof![Just(0u32), Just(HOST), Just(GUEST), any::<u32>()],
            cell in prop_oneof![Just(TEMPLE), any::<u32>()],
            host in prop_oneof![Just(0u32), Just(HOST), Just(GUEST), any::<u32>()],
        ) {
            let facts = Facts { reporter, furniture, occupant, occupant_owner, cell, host };
            let in_use = furniture && occupant != 0;
            match judge(&facts) {
                Verdict::Relay { owner: Owner::User, keep } => {
                    prop_assert!(in_use && occupant_owner != 0 && occupant_owner == reporter);
                    prop_assert!(!keep);
                }
                Verdict::Relay { owner: Owner::Host, keep } => {
                    prop_assert!(!in_use && host != 0 && host == reporter);
                    prop_assert_eq!(keep, !furniture);
                }
                Verdict::Refuse(r) => {
                    match r.code {
                        Code::NotUser => prop_assert!(in_use && (occupant_owner == 0 || occupant_owner != reporter)),
                        Code::NotHost => prop_assert!(!in_use && (host == 0 || host != reporter)),
                    }
                    prop_assert!(!r.why.is_empty());
                }
            }
        }
    }
}
