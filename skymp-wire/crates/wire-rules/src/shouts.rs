//! Shouts (thuum docs/verbs/shouts.md, M2). Whether a shout a player's game
//! reports stands (R1 against the server's record, R0): the voice slot holds
//! a shout the record has, the cast's spell is one of its variations, every
//! word up to that variation is unlocked in the record (a two-word shout
//! needs the first two words), and the player's last accepted shout's
//! recovery has run out on the server's clock. An accepted shout starts the
//! next recovery: the variation's recovery time times the shouter's
//! ShoutRecoveryMult, as the engine sets it (x-shout-probe3 20261011-221358
//! read 20 and 45 for Unrelenting Force's two and three words at a
//! ShoutRecoveryMult of 1). A recovery is a runtime timer, forgotten at a
//! restart as a cast is, and kept across a disconnect (a reconnect does not
//! end it).

use std::collections::HashMap;

/// The longest recovery the rule keeps: an hour (the masters' longest is
/// Dragonrend's three words, a few minutes).
const MAX_RECOVERY_MS: f64 = 3_600_000.0;

/// How much of the last recovery may be left when the next shout arrives,
/// milliseconds. The server starts a recovery when a shout's report
/// arrives, the shouter's game when it shouts, and the report goes out on
/// the frames after the release, on the client's one ordered channel: an
/// earlier report resent once arrives 300 ms late (wire-transport
/// `Limits::resend_ms`, as ranged::DELIVERY_SKEW_MS) and its read can lag a
/// few frames more, so the next shout, on time in the game, finds that much
/// left on the server. Playtest fifteen's Become Ethereal (20.0 s) was
/// refused with 18 ms left (thuum docs/verbs/shouts.md).
pub const RECOVERY_SKEW_MS: u64 = 400;

/// What the server knows about a shout a player's game reported.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct VoiceFacts {
    /// The voice slot holds a shout (SHOU) the player's record has.
    pub shout_equipped: bool,
    /// The cast spell's place among the shout's variations, 0 to 2, or
    /// anything else for none.
    pub variation: u8,
    /// Whether each of the shout's three words is unlocked in the record.
    pub words_unlocked: [bool; 3],
    /// The variation's recovery time, seconds.
    pub recovery_s: f32,
    /// The shouter's ShoutRecoveryMult (1 without a record).
    pub recovery_mult: f32,
}

/// Why a shout was refused, or Accepted.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VoiceRefusal {
    /// The shout stands.
    Accepted,
    /// The voice slot holds no shout the record has.
    NoShout,
    /// The spell is none of the shout's variations.
    NotAVariation,
    /// A word up to the variation is not unlocked in the record.
    WordLocked,
    /// The last shout's recovery has not run out.
    Recovering,
}

/// A recovery in milliseconds: the variation's seconds times the shouter's
/// mult, none when that is not a number or not above zero, at most an hour.
fn recovery_ms(recovery_s: f32, recovery_mult: f32) -> u64 {
    let ms = f64::from(recovery_s) * f64::from(recovery_mult) * 1000.0;
    if !ms.is_finite() || ms <= 0.0 {
        return 0;
    }
    #[allow(clippy::as_conversions, clippy::cast_possible_truncation, clippy::cast_sign_loss)] // range checked above
    let ms = ms.min(MAX_RECOVERY_MS).round() as u64;
    ms
}

/// Every player's voice: when its last accepted shout's recovery ends, on a
/// monotonic clock in milliseconds.
#[derive(Debug, Default)]
pub struct Voices {
    recovered_at: HashMap<u32, u64>,
}

impl Voices {
    /// The verdict on `actor`'s shout at `now_ms`; an accepted one starts
    /// its recovery, a refused one leaves the last.
    pub fn shout(&mut self, actor: u32, f: &VoiceFacts, now_ms: u64) -> VoiceRefusal {
        if !f.shout_equipped {
            return VoiceRefusal::NoShout;
        }
        let Some(words) = f.words_unlocked.get(..=usize::from(f.variation)) else {
            return VoiceRefusal::NotAVariation;
        };
        if !words.iter().all(|&u| u) {
            return VoiceRefusal::WordLocked;
        }
        if self.recovery_left_ms(actor, now_ms) > RECOVERY_SKEW_MS {
            return VoiceRefusal::Recovering;
        }
        self.recovered_at.insert(actor, now_ms.saturating_add(recovery_ms(f.recovery_s, f.recovery_mult)));
        VoiceRefusal::Accepted
    }

    /// How long `actor`'s recovery runs from `now_ms` (0 when it has run out
    /// or never ran).
    #[must_use]
    pub fn recovery_left_ms(&self, actor: u32, now_ms: u64) -> u64 {
        self.recovered_at.get(&actor).map_or(0, |&at| at.saturating_sub(now_ms))
    }

}

#[cfg(test)]
mod tests {
    use super::*;

    const ACTOR: u32 = 0xff00_0000;

