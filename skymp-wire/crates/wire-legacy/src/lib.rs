//! SkyMP's binary message format, read into wire-schema types (ADR-019).
//! Test tooling: it turns lab captures into typed messages for difftest
//! sessions, and it proves that every message the old protocol carried fits
//! the new schema's capacities. Nothing here runs on a live network path.
//!
//! The format is SkyMP's BitStreamOutputArchive over SLikeNet's BitStream:
//! a 0x86 packet id, then each message's `Serialize(Archive&)` field list in
//! order, most significant bit first, with no alignment. Integers and floats
//! are big-endian and as wide as their C++ type; a bool is one bit; an
//! optional is a presence bit and the value; a string or vector is a 32-bit
//! count and its elements; a `std::array` is its elements; a variant is a
//! 32-bit alternative index and the value; the `"t"` constant is one byte.
//! The schema's structs keep the C++ field order, so one serde deserializer
//! reads all 33 types; the only special case is `MsgT`, which reads as a
//! unit and so stands for that one byte here.

use serde::de::{self, DeserializeSeed, IntoDeserializer, Visitor};
use wire_schema::{skymp, Message};

/// SkyMP's application packet id (MinPacketId.h): RakNet's ID_USER_PACKET_ENUM.
pub const MIN_PACKET_ID: u8 = 0x86;

/// Why a packet could not be read.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
pub enum LegacyError {
    /// The packet does not start with [`MIN_PACKET_ID`].
    #[error("not a SkyMP packet (id {0:#04x})")]
    NotSkymp(u8),
    /// JSON after the packet id: the old fallback path, not the binary form.
    #[error("a JSON packet, not binary")]
    Json,
    /// A MsgType the schema does not have.
    #[error("unknown MsgType {0}")]
    UnknownType(u8),
    /// The bits ran out, a count was larger than what is left, or a value
    /// did not fit its schema type.
    #[error("malformed: {0}")]
    Malformed(String),
    /// More than padding left after the message.
    #[error("{0} bits after the message")]
    Trailing(usize),
}

impl de::Error for LegacyError {
    fn custom<T: core::fmt::Display>(msg: T) -> Self {
        LegacyError::Malformed(msg.to_string())
    }
}

/// Bit reader, most significant bit first.
struct Bits<'a> {
    data: &'a [u8],
    pos: usize,
}

impl Bits<'_> {
    fn remaining(&self) -> usize {
        self.data.len().saturating_mul(8).saturating_sub(self.pos)
    }

    fn bit(&mut self) -> Result<bool, LegacyError> {
        let byte = self
            .data
            .get(self.pos / 8)
            .copied()
            .ok_or_else(|| LegacyError::Malformed("out of bits".into()))?;
        let shift = 7usize.saturating_sub(self.pos % 8);
        self.pos = self.pos.saturating_add(1);
        Ok((byte >> shift) & 1 == 1)
    }

    fn bits(&mut self, n: u32) -> Result<u64, LegacyError> {
        let mut v: u64 = 0;
        for _ in 0..n {
            v = (v << 1) | u64::from(self.bit()?);
        }
        Ok(v)
    }
}

fn narrow<T: TryFrom<u64>>(v: u64) -> Result<T, LegacyError> {
    T::try_from(v).map_err(|_| LegacyError::Malformed(format!("{v} does not fit")))
}

fn signed<T: TryFrom<i64>>(v: u64, width: u32) -> Result<T, LegacyError> {
    // sign-extend a `width`-bit two's complement value
    let shift = 64u32.saturating_sub(width);
    let wide = i64::from_ne_bytes(v.to_ne_bytes());
    let extended = wide.checked_shl(shift).and_then(|x| x.checked_shr(shift)).unwrap_or(0);
    T::try_from(extended).map_err(|_| LegacyError::Malformed(format!("{extended} does not fit")))
}

/// The serde deserializer over [`Bits`].
struct De<'a, 'b> {
    bits: &'b mut Bits<'a>,
}

impl<'a, 'b> De<'a, 'b> {
    fn count(&mut self) -> Result<usize, LegacyError> {
        let n: usize = narrow(self.bits.bits(32)?)?;
        // every element is at least one bit: a count beyond what is left is a lie
        if n > self.bits.remaining() {
            return Err(LegacyError::Malformed(format!("count {n} beyond the packet")));
        }
        Ok(n)
    }
}

struct Seq<'x, 'a, 'b> {
    de: &'x mut De<'a, 'b>,
    left: usize,
}

