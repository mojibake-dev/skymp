//! The wire contract. Every message that crosses the network is a variant of
//! [`Message`]. Ids are append-only: the variant's position is its id on the
//! wire. Collections are [`bounded`] so an over-long input fails at decode,
//! at the field that overflowed, never later (ADR-012, ADR-019).
//!
//! Two families share the enum. The M0 variants (ids 0 to 8) are the
//! authority model's messages, reserved until its verbs land. The SkyMP
//! variants (ids 9 to 41, id = MsgType + 8) are SkyMP's own protocol, ported
//! field for field in [`skymp`]; their JSON form is what the C++ core and
//! skymp5-client exchange in-process (`wire-json`).
//!
//! Capacities are named constants in [`cap`], chosen from the game; every
//! message has a byte cap in [`Message::max_len`], which the codec checks
//! before it decodes a body. Every variant documents its direction, rung,
//! idempotency and the reason codes `wire-validate` can emit for it.
#![no_std]

extern crate alloc;
// The Arbitrary derive names `::std`; the feature is for the fuzz targets only.
#[cfg(feature = "arbitrary")]
extern crate std;

pub mod bounded;
pub mod finite;
mod shape;
pub mod skymp;

use serde::{Deserialize, Serialize};

use bounded::{String, Vec};

/// Bump when any variant changes shape. It is part of netcode's protocol id,
/// so peers built against another schema never complete a handshake.
pub const SCHEMA_VERSION: u16 = 2;

/// Capacities. Strings are in UTF-8 bytes, sequences in elements. Named so
/// the reason for each number is greppable.
pub mod cap {
    /// A player, item or actor name.
    pub const NAME: usize = 256;
    /// Mods in a load-order hash list.
    pub const MODS: usize = 255;
    /// Items in one M0 inventory delta; larger changes are split by the sender.
    pub const INVENTORY_DELTA: usize = 32;
    /// Active effects reported in one M0 record message.
    pub const EFFECTS: usize = 24;
    /// A path inside the game's archives (Windows MAX_PATH).
    pub const TEXTURE_PATH: usize = 260;
    /// Head parts on one character (a Nord in the lab has 8).
    pub const HEADPARTS: usize = 64;
    /// Face morph values (the engine has 19).
    pub const FACE_OPTIONS: usize = 64;
    /// Face preset values (the engine has 4).
    pub const FACE_PRESETS: usize = 16;
    /// Tint layers on one character (a Nord in the lab has 34).
    pub const TINTS: usize = 128;
    /// Stacks in one inventory: a merchant chest, not just a player.
    pub const INVENTORY: usize = 4096;
    /// Spells one actor knows.
    pub const SPELLS: usize = 2048;
    /// Leveled templates one NPC resolves through.
    pub const TEMPLATE_CHAIN: usize = 64;
    /// Node overrides on one reference.
    pub const NODES: usize = 256;
    /// A node name in a model.
    pub const NODE_NAME: usize = 128;
    /// A property, event-source or function name.
    pub const PROP_NAME: usize = 256;
    /// A property value as JSON text.
    pub const PROP_JSON: usize = 64 * 1024;
    /// A record type code, for example `"NPC_"`.
    pub const RECORD_TYPE: usize = 16;
    /// A behavior graph event name.
    pub const ANIM_EVENT: usize = 128;
    /// Packed behavior graph variables of one kind.
    pub const ANIM_VARS: usize = 8 * 1024;
    /// A movement run mode.
    pub const RUN_MODE: usize = 32;
    /// A custom packet, JSON text.
    pub const CUSTOM_PACKET_JSON: usize = 1024 * 1024;
    /// A gamemode event name.
    pub const EVENT_NAME: usize = 256;
    /// Arguments of one gamemode event.
    pub const EVENT_ARGS: usize = 64;
    /// One gamemode event argument, JSON text.
    pub const EVENT_ARG_JSON: usize = 64 * 1024;
    /// A console command name.
    pub const CONSOLE_COMMAND: usize = 64;
    /// Arguments of one console command.
    pub const CONSOLE_ARGS: usize = 32;
    /// One console command argument.
    pub const CONSOLE_ARG: usize = 256;
    /// A Papyrus class, function or type name.
    pub const PAPYRUS_NAME: usize = 128;
    /// Arguments of one delegated Papyrus call.
    pub const SNIPPET_ARGS: usize = 64;
    /// A Papyrus string argument or return value.
    pub const SNIPPET_STRING: usize = 16 * 1024;
    /// Entries in one gamemode code list.
    pub const GAMEMODE_ENTRIES: usize = 256;
    /// One gamemode function's JavaScript source.
    pub const GAMEMODE_SOURCE: usize = 256 * 1024;
    /// Gamemode properties on one form.
    pub const CUSTOM_PROPS: usize = 256;
}

