//! SkyMP's own messages (ADR-019): one struct per MsgType, fields in the
//! order and under the JSON keys of the C++ `Serialize(Archive&)` lists in
//! skymp5-server/cpp/messages and the payload types in server_guest_lib.
//! This file is the contract the C++ core and skymp5-client keep speaking
//! in-process; on the wire the same structs travel as postcard. MsgTypes 1
//! to 33 are SkyMP's; thuum appends its own after them (34, SetGameTime; 35,
//! RestIntent; 36, MapMarkerDiscovered; 37, IngredientEffectsKnown; 38,
//! Favorites; 39, RaceMenuPreset).
//!
//! Directions are SkyMP's: twelve types (fifteen with RestIntent,
//! MapMarkerDiscovered and IngredientEffectsKnown) only travel
//! client to server, thirteen (fourteen with SetGameTime) only server to
//! client, eight (ten with Favorites and RaceMenuPreset) both ways (the server relays a client's
//! UpdateMovement, UpdateAnimation, UpdateAppearance, UpdateEquipment,
//! SpellCast and UpdateAnimVariables to its neighbours, and sends its own
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
    /// (R1 intent). Not idempotent.
    pub struct PlayerBowShot {
        /// `"t": 22`.
        #[serde(default)]
        pub t: MsgT<22>,
        /// Bow form id.
        pub weapon_id: u32,
        /// Arrow form id.
        pub ammo_id: u32,
        /// Draw power, 0 to 1.
        pub power: f32,
        /// Aimed at the sun (the engine's quirk).
        pub is_sun_gazing: bool,
    }
}

wire_struct! {
    /// SpellCastMessage's `data`.
    pub struct SpellCastData {
        /// Who casts.
        pub caster: u32,
        /// At whom.
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
        /// The caster's behavior graph variables.
        pub actor_animation_variables: ActorAnimationVariables,
    }
}

wire_struct! {
    /// MsgType 23. Both directions, reliable: a spell cast (R1 intent) and
    /// its relay. Not idempotent.
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
