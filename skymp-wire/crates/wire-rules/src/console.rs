//! Console commands (thuum docs/verbs/console-commands.md): a player types
//! the commands it knows; the server decides who may run which and runs the
//! ones that change the shared world. Ranks are TES3MP's staff ranks (0
//! player, 1 moderator, 2 admin, 3 owner; TES3MP CoreScripts keeps
//! `staffRank` in each player's record). A command is one of three classes:
//! the server runs it (R0), it only touches the player's own view and never
//! reaches the server (R3), or nobody may run it because only the server may
//! make its effect (a save or a load of the game).

/// A player's staff rank.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Rank {
    /// Every player.
    Player = 0,
    /// Moves players, kills and revives.
    Moderator = 1,
    /// Changes inventories, references, actor values and the clock.
    Admin = 2,
    /// Everything an admin may, and ranks.
    Owner = 3,
}

impl Rank {
    /// A rank from its number; anything past the owner is the owner.
    #[must_use]
    pub const fn from_number(n: u8) -> Self {
        match n {
            0 => Self::Player,
            1 => Self::Moderator,
            2 => Self::Admin,
            _ => Self::Owner,
        }
    }
}

/// How a command runs.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Class {
    /// The server runs it against its own state (R0); the clients see the
    /// result through the verbs that carry that state.
    Server,
    /// The player's own view only (R3): the client never sends it.
    Client,
    /// Nobody: the server owns what it would change.
    Refused,
}

/// One command of the table.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Command {
    /// Its name, lower case, as the console's long name.
    pub name: &'static str,
    /// The console's short name, lower case, or "" for none.
    pub short: &'static str,
    /// How it runs.
    pub class: Class,
    /// The lowest rank that may run it (a refused command: none).
    pub rank: Rank,
    /// Whether the server runs it yet; a listed command it does not run is
    /// refused with a line saying so, never passed to the game.
    pub served: bool,
}

const fn cmd(name: &'static str, short: &'static str, class: Class, rank: Rank, served: bool) -> Command {
    Command { name, short, class, rank, served }
}

/// The first set (thuum docs/verbs/console-commands.md, "Commands, first
/// set"; the ranks await Eli's review). `mp` is SkyMP's own.
pub const TABLE: [Command; 28] = [
    cmd("additem", "", Class::Server, Rank::Admin, true),
    cmd("removeitem", "", Class::Server, Rank::Admin, true),
    cmd("equipitem", "", Class::Server, Rank::Admin, true),
    cmd("placeatme", "", Class::Server, Rank::Admin, true),
    cmd("disable", "", Class::Server, Rank::Admin, true),
    cmd("enable", "", Class::Server, Rank::Admin, true),
    cmd("mp", "", Class::Server, Rank::Admin, true),
    cmd("centeroncell", "coc", Class::Server, Rank::Moderator, false),
    cmd("moveto", "", Class::Server, Rank::Moderator, true),
    cmd("setpos", "", Class::Server, Rank::Moderator, true),
    cmd("setangle", "", Class::Server, Rank::Moderator, true),
    cmd("kill", "", Class::Server, Rank::Moderator, true),
    cmd("resurrect", "", Class::Server, Rank::Moderator, true),
    cmd("setactorvalue", "setav", Class::Server, Rank::Admin, true),
    cmd("modactorvalue", "modav", Class::Server, Rank::Admin, true),
    cmd("forceactorvalue", "forceav", Class::Server, Rank::Admin, true),
    cmd("setlevel", "", Class::Server, Rank::Admin, false),
    cmd("advancepcskill", "advskill", Class::Server, Rank::Admin, false),
    cmd("toggleimmortalmode", "tim", Class::Server, Rank::Admin, false),
    cmd("togglegodmode", "tgm", Class::Server, Rank::Admin, false),
    cmd("togglecollision", "tcl", Class::Server, Rank::Admin, false),
    // `set <global> to <value>`: the server owns the clock's globals (ADR-021);
    // which others it owns waits for the first verb that needs one
    cmd("set", "", Class::Server, Rank::Admin, false),
    cmd("togglefreecamera", "tfc", Class::Client, Rank::Player, false),
    cmd("togglemenus", "tm", Class::Client, Rank::Player, false),
    cmd("save", "", Class::Refused, Rank::Owner, false),
    cmd("load", "", Class::Refused, Rank::Owner, false),
    cmd("savegame", "", Class::Refused, Rank::Owner, false),
    cmd("loadgame", "", Class::Refused, Rank::Owner, false),
];

