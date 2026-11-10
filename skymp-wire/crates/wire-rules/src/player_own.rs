//! A player's own rows (thuum ADR-068 rung 5, the two transformations: Eli's
//! "yes full transformation", D-T1 A; docs/research/rung5-quests-2026-11-09.md
//! section 8(b), reads 16, 17 and 19). A per-player quest's scripts change
//! their own player through six natives, Actor.SetRace, AddSpell,
//! RemoveSpell, AddShout, RemoveShout and EquipItem: the vampire's twin and
//! back (VampireChange, VampireCure), its stages' abilities, the werewolf's
//! beast form and its level abilities, the howl and WolfSkinFXArmor. Each is
//! a state row on the seat's own player alone (wire-rules natives,
//! RecordKind::PlayerOwn, its subject 0x14), carried by ScriptCall 75 after
//! the row's shape and owner steps, and judged here before the core moves
//! the player's record. R1, over facts the core gathers from the game files.
//!
//! The kind first: the call's form (its first argument) must be a record of
//! the row's type, a RACE for SetRace, a SPEL for AddSpell and RemoveSpell,
//! a SHOU for AddShout and RemoveShout, an item (libespm's item types) for
//! EquipItem, else `E_SCRIPTCALL_ARGS` (a non-race written to the look would
//! drop the player's whole change form at the next start: WorldState skips a
//! record whose appearance names no RACE). The core reads the type.
//!
//! The bound: the form must be one the calling script's live attachments
//! name, a property's value or a member of a FormList one (libespm's VMAD
//! and FLST), else `E_SCRIPTCALL_UNNAMED`: a modified seat can teach its
//! player nothing its scripts do not name. The calling class is the seat's
//! own word (ScriptCall's `caller`), so the core passes the properties of
//! the attachments live for this player alone (the fresh review's note 3):
//! a QUST's while the quest scope classes the quest the player's or the
//! world's journal records it running or staged, an MGEF's while the
//! player's record or overlay holds a SPEL that carries it. A property a
//! script assigns at run time is no VMAD value (Dawnguard's perks swap
//! CurrentHowl) and is refused by the same name, so its count is measured.
//! One form passes unnamed: a SetRace to the recorded race, the server's own
//! fact, which a script may hold in a variable it filled when the form began
//! (a run-time value no VMAD carries), so the race's return always lands.
//!
//! The twin rule, for SetRace: the deployed PlayerVampireQuestScript is
//! RaceCompatibility's override, pairing each race with its vampire twin by
//! index in two FormLists (RaceList and VampireList, read 16), so a race is
//! the recorded race's twin when two of the attachment's FormList properties
//! hold the recorded race and the new one at the same index (the turn reads
//! RaceList to VampireList, the cure the other way). A twin, or a race with
//! the Playable flag, is recorded on the change form; `restrictedRaces`
//! still applies (the race menu's own rule, wire-rules appearance: a
//! restricted race needs the player's profile on its permit,
//! `E_SCRIPTCALL_RACE` otherwise). The recorded race itself folds.
//!
//! The overlay (D-T1 A): a race neither Playable nor a twin (WerewolfBeastRace
//! 0xcdd84, DLC1VampireBeastRace 0x0200283a; read 19) opens one per player.
//! What the player's scripts add while it is open (spells, shouts, items) is
//! held in it, never written to the change form; the core carries it on the
//! figure's CreateActor and UpdateAppearance and counts it at the equipment
//! gate. It ends at the race's return (a SetRace to the recorded race) or at
//! the player's disconnect ([`PlayerOverlays::gone`]), so a relaunch mid-form
//! logs in as the player's own race. Runtime only, like the beast form it
//! carries: a restart has none. Its lists are bounded by the bound rule (a
//! form must be one the game files name for the calling script).

use std::collections::BTreeMap;

/// The six rows, by Papyrus class and name (natives_table.rs's order).
pub const ROWS: [(&str, &str); 6] = [("Actor", "SetRace"), ("Actor", "AddSpell"), ("Actor", "RemoveSpell"), ("Actor", "AddShout"), ("Actor", "RemoveShout"), ("Actor", "EquipItem")];

