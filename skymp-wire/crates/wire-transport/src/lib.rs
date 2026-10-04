//! Transport: renet over netcode. This is the only crate that names renet
//! types, so ADR-011's alternative (quinn) is a swap of this crate alone.
//!
//! Inbound path, in order: transport limits, decode (`wire-codec`), direction
//! check, structural validation (`wire-validate`). Only what passes all four
//! reaches the bridge as [`Inbound::Message`]; everything else is surfaced as
//! [`Inbound::Rejected`] with a stable [`reason_code`]. Nothing in this crate
//! panics on packet input; renet's own parser answers malformed packets by
//! disconnecting, and `fuzz/transport_ingest` hunts the rest.

pub mod channels;
#[cfg(feature = "harness")]
pub mod harness;
pub mod limits;
pub mod token;

use std::collections::HashMap;
use std::net::{SocketAddr, UdpSocket};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use renet::{Bytes, RenetClient, RenetServer, ServerEvent};
use renet_netcode::{
    ClientAuthentication, NetcodeClientTransport, NetcodeServerTransport, ServerAuthentication,
    ServerConfig,
};
use wire_schema::Message;
use wire_validate::{ClientGuard, Reject};

pub use channels::Delivery;
use limits::{ByteBudget, Limits};

/// Transport-level failures, distinct from decode and validation failures so
/// the logs can tell them apart.
#[derive(Debug, thiserror::Error)]
pub enum TransportError {
    /// The socket could not be bound or the transport could not start.
    #[error("E_TX_BIND")]
    Bind,
    /// A message could not be encoded; a bug in the caller, never an input.
    #[error("E_TX_ENCODE")]
    Encode,
    /// A configured limit refused the operation (channel full).
    #[error("E_TX_LIMIT")]
    Limit,
    /// The client is not connected.
    #[error("E_TX_NO_CLIENT")]
    NoClient,
    /// A connect token could not be generated or parsed.
    #[error("E_TX_TOKEN")]
    Token,
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
    /// A transport limit (size, byte budget) refused the message before decode.
    Limit,
    /// A message whose direction is server-to-client arrived from a client.
    Direction,
    /// The connect token's user data did not carry the server's password;
    /// the connection is dropped before the server ever sees it.
    Password,
}

impl std::fmt::Display for RejectKind {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            RejectKind::Decode(e) => write!(f, "{e}"),
            RejectKind::Validate(r) => write!(f, "{r}"),
            RejectKind::Limit => f.write_str("E_TX_LIMIT"),
            RejectKind::Direction => f.write_str("E_TX_DIRECTION"),
            RejectKind::Password => f.write_str("E_TX_PASSWORD"),
        }
    }
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
        RejectKind::Validate(Reject::Range) => 207,
        RejectKind::Limit => 301,
        RejectKind::Direction => 302,
        RejectKind::Password => 303,
    }
}

/// True for the messages a client may send (the schema's direction table).
/// Everything else arriving from a client is rejected as
/// [`RejectKind::Direction`] before validation.
pub fn client_may_send(msg: &Message) -> bool {
    matches!(
        msg.direction(),
        wire_schema::Direction::ClientToServer | wire_schema::Direction::Both
    )
}

/// True for the messages a server may send; the client drops the rest.
pub fn server_may_send(msg: &Message) -> bool {
    matches!(
        msg.direction(),
        wire_schema::Direction::ServerToClient | wire_schema::Direction::Both
    )
}

/// A message encoded once, cheap to clone and to send to many clients.
#[derive(Clone, Debug)]
pub struct Encoded {
    bytes: Bytes,
    default_channel: u8,
}

impl Encoded {
    /// Encode `msg` (inside its byte cap, or `Encode`).
    pub fn new(msg: &Message) -> Result<Self, TransportError> {
        let bytes = wire_codec::encode(msg).map_err(|_| TransportError::Encode)?;
        Ok(Self {
            bytes: Bytes::from(bytes),
            default_channel: channels::for_message(msg),
        })
    }

    /// Encoded length in bytes.
    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    /// True for an empty encoding, which no message has.
    pub fn is_empty(&self) -> bool {
        self.bytes.is_empty()
    }
}

