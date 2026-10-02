//! Encode and decode [`Message`]. This crate is the recognizer: bytes come in,
//! a fully decoded message or a typed error comes out, nothing in between.
//!
//! Order of refusal, cheapest first: the input is longer than any message
//! ([`Message::MAX_ENCODED_LEN`]); the variant tag names no message; the input
//! is longer than that message's own byte cap ([`Message::max_len`]); the body
//! does not decode, a capacity is exceeded at some field, or bytes trail it;
//! the input is not the canonical encoding of what it decoded to. postcard's
//! varints accept overlong forms, so without the last check one message would
//! have many encodings and the byte-identical round trip that
//! `fuzz/codec_decode` asserts would not hold. Every error carries a stable
//! reason code the lab can grep.

use wire_schema::{Message, WIRE_IDS};

/// Decode failures. Reason codes are stable and greppable in lab logs.
#[derive(Debug, Clone, thiserror::Error, PartialEq, Eq)]
pub enum WireError {
    /// Input longer than the message's cap (or any message's); refused
    /// before the body is read.
    #[error("E_WIRE_TOO_LONG: {len} > {max}")]
    TooLong {
        /// Length received.
        len: usize,
        /// The cap that applied.
        max: usize,
    },
    /// Not a message: bad variant id, truncated field, over-capacity collection.
    #[error("E_WIRE_MALFORMED")]
    Malformed,
    /// A complete message followed by extra bytes.
    #[error("E_WIRE_TRAILING: {0} bytes after message")]
    Trailing(usize),
    /// Parsed, but not the canonical encoding of what it parsed to.
    #[error("E_WIRE_NONCANONICAL")]
    NonCanonical,
}

/// The variant tag at the front of an encoded message: a postcard varint
/// (LEB128, little-endian groups of seven bits, at most five bytes for u32).
fn peek_tag(bytes: &[u8]) -> Option<u32> {
    let mut value: u32 = 0;
    for (i, b) in bytes.iter().take(5).enumerate() {
        let group = u32::from(b & 0x7f);
        let shift = u32::try_from(i).ok()?.checked_mul(7)?;
        value |= group.checked_shl(shift)?;
        if b & 0x80 == 0 {
            return Some(value);
        }
    }
    None
}

/// Decode one message. Refuses over-long inputs before touching the body,
/// refuses trailing bytes after the message, and refuses non-canonical
/// encodings.
pub fn decode(bytes: &[u8]) -> Result<Message, WireError> {
    if bytes.len() > Message::MAX_ENCODED_LEN {
        return Err(WireError::TooLong {
            len: bytes.len(),
            max: Message::MAX_ENCODED_LEN,
        });
    }
    let tag = peek_tag(bytes).ok_or(WireError::Malformed)?;
    if tag >= WIRE_IDS {
        return Err(WireError::Malformed);
    }
    let cap = Message::max_len(tag).ok_or(WireError::Malformed)?;
    if bytes.len() > cap {
        return Err(WireError::TooLong {
            len: bytes.len(),
            max: cap,
        });
    }
    let (msg, rest) =
        postcard::take_from_bytes::<Message>(bytes).map_err(|_| WireError::Malformed)?;
    if !rest.is_empty() {
        return Err(WireError::Trailing(rest.len()));
    }
    let canonical = postcard::to_allocvec(&msg).map_err(|_| WireError::Malformed)?;
    if canonical != bytes {
        return Err(WireError::NonCanonical);
    }
    Ok(msg)
}

