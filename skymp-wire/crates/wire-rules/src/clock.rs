//! The server's game clock (thuum docs/verbs/time.md, ADR-021). Game time is
//! a pure function of the server's wall clock and the `time` settings: it
//! keeps no state, so it has nothing to persist, runs while nobody is
//! online, and a restart resumes where the function says. The only state
//! here is who has heard the clock and when, for the resync.
//!
//! Game mode (the default) runs a Tamriel calendar at `timeScale` game
//! seconds per real second from `start` at `epoch`. Months are
//! CommonLibSSE-NG's Calendar::DAYS_IN_MONTH (include/RE/C/Calendar.h:12-24):
//! no leap years, so a year is 365 days from any date. Real-time mode is
//! SkyMP's own mapping (skymp5-client timeService.ts), moved to the server.

use std::collections::HashMap;

use chrono::{DateTime, Datelike, Timelike, Utc};
use serde::Deserialize;

/// Days in each month, Morning Star first (CommonLibSSE-NG
/// Calendar::DAYS_IN_MONTH).
pub const DAYS_IN_MONTH: [u32; 12] = [31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31];
/// The months' sum: a Tamriel year.
const DAYS_IN_YEAR: u64 = 365;
/// How long after a player last heard the clock they hear it again.
pub const RESYNC_MS: i64 = 60_000;

const MS_PER_HOUR: f64 = 3_600_000.0;
const MS_PER_DAY: f64 = 86_400_000.0;

/// The clock at one instant, as the engine's six time globals hold it.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct GameTime {
    /// GameYear.
    pub year: u32,
    /// GameMonth, from 0 (Morning Star) to 11.
    pub month: u32,
    /// GameDay, from 1.
    pub day: u32,
    /// GameHour, at least 0 and below 24.
    pub hour: f32,
    /// GameDaysPassed.
    pub days_passed: f32,
    /// TimeScale: game seconds per real second.
    pub time_scale: f32,
}

/// Which clock the server runs.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum Mode {
    /// A Tamriel calendar at the game's rate (ADR-021 decision 1, TES3MP's).
    #[default]
    Game,
    /// The real date and time of day (SkyMP's behavior before ADR-021).
    RealTimeOfDay,
}

/// The calendar at the epoch.
#[derive(Debug, Clone, Copy, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields, default)]
pub struct Start {
    /// GameYear.
    pub year: u32,
    /// GameMonth, from 0.
    pub month: u32,
    /// GameDay, from 1.
    pub day: u32,
    /// GameHour.
    pub hour: f64,
    /// GameDaysPassed.
    pub days_passed: f64,
}

impl Default for Start {
    /// Skyrim.esm's own values (GameYear 0x35, GameMonth 0x36, GameDay 0x37,
    /// GameHour 0x38, GameDaysPassed 0x39; lab/esm.py on sky-srv,
    /// 2026-10-03): the clock starts where a new game starts, the 17th of
    /// Last Seed, 4E 201, at 08:00.
    fn default() -> Self {
        Self { year: 201, month: 7, day: 17, hour: 8.0, days_passed: 1.0 }
    }
}

/// The `time` block of server-settings.json. Every key is optional.
#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields, default)]
pub struct Settings {
    /// Which clock.
    pub mode: Mode,
    /// Game seconds per real second, game mode only. Skyrim.esm's TimeScale
    /// (GLOB 0x3a) is 20.
    pub time_scale: f64,
    /// The RFC 3339 instant at which the clock reads `start`.
    pub epoch: String,
    /// The calendar at the epoch.
    pub start: Start,
    /// Real-time mode only: hours added to UTC (SkyMP's client-side
    /// `hoursOffset`, now the server's).
    pub utc_offset_hours: f64,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            mode: Mode::Game,
            time_scale: 20.0,
            // the day ADR-021 was accepted
            epoch: "2026-10-03T00:00:00Z".into(),
            start: Start::default(),
            utc_offset_hours: 0.0,
        }
    }
}

/// Why the settings were refused. The message names the key.
#[derive(Debug, Clone, PartialEq, Eq, thiserror::Error)]
#[error("server settings time: {0}")]
pub struct SettingsError(pub String);

/// The clock and who has heard it.
#[derive(Debug, Clone)]
pub struct Clock {
    settings: Settings,
    epoch_ms: i64,
    heard_ms: HashMap<u32, i64>,
}

