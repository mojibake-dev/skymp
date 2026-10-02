//! difftest: the C++ core is the oracle for behavior we are not changing
//! (ADR-010). In M1 the comparison is between two whole stacks, run from the
//! same world (ADR-019):
//!
//! - legacy: the last RakNet server image and the C++ fakeclient;
//! - wire: the bridged server and the Rust fakeclient.
//!
//! A session (YAML under `sessions/`) is a list of timed steps per client,
//! each a fakeclient script line: `move` (an UpdateMovement at the spawn
//! plus an offset) or `send` (SkyMP JSON as the client's networking service
//! would send it). Each stack runs one fakeclient process per client, in
//! declared order, a fixed gap apart, and the clients' event lines are
//! normalized and diffed: whether each logged in and where it spawned, its
//! exit code, what it sent, and the multiset of messages it received. The
//! relays whose count depends on timing (UpdateMovement, UpdateAnimation,
//! UpdateAnimVariables by default) compare as "received at least one".
//! A session may declare divergences: a message one client receives a
//! different number of times on each stack, with the reason. Declarations
//! apply legacy against wire only and are reviewed like validator changes;
//! one that stops occurring is itself a difference, so it cannot outlive
//! its cause.
//!
//! Environment: `DIFFTEST_LEGACY_FAKECLIENT` (or the older
//! `DIFFTEST_FAKECLIENT`) and `DIFFTEST_LEGACY_ADDR` (default
//! 127.0.0.1:7777) name the legacy stack; `DIFFTEST_WIRE_FAKECLIENT` and
//! `DIFFTEST_WIRE_ADDR` (default 127.0.0.1:7778) the wire stack;
//! `DIFFTEST_PASSWORD` and `DIFFTEST_CLIENT_GAP_MS` (default 1500) apply to
//! both. With one stack named, it runs twice and diffs against itself, which
//! proves the session is deterministic; that is only meaningful if the
//! server's world is restored between the runs. Exit 0 identical, 1
//! different, 2 a driver failed.

use std::collections::{BTreeMap, BTreeSet};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::Duration;

use serde::Deserialize;
use serde_json::{json, Value};

/// One session.
#[derive(Debug, Deserialize)]
struct Session {
    id: String,
    /// Client name to its options, in run order.
    clients: serde_yaml::Mapping,
    /// MsgTypes compared as presence only.
    #[serde(default = "default_volatile")]
    volatile: Vec<u8>,
    steps: Vec<Step>,
    /// Expected differences between the legacy and the wire stack.
    #[serde(default)]
    divergences: Vec<Divergence>,
}

/// A message one client receives `legacy` times on the legacy stack and
/// `wire` times on the wire stack, for `reason`.
#[derive(Debug, Deserialize)]
struct Divergence {
    client: String,
    /// The message as received, compared in canonical form.
    msg: Value,
    legacy: i64,
    wire: i64,
    reason: String,
}

impl Divergence {
    fn key(&self) -> String {
        canonical(&self.msg).to_string()
    }
}

fn default_volatile() -> Vec<u8> {
    vec![2, 3, 24]
}

#[derive(Debug, Deserialize)]
struct ClientOpts {
    profile_id: i64,
}

/// One script line for one client.
#[derive(Debug, Deserialize)]
struct Step {
    client: String,
    #[serde(default)]
    at_ms: u64,
    #[serde(default)]
    r#move: Option<Value>,
    #[serde(default)]
    send: Option<Value>,
    #[serde(default)]
    reliable: Option<bool>,
}

