//! Structural validation. This crate knows the shape of a legal message and
//! the rates a client may send them at. It does not know who owns what; that
//! is semantic validation and lives in the server behind the bridge.
//!
//! Every rejection has a reason code. The lab greps them. The validator is
//! pure: time comes in as `now_ms` from the caller's clock, and nothing here
//! can panic on any input (checked arithmetic throughout, fuzzed by
//! `fuzz/validate`).
//!
//! The SkyMP family (ADR-019) gets the checks every message shares: every
//! float finite, wherever it sits, and positions inside the world. It gets no
//! new rate rules: the port keeps SkyMP's behavior, and the transport's byte
//! budget per client already bounds what any client can push; per-verb rules
//! come with the verbs.

use wire_schema::finite::all_finite;
use wire_schema::{skymp, ItemDelta, Message, Transform};

/// Rejection reasons. Stable names; append only.
#[derive(Debug, thiserror::Error, PartialEq, Eq, Clone, Copy)]
pub enum Reject {
    /// `Hello.schema_version` differs from [`wire_schema::SCHEMA_VERSION`].
    #[error("E_VAL_SCHEMA_VERSION")]
    SchemaVersion,
    /// A float field is NaN or infinite.
    #[error("E_VAL_NONFINITE")]
    NonFinite,
    /// A position component lies outside [`WORLD_ABS_MAX`].
    #[error("E_VAL_OUT_OF_WORLD")]
    OutOfWorld,
    /// The client's budget for this message family is spent.
    #[error("E_VAL_RATE")]
    Rate,
    /// An inventory delta with a zero count.
    #[error("E_VAL_ZERO_DELTA")]
    ZeroDelta,
    /// A sequence number already accepted, or too old to be checked.
    #[error("E_VAL_SEQ_REPLAY")]
    SeqReplay,
    /// A field outside its legal range (a calendar field, a negative rate).
    #[error("E_VAL_RANGE")]
    Range,
    /// A change form names a form id of 0 or a player's (0xff000000 and up),
    /// or a save-type index past the header byte's six bits (thuum
    /// docs/verbs/change-form.md).
    #[error("E_CHANGEFORM_RANGE")]
    ChangeFormRange,
    /// A change form's header disagrees with its body: a character that is
    /// not lowercase hex, a size code the pack never writes, the type byte
    /// against `formType`, a version other than 0x4E, the lengths against the
    /// body's size.
    #[error("E_CHANGEFORM_HEADER")]
    ChangeFormHeader,
    /// A change form's FNV-1a 32 over the body differs from `fnv1a`.
    #[error("E_CHANGEFORM_HASH")]
    ChangeFormHash,
    /// A change form's remap row lies outside the body's data, does not
    /// decode to kind 0 with its own index, names a form no row may remap
    /// (0, a load-index-00 form, a created one), or repeats an offset.
    #[error("E_CHANGEFORM_REMAP")]
    ChangeFormRemap,
    /// A change form's body is too short for any header, or a collection
    /// stands past its cap.
    #[error("E_CHANGEFORM_SIZE")]
    ChangeFormSize,
}

/// World bounds in engine units. Tamriel's worldspace fits comfortably; a
/// value outside this is a bug or a lie either way.
pub const WORLD_ABS_MAX: f32 = 1.0e6;

/// Per-client state the validator needs: sequence tracking and token buckets.
/// Owned by the transport layer, one per connection.
#[derive(Debug, Clone)]
pub struct ClientGuard {
    /// Highest movement `seq` accepted; meaningful once `movement_primed`.
    pub last_movement_seq: u32,
    /// False until the first movement sample is accepted, so that a `seq` of
    /// zero is an ordinary value and not a hole in replay protection.
    pub movement_primed: bool,
    /// Budget for movement samples.
    pub movement_budget: TokenBucket,
    /// Budget for hit intents.
    pub hit_budget: TokenBucket,
    /// Replay window for hit `seq`, which may legally arrive out of order.
    pub hit_window: ReplayWindow,
    /// Budget for rests: the Sleep/Wait menu takes seconds per rest, so two
    /// at once and one a second after that only ever stops a flood.
    pub rest_budget: TokenBucket,
    /// Budget for map marker discoveries: a player finds a location now and
    /// then, a few at once at most, so four at once and one a second after
    /// that only ever stops a flood.
    pub marker_budget: TokenBucket,
    /// Budget for ingredient effect reports: one per ingredient eaten, and a
    /// player can eat a few in a row from the menu, so eight at once and two
    /// a second after that only ever stops a flood.
    pub effects_budget: TokenBucket,
    /// Budget for favorites reports: one per closing of the inventory, magic
    /// or favorites menu that changed them, so four at once and one a second
    /// after that only ever stops a flood.
    pub favorites_budget: TokenBucket,
    /// Budget for RaceMenu presets: one per closing of the race menu that
    /// changed the look, each up to 192 KiB, so two at once and one a second
    /// after that only ever stops a flood (the transport's byte budget
    /// bounds the bytes).
    pub preset_budget: TokenBucket,
    /// Budget for actor value snapshots: one per skill or level increase,
    /// which come a few at once (a level-up after a skill-up), so eight at
    /// once and two a second after that only ever stops a flood.
    pub actor_values_budget: TokenBucket,
    /// Budget for console commands (thuum docs/verbs/console-commands.md):
    /// a player types them, a few at once at most (a batch file runs
    /// several), so four at once and one a second after that only ever
    /// stops a flood. The name, the caller's rank and the arguments are the
    /// server's decision (wire-rules `console`), which answers every command
    /// it gets with a line.
    pub console_budget: TokenBucket,
    /// Budget for bow and crossbow shots (thuum docs/verbs/marksman.md): a
    /// bow is drawn for most of a second before each arrow, so four at once
    /// and two a second after that only ever stops a flood; the server's
    /// ranged rule matches each hit to one shot.
    pub shot_budget: TokenBucket,
    /// Budget for SkyMP's hit reports (OnHit), the player's own and its
    /// hosted actors': a sweep strikes several targets at once and a host
    /// reports for every actor it runs, so thirty-two at once and sixteen a
    /// second after that only ever stops a flood; each hit's rules are the
    /// server's.
    pub on_hit_budget: TokenBucket,
    /// Budget for a host's death reports (ActorDeath, thuum docs/verbs/
    /// npc-fate.md, ADR-048's B): a fight's end can fell two or three at
    /// once, so six at once and one a second after that only ever stops a
    /// flood; which deaths are accepted is the server's rule.
    pub actor_death_budget: TokenBucket,
    /// Budget for a host's state reports (HostedActorState, thuum docs/verbs/
    /// npc-stream.md, ADR-050): one per hosted actor per half second on a
    /// change, a village's fourteen residents at once after a grant.
    /// UNMEASURED: four times the rate a host of Riverwood would send at
    /// every change (28 a second), to be replaced by P3's numbers.
    pub hosted_state_budget: TokenBucket,
    /// Budget for a host's spoken lines (ActorSpeech, thuum docs/verbs/
    /// npc-stream.md): a village's residents greet and chatter a few lines a
    /// minute each. UNMEASURED: 32 at once, 8 a second, until P7 reads a
    /// host's rate.
    pub speech_budget: TokenBucket,
    /// ActorClaim (docs/verbs/npc-talk.md): a dialogue's claim and release; 8
    /// at once, 2 a second after (UNMEASURED).
    pub claim_budget: TokenBucket,
    /// Budget for spell casts and their ends (thuum docs/verbs/spell-cast.md):
    /// a cast charges before it goes (Firebolt's SPIT charge time is half a
    /// second) and a stream's start and end are two messages, from two hands
    /// at once, so eight at once and four a second after that only ever
    /// stops a flood; the server's casts rule matches each spell hit to a
    /// cast.
    pub cast_budget: TokenBucket,
    /// Budget for equip reports (OnEquip): a potion's drink, a book's read
    /// and every item a player puts on go through it (thuum
    /// docs/verbs/magic-effects.md), and a player's hotkeys or an outfit
    /// can send a row at once, so sixteen at once and eight a second after
    /// that only ever stops a flood.
    pub equip_budget: TokenBucket,
    /// Budget for the actions a player's actor performs (thuum
    /// docs/verbs/cast-action.md): a cast is three (the hand's attack and
    /// release in one tick for a stream, attack, ready and release for a
    /// bolt, the interrupt at the end), from two hands at once, so twelve
    /// at once and six a second after that only ever stops a flood; the
    /// figures that replay them play what arrives.
    pub action_budget: TokenBucket,
    /// Budget for player globals reports (thuum docs/verbs/player-globals.md):
    /// the client reports the watched globals that changed once a second at
    /// most, so two at once and one a second after that only ever stops a
    /// flood.
    pub player_globals_budget: TokenBucket,
    /// Budget for quest stage reports (thuum docs/verbs/quest-stage.md): a
    /// dialogue's fragment sets a stage or two, a quest's script chain a few
    /// within a second, and in sharing mode all (memo D58 B) the seat
    /// reports every stage event its engine fires, a load's dozen quest
    /// starts among them, so thirty-two at once and eight a second after
    /// that only ever stops a flood. UNMEASURED: raised from sixteen and
    /// four with the mode, no lab count yet.
    pub quest_stage_budget: TokenBucket,
    /// Budget for CellLoaded reports (thuum docs/verbs/cell-host.md): a
    /// 5x5 grid load is 25 cells at once, then a row or column per grid
    /// shift; wire-rules cells::CELL_LOADED_BURST and CELL_LOADED_PER_SECOND,
    /// measured in x-cell-attach and carried with their conditions.
    pub cell_loaded_budget: TokenBucket,
    /// Budget for a simulator's prop samples (PropTransform, thuum docs/verbs/
    /// prop-physics.md, ADR-058): one per awake prop per 100 ms and one at
    /// its sleep, a kicked shelf's worth of bodies at once, and since D-S3
    /// (traps follow the interaction, point 4) a trap's bodies on the same
    /// stream: one firing streams eight (Embershard's tripwire 0xb6d3f, which
    /// its own script sets Dynamic, and its seven falling rocks), eighty
    /// samples a second for the length of the fall. UNMEASURED: 128 at once
    /// and 96 a second after that (20 and 10 before the trap bodies), bodies
    /// and props on one budget, until x-trap-boulders reads a seat's rate.
    pub prop_budget: TokenBucket,
    /// Budget for a seat's prop claims (PropClaim, the same verb): a touch, a
    /// grab, a drop or an impulse each a claim; the claim idiom's 8 at once,
    /// 2 a second after (UNMEASURED).
    pub prop_claim_budget: TokenBucket,
    /// Budget for a caster's placement reports (PlacedReport, thuum docs/verbs/
    /// spell-placed-object.md, shape A): one per base per set, Raise Wall's
    /// two a cast, two sets a caster; a burst of 8 then 2 a second
    /// (UNMEASURED).
    pub placed_report_budget: TokenBucket,
    /// Budget for a player's skill use batches (SkillUse, thuum docs/verbs/
    /// leveling.md): one a second by design; a burst of 8 then 2 a second
    /// (UNMEASURED).
    pub skill_use_budget: TokenBucket,
    /// Budget for a player's lock reports (LockReport, thuum docs/verbs/
    /// locks.md): one per attempt; a burst of 8 then 2 a second (UNMEASURED).
    pub lock_report_budget: TokenBucket,
    /// Budget for a player's perk picks (PerkPick, thuum docs/verbs/perks.md):
    /// one per pick; a burst of 8 then 2 a second (UNMEASURED).
    pub perk_pick_budget: TokenBucket,
    /// Budget for a player's word wall reports (WordWall, thuum
    /// docs/verbs/word-wall.md): one per wall; a burst of 2 then 1 a second
    /// (UNMEASURED).
    pub word_wall_budget: TokenBucket,
    /// Budget for a host's world-object animation reports (ObjectAnimation,
    /// thuum docs/verbs/traps.md, ADR-059 decision 3): a pillar's turn is two
    /// events, a plate's press a wall's swing and reset; a burst of 32 then 8
    /// a second (UNMEASURED).
    pub object_animation_budget: TokenBucket,
    /// Budget for a host's change-form reports (ChangeForm, thuum
    /// docs/verbs/change-form.md, ADR-065): one per changed actor at the
    /// frame's end, only when the body's hash moved; a grant into a village
    /// marks its residents at once. UNMEASURED: 32 at once, 8 a second,
    /// until a lab run reads a host's rate; the transport's byte budget
    /// bounds the bytes.
    pub change_form_budget: TokenBucket,
    /// Budget for a seat's destruction reports (DestructionStage, thuum
    /// D-S3): one per stage rise or destruction its engine caused or saw
    /// first, a trap's result once; 8 at once, 2 a second after (UNMEASURED).
    pub destruction_budget: TokenBucket,
    /// Budget for an owner's script records (ScriptState, thuum ADR-068 rung
    /// 3): one per reference whose record's hash moved, at the end of a
    /// stack that touched it or at the 30 s sweep, and the cell's known
    /// references at once at a release; 64 at once, 8 a second after
    /// (UNMEASURED). The transport's byte budget bounds the bytes.
    pub script_state_budget: TokenBucket,
    /// Budget for a seat's alias fills (AliasFill, thuum ADR-068 rung 4):
    /// every seat runs a quest's scripts, so each reports its ForceRefTo and
    /// Clear; 16 at once, 4 a second after (UNMEASURED: 57 ForceRefTo were
    /// counted at one guest's arrival, thuum docs/DECISIONS.md ADR-068's
    /// measurements).
    pub alias_fill_budget: TokenBucket,
    /// Budget for the world streamer's reports (WorldGlobal, thuum ADR-068
    /// rung 4): one a second by design, its entries batched; 4 at once, 1 a
    /// second after (UNMEASURED).
    pub world_global_budget: TokenBucket,
    /// Budget for a seat's trigger entries (an Activate with `entry`, thuum
    /// D-C2): one per entry and leave of its own presser into a world
    /// trigger in a cell it does not host; 8 at once, 4 a second after
    /// (UNMEASURED). An Activate without `entry` takes none.
    pub entry_budget: TokenBucket,
    /// Budget for a seat's condition-read variable reports (ScriptVariables,
    /// thuum ADR-068 rung 5): one a second by design, its changed keys
    /// batched; 4 at once, 1 a second after (WorldGlobal's shape,
    /// UNMEASURED).
    pub script_variables_budget: TokenBucket,
}

impl Default for ClientGuard {
    fn default() -> Self {
        Self {
            last_movement_seq: 0,
            movement_primed: false,
            movement_budget: TokenBucket::default(),
            hit_budget: TokenBucket::default(),
            hit_window: ReplayWindow::default(),
            rest_budget: TokenBucket { tokens: 2, capacity: 2, refill_per_s: 1, last_refill_ms: 0 },
            marker_budget: TokenBucket { tokens: 4, capacity: 4, refill_per_s: 1, last_refill_ms: 0 },
            effects_budget: TokenBucket { tokens: 8, capacity: 8, refill_per_s: 2, last_refill_ms: 0 },
            favorites_budget: TokenBucket { tokens: 4, capacity: 4, refill_per_s: 1, last_refill_ms: 0 },
            preset_budget: TokenBucket { tokens: 2, capacity: 2, refill_per_s: 1, last_refill_ms: 0 },
            actor_values_budget: TokenBucket { tokens: 8, capacity: 8, refill_per_s: 2, last_refill_ms: 0 },
            console_budget: TokenBucket { tokens: 4, capacity: 4, refill_per_s: 1, last_refill_ms: 0 },
            shot_budget: TokenBucket { tokens: 4, capacity: 4, refill_per_s: 2, last_refill_ms: 0 },
            on_hit_budget: TokenBucket { tokens: 32, capacity: 32, refill_per_s: 16, last_refill_ms: 0 },
            actor_death_budget: TokenBucket { tokens: 6, capacity: 6, refill_per_s: 1, last_refill_ms: 0 },
            hosted_state_budget: TokenBucket { tokens: 128, capacity: 128, refill_per_s: 32, last_refill_ms: 0 },
            speech_budget: TokenBucket { tokens: 32, capacity: 32, refill_per_s: 8, last_refill_ms: 0 },
            claim_budget: TokenBucket { tokens: 8, capacity: 8, refill_per_s: 2, last_refill_ms: 0 },
            cast_budget: TokenBucket { tokens: 8, capacity: 8, refill_per_s: 4, last_refill_ms: 0 },
            player_globals_budget: TokenBucket { tokens: 2, capacity: 2, refill_per_s: 1, last_refill_ms: 0 },
            quest_stage_budget: TokenBucket { tokens: 32, capacity: 32, refill_per_s: 8, last_refill_ms: 0 },
            equip_budget: TokenBucket { tokens: 16, capacity: 16, refill_per_s: 8, last_refill_ms: 0 },
            // thuum docs/verbs/npc-stream.md (ADR-050): the actions a host
            // streams for every actor it simulates ride this budget with the
            // player's own; UNMEASURED at four times the rate P1 read for
            // Riverwood (490 taken actions in 122 s for seventeen NPCs before
            // the movement filter, x-npc-stream-p1 20261026-101422), a fight's
            // burst on top; P3 replaces it
            action_budget: TokenBucket { tokens: 48, capacity: 48, refill_per_s: 16, last_refill_ms: 0 },
            cell_loaded_budget: TokenBucket { tokens: 128, capacity: 128, refill_per_s: 32, last_refill_ms: 0 },
            prop_budget: TokenBucket { tokens: 128, capacity: 128, refill_per_s: 96, last_refill_ms: 0 },
            prop_claim_budget: TokenBucket { tokens: 8, capacity: 8, refill_per_s: 2, last_refill_ms: 0 },
            placed_report_budget: TokenBucket { tokens: 8, capacity: 8, refill_per_s: 2, last_refill_ms: 0 },
            skill_use_budget: TokenBucket { tokens: 8, capacity: 8, refill_per_s: 2, last_refill_ms: 0 },
            lock_report_budget: TokenBucket { tokens: 8, capacity: 8, refill_per_s: 2, last_refill_ms: 0 },
            perk_pick_budget: TokenBucket { tokens: 8, capacity: 8, refill_per_s: 2, last_refill_ms: 0 },
            word_wall_budget: TokenBucket { tokens: 2, capacity: 2, refill_per_s: 1, last_refill_ms: 0 },
            object_animation_budget: TokenBucket { tokens: 32, capacity: 32, refill_per_s: 8, last_refill_ms: 0 },
            change_form_budget: TokenBucket { tokens: 32, capacity: 32, refill_per_s: 8, last_refill_ms: 0 },
            destruction_budget: TokenBucket { tokens: 8, capacity: 8, refill_per_s: 2, last_refill_ms: 0 },
            script_state_budget: TokenBucket { tokens: 64, capacity: 64, refill_per_s: 8, last_refill_ms: 0 },
            alias_fill_budget: TokenBucket { tokens: 16, capacity: 16, refill_per_s: 4, last_refill_ms: 0 },
            world_global_budget: TokenBucket { tokens: 4, capacity: 4, refill_per_s: 1, last_refill_ms: 0 },
            entry_budget: TokenBucket { tokens: 8, capacity: 8, refill_per_s: 4, last_refill_ms: 0 },
            script_variables_budget: TokenBucket { tokens: 4, capacity: 4, refill_per_s: 1, last_refill_ms: 0 },
        }
    }
}

/// Minimal token bucket with checked arithmetic. Refill is driven by the
/// caller's clock so the validator stays pure.
#[derive(Debug, Clone)]
pub struct TokenBucket {
    /// Tokens available now.
    pub tokens: u32,
    /// Ceiling for `tokens`.
    pub capacity: u32,
    /// Tokens added per second of caller time.
    pub refill_per_s: u32,
    /// Caller time up to which refill has been credited.
    pub last_refill_ms: u64,
}

impl Default for TokenBucket {
    fn default() -> Self {
        Self {
            tokens: 60,
            capacity: 60,
            refill_per_s: 60,
            last_refill_ms: 0,
        }
    }
}

impl TokenBucket {
    /// Credit refill for elapsed time, then try to take one token.
    pub fn take(&mut self, now_ms: u64) -> bool {
        let elapsed_ms = now_ms.saturating_sub(self.last_refill_ms);
        let per_s = u64::from(self.refill_per_s);
        let refill_u64 = elapsed_ms
            .saturating_mul(per_s)
            .checked_div(1000)
            .unwrap_or(0);
        let refill = u32::try_from(refill_u64).unwrap_or(u32::MAX);
        if refill > 0 {
            self.tokens = self.tokens.saturating_add(refill).min(self.capacity);
            // Credit exactly the time those tokens took, so fractions carry.
            let credited_ms = refill_u64
                .saturating_mul(1000)
                .checked_div(per_s)
                .unwrap_or(0);
            self.last_refill_ms = self.last_refill_ms.saturating_add(credited_ms);
        }
        match self.tokens.checked_sub(1) {
            Some(t) => {
                self.tokens = t;
                true
            }
            None => false,
        }
    }
}

/// Anti-replay window for sequence numbers that may legally arrive out of
/// order (reliable-unordered channels): a 64-bit bitmap over the sequence
/// numbers just below the highest accepted, the scheme of RFC 4303 section
/// 3.4.3. Numbers older than the window are rejected as replays.
#[derive(Debug, Default, Clone, Copy)]
pub struct ReplayWindow {
    top: u32,
    bits: u64,
    primed: bool,
}

impl ReplayWindow {
    /// Sequence numbers the window remembers below `top`.
    pub const WIDTH: u32 = 64;

    /// Accept `seq` if it has not been seen; false if it was, or if it is too
    /// old to tell.
    pub fn accept(&mut self, seq: u32) -> bool {
        if !self.primed {
            self.primed = true;
            self.top = seq;
            self.bits = 1;
            return true;
        }
        if seq > self.top {
            let shift = seq.saturating_sub(self.top);
            self.bits = self.bits.checked_shl(shift).unwrap_or(0) | 1;
            self.top = seq;
            return true;
        }
        let back = self.top.saturating_sub(seq);
        if back >= Self::WIDTH {
            return false;
        }
        let mask = 1u64.checked_shl(back).unwrap_or(0);
        if self.bits & mask != 0 {
            return false;
        }
        self.bits |= mask;
        true
    }
}

fn check_pos(pos: &skymp::Vec3) -> Result<(), Reject> {
    if pos.iter().any(|v| v.abs() > WORLD_ABS_MAX) {
        return Err(Reject::OutOfWorld);
    }
    Ok(())
}

fn check_transform(t: &Transform) -> Result<(), Reject> {
    let vals = [t.x, t.y, t.z, t.yaw, t.pitch];
    if vals.iter().any(|v| !v.is_finite()) {
        return Err(Reject::NonFinite);
    }
    if [t.x, t.y, t.z].iter().any(|v| v.abs() > WORLD_ABS_MAX) {
        return Err(Reject::OutOfWorld);
    }
    Ok(())
}

/// The engine's calendar: twelve months from 0, days from 1 (31 at most),
/// an hour below 24, and no negative day count or rate.
fn check_game_time(m: &skymp::SetGameTime) -> Result<(), Reject> {
    let in_range = m.month <= 11
        && (1..=31).contains(&m.day)
        && (0.0..24.0).contains(&m.hour)
        && m.days_passed >= 0.0
        && m.time_scale >= 0.0;
    if in_range {
        Ok(())
    } else {
        Err(Reject::Range)
    }
}

/// A bow shot's draw power (0 to 1) and aim pitch (a quarter turn either
/// way, radians), thuum docs/verbs/marksman.md, and its launch
/// ([`check_launch`]).
fn check_bow_shot(m: &skymp::PlayerBowShot) -> Result<(), Reject> {
    // the shooter's idx (thuum docs/verbs/npc-stream.md, train 53): u32::MAX
    // is no actor; whether the sender may shoot for that idx is the server's
    if m.idx == u32::MAX {
        return Err(Reject::Range);
    }
    if !((0.0..=1.0).contains(&m.power) && (-core::f32::consts::FRAC_PI_2..=core::f32::consts::FRAC_PI_2).contains(&m.aim_angle)) {
        return Err(Reject::Range);
    }
    check_launch(m.has_launch, &[m.origin_x, m.origin_y, m.origin_z], m.launch_angle_x, m.launch_angle_z)
}

/// A launch as the shooter's or the caster's engine made it, which
/// PlayerBowShot and its relay ArrowShot carry alike (thuum
/// docs/verbs/marksman.md: the arrow's path identical on both screens), and
/// SpellCast both ways (docs/verbs/spell-cast.md: a target-location spell
/// lands where its caster aimed): when carried, a pitch within a quarter
/// turn either way and an origin inside the world; when not, origin and
/// angles all zero, so no message holds a launch it says it lacks. The
/// heading is any finite angle, as the aim's is.
fn check_launch(has_launch: bool, origin: &skymp::Vec3, angle_x: f32, angle_z: f32) -> Result<(), Reject> {
    if has_launch {
        if !(-core::f32::consts::FRAC_PI_2..=core::f32::consts::FRAC_PI_2).contains(&angle_x) {
            return Err(Reject::Range);
        }
        check_pos(origin)
    } else if origin.iter().chain(&[angle_x, angle_z]).all(|v| *v == 0.0) {
        Ok(())
    } else {
        Err(Reject::Range)
    }
}

/// thuum docs/verbs/cast-action.md: an action names its BGSAction and the
/// graph event the engine sent for it (the replay entry performs the event
/// as given, so an empty one would send an empty event), and its type fits
/// the byte the engine's ActionInput reads it as (every action a player
/// performs carries 2). The variables are packed bytes, bounded at decode.
fn check_update_action(m: &skymp::UpdateAction) -> Result<(), Reject> {
    if m.action != 0 && !m.event_name.as_str().is_empty() && m.kind <= 0xFF {
        Ok(())
    } else {
        Err(Reject::Range)
    }
}

/// thuum docs/verbs/spell-cast.md: a cast names a spell, comes from a hand
/// or slot SkyMP knows (0 left, 1 right, 2 voice, 3 instant), and aims within
/// a quarter turn of level, as a bow shot does, and carries its launch as a
/// bow shot does ([`check_launch`]).
fn check_spell_cast(m: &skymp::SpellCast) -> Result<(), Reject> {
    let d = &m.data;
    if !(d.spell != 0
        && (0..=3).contains(&d.casting_source)
        && (-core::f32::consts::FRAC_PI_2..=core::f32::consts::FRAC_PI_2).contains(&d.aim_angle))
    {
        return Err(Reject::Range);
    }
    check_launch(d.has_launch, &[d.origin_x, d.origin_y, d.origin_z], d.launch_angle_x, d.launch_angle_z)?;
    check_decal(d.has_launch, d.decal.as_ref())
}

/// A cast's decal (docs/verbs/spell-cast.md, the rune's glyph): only with
/// the launch it belongs to, its normal finite and of unit length within
/// [`DECAL_NORMAL_TOLERANCE`]; the material id is any number (0 for none).
pub const DECAL_NORMAL_TOLERANCE: f32 = 0.05;

fn check_decal(has_launch: bool, decal: Option<&skymp::SpellDecal>) -> Result<(), Reject> {
    let Some(d) = decal else {
        return Ok(());
    };
    if !has_launch {
        return Err(Reject::Range);
    }
    let parts = [d.normal_x, d.normal_y, d.normal_z];
    if parts.iter().any(|v| !v.is_finite()) {
        return Err(Reject::NonFinite);
    }
    let length = parts.iter().map(|v| v * v).sum::<f32>().sqrt();
    if (length - 1.0).abs() > DECAL_NORMAL_TOLERANCE {
        return Err(Reject::Range);
    }
    Ok(())
}

/// A rest's hours: the Sleep/Wait menu's range, one hour to a day
/// (docs/verbs/rest.md; UESP, Skyrim:Health, "the hour minimum").
fn check_rest(m: &skymp::RestIntent) -> Result<(), Reject> {
    if (1.0..=24.0).contains(&m.hours) {
        Ok(())
    } else {
        Err(Reject::Range)
    }
}

/// The last location type a discovery can carry: the engine's MARKER_TYPE
/// runs from kNone (0) to kDLC02CastleKarstaag (59); kTotalLocationTypes and
/// the door, quest target, player-set and you-are-here markers after it are
/// never discovered (CommonLibSSE-NG include/RE/E/ExtraMapMarker.h:9-79).
pub const MARKER_TYPE_MAX: u16 = 59;

/// A discovery's marker type: a location's (docs/verbs/map-markers.md).
fn check_marker(m: &skymp::MapMarkerDiscovered) -> Result<(), Reject> {
    if m.marker_type <= MARKER_TYPE_MAX {
        Ok(())
    } else {
        Err(Reject::Range)
    }
}

/// The bits an ingredient's known effects can use: an ingredient has four
/// effects (CommonLibSSE-NG include/RE/I/IngredientItem.h, knownEffectFlags).
pub const EFFECT_BITS: u8 = 0x0f;

/// An ingredient effects report's mask: no bit past the fourth effect
/// (docs/verbs/learned-effects.md).
fn check_effects(m: &skymp::IngredientEffectsKnown) -> Result<(), Reject> {
    if m.mask & !EFFECT_BITS == 0 {
        Ok(())
    } else {
        Err(Reject::Range)
    }
}

/// The hotkeys a favorite can hold: -1 for none, 0 to 7 for the keys 1 to 8
/// (CommonLibSSE-NG include/RE/E/ExtraHotkey.h).
pub const HOTKEYS: core::ops::RangeInclusive<i8> = -1..=7;

/// An actor values snapshot, either way (docs/verbs/actor-values.md): every
/// number finite, each actor value below 164 and each skill below 18 listed
/// once. The values' own bounds are the server's rule (wire-rules
/// `actor_values`), which knows what the server holds.
fn check_actor_values(m: &skymp::ActorValues) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    let mut avs = [false; wire_schema::cap::ACTOR_VALUES];
    for b in m.bases.iter() {
        let slot = avs.get_mut(usize::from(b.av)).ok_or(Reject::Range)?;
        if *slot {
            return Err(Reject::Range);
        }
        *slot = true;
    }
    let mut skills = [false; wire_schema::cap::SKILLS];
    for s in m.skills.iter() {
        let slot = skills.get_mut(usize::from(s.skill)).ok_or(Reject::Range)?;
        if *slot {
            return Err(Reject::Range);
        }
        *slot = true;
    }
    let mut legendary = [false; wire_schema::cap::SKILLS];
    for l in m.legendary.iter() {
        let slot = legendary.get_mut(usize::from(l.skill)).ok_or(Reject::Range)?;
        if *slot {
            return Err(Reject::Range);
        }
        *slot = true;
    }
    Ok(())
}

/// Player globals, either way (docs/verbs/player-globals.md): every value
/// finite and each global given once among the values and once among the
/// globals to watch. Whether a global is one the server keeps per player is
/// the server's rule (wire-rules `player_globals`), which holds the list.
/// A host grant or release names a cell and an epoch above 0 (thuum
/// docs/verbs/cell-host.md).
fn check_host(cell: u32, epoch: u32) -> Result<(), Reject> {
    if cell == 0 || epoch == 0 {
        return Err(Reject::Range);
    }
    Ok(())
}

/// A player's race overlay (thuum ADR-068 rung 5, D-T1 A; wire-rules
/// player_own::Overlay): an open one wears a race of the load order (never 0
/// nor a server form) over a recorded race of the same kind, its lists name
/// load-order forms each once; a closed one names nothing (`open` false with
/// every field empty: the receiver reads it as none). Anything else is out
/// of range.
fn check_race_overlay(o: &skymp::RaceOverlay) -> Result<(), Reject> {
    let form = |id: u32| id != 0 && id < PLAYER_FORMS;
    let distinct = |ids: &[u32]| ids.iter().enumerate().all(|(i, id)| form(*id) && !ids.iter().take(i).any(|p| p == id));
    if !o.open {
        let empty = o.race == 0 && o.base == 0 && o.spells.is_empty() && o.shouts.is_empty() && o.items.is_empty();
        return if empty { Ok(()) } else { Err(Reject::Range) };
    }
    if !form(o.race) || !form(o.base) || o.race == o.base || !distinct(&o.spells) || !distinct(&o.shouts) || !distinct(&o.items) {
        return Err(Reject::Range);
    }
    Ok(())
}

/// UpdateAppearance (MsgType 4): its optional overlay is the server's word
/// on another player's figure ([`check_race_overlay`]); a client's own look
/// carries none ([`validate`]).
fn check_update_appearance(m: &skymp::UpdateAppearance) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    match &m.overlay {
        Some(overlay) => check_race_overlay(overlay),
        None => Ok(()),
    }
}

/// The record types a placed form may be (`placed_by`'s pairing): Raise
/// Wall's MSTT and ACTI (ADR-057), and since thuum ADR-068 rung 4 a script
/// set's placement, a STAT, an NPC_ or an item (libespm's utils::IsItem
/// types; wire-rules placements' script set places nothing else).
const PLACED_KINDS: [&str; 15] = ["MSTT", "ACTI", "STAT", "NPC_", "AMMO", "ARMO", "BOOK", "INGR", "ALCH", "SCRL", "SLGM", "WEAP", "MISC", "KEYM", "LIGH"];

/// A form the server introduces (thuum docs/verbs/spell-placed-object.md,
/// ADR-057): one its table placed at a caster's accepted cast names the
/// caster in `placed_by`, a server id above 0, and is an MSTT or an ACTI,
/// which its `base_record_type` says; since rung 4 a script set's
/// placement names its reporter the same way and is any of
/// [`PLACED_KINDS`]; any other `placed_by` is out of range. The transport
/// refuses the message from a client by direction.
fn check_create_actor(m: &skymp::CreateActor) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    // thuum ADR-068 rung 5 (D-T1 A): another player's figure wears its
    // overlay; a player's own CreateActor never carries one
    if let Some(overlay) = &m.props.overlay {
        if m.is_me {
            return Err(Reject::Range);
        }
        check_race_overlay(overlay)?;
    }
    if let Some(caster) = m.props.placed_by {
        let placed_kind = m.base_record_type.as_ref().is_some_and(|t| PLACED_KINDS.contains(&t.as_str()));
        if caster == 0 || !placed_kind {
            return Err(Reject::Range);
        }
    }
    // thuum docs/verbs/character-creation.md (the permit): the restricted
    // races and the ones this profile may wear travel together, on the
    // player's own CreateActor alone; no race id is 0 or repeated, and every
    // permitted race is a restricted one. Any other shape is out of range
    match (&m.props.restricted_races, &m.props.permitted_races) {
        (None, None) => {}
        (Some(restricted), Some(permitted)) => {
            let distinct = |ids: &[u32]| {
                ids.iter().enumerate().all(|(i, id)| *id != 0 && !ids.iter().take(i).any(|p| p == id))
            };
            if !m.is_me
                || !distinct(restricted)
                || !distinct(permitted)
                || permitted.iter().any(|p| !restricted.iter().any(|r| r == p))
            {
                return Err(Reject::Range);
            }
        }
        _ => return Err(Reject::Range),
    }
    Ok(())
}

fn check_player_globals(m: &skymp::PlayerGlobals) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    for (i, v) in m.values.iter().enumerate() {
        if m.values.iter().take(i).any(|p| p.global == v.global) {
            return Err(Reject::Range);
        }
    }
    for (i, w) in m.watch.iter().enumerate() {
        if m.watch.iter().take(i).any(|p| p == w) {
            return Err(Reject::Range);
        }
    }
    Ok(())
}

