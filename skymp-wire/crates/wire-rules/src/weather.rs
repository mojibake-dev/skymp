//! The server's weather (thuum docs/verbs/weather.md, M4): one weather per
//! worldspace, a pure function of the game clock, the worldspace's pool and a
//! seed, so it keeps no state worth persisting and a restart resumes where the
//! function says (as the clock does, clock.rs). The pool is the union of the
//! worldspace's weather regions' lists from the game files (REGN RDWT, read by
//! libespm: Skyrim's climate record names one weather, SkyrimCloudy, and the
//! forty weather regions carry the variety; UESP, "Skyrim Mod:Mod File
//! Format/REGN" and "/CLMT") unless server-settings.json's `weather.pools`
//! pins one for that worldspace. Time is cut into slots of `changeGameMinutes`
//! from day 0 of the clock; slot k's weather is a chance-weighted draw from the
//! pool by a hash of (seed, worldspace, k), and never its predecessor's when
//! the pool has two or more entries (so a two-entry pool alternates, which the
//! lab's pool relies on). A player hears a word once per place it enters (a
//! worldspace's weather, or a release in an interior or a worldspace without a
//! pool) and once per change it stands through. The only state is who has
//! heard what, and the last computed slot per worldspace, so the chain of
//! picks advances instead of replaying from day 0 at every ask.

use std::collections::HashMap;
use std::fmt;

/// The default slot: two game hours.
pub const DEFAULT_CHANGE_GAME_MINUTES: u32 = 120;
/// The longest slot: a game day.
pub const MAX_CHANGE_GAME_MINUTES: u32 = 1440;
/// The most weathers one pool holds (Skyrim's longest regional list, the
/// tundra's, has six; a mod's may be longer).
pub const MAX_POOL: usize = 64;

/// One weather of a pool with its chance (the region list's percent, summed
/// where regions repeat a weather).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Entry {
    /// The WTHR form id, never 0.
    pub weather: u32,
    /// Its weight, above 0.
    pub chance: u32,
}

/// A word owed to one player: the weather of the place it is in.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Word {
    /// The place: a worldspace, or the interior cell the release is for.
    pub place: u32,
    /// The WTHR form id; 0 releases the game's override (an interior, or a
    /// worldspace without a pool).
    pub weather: u32,
    /// The slot it stands for (0 for a release).
    pub slot: u64,
    /// True when the player watched the change (the same worldspace, a later
    /// slot): the game blends into it; false at a login or a place change:
    /// the game cuts to it.
    pub transition: bool,
}

/// A bad setting or pool, named.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SettingsError(pub String);

impl fmt::Display for SettingsError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.0)
    }
}

impl std::error::Error for SettingsError {}

/// What a player last heard.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Heard {
    place: u32,
    slot: u64,
    weather: u32,
}

/// The weather of every worldspace and who has heard it.
#[derive(Debug, Default)]
pub struct Weather {
    change_game_minutes: u32,
    seed: u64,
    /// From the game files' regions, merged per worldspace.
    file_pools: HashMap<u32, Vec<Entry>>,
    /// From the settings; outranks the files'.
    pinned_pools: HashMap<u32, Vec<Entry>>,
    /// The last computed (slot, weather) per worldspace.
    chains: HashMap<u32, (u64, u32)>,
    /// What each player last heard.
    heard: HashMap<u32, Heard>,
}

impl Weather {
    /// A weather from `changeGameMinutes` (1 to 1440) and a seed.
    pub fn new(change_game_minutes: u32, seed: u64) -> Result<Self, SettingsError> {
        if change_game_minutes == 0 || change_game_minutes > MAX_CHANGE_GAME_MINUTES {
            return Err(SettingsError(format!(
                "changeGameMinutes must be 1 to {MAX_CHANGE_GAME_MINUTES}, got {change_game_minutes}"
            )));
        }
        Ok(Self { change_game_minutes, seed, ..Default::default() })
    }

    /// The slot's length in game minutes.
    pub fn change_game_minutes(&self) -> u32 {
        self.change_game_minutes
    }

