//! Game rules (thuum ADR-020). The C++ world model gathers the facts a rule
//! needs (positions, game records, equipment, animation events, the wall
//! clock) and asks here; the rule, its constants and any state it keeps live
//! in Rust. Each function is a decision over plain numbers, so its tests
//! need no server. The verb docs under thuum docs/verbs/ hold the sources.

pub mod activation;
pub mod actor_values;
pub mod appearance;
pub mod casts;
pub mod clock;
pub mod console;
pub mod damage;
pub mod effects;
pub mod favorites;
pub mod hostility;
pub mod magic;
pub mod markers;
pub mod melee;
pub mod movement;
pub mod racemenu;
pub mod ranged;
pub mod rest;

/// A decision and the bound it was made against, for the server's log.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Verdict {
    /// Whether the rule lets the action through.
    pub allowed: bool,
    /// The limit the action was measured against (units), when there is one.
    pub bound: f32,
}
