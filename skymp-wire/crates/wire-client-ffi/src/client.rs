//! The client, in Rust: one connection to a SkyMP server over the wire, run
//! on its own network thread, with a JSON face (ADR-019). The C exports in
//! the crate root are a thin skin over this; the Rust fakeclient uses it
//! directly, so the lab's T2 runs the same code the game does.
//!
//! Why a thread: netcode drops a connection that is silent for 15 seconds,
//! and Skyrim's main thread, which calls `Tick`, stalls through every loading
//! screen. RakNet ran its own thread for the same reason. The thread polls
//! the transport every few milliseconds, renders what arrives as SkyMP's
//! JSON into an inbox the game drains at its own pace, and sends what the
//! game queued. The inbox is bounded; a game that stops draining for long
//! enough to fill it is disconnected rather than silently desynchronized.

use std::collections::VecDeque;
use std::net::{SocketAddr, ToSocketAddrs};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{sync_channel, Receiver, SyncSender, TrySendError};
use std::sync::{Arc, Mutex};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use wire_schema::Message;
use wire_transport::{token, Client, ClientEnd, Delivery};

/// What happened, in the terms Skyrim Platform's `tick` callback uses
/// (MpClientPlugin's packet types; the numbers are an ABI).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum PacketKind {
    /// A message from the server; the content is its JSON.
    Message = 0,
    /// The connection was up and is gone.
    Disconnect = 1,
    /// The handshake finished.
    ConnectionAccepted = 2,
    /// The handshake never finished (timeout, unreachable).
    ConnectionFailed = 3,
    /// The server refused the handshake.
    ConnectionDenied = 4,
}

impl PacketKind {
    /// The number Skyrim Platform's `tick` maps to a packet type name.
    pub fn code(self) -> i32 {
        match self {
            PacketKind::Message => 0,
            PacketKind::Disconnect => 1,
            PacketKind::ConnectionAccepted => 2,
            PacketKind::ConnectionFailed => 3,
            PacketKind::ConnectionDenied => 4,
        }
    }
}

/// One event for the game.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ClientEvent {
    /// Which callback this is.
    pub kind: PacketKind,
    /// SkyMP JSON for messages, empty otherwise.
    pub json: String,
    /// Why, for the ending kinds; empty otherwise.
    pub error: String,
}

/// Why a send was refused. Stable names for logs.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum SendError {
    /// Not SkyMP JSON, or not a message a client may send.
    Recognize(String),
    /// A NaN or an infinity, or another structural rule.
    Invalid(String),
    /// No live connection.
    NotConnected,
    /// The network thread has this many sends queued already.
    Backlog,
}

impl std::fmt::Display for SendError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            SendError::Recognize(e) => write!(f, "E_CLIENT_RECOGNIZE: {e}"),
            SendError::Invalid(e) => write!(f, "E_CLIENT_INVALID: {e}"),
            SendError::NotConnected => f.write_str("E_CLIENT_NOT_CONNECTED"),
            SendError::Backlog => f.write_str("E_CLIENT_SEND_BACKLOG"),
        }
    }
}

/// Tuning, with the defaults the game uses.
#[derive(Debug, Clone)]
pub struct Options {
    /// The server's password, carried in the connect token's user data.
    pub password: Option<String>,
    /// How often the network thread polls the transport.
    pub poll_every: Duration,
    /// Bytes of rendered JSON the inbox may hold before the connection is
    /// dropped for backlog.
    pub inbox_bytes: usize,
    /// Sends the game may queue ahead of the network thread.
    pub outbox_len: usize,
    /// How long a handshake may take before it counts as failed.
    pub connect_timeout: Duration,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            password: None,
            poll_every: Duration::from_millis(5),
            inbox_bytes: 64 * 1024 * 1024,
            outbox_len: 8192,
            connect_timeout: Duration::from_secs(60),
        }
    }
}

enum Command {
    Send(Box<Message>, Delivery),
    Close,
}

#[derive(Default)]
struct Inbox {
    events: VecDeque<ClientEvent>,
    bytes: usize,
}

/// A live client: the handle the game holds.
pub struct WireClient {
    inbox: Arc<Mutex<Inbox>>,
    connected: Arc<AtomicBool>,
    commands: SyncSender<Command>,
    thread: Option<JoinHandle<()>>,
}

