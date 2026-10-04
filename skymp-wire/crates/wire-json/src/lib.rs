//! SkyMP's JSON form at the in-process edge (ADR-019). On the wire a message
//! is postcard; in-process, the C++ core (MessageSerializer's JSON path,
//! simdjson in, nlohmann out) and skymp5-client (JSON.parse, JSON.stringify)
//! speak the JSON that SkyMP's JsonOutputArchive writes. This crate is the
//! only translation between the two, so nothing but Rust ever reads a byte a
//! peer sent:
//!
//! - [`render`]: a decoded, validated message to JSON text. Absent optionals
//!   are omitted (JsonOutputArchive writes no key, not null), `"t"` is the
//!   MsgType, keys come out sorted, as nlohmann's do.
//! - [`recognize`]: JSON text from the C++ core or the client to a typed
//!   message, choosing the type by `"t"`. Unknown keys are ignored and a
//!   required key missing is an error, as SimdJsonInputArchive does; a null
//!   optional reads as absent (the C++ reader would throw, and the old
//!   fakeclient sends one). Every capacity applies.
//!
//! Only the SkyMP family has a JSON form; the M0 variants are refused.

use serde_json::Value;
use wire_schema::Message;

/// Longest JSON text accepted or produced. The longest postcard message is
/// `Message::MAX_ENCODED_LEN`; JSON spells numbers and keys out, so the text
/// may run a few times longer.
pub const MAX_JSON_LEN: usize = 4 * Message::MAX_ENCODED_LEN;

/// Why a JSON text could not be recognized or a message rendered. The names
/// are stable and greppable in lab logs.
#[derive(Debug, Clone, thiserror::Error, PartialEq, Eq)]
pub enum JsonError {
    /// Longer than [`MAX_JSON_LEN`]; refused before parsing.
    #[error("E_JSON_TOO_LONG: {len} > {max}")]
    TooLong {
        /// Length received.
        len: usize,
        /// [`MAX_JSON_LEN`].
        max: usize,
    },
    /// Not JSON, or not UTF-8.
    #[error("E_JSON_SYNTAX: {0}")]
    Syntax(String),
    /// Not an object with an integer `"t"`.
    #[error("E_JSON_NO_TYPE")]
    NoType,
    /// A `"t"` that names no SkyMP message.
    #[error("E_JSON_UNKNOWN_TYPE: {0}")]
    UnknownType(u64),
    /// The fields do not fit the message: a key missing, a wrong type, a
    /// capacity exceeded.
    #[error("E_JSON_SHAPE: {0}")]
    Shape(String),
    /// An M0 message, which has no SkyMP JSON form.
    #[error("E_JSON_NO_FORM: {0}")]
    NoForm(&'static str),
    /// A NaN or an infinity, which JSON cannot spell.
    #[error("E_JSON_NONFINITE")]
    NonFinite,
}

/// The SkyMP family, MsgType and variant, once, for both directions.
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
            32 UpdateGamemodeData, 33 CreateActor, 34 SetGameTime, 35 RestIntent
        }
    };
}

/// The JSON value of a SkyMP message, absent optionals already removed.
pub fn render_value(msg: &Message) -> Result<Value, JsonError> {
    if !wire_schema::finite::all_finite(msg) {
        return Err(JsonError::NonFinite);
    }
    macro_rules! arms {
        ($($t:literal $name:ident),*) => {
            match msg {
                $(Message::$name(m) => serde_json::to_value(m),)*
                other => return Err(JsonError::NoForm(other.name())),
            }
        };
    }
    let mut v = skymp_family!(arms).map_err(|e| JsonError::Shape(e.to_string()))?;
    strip_absent(&mut v);
    Ok(v)
}

/// The JSON text of a SkyMP message.
pub fn render(msg: &Message) -> Result<String, JsonError> {
    let text = serde_json::to_string(&render_value(msg)?)
        .map_err(|e| JsonError::Shape(e.to_string()))?;
    if text.len() > MAX_JSON_LEN {
        return Err(JsonError::TooLong {
            len: text.len(),
            max: MAX_JSON_LEN,
        });
    }
    Ok(text)
}

