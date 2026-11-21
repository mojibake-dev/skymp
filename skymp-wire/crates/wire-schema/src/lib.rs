//! The wire contract. Every message that crosses the network is a variant of
//! [`Message`]. Ids are append-only: the variant's position is its id on the
//! wire. Collections are [`bounded`] so an over-long input fails at decode,
//! at the field that overflowed, never later (ADR-012, ADR-019).
//!
//! Two families share the enum. The M0 variants (ids 0 to 8) are the
//! authority model's messages, reserved until its verbs land. The SkyMP
//! variants (ids 9 to 57, id = MsgType + 8) are SkyMP's own protocol, ported
//! field for field in [`skymp`], and the MsgTypes thuum appends after
//! SkyMP's 33; their JSON form is what the C++ core and skymp5-client
//! exchange in-process (`wire-json`).
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
/// 21: QuestStage's mode byte (thuum D58 B, m3-quest-mode) and PropTransform,
/// PropClaim, PropOwner (thuum ADR-058, m3-prop-physics): one bump for the
/// train that carries both.
/// 22: PlacedRequest and PlacedReport (thuum ADR-057 shape A, D70,
/// m3-wall-pose): the server asks the caster where its engine stands a
/// listed spell's objects and places its copies as reported.
/// 23: SkillUse (thuum docs/verbs/leveling.md, m5-leveling): a player's
/// skill uses batched for the server's formulas.
/// 24: LockReport, LockPrompt and CreateActorProps.lock (thuum
/// docs/verbs/locks.md, m4-locks): the server's lock state on every
/// reference, the prompt and the report around the engine's minigame.
/// 25: SetWeather (thuum docs/verbs/weather.md, m4-weather): the server's
/// weather for the place a player is in.
/// 26: PerkPick and PerkState (thuum docs/verbs/perks.md, m5-perks): a perk
/// pick judged by the server, the perk set and points it holds.
/// 27: WordWall (thuum docs/verbs/word-wall.md, m4-word-wall): a word wall
/// trigger reported by the player's game, the word the server's.
/// 28: RelayedActivation (thuum docs/verbs/traps.md, m4-traps): a linked
/// activation the cell's host reported, told to the cell's other games.
/// 29: CreateActorProps.restricted_races and .permitted_races (thuum
/// docs/verbs/character-creation.md, the permit, m5-race-permit): the races
/// the server restricts to listed profiles and the ones the player's own
/// profile may wear, on its own CreateActor.
/// 30: ObjectAnimation (thuum docs/verbs/traps.md, ADR-059 decision 3,
/// m4-object-animation): a world object's animation event the cell's host
/// reported, recorded as its lastAnimation and told to the cell's other games.
/// 31: TrapHit (m4-trap-hit, thuum docs/WIRE.md). 32: m2-rune-glyph.
/// 33: ChangeForm, ChangeFormAck and ChangeFormApply (thuum
/// docs/verbs/change-form.md, ADR-065 system 1, adr065-sys1-wire): an
/// actor's change form as the owner's engine serialized it, recorded by the
/// server, acked by its hash and served to every seat that sees the actor.
/// Their `t` numbers 72 to 74 and wire ids 80 to 82 are positional (the wire
/// id is `t` plus 8, the variant index; the tests below pin both) and collide
/// with TrapHit's 72 in the doc of record (thuum docs/WIRE.md, m4-trap-hit):
/// the branch that lands on parity second renumbers, the ids shifting with
/// the variant order either way. 33 so that no two builds at 31 disagree on
/// what a 72 means.
/// 34: 33's three messages with 32's `decal` on SpellCast (the rune's glyph
/// on the observer, m2-rune-glyph) merged on m3-canon-1, Eli's baseline of
/// everything known to work; a 33 image without the field is on the lab, so
/// the union is a new number.
/// 35: ActorFateEntry.inv (thuum docs/verbs/npc-fate.md, playtest 44's 1c):
/// the record's inventory rows on the fate.
/// 36: ScriptCall and SpSnippet's `seq` (thuum ADR-068, the one seam,
/// m4-scripts-2): a tabled native the subject's owner ran, reported at the
/// native boundary, and its relay with the subject's sequence.
/// 37: DestructionStage (thuum D-S3, traps follow the interaction; ADR-068's
/// DestructionStage 78, pulled ahead of rung 4, m4-scripts-2b-traps): a
/// reference's destruction stage and destroyed flag, reported by the seat
/// whose engine reached the step first, recorded once and served to a late
/// seat; and the two reserved rows at ids 84 and 85 (ADR-068's ScriptState
/// 76 and ScriptStateApply 77, rung 3's, numbered ahead of their shapes), so
/// MsgType 78 keeps `wire id = MsgType + 8` (ADR-019 decision 1).
/// 38: ScriptState and ScriptStateApply (thuum ADR-068 rung 3, the script
/// record, m4-scripts-3-record) in place of the two reserved rows, at the
/// same ids 84 and 85: the scripts attached to a reference as its owner's
/// engine read them at a quiescent point, recorded by the server with the
/// object's sequence and its view, and served ahead of a grant and behind a
/// create word.
/// 39: the state rows (thuum ADR-068 rung 4 and its amendment B,
/// m4-scripts-4-rows): DestructionStage widened to a reference's engine
/// state (`disabled`, `deleted` and the object's `seq` beside the stage and
/// destroyed, the server's word alone), AliasFill (MsgType 79, wire id 87: a
/// quest's reference alias filled or cleared, nobody's, the latest report
/// recorded and relayed) and WorldGlobal (MsgType 80, wire id 88: the GLOBs
/// every seat's scripts write, streamed by one seat at an epoch), and
/// CreateActor's `placedBy` on a script's placement (STAT, NPC_ and items
/// beside MSTT and ACTI).
/// 40: the trigger entry channel (thuum D-W1 and D-C2, playtest 50,
/// docs/research/fifty-fixes-2026-11-07.md 1(a), m4-fifty-wire): Activate's
/// and RelayedActivation's optional `entry` (1 OnTriggerEnter, 2
/// OnTriggerLeave), a guest's entry into a trigger whose world script runs
/// on the cell's host, relayed to the host alone; no new message, so the
/// next free MsgType stays 81. And ScriptStateApply's `graph_vars` (D-C3 A,
/// PENDING Eli): a world object's behavior graph variables served beside
/// the record's view, each with its own sequence.
/// 41: rung 5's quests (thuum ADR-068 rung 5, docs/research/
/// rung5-quests-2026-11-09.md sections 1(a) and 8, m4-scripts-5-wire):
/// QuestStage's optional `word` (1 a start, 2 a stop), `acting` (the acting
/// player's actor) and `served` (the login replay), and ScriptVariables
/// (MsgType 81, wire id 89: the condition-read script variables any seat
/// reports, the latest write recorded per key with the server's sequence,
/// a player-class quest's keys its player's own). The rung's server half
/// (m4-scripts-5-server) adds, inside 41 before any build of it shipped:
/// CreateActorProps' and UpdateAppearance's optional `overlay` (D-T1 A, a
/// player's race overlay on its figure), and a ScriptCall object argument at
/// 0xff000000 and up (a scoped call's figure, its player's actor by its
/// server id), which the core takes only when it names a connected player;
/// and QuestStage's optional `player` on the login's list (the player-class
/// quests, whose scripts each seat runs for its own player).
/// 42: rung 5's fresh review (thuum ADR-068 rung 5, m4-scripts-5-server;
/// ADR-064 amendment A): QuestStage's optional `wordStages` on a start or a
/// stop word, the stages whose fragments the engine runs with it (stage 0
/// and the quest's start-up stages for a start, its shut-down stages for a
/// stop; D-Q3 A, so a served word's receiver suppresses them), and
/// QuestStage's byte cap at 4 KiB (the login's list with its `player`).
/// The number moves for 41's server half too: CreateActorProps' and
/// UpdateAppearance's `overlay`, ScriptCall's figure argument and
/// QuestStage's `player` changed layouts inside 41 after pipeline 5676's
/// image carried an earlier 41.
/// 43: D-Q4 C (thuum ADR-068 rung 5, m4-scripts-5-cause): QuestStage's
/// optional `cause` (1 a world cause, 2 the seat's own) and `root` (the
/// root frame of the script stack that made the report, "<Script>.
/// <Function>") on a seat's report, so the server refuses a run word, or a
/// report it judges a start, whose cause is the seat's own; R2, the seat's
/// own word.
/// 44: ScriptCall's optional `landed` (thuum D-R1 C's leveled gap,
/// m4-train-52): on a leveled AddItem on the sender's own player, the bases
/// and counts the engine's roll put in its inventory, read inside the call's
/// AddItemFunctor, so the server records the roll where it refused the list
/// by name (E_SCRIPTCALL_LEVELED).
/// 45: PropOwner's player 0 (thuum ADR-071's physics rung 1, the props rule
/// A1 to A7, m4-physics-1-arbitration): the host of the cell the prop's
/// record stands in, the word of every return at rest, departure and unload.
/// No shape moved; the number moves by Eli's D-PH1 A (ADR-071's amendment of
/// 2026-11-11, a bump at each rung that changes what an older peer accepts):
/// a client built before it validates every server message and counts a
/// refusal without a word (wire-transport's receive, `validate_server`), so
/// it would drop each player-0 word and keep a stale owner with no sign,
/// and the handshake's protocol id (`protocol_id`) is the one guard that
/// fails loudly. Rung 2's sample and claim shapes take 46.
/// 46: PropTransform v2 and PropClaim v2 (thuum ADR-071 decision 6, physics
/// rung 2, m4-physics-2-sample): a sample carries the owner epoch its sender
/// holds, a sequence within it, the owner's clock, both velocities, the
/// engine's own grab and up to 64 body poses (empty until a world object's
/// parts, rung 4, or a dead actor's bones, rung 5), and the server's record
/// answer to a refused sample is the same message at sequence 0; a claim
/// names one to sixteen references, each judged alone. The bump is Eli's
/// D-PH1 A guard as at 45: the handshake's protocol id carries the number,
/// where a refused server message would otherwise be dropped into a counter
/// without a word.
/// 47: ActorClaim's optional `purpose`, `quest`, `package` and `scene` (thuum
/// ADR-073 step 5a, decision 6 with D-L2 B and D-53-4, m4-owners-5a): a claim
/// names a talk, a quest's lead (its own package on its alias actor) or a
/// scene its seat started, and outlives a grant into its cell. Taken as the
/// number D-53-3's merge order gives ADR-073's first step after canon's 44,
/// C4's 45 and the physics heads' 46; it shipped alone in the fork's
/// pipelines 6031 (m4-owners-5a 938acfeb) and 6032 (m4-owners-5a-client
/// 604071a5).
/// 48: no shape of its own: the physics heads' 45 and 46 above (PropOwner's
/// player 0, PropTransform v2 and PropClaim v2; thuum ADR-071 rungs 1 to 5a,
/// m4-physics-4-statics e7e4d413), merged into m4-sitting-53 after 47's
/// images carried step 5a alone, so a 47 peer would pass the handshake and
/// drop each v2 sample, claim and player-0 word with no sign (D-PH1 A's
/// reason, 42's precedent). In D-53-3's merge order C4 (m4-reanimate-return,
/// 45 on its branch, a number physics rung 1 also shipped) takes the number
/// its merge comes to.
/// 49: ActorClaim's optional `why` (thuum D-53-5 B, ADR-073 step 5a,
/// m4-owners-5a 763bd460): a lead's release says its game stopped running
/// the actor (another quest's package or none, her unload there). It was
/// written under 47 on its branch, but m4-sitting-53's 48 images (5c772d9d's
/// pipeline, its server image and its Windows dist with MpClientPlugin.dll)
/// carried ActorClaim without it. postcard writes a struct's fields in order
/// with one presence byte per Option, so a 48 client's claim arrives one byte
/// short and wire-codec's decode refuses it as Malformed: every claim, a talk
/// as canon sends it included, would fail while the handshake passed (D-PH1
/// A's reason, 42's and 48's precedent).
/// 50: OwnHeadParts (MsgType 82, wire id 90; thuum D-53-8 B, train 53's
/// V-live, m4-fix-vlive-look): the head parts a player's own game draws on its
/// own player after its race changed (its base's overlay list where the
/// engine answers HasOverlays, else its base list), recorded as the player's
/// look and sent to every other game that sees the player, R2. Written as 49
/// on its branch while m4-sitting-53 took 49 for ActorClaim's `why`
/// (c4621a8b), so the merge moves it here: one number, one shape (D-PH1 A's
/// reason), and a 49 peer, which knows no wire id 90, fails the handshake.
/// 51: LockReport's optional `locked` and `level` and its outcome 3, the
/// owner's engine write (thuum ADR-073 step 3, decision 12, m4-step3-locks
/// 524ac581): a lock an engine wrote by itself, recorded from the seat that
/// hosts the reference or its load door's other side. Merged after 50's images
/// (m4-sitting-53 f0c378ec) carried LockReport without them: postcard is
/// positional, so a 50 client's report arrives two presence bytes short and is
/// refused as Malformed while the handshake passed (D-PH1 A's reason, 42's and
/// 48's precedent).
/// 52: MagicEffects' per-effect `caster` (thuum docs/verbs/npc-fate.md C4,
/// ADR-060's return, m4-reanimate-return 7ae7d7c2, 45 on its branch): the
/// server's form id of who applied each effect, 0 for none, so a fresh game
/// that simulates a full thrall knows whether its own player is the
/// thrall's caster and commands it. Merged into m4-sitting-53 after 51's
/// tree, so it takes the next number (D-53-3's merge order), one number,
/// one shape (D-PH1 A's reason).
/// 53: CreateActorProps' optional `looks` and SpSnippet's optional `keep`
/// (thuum #52, the Undead FX build, the reanimate-fx memo's 6.4 D6 B; the
/// review of the client's 3D keep, eaa6ba47, findings 1 and 2): the looks an
/// actor's simulator's script drew on it and has not stopped, kept by the
/// server (wire-rules actor_looks) and served with the actor's create word,
/// and the keep of a relayed look row named on its relay, so the client's 3D
/// keep has no row table of its own. Written as 51 on m4-reanimate-look-3d
/// over canon's 44 and merged after 52's tree, so it takes the next number
/// (D-53-3's merge order), one number per shape (D-PH1 A's reason: postcard
/// writes one presence byte per Option, so a 52 peer would misread every
/// CreateActor and SpSnippet of a 53 server while the handshake passed).
/// 54: HostedActorState's optional `effects` (thuum #56, docs/verbs/
/// npc-puppet.md, Ownership; ADR-073 decision 1, ADR-063): the running
/// effects whose look the host's engine draws on an actor it simulates,
/// recorded by the server and relayed, so a watcher plays exactly those on
/// its puppet in place of the ability scan and its conditioned skip (the
/// flame atronach's art, x-reanimate-look F6; the vampires' sunlight fire,
/// R-V4). Built on m4-host-running-effects and merged after 53's tree, so it
/// takes the next number, one number per shape (D-PH1 A's reason: postcard
/// writes one presence byte per Option, so a 53 peer would misread every
/// HostedActorState of a 54 server while the handshake passed).
/// 55: ScriptCall's optional `placed` and a subject or object argument of
/// 0xff000000 and up naming the sender's own created reference (thuum ADR-073
/// decision 13, the spell channel's first slice; #54, Raise Wall): a
/// script's placement and its calls on what it placed reach every screen as
/// the creator's calls, one shape for every spell, one number per shape
/// (D-PH1 A's reason: postcard writes one presence byte per Option, so a 54
/// peer would misread every ScriptCall of a 55 client while the handshake
/// passed).
/// 56: ScriptState's and ScriptStateApply's optional `graph` (thuum
/// docs/verbs/traps.md "The graph state", m4-graph-state c55f878e, its
/// platform half on the playtest 53 drop): a world object's animation graph
/// state as its owner's engine saves it for a 3D unload and a save, the
/// engine's frame as lowercase hex; the server keeps the last one given and
/// serves a record whose graph state moved at once to the reference's other
/// listeners. Written for 50 on canon and merged after 55's tree, so it takes
/// the next number, one number per shape (D-PH1 A's reason: postcard writes
/// one presence byte per Option, so a 55 peer would misread every ScriptState
/// of a 56 client while the handshake passed).
/// 57: Summon (MsgType 83, wire id 91) and CreateActorProps' optional
/// `own_local` (thuum tracker #84, docs/verbs/summon.md, ADR-052 decision 3
/// as amended 2026-11-16, ADR-063: the caster's row owns its summon): a
/// player's engine's summon reported at its summon effect's apply (its
/// place, its base, the effect and the spell, the time left) and at its end,
/// and the engine reference the summon's create word names to its caster
/// alone, so the caster's view binds the engine's own summon instead of
/// placing a copy (the summon is R2: the caster's engine simulates it, the
/// server records within bounds). One number, one shape (D-PH1 A's reason:
/// postcard writes a struct's fields in order with one presence byte per
/// Option, so a 56 peer would misread every CreateActor of a 57 server
/// while the handshake passed).
/// 58: Summon's `caster_id` (thuum tracker #106, docs/verbs/summon.md): an
/// NPC's summon reported by the game that simulates the NPC, the caster
/// named by its server id, 0 for the sender's own player as before. 57 has
/// shipped (the playtest 53 drop, 00b30651's server image and Windows dist),
/// so the field takes the next number (D-PH1 A's reason: postcard writes a
/// struct's fields in order, so a 57 server would read a 58 client's Summon
/// four bytes long while the handshake passed).
/// 59: CreateActorProps' optional `summon_effect` (thuum tracker #111,
/// docs/verbs/summon.md; playtest 53, Eli: "they slide in from somewhere
/// instead of the purple sphere effect"): the summon effect a watcher's copy
/// of a summon plays the arrival art of, on its create word to every listener
/// but the caster within the record's arrival window. 58 has shipped (the
/// playtest 53 sitting's 5b5f9b15 client and 998ea29a server), so the field
/// takes the next number (D-PH1 A's reason: postcard writes a struct's fields
/// in order with one presence byte per Option, so a 58 peer would misread
/// every CreateActor of a 59 server while the handshake passed).
/// 60: RelayedActivation's `presser_inventory` (thuum tracker #117, Eli's
/// design B): a player's press of a scripted world object relayed to the
/// cell's host with the presser's recorded inventory, which the host's
/// acting scope answers GetItemCount from. A new trailing field, so the
/// number moves for D-PH1 A's reason.
/// 61: CraftReport (MsgType 84, wire id 92) and the created forms: Entry's,
/// PutItem's and TakeItem's optional `created_base`, `created_enchantment`
/// and `created_poison` and OnEquip's optional `created_base`, a brewed
/// potion or poison or a made enchantment named by its definition, never by
/// the crafting session's own id; and a crafting sitting's lost and gained
/// stacks and learned enchantments, recorded as the crafter's engine read
/// them. One number, one shape: postcard writes a struct's fields in order
/// with one presence byte per Option, so a 60 peer would misread every
/// inventory of a 61 server while the handshake passed.
/// 62: CraftReport's `seq` and SetInventory's optional `craft_ack`: the
/// server's answer to a crafting report names it, so the seat ends its hold
/// on that answer and on no inventory sent before the report was read. A 61
/// peer would misread every SetInventory of a 62 server while the handshake
/// passed.
/// 63: DropItem carries the dropped stack's extras and its three created
/// forms, as PutItem and TakeItem do, so a dropped brew or enchanted item
/// matches its row of the record; and SpellCast's data carries
/// `created_spell`, a made enchantment by its definition, which the
/// server's relay of an enchanted weapon's hit names to the struck actor's
/// own game. A 62 peer would misread every DropItem and SpellCast.
/// 64: PushActor (MsgType 85, wire id 93): a script's push onto an actor
/// another seat simulates, reported for that actor's simulator to throw.
/// 65: Arrest and CrimeGold (MsgType 86 and 87, wire ids 94 and 95): an
/// arrest the arrested player's own game reports, and one hold's bounty as
/// that game holds it, the term, its end on the shared clock and the bounty
/// per hold the server's record; and Entry's, PutItem's and TakeItem's
/// optional `owner_id`, a stack's owner, the engine's stolen mark, recorded
/// from the player's own game so the engine's confiscation into a hold's
/// evidence chest moves it on the record too.
/// 66: DropItem's optional `owner_id`, the dropped stack's owner, so the
/// record takes the copy the player's own game dropped, a stolen one or a
/// clean one of the same item beside it. A 65 peer would misread every
/// DropItem.
/// MsgTypes 84 to 87 (wire ids 92 to 95) are all built here; none is held.
pub const SCHEMA_VERSION: u16 = 66;

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
    /// Favorites one player keeps, items and magic: far past what the
    /// inventory and magic menus show a player at once.
    pub const FAVORITES: usize = 128;
    /// Sighted actors one host names in one report (docs/verbs/npc-puppet.md):
    /// a village's residents and visitors at once, with room.
    pub const ACTOR_SIGHTINGS: usize = 64;
    /// Fates in one message (docs/verbs/npc-fate.md, ADR-049): the game-file
    /// actors of the cells a player's engine is about to attach, with room.
    pub const ACTOR_FATES: usize = 64;
    /// Worn items the server's outfit roll put on one actor.
    pub const FATE_WORN: usize = 32;
    /// Inventory rows the server's container roll put in one actor's record
    /// apart from the worn items (playtest 44's 1c): a bandit's loot is a
    /// handful; a vendor's wares sit in its chest, not on it. Sixty-four
    /// fates of thirty-two rows keep ActorFate's bound at 32 KiB.
    pub const FATE_INV: usize = 32;
    /// Leveled templates one NPC resolves through.
    pub const TEMPLATE_CHAIN: usize = 64;
    /// Races a server restricts to listed profiles (Skyrim.esm has 99 RACE
    /// records, ten of them playable; a mod list's custom races are a few).
    pub const RACES: usize = 128;
    /// Forms in one list of a player's race overlay (thuum ADR-068 D-T1 A:
    /// the spells, shouts or items a worn race's scripts added; the
    /// werewolf's level abilities, its howl and WolfSkinFXArmor are a few),
    /// the client's own bound (sync/raceOverlay.ts).
    pub const OVERLAY_FORMS: usize = 64;
    /// Looks one actor's CreateActor serves (the Undead FX build; the
    /// review of the client's 3D keep, eaa6ba47): the server keeps at most
    /// this many per actor (wire-rules actor_looks::LOOKS_KEPT), as the
    /// client does (LOOKS_MAX, objectAnimationService.ts).
    pub const LOOKS: usize = 8;
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
    /// A RaceMenu preset, JSON text (docs/verbs/racemenu-sync.md): under the
    /// transport's 256 KiB from a client, until a sculpted preset measured in
    /// the lab sets it.
    pub const RACEMENU_PRESET: usize = 192 * 1024;
    /// Actor values the engine has (CommonLibSSE-NG
    /// include/RE/A/ActorValues.h, 0 to 163).
    pub const ACTOR_VALUES: usize = 164;
    /// Skills in the player's progress (PlayerSkills::Skills::kTotal).
    pub const SKILLS: usize = 18;
    /// Effects running on one actor (thuum docs/verbs/magic-effects.md:
    /// wire-rules magic::MAX_EFFECTS).
    pub const MAGIC_EFFECTS: usize = 32;
    /// Globals the server keeps per player (thuum docs/verbs/player-globals.md:
    /// Apocalypse's 55 with room for more mods).
    pub const PLAYER_GLOBALS: usize = 256;
    /// Bases one PlacedRequest asks for: wire-rules placements' MAX_OBJECTS
    /// (Raise Wall's two, with room for a cage's walls).
    pub const PLACED_BASES: usize = 8;
    /// Skill uses one SkillUse carries: one per skill, the eighteen skills
    /// (docs/verbs/leveling.md).
    pub const SKILL_USES: usize = 18;
    /// The most perks one PerkState carries (docs/verbs/perks.md; Skyrim's trees
    /// hold 251 ranks).
    pub const PERKS: usize = 512;
    /// Quests whose stages are the world's (thuum docs/verbs/quest-stage.md:
    /// the server's `sharedQuests` list, Riverwood's first).
    pub const SHARED_QUESTS: usize = 256;
    /// The stages one run word names whose fragments the engine runs with it
    /// (thuum ADR-068 rung 5, D-Q3 A): stage 0 and the start-up stages, or
    /// the shut-down stages; a quest flags one or two (Skyrim.esm,
    /// lab/esm.py on sky-srv, 2026-11-10).
    pub const WORD_STAGES: usize = 16;
    /// A report's root frame (thuum ADR-068 rung 5, D-Q4 C), a script and
    /// one of its functions, "<Script>.<Function>": two [`PAPYRUS_NAME`]s
    /// and the dot between them.
    pub const QUEST_ROOT: usize = 2 * PAPYRUS_NAME + 1;
    /// Actor values one actor's running buffs touch: two per effect at most
    /// (a dual value modifier's second).
    pub const EFFECT_MODIFIERS: usize = 2 * MAGIC_EFFECTS;
    /// A gamemode event name.
    pub const EVENT_NAME: usize = 256;
    /// Arguments of one gamemode event.
    pub const EVENT_ARGS: usize = 64;
    /// One gamemode event argument, JSON text.
    pub const EVENT_ARG_JSON: usize = 64 * 1024;
    /// A console command name.
    pub const CONSOLE_COMMAND: usize = 64;
    /// A console reply's line (thuum docs/verbs/console-commands.md).
    pub const CONSOLE_OUTPUT: usize = 1024;
    /// Arguments of one console command.
    pub const CONSOLE_ARGS: usize = 32;
    /// One console command argument.
    pub const CONSOLE_ARG: usize = 256;
    /// A Papyrus class, function or type name.
    pub const PAPYRUS_NAME: usize = 128;
    /// Arguments of one delegated Papyrus call.
    pub const SNIPPET_ARGS: usize = 64;
    /// Arguments of one reported native call (thuum ADR-068): the most a
    /// native takes (CommonLibSSE-NG src/RE/N/NativeFunctionBase.cpp:15,
    /// "native supports max 11").
    pub const SCRIPT_CALL_ARGS: usize = 11;
    /// Landings one reported leveled AddItem carries (thuum D-R1 C's leveled
    /// gap, train 52): a use-all list lands each of its entries, and a
    /// leveled list's entry count is one byte (CommonLibSSE-NG
    /// include/RE/T/TESLeveledList.h:72, `numEntries`, LLCT); the platform
    /// folds the landings by base and never sends more.
    pub const SCRIPT_CALL_LANDED: usize = 255;
    /// A Papyrus string argument or return value.
    pub const SNIPPET_STRING: usize = 16 * 1024;
    /// Scripts one script record carries (thuum ADR-068 rung 3): the scripts
    /// attached to one reference (the Temple's puzzle lever 0xaa8c3 carries
    /// two, docs/research/trap-census-2026-11-05.md). UNMEASURED beyond the
    /// census.
    pub const SCRIPT_OBJECTS: usize = 16;
    /// Variables of one attached script, its chain's total
    /// (GetTotalNumVariables, H-R1): the largest the save read met,
    /// TrapSwingingWall over MovingTrap over TrapBase, has 52 (thuum
    /// docs/research/adr065-papyrus-state-2026-11-03.md Q1).
    pub const SCRIPT_VARS: usize = 512;
    /// A string variable's value in a script record. UNMEASURED: a save
    /// read's string histogram (rung 3's script-save-trace.js v2.1) sets it.
    pub const SCRIPT_STRING: usize = 1024;
    /// Elements of one array variable: the VM's own bound (CommonLibSSE-NG
    /// include/RE/A/Array.h:69, MAX_SIZE 128).
    pub const SCRIPT_ARRAY: usize = 128;
    /// Behavior graph variables one ScriptStateApply serves for its
    /// reference (thuum D-C3 A, PENDING Eli): the server keeps at most this
    /// many per reference, a new name past them replacing the oldest; the
    /// cobweb's script sets one (fToggleBlend). UNMEASURED.
    pub const GRAPH_VARS: usize = 16;
    /// A world object's animation graph state as its owner's engine saves it
    /// (thuum docs/verbs/traps.md "The graph state", ADR-065 decision 2):
    /// the frame the engine's own BGSLoadGameSubBuffer::SaveAnimation
    /// (Address Library 1.6.1170 36015) writes at a 3D unload, a version
    /// byte, the data's size as the engine's variable-width value and
    /// TESObjectREFR::SaveAnimation's data (20098, the bytes a save's
    /// kAnimation chunk carries). UNMEASURED: no world object's capture is
    /// read yet (x-graph-state v1 reads the sizes); a capture past it is not
    /// sent and its rung 3 replay stands.
    pub const GRAPH_STATE: usize = 8 * 1024;
    /// The same frame as it travels: two lowercase hex characters a byte,
    /// ChangeForm's spelling (the platform's SerializeGraphState hands it so
    /// and the client forwards it as is).
    pub const GRAPH_STATE_HEX: usize = 2 * GRAPH_STATE;
    /// World globals in one WorldGlobal (thuum ADR-068 rung 4): Skyrim.esm's
    /// GLOB records number in the hundreds and a second's diff moves a
    /// handful (UNMEASURED: x-world-globals-rate v1 reads the rate).
    pub const WORLD_GLOBALS: usize = 512;
    /// Keys in one ScriptVariables's watch list, and entries in one (thuum
    /// ADR-068 rung 5): the census lists 770 condition-read keys in the
    /// five masters (docs/research/rung5-quests-2026-11-09.md section 7), so
    /// a login sends the watch in several messages; a second's report moves
    /// a handful. UNMEASURED past the census.
    pub const SCRIPT_VARIABLES: usize = 256;
    /// Entries in one gamemode code list.
    pub const GAMEMODE_ENTRIES: usize = 256;
    /// One gamemode function's JavaScript source.
    pub const GAMEMODE_SOURCE: usize = 256 * 1024;
    /// Gamemode properties on one form.
    pub const CUSTOM_PROPS: usize = 256;
    /// A change-form record's body as the engine packs it (thuum
    /// docs/verbs/change-form.md, ADR-065): the research note's largest
    /// actor record is 4,795 bytes (an ACHR at kDying with its process
    /// block), so 64 KiB holds a record with its inventory many times over
    /// and still stops a flood at the shape.
    pub const CHANGE_FORM_BODY: usize = 65536;
    /// The same body as it travels: two lowercase hex characters a byte, the
    /// spelling the native's serializer hands the client and the client
    /// forwards as is (the JSON form and the wire's are one string).
    pub const CHANGE_FORM_BODY_HEX: usize = 2 * CHANGE_FORM_BODY;
    /// Remap rows one record carries: the type-0 ids its body names
    /// (Sven's kDead record had 152 ids, 103 of them portable; D6's census).
    pub const CHANGE_FORM_REMAP: usize = 256;
    /// Body poses one prop sample carries (thuum ADR-071 decision 6,
    /// PropTransform v2): a humanoid's ragdoll has 19 bodies, 18 bones and
    /// the weapon (x-body-impose 20261111-194559, fitsIn64 true); creatures
    /// and multi-body world objects are unread, so 64 with room.
    pub const PROP_BONES: usize = 64;
    /// Effects of one created form: a brewed potion's are the effects its
    /// ingredients share, and three ingredients of four effects each share
    /// at most six (twelve slots, two to an effect); an enchantment made at
    /// the enchanter joins at most two known enchantments (the Extra Effect
    /// perk), each with its own list, whose longest in the masters is unread
    /// (UNMEASURED). Eight holds a potion's six and two enchantments of four;
    /// a form past it is not sent and its stack stays the crafter's seat's
    /// alone.
    pub const CREATED_EFFECTS: usize = 8;
    /// Stacks one crafting report names on each side: a sitting's distinct
    /// inputs and products, a forge's or a lab's run of several recipes; the
    /// client sends a longer sitting as several reports, each standing alone.
    pub const CRAFT_ROWS: usize = 64;
    /// Enchantments one crafting report teaches: a disenchant destroys one
    /// stack's item and teaches its game-file enchantment or that
    /// enchantment's base, two per lost stack at most (assumes the engine's
    /// disenchant teaches nothing more).
    pub const CRAFT_LEARNED: usize = 2 * CRAFT_ROWS;
    /// References one prop claim names (PropClaim v2): one for a touch, an
    /// impact's bodies for rung 3; the Riverwood cart's blast moved at most
    /// eight with every item Dynamic (thuum lab/.cache/drafts/physics/
    /// read-x-prop-blast-223256.md section 3), so 16 with room.
    pub const PROP_CLAIM_REFRS: usize = 16;
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
pub struct ReservedHostedActorState {
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
    /// Id 5: the M0 row for a host's NPC state, reserved and dead (thuum
    /// docs/verbs/npc-stream.md: the state travels as the family message
    /// [`skymp::HostedActorState`], MsgType 53; ADR-012 keeps the id). See
    /// [`ReservedHostedActorState`].
    ReservedHostedActor(ReservedHostedActorState),
    /// Id 6: Server to clients, R0 output: one applied inventory change for
    /// clients to render. The server sends each applied delta exactly once
    /// over ReliableUnordered; clients apply what they receive.
    InventoryApply {
        /// Whose inventory.
        owner: FormId,
        /// The change.
        delta: ItemDelta,
    },
    /// Id 7: the M0 reserved grant, `{ cell }`, never sent; kept dead so the
    /// ids stay (ADR-012). The cell-host verb's grant is [`Message::HostGrant`],
    /// id 56, in the SkyMP family's shape.
    ReservedHostGrant {
        /// The cell form.
        cell: FormId,
    },
    /// Id 8: the M0 reserved release, the same way; the verb's is
    /// [`Message::HostRelease`], id 57.
    ReservedHostRelease {
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
    /// 42, MsgType 34 (thuum). See [`skymp::SetGameTime`].
    SetGameTime(skymp::SetGameTime),
    /// 43, MsgType 35 (thuum). See [`skymp::RestIntent`].
    RestIntent(skymp::RestIntent),
    /// 44, MsgType 36 (thuum). See [`skymp::MapMarkerDiscovered`].
    MapMarkerDiscovered(skymp::MapMarkerDiscovered),
    /// 45, MsgType 37 (thuum). See [`skymp::IngredientEffectsKnown`].
    IngredientEffectsKnown(skymp::IngredientEffectsKnown),
    /// 46, MsgType 38 (thuum). See [`skymp::Favorites`].
    Favorites(skymp::Favorites),
    /// 47, MsgType 39 (thuum). See [`skymp::RaceMenuPreset`].
    RaceMenuPreset(skymp::RaceMenuPreset),
    /// 48, MsgType 40 (thuum). See [`skymp::ActorValues`].
    ActorValues(skymp::ActorValues),
    /// 49, MsgType 41 (thuum). See [`skymp::ConsoleOutput`].
    ConsoleOutput(skymp::ConsoleOutput),
    /// 50, MsgType 42 (thuum). See [`skymp::ArrowShot`].
    ArrowShot(skymp::ArrowShot),
    /// 51, MsgType 43 (thuum). See [`skymp::MagicEffects`].
    MagicEffects(skymp::MagicEffects),
    /// 52, MsgType 44 (thuum). See [`skymp::UpdateAction`].
    UpdateAction(skymp::UpdateAction),
    /// 53, MsgType 45 (thuum). See [`skymp::PlayerGlobals`].
    PlayerGlobals(skymp::PlayerGlobals),
    /// 54, MsgType 46 (thuum). See [`skymp::ActorJump`].
    ActorJump(skymp::ActorJump),
    /// 55, MsgType 47 (thuum). See [`skymp::CellLoaded`].
    CellLoaded(skymp::CellLoaded),
    /// 56, MsgType 48 (thuum). See [`skymp::HostGrant`].
    HostGrant(skymp::HostGrant),
    /// 57, MsgType 49 (thuum). See [`skymp::HostRelease`].
    HostRelease(skymp::HostRelease),
    /// 58, MsgType 50 (thuum). See [`skymp::ActorSightings`].
    ActorSightings(skymp::ActorSightings),
    /// 59, MsgType 51 (thuum). See [`skymp::ActorFate`].
    ActorFate(skymp::ActorFate),
    /// 60, MsgType 52 (thuum). See [`skymp::ActorDeath`].
    ActorDeath(skymp::ActorDeath),
    /// 61, MsgType 53 (thuum). See [`skymp::HostedActorState`].
    HostedActorState(skymp::HostedActorState),
    /// 62, MsgType 54 (thuum). See [`skymp::ActorSpeech`].
    ActorSpeech(skymp::ActorSpeech),
    /// 63, MsgType 55 (thuum). See [`skymp::DamageCause`].
    DamageCause(skymp::DamageCause),
    /// 64, MsgType 56 (thuum). See [`skymp::ActorClaim`].
    ActorClaim(skymp::ActorClaim),
    /// 65, MsgType 57 (thuum). See [`skymp::QuestStage`].
    QuestStage(skymp::QuestStage),
    /// 66, MsgType 58 (thuum). See [`skymp::PropTransform`].
    PropTransform(skymp::PropTransform),
    /// 67, MsgType 59 (thuum). See [`skymp::PropClaim`].
    PropClaim(skymp::PropClaim),
    /// 68, MsgType 60 (thuum). See [`skymp::PropOwner`].
    PropOwner(skymp::PropOwner),
    /// 69, MsgType 61 (thuum). See [`skymp::PlacedRequest`].
    PlacedRequest(skymp::PlacedRequest),
    /// 70, MsgType 62 (thuum). See [`skymp::PlacedReport`].
    PlacedReport(skymp::PlacedReport),
    /// 71, MsgType 63 (thuum). See [`skymp::SkillUse`].
    SkillUse(skymp::SkillUse),
    /// 72, MsgType 64 (thuum). See [`skymp::LockReport`].
    LockReport(skymp::LockReport),
    /// 73, MsgType 65 (thuum). See [`skymp::LockPrompt`].
    LockPrompt(skymp::LockPrompt),
    /// 74, MsgType 66 (thuum). See [`skymp::SetWeather`].
    SetWeather(skymp::SetWeather),
    /// 75, MsgType 67 (thuum). See [`skymp::PerkPick`].
    PerkPick(skymp::PerkPick),
    /// 76, MsgType 68 (thuum). See [`skymp::PerkState`].
    PerkState(skymp::PerkState),
    /// 77, MsgType 69 (thuum). See [`skymp::WordWall`].
    WordWall(skymp::WordWall),
    /// 78, MsgType 70 (thuum). See [`skymp::RelayedActivation`].
    RelayedActivation(skymp::RelayedActivation),
    /// 79, MsgType 71 (thuum). See [`skymp::ObjectAnimation`].
    ObjectAnimation(skymp::ObjectAnimation),
    /// 80, MsgType 72 (thuum). See [`skymp::ChangeForm`].
    ChangeForm(skymp::ChangeForm),
    /// 81, MsgType 73 (thuum). See [`skymp::ChangeFormAck`].
    ChangeFormAck(skymp::ChangeFormAck),
    /// 82, MsgType 74 (thuum). See [`skymp::ChangeFormApply`].
    ChangeFormApply(skymp::ChangeFormApply),
    /// 83, MsgType 75 (thuum). See [`skymp::ScriptCall`].
    ScriptCall(skymp::ScriptCall),
    /// 84, MsgType 76 (thuum ADR-068 rung 3; the id D-S3 held for it while
    /// DestructionStage, 78, was built first). See [`skymp::ScriptState`].
    ScriptState(skymp::ScriptState),
    /// 85, MsgType 77 (thuum ADR-068 rung 3, held the same way). See
    /// [`skymp::ScriptStateApply`].
    ScriptStateApply(skymp::ScriptStateApply),
    /// 86, MsgType 78 (thuum). See [`skymp::DestructionStage`].
    DestructionStage(skymp::DestructionStage),
    /// 87, MsgType 79 (thuum ADR-068 rung 4). See [`skymp::AliasFill`].
    AliasFill(skymp::AliasFill),
    /// 88, MsgType 80 (thuum ADR-068 rung 4). See [`skymp::WorldGlobal`].
    WorldGlobal(skymp::WorldGlobal),
    /// 89, MsgType 81 (thuum ADR-068 rung 5). See [`skymp::ScriptVariables`].
    ScriptVariables(skymp::ScriptVariables),
    /// 90, MsgType 82 (thuum D-53-8 B). See [`skymp::OwnHeadParts`].
    OwnHeadParts(skymp::OwnHeadParts),
    /// 91, MsgType 83 (thuum tracker #84, docs/verbs/summon.md). See
    /// [`skymp::Summon`].
    Summon(skymp::Summon),
    /// 92, MsgType 84 (crafting). See [`skymp::CraftReport`].
    CraftReport(skymp::CraftReport),
    /// 93, MsgType 85. See [`skymp::PushActor`].
    PushActor(skymp::PushActor),
    /// 94, MsgType 86. See [`skymp::Arrest`].
    Arrest(skymp::Arrest),
    /// 95, MsgType 87. See [`skymp::CrimeGold`].
    CrimeGold(skymp::CrimeGold),
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
/// with room to spare, from the capacities above, where a size test pins
/// it; the transport's own per-direction cap (smaller from clients) applies
/// on top. The inventory-carrying messages are the exception, their caps
/// below the product of their capacities: a stack at its caps is 877 bytes
/// since SCHEMA_VERSION 65 (the name and three created forms at theirs, and
/// the owner), so cap::INVENTORY's 4096 such stacks would take 3.6 MB, past
/// SetInventory's and RelayedActivation's 2 MiB, CreateActor's two
/// inventories past every cap, and UpdateEquipment past the 256 KiB the
/// transport takes from a client. No real inventory nears it (a merchant's
/// chest holds hundreds of stacks of a few dozen bytes each), and a message
/// past its cap is refused at its encode, whole, never truncated.
const TABLE: [Row; 96] = [
    row("Hello", 0, 4 * KIB, C2S),
    row("Welcome", 0, 32, S2C),
    row("Refuse", 0, 8, S2C),
    row("Movement", 0, 64, C2S),
    row("Hit", 0, 64, C2S),
    row("ReservedHostedActor", 0, KIB, C2S),
    row("InventoryApply", 0, 32, S2C),
    row("ReservedHostGrant", 0, 16, S2C),
    row("ReservedHostRelease", 0, 16, S2C),
    row("CustomPacket", 1, MIB + 16, Both),
    row("UpdateMovement", 2, 256, Both),
    row("UpdateAnimation", 3, 256, Both),
    row("UpdateAppearance", 4, 64 * KIB, Both),
    row("UpdateEquipment", 5, 256 * KIB, Both),
    row("Activate", 6, 64, C2S),
    row("UpdateProperty", 7, 65 * KIB, S2C),
    // the stack's id, count and target, its extras with the name at its
    // cap, and since SCHEMA_VERSION 61 three created forms at their caps
    // (877 bytes at the longest, pinned by
    // a_stack_with_its_created_forms_fits_its_messages_byte_caps)
    row("PutItem", 8, KIB, C2S),
    row("TakeItem", 9, KIB, C2S),
    row("FinishSpSnippet", 10, 17 * KIB, C2S),
    // the base, and since SCHEMA_VERSION 61 a brewed potion's definition at
    // its caps (193 bytes at the longest, pinned by the same test)
    row("OnEquip", 11, 256, C2S),
    row("ConsoleCommand", 12, 16 * KIB, C2S),
    row("CraftItem", 13, 64 * KIB, C2S),
    row("Host", 14, 16, C2S),
    row("CustomEvent", 15, 256 * KIB, C2S),
    row("ChangeValues", 16, 32, Both),
    row("OnHit", 17, 64, C2S),
    row("DeathStateContainer", 18, 66 * KIB, S2C),
    // the base and the count, since SCHEMA_VERSION 63 a put's extras and
    // three created forms, and since 66 the owner (pinned by the same test as
    // PutItem's)
    row("DropItem", 19, KIB, C2S),
    row("Teleport", 20, 64, S2C),
    row("OpenContainer", 21, 16, S2C),
    row("PlayerBowShot", 22, 64, C2S),
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
    row("SetGameTime", 34, 32, S2C),
    row("RestIntent", 35, 16, C2S),
    row("MapMarkerDiscovered", 36, 16, C2S),
    row("IngredientEffectsKnown", 37, 16, C2S),
    row("Favorites", 38, KIB, Both),
    row("RaceMenuPreset", 39, 200 * KIB, Both),
    row("ActorValues", 40, 2 * KIB, Both),
    row("ConsoleOutput", 41, 2 * KIB, S2C),
    row("ArrowShot", 42, 64, S2C),
    row("MagicEffects", 43, KIB, S2C),
    row("UpdateAction", 44, 32 * KIB, Both),
    row("PlayerGlobals", 45, 4 * KIB, Both),
    row("ActorJump", 46, 16, S2C),
    row("CellLoaded", 47, 16, C2S),
    row("HostGrant", 48, 32, S2C),
    row("HostRelease", 49, 32, S2C),
    row("ActorSightings", 50, 4 * KIB, C2S),
    row("ActorFate", 51, 32 * KIB, S2C),
    row("ActorDeath", 52, KIB, C2S),
    // the tag, the idx, three floats, five ids and three bytes (36 at their
    // widest), and the host's running effects: a presence byte, a one-byte
    // length and 32 ids of five bytes (198 at the caps, pinned by
    // a_hosted_state_with_its_effects_at_the_cap_fits_its_byte_cap; thuum
    // docs/verbs/npc-puppet.md, Ownership)
    row("HostedActorState", 53, 256, Both),
    row("ActorSpeech", 54, 32, Both),
    row("DamageCause", 55, 32, S2C),
    // an idx, the flag and since SCHEMA_VERSION 47 a purpose, three form ids
    // and a release's why, each with its presence byte: 29 bytes at the caps
    // (thuum ADR-073 step 5a, D-53-5 B)
    row("ActorClaim", 56, 48, C2S),
    row("QuestStage", 57, 4 * KIB, Both),
    // every id and the sequence at their widest varints, the four vectors,
    // the two flags and 64 bones of 28 bytes (thuum ADR-071 decision 6,
    // SCHEMA_VERSION 46; 1,862 bytes at the longest, pinned by
    // a_prop_sample_and_claim_at_their_longest_fit_their_caps)
    row("PropTransform", 58, 2 * KIB, Both),
    // 16 references at five-byte varints and the touch (83 bytes)
    row("PropClaim", 59, 96, C2S),
    row("PropOwner", 60, 32, S2C),
    row("PlacedRequest", 61, 64, S2C),
    row("PlacedReport", 62, 48, C2S),
    row("SkillUse", 63, 128, C2S),
    // a five-byte refr, the outcome and the broken count, and since ADR-073
    // step 3 an engine write's `locked` and `level`, each behind its presence
    // byte: 12 bytes with the wire id (an_engine_lock_write_fits_its_byte_cap)
    row("LockReport", 64, 16, C2S),
    row("LockPrompt", 65, 8, S2C),
    row("SetWeather", 66, 16, S2C),
    row("PerkPick", 67, 8, C2S),
    row("PerkState", 68, 4 * KIB, S2C),
    row("WordWall", 69, 8, C2S),
    // a target, a caster and the entry byte (thuum D-C2, SCHEMA_VERSION 40;
    // 13 bytes at the longest, pinned by wire_id_matches_postcard_variant_index)
    // since SCHEMA_VERSION 60 (thuum tracker #117) it may carry the
    // presser's inventory, SetInventory's own shape, so SetInventory's cap
    row("RelayedActivation", 70, 2 * MIB, S2C),
    row("ObjectAnimation", 71, 160, Both),
    // a record's hex body at its cap (128 KiB) plus every remap row at
    // five-byte varints and the scalars, with room (docs/verbs/change-form.md)
    row("ChangeForm", 72, 136 * KIB, C2S),
    row("ChangeFormAck", 73, 16, S2C),
    row("ChangeFormApply", 74, 136 * KIB, S2C),
    // eleven arguments at SNIPPET_STRING's 16 KiB each, the four names at
    // PAPYRUS_NAME and, since SCHEMA_VERSION 44, 255 landings at ten varint
    // bytes each, with room (183,365 bytes at the fullest, pinned by the size
    // test; thuum ADR-068, D-R1 C's leveled gap)
    row("ScriptCall", 75, 180 * KIB, C2S),
    // a reference's script record (thuum ADR-068 rung 3): under the 256 KiB
    // the transport takes from a client, with room over the 160 KiB answer
    // the owner's native refuses past (its JSON, which postcard spells
    // shorter); the apply adds the sequence and the view
    row("ScriptState", 76, 192 * KIB, C2S),
    row("ScriptStateApply", 77, 256 * KIB, S2C),
    // a reference, a stage byte, two flags, the ref state's two words and
    // the object's sequence (thuum D-S3, ADR-068 rung 4; 16 bytes at the
    // longest, pinned by wire_id_matches_postcard_variant_index)
    row("DestructionStage", 78, 32, Both),
    // a quest, an alias, a reference and a flag (thuum ADR-068 rung 4; 17
    // bytes at the longest)
    row("AliasFill", 79, 32, Both),
    // 512 entries at nine bytes and the scalars (thuum ADR-068 rung 4;
    // 4,617 bytes at the longest)
    row("WorldGlobal", 80, 8 * KIB, Both),
    // 256 watched keys and 256 entries, every name at PAPYRUS_NAME and
    // every number at its widest varint (thuum ADR-068 rung 5; 141,062
    // bytes at the longest, pinned by the size test), under the 256 KiB the
    // transport takes from a client
    row("ScriptVariables", 81, 144 * KIB, Both),
    // a race and 64 parts at five-byte varints, the count and the flag
    // (thuum D-53-8 B; 328 bytes at the longest, pinned by the size test)
    row("OwnHeadParts", 82, 512, C2S),
    // the local id, the base, the effect and the spell at five-byte varints,
    // the two triple floats, the cell, the time and the flag (thuum
    // tracker #84; 55 bytes at the longest, pinned by the size test)
    row("Summon", 83, 64, C2S),
    // the bench and since SCHEMA_VERSION 62 the report's seq at five-byte
    // varints, 64 lost and 64 gained stacks at their longest (877 bytes
    // each, three created forms at their caps beside the name's and the
    // owner) and 128 learned enchantments at five-byte varints (112,911
    // bytes at the longest, pinned by the size test), under the 256 KiB the
    // transport takes from a client
    row("CraftReport", 84, 112 * KIB, C2S),
    // two ids at five-byte varints and the force (16 bytes at the longest)
    row("PushActor", 85, 32, C2S),
    // a faction and a sentence at five-byte varints
    row("Arrest", 86, 16, C2S),
    // a faction and two sums at five-byte varints
    row("CrimeGold", 87, 32, C2S),
];

/// Wire ids in use: one past the last variant.
/// Wire ids in use: one past the last variant, the table's length (ActorJump
/// at 54 found a stale 54 here, fuzz validate 2026-10-18; the assertion below
/// pins the two together).
pub const WIRE_IDS: u32 = 96;
const _: () = assert!(TABLE.len() == 96, "WIRE_IDS is the table's length");

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
            Message::ReservedHostedActor(_) => 5,
            Message::InventoryApply { .. } => 6,
            Message::ReservedHostGrant { .. } => 7,
            Message::ReservedHostRelease { .. } => 8,
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
            Message::SetGameTime(_) => 42,
            Message::RestIntent(_) => 43,
            Message::MapMarkerDiscovered(_) => 44,
            Message::IngredientEffectsKnown(_) => 45,
            Message::Favorites(_) => 46,
            Message::RaceMenuPreset(_) => 47,
            Message::ActorValues(_) => 48,
            Message::ConsoleOutput(_) => 49,
            Message::ArrowShot(_) => 50,
            Message::MagicEffects(_) => 51,
            Message::UpdateAction(_) => 52,
            Message::PlayerGlobals(_) => 53,
            Message::ActorJump(_) => 54,
            Message::CellLoaded(_) => 55,
            Message::HostGrant(_) => 56,
            Message::HostRelease(_) => 57,
            Message::ActorSightings(_) => 58,
            Message::ActorFate(_) => 59,
            Message::ActorDeath(_) => 60,
            Message::HostedActorState(_) => 61,
            Message::ActorSpeech(_) => 62,
            Message::DamageCause(_) => 63,
            Message::ActorClaim(_) => 64,
            Message::QuestStage(_) => 65,
            Message::PropTransform(_) => 66,
            Message::PropClaim(_) => 67,
            Message::PropOwner(_) => 68,
            Message::PlacedRequest(_) => 69,
            Message::PlacedReport(_) => 70,
            Message::SkillUse(_) => 71,
            Message::LockReport(_) => 72,
            Message::LockPrompt(_) => 73,
            Message::SetWeather(_) => 74,
            Message::PerkPick(_) => 75,
            Message::PerkState(_) => 76,
            Message::WordWall(_) => 77,
            Message::RelayedActivation(_) => 78,
            Message::ObjectAnimation(_) => 79,
            Message::ChangeForm(_) => 80,
            Message::ChangeFormAck(_) => 81,
            Message::ChangeFormApply(_) => 82,
            Message::ScriptCall(_) => 83,
            Message::ScriptState(_) => 84,
            Message::ScriptStateApply(_) => 85,
            Message::DestructionStage(_) => 86,
            Message::AliasFill(_) => 87,
            Message::WorldGlobal(_) => 88,
            Message::ScriptVariables(_) => 89,
            Message::OwnHeadParts(_) => 90,
            Message::Summon(_) => 91,
            Message::CraftReport(_) => 92,
            Message::PushActor(_) => 93,
            Message::Arrest(_) => 94,
            Message::CrimeGold(_) => 95,
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
        assert_eq!(Message::HostGrant(skymp::HostGrant::default()).name(), "HostGrant");
        assert_eq!(Message::ReservedHostGrant { cell: FormId(1) }.name(), "ReservedHostGrant");
        assert_eq!(Message::ScriptState(skymp::ScriptState::default()).name(), "ScriptState");
        assert_eq!(Message::ScriptStateApply(skymp::ScriptStateApply::default()).name(), "ScriptStateApply");
        assert_eq!(Message::DestructionStage(skymp::DestructionStage::default()).name(), "DestructionStage");
        assert_eq!(Message::AliasFill(skymp::AliasFill::default()).name(), "AliasFill");
        assert_eq!(Message::WorldGlobal(skymp::WorldGlobal::default()).name(), "WorldGlobal");
        assert_eq!(Message::ScriptVariables(skymp::ScriptVariables::default()).name(), "ScriptVariables");
        assert_eq!(Message::OwnHeadParts(skymp::OwnHeadParts::default()).name(), "OwnHeadParts");
        assert_eq!(Message::Summon(skymp::Summon::default()).name(), "Summon");
        assert_eq!(Message::CraftReport(skymp::CraftReport::default()).name(), "CraftReport");
        assert_eq!(Message::Arrest(skymp::Arrest::default()).name(), "Arrest");
        assert_eq!(Message::CrimeGold(skymp::CrimeGold::default()).name(), "CrimeGold");
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
        assert_eq!(name_of_msg_type(34), Some("SetGameTime"));
        assert_eq!(name_of_msg_type(35), Some("RestIntent"));
        assert_eq!(name_of_msg_type(36), Some("MapMarkerDiscovered"));
        assert_eq!(name_of_msg_type(37), Some("IngredientEffectsKnown"));
        assert_eq!(name_of_msg_type(38), Some("Favorites"));
        assert_eq!(name_of_msg_type(39), Some("RaceMenuPreset"));
        assert_eq!(name_of_msg_type(40), Some("ActorValues"));
        assert_eq!(name_of_msg_type(41), Some("ConsoleOutput"));
        assert_eq!(name_of_msg_type(42), Some("ArrowShot"));
        assert_eq!(name_of_msg_type(43), Some("MagicEffects"));
        assert_eq!(name_of_msg_type(44), Some("UpdateAction"));
        assert_eq!(name_of_msg_type(45), Some("PlayerGlobals"));
        assert_eq!(name_of_msg_type(46), Some("ActorJump"));
        assert_eq!(name_of_msg_type(47), Some("CellLoaded"));
        assert_eq!(name_of_msg_type(48), Some("HostGrant"));
        assert_eq!(name_of_msg_type(49), Some("HostRelease"));
        assert_eq!(name_of_msg_type(50), Some("ActorSightings"));
        assert_eq!(name_of_msg_type(0), None);
        assert_eq!(name_of_msg_type(51), Some("ActorFate"));
        assert_eq!(name_of_msg_type(52), Some("ActorDeath"));
        assert_eq!(name_of_msg_type(53), Some("HostedActorState"));
        assert_eq!(name_of_msg_type(54), Some("ActorSpeech"));
        assert_eq!(name_of_msg_type(55), Some("DamageCause"));
        assert_eq!(name_of_msg_type(56), Some("ActorClaim"));
        assert_eq!(name_of_msg_type(57), Some("QuestStage"));
        assert_eq!(name_of_msg_type(58), Some("PropTransform"));
        assert_eq!(name_of_msg_type(59), Some("PropClaim"));
        assert_eq!(name_of_msg_type(60), Some("PropOwner"));
        assert_eq!(name_of_msg_type(61), Some("PlacedRequest"));
        assert_eq!(name_of_msg_type(62), Some("PlacedReport"));
        assert_eq!(name_of_msg_type(63), Some("SkillUse"));
        assert_eq!(name_of_msg_type(64), Some("LockReport"));
        assert_eq!(name_of_msg_type(65), Some("LockPrompt"));
        assert_eq!(name_of_msg_type(66), Some("SetWeather"));
        assert_eq!(name_of_msg_type(67), Some("PerkPick"));
        assert_eq!(name_of_msg_type(68), Some("PerkState"));
        assert_eq!(name_of_msg_type(69), Some("WordWall"));
        assert_eq!(name_of_msg_type(70), Some("RelayedActivation"));
        assert_eq!(name_of_msg_type(71), Some("ObjectAnimation"));
        assert_eq!(name_of_msg_type(72), Some("ChangeForm"));
        assert_eq!(name_of_msg_type(73), Some("ChangeFormAck"));
        assert_eq!(name_of_msg_type(74), Some("ChangeFormApply"));
        assert_eq!(name_of_msg_type(75), Some("ScriptCall"));
        // ADR-068's 76 and 77 at the ids D-S3 held for them (thuum rung 3)
        assert_eq!(name_of_msg_type(76), Some("ScriptState"));
        assert_eq!(name_of_msg_type(77), Some("ScriptStateApply"));
        assert_eq!(name_of_msg_type(78), Some("DestructionStage"));
        // ADR-068 rung 4's two, after 78: wire ids 87 and 88
        assert_eq!(name_of_msg_type(79), Some("AliasFill"));
        assert_eq!(name_of_msg_type(80), Some("WorldGlobal"));
        // ADR-068 rung 5's one, after 80: wire id 89
        assert_eq!(name_of_msg_type(81), Some("ScriptVariables"));
        // D-53-8 B's one, after 81: wire id 90
        assert_eq!(name_of_msg_type(82), Some("OwnHeadParts"));
        // tracker #84's one, after 82: wire id 91
        assert_eq!(name_of_msg_type(83), Some("Summon"));
        // CraftReport and PushActor, after 83: wire ids 92 and 93
        assert_eq!(name_of_msg_type(84), Some("CraftReport"));
        assert_eq!(name_of_msg_type(85), Some("PushActor"));
        assert_eq!(Message::CraftReport(skymp::CraftReport::default()).wire_id(), 92);
        assert_eq!(Message::PushActor(skymp::PushActor::default()).wire_id(), 93);
        assert_eq!(Message::PushActor(skymp::PushActor::default()).name(), "PushActor");
        assert_eq!(Message::PushActor(skymp::PushActor::default()).direction(), Direction::ClientToServer);
        // Arrest and CrimeGold, after 85: wire ids 94 and 95
        assert_eq!(name_of_msg_type(86), Some("Arrest"));
        assert_eq!(name_of_msg_type(87), Some("CrimeGold"));
        assert_eq!(name_of_msg_type(88), None);
        assert_eq!(Message::OwnHeadParts(skymp::OwnHeadParts::default()).wire_id(), 90);
        assert_eq!(Message::OwnHeadParts(skymp::OwnHeadParts::default()).msg_type(), Some(82));
        assert_eq!(Message::OwnHeadParts(skymp::OwnHeadParts::default()).direction(), Direction::ClientToServer);
        assert_eq!(Message::Summon(skymp::Summon::default()).wire_id(), 91);
        assert_eq!(Message::Summon(skymp::Summon::default()).msg_type(), Some(83));
        assert_eq!(Message::Summon(skymp::Summon::default()).direction(), Direction::ClientToServer);
        assert_eq!(Message::Arrest(skymp::Arrest::default()).wire_id(), 94);
        assert_eq!(Message::Arrest(skymp::Arrest::default()).msg_type(), Some(86));
        assert_eq!(Message::Arrest(skymp::Arrest::default()).direction(), Direction::ClientToServer);
        assert_eq!(Message::CrimeGold(skymp::CrimeGold::default()).wire_id(), 95);
        assert_eq!(Message::CrimeGold(skymp::CrimeGold::default()).msg_type(), Some(87));
        assert_eq!(Message::CrimeGold(skymp::CrimeGold::default()).direction(), Direction::ClientToServer);
        assert_eq!(Message::ScriptVariables(skymp::ScriptVariables::default()).wire_id(), 89);
        assert_eq!(Message::ScriptVariables(skymp::ScriptVariables::default()).msg_type(), Some(81));
        assert_eq!(Message::ScriptVariables(skymp::ScriptVariables::default()).direction(), Direction::Both);
        assert_eq!(Message::AliasFill(skymp::AliasFill::default()).wire_id(), 87);
        assert_eq!(Message::WorldGlobal(skymp::WorldGlobal::default()).wire_id(), 88);
        assert_eq!(Message::AliasFill(skymp::AliasFill::default()).direction(), Direction::Both);
        assert_eq!(Message::WorldGlobal(skymp::WorldGlobal::default()).direction(), Direction::Both);
        assert_eq!(Message::ScriptState(skymp::ScriptState::default()).msg_type(), Some(76));
        assert_eq!(Message::ScriptStateApply(skymp::ScriptStateApply::default()).msg_type(), Some(77));
        assert_eq!(Message::ScriptState(skymp::ScriptState::default()).wire_id(), 84);
        assert_eq!(Message::ScriptStateApply(skymp::ScriptStateApply::default()).wire_id(), 85);
        assert_eq!(Message::ScriptState(skymp::ScriptState::default()).direction(), Direction::ClientToServer);
        assert_eq!(Message::ScriptStateApply(skymp::ScriptStateApply::default()).direction(), Direction::ServerToClient);
        assert_eq!(Message::DestructionStage(skymp::DestructionStage::default()).direction(), Direction::Both);
    }

