//! The game rules for the C++ core (thuum ADR-020): the core gathers the
//! facts from its world model and asks; wire-rules decides. Plain values both
//! ways; the movement budgets and the game clock (ADR-021) live here.

use wire_rules::{activation, actor_values, appearance, block, casts, clock, console, damage, effects, favorites, hostility, magic, markers, melee, movement, racemenu, ranged, rest, shouts, stagger, teleports};

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
        /// A user plays the victim.
        target_is_player: bool,
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

    /// One actor value's base (thuum docs/verbs/actor-values.md).
    #[derive(Debug, Clone)]
    struct AvBase {
        /// 0 to 163.
        av: u8,
        /// Its base value.
        base: f32,
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

    /// A spell hit's claim and, when claimed, the factor on the spell's
    /// magnitude (1 for a fire-and-forget hit, seconds for a stream's).
    #[derive(Debug, Clone, Copy)]
    struct CastClaim {
        /// Claimed, or why not.
        check: CastCheck,
        /// The factor on the magnitude.
        scale: f32,
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
        /// Whether an accepted hit is one between two players.
        fn hostility_between_players(facts: &HostilityFacts) -> bool;
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
        /// An actor value's index from its Papyrus name, case aside; -1 for
        /// a name the lab confirmed no index for (thuum docs/verbs/actor-values.md).
        fn actor_value_index(name: &str) -> i16;
        /// Whether the server may set an actor value's base to a value (R0).
        fn actor_values_set_ok(av: u8, value: f32) -> bool;

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
        /// thuum docs/verbs/cast-teleport.md: `actor` cast a teleport at
        /// `now_ms`, standing at (x, y, z) in `cell_or_world`: one jump to
        /// within `reach` of there (0: back there, docs/verbs/ghostwalk.md)
        /// is permitted for `wait_ms`.
        #[allow(clippy::too_many_arguments)]
        fn permit_cast_jump(self: &mut MovementBudgets, actor: u32, cell_or_world: u32, x: f32, y: f32, z: f32, reach: f32, wait_ms: u64, now_ms: u64);
        /// Judge a move of `actor`'s that the bounds refuse against its cast
        /// jump: Landed within reach (that ends it), else NoPermit.
        fn check_cast_jump(self: &mut MovementBudgets, actor: u32, cell_or_world: u32, x: f32, y: f32, z: f32, now_ms: u64) -> JumpCheck;

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

        /// Every player actor's recent spell casts and open streams.
        type SpellCasts;
        /// No casts yet.
        fn new_spell_casts() -> Box<SpellCasts>;
        /// `actor` began a cast at `now_ms` (a monotonic clock).
        fn start(self: &mut SpellCasts, actor: u32, facts: CastFacts, now_ms: u64);
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
    }
}

pub use ffi::{AvBase, AvChange, AvLegendary, AvMerge, AvSkill, AvSnapshot, BedFacts, BlockFacts, BlockSettings, CastCheck, CastClaim, CastFacts, CastHit, CastKind, ConeFacts, ConsoleDecision, DerivedParts, EffectApplied, EffectArrival, EffectEntry, EffectFacts, EffectKind, FavoriteEntry, FavoriteFacts, FavoriteKind, FightPair, FlagFacts, Flags, GameTime, HostilityFacts, JumpCheck, LookAppearance, LookPartFacts, LookRef, MarkerCandidate, MarkerChoice, MeleeFacts, RaceFacts, Regen, RestFacts, RestRefusal, ShotCheck, ShotClaim, ShotFacts, SneakMults, StaggerFacts, StaggerSettings, TeleportLook, Verdict, VoiceFacts, VoiceRefusal};

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

fn hostility_between_players(f: &HostilityFacts) -> bool {
    hostility::between_players(hostility::HitFacts {
        aggressor_is_player: f.aggressor_is_player,
        target_is_player: f.target_is_player,
        same_actor: f.same_actor,
    })
}

fn hostility_apart(distance: f32, interior: bool) -> bool {
    hostility::apart(distance, interior)
}

/// The fights between players going on (wire-rules hostility).
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

fn actor_value_index(name: &str) -> i16 {
    actor_values::index_of(name).map_or(-1, i16::from)
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
}

/// The spells the server names as teleports (wire-rules teleports).
pub struct CastTeleports(teleports::CastTeleports);

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

/// Every player actor's recent spell casts and open streams (wire-rules
/// casts).
#[derive(Debug, Default)]
pub struct SpellCasts(casts::Casts);

// the bridge's reach array is wire-rules' MAX_REACHED long
const _: () = assert!(casts::MAX_REACHED == 8);

fn cast_claim(check: casts::CastCheck) -> CastClaim {
    match check {
        casts::CastCheck::Claimed { scale } => CastClaim { check: CastCheck::Claimed, scale },
        casts::CastCheck::NoCast => CastClaim { check: CastCheck::NoCast, scale: 0.0 },
        casts::CastCheck::TooFar => CastClaim { check: CastCheck::TooFar, scale: 0.0 },
        casts::CastCheck::Throttled => CastClaim { check: CastCheck::Throttled, scale: 0.0 },
    }
}

fn new_spell_casts() -> Box<SpellCasts> {
    Box::default()
}

impl SpellCasts {
    fn start(&mut self, actor: u32, facts: CastFacts, now_ms: u64) {
        let CastFacts { spell, hand, kind, area, reach, x, y, z, reached, linger_ms, reached_concentration } = facts;
        let kind = match kind {
            CastKind::Concentration => casts::CastKind::Concentration,
            _ => casts::CastKind::FireAndForget,
        };
        self.0.start(actor, casts::CastFacts { spell, hand, kind, area, reach, x, y, z, reached, linger_ms, reached_concentration }, now_ms);
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
        };
        assert!((sneak_mult(1, &skyrim) - 3.0).abs() < 1e-6);
        assert!((sneak_mult(7, &skyrim) - 1.3).abs() < 1e-6);
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
        // a cast teleport's jump: within its reach of the cast, once
        b.permit_cast_jump(7, 0x3c, 0.0, -300.0, 12.0, 3_000.0, 5_000, 20);
        assert_eq!(b.check_cast_jump(7, 0x3c, 0.0, 4_000.0, 12.0, 21), JumpCheck::NoPermit);
        assert_eq!(b.check_cast_jump(7, 0x3c, 0.0, 2_000.0, 12.0, 22), JumpCheck::Landed);
        assert_eq!(b.check_cast_jump(7, 0x3c, 0.0, 2_000.0, 12.0, 23), JumpCheck::NoPermit);
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
            CastFacts { spell: 0xa101, hand: 1, kind: CastKind::FireAndForget, area: false, reach: 512.0, x: 0.0, y: 0.0, z: 0.0, reached, linger_ms: 60_000, reached_concentration: 0 };
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
        let players = HostilityFacts { aggressor_is_player: true, target_is_player: true, same_actor: false };
        assert!(hostility_between_players(&players));
        assert!(!hostility_between_players(&HostilityFacts { same_actor: true, ..players }));
        assert!(hostility_apart(hostility::EXTERIOR_RANGE + 1.0, false));
        assert!(!hostility_apart(hostility::INTERIOR_RANGE, true));
        let mut f = new_fights();
        assert!(f.hit(1, 2, 0));
        assert!(!f.hit(2, 1, 1_000));
        assert_eq!(f.pairs().iter().map(|p| (p.a, p.b)).collect::<Vec<_>>(), vec![(1, 2)]);
        assert!(f.in_fight(2));
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
