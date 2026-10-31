//! Spell-placed world objects (thuum docs/verbs/spell-placed-object.md,
//! ADR-057). A spell whose effect script puts an object into the world
//! (Apocalypse's Raise Wall first: a wall MSTT and a halo ACTI ahead of the
//! caster) does so in the caster's game alone, since the script runs
//! nowhere else (ADR-029). The server places the objects itself for every
//! other seat, and every seat receives them through the server-object path;
//! the caster's game skips the server's copies, which name it, while its own
//! cast runs.
//!
//! Shape A (D70, 2026-10-30; shape C's fixed step fell at T3: the engine
//! stands Raise Wall 206 ahead along a heading the record's yaw does not
//! carry): the caster's game reports where its engine put each object, and
//! the server places its copies there. At an accepted cast of a listed spell
//! (casts.rs) the server asks the caster for the entry's bases (`Request`,
//! one per set, open REPORT_WINDOW_MS); the caster's game finds each base's
//! new reference and answers once per base when it has settled
//! (`report_placed`); the server judges the report (the set the caster's own
//! and still open, the base the entry's and not yet reported, the pose a
//! number standing REPORT_MIN_UNITS to REPORT_MAX_UNITS from where the
//! caster stood at the cast on the ground plane and within REPORT_MAX_DZ of
//! its height) and places that one object at the reported pose. No report
//! within the window places nothing: the engine took the no-room failure
//! effect, or the caster's game saw no reference.
//!
//! The table is `spellPlacedObjects` in server-settings.json: per spell an
//! anchor (`caster` in v1: the band is measured from the caster; `hit-target`
//! and `impact` are the corpus's other two families and are refused until
//! their rules land), the objects (base and type, MSTT or ACTI; the pose is
//! the engine's, so the table carries no offsets since shape A), a lifetime
//! (`whileHeld`: the set stands while the stream holds, `lingerS` after its
//! end, `maxS` from the cast start against a stream that never ends) and
//! `perCaster`, the sets one caster may hold at once; MAX_SETS bounds the
//! world. The caps are judged at the cast, before any request goes out.
//!
//! The times are the server's monotonic milliseconds (PartOne's
//! SteadyNowMs), as casts.rs takes them.

use crate::clock::SettingsError;
use crate::forms::FormDesc;
use serde::Deserialize;

/// How long after an accepted cast the caster's reports are taken,
/// milliseconds: the script's wall rises from 240 below at 512 units a
/// second (docs/MODS.md), the caster's game reports a settled pose, and a
/// round trip each way with room for a slow frame.
pub const REPORT_WINDOW_MS: u64 = 3_000;
/// The nearest a reported object may stand to where the caster stood at the
/// cast, on the ground plane, units: the engine stands Raise Wall's wall at
/// 206.25 and its halo at 196.875 whatever the pitch (x-raise-wall-pitch v1,
/// 2026-10-30), with the caster's own motion inside the cast's window.
pub const REPORT_MIN_UNITS: f32 = 150.0;
/// The farthest, the same read.
pub const REPORT_MAX_UNITS: f32 = 260.0;
/// The most a reported object may stand above or below where the caster
/// stood, units: a wall on the slope ahead.
pub const REPORT_MAX_DZ: f32 = 256.0;
/// Sets the server holds at once, world-wide, requests awaiting their
/// reports included: a stream held on every seat of a full server twice
/// over.
pub const MAX_SETS: usize = 64;
/// The most objects one entry may name: Raise Wall's two, with room for a
/// cage's walls.
pub const MAX_OBJECTS: usize = 8;

/// What a set is measured from (docs/research/apocalypse/placed-objects.md's
/// three families).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Anchor {
    /// The caster's position at the accepted cast (Raise Wall).
    Caster,
    /// The victim of the accepted hit (Tharn's Prison); no rule yet.
    HitTarget,
    /// The explosion point of a target-location cast (Summoning Rune); no
    /// rule yet.
    Impact,
}

/// The record type of a placed base: the two kinds the client's spawn path
/// places as plain references with their meshes' collision.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BaseKind {
    /// A movable static (the wall).
    Mstt,
    /// An activator (the halo).
    Acti,
}

impl BaseKind {
    /// The record type as the wire's `baseRecordType` carries it.
    #[must_use]
    pub fn record_type(self) -> &'static str {
        match self {
            Self::Mstt => "MSTT",
            Self::Acti => "ACTI",
        }
    }
}

/// One object an entry places.
#[derive(Debug, Clone, PartialEq)]
pub struct PlacedObject {
    /// The base, as the settings write a form.
    pub base: FormDesc,
    /// Its record type.
    pub kind: BaseKind,
}

/// How long a set stands.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Lifetime {
    /// The set stands while the concentration stream holds; its end starts
    /// the linger. False: the linger starts at the first placement.
    pub while_held: bool,
    /// How long the set stands after its end begins, milliseconds.
    pub linger_ms: u64,
    /// The longest a set stands from its cast's start, milliseconds, against
    /// a stream that never ends.
    pub max_ms: u64,
}

/// One listed spell.
#[derive(Debug, Clone, PartialEq)]
pub struct Entry {
    /// The spell, as the settings write a form.
    pub spell: FormDesc,
    /// What its band is measured from.
    pub anchor: Anchor,
    /// The objects, in the settings' order.
    pub objects: Vec<PlacedObject>,
    /// How long a set stands.
    pub lifetime: Lifetime,
    /// Sets one caster may hold at once.
    pub per_caster: usize,
}

/// Where the caster stood and faced at the accepted cast: the server's
/// recorded position and its Z angle in degrees.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CasterPose {
    /// Position.
    pub x: f32,
    /// Position.
    pub y: f32,
    /// Position.
    pub z: f32,
    /// The Z angle, degrees, as the engine and the change form hold it.
    pub yaw: f32,
}

/// One object's pose, ready for the server to create it.
#[derive(Debug, Clone, PartialEq)]
pub struct ObjectPose {
    /// The base.
    pub base: FormDesc,
    /// Its record type.
    pub kind: BaseKind,
    /// Position.
    pub x: f32,
    /// Position.
    pub y: f32,
    /// Position.
    pub z: f32,
    /// The Z angle, degrees.
    pub yaw: f32,
}