/// Remove null object members, recursively. JsonOutputArchive writes no key
/// for an absent optional, and no member it writes is otherwise null; array
/// elements stay (an absent SpSnippet argument is a null element there too).
fn strip_absent(v: &mut Value) {
    match v {
        Value::Object(map) => {
            map.retain(|_, x| !x.is_null());
            for x in map.values_mut() {
                strip_absent(x);
            }
        }
        Value::Array(items) => {
            for x in items.iter_mut() {
                strip_absent(x);
            }
        }
        _ => {}
    }
}

/// A SkyMP message from JSON text.
pub fn recognize(text: &str) -> Result<Message, JsonError> {
    if text.len() > MAX_JSON_LEN {
        return Err(JsonError::TooLong {
            len: text.len(),
            max: MAX_JSON_LEN,
        });
    }
    let value: Value =
        serde_json::from_str(text).map_err(|e| JsonError::Syntax(e.to_string()))?;
    recognize_value(value)
}

/// A SkyMP message from JSON bytes, which must be UTF-8.
pub fn recognize_bytes(bytes: &[u8]) -> Result<Message, JsonError> {
    let text = std::str::from_utf8(bytes).map_err(|e| JsonError::Syntax(e.to_string()))?;
    recognize(text)
}

/// A SkyMP message from a parsed JSON value.
pub fn recognize_value(value: Value) -> Result<Message, JsonError> {
    let t = value
        .as_object()
        .and_then(|o| o.get("t"))
        .and_then(Value::as_u64)
        .ok_or(JsonError::NoType)?;
    macro_rules! arms {
        ($($t:literal $name:ident),*) => {
            match t {
                $($t => serde_json::from_value(value).map(Message::$name),)*
                other => return Err(JsonError::UnknownType(other)),
            }
        };
    }
    skymp_family!(arms).map_err(|e| JsonError::Shape(e.to_string()))
}

#[cfg(test)]
#[allow(clippy::panic, clippy::indexing_slicing)] // serde_json Value indexing returns Null, it never panics
mod tests {
    use super::*;
    use wire_schema::skymp;

    fn movement() -> Message {
        Message::UpdateMovement(skymp::UpdateMovement {
            t: skymp::MsgT,
            idx: 7,
            data: skymp::MovementData {
                world_or_cell: 0x3c,
                pos: [133857.0, -61130.0, 14662.0],
                rot: [0.0, 0.0, 72.0],
                direction: 0.0,
                health_percentage: 1.0,
                speed: 0.0,
                run_mode: "Standing".try_into().unwrap_or_default(),
                is_in_jump_state: false,
                is_sneaking: false,
                is_blocking: false,
                is_weap_drawn: false,
                is_dead: false,
                look_at: None,
            },
        })
    }

    #[test]
    fn movement_renders_as_skymp_writes_it() {
        let text = render(&movement()).unwrap_or_default();
        let v: Value = serde_json::from_str(&text).unwrap_or(Value::Null);
        assert_eq!(v["t"], 2);
        assert_eq!(v["idx"], 7);
        assert_eq!(v["data"]["runMode"], "Standing");
        assert_eq!(v["data"]["worldOrCell"], 0x3c);
        assert_eq!(v["data"]["pos"][1], -61130.0);
        assert!(v["data"].get("lookAt").is_none(), "an absent optional is no key: {text}");
        assert!(!text.contains("null"), "{text}");
    }

    #[test]
    fn round_trip_through_json() {
        let m = movement();
        assert_eq!(recognize(&render(&m).unwrap_or_default()), Ok(m));
    }

    #[test]
    fn the_nested_t_quirks_render() {
        let m = Message::Activate(skymp::Activate {
            t: skymp::MsgT,
            data: skymp::ActivateData {
                t: skymp::MsgT,
                caster: 0x14,
                target: 0xff00_0001,
                is_second_activation: false,
            },
        });
        let v = render_value(&m).unwrap_or(Value::Null);
        assert_eq!(v["t"], 6);
        assert_eq!(v["data"]["t"], 6);
        let d = Message::DeathStateContainer(skymp::DeathStateContainer {
            t: skymp::MsgT,
            t_teleport: Some(skymp::Teleport::default()),
            t_change_values: None,
            t_is_dead: None,
        });
        let v = render_value(&d).unwrap_or(Value::Null);
        assert_eq!(v["t"], 18);
        assert_eq!(v["tTeleport"]["t"], 20);
        assert!(v.get("tChangeValues").is_none());
    }

