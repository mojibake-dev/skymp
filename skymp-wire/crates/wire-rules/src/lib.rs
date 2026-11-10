//! Game rules (thuum ADR-020). The C++ world model gathers the facts a rule
//! needs (positions, game records, equipment, animation events, the wall
//! clock) and asks here; the rule, its constants and any state it keeps live
//! in Rust. Each function is a decision over plain numbers, so its tests
//! need no server. The verb docs under thuum docs/verbs/ hold the sources.

pub mod activation;
pub mod alias_fills;
pub mod block;
pub mod bonds;
pub mod actor_values;
pub mod attributes;
pub mod appearance;
pub mod casts;
pub mod cells;
pub mod clock;
pub mod console;
pub mod damage;
pub mod destruction;
pub mod effects;
pub mod favorites;
pub mod forms;
pub mod headshots;
pub mod hosted_state;
pub mod hostility;
pub mod leveling;
pub mod locks;
pub mod magic;
pub mod markers;
pub mod melee;
pub mod movement;
pub mod natives;
pub mod object_animation;
pub mod perks;
pub mod placements;
pub mod player_globals;
pub mod player_own;
pub mod props;
pub mod quest_runs;
pub mod quest_scope;
pub mod quest_stages;
pub mod racemenu;
pub mod ranged;
pub mod ref_state;
pub mod rest;
pub mod script_state;
pub mod script_variables;
pub mod scripted_effects;
pub mod shouts;
pub mod stagger;
pub mod standing;
pub mod teleports;
pub mod trigger_entry;
pub mod weather;
pub mod world_globals;
pub mod world_streamer;

/// A decision and the bound it was made against, for the server's log.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Verdict {
    /// Whether the rule lets the action through.
    pub allowed: bool,
    /// The limit the action was measured against (units), when there is one.
    pub bound: f32,
}
