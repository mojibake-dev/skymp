//! Transport: renet over netcode. This is the only crate that names renet
//! types, so ADR-011's alternative (quinn) is a swap of this crate alone.
//!
//! API shapes below follow renet 2.x as documented; verify against the pinned
//! version on first build. Bodies are skeletons.

pub mod channels;
pub mod limits;
pub mod token;

use std::time::Duration;

use wire_schema::Message;
use wire_validate::{ClientGuard, Reject};

/// Transport-level failures, distinct from decode and validation failures so
/// the logs can tell them apart.
#[derive(Debug, thiserror::Error)]
pub enum TransportError {
    #[error("E_TX_BIND")]
    Bind,
    #[error("E_TX_ENCODE")]
    Encode,
    #[error("E_TX_LIMIT")]
    Limit,
}

/// One inbound event handed to the bridge: already decoded, already validated.
#[derive(Debug)]
pub enum Inbound {
    Connected { client: u64 },
    Disconnected { client: u64, reason: String },
    Message { client: u64, msg: Message },
    /// A rejection, surfaced so the server can count and log it. Never acted on.
    Rejected { client: u64, reject: RejectKind },
}

/// Why an inbound packet was dropped.
#[derive(Debug)]
pub enum RejectKind {
    Decode(wire_codec::WireError),
    Validate(Reject),
    Limit,
}

/// Server-side transport. Owns the renet server, the netcode transport, and
/// one [`ClientGuard`] per connection.
pub struct Server {
    // renet::RenetServer + renet_netcode::NetcodeServerTransport live here.
    // Kept behind this struct so nothing else sees them.
    guards: std::collections::HashMap<u64, ClientGuard>,
    limits: limits::Limits,
    now_ms: u64,
}

impl Server {
    /// Bind to `addr` with `cfg`. Lab uses `token::Auth::Unsecure`; public
    /// servers must use `token::Auth::Secure` (see ADR-011).
    pub fn bind(_addr: std::net::SocketAddr, limits: limits::Limits, _auth: token::Auth) -> Result<Self, TransportError> {
        // TODO(M0): RenetServer::new(channels::connection_config()),
        // NetcodeServerTransport::new(ServerConfig { max_clients: limits.max_clients, .. }, socket)
        Ok(Self { guards: Default::default(), limits, now_ms: 0 })
    }

    /// Advance the transport by `dt`, ingest packets, and drain events into
    /// `out`. Every message in `out` has passed decode and validation.
    pub fn poll(&mut self, dt: Duration, out: &mut Vec<Inbound>) {
        self.now_ms = self.now_ms.saturating_add(u64::try_from(dt.as_millis()).unwrap_or(u64::MAX));
        // TODO(M0):
        // transport.update(dt, &mut server)
        // for event in server.get_event(): push Connected / Disconnected, insert/remove guard
        // for client in server.clients_id():
        //   for channel in channels::ALL:
        //     while let Some(bytes) = server.receive_message(client, channel):
        //       if bytes.len() > self.limits.max_packet { Rejected(Limit); continue }
        //       match wire_codec::decode(&bytes) {
        //         Err(e) => Rejected(Decode(e)),
        //         Ok(msg) => match wire_validate::validate(&msg, guard, self.now_ms) {
        //           Err(r) => Rejected(Validate(r)),
        //           Ok(()) => out.push(Inbound::Message { client, msg }),
        //         }
        //       }
        let _ = (&self.guards, &self.limits, out);
    }

    /// Encode and send one message to one client on the channel its family maps to.
    pub fn send(&mut self, _client: u64, msg: &Message) -> Result<(), TransportError> {
        let mut buf = [0u8; Message::MAX_ENCODED_LEN];
        let bytes = wire_codec::encode(msg, &mut buf).map_err(|_| TransportError::Encode)?;
        let _channel = channels::for_message(msg);
        // TODO(M0): server.send_message(client, channel, bytes.to_vec()); transport.send_packets(&mut server)
        let _ = bytes;
        Ok(())
    }
}

/// Client-side transport, used by `wire-client-ffi` and by difftest's wire driver.
pub struct Client {
    now_ms: u64,
}

impl Client {
    /// Connect with a token from [`token`]. Unsecure tokens are lab-only.
    pub fn connect(_server: std::net::SocketAddr, _tok: token::ConnectToken) -> Result<Self, TransportError> {
        // TODO(M0): RenetClient::new(channels::connection_config()),
        // NetcodeClientTransport::new(now, ClientAuthentication::{Unsecure|Secure}, socket)
        Ok(Self { now_ms: 0 })
    }

    /// Drain decoded, validated server messages into `out`.
    pub fn poll(&mut self, dt: Duration, out: &mut Vec<Message>) {
        self.now_ms = self.now_ms.saturating_add(u64::try_from(dt.as_millis()).unwrap_or(u64::MAX));
        // TODO(M0): same shape as Server::poll with a single guard.
        let _ = out;
    }

    /// Encode and send.
    pub fn send(&mut self, msg: &Message) -> Result<(), TransportError> {
        let mut buf = [0u8; Message::MAX_ENCODED_LEN];
        let _bytes = wire_codec::encode(msg, &mut buf).map_err(|_| TransportError::Encode)?;
        Ok(())
    }
}