/// Encode one message. A message over its own byte cap is refused: the peer
/// would refuse it anyway, and the sender should know at the send.
pub fn encode(msg: &Message) -> Result<Vec<u8>, WireError> {
    let bytes = postcard::to_allocvec(msg).map_err(|_| WireError::Malformed)?;
    let cap = Message::max_len(msg.wire_id()).unwrap_or(0);
    if bytes.len() > cap {
        return Err(WireError::TooLong {
            len: bytes.len(),
            max: cap,
        });
    }
    Ok(bytes)
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;
    use wire_schema::{skymp, FormId, HitIntent, MovementSample, Transform};

    #[allow(clippy::too_many_arguments)]
    fn movement(
        seq: u32,
        x: f32,
        y: f32,
        z: f32,
        yaw: f32,
        pitch: f32,
        run: bool,
        sneak: bool,
    ) -> Message {
        Message::Movement(MovementSample {
            seq,
            actor: FormId(0x14),
            transform: Transform {
                x,
                y,
                z,
                yaw,
                pitch,
            },
            run,
            sneak,
        })
    }

    fn skymp_movement(idx: u32, x: f32, run_mode: &str) -> Message {
        Message::UpdateMovement(skymp::UpdateMovement {
            t: skymp::MsgT,
            idx,
            data: skymp::MovementData {
                world_or_cell: 0x3c,
                pos: [x, -61130.0, 14662.0],
                run_mode: run_mode.try_into().unwrap_or_default(),
                look_at: Some([1.0, 2.0, 3.0]),
                ..Default::default()
            },
        })
    }

    #[test]
    fn round_trip_movement() -> Result<(), WireError> {
        let m = movement(7, 1.0, 2.0, 3.0, 0.5, 0.0, true, false);
        assert_eq!(decode(&encode(&m)?), Ok(m));
        let s = skymp_movement(3, 133857.0, "Running");
        assert_eq!(decode(&encode(&s)?), Ok(s));
        Ok(())
    }

    #[test]
    fn rejects_too_long() {
        let too_long = vec![0u8; Message::MAX_ENCODED_LEN.saturating_add(1)];
        assert!(matches!(decode(&too_long), Err(WireError::TooLong { .. })));
    }

    #[test]
    fn the_variant_cap_applies_before_the_body() {
        // DestroyActor (id 33) is capped at 16 bytes; 17 bytes are refused
        // by length alone, whatever follows the tag.
        let mut bytes = vec![33u8];
        bytes.extend(std::iter::repeat_n(0u8, 16));
        assert_eq!(decode(&bytes), Err(WireError::TooLong { len: 17, max: 16 }));
    }

    #[test]
    fn encode_refuses_a_message_over_its_cap() {
        let mut inv = skymp::Inventory::default();
        let name: wire_schema::bounded::String<{ wire_schema::cap::NAME }> =
            "x".repeat(wire_schema::cap::NAME).as_str().try_into().unwrap_or_default();
        for i in 0..2000u32 {
            let _ = inv.entries.push(skymp::Entry {
                base_id: i,
                count: 1,
                name: Some(name.clone()),
                ..Default::default()
            });
        }
        // 2000 stacks with 256-byte names is ~530 KiB: over UpdateEquipment's 256 KiB
        let m = Message::UpdateEquipment(skymp::UpdateEquipment {
            t: skymp::MsgT,
            idx: 1,
            data: skymp::Equipment {
                inv,
                ..Default::default()
            },
        });
        assert!(matches!(encode(&m), Err(WireError::TooLong { .. })));
    }

    #[test]
    fn rejects_trailing() -> Result<(), WireError> {
        let m = Message::HostGrant {
            cell: FormId(0x1234),
        };
        let mut with_extra = encode(&m)?;
        with_extra.push(0);
        assert_eq!(decode(&with_extra), Err(WireError::Trailing(1)));
        Ok(())
    }

    #[test]
    fn rejects_malformed() {
        assert_eq!(decode(&[0x7f]), Err(WireError::Malformed), "no such variant");
        assert_eq!(decode(&[]), Err(WireError::Malformed));
        assert_eq!(decode(&[0x80, 0x80, 0x80, 0x80, 0x80]), Err(WireError::Malformed), "endless varint");
        assert_eq!(decode(&[33]), Err(WireError::Malformed), "truncated body");
    }

    #[test]
    fn rejects_non_canonical() -> Result<(), WireError> {
        // `Refuse { reason: 0 }` encodes as [tag, 0x00]. The overlong varint
        // [0x80, 0x00] is the same zero to postcard; the codec refuses it.
        let m = Message::Refuse { reason: 0 };
        let bytes = encode(&m)?;
        assert_eq!(bytes.len(), 2);
        let tag = *bytes.first().ok_or(WireError::Malformed)?;
        let overlong = [tag, 0x80, 0x00];
        assert_eq!(decode(&overlong), Err(WireError::NonCanonical));
        // and an overlong tag
        let overlong_tag = [0x82, 0x00, 0x00];
        assert_eq!(decode(&overlong_tag), Err(WireError::NonCanonical));
        Ok(())
    }

    fn fail(e: WireError) -> TestCaseError {
        TestCaseError::fail(e.to_string())
    }

    proptest! {
        #[test]
        fn encode_decode_is_identity_on_bytes(
            seq in any::<u32>(), x in any::<f32>(), y in any::<f32>(), z in any::<f32>(),
            yaw in any::<f32>(), pitch in any::<f32>(), run in any::<bool>(), sneak in any::<bool>(),
        ) {
            let m = movement(seq, x, y, z, yaw, pitch, run, sneak);
            let first = encode(&m).map_err(fail)?;
            let decoded = decode(&first).map_err(fail)?;
            let second = encode(&decoded).map_err(fail)?;
            prop_assert_eq!(first, second);
        }

        #[test]
        fn skymp_movement_round_trip(idx in any::<u32>(), x in -1.0e6f32..1.0e6, mode in "[A-Za-z]{0,32}") {
            let m = skymp_movement(idx, x, &mode);
            let bytes = encode(&m).map_err(fail)?;
            prop_assert_eq!(decode(&bytes), Ok(m));
        }

        #[test]
        fn hit_round_trip(
            seq in any::<u32>(), a in any::<u32>(), t in any::<u32>(), w in any::<u32>(),
            p in any::<bool>(), ms in any::<u32>(),
        ) {
            let m = Message::Hit(HitIntent {
                seq, attacker: FormId(a), target: FormId(t), weapon: FormId(w),
                power_attack: p, client_time_ms: ms,
            });
            let bytes = encode(&m).map_err(fail)?;
            prop_assert_eq!(decode(&bytes), Ok(m));
        }

        #[test]
        fn decode_is_total(bytes in proptest::collection::vec(any::<u8>(), 0..4096)) {
            let _ = decode(&bytes);
        }
    }
}