    /// A pool for `world`: from the game files (`pinned` false; the entries
    /// join the worldspace's pool, a repeated weather's chances summed) or
    /// from the settings (`pinned` true; replaces the pinned pool and outranks
    /// the files'). Entries with a weather or a chance of 0 are dropped; a
    /// pool left empty or grown past `MAX_POOL` is refused and nothing
    /// changes. The pool's size after.
    pub fn set_pool(&mut self, world: u32, entries: &[Entry], pinned: bool) -> Result<usize, SettingsError> {
        let mut merged: Vec<Entry> = if pinned { Vec::new() } else { self.file_pools.get(&world).cloned().unwrap_or_default() };
        for e in entries.iter().filter(|e| e.weather != 0 && e.chance != 0) {
            match merged.iter_mut().find(|m| m.weather == e.weather) {
                Some(m) => m.chance = m.chance.saturating_add(e.chance),
                None => merged.push(*e),
            }
        }
        if merged.is_empty() {
            return Err(SettingsError(format!("worldspace {world:x}: a pool needs a weather with a chance above 0")));
        }
        if merged.len() > MAX_POOL {
            return Err(SettingsError(format!("worldspace {world:x}: a pool holds at most {MAX_POOL} weathers, got {}", merged.len())));
        }
        let n = merged.len();
        if pinned {
            self.pinned_pools.insert(world, merged);
        } else {
            self.file_pools.insert(world, merged);
        }
        // the chain restarts under a new pool (a restart recomputes it the same way)
        self.chains.remove(&world);
        Ok(n)
    }

    /// The pool in force for `world`: the pinned one, else the files'.
    pub fn pool(&self, world: u32) -> Option<&[Entry]> {
        self.pinned_pools.get(&world).or_else(|| self.file_pools.get(&world)).map(Vec::as_slice)
    }

    /// The slot at `game_minutes` of the clock (days passed times 1440); a
    /// negative or absent time is slot 0.
    pub fn slot(&self, game_minutes: f64) -> u64 {
        let minutes = if game_minutes.is_finite() && game_minutes > 0.0 { game_minutes } else { 0.0 };
        to_u64(minutes / f64::from(self.change_game_minutes))
    }

    /// The weather of `world` at `slot`, by the chain from slot 0; none for a
    /// worldspace without a pool.
    pub fn weather_at(&mut self, world: u32, slot: u64) -> Option<u32> {
        let pool = self.pool(world)?.to_vec();
        let (mut k, mut prev) = match self.chains.get(&world) {
            Some(&(s, w)) if s == slot => return Some(w),
            Some(&(s, w)) if s < slot => (s.saturating_add(1), Some(w)),
            _ => (0, None),
        };
        loop {
            let raw = draw(&pool, mix(self.seed, world, k))?;
            let mut pick = pool.get(raw)?.weather;
            if Some(pick) == prev && pool.len() >= 2 {
                let next = raw.saturating_add(1).checked_rem(pool.len())?;
                pick = pool.get(next)?.weather;
            }
            prev = Some(pick);
            if k >= slot {
                break;
            }
            k = k.saturating_add(1);
        }
        let weather = prev?;
        self.chains.insert(world, (slot, weather));
        Some(weather)
    }

    /// The word `user` is owed, standing in `place` (a worldspace when
    /// `is_world`, else an interior cell; 0 for nowhere: none) at
    /// `game_minutes` of the clock. In a worldspace with a pool: its weather
    /// now, when it differs from what the user last heard (a place change
    /// cuts, a slot change in the same place blends; a slot change that keeps
    /// the weather says nothing). Elsewhere: one release per place entered. A
    /// returned word counts as heard.
    pub fn due(&mut self, user: u32, place: u32, is_world: bool, game_minutes: f64) -> Option<Word> {
        if place == 0 {
            return None;
        }
        let last = self.heard.get(&user).copied();
        let has_pool = is_world && self.pool(place).is_some();
        if !has_pool {
            if last.map(|h| h.place) == Some(place) {
                return None;
            }
            self.heard.insert(user, Heard { place, slot: 0, weather: 0 });
            return Some(Word { place, weather: 0, slot: 0, transition: false });
        }
        let slot = self.slot(game_minutes);
        let weather = self.weather_at(place, slot)?;
        let transition = match last {
            Some(h) if h.place == place && h.weather == weather => {
                self.heard.insert(user, Heard { place, slot, weather });
                return None;
            }
            Some(h) => h.place == place,
            None => false,
        };
        self.heard.insert(user, Heard { place, slot, weather });
        Some(Word { place, weather, slot, transition })
    }

    /// A player left.
    pub fn forget(&mut self, user: u32) {
        self.heard.remove(&user);
    }
}

/// A weighted index into `pool`: the hash modulo the chances' sum, walked.
fn draw(pool: &[Entry], hash: u64) -> Option<usize> {
    let total = pool.iter().fold(0_u64, |acc, e| acc.saturating_add(u64::from(e.chance)));
    let mut r = hash.checked_rem(total)?;
    for (i, e) in pool.iter().enumerate() {
        let c = u64::from(e.chance);
        if r < c {
            return Some(i);
        }
        r = r.saturating_sub(c);
    }
    pool.len().checked_sub(1)
}