    /// A script record near the owner's native's answer cap (thuum ADR-068
    /// rung 3: the native refuses an answer past 160 KiB of JSON): sixteen
    /// scripts with their names at the cap, each with twenty-four strings of
    /// 384 bytes, the first one also with an object array of 128 handles at
    /// their caps.
    fn script_record_at_its_answer_cap() -> bounded::Vec<skymp::ScriptObjectState, { cap::SCRIPT_OBJECTS }> {
        let name = || -> String<{ cap::PAPYRUS_NAME }> { "x".repeat(cap::PAPYRUS_NAME).try_into().unwrap_or_default() };
        let handle = skymp::ScriptHandle { kind: 2, mid: u16::MAX, form: 0xfeff_ffff, cls: name() };
        let strings: alloc::vec::Vec<skymp::ScriptVar> = (0..24).map(|_| skymp::ScriptVar { k: 2, s: Some("x".repeat(384).try_into().unwrap_or_default()), ..Default::default() }).collect();
        let handles = skymp::ScriptVar {
            k: 11,
            elem: Some(name()),
            a: Some((0..cap::SCRIPT_ARRAY).map(|_| skymp::ScriptValue { k: 1, o: Some(handle.clone()), ..Default::default() }).collect::<alloc::vec::Vec<_>>().try_into().unwrap_or_default()),
            ..Default::default()
        };
        (0..cap::SCRIPT_OBJECTS)
            .map(|n| {
                let mut vars = strings.clone();
                if n == 0 {
                    vars.push(handles.clone());
                }
                skymp::ScriptObjectState { cls: name(), state: name(), flags: u8::MAX, vars: vars.try_into().unwrap_or_default() }
            })
            .collect::<alloc::vec::Vec<_>>()
            .try_into()
            .unwrap_or_default()
    }