/// One of the six natives.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Row {
    /// Actor.SetRace(Race akRace).
    SetRace,
    /// Actor.AddSpell(SPELL akSpell, Bool abVerbose).
    AddSpell,
    /// Actor.RemoveSpell(SPELL akSpell).
    RemoveSpell,
    /// Actor.AddShout(Shout akShout).
    AddShout,
    /// Actor.RemoveShout(Shout akShout).
    RemoveShout,
    /// Actor.EquipItem(Form akItem, Bool abPreventRemoval, Bool abSilent).
    EquipItem,
}

impl Row {
    /// The row of `cls`.`func`, names compared without case; none for any
    /// other native.
    #[must_use]
    pub fn of(cls: &str, func: &str) -> Option<Self> {
        let at = ROWS.iter().position(|(c, f)| c.eq_ignore_ascii_case(cls) && f.eq_ignore_ascii_case(func))?;
        [Row::SetRace, Row::AddSpell, Row::RemoveSpell, Row::AddShout, Row::RemoveShout, Row::EquipItem].get(at).copied()
    }
}

/// One object the calling script's attachment names (libespm VMAD): an
/// object property's value, or one element of an object array property's,
/// with the entries of a FormList in order (empty for any other form).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Named {
    /// The form the property names.
    pub form: u32,
    /// Its FormList entries in order (FLST's LNAM), empty for no FormList.
    pub members: Vec<u32>,
}

/// Whether the attachment's `properties` name `form`: a property's value or
/// a member of a FormList one. 0 (None) is never named.
#[must_use]
pub fn named(form: u32, properties: &[Named]) -> bool {
    form != 0 && properties.iter().any(|p| p.form == form || p.members.contains(&form))
}

/// Whether `race` is `recorded`'s twin: two of the attachment's FormList
/// properties hold the recorded race and the new one at one index
/// (RaceCompatibility's RaceList and VampireList, read 16; either way, so
/// the cure pairs as the turn does).
#[must_use]
pub fn twins(recorded: u32, race: u32, properties: &[Named]) -> bool {
    if recorded == race || recorded == 0 || race == 0 {
        return false;
    }
    properties.iter().enumerate().any(|(a, from)| {
        from.members.iter().enumerate().filter(|&(_, &m)| m == recorded).any(|(i, _)| {
            properties.iter().enumerate().any(|(b, to)| a != b && to.members.get(i) == Some(&race))
        })
    })
}

/// What the core knows of one call beside its form.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct Facts {
    /// The player's race on its change form (an overlay never moves it).
    pub recorded_race: u32,
    /// The form is a RACE with the Playable flag (read for SetRace).
    pub playable: bool,
    /// The form is on `restrictedRaces` (read for SetRace).
    pub restricted: bool,
    /// The player's profile is on the form's permit list (read for SetRace).
    pub permitted: bool,
    /// The player's record holds the form: a learned spell (AddSpell,
    /// RemoveSpell), a learned shout (AddShout, RemoveShout), the item in
    /// its inventory (EquipItem).
    pub held: bool,
    /// The form's record is the row's kind: a RACE (SetRace), a SPEL
    /// (AddSpell, RemoveSpell), a SHOU (AddShout, RemoveShout), an item
    /// (EquipItem).
    pub kind: bool,
}

/// An open overlay: the race the player wears beyond its record and what
/// its scripts added meanwhile.
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct Overlay {
    /// The race worn (neither Playable nor a twin).
    pub race: u32,
    /// The recorded race when it opened, the race its end returns to.
    pub base: u32,
    /// Spells added while open, in order.
    pub spells: Vec<u32>,
    /// Shouts added while open, in order.
    pub shouts: Vec<u32>,
    /// Items equipped while open, in order.
    pub items: Vec<u32>,
}

/// Why a call is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// The form is no record of the row's kind (natives' argument code).
    Args,
    /// The form is no value of the calling script's live attachments, or
    /// an item outside an overlay that the player's record does not hold.
    Unnamed,
    /// A race `restrictedRaces` keeps from the player's profile.
    Race,
}

impl Code {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::Args => "E_SCRIPTCALL_ARGS",
            Code::Unnamed => "E_SCRIPTCALL_UNNAMED",
            Code::Race => "E_SCRIPTCALL_RACE",
        }
    }
}

impl Row {
    /// The record type the row's form must be, as the refusal names it.
    #[must_use]
    pub fn kind(self) -> &'static str {
        match self {
            Row::SetRace => "RACE",
            Row::AddSpell | Row::RemoveSpell => "SPEL",
            Row::AddShout | Row::RemoveShout => "SHOU",
            Row::EquipItem => "item",
        }
    }
}