/// netcode protocol id: the ASCII tag `SKYMPW` in the high bytes and
/// [`wire_schema::SCHEMA_VERSION`] in the low two, so a client built against
/// another schema cannot even complete the handshake.
pub fn protocol_id() -> u64 {
    u64::from_be_bytes(*b"SKYMPW\0\0") | u64::from(wire_schema::SCHEMA_VERSION)
}

/// Wall clock as netcode wants it. Falls back to zero before the epoch,
/// which only a broken clock produces.
pub fn unix_now() -> Duration {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
}

fn ms(dt: Duration) -> u64 {
    u64::try_from(dt.as_millis()).unwrap_or(u64::MAX)
}

/// How a server binds: limits, authentication, and the password clients
/// must carry in their connect token's user data (none in the lab).
pub struct Options {
    /// Resource bounds.
    pub limits: Limits,
    /// Token checking.
    pub auth: token::Auth,
    /// The server's password, if it has one.
    pub password: Option<String>,
    /// Addresses clients use to reach this server, when they differ from the
    /// bound one (a container behind NAT); a secure token must name one.
    pub public_addresses: Vec<SocketAddr>,
}

/// Server-side transport. Owns the renet server, the netcode transport, and
/// one [`ClientGuard`] plus byte budget per connection.
pub struct Server {
    renet: RenetServer,
    transport: NetcodeServerTransport,
    guards: HashMap<u64, (ClientGuard, ByteBudget)>,
    limits: Limits,
    password: Option<[u8; token::USER_DATA_BYTES]>,
    now_ms: u64,
    local_addr: SocketAddr,
    unsecure: bool,
    io_errors: u64,
}

impl Server {
    /// Bind to `addr` with `limits`. Lab uses [`token::Auth::Unsecure`]; public
    /// servers must use [`token::Auth::Secure`] (ADR-011). Port 0 binds an
    /// ephemeral port; read it back with [`Server::local_addr`].
    pub fn bind(
        addr: SocketAddr,
        limits: Limits,
        auth: token::Auth,
    ) -> Result<Self, TransportError> {
        Self::bind_with(
            addr,
            Options {
                limits,
                auth,
                password: None,
                public_addresses: Vec::new(),
            },
        )
    }

    /// Bind with every option. `max_clients` above netcode's ceiling is
    /// clamped to it.
    pub fn bind_with(addr: SocketAddr, options: Options) -> Result<Self, TransportError> {
        let Options {
            mut limits,
            auth,
            password,
            public_addresses,
        } = options;
        limits.max_clients = limits.max_clients.clamp(1, limits::NETCODE_MAX_CLIENTS);
        let socket = UdpSocket::bind(addr).map_err(|_| TransportError::Bind)?;
        let local_addr = socket.local_addr().map_err(|_| TransportError::Bind)?;
        let (authentication, unsecure) = match auth {
            token::Auth::Unsecure => (ServerAuthentication::Unsecure, true),
            token::Auth::Secure { private_key } => {
                (ServerAuthentication::Secure { private_key }, false)
            }
        };
        let mut addresses = public_addresses;
        if !addresses.contains(&local_addr) {
            addresses.push(local_addr);
        }
        let config = ServerConfig {
            current_time: unix_now(),
            max_clients: limits.max_clients,
            protocol_id: protocol_id(),
            public_addresses: addresses,
            authentication,
        };
        let transport =
            NetcodeServerTransport::new(config, socket).map_err(|_| TransportError::Bind)?;
        let renet = RenetServer::new(channels::connection_config(&limits));
        Ok(Self {
            renet,
            transport,
            guards: HashMap::new(),
            limits,
            password: password
                .filter(|p| !p.is_empty())
                .map(|p| token::password_user_data(&p)),
            now_ms: 0,
            local_addr,
            unsecure,
            io_errors: 0,
        })
    }

    /// The address a client connects from, while it is connected.
    pub fn client_addr(&self, client: u64) -> Option<SocketAddr> {
        self.transport.client_addr(client)
    }

    /// The address the socket is bound to.
    pub fn local_addr(&self) -> SocketAddr {
        self.local_addr
    }

