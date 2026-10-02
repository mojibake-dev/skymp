//! MpClientPlugin.dll, in Rust (ADR-019). Skyrim Platform loads
//! `Data/SKSE/Plugins/MpClientPlugin.dll` by path and calls seven C exports
//! by name (skyrim-platform's MpClientPluginApi.cpp); this crate's cdylib is
//! that DLL, with the same exports and the same JSON contract, so neither
//! Skyrim Platform nor skymp5-client changes. Underneath, the bytes are the
//! wire's: renet over netcode, postcard, every capacity checked, recognized
//! in Rust before anything else sees them ([`client`]).
//!
//! Memory: every pointer the exports take is borrowed for the call only and
//! never retained. Every pointer `Tick` hands its callback is valid for that
//! callback only; Skyrim Platform copies the content into an ArrayBuffer at
//! once. Threading: the exports are called from the game's main thread; the
//! network runs on a thread of its own, and no lock is held while a callback
//! runs, so the callback may call `Send` or `DestroyClient`.

pub mod client;

use std::ffi::{c_char, c_void, CStr, CString};
use std::io::Write;
use std::sync::Mutex;

use client::{ClientEvent, Options, WireClient};

/// The callback `Tick` calls once per event: packet type, content and its
/// length, an error text (never null), and the caller's state. Null is
/// allowed and means "drop the events".
pub type OnPacket = Option<extern "C" fn(i32, *const c_char, usize, *const c_char, *mut c_void)>;

static STATE: Mutex<Option<WireClient>> = Mutex::new(None);

/// Where the plugin writes its log, relative to the game directory (the
/// process's working directory), next to Skyrim Platform's plugin logs.
const LOG_PATH: &str = "Data/Platform/Logs/MpClientPlugin-logs.txt";
/// The password file SkyMP's installer writes (MpClientPlugin.cpp).
const PASSWORD_PATH: &str = "Data/Platform/Distribution/password";
/// Log lines past this many bytes are dropped until the game restarts.
const LOG_MAX_BYTES: u64 = 4 * 1024 * 1024;

fn log(line: &str) {
    let path = std::env::var("SKYMP_WIRE_LOG").unwrap_or_else(|_| LOG_PATH.into());
    let Ok(mut f) = std::fs::OpenOptions::new().create(true).append(true).open(&path) else {
        return;
    };
    if f.metadata().map(|m| m.len()).unwrap_or(0) > LOG_MAX_BYTES {
        return;
    }
    let now = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .unwrap_or_default();
    let _ = writeln!(f, "{}.{:03} {line}", now.as_secs(), now.subsec_millis());
}

fn read_password() -> Option<String> {
    let raw = std::fs::read_to_string(PASSWORD_PATH).ok()?;
    let p = raw.trim_end_matches(['\r', '\n']).to_string();
    (!p.is_empty()).then_some(p)
}

/// Run an export's body; a panic is logged and swallowed, never unwound into
/// the game (which would abort it).
fn guard<T>(name: &str, fallback: T, f: impl FnOnce() -> T) -> T {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(f)) {
        Ok(v) => v,
        Err(_) => {
            log(&format!("E_CLIENT_PANIC in {name}"));
            fallback
        }
    }
}

/// # Safety
/// `p` is null or a valid NUL-terminated string.
unsafe fn text<'a>(p: *const c_char) -> Option<&'a str> {
    if p.is_null() {
        return None;
    }
    // SAFETY: the caller guarantees p is a valid NUL-terminated string that
    // outlives this call; we borrow it for the call only.
    unsafe { CStr::from_ptr(p) }.to_str().ok()
}

/// The version string. Nothing in skymp5-client reads it; it marks the wire.
#[no_mangle]
#[allow(non_snake_case)]
pub extern "C" fn MpCommonGetVersion() -> *const c_char {
    c"2.0.0-wire".as_ptr()
}

