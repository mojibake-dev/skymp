//! A player's RaceMenu look (thuum docs/verbs/racemenu-sync.md): the preset
//! RaceMenu saves, JSON text, which the server records as the player's
//! client reports it (R2) and hands to every client that shows the player.
//! The server cannot check a look against the engine (it has no meshes, no
//! .tri files), so it bounds what it records: a JSON object, nested no
//! deeper than a preset's own structure needs, within the wire's capacity
//! (wire-schema `cap::RACEMENU_PRESET`, already enforced at decode).

use serde_json::Value;

/// How deep a preset's JSON may nest. RaceMenu's own presets nest a handful
/// of levels (a sculpt's vertices sit inside a head part's entry inside the
/// sculpt list inside the actor); 32 leaves room and stops a pathological
/// nesting before anything walks it.
pub const MAX_DEPTH: usize = 32;

/// Whether `text` is a preset the server keeps: a JSON object at the top,
/// nested at most [`MAX_DEPTH`] deep.
pub fn preset_ok(text: &str) -> bool {
    // serde_json refuses input nested past its own recursion limit (128)
    // with an error, so parsing never overflows the stack
    match serde_json::from_str::<Value>(text) {
        Ok(v @ Value::Object(_)) => depth(&v) <= MAX_DEPTH,
        _ => false,
    }
}

fn depth(v: &Value) -> usize {
    let inner = match v {
        Value::Array(items) => items.iter().map(depth).max().unwrap_or(0),
        Value::Object(map) => map.values().map(depth).max().unwrap_or(0),
        _ => return 0,
    };
    inner.saturating_add(1)
}

/// A form a look names: RaceMenu's `"<plugin>|<hex id>"`, the id within its
/// plugin (24 bits).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FormRef {
    /// The plugin's file name, as in the load order.
    pub plugin: String,
    /// The form's id within the plugin.
    pub id: u32,
}

/// What a look says of the vanilla appearance (thuum ADR-026): the head parts
/// in the look's order, the hair colour as 0xRRGGBB, the weight, the face's
/// texture set. Each optional part is absent when the look does not carry it
/// or carries it in a shape RaceMenu does not write.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct LookFacts {
    /// `headParts[].formIdentifier`.
    pub parts: Vec<FormRef>,
    /// `actor.hairColor`, 24 bits.
    pub hair_color: Option<u32>,
    /// `actor.weight`, 0 to 100.
    pub weight: Option<f32>,
    /// `actor.headTexture`.
    pub head_texture: Option<FormRef>,
}

/// How many head parts a look may name; RaceMenu writes one per type the
/// actor wears (eight or so).
pub const MAX_PARTS: usize = 64;

/// A weight within 0 to 100 as the engine's single precision.
#[allow(clippy::as_conversions)]
fn narrow(x: f64) -> f32 {
    x as f32
}

fn form_ref(v: &Value) -> Option<FormRef> {
    let (plugin, id) = v.as_str()?.split_once('|')?;
    let id = u32::from_str_radix(id, 16).ok().filter(|&id| id <= 0x00FF_FFFF)?;
    (!plugin.is_empty()).then(|| FormRef { plugin: plugin.to_owned(), id })
}

/// The appearance facts a look carries, or None for text that is no look
/// ([`preset_ok`]), a head part whose identifier is not a form, or more
/// than [`MAX_PARTS`] parts.
#[must_use]
pub fn look_facts(text: &str) -> Option<LookFacts> {
    if !preset_ok(text) {
        return None;
    }
    let v: Value = serde_json::from_str(text).ok()?;
    let parts = match v.get("headParts") {
        None => Vec::new(),
        Some(Value::Array(items)) if items.len() <= MAX_PARTS => {
            items.iter().map(|p| p.get("formIdentifier").and_then(form_ref)).collect::<Option<Vec<_>>>()?
        }
        Some(_) => return None,
    };
    let actor = v.get("actor");
    let hair_color = actor
        .and_then(|a| a.get("hairColor"))
        .and_then(Value::as_u64)
        .and_then(|c| u32::try_from(c & 0x00FF_FFFF).ok());
    let weight = actor
        .and_then(|a| a.get("weight"))
        .and_then(Value::as_f64)
        .filter(|w| (0.0..=100.0).contains(w))
        .map(narrow);
    let head_texture = actor.and_then(|a| a.get("headTexture")).and_then(form_ref);
    Some(LookFacts { parts, hair_color, weight, head_texture })
}

