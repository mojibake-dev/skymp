//! A raise's ash (thuum docs/verbs/npc-fate.md; ADR-060 as amended
//! 2026-11-15, Eli's executive decision: "thrall dies when player logs out.
//! most undead turn to ash, there's one that doesn't when it dies, i think
//! it's thrall specifically. keep that behavior so that the thrall can just
//! be brought back up. but everything else will either dispell or ash").
//!
//! A thrall whose commander's session ended dies by the server's word (the
//! cells rule's `ThrallExpired` at a departure; the core's load of a saved
//! thrall at a restart), and what its death leaves is its raise's own. In
//! the masters a raise turns its body to ash through one script on one of
//! its effects, ReanimateAshPile on MGEF 0xF52AB ReanimateSecondayFFAimed,
//! which Reanimate Corpse 0x65BD7, Revenant 0x96D94, Dread Zombie 0x96D95
//! and VampireRaiseThrall01 0xED0A4 carry and Dead Thrall 0x7E8DF does not
//! (Skyrim.esm, read with thuum lab/esm.py on sky-srv, 2026-11-15). So
//! whether a raise ashes is whether one of its effects runs that script
//! ([`is_ash_script`]), read from the records, never a list of ids; Dead
//! Thrall's body stays, to be raised again.
//!
//! The script's OnDying runs TurnToAsh on the engine that runs the effect.
//! Once the commander is gone no engine runs it: a watcher's engine refuses
//! every effect of the raise at Character::AddTarget (thuum lab run
//! 20261113-075817, c2: "reanimate 65bd7 (effect f52ab) on 35534 refused
//! here"). So the server makes TurnToAsh's own calls, at its own moments
//! ([`plan`]), to every game that sees the body. TurnToAsh, instructions 0
//! to 29 of scripts/reanimateashpile.pex (Skyrim - Misc.bsa, sha1
//! 2cc370351f21d5da04f1683620fd58b144244649, disassembled with thuum
//! lab/pex.py): SetCriticalStage(CritStage_DisintegrateStart); the shader's
//! Play(Victim, ShaderDuration) when it has one; SetAlpha(0.0, True) when
//! bSetAlphaToZeroEarly; Utility.Wait(fDelay); AttachAshPile(AshPileObject);
//! Utility.Wait(fDelayEnd); the shader's Stop(Victim) when it has one;
//! SetAlpha(0.0, True) when bSetAlphaZero;
//! SetCriticalStage(CritStage_DisintegrateEnd). The properties come from the
//! effect's VMAD; one the VMAD does not set keeps the script's own initial
//! value (the `DEFAULT_` constants, the same pex). The script skips
//! TurnToAsh for a body it holds immune (OnEffectStart: no commanded actor,
//! or IsSummoned, the actor carrying the VMAD's ActorTypeFamiliar or
//! ActorTypeDaedra) or when AshPileCreated is set; a thrall is commanded by
//! construction, so the core checks the two keywords on the actor
//! ([`Plan::familiar`], [`Plan::daedra`]). No function reads ImmunityList.
//!
//! R0: the server's own word about a body it killed, sent with no sequence
//! and kept nowhere (wire-rules natives: SetAlpha, SetCriticalStage and
//! AttachAshPile are one-shots), so a game that loads the body later sees a
//! plain corpse. HYPOTHESIS: what SetCriticalStage(DisintegrateEnd) and
//! AttachAshPile(None) do on a seat that does not simulate the body
//! (unread; the verb doc's Dynamic section).

use crate::natives::ASH_WINDOW_MS;

/// The script whose effect turns a raised body to ash at its death.
pub const ASH_SCRIPT: &str = "ReanimateAshPile";

/// fDelay's own value in the script, seconds: TurnToAsh's wait between the
/// shader's Play and AttachAshPile (the pex's initial value; Skyrim.esm's
/// VMAD on 0xF52AB sets 1.25).
pub const DEFAULT_DELAY_S: f32 = 0.75;