    /// A prop's sample at its longest (thuum ADR-071 decision 6,
    /// SCHEMA_VERSION 46): every id and the sequence at their widest
    /// varints, every float carried, both flags and 64 bones.
    fn prop_sample_at_its_longest() -> skymp::PropTransform {
        let bone = skymp::PropBone { pos: [f32::MAX; 3], quat: [f32::MAX; 4] };
        skymp::PropTransform {
            refr: u32::MAX,
            epoch: u32::MAX,
            seq: u16::MAX,
            t_ms: u32::MAX,
            pos: [f32::MAX; 3],
            rot: [f32::MAX; 3],
            lin: [f32::MAX; 3],
            ang: [f32::MAX; 3],
            asleep: true,
            held: true,
            bones: (0..cap::PROP_BONES).map(|_| bone.clone()).collect::<alloc::vec::Vec<_>>().try_into().unwrap_or_default(),
            ..Default::default()
        }
    }

    /// A prop claim at its longest: 16 references at five-byte varints.
    fn prop_claim_at_its_longest() -> skymp::PropClaim {
        skymp::PropClaim {
            refrs: (0..cap::PROP_CLAIM_REFRS).map(|_| u32::MAX).collect::<alloc::vec::Vec<_>>().try_into().unwrap_or_default(),
            touch: u8::MAX,
            ..Default::default()
        }
    }