impl<'de, 'x, 'a, 'b> de::SeqAccess<'de> for Seq<'x, 'a, 'b> {
    type Error = LegacyError;
    fn next_element_seed<T: DeserializeSeed<'de>>(&mut self, seed: T) -> Result<Option<T::Value>, LegacyError> {
        if self.left == 0 {
            return Ok(None);
        }
        self.left = self.left.saturating_sub(1);
        seed.deserialize(&mut *self.de).map(Some)
    }
    fn size_hint(&self) -> Option<usize> {
        Some(self.left)
    }
}

struct Enum<'x, 'a, 'b> {
    de: &'x mut De<'a, 'b>,
}

impl<'de, 'x, 'a, 'b> de::EnumAccess<'de> for Enum<'x, 'a, 'b> {
    type Error = LegacyError;
    type Variant = Self;
    fn variant_seed<V: DeserializeSeed<'de>>(self, seed: V) -> Result<(V::Value, Self), LegacyError> {
        let index: u32 = narrow(self.de.bits.bits(32)?)?;
        let v = seed.deserialize(index.into_deserializer())?;
        Ok((v, self))
    }
}

impl<'de, 'x, 'a, 'b> de::VariantAccess<'de> for Enum<'x, 'a, 'b> {
    type Error = LegacyError;
    fn unit_variant(self) -> Result<(), LegacyError> {
        Ok(())
    }
    fn newtype_variant_seed<T: DeserializeSeed<'de>>(self, seed: T) -> Result<T::Value, LegacyError> {
        seed.deserialize(&mut *self.de)
    }
    fn tuple_variant<V: Visitor<'de>>(self, len: usize, visitor: V) -> Result<V::Value, LegacyError> {
        visitor.visit_seq(Seq { de: self.de, left: len })
    }
    fn struct_variant<V: Visitor<'de>>(self, fields: &'static [&'static str], visitor: V) -> Result<V::Value, LegacyError> {
        visitor.visit_seq(Seq { de: self.de, left: fields.len() })
    }
}

macro_rules! unsupported {
    ($($name:ident),*) => {$(
        fn $name<V: Visitor<'de>>(self, _: V) -> Result<V::Value, LegacyError> {
            Err(LegacyError::Malformed(concat!("the legacy format has no ", stringify!($name)).into()))
        }
    )*};
}

impl<'de, 'x, 'a, 'b> de::Deserializer<'de> for &'x mut De<'a, 'b> {
    type Error = LegacyError;

    fn is_human_readable(&self) -> bool {
        false
    }

    unsupported!(deserialize_any, deserialize_char, deserialize_bytes, deserialize_byte_buf, deserialize_map, deserialize_ignored_any);

    fn deserialize_bool<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        v.visit_bool(self.bits.bit()?)
    }
    fn deserialize_u8<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        v.visit_u8(narrow(self.bits.bits(8)?)?)
    }
    fn deserialize_u16<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        v.visit_u16(narrow(self.bits.bits(16)?)?)
    }
    fn deserialize_u32<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        v.visit_u32(narrow(self.bits.bits(32)?)?)
    }
    fn deserialize_u64<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        v.visit_u64(self.bits.bits(64)?)
    }
    fn deserialize_i8<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        v.visit_i8(signed(self.bits.bits(8)?, 8)?)
    }
    fn deserialize_i16<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        v.visit_i16(signed(self.bits.bits(16)?, 16)?)
    }
    fn deserialize_i32<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        v.visit_i32(signed(self.bits.bits(32)?, 32)?)
    }
    fn deserialize_i64<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        v.visit_i64(signed(self.bits.bits(64)?, 64)?)
    }
    fn deserialize_f32<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        v.visit_f32(f32::from_bits(narrow(self.bits.bits(32)?)?))
    }
    fn deserialize_f64<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        v.visit_f64(f64::from_bits(self.bits.bits(64)?))
    }
    fn deserialize_str<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        let n = self.count()?;
        let mut bytes = Vec::with_capacity(n.min(4096));
        for _ in 0..n {
            bytes.push(narrow::<u8>(self.bits.bits(8)?)?);
        }
        let s = String::from_utf8(bytes).map_err(|e| LegacyError::Malformed(e.to_string()))?;
        v.visit_string(s)
    }
    fn deserialize_string<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        self.deserialize_str(v)
    }
    fn deserialize_option<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        if self.bits.bit()? {
            v.visit_some(self)
        } else {
            v.visit_none()
        }
    }
    /// The one unit in the schema is `MsgT`: SkyMP writes the type constant
    /// as one byte, so a unit reads (and drops) one byte here.
    fn deserialize_unit<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        self.bits.bits(8)?;
        v.visit_unit()
    }
    fn deserialize_unit_struct<V: Visitor<'de>>(self, _: &'static str, v: V) -> Result<V::Value, LegacyError> {
        self.deserialize_unit(v)
    }
    fn deserialize_newtype_struct<V: Visitor<'de>>(self, _: &'static str, v: V) -> Result<V::Value, LegacyError> {
        v.visit_newtype_struct(self)
    }
    fn deserialize_seq<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        let left = self.count()?;
        v.visit_seq(Seq { de: self, left })
    }
    fn deserialize_tuple<V: Visitor<'de>>(self, len: usize, v: V) -> Result<V::Value, LegacyError> {
        v.visit_seq(Seq { de: self, left: len })
    }
    fn deserialize_tuple_struct<V: Visitor<'de>>(self, _: &'static str, len: usize, v: V) -> Result<V::Value, LegacyError> {
        v.visit_seq(Seq { de: self, left: len })
    }
    fn deserialize_struct<V: Visitor<'de>>(self, _: &'static str, fields: &'static [&'static str], v: V) -> Result<V::Value, LegacyError> {
        v.visit_seq(Seq { de: self, left: fields.len() })
    }
    fn deserialize_enum<V: Visitor<'de>>(self, _: &'static str, _: &'static [&'static str], v: V) -> Result<V::Value, LegacyError> {
        v.visit_enum(Enum { de: self })
    }
    fn deserialize_identifier<V: Visitor<'de>>(self, v: V) -> Result<V::Value, LegacyError> {
        self.deserialize_u32(v)
    }
}

