//! The bridge the C++ server links against (via corrosion in CMake). C++
//! sees `WireEvent`s with already decoded, already validated payloads and
//! never a byte of network input. Deleting `Networking.cpp`, `PacketParser`,
//! and the RakNet dependency is part of the PR that lands this.
//!
//! Flag bits in `WireEvent.flags`: for Movement, [`FLAG_RUN`] and
//! [`FLAG_SNEAK`]; for Hit, [`FLAG_POWER_ATTACK`]. `WireEvent.reason` is a
//! `wire_transport::reason_code` for Rejected events and zero otherwise.

use std::time::Duration;

/// Movement: the actor is running.
pub const FLAG_RUN: u32 = 1;
/// Movement: the actor is sneaking.
pub const FLAG_SNEAK: u32 = 2;
/// Hit: the swing was a power attack.
pub const FLAG_POWER_ATTACK: u32 = 1;

#[cxx::bridge(namespace = "skymp::wire")]
mod ffi {
    /// Kinds the C++ side switches on. Payload is decoded into the fields
    /// that apply; the rest are zero. Extend by appending variants.
    #[derive(Debug)]
    enum EventKind {
        /// A client completed the handshake.
        Connected,
        /// A client left or timed out.
        Disconnected,
        /// `Message::Hello`; name and mod hashes come through accessors (M1).
        Hello,
        /// `Message::Movement`: seq, actor, transform, flags.
        Movement,
        /// `Message::Hit`: seq, actor (the attacker), target, weapon, flags.
        Hit,
        /// `Message::HostedActor`: actor, transform, health, payload index.
        HostedActor,
        /// A dropped packet; `reason` holds the code.
        Rejected,
    }

    /// Position and rotation in engine units, as decoded.
    #[derive(Debug)]
    struct Transform {
        /// World x.
        x: f32,
        /// World y.
        y: f32,
        /// World z.
        z: f32,
        /// Heading, radians.
        yaw: f32,
        /// Look pitch, radians.
        pitch: f32,
    }

    /// Flattened event. Variable-length payloads (inventory deltas, names)
    /// are fetched with the accessor functions below rather than copied
    /// into every event.
    #[derive(Debug)]
    struct WireEvent {
        /// Transport-assigned client id.
        client: u64,
        /// What this event is.
        kind: EventKind,
        /// Message sequence number, when the message has one.
        seq: u32,
        /// Primary actor form id (mover, attacker, hosted NPC).
        actor: u32,
        /// Secondary form id (hit target).
        target: u32,
        /// Weapon form id for hits.
        weapon: u32,
        /// Transform for movement and hosted actors.
        transform: Transform,
        /// Health for hosted actors.
        health: f32,
        /// Flag bits per kind; see the crate docs.
        flags: u32,
        /// Reason code for Rejected, else 0.
        reason: u16,
        /// Index into the bridge's per-poll payload arena for accessors.
        payload: u32,
    }

    extern "Rust" {
        type Server;
        /// Errors surface to C++ as `rust::Error` (cxx maps Result to exceptions).
        fn wire_server_bind(addr: &str, lab_unsecure: bool) -> Result<Box<Server>>;
        fn poll(self: &mut Server, dt_ms: u64, out: &mut Vec<WireEvent>);
        fn send_inventory_apply(
            self: &mut Server,
            client: u64,
            owner: u32,
            item: u32,
            count: i32,
        ) -> bool;
        fn send_host_grant(self: &mut Server, client: u64, cell: u32) -> bool;
        fn send_host_release(self: &mut Server, client: u64, cell: u32) -> bool;
        /// Accessor for the Nth inventory delta of a HostedActor event.
        fn inventory_delta(
            self: &Server,
            payload: u32,
            n: u32,
            item: &mut u32,
            count: &mut i32,
        ) -> bool;
    }
}

/// Bridge-side server: transport plus a per-poll arena for variable payloads.
pub struct Server {
    inner: wire_transport::Server,
    arena: Vec<wire_schema::HostedActorState>,
}

fn wire_server_bind(addr: &str, lab_unsecure: bool) -> Result<Box<Server>, BindError> {
    // TODO(M0): the Secure key comes from the server's key file, never a literal.
    let auth = if lab_unsecure {
        wire_transport::token::Auth::Unsecure
    } else {
        wire_transport::token::Auth::Secure {
            private_key: [0; 32],
        }
    };
    let addr: std::net::SocketAddr = addr.parse().map_err(|_| BindError::Addr)?;
    let inner = wire_transport::Server::bind(addr, Default::default(), auth)
        .map_err(|_| BindError::Bind)?;
    Ok(Box::new(Server {
        inner,
        arena: Vec::new(),
    }))
}

/// Bind failures crossing to C++.
#[derive(Debug)]
pub enum BindError {
    /// The address string did not parse as `ip:port`.
    Addr,
    /// The transport could not bind the socket.
    Bind,
}