/// Objects to create now, one set's.
#[derive(Debug, Clone, PartialEq)]
pub struct Placement {
    /// The set's id, which names it to [`Placements::due_removals`].
    pub set: u64,
    /// The caster, whose id the objects carry on the wire (`placed_by`).
    pub caster: u32,
    /// The spell.
    pub spell: FormDesc,
    /// The objects, posed as reported.
    pub objects: Vec<ObjectPose>,
}

/// What the server asks the caster's game at an accepted cast: the set's
/// id and the bases whose poses it wants, within the window.
#[derive(Debug, Clone, PartialEq)]
pub struct Request {
    /// The set's id, which the reports name.
    pub set: u64,
    /// The caster asked.
    pub caster: u32,
    /// The spell.
    pub spell: FormDesc,
    /// The bases, in the entry's order.
    pub objects: Vec<PlacedObject>,
    /// How long the reports are taken from the cast, milliseconds.
    pub window_ms: u64,
}

/// A caster's answer for one base of a set: where its engine stands it.
#[derive(Debug, Clone, PartialEq)]
pub struct Report {
    /// The caster answering.
    pub caster: u32,
    /// The set named by the request.
    pub set: u64,
    /// The base, as the server reads a form id.
    pub base: FormDesc,
    /// Position.
    pub x: f32,
    /// Position.
    pub y: f32,
    /// Position.
    pub z: f32,
    /// The Z angle, degrees.
    pub yaw: f32,
}

/// Why a report placed nothing.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum ReportRefusal {
    /// No request of this caster's names the set (another's, dropped by a
    /// newer cast in the hand, or complete).
    NoSet,
    /// The window closed, or the set's linger ran out before the report.
    Late,
    /// The base is not one the entry names.
    Base,
    /// The base was reported for this set already.
    Repeated,
    /// A coordinate is not a number.
    NonFinite,
    /// The pose stands outside the band from where the caster stood.
    Range {
        /// Its distance on the ground plane.
        distance: f32,
        /// Its height above or below the caster.
        dz: f32,
    },
}

/// Why a set's time ran out.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RemovalWhy {
    /// The linger after its end (the cast's end, the caster's leaving, or
    /// the placement itself when the entry does not hold).
    Lingered,
    /// Its entry's maxS from the cast's start.
    MaxReached,
}

/// A set to remove now.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Removal {
    /// The set's id.
    pub set: u64,
    /// Its caster.
    pub caster: u32,
    /// Why.
    pub why: RemovalWhy,
}

/// Why a cast placed nothing.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Refusal {
    /// The caster holds its entry's `perCaster` sets already.
    PerCaster,
    /// The world holds MAX_SETS.
    ServerWide,
    /// The caster's game reported no base within the window.
    NoReport,
}

/// A cast that placed nothing, for the server's log.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Refused {
    /// The caster.
    pub caster: u32,
    /// The spell.
    pub spell: FormDesc,
    /// Why.
    pub why: Refusal,
}

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct EntryJson {
    spell: String,
    anchor: String,
    objects: Vec<ObjectJson>,
    lifetime: LifetimeJson,
    per_caster: usize,
}

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct ObjectJson {
    base: String,
    #[serde(rename = "type")]
    kind: String,
}

#[derive(Debug, Clone, PartialEq, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct LifetimeJson {
    while_held: bool,
    linger_s: u64,
    max_s: u64,
}

/// A cast as the server accepted it, for [`Placements::cast_started`].
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct CastStart {
    /// The caster's server id.
    pub caster: u32,
    /// The hand or slot: 0 left, 1 right, 2 voice, 3 instant.
    pub hand: u8,
    /// Where the caster stood and faced.
    pub pose: CasterPose,
    /// A stream opened (its end comes through [`Placements::cast_ended`]);
    /// false, the cast was released whole.
    pub concentration: bool,
}

/// A request out, awaiting the caster's reports inside the window.
#[derive(Debug, Clone, PartialEq)]
struct Pending {
    set: u64,
    caster: u32,
    hand: u8,
    entry: usize,
    pose: CasterPose,
    started_ms: u64,
    concentration: bool,
    /// When the stream ended, if it has.
    ended_ms: Option<u64>,
    /// Which of the entry's objects came in, in the entry's order.
    reported: Vec<bool>,
}

/// A set standing in the world.
#[derive(Debug, Clone, PartialEq)]
struct Set {
    id: u64,
    caster: u32,
    hand: u8,
    entry: usize,
    started_ms: u64,
    /// When its linger runs out; none while a held stream still holds it.
    end_ms: Option<u64>,
}

/// The table, the requests out and every set standing.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Placements {
    entries: Vec<Entry>,
    pending: Vec<Pending>,
    sets: Vec<Set>,
    refusals: Vec<Refused>,
    requests: Vec<Request>,
    placed: Vec<Placement>,
    next_set: u64,
}

fn form(what: &str, text: &str) -> Result<FormDesc, SettingsError> {
    FormDesc::parse(text).map_err(|why| SettingsError(format!("spellPlacedObjects: {what}: {why}")))
}

fn seconds_ms(spell: &str, what: &str, seconds: u64) -> Result<u64, SettingsError> {
    seconds
        .checked_mul(1000)
        .ok_or_else(|| SettingsError(format!("spellPlacedObjects: {spell:?} has a {what} too long to hold in milliseconds")))
}

