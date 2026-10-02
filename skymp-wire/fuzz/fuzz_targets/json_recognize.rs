//! Arbitrary bytes into the JSON edge's recognizer, the parser that reads
//! what the C++ core and skymp5-client hand the wire. Must return Ok or Err,
//! never panic; an Ok must render to JSON that recognizes as the same
//! message (a JSON number past f32's range reads as infinity, which render
//! refuses, as the client's send path does).
#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    if let Ok(msg) = wire_json::recognize_bytes(data) {
        match wire_json::render(&msg) {
            Ok(json) => assert_eq!(wire_json::recognize(&json), Ok(msg), "{json}"),
            Err(wire_json::JsonError::NonFinite) => {}
            Err(e) => panic!("a recognized message did not render: {e}"),
        }
    }
});
