//! An NPC's standing (thuum ADR-068 rung 4, the state rows; docs/research/
//! rung4-state-rows-2026-11-07.md section 1(c); W3: the server's half and T2
//! this rung). An NPC's simulator reports its scripts' six standing natives
//! through the seam as batched state rows (ScriptCall 75, wire-rules natives
//! `Keep::Record`: the owner step judged the reporter): SetFactionRank,
//! ModFactionRank, RemoveFromFaction, RemoveFromAllFactions, SetCrimeFaction
//! and SetRelationshipRank (Papyrus's AddToFaction is script over
//! SetFactionRank, no native of its own). This rule merges one call into the
//! record the server keeps on the NPC's change form (MpChangeFormREFR's
//! factions, which MpActor loads from the base's SNAM rows at its first
//! faction edit, and rung 4's crimeFaction and relationships) and names the
//! one applier the receivers run, so every seat's engine converges on the
//! record's value instead of re-running the owner's arithmetic.
//!
//! The merge, the record's own shape (a faction's rank is the engine's int8,
//! CommonLibSSE-NG include/RE/F/FactionRank.h:12; ExtraFactionChanges keeps
//! the crime faction beside the changes, include/RE/E/ExtraFactionChanges.h:
//! 25-26): SetFactionRank sets the faction's rank, adding the faction when
//! the record lacks it; ModFactionRank moves a held faction's rank by its
//! argument within the int8's range and relays the rank it reached as
//! SetFactionRank, and on a faction the record does not hold changes
//! nothing (HYPOTHESIS H-ST2: the engine's ModFactionRank leaves a
//! non-member out, read by x-standing v1); RemoveFromFaction drops a held
//! faction; RemoveFromAllFactions drops every one; SetCrimeFaction sets the
//! crime faction, None its removal; SetRelationshipRank sets the rank
//! toward another actor. A call that leaves the record as it was is folded:
//! nothing is written or relayed. Refusals, logged by name: E_STANDING_FACTION
//! (the faction argument is not a FACT record, a crime faction's None
//! aside), E_STANDING_RANK (a rank or a step outside -128 to 127, the int8's
//! range), E_STANDING_OTHER (the relationship's other is no actor, or the
//! reporter's own player, whose relationships wait for rung 5's
//! acting-player scope). R2, rule 5: the simulator's engine ran the call;
//! its numbers are bounded, never judged.

/// A faction's rank on the record, by the faction's form id in the load
/// order both ends share.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct FactionRank {
    /// The FACT record.
    pub faction: u32,
    /// Its rank, the engine's int8.
    pub rank: i8,
}

/// A relationship rank toward another actor on the record.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Relationship {
    /// The other actor's form id.
    pub other: u32,
    /// The rank.
    pub rank: i8,
}

/// One reported standing call, its arguments as the core read them (a None
/// object as 0, an Int as the number the boundary read).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Call {
    /// SetFactionRank(akFaction, aiRank).
    SetFactionRank {
        /// The faction.
        faction: u32,
        /// The rank.
        rank: i64,
    },
    /// ModFactionRank(akFaction, aiMod).
    ModFactionRank {
        /// The faction.
        faction: u32,
        /// The step.
        by: i64,
    },
    /// RemoveFromFaction(akFaction).
    RemoveFromFaction {
        /// The faction.
        faction: u32,
    },
    /// RemoveFromAllFactions().
    RemoveFromAllFactions,
    /// SetCrimeFaction(akFaction), 0 for None.
    SetCrimeFaction {
        /// The faction, 0 for None.
        faction: u32,
    },
    /// SetRelationshipRank(akOther, aiRank).
    SetRelationshipRank {
        /// The other actor, 0 for None.
        other: u32,
        /// The rank.
        rank: i64,
    },
}

/// What the core knows of a call's object arguments.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Facts {
    /// The faction argument names a FACT record (SetFactionRank,
    /// ModFactionRank, RemoveFromFaction, SetCrimeFaction's non-None).
    pub faction_is_fact: bool,
    /// The relationship's other names an actor (SetRelationshipRank).
    pub other_is_actor: bool,
}

