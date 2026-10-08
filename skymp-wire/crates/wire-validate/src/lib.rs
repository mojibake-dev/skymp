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
use wire_schema::{skymp, Message, Transform};

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

/// A RaceMenu preset, either way: not empty (docs/verbs/racemenu-sync.md).
/// From a client it is also its own ([`validate`]); the JSON's shape is the
/// server's rule (wire-rules `racemenu`), past the capacity the type holds.
fn check_preset(m: &skymp::RaceMenuPreset) -> Result<(), Reject> {
    if m.preset.is_empty() {
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
        Message::ConsoleCommand(_) => take(&mut guard.console_budget, now_ms),
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
        Message::HostedActor(s) => {
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
        // thuum docs/verbs/console-commands.md: the server's own line, its
        // length bounded at decode (cap::CONSOLE_OUTPUT); the transport
        // refuses it from a client by direction
        Message::ConsoleOutput(_) => Ok(()),
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
        cap, FormId, Hello, HitIntent, HostedActorState, ItemDelta, MovementSample, SCHEMA_VERSION,
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
        Message::HostedActor(HostedActorState {
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
        fn hosted_never_panics(health in any::<f32>(), counts in proptest::collection::vec(any::<i32>(), 0..=cap::INVENTORY_DELTA)) {
            let mut g = ClientGuard::default();
            let _ = validate(&hosted(health, &counts), &mut g, 0);
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
