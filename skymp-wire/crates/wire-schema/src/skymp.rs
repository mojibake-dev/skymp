//! SkyMP's own messages (ADR-019): one struct per MsgType, fields in the
//! order and under the JSON keys of the C++ `Serialize(Archive&)` lists in
//! skymp5-server/cpp/messages and the payload types in server_guest_lib.
//! This file is the contract the C++ core and skymp5-client keep speaking
//! in-process; on the wire the same structs travel as postcard. MsgTypes 1
//! to 33 are SkyMP's; thuum appends its own after them (34, SetGameTime; 35,
//! RestIntent; 36, MapMarkerDiscovered; 37, IngredientEffectsKnown; 38,
//! Favorites; 39, RaceMenuPreset; 40, ActorValues; 41, ConsoleOutput; 42,
//! ArrowShot; 43, MagicEffects; 44, UpdateAction; 45, PlayerGlobals).
//!
//! Directions are SkyMP's: twelve types (fifteen with RestIntent,
//! MapMarkerDiscovered and IngredientEffectsKnown) only travel
//! client to server, thirteen (fourteen with SetGameTime) only server to
//! client (seventeen with ConsoleOutput, ArrowShot and MagicEffects), eight
//! (thirteen with Favorites, RaceMenuPreset, ActorValues, UpdateAction and
//! PlayerGlobals) both ways (the server relays a client's
//! UpdateMovement, UpdateAnimation, UpdateAppearance, UpdateEquipment,
//! SpellCast, UpdateAnimVariables and UpdateAction to its neighbours, and sends its own
//! CustomPacket and ChangeValues). Rungs are the ones SkyMP gives them today;
//! M1's validation work tightens them per verb. Every float a message carries
//! must be finite (`E_VAL_NONFINITE`); that and the capacities below are the
//! structural checks all of them share.

use serde::{Deserialize, Serialize};

use crate::bounded::{String, Vec};
use crate::cap;
pub use crate::shape::{ConsoleArg, MsgT, SnippetArg, SnippetObject, SnippetValue};

/// x, y, z in engine units, or Euler angles in degrees, as SkyMP sends them.
pub type Vec3 = [f32; 3];