    #[test]
    fn a_prop_sample_and_claim_at_their_longest_fit_their_caps() {
        // thuum ADR-071 decision 6 (SCHEMA_VERSION 46): the tag, five-byte
        // refr, epoch and t_ms, a three-byte seq, four Vec3s, two flags, the
        // bones' length and 64 bones of 28 bytes; the claim's tag, its
        // length, 16 five-byte references and the touch
        let sample = Message::PropTransform(prop_sample_at_its_longest());
        assert_eq!(postcard::to_allocvec(&sample).map(|b| b.len()), Ok(1_862));
        assert_eq!(Message::max_len(sample.wire_id()), Some(2 * KIB));
        let claim = Message::PropClaim(prop_claim_at_its_longest());
        assert_eq!(postcard::to_allocvec(&claim).map(|b| b.len()), Ok(83));
        assert_eq!(Message::max_len(claim.wire_id()), Some(96));
        // a 65th bone and a 17th reference are no shape (the bounds at decode)
        let bones = postcard::to_allocvec(&alloc::vec![skymp::PropBone::default(); cap::PROP_BONES + 1]).unwrap_or_default();
        assert!(postcard::from_bytes::<bounded::Vec<skymp::PropBone, { cap::PROP_BONES }>>(&bones).is_err());
        let refrs = postcard::to_allocvec(&alloc::vec![1_u32; cap::PROP_CLAIM_REFRS + 1]).unwrap_or_default();
        assert!(postcard::from_bytes::<bounded::Vec<u32, { cap::PROP_CLAIM_REFRS }>>(&refrs).is_err());
    }

