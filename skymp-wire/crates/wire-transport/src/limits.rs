//! Resource bounds enforced before decode. Every number here is a cap the
//! lab can hit on purpose (see lab/scenarios/*-limits.yaml).
//!
//! Not here: a connect-rate cap per source address. The netcode transport
//! owns the socket, so nothing above it sees a handshake packet before it is
//! processed; that cap lives at the host firewall until the transport grows a
//! hook for it (docs/WIRE.md records the gap).

/// netcode's own ceiling on connections (renetcode's NETCODE_MAX_CLIENTS,
/// private there; it panics above it, so the transport clamps to it).
pub const NETCODE_MAX_CLIENTS: usize = 1024;

/// Transport limits. Defaults suit a lab or a 100-player server; tune per
/// gamemode. Client-to-server numbers are tight (that is the attack
/// surface); server-to-client ones are generous (the server is ours).
#[derive(Debug, Clone)]
pub struct Limits {
    /// Connections the transport accepts at once; at most
    /// [`NETCODE_MAX_CLIENTS`].
    pub max_clients: usize,
    /// Largest reassembled message a server accepts from a client. The
    /// largest legal one is UpdateEquipment or CustomEvent at 256 KiB.
    pub max_from_client: usize,
    /// Largest reassembled message a client accepts from the server:
    /// `Message::MAX_ENCODED_LEN`.
    pub max_from_server: usize,
    /// Bytes a client-to-server channel may hold unacknowledged per
    /// connection: the queue depth on both ends. A reliable channel that
    /// fills disconnects the client; an unreliable one drops new messages
    /// (renet semantics).
    pub client_channel_memory_bytes: usize,
    /// The same for server-to-client channels; room for a 4 MiB message.
    pub server_channel_memory_bytes: usize,
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
            max_clients: 128,
            max_from_client: 256 * 1024,
            max_from_server: wire_schema::Message::MAX_ENCODED_LEN,
            client_channel_memory_bytes: 2 * 1024 * 1024,
            server_channel_memory_bytes: 16 * 1024 * 1024,
            resend_ms: 300,
            bytes_per_tick: 256 * 1024,
            bytes_per_s_per_client: 256 * 1024,
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
