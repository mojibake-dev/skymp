//! Leveling (thuum docs/verbs/leveling.md, M5 Progression). A skill rises by
//! use and the character levels by skill gains, computed here from the
//! game's documented formulas so one number stands for a skill on every seat
//! and across a restart; the player's own engine advances nothing (its skill
//! experience is off under skymp5-client). The use's base amount is the
//! engine's and arrives reported (R1, bounded); everything above it is the
//! server's (R0).
//!
//! The formulas (UESP, Skyrim:Leveling, read 2026-10-31): a use earns
//! `useMult * base + useOffset`; the step from skill level L to L + 1 costs
//! `improveMult * L^fSkillUseCurve + improveOffset` with fSkillUseCurve 1.95
//! (the convention pinned against the page's totals from 15 to 100: 528,804
//! for a skill at 2 and 0, 278,852 Enchanting, 91,600 Smithing and
//! Lockpicking, 87,350 Pickpocket, 142,401 Sneak, 428,568 Alchemy); a skill
//! level gained adds `level * fXPPerSkillRank` (1) to the character's
//! experience, whose threshold at level N is `fXPLevelUpBase +
//! fXPLevelUpMult * N` (75 + 25 N). The four AVSK floats per skill are the
//! game files' (ActorValueInfo::Skill, CommonLibSSE-NG ActorValueInfo.h:34-41);
//! libespm reads no AVIF yet, so the table here carries UESP's reading of
//! them, to be replaced by the records when the reader lands.

/// The eighteen skills in the engine's ActorValue order, kOneHanded 6 to
/// kEnchanting 23 (CommonLibSSE-NG ActorValues.h).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Skill {
    /// ActorValue 6.
    OneHanded,
    /// 7.
    TwoHanded,
    /// 8 (Marksman).
    Archery,
    /// 9.
    Block,
    /// 10.
    Smithing,
    /// 11.
    HeavyArmor,
    /// 12.
    LightArmor,
    /// 13.
    Pickpocket,
    /// 14.
    Lockpicking,
    /// 15.
    Sneak,
    /// 16.
    Alchemy,
    /// 17.
    Speech,
    /// 18.
    Alteration,
    /// 19.
    Conjuration,
    /// 20.
    Destruction,
    /// 21.
    Illusion,
    /// 22.
    Restoration,
    /// 23.
    Enchanting,
}

/// The engine's skill experience constants of one skill (AVSK).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Constants {
    /// Skill Use Mult: the use's base amount is multiplied by it.
    pub use_mult: f32,
    /// Skill Use Offset: added to every use.
    pub use_offset: f32,
    /// Skill Improve Mult: the level step's multiplier.
    pub improve_mult: f32,
    /// Skill Improve Offset: added to every level step.
    pub improve_offset: f32,
}

/// fSkillUseCurve: the exponent of the level step's cost.
pub const SKILL_USE_CURVE: f32 = 1.95;
/// fXPPerSkillRank: character experience per skill level gained, times the
/// level gained.
pub const XP_PER_SKILL_RANK: f32 = 1.0;
/// fXPLevelUpBase.
pub const XP_LEVEL_UP_BASE: f32 = 75.0;
/// fXPLevelUpMult.
pub const XP_LEVEL_UP_MULT: f32 = 25.0;
/// A skill's floor: where a Legendary reset puts it and where every skill
/// starts before the racial bonus.
pub const SKILL_FLOOR: u8 = 15;
/// A skill's cap: no use earns past it.
pub const SKILL_CAP: u8 = 100;
/// The character level's bound here (actor-values' bound; the game has no
/// hard cap).
pub const LEVEL_CAP: u16 = 1000;
/// The most a single use's base amount may carry (UNMEASURED: the reference
/// trace refines it per skill; a Smithing craft of a Daedric piece is in the
/// thousands, a hit is in the tens).
pub const MAX_BASE_PER_USE: f32 = 100_000.0;
/// The first skill's ActorValue.
const FIRST_ACTOR_VALUE: u8 = 6;

