//! A player's actor values and progress (thuum docs/verbs/actor-values.md):
//! every base value its game set, its skills' progress and its level. Its
//! client reports the whole snapshot after a skill or level increase (R2:
//! recorded within bounds, not validated, until M5's progression verb). The
//! server also sets values of its own (R0: a Papyrus native, the gamemode,
//! a console command); such a value holds against a report that does not
//! carry it yet, since the client may report before it applied the server's,
//! and the hold ends with the first report that matches.

/// Actor values the engine has (CommonLibSSE-NG include/RE/A/ActorValues.h,
/// kAggression 0 to kReflectDamage 163).
pub const ACTOR_VALUES: usize = 164;
/// The skills among them: kOneHanded 6 to kEnchanting 23.
pub const SKILL_AVS: core::ops::RangeInclusive<u8> = 6..=23;
/// Skills in the player's progress (CommonLibSSE-NG
/// include/RE/P/PlayerCharacter.h, PlayerSkills::Skills::kTotal).
pub const SKILLS: usize = 18;
/// A base skill's ceiling in play ([UESP, Skyrim:Skills](https://en.uesp.net/wiki/Skyrim:Skills)).
pub const SKILL_MAX: f32 = 100.0;
/// The highest character level a report may carry.
pub const LEVEL_MAX: u16 = 1000;
/// The bound on any other base value's magnitude.
pub const BASE_MAX: f32 = 1.0e6;
/// How close a reported value must be to a held one to count as applied:
/// the engine keeps floats, and a round trip through it may round.
pub const APPLIED_WITHIN: f32 = 1.0e-3;

/// One skill's progress (PlayerSkills::Data::SkillData).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SkillProgress {
    /// 0 to 17.
    pub skill: u8,
    /// The skill's level as the progress counts it.
    pub level: f32,
    /// Experience toward the next level.
    pub xp: f32,
    /// Experience the next level needs.
    pub threshold: f32,
}

/// A snapshot of a player's actor values and progress.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Snapshot {
    /// (actor value, base value).
    pub bases: Vec<(u8, f32)>,
    /// Each skill's progress.
    pub skills: Vec<SkillProgress>,
    /// The character's experience toward the next level.
    pub xp: f32,
    /// Experience the next level needs.
    pub threshold: f32,
    /// The character level.
    pub level: u16,
    /// (skill, times made legendary).
    pub legendary: Vec<(u8, u16)>,
}

fn held_value(held: &[(u8, f32)], av: u8) -> Option<f32> {
    held.iter().find(|(h, _)| *h == av).map(|(_, v)| *v)
}

fn applied(reported: f32, held: f32) -> bool {
    (reported - held).abs() <= APPLIED_WITHIN
}

/// Whether a report is within bounds. Every actor value below
/// [`ACTOR_VALUES`] and listed once, every number finite; a skill's base
/// within 0 to [`SKILL_MAX`] unless it is a value the server holds (the
/// server may set past play's ceiling), any other base within
/// ±[`BASE_MAX`]; skills below [`SKILLS`] and listed once, each level within
/// 0 to [`SKILL_MAX`], experience 0 or more and a threshold above 0; the
/// character's experience 0 or more, its threshold above 0, its level 1 to
/// [`LEVEL_MAX`]; legendary counts below [`SKILLS`], listed once, at most
/// [`LEVEL_MAX`].
#[must_use]
pub fn report_ok(r: &Snapshot, held: &[(u8, f32)]) -> bool {
    let mut seen = [false; ACTOR_VALUES];
    for &(av, base) in &r.bases {
        let Some(slot) = seen.get_mut(usize::from(av)) else {
            return false;
        };
        if *slot || !base.is_finite() {
            return false;
        }
        *slot = true;
        let within = if SKILL_AVS.contains(&av) {
            (0.0..=SKILL_MAX).contains(&base) || held_value(held, av).is_some_and(|h| applied(base, h))
        } else {
            base.abs() <= BASE_MAX
        };
        if !within {
            return false;
        }
    }
    let mut skills = [false; SKILLS];
    for s in &r.skills {
        let Some(slot) = skills.get_mut(usize::from(s.skill)) else {
            return false;
        };
        let numbers = [s.level, s.xp, s.threshold];
        if *slot || numbers.iter().any(|n| !n.is_finite()) {
            return false;
        }
        *slot = true;
        if !(0.0..=SKILL_MAX).contains(&s.level) || s.xp < 0.0 || s.threshold <= 0.0 {
            return false;
        }
    }
    if !r.xp.is_finite() || !r.threshold.is_finite() || r.xp < 0.0 || r.threshold <= 0.0 {
        return false;
    }
    if !(1..=LEVEL_MAX).contains(&r.level) {
        return false;
    }
    let mut legendary = [false; SKILLS];
    for &(skill, count) in &r.legendary {
        let Some(slot) = legendary.get_mut(usize::from(skill)) else {
            return false;
        };
        if *slot || count > LEVEL_MAX {
            return false;
        }
        *slot = true;
    }
    true
}

