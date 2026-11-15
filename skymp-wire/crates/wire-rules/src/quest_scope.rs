//! Quest scope, D-Q1's derivation whole (thuum ADR-068 rung 5, "Rung 5's
//! words": D-Q2 replaced by one rung's full build; docs/research/
//! rung5-quests-2026-11-09.md section 8(a)). A quest is nobody's (D-F1 C)
//! unless it is one player's: a per-player quest (the vampire's, the
//! werewolf's) crossing would turn every seat's player. ADR-020's shape:
//! the rule is here, over facts the core gathers from the game files.
//!
//! Three halves. The alias half reads libespm's QUST aliases (libespm
//! QUST.h's Alias): a quest whose aliases are all the player's is Player,
//! an alias being the player's when it is forced to the player's reference
//! (ALFR [`PLAYER_REF`] 0x14), fills with the player's unique actor (ALUA
//! [`PLAYER_BASE`] 0x7, NPC_ Player; PlayerWerewolfQuest 0x2ba16's alias 0
//! fills so, with no ALFR: the census, read H-QS1), is created in the
//! inventory of an alias that is the player's (ALCO with ALCA's top bit,
//! wire-rules alias_fills' CREATE_IN: it lives in the player's inventory),
//! or fills from another quest's alias that is the player's (ALEQ and ALEA,
//! the core reading that alias's own ALFR and ALUA, one hop). A location
//! alias (ALLS), or any reference alias another way (conditions, an event,
//! a creation at an alias's place, a near alias), makes the quest World.
//! The list half is the generated setting `playerQuests` (FormDescs,
//! lab/tools/quest-census.py --emit over the deployed load order): a quest
//! with no alias that it names is Player (PlayerVampireQuest 0xeafd5, whose
//! scripts' state natives all aim at the player, read H-QS2); the list
//! never overrides an alias, so a listed quest with a world alias stays
//! World. A quest with no alias the list does not name is World.
//!
//! The started half is the generated setting `playerStartedQuests`
//! (FormDescs, the same census's emit; thuum D-VC2 B, Eli 2026-11-14
//! 23:15Z, narrowing D-VC1 B: "a quest is the player's only if what starts it
//! reads the player's own state, such as Falion's topic for a vampire"): a
//! quest one of whose fragments calls into a player quest's scripts and
//! every one of whose starts reads the player's own state (the census's
//! rule, lab/tools/quest-census.py's docstring) is Player, whatever its
//! aliases. This half overrides the alias rule, for the quests it names
//! alone: Rising at Dawn VC01 0x5c625, whose aliases are Falion and his
//! markers (the world's), is the player's, since each of its seven starts is
//! the player's vampirism (PlayerVampireQuestScript's VampireChange, or a
//! condition on the player's Vampire keyword), and it reached both journals
//! in playtest 52 when Falion's topic set its stage 100 on the world. A
//! named quest's class is Player whether or not the other halves agree.
//!
//! The setting `questScope` says whether the class acts: "off" (the
//! default) measures only, the core's startup line naming the marked set
//! against the denied list; "derived" acts: the core composes the marked
//! quests into wire-rules quest_stages (SharedQuests::set_player) and
//! script_variables, so a player-class quest's stage or word is refused
//! (`E_QUEST_PLAYER`), the login names it among what a client never
//! reports, and its variables are each player's own.

use crate::alias_fills::CREATE_IN;
use crate::clock::SettingsError;
use crate::forms::FormDesc;

/// The player's reference (PlayerRef), an alias's forced reference.
pub const PLAYER_REF: u32 = 0x14;

/// The player's actor base (NPC_ 0x7, Player), an alias's unique actor.
pub const PLAYER_BASE: u32 = 0x7;

/// Another quest's alias an alias fills from (ALEQ, ALEA), as the core read
/// that alias's own fill.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct External {
    /// Its forced reference (ALFR), 0 for none.
    pub forced: u32,
    /// Its unique actor's base (ALUA), 0 for none.
    pub unique_actor: u32,
}