impl std::fmt::Display for BindError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            BindError::Addr => f.write_str("E_BRIDGE_ADDR"),
            BindError::Bind => f.write_str("E_BRIDGE_BIND"),
        }
    }
}

impl Server {
    fn poll(&mut self, dt_ms: u64, out: &mut Vec<ffi::WireEvent>) {
        self.arena.clear();
        let mut events = Vec::new();
        self.inner.poll(Duration::from_millis(dt_ms), &mut events);
        for ev in events {
            out.push(flatten(ev, &mut self.arena));
        }
    }

    fn send_inventory_apply(&mut self, client: u64, owner: u32, item: u32, count: i32) -> bool {
        use wire_schema::{FormId, ItemDelta, Message};
        let msg = Message::InventoryApply {
            owner: FormId(owner),
            delta: ItemDelta {
                item: FormId(item),
                count,
            },
        };
        self.inner.send(client, &msg).is_ok()
    }

    fn send_host_grant(&mut self, client: u64, cell: u32) -> bool {
        self.inner
            .send(
                client,
                &wire_schema::Message::HostGrant {
                    cell: wire_schema::FormId(cell),
                },
            )
            .is_ok()
    }

    fn send_host_release(&mut self, client: u64, cell: u32) -> bool {
        self.inner
            .send(
                client,
                &wire_schema::Message::HostRelease {
                    cell: wire_schema::FormId(cell),
                },
            )
            .is_ok()
    }

    fn inventory_delta(&self, payload: u32, n: u32, item: &mut u32, count: &mut i32) -> bool {
        let Some(state) = usize::try_from(payload)
            .ok()
            .and_then(|i| self.arena.get(i))
        else {
            return false;
        };
        let Some(d) = usize::try_from(n)
            .ok()
            .and_then(|i| state.inventory_delta.get(i))
        else {
            return false;
        };
        *item = d.item.0;
        *count = d.count;
        true
    }
}

fn transform(t: &wire_schema::Transform) -> ffi::Transform {
    ffi::Transform {
        x: t.x,
        y: t.y,
        z: t.z,
        yaw: t.yaw,
        pitch: t.pitch,
    }
}

fn flatten(
    ev: wire_transport::Inbound,
    arena: &mut Vec<wire_schema::HostedActorState>,
) -> ffi::WireEvent {
    use wire_schema::Message;
    use wire_transport::{reason_code, Inbound, RejectKind};
    let mut e = ffi::WireEvent {
        client: 0,
        kind: ffi::EventKind::Rejected,
        seq: 0,
        actor: 0,
        target: 0,
        weapon: 0,
        transform: ffi::Transform {
            x: 0.0,
            y: 0.0,
            z: 0.0,
            yaw: 0.0,
            pitch: 0.0,
        },
        health: 0.0,
        flags: 0,
        reason: 0,
        payload: u32::MAX,
    };
    match ev {
        Inbound::Connected { client } => {
            e.client = client;
            e.kind = ffi::EventKind::Connected;
        }
        Inbound::Disconnected { client, .. } => {
            e.client = client;
            e.kind = ffi::EventKind::Disconnected;
        }
        Inbound::Rejected { client, reject } => {
            e.client = client;
            e.kind = ffi::EventKind::Rejected;
            e.reason = reason_code(&reject);
        }
        Inbound::Message { client, msg } => {
            e.client = client;
            match msg {
                Message::Hello(_) => e.kind = ffi::EventKind::Hello,
                Message::Movement(m) => {
                    e.kind = ffi::EventKind::Movement;
                    e.seq = m.seq;
                    e.actor = m.actor.0;
                    e.transform = transform(&m.transform);
                    e.flags =
                        (if m.run { FLAG_RUN } else { 0 }) | (if m.sneak { FLAG_SNEAK } else { 0 });
                }
                Message::Hit(h) => {
                    e.kind = ffi::EventKind::Hit;
                    e.seq = h.seq;
                    e.actor = h.attacker.0;
                    e.target = h.target.0;
                    e.weapon = h.weapon.0;
                    e.flags = if h.power_attack { FLAG_POWER_ATTACK } else { 0 };
                }
                Message::HostedActor(s) => {
                    e.kind = ffi::EventKind::HostedActor;
                    e.actor = s.actor.0;
                    e.health = s.health;
                    e.transform = transform(&s.transform);
                    e.payload = u32::try_from(arena.len()).unwrap_or(u32::MAX);
                    arena.push(s);
                }
                _ => {
                    // The transport already rejects these by direction; this
                    // arm exists because the enum is non-exhaustive.
                    e.kind = ffi::EventKind::Rejected;
                    e.reason = reason_code(&RejectKind::Direction);
                }
            }
        }
    }
    e
}