macro_rules! skymp_family {
    ($m:ident) => {
        $m! {
            1 CustomPacket, 2 UpdateMovement, 3 UpdateAnimation, 4 UpdateAppearance,
            5 UpdateEquipment, 6 Activate, 7 UpdateProperty, 8 PutItem, 9 TakeItem,
            10 FinishSpSnippet, 11 OnEquip, 12 ConsoleCommand, 13 CraftItem, 14 Host,
            15 CustomEvent, 16 ChangeValues, 17 OnHit, 18 DeathStateContainer,
            19 DropItem, 20 Teleport, 21 OpenContainer, 22 PlayerBowShot, 23 SpellCast,
            24 UpdateAnimVariables, 25 DestroyActor, 26 HostStart, 27 HostStop,
            28 SetInventory, 29 SetRaceMenuOpen, 30 SpSnippet, 31 Teleport2,
            32 UpdateGamemodeData, 33 CreateActor
        }
    };
}

/// Read one SkyMP packet (0x86, the MsgType byte, the fields) into a typed
/// message. Every schema capacity applies, so a packet that does not fit the
/// new schema fails here.
pub fn decode_packet(packet: &[u8]) -> Result<Message, LegacyError> {
    let id = *packet.first().ok_or_else(|| LegacyError::Malformed("empty".into()))?;
    if id != MIN_PACKET_ID {
        return Err(LegacyError::NotSkymp(id));
    }
    let t = *packet.get(1).ok_or_else(|| LegacyError::Malformed("no MsgType".into()))?;
    if t == b'{' {
        return Err(LegacyError::Json);
    }
    let mut bits = Bits { data: packet, pos: 8 };
    let msg = {
        let mut de = De { bits: &mut bits };
        macro_rules! arms {
            ($($n:literal $name:ident),*) => {
                match t {
                    $($n => Message::$name(<skymp::$name as serde::Deserialize>::deserialize(&mut de)?),)*
                    other => return Err(LegacyError::UnknownType(other)),
                }
            };
        }
        skymp_family!(arms)
    };
    let left = bits.remaining();
    if left >= 8 {
        return Err(LegacyError::Trailing(left));
    }
    Ok(msg)
}

#[cfg(test)]
#[allow(clippy::panic, clippy::indexing_slicing, clippy::expect_used, clippy::arithmetic_side_effects)] // test fixtures, not packet input
mod tests {
    use super::*;

    /// One line per captured packet: direction and hex, from run
    /// 20261001-233455 (smoke-two-players, green). Extracted with the RakNet
    /// frame parser in fixtures/README.md.
    const SMOKE: &str = include_str!("../fixtures/smoke-233455.hex");

