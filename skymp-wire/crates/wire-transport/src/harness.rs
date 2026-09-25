//! Fuzz harness for renet ingestion (feature `harness`). This is the one
//! place outside the transport proper that drives renet directly, and it is
//! test surface only: the fuzz crate feeds it bytes and never names renet.

use std::time::Duration;

use renet::{Bytes, RenetClient, RenetServer};

use crate::channels::{self, ALL};
use crate::limits::Limits;

/// A renet server with one connection, fed raw packets.
pub struct IngestHarness {
    server: RenetServer,
}

/// The client id the harness registers.
pub const CLIENT: u64 = 1;

impl IngestHarness {
    /// A server configured exactly as the real transport configures it.
    pub fn new(limits: &Limits) -> Self {
        let mut server = RenetServer::new(channels::connection_config(limits));
        server.add_connection(CLIENT);
        Self { server }
    }

    /// Feed one packet as if it came off the wire for the connection.
    pub fn feed(&mut self, packet: &[u8]) {
        let _ = self.server.process_packet_from(packet, CLIENT);
    }

    /// Advance time, drain every channel, and generate outgoing packets.
    pub fn tick(&mut self, ms: u64) -> usize {
        self.server.update(Duration::from_millis(ms));
        let mut drained: usize = 0;
        for ch in ALL {
            while self.server.receive_message(CLIENT, ch).is_some() {
                drained = drained.saturating_add(1);
            }
        }
        let _ = self.server.get_packets_to_send(CLIENT);
        drained
    }

    /// False once renet dropped the connection over what it was fed.
    pub fn is_connected(&self) -> bool {
        self.server.is_connected(CLIENT)
    }

    /// Bytes renet currently holds for the connection's reliable-ordered
    /// channel, for memory assertions.
    pub fn available_memory(&self) -> usize {
        self.server
            .channel_available_memory(CLIENT, channels::RELIABLE_ORDERED)
    }
}

/// Real packets from a real client: small messages on each channel and
/// sliced ones on the reliable and unreliable channels, for seeding the
/// corpus of `fuzz/transport_ingest`.
pub fn seed_packets(limits: &Limits) -> Vec<Vec<u8>> {
    let mut client = RenetClient::new(channels::connection_config(limits));
    client.set_connected();
    let big = vec![0xAB_u8; 3000];
    client.send_message(channels::RELIABLE_ORDERED, Bytes::from_static(b"ordered"));
    client.send_message(
        channels::RELIABLE_UNORDERED,
        Bytes::from_static(b"unordered"),
    );
    client.send_message(channels::UNRELIABLE, Bytes::from_static(b"unreliable"));
    client.send_message(channels::RELIABLE_ORDERED, Bytes::from(big.clone()));
    client.send_message(channels::UNRELIABLE, Bytes::from(big));
    client.update(Duration::from_millis(16));
    client.get_packets_to_send()
}
