//! Packets into renet's ingestion for one connection, through the harness
//! the transport crate exposes for exactly this. Must never panic, and live
//! heap must stay under the configured channel memory plus a fixed
//! allowance. This target exists because fragment reassembly is where
//! RakNet-class bugs live. Seed the corpus with real packets:
//! `cargo test -p wire-transport --features harness -- --ignored write_fuzz_seeds`.
#![no_main]
use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering};

use libfuzzer_sys::fuzz_target;
use wire_transport::harness::IngestHarness;
use wire_transport::limits::Limits;

struct Counting;
static LIVE: AtomicUsize = AtomicUsize::new(0);

unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        LIVE.fetch_add(layout.size(), Ordering::Relaxed);
        System.alloc(layout)
    }
    unsafe fn dealloc(&self, ptr: *mut u8, layout: Layout) {
        LIVE.fetch_sub(layout.size(), Ordering::Relaxed);
        System.dealloc(ptr, layout)
    }
}

#[global_allocator]
static ALLOC: Counting = Counting;

#[derive(arbitrary::Arbitrary, Debug)]
struct Input {
    packets: Vec<Vec<u8>>,
    ticks: Vec<u16>,
}

fuzz_target!(|input: Input| {
    let limits = Limits::default();
    // Everything renet may legitimately hold: three receive channels, three
    // send channels, plus the harness and the input itself.
    let allowance = limits.channel_memory_bytes * 6 + 16 * 1024 * 1024;
    let mut h = IngestHarness::new(&limits);
    let mut ticks = input.ticks.iter().copied().chain(std::iter::repeat(16));
    for packet in &input.packets {
        h.feed(packet);
        let tick = ticks.next().unwrap_or(16);
        h.tick(u64::from(tick));
        assert!(LIVE.load(Ordering::Relaxed) < allowance, "renet grew past the configured channel memory");
        if !h.is_connected() {
            break;
        }
    }
});