    #[test]
    fn what_the_old_fakeclient_sends_is_recognized() {
        // fakeclient/main.cpp: the login, an AddItem, a FinishSpSnippet with a null result
        let login = r#"{"t":1,"contentJsonDump":"{\"customPacketType\":\"loginWithSkympIo\",\"gameData\":{\"profileId\":1}}"}"#;
        assert!(matches!(recognize(login), Ok(Message::CustomPacket(_))));
        let add = r#"{"t":12,"data":{"commandName":"AddItem","args":[20,77495,1]}}"#;
        match recognize(add) {
            Ok(Message::ConsoleCommand(c)) => {
                assert_eq!(c.data.command_name.as_str(), "AddItem");
                assert_eq!(c.data.args.first(), Some(&skymp::ConsoleArg::Int(20)));
            }
            other => panic!("{other:?}"),
        }
        let fin = r#"{"t":10,"returnValue":null,"snippetIdx":3}"#;
        assert!(matches!(recognize(fin), Ok(Message::FinishSpSnippet(f)) if f.return_value.is_none() && f.snippet_idx == 3));
        let mv = r#"{"t":2,"idx":1,"data":{"worldOrCell":60,"pos":[1,2,3],"rot":[0,0,0],"direction":0.0,"healthPercentage":1.0,"speed":0.0,"runMode":"Walking","isInJumpState":false,"isSneaking":false,"isBlocking":false,"isWeapDrawn":false,"isDead":false}}"#;
        assert!(matches!(recognize(mv), Ok(Message::UpdateMovement(m)) if m.data.look_at.is_none() && m.data.pos == [1.0, 2.0, 3.0]));
    }

    #[test]
    fn untagged_variants_resolve_in_alternative_order() {
        let s = r#"{"t":30,"class":"Debug","function":"Notification","arguments":[true,1.5,7,"hi",{"formId":20,"type":"Actor"},null],"selfId":0,"snippetIdx":4}"#;
        let m = recognize(s);
        match &m {
            Ok(Message::SpSnippet(sp)) => {
                let a = sp.arguments.as_slice();
                assert_eq!(a.first(), Some(&Some(skymp::SnippetArg::Bool(true))));
                assert_eq!(a.get(1), Some(&Some(skymp::SnippetArg::Number(1.5))));
                assert_eq!(a.get(2), Some(&Some(skymp::SnippetArg::Number(7.0))));
                assert!(matches!(a.get(3), Some(Some(skymp::SnippetArg::String(s))) if s.as_str() == "hi"));
                assert!(matches!(a.get(4), Some(Some(skymp::SnippetArg::Object(o))) if o.form_id == 20 && o.kind.as_str() == "Actor"));
                assert_eq!(a.get(5), Some(&None));
            }
            other => panic!("{other:?}"),
        }
        // the null element survives rendering: only object members are stripped
        let text = m.as_ref().map(render).ok().and_then(Result::ok).unwrap_or_default();
        assert!(text.contains(",null]"), "{text}");
    }