/// One alias of a quest as the game files hold it (libespm QUST's Alias),
/// the form ids in the load order.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Alias {
    /// The alias's id (ALST, or ALLS's for a location alias).
    pub id: u32,
    /// A location alias (ALLS): never the player's.
    pub location: bool,
    /// The forced reference (ALFR), 0 for none.
    pub forced: u32,
    /// The unique actor's base (ALUA), 0 for none.
    pub unique_actor: u32,
    /// The base the alias creates (ALCO), 0 for none.
    pub created: u32,
    /// Where it is created (ALCA): [`CREATE_IN`] and the target alias's id
    /// in the low half.
    pub create_at: u32,
    /// The external alias it fills from (ALEQ and ALEA), none for none or
    /// one the core could not resolve.
    pub external: Option<External>,
}

/// A quest's class.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Class {
    /// The world's (D-F1 C): every seat runs it and its record crosses.
    World,
    /// One player's: its seat runs it, nothing of it crosses.
    Player,
}

impl Class {
    /// The bridge's number: 0 World, 1 Player.
    #[must_use]
    pub fn code(self) -> u8 {
        match self {
            Class::World => 0,
            Class::Player => 1,
        }
    }
}

/// Whether the class acts (`questScope`).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub enum Mode {
    /// "off": measured and logged only; the lever for a red.
    #[default]
    Off,
    /// "derived": the class gates QuestStage 57, AliasFill 79 and
    /// ScriptVariables 81.
    Derived,
}

/// Whether `alias` is the player's among its quest's `aliases`: forced to
/// the player, filled with the player's unique actor, created in a player's
/// alias's inventory, or filled from an external alias that is the player.
/// A creation chain is followed at most as many links as the quest has
/// aliases, so a cycle ends as no player's.
#[must_use]
pub fn is_players(alias: &Alias, aliases: &[Alias]) -> bool {
    let mut at = *alias;
    for _ in 0..=aliases.len() {
        if at.location {
            return false;
        }
        if at.forced == PLAYER_REF || at.unique_actor == PLAYER_BASE {
            return true;
        }
        if at.external.is_some_and(|e| e.forced == PLAYER_REF || e.unique_actor == PLAYER_BASE) {
            return true;
        }
        if at.created == 0 || at.create_at & CREATE_IN == 0 {
            return false;
        }
        let target = at.create_at & 0xFFFF;
        match aliases.iter().find(|a| !a.location && a.id == target) {
            Some(next) => at = *next,
            None => return false,
        }
    }
    false
}

/// The ids of the reference aliases among `aliases` that are the player's
/// ([`is_players`]), in their order: what the core's index of D-Q9 B's
/// alias scripts reads per quest (a script rides player aliases alone when
/// every alias it rides, over every quest, is among them).
#[must_use]
pub fn player_alias_ids(aliases: &[Alias]) -> Vec<u32> {
    aliases.iter().filter(|a| !a.location && is_players(a, aliases)).map(|a| a.id).collect()
}

/// A quest's class from all three halves: Player when `playerStartedQuests`
/// names it (`started`), whatever its aliases; otherwise [`classify`]'s.
#[must_use]
pub fn class_of(aliases: &[Alias], listed: bool, started: bool) -> Class {
    if started {
        Class::Player
    } else {
        classify(aliases, listed)
    }
}

/// A quest's class from its `aliases` and whether `playerQuests` names it
/// (`listed`), the alias and list halves: with aliases, Player when every one
/// is the player's, World otherwise whatever the list says; with none, Player
/// when listed.
#[must_use]
pub fn classify(aliases: &[Alias], listed: bool) -> Class {
    if aliases.is_empty() {
        return if listed { Class::Player } else { Class::World };
    }
    if aliases.iter().all(|a| is_players(a, aliases)) {
        Class::Player
    } else {
        Class::World
    }
}

