//! Character creation (thuum docs/verbs/character-creation.md). The race menu
//! offers the races whose RACE record has the Playable flag (UESP, "Skyrim
//! Mod:Mod File Format/RACE"; the lab saw exactly the ten playable races
//! listed, run 20261003-074358), so a client may change its race there only
//! to one of them. The race the server already records for the actor stays
//! allowed, so an actor a gamemode gave another race can still use the menu.

/// What the server knows about the race a client chose in the race menu.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RaceFacts {
    /// The server has game files to judge by (false only in unit tests).
    pub has_game_files: bool,
    /// The race equals the one the server records for the actor.
    pub is_recorded_race: bool,
    /// The form is a RACE record with the Playable flag.
    pub is_playable_race: bool,
}

/// Whether the race menu's result may set this race.
pub fn race_allowed(facts: RaceFacts) -> bool {
    !facts.has_game_files || facts.is_recorded_race || facts.is_playable_race
}

#[cfg(test)]
mod tests {
    use super::*;

    const fn facts(files: bool, recorded: bool, playable: bool) -> RaceFacts {
        RaceFacts { has_game_files: files, is_recorded_race: recorded, is_playable_race: playable }
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