/// Newtype over an engine form id. Validated against the server's load order
/// before it is used as a key anywhere.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
pub struct FormId(pub u32);

/// Server-assigned client id; never client-supplied.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
pub struct ClientId(pub u64);

/// Position and rotation in engine units. Bounds are enforced in
/// `wire-validate`, not here; this type only has to be representable.
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
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
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
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
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
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
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
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
#[derive(Clone, Copy, Debug, PartialEq, Serialize, Deserialize)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
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
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
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

/// Which way a message may travel.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Direction {
    /// Only a client sends it.
    ClientToServer,
    /// Only the server sends it.
    ServerToClient,
    /// Either sends it (SkyMP relays these, or sends its own).
    Both,
}

/// Every message on the wire. Variant order is the id; append only.
///
/// The variants differ a lot in size (CreateActor's props against a
/// DestroyActor's one index), and wire types carry no `Box` (wire rules), so
/// the lint is silenced for that reason only; the large collections inside
/// are heap backed, so no value is large.
#[allow(clippy::large_enum_variant)]
#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[non_exhaustive]
pub enum Message {
    /// Id 0: Client to server, R0. See [`Hello`].
    Hello(Hello),
    /// Id 1: Server to client, R0. Answers `Hello`; sent once per connection.
    Welcome {
        /// The id the server will use for this client.
        client_id: ClientId,
        /// Server clock at send time, milliseconds.
        server_time_ms: u64,
    },
    /// Id 2: Server to client, R0. Why the server is dropping the connection;
    /// the code is a `wire-transport` reason code. Sent at most once.
    Refuse {
        /// Reason code.
        reason: u16,
    },
    /// Id 3: Client to server, R1. See [`MovementSample`].
    Movement(MovementSample),
    /// Id 4: Client to server, R1. See [`HitIntent`].
    Hit(HitIntent),
    /// Id 5: Host to server, R2. See [`HostedActorState`].
    HostedActor(HostedActorState),
    /// Id 6: Server to clients, R0 output: one applied inventory change for
    /// clients to render. The server sends each applied delta exactly once
    /// over ReliableUnordered; clients apply what they receive.
    InventoryApply {
        /// Whose inventory.
        owner: FormId,
        /// The change.
        delta: ItemDelta,
    },
    /// Id 7: Server to one client, R0 output: you now host `cell`. Idempotent.
    HostGrant {
        /// The cell form.
        cell: FormId,
    },
    /// Id 8: Server to one client, R0 output: stop hosting `cell`. Idempotent.
    HostRelease {
        /// The cell form.
        cell: FormId,
    },
    /// 9, MsgType 1. See [`skymp::CustomPacket`].
    CustomPacket(skymp::CustomPacket),
    /// 10, MsgType 2. See [`skymp::UpdateMovement`].
    UpdateMovement(skymp::UpdateMovement),
    /// 11, MsgType 3. See [`skymp::UpdateAnimation`].
    UpdateAnimation(skymp::UpdateAnimation),
    /// 12, MsgType 4. See [`skymp::UpdateAppearance`].
    UpdateAppearance(skymp::UpdateAppearance),
    /// 13, MsgType 5. See [`skymp::UpdateEquipment`].
    UpdateEquipment(skymp::UpdateEquipment),
    /// 14, MsgType 6. See [`skymp::Activate`].
    Activate(skymp::Activate),
    /// 15, MsgType 7. See [`skymp::UpdateProperty`].
    UpdateProperty(skymp::UpdateProperty),
    /// 16, MsgType 8. See [`skymp::PutItem`].
    PutItem(skymp::PutItem),
    /// 17, MsgType 9. See [`skymp::TakeItem`].
    TakeItem(skymp::TakeItem),
    /// 18, MsgType 10. See [`skymp::FinishSpSnippet`].
    FinishSpSnippet(skymp::FinishSpSnippet),
    /// 19, MsgType 11. See [`skymp::OnEquip`].
    OnEquip(skymp::OnEquip),
    /// 20, MsgType 12. See [`skymp::ConsoleCommand`].
    ConsoleCommand(skymp::ConsoleCommand),
    /// 21, MsgType 13. See [`skymp::CraftItem`].
    CraftItem(skymp::CraftItem),
    /// 22, MsgType 14. See [`skymp::Host`].
    Host(skymp::Host),
    /// 23, MsgType 15. See [`skymp::CustomEvent`].
    CustomEvent(skymp::CustomEvent),
    /// 24, MsgType 16. See [`skymp::ChangeValues`].
    ChangeValues(skymp::ChangeValues),
    /// 25, MsgType 17. See [`skymp::OnHit`].
    OnHit(skymp::OnHit),
    /// 26, MsgType 18. See [`skymp::DeathStateContainer`].
    DeathStateContainer(skymp::DeathStateContainer),
    /// 27, MsgType 19. See [`skymp::DropItem`].
    DropItem(skymp::DropItem),
    /// 28, MsgType 20. See [`skymp::Teleport`].
    Teleport(skymp::Teleport),
    /// 29, MsgType 21. See [`skymp::OpenContainer`].
    OpenContainer(skymp::OpenContainer),
    /// 30, MsgType 22. See [`skymp::PlayerBowShot`].
    PlayerBowShot(skymp::PlayerBowShot),
    /// 31, MsgType 23. See [`skymp::SpellCast`].
    SpellCast(skymp::SpellCast),
    /// 32, MsgType 24. See [`skymp::UpdateAnimVariables`].
    UpdateAnimVariables(skymp::UpdateAnimVariables),
    /// 33, MsgType 25. See [`skymp::DestroyActor`].
    DestroyActor(skymp::DestroyActor),
    /// 34, MsgType 26. See [`skymp::HostStart`].
    HostStart(skymp::HostStart),
    /// 35, MsgType 27. See [`skymp::HostStop`].
    HostStop(skymp::HostStop),
    /// 36, MsgType 28. See [`skymp::SetInventory`].
    SetInventory(skymp::SetInventory),
    /// 37, MsgType 29. See [`skymp::SetRaceMenuOpen`].
    SetRaceMenuOpen(skymp::SetRaceMenuOpen),
    /// 38, MsgType 30. See [`skymp::SpSnippet`].
    SpSnippet(skymp::SpSnippet),
    /// 39, MsgType 31. See [`skymp::Teleport2`].
    Teleport2(skymp::Teleport2),
    /// 40, MsgType 32. See [`skymp::UpdateGamemodeData`].
    UpdateGamemodeData(skymp::UpdateGamemodeData),
    /// 41, MsgType 33. See [`skymp::CreateActor`].
    CreateActor(skymp::CreateActor),
}