/// The three settings: whether the class acts, the generated list half and
/// the generated started half.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct QuestScope {
    mode: Mode,
    listed: Vec<FormDesc>,
    started: Vec<FormDesc>,
}

/// A setting's list of quests, `setting` naming it in the error: a JSON
/// list of "<hex id>:<plugin>" strings, each quest once.
fn quest_list(setting: &str, json: &str) -> Result<Vec<FormDesc>, SettingsError> {
    let entries = match serde_json::from_str::<serde_json::Value>(json).map_err(|e| SettingsError(format!("{setting}: {e}")))? {
        serde_json::Value::Array(entries) => entries,
        other => return Err(SettingsError(format!("{setting}: {other} is not a list of \"<hex id>:<plugin>\""))),
    };
    let mut out: Vec<FormDesc> = Vec::with_capacity(entries.len());
    for e in &entries {
        let serde_json::Value::String(name) = e else {
            return Err(SettingsError(format!("{setting}: {e} is not \"<hex id>:<plugin>\"")));
        };
        let d = FormDesc::parse(name).map_err(|why| SettingsError(format!("{setting}: {why}")))?;
        if out.iter().any(|q| q.is(d.short_id, &d.file)) {
            return Err(SettingsError(format!("{setting}: {name:?} is listed twice")));
        }
        out.push(d);
    }
    Ok(out)
}

impl QuestScope {
    /// The settings from their JSON texts: `questScope`'s value, the string
    /// "off" or "derived", and `playerQuests`' and `playerStartedQuests`',
    /// each a list of "<hex id>:<plugin>" strings (`[]` for none), each quest
    /// once in its list. The core passes `"off"` and `[]` for a key the file
    /// lacks. The error names what is wrong.
    pub fn from_settings(mode_json: &str, player_quests_json: &str, player_started_quests_json: &str) -> Result<Self, SettingsError> {
        let mode = match serde_json::from_str::<serde_json::Value>(mode_json).map_err(|e| SettingsError(format!("questScope: {e}")))? {
            serde_json::Value::String(m) if m == "off" => Mode::Off,
            serde_json::Value::String(m) if m == "derived" => Mode::Derived,
            other => return Err(SettingsError(format!("questScope: {other} is neither \"off\" nor \"derived\""))),
        };
        let listed = quest_list("playerQuests", player_quests_json)?;
        let started = quest_list("playerStartedQuests", player_started_quests_json)?;
        Ok(Self { mode, listed, started })
    }

    /// Whether the class acts.
    #[must_use]
    pub fn mode(&self) -> Mode {
        self.mode
    }

    /// The list half, in the setting's order.
    #[must_use]
    pub fn listed(&self) -> &[FormDesc] {
        &self.listed
    }

    /// Whether `playerQuests` names the quest `short_id` in `file`.
    #[must_use]
    pub fn is_listed(&self, short_id: u32, file: &str) -> bool {
        self.listed.iter().any(|q| q.is(short_id, file))
    }

    /// The started half, in the setting's order.
    #[must_use]
    pub fn started(&self) -> &[FormDesc] {
        &self.started
    }

    /// Whether `playerStartedQuests` names the quest `short_id` in `file`.
    #[must_use]
    pub fn is_started(&self, short_id: u32, file: &str) -> bool {
        self.started.iter().any(|q| q.is(short_id, file))
    }