    /// A serve's graph variables at their longest (thuum D-C3 A): sixteen,
    /// each name at its cap, every payload carried, every number a
    /// five-byte varint.
    fn graph_vars_at_their_cap() -> Option<bounded::Vec<skymp::GraphVar, { cap::GRAPH_VARS }>> {
        let var = || skymp::GraphVar {
            name: "x".repeat(cap::ANIM_EVENT).try_into().unwrap_or_default(),
            k: u8::MAX,
            i: Some(i32::MIN),
            f: Some(f32::MAX),
            b: Some(true),
            seq: u32::MAX,
        };
        (0..cap::GRAPH_VARS).map(|_| var()).collect::<alloc::vec::Vec<_>>().try_into().ok()
    }

    /// A record's graph state at its cap (thuum docs/verbs/traps.md "The
    /// graph state"): GRAPH_STATE_HEX characters.
    fn graph_state_at_its_cap() -> Option<String<{ cap::GRAPH_STATE_HEX }>> {
        "f".repeat(cap::GRAPH_STATE_HEX).try_into().ok()
    }

    #[test]
    fn a_script_record_at_the_natives_answer_cap_fits_its_byte_cap() {
        let scripts = script_record_at_its_answer_cap();
        // the sample is whole, not a default left by a refused conversion
        assert_eq!(scripts.len(), cap::SCRIPT_OBJECTS);
        assert!(scripts.iter().all(|s| s.vars.len() >= 24));
        assert_eq!(scripts.first().and_then(|s| s.vars.last()).and_then(|v| v.a.as_ref()).map(|a| a.len()), Some(cap::SCRIPT_ARRAY));
        // with its graph state at its cap (docs/verbs/traps.md "The graph
        // state"), the whole string, not a default left by a refused one
        let graph = graph_state_at_its_cap();
        assert_eq!(graph.as_ref().map(|g| g.len()), Some(cap::GRAPH_STATE_HEX));
        let state = Message::ScriptState(skymp::ScriptState { epoch: u32::MAX, form_id: 0xfeff_ffff, fnv1a: u32::MAX, scripts: scripts.clone(), graph: graph.clone(), ..Default::default() });
        let bytes = postcard::to_allocvec(&state).unwrap_or_default();
        assert!(bytes.len() > 150 * KIB && bytes.len() <= 192 * KIB, "{}", bytes.len());
        // the serve with its graph variables at their cap too (thuum D-C3 A)
        let graph_vars = graph_vars_at_their_cap();
        assert_eq!(graph_vars.as_ref().map(|v| v.len()), Some(cap::GRAPH_VARS));
        let apply = Message::ScriptStateApply(skymp::ScriptStateApply {
            form_id: 0xfeff_ffff,
            fnv1a: u32::MAX,
            seq: u32::MAX,
            event: Some("x".repeat(cap::ANIM_EVENT).try_into().unwrap_or_default()),
            scripts,
            graph_vars,
            graph,
            ..Default::default()
        });
        let served = postcard::to_allocvec(&apply).unwrap_or_default();
        assert!(served.len() > bytes.len() && served.len() <= 256 * KIB, "{}", served.len());
    }

