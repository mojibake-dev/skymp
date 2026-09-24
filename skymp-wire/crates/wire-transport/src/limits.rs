//! Resource bounds enforced before decode. Every number here is a cap the
//! lab can hit on purpose (see lab/scenarios/*-limits.yaml).

/// Transport limits. Defaults are for a 64-player server; tune per gamemode.
#[derive(Debug, Clone)]
pub struct Limits {
    pub max_clients: usize,
    /// Larger than `Message::MAX_ENCODED_LEN` is never legitimate.
    pub max_packet: usize,
    pub max_queued_per_client: usize,
    pub connects_per_source_per_min: u32,
    pub bytes_per_s_per_client: u32,
}

impl Default for Limits {
    fn default() -> Self {
        Self {
            max_clients: 64,
            max_packet: wire_schema::Message::MAX_ENCODED_LEN,
            max_queued_per_client: 256,
            connects_per_source_per_min: 10,
            bytes_per_s_per_client: 64 * 1024,
        }
    }
}