/// The standing the record holds, as the rule reads it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Record<'a> {
    /// The factions and their ranks (the base's SNAM rows once loaded, with
    /// every edit since).
    pub factions: &'a [FactionRank],
    /// The crime faction now: the record's, or the base's (CRIF) while the
    /// record holds none; 0 for none.
    pub crime_faction: u32,
    /// The relationship ranks the record holds.
    pub relationships: &'a [Relationship],
}

/// The change one accepted call makes, and the applier the receivers run
/// for it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Change {
    /// The faction's rank is now `rank`, the faction added when the record
    /// lacked it: SetFactionRank(faction, rank) on every receiver.
    Rank {
        /// The faction.
        faction: u32,
        /// Its rank.
        rank: i8,
    },
    /// The faction is dropped: RemoveFromFaction(faction).
    Removed {
        /// The faction.
        faction: u32,
    },
    /// Every faction is dropped: RemoveFromAllFactions().
    RemovedAll,
    /// The crime faction is now `faction`, 0 for none:
    /// SetCrimeFaction(faction or None).
    Crime {
        /// The faction, 0 for none.
        faction: u32,
    },
    /// The rank toward `other` is now `rank`: SetRelationshipRank(other,
    /// rank).
    Relationship {
        /// The other actor.
        other: u32,
        /// The rank.
        rank: i8,
    },
}

/// Why a call is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// The faction argument is not a FACT record.
    Faction,
    /// A rank or a step outside -128 to 127.
    Rank,
    /// The relationship's other is no actor, or the reporter's own player.
    Other,
}

impl Code {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::Faction => "E_STANDING_FACTION",
            Code::Rank => "E_STANDING_RANK",
            Code::Other => "E_STANDING_OTHER",
        }
    }
}

/// The rule's word on one call.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    /// The call moved the record: the change to write and relay.
    Recorded(Change),
    /// The record stands as it was: nothing is written or relayed.
    Folded,
    /// Refused, logged by the code's name with why.
    Refuse(Code, String),
}

/// The reporter's own player as its engine names it.
const OWN_PLAYER: u32 = 0x14;

fn rank_of(value: i64) -> Option<i8> {
    i8::try_from(value).ok()
}

/// One call against the record, over the facts the core gathered.
#[must_use]
pub fn judge(record: &Record<'_>, call: Call, facts: Facts) -> Verdict {
    let held = |faction: u32| record.factions.iter().find(|f| f.faction == faction).map(|f| f.rank);
    let not_fact = |faction: u32| Verdict::Refuse(Code::Faction, format!("{faction:x} is no FACT record"));
    let out_of_range = |what: &str, value: i64| Verdict::Refuse(Code::Rank, format!("{what} {value} is outside -128 to 127"));
    match call {
        Call::SetFactionRank { faction, rank } => {
            if !facts.faction_is_fact {
                return not_fact(faction);
            }
            let Some(rank) = rank_of(rank) else {
                return out_of_range("the rank", rank);
            };
            if held(faction) == Some(rank) {
                Verdict::Folded
            } else {
                Verdict::Recorded(Change::Rank { faction, rank })
            }
        }
        Call::ModFactionRank { faction, by } => {
            if !facts.faction_is_fact {
                return not_fact(faction);
            }
            let Some(step) = rank_of(by) else {
                return out_of_range("the step", by);
            };
            let Some(rank) = held(faction) else {
                // HYPOTHESIS H-ST2: a non-member's ModFactionRank changes
                // nothing on the engine either
                return Verdict::Folded;
            };
            let after = rank.saturating_add(step);
            if after == rank {
                Verdict::Folded
            } else {
                Verdict::Recorded(Change::Rank { faction, rank: after })
            }
        }
        Call::RemoveFromFaction { faction } => {
            if !facts.faction_is_fact {
                return not_fact(faction);
            }
            if held(faction).is_some() {
                Verdict::Recorded(Change::Removed { faction })
            } else {
                Verdict::Folded
            }
        }
        Call::RemoveFromAllFactions => {
            if record.factions.is_empty() {
                Verdict::Folded
            } else {
                Verdict::Recorded(Change::RemovedAll)
            }
        }
        Call::SetCrimeFaction { faction } => {
            if faction != 0 && !facts.faction_is_fact {
                return not_fact(faction);
            }
            if record.crime_faction == faction {
                Verdict::Folded
            } else {
                Verdict::Recorded(Change::Crime { faction })
            }
        }
        Call::SetRelationshipRank { other, rank } => {
            if other == OWN_PLAYER {
                return Verdict::Refuse(Code::Other, "the reporter's own player: a relationship with it is its seat's (rung 5's acting-player scope)".to_owned());
            }
            if other == 0 || !facts.other_is_actor {
                return Verdict::Refuse(Code::Other, format!("{other:x} is no actor"));
            }
            let Some(rank) = rank_of(rank) else {
                return out_of_range("the rank", rank);
            };
            if record.relationships.iter().any(|r| r.other == other && r.rank == rank) {
                Verdict::Folded
            } else {
                Verdict::Recorded(Change::Relationship { other, rank })
            }
        }
    }
}