    fn packets() -> Vec<(String, Vec<u8>)> {
        SMOKE
            .lines()
            .filter(|l| !l.trim().is_empty() && !l.starts_with('#'))
            .map(|l| {
                let (dir, hex) = l.split_once(' ').expect("dir hex");
                let bytes = (0..hex.len())
                    .step_by(2)
                    .map(|i| u8::from_str_radix(&hex[i..i + 2], 16).expect("hex"))
                    .collect();
                (dir.to_string(), bytes)
            })
            .collect()
    }

    #[test]
    fn every_captured_packet_reads_into_the_schema() {
        let ps = packets();
        assert!(ps.len() > 50, "{}", ps.len());
        for (i, (dir, p)) in ps.iter().enumerate() {
            let m = decode_packet(p).unwrap_or_else(|e| panic!("packet {i} ({dir}, MsgType {}): {e}", p[1]));
            assert_eq!(m.msg_type(), Some(p[1]), "packet {i}");
            // it travels the new wire, inside its byte cap, byte-identically
            let bytes = wire_codec::encode(&m).unwrap_or_else(|e| panic!("packet {i}: {e}"));
            assert_eq!(wire_codec::decode(&bytes), Ok(m.clone()), "packet {i}");
            // and its JSON form round-trips
            let json = wire_json::render(&m).unwrap_or_else(|e| panic!("packet {i}: {e}"));
            assert_eq!(wire_json::recognize(&json), Ok(m.clone()), "packet {i}: {json}");
            // postcard spends a byte where the old format spent a bit (bools,
            // optional presence: an inventory stack carries eleven) and saves
            // on small integers (varints): movement shrinks, SetInventory
            // grows by about half (`wire_size_old_against_new`), never more
            // than double
            assert!(bytes.len() <= 2 * p.len() + 16, "packet {i}: {} vs {}", bytes.len(), p.len());
        }
    }

    #[test]
    fn wire_size_old_against_new() {
        let mut old = std::collections::BTreeMap::<u8, (usize, usize, usize)>::new();
        for (_, p) in packets() {
            let m = decode_packet(&p).expect("decodes");
            let new = wire_codec::encode(&m).expect("encodes").len();
            let e = old.entry(p[1]).or_default();
            e.0 += 1;
            e.1 += p.len();
            e.2 += new;
        }
        for (t, (n, a, b)) in &old {
            println!("MsgType {t:2} x{n:3}: legacy {a:6} B, postcard {b:6} B");
        }
    }

    #[test]
    fn known_values_from_the_capture() {
        // c2s ChangeValues: idx present (1), health 1.0, and so on (hand-decoded)
        let cv = [0x86u8, 0x10, 0x80, 0x00, 0x00, 0x00, 0xcf, 0xe0, 0x00, 0x00, 0x27, 0xf0, 0x00, 0x00, 0x13, 0xf8, 0x00, 0x00, 0x00];
        match decode_packet(&cv) {
            Ok(Message::ChangeValues(c)) => {
                assert_eq!(c.idx, Some(1));
                assert_eq!(c.data.health, Some(1.0));
                assert_eq!(c.data.magicka, Some(1.0));
                assert_eq!(c.data.stamina, Some(1.0));
            }
            other => panic!("{other:?}"),
        }
        // c2s OnEquip of 0x00012e46
        assert!(matches!(decode_packet(&[0x86, 0x0b, 0x00, 0x01, 0x2e, 0x46]), Ok(Message::OnEquip(e)) if e.base_id == 0x12e46));
    }

    #[test]
    fn lies_and_junk_are_errors_not_panics() {
        assert_eq!(decode_packet(&[0x85, 0x01]), Err(LegacyError::NotSkymp(0x85)));
        assert_eq!(decode_packet(&[0x86, b'{']), Err(LegacyError::Json));
        assert_eq!(decode_packet(&[0x86, 0x22]), Err(LegacyError::UnknownType(0x22)));
        // CustomPacket claiming a 4 GiB string: refused by the count check, nothing allocated
        assert!(matches!(decode_packet(&[0x86, 0x01, 0xff, 0xff, 0xff, 0xff, 0x41]), Err(LegacyError::Malformed(_))));
        // trailing bytes
        assert!(matches!(decode_packet(&[0x86, 0x0b, 0, 0, 0, 1, 0xff, 0xff]), Err(LegacyError::Trailing(16))));
        for len in 0..64usize {
            let junk: Vec<u8> = (0..len).map(|i| u8::try_from(i * 37 % 251).unwrap_or(0)).collect();
            let mut p = vec![0x86, u8::try_from(len % 34).unwrap_or(1)];
            p.extend(junk);
            let _ = decode_packet(&p);
        }
    }
}
