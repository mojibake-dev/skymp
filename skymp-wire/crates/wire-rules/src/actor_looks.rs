//! An actor's kept looks (the Undead FX build, the reanimate-fx memo's 6.4
//! D6 B; the review of the client's 3D keep, eaa6ba47, findings 1 and 2).
//! The script an actor's simulator runs draws on the actor through five
//! look rows (wire-rules natives, [`Keep::Look`]): EffectShader.Play and
//! Stop, VisualEffect.Play and Stop, and Actor.SetEyeTexture. Each call is
//! relayed to the actor's listeners as it runs. A seat whose subscription to
//! the actor begins later would have none of them: the server subscribes a
//! listener to the emitters within one grid square of 4096 units around it
//! (Grid.h's MoveImpl, MpObjectReference.cpp's GetGridPos) and sends
//! DestroyActor past it (PartOne's onUnsubscribe), while the copy's 3D
//! unloads only further out (uGridsToLoad 5, a HYPOTHESIS for the lab
//! seats), so a seat that walks off and back sees the copy's 3D load before
//! the next create word, and the relays it missed are gone with its view.
//! It would wait for the simulator's next call (up to 2.5 s for Undead FX's
//! flat shader), and a single SetEyeTexture's texture would never come.
//!
//! So the server keeps, per actor, the last Play per shader and per visual
//! effect and the last eye texture ([`Looks::keep`]), each cleared by its
//! Stop or by SetEyeTexture(None), and serves them with the actor's create
//! word ([`Looks::served`]; CreateActorProps' `looks`), a timed Play for the
//! seconds it has left. R2: recorded as the simulator reported them, the
//! forms and the seconds never judged. Runtime only, as the actor's
//! lastAnimation is (MpChangeForms.cpp writes none): a restart keeps none,
//! and the simulator's next call keeps one again.
//!
//! [`Keep::Look`]: crate::natives::Keep::Look

use std::collections::HashMap;

use crate::natives::LookKeep;

/// Looks kept per actor: a decay's flat shader and its transition shader,
/// v4's fade-in, the eyes' effect and texture fit (the client's LOOKS_MAX,
/// objectAnimationService.ts; the wire's cap::LOOKS). A new one past them
/// takes the place of the one kept longest ago.
pub const LOOKS_KEPT: usize = 8;

/// Actors whose looks are kept (consistency bounds, ADR-073 decision 5
/// (b)): a new actor past them takes the place of the actor whose looks
/// moved longest ago. UNMEASURED: a cell's reanimated thralls are a handful.
pub const ACTORS_KEPT: usize = 1024;

/// One look an actor keeps.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct KeptLook {
    /// What it is: [`LookKeep::ShaderPlay`], [`LookKeep::EffectPlay`] or
    /// [`LookKeep::Eyes`] (a Stop keeps nothing).
    pub keep: LookKeep,
    /// The shader's, the visual effect's or the texture set's form id.
    pub form: u32,
    /// A Play's seconds as called, below 0 until its Stop; -1 for the eyes.
    pub seconds: f32,
    /// VisualEffect.Play's facing object, 0 for None and for every other
    /// kind.
    pub facing: u32,
    /// The relay's sequence (the effect form's for a shader or a visual
    /// effect, the actor's for the eyes).
    pub seq: u32,
    /// When the server kept it, on the core's monotonic clock in ms.
    pub at_ms: u64,
}

/// One look call as the core read it off its message, in the row's own
/// order (FunctionsDump's parameters: EffectShader Play(ObjectReference
/// akObject, Float afDuration) and Stop(akObject), VisualEffect
/// Play(ObjectReference akObject, Float afTime, ObjectReference
/// akFacingObject) and Stop(akObject), Actor SetEyeTexture(TextureSet
/// akNewTexture)).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct LookCall {
    /// The row's keep.
    pub keep: LookKeep,
    /// The relay's self: the shader or the visual effect (the row's
    /// Subject::Arg0Method), the actor for SetEyeTexture.
    pub relay_self: u32,
    /// The call's first argument's form id when it is an object, 0 for None
    /// or anything else (SetEyeTexture's texture set).
    pub object0: u64,
    /// The call's second argument when it is a number (a Play's seconds),
    /// none for anything else.
    pub number1: Option<f64>,
    /// The call's third argument's form id when it is an object, 0 for None
    /// or anything else (VisualEffect.Play's facing object).
    pub object2: u64,
    /// The relay's sequence.
    pub seq: u32,
}

