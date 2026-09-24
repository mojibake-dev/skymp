//! Bytes into the transport's packet ingestion after a scripted connect,
//! including fragment sequences. This target exists because fragment
//! reassembly is where RakNet-class bugs live. Must never panic and must
//! not exceed the configured queue caps.
#![no_main]
use libfuzzer_sys::fuzz_target;

fuzz_target!(|data: &[u8]| {
    // TODO(M0): build a RenetServer with channels::connection_config(),
    // drive a fake client through the netcode handshake using an in-memory
    // socket (renet2 ships one; for renet, feed server.process_packet with
    // the bytes directly after a scripted connect), then push `data` as
    // packets and assert memory stays under limits::Limits::default().
    let _ = data;
});
