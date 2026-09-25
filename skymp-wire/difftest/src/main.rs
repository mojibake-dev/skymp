//! difftest: the C++ core is the oracle for behavior we are not changing.
//!
//! A session is a timed list of typed messages per client (YAML under
//! `difftest/sessions/`, each step an externally tagged `wire_schema::Message`).
//! It is replayed into two stacks through their own transports and the
//! outputs are normalized to one JSON shape and diffed:
//!
//! The wire stack is netcode into the Rust edge; in M0 the edge is an
//! in-process recorder (a real `wire_transport::Server` with no core behind
//! it), so the outputs are per-step outcomes, accepted or rejected. The legacy
//! stack is RakNet into the unmodified C++ server through the `fakeclient`
//! binary the fork will carry (Track W step 8); it is not available yet.
//!
//! Normalized shape: `{"steps": [{"i", "client", "message", "outcome"}],
//! "server_to_client": {client: [message names]}, "db": value}`. The wire
//! driver also records `reason` per rejected step; it is outside the diff
//! because the legacy stack has no reason codes. A step may declare a
//! divergence (`divergence: {legacy: accept}`) when the C++ server is known
//! to accept what the validator rejects; declared divergences are applied
//! before the diff and reviewed like validator changes.
//!
//! Without a legacy driver, `difftest` replays the session twice through the
//! wire driver and diffs the two runs (self-diff), which proves the harness
//! and the edge are deterministic; the exit code and the printed verdict say
//! which comparison ran.

use std::collections::{BTreeMap, HashMap};
use std::net::SocketAddr;
use std::path::PathBuf;
use std::time::Duration;

use serde::Deserialize;
use wire_schema::Message;
use wire_transport::limits::Limits;
use wire_transport::token::{Auth, ConnectToken};
use wire_transport::{Client, Inbound, Server};

/// One recorded session.
#[derive(Debug, Deserialize)]
struct Session {
    id: String,
    clients: Vec<String>,
    steps: Vec<Step>,
}

/// One timed message from one client, plus an optional declared divergence.
#[derive(Debug, Deserialize)]
struct Step {
    /// Milliseconds since session start.
    at_ms: u64,
    client: String,
    #[serde(default)]
    divergence: Option<Divergence>,
    /// The message, externally tagged: `Movement: {...}`.
    #[serde(flatten)]
    message: serde_json::Value,
}

/// Where the two stacks are allowed to differ on a step.
#[derive(Debug, Deserialize, Clone, Copy)]
struct Divergence {
    /// What the legacy stack does with this step where the wire rejects it.
    legacy: Outcome,
}

/// What a stack did with a step.
#[derive(Debug, Deserialize, Clone, Copy, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
enum Outcome {
    Accept,
    Reject,
}

impl Outcome {
    fn as_str(self) -> &'static str {
        match self {
            Outcome::Accept => "accepted",
            Outcome::Reject => "rejected",
        }
    }
}

#[derive(Debug, thiserror::Error)]
enum DiffError {
    #[error("E_DIFF_IO: {0}")]
    Io(String),
    #[error("E_DIFF_SESSION: step {0}: {1}")]
    Session(usize, String),
    #[error("E_DIFF_DRIVER({0}): {1}")]
    Driver(&'static str, String),
    #[error("E_DIFF_MISMATCH: {0}")]
    Mismatch(String),
}

/// A stack under test.
trait Driver {
    fn name(&self) -> &'static str;
    fn start(&mut self) -> Result<(), DiffError>;
    fn connect(&mut self, client: &str) -> Result<(), DiffError>;
    fn send(
        &mut self,
        index: usize,
        client: &str,
        at_ms: u64,
        message: &Message,
    ) -> Result<(), DiffError>;
    /// Everything the server did, normalized.
    fn outputs(&mut self) -> Result<serde_json::Value, DiffError>;
    fn stop(&mut self) -> Result<(), DiffError>;
}

/// Legacy driver: speaks RakNet to the unmodified C++ server through the
/// fork's `fakeclient`. Lands with Track W step 8; until then it reports
/// itself unavailable and the CLI self-diffs.
struct Legacy;

impl Driver for Legacy {
    fn name(&self) -> &'static str {
        "legacy"
    }
    fn start(&mut self) -> Result<(), DiffError> {
        Err(DiffError::Driver(
            "legacy",
            "fakeclient not available yet (Track W step 8)".into(),
        ))
    }
    fn connect(&mut self, _c: &str) -> Result<(), DiffError> {
        Ok(())
    }
    fn send(&mut self, _i: usize, _c: &str, _at: u64, _m: &Message) -> Result<(), DiffError> {
        Ok(())
    }
    fn outputs(&mut self) -> Result<serde_json::Value, DiffError> {
        Ok(serde_json::Value::Null)
    }
    fn stop(&mut self) -> Result<(), DiffError> {
        Ok(())
    }
}