    /// True when bound with [`token::Auth::Unsecure`]; log it loudly.
    pub fn is_unsecure(&self) -> bool {
        self.unsecure
    }

    /// Connected clients right now.
    pub fn client_count(&self) -> usize {
        self.renet.connected_clients()
    }

    /// Socket-level errors seen so far, for the stats endpoint.
    pub fn io_errors(&self) -> u64 {
        self.io_errors
    }

    /// Advance the transport by `dt`, ingest packets, and drain events into
    /// `out`. Every message in `out` has passed limits, decode, direction, and
    /// validation. Outgoing packets are flushed at the end.
    pub fn poll(&mut self, dt: Duration, out: &mut Vec<Inbound>) {
        self.now_ms = self.now_ms.saturating_add(ms(dt));
        if self.transport.update(dt, &mut self.renet).is_err() {
            self.io_errors = self.io_errors.saturating_add(1);
        }
        while let Some(event) = self.renet.get_event() {
            match event {
                ServerEvent::ClientConnected { client_id } => {
                    if let Some(want) = &self.password {
                        if self.transport.user_data(client_id).as_ref() != Some(want) {
                            self.renet.disconnect(client_id);
                            out.push(Inbound::Rejected {
                                client: client_id,
                                reject: RejectKind::Password,
                            });
                            continue;
                        }
                    }
                    self.guards.insert(client_id, Default::default());
                    out.push(Inbound::Connected { client: client_id });
                }
                ServerEvent::ClientDisconnected { client_id, reason } => {
                    // a client refused for its password never got a guard and is not announced
                    if self.guards.remove(&client_id).is_some() {
                        out.push(Inbound::Disconnected {
                            client: client_id,
                            reason: reason.to_string(),
                        });
                    }
                }
            }
        }
        for client in self.renet.clients_id() {
            let Some((guard, budget)) = self.guards.get_mut(&client) else {
                continue; // refused at connect, disconnect pending
            };
            for channel in channels::ALL {
                while let Some(bytes) = self.renet.receive_message(client, channel) {
                    out.push(ingest(
                        client,
                        &bytes,
                        guard,
                        budget,
                        &self.limits,
                        self.now_ms,
                    ));
                }
            }
        }
        self.transport.send_packets(&mut self.renet);
    }

    /// Encode and queue one message to one client on the channel its family
    /// maps to. `Limit` means the channel is full; the caller decides whether
    /// that client is worth keeping.
    pub fn send(&mut self, client: u64, msg: &Message) -> Result<(), TransportError> {
        self.send_with(client, msg, Delivery::Default)
    }

    /// Encode and queue one message with the sender's choice of delivery.
    pub fn send_with(
        &mut self,
        client: u64,
        msg: &Message,
        delivery: Delivery,
    ) -> Result<(), TransportError> {
        let encoded = Encoded::new(msg)?;
        self.send_encoded(client, &encoded, delivery)
    }

    /// Queue an already encoded message: the fan-out path, which encodes once
    /// and sends the same bytes to many clients.
    pub fn send_encoded(
        &mut self,
        client: u64,
        encoded: &Encoded,
        delivery: Delivery,
    ) -> Result<(), TransportError> {
        if !self.renet.is_connected(client) {
            return Err(TransportError::NoClient);
        }
        let channel = match delivery {
            Delivery::Default => encoded.default_channel,
            Delivery::Reliable => channels::RELIABLE_ORDERED,
            Delivery::Unreliable => channels::UNRELIABLE,
        };
        if !self.renet.can_send_message(client, channel, encoded.bytes.len()) {
            return Err(TransportError::Limit);
        }
        self.renet.send_message(client, channel, encoded.bytes.clone());
        Ok(())
    }

    /// Drop a client; the reason reaches it through netcode.
    pub fn disconnect(&mut self, client: u64) {
        self.renet.disconnect(client);
    }
}

