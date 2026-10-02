//! The two pieces of SkyMP's JSON form that a plain serde derive cannot
//! express in a format-neutral way (ADR-019). Each branches on
//! `is_human_readable()`, which is true for the JSON at the in-process edge
//! and false for postcard on the wire:
//!
//! - [`MsgT`], the `"t"` key every SkyMP message (and three `Data` structs)
//!   carries in JSON: the number in JSON, nothing at all on the wire, where
//!   the variant tag already says it.
//! - The `std::variant` fields, which SkyMP's JSON writes untagged (the bare
//!   value) and its reader resolves by trying each alternative in order; on
//!   the wire they are an ordinary tagged enum.

use core::fmt;

use serde::de::{self, Deserialize, Deserializer, MapAccess, Visitor};
use serde::ser::{Serialize, Serializer};

use crate::bounded::String;
use crate::cap;

/// The SkyMP message type number in the JSON form. Zero bytes on the wire.
/// Reading JSON ignores its value, as SkyMP's own reader does: the type is
/// chosen from the top-level `"t"` before the struct is read.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct MsgT<const N: u8>;

impl<const N: u8> MsgT<N> {
    /// The number it renders as.
    pub const VALUE: u8 = N;
}

impl<const N: u8> Serialize for MsgT<N> {
    fn serialize<S: Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        if s.is_human_readable() {
            s.serialize_u8(N)
        } else {
            s.serialize_unit()
        }
    }
}

impl<'de, const N: u8> Deserialize<'de> for MsgT<N> {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        if d.is_human_readable() {
            de::IgnoredAny::deserialize(d)?;
        } else {
            <()>::deserialize(d)?;
        }
        Ok(MsgT)
    }
}

#[cfg(feature = "arbitrary")]
impl<'a, const N: u8> arbitrary::Arbitrary<'a> for MsgT<N> {
    fn arbitrary(_: &mut arbitrary::Unstructured<'a>) -> arbitrary::Result<Self> {
        Ok(MsgT)
    }
}

/// JSON numbers are doubles on both consumers (JavaScript, simdjson's
/// `get_double`); an integer literal widens to one exactly as they would.
#[allow(clippy::as_conversions)]
fn int_as_number(v: i64) -> f64 {
    v as f64
}

#[allow(clippy::as_conversions)]
fn uint_as_number(v: u64) -> f64 {
    v as f64
}

/// `SpSnippetObjectArgument`: a form passed to a delegated Papyrus call.
#[derive(Clone, Debug, PartialEq, serde::Serialize, serde::Deserialize)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
#[serde(rename_all = "camelCase")]
pub struct SnippetObject {
    /// Form id.
    pub form_id: u64,
    /// Papyrus type name, for example `"Actor"`.
    #[serde(rename = "type")]
    pub kind: String<{ cap::PAPYRUS_NAME }>,
}

/// One argument of an `SpSnippet`: `std::variant<bool, double, std::string,
/// SpSnippetObjectArgument>`, in that alternative order.
#[derive(Clone, Debug, PartialEq)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
pub enum SnippetArg {
    /// A Papyrus Bool.
    Bool(bool),
    /// A Papyrus Int or Float, as a double.
    Number(f64),
    /// A Papyrus String.
    String(String<{ cap::SNIPPET_STRING }>),
    /// A Papyrus object.
    Object(SnippetObject),
}

/// The return value of a delegated Papyrus call: `std::variant<bool, double,
/// std::string>`, in that order.
#[derive(Clone, Debug, PartialEq)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
pub enum SnippetValue {
    /// A Papyrus Bool.
    Bool(bool),
    /// A Papyrus Int or Float, as a double.
    Number(f64),
    /// A Papyrus String.
    String(String<{ cap::SNIPPET_STRING }>),
}

