//! Arbitrary bytes into decode. Must return Ok or Err, never panic; an Ok
//! must re-encode to the same bytes.
#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    if let Ok(msg) = wire_codec::decode(data) {
        let mut buf = [0u8; wire_schema::Message::MAX_ENCODED_LEN];
        if let Ok(bytes) = wire_codec::encode(&msg, &mut buf) {
            assert_eq!(bytes, data, "round trip must be byte-identical");
        }
    }
});
