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
//! Eli's D-E1 A adds a seventh in the six's shape, Actor.ChangeHeadPart, a
//! script's swap of its own player's head part (rotfern's eyes): past the
//! kind and the bound, the head part must be one the player's recorded race
//! may wear (its HDPT's valid-race FormList, RNAM, holds the race), else
//! `E_SCRIPTCALL_UNNAMED`; one the look wears already folds, any other is
//! recorded on the look (the core swaps the part of the same HDPT type).
//!
//! Eli's D-R1 C (thuum docs/DECISIONS.md, rung 5's stage-eve words) adds the
//! inventory's two in the same shape, ObjectReference.AddItem and RemoveItem
//! on the seat's own player: a quest's reward is the seat's own record, R2
//! (rule 5's "record it as unvalidated"), so neither row has the bound ("the
//! most likely to fire every time": a bound would refuse a leveled reward or
//! an item a script computes at run time), and they amend the fresh review's
//! note 4 (no row minted an item) for these two alone. The kind holds (an
//! item of libespm's item types, else `E_SCRIPTCALL_ARGS`), and so does the
//! count's own bound ([`item_count`]: a whole number from 1 to the Papyrus
//! Int's largest). An add is recorded as reported, at most to the record's
//! count bound; a removal takes at most what the record holds (never below
//! zero), the overlay's own first (the werewolf's WolfSkinFXArmor, which its
//! EquipItem put there, removed before the race's return), and folds into a
//! container the player holds open, whose own move (PutItem) carries it. A
//! leveled list is refused by name (`E_SCRIPTCALL_LEVELED`): the engine's
//! roll is what landed, and the report carries the list, not the roll (the
//! named gap; the record never holds the list's form). Other seats run a
//! relayed fragment on the acting player's figure (rung 5's acting scope),
//! so the grant is reported once, by the acting seat, on its own 0x14.
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

/// The nine rows, by Papyrus class and name (natives_table.rs's order: the
/// flip's AddItem and RemoveItem first, D-R1 C keeping them in place).
pub const ROWS: [(&str, &str); 9] = [("ObjectReference", "AddItem"), ("ObjectReference", "RemoveItem"), ("Actor", "SetRace"), ("Actor", "AddSpell"), ("Actor", "RemoveSpell"), ("Actor", "AddShout"), ("Actor", "RemoveShout"), ("Actor", "EquipItem"), ("Actor", "ChangeHeadPart")];

/// One of the nine natives.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Row {
    /// ObjectReference.AddItem(Form akItemToAdd, Int aiCount, Bool abSilent)
    /// (D-R1 C).
    AddItem,
    /// ObjectReference.RemoveItem(Form akItemToRemove, Int aiCount, Bool
    /// abSilent, ObjectReference akOtherContainer) (D-R1 C).
    RemoveItem,
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
    /// Actor.ChangeHeadPart(HeadPart hPart) (D-E1 A).
    ChangeHeadPart,
}

impl Row {
    /// The row of `cls`.`func`, names compared without case; none for any
    /// other native.
    #[must_use]
    pub fn of(cls: &str, func: &str) -> Option<Self> {
        let at = ROWS.iter().position(|(c, f)| c.eq_ignore_ascii_case(cls) && f.eq_ignore_ascii_case(func))?;
        [Row::AddItem, Row::RemoveItem, Row::SetRace, Row::AddSpell, Row::RemoveSpell, Row::AddShout, Row::RemoveShout, Row::EquipItem, Row::ChangeHeadPart].get(at).copied()
    }

    /// The row's Papyrus class, as a refusal names its native.
    #[must_use]
    pub fn class(self) -> &'static str {
        match self {
            Row::AddItem | Row::RemoveItem => "ObjectReference",
            Row::SetRace | Row::AddSpell | Row::RemoveSpell | Row::AddShout | Row::RemoveShout | Row::EquipItem | Row::ChangeHeadPart => "Actor",
        }
    }
}