macro_rules! wire_struct {
    ($(#[$m:meta])* pub struct $name:ident { $($body:tt)* }) => {
        $(#[$m])*
        #[derive(Clone, Debug, PartialEq, Default, Serialize, Deserialize)]
        #[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
        #[serde(rename_all = "camelCase")]
        pub struct $name { $($body)* }
    };
}

// ---- payload types -------------------------------------------------------

wire_struct! {
    /// Where an actor is: `Transform` in CreateActorMessage.h.
    pub struct Transform {
        /// The worldspace or interior cell form id.
        pub world_or_cell: u32,
        /// Position.
        pub pos: Vec3,
        /// Rotation.
        pub rot: Vec3,
    }
}

wire_struct! {
    /// One tint mask layer (Appearance.h).
    pub struct Tint {
        /// Mask texture path inside the game's archives.
        pub texture_path: String<{ cap::TEXTURE_PATH }>,
        /// Colour, ARGB.
        pub argb: i32,
        /// Tint type index.
        #[serde(rename = "type")]
        pub kind: i32,
    }
}

wire_struct! {
    /// A character's look (Appearance.h). The race menu's output.
    pub struct Appearance {
        /// Sex.
        pub is_female: bool,
        /// Race form id.
        pub race_id: u32,
        /// Body weight, 0 to 100.
        pub weight: f32,
        /// Skin colour, ARGB.
        pub skin_color: i32,
        /// Hair colour, ARGB.
        pub hair_color: i32,
        /// Head part form ids.
        pub headpart_ids: Vec<u32, { cap::HEADPARTS }>,
        /// Face texture set form id.
        pub head_texture_set_id: u32,
        /// Face morph values.
        pub options: Vec<f32, { cap::FACE_OPTIONS }>,
        /// Face preset values.
        pub presets: Vec<f32, { cap::FACE_PRESETS }>,
        /// Tint layers.
        pub tints: Vec<Tint, { cap::TINTS }>,
        /// Character name.
        pub name: String<{ cap::NAME }>,
    }
}

wire_struct! {
    /// One inventory stack with its extra data (Inventory::Entry, whose
    /// ExtraData fields SkyMP writes inline, so they are inline here too).
    pub struct Entry {
        /// Item base form id.
        pub base_id: u32,
        /// Stack size.
        pub count: u32,
        /// Item health (tempering).
        pub health: Option<f32>,
        /// Enchantment form id.
        pub enchantment_id: Option<u32>,
        /// Enchantment charge maximum.
        pub max_charge: Option<f32>,
        /// Whether the enchantment goes when unequipped.
        pub remove_enchantment_on_unequip: Option<bool>,
        /// Charge left, percent.
        pub charge_percent: Option<f32>,
        /// Custom item name.
        pub name: Option<String<{ cap::NAME }>>,
        /// Soul gem fill level.
        pub soul: Option<u8>,
        /// Applied poison form id.
        pub poison_id: Option<u32>,
        /// Poison doses left.
        pub poison_count: Option<u32>,
        /// Worn in the right hand or on the body.
        pub worn: Option<bool>,
        /// Worn in the left hand.
        pub worn_left: Option<bool>,
    }
}

wire_struct! {
    /// An inventory (Inventory.h).
    pub struct Inventory {
        /// The stacks.
        pub entries: Vec<Entry, { cap::INVENTORY }>,
    }
}

wire_struct! {
    /// What an actor has equipped (Equipment.h).
    pub struct Equipment {
        /// The equipped items.
        pub inv: Inventory,
        /// Spell in the left hand.
        pub left_spell: Option<u32>,
        /// Spell in the right hand.
        pub right_spell: Option<u32>,
        /// Equipped shout or power.
        pub voice_spell: Option<u32>,
        /// Instant spell.
        pub instant_spell: Option<u32>,
        /// Change counter; the newest wins.
        pub num_changes: u32,
    }
}

wire_struct! {
    /// The last animation event an actor played (AnimationData.h).
    pub struct AnimationData {
        /// Behavior graph event name.
        pub anim_event_name: String<{ cap::ANIM_EVENT }>,
        /// Change counter; the newest wins.
        pub num_changes: u32,
    }
}

wire_struct! {
    /// Behavior graph variables, packed by the client (SpellCastMessage.h).
    pub struct ActorAnimationVariables {
        /// Packed bool variables.
        pub booleans: Vec<u8, { cap::ANIM_VARS }>,
        /// Packed float variables.
        pub floats: Vec<u8, { cap::ANIM_VARS }>,
        /// Packed int variables.
        pub integers: Vec<u8, { cap::ANIM_VARS }>,
    }
}

wire_struct! {
    /// A node texture override on a reference.
    pub struct SetNodeTextureSetEntry {
        /// Node name in the model.
        pub node_name: String<{ cap::NODE_NAME }>,
        /// Texture set form id.
        pub texture_set_id: u32,
    }
}

wire_struct! {
    /// A node scale override on a reference.
    pub struct SetNodeScaleEntry {
        /// Node name in the model.
        pub node_name: String<{ cap::NODE_NAME }>,
        /// Scale factor.
        pub scale: f32,
    }
}

wire_struct! {
    /// A gamemode-defined property and its value, as JSON text the client's
    /// gamemode code reads.
    pub struct CustomPropsEntry {
        /// Property name.
        pub prop_name: String<{ cap::PROP_NAME }>,
        /// Property value, JSON text.
        pub prop_value_json_dump: String<{ cap::PROP_JSON }>,
    }
}

wire_struct! {
    /// CreateActorMessageAdditionalProps: everything optional about a
    /// reference or actor the server introduces to a client.
    pub struct CreateActorProps {
        /// Door or container open state.
        pub is_open: Option<bool>,
        /// Flora harvested state.
        pub is_harvested: Option<bool>,
        /// Node texture overrides.
        pub set_node_texture_set: Option<Vec<SetNodeTextureSetEntry, { cap::NODES }>>,
        /// Node scale overrides.
        pub set_node_scale: Option<Vec<SetNodeScaleEntry, { cap::NODES }>>,
        /// Disabled state.
        pub is_disabled: Option<bool>,
        /// Last animation event.
        pub last_animation: Option<String<{ cap::ANIM_EVENT }>>,
        /// Display name override.
        pub display_name: Option<String<{ cap::NAME }>>,
        /// Whether another client hosts this actor.
        pub is_hosted_by_other: Option<bool>,
        /// Race menu state.
        pub is_race_menu_open: Option<bool>,
        /// Learned spell form ids.
        pub learned_spells: Option<Vec<u32, { cap::SPELLS }>>,
        /// Health regeneration rate.
        pub heal_rate: Option<f32>,
        /// Health regeneration multiplier.
        pub heal_rate_mult: Option<f32>,
        /// Base health.
        pub health: Option<f32>,
        /// Magicka regeneration rate.
        pub magicka_rate: Option<f32>,
        /// Magicka regeneration multiplier.
        pub magicka_rate_mult: Option<f32>,
        /// Base magicka.
        pub magicka: Option<f32>,
        /// Stamina regeneration rate.
        pub stamina_rate: Option<f32>,
        /// Stamina regeneration multiplier.
        pub stamina_rate_mult: Option<f32>,
        /// Base stamina.
        pub stamina: Option<f32>,
        /// Current health, 0 to 1.
        pub health_percentage: Option<f32>,
        /// Current stamina, 0 to 1.
        pub stamina_percentage: Option<f32>,
        /// Current magicka, 0 to 1.
        pub magicka_percentage: Option<f32>,
        /// Leveled template chain form ids.
        pub template_chain: Option<Vec<u32, { cap::TEMPLATE_CHAIN }>>,
        /// Inventory.
        pub inventory: Option<Inventory>,
        /// Dead state (also in the main props; SkyMP writes both).
        pub is_dead: Option<bool>,
        /// thuum docs/verbs/locks.md: the reference's lock as the server
        /// holds it (the XLOC's level byte, the key's form id or 0, locked
        /// now); absent for a reference the game files leave unlocked. The
        /// client applies it to its own reference instead of stripping the
        /// lock, so the engine's own lockpicking runs when the server
        /// prompts it (`LockPrompt`).
        pub lock: Option<LockProp>,
        /// thuum docs/verbs/spell-placed-object.md (ADR-057): the server id
        /// of the caster whose accepted cast of a listed spell placed this
        /// reference (the server's own MSTT or ACTI from its table); absent
        /// for every other form. The caster's game skips a form naming
        /// itself while its cast is open, since its engine runs the script's
        /// own copies. Nonzero, and only with `base_record_type` MSTT or
        /// ACTI (`E_VAL_RANGE`).
        pub placed_by: Option<u32>,
    }
}

wire_struct! {
    /// One gamemode function or event source: a name and its JavaScript
    /// source, which the client evaluates (SkyMP's design: the server is
    /// trusted to that extent).
    pub struct GamemodeValuePair {
        /// Property or event name.
        pub name: String<{ cap::PROP_NAME }>,
        /// JavaScript source.
        pub content: String<{ cap::GAMEMODE_SOURCE }>,
    }
}

// ---- messages, by MsgType ------------------------------------------------

wire_struct! {
    /// MsgType 1. Both directions, reliable. Gamemode-defined JSON text
    /// (`contentJsonDump`), for example the login packet; the gamemode reads
    /// it, so its rung is the gamemode's. Not idempotent in general.
    pub struct CustomPacket {
        /// `"t": 1`.
        #[serde(default)]
        pub t: MsgT<1>,
        /// The packet, JSON text.
        pub content_json_dump: String<{ cap::CUSTOM_PACKET_JSON }>,
    }
}

wire_struct! {
    /// UpdateMovementMessage's `data`.
    pub struct MovementData {
        /// The worldspace or interior cell form id.
        pub world_or_cell: u32,
        /// Position.
        pub pos: Vec3,
        /// Rotation.
        pub rot: Vec3,
        /// Movement direction, degrees.
        pub direction: f32,
        /// Health, 0 to 1.
        pub health_percentage: f32,
        /// Speed, engine units per second.
        pub speed: f32,
        /// `"Standing"`, `"Walking"`, `"Running"` or `"Sprinting"`.
        pub run_mode: String<{ cap::RUN_MODE }>,
        /// Jumping.
        pub is_in_jump_state: bool,
        /// Sneaking.
        pub is_sneaking: bool,
        /// Blocking.
        pub is_blocking: bool,
        /// Weapon drawn.
        pub is_weap_drawn: bool,
        /// Dead.
        pub is_dead: bool,
        /// Head look target.
        pub look_at: Option<Vec3>,
    }
}

wire_struct! {
    /// MsgType 2. Both directions, unreliable: the client's sample of the
    /// player or of an actor it hosts (R1 input; SkyMP applies it), relayed
    /// by the server to neighbours. A later sample wins, so a duplicate is
    /// harmless. Reasons: `E_VAL_NONFINITE`, `E_VAL_OUT_OF_WORLD`.
    pub struct UpdateMovement {
        /// `"t": 2`.
        #[serde(default)]
        pub t: MsgT<2>,
        /// The actor, as the receiver indexes it.
        pub idx: u32,
        /// The sample.
        pub data: MovementData,
    }
}

wire_struct! {
    /// MsgType 3. Both directions, unreliable: an animation event the client
    /// saw (R2, recorded and relayed). `numChanges` orders them; a duplicate is
    /// harmless.
    pub struct UpdateAnimation {
        /// `"t": 3`.
        #[serde(default)]
        pub t: MsgT<3>,
        /// The actor.
        pub idx: u32,
        /// The event.
        pub data: AnimationData,
    }
}

wire_struct! {
    /// MsgType 4. Both directions, reliable: the race menu's result (the
    /// server accepts it only while the race menu is open) and its relay.
    /// Idempotent: it replaces the look.
    pub struct UpdateAppearance {
        /// `"t": 4`.
        #[serde(default)]
        pub t: MsgT<4>,
        /// The actor.
        pub idx: u32,
        /// The look; absent clears it.
        pub data: Option<Appearance>,
    }
}

wire_struct! {
    /// MsgType 5. Both directions, reliable: what an actor wears (R2 as
    /// SkyMP stands; the inventory itself is R0) and its relay. `numChanges`
    /// orders them; idempotent.
    pub struct UpdateEquipment {
        /// `"t": 5`.
        #[serde(default)]
        pub t: MsgT<5>,
        /// The actor.
        pub idx: u32,
        /// The equipment.
        pub data: Equipment,
    }
}

wire_struct! {
    /// ActivateMessage's `data`, which repeats `"t"`.
    pub struct ActivateData {
        /// `"t": 6` again.
        #[serde(default)]
        pub t: MsgT<6>,
        /// Who activates.
        pub caster: u64,
        /// What is activated.
        pub target: u64,
        /// The engine's second activation of the same reference.
        pub is_second_activation: bool,
    }
}

wire_struct! {
    /// MsgType 6. Client to server, reliable: activate a reference (R1
    /// intent, the server decides). Not idempotent.
    pub struct Activate {
        /// `"t": 6`.
        #[serde(default)]
        pub t: MsgT<6>,
        /// The activation.
        pub data: ActivateData,
    }
}

wire_struct! {
    /// MsgType 7. Server to client, reliable: one property of a form changed
    /// (R0 output). Idempotent: it sets the value.
    pub struct UpdateProperty {
        /// `"t": 7`.
        #[serde(default)]
        pub t: MsgT<7>,
        /// The form, as the receiver indexes it.
        pub idx: u32,
        /// Property name.
        pub prop_name: String<{ cap::PROP_NAME }>,
        /// Reference id.
        pub refr_id: u32,
        /// The value, JSON text.
        pub data_dump: String<{ cap::PROP_JSON }>,
        /// Base record type, for references.
        pub base_record_type: Option<String<{ cap::RECORD_TYPE }>>,
    }
}

wire_struct! {
    /// MsgType 8. Client to server, reliable: the player put an item into a
    /// container (R1 intent, the server moves it). Not idempotent.
    pub struct PutItem {
        /// `"t": 8`.
        #[serde(default)]
        pub t: MsgT<8>,
        /// Item base form id.
        pub base_id: u32,
        /// How many.
        pub count: u32,
        /// The container.
        pub target: u32,
        /// Item health.
        pub health: Option<f32>,
        /// Enchantment form id.
        pub enchantment_id: Option<u32>,
        /// Enchantment charge maximum.
        pub max_charge: Option<f32>,
        /// Whether the enchantment goes when unequipped.
        pub remove_enchantment_on_unequip: Option<bool>,
        /// Charge left, percent.
        pub charge_percent: Option<f32>,
        /// Custom item name.
        pub name: Option<String<{ cap::NAME }>>,
        /// Soul gem fill level.
        pub soul: Option<u8>,
        /// Applied poison form id.
        pub poison_id: Option<u32>,
        /// Poison doses left.
        pub poison_count: Option<u32>,
        /// Worn in the right hand or on the body.
        pub worn: Option<bool>,
        /// Worn in the left hand.
        pub worn_left: Option<bool>,
    }
}

wire_struct! {
    /// MsgType 9. Client to server, reliable: the player took an item from a
    /// container (R1 intent, the server moves it). Not idempotent.
    pub struct TakeItem {
        /// `"t": 9`.
        #[serde(default)]
        pub t: MsgT<9>,
        /// Item base form id.
        pub base_id: u32,
        /// How many.
        pub count: u32,
        /// The container.
        pub target: u32,
        /// Item health.
        pub health: Option<f32>,
        /// Enchantment form id.
        pub enchantment_id: Option<u32>,
        /// Enchantment charge maximum.
        pub max_charge: Option<f32>,
        /// Whether the enchantment goes when unequipped.
        pub remove_enchantment_on_unequip: Option<bool>,
        /// Charge left, percent.
        pub charge_percent: Option<f32>,
        /// Custom item name.
        pub name: Option<String<{ cap::NAME }>>,
        /// Soul gem fill level.
        pub soul: Option<u8>,
        /// Applied poison form id.
        pub poison_id: Option<u32>,
        /// Poison doses left.
        pub poison_count: Option<u32>,
        /// Worn in the right hand or on the body.
        pub worn: Option<bool>,
        /// Worn in the left hand.
        pub worn_left: Option<bool>,
    }
}

wire_struct! {
    /// MsgType 10. Client to server, reliable: the result of a delegated
    /// Papyrus call (R2, recorded). Answers one SpSnippet by `snippetIdx`.
    pub struct FinishSpSnippet {
        /// `"t": 10`.
        #[serde(default)]
        pub t: MsgT<10>,
        /// The call's result, if it has one.
        pub return_value: Option<SnippetValue>,
        /// Which snippet this answers.
        pub snippet_idx: i64,
    }
}

wire_struct! {
    /// MsgType 11. Client to server, unreliable: the player equipped an item
    /// (R2, the server runs the item's scripts). Not idempotent.
    pub struct OnEquip {
        /// `"t": 11`.
        #[serde(default)]
        pub t: MsgT<11>,
        /// Item base form id.
        pub base_id: u32,
    }
}

wire_struct! {
    /// ConsoleCommandMessage's `data`, which repeats `"t"`.
    pub struct ConsoleCommandData {
        /// `"t": 12` again.
        #[serde(default)]
        pub t: MsgT<12>,
        /// Command name, for example `"AddItem"`.
        pub command_name: String<{ cap::CONSOLE_COMMAND }>,
        /// Arguments.
        pub args: Vec<ConsoleArg, { cap::CONSOLE_ARGS }>,
    }
}

wire_struct! {
    /// MsgType 12. Client to server, reliable: a console command for the
    /// server to run (R0: the server checks the caller's rights). Not
    /// idempotent.
    pub struct ConsoleCommand {
        /// `"t": 12`.
        #[serde(default)]
        pub t: MsgT<12>,
        /// The command.
        pub data: ConsoleCommandData,
    }
}

wire_struct! {
    /// CraftItemMessage's `data`, which repeats `"t"`.
    pub struct CraftItemData {
        /// `"t": 13` again.
        #[serde(default)]
        pub t: MsgT<13>,
        /// The crafting station reference.
        pub workbench: u32,
        /// What the recipe consumed.
        pub craft_input_objects: Inventory,
        /// What it made.
        pub result_object_id: u32,
    }
}

wire_struct! {
    /// MsgType 13. Client to server, reliable: the player crafted (R1
    /// intent; the server checks the recipe and moves the items). Not
    /// idempotent.
    pub struct CraftItem {
        /// `"t": 13`.
        #[serde(default)]
        pub t: MsgT<13>,
        /// The craft.
        pub data: CraftItemData,
    }
}

wire_struct! {
    /// MsgType 14. Client to server, unreliable: the client asks to host an
    /// actor (R0, the server decides). Idempotent.
    pub struct Host {
        /// `"t": 14`.
        #[serde(default)]
        pub t: MsgT<14>,
        /// The actor's remote id.
        pub remote_id: u64,
    }
}

wire_struct! {
    /// MsgType 15. Client to server, reliable: a gamemode event with JSON
    /// arguments (the gamemode's rung). Not idempotent.
    pub struct CustomEvent {
        /// `"t": 15`.
        #[serde(default)]
        pub t: MsgT<15>,
        /// Arguments, JSON text each.
        pub args_json_dumps: Vec<String<{ cap::EVENT_ARG_JSON }>, { cap::EVENT_ARGS }>,
        /// Event name.
        pub event_name: String<{ cap::EVENT_NAME }>,
    }
}

wire_struct! {
    /// ChangeValuesMessage's `data`: attribute percentages, 0 to 1.
    pub struct ChangeValuesData {
        /// Health.
        pub health: Option<f32>,
        /// Magicka.
        pub magicka: Option<f32>,
        /// Stamina.
        pub stamina: Option<f32>,
    }
}

wire_struct! {
    /// MsgType 16. Both directions, unreliable: attribute percentages, the
    /// client's (R2 as SkyMP stands, cropped by the server's regeneration
    /// model) and the server's (R0 output). Idempotent: it sets values.
    pub struct ChangeValues {
        /// `"t": 16`.
        #[serde(default)]
        pub t: MsgT<16>,
        /// The actor; absent means the receiver's own.
        pub idx: Option<u32>,
        /// The values.
        pub data: ChangeValuesData,
    }
}

wire_struct! {
    /// HitMessage's `data`.
    pub struct HitData {
        /// Who hit.
        pub aggressor: u32,
        /// Bash attack.
        pub is_bash_attack: bool,
        /// Blocked.
        pub is_hit_blocked: bool,
        /// Power attack.
        pub is_power_attack: bool,
        /// Sneak attack.
        pub is_sneak_attack: bool,
        /// Projectile form id.
        pub projectile: u32,
        /// Weapon or spell form id.
        pub source: u32,
        /// Who was hit.
        pub target: u32,
    }
}

wire_struct! {
    /// MsgType 17. Client to server, reliable: a hit the client saw (R1
    /// intent; the server computes the damage). Not idempotent.
    pub struct OnHit {
        /// `"t": 17`.
        #[serde(default)]
        pub t: MsgT<17>,
        /// The hit.
        pub data: HitData,
    }
}

wire_struct! {
    /// MsgType 18. Server to client, reliable: death or resurrection, as up
    /// to three complete messages applied together (R0 output). Idempotent.
    pub struct DeathStateContainer {
        /// `"t": 18`.
        #[serde(default)]
        pub t: MsgT<18>,
        /// Where to put the actor.
        pub t_teleport: Option<Teleport>,
        /// Its attributes.
        pub t_change_values: Option<ChangeValues>,
        /// The `isDead` property.
        pub t_is_dead: Option<UpdateProperty>,
    }
}

wire_struct! {
    /// MsgType 19. Client to server, reliable: the player dropped an item
    /// (R1 intent). Not idempotent.
    pub struct DropItem {
        /// `"t": 19`.
        #[serde(default)]
        pub t: MsgT<19>,
        /// Item base form id.
        pub base_id: u64,
        /// How many.
        pub count: u32,
    }
}

wire_struct! {
    /// MsgType 20. Server to client, reliable: move an actor (R0 output).
    /// Idempotent.
    pub struct Teleport {
        /// `"t": 20`.
        #[serde(default)]
        pub t: MsgT<20>,
        /// The actor.
        pub idx: u32,
        /// Position.
        pub pos: Vec3,
        /// Rotation.
        pub rot: Vec3,
        /// The worldspace or interior cell form id.
        pub world_or_cell: u32,
    }
}

wire_struct! {
    /// MsgType 21. Server to client, reliable: open a container's menu (R0
    /// output). Idempotent.
    pub struct OpenContainer {
        /// `"t": 21`.
        #[serde(default)]
        pub t: MsgT<21>,
        /// The container.
        pub target: u32,
    }
}

wire_struct! {
    /// MsgType 22. Client to server, reliable: the player loosed an arrow
    /// (R1 intent; thuum docs/verbs/marksman.md), or an actor its game
    /// simulates did (`idx`; thuum docs/verbs/npc-stream.md, train 53). Not
    /// idempotent. Reason codes: `E_VAL_NONFINITE`, `E_VAL_RANGE` (an idx of
    /// u32::MAX, a power outside 0 to 1, an
    /// aim or a launch pitched past a quarter turn, a launch's fields set
    /// without `has_launch`), `E_VAL_OUT_OF_WORLD` (a launch's origin),
    /// `E_VAL_RATE`.
    pub struct PlayerBowShot {
        /// `"t": 22`.
        #[serde(default)]
        pub t: MsgT<22>,
        /// The shooter, as CreateActor named it: the sender's own idx for its
        /// own shot, a hosted actor's for one its game simulates (thuum
        /// docs/verbs/npc-stream.md, train 53). 0 is a real actor, so the
        /// sender's own idx is the sentinel (slice 3's idiom), never 0.
        pub idx: u32,
        /// Bow form id.
        pub weapon_id: u32,
        /// Arrow form id.
        pub ammo_id: u32,
        /// Draw power, 0 to 1.
        pub power: f32,
        /// Aimed at the sun (the engine's quirk).
        pub is_sun_gazing: bool,
        /// Aim pitch when the arrow left, radians (the engine's
        /// Actor::GetAimAngle), within a quarter turn either way.
        pub aim_angle: f32,
        /// Aim heading when the arrow left, radians (Actor::GetAimHeading).
        pub aim_heading: f32,
        /// The shooter's game saw its own arrow's launch (Projectile::Launch's
        /// LaunchData, Skyrim Platform's projectileLaunched) and the five
        /// fields after this one are it; false, they are all zero.
        pub has_launch: bool,
        /// Where the engine launched the arrow, x, engine units.
        pub origin_x: f32,
        /// Where the engine launched the arrow, y, engine units.
        pub origin_y: f32,
        /// Where the engine launched the arrow, z, engine units.
        pub origin_z: f32,
        /// The launch's pitch, radians (LaunchData angleX), within a quarter
        /// turn either way.
        pub launch_angle_x: f32,
        /// The launch's heading, radians (LaunchData angleZ).
        pub launch_angle_z: f32,
    }
}

wire_struct! {
    /// MsgType 42 (thuum, docs/verbs/marksman.md). Server to client,
    /// reliable: a neighbour's shot the server took, for the receiving game
    /// to launch the arrow from that player's figure. The shooter's game made
    /// the shot (R1) and the server checked it; the figure's arrow is drawn,
    /// its damage the server's. Reason codes: `E_VAL_NONFINITE`, and for
    /// its launch, as the shot's, `E_VAL_RANGE` and `E_VAL_OUT_OF_WORLD`.
    pub struct ArrowShot {
        /// `"t": 42`.
        #[serde(default)]
        pub t: MsgT<42>,
        /// The shooter, as CreateActor named it.
        pub idx: u32,
        /// Bow form id.
        pub weapon_id: u32,
        /// Arrow form id.
        pub ammo_id: u32,
        /// Draw power, 0 to 1.
        pub power: f32,
        /// Aim pitch, radians.
        pub aim_angle: f32,
        /// Aim heading, radians.
        pub aim_heading: f32,
        /// The shot carried its launch ([`PlayerBowShot::has_launch`]): the
        /// receiving game launches the figure's arrow at this origin with
        /// these angles, so its path is the shooter's; false, all zero, and
        /// the arrow leaves along the aim.
        pub has_launch: bool,
        /// The launch's origin, x, engine units.
        pub origin_x: f32,
        /// The launch's origin, y, engine units.
        pub origin_y: f32,
        /// The launch's origin, z, engine units.
        pub origin_z: f32,
        /// The launch's pitch, radians.
        pub launch_angle_x: f32,
        /// The launch's heading, radians.
        pub launch_angle_z: f32,
    }
}

wire_struct! {
    /// One effect running on an actor, as MagicEffects carries it (thuum
    /// docs/verbs/magic-effects.md).
    pub struct MagicEffect {
        /// The effect's MGEF form id.
        pub effect: u32,
        /// The potion, poison, spell or enchantment that carries it.
        pub source: u32,
        /// Its magnitude, scaled as the server applied it.
        pub magnitude: f32,
        /// Seconds left; 0 for an effect without duration, shown once.
        pub remaining: f32,
    }
}

wire_struct! {
    /// What an actor's running buffs add to one actor value (thuum
    /// docs/verbs/magic-effects.md): the sum the actor's own game holds on
    /// that value while they run, which is how the server's buffs reach the
    /// player's own game (its engine applies no drink of its own).
    pub struct EffectModifier {
        /// The actor value (CommonLibSSE-NG include/RE/A/ActorValues.h).
        pub av: u32,
        /// The running buffs' sum on it.
        pub amount: f32,
    }
}

wire_struct! {
    /// MsgType 43 (thuum, docs/verbs/magic-effects.md). Server to client,
    /// reliable: the effects running on an actor (R0), whenever they change
    /// and in full when a client first sees the actor, for each game to show
    /// them (the effect's hit shader and art on that actor for the seconds
    /// left), and the sums its running buffs put on its actor values, which
    /// the actor's own game holds. Idempotent: each replaces the actor's set.
    /// Sent before the ChangeValues of the same change, on the same ordered
    /// channel, so a game moves its maximum before it takes the percentage.
    pub struct MagicEffects {
        /// `"t": 43`.
        #[serde(default)]
        pub t: MsgT<43>,
        /// The actor, as CreateActor named it.
        pub idx: u32,
        /// Its running effects.
        pub effects: Vec<MagicEffect, { cap::MAGIC_EFFECTS }>,
        /// Its running buffs' sums, one per actor value they touch (none for
        /// a value they leave at 0).
        #[serde(default)]
        pub modifiers: Vec<EffectModifier, { cap::EFFECT_MODIFIERS }>,
    }
}

wire_struct! {
    /// One global's value, as PlayerGlobals carries it (thuum
    /// docs/verbs/player-globals.md).
    pub struct GlobalEntry {
        /// The global's form id in the load order both ends share (the
        /// server keeps it as a FormDesc).
        pub global: u32,
        /// Its value (a TESGlobal holds a float whatever its type).
        pub value: f32,
    }
}

wire_struct! {
    /// MsgType 45, thuum's (docs/verbs/player-globals.md, ADR-029's A2).
    /// Both directions, reliable. Server to client, after a login: the
    /// globals the server keeps per player, to watch, and the player's
    /// recorded values, which the client's game sets. Client to server: the
    /// watched globals whose values changed since its last report (R2:
    /// recorded unvalidated), naming none to watch. Idempotent: the latest
    /// value wins per global. Reason codes: `E_VAL_RANGE` (a global given
    /// twice, or a client's report naming globals to watch),
    /// `E_VAL_NONFINITE`, `E_VAL_RATE` (client to server).
    pub struct PlayerGlobals {
        /// `"t": 45`.
        #[serde(default)]
        pub t: MsgT<45>,
        /// The globals to watch (server to client only).
        #[serde(default)]
        pub watch: Vec<u32, { cap::PLAYER_GLOBALS }>,
        /// Values, each global once.
        pub values: Vec<GlobalEntry, { cap::PLAYER_GLOBALS }>,
    }
}

wire_struct! {
    /// MsgType 57, thuum's (docs/verbs/quest-stage.md, ADR-056; memo D58 B):
    /// the world's quest stages. Both directions, reliable. Client to
    /// server: one stage its own game set on a shared quest (R1: the server
    /// checks the quest against its sharing mode, and the rate). Server to
    /// client: the relay of another seat's stage, the replay of the world's
    /// stages after a login, and at the login itself the sharing mode and
    /// its list (`mode` and `shared`, with `quest` 0 and `stage` 0): in
    /// mode 0 (list) `shared` names the quests the seat reports; in mode 1
    /// (all) every quest is shared and `shared` names the DENIED quests,
    /// which the seat never reports (it may be empty). Idempotent: a stage
    /// already done is nothing. Reason codes: `E_VAL_RANGE` (a mode above
    /// 1, a quest of 0 without a list in mode 0, a list naming a quest
    /// twice, a client naming a list, a mode or a quest of 0), `E_VAL_RATE`
    /// (client to server); the mode's and the record's refusals are the
    /// server's (wire-rules `quest_stages`).
    pub struct QuestStage {
        /// `"t": 57`.
        #[serde(default)]
        pub t: MsgT<57>,
        /// The quest's form id in the load order both ends share (the
        /// server keeps it as a FormDesc); 0 with `shared` at a login.
        pub quest: u32,
        /// The stage index the engine set (Quest.SetStage's).
        pub stage: u16,
        /// The shared quests in mode 0, the denied quests in mode 1 (server
        /// to client only, at a login).
        #[serde(default)]
        pub shared: Vec<u32, { cap::SHARED_QUESTS }>,
        /// The sharing mode (server to client only, at a login): 0 list, 1
        /// all. A client sends 0.
        #[serde(default)]
        pub mode: u8,
    }
}

wire_struct! {
    /// SpellCastMessage's `data`.
    pub struct SpellCastData {
        /// Who casts.
        pub caster: u32,
        /// At whom: the caster's game's guess (its crosshair's actor for a
        /// spell delivered to its target, 0 for none), which the receiver
        /// resolves against its own player (thuum docs/verbs/spell-cast.md).
        /// Train-27: for a spell whose every effect is delivered to its
        /// target (touch, target actor) the server relays the cast only at
        /// its first hit, with that hit's target in this field, and never
        /// when it hits nobody (wire-rules casts::CastFacts::lands_on_target);
        /// a sender's guess is a hint, not trusted alone.
        pub target: u32,
        /// Spell form id.
        pub spell: u32,
        /// Dual casting.
        pub is_dual_casting: bool,
        /// The cast was interrupted.
        pub interrupt_cast: bool,
        /// Left, right, voice or instant.
        pub casting_source: i32,
        /// Aim pitch.
        pub aim_angle: f32,
        /// Aim heading.
        pub aim_heading: f32,
        /// The caster's game saw its own launch of the spell
        /// (Projectile::Launch's LaunchData, Skyrim Platform's
        /// projectileLaunched) and the five fields after this one are it:
        /// for a target-location spell, the point its game picked along the
        /// caster's aim, which each neighbour's game launches the spell at
        /// (thuum docs/verbs/spell-cast.md); false, they are all zero.
        pub has_launch: bool,
        /// Where the engine launched the spell, x, engine units.
        pub origin_x: f32,
        /// Where the engine launched the spell, y, engine units.
        pub origin_y: f32,
        /// Where the engine launched the spell, z, engine units.
        pub origin_z: f32,
        /// The launch's pitch, radians (LaunchData angleX), within a quarter
        /// turn either way.
        pub launch_angle_x: f32,
        /// The launch's heading, radians (LaunchData angleZ).
        pub launch_angle_z: f32,
        /// The caster's behavior graph variables.
        pub actor_animation_variables: ActorAnimationVariables,
    }
}

wire_struct! {
    /// MsgType 23. Both directions, reliable: a spell cast (R1 intent) and
    /// its relay. Not idempotent. Reason codes: `E_VAL_NONFINITE`,
    /// `E_VAL_RANGE` (no spell, a hand SkyMP does not know, an aim or a
    /// launch pitched past a quarter turn, a launch's fields set without
    /// `has_launch`), `E_VAL_OUT_OF_WORLD` (a launch's origin), `E_VAL_RATE`.
    pub struct SpellCast {
        /// `"t": 23`.
        #[serde(default)]
        pub t: MsgT<23>,
        /// The cast.
        pub data: SpellCastData,
    }
}

wire_struct! {
    /// UpdateAnimVariablesMessage's `data`.
    pub struct UpdateAnimVariablesData {
        /// The actor's remote id.
        pub actor_remote_id: u32,
        /// Its behavior graph variables.
        pub actor_animation_variables: ActorAnimationVariables,
    }
}

wire_struct! {
    /// MsgType 24. Both directions: behavior graph variables (R2, recorded
    /// and relayed). Idempotent: they replace.
    pub struct UpdateAnimVariables {
        /// `"t": 24`.
        #[serde(default)]
        pub t: MsgT<24>,
        /// The variables.
        pub data: UpdateAnimVariablesData,
    }
}

wire_struct! {
    /// MsgType 44, thuum's (docs/verbs/cast-action.md). Both directions,
    /// reliable: an action the player's actor performed through the engine's
    /// action entry (a hand's attack, ready, release or interrupt, the voice's
    /// too), with the idle the engine chose, the graph event it sent, the
    /// input type and the actor's behavior graph variables as they were when
    /// the graph was asked. R2: relayed as the client reports it to the
    /// actor's neighbours, never back to the sender; a figure replays it
    /// through the same entry, the variables one update before the action.
    /// Not idempotent: each is one performance. Reason codes: `E_VAL_RANGE`
    /// (no action, an empty event, a type past 255), `E_VAL_RATE`.
    pub struct UpdateAction {
        /// `"t": 44`.
        #[serde(default)]
        pub t: MsgT<44>,
        /// The actor.
        pub idx: u32,
        /// The BGSAction's form id.
        pub action: u32,
        /// The TESIdleForm the engine chose for it, 0 for none.
        pub idle: u32,
        /// The graph event the action sent.
        pub event_name: String<{ cap::ANIM_EVENT }>,
        /// The ActionInput's type (2 for everything the player does).
        #[serde(rename = "type")]
        pub kind: u32,
        /// The actor's behavior graph variables as the graph was asked.
        pub actor_animation_variables: ActorAnimationVariables,
    }
}

wire_struct! {
    /// MsgType 25. Server to client, reliable: forget a form (R0 output).
    /// Idempotent.
    pub struct DestroyActor {
        /// `"t": 25`.
        #[serde(default)]
        pub t: MsgT<25>,
        /// The form.
        pub idx: u32,
    }
}

wire_struct! {
    /// MsgType 26. Server to client, reliable: you now host this actor (R0
    /// output). Idempotent.
    pub struct HostStart {
        /// `"t": 26`.
        #[serde(default)]
        pub t: MsgT<26>,
        /// The actor's remote id.
        pub target: u64,
    }
}

wire_struct! {
    /// MsgType 27. Server to client, reliable: stop hosting this actor (R0
    /// output). Idempotent.
    pub struct HostStop {
        /// `"t": 27`.
        #[serde(default)]
        pub t: MsgT<27>,
        /// The actor's remote id.
        pub target: u64,
    }
}

wire_struct! {
    /// MsgType 28. Server to client, reliable: the player's whole inventory
    /// (R0 output). Idempotent: it replaces.
    pub struct SetInventory {
        /// `"t": 28`.
        #[serde(default)]
        pub t: MsgT<28>,
        /// The inventory.
        pub inventory: Inventory,
    }
}

wire_struct! {
    /// MsgType 29. Server to client, reliable: open the race menu (R0
    /// output; skymp5-client cannot close it on the server's word, a known
    /// gap). Idempotent.
    pub struct SetRaceMenuOpen {
        /// `"t": 29`.
        #[serde(default)]
        pub t: MsgT<29>,
        /// Open or closed.
        pub open: bool,
    }
}

wire_struct! {
    /// MsgType 30. Server to client, reliable: run a Papyrus call on the
    /// client (R2 delegation, ledgered in docs/NATIVES.md); the client
    /// answers with FinishSpSnippet unless `snippetIdx` is 0xFFFFFFFF. Not
    /// idempotent.
    pub struct SpSnippet {
        /// `"t": 30`.
        #[serde(default)]
        pub t: MsgT<30>,
        /// Papyrus script class.
        pub class: String<{ cap::PAPYRUS_NAME }>,
        /// Papyrus function.
        pub function: String<{ cap::PAPYRUS_NAME }>,
        /// Arguments; an absent one is JSON null.
        pub arguments: Vec<Option<SnippetArg>, { cap::SNIPPET_ARGS }>,
        /// The form the call is on.
        pub self_id: u64,
        /// The call's number, echoed by FinishSpSnippet.
        pub snippet_idx: i64,
    }
}

wire_struct! {
    /// MsgType 31. Server to client, reliable: move the player (R0 output;
    /// the server's correction). Idempotent.
    pub struct Teleport2 {
        /// `"t": 31`.
        #[serde(default)]
        pub t: MsgT<31>,
        /// Position.
        pub pos: Vec3,
        /// Rotation.
        pub rot: Vec3,
        /// The worldspace or interior cell form id.
        pub world_or_cell: u32,
    }
}

wire_struct! {
    /// MsgType 32. Server to client, reliable: the gamemode's client-side
    /// code (R0 output). Idempotent: it replaces.
    pub struct UpdateGamemodeData {
        /// `"t": 32`.
        #[serde(default)]
        pub t: MsgT<32>,
        /// Event sources.
        pub event_sources: Vec<GamemodeValuePair, { cap::GAMEMODE_ENTRIES }>,
        /// Update functions run for the owner.
        pub update_owner_functions: Vec<GamemodeValuePair, { cap::GAMEMODE_ENTRIES }>,
        /// Update functions run for neighbours.
        pub update_neighbor_functions: Vec<GamemodeValuePair, { cap::GAMEMODE_ENTRIES }>,
    }
}

wire_struct! {
    /// MsgType 33. Server to client, reliable: introduce a form to the
    /// client, with everything it needs to show it (R0 output). Idempotent
    /// per `idx`.
    pub struct CreateActor {
        /// `"t": 33`.
        #[serde(default)]
        pub t: MsgT<33>,
        /// The form, as the receiver will index it.
        pub idx: u32,
        /// Base record type, for references.
        pub base_record_type: Option<String<{ cap::RECORD_TYPE }>>,
        /// Where it is.
        pub transform: Transform,
        /// It is the receiving player.
        pub is_me: bool,
        /// Everything optional.
        pub props: CreateActorProps,
        /// Gamemode properties.
        pub custom_props_json_dumps: Vec<CustomPropsEntry, { cap::CUSTOM_PROPS }>,
        /// Reference id.
        pub refr_id: Option<u64>,
        /// Base form id.
        pub base_id: Option<u32>,
        /// Look.
        pub appearance: Option<Appearance>,
        /// Equipment.
        pub equipment: Option<Equipment>,
        /// Last animation.
        pub animation: Option<AnimationData>,
        /// Dead state.
        pub is_dead: Option<bool>,
    }
}

wire_struct! {
    /// MsgType 34, thuum's (docs/verbs/time.md, ADR-021). Server to client,
    /// reliable: the world's game clock (R0 output) as the engine's six time
    /// globals hold it. Sent at login, ahead of the player's own CreateActor
    /// on the same ordered channel, and every 60 s after. Idempotent: it
    /// replaces. Reason codes: `E_VAL_NONFINITE`, `E_VAL_RANGE` (month 0 to
    /// 11, day 1 to 31, hour at least 0 and below 24, daysPassed and
    /// timeScale at least 0).
    pub struct SetGameTime {
        /// `"t": 34`.
        #[serde(default)]
        pub t: MsgT<34>,
        /// GameYear.
        pub year: u32,
        /// GameMonth, from 0 (Morning Star).
        pub month: u32,
        /// GameDay, from 1.
        pub day: u32,
        /// GameHour.
        pub hour: f32,
        /// GameDaysPassed: whole days plus the hour over 24, as the engine
        /// counts them.
        pub days_passed: f32,
        /// TimeScale: game seconds per real second.
        pub time_scale: f32,
    }
}

wire_struct! {
    /// MsgType 35, thuum's (docs/verbs/rest.md, ADR-021 decision 2). Client
    /// to server, reliable: the player waited or slept, for the game hours
    /// the engine counted across the Sleep/Wait menu (R1 intent). The server
    /// checks the hours, that the player is alive and out of a fight, and
    /// computes the recovery itself (R0); the shared clock does not move. Not
    /// idempotent: each one is a rest. Reason codes: `E_VAL_NONFINITE`,
    /// `E_VAL_RANGE` (hours 1 to 24, the menu's range), `E_VAL_RATE`.
    pub struct RestIntent {
        /// `"t": 35`.
        #[serde(default)]
        pub t: MsgT<35>,
        /// Game hours rested.
        pub hours: f32,
        /// A sleep in a bed rather than a wait.
        pub sleep: bool,
    }
}

wire_struct! {
    /// MsgType 36, thuum's (docs/verbs/map-markers.md). Client to server,
    /// reliable: the player's engine discovered a location (Skyrim
    /// Platform's `locationDiscovery`), of this marker type, with or without
    /// fast travel (R1 intent). The client does not say which marker: the
    /// server finds the nearest of that type to the player in the master
    /// files, checks the range, and records it on the player (R0).
    /// Idempotent per marker. Reason codes: `E_VAL_RANGE` (a type no
    /// location uses), `E_VAL_RATE`.
    pub struct MapMarkerDiscovered {
        /// `"t": 36`.
        #[serde(default)]
        pub t: MsgT<36>,
        /// The engine's MARKER_TYPE (CommonLibSSE-NG ExtraMapMarker.h).
        pub marker_type: u16,
        /// Fast travel allowed to it (MapMarkerData kCanTravelTo).
        pub can_travel: bool,
    }
}

wire_struct! {
    /// MsgType 37, thuum's (docs/verbs/learned-effects.md). Client to server,
    /// reliable: after the player ate an ingredient, the effects its engine
    /// now knows of it (R2, bounded): bit i set when effect i is known. The
    /// server keeps the report only when it saw that player eat that
    /// ingredient just before, and only ever adds effects (R0 record).
    /// Idempotent. Reason codes: `E_VAL_RANGE` (a bit past the fourth
    /// effect), `E_VAL_RATE`.
    pub struct IngredientEffectsKnown {
        /// `"t": 37`.
        #[serde(default)]
        pub t: MsgT<37>,
        /// The INGR's form id, as the client knows it.
        pub ingredient: u32,
        /// Known effects, bits 0 to 3.
        pub mask: u8,
    }
}

wire_struct! {
    /// One favorite (docs/verbs/favorites.md): an item or a spell or shout,
    /// and the hotkey bound to it.
    pub struct FavoriteEntry {
        /// The form id, as the sender knows it.
        pub form: u32,
        /// -1 for no hotkey, 0 to 7 for the keys 1 to 8 (CommonLibSSE-NG
        /// include/RE/E/ExtraHotkey.h).
        pub hotkey: i8,
    }
}

wire_struct! {
    /// MsgType 38, thuum's (docs/verbs/favorites.md). Both directions,
    /// reliable. Client to server: the player's favorites, the whole list,
    /// after the inventory, magic or favorites menu closed and the list
    /// changed (R2: the server keeps items the player holds, records magic
    /// unvalidated, drops the rest; R0 record). Server to client: the record
    /// after a login, which the client's engine marks. Idempotent. Reason
    /// codes, client to server: `E_VAL_RANGE` (a hotkey outside -1 to 7, a
    /// key or a form twice), `E_VAL_RATE`.
    pub struct Favorites {
        /// `"t": 38`.
        #[serde(default)]
        pub t: MsgT<38>,
        /// The favorites.
        pub entries: Vec<FavoriteEntry, { cap::FAVORITES }>,
    }
}

wire_struct! {
    /// MsgType 39, thuum's (docs/verbs/racemenu-sync.md). Both directions,
    /// reliable. Client to server: the player's look as RaceMenu saves it
    /// (its preset file, JSON text), after the race menu closed and it
    /// changed; `actor` is 0 (R2: recorded, bounded, not validated against
    /// the engine). Server to client: the look of the player `actor` names
    /// (a server id), after a login to its own client and to every client
    /// that gets that player's figure, which RaceMenu then applies.
    /// Idempotent: a later preset replaces the earlier one. Reason codes,
    /// client to server: `E_VAL_RANGE` (an actor other than 0, an empty
    /// preset), `E_VAL_RATE`.
    pub struct RaceMenuPreset {
        /// `"t": 39`.
        #[serde(default)]
        pub t: MsgT<39>,
        /// The player whose look this is: 0 from a client (its own), a
        /// server id from the server.
        pub actor: u32,
        /// The preset, RaceMenu's JSON.
        pub preset: String<{ cap::RACEMENU_PRESET }>,
    }
}

wire_struct! {
    /// One actor value's base (docs/verbs/actor-values.md).
    pub struct ActorValueBase {
        /// The actor value, 0 to 163 (CommonLibSSE-NG
        /// include/RE/A/ActorValues.h).
        pub av: u8,
        /// Its base value.
        pub base: f32,
    }
}

wire_struct! {
    /// One skill's progress (CommonLibSSE-NG include/RE/P/PlayerCharacter.h,
    /// PlayerSkills::Data::SkillData).
    pub struct SkillProgress {
        /// 0 to 17 (PlayerSkills::Skills).
        pub skill: u8,
        /// The skill's level as its progress counts it.
        pub level: f32,
        /// Experience toward the next level.
        pub xp: f32,
        /// Experience the next level needs.
        pub threshold: f32,
    }
}

wire_struct! {
    /// How many times a skill was made legendary.
    pub struct LegendarySkill {
        /// 0 to 17.
        pub skill: u8,
        /// Times made legendary.
        pub count: u16,
    }
}

wire_struct! {
    /// MsgType 40, thuum's (docs/verbs/actor-values.md). Both directions,
    /// reliable. Client to server: the player's whole snapshot after a skill
    /// or level increase (R2: recorded within bounds, not validated). Server
    /// to client: the record after a login, and after a value the server set
    /// (R0), which the client's engine applies. Idempotent: a later snapshot
    /// replaces the earlier one. Reason codes: `E_VAL_RANGE` (an actor value
    /// past 163 or listed twice, a skill past 17 or listed twice),
    /// `E_VAL_NONFINITE`, `E_VAL_RATE` (client to server).
    pub struct ActorValues {
        /// `"t": 40`.
        #[serde(default)]
        pub t: MsgT<40>,
        /// Base values, each actor value once.
        pub bases: Vec<ActorValueBase, { cap::ACTOR_VALUES }>,
        /// Skills' progress, each skill once.
        pub skills: Vec<SkillProgress, { cap::SKILLS }>,
        /// The character's experience toward the next level.
        pub xp: f32,
        /// Experience the next level needs.
        pub threshold: f32,
        /// The character level.
        pub level: u16,
        /// Skills made legendary.
        pub legendary: Vec<LegendarySkill, { cap::SKILLS }>,
    }
}

wire_struct! {
    /// MsgType 41 (thuum, docs/verbs/console-commands.md). Server to client,
    /// reliable: the line a console command the player sent prints in its
    /// console, the game's own wording where it prints one, or why the
    /// server refused it. No validation beyond its capacity: the server
    /// writes it, and the transport refuses it from a client by direction.
    pub struct ConsoleOutput {
        /// `"t": 41`.
        #[serde(default)]
        pub t: MsgT<41>,
        /// The line.
        pub text: String<{ cap::CONSOLE_OUTPUT }>,
        /// The server refused the command.
        pub refused: bool,
    }
}

wire_struct! {
    /// MsgType 46, thuum's (docs/verbs/cast-teleport.md, the teleport snap).
    /// Server to client, reliable: the actor at `idx` made a jump the server
    /// vouches for (a cast teleport's permitted landing, the arrival after a
    /// move the server made, a permitted console jump), so the game places
    /// its figure at the movement report that follows instead of gliding it
    /// there (the glide covered 0.2 s, and a jump under 1024 units slid on
    /// every other screen: playtest twenty-three). Sent to every other game
    /// that sees the actor, just before that report's relay. Idempotent: a
    /// second one before the report changes nothing. Reason codes:
    /// `E_VAL_RANGE` (the invalid index, `u32::MAX`; 0 is the first actor's).
    pub struct ActorJump {
        /// `"t": 46`.
        #[serde(default)]
        pub t: MsgT<46>,
        /// The actor's index on the wire, as its movement reports carry it;
        /// the server hands indices out from 0.
        pub idx: u32,
    }
}

wire_struct! {
    /// MsgType 47, thuum's (docs/verbs/cell-host.md, ADR-035). Client to
    /// server, reliable (a load then an unload keep their order), R1: the
    /// game reports a cell fully loaded, or gone from its loaded set; the
    /// server's election reads it against its own grid, so a forged report
    /// costs a token and nothing else. Idempotent (it sets a bit). Reason
    /// codes: `E_VAL_RATE` (the burst and the rate of wire-rules
    /// cells::CELL_LOADED_BURST and CELL_LOADED_PER_SECOND), `E_VAL_RANGE`
    /// (a cell of 0).
    pub struct CellLoaded {
        /// `"t": 47`.
        #[serde(default)]
        pub t: MsgT<47>,
        /// The cell's form id in the load order both ends share.
        pub cell: u32,
        /// Loaded (true), or left the loaded set (false).
        pub loaded: bool,
    }
}

wire_struct! {
    /// MsgType 48, thuum's (docs/verbs/cell-host.md, ADR-035): server to one
    /// client, reliable, R0 output: your game hosts `cell` from now, at
    /// `epoch`, one counter per cell that goes up at every grant and is kept
    /// while the cell is unhosted, so a grant or a report from an older
    /// epoch is dropped (a fencing token). Idempotent per (cell, epoch).
    /// Reason codes: `E_VAL_RANGE` (a cell or an epoch of 0).
    pub struct HostGrant {
        /// `"t": 48`.
        #[serde(default)]
        pub t: MsgT<48>,
        /// The cell's form id.
        pub cell: u32,
        /// The grant's epoch, 1 at a cell's first grant.
        pub epoch: u32,
    }
}

wire_struct! {
    /// MsgType 49, thuum's (docs/verbs/cell-host.md, ADR-035): server to one
    /// client, reliable, R0 output: your game stops hosting `cell`; `epoch`
    /// is the grant that ends. Idempotent per (cell, epoch). Reason codes:
    /// `E_VAL_RANGE` (a cell or an epoch of 0).
    pub struct HostRelease {
        /// `"t": 49`.
        #[serde(default)]
        pub t: MsgT<49>,
        /// The cell's form id.
        pub cell: u32,
        /// The epoch of the grant that ends.
        pub epoch: u32,
    }
}

wire_struct! {
    /// One sighted actor (docs/verbs/npc-puppet.md): a game-file actor the
    /// host's engine has attached, where the engine has it.
    pub struct ActorSighting {
        /// The actor's own form id, under 0xff000000.
        pub refr_id: u32,
        /// The worldspace, or the interior cell, the engine has it in.
        pub world_or_cell: u32,
        /// Its position there.
        pub pos: Vec3,
        /// Its heading, degrees.
        pub rot: Vec3,
    }
}

wire_struct! {
    /// MsgType 50, thuum's (docs/verbs/npc-puppet.md, ADR-045 as revised
    /// 2026-10-22; TES3MP's actor list): client to server, reliable, R1. A
    /// game hosting a cell names the game-file actors its engine has that the
    /// server holds no record of (a village's residents are placed in their
    /// houses and walked outside by their schedules, so their placement is not
    /// where they stand). The server takes an entry only from the host of the
    /// cell of the sighted position, loads the record on demand and places it
    /// there; its CreateActor and hosted word follow. Every 2 s while any
    /// remain. Reason codes: `E_VAL_RANGE` (a form id of 0 or at or above
    /// 0xff000000, a world or cell of 0), `E_VAL_WORLD` (a position outside
    /// the world's bounds), `E_VAL_FINITE`.
    pub struct ActorSightings {
        /// `"t": 50`.
        #[serde(default)]
        pub t: MsgT<50>,
        /// The sightings, at most [`cap::ACTOR_SIGHTINGS`].
        pub sightings: Vec<ActorSighting, { cap::ACTOR_SIGHTINGS }>,
    }
}

wire_struct! {
    /// One placement's fate (docs/verbs/npc-fate.md, ADR-046 decision 1,
    /// ADR-049): the server's roll for a game-file actor, for every engine to
    /// take instead of its own.
    pub struct ActorFateEntry {
        /// The placement's form id: a game-file placement under 0xff000000,
        /// or a server-made NPC's id from 0xff000000 up (ADR-050 decision 3,
        /// fated through the same roll before its create word).
        pub refr_id: u32,
        /// The NPC_ the roll chose: the template chain's leaf for a leveled
        /// template, the placed base when nothing is leveled, 0 when the roll
        /// chose none (the placement stays empty).
        pub base_id: u32,
        /// The level the roll used (0: rolled before ADR-049's rule).
        pub level: u16,
        /// The worn items the server's outfit roll chose, by form id.
        pub worn: Vec<u32, { cap::FATE_WORN }>,
    }
}

wire_struct! {
    /// MsgType 51, thuum's (docs/verbs/npc-fate.md, ADR-049): server to
    /// client, reliable, R0. The fates of the game-file actors in the cells a
    /// player's engine is about to attach (the ring around its square, two
    /// squares out), sent once per square per player ahead of the engine's
    /// own roll at the reference's load, and again at a respawn. The client
    /// pins the engine's roll from it; a fate arriving after the engine rolled
    /// goes through the engine's own re-roll. Reason codes: `E_VAL_RANGE` (a
    /// placement id of 0 or at or above 0xff000000, a worn id of 0).
    pub struct ActorFate {
        /// `"t": 51`.
        #[serde(default)]
        pub t: MsgT<51>,
        /// The fates, at most [`cap::ACTOR_FATES`].
        pub fates: Vec<ActorFateEntry, { cap::ACTOR_FATES }>,
    }
}

wire_struct! {
    /// MsgType 52, thuum's (docs/verbs/npc-fate.md, ADR-048's B): client to
    /// server, reliable, once per death, R1. The host of a game-file actor,
    /// or of a server-made one (0xff000000 up; playtest 37's finding 47),
    /// reports a death its engine played that the server has no number for:
    /// the actor, the host's reading of the cause, the killer when an actor,
    /// the point. The server accepts it only when it can check the cause by
    /// its own evidence (a fall by the record's height history, a hazard by
    /// an activation it saw near the point, an NPC's hit by a recent hit it
    /// numbered), refuses and logs the rest, and caps a host's reports. Reason
    /// codes: `E_VAL_RANGE` (an actor id of 0, a cause above 4),
    /// `E_VAL_OUT_OF_WORLD`, `E_VAL_NONFINITE`, `E_VAL_RATE`.
    pub struct ActorDeath {
        /// `"t": 52`.
        #[serde(default)]
        pub t: MsgT<52>,
        /// The dead actor's own form id, under 0xff000000.
        pub refr_id: u32,
        /// The host's reading of the cause: 0 unknown, 1 fall, 2 hazard,
        /// 3 script, 4 an actor's hit (the killer named).
        pub cause: u8,
        /// The killer's form id when the cause is an actor's hit, else 0.
        pub killer: u32,
        /// Where the body fell.
        pub pos: Vec3,
    }
}

wire_struct! {
    /// MsgType 53, thuum's (docs/verbs/npc-stream.md, ADR-050 decision 1):
    /// client to server, unreliable (the first for an actor after a grant
    /// marked reliable, the keyframe), at most once per 500 ms per hosted
    /// actor and only on a change, R2 bounded by the server's numbers. The
    /// state of an actor the sender's game simulates (cell-host's host of the
    /// actor's cell) as its engine has it: the three percentages, its combat
    /// target as the server knows it and the package its last package event
    /// named. Absolute: a lost one is replaced by the next, the newest wins.
    /// No epoch: the server's host table already refuses a sender that is
    /// not the actor's host, and a client cannot name a persistent
    /// reference's cell (the verb doc's Q1, revised at the build). Reason
    /// codes: `E_VAL_NONFINITE` (a percentage that is not a number),
    /// `E_VAL_RANGE` (a percentage outside 0 to 1, an idx of u32::MAX),
    /// `E_VAL_RATE` (the budget).
    pub struct HostedActorState {
        /// `"t": 53`.
        #[serde(default)]
        pub t: MsgT<53>,
        /// The actor as the server indexes it; the sender's own is refused
        /// there (a player's values go as ChangeValues).
        pub idx: u32,
        /// Health, 0 to 1, as the host's engine has it (0 for a dead actor).
        pub health: f32,
        /// Magicka, 0 to 1.
        pub magicka: f32,
        /// Stamina, 0 to 1.
        pub stamina: f32,
        /// The combat target's form id as the server knows it, 0 for none.
        pub combat_target: u32,
        /// The running package's form id, the last package event's, 0 for
        /// none.
        pub package: u32,
        /// Slice 5 (the gaze): the head-track target's form id as the server
        /// knows it, the engine's highest set slot; 0 for none.
        pub gaze_target: u32,
        /// The engine's HEAD_TRACK_TYPE of that slot (0 default, 1 action, 2
        /// script, 3 combat, 4 dialogue, 5 procedure; CommonLibSSE-NG
        /// HighProcessData.h:80-94), 0 when no slot is set; below 6.
        pub gaze_slot: u8,
        /// The engine's EmotionType (0 neutral, 1 anger, 2 disgust, 3 fear, 4
        /// sad, 5 happy, 6 surprise, 7 puzzled; TESTopicInfo.h:75-85); at
        /// most 7.
        pub emotion_type: u8,
        /// The emotion's strength, 0 to 100.
        pub emotion_value: u8,
    }
}

wire_struct! {
    /// MsgType 54, thuum's (docs/verbs/npc-stream.md, the voice slice, ADR-050):
    /// a spoken line begins on an actor the sender's game simulates. Client
    /// to server, unreliable (a lost line is a lost line, TES3MP's shape),
    /// R2: the server gates the host and relays the bytes to the actor's
    /// other listeners, which make their puppet say the same info through
    /// the engine's own say. Reason codes: `E_VAL_RANGE` (an idx of u32::MAX,
    /// a topic or info of 0), `E_VAL_RATE` (the budget).
    pub struct ActorSpeech {
        /// `"t": 54`.
        #[serde(default)]
        pub t: MsgT<54>,
        /// The speaking actor as the server indexes it; the sender's own is
        /// refused there.
        pub idx: u32,
        /// The topic's form id (the info's parent).
        pub topic: u32,
        /// The topic info's form id: the line itself.
        pub info: u32,
    }
}

wire_struct! {
    /// MsgType 55, thuum's (docs/verbs/npc-fate.md, ADR-050 decision 2, deaths
    /// by the engine): the server's number for a hit on an NPC some game
    /// hosts, sent to that host as a CAUSE instead of absolute values, so the
    /// host's engine applies it through its own damage entry (a reaction, a
    /// killer, its own death at zero). Server to client, reliable, R0 output:
    /// `health` is the resulting percentage (absolute, never a delta: the
    /// hold rule keeps the record below the engine for 10 s after a hit, and
    /// a delta would double on a hit the host also reports), `attacker` the
    /// aggressor's form id (0 for none), `kill_move` whether the server's
    /// number killed through a headshot or a scripted kill. Reason codes:
    /// `E_VAL_NONFINITE` (a health that is not a number), `E_VAL_RANGE` (a
    /// health outside 0 to 1, an idx of u32::MAX).
    pub struct DamageCause {
        /// `"t": 55`.
        #[serde(default)]
        pub t: MsgT<55>,
        /// The victim as the server indexes it.
        pub idx: u32,
        /// The aggressor's form id, 0 for none.
        pub attacker: u32,
        /// The resulting health, 0 to 1.
        pub health: f32,
        /// Whether the number killed through a headshot or a scripted kill.
        pub kill_move: bool,
    }
}

wire_struct! {
    /// MsgType 56, thuum's (docs/verbs/npc-talk.md, ADR-052): a player asks
    /// for an NPC some other player's game simulates, for a dialogue (`claim`
    /// true), or gives it back (`claim` false). Client to server, reliable,
    /// R0: the server grants when the claimant stands in the actor's cell
    /// within activation reach and the actor is an NPC, alive, not in combat
    /// and unclaimed; the word (isHostedByOther) then names the claimant as
    /// the simulator for the claim's lease. Reason codes: `E_VAL_RANGE` (an
    /// idx of u32::MAX), `E_VAL_RATE` (the budget).
    pub struct ActorClaim {
        /// `"t": 56`.
        #[serde(default)]
        pub t: MsgT<56>,
        /// The actor as the server indexes it.
        pub idx: u32,
        /// True asks for the actor, false releases it.
        pub claim: bool,
    }
}

wire_struct! {
    /// MsgType 58, thuum's (docs/verbs/prop-physics.md, ADR-058 decision 2):
    /// where a physics prop (a kicked basket, a dropped sword) is, from the
    /// one seat that simulates it. Client to server, unreliable, at most once
    /// per prop per 100 ms while the body is awake and once more when it
    /// sleeps (`asleep` true), R2: the simulator's Havok computes, the server
    /// records the last transform and the flag on the reference's record.
    /// Server to client, the same bytes relayed to the cell's other listeners
    /// (unreliable while awake, reliable at sleep), which impose it on a
    /// Keyframed body. Whose transforms are taken is the server's rule
    /// (wire-rules `props`: the prop's owner, the cell's host by default).
    /// Reason codes: `E_VAL_RANGE` (a refr of 0), `E_VAL_NONFINITE`,
    /// `E_VAL_OUT_OF_WORLD`, `E_VAL_RATE` (the budget, client to server).
    pub struct PropTransform {
        /// `"t": 58`.
        #[serde(default)]
        pub t: MsgT<58>,
        /// The prop's reference form id (a game-file placement or a server-made
        /// drop above 0xff000000); never 0.
        pub refr: u32,
        /// Position, engine units.
        pub pos: Vec3,
        /// Rotation, Euler degrees as SkyMP sends them.
        pub rot: Vec3,
        /// The body came to rest at this transform (the simulation island's
        /// deactivation): the record's final sample, relayed reliable.
        pub asleep: bool,
    }
}

wire_struct! {
    /// MsgType 59, thuum's (docs/verbs/prop-physics.md, ADR-058 decision 1):
    /// a player's game touched a prop it does not simulate. Client to server,
    /// reliable, R0: the server names the newest toucher the simulator (one
    /// epoch per prop, newest touch wins) and tells every listener of the prop
    /// (`PropOwner`); refused when the claimant is not in the prop's cell or
    /// another player holds the prop grabbed. Reason codes: `E_VAL_RANGE` (a
    /// refr of 0, a touch above 3), `E_VAL_RATE` (the budget).
    pub struct PropClaim {
        /// `"t": 59`.
        #[serde(default)]
        pub t: MsgT<59>,
        /// The prop's reference form id; never 0.
        pub refr: u32,
        /// How it was touched: 0 a contact of the character controller, 1 a
        /// grab, 2 a drop, 3 a shout's or spell's impulse; at most 3.
        pub touch: u8,
    }
}

wire_struct! {
    /// MsgType 60, thuum's (docs/verbs/prop-physics.md, ADR-058 decision 1):
    /// who simulates a prop from now, to every listener of the prop at each
    /// owner change. Server to client, reliable, R0 output: the named seat's
    /// body goes Dynamic with the last streamed velocity, every other seat's
    /// goes Keyframed; a word with an older epoch than one already applied is
    /// dropped (a fencing token, one counter per prop). Reason codes:
    /// `E_VAL_RANGE` (a refr or a player of 0).
    pub struct PropOwner {
        /// `"t": 60`.
        #[serde(default)]
        pub t: MsgT<60>,
        /// The prop's reference form id; never 0.
        pub refr: u32,
        /// The simulating player's actor form id; never 0.
        pub player: u32,
        /// The prop's owner epoch, up at every claim granted.
        pub epoch: u32,
    }
}

wire_struct! {
    /// MsgType 61, thuum's (docs/verbs/spell-placed-object.md, ADR-057 shape
    /// A, D70): the server asks the caster's game where its engine stands the
    /// objects a listed spell's script placed. Server to client, reliable, to
    /// the caster alone at its accepted cast (wire-rules placements): the
    /// set's id, the bases to find (the server's form ids, the load order
    /// both ends share) and the window in milliseconds within which the
    /// reports are taken. The caster's game answers one `PlacedReport` per
    /// base once the reference has settled. Reason codes: `E_VAL_RANGE` (no
    /// base, a base of 0, a window of 0 or over ten seconds).
    pub struct PlacedRequest {
        /// `"t": 61`.
        #[serde(default)]
        pub t: MsgT<61>,
        /// The set's id, which the reports name.
        pub set: u64,
        /// The bases to find, each a form id; one to eight, none 0.
        pub bases: Vec<u32, { cap::PLACED_BASES }>,
        /// How long from the cast the reports are taken, milliseconds; 1 to
        /// 10,000.
        pub window_ms: u32,
    }
}

wire_struct! {
    /// MsgType 62, thuum's (docs/verbs/spell-placed-object.md, ADR-057 shape
    /// A, D70): where the caster's engine stands one base of a requested
    /// set. Client to server, reliable, once per base per set, R1: the
    /// caster's game computes (the script's placement), the server validates
    /// (the set its own request to this caster and still open, the base one
    /// the set asked for and not yet reported, the pose a number 150 to 260
    /// units from where the caster stood at the cast on the ground plane and
    /// within 256 of its height; wire-rules placements) and places its copy
    /// there for every other seat. Reason codes: `E_VAL_RANGE` (a base of
    /// 0), `E_VAL_NONFINITE`, `E_VAL_OUT_OF_WORLD`, `E_VAL_RATE` (the budget).
    pub struct PlacedReport {
        /// `"t": 62`.
        #[serde(default)]
        pub t: MsgT<62>,
        /// The set named by the request.
        pub set: u64,
        /// The base found, a form id; never 0.
        pub base: u32,
        /// Where it stands, engine units.
        pub pos: Vec3,
        /// Its Z angle, degrees.
        pub yaw: f32,
    }
}

wire_struct! {
    /// A reference's lock (docs/verbs/locks.md), on CreateActor's props and
    /// as the `lock` property's value.
    pub struct LockProp {
        /// The XLOC level byte: 0 or 1 Novice, 25 Apprentice, 50 Adept, 75
        /// Expert, 100 Master, 255 requires its key.
        pub level: u8,
        /// The key's base form id, 0 for none.
        pub key: u32,
        /// Locked now.
        pub locked: bool,
    }
}

wire_struct! {
    /// MsgType 64, thuum's (docs/verbs/locks.md, M4): a player's game reports
    /// how its lockpicking attempt on a locked reference ended. Client to
    /// server, reliable, R1: the engine ran the minigame, the server judges
    /// the report (the reference locked and within the activation reach, the
    /// lock's tier open to picks, a Lockpick held, a gap between reports;
    /// wire-rules locks) and writes the unlock or takes the broken picks from
    /// the record. Reason codes: `E_VAL_RANGE` (a refr of 0, an outcome past
    /// 2, more than 8 picks broken), `E_VAL_RATE`.
    pub struct LockReport {
        /// `"t": 64`.
        #[serde(default)]
        pub t: MsgT<64>,
        /// The reference's form id; never 0.
        pub refr: u32,
        /// 0 the lock turned, 1 picks broke and the lock stands, 2 the menu
        /// was left with the lock standing.
        pub outcome: u8,
        /// Picks broken in this attempt, 0 to 8.
        pub broken: u8,
    }
}

wire_struct! {
    /// MsgType 65, thuum's (docs/verbs/locks.md, M4): the server tells the
    /// player's game that its activation met a lock without the key, so the
    /// game runs the engine's own activation on its locked reference (the
    /// lockpicking menu, or the "requires key" box). Server to client,
    /// reliable, to the activating player alone. Reason codes: `E_VAL_RANGE`
    /// (a refr of 0).
    pub struct LockPrompt {
        /// `"t": 65`.
        #[serde(default)]
        pub t: MsgT<65>,
        /// The reference's form id; never 0.
        pub refr: u32,
    }
}

wire_struct! {
    /// MsgType 66, thuum's (docs/verbs/weather.md, M4): the server's weather
    /// for the place the player is in. Server to client, reliable, to one
    /// player: at login, when it enters a place and at each change of the
    /// server's pick for its worldspace (wire-rules weather). The game forces
    /// the weather (`transition` false: a cut, as at login) or lets its engine
    /// blend into it (`transition` true, a change the player stood through);
    /// `weather` 0 releases the game's override (an interior, a worldspace
    /// without a pool), so the engine's own sky stands. A game in another
    /// place keeps the word until it gets there. Reason codes: `E_VAL_RANGE`
    /// (a place of 0).
    pub struct SetWeather {
        /// `"t": 66`.
        #[serde(default)]
        pub t: MsgT<66>,
        /// The worldspace, or the interior cell a release is for; never 0.
        pub place: u32,
        /// The WTHR form id; 0 releases the override.
        pub weather: u32,
        /// Blend from the current weather (true) or cut to it (false).
        pub transition: bool,
    }
}

wire_struct! {
    /// MsgType 67, thuum's (docs/verbs/perks.md, M5): a player's game reports
    /// the perk its Skills menu took. Client to server, reliable, R1: the
    /// server judges the pick by the perk record's own conditions and the
    /// points left (wire-rules perks), writes it to the record and answers
    /// with `PerkState`; a refused pick is undone by that answer. Reason
    /// codes: `E_VAL_RANGE` (a perk of 0), `E_VAL_RATE`.
    pub struct PerkPick {
        /// `"t": 67`.
        #[serde(default)]
        pub t: MsgT<67>,
        /// The perk's form id; never 0.
        pub perk: u32,
    }
}

wire_struct! {
    /// MsgType 68, thuum's (docs/verbs/perks.md, M5): the server's whole perk
    /// set and the points left for one player: at login, after each pick
    /// (accepted or refused), after a level change and after the console's
    /// AddPerk, RemovePerk or SetLevel. Server to client, reliable. The game
    /// sets its perk points, adds what it lacks and drops a tree perk the set
    /// lacks. Reason codes: `E_VAL_RANGE` (a perk of 0 in the set).
    pub struct PerkState {
        /// `"t": 68`.
        #[serde(default)]
        pub t: MsgT<68>,
        /// The perks held, none 0; at most 512.
        pub perks: Vec<u32, { cap::PERKS }>,
        /// The points left to spend.
        pub points: u16,
    }
}

wire_struct! {
    /// One skill use (docs/verbs/leveling.md): the skill by its ActorValue and
    /// the action's base amount as the engine computed it.
    pub struct SkillUseEntry {
        /// The skill's ActorValue, 6 (One-handed) to 23 (Enchanting).
        pub skill: u8,
        /// The action's base amount (damage dealt, magicka spent, gold traded
        /// and so on), summed over the second; finite and above 0.
        pub amount: f32,
    }
}

wire_struct! {
    /// MsgType 63, thuum's (docs/verbs/leveling.md, M5 Progression): the skill
    /// uses a player's engine made in the last second, batched. Client to
    /// server, reliable, R1: the engine computes each action's base amount
    /// (its own skill experience is off), the server bounds it and runs the
    /// game's formulas (wire-rules leveling) into the record, answering with
    /// ActorValues (MsgType 40) when a level moves. Reason codes:
    /// `E_VAL_RANGE` (a skill outside 6 to 23 or listed twice, an amount of 0
    /// or below or over 1e6, no use at all), `E_VAL_NONFINITE`, `E_VAL_RATE`.
    pub struct SkillUse {
        /// `"t": 63`.
        #[serde(default)]
        pub t: MsgT<63>,
        /// The uses, each skill once; 1 to 18.
        pub uses: Vec<SkillUseEntry, { cap::SKILL_USES }>,
    }
}