impl Skill {
    /// Every skill, in ActorValue order.
    pub const ALL: [Self; 18] = [
        Self::OneHanded,
        Self::TwoHanded,
        Self::Archery,
        Self::Block,
        Self::Smithing,
        Self::HeavyArmor,
        Self::LightArmor,
        Self::Pickpocket,
        Self::Lockpicking,
        Self::Sneak,
        Self::Alchemy,
        Self::Speech,
        Self::Alteration,
        Self::Conjuration,
        Self::Destruction,
        Self::Illusion,
        Self::Restoration,
        Self::Enchanting,
    ];

    /// The skill whose ActorValue is `av`, 6 to 23; none for any other.
    #[must_use]
    pub fn from_actor_value(av: u8) -> Option<Self> {
        let index = av.checked_sub(FIRST_ACTOR_VALUE)?;
        Self::ALL.get(usize::from(index)).copied()
    }

    /// The skill's ActorValue, 6 to 23.
    #[must_use]
    pub fn actor_value(self) -> u8 {
        FIRST_ACTOR_VALUE.saturating_add(self.index())
    }

    /// The skill's place in [`Self::ALL`].
    #[must_use]
    pub fn index(self) -> u8 {
        match self {
            Self::OneHanded => 0,
            Self::TwoHanded => 1,
            Self::Archery => 2,
            Self::Block => 3,
            Self::Smithing => 4,
            Self::HeavyArmor => 5,
            Self::LightArmor => 6,
            Self::Pickpocket => 7,
            Self::Lockpicking => 8,
            Self::Sneak => 9,
            Self::Alchemy => 10,
            Self::Speech => 11,
            Self::Alteration => 12,
            Self::Conjuration => 13,
            Self::Destruction => 14,
            Self::Illusion => 15,
            Self::Restoration => 16,
            Self::Enchanting => 17,
        }
    }

    /// The skill's AVSK constants (UESP's table until libespm reads the AVIF
    /// records).
    #[must_use]
    pub fn constants(self) -> Constants {
        let c = |use_mult: f32, use_offset: f32, improve_mult: f32, improve_offset: f32| Constants {
            use_mult,
            use_offset,
            improve_mult,
            improve_offset,
        };
        match self {
            Self::OneHanded => c(6.3, 0.0, 2.0, 0.0),
            Self::TwoHanded => c(5.95, 0.0, 2.0, 0.0),
            Self::Archery => c(9.3, 0.0, 2.0, 0.0),
            Self::Block => c(8.1, 0.0, 2.0, 0.0),
            Self::Smithing => c(1.0, 0.0, 0.25, 300.0),
            Self::HeavyArmor => c(3.8, 0.0, 2.0, 0.0),
            Self::LightArmor => c(4.0, 0.0, 2.0, 0.0),
            Self::Pickpocket => c(8.1, 0.0, 0.25, 250.0),
            Self::Lockpicking => c(45.0, 10.0, 0.25, 300.0),
            Self::Sneak => c(11.25, 0.0, 0.5, 120.0),
            Self::Alchemy => c(0.75, 0.0, 1.6, 65.0),
            Self::Speech => c(0.36, 0.0, 2.0, 0.0),
            Self::Alteration => c(3.0, 0.0, 2.0, 0.0),
            Self::Conjuration => c(2.1, 0.0, 2.0, 0.0),
            Self::Destruction => c(1.35, 0.0, 2.0, 0.0),
            Self::Illusion => c(4.6, 0.0, 2.0, 0.0),
            Self::Restoration => c(2.0, 0.0, 2.0, 0.0),
            Self::Enchanting => c(900.0, 0.0, 1.0, 170.0),
        }
    }
}

/// The skill experience one use of `skill` earns from the action's `base`
/// amount.
#[must_use]
pub fn use_xp(skill: Skill, base: f32) -> f32 {
    let c = skill.constants();
    c.use_mult * base + c.use_offset
}

/// The experience the step from `level` to the next costs for `skill`.
#[must_use]
pub fn level_cost(skill: Skill, level: u8) -> f32 {
    let c = skill.constants();
    c.improve_mult * f32::from(level).powf(SKILL_USE_CURVE) + c.improve_offset
}

