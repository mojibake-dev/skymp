//! The bridge the C++ server links against (via corrosion in CMake). C++
//! sees `WireEvent`s with already decoded, already validated payloads and
//! never a byte of network input. Deleting `Networking.cpp`, `PacketParser`,
//! and the RakNet dependency is part of the PR that lands this.

use std::time::Duration;

#[cxx::bridge(namespace = "skymp::wire")]
mod ffi {
    /// Kinds the C++ side switches on. Payload is decoded into the fields
    /// that apply; the rest are zero. Extend by appending variants.
    #[derive(Debug)]
    enum EventKind {
        Connected,
        Disconnected,
        Hello,
        Movement,
        Hit,
        HostedActor,
        Rejected,
    }

    #[derive(Debug)]
    struct Transform {
        x: f32,
        y: f32,
        z: f32,
        yaw: f32,
        pitch: f32,
    }

    /// Flattened event. Variable-length payloads (inventory deltas, names)
    /// are fetched with the accessor functions below rather than copied
    /// into every event.
    #[derive(Debug)]
    struct WireEvent {
        client: u64,
        kind: EventKind,
        seq: u32,
        actor: u32,
        target: u32,
        weapon: u32,
        transform: Transform,
        health: f32,
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
        fn send_inventory_apply(self: &mut Server, client: u64, owner: u32, item: u32, count: i32) -> bool;
        fn send_host_grant(self: &mut Server, client: u64, cell: u32) -> bool;
        fn send_host_release(self: &mut Server, client: u64, cell: u32) -> bool;
        /// Accessor for the Nth inventory delta of a HostedActor event.
        fn inventory_delta(self: &Server, payload: u32, n: u32, item: &mut u32, count: &mut i32) -> bool;
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
        wire_transport::token::Auth::Secure { private_key: [0; 32] }
    };
    let addr: std::net::SocketAddr = addr.parse().map_err(|_| BindError::Addr)?;
    let inner = wire_transport::Server::bind(addr, Default::default(), auth).map_err(|_| BindError::Bind)?;
    Ok(Box::new(Server { inner, arena: Vec::new() }))
}

/// Bind failures crossing to C++.
#[derive(Debug)]
pub enum BindError {
    Addr,
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
        self.inner.send(client, &Message::InventoryApply { owner: FormId(owner), delta: ItemDelta { item: FormId(item), count } }).is_ok()
    }

    fn send_host_grant(&mut self, client: u64, cell: u32) -> bool {
        self.inner.send(client, &wire_schema::Message::HostGrant { cell: wire_schema::FormId(cell) }).is_ok()
    }

    fn send_host_release(&mut self, client: u64, cell: u32) -> bool {
        self.inner.send(client, &wire_schema::Message::HostRelease { cell: wire_schema::FormId(cell) }).is_ok()
    }

    fn inventory_delta(&self, payload: u32, n: u32, item: &mut u32, count: &mut i32) -> bool {
        let Some(state) = usize::try_from(payload).ok().and_then(|i| self.arena.get(i)) else { return false };
        let Some(d) = usize::try_from(n).ok().and_then(|i| state.inventory_delta.get(i)) else { return false };
        *item = d.item.0;
        *count = d.count;
        true
    }
}

fn flatten(ev: wire_transport::Inbound, arena: &mut Vec<wire_schema::HostedActorState>) -> ffi::WireEvent {
    use wire_schema::Message;
    let zero = ffi::Transform { x: 0.0, y: 0.0, z: 0.0, yaw: 0.0, pitch: 0.0 };
    let mut e = ffi::WireEvent { client: 0, kind: ffi::EventKind::Rejected, seq: 0, actor: 0, target: 0, weapon: 0, transform: zero, health: 0.0, flags: 0, reason: 0, payload: u32::MAX };
    match ev {
        wire_transport::Inbound::Connected { client } => { e.client = client; e.kind = ffi::EventKind::Connected; }
        wire_transport::Inbound::Disconnected { client, .. } => { e.client = client; e.kind = ffi::EventKind::Disconnected; }
        wire_transport::Inbound::Rejected { client, .. } => { e.client = client; e.kind = ffi::EventKind::Rejected; /* TODO(M0): reason code */ }
        wire_transport::Inbound::Message { client, msg } => {
            e.client = client;
            match msg {
                Message::Hello(_) => e.kind = ffi::EventKind::Hello,
                Message::Movement(m) => {
                    e.kind = ffi::EventKind::Movement; e.seq = m.seq; e.actor = m.actor.0;
                    e.transform = ffi::Transform { x: m.transform.x, y: m.transform.y, z: m.transform.z, yaw: m.transform.yaw, pitch: m.transform.pitch };
                    e.flags = u32::from(m.run) | (u32::from(m.sneak) << 1);
                }
                Message::Hit(h) => { e.kind = ffi::EventKind::Hit; e.seq = h.seq; e.actor = h.attacker.0; e.target = h.target.0; e.weapon = h.weapon.0; e.flags = u32::from(h.power_attack); }
                Message::HostedActor(s) => {
                    e.kind = ffi::EventKind::HostedActor; e.actor = s.actor.0; e.health = s.health;
                    e.transform = ffi::Transform { x: s.transform.x, y: s.transform.y, z: s.transform.z, yaw: s.transform.yaw, pitch: s.transform.pitch };
                    e.payload = u32::try_from(arena.len()).unwrap_or(u32::MAX);
                    arena.push(s);
                }
                _ => { e.kind = ffi::EventKind::Rejected; /* wrong direction */ }
            }
        }
    }
    e
}
