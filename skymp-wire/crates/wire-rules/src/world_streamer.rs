//! The world streamer (thuum ADR-068 rung 4 and its amendment B;
//! docs/research/rung4-state-rows-2026-11-07.md section 1(c)): the one seat
//! that streams what nobody owns. A world global (WorldGlobal 80) is written
//! by scripts every seat runs (D-F1 C: a quest's, a global's, an alias's
//! consequences are nobody's), so every seat would report the same write; one
//! seat at an epoch reports instead, and rung 5's ScriptVariables reuses the
//! same election. The epoch is ADR-066's fence (decision 3): it rises at each
//! change of the streamer, the word naming the new one carries it, and a
//! report is judged by it first (decision 4): below it StaleEpoch, above it
//! FutureEpoch, at it from any seat but the streamer NotOwner, Current from
//! the streamer.
//!
//! Eligibility is built from what ADR-066 decision 6 has today (amendment B:
//! cells.rs keeps stale and dead per player; the pause and load flags and
//! OwnerAck are still absent): a seat is eligible once past its first
//! movement report (its game is in the world, past the login's load block),
//! while its connection is not stale and its player not dead. An election
//! picks the lowest eligible player id (UNMEASURED choice: any deterministic
//! pick works; the lowest keeps a reconnect's new id out of the way). An
//! election runs when the streamer leaves, goes stale or dies, and when
//! nobody streams and a seat becomes eligible; an eligible streamer is never
//! displaced by a newcomer, so the stream does not hop at every join.

use std::collections::BTreeMap;

/// A seat as the election reads it.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
struct Seat {
    /// Past its first movement report.
    moved: bool,
    /// Its connection is stale (cells.rs's stale).
    stale: bool,
    /// Its player is dead (cells.rs's dead).
    dead: bool,
}

impl Seat {
    fn eligible(self) -> bool {
        self.moved && !self.stale && !self.dead
    }
}

/// An election's outcome: who streams from now and at which epoch.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Election {
    /// The streaming seat's player, 0 for none.
    pub streamer: u32,
    /// The epoch the word names.
    pub epoch: u32,
    /// The seat that streamed before, 0 for none: its stream ends.
    pub previous: u32,
}

/// A report's standing against the streamer and its epoch (ADR-066
/// decision 4, the epoch first).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Judgement {
    /// The streamer at the epoch.
    Current,
    /// An epoch below the streamer's.
    StaleEpoch,
    /// An epoch above it.
    FutureEpoch,
    /// The epoch from a seat that does not stream (or while nobody does).
    NotOwner,
}

/// The seats, the streamer and its epoch. Runtime only: a restart elects
/// afresh from epoch 1, and a report made under the old run's epoch is
/// stale or future by number alone until its seat hears the new word.
#[derive(Debug, Default)]
pub struct WorldStreamer {
    seats: BTreeMap<u32, Seat>,
    streamer: u32,
    epoch: u32,
}

impl WorldStreamer {
    /// The streamer now (0 for none) and the epoch.
    #[must_use]
    pub fn streamer(&self) -> (u32, u32) {
        (self.streamer, self.epoch)
    }

    /// Whether `player` is eligible now.
    #[must_use]
    pub fn eligible(&self, player: u32) -> bool {
        self.seats.get(&player).is_some_and(|s| s.eligible())
    }

    /// `player`'s movement report: its first makes the seat eligible (an
    /// election when nobody streams); a later one changes nothing.
    pub fn moved(&mut self, player: u32) -> Option<Election> {
        let seat = self.seats.entry(player).or_default();
        if seat.moved {
            return None;
        }
        seat.moved = true;
        self.elect()
    }

    /// `player`'s connection went stale (`true`) or came back (`false`).
    pub fn stale(&mut self, player: u32, stale: bool) -> Option<Election> {
        let seat = self.seats.entry(player).or_default();
        if seat.stale == stale {
            return None;
        }
        seat.stale = stale;
        self.elect()
    }

    /// `player`'s player died (`true`) or respawned (`false`).
    pub fn dead(&mut self, player: u32, dead: bool) -> Option<Election> {
        let seat = self.seats.entry(player).or_default();
        if seat.dead == dead {
            return None;
        }
        seat.dead = dead;
        self.elect()
    }

    /// `player` left: its seat is forgotten (a reconnect starts past no
    /// movement report).
    pub fn gone(&mut self, player: u32) -> Option<Election> {
        self.seats.remove(&player)?;
        self.elect()
    }

