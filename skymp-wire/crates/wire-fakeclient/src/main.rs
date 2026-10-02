//! fakeclient: a headless SkyMP client on the wire (ADR-019), a drop-in for
//! the C++ fakeclient it replaces (skymp5-server/cpp/fakeclient): the same
//! options, the same `{"event": ...}` lines on stdout, the same exit codes,
//! so lab-api's `server: fakeclient` steps, `just test-proto` and difftest's
//! drivers run it unchanged. Underneath it is `skymp_wire::client`, the
//! code the game's MpClientPlugin.dll runs, so T2 exercises the client's
//! own network path.
//!
//! Two modes:
//!   smoke (default): connect, log in with a profile id, wait for our actor,
//!     send a few movement updates near the spawn, AddItem through a console
//!     command, answer SpSnippets, and exit 0.
//!   --script FILE: after the login handshake, replay JSON lines, one per
//!     step, each with "at_ms" (milliseconds since the script started) and
//!     one of "move": {"dx", "dy", "dz", "runMode"} (an UpdateMovement at
//!     spawn plus offset) or "send": {...SkyMP JSON...} (sent as is; the
//!     strings "{{idx}}" and "{{worldOrCell}}" become this client's numbers),
//!     plus an optional "reliable" (default true).
//! Output: one JSON object per line. Exit 0 on success, 1 on any failure or
//! timeout, 2 on bad options.

use std::io::{BufRead, Write};
use std::time::{Duration, Instant};

use serde_json::{json, Value};
use skymp_wire::client::{ClientEvent, Options as ClientOptions, PacketKind, WireClient};

struct Options {
    host: String,
    port: u16,
    password: String,
    profile_id: i64,
    timeout_ms: u64,
    moves: u32,
    settle_ms: u64,
    add_item_base: u32,
    add_item_count: i64,
    script: Option<String>,
}

impl Default for Options {
    fn default() -> Self {
        Self {
            host: "127.0.0.1".into(),
            port: 7777,
            password: String::new(),
            profile_id: 1,
            timeout_ms: 20_000,
            moves: 5,
            settle_ms: 2_000,
            add_item_base: 0x0001_2EB7, // IronSword, Skyrim.esm
            add_item_count: 1,
            script: None,
        }
    }
}

fn emit(v: &Value) {
    let mut out = std::io::stdout().lock();
    let _ = writeln!(out, "{v}");
    let _ = out.flush();
}

fn parse_u32(s: &str) -> Option<u32> {
    match s.strip_prefix("0x").or_else(|| s.strip_prefix("0X")) {
        Some(hex) => u32::from_str_radix(hex, 16).ok(),
        None => s.parse().ok(),
    }
}

fn parse_options(args: &[String]) -> Result<Options, String> {
    let mut o = Options::default();
    let mut it = args.iter().skip(1);
    while let Some(a) = it.next() {
        let mut next = || it.next().cloned().ok_or_else(|| format!("unknown or incomplete option: {a}"));
        let bad = |v: &str| format!("bad value for {a}: {v}");
        match a.as_str() {
            "--host" => o.host = next()?,
            "--port" => {
                let v = next()?;
                o.port = v.parse().map_err(|_| bad(&v))?;
            }
            "--password" => o.password = next()?,
            "--profile-id" => {
                let v = next()?;
                o.profile_id = v.parse().map_err(|_| bad(&v))?;
            }
            "--timeout-ms" => {
                let v = next()?;
                o.timeout_ms = v.parse().map_err(|_| bad(&v))?;
            }
            "--moves" => {
                let v = next()?;
                o.moves = v.parse().map_err(|_| bad(&v))?;
            }
            "--settle-ms" => {
                let v = next()?;
                o.settle_ms = v.parse().map_err(|_| bad(&v))?;
            }
            "--add-item" => {
                let v = next()?;
                o.add_item_base = parse_u32(&v).ok_or_else(|| bad(&v))?;
            }
            "--add-item-count" => {
                let v = next()?;
                o.add_item_count = v.parse().map_err(|_| bad(&v))?;
            }
            "--script" => o.script = Some(next()?),
            _ => return Err(format!("unknown or incomplete option: {a}")),
        }
    }
    Ok(o)
}

/// Our actor, from the CreateActor with `isMe`.
#[derive(Default)]
struct Me {
    idx: Option<u64>,
    world_or_cell: u64,
    pos: [f64; 3],
    rot: [f64; 3],
}