/// Start connecting to `host:port`, replacing any client that exists.
/// Progress arrives through `Tick`.
///
/// # Safety
/// `host` is null or a valid NUL-terminated string, borrowed for the call.
#[no_mangle]
#[allow(non_snake_case)]
pub unsafe extern "C" fn CreateClient(host: *const c_char, port: u16) {
    // SAFETY: forwarded from this function's contract.
    let host = unsafe { text(host) }.unwrap_or("").to_string();
    guard("CreateClient", (), || {
        let password = read_password();
        log(&format!("connect {host}:{port} (password {})", if password.is_some() { "set" } else { "none" }));
        let fresh = WireClient::connect(
            &host,
            port,
            Options {
                password,
                ..Options::default()
            },
        );
        let old = STATE.lock().map(|mut s| s.replace(fresh));
        // the old client (if any) shuts down here, outside the lock
        drop(old);
    })
}

/// Disconnect and forget the client.
#[no_mangle]
#[allow(non_snake_case)]
pub extern "C" fn DestroyClient() {
    guard("DestroyClient", (), || {
        let old = STATE.lock().map(|mut s| s.take());
        drop(old);
    })
}

/// True while the connection is up.
#[no_mangle]
#[allow(non_snake_case)]
pub extern "C" fn IsConnected() -> bool {
    guard("IsConnected", false, || {
        STATE
            .lock()
            .map(|s| s.as_ref().is_some_and(WireClient::is_connected))
            .unwrap_or(false)
    })
}

/// Deliver every pending event to `on_packet`, oldest first.
///
/// # Safety
/// `on_packet` is null or a function with the [`OnPacket`] signature;
/// `state` is passed back to it untouched.
#[no_mangle]
#[allow(non_snake_case)]
pub unsafe extern "C" fn Tick(on_packet: OnPacket, state: *mut c_void) {
    let Some(on_packet) = on_packet else {
        return;
    };
    let mut events: Vec<ClientEvent> = Vec::new();
    guard("Tick", (), || {
        if let Ok(s) = STATE.lock() {
            if let Some(c) = s.as_ref() {
                c.drain(&mut events);
            }
        }
    });
    // the lock is released: the callback may call Send or DestroyClient
    for ev in events {
        if !ev.error.is_empty() {
            log(&format!("event {:?}: {}", ev.kind, ev.error));
        }
        let error = CString::new(ev.error.replace('\0', " ")).unwrap_or_default();
        let content: &[u8] = ev.json.as_bytes();
        on_packet(
            ev.kind.code(),
            content.as_ptr().cast::<c_char>(),
            content.len(),
            error.as_ptr(),
            state,
        );
    }
}

fn send(json: &str, reliable: bool) {
    let result = STATE.lock().map(|s| match s.as_ref() {
        Some(c) => c.send_json(json, reliable),
        None => Err(client::SendError::NotConnected),
    });
    match result {
        Ok(Ok(())) => {}
        Ok(Err(e)) => log(&format!("send refused: {e}: {}", json.chars().take(200).collect::<String>())),
        Err(_) => log("send refused: E_CLIENT_STATE_POISONED"),
    }
}

/// Send one SkyMP message, given as its JSON (what skymp5-client's
/// networking service passes).
///
/// # Safety
/// `json` is null or a valid NUL-terminated string, borrowed for the call.
#[no_mangle]
#[allow(non_snake_case)]
pub unsafe extern "C" fn Send(json: *const c_char, reliable: bool) {
    // SAFETY: forwarded from this function's contract.
    let Some(json) = (unsafe { text(json) }) else {
        log("send refused: E_CLIENT_RECOGNIZE: not UTF-8 text");
        return;
    };
    guard("Send", (), || send(json, reliable))
}