fn ingest(
    client: u64,
    bytes: &[u8],
    guard: &mut ClientGuard,
    budget: &mut ByteBudget,
    limits: &Limits,
    now_ms: u64,
) -> Inbound {
    if bytes.len() > limits.max_from_client
        || !budget.charge(now_ms, bytes.len(), limits.bytes_per_s_per_client)
    {
        return Inbound::Rejected {
            client,
            reject: RejectKind::Limit,
        };
    }
    match wire_codec::decode(bytes) {
        Err(e) => Inbound::Rejected {
            client,
            reject: RejectKind::Decode(e),
        },
        Ok(msg) if !client_may_send(&msg) => Inbound::Rejected {
            client,
            reject: RejectKind::Direction,
        },
        Ok(msg) => match wire_validate::validate(&msg, guard, now_ms) {
            Err(r) => Inbound::Rejected {
                client,
                reject: RejectKind::Validate(r),
            },
            Ok(()) => Inbound::Message { client, msg },
        },
    }
}

/// How a client's connection ended, in netcode's terms without naming netcode.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ClientEnd {
    /// The server refused the handshake (full, or a bad token).
    Denied,
    /// The handshake or the connection timed out.
    TimedOut,
    /// The server closed the connection.
    ByServer,
    /// This side closed it.
    ByClient,
    /// The connect token expired before the handshake finished.
    TokenExpired,
    /// renet dropped the connection (a channel overflowed, a malformed packet).
    Transport,
}

/// Client-side transport, used by `wire-client-ffi` and by difftest's wire driver.
pub struct Client {
    renet: RenetClient,
    transport: NetcodeClientTransport,
    limits: Limits,
    now_ms: u64,
    rejected: u64,
    io_errors: u64,
    guard: ClientGuard,
}

impl Client {
    /// Connect to `server` with a token from [`token`]. An empty token means
    /// an unsecure connection, which only a lab server accepts.
    pub fn connect(server: SocketAddr, tok: token::ConnectToken) -> Result<Self, TransportError> {
        Self::connect_with(server, tok, None)
    }

    /// Connect, carrying the server's password in the token's user data when
    /// the connection is unsecure (a secure token carries what its issuer put
    /// there).
    pub fn connect_with(
        server: SocketAddr,
        tok: token::ConnectToken,
        password: Option<&str>,
    ) -> Result<Self, TransportError> {
        let bind: SocketAddr = if server.is_ipv4() {
            "0.0.0.0:0"
        } else {
            "[::]:0"
        }
        .parse()
        .map_err(|_| TransportError::Bind)?;
        let socket = UdpSocket::bind(bind).map_err(|_| TransportError::Bind)?;
        let authentication = if tok.0.is_empty() {
            let client_id = u64::from_le_bytes(renet_netcode::generate_random_bytes::<8>());
            ClientAuthentication::Unsecure {
                protocol_id: protocol_id(),
                client_id,
                server_addr: server,
                user_data: password.map(token::password_user_data),
            }
        } else {
            let connect_token = renet_netcode::ConnectToken::read(&mut tok.0.as_slice())
                .map_err(|_| TransportError::Token)?;
            ClientAuthentication::Secure { connect_token }
        };
        let transport = NetcodeClientTransport::new(unix_now(), authentication, socket)
            .map_err(|_| TransportError::Bind)?;
        let limits = Limits::default();
        let renet = RenetClient::new(channels::connection_config(&limits));
        Ok(Self {
            renet,
            transport,
            limits,
            now_ms: 0,
            rejected: 0,
            io_errors: 0,
            guard: ClientGuard::default(),
        })
    }

    /// Why the connection ended, once it has.
    pub fn end(&self) -> Option<ClientEnd> {
        use renet_netcode::NetcodeDisconnectReason as D;
        if let Some(r) = self.transport.disconnect_reason() {
            return Some(match r {
                D::ConnectionDenied => ClientEnd::Denied,
                D::ConnectTokenExpired => ClientEnd::TokenExpired,
                D::ConnectionTimedOut
                | D::ConnectionResponseTimedOut
                | D::ConnectionRequestTimedOut => ClientEnd::TimedOut,
                D::DisconnectedByClient => ClientEnd::ByClient,
                D::DisconnectedByServer => ClientEnd::ByServer,
            });
        }
        self.renet
            .is_disconnected()
            .then_some(ClientEnd::Transport)
    }