struct Session {
    client: WireClient,
    me: Me,
    received: u64,
    failure: Option<String>,
    pending_snippets: Vec<Value>,
}

impl Session {
    fn send(&mut self, msg: &Value, reliable: bool) {
        match self.client.send_json(&msg.to_string(), reliable) {
            Ok(()) => emit(&json!({"event": "sent", "msg": msg})),
            Err(e) => emit(&json!({"event": "error", "error": e.to_string(), "msg": msg})),
        }
    }

    fn on_event(&mut self, ev: ClientEvent) {
        match ev.kind {
            PacketKind::ConnectionAccepted => emit(&json!({"event": "connected"})),
            PacketKind::Message => {
                let msg: Value = serde_json::from_str(&ev.json).unwrap_or_else(|_| json!({"raw": ev.json}));
                self.received = self.received.saturating_add(1);
                emit(&json!({"event": "message", "msg": msg}));
                let t = msg.get("t").and_then(Value::as_i64).unwrap_or(-1);
                if t == 33 && msg.get("isMe").and_then(Value::as_bool).unwrap_or(false) {
                    self.me.idx = msg.get("idx").and_then(Value::as_u64);
                    if let Some(tr) = msg.get("transform") {
                        self.me.world_or_cell = tr.get("worldOrCell").and_then(Value::as_u64).unwrap_or(0);
                        for (i, slot) in self.me.pos.iter_mut().enumerate() {
                            *slot = tr.get("pos").and_then(|p| p.get(i)).and_then(Value::as_f64).unwrap_or(0.0);
                        }
                        for (i, slot) in self.me.rot.iter_mut().enumerate() {
                            *slot = tr.get("rot").and_then(|p| p.get(i)).and_then(Value::as_f64).unwrap_or(0.0);
                        }
                    }
                    emit(&json!({"event": "actor", "idx": self.me.idx, "worldOrCell": self.me.world_or_cell, "pos": self.me.pos, "rot": self.me.rot}));
                } else if t == 30 {
                    self.pending_snippets.push(msg);
                }
            }
            PacketKind::ConnectionFailed | PacketKind::ConnectionDenied | PacketKind::Disconnect => {
                let error = if ev.error.is_empty() { "connection lost".to_string() } else { ev.error };
                emit(&json!({"event": "error", "error": error, "packetType": ev.kind.code()}));
                self.failure = Some(error);
            }
        }
    }

    fn answer_snippets(&mut self) {
        for s in std::mem::take(&mut self.pending_snippets) {
            let idx = s.get("snippetIdx").and_then(Value::as_i64).unwrap_or(-1);
            // 0xFFFFFFFF means the server wants no result (SpSnippet.cpp)
            if idx >= 0 && idx != 0xFFFF_FFFF {
                self.send(&json!({"t": 10, "returnValue": null, "snippetIdx": idx}), true);
            }
        }
    }

    fn tick(&mut self) {
        let mut events = Vec::new();
        self.client.drain(&mut events);
        for ev in events {
            self.on_event(ev);
        }
        self.answer_snippets();
    }

    fn wait_for(&mut self, timeout: Duration, mut pred: impl FnMut(&Session) -> bool) -> bool {
        let start = Instant::now();
        while start.elapsed() < timeout {
            self.tick();
            if self.failure.is_some() {
                return false;
            }
            if pred(self) {
                return true;
            }
            std::thread::sleep(Duration::from_millis(5));
        }
        false
    }

    fn settle(&mut self, ms: u64) {
        let start = Instant::now();
        while start.elapsed() < Duration::from_millis(ms) {
            self.tick();
            std::thread::sleep(Duration::from_millis(5));
        }
    }

    fn movement(&self, dx: f64, dy: f64, dz: f64, run_mode: &str) -> Value {
        let p = [self.me.pos[0] + dx, self.me.pos[1] + dy, self.me.pos[2] + dz];
        json!({
            "t": 2, "idx": self.me.idx.unwrap_or(0),
            "data": {
                "worldOrCell": self.me.world_or_cell, "pos": p, "rot": self.me.rot,
                "direction": 0.0, "healthPercentage": 1.0, "speed": 0.0, "runMode": run_mode,
                "isInJumpState": false, "isSneaking": false, "isBlocking": false,
                "isWeapDrawn": false, "isDead": false
            }
        })
    }
}