/// The call's count (its second argument, aiCount, as the wire carried it)
/// as a count of items for AddItem and RemoveItem (D-R1 C): a whole number
/// from 1 to 2147483647, the Papyrus Int's largest (the platform reads the
/// frame's Int as itself, ReadArg in skyrim-platform ScriptNatives.cpp); none
/// for any other number, a count of none or below landing nothing and no
/// seat's frame carrying a fraction or a number past an Int.
#[must_use]
pub fn item_count(count: f64) -> Option<u32> {
    if !count.is_finite() || count.fract() != 0.0 || !(1.0..=f64::from(i32::MAX)).contains(&count) {
        return None;
    }
    #[allow(clippy::as_conversions, clippy::cast_possible_truncation, clippy::cast_sign_loss)] // range checked above
    Some(count as u32)
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
    /// (EquipItem, AddItem, RemoveItem), an HDPT (ChangeHeadPart).
    pub kind: bool,
    /// ChangeHeadPart's head part may be worn by the recorded race: its
    /// HDPT's valid-race FormList (RNAM) holds the race.
    pub race_valid: bool,
    /// AddItem's form is a leveled item list (LVLI), whose roll is the
    /// engine's (D-R1 C's named gap).
    pub leveled: bool,
    /// How many of the form the player's record holds over all its stacks
    /// (AddItem's room below the count's bound, RemoveItem's clamp).
    pub held_count: u32,
    /// AddItem's and RemoveItem's count of items ([`item_count`] of the
    /// call's aiCount), none when the number lands nothing.
    pub count: Option<u32>,
    /// RemoveItem's akOtherContainer is a container the player holds open:
    /// that container's own move (PutItem) carries the removal.
    pub into_held_open: bool,
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
    /// Items equipped while open, in order (a RemoveItem of one takes it
    /// from here first, D-R1 C).
    pub items: Vec<u32>,
}

/// Why a call is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// The form is no record of the row's kind (natives' argument code), or
    /// AddItem's or RemoveItem's count lands nothing.
    Args,
    /// The form is no value of the calling script's live attachments, or
    /// an item outside an overlay that the player's record does not hold.
    Unnamed,
    /// A race `restrictedRaces` keeps from the player's profile.
    Race,
    /// AddItem's form is a leveled list: what landed is the engine's roll,
    /// which the report does not carry (D-R1 C's named gap).
    Leveled,
}

impl Code {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::Args => "E_SCRIPTCALL_ARGS",
            Code::Unnamed => "E_SCRIPTCALL_UNNAMED",
            Code::Race => "E_SCRIPTCALL_RACE",
            Code::Leveled => "E_SCRIPTCALL_LEVELED",
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
            Row::EquipItem | Row::AddItem | Row::RemoveItem => "item",
            Row::ChangeHeadPart => "HDPT",
        }
    }
}

