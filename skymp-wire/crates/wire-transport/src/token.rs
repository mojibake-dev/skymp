//! Connect tokens. netcode assumes an issuer; for community servers the
//! server is the issuer (ADR-011). `Unsecure` exists for the lab and for
//! nothing else; a server bound with it reports so in `Server::is_unsecure`.

use std::net::SocketAddr;

use crate::TransportError;

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

/// An opaque connect token handed to a client by the issuer, serialized with
/// netcode's own layout. Empty means "connect unsecure" (lab only).
pub struct ConnectToken(pub Vec<u8>);

/// Seconds a freshly issued token stays valid.
pub const TOKEN_EXPIRE_S: u64 = 300;
/// Seconds without packets before netcode drops a connection.
pub const TOKEN_TIMEOUT_S: i32 = 15;

/// Issue a token for `client_id` after the operator's own check (password,
/// invite, allowlist) has passed. The HTTPS front for this lives in the
/// gamemode, not here; this function only signs.
pub fn issue(
    private_key: &[u8; 32],
    client_id: u64,
    server_addr: SocketAddr,
) -> Result<ConnectToken, TransportError> {
    let token = renet_netcode::ConnectToken::generate(
        crate::unix_now(),
        crate::protocol_id(),
        TOKEN_EXPIRE_S,
        client_id,
        TOKEN_TIMEOUT_S,
        vec![server_addr],
        None,
        private_key,
    )
    .map_err(|_| TransportError::Token)?;
    let mut bytes = Vec::new();
    token.write(&mut bytes).map_err(|_| TransportError::Token)?;
    Ok(ConnectToken(bytes))
}

/// Bytes of netcode user data a connect token carries.
pub const USER_DATA_BYTES: usize = renet_netcode::NETCODE_USER_DATA_BYTES;

/// The server password as connect-token user data: its UTF-8 bytes, zero
/// padded, cut at [`USER_DATA_BYTES`]. Where RakNet carried "7_" + password
/// (ADR-019); the version half of that gate is netcode's protocol id now.
pub fn password_user_data(password: &str) -> [u8; USER_DATA_BYTES] {
    let mut out = [0u8; USER_DATA_BYTES];
    for (slot, b) in out.iter_mut().zip(password.as_bytes()) {
        *slot = *b;
    }
    out
}

