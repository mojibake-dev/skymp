//! Structured fuzzing of the validator, the codec and the JSON edge:
//! arbitrary but well-typed messages (wire-schema's `arbitrary` feature)
//! through every validator path with a moving clock. Must never panic; every
//! message inside its byte cap must survive encode, decode, encode byte for
//! byte; every SkyMP message with finite floats must survive render and
//! recognize unchanged.
#![no_main]
use libfuzzer_sys::fuzz_target;
use wire_schema::Message;

fuzz_target!(|input: (Vec<Message>, Vec<u16>)| {
    let (messages, ticks) = input;
    let mut guard = wire_validate::ClientGuard::default();
    let mut now = 0u64;
    for (msg, tick) in messages.iter().zip(ticks.iter().chain(std::iter::repeat(&16))) {
        now = now.saturating_add(u64::from(*tick));
        let _ = wire_validate::validate(msg, &mut guard, now);
        if let Ok(first) = wire_codec::encode(msg) {
            let decoded = wire_codec::decode(&first).expect("every encoding decodes");
            let second = wire_codec::encode(&decoded).expect("re-encode");
            assert_eq!(first, second, "canonical round trip");
        }
        if msg.msg_type().is_some() && wire_schema::finite::all_finite(msg) {
            let json = wire_json::render(msg).expect("a finite SkyMP message renders");
            assert_eq!(wire_json::recognize(&json).as_ref(), Ok(msg), "JSON round trip: {json}");
        }
    }
});
