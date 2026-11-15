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
/// other field of the entry kept), and any later one goes. A look with no
/// entry of that type gains one, `{"formIdentifier", "type": kind}` (`kind`
/// the part's HDPT type, PNAM), at the end of `headParts`: thuum #51,
/// x-vc01-cure-relog 20261115-145454, where the turn's SetRace had stashed
/// rotfern's own eyes ([`without_head_parts`]) and her script's recorded
/// ChangeHeadPart of her vampire eyes found no eyes entry to name, so the
/// stored look carried no eyes, and RaceMenu's load of it after her login
/// left her base with none (her game's head parts read mouth, head, ears,
/// hair, hairline and brows). None when `text` is no look, `same_type` does
/// not cover its entries one for one, no entry is of that type and `kind`
/// is [`NO_TYPE`], the entry names `part` already with no later one of the
/// type, or the result is no preset ([`preset_ok`]).
#[must_use]
pub fn with_head_part(text: &str, same_type: &[bool], part: &FormRef, kind: u32) -> Option<String> {
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
    if !placed {
        if kind == NO_TYPE {
            return None;
        }
        let mut entry = serde_json::Map::new();
        entry.insert("formIdentifier".to_owned(), Value::String(format!("{}|{:06X}", part.plugin, part.id)));
        entry.insert("type".to_owned(), Value::from(kind));
        kept.push(Value::Object(entry));
        changed = true;
    }
    *entries = kept;
    if !changed {
        return None;
    }
    let out = serde_json::to_string(&look).ok()?;
    preset_ok(&out).then_some(out)
}

/// The key of the stored look's stash (thuum train 53's V-look, its review's
/// correctness-4): a top-level array of the `headParts` entries a recorded
/// `Actor.SetRace` took off the look ([`without_head_parts`]), each whole,
/// the oldest first, at most [`MAX_PARTS`], so a later `SetRace` to a race
/// that may wear one gives it back ([`with_race_parts`]): VampireCure's
/// return to the base race gets back the eyes the turn took. The server's own
/// record: the core strips it from a look a game sends (a race menu's new
/// look replaces any stash) and from every look it sends a game
/// ([`without_race_stash`]), so RaceMenu's load never reads it.
pub const RACE_STASH: &str = "thuumRaceDropped";

/// The HDPT type ([`with_race_parts`]'s `stash_type` and `entry_type`) of a
/// part whose record gives no answer: it is no other part's type, and a
/// stashed part of it never comes back.
pub const NO_TYPE: u32 = u32::MAX;

/// The stored look without the head parts its new race may not wear (thuum
/// train 53's V-look; lab/.cache/drafts/reads-53/r-v3-v4-v5-look.md, R-V3).
/// A recorded `Actor.SetRace` writes only the appearance's race, and every
/// figure a game builds of the player loads the stored look over it
/// (raceMenuService's LoadCharacterPresetEx, APPLY_ALL), whose head parts
/// replace the base's by type, so a part the new race may not wear comes
/// back on the new race's figure (playtest 51: Ariadne's
/// FemaleEyesKhajiitBlueNarrow 0xEE878, KhajiitRace's alone, on her vampire
/// figure on rotfern's screen). `keep` says, for each `headParts` entry in
/// order, whether its part stays (the core reads each part's valid-race list,
/// HDPT RNAM, against the new race); every other field of a kept entry, and
/// the rest of the look, stay as they are. Each entry that goes is appended,
/// whole, to the look's stash ([`RACE_STASH`], created when absent, a
/// non-array one replaced), whose oldest entries go past [`MAX_PARTS`]. None
/// when `text` is no look, `keep` does not cover its entries one for one,
/// nothing goes, or the result is no preset ([`preset_ok`]).
#[must_use]
pub fn without_head_parts(text: &str, keep: &[bool]) -> Option<String> {
    if !preset_ok(text) {
        return None;
    }
    let mut look: Value = serde_json::from_str(text).ok()?;
    let entries = look.get_mut("headParts")?.as_array_mut()?;
    if entries.len() != keep.len() || keep.iter().all(|&k| k) {
        return None;
    }
    let mut dropped = Vec::new();
    for (entry, &k) in std::mem::take(entries).into_iter().zip(keep) {
        if k {
            entries.push(entry);
        } else {
            dropped.push(entry);
        }
    }
    let stash = look.as_object_mut()?.entry(RACE_STASH).or_insert_with(|| Value::Array(Vec::new()));
    if !stash.is_array() {
        *stash = Value::Array(Vec::new());
    }
    let stashed = stash.as_array_mut()?;
    stashed.extend(dropped);
    let over = stashed.len().saturating_sub(MAX_PARTS);
    stashed.drain(..over);
    let out = serde_json::to_string(&look).ok()?;
    preset_ok(&out).then_some(out)
}

