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

    #[test]
    fn deep_nesting_is_refused() {
        let at = |n: usize| format!("{}{}", "{\"a\":".repeat(n), "1".to_owned() + &"}".repeat(n));
        assert!(preset_ok(&at(MAX_DEPTH)));
        assert!(!preset_ok(&at(MAX_DEPTH + 1)));
        // past serde_json's own limit: an error, not a crash
        assert!(!preset_ok(&at(10_000)));
    }
}