/// One console command argument: `std::variant<int64_t, std::string>`.
#[derive(Clone, Debug, PartialEq, Eq)]
#[cfg_attr(feature = "arbitrary", derive(arbitrary::Arbitrary))]
pub enum ConsoleArg {
    /// A number; form ids arrive this way.
    Int(i64),
    /// Anything else.
    String(String<{ cap::CONSOLE_ARG }>),
}

/// The wire (tagged) forms, derived so the variant indexes are the
/// alternative order. Never rendered as JSON.
mod tagged {
    use super::*;

    #[derive(serde::Serialize)]
    pub enum SnippetArgRef<'a> {
        Bool(bool),
        Number(f64),
        String(&'a String<{ cap::SNIPPET_STRING }>),
        Object(&'a SnippetObject),
    }

    #[derive(serde::Deserialize)]
    pub enum SnippetArgOwned {
        Bool(bool),
        Number(f64),
        String(String<{ cap::SNIPPET_STRING }>),
        Object(SnippetObject),
    }

    #[derive(serde::Serialize)]
    pub enum SnippetValueRef<'a> {
        Bool(bool),
        Number(f64),
        String(&'a String<{ cap::SNIPPET_STRING }>),
    }

    #[derive(serde::Deserialize)]
    pub enum SnippetValueOwned {
        Bool(bool),
        Number(f64),
        String(String<{ cap::SNIPPET_STRING }>),
    }

    #[derive(serde::Serialize)]
    pub enum ConsoleArgRef<'a> {
        Int(i64),
        String(&'a String<{ cap::CONSOLE_ARG }>),
    }

    #[derive(serde::Deserialize)]
    pub enum ConsoleArgOwned {
        Int(i64),
        String(String<{ cap::CONSOLE_ARG }>),
    }
}

impl Serialize for SnippetArg {
    fn serialize<S: Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        if s.is_human_readable() {
            return match self {
                Self::Bool(v) => s.serialize_bool(*v),
                Self::Number(v) => s.serialize_f64(*v),
                Self::String(v) => v.serialize(s),
                Self::Object(v) => v.serialize(s),
            };
        }
        match self {
            Self::Bool(v) => tagged::SnippetArgRef::Bool(*v),
            Self::Number(v) => tagged::SnippetArgRef::Number(*v),
            Self::String(v) => tagged::SnippetArgRef::String(v),
            Self::Object(v) => tagged::SnippetArgRef::Object(v),
        }
        .serialize(s)
    }
}

impl<'de> Deserialize<'de> for SnippetArg {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        if !d.is_human_readable() {
            return Ok(match tagged::SnippetArgOwned::deserialize(d)? {
                tagged::SnippetArgOwned::Bool(v) => Self::Bool(v),
                tagged::SnippetArgOwned::Number(v) => Self::Number(v),
                tagged::SnippetArgOwned::String(v) => Self::String(v),
                tagged::SnippetArgOwned::Object(v) => Self::Object(v),
            });
        }
        struct V;
        impl<'de> Visitor<'de> for V {
            type Value = SnippetArg;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("a bool, a number, a string or a form object")
            }
            fn visit_bool<E: de::Error>(self, v: bool) -> Result<Self::Value, E> {
                Ok(SnippetArg::Bool(v))
            }
            fn visit_i64<E: de::Error>(self, v: i64) -> Result<Self::Value, E> {
                Ok(SnippetArg::Number(int_as_number(v)))
            }
            fn visit_u64<E: de::Error>(self, v: u64) -> Result<Self::Value, E> {
                Ok(SnippetArg::Number(uint_as_number(v)))
            }
            fn visit_f64<E: de::Error>(self, v: f64) -> Result<Self::Value, E> {
                Ok(SnippetArg::Number(v))
            }
            fn visit_str<E: de::Error>(self, v: &str) -> Result<Self::Value, E> {
                String::try_from(v)
                    .map(SnippetArg::String)
                    .map_err(|e| E::invalid_length(e.len, &self))
            }
            fn visit_map<A: MapAccess<'de>>(self, map: A) -> Result<Self::Value, A::Error> {
                SnippetObject::deserialize(de::value::MapAccessDeserializer::new(map))
                    .map(SnippetArg::Object)
            }
        }
        d.deserialize_any(V)
    }
}

