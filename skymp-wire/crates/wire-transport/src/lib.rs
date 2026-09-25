//! Transport: renet over netcode. This is the only crate that names renet
//! types, so ADR-011's alternative (quinn) is a swap of this crate alone.
//!
//! Inbound path, in order: transport limits, decode (`wire-codec`), direction
//! check, structural validation (`wire-validate`). Only what passes all four
//! reaches the bridge as [`Inbound::Message`]; everything else is surfaced as
//! [`Inbound::Rejected`] with a stable [`reason_code`]. Bodies are skeletons
//! until the M0 transport step fills them against the pinned renet.

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
    /// The socket could not be bound.
    #[error("E_TX_BIND")]
    Bind,
    /// A message could not be encoded; a bug in the caller, never an input.
    #[error("E_TX_ENCODE")]
    Encode,
    /// A configured limit refused the operation.
    #[error("E_TX_LIMIT")]
    Limit,
}

/// One inbound event handed to the bridge: already decoded, already validated.
///
/// `Message` rides inline (no `Box`): one event per packet on the hot path,
/// and the size is bounded by the schema; the lint is silenced for that reason.
#[allow(clippy::large_enum_variant)]
#[derive(Debug)]
pub enum Inbound {
    /// A client completed the netcode handshake.
    Connected {
        /// Transport-assigned client id.
        client: u64,
    },
    /// A client left or timed out.
    Disconnected {
        /// Transport-assigned client id.
        client: u64,
        /// The transport's reason text, for logs only.
        reason: String,
    },
    /// A decoded, direction-checked, validated message.
    Message {
        /// Sender.
        client: u64,
        /// The message.
        msg: Message,
    },
    /// A rejection, surfaced so the server can count and log it. Never acted on.
    Rejected {
        /// Sender.
        client: u64,
        /// Why.
        reject: RejectKind,
    },
}

/// Why an inbound packet was dropped.
#[derive(Debug)]
pub enum RejectKind {
    /// The recognizer refused the bytes.
    Decode(wire_codec::WireError),
    /// The structural validator refused the message.
    Validate(Reject),
    /// A transport limit (size, queue, rate) refused the packet before decode.
    Limit,
    /// A message whose direction is server-to-client arrived from a client.
    Direction,
}

/// Stable numeric reason codes: the `u16` carried by `Message::Refuse` and by
/// the bridge's `WireEvent.reason`. Decode failures are 1xx, validation 2xx,
/// transport 3xx. Append only; the match is exhaustive on purpose so a new
/// reason cannot ship without a number.
pub fn reason_code(kind: &RejectKind) -> u16 {
    use wire_codec::WireError as W;
    match kind {
        RejectKind::Decode(W::TooLong { .. }) => 101,
        RejectKind::Decode(W::Malformed) => 102,
        RejectKind::Decode(W::Trailing(_)) => 103,
        RejectKind::Decode(W::NonCanonical) => 104,
        RejectKind::Validate(Reject::SchemaVersion) => 201,
        RejectKind::Validate(Reject::NonFinite) => 202,
        RejectKind::Validate(Reject::OutOfWorld) => 203,
        RejectKind::Validate(Reject::Rate) => 204,
        RejectKind::Validate(Reject::ZeroDelta) => 205,
        RejectKind::Validate(Reject::SeqReplay) => 206,
        RejectKind::Limit => 301,
        RejectKind::Direction => 302,
    }
}

/// True for the message families a client may send. Everything else arriving
/// from a client is rejected as [`RejectKind::Direction`] before validation.
pub fn client_may_send(msg: &Message) -> bool {
    matches!(
        msg,
        Message::Hello(_) | Message::Movement(_) | Message::Hit(_) | Message::HostedActor(_)
    )
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
    /// Bind to `addr` with `limits`. Lab uses `token::Auth::Unsecure`; public
    /// servers must use `token::Auth::Secure` (see ADR-011).
    pub fn bind(
        _addr: std::net::SocketAddr,
        limits: limits::Limits,
        _auth: token::Auth,
    ) -> Result<Self, TransportError> {
        // TODO(M0): RenetServer::new(channels::connection_config()),
        // NetcodeServerTransport::new(ServerConfig { max_clients: limits.max_clients, .. }, socket)
        Ok(Self {
            guards: Default::default(),
            limits,
            now_ms: 0,
        })
    }

    /// Advance the transport by `dt`, ingest packets, and drain events into
    /// `out`. Every message in `out` has passed limits, decode, direction, and
    /// validation.
    pub fn poll(&mut self, dt: Duration, out: &mut Vec<Inbound>) {
        self.now_ms = self
            .now_ms
            .saturating_add(u64::try_from(dt.as_millis()).unwrap_or(u64::MAX));
        // TODO(M0):
        // transport.update(dt, &mut server)
        // for event in server.get_event(): push Connected / Disconnected, insert/remove guard
        // for client in server.clients_id():
        //   for channel in channels::ALL:
        //     while let Some(bytes) = server.receive_message(client, channel):
        //       if bytes.len() > self.limits.max_packet { Rejected(Limit); continue }
        //       match wire_codec::decode(&bytes) {
        //         Err(e) => Rejected(Decode(e)),
        //         Ok(msg) if !client_may_send(&msg) => Rejected(Direction),
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
    pub fn connect(
        _server: std::net::SocketAddr,
        _tok: token::ConnectToken,
    ) -> Result<Self, TransportError> {
        // TODO(M0): RenetClient::new(channels::connection_config()),
        // NetcodeClientTransport::new(now, ClientAuthentication::{Unsecure|Secure}, socket)
        Ok(Self { now_ms: 0 })
    }

    /// Drain decoded, validated server messages into `out`, oldest first.
    pub fn poll(&mut self, dt: Duration, out: &mut Vec<Message>) {
        self.now_ms = self
            .now_ms
            .saturating_add(u64::try_from(dt.as_millis()).unwrap_or(u64::MAX));
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

#[cfg(test)]
mod tests {
    use super::*;
    use wire_schema::{ClientId, FormId};

    #[test]
    fn direction_table() {
        assert!(!client_may_send(&Message::Welcome {
            client_id: ClientId(1),
            server_time_ms: 0
        }));
        assert!(!client_may_send(&Message::Refuse { reason: 0 }));
        assert!(!client_may_send(&Message::HostGrant { cell: FormId(1) }));
        assert!(!client_may_send(&Message::HostRelease { cell: FormId(1) }));
    }

    #[test]
    fn reason_codes_are_distinct() {
        use wire_codec::WireError as W;
        let kinds = [
            RejectKind::Decode(W::TooLong { len: 1, max: 0 }),
            RejectKind::Decode(W::Malformed),
            RejectKind::Decode(W::Trailing(1)),
            RejectKind::Decode(W::NonCanonical),
            RejectKind::Validate(Reject::SchemaVersion),
            RejectKind::Validate(Reject::NonFinite),
            RejectKind::Validate(Reject::OutOfWorld),
            RejectKind::Validate(Reject::Rate),
            RejectKind::Validate(Reject::ZeroDelta),
            RejectKind::Validate(Reject::SeqReplay),
            RejectKind::Limit,
            RejectKind::Direction,
        ];
        let mut codes: Vec<u16> = kinds.iter().map(reason_code).collect();
        codes.sort_unstable();
        codes.dedup();
        assert_eq!(codes.len(), kinds.len());
    }
}