#[cfg(test)]
#[allow(clippy::panic)] // a test's failure message, never a packet's path
mod tests {
    use super::*;

    // a town faction at rank 0 and a crime faction (illustrative ids: the
    // rule reads numbers, the core reads the records)
    const TOWN: u32 = 0x2a01e;
    const CRIME: u32 = 0x267ea;
    const OTHER_NPC: u32 = 0x1347b;
    const FACT: Facts = Facts { faction_is_fact: true, other_is_actor: true };

    fn record<'a>(factions: &'a [FactionRank], relationships: &'a [Relationship]) -> Record<'a> {
        Record { factions, crime_faction: CRIME, relationships }
    }

    #[test]
    fn a_rank_is_set_and_a_repeat_folded() {
        let factions = [FactionRank { faction: TOWN, rank: 0 }];
        let r = record(&factions, &[]);
        assert_eq!(judge(&r, Call::SetFactionRank { faction: TOWN, rank: 2 }, FACT), Verdict::Recorded(Change::Rank { faction: TOWN, rank: 2 }));
        assert_eq!(judge(&r, Call::SetFactionRank { faction: TOWN, rank: 0 }, FACT), Verdict::Folded);
        // a faction the record lacks is added
        assert_eq!(judge(&r, Call::SetFactionRank { faction: 0x5c84e, rank: -1 }, FACT), Verdict::Recorded(Change::Rank { faction: 0x5c84e, rank: -1 }));
    }

    #[test]
    fn a_step_lands_as_the_rank_it_reached() {
        let factions = [FactionRank { faction: TOWN, rank: 120 }];
        let r = record(&factions, &[]);
        assert_eq!(judge(&r, Call::ModFactionRank { faction: TOWN, by: 3 }, FACT), Verdict::Recorded(Change::Rank { faction: TOWN, rank: 123 }));
        // the int8 holds it
        assert_eq!(judge(&r, Call::ModFactionRank { faction: TOWN, by: 100 }, FACT), Verdict::Recorded(Change::Rank { faction: TOWN, rank: 127 }));
        assert_eq!(judge(&r, Call::ModFactionRank { faction: TOWN, by: 0 }, FACT), Verdict::Folded);
        // a faction the record lacks: nothing (H-ST2)
        assert_eq!(judge(&r, Call::ModFactionRank { faction: 0x5c84e, by: 1 }, FACT), Verdict::Folded);
        let top = [FactionRank { faction: TOWN, rank: 127 }];
        assert_eq!(judge(&record(&top, &[]), Call::ModFactionRank { faction: TOWN, by: 1 }, FACT), Verdict::Folded);
    }

    #[test]
    fn a_removal_drops_a_held_faction_and_folds_otherwise() {
        let factions = [FactionRank { faction: TOWN, rank: 0 }];
        let r = record(&factions, &[]);
        assert_eq!(judge(&r, Call::RemoveFromFaction { faction: TOWN }, FACT), Verdict::Recorded(Change::Removed { faction: TOWN }));
        assert_eq!(judge(&r, Call::RemoveFromFaction { faction: 0x5c84e }, FACT), Verdict::Folded);
        assert_eq!(judge(&r, Call::RemoveFromAllFactions, FACT), Verdict::Recorded(Change::RemovedAll));
        assert_eq!(judge(&record(&[], &[]), Call::RemoveFromAllFactions, FACT), Verdict::Folded);
    }

