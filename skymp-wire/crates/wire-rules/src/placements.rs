//! Spell-placed world objects (thuum docs/verbs/spell-placed-object.md,
//! ADR-057). A spell whose effect script puts an object into the world
//! (Apocalypse's Raise Wall first: a wall MSTT and a halo ACTI, 192 units
//! ahead of the caster) does so in the caster's game alone, since the script
//! runs nowhere else (ADR-029). The server places the objects itself from a
//! settings table keyed by spell, at the cast start it already accepts
//! (casts.rs), and every seat receives them through the server-object path;
//! the caster's game skips the server's copies, which name it, while its
//! own cast runs. The table is `spellPlacedObjects` in server-settings.json:
//! per spell an anchor (`caster` in v1; `hit-target` and `impact` are the
//! corpus's other two families and are refused until their pose rules
//! land), the objects (base, type MSTT or ACTI, the offset forward along the
//! caster's yaw and the height offset), a lifetime (`whileHeld`: the set
//! stands while the stream holds, `lingerS` after its end, `maxS` from the
//! cast start against a stream that never ends) and `perCaster`, the sets
//! one caster may hold at once; MAX_SETS bounds the world.
//!
//! The pose is the script's (apocrypha's read of WB_FabricateWall_Script
//! and the records, 2026-10-28): with Papyrus angles, x += forward *
//! sin(yaw), y += forward * cos(yaw), the yaw in degrees as the engine's Z
//! angle, z += dz; the rotation is (0, 0, yaw). Nothing moves or scales the
//! wall after the script's rise, so one pose suffices; a dual cast changes
//! nothing (SPIT No Dual Cast Modification: one effect instance, one wall).
//!
//! The gate: a listed spell, a cast of it by that caster still open
//! PLACE_AFTER_MS after its accepted start (the failure effect's window:
//! HYPOTHESIS that its script dispels within it), the caster under its cap
//! and the world under MAX_SETS. The times are the server's monotonic
//! milliseconds (PartOne's SteadyNowMs), as casts.rs takes them.

use crate::clock::SettingsError;
use crate::forms::FormDesc;
use serde::Deserialize;

/// How long a listed spell's cast must have stayed open before its objects
/// are placed, milliseconds: the engine runs the no-room failure effect in
/// the caster's game within this window (HYPOTHESIS, docs/verbs/
/// spell-placed-object.md Inventory), and a cast interrupted this soon
/// placed nothing in the script either.
pub const PLACE_AFTER_MS: u64 = 250;
/// Sets the server holds at once, world-wide: a stream held on every seat
/// of a full server twice over.
pub const MAX_SETS: usize = 64;
/// The most objects one entry may name: Raise Wall's two, with room for a
/// cage's walls.
pub const MAX_OBJECTS: usize = 8;

/// What a set is posed from (docs/research/apocalypse/placed-objects.md's
/// three families).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Anchor {
    /// The caster's position and yaw at the accepted cast (Raise Wall).
    Caster,
    /// The victim of the accepted hit (Tharn's Prison); no pose rule yet.
    HitTarget,
    /// The explosion point of a target-location cast (Summoning Rune); no
    /// pose rule yet.
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
    /// Units ahead of the anchor along its yaw.
    pub forward: f32,
    /// Units above the anchor (negative below).
    pub dz: f32,
}

/// How long a set stands.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Lifetime {
    /// The set stands while the concentration stream holds; its end starts
    /// the linger. False: the linger starts at the placement.
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
    /// What its objects are posed from.
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

