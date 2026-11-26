//! The quests a new character's own game sets once its first race menu
//! closes. The server setting `newPlayerQuests` lists them: each entry an
//! object `{"quest": "<hex id>:<plugin>", "stage": <n>}`, in the order the
//! game sets them, each sent as a Quest.SetStage call to that player's game
//! alone.
use crate::clock::SettingsError;
use crate::forms::FormDesc;

/// One quest's stage.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QuestStage {
    /// The quest, as the load order names it.
    pub quest: FormDesc,
    /// The stage set (a quest stage's index is 16 bits wide).
    pub stage: u16,
}

/// The list, from the `newPlayerQuests` block's JSON text (`[]` for none). A
/// bad entry is named in the error, so the server stops at its start.
pub fn from_json(text: &str) -> Result<Vec<QuestStage>, SettingsError> {
    let entries: Vec<serde_json::Value> = serde_json::from_str(text).map_err(|e| SettingsError(format!("newPlayerQuests: {e}")))?;
    let mut list: Vec<QuestStage> = Vec::with_capacity(entries.len());
    for e in entries {
        let serde_json::Value::Object(o) = &e else {
            return Err(SettingsError(format!("newPlayerQuests: {e} is not an object")));
        };
        let Some(name) = o.get("quest").and_then(serde_json::Value::as_str) else {
            return Err(SettingsError(format!("newPlayerQuests: {e} has no \"quest\" string")));
        };
        let quest = FormDesc::parse(name).map_err(|why| SettingsError(format!("newPlayerQuests: {why}")))?;
        let Some(stage) = o.get("stage").and_then(serde_json::Value::as_u64).and_then(|n| u16::try_from(n).ok()) else {
            return Err(SettingsError(format!("newPlayerQuests: {e} has no stage from 0 to 65535")));
        };
        if list.iter().any(|q| q.quest.is(quest.short_id, &quest.file) && q.stage == stage) {
            return Err(SettingsError(format!("newPlayerQuests: {name:?} stage {stage} is listed twice")));
        }
        list.push(QuestStage { quest, stage });
    }
    Ok(list)
}

#[cfg(test)]
#[allow(clippy::expect_used, clippy::indexing_slicing)]
mod tests {
    use super::*;

    #[test]
    fn each_quest_and_its_stage_in_order() {
        let list = from_json(r#"[{"quest": "daf:alternate start - live another life.esp", "stage": 10}, {"quest": "0x3372b:Skyrim.esm", "stage": 0}]"#).expect("a well-formed list");
        assert_eq!(list.len(), 2);
        assert_eq!(list[0], QuestStage { quest: FormDesc { short_id: 0xdaf, file: "alternate start - live another life.esp".into() }, stage: 10 });
        assert_eq!(list[1].quest.short_id, 0x3372b);
        assert_eq!(list[1].stage, 0);
    }

    #[test]
    fn no_quests_is_an_empty_list() {
        assert_eq!(from_json("[]").expect("empty"), Vec::new());
    }

    #[test]
    fn a_bad_entry_stops_the_setting_named() {
        for bad in [
            r#"{"quest": "daf:a.esp", "stage": 10}"#,
            r#"["daf:a.esp"]"#,
            r#"[{"stage": 10}]"#,
            r#"[{"quest": "daf:a.esp"}]"#,
            r#"[{"quest": "zz:a.esp", "stage": 10}]"#,
            r#"[{"quest": "daf:a.esp", "stage": -1}]"#,
            r#"[{"quest": "daf:a.esp", "stage": 65536}]"#,
            r#"[{"quest": "daf:a.esp", "stage": 10}, {"quest": "0xdaf:a.esp", "stage": 10}]"#,
        ] {
            let err = from_json(bad).expect_err(bad);
            assert!(err.0.starts_with("newPlayerQuests: "), "{bad}: {}", err.0);
        }
    }
}