/// One row per wire id: name, SkyMP MsgType (0 for the M0 family), the byte
/// cap the codec enforces before decoding, and the direction.
struct Row {
    name: &'static str,
    msg_type: u8,
    max_len: usize,
    direction: Direction,
}

const KIB: usize = 1024;
const MIB: usize = 1024 * 1024;

const fn row(name: &'static str, msg_type: u8, max_len: usize, direction: Direction) -> Row {
    Row {
        name,
        msg_type,
        max_len,
        direction,
    }
}

use Direction::{Both, ClientToServer as C2S, ServerToClient as S2C};

/// The table, indexed by wire id. Byte caps are the largest legal encoding
/// with room to spare, from the capacities above; the transport's own
/// per-direction cap (smaller from clients) applies on top.
const TABLE: [Row; 42] = [
    row("Hello", 0, 4 * KIB, C2S),
    row("Welcome", 0, 32, S2C),
    row("Refuse", 0, 8, S2C),
    row("Movement", 0, 64, C2S),
    row("Hit", 0, 64, C2S),
    row("HostedActor", 0, KIB, C2S),
    row("InventoryApply", 0, 32, S2C),
    row("HostGrant", 0, 16, S2C),
    row("HostRelease", 0, 16, S2C),
    row("CustomPacket", 1, MIB + 16, Both),
    row("UpdateMovement", 2, 256, Both),
    row("UpdateAnimation", 3, 256, Both),
    row("UpdateAppearance", 4, 64 * KIB, Both),
    row("UpdateEquipment", 5, 256 * KIB, Both),
    row("Activate", 6, 64, C2S),
    row("UpdateProperty", 7, 65 * KIB, S2C),
    row("PutItem", 8, 512, C2S),
    row("TakeItem", 9, 512, C2S),
    row("FinishSpSnippet", 10, 17 * KIB, C2S),
    row("OnEquip", 11, 16, C2S),
    row("ConsoleCommand", 12, 16 * KIB, C2S),
    row("CraftItem", 13, 64 * KIB, C2S),
    row("Host", 14, 16, C2S),
    row("CustomEvent", 15, 256 * KIB, C2S),
    row("ChangeValues", 16, 32, Both),
    row("OnHit", 17, 64, C2S),
    row("DeathStateContainer", 18, 66 * KIB, S2C),
    row("DropItem", 19, 32, C2S),
    row("Teleport", 20, 64, S2C),
    row("OpenContainer", 21, 16, S2C),
    row("PlayerBowShot", 22, 32, C2S),
    row("SpellCast", 23, 32 * KIB, Both),
    row("UpdateAnimVariables", 24, 32 * KIB, Both),
    row("DestroyActor", 25, 16, S2C),
    row("HostStart", 26, 16, S2C),
    row("HostStop", 27, 16, S2C),
    row("SetInventory", 28, 2 * MIB, S2C),
    row("SetRaceMenuOpen", 29, 8, S2C),
    row("SpSnippet", 30, MIB, S2C),
    row("Teleport2", 31, 64, S2C),
    row("UpdateGamemodeData", 32, 4 * MIB, S2C),
    row("CreateActor", 33, 2 * MIB, S2C),
];