/// The record after a kept report, and the holds still standing. The
/// report's bases replace the record's, except a value the server holds
/// that the report does not carry yet: the record keeps the held value. A
/// hold the report matches ends. Progress, experience, level and legendary
/// counts are the report's.
#[must_use]
pub fn merge(record: &Snapshot, report: &Snapshot, held: &[(u8, f32)]) -> (Snapshot, Vec<(u8, f32)>) {
    let mut out = report.clone();
    let mut still = Vec::new();
    for &(av, value) in held {
        let reported = report.bases.iter().find(|(a, _)| *a == av).map(|(_, v)| *v);
        if reported.is_some_and(|r| applied(r, value)) {
            continue; // applied: the hold ends
        }
        still.push((av, value));
        match out.bases.iter_mut().find(|(a, _)| *a == av) {
            Some(entry) => entry.1 = value,
            None => out.bases.push((av, value)),
        }
    }
    let _ = record; // the report is a whole snapshot: nothing of the record survives but the holds
    (out, still)
}

/// Whether a report shows the record a login sent as applied: the level at
/// least the record's, and every recorded base at least the record's value
/// (play only raises them). A report from before the client applied the
/// record carries the race's values a fresh session starts on, and is not
/// taken; the record holds until one is.
#[must_use]
pub fn login_applied(record: &Snapshot, report: &Snapshot) -> bool {
    report.level >= record.level
        && record.bases.iter().all(|&(av, base)| {
            report.bases.iter().any(|&(a, v)| a == av && v >= base - APPLIED_WITHIN)
        })
}

/// Whether the server may set an actor value's base to a value (a Papyrus
/// native or the gamemode, R0): an actor value below [`ACTOR_VALUES`] and a
/// finite value within ±[`BASE_MAX`], the bound any recorded base keeps. A
/// skill past [`SKILL_MAX`] is the server's to set, as the game's own
/// SetActorValue allows; the player's next report carries it under the hold
/// ([`report_ok`], [`merge`]).
#[must_use]
pub fn set_ok(av: u8, value: f32) -> bool {
    usize::from(av) < ACTOR_VALUES && value.is_finite() && value.abs() <= BASE_MAX
}

