//! The server half of the wire, for the C++ core (ADR-019). The core polls
//! for events and sends messages; both directions are SkyMP's JSON, rendered
//! and recognized here, so the core keeps its own JSON path (the
//! MessageSerializer's simdjson reader and nlohmann writer) and never sees a
//! byte a client sent. skymp5-server's WireServer (an `IServer`) is the only
//! caller; it hands the core `0x86 + JSON` exactly as PacketParser already
//! accepts it.
//!
//! The core's sends are recognized, direction-checked, validated and encoded
//! here. A broadcast loop sends the same text to many clients, so the last
//! encoding is kept and reused while the text repeats.

use std::collections::BTreeMap;
use std::net::{IpAddr, SocketAddr};
use std::time::Instant;

use wire_schema::Message;
use wire_transport::{limits::Limits, token, Delivery, Encoded, Inbound, TransportError};

#[cxx::bridge(namespace = "skymp::wire")]
mod ffi {
    /// What a polled event is.
    #[derive(Debug)]
    enum EventKind {
        /// A client finished the handshake (and passed the password).
        Connected,
        /// A client left or timed out.
        Disconnected,
        /// A recognized, validated SkyMP message; `json` holds it.
        Message,
        /// A refused packet or connection; `reason` and `detail` say why.
        /// Never acted on, only counted and logged.
        Rejected,
    }

    /// One polled event.
    #[derive(Debug)]
    struct WireEvent {
        /// What it is.
        kind: EventKind,
        /// The transport's id for the client.
        client: u64,
        /// SkyMP MsgType for messages, else 0.
        msg_type: u8,
        /// SkyMP JSON for messages, else empty.
        json: String,
        /// Reason code for rejections, else 0.
        reason: u16,
        /// For logs: why it was rejected, or why the client left.
        detail: String,
    }

    /// How to bind.
    #[derive(Debug)]
    struct BindOptions {
        /// Address to listen on: an IP literal, a host name, or empty for
        /// every interface (SkyMP's default when `listenHost` is unset).
        listen_host: String,
        /// UDP port.
        port: u16,
        /// Connections at once (clamped to netcode's 1024).
        max_clients: u32,
        /// The server password clients must carry; empty for none.
        password: String,
    }

    extern "Rust" {
        type Server;
        /// Bind the socket and start the transport. Lab authentication
        /// (unsecure netcode tokens) is the only kind before the token
        /// issuer exists; the password gate still applies.
        fn wire_server_bind(options: &BindOptions) -> Result<Box<Server>>;
        /// Advance the transport by the time since the last poll and append
        /// every event to `out`.
        fn poll(self: &mut Server, out: &mut Vec<WireEvent>);
        /// Send SkyMP JSON to a client. Returns 0, or the reason code it was
        /// refused for (a 4xx code is the JSON's, 2xx the validator's, 3xx
        /// the transport's, 101 the message's byte cap).
        fn send(self: &mut Server, client: u64, json: &str, reliable: bool) -> u16;
        /// Drop a client; it sees the server close the connection.
        fn disconnect(self: &mut Server, client: u64);
        /// The client's IP address, or an empty string once it is gone.
        fn client_ip(self: &Server, client: u64) -> String;
        /// Connected clients now.
        fn client_count(self: &Server) -> usize;
        /// Counters, as JSON, for the server's metrics and logs.
        fn stats_json(self: &Server) -> String;
    }
}

pub use ffi::{BindOptions, EventKind, WireEvent};

/// The bridge's server: the transport, the last encoding, and counters.
pub struct Server {
    inner: wire_transport::Server,
    last_poll: Instant,
    last_sent: Option<(String, Encoded)>,
    sent: u64,
    refused: BTreeMap<u16, u64>,
    rejected: BTreeMap<u16, u64>,
}

/// Bind failures crossing to C++ (as exceptions).
#[derive(Debug)]
pub enum BindError {
    /// The listen host did not parse as an IP address.
    Addr(String),
    /// The transport could not bind the socket.
    Bind(String),
}

