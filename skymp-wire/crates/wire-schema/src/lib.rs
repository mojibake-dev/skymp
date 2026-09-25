//! The wire contract. Every message that crosses the network is a variant of
//! [`Message`]. Ids are append-only. Collections are fixed-capacity so an
//! over-long input fails at decode, never later (ADR-012).
//!
//! Capacities are named constants chosen from the game; changing one is a
//! contract change and gets a new message id, not an edit.
//!
//! Every variant documents its direction, rung, idempotency, and the reason
//! codes `wire-validate` can emit for it. Reason codes cross the wire as the
//! `u16` in [`Message::Refuse`]; the numbering lives in `wire-transport`.
#![no_std]

use heapless::{String, Vec};
use postcard::experimental::max_size::MaxSize;
use serde::{Deserialize, Serialize};

/// Bump when any variant changes shape. Clients with a different value are
/// refused at `Hello` (`E_VAL_SCHEMA_VERSION`).
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
    /// World x, engine units.
    pub x: f32,
    /// World y, engine units.
    pub y: f32,
    /// World z, engine units.
    pub z: f32,
    /// Heading, radians.
    pub yaw: f32,
    /// Look pitch, radians.
    pub pitch: f32,
}

/// Session family, client to server, rung R0: the server decides everything
/// about a session. Sent once per connection; a second `Hello` on a live
/// connection is a protocol error the server closes on. Reason codes:
/// `E_VAL_SCHEMA_VERSION`.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, MaxSize)]
pub struct Hello {
    /// Must equal [`SCHEMA_VERSION`].
    pub schema_version: u16,
    /// Build number of the client stack, compared against the server's allowlist.
    pub client_build: u32,
    /// Hash per mod in load order; the server compares against its own.
    pub mod_hashes: Vec<u64, { cap::MODS }>,
    /// Display name the player asked for; the server may rename.
    pub name: String<{ cap::NAME }>,
}

/// Intent family, client to server, rung R1. Sent at the client's sample rate
/// on the Unreliable channel; safe to receive twice: the newest `seq` wins and
/// anything not newer than the last accepted sample is dropped. Reason codes:
/// `E_VAL_NONFINITE`, `E_VAL_OUT_OF_WORLD`, `E_VAL_SEQ_REPLAY`, `E_VAL_RATE`.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize, MaxSize)]
pub struct MovementSample {
    /// Monotonic per client; the validator drops anything not newer.
    pub seq: u32,
    /// The actor moved; ownership is checked by the server, not here.
    pub actor: FormId,
    /// Where the actor is now.
    pub transform: Transform,
    /// Running flag as the engine reports it.
    pub run: bool,
    /// Sneaking flag as the engine reports it.
    pub sneak: bool,
}

/// Intent family, client to server, rung R1. Rides ReliableUnordered, so it
/// may arrive out of order; idempotent on `seq` inside a 64-wide replay
/// window. Reason codes: `E_VAL_SEQ_REPLAY`, `E_VAL_RATE`.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize, MaxSize)]
pub struct HitIntent {
    /// Per-client hit counter; a repeat inside the window is dropped.
    pub seq: u32,
    /// Who swung.
    pub attacker: FormId,
    /// Who was hit, as the client saw it; the server re-checks range and angle.
    pub target: FormId,
    /// Weapon form used, or the unarmed form.
    pub weapon: FormId,
    /// Power attack flag.
    pub power_attack: bool,
    /// Client's estimate of when it saw the hit, for lag compensation.
    pub client_time_ms: u32,
}

/// One inventory change. Positive count adds, negative removes; the server
/// decides whether it happens (R0). A zero count is invalid
/// (`E_VAL_ZERO_DELTA`).
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize, MaxSize)]
pub struct ItemDelta {
    /// The item form.
    pub item: FormId,
    /// Signed count; never zero.
    pub count: i32,
}

/// Record family, host client to server, rung R2: the cell host reports an
/// NPC it simulates. Rides ReliableUnordered; the server keeps the latest per
/// actor, so a duplicate is harmless. Reason codes: `E_VAL_NONFINITE`,
/// `E_VAL_OUT_OF_WORLD`, `E_VAL_ZERO_DELTA`.
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, MaxSize)]
pub struct HostedActorState {
    /// The NPC.
    pub actor: FormId,
    /// Where the host's engine has it.
    pub transform: Transform,
    /// Current health as the host's engine has it.
    pub health: f32,
    /// Inventory changes since the last report.
    pub inventory_delta: Vec<ItemDelta, { cap::INVENTORY_DELTA }>,
}

/// Every message on the wire. Variant order is the id; append only.
///
/// The enum is the on-wire format, so its largest variant (`Hello`, bounded by
/// [`cap::MODS`]) sets the size of every value. That is by design: wire types
/// carry no `Box` (wire rules), and the lint is silenced for that reason only.
#[allow(clippy::large_enum_variant)]
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize, MaxSize)]
#[non_exhaustive]
pub enum Message {
    /// Client to server, R0. See [`Hello`].
    Hello(Hello),
    /// Server to client, R0. Answers `Hello`; sent once per connection.
    Welcome {
        /// The id the server will use for this client.
        client_id: ClientId,
        /// Server clock at send time, milliseconds.
        server_time_ms: u64,
    },
    /// Server to client, R0. Why the server is dropping the connection; the
    /// code is a `wire-transport` reason code. Sent at most once.
    Refuse {
        /// Reason code.
        reason: u16,
    },
    /// Client to server, R1. See [`MovementSample`].
    Movement(MovementSample),
    /// Client to server, R1. See [`HitIntent`].
    Hit(HitIntent),
    /// Host to server, R2. See [`HostedActorState`].
    HostedActor(HostedActorState),
    /// Server to clients, R0 output: one applied inventory change for clients
    /// to render. The server sends each applied delta exactly once over
    /// ReliableUnordered; clients apply what they receive.
    InventoryApply {
        /// Whose inventory.
        owner: FormId,
        /// The change.
        delta: ItemDelta,
    },
    /// Server to one client, R0 output: you now host `cell`. Idempotent.
    HostGrant {
        /// The cell form.
        cell: FormId,
    },
    /// Server to one client, R0 output: stop hosting `cell`. Idempotent.
    HostRelease {
        /// The cell form.
        cell: FormId,
    },
}

impl Message {
    /// Hard cap on the encoded size of any message, derived from the types.
    /// `wire-codec` refuses inputs longer than this before decoding.
    pub const MAX_ENCODED_LEN: usize = <Message as MaxSize>::POSTCARD_MAX_SIZE;
}