/// The actor values' Papyrus names by index, as the game resolves them: each
/// a Skyrim.esm AVIF editor ID less its "AV", which SKSE's
/// ActorValueInfo.GetActorValueInfoByName finds at the form the game lists
/// at that index (thuum run 20261007-022013-x-racemenu-probe2, steps 025 and
/// 026). None where the lab confirmed no name: 24 of the 164 (thuum
/// docs/verbs/actor-values.md).
pub const NAMES: [Option<&str>; ACTOR_VALUES] = [
    Some("Aggression"),               // 0
    Some("Confidence"),               // 1
    Some("Energy"),                   // 2
    Some("Morality"),                 // 3
    Some("Mood"),                     // 4
    Some("Assistance"),               // 5
    Some("OneHanded"),                // 6
    Some("TwoHanded"),                // 7
    Some("Marksman"),                 // 8
    Some("Block"),                    // 9
    Some("Smithing"),                 // 10
    Some("HeavyArmor"),               // 11
    Some("LightArmor"),               // 12
    Some("Pickpocket"),               // 13
    Some("Lockpicking"),              // 14
    Some("Sneak"),                    // 15
    Some("Alchemy"),                  // 16
    Some("Speechcraft"),              // 17
    Some("Alteration"),               // 18
    Some("Conjuration"),              // 19
    Some("Destruction"),              // 20
    None,                             // 21
    Some("Restoration"),              // 22
    Some("Enchanting"),               // 23
    Some("Health"),                   // 24
    Some("Magicka"),                  // 25
    Some("Stamina"),                  // 26
    Some("HealRate"),                 // 27
    Some("MagickaRate"),              // 28
    Some("StaminaRate"),              // 29
    Some("SpeedMult"),                // 30
    Some("InventoryWeight"),          // 31
    Some("CarryWeight"),              // 32
    Some("CritChance"),               // 33
    Some("MeleeDamage"),              // 34
    Some("UnarmedDamage"),            // 35
    Some("Mass"),                     // 36
    None,                             // 37
    Some("VoiceRate"),                // 38
    Some("DamageResist"),             // 39
    Some("PoisonResist"),             // 40
    Some("FireResist"),               // 41
    Some("ElectricResist"),           // 42
    Some("FrostResist"),              // 43
    Some("MagicResist"),              // 44
    None,                             // 45
    Some("PerceptionCondition"),      // 46
    Some("EnduranceCondition"),       // 47
    Some("LeftAttackCondition"),      // 48
    Some("RightAttackCondition"),     // 49
    Some("LeftMobilityCondition"),    // 50
    Some("RightMobilityCondition"),   // 51
    Some("BrainCondition"),           // 52
    Some("Paralysis"),                // 53
    Some("Invisibility"),             // 54
    Some("NightEye"),                 // 55
    Some("DetectLifeRange"),          // 56
    None,                             // 57
    None,                             // 58
    Some("IgnoreCrippledLimbs"),      // 59
    Some("Fame"),                     // 60
    Some("Infamy"),                   // 61
    Some("JumpingBonus"),             // 62
    None,                             // 63
    None,                             // 64
    Some("ArmorPerks"),               // 65
    Some("ShieldPerks"),              // 66
    None,                             // 67
    Some("Variable01"),               // 68
    Some("Variable02"),               // 69
    Some("Variable03"),               // 70
    None,                             // 71
    None,                             // 72
    Some("Variable06"),               // 73
    Some("Variable07"),               // 74
    Some("Variable08"),               // 75
    Some("Variable09"),               // 76
    Some("Variable10"),               // 77
    Some("BowSpeedBonus"),            // 78
    Some("FavorActive"),              // 79
    Some("FavorsPerDay"),             // 80
    Some("FavorsPerDayTimer"),        // 81
    None,                             // 82
    Some("AbsorbChance"),             // 83
    Some("Blindness"),                // 84
    None,                             // 85
    Some("ShoutRecoveryMult"),        // 86
    Some("BowStaggerBonus"),          // 87
    Some("Telekinesis"),              // 88
    Some("FavorPointsBonus"),         // 89
    Some("LastBribedIntimidated"),    // 90
    Some("LastFlattered"),            // 91
    None,                             // 92
    Some("BypassVendorStolenCheck"),  // 93
    Some("BypassVendorKeywordCheck"), // 94
    Some("WaitingForPlayer"),         // 95
    Some("OneHandedMod"),             // 96
    Some("TwoHandedMod"),             // 97
    Some("MarksmanMod"),              // 98
    Some("BlockMod"),                 // 99
    None,                             // 100
    Some("HeavyArmorMod"),            // 101
    Some("LightArmorMod"),            // 102
    Some("PickPocketMod"),            // 103
    Some("LockpickingMod"),           // 104
    Some("SneakMod"),                 // 105
    Some("AlchemyMod"),               // 106
    Some("SpeechcraftMod"),           // 107
    Some("AlterationMod"),            // 108
    Some("ConjurationMod"),           // 109
    Some("DestructionMod"),           // 110
    Some("IllusionMod"),              // 111
    Some("RestorationMod"),           // 112
    Some("EnchantingMod"),            // 113
    Some("OneHandedSkillAdvance"),    // 114
    Some("TwoHandedSkillAdvance"),    // 115
    Some("MarksmanSkillAdvance"),     // 116
    Some("BlockSkillAdvance"),        // 117
    Some("SmithingSkillAdvance"),     // 118
    Some("HeavyArmorSkillAdvance"),   // 119
    Some("LightArmorSkillAdvance"),   // 120
    Some("PickPocketSkillAdvance"),   // 121
    Some("LockpickingSkillAdvance"),  // 122
    Some("SneakSkillAdvance"),        // 123
    Some("AlchemySkillAdvance"),      // 124
    Some("SpeechcraftSkillAdvance"),  // 125
    Some("AlterationSkillAdvance"),   // 126
    Some("ConjurationSkillAdvance"),  // 127
    Some("DestructionSkillAdvance"),  // 128
    Some("IllusionSkillAdvance"),     // 129
    Some("RestorationSkillAdvance"),  // 130
    Some("EnchantingSkillAdvance"),   // 131
    Some("LeftWeaponSpeedMult"),      // 132
    Some("DragonSouls"),              // 133
    Some("CombatHealthRegenMult"),    // 134
    Some("OneHandedPowerMod"),        // 135
    None,                             // 136
    Some("MarksmanPowerMod"),         // 137
    Some("BlockPowerMod"),            // 138
    Some("SmithingPowerMod"),         // 139
    Some("HeavyArmorPowerMod"),       // 140
    Some("LightArmorPowerMod"),       // 141
    Some("PickPocketPowerMod"),       // 142
    Some("LockpickingPowerMod"),      // 143
    Some("SneakPowerMod"),            // 144
    Some("AlchemyPowerMod"),          // 145
    Some("SpeechcraftPowerMod"),      // 146
    Some("AlterationPowerMod"),       // 147
    Some("ConjurationPowerMod"),      // 148
    None,                             // 149
    Some("IllusionPowerMod"),         // 150
    Some("RestorationPowerMod"),      // 151
    Some("EnchantingPowerMod"),       // 152
    Some("DragonRend"),               // 153
    Some("AttackDamageMult"),         // 154
    None,                             // 155
    None,                             // 156
    None,                             // 157
    None,                             // 158
    None,                             // 159
    None,                             // 160
    None,                             // 161
    None,                             // 162
    Some("ReflectDamage"),            // 163
];

