//! Magic effects (thuum docs/verbs/magic-effects.md, M2): what a potion's, a
//! poison's or a spell's effects do to an actor value, and when, as the
//! server's own state (R0, thuum ADR-028). The world model reads each
//! effect's facts from the records (MGEF: archetype, actor values, flags;
//! the item's magnitude and duration) and keeps an actor's running effects
//! in its change form, so they outlast a restart; the rules here are pure
//! functions over those facts and entries.
//!
//! The archetypes' timing is the Creation Kit wiki's ("Magic Effect"): with
//! Recover set, a value or peak value modifier changes its actor value once
//! at the start and changes it back when the effect ends, and for Health,
//! Magicka and Stamina that buff moves the maximum with the current value;
//! without Recover the value changes every second of the duration and is not
//! given back. Whether the engine steps once a second or flows the same
//! amount each frame is HYPOTHESIS until the lab watches a duration potion
//! (the doc's Dynamic plan); the totals agree either way, and these rules
//! flow it. Taper (fire's after-burn) is not counted until the lab measures
//! it.

/// The most effects running on one actor at once: a player's chug of
/// potions stays inside it.
pub const MAX_EFFECTS: usize = 32;

/// The longest duration an effect keeps, seconds (a day): a record's larger
/// number is cut to it, so a running effect always ends.
pub const MAX_DURATION_S: f32 = 86_400.0;

/// How an effect changes its actor values (the MGEF archetype).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EffectKind {
    /// Archetype 0, value modifier: restore or damage, or with Recover a
    /// buff.
    Value,
    /// Archetype 34, peak value modifier: the maximum (with Recover, a
    /// fortify).
    PeakValue,
    /// Archetype 5, dual value modifier: the primary value, and the second
    /// at its weight (shock's Magicka, frost's Stamina).
    DualValue,
    /// An archetype that changes no actor value and lasts its duration:
    /// Invisibility (11), Light (12), Etherealize (41). The server keeps it
    /// as the actor's running state, which its rules read and every game
    /// shows (thuum docs/verbs/self-cast.md).
    State,
}

/// How the effect arrives: a concentration spell's hits each bring the
/// seconds the stream held (spell-cast's claim), so its effects count at
/// once and never keep running; everything else arrives once.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Arrival {
    /// A drink, a poisoned hit, a fire-and-forget spell's hit.
    Once,
    /// A concentration spell's hit: `scale` is the seconds it counts.
    Stream,
}

/// One effect of an item or a spell, as the records give it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct EffectFacts {
    /// The MGEF.
    pub effect: u32,
    /// The potion, poison, spell or enchantment that carries it.
    pub source: u32,
    /// The archetype.
    pub kind: EffectKind,
    /// The primary actor value (its index in the engine's list).
    pub av: u32,
    /// The second actor value of a dual modifier, if any.
    pub second_av: Option<u32>,
    /// The second value's share of the magnitude (MGEF DATA 0x3C).
    pub second_weight: f32,
    /// The item's magnitude for this effect (EFIT).
    pub magnitude: f32,
    /// The item's duration for this effect, seconds (EFIT); 0 for none.
    pub duration_s: f32,
    /// MGEF flag Recover (0x2).
    pub recover: bool,
    /// MGEF flag Detrimental (0x4): the magnitude lowers the value.
    pub detrimental: bool,
    /// MGEF flag NoDuration (0x200).
    pub no_duration: bool,
}

/// A change to one actor value. `current` moves the value alone (a heal, a
/// wound); `modifier` moves the temporary modifier, so the maximum and the
/// current value move together (a buff, and its end).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct AvChange {
    /// The actor value.
    pub av: u32,
    /// The change to the current value alone.
    pub current: f32,
    /// The change to the temporary modifier.
    pub modifier: f32,
}

/// A running effect, as the change form keeps it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct EffectEntry {
    /// The facts it was applied with, its magnitude already scaled.
    pub facts: EffectFacts,
    /// Who applied it (0 for none): a poison's attacker, a spell's caster.
    pub caster: u32,
    /// Seconds it has run.
    pub elapsed_s: f32,
}

impl EffectEntry {
    /// Seconds left.
    pub fn remaining_s(&self) -> f32 {
        (self.facts.duration_s - self.elapsed_s).max(0.0)
    }
}

