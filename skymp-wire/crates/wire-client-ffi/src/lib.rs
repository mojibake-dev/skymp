//! C ABI for the Skyrim Platform client. Memory is owned by Rust and freed by
//! Rust; every function's doc says which pointers it retains. The C++ side
//! never sees network bytes, only [`SkympWireEvent`] views.
//!
//! Events are delivered in the order the transport produced them (FIFO), so
//! the reliable-ordered channel's ordering survives the last hop. The header
//! `include/skymp_wire.h` is generated from this file by `just wire-header`
//! and committed; CI diffs it.

use std::collections::VecDeque;
use std::time::Duration;

/// A slot that was not filled.
pub const KIND_NONE: u32 = 0;
/// `Message::Welcome`.
pub const KIND_WELCOME: u32 = 1;
/// `Message::Refuse`; `reason` is set.
pub const KIND_REFUSE: u32 = 2;
/// `Message::InventoryApply`; `actor` is the owner, `target` the item,
/// `count` the signed count.
pub const KIND_INVENTORY_APPLY: u32 = 3;
/// `Message::HostGrant`; `actor` is the cell.
pub const KIND_HOST_GRANT: u32 = 4;
/// `Message::HostRelease`; `actor` is the cell.
pub const KIND_HOST_RELEASE: u32 = 5;

/// Opaque client handle.
pub struct Client {
    inner: wire_transport::Client,
    pending: VecDeque<wire_schema::Message>,
}

/// Event view handed to C++. Mirrors wire-bridge's flattening; keep in sync.
#[repr(C)]
pub struct SkympWireEvent {
    /// One of the `KIND_*` constants.
    pub kind: u32,
    /// Message sequence number, when the message has one.
    pub seq: u32,
    /// Primary form id (owner, cell).
    pub actor: u32,
    /// Secondary form id (item).
    pub target: u32,
    /// Weapon form id.
    pub weapon: u32,
    /// World x.
    pub x: f32,
    /// World y.
    pub y: f32,
    /// World z.
    pub z: f32,
    /// Heading, radians.
    pub yaw: f32,
    /// Look pitch, radians.
    pub pitch: f32,
    /// Flag bits per kind.
    pub flags: u32,
    /// Signed item count for inventory events.
    pub count: i32,
    /// Reason code for `KIND_REFUSE`, else 0.
    pub reason: u16,
}

/// Connect to `addr` (NUL-terminated `ip:port`) with a token of `token_len`
/// bytes. Returns null on failure. Caller frees with `skymp_wire_disconnect`.
///
/// # Safety
/// `addr` must be a valid NUL-terminated string; `token` must point to
/// `token_len` readable bytes. Neither is retained after return.
#[no_mangle]
pub unsafe extern "C" fn skymp_wire_connect(
    addr: *const std::os::raw::c_char,
    token: *const u8,
    token_len: usize,
) -> *mut Client {
    if addr.is_null() || (token.is_null() && token_len > 0) {
        return std::ptr::null_mut();
    }
    // SAFETY: caller guarantees addr is NUL-terminated and readable.
    let addr = match unsafe { std::ffi::CStr::from_ptr(addr) }.to_str() {
        Ok(s) => s,
        Err(_) => return std::ptr::null_mut(),
    };
    let tok = if token_len == 0 {
        Vec::new()
    } else {
        // SAFETY: caller guarantees token points to token_len readable bytes.
        unsafe { std::slice::from_raw_parts(token, token_len) }.to_vec()
    };
    let Ok(sock) = addr.parse() else {
        return std::ptr::null_mut();
    };
    match wire_transport::Client::connect(sock, wire_transport::token::ConnectToken(tok)) {
        Ok(inner) => Box::into_raw(Box::new(Client {
            inner,
            pending: VecDeque::new(),
        })),
        Err(_) => std::ptr::null_mut(),
    }
}