/// A set to create now.
#[derive(Debug, Clone, PartialEq)]
pub struct Placement {
    /// The set's id, which names it to [`Placements::due_removals`].
    pub set: u64,
    /// The caster, whose id the objects carry on the wire (`placed_by`).
    pub caster: u32,
    /// The spell.
    pub spell: FormDesc,
    /// The objects, posed.
    pub objects: Vec<ObjectPose>,
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
    forward: f32,
    dz: f32,
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

/// A cast of a listed spell waiting for PLACE_AFTER_MS.
#[derive(Debug, Clone, PartialEq)]
struct Pending {
    caster: u32,
    hand: u8,
    entry: usize,
    pose: CasterPose,
    started_ms: u64,
    concentration: bool,
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

/// The table and every set standing.
#[derive(Debug, Clone, Default, PartialEq)]
pub struct Placements {
    entries: Vec<Entry>,
    pending: Vec<Pending>,
    sets: Vec<Set>,
    refusals: Vec<Refused>,
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
        // placed-objects.md) take this table with pose rules of their own,
        // not written yet; naming one is a mistake the operator should hear
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
        if !(o.forward.is_finite() && o.dz.is_finite()) {
            return Err(SettingsError(format!("spellPlacedObjects: {:?} has an object whose forward or dz is not a number", e.spell)));
        }
        objects.push(PlacedObject { base: form("base", &o.base)?, kind, forward: o.forward, dz: o.dz });
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

/// Where an object of `entry`'s kind stands for a caster posed at `caster`:
/// the caster anchor's rule (the module doc; the other anchors are refused
/// at parse).
#[must_use]
pub fn pose(caster: CasterPose, object: &PlacedObject) -> ObjectPose {
    let yaw = caster.yaw.to_radians();
    ObjectPose {
        base: object.base.clone(),
        kind: object.kind,
        x: caster.x + object.forward * yaw.sin(),
        y: caster.y + object.forward * yaw.cos(),
        z: caster.z + object.dz,
        yaw: caster.yaw,
    }
}

fn finite(pose: &CasterPose) -> bool {
    pose.x.is_finite() && pose.y.is_finite() && pose.z.is_finite() && pose.yaw.is_finite()
}

impl Placements {
    /// The table from the `spellPlacedObjects` block's JSON text (`[]` for
    /// none). The error names a bad entry: a form that is not "<hex
    /// id>:<plugin>", an anchor other than `caster`, no objects or more than
    /// MAX_OBJECTS, a type other than MSTT or ACTI, an offset that is not a
    /// number, a maxS of 0, a perCaster outside 1 to MAX_SETS, an unknown
    /// key, a spell listed twice.
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

    /// The server accepted `cast` of the spell `short_id` in `file` at
    /// `now_ms`. A listed spell's cast waits for PLACE_AFTER_MS (the hand's
    /// earlier wait, if any, is dropped, as a new stream ends the hand's
    /// old one); a pose that is not a number places nothing. Returns
    /// whether the spell is listed.
    pub fn cast_started(&mut self, short_id: u32, file: &str, cast: CastStart, now_ms: u64) -> bool {
        let Some(entry) = self.entry_of(short_id, file) else {
            return false;
        };
        let CastStart { caster, hand, pose, concentration } = cast;
        // the hand's earlier stream is over, as casts.rs takes it: a set it
        // held begins its linger, a wait it had is dropped
        self.end_hand(caster, hand, now_ms);
        if finite(&pose) {
            self.pending.push(Pending { caster, hand, entry, pose, started_ms: now_ms, concentration });
        }
        true
    }

    /// `caster`'s stream in `hand` ended at `now_ms`: a cast still waiting
    /// places nothing; a held set's linger begins.
    pub fn cast_ended(&mut self, caster: u32, hand: u8, now_ms: u64) {
        self.end_hand(caster, hand, now_ms);
    }

    fn end_hand(&mut self, caster: u32, hand: u8, now_ms: u64) {
        self.pending.retain(|p| !(p.caster == caster && p.hand == hand));
        for set in self.sets.iter_mut().filter(|s| s.caster == caster && s.hand == hand && s.end_ms.is_none()) {
            let linger = self.entries.get(set.entry).map_or(0, |e| e.lifetime.linger_ms);
            set.end_ms = Some(now_ms.saturating_add(linger));
        }
    }

    /// `caster` is gone (disconnected) at `now_ms`: every hand's stream
    /// ends as in [`Self::cast_ended`].
    pub fn caster_gone(&mut self, caster: u32, now_ms: u64) {
        for hand in 0..4u8 {
            self.cast_ended(caster, hand, now_ms);
        }
    }