/// The stored look with `part` in place of its part of the same head part
/// type (thuum Eli's D-E1 A, ADR-026's one authority over a face): a
/// script's recorded `Actor.ChangeHeadPart` swaps the appearance's part, and
/// RaceMenu's load of this look on a figure, and on the player after a
/// login, replaces the base's parts by type with the look's
/// (docs/verbs/racemenu-sync.md, x-head-parts 20261007-133253), so the look
/// must name the new part too or it puts the old one back. `same_type` says,
/// for each `headParts` entry in order, whether its part is of `part`'s HDPT
/// type (PNAM; the core reads each part's record). The first such entry
/// names `part` instead (RaceMenu's `"<plugin>|<six hex digits>"`, every
/// other field of the entry kept), and any later one goes. None when `text`
/// is no look, `same_type` does not cover its entries one for one, no entry
/// is of that type (the look then leaves that type to the appearance), the
/// entry names `part` already with no later one of the type, or the result
/// is no preset ([`preset_ok`]).
#[must_use]
pub fn with_head_part(text: &str, same_type: &[bool], part: &FormRef) -> Option<String> {
    if !preset_ok(text) {
        return None;
    }
    let mut look: Value = serde_json::from_str(text).ok()?;
    let entries = look.get_mut("headParts")?.as_array_mut()?;
    if entries.len() != same_type.len() {
        return None;
    }
    let names_part = |entry: &Value| entry.get("formIdentifier").and_then(form_ref).is_some_and(|r| r.id == part.id && r.plugin.eq_ignore_ascii_case(&part.plugin));
    let mut placed = false;
    let mut changed = false;
    let mut kept = Vec::with_capacity(entries.len());
    for (mut entry, &same) in std::mem::take(entries).into_iter().zip(same_type) {
        if !same {
            kept.push(entry);
            continue;
        }
        if placed {
            changed = true;
            continue;
        }
        placed = true;
        if !names_part(&entry) {
            entry.as_object_mut()?.insert("formIdentifier".to_owned(), Value::String(format!("{}|{:06X}", part.plugin, part.id)));
            changed = true;
        }
        kept.push(entry);
    }
    *entries = kept;
    if !placed || !changed {
        return None;
    }
    let out = serde_json::to_string(&look).ok()?;
    preset_ok(&out).then_some(out)
}

/// What the core knows of one head part a look names.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PartFacts {
    /// The part's form id in the server's load order; 0 when the look's
    /// identifier names no head part the server has.
    pub form: u32,
    /// The part's valid-race list holds the appearance's race; true when
    /// the record lists none (the data gives no answer).
    pub valid_for_race: bool,
    /// The extra parts the record lists, in its order (its hairline, for a
    /// hair).
    pub extras: Vec<u32>,
}