    /// The quest's class from its aliases and the list and started halves.
    #[must_use]
    pub fn class(&self, quest: &FormDesc, aliases: &[Alias]) -> Class {
        class_of(aliases, self.is_listed(quest.short_id, &quest.file), self.is_started(quest.short_id, &quest.file))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    fn forced(id: u32, refr: u32) -> Alias {
        Alias { id, forced: refr, ..Alias::default() }
    }

    #[test]
    fn a_quest_of_player_aliases_is_the_players() {
        // PlayerWerewolfQuest 0x2ba16's alias 0 Player: ALUA 0x7, no ALFR
        // (the census, read H-QS1)
        let werewolf = [Alias { id: 0, unique_actor: PLAYER_BASE, ..Alias::default() }];
        assert_eq!(classify(&werewolf, false), Class::Player);
        // forced to PlayerRef, and an item created in the player's inventory
        let quest = [forced(0, PLAYER_REF), Alias { id: 1, created: 0x39647, create_at: CREATE_IN, ..Alias::default() }];
        assert_eq!(classify(&quest, false), Class::Player);
        // a chain of creations ending in the player, and an external fill
        let chain = [
            Alias { id: 2, created: 0x1000, create_at: CREATE_IN | 1, ..Alias::default() },
            Alias { id: 1, created: 0x1001, create_at: CREATE_IN, ..Alias::default() },
            forced(0, PLAYER_REF),
            Alias { id: 3, external: Some(External { forced: 0, unique_actor: PLAYER_BASE }), ..Alias::default() },
        ];
        assert_eq!(classify(&chain, false), Class::Player);
    }

    #[test]
    fn one_world_alias_makes_the_quest_the_worlds() {
        // MS13 0x39645 (alias_fills' facts): Arvel's unique actor 0x39646
        // and the web forced 0x1fc43 beside the claw created in Arvel
        let ms13 = [
            Alias { id: 1, unique_actor: 0x39646, ..Alias::default() },
            forced(10, 0x1fc43),
            Alias { id: 11, created: 0x39647, create_at: CREATE_IN | 1, ..Alias::default() },
        ];
        assert_eq!(classify(&ms13, false), Class::World);
        assert_eq!(classify(&ms13, true), Class::World, "the list never overrides a world alias");
        // a location alias, a fill by conditions, a creation at the player's place
        assert_eq!(classify(&[forced(0, PLAYER_REF), Alias { id: 1, location: true, ..Alias::default() }], false), Class::World);
        assert_eq!(classify(&[forced(0, PLAYER_REF), Alias { id: 1, ..Alias::default() }], false), Class::World);
        assert_eq!(classify(&[forced(0, PLAYER_REF), Alias { id: 1, created: 0x1000, create_at: 0, ..Alias::default() }], false), Class::World);
        // an external fill from another quest's world alias, a creation in an
        // alias the quest lacks, a creation cycle
        assert_eq!(classify(&[Alias { id: 0, external: Some(External { forced: 0x1fc43, unique_actor: 0 }), ..Alias::default() }], false), Class::World);
        assert_eq!(classify(&[Alias { id: 0, created: 0x1000, create_at: CREATE_IN | 9, ..Alias::default() }], false), Class::World);
        let cycle = [Alias { id: 0, created: 0x1000, create_at: CREATE_IN | 1, ..Alias::default() }, Alias { id: 1, created: 0x1001, create_at: CREATE_IN, ..Alias::default() }];
        assert_eq!(classify(&cycle, false), Class::World);
    }

    #[test]
    fn a_quests_player_aliases_are_named_by_id_whatever_its_class() {
        // D-Q9 B's index reads them per quest: MQ101 0x3372b's player alias
        // 119 beside its NPC aliases (the lab's cached masters, 2026-11-10;
        // the forms illustrative), a quest of the world's class all the same
        let mq101 = [forced(119, PLAYER_REF), Alias { id: 12, unique_actor: 0x2bfb3, ..Alias::default() }, forced(13, 0x6e4b8)];
        assert_eq!(classify(&mq101, false), Class::World);
        assert_eq!(player_alias_ids(&mq101), vec![119]);
        // a unique actor, a creation in the player's inventory and an
        // external fill are the player's; a location alias never is
        let mixed = [
            Alias { id: 0, unique_actor: PLAYER_BASE, ..Alias::default() },
            Alias { id: 1, created: 0x39647, create_at: CREATE_IN, ..Alias::default() },
            Alias { id: 2, external: Some(External { forced: PLAYER_REF, unique_actor: 0 }), ..Alias::default() },
            Alias { id: 3, location: true, forced: PLAYER_REF, ..Alias::default() },
            Alias { id: 4, ..Alias::default() },
        ];
        assert_eq!(player_alias_ids(&mixed), vec![0, 1, 2]);
        assert!(player_alias_ids(&[]).is_empty());
    }

    #[test]
    fn a_quest_whose_start_reads_the_player_is_the_players_whatever_its_aliases() {
        // thuum D-VC2 B: VC01 0x5c625's aliases are Falion's unique actor
        // 0x135e9, a soul gem 0x2e500 created in him and two markers (the
        // census, 2026-11-15: alias 0 ALUA 0x135e9, 1 ALCO 0x2e500 in alias
        // 0, 2 and 3 forced 0x44389 and 0x1eb95), so the alias half makes it
        // World; the started half names it, so it is the player's
        let vc01 = [
            Alias { id: 0, unique_actor: 0x135e9, ..Alias::default() },
            Alias { id: 1, created: 0x2e500, create_at: CREATE_IN, ..Alias::default() },
            forced(2, 0x44389),
            forced(3, 0x1eb95),
        ];
        assert_eq!(classify(&vc01, false), Class::World);
        assert_eq!(class_of(&vc01, false, true), Class::Player);
        assert_eq!(class_of(&vc01, false, false), Class::World, "without the start fact the alias half decides");
        assert_eq!(class_of(&[], false, true), Class::Player, "with no alias, the started half alone");
        let scope = QuestScope::from_settings(r#""derived""#, r#"["0eafd5:Skyrim.esm"]"#, r#"["5c625:Skyrim.esm"]"#).unwrap_or_default();
        assert_eq!(scope.started(), &[FormDesc { short_id: 0x5c625, file: "Skyrim.esm".to_owned() }]);
        assert!(scope.is_started(0x5c625, "skyrim.esm") && !scope.is_started(0x5c625, "Dawnguard.esm"));
        assert_eq!(scope.class(&FormDesc { short_id: 0x5c625, file: "Skyrim.esm".to_owned() }, &vc01), Class::Player);
        // MS13 0x39645 has a world alias and no start fact: World, as before
        let ms13 = [Alias { id: 1, unique_actor: 0x39646, ..Alias::default() }, forced(10, 0x1fc43)];
        assert_eq!(scope.class(&FormDesc { short_id: 0x39645, file: "Skyrim.esm".to_owned() }, &ms13), Class::World);
        // the list half unchanged: a listed quest with no alias is Player, a
        // listed quest with a world alias stays World
        assert_eq!(scope.class(&FormDesc { short_id: 0xeafd5, file: "Skyrim.esm".to_owned() }, &[]), Class::Player);
        assert_eq!(scope.class(&FormDesc { short_id: 0xeafd5, file: "Skyrim.esm".to_owned() }, &ms13), Class::World);
    }

    #[test]
    fn a_quest_with_no_alias_is_the_lists() {
        // PlayerVampireQuest 0xeafd5 has no alias (read H-QS2): the list's
        let scope = QuestScope::from_settings(r#""derived""#, r#"["0eafd5:Skyrim.esm"]"#, "[]").unwrap_or_default();
        assert_eq!(scope.mode(), Mode::Derived);
        assert_eq!(scope.class(&FormDesc { short_id: 0xeafd5, file: "skyrim.esm".to_owned() }, &[]), Class::Player);
        assert_eq!(scope.class(&FormDesc { short_id: 0x74a83, file: "Skyrim.esm".to_owned() }, &[]), Class::World);
        assert_eq!((Class::World.code(), Class::Player.code()), (0, 1));
    }

    #[test]
    fn the_settings_parse_and_refuse_by_name() {
        let off = QuestScope::from_settings(r#""off""#, "[]", "[]");
        assert!(off.as_ref().is_ok_and(|s| s.mode() == Mode::Off && s.listed().is_empty() && s.started().is_empty()));
        assert_eq!(QuestScope::default().mode(), Mode::Off);
        let two = QuestScope::from_settings(r#""derived""#, r#"["0eafd5:Skyrim.esm", "0x2ba16:Skyrim.esm"]"#, "[]");
        assert!(two.as_ref().is_ok_and(|s| s.listed().len() == 2 && s.is_listed(0x2ba16, "SKYRIM.ESM") && !s.is_listed(0x2ba16, "Dawnguard.esm")));
        // the same quest in both lists is no error: each list is its own half
        let both = QuestScope::from_settings(r#""derived""#, r#"["5c625:Skyrim.esm"]"#, r#"["5c625:Skyrim.esm"]"#);
        assert!(both.as_ref().is_ok_and(|s| s.is_listed(0x5c625, "Skyrim.esm") && s.is_started(0x5c625, "Skyrim.esm")));
        for (mode, list) in [(r#""aliases""#, "[]"), ("1", "[]"), ("off", "[]"), (r#""off""#, r#"{"0eafd5:Skyrim.esm": 1}"#), (r#""off""#, r#"["0eafd5"]"#), (r#""off""#, r#"[7]"#), (r#""off""#, r#"["0eafd5:Skyrim.esm", "eafd5:Skyrim.esm"]"#)] {
            assert!(QuestScope::from_settings(mode, list, "[]").is_err(), "{mode} {list}");
            // the started half takes the list half's validation, by its own name
            if mode == r#""off""# {
                let refused = QuestScope::from_settings(mode, "[]", list);
                assert!(refused.as_ref().is_err_and(|e| e.0.starts_with("playerStartedQuests: ")), "{list}: {refused:?}");
            }
        }
    }

    // a form as the generator draws it: none, the player's two, or any
    fn form() -> impl Strategy<Value = u32> {
        prop_oneof![Just(0u32), Just(PLAYER_REF), Just(PLAYER_BASE), any::<u32>()]
    }

    fn alias() -> impl Strategy<Value = Alias> {
        let id = prop_oneof![0u32..4, any::<u32>()];
        let at = prop_oneof![Just(0u32), (0u32..4).prop_map(|t| CREATE_IN | t), any::<u32>()];
        let external = proptest::option::of((form(), form()).prop_map(|(forced, unique_actor)| External { forced, unique_actor }));
        (id, any::<bool>(), form(), form(), form(), at, external)
            .prop_map(|(id, location, forced, unique_actor, created, create_at, external)| Alias { id, location, forced, unique_actor, created, create_at, external })
    }

    proptest! {
        // the started half: a named quest is Player whatever its aliases and
        // the list; an unnamed one is the other two halves'
        #[test]
        fn a_named_quest_is_the_players_and_an_unnamed_one_the_other_halves(aliases in proptest::collection::vec(alias(), 0..8), listed in any::<bool>()) {
            prop_assert_eq!(class_of(&aliases, listed, true), Class::Player);
            prop_assert_eq!(class_of(&aliases, listed, false), classify(&aliases, listed));
        }

        // any facts: a class, never a panic; a quest with an alias that is
        // not the player's is World whatever the list says, one with none is
        // the list's, and a world alias never turns Player by another alias
        #[test]
        fn the_class_never_panics_and_a_world_alias_keeps_the_quest_the_worlds(aliases in proptest::collection::vec(alias(), 0..8), listed in any::<bool>()) {
            let class = classify(&aliases, listed);
            if aliases.is_empty() {
                prop_assert_eq!(class, if listed { Class::Player } else { Class::World });
            } else if aliases.iter().any(|a| !is_players(a, &aliases)) {
                prop_assert_eq!(class, Class::World);
            } else {
                prop_assert_eq!(class, Class::Player);
            }
            if aliases.iter().any(|a| a.location) {
                prop_assert_eq!(class, Class::World);
            }
        }
    }
}
