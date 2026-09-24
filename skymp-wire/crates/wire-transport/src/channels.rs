//! Channel map. SkyMP's RakNet reliability choices map onto renet's three
//! send types one to one; this file is the whole port of that decision.

use wire_schema::Message;

/// Channel ids, stable.
pub const RELIABLE_ORDERED: u8 = 0;
pub const RELIABLE_UNORDERED: u8 = 1;
pub const UNRELIABLE: u8 = 2;
pub const ALL: [u8; 3] = [RELIABLE_ORDERED, RELIABLE_UNORDERED, UNRELIABLE];

/// Which channel a message family rides.
pub fn for_message(msg: &Message) -> u8 {
    match msg {
        Message::Movement(_) => UNRELIABLE,
        Message::Hit(_) | Message::HostedActor(_) | Message::InventoryApply { .. } => RELIABLE_UNORDERED,
        _ => RELIABLE_ORDERED,
    }
}

// TODO(M0): pub fn connection_config() -> renet::ConnectionConfig with three
// ChannelConfig entries (ReliableOrdered / ReliableUnordered / Unreliable,
// resend_time from limits), applied to both client and server lists.