impl Clock {
    /// A clock from the `time` block's JSON text (`{}` for every default).
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let settings: Settings = serde_json::from_str(text).map_err(|e| SettingsError(e.to_string()))?;
        Self::new(settings)
    }

    /// A clock from parsed settings, each checked.
    pub fn new(settings: Settings) -> Result<Self, SettingsError> {
        let bad = |what: &str| Err(SettingsError(what.into()));
        if !(settings.time_scale.is_finite() && settings.time_scale > 0.0) {
            return bad("timeScale must be a positive number");
        }
        if !(settings.utc_offset_hours.is_finite() && settings.utc_offset_hours.abs() <= 24.0) {
            return bad("utcOffsetHours must be between -24 and 24");
        }
        let s = settings.start;
        let Some(&days_in_month) = DAYS_IN_MONTH.get(usize::try_from(s.month).unwrap_or(usize::MAX)) else {
            return bad("start.month must be 0 to 11");
        };
        if !(1..=days_in_month).contains(&s.day) {
            return bad("start.day must be a day of start.month");
        }
        if !(s.hour.is_finite() && (0.0..24.0).contains(&s.hour)) {
            return bad("start.hour must be at least 0 and below 24");
        }
        if !(s.days_passed.is_finite() && s.days_passed >= 0.0) {
            return bad("start.daysPassed must be at least 0");
        }
        let epoch_ms = DateTime::parse_from_rfc3339(&settings.epoch)
            .map_err(|e| SettingsError(format!("epoch {:?}: {e}", settings.epoch)))?
            .timestamp_millis();
        Ok(Self { settings, epoch_ms, heard_ms: HashMap::new() })
    }

    /// The settings in force.
    pub fn settings(&self) -> &Settings {
        &self.settings
    }

    /// The clock at `now_ms`, Unix milliseconds.
    pub fn now(&self, now_ms: i64) -> GameTime {
        let elapsed_ms = ms_as_f64(now_ms.saturating_sub(self.epoch_ms).max(0));
        match self.settings.mode {
            Mode::Game => self.game(elapsed_ms),
            Mode::RealTimeOfDay => self.real(now_ms, elapsed_ms),
        }
    }

    fn game(&self, elapsed_ms: f64) -> GameTime {
        let s = self.settings.start;
        let elapsed_hours = elapsed_ms * self.settings.time_scale / MS_PER_HOUR;
        let total_hours = s.hour + elapsed_hours;
        let hour = total_hours.rem_euclid(24.0);
        let days = whole(((total_hours - hour) / 24.0).round());
        let (year, month, day) = advance(s.year, s.month, s.day, days);
        GameTime {
            year,
            month,
            day,
            hour: below_24(hour),
            days_passed: narrow(s.days_passed + elapsed_hours / 24.0),
            time_scale: narrow(self.settings.time_scale),
        }
    }

    fn real(&self, now_ms: i64, elapsed_ms: f64) -> GameTime {
        let offset_ms = rounded_ms(self.settings.utc_offset_hours * MS_PER_HOUR);
        let t = DateTime::<Utc>::from_timestamp_millis(now_ms.saturating_add(offset_ms)).unwrap_or_default();
        let hour = f64::from(t.hour())
            + f64::from(t.minute()) / 60.0
            + f64::from(t.second()) / 3600.0
            + f64::from(t.timestamp_subsec_millis()) / MS_PER_HOUR;
        GameTime {
            // SkyMP's year mapping: 2020 was 4E 199
            year: u32::try_from(t.year().saturating_sub(2020).saturating_add(199)).unwrap_or(0),
            month: t.month0(),
            day: t.day(),
            hour: below_24(hour),
            days_passed: narrow(self.settings.start.days_passed + elapsed_ms / MS_PER_DAY),
            time_scale: 1.0,
        }
    }

    /// A player logs in: they hear the clock now.
    pub fn login(&mut self, user: u32, now_ms: i64) -> GameTime {
        self.heard_ms.insert(user, now_ms);
        self.now(now_ms)
    }

    /// Whether a logged-in player is due to hear the clock again: true once
    /// [`RESYNC_MS`] has passed since they last did, or when the wall clock
    /// stepped back past it. A true answer counts as heard.
    pub fn resync_due(&mut self, user: u32, now_ms: i64) -> bool {
        let Some(last) = self.heard_ms.get_mut(&user) else {
            return false;
        };
        if now_ms.saturating_sub(*last) >= RESYNC_MS || now_ms < *last {
            *last = now_ms;
            return true;
        }
        false
    }

    /// A player left.
    pub fn forget(&mut self, user: u32) {
        self.heard_ms.remove(&user);
    }
}

