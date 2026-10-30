//! A form as the server's change forms name it (a FormDesc): its id within
//! its plugin and the plugin's name, so a change of load order keeps it.
//! Settings name forms this way, "<hex form id>:<plugin>" (castTeleports,
//! playerGlobals).

/// A form within its plugin. Hash and Eq are the exact pair (the record's
/// own spelling); `is` is the settings' comparison, without the file's case.
#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct FormDesc {
    /// The form id within its plugin (without the load index).
    pub short_id: u32,
    /// The plugin, as the load order names it, compared without case.
    pub file: String,
}

impl FormDesc {
    /// "<hex form id>:<plugin>", the id with or without "0x". The error says
    /// what is wrong with the text.
    pub fn parse(text: &str) -> Result<Self, String> {
        let Some((id, file)) = text.split_once(':') else {
            return Err(format!("{text:?} is not <hex form id>:<plugin>"));
        };
        let digits = id.strip_prefix("0x").unwrap_or(id);
        let Ok(short_id) = u32::from_str_radix(digits, 16) else {
            return Err(format!("{text:?} has no hex form id"));
        };
        if file.is_empty() {
            return Err(format!("{text:?} names no plugin"));
        }
        Ok(Self { short_id, file: file.to_owned() })
    }

    /// Whether this is the form `short_id` in `file`.
    #[must_use]
    pub fn is(&self, short_id: u32, file: &str) -> bool {
        self.short_id == short_id && self.file.eq_ignore_ascii_case(file)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_form_desc_parses_with_or_without_0x_and_matches_without_case() {
        let d = FormDesc::parse("0a86df:LostGrimoire.esp").unwrap_or(FormDesc { short_id: 0, file: String::new() });
        assert_eq!(d, FormDesc { short_id: 0xa86df, file: "LostGrimoire.esp".to_owned() });
        assert!(d.is(0xa86df, "lostgrimoire.esp"));
        assert!(!d.is(0xa86df, "Skyrim.esm"));
        let spaced = FormDesc::parse("0x00AAE8:Apocalypse - Magic of Skyrim.esp");
        assert!(spaced.is_ok_and(|d| d.is(0xaae8, "Apocalypse - Magic of Skyrim.esp")));
        for bad in ["a86df", "zz:LostGrimoire.esp", "a86df:", ":LostGrimoire.esp", ""] {
            assert!(FormDesc::parse(bad).is_err(), "{bad}");
        }
    }
}