/// The rule's word on one call.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    /// Written to the player's change form (the race, a learned spell or
    /// shout added or removed, a head part on the look; never an item, which
    /// only the two inventory rows write, [`Verdict::Items`]). `ended` an
    /// overlay this call closed (a twin or a Playable race taken while one
    /// was open), whose adds the core strips.
    Record {
        /// The overlay the call ended, if any.
        ended: Option<Overlay>,
    },
    /// Written to the player's inventory on its change form (D-R1 C): this
    /// many of the form added (AddItem, as reported, at most to the record's
    /// count bound) or removed (RemoveItem, at most what the record holds).
    Items(u32),
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
    /// return). AddItem and RemoveItem have no bound (D-R1 C, the inventory's
    /// own judgment below).
    pub fn judge(&mut self, player: u32, row: Row, form: u32, properties: &[Named], facts: Facts) -> Verdict {
        if form == 0 {
            return Verdict::Refuse(Code::Unnamed, "the call names no form".to_owned());
        }
        if matches!(row, Row::AddItem | Row::RemoveItem) {
            return self.items(player, row, form, facts);
        }
        if !facts.kind {
            return Verdict::Refuse(Code::Args, format!("{form:x} is no {} record, the form {}.{row:?} takes", row.kind(), row.class()));
        }
        let returns = row == Row::SetRace && form == facts.recorded_race;
        if !returns && !named(form, properties) {
            return Verdict::Refuse(Code::Unnamed, format!("{form:x} is no value of the calling script's live attachments, nor of a FormList one"));
        }
        if row == Row::SetRace {
            return self.set_race(player, form, properties, facts);
        }
        if row == Row::ChangeHeadPart {
            // D-E1 A: the look's, overlay or not (an overlay's race is worn
            // over the look, whose head parts are the record's)
            if !facts.race_valid {
                return Verdict::Refuse(Code::Unnamed, format!("{form:x} is no head part the recorded race {:x} may wear (its HDPT's valid-race list, RNAM, lacks the race)", facts.recorded_race));
            }
            return if facts.held { Verdict::Fold } else { Verdict::Record { ended: None } };
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
            Row::EquipItem | Row::SetRace | Row::ChangeHeadPart | Row::AddItem | Row::RemoveItem => &mut overlay.items,
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

    /// AddItem's and RemoveItem's judgment (D-R1 C, R2): the seat's report
    /// recorded as it ran, with no bound. A leveled list's add is refused by
    /// name (the engine's roll is what landed; the record never holds the
    /// list), any other form must be an item and the count a Papyrus Int
    /// above zero. An add is recorded whole, but never past the record's
    /// count bound (u32, over all the form's stacks); a removal into a
    /// container the player holds open folds (its PutItem moves the item,
    /// both sides), one of an item the open overlay holds takes it from
    /// there (the beast form's skin), and any other takes at most what the
    /// record holds, a removal of what it lacks folding.
    fn items(&mut self, player: u32, row: Row, form: u32, facts: Facts) -> Verdict {
        if row == Row::AddItem && facts.leveled {
            return Verdict::Refuse(Code::Leveled, format!("{form:x} is a leveled list (LVLI): what landed is the engine's roll, which the report does not carry, so nothing is written (D-R1 C's named gap)"));
        }
        if !facts.kind {
            return Verdict::Refuse(Code::Args, format!("{form:x} is no {} record, the form {}.{row:?} takes", row.kind(), row.class()));
        }
        let Some(count) = facts.count else {
            return Verdict::Refuse(Code::Args, format!("the count of {form:x} is no whole number from 1 to {}, a Papyrus Int above zero: nothing lands", i32::MAX));
        };
        if row == Row::AddItem {
            let room = u32::MAX.saturating_sub(facts.held_count);
            return match count.min(room) {
                0 => Verdict::Fold,
                add => Verdict::Items(add),
            };
        }
        if facts.into_held_open {
            return Verdict::Fold;
        }
        if let Some(overlay) = self.open.get_mut(&player) {
            if overlay.items.contains(&form) {
                overlay.items.retain(|&f| f != form);
                return Verdict::Overlay;
            }
        }
        match count.min(facts.held_count) {
            0 => Verdict::Fold,
            take => Verdict::Items(take),
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
    fn the_nine_rows_read_by_name() {
        assert_eq!(Row::of("Actor", "SetRace"), Some(Row::SetRace));
        assert_eq!(Row::of("actor", "equipitem"), Some(Row::EquipItem));
        assert_eq!(Row::of("Actor", "EquipSpell"), None);
        assert_eq!((Code::Args.name(), Code::Unnamed.name(), Code::Race.name()), ("E_SCRIPTCALL_ARGS", "E_SCRIPTCALL_UNNAMED", "E_SCRIPTCALL_RACE"));
        assert_eq!([Row::SetRace, Row::RemoveSpell, Row::AddShout, Row::EquipItem, Row::ChangeHeadPart].map(Row::kind), ["RACE", "SPEL", "SHOU", "item", "HDPT"]);
        assert_eq!(Row::of("actor", "changeheadpart"), Some(Row::ChangeHeadPart));
        // D-R1 C: the inventory's two by their own class, which no Actor
        // spelling reaches (the table holds them as ObjectReference's)
        assert_eq!(Row::of("ObjectReference", "AddItem"), Some(Row::AddItem));
        assert_eq!(Row::of("objectreference", "removeitem"), Some(Row::RemoveItem));
        assert_eq!(Row::of("Actor", "AddItem"), None);
        assert_eq!([Row::AddItem, Row::RemoveItem, Row::SetRace].map(Row::class), ["ObjectReference", "ObjectReference", "Actor"]);
        assert_eq!([Row::AddItem, Row::RemoveItem].map(Row::kind), ["item", "item"]);
        assert_eq!(Code::Leveled.name(), "E_SCRIPTCALL_LEVELED");
    }

    // Eli's D-R1 C, x-acting-frame 20261110-072506's fragment: A Lovely
    // Letter's stage 310 or 320, `Game.GetPlayer().AddItem(Gold, 25, False)`
    // (Gold a MiscObject property, 0xf; docs/research/
    // rung5-quests-2026-11-09.md, its stage list)
    const GOLD: u32 = 0xf;

    fn gold(held_count: u32, count: u32) -> Facts {
        Facts { kind: true, held_count, count: Some(count), ..Facts::default() }
    }

    #[test]
    fn a_count_is_a_papyrus_int_above_zero() {
        assert_eq!([25.0, 1.0, 2_147_483_647.0].map(item_count), [Some(25), Some(1), Some(2_147_483_647)]);
        assert_eq!([0.0, -1.0, 2.5, 2_147_483_648.0, f64::NAN, f64::INFINITY].map(item_count), [None; 6]);
    }

    #[test]
    fn a_grant_to_the_players_own_inventory_is_recorded_as_reported_with_no_bound() {
        let mut o = PlayerOverlays::default();
        // no property of any attachment names the gold, and none needs to
        assert_eq!(o.judge(ARIADNE, Row::AddItem, GOLD, &[], gold(0, 25)), Verdict::Items(25));
        assert_eq!(o.judge(ARIADNE, Row::AddItem, GOLD, &[], gold(40, 25)), Verdict::Items(25));
        // the record's count bound: an add stops at u32's largest, and one
        // with no room left folds
        assert_eq!(o.judge(ARIADNE, Row::AddItem, GOLD, &[], gold(u32::MAX - 10, 25)), Verdict::Items(10));
        assert_eq!(o.judge(ARIADNE, Row::AddItem, GOLD, &[], gold(u32::MAX, 25)), Verdict::Fold);
        // a leveled list: refused by name, the record never holds the list
        // (LItemFoodCabbage75 0x10bf6a, unit/LeveledListUtilsTest.cpp)
        match o.judge(ARIADNE, Row::AddItem, 0x10_bf6a, &[], Facts { leveled: true, kind: false, ..gold(0, 1) }) {
            Verdict::Refuse(code, why) => assert_eq!((code, why.as_str()), (Code::Leveled, "10bf6a is a leveled list (LVLI): what landed is the engine's roll, which the report does not carry, so nothing is written (D-R1 C's named gap)")),
            other => panic!("not refused: {other:?}"),
        }
        // the kind, the count's bound, the form
        match o.judge(ARIADNE, Row::AddItem, HOWL, &[], Facts { kind: false, ..gold(0, 1) }) {
            Verdict::Refuse(code, why) => assert_eq!((code, why.as_str()), (Code::Args, "92c48 is no item record, the form ObjectReference.AddItem takes")),
            other => panic!("not refused: {other:?}"),
        }
        match o.judge(ARIADNE, Row::AddItem, GOLD, &[], Facts { count: None, ..gold(0, 1) }) {
            Verdict::Refuse(code, why) => assert_eq!((code, why.as_str()), (Code::Args, "the count of f is no whole number from 1 to 2147483647, a Papyrus Int above zero: nothing lands")),
            other => panic!("not refused: {other:?}"),
        }
        assert!(matches!(o.judge(ARIADNE, Row::AddItem, 0, &[], gold(0, 25)), Verdict::Refuse(Code::Unnamed, _)));
        // an open overlay leaves a grant the record's: a reward outlasts the form
        let nord = Facts { recorded_race: NORD, kind: true, ..Facts::default() };
        assert_eq!(o.judge(ARIADNE, Row::SetRace, BEAST, &werewolf_script(), nord), Verdict::Overlay);
        assert_eq!(o.judge(ARIADNE, Row::AddItem, GOLD, &[], gold(0, 25)), Verdict::Items(25));
        assert_eq!(o.overlay(ARIADNE).map(|v| v.items.len()), Some(0));
    }

    #[test]
    fn a_removal_is_never_below_zero_takes_the_overlays_own_first_and_folds_into_a_held_open_container() {
        let mut o = PlayerOverlays::default();
        assert_eq!(o.judge(ARIADNE, Row::RemoveItem, GOLD, &[], gold(25, 10)), Verdict::Items(10));
        assert_eq!(o.judge(ARIADNE, Row::RemoveItem, GOLD, &[], gold(15, 1000)), Verdict::Items(15));
        assert_eq!(o.judge(ARIADNE, Row::RemoveItem, GOLD, &[], gold(0, 1)), Verdict::Fold);
        // into a container the player holds open: its PutItem moves it
        assert_eq!(o.judge(ARIADNE, Row::RemoveItem, GOLD, &[], Facts { into_held_open: true, ..gold(25, 10) }), Verdict::Fold);
        // a leveled list is no item to remove (the engine takes its members alone)
        assert!(matches!(o.judge(ARIADNE, Row::RemoveItem, 0x10_bf6a, &[], Facts { leveled: true, kind: false, ..gold(0, 1) }), Verdict::Refuse(Code::Args, _)));
        // the werewolf's shift back: RemoveItem(WolfSkinFXArmor, 1, True)
        // ahead of the race's return takes the skin its EquipItem put in
        // the overlay, never the record's count
        let script = werewolf_script();
        let nord = Facts { recorded_race: NORD, kind: true, ..Facts::default() };
        assert_eq!(o.judge(ARIADNE, Row::SetRace, BEAST, &script, nord), Verdict::Overlay);
        assert_eq!(o.judge(ARIADNE, Row::EquipItem, SKIN, &script, nord), Verdict::Overlay);
        assert_eq!(o.judge(ARIADNE, Row::RemoveItem, SKIN, &[], gold(0, 1)), Verdict::Overlay);
        assert_eq!(o.overlay(ARIADNE).map(|v| v.items.len()), Some(0));
        assert_eq!(o.judge(ARIADNE, Row::RemoveItem, SKIN, &[], gold(0, 1)), Verdict::Fold);
        let ended = Overlay { race: BEAST, base: NORD, ..Overlay::default() };
        assert_eq!(o.judge(ARIADNE, Row::SetRace, NORD, &script, nord), Verdict::Ended(ended));
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
    fn a_head_part_the_recorded_race_may_wear_is_recorded_and_any_other_refused() {
        // Eli's D-E1 A, rotfern's eyes: RotfernEyesMora for RotfernEyes, both
        // HDPTs whose valid-race list holds race rotfern (rotfern.esp's ids
        // illustrative here; lab/esm.py on sky-srv, 2026-11-10)
        const ROTFERN: u32 = 0x0e00_aa00;
        const EYES: u32 = 0x0e01_4c06;
        const MORA: u32 = 0x0e02_3f0e;
        let mut o = PlayerOverlays::default();
        let script = [Named { form: MORA, members: Vec::new() }, Named { form: EYES, members: Vec::new() }];
        let worn = Facts { recorded_race: ROTFERN, kind: true, race_valid: true, ..Facts::default() };
        assert_eq!(o.judge(ARIADNE, Row::ChangeHeadPart, MORA, &script, worn), Verdict::Record { ended: None });
        assert_eq!(o.judge(ARIADNE, Row::ChangeHeadPart, EYES, &script, Facts { held: true, ..worn }), Verdict::Fold);
        // a race the part's list lacks, an unnamed part, another kind
        match o.judge(ARIADNE, Row::ChangeHeadPart, MORA, &script, Facts { race_valid: false, ..worn }) {
            Verdict::Refuse(Code::Unnamed, why) => assert_eq!(why, "e023f0e is no head part the recorded race e00aa00 may wear (its HDPT's valid-race list, RNAM, lacks the race)"),
            other => panic!("not refused: {other:?}"),
        }
        assert!(matches!(o.judge(ARIADNE, Row::ChangeHeadPart, 0x0e01_0001, &script, worn), Verdict::Refuse(Code::Unnamed, _)));
        assert!(matches!(o.judge(ARIADNE, Row::ChangeHeadPart, MORA, &script, Facts { kind: false, ..worn }), Verdict::Refuse(Code::Args, _)));
        // an open overlay leaves it the look's
        assert_eq!(o.judge(ARIADNE, Row::SetRace, BEAST, &[Named { form: BEAST, members: Vec::new() }], worn), Verdict::Overlay);
        assert_eq!(o.judge(ARIADNE, Row::ChangeHeadPart, MORA, &script, worn), Verdict::Record { ended: None });
        assert_eq!(o.overlay(ARIADNE).map(|v| v.items.len()), Some(0));
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