/// What one applied effect did at once, and what keeps running.
#[derive(Debug, Clone, PartialEq)]
pub struct Applied {
    /// The changes made now.
    pub changes: Vec<AvChange>,
    /// The entry that keeps running, if the effect has a duration.
    pub entry: Option<EffectEntry>,
}

fn sign(facts: &EffectFacts) -> f32 {
    if facts.detrimental {
        -1.0
    } else {
        1.0
    }
}

fn finite(facts: &EffectFacts) -> bool {
    facts.magnitude.is_finite() && facts.duration_s.is_finite() && facts.second_weight.is_finite()
}

/// The changes `amount` (signed) makes through `facts`' actor values: the
/// primary at once, a dual modifier's second at its weight; `as_modifier`
/// moves the temporary modifier, otherwise the current value.
fn changes_of(facts: &EffectFacts, amount: f32, as_modifier: bool) -> Vec<AvChange> {
    let make = |av: u32, amount: f32| {
        if as_modifier {
            AvChange { av, current: 0.0, modifier: amount }
        } else {
            AvChange { av, current: amount, modifier: 0.0 }
        }
    };
    let mut out = vec![make(facts.av, amount)];
    if let (EffectKind::DualValue, Some(second)) = (facts.kind, facts.second_av) {
        out.push(make(second, amount * facts.second_weight));
    }
    out
}

/// Whether the effect buffs (moves the temporary modifier for its duration
/// and gives it back) rather than heals or wounds: Recover on a value or
/// peak value modifier, and a peak value modifier always moves the maximum.
fn buffs(facts: &EffectFacts) -> bool {
    facts.kind != EffectKind::State && (facts.recover || facts.kind == EffectKind::PeakValue)
}

/// Applies one effect at `scale` times its magnitude, cast by `caster`: a
/// stream's hit counts at once by the seconds it held; an effect without a
/// duration counts at once; a buff moves its modifier now and keeps running
/// to give it back; anything else keeps running and counts each second. An
/// effect whose numbers are not finite, or whose scale is not, does
/// nothing.
pub fn apply(facts: EffectFacts, scale: f32, caster: u32, arrival: Arrival) -> Applied {
    if !finite(&facts) || !scale.is_finite() || scale < 0.0 {
        return Applied { changes: Vec::new(), entry: None };
    }
    let mut facts = facts;
    facts.magnitude = facts.magnitude.max(0.0) * scale;
    facts.duration_s = facts.duration_s.clamp(0.0, MAX_DURATION_S);
    let amount = facts.magnitude * sign(&facts);
    let instant = arrival == Arrival::Stream || facts.no_duration || facts.duration_s <= 0.0;
    if facts.kind == EffectKind::State {
        // a state changes nothing; it runs for its duration, or not at all
        let entry = (!instant).then_some(EffectEntry { facts, caster, elapsed_s: 0.0 });
        return Applied { changes: Vec::new(), entry };
    }
    if buffs(&facts) && !instant {
        return Applied {
            changes: changes_of(&facts, amount, true),
            entry: Some(EffectEntry { facts, caster, elapsed_s: 0.0 }),
        };
    }
    if instant {
        // a peak modifier with no duration has nothing to give back: its
        // change is the value's own
        return Applied { changes: changes_of(&facts, amount, false), entry: None };
    }
    Applied { changes: Vec::new(), entry: Some(EffectEntry { facts, caster, elapsed_s: 0.0 }) }
}

/// Runs `entries` for `dt_s` seconds: each flowing effect's share of its
/// magnitude for the time it ran within its duration, each buff's return
/// when it ends; ended entries are removed. A `dt_s` that is not a finite
/// positive number runs nothing.
pub fn advance(entries: &mut Vec<EffectEntry>, dt_s: f32) -> Vec<AvChange> {
    let mut out = Vec::new();
    if !dt_s.is_finite() || dt_s <= 0.0 {
        return out;
    }
    for entry in entries.iter_mut() {
        let before = entry.elapsed_s;
        let after = (before + dt_s).min(entry.facts.duration_s);
        entry.elapsed_s = before + dt_s;
        let facts = entry.facts;
        let amount = facts.magnitude * sign(&facts);
        if facts.kind == EffectKind::State {
            continue;
        }
        if buffs(&facts) {
            if entry.elapsed_s >= facts.duration_s {
                out.extend(changes_of(&facts, -amount, true));
            }
        } else if after > before {
            out.extend(changes_of(&facts, amount * (after - before), false));
        }
    }
    entries.retain(|e| e.elapsed_s < e.facts.duration_s);
    out
}

