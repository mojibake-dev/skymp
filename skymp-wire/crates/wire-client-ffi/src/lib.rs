//! C ABI for the Skyrim Platform client. Memory is owned by Rust and freed by
//! Rust; every function's doc says which pointers it retains. The C++ side
//! never sees network bytes, only `skymp_wire_Event` views.

use std::time::Duration;

/// Opaque client handle.
pub struct Client {
    inner: wire_transport::Client,
    pending: Vec<wire_schema::Message>,
}

/// Event view handed to C++. Mirrors wire-bridge's flattening; keep in sync.
#[repr(C)]
pub struct SkympWireEvent {
    pub kind: u32,
    pub seq: u32,
    pub actor: u32,
    pub target: u32,
    pub weapon: u32,
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub yaw: f32,
    pub pitch: f32,
    pub flags: u32,
    pub reason: u16,
}

/// Connect to `addr` (NUL-terminated) with a token of `token_len` bytes.
/// Returns null on failure. Caller frees with `skymp_wire_disconnect`.
///
/// # Safety
/// `addr` must be a valid NUL-terminated string; `token` must point to
/// `token_len` readable bytes. Neither is retained after return.
#[no_mangle]
pub unsafe extern "C" fn skymp_wire_connect(addr: *const std::os::raw::c_char, token: *const u8, token_len: usize) -> *mut Client {
    if addr.is_null() || (token.is_null() && token_len > 0) {
        return std::ptr::null_mut();
    }
    // SAFETY: caller guarantees addr is NUL-terminated and readable.
    let addr = match unsafe { std::ffi::CStr::from_ptr(addr) }.to_str() { Ok(s) => s, Err(_) => return std::ptr::null_mut() };
    // SAFETY: caller guarantees token points to token_len readable bytes.
    let tok = if token_len == 0 { Vec::new() } else { unsafe { std::slice::from_raw_parts(token, token_len) }.to_vec() };
    let Ok(sock) = addr.parse() else { return std::ptr::null_mut() };
    match wire_transport::Client::connect(sock, wire_transport::token::ConnectToken(tok)) {
        Ok(inner) => Box::into_raw(Box::new(Client { inner, pending: Vec::new() })),
        Err(_) => std::ptr::null_mut(),
    }
}

/// Advance by `dt_ms` and fill `out` with up to `cap` events. Returns the
/// count written. Events not returned this call are returned next call.
///
/// # Safety
/// `client` must come from `skymp_wire_connect` and not yet be disconnected; `out` must
/// point to `cap` writable `SkympWireEvent`s.
#[no_mangle]
pub unsafe extern "C" fn skymp_wire_poll(client: *mut Client, dt_ms: u64, out: *mut SkympWireEvent, cap: usize) -> usize {
    if client.is_null() || (out.is_null() && cap > 0) {
        return 0;
    }
    // SAFETY: caller guarantees client is a live handle from skymp_wire_connect.
    let c = unsafe { &mut *client };
    c.inner.poll(Duration::from_millis(dt_ms), &mut c.pending);
    // SAFETY: caller guarantees out points to cap writable events.
    let out = unsafe { std::slice::from_raw_parts_mut(out, cap) };
    let mut n = 0;
    for slot in out.iter_mut() {
        let Some(msg) = c.pending.pop() else { break };
        *slot = flatten(&msg);
        n = n.saturating_add(1);
    }
    n
}

/// Send a movement sample. Returns false if the client is gone.
///
/// # Safety
/// `client` must be a live handle from `connect`.
#[no_mangle]
pub unsafe extern "C" fn skymp_wire_send_movement(client: *mut Client, seq: u32, actor: u32, x: f32, y: f32, z: f32, yaw: f32, pitch: f32, run: bool, sneak: bool) -> bool {
    if client.is_null() { return false; }
    // SAFETY: caller guarantees client is a live handle.
    let c = unsafe { &mut *client };
    use wire_schema::{FormId, Message, MovementSample, Transform};
    c.inner.send(&Message::Movement(MovementSample { seq, actor: FormId(actor), transform: Transform { x, y, z, yaw, pitch }, run, sneak })).is_ok()
}

/// Disconnect and free. `client` is invalid after this call.
///
/// # Safety
/// `client` must come from `skymp_wire_connect` and must not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn skymp_wire_disconnect(client: *mut Client) {
    if client.is_null() { return; }
    // SAFETY: caller guarantees this is the unique owner of a handle from skymp_wire_connect.
    drop(unsafe { Box::from_raw(client) });
}

fn flatten(msg: &wire_schema::Message) -> SkympWireEvent {
    use wire_schema::Message;
    let mut e = SkympWireEvent { kind: 0, seq: 0, actor: 0, target: 0, weapon: 0, x: 0.0, y: 0.0, z: 0.0, yaw: 0.0, pitch: 0.0, flags: 0, reason: 0 };
    match msg {
        Message::Welcome { .. } => e.kind = 1,
        Message::Refuse { reason } => { e.kind = 2; e.reason = *reason; }
        Message::InventoryApply { owner, delta } => { e.kind = 3; e.actor = owner.0; e.target = delta.item.0; e.flags = delta.count.unsigned_abs(); }
        Message::HostGrant { cell } => { e.kind = 4; e.actor = cell.0; }
        Message::HostRelease { cell } => { e.kind = 5; e.actor = cell.0; }
        _ => e.kind = 0,
    }
    e
}