/// The rule's word on one call.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    /// Written to the player's change form (the race, a learned spell or
    /// shout added or removed; never an item, which no row mints). `ended`
    /// an overlay this call closed (a twin or a Playable race taken while
    /// one was open), whose adds the core strips.
    Record {
        /// The overlay the call ended, if any.
        ended: Option<Overlay>,
    },
    /// Held in the player's overlay (opened, its race moved, a form added
    /// or removed), never written to the change form.
    Overlay,
    /// The overlay ended at the race's return: its race and its adds go.
    Ended(Overlay),
    /// Nothing moves: the record or the overlay holds it already.
    Fold,
    /// Refused, logged by the code's name with why.
    Refuse(Code, String),
}

/// Every player's open overlay, by its actor.
#[derive(Debug, Default)]
pub struct PlayerOverlays {
    open: BTreeMap<u32, Overlay>,
}

impl PlayerOverlays {
    /// The overlay `player` wears, none when it wears its record.
    #[must_use]
    pub fn overlay(&self, player: u32) -> Option<&Overlay> {
        self.open.get(&player)
    }

    /// `player` left: its overlay ends with the connection (D-T1 A), given
    /// back for the core's line.
    pub fn gone(&mut self, player: u32) -> Option<Overlay> {
        self.open.remove(&player)
    }

    /// One call of `row` by `player`'s own scripts naming `form`: the kind
    /// first, then the bound over the calling script's live attachments'
    /// `properties` (a SetRace to the recorded race passes it: the race's
    /// return).
    pub fn judge(&mut self, player: u32, row: Row, form: u32, properties: &[Named], facts: Facts) -> Verdict {
        if form == 0 {
            return Verdict::Refuse(Code::Unnamed, "the call names no form".to_owned());
        }
        if !facts.kind {
            return Verdict::Refuse(Code::Args, format!("{form:x} is no {} record, the form Actor.{row:?} takes", row.kind()));
        }
        let returns = row == Row::SetRace && form == facts.recorded_race;
        if !returns && !named(form, properties) {
            return Verdict::Refuse(Code::Unnamed, format!("{form:x} is no value of the calling script's live attachments, nor of a FormList one"));
        }
        if row == Row::SetRace {
            return self.set_race(player, form, properties, facts);
        }
        let Some(overlay) = self.open.get_mut(&player) else {
            // no overlay: the record's, an add of what it holds or a removal
            // of what it lacks folding; an item the record lacks is minted by
            // no row (R0's inventory is never a client's word: the fresh
            // review's note 4), so EquipItem folds or is refused
            if row == Row::EquipItem {
                return if facts.held { Verdict::Fold } else { Verdict::Refuse(Code::Unnamed, format!("{form:x} is no item the player's record holds, and outside an overlay EquipItem adds none")) };
            }
            let adds = matches!(row, Row::AddSpell | Row::AddShout);
            return if adds == facts.held { Verdict::Fold } else { Verdict::Record { ended: None } };
        };
        let list = match row {
            Row::AddSpell | Row::RemoveSpell => &mut overlay.spells,
            Row::AddShout | Row::RemoveShout => &mut overlay.shouts,
            Row::EquipItem | Row::SetRace => &mut overlay.items,
        };
        let in_overlay = list.contains(&form);
        match row {
            Row::AddSpell | Row::AddShout | Row::EquipItem if in_overlay || facts.held => Verdict::Fold,
            Row::AddSpell | Row::AddShout | Row::EquipItem => {
                list.push(form);
                Verdict::Overlay
            }
            // a removal: the overlay's own first, then the record's
            _ if in_overlay => {
                list.retain(|&f| f != form);
                Verdict::Overlay
            }
            _ if facts.held => Verdict::Record { ended: None },
            _ => Verdict::Fold,
        }
    }