    /// The sets to create at `now_ms`: every waiting cast open for
    /// PLACE_AFTER_MS whose caster is under its entry's cap and whose world
    /// is under MAX_SETS, each once; a cast over a cap is dropped and noted
    /// for [`Self::take_refusals`]. A set that does not hold (the entry's
    /// `whileHeld` false, or a cast released whole) begins its linger now.
    pub fn due_placements(&mut self, now_ms: u64) -> Vec<Placement> {
        let mut out = Vec::new();
        let (ready, waiting): (Vec<Pending>, Vec<Pending>) =
            self.pending.drain(..).partition(|p| now_ms.saturating_sub(p.started_ms) >= PLACE_AFTER_MS);
        self.pending = waiting;
        for p in ready {
            let Some(entry) = self.entries.get(p.entry) else {
                continue;
            };
            let held_by_caster = self.sets.iter().filter(|s| s.caster == p.caster).count();
            let refusal = if self.sets.len() >= MAX_SETS {
                Some(Refusal::ServerWide)
            } else if held_by_caster >= entry.per_caster {
                Some(Refusal::PerCaster)
            } else {
                None
            };
            if let Some(why) = refusal {
                self.refusals.push(Refused { caster: p.caster, spell: entry.spell.clone(), why });
                continue;
            }
            let id = self.next_set;
            self.next_set = self.next_set.saturating_add(1);
            let holds = p.concentration && entry.lifetime.while_held;
            let end_ms = if holds { None } else { Some(now_ms.saturating_add(entry.lifetime.linger_ms)) };
            self.sets.push(Set { id, caster: p.caster, hand: p.hand, entry: p.entry, started_ms: p.started_ms, end_ms });
            out.push(Placement {
                set: id,
                caster: p.caster,
                spell: entry.spell.clone(),
                objects: entry.objects.iter().map(|o| pose(p.pose, o)).collect(),
            });
        }
        out
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

    /// Sets `caster` holds.
    #[must_use]
    pub fn sets_of(&self, caster: u32) -> usize {
        self.sets.iter().filter(|s| s.caster == caster).count()
    }

    /// Casts waiting for PLACE_AFTER_MS.
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

    // the Raise Wall entry as both deploy settings files carry it
    fn raise_wall(per_caster: usize, while_held: bool, linger_s: u64, max_s: u64) -> String {
        format!(
            r#"[{{"spell": "00cc14:{PLUGIN}", "anchor": "caster",
                 "objects": [{{"base": "098529:{PLUGIN}", "type": "MSTT", "forward": 192, "dz": -48}},
                             {{"base": "0735e8:{PLUGIN}", "type": "ACTI", "forward": 192, "dz": 0}}],
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
        assert!(wall.is_some_and(|o| o.base.is(WALL, PLUGIN) && o.kind == BaseKind::Mstt && near(o.forward, 192.0) && near(o.dz, -48.0)), "{wall:?}");
        let halo = e.and_then(|e| e.objects.get(1));
        assert!(halo.is_some_and(|o| o.base.is(HALO, PLUGIN) && o.kind == BaseKind::Acti && near(o.forward, 192.0) && near(o.dz, 0.0)), "{halo:?}");
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
            (good.replace(r#""dz": -48"#, r#""dz": 1e39"#), "dz out of f32"),
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
    fn the_pose_follows_the_yaw_with_papyrus_angles() {
        // apocrypha's read, 2026-10-28: x += 192 sin(yaw), y += 192 cos(yaw),
        // the wall 48 below the caster, the halo at its height, both at the
        // caster's yaw
        let wall = PlacedObject { base: FormDesc { short_id: WALL, file: PLUGIN.to_owned() }, kind: BaseKind::Mstt, forward: 192.0, dz: -48.0 };
        let halo = PlacedObject { base: FormDesc { short_id: HALO, file: PLUGIN.to_owned() }, kind: BaseKind::Acti, forward: 192.0, dz: 0.0 };
        let at = standing(1000.0, -2000.0, 300.0, 0.0);
        for (yaw, dx, dy) in [(0.0, 0.0, 192.0), (90.0, 192.0, 0.0), (180.0, 0.0, -192.0), (270.0, -192.0, 0.0), (-90.0, -192.0, 0.0)] {
            let p = pose(CasterPose { yaw, ..at }, &wall);
            assert!(near(p.x, 1000.0 + dx) && near(p.y, -2000.0 + dy) && near(p.z, 252.0) && near(p.yaw, yaw), "{yaw}: {p:?}");
            assert_eq!(p.kind, BaseKind::Mstt);
            assert!(p.base.is(WALL, PLUGIN));
            let h = pose(CasterPose { yaw, ..at }, &halo);
            assert!(near(h.x, p.x) && near(h.y, p.y) && near(h.z, 300.0) && near(h.yaw, yaw), "{yaw}: {h:?}");
        }
    }

    #[test]
    fn a_listed_cast_places_after_the_wait_while_open_and_not_before() {
        let mut t = table();
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(0xff00_0000, 1, standing(0.0, 0.0, 0.0, 90.0), true), 10_000));
        assert_eq!(t.waiting(), 1);
        assert!(t.due_placements(10_000 + PLACE_AFTER_MS - 1).is_empty());
        let placed = t.due_placements(10_000 + PLACE_AFTER_MS);
        assert_eq!(placed.len(), 1);
        let p = placed.first();
        assert!(p.is_some_and(|p| p.set == 0 && p.caster == 0xff00_0000 && p.spell.is(RAISE_WALL, PLUGIN) && p.objects.len() == 2), "{p:?}");
        let wall = p.and_then(|p| p.objects.first());
        assert!(wall.is_some_and(|o| near(o.x, 192.0) && near(o.y, 0.0) && near(o.z, -48.0) && near(o.yaw, 90.0) && o.kind == BaseKind::Mstt), "{wall:?}");
        let halo = p.and_then(|p| p.objects.get(1));
        assert!(halo.is_some_and(|o| near(o.x, 192.0) && near(o.y, 0.0) && near(o.z, 0.0) && o.kind == BaseKind::Acti), "{halo:?}");
        assert_eq!((t.live_sets(), t.sets_of(0xff00_0000), t.waiting()), (1, 1, 0));
        // placed once
        assert!(t.due_placements(20_000).is_empty());
        // a cast ended inside the wait places nothing
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(0xff00_0001, 0, standing(0.0, 0.0, 0.0, 0.0), true), 30_000));
        t.cast_ended(0xff00_0001, 0, 30_100);
        assert!(t.due_placements(31_000).is_empty());
        assert_eq!(t.sets_of(0xff00_0001), 0);
        // an unlisted spell is nobody's business here
        assert!(!t.cast_started(0x12fcd, "Skyrim.esm", cast(0xff00_0001, 1, standing(0.0, 0.0, 0.0, 0.0), true), 40_000));
        assert!(t.due_placements(50_000).is_empty());
        // a pose that is not a number places nothing
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(0xff00_0001, 1, standing(f32::NAN, 0.0, 0.0, 0.0), true), 60_000));
        assert!(t.due_placements(70_000).is_empty());
        assert!(t.take_refusals().is_empty());
    }