impl LookCall {
    /// The form the call names: the relay's self for a shader's or a visual
    /// effect's row, the texture set for the eyes (0 for None, or for an id
    /// past 32 bits, which names no form of the load order).
    #[must_use]
    pub fn form(&self) -> u32 {
        match self.keep {
            LookKeep::Eyes => u32::try_from(self.object0).unwrap_or(0),
            _ => self.relay_self,
        }
    }

    /// A Play's seconds as called, -1 for any below 0 (until its Stop) and
    /// for a call with no finite number there (the eyes, a Stop, which the
    /// shape step's parameter types never let a Play be); past f32's
    /// largest, f32's largest, so a served look is always finite.
    #[must_use]
    pub fn seconds(&self) -> f32 {
        match (self.keep, self.number1) {
            (LookKeep::ShaderPlay | LookKeep::EffectPlay, Some(s)) if s.is_finite() => {
                if s < 0.0 {
                    -1.0
                } else {
                    narrow(s.min(f64::from(f32::MAX)))
                }
            }
            _ => -1.0,
        }
    }

    /// VisualEffect.Play's facing object, 0 for every other kind.
    #[must_use]
    pub fn facing(&self) -> u32 {
        match self.keep {
            LookKeep::EffectPlay => u32::try_from(self.object2).unwrap_or(0),
            _ => 0,
        }
    }
}

/// What a call did to its actor's kept looks, for the core's line.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kept {
    /// A Play or an eye texture kept, none of its own kind and form before
    /// it.
    Kept,
    /// A Play or an eye texture kept in place of the one before it.
    Replaced,
    /// A Stop or SetEyeTexture(None) cleared the kept one it names.
    Cleared,
    /// A Stop or SetEyeTexture(None) that named nothing kept.
    Nothing,
    /// A timed Play whose seconds are none (0): nothing kept, and the one
    /// it would replace cleared.
    Spent,
}

/// One look served with an actor's create word.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ServedLook {
    /// What it is ([`LookKeep::ShaderPlay`], [`LookKeep::EffectPlay`] or
    /// [`LookKeep::Eyes`]).
    pub keep: LookKeep,
    /// Its form id.
    pub form: u32,
    /// The seconds it has left, above 0; -1 until its Stop (and the eyes).
    pub seconds: f32,
    /// VisualEffect.Play's facing object, 0 for None and every other kind.
    pub facing: u32,
    /// The relay's sequence it was kept at.
    pub seq: u32,
}

/// The actors' kept looks, by the actor's form id.
#[derive(Debug, Default)]
pub struct Looks {
    by_actor: HashMap<u32, Held>,
    /// A counter of the moves, the order [`ACTORS_KEPT`] goes by.
    moves: u64,
}

#[derive(Debug, Default)]
struct Held {
    looks: Vec<KeptLook>,
    moved: u64,
}

/// A Play's seconds as the record's f32 (the one narrowing the rule makes);
/// the caller has checked it is finite.
#[allow(clippy::as_conversions)]
fn narrow(x: f64) -> f32 {
    x as f32
}

/// The seconds a kept look has left at `now_ms`: -1 until its Stop, 0 once
/// they ran out.
fn left(look: &KeptLook, now_ms: u64) -> f32 {
    if look.seconds < 0.0 {
        return -1.0;
    }
    let elapsed_ms = u32::try_from(now_ms.saturating_sub(look.at_ms)).unwrap_or(u32::MAX);
    let remaining = f64::from(look.seconds) - f64::from(elapsed_ms) / 1000.0;
    if remaining > 0.0 {
        narrow(remaining)
    } else {
        0.0
    }
}

