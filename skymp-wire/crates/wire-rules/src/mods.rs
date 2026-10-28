//! The mod lock (thuum docs/verbs/mod-lock.md): a client's plugins against
//! the server's, judged at the handshake. The client sends one hash per
//! plugin in the wire's `Hello` (`mod_hashes`); the server computes the same
//! list from its own manifest (the `manifest.json` the TypeScript layer
//! writes from the load order at every start) and compares. Pure: the files'
//! facts come from the manifest here and from Skyrim Platform's
//! `getFileInfo` on the client; the order, the hash and the judgement live
//! here and nowhere else.
//!
//! The list's order is the engine's: the full plugins in load order first,
//! then the light plugins in the engine's light order (the two are numbered
//! apart, docs/verbs/light-plugins.md); archives (.bsa) are not plugins and
//! never in the list. The hash is FNV-1a 64 over the plugin's name in
//! lowercase, one zero byte, its size as a little-endian u32 and its CRC-32
//! as a little-endian u32: the three facts both ends already have.

/// One plugin as the server's manifest lists it.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ModFile {
    /// The file name, as the load order spells it.
    pub name: String,
    /// The file's size in bytes.
    pub size: u32,
    /// The file's IEEE CRC-32 (zlib's; npm crc-32's and Skyrim Platform's).
    pub crc32: u32,
    /// Whether the engine loads it as a light plugin (an .esl, or the TES4
    /// record's kSmallFile flag), numbered apart from the full ones.
    pub light: bool,
}

/// The verdict on a client's list against the server's.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ModVerdict {
    /// Every plugin matches, in order.
    Ok,
    /// Every compared plugin matches but the counts differ (the shorter list
    /// is a prefix of the longer).
    Count {
        /// How many the server has.
        server: usize,
        /// How many the client sent.
        client: usize,
    },
    /// The first plugin that differs, by its index in the list.
    Mismatch {
        /// The index into the two-part list (full plugins, then light).
        index: usize,
    },
}

const FNV_OFFSET: u64 = 0xcbf2_9ce4_8422_2325;
const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;

fn fnv1a64(bytes: &[u8]) -> u64 {
    bytes.iter().fold(FNV_OFFSET, |h, &b| (h ^ u64::from(b)).wrapping_mul(FNV_PRIME))
}

/// The hash of one plugin: FNV-1a 64 over `lowercase(name) + 0x00 + size as
/// u32 little-endian + crc32 as u32 little-endian`. The same bytes on the
/// client (skymp5-client's HelloService, with BigInt).
#[must_use]
pub fn mod_hash(name: &str, size: u32, crc32: u32) -> u64 {
    let mut bytes = name.to_lowercase().into_bytes();
    bytes.push(0);
    bytes.extend_from_slice(&size.to_le_bytes());
    bytes.extend_from_slice(&crc32.to_le_bytes());
    fnv1a64(&bytes)
}

/// Whether a manifest entry is a plugin the engine numbers (not an archive).
fn is_plugin(name: &str) -> bool {
    let lower = name.to_lowercase();
    lower.ends_with(".esm") || lower.ends_with(".esp") || lower.ends_with(".esl")
}

/// The server's list to compare against: the full plugins' hashes in load
/// order, then the light plugins' in theirs; archives skipped.
#[must_use]
pub fn expected(files: &[ModFile]) -> Vec<u64> {
    let plugins = files.iter().filter(|f| is_plugin(&f.name));
    let full = plugins.clone().filter(|f| !f.light);
    let light = plugins.filter(|f| f.light);
    full.chain(light).map(|f| mod_hash(&f.name, f.size, f.crc32)).collect()
}

/// A client's list against the server's: the first differing index, else
/// the counts, else Ok.
#[must_use]
pub fn judge(expected: &[u64], got: &[u64]) -> ModVerdict {
    if let Some(index) = expected.iter().zip(got.iter()).position(|(e, g)| e != g) {
        return ModVerdict::Mismatch { index };
    }
    if expected.len() != got.len() {
        return ModVerdict::Count { server: expected.len(), client: got.len() };
    }
    ModVerdict::Ok
}