    #[test]
    fn a_new_stream_in_the_hand_replaces_the_wait() {
        let mut t = table();
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(1, 1, standing(0.0, 0.0, 0.0, 0.0), true), 0));
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(1, 1, standing(0.0, 0.0, 0.0, 90.0), true), 200));
        assert!(t.due_placements(260).is_empty());
        let placed = t.due_placements(450);
        assert_eq!(placed.len(), 1);
        assert!(placed.first().and_then(|p| p.objects.first()).is_some_and(|o| near(o.x, 192.0)));
    }

    #[test]
    fn the_caps_refuse_the_cast_over_them() {
        let mut t = table();
        for (hand, at) in [(0u8, 0u64), (1, 10_000)] {
            assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(7, hand, standing(0.0, 0.0, 0.0, 0.0), true), at));
            assert_eq!(t.due_placements(at.saturating_add(PLACE_AFTER_MS)).len(), 1);
        }
        assert_eq!(t.sets_of(7), 2);
        // the third, with both held: refused for the caster, nothing placed
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(7, 2, standing(0.0, 0.0, 0.0, 0.0), true), 20_000));
        assert!(t.due_placements(20_000 + PLACE_AFTER_MS).is_empty());
        assert_eq!(t.sets_of(7), 2);
        assert_eq!(t.take_refusals(), vec![Refused { caster: 7, spell: FormDesc { short_id: RAISE_WALL, file: PLUGIN.to_owned() }, why: Refusal::PerCaster }]);
        assert!(t.take_refusals().is_empty());
        // another caster is under its own cap
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(8, 1, standing(0.0, 0.0, 0.0, 0.0), true), 20_000));
        assert_eq!(t.due_placements(20_000 + PLACE_AFTER_MS).len(), 1);

        // the world's cap: 64 casters holding one each refuse the 65th
        let mut world = Placements::from_json(&raise_wall(1, true, 5, 120)).unwrap_or_default();
        for caster in 1..=64u32 {
            assert!(world.cast_started(RAISE_WALL, PLUGIN, cast(caster, 1, standing(0.0, 0.0, 0.0, 0.0), true), 0));
        }
        assert_eq!(world.due_placements(PLACE_AFTER_MS).len(), MAX_SETS);
        assert_eq!(world.live_sets(), MAX_SETS);
        assert!(world.cast_started(RAISE_WALL, PLUGIN, cast(65, 1, standing(0.0, 0.0, 0.0, 0.0), true), 1_000));
        assert!(world.due_placements(1_000 + PLACE_AFTER_MS).is_empty());
        assert_eq!(world.take_refusals().first().map(|r| (r.caster, r.why)), Some((65, Refusal::ServerWide)));
    }

    #[test]
    fn a_held_set_lingers_after_its_end_and_falls_at_max() {
        let mut t = table();
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(1, 1, standing(0.0, 0.0, 0.0, 0.0), true), 1_000));
        assert_eq!(t.due_placements(1_250).len(), 1);
        // held for a minute: nothing falls
        assert!(t.due_removals(61_000).is_empty());
        t.cast_ended(1, 1, 61_000);
        assert!(t.due_removals(65_999).is_empty());
        assert_eq!(t.due_removals(66_000), vec![Removal { set: 0, caster: 1, why: RemovalWhy::Lingered }]);
        assert_eq!(t.live_sets(), 0);
        assert!(t.due_removals(70_000).is_empty());

        // a stream that never ends falls at the cast's start plus maxS
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(2, 1, standing(0.0, 0.0, 0.0, 0.0), true), 100_000));
        assert_eq!(t.due_placements(100_250).len(), 1);
        assert!(t.due_removals(219_999).is_empty());
        assert_eq!(t.due_removals(220_000).first().map(|r| (r.caster, r.why)), Some((2, RemovalWhy::MaxReached)));

        // an end whose linger would pass maxS still falls at maxS
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(3, 1, standing(0.0, 0.0, 0.0, 0.0), true), 300_000));
        assert_eq!(t.due_placements(300_250).len(), 1);
        t.cast_ended(3, 1, 419_000);
        assert_eq!(t.due_removals(420_000).first().map(|r| (r.caster, r.why)), Some((3, RemovalWhy::MaxReached)));
    }

    #[test]
    fn a_set_that_does_not_hold_lingers_from_its_placement() {
        // the entry's whileHeld false: the stream's end is not waited for
        let mut t = Placements::from_json(&raise_wall(2, false, 5, 120)).unwrap_or_default();
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(1, 1, standing(0.0, 0.0, 0.0, 0.0), true), 1_000));
        assert_eq!(t.due_placements(1_250).len(), 1);
        assert!(t.due_removals(6_249).is_empty());
        assert_eq!(t.due_removals(6_250).first().map(|r| r.why), Some(RemovalWhy::Lingered));
        // a cast released whole (no stream) lingers from its placement too,
        // whatever the entry says of holding
        let mut held = table();
        assert!(held.cast_started(RAISE_WALL, PLUGIN, cast(1, 1, standing(0.0, 0.0, 0.0, 0.0), false), 1_000));
        assert_eq!(held.due_placements(1_250).len(), 1);
        assert!(held.due_removals(6_249).is_empty());
        assert_eq!(held.due_removals(6_250).len(), 1);
    }

    #[test]
    fn a_caster_that_leaves_ends_its_sets_and_its_waits() {
        let mut t = table();
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(1, 0, standing(0.0, 0.0, 0.0, 0.0), true), 1_000));
        assert_eq!(t.due_placements(1_250).len(), 1);
        assert!(t.cast_started(RAISE_WALL, PLUGIN, cast(1, 1, standing(0.0, 0.0, 0.0, 0.0), true), 1_300));
        t.caster_gone(1, 1_400);
        assert_eq!(t.waiting(), 0);
        assert!(t.due_placements(2_000).is_empty());
        assert!(t.due_removals(6_399).is_empty());
        assert_eq!(t.due_removals(6_400), vec![Removal { set: 0, caster: 1, why: RemovalWhy::Lingered }]);
    }
}