fn parse_entry(e: EntryJson) -> Result<Entry, SettingsError> {
    let spell = form("spell", &e.spell)?;
    let anchor = match e.anchor.as_str() {
        "caster" => Anchor::Caster,
        // the corpus's other two families (docs/research/apocalypse/
        // placed-objects.md) take this table with rules of their own, not
        // written yet; naming one is a mistake the operator should hear
        "hit-target" | "impact" => {
            return Err(SettingsError(format!(
                "spellPlacedObjects: {:?} names the anchor {:?}, which v1 does not place (caster alone)",
                e.spell, e.anchor
            )));
        }
        other => {
            return Err(SettingsError(format!("spellPlacedObjects: {:?} names an unknown anchor {other:?}", e.spell)));
        }
    };
    if e.objects.is_empty() || e.objects.len() > MAX_OBJECTS {
        return Err(SettingsError(format!("spellPlacedObjects: {:?} names {} objects, not 1 to {MAX_OBJECTS}", e.spell, e.objects.len())));
    }
    let mut objects = Vec::with_capacity(e.objects.len());
    for o in e.objects {
        let kind = match o.kind.as_str() {
            "MSTT" => BaseKind::Mstt,
            "ACTI" => BaseKind::Acti,
            other => {
                return Err(SettingsError(format!("spellPlacedObjects: {:?} names an object of type {other:?}, not MSTT or ACTI", e.spell)));
            }
        };
        let base = form("base", &o.base)?;
        if objects.iter().any(|p: &PlacedObject| p.base.is(base.short_id, &base.file)) {
            return Err(SettingsError(format!("spellPlacedObjects: {:?} names the base {:?} twice", e.spell, o.base)));
        }
        objects.push(PlacedObject { base, kind });
    }
    if e.lifetime.max_s == 0 {
        return Err(SettingsError(format!("spellPlacedObjects: {:?} has a maxS of 0", e.spell)));
    }
    let lifetime = Lifetime {
        while_held: e.lifetime.while_held,
        linger_ms: seconds_ms(&e.spell, "lingerS", e.lifetime.linger_s)?,
        max_ms: seconds_ms(&e.spell, "maxS", e.lifetime.max_s)?,
    };
    if e.per_caster == 0 || e.per_caster > MAX_SETS {
        return Err(SettingsError(format!("spellPlacedObjects: {:?} has a perCaster of {}, not 1 to {MAX_SETS}", e.spell, e.per_caster)));
    }
    Ok(Entry { spell, anchor, objects, lifetime, per_caster: e.per_caster })
}

fn finite(pose: &CasterPose) -> bool {
    pose.x.is_finite() && pose.y.is_finite() && pose.z.is_finite() && pose.yaw.is_finite()
}

/// What a report needs of its pending request, copied out so the table can
/// change under it.
struct Judged {
    index: usize,
    object: usize,
    spell: FormDesc,
    base: FormDesc,
    kind: BaseKind,
    hand: u8,
    entry: usize,
    started_ms: u64,
    end_ms: Option<u64>,
}

impl Placements {
    /// The table from the `spellPlacedObjects` block's JSON text (`[]` for
    /// none). The error names a bad entry: a form that is not "<hex
    /// id>:<plugin>", an anchor other than `caster`, no objects or more than
    /// MAX_OBJECTS, a type other than MSTT or ACTI, a base named twice, a
    /// maxS of 0, a perCaster outside 1 to MAX_SETS, an unknown key (shape
    /// C's `forward` and `dz` among them), a spell listed twice.
    pub fn from_json(text: &str) -> Result<Self, SettingsError> {
        let entries: Vec<EntryJson> = serde_json::from_str(text).map_err(|e| SettingsError(format!("spellPlacedObjects: {e}")))?;
        let mut parsed: Vec<Entry> = Vec::with_capacity(entries.len());
        for e in entries {
            let entry = parse_entry(e)?;
            if parsed.iter().any(|p| p.spell.is(entry.spell.short_id, &entry.spell.file)) {
                return Err(SettingsError(format!("spellPlacedObjects: {:?} is listed twice", entry.spell)));
            }
            parsed.push(entry);
        }
        Ok(Self { entries: parsed, ..Self::default() })
    }

    /// The listed spells, in the settings' order.
    #[must_use]
    pub fn entries(&self) -> &[Entry] {
        &self.entries
    }

    fn entry_of(&self, short_id: u32, file: &str) -> Option<usize> {
        self.entries.iter().position(|e| e.spell.is(short_id, file))
    }

    /// Whether the spell `short_id` in `file` places objects.
    #[must_use]
    pub fn is_listed(&self, short_id: u32, file: &str) -> bool {
        self.entry_of(short_id, file).is_some()
    }

    /// Requests whose set does not stand yet (no report in): what counts
    /// toward the caps beside the standing sets, each set once.
    fn unanswered(&self) -> impl Iterator<Item = &Pending> {
        self.pending.iter().filter(|p| !self.sets.iter().any(|s| s.id == p.set))
    }

    /// Sets `caster` holds, standing or awaiting their first report.
    fn held_by(&self, caster: u32) -> usize {
        self.sets_of(caster).saturating_add(self.unanswered().filter(|p| p.caster == caster).count())
    }

    /// The server accepted `cast` of the spell `short_id` in `file` at
    /// `now_ms`. A listed spell's cast asks the caster for the entry's
    /// bases (the hand's earlier request, if any, is dropped, as a new
    /// stream ends the hand's old one) unless the caster holds its
    /// `perCaster` sets or the world holds MAX_SETS, which is noted for
    /// [`Self::take_refusals`]; a pose that is not a number asks nothing.
    /// Returns whether the spell is listed.
    pub fn cast_started(&mut self, short_id: u32, file: &str, cast: CastStart, now_ms: u64) -> bool {
        let Some(entry) = self.entry_of(short_id, file) else {
            return false;
        };
        let CastStart { caster, hand, pose, concentration } = cast;
        // the hand's earlier stream is over, as casts.rs takes it: a set it
        // held begins its linger, a request it had is dropped
        self.end_hand(caster, hand, now_ms, true);
        if !finite(&pose) {
            return true;
        }
        let Some(listed) = self.entries.get(entry) else {
            return true;
        };
        let refusal = if self.sets.len().saturating_add(self.unanswered().count()) >= MAX_SETS {
            Some(Refusal::ServerWide)
        } else if self.held_by(caster) >= listed.per_caster {
            Some(Refusal::PerCaster)
        } else {
            None
        };
        if let Some(why) = refusal {
            self.refusals.push(Refused { caster, spell: listed.spell.clone(), why });
            return true;
        }
        let set = self.next_set;
        self.next_set = self.next_set.saturating_add(1);
        self.pending.push(Pending {
            set,
            caster,
            hand,
            entry,
            pose,
            started_ms: now_ms,
            concentration,
            ended_ms: None,
            reported: vec![false; listed.objects.len()],
        });
        self.requests.push(Request { set, caster, spell: listed.spell.clone(), objects: listed.objects.clone(), window_ms: REPORT_WINDOW_MS });
        true
    }