#[derive(Debug, thiserror::Error)]
enum DiffError {
    #[error("session: {0}")]
    Session(String),
    #[error("io: {0}")]
    Io(String),
    #[error("{0} stack: {1}")]
    Driver(&'static str, String),
}

/// A stack: a fakeclient binary and the server it talks to.
#[derive(Debug, Clone)]
struct Stack {
    name: &'static str,
    fakeclient: PathBuf,
    host: String,
    port: u16,
    password: Option<String>,
    gap: Duration,
}

fn env_stack(name: &'static str, bin_vars: &[&str], addr_var: &str, default_addr: &str) -> Result<Option<Stack>, DiffError> {
    let Some(bin) = bin_vars.iter().find_map(std::env::var_os) else {
        return Ok(None);
    };
    let addr = std::env::var(addr_var).unwrap_or_else(|_| default_addr.to_string());
    let (host, port) = addr
        .rsplit_once(':')
        .and_then(|(h, p)| p.parse::<u16>().ok().map(|p| (h.to_string(), p)))
        .ok_or_else(|| DiffError::Session(format!("{addr_var}={addr} is not host:port")))?;
    let gap = std::env::var("DIFFTEST_CLIENT_GAP_MS")
        .ok()
        .and_then(|v| v.parse::<u64>().ok())
        .unwrap_or(1500);
    Ok(Some(Stack {
        name,
        fakeclient: PathBuf::from(bin),
        host,
        port,
        password: std::env::var("DIFFTEST_PASSWORD").ok().filter(|p| !p.is_empty()),
        gap: Duration::from_millis(gap),
    }))
}

fn load_session(path: &Path) -> Result<Session, DiffError> {
    let text = std::fs::read_to_string(path).map_err(|e| DiffError::Io(format!("{}: {e}", path.display())))?;
    let s: Session = serde_yaml::from_str(&text).map_err(|e| DiffError::Session(e.to_string()))?;
    for st in &s.steps {
        if !s.clients.contains_key(st.client.as_str()) {
            return Err(DiffError::Session(format!("step for unknown client {}", st.client)));
        }
        if st.r#move.is_some() == st.send.is_some() {
            return Err(DiffError::Session(format!("a step for {} needs exactly one of move, send", st.client)));
        }
    }
    for d in &s.divergences {
        if !s.clients.contains_key(d.client.as_str()) {
            return Err(DiffError::Session(format!("divergence for unknown client {}", d.client)));
        }
        if d.legacy == d.wire || d.legacy < 0 || d.wire < 0 {
            return Err(DiffError::Session(format!("divergence {} for {}: counts must differ and be >= 0", d.key(), d.client)));
        }
        if d.reason.trim().is_empty() {
            return Err(DiffError::Session(format!("divergence {} for {} has no reason", d.key(), d.client)));
        }
    }
    Ok(s)
}

fn clients(s: &Session) -> Result<Vec<(String, ClientOpts)>, DiffError> {
    s.clients
        .iter()
        .map(|(k, v)| {
            let name = k.as_str().ok_or_else(|| DiffError::Session("client names are strings".into()))?;
            let opts: ClientOpts = serde_yaml::from_value(v.clone()).map_err(|e| DiffError::Session(format!("{name}: {e}")))?;
            Ok((name.to_string(), opts))
        })
        .collect()
}

fn script(s: &Session, client: &str) -> String {
    let mut out = String::new();
    for st in s.steps.iter().filter(|st| st.client == client) {
        let mut line = serde_json::Map::new();
        line.insert("at_ms".into(), json!(st.at_ms));
        if let Some(m) = &st.r#move {
            line.insert("move".into(), m.clone());
        }
        if let Some(m) = &st.send {
            line.insert("send".into(), m.clone());
        }
        if let Some(r) = st.reliable {
            line.insert("reliable".into(), Value::Bool(r));
        }
        out.push_str(&Value::Object(line).to_string());
        out.push('\n');
    }
    out
}

/// Run every client of the session on one stack; event lines per client.
fn run_stack(stack: &Stack, s: &Session, tag: &str) -> Result<BTreeMap<String, (i32, Vec<Value>)>, DiffError> {
    let dir = std::env::temp_dir().join(format!("difftest-{}-{}-{tag}", std::process::id(), stack.name));
    std::fs::create_dir_all(&dir).map_err(|e| DiffError::Io(e.to_string()))?;
    let mut children = Vec::new();
    for (i, (name, opts)) in clients(s)?.into_iter().enumerate() {
        if i > 0 {
            std::thread::sleep(stack.gap);
        }
        let path = dir.join(format!("{name}.jsonl"));
        std::fs::write(&path, script(s, &name)).map_err(|e| DiffError::Io(e.to_string()))?;
        let mut cmd = Command::new(&stack.fakeclient);
        cmd.args(["--host", &stack.host, "--port", &stack.port.to_string(), "--profile-id", &opts.profile_id.to_string(), "--settle-ms", "2000"])
            .arg("--script")
            .arg(&path)
            .stdout(Stdio::piped())
            .stderr(Stdio::null());
        if let Some(p) = &stack.password {
            cmd.args(["--password", p]);
        }
        let child = cmd
            .spawn()
            .map_err(|e| DiffError::Driver(stack.name, format!("spawn {}: {e}", stack.fakeclient.display())))?;
        children.push((name, child));
    }
    let mut out = BTreeMap::new();
    for (name, child) in children {
        let output = child.wait_with_output().map_err(|e| DiffError::Driver(stack.name, e.to_string()))?;
        let events = String::from_utf8_lossy(&output.stdout)
            .lines()
            .filter_map(|l| serde_json::from_str(l).ok())
            .collect();
        out.insert(name, (output.status.code().unwrap_or(-1), events));
    }
    Ok(out)
}