/// Whether a kept look is the one a call of `keep` on `form` names: a
/// shader's or a visual effect's per form, the eyes one per actor.
fn names(look: &KeptLook, keep: LookKeep, form: u32) -> bool {
    look.keep == keep.kept_as() && (keep == LookKeep::Eyes || look.form == form)
}

impl Looks {
    /// One reported look call on `actor` (the row's subject, the actor the
    /// reporter simulates), at `now_ms` on the core's monotonic clock: a
    /// Stop or SetEyeTexture(None) clears the kept one it names; a Play or
    /// an eye texture takes its place, last, the one kept longest ago going
    /// past [`LOOKS_KEPT`]. Every kept look whose seconds ran out goes
    /// first.
    pub fn keep(&mut self, actor: u32, call: &LookCall, now_ms: u64) -> Kept {
        let form = call.form();
        let clears = call.keep.is_stop() || form == 0;
        let seconds = call.seconds();
        self.moves = self.moves.saturating_add(1);
        let moves = self.moves;
        if !self.by_actor.contains_key(&actor) {
            if clears {
                return Kept::Nothing;
            }
            self.make_room();
        }
        let held = self.by_actor.entry(actor).or_default();
        held.moved = moves;
        held.looks.retain(|l| left(l, now_ms) != 0.0);
        let before = held.looks.len();
        held.looks.retain(|l| !names(l, call.keep, form));
        let had = held.looks.len() != before;
        let out = if clears {
            if had {
                Kept::Cleared
            } else {
                Kept::Nothing
            }
        } else if seconds == 0.0 {
            Kept::Spent
        } else {
            if held.looks.len() >= LOOKS_KEPT {
                held.looks.remove(0);
            }
            held.looks.push(KeptLook { keep: call.keep, form, seconds, facing: call.facing(), seq: call.seq, at_ms: now_ms });
            if had {
                Kept::Replaced
            } else {
                Kept::Kept
            }
        };
        if held.looks.is_empty() {
            self.by_actor.remove(&actor);
        }
        out
    }

    /// The looks `actor` keeps at `now_ms`, in the order they were kept,
    /// for its create word: a timed Play with the seconds it has left, one
    /// whose seconds ran out gone.
    pub fn served(&mut self, actor: u32, now_ms: u64) -> Vec<ServedLook> {
        let Some(held) = self.by_actor.get_mut(&actor) else {
            return Vec::new();
        };
        held.looks.retain(|l| left(l, now_ms) != 0.0);
        let out = held.looks.iter().map(|l| ServedLook { keep: l.keep, form: l.form, seconds: left(l, now_ms), facing: l.facing, seq: l.seq }).collect();
        if held.looks.is_empty() {
            self.by_actor.remove(&actor);
        }
        out
    }

    /// How many actors keep looks.
    #[must_use]
    pub fn actors(&self) -> usize {
        self.by_actor.len()
    }

