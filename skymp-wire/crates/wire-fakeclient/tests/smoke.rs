//! The fakeclient binary against a scripted server built on the bridge the
//! C++ core uses: login, our actor, five moves, the AddItem command, and an
//! SpSnippet answered. The event lines are what lab-api reads.
#![allow(clippy::panic, clippy::expect_used, clippy::indexing_slicing)]

use std::io::{BufRead, BufReader};
use std::process::{Command, Stdio};
use std::sync::mpsc;
use std::time::{Duration, Instant};

use serde_json::Value;
use wire_bridge::{wire_server_bind, BindOptions, EventKind};

#[test]
fn smoke_against_a_scripted_server() {
    let mut server = wire_server_bind(&BindOptions {
        listen_host: "127.0.0.1".into(),
        port: 0,
        max_clients: 8,
        password: "lab".into(),
    })
    .expect("bind");
    let port = server.local_addr().port();
    let mut child = Command::new(env!("CARGO_BIN_EXE_fakeclient"))
        .args(["--port", &port.to_string(), "--password", "lab", "--profile-id", "7", "--settle-ms", "800"])
        .stdout(Stdio::piped())
        .spawn()
        .expect("spawn");
    let stdout = child.stdout.take().expect("stdout");
    let (tx, rx) = mpsc::channel::<String>();
    std::thread::spawn(move || {
        for line in BufReader::new(stdout).lines().map_while(Result::ok) {
            let _ = tx.send(line);
        }
    });

    let mut client = None;
    let mut got = Vec::new();
    let mut lines = Vec::new();
    let mut snippet_sent = false;
    let start = Instant::now();
    while start.elapsed() < Duration::from_secs(30) {
        let mut events = Vec::new();
        server.poll(&mut events);
        for ev in events {
            match ev.kind {
                EventKind::Connected => client = Some(ev.client),
                EventKind::Message => {
                    let id = client.expect("connected first");
                    let msg: Value = serde_json::from_str(&ev.json).expect("json");
                    if ev.msg_type == 1 {
                        // the login: profile 7, then our actor and a snippet to answer
                        let content: Value = serde_json::from_str(msg["contentJsonDump"].as_str().expect("str")).expect("json");
                        assert_eq!(content["gameData"]["profileId"], 7);
                        let create = r#"{"t":33,"idx":4,"isMe":true,"transform":{"worldOrCell":60,"pos":[100,200,300],"rot":[0,0,90]},"props":{},"customPropsJsonDumps":[]}"#;
                        assert_eq!(server.send(id, create, true), 0);
                        let snippet = r#"{"t":30,"class":"Debug","function":"Notification","arguments":["hi"],"selfId":0,"snippetIdx":11}"#;
                        assert_eq!(server.send(id, snippet, true), 0);
                        snippet_sent = true;
                    }
                    got.push(msg);
                }
                _ => {}
            }
        }
        while let Ok(line) = rx.try_recv() {
            lines.push(line);
        }
        if lines.iter().any(|l| l.contains("\"done\"")) {
            break;
        }
        std::thread::sleep(Duration::from_millis(5));
    }
    let status = child.wait().expect("wait");
    while let Ok(line) = rx.try_recv() {
        lines.push(line);
    }
    assert!(status.success(), "{lines:#?}");
    assert!(snippet_sent);
    let events: Vec<Value> = lines.iter().map(|l| serde_json::from_str(l).expect("event line")).collect();
    let kinds: Vec<&str> = events.iter().filter_map(|e| e["event"].as_str()).collect();
    assert_eq!(kinds.first(), Some(&"connected"), "{kinds:?}");
    let actor = events.iter().find(|e| e["event"] == "actor").expect("actor event");
    assert_eq!(actor["idx"], 4);
    assert_eq!(actor["worldOrCell"], 60);
    assert_eq!(actor["pos"][1], 200.0);
    assert_eq!(events.last().map(|e| e["rc"].clone()), Some(Value::from(0)));

    // what the server got: login, five moves from the actor's spot, the
    // AddItem command, and the snippet's answer
    let moves: Vec<&Value> = got.iter().filter(|m| m["t"] == 2).collect();
    assert_eq!(moves.len(), 5, "{got:#?}");
    assert_eq!(moves[0]["idx"], 4);
    assert_eq!(moves[0]["data"]["pos"][0], 130.0);
    assert_eq!(moves[4]["data"]["pos"][0], 250.0);
    let add = got.iter().find(|m| m["t"] == 12).expect("AddItem");
    assert_eq!(add["data"]["commandName"], "AddItem");
    assert_eq!(add["data"]["args"][1], 0x12EB7);
    let fin = got.iter().find(|m| m["t"] == 10).expect("FinishSpSnippet");
    assert_eq!(fin["snippetIdx"], 11);
    assert!(fin.get("returnValue").is_none(), "a null result is an absent key: {fin}");
}

#[test]
fn bad_options_exit_2_and_no_server_exits_1() {
    let s = Command::new(env!("CARGO_BIN_EXE_fakeclient")).args(["--bogus"]).status().expect("run");
    assert_eq!(s.code(), Some(2));
    let s = Command::new(env!("CARGO_BIN_EXE_fakeclient"))
        .args(["--port", "9", "--timeout-ms", "500"])
        .stdout(Stdio::null())
        .status()
        .expect("run");
    assert_eq!(s.code(), Some(1));
}
