//! Resource bounds enforced before decode. Every number here is a cap the
//! lab can hit on purpose (see lab/scenarios/*-limits.yaml).
//!
//! Not here: a connect-rate cap per source address. The netcode transport
//! owns the socket, so nothing above it sees a handshake packet before it is
//! processed; that cap lives at the host firewall until the transport grows a
//! hook for it (docs/WIRE.md records the gap).

/// Transport limits. Defaults are for a 64-player server; tune per gamemode.
#[derive(Debug, Clone)]
pub struct Limits {
    /// Connections the transport accepts at once.
    pub max_clients: usize,
    /// Largest reassembled message handed to the decoder; larger than
    /// `Message::MAX_ENCODED_LEN` is never legitimate.
    pub max_packet: usize,
    /// Bytes a channel may hold unacknowledged per connection: the queue
    /// depth. A reliable channel that fills disconnects the client; an
    /// unreliable one drops new messages (renet semantics).
    pub channel_memory_bytes: usize,
    /// Resend interval for reliable channels, milliseconds.
    pub resend_ms: u64,
    /// Bytes the transport may emit per tick per connection.
    pub bytes_per_tick: u64,
    /// Inbound message bytes per second per client; past it, that client's
    /// messages are dropped with a Limit rejection until the second turns.
    pub bytes_per_s_per_client: u64,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            max_clients: 64,
            max_packet: wire_schema::Message::MAX_ENCODED_LEN,
            channel_memory_bytes: 5 * 1024 * 1024,
            resend_ms: 300,
            bytes_per_tick: 60_000,
            bytes_per_s_per_client: 64 * 1024,
        }
    }
}

/// Fixed one-second window of inbound bytes for one client.
#[derive(Debug, Clone, Copy, Default)]
pub struct ByteBudget {
    window_start_ms: u64,
    used: u64,
}

impl ByteBudget {
    /// Charge `len` bytes at `now_ms`; false when the window is exhausted.
    pub fn charge(&mut self, now_ms: u64, len: usize, per_s: u64) -> bool {
        if now_ms.saturating_sub(self.window_start_ms) >= 1000 {
            self.window_start_ms = now_ms;
            self.used = 0;
        }
        let len = u64::try_from(len).unwrap_or(u64::MAX);
        let next = self.used.saturating_add(len);
        if next > per_s {
            return false;
        }
        self.used = next;
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn byte_budget_windows() {
        let mut b = ByteBudget::default();
        assert!(b.charge(0, 60, 100));
        assert!(!b.charge(500, 50, 100), "over the window");
        assert!(b.charge(500, 40, 100), "exactly full is allowed");
        assert!(b.charge(1000, 100, 100), "new second");
    }
}