/// Floats compare at the precision the game keeps (f32); everything else
/// as is, objects with sorted keys (serde_json's map is ordered).
fn canonical(v: &Value) -> Value {
    match v {
        Value::Number(n) if n.is_f64() => {
            let f = n.as_f64().unwrap_or(0.0);
            #[allow(clippy::as_conversions)] // the precision the engine keeps
            let f32v = f as f32;
            if f32v.fract() == 0.0 && f32v.abs() < 1.0e15 {
                #[allow(clippy::as_conversions)] // integral and in range
                let i = f32v as i64;
                json!(i)
            } else {
                serde_json::Number::from_f64(f64::from(f32v)).map_or(Value::Null, Value::Number)
            }
        }
        Value::Array(a) => Value::Array(a.iter().map(canonical).collect()),
        Value::Object(o) => Value::Object(o.iter().map(|(k, v)| (k.clone(), canonical(v))).collect()),
        other => other.clone(),
    }
}

/// What one client saw, in comparable form.
fn normalize(rc: i32, events: &[Value], volatile: &BTreeSet<i64>) -> Value {
    let kind = |e: &Value| e.get("event").and_then(Value::as_str).unwrap_or("").to_string();
    let actor = events
        .iter()
        .find(|e| kind(e) == "actor")
        .map(|e| canonical(&json!({"worldOrCell": e.get("worldOrCell"), "pos": e.get("pos")})));
    let mut received: Vec<String> = Vec::new();
    let mut relays: BTreeSet<i64> = BTreeSet::new();
    let mut sent: BTreeMap<i64, u64> = BTreeMap::new();
    let mut errors: Vec<String> = Vec::new();
    for e in events {
        let msg = e.get("msg").cloned().unwrap_or(Value::Null);
        let t = msg.get("t").and_then(Value::as_i64).unwrap_or(-1);
        match kind(e).as_str() {
            "message" if volatile.contains(&t) => {
                relays.insert(t);
            }
            "message" => received.push(canonical(&msg).to_string()),
            "sent" => {
                let n = sent.entry(t).or_insert(0);
                *n = n.saturating_add(1);
            }
            "error" => errors.push(e.get("error").and_then(Value::as_str).unwrap_or("").to_string()),
            _ => {}
        }
    }
    received.sort();
    json!({
        "rc": rc,
        "actor": actor,
        "sent": sent.iter().map(|(t, n)| (t.to_string(), json!(n))).collect::<serde_json::Map<String, Value>>(),
        "errors": errors,
        "relays_seen": relays.into_iter().collect::<Vec<_>>(),
        "received": received,
    })
}

fn normalized(stack: &Stack, s: &Session, tag: &str) -> Result<BTreeMap<String, Value>, DiffError> {
    let volatile: BTreeSet<i64> = s.volatile.iter().map(|t| i64::from(*t)).collect();
    Ok(run_stack(stack, s, tag)?
        .into_iter()
        .map(|(name, (rc, events))| (name, normalize(rc, &events, &volatile)))
        .collect())
}

/// What a comparison found: differences, and the declared divergences that
/// occurred as declared.
#[derive(Debug, Default)]
struct Outcome {
    differences: Vec<String>,
    declared: Vec<String>,
}