    #[test]
    fn the_crime_faction_and_its_removal() {
        let r = record(&[], &[]);
        assert_eq!(judge(&r, Call::SetCrimeFaction { faction: 0x28170 }, FACT), Verdict::Recorded(Change::Crime { faction: 0x28170 }));
        assert_eq!(judge(&r, Call::SetCrimeFaction { faction: CRIME }, FACT), Verdict::Folded);
        // None removes it, whatever the facts say of a faction
        assert_eq!(judge(&r, Call::SetCrimeFaction { faction: 0 }, Facts { faction_is_fact: false, ..FACT }), Verdict::Recorded(Change::Crime { faction: 0 }));
        assert_eq!(judge(&Record { crime_faction: 0, ..r }, Call::SetCrimeFaction { faction: 0 }, FACT), Verdict::Folded);
    }

    #[test]
    fn a_relationship_rank_toward_another_actor() {
        let held = [Relationship { other: OTHER_NPC, rank: 1 }];
        let r = record(&[], &held);
        assert_eq!(judge(&r, Call::SetRelationshipRank { other: OTHER_NPC, rank: 3 }, FACT), Verdict::Recorded(Change::Relationship { other: OTHER_NPC, rank: 3 }));
        assert_eq!(judge(&r, Call::SetRelationshipRank { other: OTHER_NPC, rank: 1 }, FACT), Verdict::Folded);
    }

    #[test]
    fn the_refusals_have_their_names() {
        let r = record(&[], &[]);
        let not_fact = Facts { faction_is_fact: false, ..FACT };
        let refused = |v: Verdict| match v {
            Verdict::Refuse(code, why) => (code.name(), why),
            other => panic!("not refused: {other:?}"),
        };
        assert_eq!(refused(judge(&r, Call::SetFactionRank { faction: 0x12eb7, rank: 0 }, not_fact)), ("E_STANDING_FACTION", "12eb7 is no FACT record".to_owned()));
        assert_eq!(refused(judge(&r, Call::ModFactionRank { faction: 0x12eb7, by: 1 }, not_fact)).0, "E_STANDING_FACTION");
        assert_eq!(refused(judge(&r, Call::RemoveFromFaction { faction: 0 }, not_fact)).0, "E_STANDING_FACTION");
        assert_eq!(refused(judge(&r, Call::SetCrimeFaction { faction: 0x12eb7 }, not_fact)).0, "E_STANDING_FACTION");
        assert_eq!(refused(judge(&r, Call::SetFactionRank { faction: TOWN, rank: 128 }, FACT)), ("E_STANDING_RANK", "the rank 128 is outside -128 to 127".to_owned()));
        assert_eq!(refused(judge(&r, Call::SetFactionRank { faction: TOWN, rank: -129 }, FACT)).0, "E_STANDING_RANK");
        assert_eq!(refused(judge(&r, Call::ModFactionRank { faction: TOWN, by: 1000 }, FACT)), ("E_STANDING_RANK", "the step 1000 is outside -128 to 127".to_owned()));
        assert_eq!(refused(judge(&r, Call::SetRelationshipRank { other: OTHER_NPC, rank: 300 }, FACT)).0, "E_STANDING_RANK");
        assert_eq!(refused(judge(&r, Call::SetRelationshipRank { other: 0x12eb7, rank: 1 }, Facts { other_is_actor: false, ..FACT })), ("E_STANDING_OTHER", "12eb7 is no actor".to_owned()));
        assert_eq!(refused(judge(&r, Call::SetRelationshipRank { other: 0, rank: 1 }, FACT)).0, "E_STANDING_OTHER");
        assert_eq!(refused(judge(&r, Call::SetRelationshipRank { other: 0x14, rank: 1 }, FACT)).0, "E_STANDING_OTHER");
    }
}
