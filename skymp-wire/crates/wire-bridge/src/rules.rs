//! The game rules for the C++ core (thuum ADR-020): the core gathers the
//! facts from its world model and asks; wire-rules decides. Plain values both
//! ways; the movement budgets and the game clock (ADR-021) live here.

use wire_rules::{activation, actor_values, appearance, attributes, block, bonds, casts, cells, clock, console, damage, effects, favorites, forms, headshots, hosted_state, hostility, leveling, locks, magic, markers, melee, movement, placements, player_globals, props, quest_stages, racemenu, ranged, rest, scripted_effects, shouts, stagger, teleports};

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
    }

    /// One decision of the cell host table, for the core to send to its
    /// game or apply.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    struct HostDecision {
        /// Which decision.
        kind: HostKind,
        /// The player it concerns (0 for Unhosted).
        player: u32,
        /// The cell.
        cell: u32,
        /// The grant's epoch (the cell's kept epoch for Unhosted).
        epoch: u32,
        /// The actor a ClaimEnded concerns (0 for the others).
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
        /// (mode list) or by `{"mode": "all", "denied": [...], "allowed":
        /// [...]}` (mode all; `allowed` the start-game-enabled quests that
        /// cross, memo D62).
        type SharedQuests;
        /// The setting from the block as JSON text (`[]` for none); the
        /// error names the bad entry or shape.
        fn new_shared_quests(settings_json: &str) -> Result<Box<SharedQuests>>;
        /// The sharing mode as the wire carries it: 0 list, 1 all.
        fn shared_quests_mode(self: &SharedQuests) -> u8;
        /// What the login names, in the settings' order (stage 0): the
        /// listed quests in mode list (what a client reports), the denied
        /// quests in mode all (what a client never reports).
        fn shared_quests_listed(self: &SharedQuests) -> Vec<QuestStageEntry>;
        /// Judge a report's quest: 0 the world's, 1 not listed (mode list),
        /// 2 denied (mode all).
        fn shared_quests_check(self: &SharedQuests, short_id: u32, file: &str) -> u8;
        /// Whether mode all's `allowed` names the quest (memo D62): a quest
        /// the game starts by itself that crosses all the same. The flag is
        /// the record's, read on the C++ side beside the check.
        fn shared_quests_allowed(self: &SharedQuests, short_id: u32, file: &str) -> bool;
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
        /// The character experience the step from `level` to the next needs.
        fn leveling_character_threshold(level: u16) -> f32;

        /// An activation of a reference whose lock is `state` by a player who holds its key or not
        /// (thuum docs/verbs/locks.md).
        fn lock_activation(state: &LockStateRow, has_key: bool) -> LockActivationVerdict;
        /// A player's report on a lock (`outcome` 0 picked, 1 broken `broken` picks, 2 left), judged
        /// against the state, the player's reach, the Lockpicks it holds and the gap since its last
        /// report on this lock (u64::MAX for none).
        fn lock_report(state: &LockStateRow, outcome: u8, broken: u8, within_reach: bool, picks_held: u32, since_last_ms: u64) -> LockReportVerdict;
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
        /// `player` releases its claim on `actor`; false when it holds none.
        fn release_claim(self: &mut Cells, actor: u32, player: u32) -> bool;
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
    }
}