/// Advance by `dt_ms` and fill `out` with up to `cap` events, oldest first.
/// Returns the count written. Events not returned this call are returned
/// next call.
///
/// # Safety
/// `client` must come from `skymp_wire_connect` and not yet be disconnected;
/// `out` must point to `cap` writable `SkympWireEvent`s.
#[no_mangle]
pub unsafe extern "C" fn skymp_wire_poll(
    client: *mut Client,
    dt_ms: u64,
    out: *mut SkympWireEvent,
    cap: usize,
) -> usize {
    if client.is_null() || (out.is_null() && cap > 0) {
        return 0;
    }
    // SAFETY: caller guarantees client is a live handle from skymp_wire_connect.
    let c = unsafe { &mut *client };
    let mut fresh = Vec::new();
    c.inner.poll(Duration::from_millis(dt_ms), &mut fresh);
    c.pending.extend(fresh);
    // SAFETY: caller guarantees out points to cap writable events.
    let out = unsafe { std::slice::from_raw_parts_mut(out, cap) };
    let mut n: usize = 0;
    for slot in out.iter_mut() {
        let Some(msg) = c.pending.pop_front() else {
            break;
        };
        *slot = flatten(&msg);
        n = n.saturating_add(1);
    }
    n
}

/// Send a movement sample. Returns false if the client is gone.
///
/// # Safety
/// `client` must be a live handle from `skymp_wire_connect`.
#[no_mangle]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn skymp_wire_send_movement(
    client: *mut Client,
    seq: u32,
    actor: u32,
    x: f32,
    y: f32,
    z: f32,
    yaw: f32,
    pitch: f32,
    run: bool,
    sneak: bool,
) -> bool {
    if client.is_null() {
        return false;
    }
    // SAFETY: caller guarantees client is a live handle.
    let c = unsafe { &mut *client };
    use wire_schema::{FormId, Message, MovementSample, Transform};
    let msg = Message::Movement(MovementSample {
        seq,
        actor: FormId(actor),
        transform: Transform {
            x,
            y,
            z,
            yaw,
            pitch,
        },
        run,
        sneak,
    });
    c.inner.send(&msg).is_ok()
}

/// Disconnect and free. `client` is invalid after this call.
///
/// # Safety
/// `client` must come from `skymp_wire_connect` and must not be used afterwards.
#[no_mangle]
pub unsafe extern "C" fn skymp_wire_disconnect(client: *mut Client) {
    if client.is_null() {
        return;
    }
    // SAFETY: caller guarantees this is the unique owner of a handle from skymp_wire_connect.
    drop(unsafe { Box::from_raw(client) });
}

fn flatten(msg: &wire_schema::Message) -> SkympWireEvent {
    use wire_schema::Message;
    let mut e = SkympWireEvent {
        kind: KIND_NONE,
        seq: 0,
        actor: 0,
        target: 0,
        weapon: 0,
        x: 0.0,
        y: 0.0,
        z: 0.0,
        yaw: 0.0,
        pitch: 0.0,
        flags: 0,
        count: 0,
        reason: 0,
    };
    match msg {
        Message::Welcome { .. } => e.kind = KIND_WELCOME,
        Message::Refuse { reason } => {
            e.kind = KIND_REFUSE;
            e.reason = *reason;
        }
        Message::InventoryApply { owner, delta } => {
            e.kind = KIND_INVENTORY_APPLY;
            e.actor = owner.0;
            e.target = delta.item.0;
            e.count = delta.count;
        }
        Message::HostGrant { cell } => {
            e.kind = KIND_HOST_GRANT;
            e.actor = cell.0;
        }
        Message::HostRelease { cell } => {
            e.kind = KIND_HOST_RELEASE;
            e.actor = cell.0;
        }
        _ => e.kind = KIND_NONE,
    }
    e
}

#[cfg(test)]
mod tests {
    use super::*;
    use wire_schema::{FormId, ItemDelta, Message};

    #[test]
    fn inventory_keeps_the_sign() {
        let e = flatten(&Message::InventoryApply {
            owner: FormId(1),
            delta: ItemDelta {
                item: FormId(2),
                count: -3,
            },
        });
        assert_eq!(e.kind, KIND_INVENTORY_APPLY);
        assert_eq!(e.count, -3);
    }
}
