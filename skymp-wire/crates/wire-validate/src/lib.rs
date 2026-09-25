//! Structural validation. This crate knows the shape of a legal message and
//! the rates a client may send them at. It does not know who owns what; that
//! is semantic validation and lives in the server behind the bridge.
//!
//! Every rejection has a reason code. The lab greps them. The validator is
//! pure: time comes in as `now_ms` from the caller's clock, and nothing here
//! can panic on any input (checked arithmetic throughout, fuzzed by
//! `fuzz/validate`).

use wire_schema::{Message, Transform};

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
}

/// World bounds in engine units. Tamriel's worldspace fits comfortably; a
/// value outside this is a bug or a lie either way.
pub const WORLD_ABS_MAX: f32 = 1.0e6;

/// Per-client state the validator needs: sequence tracking and token buckets.
/// Owned by the transport layer, one per connection.
#[derive(Debug, Default, Clone)]
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

/// Validate one inbound message against the client's guard state.
/// `Ok(())` means "well-formed and within rate"; ownership is checked later.
pub fn validate(msg: &Message, guard: &mut ClientGuard, now_ms: u64) -> Result<(), Reject> {
    match msg {
        Message::Hello(h) => {
            if h.schema_version != wire_schema::SCHEMA_VERSION {
                return Err(Reject::SchemaVersion);
            }
            Ok(())
        }
        Message::Movement(m) => {
            check_transform(&m.transform)?;
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
        // Server-to-client messages arriving from a client are shape-valid
        // but semantically impossible; the transport rejects them by direction.
        _ => Ok(()),
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
            mod_hashes: heapless::Vec::new(),
            name: heapless::String::new(),
        })
    }

    fn hosted(health: f32, counts: &[i32]) -> Message {
        let mut inv = heapless::Vec::new();
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
        fn bucket_never_exceeds_capacity(times in proptest::collection::vec(any::<u64>(), 1..100)) {
            let mut b = TokenBucket::default();
            for t in times {
                let _ = b.take(t);
                prop_assert!(b.tokens <= b.capacity);
            }
        }
    }
}