    /// SetRace's judgment: the fold, the restriction, the twin or a Playable
    /// race recorded (ending an open overlay), the recorded race's return
    /// ending one, and any other race opening or moving one.
    fn set_race(&mut self, player: u32, race: u32, properties: &[Named], facts: Facts) -> Verdict {
        let open = self.open.get(&player).map(|o| o.race);
        if open == Some(race) || (open.is_none() && race == facts.recorded_race) {
            return Verdict::Fold;
        }
        if open.is_some() && race == facts.recorded_race {
            return self.open.remove(&player).map_or(Verdict::Fold, Verdict::Ended);
        }
        if facts.restricted && !facts.permitted {
            return Verdict::Refuse(Code::Race, format!("race {race:x} is on restrictedRaces and the player's profile is not on its permit"));
        }
        if facts.playable || twins(facts.recorded_race, race, properties) {
            return Verdict::Record { ended: self.open.remove(&player) };
        }
        match self.open.get_mut(&player) {
            Some(overlay) => overlay.race = race,
            None => {
                self.open.insert(player, Overlay { race, base: facts.recorded_race, ..Overlay::default() });
            }
        }
        Verdict::Overlay
    }
}

#[cfg(test)]
#[allow(clippy::panic)] // a test's failure message, never a packet's path
mod tests {
    use super::*;

    // the lab's actors and forms, ids from the plan's reads (docs/research/
    // rung5-quests-2026-11-09.md section 8): Ariadne's KhajiitRace and its
    // twin KhajiitRaceVampire 0x88845, WerewolfBeastRace 0xcdd84; the
    // FormLists' ids, the Khajiit race's and the spells' are illustrative
    // (RaceCompatibility's lists are UNVERIFIED by a read of the plugin)
    const ARIADNE: u32 = 0xff00_0001;
    const KHAJIIT: u32 = 0x13745;
    const KHAJIIT_VAMPIRE: u32 = 0x88845;
    const NORD: u32 = 0x13746;
    const NORD_VAMPIRE: u32 = 0x88794;
    const BEAST: u32 = 0xcdd84;
    const ABILITY: u32 = 0xed0a8;
    const HOWL: u32 = 0x92c48;
    const SKIN: u32 = 0x10d4f9;

    fn vampire_script() -> Vec<Named> {
        vec![
            Named { form: 0x0d00_1000, members: vec![NORD, KHAJIIT] },
            Named { form: 0x0d00_1001, members: vec![NORD_VAMPIRE, KHAJIIT_VAMPIRE] },
            Named { form: ABILITY, members: Vec::new() },
        ]
    }

    fn werewolf_script() -> Vec<Named> {
        [BEAST, HOWL, SKIN, ABILITY].iter().map(|&form| Named { form, members: Vec::new() }).collect()
    }

    fn khajiit() -> Facts {
        Facts { recorded_race: KHAJIIT, kind: true, ..Facts::default() }
    }

    #[test]
    fn the_six_rows_read_by_name() {
        assert_eq!(Row::of("Actor", "SetRace"), Some(Row::SetRace));
        assert_eq!(Row::of("actor", "equipitem"), Some(Row::EquipItem));
        assert_eq!(Row::of("Actor", "EquipSpell"), None);
        assert_eq!(Row::of("ObjectReference", "AddItem"), None);
        assert_eq!((Code::Args.name(), Code::Unnamed.name(), Code::Race.name()), ("E_SCRIPTCALL_ARGS", "E_SCRIPTCALL_UNNAMED", "E_SCRIPTCALL_RACE"));
        assert_eq!([Row::SetRace, Row::RemoveSpell, Row::AddShout, Row::EquipItem].map(Row::kind), ["RACE", "SPEL", "SHOU", "item"]);
    }

    #[test]
    fn a_form_the_attachment_does_not_name_is_refused() {
        let mut o = PlayerOverlays::default();
        assert!(named(KHAJIIT_VAMPIRE, &vampire_script()) && named(ABILITY, &vampire_script()));
        assert!(!named(0, &vampire_script()) && !named(HOWL, &vampire_script()));
        for (form, why) in [(HOWL, "92c48 is no value of the calling script's live attachments, nor of a FormList one"), (0, "the call names no form")] {
            match o.judge(ARIADNE, Row::AddSpell, form, &vampire_script(), khajiit()) {
                Verdict::Refuse(code, w) => assert_eq!((code, w.as_str()), (Code::Unnamed, why)),
                other => panic!("not refused: {other:?}"),
            }
        }
        // the race too, before the twin rule
        assert!(matches!(o.judge(ARIADNE, Row::SetRace, BEAST, &vampire_script(), khajiit()), Verdict::Refuse(Code::Unnamed, _)));
    }

