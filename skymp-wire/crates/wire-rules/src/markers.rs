//! Map markers a player discovers (thuum docs/verbs/map-markers.md). The
//! player's engine decides that a location is discovered and its client
//! reports only the marker's type; the event carries no reference (CommonLib
//! LocationDiscovery::Event holds the MapMarkerData alone). So the server
//! decides which marker that was from its own facts: the master files'
//! markers of that type in the player's worldspace, and the player's
//! position. The nearest within range is the one; none within range refuses
//! the report.

/// How far from the player a discovered marker may lie. Measured (thuum run
/// 20261005-222458): the engine discovered the clearing REFR 0x00016223
/// with the player 2000 units off in x-y and not at 3000, and the server
/// measured the report at 1947 units. The bound stays generous on purpose:
/// a refusal loses a legitimate discovery from the player's map (a location
/// with a wider discovery radius than a clearing's, a stale position), while
/// a loose bound only lets a client claim markers near where the server
/// already holds it. A refusal logs the nearest candidate's distance.
pub const DISCOVERY_RANGE: f32 = 8000.0;

/// A marker of the reported type in the player's worldspace, from the
/// master files.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Candidate {
    /// The marker reference's form id.
    pub refr_id: u32,
    /// Its position.
    pub x: f32,
    /// Its position.
    pub y: f32,
    /// Its position.
    pub z: f32,
}

/// The rule's answer: the marker meant, or none in range, with the nearest
/// candidate's distance either way (infinite when there is no candidate).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Choice {
    /// A candidate lies within range.
    pub found: bool,
    /// The nearest candidate's form id; 0 when there is none.
    pub refr_id: u32,
    /// The nearest candidate's distance from the player.
    pub distance: f32,
}

/// The marker a discovery reported at the player's position means: the
/// nearest candidate, if it lies within [`DISCOVERY_RANGE`]. Non-finite
/// coordinates never match.
#[must_use]
pub fn discovered(x: f32, y: f32, z: f32, candidates: &[Candidate]) -> Choice {
    let mut best = Choice { found: false, refr_id: 0, distance: f32::INFINITY };
    for c in candidates {
        let (dx, dy, dz) = (c.x - x, c.y - y, c.z - z);
        let d = (dx * dx + dy * dy + dz * dz).sqrt();
        if d.is_finite() && d < best.distance {
            best = Choice { found: false, refr_id: c.refr_id, distance: d };
        }
    }
    best.found = best.distance <= DISCOVERY_RANGE;
    best
}

/// A marker recorded again keeps the wider travel flag: a discovery never
/// takes fast travel away.
#[must_use]
pub const fn travel_after(recorded: bool, reported: bool) -> bool {
    recorded || reported
}

#[cfg(test)]
mod tests {
    use super::*;

    const fn c(refr_id: u32, x: f32, y: f32) -> Candidate {
        Candidate { refr_id, x, y, z: 0.0 }
    }

    #[test]
    fn the_nearest_marker_of_the_type_is_the_one() {
        let got = discovered(0.0, 0.0, 0.0, &[c(1, 3000.0, 0.0), c(2, 0.0, 1000.0), c(3, -5000.0, 0.0)]);
        assert_eq!(got, Choice { found: true, refr_id: 2, distance: 1000.0 });
    }

    #[test]
    fn none_within_range_refuses_and_still_says_how_far() {
        let got = discovered(0.0, 0.0, 0.0, &[c(7, DISCOVERY_RANGE + 1.0, 0.0)]);
        assert!(!got.found);
        assert_eq!(got.refr_id, 7);
        assert_eq!(got.distance, DISCOVERY_RANGE + 1.0);
        assert!(discovered(0.0, 0.0, 0.0, &[c(8, DISCOVERY_RANGE, 0.0)]).found);
    }

    #[test]
    fn no_candidate_is_no_marker() {
        let got = discovered(0.0, 0.0, 0.0, &[]);
        assert_eq!(got, Choice { found: false, refr_id: 0, distance: f32::INFINITY });
    }

    #[test]
    fn height_counts() {
        let high = Candidate { refr_id: 4, x: 0.0, y: 0.0, z: DISCOVERY_RANGE + 1.0 };
        assert!(!discovered(0.0, 0.0, 0.0, &[high]).found);
    }

    #[test]
    fn a_bad_position_matches_nothing() {
        for bad in [f32::NAN, f32::INFINITY] {
            assert!(!discovered(bad, 0.0, 0.0, &[c(1, 0.0, 0.0)]).found);
        }
    }

    #[test]
    fn fast_travel_is_never_taken_away() {
        assert!(travel_after(true, false));
        assert!(travel_after(false, true));
        assert!(!travel_after(false, false));
    }
}
