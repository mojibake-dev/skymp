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
            32 UpdateGamemodeData, 33 CreateActor, 34 SetGameTime, 35 RestIntent,
            36 MapMarkerDiscovered, 37 IngredientEffectsKnown, 38 Favorites,
            39 RaceMenuPreset, 40 ActorValues, 41 ConsoleOutput, 42 ArrowShot,
            43 MagicEffects, 44 UpdateAction, 45 PlayerGlobals, 46 ActorJump,
            47 CellLoaded, 48 HostGrant, 49 HostRelease, 50 ActorSightings,
            51 ActorFate, 52 ActorDeath, 53 HostedActorState, 54 ActorSpeech, 55 DamageCause, 56 ActorClaim, 57 QuestStage,
            58 PropTransform, 59 PropClaim, 60 PropOwner, 61 PlacedRequest, 62 PlacedReport, 63 SkillUse, 64 LockReport, 65 LockPrompt, 66 SetWeather, 67 PerkPick, 68 PerkState, 69 WordWall, 70 RelayedActivation, 71 ObjectAnimation,
            72 ChangeForm, 73 ChangeFormAck, 74 ChangeFormApply, 75 ScriptCall,
            76 ScriptState, 77 ScriptStateApply, 78 DestructionStage, 79 AliasFill,
            80 WorldGlobal
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
    /// thuum docs/verbs/npc-stream.md (slice 3): an equipment entry's count is
    /// u32 on the wire, so the -1 an ExtraCount delta once produced (docs/
    /// LOG.md 2026-10-22) is refused at the shape and never carried; a worn
    /// entry of one is the shape a host sends for an actor it simulates.
    #[test]
    fn an_equipment_count_below_one_is_no_shape() {
        let refused = r#"{"t":5,"idx":3,"data":{"numChanges":1,"inv":{"entries":[{"baseId":77495,"count":-1}]}}}"#;
        assert!(matches!(recognize(refused), Err(JsonError::Shape(_))));
        let worn = r#"{"t":5,"idx":3,"data":{"numChanges":1,"inv":{"entries":[{"baseId":77495,"count":1,"worn":true}]}}}"#;
        assert!(recognize(worn).is_ok());
    }

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
        assert!(n >= 90, "{n} fixtures");
        assert_eq!(types.len(), 80, "every SkyMP type has fixtures");
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

    /// thuum docs/verbs/change-form.md (ADR-065): the ChangeForm fixtures
    /// are the contract's refusals as well as its shapes. 0 and 1 are
    /// records the validator takes (an ACHR with one remap row, the lab's
    /// own REFR record); 2 to 6 each break one rule and name its code, so
    /// the C++ contract test reads the refused shapes too and the client's
    /// agent can replay them against its own encoder.
    #[test]
    fn change_form_fixtures_validate_as_named() {
        use wire_validate::{validate_server, Reject};
        let verdicts: [(&str, Result<(), Reject>); 7] = [
            ("72-ChangeForm-0.json", Ok(())),
            ("72-ChangeForm-1.json", Ok(())),
            ("72-ChangeForm-2.json", Err(Reject::ChangeFormRange)),
            ("72-ChangeForm-3.json", Err(Reject::ChangeFormHeader)),
            ("72-ChangeForm-4.json", Err(Reject::ChangeFormHash)),
            ("72-ChangeForm-5.json", Err(Reject::ChangeFormRemap)),
            ("72-ChangeForm-6.json", Err(Reject::ChangeFormSize)),
        ];
        for (name, want) in verdicts {
            let text = std::fs::read_to_string(fixtures_dir().join(name)).unwrap_or_default();
            let m = recognize(&text).unwrap_or_else(|e| panic!("{name}: {e}"));
            assert_eq!(validate_server(&m), want, "{name}");
        }
        for name in ["73-ChangeFormAck-0.json", "73-ChangeFormAck-1.json", "74-ChangeFormApply-0.json", "74-ChangeFormApply-1.json"] {
            let text = std::fs::read_to_string(fixtures_dir().join(name)).unwrap_or_default();
            let m = recognize(&text).unwrap_or_else(|e| panic!("{name}: {e}"));
            assert_eq!(validate_server(&m), Ok(()), "{name}");
        }
    }

    /// thuum ADR-068 (the one seam): the ScriptCall fixtures are the
    /// contract's shapes and its refusals, as the ChangeForm ones are. 0 is
    /// the puzzle pillar's turn on Bleak Falls Temple's 0xaa8cb (a method
    /// native, latent, two strings; docs/verbs/traps.md's census of
    /// defaultPuzzlePillarScript), 1 a static native on a world object (the
    /// subject first among the arguments and named again as `selfId`); 2 and
    /// 3 each name a form the server names (an object argument, a subject at
    /// 0xff000000 and up) and are refused by the shape. The SpSnippet
    /// fixture 2 is the pillar's relay with the subject's sequence; 0 and 1,
    /// the snippets without one, render no `seq`.
    #[test]
    fn script_call_fixtures_validate_as_named() {
        use wire_validate::{validate_server, Reject};
        let verdicts: [(&str, Result<(), Reject>); 4] = [
            ("75-ScriptCall-0.json", Ok(())),
            ("75-ScriptCall-1.json", Ok(())),
            ("75-ScriptCall-2.json", Err(Reject::Range)),
            ("75-ScriptCall-3.json", Err(Reject::Range)),
        ];
        for (name, want) in verdicts {
            let text = std::fs::read_to_string(fixtures_dir().join(name)).unwrap_or_default();
            let m = recognize(&text).unwrap_or_else(|e| panic!("{name}: {e}"));
            assert!(matches!(m, Message::ScriptCall(_)), "{name}");
            assert_eq!(validate_server(&m), want, "{name}");
        }
        let text = std::fs::read_to_string(fixtures_dir().join("30-SpSnippet-2.json")).unwrap_or_default();
        match recognize(&text) {
            Ok(Message::SpSnippet(s)) => assert_eq!(s.seq, Some(1)),
            other => panic!("{other:?}"),
        }
        for name in ["30-SpSnippet-0.json", "30-SpSnippet-1.json"] {
            let text = std::fs::read_to_string(fixtures_dir().join(name)).unwrap_or_default();
            let m = recognize(&text).unwrap_or_else(|e| panic!("{name}: {e}"));
            assert!(matches!(&m, Message::SpSnippet(s) if s.seq.is_none()), "{name}");
            assert!(!render(&m).unwrap_or_default().contains("\"seq\""), "{name}: an absent sequence is no key");
        }
    }

    /// thuum D-S3 (ADR-068's DestructionStage row): one fixture each way.
    /// 0 is a seat's report, Bleak Falls Temple's oil lamp 0x9b0fb at its
    /// first stage (the census's TrapFallingOilLampColLayer, its first shot's
    /// DamageObject); 1 the server's record served behind a create word,
    /// Embershard Mine's tripwire 0xb6d3f spent (SetDestroyed true after its
    /// firing, docs/research/trap-census-2026-11-05.md). Named by MsgType,
    /// 78, as every fixture is.
    #[test]
    fn destruction_stage_fixtures_validate_as_named() {
        // thuum D-S3's two (a seat's report, a served destruction) with
        // rung 4's sequence of 0, then the ref state's: 78-2 the server's
        // served word for Bleak Falls Temple's cobweb 0x1fc43, disabled at
        // the object's sequence 3, and 78-3 a seat's report carrying the
        // server's words: shapes either side may hold, the ref state's words
        // refused from a client at the edge
        use wire_validate::{validate, validate_server, ClientGuard, Reject};
        type Want = (&'static str, u32, u8, bool, bool, Option<bool>, Option<bool>, u32, Result<(), Reject>);
        let wants: [Want; 4] = [
            ("78-DestructionStage-0.json", 0x9b0fb, 1, false, false, None, None, 0, Ok(())),
            ("78-DestructionStage-1.json", 0xb6d3f, 0, true, true, None, None, 0, Ok(())),
            ("78-DestructionStage-2.json", 0x1fc43, 0, false, true, Some(true), None, 3, Err(Reject::Range)),
            ("78-DestructionStage-3.json", 0x1fc43, 0, false, false, Some(true), Some(false), 1, Err(Reject::Range)),
        ];
        for (name, refr, stage, destroyed, served, disabled, deleted, seq, from_client) in wants {
            let text = std::fs::read_to_string(fixtures_dir().join(name)).unwrap_or_default();
            match recognize(&text) {
                Ok(m @ Message::DestructionStage(_)) => {
                    assert_eq!(validate_server(&m), Ok(()), "{name}");
                    assert_eq!(validate(&m, &mut ClientGuard::default(), 0), from_client, "{name}");
                    if let Message::DestructionStage(d) = m {
                        assert_eq!((d.refr, d.stage, d.destroyed, d.served, d.disabled, d.deleted, d.seq), (refr, stage, destroyed, served, disabled, deleted, seq), "{name}");
                    }
                }
                other => panic!("{name}: {other:?}"),
            }
        }
    }

    /// thuum ADR-068 rung 4: the AliasFill and WorldGlobal fixtures, shapes
    /// and refusals. 79-0 a relay of MS13 0x39645's ArvelWeb alias 10, forced
    /// to the web 0x1fc43 (Skyrim.esm, lab/esm.py 2026-11-06); 79-1 its alias
    /// 3 served cleared; 79-2 a fill naming a server form, refused. 80-0 the
    /// streamer's report at epoch 2 of DA04MinLevel 0xe316c (a Skyrim.esm
    /// GLOB, lab/esm.py), 80-1 the stream word at epoch 3 with its empty
    /// entries, 80-2 a global named twice, refused (JSON cannot spell the
    /// NaN the plan named; the unit tests carry it).
    #[test]
    fn alias_fill_and_world_global_fixtures_validate_as_named() {
        use wire_validate::{validate, validate_server, ClientGuard, Reject};
        type Verdict = (&'static str, Result<(), Reject>, Result<(), Reject>);
        let verdicts: [Verdict; 6] = [
            ("79-AliasFill-0.json", Ok(()), Ok(())),
            ("79-AliasFill-1.json", Ok(()), Err(Reject::Range)),
            ("79-AliasFill-2.json", Err(Reject::Range), Err(Reject::Range)),
            ("80-WorldGlobal-0.json", Ok(()), Ok(())),
            ("80-WorldGlobal-1.json", Ok(()), Err(Reject::Range)),
            ("80-WorldGlobal-2.json", Err(Reject::Range), Err(Reject::Range)),
        ];
        for (name, shape, from_client) in verdicts {
            let text = std::fs::read_to_string(fixtures_dir().join(name)).unwrap_or_default();
            let m = recognize(&text).unwrap_or_else(|e| panic!("{name}: {e}"));
            assert_eq!(validate_server(&m), shape, "{name}");
            assert_eq!(validate(&m, &mut ClientGuard::default(), 0), from_client, "{name}");
        }
        // the stream word's entries are present though empty
        let text = std::fs::read_to_string(fixtures_dir().join("80-WorldGlobal-1.json")).unwrap_or_default();
        let m = recognize(&text).unwrap_or_else(|e| panic!("80-WorldGlobal-1.json: {e}"));
        let rendered: Value = serde_json::from_str(&render(&m).unwrap_or_default()).unwrap_or(Value::Null);
        assert_eq!(rendered.get("entries"), Some(&Value::Array(Vec::new())));
        assert_eq!(rendered.get("stream"), Some(&Value::Bool(true)));
        // a report carries no stream key
        let text = std::fs::read_to_string(fixtures_dir().join("80-WorldGlobal-0.json")).unwrap_or_default();
        let m = recognize(&text).unwrap_or_else(|e| panic!("80-WorldGlobal-0.json: {e}"));
        let rendered: Value = serde_json::from_str(&render(&m).unwrap_or_default()).unwrap_or(Value::Null);
        assert_eq!(rendered.get("stream"), None);
    }

    /// thuum ADR-068 rung 4: a script set's placement names its reporter on
    /// a STAT (33-CreateActor-3: an XMarker 0x3b placed at Bleak Falls
    /// Temple's cobweb, the reporter 0xff000000), a pairing the server sends
    #[test]
    fn a_script_placements_create_word_validates() {
        use wire_validate::validate_server;
        let text = std::fs::read_to_string(fixtures_dir().join("33-CreateActor-3.json")).unwrap_or_default();
        let m = recognize(&text).unwrap_or_else(|e| panic!("33-CreateActor-3.json: {e}"));
        assert_eq!(validate_server(&m), Ok(()));
        match m {
            Message::CreateActor(c) => {
                assert_eq!((c.props.placed_by, c.base_record_type.as_ref().map(|t| t.as_str())), (Some(0xff00_0000), Some("STAT")));
            }
            other => panic!("not a CreateActor: {other:?}"),
        }
    }

    /// thuum ADR-068 rung 3 (the script record): the ScriptState and
    /// ScriptStateApply fixtures are the contract's shapes and one refusal,
    /// as the ChangeForm ones are. 76-0 is the Temple pillar 0xaa8cb's
    /// record as its host reads it after a turn to position02, its
    /// defaultPuzzlePillarScript with the census's five variables (raw
    /// types 3, 3, 5, a class, 5; docs/verbs/papyrus-events.md), the class
    /// one the puzzle lever 0xaa8c3's plain form handle (kind 0, mid
    /// 0xFFFF); 76-1 the same record with the handle's kind 3, which no
    /// handle has (E_VAL_RANGE); 77-0 the pillar's record served with its
    /// view, Trigger01, at the object's sequence 2, in position01; 77-1 the
    /// lever's own defaultPillarPuzzleLever (state Waiting) served with no
    /// view and sequence 0, since no animation native runs on the lever
    /// (docs/research/trap-census-2026-11-05.md), its handles the puzzle
    /// door's marker 0x3e728, the portcullis 0x3b9f1 and a failure dart
    /// 0x3720f (docs/verbs/traps.md's read of its properties).
    #[test]
    fn script_state_fixtures_validate_as_named() {
        use wire_validate::{validate_server, Reject};
        let read = |name: &str| std::fs::read_to_string(fixtures_dir().join(name)).unwrap_or_default();
        let verdicts: [(&str, Result<(), Reject>); 4] = [
            ("76-ScriptState-0.json", Ok(())),
            ("76-ScriptState-1.json", Err(Reject::Range)),
            ("77-ScriptStateApply-0.json", Ok(())),
            ("77-ScriptStateApply-1.json", Ok(())),
        ];
        for (name, want) in verdicts {
            let m = recognize(&read(name)).unwrap_or_else(|e| panic!("{name}: {e}"));
            assert!(matches!(m, Message::ScriptState(_) | Message::ScriptStateApply(_)), "{name}");
            assert_eq!(validate_server(&m), want, "{name}");
        }
        // the record's names as the wire spells them: formId, type, k, o
        match recognize(&read("76-ScriptState-0.json")) {
            Ok(Message::ScriptState(r)) => {
                assert_eq!((r.epoch, r.form_id, r.scripts.len()), (1, 0xaa8cb, 1));
                let pillar = r.scripts.first();
                assert_eq!(pillar.map(|p| (p.cls.as_str(), p.state.as_str(), p.flags, p.vars.len())), Some(("defaultPuzzlePillarScript", "position02", 11, 5)));
                let handle = pillar.and_then(|p| p.vars.get(3)).and_then(|v| v.o.as_ref());
                assert_eq!(handle.map(|h| (h.kind, h.mid, h.form, h.cls.as_str())), Some((0, 0xffff, 0xaa8c3, "defaultPillarPuzzleLever")));
            }
            other => panic!("{other:?}"),
        }
        // the served pillar's sequence and view; the lever's no view, which
        // renders no key
        match recognize(&read("77-ScriptStateApply-0.json")) {
            Ok(Message::ScriptStateApply(a)) => {
                assert_eq!((a.form_id, a.seq, a.event.as_ref().map(|e| e.as_str())), (0xaa8cb, 2, Some("Trigger01")));
            }
            other => panic!("{other:?}"),
        }
        let lever = recognize(&read("77-ScriptStateApply-1.json"));
        match &lever {
            Ok(Message::ScriptStateApply(a)) => {
                assert_eq!((a.form_id, a.seq, a.event.is_none(), a.scripts.first().map(|s| s.vars.len())), (0xaa8c3, 0, true, Some(6)));
            }
            other => panic!("{other:?}"),
        }
        let rendered = lever.as_ref().map(render).ok().and_then(Result::ok).unwrap_or_default();
        assert!(!rendered.is_empty() && !rendered.contains("\"event\""), "{rendered}");
        // a null view reads as none, the client's spelling of no view
        let null = read("77-ScriptStateApply-1.json").replacen("\"fnv1a\"", "\"event\": null, \"fnv1a\"", 1);
        assert!(matches!(recognize(&null), Ok(Message::ScriptStateApply(a)) if a.event.is_none()));
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