fn resolve(host: &str, port: u16) -> Option<SocketAddr> {
    let addrs: Vec<SocketAddr> = (host, port).to_socket_addrs().ok()?.collect();
    addrs
        .iter()
        .find(|a| a.is_ipv4())
        .or_else(|| addrs.first())
        .copied()
}

impl WireClient {
    /// Start connecting to `host:port`. Failure to resolve or to open the
    /// socket is reported the way SkyMP's client reported it: a
    /// ConnectionFailed event on the first drain, so the game's reconnect
    /// logic stays the only reconnect logic.
    pub fn connect(host: &str, port: u16, options: Options) -> WireClient {
        let inbox = Arc::new(Mutex::new(Inbox::default()));
        let connected = Arc::new(AtomicBool::new(false));
        let (tx, rx) = sync_channel::<Command>(options.outbox_len.max(1));
        let started = resolve(host, port).ok_or_else(|| format!("cannot resolve {host}:{port}")).and_then(|addr| {
            Client::connect_with(addr, token::ConnectToken(Vec::new()), options.password.as_deref())
                .map_err(|e| format!("{e} for {addr}"))
        });
        let thread = match started {
            Ok(client) => {
                let (inbox2, connected2) = (Arc::clone(&inbox), Arc::clone(&connected));
                std::thread::Builder::new()
                    .name("skymp-wire".into())
                    .spawn(move || run(client, rx, inbox2, connected2, options))
                    .map_err(|e| e.to_string())
            }
            Err(e) => Err(e),
        };
        let thread = match thread {
            Ok(t) => Some(t),
            Err(e) => {
                push(
                    &inbox,
                    ClientEvent {
                        kind: PacketKind::ConnectionFailed,
                        json: String::new(),
                        error: e,
                    },
                    usize::MAX,
                );
                None
            }
        };
        WireClient {
            inbox,
            connected,
            commands: tx,
            thread,
        }
    }

    /// True while the connection is up.
    pub fn is_connected(&self) -> bool {
        self.connected.load(Ordering::Acquire)
    }

    /// Move every pending event into `out`, oldest first.
    pub fn drain(&self, out: &mut Vec<ClientEvent>) {
        if let Ok(mut inbox) = self.inbox.lock() {
            out.extend(inbox.events.drain(..));
            inbox.bytes = 0;
        }
    }

    /// Take the oldest pending event, if any.
    pub fn pop(&self) -> Option<ClientEvent> {
        let mut inbox = self.inbox.lock().ok()?;
        let ev = inbox.events.pop_front()?;
        inbox.bytes = inbox.bytes.saturating_sub(event_size(&ev));
        Some(ev)
    }

    /// How many events are waiting.
    pub fn pending(&self) -> usize {
        self.inbox.lock().map(|i| i.events.len()).unwrap_or(0)
    }

    /// Recognize SkyMP JSON from the game, check it, queue it for the
    /// network thread.
    pub fn send_json(&self, json: &str, reliable: bool) -> Result<(), SendError> {
        let msg = wire_json::recognize(json).map_err(|e| SendError::Recognize(e.to_string()))?;
        if !wire_transport::client_may_send(&msg) {
            return Err(SendError::Recognize(format!(
                "E_TX_DIRECTION: a client does not send {}",
                msg.name()
            )));
        }
        if !wire_schema::finite::all_finite(&msg) {
            return Err(SendError::Invalid("E_VAL_NONFINITE".into()));
        }
        if !self.is_connected() {
            return Err(SendError::NotConnected);
        }
        let delivery = if reliable {
            Delivery::Reliable
        } else {
            Delivery::Unreliable
        };
        match self.commands.try_send(Command::Send(Box::new(msg), delivery)) {
            Ok(()) => Ok(()),
            Err(TrySendError::Full(_)) => Err(SendError::Backlog),
            Err(TrySendError::Disconnected(_)) => Err(SendError::NotConnected),
        }
    }

    /// Disconnect and wait (briefly) for the network thread to finish.
    pub fn close(mut self) {
        self.shutdown();
    }