/// The temporary modifier the running buffs in `entries` put on `av`: what
/// the maximum (Health, Magicka, Stamina) and the regeneration rates count
/// while they run.
pub fn modifier(entries: &[EffectEntry], av: u32) -> f32 {
    entries
        .iter()
        .filter(|e| buffs(&e.facts))
        .map(|e| {
            let amount = e.facts.magnitude * sign(&e.facts);
            let primary = if e.facts.av == av { amount } else { 0.0 };
            let second = match (e.facts.kind, e.facts.second_av) {
                (EffectKind::DualValue, Some(second)) if second == av => amount * e.facts.second_weight,
                _ => 0.0,
            };
            primary + second
        })
        .sum()
}

/// The standing modifier a set of constant effects puts on `av`: a race's
/// abilities (RACE SPLO, each an Ability spell of constant cast type) as
/// the engine keeps them from the first frame, a Nord's 50 ResistFrost and
/// an Altmer's 50 Magicka above all. Every value-moving kind counts, with
/// its sign; nothing here has a duration, so nothing is given back
/// (thuum docs/verbs/spell-cast.md, Hailstone).
pub fn constant_modifier(facts: &[EffectFacts], av: u32) -> f32 {
    facts
        .iter()
        .filter(|f| f.kind != EffectKind::State)
        .map(|f| {
            let amount = f.magnitude * sign(f);
            let primary = if f.av == av { amount } else { 0.0 };
            let second = match (f.kind, f.second_av) {
                (EffectKind::DualValue, Some(second)) if second == av => amount * f.second_weight,
                _ => 0.0,
            };
            primary + second
        })
        .sum()
}

/// How much of a hostile effect's magnitude a target takes, by its
/// resistances: the element's resist value (the MGEF's ResistanceAV read on
/// the target) after its Magic Resistance, each a percentage off and
/// capped at `cap` (the player's 85, fPlayerMaxResistance's executable
/// default; an NPC's 100 is immunity), a negative value a weakness that
/// adds (UESP "Skyrim:Resist Frost", "Skyrim:Resist Magic": "applied after
/// damage reduction from Resist Magic"; thuum docs/verbs/magic-effects.md).
/// Non-finite inputs resist nothing.
pub fn resistance_factor(resist: f32, magic_resist: f32, cap: f32) -> f32 {
    if !resist.is_finite() || !magic_resist.is_finite() || !cap.is_finite() {
        return 1.0;
    }
    let share = |r: f32| 1.0 - r.min(cap) / 100.0;
    (share(magic_resist) * share(resist)).max(0.0)
}

/// Ends the running effects among `entries` whose MGEF is one of `effects`
/// before their time (an ethereal or invisible actor's own attack ends its
/// state, thuum docs/verbs/self-cast.md); a buff that ends gives its
/// modifier back: those changes are returned.
pub fn remove(entries: &mut Vec<EffectEntry>, effects: &[u32]) -> Vec<AvChange> {
    let mut out = Vec::new();
    for gone in entries.iter().filter(|e| effects.contains(&e.facts.effect)) {
        if buffs(&gone.facts) {
            let amount = gone.facts.magnitude * sign(&gone.facts);
            out.extend(changes_of(&gone.facts, -amount, true));
        }
    }
    entries.retain(|e| !effects.contains(&e.facts.effect));
    out
}