/// The appearance's head parts a look implies (thuum ADR-026): each part
/// followed by its extra parts, in the look's order, each once; None when a
/// part is unknown to the server or not valid for the appearance's race, and
/// the look is refused.
#[must_use]
pub fn derived_head_parts(parts: &[PartFacts]) -> Option<Vec<u32>> {
    let mut out: Vec<u32> = Vec::new();
    for p in parts {
        if p.form == 0 || !p.valid_for_race {
            return None;
        }
        for &f in core::iter::once(&p.form).chain(p.extras.iter()) {
            if f != 0 && !out.contains(&f) {
                out.push(f);
            }
        }
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_object_is_kept() {
        assert!(preset_ok(r#"{"version": {"formatVersion": 3}, "headParts": []}"#));
        assert!(preset_ok("{}"));
    }

    #[test]
    fn anything_else_is_refused() {
        for text in ["", "[]", "1", "\"x\"", "null", "{", "{\"a\": }", "{} {}"] {
            assert!(!preset_ok(text), "{text:?}");
        }
    }

    // rotfern's look as the lab recorded it on 2026-10-08 (parts, actor)
    const LOOK: &str = r#"{"actor": {"hairColor": 6185079, "headTexture": "rotfern.esp|02E110", "weight": 50},
        "headParts": [{"formIdentifier": "rotfern.esp|02E117", "type": 0},
                      {"formIdentifier": "rotfern.esp|02E116", "type": 1},
                      {"formIdentifier": "Skyrim.esm|0EC1B2", "type": 5},
                      {"formIdentifier": "rotfern.esp|02E11A", "type": 3}]}"#;

    #[test]
    fn a_look_names_its_parts_hair_colour_weight_and_face_texture() {
        let f = look_facts(LOOK).unwrap_or_default();
        let ids: Vec<(&str, u32)> = f.parts.iter().map(|p| (p.plugin.as_str(), p.id)).collect();
        assert_eq!(ids, vec![("rotfern.esp", 0x02E117), ("rotfern.esp", 0x02E116), ("Skyrim.esm", 0x0EC1B2), ("rotfern.esp", 0x02E11A)]);
        assert_eq!(f.hair_color, Some(0x5E6077));
        assert_eq!(f.weight, Some(50.0));
        assert_eq!(f.head_texture, Some(FormRef { plugin: "rotfern.esp".into(), id: 0x02E110 }));
        assert_eq!(look_facts("{}"), Some(LookFacts::default()));
    }

    #[test]
    fn a_look_with_a_part_that_is_no_form_is_no_look() {
        for text in [
            r#"{"headParts": [{"formIdentifier": "rotfern.esp"}]}"#,
            r#"{"headParts": [{"formIdentifier": "|02E117"}]}"#,
            r#"{"headParts": [{"formIdentifier": "rotfern.esp|1000000"}]}"#,
            r#"{"headParts": [{"type": 1}]}"#,
            r#"{"headParts": {"formIdentifier": "rotfern.esp|02E117"}}"#,
            "[]",
        ] {
            assert_eq!(look_facts(text), None, "{text}");
        }
        let many = format!("{{\"headParts\": [{}]}}", vec![r#"{"formIdentifier": "a.esp|1"}"#; MAX_PARTS + 1].join(","));
        assert_eq!(look_facts(&many), None);
        // a weight outside 0 to 100 or a colour of another shape is left out
        let odd = look_facts(r#"{"actor": {"weight": 150, "hairColor": "white"}}"#).unwrap_or_default();
        assert_eq!((odd.weight, odd.hair_color), (None, None));
    }

    #[test]
    fn the_parts_a_look_implies_come_with_their_extras_once() {
        let part = |form: u32, extras: &[u32]| PartFacts { form, valid_for_race: true, extras: extras.to_vec() };
        // her look's parts as the lab's server resolved them: the hair
        // brings its hairline, after it, as the recorded appearance has it
        let parts = [part(0x0802_E117, &[]), part(0x0802_E116, &[]), part(0x0802_E11C, &[]), part(0x000E_C1B2, &[]),
                     part(0x0802_E11A, &[0x0802_E119]), part(0x0801_4C06, &[]), part(0x000E_4DA8, &[])];
        assert_eq!(derived_head_parts(&parts), Some(vec![0x0802_E117, 0x0802_E116, 0x0802_E11C, 0x000E_C1B2,
                                                        0x0802_E11A, 0x0802_E119, 0x0801_4C06, 0x000E_4DA8]));
        assert_eq!(derived_head_parts(&[part(1, &[2]), part(2, &[])]), Some(vec![1, 2]));
        assert_eq!(derived_head_parts(&[]), Some(vec![]));
    }

    #[test]
    fn a_part_the_server_lacks_or_the_race_may_not_wear_refuses_the_look() {
        let ok = PartFacts { form: 1, valid_for_race: true, extras: vec![] };
        assert_eq!(derived_head_parts(&[ok.clone(), PartFacts { form: 0, ..ok.clone() }]), None);
        assert_eq!(derived_head_parts(&[ok.clone(), PartFacts { form: 2, valid_for_race: false, extras: vec![] }]), None);
    }

    // rotfern's eyes (thuum D-E1 A): RotfernEyes 0x014C06 and RotfernEyesMora
    // 0x023F0E, both rotfern.esp's; the look keeps her mouth and hair beside
    // the eyes (type 2, PNAM's Eyes)
    const EYES_LOOK: &str = r#"{"actor": {"weight": 50},
        "headParts": [{"formIdentifier": "rotfern.esp|02E117", "type": 0},
                      {"formIdentifier": "rotfern.esp|014C06", "type": 2},
                      {"formIdentifier": "rotfern.esp|02E11A", "type": 3}]}"#;

    fn mora() -> FormRef {
        FormRef { plugin: "rotfern.esp".into(), id: 0x023F0E }
    }

    fn parts_of(look: Option<&str>) -> Vec<(String, u32)> {
        look.and_then(look_facts).unwrap_or_default().parts.into_iter().map(|p| (p.plugin, p.id)).collect()
    }

    #[test]
    fn a_swapped_part_takes_its_types_place_in_the_look() {
        let out = with_head_part(EYES_LOOK, &[false, true, false], &mora());
        assert_eq!(parts_of(out.as_deref()), vec![("rotfern.esp".to_owned(), 0x02E117), ("rotfern.esp".to_owned(), 0x023F0E), ("rotfern.esp".to_owned(), 0x02E11A)]);
        // written as RaceMenu writes an identifier, the entry's type and the
        // rest of the look kept
        let text = out.unwrap_or_default();
        assert!(text.contains("\"rotfern.esp|023F0E\""), "{text}");
        let v: Value = serde_json::from_str(&text).unwrap_or_default();
        let eyes = v.get("headParts").and_then(|h| h.get(1));
        assert_eq!(eyes.and_then(|e| e.get("type")).and_then(Value::as_u64), Some(2));
        assert_eq!(look_facts(&text).and_then(|f| f.weight), Some(50.0));
    }

    #[test]
    fn a_look_without_the_type_or_naming_the_part_already_is_left_as_it_is() {
        // no part of the type: the appearance keeps that type
        assert_eq!(with_head_part(EYES_LOOK, &[false, false, false], &mora()), None);
        // the core's facts must cover every entry
        assert_eq!(with_head_part(EYES_LOOK, &[false, true], &mora()), None);
        // the part already, its plugin named in another case
        assert_eq!(with_head_part(EYES_LOOK, &[false, true, false], &FormRef { plugin: "ROTFERN.ESP".into(), id: 0x014C06 }), None);
        assert_eq!(with_head_part("{}", &[], &mora()), None);
        assert_eq!(with_head_part("[]", &[], &mora()), None);
    }

    #[test]
    fn a_second_part_of_the_type_goes() {
        let two = r#"{"headParts": [{"formIdentifier": "rotfern.esp|014C06", "type": 2}, {"formIdentifier": "Skyrim.esm|051632", "type": 2}]}"#;
        assert_eq!(parts_of(with_head_part(two, &[true, true], &mora()).as_deref()), vec![("rotfern.esp".to_owned(), 0x023F0E)]);
        // the first names it already: the later one still goes
        let eyes = FormRef { plugin: "rotfern.esp".into(), id: 0x014C06 };
        assert_eq!(parts_of(with_head_part(two, &[true, true], &eyes).as_deref()), vec![("rotfern.esp".to_owned(), 0x014C06)]);
    }

    #[test]
    fn deep_nesting_is_refused() {
        let at = |n: usize| format!("{}{}", "{\"a\":".repeat(n), "1".to_owned() + &"}".repeat(n));
        assert!(preset_ok(&at(MAX_DEPTH)));
        assert!(!preset_ok(&at(MAX_DEPTH + 1)));
        // past serde_json's own limit: an error, not a crash
        assert!(!preset_ok(&at(10_000)));
    }
}
