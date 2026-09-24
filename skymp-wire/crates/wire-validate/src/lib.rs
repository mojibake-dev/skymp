//! Structural validation. This crate knows the shape of a legal message and
//! the rates a client may send them at. It does not know who owns what; that
//! is semantic validation and lives in the server behind the bridge.
//!
//! Every rejection has a reason code. The lab greps them.

use wire_schema::{Message, Transform};

/// Rejection reasons. Stable names; append only.
#[derive(Debug, thiserror::Error, PartialEq, Eq, Clone, Copy)]
pub enum Reject {
    #[error("E_VAL_SCHEMA_VERSION")]
    SchemaVersion,
    #[error("E_VAL_NONFINITE")]
    NonFinite,
    #[error("E_VAL_OUT_OF_WORLD")]
    OutOfWorld,
    #[error("E_VAL_RATE")]
    Rate,
    #[error("E_VAL_ZERO_DELTA")]
    ZeroDelta,
    #[error("E_VAL_SEQ_REPLAY")]
    SeqReplay,
}

/// World bounds in engine units. Tamriel's worldspace fits comfortably; a
/// value outside this is a bug or a lie either way.
pub const WORLD_ABS_MAX: f32 = 1.0e6;

/// Per-client state the validator needs: last sequence numbers and token
/// buckets. Owned by the transport layer, one per connection.
#[derive(Debug, Default, Clone)]
pub struct ClientGuard {
    pub last_movement_seq: u32,
    pub movement_budget: TokenBucket,
    pub hit_budget: TokenBucket,
}

/// Minimal token bucket with checked arithmetic. Refill is driven by the
/// caller's clock so the validator stays pure.
#[derive(Debug, Clone)]
pub struct TokenBucket {
    pub tokens: u32,
    pub capacity: u32,
    pub refill_per_s: u32,
    pub last_refill_ms: u64,
}

impl Default for TokenBucket {
    fn default() -> Self {
        Self { tokens: 60, capacity: 60, refill_per_s: 60, last_refill_ms: 0 }
    }
}

impl TokenBucket {
    /// Refill for elapsed time, then try to take one token.
    pub fn take(&mut self, now_ms: u64) -> bool {
        let elapsed_ms = now_ms.saturating_sub(self.last_refill_ms);
        let refill_u64 = elapsed_ms.saturating_mul(u64::from(self.refill_per_s)) / 1000;
        let refill = u32::try_from(refill_u64).unwrap_or(u32::MAX);
        if refill > 0 {
            self.tokens = self.tokens.saturating_add(refill).min(self.capacity);
            self.last_refill_ms = now_ms;
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
            if guard.last_movement_seq != 0 && m.seq <= guard.last_movement_seq {
                return Err(Reject::SeqReplay);
            }
            if !guard.movement_budget.take(now_ms) {
                return Err(Reject::Rate);
            }
            guard.last_movement_seq = m.seq;
            Ok(())
        }
        Message::Hit(_) => {
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
        // but semantically impossible; the server rejects them by direction.
        _ => Ok(()),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;
    use wire_schema::{FormId, MovementSample};

    proptest! {
        #[test]
        fn movement_never_panics(x in any::<f32>(), y in any::<f32>(), z in any::<f32>(), seq in any::<u32>()) {
            let m = Message::Movement(MovementSample {
                seq, actor: FormId(0x14),
                transform: Transform { x, y, z, yaw: 0.0, pitch: 0.0 },
                run: false, sneak: false,
            });
            let mut g = ClientGuard::default();
            let _ = validate(&m, &mut g, 1_000);
        }
    }
}