/// Adds `entry` to `entries`, keeping at most MAX_EFFECTS: past the bound the
/// oldest entry of the same effect and source goes, or else the oldest of
/// all. A buff that goes this way gives its modifier back: those changes are
/// returned.
pub fn admit(entries: &mut Vec<EffectEntry>, entry: EffectEntry) -> Vec<AvChange> {
    let mut out = Vec::new();
    if entries.len() >= MAX_EFFECTS {
        let same = entries
            .iter()
            .enumerate()
            .filter(|(_, e)| e.facts.effect == entry.facts.effect && e.facts.source == entry.facts.source)
            .max_by(|(_, a), (_, b)| a.elapsed_s.total_cmp(&b.elapsed_s))
            .map(|(i, _)| i);
        let oldest = entries
            .iter()
            .enumerate()
            .max_by(|(_, a), (_, b)| a.elapsed_s.total_cmp(&b.elapsed_s))
            .map(|(i, _)| i);
        if let Some(i) = same.or(oldest) {
            if i < entries.len() {
                let gone = entries.remove(i);
                if buffs(&gone.facts) {
                    let amount = gone.facts.magnitude * sign(&gone.facts);
                    out.extend(changes_of(&gone.facts, -amount, true));
                }
            }
        }
    }
    entries.push(entry);
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    // Skyrim.esm (lab/esm.py, 2026-10-10; docs/verbs/magic-effects.md):
    // AlchRestoreHealth 0x3EB15 (value, NoDuration), AlchFortifyHealth
    // 0x3EAF3 (peak value, Recover), AlchDamageHealthDuration 0x10AA4A
    // (value, detrimental), ShockDamageConcAimed 0x13CAB (dual: Health,
    // Magicka); Health is actor value 24, Magicka 25
    const HEALTH: u32 = 24;
    const MAGICKA: u32 = 25;

    fn facts(kind: EffectKind, magnitude: f32, duration_s: f32) -> EffectFacts {
        EffectFacts {
            effect: 0x3eb15,
            source: 0x3eadd,
            kind,
            av: HEALTH,
            second_av: None,
            second_weight: 0.0,
            magnitude,
            duration_s,
            recover: false,
            detrimental: false,
            no_duration: false,
        }
    }

    fn total(changes: &[AvChange], av: u32) -> (f32, f32) {
        changes.iter().filter(|c| c.av == av).fold((0.0, 0.0), |(c, m), x| (c + x.current, m + x.modifier))
    }

    fn near(a: f32, b: f32) -> bool {
        (a - b).abs() < 1e-3
    }

    #[test]
    fn a_potion_of_minor_healing_heals_25_at_once() {
        let mut f = facts(EffectKind::Value, 25.0, 0.0);
        f.no_duration = true;
        let applied = apply(f, 1.0, 0, Arrival::Once);
        assert_eq!(applied.entry, None);
        assert_eq!(total(&applied.changes, HEALTH), (25.0, 0.0));
    }

    #[test]
    fn a_lingering_poison_wounds_each_second_of_its_duration() {
        // DB03Poison 0x58CFB: AlchDamageHealthDuration 6 a second for 10 s
        let mut f = facts(EffectKind::Value, 6.0, 10.0);
        f.effect = 0x10aa4a;
        f.source = 0x58cfb;
        f.detrimental = true;
        let applied = apply(f, 1.0, 0xff00_0000, Arrival::Once);
        assert!(applied.changes.is_empty());
        let mut entries = applied.entry.into_iter().collect::<Vec<_>>();
        let mut hurt = 0.0;
        for _ in 0..40 {
            hurt += total(&advance(&mut entries, 0.25), HEALTH).0;
        }
        assert!(near(hurt, -60.0), "{hurt}");
        assert!(entries.is_empty());
        // the last step past the end counts nothing more
        assert!(advance(&mut entries, 5.0).is_empty());
    }

    #[test]
    fn a_fortify_raises_the_maximum_and_gives_it_back_at_its_end() {
        // FortifyHealth01 0x3EAF2: AlchFortifyHealth 20 for 60 s
        let mut f = facts(EffectKind::PeakValue, 20.0, 60.0);
        f.effect = 0x3eaf3;
        f.recover = true;
        let applied = apply(f, 1.0, 0, Arrival::Once);
        assert_eq!(total(&applied.changes, HEALTH), (0.0, 20.0));
        let mut entries = applied.entry.into_iter().collect::<Vec<_>>();
        assert!(advance(&mut entries, 59.0).is_empty());
        assert!(near(entries.first().map(EffectEntry::remaining_s).unwrap_or(-1.0), 1.0));
        assert_eq!(total(&advance(&mut entries, 2.0), HEALTH), (0.0, -20.0));
        assert!(entries.is_empty());
    }

    #[test]
    fn the_running_buffs_sum_into_the_modifier() {
        let mut fortify = facts(EffectKind::PeakValue, 20.0, 60.0);
        fortify.recover = true;
        let mut poison = facts(EffectKind::Value, 6.0, 10.0);
        poison.detrimental = true;
        let mut entries = Vec::new();
        for f in [fortify, fortify, poison] {
            if let Some(e) = apply(f, 1.0, 0, Arrival::Once).entry {
                admit(&mut entries, e);
            }
        }
        // two fortifies count, the flowing poison does not
        assert!(near(modifier(&entries, HEALTH), 40.0));
        assert!(near(modifier(&entries, MAGICKA), 0.0));
    }

    #[test]
    fn a_stream_counts_its_seconds_at_once_on_both_values() {
        // Sparks: ShockDamageConcAimed, Health and Magicka at half
        let mut f = facts(EffectKind::DualValue, 8.0, 1.0);
        f.effect = 0x13cab;
        f.detrimental = true;
        f.second_av = Some(MAGICKA);
        f.second_weight = 0.5;
        let applied = apply(f, 0.25, 0xff00_0000, Arrival::Stream);
        assert_eq!(applied.entry, None);
        assert!(near(total(&applied.changes, HEALTH).0, -2.0));
        assert!(near(total(&applied.changes, MAGICKA).0, -1.0));
    }

    #[test]
    fn numbers_that_are_not_finite_do_nothing() {
        let f = facts(EffectKind::Value, f32::NAN, 10.0);
        assert_eq!(apply(f, 1.0, 0, Arrival::Once), Applied { changes: Vec::new(), entry: None });
        let g = facts(EffectKind::Value, 5.0, 10.0);
        assert_eq!(apply(g, f32::INFINITY, 0, Arrival::Once).entry, None);
        let mut entries = apply(g, 1.0, 0, Arrival::Once).entry.into_iter().collect::<Vec<_>>();
        assert!(advance(&mut entries, f32::NAN).is_empty());
        assert!(advance(&mut entries, -1.0).is_empty());
        assert_eq!(entries.len(), 1);
    }

    #[test]
    fn a_duration_past_a_day_is_cut_to_a_day() {
        let f = facts(EffectKind::Value, 1.0, 1.0e9);
        let entry = apply(f, 1.0, 0, Arrival::Once).entry;
        assert_eq!(entry.map(|e| e.facts.duration_s), Some(MAX_DURATION_S));
    }

    #[test]
    fn past_the_bound_the_oldest_of_the_same_goes_and_a_buff_gives_back() {
        let mut entries = Vec::new();
        let mut buff = facts(EffectKind::PeakValue, 20.0, 60.0);
        buff.effect = 0x3eaf3;
        buff.recover = true;
        let first = apply(buff, 1.0, 0, Arrival::Once).entry;
        if let Some(e) = first {
            admit(&mut entries, e);
        }
        advance(&mut entries, 1.0);
        for i in 0..(MAX_EFFECTS - 1) {
            let mut other = facts(EffectKind::Value, 1.0, 600.0);
            other.effect = 0x1000 + u32::try_from(i).unwrap_or(0);
            if let Some(e) = apply(other, 1.0, 0, Arrival::Once).entry {
                admit(&mut entries, e);
            }
        }
        assert_eq!(entries.len(), MAX_EFFECTS);
        // a second fortify replaces the first, which gives its 20 back
        let back = match apply(buff, 1.0, 0, Arrival::Once).entry {
            Some(e) => admit(&mut entries, e),
            None => Vec::new(),
        };
        assert_eq!(total(&back, HEALTH), (0.0, -20.0));
        assert_eq!(entries.len(), MAX_EFFECTS);
    }

    // Skyrim.esm (docs/verbs/magic-effects.md's table): InvisibillityFFSelf
    // 0x1EA6A (archetype 11, actor value 54), Invisibility's 30 s
    fn invisibility() -> EffectFacts {
        EffectFacts {
            effect: 0x1ea6a,
            source: 0x27eb6,
            kind: EffectKind::State,
            av: 54,
            second_av: None,
            second_weight: 0.0,
            magnitude: 0.0,
            duration_s: 30.0,
            recover: true,
            detrimental: false,
            no_duration: false,
        }
    }

    #[test]
    fn a_state_runs_its_duration_and_changes_nothing() {
        let applied = apply(invisibility(), 1.0, 0x14, Arrival::Once);
        assert!(applied.changes.is_empty());
        let mut entries: Vec<EffectEntry> = applied.entry.into_iter().collect();
        assert_eq!(entries.len(), 1, "a state with a duration runs");
        assert_eq!(modifier(&entries, 54), 0.0);
        assert!(advance(&mut entries, 10.0).is_empty());
        assert_eq!(entries.len(), 1);
        assert!(advance(&mut entries, 25.0).is_empty());
        assert!(entries.is_empty());
        // without a duration it never runs
        let none = apply(EffectFacts { duration_s: 0.0, ..invisibility() }, 1.0, 0x14, Arrival::Once);
        assert!(none.changes.is_empty() && none.entry.is_none());
    }

    #[test]
    fn removing_ends_the_named_effects_and_a_buff_gives_back() {
        let fortify = facts(EffectKind::PeakValue, 100.0, 60.0);
        let mut entries = Vec::new();
        for f in [fortify, invisibility()] {
            if let Some(e) = apply(f, 1.0, 0x14, Arrival::Once).entry {
                admit(&mut entries, e);
            }
        }
        assert_eq!(entries.len(), 2);
        // the state goes and gives nothing back; the buff stays
        assert!(remove(&mut entries, &[0x1ea6a]).is_empty());
        assert_eq!(entries.len(), 1);
        assert_eq!(modifier(&entries, HEALTH), 100.0);
        // the buff goes and gives its 100 back
        let back = remove(&mut entries, &[fortify.effect]);
        assert_eq!(total(&back, HEALTH), (0.0, -100.0));
        assert!(entries.is_empty());
    }

    #[test]
    fn constant_modifier_sums_every_value_kind_with_its_sign_and_no_buff_filter() {
        let mut resist = EffectFacts {
            effect: 0x3EAF1,
            source: 0xAA020,
            kind: EffectKind::Value,
            av: 43,
            second_av: None,
            second_weight: 0.0,
            magnitude: 50.0,
            duration_s: 0.0,
            recover: false,
            detrimental: false,
            no_duration: true,
        };
        assert!((constant_modifier(&[resist], 43) - 50.0).abs() < 1e-6);
        assert!(constant_modifier(&[resist], 24).abs() < 1e-6);
        resist.detrimental = true;
        assert!((constant_modifier(&[resist], 43) + 50.0).abs() < 1e-6);
        let dual = EffectFacts {
            kind: EffectKind::DualValue,
            av: 24,
            second_av: Some(26),
            second_weight: 0.5,
            magnitude: 20.0,
            detrimental: false,
            ..resist
        };
        assert!((constant_modifier(&[dual], 26) - 10.0).abs() < 1e-6);
        let state = EffectFacts { kind: EffectKind::State, ..resist };
        assert!(constant_modifier(&[state], 43).abs() < 1e-6);
    }

    #[test]
    fn resistance_factor_takes_the_element_after_magic_resistance_under_the_cap() {
        assert!((resistance_factor(0.0, 0.0, 85.0) - 1.0).abs() < 1e-6);
        assert!((resistance_factor(50.0, 0.0, 85.0) - 0.5).abs() < 1e-6);
        assert!((resistance_factor(50.0, 50.0, 85.0) - 0.25).abs() < 1e-6);
        // UESP's example: 85 and 85 leave 2.25 of 100
        assert!((resistance_factor(85.0, 85.0, 85.0) - 0.0225).abs() < 1e-6);
        // the player's cap holds 100 at 85; an NPC's 100 is immunity
        assert!((resistance_factor(100.0, 0.0, 85.0) - 0.15).abs() < 1e-6);
        assert!(resistance_factor(100.0, 0.0, 100.0).abs() < 1e-6);
        // a weakness adds
        assert!((resistance_factor(-50.0, 0.0, 85.0) - 1.5).abs() < 1e-6);
        assert!((resistance_factor(f32::NAN, 0.0, 85.0) - 1.0).abs() < 1e-6);
    }
}
