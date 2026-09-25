//! Connect tokens. netcode assumes an issuer; for community servers the
//! server is the issuer (ADR-011). `Unsecure` exists for the lab and for
//! nothing else; the server logs a warning on every start with it.

/// Server-side authentication mode.
pub enum Auth {
    /// Accept unsigned tokens. Lab only.
    Unsecure,
    /// Verify tokens signed with this key. The key never leaves the server
    /// process; the issuer endpoint runs inside it.
    Secure {
        /// The netcode private key.
        private_key: [u8; 32],
    },
}

/// An opaque connect token handed to a client by the issuer.
pub struct ConnectToken(pub Vec<u8>);

/// Issue a token for `client_id` after the operator's own check (password,
/// invite, allowlist) has passed. The HTTPS front for this lives in the
/// gamemode, not here; this function only signs.
pub fn issue(
    _private_key: &[u8; 32],
    _client_id: u64,
    _server_addr: std::net::SocketAddr,
) -> ConnectToken {
    // TODO(M0): renet_netcode::ConnectToken::generate(current_time, protocol_id, expire_seconds,
    // client_id, timeout_seconds, server_addresses, user_data, private_key)
    ConnectToken(Vec::new())
}
