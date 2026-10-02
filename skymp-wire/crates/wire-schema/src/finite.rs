//! Whether every float in a value is finite, for any wire type, without a
//! line of per-message code: a serde serializer that visits the value and
//! fails on the first NaN or infinity. `wire-validate` rejects such messages
//! (`E_VAL_NONFINITE`), and `wire-json` refuses to render one, since JSON has
//! no spelling for them.

use core::fmt;

use serde::ser::{self, Serialize, Serializer};

/// True when no `f32` or `f64` inside `value` is NaN or infinite.
pub fn all_finite<T: Serialize + ?Sized>(value: &T) -> bool {
    value.serialize(Probe).is_ok()
}

#[derive(Debug)]
struct NonFinite;

impl fmt::Display for NonFinite {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str("E_VAL_NONFINITE")
    }
}

impl ser::StdError for NonFinite {}

impl ser::Error for NonFinite {
    fn custom<T: fmt::Display>(_: T) -> Self {
        NonFinite
    }
}

struct Probe;

type R = Result<(), NonFinite>;

impl Serializer for Probe {
    type Ok = ();
    type Error = NonFinite;
    type SerializeSeq = Probe;
    type SerializeTuple = Probe;
    type SerializeTupleStruct = Probe;
    type SerializeTupleVariant = Probe;
    type SerializeMap = Probe;
    type SerializeStruct = Probe;
    type SerializeStructVariant = Probe;

    fn serialize_bool(self, _: bool) -> R {
        Ok(())
    }
    fn serialize_i8(self, _: i8) -> R {
        Ok(())
    }
    fn serialize_i16(self, _: i16) -> R {
        Ok(())
    }
    fn serialize_i32(self, _: i32) -> R {
        Ok(())
    }
    fn serialize_i64(self, _: i64) -> R {
        Ok(())
    }
    fn serialize_u8(self, _: u8) -> R {
        Ok(())
    }
    fn serialize_u16(self, _: u16) -> R {
        Ok(())
    }
    fn serialize_u32(self, _: u32) -> R {
        Ok(())
    }
    fn serialize_u64(self, _: u64) -> R {
        Ok(())
    }
    fn serialize_f32(self, v: f32) -> R {
        if v.is_finite() {
            Ok(())
        } else {
            Err(NonFinite)
        }
    }
    fn serialize_f64(self, v: f64) -> R {
        if v.is_finite() {
            Ok(())
        } else {
            Err(NonFinite)
        }
    }
    fn serialize_char(self, _: char) -> R {
        Ok(())
    }
    fn serialize_str(self, _: &str) -> R {
        Ok(())
    }
    fn serialize_bytes(self, _: &[u8]) -> R {
        Ok(())
    }
    fn serialize_none(self) -> R {
        Ok(())
    }
    fn serialize_some<T: Serialize + ?Sized>(self, v: &T) -> R {
        v.serialize(Probe)
    }
    fn serialize_unit(self) -> R {
        Ok(())
    }
    fn serialize_unit_struct(self, _: &'static str) -> R {
        Ok(())
    }
    fn serialize_unit_variant(self, _: &'static str, _: u32, _: &'static str) -> R {
        Ok(())
    }
    fn serialize_newtype_struct<T: Serialize + ?Sized>(self, _: &'static str, v: &T) -> R {
        v.serialize(Probe)
    }
    fn serialize_newtype_variant<T: Serialize + ?Sized>(
        self,
        _: &'static str,
        _: u32,
        _: &'static str,
        v: &T,
    ) -> R {
        v.serialize(Probe)
    }
    fn serialize_seq(self, _: Option<usize>) -> Result<Probe, NonFinite> {
        Ok(Probe)
    }
    fn serialize_tuple(self, _: usize) -> Result<Probe, NonFinite> {
        Ok(Probe)
    }
    fn serialize_tuple_struct(self, _: &'static str, _: usize) -> Result<Probe, NonFinite> {
        Ok(Probe)
    }
    fn serialize_tuple_variant(
        self,
        _: &'static str,
        _: u32,
        _: &'static str,
        _: usize,
    ) -> Result<Probe, NonFinite> {
        Ok(Probe)
    }
    fn serialize_map(self, _: Option<usize>) -> Result<Probe, NonFinite> {
        Ok(Probe)
    }
    fn serialize_struct(self, _: &'static str, _: usize) -> Result<Probe, NonFinite> {
        Ok(Probe)
    }
    fn serialize_struct_variant(
        self,
        _: &'static str,
        _: u32,
        _: &'static str,
        _: usize,
    ) -> Result<Probe, NonFinite> {
        Ok(Probe)
    }
    fn is_human_readable(&self) -> bool {
        false
    }
}

impl ser::SerializeSeq for Probe {
    type Ok = ();
    type Error = NonFinite;
    fn serialize_element<T: Serialize + ?Sized>(&mut self, v: &T) -> R {
        v.serialize(Probe)
    }
    fn end(self) -> R {
        Ok(())
    }
}

impl ser::SerializeTuple for Probe {
    type Ok = ();
    type Error = NonFinite;
    fn serialize_element<T: Serialize + ?Sized>(&mut self, v: &T) -> R {
        v.serialize(Probe)
    }
    fn end(self) -> R {
        Ok(())
    }
}

impl ser::SerializeTupleStruct for Probe {
    type Ok = ();
    type Error = NonFinite;
    fn serialize_field<T: Serialize + ?Sized>(&mut self, v: &T) -> R {
        v.serialize(Probe)
    }
    fn end(self) -> R {
        Ok(())
    }
}

impl ser::SerializeTupleVariant for Probe {
    type Ok = ();
    type Error = NonFinite;
    fn serialize_field<T: Serialize + ?Sized>(&mut self, v: &T) -> R {
        v.serialize(Probe)
    }
    fn end(self) -> R {
        Ok(())
    }
}

impl ser::SerializeMap for Probe {
    type Ok = ();
    type Error = NonFinite;
    fn serialize_key<T: Serialize + ?Sized>(&mut self, k: &T) -> R {
        k.serialize(Probe)
    }
    fn serialize_value<T: Serialize + ?Sized>(&mut self, v: &T) -> R {
        v.serialize(Probe)
    }
    fn end(self) -> R {
        Ok(())
    }
}

impl ser::SerializeStruct for Probe {
    type Ok = ();
    type Error = NonFinite;
    fn serialize_field<T: Serialize + ?Sized>(&mut self, _: &'static str, v: &T) -> R {
        v.serialize(Probe)
    }
    fn end(self) -> R {
        Ok(())
    }
}

impl ser::SerializeStructVariant for Probe {
    type Ok = ();
    type Error = NonFinite;
    fn serialize_field<T: Serialize + ?Sized>(&mut self, _: &'static str, v: &T) -> R {
        v.serialize(Probe)
    }
    fn end(self) -> R {
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::skymp;

    #[test]
    fn finds_a_nan_deep_inside() {
        let mut m = skymp::CreateActor::default();
        assert!(all_finite(&m));
        m.props.health_percentage = Some(f32::NAN);
        assert!(!all_finite(&m));
        m.props.health_percentage = Some(0.5);
        let entry = skymp::Entry {
            health: Some(f32::INFINITY),
            ..Default::default()
        };
        let mut inv = skymp::Inventory::default();
        assert!(inv.entries.push(entry).is_ok());
        m.props.inventory = Some(inv);
        assert!(!all_finite(&m));
    }
}