/// fDelayEnd's own value in the script, seconds: the wait between
/// AttachAshPile and the end (the pex's initial value; Skyrim.esm's VMAD
/// leaves it).
pub const DEFAULT_DELAY_END_S: f32 = 1.65;

/// ShaderDuration's own value in the script, seconds (the pex's initial
/// value; Skyrim.esm's VMAD sets 4.0).
pub const DEFAULT_SHADER_DURATION_S: f32 = 0.0;

/// bSetAlphaZero's own value in the script: the body fades out at the end
/// (the pex's initial value; Skyrim.esm's VMAD leaves it).
pub const DEFAULT_ALPHA_ZERO: bool = true;

/// bSetAlphaToZeroEarly's own value in the script: no fade at the start
/// (the pex's initial value; Skyrim.esm's VMAD leaves it).
pub const DEFAULT_ALPHA_ZERO_EARLY: bool = false;

/// The engine's critical stage TurnToAsh sets first (CommonLibSSE-NG
/// b93280e8 include/RE/A/Actor.h:53-62, ACTOR_CRITICAL_STAGE's
/// kDisintegrateStart). The pex reads it as Actor's
/// CritStage_DisintegrateStart; Actor.pex's own value is unread.
pub const DISINTEGRATE_START: u32 = 3;

/// The critical stage TurnToAsh sets last (the same enum's
/// kDisintegrateEnd; Actor's CritStage_DisintegrateEnd).
pub const DISINTEGRATE_END: u32 = 4;

/// A VMAD property's type code for an object (UESP "Skyrim Mod:Mod File
/// Format/VMAD Field"; libespm Property::Type).
pub const KIND_OBJECT: u32 = 1;

/// A VMAD property's type code for a float.
pub const KIND_FLOAT: u32 = 4;

/// A VMAD property's type code for a bool.
pub const KIND_BOOL: u32 = 5;

/// One property's value as a script's VMAD writes it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Value {
    /// An object: its form id, global; 0 for None.
    Object(u32),
    /// A float.
    Float(f32),
    /// A bool.
    Bool(bool),
    /// Any other type, which no property of the ash script takes.
    Other,
}

impl Value {
    /// The value of type `kind` (a VMAD type code) as the core read it: the
    /// object's global id, the float or the bool, the others unread.
    pub const fn of_kind(kind: u32, form: u32, number: f32, flag: bool) -> Value {
        match kind {
            KIND_OBJECT => Value::Object(form),
            KIND_FLOAT => Value::Float(number),
            KIND_BOOL => Value::Bool(flag),
            _ => Value::Other,
        }
    }
}

/// One property of the ash script's VMAD.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Property<'a> {
    /// Its name as the VMAD writes it (Papyrus names ignore case).
    pub name: &'a str,
    /// Its value.
    pub value: Value,
}

/// One of TurnToAsh's calls on the body.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Step {
    /// Actor.SetCriticalStage on the body, the stage [`Call::number`].
    CriticalStage,
    /// EffectShader.Play on [`Plan::shader`] with (the body,
    /// [`Call::number`] seconds).
    ShaderPlay,
    /// Actor.SetAlpha(0.0, True) on the body.
    AlphaZero,
    /// Actor.AttachAshPile([`Plan::pile`], None for 0) on the body.
    AttachAshPile,
    /// EffectShader.Stop on [`Plan::shader`] with (the body).
    ShaderStop,
}

/// One call of the ash and its moment.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Call {
    /// How long after the death, ms.
    pub at_ms: u64,
    /// The call.
    pub step: Step,
    /// Its number argument: the stage for [`Step::CriticalStage`], the
    /// shader's seconds for [`Step::ShaderPlay`], 0 for the others.
    pub number: f64,
}