/// Every difference between two runs; `declared` (legacy against wire
/// only, `a` legacy and `b` wire) moves the expected ones aside.
fn diff(a_name: &str, a: &BTreeMap<String, Value>, b_name: &str, b: &BTreeMap<String, Value>, declared: &[Divergence]) -> Outcome {
    let mut out = Vec::new();
    let mut expected = Vec::new();
    let mut used = vec![false; declared.len()];
    let names: BTreeSet<&String> = a.keys().chain(b.keys()).collect();
    for name in names {
        let (x, y) = (a.get(name).unwrap_or(&Value::Null), b.get(name).unwrap_or(&Value::Null));
        for key in ["rc", "actor", "sent", "errors", "relays_seen"] {
            if x.get(key) != y.get(key) {
                out.push(format!("{name}.{key}: {a_name} {} | {b_name} {}", x.get(key).unwrap_or(&Value::Null), y.get(key).unwrap_or(&Value::Null)));
            }
        }
        let list = |v: &Value| -> BTreeMap<String, i64> {
            let mut m: BTreeMap<String, i64> = BTreeMap::new();
            for s in v.get("received").and_then(Value::as_array).into_iter().flatten().filter_map(Value::as_str) {
                let n = m.entry(s.to_string()).or_insert(0);
                *n = n.saturating_add(1);
            }
            m
        };
        let (lx, ly) = (list(x), list(y));
        let msgs: BTreeSet<&String> = lx.keys().chain(ly.keys()).collect();
        for msg in msgs {
            let (n, m) = (lx.get(msg).copied().unwrap_or(0), ly.get(msg).copied().unwrap_or(0));
            if n == m {
                continue;
            }
            let line = format!("{name}: {msg} x{n} on {a_name}, x{m} on {b_name}");
            match declared
                .iter()
                .position(|d| d.client == *name && d.key() == *msg && d.legacy == n && d.wire == m)
            {
                Some(i) => {
                    if let Some(u) = used.get_mut(i) {
                        *u = true;
                    }
                    let reason = declared.get(i).map_or("", |d| d.reason.trim());
                    expected.push(format!("{line} (declared: {reason})"));
                }
                None => out.push(line),
            }
        }
    }
    for (d, u) in declared.iter().zip(&used) {
        if !u {
            out.push(format!(
                "{}: declared divergence {} (x{} on {a_name}, x{} on {b_name}) did not occur as declared",
                d.client,
                d.key(),
                d.legacy,
                d.wire
            ));
        }
    }
    Outcome { differences: out, declared: expected }
}

fn run(path: &Path) -> Result<(bool, String), DiffError> {
    let s = load_session(path)?;
    let legacy = env_stack("legacy", &["DIFFTEST_LEGACY_FAKECLIENT", "DIFFTEST_FAKECLIENT"], "DIFFTEST_LEGACY_ADDR", "127.0.0.1:7777")?;
    let wire = env_stack("wire", &["DIFFTEST_WIRE_FAKECLIENT"], "DIFFTEST_WIRE_ADDR", "127.0.0.1:7778")?;
    let (a, b, what, names, declared) = match (legacy, wire) {
        (Some(l), Some(w)) => (
            normalized(&l, &s, "a")?,
            normalized(&w, &s, "a")?,
            "legacy against wire".to_string(),
            ("legacy", "wire"),
            s.divergences.as_slice(),
        ),
        (Some(one), None) | (None, Some(one)) => (
            normalized(&one, &s, "a")?,
            normalized(&one, &s, "b")?,
            format!("{} against itself (determinism)", one.name),
            ("first", "second"),
            &[][..],
        ),
        (None, None) => {
            return Err(DiffError::Session(
                "no stack: set DIFFTEST_LEGACY_FAKECLIENT and/or DIFFTEST_WIRE_FAKECLIENT".into(),
            ))
        }
    };
    let o = diff(names.0, &a, names.1, &b, declared);
    let mut verdict = if o.differences.is_empty() {
        format!("session {}: {what}: identical ({} clients, {} declared divergences)", s.id, a.len(), o.declared.len())
    } else {
        format!("session {}: {what}: {} differences\n{}", s.id, o.differences.len(), o.differences.join("\n"))
    };
    for line in &o.declared {
        verdict.push('\n');
        verdict.push_str(line);
    }
    Ok((o.differences.is_empty(), verdict))
}

fn main() -> std::process::ExitCode {
    let Some(path) = std::env::args().nth(1) else {
        eprintln!("usage: difftest <session.yaml>");
        return std::process::ExitCode::from(2);
    };
    match run(Path::new(&path)) {
        Ok((same, verdict)) => {
            println!("{verdict}");
            std::process::ExitCode::from(if same { 0 } else { 1 })
        }
        Err(e) => {
            eprintln!("difftest: {e}");
            std::process::ExitCode::from(2)
        }
    }
}