    // Unrelenting Force's second variation (Skyrim.esm 0x13E07: recovery 15,
    // 20 and 45), its first two words unlocked
    fn two_words() -> VoiceFacts {
        VoiceFacts {
            shout_equipped: true,
            variation: 1,
            words_unlocked: [true, true, false],
            recovery_s: 20.0,
            recovery_mult: 1.0,
        }
    }

    #[test]
    fn an_unlocked_shout_stands_and_starts_its_recovery() {
        let mut v = Voices::default();
        assert_eq!(v.shout(ACTOR, &two_words(), 100_000), VoiceRefusal::Accepted);
        assert_eq!(v.recovery_left_ms(ACTOR, 100_000), 20_000);
        assert_eq!(v.recovery_left_ms(ACTOR, 125_000), 0);
        // the Amulet of Talos's kind: a smaller mult, a shorter recovery
        let mut talos = Voices::default();
        assert_eq!(talos.shout(ACTOR, &VoiceFacts { recovery_mult: 0.8, ..two_words() }, 100_000), VoiceRefusal::Accepted);
        assert_eq!(talos.recovery_left_ms(ACTOR, 100_000), 16_000);
    }

    #[test]
    fn the_record_refuses() {
        let mut v = Voices::default();
        assert_eq!(v.shout(ACTOR, &VoiceFacts { shout_equipped: false, ..two_words() }, 0), VoiceRefusal::NoShout);
        assert_eq!(v.shout(ACTOR, &VoiceFacts { variation: 3, ..two_words() }, 0), VoiceRefusal::NotAVariation);
        // three words with the third locked; two with the first locked
        assert_eq!(v.shout(ACTOR, &VoiceFacts { variation: 2, ..two_words() }, 0), VoiceRefusal::WordLocked);
        assert_eq!(
            v.shout(ACTOR, &VoiceFacts { words_unlocked: [false, true, true], ..two_words() }, 0),
            VoiceRefusal::WordLocked
        );
        // a refused shout starts no recovery
        assert_eq!(v.recovery_left_ms(ACTOR, 0), 0);
    }

    #[test]
    fn a_shout_inside_the_recovery_is_refused_and_leaves_it() {
        let mut v = Voices::default();
        assert_eq!(v.shout(ACTOR, &two_words(), 100_000), VoiceRefusal::Accepted);
        assert_eq!(v.shout(ACTOR, &two_words(), 110_000), VoiceRefusal::Recovering);
        assert_eq!(v.recovery_left_ms(ACTOR, 110_000), 10_000);
        // another player's voice is its own
        assert_eq!(v.shout(ACTOR + 1, &two_words(), 110_000), VoiceRefusal::Accepted);
        // at the recovery's end the next stands
        assert_eq!(v.shout(ACTOR, &two_words(), 120_000), VoiceRefusal::Accepted);
        assert_eq!(v.recovery_left_ms(ACTOR, 120_000), 20_000);
    }

    #[test]
    fn a_shout_on_time_in_its_game_stands_on_the_server() {
        // playtest fifteen: Become Ethereal (20 s) again 19.982 s later
        let mut v = Voices::default();
        let ethereal = VoiceFacts { variation: 0, recovery_s: 20.0, ..two_words() };
        assert_eq!(v.shout(ACTOR, &ethereal, 100_000), VoiceRefusal::Accepted);
        assert_eq!(v.shout(ACTOR, &ethereal, 119_982), VoiceRefusal::Accepted);
        // the skew's edge, and past it
        assert_eq!(v.shout(ACTOR, &ethereal, 139_982 - RECOVERY_SKEW_MS), VoiceRefusal::Accepted);
        let edge = 139_982 - RECOVERY_SKEW_MS + 20_000 - RECOVERY_SKEW_MS - 1;
        assert_eq!(v.shout(ACTOR, &ethereal, edge), VoiceRefusal::Recovering);
        // a second early is still refused
        assert_eq!(v.shout(ACTOR, &ethereal, edge - 600), VoiceRefusal::Recovering);
    }

    #[test]
    fn nonsense_recovery_sets_none() {
        let mut v = Voices::default();
        assert_eq!(v.shout(ACTOR, &VoiceFacts { recovery_s: f32::NAN, ..two_words() }, 100_000), VoiceRefusal::Accepted);
        assert_eq!(v.recovery_left_ms(ACTOR, 100_000), 0);
        assert_eq!(v.shout(ACTOR, &VoiceFacts { recovery_mult: -1.0, ..two_words() }, 100_000), VoiceRefusal::Accepted);
        assert_eq!(v.recovery_left_ms(ACTOR, 100_000), 0);
        assert_eq!(v.shout(ACTOR, &VoiceFacts { recovery_s: f32::INFINITY, ..two_words() }, 100_000), VoiceRefusal::Accepted);
        assert_eq!(v.recovery_left_ms(ACTOR, 100_000), 0);
        let mut long = Voices::default();
        assert_eq!(long.shout(ACTOR, &VoiceFacts { recovery_s: 1.0e9, ..two_words() }, 0), VoiceRefusal::Accepted);
        assert_eq!(long.recovery_left_ms(ACTOR, 0), 3_600_000);
    }
}