    /// The requests to send since the last call, each once.
    pub fn take_requests(&mut self) -> Vec<Request> {
        std::mem::take(&mut self.requests)
    }

    fn judge(&self, report: &Report, now_ms: u64) -> Result<Judged, ReportRefusal> {
        let Report { caster, set, base, x, y, z, yaw } = report;
        let Some(index) = self.pending.iter().position(|p| p.set == *set && p.caster == *caster) else {
            return Err(ReportRefusal::NoSet);
        };
        let (Some(pending), Some(entry)) = (self.pending.get(index), self.pending.get(index).and_then(|p| self.entries.get(p.entry))) else {
            return Err(ReportRefusal::NoSet);
        };
        if now_ms.saturating_sub(pending.started_ms) > REPORT_WINDOW_MS || now_ms.saturating_sub(pending.started_ms) >= entry.lifetime.max_ms {
            return Err(ReportRefusal::Late);
        }
        if !(x.is_finite() && y.is_finite() && z.is_finite() && yaw.is_finite()) {
            return Err(ReportRefusal::NonFinite);
        }
        let Some(object) = entry.objects.iter().position(|o| o.base.is(base.short_id, &base.file)) else {
            return Err(ReportRefusal::Base);
        };
        if pending.reported.get(object).copied().unwrap_or(true) {
            return Err(ReportRefusal::Repeated);
        }
        let distance = (x - pending.pose.x).hypot(y - pending.pose.y);
        let dz = (z - pending.pose.z).abs();
        if !(REPORT_MIN_UNITS..=REPORT_MAX_UNITS).contains(&distance) || dz > REPORT_MAX_DZ {
            return Err(ReportRefusal::Range { distance, dz });
        }
        // the set's end, when it does not hold: from the stream's end if it
        // came already, else from this first placement
        let holds = pending.concentration && entry.lifetime.while_held && pending.ended_ms.is_none();
        let end_ms = if holds { None } else { Some(pending.ended_ms.unwrap_or(now_ms).saturating_add(entry.lifetime.linger_ms)) };
        if end_ms.is_some_and(|end| end <= now_ms) {
            return Err(ReportRefusal::Late);
        }
        let Some(named) = entry.objects.get(object) else {
            return Err(ReportRefusal::Base);
        };
        Ok(Judged {
            index,
            object,
            spell: entry.spell.clone(),
            base: named.base.clone(),
            kind: named.kind,
            hand: pending.hand,
            entry: pending.entry,
            started_ms: pending.started_ms,
            end_ms,
        })
    }

    /// `report` arrived at `now_ms`: the caster's game stands one of a
    /// requested set's bases at the reported pose. Judged as the module doc
    /// says; accepted, the object is queued for [`Self::due_placements`] and
    /// the set stands (its first report creates it); when every base of the
    /// set is in, the request is complete.
    pub fn report_placed(&mut self, report: Report, now_ms: u64) -> Result<(), ReportRefusal> {
        let judged = self.judge(&report, now_ms)?;
        let Report { caster, set, x, y, z, yaw, .. } = report;
        if !self.sets.iter().any(|s| s.id == set) {
            self.sets.push(Set { id: set, caster, hand: judged.hand, entry: judged.entry, started_ms: judged.started_ms, end_ms: judged.end_ms });
        }
        let complete = match self.pending.get_mut(judged.index) {
            Some(pending) => {
                if let Some(flag) = pending.reported.get_mut(judged.object) {
                    *flag = true;
                }
                pending.reported.iter().all(|r| *r)
            }
            None => false,
        };
        if complete {
            self.pending.retain(|p| p.set != set);
        }
        self.placed.push(Placement {
            set,
            caster,
            spell: judged.spell,
            objects: vec![ObjectPose { base: judged.base, kind: judged.kind, x, y, z, yaw }],
        });
        Ok(())
    }

    /// `caster`'s stream in `hand` ended at `now_ms`: a held set's linger
    /// begins; a request still open keeps taking reports inside its window
    /// (the engine's objects stood while the stream did), each placing a set
    /// that lingers from this end.
    pub fn cast_ended(&mut self, caster: u32, hand: u8, now_ms: u64) {
        self.end_hand(caster, hand, now_ms, false);
    }

    fn end_hand(&mut self, caster: u32, hand: u8, now_ms: u64, drop_requests: bool) {
        if drop_requests {
            self.pending.retain(|p| !(p.caster == caster && p.hand == hand));
        } else {
            for p in self.pending.iter_mut().filter(|p| p.caster == caster && p.hand == hand && p.ended_ms.is_none()) {
                p.ended_ms = Some(now_ms);
            }
        }
        for set in self.sets.iter_mut().filter(|s| s.caster == caster && s.hand == hand && s.end_ms.is_none()) {
            let linger = self.entries.get(set.entry).map_or(0, |e| e.lifetime.linger_ms);
            set.end_ms = Some(now_ms.saturating_add(linger));
        }
    }

    /// `caster` is gone (disconnected) at `now_ms`: every hand's stream ends
    /// as in [`Self::cast_ended`], and its requests are dropped (no report
    /// will come).
    pub fn caster_gone(&mut self, caster: u32, now_ms: u64) {
        for hand in 0..4u8 {
            self.end_hand(caster, hand, now_ms, true);
        }
    }

    /// The objects to create at `now_ms`: every report accepted since the
    /// last call, grouped as it came (one object a report). A request whose
    /// window closed is dropped here; one that drew no report at all is
    /// noted for [`Self::take_refusals`].
    pub fn due_placements(&mut self, now_ms: u64) -> Vec<Placement> {
        let entries = &self.entries;
        let refusals = &mut self.refusals;
        self.pending.retain(|p| {
            if now_ms.saturating_sub(p.started_ms) <= REPORT_WINDOW_MS {
                return true;
            }
            if !p.reported.iter().any(|r| *r) {
                if let Some(e) = entries.get(p.entry) {
                    refusals.push(Refused { caster: p.caster, spell: e.spell.clone(), why: Refusal::NoReport });
                }
            }
            false
        });
        std::mem::take(&mut self.placed)
    }