impl std::fmt::Display for BindError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            BindError::Addr(a) => write!(f, "E_BRIDGE_ADDR: {a}"),
            BindError::Bind(e) => write!(f, "E_BRIDGE_BIND: {e}"),
        }
    }
}

/// The address to bind: every interface for an empty host, as RakNet's
/// socket did; an IP literal as given; otherwise the name's first IPv4
/// address (or its first address at all).
fn listen_ip(host: &str) -> Result<IpAddr, BindError> {
    let host = host.trim();
    if host.is_empty() {
        return Ok(IpAddr::from([0u8, 0, 0, 0]));
    }
    if let Ok(ip) = host.parse::<IpAddr>() {
        return Ok(ip);
    }
    let addrs: Vec<SocketAddr> = std::net::ToSocketAddrs::to_socket_addrs(&(host, 0u16))
        .map_err(|_| BindError::Addr(host.to_string()))?
        .collect();
    addrs
        .iter()
        .find(|a| a.is_ipv4())
        .or_else(|| addrs.first())
        .map(SocketAddr::ip)
        .ok_or_else(|| BindError::Addr(host.to_string()))
}

/// Bind with SkyMP's settings.
pub fn wire_server_bind(options: &BindOptions) -> Result<Box<Server>, BindError> {
    let ip = listen_ip(&options.listen_host)?;
    let limits = Limits {
        max_clients: usize::try_from(options.max_clients).unwrap_or(usize::MAX),
        ..Limits::default()
    };
    let inner = wire_transport::Server::bind_with(
        SocketAddr::new(ip, options.port),
        wire_transport::Options {
            limits,
            auth: token::Auth::Unsecure,
            password: Some(options.password.clone()),
            public_addresses: Vec::new(),
        },
    )
    .map_err(|e| BindError::Bind(e.to_string()))?;
    Ok(Box::new(Server {
        inner,
        last_poll: Instant::now(),
        last_sent: None,
        sent: 0,
        refused: BTreeMap::new(),
        rejected: BTreeMap::new(),
    }))
}

/// Reason codes for sends refused before the transport: the JSON's (4xx).
fn json_code(e: &wire_json::JsonError) -> u16 {
    use wire_json::JsonError as J;
    match e {
        J::Syntax(_) => 401,
        J::NoType => 402,
        J::UnknownType(_) => 403,
        J::Shape(_) => 404,
        J::NoForm(_) => 405,
        J::NonFinite => 406,
        J::TooLong { .. } => 407,
    }
}

fn transport_code(e: &TransportError) -> u16 {
    match e {
        TransportError::Encode => 101,
        TransportError::Limit => 301,
        TransportError::NoClient => 304,
        TransportError::Bind => 305,
        TransportError::Token => 306,
    }
}

impl Server {
    /// See the bridge declaration.
    pub fn poll(&mut self, out: &mut Vec<WireEvent>) {
        let now = Instant::now();
        let dt = now.saturating_duration_since(self.last_poll);
        self.last_poll = now;
        let mut events = Vec::new();
        self.inner.poll(dt, &mut events);
        for ev in events {
            out.push(match ev {
                Inbound::Connected { client } => event(ffi::EventKind::Connected, client),
                Inbound::Disconnected { client, reason } => WireEvent {
                    detail: reason,
                    ..event(ffi::EventKind::Disconnected, client)
                },
                Inbound::Message { client, msg } => match wire_json::render(&msg) {
                    Ok(json) => WireEvent {
                        msg_type: msg.msg_type().unwrap_or(0),
                        json,
                        ..event(ffi::EventKind::Message, client)
                    },
                    // an M0 message has no JSON form: the core cannot take it
                    Err(e) => self.rejection(client, json_code(&e), e.to_string()),
                },
                Inbound::Rejected { client, reject } => {
                    self.rejection(client, wire_transport::reason_code(&reject), reject.to_string())
                }
            });
        }
    }