/// Send raw bytes. Nothing in skymp5-client emits a raw send; for the
/// export's sake, bytes that are SkyMP JSON (with or without the 0x86 packet
/// id in front) are sent as `Send` would, anything else is refused.
///
/// # Safety
/// `data` points to `size` readable bytes (or is null with `size` 0),
/// borrowed for the call.
#[no_mangle]
#[allow(non_snake_case)]
pub unsafe extern "C" fn SendRaw(data: *const c_void, size: usize, reliable: bool) {
    if data.is_null() || size == 0 {
        return;
    }
    // SAFETY: the caller guarantees data points to size readable bytes for
    // the duration of the call; we copy nothing out past it.
    let bytes = unsafe { std::slice::from_raw_parts(data.cast::<u8>(), size) };
    let bytes = bytes.strip_prefix(&[0x86]).unwrap_or(bytes);
    match std::str::from_utf8(bytes) {
        Ok(json) => guard("SendRaw", (), || send(json, reliable)),
        Err(_) => log("sendRaw refused: E_CLIENT_RECOGNIZE: not SkyMP JSON"),
    }
}

#[cfg(test)]
#[allow(clippy::panic, clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use super::*;
    use std::sync::Mutex as StdMutex;
    use std::time::Duration;

    static SEEN: StdMutex<Vec<(i32, String, String)>> = StdMutex::new(Vec::new());

    extern "C" fn record(kind: i32, content: *const c_char, len: usize, error: *const c_char, _: *mut c_void) {
        // SAFETY: Tick passes content with len readable bytes and a NUL-terminated error.
        let content = unsafe { std::slice::from_raw_parts(content.cast::<u8>(), len) };
        // SAFETY: as above.
        let error = unsafe { CStr::from_ptr(error) }.to_string_lossy().into_owned();
        if let Ok(mut seen) = SEEN.lock() {
            seen.push((kind, String::from_utf8_lossy(content).into_owned(), error));
        }
    }

    #[test]
    fn the_exports_drive_a_connection_like_skyrim_platform_does() {
        std::env::set_var("SKYMP_WIRE_LOG", std::env::temp_dir().join("skymp-wire-test.log"));
        let mut server = wire_transport::Server::bind(
            "127.0.0.1:0".parse().expect("addr"),
            wire_transport::limits::Limits::default(),
            wire_transport::token::Auth::Unsecure,
        )
        .expect("bind");
        let port = server.local_addr().port();
        let host = CString::new("127.0.0.1").expect("cstr");
        // SAFETY: a valid NUL-terminated string, alive for the call.
        unsafe { CreateClient(host.as_ptr(), port) };
        let mut inbound = Vec::new();
        let mut accepted = false;
        for _ in 0..500 {
            server.poll(Duration::from_millis(10), &mut inbound);
            // SAFETY: record has the OnPacket signature.
            unsafe { Tick(Some(record), std::ptr::null_mut()) };
            if SEEN.lock().map(|s| s.iter().any(|e| e.0 == 2)).unwrap_or(false) {
                accepted = true;
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        assert!(accepted, "connectionAccepted");
        assert!(IsConnected());
        let login = CString::new(r#"{"t":1,"contentJsonDump":"{}"}"#).expect("cstr");
        // SAFETY: a valid NUL-terminated string, alive for the call.
        unsafe { Send(login.as_ptr(), true) };
        let raw = b"\x86{\"t\":11,\"baseId\":77495}";
        // SAFETY: raw is a live slice of raw.len() bytes.
        unsafe { SendRaw(raw.as_ptr().cast::<c_void>(), raw.len(), false) };
        let mut got = 0;
        for _ in 0..500 {
            server.poll(Duration::from_millis(10), &mut inbound);
            got = inbound
                .iter()
                .filter(|e| matches!(e, wire_transport::Inbound::Message { .. }))
                .count();
            if got >= 2 {
                break;
            }
            std::thread::sleep(Duration::from_millis(10));
        }
        assert_eq!(got, 2, "{inbound:?}");
        DestroyClient();
        assert!(!IsConnected());
        assert!(!MpCommonGetVersion().is_null());
    }
}