impl Serialize for SnippetValue {
    fn serialize<S: Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        if s.is_human_readable() {
            return match self {
                Self::Bool(v) => s.serialize_bool(*v),
                Self::Number(v) => s.serialize_f64(*v),
                Self::String(v) => v.serialize(s),
            };
        }
        match self {
            Self::Bool(v) => tagged::SnippetValueRef::Bool(*v),
            Self::Number(v) => tagged::SnippetValueRef::Number(*v),
            Self::String(v) => tagged::SnippetValueRef::String(v),
        }
        .serialize(s)
    }
}

impl<'de> Deserialize<'de> for SnippetValue {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        if !d.is_human_readable() {
            return Ok(match tagged::SnippetValueOwned::deserialize(d)? {
                tagged::SnippetValueOwned::Bool(v) => Self::Bool(v),
                tagged::SnippetValueOwned::Number(v) => Self::Number(v),
                tagged::SnippetValueOwned::String(v) => Self::String(v),
            });
        }
        struct V;
        impl Visitor<'_> for V {
            type Value = SnippetValue;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("a bool, a number or a string")
            }
            fn visit_bool<E: de::Error>(self, v: bool) -> Result<Self::Value, E> {
                Ok(SnippetValue::Bool(v))
            }
            fn visit_i64<E: de::Error>(self, v: i64) -> Result<Self::Value, E> {
                Ok(SnippetValue::Number(int_as_number(v)))
            }
            fn visit_u64<E: de::Error>(self, v: u64) -> Result<Self::Value, E> {
                Ok(SnippetValue::Number(uint_as_number(v)))
            }
            fn visit_f64<E: de::Error>(self, v: f64) -> Result<Self::Value, E> {
                Ok(SnippetValue::Number(v))
            }
            fn visit_str<E: de::Error>(self, v: &str) -> Result<Self::Value, E> {
                String::try_from(v)
                    .map(SnippetValue::String)
                    .map_err(|e| E::invalid_length(e.len, &self))
            }
        }
        d.deserialize_any(V)
    }
}

impl Serialize for ConsoleArg {
    fn serialize<S: Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        if s.is_human_readable() {
            return match self {
                Self::Int(v) => s.serialize_i64(*v),
                Self::String(v) => v.serialize(s),
            };
        }
        match self {
            Self::Int(v) => tagged::ConsoleArgRef::Int(*v),
            Self::String(v) => tagged::ConsoleArgRef::String(v),
        }
        .serialize(s)
    }
}

impl<'de> Deserialize<'de> for ConsoleArg {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        if !d.is_human_readable() {
            return Ok(match tagged::ConsoleArgOwned::deserialize(d)? {
                tagged::ConsoleArgOwned::Int(v) => Self::Int(v),
                tagged::ConsoleArgOwned::String(v) => Self::String(v),
            });
        }
        struct V;
        impl Visitor<'_> for V {
            type Value = ConsoleArg;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                f.write_str("an integer or a string")
            }
            fn visit_i64<E: de::Error>(self, v: i64) -> Result<Self::Value, E> {
                Ok(ConsoleArg::Int(v))
            }
            fn visit_u64<E: de::Error>(self, v: u64) -> Result<Self::Value, E> {
                i64::try_from(v)
                    .map(ConsoleArg::Int)
                    .map_err(|_| E::invalid_value(de::Unexpected::Unsigned(v), &self))
            }
            fn visit_str<E: de::Error>(self, v: &str) -> Result<Self::Value, E> {
                String::try_from(v)
                    .map(ConsoleArg::String)
                    .map_err(|e| E::invalid_length(e.len, &self))
            }
        }
        d.deserialize_any(V)
    }
}