    fn rejection(&mut self, client: u64, reason: u16, detail: String) -> WireEvent {
        let n = self.rejected.entry(reason).or_insert(0);
        *n = n.saturating_add(1);
        WireEvent {
            reason,
            detail,
            ..event(ffi::EventKind::Rejected, client)
        }
    }

    fn encode(&mut self, json: &str) -> Result<Encoded, u16> {
        if let Some((text, enc)) = &self.last_sent {
            if text == json {
                return Ok(enc.clone());
            }
        }
        let msg: Message = wire_json::recognize(json).map_err(|e| json_code(&e))?;
        if !wire_transport::server_may_send(&msg) {
            return Err(302);
        }
        let mut guard = wire_validate::ClientGuard::default();
        wire_validate::validate(&msg, &mut guard, 0).map_err(|r| {
            wire_transport::reason_code(&wire_transport::RejectKind::Validate(r))
        })?;
        let enc = Encoded::new(&msg).map_err(|e| transport_code(&e))?;
        self.last_sent = Some((json.to_string(), enc.clone()));
        Ok(enc)
    }

    /// See the bridge declaration.
    pub fn send(&mut self, client: u64, json: &str, reliable: bool) -> u16 {
        let delivery = if reliable {
            Delivery::Reliable
        } else {
            Delivery::Unreliable
        };
        let result = self
            .encode(json)
            .and_then(|enc| self.inner.send_encoded(client, &enc, delivery).map_err(|e| transport_code(&e)));
        match result {
            Ok(()) => {
                self.sent = self.sent.saturating_add(1);
                0
            }
            Err(code) => {
                let n = self.refused.entry(code).or_insert(0);
                *n = n.saturating_add(1);
                code
            }
        }
    }

    /// See the bridge declaration.
    pub fn disconnect(&mut self, client: u64) {
        self.inner.disconnect(client);
    }

    /// See the bridge declaration.
    pub fn client_ip(&self, client: u64) -> String {
        self.inner
            .client_addr(client)
            .map(|a| a.ip().to_string())
            .unwrap_or_default()
    }

    /// See the bridge declaration.
    pub fn client_count(&self) -> usize {
        self.inner.client_count()
    }

    /// See the bridge declaration.
    pub fn stats_json(&self) -> String {
        let map = |m: &BTreeMap<u16, u64>| {
            m.iter()
                .map(|(k, v)| format!("\"{k}\":{v}"))
                .collect::<Vec<_>>()
                .join(",")
        };
        format!(
            "{{\"clients\":{},\"io_errors\":{},\"sent\":{},\"send_refused\":{{{}}},\"inbound_rejected\":{{{}}}}}",
            self.inner.client_count(),
            self.inner.io_errors(),
            self.sent,
            map(&self.refused),
            map(&self.rejected)
        )
    }

    /// The bound address (tests; the C++ side knows its port).
    pub fn local_addr(&self) -> SocketAddr {
        self.inner.local_addr()
    }
}

fn event(kind: ffi::EventKind, client: u64) -> WireEvent {
    WireEvent {
        kind,
        client,
        msg_type: 0,
        json: String::new(),
        reason: 0,
        detail: String::new(),
    }
}

