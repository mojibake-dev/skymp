//! difftest: the C++ core is the oracle for behavior we are not changing.
//!
//! A session is a timed list of typed messages per client. It is replayed
//! into two stacks through their own transports:
//!   legacy: RakNet -> unmodified C++ server (driver spawns the existing
//!           client library or a RakNet stub built from the fork)
//!   wire:   netcode -> Rust edge fronting the same core
//! Each stack's outputs (message stream per client, DB dump at the end) are
//! normalized to canonical JSON and diffed. Exit code 0 means identical.
//!
//! Skeleton: the drivers are traits with one stub each; M0 fills them.

use std::path::PathBuf;

use serde::Deserialize;

#[derive(Debug, Deserialize)]
struct Session {
    id: String,
    clients: Vec<String>,
    steps: Vec<Step>,
}

#[derive(Debug, Deserialize)]
struct Step {
    /// Milliseconds since session start.
    at_ms: u64,
    client: String,
    /// One of the wire-schema message names, with fields.
    #[serde(flatten)]
    message: serde_json::Value,
}

/// A stack under test.
trait Driver {
    fn name(&self) -> &'static str;
    fn start(&mut self) -> Result<(), DiffError>;
    fn connect(&mut self, client: &str) -> Result<(), DiffError>;
    fn send(
        &mut self,
        client: &str,
        at_ms: u64,
        message: &serde_json::Value,
    ) -> Result<(), DiffError>;
    /// Drain everything the server sent, normalized.
    fn outputs(&mut self) -> Result<serde_json::Value, DiffError>;
    fn stop(&mut self) -> Result<(), DiffError>;
}

#[derive(Debug, thiserror::Error)]
enum DiffError {
    #[error("E_DIFF_IO: {0}")]
    Io(String),
    #[error("E_DIFF_DRIVER({0}): {1}")]
    Driver(&'static str, String),
    #[error("E_DIFF_MISMATCH: {0}")]
    Mismatch(String),
}

/// Legacy driver: speaks RakNet to the unmodified C++ server.
struct Legacy;
impl Driver for Legacy {
    fn name(&self) -> &'static str {
        "legacy"
    }
    fn start(&mut self) -> Result<(), DiffError> {
        Err(DiffError::Driver(
            "legacy",
            "TODO(M0): spawn C++ server + RakNet stub client".into(),
        ))
    }
    fn connect(&mut self, _c: &str) -> Result<(), DiffError> {
        Ok(())
    }
    fn send(&mut self, _c: &str, _at: u64, _m: &serde_json::Value) -> Result<(), DiffError> {
        Ok(())
    }
    fn outputs(&mut self) -> Result<serde_json::Value, DiffError> {
        Ok(serde_json::Value::Null)
    }
    fn stop(&mut self) -> Result<(), DiffError> {
        Ok(())
    }
}

/// Wire driver: speaks netcode to the Rust edge through wire-transport.
struct Wire;
impl Driver for Wire {
    fn name(&self) -> &'static str {
        "wire"
    }
    fn start(&mut self) -> Result<(), DiffError> {
        Err(DiffError::Driver(
            "wire",
            "TODO(M0): spawn bridged server + wire_transport::Client per session client".into(),
        ))
    }
    fn connect(&mut self, _c: &str) -> Result<(), DiffError> {
        Ok(())
    }
    fn send(&mut self, _c: &str, _at: u64, _m: &serde_json::Value) -> Result<(), DiffError> {
        Ok(())
    }
    fn outputs(&mut self) -> Result<serde_json::Value, DiffError> {
        Ok(serde_json::Value::Null)
    }
    fn stop(&mut self) -> Result<(), DiffError> {
        Ok(())
    }
}

fn replay(d: &mut dyn Driver, s: &Session) -> Result<serde_json::Value, DiffError> {
    d.start()?;
    for c in &s.clients {
        d.connect(c)?;
    }
    for st in &s.steps {
        d.send(&st.client, st.at_ms, &st.message)?;
    }
    let out = d.outputs()?;
    d.stop()?;
    Ok(out)
}

fn run(path: PathBuf) -> Result<(), DiffError> {
    let text = std::fs::read_to_string(&path).map_err(|e| DiffError::Io(e.to_string()))?;
    let session: Session = serde_yaml::from_str(&text).map_err(|e| DiffError::Io(e.to_string()))?;
    let mut legacy = Legacy;
    let mut wire = Wire;
    let a = replay(&mut legacy, &session)?;
    let b = replay(&mut wire, &session)?;
    if a != b {
        return Err(DiffError::Mismatch(format!(
            "session {}: {} != {}",
            session.id,
            legacy.name(),
            wire.name()
        )));
    }
    println!("difftest {}: identical", session.id);
    Ok(())
}

fn main() -> std::process::ExitCode {
    let Some(path) = std::env::args().nth(1) else {
        eprintln!("usage: difftest <session.yaml>");
        return std::process::ExitCode::from(2);
    };
    match run(PathBuf::from(path)) {
        Ok(()) => std::process::ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            std::process::ExitCode::from(1)
        }
    }
}