    /// A ScriptVariables at its caps (thuum ADR-068 rung 5): 256 watched
    /// keys and 256 entries, every name at PAPYRUS_NAME, every number a
    /// varint at its widest and every payload field carried.
    fn script_variables_at_their_caps() -> skymp::ScriptVariables {
        let name = || -> String<{ cap::PAPYRUS_NAME }> { "x".repeat(cap::PAPYRUS_NAME).try_into().unwrap_or_default() };
        let key = || skymp::ScriptVarKey { form: u32::MAX, script: name(), var: name(), own: true };
        let entry = || skymp::ScriptVarEntry { form: u32::MAX, script: name(), var: name(), own: true, k: u8::MAX, i: Some(i32::MIN), f: Some(f32::MAX), b: Some(true), seq: u32::MAX };
        skymp::ScriptVariables {
            watch: (0..cap::SCRIPT_VARIABLES).map(|_| key()).collect::<alloc::vec::Vec<_>>().try_into().unwrap_or_default(),
            entries: (0..cap::SCRIPT_VARIABLES).map(|_| entry()).collect::<alloc::vec::Vec<_>>().try_into().unwrap_or_default(),
            served: true,
            ..Default::default()
        }
    }

    #[test]
    fn a_script_variables_message_at_its_caps_fits_its_byte_cap() {
        let full = script_variables_at_their_caps();
        // the sample is whole, not a default left by a refused conversion
        assert_eq!((full.watch.len(), full.entries.len()), (cap::SCRIPT_VARIABLES, cap::SCRIPT_VARIABLES));
        let m = Message::ScriptVariables(full);
        let bytes = postcard::to_allocvec(&m).unwrap_or_default();
        assert_eq!(bytes.len(), 141_062);
        assert!(bytes.len() <= Message::max_len(m.wire_id()).unwrap_or(0));
        // a quest stage with every rung 5 field, the login's list and its
        // player class full, stays inside its 4 KiB (2 KiB held the list
        // alone; the class doubles it, thuum ADR-068 rung 5's fresh review)
        let full = || (0..cap::SHARED_QUESTS).map(|_| u32::MAX).collect::<alloc::vec::Vec<_>>().try_into().unwrap_or_default();
        let stage = Message::QuestStage(skymp::QuestStage {
            quest: u32::MAX,
            stage: u16::MAX,
            shared: full(),
            mode: u8::MAX,
            word: Some(u8::MAX),
            acting: Some(u32::MAX),
            served: Some(true),
            player: Some(full()),
            word_stages: Some((0..cap::WORD_STAGES).map(|_| u16::MAX).collect::<alloc::vec::Vec<_>>().try_into().unwrap_or_default()),
            // SCHEMA_VERSION 43 (D-Q4 C): a report's cause and its root at
            // the cap
            cause: Some(u8::MAX),
            root: Some(core::iter::repeat_n('_', cap::QUEST_ROOT).collect::<alloc::string::String>().as_str().try_into().unwrap_or_default()),
            ..Default::default()
        });
        if let Message::QuestStage(q) = &stage {
            assert_eq!((q.shared.len(), q.player.as_ref().map(|p| p.len()), q.word_stages.as_ref().map(|w| w.len())), (cap::SHARED_QUESTS, Some(cap::SHARED_QUESTS), Some(cap::WORD_STAGES)));
            assert_eq!(q.root.as_ref().map(|r| r.len()), Some(cap::QUEST_ROOT));
        }
        let bytes = postcard::to_allocvec(&stage).unwrap_or_default();
        assert!(bytes.len() > 2 * KIB && bytes.len() <= 4 * KIB, "{}", bytes.len());
        assert!(bytes.len() <= Message::max_len(stage.wire_id()).unwrap_or(0));
    }

    /// A reported native call at its caps (thuum ADR-068): eleven arguments,
    /// each a string at SNIPPET_STRING, every name at PAPYRUS_NAME, every
    /// number a five-byte varint, since SCHEMA_VERSION 44 (D-R1 C's leveled
    /// gap) 255 landings, each an item and a count at five varint bytes
    /// apiece, and since SCHEMA_VERSION 55 (the spell channel) a creation's
    /// result and every argument marked as a created reference.
    fn script_call_at_its_caps() -> skymp::ScriptCall {
        let name = || -> String<{ cap::PAPYRUS_NAME }> { "x".repeat(cap::PAPYRUS_NAME).try_into().unwrap_or_default() };
        skymp::ScriptCall {
            epoch: u32::MAX,
            self_id: 0xfeff_ffff,
            cls: name(),
            func: name(),
            args: (0..cap::SCRIPT_CALL_ARGS)
                .map(|_| Some(skymp::SnippetArg::String("x".repeat(cap::SNIPPET_STRING).try_into().unwrap_or_default())))
                .collect::<alloc::vec::Vec<_>>()
                .try_into()
                .unwrap_or_default(),
            caller: name(),
            handler: name(),
            latent: true,
            landed: (0..cap::SCRIPT_CALL_LANDED).map(|_| ItemDelta { item: FormId(u32::MAX), count: i32::MIN }).collect::<alloc::vec::Vec<_>>().try_into().ok(),
            placed: Some(u32::MAX),
            local_args: Some(u16::MAX),
            ..Default::default()
        }
    }

    #[test]
    fn a_script_call_at_its_caps_fits_its_byte_cap() {
        let full = script_call_at_its_caps();
        // the sample is whole, not a default left by a refused conversion
        assert_eq!((full.args.len(), full.landed.as_ref().map(|l| l.len())), (cap::SCRIPT_CALL_ARGS, Some(cap::SCRIPT_CALL_LANDED)));
        let m = Message::ScriptCall(full);
        let bytes = postcard::to_allocvec(&m).unwrap_or_default();
        // 180,812 before SCHEMA_VERSION 44, then the landings' presence byte,
        // their two-byte length and 255 of ten bytes (the table's comment),
        // and since SCHEMA_VERSION 55 `placed` (a presence byte and five)
        // and `local_args` (a presence byte and three)
        assert_eq!(bytes.len(), 183_375);
        assert!(bytes.len() <= Message::max_len(m.wire_id()).unwrap_or(0));
        // an absent landing costs its presence byte alone
        let plain = Message::ScriptCall(skymp::ScriptCall { landed: None, ..script_call_at_its_caps() });
        assert_eq!(postcard::to_allocvec(&plain).map(|b| b.len()), Ok(180_823));
        // a 256th landing fails at the bound, before any check
        let over: Result<bounded::Vec<ItemDelta, { cap::SCRIPT_CALL_LANDED }>, _> = (0..=cap::SCRIPT_CALL_LANDED).map(|_| ItemDelta { item: FormId(0xf), count: 1 }).collect::<alloc::vec::Vec<_>>().try_into();
        assert!(over.is_err());
    }

    // thuum docs/verbs/npc-puppet.md, Ownership: a host's state with its
    // running effects at the cap fits the row's 256 bytes; an absent set
    // costs its presence byte alone
    #[test]
    fn a_hosted_state_with_its_effects_at_the_cap_fits_its_byte_cap() {
        let widest = skymp::HostedActorState {
            idx: u32::MAX,
            health: f32::MAX,
            magicka: f32::MAX,
            stamina: f32::MAX,
            combat_target: u32::MAX,
            package: u32::MAX,
            gaze_target: u32::MAX,
            gaze_slot: u8::MAX,
            emotion_type: u8::MAX,
            emotion_value: u8::MAX,
            effects: (0..cap::MAGIC_EFFECTS).map(|_| 0xfeff_ffff).collect::<alloc::vec::Vec<_>>().try_into().ok(),
            ..Default::default()
        };
        assert_eq!(widest.effects.as_ref().map(|e| e.len()), Some(cap::MAGIC_EFFECTS));
        let m = Message::HostedActorState(widest.clone());
        let bytes = postcard::to_allocvec(&m).unwrap_or_default();
        assert_eq!(bytes.len(), 198);
        assert!(bytes.len() <= Message::max_len(m.wire_id()).unwrap_or(0));
        let plain = Message::HostedActorState(skymp::HostedActorState { effects: None, ..widest });
        assert_eq!(postcard::to_allocvec(&plain).map(|b| b.len()), Ok(37));
    }