#[cfg(test)]
#[allow(clippy::panic, clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use super::*;
    use std::time::Duration;
    use wire_transport::Client;

    fn bind(password: &str) -> Box<Server> {
        wire_server_bind(&BindOptions {
            listen_host: "127.0.0.1".into(),
            port: 0,
            max_clients: 100,
            password: password.into(),
        })
        .expect("bind")
    }

    fn pump(server: &mut Server, client: &mut Client, out: &mut Vec<WireEvent>, inbox: &mut Vec<Message>, mut done: impl FnMut(&[WireEvent], &[Message]) -> bool) -> bool {
        for _ in 0..500 {
            client.poll(Duration::from_millis(10), inbox);
            server.poll(out);
            if done(out, inbox) {
                return true;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        false
    }

    #[test]
    fn the_core_sees_skymp_json_and_sends_it() {
        let mut server = bind("");
        let mut client =
            Client::connect(server.local_addr(), token::ConnectToken(Vec::new())).expect("connect");
        let (mut out, mut inbox) = (Vec::new(), Vec::new());
        assert!(pump(&mut server, &mut client, &mut out, &mut inbox, |o, _| o
            .iter()
            .any(|e| e.kind == EventKind::Connected)));
        let id = out.iter().find(|e| e.kind == EventKind::Connected).map(|e| e.client).expect("id");
        assert_eq!(server.client_ip(id), "127.0.0.1");

        // a client's UpdateMovement arrives as the JSON PacketParser takes
        let mv = wire_json::recognize(r#"{"t":2,"idx":0,"data":{"worldOrCell":60,"pos":[1,2,3],"rot":[0,0,0],"direction":0,"healthPercentage":1,"speed":0,"runMode":"Standing","isInJumpState":false,"isSneaking":false,"isBlocking":false,"isWeapDrawn":false,"isDead":false}}"#).expect("json");
        client.send_with(&mv, Delivery::Unreliable).expect("send");
        out.clear();
        assert!(pump(&mut server, &mut client, &mut out, &mut inbox, |o, _| o
            .iter()
            .any(|e| e.kind == EventKind::Message)));
        let m = out.iter().find(|e| e.kind == EventKind::Message).expect("message");
        assert_eq!(m.msg_type, 2);
        assert!(m.json.starts_with('{') && m.json.contains("\"runMode\":\"Standing\""), "{}", m.json);

        // the core's CreateActor goes out, twice from one encoding (the cache)
        let create = r#"{"t":33,"idx":0,"isMe":true,"transform":{"worldOrCell":60,"pos":[1,2,3],"rot":[0,0,0]},"props":{},"customPropsJsonDumps":[]}"#;
        assert_eq!(server.send(id, create, true), 0);
        assert_eq!(server.send(id, create, true), 0);
        assert!(pump(&mut server, &mut client, &mut out, &mut inbox, |_, i| i
            .iter()
            .filter(|m| matches!(m, Message::CreateActor(_)))
            .count()
            == 2));

        // what the core may not send, or cannot spell, is refused with a code
        assert_eq!(server.send(id, r#"{"t":6,"data":{"caster":1,"target":2,"isSecondActivation":false}}"#, true), 302, "Activate is client to server");
        assert_eq!(server.send(id, "{", true), 401);
        assert_eq!(server.send(id, r#"{"t":99}"#, true), 403);
        assert_eq!(server.send(id, r#"{"t":25}"#, true), 404);
        assert_eq!(server.send(id + 1, r#"{"t":25,"idx":1}"#, true), 304, "no such client");
        let stats = server.stats_json();
        assert!(stats.contains("\"sent\":2") && stats.contains("\"302\":1"), "{stats}");
    }

    #[test]
    fn listen_hosts_as_skymp_settings_give_them() {
        assert_eq!(listen_ip("").ok(), Some(IpAddr::from([0u8, 0, 0, 0])), "unset means every interface");
        assert_eq!(listen_ip("127.0.0.1").ok(), Some(IpAddr::from([127u8, 0, 0, 1])));
        assert_eq!(listen_ip("localhost").ok().map(|ip| ip.is_loopback()), Some(true));
        assert!(listen_ip("no-such-host.invalid").is_err());
        let s = wire_server_bind(&BindOptions { listen_host: String::new(), port: 0, max_clients: 4, password: String::new() });
        assert!(s.is_ok());
    }

    #[test]
    fn a_wrong_password_is_a_rejection_not_a_connection() {
        let mut server = bind("secret");
        let mut client = Client::connect_with(server.local_addr(), token::ConnectToken(Vec::new()), Some("wrong"))
            .expect("connect");
        let (mut out, mut inbox) = (Vec::new(), Vec::new());
        assert!(pump(&mut server, &mut client, &mut out, &mut inbox, |o, _| o
            .iter()
            .any(|e| e.kind == EventKind::Rejected && e.reason == 303)));
        assert!(out.iter().all(|e| e.kind != EventKind::Connected));
    }
}