/// The character experience the step from `level` to the next needs.
#[must_use]
pub fn character_threshold(level: u16) -> f32 {
    XP_LEVEL_UP_BASE + XP_LEVEL_UP_MULT * f32::from(level)
}

/// One skill's standing: its level and the experience toward the next.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SkillProgress {
    /// The level, [`SKILL_FLOOR`] to [`SKILL_CAP`].
    pub level: u8,
    /// Experience toward the next level.
    pub xp: f32,
}

/// The character's standing.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Character {
    /// The level, 1 to [`LEVEL_CAP`].
    pub level: u16,
    /// Experience toward the next level.
    pub xp: f32,
}

/// A player's progress: the eighteen skills and the character.
#[derive(Debug, Clone, PartialEq)]
pub struct Progress {
    skills: [SkillProgress; 18],
    /// The character.
    pub character: Character,
}

/// What a use changed, in order.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Event {
    /// A skill reached a level.
    SkillLevel {
        /// The skill.
        skill: Skill,
        /// Its new level.
        level: u8,
    },
    /// The character reached a level.
    CharacterLevel {
        /// The new level.
        level: u16,
    },
}

/// Why a use earned nothing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Refusal {
    /// The ActorValue is no skill.
    Skill,
    /// The base amount is not a number.
    NonFinite,
    /// The base amount is zero or negative, or over [`MAX_BASE_PER_USE`].
    Range,
}

impl Progress {
    /// A player at `character_level` whose skills stand at `levels` (in
    /// ActorValue order) with no experience toward the next; levels below
    /// the floor read as the floor, above the cap as the cap.
    #[must_use]
    pub fn new(levels: [u8; 18], character_level: u16) -> Self {
        Self {
            skills: levels.map(|level| SkillProgress { level: level.clamp(SKILL_FLOOR, SKILL_CAP), xp: 0.0 }),
            character: Character { level: character_level.clamp(1, LEVEL_CAP), xp: 0.0 },
        }
    }

    /// A player as the record holds it.
    #[must_use]
    pub fn from_record(skills: [SkillProgress; 18], character: Character) -> Self {
        Self { skills, character }
    }

    /// `skill`'s standing.
    #[must_use]
    pub fn skill(&self, skill: Skill) -> SkillProgress {
        self.skills
            .get(usize::from(skill.index()))
            .copied()
            .unwrap_or(SkillProgress { level: SKILL_FLOOR, xp: 0.0 })
    }

    /// Every skill, in ActorValue order.
    #[must_use]
    pub fn skills(&self) -> &[SkillProgress; 18] {
        &self.skills
    }

    /// A use of the skill at ActorValue `skill_av` with the action's `base`
    /// amount: the skill's experience rises by [`use_xp`], each level step it
    /// affords is taken (the remainder carried), each skill level gained adds
    /// to the character's experience, and each character threshold met is a
    /// level. A skill at the cap earns nothing and changes nothing. Returns
    /// what changed, in order.
    pub fn apply_use(&mut self, skill_av: u8, base: f32) -> Result<Vec<Event>, Refusal> {
        let skill = Skill::from_actor_value(skill_av).ok_or(Refusal::Skill)?;
        if !base.is_finite() {
            return Err(Refusal::NonFinite);
        }
        if base <= 0.0 || base > MAX_BASE_PER_USE {
            return Err(Refusal::Range);
        }
        let mut events = Vec::new();
        let gained = use_xp(skill, base);
        let mut character_gain = 0.0f32;
        if let Some(s) = self.skills.get_mut(usize::from(skill.index())) {
            if s.level >= SKILL_CAP {
                return Ok(events);
            }
            s.xp += gained;
            while s.level < SKILL_CAP {
                let cost = level_cost(skill, s.level);
                if s.xp < cost {
                    break;
                }
                s.xp -= cost;
                s.level = s.level.saturating_add(1);
                character_gain += f32::from(s.level) * XP_PER_SKILL_RANK;
                events.push(Event::SkillLevel { skill, level: s.level });
            }
            if s.level >= SKILL_CAP {
                s.xp = 0.0;
            }
        }
        if character_gain > 0.0 {
            self.character.xp += character_gain;
            while self.character.level < LEVEL_CAP {
                let threshold = character_threshold(self.character.level);
                if self.character.xp < threshold {
                    break;
                }
                self.character.xp -= threshold;
                self.character.level = self.character.level.saturating_add(1);
                events.push(Event::CharacterLevel { level: self.character.level });
            }
        }
        Ok(events)
    }
}

