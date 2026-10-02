//! Bounded collections (ADR-019). Heap backed, so a decoded message costs
//! what it carries and not its worst case, and capacity checked before any
//! growth: at decode, a length prefix or an element past `N` is an error at
//! that exact field; at construction, `push` and `TryFrom` refuse to exceed
//! `N`. These are the only collection types a wire type may hold.
//!
//! Serialized form is the plain one (a sequence, a string) in every format,
//! so the JSON at the in-process edge is an ordinary array or string.

use alloc::{string::String as StdString, vec::Vec as StdVec};
use core::fmt;
use core::marker::PhantomData;
use core::ops::Deref;

use serde::de::{self, Deserialize, Deserializer, SeqAccess, Visitor};
use serde::ser::{Serialize, Serializer};

/// A collection or string would exceed its capacity.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct CapacityError {
    /// The declared capacity.
    pub cap: usize,
    /// The length that was refused.
    pub len: usize,
}

impl fmt::Display for CapacityError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "E_WIRE_CAPACITY: {} > {}", self.len, self.cap)
    }
}

/// Elements reserved up front when a decoder announces a length; the rest
/// grow as elements actually arrive, so a short input claiming a long
/// sequence allocates for what it delivers.
const PREALLOC_MAX: usize = 64;

/// A sequence of at most `N` elements.
#[derive(Clone, PartialEq, Eq, Hash)]
pub struct Vec<T, const N: usize>(StdVec<T>);

impl<T, const N: usize> Vec<T, N> {
    /// The declared capacity.
    pub const CAPACITY: usize = N;

    /// An empty sequence.
    pub const fn new() -> Self {
        Self(StdVec::new())
    }

    /// Append one element, or refuse past the capacity.
    pub fn push(&mut self, value: T) -> Result<(), CapacityError> {
        let len = self.0.len();
        if len >= N {
            return Err(CapacityError {
                cap: N,
                len: len.saturating_add(1),
            });
        }
        self.0.push(value);
        Ok(())
    }

    /// The elements.
    pub fn as_slice(&self) -> &[T] {
        &self.0
    }

    /// The elements, owned.
    pub fn into_inner(self) -> StdVec<T> {
        self.0
    }
}

impl<T, const N: usize> Default for Vec<T, N> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T, const N: usize> Deref for Vec<T, N> {
    type Target = [T];
    fn deref(&self) -> &[T] {
        &self.0
    }
}

impl<T, const N: usize> TryFrom<StdVec<T>> for Vec<T, N> {
    type Error = CapacityError;
    fn try_from(v: StdVec<T>) -> Result<Self, CapacityError> {
        if v.len() > N {
            return Err(CapacityError {
                cap: N,
                len: v.len(),
            });
        }
        Ok(Self(v))
    }
}

impl<T: fmt::Debug, const N: usize> fmt::Debug for Vec<T, N> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

impl<T: Serialize, const N: usize> Serialize for Vec<T, N> {
    fn serialize<S: Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        self.0.serialize(s)
    }
}

impl<'de, T: Deserialize<'de>, const N: usize> Deserialize<'de> for Vec<T, N> {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        struct V<T, const N: usize>(PhantomData<T>);
        impl<'de, T: Deserialize<'de>, const N: usize> Visitor<'de> for V<T, N> {
            type Value = Vec<T, N>;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(f, "a sequence of at most {N} elements")
            }
            fn visit_seq<A: SeqAccess<'de>>(self, mut seq: A) -> Result<Self::Value, A::Error> {
                let announced = seq.size_hint();
                if let Some(n) = announced {
                    if n > N {
                        return Err(de::Error::invalid_length(n, &self));
                    }
                }
                let mut out = StdVec::with_capacity(announced.unwrap_or(0).min(N).min(PREALLOC_MAX));
                while let Some(x) = seq.next_element()? {
                    if out.len() >= N {
                        return Err(de::Error::invalid_length(N.saturating_add(1), &self));
                    }
                    out.push(x);
                }
                Ok(Vec(out))
            }
        }
        d.deserialize_seq(V::<T, N>(PhantomData))
    }
}