    /// The sets whose time ran out by `now_ms`, each once: the linger after
    /// the set's end, or the entry's maxS from the cast's start, whichever
    /// comes first.
    pub fn due_removals(&mut self, now_ms: u64) -> Vec<Removal> {
        let entries = &self.entries;
        let why_of = |s: &Set| -> Option<RemovalWhy> {
            let max_ms = entries.get(s.entry).map_or(0, |e| e.lifetime.max_ms);
            if now_ms.saturating_sub(s.started_ms) >= max_ms {
                Some(RemovalWhy::MaxReached)
            } else if s.end_ms.is_some_and(|end| now_ms >= end) {
                Some(RemovalWhy::Lingered)
            } else {
                None
            }
        };
        let (due, standing): (Vec<Set>, Vec<Set>) = self.sets.drain(..).partition(|s| why_of(s).is_some());
        self.sets = standing;
        due.iter()
            .filter_map(|s| why_of(s).map(|why| Removal { set: s.id, caster: s.caster, why }))
            .collect()
    }

    /// The casts that placed nothing since the last call, for the log.
    pub fn take_refusals(&mut self) -> Vec<Refused> {
        std::mem::take(&mut self.refusals)
    }

    /// Sets standing in the world.
    #[must_use]
    pub fn live_sets(&self) -> usize {
        self.sets.len()
    }

    /// Sets `caster` holds standing.
    #[must_use]
    pub fn sets_of(&self, caster: u32) -> usize {
        self.sets.iter().filter(|s| s.caster == caster).count()
    }

    /// Requests out, awaiting their reports.
    #[must_use]
    pub fn waiting(&self) -> usize {
        self.pending.len()
    }
}

#[cfg(test)]
#[allow(clippy::arithmetic_side_effects)]
mod tests {
    use super::*;

    const PLUGIN: &str = "Apocalypse - Magic of Skyrim.esp";
    const RAISE_WALL: u32 = 0xcc14;
    const WALL: u32 = 0x98529;
    const HALO: u32 = 0x735e8;
    const CASTER: u32 = 0xff00_0000;