#[cfg(test)]
#[allow(clippy::arithmetic_side_effects, clippy::float_cmp)]
mod tests {
    use super::*;

    fn near(a: f32, b: f32, tolerance: f32) -> bool {
        (a - b).abs() <= tolerance
    }

    fn total_15_to_100(skill: Skill) -> f32 {
        (SKILL_FLOOR..SKILL_CAP).map(|level| level_cost(skill, level)).sum()
    }

    #[test]
    fn the_skills_map_to_the_actor_values_6_to_23_and_back() {
        for (i, skill) in Skill::ALL.iter().enumerate() {
            let av = u8::try_from(i).unwrap_or_default() + 6;
            assert_eq!(skill.actor_value(), av);
            assert_eq!(Skill::from_actor_value(av), Some(*skill));
        }
        assert_eq!(Skill::from_actor_value(5), None);
        assert_eq!(Skill::from_actor_value(24), None);
        assert_eq!(Skill::from_actor_value(0), None);
        assert_eq!(Skill::ALL.len(), 18);
    }

    #[test]
    fn the_level_costs_sum_to_uesps_totals_from_15_to_100() {
        // the page's totals (Skyrim:Leveling), within a tenth of a point
        for (skill, want) in [
            (Skill::OneHanded, 528_804.02),
            (Skill::Speech, 528_804.02),
            (Skill::Destruction, 528_804.02),
            (Skill::Enchanting, 278_852.01),
            (Skill::Smithing, 91_600.5),
            (Skill::Lockpicking, 91_600.5),
            (Skill::Pickpocket, 87_350.5),
            (Skill::Sneak, 142_401.01),
            (Skill::Alchemy, 428_568.22),
        ] {
            let got = total_15_to_100(skill);
            assert!(near(got, want, 0.5), "{skill:?}: {got} against {want}");
        }
        // the first step and the last
        assert!(near(level_cost(Skill::OneHanded, 15), 2.0 * 15.0f32.powf(1.95), 0.01));
        assert!(near(level_cost(Skill::Smithing, 99), 0.25 * 99.0f32.powf(1.95) + 300.0, 0.01));
    }

    #[test]
    fn a_use_earns_mult_times_base_plus_offset() {
        assert!(near(use_xp(Skill::OneHanded, 10.0), 63.0, 0.001));
        assert!(near(use_xp(Skill::Lockpicking, 2.0), 100.0, 0.001));
        assert!(near(use_xp(Skill::Speech, 100.0), 36.0, 0.001));
        assert!(near(use_xp(Skill::Enchanting, 1.0), 900.0, 0.001));
    }

    #[test]
    fn the_character_threshold_is_75_plus_25_per_level() {
        assert_eq!(character_threshold(1), 100.0);
        assert_eq!(character_threshold(10), 325.0);
        assert_eq!(character_threshold(50), 1_325.0);
    }