    /// True once the handshake completed.
    pub fn is_connected(&self) -> bool {
        self.renet.is_connected()
    }

    /// True once the server dropped us or the handshake failed.
    pub fn is_disconnected(&self) -> bool {
        self.renet.is_disconnected()
    }

    /// Messages the client dropped as malformed, oversized, or wrong-direction.
    pub fn rejected(&self) -> u64 {
        self.rejected
    }

    /// Socket-level errors seen so far.
    pub fn io_errors(&self) -> u64 {
        self.io_errors
    }

    /// Advance by `dt`, then drain decoded server messages into `out`, oldest
    /// first per channel. Outgoing packets are flushed at the end.
    pub fn poll(&mut self, dt: Duration, out: &mut Vec<Message>) {
        self.now_ms = self.now_ms.saturating_add(ms(dt));
        if self.transport.update(dt, &mut self.renet).is_err() {
            self.io_errors = self.io_errors.saturating_add(1);
        }
        for channel in channels::ALL {
            while let Some(bytes) = self.renet.receive_message(channel) {
                if bytes.len() > self.limits.max_from_server {
                    self.rejected = self.rejected.saturating_add(1);
                    continue;
                }
                match wire_codec::decode(&bytes) {
                    Ok(msg)
                        if server_may_send(&msg)
                            && wire_validate::validate(&msg, &mut self.guard, self.now_ms)
                                .is_ok() =>
                    {
                        out.push(msg)
                    }
                    _ => self.rejected = self.rejected.saturating_add(1),
                }
            }
        }
        if self.transport.send_packets(&mut self.renet).is_err() {
            self.io_errors = self.io_errors.saturating_add(1);
        }
    }

    /// Encode and queue one message on the channel its family maps to.
    pub fn send(&mut self, msg: &Message) -> Result<(), TransportError> {
        self.send_with(msg, Delivery::Default)
    }

    /// Encode and queue one message with the sender's choice of delivery.
    pub fn send_with(&mut self, msg: &Message, delivery: Delivery) -> Result<(), TransportError> {
        let bytes = wire_codec::encode(msg).map_err(|_| TransportError::Encode)?;
        let channel = channels::for_send(msg, delivery);
        if !self.renet.can_send_message(channel, bytes.len()) {
            return Err(TransportError::Limit);
        }
        self.renet.send_message(channel, Bytes::from(bytes));
        Ok(())
    }

    /// Leave cleanly; the server sees a Disconnected event.
    pub fn disconnect(&mut self) {
        self.transport.disconnect();
        self.renet.disconnect();
    }
}

#[cfg(test)]
#[allow(clippy::expect_used, clippy::unreachable)]
mod tests {
    use super::*;
    use std::thread::sleep;
    use wire_schema::{ClientId, FormId, MovementSample, Transform};