#[derive(Debug, Clone, serde::Serialize)]
struct StepRecord {
    i: usize,
    client: String,
    message: &'static str,
    outcome: &'static str,
    #[serde(skip_serializing_if = "Option::is_none")]
    reason: Option<String>,
}

/// Wire driver: real `wire_transport::Client`s over loopback UDP into an
/// in-process `wire_transport::Server` that records what the edge decided.
struct Wire {
    server: Option<Server>,
    clients: HashMap<String, Client>,
    ids: HashMap<String, u64>,
    steps: Vec<StepRecord>,
    to_client: BTreeMap<String, Vec<&'static str>>,
    now_ms: u64,
    events: Vec<Inbound>,
}

impl Wire {
    fn new() -> Self {
        Self {
            server: None,
            clients: HashMap::new(),
            ids: HashMap::new(),
            steps: Vec::new(),
            to_client: BTreeMap::new(),
            now_ms: 0,
            events: Vec::new(),
        }
    }

    /// One tick of every client and the server, `dt` of transport time.
    fn pump(&mut self, dt: Duration) -> Result<(), DiffError> {
        let server = self
            .server
            .as_mut()
            .ok_or_else(|| DiffError::Driver("wire", "not started".into()))?;
        for (name, client) in self.clients.iter_mut() {
            let mut inbox = Vec::new();
            client.poll(dt, &mut inbox);
            let list = self.to_client.entry(name.clone()).or_default();
            list.extend(inbox.iter().map(Message::name));
        }
        server.poll(dt, &mut self.events);
        Ok(())
    }

    fn pump_until(
        &mut self,
        budget_ms: u64,
        mut done: impl FnMut(&[Inbound]) -> bool,
    ) -> Result<bool, DiffError> {
        let step = Duration::from_millis(5);
        let mut spent: u64 = 0;
        while spent < budget_ms {
            self.pump(step)?;
            if done(&self.events) {
                return Ok(true);
            }
            std::thread::sleep(step);
            spent = spent.saturating_add(5);
        }
        Ok(false)
    }

    fn client_name(&self, id: u64) -> Option<&str> {
        self.ids
            .iter()
            .find(|(_, v)| **v == id)
            .map(|(k, _)| k.as_str())
    }
}

