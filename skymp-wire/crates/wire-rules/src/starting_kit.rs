//! The kit a new player's character starts in. The server setting
//! `startingKit` lists it: each entry an object `{"form": "<hex id>:<plugin>",
//! "count": <n>, "worn": <bool>}`, `count` 1 when left out, `worn` false. The
//! server lays it on a profile's new character's record at its creation, in
//! place of an empty one, and the worn entries in its equipment; the login
//! then dresses the player's game from that record.
use crate::clock::SettingsError;
use crate::forms::FormDesc;

/// One item of the kit.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct KitItem {
    /// The item, as the load order names it.
    pub item: FormDesc,
    /// How many, at least 1.
    pub count: u32,
    /// Worn at the start.
    pub worn: bool,
}

/// The kit, from the `startingKit` block's JSON text (`[]` for none). A bad
/// entry is named in the error, so the server stops at its start rather
/// than make characters without it.
pub fn from_json(text: &str) -> Result<Vec<KitItem>, SettingsError> {
    let entries: Vec<serde_json::Value> = serde_json::from_str(text).map_err(|e| SettingsError(format!("startingKit: {e}")))?;
    let mut kit: Vec<KitItem> = Vec::with_capacity(entries.len());
    for e in entries {
        let serde_json::Value::Object(o) = &e else {
            return Err(SettingsError(format!("startingKit: {e} is not an object")));
        };
        let Some(name) = o.get("form").and_then(serde_json::Value::as_str) else {
            return Err(SettingsError(format!("startingKit: {e} has no \"form\" string")));
        };
        let item = FormDesc::parse(name).map_err(|why| SettingsError(format!("startingKit: {why}")))?;
        let count = match o.get("count") {
            None | Some(serde_json::Value::Null) => 1,
            Some(v) => match v.as_u64().and_then(|n| u32::try_from(n).ok()) {
                Some(n) if n > 0 => n,
                _ => return Err(SettingsError(format!("startingKit: {e} has a count that is not a whole number from 1"))),
            },
        };
        let worn = match o.get("worn") {
            None | Some(serde_json::Value::Null) => false,
            Some(serde_json::Value::Bool(b)) => *b,
            Some(_) => return Err(SettingsError(format!("startingKit: {e} has a worn that is not true or false"))),
        };
        if kit.iter().any(|k| k.item.is(item.short_id, &item.file)) {
            return Err(SettingsError(format!("startingKit: {name:?} is listed twice")));
        }
        kit.push(KitItem { item, count, worn });
    }
    Ok(kit)
}

#[cfg(test)]
#[allow(clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use super::*;

    #[test]
    fn the_prisoners_clothes_worn_and_a_purse_carried() {
        let kit = from_json(
            r#"[{"form": "03c9fe:Skyrim.esm", "worn": true},
                {"form": "0x3ca00:Skyrim.esm", "count": 1, "worn": true},
                {"form": "f:Skyrim.esm", "count": 5}]"#,
        )
        .expect("a well-formed kit");
        assert_eq!(kit.len(), 3);
        assert_eq!(kit[0], KitItem { item: FormDesc { short_id: 0x3c9fe, file: "Skyrim.esm".into() }, count: 1, worn: true });
        assert_eq!(kit[1].item.short_id, 0x3ca00);
        assert!(kit[1].worn);
        assert_eq!(kit[2], KitItem { item: FormDesc { short_id: 0xf, file: "Skyrim.esm".into() }, count: 5, worn: false });
    }

    #[test]
    fn no_kit_is_an_empty_list() {
        assert_eq!(from_json("[]").expect("empty"), Vec::new());
    }

    #[test]
    fn a_bad_entry_stops_the_setting_named() {
        for bad in [
            r#"{"form": "03c9fe:Skyrim.esm"}"#,
            r#"["03c9fe:Skyrim.esm"]"#,
            r#"[{"count": 1}]"#,
            r#"[{"form": "nothex:Skyrim.esm"}]"#,
            r#"[{"form": "03c9fe:"}]"#,
            r#"[{"form": "03c9fe:Skyrim.esm", "count": 0}]"#,
            r#"[{"form": "03c9fe:Skyrim.esm", "count": -1}]"#,
            r#"[{"form": "03c9fe:Skyrim.esm", "count": 4294967296}]"#,
            r#"[{"form": "03c9fe:Skyrim.esm", "worn": "yes"}]"#,
            r#"[{"form": "03c9fe:Skyrim.esm"}, {"form": "3c9fe:Skyrim.esm"}]"#,
        ] {
            let err = from_json(bad).expect_err(bad);
            assert!(err.0.starts_with("startingKit: "), "{bad}: {}", err.0);
        }
    }
}