fn run_script(s: &mut Session, path: &str, settle_ms: u64) -> i32 {
    let Ok(file) = std::fs::File::open(path) else {
        emit(&json!({"event": "error", "error": "cannot open script"}));
        return 1;
    };
    let start = Instant::now();
    for line in std::io::BufReader::new(file).lines() {
        let Ok(line) = line else {
            emit(&json!({"event": "error", "error": "cannot read script"}));
            return 1;
        };
        if line.trim().is_empty() {
            continue;
        }
        let step: Value = match serde_json::from_str::<Value>(&line) {
            Ok(v) if v.get("send").is_some() || v.get("move").is_some() => v,
            _ => {
                emit(&json!({"event": "error", "error": "bad script line"}));
                return 1;
            }
        };
        let at = Duration::from_millis(step.get("at_ms").and_then(Value::as_u64).unwrap_or(0));
        while start.elapsed() < at {
            s.tick();
            if s.failure.is_some() {
                return 1;
            }
            std::thread::sleep(Duration::from_millis(1));
        }
        let reliable = step.get("reliable").and_then(Value::as_bool).unwrap_or(true);
        if let Some(m) = step.get("move") {
            let f = |k: &str| m.get(k).and_then(Value::as_f64).unwrap_or(0.0);
            let mode = m.get("runMode").and_then(Value::as_str).unwrap_or("Walking").to_string();
            let msg = s.movement(f("dx"), f("dy"), f("dz"), &mode);
            s.send(&msg, false);
            continue;
        }
        let raw = step.get("send").map(Value::to_string).unwrap_or_default();
        let raw = raw
            .replace("\"{{idx}}\"", &s.me.idx.unwrap_or(0).to_string())
            .replace("\"{{worldOrCell}}\"", &s.me.world_or_cell.to_string());
        match serde_json::from_str::<Value>(&raw) {
            Ok(msg) => s.send(&msg, reliable),
            Err(_) => {
                emit(&json!({"event": "error", "error": "bad send after substitution"}));
                return 1;
            }
        }
    }
    s.settle(settle_ms);
    i32::from(s.failure.is_some())
}

fn main() -> std::process::ExitCode {
    let args: Vec<String> = std::env::args().collect();
    let o = match parse_options(&args) {
        Ok(o) => o,
        Err(e) => {
            eprintln!("{e}");
            return std::process::ExitCode::from(2);
        }
    };
    let client = WireClient::connect(
        &o.host,
        o.port,
        ClientOptions {
            password: (!o.password.is_empty()).then(|| o.password.clone()),
            connect_timeout: Duration::from_millis(o.timeout_ms),
            ..ClientOptions::default()
        },
    );
    let mut s = Session {
        client,
        me: Me::default(),
        received: 0,
        failure: None,
        pending_snippets: Vec::new(),
    };
    let timeout = Duration::from_millis(o.timeout_ms);
    if !s.wait_for(timeout, |s| s.client.is_connected()) {
        if s.failure.is_none() {
            emit(&json!({"event": "error", "error": "connect timed out"}));
        }
        return std::process::ExitCode::from(1);
    }
    let login = json!({"customPacketType": "loginWithSkympIo", "gameData": {"profileId": o.profile_id}});
    s.send(&json!({"t": 1, "contentJsonDump": login.to_string()}), true);
    if !s.wait_for(timeout, |s| s.me.idx.is_some()) {
        emit(&json!({"event": "error", "error": "no CreateActor with isMe"}));
        return std::process::ExitCode::from(1);
    }
    let rc = match &o.script {
        Some(path) => run_script(&mut s, path, o.settle_ms),
        None => {
            for i in 1..=o.moves {
                let msg = s.movement(30.0 * f64::from(i), 0.0, 0.0, "Walking");
                s.send(&msg, false);
                s.settle(130);
            }
            let add = json!({"t": 12, "data": {"commandName": "AddItem", "args": [0x14, o.add_item_base, o.add_item_count]}});
            s.send(&add, true);
            s.settle(o.settle_ms);
            i32::from(s.failure.is_some())
        }
    };
    emit(&json!({"event": "done", "received": s.received, "rc": rc}));
    s.client.close();
    std::process::ExitCode::from(u8::try_from(rc).unwrap_or(1))
}