    /// Room for one more actor: past [`ACTORS_KEPT`] the actor whose looks
    /// moved longest ago loses them.
    fn make_room(&mut self) {
        if self.by_actor.len() < ACTORS_KEPT {
            return;
        }
        let oldest = self.by_actor.iter().min_by_key(|(_, h)| h.moved).map(|(id, _)| *id);
        if let Some(id) = oldest {
            self.by_actor.remove(&id);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // the reanimate-fx memo's forms (natives.rs's tests): vanilla's
    // ShockDisintegrate01FXS 0xd22fb, Undead FX's US_Disintegrate_full_flat
    // 0x2b050cba, DA10ReanimateTargetFX 0x107d9b; Riverwood's Hilde 0x35534,
    // the memo's thrall, and the temple's draugr 0x380b4
    const DISINTEGRATE: u32 = 0xd22fb;
    const DECAY_FULL: u32 = 0x2b05_0cba;
    const REANIMATE_FX: u32 = 0x10_7d9b;
    const EYES: u64 = 0x2b01_264b;
    const THRALL: u32 = 0x35534;
    const DRAUGR: u32 = 0x380b4;

    fn play(form: u32, seconds: f64, seq: u32) -> LookCall {
        LookCall { keep: LookKeep::ShaderPlay, relay_self: form, object0: u64::from(THRALL), number1: Some(seconds), object2: 0, seq }
    }

    fn stop(form: u32, seq: u32) -> LookCall {
        LookCall { keep: LookKeep::ShaderStop, relay_self: form, object0: u64::from(THRALL), number1: None, object2: 0, seq }
    }

    fn eyes(texture: u64, seq: u32) -> LookCall {
        LookCall { keep: LookKeep::Eyes, relay_self: THRALL, object0: texture, number1: None, object2: 0, seq }
    }

    fn forms(served: &[ServedLook]) -> Vec<(LookKeep, u32)> {
        served.iter().map(|s| (s.keep, s.form)).collect()
    }

    #[test]
    fn a_play_is_kept_per_form_until_its_stop_and_served_with_its_seconds_left() {
        let mut looks = Looks::default();
        // the stage's flat shader with no duration, then a transition for
        // 2.5 s beside it (the memo's 6.2)
        assert_eq!(looks.keep(THRALL, &play(DECAY_FULL, -1.0, 1), 0), Kept::Kept);
        assert_eq!(looks.keep(THRALL, &play(DISINTEGRATE, 2.5, 1), 0), Kept::Kept);
        let served = looks.served(THRALL, 1_000);
        assert_eq!(forms(&served), [(LookKeep::ShaderPlay, DECAY_FULL), (LookKeep::ShaderPlay, DISINTEGRATE)]);
        assert_eq!(served.first().map(|s| (s.seconds, s.seq)), Some((-1.0, 1)));
        assert_eq!(served.get(1).map(|s| s.seconds), Some(1.5));
        // the transition's seconds run out: it goes, the flat one stays
        assert_eq!(forms(&looks.served(THRALL, 2_500)), [(LookKeep::ShaderPlay, DECAY_FULL)]);
        // Undead FX's replay every 2.5 s takes the same place, newer
        assert_eq!(looks.keep(THRALL, &play(DECAY_FULL, -1.0, 2), 2_500), Kept::Replaced);
        assert_eq!(looks.served(THRALL, 3_000).first().map(|s| s.seq), Some(2));
        // its Stop clears it and the actor keeps nothing
        assert_eq!(looks.keep(THRALL, &stop(DECAY_FULL, 3), 3_000), Kept::Cleared);
        assert!(looks.served(THRALL, 3_000).is_empty());
        assert_eq!(looks.actors(), 0);
        // a Stop that names nothing keeps nothing
        assert_eq!(looks.keep(THRALL, &stop(DECAY_FULL, 4), 3_000), Kept::Nothing);
        assert_eq!(looks.actors(), 0);
    }

    #[test]
    fn a_visual_effect_keeps_its_facing_and_a_shader_of_the_same_form_is_another_look() {
        let mut looks = Looks::default();
        let fx = LookCall { keep: LookKeep::EffectPlay, relay_self: REANIMATE_FX, object0: u64::from(THRALL), number1: Some(-1.0), object2: 0x14, seq: 1 };
        assert_eq!(looks.keep(THRALL, &fx, 0), Kept::Kept);
        // a shader and an effect are kept apart, whatever their ids
        assert_eq!(looks.keep(THRALL, &play(REANIMATE_FX, -1.0, 1), 0), Kept::Kept);
        let served = looks.served(THRALL, 0);
        assert_eq!(forms(&served), [(LookKeep::EffectPlay, REANIMATE_FX), (LookKeep::ShaderPlay, REANIMATE_FX)]);
        assert_eq!(served.iter().map(|s| s.facing).collect::<Vec<_>>(), [0x14, 0]);
        let stop_fx = LookCall { keep: LookKeep::EffectStop, relay_self: REANIMATE_FX, object0: u64::from(THRALL), number1: None, object2: 0, seq: 2 };
        assert_eq!(looks.keep(THRALL, &stop_fx, 0), Kept::Cleared);
        assert_eq!(forms(&looks.served(THRALL, 0)), [(LookKeep::ShaderPlay, REANIMATE_FX)]);
    }

    #[test]
    fn the_eyes_are_one_per_actor_and_none_clears_them() {
        let mut looks = Looks::default();
        assert_eq!(looks.keep(THRALL, &eyes(EYES, 1), 0), Kept::Kept);
        assert_eq!(looks.keep(THRALL, &eyes(0x2b01_264c, 2), 0), Kept::Replaced);
        let served = looks.served(THRALL, 60_000);
        assert_eq!(served.iter().map(|s| (s.form, s.seconds, s.seq)).collect::<Vec<_>>(), [(0x2b01_264c, -1.0, 2)]);
        assert_eq!(looks.keep(THRALL, &eyes(0, 3), 0), Kept::Cleared);
        assert_eq!(looks.actors(), 0);
        // an id past 32 bits names no form: as None
        assert_eq!(looks.keep(THRALL, &eyes(1 << 40, 4), 0), Kept::Nothing);
        assert_eq!(looks.actors(), 0);
    }

    #[test]
    fn a_spent_play_keeps_nothing_and_clears_its_place() {
        let mut looks = Looks::default();
        assert_eq!(looks.keep(THRALL, &play(DISINTEGRATE, 4.0, 1), 0), Kept::Kept);
        assert_eq!(looks.keep(THRALL, &play(DISINTEGRATE, 0.0, 2), 100), Kept::Spent);
        assert!(looks.served(THRALL, 100).is_empty());
        assert_eq!(looks.actors(), 0);
    }

    #[test]
    fn an_actor_keeps_at_most_its_bound_the_oldest_going() {
        let mut looks = Looks::default();
        for i in 0..=LOOKS_KEPT {
            let seq = u32::try_from(i).unwrap_or(u32::MAX);
            looks.keep(THRALL, &play(DISINTEGRATE.saturating_add(seq), -1.0, seq), 0);
        }
        let served = looks.served(THRALL, 0);
        assert_eq!(served.len(), LOOKS_KEPT);
        assert_eq!(served.first().map(|s| s.form), Some(DISINTEGRATE.saturating_add(1)));
        // another actor's are its own
        looks.keep(DRAUGR, &play(DISINTEGRATE, 4.0, 1), 0);
        assert_eq!(forms(&looks.served(DRAUGR, 0)), [(LookKeep::ShaderPlay, DISINTEGRATE)]);
        assert_eq!(looks.served(THRALL, 0).len(), LOOKS_KEPT);
    }

    #[test]
    fn past_the_actors_bound_the_longest_unmoved_actor_goes() {
        let mut looks = Looks::default();
        for i in 0..ACTORS_KEPT {
            let actor = u32::try_from(i).unwrap_or(u32::MAX).saturating_add(1);
            looks.keep(actor, &play(DISINTEGRATE, -1.0, 1), 0);
        }
        // the first actor moves again, so the second is the longest unmoved
        looks.keep(1, &play(DECAY_FULL, -1.0, 1), 0);
        looks.keep(THRALL, &play(DISINTEGRATE, -1.0, 1), 0);
        assert_eq!(looks.actors(), ACTORS_KEPT);
        assert!(looks.served(2, 0).is_empty());
        assert_eq!(looks.served(1, 0).len(), 2);
        assert_eq!(looks.served(THRALL, 0).len(), 1);
    }

    #[test]
    fn the_seconds_left_count_from_the_keep_and_saturate() {
        let mut looks = Looks::default();
        looks.keep(THRALL, &play(DISINTEGRATE, 4.0, 1), 10_000);
        // a clock read before the keep is no time passed
        assert_eq!(looks.served(THRALL, 9_000).first().map(|s| s.seconds), Some(4.0));
        assert_eq!(looks.served(THRALL, 13_999).first().map(|s| s.seconds > 0.0), Some(true));
        assert!(looks.served(THRALL, 14_000).is_empty());
        // a duration past f32 is as long as f32 holds, never infinite; any
        // below 0 and a number that is none are until a Stop
        assert_eq!(play(DECAY_FULL, f64::MAX, 2).seconds(), f32::MAX);
        assert_eq!(play(DECAY_FULL, -7.5, 2).seconds(), -1.0);
        assert_eq!(play(DECAY_FULL, f64::NAN, 2).seconds(), -1.0);
    }
}