/// Wire ids in use: one past the last variant.
pub const WIRE_IDS: u32 = 42;

/// The wire id of the first SkyMP variant; `wire id = MsgType + SKYMP_OFFSET`.
pub const SKYMP_OFFSET: u32 = 8;

/// The largest byte cap of any message: nothing longer is ever decoded.
pub const MAX_ENCODED_LEN: usize = 4 * MIB;

impl Message {
    /// Largest encoding of any message; inputs longer than this are refused
    /// before decoding (the per-variant cap is [`Message::max_len`]).
    pub const MAX_ENCODED_LEN: usize = MAX_ENCODED_LEN;

    /// The byte cap for the variant with this wire id, or `None` for an id
    /// that names no variant.
    pub fn max_len(wire_id: u32) -> Option<usize> {
        usize::try_from(wire_id)
            .ok()
            .and_then(|i| TABLE.get(i))
            .map(|r| r.max_len)
    }

    /// This message's wire id: its variant's position.
    pub fn wire_id(&self) -> u32 {
        match self {
            Message::Hello(_) => 0,
            Message::Welcome { .. } => 1,
            Message::Refuse { .. } => 2,
            Message::Movement(_) => 3,
            Message::Hit(_) => 4,
            Message::HostedActor(_) => 5,
            Message::InventoryApply { .. } => 6,
            Message::HostGrant { .. } => 7,
            Message::HostRelease { .. } => 8,
            Message::CustomPacket(_) => 9,
            Message::UpdateMovement(_) => 10,
            Message::UpdateAnimation(_) => 11,
            Message::UpdateAppearance(_) => 12,
            Message::UpdateEquipment(_) => 13,
            Message::Activate(_) => 14,
            Message::UpdateProperty(_) => 15,
            Message::PutItem(_) => 16,
            Message::TakeItem(_) => 17,
            Message::FinishSpSnippet(_) => 18,
            Message::OnEquip(_) => 19,
            Message::ConsoleCommand(_) => 20,
            Message::CraftItem(_) => 21,
            Message::Host(_) => 22,
            Message::CustomEvent(_) => 23,
            Message::ChangeValues(_) => 24,
            Message::OnHit(_) => 25,
            Message::DeathStateContainer(_) => 26,
            Message::DropItem(_) => 27,
            Message::Teleport(_) => 28,
            Message::OpenContainer(_) => 29,
            Message::PlayerBowShot(_) => 30,
            Message::SpellCast(_) => 31,
            Message::UpdateAnimVariables(_) => 32,
            Message::DestroyActor(_) => 33,
            Message::HostStart(_) => 34,
            Message::HostStop(_) => 35,
            Message::SetInventory(_) => 36,
            Message::SetRaceMenuOpen(_) => 37,
            Message::SpSnippet(_) => 38,
            Message::Teleport2(_) => 39,
            Message::UpdateGamemodeData(_) => 40,
            Message::CreateActor(_) => 41,
        }
    }