/// A raise's ash as the server plays it on the body.
#[derive(Debug, Clone, PartialEq, Default)]
pub struct Plan {
    /// TurnToAsh's calls in its order, their moments never decreasing;
    /// empty when the VMAD sets AshPileCreated (the script skips TurnToAsh).
    pub calls: Vec<Call>,
    /// MagicEffectShader's form id, 0 for None.
    pub shader: u32,
    /// AshPileObject's form id, 0 for None (the engine's own pile).
    pub pile: u32,
    /// ActorTypeFamiliar's keyword id, 0 for None: a body carrying it is
    /// immune.
    pub familiar: u32,
    /// ActorTypeDaedra's keyword id, 0 for None: a body carrying it is
    /// immune.
    pub daedra: u32,
}

/// Whether a script attached to a raise's effect is the ash's (Papyrus
/// names ignore case).
pub fn is_ash_script(name: &str) -> bool {
    name.eq_ignore_ascii_case(ASH_SCRIPT)
}

/// The value the VMAD sets for `name`, none when it sets none.
fn find(properties: &[Property<'_>], name: &str) -> Option<Value> {
    properties.iter().find(|p| p.name.eq_ignore_ascii_case(name)).map(|p| p.value)
}

/// A float property, the script's own value when the VMAD sets none, sets
/// another type, or sets no number.
fn float_of(properties: &[Property<'_>], name: &str, default: f32) -> f32 {
    match find(properties, name) {
        Some(Value::Float(x)) if x.is_finite() => x,
        _ => default,
    }
}

/// A bool property, the script's own value when the VMAD sets none or
/// another type.
fn bool_of(properties: &[Property<'_>], name: &str, default: bool) -> bool {
    match find(properties, name) {
        Some(Value::Bool(b)) => b,
        _ => default,
    }
}

/// An object property, 0 (None, the script's own) when the VMAD sets none or
/// another type.
fn object_of(properties: &[Property<'_>], name: &str) -> u32 {
    match find(properties, name) {
        Some(Value::Object(id)) => id,
        _ => 0,
    }
}

/// A Utility.Wait's seconds as the server's timer counts them, ms: none for
/// a wait below zero or no number, at most [`ASH_WINDOW_MS`] (the window
/// the server already gives the caster's own ash after the death, a bound
/// of the server's policy; vanilla's whole ash takes 2.9 s).
fn wait_ms(seconds: f32) -> u64 {
    let ms = f64::from(seconds) * 1000.0;
    if !ms.is_finite() || ms <= 0.0 {
        return 0;
    }
    #[allow(clippy::as_conversions, clippy::cast_possible_truncation, clippy::cast_sign_loss)] // range checked above, the cast saturates
    let ms = ms.round() as u64;
    ms.min(ASH_WINDOW_MS)
}

/// TurnToAsh's calls for a raise whose effect's VMAD sets `properties` on
/// the ash script, timed from the death: at once the start's critical
/// stage, the shader's Play and the early fade; after fDelay the pile;
/// after fDelayEnd more the shader's Stop, the fade and the end's critical
/// stage, the last at most [`ASH_WINDOW_MS`] after the death.
pub fn plan(properties: &[Property<'_>]) -> Plan {
    let mut plan = Plan {
        calls: Vec::new(),
        shader: object_of(properties, "MagicEffectShader"),
        pile: object_of(properties, "AshPileObject"),
        familiar: object_of(properties, "ActorTypeFamiliar"),
        daedra: object_of(properties, "ActorTypeDaedra"),
    };
    if bool_of(properties, "AshPileCreated", false) {
        return plan;
    }
    let pile_ms = wait_ms(float_of(properties, "fDelay", DEFAULT_DELAY_S));
    let end_ms = pile_ms.saturating_add(wait_ms(float_of(properties, "fDelayEnd", DEFAULT_DELAY_END_S))).min(ASH_WINDOW_MS);
    let mut call = |at_ms: u64, step: Step, number: f64| plan.calls.push(Call { at_ms, step, number });
    call(0, Step::CriticalStage, f64::from(DISINTEGRATE_START));
    if plan.shader != 0 {
        call(0, Step::ShaderPlay, f64::from(float_of(properties, "ShaderDuration", DEFAULT_SHADER_DURATION_S)));
    }
    if bool_of(properties, "bSetAlphaToZeroEarly", DEFAULT_ALPHA_ZERO_EARLY) {
        call(0, Step::AlphaZero, 0.0);
    }
    call(pile_ms, Step::AttachAshPile, 0.0);
    if plan.shader != 0 {
        call(end_ms, Step::ShaderStop, 0.0);
    }
    if bool_of(properties, "bSetAlphaZero", DEFAULT_ALPHA_ZERO) {
        call(end_ms, Step::AlphaZero, 0.0);
    }
    call(end_ms, Step::CriticalStage, f64::from(DISINTEGRATE_END));
    plan
}

#[cfg(test)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    // Skyrim.esm's VMAD on MGEF 0xF52AB ReanimateSecondayFFAimed, as thuum
    // lab/esm.py's VMAD read on sky-srv printed it (2026-11-15): the one
    // script, ReanimateAshPile, and its six properties
    const SHADER: u32 = 0xd22fb; // EFSH ShockDisintegrate01FXS
    const DAEDRA: u32 = 0x13797; // KYWD ActorTypeDaedra
    const FAMILIAR: u32 = 0x10ead7; // KYWD ActorTypeFamiliar
    fn skyrim() -> Vec<Property<'static>> {
        vec![
            Property { name: "ImmunityList", value: Value::Object(0xf6534) },
            Property { name: "ShaderDuration", value: Value::Float(4.0) },
            Property { name: "ActorTypeDaedra", value: Value::Object(DAEDRA) },
            Property { name: "MagicEffectShader", value: Value::Object(SHADER) },
            Property { name: "fDelay", value: Value::Float(1.25) },
            Property { name: "ActorTypeFamiliar", value: Value::Object(FAMILIAR) },
        ]
    }

    const fn at(at_ms: u64, step: Step, number: f64) -> Call {
        Call { at_ms, step, number }
    }

    #[test]
    fn the_masters_ash_is_turn_to_ashs_calls_at_its_moments() {
        let plan = plan(&skyrim());
        assert_eq!(
            plan.calls,
            vec![
                at(0, Step::CriticalStage, 3.0),
                at(0, Step::ShaderPlay, 4.0),
                at(1_250, Step::AttachAshPile, 0.0),
                at(2_900, Step::ShaderStop, 0.0),
                at(2_900, Step::AlphaZero, 0.0),
                at(2_900, Step::CriticalStage, 4.0),
            ]
        );
        assert_eq!((plan.shader, plan.pile, plan.familiar, plan.daedra), (SHADER, 0, FAMILIAR, DAEDRA));
    }

    #[test]
    fn a_property_the_vmad_leaves_keeps_the_scripts_own_value() {
        let plan = plan(&[]);
        assert_eq!(
            plan.calls,
            vec![at(0, Step::CriticalStage, 3.0), at(750, Step::AttachAshPile, 0.0), at(2_400, Step::AlphaZero, 0.0), at(2_400, Step::CriticalStage, 4.0)]
        );
        assert_eq!(plan, Plan { calls: plan.calls.clone(), ..Plan::default() });
        // a property of another type is no value of its own either
        let wrong = [Property { name: "fDelay", value: Value::Bool(true) }, Property { name: "bSetAlphaZero", value: Value::Float(0.0) }];
        assert_eq!(super::plan(&wrong).calls, plan.calls);
    }

    #[test]
    fn the_script_and_its_properties_are_named_in_any_case() {
        assert!(is_ash_script("ReanimateAshPile"));
        assert!(is_ash_script("reanimateashpile"));
        assert!(!is_ash_script("SayOnHitByMagicEffectScript"));
        assert!(!is_ash_script("ODN_DispelSelfImmediately_Script"));
        let props = [Property { name: "FDELAY", value: Value::Float(2.0) }, Property { name: "magiceffectshader", value: Value::Object(SHADER) }];
        let plan = plan(&props);
        assert_eq!(plan.shader, SHADER);
        assert_eq!(plan.calls.iter().find(|c| c.step == Step::AttachAshPile).map(|c| c.at_ms), Some(2_000));
    }

    #[test]
    fn the_fades_follow_their_flags_and_a_set_pile_is_named() {
        let props = [
            Property { name: "bSetAlphaToZeroEarly", value: Value::Bool(true) },
            Property { name: "bSetAlphaZero", value: Value::Bool(false) },
            Property { name: "AshPileObject", value: Value::Object(0x1b) },
        ];
        let plan = plan(&props);
        assert_eq!(plan.pile, 0x1b);
        assert_eq!(
            plan.calls,
            vec![at(0, Step::CriticalStage, 3.0), at(0, Step::AlphaZero, 0.0), at(750, Step::AttachAshPile, 0.0), at(2_400, Step::CriticalStage, 4.0)]
        );
    }

    #[test]
    fn a_set_ash_pile_created_skips_turn_to_ash() {
        let mut props = skyrim();
        props.push(Property { name: "AshPileCreated", value: Value::Bool(true) });
        let plan = plan(&props);
        assert!(plan.calls.is_empty());
        assert_eq!((plan.familiar, plan.daedra), (FAMILIAR, DAEDRA));
    }

    #[test]
    fn a_wait_below_zero_or_no_number_is_none_and_a_long_one_is_bounded() {
        let none = [Property { name: "fDelay", value: Value::Float(-1.0) }, Property { name: "fDelayEnd", value: Value::Float(f32::NAN) }];
        // NaN is no number: fDelayEnd keeps the script's 1.65 s
        assert_eq!(plan(&none).calls.last().map(|c| c.at_ms), Some(1_650));
        let long = [Property { name: "fDelay", value: Value::Float(1.0e9) }];
        let plan = plan(&long);
        assert!(plan.calls.iter().all(|c| c.at_ms <= ASH_WINDOW_MS));
        assert_eq!(plan.calls.last().map(|c| c.at_ms), Some(ASH_WINDOW_MS));
    }

    #[test]
    fn the_type_codes_read_their_own_value() {
        assert_eq!(Value::of_kind(KIND_OBJECT, SHADER, 4.0, true), Value::Object(SHADER));
        assert_eq!(Value::of_kind(KIND_FLOAT, SHADER, 4.0, true), Value::Float(4.0));
        assert_eq!(Value::of_kind(KIND_BOOL, SHADER, 4.0, true), Value::Bool(true));
        assert_eq!(Value::of_kind(3, SHADER, 4.0, true), Value::Other);
    }

    proptest! {
        // whatever the VMAD sets, the calls keep TurnToAsh's order: their
        // moments never decrease, the window bounds them, the first is the
        // start's stage and the last the end's
        #[test]
        fn the_calls_keep_their_order_within_the_window(delay in proptest::num::f32::ANY, delay_end in proptest::num::f32::ANY, early in any::<bool>(), zero in any::<bool>(), shader in any::<u32>()) {
            let props = [
                Property { name: "fDelay", value: Value::Float(delay) },
                Property { name: "fDelayEnd", value: Value::Float(delay_end) },
                Property { name: "bSetAlphaToZeroEarly", value: Value::Bool(early) },
                Property { name: "bSetAlphaZero", value: Value::Bool(zero) },
                Property { name: "MagicEffectShader", value: Value::Object(shader) },
            ];
            let plan = plan(&props);
            prop_assert!(plan.calls.windows(2).all(|w| w.first().map(|c| c.at_ms) <= w.last().map(|c| c.at_ms)));
            prop_assert!(plan.calls.iter().all(|c| c.at_ms <= ASH_WINDOW_MS));
            prop_assert_eq!(plan.calls.first().copied(), Some(at(0, Step::CriticalStage, 3.0)));
            prop_assert_eq!(plan.calls.last().map(|c| (c.step, c.number)), Some((Step::CriticalStage, 4.0)));
        }
    }
}