pub use ffi::{AvBase, CellHost, CellHostRow, CellFollowRow, CellMove, HostDecision, HostKind, HostedStateFacts, HostedStateVerdict, HostedValueFacts, HostedValueVerdict, PercentHold, AvChange, BondBreak, AvLegendary, AvMerge, AvSkill, AvSnapshot, BedFacts, BlockFacts, BlockSettings, CastCheck, CastClaim, CastFacts, CastHit, CastKind, ConeFacts, ConsoleDecision, DerivedParts, EffectApplied, EffectArrival, EffectEntry, EffectFacts, EffectKind, FavoriteEntry, FavoriteFacts, FavoriteKind, FightPair, FlagFacts, Flags, GameTime, DeathBlast, HeadshotCheck, HeadshotFacts, HostilityFacts, JumpCheck, LookAppearance, LookPartFacts, LookRef, MarkerCandidate, MarkerChoice, MeleeFacts, LevelingCharacter, LevelingEvent, LevelingOutcome, LevelingSkill, LockActivationVerdict, LockReportVerdict, LockStateRow, PlacedCast, PlacementRefusal, PlacementRemoval, PlacementReport, PlacementReportVerdict, PlacementRequestRow, PlacementRow, PlayerGlobalEntry, PropSampleFacts, QuestStageEntry, RaceFacts, Regen, RestFacts, RestRefusal, ShotCheck, ShotClaim, ShotFacts, SneakMults, StaggerFacts, StaggerSettings, TeleportLook, Verdict, VoiceFacts, VoiceRefusal};

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
        self.0.listed().iter().map(|d| QuestStageEntry { short_id: d.short_id, file: d.file.clone(), stage: 0 }).collect()
    }

    fn shared_quests_check(&self, short_id: u32, file: &str) -> u8 {
        match self.0.check(&forms::FormDesc { short_id, file: file.to_owned() }) {
            Ok(()) => 0,
            Err(quest_stages::Refusal::Unlisted) => 1,
            Err(quest_stages::Refusal::Denied) => 2,
        }
    }

    fn shared_quests_allowed(&self, short_id: u32, file: &str) -> bool {
        self.0.is_allowed(short_id, file)
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
        let (accepted, why, events) = match progress.apply_use(&self.0, skill_av, base) {
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

    fn release_claim(&mut self, actor: u32, player: u32) -> bool {
        self.0.release_claim(actor, player)
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

/// The prop owner table (wire-rules props, thuum docs/verbs/prop-physics.md).
#[derive(Debug, Default)]
pub struct Props(props::Props);

fn new_props() -> Box<Props> {
    Box::default()
}

impl Props {
    fn claim_prop(&mut self, refr: u32, player: u32, cell: u32, touch: u8, now_ms: u64) -> u8 {
        match self.0.claim(refr, player, cell, touch, now_ms) {
            props::ClaimVerdict::Granted(_) => 0,
            props::ClaimVerdict::Held => 1,
            props::ClaimVerdict::Same => 2,
        }
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

#[cfg(test)]
mod tests {
    use super::*;

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
        // the host leaves, nobody else has the cell loaded: released, unhosted at the kept epoch
        assert!(c.gone(0xff00_0000, 2_000).is_empty());
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
    fn the_bridge_passes_the_rules_through() {
        assert!(!activation_within_reach(1509.0, 0.0).allowed);
        assert!(race_allowed(&RaceFacts { has_game_files: true, is_recorded_race: false, is_playable_race: true }));
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
        // memo D62: the allowed list beside the denied, for the start-game-enabled
        // quests that cross (MS13 0x39645 is one); a quest in both is refused
        assert!(all.as_ref().is_ok_and(|a| !a.shared_quests_allowed(0x39645, "Skyrim.esm")));
        let allowed = new_shared_quests(r#"{"mode": "all", "denied": ["04e50d:Skyrim.esm"], "allowed": ["039645:Skyrim.esm"]}"#);
        assert!(allowed.as_ref().is_ok_and(|a| a.shared_quests_allowed(0x39645, "Skyrim.esm") && !a.shared_quests_allowed(0x4e50d, "Skyrim.esm")));
        assert!(allowed.as_ref().is_ok_and(|a| a.shared_quests_listed() == vec![e(0x4e50d, 0)] && a.shared_quests_check(0x39645, "Skyrim.esm") == 0));
        assert!(new_shared_quests(r#"{"mode": "all", "denied": ["039645:Skyrim.esm"], "allowed": ["39645:Skyrim.esm"]}"#).is_err());
        assert!(list.as_ref().is_ok_and(|l| !l.shared_quests_allowed(0x74a83, "Skyrim.esm")));
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
        let far = t.report_placed(report(0, 0x98529, 300.0, 0.0, 0.0), 1_300);
        assert!(!far.accepted && far.why == 6 && (far.distance - 300.0).abs() < 0.01, "{far:?}");
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
}