    fn row(&self) -> Option<&'static Row> {
        usize::try_from(self.wire_id())
            .ok()
            .and_then(|i| TABLE.get(i))
    }

    /// The variant's name, stable, for logs and the differential harness.
    pub fn name(&self) -> &'static str {
        self.row().map_or("?", |r| r.name)
    }

    /// SkyMP's MsgType for the SkyMP family; `None` for the M0 family.
    pub fn msg_type(&self) -> Option<u8> {
        self.row().map(|r| r.msg_type).filter(|t| *t != 0)
    }

    /// Which way this message may travel.
    pub fn direction(&self) -> Direction {
        self.row().map_or(Direction::Both, |r| r.direction)
    }
}

/// The name of the variant with this SkyMP MsgType, if there is one.
pub fn name_of_msg_type(msg_type: u8) -> Option<&'static str> {
    TABLE
        .iter()
        .find(|r| r.msg_type == msg_type && msg_type != 0)
        .map(|r| r.name)
}

#[cfg(test)]
mod tests {
    use super::*;
    use alloc::vec;

    #[test]
    fn names_are_the_variant_identifiers() {
        assert_eq!(Message::Refuse { reason: 0 }.name(), "Refuse");
        assert_eq!(Message::HostGrant { cell: FormId(1) }.name(), "HostGrant");
        assert_eq!(
            Message::CreateActor(skymp::CreateActor::default()).name(),
            "CreateActor"
        );
    }

    #[test]
    fn skymp_wire_ids_are_msg_type_plus_offset() {
        for (i, r) in TABLE.iter().enumerate() {
            let id = u32::try_from(i).unwrap_or(u32::MAX);
            if r.msg_type != 0 {
                assert_eq!(id, u32::from(r.msg_type) + SKYMP_OFFSET, "{}", r.name);
            }
        }
        assert_eq!(
            Message::UpdateMovement(skymp::UpdateMovement::default()).msg_type(),
            Some(2)
        );
        assert_eq!(Message::Refuse { reason: 1 }.msg_type(), None);
        assert_eq!(name_of_msg_type(33), Some("CreateActor"));
        assert_eq!(name_of_msg_type(0), None);
        assert_eq!(name_of_msg_type(34), None);
    }

    #[test]
    fn wire_id_matches_postcard_variant_index() {
        let samples = vec![
            Message::Refuse { reason: 7 },
            Message::CustomPacket(skymp::CustomPacket::default()),
            Message::UpdateMovement(skymp::UpdateMovement::default()),
            Message::DeathStateContainer(skymp::DeathStateContainer::default()),
            Message::CreateActor(skymp::CreateActor::default()),
        ];
        for m in samples {
            let bytes = postcard::to_allocvec(&m).unwrap_or_default();
            // the first byte is the variant index (a varint; every id fits in one byte)
            assert_eq!(bytes.first().copied().map(u32::from), Some(m.wire_id()), "{}", m.name());
            assert!(bytes.len() <= Message::max_len(m.wire_id()).unwrap_or(0), "{}", m.name());
        }
    }

    #[test]
    fn caps_hold_at_decode() {
        type Three = bounded::Vec<u8, 3>;
        let ok = postcard::to_allocvec(&alloc::vec![1u8, 2, 3]).unwrap_or_default();
        assert_eq!(postcard::from_bytes::<Three>(&ok).map(|v| v.len()), Ok(3));
        let over = postcard::to_allocvec(&alloc::vec![1u8, 2, 3, 4]).unwrap_or_default();
        assert!(postcard::from_bytes::<Three>(&over).is_err());
        type Short = bounded::String<4>;
        let s = postcard::to_allocvec("hello").unwrap_or_default();
        assert!(postcard::from_bytes::<Short>(&s).is_err());
        let s = postcard::to_allocvec("hell").unwrap_or_default();
        assert_eq!(postcard::from_bytes::<Short>(&s).map(|v| v.len()), Ok(4));
    }

    #[test]
    fn a_huge_length_prefix_allocates_nothing_and_fails() {
        // varint 0xFFFFFFFF as a sequence length, then nothing
        let bytes = [0xFF, 0xFF, 0xFF, 0xFF, 0x0F];
        assert!(postcard::from_bytes::<bounded::Vec<u64, 4096>>(&bytes).is_err());
        assert!(postcard::from_bytes::<bounded::String<256>>(&bytes).is_err());
    }

    #[test]
    fn msg_t_is_zero_bytes_on_the_wire() {
        let bytes = postcard::to_allocvec(&skymp::DestroyActor { t: skymp::MsgT, idx: 5 })
            .unwrap_or_default();
        assert_eq!(bytes, vec![5u8]);
    }
}
