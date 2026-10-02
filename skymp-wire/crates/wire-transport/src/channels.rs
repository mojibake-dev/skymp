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

/// How the sender wants a message delivered. SkyMP's senders say per send
/// (`reliable` true or false); the M0 families have a fixed channel.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Delivery {
    /// The family's channel ([`for_message`]).
    Default,
    /// Must arrive, in order: ReliableOrdered. SkyMP's server sent reliable
    /// as RELIABLE_ORDERED and its client as RELIABLE (unordered); ordered is
    /// the stronger of the two and only ever adds ordering (ADR-019).
    Reliable,
    /// May be lost: Unreliable.
    Unreliable,
}

/// Which channel a message family rides when the sender does not say.
pub fn for_message(msg: &Message) -> u8 {
    match msg {
        Message::Movement(_) | Message::UpdateMovement(_) => UNRELIABLE,
        Message::Hit(_) | Message::HostedActor(_) | Message::InventoryApply { .. } => {
            RELIABLE_UNORDERED
        }
        _ => RELIABLE_ORDERED,
    }
}

/// The channel for a send.
pub fn for_send(msg: &Message, delivery: Delivery) -> u8 {
    match delivery {
        Delivery::Default => for_message(msg),
        Delivery::Reliable => RELIABLE_ORDERED,
        Delivery::Unreliable => UNRELIABLE,
    }
}

fn channels(limits_bytes: usize, resend_time: Duration) -> Vec<ChannelConfig> {
    vec![
        ChannelConfig {
            channel_id: RELIABLE_ORDERED,
            max_memory_usage_bytes: limits_bytes,
            send_type: SendType::ReliableOrdered { resend_time },
        },
        ChannelConfig {
            channel_id: RELIABLE_UNORDERED,
            max_memory_usage_bytes: limits_bytes,
            send_type: SendType::ReliableUnordered { resend_time },
        },
        ChannelConfig {
            channel_id: UNRELIABLE,
            max_memory_usage_bytes: limits_bytes,
            send_type: SendType::Unreliable,
        },
    ]
}

/// The three channels for both directions, sized from `limits`: the
/// server's sending channels (what clients receive) generous, the clients'
/// sending channels (what the server receives) tight. Channel order is send
/// priority per tick: session traffic first, movement last.
pub fn connection_config(limits: &Limits) -> ConnectionConfig {
    let resend_time = Duration::from_millis(limits.resend_ms);
    ConnectionConfig {
        available_bytes_per_tick: limits.bytes_per_tick,
        server_channels_config: channels(limits.server_channel_memory_bytes, resend_time),
        client_channels_config: channels(limits.client_channel_memory_bytes, resend_time),
    }
}
