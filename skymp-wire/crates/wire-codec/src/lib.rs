//! Encode and decode [`Message`]. This crate is the recognizer: bytes come in,
//! a fully decoded message or a typed error comes out, nothing in between.
//! `fuzz/codec_decode` is its proof of totality.

use wire_schema::Message;

/// Decode failures. Reason codes are stable and greppable in lab logs.
#[derive(Debug, thiserror::Error, PartialEq, Eq)]
pub enum WireError {
    #[error("E_WIRE_TOO_LONG: {len} > {max}")]
    TooLong { len: usize, max: usize },
    #[error("E_WIRE_MALFORMED")]
    Malformed,
    #[error("E_WIRE_TRAILING: {0} bytes after message")]
    Trailing(usize),
}

/// Decode one message. Refuses over-long inputs before touching them and
/// refuses trailing bytes after the message.
pub fn decode(bytes: &[u8]) -> Result<Message, WireError> {
    let max = Message::MAX_ENCODED_LEN;
    if bytes.len() > max {
        return Err(WireError::TooLong { len: bytes.len(), max });
    }
    let (msg, rest) =
        postcard::take_from_bytes::<Message>(bytes).map_err(|_| WireError::Malformed)?;
    if !rest.is_empty() {
        return Err(WireError::Trailing(rest.len()));
    }
    Ok(msg)
}

/// Encode one message into a caller-provided buffer; returns the used slice.
/// The buffer must be at least [`Message::MAX_ENCODED_LEN`] bytes.
pub fn encode<'a>(msg: &Message, buf: &'a mut [u8]) -> Result<&'a [u8], WireError> {
    postcard::to_slice(msg, buf)
        .map(|s| &*s)
        .map_err(|_| WireError::Malformed)
}

#[cfg(test)]
mod tests {
    use super::*;
    use wire_schema::{FormId, MovementSample, Transform};

    #[test]
    fn round_trip_movement() -> Result<(), WireError> {
        let m = Message::Movement(MovementSample {
            seq: 7,
            actor: FormId(0x14),
            transform: Transform { x: 1.0, y: 2.0, z: 3.0, yaw: 0.5, pitch: 0.0 },
            run: true,
            sneak: false,
        });
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
}