    #[test]
    fn errors_have_their_codes() {
        assert_eq!(recognize("[1]"), Err(JsonError::NoType));
        assert_eq!(recognize(r#"{"t":"2"}"#), Err(JsonError::NoType));
        assert_eq!(recognize(r#"{"t":99}"#), Err(JsonError::UnknownType(99)));
        assert!(matches!(recognize("{"), Err(JsonError::Syntax(_))));
        assert!(matches!(recognize(r#"{"t":25}"#), Err(JsonError::Shape(_))), "idx is required");
        let long_mode = "x".repeat(wire_schema::cap::RUN_MODE + 1);
        let over = format!(r#"{{"t":2,"idx":1,"data":{{"worldOrCell":60,"pos":[1,2,3],"rot":[0,0,0],"direction":0,"healthPercentage":1,"speed":0,"runMode":"{long_mode}","isInJumpState":false,"isSneaking":false,"isBlocking":false,"isWeapDrawn":false,"isDead":false}}}}"#);
        assert!(matches!(recognize(&over), Err(JsonError::Shape(_))), "capacity");
        assert_eq!(render(&Message::Refuse { reason: 1 }), Err(JsonError::NoForm("Refuse")));
        let mut m = movement();
        if let Message::UpdateMovement(u) = &mut m {
            u.data.speed = f32::NAN;
        }
        assert_eq!(render(&m), Err(JsonError::NonFinite));
    }

    #[test]
    fn doubles_round_trip_exactly() {
        // fuzz/validate found it: serde_json's default float parser is not
        // correctly rounded, and this argument came back one digit off; the
        // workspace turns on serde_json's float_roundtrip, which is
        for x in [1.928_591_755_796_954_1e-168, 3.046_446_040_377_607_6e299, 0.1, -0.0, 5e-324, f64::MAX] {
            let m = Message::SpSnippet(skymp::SpSnippet {
                arguments: [Some(skymp::SnippetArg::Number(x))].to_vec().try_into().unwrap_or_default(),
                ..Default::default()
            });
            let text = render(&m).unwrap_or_default();
            assert_eq!(recognize(&text), Ok(m), "{text}");
        }
    }

    fn fixtures_dir() -> std::path::PathBuf {
        std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("fixtures")
    }

    /// The shared JSON contract: every fixture recognizes and renders back
    /// to the same JSON. The C++ core's unit test (unit/WireJsonContractTest)
    /// reads the same files through ReadJson and WriteJson and must do the
    /// same, so the two sides agree on every message's shape.
    #[test]
    fn fixtures_round_trip() {
        let mut n = 0;
        let mut types = std::collections::BTreeSet::new();
        for entry in std::fs::read_dir(fixtures_dir()).into_iter().flatten().flatten() {
            let path = entry.path();
            if path.extension().and_then(|e| e.to_str()) != Some("json") {
                continue;
            }
            let text = std::fs::read_to_string(&path).unwrap_or_default();
            let m = recognize(&text).unwrap_or_else(|e| panic!("{}: {e}", path.display()));
            types.insert(m.msg_type());
            let again: Value = serde_json::from_str(&render(&m).unwrap_or_default()).unwrap_or(Value::Null);
            let want: Value = serde_json::from_str(&text).unwrap_or(Value::Null);
            assert_eq!(again, want, "{}", path.display());
            n += 1;
        }
        assert!(n >= 70, "{n} fixtures");
        assert_eq!(types.len(), 35, "every SkyMP type has fixtures");
    }

    /// Writes the fixtures: two per SkyMP type, from arbitrary values with
    /// fixed seeds, finite floats only. Run by hand when the schema grows:
    /// `cargo test -p wire-json -- --ignored write_fixtures`.
    #[test]
    #[ignore]
    #[allow(clippy::arithmetic_side_effects, clippy::as_conversions)]
    fn write_fixtures() {
        use arbitrary::{Arbitrary, Unstructured};
        let dir = fixtures_dir();
        std::fs::create_dir_all(&dir).unwrap_or_default();
        macro_rules! each {
            ($($t:literal $name:ident),*) => {
                $(
                    let mut written = 0;
                    let mut seed: u64 = $t;
                    while written < 2 {
                        seed = seed.wrapping_mul(6364136223846793005).wrapping_add(1442695040888963407);
                        let bytes: Vec<u8> = (0..4096u64).map(|i| ((seed.wrapping_add(i).wrapping_mul(0x9E3779B97F4A7C15)) >> 56) as u8).collect();
                        let mut u = Unstructured::new(&bytes);
                        let Ok(v) = skymp::$name::arbitrary(&mut u) else { continue };
                        let m = Message::$name(v);
                        let Ok(text) = render(&m) else { continue }; // non-finite: try another seed
                        let pretty = serde_json::to_string_pretty(&serde_json::from_str::<Value>(&text).unwrap_or(Value::Null)).unwrap_or_default();
                        std::fs::write(dir.join(format!("{:02}-{}-{}.json", $t, stringify!($name), written)), pretty + "\n").unwrap_or_default();
                        written += 1;
                    }
                )*
            };
        }
        skymp_family!(each);
    }

    #[test]
    fn every_skymp_type_has_a_json_form() {
        macro_rules! each {
            ($($t:literal $name:ident),*) => {
                $(
                    let m = Message::$name(skymp::$name::default());
                    let text = render(&m).unwrap_or_default();
                    assert_eq!(recognize(&text).as_ref().map(Message::msg_type), Ok(Some($t)), "{}", stringify!($name));
                )*
            };
        }
        skymp_family!(each);
    }
}