/// The head parts the stored look's stash holds ([`RACE_STASH`]), in its
/// order, the oldest first: empty when the look has none. None when `text`
/// is no look, the stash is no array or holds more than [`MAX_PARTS`], or
/// an entry's identifier is not a form.
#[must_use]
pub fn race_stash(text: &str) -> Option<Vec<FormRef>> {
    if !preset_ok(text) {
        return None;
    }
    let v: Value = serde_json::from_str(text).ok()?;
    match v.get(RACE_STASH) {
        None => Some(Vec::new()),
        Some(Value::Array(items)) if items.len() <= MAX_PARTS => items.iter().map(|p| p.get("formIdentifier").and_then(form_ref)).collect(),
        Some(_) => None,
    }
}

/// The stored look with the stashed head parts its new race may wear given
/// back (thuum train 53's V-look, its review's correctness-4: VampireCure's
/// `SetRace` back to the base race got nothing back, the watcher's figure and
/// the player's own game after a login keeping the new race's defaults for
/// good). `fits` says, for each stashed entry in order ([`race_stash`]),
/// whether the new race may wear its part (the core reads its valid-race
/// list; a record with no answer fits), and `stash_type` its HDPT type
/// (PNAM); `entry_type` gives each current `headParts` entry's type,
/// [`NO_TYPE`] where the record gives none. Each stashed entry that fits and
/// has a type leaves the stash and takes the place of every entry of its
/// type (one given back before it included, so the newest of a type wins);
/// the rest stay stashed, and the stash's key goes once it is empty. None
/// when `text` is no look or has no stash, the lengths do not cover the
/// entries one for one, nothing comes back, or the result is no preset.
#[must_use]
pub fn with_race_parts(text: &str, fits: &[bool], stash_type: &[u32], entry_type: &[u32]) -> Option<String> {
    if !preset_ok(text) {
        return None;
    }
    let mut look: Value = serde_json::from_str(text).ok()?;
    let map = look.as_object_mut()?;
    let Value::Array(stash) = map.remove(RACE_STASH)? else {
        return None;
    };
    if stash.len() != fits.len() || stash.len() != stash_type.len() {
        return None;
    }
    let entries = map.entry("headParts").or_insert_with(|| Value::Array(Vec::new())).as_array_mut()?;
    if entries.len() != entry_type.len() {
        return None;
    }
    let mut parts: Vec<(Value, u32)> = std::mem::take(entries).into_iter().zip(entry_type.iter().copied()).collect();
    let mut left = Vec::new();
    let mut moved = false;
    for ((entry, &fit), &kind) in stash.into_iter().zip(fits).zip(stash_type) {
        if fit && kind != NO_TYPE {
            parts.retain(|&(_, t)| t != kind);
            parts.push((entry, kind));
            moved = true;
        } else {
            left.push(entry);
        }
    }
    if !moved {
        return None;
    }
    *entries = parts.into_iter().map(|(entry, _)| entry).collect();
    if !left.is_empty() {
        map.insert(RACE_STASH.to_owned(), Value::Array(left));
    }
    let out = serde_json::to_string(&look).ok()?;
    preset_ok(&out).then_some(out)
}