impl Driver for Wire {
    fn name(&self) -> &'static str {
        "wire"
    }

    fn start(&mut self) -> Result<(), DiffError> {
        let addr: SocketAddr = "127.0.0.1:0"
            .parse()
            .map_err(|_| DiffError::Driver("wire", "bad literal".into()))?;
        let server = Server::bind(addr, Limits::default(), Auth::Unsecure)
            .map_err(|e| DiffError::Driver("wire", e.to_string()))?;
        self.server = Some(server);
        Ok(())
    }

    fn connect(&mut self, client: &str) -> Result<(), DiffError> {
        let addr = self
            .server
            .as_ref()
            .ok_or_else(|| DiffError::Driver("wire", "not started".into()))?
            .local_addr();
        let c = Client::connect(addr, ConnectToken(Vec::new()))
            .map_err(|e| DiffError::Driver("wire", e.to_string()))?;
        self.clients.insert(client.to_string(), c);
        let known: Vec<u64> = self.ids.values().copied().collect();
        let connected = self.pump_until(5_000, |ev| {
            ev.iter()
                .any(|e| matches!(e, Inbound::Connected { client } if !known.contains(client)))
        })?;
        if !connected {
            return Err(DiffError::Driver(
                "wire",
                format!("{client}: handshake timed out"),
            ));
        }
        let id = self
            .events
            .iter()
            .filter_map(|e| match e {
                Inbound::Connected { client } if !known.contains(client) => Some(*client),
                _ => None,
            })
            .next()
            .ok_or_else(|| DiffError::Driver("wire", "connected event vanished".into()))?;
        self.ids.insert(client.to_string(), id);
        self.events.clear();
        Ok(())
    }

    fn send(
        &mut self,
        index: usize,
        client: &str,
        at_ms: u64,
        message: &Message,
    ) -> Result<(), DiffError> {
        // Advance transport time to the step's timestamp before sending, so
        // rate limits and sequence windows see the session's own clock.
        let gap = at_ms.saturating_sub(self.now_ms);
        if gap > 0 {
            self.pump(Duration::from_millis(gap))?;
            self.now_ms = at_ms;
        }
        let id = *self
            .ids
            .get(client)
            .ok_or_else(|| DiffError::Driver("wire", format!("{client}: not connected")))?;
        let c = self
            .clients
            .get_mut(client)
            .ok_or_else(|| DiffError::Driver("wire", format!("{client}: unknown")))?;
        c.send(message)
            .map_err(|e| DiffError::Driver("wire", e.to_string()))?;
        let arrived = self.pump_until(2_000, |ev| ev.iter().any(|e| matches!(e, Inbound::Message { client, .. } | Inbound::Rejected { client, .. } if *client == id)))?;
        if !arrived {
            return Err(DiffError::Driver(
                "wire",
                format!("step {index}: no outcome from the edge within 2 s"),
            ));
        }
        let mut record = StepRecord {
            i: index,
            client: client.to_string(),
            message: message.name(),
            outcome: "lost",
            reason: None,
        };
        let mut keep = Vec::new();
        for ev in self.events.drain(..) {
            match ev {
                Inbound::Message { client: c, msg } if c == id && record.outcome == "lost" => {
                    record.message = msg.name();
                    record.outcome = Outcome::Accept.as_str();
                }
                Inbound::Rejected { client: c, reject } if c == id && record.outcome == "lost" => {
                    record.outcome = Outcome::Reject.as_str();
                    record.reason = Some(reject.to_string());
                }
                other => keep.push(other),
            }
        }
        self.events = keep;
        self.steps.push(record);
        Ok(())
    }

    fn outputs(&mut self) -> Result<serde_json::Value, DiffError> {
        // Anything the server still holds for a client, then normalize.
        self.pump(Duration::from_millis(50))?;
        let unknown: Vec<String> = self
            .events
            .iter()
            .filter_map(|e| match e {
                Inbound::Message { client, .. } | Inbound::Rejected { client, .. } => {
                    self.client_name(*client).map(str::to_string)
                }
                _ => None,
            })
            .collect();
        if !unknown.is_empty() {
            return Err(DiffError::Driver(
                "wire",
                format!("unattributed outcomes for {unknown:?}"),
            ));
        }
        Ok(serde_json::json!({
            "steps": self.steps,
            "server_to_client": self.to_client,
            "db": serde_json::Value::Null,
        }))
    }

    fn stop(&mut self) -> Result<(), DiffError> {
        for c in self.clients.values_mut() {
            c.disconnect();
        }
        self.pump(Duration::from_millis(20))?;
        self.clients.clear();
        self.server = None;
        Ok(())
    }
}

fn load_session(path: &PathBuf) -> Result<Session, DiffError> {
    let text = std::fs::read_to_string(path).map_err(|e| DiffError::Io(e.to_string()))?;
    serde_yaml::from_str(&text).map_err(|e| DiffError::Io(e.to_string()))
}

fn parse_message(index: usize, step: &Step) -> Result<Message, DiffError> {
    serde_json::from_value::<Message>(step.message.clone())
        .map_err(|e| DiffError::Session(index, e.to_string()))
}

fn replay(d: &mut dyn Driver, s: &Session) -> Result<serde_json::Value, DiffError> {
    d.start()?;
    for c in &s.clients {
        d.connect(c)?;
    }
    for (i, st) in s.steps.iter().enumerate() {
        let msg = parse_message(i, st)?;
        d.send(i, &st.client, st.at_ms, &msg)?;
    }
    let out = d.outputs()?;
    d.stop()?;
    Ok(out)
}

/// The part of an output the diff looks at: outcomes without wire-only
/// detail, with declared divergences applied to the legacy side.
fn comparable(out: &serde_json::Value, session: &Session, legacy: bool) -> serde_json::Value {
    let mut v = out.clone();
    if let Some(steps) = v.get_mut("steps").and_then(|s| s.as_array_mut()) {
        for st in steps.iter_mut() {
            if let Some(obj) = st.as_object_mut() {
                obj.remove("reason");
                if legacy {
                    let i = obj
                        .get("i")
                        .and_then(|i| i.as_u64())
                        .and_then(|i| usize::try_from(i).ok());
                    if let Some(div) = i
                        .and_then(|i| session.steps.get(i))
                        .and_then(|s| s.divergence)
                    {
                        obj.insert(
                            "outcome".into(),
                            serde_json::Value::String(div.legacy.as_str().into()),
                        );
                    }
                }
            }
        }
    }
    v
}