    #[test]
    fn direction_tables() {
        assert!(!client_may_send(&Message::Welcome {
            client_id: ClientId(1),
            server_time_ms: 0
        }));
        assert!(!client_may_send(&Message::Refuse { reason: 0 }));
        assert!(!client_may_send(&Message::HostGrant { cell: FormId(1) }));
        assert!(!client_may_send(&Message::HostRelease { cell: FormId(1) }));
        assert!(!server_may_send(&Message::Movement(movement(1))));
        assert!(server_may_send(&Message::HostGrant { cell: FormId(1) }));
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
            RejectKind::Validate(Reject::Range),
            RejectKind::Limit,
            RejectKind::Direction,
            RejectKind::Password,
        ];
        let mut codes: Vec<u16> = kinds.iter().map(reason_code).collect();
        codes.sort_unstable();
        codes.dedup();
        assert_eq!(codes.len(), kinds.len());
    }

    #[test]
    fn protocol_id_carries_the_schema_version() {
        assert_eq!(
            protocol_id() & 0xFFFF,
            u64::from(wire_schema::SCHEMA_VERSION)
        );
        assert_eq!(
            protocol_id() >> 16,
            u64::from_be_bytes(*b"SKYMPW\0\0") >> 16
        );
    }

    fn movement(seq: u32) -> MovementSample {
        MovementSample {
            seq,
            actor: FormId(0x14),
            transform: Transform {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                yaw: 0.0,
                pitch: 0.0,
            },
            run: false,
            sneak: false,
        }
    }

    /// Pump both ends until `done` holds or the deadline passes.
    fn pump(
        server: &mut Server,
        client: &mut Client,
        events: &mut Vec<Inbound>,
        inbox: &mut Vec<Message>,
        mut done: impl FnMut(&[Inbound], &[Message], &Client) -> bool,
    ) -> bool {
        let step = Duration::from_millis(10);
        for _ in 0..500 {
            client.poll(step, inbox);
            server.poll(step, events);
            if done(events, inbox, client) {
                return true;
            }
            sleep(step);
        }
        false
    }

    #[test]
    fn loopback_connect_message_reject_and_reply() {
        let localhost: SocketAddr = "127.0.0.1:0".parse().expect("literal");
        let mut server =
            Server::bind(localhost, Limits::default(), token::Auth::Unsecure).expect("bind");
        assert!(server.is_unsecure());
        let mut client =
            Client::connect(server.local_addr(), token::ConnectToken(Vec::new())).expect("connect");
        let mut events = Vec::new();
        let mut inbox = Vec::new();

        assert!(
            pump(
                &mut server,
                &mut client,
                &mut events,
                &mut inbox,
                |ev, _, c| c.is_connected()
                    && ev.iter().any(|e| matches!(e, Inbound::Connected { .. }))
            ),
            "handshake"
        );
        assert!(client.is_connected());
        let client_id = match events
            .iter()
            .find(|e| matches!(e, Inbound::Connected { .. }))
        {
            Some(Inbound::Connected { client }) => *client,
            _ => unreachable!("checked above"),
        };
        events.clear();

        client.send(&Message::Movement(movement(1))).expect("send");
        assert!(
            pump(
                &mut server,
                &mut client,
                &mut events,
                &mut inbox,
                |ev, _, _| ev.iter().any(|e| matches!(e, Inbound::Message { .. }))
            ),
            "message"
        );
        assert!(
            matches!(events.iter().find(|e| matches!(e, Inbound::Message { .. })), Some(Inbound::Message { msg: Message::Movement(m), .. }) if m.seq == 1)
        );
        events.clear();

        // The same seq again is a replay; the server surfaces exactly one rejection.
        client.send(&Message::Movement(movement(1))).expect("send");
        assert!(
            pump(
                &mut server,
                &mut client,
                &mut events,
                &mut inbox,
                |ev, _, _| ev.iter().any(|e| matches!(e, Inbound::Rejected { .. }))
            ),
            "reject"
        );
        let rejects: Vec<&Inbound> = events
            .iter()
            .filter(|e| matches!(e, Inbound::Rejected { .. }))
            .collect();
        assert_eq!(rejects.len(), 1);
        assert!(matches!(
            rejects.first(),
            Some(Inbound::Rejected {
                reject: RejectKind::Validate(Reject::SeqReplay),
                ..
            })
        ));
        events.clear();

        // Server to client on the reliable-ordered channel.
        server
            .send(
                client_id,
                &Message::HostGrant {
                    cell: FormId(0x1234),
                },
            )
            .expect("send");
        assert!(
            pump(
                &mut server,
                &mut client,
                &mut events,
                &mut inbox,
                |_, inbox, _| !inbox.is_empty()
            ),
            "reply"
        );
        assert!(matches!(inbox.first(), Some(Message::HostGrant { cell }) if cell.0 == 0x1234));
        assert_eq!(client.rejected(), 0);

        client.disconnect();
        assert!(
            pump(
                &mut server,
                &mut client,
                &mut events,
                &mut inbox,
                |ev, _, _| ev.iter().any(|e| matches!(e, Inbound::Disconnected { .. }))
            ),
            "disconnect"
        );
        assert_eq!(server.client_count(), 0);
    }

    #[test]
    fn secure_token_round_trips_through_the_handshake() {
        let private_key: [u8; 32] = renet_netcode::generate_random_bytes();
        let localhost: SocketAddr = "127.0.0.1:0".parse().expect("literal");
        let mut server = Server::bind(
            localhost,
            Limits::default(),
            token::Auth::Secure { private_key },
        )
        .expect("bind");
        let tok = token::issue(&private_key, 42, server.local_addr()).expect("issue");
        assert!(!tok.0.is_empty());
        let mut client = Client::connect(server.local_addr(), tok).expect("connect");
        let mut events = Vec::new();
        let mut inbox = Vec::new();
        assert!(
            pump(
                &mut server,
                &mut client,
                &mut events,
                &mut inbox,
                |ev, _, _| ev
                    .iter()
                    .any(|e| matches!(e, Inbound::Connected { client: 42 }))
            ),
            "secure handshake"
        );
    }

    #[test]
    fn wrong_key_never_connects() {
        let private_key: [u8; 32] = renet_netcode::generate_random_bytes();
        let other_key: [u8; 32] = renet_netcode::generate_random_bytes();
        let localhost: SocketAddr = "127.0.0.1:0".parse().expect("literal");
        let mut server = Server::bind(
            localhost,
            Limits::default(),
            token::Auth::Secure { private_key },
        )
        .expect("bind");
        let tok = token::issue(&other_key, 7, server.local_addr()).expect("issue");
        let mut client = Client::connect(server.local_addr(), tok).expect("connect");
        let mut events = Vec::new();
        let mut inbox = Vec::new();
        let step = Duration::from_millis(10);
        for _ in 0..100 {
            client.poll(step, &mut inbox);
            server.poll(step, &mut events);
            sleep(step);
        }
        assert!(events
            .iter()
            .all(|e| !matches!(e, Inbound::Connected { .. })));
        assert_eq!(server.client_count(), 0);
    }

    fn connect_pair(password: Option<&str>, client_password: Option<&str>) -> (Server, Client, Vec<Inbound>, Vec<Message>) {
        let localhost: SocketAddr = "127.0.0.1:0".parse().expect("literal");
        let server = Server::bind_with(
            localhost,
            Options {
                limits: Limits::default(),
                auth: token::Auth::Unsecure,
                password: password.map(String::from),
                public_addresses: Vec::new(),
            },
        )
        .expect("bind");
        let client = Client::connect_with(
            server.local_addr(),
            token::ConnectToken(Vec::new()),
            client_password,
        )
        .expect("connect");
        (server, client, Vec::new(), Vec::new())
    }

    #[test]
    fn the_password_gate() {
        let (mut server, mut client, mut events, mut inbox) = connect_pair(Some("lab"), Some("lab"));
        assert!(
            pump(&mut server, &mut client, &mut events, &mut inbox, |ev, _, _| ev
                .iter()
                .any(|e| matches!(e, Inbound::Connected { .. }))),
            "the right password connects"
        );

        let (mut server, mut client, mut events, mut inbox) = connect_pair(Some("lab"), Some("nope"));
        assert!(
            pump(&mut server, &mut client, &mut events, &mut inbox, |ev, _, _| ev
                .iter()
                .any(|e| matches!(e, Inbound::Rejected { reject: RejectKind::Password, .. }))),
            "a wrong password is refused"
        );
        assert!(events.iter().all(|e| !matches!(e, Inbound::Connected { .. })));
        let mut ended = false;
        for _ in 0..200 {
            client.poll(Duration::from_millis(10), &mut inbox);
            server.poll(Duration::from_millis(10), &mut events);
            if client.end().is_some() {
                ended = true;
                break;
            }
            sleep(Duration::from_millis(10));
        }
        assert!(ended, "the refused client learns its connection ended");
        assert!(events.iter().all(|e| !matches!(e, Inbound::Disconnected { .. })), "never announced, never mourned");

        let (mut server, mut client, mut events, mut inbox) = connect_pair(Some("lab"), None);
        assert!(
            pump(&mut server, &mut client, &mut events, &mut inbox, |ev, _, _| ev
                .iter()
                .any(|e| matches!(e, Inbound::Rejected { reject: RejectKind::Password, .. }))),
            "no password is refused"
        );
    }

    fn skymp_movement() -> Message {
        Message::UpdateMovement(wire_schema::skymp::UpdateMovement {
            idx: 3,
            data: wire_schema::skymp::MovementData {
                world_or_cell: 0x3c,
                pos: [133857.0, -61130.0, 14662.0],
                run_mode: "Running".try_into().unwrap_or_default(),
                ..Default::default()
            },
            ..Default::default()
        })
    }

    #[test]
    fn skymp_messages_both_ways_and_a_large_one_in_fragments() {
        let (mut server, mut client, mut events, mut inbox) = connect_pair(None, None);
        assert!(pump(&mut server, &mut client, &mut events, &mut inbox, |ev, _, c| c.is_connected()
            && ev.iter().any(|e| matches!(e, Inbound::Connected { .. }))));
        let id = match events.iter().find(|e| matches!(e, Inbound::Connected { .. })) {
            Some(Inbound::Connected { client }) => *client,
            _ => unreachable!("checked above"),
        };
        events.clear();

        client.send_with(&skymp_movement(), Delivery::Unreliable).expect("send");
        let login = wire_json::recognize(r#"{"t":1,"contentJsonDump":"{}"}"#).expect("json");
        client.send_with(&login, Delivery::Reliable).expect("send");
        assert!(pump(&mut server, &mut client, &mut events, &mut inbox, |ev, _, _| ev
            .iter()
            .filter(|e| matches!(e, Inbound::Message { .. }))
            .count()
            >= 2));

        // a client may not send what only the server sends
        client
            .send_with(&Message::DestroyActor(wire_schema::skymp::DestroyActor::default()), Delivery::Reliable)
            .expect("send");
        assert!(pump(&mut server, &mut client, &mut events, &mut inbox, |ev, _, _| ev
            .iter()
            .any(|e| matches!(e, Inbound::Rejected { reject: RejectKind::Direction, .. }))));

        // a CreateActor of ~120 KiB: an inventory of 1000 named stacks, fragmented by renet
        let mut inv = wire_schema::skymp::Inventory::default();
        for i in 0..1000u32 {
            let _ = inv.entries.push(wire_schema::skymp::Entry {
                base_id: i,
                count: 1,
                name: Some("Iron Sword of Testing, Very Long Name, Fine".repeat(2).as_str().try_into().unwrap_or_default()),
                ..Default::default()
            });
        }
        let big = Message::CreateActor(wire_schema::skymp::CreateActor {
            idx: 9,
            is_me: true,
            props: wire_schema::skymp::CreateActorProps {
                inventory: Some(inv),
                ..Default::default()
            },
            ..Default::default()
        });
        let len = wire_codec::encode(&big).expect("encode").len();
        assert!(len > 64 * 1024, "{len}");
        server.send_with(id, &big, Delivery::Reliable).expect("send");
        assert!(pump(&mut server, &mut client, &mut events, &mut inbox, |_, inbox, _| inbox
            .iter()
            .any(|m| matches!(m, Message::CreateActor(_)))));
        assert!(inbox.contains(&big));
        assert_eq!(client.rejected(), 0);

        // the server ends it; the client is told why
        server.disconnect(id);
        let mut end = None;
        for _ in 0..200 {
            client.poll(Duration::from_millis(10), &mut inbox);
            server.poll(Duration::from_millis(10), &mut events);
            end = client.end();
            if end.is_some() {
                break;
            }
            sleep(Duration::from_millis(10));
        }
        assert_eq!(end, Some(ClientEnd::ByServer));
        assert_eq!(server.client_addr(id), None);
    }

    /// Writes real renet packets into the transport_ingest corpus. Run by hand:
    /// `cargo test -p wire-transport --features harness -- --ignored write_fuzz_seeds`.
    #[test]
    #[ignore]
    #[cfg(feature = "harness")]
    fn write_fuzz_seeds() {
        let dir = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../fuzz/corpus/transport_ingest");
        std::fs::create_dir_all(&dir).expect("corpus dir");
        for (i, p) in harness::seed_packets(&Limits::default()).iter().enumerate() {
            std::fs::write(dir.join(format!("seed-{i:02}")), p).expect("write seed");
        }
    }
}