    #[test]
    fn a_form_of_another_kind_is_refused_before_the_bound() {
        // the fresh review's note 3: a non-race written to the look drops the
        // player's change form at the next start, so the record's type is
        // judged first, named or not
        let mut o = PlayerOverlays::default();
        let wrong = Facts { kind: false, ..khajiit() };
        for (row, form, why) in [
            (Row::SetRace, ABILITY, "ed0a8 is no RACE record, the form Actor.SetRace takes"),
            (Row::AddSpell, KHAJIIT_VAMPIRE, "88845 is no SPEL record, the form Actor.AddSpell takes"),
            (Row::EquipItem, HOWL, "92c48 is no item record, the form Actor.EquipItem takes"),
        ] {
            assert_eq!(o.judge(ARIADNE, row, form, &vampire_script(), wrong), Verdict::Refuse(Code::Args, why.to_owned()));
        }
        assert!(o.overlay(ARIADNE).is_none());
    }

    #[test]
    fn an_item_outside_an_overlay_folds_when_held_and_is_refused_when_not() {
        // the fresh review's note 4: no row mints an item (R0's inventory)
        let mut o = PlayerOverlays::default();
        let script = werewolf_script();
        let nord = Facts { recorded_race: NORD, kind: true, ..Facts::default() };
        assert_eq!(o.judge(ARIADNE, Row::EquipItem, SKIN, &script, Facts { held: true, ..nord }), Verdict::Fold);
        match o.judge(ARIADNE, Row::EquipItem, SKIN, &script, nord) {
            Verdict::Refuse(Code::Unnamed, why) => assert_eq!(why, "10d4f9 is no item the player's record holds, and outside an overlay EquipItem adds none"),
            other => panic!("not refused: {other:?}"),
        }
        // inside one the item is the overlay's, as before
        assert_eq!(o.judge(ARIADNE, Row::SetRace, BEAST, &script, nord), Verdict::Overlay);
        assert_eq!(o.judge(ARIADNE, Row::EquipItem, SKIN, &script, nord), Verdict::Overlay);
    }

    #[test]
    fn the_twin_is_recorded_and_the_cure_returns_it() {
        let mut o = PlayerOverlays::default();
        assert!(twins(KHAJIIT, KHAJIIT_VAMPIRE, &vampire_script()) && twins(KHAJIIT_VAMPIRE, KHAJIIT, &vampire_script()));
        assert!(!twins(KHAJIIT, NORD_VAMPIRE, &vampire_script()), "another index is no twin");
        assert!(!twins(KHAJIIT, KHAJIIT, &vampire_script()));
        assert_eq!(o.judge(ARIADNE, Row::SetRace, KHAJIIT_VAMPIRE, &vampire_script(), khajiit()), Verdict::Record { ended: None });
        let turned = Facts { recorded_race: KHAJIIT_VAMPIRE, kind: true, ..Facts::default() };
        assert_eq!(o.judge(ARIADNE, Row::SetRace, KHAJIIT, &vampire_script(), turned), Verdict::Record { ended: None });
        // the recorded race folds; a stage's ability is the record's
        assert_eq!(o.judge(ARIADNE, Row::SetRace, KHAJIIT, &vampire_script(), khajiit()), Verdict::Fold);
        assert_eq!(o.judge(ARIADNE, Row::AddSpell, ABILITY, &vampire_script(), khajiit()), Verdict::Record { ended: None });
        assert_eq!(o.judge(ARIADNE, Row::AddSpell, ABILITY, &vampire_script(), Facts { held: true, ..khajiit() }), Verdict::Fold);
        assert_eq!(o.judge(ARIADNE, Row::RemoveSpell, ABILITY, &vampire_script(), Facts { held: true, ..khajiit() }), Verdict::Record { ended: None });
        assert_eq!(o.judge(ARIADNE, Row::RemoveSpell, ABILITY, &vampire_script(), khajiit()), Verdict::Fold);
        assert!(o.overlay(ARIADNE).is_none());
    }

    #[test]
    fn a_restricted_race_needs_the_permit() {
        // rotfernRaceVampire 0x19d08: Playable, restricted to profile 1
        let mut o = PlayerOverlays::default();
        let script = [Named { form: 0x19d08, members: Vec::new() }];
        let facts = Facts { recorded_race: 0x19d07, playable: true, restricted: true, kind: true, ..Facts::default() };
        assert!(matches!(o.judge(ARIADNE, Row::SetRace, 0x19d08, &script, facts), Verdict::Refuse(Code::Race, _)));
        assert_eq!(o.judge(ARIADNE, Row::SetRace, 0x19d08, &script, Facts { permitted: true, ..facts }), Verdict::Record { ended: None });
    }