    #[test]
    fn wire_id_matches_postcard_variant_index() {
        let samples = vec![
            Message::Refuse { reason: 7 },
            Message::CustomPacket(skymp::CustomPacket::default()),
            Message::UpdateMovement(skymp::UpdateMovement::default()),
            Message::DeathStateContainer(skymp::DeathStateContainer::default()),
            Message::CreateActor(skymp::CreateActor::default()),
            Message::SetGameTime(skymp::SetGameTime {
                year: u32::MAX,
                month: 11,
                day: 31,
                hour: 23.9,
                days_passed: 1.0e9,
                time_scale: 20.0,
                ..Default::default()
            }),
            Message::RestIntent(skymp::RestIntent { hours: 24.0, sleep: true, ..Default::default() }),
            Message::MapMarkerDiscovered(skymp::MapMarkerDiscovered { marker_type: 59, can_travel: true, ..Default::default() }),
            Message::IngredientEffectsKnown(skymp::IngredientEffectsKnown { ingredient: 0x0003_4cdd, mask: 0x0f, ..Default::default() }),
            Message::Favorites(skymp::Favorites {
                entries: (0..cap::FAVORITES)
                    .map(|i| skymp::FavoriteEntry { form: u32::MAX - u32::try_from(i).unwrap_or(0), hotkey: if i < 8 { i8::try_from(i).unwrap_or(-1) } else { -1 } })
                    .collect::<alloc::vec::Vec<_>>()
                    .try_into()
                    .unwrap_or_default(),
                ..Default::default()
            }),
            // a host's roster at its fullest: every id a five-byte varint,
            // every float carried (docs/verbs/npc-puppet.md)
            Message::ActorSightings(skymp::ActorSightings {
                sightings: (0..cap::ACTOR_SIGHTINGS)
                    .map(|_| skymp::ActorSighting { refr_id: 0xfeff_ffff, world_or_cell: u32::MAX, pos: [f32::MAX; 3], rot: [f32::MAX; 3] })
                    .collect::<alloc::vec::Vec<_>>()
                    .try_into()
                    .unwrap_or_default(),
                ..Default::default()
            }),
            // the fates of a full ring at their fullest: every id a five-byte
            // varint, every worn list full (docs/verbs/npc-fate.md)
            Message::ActorFate(skymp::ActorFate {
                fates: (0..cap::ACTOR_FATES)
                    .map(|_| skymp::ActorFateEntry {
                        refr_id: 0xfeff_ffff,
                        base_id: u32::MAX,
                        level: u16::MAX,
                        worn: (0..cap::FATE_WORN).map(|_| u32::MAX).collect::<alloc::vec::Vec<_>>().try_into().unwrap_or_default(),
                        inv: (0..cap::FATE_INV).map(|_| skymp::FateInvRow { base_id: u32::MAX, count: u32::MAX }).collect::<alloc::vec::Vec<_>>().try_into().unwrap_or_default(),
                    })
                    .collect::<alloc::vec::Vec<_>>()
                    .try_into()
                    .unwrap_or_default(),
                ..Default::default()
            }),
            Message::ActorDeath(skymp::ActorDeath { refr_id: 0xfeff_ffff, cause: u8::MAX, killer: u32::MAX, pos: [f32::MAX; 3], ..Default::default() }),
            Message::HostedActorState(skymp::HostedActorState {
                idx: u32::MAX,
                health: f32::MAX,
                magicka: f32::MAX,
                stamina: f32::MAX,
                combat_target: u32::MAX,
                package: u32::MAX,
                gaze_target: u32::MAX,
                gaze_slot: u8::MAX,
                emotion_type: u8::MAX,
                emotion_value: u8::MAX,
                // the host's running effects at their cap (docs/verbs/npc-puppet.md)
                effects: (0..cap::MAGIC_EFFECTS).map(|_| u32::MAX).collect::<alloc::vec::Vec<_>>().try_into().ok(),
                ..Default::default()
            }),
            Message::ActorSpeech(skymp::ActorSpeech { idx: u32::MAX, topic: u32::MAX, info: u32::MAX, ..Default::default() }),
            // a prop's sample, claim and owner word at their longest: every id
            // a five-byte varint, every float carried (thuum docs/verbs/
            // prop-physics.md, ADR-058)
            Message::PropTransform(prop_sample_at_its_longest()),
            Message::PropClaim(prop_claim_at_its_longest()),
            Message::PropOwner(skymp::PropOwner { refr: u32::MAX, player: u32::MAX, epoch: u32::MAX, ..Default::default() }),
            // a placement request with every base slot full and a report
            // with every float carried (thuum docs/verbs/spell-placed-object.md,
            // ADR-057 shape A)
            Message::PlacedRequest(skymp::PlacedRequest {
                set: u64::MAX,
                bases: (0..cap::PLACED_BASES).map(|_| u32::MAX).collect::<alloc::vec::Vec<_>>().try_into().unwrap_or_default(),
                window_ms: u32::MAX,
                ..Default::default()
            }),
            Message::PlacedReport(skymp::PlacedReport { set: u64::MAX, base: u32::MAX, pos: [f32::MAX; 3], yaw: f32::MAX, ..Default::default() }),
            // every skill used at once, each amount carried (thuum docs/verbs/leveling.md)
            Message::SkillUse(skymp::SkillUse {
                uses: (0..cap::SKILL_USES)
                    .map(|_| skymp::SkillUseEntry { skill: u8::MAX, amount: f32::MAX })
                    .collect::<alloc::vec::Vec<_>>()
                    .try_into()
                    .unwrap_or_default(),
                ..Default::default()
            }),
            // a lock report and a prompt at their longest (thuum docs/verbs/locks.md)
            Message::LockReport(skymp::LockReport { refr: u32::MAX, outcome: u8::MAX, broken: u8::MAX, locked: Some(true), level: Some(u8::MAX), ..Default::default() }),
            Message::LockPrompt(skymp::LockPrompt { refr: u32::MAX, ..Default::default() }),
            // a weather word at its longest (thuum docs/verbs/weather.md)
            Message::SetWeather(skymp::SetWeather { place: u32::MAX, weather: u32::MAX, transition: true, ..Default::default() }),
            // a perk pick and the fullest perk set (thuum docs/verbs/perks.md)
            Message::PerkPick(skymp::PerkPick { perk: u32::MAX, ..Default::default() }),
            // a word wall report (thuum docs/verbs/word-wall.md)
            Message::WordWall(skymp::WordWall { wall: u32::MAX, ..Default::default() }),
            // an arrest and a bounty at their longest
            Message::Arrest(skymp::Arrest { faction: u32::MAX, days: u32::MAX, ..Default::default() }),
            Message::CrimeGold(skymp::CrimeGold { faction: u32::MAX, violent: u32::MAX, non_violent: u32::MAX, ..Default::default() }),
            // a linked activation relayed (thuum docs/verbs/traps.md), and
            // since SCHEMA_VERSION 40 an activation and its relay with the
            // entry byte at their longest (thuum D-C2: 24 and 13 bytes)
            Message::RelayedActivation(skymp::RelayedActivation { target: u32::MAX, caster: u32::MAX, ..Default::default() }),
            Message::RelayedActivation(skymp::RelayedActivation { target: u32::MAX, caster: u32::MAX, entry: Some(u8::MAX), ..Default::default() }),
            Message::Activate(skymp::Activate {
                data: skymp::ActivateData { caster: u64::MAX, target: u64::MAX, is_second_activation: true, entry: Some(u8::MAX), ..Default::default() },
                ..Default::default()
            }),
            Message::ObjectAnimation(skymp::ObjectAnimation {
                refr: u32::MAX,
                event: "x".repeat(cap::ANIM_EVENT).try_into().unwrap_or_default(),
                ..Default::default()
            }),
            // a change-form record at its fullest: the body at its cap,
            // every remap row carried at five-byte varints, every scalar at
            // its widest (thuum docs/verbs/change-form.md, ADR-065)
            Message::ChangeForm(skymp::ChangeForm {
                epoch: u32::MAX,
                form_id: 0xfeff_ffff,
                form_type: u8::MAX,
                flags: u32::MAX,
                version: u8::MAX,
                body: "f".repeat(cap::CHANGE_FORM_BODY_HEX).try_into().unwrap_or_default(),
                fnv1a: u32::MAX,
                remap: (0..cap::CHANGE_FORM_REMAP)
                    .map(|_| skymp::ChangeFormRemap { offset: u32::MAX, form_id: u32::MAX, index: u32::MAX })
                    .collect::<alloc::vec::Vec<_>>()
                    .try_into()
                    .unwrap_or_default(),
                ..Default::default()
            }),
            Message::ChangeFormAck(skymp::ChangeFormAck { form_id: u32::MAX, fnv1a: u32::MAX, ..Default::default() }),
            Message::ChangeFormApply(skymp::ChangeFormApply {
                form_id: 0xfeff_ffff,
                form_type: u8::MAX,
                flags: u32::MAX,
                version: u8::MAX,
                body: "f".repeat(cap::CHANGE_FORM_BODY_HEX).try_into().unwrap_or_default(),
                remap: (0..cap::CHANGE_FORM_REMAP)
                    .map(|_| skymp::ChangeFormRemap { offset: u32::MAX, form_id: u32::MAX, index: u32::MAX })
                    .collect::<alloc::vec::Vec<_>>()
                    .try_into()
                    .unwrap_or_default(),
                ..Default::default()
            }),
            // a reported native call at its fullest: eleven arguments, each a
            // string at its cap, every name at its cap, every number a
            // five-byte varint (thuum ADR-068), and since SCHEMA_VERSION 44
            // its landings at their cap
            Message::ScriptCall(script_call_at_its_caps()),
            // a script record and its apply near the native's answer cap
            // (thuum ADR-068 rung 3), and a destruction report at its
            // longest: the reference a five-byte varint, the stage and both
            // flags (thuum D-S3)
            Message::ScriptState(skymp::ScriptState { epoch: u32::MAX, form_id: 0xfeff_ffff, fnv1a: u32::MAX, scripts: script_record_at_its_answer_cap(), graph: graph_state_at_its_cap(), ..Default::default() }),
            Message::ScriptStateApply(skymp::ScriptStateApply {
                form_id: 0xfeff_ffff,
                fnv1a: u32::MAX,
                seq: u32::MAX,
                event: Some("x".repeat(cap::ANIM_EVENT).try_into().unwrap_or_default()),
                scripts: script_record_at_its_answer_cap(),
                graph_vars: graph_vars_at_their_cap(),
                graph: graph_state_at_its_cap(),
                ..Default::default()
            }),
            Message::DestructionStage(skymp::DestructionStage {
                refr: u32::MAX,
                stage: u8::MAX,
                destroyed: true,
                served: true,
                disabled: Some(true),
                deleted: Some(true),
                seq: u32::MAX,
                ..Default::default()
            }),
            // an alias fill and a world global report at their longest
            // (thuum ADR-068 rung 4)
            Message::AliasFill(skymp::AliasFill { quest: u32::MAX, alias: u32::MAX, refr: u32::MAX, served: true, ..Default::default() }),
            Message::WorldGlobal(skymp::WorldGlobal {
                epoch: u32::MAX,
                stream: Some(true),
                entries: (0..cap::WORLD_GLOBALS)
                    .map(|_| skymp::WorldGlobalEntry { global: u32::MAX, value: f32::MAX })
                    .collect::<alloc::vec::Vec<_>>()
                    .try_into()
                    .unwrap_or_default(),
                served: true,
                ..Default::default()
            }),
            // the condition-read variables at their caps and a stage word at
            // its longest (thuum ADR-068 rung 5)
            Message::ScriptVariables(script_variables_at_their_caps()),
            // the fullest own head parts report (thuum D-53-8 B)
            Message::OwnHeadParts(own_head_parts_at_their_caps()),
            Message::QuestStage(skymp::QuestStage { quest: u32::MAX, stage: u16::MAX, word: Some(u8::MAX), acting: Some(u32::MAX), served: Some(true), word_stages: Some(core::iter::repeat_n(u16::MAX, cap::WORD_STAGES).collect::<alloc::vec::Vec<u16>>().try_into().unwrap_or_default()), cause: Some(u8::MAX), root: Some("QF_dunEmbershardQST_000B6FEF.Fragment_16".try_into().unwrap_or_default()), ..Default::default() }),
            // and its relay, the sequence carried (thuum ADR-068)
            Message::SpSnippet(skymp::SpSnippet {
                class: "ObjectReference".try_into().unwrap_or_default(),
                function: "PlayAnimation".try_into().unwrap_or_default(),
                self_id: 0xfeff_ffff,
                snippet_idx: i64::from(u32::MAX),
                seq: Some(u32::MAX),
                keep: Some(u8::MAX),
                ..Default::default()
            }),
            Message::PerkState(skymp::PerkState {
                perks: core::iter::repeat_n(u32::MAX, cap::PERKS).collect::<alloc::vec::Vec<u32>>().try_into().unwrap_or_default(),
                points: u16::MAX,
                ..Default::default()
            }),
            Message::RaceMenuPreset(skymp::RaceMenuPreset {
                actor: u32::MAX,
                preset: "x".repeat(cap::RACEMENU_PRESET).try_into().unwrap_or_default(),
                ..Default::default()
            }),
            Message::ActorValues(skymp::ActorValues {
                bases: (0..cap::ACTOR_VALUES)
                    .map(|i| skymp::ActorValueBase { av: u8::try_from(i).unwrap_or(u8::MAX), base: f32::MAX })
                    .collect::<alloc::vec::Vec<_>>()
                    .try_into()
                    .unwrap_or_default(),
                skills: (0..cap::SKILLS)
                    .map(|i| skymp::SkillProgress { skill: u8::try_from(i).unwrap_or(u8::MAX), level: f32::MAX, xp: f32::MAX, threshold: f32::MAX })
                    .collect::<alloc::vec::Vec<_>>()
                    .try_into()
                    .unwrap_or_default(),
                xp: f32::MAX,
                threshold: f32::MAX,
                level: u16::MAX,
                legendary: (0..cap::SKILLS)
                    .map(|i| skymp::LegendarySkill { skill: u8::try_from(i).unwrap_or(u8::MAX), count: u16::MAX })
                    .collect::<alloc::vec::Vec<_>>()
                    .try_into()
                    .unwrap_or_default(),
                ..Default::default()
            }),
            Message::ConsoleOutput(skymp::ConsoleOutput {
                text: "x".repeat(cap::CONSOLE_OUTPUT).try_into().unwrap_or_default(),
                refused: true,
                ..Default::default()
            }),
            // a shot and its relay at their longest: every id a five-byte
            // varint, the launch carried
            Message::PlayerBowShot(skymp::PlayerBowShot {
                idx: u32::MAX,
                weapon_id: u32::MAX,
                ammo_id: u32::MAX,
                power: 1.0,
                is_sun_gazing: true,
                aim_angle: f32::MAX,
                aim_heading: f32::MAX,
                has_launch: true,
                origin_x: f32::MAX,
                origin_y: f32::MAX,
                origin_z: f32::MAX,
                launch_angle_x: f32::MAX,
                launch_angle_z: f32::MAX,
                ..Default::default()
            }),
            Message::ArrowShot(skymp::ArrowShot {
                idx: u32::MAX,
                weapon_id: u32::MAX,
                ammo_id: u32::MAX,
                power: 1.0,
                aim_angle: f32::MAX,
                aim_heading: f32::MAX,
                has_launch: true,
                origin_x: f32::MAX,
                origin_y: f32::MAX,
                origin_z: f32::MAX,
                launch_angle_x: f32::MAX,
                launch_angle_z: f32::MAX,
                ..Default::default()
            }),
            // a cast at its longest: every id and the hand a five-byte
            // varint, the graph's variables full, the launch carried
            Message::SpellCast(skymp::SpellCast {
                data: skymp::SpellCastData {
                    caster: u32::MAX,
                    target: u32::MAX,
                    spell: u32::MAX,
                    is_dual_casting: true,
                    interrupt_cast: true,
                    casting_source: i32::MIN,
                    aim_angle: f32::MAX,
                    aim_heading: f32::MAX,
                    has_launch: true,
                    origin_x: f32::MAX,
                    origin_y: f32::MAX,
                    origin_z: f32::MAX,
                    launch_angle_x: f32::MAX,
                    launch_angle_z: f32::MAX,
                    actor_animation_variables: skymp::ActorAnimationVariables {
                        booleans: alloc::vec![u8::MAX; cap::ANIM_VARS].try_into().unwrap_or_default(),
                        floats: alloc::vec![u8::MAX; cap::ANIM_VARS].try_into().unwrap_or_default(),
                        integers: alloc::vec![u8::MAX; cap::ANIM_VARS].try_into().unwrap_or_default(),
                    },
                    decal: Some(skymp::SpellDecal { normal_x: f32::MAX, normal_y: f32::MAX, normal_z: f32::MAX, material_id: u32::MAX }),
                    created_spell: Some(created_form_at_its_caps()),
                },
                ..Default::default()
            }),
        ];
        for m in samples {
            let bytes = postcard::to_allocvec(&m).unwrap_or_default();
            // the first byte is the variant index (a varint; every id fits in one byte)
            assert_eq!(bytes.first().copied().map(u32::from), Some(m.wire_id()), "{}", m.name());
            assert!(bytes.len() <= Message::max_len(m.wire_id()).unwrap_or(0), "{}", m.name());
        }
    }

    /// An own head parts report at its caps (thuum D-53-8 B): the widest race
    /// and 64 parts at their widest varints, the overlay flag set.
    fn own_head_parts_at_their_caps() -> skymp::OwnHeadParts {
        skymp::OwnHeadParts {
            race: u32::MAX,
            parts: core::iter::repeat_n(u32::MAX, cap::HEADPARTS).collect::<alloc::vec::Vec<u32>>().try_into().unwrap_or_default(),
            overlay: true,
            ..Default::default()
        }
    }

    #[test]
    fn own_head_parts_at_their_caps_fit_their_byte_cap() {
        // thuum D-53-8 B (SCHEMA_VERSION 50): the wire id, a five-byte race,
        // the count's one byte, 64 five-byte parts and the flag; Ariadne's
        // seven-part overlay list of train 53, every id a three-byte varint,
        // costs 27
        let full = Message::OwnHeadParts(own_head_parts_at_their_caps());
        assert_eq!(postcard::to_allocvec(&full).map(|b| b.len()), Ok(328));
        assert_eq!(Message::max_len(full.wire_id()), Some(512));
        let ariadne = Message::OwnHeadParts(skymp::OwnHeadParts {
            race: 0x88845,
            parts: alloc::vec![0x51612, 0x511a8, 0xee87f, 0xd82f6, 0x85de9, 0x85ddf, 0xf3916].try_into().unwrap_or_default(),
            overlay: true,
            ..Default::default()
        });
        assert_eq!(postcard::to_allocvec(&ariadne).map(|b| b.len()), Ok(27));
        let over: Result<bounded::Vec<u32, { cap::HEADPARTS }>, _> = core::iter::repeat_n(1u32, cap::HEADPARTS + 1).collect::<alloc::vec::Vec<u32>>().try_into();
        assert!(over.is_err());
    }