/// The server's `manifest.json` (skymp5-server/ts/manifestGen.ts): `mods`
/// is the load order's plugins and their archives with `filename`, `size`,
/// `crc32` (npm crc-32's signed 32-bit integer) and, for a light plugin,
/// `light: true`. The error names what is wrong.
pub fn parse_manifest(text: &str) -> Result<Vec<ModFile>, String> {
    let value: serde_json::Value =
        serde_json::from_str(text).map_err(|e| format!("manifest is not JSON: {e}"))?;
    let mods = value
        .get("mods")
        .and_then(serde_json::Value::as_array)
        .ok_or_else(|| "manifest has no mods array".to_string())?;
    let mut out = Vec::with_capacity(mods.len());
    for (i, m) in mods.iter().enumerate() {
        let name = m
            .get("filename")
            .and_then(serde_json::Value::as_str)
            .ok_or_else(|| format!("mods[{i}] has no filename"))?;
        let size = m
            .get("size")
            .and_then(serde_json::Value::as_u64)
            .and_then(|s| u32::try_from(s).ok())
            .ok_or_else(|| format!("mods[{i}] ({name}) has no size"))?;
        // npm crc-32 writes a signed 32-bit integer; the bytes are the same
        let crc = m
            .get("crc32")
            .and_then(serde_json::Value::as_i64)
            .ok_or_else(|| format!("mods[{i}] ({name}) has no crc32"))?;
        let crc32 = u32::try_from(crc & 0xFFFF_FFFF).map_err(|_| format!("mods[{i}] ({name}) crc32 out of range"))?;
        let light = m.get("light").and_then(serde_json::Value::as_bool).unwrap_or(false);
        out.push(ModFile { name: name.to_owned(), size, crc32, light });
    }
    Ok(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn file(name: &str, size: u32, crc32: u32, light: bool) -> ModFile {
        ModFile { name: name.to_owned(), size, crc32, light }
    }

    #[test]
    fn the_hash_is_pinned_against_a_reference_fnv1a() {
        // vectors from a Python reference (lowercase name, 0x00, size LE, crc LE)
        assert_eq!(mod_hash("A.esp", 1, 2), 0x879a_366e_e820_d57d);
        assert_eq!(mod_hash("Skyrim.esm", 249_753_797, 0x3af8_e5c1), 0x4381_e91f_019b_44e0);
        assert_eq!(mod_hash("ccQDRSSE001-SurvivalMode.esl", 12_345, 0xffff_ffff), 0x8467_297b_9001_4921);
        assert_eq!(mod_hash("a.esp", 1, 2), mod_hash("A.ESP", 1, 2), "case never counts");
        assert_eq!(fnv1a64(b""), FNV_OFFSET);
    }

    #[test]
    fn the_list_is_full_plugins_then_light_ones_without_archives() {
        let files = [
            file("Skyrim.esm", 10, 1, false),
            file("Skyrim.bsa", 99, 9, false),
            file("ccQDRSSE001-SurvivalMode.esl", 20, 2, true),
            file("Update.esm", 30, 3, false),
            file("Small.esp", 40, 4, true),
        ];
        let list = expected(&files);
        assert_eq!(
            list,
            vec![
                mod_hash("Skyrim.esm", 10, 1),
                mod_hash("Update.esm", 30, 3),
                mod_hash("ccQDRSSE001-SurvivalMode.esl", 20, 2),
                mod_hash("Small.esp", 40, 4),
            ]
        );
    }

    #[test]
    fn the_judgement_names_the_first_difference_then_the_counts() {
        let server = vec![1, 2, 3, 4];
        assert_eq!(judge(&server, &[1, 2, 3, 4]), ModVerdict::Ok);
        assert_eq!(judge(&server, &[1, 2, 3]), ModVerdict::Count { server: 4, client: 3 }, "a light plugin missing");
        assert_eq!(judge(&server, &[1, 2, 3, 4, 5]), ModVerdict::Count { server: 4, client: 5 }, "one extra");
        assert_eq!(judge(&server, &[1, 3, 2, 4]), ModVerdict::Mismatch { index: 1 }, "two swapped");
        assert_eq!(judge(&server, &[1, 2, 9, 4]), ModVerdict::Mismatch { index: 2 }, "a re-saved plugin");
        assert_eq!(judge(&server, &[1, 9]), ModVerdict::Mismatch { index: 1 }, "a difference before the count");
        assert_eq!(judge(&[], &[]), ModVerdict::Ok);
    }

    #[test]
    fn the_manifest_parses_as_manifest_gen_writes_it() {
        let text = r#"{"versionMajor":1,"loadOrder":["Skyrim.esm","Small.esl"],"mods":[
            {"crc32":-1159836192,"filename":"Skyrim.esm","size":249753797,"light":false},
            {"crc32":12,"filename":"Skyrim.bsa","size":5},
            {"crc32":-1,"filename":"Small.esl","size":7,"light":true}]}"#;
        let files = parse_manifest(text).unwrap_or_default();
        assert_eq!(files.len(), 3);
        assert_eq!(files.first().map(|f| (f.size, f.crc32, f.light)), Some((249_753_797, 0xbade_4de0, false)));
        assert_eq!(files.get(2).map(|f| (f.crc32, f.light)), Some((0xffff_ffff, true)));
        assert_eq!(expected(&files).len(), 2, "the archive is skipped");
        assert!(parse_manifest("{}").is_err());
        assert!(parse_manifest(r#"{"mods":[{"filename":"a.esp"}]}"#).is_err());
    }
}
