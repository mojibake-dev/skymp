//! Structured fuzzing of the validator and, on the way, the codec: arbitrary
//! but well-typed messages through every validator path with a moving clock.
//! Must never panic; every constructed message must also survive an
//! encode, decode, encode round trip byte for byte.
#![no_main]
use libfuzzer_sys::fuzz_target;
use wire_schema::{cap, FormId, Hello, HitIntent, HostedActorState, ItemDelta, Message, MovementSample, Transform};

#[derive(arbitrary::Arbitrary, Debug)]
struct T {
    x: f32,
    y: f32,
    z: f32,
    yaw: f32,
    pitch: f32,
}

/// Mirror of `Message` with unbounded collections; the conversion truncates
/// to the schema's capacities so the fuzzer explores both sides of each cap.
#[derive(arbitrary::Arbitrary, Debug)]
enum M {
    Hello { v: u16, build: u32, mods: Vec<u64>, name: String },
    Movement { seq: u32, actor: u32, t: T, run: bool, sneak: bool },
    Hit { seq: u32, a: u32, target: u32, w: u32, p: bool, ms: u32 },
    Hosted { actor: u32, t: T, health: f32, deltas: Vec<(u32, i32)> },
    Welcome { id: u64, t: u64 },
    Refuse { r: u16 },
    InventoryApply { owner: u32, item: u32, count: i32 },
    HostGrant { cell: u32 },
    HostRelease { cell: u32 },
}

fn tf(t: &T) -> Transform {
    Transform { x: t.x, y: t.y, z: t.z, yaw: t.yaw, pitch: t.pitch }
}

fn build(m: &M) -> Message {
    match m {
        M::Hello { v, build, mods, name } => {
            let mut mod_hashes = heapless::Vec::<u64, { cap::MODS }>::new();
            for h in mods.iter().take(cap::MODS) {
                let _ = mod_hashes.push(*h);
            }
            let mut n = heapless::String::<{ cap::NAME }>::new();
            for ch in name.chars() {
                if n.push(ch).is_err() {
                    break;
                }
            }
            Message::Hello(Hello { schema_version: *v, client_build: *build, mod_hashes, name: n })
        }
        M::Movement { seq, actor, t, run, sneak } => {
            Message::Movement(MovementSample { seq: *seq, actor: FormId(*actor), transform: tf(t), run: *run, sneak: *sneak })
        }
        M::Hit { seq, a, target, w, p, ms } => Message::Hit(HitIntent {
            seq: *seq,
            attacker: FormId(*a),
            target: FormId(*target),
            weapon: FormId(*w),
            power_attack: *p,
            client_time_ms: *ms,
        }),
        M::Hosted { actor, t, health, deltas } => {
            let mut inventory_delta = heapless::Vec::<ItemDelta, { cap::INVENTORY_DELTA }>::new();
            for (item, count) in deltas.iter().take(cap::INVENTORY_DELTA) {
                let _ = inventory_delta.push(ItemDelta { item: FormId(*item), count: *count });
            }
            Message::HostedActor(HostedActorState { actor: FormId(*actor), transform: tf(t), health: *health, inventory_delta })
        }
        M::Welcome { id, t } => Message::Welcome { client_id: wire_schema::ClientId(*id), server_time_ms: *t },
        M::Refuse { r } => Message::Refuse { reason: *r },
        M::InventoryApply { owner, item, count } => Message::InventoryApply { owner: FormId(*owner), delta: ItemDelta { item: FormId(*item), count: *count } },
        M::HostGrant { cell } => Message::HostGrant { cell: FormId(*cell) },
        M::HostRelease { cell } => Message::HostRelease { cell: FormId(*cell) },
    }
}

fuzz_target!(|input: (Vec<M>, Vec<u16>)| {
    let (messages, ticks) = input;
    let mut guard = wire_validate::ClientGuard::default();
    let mut now = 0u64;
    for (m, tick) in messages.iter().zip(ticks.iter().chain(std::iter::repeat(&16))) {
        now = now.saturating_add(u64::from(*tick));
        let msg = build(m);
        let _ = wire_validate::validate(&msg, &mut guard, now);
        let mut a = [0u8; Message::MAX_ENCODED_LEN];
        let first = wire_codec::encode(&msg, &mut a).expect("every constructed message encodes");
        let decoded = wire_codec::decode(first).expect("every encoding decodes");
        let mut b = [0u8; Message::MAX_ENCODED_LEN];
        let second = wire_codec::encode(&decoded, &mut b).expect("re-encode");
        assert_eq!(first, second, "canonical round trip");
    }
});