    #[test]
    fn summon_at_its_longest_fits_its_byte_cap() {
        // thuum tracker #84 (SCHEMA_VERSION 57): the wire id, the local id,
        // the base, the effect and the spell at five-byte varints, the two
        // triple floats, the cell at a five-byte varint, the time and the
        // flag; since 58 (tracker #106) the caster at a five-byte varint
        // last; the baseline's report (x-summon-baseline 20261116-083744:
        // rotfern's potent atronach, its caster 0 at one byte) costs 46
        let full = Message::Summon(skymp::Summon {
            local_id: u32::MAX,
            base_id: u32::MAX,
            effect_id: u32::MAX,
            spell_id: u32::MAX,
            pos: [f32::MAX; 3],
            rot: [f32::MAX; 3],
            world_or_cell: u32::MAX,
            remaining_s: f32::MAX,
            ended: true,
            caster_id: u32::MAX,
            ..Default::default()
        });
        assert_eq!(postcard::to_allocvec(&full).map(|b| b.len()), Ok(60));
        assert_eq!(Message::max_len(full.wire_id()), Some(64));
        let baseline = Message::Summon(skymp::Summon {
            local_id: 0xff00_08fd,
            base_id: 0x4e940,
            effect_id: 0x4e945,
            spell_id: 0x100e76,
            pos: [8849.0, -58073.0, 1446.0],
            rot: [0.0, 0.0, 2.46],
            world_or_cell: 0x3c,
            remaining_s: 59.5,
            ..Default::default()
        });
        assert_eq!(postcard::to_allocvec(&baseline).map(|b| b.len()), Ok(46));
    }

    /// A created form at its caps: every effect's MGEF, area and duration at
    /// five-byte varints, both floats carried.
    fn created_form_at_its_caps() -> skymp::CreatedForm {
        skymp::CreatedForm {
            kind: skymp::CREATED_POISON,
            effects: core::iter::repeat_n(
                skymp::CreatedEffect { mgef: u32::MAX, magnitude: f32::MAX, area: u32::MAX, duration: u32::MAX, cost: f32::MAX },
                cap::CREATED_EFFECTS,
            )
            .collect::<alloc::vec::Vec<_>>()
            .try_into()
            .unwrap_or_default(),
        }
    }

    /// A stack at its longest: every id a five-byte varint, every extra
    /// carried, the name at its cap, the three created forms at theirs and
    /// the owner.
    fn entry_at_its_caps() -> skymp::Entry {
        skymp::Entry {
            base_id: u32::MAX,
            count: u32::MAX,
            health: Some(f32::MAX),
            enchantment_id: Some(u32::MAX),
            max_charge: Some(f32::MAX),
            remove_enchantment_on_unequip: Some(true),
            charge_percent: Some(f32::MAX),
            name: Some("x".repeat(cap::NAME).try_into().unwrap_or_default()),
            soul: Some(u8::MAX),
            poison_id: Some(u32::MAX),
            poison_count: Some(u32::MAX),
            worn: Some(true),
            worn_left: Some(true),
            created_base: Some(created_form_at_its_caps()),
            created_enchantment: Some(created_form_at_its_caps()),
            created_poison: Some(created_form_at_its_caps()),
            owner_id: Some(u32::MAX),
        }
    }

    #[test]
    fn a_stack_with_its_created_forms_fits_its_messages_byte_caps() {
        // a created form at its caps is its kind, the count's byte and eight
        // effects of 23 bytes; a stack carries three behind their presence
        // bytes, so a put or a take at its longest is the wire id, the three
        // ids, the extras with the name at its cap, the three forms and the
        // owner (SCHEMA_VERSION 65)
        let form = created_form_at_its_caps();
        assert_eq!(postcard::to_allocvec(&form).map(|b| b.len()), Ok(186));
        let e = entry_at_its_caps();
        assert_eq!(postcard::to_allocvec(&e).map(|b| b.len()), Ok(877));
        let put = Message::PutItem(skymp::PutItem {
            base_id: u32::MAX,
            count: u32::MAX,
            target: u32::MAX,
            health: e.health,
            enchantment_id: e.enchantment_id,
            max_charge: e.max_charge,
            remove_enchantment_on_unequip: e.remove_enchantment_on_unequip,
            charge_percent: e.charge_percent,
            name: e.name.clone(),
            soul: e.soul,
            poison_id: e.poison_id,
            poison_count: e.poison_count,
            worn: e.worn,
            worn_left: e.worn_left,
            created_base: e.created_base.clone(),
            created_enchantment: e.created_enchantment.clone(),
            created_poison: e.created_poison.clone(),
            owner_id: e.owner_id,
            ..Default::default()
        });
        assert_eq!(postcard::to_allocvec(&put).map(|b| b.len()), Ok(883));
        assert_eq!(Message::max_len(put.wire_id()), Some(KIB));
        // a drop at its longest, a put's fields with a u64 base and no target
        // or worn flags (SCHEMA_VERSION 63), and the owner (66)
        let drop = Message::DropItem(skymp::DropItem {
            base_id: u64::MAX,
            count: u32::MAX,
            health: e.health,
            enchantment_id: e.enchantment_id,
            max_charge: e.max_charge,
            remove_enchantment_on_unequip: e.remove_enchantment_on_unequip,
            charge_percent: e.charge_percent,
            name: e.name.clone(),
            soul: e.soul,
            poison_id: e.poison_id,
            poison_count: e.poison_count,
            created_base: e.created_base.clone(),
            created_enchantment: e.created_enchantment.clone(),
            created_poison: e.created_poison.clone(),
            owner_id: e.owner_id,
            ..Default::default()
        });
        assert_eq!(postcard::to_allocvec(&drop).map(|b| b.len()), Ok(879));
        assert_eq!(Message::max_len(drop.wire_id()), Some(KIB));
        let take = Message::TakeItem(skymp::TakeItem {
            base_id: u32::MAX,
            count: u32::MAX,
            target: u32::MAX,
            name: e.name.clone(),
            health: e.health,
            enchantment_id: e.enchantment_id,
            max_charge: e.max_charge,
            remove_enchantment_on_unequip: e.remove_enchantment_on_unequip,
            charge_percent: e.charge_percent,
            soul: e.soul,
            poison_id: e.poison_id,
            poison_count: e.poison_count,
            worn: e.worn,
            worn_left: e.worn_left,
            created_base: e.created_base.clone(),
            created_enchantment: e.created_enchantment.clone(),
            created_poison: e.created_poison.clone(),
            owner_id: e.owner_id,
            ..Default::default()
        });
        assert_eq!(postcard::to_allocvec(&take).map(|b| b.len()), Ok(883));
        assert_eq!(Message::max_len(take.wire_id()), Some(KIB));
        let drink = Message::OnEquip(skymp::OnEquip { base_id: u32::MAX, created_base: Some(form), ..Default::default() });
        assert_eq!(postcard::to_allocvec(&drink).map(|b| b.len()), Ok(193));
        assert_eq!(Message::max_len(drink.wire_id()), Some(256));
        // a plain stack costs the three forms' presence bytes and the
        // owner's alone, and a plain equip report one
        let plain = skymp::Entry { base_id: 0x12eb7, count: 1, ..Default::default() };
        assert_eq!(postcard::to_allocvec(&plain).map(|b| b.len()), Ok(19));
        let equip = Message::OnEquip(skymp::OnEquip { base_id: 0x3eadd, ..Default::default() });
        assert_eq!(postcard::to_allocvec(&equip).map(|b| b.len()), Ok(5));
        let over: Result<bounded::Vec<skymp::CreatedEffect, { cap::CREATED_EFFECTS }>, _> =
            core::iter::repeat_n(skymp::CreatedEffect::default(), cap::CREATED_EFFECTS + 1).collect::<alloc::vec::Vec<_>>().try_into();
        assert!(over.is_err());
    }

    #[test]
    fn a_craft_report_at_its_caps_fits_its_byte_cap() {
        // the wire id, a five-byte bench and (since SCHEMA_VERSION 62) a
        // five-byte seq, 64 stacks lost and 64 gained at their longest and
        // 128 learned enchantments at five-byte varints
        let full = Message::CraftReport(skymp::CraftReport {
            workbench: u32::MAX,
            seq: u32::MAX,
            lost: core::iter::repeat_n(entry_at_its_caps(), cap::CRAFT_ROWS).collect::<alloc::vec::Vec<_>>().try_into().unwrap_or_default(),
            gained: core::iter::repeat_n(entry_at_its_caps(), cap::CRAFT_ROWS).collect::<alloc::vec::Vec<_>>().try_into().unwrap_or_default(),
            learned_enchantments: core::iter::repeat_n(u32::MAX, cap::CRAFT_LEARNED).collect::<alloc::vec::Vec<u32>>().try_into().unwrap_or_default(),
            ..Default::default()
        });
        let full_len = postcard::to_allocvec(&full).map(|b| b.len());
        assert_eq!(full_len, Ok(112_911));
        assert!(full_len.unwrap_or(usize::MAX) <= Message::max_len(full.wire_id()).unwrap_or(0));
        assert_eq!(Message::max_len(full.wire_id()), Some(112 * KIB));
        assert_eq!(full.name(), "CraftReport");
        assert_eq!(full.msg_type(), Some(84));
        assert_eq!(full.direction(), Direction::ClientToServer);
        // a brew of two ingredients into one potion: the bench at a
        // three-byte varint, two plain stacks lost, one created stack gained
        let brew = Message::CraftReport(skymp::CraftReport {
            workbench: 0x000b_e9a1,
            seq: 1,
            lost: alloc::vec![
                skymp::Entry { base_id: 0x0003_4cdd, count: 1, ..Default::default() },
                skymp::Entry { base_id: 0x0006_bc02, count: 1, ..Default::default() },
            ]
            .try_into()
            .unwrap_or_default(),
            gained: alloc::vec![skymp::Entry {
                base_id: 0x0003_eadd,
                count: 1,
                created_base: Some(skymp::CreatedForm {
                    kind: skymp::CREATED_POTION,
                    effects: alloc::vec![skymp::CreatedEffect { mgef: 0x0003_eb15, magnitude: 25.0, area: 0, duration: 0, cost: 12.5 }].try_into().unwrap_or_default(),
                }),
                ..Default::default()
            }]
            .try_into()
            .unwrap_or_default(),
            ..Default::default()
        });
        assert!(postcard::to_allocvec(&brew).map(|b| b.len()).unwrap_or(usize::MAX) < 128);
        // the answer's ack costs its presence byte and the seq (62)
        let answer = Message::SetInventory(skymp::SetInventory { craft_ack: Some(u32::MAX), ..Default::default() });
        assert_eq!(postcard::to_allocvec(&answer).map(|b| b.len()), Ok(8));
        let plain = Message::SetInventory(skymp::SetInventory::default());
        assert_eq!(postcard::to_allocvec(&plain).map(|b| b.len()), Ok(3));
    }

    #[test]
    fn an_engine_lock_write_fits_its_byte_cap() {
        // thuum ADR-073 step 3: the wire id, a five-byte refr, the outcome,
        // the broken count, then `locked` and `level` behind their presence
        // bytes; a pick's report as canon sent it costs the two presence
        // bytes alone
        let write = Message::LockReport(skymp::LockReport {
            refr: u32::MAX,
            outcome: skymp::LOCK_ENGINE_WRITE,
            locked: Some(true),
            level: Some(u8::MAX),
            ..Default::default()
        });
        assert_eq!(postcard::to_allocvec(&write).map(|b| b.len()), Ok(12));
        assert_eq!(Message::max_len(write.wire_id()), Some(16));
        let pick = Message::LockReport(skymp::LockReport { refr: u32::MAX, ..Default::default() });
        assert_eq!(postcard::to_allocvec(&pick).map(|b| b.len()), Ok(10));
    }

    #[test]
    fn an_actor_claim_at_its_caps_fits_its_byte_cap() {
        // thuum ADR-073 step 5a (SCHEMA_VERSION 47; the why since 49): the
        // wire id, a five-byte idx, the flag, then the purpose's two bytes,
        // three five-byte ids and the release's why (D-53-5 B), each behind
        // its presence byte; a
        // talk as canon sent it costs the five presence bytes alone
        let full = Message::ActorClaim(skymp::ActorClaim {
            idx: u32::MAX,
            claim: true,
            purpose: Some(u8::MAX),
            quest: Some(u32::MAX),
            package: Some(u32::MAX),
            scene: Some(u32::MAX),
            why: Some(u8::MAX),
            ..Default::default()
        });
        assert_eq!(postcard::to_allocvec(&full).map(|b| b.len()), Ok(29));
        assert_eq!(Message::max_len(full.wire_id()), Some(48));
        let talk = Message::ActorClaim(skymp::ActorClaim { idx: u32::MAX, claim: true, ..Default::default() });
        assert_eq!(postcard::to_allocvec(&talk).map(|b| b.len()), Ok(12));
    }

    #[test]
    fn an_entry_rides_inside_its_messages_byte_caps() {
        // thuum D-C2 (SCHEMA_VERSION 40): the relay's longest entry is its
        // wire id, two five-byte varints and the entry's presence and byte,
        // and since SCHEMA_VERSION 60 (#117) the absent inventory's presence
        // byte; the report's two ten-byte varints, the flag and the same two.
        // The relay's cap is SetInventory's, for the presser's inventory
        let relay = Message::RelayedActivation(skymp::RelayedActivation { target: u32::MAX, caster: u32::MAX, entry: Some(skymp::ENTRY_LEAVE), ..Default::default() });
        assert_eq!(postcard::to_allocvec(&relay).map(|b| b.len()), Ok(14));
        assert_eq!(Message::max_len(relay.wire_id()), Some(2 * MIB));
        let report = Message::Activate(skymp::Activate {
            data: skymp::ActivateData { caster: u64::MAX, target: u64::MAX, entry: Some(skymp::ENTRY_ENTER), ..Default::default() },
            ..Default::default()
        });
        assert_eq!(postcard::to_allocvec(&report).map(|b| b.len()), Ok(24));
        assert_eq!(Message::max_len(report.wire_id()), Some(64));
        // an absent entry and an absent inventory cost their presence bytes
        // alone
        let plain = Message::RelayedActivation(skymp::RelayedActivation { target: u32::MAX, caster: u32::MAX, ..Default::default() });
        assert_eq!(postcard::to_allocvec(&plain).map(|b| b.len()), Ok(13));
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
