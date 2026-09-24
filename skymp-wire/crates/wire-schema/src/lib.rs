//! The wire contract. Every message that crosses the network is a variant of
//! [`Message`]. Ids are append-only. Collections are fixed-capacity so an
//! over-long input fails at decode, never later (ADR-012).
//!
//! Capacities are named constants chosen from the game; changing one is a
//! contract change and gets a new message id, not an edit.
#![no_std]

use heapless::{String, Vec};
use postcard::experimental::max_size::MaxSize;
use serde::{Deserialize, Serialize};

/// Bump when any variant changes shape. Clients with a different value are
/// refused at `Hello`.
pub const SCHEMA_VERSION: u16 = 1;

/// Capacities. Named so the reason for each number is greppable.
pub mod cap {
    /// Longest player or actor name we will carry on the wire.
    pub const NAME: usize = 64;
    /// Items in one inventory delta; larger changes are split by the sender.
    pub const INVENTORY_DELTA: usize = 32;
    /// Active effects reported in one record message.
    pub const EFFECTS: usize = 24;
    /// Mods in a load-order hash list.
    pub const MODS: usize = 255;
}

/// Newtype over an engine form id. Validated against the server's load order
/// before it is used as a key anywhere.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, MaxSize)]
pub struct FormId(pub u32);

/// Server-assigned client id; never client-supplied.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, MaxSize)]
pub struct ClientId(pub u64);

/// Position and rotation in engine units. Bounds are enforced in
/// `wire-validate`, not here; this type only has to be representable.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize, MaxSize)]
pub struct Transform {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub yaw: f32,
    pub pitch: f32,
}

/// Session family (R0).
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, MaxSize)]
pub struct Hello {
    pub schema_version: u16,
    pub client_build: u32,
    /// Hash per mod in load order; the server compares against its own.
    pub mod_hashes: Vec<u64, { cap::MODS }>,
    pub name: String<{ cap::NAME }>,
}

/// Intent family (R1). Sent at the client's sample rate on the Unreliable
/// channel; safe to receive twice, later sequence wins.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize, MaxSize)]
pub struct MovementSample {
    pub seq: u32,
    pub actor: FormId,
    pub transform: Transform,
    pub run: bool,
    pub sneak: bool,
}

/// Intent family (R1). Reliable; idempotent on `seq`.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize, MaxSize)]
pub struct HitIntent {
    pub seq: u32,
    pub attacker: FormId,
    pub target: FormId,
    pub weapon: FormId,
    pub power_attack: bool,
    /// Client's estimate of when it saw the hit, for lag compensation.
    pub client_time_ms: u32,
}

/// One inventory change. Positive count adds, negative removes; the server
/// decides whether it happens (R0).
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize, MaxSize)]
pub struct ItemDelta {
    pub item: FormId,
    pub count: i32,
}

/// Record family (R2): the cell host reports an NPC it simulates.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, MaxSize)]
pub struct HostedActorState {
    pub actor: FormId,
    pub transform: Transform,
    pub health: f32,
    pub inventory_delta: Vec<ItemDelta, { cap::INVENTORY_DELTA }>,
}

/// Every message on the wire. Variant order is the id; append only.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, MaxSize)]
#[non_exhaustive]
pub enum Message {
    // Session
    Hello(Hello),
    Welcome { client_id: ClientId, server_time_ms: u64 },
    Refuse { reason: u16 },
    // Intent (R1)
    Movement(MovementSample),
    Hit(HitIntent),
    // Record (R2)
    HostedActor(HostedActorState),
    // World (R0 output)
    InventoryApply { owner: FormId, delta: ItemDelta },
    // Ownership (R0 output)
    HostGrant { cell: FormId },
    HostRelease { cell: FormId },
}

impl Message {
    /// Hard cap on the encoded size of any message, derived from the types.
    /// `wire-codec` refuses inputs longer than this before decoding.
    pub const MAX_ENCODED_LEN: usize = <Message as MaxSize>::POSTCARD_MAX_SIZE;
}
