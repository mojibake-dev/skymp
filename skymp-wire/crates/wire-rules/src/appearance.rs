//! Character creation (thuum docs/verbs/character-creation.md). The race menu
//! offers the races whose RACE record has the Playable flag (UESP, "Skyrim
//! Mod:Mod File Format/RACE"; the lab saw exactly the ten playable races
//! listed, run 20261003-074358), so a client may change its race there only
//! to one of them. The race the server already records for the actor stays
//! allowed, so an actor a gamemode gave another race can still use the menu.
//!
//! The permit (M5, 2026-11-01): a server may restrict a playable race to the
//! profiles it lists (server setting `restrictedRaces`, a mod's custom race
//! for the one player it was made for), so a restricted race passes the menu
//! only for a listed profile. A permit never makes an unplayable race
//! playable, and the recorded race stays allowed as before.

/// What the server knows about the race a client chose in the race menu.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RaceFacts {
    /// The server has game files to judge by (false only in unit tests).
    pub has_game_files: bool,
    /// The race equals the one the server records for the actor.
    pub is_recorded_race: bool,
    /// The form is a RACE record with the Playable flag.
    pub is_playable_race: bool,
    /// The race is on the server's restricted list (`restrictedRaces`).
    pub is_restricted: bool,
    /// The actor's profile is on the race's permit list.
    pub is_permitted: bool,
}

/// Whether the race menu's result may set this race.
pub fn race_allowed(facts: RaceFacts) -> bool {
    !facts.has_game_files
        || facts.is_recorded_race
        || (facts.is_playable_race && (!facts.is_restricted || facts.is_permitted))
}

#[cfg(test)]
mod tests {
    use super::*;

    const fn facts(files: bool, recorded: bool, playable: bool) -> RaceFacts {
        RaceFacts {
            has_game_files: files,
            is_recorded_race: recorded,
            is_playable_race: playable,
            is_restricted: false,
            is_permitted: false,
        }
    }

    const fn restricted(playable: bool, recorded: bool, permitted: bool) -> RaceFacts {
        RaceFacts {
            has_game_files: true,
            is_recorded_race: recorded,
            is_playable_race: playable,
            is_restricted: true,
            is_permitted: permitted,
        }
    }

    #[test]
    fn a_restricted_race_passes_for_a_permitted_profile_alone() {
        assert!(race_allowed(restricted(true, false, true)));
        assert!(!race_allowed(restricted(true, false, false)));
    }

    #[test]
    fn a_permit_makes_no_unplayable_race_playable() {
        assert!(!race_allowed(restricted(false, false, true)));
    }

    #[test]
    fn the_recorded_race_stays_allowed_under_a_restriction() {
        assert!(race_allowed(restricted(true, true, false)));
        assert!(race_allowed(restricted(false, true, false)));
    }

    #[test]
    fn a_playable_or_recorded_race_passes_and_nothing_else() {
        assert!(race_allowed(facts(true, false, true)));
        assert!(race_allowed(facts(true, true, false)));
        assert!(!race_allowed(facts(true, false, false)));
    }

    #[test]
    fn without_game_files_there_is_nothing_to_judge_by() {
        assert!(race_allowed(facts(false, false, false)));
    }
}