/// splitmix64 over (seed, worldspace, slot).
fn mix(seed: u64, world: u32, slot: u64) -> u64 {
    let mut z = seed
        .wrapping_add(u64::from(world).wrapping_shl(32))
        .wrapping_add(slot)
        .wrapping_add(0x9E37_79B9_7F4A_7C15);
    z = (z ^ z.wrapping_shr(30)).wrapping_mul(0xBF58_476D_1CE4_E5B9);
    z = (z ^ z.wrapping_shr(27)).wrapping_mul(0x94D0_49BB_1331_11EB);
    z ^ z.wrapping_shr(31)
}

/// Saturating float to count (`as` rounds toward zero and clamps; a NaN is 0).
#[allow(clippy::as_conversions)]
fn to_u64(x: f64) -> u64 {
    x as u64
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::arithmetic_side_effects, clippy::indexing_slicing)]
mod tests {
    use super::*;

    const TAMRIEL: u32 = 0x3c;
    const SOVNGARDE: u32 = 0x2ee41;
    const TEMPLE: u32 = 0x371de;
    const CLEAR: u32 = 0x81a;
    const CLOUDY: u32 = 0x12f89;
    const RAIN: u32 = 0xc821f;

    fn e(weather: u32, chance: u32) -> Entry {
        Entry { weather, chance }
    }

    fn lab() -> Weather {
        let mut w = Weather::new(10, 1).unwrap();
        w.set_pool(TAMRIEL, &[e(CLEAR, 50), e(CLOUDY, 50)], true).unwrap();
        w
    }

    fn three(seed: u64) -> Weather {
        let mut w = Weather::new(120, seed).unwrap();
        w.set_pool(TAMRIEL, &[e(CLEAR, 80), e(CLOUDY, 10), e(RAIN, 10)], false).unwrap();
        w
    }

    #[test]
    fn slots_cut_the_clock_from_day_zero() {
        let w = lab();
        assert_eq!(w.slot(0.0), 0);
        assert_eq!(w.slot(9.99), 0);
        assert_eq!(w.slot(10.0), 1);
        assert_eq!(w.slot(1440.0), 144);
        assert_eq!(w.slot(-5.0), 0);
        assert_eq!(w.slot(f64::NAN), 0);
        assert_eq!(w.slot(f64::INFINITY), 0);
        assert_eq!(Weather::new(120, 0).unwrap().slot(1440.0 * 3.0 + 119.0), 36);
    }

    #[test]
    fn a_two_entry_pool_alternates_and_a_pool_of_one_stands() {
        let mut w = lab();
        for k in 1..500 {
            assert_ne!(w.weather_at(TAMRIEL, k), w.weather_at(TAMRIEL, k - 1), "slot {k}");
        }
        let mut one = Weather::new(10, 1).unwrap();
        one.set_pool(TAMRIEL, &[e(CLOUDY, 100)], false).unwrap();
        for k in 0..50 {
            assert_eq!(one.weather_at(TAMRIEL, k), Some(CLOUDY));
        }
    }

    #[test]
    fn the_chain_is_a_pure_function_of_seed_world_and_slot() {
        let mut a = three(7);
        let mut b = three(7);
        let forward: Vec<_> = (0..300).map(|k| a.weather_at(TAMRIEL, k).unwrap()).collect();
        // asked out of order, and backwards, the same answers
        let mut backward: Vec<_> = (0..300).rev().map(|k| b.weather_at(TAMRIEL, k).unwrap()).collect();
        backward.reverse();
        assert_eq!(forward, backward);
        let mut c = three(8);
        let other: Vec<_> = (0..300).map(|k| c.weather_at(TAMRIEL, k).unwrap()).collect();
        assert_ne!(forward, other, "a different seed is a different sky");
        // a different worldspace with the same pool is its own sequence
        let mut d = three(7);
        d.set_pool(SOVNGARDE, &[e(CLEAR, 80), e(CLOUDY, 10), e(RAIN, 10)], false).unwrap();
        let sov: Vec<_> = (0..300).map(|k| d.weather_at(SOVNGARDE, k).unwrap()).collect();
        assert_ne!(forward, sov);
    }