/// The actor value a Papyrus name names, ASCII case ignored as the server
/// compares names (papyrus-vm CIString, the core's ConvertToAV); None for a
/// name the table does not hold.
#[must_use]
pub fn index_of(name: &str) -> Option<u8> {
    NAMES
        .iter()
        .position(|n| n.is_some_and(|n| n.eq_ignore_ascii_case(name)))
        .and_then(|i| u8::try_from(i).ok())
}

#[cfg(test)]
mod tests {
    use super::*;

    fn snapshot(bases: &[(u8, f32)]) -> Snapshot {
        Snapshot {
            bases: bases.to_vec(),
            skills: vec![SkillProgress { skill: 0, level: 15.0, xp: 2.0, threshold: 10.0 }],
            xp: 0.0,
            threshold: 75.0,
            level: 1,
            legendary: vec![],
        }
    }

    #[test]
    fn a_report_in_bounds_is_kept() {
        assert!(report_ok(&snapshot(&[(6, 15.0), (24, 100.0), (32, 300.0)]), &[]));
    }

    #[test]
    fn out_of_bounds_reports_are_refused() {
        let bad: &[Snapshot] = &[
            snapshot(&[(164, 1.0)]),            // no such actor value
            snapshot(&[(6, 15.0), (6, 16.0)]),  // listed twice
            snapshot(&[(6, 101.0)]),            // a skill past play's ceiling
            snapshot(&[(6, -1.0)]),             // a negative skill
            snapshot(&[(24, f32::NAN)]),        // not finite
            snapshot(&[(24, 2.0e6)]),           // past any base
        ];
        for r in bad {
            assert!(!report_ok(r, &[]), "{r:?}");
        }
        let mut level0 = snapshot(&[]);
        level0.level = 0;
        assert!(!report_ok(&level0, &[]));
        let mut twice = snapshot(&[]);
        let first = twice.skills.first().copied();
        twice.skills.extend(first);
        assert!(!report_ok(&twice, &[]));
        let mut no_threshold = snapshot(&[]);
        no_threshold.threshold = 0.0;
        assert!(!report_ok(&no_threshold, &[]));
        let mut legendary = snapshot(&[]);
        legendary.legendary = vec![(18, 1)];
        assert!(!report_ok(&legendary, &[]));
    }