/// A UTF-8 string of at most `N` bytes.
#[derive(Clone, PartialEq, Eq, Hash, PartialOrd, Ord, Default)]
pub struct String<const N: usize>(StdString);

impl<const N: usize> String<N> {
    /// The declared capacity in bytes.
    pub const CAPACITY: usize = N;

    /// An empty string.
    pub const fn new() -> Self {
        Self(StdString::new())
    }

    /// The text.
    pub fn as_str(&self) -> &str {
        &self.0
    }

    /// The text, owned.
    pub fn into_inner(self) -> StdString {
        self.0
    }
}

impl<const N: usize> Deref for String<N> {
    type Target = str;
    fn deref(&self) -> &str {
        &self.0
    }
}

impl<const N: usize> TryFrom<&str> for String<N> {
    type Error = CapacityError;
    fn try_from(s: &str) -> Result<Self, CapacityError> {
        if s.len() > N {
            return Err(CapacityError {
                cap: N,
                len: s.len(),
            });
        }
        Ok(Self(StdString::from(s)))
    }
}

impl<const N: usize> TryFrom<StdString> for String<N> {
    type Error = CapacityError;
    fn try_from(s: StdString) -> Result<Self, CapacityError> {
        if s.len() > N {
            return Err(CapacityError {
                cap: N,
                len: s.len(),
            });
        }
        Ok(Self(s))
    }
}

impl<const N: usize> fmt::Debug for String<N> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

impl<const N: usize> fmt::Display for String<N> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        self.0.fmt(f)
    }
}

impl<const N: usize> Serialize for String<N> {
    fn serialize<S: Serializer>(&self, s: S) -> Result<S::Ok, S::Error> {
        s.serialize_str(&self.0)
    }
}

impl<'de, const N: usize> Deserialize<'de> for String<N> {
    fn deserialize<D: Deserializer<'de>>(d: D) -> Result<Self, D::Error> {
        struct V<const N: usize>;
        impl<const N: usize> Visitor<'_> for V<N> {
            type Value = String<N>;
            fn expecting(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
                write!(f, "a string of at most {N} bytes")
            }
            fn visit_str<E: de::Error>(self, v: &str) -> Result<Self::Value, E> {
                if v.len() > N {
                    return Err(E::invalid_length(v.len(), &self));
                }
                Ok(String(StdString::from(v)))
            }
            fn visit_string<E: de::Error>(self, v: StdString) -> Result<Self::Value, E> {
                if v.len() > N {
                    return Err(E::invalid_length(v.len(), &self));
                }
                Ok(String(v))
            }
        }
        d.deserialize_string(V::<N>)
    }
}

#[cfg(feature = "arbitrary")]
impl<'a, T: arbitrary::Arbitrary<'a>, const N: usize> arbitrary::Arbitrary<'a> for Vec<T, N> {
    fn arbitrary(u: &mut arbitrary::Unstructured<'a>) -> arbitrary::Result<Self> {
        let len = u.int_in_range(0..=N.min(16))?;
        let mut out = StdVec::with_capacity(len);
        for _ in 0..len {
            out.push(T::arbitrary(u)?);
        }
        Ok(Self(out))
    }
}

#[cfg(feature = "arbitrary")]
impl<'a, const N: usize> arbitrary::Arbitrary<'a> for String<N> {
    fn arbitrary(u: &mut arbitrary::Unstructured<'a>) -> arbitrary::Result<Self> {
        let s = <&str as arbitrary::Arbitrary>::arbitrary(u)?;
        let mut end = s.len().min(N);
        while !s.is_char_boundary(end) {
            end = end.saturating_sub(1);
        }
        Ok(Self(StdString::from(s.get(..end).unwrap_or(""))))
    }
}