/// `n` days after a date. A year is 365 days from any date, so whole years
/// go at once and at most a year's days are walked.
fn advance(year: u32, month: u32, day: u32, n: u64) -> (u32, u32, u32) {
    let years = n.checked_div(DAYS_IN_YEAR).unwrap_or(0);
    let mut left = n.checked_rem(DAYS_IN_YEAR).unwrap_or(0);
    let (mut y, mut m, mut d) = (year.saturating_add(u32::try_from(years).unwrap_or(u32::MAX)), month, day);
    while left > 0 {
        let in_month = DAYS_IN_MONTH.get(usize::try_from(m).unwrap_or(usize::MAX)).copied().unwrap_or(31);
        let rest = u64::from(in_month.saturating_sub(d));
        if left <= rest {
            d = d.saturating_add(u32::try_from(left).unwrap_or(0));
            left = 0;
        } else {
            left = left.saturating_sub(rest.saturating_add(1));
            d = 1;
            m = m.saturating_add(1);
            if m == 12 {
                m = 0;
                y = y.saturating_add(1);
            }
        }
    }
    (y, m, d)
}

/// Milliseconds as a double: exact below 2^53 ms (285,000 years).
#[allow(clippy::as_conversions)]
fn ms_as_f64(ms: i64) -> f64 {
    ms as f64
}

/// A whole, non-negative double as an integer. `as` saturates (NaN is 0);
/// the callers pass whole numbers of days far below 2^53.
#[allow(clippy::as_conversions)]
fn whole(x: f64) -> u64 {
    x as u64
}

/// Milliseconds, rounded, from a double that is at most a day.
#[allow(clippy::as_conversions)]
fn rounded_ms(x: f64) -> i64 {
    x.round() as i64
}

/// A double as the engine's single precision.
#[allow(clippy::as_conversions)]
fn narrow(x: f64) -> f32 {
    x as f32
}

/// An hour below 24 as a float below 24: single precision rounds the last
/// microseconds of a day up to 24.0, which is no hour.
fn below_24(hour: f64) -> f32 {
    narrow(hour).min(24.0_f32.next_down())
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::arithmetic_side_effects)]
mod tests {
    use super::*;
    use proptest::prelude::*;

    /// 2026-10-03T00:00:00Z, the default epoch.
    const EPOCH: i64 = 1_790_985_600_000;
    const HOUR_MS: i64 = 3_600_000;
    const DAY_MS: i64 = 86_400_000;

    fn clock(json: &str) -> Clock {
        Clock::from_json(json).unwrap()
    }

    fn at(c: &Clock, real_ms_after_epoch: i64) -> GameTime {
        c.now(EPOCH + real_ms_after_epoch)
    }

    #[test]
    fn the_default_epoch_is_the_constant_here() {
        assert_eq!(clock("{}").epoch_ms, EPOCH);
    }

    #[test]
    fn at_the_epoch_it_reads_skyrim_esm_and_holds_before_it() {
        let c = clock("{}");
        let t = at(&c, 0);
        assert_eq!((t.year, t.month, t.day, t.hour, t.days_passed, t.time_scale), (201, 7, 17, 8.0, 1.0, 20.0));
        assert_eq!(at(&c, -DAY_MS), t);
    }

    #[test]
    fn it_runs_at_the_time_scale() {
        let c = clock("{}");
        // three real minutes at 20 is one game hour
        let t = at(&c, 3 * 60_000);
        assert_eq!((t.day, t.hour), (17, 9.0));
        assert!((t.days_passed - (1.0 + 1.0 / 24.0)).abs() < 1e-6);
        // 30 real seconds is 10 game minutes
        let t = at(&c, 30_000);
        assert!((t.hour - (8.0 + 10.0 / 60.0)).abs() < 1e-5);
    }

    #[test]
    fn days_months_and_years_roll() {
        let c = clock("{}");
        // 16 game hours from 08:00 is midnight: the 18th
        let t = at(&c, 16 * 3 * 60_000);
        assert_eq!((t.month, t.day, t.hour), (7, 18, 0.0));
        // Last Seed has 31 days: the 17th plus 15 days is the 1st of Hearthfire
        let t = at(&c, 15 * DAY_MS / 20);
        assert_eq!((t.month, t.day), (8, 1));
        // from the 17th of Last Seed, 4E 201, to the 1st of Morning Star, 4E 202:
        // 14 more days of Last Seed, then 30 + 31 + 30 + 31
        let t = at(&c, (15 + 30 + 31 + 30 + 31) * DAY_MS / 20);
        assert_eq!((t.year, t.month, t.day), (202, 0, 1));
        // a whole Tamriel year later it is the same date again
        let t = at(&c, 365 * DAY_MS / 20);
        assert_eq!((t.year, t.month, t.day, t.hour), (202, 7, 17, 8.0));
        assert!((t.days_passed - 366.0).abs() < 1e-3);
    }