    #[test]
    fn chances_weight_the_draw() {
        let mut w = three(3);
        let mut counts = HashMap::new();
        for k in 0..20_000 {
            *counts.entry(w.weather_at(TAMRIEL, k).unwrap()).or_insert(0_u32) += 1;
        }
        let (clear, cloudy, rain) = (counts[&CLEAR], counts[&CLOUDY], counts[&RAIN]);
        assert!(clear > cloudy && clear > rain, "{counts:?}");
        assert!(cloudy > 0 && rain > 0, "{counts:?}");
        // never twice in a row
        let mut v = three(3);
        for k in 1..2_000 {
            assert_ne!(v.weather_at(TAMRIEL, k), v.weather_at(TAMRIEL, k - 1));
        }
    }

    #[test]
    fn a_player_hears_a_change_as_a_transition_and_a_new_place_as_a_cut() {
        let mut w = lab();
        let first = w.due(7, TAMRIEL, true, 0.0).unwrap();
        assert_eq!((first.place, first.slot, first.transition), (TAMRIEL, 0, false));
        assert!(first.weather == CLEAR || first.weather == CLOUDY);
        assert_eq!(w.due(7, TAMRIEL, true, 5.0), None, "the same slot is heard");
        let next = w.due(7, TAMRIEL, true, 10.0).unwrap();
        assert_eq!((next.slot, next.transition), (1, true));
        assert_ne!(next.weather, first.weather);
        // an interior: one release, then nothing while the player stays
        let inside = w.due(7, TEMPLE, false, 12.0).unwrap();
        assert_eq!(inside, Word { place: TEMPLE, weather: 0, slot: 0, transition: false });
        assert_eq!(w.due(7, TEMPLE, false, 13.0), None);
        // out again in the same slot: a cut (the place changed)
        let back = w.due(7, TAMRIEL, true, 14.0).unwrap();
        assert_eq!((back.slot, back.transition, back.weather), (1, false, next.weather));
        // another worldspace with a pool: a cut
        w.set_pool(SOVNGARDE, &[e(RAIN, 100)], false).unwrap();
        let sov = w.due(7, SOVNGARDE, true, 15.0).unwrap();
        assert_eq!((sov.place, sov.weather, sov.transition), (SOVNGARDE, RAIN, false));
        // a slot change that keeps the weather (a pool of one) says nothing
        assert_eq!(w.due(7, SOVNGARDE, true, 25.0), None);
        // a worldspace without a pool: a release, once
        assert_eq!(w.due(7, 0x1234, true, 26.0), Some(Word { place: 0x1234, weather: 0, slot: 0, transition: false }));
        assert_eq!(w.due(7, 0x1234, true, 27.0), None);
        // nowhere: nothing
        assert_eq!(w.due(7, 0, true, 27.0), None);
        // a second player hears its own first word; a relaunch hears again
        assert_eq!(w.due(8, TAMRIEL, true, 14.0).map(|x| x.transition), Some(false));
        w.forget(8);
        assert_eq!(w.due(8, TAMRIEL, true, 14.0).map(|x| (x.slot, x.transition)), Some((1, false)));
    }

    #[test]
    fn pools_merge_from_files_and_the_settings_pin() {
        let mut w = Weather::new(120, 0).unwrap();
        assert_eq!(w.set_pool(TAMRIEL, &[e(CLEAR, 30), e(RAIN, 5)], false), Ok(2));
        assert_eq!(w.set_pool(TAMRIEL, &[e(CLEAR, 20), e(CLOUDY, 0), e(0, 9)], false), Ok(2));
        assert_eq!(w.pool(TAMRIEL), Some(&[e(CLEAR, 50), e(RAIN, 5)][..]));
        assert_eq!(w.set_pool(TAMRIEL, &[e(CLOUDY, 100)], true), Ok(1));
        assert_eq!(w.pool(TAMRIEL), Some(&[e(CLOUDY, 100)][..]));
        assert_eq!(w.weather_at(TAMRIEL, 9), Some(CLOUDY));
        assert!(w.set_pool(SOVNGARDE, &[], false).is_err());
        assert!(w.set_pool(SOVNGARDE, &[e(CLEAR, 0)], false).is_err());
        assert_eq!(w.pool(SOVNGARDE), None);
        let big: Vec<Entry> = (1..=65).map(|i| e(i, 1)).collect();
        assert!(w.set_pool(SOVNGARDE, &big, true).is_err());
        assert!(Weather::new(0, 0).is_err());
        assert!(Weather::new(1441, 0).is_err());
        assert_eq!(Weather::new(1440, 0).map(|x| x.change_game_minutes()), Ok(1440));
    }
}
