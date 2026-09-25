//! Channel map. SkyMP's RakNet reliability choices map onto renet's three
//! send types one to one; this file is the whole port of that decision.

use std::time::Duration;

use renet::{ChannelConfig, ConnectionConfig, SendType};
use wire_schema::Message;

use crate::limits::Limits;

/// Session, ownership, inventory, quests, chat, snippets: order matters.
pub const RELIABLE_ORDERED: u8 = 0;
/// Effects, container deltas, hits: must arrive, order does not matter.
pub const RELIABLE_UNORDERED: u8 = 1;
/// Movement, animation events, aim: newest wins, losses are fine.
pub const UNRELIABLE: u8 = 2;
/// Every channel, for polling loops.
pub const ALL: [u8; 3] = [RELIABLE_ORDERED, RELIABLE_UNORDERED, UNRELIABLE];

/// Which channel a message family rides.
pub fn for_message(msg: &Message) -> u8 {
    match msg {
        Message::Movement(_) => UNRELIABLE,
        Message::Hit(_) | Message::HostedActor(_) | Message::InventoryApply { .. } => {
            RELIABLE_UNORDERED
        }
        _ => RELIABLE_ORDERED,
    }
}

/// The three channels for both directions, sized from `limits`. Channel
/// order is send priority per tick: session traffic first, movement last.
pub fn connection_config(limits: &Limits) -> ConnectionConfig {
    let resend_time = Duration::from_millis(limits.resend_ms);
    let channels = vec![
        ChannelConfig {
            channel_id: RELIABLE_ORDERED,
            max_memory_usage_bytes: limits.channel_memory_bytes,
            send_type: SendType::ReliableOrdered { resend_time },
        },
        ChannelConfig {
            channel_id: RELIABLE_UNORDERED,
            max_memory_usage_bytes: limits.channel_memory_bytes,
            send_type: SendType::ReliableUnordered { resend_time },
        },
        ChannelConfig {
            channel_id: UNRELIABLE,
            max_memory_usage_bytes: limits.channel_memory_bytes,
            send_type: SendType::Unreliable,
        },
    ];
    ConnectionConfig {
        available_bytes_per_tick: limits.bytes_per_tick,
        server_channels_config: channels.clone(),
        client_channels_config: channels,
    }
}