    /// The election: an eligible streamer keeps the stream; otherwise the
    /// lowest eligible player streams from the next epoch (nobody, when
    /// none is eligible). None when nothing changed.
    fn elect(&mut self) -> Option<Election> {
        if self.streamer != 0 && self.eligible(self.streamer) {
            return None;
        }
        let next = self.seats.iter().find(|(_, s)| s.eligible()).map_or(0, |(&p, _)| p);
        if next == self.streamer {
            return None;
        }
        let previous = self.streamer;
        self.streamer = next;
        self.epoch = self.epoch.saturating_add(1);
        Some(Election { streamer: next, epoch: self.epoch, previous })
    }

    /// A report from `sender` naming `epoch`, the epoch first.
    #[must_use]
    pub fn judge(&self, sender: u32, epoch: u32) -> Judgement {
        if epoch < self.epoch {
            Judgement::StaleEpoch
        } else if epoch > self.epoch {
            Judgement::FutureEpoch
        } else if self.streamer == 0 || sender != self.streamer {
            Judgement::NotOwner
        } else {
            Judgement::Current
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const C1: u32 = 0xff00_0001;
    const C2: u32 = 0xff00_0002;
    const C3: u32 = 0xff00_0003;

    #[test]
    fn the_first_seat_past_its_first_movement_report_streams() {
        let mut w = WorldStreamer::default();
        assert_eq!(w.streamer(), (0, 0));
        // a login is no movement report: nobody streams until one comes
        assert_eq!(w.stale(C2, false), None);
        assert_eq!(w.moved(C2), Some(Election { streamer: C2, epoch: 1, previous: 0 }));
        // a later report changes nothing
        assert_eq!(w.moved(C2), None);
        // a lower id arriving later does not displace an eligible streamer
        assert_eq!(w.moved(C1), None);
        assert_eq!(w.streamer(), (C2, 1));
    }

    #[test]
    fn the_streamer_leaving_going_stale_or_dying_hands_it_on() {
        let mut w = WorldStreamer::default();
        w.moved(C2);
        w.moved(C1);
        w.moved(C3);
        // the lowest eligible wins each election
        assert_eq!(w.gone(C2), Some(Election { streamer: C1, epoch: 2, previous: C2 }));
        assert_eq!(w.stale(C1, true), Some(Election { streamer: C3, epoch: 3, previous: C1 }));
        // a stale seat's return is no election while the streamer holds
        assert_eq!(w.stale(C1, false), None);
        assert_eq!(w.dead(C3, true), Some(Election { streamer: C1, epoch: 4, previous: C3 }));
        assert_eq!(w.dead(C3, false), None);
        // the last eligible seat gone: nobody, at its own epoch
        w.gone(C3);
        assert_eq!(w.gone(C1), Some(Election { streamer: 0, epoch: 5, previous: C1 }));
        assert_eq!(w.gone(C1), None);
        // a reconnect starts past no movement report
        assert_eq!(w.moved(C1), Some(Election { streamer: C1, epoch: 6, previous: 0 }));
    }

    #[test]
    fn a_dead_or_stale_seat_is_not_elected() {
        let mut w = WorldStreamer::default();
        assert_eq!(w.dead(C1, true), None);
        assert_eq!(w.moved(C1), None, "a dead seat waits");
        assert_eq!(w.stale(C2, true), None);
        assert_eq!(w.moved(C2), None, "a stale seat waits");
        assert_eq!(w.dead(C1, false), Some(Election { streamer: C1, epoch: 1, previous: 0 }));
        assert!(w.eligible(C1) && !w.eligible(C2));
    }

    #[test]
    fn a_report_is_judged_by_its_epoch_first() {
        let mut w = WorldStreamer::default();
        // nobody streams at epoch 0
        assert_eq!(w.judge(C1, 0), Judgement::NotOwner);
        w.moved(C1);
        w.moved(C2);
        assert_eq!(w.judge(C1, 1), Judgement::Current);
        assert_eq!(w.judge(C2, 1), Judgement::NotOwner);
        assert_eq!(w.judge(C1, 2), Judgement::FutureEpoch);
        w.gone(C1);
        // the old streamer's last report at its epoch is stale, whoever
        // sends it; the new one's is current
        assert_eq!(w.judge(C1, 1), Judgement::StaleEpoch);
        assert_eq!(w.judge(C2, 1), Judgement::StaleEpoch);
        assert_eq!(w.judge(C2, 2), Judgement::Current);
    }
}
