//! Encode and decode [`Message`]. This crate is the recognizer: bytes come in,
//! a fully decoded message or a typed error comes out, nothing in between.
//!
//! Decoding is canonical: the input must be exactly the bytes `encode` produces
//! for the message it parsed to. postcard's varints accept overlong forms, so
//! without this check one message would have many encodings and the
//! byte-identical round trip that `fuzz/codec_decode` asserts would not hold.
//! Every error carries a stable reason code the lab can grep.

use wire_schema::Message;

/// Decode failures. Reason codes are stable and greppable in lab logs.
#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum WireError {
    /// Input longer than any legal message; refused before it is read.
    #[error("E_WIRE_TOO_LONG: {len} > {max}")]
    TooLong {
        /// Length received.
        len: usize,
        /// [`Message::MAX_ENCODED_LEN`].
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

/// Decode one message. Refuses over-long inputs before touching them, refuses
/// trailing bytes after the message, and refuses non-canonical encodings.
pub fn decode(bytes: &[u8]) -> Result<Message, WireError> {
    let max = Message::MAX_ENCODED_LEN;
    if bytes.len() > max {
        return Err(WireError::TooLong {
            len: bytes.len(),
            max,
        });
    }
    let (msg, rest) =
        postcard::take_from_bytes::<Message>(bytes).map_err(|_| WireError::Malformed)?;
    if !rest.is_empty() {
        return Err(WireError::Trailing(rest.len()));
    }
    let mut buf = [0u8; Message::MAX_ENCODED_LEN];
    let canonical = encode(&msg, &mut buf)?;
    if canonical != bytes {
        return Err(WireError::NonCanonical);
    }
    Ok(msg)
}

/// Encode one message into a caller-provided buffer; returns the used slice.
/// The buffer must be at least [`Message::MAX_ENCODED_LEN`] bytes; a shorter
/// buffer yields `Malformed`.
pub fn encode<'a>(msg: &Message, buf: &'a mut [u8]) -> Result<&'a [u8], WireError> {
    postcard::to_slice(msg, buf)
        .map(|s| &*s)
        .map_err(|_| WireError::Malformed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;
    use wire_schema::{FormId, HitIntent, MovementSample, Transform};

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

    #[test]
    fn round_trip_movement() -> Result<(), WireError> {
        let m = movement(7, 1.0, 2.0, 3.0, 0.5, 0.0, true, false);
        let mut buf = [0u8; Message::MAX_ENCODED_LEN];
        let bytes = encode(&m, &mut buf)?;
        assert_eq!(decode(bytes), Ok(m));
        Ok(())
    }

    #[test]
    fn rejects_too_long() {
        let too_long = vec![0u8; Message::MAX_ENCODED_LEN.saturating_add(1)];
        assert!(matches!(decode(&too_long), Err(WireError::TooLong { .. })));
    }

    #[test]
    fn rejects_trailing() -> Result<(), WireError> {
        let m = Message::HostGrant {
            cell: FormId(0x1234),
        };
        let mut buf = [0u8; Message::MAX_ENCODED_LEN];
        let bytes = encode(&m, &mut buf)?;
        let mut with_extra = bytes.to_vec();
        with_extra.push(0);
        assert_eq!(decode(&with_extra), Err(WireError::Trailing(1)));
        Ok(())
    }

    #[test]
    fn rejects_malformed() {
        assert_eq!(decode(&[0x7f]), Err(WireError::Malformed));
        assert_eq!(decode(&[]), Err(WireError::Malformed));
    }

    #[test]
    fn rejects_non_canonical() -> Result<(), WireError> {
        // `Refuse { reason: 0 }` encodes as [tag, 0x00]. The overlong varint
        // [0x80, 0x00] is the same zero to postcard; the codec refuses it.
        let m = Message::Refuse { reason: 0 };
        let mut buf = [0u8; Message::MAX_ENCODED_LEN];
        let bytes = encode(&m, &mut buf)?;
        assert_eq!(bytes.len(), 2);
        let tag = *bytes.first().ok_or(WireError::Malformed)?;
        let overlong = [tag, 0x80, 0x00];
        assert_eq!(decode(&overlong), Err(WireError::NonCanonical));
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
            let mut a = [0u8; Message::MAX_ENCODED_LEN];
            let first = encode(&m, &mut a).map_err(fail)?;
            let decoded = decode(first).map_err(fail)?;
            let mut b = [0u8; Message::MAX_ENCODED_LEN];
            let second = encode(&decoded, &mut b).map_err(fail)?;
            prop_assert_eq!(first, second);
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
            let mut buf = [0u8; Message::MAX_ENCODED_LEN];
            let bytes = encode(&m, &mut buf).map_err(fail)?;
            prop_assert_eq!(decode(bytes), Ok(m));
        }

        #[test]
        fn decode_is_total(bytes in proptest::collection::vec(any::<u8>(), 0..=Message::MAX_ENCODED_LEN.saturating_add(2))) {
            let _ = decode(&bytes);
        }
    }
}