/// A quest stage, either way (docs/verbs/quest-stage.md): the mode is 0
/// (list) or 1 (all); a quest of 0 carries the login's list, which in mode
/// 0 is never empty (in mode 1 it is the denied list, empty when nothing
/// is denied), and the list names each quest once; from a client the list
/// is empty, the mode 0 and the quest named ([`validate`]). Since
/// SCHEMA_VERSION 41 (thuum ADR-068 rung 5): a word is a start or a stop
/// ([`skymp::QUEST_START`], [`skymp::QUEST_STOP`]) with stage 0 on a named
/// quest, never the login's list, and an acting player is a server id
/// (0xff000000 and up: the server names players) on a named quest; from a
/// client nothing is served ([`validate`]). The login's list may name the
/// player-class quests (`player`, thuum ADR-068 rung 5's ownership rule):
/// on quest 0 alone, never empty when present, each a form of the load
/// order (never 0 nor 0xff000000 and up) named once, at most the list's own
/// cap. Since SCHEMA_VERSION 42 (D-Q3 A): a run word may name the stages
/// whose fragments the engine runs with it (`wordStages`), on a word alone,
/// never empty when present, each stage once; a client names none
/// ([`validate`]: the record's flags are the server's to read). The mode's
/// membership, the class and the records are the server's rules
/// (wire-rules `quest_stages`, `quest_scope` and `quest_runs`).
fn check_quest_stage(m: &skymp::QuestStage) -> Result<(), Reject> {
    if m.mode > 1 {
        return Err(Reject::Range);
    }
    if m.mode == 0 && m.quest == 0 && m.shared.is_empty() {
        return Err(Reject::Range);
    }
    for (i, q) in m.shared.iter().enumerate() {
        if *q == 0 || m.shared.iter().take(i).any(|p| p == q) {
            return Err(Reject::Range);
        }
    }
    if let Some(word) = m.word {
        if (word != skymp::QUEST_START && word != skymp::QUEST_STOP) || m.stage != 0 || m.quest == 0 {
            return Err(Reject::Range);
        }
    }
    if m.acting.is_some_and(|acting| acting < PLAYER_FORMS || m.quest == 0) {
        return Err(Reject::Range);
    }
    if let Some(player) = &m.player {
        if m.quest != 0 || player.is_empty() {
            return Err(Reject::Range);
        }
        for (i, q) in player.iter().enumerate() {
            if *q == 0 || *q >= PLAYER_FORMS || player.iter().take(i).any(|p| p == q) {
                return Err(Reject::Range);
            }
        }
    }
    if let Some(stages) = &m.word_stages {
        if m.word.is_none() || stages.is_empty() {
            return Err(Reject::Range);
        }
        for (i, s) in stages.iter().enumerate() {
            if stages.iter().take(i).any(|p| p == s) {
                return Err(Reject::Range);
            }
        }
    }
    // SCHEMA_VERSION 43 (thuum ADR-068 rung 5, D-Q4 C): a report's cause is a
    // world cause or the seat's own, on a report of a quest; its root a
    // script's "<Script>.<Function>" in the characters Papyrus names take,
    // and only beside a cause. Whether the cause holds is the server's rule
    // (wire-rules quest_runs), and it is the seat's word either way (R2)
    if m.cause.is_some_and(|c| (c != skymp::QUEST_CAUSE_WORLD && c != skymp::QUEST_CAUSE_SEAT) || m.quest == 0) {
        return Err(Reject::Range);
    }
    if let Some(root) = &m.root {
        if m.cause.is_none() || root.is_empty() || !root.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'.') {
            return Err(Reject::Range);
        }
    }
    Ok(())
}

/// A RaceMenu preset, either way: not empty (docs/verbs/racemenu-sync.md).
/// From a client it is also its own ([`validate`]); the JSON's shape is the
/// server's rule (wire-rules `racemenu`), past the capacity the type holds.
fn check_preset(m: &skymp::RaceMenuPreset) -> Result<(), Reject> {
    if m.preset.is_empty() {
        return Err(Reject::Range);
    }
    Ok(())
}

/// A host's roster (docs/verbs/npc-puppet.md): every sighting names a
/// game-file actor (a form id of 1 to 0xfeffffff) in a named world or cell,
/// at a finite position inside the world's bounds. Whose cell it is, and
/// whether the sender hosts it, is the server's rule (PartOne).
fn check_sightings(m: &skymp::ActorSightings) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    for s in m.sightings.iter() {
        if s.refr_id == 0 || s.refr_id >= 0xff00_0000 || s.world_or_cell == 0 {
            return Err(Reject::Range);
        }
        check_pos(&s.pos)?;
    }
    Ok(())
}

/// A host's death report (docs/verbs/npc-fate.md, ADR-048's B): a game-file
/// actor (a form id of 1 to 0xfeffffff), a cause the contract names (0 to 4),
/// a finite point inside the world's bounds. Whether the sender simulates
/// the actor and whether the cause checks out is the server's rule.
fn check_actor_death(m: &skymp::ActorDeath) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    // thuum docs/verbs/npc-fate.md: a game-file actor's id, or since playtest
    // 37's finding 47 (2026-10-28) a server-made actor's (0xff000000 up, the
    // fate's own placements: a creature the server placed died by the
    // server's word after 3 s because its host could not report it); who may
    // report whom is the server's rule (ActionListener::OnActorDeath: the
    // actor's simulator, a fated actor, alive)
    if m.refr_id == 0 || m.cause > 4 {
        return Err(Reject::Range);
    }
    check_pos(&m.pos)?;
    Ok(())
}

/// A simulator's prop sample (docs/verbs/prop-physics.md, ADR-058): a
/// reference (never 0), a finite position inside the world's bounds, a
/// finite rotation. Whose sample is taken (the prop's owner, the cell's host
/// by default) is the server's rule (wire-rules `props`).
fn check_prop_transform(m: &skymp::PropTransform) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    if m.refr == 0 {
        return Err(Reject::Range);
    }
    check_pos(&m.pos)?;
    Ok(())
}

/// A seat's prop claim (docs/verbs/prop-physics.md): a reference (never 0)
/// and a touch the contract names (0 contact, 1 grab, 2 drop, 3 impulse).
/// Whether the claimant stands in the prop's cell and whether the prop is
/// another's to hold is the server's rule.
fn check_prop_claim(m: &skymp::PropClaim) -> Result<(), Reject> {
    if m.refr == 0 || m.touch > 3 {
        return Err(Reject::Range);
    }
    Ok(())
}

/// The server's owner word for a prop (docs/verbs/prop-physics.md): a
/// reference and a player, neither 0; the transport refuses it from a client
/// by direction.
fn check_prop_owner(m: &skymp::PropOwner) -> Result<(), Reject> {
    if m.refr == 0 || m.player == 0 {
        return Err(Reject::Range);
    }
    Ok(())
}

/// The server's request for a listed spell's placed objects (docs/verbs/
/// spell-placed-object.md, shape A): one to eight bases, none 0, and a
/// window of 1 to 10,000 ms; the transport refuses it from a client by
/// direction.
fn check_placed_request(m: &skymp::PlacedRequest) -> Result<(), Reject> {
    if m.bases.is_empty() || m.bases.contains(&0) || m.window_ms == 0 || m.window_ms > 10_000 {
        return Err(Reject::Range);
    }
    Ok(())
}

/// A caster's report of where its engine stands one base of a requested set
/// (docs/verbs/spell-placed-object.md, shape A): a base (never 0) and a
/// finite pose inside the world. Whether the set is this caster's and open,
/// the base the set's, and the pose inside the band from the caster is the
/// server's rule (wire-rules placements).
fn check_placed_report(m: &skymp::PlacedReport) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    if m.base == 0 {
        return Err(Reject::Range);
    }
    check_pos(&m.pos)?;
    Ok(())
}

/// A player's lock report (docs/verbs/locks.md): a reference (never 0), an
/// outcome the contract names (0 picked, 1 broken, 2 left) and at most 8
/// picks broken. The rest (locked, the reach, the tier, the picks held, the
/// gap) is the server's rule (wire-rules locks).
fn check_lock_report(m: &skymp::LockReport) -> Result<(), Reject> {
    if m.refr == 0 || m.outcome > 2 || m.broken > 8 {
        return Err(Reject::Range);
    }
    Ok(())
}

/// The server's lock prompt (docs/verbs/locks.md): a reference, never 0; the
/// transport refuses it from a client by direction.
fn check_lock_prompt(m: &skymp::LockPrompt) -> Result<(), Reject> {
    if m.refr == 0 {
        return Err(Reject::Range);
    }
    Ok(())
}

/// A player's perk pick (docs/verbs/perks.md): a perk, never 0. The rest (the
/// record's conditions, the points) is the server's rule (wire-rules perks).
fn check_perk_pick(m: &skymp::PerkPick) -> Result<(), Reject> {
    if m.perk == 0 {
        return Err(Reject::Range);
    }
    Ok(())
}

/// The server's perk state (docs/verbs/perks.md): no perk of 0 in the set;
/// the transport refuses it from a client by direction.
fn check_perk_state(m: &skymp::PerkState) -> Result<(), Reject> {
    if m.perks.contains(&0) {
        return Err(Reject::Range);
    }
    Ok(())
}

/// A player's word wall report (docs/verbs/word-wall.md): a wall reference,
/// never 0. The word, the reach and the record are the server's.
fn check_word_wall(m: &skymp::WordWall) -> Result<(), Reject> {
    if m.wall == 0 {
        return Err(Reject::Range);
    }
    Ok(())
}

/// Whether a trigger entry's event is one the schema names (thuum D-C2):
/// [`skymp::ENTRY_ENTER`] or [`skymp::ENTRY_LEAVE`].
fn is_entry_event(entry: u8) -> bool {
    entry == skymp::ENTRY_ENTER || entry == skymp::ENTRY_LEAVE
}

/// A seat's activation report (SkyMP's MsgType 6) and, since SCHEMA_VERSION
/// 40, its trigger entry (thuum D-W1 and D-C2): an Activate with `entry`
/// names OnTriggerEnter (1) or OnTriggerLeave (2), is no second activation,
/// names a presser (`caster`, never 0 nor past 32 bits, a form id's width:
/// the sender's own player 0x14 or an actor its game simulates) and a
/// trigger of the shared load order
/// (`target`, never 0 nor 0xff000000 and up). An Activate without `entry`
/// keeps the checks every message shares (it carries no float). Whether the
/// trigger is a world object the server holds, the presser the sender's,
/// the sender a loader of the cell that does not host it, a host there and
/// the presser within reach is the server's rule (wire-rules
/// trigger_entry); the budget is [`ClientGuard::entry_budget`].
fn check_activate(m: &skymp::Activate) -> Result<(), Reject> {
    let Some(entry) = m.data.entry else {
        return Ok(());
    };
    if !is_entry_event(entry) || m.data.is_second_activation || m.data.caster == 0 || m.data.target == 0 || m.data.target >= u64::from(PLAYER_FORMS) || m.data.caster > u64::from(u32::MAX) {
        return Err(Reject::Range);
    }
    Ok(())
}

/// The server's relayed activation (docs/verbs/traps.md): a target and a
/// caster, neither 0, and since SCHEMA_VERSION 40 (thuum D-C2) a trigger
/// entry's event, absent or 1 or 2; the transport refuses it from a client
/// by direction.
fn check_relayed_activation(m: &skymp::RelayedActivation) -> Result<(), Reject> {
    if m.target == 0 || m.caster == 0 || m.entry.is_some_and(|e| !is_entry_event(e)) {
        return Err(Reject::Range);
    }
    Ok(())
}

/// A world object's animation event (docs/verbs/traps.md, ADR-059 decision
/// 3): a reference, never 0, and an event name, never empty; the host's
/// budget is `object_animation_budget`, the same shape both ways.
fn check_object_animation(m: &skymp::ObjectAnimation) -> Result<(), Reject> {
    if m.refr == 0 || m.event.as_str().is_empty() {
        return Err(Reject::Range);
    }
    Ok(())
}

/// The highest destruction stage a report or a record may name (thuum
/// D-S3): wire-rules destruction's MAX_STAGE, the same number (wire-bridge's
/// test pins the two together).
pub const DESTRUCTION_MAX_STAGE: u8 = 127;

/// A reference's destruction stage (thuum D-S3, ADR-068's DestructionStage
/// row): a game-file reference, never 0 nor one the server names
/// (0xff000000 and up), and a stage of at most [`DESTRUCTION_MAX_STAGE`]; the
/// same shape both ways (a seat's report, the server's relay and its served
/// record). Whether the reference is one the server holds, no actor, in a
/// cell the reporter's game has loaded, and whether the report moves the
/// record is the server's rule (wire-rules destruction).
fn check_destruction_stage(m: &skymp::DestructionStage) -> Result<(), Reject> {
    if m.refr == 0 || m.refr >= PLAYER_FORMS || m.stage > DESTRUCTION_MAX_STAGE {
        return Err(Reject::Range);
    }
    Ok(())
}

/// A quest alias's fill (thuum ADR-068 rung 4, AliasFill): a quest of the
/// shared load order (never 0 nor one the server names, 0xff000000 and up),
/// an alias id of at most 0xFFFF (ALST's ids are an alias count's; the
/// handle's middle word, the platform's read, is 16 bits), a reference of
/// the load order or 0 for a clear; the same shape both ways (a seat's
/// report, the server's relay and its served record). Whether the quest has
/// that reference alias, and whether the fill moves the record, is the
/// server's rule (wire-rules alias_fills).
fn check_alias_fill(m: &skymp::AliasFill) -> Result<(), Reject> {
    if m.quest == 0 || m.quest >= PLAYER_FORMS || m.alias > 0xFFFF || m.refr >= PLAYER_FORMS {
        return Err(Reject::Range);
    }
    Ok(())
}

/// World globals (thuum ADR-068 rung 4, WorldGlobal): every value finite, at
/// most [`cap::WORLD_GLOBALS`](wire_schema::cap::WORLD_GLOBALS) entries, each
/// a GLOB of the shared load order (never 0 nor 0xff000000 and up) named
/// once, and a stream word naming its epoch (never 0); the same shape both
/// ways. Which globals the world keeps, and whose report counts, is the
/// server's rule (wire-rules world_globals and world_streamer).
fn check_world_global(m: &skymp::WorldGlobal) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    if m.entries.len() > wire_schema::cap::WORLD_GLOBALS || (m.stream.is_some() && m.epoch == 0) {
        return Err(Reject::Range);
    }
    for (i, entry) in m.entries.iter().enumerate() {
        if entry.global == 0 || entry.global >= PLAYER_FORMS {
            return Err(Reject::Range);
        }
        if m.entries.iter().take(i).any(|e| e.global == entry.global) {
            return Err(Reject::Range);
        }
    }
    Ok(())
}

/// One condition-read variable's key (thuum ADR-068 rung 5, ScriptVariables):
/// a form of the shared load order (never 0 nor 0xff000000 and up) and two
/// names neither empty nor carrying a NUL ([`has_nul`]: the receiver's
/// variable lookup takes a C string); each name's length holds at decode.
fn script_var_key_fits(form: u32, script: &str, var: &str) -> bool {
    form != 0 && form < PLAYER_FORMS && !script.is_empty() && !var.is_empty() && !has_nul(script) && !has_nul(var)
}

/// Whether two keys are one: the same form and the same names without case,
/// as the VM finds a script and a variable by name.
fn same_script_var_key(a: (u32, &str, &str), b: (u32, &str, &str)) -> bool {
    a.0 == b.0 && a.1.eq_ignore_ascii_case(b.1) && a.2.eq_ignore_ascii_case(b.2)
}

/// The condition-read variables (thuum ADR-068 rung 5, ScriptVariables),
/// either way: every float finite; every watched key and every entry's key
/// [`script_var_key_fits`], each key once in its list (the watch's and the
/// entries' apart; `own` no part of a key); each entry a kind of 3 (an
/// int), 4 (a float) or 5 (a bool) carrying that kind's one field and no
/// other, ScriptVar's folded kinds as GraphVar holds them; a served entry
/// with its key's sequence (never 0). The counts (at most
/// `cap::SCRIPT_VARIABLES` each) hold at decode. From a client no watch, no
/// served record and no sequence ([`validate`]). Which keys are listed,
/// whose they are and whether a write moves the record is the server's rule
/// (wire-rules script_variables).
fn check_script_variables(m: &skymp::ScriptVariables) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    for (n, k) in m.watch.iter().enumerate() {
        let key = (k.form, k.script.as_str(), k.var.as_str());
        if !script_var_key_fits(k.form, &k.script, &k.var) || m.watch.iter().take(n).any(|w| same_script_var_key((w.form, &w.script, &w.var), key)) {
            return Err(Reject::Range);
        }
    }
    for (n, e) in m.entries.iter().enumerate() {
        let key = (e.form, e.script.as_str(), e.var.as_str());
        let carried = [e.i.is_some(), e.f.is_some(), e.b.is_some()];
        let fits = match e.k {
            3 => carried == [true, false, false],
            4 => carried == [false, true, false],
            5 => carried == [false, false, true],
            _ => false,
        };
        let twice = m.entries.iter().take(n).any(|w| same_script_var_key((w.form, &w.script, &w.var), key));
        if !fits || twice || !script_var_key_fits(e.form, &e.script, &e.var) || (m.served && e.seq == 0) {
            return Err(Reject::Range);
        }
    }
    Ok(())
}

/// The server's weather word (docs/verbs/weather.md): a place, never 0; the
/// weather may be 0 (a release); the transport refuses it from a client by
/// direction.
fn check_set_weather(m: &skymp::SetWeather) -> Result<(), Reject> {
    if m.place == 0 {
        return Err(Reject::Range);
    }
    Ok(())
}

/// The most one skill use's base amount may carry: wire-rules leveling's
/// MAX_BASE_PER_USE (UNMEASURED; a Daedric craft is in the thousands).
const SKILL_USE_MAX_AMOUNT: f32 = 100_000.0;

/// A player's skill uses (docs/verbs/leveling.md): one to eighteen entries,
/// each skill (ActorValue 6 to 23) once, each amount finite and within 0
/// exclusive to the bound. What a use earns is the server's rule (wire-rules
/// leveling).
fn check_skill_use(m: &skymp::SkillUse) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    if m.uses.is_empty() {
        return Err(Reject::Range);
    }
    let mut seen = [false; 18];
    for u in m.uses.iter() {
        let Some(index) = u.skill.checked_sub(6).map(usize::from) else {
            return Err(Reject::Range);
        };
        let Some(slot) = seen.get_mut(index) else {
            return Err(Reject::Range);
        };
        if *slot || u.amount <= 0.0 || u.amount > SKILL_USE_MAX_AMOUNT {
            return Err(Reject::Range);
        }
        *slot = true;
    }
    Ok(())
}

/// A host's state report for an actor it simulates (docs/verbs/npc-stream.md,
/// ADR-050): three percentages, each a number within 0 and 1, and an actor
/// index below u32::MAX (ActorJump's idiom). Whether the sender hosts the
/// actor and whether a value is taken against the server's own numbers is
/// the server's rule (wire-rules hosted_state).
fn check_hosted_state(m: &skymp::HostedActorState) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    let within = |v: f32| (0.0..=1.0).contains(&v);
    if m.idx == u32::MAX || !within(m.health) || !within(m.magicka) || !within(m.stamina) {
        return Err(Reject::Range);
    }
    // slice 5: the gaze slot below the engine's six, the emotion within its
    // eight types, its strength at most 100
    if m.gaze_slot >= 6 || m.emotion_type > 7 || m.emotion_value > 100 {
        return Err(Reject::Range);
    }
    Ok(())
}

/// A host's spoken line (docs/verbs/npc-stream.md, the voice slice): an actor
/// index below u32::MAX, a topic and an info that are forms. Whether the
/// sender hosts the actor is the server's rule.
fn check_actor_speech(m: &skymp::ActorSpeech) -> Result<(), Reject> {
    if m.idx == u32::MAX || m.topic == 0 || m.info == 0 {
        return Err(Reject::Range);
    }
    Ok(())
}

/// The server's cause for a hit on a hosted NPC (docs/verbs/npc-fate.md,
/// ADR-050 decision 2): the victim's idx in range and the resulting health a
/// percentage.
fn check_damage_cause(m: &skymp::DamageCause) -> Result<(), Reject> {
    if !m.health.is_finite() {
        return Err(Reject::NonFinite);
    }
    if m.idx == u32::MAX || !(0.0..=1.0).contains(&m.health) {
        return Err(Reject::Range);
    }
    Ok(())
}

/// A favorites report: every hotkey in [`HOTKEYS`], no key bound twice, no
/// form listed twice (docs/verbs/favorites.md).
fn check_favorites(m: &skymp::Favorites) -> Result<(), Reject> {
    let mut keys = 0u8;
    for (i, e) in m.entries.iter().enumerate() {
        if !HOTKEYS.contains(&e.hotkey) {
            return Err(Reject::Range);
        }
        if let Ok(k) = u32::try_from(e.hotkey) {
            let bit = 1u8.checked_shl(k).unwrap_or(0);
            if bit == 0 || keys & bit != 0 {
                return Err(Reject::Range);
            }
            keys |= bit;
        }
        if m.entries.iter().take(i).any(|f| f.form == e.form) {
            return Err(Reject::Range);
        }
    }
    Ok(())
}

/// The form version byte every record on 1.6.1170 carries (the constant
/// 35740 writes; docs/research/adr065-serialize-on-demand-2026-11-03.md).
pub const CHANGE_FORM_VERSION: u8 = 0x4e;

/// Where the players' and the server-made forms begin: a change form names
/// a game-file actor alone (thuum docs/verbs/change-form.md).
const PLAYER_FORMS: u32 = 0xff00_0000;

/// The shortest packed record: a one-byte size code's header with no data.
const RECORD_MIN_LEN: usize = 4;

/// FNV-1a 32 over `bytes` (offset basis 0x811c9dc5, prime 0x01000193): the
/// variant the lab's Frida scripts (serialize-on-demand.js, apply-on-demand.js)
/// and skymp5-client compute over a packed record. The multiplication wraps
/// by the hash's definition, the one place a wrap is the meaning.
pub fn fnv1a_32(bytes: &[u8]) -> u32 {
    bytes
        .iter()
        .fold(0x811c_9dc5_u32, |h, b| (h ^ u32::from(*b)).wrapping_mul(0x0100_0193))
}

/// A packed change-form record's header as the engine's pack (Address
/// Library 36081) writes it: byte 0 the save-type index in its low six bits
/// and the size code in its top two, byte 1 the form version, then length1
/// (the data's size) and length2 (the inflated size when deflated, 0 when
/// raw), each of the code's width (0 one byte, 1 two, 2 four; the pack never
/// writes 3). docs/research/adr065-serialize-on-demand-2026-11-03.md Q3.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct RecordHeader {
    /// The save-type index (0 REFR, 1 ACHR).
    pub form_type: u8,
    /// The lengths' width code.
    pub size_code: u8,
    /// The form version byte.
    pub version: u8,
    /// The data's size in bytes.
    pub length1: u32,
    /// The inflated size when deflated, 0 for a raw record.
    pub length2: u32,
    /// Where the data starts: 2 plus twice the lengths' width.
    pub header_len: usize,
}

/// The header at the start of `body`, or `None` for a body too short for
/// its own header or a size code the pack never writes.
pub fn read_record_header(body: &[u8]) -> Option<RecordHeader> {
    let b0 = *body.first()?;
    let version = *body.get(1)?;
    let size_code = b0.checked_shr(6).unwrap_or(0);
    let width: usize = match size_code {
        0 => 1,
        1 => 2,
        2 => 4,
        _ => return None,
    };
    let read_len = |at: usize| -> Option<u32> {
        let bytes = body.get(at..at.checked_add(width)?)?;
        let mut value: u32 = 0;
        for (i, b) in bytes.iter().enumerate() {
            let shift = u32::try_from(i.checked_mul(8)?).ok()?;
            value |= u32::from(*b).checked_shl(shift)?;
        }
        Some(value)
    };
    let length1 = read_len(2)?;
    let length2 = read_len(2usize.checked_add(width)?)?;
    let header_len = 2usize.checked_add(width.checked_mul(2)?)?;
    Some(RecordHeader {
        form_type: b0 & 0x3f,
        size_code,
        version,
        length1,
        length2,
        header_len,
    })
}

/// The shape a change-form record and its remap rows must have, shared by
/// the host's report and the server's apply (thuum docs/verbs/change-form.md),
/// over the DECODED record bytes ([`decode_record_hex`] runs first):
/// a game-file actor's id and a six-bit type (`E_CHANGEFORM_RANGE`); a body
/// long enough for a header and no collection past its cap
/// (`E_CHANGEFORM_SIZE`); a header whose type byte is `form_type`, whose
/// version and the message's are 0x4E, whose length1 is the data's size and
/// whose length2 stays within the body cap (`E_CHANGEFORM_HEADER`); every
/// remap row inside the data, its three bytes kind 0 (top two bits clear)
/// decoding to the row's index, its form one a row may remap (never 0,
/// never load index 00, never a created form) and no offset twice
/// (`E_CHANGEFORM_REMAP`). Whose record it is and at which epoch is the
/// server's handler's rule.
fn check_change_form_shape(
    form_id: u32,
    form_type: u8,
    version: u8,
    body: &[u8],
    remap: &[skymp::ChangeFormRemap],
) -> Result<RecordHeader, Reject> {
    if form_id == 0 || form_id >= PLAYER_FORMS || form_type > 0x3f {
        return Err(Reject::ChangeFormRange);
    }
    if body.len() < RECORD_MIN_LEN
        || body.len() > wire_schema::cap::CHANGE_FORM_BODY
        || remap.len() > wire_schema::cap::CHANGE_FORM_REMAP
    {
        return Err(Reject::ChangeFormSize);
    }
    let header = read_record_header(body).ok_or(Reject::ChangeFormHeader)?;
    if header.form_type != form_type
        || header.version != CHANGE_FORM_VERSION
        || version != CHANGE_FORM_VERSION
    {
        return Err(Reject::ChangeFormHeader);
    }
    let data_len = body.len().checked_sub(header.header_len).ok_or(Reject::ChangeFormHeader)?;
    if usize::try_from(header.length1).ok() != Some(data_len) {
        return Err(Reject::ChangeFormHeader);
    }
    if usize::try_from(header.length2).map_or(true, |n| n > wire_schema::cap::CHANGE_FORM_BODY) {
        return Err(Reject::ChangeFormHeader);
    }
    for (i, row) in remap.iter().enumerate() {
        let at = usize::try_from(row.offset).map_err(|_| Reject::ChangeFormRemap)?;
        if at < header.header_len {
            return Err(Reject::ChangeFormRemap);
        }
        let end = at.checked_add(3).ok_or(Reject::ChangeFormRemap)?;
        let bytes = body.get(at..end).ok_or(Reject::ChangeFormRemap)?;
        let (d1, d2, d3) = match (bytes.first(), bytes.get(1), bytes.get(2)) {
            (Some(a), Some(b), Some(c)) => (*a, *b, *c),
            _ => return Err(Reject::ChangeFormRemap),
        };
        if d1 & 0xc0 != 0 {
            return Err(Reject::ChangeFormRemap);
        }
        let high = u32::from(d1 & 0x3f).checked_shl(16).ok_or(Reject::ChangeFormRemap)?;
        let mid = u32::from(d2).checked_shl(8).ok_or(Reject::ChangeFormRemap)?;
        if high | mid | u32::from(d3) != row.index {
            return Err(Reject::ChangeFormRemap);
        }
        // the engine encodes a load-index-00 form as kind 1 and a created one
        // as kind 2 on every seat: no row remaps either (apply-on-demand.js
        // v2.3's rule)
        if row.form_id < 0x0100_0000 || row.form_id >= PLAYER_FORMS {
            return Err(Reject::ChangeFormRemap);
        }
        if remap.iter().take(i).any(|earlier| earlier.offset == row.offset) {
            return Err(Reject::ChangeFormRemap);
        }
    }
    Ok(header)
}

/// The record bytes behind a message's hex body (docs/verbs/change-form.md):
/// two lowercase hex characters a byte, the spelling the native's serializer
/// hands the client. An odd length or a string past the cap is
/// `E_CHANGEFORM_SIZE`; a character that is not lowercase hex is
/// `E_CHANGEFORM_HEADER` (such a body holds no record at all).
pub fn decode_record_hex(hex: &str) -> Result<Vec<u8>, Reject> {
    if !hex.len().is_multiple_of(2) || hex.len() > wire_schema::cap::CHANGE_FORM_BODY_HEX {
        return Err(Reject::ChangeFormSize);
    }
    let nibble = |c: u8| -> Result<u8, Reject> {
        match c {
            b'0'..=b'9' => Ok(c.wrapping_sub(b'0')),
            b'a'..=b'f' => Ok(c.wrapping_sub(b'a').saturating_add(10)),
            _ => Err(Reject::ChangeFormHeader),
        }
    };
    // as_chunks (stable since 1.88): the CI's clippy (1.99) refuses
    // chunks_exact with a constant size; the remainder is empty after the
    // even-length check above, refused as SIZE if it ever were not
    let (pairs, rest) = hex.as_bytes().as_chunks::<2>();
    if !rest.is_empty() {
        return Err(Reject::ChangeFormSize);
    }
    let mut out = Vec::with_capacity(pairs.len());
    for &[high, low] in pairs {
        out.push(nibble(high)?.checked_shl(4).unwrap_or(0) | nibble(low)?);
    }
    Ok(out)
}

/// A host's change-form report (docs/verbs/change-form.md): the hex decoded,
/// the shared shape, then the hash over the record bytes
/// (`E_CHANGEFORM_HASH`).
fn check_change_form(m: &skymp::ChangeForm) -> Result<(), Reject> {
    let body = decode_record_hex(m.body.as_str())?;
    check_change_form_shape(m.form_id, m.form_type, m.version, &body, &m.remap)?;
    if fnv1a_32(&body) != m.fnv1a {
        return Err(Reject::ChangeFormHash);
    }
    Ok(())
}

/// The server's apply (docs/verbs/change-form.md): the hex decoded and the
/// shared shape; no hash travels. The transport refuses it from a client by
/// direction.
fn check_change_form_apply(m: &skymp::ChangeFormApply) -> Result<(), Reject> {
    let body = decode_record_hex(m.body.as_str())?;
    check_change_form_shape(m.form_id, m.form_type, m.version, &body, &m.remap).map(|_| ())
}

/// The server's receipt (docs/verbs/change-form.md): a form, never 0.
fn check_change_form_ack(m: &skymp::ChangeFormAck) -> Result<(), Reject> {
    if m.form_id == 0 {
        return Err(Reject::ChangeFormRange);
    }
    Ok(())
}

/// A tabled native an owner's engine ran (thuum ADR-068, the one seam;
/// docs/research/scripts-one-system-2026-11-05.md 6.11 item 13, "the shape
/// is the gate"): a class and a function, neither empty; a subject the
/// client may name (a load-order form or its own 0x14; the server names
/// 0xff000000 and up, its own forms and the players' figures), and every
/// object argument's id a form's 32 bits; every number finite. Since thuum
/// ADR-068 rung 5 an object argument may name a server id (0xff000000 to
/// 0xffffffff): inside an acting scope the platform puts the acting player's
/// figure where the script named the player (a relayed fragment's
/// ForceRefTo, an owned NPC's SetRelationshipRank), and the seat sends it by
/// the player's actor id; the server takes it only when it names a connected
/// player's actor (wire-rules natives::figures, E_SCRIPTCALL_ARGS), never
/// any other server form. The count (at most `cap::SCRIPT_CALL_ARGS`) and
/// every string's length hold at decode. Whether the pair is a tabled action
/// row, the arguments its count and kinds, the subject the sender's at the
/// epoch and the sender within its budget is the server's rule (wire-rules
/// natives); a value is never judged. Since SCHEMA_VERSION 44 a call may
/// carry `landed`, its leveled roll's landings ([`check_landed`]).
fn check_script_call(m: &skymp::ScriptCall) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    if m.cls.as_str().is_empty() || m.func.as_str().is_empty() || m.self_id >= PLAYER_FORMS {
        return Err(Reject::Range);
    }
    let past_a_form = |a: &Option<skymp::SnippetArg>| {
        matches!(a, Some(skymp::SnippetArg::Object(o)) if o.form_id > u64::from(u32::MAX))
    };
    if m.args.iter().any(past_a_form) {
        return Err(Reject::Range);
    }
    if let Some(landed) = &m.landed {
        check_landed(m, landed.as_slice())?;
    }
    Ok(())
}

/// A reported call's landings (thuum D-R1 C's leveled gap, SCHEMA_VERSION
/// 44), structural only: they ride an ObjectReference.AddItem (names without
/// case, as the server's table reads them) on the sender's own player, 0x14,
/// and no other call (`E_VAL_RANGE`); each names an item of the load order
/// both ends share (not 0, below 0xff000000, the server's forms), a count
/// above zero (0 is `E_VAL_ZERO_DELTA`, the ItemDelta rule; a negative one
/// `E_VAL_RANGE`) and a base no earlier landing named (the platform folds by
/// base). An empty list is the roll that landed nothing. The count, at most
/// `cap::SCRIPT_CALL_LANDED`, holds at decode; whether each base is one the
/// list can roll is the server's rule (wire-rules player_own,
/// `E_SCRIPTCALL_ROLL`).
fn check_landed(m: &skymp::ScriptCall, landed: &[ItemDelta]) -> Result<(), Reject> {
    let add_item = m.cls.as_str().eq_ignore_ascii_case("ObjectReference") && m.func.as_str().eq_ignore_ascii_case("AddItem");
    if !add_item || m.self_id != OWN_PLAYER {
        return Err(Reject::Range);
    }
    for (i, landing) in landed.iter().enumerate() {
        if landing.item.0 == 0 || landing.item.0 >= PLAYER_FORMS {
            return Err(Reject::Range);
        }
        if landing.count == 0 {
            return Err(Reject::ZeroDelta);
        }
        if landing.count < 0 || landed.iter().take(i).any(|earlier| earlier.item == landing.item) {
            return Err(Reject::Range);
        }
    }
    Ok(())
}

/// Object::Save's flags byte in a script record (thuum ADR-068 rung 3):
/// constructed, which a read requires.
const SCRIPT_CONSTRUCTED: u8 = 0x01;
/// The lock held: a read taken while a stack held the object, never a
/// quiescent one.
const SCRIPT_LOCKED: u8 = 0x04;
/// Initialized, which a read requires (an apply waits for it on the
/// receiver).
const SCRIPT_INITIALIZED: u8 = 0x08;
/// The kinds a script record's handle may name: 0 a form's, 1 and 2 the
/// others the save read met (thuum docs/research/
/// adr065-papyrus-state-2026-11-03.md Q1).
const SCRIPT_HANDLE_KINDS: u8 = 2;
/// A seat's own player, which a record never names (the owner encodes it as
/// None).
const OWN_PLAYER: u32 = 0x14;

/// Whether a script record's string or name holds a NUL. The receiver's
/// BSFixedString reads a C string, so a NUL would cut a type, a state, an
/// element type or a value short, and the platform's apply refuses the whole
/// record for one (its ReadString, skyrim-platform ScriptState.cpp): a
/// record every receiver refuses is refused here first.
fn has_nul(s: &str) -> bool {
    s.contains('\0')
}