    #[test]
    fn the_beast_form_is_an_overlay_its_adds_stay_off_the_record_and_its_end_returns_them() {
        let mut o = PlayerOverlays::default();
        let rotfern = 0xff00_0000;
        let script = werewolf_script();
        let nord = Facts { recorded_race: NORD, kind: true, ..Facts::default() };
        // the beast race: neither Playable nor a twin, an overlay
        assert_eq!(o.judge(rotfern, Row::SetRace, BEAST, &script, nord), Verdict::Overlay);
        assert_eq!(o.overlay(rotfern).map(|v| (v.race, v.base)), Some((BEAST, NORD)));
        // its level ability, the howl and the skin go into it, once each
        assert_eq!(o.judge(rotfern, Row::AddSpell, ABILITY, &script, nord), Verdict::Overlay);
        assert_eq!(o.judge(rotfern, Row::AddSpell, ABILITY, &script, nord), Verdict::Fold);
        assert_eq!(o.judge(rotfern, Row::AddShout, HOWL, &script, nord), Verdict::Overlay);
        assert_eq!(o.judge(rotfern, Row::EquipItem, SKIN, &script, nord), Verdict::Overlay);
        // what the record holds already folds; a removal takes the overlay's first
        assert_eq!(o.judge(rotfern, Row::AddShout, HOWL, &script, Facts { held: true, ..nord }), Verdict::Fold);
        assert_eq!(o.judge(rotfern, Row::RemoveShout, HOWL, &script, nord), Verdict::Overlay);
        assert_eq!(o.judge(rotfern, Row::RemoveShout, HOWL, &script, Facts { held: true, ..nord }), Verdict::Record { ended: None });
        assert_eq!(o.judge(rotfern, Row::RemoveShout, HOWL, &script, nord), Verdict::Fold);
        assert_eq!(o.judge(rotfern, Row::SetRace, BEAST, &script, nord), Verdict::Fold);
        // the race's return ends it, its adds given back to strip; the
        // recorded race passes the bound though no property names it
        assert!(!named(NORD, &script));
        let ended = Overlay { race: BEAST, base: NORD, spells: vec![ABILITY], shouts: Vec::new(), items: vec![SKIN] };
        assert_eq!(o.judge(rotfern, Row::SetRace, NORD, &script, nord), Verdict::Ended(ended));
        assert!(o.overlay(rotfern).is_none());
        // a disconnect mid-form ends it too, and the other player's is its own
        assert_eq!(o.judge(rotfern, Row::SetRace, BEAST, &script, nord), Verdict::Overlay);
        assert!(o.overlay(ARIADNE).is_none());
        assert_eq!(o.gone(rotfern).map(|v| v.race), Some(BEAST));
        assert_eq!(o.gone(rotfern), None);
    }

    #[test]
    fn a_twin_or_playable_race_taken_mid_form_ends_the_overlay_and_is_recorded() {
        let mut o = PlayerOverlays::default();
        let mut script = werewolf_script();
        script.extend(vampire_script());
        let nord = Facts { recorded_race: NORD, kind: true, ..Facts::default() };
        assert_eq!(o.judge(ARIADNE, Row::SetRace, BEAST, &script, nord), Verdict::Overlay);
        assert_eq!(o.judge(ARIADNE, Row::AddSpell, ABILITY, &script, nord), Verdict::Overlay);
        // another race neither Playable nor a twin moves the overlay's race
        assert_eq!(o.judge(ARIADNE, Row::SetRace, HOWL, &script, nord), Verdict::Overlay);
        assert_eq!(o.overlay(ARIADNE).map(|v| (v.race, v.base, v.spells.len())), Some((HOWL, NORD, 1)));
        let ended = Overlay { race: HOWL, base: NORD, spells: vec![ABILITY], ..Overlay::default() };
        assert_eq!(o.judge(ARIADNE, Row::SetRace, NORD_VAMPIRE, &script, nord), Verdict::Record { ended: Some(ended) });
        assert!(o.overlay(ARIADNE).is_none());
    }
}