#[cfg(test)]
#[allow(clippy::panic, clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use super::*;

    fn smoke_path() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("sessions/smoke.yaml")
    }

    fn stub() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tools/fakeclient-stub.py")
    }

    fn stub_available() -> bool {
        Command::new("python3").arg("--version").output().map(|o| o.status.success()).unwrap_or(false)
    }

    fn stack(bin: PathBuf) -> Stack {
        Stack { name: "stub", fakeclient: bin, host: "127.0.0.1".into(), port: 1, password: None, gap: Duration::from_millis(10) }
    }

    #[test]
    fn smoke_session_parses_and_scripts() {
        let s = load_session(&smoke_path()).expect("parse");
        assert_eq!(s.id, "smoke");
        let names: Vec<String> = clients(&s).expect("clients").into_iter().map(|(n, _)| n).collect();
        assert_eq!(names, vec!["c1".to_string(), "c2".to_string()]);
        let text = script(&s, "c1");
        assert!(text.lines().count() >= 3, "{text}");
        for line in text.lines() {
            let v: Value = serde_json::from_str(line).expect("json line");
            assert!(v.get("move").is_some() || v.get("send").is_some());
        }
    }

    #[test]
    fn floats_compare_at_f32_precision() {
        assert_eq!(canonical(&json!(0.1)), canonical(&json!(0.100_000_001_490_116_12)));
        assert_eq!(canonical(&json!(133_857.0)), json!(133_857));
        assert_ne!(canonical(&json!(0.1)), canonical(&json!(0.2)));
    }

    #[test]
    fn stub_stacks_agree_and_a_changed_stub_does_not() {
        if !stub_available() {
            return;
        }
        let s = load_session(&smoke_path()).expect("parse");
        let a = normalized(&stack(stub()), &s, "t1").expect("run");
        let b = normalized(&stack(stub()), &s, "t2").expect("run");
        let same = diff("a", &a, "b", &b, &[]);
        assert!(same.differences.is_empty(), "{:?}", same.differences);
        assert_eq!(a["c1"]["rc"], 0);
        assert!(a["c1"]["actor"].is_object());
        // a stack that relays nothing and answers AddItem with an extra message
        std::env::set_var("FAKECLIENT_STUB_VARIANT", "1");
        let c = normalized(&stack(stub()), &s, "t3").expect("run");
        std::env::remove_var("FAKECLIENT_STUB_VARIANT");
        let lines = diff("a", &a, "c", &c, &[]).differences;
        assert!(!lines.is_empty());
        assert!(lines.iter().any(|l| l.contains("relays_seen")), "{lines:?}");
    }

    fn one_client(received: &[&str]) -> BTreeMap<String, Value> {
        let mut m = BTreeMap::new();
        m.insert("c2".to_string(), json!({"rc": 0, "received": received}));
        m
    }

    fn destroy_c1(legacy: i64, wire: i64) -> Divergence {
        Divergence { client: "c2".into(), msg: json!({"t": 25, "idx": 2}), legacy, wire, reason: "departure detection".into() }
    }

    #[test]
    fn a_declared_divergence_is_set_aside_and_must_occur() {
        let legacy = one_client(&[]);
        let wire = one_client(&[r#"{"idx":2,"t":25}"#]);
        // undeclared: a difference
        let o = diff("legacy", &legacy, "wire", &wire, &[]);
        assert_eq!(o.differences.len(), 1, "{:?}", o.differences);
        // declared with the observed counts: set aside, with its reason
        let o = diff("legacy", &legacy, "wire", &wire, &[destroy_c1(0, 1)]);
        assert!(o.differences.is_empty(), "{:?}", o.differences);
        assert_eq!(o.declared.len(), 1);
        assert!(o.declared[0].contains("departure detection"), "{:?}", o.declared);
        // declared, but the stacks agree now: the stale declaration fails
        let o = diff("legacy", &wire, "wire", &wire, &[destroy_c1(0, 1)]);
        assert_eq!(o.differences.len(), 1, "{:?}", o.differences);
        assert!(o.differences[0].contains("did not occur"), "{:?}", o.differences);
        // declared with other counts: both the difference and the stale declaration
        let o = diff("legacy", &legacy, "wire", &wire, &[destroy_c1(0, 2)]);
        assert_eq!(o.differences.len(), 2, "{:?}", o.differences);
    }

    #[test]
    fn divergence_declarations_are_checked_at_load() {
        let dir = std::env::temp_dir().join(format!("difftest-load-{}", std::process::id()));
        std::fs::create_dir_all(&dir).expect("dir");
        let base = "id: t\nclients:\n  c1: {profile_id: 1}\nsteps:\n  - {client: c1, move: {dx: 1}}\n";
        for (bad, why) in [
            ("divergences:\n  - {client: c9, msg: {t: 25}, legacy: 0, wire: 1, reason: r}\n", "unknown client"),
            ("divergences:\n  - {client: c1, msg: {t: 25}, legacy: 1, wire: 1, reason: r}\n", "counts must differ"),
            ("divergences:\n  - {client: c1, msg: {t: 25}, legacy: 0, wire: 1, reason: ' '}\n", "no reason"),
        ] {
            let path = dir.join("s.yaml");
            std::fs::write(&path, format!("{base}{bad}")).expect("write");
            let err = load_session(&path).expect_err(why).to_string();
            assert!(err.contains(why), "{err}");
        }
    }
}