    // the Raise Wall entry as both deploy settings files carry it
    fn raise_wall(per_caster: usize, while_held: bool, linger_s: u64, max_s: u64) -> String {
        format!(
            r#"[{{"spell": "00cc14:{PLUGIN}", "anchor": "caster",
                 "objects": [{{"base": "098529:{PLUGIN}", "type": "MSTT"}},
                             {{"base": "0735e8:{PLUGIN}", "type": "ACTI"}}],
                 "lifetime": {{"whileHeld": {while_held}, "lingerS": {linger_s}, "maxS": {max_s}}},
                 "perCaster": {per_caster}}}]"#
        )
    }

    fn table() -> Placements {
        Placements::from_json(&raise_wall(2, true, 5, 120)).unwrap_or_default()
    }

    fn standing(x: f32, y: f32, z: f32, yaw: f32) -> CasterPose {
        CasterPose { x, y, z, yaw }
    }

    fn near(a: f32, b: f32) -> bool {
        (a - b).abs() < 0.01
    }

    fn cast(caster: u32, hand: u8, pose: CasterPose, concentration: bool) -> CastStart {
        CastStart { caster, hand, pose, concentration }
    }

    fn desc(short_id: u32) -> FormDesc {
        FormDesc { short_id, file: PLUGIN.to_owned() }
    }

    // the engine's pose for a caster at the origin facing east: the wall
    // 206.25 along x, 48 below, the halo 196.875 along x at its height
    fn report(caster: u32, set: u64, base: u32, x: f32, y: f32, z: f32, yaw: f32) -> Report {
        Report { caster, set, base: desc(base), x, y, z, yaw }
    }

    fn wall_report(caster: u32, set: u64) -> Report {
        report(caster, set, WALL, 206.25, 0.0, -48.0, 90.0)
    }

    fn halo_report(caster: u32, set: u64) -> Report {
        report(caster, set, HALO, 196.875, 0.0, 0.0, 90.0)
    }

    // a cast at `at` whose wall and halo come in `after` ms later: one
    // standing set
    fn stand(t: &mut Placements, caster: u32, hand: u8, at: u64, after: u64) -> u64 {
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(caster, hand, standing(0.0, 0.0, 0.0, 90.0), true), at));
        let requests = t.take_requests();
        let set = requests.first().map(|r| r.set);
        assert!(set.is_some(), "{requests:?}");
        let set = set.unwrap_or_default();
        assert_eq!(t.report_placed(wall_report(caster, set), at + after), Ok(()));
        assert_eq!(t.report_placed(halo_report(caster, set), at + after), Ok(()));
        assert_eq!(t.due_placements(at + after).len(), 2);
        set
    }

    #[test]
    fn the_table_parses_the_raise_wall_entry() {
        let t = table();
        assert_eq!(t.entries().len(), 1);
        assert!(t.is_listed(RAISE_WALL, PLUGIN));
        assert!(t.is_listed(RAISE_WALL, "apocalypse - magic of skyrim.esp"));
        assert!(!t.is_listed(RAISE_WALL, "Skyrim.esm"));
        assert!(!t.is_listed(0xcc15, PLUGIN));
        let e = t.entries().first();
        assert!(e.is_some_and(|e| e.anchor == Anchor::Caster && e.per_caster == 2), "{e:?}");
        assert!(e.is_some_and(|e| e.lifetime == Lifetime { while_held: true, linger_ms: 5_000, max_ms: 120_000 }), "{e:?}");
        assert!(e.is_some_and(|e| e.objects.len() == 2), "{e:?}");
        let wall = e.and_then(|e| e.objects.first());
        assert!(wall.is_some_and(|o| o.base.is(WALL, PLUGIN) && o.kind == BaseKind::Mstt), "{wall:?}");
        let halo = e.and_then(|e| e.objects.get(1));
        assert!(halo.is_some_and(|o| o.base.is(HALO, PLUGIN) && o.kind == BaseKind::Acti), "{halo:?}");
        assert_eq!(BaseKind::Mstt.record_type(), "MSTT");
        assert_eq!(BaseKind::Acti.record_type(), "ACTI");
        assert!(Placements::from_json("[]").is_ok_and(|t| t.entries().is_empty()));
    }

    #[test]
    fn bad_entries_are_refused_by_name() {
        let good = raise_wall(2, true, 5, 120);
        for (bad, what) in [
            (good.replace(r#""anchor": "caster""#, r#""anchor": "hit-target""#), "hit-target"),
            (good.replace(r#""anchor": "caster""#, r#""anchor": "impact""#), "impact"),
            (good.replace(r#""anchor": "caster""#, r#""anchor": "sky""#), "unknown anchor"),
            (good.replace(r#""type": "MSTT""#, r#""type": "DOOR""#), "DOOR"),
            (good.replace(r#""type": "MSTT""#, r#""type": "MSTT", "forward": 192, "dz": -48"#), "shape C's offsets"),
            (good.replace("0735e8:", "098529:"), "a base twice"),
            (raise_wall(0, true, 5, 120), "perCaster 0"),
            (raise_wall(65, true, 5, 120), "perCaster over MAX_SETS"),
            (raise_wall(2, true, 5, 0), "maxS 0"),
            (good.replace(r#""perCaster""#, r#""perHand""#), "unknown key"),
            (good.replace("00cc14:", "cc14"), "spell without a plugin"),
            (good.replace("098529:", "zz:"), "base without a hex id"),
            (
                format!(r#"[{{"spell": "00cc14:{PLUGIN}", "anchor": "caster", "objects": [], "lifetime": {{"whileHeld": true, "lingerS": 5, "maxS": 120}}, "perCaster": 2}}]"#),
                "no objects",
            ),
        ] {
            assert!(Placements::from_json(&bad).is_err(), "{what}: {bad}");
        }
        let twice = format!("[{}, {}]", good.trim_start_matches('[').trim_end_matches(']'), good.trim_start_matches('[').trim_end_matches(']'));
        assert!(Placements::from_json(&twice).is_err());
    }

    #[test]
    fn a_listed_cast_asks_the_caster_and_each_report_places_its_base_where_reported() {
        let mut t = table();
        assert!(t.take_requests().is_empty());
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(CASTER, 1, standing(1000.0, -2000.0, 300.0, 90.0), true), 10_000));
        assert_eq!(t.waiting(), 1);
        let requests = t.take_requests();
        assert_eq!(requests.len(), 1);
        let r = requests.first();
        assert!(r.is_some_and(|r| r.set == 0 && r.caster == CASTER && r.spell.is(RAISE_WALL, PLUGIN) && r.window_ms == REPORT_WINDOW_MS), "{r:?}");
        assert!(r.is_some_and(|r| r.objects.len() == 2 && r.objects.first().is_some_and(|o| o.base.is(WALL, PLUGIN) && o.kind == BaseKind::Mstt)), "{r:?}");
        assert!(t.take_requests().is_empty());
        // nothing stands before a report
        assert!(t.due_placements(10_500).is_empty());
        assert_eq!((t.live_sets(), t.waiting()), (0, 1));
        // the wall, where the engine put it
        assert_eq!(t.report_placed(report(CASTER, 0, WALL, 1000.0 + 206.25, -2000.0, 252.0, 97.0), 10_600), Ok(()));
        let placed = t.due_placements(10_600);
        assert_eq!(placed.len(), 1);
        let p = placed.first();
        assert!(p.is_some_and(|p| p.set == 0 && p.caster == CASTER && p.spell.is(RAISE_WALL, PLUGIN) && p.objects.len() == 1), "{p:?}");
        let wall = p.and_then(|p| p.objects.first());
        assert!(wall.is_some_and(|o| o.base.is(WALL, PLUGIN) && o.kind == BaseKind::Mstt && near(o.x, 1206.25) && near(o.y, -2000.0) && near(o.z, 252.0) && near(o.yaw, 97.0)), "{wall:?}");
        assert_eq!((t.live_sets(), t.sets_of(CASTER), t.waiting()), (1, 1, 1));
        // the halo, later, into the same set; the request is then complete
        assert_eq!(t.report_placed(report(CASTER, 0, HALO, 1000.0, -2000.0 - 196.875, 300.0, 97.0), 11_000), Ok(()));
        let placed = t.due_placements(11_000);
        assert_eq!(placed.len(), 1);
        let halo = placed.first().and_then(|p| p.objects.first());
        assert!(halo.is_some_and(|o| o.base.is(HALO, PLUGIN) && o.kind == BaseKind::Acti && near(o.y, -2196.875) && near(o.z, 300.0)), "{halo:?}");
        assert_eq!((t.live_sets(), t.sets_of(CASTER), t.waiting()), (1, 1, 0));
        // placed once
        assert!(t.due_placements(20_000).is_empty());
        assert!(t.take_refusals().is_empty());
        // an unlisted spell is nobody's business here
        assert!(!t.cast_started(0x12fcd, "Skyrim.esm", cast(CASTER, 0, standing(0.0, 0.0, 0.0, 0.0), true), 40_000));
        assert!(t.take_requests().is_empty());
        // a pose that is not a number asks nothing
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(CASTER, 0, standing(f32::NAN, 0.0, 0.0, 0.0), true), 60_000));
        assert!(t.take_requests().is_empty());
        assert_eq!(t.waiting(), 0);
    }

    #[test]
    fn reports_outside_the_rule_are_refused_by_name() {
        let mut t = table();
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(CASTER, 1, standing(0.0, 0.0, 0.0, 90.0), true), 1_000));
        assert_eq!(t.take_requests().len(), 1);
        // not this caster's set, no such set
        assert_eq!(t.report_placed(wall_report(CASTER + 1, 0), 1_500), Err(ReportRefusal::NoSet));
        assert_eq!(t.report_placed(wall_report(CASTER, 7), 1_500), Err(ReportRefusal::NoSet));
        // a base the entry does not name
        assert_eq!(t.report_placed(report(CASTER, 0, 0x1234, 200.0, 0.0, 0.0, 90.0), 1_500), Err(ReportRefusal::Base));
        assert_eq!(t.report_placed(Report { base: FormDesc { short_id: WALL, file: "Skyrim.esm".to_owned() }, ..wall_report(CASTER, 0) }, 1_500), Err(ReportRefusal::Base));
        // not a number
        assert_eq!(t.report_placed(report(CASTER, 0, WALL, f32::NAN, 0.0, 0.0, 90.0), 1_500), Err(ReportRefusal::NonFinite));
        assert_eq!(t.report_placed(report(CASTER, 0, WALL, 200.0, 0.0, 0.0, f32::INFINITY), 1_500), Err(ReportRefusal::NonFinite));
        // outside the band: too near, too far, too high; the distance on the ground plane
        assert!(matches!(t.report_placed(report(CASTER, 0, WALL, 149.0, 0.0, 0.0, 90.0), 1_500), Err(ReportRefusal::Range { distance, .. }) if near(distance, 149.0)));
        assert!(matches!(t.report_placed(report(CASTER, 0, WALL, 0.0, 261.0, 0.0, 90.0), 1_500), Err(ReportRefusal::Range { distance, .. }) if near(distance, 261.0)));
        assert!(matches!(t.report_placed(report(CASTER, 0, WALL, 200.0, 0.0, 300.0, 90.0), 1_500), Err(ReportRefusal::Range { dz, .. }) if near(dz, 300.0)));
        assert!(matches!(t.report_placed(report(CASTER, 0, WALL, 0.0, 0.0, 200.0, 90.0), 1_500), Err(ReportRefusal::Range { distance, .. }) if near(distance, 0.0)));
        // the band's edges stand
        assert_eq!(t.report_placed(report(CASTER, 0, WALL, 150.0, 0.0, -256.0, 90.0), 1_500), Ok(()));
        assert_eq!(t.report_placed(report(CASTER, 0, HALO, 0.0, -260.0, 0.0, 90.0), 1_500), Ok(()));
        assert_eq!(t.due_placements(1_500).len(), 2);
        // the same base again: the request is complete, so no set of the caster's is open
        assert_eq!(t.report_placed(wall_report(CASTER, 0), 1_600), Err(ReportRefusal::NoSet));
        // a base twice inside an open request
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(CASTER, 0, standing(0.0, 0.0, 0.0, 90.0), true), 2_000));
        assert_eq!(t.take_requests().first().map(|r| r.set), Some(1));
        assert_eq!(t.report_placed(wall_report(CASTER, 1), 2_500), Ok(()));
        assert_eq!(t.report_placed(wall_report(CASTER, 1), 2_600), Err(ReportRefusal::Repeated));
        assert_eq!(t.due_placements(2_600).len(), 1);
        // past the window: late, and the request is dropped at the next tick
        assert_eq!(t.report_placed(halo_report(CASTER, 1), 2_000 + REPORT_WINDOW_MS + 1), Err(ReportRefusal::Late));
        assert!(t.due_placements(2_000 + REPORT_WINDOW_MS + 1).is_empty());
        assert_eq!(t.waiting(), 0);
        // a set with one base reported stands, and its request's close is no refusal
        assert_eq!((t.live_sets(), t.sets_of(CASTER)), (2, 2));
        assert!(t.take_refusals().is_empty());
    }

    #[test]
    fn a_cast_with_no_report_in_the_window_places_nothing_and_is_noted() {
        let mut t = table();
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(CASTER, 1, standing(0.0, 0.0, 0.0, 90.0), true), 1_000));
        assert_eq!(t.take_requests().len(), 1);
        assert!(t.due_placements(1_000 + REPORT_WINDOW_MS).is_empty());
        assert_eq!(t.waiting(), 1);
        assert!(t.due_placements(1_000 + REPORT_WINDOW_MS + 1).is_empty());
        assert_eq!((t.waiting(), t.live_sets()), (0, 0));
        assert_eq!(t.take_refusals(), vec![Refused { caster: CASTER, spell: desc(RAISE_WALL), why: Refusal::NoReport }]);
        assert!(t.take_refusals().is_empty());
        assert_eq!(t.report_placed(wall_report(CASTER, 0), 5_000), Err(ReportRefusal::NoSet));
        // the stream's end inside the window keeps the request open: the
        // engine's objects stood while the stream did (a linger of 1 s here)
        let mut t = Placements::from_json(&raise_wall(2, true, 1, 120)).unwrap_or_default();
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(CASTER, 1, standing(0.0, 0.0, 0.0, 90.0), true), 10_000));
        assert_eq!(t.take_requests().first().map(|r| r.set), Some(0));
        t.cast_ended(CASTER, 1, 10_200);
        assert_eq!(t.waiting(), 1);
        assert_eq!(t.report_placed(wall_report(CASTER, 0), 10_500), Ok(()));
        assert_eq!(t.due_placements(10_500).len(), 1);
        // a report inside the window but after the linger ran out is late,
        // and nothing stands again
        assert_eq!(t.report_placed(halo_report(CASTER, 0), 11_300), Err(ReportRefusal::Late));
        // the set lingers from the stream's end, not from the report
        assert!(t.due_removals(11_199).is_empty());
        assert_eq!(t.due_removals(11_200), vec![Removal { set: 0, caster: CASTER, why: RemovalWhy::Lingered }]);
        assert_eq!(t.live_sets(), 0);
        // a short maxS closes the request too: a stream of 1 s at most
        let mut short = Placements::from_json(&raise_wall(2, true, 5, 1)).unwrap_or_default();
        assert!(short.cast_started(RAISE_WALL, PLUGIN, cast(CASTER, 1, standing(0.0, 0.0, 0.0, 90.0), true), 20_000));
        assert_eq!(short.take_requests().len(), 1);
        assert_eq!(short.report_placed(wall_report(CASTER, 0), 20_999), Ok(()));
        assert_eq!(short.report_placed(halo_report(CASTER, 0), 21_000), Err(ReportRefusal::Late));
        assert_eq!(short.due_placements(21_000).len(), 1);
        assert_eq!(short.due_removals(21_000).first().map(|r| r.why), Some(RemovalWhy::MaxReached));
    }

    #[test]
    fn a_new_stream_in_the_hand_replaces_the_request() {
        let mut t = table();
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(CASTER, 1, standing(0.0, 0.0, 0.0, 0.0), true), 0));
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(CASTER, 1, standing(0.0, 0.0, 0.0, 90.0), true), 200));
        let requests = t.take_requests();
        assert_eq!(requests.iter().map(|r| r.set).collect::<Vec<_>>(), vec![0, 1]);
        assert_eq!(t.waiting(), 1);
        assert_eq!(t.report_placed(wall_report(CASTER, 0), 500), Err(ReportRefusal::NoSet));
        assert_eq!(t.report_placed(wall_report(CASTER, 1), 500), Ok(()));
        assert_eq!(t.due_placements(500).len(), 1);
        // the other hand's request stands beside
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(CASTER, 0, standing(0.0, 0.0, 0.0, 90.0), true), 600));
        assert_eq!(t.take_requests().first().map(|r| r.set), Some(2));
        assert_eq!(t.waiting(), 2);
        assert_eq!(t.report_placed(halo_report(CASTER, 1), 700), Ok(()));
        assert_eq!(t.report_placed(wall_report(CASTER, 2), 700), Ok(()));
        assert_eq!(t.due_placements(700).len(), 2);
        assert_eq!((t.live_sets(), t.waiting()), (2, 1));
    }

    #[test]
    fn the_caps_refuse_the_cast_over_them_before_any_request() {
        let mut t = table();
        stand(&mut t, 7, 0, 0, 500);
        stand(&mut t, 7, 1, 10_000, 500);
        assert_eq!(t.sets_of(7), 2);
        // the third, with both held: refused for the caster, no request
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(7, 2, standing(0.0, 0.0, 0.0, 90.0), true), 20_000));
        assert!(t.take_requests().is_empty());
        assert_eq!((t.sets_of(7), t.waiting()), (2, 0));
        assert_eq!(t.take_refusals(), vec![Refused { caster: 7, spell: desc(RAISE_WALL), why: Refusal::PerCaster }]);
        assert!(t.take_refusals().is_empty());
        // another caster is under its own cap
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(8, 1, standing(0.0, 0.0, 0.0, 90.0), true), 20_000));
        assert_eq!(t.take_requests().len(), 1);
        // a request awaiting its reports counts as held
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(8, 0, standing(0.0, 0.0, 0.0, 90.0), true), 20_100));
        assert_eq!(t.take_requests().len(), 1);
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(8, 2, standing(0.0, 0.0, 0.0, 90.0), true), 20_200));
        assert!(t.take_requests().is_empty());
        assert_eq!(t.take_refusals().first().map(|r| (r.caster, r.why)), Some((8, Refusal::PerCaster)));

        // the world's cap: 64 casters holding one each refuse the 65th
        let mut world = Placements::from_json(&raise_wall(1, true, 5, 120)).unwrap_or_default();
        for caster in 1..=64u32 {
            stand(&mut world, caster, 1, 0, 500);
        }
        assert_eq!(world.live_sets(), MAX_SETS);
        assert!(world.cast_started(RAISE_WALL, PLUGIN, cast(65, 1, standing(0.0, 0.0, 0.0, 90.0), true), 1_000));
        assert!(world.take_requests().is_empty());
        assert_eq!(world.take_refusals().first().map(|r| (r.caster, r.why)), Some((65, Refusal::ServerWide)));
    }

    #[test]
    fn a_held_set_lingers_after_its_end_and_falls_at_max() {
        let mut t = table();
        stand(&mut t, 1, 1, 1_000, 500);
        // held for a minute: nothing falls
        assert!(t.due_removals(61_000).is_empty());
        t.cast_ended(1, 1, 61_000);
        assert!(t.due_removals(65_999).is_empty());
        assert_eq!(t.due_removals(66_000), vec![Removal { set: 0, caster: 1, why: RemovalWhy::Lingered }]);
        assert_eq!(t.live_sets(), 0);
        assert!(t.due_removals(70_000).is_empty());

        // a stream that never ends falls at the cast's start plus maxS
        stand(&mut t, 2, 1, 100_000, 500);
        assert!(t.due_removals(219_999).is_empty());
        assert_eq!(t.due_removals(220_000).first().map(|r| (r.caster, r.why)), Some((2, RemovalWhy::MaxReached)));

        // an end whose linger would pass maxS still falls at maxS
        stand(&mut t, 3, 1, 300_000, 500);
        t.cast_ended(3, 1, 419_000);
        assert_eq!(t.due_removals(420_000).first().map(|r| (r.caster, r.why)), Some((3, RemovalWhy::MaxReached)));
    }

    #[test]
    fn a_set_that_does_not_hold_lingers_from_its_first_placement() {
        // the entry's whileHeld false: the stream's end is not waited for
        let mut t = Placements::from_json(&raise_wall(2, false, 5, 120)).unwrap_or_default();
        stand(&mut t, 1, 1, 1_000, 500);
        assert!(t.due_removals(6_499).is_empty());
        assert_eq!(t.due_removals(6_500).first().map(|r| r.why), Some(RemovalWhy::Lingered));
        // a cast released whole (no stream) lingers from its first
        // placement too, whatever the entry says of holding
        let mut held = table();
        assert!(held.cast_started(RAISE_WALL, PLUGIN, cast(1, 1, standing(0.0, 0.0, 0.0, 90.0), false), 1_000));
        assert_eq!(held.take_requests().len(), 1);
        assert_eq!(held.report_placed(wall_report(1, 0), 1_500), Ok(()));
        assert_eq!(held.report_placed(halo_report(1, 0), 1_800), Ok(()));
        assert_eq!(held.due_placements(1_800).len(), 2);
        assert!(held.due_removals(6_499).is_empty());
        assert_eq!(held.due_removals(6_500).len(), 1);
    }

    #[test]
    fn a_caster_that_leaves_ends_its_sets_and_drops_its_requests() {
        let mut t = table();
        stand(&mut t, 1, 0, 1_000, 250);
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(1, 1, standing(0.0, 0.0, 0.0, 90.0), true), 1_300));
        assert_eq!(t.take_requests().len(), 1);
        t.caster_gone(1, 1_400);
        assert_eq!(t.waiting(), 0);
        assert_eq!(t.report_placed(wall_report(1, 1), 1_500), Err(ReportRefusal::NoSet));
        assert!(t.due_placements(2_000).is_empty());
        assert!(t.take_refusals().is_empty());
        assert!(t.due_removals(6_399).is_empty());
        assert_eq!(t.due_removals(6_400), vec![Removal { set: 0, caster: 1, why: RemovalWhy::Lingered }]);
    }
}