    fn shutdown(&mut self) {
        let _ = self.commands.try_send(Command::Close);
        if let Some(t) = self.thread.take() {
            let _ = t.join();
        }
        self.connected.store(false, Ordering::Release);
    }
}

impl Drop for WireClient {
    fn drop(&mut self) {
        self.shutdown();
    }
}

/// What an event counts against the inbox's byte cap.
fn event_size(ev: &ClientEvent) -> usize {
    ev.json.len().saturating_add(ev.error.len()).saturating_add(64)
}

fn push(inbox: &Mutex<Inbox>, ev: ClientEvent, cap: usize) -> bool {
    let Ok(mut inbox) = inbox.lock() else {
        return false;
    };
    let size = event_size(&ev);
    if ev.kind == PacketKind::Message && inbox.bytes.saturating_add(size) > cap {
        return false;
    }
    inbox.bytes = inbox.bytes.saturating_add(size);
    inbox.events.push_back(ev);
    true
}

fn end_event(end: Option<ClientEnd>, was_connected: bool) -> ClientEvent {
    let (kind, error) = match (end, was_connected) {
        (Some(ClientEnd::Denied), _) => (PacketKind::ConnectionDenied, "E_CLIENT_DENIED: the server refused the connection"),
        (_, true) => (PacketKind::Disconnect, match end {
            Some(ClientEnd::TimedOut) => "E_CLIENT_TIMEOUT",
            Some(ClientEnd::ByServer) => "E_CLIENT_BY_SERVER",
            Some(ClientEnd::ByClient) => "E_CLIENT_BY_CLIENT",
            Some(ClientEnd::TokenExpired) => "E_CLIENT_TOKEN_EXPIRED",
            Some(ClientEnd::Transport) => "E_CLIENT_TRANSPORT",
            Some(ClientEnd::Denied) | None => "E_CLIENT_CLOSED",
        }),
        (_, false) => (PacketKind::ConnectionFailed, match end {
            Some(ClientEnd::TokenExpired) => "E_CLIENT_TOKEN_EXPIRED",
            _ => "E_CLIENT_TIMEOUT: no answer from the server",
        }),
    };
    ClientEvent {
        kind,
        json: String::new(),
        error: error.into(),
    }
}

fn run(
    mut client: Client,
    commands: Receiver<Command>,
    inbox: Arc<Mutex<Inbox>>,
    connected: Arc<AtomicBool>,
    options: Options,
) {
    let started = Instant::now();
    let mut last = Instant::now();
    let mut was_connected = false;
    let mut inbound = Vec::new();
    loop {
        let now = Instant::now();
        client.poll(now.saturating_duration_since(last), &mut inbound);
        last = now;
        if !was_connected && client.is_connected() {
            was_connected = true;
            connected.store(true, Ordering::Release);
            push(
                &inbox,
                ClientEvent {
                    kind: PacketKind::ConnectionAccepted,
                    json: String::new(),
                    error: String::new(),
                },
                usize::MAX,
            );
        }
        for msg in inbound.drain(..) {
            // render() refuses only what the transport already refuses
            // (M0 variants, non-finite floats); such a message is dropped
            let Ok(json) = wire_json::render(&msg) else {
                continue;
            };
            let ok = push(
                &inbox,
                ClientEvent {
                    kind: PacketKind::Message,
                    json,
                    error: String::new(),
                },
                options.inbox_bytes,
            );
            if !ok {
                client.disconnect();
                connected.store(false, Ordering::Release);
                push(
                    &inbox,
                    ClientEvent {
                        kind: PacketKind::Disconnect,
                        json: String::new(),
                        error: "E_CLIENT_BACKLOG: the game stopped reading messages".into(),
                    },
                    usize::MAX,
                );
                return;
            }
        }
        let ended = client.end();
        let timed_out = !was_connected && started.elapsed() > options.connect_timeout;
        if ended.is_some() || timed_out {
            connected.store(false, Ordering::Release);
            push(&inbox, end_event(ended, was_connected), usize::MAX);
            return;
        }
        loop {
            match commands.try_recv() {
                Ok(Command::Send(msg, delivery)) => {
                    // a full channel or a vanished connection surfaces as the
                    // connection's end on a later poll
                    let _ = client.send_with(&msg, delivery);
                }
                Ok(Command::Close) | Err(std::sync::mpsc::TryRecvError::Disconnected) => {
                    client.disconnect();
                    // one last poll flushes the disconnect packet
                    client.poll(Duration::ZERO, &mut inbound);
                    connected.store(false, Ordering::Release);
                    return;
                }
                Err(std::sync::mpsc::TryRecvError::Empty) => break,
            }
        }
        std::thread::sleep(options.poll_every);
    }
}