/// A command by its long or short name, case aside.
#[must_use]
pub fn find(name: &str) -> Option<&'static Command> {
    TABLE
        .iter()
        .find(|c| c.name.eq_ignore_ascii_case(name) || (!c.short.is_empty() && c.short.eq_ignore_ascii_case(name)))
}

/// What the server does with a command a player sent.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Decision {
    /// Run it.
    Run,
    /// The caller's rank is below the command's.
    RankTooLow,
    /// Nobody may run it.
    Refused,
    /// Listed, not run by the server yet.
    NotServed,
    /// A client-only command, which a client never sends.
    ClientOnly,
    /// Not in the table.
    Unknown,
}

/// Whether a caller of a rank may have the server run a command.
#[must_use]
pub fn decide(name: &str, rank: Rank) -> Decision {
    let Some(c) = find(name) else {
        return Decision::Unknown;
    };
    match c.class {
        Class::Refused => Decision::Refused,
        Class::Client => Decision::ClientOnly,
        Class::Server if rank < c.rank => Decision::RankTooLow,
        Class::Server if !c.served => Decision::NotServed,
        Class::Server => Decision::Run,
    }
}

/// The line the caller's console prints for a decision other than Run.
#[must_use]
pub const fn refusal_line(d: Decision) -> &'static str {
    match d {
        Decision::Run => "",
        Decision::RankTooLow => "Not enough permissions to use this command",
        Decision::Refused => "The server owns the world: this command is not available",
        Decision::NotServed => "The server does not run this command yet",
        Decision::ClientOnly => "This command runs in your own game only",
        Decision::Unknown => "Unknown command",
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn names_resolve_long_and_short_case_aside() {
        assert_eq!(find("AddItem").map(|c| c.name), Some("additem"));
        assert_eq!(find("COC").map(|c| c.name), Some("centeroncell"));
        assert_eq!(find("setav").map(|c| c.name), Some("setactorvalue"));
        assert_eq!(find("SetActorValue").map(|c| c.short), Some("setav"));
        assert_eq!(find(""), None);
        assert_eq!(find("nosuchcommand"), None);
    }

    #[test]
    fn ranks_gate_server_commands() {
        assert_eq!(decide("additem", Rank::Admin), Decision::Run);
        assert_eq!(decide("additem", Rank::Owner), Decision::Run);
        assert_eq!(decide("additem", Rank::Moderator), Decision::RankTooLow);
        assert_eq!(decide("additem", Rank::Player), Decision::RankTooLow);
        assert_eq!(decide("setav", Rank::Admin), Decision::Run);
        assert_eq!(decide("modav", Rank::Player), Decision::RankTooLow);
    }

    #[test]
    fn saves_and_loads_are_nobody_s() {
        for name in ["save", "Load", "savegame", "loadgame"] {
            assert_eq!(decide(name, Rank::Owner), Decision::Refused, "{name}");
        }
    }

    #[test]
    fn listed_commands_the_server_does_not_run_yet_say_so() {
        assert_eq!(decide("coc", Rank::Owner), Decision::NotServed);
        assert_eq!(decide("kill", Rank::Moderator), Decision::Run);
        assert_eq!(decide("setpos", Rank::Player), Decision::RankTooLow);
        assert_eq!(decide("removeitem", Rank::Moderator), Decision::RankTooLow);
        assert_eq!(decide("coc", Rank::Player), Decision::RankTooLow);
        assert_eq!(decide("tfc", Rank::Owner), Decision::ClientOnly);
        assert_eq!(decide("set", Rank::Owner), Decision::NotServed);
        assert_eq!(decide("whatever", Rank::Owner), Decision::Unknown);
        assert!(!refusal_line(Decision::NotServed).is_empty());
        assert!(refusal_line(Decision::Run).is_empty());
    }

    #[test]
    fn rank_numbers_past_the_owner_are_the_owner() {
        assert_eq!(Rank::from_number(0), Rank::Player);
        assert_eq!(Rank::from_number(2), Rank::Admin);
        assert_eq!(Rank::from_number(3), Rank::Owner);
        assert_eq!(Rank::from_number(200), Rank::Owner);
    }

    #[test]
    fn every_name_is_lower_case_and_unique() {
        let mut seen = std::collections::BTreeSet::new();
        for c in TABLE {
            assert_eq!(c.name, c.name.to_ascii_lowercase());
            assert_eq!(c.short, c.short.to_ascii_lowercase());
            assert!(seen.insert(c.name), "{}", c.name);
            if !c.short.is_empty() {
                assert!(seen.insert(c.short), "{}", c.short);
            }
        }
    }
}
