//! The game rules for the C++ core (thuum ADR-020): the core gathers the
//! facts from its world model and asks; wire-rules decides. Plain values both
//! ways; the movement budgets and the game clock (ADR-021) live here.

use wire_rules::{activation, actor_values, alias_fills, appearance, attributes, block, bonds, casts, cells, clock, console, damage, destruction, effects, favorites, forms, headshots, hosted_state, hostility, leveling, locks, magic, markers, melee, movement, natives, object_animation, placements, player_globals, player_own, props, quest_runs, quest_scope, quest_stages, racemenu, ranged, rest, perks, ref_state, script_state, script_variables, scripted_effects, shouts, stagger, standing, teleports, trigger_entry, weather, world_globals, world_streamer};

// the perk table's effect calls carry a CTDA's five parts flat, as add_condition does
#[allow(clippy::too_many_arguments)]
#[cxx::bridge(namespace = "skymp::rules")]
mod ffi {
    /// A decision and the bound it was made against (units), for the log.
    #[derive(Debug)]
    struct Verdict {
        /// The rule lets the action through.
        allowed: bool,
        /// The limit, units.
        bound: f32,
    }

    /// What the core knows about the race a client chose in the race menu.
    #[derive(Debug)]
    struct RaceFacts {
        /// The core has game files to judge by.
        has_game_files: bool,
        /// The race is the one the core records for the actor.
        is_recorded_race: bool,
        /// The form is a RACE with the Playable flag.
        is_playable_race: bool,
        /// The race is on the core's restricted list (`restrictedRaces`).
        is_restricted: bool,
        /// The actor's profile is on the race's permit list.
        is_permitted: bool,
    }

    /// What the core knows about a player's melee hit on another player.
    #[derive(Debug)]
    struct MeleeFacts {
        /// Center to center, units.
        distance: f32,
        /// fCombatDistance.
        combat_distance: f32,
        /// fCombatBashReach.
        bash_reach: f32,
        /// The longest DNAM reach among the worn weapons, 0 for none.
        weapon_reach: f32,
        /// The attacker race's unarmed reach.
        unarmed_reach: f32,
        /// The attacker's race height for its sex.
        aggressor_scale: f32,
        /// The target's race height for its sex.
        target_scale: f32,
    }

    /// What the core knows about a simulator's sample of a physics prop
    /// (wire-rules props, thuum docs/verbs/prop-physics.md).
    #[derive(Debug)]
    struct PropSampleFacts {
        /// The prop's reference id.
        refr: u32,
        /// The sender's actor id.
        player: u32,
        /// The cell the sample's position lands in, 0 for none.
        cell: u32,
        /// The host of the cell the RECORD stands in, 0 while nobody.
        cell_host: u32,
        /// The body's reported state.
        asleep: bool,
        /// Units between the sample and the record.
        moved: f32,
        /// The server's wall clock, ms.
        now_ms: u64,
    }

    /// What the core knows about the direction of a player's melee hit on a
    /// player (thuum docs/verbs/hit-cone.md).
    #[derive(Debug)]
    struct ConeFacts {
        /// The attacker's heading as the core records it, degrees.
        heading: f32,
        /// Target minus attacker, east, units.
        dx: f32,
        /// Target minus attacker, north, units.
        dy: f32,
        /// The widest ATKD strike angle of the attacker's race, 0 for none.
        widest_strike_angle: f32,
        /// The hit is a power attack, as the core kept the flag.
        power: bool,
        /// The target is dead.
        target_dead: bool,
    }

    /// What the core knows about a player's hit flags.
    #[derive(Debug)]
    struct FlagFacts {
        /// The client says power attack.
        claims_power: bool,
        /// The client says sneak attack.
        claims_sneak: bool,
        /// Whether the attacker ever started a power attack.
        saw_power_start: bool,
        /// Milliseconds since that start (0 when there was none).
        since_power_start_ms: u64,
        /// The core holds the attacker sneaking.
        is_sneaking: bool,
        /// The client says bash.
        claims_bash: bool,
        /// Whether the attacker ever started a bash.
        saw_bash_start: bool,
        /// Milliseconds since that start (0 when there was none).
        since_bash_start_ms: u64,
    }

    /// thuum docs/verbs/blocking.md: the game's block settings (Skyrim.esm's
    /// GMSTs as the core read them, the executable's default where the
    /// masters set none).
    #[derive(Debug)]
    struct BlockSettings {
        /// fShieldBaseFactor.
        shield_base: f32,
        /// fShieldScalingFactor, percent per armor point.
        shield_scaling: f32,
        /// fBlockWeaponBase.
        weapon_base: f32,
        /// fBlockWeaponScaling, percent per point of weapon damage.
        weapon_scaling: f32,
        /// fArmorRatingBase.
        rating_base: f32,
        /// fArmorRatingMax.
        rating_max: f32,
        /// fBlockPowerAttackMult.
        power_attack_mult: f32,
        /// fPlayerMaxResistance over 100.
        max_share: f32,
    }

    /// What the server knows about a hit on a target that may be blocking.
    #[derive(Debug)]
    struct BlockFacts {
        /// The target's recorded block is up.
        blocking: bool,
        /// The target's heading, degrees.
        heading: f32,
        /// The attacker's offset from the target, x.
        dx: f32,
        /// The attacker's offset from the target, y.
        dy: f32,
        /// An arrow or a bolt.
        ranged: bool,
        /// The target holds a shield.
        shield: bool,
        /// The shield record's armor rating.
        shield_rating: f32,
        /// The target's ShieldPerks (actor value 66).
        shield_perks: f32,
        /// The attacker's weapon's base damage.
        weapon_damage: f32,
        /// The target's Block skill.
        block_skill: f32,
        /// A power attack, as damage-flags backed it.
        power: bool,
    }

    /// thuum docs/verbs/stagger.md: the game's stagger settings (Skyrim.esm's
    /// GMSTs as the core read them, the executable's defaults where the
    /// masters set none), and the server's own playerAlwaysStaggered.
    #[derive(Debug)]
    struct StaggerSettings {
        /// fStaggerMassMult.
        mass_mult: f32,
        /// fStaggerMassBase.
        mass_base: f32,
        /// fStaggerMassOffsetBase.
        mass_offset_base: f32,
        /// fStaggerMassOffsetMult.
        mass_offset_mult: f32,
        /// fStaggerPlayerMassMult.
        player_mass_mult: f32,
        /// fStaggerAttackBase (a bash's).
        bash_base: f32,
        /// fStaggerAttackMult (a bash's, per point of Block).
        bash_mult: f32,
        /// fStaggerShieldMult.
        shield_mult: f32,
        /// fStagger1WeapMult.
        one_hand_mult: f32,
        /// fStagger2WeapMult.
        two_hand_mult: f32,
        /// fStaggerRunningMult.
        running_mult: f32,
        /// fStaggerRecoilingMult.
        recoiling_mult: f32,
        /// fStaggerBlockingMult.
        blocking_mult: f32,
        /// fStaggerBlockBase.
        block_base: f32,
        /// fStaggerBlockMult.
        block_mult: f32,
        /// fStaggerBlockAttackShieldBase.
        block_shield_base: f32,
        /// fStaggerBlockAttackShieldMult.
        block_shield_mult: f32,
        /// fStaggerMin.
        min: f32,
        /// iStaggerAttackChance, percent.
        attack_chance: u8,
        /// bPlayerAlwaysStaggered:Combat, the server's.
        player_always_staggered: bool,
    }

    /// What the server knows about a hit that may stagger its victim.
    #[derive(Debug)]
    struct StaggerFacts {
        /// A bash, as damage-flags backed it.
        bash: bool,
        /// The attacker's weapon's stagger (1 without a weapon).
        weapon_stagger: f32,
        /// The attack's staggerOffset (its race's ATKD).
        stagger_offset: f32,
        /// A bow's or a crossbow's shot.
        ranged: bool,
        /// The attacker's BowStaggerBonus (actor value 87).
        bow_stagger_bonus: f32,
        /// The attacker's Block.
        attacker_block: f32,
        /// The attacker bashes with a shield.
        attacker_shield: bool,
        /// Its weapon is two-handed, a bow or a crossbow.
        attacker_two_handed: bool,
        /// It bashes with nothing in hand.
        attacker_unarmed: bool,
        /// The victim's Mass (actor value 36).
        victim_mass: f32,
        /// The victim is a player.
        victim_is_player: bool,
        /// The victim runs.
        victim_running: bool,
        /// The victim recoils.
        victim_recoiling: bool,
        /// The victim's recorded block is up.
        victim_blocking: bool,
        /// The victim's Block.
        victim_block: f32,
        /// Its shield's rating, or its drawn weapon's type value, or 0.
        victim_block_rating: f32,
        /// The hit was blocked (blocking's verdict).
        blocked: bool,
        /// The hit dealt damage.
        dealt_damage: bool,
    }

    /// The game's base sneak attack multipliers by weapon type (Skyrim.esm's
    /// fCombatSneak*Mult settings) as the core read them; 0 for one it could
    /// not read.
    #[derive(Debug)]
    struct SneakMults {
        /// fCombatSneakHandMult.
        hand: f32,
        /// fCombatSneak1HSwordMult.
        one_hand_sword: f32,
        /// fCombatSneak1HDaggerMult.
        one_hand_dagger: f32,
        /// fCombatSneak1HAxeMult.
        one_hand_axe: f32,
        /// fCombatSneak1HMaceMult.
        one_hand_mace: f32,
        /// fCombatSneak2HSwordMult.
        two_hand_sword: f32,
        /// fCombatSneak2HAxeMult.
        two_hand_axe: f32,
        /// The ranged weapons' multiplier the server setting sneakRangedMult
        /// gives (no game setting exists; 0 keeps SkyMP's 1.3), train 52.
        ranged: f32,
    }

    /// What the core knows about a player's last bed (thuum
    /// docs/verbs/sleep.md).
    #[derive(Debug)]
    struct BedFacts {
        /// The player activated a bed since its last rest.
        has_bed: bool,
        /// Milliseconds since that activation (0 without one).
        since_bed_activation_ms: u64,
        /// The player's distance to that bed, in units.
        distance_to_bed: f32,
    }

    /// What the core knows about a hit it accepted, for hostility sync
    /// (thuum docs/verbs/hostility-sync.md).
    #[derive(Debug)]
    struct HostilityFacts {
        /// A user plays the attacker.
        aggressor_is_player: bool,
        /// A host's game simulates the attacker: an NPC in a hosted cell that
        /// no user plays (thuum docs/verbs/npc-stream.md, H-53h).
        aggressor_is_hosted: bool,
        /// A user plays the victim.
        target_is_player: bool,
        /// A host's game simulates the victim.
        target_is_hosted: bool,
        /// The attacker hit itself.
        same_actor: bool,
    }

    /// A map marker of the reported type in the player's worldspace, from
    /// the master files (thuum docs/verbs/map-markers.md).
    #[derive(Debug)]
    struct MarkerCandidate {
        /// The marker reference's form id.
        refr_id: u32,
        /// Its position.
        x: f32,
        /// Its position.
        y: f32,
        /// Its position.
        z: f32,
    }

    /// The marker a discovery means, or none within range; the nearest
    /// candidate's distance either way (infinite without one).
    #[derive(Debug)]
    struct MarkerChoice {
        /// A candidate lies within the discovery range.
        found: bool,
        /// The nearest candidate's form id; 0 without one.
        refr_id: u32,
        /// The nearest candidate's distance from the player.
        distance: f32,
    }

    /// What the core knows of a reported favorite's form (thuum
    /// docs/verbs/favorites.md).
    #[derive(Debug)]
    enum FavoriteKind {
        /// An item the player holds in the core's inventory.
        HeldItem,
        /// An item the player does not hold.
        MissingItem,
        /// A spell or a shout in the master files.
        Magic,
        /// Anything else.
        Other,
    }

    /// A reported favorite and what the core knows of its form.
    #[derive(Debug)]
    struct FavoriteFacts {
        /// The form id.
        form: u32,
        /// -1 for none, 0 to 7 for the keys 1 to 8.
        hotkey: i8,
        /// What the form is to the core.
        kind: FavoriteKind,
    }

    /// A favorite to record.
    #[derive(Debug)]
    struct FavoriteEntry {
        /// The form id.
        form: u32,
        /// -1 for none, 0 to 7.
        hotkey: i8,
    }

    /// One global a player's game wrote for its player, by its FormDesc's
    /// parts (thuum docs/verbs/player-globals.md).
    #[derive(Debug, Clone, PartialEq)]
    struct PlayerGlobalEntry {
        /// The form id within its plugin.
        short_id: u32,
        /// The plugin, as the load order names it.
        file: String,
        /// Its value.
        value: f32,
    }

    /// One stage done on a shared quest, by the quest's FormDesc's parts
    /// (thuum docs/verbs/quest-stage.md, ADR-056).
    #[derive(Debug, Clone, PartialEq)]
    struct QuestStageEntry {
        /// The quest's form id within its plugin.
        short_id: u32,
        /// The plugin, as the load order names it.
        file: String,
        /// The stage index the engine set.
        stage: u16,
    }

    /// A cast the server accepted, for Placements::cast_started (thuum
    /// docs/verbs/spell-placed-object.md, ADR-057): who cast from which hand,
    /// where it stood and faced, and whether a stream opened.
    #[derive(Debug, Clone, PartialEq)]
    struct PlacedCast {
        /// The caster's server id.
        caster: u32,
        /// The hand or slot: 0 left, 1 right, 2 voice, 3 instant.
        hand: u8,
        /// Position.
        x: f32,
        /// Position.
        y: f32,
        /// Position.
        z: f32,
        /// The Z angle, degrees.
        yaw: f32,
        /// A stream opened (its end comes through cast_ended); false, the
        /// cast was released whole.
        concentration: bool,
    }

    /// One object the server places at an accepted cast (thuum
    /// docs/verbs/spell-placed-object.md, ADR-057): its set, its caster, the
    /// spell, the base (a FormDesc's parts) with its record type, and its
    /// pose. A set's objects come as consecutive rows sharing `set`.
    #[derive(Debug, Clone, PartialEq)]
    struct PlacementRow {
        /// The set's id, which names it in a PlacementRemoval.
        set: u64,
        /// The caster, whose id the object carries on the wire (placedBy).
        caster: u32,
        /// The spell's form id within its plugin.
        spell_short_id: u32,
        /// The spell's plugin.
        spell_file: String,
        /// The base's form id within its plugin.
        short_id: u32,
        /// The base's plugin.
        file: String,
        /// The base's record type, MSTT or ACTI.
        record_type: String,
        /// Position.
        x: f32,
        /// Position.
        y: f32,
        /// Position.
        z: f32,
        /// The Z angle, degrees.
        yaw: f32,
    }

    /// A set whose time ran out (thuum docs/verbs/spell-placed-object.md):
    /// `why` 0 the linger after its end, 1 its entry's maxS.
    #[derive(Debug, Clone, PartialEq)]
    struct PlacementRemoval {
        /// The set's id.
        set: u64,
        /// Its caster.
        caster: u32,
        /// Why, as above.
        why: u8,
    }

    /// A cast that placed nothing (thuum docs/verbs/spell-placed-object.md):
    /// `why` 0 the caster's perCaster cap, 1 the server-wide cap, 2 no
    /// report from the caster's game within the window (shape A).
    #[derive(Debug, Clone, PartialEq)]
    struct PlacementRefusal {
        /// The caster.
        caster: u32,
        /// The spell's form id within its plugin.
        spell_short_id: u32,
        /// The spell's plugin.
        spell_file: String,
        /// Why, as above.
        why: u8,
    }

    /// One base the server asks the caster's game to report at an accepted
    /// cast (thuum docs/verbs/spell-placed-object.md, shape A): the set the
    /// reports will name, the caster, the spell, the base (a FormDesc's
    /// parts) with its record type and the window. A set's bases come as
    /// consecutive rows sharing `set`; PartOne sends one PlacedRequest a set.
    #[derive(Debug, Clone, PartialEq)]
    struct PlacementRequestRow {
        /// The set's id, which the caster's reports name.
        set: u64,
        /// The caster asked.
        caster: u32,
        /// The spell's form id within its plugin.
        spell_short_id: u32,
        /// The spell's plugin.
        spell_file: String,
        /// The base's form id within its plugin.
        short_id: u32,
        /// The base's plugin.
        file: String,
        /// The base's record type, MSTT or ACTI.
        record_type: String,
        /// How long the reports are taken from the cast, milliseconds.
        window_ms: u64,
    }

    /// A caster's report of where its engine stands one base of a requested
    /// set (thuum docs/verbs/spell-placed-object.md, shape A; the
    /// PlacedReport message, the base already read as a FormDesc).
    #[derive(Debug, Clone, PartialEq)]
    struct PlacementReport {
        /// The caster answering.
        caster: u32,
        /// The set named by the request.
        set: u64,
        /// The base's form id within its plugin.
        short_id: u32,
        /// The base's plugin.
        file: String,
        /// Position.
        x: f32,
        /// Position.
        y: f32,
        /// Position.
        z: f32,
        /// The Z angle, degrees.
        yaw: f32,
    }

    /// What a report earned: accepted (the object is queued for
    /// due_placements), or `why` 1 no such set of the caster's, 2 late (the
    /// window closed or the set's time ran out), 3 a base the entry does not
    /// name, 4 the base reported already, 5 not a number, 6 outside the band
    /// (`distance` on the ground plane and `dz` from where the caster stood
    /// carry the numbers for the log).
    #[derive(Debug, Clone, PartialEq)]
    struct PlacementReportVerdict {
        /// Accepted.
        accepted: bool,
        /// Why not, as above; 0 when accepted.
        why: u8,
        /// The reported pose's distance from the caster on the ground plane.
        distance: f32,
        /// The reported pose's height above or below the caster.
        dz: f32,
    }

    /// A reference's lock as the record holds it (thuum docs/verbs/locks.md):
    /// the XLOC level byte, the key's base form id (0 none), locked now, picks
    /// broken on it.
    #[derive(Debug, Clone, PartialEq)]
    struct LockStateRow {
        /// The level byte: 0 or 1 Novice, 25, 50, 75, 100, 255 requires key.
        level: u8,
        /// The key's base form id, 0 for none.
        key: u32,
        /// Locked now.
        locked: bool,
        /// Picks broken on it so far.
        broken: u32,
    }

    /// What an activation of a locked reference met (thuum docs/verbs/
    /// locks.md): `verdict` 0 open (not locked), 1 opened with the key the
    /// player holds, 2 locked (the seat's menu is the path); the state after.
    #[derive(Debug, Clone, PartialEq)]
    struct LockActivationVerdict {
        /// 0 open, 1 opened with the key, 2 locked.
        verdict: u8,
        /// The lock after.
        state: LockStateRow,
    }

    /// A lock report judged (thuum docs/verbs/locks.md): accepted, or `why` 1
    /// not locked, 2 out of reach, 3 a key lock, 4 no pick held, 5 a broken
    /// count out of range, 6 inside the gap; `change` 0 nothing, 1 unlocked,
    /// 2 picks broken (`picks` of them leave the record); the state after.
    #[derive(Debug, Clone, PartialEq)]
    struct LockReportVerdict {
        /// Accepted.
        accepted: bool,
        /// Why not, as above; 0 when accepted.
        why: u8,
        /// 0 nothing, 1 unlocked, 2 picks broken.
        change: u8,
        /// Lockpicks to remove from the record for change 2.
        picks: u8,
        /// The lock after.
        state: LockStateRow,
    }

    /// A reference's destruction as the record holds it (thuum D-S3, traps
    /// follow the interaction; ADR-068's DestructionStage row): the stage the
    /// engine reached and the destroyed flag; a reference with no record
    /// reads as the game files' state, stage 0 and not destroyed.
    #[derive(Debug, Clone, PartialEq)]
    struct DestructionRow {
        /// The destruction stage, 0 for none.
        stage: u8,
        /// Destroyed (a spent trap).
        destroyed: bool,
    }

    /// A destruction report judged (wire-rules destruction): `verdict` 0
    /// recorded (the first report of a step: `record` is the state after, to
    /// write and relay), 1 the record stands (an equal or a lower report:
    /// `record` is the record unchanged), 2 a stage past the bound (`record`
    /// unchanged).
    #[derive(Debug, Clone, PartialEq)]
    struct DestructionVerdict {
        /// 0 recorded, 1 the record stands, 2 a stage past the bound.
        verdict: u8,
        /// The record after the report.
        record: DestructionRow,
    }

    /// A skill's standing as the record holds it (thuum docs/verbs/
    /// leveling.md): the skill by its ActorValue (6 to 23), its level and the
    /// experience toward the next.
    #[derive(Debug, Clone, PartialEq)]
    struct LevelingSkill {
        /// The skill's ActorValue, 6 to 23.
        skill_av: u8,
        /// The level, 15 to 100.
        level: u8,
        /// Experience toward the next level.
        xp: f32,
    }

    /// The character's standing (thuum docs/verbs/leveling.md).
    #[derive(Debug, Clone, PartialEq)]
    struct LevelingCharacter {
        /// The level, 1 and up.
        level: u16,
        /// Experience toward the next level.
        xp: f32,
    }

    /// What a skill use changed (thuum docs/verbs/leveling.md): `kind` 0 a
    /// skill reached `level` (the skill at `skill_av`), 1 the character
    /// reached `level`.
    #[derive(Debug, Clone, PartialEq)]
    struct LevelingEvent {
        /// 0 a skill level, 1 a character level.
        kind: u8,
        /// The skill's ActorValue for kind 0; 0 otherwise.
        skill_av: u8,
        /// The level reached.
        level: u16,
    }

    /// A skill use judged (thuum docs/verbs/leveling.md): accepted, or `why`
    /// 1 no skill at that ActorValue, 2 an amount that is not a number, 3 an
    /// amount of 0 or below or over the bound; the eighteen skills after (in
    /// ActorValue order), the character after, and what changed in order.
    #[derive(Debug, Clone, PartialEq)]
    struct LevelingOutcome {
        /// Accepted.
        accepted: bool,
        /// Why not, as above; 0 when accepted.
        why: u8,
        /// Every skill after, 6 to 23.
        skills: Vec<LevelingSkill>,
        /// The character after.
        character: LevelingCharacter,
        /// What changed, in order.
        events: Vec<LevelingEvent>,
    }

    /// One actor value's base (thuum docs/verbs/actor-values.md).
    #[derive(Debug, Clone)]
    struct AvBase {
        /// 0 to 163.
        av: u8,
        /// Its base value.
        base: f32,
    }

    /// A percentage the server raised, awaiting the client's report of it
    /// (thuum docs/verbs/attributes.md).
    struct PercentHold {
        /// Health 24, Magicka 25, Stamina 26.
        av: u8,
        /// The percentage set, 0 to 1.
        value: f32,
        /// When the hold lapses, Unix milliseconds.
        until_ms: u64,
    }

    /// One skill's progress.
    #[derive(Debug, Clone)]
    struct AvSkill {
        /// 0 to 17.
        skill: u8,
        /// The skill's level as its progress counts it.
        level: f32,
        /// Experience toward the next level.
        xp: f32,
        /// Experience the next level needs.
        threshold: f32,
    }

    /// How many times a skill was made legendary.
    #[derive(Debug, Clone)]
    struct AvLegendary {
        /// 0 to 17.
        skill: u8,
        /// Times made legendary.
        count: u16,
    }

    /// A player's actor values and progress.
    #[derive(Debug, Clone)]
    struct AvSnapshot {
        /// Base values, each actor value once.
        bases: Vec<AvBase>,
        /// Skills' progress.
        skills: Vec<AvSkill>,
        /// The character's experience toward the next level.
        xp: f32,
        /// Experience the next level needs.
        threshold: f32,
        /// The character level.
        level: u16,
        /// Skills made legendary.
        legendary: Vec<AvLegendary>,
    }

    /// What the server does with a console command a player sent (thuum
    /// docs/verbs/console-commands.md).
    #[derive(Debug, Clone, Copy)]
    enum ConsoleDecision {
        /// Run it.
        Run,
        /// The caller's rank is below the command's.
        RankTooLow,
        /// Nobody may run it.
        Refused,
        /// Listed, not run by the server yet.
        NotServed,
        /// A client-only command.
        ClientOnly,
        /// Not in the table.
        Unknown,
    }

    /// What a move a player's actor made means for the jump the server
    /// permitted it (thuum docs/verbs/console-commands.md, COC).
    #[derive(Debug, Clone, Copy)]
    enum JumpCheck {
        /// No jump is permitted: the move is judged as any other.
        NoPermit,
        /// The permitted jump: it passes, and the permit ends.
        Landed,
        /// A jump is permitted and this move is not it: drop it without
        /// sending the player back.
        Waiting,
    }

    /// A ranged hit's claim on its shot (thuum docs/verbs/marksman.md).
    #[derive(Debug, Clone, Copy)]
    enum ShotCheck {
        /// A recorded shot covers the hit, and is used up.
        Claimed,
        /// No unused shot of that weapon in the window: refuse the hit.
        NoShot,
        /// Shots wait, but none can have reached the target yet: refuse.
        TooFar,
    }

    /// A ranged hit's claim and, when claimed, its shot's draw power (0 to
    /// 1, the factor on the hit's damage; 0 otherwise).
    #[derive(Debug, Clone, Copy)]
    struct ShotClaim {
        /// Claimed, or why not.
        check: ShotCheck,
        /// The claimed shot's draw power.
        power: f32,
    }

    /// A shot as its shooter loosed it: the weapon, the draw power (0 to 1)
    /// and where the shooter stood.
    #[derive(Debug, Clone, Copy)]
    struct ShotFacts {
        /// The bow or crossbow.
        weapon: u32,
        /// The draw power.
        power: f32,
        /// Where the shooter stood.
        x: f32,
        /// Where the shooter stood.
        y: f32,
        /// Where the shooter stood.
        z: f32,
    }

    /// How an effect changes its actor values (the MGEF archetype; thuum
    /// docs/verbs/magic-effects.md).
    #[derive(Debug, Clone, Copy)]
    enum EffectKind {
        /// Archetype 0, value modifier.
        Value,
        /// Archetype 34, peak value modifier.
        PeakValue,
        /// Archetype 5, dual value modifier.
        DualValue,
        /// A state that changes no value (Invisibility, Light,
        /// Etherealize; thuum docs/verbs/self-cast.md).
        State,
        /// Archetype 22, reanimate: the thrall's state (thuum
        /// docs/verbs/npc-fate.md), which its actor's death alone ends
        /// (wire-rules magic::EffectKind::Reanimate).
        Reanimate,
    }

    /// How an effect arrives.
    #[derive(Debug, Clone, Copy)]
    enum EffectArrival {
        /// A drink, a poisoned hit, a fire-and-forget spell's hit.
        Once,
        /// A concentration spell's hit, counting the seconds it held.
        Stream,
    }

    /// One effect of an item or a spell, as the records give it.
    #[derive(Debug, Clone, Copy)]
    struct EffectFacts {
        /// The MGEF.
        effect: u32,
        /// The potion, poison, spell or enchantment that carries it.
        source: u32,
        /// The archetype.
        kind: EffectKind,
        /// The primary actor value.
        av: u32,
        /// A dual modifier's second actor value, u32::MAX for none.
        second_av: u32,
        /// The second value's share of the magnitude.
        second_weight: f32,
        /// The item's magnitude for this effect.
        magnitude: f32,
        /// The item's duration for this effect, seconds; 0 for none.
        duration_s: f32,
        /// MGEF flag Recover.
        recover: bool,
        /// MGEF flag Detrimental.
        detrimental: bool,
        /// MGEF flag NoDuration.
        no_duration: bool,
    }

    /// A change to one actor value: `current` moves the value alone,
    /// `modifier` the temporary modifier (the maximum and the value).
    #[derive(Debug, Clone, Copy)]
    struct AvChange {
        /// The actor value.
        av: u32,
        /// The change to the current value alone.
        current: f32,
        /// The change to the temporary modifier.
        modifier: f32,
    }

    /// A running effect, as the change form keeps it.
    #[derive(Debug, Clone, Copy)]
    struct EffectEntry {
        /// The facts it was applied with, its magnitude scaled.
        facts: EffectFacts,
        /// Who applied it, 0 for none.
        caster: u32,
        /// Seconds it has run.
        elapsed_s: f32,
    }

    /// What one applied effect did at once, and what keeps running.
    #[derive(Debug, Clone)]
    struct EffectApplied {
        /// The changes made now.
        changes: Vec<AvChange>,
        /// Whether `entry` keeps running.
        has_entry: bool,
        /// The running entry, when `has_entry`.
        entry: EffectEntry,
    }

    /// How a spell is cast (the record's SPIT casting type; thuum
    /// docs/verbs/spell-cast.md).
    #[derive(Debug, Clone, Copy)]
    enum CastKind {
        /// One release, one projectile or one touch.
        FireAndForget,
        /// A stream held while the hand casts.
        Concentration,
    }

    /// A cast as its caster's game made it.
    #[derive(Debug, Clone, Copy)]
    struct CastFacts {
        /// The spell.
        spell: u32,
        /// The hand or slot: 0 left, 1 right, 2 voice, 3 instant.
        hand: u8,
        /// Fire-and-forget or concentration.
        kind: CastKind,
        /// Whether the spell hits an area.
        area: bool,
        /// How far from the caster its hit can be, units (its projectile's
        /// range, from the records).
        reach: f32,
        /// Where the caster stood.
        x: f32,
        /// Where the caster stood.
        y: f32,
        /// Where the caster stood.
        z: f32,
        /// The spells the cast's records reach, 0 for an empty slot (thuum
        /// docs/verbs/second-hand.md; wire-rules casts::MAX_REACHED).
        reached: [u32; 8],
        /// How long a reached spell may claim the cast, milliseconds.
        linger_ms: u64,
        /// Bit i set: `reached[i]` is a concentration spell, counted by its
        /// time.
        reached_concentration: u8,
        /// Every effect is delivered to its target (MGEF delivery touch or
        /// target actor): the cast's relay waits for its first hit (thuum
        /// docs/verbs/spell-cast.md, wire-rules casts).
        lands_on_target: bool,
        /// How long the cast itself may claim its hit when longer than the
        /// bolt window: a placed trigger's (a rune's); 0 for the bolt window.
        hazard_ms: u64,
        /// The cast was dual cast (SpellCastMessage.isDualCasting), for
        /// EffectWasDualCast (724).
        dual: bool,
    }

    /// A spell hit as the caster's game reported it.
    #[derive(Debug, Clone, Copy)]
    struct CastHit {
        /// The spell.
        spell: u32,
        /// The target's form id.
        target: u32,
        /// Where the target stands.
        x: f32,
        /// Where the target stands.
        y: f32,
        /// Where the target stands.
        z: f32,
    }

    /// A spell hit's claim on its cast.
    #[derive(Debug, Clone, Copy)]
    enum CastCheck {
        /// A recorded cast covers the hit.
        Claimed,
        /// No cast of that spell covers it: refuse.
        NoCast,
        /// Casts wait, but the target is beyond their reach: refuse.
        TooFar,
        /// A reached spell's hit on a target it claimed within the tick:
        /// refuse.
        Throttled,
    }

    /// A bond's facts (thuum docs/verbs/shadowbond.md): whether there was
    /// one (the actor was in a standing bond, or the spell is a bond), its
    /// two actors, the window and slack for their two landings, its
    /// duration, and the concealment the server records and casts for it
    /// (train-28), if its entry names one.
    #[derive(Debug, Clone)]
    struct BondBreak {
        /// The actor was in a standing bond, now ended (or, from `record`,
        /// the spell is a bond and this is it).
        found: bool,
        /// Who cast the bond's spell.
        caster: u32,
        /// Whom it landed on.
        target: u32,
        /// How long after the break each landing is permitted, ms.
        window_ms: u64,
        /// How far from the other's position a landing may fall, units.
        slack_units: f32,
        /// How long the bond stands from its claim, ms.
        duration_ms: u64,
        /// Whether the entry names a proc, the concealment.
        has_proc: bool,
        /// The proc's form id within its plugin.
        proc_short_id: u32,
        /// The proc's plugin, as the load order names it.
        proc_file: String,
    }

    /// A spell hit's claim and, when claimed, the factor on the spell's
    /// magnitude (1 for a fire-and-forget hit, seconds for a stream's).
    #[derive(Debug, Clone, Copy)]
    struct CastClaim {
        /// Claimed, or why not.
        check: CastCheck,
        /// The factor on the magnitude.
        scale: f32,
        /// The hit is the first of a cast whose relay waited for it: relay
        /// the cast now, with this hit's target.
        relay: bool,
        /// The claimed cast was dual cast.
        dual: bool,
    }

    /// What the server knows at a headshot's report (thuum
    /// docs/verbs/headshot.md): the victim's head and hair slots bare by its
    /// equipment record, its race as a FormDesc's parts, the shooter's base
    /// Archery.
    #[derive(Debug, Clone)]
    struct HeadshotFacts {
        /// Nothing worn in the head and hair slots (ARMO BOD2 30 and 31).
        head_bare: bool,
        /// The victim's race: its id within its plugin.
        race_short: u32,
        /// The victim's race's plugin.
        race_file: String,
        /// The shooter's base Archery.
        archery: f32,
    }

    /// The server's answer to a headshot report.
    #[derive(Debug, Clone, Copy)]
    enum HeadshotCheck {
        /// Everything holds: the victim dies.
        Killed,
        /// No claimed bow hit on that target within the window, or spent.
        NoShot,
        /// The victim wears something in the head or hair slot.
        Helmeted,
        /// The victim's race is not one the rule names.
        Race,
        /// The shooter's Archery is under the floor.
        Archery,
    }

    /// thuum docs/verbs/scripted-effects.md: what Finger of Death does to
    /// its two actors (wire-rules scripted_effects::finger_of_death).
    #[derive(Debug, Clone, Copy)]
    struct DeathBlast {
        /// Health the target loses.
        target_damage: f32,
        /// Health the caster loses.
        caster_damage: f32,
        /// The target dies by the caster's hand.
        kill: bool,
    }

    /// A form a RaceMenu look names (thuum ADR-026), or a cast teleport's
    /// look (TeleportLook): its plugin and its id within it.
    #[derive(Debug, Clone)]
    struct LookRef {
        /// The plugin's file name.
        plugin: String,
        /// The form's id within the plugin, 24 bits.
        id: u32,
    }

    /// What a look says of the vanilla appearance; `ok` false for text that
    /// is no look.
    #[derive(Debug, Clone)]
    struct LookAppearance {
        /// The text is a look.
        ok: bool,
        /// Head parts in the look's order.
        parts: Vec<LookRef>,
        /// The look carries a hair colour.
        has_hair_color: bool,
        /// 0xRRGGBB.
        hair_color: u32,
        /// The look carries a weight.
        has_weight: bool,
        /// 0 to 100.
        weight: f32,
        /// The look carries the face's texture set.
        has_head_texture: bool,
        /// The face's texture set.
        head_texture: LookRef,
    }

    /// What the core knows of one head part a look names.
    #[derive(Debug, Clone)]
    struct LookPartFacts {
        /// Its form id in the server's load order; 0 when none.
        form: u32,
        /// Its valid-race list holds the appearance's race (true without a list).
        valid_for_race: bool,
        /// The extra parts it lists.
        extras: Vec<u32>,
    }

    /// thuum docs/verbs/cast-teleport.md, the look on other screens: what a
    /// teleport's castTeleports entry names for the other players' games to
    /// play on its caster's figure, each form as the entry writes it; every
    /// `has_` false for a spell the server names no teleport, or whose entry
    /// names no look.
    #[derive(Debug, Clone)]
    struct TeleportLook {
        /// The look names a vanish.
        has_vanish: bool,
        /// A VisualEffect played from the cast until the landing.
        vanish: LookRef,
        /// The look names an appear.
        has_appear: bool,
        /// A VisualEffect played at the landing.
        appear: LookRef,
        /// How long the appear plays, seconds.
        appear_seconds: f32,
        /// The look names a sound.
        has_sound: bool,
        /// A Sound played at the landing.
        sound: LookRef,
    }

    /// The appearance's head parts a look implies; `ok` false refuses the look.
    #[derive(Debug, Clone)]
    struct DerivedParts {
        /// The look's parts are all known and valid for the race.
        ok: bool,
        /// Each part followed by its extras, each once.
        parts: Vec<u32>,
    }

    /// The record after a report, and the server's values still held.
    #[derive(Debug)]
    struct AvMerge {
        /// The new record.
        record: AvSnapshot,
        /// The values the server set that the client has not reported yet.
        held: Vec<AvBase>,
    }

    /// Two player actors in a fight, the lower form id first.
    #[derive(Debug)]
    struct FightPair {
        /// The lower actor form id.
        a: u32,
        /// The higher actor form id.
        b: u32,
    }

    /// The hit flags the server keeps.
    #[derive(Debug)]
    struct Flags {
        /// Power attack kept.
        power: bool,
        /// Sneak attack kept.
        sneak: bool,
        /// Bash kept (thuum docs/verbs/blocking.md).
        bash: bool,
    }

    /// The judgement of a perk pick (thuum docs/verbs/perks.md): accepted, or
    /// refused with its reason code and a detail (the perk a prerequisite
    /// names, the actor value a requirement names, the function the rule
    /// cannot judge; 0 otherwise).
    #[derive(Debug)]
    struct PerkVerdict {
        /// Accepted.
        accepted: bool,
        /// The reason code when refused (E_PERK_...), empty when accepted.
        code: String,
        /// The code's detail.
        detail: u32,
    }

    /// A weather word for one player (thuum docs/verbs/weather.md): the
    /// weather of the place it is in, or a release (`weather` 0) in an
    /// interior or a worldspace without a pool; `some` false when nothing is
    /// owed.
    #[derive(Debug)]
    struct WeatherWord {
        /// Whether a word is owed.
        some: bool,
        /// The worldspace, or the interior cell the release is for.
        place: u32,
        /// The WTHR form id; 0 releases.
        weather: u32,
        /// The slot it stands for.
        slot: u64,
        /// Blend (true) or cut (false).
        transition: bool,
    }

    /// The game clock at one instant, as the engine's six time globals hold
    /// it (thuum docs/verbs/time.md).
    #[derive(Debug)]
    struct GameTime {
        /// GameYear.
        year: u32,
        /// GameMonth, from 0.
        month: u32,
        /// GameDay, from 1.
        day: u32,
        /// GameHour, at least 0 and below 24.
        hour: f32,
        /// GameDaysPassed.
        days_passed: f32,
        /// TimeScale.
        time_scale: f32,
    }

    /// What the core knows about one of a hosted actor's values (thuum
    /// docs/verbs/npc-stream.md, ADR-050; wire-rules hosted_state).
    #[derive(Debug, Clone, Copy)]
    struct HostedValueFacts {
        /// The host's percentage, 0 to 1.
        reported: f32,
        /// The record's percentage.
        recorded: f32,
        /// The hold table refuses this report (a value the server raised that
        /// the host has not reported yet).
        held: bool,
        /// The server lowered this value by its own number at some time.
        lowered: bool,
        /// Milliseconds since that lowering (read when `lowered`).
        lowered_ago_ms: u64,
    }

    /// The facts of one state report.
    #[derive(Debug, Clone, Copy)]
    struct HostedStateFacts {
        /// Health.
        health: HostedValueFacts,
        /// Magicka.
        magicka: HostedValueFacts,
        /// Stamina.
        stamina: HostedValueFacts,
        /// The record is alive and not respawning.
        alive: bool,
        /// One point of health as a percentage of the actor's maximum.
        one_point: f32,
    }

    /// The verdict on one value.
    #[derive(Debug, Clone, Copy)]
    struct HostedValueVerdict {
        /// The percentage to record.
        record: f32,
        /// The record changes (the value travels to the viewers).
        changed: bool,
        /// The report was dropped against the server's number.
        dropped: bool,
    }

    /// The verdict on one state report.
    #[derive(Debug, Clone, Copy)]
    struct HostedStateVerdict {
        /// Health.
        health: HostedValueVerdict,
        /// Magicka.
        magicka: HostedValueVerdict,
        /// Stamina.
        stamina: HostedValueVerdict,
        /// The health was floored at one point.
        floored: bool,
        /// The whole report was dropped: a dead or respawning record.
        dead: bool,
    }

    /// What the core knows about a player's rest (thuum docs/verbs/rest.md).
    #[derive(Debug)]
    struct RestFacts {
        /// The game hours the client says its player rested.
        hours: f32,
        /// A sleep in a bed rather than a wait.
        sleep: bool,
        /// The core holds the player dead.
        is_dead: bool,
        /// The player dealt or took a hit the core saw.
        has_hit: bool,
        /// Milliseconds since the last such hit (0 when there was none).
        since_last_hit_ms: u64,
        /// The player is in a fight with another player.
        in_fight: bool,
    }

    /// Why a rest is refused, `Allowed` when it is let through.
    #[derive(Debug)]
    enum RestRefusal {
        /// Let through.
        Allowed,
        /// Hours outside the menu's 1 to 24, or not a number.
        Hours,
        /// The server switched this kind of rest off.
        Off,
        /// The player is dead.
        Dead,
        /// A hit within the last 10 s.
        Fighting,
    }

    /// What the core knows about a shout a player's game reported (thuum
    /// docs/verbs/shouts.md).
    #[derive(Debug)]
    struct VoiceFacts {
        /// The voice slot holds a shout the player's record has.
        shout_equipped: bool,
        /// The cast spell's place among the shout's variations, 0 to 2, or
        /// anything else for none.
        variation: u8,
        /// The record's unlocked words among the shout's three, bit i for
        /// word i.
        words_unlocked: u8,
        /// The variation's recovery time, seconds.
        recovery_s: f32,
        /// The shouter's ShoutRecoveryMult (1 without a record).
        recovery_mult: f32,
    }

    /// Why a shout is refused, `Accepted` when it stands.
    #[derive(Debug)]
    enum VoiceRefusal {
        /// The shout stands.
        Accepted,
        /// The voice slot holds no shout the record has.
        NoShout,
        /// The spell is none of the shout's variations.
        NotAVariation,
        /// A word up to the variation is not unlocked in the record.
        WordLocked,
        /// The last shout's recovery has not run out.
        Recovering,
    }

    /// One attribute as the core's CropRegeneration takes it.
    #[derive(Debug)]
    struct Regen {
        /// The current percentage, 0 to 1.
        percentage: f32,
        /// The regeneration rate, percent of the maximum a second.
        rate: f32,
        /// The rate multiplier, percent.
        rate_mult: f32,
    }

    /// What the cell host table decides (thuum docs/verbs/cell-host.md,
    /// ADR-035).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum HostKind {
        /// `player` hosts `cell` at `epoch` from now.
        Grant,
        /// `player` stops hosting `cell`; `epoch` is the grant that ends.
        Release,
        /// Nobody hosts `cell`: it freezes at its record, the epoch kept.
        Unhosted,
        /// A player's claim on `actor` in `cell` ended with its lease
        /// (ADR-052): the cell's rule names the simulator again.
        ClaimEnded,
        /// A claim on `actor` in `cell` ended with its claimant `player`'s
        /// departure (ADR-060; a dialogue's alike): the cell's rule names
        /// the simulator again, the word follows.
        ClaimGone,
        /// A thrall whose commander left has reached its effect's estimate
        /// (ADR-060, HYPOTHESIS): the server's word ends it.
        ThrallExpired,
    }

    /// One decision of the cell host table, for the core to send to its
    /// game or apply.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct HostDecision {
        /// Which decision.
        kind: HostKind,
        /// The player it concerns (0 for Unhosted, ClaimEnded and
        /// ThrallExpired; the claimant that left for ClaimGone).
        player: u32,
        /// The cell.
        cell: u32,
        /// The grant's epoch (the cell's kept epoch for Unhosted).
        epoch: u32,
        /// The actor a ClaimEnded, ClaimGone or ThrallExpired concerns (0 for
        /// the others).
        actor: u32,
    }

    /// A simulator's report moving an actor between cells, the facts the
    /// core gathers for `may_move` (ADR-047 D6): the reporting player's
    /// actor, the actor moved, the cell its record stands in and the cell
    /// the report puts it in (0 for none), whether the two exterior squares
    /// touch in one worldspace, whether the move passes a door (an interior
    /// at either end, any actor's: memo D54 B).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct CellMove {
        /// The reporting player's actor.
        sender: u32,
        /// The actor moved.
        actor: u32,
        /// The cell the actor's record stands in (0 for none).
        origin: u32,
        /// The cell the report puts it in (0 for none).
        dest: u32,
        /// Whether the two exterior grid squares touch in one worldspace.
        adjacent: bool,
        /// Whether the move passes a door: an interior at either end, any
        /// actor's (memo D54 B).
        door: bool,
        /// Units from the reported position to the destination square's nearest
        /// edge (u32::MAX for an interior): the pass margin's fact (D22).
        edge_units: u32,
    }

    /// A follow (ADR-047 D6, thuum docs/verbs/npc-handoff.md): `player`
    /// simulates `actor` in `cell`, a cell nobody hosts, for the lab's reading.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct CellFollowRow {
        /// The actor followed.
        actor: u32,
        /// The player whose game simulates it.
        player: u32,
        /// The cell nobody hosts that it stands in.
        cell: u32,
    }

    /// One cell the host table knows, for the lab's reading of the server.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct CellHostRow {
        /// The cell.
        cell: u32,
        /// False while nobody hosts it.
        hosted: bool,
        /// The host's player actor (0 when unhosted).
        player: u32,
        /// The standing grant's epoch (the kept epoch when unhosted).
        epoch: u32,
    }

    /// Who hosts a cell now.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct CellHost {
        /// False while nobody hosts the cell.
        hosted: bool,
        /// The host's player actor (0 when unhosted).
        player: u32,
        /// The standing grant's epoch (the kept epoch when unhosted).
        epoch: u32,
    }

    /// A reported script call's row and subject (thuum ADR-068, wire-rules
    /// natives): the row the core hands the shape step before it reads
    /// anything of the subject.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct ScriptCallSubject {
        /// The row's place in the table, -1 when the pair has none.
        row: i32,
        /// The subject's form id as the reporter named it (0x14 for its own
        /// player), 0 when the call names none the reporter may name.
        subject: u32,
        /// The record the row keeps, so the core gathers a record row's
        /// facts (thuum ADR-068 rung 4): 0 none, 1 the ref state, 2 the
        /// standing, 3 a placement, 4 a player's own (rung 5: its subject
        /// 0x14, judged by player_own_judge after the owner step).
        record: u8,
        /// The row's look channel, so the core gathers the subject's firing
        /// facts (thuum ADR-068 D-W2 A, wire-rules natives::Look): 0 never,
        /// 1 its own channel first and then the look, 2 the look alone.
        look: u8,
    }

    /// One argument of a reported script call as the core parsed it: its
    /// kind and an object's form id; the value is never judged.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct ScriptArg {
        /// 0 null (unread, or a None object), 1 bool, 2 number, 3 string,
        /// 4 object.
        kind: u8,
        /// The object's form id (kind 4 alone).
        form_id: u64,
    }

    /// What the core knows of a reported script call (wire-rules
    /// natives::Facts).
    #[derive(Debug, Clone, Copy, PartialEq)]
    struct ScriptCallFacts {
        /// The reporter's actor.
        sender: u32,
        /// The message's selfId: the call's subject.
        self_id: u32,
        /// The message's epoch.
        epoch: u32,
        /// What the server holds at the subject: 0 no reference, 1 a world
        /// object, 2 a door or a container, 3 an actor, 4 a world object it
        /// holds no reference for, a game-file placement (REFR) placed by its
        /// record (wire-rules natives::SubjectKind::Record); any other value
        /// reads as 0.
        subject_kind: u8,
        /// The subject's cell, 0 when the server cannot place it.
        cell: u32,
        /// The host of that cell, 0 while nobody hosts it, for a world
        /// object, held or placed by its record; an actor's simulator for an
        /// actor (0 for none), which a row kept as evidence reads; 0 for
        /// every other kind.
        owner: u32,
        /// That host's standing epoch (unread for an actor).
        owner_epoch: u32,
        /// The host of an actor's cell, 0 while nobody hosts it (the cell
        /// the server holds it in, or its ACHR's for one it holds no
        /// reference for): the owner of a ref state's or a placement's
        /// record row on an actor nobody simulates (thuum ADR-068 rung 4);
        /// unread for every other kind and row.
        host: u32,
        /// That host's standing epoch.
        host_epoch: u32,
        /// The subject's firing facts for a row with a look channel (thuum
        /// ADR-068 D-W2 A; `look` in ScriptCallSubject), as the core
        /// gathers a trigger's firing (GatherFiringFacts over the subject it
        /// holds, none for one it places by its record): read only when its
        /// reporter and cell are the call's, so one never gathered (all
        /// zero) reads as a game without the cell loaded.
        firing: TriggerFiringFacts,
    }

    /// The rule's word on a reported script call.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum ScriptCallCode {
        /// Relayed to the audience but the reporter, kept as the subject's
        /// evidence (`keep_evidence`) or recorded as its record (`record`),
        /// from the owner step; from the shape step, the shape holds and the
        /// core gathers the subject's facts for the owner step.
        Accepted,
        /// Dropped quietly: a door's or a container's.
        Skipped,
        /// E_SCRIPTCALL_ROW: no tabled action row the server relays, nor a
        /// state row it keeps.
        Row,
        /// E_SCRIPTCALL_ARGS: not the row's count and kinds, or no subject
        /// the reporter may name.
        Args,
        /// E_SCRIPTCALL_OWNER: the reporter does not own the subject at the
        /// epoch.
        Owner,
        /// E_SCRIPTCALL_RATE: the reporter's budget is spent.
        Rate,
        /// E_SCRIPTCALL_FAR: a look from a game with the subject's cell
        /// loaded, none of whose actors stands within the subject's firing
        /// reach (thuum ADR-068 D-W2 A).
        Far,
        /// E_SCRIPTCALL_LOOK_RATE: a look past the subject's look cap.
        LookRate,
    }

    /// The verdict on one reported script call.
    #[derive(Debug)]
    struct ScriptCallVerdict {
        /// Which word.
        code: ScriptCallCode,
        /// The refusal's stable name for the log, empty when accepted or
        /// skipped.
        name: String,
        /// Why, for the log; empty when accepted or skipped.
        why: String,
        /// The subject's form id (accepted).
        subject: u32,
        /// The subject's sequence after this call, 1 for its first
        /// (accepted).
        seq: u32,
        /// The relay native's class (accepted).
        relay_cls: String,
        /// The relay native's name (accepted).
        relay_fn: String,
        /// The call's arguments the relay carries, by index, in order.
        relay_args: Vec<u8>,
        /// The relay's self: the subject for a method native, 0 for a static
        /// one.
        relay_self: u32,
        /// Who hears it: 0 the subject's listeners, 1 every seat, 2 none, 3
        /// the subject's cell's host alone (a look's CreateDetectionEvent,
        /// thuum ADR-068 D-W2 A).
        audience: u8,
        /// The call's graph event becomes the subject's lastAnimation.
        keep_last: bool,
        /// The call is kept as its subject's evidence and relayed to no seat
        /// (wire-rules natives Keep::Evidence: ProcessTrapHit's, a trap's hit
        /// on an actor the reporter simulates, its death evidence), with no
        /// sequence (accepted).
        keep_evidence: bool,
        /// The record the call moves, recorded with the subject's next
        /// sequence (`seq`) and relayed as the record's own word, never as
        /// an action (wire-rules natives Keep::Record, thuum ADR-068 rung
        /// 4): 1 the ref state (Enable, EnableNoWait, Disable,
        /// DisableNoWait, Delete), 2 the standing (an NPC's factions, crime
        /// faction and relationships), 3 a placement (PlaceAtMe,
        /// PlaceActorAtMe), 4 a player's own (thuum ADR-068 rung 5:
        /// Actor.SetRace, AddSpell, RemoveSpell, AddShout, RemoveShout,
        /// EquipItem on the reporter's own player, `subject` 0x14 and `seq`
        /// 0, which the core hands player_own_judge); 0 for every other
        /// verdict.
        record: u8,
        /// The argument naming the call's behavior graph event, for the
        /// "animates" line; -1 for none.
        event_arg: i8,
        /// The call's graph variable (its first argument's name, its
        /// second's value) is kept on its subject with `seq`, by its folded
        /// kind: 3 an int, 4 a float, 5 a bool (wire-rules natives
        /// Keep::Variable, the fifty fixes' D-C3 A, PENDING ELI); 0 for
        /// none.
        keep_var: u8,
        /// Taken as a firing's look (thuum ADR-068 D-W2 A, wire-rules
        /// natives' look channel): relayed to the subject's listeners as
        /// `relay_cls`.`relay_fn`, kept nowhere, `seq` 0 for a relay with no
        /// sequence (Sound.Play's, its self the Sound).
        look: bool,
        /// The relay names the reporter's own player (0x14 among the call's
        /// object arguments) as its figure, the reporter's actor (a look's:
        /// CreateDetectionEvent's akOwner).
        figure_args: bool,
    }

    /// One graph variable a subject's record keeps, as the core reads it
    /// for the slot rule (wire-rules natives::KeptVariable): whether it names
    /// the reported call's variable, the names compared without case, and
    /// the sequence it was kept at.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct GraphVariableKept {
        /// It names the reported call's variable.
        same_name: bool,
        /// The sequence it was kept at.
        seq: u32,
    }

    /// Where a reported call's variable goes in its subject's record
    /// (wire-rules natives::variable_slot): `code` 0 in place of the kept
    /// entry at `index`, 1 a new entry at the end, 2 nowhere (the record
    /// keeps the same variable at a later sequence).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct GraphVariableSlot {
        /// 0 replace, 1 append, 2 stale.
        code: u8,
        /// The entry replaced (code 0), else 0.
        index: u32,
    }

    /// What the core knows of a reported script record (thuum ADR-068 rung
    /// 3, wire-rules script_state::Facts).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct ScriptStateFacts {
        /// The reporter's actor.
        sender: u32,
        /// The record's epoch: the cell's host epoch the reporter holds.
        epoch: u32,
        /// What the server holds at the reference: 0 no reference, 1 a
        /// reference that is no actor, 2 an actor; any other value reads as
        /// 0.
        ref_kind: u8,
        /// The reference's cell, 0 when the server cannot place it.
        cell: u32,
        /// The host of that cell, 0 while nobody hosts it.
        owner: u32,
        /// The cell's standing epoch (the kept one while nobody hosts it).
        owner_epoch: u32,
    }

    /// The rule's word on a reported script record.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum ScriptStateCode {
        /// Recorded: the reporter hosts the reference's cell at the epoch.
        Accepted,
        /// Recorded: the release read, a read of the host that just
        /// released the cell, at the epoch it released, while nobody hosts
        /// the cell.
        ReleaseRead,
        /// E_SCRIPTSTATE_NO_REF: no reference the server holds or loads.
        NoRef,
        /// E_SCRIPTSTATE_KIND: an actor's record.
        Kind,
        /// E_SCRIPTSTATE_OWNER: the reporter does not host the cell, nor is
        /// its read the release read.
        Owner,
        /// E_SCRIPTSTATE_EPOCH: the record's epoch is not the cell's.
        Epoch,
    }

    /// The verdict on one reported script record.
    #[derive(Debug)]
    struct ScriptStateVerdict {
        /// Which word.
        code: ScriptStateCode,
        /// The refusal's stable name for the log, empty when recorded.
        name: String,
        /// Why, for the log; empty when recorded.
        why: String,
    }

    /// What the core knows of a trap trigger's reported firing, its own
    /// activation or its first link (thuum ADR-068 D-W1; wire-rules
    /// trigger_entry::FiringFacts).
    #[derive(Debug, Clone, Copy, PartialEq)]
    struct TriggerFiringFacts {
        /// The reporter's actor.
        reporter: u32,
        /// The cell the firing is judged in (a linked pair's target's, a
        /// trigger's own for its own activation), 0 when the server cannot
        /// place it.
        cell: u32,
        /// That cell's host, 0 while nobody hosts it.
        host: u32,
        /// The reporter's game reports the cell loaded.
        loader: bool,
        /// How far the nearest of the reporter's actors (its player, or an
        /// actor it simulates) stands from the trigger, units; -1 (any
        /// negative or non-finite value) when none stands in the trigger's
        /// cell or world, or the server places no trigger.
        nearest: f32,
        /// The trigger's size: the farthest point of its bounds, times its
        /// scale.
        trigger_size: f32,
    }

    /// The rule's word on a reported firing.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum TriggerFiringCode {
        /// Taken: the cell's host's, as before.
        Host,
        /// Taken: a game with the cell loaded, one of whose actors stands by
        /// the trigger (D-W1, the presser's owner).
        Loader,
        /// E_TRAP_NOT_HOST: neither the cell's host nor a game with the cell
        /// loaded.
        NotHost,
        /// E_TRAP_FAR: a game with the cell loaded, none of whose actors
        /// stands by the trigger.
        Far,
    }

    /// The verdict on one reported firing.
    #[derive(Debug)]
    struct TriggerFiringVerdict {
        /// Which word.
        code: TriggerFiringCode,
        /// The refusal's stable name for the log, empty when taken.
        name: String,
        /// Why, for the log; empty when taken.
        why: String,
    }

    /// What the core knows of a presser's owner's report that its actor
    /// entered or left a trigger whose world script runs on the cell's host
    /// (thuum ADR-068 D-C2; wire-rules trigger_entry::EntryFacts).
    #[derive(Debug, Clone, Copy, PartialEq)]
    struct TriggerEntryFacts {
        /// The reporter's actor.
        reporter: u32,
        /// What the server holds or places by its record at the trigger: 0
        /// no reference, 1 a reference that is no actor, 2 an actor; any
        /// other value reads as 0.
        trigger_kind: u8,
        /// The presser: the reporter's own actor for its player, or another
        /// actor.
        presser: u32,
        /// The presser's owner: its seat's actor for a player's actor, an
        /// NPC's simulator, 0 for nobody.
        presser_owner: u32,
        /// The trigger's cell, 0 when the server cannot place it.
        cell: u32,
        /// That cell's host, 0 while nobody hosts it.
        host: u32,
        /// The reporter's game reports the cell loaded.
        loader: bool,
        /// How far the presser stands from the trigger, units; -1 (any
        /// negative or non-finite value) when it stands in another cell or
        /// world.
        distance: f32,
        /// The trigger's size: the farthest point of its bounds, times its
        /// scale.
        trigger_size: f32,
        /// The trigger's form id, the pairing's key beside the presser's.
        trigger_id: u32,
        /// The report is a leave (the wire's entry 2), an enter otherwise.
        leave: bool,
    }

    /// The rule's word on a reported entry.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum TriggerEntryCode {
        /// Relayed to the cell's host alone (`host`).
        Relay,
        /// E_ENTRY_NO_REF: no world object the server holds or places, or
        /// one it cannot place in a cell.
        NoRef,
        /// E_ENTRY_ACTOR: the trigger is an actor.
        Actor,
        /// E_ENTRY_PRESSER: the presser is not the reporter's own actor nor
        /// one it simulates.
        Presser,
        /// E_ENTRY_NOT_LOADER: the reporter has not loaded the cell.
        NotLoader,
        /// E_ENTRY_HOST: the reporter hosts the cell, its own engine ran the
        /// script.
        Host,
        /// E_ENTRY_NO_HOST: nobody hosts the cell, no engine runs the script.
        NoHost,
        /// E_ENTRY_REACH: the presser stands outside the trigger's
        /// activation reach.
        Reach,
        /// E_ENTRY_UNPAIRED: a leave with no accepted enter of the same
        /// presser into the same trigger to end.
        Unpaired,
    }

    /// The verdict on one reported entry.
    #[derive(Debug)]
    struct TriggerEntryVerdict {
        /// Which word.
        code: TriggerEntryCode,
        /// The refusal's stable name for the log, empty when relayed.
        name: String,
        /// Why, for the log; empty when relayed.
        why: String,
        /// The host's actor to relay to (relayed), 0 otherwise.
        host: u32,
    }

    /// What the core knows of a reported world object's graph event's
    /// reference (ObjectAnimation 71; the fifty fixes' furniture rule,
    /// wire-rules object_animation::Facts).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct ObjectAnimationFacts {
        /// The reporter's actor.
        reporter: u32,
        /// The reference's base is a furniture (FURN).
        furniture: bool,
        /// The furniture's recorded occupant, 0 for none.
        occupant: u32,
        /// The occupant's owner: the occupant itself for a player's actor,
        /// an NPC's simulator, 0 for nobody.
        occupant_owner: u32,
        /// The reference's cell.
        cell: u32,
        /// That cell's host, 0 while nobody hosts it.
        host: u32,
    }

    /// The rule's word on a reported graph event.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    enum ObjectAnimationCode {
        /// Relayed: the cell's host's.
        Host,
        /// Relayed: a furniture's user's (its occupant's seat or simulator).
        User,
        /// E_OBJANIM_NOT_HOST: a reference nobody uses, and the reporter does
        /// not host its cell.
        NotHost,
        /// E_OBJANIM_NOT_USER: a furniture in use, and the reporter is not
        /// its user's owner.
        NotUser,
    }

    /// The verdict on one reported graph event.
    #[derive(Debug)]
    struct ObjectAnimationVerdict {
        /// Which word.
        code: ObjectAnimationCode,
        /// The refusal's stable name for the log, empty when relayed.
        name: String,
        /// Why, for the log; empty when relayed.
        why: String,
        /// Relayed and kept as the reference's lastAnimation: false for a
        /// furniture and for a refusal.
        keep: bool,
    }

    /// One reference alias of a quest as the game files hold it (thuum
    /// ADR-068 rung 4, amendment B read 8; wire-rules alias_fills::Alias),
    /// the form ids in the load order.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct QuestAliasRow {
        /// The alias's id (ALST).
        id: u32,
        /// The base it creates (ALCO), 0 for none.
        created: u32,
        /// Where it is created (ALCA): the top bit "in", the target alias's
        /// id in the low half.
        create_at: u32,
        /// The forced reference (ALFR), 0 for none.
        forced: u32,
        /// The unique actor's base (ALUA), 0 for none.
        unique_actor: u32,
    }

    /// A created alias's item and where it goes (wire-rules
    /// alias_fills::Creation).
    #[derive(Debug, Clone, PartialEq, Eq)]
    struct AliasCreationRow {
        /// The created alias's id.
        alias: u32,
        /// The base it creates.
        base: u32,
        /// The target alias's id.
        target: u32,
        /// 0 unresolved (E_ALIASFILL_UNRESOLVED, `why`), 1 a forced
        /// reference (`container` the reference), 2 a unique actor
        /// (`container` the actor's base).
        container_kind: u8,
        /// The reference or the base, per `container_kind`; 0 unresolved.
        container: u32,
        /// Why the container is unresolved, empty otherwise.
        why: String,
    }

    /// A created alias's mark on the journal (wire-rules alias_fills::Mark).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct AliasMarkRow {
        /// The QUST.
        quest: u32,
        /// The created alias's id.
        alias: u32,
        /// The item was added.
        added: bool,
    }

    /// One quest alias's fill on the journal (thuum ADR-068 rung 4,
    /// wire-rules alias_fills::Fill), the forms in the load order.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct AliasFillEntry {
        /// The QUST.
        quest: u32,
        /// The alias's id.
        alias: u32,
        /// The reference filling it, 0 when cleared.
        refr: u32,
    }

    /// What the core knows of a reported alias fill (wire-rules
    /// alias_fills::Facts).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct AliasFillFacts {
        /// The quest is a QUST record.
        is_quest: bool,
        /// The quest's record has a reference alias of that id.
        has_alias: bool,
        /// The fill's form is a reference (true for a clear).
        refr_is_ref: bool,
    }

    /// The rule's word on a reported alias fill.
    #[derive(Debug, Clone, PartialEq, Eq)]
    struct AliasFillVerdict {
        /// 0 recorded (merge and relay), 1 folded (nothing), 2 refused.
        code: u8,
        /// The refusal's stable name, empty otherwise.
        name: String,
        /// Why, for the log; empty otherwise.
        why: String,
    }

    /// One world global's value (wire-rules world_globals::Value).
    #[derive(Debug, Clone, Copy, PartialEq)]
    struct WorldGlobalValue {
        /// The GLOB's form id.
        global: u32,
        /// Its value.
        value: f32,
    }

    /// What the core knows of one reported world global (wire-rules
    /// world_globals::EntryFacts).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct WorldGlobalFacts {
        /// The GLOB's form id.
        global: u32,
        /// The form is a GLOB record.
        is_glob: bool,
        /// The record flags it constant.
        constant: bool,
        /// It is on the playerGlobals list.
        player_listed: bool,
    }

    /// The record after a report's accepted entries, and what moved.
    #[derive(Debug, Clone, PartialEq)]
    struct WorldGlobalsMerge {
        /// The record after.
        record: Vec<WorldGlobalValue>,
        /// The entries that moved it, to relay.
        changed: Vec<WorldGlobalValue>,
    }

    /// The ref state's rule on one call (thuum ADR-068 rung 4, wire-rules
    /// ref_state).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct RefStateVerdict {
        /// 0 recorded (write, stamp and relay), 1 folded (nothing), 2 the
        /// native is none of the five.
        code: u8,
        /// The reference is disabled after the call.
        disabled: bool,
        /// The reference is deleted after the call.
        deleted: bool,
    }

    /// One faction's rank on an NPC's record (wire-rules
    /// standing::FactionRank).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct StandingFaction {
        /// The FACT.
        faction: u32,
        /// Its rank.
        rank: i8,
    }

    /// One relationship rank on an NPC's record (wire-rules
    /// standing::Relationship).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct StandingRelation {
        /// The other actor.
        other: u32,
        /// The rank.
        rank: i8,
    }

    /// The standing's rule on one call (wire-rules standing::judge).
    #[derive(Debug, Clone, PartialEq, Eq)]
    struct StandingVerdict {
        /// 0 recorded, 1 folded, 2 refused (`name`, `why`), 3 the native is
        /// none of the six.
        code: u8,
        /// The refusal's stable name, empty otherwise.
        name: String,
        /// Why, for the log; empty otherwise.
        why: String,
        /// The change a recorded call makes, the applier the receivers run:
        /// 1 SetFactionRank(form, rank), 2 RemoveFromFaction(form), 3
        /// RemoveFromAllFactions(), 4 SetCrimeFaction(form, 0 for None), 5
        /// SetRelationshipRank(form, rank).
        change: u8,
        /// The faction, or the relationship's other actor.
        form: u32,
        /// The rank.
        rank: i8,
    }

    /// A script set opened, or why not (thuum ADR-068 rung 4, wire-rules
    /// placements' script set).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct ScriptSetOpen {
        /// The set's id when opened.
        set: u64,
        /// 0 opened, 1 E_PLACED_KIND (no base a script set places), 2 the
        /// world's script sets are at their cap, 3 the anchor is not a
        /// number.
        refusal: u8,
    }

    /// A script set's request to its reporter (the PlacedRequest).
    #[derive(Debug, Clone, PartialEq, Eq)]
    struct ScriptRequestRow {
        /// The set's id.
        set: u64,
        /// The reporter asked.
        caster: u32,
        /// The call's self, the anchor.
        self_id: u32,
        /// The base's form id within its plugin.
        short_id: u32,
        /// The base's plugin.
        file: String,
        /// The base's record type.
        record_type: String,
        /// How long the report is taken, milliseconds.
        window_ms: u64,
    }

    /// What a script set came to (wire-rules placements::ScriptOutcome).
    #[derive(Debug, Clone, PartialEq)]
    struct ScriptPlacementRow {
        /// The report came in (place the object); false, the window closed
        /// with none (nothing placed).
        placed: bool,
        /// The set's id.
        set: u64,
        /// The reporter.
        caster: u32,
        /// The call's self.
        self_id: u32,
        /// The base's form id within its plugin.
        short_id: u32,
        /// The base's plugin.
        file: String,
        /// The base's record type.
        record_type: String,
        /// Position, as reported.
        x: f32,
        /// Position.
        y: f32,
        /// Position.
        z: f32,
        /// The Z angle, degrees.
        yaw: f32,
    }

    /// The world streamer after an event (thuum ADR-068 rung 4, wire-rules
    /// world_streamer).
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct StreamerElection {
        /// An election ran: send the stream words.
        changed: bool,
        /// The streaming seat's player, 0 for none.
        streamer: u32,
        /// The epoch.
        epoch: u32,
        /// The seat that streamed before the election, 0 for none.
        previous: u32,
    }

    /// One quest's run on the world's record (thuum ADR-068 rung 5,
    /// wire-rules quest_runs::QuestRun), the quest by its FormDesc's parts:
    /// what the journal's questRuns keeps and serves.
    #[derive(Debug, Clone, PartialEq, Eq)]
    struct QuestRunRow {
        /// The quest's form id within its plugin.
        short_id: u32,
        /// The plugin, as the load order names it.
        file: String,
        /// Recorded running: its last word a start.
        running: bool,
        /// The record's sequence for the quest: how many words it took, 1
        /// at its first.
        seq: u32,
    }

    /// The rule's word on one run word (wire-rules quest_runs::Verdict).
    #[derive(Debug, Clone, PartialEq, Eq)]
    struct QuestRunVerdict {
        /// 0 recorded (merge with quest_runs_merge, then relay the word to
        /// every other seat), 1 folded (nothing written or relayed), 2 held
        /// (the quest is a seat's churn, `E_QUEST_CHURN`: nothing written or
        /// relayed), 3 refused (`E_QUEST_RUNS_FULL`), 4 refused by its cause
        /// (D-Q4 C, `E_QUEST_CAUSE`: the seat's own), 5 refused as a Story
        /// Manager instance (`questStoryInstances` "seat",
        /// `E_QUEST_INSTANCE`); nothing written, relayed or counted for 3 to
        /// 5.
        code: u8,
        /// Held (code 2) by this very word: drop the quest's entry
        /// (quest_runs_without) and log E_QUEST_CHURN; false for a later
        /// word of a held quest, which the core says at most once a minute
        /// per seat.
        first: bool,
        /// The stable name for codes 2 to 5, empty otherwise.
        name: String,
        /// Why, for the log, codes 3 to 5; empty otherwise.
        why: String,
    }

    /// What a login sends around the stages (wire-rules
    /// quest_runs::QuestRuns::at_login): served start words for the quests
    /// recorded running and for those recorded stopped whose stages the login
    /// replays (D-Q3 A), ahead of the stages, and served stop words for those
    /// recorded stopped, behind them (stage 0 in each entry).
    #[derive(Debug, Clone, PartialEq)]
    struct QuestRunsLogin {
        /// The quests recorded running and the stopped ones whose stages the
        /// login replays, in the record's order.
        starts: Vec<QuestStageEntry>,
        /// The quests recorded stopped, in the record's order.
        stops: Vec<QuestStageEntry>,
    }

    /// One alias of a quest for its class (thuum ADR-068 rung 5, wire-rules
    /// quest_scope::Alias), as libespm's QUST::GetQuestAliases reads it, the
    /// forms in the load order.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct QuestScopeAliasRow {
        /// The alias's id (ALST, or ALLS's for a location alias).
        id: u32,
        /// A location alias (libespm's `locationAlias`, ALLS).
        location: bool,
        /// The forced reference (ALFR, `aliasForcedReference`), 0 for none.
        forced: u32,
        /// The unique actor's base (ALUA, `aliasUniqueActor`), 0 for none.
        unique_actor: u32,
        /// The base it creates (ALCO, `aliasCreatedObject`), 0 for none.
        created: u32,
        /// Where it is created (ALCA, `createAt`): the top bit "in", the
        /// target alias's id in the low half.
        create_at: u32,
        /// It fills from another quest's alias (ALEQ `externalAliasReference`
        /// a quest of the load order and ALEA `externalAlias` one of its
        /// alias ids) that the core found.
        external: bool,
        /// That external alias's own ALFR, 0 for none or no external.
        external_forced: u32,
        /// That external alias's own ALUA, 0 for none or no external.
        external_unique_actor: u32,
    }

    /// One condition-read variable's key on the watch list a login names
    /// (thuum ADR-068 rung 5, ScriptVariables 81's `watch`; wire-rules
    /// script_variables::Key), its form by its FormDesc's parts.
    #[derive(Debug, Clone, PartialEq, Eq)]
    struct ScriptVarKeyRow {
        /// The QUST's or REFR's form id within its plugin.
        short_id: u32,
        /// The plugin, as the load order names it.
        file: String,
        /// The attached script whose chain declares the variable.
        script: String,
        /// The variable, as the condition names it.
        var: String,
        /// The key is its player's own (its form a player-class quest while
        /// the scope acts): the seat reports it flagged own, and its value
        /// is the player's alone.
        own: bool,
    }

    /// One variable's entry (thuum ADR-068 rung 5, ScriptVariables 81's
    /// `entries`; wire-rules script_variables::Entry): a report's, or a
    /// record's (the journal's scriptVariables for the world's keys, a
    /// player's change form's for its own), the form by its FormDesc's
    /// parts. The value is the one field `k` names.
    #[derive(Debug, Clone, PartialEq)]
    struct ScriptVarRow {
        /// The QUST's or REFR's form id within its plugin.
        short_id: u32,
        /// The plugin, as the load order names it.
        file: String,
        /// The attached script whose chain declares the variable.
        script: String,
        /// The variable.
        var: String,
        /// A player's own key: flagged own by the report, and a row of a
        /// player's record.
        own: bool,
        /// The folded kind: 3 an int, 4 a float, 5 a bool.
        k: u8,
        /// The int (k 3), else 0.
        i: i32,
        /// The float (k 4), else 0.
        f: f32,
        /// The bool (k 5), else false.
        b: bool,
        /// The record's sequence for the key (1 at its first write); 0 on a
        /// report.
        seq: u32,
    }

    /// The rule's word on one reported entry (wire-rules
    /// script_variables::Verdict).
    #[derive(Debug, Clone, PartialEq, Eq)]
    struct ScriptVarVerdict {
        /// 0 recorded (merge, then relay a world key's entry or keep an own
        /// key's), 1 folded (nothing), 2 refused.
        code: u8,
        /// The refusal's stable name (E_SCRIPTVAR_UNLISTED, _PLAYER, _KIND,
        /// _FULL), empty otherwise.
        name: String,
        /// Why, for the log; empty otherwise.
        why: String,
    }

    /// One ScriptVariables 81 report judged whole (thuum ADR-068 rung 5,
    /// the fresh review's note 13: one bridge call a message): a verdict
    /// per entry, in order, and the two records after the report.
    #[derive(Debug, Clone, PartialEq)]
    struct ScriptVarReport {
        /// Per entry, in order: 0 recorded, 1 folded, 2 refused (with the
        /// refusal's name and why).
        verdicts: Vec<ScriptVarVerdict>,
        /// Per entry: the key's sequence after a recorded entry, which the
        /// relay of a world key's carries; 0 otherwise.
        seqs: Vec<u32>,
        /// The world's record after the report (the journal's).
        world: Vec<ScriptVarRow>,
        /// The reporter's player's own record after it (its change form's).
        own: Vec<ScriptVarRow>,
    }

    /// One object the calling script's attachment names (thuum ADR-068 rung
    /// 5, wire-rules player_own::Named): from libespm's VMAD for each
    /// attachment of the ScriptCall's `caller` script live for the reporter
    /// (PartOne::ScriptAttachmentsOf, the fresh review's note 3: a QUST the
    /// scope classes the player's or the journal records running or staged,
    /// an MGEF a SPEL the player holds carries), an object property's value
    /// (Property::Type::Object 1) or each element of an object array
    /// property's (ObjectArray 11), with a FormList's entries when the form
    /// is a FLST (FLST::GetData's formIds, in order).
    #[derive(Debug, Clone, PartialEq, Eq)]
    struct ScriptPropertyRow {
        /// The form the property names.
        form: u32,
        /// Its FormList entries in order, empty for any other form.
        members: Vec<u32>,
    }

    /// What the core knows of one player's own call beside its form
    /// (wire-rules player_own::Facts).
    #[derive(Debug, Clone, Copy, PartialEq)]
    struct PlayerOwnFacts {
        /// The player's race on its change form (its appearance's raceId;
        /// an overlay never moves it).
        recorded_race: u32,
        /// SetRace's form is a RACE with the Playable flag.
        playable: bool,
        /// SetRace's form is on `restrictedRaces`.
        restricted: bool,
        /// The player's profile is on that race's permit list.
        permitted: bool,
        /// The player's record holds the form: a learned spell (AddSpell,
        /// RemoveSpell), a learned shout (AddShout, RemoveShout), the item in
        /// its inventory (EquipItem).
        held: bool,
        /// The form's record is the row's kind: a RACE (SetRace), a SPEL
        /// (AddSpell, RemoveSpell), a SHOU (AddShout, RemoveShout), an item
        /// of libespm's item types (EquipItem), an HDPT (ChangeHeadPart).
        kind: bool,
        /// ChangeHeadPart's head part may be worn by the player's recorded
        /// race: its HDPT's valid-race FormList (RNAM) holds the race (Eli's
        /// D-E1 A).
        race_valid: bool,
        /// AddItem's form is a leveled item list, an LVLI record (Eli's
        /// D-R1 C: refused by name, the engine's roll unreported).
        leveled: bool,
        /// How many of the form the player's record holds over all its
        /// stacks (Inventory::GetItemCount), for AddItem and RemoveItem.
        held_count: u32,
        /// AddItem's and RemoveItem's second argument, aiCount, as the wire
        /// carried it (a number; 0 for none), judged by wire-rules
        /// player_own::item_count.
        count: f64,
        /// RemoveItem's fourth argument, akOtherContainer, names a reference
        /// the reporter occupies (MpObjectReference::IsContainerOccupant):
        /// its PutItem carries the move.
        into_held_open: bool,
        /// How many stacks the player's record holds (its Inventory's
        /// entries), for AddItem's bound (wire-rules
        /// player_own::INVENTORY_STACKS, the wire's cap::INVENTORY).
        stacks: usize,
        /// AddItem's form joins no stack the record holds (no entry of the
        /// form with no extra data, unworn: Inventory::AddItems' match for a
        /// plain add), so the add appends a new one.
        new_stack: bool,
        /// For one landing of a leveled AddItem's roll (ScriptCall's
        /// `landed`, SCHEMA_VERSION 44; thuum D-R1 C's leveled gap, train
        /// 52), judged with the landing's base as the form: the leveled list
        /// the call named. 0 for every other call.
        landing_of: u32,
        /// The landing's base is one that list can roll, a non-list base
        /// among its entries with nested lists followed
        /// (PartOne::LeveledReach over LeveledListUtils::Reach; D-R2 B).
        in_reach: bool,
    }

    /// A player's overlay (thuum ADR-068 D-T1 A, wire-rules
    /// player_own::Overlay): the race it wears beyond its record and what
    /// its scripts added meanwhile, which the core carries on the figure's
    /// CreateActor and UpdateAppearance and counts at the equipment gate,
    /// never on the change form.
    #[derive(Debug, Clone, PartialEq, Eq)]
    struct PlayerOverlayRow {
        /// An overlay is open; every other field is empty when false.
        open: bool,
        /// The race worn.
        race: u32,
        /// The recorded race when it opened, the race its end returns to.
        base: u32,
        /// Spells added while open, in order.
        spells: Vec<u32>,
        /// Shouts added while open, in order.
        shouts: Vec<u32>,
        /// Items equipped while open, in order.
        items: Vec<u32>,
    }

    /// The rule's word on one player's own call (wire-rules
    /// player_own::Verdict).
    #[derive(Debug, Clone, PartialEq, Eq)]
    struct PlayerOwnVerdict {
        /// 0 recorded on the change form (the race, a learned spell or shout
        /// added or removed, a head part; AddItem's and RemoveItem's `count`
        /// items, D-R1 C), 1 held in the overlay (opened, its race moved, a
        /// form added or removed), 2 the overlay ended at the race's return,
        /// 3 folded (nothing), 4 refused.
        code: u8,
        /// The refusal's stable name (E_SCRIPTCALL_ARGS, a form of another
        /// kind than the row's or a count that lands nothing;
        /// E_SCRIPTCALL_UNNAMED; E_SCRIPTCALL_RACE; E_SCRIPTCALL_LEVELED, a
        /// leveled list's add that carries no landing; E_SCRIPTCALL_FULL, an
        /// add needing a new stack once the record holds the wire's stacks
        /// in one inventory; E_SCRIPTCALL_ROLL, a landing whose base the
        /// list cannot roll; or E_SCRIPTCALL_ROW for a native that is none
        /// of the nine), empty otherwise.
        name: String,
        /// Why, for the log; empty otherwise.
        why: String,
        /// The overlay that ended (code 2, or code 0 when a twin or a
        /// Playable race was taken while one was open), its adds to strip;
        /// `open` false when none ended.
        ended: PlayerOverlayRow,
        /// The player's overlay after the call, `open` false for none.
        overlay: PlayerOverlayRow,
        /// Code 0 of AddItem or RemoveItem (D-R1 C): how many of the form
        /// the core adds to or removes from the player's inventory; 0
        /// otherwise.
        count: u32,
    }

    extern "Rust" {
        /// A client's first activation of a target `distance` units away,
        /// `target_size` being its bounds' farthest point times its scale.
        fn activation_within_reach(distance: f32, target_size: f32) -> Verdict;
        /// Whether the race menu's result may set the race.
        fn race_allowed(facts: &RaceFacts) -> bool;
        /// Whether a player's melee hit on a player is within reach.
        fn melee_within_reach(facts: &MeleeFacts) -> Verdict;
        /// Whether a player's melee hit on a player is within the attacker's
        /// cone (the bound in degrees off its heading).
        fn melee_within_cone(facts: &ConeFacts) -> Verdict;
        /// The hit flags the server keeps.
        fn backed_flags(facts: &FlagFacts) -> Flags;
        /// A kept sneak attack's damage multiplier for a weapon of
        /// `anim_type` (the WEAP record's DNAM animation type).
        fn sneak_mult(anim_type: u8, mults: &SneakMults) -> f32;
        /// thuum docs/verbs/blocking.md: the share of a hit's damage the
        /// target's block stops (0 when it is not blocked).
        fn blocked_share(settings: &BlockSettings, facts: &BlockFacts) -> f32;
        /// thuum docs/verbs/stagger.md: the stagger the victim's game plays
        /// (0 for none; at most 1); `roll` is 0 to 99, the caller's.
        fn stagger_magnitude(settings: &StaggerSettings, facts: &StaggerFacts, roll: u8) -> f32;
        /// Whether an accepted hit is one a fight counts: a player on one
        /// side, a player or a hosted NPC on the other.
        fn hostility_counted(facts: &HostilityFacts) -> bool;
        /// Whether two players stand farther apart than the engine's
        /// "enemies nearby" range where they are (`interior`); players in
        /// different cells or worlds pass an infinite distance.
        fn hostility_apart(distance: f32, interior: bool) -> bool;
        /// TES3MP's rest switches from server-settings.json's `rest` block.
        type RestSettings;
        /// The switches from the block as JSON text (`{}` for both on); the
        /// error names the bad key.
        fn new_rest_settings(settings_json: &str) -> Result<Box<RestSettings>>;
        /// Whether a player's rest is let through, and why not.
        fn rest_check(settings: &RestSettings, facts: &RestFacts) -> RestRefusal;
        /// An attribute's percentage after a rest of `hours`.
        fn rest_after(regen: &Regen, hours: f32) -> f32;
        /// A host's state report for an actor it simulates, judged value by
        /// value against the server's numbers (thuum docs/verbs/npc-stream.md).
        fn hosted_state_judge(facts: &HostedStateFacts) -> HostedStateVerdict;
        /// Whether a rest was a sleep, from the server's own bed facts.
        fn rest_slept(facts: &BedFacts) -> bool;
        /// Real milliseconds the Rested bonus of a rest lasts at the
        /// clock's `time_scale`; 0 when it earns none.
        fn rested_ms(slept: bool, time_scale: f32) -> u64;
        /// The seconds a rest of `hours` runs the resting player's magic
        /// effects (thuum docs/verbs/self-cast.md).
        fn rest_effect_seconds(hours: f32) -> f32;

        /// The marker a discovery reported at the player's position means:
        /// the nearest candidate within the discovery range.
        fn map_marker_discovered(x: f32, y: f32, z: f32, candidates: &[MarkerCandidate]) -> MarkerChoice;
        /// A recorded marker's travel flag after another report of it.
        fn map_marker_travel_after(recorded: bool, reported: bool) -> bool;
        /// How far from the player a discovered marker may lie, so the core
        /// knows how many grid cells to gather candidates from.
        fn map_marker_range() -> f32;
        /// Whether an ingredient effects report is kept: the player ate that
        /// same ingredient (`ate_same`), `since_eat_ms` ago (`has_eat` false
        /// when it ate none).
        fn ingredient_effects_kept(ate_same: bool, has_eat: bool, since_eat_ms: u64) -> bool;
        /// The recorded mask after a kept report.
        fn ingredient_effects_union(recorded: u8, reported: u8) -> u8;
        /// The favorites a report keeps, in its order: held items and magic,
        /// the first entry per form, the first claim per key, at most 128.
        fn favorites_kept(report: &[FavoriteFacts]) -> Vec<FavoriteEntry>;
        /// Whether a RaceMenu preset is one the server records: a JSON
        /// object, not nested past what a preset needs.
        fn racemenu_preset_ok(preset: &str) -> bool;
        /// thuum docs/verbs/console-commands.md: what the server does with a
        /// command a caller of a staff rank (0 to 3) sent.
        fn console_decide(name: &str, rank: u8) -> ConsoleDecision;
        /// The line the caller's console prints for a decision other than Run.
        fn console_refusal_line(decision: ConsoleDecision) -> String;
        /// thuum ADR-026: what a look says of the vanilla appearance.
        fn racemenu_look_facts(preset: &str) -> LookAppearance;
        /// thuum ADR-026: the appearance's head parts a look implies.
        fn racemenu_derived_head_parts(parts: &[LookPartFacts]) -> DerivedParts;
        /// Eli's D-E1 A (ADR-026's one authority): the stored look with the
        /// head part `plugin`|`id` (its plugin's file name, its id within
        /// the plugin) in place of its part of the same HDPT type, `same_type`
        /// one byte per `headParts` entry in order, nonzero for a part of
        /// that type (wire-rules racemenu::with_head_part); empty when
        /// nothing changes or the look would pass the wire's preset cap.
        fn racemenu_with_head_part(preset: &str, same_type: &[u8], plugin: &str, id: u32) -> String;
        /// Whether an actor values report is within bounds, given the
        /// values the server holds (thuum docs/verbs/actor-values.md).
        fn actor_values_report_ok(report: &AvSnapshot, held: &[AvBase]) -> bool;
        /// The record after a kept report, and the holds still standing.
        fn actor_values_merge(record: &AvSnapshot, report: &AvSnapshot, held: &[AvBase]) -> AvMerge;
        /// Whether a report shows the record a login sent as applied.
        fn actor_values_login_applied(record: &AvSnapshot, report: &AvSnapshot) -> bool;
        /// The recorded skills and attributes the report falls under
        /// (wire-rules actor_values::login_shortfall), for the refusal's log.
        fn actor_values_login_shortfall(record: &AvSnapshot, report: &AvSnapshot) -> Vec<AvBase>;
        /// An actor value's index from its Papyrus name, case aside; -1 for
        /// a name the lab confirmed no index for (thuum docs/verbs/actor-values.md).
        fn actor_value_index(name: &str) -> i16;
        /// Whether the server may set an actor value's base to a value (R0).
        fn actor_values_set_ok(av: u8, value: f32) -> bool;
        /// A skill actor value's progress index (0 to 17), -1 for any other
        /// value: a server set of a skill moves the record's progress entry
        /// with the base (thuum docs/verbs/leveling.md).
        fn actor_values_skill_index(av: u8) -> i16;
        /// Arms a hold on a percentage the server raised (thuum
        /// docs/verbs/attributes.md).
        fn percentages_hold(holds: &mut Vec<PercentHold>, av: u8, value: f32, now_ms: u64);
        /// Whether a client's report of a percentage is taken: false while
        /// a hold on it stands and the report does not reach the held value.
        fn percentages_report(holds: &mut Vec<PercentHold>, av: u8, reported: f32, now_ms: u64) -> bool;
        /// How far a percentage must move to count as raised or applied.
        fn attributes_applied_within() -> f32;
        /// How long a Self concentration cast's effects run on the server
        /// without the cast's end message (wire-rules magic::SELF_STREAM_CAP_S).
        fn self_stream_cap_seconds() -> f32;

        /// The fights between players going on (thuum ADR-023).
        type Fights;
        /// No fights.
        fn new_fights() -> Box<Fights>;
        /// An accepted hit between two players at `now_ms` (a monotonic
        /// clock): true when it begins a fight, so the victim's game is to
        /// be told.
        fn hit(self: &mut Fights, aggressor: u32, victim: u32, now_ms: u64) -> bool;
        /// The pairs fighting, for the core to measure.
        fn pairs(self: &Fights) -> Vec<FightPair>;
        /// The core's measure of a pair now: true when the fight is over (a
        /// minute without a hit, or apart for a few seconds), which forgets it.
        fn check(self: &mut Fights, a: u32, b: u32, apart: bool, now_ms: u64) -> bool;
        /// Whether the actor is in a fight.
        fn in_fight(self: &Fights, actor: u32) -> bool;
        /// Whether the two actors are in a fight with each other
        /// (IsHostileToActor between players, thuum ADR-039).
        fn between(self: &Fights, a: u32, b: u32) -> bool;
        /// The actor is gone: its fights end without a notice.
        fn forget(self: &mut Fights, actor: u32);

        /// Every player actor's ground speed budget.
        type MovementBudgets;
        /// Empty budgets: each actor starts full.
        fn new_movement_budgets() -> Box<MovementBudgets>;
        /// Charge `actor` a move of `ground` units at `now_ms` (a monotonic
        /// clock); true when it fits.
        fn spend(self: &mut MovementBudgets, actor: u32, ground: f32, now_ms: u64) -> bool;
        /// Drop an actor's budget and permitted jump.
        fn forget(self: &mut MovementBudgets, actor: u32);
        /// thuum docs/verbs/console-commands.md, COC: permit `actor` one
        /// jump into an interior cell until a minute after `now_ms`.
        fn permit_jump_interior(self: &mut MovementBudgets, actor: u32, cell: u32, now_ms: u64);
        /// The same into an exterior cell: its worldspace and grid square.
        fn permit_jump_exterior(self: &mut MovementBudgets, actor: u32, world: u32, x: i16, y: i16, now_ms: u64);
        /// Judge a move of `actor`'s that the bounds refuse (another cell,
        /// or a jump) against its permit; a landing ends the permit.
        fn check_jump(self: &mut MovementBudgets, actor: u32, cell_or_world: u32, x: f32, y: f32, now_ms: u64) -> JumpCheck;
        /// The server moved `actor` itself, to (x, y) in `cell_or_world`:
        /// its reports from elsewhere were sent before the move until one
        /// comes from there, or ten seconds pass.
        fn expect_arrival(self: &mut MovementBudgets, actor: u32, cell_or_world: u32, x: f32, y: f32, now_ms: u64);
        /// Judge any report of `actor`'s against the arrival the server
        /// expects; the arrival ends the wait.
        fn check_arrival(self: &mut MovementBudgets, actor: u32, cell_or_world: u32, x: f32, y: f32, now_ms: u64) -> JumpCheck;
        /// Hold `actor` where the server put it until `until_ms` (thuum
        /// docs/verbs/shadowbond.md: the swap against the mod's own script).
        fn pin(self: &mut MovementBudgets, actor: u32, cell_or_world: u32, x: f32, y: f32, radius: f32, until_ms: u64);
        /// Whether a report leaves the pinned place while the pin holds.
        fn pinned_away(self: &mut MovementBudgets, actor: u32, cell_or_world: u32, x: f32, y: f32, now_ms: u64) -> bool;
        /// thuum docs/verbs/cast-teleport.md: `actor` cast a teleport at
        /// `now_ms`, standing at (x, y, z) in `cell_or_world`: one jump to
        /// within `reach` of there (0: back there, docs/verbs/ghostwalk.md)
        /// is permitted for `wait_ms`.
        #[allow(clippy::too_many_arguments)]
        fn permit_cast_jump(self: &mut MovementBudgets, actor: u32, cell_or_world: u32, x: f32, y: f32, z: f32, reach: f32, wait_ms: u64, now_ms: u64);
        /// Judge a move of `actor`'s that the bounds refuse against its cast
        /// jump: Landed within reach (that ends it), else NoPermit.
        fn check_cast_jump(self: &mut MovementBudgets, actor: u32, cell_or_world: u32, x: f32, y: f32, z: f32, now_ms: u64) -> JumpCheck;
        /// Whether a move of `actor`'s that the bounds took lands within the
        /// cast jump that waits for it; the jump stands. The other games
        /// place the figure there (thuum docs/verbs/cast-teleport.md, the
        /// teleport snap; docs/verbs/shadowbond.md).
        fn cast_jump_lands(self: &MovementBudgets, actor: u32, cell_or_world: u32, x: f32, y: f32, z: f32, now_ms: u64) -> bool;

        /// thuum docs/verbs/headshot.md: the headshot rule's settings
        /// (server-settings.json's `headshot` block) and every shooter's
        /// recent claimed bow hits.
        type Headshots;
        /// The block as JSON text (`{}` for none); the error names the bad
        /// entry.
        fn new_headshots(settings_json: &str) -> Result<Box<Headshots>>;
        /// Whether the spell `short_id` in `file` reports a headshot.
        fn is_headshot_spell(self: &Headshots, short_id: u32, file: &str) -> bool;
        /// A claimed bow hit by `actor` on `target` at `now_ms`.
        fn note_bow_hit(self: &mut Headshots, actor: u32, target: u32, now_ms: u64);
        /// A headshot report by `actor` on `target`, judged against its
        /// newest unspent bow hit and the facts; a kill spends the hit.
        fn claim(self: &mut Headshots, actor: u32, target: u32, facts: &HeadshotFacts, now_ms: u64) -> HeadshotCheck;
        /// The actor is gone: its bow hits with it.
        fn forget(self: &mut Headshots, actor: u32);

        /// thuum docs/verbs/scripted-effects.md: the spells
        /// server-settings.json's `scriptedEffects` names, whose scripts'
        /// work the server does itself from the claimed hit.
        type ScriptedEffects;
        /// The list from the block as JSON text (`[]` for none); the error
        /// names the bad entry.
        fn new_scripted_effects(settings_json: &str) -> Result<Box<ScriptedEffects>>;
        /// Whether the spell `short_id` in `file` is a scripted kill.
        fn is_kill_spell(self: &ScriptedEffects, short_id: u32, file: &str) -> bool;
        /// The percentage the spell's entry carries (the script's global as
        /// the mod ships it when the entry says nothing).
        fn kill_percentage(self: &ScriptedEffects, short_id: u32, file: &str) -> f32;
        /// Finger of Death's arithmetic over the target's and the caster's
        /// current Health and the percentage.
        fn finger_of_death(target_health: f32, caster_health: f32, percentage: f32) -> DeathBlast;

        /// thuum docs/verbs/cast-teleport.md: the spells server-settings.json's
        /// `castTeleports` names as teleports, each with its reach.
        type CastTeleports;
        /// The list from the block as JSON text (`[]` for none); the error
        /// names the bad entry.
        fn new_cast_teleports(settings_json: &str) -> Result<Box<CastTeleports>>;
        /// The reach of the spell `short_id` in `file` (a FormDesc's
        /// parts), or a negative number when the server names it no
        /// teleport.
        fn teleport_reach(self: &CastTeleports, short_id: u32, file: &str) -> f32;
        /// How long after its cast the spell's jump is permitted, ms (0 when
        /// the server names it no teleport).
        fn teleport_wait_ms(self: &CastTeleports, short_id: u32, file: &str) -> u64;
        /// Whether the spell's cast is still relayed to the neighbours
        /// (false when the server names it no teleport: then the cast is
        /// any spell's, relayed as ever).
        fn teleport_relays(self: &CastTeleports, short_id: u32, file: &str) -> bool;
        /// What the spell's entry names for the other players' games to play
        /// on its caster's figure (every `has_` false without a look).
        fn teleport_look(self: &CastTeleports, short_id: u32, file: &str) -> TeleportLook;
        /// Whether a move of `step` units over the ground, which the
        /// movement bounds took while a teleport's look waits, is its
        /// landing (wire-rules teleports::LOOK_LANDING_STEP).
        fn teleport_look_lands(step: f32) -> bool;

        /// thuum docs/verbs/shadowbond.md: the spells server-settings.json's
        /// `bonds` names, and the bonds standing between actors.
        type Bonds;
        /// The list from the block as JSON text (`[]` for none); the error
        /// names the bad entry.
        fn new_bonds(settings_json: &str) -> Result<Box<Bonds>>;
        /// The bond's duration for the spell `short_id` in `file`, ms, or 0
        /// when the server names it no bond.
        fn bond_duration_ms(self: &Bonds, short_id: u32, file: &str) -> u64;
        /// `caster`'s cast of the spell landed on `target` at `now_ms`: a
        /// bond stands for the spell's duration, returned (`found` false,
        /// and nothing recorded, for a spell the server names no bond).
        fn record(self: &mut Bonds, caster: u32, target: u32, short_id: u32, file: &str, now_ms: u64) -> BondBreak;
        /// A recast: the caster's standing bond, removed and returned
        /// (`found` false for none), before `record` replaces it.
        fn supersede(self: &mut Bonds, caster: u32, now_ms: u64) -> BondBreak;
        /// `actor` acted at `now_ms`: the bond it is in ends and is returned
        /// (`found` false when it is in none).
        fn break_of(self: &mut Bonds, actor: u32, now_ms: u64) -> BondBreak;
        /// The bonds that ran out by `now_ms`, each once.
        fn expired(self: &mut Bonds, now_ms: u64) -> Vec<BondBreak>;
        /// The actor is gone: its bonds with it.
        fn forget(self: &mut Bonds, actor: u32);

        /// thuum docs/verbs/player-globals.md: the globals server-settings.json's
        /// `playerGlobals` names, kept per player.
        type PlayerGlobalsList;
        /// The list from the block as JSON text (`[]` for none); the error
        /// names the bad entry.
        fn new_player_globals(settings_json: &str) -> Result<Box<PlayerGlobalsList>>;
        /// The listed globals, in the settings' order (values 0): what a
        /// client watches.
        fn player_globals_listed(self: &PlayerGlobalsList) -> Vec<PlayerGlobalEntry>;
        /// Judge a player's report: 0 taken, 1 a global not listed, 2 a value
        /// that is not finite, 3 a global twice, 4 more entries than listed.
        fn player_globals_check(self: &PlayerGlobalsList, report: &[PlayerGlobalEntry]) -> u8;
        /// What the login sends: the recorded values still listed.
        fn player_globals_at_login(self: &PlayerGlobalsList, record: &[PlayerGlobalEntry]) -> Vec<PlayerGlobalEntry>;
        /// memo D51 B: the recorded globals at or past their killAt, the server's word that the player dies
        fn player_globals_lethal(self: &PlayerGlobalsList, record: &[PlayerGlobalEntry]) -> Vec<PlayerGlobalEntry>;
        /// The record after a checked report: the latest value wins.
        fn player_globals_merge(record: &[PlayerGlobalEntry], report: &[PlayerGlobalEntry]) -> Vec<PlayerGlobalEntry>;

        /// thuum docs/verbs/quest-stage.md (ADR-056; memo D58 B): the quests
        /// server-settings.json's `sharedQuests` makes the world's, by a list
        /// (mode list) or by `{"mode": "all", "denied": [...]}` (mode all;
        /// rung 5 retired memo D62's `allowed`, read and ignored), less the
        /// player class composed in (shared_quests_set_player).
        type SharedQuests;
        /// The setting from the block as JSON text (`[]` for none); the
        /// error names the bad entry or shape.
        fn new_shared_quests(settings_json: &str) -> Result<Box<SharedQuests>>;
        /// The sharing mode as the wire carries it: 0 list, 1 all.
        fn shared_quests_mode(self: &SharedQuests) -> u8;
        /// What the login names, in the settings' order (stage 0): in mode
        /// list the listed quests (what a client reports) less the player
        /// class; in mode all the denied quests, then the player class's
        /// (what a client never reports: denied ∪ player).
        fn shared_quests_listed(self: &SharedQuests) -> Vec<QuestStageEntry>;
        /// Judge a report's quest, a stage's or a word's: 0 the world's, 1
        /// not listed (mode list, E_QUEST_UNLISTED), 2 denied (mode all,
        /// E_QUEST_DENIED), 3 a player-class quest (E_QUEST_PLAYER, thuum
        /// ADR-068 rung 5), the mode's own refusal first.
        fn shared_quests_check(self: &SharedQuests, short_id: u32, file: &str) -> u8;
        /// thuum ADR-068 rung 5 (read 4): the line the core logs once at load
        /// when the block still carries the retired `allowed` key, empty
        /// when it does not.
        fn shared_quests_retired(self: &SharedQuests) -> String;
        /// thuum ADR-068 rung 5 (D-Q1's derivation): compose the player
        /// class in, the quests quest_scope_class marks 1, each once
        /// (stage ignored), replacing any composed before. Call it at load
        /// and only while quest_scope_acting: "off" leaves the setting as
        /// written. A composed quest is never the world's in either mode,
        /// the login names it in mode all, its report is code 3, and its
        /// recorded stages and runs stay unsent at a login.
        fn shared_quests_set_player(self: &mut SharedQuests, quests: &[QuestStageEntry]);
        /// What the login sends: the recorded stages of quests still the
        /// world's, ascending within each quest.
        fn shared_quests_at_login(self: &SharedQuests, record: &[QuestStageEntry]) -> Vec<QuestStageEntry>;
        /// Whether the record holds the stage already (a report of it is a
        /// no-op).
        fn quest_stage_done(record: &[QuestStageEntry], done: &QuestStageEntry) -> bool;
        /// The record after a checked report: the stage added once, ascending
        /// within its quest; unchanged when the record is full.
        fn quest_stages_merge(record: &[QuestStageEntry], done: &QuestStageEntry) -> Vec<QuestStageEntry>;
        /// Whether the record is at wire-rules' QUEST_RECORD_CAP (4096
        /// pairs), so a new stage is refused (E_QUEST_FULL) and the merge
        /// would leave the record as it is.
        fn quest_record_full(record: &[QuestStageEntry]) -> bool;

        /// thuum docs/verbs/spell-placed-object.md (ADR-057): the objects
        /// server-settings.json's `spellPlacedObjects` names per spell, and
        /// every set standing in the world.
        type Placements;
        /// The table from the block as JSON text (`[]` for none); the error
        /// names the bad entry.
        fn new_placements(settings_json: &str) -> Result<Box<Placements>>;
        /// Whether the spell `short_id` in `file` (a FormDesc's parts) places
        /// objects.
        fn is_listed(self: &Placements, short_id: u32, file: &str) -> bool;
        /// The server accepted `cast` of the spell `short_id` in `file` at
        /// `now_ms` (a monotonic clock). True: the spell is listed and the
        /// cast waits for the placement.
        fn cast_started(self: &mut Placements, short_id: u32, file: &str, cast: PlacedCast, now_ms: u64) -> bool;
        /// `caster`'s stream in `hand` ended at `now_ms`.
        fn cast_ended(self: &mut Placements, caster: u32, hand: u8, now_ms: u64);
        /// `caster` is gone at `now_ms`: every hand's stream ends.
        fn caster_gone(self: &mut Placements, caster: u32, now_ms: u64);
        /// The objects to create at `now_ms`, each set's rows together, each
        /// set once.
        fn due_placements(self: &mut Placements, now_ms: u64) -> Vec<PlacementRow>;
        /// The sets whose time ran out by `now_ms`, each once.
        fn due_removals(self: &mut Placements, now_ms: u64) -> Vec<PlacementRemoval>;
        /// The casts that placed nothing since the last call, for the log.
        fn take_refusals(self: &mut Placements) -> Vec<PlacementRefusal>;
        /// The requests to send since the last call (shape A): one row per base, a set's rows consecutive.
        fn take_requests(self: &mut Placements) -> Vec<PlacementRequestRow>;
        /// A caster's report for one base of a requested set, judged (shape A).
        fn report_placed(self: &mut Placements, report: PlacementReport, now_ms: u64) -> PlacementReportVerdict;

        /// The eighteen skills' experience constants (thuum docs/verbs/leveling.md): UESP's until the
        /// game files' AVIF rows are set on it.
        type LevelingTable;
        fn new_leveling_table() -> Box<LevelingTable>;
        /// A skill's constants from its AVIF record; false for an ActorValue that is no skill or a
        /// constant that is not a number.
        fn set_skill(self: &mut LevelingTable, skill_av: u8, use_mult: f32, use_offset: f32, improve_mult: f32, improve_offset: f32) -> bool;
        /// The experience one use of the skill at `skill_av` earns from the action's `base` amount
        /// (0 for no skill).
        fn use_xp(self: &LevelingTable, skill_av: u8, base: f32) -> f32;
        /// The experience the step from `level` to the next costs (0 for no skill).
        fn level_cost(self: &LevelingTable, skill_av: u8, level: u8) -> f32;
        /// A use applied to a player's standing: the skills the record holds (a skill not listed
        /// stands at the floor), the character, the use.
        fn apply_use(self: &LevelingTable, skills: &[LevelingSkill], character: LevelingCharacter, skill_av: u8, base: f32) -> LevelingOutcome;
        /// A skill book read applied to a player's standing: the skill at `skill_av` one level up
        /// (nothing at the cap), its experience carried as the same share of the new step, the
        /// character credited the level (thuum docs/verbs/leveling.md, skill books). Once per
        /// book per character is the caller's record.
        fn apply_book(self: &LevelingTable, skills: &[LevelingSkill], character: LevelingCharacter, skill_av: u8) -> LevelingOutcome;
        /// The character experience the step from `level` to the next needs.
        fn leveling_character_threshold(level: u16) -> f32;

        /// An activation of a reference whose lock is `state` by a player who holds its key or not
        /// (thuum docs/verbs/locks.md).
        fn lock_activation(state: &LockStateRow, has_key: bool) -> LockActivationVerdict;
        /// A player's report on a lock (`outcome` 0 picked, 1 broken `broken` picks, 2 left), judged
        /// against the state, the player's reach, the Lockpicks it holds and the gap since its last
        /// report on this lock (u64::MAX for none).
        fn lock_report(state: &LockStateRow, outcome: u8, broken: u8, within_reach: bool, picks_held: u32, since_last_ms: u64) -> LockReportVerdict;

        /// A seat's report of a reference's destruction `stage` and
        /// `destroyed` flag against its record (thuum D-S3; the game files'
        /// state, stage 0 and not destroyed, for a reference with none): the
        /// stage only rises, destroyed only goes true, the first report of
        /// each step wins.
        fn destruction_report(record: &DestructionRow, stage: u8, destroyed: bool) -> DestructionVerdict;
        /// The highest stage a report may name (wire-rules
        /// destruction::MAX_STAGE, wire-validate's bound).
        fn destruction_max_stage() -> u8;
        /// Sets standing in the world.
        fn live_sets(self: &Placements) -> usize;

        /// Every player actor's recent bow and crossbow shots.
        type RangedShots;
        /// No shots yet.
        fn new_ranged_shots() -> Box<RangedShots>;
        /// `actor` loosed `shot` at `now_ms` (a monotonic clock).
        fn record(self: &mut RangedShots, actor: u32, shot: ShotFacts, now_ms: u64);
        /// A hit by `actor` with `weapon` on a target at (x, y, z): claims
        /// the newest unused shot whose arrow can have reached it.
        fn claim(self: &mut RangedShots, actor: u32, weapon: u32, x: f32, y: f32, z: f32, now_ms: u64) -> ShotClaim;
        /// The actor is gone: its shots with it.
        fn forget(self: &mut RangedShots, actor: u32);

        /// Applies one effect at `scale` times its magnitude, cast by
        /// `caster` (wire-rules magic::apply).
        fn apply_effect(facts: EffectFacts, scale: f32, caster: u32, arrival: EffectArrival) -> EffectApplied;
        /// Runs `entries` for `dt_s` seconds: their changes; ended entries
        /// are removed.
        fn advance_effects(entries: &mut Vec<EffectEntry>, dt_s: f32) -> Vec<AvChange>;
        /// Adds `entry` to `entries` within the bound: a displaced buff's
        /// return comes back.
        fn admit_effect(entries: &mut Vec<EffectEntry>, entry: EffectEntry) -> Vec<AvChange>;
        /// Ends the running effects whose MGEF is one of `effects`; a buff's
        /// give-back is returned.
        fn remove_effects(entries: &mut Vec<EffectEntry>, effects: &[u32]) -> Vec<AvChange>;
        /// The temporary modifier the running buffs put on `av`.
        fn effect_modifier(entries: &Vec<EffectEntry>, av: u32) -> f32;
        /// The standing modifier a race's constant abilities put on `av`.
        fn ability_modifier(facts: &Vec<EffectFacts>, av: u32) -> f32;
        /// The share of a hostile effect's magnitude a target takes, by its
        /// element and magic resistances under `cap`.
        fn resistance_factor(resist: f32, magic_resist: f32, cap: f32) -> f32;
        /// A Reanimate effect at least this long is a full thrall's, seconds
        /// (wire-rules magic::FULL_THRALL_MIN_S), for the core's restart:
        /// every commander's departure, at which a full thrall stays and a
        /// timed one dies.
        fn full_thrall_min_s() -> f32;

        /// Every player actor's recent spell casts and open streams.
        type SpellCasts;
        /// No casts yet.
        fn new_spell_casts() -> Box<SpellCasts>;
        /// `actor` began a cast at `now_ms` (a monotonic clock). True: the
        /// cast's relay waits for its first hit (`CastFacts::lands_on_target`).
        fn start(self: &mut SpellCasts, actor: u32, facts: CastFacts, now_ms: u64) -> bool;
        /// `actor`'s stream in `hand` ended.
        fn end(self: &mut SpellCasts, actor: u32, hand: u8);
        /// Whether `actor` holds a stream open in `hand`.
        fn is_open(self: &SpellCasts, actor: u32, hand: u8) -> bool;
        /// A spell hit by `actor`: claims a cast that covers it.
        fn claim(self: &mut SpellCasts, actor: u32, hit: CastHit, now_ms: u64) -> CastClaim;
        /// A hit by a spell a recorded cast's records reach (thuum
        /// docs/verbs/second-hand.md), judged from where the caster stands.
        fn claim_reached(
            self: &mut SpellCasts,
            actor: u32,
            hit: CastHit,
            caster_x: f32,
            caster_y: f32,
            caster_z: f32,
            now_ms: u64,
        ) -> CastClaim;
        /// The actor is gone: its casts and streams with it.
        fn forget(self: &mut SpellCasts, actor: u32);

        /// Every player's voice: when its last accepted shout's recovery ends.
        type Voices;
        /// No shouts yet.
        fn new_voices() -> Box<Voices>;
        /// The verdict on `actor`'s shout at `now_ms` (a monotonic clock);
        /// an accepted one starts its recovery.
        fn shout(self: &mut Voices, actor: u32, facts: &VoiceFacts, now_ms: u64) -> VoiceRefusal;
        /// How long `actor`'s recovery runs from `now_ms`, 0 when none.
        fn recovery_left_ms(self: &Voices, actor: u32, now_ms: u64) -> u64;

        /// The server's game clock and who has heard it.
        type GameClock;
        /// A clock from server-settings.json's `time` block as JSON text
        /// (`{}` for every default); the error names the bad key.
        fn new_game_clock(settings_json: &str) -> Result<Box<GameClock>>;
        /// The clock at `now_ms`, Unix milliseconds.
        fn now(self: &GameClock, now_ms: i64) -> GameTime;
        /// A player logs in and hears the clock.
        fn login(self: &mut GameClock, user: u32, now_ms: i64) -> GameTime;
        /// Whether a logged-in player is due to hear it again; a true
        /// answer counts as heard.
        fn resync_due(self: &mut GameClock, user: u32, now_ms: i64) -> bool;
        /// A player left.
        fn forget(self: &mut GameClock, user: u32);

        /// thuum docs/verbs/weather.md (M4): the server's weather per
        /// worldspace and who has heard it (wire-rules weather).
        type WeatherSync;
        /// From server-settings.json's `weather.changeGameMinutes` (1 to
        /// 1440) and `weather.seed`; the error names the bad value.
        fn new_weather_sync(change_game_minutes: u32, seed: u64) -> Result<Box<WeatherSync>>;
        /// A worldspace's pool, `weathers[i]` with `chances[i]`: from the
        /// game files (`pinned` false, joined with the worldspace's) or the
        /// settings (`pinned` true, outranking the files'). The pool's size
        /// after; 0 when refused (a length mismatch, no weather with a
        /// chance above 0, more than 64).
        fn set_pool(self: &mut WeatherSync, world: u32, weathers: &[u32], chances: &[u32], pinned: bool) -> usize;
        /// The word `user` is owed standing in `place` (a worldspace when
        /// `is_world`) at `game_minutes` of the clock; a returned word
        /// counts as heard.
        fn due(self: &mut WeatherSync, user: u32, place: u32, is_world: bool, game_minutes: f64) -> WeatherWord;
        /// A player left.
        fn forget(self: &mut WeatherSync, user: u32);
        /// The size of the pool in force for `world`, 0 for none.
        fn pool_len(self: &WeatherSync, world: u32) -> usize;

        /// thuum docs/verbs/perks.md (M5): every perk of the load order and
        /// its own conditions (wire-rules perks).
        type PerkTable;
        /// An empty table.
        fn new_perk_table() -> Box<PerkTable>;
        /// A perk with its DATA bytes; a repeated id replaces the entry.
        fn add_perk(self: &mut PerkTable, perk: u32, playable: bool, hidden: bool);
        /// One of `perk`'s own conditions from its CTDA: the function id, the
        /// first byte, the comparison value, the first parameter (a form id
        /// made global by the caller for HasPerk, an actor value otherwise)
        /// and the run-on type; false for a perk not added.
        fn add_condition(self: &mut PerkTable, perk: u32, function: u16, byte0: u8, value: f32, param1: u32, run_on: u32) -> bool;
        /// How many perks the table knows.
        fn perk_count(self: &PerkTable) -> usize;
        /// Whether the table knows `perk`.
        fn knows(self: &PerkTable, perk: u32) -> bool;
        /// The judgement of `perk` for a player at `level` holding `perks`,
        /// whose actor values the conditions may read are `avs[i]` at
        /// `values[i]` (a skill's level, a base otherwise).
        fn pick(self: &PerkTable, level: u16, perks: &[u32], picks: u16, avs: &[u8], values: &[f32], perk: u32) -> PerkVerdict;
        /// The perk points left: one a level past the first, less the perks held.
        /// The perk points left: one a level past the first, less the picks the server granted (not the perks held).
        fn perk_points(level: u16, picks: u16) -> u16;
        /// The most perks a record holds (the wire's cap on PerkState).
        fn perks_max() -> usize;
        /// One of `perk`'s entry point effects (a PRKE section with a one-float
        /// value): its index among the perk's effects, -1 for a perk not added
        /// (thuum docs/verbs/perks.md, the second half).
        fn add_effect(self: &mut PerkTable, perk: u32, entry_point: u8, function: u8, value: f32, rank: u8, priority: u8) -> i32;
        /// A condition of that effect on `tab` (0 the perk owner, 1 the subject:
        /// the weapon or the armor piece), from its CTDA as `add_condition`
        /// takes one; false when it lands on a third tab, which leaves the
        /// effect out, or names no effect.
        fn add_effect_condition(self: &mut PerkTable, perk: u32, effect: i32, tab: u8, function: u16, byte0: u8, value: f32, param1: u32, run_on: u32) -> bool;
        /// How many effects the formulas judge.
        fn judged_effects(self: &PerkTable) -> usize;
        /// How many effects are left out (another function, a third tab).
        fn unjudged_effects(self: &PerkTable) -> usize;
        /// `start` through the held perks' effects on `entry_point` whose owner
        /// conditions hold for the record (as `pick` reads it) and whose subject
        /// conditions hold for `subject_keywords`, in priority order.
        fn apply_entry_point(self: &PerkTable, entry_point: u8, start: f32, level: u16, perks: &[u32], avs: &[u8], values: &[f32], subject_keywords: &[u32]) -> f32;

        /// thuum docs/verbs/cell-host.md (ADR-035): which player's game runs
        /// each cell's NPCs, the grace and the epochs.
        type Cells;
        /// No cell hosted, no player known.
        fn new_cells() -> Box<Cells>;
        /// The cells the server's grid gives `player` now (its 3x3 of
        /// exterior grid cells, or its interior) at `now_ms`: a grant for a
        /// cell newly reached that the game has loaded and nobody hosts; a
        /// hosted cell the grid dropped starts its grace, one reached again
        /// disarms it.
        fn reach(self: &mut Cells, player: u32, cells: &[u32], now_ms: u64) -> Vec<HostDecision>;
        /// The transport's round trip for `player`'s connection, ms.
        fn rtt(self: &mut Cells, player: u32, ms: u32);
        /// `player`'s game reports `cell` fully loaded at `now_ms` (a
        /// monotonic clock): a grant when the cell has no host and the grid
        /// gives it the cell.
        fn loaded(self: &mut Cells, player: u32, cell: u32, now_ms: u64) -> Vec<HostDecision>;
        /// `player`'s game reports `cell` gone from its loaded set: its
        /// grace starts when it hosts the cell.
        fn unloaded(self: &mut Cells, player: u32, cell: u32, now_ms: u64) -> Vec<HostDecision>;
        /// `player` left the server: every cell it hosts starts its grace.
        fn gone(self: &mut Cells, player: u32, now_ms: u64) -> Vec<HostDecision>;
        /// `player`'s connection went silent while netcode still holds it
        /// (thuum docs/verbs/cell-host.md, the quitting host): every cell
        /// it hosts starts its grace and it is no candidate, its loaded set
        /// kept for its return.
        fn stale(self: &mut Cells, player: u32, now_ms: u64) -> Vec<HostDecision>;
        /// `player`'s messages resumed: a candidate again; a free cell it
        /// has loaded is granted, its own running grace disarmed.
        fn revived(self: &mut Cells, player: u32, now_ms: u64) -> Vec<HostDecision>;
        /// `player`'s actor died (thuum docs/research/m3/
        /// decisions-after-playtest-34.md D18): every cell it hosts starts
        /// its grace and it is no candidate until `respawned`; its loaded
        /// set kept.
        fn died(self: &mut Cells, player: u32, now_ms: u64) -> Vec<HostDecision>;
        /// `player`'s actor stood again: a candidate again; a free cell it
        /// has loaded is granted, its own running grace disarmed.
        fn respawned(self: &mut Cells, player: u32, now_ms: u64) -> Vec<HostDecision>;
        /// The clock: each grace that ran out passes its cell on (a Release,
        /// then a Grant or an Unhosted).
        fn tick(self: &mut Cells, now_ms: u64) -> Vec<HostDecision>;
        /// Who hosts `cell` now.
        fn host_of(self: &Cells, cell: u32) -> CellHost;
        /// Whether a report from `player` for an actor in `cell` at `epoch`
        /// is the standing host's.
        fn accepts(self: &Cells, cell: u32, player: u32, epoch: u32) -> bool;
        /// The cell's epoch, kept while unhosted; 0 before any grant.
        fn epoch_of(self: &Cells, cell: u32) -> u32;
        /// Every player whose game reports `cell` loaded, lowest id first
        /// (thuum ADR-068: the listeners of a script call's subject the
        /// server holds no reference for, placed by its record).
        fn loaders_of(self: &Cells, cell: u32) -> Vec<u32>;
        /// Every cell the table knows, by cell id (the lab's reading).
        fn hosts(self: &Cells) -> Vec<CellHostRow>;
        /// `reach` with each cell's ring (0 the player's own square, 1 a
        /// neighbour), the nearness the cold start ranks by (ADR-047 D10).
        fn reach_ranked(self: &mut Cells, player: u32, cells: &[u32], rings: &[u8], now_ms: u64) -> Vec<HostDecision>;
        /// A simulator's report moving an actor between cells: 0 Refuse,
        /// 1 Accept, 2 Follow (recorded here) (ADR-047 D6).
        fn may_move(self: &mut Cells, report: &CellMove, now_ms: u64) -> u8;
        /// `may_move`'s verdict without recording anything (the door's
        /// pre-check, ADR-047 D6).
        fn judge_move(self: &Cells, report: &CellMove, now_ms: u64) -> u8;
        /// Who simulates `actor` standing in `cell`: the host, else the
        /// follower, else 0.
        fn simulator_of(self: &Cells, actor: u32, cell: u32) -> u32;
        /// ADR-052: `player` claims `actor` in `cell` for a dialogue; 0
        /// granted, 1 held by another, 2 the claimant has not loaded the
        /// cell, 3 renewed.
        fn claim_actor(self: &mut Cells, actor: u32, player: u32, cell: u32, now_ms: u64) -> u8;
        /// ADR-060: `player`'s spell commands `actor` in `cell` until its
        /// engine ends the effect (the thrall's death; no lease); `effect_ms`
        /// the record's duration (0 permanent), the thrall's estimate once
        /// its commander leaves: the same codes as claim_actor.
        fn claim_commanded(self: &mut Cells, actor: u32, player: u32, cell: u32, effect_ms: u64, now_ms: u64) -> u8;
        /// `player` releases its dialogue claim on `actor`; false when it
        /// holds none (a commanded row ends by its death, lease or
        /// commander's departure, never a dialogue's release; ADR-060).
        fn release_claim(self: &mut Cells, actor: u32, player: u32) -> bool;
        /// Any claim on `actor` ends (its death): the claimant it had, 0 when none.
        fn release_any_claim(self: &mut Cells, actor: u32) -> u32;
        /// ADR-060: the player commanding `actor`, 0 when none.
        fn commander_of(self: &Cells, actor: u32) -> u32;
        /// Every follow, by actor id.
        fn follows(self: &Cells) -> Vec<CellFollowRow>;
        /// The grace a host keeps its cell after leaving it, ms
        /// (wire-rules cells::GRACE_MS), for the core's timer.
        fn host_grace_ms() -> u64;

        /// thuum docs/verbs/prop-physics.md (ADR-058): one simulating seat per
        /// physics prop, the cell's host by default, the last toucher while
        /// its lease holds; the owner table's "prop" rows.
        type Props;
        /// No prop claimed.
        fn new_props() -> Box<Props>;
        /// `player` touched `refr` in `cell` by `touch` (0 contact, 1 grab, 2
        /// drop, 3 impulse) at `now_ms`: 0 granted (the epoch is
        /// `prop_epoch_of`), 1 held by another's grab, 2 the claimant
        /// simulates it already.
        fn claim_prop(self: &mut Props, refr: u32, player: u32, cell: u32, touch: u8, now_ms: u64) -> u8;
        /// thuum D-S3 point 4: `player`'s seat claims `refr`, a trap's world
        /// body in `cell` (a world object its own script set moving; the core
        /// admits it by the impulse touch alone), at `now_ms`: the codes of
        /// `claim_prop`, and 3 Moving, another seat's firing set the body
        /// moving and it is awake under that seat's live row (the first firing
        /// wins).
        fn claim_prop_world_body(self: &mut Props, refr: u32, player: u32, cell: u32, now_ms: u64) -> u8;
        /// The prop's owner epoch, up at every grant; 0 before any.
        fn prop_epoch_of(self: &Props, refr: u32) -> u32;
        /// `player`'s transform of `refr`, landing in `cell` (0 for none)
        /// whose host is `cell_host` (0 while nobody), the body asleep or
        /// not, `moved` units from the record: 0 accepted (the record takes
        /// it), 1 not the owner's, 2 no cell or an unhosted cell nobody holds
        /// (the prop freezes), 3 farther than the step budget allows (the
        /// record stands); `cell_host` hosts the cell the RECORD stands in.
        fn prop_transform(self: &mut Props, facts: &PropSampleFacts) -> u8;
        /// The row's owner while the row is live, else 0: the word a late
        /// listener gets at its subscription.
        fn prop_live_owner(self: &Props, refr: u32, now_ms: u64) -> u32;
        /// Who simulates `refr` now, given its cell's host (0 while nobody):
        /// the toucher while awake or inside its lease, else the host.
        fn prop_owner_of(self: &Props, refr: u32, cell_host: u32, now_ms: u64) -> u32;
        /// `host` now hosts `cell`: the rows nobody holds live name it, their
        /// epochs kept.
        fn props_host_changed(self: &mut Props, cell: u32, host: u32, now_ms: u64);
        /// `player` left: its rows fall back to their cells' hosts.
        fn props_gone(self: &mut Props, player: u32);
        /// The lease a toucher keeps a sleeping prop for, ms (wire-rules
        /// props::PROP_LEASE_MS), for the core's log.
        fn prop_lease_ms() -> u64;

        /// thuum ADR-068 (the one seam): the row of a reported script call
        /// and its subject, the row for the core's shape step.
        fn script_call_subject(cls: &str, func: &str, self_id: u32, args: &[ScriptArg]) -> ScriptCallSubject;
        /// Every reporter's budget and every subject's sequence (wire-rules
        /// natives), and the call the shape step passed until the owner
        /// step takes it.
        type ScriptCalls;
        /// Every budget full, every sequence at 0.
        fn new_script_calls() -> Box<ScriptCalls>;
        /// The shape step on one reported call of the table's `row` (-1 for
        /// a pair it lacks) from `sender` naming `self_id`, at `now_ms` on a
        /// monotonic clock, which the core asks before it reads anything of
        /// the subject: the reporter's token (spent by every call that
        /// reaches the step, refused or not), the row, the arguments and the
        /// subject's form. Accepted with the subject when the shape holds,
        /// the call then waiting for `owner`; Row, Args or Rate otherwise,
        /// and the core looks nothing up.
        fn shape(self: &mut ScriptCalls, row: i32, args: &[ScriptArg], sender: u32, self_id: u32, now_ms: u64) -> ScriptCallVerdict;
        /// thuum ADR-068 rung 5: whether a shaped call's object arguments at
        /// 0xff000000 and up each name a connected player's actor (`players`,
        /// the core's list), the acting player's figure a scoped call names
        /// where its script named the player (wire-rules natives::figures):
        /// Accepted, or Args with its name and why. The core asks it after
        /// `shape` and before `owner`.
        fn script_call_figures(args: &[ScriptArg], players: &[u32]) -> ScriptCallVerdict;
        /// The owner step on the call the last `shape` accepted, over the
        /// facts the core gathered for its subject (its kind, its cell, that
        /// cell's host and the host's epoch, or an actor's simulator and its
        /// cell's host); an accepted relay or record takes its subject's
        /// next sequence, a call kept as evidence none. Skipped, nothing
        /// relayed, when no shaped call waits, so each shaped call is judged
        /// once.
        fn owner(self: &mut ScriptCalls, facts: &ScriptCallFacts) -> ScriptCallVerdict;
        /// The subject's sequence: its accepted calls so far, or the stored
        /// record's it was seeded with (thuum ADR-068 rung 3).
        fn seq_of(self: &ScriptCalls, subject: u32) -> u32;
        /// Raise the subject's sequence to `seq` where it stands below, never
        /// lower it: `seq` is the object's stored record's, so after a
        /// restart the next accepted call on the object takes a number above
        /// the record's (wire-rules natives' seed).
        fn seed(self: &mut ScriptCalls, subject: u32, seq: u32);

        /// thuum ADR-068 rung 3 (the script record): the release each cell
        /// last saw, until its next grant (wire-rules script_state).
        type ScriptStates;
        /// No release noted.
        fn new_script_states() -> Box<ScriptStates>;
        /// The rule on one reported record over the facts the core gathered
        /// for its reference (its kind, its cell, that cell's host and its
        /// standing epoch): the reference first, then the epoch, then the
        /// owner; while nobody hosts the cell, the release read of the host
        /// that released it, at the epoch it released.
        fn judge(self: &ScriptStates, facts: &ScriptStateFacts) -> ScriptStateVerdict;
        /// `player` released `cell` at `epoch` (the cells table's Release):
        /// its release read at that epoch is taken while nobody hosts it.
        fn released(self: &mut ScriptStates, cell: u32, player: u32, epoch: u32);
        /// `cell` was granted, to any seat: the release read ends.
        fn granted(self: &mut ScriptStates, cell: u32);

        /// thuum ADR-068 D-W1 (the trigger entry channel): a trap trigger's
        /// reported firing over the facts the core gathered, the cell's host
        /// first, then a game with the cell loaded one of whose actors stands
        /// within the trigger's activation reach plus the firing's slack
        /// (wire-rules trigger_entry::firing_source); the link and the budget
        /// stay the core's.
        fn trigger_firing_source(facts: &TriggerFiringFacts) -> TriggerFiringVerdict;
        /// thuum ADR-068 D-C2: each presser's accepted enters it has not
        /// left, by trigger (wire-rules trigger_entry::Entries).
        type TriggerEntries;
        /// No enter open.
        fn new_trigger_entries() -> Box<TriggerEntries>;
        /// A presser's owner's report of an entry into a world trigger whose
        /// script runs on the cell's host: an enter judged by the trigger,
        /// the presser, the loader, the host and the reach, in that order,
        /// and kept open when relayed; a leave first by its pairing
        /// (E_ENTRY_UNPAIRED when it ends no open enter of its presser's
        /// into its trigger), then by the same checks but the loader and the
        /// reach (wire-rules trigger_entry::Entries::judge); relayed, `host`
        /// names the one game to send it to.
        fn judge(self: &mut TriggerEntries, facts: &TriggerEntryFacts) -> TriggerEntryVerdict;
        /// The presser's game left: its open enters go.
        fn forget(self: &mut TriggerEntries, presser: u32);
        /// The fifty fixes' furniture rule: who owns a world object's
        /// reported graph event, a furniture's recorded occupant's owner for
        /// a furniture in use, else the cell's host, and whether it is kept
        /// as the reference's lastAnimation (never for a furniture)
        /// (wire-rules object_animation::judge).
        fn object_animation_owner(facts: &ObjectAnimationFacts) -> ObjectAnimationVerdict;
        /// The fifty fixes' D-C3 A (PENDING ELI): where a reported call's
        /// graph variable at `seq` goes in its subject's kept variables, at
        /// most sixteen (wire-rules natives::variable_slot).
        fn graph_variable_slot(kept: &[GraphVariableKept], seq: u32) -> GraphVariableSlot;

        /// thuum ADR-068 rung 4 (amendment B, read 8): every created alias
        /// of one quest's reference aliases and where its item goes
        /// (wire-rules alias_fills::creations).
        fn alias_creations(aliases: &[QuestAliasRow]) -> Vec<AliasCreationRow>;
        /// Whether the created alias `alias` of `quest` still owes its item:
        /// no mark of the journal's says it was added.
        fn alias_created_due(marks: &[AliasMarkRow], quest: u32, alias: u32) -> bool;
        /// thuum ADR-068 rung 4 (AliasFill 79): one reported fill against
        /// the journal's fills, over the facts the core gathered (wire-rules
        /// alias_fills::judge).
        fn alias_fill_judge(record: &[AliasFillEntry], fill: &AliasFillEntry, facts: &AliasFillFacts) -> AliasFillVerdict;
        /// thuum ADR-068 rung 4 (WorldGlobal 80): one reported entry, 0
        /// accepted, else its code: 1 E_WORLDGLOBAL_CLOCK, 2 _PLAYER, 3
        /// _CONSTANT, 4 _NO_GLOB (wire-rules world_globals::entry).
        fn world_global_entry(facts: &WorldGlobalFacts) -> u8;
        /// The stable name of a world global's code: an entry's (1 to 4),
        /// 5 E_WORLDGLOBAL_OWNER, 6 E_WORLDGLOBAL_EPOCH; empty for any other.
        fn world_global_code_name(code: u8) -> String;
        /// The record after a report's accepted entries: the latest value
        /// wins, an equal one is folded, the moved ones listed to relay.
        fn world_globals_merge(record: &[WorldGlobalValue], accepted: &[WorldGlobalValue]) -> WorldGlobalsMerge;

        /// thuum ADR-068 rung 4 (the ref state): one call of `func` (Enable,
        /// EnableNoWait, Disable, DisableNoWait, Delete) against the record
        /// (wire-rules ref_state::apply).
        fn ref_state_apply(disabled: bool, deleted: bool, func: &str) -> RefStateVerdict;
        /// thuum ADR-068 rung 4 (the standing, W3): one call of `func` with
        /// its object argument (0 for None) and its number against the
        /// NPC's record (wire-rules standing::judge); `crime_faction` the
        /// record's (0 for none), or one no FACT holds while the record
        /// keeps none and the base's is unread.
        fn standing_judge(
            func: &str,
            object: u32,
            number: i64,
            faction_is_fact: bool,
            other_is_actor: bool,
            factions: &[StandingFaction],
            crime_faction: u32,
            relationships: &[StandingRelation],
        ) -> StandingVerdict;

        /// thuum ADR-068 rung 4 (the script set, W3): a script's PlaceAtMe or
        /// PlaceActorAtMe by `caster` on `self_id`, which stood at (x, y,
        /// z), of the base (short id, file) of `record_type`.
        #[allow(clippy::too_many_arguments)]
        fn open_script_set(self: &mut Placements, caster: u32, self_id: u32, x: f32, y: f32, z: f32, short_id: u32, file: &str, record_type: &str, now_ms: u64) -> ScriptSetOpen;
        /// The script sets' requests to send since the last call, each once.
        fn take_script_requests(self: &mut Placements) -> Vec<ScriptRequestRow>;
        /// The script sets' outcomes at `now_ms`: placed or expired.
        fn due_script_placements(self: &mut Placements, now_ms: u64) -> Vec<ScriptPlacementRow>;

        /// thuum ADR-068 rung 4: the one seat that streams what nobody owns
        /// (wire-rules world_streamer).
        type WorldStreamer;
        /// Nobody streams, epoch 0.
        fn new_world_streamer() -> Box<WorldStreamer>;
        /// `player`'s movement report: the first makes the seat eligible.
        fn moved(self: &mut WorldStreamer, player: u32) -> StreamerElection;
        /// `player`'s connection went stale, or came back.
        fn stale(self: &mut WorldStreamer, player: u32, stale: bool) -> StreamerElection;
        /// `player` died, or stands again.
        fn dead(self: &mut WorldStreamer, player: u32, dead: bool) -> StreamerElection;
        /// `player` left.
        fn gone(self: &mut WorldStreamer, player: u32) -> StreamerElection;
        /// The streamer now and the epoch (`changed` false).
        fn streamer_now(self: &WorldStreamer) -> StreamerElection;
        /// A report from `sender` at `epoch`: 0 Current, 1 StaleEpoch, 2
        /// FutureEpoch, 3 NotOwner, the epoch first.
        fn judge(self: &WorldStreamer, sender: u32, epoch: u32) -> u8;

        /// thuum ADR-068 rung 5 (QuestStage 57's word, read 5): what a report
        /// asks of the records, from its `word` (0 for none; the message's
        /// std::optional), its `stage` and `start_up`, whether the stage is
        /// one the quest's start runs (the core's WordStages for a start:
        /// stage 0 and the record's start-up stages, QUST INDX): 0 a stage
        /// (quest_stages), 1 a start, 2 a stop (quest_runs), a report with
        /// no word of stage 0 or of a start-up stage a start (read 5,
        /// generalized 2026-11-10 for b-quest-rumor's HeartwoodMill claim);
        /// 3 nothing, for a word the wire refuses first.
        fn quest_report_word(word: u32, stage: u16, start_up: bool) -> u8;
        /// thuum ADR-068 rung 5: the churn counter and the quests held out
        /// of the run record for the server's run (wire-rules quest_runs).
        type QuestRuns;
        /// No counters, nothing held.
        fn new_quest_runs() -> Box<QuestRuns>;
        /// One run word (`word` 1 a start, 2 a stop; any other is folded) of
        /// the quest `short_id` in `file` from `seat`, the reporter's actor,
        /// at `now_ms` on a monotonic clock, with the report's `cause` (the
        /// wire's QuestStage `cause`: 1 world, 2 the seat's own, 0 none from
        /// an older client), against the journal's `record`: a seat-caused
        /// word is refused (D-Q4 C, code 4), a Story Manager instance's under
        /// "seat" (code 5), a held quest's word is held, then the seat's flip
        /// (past RUN_FLIPS inside RUN_WINDOW_MS the quest becomes held), the
        /// fold, and RUN_RECORD_CAP for a quest the record lacks. Ask
        /// shared_quests_check first: a denied or player-class quest's word
        /// is refused there, never counted here.
        #[allow(clippy::too_many_arguments)]
        fn quest_runs_judge(self: &mut QuestRuns, record: &[QuestRunRow], short_id: u32, file: &str, word: u8, cause: u8, seat: u32, now_ms: u64) -> QuestRunVerdict;
        /// thuum ADR-068 rung 5, D-Q4 C: the setting `questStoryInstances`
        /// from its JSON text ("seat", the default, or "world"; the core
        /// passes `"seat"` for a key the file lacks), true for "world"; an
        /// error names what is wrong.
        fn quest_story_instances_world(settings_json: &str) -> Result<bool>;
        /// thuum D-Q9 B (Eli 2026-11-11; train 52): the setting
        /// `questPlayerAliasCause` from its JSON text ("seat", the default,
        /// D-Q9 B, or "world", D-Q9 A; the core passes `"seat"` for a key the
        /// file lacks), true for "world"; an error names what is wrong.
        fn quest_player_alias_cause_world(settings_json: &str) -> Result<bool>;
        /// D-Q9 B: whether a report's root is the seat's own: its `function`
        /// (the root's part after its last dot) one of wire-rules
        /// quest_runs::PLAYER_ALIAS_EVENTS, names without case, on a script
        /// the game files attach to player aliases alone
        /// (`script_on_player_aliases_only`, the core's index).
        fn quest_root_is_seat_own(function: &str, script_on_player_aliases_only: bool) -> bool;
        /// The Story Manager's quests (every quest an SMQN node of the game
        /// files names, the core's read; stage 0 in each entry) and the
        /// setting (`world`, quest_story_instances_world's answer), both
        /// replaced whole.
        fn quest_runs_set_story(self: &mut QuestRuns, quests: &[QuestStageEntry], world: bool);
        /// Whether a Story Manager quest node names the quest, whatever the
        /// setting says.
        fn quest_runs_story_named(self: &QuestRuns, short_id: u32, file: &str) -> bool;
        /// How many quests the Story Manager's nodes name.
        fn quest_runs_story_count(self: &QuestRuns) -> usize;
        /// Whether the quest is held (a seat's churn) for the run.
        fn quest_runs_held(self: &QuestRuns, short_id: u32, file: &str) -> bool;
        /// `seat` left: its churn counters go.
        fn quest_runs_gone(self: &mut QuestRuns, seat: u32);
        /// What the login sends around the stages: the quests recorded
        /// running (served starts, ahead of the stages) and stopped (served
        /// stops, behind them), in the record's order, each still the
        /// world's by `shared` and not held; a quest recorded stopped whose
        /// stages the login replays (`stages`, the journal's stage record)
        /// is among the starts too (D-Q3 A: its served start names its
        /// start-up stages, so their fragments never run again on a seat).
        fn quest_runs_at_login(self: &QuestRuns, record: &[QuestRunRow], shared: &SharedQuests, stages: &[QuestStageEntry]) -> QuestRunsLogin;
        /// The record after a recorded word (`word` 1 or 2): the quest's run
        /// set and its sequence raised, or the quest added last at sequence
        /// 1 (never past RUN_RECORD_CAP).
        fn quest_runs_merge(record: &[QuestRunRow], short_id: u32, file: &str, word: u8) -> Vec<QuestRunRow>;
        /// The record without the quest: a held quest's entry leaves it
        /// (quest_runs_judge's code 2 with `first`).
        fn quest_runs_without(record: &[QuestRunRow], short_id: u32, file: &str) -> Vec<QuestRunRow>;

        /// thuum ADR-068 rung 5 (D-Q1's derivation whole): the settings
        /// `questScope` ("off" measures, "derived" acts) and `playerQuests`
        /// (the generated list half), wire-rules quest_scope.
        type QuestScope;
        /// The settings from their values' JSON texts (pass `"off"` and `[]`
        /// for a key the file lacks); the error names what is wrong.
        fn new_quest_scope(mode_json: &str, player_quests_json: &str) -> Result<Box<QuestScope>>;
        /// Whether the class acts ("derived"): compose the marked quests in
        /// (shared_quests_set_player, script_variables_set_player) and
        /// refuse a marked quest's report. False ("off"): log the
        /// measurement line alone.
        fn quest_scope_acting(self: &QuestScope) -> bool;
        /// The list half, in the setting's order (stage 0).
        fn quest_scope_listed(self: &QuestScope) -> Vec<QuestStageEntry>;
        /// The class of the quest `short_id` in `file` from its `aliases`
        /// (every ALST and ALLS of its QUST record, libespm's order): 1
        /// Player (every alias the player's, or none and the list names
        /// it), 0 World (a world alias, whatever the list says, or none and
        /// unlisted).
        fn quest_scope_class(self: &QuestScope, short_id: u32, file: &str, aliases: &[QuestScopeAliasRow]) -> u8;
        /// thuum D-Q9 B: the ids of the reference aliases among a quest's
        /// `aliases` (every ALST and ALLS of its QUST record) that are the
        /// player's (wire-rules quest_scope::is_players), in their order,
        /// whatever the quest's class: the core's index of the scripts that
        /// ride player aliases alone reads them per quest.
        fn quest_scope_player_aliases(aliases: &[QuestScopeAliasRow]) -> Vec<u32>;

        /// thuum ADR-068 rung 5 (D-V1 B, D-T2 B): the condition-read
        /// variables server-settings.json's `conditionVariables` lists, and
        /// the player class composed in (wire-rules script_variables).
        type ScriptVariables;
        /// The setting from the block as JSON text (`[]` for none): a list
        /// of {"form": "<hex id>:<plugin>", "script", "var"}; the error
        /// names the bad entry.
        fn new_script_variables(settings_json: &str) -> Result<Box<ScriptVariables>>;
        /// Compose the player class in, while the scope acts: a listed key
        /// whose form is one of `quests` (stage ignored) is its player's own,
        /// and so is each of `own` (D-T2 B: the quests' scalar variables and
        /// their game-time registration's two reserved names, its `own`
        /// unread) whose form is one of `quests` and which the setting does
        /// not list already, each once, at most VARIABLE_RECORD_CAP; answers
        /// how many of `own` it kept.
        fn script_variables_set_player(self: &mut ScriptVariables, quests: &[QuestStageEntry], own: &[ScriptVarKeyRow]) -> usize;
        /// The watch list a login names: the listed keys in the setting's
        /// order, then the composed own keys, each key's `own` set (the login
        /// sends it in ScriptVariables 81's `watch`, at most 256 keys a
        /// message).
        fn script_variables_watch(self: &ScriptVariables) -> Vec<ScriptVarKeyRow>;
        /// One report's entries, in order, each against the record it names
        /// as the entries before it left it: `world` (the journal's) for an
        /// entry not flagged own, `own` (the reporter's player's change
        /// form's) for one flagged own. Each is 0 recorded (merged: its
        /// value replaced and its sequence raised, or the key added last at
        /// 1, the rows keeping their record's `own`), 1 folded, 2 refused
        /// (E_SCRIPTVAR_UNLISTED, a key the setting does not list; _PLAYER,
        /// an own key reported as the world's or a world key as own; _KIND,
        /// a kind other than the record's for the key, or none a condition
        /// compares; _FULL, a new key past VARIABLE_RECORD_CAP). An entry's
        /// `seq` is unread (a report's is 0).
        fn script_variables_report(self: &ScriptVariables, world: &[ScriptVarRow], own: &[ScriptVarRow], entries: &[ScriptVarRow]) -> ScriptVarReport;
        /// What the login serves of `record`, in its order: the entries
        /// still listed, the world's half (`own` false, the journal's
        /// record, to every seat) or a player's own (`own` true, its change
        /// form's record, to that player alone), each with its sequence.
        fn script_variables_at_login(self: &ScriptVariables, record: &[ScriptVarRow], own: bool) -> Vec<ScriptVarRow>;

        /// thuum ADR-068 rung 5 (the two transformations, D-T1 A): every
        /// player's overlay, and the player's own rows' rule (wire-rules
        /// player_own: the bound, the twin rule, the overlay).
        type PlayerOverlays;
        /// No overlay open.
        fn new_player_overlays() -> Box<PlayerOverlays>;
        /// One player's own call (ScriptCall 75 whose owner step recorded
        /// it with `record` 4, its subject 0x14) of `cls`.`func` (Actor's
        /// SetRace, AddSpell, RemoveSpell, AddShout, RemoveShout, EquipItem
        /// and ChangeHeadPart, ObjectReference's AddItem and RemoveItem,
        /// names without case) by `player`'s seat naming `form` (its first
        /// argument's form id, 0 for None; for one landing of a leveled
        /// AddItem's roll, the landing's base, `facts.landing_of` naming the
        /// list), over the `properties` of the calling script's attachments
        /// (none for AddItem and RemoveItem, which have no bound) and the
        /// record's `facts`.
        fn player_own_judge(self: &mut PlayerOverlays, player: u32, cls: &str, func: &str, form: u32, properties: &[ScriptPropertyRow], facts: &PlayerOwnFacts) -> PlayerOwnVerdict;
        /// The overlay `player` wears now (`open` false for none), for its
        /// figure's CreateActor and UpdateAppearance and the equipment gate.
        fn player_overlay(self: &PlayerOverlays, player: u32) -> PlayerOverlayRow;
        /// `player` left: its overlay ends with the connection, given back
        /// for the core's line (`open` false for none).
        fn player_overlay_gone(self: &mut PlayerOverlays, player: u32) -> PlayerOverlayRow;
    }
}

pub use ffi::{PlayerOverlayRow, PlayerOwnFacts, PlayerOwnVerdict, QuestRunRow, QuestRunVerdict, QuestRunsLogin, QuestScopeAliasRow, ScriptPropertyRow, ScriptVarKeyRow, ScriptVarReport, ScriptVarRow, ScriptVarVerdict, RefStateVerdict, ScriptPlacementRow, ScriptRequestRow, ScriptSetOpen, StandingFaction, StandingRelation, StandingVerdict, AliasCreationRow, AliasFillEntry, AliasFillFacts, AliasFillVerdict, AliasMarkRow, QuestAliasRow, StreamerElection, WorldGlobalFacts, WorldGlobalValue, WorldGlobalsMerge, AvBase, CellHost, CellHostRow, CellFollowRow, CellMove, HostDecision, HostKind, ScriptArg, ScriptCallCode, ScriptCallFacts, ScriptCallSubject, ScriptCallVerdict, ScriptStateCode, ScriptStateFacts, ScriptStateVerdict, TriggerEntryCode, TriggerEntryFacts, TriggerEntryVerdict, TriggerFiringCode, TriggerFiringFacts, TriggerFiringVerdict, ObjectAnimationCode, ObjectAnimationFacts, ObjectAnimationVerdict, GraphVariableKept, GraphVariableSlot, HostedStateFacts, HostedStateVerdict, HostedValueFacts, HostedValueVerdict, PercentHold, AvChange, BondBreak, AvLegendary, AvMerge, AvSkill, AvSnapshot, BedFacts, BlockFacts, BlockSettings, CastCheck, CastClaim, CastFacts, CastHit, CastKind, ConeFacts, ConsoleDecision, DerivedParts, EffectApplied, EffectArrival, EffectEntry, EffectFacts, EffectKind, FavoriteEntry, FavoriteFacts, FavoriteKind, FightPair, FlagFacts, Flags, GameTime, DeathBlast, HeadshotCheck, HeadshotFacts, HostilityFacts, JumpCheck, LookAppearance, LookPartFacts, LookRef, MarkerCandidate, MarkerChoice, MeleeFacts, LevelingCharacter, LevelingEvent, LevelingOutcome, LevelingSkill, LockActivationVerdict, LockReportVerdict, LockStateRow, DestructionRow, DestructionVerdict, PlacedCast, PlacementRefusal, PlacementRemoval, PlacementReport, PlacementReportVerdict, PlacementRequestRow, PlacementRow, PlayerGlobalEntry, PropSampleFacts, QuestStageEntry, RaceFacts, Regen, RestFacts, RestRefusal, ShotCheck, ShotClaim, ShotFacts, SneakMults, StaggerFacts, StaggerSettings, PerkVerdict, TeleportLook, Verdict, VoiceFacts, VoiceRefusal, WeatherWord};

fn verdict(v: wire_rules::Verdict) -> Verdict {
    Verdict { allowed: v.allowed, bound: v.bound }
}

fn activation_within_reach(distance: f32, target_size: f32) -> Verdict {
    verdict(activation::within_reach(distance, target_size))
}

fn race_allowed(f: &RaceFacts) -> bool {
    appearance::race_allowed(appearance::RaceFacts {
        has_game_files: f.has_game_files,
        is_recorded_race: f.is_recorded_race,
        is_playable_race: f.is_playable_race,
        is_restricted: f.is_restricted,
        is_permitted: f.is_permitted,
    })
}

fn melee_within_reach(f: &MeleeFacts) -> Verdict {
    verdict(melee::within_reach(melee::MeleeFacts {
        distance: f.distance,
        combat_distance: f.combat_distance,
        bash_reach: f.bash_reach,
        weapon_reach: f.weapon_reach,
        unarmed_reach: f.unarmed_reach,
        aggressor_scale: f.aggressor_scale,
        target_scale: f.target_scale,
    }))
}

fn melee_within_cone(f: &ConeFacts) -> Verdict {
    verdict(melee::within_cone(melee::ConeFacts {
        heading: f.heading,
        dx: f.dx,
        dy: f.dy,
        widest_strike_angle: f.widest_strike_angle,
        power: f.power,
        target_dead: f.target_dead,
    }))
}

fn backed_flags(f: &FlagFacts) -> Flags {
    let kept = damage::backed_flags(damage::FlagFacts {
        claims_power: f.claims_power,
        claims_sneak: f.claims_sneak,
        since_power_start_ms: f.saw_power_start.then_some(f.since_power_start_ms),
        is_sneaking: f.is_sneaking,
        claims_bash: f.claims_bash,
        since_bash_start_ms: f.saw_bash_start.then_some(f.since_bash_start_ms),
    });
    Flags { power: kept.power, sneak: kept.sneak, bash: kept.bash }
}

/// TES3MP's rest switches (wire-rules rest).
pub struct RestSettings(rest::Switches);

fn new_rest_settings(settings_json: &str) -> Result<Box<RestSettings>, clock::SettingsError> {
    rest::Switches::from_json(settings_json).map(|s| Box::new(RestSettings(s)))
}

fn rest_check(settings: &RestSettings, f: &RestFacts) -> RestRefusal {
    let facts = rest::RestFacts {
        hours: f.hours,
        sleep: f.sleep,
        is_dead: f.is_dead,
        since_last_hit_ms: f.has_hit.then_some(f.since_last_hit_ms),
        in_fight: f.in_fight,
    };
    match rest::check(facts, settings.0) {
        Ok(()) => RestRefusal::Allowed,
        Err(rest::Refusal::Hours) => RestRefusal::Hours,
        Err(rest::Refusal::Off) => RestRefusal::Off,
        Err(rest::Refusal::Dead) => RestRefusal::Dead,
        Err(rest::Refusal::Fighting) => RestRefusal::Fighting,
    }
}

fn hosted_value_facts(f: &HostedValueFacts) -> hosted_state::ValueFacts {
    hosted_state::ValueFacts {
        reported: f.reported,
        recorded: f.recorded,
        held: f.held,
        lowered_ago_ms: f.lowered.then_some(f.lowered_ago_ms),
    }
}

fn hosted_value_verdict(v: hosted_state::ValueVerdict) -> HostedValueVerdict {
    HostedValueVerdict { record: v.record, changed: v.changed, dropped: v.dropped }
}

fn hosted_state_judge(f: &HostedStateFacts) -> HostedStateVerdict {
    let v = hosted_state::judge(&hosted_state::Facts {
        health: hosted_value_facts(&f.health),
        magicka: hosted_value_facts(&f.magicka),
        stamina: hosted_value_facts(&f.stamina),
        alive: f.alive,
        one_point: f.one_point,
    });
    HostedStateVerdict {
        health: hosted_value_verdict(v.health),
        magicka: hosted_value_verdict(v.magicka),
        stamina: hosted_value_verdict(v.stamina),
        floored: v.floored,
        dead: v.dead,
    }
}

fn sneak_mult(anim_type: u8, m: &SneakMults) -> f32 {
    damage::sneak_mult(
        anim_type,
        damage::SneakMults {
            hand: m.hand,
            one_hand_sword: m.one_hand_sword,
            one_hand_dagger: m.one_hand_dagger,
            one_hand_axe: m.one_hand_axe,
            one_hand_mace: m.one_hand_mace,
            two_hand_sword: m.two_hand_sword,
            two_hand_axe: m.two_hand_axe,
            ranged: m.ranged,
        },
    )
}

fn blocked_share(s: &BlockSettings, f: &BlockFacts) -> f32 {
    block::blocked_share(
        &block::BlockSettings {
            shield_base: s.shield_base,
            shield_scaling: s.shield_scaling,
            weapon_base: s.weapon_base,
            weapon_scaling: s.weapon_scaling,
            rating_base: s.rating_base,
            rating_max: s.rating_max,
            power_attack_mult: s.power_attack_mult,
            max_share: s.max_share,
        },
        &block::BlockFacts {
            blocking: f.blocking,
            heading: f.heading,
            dx: f.dx,
            dy: f.dy,
            ranged: f.ranged,
            shield: f.shield,
            shield_rating: f.shield_rating,
            shield_perks: f.shield_perks,
            weapon_damage: f.weapon_damage,
            block_skill: f.block_skill,
            power: f.power,
        },
    )
}

fn stagger_magnitude(s: &StaggerSettings, f: &StaggerFacts, roll: u8) -> f32 {
    stagger::stagger(
        &stagger::StaggerSettings {
            mass_mult: s.mass_mult,
            mass_base: s.mass_base,
            mass_offset_base: s.mass_offset_base,
            mass_offset_mult: s.mass_offset_mult,
            player_mass_mult: s.player_mass_mult,
            bash_base: s.bash_base,
            bash_mult: s.bash_mult,
            shield_mult: s.shield_mult,
            one_hand_mult: s.one_hand_mult,
            two_hand_mult: s.two_hand_mult,
            running_mult: s.running_mult,
            recoiling_mult: s.recoiling_mult,
            blocking_mult: s.blocking_mult,
            block_base: s.block_base,
            block_mult: s.block_mult,
            block_shield_base: s.block_shield_base,
            block_shield_mult: s.block_shield_mult,
            min: s.min,
            attack_chance: s.attack_chance,
            player_always_staggered: s.player_always_staggered,
        },
        &stagger::StaggerFacts {
            bash: f.bash,
            weapon_stagger: f.weapon_stagger,
            stagger_offset: f.stagger_offset,
            ranged: f.ranged,
            bow_stagger_bonus: f.bow_stagger_bonus,
            attacker_block: f.attacker_block,
            attacker_shield: f.attacker_shield,
            attacker_two_handed: f.attacker_two_handed,
            attacker_unarmed: f.attacker_unarmed,
            victim_mass: f.victim_mass,
            victim_is_player: f.victim_is_player,
            victim_running: f.victim_running,
            victim_recoiling: f.victim_recoiling,
            victim_blocking: f.victim_blocking,
            victim_block: f.victim_block,
            victim_block_rating: f.victim_block_rating,
            blocked: f.blocked,
            dealt_damage: f.dealt_damage,
        },
        roll,
    )
}

fn hostility_counted(f: &HostilityFacts) -> bool {
    hostility::counted(hostility::HitFacts {
        aggressor_is_player: f.aggressor_is_player,
        aggressor_is_hosted: f.aggressor_is_hosted,
        target_is_player: f.target_is_player,
        target_is_hosted: f.target_is_hosted,
        same_actor: f.same_actor,
    })
}

fn hostility_apart(distance: f32, interior: bool) -> bool {
    hostility::apart(distance, interior)
}

/// The fights going on, between players or a player and a hosted NPC
/// (wire-rules hostility).
#[derive(Debug, Default)]
pub struct Fights(hostility::Fights);

fn new_fights() -> Box<Fights> {
    Box::default()
}

impl Fights {
    fn hit(&mut self, aggressor: u32, victim: u32, now_ms: u64) -> bool {
        self.0.hit(aggressor, victim, now_ms)
    }

    fn pairs(&self) -> Vec<FightPair> {
        self.0.pairs().into_iter().map(|(a, b)| FightPair { a, b }).collect()
    }

    fn check(&mut self, a: u32, b: u32, apart: bool, now_ms: u64) -> bool {
        self.0.check(a, b, apart, now_ms)
    }

    fn in_fight(&self, actor: u32) -> bool {
        self.0.in_fight(actor)
    }

    fn between(&self, a: u32, b: u32) -> bool {
        self.0.between(a, b)
    }

    fn forget(&mut self, actor: u32) {
        self.0.forget(actor);
    }
}

fn map_marker_discovered(x: f32, y: f32, z: f32, candidates: &[MarkerCandidate]) -> MarkerChoice {
    let cs: Vec<markers::Candidate> =
        candidates.iter().map(|c| markers::Candidate { refr_id: c.refr_id, x: c.x, y: c.y, z: c.z }).collect();
    let got = markers::discovered(x, y, z, &cs);
    MarkerChoice { found: got.found, refr_id: got.refr_id, distance: got.distance }
}

fn map_marker_travel_after(recorded: bool, reported: bool) -> bool {
    markers::travel_after(recorded, reported)
}

const fn map_marker_range() -> f32 {
    markers::DISCOVERY_RANGE
}

fn ingredient_effects_kept(ate_same: bool, has_eat: bool, since_eat_ms: u64) -> bool {
    effects::kept(ate_same, has_eat.then_some(since_eat_ms))
}

const fn ingredient_effects_union(recorded: u8, reported: u8) -> u8 {
    effects::union(recorded, reported)
}

fn racemenu_preset_ok(preset: &str) -> bool {
    racemenu::preset_ok(preset)
}

fn console_decision(d: console::Decision) -> ConsoleDecision {
    match d {
        console::Decision::Run => ConsoleDecision::Run,
        console::Decision::RankTooLow => ConsoleDecision::RankTooLow,
        console::Decision::Refused => ConsoleDecision::Refused,
        console::Decision::NotServed => ConsoleDecision::NotServed,
        console::Decision::ClientOnly => ConsoleDecision::ClientOnly,
        console::Decision::Unknown => ConsoleDecision::Unknown,
    }
}

fn console_decide(name: &str, rank: u8) -> ConsoleDecision {
    console_decision(console::decide(name, console::Rank::from_number(rank)))
}

fn console_refusal_line(decision: ConsoleDecision) -> String {
    let d = match decision {
        ConsoleDecision::Run => console::Decision::Run,
        ConsoleDecision::RankTooLow => console::Decision::RankTooLow,
        ConsoleDecision::Refused => console::Decision::Refused,
        ConsoleDecision::NotServed => console::Decision::NotServed,
        ConsoleDecision::ClientOnly => console::Decision::ClientOnly,
        _ => console::Decision::Unknown,
    };
    console::refusal_line(d).to_owned()
}

fn look_ref(r: Option<&racemenu::FormRef>) -> LookRef {
    r.map_or_else(
        || LookRef { plugin: String::new(), id: 0 },
        |r| LookRef { plugin: r.plugin.clone(), id: r.id },
    )
}

fn racemenu_look_facts(preset: &str) -> LookAppearance {
    match racemenu::look_facts(preset) {
        Some(f) => LookAppearance {
            ok: true,
            parts: f.parts.iter().map(|p| look_ref(Some(p))).collect(),
            has_hair_color: f.hair_color.is_some(),
            hair_color: f.hair_color.unwrap_or(0),
            has_weight: f.weight.is_some(),
            weight: f.weight.unwrap_or(0.0),
            has_head_texture: f.head_texture.is_some(),
            head_texture: look_ref(f.head_texture.as_ref()),
        },
        None => LookAppearance {
            ok: false,
            parts: Vec::new(),
            has_hair_color: false,
            hair_color: 0,
            has_weight: false,
            weight: 0.0,
            has_head_texture: false,
            head_texture: look_ref(None),
        },
    }
}

fn racemenu_derived_head_parts(parts: &[LookPartFacts]) -> DerivedParts {
    let facts: Vec<racemenu::PartFacts> = parts
        .iter()
        .map(|p| racemenu::PartFacts { form: p.form, valid_for_race: p.valid_for_race, extras: p.extras.clone() })
        .collect();
    match racemenu::derived_head_parts(&facts) {
        Some(parts) => DerivedParts { ok: true, parts },
        None => DerivedParts { ok: false, parts: Vec::new() },
    }
}

fn racemenu_with_head_part(preset: &str, same_type: &[u8], plugin: &str, id: u32) -> String {
    let same: Vec<bool> = same_type.iter().map(|&b| b != 0).collect();
    let part = racemenu::FormRef { plugin: plugin.to_owned(), id };
    racemenu::with_head_part(preset, &same, &part).filter(|look| look.len() <= wire_schema::cap::RACEMENU_PRESET).unwrap_or_default()
}

fn av_snapshot(s: &AvSnapshot) -> actor_values::Snapshot {
    actor_values::Snapshot {
        bases: s.bases.iter().map(|b| (b.av, b.base)).collect(),
        skills: s
            .skills
            .iter()
            .map(|k| actor_values::SkillProgress { skill: k.skill, level: k.level, xp: k.xp, threshold: k.threshold })
            .collect(),
        xp: s.xp,
        threshold: s.threshold,
        level: s.level,
        legendary: s.legendary.iter().map(|l| (l.skill, l.count)).collect(),
    }
}

fn av_bases(bases: &[(u8, f32)]) -> Vec<AvBase> {
    bases.iter().map(|&(av, base)| AvBase { av, base }).collect()
}

fn actor_values_report_ok(report: &AvSnapshot, held: &[AvBase]) -> bool {
    let held: Vec<(u8, f32)> = held.iter().map(|h| (h.av, h.base)).collect();
    actor_values::report_ok(&av_snapshot(report), &held)
}

fn actor_values_merge(record: &AvSnapshot, report: &AvSnapshot, held: &[AvBase]) -> AvMerge {
    let held: Vec<(u8, f32)> = held.iter().map(|h| (h.av, h.base)).collect();
    let (merged, still) = actor_values::merge(&av_snapshot(record), &av_snapshot(report), &held);
    AvMerge {
        record: AvSnapshot {
            bases: av_bases(&merged.bases),
            skills: merged
                .skills
                .iter()
                .map(|k| AvSkill { skill: k.skill, level: k.level, xp: k.xp, threshold: k.threshold })
                .collect(),
            xp: merged.xp,
            threshold: merged.threshold,
            level: merged.level,
            legendary: merged.legendary.iter().map(|&(skill, count)| AvLegendary { skill, count }).collect(),
        },
        held: av_bases(&still),
    }
}

fn actor_values_login_applied(record: &AvSnapshot, report: &AvSnapshot) -> bool {
    actor_values::login_applied(&av_snapshot(record), &av_snapshot(report))
}

fn actor_values_login_shortfall(record: &AvSnapshot, report: &AvSnapshot) -> Vec<AvBase> {
    actor_values::login_shortfall(&av_snapshot(record), &av_snapshot(report))
        .into_iter()
        .map(|(av, base)| AvBase { av, base })
        .collect()
}

fn actor_value_index(name: &str) -> i16 {
    actor_values::index_of(name).map_or(-1, i16::from)
}

// cxx passes a C++ rust::Vec by reference as &mut Vec
#[allow(clippy::ptr_arg)]
fn percentages_hold(holds: &mut Vec<PercentHold>, av: u8, value: f32, now_ms: u64) {
    let mut ours: Vec<attributes::Hold> = holds.iter().map(|h| attributes::Hold { av: h.av, value: h.value, until_ms: h.until_ms }).collect();
    attributes::hold(&mut ours, av, value, now_ms);
    *holds = ours.into_iter().map(|h| PercentHold { av: h.av, value: h.value, until_ms: h.until_ms }).collect();
}

#[allow(clippy::ptr_arg)]
fn percentages_report(holds: &mut Vec<PercentHold>, av: u8, reported: f32, now_ms: u64) -> bool {
    let mut ours: Vec<attributes::Hold> = holds.iter().map(|h| attributes::Hold { av: h.av, value: h.value, until_ms: h.until_ms }).collect();
    let taken = attributes::report(&mut ours, av, reported, now_ms);
    *holds = ours.into_iter().map(|h| PercentHold { av: h.av, value: h.value, until_ms: h.until_ms }).collect();
    taken
}

fn self_stream_cap_seconds() -> f32 {
    magic::SELF_STREAM_CAP_S
}

fn attributes_applied_within() -> f32 {
    attributes::APPLIED_WITHIN
}

fn actor_values_set_ok(av: u8, value: f32) -> bool {
    actor_values::set_ok(av, value)
}

fn actor_values_skill_index(av: u8) -> i16 {
    actor_values::skill_index(av).map_or(-1, i16::from)
}

fn favorites_kept(report: &[FavoriteFacts]) -> Vec<FavoriteEntry> {
    let facts: Vec<(favorites::Entry, favorites::Kind)> = report
        .iter()
        .map(|f| {
            let kind = match f.kind {
                FavoriteKind::HeldItem => favorites::Kind::HeldItem,
                FavoriteKind::MissingItem => favorites::Kind::MissingItem,
                FavoriteKind::Magic => favorites::Kind::Magic,
                _ => favorites::Kind::Other,
            };
            (favorites::Entry { form: f.form, hotkey: f.hotkey }, kind)
        })
        .collect();
    favorites::kept(&facts).into_iter().map(|e| FavoriteEntry { form: e.form, hotkey: e.hotkey }).collect()
}

fn rest_slept(f: &BedFacts) -> bool {
    rest::slept(rest::BedFacts {
        since_bed_activation_ms: f.has_bed.then_some(f.since_bed_activation_ms),
        distance_to_bed: f.distance_to_bed,
    })
}

fn rested_ms(slept: bool, time_scale: f32) -> u64 {
    rest::rested_ms(slept, time_scale).unwrap_or(0)
}

fn rest_effect_seconds(hours: f32) -> f32 {
    rest::effect_seconds(hours)
}

fn rest_after(r: &Regen, hours: f32) -> f32 {
    rest::after_rest(rest::Regen { percentage: r.percentage, rate: r.rate, rate_mult: r.rate_mult }, hours)
}

/// Every player actor's ground speed budget (wire-rules movement).
#[derive(Debug, Default)]
pub struct MovementBudgets(movement::Budgets);

fn new_movement_budgets() -> Box<MovementBudgets> {
    Box::default()
}

impl MovementBudgets {
    fn spend(&mut self, actor: u32, ground: f32, now_ms: u64) -> bool {
        self.0.spend(actor, ground, now_ms)
    }

    fn forget(&mut self, actor: u32) {
        self.0.forget(actor);
    }

    fn permit_jump_interior(&mut self, actor: u32, cell: u32, now_ms: u64) {
        self.0.permit_jump(actor, movement::Landing::Interior(cell), now_ms);
    }

    fn permit_jump_exterior(&mut self, actor: u32, world: u32, x: i16, y: i16, now_ms: u64) {
        self.0.permit_jump(actor, movement::Landing::Exterior { world, x, y }, now_ms);
    }

    fn check_jump(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, now_ms: u64) -> JumpCheck {
        jump_check(self.0.check_jump(actor, cell_or_world, x, y, now_ms))
    }

    fn expect_arrival(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, now_ms: u64) {
        self.0.expect_arrival(actor, cell_or_world, x, y, now_ms);
    }

    fn pin(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, radius: f32, until_ms: u64) {
        self.0.pin(actor, cell_or_world, x, y, radius, until_ms);
    }

    fn pinned_away(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, now_ms: u64) -> bool {
        self.0.pinned_away(actor, cell_or_world, x, y, now_ms)
    }

    fn check_arrival(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, now_ms: u64) -> JumpCheck {
        jump_check(self.0.check_arrival(actor, cell_or_world, x, y, now_ms))
    }

    #[allow(clippy::too_many_arguments)]
    fn permit_cast_jump(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, z: f32, reach: f32, wait_ms: u64, now_ms: u64) {
        self.0.permit_cast_jump(actor, cell_or_world, x, y, z, reach, wait_ms, now_ms);
    }

    fn check_cast_jump(&mut self, actor: u32, cell_or_world: u32, x: f32, y: f32, z: f32, now_ms: u64) -> JumpCheck {
        jump_check(self.0.check_cast_jump(actor, cell_or_world, x, y, z, now_ms))
    }

    fn cast_jump_lands(&self, actor: u32, cell_or_world: u32, x: f32, y: f32, z: f32, now_ms: u64) -> bool {
        self.0.cast_jump_lands(actor, cell_or_world, x, y, z, now_ms)
    }
}

/// The globals the server keeps per player (wire-rules player_globals).
pub struct PlayerGlobalsList(player_globals::PlayerGlobals);

fn new_player_globals(settings_json: &str) -> Result<Box<PlayerGlobalsList>, clock::SettingsError> {
    player_globals::PlayerGlobals::from_json(settings_json).map(|l| Box::new(PlayerGlobalsList(l)))
}

fn to_values(entries: &[PlayerGlobalEntry]) -> Vec<player_globals::GlobalValue> {
    entries
        .iter()
        .map(|e| player_globals::GlobalValue { global: forms::FormDesc { short_id: e.short_id, file: e.file.clone() }, value: e.value })
        .collect()
}

fn to_entries(values: &[player_globals::GlobalValue]) -> Vec<PlayerGlobalEntry> {
    values.iter().map(|v| PlayerGlobalEntry { short_id: v.global.short_id, file: v.global.file.clone(), value: v.value }).collect()
}

impl PlayerGlobalsList {
    fn player_globals_listed(&self) -> Vec<PlayerGlobalEntry> {
        self.0.listed().iter().map(|d| PlayerGlobalEntry { short_id: d.short_id, file: d.file.clone(), value: 0.0 }).collect()
    }

    fn player_globals_check(&self, report: &[PlayerGlobalEntry]) -> u8 {
        match self.0.check(&to_values(report)) {
            Ok(()) => 0,
            Err(player_globals::Refusal::Unlisted) => 1,
            Err(player_globals::Refusal::NotFinite) => 2,
            Err(player_globals::Refusal::Duplicate) => 3,
            Err(player_globals::Refusal::TooMany) => 4,
        }
    }

    fn player_globals_at_login(&self, record: &[PlayerGlobalEntry]) -> Vec<PlayerGlobalEntry> {
        to_entries(&self.0.at_login(&to_values(record)))
    }
    fn player_globals_lethal(&self, record: &[PlayerGlobalEntry]) -> Vec<PlayerGlobalEntry> {
        to_entries(&self.0.lethal(&to_values(record)))
    }
}

fn player_globals_merge(record: &[PlayerGlobalEntry], report: &[PlayerGlobalEntry]) -> Vec<PlayerGlobalEntry> {
    to_entries(&player_globals::merge(&to_values(record), &to_values(report)))
}

/// The quests whose stages are the world's (wire-rules quest_stages).
pub struct SharedQuests(quest_stages::SharedQuests);

fn new_shared_quests(settings_json: &str) -> Result<Box<SharedQuests>, clock::SettingsError> {
    quest_stages::SharedQuests::from_json(settings_json).map(|l| Box::new(SharedQuests(l)))
}

fn to_stages(entries: &[QuestStageEntry]) -> Vec<quest_stages::StageDone> {
    entries
        .iter()
        .map(|e| quest_stages::StageDone { quest: forms::FormDesc { short_id: e.short_id, file: e.file.clone() }, stage: e.stage })
        .collect()
}

fn to_stage(e: &QuestStageEntry) -> quest_stages::StageDone {
    quest_stages::StageDone { quest: forms::FormDesc { short_id: e.short_id, file: e.file.clone() }, stage: e.stage }
}

fn to_stage_entries(stages: &[quest_stages::StageDone]) -> Vec<QuestStageEntry> {
    stages.iter().map(|s| QuestStageEntry { short_id: s.quest.short_id, file: s.quest.file.clone(), stage: s.stage }).collect()
}

impl SharedQuests {
    fn shared_quests_mode(&self) -> u8 {
        self.0.mode().wire()
    }

    fn shared_quests_listed(&self) -> Vec<QuestStageEntry> {
        quest_entries(&self.0.listed())
    }

    fn shared_quests_check(&self, short_id: u32, file: &str) -> u8 {
        match self.0.check(&forms::FormDesc { short_id, file: file.to_owned() }) {
            Ok(()) => 0,
            Err(quest_stages::Refusal::Unlisted) => 1,
            Err(quest_stages::Refusal::Denied) => 2,
            Err(quest_stages::Refusal::Player) => 3,
        }
    }

    fn shared_quests_retired(&self) -> String {
        self.0.retired().unwrap_or_default()
    }

    fn shared_quests_set_player(&mut self, quests: &[QuestStageEntry]) {
        self.0.set_player(&form_descs(quests));
    }

    fn shared_quests_at_login(&self, record: &[QuestStageEntry]) -> Vec<QuestStageEntry> {
        to_stage_entries(&self.0.at_login(&to_stages(record)))
    }
}

fn quest_stage_done(record: &[QuestStageEntry], done: &QuestStageEntry) -> bool {
    quest_stages::done(&to_stages(record), &to_stage(done))
}

fn quest_stages_merge(record: &[QuestStageEntry], done: &QuestStageEntry) -> Vec<QuestStageEntry> {
    to_stage_entries(&quest_stages::merge(&to_stages(record), &to_stage(done)))
}

fn quest_record_full(record: &[QuestStageEntry]) -> bool {
    quest_stages::full(record.len())
}

fn lock_state_of(row: &LockStateRow) -> locks::LockState {
    locks::LockState { level: row.level, key: row.key, locked: row.locked, broken: row.broken }
}

fn lock_row_of(state: locks::LockState) -> LockStateRow {
    LockStateRow { level: state.level, key: state.key, locked: state.locked, broken: state.broken }
}

fn lock_activation(state: &LockStateRow, has_key: bool) -> LockActivationVerdict {
    let (verdict, after) = locks::activation(lock_state_of(state), has_key);
    LockActivationVerdict {
        verdict: match verdict {
            locks::Activation::Open => 0,
            locks::Activation::OpenedWithKey => 1,
            locks::Activation::Locked => 2,
        },
        state: lock_row_of(after),
    }
}

fn lock_report(state: &LockStateRow, outcome: u8, broken: u8, within_reach: bool, picks_held: u32, since_last_ms: u64) -> LockReportVerdict {
    let outcome = match outcome {
        0 => locks::Outcome::Picked,
        1 => locks::Outcome::Broken(broken),
        _ => locks::Outcome::Left,
    };
    let since = (since_last_ms != u64::MAX).then_some(since_last_ms);
    match locks::report(lock_state_of(state), outcome, within_reach, picks_held, since) {
        Ok((change, after)) => {
            let (change, picks) = match change {
                locks::Change::Nothing => (0, 0),
                locks::Change::Unlocked => (1, 0),
                locks::Change::PicksBroken(n) => (2, n),
            };
            LockReportVerdict { accepted: true, why: 0, change, picks, state: lock_row_of(after) }
        }
        Err(why) => LockReportVerdict {
            accepted: false,
            why: match why {
                locks::Refusal::Open => 1,
                locks::Refusal::Reach => 2,
                locks::Refusal::Key => 3,
                locks::Refusal::NoPick => 4,
                locks::Refusal::Range => 5,
                locks::Refusal::Rate => 6,
            },
            change: 0,
            picks: 0,
            state: state.clone(),
        },
    }
}

fn destruction_of(row: &DestructionRow) -> destruction::Destruction {
    destruction::Destruction { stage: row.stage, destroyed: row.destroyed }
}

fn destruction_row_of(d: destruction::Destruction) -> DestructionRow {
    DestructionRow { stage: d.stage, destroyed: d.destroyed }
}

fn destruction_report(record: &DestructionRow, stage: u8, destroyed: bool) -> DestructionVerdict {
    match destruction::report(destruction_of(record), stage, destroyed) {
        destruction::ReportVerdict::Recorded(after) => DestructionVerdict { verdict: 0, record: destruction_row_of(after) },
        destruction::ReportVerdict::Stands(kept) => DestructionVerdict { verdict: 1, record: destruction_row_of(kept) },
        destruction::ReportVerdict::Range => DestructionVerdict { verdict: 2, record: record.clone() },
    }
}

const fn destruction_max_stage() -> u8 {
    destruction::MAX_STAGE
}

/// The eighteen skills' constants (wire-rules leveling).
pub struct LevelingTable(leveling::Table);

fn new_leveling_table() -> Box<LevelingTable> {
    Box::new(LevelingTable(leveling::Table::uesp()))
}

fn leveling_character_threshold(level: u16) -> f32 {
    leveling::character_threshold(level)
}

impl LevelingTable {
    fn set_skill(&mut self, skill_av: u8, use_mult: f32, use_offset: f32, improve_mult: f32, improve_offset: f32) -> bool {
        let Some(skill) = leveling::Skill::from_actor_value(skill_av) else {
            return false;
        };
        let constants = leveling::Constants { use_mult, use_offset, improve_mult, improve_offset };
        let before = self.0.constants(skill);
        self.0.set(skill, constants);
        self.0.constants(skill) == constants || before == constants
    }

    fn use_xp(&self, skill_av: u8, base: f32) -> f32 {
        leveling::Skill::from_actor_value(skill_av).map_or(0.0, |skill| leveling::use_xp(&self.0, skill, base))
    }

    fn level_cost(&self, skill_av: u8, level: u8) -> f32 {
        leveling::Skill::from_actor_value(skill_av).map_or(0.0, |skill| leveling::level_cost(&self.0, skill, level))
    }

    fn apply_use(&self, skills: &[LevelingSkill], character: LevelingCharacter, skill_av: u8, base: f32) -> LevelingOutcome {
        self.judge(skills, character, |progress, table| progress.apply_use(table, skill_av, base))
    }

    fn apply_book(&self, skills: &[LevelingSkill], character: LevelingCharacter, skill_av: u8) -> LevelingOutcome {
        self.judge(skills, character, |progress, table| progress.apply_book(table, skill_av))
    }

    /// The record's standing judged by `step`, the outcome in the bridge's shape.
    fn judge(
        &self,
        skills: &[LevelingSkill],
        character: LevelingCharacter,
        step: impl FnOnce(&mut leveling::Progress, &leveling::Table) -> Result<Vec<leveling::Event>, leveling::Refusal>,
    ) -> LevelingOutcome {
        let mut standing = [leveling::SkillProgress { level: leveling::SKILL_FLOOR, xp: 0.0 }; 18];
        for s in skills {
            if let Some(skill) = leveling::Skill::from_actor_value(s.skill_av) {
                if let Some(slot) = standing.get_mut(usize::from(skill.index())) {
                    *slot = leveling::SkillProgress { level: s.level.clamp(leveling::SKILL_FLOOR, leveling::SKILL_CAP), xp: if s.xp.is_finite() && s.xp >= 0.0 { s.xp } else { 0.0 } };
                }
            }
        }
        let mut progress = leveling::Progress::from_record(
            standing,
            leveling::Character { level: character.level.clamp(1, leveling::LEVEL_CAP), xp: if character.xp.is_finite() && character.xp >= 0.0 { character.xp } else { 0.0 } },
        );
        let (accepted, why, events) = match step(&mut progress, &self.0) {
            Ok(events) => (
                true,
                0,
                events
                    .into_iter()
                    .map(|e| match e {
                        leveling::Event::SkillLevel { skill, level } => LevelingEvent { kind: 0, skill_av: skill.actor_value(), level: u16::from(level) },
                        leveling::Event::CharacterLevel { level } => LevelingEvent { kind: 1, skill_av: 0, level },
                    })
                    .collect(),
            ),
            Err(leveling::Refusal::Skill) => (false, 1, Vec::new()),
            Err(leveling::Refusal::NonFinite) => (false, 2, Vec::new()),
            Err(leveling::Refusal::Range) => (false, 3, Vec::new()),
        };
        LevelingOutcome {
            accepted,
            why,
            skills: leveling::Skill::ALL
                .iter()
                .map(|skill| {
                    let s = progress.skill(*skill);
                    LevelingSkill { skill_av: skill.actor_value(), level: s.level, xp: s.xp }
                })
                .collect(),
            character: LevelingCharacter { level: progress.character.level, xp: progress.character.xp },
            events,
        }
    }
}

/// The spell-placed objects' table and the sets standing (wire-rules
/// placements).
pub struct Placements(placements::Placements);

fn new_placements(settings_json: &str) -> Result<Box<Placements>, clock::SettingsError> {
    placements::Placements::from_json(settings_json).map(|t| Box::new(Placements(t)))
}

impl Placements {
    fn is_listed(&self, short_id: u32, file: &str) -> bool {
        self.0.is_listed(short_id, file)
    }

    fn cast_started(&mut self, short_id: u32, file: &str, cast: PlacedCast, now_ms: u64) -> bool {
        let PlacedCast { caster, hand, x, y, z, yaw, concentration } = cast;
        let cast = placements::CastStart { caster, hand, pose: placements::CasterPose { x, y, z, yaw }, concentration };
        self.0.cast_started(short_id, file, cast, now_ms)
    }

    fn cast_ended(&mut self, caster: u32, hand: u8, now_ms: u64) {
        self.0.cast_ended(caster, hand, now_ms);
    }

    fn caster_gone(&mut self, caster: u32, now_ms: u64) {
        self.0.caster_gone(caster, now_ms);
    }

    fn due_placements(&mut self, now_ms: u64) -> Vec<PlacementRow> {
        self.0
            .due_placements(now_ms)
            .into_iter()
            .flat_map(|p| {
                let (set, caster, spell) = (p.set, p.caster, p.spell);
                p.objects.into_iter().map(move |o| PlacementRow {
                    set,
                    caster,
                    spell_short_id: spell.short_id,
                    spell_file: spell.file.clone(),
                    short_id: o.base.short_id,
                    file: o.base.file,
                    record_type: o.kind.record_type().to_owned(),
                    x: o.x,
                    y: o.y,
                    z: o.z,
                    yaw: o.yaw,
                })
            })
            .collect()
    }

    fn due_removals(&mut self, now_ms: u64) -> Vec<PlacementRemoval> {
        self.0
            .due_removals(now_ms)
            .into_iter()
            .map(|r| PlacementRemoval {
                set: r.set,
                caster: r.caster,
                why: match r.why {
                    placements::RemovalWhy::Lingered => 0,
                    placements::RemovalWhy::MaxReached => 1,
                },
            })
            .collect()
    }

    fn take_refusals(&mut self) -> Vec<PlacementRefusal> {
        self.0
            .take_refusals()
            .into_iter()
            .map(|r| PlacementRefusal {
                caster: r.caster,
                spell_short_id: r.spell.short_id,
                spell_file: r.spell.file,
                why: match r.why {
                    placements::Refusal::PerCaster => 0,
                    placements::Refusal::ServerWide => 1,
                    placements::Refusal::NoReport => 2,
                },
            })
            .collect()
    }

    fn take_requests(&mut self) -> Vec<PlacementRequestRow> {
        self.0
            .take_requests()
            .into_iter()
            .flat_map(|r| {
                let (set, caster, spell, window_ms) = (r.set, r.caster, r.spell, r.window_ms);
                r.objects.into_iter().map(move |o| PlacementRequestRow {
                    set,
                    caster,
                    spell_short_id: spell.short_id,
                    spell_file: spell.file.clone(),
                    short_id: o.base.short_id,
                    file: o.base.file,
                    record_type: o.kind.record_type().to_owned(),
                    window_ms,
                })
            })
            .collect()
    }

    #[allow(clippy::too_many_arguments)]
    fn open_script_set(&mut self, caster: u32, self_id: u32, x: f32, y: f32, z: f32, short_id: u32, file: &str, record_type: &str, now_ms: u64) -> ScriptSetOpen {
        let anchor = placements::CasterPose { x, y, z, yaw: 0.0 };
        let base = forms::FormDesc { short_id, file: file.to_owned() };
        match self.0.open_script_set(caster, self_id, anchor, base, record_type, now_ms) {
            Ok(set) => ScriptSetOpen { set, refusal: 0 },
            Err(placements::ScriptRefusal::Kind) => ScriptSetOpen { set: 0, refusal: 1 },
            Err(placements::ScriptRefusal::ServerWide) => ScriptSetOpen { set: 0, refusal: 2 },
            Err(placements::ScriptRefusal::NonFinite) => ScriptSetOpen { set: 0, refusal: 3 },
        }
    }

    fn take_script_requests(&mut self) -> Vec<ScriptRequestRow> {
        self.0
            .take_script_requests()
            .into_iter()
            .map(|r| ScriptRequestRow { set: r.set, caster: r.caster, self_id: r.self_id, short_id: r.base.short_id, file: r.base.file, record_type: r.record_type, window_ms: r.window_ms })
            .collect()
    }

    fn due_script_placements(&mut self, now_ms: u64) -> Vec<ScriptPlacementRow> {
        self.0
            .due_script_placements(now_ms)
            .into_iter()
            .map(|o| match o {
                placements::ScriptOutcome::Placed(p) => ScriptPlacementRow {
                    placed: true,
                    set: p.set,
                    caster: p.caster,
                    self_id: p.self_id,
                    short_id: p.base.short_id,
                    file: p.base.file,
                    record_type: p.record_type,
                    x: p.x,
                    y: p.y,
                    z: p.z,
                    yaw: p.yaw,
                },
                placements::ScriptOutcome::NoReport(r) => ScriptPlacementRow {
                    placed: false,
                    set: r.set,
                    caster: r.caster,
                    self_id: r.self_id,
                    short_id: r.base.short_id,
                    file: r.base.file,
                    record_type: r.record_type,
                    x: 0.0,
                    y: 0.0,
                    z: 0.0,
                    yaw: 0.0,
                },
            })
            .collect()
    }

    fn report_placed(&mut self, report: PlacementReport, now_ms: u64) -> PlacementReportVerdict {
        let PlacementReport { caster, set, short_id, file, x, y, z, yaw } = report;
        let report = placements::Report { caster, set, base: forms::FormDesc { short_id, file }, x, y, z, yaw };
        match self.0.report_placed(report, now_ms) {
            Ok(()) => PlacementReportVerdict { accepted: true, why: 0, distance: 0.0, dz: 0.0 },
            Err(why) => {
                let (code, distance, dz) = match why {
                    placements::ReportRefusal::NoSet => (1, 0.0, 0.0),
                    placements::ReportRefusal::Late => (2, 0.0, 0.0),
                    placements::ReportRefusal::Base => (3, 0.0, 0.0),
                    placements::ReportRefusal::Repeated => (4, 0.0, 0.0),
                    placements::ReportRefusal::NonFinite => (5, 0.0, 0.0),
                    placements::ReportRefusal::Range { distance, dz } => (6, distance, dz),
                };
                PlacementReportVerdict { accepted: false, why: code, distance, dz }
            }
        }
    }

    fn live_sets(&self) -> usize {
        self.0.live_sets()
    }
}

/// The cell host table (wire-rules cells, thuum docs/verbs/cell-host.md).
#[derive(Debug, Default)]
pub struct Cells(cells::Cells);

fn new_cells() -> Box<Cells> {
    Box::default()
}

fn host_decisions(table: &cells::Cells, decisions: Vec<cells::Decision>) -> Vec<HostDecision> {
    decisions
        .into_iter()
        .map(|d| match d {
            cells::Decision::Grant { player, cell, epoch } => HostDecision { actor: 0, kind: HostKind::Grant, player, cell, epoch },
            cells::Decision::Release { player, cell, epoch } => {
                HostDecision { actor: 0, kind: HostKind::Release, player, cell, epoch }
            }
            cells::Decision::Unhosted { cell } => {
                HostDecision { actor: 0, kind: HostKind::Unhosted, player: 0, cell, epoch: table.epoch_of(cell) }
            }
            cells::Decision::ClaimEnded { actor, cell } => {
                HostDecision { actor, kind: HostKind::ClaimEnded, player: 0, cell, epoch: table.epoch_of(cell) }
            }
            cells::Decision::ClaimGone { actor, cell, player } => {
                HostDecision { actor, kind: HostKind::ClaimGone, player, cell, epoch: table.epoch_of(cell) }
            }
            cells::Decision::ThrallExpired { actor, cell } => {
                HostDecision { actor, kind: HostKind::ThrallExpired, player: 0, cell, epoch: table.epoch_of(cell) }
            }
        })
        .collect()
}

impl Cells {
    fn reach(&mut self, player: u32, cells: &[u32], now_ms: u64) -> Vec<HostDecision> {
        let decisions = self.0.reach(player, cells, now_ms);
        host_decisions(&self.0, decisions)
    }

    fn rtt(&mut self, player: u32, ms: u32) {
        self.0.rtt(player, ms);
    }

    fn loaded(&mut self, player: u32, cell: u32, now_ms: u64) -> Vec<HostDecision> {
        let decisions = self.0.loaded(player, cell, now_ms);
        host_decisions(&self.0, decisions)
    }

    fn unloaded(&mut self, player: u32, cell: u32, now_ms: u64) -> Vec<HostDecision> {
        let decisions = self.0.unloaded(player, cell, now_ms);
        host_decisions(&self.0, decisions)
    }

    fn gone(&mut self, player: u32, now_ms: u64) -> Vec<HostDecision> {
        let decisions = self.0.gone(player, now_ms);
        host_decisions(&self.0, decisions)
    }

    fn stale(&mut self, player: u32, now_ms: u64) -> Vec<HostDecision> {
        let decisions = self.0.stale(player, now_ms);
        host_decisions(&self.0, decisions)
    }

    fn revived(&mut self, player: u32, now_ms: u64) -> Vec<HostDecision> {
        let decisions = self.0.revived(player, now_ms);
        host_decisions(&self.0, decisions)
    }

    fn died(&mut self, player: u32, now_ms: u64) -> Vec<HostDecision> {
        let decisions = self.0.died(player, now_ms);
        host_decisions(&self.0, decisions)
    }

    fn respawned(&mut self, player: u32, now_ms: u64) -> Vec<HostDecision> {
        let decisions = self.0.respawned(player, now_ms);
        host_decisions(&self.0, decisions)
    }

    fn tick(&mut self, now_ms: u64) -> Vec<HostDecision> {
        let decisions = self.0.tick(now_ms);
        host_decisions(&self.0, decisions)
    }

    fn host_of(&self, cell: u32) -> CellHost {
        match self.0.host_of(cell) {
            Some((player, epoch)) => CellHost { hosted: true, player, epoch },
            None => CellHost { hosted: false, player: 0, epoch: self.0.epoch_of(cell) },
        }
    }

    fn accepts(&self, cell: u32, player: u32, epoch: u32) -> bool {
        self.0.accepts(cell, player, epoch)
    }

    fn epoch_of(&self, cell: u32) -> u32 {
        self.0.epoch_of(cell)
    }

    fn loaders_of(&self, cell: u32) -> Vec<u32> {
        self.0.loaders_of(cell)
    }

    fn reach_ranked(&mut self, player: u32, cells: &[u32], rings: &[u8], now_ms: u64) -> Vec<HostDecision> {
        let decisions = self.0.reach_ranked(player, cells, rings, now_ms);
        host_decisions(&self.0, decisions)
    }

    fn judge_move(&self, report: &CellMove, now_ms: u64) -> u8 {
        let m = cells::CellMove {
            sender: report.sender,
            actor: report.actor,
            origin: report.origin,
            dest: report.dest,
            adjacent: report.adjacent,
            door: report.door,
            edge_units: report.edge_units,
        };
        match self.0.judge(&m, now_ms) {
            cells::MoveVerdict::Refuse => 0,
            cells::MoveVerdict::Accept => 1,
            cells::MoveVerdict::Follow => 2,
            cells::MoveVerdict::Pass => 3,
        }
    }

    fn may_move(&mut self, report: &CellMove, now_ms: u64) -> u8 {
        let m = cells::CellMove {
            sender: report.sender,
            actor: report.actor,
            origin: report.origin,
            dest: report.dest,
            adjacent: report.adjacent,
            door: report.door,
            edge_units: report.edge_units,
        };
        match self.0.may_move(&m, now_ms) {
            cells::MoveVerdict::Refuse => 0,
            cells::MoveVerdict::Accept => 1,
            cells::MoveVerdict::Follow => 2,
            cells::MoveVerdict::Pass => 3,
        }
    }

    fn claim_actor(&mut self, actor: u32, player: u32, cell: u32, now_ms: u64) -> u8 {
        match self.0.claim(actor, player, cell, now_ms) {
            cells::ClaimVerdict::Granted => 0,
            cells::ClaimVerdict::Held => 1,
            cells::ClaimVerdict::Unloaded => 2,
            cells::ClaimVerdict::Reclaimed => 3,
        }
    }

    fn claim_commanded(&mut self, actor: u32, player: u32, cell: u32, effect_ms: u64, now_ms: u64) -> u8 {
        match self.0.claim_commanded(actor, player, cell, effect_ms, now_ms) {
            cells::ClaimVerdict::Granted => 0,
            cells::ClaimVerdict::Held => 1,
            cells::ClaimVerdict::Unloaded => 2,
            cells::ClaimVerdict::Reclaimed => 3,
        }
    }

    fn release_claim(&mut self, actor: u32, player: u32) -> bool {
        self.0.release_claim(actor, player)
    }

    fn release_any_claim(&mut self, actor: u32) -> u32 {
        self.0.release_any_claim(actor)
    }

    fn commander_of(&self, actor: u32) -> u32 {
        self.0.commander_of(actor)
    }

    fn simulator_of(&self, actor: u32, cell: u32) -> u32 {
        self.0.simulator_of(actor, cell)
    }

    fn follows(&self) -> Vec<CellFollowRow> {
        self.0.follows().into_iter().map(|(actor, player, cell)| CellFollowRow { actor, player, cell }).collect()
    }

    fn hosts(&self) -> Vec<CellHostRow> {
        self.0
            .hosts()
            .into_iter()
            .map(|(cell, host, epoch)| CellHostRow { cell, hosted: host.is_some(), player: host.unwrap_or(0), epoch })
            .collect()
    }
}

const fn host_grace_ms() -> u64 {
    cells::GRACE_MS
}

/// A claim's verdict as the core's code: 0 granted, 1 held, 2 the same
/// simulator, 3 a world body another seat's firing moves (thuum D-S3).
fn claim_code(verdict: props::ClaimVerdict) -> u8 {
    match verdict {
        props::ClaimVerdict::Granted(_) => 0,
        props::ClaimVerdict::Held => 1,
        props::ClaimVerdict::Same => 2,
        props::ClaimVerdict::Moving => 3,
    }
}

/// The prop owner table (wire-rules props, thuum docs/verbs/prop-physics.md).
#[derive(Debug, Default)]
pub struct Props(props::Props);

fn new_props() -> Box<Props> {
    Box::default()
}

impl Props {
    fn claim_prop(&mut self, refr: u32, player: u32, cell: u32, touch: u8, now_ms: u64) -> u8 {
        claim_code(self.0.claim(refr, player, cell, touch, now_ms))
    }

    fn claim_prop_world_body(&mut self, refr: u32, player: u32, cell: u32, now_ms: u64) -> u8 {
        claim_code(self.0.claim_world_body(refr, player, cell, now_ms))
    }

    fn prop_epoch_of(&self, refr: u32) -> u32 {
        self.0.epoch_of(refr)
    }

    fn prop_transform(&mut self, f: &PropSampleFacts) -> u8 {
        let facts = props::SampleFacts {
            refr: f.refr,
            player: f.player,
            cell: f.cell,
            cell_host: f.cell_host,
            asleep: f.asleep,
            moved: f.moved,
            now_ms: f.now_ms,
        };
        match self.0.transform(&facts) {
            props::TransformVerdict::Accepted => 0,
            props::TransformVerdict::NotOwner => 1,
            props::TransformVerdict::WrongCell => 2,
            props::TransformVerdict::TooFast => 3,
        }
    }

    fn prop_live_owner(&self, refr: u32, now_ms: u64) -> u32 {
        self.0.live_owner(refr, now_ms)
    }

    fn prop_owner_of(&self, refr: u32, cell_host: u32, now_ms: u64) -> u32 {
        self.0.owner_of(refr, cell_host, now_ms)
    }

    fn props_host_changed(&mut self, cell: u32, host: u32, now_ms: u64) {
        self.0.host_changed(cell, host, now_ms);
    }

    fn props_gone(&mut self, player: u32) {
        self.0.gone(player);
    }
}

const fn prop_lease_ms() -> u64 {
    props::PROP_LEASE_MS
}

/// The spells the server names as teleports (wire-rules teleports).
pub struct CastTeleports(teleports::CastTeleports);

/// The headshot rule (wire-rules headshots).
#[derive(Debug, Clone, Default)]
pub struct Headshots(headshots::Headshots);

fn new_headshots(settings_json: &str) -> Result<Box<Headshots>, clock::SettingsError> {
    headshots::Headshots::from_json(settings_json).map(|h| Box::new(Headshots(h)))
}

impl Headshots {
    fn is_headshot_spell(&self, short_id: u32, file: &str) -> bool {
        self.0.is_headshot_spell(short_id, file)
    }

    fn note_bow_hit(&mut self, actor: u32, target: u32, now_ms: u64) {
        self.0.note_bow_hit(actor, target, now_ms);
    }

    fn claim(&mut self, actor: u32, target: u32, facts: &HeadshotFacts, now_ms: u64) -> HeadshotCheck {
        let f = headshots::HeadshotFacts {
            head_bare: facts.head_bare,
            race_short: facts.race_short,
            race_file: facts.race_file.clone(),
            archery: facts.archery,
        };
        match self.0.claim(actor, target, &f, now_ms) {
            headshots::HeadshotCheck::Killed => HeadshotCheck::Killed,
            headshots::HeadshotCheck::NoShot => HeadshotCheck::NoShot,
            headshots::HeadshotCheck::Helmeted => HeadshotCheck::Helmeted,
            headshots::HeadshotCheck::Race => HeadshotCheck::Race,
            headshots::HeadshotCheck::Archery => HeadshotCheck::Archery,
        }
    }

    fn forget(&mut self, actor: u32) {
        self.0.forget(actor);
    }
}

/// The scripted effects the server runs (wire-rules scripted_effects).
#[derive(Debug, Clone, Default)]
pub struct ScriptedEffects(scripted_effects::ScriptedEffects);

fn new_scripted_effects(settings_json: &str) -> Result<Box<ScriptedEffects>, clock::SettingsError> {
    scripted_effects::ScriptedEffects::from_json(settings_json).map(|s| Box::new(ScriptedEffects(s)))
}

impl ScriptedEffects {
    fn is_kill_spell(&self, short_id: u32, file: &str) -> bool {
        self.0.is_kill_spell(short_id, file)
    }

    fn kill_percentage(&self, short_id: u32, file: &str) -> f32 {
        self.0.percentage(short_id, file)
    }
}

fn finger_of_death(target_health: f32, caster_health: f32, percentage: f32) -> DeathBlast {
    let b = scripted_effects::finger_of_death(target_health, caster_health, percentage);
    DeathBlast { target_damage: b.target_damage, caster_damage: b.caster_damage, kill: b.kill }
}

/// The bonds rule (wire-rules bonds).
#[derive(Debug, Clone, Default)]
pub struct Bonds(bonds::Bonds);

fn new_bonds(settings_json: &str) -> Result<Box<Bonds>, clock::SettingsError> {
    bonds::Bonds::from_json(settings_json).map(|b| Box::new(Bonds(b)))
}

/// A bond as the bridge carries it; `found` false for none.
fn bond_facts(bond: Option<bonds::Bond>) -> BondBreak {
    match bond {
        Some(b) => BondBreak {
            found: true,
            caster: b.caster,
            target: b.target,
            window_ms: b.window_ms,
            slack_units: b.slack_units,
            duration_ms: b.duration_ms,
            has_proc: b.proc.is_some(),
            proc_short_id: b.proc.as_ref().map_or(0, |p| p.short_id),
            proc_file: b.proc.map_or_else(String::new, |p| p.file),
        },
        None => BondBreak {
            found: false,
            caster: 0,
            target: 0,
            window_ms: 0,
            slack_units: 0.0,
            duration_ms: 0,
            has_proc: false,
            proc_short_id: 0,
            proc_file: String::new(),
        },
    }
}

impl Bonds {
    fn bond_duration_ms(&self, short_id: u32, file: &str) -> u64 {
        self.0.named(short_id, file).map_or(0, |s| s.duration_ms)
    }

    fn record(&mut self, caster: u32, target: u32, short_id: u32, file: &str, now_ms: u64) -> BondBreak {
        match self.0.named(short_id, file).cloned() {
            Some(spell) => bond_facts(Some(self.0.record(caster, target, &spell, now_ms))),
            None => bond_facts(None),
        }
    }

    fn supersede(&mut self, caster: u32, now_ms: u64) -> BondBreak {
        bond_facts(self.0.supersede(caster, now_ms))
    }

    fn break_of(&mut self, actor: u32, now_ms: u64) -> BondBreak {
        bond_facts(self.0.break_of(actor, now_ms))
    }

    fn expired(&mut self, now_ms: u64) -> Vec<BondBreak> {
        self.0.expired(now_ms).into_iter().map(|b| bond_facts(Some(b))).collect()
    }

    fn forget(&mut self, actor: u32) {
        self.0.forget(actor);
    }
}

fn new_cast_teleports(settings_json: &str) -> Result<Box<CastTeleports>, clock::SettingsError> {
    teleports::CastTeleports::from_json(settings_json).map(|t| Box::new(CastTeleports(t)))
}

impl CastTeleports {
    fn teleport_reach(&self, short_id: u32, file: &str) -> f32 {
        self.0.reach_of(short_id, file).unwrap_or(-1.0)
    }

    fn teleport_wait_ms(&self, short_id: u32, file: &str) -> u64 {
        self.0.named(short_id, file).map_or(0, |t| t.wait_ms)
    }

    fn teleport_relays(&self, short_id: u32, file: &str) -> bool {
        self.0.named(short_id, file).is_some_and(|t| t.relay)
    }

    fn teleport_look(&self, short_id: u32, file: &str) -> TeleportLook {
        let look = self.0.named(short_id, file).and_then(|t| t.look.as_ref());
        let (has_vanish, vanish) = teleport_look_ref(look.and_then(|l| l.vanish.as_ref()));
        let (has_appear, appear) = teleport_look_ref(look.and_then(|l| l.appear.as_ref()));
        let (has_sound, sound) = teleport_look_ref(look.and_then(|l| l.sound.as_ref()));
        TeleportLook {
            has_vanish,
            vanish,
            has_appear,
            appear,
            appear_seconds: look.map_or(0.0, |l| l.appear_seconds),
            has_sound,
            sound,
        }
    }
}

/// A teleport look's form for the core: whether the look names it, and the
/// form as the entry writes it (an empty one when it does not).
fn teleport_look_ref(spec: Option<&teleports::FormSpec>) -> (bool, LookRef) {
    spec.map_or_else(
        || (false, LookRef { plugin: String::new(), id: 0 }),
        |s| (true, LookRef { plugin: s.file.clone(), id: s.short_id }),
    )
}

fn teleport_look_lands(step: f32) -> bool {
    teleports::look_lands(step)
}

fn jump_check(c: movement::JumpCheck) -> JumpCheck {
    match c {
        movement::JumpCheck::NoPermit => JumpCheck::NoPermit,
        movement::JumpCheck::Landed => JumpCheck::Landed,
        movement::JumpCheck::Waiting => JumpCheck::Waiting,
    }
}

/// Every player actor's recent bow and crossbow shots (wire-rules ranged).
#[derive(Debug, Default)]
pub struct RangedShots(ranged::Shots);

fn new_ranged_shots() -> Box<RangedShots> {
    Box::default()
}

impl RangedShots {
    fn record(&mut self, actor: u32, shot: ShotFacts, now_ms: u64) {
        let ShotFacts { weapon, power, x, y, z } = shot;
        self.0.record(actor, ranged::ShotFacts { weapon, power, x, y, z }, now_ms);
    }

    fn claim(&mut self, actor: u32, weapon: u32, x: f32, y: f32, z: f32, now_ms: u64) -> ShotClaim {
        match self.0.claim(actor, weapon, x, y, z, now_ms) {
            ranged::ShotCheck::Claimed { power } => ShotClaim { check: ShotCheck::Claimed, power },
            ranged::ShotCheck::NoShot => ShotClaim { check: ShotCheck::NoShot, power: 0.0 },
            ranged::ShotCheck::TooFar => ShotClaim { check: ShotCheck::TooFar, power: 0.0 },
        }
    }

    fn forget(&mut self, actor: u32) {
        self.0.forget(actor);
    }
}

fn to_magic_facts(f: EffectFacts) -> magic::EffectFacts {
    let EffectFacts { effect, source, kind, av, second_av, second_weight, magnitude, duration_s, recover, detrimental, no_duration } = f;
    let kind = match kind {
        EffectKind::PeakValue => magic::EffectKind::PeakValue,
        EffectKind::DualValue => magic::EffectKind::DualValue,
        EffectKind::State => magic::EffectKind::State,
        EffectKind::Reanimate => magic::EffectKind::Reanimate,
        _ => magic::EffectKind::Value,
    };
    let second_av = if second_av == u32::MAX { None } else { Some(second_av) };
    magic::EffectFacts { effect, source, kind, av, second_av, second_weight, magnitude, duration_s, recover, detrimental, no_duration }
}

fn from_magic_facts(f: magic::EffectFacts) -> EffectFacts {
    let magic::EffectFacts { effect, source, kind, av, second_av, second_weight, magnitude, duration_s, recover, detrimental, no_duration } = f;
    let kind = match kind {
        magic::EffectKind::Value => EffectKind::Value,
        magic::EffectKind::PeakValue => EffectKind::PeakValue,
        magic::EffectKind::DualValue => EffectKind::DualValue,
        magic::EffectKind::State => EffectKind::State,
        magic::EffectKind::Reanimate => EffectKind::Reanimate,
    };
    EffectFacts { effect, source, kind, av, second_av: second_av.unwrap_or(u32::MAX), second_weight, magnitude, duration_s, recover, detrimental, no_duration }
}

fn to_magic_entry(e: &EffectEntry) -> magic::EffectEntry {
    magic::EffectEntry { facts: to_magic_facts(e.facts), caster: e.caster, elapsed_s: e.elapsed_s }
}

fn from_magic_entry(e: &magic::EffectEntry) -> EffectEntry {
    EffectEntry { facts: from_magic_facts(e.facts), caster: e.caster, elapsed_s: e.elapsed_s }
}

fn from_magic_changes(changes: &[magic::AvChange]) -> Vec<AvChange> {
    changes.iter().map(|c| AvChange { av: c.av, current: c.current, modifier: c.modifier }).collect()
}

fn apply_effect(facts: EffectFacts, scale: f32, caster: u32, arrival: EffectArrival) -> EffectApplied {
    let arrival = match arrival {
        EffectArrival::Stream => magic::Arrival::Stream,
        _ => magic::Arrival::Once,
    };
    let applied = magic::apply(to_magic_facts(facts), scale, caster, arrival);
    let changes = from_magic_changes(&applied.changes);
    match applied.entry {
        Some(e) => EffectApplied { changes, has_entry: true, entry: from_magic_entry(&e) },
        None => EffectApplied { changes, has_entry: false, entry: EffectEntry { facts, caster, elapsed_s: 0.0 } },
    }
}

/// Runs a magic rule over the entries the C++ change form holds, writing
/// them back.
fn with_entries(entries: &mut Vec<EffectEntry>, run: impl FnOnce(&mut Vec<magic::EffectEntry>) -> Vec<magic::AvChange>) -> Vec<AvChange> {
    let mut ours: Vec<magic::EffectEntry> = entries.iter().map(to_magic_entry).collect();
    let changes = run(&mut ours);
    *entries = ours.iter().map(from_magic_entry).collect();
    from_magic_changes(&changes)
}

fn advance_effects(entries: &mut Vec<EffectEntry>, dt_s: f32) -> Vec<AvChange> {
    with_entries(entries, |ours| magic::advance(ours, dt_s))
}

fn admit_effect(entries: &mut Vec<EffectEntry>, entry: EffectEntry) -> Vec<AvChange> {
    let entry = to_magic_entry(&entry);
    with_entries(entries, |ours| magic::admit(ours, entry))
}

fn remove_effects(entries: &mut Vec<EffectEntry>, effects: &[u32]) -> Vec<AvChange> {
    with_entries(entries, |ours| magic::remove(ours, effects))
}

// cxx passes a C++ rust::Vec by reference as &Vec
#[allow(clippy::ptr_arg)]
fn effect_modifier(entries: &Vec<EffectEntry>, av: u32) -> f32 {
    let ours: Vec<magic::EffectEntry> = entries.iter().map(to_magic_entry).collect();
    magic::modifier(&ours, av)
}

// cxx passes a C++ rust::Vec by reference as &Vec
#[allow(clippy::ptr_arg)]
fn ability_modifier(facts: &Vec<EffectFacts>, av: u32) -> f32 {
    let ours: Vec<magic::EffectFacts> = facts.iter().map(|f| to_magic_facts(*f)).collect();
    magic::constant_modifier(&ours, av)
}

fn resistance_factor(resist: f32, magic_resist: f32, cap: f32) -> f32 {
    magic::resistance_factor(resist, magic_resist, cap)
}

const fn full_thrall_min_s() -> f32 {
    magic::FULL_THRALL_MIN_S
}

/// Every player actor's recent spell casts and open streams (wire-rules
/// casts).
#[derive(Debug, Default)]
pub struct SpellCasts(casts::Casts);

// the bridge's reach array is wire-rules' MAX_REACHED long
const _: () = assert!(casts::MAX_REACHED == 8);

fn cast_claim(check: casts::CastCheck) -> CastClaim {
    match check {
        casts::CastCheck::Claimed { scale, relay, dual } => CastClaim { check: CastCheck::Claimed, scale, relay, dual },
        casts::CastCheck::NoCast => CastClaim { check: CastCheck::NoCast, scale: 0.0, relay: false, dual: false },
        casts::CastCheck::TooFar => CastClaim { check: CastCheck::TooFar, scale: 0.0, relay: false, dual: false },
        casts::CastCheck::Throttled => CastClaim { check: CastCheck::Throttled, scale: 0.0, relay: false, dual: false },
    }
}

fn new_spell_casts() -> Box<SpellCasts> {
    Box::default()
}

impl SpellCasts {
    fn start(&mut self, actor: u32, facts: CastFacts, now_ms: u64) -> bool {
        let CastFacts { spell, hand, kind, area, reach, x, y, z, reached, linger_ms, reached_concentration, lands_on_target, hazard_ms, dual } = facts;
        let kind = match kind {
            CastKind::Concentration => casts::CastKind::Concentration,
            _ => casts::CastKind::FireAndForget,
        };
        self.0.start(actor, casts::CastFacts { spell, hand, kind, area, reach, x, y, z, reached, linger_ms, reached_concentration, lands_on_target, hazard_ms, dual }, now_ms)
    }

    fn end(&mut self, actor: u32, hand: u8) {
        self.0.end(actor, hand);
    }

    fn is_open(&self, actor: u32, hand: u8) -> bool {
        self.0.is_open(actor, hand)
    }

    fn claim(&mut self, actor: u32, hit: CastHit, now_ms: u64) -> CastClaim {
        let CastHit { spell, target, x, y, z } = hit;
        cast_claim(self.0.claim(actor, casts::CastHit { spell, target, x, y, z }, now_ms))
    }

    fn claim_reached(
        &mut self,
        actor: u32,
        hit: CastHit,
        caster_x: f32,
        caster_y: f32,
        caster_z: f32,
        now_ms: u64,
    ) -> CastClaim {
        let CastHit { spell, target, x, y, z } = hit;
        cast_claim(self.0.claim_reached(
            actor,
            casts::CastHit { spell, target, x, y, z },
            [caster_x, caster_y, caster_z],
            now_ms,
        ))
    }

    fn forget(&mut self, actor: u32) {
        self.0.forget(actor);
    }
}

/// Every player's voice (wire-rules shouts).
#[derive(Debug, Default)]
pub struct Voices(shouts::Voices);

fn new_voices() -> Box<Voices> {
    Box::default()
}

impl Voices {
    fn shout(&mut self, actor: u32, f: &VoiceFacts, now_ms: u64) -> VoiceRefusal {
        let facts = shouts::VoiceFacts {
            shout_equipped: f.shout_equipped,
            variation: f.variation,
            words_unlocked: [f.words_unlocked & 1 != 0, f.words_unlocked & 2 != 0, f.words_unlocked & 4 != 0],
            recovery_s: f.recovery_s,
            recovery_mult: f.recovery_mult,
        };
        match self.0.shout(actor, &facts, now_ms) {
            shouts::VoiceRefusal::Accepted => VoiceRefusal::Accepted,
            shouts::VoiceRefusal::NoShout => VoiceRefusal::NoShout,
            shouts::VoiceRefusal::NotAVariation => VoiceRefusal::NotAVariation,
            shouts::VoiceRefusal::WordLocked => VoiceRefusal::WordLocked,
            shouts::VoiceRefusal::Recovering => VoiceRefusal::Recovering,
        }
    }

    fn recovery_left_ms(&self, actor: u32, now_ms: u64) -> u64 {
        self.0.recovery_left_ms(actor, now_ms)
    }
}

/// The server's game clock (wire-rules clock).
#[derive(Debug)]
pub struct GameClock(clock::Clock);

fn new_game_clock(settings_json: &str) -> Result<Box<GameClock>, clock::SettingsError> {
    clock::Clock::from_json(settings_json).map(|c| Box::new(GameClock(c)))
}

fn game_time(t: clock::GameTime) -> GameTime {
    GameTime {
        year: t.year,
        month: t.month,
        day: t.day,
        hour: t.hour,
        days_passed: t.days_passed,
        time_scale: t.time_scale,
    }
}

impl GameClock {
    fn now(&self, now_ms: i64) -> GameTime {
        game_time(self.0.now(now_ms))
    }

    fn login(&mut self, user: u32, now_ms: i64) -> GameTime {
        game_time(self.0.login(user, now_ms))
    }

    fn resync_due(&mut self, user: u32, now_ms: i64) -> bool {
        self.0.resync_due(user, now_ms)
    }

    fn forget(&mut self, user: u32) {
        self.0.forget(user);
    }
}

/// Every perk's conditions (wire-rules perks).
#[derive(Debug, Default)]
pub struct PerkTable(perks::Table);

fn new_perk_table() -> Box<PerkTable> {
    Box::default()
}

fn perk_points(level: u16, picks: u16) -> u16 {
    perks::points(level, picks)
}

fn perks_max() -> usize {
    perks::MAX_PERKS
}

impl PerkTable {
    fn add_perk(&mut self, perk: u32, playable: bool, hidden: bool) {
        self.0.add_perk(perk, playable, hidden);
    }

    fn add_condition(&mut self, perk: u32, function: u16, byte0: u8, value: f32, param1: u32, run_on: u32) -> bool {
        // the OR flag joins this condition to the next (wire-rules perks FLAG_OR)
        self.0.add_clause(perk, perks::Condition::from_ctda(function, byte0, value, param1, run_on), byte0 & perks::FLAG_OR != 0)
    }

    fn perk_count(&self) -> usize {
        self.0.len()
    }

    fn knows(&self, perk: u32) -> bool {
        self.0.get(perk).is_some()
    }

    fn add_effect(&mut self, perk: u32, entry_point: u8, function: u8, value: f32, rank: u8, priority: u8) -> i32 {
        self.0.add_effect(perk, entry_point, function, value, rank, priority).and_then(|i| i32::try_from(i).ok()).unwrap_or(-1)
    }

    #[allow(clippy::too_many_arguments)]
    fn add_effect_condition(&mut self, perk: u32, effect: i32, tab: u8, function: u16, byte0: u8, value: f32, param1: u32, run_on: u32) -> bool {
        let Ok(effect) = usize::try_from(effect) else {
            return false;
        };
        self.0.add_effect_condition(perk, effect, tab, perks::Condition::from_ctda(function, byte0, value, param1, run_on), byte0 & perks::FLAG_OR != 0)
    }

    fn judged_effects(&self) -> usize {
        self.0.judged_effects()
    }

    fn unjudged_effects(&self) -> usize {
        self.0.unjudged_effects()
    }

    #[allow(clippy::too_many_arguments)]
    fn apply_entry_point(&self, entry_point: u8, start: f32, level: u16, perks: &[u32], avs: &[u8], values: &[f32], subject_keywords: &[u32]) -> f32 {
        let pairs: Vec<(u8, f32)> = avs.iter().copied().zip(values.iter().copied()).collect();
        // the picks play no part in an entry point's number
        let record = perks::Record { level, perks, picks: 0, values: &pairs };
        perks::apply_entry_point(&self.0, entry_point, start, &record, subject_keywords)
    }

    fn pick(&self, level: u16, held: &[u32], picks: u16, avs: &[u8], values: &[f32], perk: u32) -> PerkVerdict {
        let pairs: Vec<(u8, f32)> = avs.iter().copied().zip(values.iter().copied()).collect();
        let record = perks::Record { level, perks: held, picks, values: &pairs };
        match perks::pick(&self.0, &record, perk) {
            Ok(()) => PerkVerdict { accepted: true, code: String::new(), detail: 0 },
            Err(refusal) => PerkVerdict { accepted: false, code: refusal.code().to_owned(), detail: refusal.detail() },
        }
    }
}

/// The server's weather (wire-rules weather).
#[derive(Debug)]
pub struct WeatherSync(weather::Weather);

fn new_weather_sync(change_game_minutes: u32, seed: u64) -> Result<Box<WeatherSync>, weather::SettingsError> {
    weather::Weather::new(change_game_minutes, seed).map(|w| Box::new(WeatherSync(w)))
}

impl WeatherSync {
    fn set_pool(&mut self, world: u32, weathers: &[u32], chances: &[u32], pinned: bool) -> usize {
        if weathers.len() != chances.len() {
            return 0;
        }
        let entries: Vec<weather::Entry> =
            weathers.iter().zip(chances).map(|(&weather, &chance)| weather::Entry { weather, chance }).collect();
        self.0.set_pool(world, &entries, pinned).unwrap_or(0)
    }

    fn due(&mut self, user: u32, place: u32, is_world: bool, game_minutes: f64) -> WeatherWord {
        match self.0.due(user, place, is_world, game_minutes) {
            Some(w) => WeatherWord { some: true, place: w.place, weather: w.weather, slot: w.slot, transition: w.transition },
            None => WeatherWord { some: false, place: 0, weather: 0, slot: 0, transition: false },
        }
    }

    fn forget(&mut self, user: u32) {
        self.0.forget(user);
    }

    fn pool_len(&self, world: u32) -> usize {
        self.0.pool(world).map_or(0, <[weather::Entry]>::len)
    }
}

/// thuum ADR-068: a reported argument as wire-rules natives reads it; a kind
/// the core never sends reads as null.
fn native_arg(a: &ScriptArg) -> natives::Arg {
    match a.kind {
        1 => natives::Arg::Bool,
        2 => natives::Arg::Number,
        3 => natives::Arg::String,
        4 => natives::Arg::Object(a.form_id),
        _ => natives::Arg::Absent,
    }
}

fn native_args(args: &[ScriptArg]) -> Vec<natives::Arg> {
    args.iter().map(native_arg).collect()
}

fn script_call_subject(cls: &str, func: &str, self_id: u32, args: &[ScriptArg]) -> ScriptCallSubject {
    let Some(index) = natives::row_index(cls, func) else {
        return ScriptCallSubject { row: -1, subject: 0, record: 0, look: 0 };
    };
    let row = natives::TABLE.get(index);
    let subject = row.and_then(|row| natives::subject_of(row, self_id, &native_args(args))).unwrap_or(0);
    let record = match row.map(|r| r.keep) {
        Some(natives::Keep::Record(kind)) => kind.code(),
        _ => 0,
    };
    let look = match row.map(|r| r.look) {
        Some(natives::Look::Also) => 1,
        Some(natives::Look::Only) => 2,
        _ => 0,
    };
    ScriptCallSubject { row: i32::try_from(index).unwrap_or(-1), subject, record, look }
}

/// Every reporter's budget and every subject's sequence (wire-rules natives),
/// and the call the shape step passed, held until the owner step takes it
/// (natives::Shaped, which the shape step alone makes).
#[derive(Debug, Default)]
pub struct ScriptCalls(natives::ScriptCalls, Option<natives::Shaped>);

fn new_script_calls() -> Box<ScriptCalls> {
    Box::default()
}

/// A script call's verdict before any word: skipped, nothing relayed.
fn quiet_script_call() -> ScriptCallVerdict {
    ScriptCallVerdict {
        code: ScriptCallCode::Skipped,
        name: String::new(),
        why: String::new(),
        subject: 0,
        seq: 0,
        relay_cls: String::new(),
        relay_fn: String::new(),
        relay_args: Vec::new(),
        relay_self: 0,
        audience: 2,
        keep_last: false,
        keep_evidence: false,
        record: 0,
        event_arg: -1,
        keep_var: 0,
        look: false,
        figure_args: false,
    }
}

/// A refused script call, by its code and its name, either step's.
fn script_call_figures(args: &[ScriptArg], players: &[u32]) -> ScriptCallVerdict {
    match natives::figures(&native_args(args), players) {
        Ok(()) => {
            let mut out = quiet_script_call();
            out.code = ScriptCallCode::Accepted;
            out
        }
        Err(r) => refused_script_call(r),
    }
}

fn refused_script_call(r: natives::Refusal) -> ScriptCallVerdict {
    let mut out = quiet_script_call();
    out.code = match r.code {
        natives::Code::Row => ScriptCallCode::Row,
        natives::Code::Args => ScriptCallCode::Args,
        natives::Code::Owner => ScriptCallCode::Owner,
        natives::Code::Rate => ScriptCallCode::Rate,
        natives::Code::Far => ScriptCallCode::Far,
        natives::Code::LookRate => ScriptCallCode::LookRate,
    };
    out.name = r.code.name().to_owned();
    out.why = r.why;
    out
}

impl ScriptCalls {
    fn shape(&mut self, row: i32, args: &[ScriptArg], sender: u32, self_id: u32, now_ms: u64) -> ScriptCallVerdict {
        let row = usize::try_from(row).ok().and_then(|i| natives::TABLE.get(i));
        // one call at a time: a shape step drops a call no owner step took
        self.1 = None;
        match self.0.shape(row, &native_args(args), sender, self_id, now_ms) {
            Ok(shaped) => {
                let mut out = quiet_script_call();
                out.code = ScriptCallCode::Accepted;
                out.subject = shaped.subject();
                self.1 = Some(shaped);
                out
            }
            Err(r) => refused_script_call(r),
        }
    }

    fn owner(&mut self, f: &ScriptCallFacts) -> ScriptCallVerdict {
        let Some(shaped) = self.1.take() else {
            return quiet_script_call();
        };
        let kind = match f.subject_kind {
            1 => natives::SubjectKind::WorldObject,
            2 => natives::SubjectKind::DoorOrContainer,
            3 => natives::SubjectKind::Actor,
            4 => natives::SubjectKind::Record,
            _ => natives::SubjectKind::Unknown,
        };
        // the firing facts count where the core gathered them for this call
        let gathered = f.firing.reporter != 0 && f.firing.reporter == f.sender && f.firing.cell == f.cell;
        let facts = natives::Facts {
            sender: f.sender,
            self_id: f.self_id,
            epoch: f.epoch,
            kind,
            cell: f.cell,
            owner: f.owner,
            owner_epoch: f.owner_epoch,
            host: f.host,
            host_epoch: f.host_epoch,
            loader: gathered && f.firing.loader,
            nearest: if gathered { distance_of(f.firing.nearest) } else { None },
            subject_size: if gathered { f.firing.trigger_size } else { 0.0 },
        };
        let mut out = quiet_script_call();
        match self.0.owner(shaped, &facts) {
            natives::Verdict::Accept(a) => {
                out.code = ScriptCallCode::Accepted;
                out.subject = a.subject;
                out.seq = a.seq;
                out.relay_cls = a.relay_cls.to_owned();
                out.relay_fn = a.relay_fn.to_owned();
                out.relay_args = a.relay_args.to_vec();
                out.relay_self = a.relay_self;
                out.audience = match a.audience {
                    natives::Audience::Listeners => 0,
                    natives::Audience::Everyone => 1,
                    natives::Audience::Nobody => 2,
                    natives::Audience::Host => 3,
                };
                out.keep_last = a.keep == natives::Keep::Last;
                out.keep_var = a.variable_kind.unwrap_or(0);
                out.look = a.look;
                out.figure_args = a.figure_args;
                out.event_arg = a.graph_event_arg.and_then(|i| i8::try_from(i).ok()).unwrap_or(-1);
            }
            natives::Verdict::Kept { subject } => {
                out.code = ScriptCallCode::Accepted;
                out.subject = subject;
                out.keep_evidence = true;
            }
            natives::Verdict::Record { subject, seq, kind } => {
                out.code = ScriptCallCode::Accepted;
                out.subject = subject;
                out.seq = seq;
                out.record = kind.code();
            }
            natives::Verdict::Skip => {}
            natives::Verdict::Refuse(r) => out = refused_script_call(r),
        }
        out
    }

    /// Both steps on one call in the core's order, the shape step and then
    /// the owner step on a call it accepted, for the tests, whose facts are
    /// at hand before the call.
    #[cfg(test)]
    fn judge(&mut self, row: i32, args: &[ScriptArg], facts: &ScriptCallFacts, now_ms: u64) -> ScriptCallVerdict {
        let shaped = self.shape(row, args, facts.sender, facts.self_id, now_ms);
        if shaped.code != ScriptCallCode::Accepted {
            return shaped;
        }
        self.owner(facts)
    }

    fn seq_of(&self, subject: u32) -> u32 {
        self.0.seq_of(subject)
    }

    fn seed(&mut self, subject: u32, seq: u32) {
        self.0.seed(subject, seq);
    }
}

/// thuum ADR-068 rung 3 (the script record): the release each cell last saw
/// until its next grant (wire-rules script_state), for the core's judgment
/// of each reported record.
#[derive(Debug, Default)]
pub struct ScriptStates(script_state::ScriptStates);

fn new_script_states() -> Box<ScriptStates> {
    Box::default()
}

impl ScriptStates {
    fn judge(&self, f: &ScriptStateFacts) -> ScriptStateVerdict {
        let kind = match f.ref_kind {
            1 => script_state::RefKind::WorldObject,
            2 => script_state::RefKind::Actor,
            _ => script_state::RefKind::Unknown,
        };
        let facts = script_state::Facts { sender: f.sender, epoch: f.epoch, kind, cell: f.cell, owner: f.owner, owner_epoch: f.owner_epoch };
        let recorded = |code| ScriptStateVerdict { code, name: String::new(), why: String::new() };
        match self.0.judge(&facts) {
            script_state::Verdict::Accepted => recorded(ScriptStateCode::Accepted),
            script_state::Verdict::ReleaseRead => recorded(ScriptStateCode::ReleaseRead),
            script_state::Verdict::Refuse(r) => ScriptStateVerdict {
                code: match r.code {
                    script_state::Code::NoRef => ScriptStateCode::NoRef,
                    script_state::Code::Kind => ScriptStateCode::Kind,
                    script_state::Code::Owner => ScriptStateCode::Owner,
                    script_state::Code::Epoch => ScriptStateCode::Epoch,
                },
                name: r.code.name().to_owned(),
                why: r.why,
            },
        }
    }

    fn released(&mut self, cell: u32, player: u32, epoch: u32) {
        self.0.released(cell, player, epoch);
    }

    fn granted(&mut self, cell: u32) {
        self.0.granted(cell);
    }
}

/// The core's distance, or none: -1 (any negative or non-finite value) is
/// none.
fn distance_of(d: f32) -> Option<f32> {
    (d.is_finite() && d >= 0.0).then_some(d)
}

fn trigger_firing_source(f: &TriggerFiringFacts) -> TriggerFiringVerdict {
    let facts = trigger_entry::FiringFacts { reporter: f.reporter, cell: f.cell, host: f.host, loader: f.loader, nearest: distance_of(f.nearest), trigger_size: f.trigger_size };
    let taken = |code| TriggerFiringVerdict { code, name: String::new(), why: String::new() };
    match trigger_entry::firing_source(&facts) {
        trigger_entry::FiringVerdict::Taken(trigger_entry::FiringSource::Host) => taken(TriggerFiringCode::Host),
        trigger_entry::FiringVerdict::Taken(trigger_entry::FiringSource::Loader) => taken(TriggerFiringCode::Loader),
        trigger_entry::FiringVerdict::Refuse(r) => TriggerFiringVerdict {
            code: match r.code {
                trigger_entry::FiringCode::NotHost => TriggerFiringCode::NotHost,
                trigger_entry::FiringCode::Far => TriggerFiringCode::Far,
            },
            name: r.code.name().to_owned(),
            why: r.why,
        },
    }
}

/// thuum ADR-068 D-C2: each presser's accepted enters it has not left
/// (wire-rules trigger_entry::Entries), for the core's judgment of each
/// reported entry.
#[derive(Debug, Default)]
pub struct TriggerEntries(trigger_entry::Entries);

fn new_trigger_entries() -> Box<TriggerEntries> {
    Box::default()
}

impl TriggerEntries {
    fn judge(&mut self, f: &TriggerEntryFacts) -> TriggerEntryVerdict {
        let trigger = match f.trigger_kind {
            1 => script_state::RefKind::WorldObject,
            2 => script_state::RefKind::Actor,
            _ => script_state::RefKind::Unknown,
        };
        let facts = trigger_entry::EntryFacts {
            reporter: f.reporter,
            trigger,
            presser: f.presser,
            presser_owner: f.presser_owner,
            cell: f.cell,
            host: f.host,
            loader: f.loader,
            distance: distance_of(f.distance),
            trigger_size: f.trigger_size,
            trigger_id: f.trigger_id,
            leave: f.leave,
        };
        entry_verdict(self.0.judge(&facts))
    }

    fn forget(&mut self, presser: u32) {
        self.0.forget(presser);
    }
}

fn entry_verdict(verdict: trigger_entry::EntryVerdict) -> TriggerEntryVerdict {
    match verdict {
        trigger_entry::EntryVerdict::Relay { host } => TriggerEntryVerdict { code: TriggerEntryCode::Relay, name: String::new(), why: String::new(), host },
        trigger_entry::EntryVerdict::Refuse(r) => TriggerEntryVerdict {
            code: match r.code {
                trigger_entry::EntryCode::NoRef => TriggerEntryCode::NoRef,
                trigger_entry::EntryCode::Actor => TriggerEntryCode::Actor,
                trigger_entry::EntryCode::Presser => TriggerEntryCode::Presser,
                trigger_entry::EntryCode::NotLoader => TriggerEntryCode::NotLoader,
                trigger_entry::EntryCode::Host => TriggerEntryCode::Host,
                trigger_entry::EntryCode::NoHost => TriggerEntryCode::NoHost,
                trigger_entry::EntryCode::Reach => TriggerEntryCode::Reach,
                trigger_entry::EntryCode::Unpaired => TriggerEntryCode::Unpaired,
            },
            name: r.code.name().to_owned(),
            why: r.why,
            host: 0,
        },
    }
}

fn graph_variable_slot(kept: &[GraphVariableKept], seq: u32) -> GraphVariableSlot {
    let parsed: Vec<natives::KeptVariable> = kept.iter().map(|k| natives::KeptVariable { same_name: k.same_name, seq: k.seq }).collect();
    match natives::variable_slot(&parsed, seq) {
        // the index is below the slice's length, at most a u32 the core sent
        natives::VariableSlot::Replace(i) => u32::try_from(i).map_or(GraphVariableSlot { code: 2, index: 0 }, |index| GraphVariableSlot { code: 0, index }),
        natives::VariableSlot::Append => GraphVariableSlot { code: 1, index: 0 },
        natives::VariableSlot::Stale => GraphVariableSlot { code: 2, index: 0 },
    }
}

fn object_animation_owner(f: &ObjectAnimationFacts) -> ObjectAnimationVerdict {
    let facts = object_animation::Facts { reporter: f.reporter, furniture: f.furniture, occupant: f.occupant, occupant_owner: f.occupant_owner, cell: f.cell, host: f.host };
    match object_animation::judge(&facts) {
        object_animation::Verdict::Relay { owner, keep } => ObjectAnimationVerdict {
            code: match owner {
                object_animation::Owner::Host => ObjectAnimationCode::Host,
                object_animation::Owner::User => ObjectAnimationCode::User,
            },
            name: String::new(),
            why: String::new(),
            keep,
        },
        object_animation::Verdict::Refuse(r) => ObjectAnimationVerdict {
            code: match r.code {
                object_animation::Code::NotHost => ObjectAnimationCode::NotHost,
                object_animation::Code::NotUser => ObjectAnimationCode::NotUser,
            },
            name: r.code.name().to_owned(),
            why: r.why,
            keep: false,
        },
    }
}

fn alias_creations(aliases: &[QuestAliasRow]) -> Vec<AliasCreationRow> {
    let parsed: Vec<alias_fills::Alias> =
        aliases.iter().map(|a| alias_fills::Alias { id: a.id, created: a.created, create_at: a.create_at, forced: a.forced, unique_actor: a.unique_actor }).collect();
    alias_fills::creations(&parsed)
        .into_iter()
        .map(|c| {
            let (container_kind, container, why) = match c.container {
                alias_fills::Container::Unresolved(why) => (0, 0, why.to_owned()),
                alias_fills::Container::Forced(refr) => (1, refr, String::new()),
                alias_fills::Container::UniqueActor(base) => (2, base, String::new()),
            };
            AliasCreationRow { alias: c.alias, base: c.base, target: c.target, container_kind, container, why }
        })
        .collect()
}

fn alias_fill_of(e: &AliasFillEntry) -> alias_fills::Fill {
    alias_fills::Fill { quest: e.quest, alias: e.alias, refr: e.refr }
}

fn alias_fill_judge(record: &[AliasFillEntry], fill: &AliasFillEntry, facts: &AliasFillFacts) -> AliasFillVerdict {
    let parsed: Vec<alias_fills::Fill> = record.iter().map(alias_fill_of).collect();
    let f = alias_fills::Facts { is_quest: facts.is_quest, has_alias: facts.has_alias, refr_is_ref: facts.refr_is_ref };
    match alias_fills::judge(&parsed, alias_fill_of(fill), f) {
        alias_fills::Verdict::Recorded => AliasFillVerdict { code: 0, name: String::new(), why: String::new() },
        alias_fills::Verdict::Folded => AliasFillVerdict { code: 1, name: String::new(), why: String::new() },
        alias_fills::Verdict::Refuse(code, why) => AliasFillVerdict { code: 2, name: code.name().to_owned(), why },
    }
}

fn world_global_entry(facts: &WorldGlobalFacts) -> u8 {
    let f = world_globals::EntryFacts { is_glob: facts.is_glob, constant: facts.constant, player_listed: facts.player_listed };
    match world_globals::entry(facts.global, f) {
        Ok(()) => 0,
        Err(world_globals::Code::Clock) => 1,
        Err(world_globals::Code::Player) => 2,
        Err(world_globals::Code::Constant) => 3,
        Err(world_globals::Code::NoGlob) => 4,
        Err(world_globals::Code::Owner) => 5,
        Err(world_globals::Code::Epoch) => 6,
    }
}

fn world_global_code_name(code: u8) -> String {
    let code = match code {
        1 => world_globals::Code::Clock,
        2 => world_globals::Code::Player,
        3 => world_globals::Code::Constant,
        4 => world_globals::Code::NoGlob,
        5 => world_globals::Code::Owner,
        6 => world_globals::Code::Epoch,
        _ => return String::new(),
    };
    code.name().to_owned()
}

fn world_globals_merge(record: &[WorldGlobalValue], accepted: &[WorldGlobalValue]) -> WorldGlobalsMerge {
    let to_value = |v: &WorldGlobalValue| world_globals::Value { global: v.global, value: v.value };
    let from_value = |v: world_globals::Value| WorldGlobalValue { global: v.global, value: v.value };
    let record: Vec<world_globals::Value> = record.iter().map(to_value).collect();
    let accepted: Vec<world_globals::Value> = accepted.iter().map(to_value).collect();
    let (after, changed) = world_globals::merge(&record, &accepted);
    WorldGlobalsMerge { record: after.into_iter().map(from_value).collect(), changed: changed.into_iter().map(from_value).collect() }
}

fn ref_state_apply(disabled: bool, deleted: bool, func: &str) -> RefStateVerdict {
    let Some(call) = ref_state::Call::of(func) else {
        return RefStateVerdict { code: 2, disabled, deleted };
    };
    match ref_state::apply(ref_state::RefState { disabled, deleted }, call) {
        ref_state::Verdict::Recorded(after) => RefStateVerdict { code: 0, disabled: after.disabled, deleted: after.deleted },
        ref_state::Verdict::Folded(kept) => RefStateVerdict { code: 1, disabled: kept.disabled, deleted: kept.deleted },
    }
}

#[allow(clippy::too_many_arguments)]
fn standing_judge(
    func: &str,
    object: u32,
    number: i64,
    faction_is_fact: bool,
    other_is_actor: bool,
    factions: &[StandingFaction],
    crime_faction: u32,
    relationships: &[StandingRelation],
) -> StandingVerdict {
    let is = |name: &str| func.eq_ignore_ascii_case(name);
    let call = if is("SetFactionRank") {
        standing::Call::SetFactionRank { faction: object, rank: number }
    } else if is("ModFactionRank") {
        standing::Call::ModFactionRank { faction: object, by: number }
    } else if is("RemoveFromFaction") {
        standing::Call::RemoveFromFaction { faction: object }
    } else if is("RemoveFromAllFactions") {
        standing::Call::RemoveFromAllFactions
    } else if is("SetCrimeFaction") {
        standing::Call::SetCrimeFaction { faction: object }
    } else if is("SetRelationshipRank") {
        standing::Call::SetRelationshipRank { other: object, rank: number }
    } else {
        return StandingVerdict { code: 3, name: String::new(), why: String::new(), change: 0, form: 0, rank: 0 };
    };
    let factions: Vec<standing::FactionRank> = factions.iter().map(|f| standing::FactionRank { faction: f.faction, rank: f.rank }).collect();
    let relationships: Vec<standing::Relationship> = relationships.iter().map(|r| standing::Relationship { other: r.other, rank: r.rank }).collect();
    let record = standing::Record { factions: &factions, crime_faction, relationships: &relationships };
    let out = |code, change, form, rank| StandingVerdict { code, name: String::new(), why: String::new(), change, form, rank };
    match standing::judge(&record, call, standing::Facts { faction_is_fact, other_is_actor }) {
        standing::Verdict::Recorded(standing::Change::Rank { faction, rank }) => out(0, 1, faction, rank),
        standing::Verdict::Recorded(standing::Change::Removed { faction }) => out(0, 2, faction, 0),
        standing::Verdict::Recorded(standing::Change::RemovedAll) => out(0, 3, 0, 0),
        standing::Verdict::Recorded(standing::Change::Crime { faction }) => out(0, 4, faction, 0),
        standing::Verdict::Recorded(standing::Change::Relationship { other, rank }) => out(0, 5, other, rank),
        standing::Verdict::Folded => out(1, 0, 0, 0),
        standing::Verdict::Refuse(code, why) => StandingVerdict { code: 2, name: code.name().to_owned(), why, change: 0, form: 0, rank: 0 },
    }
}

/// thuum ADR-068 rung 4: the world streamer (wire-rules world_streamer).
#[derive(Debug, Default)]
pub struct WorldStreamer(world_streamer::WorldStreamer);

fn new_world_streamer() -> Box<WorldStreamer> {
    Box::default()
}

impl WorldStreamer {
    fn election(&self, e: Option<world_streamer::Election>) -> StreamerElection {
        match e {
            Some(e) => StreamerElection { changed: true, streamer: e.streamer, epoch: e.epoch, previous: e.previous },
            None => {
                let (streamer, epoch) = self.0.streamer();
                StreamerElection { changed: false, streamer, epoch, previous: 0 }
            }
        }
    }

    fn moved(&mut self, player: u32) -> StreamerElection {
        let e = self.0.moved(player);
        self.election(e)
    }

    fn stale(&mut self, player: u32, stale: bool) -> StreamerElection {
        let e = self.0.stale(player, stale);
        self.election(e)
    }

    fn dead(&mut self, player: u32, dead: bool) -> StreamerElection {
        let e = self.0.dead(player, dead);
        self.election(e)
    }

    fn gone(&mut self, player: u32) -> StreamerElection {
        let e = self.0.gone(player);
        self.election(e)
    }

    fn streamer_now(&self) -> StreamerElection {
        self.election(None)
    }

    fn judge(&self, sender: u32, epoch: u32) -> u8 {
        match self.0.judge(sender, epoch) {
            world_streamer::Judgement::Current => 0,
            world_streamer::Judgement::StaleEpoch => 1,
            world_streamer::Judgement::FutureEpoch => 2,
            world_streamer::Judgement::NotOwner => 3,
        }
    }
}

fn alias_created_due(marks: &[AliasMarkRow], quest: u32, alias: u32) -> bool {
    let parsed: Vec<alias_fills::Mark> = marks.iter().map(|m| alias_fills::Mark { quest: m.quest, alias: m.alias, added: m.added }).collect();
    alias_fills::due(&parsed, quest, alias)
}

/// Quests as the bridge's entries name them (stage 0), and back.
fn quest_entries(quests: &[forms::FormDesc]) -> Vec<QuestStageEntry> {
    quests.iter().map(|d| QuestStageEntry { short_id: d.short_id, file: d.file.clone(), stage: 0 }).collect()
}

fn form_descs(entries: &[QuestStageEntry]) -> Vec<forms::FormDesc> {
    entries.iter().map(|e| forms::FormDesc { short_id: e.short_id, file: e.file.clone() }).collect()
}

fn quest_report_word(word: u32, stage: u16, start_up: bool) -> u8 {
    let word = if word == 0 { None } else { Some(u8::try_from(word).unwrap_or(u8::MAX)) };
    match quest_runs::Report::of(word, stage, start_up) {
        Some(quest_runs::Report::Stage(_)) => 0,
        Some(quest_runs::Report::Word(w)) => w.wire(),
        None => 3,
    }
}

/// thuum ADR-068 rung 5: the churn counter and the held quests (wire-rules
/// quest_runs).
#[derive(Debug, Default)]
pub struct QuestRuns(quest_runs::QuestRuns);

fn new_quest_runs() -> Box<QuestRuns> {
    Box::default()
}

fn quest_run_of(r: &QuestRunRow) -> quest_runs::QuestRun {
    quest_runs::QuestRun { quest: forms::FormDesc { short_id: r.short_id, file: r.file.clone() }, running: r.running, seq: r.seq }
}

fn quest_run_rows(record: &[quest_runs::QuestRun]) -> Vec<QuestRunRow> {
    record.iter().map(|r| QuestRunRow { short_id: r.quest.short_id, file: r.quest.file.clone(), running: r.running, seq: r.seq }).collect()
}

fn run_word(word: u8) -> Option<quest_runs::Word> {
    match word {
        1 => Some(quest_runs::Word::Start),
        2 => Some(quest_runs::Word::Stop),
        _ => None,
    }
}

fn quest_story_instances_world(settings_json: &str) -> Result<bool, clock::SettingsError> {
    quest_runs::StoryInstances::from_json(settings_json).map(|s| s == quest_runs::StoryInstances::World)
}

fn quest_player_alias_cause_world(settings_json: &str) -> Result<bool, clock::SettingsError> {
    quest_runs::PlayerAliasCause::from_json(settings_json).map(|s| s == quest_runs::PlayerAliasCause::World)
}

fn quest_root_is_seat_own(function: &str, script_on_player_aliases_only: bool) -> bool {
    quest_runs::root_is_seat_own(function, script_on_player_aliases_only)
}

impl QuestRuns {
    #[allow(clippy::too_many_arguments)]
    fn quest_runs_judge(&mut self, record: &[QuestRunRow], short_id: u32, file: &str, word: u8, cause: u8, seat: u32, now_ms: u64) -> QuestRunVerdict {
        let out = |code, first, name: &str, why: String| QuestRunVerdict { code, first, name: name.to_owned(), why };
        let Some(word) = run_word(word) else {
            return out(1, false, "", String::new());
        };
        let parsed: Vec<quest_runs::QuestRun> = record.iter().map(quest_run_of).collect();
        let quest = forms::FormDesc { short_id, file: file.to_owned() };
        let cause = if cause == 0 { None } else { quest_runs::Cause::of(Some(cause)) };
        match self.0.judge_caused(&parsed, &quest, word, cause, seat, now_ms) {
            quest_runs::Verdict::Recorded => out(0, false, "", String::new()),
            quest_runs::Verdict::Folded => out(1, false, "", String::new()),
            quest_runs::Verdict::Held { first } => out(2, first, quest_runs::Code::Churn.name(), String::new()),
            quest_runs::Verdict::Refuse(code @ quest_runs::Code::Cause, why) => out(4, false, code.name(), why),
            quest_runs::Verdict::Refuse(code @ quest_runs::Code::Instance, why) => out(5, false, code.name(), why),
            quest_runs::Verdict::Refuse(code, why) => out(3, false, code.name(), why),
        }
    }

    fn quest_runs_set_story(&mut self, quests: &[QuestStageEntry], world: bool) {
        let instances = if world { quest_runs::StoryInstances::World } else { quest_runs::StoryInstances::Seat };
        self.0.set_story(&form_descs(quests), instances);
    }

    fn quest_runs_story_named(&self, short_id: u32, file: &str) -> bool {
        self.0.story_named(&forms::FormDesc { short_id, file: file.to_owned() })
    }

    fn quest_runs_story_count(&self) -> usize {
        self.0.story_count()
    }

    fn quest_runs_held(&self, short_id: u32, file: &str) -> bool {
        self.0.held(&forms::FormDesc { short_id, file: file.to_owned() })
    }

    fn quest_runs_gone(&mut self, seat: u32) {
        self.0.gone(seat);
    }

    fn quest_runs_at_login(&self, record: &[QuestRunRow], shared: &SharedQuests, stages: &[QuestStageEntry]) -> QuestRunsLogin {
        let parsed: Vec<quest_runs::QuestRun> = record.iter().map(quest_run_of).collect();
        let (starts, stops) = self.0.at_login(&parsed, &shared.0, &to_stages(stages));
        let entries = |runs: Vec<&quest_runs::QuestRun>| runs.iter().map(|r| QuestStageEntry { short_id: r.quest.short_id, file: r.quest.file.clone(), stage: 0 }).collect();
        QuestRunsLogin { starts: entries(starts), stops: entries(stops) }
    }
}

fn quest_runs_merge(record: &[QuestRunRow], short_id: u32, file: &str, word: u8) -> Vec<QuestRunRow> {
    let parsed: Vec<quest_runs::QuestRun> = record.iter().map(quest_run_of).collect();
    match run_word(word) {
        Some(word) => quest_run_rows(&quest_runs::merge(&parsed, &forms::FormDesc { short_id, file: file.to_owned() }, word)),
        None => record.to_vec(),
    }
}

fn quest_runs_without(record: &[QuestRunRow], short_id: u32, file: &str) -> Vec<QuestRunRow> {
    let parsed: Vec<quest_runs::QuestRun> = record.iter().map(quest_run_of).collect();
    quest_run_rows(&quest_runs::without(&parsed, &forms::FormDesc { short_id, file: file.to_owned() }))
}

/// thuum ADR-068 rung 5: the quest scope's settings (wire-rules quest_scope).
pub struct QuestScope(quest_scope::QuestScope);

fn new_quest_scope(mode_json: &str, player_quests_json: &str) -> Result<Box<QuestScope>, clock::SettingsError> {
    quest_scope::QuestScope::from_settings(mode_json, player_quests_json).map(|s| Box::new(QuestScope(s)))
}

impl QuestScope {
    fn quest_scope_acting(&self) -> bool {
        self.0.mode() == quest_scope::Mode::Derived
    }

    fn quest_scope_listed(&self) -> Vec<QuestStageEntry> {
        quest_entries(self.0.listed())
    }

    fn quest_scope_class(&self, short_id: u32, file: &str, aliases: &[QuestScopeAliasRow]) -> u8 {
        self.0.class(&forms::FormDesc { short_id, file: file.to_owned() }, &scope_aliases(aliases)).code()
    }
}

/// A quest's alias rows as wire-rules quest_scope reads them.
fn scope_aliases(aliases: &[QuestScopeAliasRow]) -> Vec<quest_scope::Alias> {
    aliases
        .iter()
        .map(|a| quest_scope::Alias {
            id: a.id,
            location: a.location,
            forced: a.forced,
            unique_actor: a.unique_actor,
            created: a.created,
            create_at: a.create_at,
            external: a.external.then_some(quest_scope::External { forced: a.external_forced, unique_actor: a.external_unique_actor }),
        })
        .collect()
}

fn quest_scope_player_aliases(aliases: &[QuestScopeAliasRow]) -> Vec<u32> {
    quest_scope::player_alias_ids(&scope_aliases(aliases))
}

/// thuum ADR-068 rung 5: the condition-read variables (wire-rules
/// script_variables).
pub struct ScriptVariables(script_variables::ScriptVariables);

fn new_script_variables(settings_json: &str) -> Result<Box<ScriptVariables>, clock::SettingsError> {
    script_variables::ScriptVariables::from_json(settings_json).map(|v| Box::new(ScriptVariables(v)))
}

fn script_var_key(r: &ScriptVarRow) -> script_variables::Key {
    script_variables::Key { form: forms::FormDesc { short_id: r.short_id, file: r.file.clone() }, script: r.script.clone(), var: r.var.clone() }
}

fn script_var_entries(record: &[ScriptVarRow]) -> Vec<script_variables::Entry> {
    record
        .iter()
        .filter_map(|r| script_variables::Value::of(r.k, r.i, r.f, r.b).map(|value| script_variables::Entry { key: script_var_key(r), value, seq: r.seq }))
        .collect()
}

fn script_var_row(e: &script_variables::Entry, own: bool) -> ScriptVarRow {
    let (i, f, b) = match e.value {
        script_variables::Value::Int(i) => (i, 0.0, false),
        script_variables::Value::Float(f) => (0, f, false),
        script_variables::Value::Bool(b) => (0, 0.0, b),
    };
    ScriptVarRow { short_id: e.key.form.short_id, file: e.key.form.file.clone(), script: e.key.script.clone(), var: e.key.var.clone(), own, k: e.value.kind(), i, f, b, seq: e.seq }
}

impl ScriptVariables {
    fn script_variables_set_player(&mut self, quests: &[QuestStageEntry], own: &[ScriptVarKeyRow]) -> usize {
        let keys: Vec<script_variables::Key> = own
            .iter()
            .map(|k| script_variables::Key { form: forms::FormDesc { short_id: k.short_id, file: k.file.clone() }, script: k.script.clone(), var: k.var.clone() })
            .collect();
        self.0.set_player(&form_descs(quests), &keys)
    }

    fn script_variables_watch(&self) -> Vec<ScriptVarKeyRow> {
        self.0
            .watch()
            .into_iter()
            .map(|k| ScriptVarKeyRow { short_id: k.form.short_id, file: k.form.file.clone(), script: k.script.clone(), var: k.var.clone(), own: self.0.is_own(k) })
            .collect()
    }

    fn script_variables_report(&self, world: &[ScriptVarRow], own: &[ScriptVarRow], entries: &[ScriptVarRow]) -> ScriptVarReport {
        let out = |code, name: &str, why: String| ScriptVarVerdict { code, name: name.to_owned(), why };
        let (mut w, mut o) = (script_var_entries(world), script_var_entries(own));
        let mut verdicts = Vec::with_capacity(entries.len());
        let mut seqs = Vec::with_capacity(entries.len());
        for entry in entries {
            let Some(value) = script_variables::Value::of(entry.k, entry.i, entry.f, entry.b) else {
                verdicts.push(out(2, script_variables::Code::Kind.name(), format!("kind {} is none a condition compares (3, 4 or 5)", entry.k)));
                seqs.push(0);
                continue;
            };
            let key = script_var_key(entry);
            let record = if entry.own { &mut o } else { &mut w };
            match self.0.judge(record, &key, value, entry.own) {
                script_variables::Verdict::Recorded => {
                    let (after, seq) = script_variables::merge(record, &key, value);
                    *record = after;
                    verdicts.push(out(0, "", String::new()));
                    seqs.push(seq);
                }
                script_variables::Verdict::Folded => {
                    verdicts.push(out(1, "", String::new()));
                    seqs.push(0);
                }
                script_variables::Verdict::Refuse(code, why) => {
                    verdicts.push(out(2, code.name(), why));
                    seqs.push(0);
                }
            }
        }
        ScriptVarReport { verdicts, seqs, world: w.iter().map(|e| script_var_row(e, false)).collect(), own: o.iter().map(|e| script_var_row(e, true)).collect() }
    }

    fn script_variables_at_login(&self, record: &[ScriptVarRow], own: bool) -> Vec<ScriptVarRow> {
        let entries = script_var_entries(record);
        self.0.at_login(&entries, own).into_iter().map(|e| script_var_row(e, own)).collect()
    }
}

/// thuum ADR-068 rung 5: every player's overlay (wire-rules player_own).
#[derive(Debug, Default)]
pub struct PlayerOverlays(player_own::PlayerOverlays);

fn new_player_overlays() -> Box<PlayerOverlays> {
    Box::default()
}

fn overlay_row(o: Option<&player_own::Overlay>) -> PlayerOverlayRow {
    match o {
        Some(o) => PlayerOverlayRow { open: true, race: o.race, base: o.base, spells: o.spells.clone(), shouts: o.shouts.clone(), items: o.items.clone() },
        None => PlayerOverlayRow { open: false, race: 0, base: 0, spells: Vec::new(), shouts: Vec::new(), items: Vec::new() },
    }
}

impl PlayerOverlays {
    fn player_own_judge(&mut self, player: u32, cls: &str, func: &str, form: u32, properties: &[ScriptPropertyRow], facts: &PlayerOwnFacts) -> PlayerOwnVerdict {
        let Some(row) = player_own::Row::of(cls, func) else {
            return PlayerOwnVerdict {
                code: 4,
                name: natives::Code::Row.name().to_owned(),
                why: format!("{cls}.{func} is none of a player's own nine rows"),
                ended: overlay_row(None),
                overlay: overlay_row(self.0.overlay(player)),
                count: 0,
            };
        };
        let named: Vec<player_own::Named> = properties.iter().map(|p| player_own::Named { form: p.form, members: p.members.clone() }).collect();
        let f = player_own::Facts { recorded_race: facts.recorded_race, playable: facts.playable, restricted: facts.restricted, permitted: facts.permitted, held: facts.held, kind: facts.kind, race_valid: facts.race_valid, leveled: facts.leveled, held_count: facts.held_count, count: player_own::item_count(facts.count), into_held_open: facts.into_held_open, stacks: facts.stacks, new_stack: facts.new_stack, landing_of: facts.landing_of, in_reach: facts.in_reach };
        let (code, name, why, ended, count) = match self.0.judge(player, row, form, &named, f) {
            player_own::Verdict::Record { ended } => (0, "", String::new(), ended, 0),
            player_own::Verdict::Items(n) => (0, "", String::new(), None, n),
            player_own::Verdict::Overlay => (1, "", String::new(), None, 0),
            player_own::Verdict::Ended(o) => (2, "", String::new(), Some(o), 0),
            player_own::Verdict::Fold => (3, "", String::new(), None, 0),
            player_own::Verdict::Refuse(code, why) => (4, code.name(), why, None, 0),
        };
        PlayerOwnVerdict { code, name: name.to_owned(), why, ended: overlay_row(ended.as_ref()), overlay: overlay_row(self.0.overlay(player)), count }
    }

    fn player_overlay(&self, player: u32) -> PlayerOverlayRow {
        overlay_row(self.0.overlay(player))
    }

    fn player_overlay_gone(&mut self, player: u32) -> PlayerOverlayRow {
        overlay_row(self.0.gone(player).as_ref())
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The firing facts of a call the core gathered none for (all zero).
    const NO_FIRING: TriggerFiringFacts = TriggerFiringFacts { reporter: 0, cell: 0, host: 0, loader: false, nearest: 0.0, trigger_size: 0.0 };

    #[test]
    fn a_hosted_state_crosses_value_by_value() {
        let value = |reported: f32, recorded: f32| HostedValueFacts { reported, recorded, held: false, lowered: false, lowered_ago_ms: 0 };
        let facts = HostedStateFacts {
            health: HostedValueFacts { lowered: true, lowered_ago_ms: 300, ..value(0.9, 0.6) },
            magicka: value(0.0, 1.0),
            stamina: value(0.5, 0.5),
            alive: true,
            one_point: 0.01,
        };
        let v = hosted_state_judge(&facts);
        assert!(v.health.dropped && !v.health.changed && v.health.record == 0.6);
        assert!(v.magicka.changed && v.magicka.record == 0.0);
        assert!(!v.stamina.changed && !v.stamina.dropped);
        assert!(!v.floored && !v.dead);
        let dead = hosted_state_judge(&HostedStateFacts { alive: false, ..facts });
        assert!(dead.dead && dead.health.dropped && dead.magicka.dropped);
    }

    #[test]
    fn the_cell_host_table_crosses_as_decisions() {
        let mut c = new_cells();
        assert!(c.reach(0xff00_0000, &[0x9732], 0).is_empty());
        assert!(c.reach(0xff00_0001, &[0x9732], 0).is_empty());
        assert_eq!(
            c.loaded(0xff00_0000, 0x9732, 1_000),
            vec![HostDecision { actor: 0, kind: HostKind::Grant, player: 0xff00_0000, cell: 0x9732, epoch: 1 }]
        );
        assert_eq!(c.host_of(0x9732), CellHost { hosted: true, player: 0xff00_0000, epoch: 1 });
        assert!(c.accepts(0x9732, 0xff00_0000, 1));
        // the games with the cell loaded (ADR-068's mirror of a placement the server holds no reference for)
        assert!(c.loaded(0xff00_0001, 0x9732, 1_100).is_empty());
        assert_eq!(c.loaders_of(0x9732), vec![0xff00_0000, 0xff00_0001]);
        assert!(c.unloaded(0xff00_0001, 0x9732, 1_200).is_empty());
        assert_eq!(c.loaders_of(0x9732), vec![0xff00_0000]);
        // the host leaves, nobody else has the cell loaded: released, unhosted at the kept epoch
        assert!(c.gone(0xff00_0000, 2_000).is_empty());
        assert!(c.loaders_of(0x9732).is_empty());
        assert!(c.tick(2_000 + host_grace_ms() - 1).is_empty());
        assert_eq!(
            c.tick(2_000 + host_grace_ms()),
            vec![
                HostDecision { actor: 0, kind: HostKind::Release, player: 0xff00_0000, cell: 0x9732, epoch: 1 },
                HostDecision { actor: 0, kind: HostKind::Unhosted, player: 0, cell: 0x9732, epoch: 1 },
            ]
        );
        assert_eq!(c.host_of(0x9732), CellHost { hosted: false, player: 0, epoch: 1 });
        assert_eq!(c.epoch_of(0x9732), 1);
        assert_eq!(c.hosts(), vec![CellHostRow { cell: 0x9732, hosted: false, player: 0, epoch: 1 }]);
    }

    // ADR-060: a commander's departure crosses as a word naming its
    // claimant, and its thrall's estimate as the server's end of it
    #[test]
    fn a_commanders_departure_crosses_as_a_word_and_its_thralls_end() {
        let mut k = new_cells();
        assert!(k.reach(0xff00_0002, &[0x9731], 0).is_empty());
        assert!(k.reach(0xff00_0003, &[0x9731], 0).is_empty());
        assert_eq!(k.loaded(0xff00_0002, 0x9731, 1_000).len(), 1);
        assert!(k.loaded(0xff00_0003, 0x9731, 1_100).is_empty());
        assert_eq!(k.claim_commanded(0x35534, 0xff00_0003, 0x9731, 60_000, 1_500), 0);
        assert_eq!(k.commander_of(0x35534), 0xff00_0003);
        assert_eq!(
            k.gone(0xff00_0003, 2_000),
            vec![HostDecision { actor: 0x35534, kind: HostKind::ClaimGone, player: 0xff00_0003, cell: 0x9731, epoch: 1 }]
        );
        assert_eq!(k.commander_of(0x35534), 0);
        assert!(k.tick(1_500 + 60_000 - 1).is_empty());
        assert_eq!(
            k.tick(1_500 + 60_000),
            vec![HostDecision { actor: 0x35534, kind: HostKind::ThrallExpired, player: 0, cell: 0x9731, epoch: 1 }]
        );
    }

    fn sample(refr: u32, player: u32, cell: u32, cell_host: u32, asleep: bool, moved: f32, now_ms: u64) -> PropSampleFacts {
        PropSampleFacts { refr, player, cell, cell_host, asleep, moved, now_ms }
    }

    #[test]
    fn the_prop_table_crosses_as_codes() {
        // thuum docs/verbs/prop-physics.md (ADR-058): the claim's and the
        // transform's codes, the epoch and the lease's end as the core reads them
        let mut p = new_props();
        assert_eq!(p.prop_owner_of(0x2ad9c, 0xff00_0002, 0), 0xff00_0002);
        assert_eq!(p.claim_prop(0x2ad9c, 0xff00_0003, 0x9732, 0, 1_000), 0);
        assert_eq!(p.prop_epoch_of(0x2ad9c), 1);
        assert_eq!(p.claim_prop(0x2ad9c, 0xff00_0003, 0x9732, 1, 1_100), 2);
        assert_eq!(p.claim_prop(0x2ad9c, 0xff00_0002, 0x9732, 0, 1_200), 1);
        assert_eq!(p.prop_transform(&sample(0x2ad9c, 0xff00_0002, 0x9732, 0xff00_0002, false, 10.0, 1_300)), 1);
        assert_eq!(p.prop_transform(&sample(0x2ad9c, 0xff00_0003, 0x9732, 0xff00_0002, true, 10.0, 1_300)), 0);
        assert_eq!(p.prop_transform(&sample(0x2ad9c, 0xff00_0003, 0, 0xff00_0002, true, 10.0, 1_300)), 2);
        assert_eq!(p.prop_transform(&sample(0x2ad9c, 0xff00_0003, 0x9732, 0xff00_0002, true, 5_000.0, 1_300)), 3);
        assert_eq!(p.prop_live_owner(0x2ad9c, 1_300), 0xff00_0003);
        // the grab holds through the sleep; the holder's drop and its rest start the lease
        assert_eq!(p.prop_owner_of(0x2ad9c, 0xff00_0002, 1_300 + prop_lease_ms()), 0xff00_0003);
        assert_eq!(p.claim_prop(0x2ad9c, 0xff00_0003, 0x9732, 2, 1_300), 2);
        assert_eq!(p.prop_transform(&sample(0x2ad9c, 0xff00_0003, 0x9732, 0xff00_0002, true, 10.0, 1_300)), 0);
        assert_eq!(p.prop_owner_of(0x2ad9c, 0xff00_0002, 1_300 + prop_lease_ms() - 1), 0xff00_0003);
        assert_eq!(p.prop_owner_of(0x2ad9c, 0xff00_0002, 1_300 + prop_lease_ms()), 0xff00_0002);
        p.props_host_changed(0x9732, 0xff00_0004, 9_000);
        assert_eq!(p.prop_owner_of(0x2ad9c, 0xff00_0004, 9_000), 0xff00_0004);
        p.props_gone(0xff00_0004);
        assert_eq!(p.prop_owner_of(0x2ad9c, 0xff00_0002, 9_100), 0xff00_0002);
        assert_eq!(p.prop_epoch_of(0x2ad9c), 1);
    }

    #[test]
    fn a_trap_bodys_claim_crosses_as_codes() {
        // thuum D-S3 point 4: Embershard's rock 0xb6d45 in the mine 0xb6be6;
        // the firing seat's grant, the second firing's Moving (3), the
        // firing seat's own Same (2); an item's impulse keeps its grant
        let mut p = new_props();
        assert_eq!(p.claim_prop_world_body(0xb6d45, 0xff00_0003, 0xb6be6, 1_000), 0);
        assert_eq!(p.prop_epoch_of(0xb6d45), 1);
        assert_eq!(p.claim_prop_world_body(0xb6d45, 0xff00_0002, 0xb6be6, 1_050), 3);
        assert_eq!(p.claim_prop_world_body(0xb6d45, 0xff00_0003, 0xb6be6, 1_100), 2);
        assert_eq!(p.prop_epoch_of(0xb6d45), 1);
        assert_eq!(p.prop_live_owner(0xb6d45, 1_100), 0xff00_0003);
        assert_eq!(p.claim_prop(0x2ad9c, 0xff00_0003, 0x9732, 0, 1_000), 0);
        assert_eq!(p.claim_prop(0x2ad9c, 0xff00_0002, 0x9732, 3, 1_050), 0);
    }

    #[test]
    fn the_bridge_passes_the_rules_through() {
        assert!(!activation_within_reach(1509.0, 0.0).allowed);
        assert!(race_allowed(&RaceFacts {
            has_game_files: true,
            is_recorded_race: false,
            is_playable_race: true,
            is_restricted: false,
            is_permitted: false,
        }));
        assert!(!race_allowed(&RaceFacts {
            has_game_files: true,
            is_recorded_race: false,
            is_playable_race: true,
            is_restricted: true,
            is_permitted: false,
        }));
        let v = melee_within_reach(&MeleeFacts {
            distance: 500.0,
            combat_distance: 141.0,
            bash_reach: 141.0,
            weapon_reach: 1.0,
            unarmed_reach: 96.0,
            aggressor_scale: 1.03,
            target_scale: 1.03,
        });
        assert!(!v.allowed);
        let behind = melee_within_cone(&ConeFacts {
            heading: 180.0,
            dx: 0.0,
            dy: 120.0,
            widest_strike_angle: 50.0,
            power: false,
            target_dead: false,
        });
        assert!(!behind.allowed && (behind.bound - 95.0).abs() < 1e-4);
        let kept = backed_flags(&FlagFacts {
            claims_power: true,
            claims_sneak: true,
            saw_power_start: false,
            since_power_start_ms: 0,
            is_sneaking: true,
            claims_bash: true,
            saw_bash_start: true,
            since_bash_start_ms: 951,
        });
        assert!(!kept.power && kept.sneak && kept.bash);
        let skyrim = SneakMults {
            hand: 2.0,
            one_hand_sword: 3.0,
            one_hand_dagger: 3.0,
            one_hand_axe: 3.0,
            one_hand_mace: 3.0,
            two_hand_sword: 2.0,
            two_hand_axe: 2.0,
            ranged: 0.0,
        };
        assert!((sneak_mult(1, &skyrim) - 3.0).abs() < 1e-6);
        assert!((sneak_mult(7, &skyrim) - 1.3).abs() < 1e-6);
        let measured = SneakMults { ranged: 2.0, ..skyrim };
        assert!((sneak_mult(7, &measured) - 2.0).abs() < 1e-6);
        let mut b = new_movement_budgets();
        assert!(b.spend(7, 2048.0, 0));
        assert!(!b.spend(7, 1.0, 0));
        // COC's permit: Riverwood's square of Tamriel, then its inn
        b.permit_jump_exterior(7, 0x3c, 4, -12, 0);
        assert_eq!(b.check_jump(7, 0x3c, 0.0, 0.0, 1), JumpCheck::Waiting);
        assert_eq!(b.check_jump(7, 0x3c, 18_432.0, -47_104.0, 2), JumpCheck::Landed);
        assert_eq!(b.check_jump(7, 0x3c, 18_432.0, -47_104.0, 3), JumpCheck::NoPermit);
        b.permit_jump_interior(7, 0x133c6, 0);
        assert_eq!(b.check_jump(7, 0x133c6, 1.0, 2.0, 4), JumpCheck::Landed);
        // a teleport the server made: a report from before it is dropped,
        // the arrival ends the wait
        b.expect_arrival(7, 0x3c, 500.0, 500.0, 10);
        assert_eq!(b.check_arrival(7, 0x3c, 0.0, 0.0, 11), JumpCheck::Waiting);
        assert_eq!(b.check_arrival(7, 0x3c, 510.0, 490.0, 12), JumpCheck::Landed);
        assert_eq!(b.check_arrival(7, 0x3c, 0.0, 0.0, 13), JumpCheck::NoPermit);
        // a pin after the server's swap: a report away from the place while
        // it holds is a move the server did not author
        b.pin(7, 0x3c, 500.0, 500.0, 150.0, 1_500);
        assert!(!b.pinned_away(7, 0x3c, 510.0, 490.0, 14));
        assert!(b.pinned_away(7, 0x3c, 0.0, 0.0, 15));
        assert!(!b.pinned_away(7, 0x3c, 0.0, 0.0, 1_501));
        // a cast teleport's jump: within its reach of the cast, once
        b.permit_cast_jump(7, 0x3c, 0.0, -300.0, 12.0, 3_000.0, 5_000, 20);
        assert_eq!(b.check_cast_jump(7, 0x3c, 0.0, 4_000.0, 12.0, 21), JumpCheck::NoPermit);
        assert_eq!(b.check_cast_jump(7, 0x3c, 0.0, 2_000.0, 12.0, 22), JumpCheck::Landed);
        assert_eq!(b.check_cast_jump(7, 0x3c, 0.0, 2_000.0, 12.0, 23), JumpCheck::NoPermit);
        // a move the budget took within a waiting cast jump: placed, the
        // jump standing for a move the bounds refuse
        b.permit_cast_jump(7, 0x3c, 0.0, 300.0, 0.0, 256.0, 1_500, 30);
        assert!(b.cast_jump_lands(7, 0x3c, 0.0, 300.0, 0.0, 31));
        assert!(!b.cast_jump_lands(7, 0x3c, 0.0, 900.0, 0.0, 31));
        assert_eq!(b.check_cast_jump(7, 0x3c, 0.0, 300.0, 0.0, 32), JumpCheck::Landed);
        assert!(!b.cast_jump_lands(7, 0x3c, 0.0, 300.0, 0.0, 33));
    }

    #[test]
    fn the_bridge_keeps_player_globals() {
        let apoc = "Apocalypse - Magic of Skyrim.esp";
        let e = |short_id: u32, value: f32| PlayerGlobalEntry { short_id, file: apoc.to_owned(), value };
        let list = new_player_globals(r#"["0b1c19:Apocalypse - Magic of Skyrim.esp", "043cef:Apocalypse - Magic of Skyrim.esp"]"#);
        assert!(list.as_ref().is_ok_and(|l| l.player_globals_listed() == vec![e(0xb1c19, 0.0), e(0x43cef, 0.0)]));
        assert!(list.as_ref().is_ok_and(|l| l.player_globals_check(&[e(0xb1c19, 3.0)]) == 0));
        assert!(list.as_ref().is_ok_and(|l| l.player_globals_check(&[e(0xb1c1a, 3.0)]) == 1));
        assert!(list.as_ref().is_ok_and(|l| l.player_globals_check(&[e(0xb1c19, f32::NAN)]) == 2));
        assert!(list.as_ref().is_ok_and(|l| l.player_globals_check(&[e(0xb1c19, 1.0), e(0xb1c19, 2.0)]) == 3));
        let record = player_globals_merge(&[e(0xb1c19, 3.0)], &[e(0xb1c19, 1.0), e(0x43cef, 2.0)]);
        assert_eq!(record, vec![e(0xb1c19, 1.0), e(0x43cef, 2.0)]);
        assert!(list.as_ref().is_ok_and(|l| l.player_globals_at_login(&[e(0x99999, 7.0), e(0x43cef, 2.0)]) == vec![e(0x43cef, 2.0)]));
        // memo D51 B: a killAt on a listed global names the recorded value the server kills from
        let lethal = new_player_globals(r#"[{"global": "43cef:Apocalypse - Magic of Skyrim.esp", "killAt": 10}]"#);
        assert!(lethal.as_ref().is_ok_and(|l| l.player_globals_lethal(&[e(0x43cef, 9.5)]).is_empty()));
        assert!(lethal.as_ref().is_ok_and(|l| l.player_globals_lethal(&[e(0x43cef, 10.0)]) == vec![e(0x43cef, 10.0)]));
        assert!(new_player_globals(r#"["0b1c19"]"#).is_err());
    }

    #[test]
    fn the_bridge_keeps_the_shared_quests() {
        let e = |short_id: u32, stage: u16| QuestStageEntry { short_id, file: "Skyrim.esm".to_owned(), stage };
        let list = new_shared_quests(r#"["074a83:Skyrim.esm"]"#);
        assert!(list.as_ref().is_ok_and(|l| l.shared_quests_mode() == 0));
        assert!(list.as_ref().is_ok_and(|l| l.shared_quests_listed() == vec![e(0x74a83, 0)]));
        assert!(list.as_ref().is_ok_and(|l| l.shared_quests_check(0x74a83, "Skyrim.esm") == 0));
        assert!(list.as_ref().is_ok_and(|l| l.shared_quests_check(0x74a84, "Skyrim.esm") == 1));
        // mode all (memo D58 B): the login names the denied quests, a denied
        // report is 2, and the login replays every recorded quest but those
        let all = new_shared_quests(r#"{"mode": "all", "denied": ["04e50d:Skyrim.esm"]}"#);
        assert!(all.as_ref().is_ok_and(|a| a.shared_quests_mode() == 1));
        assert!(all.as_ref().is_ok_and(|a| a.shared_quests_listed() == vec![e(0x4e50d, 0)]));
        assert!(all.as_ref().is_ok_and(|a| a.shared_quests_check(0x74a84, "Skyrim.esm") == 0));
        assert!(all.as_ref().is_ok_and(|a| a.shared_quests_check(0x4e50d, "Skyrim.esm") == 2));
        assert!(all.as_ref().is_ok_and(|a| a.shared_quests_at_login(&[e(0x4e50d, 5), e(0x74a84, 20), e(0x74a83, 10)]) == vec![e(0x74a84, 20), e(0x74a83, 10)]));
        assert!(new_shared_quests(r#"{"mode": "list"}"#).is_err());
        // thuum ADR-068 rung 5 (read 4): memo D62's allowed list is a
        // retired key, read, said once and ignored (the core's
        // IsSeatsOwnQuest retired with it, and the bridge's allowed stub)
        let allowed = new_shared_quests(r#"{"mode": "all", "denied": ["04e50d:Skyrim.esm"], "allowed": ["039645:Skyrim.esm"]}"#);
        assert!(allowed.as_ref().is_ok_and(|a| a.shared_quests_check(0x39645, "Skyrim.esm") == 0 && a.shared_quests_retired().starts_with("sharedQuests.allowed is retired")));
        assert!(allowed.as_ref().is_ok_and(|a| a.shared_quests_listed() == vec![e(0x4e50d, 0)] && a.shared_quests_check(0x39645, "Skyrim.esm") == 0));
        assert!(new_shared_quests(r#"{"mode": "all", "denied": ["039645:Skyrim.esm"], "allowed": ["39645:Skyrim.esm"]}"#).is_ok_and(|a| a.shared_quests_check(0x39645, "Skyrim.esm") == 2));
        assert!(all.as_ref().is_ok_and(|a| a.shared_quests_retired().is_empty()));
        // the player class composed in (D-Q1's derivation): code 3, named in
        // mode all's login after the denied, never replayed
        let mut marked = new_shared_quests(r#"{"mode": "all", "denied": ["04e50d:Skyrim.esm"]}"#).unwrap_or_else(|_| Box::new(SharedQuests(quest_stages::SharedQuests::default())));
        marked.shared_quests_set_player(&[e(0x2ba16, 0), e(0xeafd5, 0)]);
        assert_eq!(marked.shared_quests_check(0x2ba16, "Skyrim.esm"), 3);
        assert_eq!(marked.shared_quests_check(0x4e50d, "Skyrim.esm"), 2);
        assert_eq!(marked.shared_quests_listed(), vec![e(0x4e50d, 0), e(0x2ba16, 0), e(0xeafd5, 0)]);
        assert_eq!(marked.shared_quests_at_login(&[e(0xeafd5, 10), e(0x74a83, 10)]), vec![e(0x74a83, 10)]);
        let record = quest_stages_merge(&[e(0x74a83, 20)], &e(0x74a83, 10));
        assert_eq!(record, vec![e(0x74a83, 10), e(0x74a83, 20)]);
        assert!(quest_stage_done(&record, &e(0x74a83, 20)));
        assert!(!quest_stage_done(&record, &e(0x74a83, 30)));
        assert_eq!(quest_stages_merge(&record, &e(0x74a83, 20)), record);
        assert!(list.as_ref().is_ok_and(|l| l.shared_quests_at_login(&[e(0x74a84, 5), e(0x74a83, 20), e(0x74a83, 10)]) == vec![e(0x74a83, 10), e(0x74a83, 20)]));
        assert!(new_shared_quests(r#"["074a83"]"#).is_err());
        // the record's bound (review finding 2): full at QUEST_RECORD_CAP
        // pairs, when the merge leaves it as it is
        assert!(!quest_record_full(&record));
        let capped: Vec<QuestStageEntry> = (0..=u16::MAX).take(quest_stages::QUEST_RECORD_CAP).map(|s| e(0x74a83, s)).collect();
        assert_eq!(capped.len(), quest_stages::QUEST_RECORD_CAP);
        assert!(quest_record_full(&capped));
        assert_eq!(quest_stages_merge(&capped, &e(0x74a84, 10)), capped);
    }

    #[test]
    fn the_bridge_asks_places_as_reported_and_removes_a_set() {
        const PLUGIN: &str = "Apocalypse - Magic of Skyrim.esp";
        let table = r#"[{"spell": "00cc14:Apocalypse - Magic of Skyrim.esp", "anchor": "caster",
            "objects": [{"base": "098529:Apocalypse - Magic of Skyrim.esp", "type": "MSTT"},
                        {"base": "0735e8:Apocalypse - Magic of Skyrim.esp", "type": "ACTI"}],
            "lifetime": {"whileHeld": true, "lingerS": 5, "maxS": 120}, "perCaster": 1}]"#;
        let parsed = new_placements(table);
        assert!(parsed.is_ok());
        let Ok(mut t) = parsed else {
            return;
        };
        assert!(t.is_listed(0xcc14, PLUGIN));
        assert!(!t.is_listed(0xcc14, "Skyrim.esm"));
        let cast = |hand: u8, yaw: f32| PlacedCast { caster: 0xff00_0000, hand, x: 0.0, y: 0.0, z: 0.0, yaw, concentration: true };
        let report = |set: u64, short_id: u32, x: f32, y: f32, z: f32| PlacementReport { caster: 0xff00_0000, set, short_id, file: PLUGIN.to_owned(), x, y, z, yaw: 90.0 };
        assert!(t.take_requests().is_empty());
        assert!(t.cast_started(0xcc14, PLUGIN, cast(1, 90.0), 1_000));
        // the request: one row per base, both on set 0, the window the rule's
        let rows = t.take_requests();
        assert_eq!(rows.len(), 2);
        assert!(rows.first().is_some_and(|r| r.set == 0 && r.caster == 0xff00_0000 && r.spell_short_id == 0xcc14 && r.spell_file == PLUGIN && r.short_id == 0x98529 && r.record_type == "MSTT" && r.window_ms == placements::REPORT_WINDOW_MS), "{rows:?}");
        assert!(rows.get(1).is_some_and(|r| r.set == 0 && r.short_id == 0x735e8 && r.record_type == "ACTI"), "{rows:?}");
        assert!(t.take_requests().is_empty());
        assert!(t.due_placements(1_200).is_empty());
        // a report outside the band carries its numbers; one inside places
        let far = t.report_placed(report(0, 0x98529, 400.0, 0.0, 0.0), 1_300);
        assert!(!far.accepted && far.why == 6 && (far.distance - 400.0).abs() < 0.01, "{far:?}");
        let wall = t.report_placed(report(0, 0x98529, 206.25, 0.0, -48.0), 1_400);
        assert!(wall.accepted && wall.why == 0, "{wall:?}");
        let rows = t.due_placements(1_400);
        assert_eq!(rows.len(), 1);
        assert!(rows.first().is_some_and(|r| r.set == 0 && r.caster == 0xff00_0000 && r.spell_short_id == 0xcc14 && r.short_id == 0x98529 && r.record_type == "MSTT" && (r.x - 206.25).abs() < 0.01 && (r.z + 48.0).abs() < 0.01 && (r.yaw - 90.0).abs() < 0.01), "{rows:?}");
        assert_eq!(t.live_sets(), 1);
        let again = t.report_placed(report(0, 0x98529, 206.25, 0.0, -48.0), 1_500);
        assert!(!again.accepted && again.why == 4, "{again:?}");
        let halo = t.report_placed(report(0, 0x735e8, 196.875, 0.0, 0.0), 1_600);
        assert!(halo.accepted, "{halo:?}");
        assert!(t.due_placements(1_600).first().is_some_and(|r| r.set == 0 && r.short_id == 0x735e8 && r.record_type == "ACTI" && r.z.abs() < 0.01));
        let none = t.report_placed(report(9, 0x735e8, 196.875, 0.0, 0.0), 1_700);
        assert!(!none.accepted && none.why == 1, "{none:?}");
        // a second cast over the caster's cap of one is refused at the cast
        assert!(t.cast_started(0xcc14, PLUGIN, cast(0, 0.0), 2_000));
        assert!(t.take_requests().is_empty());
        assert_eq!(t.take_refusals(), vec![PlacementRefusal { caster: 0xff00_0000, spell_short_id: 0xcc14, spell_file: PLUGIN.to_owned(), why: 0 }]);
        t.cast_ended(0xff00_0000, 1, 10_000);
        assert!(t.due_removals(14_999).is_empty());
        assert_eq!(t.due_removals(15_000), vec![PlacementRemoval { set: 0, caster: 0xff00_0000, why: 0 }]);
        assert_eq!(t.live_sets(), 0);
        // a cast nobody reports is noted when its window closes
        assert!(t.cast_started(0xcc14, PLUGIN, cast(1, 90.0), 20_000));
        assert_eq!(t.take_requests().len(), 2);
        assert!(t.due_placements(20_000 + placements::REPORT_WINDOW_MS + 1).is_empty());
        assert_eq!(t.take_refusals().first().map(|r| r.why), Some(2));
        t.caster_gone(0xff00_0000, 30_000);
        assert!(new_placements(r#"[{"spell": "cc14"}]"#).is_err());
        // shape C's offsets are read and ignored
        assert!(new_placements(r#"[{"spell": "00cc14:Apocalypse - Magic of Skyrim.esp", "anchor": "caster", "objects": [{"base": "098529:Apocalypse - Magic of Skyrim.esp", "type": "MSTT", "forward": 192, "dz": -48}], "lifetime": {"whileHeld": true, "lingerS": 5, "maxS": 120}, "perCaster": 1}]"#).is_ok());
    }

    #[test]
    fn the_bridge_levels_a_skill_from_the_records_constants() {
        let mut t = new_leveling_table();
        assert!((t.use_xp(6, 10.0) - 63.0).abs() < 0.001);
        // Skyrim.esm's Smithing AVSK replaces UESP's reading
        assert!(t.set_skill(10, 160.0, 0.0, 0.25, 300.0));
        assert!((t.use_xp(10, 1.0) - 160.0).abs() < 0.001);
        assert!(!t.set_skill(5, 1.0, 0.0, 1.0, 0.0));
        assert!(!t.set_skill(10, f32::NAN, 0.0, 1.0, 0.0));
        assert!((t.use_xp(10, 1.0) - 160.0).abs() < 0.001);
        assert!((t.level_cost(6, 15) - 2.0 * 15.0f32.powf(1.95)).abs() < 0.01);
        assert_eq!(t.use_xp(5, 10.0), 0.0);
        assert_eq!(leveling_character_threshold(1), 100.0);
        // seven hits of a base of 10: One-handed 15 to 16, the character at 16 toward 100
        let mut skills = vec![LevelingSkill { skill_av: 6, level: 15, xp: 0.0 }];
        let mut character = LevelingCharacter { level: 1, xp: 0.0 };
        let mut outcome = LevelingOutcome { accepted: false, why: 0, skills: Vec::new(), character: character.clone(), events: Vec::new() };
        for _ in 0..7 {
            outcome = t.apply_use(&skills, character.clone(), 6, 10.0);
            assert!(outcome.accepted, "{outcome:?}");
            skills = outcome.skills.clone();
            character = outcome.character.clone();
        }
        assert_eq!(outcome.events, vec![LevelingEvent { kind: 0, skill_av: 6, level: 16 }]);
        assert_eq!(skills.len(), 18);
        assert!(skills.iter().any(|s| s.skill_av == 6 && s.level == 16));
        assert!(skills.iter().any(|s| s.skill_av == 23 && s.level == 15 && s.xp == 0.0));
        assert_eq!(character.level, 1);
        assert!((character.xp - 16.0).abs() < 0.001);
        // the refusals by number
        assert_eq!(t.apply_use(&skills, character.clone(), 5, 10.0).why, 1);
        assert_eq!(t.apply_use(&skills, character.clone(), 6, f32::NAN).why, 2);
        assert_eq!(t.apply_use(&skills, character.clone(), 6, 0.0).why, 3);
        // a skill book: One-handed 16 to 17 at once, the character credited 17 more (33 toward 100)
        let book = t.apply_book(&skills, character.clone(), 6);
        assert!(book.accepted, "{book:?}");
        assert_eq!(book.events, vec![LevelingEvent { kind: 0, skill_av: 6, level: 17 }]);
        assert!(book.skills.iter().any(|s| s.skill_av == 6 && s.level == 17));
        assert!((book.character.xp - 33.0).abs() < 0.001);
        // for no skill: refused by number, nothing moved
        let none = t.apply_book(&skills, character.clone(), 5);
        assert!(!none.accepted);
        assert_eq!(none.why, 1);
        assert!(none.skills.iter().any(|s| s.skill_av == 6 && s.level == 16));
    }

    #[test]
    fn the_bridge_judges_a_lock() {
        let novice = LockStateRow { level: 0, key: 0, locked: true, broken: 0 };
        let keyed = LockStateRow { level: 255, key: 0x1234, locked: true, broken: 0 };
        assert_eq!(lock_activation(&novice, false).verdict, 2);
        assert_eq!(lock_activation(&keyed, false).verdict, 2);
        let opened = lock_activation(&keyed, true);
        assert_eq!(opened.verdict, 1);
        assert!(!opened.state.locked);
        assert_eq!(lock_activation(&opened.state, false).verdict, 0);
        let picked = lock_report(&novice, 0, 0, true, 2, u64::MAX);
        assert!(picked.accepted && picked.change == 1 && !picked.state.locked, "{picked:?}");
        assert_eq!(lock_report(&picked.state, 0, 0, true, 2, u64::MAX).why, 1);
        assert_eq!(lock_report(&novice, 0, 0, false, 2, u64::MAX).why, 2);
        assert_eq!(lock_report(&keyed, 0, 0, true, 2, u64::MAX).why, 3);
        assert_eq!(lock_report(&novice, 0, 0, true, 0, u64::MAX).why, 4);
        assert_eq!(lock_report(&novice, 1, 0, true, 2, u64::MAX).why, 5);
        assert_eq!(lock_report(&novice, 0, 0, true, 2, 100).why, 6);
        let broke = lock_report(&novice, 1, 5, true, 2, u64::MAX);
        assert!(broke.accepted && broke.change == 2 && broke.picks == 2 && broke.state.locked && broke.state.broken == 2, "{broke:?}");
        let left = lock_report(&novice, 2, 0, true, 0, u64::MAX);
        assert!(left.accepted && left.change == 0 && left.state == novice, "{left:?}");
    }

    #[test]
    fn the_bridge_judges_a_destruction_report() {
        // thuum D-S3: the lamp's first shot over the game files' state, the
        // second seat's copy, a lower report, the spent tripwire, the bound
        let none = DestructionRow { stage: 0, destroyed: false };
        let first = destruction_report(&none, 1, false);
        assert_eq!(first, DestructionVerdict { verdict: 0, record: DestructionRow { stage: 1, destroyed: false } });
        assert_eq!(destruction_report(&first.record, 1, false), DestructionVerdict { verdict: 1, record: first.record.clone() });
        let two = destruction_report(&first.record, 2, false).record;
        assert_eq!(destruction_report(&two, 1, false).verdict, 1);
        assert_eq!(destruction_report(&two, 1, false).record, two);
        let spent = destruction_report(&none, 0, true);
        assert_eq!(spent.record, DestructionRow { stage: 0, destroyed: true });
        assert_eq!(destruction_report(&spent.record, 0, false).verdict, 1);
        assert_eq!(destruction_max_stage(), destruction::MAX_STAGE);
        // the rule's bound is the edge's (wire-validate refuses past it)
        assert_eq!(destruction_max_stage(), wire_validate::DESTRUCTION_MAX_STAGE);
        let past = destruction_report(&two, destruction_max_stage().saturating_add(1), true);
        assert_eq!(past, DestructionVerdict { verdict: 2, record: two });
    }

    #[test]
    fn the_bridge_names_cast_teleports() {
        let named = new_cast_teleports(r#"[{"spell": "a86df:LostGrimoire.esp", "reach": 3000}]"#);
        assert!(named.as_ref().is_ok_and(|t| (t.teleport_reach(0xa86df, "LostGrimoire.esp") - 3000.0).abs() < 1e-3));
        assert!(named.as_ref().is_ok_and(|t| t.teleport_reach(0x12fd0, "Skyrim.esm") < 0.0));
        assert!(new_cast_teleports("[]").is_ok_and(|t| t.teleport_reach(0xa86df, "LostGrimoire.esp") < 0.0));
        assert!(new_cast_teleports(r#"[{"spell": "a86df", "reach": 3000}]"#).is_err());
        // a teleport waits the default and is not relayed; a return names both
        assert!(named.as_ref().is_ok_and(|t| t.teleport_wait_ms(0xa86df, "LostGrimoire.esp") == 5_000));
        assert!(named.as_ref().is_ok_and(|t| !t.teleport_relays(0xa86df, "LostGrimoire.esp")));
        assert!(named.as_ref().is_ok_and(|t| t.teleport_wait_ms(0x12fd0, "Skyrim.esm") == 0 && !t.teleport_relays(0x12fd0, "Skyrim.esm")));
        let ghostwalk = new_cast_teleports(r#"[{"spell": "aae8:Apocalypse - Magic of Skyrim.esp", "reach": 0, "waitMs": 15400, "relay": true}]"#);
        assert!(ghostwalk.as_ref().is_ok_and(|t| t.teleport_reach(0xaae8, "Apocalypse - Magic of Skyrim.esp").abs() < 1e-3));
        assert!(ghostwalk.as_ref().is_ok_and(|t| t.teleport_wait_ms(0xaae8, "Apocalypse - Magic of Skyrim.esp") == 15_400));
        assert!(ghostwalk.as_ref().is_ok_and(|t| t.teleport_relays(0xaae8, "Apocalypse - Magic of Skyrim.esp")));
    }

    #[test]
    fn the_bridge_passes_a_teleports_look() {
        let looked = new_cast_teleports(
            r#"[{"spell": "a86df:LostGrimoire.esp", "reach": 8000, "look": {"vanish": "102731:Skyrim.esm",
                "appear": "102730:Skyrim.esm", "appearSeconds": 1.75, "sound": "010ec4:Dawnguard.esm"}},
               {"spell": "aae8:Apocalypse - Magic of Skyrim.esp", "reach": 0, "look": {"sound": "010ec4:Dawnguard.esm"}}]"#,
        );
        let look = looked.as_ref().map(|t| t.teleport_look(0xa86df, "lostgrimoire.esp"));
        assert!(look.as_ref().is_ok_and(|l| l.has_vanish && l.vanish.plugin == "Skyrim.esm" && l.vanish.id == 0x102731));
        assert!(look.as_ref().is_ok_and(|l| l.has_appear && l.appear.plugin == "Skyrim.esm" && l.appear.id == 0x102730));
        assert!(look.as_ref().is_ok_and(|l| (l.appear_seconds - 1.75).abs() < 1e-6));
        assert!(look.as_ref().is_ok_and(|l| l.has_sound && l.sound.plugin == "Dawnguard.esm" && l.sound.id == 0x10ec4));
        // a look of one part names the others not
        let sound = looked.as_ref().map(|t| t.teleport_look(0xaae8, "Apocalypse - Magic of Skyrim.esp"));
        assert!(sound.as_ref().is_ok_and(|l| !l.has_vanish && !l.has_appear && l.has_sound && l.sound.id == 0x10ec4));
        // no look without an entry's, nor for a spell the server names no teleport
        let plain = new_cast_teleports(r#"[{"spell": "a86df:LostGrimoire.esp", "reach": 8000}]"#);
        for l in [plain.as_ref().map(|t| t.teleport_look(0xa86df, "LostGrimoire.esp")), looked.as_ref().map(|t| t.teleport_look(0x12fd0, "Skyrim.esm"))] {
            assert!(l.is_ok_and(|l| !l.has_vanish && !l.has_appear && !l.has_sound));
        }
        assert!(new_cast_teleports(r#"[{"spell": "a86df:LostGrimoire.esp", "reach": 8000, "look": {}}]"#).is_err());
        // the landing a step no run covers
        assert!(teleport_look_lands(549.0) && !teleport_look_lands(86.0) && !teleport_look_lands(f32::NAN));
    }

    #[test]
    fn the_bridge_passes_effects_through() {
        // DB03Poison's AlchDamageHealthDuration: 6 a second for 10 s, Health
        let poison = EffectFacts {
            effect: 0x10aa4a,
            source: 0x58cfb,
            kind: EffectKind::Value,
            av: 24,
            second_av: u32::MAX,
            second_weight: 0.0,
            magnitude: 6.0,
            duration_s: 10.0,
            recover: false,
            detrimental: true,
            no_duration: false,
        };
        let applied = apply_effect(poison, 1.0, 7, EffectArrival::Once);
        assert!(applied.changes.is_empty());
        assert!(applied.has_entry);
        let mut entries = Vec::new();
        assert!(admit_effect(&mut entries, applied.entry).is_empty());
        let hurt: f32 = advance_effects(&mut entries, 4.0).iter().map(|c| c.current).sum();
        assert!((hurt + 24.0).abs() < 1e-3);
        assert_eq!(entries.first().map(|e| e.caster), Some(7));
        assert!(effect_modifier(&entries, 24).abs() < 1e-6);
        let rest: f32 = advance_effects(&mut entries, 10.0).iter().map(|c| c.current).sum();
        assert!((rest + 36.0).abs() < 1e-3);
        assert!(entries.is_empty());
        // a stream's hit counts at once, the second value at its weight
        let shock = EffectFacts { kind: EffectKind::DualValue, second_av: 25, second_weight: 0.5, magnitude: 8.0, duration_s: 1.0, ..poison };
        let hit = apply_effect(shock, 0.25, 7, EffectArrival::Stream);
        assert!(!hit.has_entry);
        let magicka: f32 = hit.changes.iter().filter(|c| c.av == 25).map(|c| c.current).sum();
        assert!((magicka + 1.0).abs() < 1e-3);
        // Dead Thrall's reanimate: a state, its record's 86,313,600 s kept
        let thrall = EffectFacts { effect: 0x7e8e0, source: 0x7e8df, kind: EffectKind::Reanimate, av: u32::MAX, magnitude: 40.0, duration_s: 86_313_600.0, recover: true, detrimental: false, ..poison };
        let raised = apply_effect(thrall, 1.0, 7, EffectArrival::Once);
        assert!(raised.changes.is_empty() && raised.has_entry);
        assert!(matches!(raised.entry.facts.kind, EffectKind::Reanimate));
        assert!((raised.entry.facts.duration_s - 86_313_600.0).abs() < 1.0);
    }

    #[test]
    fn the_bridge_passes_casts_through() {
        let mut c = new_spell_casts();
        let flames = CastFacts {
            spell: 0x12fcd,
            hand: 1,
            kind: CastKind::Concentration,
            area: false,
            reach: 1_000.0,
            x: 0.0,
            y: 0.0,
            z: 0.0,
            reached: [0; 8],
            linger_ms: 0,
            reached_concentration: 0,
            lands_on_target: false,
            hazard_ms: 0,
            dual: false,
        };
        let on = |t| CastHit { spell: 0x12fcd, target: 2, x: 150.0, y: 0.0, z: t };
        assert_eq!(c.claim(7, on(0.0), 0).check, CastCheck::NoCast);
        c.start(7, flames, 0);
        assert!(c.is_open(7, 1));
        let claim = c.claim(7, on(0.0), 200);
        assert_eq!(claim.check, CastCheck::Claimed);
        assert!((claim.scale - 0.2).abs() < f32::EPSILON);
        assert_eq!(c.claim(7, on(5_000.0), 300).check, CastCheck::TooFar);
        c.end(7, 1);
        assert!(!c.is_open(7, 1));
        c.forget(7);
        assert_eq!(c.claim(7, on(0.0), 400).check, CastCheck::NoCast);
    }

    #[test]
    fn the_bridge_passes_reached_claims_through() {
        let mut c = new_spell_casts();
        // labels, not records: a cloak and the spell its records reach
        let mut reached = [0; 8];
        reached[0] = 0xa102;
        let cloak =
            CastFacts { spell: 0xa101, hand: 1, kind: CastKind::FireAndForget, area: false, reach: 512.0, x: 0.0, y: 0.0, z: 0.0, reached, linger_ms: 60_000, reached_concentration: 0, lands_on_target: false, hazard_ms: 0, dual: false };
        let proc_on = |x| CastHit { spell: 0xa102, target: 2, x, y: 0.0, z: 0.0 };
        assert_eq!(c.claim_reached(7, proc_on(300.0), 0.0, 0.0, 0.0, 0).check, CastCheck::NoCast);
        c.start(7, cloak, 0);
        let claim = c.claim_reached(7, proc_on(300.0), 0.0, 0.0, 0.0, 1_000);
        assert_eq!(claim.check, CastCheck::Claimed);
        assert!((claim.scale - 1.0).abs() < f32::EPSILON);
        assert_eq!(c.claim_reached(7, proc_on(300.0), 0.0, 0.0, 0.0, 1_100).check, CastCheck::Throttled);
        assert_eq!(c.claim_reached(7, proc_on(9_000.0), 0.0, 0.0, 0.0, 2_000).check, CastCheck::TooFar);
    }

    #[test]
    fn the_bridge_passes_shouts_through() {
        let mut v = new_voices();
        // Unrelenting Force's second variation with its first two words
        let two = VoiceFacts { shout_equipped: true, variation: 1, words_unlocked: 0b011, recovery_s: 20.0, recovery_mult: 1.0 };
        assert_eq!(v.shout(7, &VoiceFacts { words_unlocked: 0b010, ..two }, 0), VoiceRefusal::WordLocked);
        assert_eq!(v.shout(7, &VoiceFacts { variation: 2, ..two }, 0), VoiceRefusal::WordLocked);
        assert_eq!(v.shout(7, &VoiceFacts { variation: 7, ..two }, 0), VoiceRefusal::NotAVariation);
        assert_eq!(v.shout(7, &VoiceFacts { shout_equipped: false, ..two }, 0), VoiceRefusal::NoShout);
        assert_eq!(v.shout(7, &two, 0), VoiceRefusal::Accepted);
        assert_eq!(v.recovery_left_ms(7, 5_000), 15_000);
        assert_eq!(v.shout(7, &two, 5_000), VoiceRefusal::Recovering);
        assert_eq!(v.shout(7, &two, 20_000), VoiceRefusal::Accepted);
    }

    #[test]
    fn the_bridge_passes_shots_through() {
        let mut s = new_ranged_shots();
        assert_eq!(s.claim(7, 0x3b562, 100.0, 0.0, 0.0, 0).check, ShotCheck::NoShot);
        s.record(7, ShotFacts { weapon: 0x3b562, power: 0.5, x: 0.0, y: 0.0, z: 0.0 }, 0);
        assert_eq!(s.claim(7, 0x3b562, 9_000.0, 0.0, 0.0, 10).check, ShotCheck::TooFar);
        let claim = s.claim(7, 0x3b562, 100.0, 0.0, 0.0, 10);
        assert_eq!(claim.check, ShotCheck::Claimed);
        assert!((claim.power - 0.5).abs() < f32::EPSILON);
        s.record(7, ShotFacts { weapon: 0x3b562, power: 1.0, x: 0.0, y: 0.0, z: 0.0 }, 20);
        s.forget(7);
        assert_eq!(s.claim(7, 0x3b562, 100.0, 0.0, 0.0, 30).check, ShotCheck::NoShot);
    }

    #[test]
    fn the_bridge_passes_the_rest_through() {
        let facts = |hours, is_dead, has_hit, since_last_hit_ms| RestFacts { hours, sleep: false, is_dead, has_hit, since_last_hit_ms, in_fight: false };
        let on = new_rest_settings("{}");
        assert!(on.is_ok());
        let Ok(on) = on else { return };
        assert_eq!(rest_check(&on, &facts(8.0, false, false, 0)), RestRefusal::Allowed);
        assert_eq!(rest_check(&on, &facts(8.0, false, true, 10_000)), RestRefusal::Allowed);
        assert_eq!(rest_check(&on, &facts(8.0, false, true, 3_000)), RestRefusal::Fighting);
        assert_eq!(rest_check(&on, &facts(0.5, false, false, 0)), RestRefusal::Hours);
        assert_eq!(rest_check(&on, &facts(8.0, true, false, 0)), RestRefusal::Dead);
        let no_wait = new_rest_settings(r#"{"allowWait": false}"#);
        assert!(no_wait.is_ok());
        let Ok(no_wait) = no_wait else { return };
        assert_eq!(rest_check(&no_wait, &facts(8.0, false, false, 0)), RestRefusal::Off);
        assert!(new_rest_settings(r#"{"allowWait": "no"}"#).is_err());
        let half = Regen { percentage: 0.5, rate: 0.7, rate_mult: 1.0 };
        assert!((rest_after(&half, 1.0) - 0.5252).abs() < 1e-5);
    }

    #[test]
    fn the_bridge_passes_sleep_through() {
        let bed = |has_bed, since_bed_activation_ms, distance_to_bed| BedFacts { has_bed, since_bed_activation_ms, distance_to_bed };
        assert!(rest_slept(&bed(true, 5_000, 80.0)));
        assert!(!rest_slept(&bed(false, 0, 0.0)));
        assert!(!rest_slept(&bed(true, 5_000, 900.0)));
        assert_eq!(rested_ms(true, 20.0), 1_440_000);
        assert_eq!(rested_ms(false, 20.0), 0);
    }

    #[test]
    fn the_bridge_passes_favorites_through() {
        let f = |form, hotkey, kind| FavoriteFacts { form, hotkey, kind };
        let got = favorites_kept(&[
            f(1, 0, FavoriteKind::HeldItem),
            f(2, 1, FavoriteKind::MissingItem),
            f(3, 1, FavoriteKind::Magic),
            f(4, -1, FavoriteKind::Other),
        ]);
        assert_eq!(got.iter().map(|e| (e.form, e.hotkey)).collect::<Vec<_>>(), vec![(1, 0), (3, 1)]);
    }

    #[test]
    fn the_bridge_passes_hostility_through() {
        let players = HostilityFacts { aggressor_is_player: true, aggressor_is_hosted: false, target_is_player: true, target_is_hosted: false, same_actor: false };
        assert!(hostility_counted(&players));
        assert!(!hostility_counted(&HostilityFacts { same_actor: true, ..players }));
        assert!(hostility_counted(&HostilityFacts { aggressor_is_player: false, aggressor_is_hosted: true, ..players }));
        assert!(!hostility_counted(&HostilityFacts { aggressor_is_player: false, ..players }));
        assert!(hostility_apart(hostility::EXTERIOR_RANGE + 1.0, false));
        assert!(!hostility_apart(hostility::INTERIOR_RANGE, true));
        let mut f = new_fights();
        assert!(f.hit(1, 2, 0));
        assert!(!f.hit(2, 1, 1_000));
        assert_eq!(f.pairs().iter().map(|p| (p.a, p.b)).collect::<Vec<_>>(), vec![(1, 2)]);
        assert!(f.in_fight(2));
        assert!(f.between(2, 1) && !f.between(1, 3));
        assert!(!f.check(1, 2, false, 2_000));
        assert!(f.check(1, 2, false, 1_000 + hostility::FIGHT_QUIET_MS));
        assert!(!f.in_fight(1));
        f.hit(3, 4, 0);
        f.forget(4);
        assert!(f.pairs().is_empty());
    }

    #[test]
    fn the_bridge_passes_the_clock_through() {
        // 2026-10-03T00:00:00Z, the default epoch: Skyrim.esm's start
        let epoch = 1_790_985_600_000;
        assert!(new_game_clock("{}").is_ok());
        let Ok(mut c) = new_game_clock("{}") else { return };
        let t = c.login(3, epoch);
        assert_eq!((t.year, t.month, t.day, t.hour, t.time_scale), (201, 7, 17, 8.0, 20.0));
        assert!((t.days_passed - (1.0 + 8.0 / 24.0)).abs() < 1e-6);
        assert!(!c.resync_due(3, epoch + 1));
        assert!(c.resync_due(3, epoch + clock::RESYNC_MS));
        c.forget(3);
        assert!(new_game_clock(r#"{"timeScale": 0}"#).is_err());
    }

    #[test]
    fn the_bridge_passes_script_calls_through() {
        // thuum ADR-068: Bleak Falls Temple's pillar 0xaa8cb turned by its
        // host 0xff000000 at epoch 3, a guest 0xff000001 beside it
        let text = ScriptArg { kind: 3, form_id: 0 };
        let pillar = ScriptArg { kind: 4, form_id: 0xaa8cb };
        let at = script_call_subject("ObjectReference", "PlayAnimationAndWait", 0xaa8cb, &[text, text]);
        assert!(at.row >= 0);
        assert_eq!(at.subject, 0xaa8cb);
        let send = script_call_subject("Debug", "SendAnimationEvent", 0xaa8cb, &[pillar, text]);
        assert_eq!(send.subject, 0xaa8cb);
        assert_eq!(script_call_subject("Debug", "Notification", 0, &[text]), ScriptCallSubject { row: -1, subject: 0, record: 0, look: 0 });
        assert_eq!(at.record, 0, "an action row keeps no record");
        let facts = ScriptCallFacts { sender: 0xff00_0000, self_id: 0xaa8cb, epoch: 3, subject_kind: 1, cell: 0x371de, owner: 0xff00_0000, owner_epoch: 3, host: 0, host_epoch: 0, firing: NO_FIRING };
        let mut s = new_script_calls();
        let v = s.judge(at.row, &[text, text], &facts, 0);
        assert_eq!((v.code, v.subject, v.seq, v.relay_self), (ScriptCallCode::Accepted, 0xaa8cb, 1, 0xaa8cb));
        assert_eq!((v.relay_cls.as_str(), v.relay_fn.as_str(), v.relay_args.as_slice()), ("ObjectReference", "PlayAnimation", &[0u8][..]));
        assert_eq!((v.audience, v.keep_last, v.event_arg, v.name.as_str()), (0, true, 0, ""));
        let static_call = s.judge(send.row, &[pillar, text], &facts, 0);
        assert_eq!((static_call.code, static_call.relay_self, static_call.seq, static_call.event_arg), (ScriptCallCode::Accepted, 0, 2, 1));
        assert_eq!(static_call.relay_args, vec![0u8, 1]);
        // the refusals cross with their names
        let guest = s.judge(at.row, &[text, text], &ScriptCallFacts { sender: 0xff00_0001, ..facts }, 0);
        assert_eq!((guest.code, guest.name.as_str()), (ScriptCallCode::Owner, "E_SCRIPTCALL_OWNER"));
        assert_eq!(s.judge(-1, &[text], &facts, 0).name, "E_SCRIPTCALL_ROW");
        assert_eq!(s.judge(at.row, &[text], &facts, 0).name, "E_SCRIPTCALL_ARGS");
        assert_eq!(s.judge(at.row, &[text, text], &ScriptCallFacts { subject_kind: 2, ..facts }, 0).code, ScriptCallCode::Skipped);
        // kind 4, a placement the server holds no reference for (Bleak Falls Sanctum 0x371dd's door FX 0x5256c,
        // an MSTT): judged by its record's cell, the refusal's words naming it; an unknown kind reads as no reference
        let fx = ScriptCallFacts { self_id: 0x5256c, subject_kind: 4, cell: 0x371dd, ..facts };
        let fx_at = script_call_subject("ObjectReference", "PlayAnimation", 0x5256c, &[text]);
        let placed = s.judge(fx_at.row, &[text], &fx, 0);
        assert_eq!((placed.code, placed.subject, placed.seq, placed.audience, placed.keep_last), (ScriptCallCode::Accepted, 0x5256c, 1, 0, true));
        let unhosted = s.judge(fx_at.row, &[text], &ScriptCallFacts { sender: 0xff00_0001, ..fx }, 0);
        assert_eq!((unhosted.code, unhosted.name.as_str()), (ScriptCallCode::Owner, "E_SCRIPTCALL_OWNER"));
        assert!(unhosted.why.starts_with("ff000000 hosts cell 371dd, not the reporter (") && unhosted.why.contains("record"), "{}", unhosted.why);
        let unknown = s.judge(fx_at.row, &[text], &ScriptCallFacts { subject_kind: 5, ..fx }, 0);
        assert_eq!((unknown.code, unknown.why.as_str()), (ScriptCallCode::Owner, "the server holds no such reference"));
        // the budget, wire-rules natives' burst (the bridge names it, never a literal)
        let mut b = new_script_calls();
        for _ in 0..natives::BURST {
            assert_eq!(b.judge(at.row, &[text, text], &facts, 0).code, ScriptCallCode::Accepted);
        }
        let spent = b.judge(at.row, &[text, text], &facts, 0);
        assert_eq!((spent.code, spent.name.as_str()), (ScriptCallCode::Rate, "E_SCRIPTCALL_RATE"));
    }

    #[test]
    fn the_bridge_keeps_a_trap_hit_as_its_actors_evidence() {
        // thuum docs/verbs/npc-fate.md (playtest 48): Bleak Falls Temple's
        // draugr 0x800a6 hit by the middle blade 0x3ac35 on its host
        // 0xff000000, which simulates it; kind 3, an actor, its simulator
        // the facts' owner
        let number = ScriptArg { kind: 2, form_id: 0 };
        let blade = ScriptArg { kind: 4, form_id: 0x3ac35 };
        let args = [blade, number, number, number, number, number, number, number, number, number, number];
        let at = script_call_subject("ObjectReference", "ProcessTrapHit", 0x800a6, &args);
        assert!(at.row >= 0);
        assert_eq!(at.subject, 0x800a6);
        let facts = ScriptCallFacts { sender: 0xff00_0000, self_id: 0x800a6, epoch: 3, subject_kind: 3, cell: 0x371de, owner: 0xff00_0000, owner_epoch: 0, host: 0xff00_0000, host_epoch: 3, firing: NO_FIRING };
        let mut s = new_script_calls();
        let v = s.judge(at.row, &args, &facts, 0);
        assert_eq!((v.code, v.subject, v.seq, v.keep_evidence, v.keep_last, v.audience), (ScriptCallCode::Accepted, 0x800a6, 0, true, false, 2));
        assert_eq!((v.relay_cls.as_str(), v.relay_fn.as_str(), v.relay_args.len(), v.event_arg, v.name.as_str()), ("", "", 0, -1, ""));
        // a guest's report and the reporter's own player cross with their names
        let guest = s.judge(at.row, &args, &ScriptCallFacts { sender: 0xff00_0001, ..facts }, 0);
        assert_eq!((guest.code, guest.name.as_str(), guest.keep_evidence), (ScriptCallCode::Owner, "E_SCRIPTCALL_OWNER", false));
        let own = s.judge(at.row, &args, &ScriptCallFacts { self_id: 0x14, ..facts }, 0);
        assert_eq!((own.code, own.name.as_str()), (ScriptCallCode::Row, "E_SCRIPTCALL_ROW"));
        // an action row's relay keeps no evidence
        let text = ScriptArg { kind: 3, form_id: 0 };
        let play = script_call_subject("ObjectReference", "PlayAnimation", 0xaa8cb, &[text]);
        let pillar = ScriptCallFacts { self_id: 0xaa8cb, subject_kind: 1, owner_epoch: 3, ..facts };
        let relayed = s.judge(play.row, &[text], &pillar, 0);
        assert_eq!((relayed.code, relayed.keep_evidence, relayed.seq), (ScriptCallCode::Accepted, false, 1));
    }

    #[test]
    fn the_bridge_reads_a_reports_word_and_records_the_runs() {
        // thuum ADR-068 rung 5 (read 5): a stage-0 report is a start, and
        // since 2026-11-10 a start-up stage's too (FreeformHeartwoodMill
        // 0xe3ea5's flagged 10, its unflagged 20 a stage); the record, the
        // fold, the churn and the login's two halves
        assert_eq!((quest_report_word(0, 0, false), quest_report_word(1, 0, false), quest_report_word(2, 0, false), quest_report_word(0, 10, false), quest_report_word(3, 0, false), quest_report_word(0x101, 0, false)), (1, 1, 2, 0, 3, 3));
        assert_eq!((quest_report_word(0, 10, true), quest_report_word(0, 20, false), quest_report_word(2, 0, true), quest_report_word(3, 10, true)), (1, 0, 2, 3));
        let row = |short_id: u32, running: bool, seq: u32| QuestRunRow { short_id, file: "Skyrim.esm".to_owned(), running, seq };
        let mut runs = new_quest_runs();
        let v = runs.quest_runs_judge(&[], 0x10d3bb, "Skyrim.esm", 1, 0, 0xff00_0000, 0);
        assert_eq!((v.code, v.first, v.name.as_str()), (0, false, ""));
        let record = quest_runs_merge(&[], 0x10d3bb, "Skyrim.esm", 1);
        assert_eq!(record, vec![row(0x10d3bb, true, 1)]);
        assert_eq!(runs.quest_runs_judge(&record, 0x10d3bb, "Skyrim.esm", 1, 0, 0xff00_0001, 10).code, 1);
        assert_eq!(runs.quest_runs_judge(&record, 0x10d3bb, "Skyrim.esm", 9, 0, 0xff00_0001, 10).code, 1, "no run word is nothing");
        let record = quest_runs_merge(&record, 0x10d3bb, "Skyrim.esm", 2);
        assert_eq!(record, vec![row(0x10d3bb, false, 2)]);
        assert_eq!(quest_runs_merge(&record, 0x10d3bb, "Skyrim.esm", 0), record);
        // one seat's flips past the bound: held, first once, then quiet
        let mut word = 1;
        let mut last = QuestRunVerdict { code: 0, first: false, name: String::new(), why: String::new() };
        for t in 0..=quest_runs::RUN_FLIPS + 1 {
            last = runs.quest_runs_judge(&[], 0x1a428, "Dawnguard.esm", word, 0, 0xff00_0000, u64::try_from(t).unwrap_or(0));
            word = 3 - word;
        }
        assert_eq!((last.code, last.first, last.name.as_str()), (2, true, "E_QUEST_CHURN"));
        assert!(runs.quest_runs_held(0x1a428, "Dawnguard.esm"));
        assert!(!runs.quest_runs_judge(&[], 0x1a428, "Dawnguard.esm", 1, 0, 0xff00_0001, 100).first);
        let held = vec![QuestRunRow { short_id: 0x1a428, file: "Dawnguard.esm".to_owned(), running: true, seq: 4 }, row(0x10d3bb, false, 2)];
        assert_eq!(quest_runs_without(&held, 0x1a428, "Dawnguard.esm"), vec![row(0x10d3bb, false, 2)]);
        runs.quest_runs_gone(0xff00_0000);
        // the login: starts and stops still shared and not held
        let all = new_shared_quests(r#"{"mode": "all", "denied": ["04e50d:Skyrim.esm"]}"#).unwrap_or_else(|_| Box::new(SharedQuests(quest_stages::SharedQuests::default())));
        let record = vec![row(0x74a83, true, 1), row(0x4e50d, true, 1), row(0x10d3bb, false, 2)];
        let login = runs.quest_runs_at_login(&record, &all, &[]);
        let e = |short_id: u32| QuestStageEntry { short_id, file: "Skyrim.esm".to_owned(), stage: 0 };
        assert_eq!((login.starts, login.stops), (vec![e(0x74a83)], vec![e(0x10d3bb)]));
        // D-Q3 A: the stopped quest with a stage the login replays is started
        // ahead of it too, still stopped behind it; the denied quest's stage
        // names nothing
        let staged = [QuestStageEntry { stage: 10, ..e(0x10d3bb) }, QuestStageEntry { stage: 10, ..e(0x4e50d) }];
        let login = runs.quest_runs_at_login(&record, &all, &staged);
        assert_eq!((login.starts, login.stops), (vec![e(0x74a83), e(0x10d3bb)], vec![e(0x10d3bb)]));
        // the cap, by name
        let full: Vec<QuestRunRow> = (1..=u32::try_from(quest_runs::RUN_RECORD_CAP).unwrap_or(0)).map(|q| row(q, true, 1)).collect();
        let v = runs.quest_runs_judge(&full, 0x0010_0000, "Skyrim.esm", 1, 0, 0xff00_0000, 0);
        assert_eq!((v.code, v.name.as_str()), (3, "E_QUEST_RUNS_FULL"));
    }

    #[test]
    fn the_bridge_reads_a_player_aliass_location_change_as_the_seats_own() {
        // thuum D-Q9 B (train 52): the setting, the event rule and the
        // quest's player aliases the core's index reads (MQ101 0x3372b's
        // player alias 119 beside an NPC one, the forms illustrative)
        assert_eq!((quest_player_alias_cause_world(r#""seat""#).ok(), quest_player_alias_cause_world(r#""world""#).ok()), (Some(false), Some(true)));
        assert!(quest_player_alias_cause_world(r#""off""#).is_err());
        assert!(quest_root_is_seat_own("OnLocationChange", true) && quest_root_is_seat_own("ONLOCATIONCHANGE", true));
        assert!(!quest_root_is_seat_own("OnLocationChange", false) && !quest_root_is_seat_own("OnActivate", true));
        let alias = |id: u32, forced: u32, unique_actor: u32| QuestScopeAliasRow { id, location: false, forced, unique_actor, created: 0, create_at: 0, external: false, external_forced: 0, external_unique_actor: 0 };
        assert_eq!(quest_scope_player_aliases(&[alias(119, 0x14, 0), alias(12, 0, 0x2bfb3), alias(7, 0, 7)]), vec![119, 7]);
        assert!(quest_scope_player_aliases(&[QuestScopeAliasRow { location: true, ..alias(3, 0x14, 0) }]).is_empty());
    }

    #[test]
    fn the_bridge_refuses_a_seat_caused_word_and_a_story_instances_run() {
        // thuum ADR-068 rung 5, D-Q4 C: dunEmbershardQST 0xb6fef, which no
        // Story Manager quest node names, and FreeformRiverwood01 0x74a83,
        // which FreeformRiverwoodNode 0xb555d names (the run-cause read's
        // census of Skyrim.esm, 2026-11-10)
        let row = |short_id: u32, running: bool, seq: u32| QuestRunRow { short_id, file: "Skyrim.esm".to_owned(), running, seq };
        let e = |short_id: u32| QuestStageEntry { short_id, file: "Skyrim.esm".to_owned(), stage: 0 };
        assert_eq!((quest_story_instances_world(r#""seat""#).ok(), quest_story_instances_world(r#""world""#).ok()), (Some(false), Some(true)));
        assert!(quest_story_instances_world(r#""off""#).is_err());
        let mut runs = new_quest_runs();
        // the seat's own cause: refused, nothing counted; a world cause and
        // an older client's report (no cause) recorded
        let v = runs.quest_runs_judge(&[], 0xb6fef, "Skyrim.esm", 2, 2, 0xff00_0000, 0);
        assert_eq!((v.code, v.first, v.name.as_str()), (4, false, "E_QUEST_CAUSE"));
        assert!(v.why.contains("b6fef:Skyrim.esm"), "{}", v.why);
        assert_eq!(runs.quest_runs_judge(&[], 0xb6fef, "Skyrim.esm", 1, 1, 0xff00_0000, 0).code, 0);
        assert_eq!(runs.quest_runs_judge(&[], 0xb6fef, "Skyrim.esm", 1, 0, 0xff00_0000, 0).code, 0);
        // a Story Manager instance under "seat": refused whatever its cause
        // says, and the login serves none of its runs
        runs.quest_runs_set_story(&[e(0x74a83)], false);
        assert_eq!(runs.quest_runs_story_count(), 1);
        assert!(runs.quest_runs_story_named(0x74a83, "skyrim.esm") && !runs.quest_runs_story_named(0xb6fef, "Skyrim.esm"));
        let v = runs.quest_runs_judge(&[], 0x74a83, "Skyrim.esm", 1, 1, 0xff00_0000, 0);
        assert_eq!((v.code, v.name.as_str()), (5, "E_QUEST_INSTANCE"));
        assert_eq!(runs.quest_runs_judge(&[], 0x74a83, "Skyrim.esm", 1, 2, 0xff00_0000, 0).code, 4, "the cause first");
        let all = new_shared_quests(r#"{"mode": "all"}"#).unwrap_or_else(|_| Box::new(SharedQuests(quest_stages::SharedQuests::default())));
        let record = vec![row(0x74a83, true, 1), row(0xb6fef, true, 1)];
        let login = runs.quest_runs_at_login(&record, &all, &[]);
        assert_eq!((login.starts, login.stops), (vec![e(0xb6fef)], vec![]));
        // a stopped instance with its stage 10 (which crosses): no start
        // either
        let stopped = vec![row(0x74a83, false, 2)];
        let login = runs.quest_runs_at_login(&stopped, &all, &[QuestStageEntry { stage: 10, ..e(0x74a83) }]);
        assert_eq!((login.starts, login.stops), (vec![], vec![]));
        // "world": judged like any other quest, served at the login
        runs.quest_runs_set_story(&[e(0x74a83)], true);
        assert_eq!(runs.quest_runs_judge(&[], 0x74a83, "Skyrim.esm", 1, 1, 0xff00_0000, 0).code, 0);
        assert_eq!(runs.quest_runs_at_login(&record, &all, &[]).starts, vec![e(0x74a83), e(0xb6fef)]);
    }

    #[test]
    fn the_bridge_classes_a_quest_by_its_aliases_and_the_list() {
        // thuum ADR-068 rung 5: PlayerWerewolfQuest 0x2ba16's alias by ALUA
        // 0x7, PlayerVampireQuest 0xeafd5 by the list, MS13 0x39645 World
        let alias = |id: u32, forced: u32, unique_actor: u32| QuestScopeAliasRow { id, location: false, forced, unique_actor, created: 0, create_at: 0, external: false, external_forced: 0, external_unique_actor: 0 };
        let scope = new_quest_scope(r#""derived""#, r#"["0eafd5:Skyrim.esm"]"#).unwrap_or_else(|_| Box::new(QuestScope(quest_scope::QuestScope::default())));
        assert!(scope.quest_scope_acting());
        assert_eq!(scope.quest_scope_listed(), vec![QuestStageEntry { short_id: 0xeafd5, file: "Skyrim.esm".to_owned(), stage: 0 }]);
        assert_eq!(scope.quest_scope_class(0x2ba16, "Skyrim.esm", &[alias(0, 0, 7)]), 1);
        assert_eq!(scope.quest_scope_class(0xeafd5, "Skyrim.esm", &[]), 1);
        assert_eq!(scope.quest_scope_class(0x39645, "Skyrim.esm", &[alias(1, 0, 0x39646), alias(10, 0x1fc43, 0)]), 0);
        // a listed quest with a world alias stays World; an external fill
        // from the player's alias and a creation in the player's are Player
        assert_eq!(scope.quest_scope_class(0xeafd5, "Skyrim.esm", &[alias(0, 0x1fc43, 0)]), 0);
        let external = QuestScopeAliasRow { external: true, external_forced: 0x14, ..alias(0, 0, 0) };
        let created = QuestScopeAliasRow { created: 0x39647, create_at: alias_fills::CREATE_IN, ..alias(1, 0, 0) };
        assert_eq!(scope.quest_scope_class(0x12345, "Skyrim.esm", &[alias(0, 0x14, 0), created, QuestScopeAliasRow { id: 2, ..external }]), 1);
        assert_eq!(scope.quest_scope_class(0x12345, "Skyrim.esm", &[alias(0, 0x14, 0), QuestScopeAliasRow { location: true, ..alias(0, 0, 0) }]), 0);
        let off = new_quest_scope(r#""off""#, "[]");
        assert!(off.as_ref().is_ok_and(|s| !s.quest_scope_acting() && s.quest_scope_listed().is_empty()));
        assert!(new_quest_scope(r#""aliases""#, "[]").is_err());
        assert!(new_quest_scope(r#""off""#, r#"["0eafd5"]"#).is_err());
    }

    #[test]
    fn the_bridge_judges_and_merges_the_condition_variables_and_cuts_the_players() {
        // thuum ADR-068 rung 5 (D-V1 B, D-T2 B): MS13 0x39645's
        // ::camillaescort_var any seat reports, PlayerVampireQuest
        // 0xeafd5's ::vampirestatus_var its player's own
        let settings = r#"[{"form": "039645:Skyrim.esm", "script": "MS13QuestScript", "var": "::camillaescort_var"},
            {"form": "0eafd5:Skyrim.esm", "script": "PlayerVampireQuestScript", "var": "::vampirestatus_var"}]"#;
        let mut vars = new_script_variables(settings).unwrap_or_else(|_| Box::new(ScriptVariables(script_variables::ScriptVariables::default())));
        let hours = ScriptVarKeyRow { short_id: 0xeafd5, file: "Skyrim.esm".to_owned(), script: "PlayerVampireQuestScript".to_owned(), var: "<OnUpdateGameTime>".to_owned(), own: true };
        let kept = vars.script_variables_set_player(&[QuestStageEntry { short_id: 0xeafd5, file: "Skyrim.esm".to_owned(), stage: 0 }], std::slice::from_ref(&hours));
        assert_eq!(kept, 1);
        let watch = vars.script_variables_watch();
        assert_eq!(watch.iter().map(|k| (k.short_id, k.own)).collect::<Vec<_>>(), [(0x39645, false), (0xeafd5, true), (0xeafd5, true)]);
        assert_eq!(watch.last().map(|k| k.var.as_str()), Some("<OnUpdateGameTime>"));
        let entry = |short_id: u32, script: &str, var: &str, own: bool, k: u8, i: i32| ScriptVarRow { short_id, file: "Skyrim.esm".to_owned(), script: script.to_owned(), var: var.to_owned(), own, k, i, f: 0.0, b: false, seq: 0 };
        let escort = entry(0x39645, "MS13QuestScript", "::camillaescort_var", false, 3, 1);
        // one call a message (the fresh review's note 13): the escort
        // recorded, then its new value against the record the first entry
        // left, the vampire's status its player's own
        let status = entry(0xeafd5, "PlayerVampireQuestScript", "::vampirestatus_var", true, 3, 2);
        let first = vars.script_variables_report(&[], &[], &[escort.clone(), ScriptVarRow { i: 2, ..escort.clone() }, status.clone()]);
        assert_eq!(first.verdicts.iter().map(|v| (v.code, v.name.as_str())).collect::<Vec<_>>(), [(0, ""), (0, ""), (0, "")]);
        assert_eq!(first.seqs, [1, 2, 1]);
        assert_eq!(first.world.iter().map(|r| (r.short_id, r.i, r.seq, r.own)).collect::<Vec<_>>(), [(0x39645, 2, 2, false)]);
        assert_eq!(first.own.iter().map(|r| (r.short_id, r.i, r.seq, r.own)).collect::<Vec<_>>(), [(0xeafd5, 2, 1, true)]);
        // an equal value folds and leaves both records as they were
        let again = vars.script_variables_report(&first.world, &first.own, &[ScriptVarRow { i: 2, ..escort.clone() }, status.clone()]);
        assert_eq!((again.verdicts.iter().map(|v| v.code).collect::<Vec<_>>(), again.seqs.clone()), (vec![1, 1], vec![0, 0]));
        assert_eq!((again.world.clone(), again.own.clone()), (first.world.clone(), first.own.clone()));
        // the cut, both ways, a kind no condition compares and one the
        // record does not hold for the key, an unlisted key: each refused
        // by name, nothing written
        let refused = vars.script_variables_report(
            &first.world,
            &[],
            &[
                ScriptVarRow { own: false, ..status.clone() },
                ScriptVarRow { own: true, ..escort.clone() },
                ScriptVarRow { k: 2, ..escort.clone() },
                ScriptVarRow { k: 5, b: true, ..escort.clone() },
                entry(0x74a83, "S", "v", false, 3, 1),
            ],
        );
        assert_eq!(
            refused.verdicts.iter().map(|v| (v.code, v.name.as_str())).collect::<Vec<_>>(),
            [(2, "E_SCRIPTVAR_PLAYER"), (2, "E_SCRIPTVAR_PLAYER"), (2, "E_SCRIPTVAR_KIND"), (2, "E_SCRIPTVAR_KIND"), (2, "E_SCRIPTVAR_UNLISTED")]
        );
        assert_eq!((refused.world.clone(), refused.own.is_empty()), (first.world.clone(), true));
        // the login: the world's half to every seat, the player's own to it alone
        assert_eq!(vars.script_variables_at_login(&first.world, false).iter().map(|r| (r.i, r.seq)).collect::<Vec<_>>(), [(2, 2)]);
        assert_eq!(vars.script_variables_at_login(&first.own, true).iter().map(|r| (r.short_id, r.own, r.seq)).collect::<Vec<_>>(), [(0xeafd5, true, 1)]);
        assert!(vars.script_variables_at_login(&first.own, false).is_empty());
        assert!(new_script_variables(r#"[{"form": "039645"}]"#).is_err());
    }

    #[test]
    fn the_bridge_records_a_players_own_row_and_judges_it_into_its_record_or_its_overlay() {
        // thuum ADR-068 rung 5 (the two transformations): Ariadne
        // 0xff000001's VampireChange to KhajiitRaceVampire 0x88845 through
        // the script call's two steps (record 4, subject 0x14, no
        // sequence), then the rule over the attachment's FormLists (their
        // ids and KhajiitRace's illustrative), and rotfern's beast form
        // (WerewolfBeastRace 0xcdd84) as an overlay
        let race = ScriptArg { kind: 4, form_id: 0x88845 };
        let at = script_call_subject("Actor", "SetRace", 0x14, &[race]);
        assert!(at.row >= 0);
        assert_eq!((at.subject, at.record), (0x14, 4));
        let facts = ScriptCallFacts { sender: 0xff00_0001, self_id: 0x14, epoch: 0, subject_kind: 3, cell: 0, owner: 0, owner_epoch: 0, host: 0, host_epoch: 0, firing: NO_FIRING };
        let mut s = new_script_calls();
        let v = s.judge(at.row, &[race], &facts, 0);
        assert_eq!((v.code, v.subject, v.seq, v.record, v.audience), (ScriptCallCode::Accepted, 0x14, 0, 4, 2));
        let npc = s.judge(at.row, &[race], &ScriptCallFacts { self_id: 0x800a6, ..facts }, 0);
        assert_eq!((npc.code, npc.name.as_str()), (ScriptCallCode::Row, "E_SCRIPTCALL_ROW"));
        let lists = [ScriptPropertyRow { form: 0x0d00_1000, members: vec![0x13746, 0x13745] }, ScriptPropertyRow { form: 0x0d00_1001, members: vec![0x88794, 0x88845] }];
        let khajiit = PlayerOwnFacts { recorded_race: 0x13745, playable: false, restricted: false, permitted: false, held: false, kind: true, race_valid: false, leveled: false, held_count: 0, count: 0.0, into_held_open: false, stacks: 0, new_stack: false, landing_of: 0, in_reach: false };
        let mut overlays = new_player_overlays();
        let v = overlays.player_own_judge(0xff00_0001, "Actor", "SetRace", 0x88845, &lists, &khajiit);
        assert_eq!((v.code, v.name.as_str(), v.ended.open, v.overlay.open), (0, "", false, false));
        let unnamed = overlays.player_own_judge(0xff00_0001, "Actor", "AddSpell", 0x92c48, &lists, &khajiit);
        assert_eq!((unnamed.code, unnamed.name.as_str()), (4, "E_SCRIPTCALL_UNNAMED"));
        let restricted = overlays.player_own_judge(0xff00_0001, "Actor", "SetRace", 0x88845, &lists, &PlayerOwnFacts { restricted: true, ..khajiit });
        assert_eq!(restricted.name, "E_SCRIPTCALL_RACE");
        assert_eq!(overlays.player_own_judge(0xff00_0001, "Actor", "EquipSpell", 0x88845, &lists, &khajiit).name, "E_SCRIPTCALL_ROW");
        // Eli's D-R1 C: the inventory's two by their own class, with no
        // bound (no property names the gold), the add whole, the removal at
        // most what the record holds, the count's own bound, a leveled list
        // refused by name, and an Actor spelling of the pair no row
        let gold = PlayerOwnFacts { held_count: 40, count: 25.0, ..khajiit };
        let add = overlays.player_own_judge(0xff00_0001, "ObjectReference", "AddItem", 0xf, &[], &gold);
        assert_eq!((add.code, add.count, add.name.as_str()), (0, 25, ""));
        let take = overlays.player_own_judge(0xff00_0001, "objectreference", "removeitem", 0xf, &[], &PlayerOwnFacts { count: 1000.0, ..gold });
        assert_eq!((take.code, take.count), (0, 40));
        assert_eq!(overlays.player_own_judge(0xff00_0001, "ObjectReference", "RemoveItem", 0xf, &[], &PlayerOwnFacts { into_held_open: true, ..gold }).code, 3);
        assert_eq!(overlays.player_own_judge(0xff00_0001, "ObjectReference", "AddItem", 0xf, &[], &PlayerOwnFacts { count: 2.5, ..gold }).name, "E_SCRIPTCALL_ARGS");
        assert_eq!(overlays.player_own_judge(0xff00_0001, "ObjectReference", "AddItem", 0x10_bf6a, &[], &PlayerOwnFacts { leveled: true, kind: false, ..gold }).name, "E_SCRIPTCALL_LEVELED");
        assert_eq!(overlays.player_own_judge(0xff00_0001, "Actor", "AddItem", 0xf, &[], &gold).name, "E_SCRIPTCALL_ROW");
        // the seven's verdicts carry no count
        assert_eq!(overlays.player_own_judge(0xff00_0001, "Actor", "EquipItem", 0x88845, &lists, &PlayerOwnFacts { held: true, ..khajiit }).count, 0);
        // a form of another kind than the row's, named or not (the fresh
        // review's note 3), and an item outside an overlay that the record
        // lacks (note 4)
        assert_eq!(overlays.player_own_judge(0xff00_0001, "Actor", "SetRace", 0x88845, &lists, &PlayerOwnFacts { kind: false, ..khajiit }).name, "E_SCRIPTCALL_ARGS");
        assert_eq!(overlays.player_own_judge(0xff00_0001, "Actor", "EquipItem", 0x88845, &lists, &khajiit).name, "E_SCRIPTCALL_UNNAMED");
        assert_eq!(overlays.player_own_judge(0xff00_0001, "Actor", "EquipItem", 0x88845, &lists, &PlayerOwnFacts { held: true, ..khajiit }).code, 3);
        // the seventh row (Eli's D-E1 A): a head part the recorded race may
        // wear is recorded, one its list lacks refused
        let eyes = [ScriptPropertyRow { form: 0x24244, members: Vec::new() }];
        assert_eq!(overlays.player_own_judge(0xff00_0001, "Actor", "ChangeHeadPart", 0x24244, &eyes, &PlayerOwnFacts { race_valid: true, ..khajiit }).code, 0);
        assert_eq!(overlays.player_own_judge(0xff00_0001, "Actor", "ChangeHeadPart", 0x24244, &eyes, &khajiit).name, "E_SCRIPTCALL_UNNAMED");
        // the beast form: an overlay, its howl held in it, its return ending it
        let werewolf = [ScriptPropertyRow { form: 0xcdd84, members: Vec::new() }, ScriptPropertyRow { form: 0x92c48, members: Vec::new() }];
        let nord = PlayerOwnFacts { recorded_race: 0x13746, ..khajiit };
        let opened = overlays.player_own_judge(0xff00_0000, "Actor", "SetRace", 0xcdd84, &werewolf, &nord);
        assert_eq!((opened.code, opened.overlay.open, opened.overlay.race, opened.overlay.base), (1, true, 0xcdd84, 0x13746));
        assert_eq!(overlays.player_own_judge(0xff00_0000, "Actor", "AddShout", 0x92c48, &werewolf, &nord).overlay.shouts, vec![0x92c48]);
        assert!(overlays.player_overlay(0xff00_0000).open && !overlays.player_overlay(0xff00_0001).open);
        let back = overlays.player_own_judge(0xff00_0000, "Actor", "SetRace", 0x13746, &werewolf, &nord);
        assert_eq!((back.code, back.ended.open, back.ended.shouts.clone(), back.overlay.open), (2, true, vec![0x92c48], false));
        // a disconnect mid-form ends it
        assert_eq!(overlays.player_own_judge(0xff00_0000, "Actor", "SetRace", 0xcdd84, &werewolf, &nord).code, 1);
        assert_eq!(overlays.player_overlay_gone(0xff00_0000).race, 0xcdd84);
        assert!(!overlays.player_overlay_gone(0xff00_0000).open);
    }

    #[test]
    fn a_stored_look_takes_a_swapped_head_part_of_its_type_within_the_wire_cap() {
        // Eli's D-E1 A: rotfern's eyes, RotfernEyes 0x014C06 for
        // RotfernEyesMora 0x023F0E (rotfern.esp's)
        let look = r#"{"headParts": [{"formIdentifier": "rotfern.esp|02E117", "type": 0}, {"formIdentifier": "rotfern.esp|014C06", "type": 2}]}"#;
        let out = racemenu_with_head_part(look, &[0, 1], "rotfern.esp", 0x023F0E);
        let parts: Vec<(String, u32)> = racemenu_look_facts(&out).parts.into_iter().map(|p| (p.plugin, p.id)).collect();
        assert_eq!(parts, vec![("rotfern.esp".to_owned(), 0x02E117), ("rotfern.esp".to_owned(), 0x023F0E)]);
        // no part of the type, or a look past the preset cap once written: nothing
        assert!(racemenu_with_head_part(look, &[0, 0], "rotfern.esp", 0x023F0E).is_empty());
        let padding = "x".repeat(wire_schema::cap::RACEMENU_PRESET);
        let big = format!(r#"{{"pad": "{padding}", "headParts": [{{"formIdentifier": "rotfern.esp|014C06", "type": 2}}]}}"#);
        assert!(racemenu_with_head_part(&big, &[1], "rotfern.esp", 0x023F0E).is_empty());
    }

    #[test]
    fn an_own_add_needing_a_new_stack_stops_at_the_wires_stacks_in_one_inventory() {
        // the fresh review of the second head (FIX FIRST 2): the record's
        // bound is the wire's, so every SetInventory and CreateActor of the
        // player still carries its whole inventory
        assert_eq!(player_own::INVENTORY_STACKS, wire_schema::cap::INVENTORY);
        let gold = PlayerOwnFacts { recorded_race: 0x13745, playable: false, restricted: false, permitted: false, held: false, kind: true, race_valid: false, leveled: false, held_count: 40, count: 25.0, into_held_open: false, stacks: wire_schema::cap::INVENTORY, new_stack: true, landing_of: 0, in_reach: false };
        let mut overlays = new_player_overlays();
        let full = overlays.player_own_judge(0xff00_0001, "ObjectReference", "AddItem", 0xf, &[], &gold);
        assert_eq!((full.code, full.name.as_str(), full.count), (4, "E_SCRIPTCALL_FULL", 0));
        assert!(full.why.contains("4096 stacks"), "{}", full.why);
        let joins = overlays.player_own_judge(0xff00_0001, "ObjectReference", "AddItem", 0xf, &[], &PlayerOwnFacts { new_stack: false, ..gold });
        assert_eq!((joins.code, joins.count), (0, 25));
    }

    #[test]
    fn a_leveled_rolls_landing_crosses_as_a_plain_add_of_its_base_and_one_its_list_cannot_roll_is_refused_by_name() {
        // thuum D-R1 C's leveled gap (train 52): MS01's reward
        // LvlQuestReward02Medium 0xc0405, whose roll landed Gold001 0xf x400,
        // judged with the base as the form and the list beside it; IronSword
        // 0x12eb7 is no base the list can roll (D-R2 B)
        let landing = PlayerOwnFacts { recorded_race: 0x13745, playable: false, restricted: false, permitted: false, held: false, kind: true, race_valid: false, leveled: false, held_count: 140, count: 400.0, into_held_open: false, stacks: 12, new_stack: false, landing_of: 0xc0405, in_reach: true };
        let mut overlays = new_player_overlays();
        let recorded = overlays.player_own_judge(0xff00_0001, "ObjectReference", "AddItem", 0xf, &[], &landing);
        assert_eq!((recorded.code, recorded.name.as_str(), recorded.count), (0, "", 400));
        let rolled = overlays.player_own_judge(0xff00_0001, "ObjectReference", "AddItem", 0x12eb7, &[], &PlayerOwnFacts { in_reach: false, count: 1.0, ..landing });
        assert_eq!((rolled.code, rolled.name.as_str(), rolled.count), (4, "E_SCRIPTCALL_ROLL", 0));
        assert_eq!(rolled.why, "12eb7 is no base the leveled list c0405 can roll (its entries, nested lists followed): that landing is not recorded");
        // the call itself with no landing keeps its refusal
        let unreported = overlays.player_own_judge(0xff00_0001, "ObjectReference", "AddItem", 0xc0405, &[], &PlayerOwnFacts { landing_of: 0, in_reach: false, leveled: true, kind: false, count: 1.0, ..landing });
        assert_eq!((unreported.code, unreported.name.as_str()), (4, "E_SCRIPTCALL_LEVELED"));
        assert!(unreported.why.contains("the report carries no landing"), "{}", unreported.why);
    }

    #[test]
    fn the_bridge_takes_a_figure_argument_naming_a_connected_player_alone() {
        // rung 5: a scoped SetRelationshipRank naming c1's figure 0xff000000
        // on c2's seat; a server copy of an NPC 0xff000002 is no figure
        let rank = ScriptArg { kind: 2, form_id: 0 };
        let figure = ScriptArg { kind: 4, form_id: 0xff00_0000 };
        let taken = script_call_figures(&[figure, rank], &[0xff00_0000, 0xff00_0001]);
        assert_eq!((taken.code, taken.name.as_str()), (ScriptCallCode::Accepted, ""));
        let copy = script_call_figures(&[ScriptArg { kind: 4, form_id: 0xff00_0002 }, rank], &[0xff00_0000, 0xff00_0001]);
        assert_eq!((copy.code, copy.name.as_str()), (ScriptCallCode::Args, "E_SCRIPTCALL_ARGS"));
        assert_eq!(script_call_figures(&[ScriptArg { kind: 4, form_id: 0x39655 }, rank], &[]).code, ScriptCallCode::Accepted);
    }

    #[test]
    fn the_bridge_records_a_state_row_with_its_kind_and_sequence() {
        // thuum ADR-068 rung 4: Bleak Falls Temple's cobweb 0x1fc43 disabled
        // by its host 0xff000000 at epoch 3; the Temple's ambush draugr
        // 0x380ba, an actor nobody simulates, enabled by its cell's host
        let flag = ScriptArg { kind: 1, form_id: 0 };
        let at = script_call_subject("ObjectReference", "Disable", 0x1fc43, &[flag]);
        assert!(at.row >= 0);
        assert_eq!(at.subject, 0x1fc43);
        let facts = ScriptCallFacts { sender: 0xff00_0000, self_id: 0x1fc43, epoch: 3, subject_kind: 1, cell: 0x371de, owner: 0xff00_0000, owner_epoch: 3, host: 0, host_epoch: 0, firing: NO_FIRING };
        let mut s = new_script_calls();
        let v = s.judge(at.row, &[flag], &facts, 0);
        assert_eq!((v.code, v.subject, v.seq, v.record, v.keep_evidence, v.keep_last, v.audience), (ScriptCallCode::Accepted, 0x1fc43, 1, 1, false, false, 2));
        assert_eq!((v.relay_cls.as_str(), v.relay_fn.as_str(), v.relay_args.len(), v.event_arg), ("", "", 0, -1));
        // a door's: recorded, never the action rows' quiet drop
        let door = s.judge(at.row, &[flag], &ScriptCallFacts { subject_kind: 2, ..facts }, 0);
        assert_eq!((door.code, door.record, door.seq), (ScriptCallCode::Accepted, 1, 2));
        // a guest's report, by name
        let guest = s.judge(at.row, &[flag], &ScriptCallFacts { sender: 0xff00_0001, ..facts }, 0);
        assert_eq!((guest.code, guest.name.as_str(), guest.record), (ScriptCallCode::Owner, "E_SCRIPTCALL_OWNER", 0));
        // the actor nobody simulates: its cell's host at the epoch
        let enable = script_call_subject("ObjectReference", "Enable", 0x380ba, &[flag]);
        let ambush = ScriptCallFacts { self_id: 0x380ba, subject_kind: 3, cell: 0x371de, owner: 0, owner_epoch: 0, host: 0xff00_0000, host_epoch: 3, ..facts };
        let woke = s.judge(enable.row, &[flag], &ambush, 0);
        assert_eq!((woke.code, woke.subject, woke.seq, woke.record), (ScriptCallCode::Accepted, 0x380ba, 1, 1));
        assert_eq!(s.judge(enable.row, &[flag], &ScriptCallFacts { host_epoch: 4, ..ambush }, 0).name, "E_SCRIPTCALL_OWNER");
        // a standing row (2) and a placement (3)
        let faction = ScriptArg { kind: 4, form_id: 0x5c84e };
        let rank = ScriptArg { kind: 2, form_id: 0 };
        let set_rank = script_call_subject("Actor", "SetFactionRank", 0x800a6, &[faction, rank]);
        let npc = ScriptCallFacts { self_id: 0x800a6, subject_kind: 3, owner: 0xff00_0000, owner_epoch: 0, ..facts };
        assert_eq!(s.judge(set_rank.row, &[faction, rank], &npc, 0).record, 2);
        assert_eq!(s.judge(set_rank.row, &[faction, rank], &ScriptCallFacts { self_id: 0x14, ..npc }, 0).name, "E_SCRIPTCALL_ROW");
        let base = ScriptArg { kind: 4, form_id: 0x4e2b8 };
        let place = script_call_subject("ObjectReference", "PlaceAtMe", 0x1fc43, &[base, rank, flag, flag]);
        assert_eq!(s.judge(place.row, &[base, rank, flag, flag], &facts, 0).record, 3);
        // the record rows' own budget (wire-rules natives' burst, named)
        let mut b = new_script_calls();
        for _ in 0..natives::BURST_RECORD {
            assert_eq!(b.judge(at.row, &[flag], &facts, 0).code, ScriptCallCode::Accepted);
        }
        assert_eq!(b.judge(at.row, &[flag], &facts, 0).name, "E_SCRIPTCALL_RATE");
    }

    #[test]
    fn the_bridge_derives_a_quests_created_items() {
        // thuum ADR-068 rung 4: MS13 0x39645's aliases as Skyrim.esm holds
        // them (lab/esm.py, 2026-11-06), the claw 0x39647 and the journal
        // 0x39654 created in Arvel, the unique actor 0x39646's alias 1
        let row = |id, created, create_at, forced, unique_actor| QuestAliasRow { id, created, create_at, forced, unique_actor };
        let aliases = [row(1, 0, 0, 0, 0x39646), row(11, 0x39647, 0x8000_0001, 0, 0), row(10, 0, 0, 0x1fc43, 0), row(12, 0x39654, 0x8000_0007, 0, 0)];
        let made = alias_creations(&aliases);
        assert_eq!(made.len(), 2);
        assert_eq!(made.first(), Some(&AliasCreationRow { alias: 11, base: 0x39647, target: 1, container_kind: 2, container: 0x39646, why: String::new() }));
        let unresolved = made.get(1).map(|c| (c.container_kind, c.container, c.why.as_str()));
        assert_eq!(unresolved, Some((0, 0, "the quest has no alias of the target's id")));
        let marks = [AliasMarkRow { quest: 0x39645, alias: 11, added: true }];
        assert!(!alias_created_due(&marks, 0x39645, 11));
        assert!(alias_created_due(&marks, 0x39645, 16));
    }

    #[test]
    fn the_bridge_judges_alias_fills_and_world_globals_and_elects_a_streamer() {
        // thuum ADR-068 rung 4: MS13 0x39645's ArvelWeb alias 10 and the web
        // 0x1fc43; an illustrative GLOB 0x1000; two players
        let ok = AliasFillFacts { is_quest: true, has_alias: true, refr_is_ref: true };
        let web = AliasFillEntry { quest: 0x39645, alias: 10, refr: 0x1fc43 };
        assert_eq!(alias_fill_judge(&[], &web, &ok).code, 0);
        let record = [web];
        assert_eq!(alias_fill_judge(&record, &web, &ok).code, 1);
        let refused = alias_fill_judge(&record, &web, &AliasFillFacts { has_alias: false, ..ok });
        assert_eq!((refused.code, refused.name.as_str()), (2, "E_ALIASFILL_NO_ALIAS"));
        // a clear moves the record (the core keeps it as refr 0)
        assert_eq!(alias_fill_judge(&record, &AliasFillEntry { refr: 0, ..web }, &ok).code, 0);

        let facts = |global| WorldGlobalFacts { global, is_glob: true, constant: false, player_listed: false };
        assert_eq!(world_global_entry(&facts(0x1000)), 0);
        assert_eq!(world_global_entry(&facts(0x38)), 1);
        assert_eq!(world_global_entry(&WorldGlobalFacts { player_listed: true, ..facts(0x1000) }), 2);
        assert_eq!(world_global_entry(&WorldGlobalFacts { constant: true, ..facts(0x1000) }), 3);
        assert_eq!(world_global_entry(&WorldGlobalFacts { is_glob: false, ..facts(0x1000) }), 4);
        let names: Vec<String> = (0..=7).map(world_global_code_name).collect();
        assert_eq!(names, ["", "E_WORLDGLOBAL_CLOCK", "E_WORLDGLOBAL_PLAYER", "E_WORLDGLOBAL_CONSTANT", "E_WORLDGLOBAL_NO_GLOB", "E_WORLDGLOBAL_OWNER", "E_WORLDGLOBAL_EPOCH", ""]);
        let merged = world_globals_merge(&[WorldGlobalValue { global: 0x1000, value: 1.0 }], &[WorldGlobalValue { global: 0x1000, value: 7.0 }, WorldGlobalValue { global: 0x2000, value: 0.5 }]);
        assert_eq!(merged.record.len(), 2);
        assert_eq!(merged.changed.len(), 2);

        let mut w = new_world_streamer();
        assert_eq!(w.streamer_now(), StreamerElection { changed: false, streamer: 0, epoch: 0, previous: 0 });
        assert_eq!(w.moved(0xff00_0002), StreamerElection { changed: true, streamer: 0xff00_0002, epoch: 1, previous: 0 });
        assert!(!w.moved(0xff00_0001).changed);
        assert_eq!((w.judge(0xff00_0002, 1), w.judge(0xff00_0001, 1), w.judge(0xff00_0002, 0), w.judge(0xff00_0002, 2)), (0, 3, 1, 2));
        assert_eq!(w.stale(0xff00_0002, true), StreamerElection { changed: true, streamer: 0xff00_0001, epoch: 2, previous: 0xff00_0002 });
        assert_eq!(w.dead(0xff00_0001, true), StreamerElection { changed: true, streamer: 0, epoch: 3, previous: 0xff00_0001 });
        assert!(w.stale(0xff00_0002, false).changed);
        assert!(w.gone(0xff00_0002).changed);
    }

    #[test]
    fn the_bridge_moves_a_ref_state_and_a_standing_and_opens_a_script_set() {
        // thuum ADR-068 rung 4: the five natives against the record
        assert_eq!(ref_state_apply(false, false, "Disable"), RefStateVerdict { code: 0, disabled: true, deleted: false });
        assert_eq!(ref_state_apply(true, false, "DisableNoWait"), RefStateVerdict { code: 1, disabled: true, deleted: false });
        assert_eq!(ref_state_apply(true, false, "EnableNoWait"), RefStateVerdict { code: 0, disabled: false, deleted: false });
        assert_eq!(ref_state_apply(false, false, "Delete"), RefStateVerdict { code: 0, disabled: true, deleted: true });
        assert_eq!(ref_state_apply(true, true, "Enable").code, 1);
        assert_eq!(ref_state_apply(false, false, "PlayAnimation").code, 2);
        // the subject's record kind crosses with the row
        let flag = ScriptArg { kind: 1, form_id: 0 };
        assert_eq!(script_call_subject("ObjectReference", "Disable", 0x1fc43, &[flag]).record, 1);
        let faction = ScriptArg { kind: 4, form_id: 0x5c84e };
        assert_eq!(script_call_subject("Actor", "RemoveFromFaction", 0x800a6, &[faction]).record, 2);
        // the standing: an illustrative faction 0x5c84e on an NPC that holds it at 0
        let held = [StandingFaction { faction: 0x5c84e, rank: 0 }];
        let set = standing_judge("SetFactionRank", 0x5c84e, 2, true, false, &held, 0, &[]);
        assert_eq!((set.code, set.change, set.form, set.rank), (0, 1, 0x5c84e, 2));
        assert_eq!(standing_judge("setfactionrank", 0x5c84e, 0, true, false, &held, 0, &[]).code, 1);
        let refused = standing_judge("SetFactionRank", 0x12eb7, 0, false, false, &held, 0, &[]);
        assert_eq!((refused.code, refused.name.as_str()), (2, "E_STANDING_FACTION"));
        assert_eq!(standing_judge("RemoveFromAllFactions", 0, 0, false, false, &held, 0, &[]).change, 3);
        // a crime faction the record keeps none of, the base's unread: nothing folds
        assert_eq!(standing_judge("SetCrimeFaction", 0, 0, false, false, &[], u32::MAX, &[]).change, 4);
        let other = standing_judge("SetRelationshipRank", 0x1347b, 3, false, true, &[], 0, &[StandingRelation { other: 0x1347b, rank: 1 }]);
        assert_eq!((other.change, other.form, other.rank), (5, 0x1347b, 3));
        assert_eq!(standing_judge("AddToFaction", 0x5c84e, 0, true, false, &[], 0, &[]).code, 3);
        // the script set through the Placements box
        let Ok(mut p) = new_placements("[]") else {
            return;
        };
        let opened = p.open_script_set(0xff00_0000, 0x1fc43, 100.0, 200.0, -50.0, 0x3b, "Skyrim.esm", "STAT", 0);
        assert_eq!(opened, ScriptSetOpen { set: 0, refusal: 0 });
        assert_eq!(p.open_script_set(0xff00_0000, 0x1fc43, 0.0, 0.0, 0.0, 0x3b, "Skyrim.esm", "EXPL", 0).refusal, 1);
        let asks = p.take_script_requests();
        assert_eq!(asks.len(), 1);
        let ask = asks.first().map(|a| (a.set, a.self_id, a.short_id, a.record_type.as_str()));
        assert_eq!(ask, Some((0, 0x1fc43, 0x3b, "STAT")));
        let report = PlacementReport { caster: 0xff00_0000, set: 0, short_id: 0x3b, file: "Skyrim.esm".to_owned(), x: 110.0, y: 200.0, z: -50.0, yaw: 45.0 };
        assert!(p.report_placed(report, 100).accepted);
        let rows = p.due_script_placements(100);
        assert_eq!(rows.len(), 1);
        let placed = rows.first().map(|r| (r.placed, r.x, r.yaw, r.self_id));
        assert_eq!(placed, Some((true, 110.0, 45.0, 0x1fc43)));
    }

    #[test]
    fn the_bridge_runs_the_shape_step_before_the_owner_step() {
        // the hall door's FX review, finding 1: the core asks `shape` before
        // it reads anything of the subject; a call it refuses spends the
        // reporter's token and leaves `owner` nothing to judge, so a flood
        // of malformed calls is cut off by the rate
        let text = ScriptArg { kind: 3, form_id: 0 };
        let at = script_call_subject("ObjectReference", "PlayAnimation", 0xaa8cb, &[text]);
        let facts = ScriptCallFacts { sender: 0xff00_0000, self_id: 0xaa8cb, epoch: 3, subject_kind: 1, cell: 0x371de, owner: 0xff00_0000, owner_epoch: 3, host: 0, host_epoch: 0, firing: NO_FIRING };
        let mut s = new_script_calls();
        // a shape that holds names the subject whose facts the core gathers
        let shaped = s.shape(at.row, &[text], facts.sender, facts.self_id, 0);
        assert_eq!((shaped.code, shaped.subject, shaped.seq, shaped.name.as_str()), (ScriptCallCode::Accepted, 0xaa8cb, 0, ""));
        // the owner step judges it once: a second ask finds no call waiting
        let v = s.owner(&facts);
        assert_eq!((v.code, v.subject, v.seq), (ScriptCallCode::Accepted, 0xaa8cb, 1));
        assert_eq!(s.owner(&facts).code, ScriptCallCode::Skipped);
        // the rest of the burst on malformed calls, each refused by its name
        // at the shape step, none left waiting for the owner step
        let one = [text];
        let malformed: [(i32, &[ScriptArg], &str); 2] = [(-1, &one, "E_SCRIPTCALL_ROW"), (at.row, &[], "E_SCRIPTCALL_ARGS")];
        for (n, (row, args, name)) in (1..natives::BURST).zip(malformed.iter().cycle()) {
            assert_eq!(s.shape(*row, args, facts.sender, facts.self_id, 0).name, *name, "call {n}");
            assert_eq!(s.owner(&facts).code, ScriptCallCode::Skipped, "call {n}");
        }
        // the burst is spent: cut off by the rate at the shape step
        let spent = s.shape(at.row, &[text], facts.sender, facts.self_id, 0);
        assert_eq!((spent.code, spent.name.as_str()), (ScriptCallCode::Rate, "E_SCRIPTCALL_RATE"));
        assert_eq!(s.owner(&facts).code, ScriptCallCode::Skipped);
        // no refusal took a sequence: a token later the next relay is the
        // object's second
        assert_eq!(s.shape(at.row, &[text], facts.sender, facts.self_id, 125).code, ScriptCallCode::Accepted);
        assert_eq!(s.owner(&facts).seq, 2);
        // a later shape step drops a call no owner step took
        assert_eq!(s.shape(at.row, &[text], facts.sender, facts.self_id, 250).code, ScriptCallCode::Accepted);
        assert_eq!(s.shape(-1, &[text], facts.sender, facts.self_id, 375).code, ScriptCallCode::Row);
        assert_eq!(s.owner(&facts).code, ScriptCallCode::Skipped);
    }

    #[test]
    fn the_bridge_judges_a_script_record() {
        // thuum ADR-068 rung 3: Bleak Falls Temple 0x371de hosted by
        // 0xff000000 at epoch 3, a guest 0xff000001 beside it
        let host = ScriptStateFacts { sender: 0xff00_0000, epoch: 3, ref_kind: 1, cell: 0x371de, owner: 0xff00_0000, owner_epoch: 3 };
        let mut s = new_script_states();
        let v = s.judge(&host);
        assert_eq!((v.code, v.name.as_str(), v.why.as_str()), (ScriptStateCode::Accepted, "", ""));
        // the refusals cross with their names and their words
        let guest = s.judge(&ScriptStateFacts { sender: 0xff00_0001, ..host });
        assert_eq!((guest.code, guest.name.as_str(), guest.why.as_str()), (ScriptStateCode::Owner, "E_SCRIPTSTATE_OWNER", "ff000000 hosts cell 371de, not the reporter"));
        let stale = s.judge(&ScriptStateFacts { epoch: 2, ..host });
        assert_eq!((stale.code, stale.name.as_str(), stale.why.as_str()), (ScriptStateCode::Epoch, "E_SCRIPTSTATE_EPOCH", "cell 371de stands at epoch 3, the record names 2"));
        assert_eq!(s.judge(&ScriptStateFacts { ref_kind: 2, ..host }).name, "E_SCRIPTSTATE_KIND");
        assert_eq!(s.judge(&ScriptStateFacts { ref_kind: 0, ..host }).name, "E_SCRIPTSTATE_NO_REF");
        assert_eq!(s.judge(&ScriptStateFacts { ref_kind: 9, ..host }).code, ScriptStateCode::NoRef, "a kind the core never sends reads as no reference");
        // the release read: the host released the temple at its epoch and
        // nobody took it, the kept epoch 3
        let unhosted = ScriptStateFacts { owner: 0, ..host };
        assert_eq!(s.judge(&unhosted).code, ScriptStateCode::Owner);
        s.released(0x371de, 0xff00_0000, 3);
        let read = s.judge(&unhosted);
        assert_eq!((read.code, read.name.as_str()), (ScriptStateCode::ReleaseRead, ""));
        // a grant ends it: the guest's at epoch 4, the old host's read behind
        s.granted(0x371de);
        let regranted = ScriptStateFacts { owner: 0xff00_0001, owner_epoch: 4, ..host };
        assert_eq!(s.judge(&regranted).code, ScriptStateCode::Epoch);
        assert_eq!(s.judge(&ScriptStateFacts { sender: 0xff00_0001, epoch: 4, ..regranted }).code, ScriptStateCode::Accepted);
        // the script calls' sequence: a stored record's seeds it, a max, and
        // the next relay runs on from there
        let text = ScriptArg { kind: 3, form_id: 0 };
        let at = script_call_subject("ObjectReference", "PlayAnimation", 0xaa8cb, &[text]);
        let facts = ScriptCallFacts { sender: 0xff00_0000, self_id: 0xaa8cb, epoch: 3, subject_kind: 1, cell: 0x371de, owner: 0xff00_0000, owner_epoch: 3, host: 0, host_epoch: 0, firing: NO_FIRING };
        let mut calls = new_script_calls();
        assert_eq!(calls.seq_of(0xaa8cb), 0);
        calls.seed(0xaa8cb, 5);
        calls.seed(0xaa8cb, 2);
        assert_eq!(calls.seq_of(0xaa8cb), 5);
        assert_eq!(calls.judge(at.row, &[text], &facts, 0).seq, 6);
        assert_eq!(calls.seq_of(0xaa8cb), 6);
    }

    #[test]
    fn the_bridge_judges_a_trigger_firing_and_an_entry() {
        // thuum ADR-068 D-W1: Bleak Falls Temple 0x371de hosted by
        // 0xff000000; the guest 0xff000001 has it loaded and stands 40 units
        // off the plate 0x567f2 (a stand-in size of 64)
        let guest = TriggerFiringFacts { reporter: 0xff00_0001, cell: 0x371de, host: 0xff00_0000, loader: true, nearest: 40.0, trigger_size: 64.0 };
        let v = trigger_firing_source(&guest);
        assert_eq!((v.code, v.name.as_str(), v.why.as_str()), (TriggerFiringCode::Loader, "", ""));
        assert_eq!(trigger_firing_source(&TriggerFiringFacts { reporter: 0xff00_0000, ..guest }).code, TriggerFiringCode::Host);
        let unloaded = trigger_firing_source(&TriggerFiringFacts { loader: false, ..guest });
        assert_eq!((unloaded.code, unloaded.name.as_str(), unloaded.why.as_str()), (TriggerFiringCode::NotHost, "E_TRAP_NOT_HOST", "which it neither hosts nor has loaded"));
        let far = trigger_firing_source(&TriggerFiringFacts { nearest: 2000.0, ..guest });
        assert_eq!((far.code, far.name.as_str(), far.why.as_str()), (TriggerFiringCode::Far, "E_TRAP_FAR", "its nearest actor 2000 units off the trigger, the bound 1028"));
        // the core's -1, and a value that means nothing, are none measured
        for nearest in [-1.0, f32::NAN, f32::INFINITY] {
            let none = trigger_firing_source(&TriggerFiringFacts { nearest, ..guest });
            assert_eq!((none.code, none.why.as_str()), (TriggerFiringCode::Far, "none of its actors stands where the trigger does"));
        }
        // D-C2: the guest's own player walks into the web 0x3758c (a
        // stand-in size of 100), relayed to the host alone
        let entry = TriggerEntryFacts { reporter: 0xff00_0001, trigger_kind: 1, presser: 0xff00_0001, presser_owner: 0xff00_0001, cell: 0x371de, host: 0xff00_0000, loader: true, distance: 30.0, trigger_size: 100.0, trigger_id: 0x3758c, leave: false };
        let mut entries = new_trigger_entries();
        let relayed = entries.judge(&entry);
        assert_eq!((relayed.code, relayed.name.as_str(), relayed.why.as_str(), relayed.host), (TriggerEntryCode::Relay, "", "", 0xff00_0000));
        // its leave from 1,800 units off, after a door: paired, relayed; a
        // second leave ends nothing
        let leave = TriggerEntryFacts { leave: true, distance: 1800.0, ..entry };
        assert_eq!(entries.judge(&leave).code, TriggerEntryCode::Relay);
        let unpaired = entries.judge(&leave);
        assert_eq!((unpaired.code, unpaired.name.as_str(), unpaired.host), (TriggerEntryCode::Unpaired, "E_ENTRY_UNPAIRED", 0));
        // through a door into another cell, its game with the cell
        // unloaded: still paired, relayed
        assert_eq!(entries.judge(&entry).code, TriggerEntryCode::Relay);
        assert_eq!(entries.judge(&TriggerEntryFacts { loader: false, distance: -1.0, ..leave }).code, TriggerEntryCode::Relay);
        // the presser's disconnect drops its open enters
        assert_eq!(entries.judge(&entry).code, TriggerEntryCode::Relay);
        entries.forget(0xff00_0001);
        assert_eq!(entries.judge(&leave).code, TriggerEntryCode::Unpaired);
        let refused = |f: TriggerEntryFacts| {
            let v = new_trigger_entries().judge(&f);
            (v.code, v.name, v.host)
        };
        assert_eq!(refused(TriggerEntryFacts { trigger_kind: 0, ..entry }), (TriggerEntryCode::NoRef, "E_ENTRY_NO_REF".to_owned(), 0));
        assert_eq!(refused(TriggerEntryFacts { trigger_kind: 7, ..entry }).0, TriggerEntryCode::NoRef, "a kind the core never sends reads as no reference");
        assert_eq!(refused(TriggerEntryFacts { trigger_kind: 2, ..entry }), (TriggerEntryCode::Actor, "E_ENTRY_ACTOR".to_owned(), 0));
        assert_eq!(refused(TriggerEntryFacts { presser_owner: 0xff00_0000, ..entry }), (TriggerEntryCode::Presser, "E_ENTRY_PRESSER".to_owned(), 0));
        assert_eq!(refused(TriggerEntryFacts { loader: false, ..entry }), (TriggerEntryCode::NotLoader, "E_ENTRY_NOT_LOADER".to_owned(), 0));
        let host = TriggerEntryFacts { reporter: 0xff00_0000, presser: 0xff00_0000, presser_owner: 0xff00_0000, ..entry };
        assert_eq!(refused(host), (TriggerEntryCode::Host, "E_ENTRY_HOST".to_owned(), 0));
        assert_eq!(refused(TriggerEntryFacts { host: 0, ..entry }), (TriggerEntryCode::NoHost, "E_ENTRY_NO_HOST".to_owned(), 0));
        assert_eq!(refused(TriggerEntryFacts { distance: 553.0, ..entry }), (TriggerEntryCode::Reach, "E_ENTRY_REACH".to_owned(), 0));
        assert_eq!(refused(TriggerEntryFacts { distance: -1.0, ..entry }), (TriggerEntryCode::Reach, "E_ENTRY_REACH".to_owned(), 0));
    }

    #[test]
    fn the_bridge_judges_a_world_objects_graph_event_by_its_owner() {
        // the fifty fixes' furniture rule: Bleak Falls Temple 0x371de hosted
        // by 0xff000000; the guest 0xff000001 pulls the puzzle lever 0xaa8c3
        let pull = ObjectAnimationFacts { reporter: 0xff00_0001, furniture: true, occupant: 0xff00_0001, occupant_owner: 0xff00_0001, cell: 0x371de, host: 0xff00_0000 };
        let v = object_animation_owner(&pull);
        assert_eq!((v.code, v.name.as_str(), v.why.as_str(), v.keep), (ObjectAnimationCode::User, "", "", false));
        let echo = object_animation_owner(&ObjectAnimationFacts { reporter: 0xff00_0000, ..pull });
        assert_eq!((echo.code, echo.name.as_str(), echo.why.as_str(), echo.keep), (ObjectAnimationCode::NotUser, "E_OBJANIM_NOT_USER", "used by ff000001, whose owner is ff000001", false));
        // the portcullis 0x3b9f1: the host's, kept; a guest's, refused
        let portcullis = ObjectAnimationFacts { reporter: 0xff00_0000, furniture: false, occupant: 0, occupant_owner: 0, cell: 0x371de, host: 0xff00_0000 };
        let kept = object_animation_owner(&portcullis);
        assert_eq!((kept.code, kept.keep), (ObjectAnimationCode::Host, true));
        let guest = object_animation_owner(&ObjectAnimationFacts { reporter: 0xff00_0001, ..portcullis });
        assert_eq!((guest.code, guest.name.as_str(), guest.why.as_str(), guest.keep), (ObjectAnimationCode::NotHost, "E_OBJANIM_NOT_HOST", "which it does not host", false));
        // the lever nobody uses: the host's, kept nowhere
        let unused = object_animation_owner(&ObjectAnimationFacts { reporter: 0xff00_0000, occupant: 0, occupant_owner: 0, ..pull });
        assert_eq!((unused.code, unused.keep), (ObjectAnimationCode::Host, false));
    }

    #[test]
    fn the_bridge_keeps_a_graph_variable_with_its_kind_and_slot() {
        // the fifty fixes' D-C3 A: the host's SetAnimationVariableFloat on
        // the web 0x3758c, relayed as an action and kept by its kind
        let float = ScriptArg { kind: 2, form_id: 0 };
        let text = ScriptArg { kind: 3, form_id: 0 };
        let at = script_call_subject("ObjectReference", "SetAnimationVariableFloat", 0x3758c, &[text, float]);
        let facts = ScriptCallFacts { sender: 0xff00_0000, self_id: 0x3758c, epoch: 1, subject_kind: 1, cell: 0x371de, owner: 0xff00_0000, owner_epoch: 1, host: 0, host_epoch: 0, firing: NO_FIRING };
        let mut calls = new_script_calls();
        let v = calls.judge(at.row, &[text, float], &facts, 0);
        assert_eq!((v.code, v.seq, v.keep_var, v.keep_last, v.relay_fn.as_str()), (ScriptCallCode::Accepted, 1, 4, false, "SetAnimationVariableFloat"));
        let play = script_call_subject("ObjectReference", "PlayAnimation", 0x3758c, &[text]);
        assert_eq!(calls.judge(play.row, &[text], &facts, 0).keep_var, 0);
        // the slot rule: the same name in place, a new one appended, a stale
        // call nowhere, the oldest out of a full record
        let kept = |same_name, seq| GraphVariableKept { same_name, seq };
        assert_eq!(graph_variable_slot(&[], 1), GraphVariableSlot { code: 1, index: 0 });
        assert_eq!(graph_variable_slot(&[kept(false, 1), kept(true, 2)], 3), GraphVariableSlot { code: 0, index: 1 });
        assert_eq!(graph_variable_slot(&[kept(true, 4)], 3), GraphVariableSlot { code: 2, index: 0 });
        let full: Vec<GraphVariableKept> = (1..=16u32).map(|seq| kept(false, seq)).collect();
        assert_eq!(graph_variable_slot(&full, 17), GraphVariableSlot { code: 0, index: 0 });
    }

    #[test]
    fn the_bridge_takes_a_firings_look_from_the_facts_the_core_gathered() {
        // thuum ADR-068 D-W2 A: Embershard's tripwire 0xb6d3f in the host's
        // cell, the guest 0xff000001 with the cell loaded 40 units off it
        let number = ScriptArg { kind: 2, form_id: 0 };
        let flag = ScriptArg { kind: 1, form_id: 0 };
        let motion = script_call_subject("ObjectReference", "SetMotionType", 0xb6d3f, &[number, flag]);
        assert_eq!((motion.subject, motion.look), (0xb6d3f, 2));
        let text = ScriptArg { kind: 3, form_id: 0 };
        assert_eq!(script_call_subject("ObjectReference", "PlayAnimation", 0xb6d3f, &[text]).look, 1);
        assert_eq!(script_call_subject("ObjectReference", "SetOpen", 0xb6d3f, &[flag]).look, 0);
        let firing = TriggerFiringFacts { reporter: 0xff00_0001, cell: 0x371de, host: 0xff00_0000, loader: true, nearest: 40.0, trigger_size: 64.0 };
        let facts = ScriptCallFacts { sender: 0xff00_0001, self_id: 0xb6d3f, epoch: 0, subject_kind: 1, cell: 0x371de, owner: 0xff00_0000, owner_epoch: 3, host: 0, host_epoch: 0, firing };
        let mut calls = new_script_calls();
        let v = calls.judge(motion.row, &[number, flag], &facts, 0);
        assert_eq!((v.code, v.look, v.seq, v.relay_self, v.relay_fn.as_str(), v.relay_args.as_slice()), (ScriptCallCode::Accepted, true, 1, 0xb6d3f, "SetMotionType", &[0u8, 1][..]));
        assert_eq!((v.keep_last, v.keep_var, v.event_arg, v.audience), (false, 0, -1, 0));
        // no firing facts gathered for this call: a game without the cell
        let unmeasured = calls.judge(motion.row, &[number, flag], &ScriptCallFacts { firing: NO_FIRING, ..facts }, 0);
        assert_eq!((unmeasured.code, unmeasured.name.as_str()), (ScriptCallCode::Owner, "E_SCRIPTCALL_OWNER"));
        let other = TriggerFiringFacts { reporter: 0xff00_0002, ..firing };
        assert_eq!(calls.judge(motion.row, &[number, flag], &ScriptCallFacts { firing: other, ..facts }, 0).code, ScriptCallCode::Owner);
        // far, by name; the core's -1 is none measured
        let far = calls.judge(motion.row, &[number, flag], &ScriptCallFacts { firing: TriggerFiringFacts { nearest: 3000.0, ..firing }, ..facts }, 0);
        assert_eq!((far.code, far.name.as_str()), (ScriptCallCode::Far, "E_SCRIPTCALL_FAR"));
        assert_eq!(calls.judge(motion.row, &[number, flag], &ScriptCallFacts { firing: TriggerFiringFacts { nearest: -1.0, ..firing }, ..facts }, 0).code, ScriptCallCode::Far);
        // TriggerSound.Play(self): its self the Sound 0x1d492, relayed on it
        // with no sequence
        let source = ScriptArg { kind: 4, form_id: 0xb6d3f };
        let sound = script_call_subject("Sound", "Play", 0x1d492, &[source]);
        assert_eq!((sound.subject, sound.look), (0xb6d3f, 2));
        let played = calls.judge(sound.row, &[source], &ScriptCallFacts { self_id: 0x1d492, ..facts }, 0);
        assert_eq!((played.code, played.look, played.seq, played.subject, played.relay_self, played.relay_cls.as_str()), (ScriptCallCode::Accepted, true, 0, 0xb6d3f, 0x1d492, "Sound"));
        // the subject's look cap: twelve at once, the first taken above, the
        // sound's among them
        for _ in 0..10 {
            assert_eq!(calls.judge(motion.row, &[number, flag], &facts, 0).code, ScriptCallCode::Accepted);
        }
        let capped = calls.judge(motion.row, &[number, flag], &facts, 0);
        assert_eq!((capped.code, capped.name.as_str()), (ScriptCallCode::LookRate, "E_SCRIPTCALL_LOOK_RATE"));
        // the bone alarm's CreateDetectionEvent: the cell's host alone, the
        // reporter's own player named as its figure
        let owner = ScriptArg { kind: 4, form_id: 0x14 };
        let detect = script_call_subject("ObjectReference", "CreateDetectionEvent", 0x10481e, &[owner, number]);
        assert_eq!((detect.subject, detect.look), (0x10481e, 2));
        let alarm = ScriptCallFacts { self_id: 0x10481e, ..facts };
        let woke = calls.judge(detect.row, &[owner, number], &alarm, 0);
        assert_eq!((woke.code, woke.look, woke.audience, woke.figure_args, woke.relay_self), (ScriptCallCode::Accepted, true, 3, true, 0x10481e));
    }
}