/// A script value's payload against its kind (thuum ADR-068 rung 3): `k` 0
/// (None) carries nothing, 1 (an object) at most its handle (none is a typed
/// None), 2 its string, 3 its int, 4 its float, 5 its bool, and no other
/// field; `numbers` is which of the int, float and bool fields are present
/// and `s` the string. A string holds no NUL. A handle names a kind of 0 to
/// 2, the script type bound at it (never empty, no NUL: the receiver's
/// FindBoundObject takes it, and the platform's apply refuses an empty one)
/// and a form every seat shares: never a seat's own player (0x14) nor a form
/// the server names (0xff000000 and up), which the owner encodes as None.
fn check_script_payload(k: u8, numbers: [bool; 3], s: Option<&str>, o: Option<&skymp::ScriptHandle>) -> Result<(), Reject> {
    let [i, f, b] = numbers;
    let carried = [i, f, b, s.is_some()];
    let fits = match k {
        0 => carried == [false, false, false, false] && o.is_none(),
        1 => carried == [false, false, false, false],
        2 => carried == [false, false, false, true] && o.is_none(),
        3 => carried == [true, false, false, false] && o.is_none(),
        4 => carried == [false, true, false, false] && o.is_none(),
        5 => carried == [false, false, true, false] && o.is_none(),
        _ => false,
    };
    if !fits || s.is_some_and(has_nul) {
        return Err(Reject::Range);
    }
    match o {
        Some(h) if h.kind > SCRIPT_HANDLE_KINDS || h.form == OWN_PLAYER || h.form >= PLAYER_FORMS || h.cls.is_empty() || has_nul(&h.cls) => Err(Reject::Range),
        _ => Ok(()),
    }
}

/// The None array's kind (CommonLibSSE-NG include/RE/T/TypeInfo.h,
/// kNoneArray): a None array value, which carries no elements (the
/// platform's apply refuses any, its ParseVar); 11 to 15 are the typed
/// arrays.
const SCRIPT_NONE_ARRAY: u8 = 10;

/// One variable of a script record (thuum ADR-068 rung 3): a value of kind 0
/// to 5 ([`check_script_payload`]) with no array fields; an array of kind 11
/// to 15 with no value fields, its elements (none for a None array) each a
/// value of kind `k - 10`, and `elem` on an object array (11) alone, with no
/// NUL; or kind 10, the None array, with no field at all.
fn check_script_var(v: &skymp::ScriptVar) -> Result<(), Reject> {
    let numbers = [v.i.is_some(), v.f.is_some(), v.b.is_some()];
    match v.k {
        0..=5 => {
            if v.elem.is_some() || v.a.is_some() {
                return Err(Reject::Range);
            }
            check_script_payload(v.k, numbers, v.s.as_deref(), v.o.as_ref())
        }
        10..=15 => {
            let elem_fits = v.elem.as_deref().is_none_or(|elem| v.k == 11 && !has_nul(elem));
            if numbers.contains(&true) || v.s.is_some() || v.o.is_some() || !elem_fits || (v.k == SCRIPT_NONE_ARRAY && v.a.is_some()) {
                return Err(Reject::Range);
            }
            let element = v.k.checked_sub(SCRIPT_NONE_ARRAY).ok_or(Reject::Range)?;
            for e in v.a.iter().flat_map(|a| a.iter()) {
                if e.k != element {
                    return Err(Reject::Range);
                }
                check_script_payload(e.k, [e.i.is_some(), e.f.is_some(), e.b.is_some()], e.s.as_deref(), e.o.as_ref())?;
            }
            Ok(())
        }
        _ => Err(Reject::Range),
    }
}

/// A script record's reference and scripts (thuum ADR-068 rung 3, the shape
/// ScriptState and ScriptStateApply share): a game-file reference, never 0
/// nor one the server names (0xff000000 and up); 1 to 16 scripts (the
/// count's cap holds at decode), each with a type, its type and its state
/// holding no NUL ([`has_nul`]), its flags read at a quiescent point
/// (constructed and initialized, the lock not held) and every variable of a
/// kind the VM has with its payload. So whatever passes here the platform's apply parses
/// (ScriptState.cpp's ParseRecord): a record it would refuse whole is never
/// stored to be served. Whether the sender owns the reference at the epoch
/// is the server's rule (wire-rules script_state); no value is judged.
fn check_script_objects(form_id: u32, scripts: &[skymp::ScriptObjectState]) -> Result<(), Reject> {
    if form_id == 0 || form_id >= PLAYER_FORMS || scripts.is_empty() {
        return Err(Reject::Range);
    }
    for script in scripts {
        let quiescent = script.flags & SCRIPT_CONSTRUCTED != 0 && script.flags & SCRIPT_INITIALIZED != 0 && script.flags & SCRIPT_LOCKED == 0;
        if script.cls.is_empty() || has_nul(&script.cls) || has_nul(&script.state) || !quiescent {
            return Err(Reject::Range);
        }
        for v in script.vars.iter() {
            check_script_var(v)?;
        }
    }
    Ok(())
}

/// An owner's script record (thuum ADR-068 rung 3): every float finite, then
/// the shape [`check_script_objects`] holds.
fn check_script_state(m: &skymp::ScriptState) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    check_script_objects(m.form_id, m.scripts.as_slice())
}

/// The server's serve of a script record (thuum ADR-068 rung 3): the
/// record's shape, and since SCHEMA_VERSION 40 its graph variables'
/// ([`check_graph_vars`]); the transport refuses it from a client by
/// direction.
fn check_script_state_apply(m: &skymp::ScriptStateApply) -> Result<(), Reject> {
    if !all_finite(m) {
        return Err(Reject::NonFinite);
    }
    check_script_objects(m.form_id, m.scripts.as_slice())?;
    check_graph_vars(m.graph_vars.as_deref().unwrap_or_default())
}

/// A serve's graph variables (thuum D-C3 A, PENDING Eli): each with a name
/// neither empty nor holding a NUL ([`has_nul`]: the receiver's
/// SetAnimationVariable takes a C string), named once, a kind of 3 (an
/// int), 4 (a float) or 5 (a bool) carrying that kind's one field and no
/// other (ScriptVar's folded kinds), and a sequence above 0, the object's
/// at the call that set it. The count (at most `cap::GRAPH_VARS`) and each
/// name's length hold at decode, and every float's finiteness is the
/// apply's own first check ([`check_script_state_apply`]). Which variables
/// the server keeps is its rule (wire-rules natives, Keep::Variable).
fn check_graph_vars(vars: &[skymp::GraphVar]) -> Result<(), Reject> {
    for (n, v) in vars.iter().enumerate() {
        let carried = [v.i.is_some(), v.f.is_some(), v.b.is_some()];
        let fits = match v.k {
            3 => carried == [true, false, false],
            4 => carried == [false, true, false],
            5 => carried == [false, false, true],
            _ => false,
        };
        let twice = vars.iter().take(n).any(|w| w.name.as_str() == v.name.as_str());
        if !fits || v.seq == 0 || v.name.is_empty() || has_nul(&v.name) || twice {
            return Err(Reject::Range);
        }
    }
    Ok(())
}

/// Validate one inbound message from a client against its guard state: the
/// message's shape ([`validate_server`]'s rules) and then what only a
/// server's view of a client adds, replay order and rate budgets.
/// `Ok(())` means "well-formed and within rate"; ownership is checked later.
pub fn validate(msg: &Message, guard: &mut ClientGuard, now_ms: u64) -> Result<(), Reject> {
    validate_server(msg)?;
    match msg {
        Message::Movement(m) => {
            if guard.movement_primed && m.seq <= guard.last_movement_seq {
                return Err(Reject::SeqReplay);
            }
            if !guard.movement_budget.take(now_ms) {
                return Err(Reject::Rate);
            }
            guard.last_movement_seq = m.seq;
            guard.movement_primed = true;
            Ok(())
        }
        Message::Hit(h) => {
            if !guard.hit_window.accept(h.seq) {
                return Err(Reject::SeqReplay);
            }
            if !guard.hit_budget.take(now_ms) {
                return Err(Reject::Rate);
            }
            Ok(())
        }
        Message::RestIntent(_) => take(&mut guard.rest_budget, now_ms),
        Message::MapMarkerDiscovered(_) => take(&mut guard.marker_budget, now_ms),
        Message::IngredientEffectsKnown(_) => take(&mut guard.effects_budget, now_ms),
        Message::Favorites(_) => take(&mut guard.favorites_budget, now_ms),
        Message::RaceMenuPreset(m) => {
            // a client sends its own look; the server names whose it sends
            if m.actor != 0 {
                return Err(Reject::Range);
            }
            take(&mut guard.preset_budget, now_ms)
        }
        Message::ActorValues(_) => take(&mut guard.actor_values_budget, now_ms),
        Message::PlayerGlobals(m) => {
            // a client reports values; which globals to watch is the server's
            if !m.watch.is_empty() {
                return Err(Reject::Range);
            }
            take(&mut guard.player_globals_budget, now_ms)
        }
        Message::QuestStage(m) => {
            // a client reports one stage or run word of one quest; the
            // sharing mode, its list, the login replay and a word's stages
            // are the server's (docs/verbs/quest-stage.md; thuum ADR-068
            // rung 5); a word shares the stages' budget
            if !m.shared.is_empty() || m.quest == 0 || m.mode != 0 || m.served == Some(true) || m.word_stages.is_some() {
                return Err(Reject::Range);
            }
            take(&mut guard.quest_stage_budget, now_ms)
        }
        Message::ConsoleCommand(_) => take(&mut guard.console_budget, now_ms),
        Message::PlayerBowShot(_) => take(&mut guard.shot_budget, now_ms),
        Message::OnHit(_) => take(&mut guard.on_hit_budget, now_ms),
        Message::ActorDeath(_) => take(&mut guard.actor_death_budget, now_ms),
        Message::SpellCast(_) => take(&mut guard.cast_budget, now_ms),
        Message::OnEquip(_) => take(&mut guard.equip_budget, now_ms),
        Message::UpdateAction(_) => take(&mut guard.action_budget, now_ms),
        Message::CellLoaded(_) => take(&mut guard.cell_loaded_budget, now_ms),
        Message::HostedActorState(_) => take(&mut guard.hosted_state_budget, now_ms),
        Message::ActorSpeech(_) => take(&mut guard.speech_budget, now_ms),
        Message::ActorClaim(_) => take(&mut guard.claim_budget, now_ms),
        Message::PropTransform(_) => take(&mut guard.prop_budget, now_ms),
        Message::PropClaim(_) => take(&mut guard.prop_claim_budget, now_ms),
        Message::PlacedReport(_) => take(&mut guard.placed_report_budget, now_ms),
        Message::SkillUse(_) => take(&mut guard.skill_use_budget, now_ms),
        Message::LockReport(_) => take(&mut guard.lock_report_budget, now_ms),
        Message::PerkPick(_) => take(&mut guard.perk_pick_budget, now_ms),
        Message::WordWall(_) => take(&mut guard.word_wall_budget, now_ms),
        // thuum D-C2: a trigger entry spends the entry budget; any other
        // activation spends none, as before
        Message::Activate(m) if m.data.entry.is_some() => take(&mut guard.entry_budget, now_ms),
        Message::ObjectAnimation(_) => take(&mut guard.object_animation_budget, now_ms),
        Message::ChangeForm(_) => take(&mut guard.change_form_budget, now_ms),
        Message::DestructionStage(m) => {
            // a seat reports a destruction step alone: the ref state's words
            // and the object's sequence are the server's (thuum ADR-068 rung
            // 4; its report is ScriptCall 75)
            if m.disabled.is_some() || m.deleted.is_some() || m.seq != 0 {
                return Err(Reject::Range);
            }
            take(&mut guard.destruction_budget, now_ms)
        }
        Message::ScriptState(_) => take(&mut guard.script_state_budget, now_ms),
        Message::AliasFill(m) => {
            // a seat reports its own fill; a served record is the server's
            if m.served {
                return Err(Reject::Range);
            }
            take(&mut guard.alias_fill_budget, now_ms)
        }
        Message::WorldGlobal(m) => {
            // the streamer reports under the epoch its word named (ADR-066's
            // epoch-0 rule); the stream word and a served record are the
            // server's
            if m.stream.is_some() || m.served || m.epoch == 0 {
                return Err(Reject::Range);
            }
            take(&mut guard.world_global_budget, now_ms)
        }
        Message::ScriptVariables(m) => {
            // a seat reports what its game changed (thuum ADR-068 rung 5):
            // the watch list, a served record and every key's sequence are
            // the server's
            if !m.watch.is_empty() || m.served || m.entries.iter().any(|e| e.seq != 0) {
                return Err(Reject::Range);
            }
            take(&mut guard.script_variables_budget, now_ms)
        }
        // thuum ADR-068 rung 5 (D-T1 A): a seat sends its own look; a
        // figure's overlay is the server's word
        Message::UpdateAppearance(m) if m.overlay.is_some() => Err(Reject::Range),
        _ => Ok(()),
    }
}

fn take(budget: &mut TokenBucket, now_ms: u64) -> Result<(), Reject> {
    if budget.take(now_ms) {
        Ok(())
    } else {
        Err(Reject::Rate)
    }
}

/// The server's fates (docs/verbs/npc-fate.md, ADR-049): every entry names a
/// placement, a game-file one (a form id of 1 to 0xfeffffff) or, since
/// ADR-050 decision 3 (train 51), a server-made NPC's (0xff000000 up, fated
/// through the same roll before its create word); the base may be 0 (the
/// roll chose none) or any form; every worn id is a form (never 0).
fn check_fates(m: &skymp::ActorFate) -> Result<(), Reject> {
    for f in m.fates.iter() {
        if f.refr_id == 0 {
            return Err(Reject::Range);
        }
        if f.worn.contains(&0) {
            return Err(Reject::Range);
        }
        // the record's inventory rows (playtest 44's 1c): a real item, a count
        if f.inv.iter().any(|row| row.base_id == 0 || row.count == 0) {
            return Err(Reject::Range);
        }
    }
    Ok(())
}

