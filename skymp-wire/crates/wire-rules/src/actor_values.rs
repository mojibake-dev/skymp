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
    fn a_held_value_missing_from_the_report_is_kept() {
        let (after, still) = merge(&snapshot(&[]), &snapshot(&[(6, 20.0)]), &[(9, 40.0)]);
        assert_eq!(after.bases, vec![(6, 20.0), (9, 40.0)]);
        assert_eq!(still, vec![(9, 40.0)]);
    }
}