/// The look without its stash ([`RACE_STASH`]): what a game is sent, and what
/// the core records of a look a game sends (rule 5: the stash is the
/// server's word, never a seat's). None when `text` names no stash (a
/// substring test first, so a look without one costs no parse), is no look,
/// or the result is no preset.
#[must_use]
pub fn without_race_stash(text: &str) -> Option<String> {
    if !text.contains(RACE_STASH) || !preset_ok(text) {
        return None;
    }
    let mut look: Value = serde_json::from_str(text).ok()?;
    look.as_object_mut()?.remove(RACE_STASH)?;
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
        let out = with_head_part(EYES_LOOK, &[false, true, false], &mora(), 2);
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
    fn a_look_naming_the_part_already_or_with_no_type_to_give_is_left_as_it_is() {
        // no part of the type and no type to give the entry: nothing
        assert_eq!(with_head_part(EYES_LOOK, &[false, false, false], &mora(), NO_TYPE), None);
        // the core's facts must cover every entry
        assert_eq!(with_head_part(EYES_LOOK, &[false, true], &mora(), 2), None);
        // the part already, its plugin named in another case
        assert_eq!(with_head_part(EYES_LOOK, &[false, true, false], &FormRef { plugin: "ROTFERN.ESP".into(), id: 0x014C06 }, 2), None);
        assert_eq!(with_head_part("{}", &[], &mora(), 2), None);
        assert_eq!(with_head_part("[]", &[], &mora(), 2), None);
    }

    #[test]
    fn a_look_with_no_entry_of_the_type_gains_one() {
        // thuum #51 (x-vc01-cure-relog 20261115-145454): the turn stashed her
        // own eyes, so her script's vampire eyes are added, not lost
        let out = with_head_part(EYES_LOOK, &[false, false, false], &mora(), 2);
        assert_eq!(parts_of(out.as_deref()), vec![("rotfern.esp".to_owned(), 0x02E117), ("rotfern.esp".to_owned(), 0x014C06), ("rotfern.esp".to_owned(), 0x02E11A), ("rotfern.esp".to_owned(), 0x023F0E)]);
        let v: Value = serde_json::from_str(&out.unwrap_or_default()).unwrap_or_default();
        let added = v.get("headParts").and_then(|h| h.get(3));
        assert_eq!(added.and_then(|e| e.get("formIdentifier")).and_then(Value::as_str), Some("rotfern.esp|023F0E"));
        assert_eq!(added.and_then(|e| e.get("type")).and_then(Value::as_u64), Some(2));
        // a look with no headParts at all gains the list
        assert_eq!(parts_of(with_head_part(r#"{"headParts": []}"#, &[], &mora(), 2).as_deref()), vec![("rotfern.esp".to_owned(), 0x023F0E)]);
    }

    #[test]
    fn a_second_part_of_the_type_goes() {
        let two = r#"{"headParts": [{"formIdentifier": "rotfern.esp|014C06", "type": 2}, {"formIdentifier": "Skyrim.esm|051632", "type": 2}]}"#;
        assert_eq!(parts_of(with_head_part(two, &[true, true], &mora(), 2).as_deref()), vec![("rotfern.esp".to_owned(), 0x023F0E)]);
        // the first names it already: the later one still goes
        let eyes = FormRef { plugin: "rotfern.esp".into(), id: 0x014C06 };
        assert_eq!(parts_of(with_head_part(two, &[true, true], &eyes, 2).as_deref()), vec![("rotfern.esp".to_owned(), 0x014C06)]);
    }

    // Ariadne's look before her turn (playtest 51, R-V3): her eyes
    // FemaleEyesKhajiitBlueNarrow 0xEE878 (Skyrim.esm, KhajiitRace's alone by
    // its RNAM list, the read's esm read) between two parts that stand in
    #[test]
    fn a_new_races_look_drops_the_parts_it_may_not_wear_and_keeps_the_rest() {
        let look = r#"{"actor": {"weight": 30, "hairColor": 1193046},
            "headParts": [{"formIdentifier": "rotfern.esp|02E117", "type": 0},
                          {"formIdentifier": "Skyrim.esm|0EE878", "type": 2},
                          {"formIdentifier": "rotfern.esp|02E11A", "type": 3}],
            "tintInfo": [{"color": 1, "index": 4}]}"#;
        let out = without_head_parts(look, &[true, false, true]);
        assert_eq!(parts_of(out.as_deref()), vec![("rotfern.esp".to_owned(), 0x02E117), ("rotfern.esp".to_owned(), 0x02E11A)]);
        // the rest of the look, and each kept entry's fields, as they were
        let text = out.unwrap_or_default();
        let v: Value = serde_json::from_str(&text).unwrap_or_default();
        assert_eq!(v.get("headParts").and_then(|h| h.get(1)).and_then(|e| e.get("type")).and_then(Value::as_u64), Some(3));
        assert_eq!(look_facts(&text).and_then(|f| f.weight), Some(30.0));
        assert_eq!(look_facts(&text).and_then(|f| f.hair_color), Some(1_193_046));
        assert!(v.get("tintInfo").is_some_and(Value::is_array));
        // every part dropped: an empty list, still a look
        assert_eq!(parts_of(without_head_parts(look, &[false, false, false]).as_deref()), Vec::<(String, u32)>::new());
        // nothing to drop, facts that do not cover the entries, no look
        assert_eq!(without_head_parts(look, &[true, true, true]), None);
        assert_eq!(without_head_parts(look, &[true, false]), None);
        assert_eq!(without_head_parts("{}", &[]), None);
        assert_eq!(without_head_parts("[]", &[]), None);
        assert_eq!(without_head_parts(r#"{"headParts": 3}"#, &[]), None);
    }

    proptest::proptest! {
        #[test]
        fn a_dropped_look_keeps_exactly_the_kept_parts_in_order(keep in proptest::collection::vec(proptest::prelude::any::<bool>(), 0..12)) {
            let entries: Vec<String> = (0..keep.len()).map(|i| format!(r#"{{"formIdentifier": "Skyrim.esm|{:06X}", "type": {}}}"#, i.saturating_add(1), i & 3)).collect();
            let look = format!(r#"{{"actor": {{"weight": 10}}, "headParts": [{}]}}"#, entries.join(","));
            let out = without_head_parts(&look, &keep);
            if keep.iter().all(|&k| k) {
                proptest::prop_assert_eq!(out, None);
            } else {
                let ids: Vec<u32> = parts_of(out.as_deref()).into_iter().map(|(_, id)| id).collect();
                let want: Vec<u32> = keep.iter().enumerate().filter(|(_, &k)| k).filter_map(|(i, _)| u32::try_from(i.saturating_add(1)).ok()).collect();
                proptest::prop_assert_eq!(ids, want);
            }
        }
    }

    // the cure's round trip (the V-look review's correctness-4): the turn's
    // drop stashes Ariadne's eyes, and the return to the base race gives
    // them back
    #[test]
    fn a_stashed_part_comes_back_at_a_race_that_may_wear_it() {
        let look = r#"{"actor": {"weight": 30},
            "headParts": [{"formIdentifier": "rotfern.esp|02E117", "type": 0},
                          {"formIdentifier": "Skyrim.esm|0EE878", "type": 2},
                          {"formIdentifier": "rotfern.esp|02E11A", "type": 3}]}"#;
        let turned = without_head_parts(look, &[true, false, true]).unwrap_or_default();
        assert_eq!(race_stash(&turned), Some(vec![FormRef { plugin: "Skyrim.esm".into(), id: 0x0EE878 }]));
        // the stashed entry whole, its fields kept
        let v: Value = serde_json::from_str(&turned).unwrap_or_default();
        assert_eq!(v.get(RACE_STASH).and_then(|s| s.get(0)).and_then(|e| e.get("type")).and_then(Value::as_u64), Some(2));
        // a race that may not wear it: nothing comes back
        assert_eq!(with_race_parts(&turned, &[false], &[2], &[0, 3]), None);
        let cured = with_race_parts(&turned, &[true], &[2], &[0, 3]).unwrap_or_default();
        let mut ids: Vec<u32> = parts_of(Some(&cured)).into_iter().map(|(_, id)| id).collect();
        ids.sort_unstable();
        assert_eq!(ids, vec![0x02E117, 0x02E11A, 0x0EE878]);
        assert_eq!(race_stash(&cured), Some(Vec::new()));
        assert!(!cured.contains(RACE_STASH), "{cured}");
        assert_eq!(look_facts(&cured).and_then(|f| f.weight), Some(30.0));
    }

    #[test]
    fn a_stashed_part_takes_its_types_place_and_the_rest_stay_stashed() {
        // the types are the core's reading of each HDPT (stash_type,
        // entry_type), never the look's own field, which a seat wrote
        let look = r#"{"headParts": [{"formIdentifier": "rotfern.esp|014C06", "type": 2}],
            "thuumRaceDropped": [{"formIdentifier": "Skyrim.esm|0EE878", "type": 2},
                                 {"formIdentifier": "Skyrim.esm|05150A", "type": 3},
                                 {"formIdentifier": "Skyrim.esm|051507", "type": 4}]}"#;
        // the first fits and replaces the eyes the look holds; the second
        // does not fit; the third is of no type the records answer
        let out = with_race_parts(look, &[true, false, true], &[2, 3, NO_TYPE], &[2]).unwrap_or_default();
        assert_eq!(parts_of(Some(&out)), vec![("Skyrim.esm".to_owned(), 0x0EE878)]);
        assert_eq!(race_stash(&out).map(|s| s.into_iter().map(|r| r.id).collect::<Vec<_>>()), Some(vec![0x05150A, 0x051507]));
        // two of one type: the newer wins
        let two = with_race_parts(look, &[true, true, false], &[2, 2, 4], &[2]).unwrap_or_default();
        assert_eq!(parts_of(Some(&two)), vec![("Skyrim.esm".to_owned(), 0x05150A)]);
        // lengths that do not cover the entries, or no stash: nothing
        assert_eq!(with_race_parts(look, &[true], &[2], &[2]), None);
        assert_eq!(with_race_parts(look, &[true, false, true], &[2, 3, 4], &[]), None);
        assert_eq!(with_race_parts(r#"{"headParts": []}"#, &[], &[], &[]), None);
        assert_eq!(with_race_parts(r#"{"thuumRaceDropped": 3}"#, &[], &[], &[]), None);
    }

    #[test]
    fn the_stash_keeps_its_newest_max_parts() {
        let count = MAX_PARTS.saturating_add(6);
        let entries: Vec<String> = (1..=count).map(|i| format!(r#"{{"formIdentifier": "Skyrim.esm|{i:06X}", "type": 0}}"#)).collect();
        let look = format!(r#"{{"headParts": [{}]}}"#, entries.join(","));
        let out = without_head_parts(&look, &vec![false; count]).unwrap_or_default();
        let ids: Vec<usize> = race_stash(&out).unwrap_or_default().into_iter().filter_map(|r| usize::try_from(r.id).ok()).collect();
        assert_eq!(ids, (7..=count).collect::<Vec<_>>());
    }

    #[test]
    fn a_look_for_a_game_carries_no_stash() {
        let look = r#"{"actor": {"weight": 30}, "headParts": [], "thuumRaceDropped": [{"formIdentifier": "Skyrim.esm|0EE878", "type": 2}]}"#;
        let out = without_race_stash(look).unwrap_or_default();
        assert!(!out.contains(RACE_STASH), "{out}");
        assert_eq!(look_facts(&out).and_then(|f| f.weight), Some(30.0));
        // none to strip, the name only in a value, or no look: nothing
        assert_eq!(without_race_stash(r#"{"headParts": []}"#), None);
        assert_eq!(without_race_stash(r#"{"name": "thuumRaceDropped"}"#), None);
        assert_eq!(without_race_stash(r#"["thuumRaceDropped"]"#), None);
        // a stash the rules do not read
        assert_eq!(race_stash(r#"{"thuumRaceDropped": 3}"#), None);
        assert_eq!(race_stash(r#"{"thuumRaceDropped": [{"formIdentifier": "nope"}]}"#), None);
        assert_eq!(race_stash(r#"{"headParts": []}"#), Some(Vec::new()));
    }

    proptest::proptest! {
        #[test]
        fn a_drop_then_a_return_gives_back_exactly_what_fits(
            keep in proptest::collection::vec(proptest::prelude::any::<bool>(), 0..12),
            fits in proptest::collection::vec(proptest::prelude::any::<bool>(), 12),
        ) {
            // one type a part (its id less one), so no part takes another's place
            let entries: Vec<String> = (0..keep.len()).map(|i| format!(r#"{{"formIdentifier": "Skyrim.esm|{:06X}", "type": 0}}"#, i.saturating_add(1))).collect();
            let look = format!(r#"{{"actor": {{"weight": 10}}, "headParts": [{}]}}"#, entries.join(","));
            let ids_of = |text: Option<&str>| -> Vec<u32> { parts_of(text).into_iter().map(|(_, id)| id).collect() };
            match without_head_parts(&look, &keep) {
                None => proptest::prop_assert!(keep.iter().all(|&k| k)),
                Some(turned) => {
                    let stashed: Vec<u32> = race_stash(&turned).unwrap_or_default().into_iter().map(|r| r.id).collect();
                    let dropped: Vec<u32> = keep.iter().enumerate().filter(|(_, &k)| !k).filter_map(|(i, _)| u32::try_from(i.saturating_add(1)).ok()).collect();
                    proptest::prop_assert_eq!(&stashed, &dropped);
                    let fit: Vec<bool> = fits.iter().take(stashed.len()).copied().collect();
                    let stash_type: Vec<u32> = stashed.iter().map(|id| id.saturating_sub(1)).collect();
                    let current = ids_of(Some(&turned));
                    let entry_type: Vec<u32> = current.iter().map(|id| id.saturating_sub(1)).collect();
                    let back = with_race_parts(&turned, &fit, &stash_type, &entry_type);
                    if fit.iter().any(|&f| f) {
                        let text = back.unwrap_or_default();
                        let mut got = ids_of(Some(&text));
                        got.sort_unstable();
                        let mut want: Vec<u32> = current.iter().copied().chain(stashed.iter().zip(&fit).filter(|(_, &f)| f).map(|(&id, _)| id)).collect();
                        want.sort_unstable();
                        proptest::prop_assert_eq!(got, want);
                        let left: Vec<u32> = race_stash(&text).unwrap_or_default().into_iter().map(|r| r.id).collect();
                        let want_left: Vec<u32> = stashed.iter().zip(&fit).filter(|(_, &f)| !f).map(|(&id, _)| id).collect();
                        proptest::prop_assert_eq!(left, want_left);
                    } else {
                        proptest::prop_assert_eq!(back, None);
                    }
                }
            }
        }
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
