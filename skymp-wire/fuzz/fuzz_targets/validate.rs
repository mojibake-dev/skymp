//! Structured fuzzing of the validator: arbitrary but well-typed messages
//! through every validator path with a moving clock. Must never panic.
#![no_main]
use libfuzzer_sys::fuzz_target;

#[derive(arbitrary::Arbitrary, Debug)]
struct Input {
    // TODO(M0): derive Arbitrary for wire_schema::Message behind a feature
    // flag so this target constructs messages directly instead of bytes.
    bytes: Vec<u8>,
    clock: Vec<u16>,
}

fuzz_target!(|input: Input| {
    let mut guard = wire_validate::ClientGuard::default();
    let mut now = 0u64;
    for (i, tick) in input.clock.iter().enumerate() {
        now = now.saturating_add(u64::from(*tick));
        let start = i.saturating_mul(8) % input.bytes.len().max(1);
        if let Ok(msg) = wire_codec::decode(input.bytes.get(start..).unwrap_or(&[])) {
            let _ = wire_validate::validate(&msg, &mut guard, now);
        }
    }
});