#[cfg(test)]
#[allow(clippy::panic, clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use super::*;
    use wire_transport::{limits::Limits, Inbound, Server};

    fn wait<F: FnMut() -> bool>(mut f: F) -> bool {
        for _ in 0..600 {
            if f() {
                return true;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        false
    }

    #[test]
    fn connects_exchanges_json_and_reports_the_end() {
        let mut server = Server::bind("127.0.0.1:0".parse().expect("addr"), Limits::default(), token::Auth::Unsecure)
            .expect("bind");
        let port = server.local_addr().port();
        let client = WireClient::connect("127.0.0.1", port, Options::default());
        let mut events = Vec::new();
        let mut inbound = Vec::new();
        assert!(wait(|| {
            server.poll(Duration::from_millis(10), &mut inbound);
            client.drain(&mut events);
            events.iter().any(|e| e.kind == PacketKind::ConnectionAccepted)
        }));
        assert!(client.is_connected());
        let id = match inbound.iter().find(|e| matches!(e, Inbound::Connected { .. })) {
            Some(Inbound::Connected { client }) => *client,
            other => panic!("{other:?}"),
        };
        // the game's JSON reaches the server as a typed message
        client
            .send_json(r#"{"t":1,"contentJsonDump":"{\"customPacketType\":\"loginWithSkympIo\"}"}"#, true)
            .expect("send");
        assert!(wait(|| {
            server.poll(Duration::from_millis(10), &mut inbound);
            inbound.iter().any(|e| matches!(e, Inbound::Message { msg: Message::CustomPacket(_), .. }))
        }));
        // what a client may not send never leaves
        assert!(matches!(client.send_json(r#"{"t":25,"idx":1}"#, true), Err(SendError::Recognize(_))));
        assert!(matches!(client.send_json("not json", true), Err(SendError::Recognize(_))));
        // the server's message arrives as SkyMP JSON
        let teleport = wire_json::recognize(r#"{"t":20,"idx":0,"pos":[1,2,3],"rot":[0,0,90],"worldOrCell":60}"#).expect("json");
        server.send_with(id, &teleport, Delivery::Reliable).expect("send");
        events.clear();
        assert!(wait(|| {
            server.poll(Duration::from_millis(10), &mut inbound);
            client.drain(&mut events);
            events.iter().any(|e| e.kind == PacketKind::Message)
        }));
        let m = events.iter().find(|e| e.kind == PacketKind::Message).expect("message");
        assert!(m.json.contains("\"t\":20") && m.json.contains("\"worldOrCell\":60"), "{}", m.json);
        // the server drops the client: a Disconnect event
        server.disconnect(id);
        events.clear();
        assert!(wait(|| {
            server.poll(Duration::from_millis(10), &mut inbound);
            client.drain(&mut events);
            events.iter().any(|e| e.kind == PacketKind::Disconnect)
        }));
        assert!(!client.is_connected());
        client.close();
    }

    #[test]
    fn nobody_listening_is_a_failed_connection() {
        // a port nothing listens on; a short timeout
        let client = WireClient::connect(
            "127.0.0.1",
            9,
            Options {
                connect_timeout: Duration::from_millis(300),
                ..Options::default()
            },
        );
        let mut events = Vec::new();
        assert!(wait(|| {
            client.drain(&mut events);
            !events.is_empty()
        }));
        assert_eq!(events[0].kind, PacketKind::ConnectionFailed, "{events:?}");
        assert!(!client.is_connected());
    }

    #[test]
    fn an_unresolvable_host_fails_on_the_first_drain() {
        let client = WireClient::connect("no-such-host.invalid", 7777, Options::default());
        let mut events = Vec::new();
        client.drain(&mut events);
        assert_eq!(events.len(), 1);
        assert_eq!(events[0].kind, PacketKind::ConnectionFailed);
    }
}