    #[test]
    fn sun_s_dawn_has_28_days() {
        let c = clock(r#"{"start": {"year": 201, "month": 1, "day": 28, "hour": 23.5, "daysPassed": 1}}"#);
        let t = at(&c, HOUR_MS / 20);
        assert_eq!((t.month, t.day, t.hour), (2, 1, 0.5));
    }

    #[test]
    fn the_hour_is_never_24() {
        assert_eq!(below_24(23.999_999_999_9), 24.0_f32.next_down());
        assert!(below_24(23.999_999_999_9) < 24.0);
        assert_eq!(below_24(0.0), 0.0);
    }

    #[test]
    fn real_time_of_day_is_skymp_s_mapping() {
        let c = clock(r#"{"mode": "realTimeOfDay"}"#);
        // 2026-10-03T00:00:00Z plus 13.5 hours: October is month 9, 2026 is 4E 205
        let t = at(&c, 13 * HOUR_MS + HOUR_MS / 2);
        assert_eq!((t.year, t.month, t.day, t.hour, t.time_scale), (205, 9, 3, 13.5, 1.0));
        assert!((t.days_passed - (1.0 + 13.5 / 24.0)).abs() < 1e-6);
        // the offset moves the date too, as SkyMP's did
        let c = clock(r#"{"mode": "realTimeOfDay", "utcOffsetHours": -1}"#);
        let t = at(&c, 0);
        assert_eq!((t.month, t.day, t.hour), (9, 2, 23.0));
    }

    #[test]
    fn bad_settings_name_their_key() {
        for (json, key) in [
            (r#"{"timeScale": 0}"#, "timeScale"),
            (r#"{"timeScale": -20}"#, "timeScale"),
            (r#"{"utcOffsetHours": 25}"#, "utcOffsetHours"),
            (r#"{"start": {"month": 12}}"#, "start.month"),
            (r#"{"start": {"month": 1, "day": 29}}"#, "start.day"),
            (r#"{"start": {"day": 0}}"#, "start.day"),
            (r#"{"start": {"hour": 24}}"#, "start.hour"),
            (r#"{"start": {"daysPassed": -1}}"#, "start.daysPassed"),
            (r#"{"epoch": "yesterday"}"#, "epoch"),
            (r#"{"mode": "sundial"}"#, "unknown variant"),
            (r#"{"timescale": 20}"#, "unknown field"),
        ] {
            let e = Clock::from_json(json).unwrap_err();
            assert!(e.0.contains(key), "{json}: {e}");
        }
    }

    #[test]
    fn players_hear_it_at_login_and_every_minute() {
        let mut c = clock("{}");
        assert!(!c.resync_due(1, EPOCH), "nobody logged in yet");
        let t = c.login(1, EPOCH);
        assert_eq!(t, at(&c, 0));
        assert!(!c.resync_due(1, EPOCH + RESYNC_MS - 1));
        assert!(c.resync_due(1, EPOCH + RESYNC_MS));
        assert!(!c.resync_due(1, EPOCH + RESYNC_MS + 1), "a resync counts as heard");
        assert!(c.resync_due(1, EPOCH), "a wall clock stepped back resyncs");
        c.forget(1);
        assert!(!c.resync_due(1, EPOCH + 10 * RESYNC_MS));
    }

    proptest! {
        #[test]
        fn every_instant_is_a_valid_calendar(after_ms in 0i64..(400 * 365 * DAY_MS), scale in 1u32..1000) {
            let c = clock(&format!(r#"{{"timeScale": {scale}}}"#));
            let t = at(&c, after_ms);
            prop_assert!(t.hour >= 0.0 && t.hour < 24.0);
            prop_assert!(t.month < 12);
            prop_assert!(t.day >= 1 && Some(t.day) <= DAYS_IN_MONTH.get(usize::try_from(t.month).unwrap()).copied());
            prop_assert!(t.days_passed >= 1.0);
        }

        #[test]
        fn time_never_runs_backward(a in 0i64..(40 * 365 * DAY_MS), step in 0i64..DAY_MS) {
            let c = clock("{}");
            let (t0, t1) = (at(&c, a), at(&c, a + step));
            prop_assert!((t1.year, t1.month, t1.day) >= (t0.year, t0.month, t0.day));
            if (t1.year, t1.month, t1.day) == (t0.year, t0.month, t0.day) {
                prop_assert!(t1.hour >= t0.hour);
            }
        }
    }
}