/// Validate one message by its shape alone: what holds whichever side sent
/// it, without a client's rate budgets or replay state, which guard a server
/// against its clients ([`validate`]). A client checks what its server sends
/// with this, and the server checks what it is about to send.
pub fn validate_server(msg: &Message) -> Result<(), Reject> {
    match msg {
        Message::Hello(h) => {
            if h.schema_version != wire_schema::SCHEMA_VERSION {
                return Err(Reject::SchemaVersion);
            }
            Ok(())
        }
        Message::Movement(m) => check_transform(&m.transform),
        // replay order and rate are all a hit is checked for ([`validate`])
        Message::Hit(_) => Ok(()),
        Message::ReservedHostedActor(s) => {
            check_transform(&s.transform)?;
            if !s.health.is_finite() {
                return Err(Reject::NonFinite);
            }
            if s.inventory_delta.iter().any(|d| d.count == 0) {
                return Err(Reject::ZeroDelta);
            }
            Ok(())
        }
        Message::UpdateMovement(m) => {
            if !all_finite(m) {
                return Err(Reject::NonFinite);
            }
            // thuum docs/verbs/npc-puppet.md (x-npc-death 20261023-054215): a
            // health percentage lives in [0, 1]. The engine's deferred kill
            // lets a host's actor sit below zero, and that number killed the
            // puppet on the other screen; the client clamps at the source and
            // this refuses the rest
            if !(0.0..=1.0).contains(&m.data.health_percentage) {
                return Err(Reject::Range);
            }
            check_pos(&m.data.pos)
        }
        Message::Teleport(m) => {
            if !all_finite(m) {
                return Err(Reject::NonFinite);
            }
            check_pos(&m.pos)
        }
        Message::Teleport2(m) => {
            if !all_finite(m) {
                return Err(Reject::NonFinite);
            }
            check_pos(&m.pos)
        }
        // thuum docs/verbs/cast-teleport.md: a placement names an actor by
        // its world index, which the server hands out from 0 (the first
        // actor's); only the invalid index is out of range
        Message::ActorJump(m) => {
            if m.idx == u32::MAX {
                return Err(Reject::Range);
            }
            Ok(())
        }
        Message::SetGameTime(m) => {
            if !all_finite(m) {
                return Err(Reject::NonFinite);
            }
            check_game_time(m)
        }
        Message::RestIntent(m) => {
            if !all_finite(m) {
                return Err(Reject::NonFinite);
            }
            check_rest(m)
        }
        Message::MapMarkerDiscovered(m) => check_marker(m),
        Message::IngredientEffectsKnown(m) => check_effects(m),
        Message::Favorites(m) => check_favorites(m),
        Message::RaceMenuPreset(m) => check_preset(m),
        Message::ActorValues(m) => check_actor_values(m),
        Message::PlayerGlobals(m) => check_player_globals(m),
        Message::QuestStage(m) => check_quest_stage(m),
        // thuum docs/verbs/cell-host.md: a cell of 0 names nothing; an
        // epoch of 0 is before any grant
        Message::CellLoaded(m) => {
            if m.cell == 0 {
                return Err(Reject::Range);
            }
            Ok(())
        }
        Message::HostGrant(m) => check_host(m.cell, m.epoch),
        Message::HostRelease(m) => check_host(m.cell, m.epoch),
        Message::PlayerBowShot(m) => {
            if !all_finite(m) {
                return Err(Reject::NonFinite);
            }
            check_bow_shot(m)
        }
        // thuum docs/verbs/marksman.md: a shot's relay carries the shot's
        // launch, under the shot's rule; the transport refuses it from a
        // client by direction
        Message::ArrowShot(m) => {
            if !all_finite(m) {
                return Err(Reject::NonFinite);
            }
            check_launch(m.has_launch, &[m.origin_x, m.origin_y, m.origin_z], m.launch_angle_x, m.launch_angle_z)
        }
        Message::SpellCast(m) => {
            if !all_finite(m) {
                return Err(Reject::NonFinite);
            }
            check_spell_cast(m)
        }
        // thuum docs/verbs/console-commands.md: the server's own line, its
        // length bounded at decode (cap::CONSOLE_OUTPUT); the transport
        // refuses it from a client by direction
        Message::ConsoleOutput(_) => Ok(()),
        Message::UpdateAction(m) => check_update_action(m),
        // thuum docs/verbs/magic-effects.md: the server's own effect set, at
        // most MAGIC_EFFECTS entries and EFFECT_MODIFIERS sums (bounded at
        // decode); the transport refuses it from a client by direction, its
        // numbers are finite and each sum names an actor value the engine
        // has; an effect's caster is any form id, 0 for none (thuum
        // docs/verbs/npc-fate.md C4: a reanimate's caster, the server's own)
        Message::MagicEffects(m) => {
            if !all_finite(m) {
                Err(Reject::NonFinite)
            } else if m.modifiers.iter().any(|x| usize::try_from(x.av).map_or(true, |av| av >= wire_schema::cap::ACTOR_VALUES)) {
                Err(Reject::Range)
            } else {
                Ok(())
            }
        }
        Message::ActorSightings(m) => check_sightings(m),
        Message::ActorFate(m) => check_fates(m),
        Message::ActorDeath(m) => check_actor_death(m),
        Message::HostedActorState(m) => check_hosted_state(m),
        Message::ActorSpeech(m) => check_actor_speech(m),
        Message::DamageCause(m) => check_damage_cause(m),
        Message::ActorClaim(m) => {
            if m.idx == u32::MAX {
                Err(Reject::Range)
            } else {
                Ok(())
            }
        }
        Message::PropTransform(m) => check_prop_transform(m),
        Message::PropClaim(m) => check_prop_claim(m),
        Message::PropOwner(m) => check_prop_owner(m),
        Message::PlacedRequest(m) => check_placed_request(m),
        Message::PlacedReport(m) => check_placed_report(m),
        Message::SkillUse(m) => check_skill_use(m),
        Message::LockReport(m) => check_lock_report(m),
        Message::LockPrompt(m) => check_lock_prompt(m),
        Message::SetWeather(m) => check_set_weather(m),
        Message::PerkPick(m) => check_perk_pick(m),
        Message::PerkState(m) => check_perk_state(m),
        Message::WordWall(m) => check_word_wall(m),
        Message::Activate(m) => check_activate(m),
        Message::RelayedActivation(m) => check_relayed_activation(m),
        Message::ObjectAnimation(m) => check_object_animation(m),
        Message::ChangeForm(m) => check_change_form(m),
        Message::ChangeFormAck(m) => check_change_form_ack(m),
        Message::ChangeFormApply(m) => check_change_form_apply(m),
        Message::ScriptCall(m) => check_script_call(m),
        Message::ScriptState(m) => check_script_state(m),
        Message::ScriptStateApply(m) => check_script_state_apply(m),
        Message::DestructionStage(m) => check_destruction_stage(m),
        Message::AliasFill(m) => check_alias_fill(m),
        Message::WorldGlobal(m) => check_world_global(m),
        Message::ScriptVariables(m) => check_script_variables(m),
        Message::CreateActor(m) => check_create_actor(m),
        Message::UpdateAppearance(m) => check_update_appearance(m),
        // Everything else: the checks all messages share. A server-to-client
        // message arriving from a client is shape-valid here; the transport
        // rejects it by direction before it gets this far.
        other => {
            if all_finite(other) {
                Ok(())
            } else {
                Err(Reject::NonFinite)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;
    use wire_schema::{
        cap, FormId, Hello, HitIntent, ItemDelta, MovementSample, ReservedHostedActorState, SCHEMA_VERSION,
    };

    fn movement(seq: u32, x: f32) -> Message {
        Message::Movement(MovementSample {
            seq,
            actor: FormId(0x14),
            transform: Transform {
                x,
                y: 0.0,
                z: 0.0,
                yaw: 0.0,
                pitch: 0.0,
            },
            run: false,
            sneak: false,
        })
    }

    fn hit(seq: u32) -> Message {
        Message::Hit(HitIntent {
            seq,
            attacker: FormId(0x14),
            target: FormId(0x15),
            weapon: FormId(0x1234),
            power_attack: false,
            client_time_ms: 0,
        })
    }

    fn hello(v: u16) -> Message {
        Message::Hello(Hello {
            schema_version: v,
            client_build: 1,
            mod_hashes: wire_schema::bounded::Vec::new(),
            name: wire_schema::bounded::String::new(),
        })
    }

    fn hosted(health: f32, counts: &[i32]) -> Message {
        let mut inv = wire_schema::bounded::Vec::new();
        for c in counts {
            let _ = inv.push(ItemDelta {
                item: FormId(1),
                count: *c,
            });
        }
        Message::ReservedHostedActor(ReservedHostedActorState {
            actor: FormId(0x20),
            transform: Transform {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                yaw: 0.0,
                pitch: 0.0,
            },
            health,
            inventory_delta: inv,
        })
    }

    #[test]
    fn schema_version_both_ways() {
        let mut g = ClientGuard::default();
        assert_eq!(validate(&hello(SCHEMA_VERSION), &mut g, 0), Ok(()));
        assert_eq!(
            validate(&hello(SCHEMA_VERSION ^ 1), &mut g, 0),
            Err(Reject::SchemaVersion)
        );
    }

    #[test]
    fn non_finite_both_ways() {
        let mut g = ClientGuard::default();
        assert_eq!(
            validate(&movement(1, f32::NAN), &mut g, 0),
            Err(Reject::NonFinite)
        );
        assert_eq!(validate(&movement(1, 10.0), &mut g, 0), Ok(()));
        assert_eq!(
            validate(&hosted(f32::INFINITY, &[1]), &mut g, 0),
            Err(Reject::NonFinite)
        );
        assert_eq!(validate(&hosted(50.0, &[1]), &mut g, 0), Ok(()));
    }

    #[test]
    fn out_of_world_both_ways() {
        let mut g = ClientGuard::default();
        assert_eq!(
            validate(&movement(1, 2.0e6), &mut g, 0),
            Err(Reject::OutOfWorld)
        );
        assert_eq!(
            validate(&movement(1, -2.0e6), &mut g, 0),
            Err(Reject::OutOfWorld)
        );
        assert_eq!(validate(&movement(1, WORLD_ABS_MAX), &mut g, 0), Ok(()));
    }

    #[test]
    fn rate_both_ways() {
        let mut g = ClientGuard::default();
        for seq in 1..=60u32 {
            assert_eq!(validate(&movement(seq, 0.0), &mut g, 0), Ok(()));
        }
        assert_eq!(validate(&movement(61, 0.0), &mut g, 0), Err(Reject::Rate));
        // One second later the bucket has refilled.
        assert_eq!(validate(&movement(61, 0.0), &mut g, 1_000), Ok(()));
    }

    #[test]
    fn zero_delta_both_ways() {
        let mut g = ClientGuard::default();
        assert_eq!(
            validate(&hosted(1.0, &[0]), &mut g, 0),
            Err(Reject::ZeroDelta)
        );
        assert_eq!(validate(&hosted(1.0, &[1, -1]), &mut g, 0), Ok(()));
    }

    #[test]
    fn movement_replay_is_monotonic_and_seq_zero_counts() {
        let mut g = ClientGuard::default();
        assert_eq!(validate(&movement(0, 0.0), &mut g, 0), Ok(()));
        assert_eq!(
            validate(&movement(0, 0.0), &mut g, 0),
            Err(Reject::SeqReplay)
        );
        assert_eq!(validate(&movement(5, 0.0), &mut g, 0), Ok(()));
        assert_eq!(
            validate(&movement(5, 0.0), &mut g, 0),
            Err(Reject::SeqReplay)
        );
        assert_eq!(
            validate(&movement(4, 0.0), &mut g, 0),
            Err(Reject::SeqReplay)
        );
        assert_eq!(validate(&movement(6, 0.0), &mut g, 0), Ok(()));
    }

    #[test]
    fn hit_replay_is_a_window() {
        let mut g = ClientGuard::default();
        assert_eq!(validate(&hit(10), &mut g, 0), Ok(()));
        assert_eq!(validate(&hit(12), &mut g, 0), Ok(()));
        assert_eq!(
            validate(&hit(11), &mut g, 0),
            Ok(()),
            "out of order inside the window is legal"
        );
        assert_eq!(validate(&hit(11), &mut g, 0), Err(Reject::SeqReplay));
        assert_eq!(validate(&hit(10), &mut g, 0), Err(Reject::SeqReplay));
        assert_eq!(validate(&hit(200), &mut g, 0), Ok(()));
        assert_eq!(
            validate(&hit(100), &mut g, 0),
            Err(Reject::SeqReplay),
            "older than the window"
        );
        assert_eq!(validate(&hit(199), &mut g, 0), Ok(()));
        assert_eq!(
            validate(&hit(137), &mut g, 0),
            Ok(()),
            "last slot inside the window"
        );
        assert_eq!(
            validate(&hit(136), &mut g, 0),
            Err(Reject::SeqReplay),
            "first slot outside it"
        );
    }

    fn skymp_movement(x: f32, speed: f32) -> Message {
        Message::UpdateMovement(skymp::UpdateMovement {
            t: skymp::MsgT,
            idx: 1,
            data: skymp::MovementData {
                pos: [x, 0.0, 0.0],
                speed,
                ..Default::default()
            },
        })
    }

    // thuum docs/verbs/npc-puppet.md (x-npc-death 20261023-054215)
    #[test]
    fn skymp_movement_health_outside_the_unit_interval_is_range() {
        let mut g = ClientGuard::default();
        let at = |health: f32| {
            Message::UpdateMovement(skymp::UpdateMovement {
                t: skymp::MsgT,
                idx: 1,
                data: skymp::MovementData {
                    pos: [0.0, 0.0, 0.0],
                    health_percentage: health,
                    ..Default::default()
                },
            })
        };
        assert_eq!(validate(&at(-1.2887), &mut g, 0), Err(Reject::Range));
        assert_eq!(validate(&at(1.5), &mut g, 0), Err(Reject::Range));
        assert!(validate(&at(0.0), &mut g, 0).is_ok());
        assert!(validate(&at(1.0), &mut g, 0).is_ok());
        assert!(validate(&at(0.5), &mut g, 0).is_ok());
    }

    #[test]
    fn skymp_movement_both_ways() {
        let mut g = ClientGuard::default();
        assert_eq!(validate(&skymp_movement(133857.0, 300.0), &mut g, 0), Ok(()));
        assert_eq!(
            validate(&skymp_movement(f32::NAN, 0.0), &mut g, 0),
            Err(Reject::NonFinite)
        );
        assert_eq!(
            validate(&skymp_movement(0.0, f32::INFINITY), &mut g, 0),
            Err(Reject::NonFinite),
            "any float, not only the position"
        );
        assert_eq!(
            validate(&skymp_movement(2.0e6, 0.0), &mut g, 0),
            Err(Reject::OutOfWorld)
        );
        // no rate rule: SkyMP's own behavior, bounded by the transport's byte budget
        for _ in 0..1000 {
            assert_eq!(validate(&skymp_movement(1.0, 0.0), &mut g, 0), Ok(()));
        }
    }

    #[test]
    fn every_skymp_float_is_checked() {
        let mut g = ClientGuard::default();
        let ok = Message::ChangeValues(skymp::ChangeValues {
            data: skymp::ChangeValuesData {
                health: Some(0.5),
                ..Default::default()
            },
            ..Default::default()
        });
        assert_eq!(validate(&ok, &mut g, 0), Ok(()));
        let bad = Message::ChangeValues(skymp::ChangeValues {
            data: skymp::ChangeValuesData {
                health: Some(f32::NAN),
                ..Default::default()
            },
            ..Default::default()
        });
        assert_eq!(validate(&bad, &mut g, 0), Err(Reject::NonFinite));
        let shot = Message::PlayerBowShot(skymp::PlayerBowShot {
            power: f32::NEG_INFINITY,
            ..Default::default()
        });
        assert_eq!(validate(&shot, &mut g, 0), Err(Reject::NonFinite));
    }

    fn game_time(month: u32, day: u32, hour: f32, days_passed: f32, time_scale: f32) -> Message {
        Message::SetGameTime(skymp::SetGameTime {
            year: 201,
            month,
            day,
            hour,
            days_passed,
            time_scale,
            ..Default::default()
        })
    }

    #[test]
    fn game_time_both_ways() {
        let mut g = ClientGuard::default();
        for ok in [game_time(7, 17, 8.0, 1.0, 20.0), game_time(0, 1, 0.0, 0.0, 0.0), game_time(11, 31, 23.99, 1e6, 1.0)] {
            assert_eq!(validate(&ok, &mut g, 0), Ok(()));
        }
        for bad in [
            game_time(12, 1, 8.0, 1.0, 20.0),
            game_time(7, 0, 8.0, 1.0, 20.0),
            game_time(7, 32, 8.0, 1.0, 20.0),
            game_time(7, 17, 24.0, 1.0, 20.0),
            game_time(7, 17, -0.5, 1.0, 20.0),
            game_time(7, 17, 8.0, -1.0, 20.0),
            game_time(7, 17, 8.0, 1.0, -20.0),
        ] {
            assert_eq!(validate(&bad, &mut g, 0), Err(Reject::Range), "{bad:?}");
        }
        assert_eq!(validate(&game_time(7, 17, f32::NAN, 1.0, 20.0), &mut g, 0), Err(Reject::NonFinite));
        assert_eq!(validate(&game_time(7, 17, 8.0, 1.0, f32::INFINITY), &mut g, 0), Err(Reject::NonFinite));
    }

    fn rest(hours: f32) -> Message {
        Message::RestIntent(skymp::RestIntent { hours, sleep: false, ..Default::default() })
    }

    #[test]
    fn rest_both_ways() {
        let mut g = ClientGuard::default();
        assert_eq!(validate(&rest(1.0), &mut g, 0), Ok(()));
        assert_eq!(validate(&rest(24.0), &mut g, 0), Ok(()));
        // two at once, then one a second
        assert_eq!(validate(&rest(8.0), &mut g, 0), Err(Reject::Rate));
        assert_eq!(validate(&rest(8.0), &mut g, 1_000), Ok(()));
        for bad in [0.0, 0.99, 24.01, -1.0, 1e9] {
            assert_eq!(validate(&rest(bad), &mut ClientGuard::default(), 0), Err(Reject::Range), "{bad}");
        }
        for odd in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
            assert_eq!(validate(&rest(odd), &mut ClientGuard::default(), 0), Err(Reject::NonFinite), "{odd}");
        }
    }

    fn marker(marker_type: u16) -> Message {
        Message::MapMarkerDiscovered(skymp::MapMarkerDiscovered { marker_type, can_travel: true, ..Default::default() })
    }

    fn effects(mask: u8) -> Message {
        Message::IngredientEffectsKnown(skymp::IngredientEffectsKnown { ingredient: 0x0003_4cdd, mask, ..Default::default() })
    }

    #[test]
    fn effects_both_ways() {
        let mut g = ClientGuard::default();
        for m in [0x00, 0x01, 0x05, 0x0f, 0x08, 0x0e, 0x02, 0x04] {
            assert_eq!(validate(&effects(m), &mut g, 0), Ok(()), "{m:#x}");
        }
        // eight at once, then two a second
        assert_eq!(validate(&effects(1), &mut g, 0), Err(Reject::Rate));
        assert_eq!(validate(&effects(1), &mut g, 1_000), Ok(()));
        for bad in [0x10, 0x1f, 0x80, 0xff] {
            assert_eq!(validate(&effects(bad), &mut ClientGuard::default(), 0), Err(Reject::Range), "{bad:#x}");
        }
    }

    fn favorites(entries: &[(u32, i8)]) -> Message {
        Message::Favorites(skymp::Favorites {
            entries: entries
                .iter()
                .map(|&(form, hotkey)| skymp::FavoriteEntry { form, hotkey })
                .collect::<std::vec::Vec<_>>()
                .try_into()
                .unwrap_or_default(),
            ..Default::default()
        })
    }

    #[test]
    fn favorites_both_ways() {
        let every_key: std::vec::Vec<(u32, i8)> = (0u8..8).map(|k| (0x0001_0000 + u32::from(k), i8::try_from(k).unwrap_or(-1))).collect();
        let fine: [&[(u32, i8)]; 4] = [&[], &[(0x0001_397e, 2)], &[(0x0001_397e, -1), (0x0001_2fcd, -1)], &every_key];
        for ok in fine {
            assert_eq!(validate(&favorites(ok), &mut ClientGuard::default(), 0), Ok(()), "{ok:?}");
        }
        // four at once, then one a second
        let mut g = ClientGuard::default();
        for _ in 0..4 {
            assert_eq!(validate(&favorites(&[]), &mut g, 0), Ok(()));
        }
        assert_eq!(validate(&favorites(&[]), &mut g, 0), Err(Reject::Rate));
        assert_eq!(validate(&favorites(&[]), &mut g, 1_000), Ok(()));
        // a hotkey out of range, a key twice, a form twice
        let bad: [&[(u32, i8)]; 6] = [&[(1, 8)], &[(1, -2)], &[(1, i8::MAX)], &[(1, 3), (2, 3)], &[(1, -1), (1, 2)], &[(1, -1), (2, -1), (1, -1)]];
        for b in bad {
            assert_eq!(validate(&favorites(b), &mut ClientGuard::default(), 0), Err(Reject::Range), "{b:?}");
        }
    }

    fn console(name: &str) -> Message {
        Message::ConsoleCommand(skymp::ConsoleCommand {
            data: skymp::ConsoleCommandData { command_name: name.try_into().unwrap_or_default(), ..Default::default() },
            ..Default::default()
        })
    }

    #[test]
    fn console_commands_four_at_once_then_one_a_second() {
        let mut g = ClientGuard::default();
        for name in ["additem", "save", "nosuchcommand", "setav"] {
            assert_eq!(validate(&console(name), &mut g, 0), Ok(()), "{name}");
        }
        assert_eq!(validate(&console("additem"), &mut g, 0), Err(Reject::Rate));
        assert_eq!(validate(&console("additem"), &mut g, 1_000), Ok(()));
        assert_eq!(validate(&console("additem"), &mut g, 1_000), Err(Reject::Rate));
        // the server's own view of a message carries no rate
        assert_eq!(validate_server(&console("additem")), Ok(()));
    }

    fn shot(power: f32, aim_angle: f32) -> Message {
        Message::PlayerBowShot(skymp::PlayerBowShot { weapon_id: 0x3b562, ammo_id: 0x1397d, power, aim_angle, ..Default::default() })
    }

    #[test]
    fn a_bow_shot_carries_a_draw_and_an_aim_within_bounds() {
        let mut g = ClientGuard::default();
        assert_eq!(validate(&shot(1.0, 0.3), &mut g, 0), Ok(()));
        assert_eq!(validate(&shot(0.0, -1.5), &mut g, 0), Ok(()));
        for (power, aim) in [(1.5, 0.0), (-0.1, 0.0), (0.5, 2.0), (0.5, -2.0)] {
            assert_eq!(validate(&shot(power, aim), &mut ClientGuard::default(), 0), Err(Reject::Range), "{power} {aim}");
        }
        assert_eq!(validate(&shot(f32::NAN, 0.0), &mut ClientGuard::default(), 0), Err(Reject::NonFinite));
        assert_eq!(validate(&shot(1.0, f32::INFINITY), &mut ClientGuard::default(), 0), Err(Reject::NonFinite));
    }

    #[test]
    fn a_bow_shot_names_its_shooter_by_a_real_idx() {
        // the sender's own idx for its own shot, a hosted actor's for one its
        // game simulates (thuum docs/verbs/npc-stream.md, train 53); 0 is a
        // real actor, u32::MAX none
        let own = Message::PlayerBowShot(skymp::PlayerBowShot { idx: 7, weapon_id: 0x3b562, ammo_id: 0x1397d, power: 1.0, ..Default::default() });
        assert_eq!(validate(&own, &mut ClientGuard::default(), 0), Ok(()));
        let none = Message::PlayerBowShot(skymp::PlayerBowShot { idx: u32::MAX, weapon_id: 0x3b562, ammo_id: 0x1397d, power: 1.0, ..Default::default() });
        assert_eq!(validate(&none, &mut ClientGuard::default(), 0), Err(Reject::Range));
    }

    // a shot with the launch its game made, from (x, y, z) at pitch angle_x
    fn launched(x: f32, y: f32, z: f32, angle_x: f32) -> Message {
        Message::PlayerBowShot(skymp::PlayerBowShot {
            weapon_id: 0x3b562,
            ammo_id: 0x1397d,
            power: 1.0,
            aim_angle: 0.1,
            aim_heading: 1.57,
            has_launch: true,
            origin_x: x,
            origin_y: y,
            origin_z: z,
            launch_angle_x: angle_x,
            launch_angle_z: 1.57,
            ..Default::default()
        })
    }

    #[test]
    fn a_bow_shot_carries_its_launch_from_inside_the_world_at_a_pitch_within_bounds() {
        assert_eq!(validate(&launched(1234.5, -300.25, 96.0, 0.1), &mut ClientGuard::default(), 0), Ok(()));
        assert_eq!(validate(&launched(-WORLD_ABS_MAX, WORLD_ABS_MAX, 0.0, -1.5), &mut ClientGuard::default(), 0), Ok(()));
        for (x, y, z) in [(1.5e6, 0.0, 96.0), (1234.5, -1.0e7, 96.0), (1234.5, -300.25, 2.0e6)] {
            assert_eq!(validate(&launched(x, y, z, 0.1), &mut ClientGuard::default(), 0), Err(Reject::OutOfWorld), "{x} {y} {z}");
        }
        for angle_x in [2.0, -2.0, core::f32::consts::PI] {
            assert_eq!(validate(&launched(1234.5, -300.25, 96.0, angle_x), &mut ClientGuard::default(), 0), Err(Reject::Range), "{angle_x}");
        }
        assert_eq!(validate(&launched(f32::NAN, -300.25, 96.0, 0.1), &mut ClientGuard::default(), 0), Err(Reject::NonFinite));
    }

    #[test]
    fn a_bow_shot_without_its_launch_carries_zeros() {
        // shot() carries none: has_launch false, origin and angles zero
        assert_eq!(validate(&shot(1.0, 0.3), &mut ClientGuard::default(), 0), Ok(()));
        let none = skymp::PlayerBowShot { weapon_id: 0x3b562, ammo_id: 0x1397d, power: 1.0, aim_angle: 0.1, ..Default::default() };
        for m in [
            skymp::PlayerBowShot { origin_x: 1234.5, ..none.clone() },
            skymp::PlayerBowShot { origin_y: -300.25, ..none.clone() },
            skymp::PlayerBowShot { origin_z: 96.0, ..none.clone() },
            skymp::PlayerBowShot { launch_angle_x: 0.1, ..none.clone() },
            skymp::PlayerBowShot { launch_angle_z: 1.57, ..none.clone() },
        ] {
            assert_eq!(validate(&Message::PlayerBowShot(m.clone()), &mut ClientGuard::default(), 0), Err(Reject::Range), "{m:?}");
        }
    }

    #[test]
    fn an_arrow_shot_carries_its_shots_launch_under_the_same_rule() {
        let relay = skymp::ArrowShot {
            idx: 2,
            weapon_id: 0x3b562,
            ammo_id: 0x1397d,
            power: 1.0,
            aim_angle: 0.1,
            aim_heading: 1.57,
            has_launch: true,
            origin_x: 1234.5,
            origin_y: -300.25,
            origin_z: 96.0,
            launch_angle_x: 0.1,
            launch_angle_z: 1.57,
            ..Default::default()
        };
        assert_eq!(validate_server(&Message::ArrowShot(relay.clone())), Ok(()));
        assert_eq!(validate_server(&Message::ArrowShot(skymp::ArrowShot { idx: 2, ..Default::default() })), Ok(()));
        assert_eq!(validate_server(&Message::ArrowShot(skymp::ArrowShot { origin_y: -1.0e7, ..relay.clone() })), Err(Reject::OutOfWorld));
        assert_eq!(validate_server(&Message::ArrowShot(skymp::ArrowShot { launch_angle_x: 2.0, ..relay.clone() })), Err(Reject::Range));
        assert_eq!(validate_server(&Message::ArrowShot(skymp::ArrowShot { has_launch: false, ..relay })), Err(Reject::Range));
    }

    fn spell_cast(spell: u32, casting_source: i32, aim_angle: f32) -> Message {
        Message::SpellCast(skymp::SpellCast {
            data: skymp::SpellCastData { spell, casting_source, aim_angle, ..Default::default() },
            ..Default::default()
        })
    }

    #[test]
    fn a_cast_names_a_spell_a_known_hand_and_an_aim_within_bounds() {
        let mut g = ClientGuard::default();
        assert_eq!(validate(&spell_cast(0x12fcd, 1, 0.3), &mut g, 0), Ok(()));
        assert_eq!(validate(&spell_cast(0x12fcd, 3, -1.5), &mut g, 0), Ok(()));
        for (spell, source, aim) in [(0, 1, 0.0), (0x12fcd, 4, 0.0), (0x12fcd, -1, 0.0), (0x12fcd, 0, 2.0)] {
            assert_eq!(validate(&spell_cast(spell, source, aim), &mut ClientGuard::default(), 0), Err(Reject::Range), "{spell} {source} {aim}");
        }
        assert_eq!(validate(&spell_cast(0x12fcd, 1, f32::NAN), &mut ClientGuard::default(), 0), Err(Reject::NonFinite));
    }

    // a cast with the launch its game made, from (x, y, z) at pitch angle_x
    fn cast_launched(x: f32, y: f32, z: f32, angle_x: f32) -> Message {
        Message::SpellCast(skymp::SpellCast {
            data: skymp::SpellCastData {
                spell: 0x12fd0,
                casting_source: 1,
                aim_angle: 0.1,
                aim_heading: 1.57,
                has_launch: true,
                origin_x: x,
                origin_y: y,
                origin_z: z,
                launch_angle_x: angle_x,
                launch_angle_z: 1.57,
                ..Default::default()
            },
            ..Default::default()
        })
    }

    // the cast's decal (docs/verbs/spell-cast.md, the rune's glyph): with its launch, a unit normal
    fn cast_with_decal(has_launch: bool, nx: f32, ny: f32, nz: f32) -> Message {
        Message::SpellCast(skymp::SpellCast {
            data: skymp::SpellCastData {
                spell: 0x6796f,
                casting_source: 1,
                aim_angle: 0.1,
                has_launch,
                origin_x: if has_launch { 1234.5 } else { 0.0 },
                origin_y: if has_launch { -300.25 } else { 0.0 },
                origin_z: if has_launch { 96.0 } else { 0.0 },
                launch_angle_x: 0.0,
                launch_angle_z: if has_launch { 1.57 } else { 0.0 },
                decal: Some(skymp::SpellDecal { normal_x: nx, normal_y: ny, normal_z: nz, material_id: 0xe4d39ca3 }),
                ..Default::default()
            },
            ..Default::default()
        })
    }

    #[test]
    fn a_cast_s_decal_rides_its_launch_with_a_unit_normal() {
        assert_eq!(validate(&cast_with_decal(true, 0.0, 0.0, 1.0), &mut ClientGuard::default(), 0), Ok(()));
        assert_eq!(validate(&cast_with_decal(true, 0.083, -0.996, 0.0), &mut ClientGuard::default(), 0), Ok(()));
        // the normal the caster's engine hands the placement is scaled to 69.99: the client unitizes it, the wire refuses it raw
        assert_eq!(validate(&cast_with_decal(true, 5.83, -69.75, 0.0), &mut ClientGuard::default(), 0), Err(Reject::Range));
        assert_eq!(validate(&cast_with_decal(true, 0.0, 0.0, 0.0), &mut ClientGuard::default(), 0), Err(Reject::Range));
        assert_eq!(validate(&cast_with_decal(true, f32::NAN, 0.0, 1.0), &mut ClientGuard::default(), 0), Err(Reject::NonFinite));
        // a decal without the launch it belongs to
        assert_eq!(validate(&cast_with_decal(false, 0.0, 0.0, 1.0), &mut ClientGuard::default(), 0), Err(Reject::Range));
    }

    #[test]
    fn a_cast_carries_its_launch_from_inside_the_world_at_a_pitch_within_bounds() {
        assert_eq!(validate(&cast_launched(1234.5, -300.25, 96.0, 0.1), &mut ClientGuard::default(), 0), Ok(()));
        assert_eq!(validate(&cast_launched(-WORLD_ABS_MAX, WORLD_ABS_MAX, 0.0, -1.5), &mut ClientGuard::default(), 0), Ok(()));
        for (x, y, z) in [(1.5e6, 0.0, 96.0), (1234.5, -1.0e7, 96.0), (1234.5, -300.25, 2.0e6)] {
            assert_eq!(validate(&cast_launched(x, y, z, 0.1), &mut ClientGuard::default(), 0), Err(Reject::OutOfWorld), "{x} {y} {z}");
        }
        for angle_x in [2.0, -2.0, core::f32::consts::PI] {
            assert_eq!(validate(&cast_launched(1234.5, -300.25, 96.0, angle_x), &mut ClientGuard::default(), 0), Err(Reject::Range), "{angle_x}");
        }
        assert_eq!(validate(&cast_launched(f32::NAN, -300.25, 96.0, 0.1), &mut ClientGuard::default(), 0), Err(Reject::NonFinite));
        // its relay, server to client, under the same rule
        assert_eq!(validate_server(&cast_launched(1234.5, -300.25, 96.0, 0.1)), Ok(()));
        assert_eq!(validate_server(&cast_launched(1234.5, -1.0e7, 96.0, 0.1)), Err(Reject::OutOfWorld));
    }

    #[test]
    fn a_cast_without_its_launch_carries_zeros() {
        // spell_cast() carries none: has_launch false, origin and angles zero
        assert_eq!(validate(&spell_cast(0x12fd0, 1, 0.3), &mut ClientGuard::default(), 0), Ok(()));
        let none = skymp::SpellCastData { spell: 0x12fd0, casting_source: 1, aim_angle: 0.1, ..Default::default() };
        for d in [
            skymp::SpellCastData { origin_x: 1234.5, ..none.clone() },
            skymp::SpellCastData { origin_y: -300.25, ..none.clone() },
            skymp::SpellCastData { origin_z: 96.0, ..none.clone() },
            skymp::SpellCastData { launch_angle_x: 0.1, ..none.clone() },
            skymp::SpellCastData { launch_angle_z: 1.57, ..none.clone() },
        ] {
            let m = Message::SpellCast(skymp::SpellCast { data: d, ..Default::default() });
            assert_eq!(validate(&m, &mut ClientGuard::default(), 0), Err(Reject::Range), "{m:?}");
            assert_eq!(validate_server(&m), Err(Reject::Range), "{m:?}");
        }
    }

    #[test]
    fn casts_eight_at_once_then_four_a_second() {
        let mut g = ClientGuard::default();
        for _ in 0..8 {
            assert_eq!(validate(&spell_cast(0x12fcd, 1, 0.0), &mut g, 0), Ok(()));
        }
        assert_eq!(validate(&spell_cast(0x12fcd, 1, 0.0), &mut g, 0), Err(Reject::Rate));
        for _ in 0..4 {
            assert_eq!(validate(&spell_cast(0x12fcd, 1, 0.0), &mut g, 1_000), Ok(()));
        }
        assert_eq!(validate(&spell_cast(0x12fcd, 1, 0.0), &mut g, 1_000), Err(Reject::Rate));
    }

    #[test]
    fn shots_four_at_once_then_two_a_second() {
        let mut g = ClientGuard::default();
        for _ in 0..4 {
            assert_eq!(validate(&shot(1.0, 0.0), &mut g, 0), Ok(()));
        }
        assert_eq!(validate(&shot(1.0, 0.0), &mut g, 0), Err(Reject::Rate));
        assert_eq!(validate(&shot(1.0, 0.0), &mut g, 1_000), Ok(()));
        assert_eq!(validate(&shot(1.0, 0.0), &mut g, 1_000), Ok(()));
        assert_eq!(validate(&shot(1.0, 0.0), &mut g, 1_000), Err(Reject::Rate));
    }

    #[test]
    fn hit_reports_thirty_two_at_once_then_sixteen_a_second() {
        let hit = Message::OnHit(skymp::OnHit::default());
        let mut g = ClientGuard::default();
        for _ in 0..32 {
            assert_eq!(validate(&hit, &mut g, 0), Ok(()));
        }
        assert_eq!(validate(&hit, &mut g, 0), Err(Reject::Rate));
        assert_eq!(validate(&hit, &mut g, 1_000), Ok(()));
        // the server's own view of a message carries no rate
        assert_eq!(validate_server(&hit), Ok(()));
    }

    fn preset(actor: u32, text: &str) -> Message {
        Message::RaceMenuPreset(skymp::RaceMenuPreset {
            actor,
            preset: text.try_into().unwrap_or_default(),
            ..Default::default()
        })
    }

    #[test]
    fn presets_both_ways() {
        assert_eq!(validate(&preset(0, "{}"), &mut ClientGuard::default(), 0), Ok(()));
        // another player's look, or none, from a client
        assert_eq!(validate(&preset(0xff00_0001, "{}"), &mut ClientGuard::default(), 0), Err(Reject::Range));
        assert_eq!(validate(&preset(0, ""), &mut ClientGuard::default(), 0), Err(Reject::Range));
        let mut g = ClientGuard::default();
        for _ in 0..2 {
            assert_eq!(validate(&preset(0, "{}"), &mut g, 0), Ok(()));
        }
        assert_eq!(validate(&preset(0, "{}"), &mut g, 0), Err(Reject::Rate));
        assert_eq!(validate(&preset(0, "{}"), &mut g, 1_000), Ok(()));
        // from the server: whose look it is, and a burst at a login is fine
        for _ in 0..16 {
            assert_eq!(validate_server(&preset(0xff00_0001, "{}")), Ok(()));
        }
        assert_eq!(validate_server(&preset(0xff00_0001, "")), Err(Reject::Range));
    }

    fn actor_values(avs: &[u8], skills: &[u8], legendary: &[u8], base: f32) -> Message {
        Message::ActorValues(skymp::ActorValues {
            bases: avs
                .iter()
                .map(|&av| skymp::ActorValueBase { av, base })
                .collect::<std::vec::Vec<_>>()
                .try_into()
                .unwrap_or_default(),
            skills: skills
                .iter()
                .map(|&skill| skymp::SkillProgress { skill, level: 15.0, xp: 0.0, threshold: 10.0 })
                .collect::<std::vec::Vec<_>>()
                .try_into()
                .unwrap_or_default(),
            xp: 0.0,
            threshold: 75.0,
            level: 1,
            legendary: legendary
                .iter()
                .map(|&skill| skymp::LegendarySkill { skill, count: 1 })
                .collect::<std::vec::Vec<_>>()
                .try_into()
                .unwrap_or_default(),
            ..Default::default()
        })
    }

    #[test]
    fn actor_values_both_ways() {
        assert_eq!(validate_server(&actor_values(&[6, 24, 163], &[0, 17], &[3], 15.0)), Ok(()));
        for bad in [
            actor_values(&[164], &[], &[], 1.0),
            actor_values(&[6, 6], &[], &[], 1.0),
            actor_values(&[], &[18], &[], 1.0),
            actor_values(&[], &[2, 2], &[], 1.0),
            actor_values(&[], &[], &[18], 1.0),
        ] {
            assert_eq!(validate_server(&bad), Err(Reject::Range), "{bad:?}");
        }
        assert_eq!(validate_server(&actor_values(&[24], &[], &[], f32::NAN)), Err(Reject::NonFinite));
        let mut g = ClientGuard::default();
        for _ in 0..8 {
            assert_eq!(validate(&actor_values(&[6], &[], &[], 1.0), &mut g, 0), Ok(()));
        }
        assert_eq!(validate(&actor_values(&[6], &[], &[], 1.0), &mut g, 0), Err(Reject::Rate));
    }

    #[test]
    fn equips_have_a_budget_that_only_stops_a_flood() {
        use wire_schema::skymp::OnEquip;
        let drink = Message::OnEquip(OnEquip { base_id: 0x3eadd, ..OnEquip::default() });
        let mut g = ClientGuard::default();
        for _ in 0..16 {
            assert_eq!(validate(&drink, &mut g, 0), Ok(()));
        }
        assert_eq!(validate(&drink, &mut g, 0), Err(Reject::Rate));
        // a second later, eight more
        for _ in 0..8 {
            assert_eq!(validate(&drink, &mut g, 1_000), Ok(()));
        }
        assert_eq!(validate(&drink, &mut g, 1_000), Err(Reject::Rate));
    }

    fn player_globals(watch: &[u32], values: &[(u32, f32)]) -> Message {
        use wire_schema::skymp::{GlobalEntry, PlayerGlobals};
        let mut m = PlayerGlobals::default();
        for w in watch {
            if m.watch.push(*w).is_err() {
                return Message::PlayerGlobals(PlayerGlobals::default());
            }
        }
        for (global, value) in values {
            if m.values.push(GlobalEntry { global: *global, value: *value }).is_err() {
                return Message::PlayerGlobals(PlayerGlobals::default());
            }
        }
        Message::PlayerGlobals(m)
    }

    #[test]
    fn player_globals_each_once_finite_and_watched_only_from_the_server() {
        // the Craftlord's unspent points and the MCM's general XP multiplier
        // at Apocalypse's slot 0C (thuum docs/verbs/player-globals.md)
        let (points, xp) = (0x0c0b_1c19, 0x0c04_3cef);
        let mut g = ClientGuard::default();
        assert_eq!(validate(&player_globals(&[], &[(points, 3.0), (xp, 1.5)]), &mut g, 0), Ok(()));
        assert_eq!(validate(&player_globals(&[], &[(points, 3.0), (points, 1.0)]), &mut g, 0), Err(Reject::Range));
        assert_eq!(validate(&player_globals(&[], &[(points, f32::NAN)]), &mut g, 0), Err(Reject::NonFinite));
        // a client names nothing to watch
        assert_eq!(validate(&player_globals(&[points], &[]), &mut g, 0), Err(Reject::Range));
        // the server's login message: the list and the record
        assert_eq!(validate_server(&player_globals(&[points, xp], &[(points, 3.0)])), Ok(()));
        assert_eq!(validate_server(&player_globals(&[points, points], &[])), Err(Reject::Range));
        // one report a second after the first two
        let mut g = ClientGuard::default();
        assert_eq!(validate(&player_globals(&[], &[(points, 1.0)]), &mut g, 0), Ok(()));
        assert_eq!(validate(&player_globals(&[], &[(points, 2.0)]), &mut g, 0), Ok(()));
        assert_eq!(validate(&player_globals(&[], &[(points, 3.0)]), &mut g, 0), Err(Reject::Rate));
        assert_eq!(validate(&player_globals(&[], &[(points, 3.0)]), &mut g, 1_000), Ok(()));
    }

    #[test]
    fn an_action_names_its_action_and_event_and_keeps_its_type_a_byte() {
        use wire_schema::skymp::UpdateAction;
        // Flames' start as the engine performs it (thuum docs/verbs/cast-action.md):
        // ActionRightAttack 0x13005, idle CastRightConcentrationAimed 0x1346F,
        // MRh_SpellAimedConcentrationStart, type 2 (x-figure-action 20261015-055039)
        let action = |action: u32, event: &str, kind: u32| {
            let Ok(event_name) = wire_schema::bounded::String::try_from(event) else {
                return Message::UpdateAction(UpdateAction::default());
            };
            Message::UpdateAction(UpdateAction { idx: 64, action, idle: 0x1346f, event_name, kind, ..UpdateAction::default() })
        };
        assert_eq!(validate_server(&action(0x13005, "MRh_SpellAimedConcentrationStart", 2)), Ok(()));
        assert_eq!(validate_server(&action(0, "MRh_SpellAimedConcentrationStart", 2)), Err(Reject::Range));
        assert_eq!(validate_server(&action(0x13005, "", 2)), Err(Reject::Range));
        assert_eq!(validate_server(&action(0x13005, "MRh_SpellAimedConcentrationStart", 256)), Err(Reject::Range));
    }

    #[test]
    fn actions_are_rate_limited() {
        use wire_schema::skymp::UpdateAction;
        let mut g = ClientGuard::default();
        let Ok(event_name) = wire_schema::bounded::String::try_from("MRh_SpellAimedConcentrationStart") else {
            return;
        };
        let m = Message::UpdateAction(UpdateAction { idx: 64, action: 0x13005, event_name, kind: 2, ..UpdateAction::default() });
        // 48 at once, 16 a second after that: a host's budget (thuum
        // docs/verbs/npc-stream.md, UNMEASURED from x-npc-stream-p1's rate)
        for _ in 0..48 {
            assert_eq!(validate(&m, &mut g, 1_000), Ok(()));
        }
        assert_eq!(validate(&m, &mut g, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&m, &mut g, 2_000), Ok(()));
    }

    #[test]
    fn a_magic_effects_set_with_a_number_that_is_not_finite_is_refused() {
        use wire_schema::skymp::{MagicEffect, MagicEffects};
        // DB03Poison's effect, 3.5 s into its 10 (thuum docs/verbs/magic-effects.md)
        let set = |remaining: f32| {
            let mut m = MagicEffects { idx: 64, ..MagicEffects::default() };
            let entry = MagicEffect { effect: 0x10aa4a, source: 0x58cfb, magnitude: 6.0, remaining, caster: 0 };
            if m.effects.push(entry).is_err() {
                return Message::MagicEffects(MagicEffects::default());
            }
            Message::MagicEffects(m)
        };
        assert_eq!(validate_server(&set(6.5)), Ok(()));
        assert_eq!(validate_server(&set(f32::NAN)), Err(Reject::NonFinite));
    }

    // thuum docs/verbs/npc-fate.md C4 (ADR-060's return): an effect names
    // its caster, any form id or none; Dead Thrall's state on Sven, no clock
    // (-1), cast by a player's actor, and a potion's with no caster
    #[test]
    fn a_magic_effects_caster_is_any_form_id_or_none() {
        use wire_schema::skymp::{MagicEffect, MagicEffects};
        let set = |caster: u32| {
            let mut m = MagicEffects { idx: 64, ..MagicEffects::default() };
            let entry = MagicEffect { effect: 0x7e8e0, source: 0x7e8df, magnitude: 999.0, remaining: -1.0, caster };
            if m.effects.push(entry).is_err() {
                return Message::MagicEffects(MagicEffects::default());
            }
            Message::MagicEffects(m)
        };
        for caster in [0, 0x13491, 0xff00_0000, u32::MAX] {
            assert_eq!(validate_server(&set(caster)), Ok(()), "{caster:x}");
        }
    }

    #[test]
    fn a_magic_effects_sum_names_an_actor_value_the_engine_has() {
        use wire_schema::skymp::{EffectModifier, MagicEffects};
        // FortifyHealth05's 100 on Health (actor value 24)
        let sum = |av: u32, amount: f32| {
            let mut m = MagicEffects { idx: 64, ..MagicEffects::default() };
            if m.modifiers.push(EffectModifier { av, amount }).is_err() {
                return Message::MagicEffects(MagicEffects::default());
            }
            Message::MagicEffects(m)
        };
        assert_eq!(validate_server(&sum(24, 100.0)), Ok(()));
        assert_eq!(validate_server(&sum(163, -20.0)), Ok(()));
        assert_eq!(validate_server(&sum(164, 100.0)), Err(Reject::Range));
        assert_eq!(validate_server(&sum(24, f32::INFINITY)), Err(Reject::NonFinite));
    }

    #[test]
    fn marker_both_ways() {
        let mut g = ClientGuard::default();
        for t in [0, 1, 5, MARKER_TYPE_MAX] {
            assert_eq!(validate(&marker(t), &mut g, 0), Ok(()), "{t}");
        }
        // four at once, then one a second
        assert_eq!(validate(&marker(1), &mut g, 0), Err(Reject::Rate));
        assert_eq!(validate(&marker(1), &mut g, 1_000), Ok(()));
        for bad in [60, 61, 64, 65, u16::MAX] {
            assert_eq!(validate(&marker(bad), &mut ClientGuard::default(), 0), Err(Reject::Range), "{bad}");
        }
    }

    #[test]
    fn replay_window_edges() {
        let mut w = ReplayWindow::default();
        assert!(w.accept(0));
        assert!(!w.accept(0));
        assert!(w.accept(u32::MAX));
        assert!(w.accept(u32::MAX - 63));
        assert!(!w.accept(u32::MAX - 64));
        assert!(!w.accept(0));
    }

    #[test]
    fn token_bucket_carries_fractions() {
        let mut b = TokenBucket {
            tokens: 0,
            capacity: 60,
            refill_per_s: 60,
            last_refill_ms: 0,
        };
        assert!(!b.take(10), "10 ms at 60/s is under one token");
        assert!(b.take(17), "17 ms is one token");
        assert!(!b.take(17), "and it is spent");
        assert!(
            b.take(34),
            "the 1 ms remainder from before carries into the next token"
        );
    }

    // thuum docs/verbs/cell-host.md: a game's loaded set crosses as
    // CellLoaded under a 128-then-32-a-second budget; the grants and releases
    // are the server's, each naming a cell and an epoch after the first
    fn cell_loaded(cell: u32) -> Message {
        use wire_schema::skymp::CellLoaded;
        Message::CellLoaded(CellLoaded { cell, loaded: true, ..Default::default() })
    }

    #[test]
    fn cell_loaded_has_the_grid_load_budget_and_names_a_cell() {
        let mut g = ClientGuard::default();
        // the save's load is about 30 reports and a teleport about 50 (25
        // unloads and 25 loads); two teleports inside five seconds fit the
        // burst of 128 (b-cell-host 20261021-184501 measured them)
        for i in 0..128u32 {
            assert_eq!(validate(&cell_loaded(0x9732 + i), &mut g, 0), Ok(()));
        }
        assert_eq!(validate(&cell_loaded(0x9732), &mut g, 0), Err(Reject::Rate));
        // a second later, thirty-two more
        for _ in 0..32 {
            assert_eq!(validate(&cell_loaded(0x9732), &mut g, 1_000), Ok(()));
        }
        assert_eq!(validate(&cell_loaded(0x9732), &mut g, 1_000), Err(Reject::Rate));
        // a cell of 0 names nothing
        assert_eq!(validate(&cell_loaded(0), &mut ClientGuard::default(), 0), Err(Reject::Range));
    }

    // thuum docs/verbs/quest-stage.md (ADR-056, memo D58 B): a seat's report
    // names one quest and no list; the login's message names the mode and
    // its list
    fn quest_stage(quest: u32, stage: u16, shared: &[u32], mode: u8) -> Message {
        use wire_schema::skymp::QuestStage;
        let mut m = QuestStage { quest, stage, mode, ..Default::default() };
        for q in shared {
            if m.shared.push(*q).is_err() {
                return Message::QuestStage(QuestStage::default());
            }
        }
        Message::QuestStage(m)
    }

    #[test]
    fn quest_stages_name_one_quest_from_a_client_and_the_mode_from_the_server() {
        // MS13Intro 0xb6031 and MQ102 0x4e50d (Skyrim.esm, thuum lab/esm.py)
        let mut g = ClientGuard::default();
        assert_eq!(validate(&quest_stage(0xb6031, 10, &[], 0), &mut g, 0), Ok(()));
        // a client names no list, no quest 0 and no mode
        assert_eq!(validate(&quest_stage(0xb6031, 10, &[0xb6031], 0), &mut g, 0), Err(Reject::Range));
        assert_eq!(validate(&quest_stage(0, 10, &[], 0), &mut g, 0), Err(Reject::Range));
        assert_eq!(validate(&quest_stage(0xb6031, 10, &[], 1), &mut g, 0), Err(Reject::Range));
        // the server's login message: mode list needs its list, mode all
        // carries the denied list, empty or not; no third mode
        assert_eq!(validate_server(&quest_stage(0, 0, &[0xb6031], 0)), Ok(()));
        assert_eq!(validate_server(&quest_stage(0, 0, &[], 0)), Err(Reject::Range));
        assert_eq!(validate_server(&quest_stage(0, 0, &[0x4e50d], 1)), Ok(()));
        assert_eq!(validate_server(&quest_stage(0, 0, &[], 1)), Ok(()));
        assert_eq!(validate_server(&quest_stage(0, 0, &[], 2)), Err(Reject::Range));
        // the list names each quest once and never 0
        assert_eq!(validate_server(&quest_stage(0, 0, &[0x4e50d, 0x4e50d], 1)), Err(Reject::Range));
        assert_eq!(validate_server(&quest_stage(0, 0, &[0], 0)), Err(Reject::Range));
        // thirty-two at once, then eight a second (UNMEASURED, mode all's
        // load fires a dozen quest starts)
        let mut g = ClientGuard::default();
        for _ in 0..32 {
            assert_eq!(validate(&quest_stage(0xb6031, 10, &[], 0), &mut g, 0), Ok(()));
        }
        assert_eq!(validate(&quest_stage(0xb6031, 10, &[], 0), &mut g, 0), Err(Reject::Rate));
        for _ in 0..8 {
            assert_eq!(validate(&quest_stage(0xb6031, 10, &[], 0), &mut g, 1_000), Ok(()));
        }
        assert_eq!(validate(&quest_stage(0xb6031, 10, &[], 0), &mut g, 1_000), Err(Reject::Rate));
    }

    // thuum docs/verbs/npc-puppet.md: a host's roster names game-file actors
    // in a named world or cell at a finite position inside the world
    fn sightings(entries: &[(u32, u32, [f32; 3])]) -> Message {
        use wire_schema::skymp::{ActorSighting, ActorSightings};
        Message::ActorSightings(ActorSightings {
            sightings: entries
                .iter()
                .map(|&(refr_id, world_or_cell, pos)| ActorSighting { refr_id, world_or_cell, pos, rot: [0.0, 0.0, 165.3] })
                .collect::<Vec<_>>()
                .try_into()
                .unwrap_or_default(),
            ..Default::default()
        })
    }

    // thuum docs/verbs/npc-fate.md (ADR-048's B): a host's death report names
    // a game-file actor, a cause the contract knows and a point in the world;
    // six at once, one a second after that
    fn death(refr_id: u32, cause: u8, killer: u32, pos: [f32; 3]) -> Message {
        Message::ActorDeath(wire_schema::skymp::ActorDeath { refr_id, cause, killer, pos, ..Default::default() })
    }

    #[test]
    fn a_death_report_names_an_actor_a_cause_and_a_point() {
        assert_eq!(validate_server(&death(0x85547, 1, 0, [123592.5, -80301.25, 10705.0])), Ok(()));
        assert_eq!(validate_server(&death(0x85547, 4, 0x85549, [0.0, 0.0, 0.0])), Ok(()));
        assert_eq!(validate_server(&death(0, 1, 0, [0.0, 0.0, 0.0])), Err(Reject::Range));
        // a server-made actor's death, reported by its host (finding 47)
        assert_eq!(validate_server(&death(0xff00_0004, 4, 0xff00_0000, [0.0, 0.0, 0.0])), Ok(()));
        assert_eq!(validate_server(&death(0x85547, 5, 0, [0.0, 0.0, 0.0])), Err(Reject::Range));
        assert_eq!(validate_server(&death(0x85547, 1, 0, [f32::NAN, 0.0, 0.0])), Err(Reject::NonFinite));
        let mut guard = ClientGuard::default();
        for _ in 0..6 {
            assert_eq!(validate(&death(0x85547, 1, 0, [0.0, 0.0, 0.0]), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&death(0x85547, 1, 0, [0.0, 0.0, 0.0]), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&death(0x85547, 1, 0, [0.0, 0.0, 0.0]), &mut guard, 2_100), Ok(()));
    }

    // thuum docs/verbs/npc-stream.md (ADR-050): a host's state for an actor
    // it simulates names the actor's index and three percentages within 0
    // and 1; 128 at once, 32 a second after that (UNMEASURED until P3)
    #[test]
    fn a_hosted_states_gaze_and_face_are_bounded() {
        use wire_schema::skymp::HostedActorState;
        let face = |gaze_slot, emotion_type, emotion_value| {
            Message::HostedActorState(HostedActorState { idx: 7, health: 1.0, magicka: 1.0, stamina: 1.0, gaze_target: 0x13491, gaze_slot, emotion_type, emotion_value, ..Default::default() })
        };
        assert_eq!(validate_server(&face(4, 5, 100)), Ok(()));
        assert_eq!(validate_server(&face(0, 0, 0)), Ok(()));
        assert_eq!(validate_server(&face(6, 0, 0)), Err(Reject::Range));
        assert_eq!(validate_server(&face(0, 8, 0)), Err(Reject::Range));
        assert_eq!(validate_server(&face(0, 0, 101)), Err(Reject::Range));
    }

    fn hosted_state(idx: u32, health: f32, magicka: f32, stamina: f32) -> Message {
        Message::HostedActorState(wire_schema::skymp::HostedActorState {
            idx,
            health,
            magicka,
            stamina,
            combat_target: 0xff00_0000,
            package: 0x4_0000,
            ..Default::default()
        })
    }

    #[test]
    fn a_hosted_state_names_an_actor_and_its_values() {
        assert_eq!(validate_server(&hosted_state(7, 0.5, 1.0, 0.75)), Ok(()));
        assert_eq!(validate_server(&hosted_state(7, 0.0, 0.0, 0.0)), Ok(()));
        assert_eq!(validate_server(&hosted_state(7, 1.01, 1.0, 1.0)), Err(Reject::Range));
        assert_eq!(validate_server(&hosted_state(7, 1.0, -0.01, 1.0)), Err(Reject::Range));
        assert_eq!(validate_server(&hosted_state(7, 1.0, 1.0, 2.0)), Err(Reject::Range));
        assert_eq!(validate_server(&hosted_state(u32::MAX, 1.0, 1.0, 1.0)), Err(Reject::Range));
        assert_eq!(validate_server(&hosted_state(7, f32::NAN, 1.0, 1.0)), Err(Reject::NonFinite));
        let mut guard = ClientGuard::default();
        for _ in 0..128 {
            assert_eq!(validate(&hosted_state(7, 0.5, 1.0, 1.0), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&hosted_state(7, 0.5, 1.0, 1.0), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&hosted_state(7, 0.5, 1.0, 1.0), &mut guard, 1_100), Ok(()));
    }

    // thuum docs/verbs/npc-stream.md (the voice slice): a host's spoken line
    // names the actor's index, a topic and an info; 32 at once, 8 a second
    fn speech(idx: u32, topic: u32, info: u32) -> Message {
        Message::ActorSpeech(wire_schema::skymp::ActorSpeech { idx, topic, info, ..Default::default() })
    }

    #[test]
    fn a_spoken_line_names_an_actor_a_topic_and_an_info() {
        assert_eq!(validate_server(&speech(7, 0x2_0000, 0x2_0001)), Ok(()));
        assert_eq!(validate_server(&speech(u32::MAX, 0x2_0000, 0x2_0001)), Err(Reject::Range));
        assert_eq!(validate_server(&speech(7, 0, 0x2_0001)), Err(Reject::Range));
        assert_eq!(validate_server(&speech(7, 0x2_0000, 0)), Err(Reject::Range));
        let mut guard = ClientGuard::default();
        for _ in 0..32 {
            assert_eq!(validate(&speech(7, 0x2_0000, 0x2_0001), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&speech(7, 0x2_0000, 0x2_0001), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&speech(7, 0x2_0000, 0x2_0001), &mut guard, 1_200), Ok(()));
    }

    // thuum docs/verbs/npc-fate.md (ADR-049): the server's fates name
    // game-file placements; a base of 0 is a roll of none; a worn id is a form
    fn fates(entries: &[(u32, u32, u16, &[u32])]) -> Message {
        use wire_schema::skymp::{ActorFate, ActorFateEntry};
        Message::ActorFate(ActorFate {
            fates: entries
                .iter()
                .map(|&(refr_id, base_id, level, worn)| ActorFateEntry {
                    refr_id,
                    base_id,
                    level,
                    worn: worn.to_vec().try_into().unwrap_or_default(),
                    ..Default::default()
                })
                .collect::<Vec<_>>()
                .try_into()
                .unwrap_or_default(),
            ..Default::default()
        })
    }

    #[test]
    fn actor_claims_name_an_actor_and_are_rate_limited() {
        use wire_schema::skymp::ActorClaim;
        let claim = |idx, claim| Message::ActorClaim(ActorClaim { idx, claim, ..Default::default() });
        assert_eq!(validate_server(&claim(7, true)), Ok(()));
        assert_eq!(validate_server(&claim(7, false)), Ok(()));
        assert_eq!(validate_server(&claim(u32::MAX, true)), Err(Reject::Range));
        let mut guard = ClientGuard::default();
        for _ in 0..8 {
            assert_eq!(validate(&claim(7, true), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&claim(7, true), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&claim(7, false), &mut guard, 1_500), Ok(()));
    }

    // thuum docs/verbs/prop-physics.md (ADR-058): a simulator's prop sample
    // names a reference, a finite point in the world and a finite rotation;
    // 128 at once, 96 a second after that (UNMEASURED; raised from 20 and 10
    // for a trap's bodies, thuum D-S3 point 4)
    fn prop(refr: u32, pos: [f32; 3], rot: [f32; 3], asleep: bool) -> Message {
        Message::PropTransform(wire_schema::skymp::PropTransform { refr, pos, rot, asleep, ..Default::default() })
    }

    #[test]
    fn a_prop_sample_names_a_reference_at_a_finite_place_and_is_rate_limited() {
        let basket = [23877.5, -45485.25, 8.0];
        assert_eq!(validate_server(&prop(0x54a44, basket, [0.0, 0.0, 72.5], false)), Ok(()));
        assert_eq!(validate_server(&prop(0xff00_0004, basket, [0.0, 0.0, 0.0], true)), Ok(()));
        assert_eq!(validate_server(&prop(0, basket, [0.0, 0.0, 0.0], false)), Err(Reject::Range));
        assert_eq!(validate_server(&prop(0x54a44, [f32::NAN, 0.0, 0.0], [0.0, 0.0, 0.0], false)), Err(Reject::NonFinite));
        assert_eq!(validate_server(&prop(0x54a44, basket, [0.0, f32::INFINITY, 0.0], false)), Err(Reject::NonFinite));
        assert_eq!(validate_server(&prop(0x54a44, [2.0e6, 0.0, 0.0], [0.0, 0.0, 0.0], false)), Err(Reject::OutOfWorld));
        let mut guard = ClientGuard::default();
        for _ in 0..128 {
            assert_eq!(validate(&prop(0x54a44, basket, [0.0, 0.0, 0.0], false), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&prop(0x54a44, basket, [0.0, 0.0, 0.0], false), &mut guard, 1_000), Err(Reject::Rate));
        // 100 ms buys nine samples at 96 a second (9.6, the fraction carried
        // to the next refill), and no tenth
        for _ in 0..9 {
            assert_eq!(validate(&prop(0x54a44, basket, [0.0, 0.0, 0.0], true), &mut guard, 1_100), Ok(()));
        }
        assert_eq!(validate(&prop(0x54a44, basket, [0.0, 0.0, 0.0], true), &mut guard, 1_100), Err(Reject::Rate));
        // one firing's eight bodies (Embershard's tripwire 0xb6d3f and its
        // seven falling rocks, thuum docs/research/trap-census-2026-11-05.md
        // rows 18 and the tripwire's) at 10 Hz for three seconds of fall, 240
        // samples at 80 a second, all pass (the old 20 and 10 refuse them
        // from the third tick)
        let bodies = [0xb6d3f, 0xb6d45, 0xb6d48, 0xb6d49, 0xb6d4a, 0xb6d4b, 0xb6d4c, 0xb6d4d];
        let mut seat = ClientGuard::default();
        let mut refused = 0u32;
        for tick in 0..30u64 {
            let now = 10_000u64.saturating_add(tick.saturating_mul(100));
            for body in bodies {
                if validate(&prop(body, basket, [0.0, 0.0, 0.0], false), &mut seat, now) != Ok(()) {
                    refused = refused.saturating_add(1);
                }
            }
        }
        assert_eq!(refused, 0);
    }

    #[test]
    fn prop_claims_name_a_reference_and_a_touch_and_are_rate_limited() {
        use wire_schema::skymp::PropClaim;
        let claim = |refr, touch| Message::PropClaim(PropClaim { refr, touch, ..Default::default() });
        assert_eq!(validate_server(&claim(0x54a44, 0)), Ok(()));
        assert_eq!(validate_server(&claim(0xff00_0004, 3)), Ok(()));
        assert_eq!(validate_server(&claim(0, 0)), Err(Reject::Range));
        assert_eq!(validate_server(&claim(0x54a44, 4)), Err(Reject::Range));
        let mut guard = ClientGuard::default();
        for _ in 0..8 {
            assert_eq!(validate(&claim(0x54a44, 0), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&claim(0x54a44, 0), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&claim(0x54a44, 2), &mut guard, 1_500), Ok(()));
    }

    #[test]
    fn a_prop_owner_word_names_a_reference_and_a_player() {
        use wire_schema::skymp::PropOwner;
        let word = |refr, player, epoch| Message::PropOwner(PropOwner { refr, player, epoch, ..Default::default() });
        assert_eq!(validate_server(&word(0x54a44, 0xff00_0002, 1)), Ok(()));
        assert_eq!(validate_server(&word(0x54a44, 0xff00_0002, 0)), Ok(()));
        assert_eq!(validate_server(&word(0, 0xff00_0002, 1)), Err(Reject::Range));
        assert_eq!(validate_server(&word(0x54a44, 0, 1)), Err(Reject::Range));
    }

    #[test]
    fn placed_requests_name_bases_and_a_window() {
        use wire_schema::skymp::PlacedRequest;
        let ask = |bases: &[u32], window_ms| Message::PlacedRequest(PlacedRequest { set: 3, bases: bases.to_vec().try_into().unwrap_or_default(), window_ms, ..Default::default() });
        assert_eq!(validate_server(&ask(&[0x1a09_8529, 0x1a07_35e8], 3_000)), Ok(()));
        assert_eq!(validate_server(&ask(&[0x1a09_8529], 1)), Ok(()));
        assert_eq!(validate_server(&ask(&[], 3_000)), Err(Reject::Range));
        assert_eq!(validate_server(&ask(&[0x1a09_8529, 0], 3_000)), Err(Reject::Range));
        assert_eq!(validate_server(&ask(&[0x1a09_8529], 0)), Err(Reject::Range));
        assert_eq!(validate_server(&ask(&[0x1a09_8529], 10_001)), Err(Reject::Range));
    }

    #[test]
    fn placed_reports_name_a_base_at_a_finite_place_and_are_rate_limited() {
        use wire_schema::skymp::PlacedReport;
        let report = |base, pos, yaw| Message::PlacedReport(PlacedReport { set: 3, base, pos, yaw, ..Default::default() });
        assert_eq!(validate_server(&report(0x1a09_8529, [23_877.5, -45_485.25, 8.0], 72.5)), Ok(()));
        assert_eq!(validate_server(&report(0, [0.0, 0.0, 0.0], 0.0)), Err(Reject::Range));
        assert_eq!(validate_server(&report(0x1a09_8529, [f32::NAN, 0.0, 0.0], 0.0)), Err(Reject::NonFinite));
        assert_eq!(validate_server(&report(0x1a09_8529, [0.0, 0.0, 0.0], f32::INFINITY)), Err(Reject::NonFinite));
        assert_eq!(validate_server(&report(0x1a09_8529, [3.0e6, 0.0, 0.0], 0.0)), Err(Reject::OutOfWorld));
        let mut guard = ClientGuard::default();
        for _ in 0..8 {
            assert_eq!(validate(&report(0x1a09_8529, [0.0, 0.0, 0.0], 0.0), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&report(0x1a09_8529, [0.0, 0.0, 0.0], 0.0), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&report(0x1a09_8529, [0.0, 0.0, 0.0], 0.0), &mut guard, 1_500), Ok(()));
    }

    #[test]
    fn skill_uses_name_each_skill_once_with_a_bounded_amount_and_are_rate_limited() {
        use wire_schema::skymp::{SkillUse, SkillUseEntry};
        let uses = |entries: &[(u8, f32)]| {
            Message::SkillUse(SkillUse {
                uses: entries.iter().map(|(skill, amount)| SkillUseEntry { skill: *skill, amount: *amount }).collect::<std::vec::Vec<_>>().try_into().unwrap_or_default(),
                ..Default::default()
            })
        };
        assert_eq!(validate_server(&uses(&[(6, 10.0)])), Ok(()));
        assert_eq!(validate_server(&uses(&[(6, 10.0), (23, 1.0)])), Ok(()));
        assert_eq!(validate_server(&uses(&[])), Err(Reject::Range));
        assert_eq!(validate_server(&uses(&[(5, 10.0)])), Err(Reject::Range));
        assert_eq!(validate_server(&uses(&[(24, 10.0)])), Err(Reject::Range));
        assert_eq!(validate_server(&uses(&[(6, 10.0), (6, 1.0)])), Err(Reject::Range));
        assert_eq!(validate_server(&uses(&[(6, 0.0)])), Err(Reject::Range));
        assert_eq!(validate_server(&uses(&[(6, -1.0)])), Err(Reject::Range));
        assert_eq!(validate_server(&uses(&[(6, 100_001.0)])), Err(Reject::Range));
        assert_eq!(validate_server(&uses(&[(6, f32::NAN)])), Err(Reject::NonFinite));
        let mut guard = ClientGuard::default();
        for _ in 0..8 {
            assert_eq!(validate(&uses(&[(6, 10.0)]), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&uses(&[(6, 10.0)]), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&uses(&[(6, 10.0)]), &mut guard, 1_500), Ok(()));
    }

    #[test]
    fn lock_reports_name_a_reference_an_outcome_and_a_bounded_break_count_and_are_rate_limited() {
        use wire_schema::skymp::{LockPrompt, LockReport};
        let report = |refr, outcome, broken| Message::LockReport(LockReport { refr, outcome, broken, ..Default::default() });
        assert_eq!(validate_server(&report(0x371e9, 0, 0)), Ok(()));
        assert_eq!(validate_server(&report(0x371e9, 1, 3)), Ok(()));
        assert_eq!(validate_server(&report(0x371e9, 2, 0)), Ok(()));
        assert_eq!(validate_server(&report(0, 0, 0)), Err(Reject::Range));
        assert_eq!(validate_server(&report(0x371e9, 3, 0)), Err(Reject::Range));
        assert_eq!(validate_server(&report(0x371e9, 1, 9)), Err(Reject::Range));
        let mut guard = ClientGuard::default();
        for _ in 0..8 {
            assert_eq!(validate(&report(0x371e9, 2, 0), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&report(0x371e9, 2, 0), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&report(0x371e9, 2, 0), &mut guard, 1_500), Ok(()));
        let prompt = |refr| Message::LockPrompt(LockPrompt { refr, ..Default::default() });
        assert_eq!(validate_server(&prompt(0x371e9)), Ok(()));
        assert_eq!(validate_server(&prompt(0)), Err(Reject::Range));
    }

    #[test]
    fn perk_picks_name_a_perk_and_are_rate_limited_and_the_state_holds_no_zero() {
        use wire_schema::skymp::{PerkPick, PerkState};
        let pick = |perk| Message::PerkPick(PerkPick { perk, ..Default::default() });
        assert_eq!(validate_server(&pick(0xbabe4)), Ok(()));
        assert_eq!(validate_server(&pick(0)), Err(Reject::Range));
        let mut guard = ClientGuard::default();
        for _ in 0..8 {
            assert_eq!(validate(&pick(0xbabe4), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&pick(0xbabe4), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&pick(0xbabe4), &mut guard, 1_500), Ok(()));
        let state = |perks: &[u32], points| Message::PerkState(PerkState { perks: perks.to_vec().try_into().unwrap_or_default(), points, ..Default::default() });
        assert_eq!(validate_server(&state(&[], 0)), Ok(()));
        assert_eq!(validate_server(&state(&[0xbabe4, 0x79343], 1)), Ok(()));
        assert_eq!(validate_server(&state(&[0xbabe4, 0], 1)), Err(Reject::Range));
    }

    #[test]
    fn word_wall_reports_name_a_wall_and_are_rate_limited() {
        use wire_schema::skymp::WordWall;
        let report = |wall| Message::WordWall(WordWall { wall, ..Default::default() });
        assert_eq!(validate_server(&report(0x709d2)), Ok(()));
        assert_eq!(validate_server(&report(0)), Err(Reject::Range));
        let mut guard = ClientGuard::default();
        assert_eq!(validate(&report(0x709d2), &mut guard, 1_000), Ok(()));
        assert_eq!(validate(&report(0x709d2), &mut guard, 1_000), Ok(()));
        assert_eq!(validate(&report(0x709d2), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&report(0x709d2), &mut guard, 2_000), Ok(()));
    }

    #[test]
    fn object_animations_name_a_reference_and_an_event_and_are_rate_limited() {
        use wire_schema::skymp::ObjectAnimation;
        let anim = |refr, event: &str| {
            Message::ObjectAnimation(ObjectAnimation { refr, event: event.try_into().unwrap_or_default(), ..Default::default() })
        };
        assert_eq!(validate_server(&anim(0xaa8cb, "Turned01")), Ok(()));
        assert_eq!(validate_server(&anim(0, "Turned01")), Err(Reject::Range));
        assert_eq!(validate_server(&anim(0xaa8cb, "")), Err(Reject::Range));
        let mut guard = ClientGuard::default();
        for _ in 0..32 {
            assert_eq!(validate(&anim(0xaa8cb, "Trigger01"), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&anim(0xaa8cb, "Trigger01"), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&anim(0xaa8cb, "Trigger01"), &mut guard, 2_000), Ok(()));
    }

    #[test]
    fn destruction_stages_name_a_game_file_reference_and_a_bounded_stage_and_are_rate_limited() {
        // thuum D-S3: Bleak Falls Temple's oil lamp 0x9b0fb at its first
        // stage, Embershard's spent tripwire 0xb6d3f served after a create
        // word, the bound's edges
        use wire_schema::skymp::DestructionStage;
        let stage = |refr, stage, destroyed, served| Message::DestructionStage(DestructionStage { refr, stage, destroyed, served, ..Default::default() });
        assert_eq!(validate_server(&stage(0x9b0fb, 1, false, false)), Ok(()));
        assert_eq!(validate_server(&stage(0xb6d3f, 0, true, true)), Ok(()));
        assert_eq!(validate_server(&stage(0xfeff_ffff, DESTRUCTION_MAX_STAGE, true, false)), Ok(()));
        assert_eq!(validate_server(&stage(0, 1, false, false)), Err(Reject::Range));
        assert_eq!(validate_server(&stage(0xff00_0000, 1, false, false)), Err(Reject::Range));
        assert_eq!(validate_server(&stage(u32::MAX, 0, true, false)), Err(Reject::Range));
        assert_eq!(validate_server(&stage(0x9b0fb, DESTRUCTION_MAX_STAGE.saturating_add(1), false, false)), Err(Reject::Range));
        assert_eq!(validate_server(&stage(0x9b0fb, u8::MAX, false, false)), Err(Reject::Range));
        let mut guard = ClientGuard::default();
        for _ in 0..8 {
            assert_eq!(validate(&stage(0x9b0fb, 1, false, false), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&stage(0x9b0fb, 1, false, false), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&stage(0x9b0fb, 2, false, false), &mut guard, 1_500), Ok(()));
        // a refused shape spends no token: the shape is judged first
        let mut fresh = ClientGuard::default();
        for _ in 0..20 {
            assert_eq!(validate(&stage(0, 1, false, false), &mut fresh, 1_000), Err(Reject::Range));
        }
        assert_eq!(validate(&stage(0x9b0fb, 1, false, false), &mut fresh, 1_000), Ok(()));
    }

    #[test]
    fn a_ref_state_word_is_the_servers_alone() {
        // thuum ADR-068 rung 4: the widened DestructionStage, the cobweb
        // 0x1fc43 disabled at the object's sequence 3 and served; a seat's
        // report carries no ref state word and a sequence of 0
        use wire_schema::skymp::DestructionStage;
        let word = |disabled: Option<bool>, deleted: Option<bool>, seq: u32| Message::DestructionStage(DestructionStage { refr: 0x1fc43, disabled, deleted, seq, served: true, ..Default::default() });
        assert_eq!(validate_server(&word(Some(true), None, 3)), Ok(()));
        assert_eq!(validate_server(&word(Some(true), Some(true), u32::MAX)), Ok(()));
        let mut guard = ClientGuard::default();
        assert_eq!(validate(&word(Some(true), None, 3), &mut guard, 0), Err(Reject::Range));
        assert_eq!(validate(&word(None, Some(false), 0), &mut guard, 0), Err(Reject::Range));
        let report = |seq: u32| Message::DestructionStage(DestructionStage { refr: 0x9b0fb, stage: 1, seq, ..Default::default() });
        assert_eq!(validate(&report(1), &mut guard, 0), Err(Reject::Range));
        assert_eq!(validate(&report(0), &mut guard, 0), Ok(()));
        // the refusals spent no token: eight in all
        for _ in 1..8 {
            assert_eq!(validate(&report(0), &mut guard, 0), Ok(()));
        }
        assert_eq!(validate(&report(0), &mut guard, 0), Err(Reject::Rate));
    }

    #[test]
    fn alias_fills_name_a_quest_an_alias_and_a_reference() {
        // thuum ADR-068 rung 4: MS13 0x39645's ArvelWeb alias 10 forced to
        // the web 0x1fc43, a clear, the edges
        use wire_schema::skymp::AliasFill;
        let fill = |quest, alias, refr, served| Message::AliasFill(AliasFill { quest, alias, refr, served, ..Default::default() });
        assert_eq!(validate_server(&fill(0x39645, 10, 0x1fc43, false)), Ok(()));
        assert_eq!(validate_server(&fill(0x39645, 10, 0, true)), Ok(()));
        assert_eq!(validate_server(&fill(0x39645, 0xFFFF, 0xfeff_ffff, false)), Ok(()));
        assert_eq!(validate_server(&fill(0, 10, 0x1fc43, false)), Err(Reject::Range));
        assert_eq!(validate_server(&fill(0xff00_0000, 10, 0x1fc43, false)), Err(Reject::Range));
        assert_eq!(validate_server(&fill(0x39645, 0x1_0000, 0x1fc43, false)), Err(Reject::Range));
        assert_eq!(validate_server(&fill(0x39645, 10, 0xff00_0002, false)), Err(Reject::Range));
        let mut guard = ClientGuard::default();
        // a seat never sends a served record
        assert_eq!(validate(&fill(0x39645, 10, 0, true), &mut guard, 0), Err(Reject::Range));
        for _ in 0..16 {
            assert_eq!(validate(&fill(0x39645, 10, 0x1fc43, false), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&fill(0x39645, 10, 0x1fc43, false), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&fill(0x39645, 10, 0x1fc43, false), &mut guard, 1_250), Ok(()));
    }

    #[test]
    fn world_globals_name_glob_forms_once_and_finite() {
        // thuum ADR-068 rung 4: a streamer's report at epoch 2, the stream
        // word, a served record (illustrative GLOB ids)
        use wire_schema::skymp::{WorldGlobal, WorldGlobalEntry};
        let entry = |global, value| WorldGlobalEntry { global, value };
        let globals = |epoch, stream, entries: &[WorldGlobalEntry], served| {
            Message::WorldGlobal(WorldGlobal { epoch, stream, entries: entries.to_vec().try_into().unwrap_or_default(), served, ..Default::default() })
        };
        assert_eq!(validate_server(&globals(2, None, &[entry(0x5e3bd, 7.0), entry(0x1000, -1.5)], false)), Ok(()));
        assert_eq!(validate_server(&globals(3, Some(true), &[], false)), Ok(()));
        assert_eq!(validate_server(&globals(0, None, &[entry(0x5e3bd, 7.0)], true)), Ok(()), "a record served before any election");
        assert_eq!(validate_server(&globals(0, Some(true), &[], false)), Err(Reject::Range), "a stream word names its epoch");
        assert_eq!(validate_server(&globals(2, None, &[entry(0, 1.0)], false)), Err(Reject::Range));
        assert_eq!(validate_server(&globals(2, None, &[entry(0xff00_0001, 1.0)], false)), Err(Reject::Range));
        assert_eq!(validate_server(&globals(2, None, &[entry(0x5e3bd, 1.0), entry(0x5e3bd, 2.0)], false)), Err(Reject::Range));
        assert_eq!(validate_server(&globals(2, None, &[entry(0x5e3bd, f32::NAN)], false)), Err(Reject::NonFinite));
        assert_eq!(validate_server(&globals(2, None, &[entry(0x5e3bd, f32::INFINITY)], false)), Err(Reject::NonFinite));
        let mut guard = ClientGuard::default();
        // a report never carries the word, a served flag or epoch 0
        assert_eq!(validate(&globals(2, Some(true), &[], false), &mut guard, 0), Err(Reject::Range));
        assert_eq!(validate(&globals(2, None, &[entry(0x5e3bd, 1.0)], true), &mut guard, 0), Err(Reject::Range));
        assert_eq!(validate(&globals(0, None, &[entry(0x5e3bd, 1.0)], false), &mut guard, 0), Err(Reject::Range));
        for _ in 0..4 {
            assert_eq!(validate(&globals(2, None, &[entry(0x5e3bd, 1.0)], false), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&globals(2, None, &[entry(0x5e3bd, 1.0)], false), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&globals(2, None, &[entry(0x5e3bd, 1.0)], false), &mut guard, 2_000), Ok(()));
    }

    #[test]
    fn relayed_activations_name_both_references() {
        use wire_schema::skymp::RelayedActivation;
        let relay = |target, caster| Message::RelayedActivation(RelayedActivation { target, caster, ..Default::default() });
        assert_eq!(validate_server(&relay(0x54b15, 0x567f2)), Ok(()));
        assert_eq!(validate_server(&relay(0, 0x567f2)), Err(Reject::Range));
        assert_eq!(validate_server(&relay(0x54b15, 0)), Err(Reject::Range));
        // thuum D-C2 (SCHEMA_VERSION 40): a guest's entry into the web
        // 0x37281 relayed to the host with its figure 0xff000001, enter or
        // leave; any other event byte is no event
        let entry = |entry| Message::RelayedActivation(RelayedActivation { target: 0x37281, caster: 0xff00_0001, entry, ..Default::default() });
        assert_eq!(validate_server(&entry(Some(1))), Ok(()));
        assert_eq!(validate_server(&entry(Some(2))), Ok(()));
        assert_eq!(validate_server(&entry(None)), Ok(()));
        for bad in [0u8, 3, u8::MAX] {
            assert_eq!(validate_server(&entry(Some(bad))), Err(Reject::Range), "{bad}");
        }
    }

    #[test]
    fn trigger_entries_name_an_event_a_presser_and_a_trigger_and_are_rate_limited() {
        // thuum D-W1 and D-C2 (SCHEMA_VERSION 40): c2's own player entering
        // and leaving Bleak Falls Temple's web 0x37281, an NPC it simulates
        // (illustrative 0x380b3, a draugr of the Temple), the edges
        use wire_schema::skymp::{Activate, ActivateData};
        let activate = |caster: u64, target: u64, is_second_activation: bool, entry: Option<u8>| {
            Message::Activate(Activate { data: ActivateData { caster, target, is_second_activation, entry, ..Default::default() }, ..Default::default() })
        };
        assert_eq!(validate_server(&activate(0x14, 0x37281, false, Some(1))), Ok(()));
        assert_eq!(validate_server(&activate(0x14, 0x37281, false, Some(2))), Ok(()));
        assert_eq!(validate_server(&activate(0x380b3, 0x37281, false, Some(1))), Ok(()));
        assert_eq!(validate_server(&activate(0xff00_0005, 0xfeff_ffff, false, Some(1))), Ok(()), "a server-made NPC presses a trigger at the load order's top");
        for bad in [0u8, 3, u8::MAX] {
            assert_eq!(validate_server(&activate(0x14, 0x37281, false, Some(bad))), Err(Reject::Range), "{bad}");
        }
        assert_eq!(validate_server(&activate(0x14, 0x37281, true, Some(1))), Err(Reject::Range), "an entry is no second activation");
        assert_eq!(validate_server(&activate(0, 0x37281, false, Some(1))), Err(Reject::Range));
        assert_eq!(validate_server(&activate(0x14, 0, false, Some(1))), Err(Reject::Range));
        assert_eq!(validate_server(&activate(0x14, 0xff00_0000, false, Some(1))), Err(Reject::Range));
        assert_eq!(validate_server(&activate(0x14, u64::MAX, false, Some(2))), Err(Reject::Range));
        // an activation without an entry keeps the shared checks alone: the
        // legacy shapes, a second activation and a server form's target too
        assert_eq!(validate_server(&activate(0x14, 0x37281, true, None)), Ok(()));
        assert_eq!(validate_server(&activate(0, 0, false, None)), Ok(()));
        assert_eq!(validate_server(&activate(0x14, 0xff00_0001, false, None)), Ok(()));
        // eight entries at once, then four a second; a refused shape spends
        // no token, and an activation without an entry spends none
        let mut guard = ClientGuard::default();
        for _ in 0..100 {
            assert_eq!(validate(&activate(0x14, 0x37281, false, None), &mut guard, 1_000), Ok(()));
            assert_eq!(validate(&activate(0x14, 0x37281, true, Some(1)), &mut guard, 1_000), Err(Reject::Range));
        }
        for _ in 0..8 {
            assert_eq!(validate(&activate(0x14, 0x37281, false, Some(1)), &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&activate(0x14, 0x37281, false, Some(2)), &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&activate(0x14, 0x37281, false, None), &mut guard, 1_000), Ok(()), "the entry budget is the entries' alone");
        // one token every 250 ms
        assert_eq!(validate(&activate(0x14, 0x37281, false, Some(2)), &mut guard, 1_249), Err(Reject::Rate));
        assert_eq!(validate(&activate(0x14, 0x37281, false, Some(2)), &mut guard, 1_250), Ok(()));
    }

    #[test]
    fn weather_words_name_a_place_and_may_release() {
        use wire_schema::skymp::SetWeather;
        let word = |place, weather, transition| Message::SetWeather(SetWeather { place, weather, transition, ..Default::default() });
        assert_eq!(validate_server(&word(0x3c, 0x81a, false)), Ok(()));
        assert_eq!(validate_server(&word(0x3c, 0x12f89, true)), Ok(()));
        assert_eq!(validate_server(&word(0x371de, 0, false)), Ok(()), "a release in an interior");
        assert_eq!(validate_server(&word(0, 0x81a, false)), Err(Reject::Range));
    }

    #[test]
    fn damage_causes_carry_a_percentage_for_a_victim_in_range() {
        use wire_schema::skymp::DamageCause;
        let cause = |idx, health| Message::DamageCause(DamageCause { idx, attacker: 0xff00_0001, health, kill_move: false, ..Default::default() });
        assert_eq!(validate_server(&cause(7, 0.42)), Ok(()));
        assert_eq!(validate_server(&cause(7, 0.0)), Ok(()));
        assert_eq!(validate_server(&cause(7, 1.0)), Ok(()));
        assert_eq!(validate_server(&cause(7, f32::NAN)), Err(Reject::NonFinite));
        assert_eq!(validate_server(&cause(7, 1.5)), Err(Reject::Range));
        assert_eq!(validate_server(&cause(u32::MAX, 0.5)), Err(Reject::Range));
    }

    #[test]
    fn actor_fates_name_placements_with_a_base_and_worn_forms() {
        assert_eq!(validate_server(&fates(&[(0x85547, 0xff00_1234, 25, &[0x1397f, 0x13982])])), Ok(()));
        assert_eq!(validate_server(&fates(&[])), Ok(()));
        // a roll of none: the placement stays empty
        assert_eq!(validate_server(&fates(&[(0x85547, 0, 25, &[])])), Ok(()));
        // a server-made NPC is a placement too (ADR-050 decision 3); a placement of 0; a worn id of 0
        assert_eq!(validate_server(&fates(&[(0xff00_0c03, 0x1bcd8, 25, &[0x1397f])])), Ok(()));
        assert_eq!(validate_server(&fates(&[(0, 0x7, 1, &[])])), Err(Reject::Range));
        assert_eq!(validate_server(&fates(&[(0x85547, 0x7, 1, &[0])])), Err(Reject::Range));
    }

    #[test]
    fn actor_fates_carry_the_records_inventory_rows_with_ids_and_counts() {
        use wire_schema::skymp::{ActorFate, ActorFateEntry, FateInvRow};
        let with_inv = |rows: &[(u32, u32)]| {
            Message::ActorFate(ActorFate {
                fates: vec![ActorFateEntry {
                    refr_id: 0x39fe2,
                    base_id: 0x1bcd8,
                    level: 7,
                    worn: vec![0x13982].try_into().unwrap_or_default(),
                    inv: rows
                        .iter()
                        .map(|&(base_id, count)| FateInvRow { base_id, count })
                        .collect::<Vec<_>>()
                        .try_into()
                        .unwrap_or_default(),
                }]
                .try_into()
                .unwrap_or_default(),
                ..Default::default()
            })
        };
        // the longbow and twenty arrows the host's engine used to roll alone
        assert_eq!(validate_server(&with_inv(&[(0x3b562, 1), (0x1397d, 20), (0xf, 43)])), Ok(()));
        assert_eq!(validate_server(&with_inv(&[])), Ok(()));
        assert_eq!(validate_server(&with_inv(&[(0, 1)])), Err(Reject::Range));
        assert_eq!(validate_server(&with_inv(&[(0x3b562, 0)])), Err(Reject::Range));
    }

    #[test]
    fn actor_sightings_name_game_file_actors_in_a_world_at_a_place() {
        let riverwood = [24063.7, -45204.2, -40.6];
        assert_eq!(validate(&sightings(&[(0x13483, 0x3c, riverwood)]), &mut ClientGuard::default(), 0), Ok(()));
        assert_eq!(validate(&sightings(&[]), &mut ClientGuard::default(), 0), Ok(()));
        // a server-made actor, nobody's to sight; a form id of 0; a world of 0
        assert_eq!(validate(&sightings(&[(0xff00_0c03, 0x3c, riverwood)]), &mut ClientGuard::default(), 0), Err(Reject::Range));
        assert_eq!(validate(&sightings(&[(0, 0x3c, riverwood)]), &mut ClientGuard::default(), 0), Err(Reject::Range));
        assert_eq!(validate(&sightings(&[(0x13483, 0, riverwood)]), &mut ClientGuard::default(), 0), Err(Reject::Range));
        // a position past the world, or not a number
        assert_eq!(validate(&sightings(&[(0x13483, 0x3c, [1.0e9, 0.0, 0.0])]), &mut ClientGuard::default(), 0), Err(Reject::OutOfWorld));
        assert_eq!(validate(&sightings(&[(0x13483, 0x3c, [f32::NAN, 0.0, 0.0])]), &mut ClientGuard::default(), 0), Err(Reject::NonFinite));
    }

    #[test]
    fn host_grants_and_releases_name_a_cell_and_an_epoch_after_the_first() {
        use wire_schema::skymp::{HostGrant, HostRelease};
        let grant = |cell, epoch| Message::HostGrant(HostGrant { cell, epoch, ..Default::default() });
        let release = |cell, epoch| Message::HostRelease(HostRelease { cell, epoch, ..Default::default() });
        assert_eq!(validate_server(&grant(0x9732, 1)), Ok(()));
        assert_eq!(validate_server(&release(0x9732, 1)), Ok(()));
        assert_eq!(validate_server(&grant(0, 1)), Err(Reject::Range));
        assert_eq!(validate_server(&grant(0x9732, 0)), Err(Reject::Range));
        assert_eq!(validate_server(&release(0x9732, 0)), Err(Reject::Range));
    }

    proptest! {
        #[test]
        fn movement_never_panics(
            x in any::<f32>(), y in any::<f32>(), z in any::<f32>(), yaw in any::<f32>(),
            pitch in any::<f32>(), seq in any::<u32>(), now in any::<u64>(),
        ) {
            let m = Message::Movement(MovementSample {
                seq, actor: FormId(0x14),
                transform: Transform { x, y, z, yaw, pitch },
                run: false, sneak: false,
            });
            let mut g = ClientGuard::default();
            let _ = validate(&m, &mut g, now);
            let _ = validate(&m, &mut g, now);
        }

        #[test]
        fn hits_never_panic(seqs in proptest::collection::vec(any::<u32>(), 0..200), ticks in proptest::collection::vec(any::<u16>(), 0..200)) {
            let mut g = ClientGuard::default();
            let mut now = 0u64;
            for (seq, tick) in seqs.iter().zip(ticks.iter()) {
                now = now.saturating_add(u64::from(*tick));
                let _ = validate(&hit(*seq), &mut g, now);
            }
        }

        #[test]
        fn cell_host_messages_never_panic(cell in any::<u32>(), epoch in any::<u32>(), loaded in any::<bool>(), now in any::<u64>()) {
            use wire_schema::skymp::{CellLoaded, HostGrant, HostRelease};
            let mut g = ClientGuard::default();
            let _ = validate(&Message::CellLoaded(CellLoaded { cell, loaded, ..Default::default() }), &mut g, now);
            let _ = validate_server(&Message::HostGrant(HostGrant { cell, epoch, ..Default::default() }));
            let _ = validate_server(&Message::HostRelease(HostRelease { cell, epoch, ..Default::default() }));
        }

        #[test]
        fn hosted_never_panics(health in any::<f32>(), counts in proptest::collection::vec(any::<i32>(), 0..=cap::INVENTORY_DELTA)) {
            let mut g = ClientGuard::default();
            let _ = validate(&hosted(health, &counts), &mut g, 0);
        }

        #[test]
        fn speech_never_panics(idx in any::<u32>(), topic in any::<u32>(), info in any::<u32>(), now in any::<u64>()) {
            let m = Message::ActorSpeech(wire_schema::skymp::ActorSpeech { idx, topic, info, ..Default::default() });
            let mut g = ClientGuard::default();
            let _ = validate(&m, &mut g, now);
            let _ = validate_server(&m);
        }

        #[test]
        fn lock_messages_never_panic(refr in any::<u32>(), outcome in any::<u8>(), broken in any::<u8>(), now in any::<u64>()) {
            use wire_schema::skymp::{LockPrompt, LockReport};
            let mut g = ClientGuard::default();
            let m = Message::LockReport(LockReport { refr, outcome, broken, ..Default::default() });
            let _ = validate(&m, &mut g, now);
            prop_assert_eq!(validate_server(&m), if refr == 0 || outcome > 2 || broken > 8 { Err(Reject::Range) } else { Ok(()) });
            let _ = validate_server(&Message::LockPrompt(LockPrompt { refr, ..Default::default() }));
        }

        #[test]
        fn perk_messages_never_panic(perk in any::<u32>(), perks in proptest::collection::vec(any::<u32>(), 0..=8), points in any::<u16>(), now in any::<u64>()) {
            use wire_schema::skymp::{PerkPick, PerkState};
            let mut g = ClientGuard::default();
            let m = Message::PerkPick(PerkPick { perk, ..Default::default() });
            let _ = validate(&m, &mut g, now);
            prop_assert_eq!(validate_server(&m), if perk == 0 { Err(Reject::Range) } else { Ok(()) });
            let has_zero = perks.contains(&0);
            let s = Message::PerkState(PerkState { perks: perks.try_into().unwrap_or_default(), points, ..Default::default() });
            prop_assert_eq!(validate_server(&s), if has_zero { Err(Reject::Range) } else { Ok(()) });
        }

        #[test]
        fn word_wall_reports_never_panic(wall in any::<u32>(), now in any::<u64>()) {
            use wire_schema::skymp::WordWall;
            let mut g = ClientGuard::default();
            let m = Message::WordWall(WordWall { wall, ..Default::default() });
            let _ = validate(&m, &mut g, now);
            prop_assert_eq!(validate_server(&m), if wall == 0 { Err(Reject::Range) } else { Ok(()) });
        }

        #[test]
        fn destruction_stages_never_panic(refr in any::<u32>(), stage in any::<u8>(), destroyed in any::<bool>(), served in any::<bool>(), now in any::<u64>()) {
            use wire_schema::skymp::DestructionStage;
            let mut g = ClientGuard::default();
            let m = Message::DestructionStage(DestructionStage { refr, stage, destroyed, served, ..Default::default() });
            let _ = validate(&m, &mut g, now);
            let want = if refr == 0 || refr >= 0xff00_0000 || stage > DESTRUCTION_MAX_STAGE { Err(Reject::Range) } else { Ok(()) };
            prop_assert_eq!(validate_server(&m), want);
        }

        #[test]
        fn relayed_activations_never_panic(target in any::<u32>(), caster in any::<u32>(), entry in proptest::option::of(prop_oneof![0u8..4, any::<u8>()])) {
            use wire_schema::skymp::RelayedActivation;
            let m = Message::RelayedActivation(RelayedActivation { target, caster, entry, ..Default::default() });
            let entry_ok = entry.is_none_or(|e| e == 1 || e == 2);
            prop_assert_eq!(validate_server(&m), if target == 0 || caster == 0 || !entry_ok { Err(Reject::Range) } else { Ok(()) });
        }

        // thuum D-C2: any presser, trigger, flag and event byte, any clock:
        // a verdict, never a panic, and the shape's verdict the rule gives
        #[test]
        fn trigger_entries_never_panic(
            caster in prop_oneof![Just(0u64), Just(0x14u64), any::<u64>()],
            target in prop_oneof![Just(0u64), Just(0x37281u64), Just(0xff00_0000u64), any::<u64>()],
            second in any::<bool>(), entry in proptest::option::of(prop_oneof![0u8..4, any::<u8>()]), now in any::<u64>(),
        ) {
            use wire_schema::skymp::{Activate, ActivateData};
            let m = Message::Activate(Activate { data: ActivateData { caster, target, is_second_activation: second, entry, ..Default::default() }, ..Default::default() });
            let want = match entry {
                None => Ok(()),
                Some(e) if (e == 1 || e == 2) && !second && caster != 0 && target != 0 && target < 0xff00_0000 && caster <= u64::from(u32::MAX) => Ok(()),
                Some(_) => Err(Reject::Range),
            };
            prop_assert_eq!(validate_server(&m), want);
            let mut g = ClientGuard::default();
            let _ = validate(&m, &mut g, now);
        }

        #[test]
        fn weather_words_never_panic(place in any::<u32>(), weather in any::<u32>(), transition in any::<bool>()) {
            use wire_schema::skymp::SetWeather;
            let m = Message::SetWeather(SetWeather { place, weather, transition, ..Default::default() });
            prop_assert_eq!(validate_server(&m), if place == 0 { Err(Reject::Range) } else { Ok(()) });
        }

        #[test]
        fn skill_uses_never_panic(
            entries in proptest::collection::vec((any::<u8>(), any::<f32>()), 0..=cap::SKILL_USES),
            now in any::<u64>(),
        ) {
            use wire_schema::skymp::{SkillUse, SkillUseEntry};
            let m = Message::SkillUse(SkillUse {
                uses: entries.iter().map(|(skill, amount)| SkillUseEntry { skill: *skill, amount: *amount }).collect::<std::vec::Vec<_>>().try_into().unwrap_or_default(),
                ..Default::default()
            });
            let mut g = ClientGuard::default();
            let _ = validate(&m, &mut g, now);
            let _ = validate_server(&m);
        }

        #[test]
        fn placed_messages_never_panic(
            set in any::<u64>(), base in any::<u32>(), x in any::<f32>(), y in any::<f32>(), z in any::<f32>(),
            yaw in any::<f32>(), window_ms in any::<u32>(), now in any::<u64>(),
            bases in proptest::collection::vec(any::<u32>(), 0..=cap::PLACED_BASES),
        ) {
            use wire_schema::skymp::{PlacedReport, PlacedRequest};
            let mut g = ClientGuard::default();
            let report = Message::PlacedReport(PlacedReport { set, base, pos: [x, y, z], yaw, ..Default::default() });
            let _ = validate(&report, &mut g, now);
            let _ = validate_server(&report);
            let ask = Message::PlacedRequest(PlacedRequest { set, bases: bases.clone().try_into().unwrap_or_default(), window_ms, ..Default::default() });
            let _ = validate(&ask, &mut g, now);
            let want = if bases.is_empty() || bases.contains(&0) || window_ms == 0 || window_ms > 10_000 { Err(Reject::Range) } else { Ok(()) };
            prop_assert_eq!(validate_server(&ask), want);
        }

        #[test]
        fn prop_messages_never_panic(
            refr in any::<u32>(), x in any::<f32>(), y in any::<f32>(), z in any::<f32>(),
            rx in any::<f32>(), touch in any::<u8>(), player in any::<u32>(), epoch in any::<u32>(),
            asleep in any::<bool>(), now in any::<u64>(),
        ) {
            use wire_schema::skymp::{PropClaim, PropOwner, PropTransform};
            let mut g = ClientGuard::default();
            let m = Message::PropTransform(PropTransform { refr, pos: [x, y, z], rot: [rx, y, x], asleep, ..Default::default() });
            let _ = validate(&m, &mut g, now);
            let _ = validate(&m, &mut g, now);
            let _ = validate_server(&m);
            let c = Message::PropClaim(PropClaim { refr, touch, ..Default::default() });
            let _ = validate(&c, &mut g, now);
            let _ = validate_server(&c);
            let _ = validate_server(&Message::PropOwner(PropOwner { refr, player, epoch, ..Default::default() }));
        }

        #[test]
        fn hosted_state_never_panics(
            idx in any::<u32>(), health in any::<f32>(), magicka in any::<f32>(), stamina in any::<f32>(),
            target in any::<u32>(), package in any::<u32>(), now in any::<u64>(),
        ) {
            let m = Message::HostedActorState(wire_schema::skymp::HostedActorState {
                idx, health, magicka, stamina, combat_target: target, package, ..Default::default()
            });
            let mut g = ClientGuard::default();
            let _ = validate(&m, &mut g, now);
            let _ = validate(&m, &mut g, now);
            let _ = validate_server(&m);
        }

        #[test]
        fn effects_are_accepted_exactly_within_four_bits(m in any::<u8>()) {
            let want = if m & !EFFECT_BITS == 0 { Ok(()) } else { Err(Reject::Range) };
            prop_assert_eq!(validate(&effects(m), &mut ClientGuard::default(), 0), want);
        }

        #[test]
        fn favorites_are_accepted_exactly_when_keys_are_legal_and_unique_and_forms_unique(
            entries in proptest::collection::vec((0u32..24, prop_oneof![3 => -1i8..=7, 1 => any::<i8>()]), 0..24),
        ) {
            let mut keys = std::collections::BTreeSet::new();
            let mut forms = std::collections::BTreeSet::new();
            let legal = entries.iter().all(|&(form, key)| {
                HOTKEYS.contains(&key) && (key < 0 || keys.insert(key)) && forms.insert(form)
            });
            let want = if legal { Ok(()) } else { Err(Reject::Range) };
            prop_assert_eq!(validate(&favorites(&entries), &mut ClientGuard::default(), 0), want);
        }

        #[test]
        fn favorites_never_panic(entries in proptest::collection::vec((any::<u32>(), any::<i8>()), 0..=cap::FAVORITES)) {
            let _ = validate(&favorites(&entries), &mut ClientGuard::default(), 0);
        }

        #[test]
        fn actor_sightings_never_panic(entries in proptest::collection::vec((any::<u32>(), any::<u32>(), any::<[f32; 3]>()), 0..=cap::ACTOR_SIGHTINGS)) {
            let _ = validate(&sightings(&entries), &mut ClientGuard::default(), 0);
        }
        #[test]
        fn actor_fates_never_panic(entries in proptest::collection::vec((any::<u32>(), any::<u32>(), any::<u16>(), proptest::collection::vec(any::<u32>(), 0..=cap::FATE_WORN)), 0..=cap::ACTOR_FATES)) {
            let borrowed: Vec<(u32, u32, u16, &[u32])> = entries.iter().map(|(r, b, l, w)| (*r, *b, *l, w.as_slice())).collect();
            let _ = validate_server(&fates(&borrowed));
        }

        #[test]
        fn presets_are_accepted_exactly_when_not_empty_and_from_a_client_its_own(
            actor in prop_oneof![1 => Just(0u32), 1 => any::<u32>()],
            text in "[ -~]{0,64}",
        ) {
            let m = preset(actor, &text);
            let shape = if text.is_empty() { Err(Reject::Range) } else { Ok(()) };
            prop_assert_eq!(validate_server(&m), shape);
            let from_client = shape.and(if actor == 0 { Ok(()) } else { Err(Reject::Range) });
            prop_assert_eq!(validate(&m, &mut ClientGuard::default(), 0), from_client);
        }

        #[test]
        fn presets_never_panic(actor in any::<u32>(), text in ".{0,256}") {
            let _ = validate(&preset(actor, &text), &mut ClientGuard::default(), 0);
            let _ = validate_server(&preset(actor, &text));
        }

        #[test]
        fn actor_values_are_accepted_exactly_when_indices_are_in_range_and_unique(
            avs in proptest::collection::vec(0u8..=170, 0..12),
            skills in proptest::collection::vec(0u8..=20, 0..6),
        ) {
            let m = actor_values(&avs, &skills, &[], 1.0);
            let unique = |v: &[u8]| v.iter().enumerate().all(|(i, x)| !v.iter().take(i).any(|y| y == x));
            let ok = avs.iter().all(|&a| usize::from(a) < cap::ACTOR_VALUES)
                && skills.iter().all(|&s| usize::from(s) < cap::SKILLS)
                && unique(&avs) && unique(&skills);
            prop_assert_eq!(validate_server(&m), if ok { Ok(()) } else { Err(Reject::Range) });
        }

        #[test]
        fn actor_values_never_panic(
            avs in proptest::collection::vec(any::<u8>(), 0..=cap::ACTOR_VALUES),
            skills in proptest::collection::vec(any::<u8>(), 0..=cap::SKILLS),
            base in any::<f32>(),
        ) {
            let _ = validate(&actor_values(&avs, &skills, &skills, base), &mut ClientGuard::default(), 0);
        }

        #[test]
        fn marker_is_accepted_exactly_for_location_types(t in any::<u16>()) {
            let want = if t <= MARKER_TYPE_MAX { Ok(()) } else { Err(Reject::Range) };
            prop_assert_eq!(validate(&marker(t), &mut ClientGuard::default(), 0), want);
        }

        #[test]
        fn a_casts_launch_is_accepted_exactly_inside_the_world_at_a_pitch_within_bounds_or_all_zero(
            has_launch in any::<bool>(),
            origin in proptest::array::uniform3(prop_oneof![1 => Just(0.0f32), 3 => -2.0e6f32..2.0e6]),
            angle_x in prop_oneof![1 => Just(0.0f32), 3 => -3.2f32..3.2],
            angle_z in prop_oneof![1 => Just(0.0f32), 3 => -7.0f32..7.0],
        ) {
            let [x, y, z] = origin;
            let m = Message::SpellCast(skymp::SpellCast {
                data: skymp::SpellCastData {
                    spell: 0x12fd0,
                    casting_source: 1,
                    has_launch,
                    origin_x: x,
                    origin_y: y,
                    origin_z: z,
                    launch_angle_x: angle_x,
                    launch_angle_z: angle_z,
                    ..Default::default()
                },
                ..Default::default()
            });
            let want = if !has_launch {
                if origin.iter().chain(&[angle_x, angle_z]).all(|v| *v == 0.0) { Ok(()) } else { Err(Reject::Range) }
            } else if !(-core::f32::consts::FRAC_PI_2..=core::f32::consts::FRAC_PI_2).contains(&angle_x) {
                Err(Reject::Range)
            } else if origin.iter().any(|v| v.abs() > WORLD_ABS_MAX) {
                Err(Reject::OutOfWorld)
            } else {
                Ok(())
            };
            prop_assert_eq!(validate_server(&m), want);
            prop_assert_eq!(validate(&m, &mut ClientGuard::default(), 0), want);
        }

        #[test]
        fn rest_is_accepted_exactly_in_range(hours in -30.0f32..60.0) {
            let want = if (1.0..=24.0).contains(&hours) { Ok(()) } else { Err(Reject::Range) };
            prop_assert_eq!(validate(&rest(hours), &mut ClientGuard::default(), 0), want);
        }

        #[test]
        fn game_time_is_accepted_exactly_in_range(
            month in 0u32..16, day in 0u32..40, hour in -2.0f32..26.0,
            days_passed in -10.0f32..1e7, time_scale in -5.0f32..100.0,
        ) {
            let mut g = ClientGuard::default();
            let legal = month <= 11 && (1..=31).contains(&day) && (0.0..24.0).contains(&hour)
                && days_passed >= 0.0 && time_scale >= 0.0;
            let want = if legal { Ok(()) } else { Err(Reject::Range) };
            prop_assert_eq!(validate(&game_time(month, day, hour, days_passed, time_scale), &mut g, 0), want);
        }

        #[test]
        fn game_time_never_panics(year in any::<u32>(), month in any::<u32>(), day in any::<u32>(),
            hour in any::<f32>(), days_passed in any::<f32>(), time_scale in any::<f32>()) {
            let m = Message::SetGameTime(skymp::SetGameTime { year, month, day, hour, days_passed, time_scale, ..Default::default() });
            let _ = validate(&m, &mut ClientGuard::default(), 0);
        }

        #[test]
        fn bucket_never_exceeds_capacity(times in proptest::collection::vec(any::<u64>(), 1..100)) {
            let mut b = TokenBucket::default();
            for t in times {
                let _ = b.take(t);
                prop_assert!(b.tokens <= b.capacity);
            }
        }
    }
}

#[cfg(test)]
mod actor_jump_tests {
    use super::*;

    #[test]
    fn a_placement_names_any_actor_index_but_the_invalid_one() {
        // thuum docs/verbs/cast-teleport.md: WorldState hands out indices from
        // 0, so the first actor is idx 0 (the lab's c1 in a fresh world:
        // x-azure-player 20261019-064704 saw its snap refused with the old
        // rule); only FormIndex::g_invalidIdx (u32::MAX) is no actor
        let placement = |idx: u32| Message::ActorJump(skymp::ActorJump { t: Default::default(), idx });
        assert_eq!(validate_server(&placement(0)), Ok(()));
        assert_eq!(validate_server(&placement(1)), Ok(()));
        assert_eq!(validate_server(&placement(u32::MAX - 1)), Ok(()));
        assert_eq!(validate_server(&placement(u32::MAX)), Err(Reject::Range));
    }
}

#[cfg(test)]
mod spell_placed_tests {
    use super::*;

    fn placed(placed_by: Option<u32>, base_record_type: Option<&str>) -> Message {
        Message::CreateActor(skymp::CreateActor {
            idx: 7,
            base_record_type: base_record_type.map(|t| t.try_into().unwrap_or_default()),
            refr_id: Some(0xff00_0938),
            base_id: Some(0x1a07_35e8),
            props: skymp::CreateActorProps { placed_by, ..Default::default() },
            ..Default::default()
        })
    }

    #[test]
    fn a_placed_form_names_a_caster_above_zero_on_an_mstt_or_acti_alone() {
        // thuum docs/verbs/spell-placed-object.md (ADR-057): Raise Wall's
        // wall (MSTT) and halo (ACTI) carry their caster; a form of any other
        // kind, or a caster of 0, is no placement the server would send
        assert_eq!(validate_server(&placed(Some(0xff00_0000), Some("MSTT"))), Ok(()));
        assert_eq!(validate_server(&placed(Some(0xff00_0000), Some("ACTI"))), Ok(()));
        assert_eq!(validate_server(&placed(Some(0), Some("MSTT"))), Err(Reject::Range));
        assert_eq!(validate_server(&placed(Some(0xff00_0000), None)), Err(Reject::Range));
        assert_eq!(validate_server(&placed(Some(0xff00_0000), Some("DOOR"))), Err(Reject::Range));
        // thuum ADR-068 rung 4: a script set's placement, a static, an NPC
        // or an item, names its reporter too; an explosion never travels
        // placed
        for kind in ["STAT", "NPC_", "MISC", "WEAP", "LIGH"] {
            assert_eq!(validate_server(&placed(Some(0xff00_0000), Some(kind))), Ok(()), "{kind}");
        }
        for kind in ["EXPL", "HAZD", "PROJ", "CONT", "FURN"] {
            assert_eq!(validate_server(&placed(Some(0xff00_0000), Some(kind))), Err(Reject::Range), "{kind}");
        }
        // every other CreateActor is as it was: no placer, any base type
        assert_eq!(validate_server(&placed(None, None)), Ok(()));
        assert_eq!(validate_server(&placed(None, Some("DOOR"))), Ok(()));
        assert_eq!(validate_server(&placed(None, Some("NPC_"))), Ok(()));
    }
}

#[cfg(test)]
mod race_permit_tests {
    use super::*;

    fn permit(is_me: bool, restricted: Option<&[u32]>, permitted: Option<&[u32]>) -> Message {
        let list = |ids: &[u32]| ids.to_vec().try_into().unwrap_or_default();
        Message::CreateActor(skymp::CreateActor {
            idx: 7,
            is_me,
            refr_id: Some(0xff00_0000),
            base_id: Some(0x7),
            props: skymp::CreateActorProps {
                restricted_races: restricted.map(list),
                permitted_races: permitted.map(list),
                ..Default::default()
            },
            ..Default::default()
        })
    }

    #[test]
    fn the_permit_travels_whole_on_the_players_own_create_actor() {
        // thuum docs/verbs/character-creation.md (the permit): rotfern's two
        // races restricted, one profile permitted both, another none
        const A: u32 = 0x0800_aa00;
        const B: u32 = 0x0801_9d08;
        assert_eq!(validate_server(&permit(true, Some(&[A, B]), Some(&[A, B]))), Ok(()));
        assert_eq!(validate_server(&permit(true, Some(&[A, B]), Some(&[]))), Ok(()));
        assert_eq!(validate_server(&permit(true, Some(&[]), Some(&[]))), Ok(()));
        assert_eq!(validate_server(&permit(true, None, None)), Ok(()));
        // half a permit, a figure's, a race of 0, a repeat, a permitted race
        // that is not restricted: none is a shape the server sends
        assert_eq!(validate_server(&permit(true, Some(&[A]), None)), Err(Reject::Range));
        assert_eq!(validate_server(&permit(true, None, Some(&[A]))), Err(Reject::Range));
        assert_eq!(validate_server(&permit(false, Some(&[A]), Some(&[A]))), Err(Reject::Range));
        assert_eq!(validate_server(&permit(true, Some(&[A, 0]), Some(&[]))), Err(Reject::Range));
        assert_eq!(validate_server(&permit(true, Some(&[A, A]), Some(&[]))), Err(Reject::Range));
        assert_eq!(validate_server(&permit(true, Some(&[A]), Some(&[B]))), Err(Reject::Range));
        assert_eq!(validate_server(&permit(true, Some(&[A, B]), Some(&[A, A]))), Err(Reject::Range));
    }
}

// thuum docs/verbs/change-form.md (ADR-065 system 1): the host's change form
#[cfg(test)]
mod change_form_tests {
    use super::*;
    use proptest::prelude::*;
    use wire_schema::{skymp, Message};

    // thuum docs/verbs/change-form.md (ADR-065 system 1): a host's change
    // form is a packed record whose header must agree with its body, whose
    // hash must be the body's, and whose remap rows must decode to what
    // they claim; whose record it is and at which epoch is the server's

    /// A packed record of `form_type` around `data`: the pack's one-byte
    /// lengths under 0x100, two-byte ones under 0x10000 (35522's rule).
    fn record(form_type: u8, data: &[u8]) -> Vec<u8> {
        let mut out = Vec::new();
        if data.len() < 0x100 {
            out.push(form_type);
            out.push(CHANGE_FORM_VERSION);
            out.push(u8::try_from(data.len()).unwrap_or(0));
            out.push(0);
        } else {
            out.push(form_type | 0x40);
            out.push(CHANGE_FORM_VERSION);
            let len = u16::try_from(data.len()).unwrap_or(0).to_le_bytes();
            out.extend_from_slice(&len);
            out.extend_from_slice(&[0, 0]);
        }
        out.extend_from_slice(data);
        out
    }

    /// The record's hex as the native spells it: two lowercase digits a byte.
    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }

    fn change_form(form_id: u32, form_type: u8, body: Vec<u8>, remap: &[(u32, u32, u32)]) -> Message {
        let fnv1a = fnv1a_32(&body);
        change_form_hex(form_id, form_type, hex(&body), fnv1a, remap)
    }

    fn change_form_hex(form_id: u32, form_type: u8, body: String, fnv1a: u32, remap: &[(u32, u32, u32)]) -> Message {
        Message::ChangeForm(skymp::ChangeForm {
            epoch: 1,
            form_id,
            form_type,
            flags: 0x0400_0427,
            version: CHANGE_FORM_VERSION,
            body: body.try_into().unwrap_or_default(),
            fnv1a,
            remap: remap
                .iter()
                .map(|(offset, form_id, index)| skymp::ChangeFormRemap { offset: *offset, form_id: *form_id, index: *index })
                .collect::<Vec<_>>()
                .try_into()
                .unwrap_or_default(),
            ..Default::default()
        })
    }

    fn with_fnv(m: Message, fnv1a: u32) -> Message {
        match m {
            Message::ChangeForm(mut c) => {
                c.fnv1a = fnv1a;
                Message::ChangeForm(c)
            }
            other => other,
        }
    }

    fn with_version(m: Message, version: u8) -> Message {
        match m {
            Message::ChangeForm(mut c) => {
                c.version = version;
                Message::ChangeForm(c)
            }
            other => other,
        }
    }

    // the lab's own REFR record (run 20261103-131511: the Sleeping Giant's
    // door 0xe3a7a at `lock 50`), 40 bytes, FNV-1a 0x4ebbb261 as the Frida
    // script read it
    const LAB_REFR_RECORD: [u8; 40] = [
        0x00, 0x4e, 0x24, 0x00, 0x08, 0x32, 0x4e, 0x3a, 0x7a, 0x00, 0x2a, 0x32, 0x01, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x05, 0x00, 0x43, 0x6c, 0x6f, 0x73,
        0x65, 0x00, 0x2a, 0x00, 0x00, 0x00, 0x00, 0x00,
    ];

    #[test]
    fn the_labs_own_record_reads_and_hashes_as_the_frida_script_read_it() {
        let header = read_record_header(&LAB_REFR_RECORD);
        assert_eq!(
            header,
            Some(RecordHeader { form_type: 0, size_code: 0, version: 0x4e, length1: 36, length2: 0, header_len: 4 })
        );
        assert_eq!(fnv1a_32(&LAB_REFR_RECORD), 0x4ebb_b261);
        assert_eq!(validate_server(&change_form(0xe3a7a, 0, LAB_REFR_RECORD.to_vec(), &[])), Ok(()));
        assert_eq!(validate_server(&change_form(0xe3a7a, 1, LAB_REFR_RECORD.to_vec(), &[])), Err(Reject::ChangeFormHeader));
    }

    #[test]
    fn a_change_form_names_a_game_file_actor_and_a_six_bit_type() {
        let body = record(1, &[0x00, 0x00, 0x05, 0x7f]);
        assert_eq!(validate_server(&change_form(0x13480, 1, body.clone(), &[])), Ok(()));
        assert_eq!(validate_server(&change_form(0, 1, body.clone(), &[])), Err(Reject::ChangeFormRange));
        assert_eq!(validate_server(&change_form(0xff00_0000, 1, body.clone(), &[])), Err(Reject::ChangeFormRange));
        assert_eq!(validate_server(&change_form(0x13480, 0x40, body, &[])), Err(Reject::ChangeFormRange));
    }

    #[test]
    fn a_change_forms_body_holds_at_least_a_header() {
        assert_eq!(validate_server(&change_form(0x13480, 1, Vec::new(), &[])), Err(Reject::ChangeFormSize));
        assert_eq!(validate_server(&change_form(0x13480, 1, vec![0x01, 0x4e, 0x00], &[])), Err(Reject::ChangeFormSize));
        // the shortest record: a header and no data
        assert_eq!(validate_server(&change_form(0x13480, 1, record(1, &[]), &[])), Ok(()));
    }

    #[test]
    fn a_change_forms_body_is_lowercase_hex_of_even_length() {
        let body = record(1, &[0x00, 0x00, 0x05, 0x7f]);
        let fnv1a = fnv1a_32(&body);
        let good = hex(&body);
        assert_eq!(validate_server(&change_form_hex(0x13480, 1, good.clone(), fnv1a, &[])), Ok(()));
        // the native's hex is lowercase; the record decodes to the same bytes
        // either way, but one spelling is the contract
        assert_eq!(validate_server(&change_form_hex(0x13480, 1, good.to_uppercase(), fnv1a, &[])), Err(Reject::ChangeFormHeader));
        // a character that is not hex at all, a sign from_str_radix would take
        let mut bad = good.clone();
        bad.replace_range(4..5, "g");
        assert_eq!(validate_server(&change_form_hex(0x13480, 1, bad, fnv1a, &[])), Err(Reject::ChangeFormHeader));
        let mut signed = good.clone();
        signed.replace_range(4..5, "+");
        assert_eq!(validate_server(&change_form_hex(0x13480, 1, signed, fnv1a, &[])), Err(Reject::ChangeFormHeader));
        // an odd length holds no whole byte
        let mut odd = good.clone();
        odd.pop();
        assert_eq!(validate_server(&change_form_hex(0x13480, 1, odd, fnv1a, &[])), Err(Reject::ChangeFormSize));
        // the decoded bytes are what the header, the hash and the rows see
        assert_eq!(decode_record_hex(&good), Ok(body));
        assert_eq!(decode_record_hex(""), Ok(Vec::new()));
    }

    #[test]
    fn a_change_forms_header_agrees_with_its_body() {
        let data = [0x00, 0x00, 0x05, 0x7f];
        // the type byte against formType
        assert_eq!(validate_server(&change_form(0x13480, 1, record(0, &data), &[])), Err(Reject::ChangeFormHeader));
        // the version byte, and the message's version
        let mut old = record(1, &data);
        if let Some(v) = old.get_mut(1) {
            *v = 0x4d;
        }
        assert_eq!(validate_server(&change_form(0x13480, 1, old, &[])), Err(Reject::ChangeFormHeader));
        assert_eq!(
            validate_server(&with_version(change_form(0x13480, 1, record(1, &data), &[]), 0x4d)),
            Err(Reject::ChangeFormHeader)
        );
        // length1 off the body by one, either way
        let mut long = record(1, &data);
        if let Some(l) = long.get_mut(2) {
            *l = 5;
        }
        assert_eq!(validate_server(&change_form(0x13480, 1, long, &[])), Err(Reject::ChangeFormHeader));
        let mut short = record(1, &data);
        if let Some(l) = short.get_mut(2) {
            *l = 3;
        }
        assert_eq!(validate_server(&change_form(0x13480, 1, short, &[])), Err(Reject::ChangeFormHeader));
        // a size code the pack never writes
        let mut three = record(1, &data);
        if let Some(b) = three.get_mut(0) {
            *b |= 0xc0;
        }
        assert_eq!(validate_server(&change_form(0x13480, 1, three, &[])), Err(Reject::ChangeFormHeader));
        // a header whose lengths run past the body
        assert_eq!(validate_server(&change_form(0x13480, 1, vec![0x41, 0x4e, 0x10, 0x00], &[])), Err(Reject::ChangeFormHeader));
        // a two-byte-length record (an actor at kDying is 4,789 bytes)
        let big = vec![0x7a; 4789];
        assert_eq!(validate_server(&change_form(0x13480, 1, record(1, &big), &[])), Ok(()));
        // a deflated record's inflated size is bounded like the body
        let mut inflated = record(1, &big);
        if let Some(l) = inflated.get_mut(4..6) {
            l.copy_from_slice(&[0xff, 0xff]);
        }
        assert_eq!(validate_server(&change_form(0x13480, 1, inflated, &[])), Ok(()));
        let mut bomb = record(1, &big);
        if let Some(b) = bomb.get_mut(0) {
            *b = 0x81;
        }
        bomb.splice(2..6, [0xb5, 0x12, 0x00, 0x00, 0xff, 0xff, 0xff, 0x7f]);
        assert_eq!(validate_server(&change_form(0x13480, 1, bomb, &[])), Err(Reject::ChangeFormHeader));
    }

    #[test]
    fn a_change_forms_hash_is_the_bodys() {
        let body = record(1, &[0x00, 0x00, 0x05, 0x7f]);
        let good = change_form(0x13480, 1, body, &[]);
        assert_eq!(validate_server(&good), Ok(()));
        assert_eq!(validate_server(&with_fnv(good, 0xdead_beef)), Err(Reject::ChangeFormHash));
    }

    #[test]
    fn a_change_forms_remap_rows_decode_to_what_they_claim() {
        // the data: a kind-0 id (index 5) at offset 4, a kind-1 id at 7, pad
        let data = [0x00, 0x00, 0x05, 0x4e, 0x3a, 0x7a, 0x00, 0x00];
        let body = record(1, &data);
        const FORM: u32 = 0x0200_abcd;
        assert_eq!(validate_server(&change_form(0x13480, 1, body.clone(), &[(4, FORM, 5)])), Ok(()));
        // the index against the bytes
        assert_eq!(validate_server(&change_form(0x13480, 1, body.clone(), &[(4, FORM, 6)])), Err(Reject::ChangeFormRemap));
        // kind 1 bytes (a portable id needs no row)
        assert_eq!(validate_server(&change_form(0x13480, 1, body.clone(), &[(7, FORM, 0x0e3a7a)])), Err(Reject::ChangeFormRemap));
        // outside the data: in the header, past the end, at the very end
        assert_eq!(validate_server(&change_form(0x13480, 1, body.clone(), &[(1, FORM, 0x4e0000)])), Err(Reject::ChangeFormRemap));
        assert_eq!(validate_server(&change_form(0x13480, 1, body.clone(), &[(12, FORM, 0)])), Err(Reject::ChangeFormRemap));
        assert_eq!(validate_server(&change_form(0x13480, 1, body.clone(), &[(10, FORM, 0)])), Err(Reject::ChangeFormRemap));
        assert_eq!(validate_server(&change_form(0x13480, 1, body.clone(), &[(u32::MAX, FORM, 0)])), Err(Reject::ChangeFormRemap));
        // forms no row may remap: 0, a load-index-00 form, a created one
        assert_eq!(validate_server(&change_form(0x13480, 1, body.clone(), &[(4, 0, 5)])), Err(Reject::ChangeFormRemap));
        assert_eq!(validate_server(&change_form(0x13480, 1, body.clone(), &[(4, 0x0001_2345, 5)])), Err(Reject::ChangeFormRemap));
        assert_eq!(validate_server(&change_form(0x13480, 1, body.clone(), &[(4, 0xff00_0001, 5)])), Err(Reject::ChangeFormRemap));
        // an offset twice
        assert_eq!(
            validate_server(&change_form(0x13480, 1, body, &[(4, FORM, 5), (4, 0x0200_abce, 5)])),
            Err(Reject::ChangeFormRemap)
        );
    }

    #[test]
    fn change_forms_are_rate_limited_per_host() {
        let body = record(1, &[0x00, 0x00, 0x05, 0x7f]);
        let report = change_form(0x13480, 1, body, &[]);
        let mut guard = ClientGuard::default();
        for _ in 0..32 {
            assert_eq!(validate(&report, &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&report, &mut guard, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&report, &mut guard, 2_000), Ok(()));
    }

    #[test]
    fn the_servers_ack_and_apply_keep_the_records_shape() {
        use wire_schema::skymp::{ChangeFormAck, ChangeFormApply, ChangeFormRemap};
        assert_eq!(validate_server(&Message::ChangeFormAck(ChangeFormAck { form_id: 0x13480, fnv1a: 1, ..Default::default() })), Ok(()));
        assert_eq!(validate_server(&Message::ChangeFormAck(ChangeFormAck { form_id: 0, fnv1a: 1, ..Default::default() })), Err(Reject::ChangeFormRange));
        let data = [0x00, 0x00, 0x05, 0x7f];
        let apply = |form_type: u8, body: Vec<u8>, remap: &[(u32, u32, u32)]| {
            Message::ChangeFormApply(ChangeFormApply {
                form_id: 0x13480,
                form_type,
                flags: 0x0400_0427,
                version: CHANGE_FORM_VERSION,
                body: hex(&body).try_into().unwrap_or_default(),
                remap: remap
                    .iter()
                    .map(|(offset, form_id, index)| ChangeFormRemap { offset: *offset, form_id: *form_id, index: *index })
                    .collect::<Vec<_>>()
                    .try_into()
                    .unwrap_or_default(),
                ..Default::default()
            })
        };
        assert_eq!(validate_server(&apply(1, record(1, &data), &[(4, 0x0200_abcd, 5)])), Ok(()));
        assert_eq!(validate_server(&apply(0, record(1, &data), &[])), Err(Reject::ChangeFormHeader));
        assert_eq!(validate_server(&apply(1, record(1, &data), &[(4, 0x0200_abcd, 6)])), Err(Reject::ChangeFormRemap));
        assert_eq!(validate_server(&apply(1, Vec::new(), &[])), Err(Reject::ChangeFormSize));
    }

    proptest! {
        // any bytes and rows: a verdict, never a panic (the fuzz target's
        // property in miniature)
        #[test]
        fn change_form_checks_never_panic(body in proptest::collection::vec(any::<u8>(), 0..300),
                                          text in "[0-9a-fA-Fxyz+ ]{0,80}",
                                          rows in proptest::collection::vec((any::<u32>(), any::<u32>(), any::<u32>()), 0..8),
                                          form_id in any::<u32>(), form_type in any::<u8>(), fnv1a in any::<u32>()) {
            let _ = validate_server(&change_form(form_id, form_type, body, &rows));
            let _ = validate_server(&change_form_hex(form_id, form_type, text, fnv1a, &rows));
        }
    }
}

#[cfg(test)]
mod script_call_tests {
    use super::*;
    use proptest::prelude::*;
    use wire_schema::cap;
    use wire_schema::skymp::{ScriptCall, SnippetArg, SnippetObject};
    use wire_schema::FormId;

    fn object(form_id: u64, kind: &str) -> Option<SnippetArg> {
        Some(SnippetArg::Object(SnippetObject { form_id, kind: kind.try_into().unwrap_or_default() }))
    }

    fn text(s: &str) -> Option<SnippetArg> {
        Some(SnippetArg::String(s.try_into().unwrap_or_default()))
    }

    fn call(self_id: u32, cls: &str, func: &str, args: Vec<Option<SnippetArg>>) -> Message {
        Message::ScriptCall(ScriptCall {
            epoch: 3,
            self_id,
            cls: cls.try_into().unwrap_or_default(),
            func: func.try_into().unwrap_or_default(),
            args: args.try_into().unwrap_or_default(),
            caller: "defaultPuzzlePillarScript".try_into().unwrap_or_default(),
            handler: "onActivate".try_into().unwrap_or_default(),
            latent: true,
            ..Default::default()
        })
    }

    #[test]
    fn a_reported_call_names_a_class_a_function_and_a_subject_the_client_may_name() {
        // thuum ADR-068: the pillar's turn (Bleak Falls Temple's 0xaa8cb), a
        // static call on a world object, the player's own 0x14, an absent
        // argument; the row, the kinds and the owner are the server's rule
        assert_eq!(validate_server(&call(0xaa8cb, "ObjectReference", "PlayAnimationAndWait", vec![text("Trigger02"), text("Turned02")])), Ok(()));
        assert_eq!(validate_server(&call(0x10e6c5, "Debug", "SendAnimationEvent", vec![object(0x10e6c5, "ObjectReference"), text("Open")])), Ok(()));
        assert_eq!(validate_server(&call(0x14, "ObjectReference", "PlayAnimation", vec![text("Open")])), Ok(()));
        assert_eq!(validate_server(&call(0xaa8cb, "ObjectReference", "PlayAnimation", vec![None])), Ok(()));
        assert_eq!(validate_server(&call(0xaa8cb, "Debug", "Notification", vec![])), Ok(()), "an untabled pair is the rule's to refuse");
        // the names, never empty
        assert_eq!(validate_server(&call(0xaa8cb, "", "PlayAnimation", vec![text("Open")])), Err(Reject::Range));
        assert_eq!(validate_server(&call(0xaa8cb, "ObjectReference", "", vec![text("Open")])), Err(Reject::Range));
        // what the server names: its own forms and the players' figures, as
        // the subject, and no form past 32 bits
        assert_eq!(validate_server(&call(0xff00_0002, "ObjectReference", "PlayAnimation", vec![text("Open")])), Err(Reject::Range));
        assert_eq!(validate_server(&call(0xfeff_ffff, "ObjectReference", "PlayAnimation", vec![text("Open")])), Ok(()));
        assert_eq!(validate_server(&call(0x10e6c5, "Debug", "SendAnimationEvent", vec![object(0x1_0010_e6c5, "ObjectReference"), text("Open")])), Err(Reject::Range));
        // thuum ADR-068 rung 5: a scoped call's figure, an object argument
        // naming a player's actor by its server id, passes the edge to the
        // server's rule (wire-rules natives::figures takes a connected
        // player's alone); the top of the 32 bits too, nothing past them
        assert_eq!(validate_server(&call(0x10e6c5, "Debug", "SendAnimationEvent", vec![object(0xff00_0002, "Actor"), text("Open")])), Ok(()));
        assert_eq!(validate_server(&call(0x39655, "Actor", "SetRelationshipRank", vec![object(0xffff_ffff, "Actor"), Some(SnippetArg::Number(1.0))])), Ok(()));
        assert_eq!(validate_server(&call(0x39655, "Actor", "SetRelationshipRank", vec![object(0x1_0000_0000, "Actor"), Some(SnippetArg::Number(1.0))])), Err(Reject::Range));
        // every number a number
        assert_eq!(validate_server(&call(0xaa8cb, "ObjectReference", "SetAnimationVariableFloat", vec![text("fSpeed"), Some(SnippetArg::Number(f64::NAN))])), Err(Reject::NonFinite));
        assert_eq!(validate_server(&call(0xaa8cb, "ObjectReference", "SetAnimationVariableFloat", vec![text("fSpeed"), Some(SnippetArg::Number(1.5))])), Ok(()));
    }

    #[test]
    fn the_arguments_stop_at_the_engines_eleven() {
        // CommonLibSSE-NG src/RE/N/NativeFunctionBase.cpp:15: a native takes
        // at most eleven; a twelfth fails at decode, before any check
        let eleven: Vec<Option<SnippetArg>> = (0..cap::SCRIPT_CALL_ARGS).map(|_| Some(SnippetArg::Bool(true))).collect();
        let held: Result<wire_schema::bounded::Vec<Option<SnippetArg>, { cap::SCRIPT_CALL_ARGS }>, _> = eleven.clone().try_into();
        assert!(held.is_ok());
        let mut twelve = eleven;
        twelve.push(Some(SnippetArg::Bool(true)));
        let over: Result<wire_schema::bounded::Vec<Option<SnippetArg>, { cap::SCRIPT_CALL_ARGS }>, _> = twelve.try_into();
        assert!(over.is_err());
    }

    #[test]
    fn the_rate_is_the_rules_not_the_edges() {
        // thuum ADR-068: the sender's budget (32 then 8 a second) is the
        // server's rule's, E_SCRIPTCALL_RATE by name (wire-rules natives);
        // the edge takes every well-formed call
        let mut guard = ClientGuard::default();
        for _ in 0..100 {
            assert_eq!(validate(&call(0xaa8cb, "ObjectReference", "PlayAnimation", vec![text("Open")]), &mut guard, 0), Ok(()));
        }
    }

    // thuum D-R1 C's leveled gap (SCHEMA_VERSION 44): MS01's stage 39,
    // `Game.GetPlayer().AddItem(LvlQuestReward02Medium, 1, False)`, the list
    // 0xc0405 whose roll landed Gold001 0xf x400 (x-additem-functor
    // 20261110-151634)
    const REWARD: u32 = 0xc0405;

    fn reward(self_id: u32, cls: &str, func: &str, landed: &[(u32, i32)]) -> Message {
        let Message::ScriptCall(c) = call(self_id, cls, func, vec![object(u64::from(REWARD), "LeveledItem"), Some(SnippetArg::Number(1.0)), Some(SnippetArg::Bool(false))]) else {
            return Message::Refuse { reason: 0 };
        };
        let landed: Vec<ItemDelta> = landed.iter().map(|&(item, count)| ItemDelta { item: FormId(item), count }).collect();
        Message::ScriptCall(ScriptCall { landed: landed.try_into().ok(), caller: "QF_MS01_00018B4B".try_into().unwrap_or_default(), handler: "Fragment_56".try_into().unwrap_or_default(), ..c })
    }

    #[test]
    fn a_leveled_rolls_landings_ride_the_players_own_additem_alone() {
        // the roll's one base, an empty roll, a call with no landings at all
        assert_eq!(validate_server(&reward(0x14, "ObjectReference", "AddItem", &[(0xf, 400)])), Ok(()));
        assert_eq!(validate_server(&reward(0x14, "objectreference", "additem", &[(0xf, 400), (0x12eb7, 1)])), Ok(()), "names without case; the reach is the server's");
        assert_eq!(validate_server(&reward(0x14, "ObjectReference", "AddItem", &[])), Ok(()));
        assert_eq!(validate_server(&call(0x14, "ObjectReference", "AddItem", vec![object(u64::from(REWARD), "LeveledItem")])), Ok(()));
        // on another native or another subject: E_VAL_RANGE
        assert_eq!(validate_server(&reward(0x14, "ObjectReference", "RemoveItem", &[(0xf, 400)])), Err(Reject::Range));
        assert_eq!(validate_server(&reward(0x14, "Actor", "SetRace", &[(0xf, 400)])), Err(Reject::Range));
        assert_eq!(validate_server(&reward(0x10e6c5, "ObjectReference", "AddItem", &[(0xf, 400)])), Err(Reject::Range));
        assert_eq!(validate_server(&reward(0x10e6c5, "ObjectReference", "AddItem", &[])), Err(Reject::Range), "an empty list is still a landing");
        // each landing an item of the load order, a count above zero, a base once
        assert_eq!(validate_server(&reward(0x14, "ObjectReference", "AddItem", &[(0, 400)])), Err(Reject::Range));
        assert_eq!(validate_server(&reward(0x14, "ObjectReference", "AddItem", &[(0xff00_0001, 1)])), Err(Reject::Range));
        assert_eq!(validate_server(&reward(0x14, "ObjectReference", "AddItem", &[(0xfeff_ffff, 1)])), Ok(()));
        assert_eq!(validate_server(&reward(0x14, "ObjectReference", "AddItem", &[(0xf, 0)])), Err(Reject::ZeroDelta));
        assert_eq!(validate_server(&reward(0x14, "ObjectReference", "AddItem", &[(0xf, -1)])), Err(Reject::Range));
        assert_eq!(validate_server(&reward(0x14, "ObjectReference", "AddItem", &[(0xf, i32::MAX)])), Ok(()));
        assert_eq!(validate_server(&reward(0x14, "ObjectReference", "AddItem", &[(0xf, 400), (0x12eb7, 1), (0xf, 25)])), Err(Reject::Range));
        // the client's edge says the same
        assert_eq!(validate(&reward(0x14, "ObjectReference", "AddItem", &[(0xf, 0)]), &mut ClientGuard::default(), 0), Err(Reject::ZeroDelta));
    }

    #[test]
    fn the_landings_stop_at_a_leveled_lists_255_entries() {
        // TESLeveledList's entry count is one byte (CommonLibSSE-NG
        // include/RE/T/TESLeveledList.h:72): 255 bases land at most, and a
        // 256th fails at the bound (E_WIRE_CAPACITY), before any check
        let bases: Vec<(u32, i32)> = (0x1000u32..).take(cap::SCRIPT_CALL_LANDED).map(|item| (item, 1)).collect();
        let full = reward(0x14, "ObjectReference", "AddItem", &bases);
        assert!(matches!(&full, Message::ScriptCall(c) if c.landed.as_ref().map(|l| l.len()) == Some(cap::SCRIPT_CALL_LANDED)));
        assert_eq!(validate_server(&full), Ok(()));
        let over: Vec<ItemDelta> = (0x1000u32..).take(cap::SCRIPT_CALL_LANDED.saturating_add(1)).map(|item| ItemDelta { item: FormId(item), count: 1 }).collect();
        let held: Result<wire_schema::bounded::Vec<ItemDelta, { cap::SCRIPT_CALL_LANDED }>, _> = over.try_into();
        assert_eq!(held.map(|_| ()).map_err(|e| e.to_string()), Err("E_WIRE_CAPACITY: 256 > 255".to_owned()));
    }

    proptest! {
        // any landings on any call: a verdict, never a panic; on the
        // player's own AddItem a landing in range passes and one out of it
        // is refused
        #[test]
        fn landings_never_panic(landed in proptest::collection::vec((any::<u32>(), any::<i32>()), 0..40), self_id in any::<u32>(), add in any::<bool>()) {
            let func = if add { "AddItem" } else { "RemoveItem" };
            let _ = validate_server(&reward(self_id, "ObjectReference", func, &landed));
            let _ = validate(&reward(self_id, "ObjectReference", func, &landed), &mut ClientGuard::default(), 0);
        }

        #[test]
        fn a_landing_in_range_passes_and_one_outside_is_refused(item in 1u32..0xff00_0000, count in 1i32.., bad_item in prop_oneof![Just(0u32), 0xff00_0000u32..], bad_count in i32::MIN..1) {
            prop_assert_eq!(validate_server(&reward(0x14, "ObjectReference", "AddItem", &[(item, count)])), Ok(()));
            prop_assert_eq!(validate_server(&reward(0x14, "ObjectReference", "AddItem", &[(bad_item, count)])), Err(Reject::Range));
            let refused = validate_server(&reward(0x14, "ObjectReference", "AddItem", &[(item, bad_count)]));
            prop_assert_eq!(refused, Err(if bad_count == 0 { Reject::ZeroDelta } else { Reject::Range }));
        }
    }
}

#[cfg(test)]
mod script_state_tests {
    use super::*;
    use proptest::prelude::*;
    use wire_schema::cap;
    use wire_schema::skymp::{ScriptHandle, ScriptObjectState, ScriptState, ScriptStateApply, ScriptValue, ScriptVar};

    // thuum ADR-068 rung 3: Bleak Falls Temple's pillar 0xaa8cb, its
    // defaultPuzzlePillarScript in its turned state (the census's five
    // variables: raw types 3, 3, 5, a class, 5), the class one the puzzle
    // lever 0xaa8c3's plain form handle (kind 0, mid 0xFFFF)
    const PILLAR: u32 = 0xaa8cb;

    fn lever() -> ScriptHandle {
        ScriptHandle { kind: 0, mid: 0xffff, form: 0xaa8c3, cls: "defaultPillarPuzzleLever".try_into().unwrap_or_default() }
    }

    fn var(k: u8) -> ScriptVar {
        ScriptVar { k, ..Default::default() }
    }

    fn pillar_vars() -> Vec<ScriptVar> {
        vec![
            ScriptVar { i: Some(1), ..var(3) },
            ScriptVar { i: Some(3), ..var(3) },
            ScriptVar { b: Some(true), ..var(5) },
            ScriptVar { o: Some(lever()), ..var(1) },
            ScriptVar { b: Some(false), ..var(5) },
        ]
    }

    fn script(vars: Vec<ScriptVar>) -> ScriptObjectState {
        ScriptObjectState {
            cls: "defaultPuzzlePillarScript".try_into().unwrap_or_default(),
            state: "position02".try_into().unwrap_or_default(),
            flags: 0x0b,
            vars: vars.try_into().unwrap_or_default(),
        }
    }

    fn record(form_id: u32, scripts: Vec<ScriptObjectState>) -> Message {
        Message::ScriptState(ScriptState { epoch: 1, form_id, fnv1a: 0x5c1a_7e01, scripts: scripts.try_into().unwrap_or_default(), ..Default::default() })
    }

    fn with_var(v: ScriptVar) -> Message {
        let mut vars = pillar_vars();
        vars.push(v);
        record(PILLAR, vec![script(vars)])
    }

    #[test]
    fn the_pillars_record_is_a_record() {
        assert_eq!(validate_server(&record(PILLAR, vec![script(pillar_vars())])), Ok(()));
        // the empty state, a script with no variables, every kind of value
        let empty = ScriptObjectState { state: "".try_into().unwrap_or_default(), ..script(vec![]) };
        assert_eq!(validate_server(&record(PILLAR, vec![empty])), Ok(()));
        let every = vec![
            var(0),
            var(1),
            ScriptVar { s: Some("Turned02".try_into().unwrap_or_default()), ..var(2) },
            ScriptVar { f: Some(1.5), ..var(4) },
        ];
        assert_eq!(validate_server(&record(PILLAR, vec![script(every)])), Ok(()));
    }

    #[test]
    fn a_record_names_a_game_file_reference_and_one_to_sixteen_scripts() {
        let one = || vec![script(pillar_vars())];
        assert_eq!(validate_server(&record(0, one())), Err(Reject::Range));
        assert_eq!(validate_server(&record(0xff00_0000, one())), Err(Reject::Range));
        assert_eq!(validate_server(&record(u32::MAX, one())), Err(Reject::Range));
        assert_eq!(validate_server(&record(0xfeff_ffff, one())), Ok(()));
        assert_eq!(validate_server(&record(PILLAR, vec![])), Err(Reject::Range));
        let sixteen: Vec<ScriptObjectState> = (0..cap::SCRIPT_OBJECTS).map(|_| script(pillar_vars())).collect();
        assert_eq!(validate_server(&record(PILLAR, sixteen.clone())), Ok(()));
        // a seventeenth fails at decode, before any check
        let mut seventeen = sixteen;
        seventeen.push(script(pillar_vars()));
        let over: Result<wire_schema::bounded::Vec<ScriptObjectState, { cap::SCRIPT_OBJECTS }>, _> = seventeen.try_into();
        assert!(over.is_err());
        // a script with no type
        let untyped = ScriptObjectState { cls: "".try_into().unwrap_or_default(), ..script(pillar_vars()) };
        assert_eq!(validate_server(&record(PILLAR, vec![untyped])), Err(Reject::Range));
    }

    #[test]
    fn a_record_is_read_at_a_quiescent_point() {
        // Object::Save's byte: 0x01 constructed, 0x02 valid, 0x04 lock held,
        // 0x08 initialized
        let flagged = |flags| record(PILLAR, vec![ScriptObjectState { flags, ..script(pillar_vars()) }]);
        assert_eq!(validate_server(&flagged(0x09)), Ok(()));
        assert_eq!(validate_server(&flagged(0x0b)), Ok(()));
        assert_eq!(validate_server(&flagged(0x08)), Err(Reject::Range), "not constructed");
        assert_eq!(validate_server(&flagged(0x03)), Err(Reject::Range), "not initialized");
        assert_eq!(validate_server(&flagged(0x0f)), Err(Reject::Range), "the lock held");
    }

    #[test]
    fn a_value_carries_the_one_field_its_kind_names() {
        let text = || Some("x".try_into().unwrap_or_default());
        assert_eq!(validate_server(&with_var(var(0))), Ok(()));
        assert_eq!(validate_server(&with_var(ScriptVar { i: Some(1), ..var(0) })), Err(Reject::Range), "a None carries nothing");
        assert_eq!(validate_server(&with_var(ScriptVar { o: Some(lever()), ..var(0) })), Err(Reject::Range));
        assert_eq!(validate_server(&with_var(ScriptVar { i: Some(1), ..var(1) })), Err(Reject::Range), "an object carries its handle alone");
        assert_eq!(validate_server(&with_var(var(2))), Err(Reject::Range), "a string carries its string");
        assert_eq!(validate_server(&with_var(ScriptVar { s: text(), ..var(2) })), Ok(()));
        assert_eq!(validate_server(&with_var(ScriptVar { s: text(), i: Some(1), ..var(2) })), Err(Reject::Range));
        assert_eq!(validate_server(&with_var(var(3))), Err(Reject::Range), "an int carries its int");
        assert_eq!(validate_server(&with_var(ScriptVar { f: Some(1.0), ..var(3) })), Err(Reject::Range));
        assert_eq!(validate_server(&with_var(ScriptVar { f: Some(-0.5), ..var(4) })), Ok(()));
        assert_eq!(validate_server(&with_var(ScriptVar { b: Some(true), ..var(4) })), Err(Reject::Range));
        assert_eq!(validate_server(&with_var(ScriptVar { b: Some(false), o: Some(lever()), ..var(5) })), Err(Reject::Range));
        // the kinds the VM has: 0 to 5 and the arrays 10 to 15
        for k in [6, 7, 8, 9, 16, 255] {
            assert_eq!(validate_server(&with_var(var(k))), Err(Reject::Range), "kind {k}");
        }
    }

    #[test]
    fn an_array_holds_values_of_its_element_kind() {
        let ints = |values: &[i32]| Some(values.iter().map(|i| ScriptValue { k: 3, i: Some(*i), ..Default::default() }).collect::<Vec<_>>().try_into().unwrap_or_default());
        assert_eq!(validate_server(&with_var(ScriptVar { a: ints(&[1, 3]), ..var(13) })), Ok(()));
        // a None array, and an empty one
        assert_eq!(validate_server(&with_var(var(12))), Ok(()));
        assert_eq!(validate_server(&with_var(ScriptVar { a: ints(&[]), ..var(13) })), Ok(()));
        // an element of another kind
        assert_eq!(validate_server(&with_var(ScriptVar { a: ints(&[1]), ..var(14) })), Err(Reject::Range));
        // an object array names its element type, and only an object array
        let objects = Some(vec![ScriptValue { k: 1, o: Some(lever()), ..Default::default() }, ScriptValue { k: 1, ..Default::default() }].try_into().unwrap_or_default());
        assert_eq!(validate_server(&with_var(ScriptVar { elem: Some("ObjectReference".try_into().unwrap_or_default()), a: objects.clone(), ..var(11) })), Ok(()));
        assert_eq!(validate_server(&with_var(ScriptVar { a: objects, ..var(11) })), Ok(()), "its element type may be the declared one's");
        assert_eq!(validate_server(&with_var(ScriptVar { elem: Some("Int".try_into().unwrap_or_default()), a: ints(&[1]), ..var(13) })), Err(Reject::Range));
        assert_eq!(validate_server(&with_var(ScriptVar { elem: Some("Int".try_into().unwrap_or_default()), i: Some(1), ..var(3) })), Err(Reject::Range));
        // an array carries no value of its own, a value no array
        assert_eq!(validate_server(&with_var(ScriptVar { i: Some(1), a: ints(&[1]), ..var(13) })), Err(Reject::Range));
        assert_eq!(validate_server(&with_var(ScriptVar { i: Some(1), a: ints(&[1]), ..var(3) })), Err(Reject::Range));
        // the None array (kind 10) is a value with no elements: no `a` at all,
        // not even an empty one, and no element type (the platform's apply
        // refuses any, ScriptState.cpp's ParseVar)
        assert_eq!(validate_server(&with_var(var(10))), Ok(()));
        let nones = Some(vec![ScriptValue::default()].try_into().unwrap_or_default());
        assert_eq!(validate_server(&with_var(ScriptVar { a: nones, ..var(10) })), Err(Reject::Range));
        assert_eq!(validate_server(&with_var(ScriptVar { a: Some(Default::default()), ..var(10) })), Err(Reject::Range));
        assert_eq!(validate_server(&with_var(ScriptVar { elem: Some("ObjectReference".try_into().unwrap_or_default()), ..var(10) })), Err(Reject::Range));
        // the VM's 128 elements hold at decode (CommonLibSSE-NG
        // include/RE/A/Array.h:69)
        let full: Vec<ScriptValue> = (0..cap::SCRIPT_ARRAY).map(|_| ScriptValue { k: 5, b: Some(true), ..Default::default() }).collect();
        let held: Result<wire_schema::bounded::Vec<ScriptValue, { cap::SCRIPT_ARRAY }>, _> = full.clone().try_into();
        assert!(held.is_ok());
        let mut over = full;
        over.push(ScriptValue { k: 5, b: Some(true), ..Default::default() });
        let refused: Result<wire_schema::bounded::Vec<ScriptValue, { cap::SCRIPT_ARRAY }>, _> = over.try_into();
        assert!(refused.is_err());
    }

    #[test]
    fn a_handle_names_a_form_every_seat_shares() {
        let handle = |kind, form| with_var(ScriptVar { o: Some(ScriptHandle { kind, form, ..lever() }), ..var(1) });
        assert_eq!(validate_server(&handle(1, 0xaa8c3)), Ok(()), "a kind a receiver reads as None");
        assert_eq!(validate_server(&handle(2, 0xaa8c3)), Ok(()));
        assert_eq!(validate_server(&handle(3, 0xaa8c3)), Err(Reject::Range));
        assert_eq!(validate_server(&handle(0, 0x14)), Err(Reject::Range), "a seat's own player");
        assert_eq!(validate_server(&handle(0, 0xff00_0002)), Err(Reject::Range), "a form the server names");
        // inside an object array too
        let element = |form| Some(vec![ScriptValue { k: 1, o: Some(ScriptHandle { form, ..lever() }), ..Default::default() }].try_into().unwrap_or_default());
        assert_eq!(validate_server(&with_var(ScriptVar { a: element(0xaa8c3), ..var(11) })), Ok(()));
        assert_eq!(validate_server(&with_var(ScriptVar { a: element(0xff00_0002), ..var(11) })), Err(Reject::Range));
        // and the script type bound at it, which the receiver's
        // FindBoundObject takes: never empty (the platform's apply refuses
        // an object with no class, ScriptState.cpp's ParseValue)
        let typed = |cls: &str| ScriptHandle { cls: cls.try_into().unwrap_or_default(), ..lever() };
        assert_eq!(validate_server(&with_var(ScriptVar { o: Some(typed("defaultPillarPuzzleLever")), ..var(1) })), Ok(()));
        assert_eq!(validate_server(&with_var(ScriptVar { o: Some(typed("")), ..var(1) })), Err(Reject::Range));
        let untyped = Some(vec![ScriptValue { k: 1, o: Some(typed("")), ..Default::default() }].try_into().unwrap_or_default());
        assert_eq!(validate_server(&with_var(ScriptVar { a: untyped, ..var(11) })), Err(Reject::Range));
    }

    #[test]
    fn no_string_or_name_holds_a_nul() {
        // the receiver's BSFixedString reads a C string, and the platform's
        // apply refuses the whole record for a NUL anywhere it reads one
        // (ScriptState.cpp's ReadString): a string value, alone or in an
        // array, a script's type and state, an object array's element type,
        // a handle's type
        let text = |s: &str| -> Option<wire_schema::bounded::String<{ cap::SCRIPT_STRING }>> { Some(s.try_into().unwrap_or_default()) };
        assert_eq!(validate_server(&with_var(ScriptVar { s: text("Turned02"), ..var(2) })), Ok(()));
        assert_eq!(validate_server(&with_var(ScriptVar { s: text("Turned02\0"), ..var(2) })), Err(Reject::Range));
        assert_eq!(validate_server(&with_var(ScriptVar { s: text("\0"), ..var(2) })), Err(Reject::Range));
        let strings = |s: &str| Some(vec![ScriptValue { k: 2, s: text(s), ..Default::default() }].try_into().unwrap_or_default());
        assert_eq!(validate_server(&with_var(ScriptVar { a: strings("Turned02"), ..var(12) })), Ok(()));
        assert_eq!(validate_server(&with_var(ScriptVar { a: strings("Turned\0Two"), ..var(12) })), Err(Reject::Range));
        let named = |cls: &str, state: &str| {
            record(PILLAR, vec![ScriptObjectState { cls: cls.try_into().unwrap_or_default(), state: state.try_into().unwrap_or_default(), ..script(pillar_vars()) }])
        };
        assert_eq!(validate_server(&named("defaultPuzzlePillarScript", "position02")), Ok(()));
        assert_eq!(validate_server(&named("defaultPuzzle\0PillarScript", "position02")), Err(Reject::Range));
        assert_eq!(validate_server(&named("defaultPuzzlePillarScript", "position02\0")), Err(Reject::Range));
        // an empty state is the empty state, not a cut one
        assert_eq!(validate_server(&named("defaultPuzzlePillarScript", "")), Ok(()));
        let elem = |e: &str| with_var(ScriptVar { elem: Some(e.try_into().unwrap_or_default()), ..var(11) });
        assert_eq!(validate_server(&elem("ObjectReference")), Ok(()));
        assert_eq!(validate_server(&elem("Object\0Reference")), Err(Reject::Range));
        // an empty element type cuts nothing: the declared one's stands
        assert_eq!(validate_server(&elem("")), Ok(()));
        let handle = |cls: &str| ScriptHandle { cls: cls.try_into().unwrap_or_default(), ..lever() };
        assert_eq!(validate_server(&with_var(ScriptVar { o: Some(handle("defaultPillar\0PuzzleLever")), ..var(1) })), Err(Reject::Range));
        let element = Some(vec![ScriptValue { k: 1, o: Some(handle("\0")), ..Default::default() }].try_into().unwrap_or_default());
        assert_eq!(validate_server(&with_var(ScriptVar { a: element, ..var(11) })), Err(Reject::Range));
    }

    #[test]
    fn a_float_is_finite() {
        assert_eq!(validate_server(&with_var(ScriptVar { f: Some(f32::NAN), ..var(4) })), Err(Reject::NonFinite));
        assert_eq!(validate_server(&with_var(ScriptVar { f: Some(f32::INFINITY), ..var(4) })), Err(Reject::NonFinite));
        let array = Some(vec![ScriptValue { k: 4, f: Some(f32::NEG_INFINITY), ..Default::default() }].try_into().unwrap_or_default());
        assert_eq!(validate_server(&with_var(ScriptVar { a: array, ..var(14) })), Err(Reject::NonFinite));
    }

    #[test]
    fn the_apply_obeys_the_records_shape() {
        let apply = |form_id, event: Option<&str>, scripts: Vec<ScriptObjectState>| {
            Message::ScriptStateApply(ScriptStateApply {
                form_id,
                fnv1a: 0x5c1a_7e01,
                seq: 2,
                event: event.map(|e| e.try_into().unwrap_or_default()),
                scripts: scripts.try_into().unwrap_or_default(),
                ..Default::default()
            })
        };
        assert_eq!(validate_server(&apply(PILLAR, Some("Trigger01"), vec![script(pillar_vars())])), Ok(()));
        assert_eq!(validate_server(&apply(PILLAR, None, vec![script(pillar_vars())])), Ok(()), "a record with no view");
        assert_eq!(validate_server(&apply(0, None, vec![script(pillar_vars())])), Err(Reject::Range));
        assert_eq!(validate_server(&apply(PILLAR, None, vec![])), Err(Reject::Range));
        let mut bad = pillar_vars();
        bad.push(var(6));
        assert_eq!(validate_server(&apply(PILLAR, None, vec![script(bad)])), Err(Reject::Range));
        // a NUL in a served name, as in a reported one
        let cut = ScriptObjectState { state: "position\0One".try_into().unwrap_or_default(), ..script(pillar_vars()) };
        assert_eq!(validate_server(&apply(PILLAR, Some("Trigger01"), vec![cut])), Err(Reject::Range));
    }

    #[test]
    fn an_owners_records_are_rate_limited() {
        let mut guard = ClientGuard::default();
        let ok = record(PILLAR, vec![script(pillar_vars())]);
        for _ in 0..64 {
            assert_eq!(validate(&ok, &mut guard, 1_000), Ok(()));
        }
        assert_eq!(validate(&ok, &mut guard, 1_000), Err(Reject::Rate));
        // one token every 125 ms
        assert_eq!(validate(&ok, &mut guard, 1_124), Err(Reject::Rate));
        assert_eq!(validate(&ok, &mut guard, 1_125), Ok(()));
        // a refused shape spends no token: the shape is judged first
        let mut fresh = ClientGuard::default();
        for _ in 0..100 {
            assert_eq!(validate(&record(0, vec![script(pillar_vars())]), &mut fresh, 1_000), Err(Reject::Range));
        }
        assert_eq!(validate(&ok, &mut fresh, 1_000), Ok(()));
        // the server's apply takes no client budget
        let served = Message::ScriptStateApply(ScriptStateApply { form_id: PILLAR, scripts: vec![script(pillar_vars())].try_into().unwrap_or_default(), ..Default::default() });
        let mut other = ClientGuard::default();
        for _ in 0..100 {
            assert_eq!(validate(&served, &mut other, 1_000), Ok(()));
        }
    }

    // a value as the generator drew it: its kind; which of the int, float,
    // bool and string fields are present; whether its string holds a NUL;
    // and its handle when there is one (its kind, its form, and its type as a
    // name code)
    type Draw = (u8, [bool; 4], bool, Option<(u8, u32, u8)>);

    // a name as a code: mostly the stage's own (0), else empty (1) or the
    // stage's own with a NUL (2)
    fn name_strategy() -> impl Strategy<Value = u8> {
        prop_oneof![4 => Just(0u8), 1 => Just(1u8), 1 => Just(2u8)]
    }

    fn name_of<const N: usize>(code: u8, own: &str) -> wire_schema::bounded::String<N> {
        let text = match code {
            0 => own.to_owned(),
            1 => String::new(),
            _ => format!("{own}\0"),
        };
        text.as_str().try_into().unwrap_or_default()
    }

    fn value_strategy() -> impl Strategy<Value = Draw> {
        (
            0u8..17,
            any::<[bool; 4]>(),
            proptest::bool::weighted(0.25),
            proptest::option::of((0u8..4, prop_oneof![Just(0x14u32), Just(0xaa8c3u32), Just(0xff00_0002u32), any::<u32>()], name_strategy())),
        )
    }

    // a value's payload as the rules judge it: the one field its kind names, a
    // string with no NUL, a handle on an object alone with a kind of 0 to 2, a
    // form every seat shares and a type neither empty nor cut by a NUL
    fn payload_ok((k, carried, s_nul, handle): Draw) -> bool {
        let expected = match k {
            0 | 1 => [false; 4],
            2 => [false, false, false, true],
            3 => [true, false, false, false],
            4 => [false, true, false, false],
            5 => [false, false, true, false],
            _ => return false,
        };
        let [_, _, _, s] = carried;
        carried == expected
            && !(s && s_nul)
            && (k == 1 || handle.is_none())
            && handle.is_none_or(|(kind, form, name)| kind <= 2 && form != 0x14 && form < 0xff00_0000 && name == 0)
    }

    fn value_of((k, carried, s_nul, handle): Draw) -> ScriptValue {
        let [i, f, b, s] = carried;
        ScriptValue {
            k,
            i: i.then_some(7),
            f: f.then_some(0.25),
            b: b.then_some(true),
            s: s.then(|| (if s_nul { "x\0" } else { "x" }).try_into().unwrap_or_default()),
            o: handle.map(|(kind, form, name)| ScriptHandle { kind, form, cls: name_of(name, "defaultPillarPuzzleLever"), ..lever() }),
        }
    }

    proptest! {
        // any kinds, payloads, handles, names, flags and forms: a verdict,
        // never a panic, and the verdict the record's rules give
        #[test]
        fn script_records_never_panic_and_judge_their_shapes(
            form_id in prop_oneof![Just(0u32), Just(PILLAR), Just(0xff00_0000u32), any::<u32>()],
            flags in any::<u8>(), names in (name_strategy(), name_strategy()), scalar in value_strategy(),
            array in proptest::option::of((10u8..17, proptest::option::of(proptest::collection::vec(value_strategy(), 0..4)), proptest::option::of(name_strategy()))),
            now in any::<u64>(),
        ) {
            let (k, carried, _, handle) = scalar;
            let v = value_of(scalar);
            // a scalar draw of an array kind with no payload is a None array, the
            // same variable as the array branch's draw with no elements, so it is
            // valid too (4f525bad's oracle fix on the integrated rung)
            let mut var_ok = payload_ok(scalar) || ((10..=15).contains(&k) && carried == [false; 4] && handle.is_none());
            let mut as_var = ScriptVar { k: v.k, i: v.i, f: v.f, b: v.b, s: v.s, o: v.o, ..Default::default() };
            if let Some((ak, elements, elem)) = array {
                // an array instead: no value fields; `elem` (a name code) on an
                // object array alone and with no NUL; no elements at all on the
                // None array (kind 10); a typed one's elements each of kind ak - 10
                as_var = ScriptVar { k: ak, ..Default::default() };
                as_var.elem = elem.map(|code| name_of(code, "ObjectReference"));
                var_ok = (10..=15).contains(&ak) && elem.is_none_or(|code| ak == 11 && code != 2) && (ak != 10 || elements.is_none());
                if let Some(elements) = elements {
                    for e in &elements {
                        var_ok = var_ok && ak.checked_sub(10) == Some(e.0) && payload_ok(*e);
                    }
                    as_var.a = Some(elements.into_iter().map(value_of).collect::<Vec<_>>().try_into().unwrap_or_default());
                }
            }
            // the script's type (never empty) and its state (empty is the empty
            // state) as name codes, neither with a NUL
            let (type_code, state_code) = names;
            let named = ScriptObjectState {
                cls: name_of(type_code, "defaultPuzzlePillarScript"),
                state: name_of(state_code, "position02"),
                flags,
                ..script(vec![as_var])
            };
            let m = record(form_id, vec![named]);
            let names_ok = type_code == 0 && state_code != 2;
            let shape_ok = form_id != 0 && form_id < 0xff00_0000 && flags & 0x01 != 0 && flags & 0x08 != 0 && flags & 0x04 == 0 && names_ok;
            let want = if shape_ok && var_ok { Ok(()) } else { Err(Reject::Range) };
            prop_assert_eq!(validate_server(&m), want);
            let mut g = ClientGuard::default();
            let _ = validate(&m, &mut g, now);
        }
    }
}

// thuum D-C3 A (PENDING Eli; docs/research/fifty-fixes-2026-11-07.md 1(a)):
// a serve's graph variables
#[cfg(test)]
mod graph_var_tests {
    use super::*;
    use proptest::prelude::*;
    use wire_schema::cap;
    use wire_schema::skymp::{GraphVar, ScriptObjectState, ScriptStateApply, ScriptVar};

    // Bleak Falls Temple's web 0x3758c after its tear: its FXtriggerAmbFX
    // with the two variables the pex declares (broken, here 2, spent; myFX,
    // the placed explosion a seat's engine made, encoded None), its
    // properties' backing variables left out (the record's values are the
    // owner's and never judged), and the variable its OnTriggerEnter set
    // through SetAnimationVariableFloat("fToggleBlend", 1.0) at the web's
    // first sequence (lab/.cache/drafts/fifty/trigger-entry-static.md
    // section 6)
    const WEB: u32 = 0x3758c;

    fn web_script() -> ScriptObjectState {
        ScriptObjectState {
            cls: "FXtriggerAmbFX".try_into().unwrap_or_default(),
            state: "".try_into().unwrap_or_default(),
            flags: 0x0b,
            vars: vec![ScriptVar { k: 3, i: Some(2), ..Default::default() }, ScriptVar { k: 1, ..Default::default() }].try_into().unwrap_or_default(),
        }
    }

    fn var(name: &str, k: u8, seq: u32) -> GraphVar {
        GraphVar { name: name.try_into().unwrap_or_default(), k, seq, ..Default::default() }
    }

    fn torn() -> GraphVar {
        GraphVar { f: Some(1.0), ..var("fToggleBlend", 4, 1) }
    }

    fn serve(graph_vars: Option<Vec<GraphVar>>) -> Message {
        Message::ScriptStateApply(ScriptStateApply {
            form_id: WEB,
            fnv1a: 0x1f2e_3d4c,
            seq: 1,
            scripts: vec![web_script()].try_into().unwrap_or_default(),
            graph_vars: graph_vars.map(|v| v.try_into().unwrap_or_default()),
            ..Default::default()
        })
    }

    #[test]
    fn the_torn_webs_variable_is_served() {
        assert_eq!(validate_server(&serve(Some(vec![torn()]))), Ok(()));
        // no variable kept: absent, or (a shape nobody sends) present and empty
        assert_eq!(validate_server(&serve(None)), Ok(()));
        assert_eq!(validate_server(&serve(Some(vec![]))), Ok(()));
        // each kind with its own field
        let int = GraphVar { i: Some(-3), ..var("iState", 3, 2) };
        let flag = GraphVar { b: Some(true), ..var("bOpen", 5, 3) };
        assert_eq!(validate_server(&serve(Some(vec![torn(), int, flag]))), Ok(()));
        // sixteen names at once, each at its cap's length
        let full: Vec<GraphVar> = (0..cap::GRAPH_VARS)
            .map(|n| GraphVar { f: Some(0.5), ..var(&format!("{n:x}{}", "x".repeat(cap::ANIM_EVENT.saturating_sub(1))), 4, 7) })
            .collect();
        assert_eq!(full.first().map(|v| v.name.len()), Some(cap::ANIM_EVENT));
        assert_eq!(validate_server(&serve(Some(full))), Ok(()));
    }

    #[test]
    fn a_variable_carries_its_kinds_one_field_a_name_once_and_a_sequence() {
        let refused = |v: GraphVar| validate_server(&serve(Some(vec![v])));
        // a kind outside 3 to 5, whatever it carries
        for k in [0u8, 1, 2, 6, 10, u8::MAX] {
            assert_eq!(refused(GraphVar { f: Some(1.0), ..var("fToggleBlend", k, 1) }), Err(Reject::Range), "{k}");
        }
        // a payload that is not exactly its kind's
        assert_eq!(refused(var("fToggleBlend", 4, 1)), Err(Reject::Range), "a float with no value");
        assert_eq!(refused(GraphVar { i: Some(1), ..var("fToggleBlend", 4, 1) }), Err(Reject::Range), "a float carrying an int");
        assert_eq!(refused(GraphVar { b: Some(true), ..torn() }), Err(Reject::Range), "a float carrying a bool too");
        assert_eq!(refused(GraphVar { f: Some(1.0), ..var("iState", 3, 1) }), Err(Reject::Range), "an int carrying a float");
        assert_eq!(refused(GraphVar { i: Some(1), b: Some(false), ..var("bOpen", 5, 1) }), Err(Reject::Range), "a bool carrying an int too");
        // a sequence of 0
        assert_eq!(refused(GraphVar { seq: 0, ..torn() }), Err(Reject::Range));
        // a name empty or cut by a NUL
        assert_eq!(refused(GraphVar { name: "".try_into().unwrap_or_default(), ..torn() }), Err(Reject::Range));
        assert_eq!(refused(GraphVar { name: "fToggle\0Blend".try_into().unwrap_or_default(), ..torn() }), Err(Reject::Range));
        // a name twice, whatever the values and sequences
        let again = GraphVar { f: Some(0.0), seq: 2, ..torn() };
        assert_eq!(validate_server(&serve(Some(vec![torn(), again]))), Err(Reject::Range));
        let int = GraphVar { i: Some(1), ..var("fToggleBlend", 3, 3) };
        assert_eq!(validate_server(&serve(Some(vec![int, torn()]))), Err(Reject::Range));
        // names compare exactly: another case is another name
        let other = GraphVar { f: Some(0.0), seq: 2, ..var("ftoggleblend", 4, 2) };
        assert_eq!(validate_server(&serve(Some(vec![torn(), other]))), Ok(()));
    }

    #[test]
    fn a_variables_float_is_finite() {
        for f in [f32::NAN, f32::INFINITY, f32::NEG_INFINITY] {
            assert_eq!(validate_server(&serve(Some(vec![GraphVar { f: Some(f), ..torn() }]))), Err(Reject::NonFinite));
        }
        // the record's own rules come first: a bad record is refused with
        // whatever variables ride beside it
        let mut bad = serve(Some(vec![torn()]));
        if let Message::ScriptStateApply(a) = &mut bad {
            a.form_id = 0;
        }
        assert_eq!(validate_server(&bad), Err(Reject::Range));
        // the serve takes no client budget, as before
        let mut g = ClientGuard::default();
        for _ in 0..100 {
            assert_eq!(validate(&serve(Some(vec![torn()])), &mut g, 1_000), Ok(()));
        }
    }

    // a variable as the generator drew it: its kind, which of the int,
    // float and bool fields are present, its name as a code (0 its own, 1
    // empty, 2 with a NUL, 3 the first variable's) and its sequence
    type Draw = (u8, [bool; 3], u8, u32);

    fn draw() -> impl Strategy<Value = Draw> {
        (
            prop_oneof![3u8..6, any::<u8>()],
            any::<[bool; 3]>(),
            prop_oneof![4 => Just(0u8), 1 => Just(1u8), 1 => Just(2u8), 1 => Just(3u8)],
            prop_oneof![Just(0u32), 1u32..8, any::<u32>()],
        )
    }

    proptest! {
        // any kinds, payloads, names and sequences, up to the cap: a
        // verdict, never a panic, and the verdict the rules give
        #[test]
        fn graph_vars_never_panic_and_judge_their_shapes(draws in proptest::collection::vec(draw(), 0..=cap::GRAPH_VARS), now in any::<u64>()) {
            let mut vars = Vec::new();
            let mut names: Vec<String> = Vec::new();
            let mut ok = true;
            for (n, (k, [i, f, b], code, seq)) in draws.iter().copied().enumerate() {
                let name = match code {
                    0 => format!("v{n}"),
                    1 => String::new(),
                    2 => format!("v{n}\0"),
                    _ => names.first().cloned().unwrap_or_else(|| format!("v{n}")),
                };
                let fits = matches!((k, [i, f, b]), (3, [true, false, false]) | (4, [false, true, false]) | (5, [false, false, true]));
                ok = ok && fits && seq != 0 && !name.is_empty() && !name.contains('\0') && !names.contains(&name);
                names.push(name.clone());
                vars.push(GraphVar {
                    name: name.as_str().try_into().unwrap_or_default(),
                    k,
                    i: i.then_some(7),
                    f: f.then_some(0.25),
                    b: b.then_some(false),
                    seq,
                });
            }
            let m = serve(Some(vars));
            prop_assert_eq!(validate_server(&m), if ok { Ok(()) } else { Err(Reject::Range) });
            let mut g = ClientGuard::default();
            let _ = validate(&m, &mut g, now);
        }
    }
}

#[cfg(test)]
mod quest_word_and_script_variable_tests {
    // thuum ADR-068 rung 5 (SCHEMA_VERSION 41): QuestStage's word, acting
    // and served, and ScriptVariables 81
    use super::*;
    use proptest::prelude::*;
    use wire_schema::cap;

    // thuum ADR-068 rung 5: a run word, an acting player, the login replay
    fn quest_word(quest: u32, stage: u16, word: Option<u8>, acting: Option<u32>, served: Option<bool>) -> Message {
        Message::QuestStage(wire_schema::skymp::QuestStage { quest, stage, word, acting, served, ..Default::default() })
    }

    #[test]
    fn the_logins_list_names_the_player_class_once_each_and_nowhere_else() {
        // thuum ADR-068 rung 5 (the ownership rule): PlayerWerewolfQuest
        // 0x2ba16 and PlayerVampireQuest 0xeafd5 beside mode all's denied
        // MQ102 0x4e50d
        let list = |quest: u32, player: Option<Vec<u32>>| {
            Message::QuestStage(wire_schema::skymp::QuestStage {
                quest,
                mode: 1,
                shared: vec![0x4e50d, 0x2ba16, 0xeafd5].try_into().unwrap_or_default(),
                player: player.map(|p| p.try_into().unwrap_or_default()),
                ..Default::default()
            })
        };
        assert_eq!(validate_server(&list(0, Some(vec![0x2ba16, 0xeafd5]))), Ok(()));
        assert_eq!(validate_server(&list(0, None)), Ok(()));
        for bad in [vec![], vec![0x2ba16, 0x2ba16], vec![0], vec![0xff00_0000]] {
            assert_eq!(validate_server(&list(0, Some(bad.clone()))), Err(Reject::Range), "{bad:?}");
        }
        // on the login's list alone, never a stage or a word, never a client's
        assert_eq!(validate_server(&list(0x74a83, Some(vec![0x2ba16]))), Err(Reject::Range));
        let mut g = ClientGuard::default();
        assert_eq!(validate(&list(0, Some(vec![0x2ba16])), &mut g, 0), Err(Reject::Range));
    }

    #[test]
    fn a_run_word_names_its_engine_stages_once_each_and_only_a_word_does() {
        // thuum ADR-068 rung 5 (D-Q3 A; Skyrim.esm, lab/esm.py on sky-srv,
        // 2026-11-10): dunEldergleamT03 0x15cc2's start runs stage 0 and its
        // start-up stage 10; ClearSkiesQuest 0x1e3b1's stop runs its
        // shut-down stage 100
        let word = |quest: u32, stage: u16, word: Option<u8>, stages: Option<Vec<u16>>| {
            Message::QuestStage(wire_schema::skymp::QuestStage { quest, stage, word, served: Some(true), word_stages: stages.map(|s| s.try_into().unwrap_or_default()), ..Default::default() })
        };
        assert_eq!(validate_server(&word(0x15cc2, 0, Some(1), Some(vec![0, 10]))), Ok(()));
        assert_eq!(validate_server(&word(0x1e3b1, 0, Some(2), Some(vec![100]))), Ok(()));
        assert_eq!(validate_server(&word(0x1e3b1, 0, Some(2), None)), Ok(()));
        // never empty, each stage once, never on a stage or the login's list
        for bad in [vec![], vec![10, 10]] {
            assert_eq!(validate_server(&word(0x15cc2, 0, Some(1), Some(bad.clone()))), Err(Reject::Range), "{bad:?}");
        }
        assert_eq!(validate_server(&word(0x15cc2, 10, None, Some(vec![10]))), Err(Reject::Range));
        assert_eq!(validate_server(&word(0, 0, None, Some(vec![0]))), Err(Reject::Range));
    }

    #[test]
    fn a_seats_report_names_its_cause_and_root_in_their_shapes() {
        // thuum ADR-068 rung 5, D-Q4 C (SCHEMA_VERSION 43): c1's stop of
        // dunEmbershardQST 0xb6fef from its stage 40 fragment (a world cause,
        // the probe's world pair), USSEP's housekeeping stop of
        // USKPDrownedSorrowsFix (the seat's own, rooted at OnInit), and an
        // engine start's stage 0 (the seat's own, no script call: no root)
        let report = |quest: u32, stage: u16, word: Option<u8>, cause: Option<u8>, root: Option<&str>| {
            Message::QuestStage(wire_schema::skymp::QuestStage { quest, stage, word, cause, root: root.map(|r| r.try_into().unwrap_or_default()), ..Default::default() })
        };
        let mut g = ClientGuard::default();
        assert_eq!(validate(&report(0xb6fef, 0, Some(2), Some(1), Some("QF_dunEmbershardQST_000B6FEF.Fragment_16")), &mut g, 0), Ok(()));
        assert_eq!(validate(&report(0x701e8bd, 0, Some(2), Some(2), Some("USKP_VersionTrackingScript.OnInit")), &mut g, 0), Ok(()));
        assert_eq!(validate(&report(0x74a83, 0, None, Some(2), None), &mut g, 0), Ok(()));
        // a stage with its cause, and an older client's report with none
        assert_eq!(validate(&report(0xb6fef, 40, None, Some(1), Some("defaultSetStageTrigScript.OnTriggerEnter")), &mut g, 0), Ok(()));
        assert_eq!(validate(&report(0xb6fef, 40, None, None, None), &mut g, 0), Ok(()));
        // a cause other than 1 or 2, a cause or a root on the login's list, a
        // root without a cause, an empty root, a root with a character no
        // Papyrus name takes
        for bad in [0u8, 3, u8::MAX] {
            assert_eq!(validate(&report(0xb6fef, 0, Some(1), Some(bad), None), &mut g, 0), Err(Reject::Range), "{bad}");
        }
        assert_eq!(validate_server(&report(0, 0, None, Some(1), None)), Err(Reject::Range));
        for root in ["QF_dunEmbershardQST_000B6FEF.Fragment_16", "", "Script.On Init", "Script.OnInit;", "Skrípt.OnInit"] {
            assert_eq!(validate(&report(0xb6fef, 0, Some(1), None, Some(root)), &mut g, 0), Err(Reject::Range), "{root:?} without a cause");
        }
        for root in ["", "Script.On Init", "Script.OnInit;", "Skrípt.OnInit"] {
            assert_eq!(validate(&report(0xb6fef, 0, Some(1), Some(1), Some(root)), &mut g, 0), Err(Reject::Range), "{root:?}");
        }
    }

    #[test]
    fn quest_stages_carry_a_run_word_an_acting_player_and_the_replay_flag() {
        // a relayed start of 0x10d3bb with c1 0xff000000 acting, a stop, a
        // served stage of A Lovely Letter 0x74a83
        let mut g = ClientGuard::default();
        assert_eq!(validate_server(&quest_word(0x10d3bb, 0, Some(1), Some(0xff00_0000), None)), Ok(()));
        assert_eq!(validate_server(&quest_word(0x10d3bb, 0, Some(2), None, None)), Ok(()));
        assert_eq!(validate_server(&quest_word(0x74a83, 10, None, None, Some(true))), Ok(()));
        assert_eq!(validate_server(&quest_word(0x74a83, 10, None, Some(0xffff_ffff), Some(false))), Ok(()));
        // a seat's own words and its scope's acting player cross
        assert_eq!(validate(&quest_word(0x10d3bb, 0, Some(1), None, None), &mut g, 0), Ok(()));
        assert_eq!(validate(&quest_word(0x74a83, 10, None, Some(0xff00_0001), None), &mut g, 0), Ok(()));
        assert_eq!(validate(&quest_word(0x74a83, 10, None, None, Some(false)), &mut g, 0), Ok(()));
        // a word other than a start or a stop, a word with a stage, a word or
        // an acting player on the login's list, an acting player the server
        // does not name
        for bad in [0u8, 3, u8::MAX] {
            assert_eq!(validate_server(&quest_word(0x10d3bb, 0, Some(bad), None, None)), Err(Reject::Range), "{bad}");
        }
        assert_eq!(validate_server(&quest_word(0x10d3bb, 10, Some(2), None, None)), Err(Reject::Range));
        assert_eq!(validate_server(&quest_word(0, 0, Some(1), None, None)), Err(Reject::Range));
        assert_eq!(validate_server(&quest_word(0, 0, None, Some(0xff00_0000), Some(true))), Err(Reject::Range));
        for low in [0u32, 0x14, 0xfeff_ffff] {
            assert_eq!(validate_server(&quest_word(0x74a83, 10, None, Some(low), None)), Err(Reject::Range), "{low:x}");
        }
        // a client never sends the replay, and is refused before its budget
        let mut g = ClientGuard::default();
        assert_eq!(validate(&quest_word(0x74a83, 10, None, None, Some(true)), &mut g, 0), Err(Reject::Range));
        // nor a word's stages (thuum ADR-068 rung 5, D-Q3 A: the server reads
        // them from the record)
        let staged = |stages: &[u16]| Message::QuestStage(wire_schema::skymp::QuestStage { quest: 0x15cc2, word: Some(1), word_stages: Some(stages.to_vec().try_into().unwrap_or_default()), ..Default::default() });
        assert_eq!(validate(&staged(&[0, 10]), &mut g, 0), Err(Reject::Range));
        for _ in 0..32 {
            assert_eq!(validate(&quest_word(0x10d3bb, 0, Some(1), None, None), &mut g, 0), Ok(()));
        }
        assert_eq!(validate(&quest_word(0x10d3bb, 0, Some(2), None, None), &mut g, 0), Err(Reject::Rate), "words share the stages' budget");
    }

    // thuum ADR-068 rung 5 (D-V1 B): MS13 0x39645's ::camillaescort_var and
    // PlayerVampireQuest 0xeafd5's ::vampirestatus_var, its player's own
    fn script_var(form: u32, var: &str, own: bool, k: u8, carried: [bool; 3], seq: u32) -> wire_schema::skymp::ScriptVarEntry {
        let [i, f, b] = carried;
        wire_schema::skymp::ScriptVarEntry {
            form,
            script: "MS13QuestScript".try_into().unwrap_or_default(),
            var: var.try_into().unwrap_or_default(),
            own,
            k,
            i: i.then_some(1),
            f: f.then_some(0.5),
            b: b.then_some(true),
            seq,
        }
    }

    fn script_vars(watch: &[(u32, &str)], entries: &[wire_schema::skymp::ScriptVarEntry], served: bool) -> Message {
        use wire_schema::skymp::{ScriptVarKey, ScriptVariables};
        let keys: Vec<ScriptVarKey> = watch.iter().map(|&(form, var)| ScriptVarKey { form, script: "MS13QuestScript".try_into().unwrap_or_default(), var: var.try_into().unwrap_or_default(), own: false }).collect();
        Message::ScriptVariables(ScriptVariables { watch: keys.try_into().unwrap_or_default(), entries: entries.to_vec().try_into().unwrap_or_default(), served, ..Default::default() })
    }

    #[test]
    fn script_variables_name_keys_once_with_their_kinds_and_a_client_reports_alone() {
        let int = [true, false, false];
        let escort = script_var(0x39645, "::camillaescort_var", false, 3, int, 0);
        let status = script_var(0xeafd5, "::vampirestatus_var", true, 3, int, 0);
        // a seat's report, its own key beside a world key
        let mut g = ClientGuard::default();
        assert_eq!(validate(&script_vars(&[], &[escort.clone(), status.clone()], false), &mut g, 0), Ok(()));
        // the login: the watch, the served values with their sequences
        let served = [script_var(0x39645, "::camillaescort_var", false, 3, int, 2), script_var(0xeafd5, "::vampirestatus_var", true, 4, [false, true, false], 1)];
        let login = script_vars(&[(0x39645, "::camillaescort_var"), (0xeafd5, "::vampirestatus_var")], &served, true);
        assert_eq!(validate_server(&login), Ok(()));
        // a client sends no watch, no served record and no sequence, refused
        // before its budget
        assert_eq!(validate(&login, &mut g, 0), Err(Reject::Range));
        assert_eq!(validate(&script_vars(&[(0x39645, "::camillaescort_var")], &[], false), &mut g, 0), Err(Reject::Range));
        assert_eq!(validate(&script_vars(&[], &[script_var(0x39645, "::camillaescort_var", false, 3, int, 1)], false), &mut g, 0), Err(Reject::Range));
        // the shape both ways: a kind outside 3 to 5 or a payload not its
        // kind's, a key twice (names without case), a form of 0 or the
        // server's, an empty name or one with a NUL, a served seq of 0
        let refused = [
            script_var(0x39645, "::camillaescort_var", false, 2, int, 0),
            script_var(0x39645, "::camillaescort_var", false, 3, [true, true, false], 0),
            script_var(0x39645, "::camillaescort_var", false, 5, int, 0),
            script_var(0, "::camillaescort_var", false, 3, int, 0),
            script_var(0xff00_0000, "::camillaescort_var", false, 3, int, 0),
            script_var(0x39645, "", false, 3, int, 0),
            script_var(0x39645, "::camillaescort_var\0", false, 3, int, 0),
        ];
        for e in refused {
            assert_eq!(validate_server(&script_vars(&[], std::slice::from_ref(&e), false)), Err(Reject::Range), "{e:?}");
        }
        assert_eq!(validate_server(&script_vars(&[], &[escort.clone(), script_var(0x39645, "::CamillaEscort_var", true, 3, int, 0)], false)), Err(Reject::Range));
        assert_eq!(validate_server(&script_vars(&[(0x39645, "::camillaescort_var"), (0x39645, "::CAMILLAESCORT_VAR")], &[], true)), Err(Reject::Range));
        assert_eq!(validate_server(&script_vars(&[(0, "::camillaescort_var")], &[], true)), Err(Reject::Range));
        assert_eq!(validate_server(&script_vars(&[], &[script_var(0x39645, "::camillaescort_var", false, 3, int, 0)], true)), Err(Reject::Range));
        let nan = wire_schema::skymp::ScriptVarEntry { k: 4, i: None, f: Some(f32::NAN), ..escort.clone() };
        assert_eq!(validate_server(&script_vars(&[], &[nan], false)), Err(Reject::NonFinite));
        // four at once, then one a second
        let mut g = ClientGuard::default();
        for _ in 0..4 {
            assert_eq!(validate(&script_vars(&[], std::slice::from_ref(&escort), false), &mut g, 1_000), Ok(()));
        }
        assert_eq!(validate(&script_vars(&[], std::slice::from_ref(&escort), false), &mut g, 1_000), Err(Reject::Rate));
        assert_eq!(validate(&script_vars(&[], &[escort], false), &mut g, 2_000), Ok(()));
    }

    // thuum ADR-068 rung 5: a quest stage's word, acting, served and the
    // word's engine stages as the generator drew them
    proptest! {
        // any word, stage, quest, acting player, replay flag and word stages:
        // a verdict, never a panic, and the verdict the rules give both ways
        #[test]
        fn quest_words_never_panic_and_judge_their_shapes(
            quest in prop_oneof![Just(0u32), Just(0x10d3bbu32), any::<u32>()],
            stage in prop_oneof![Just(0u16), any::<u16>()],
            word in proptest::option::of(prop_oneof![1u8..3, any::<u8>()]),
            acting in proptest::option::of(prop_oneof![Just(0xff00_0000u32), Just(0x14u32), any::<u32>()]),
            served in proptest::option::of(any::<bool>()),
            stages in proptest::option::of(proptest::collection::vec(prop_oneof![Just(0u16), Just(10u16), any::<u16>()], 0..4)),
            // SCHEMA_VERSION 43 (D-Q4 C): a cause in range or not, and a root
            // a script's name, empty, or with a character no name takes
            cause in proptest::option::of(prop_oneof![1u8..3, any::<u8>()]),
            root in proptest::option::of(prop_oneof![Just("QF_dunEmbershardQST_000B6FEF.Fragment_16".to_owned()), Just(String::new()), "[A-Za-z0-9_. :-]{1,24}"]),
            now in any::<u64>(),
        ) {
            let mut m = quest_word(quest, stage, word, acting, served);
            if let Message::QuestStage(q) = &mut m {
                q.word_stages = stages.clone().map(|s| s.try_into().unwrap_or_default());
                q.cause = cause;
                q.root = root.as_deref().map(|r| r.try_into().unwrap_or_default());
            }
            let word_ok = word.is_none_or(|w| (w == 1 || w == 2) && stage == 0 && quest != 0);
            let acting_ok = acting.is_none_or(|a| a >= 0xff00_0000 && quest != 0);
            // quest 0 in mode 0 without a list is the login's message shape alone
            let list_ok = quest != 0;
            let stages_ok = stages.as_ref().is_none_or(|s| word.is_some() && !s.is_empty() && s.iter().enumerate().all(|(i, x)| !s.iter().take(i).any(|y| y == x)));
            let cause_ok = cause.is_none_or(|c| (c == 1 || c == 2) && quest != 0);
            let root_ok = root.as_ref().is_none_or(|r| cause.is_some() && !r.is_empty() && r.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_' || b == b'.'));
            let shape = if word_ok && acting_ok && list_ok && stages_ok && cause_ok && root_ok { Ok(()) } else { Err(Reject::Range) };
            prop_assert_eq!(validate_server(&m), shape);
            let mut g = ClientGuard::default();
            let client = if shape.is_ok() && served != Some(true) && stages.is_none() { Ok(()) } else { Err(Reject::Range) };
            prop_assert_eq!(validate(&m, &mut g, now), client);
        }
    }

    // a condition-read entry as the generator drew it: its form as a code (0
    // a world key's, 1 0, 2 a server form), its name as a code (0 its own,
    // 1 empty, 2 with a NUL, 3 the first entry's), its kind, which of the
    // int, float and bool fields are present, and its sequence
    type VarDraw = (u8, u8, u8, [bool; 3], u32);

    fn var_draw() -> impl Strategy<Value = VarDraw> {
        (
            prop_oneof![4 => Just(0u8), 1 => Just(1u8), 1 => Just(2u8)],
            prop_oneof![4 => Just(0u8), 1 => Just(1u8), 1 => Just(2u8), 1 => Just(3u8)],
            prop_oneof![3u8..6, any::<u8>()],
            any::<[bool; 3]>(),
            prop_oneof![Just(0u32), any::<u32>()],
        )
    }

    proptest! {
        // any entries up to the cap, served or not: a verdict, never a
        // panic, and the verdict the rules give both ways
        #[test]
        fn script_variables_never_panic_and_judge_their_shapes(draws in proptest::collection::vec(var_draw(), 0..=cap::SCRIPT_VARIABLES), served in any::<bool>(), now in any::<u64>()) {
            let mut entries = Vec::new();
            let mut names: Vec<String> = Vec::new();
            let mut ok = true;
            for (n, (form, name, k, [i, f, b], seq)) in draws.iter().copied().enumerate() {
                let form = match form {
                    0 => 0x39645,
                    1 => 0,
                    _ => 0xff00_0000,
                };
                let name = match name {
                    0 => format!("::v{n}_var"),
                    1 => String::new(),
                    2 => format!("::v{n}\0"),
                    _ => names.first().cloned().unwrap_or_else(|| format!("::v{n}_var")),
                };
                let fits = matches!((k, [i, f, b]), (3, [true, false, false]) | (4, [false, true, false]) | (5, [false, false, true]));
                ok = ok && fits && form == 0x39645 && !name.is_empty() && !name.contains('\0') && !names.contains(&name) && !(served && seq == 0);
                names.push(name.clone());
                entries.push(wire_schema::skymp::ScriptVarEntry {
                    form,
                    script: "MS13QuestScript".try_into().unwrap_or_default(),
                    var: name.as_str().try_into().unwrap_or_default(),
                    own: n % 2 == 1,
                    k,
                    i: i.then_some(7),
                    f: f.then_some(0.25),
                    b: b.then_some(false),
                    seq,
                });
            }
            let m = script_vars(&[], &entries, served);
            prop_assert_eq!(validate_server(&m), if ok { Ok(()) } else { Err(Reject::Range) });
            let mut g = ClientGuard::default();
            let client = if ok && !served && entries.iter().all(|e| e.seq == 0) { Ok(()) } else { Err(Reject::Range) };
            prop_assert_eq!(validate(&m, &mut g, now), client);
        }
    }
}

#[cfg(test)]
mod race_overlay_tests {
    use super::*;
    use proptest::prelude::*;
    use wire_schema::cap;
    use wire_schema::skymp::{CreateActor, CreateActorProps, RaceOverlay, UpdateAppearance};

    // thuum ADR-068 rung 5 (D-T1 A): rotfern's beast form, WerewolfBeastRace
    // 0xcdd84 over its recorded race (NordRace 0x13746), the howl 0x92c48
    // and WolfSkinFXArmor's id illustrative
    fn beast() -> RaceOverlay {
        RaceOverlay {
            open: true,
            race: 0xcdd84,
            base: 0x13746,
            spells: vec![0x92c48].try_into().unwrap_or_default(),
            shouts: vec![0x6f6da].try_into().unwrap_or_default(),
            items: vec![0x10d4f9].try_into().unwrap_or_default(),
        }
    }

    fn appearance(overlay: Option<RaceOverlay>) -> Message {
        Message::UpdateAppearance(UpdateAppearance { idx: 7, overlay, ..Default::default() })
    }

    fn figure(overlay: Option<RaceOverlay>, is_me: bool) -> Message {
        Message::CreateActor(CreateActor { idx: 7, is_me, props: CreateActorProps { overlay, ..Default::default() }, ..Default::default() })
    }

    #[test]
    fn a_figures_overlay_wears_a_race_of_the_load_order_and_a_seat_sends_none() {
        assert_eq!(validate_server(&appearance(Some(beast()))), Ok(()));
        assert_eq!(validate_server(&appearance(None)), Ok(()));
        assert_eq!(validate_server(&figure(Some(beast()), false)), Ok(()));
        // a player's own CreateActor never carries one
        assert_eq!(validate_server(&figure(Some(beast()), true)), Err(Reject::Range));
        // a closed overlay names nothing
        assert_eq!(validate_server(&appearance(Some(RaceOverlay::default()))), Ok(()));
        assert_eq!(validate_server(&appearance(Some(RaceOverlay { open: false, ..beast() }))), Err(Reject::Range));
        // an open one wears a load-order race over another, each form once
        let refused = [
            RaceOverlay { race: 0, ..beast() },
            RaceOverlay { race: 0xff00_0001, ..beast() },
            RaceOverlay { base: 0, ..beast() },
            RaceOverlay { base: 0xcdd84, ..beast() },
            RaceOverlay { spells: vec![0x92c48, 0x92c48].try_into().unwrap_or_default(), ..beast() },
            RaceOverlay { shouts: vec![0].try_into().unwrap_or_default(), ..beast() },
            RaceOverlay { items: vec![0xff00_0002].try_into().unwrap_or_default(), ..beast() },
        ];
        for o in refused {
            assert_eq!(validate_server(&appearance(Some(o.clone()))), Err(Reject::Range), "{o:?}");
        }
        // a seat's own look carries no overlay
        let mut g = ClientGuard::default();
        assert_eq!(validate(&appearance(None), &mut g, 0), Ok(()));
        assert_eq!(validate(&appearance(Some(beast())), &mut g, 0), Err(Reject::Range));
        assert_eq!(validate(&appearance(Some(RaceOverlay::default())), &mut g, 0), Err(Reject::Range));
    }

    proptest! {
        // any overlay up to the caps, on either carrier: a verdict, never a
        // panic, and the verdict the rule gives
        #[test]
        fn overlays_never_panic_and_judge_their_shapes(
            open in any::<bool>(),
            race in prop_oneof![Just(0u32), Just(0xcdd84u32), Just(0xff00_0000u32), any::<u32>()],
            base in prop_oneof![Just(0u32), Just(0x13746u32), any::<u32>()],
            spells in proptest::collection::vec(prop_oneof![Just(0u32), 1u32..0x200, any::<u32>()], 0..=cap::OVERLAY_FORMS),
            shouts in proptest::collection::vec(1u32..0x200, 0..=4),
            items in proptest::collection::vec(any::<u32>(), 0..=4),
            is_me in any::<bool>(),
        ) {
            let o = RaceOverlay {
                open,
                race,
                base,
                spells: spells.clone().try_into().unwrap_or_default(),
                shouts: shouts.clone().try_into().unwrap_or_default(),
                items: items.clone().try_into().unwrap_or_default(),
            };
            let form = |id: u32| id != 0 && id < 0xff00_0000;
            let distinct = |ids: &[u32]| ids.iter().enumerate().all(|(i, id)| form(*id) && !ids.iter().take(i).any(|p| p == id));
            let shape_ok = if open {
                form(race) && form(base) && race != base && distinct(&spells) && distinct(&shouts) && distinct(&items)
            } else {
                race == 0 && base == 0 && spells.is_empty() && shouts.is_empty() && items.is_empty()
            };
            let want = if shape_ok { Ok(()) } else { Err(Reject::Range) };
            prop_assert_eq!(validate_server(&appearance(Some(o.clone()))), want);
            prop_assert_eq!(validate_server(&figure(Some(o), is_me)), if is_me { Err(Reject::Range) } else { want });
        }
    }
}