    #[test]
    fn a_skill_the_server_set_past_the_ceiling_is_kept_once_applied() {
        assert!(!report_ok(&snapshot(&[(7, 150.0)]), &[]));
        assert!(report_ok(&snapshot(&[(7, 150.0)]), &[(7, 150.0)]));
    }

    #[test]
    fn a_held_value_survives_a_stale_report_and_ends_once_applied() {
        let record = snapshot(&[(7, 50.0)]);
        let held = [(7u8, 50.0f32)];
        // the client reports before it applied the server's 50
        let (after, still) = merge(&record, &snapshot(&[(6, 20.0), (7, 15.0)]), &held);
        assert_eq!(after.bases, vec![(6, 20.0), (7, 50.0)]);
        assert_eq!(still, held.to_vec());
        // then it reports the value applied
        let (after, still) = merge(&after, &snapshot(&[(6, 20.0), (7, 50.0)]), &still);
        assert_eq!(after.bases, vec![(6, 20.0), (7, 50.0)]);
        assert!(still.is_empty());
    }

    #[test]
    fn a_login_record_holds_until_a_report_reaches_it() {
        let mut record = snapshot(&[(6, 40.0), (24, 150.0)]);
        record.level = 12;
        // a fresh session's values, reported before the record was applied
        let fresh = snapshot(&[(6, 15.0), (24, 100.0)]);
        assert!(!login_applied(&record, &fresh));
        // applied, and One-Handed raised since by play
        let mut after = snapshot(&[(6, 41.0), (24, 150.0)]);
        after.level = 12;
        assert!(login_applied(&record, &after));
        // a recorded value the report does not carry
        let mut missing = snapshot(&[(6, 41.0)]);
        missing.level = 12;
        assert!(!login_applied(&record, &missing));
    }

    #[test]
    fn a_held_value_missing_from_the_report_is_kept() {
        let (after, still) = merge(&snapshot(&[]), &snapshot(&[(6, 20.0)]), &[(9, 40.0)]);
        assert_eq!(after.bases, vec![(6, 20.0), (9, 40.0)]);
        assert_eq!(still, vec![(9, 40.0)]);
    }

    #[test]
    fn names_resolve_as_the_game_lists_them() {
        assert_eq!(index_of("Health"), Some(24));
        assert_eq!(index_of("health"), Some(24));
        assert_eq!(index_of("OneHanded"), Some(6));
        assert_eq!(index_of("Enchanting"), Some(23));
        assert_eq!(index_of("CarryWeight"), Some(32));
        assert_eq!(index_of("Mysticism"), None); // its editor ID's name finds nothing
        assert_eq!(index_of(""), None);
        assert_eq!(index_of("NoSuchValue"), None);
        assert_eq!(NAMES.iter().filter(|n| n.is_some()).count(), 140);
    }

    #[test]
    fn the_server_sets_any_finite_base_within_the_record_bound() {
        assert!(set_ok(6, 150.0)); // a skill past play's ceiling: the server's to set
        assert!(set_ok(24, 250.0));
        assert!(set_ok(163, -5.0));
        assert!(set_ok(0, BASE_MAX));
        assert!(!set_ok(164, 1.0));
        assert!(!set_ok(6, f32::NAN));
        assert!(!set_ok(6, f32::INFINITY));
        assert!(!set_ok(32, BASE_MAX * 2.0));
    }

    #[test]
    fn a_value_the_server_set_past_the_ceiling_passes_the_next_report() {
        let held = [(6u8, 150.0f32)];
        assert!(set_ok(6, 150.0));
        assert!(report_ok(&snapshot(&[(6, 150.0)]), &held));
        assert!(!report_ok(&snapshot(&[(6, 150.0)]), &[]));
    }

    #[test]
    fn every_name_finds_its_own_index() {
        for (i, n) in NAMES.iter().enumerate() {
            if let Some(n) = n {
                assert_eq!(index_of(n).map(usize::from), Some(i), "{n}");
            }
        }
    }
}