    #[test]
    fn seven_hits_of_a_steel_sword_take_one_handed_from_15_to_16_with_the_remainder_carried() {
        let mut p = Progress::new([15; 18], 1);
        // 2 * 15^1.95 is about 392.6; a base of 10 earns 63 a hit
        for hit in 1..=6 {
            assert_eq!(p.apply_use(6, 10.0), Ok(vec![]), "hit {hit}");
        }
        assert_eq!(p.skill(Skill::OneHanded).level, 15);
        assert!(near(p.skill(Skill::OneHanded).xp, 378.0, 0.01));
        let events = p.apply_use(6, 10.0);
        assert_eq!(events, Ok(vec![Event::SkillLevel { skill: Skill::OneHanded, level: 16 }]));
        let s = p.skill(Skill::OneHanded);
        assert_eq!(s.level, 16);
        assert!(near(s.xp, 441.0 - level_cost(Skill::OneHanded, 15), 0.01));
        // the character took 16 toward its 100
        assert_eq!(p.character.level, 1);
        assert!(near(p.character.xp, 16.0, 0.001));
        // the other skills untouched
        assert_eq!(p.skill(Skill::TwoHanded), SkillProgress { level: 15, xp: 0.0 });
    }

    #[test]
    fn a_big_use_takes_several_levels_and_the_character_levels_when_its_threshold_is_met() {
        let mut p = Progress::new([15; 18], 1);
        // Enchanting earns 900 an item: the first steps cost 170 + 15^1.95
        // (about 367), 170 + 16^1.95 (about 393) and so on
        let events = p.apply_use(23, 1.0).unwrap_or_default();
        assert_eq!(events, vec![Event::SkillLevel { skill: Skill::Enchanting, level: 16 }, Event::SkillLevel { skill: Skill::Enchanting, level: 17 }]);
        assert_eq!(p.skill(Skill::Enchanting).level, 17);
        assert!(near(p.character.xp, 16.0 + 17.0, 0.001));
        // four more items: levels 18 to 21 or so, the character past 100
        let mut all = Vec::new();
        for _ in 0..4 {
            all.extend(p.apply_use(23, 1.0).unwrap_or_default());
        }
        assert!(all.contains(&Event::CharacterLevel { level: 2 }), "{all:?}");
        assert_eq!(p.character.level, 2);
        assert!(p.character.xp >= 0.0);
        assert!(p.character.xp < character_threshold(2));
    }

    #[test]
    fn a_skill_at_the_cap_earns_nothing_and_the_cap_holds() {
        let mut p = Progress::new([100; 18], 10);
        assert_eq!(p.apply_use(6, 50.0), Ok(vec![]));
        assert_eq!(p.skill(Skill::OneHanded), SkillProgress { level: 100, xp: 0.0 });
        assert_eq!(p.character, Character { level: 10, xp: 0.0 });
        // a skill at 99 with enough for two steps takes one and stops at the cap
        let mut q = Progress::from_record(
            Progress::new([99; 18], 1).skills,
            Character { level: 1, xp: 0.0 },
        );
        let events = q.apply_use(23, 100.0).unwrap_or_default();
        assert_eq!(events, vec![Event::SkillLevel { skill: Skill::Enchanting, level: 100 }, Event::CharacterLevel { level: 2 }]);
        assert_eq!(q.skill(Skill::Enchanting), SkillProgress { level: 100, xp: 0.0 });
    }

    #[test]
    fn refusals_are_named_and_change_nothing() {
        let mut p = Progress::new([15; 18], 1);
        let before = p.clone();
        assert_eq!(p.apply_use(5, 10.0), Err(Refusal::Skill));
        assert_eq!(p.apply_use(24, 10.0), Err(Refusal::Skill));
        assert_eq!(p.apply_use(6, f32::NAN), Err(Refusal::NonFinite));
        assert_eq!(p.apply_use(6, f32::INFINITY), Err(Refusal::NonFinite));
        assert_eq!(p.apply_use(6, 0.0), Err(Refusal::Range));
        assert_eq!(p.apply_use(6, -1.0), Err(Refusal::Range));
        assert_eq!(p.apply_use(6, MAX_BASE_PER_USE + 1.0), Err(Refusal::Range));
        assert_eq!(p, before);
        // the floor and the cap clamp a record's levels at construction
        let clamped = Progress::new([0, 200, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15], 0);
        assert_eq!(clamped.skill(Skill::OneHanded).level, SKILL_FLOOR);
        assert_eq!(clamped.skill(Skill::TwoHanded).level, SKILL_CAP);
        assert_eq!(clamped.character.level, 1);
    }
}