fn run(path: PathBuf) -> Result<String, DiffError> {
    let session = load_session(&path)?;
    let mut wire = Wire::new();
    let a = replay(&mut wire, &session)?;
    let mut legacy = Legacy;
    match legacy.start() {
        Ok(()) => {
            let b = replay(&mut legacy, &session)?;
            if comparable(&a, &session, false) != comparable(&b, &session, true) {
                return Err(DiffError::Mismatch(format!(
                    "session {}: {} != {}",
                    session.id,
                    wire.name(),
                    legacy.name()
                )));
            }
            Ok(format!(
                "difftest {}: identical ({} vs {})",
                session.id,
                wire.name(),
                legacy.name()
            ))
        }
        Err(DiffError::Driver(_, why)) => {
            let mut again = Wire::new();
            let b = replay(&mut again, &session)?;
            if comparable(&a, &session, false) != comparable(&b, &session, false) {
                return Err(DiffError::Mismatch(format!(
                    "session {}: {} run 1 != run 2 (nondeterministic edge)",
                    session.id,
                    wire.name()
                )));
            }
            Ok(format!(
                "difftest {}: identical ({} self-diff; {} driver: {why})",
                session.id,
                wire.name(),
                legacy.name()
            ))
        }
        Err(e) => Err(e),
    }
}

fn main() -> std::process::ExitCode {
    let Some(path) = std::env::args().nth(1) else {
        eprintln!("usage: difftest <session.yaml>");
        return std::process::ExitCode::from(2);
    };
    match run(PathBuf::from(path)) {
        Ok(verdict) => {
            println!("{verdict}");
            std::process::ExitCode::SUCCESS
        }
        Err(e) => {
            eprintln!("{e}");
            std::process::ExitCode::from(1)
        }
    }
}

#[cfg(test)]
#[allow(clippy::expect_used)]
mod tests {
    use super::*;

    fn smoke_path() -> PathBuf {
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("sessions/smoke.yaml")
    }

    #[test]
    fn smoke_session_parses_into_wire_messages() {
        let s = load_session(&smoke_path()).expect("session");
        assert_eq!(s.clients, vec!["c1", "c2"]);
        let names: Vec<&str> = s
            .steps
            .iter()
            .enumerate()
            .map(|(i, st)| parse_message(i, st).expect("message").name())
            .collect();
        assert_eq!(
            names,
            vec!["Hello", "Hello", "Movement", "Movement", "Movement", "Hit"]
        );
    }

    #[test]
    fn wire_driver_records_the_replay_and_the_reason() {
        let s = load_session(&smoke_path()).expect("session");
        let mut wire = Wire::new();
        let out = replay(&mut wire, &s).expect("replay");
        let steps = out.get("steps").and_then(|v| v.as_array()).expect("steps");
        assert_eq!(steps.len(), s.steps.len());
        let outcomes: Vec<&str> = steps
            .iter()
            .filter_map(|st| st.get("outcome").and_then(|o| o.as_str()))
            .collect();
        assert_eq!(
            outcomes,
            vec!["accepted", "accepted", "accepted", "accepted", "rejected", "accepted"]
        );
        let reason = steps
            .get(4)
            .and_then(|st| st.get("reason"))
            .and_then(|r| r.as_str())
            .expect("reason on the replayed seq");
        assert_eq!(reason, "E_VAL_SEQ_REPLAY");
    }

    #[test]
    fn self_diff_is_identical_and_divergences_apply_to_legacy_only() {
        let verdict = run(smoke_path()).expect("run");
        assert!(verdict.contains("identical (wire self-diff"), "{verdict}");
        let s = load_session(&smoke_path()).expect("session");
        let out = serde_json::json!({ "steps": [ { "i": 4, "client": "c1", "message": "Movement", "outcome": "rejected", "reason": "E_VAL_SEQ_REPLAY" } ], "server_to_client": {}, "db": null });
        let wire_side = comparable(&out, &s, false);
        let legacy_side = comparable(&out, &s, true);
        assert_eq!(
            wire_side
                .pointer("/steps/0/outcome")
                .and_then(|v| v.as_str()),
            Some("rejected")
        );
        assert!(wire_side.pointer("/steps/0/reason").is_none());
        // smoke.yaml declares that the legacy server accepts the replayed seq.
        assert_eq!(
            legacy_side
                .pointer("/steps/0/outcome")
                .and_then(|v| v.as_str()),
            Some("accepted")
        );
    }
}
