//! Script natives (thuum ADR-068, the one seam; docs/research/
//! scripts-one-system-2026-11-05.md 6.3, 6.4 and 6.11 items 1, 8 and 13). A
//! script runs on the engine that owns its object, and every native it calls
//! passes the engine's one dispatch (NativeFunctionBase::Call), where the
//! platform reads the call's typed frame and the owner reports a tabled call
//! as ScriptCall (MsgType 75). The table is data, one row per native
//! ([`TABLE`], natives_table.rs): what a call is (kind), whose object it
//! concerns (subject), who owns that object's scripts (owner), who hears it
//! (audience), what a receiver runs for it (relay), what the server keeps of
//! it (keep) and its parameters' types.
//!
//! The server never understands an action: the shape is the gate, judged in
//! two steps. The shape step ([`ScriptCalls::shape`]) comes first, before the
//! core reads anything of the subject: the reporter is within its budget,
//! [`BURST`] then [`PER_SECOND`] a second, a token spent by every call that
//! reaches the step, refused or not (`E_SCRIPTCALL_RATE`); the pair is a
//! tabled action row the server relays (`E_SCRIPTCALL_ROW`); the arguments
//! are the row's count and kinds and the subject is one the reporter may name
//! (`E_SCRIPTCALL_ARGS`). So a flood of malformed calls is cut off by the
//! rate, and a call refused there costs the core no lookup (a subject the
//! server does not hold is read from its game-file record). The owner step
//! ([`ScriptCalls::owner`]) judges a shaped call over the facts the core then
//! gathers for its subject: a world object whose cell the reporter hosts at
//! the reported epoch (`E_SCRIPTCALL_OWNER`), a reference the server holds or
//! a game-file placement it holds none for, placed by its record
//! ([`SubjectKind::Record`]: the loader attaches no moving static, so Bleak
//! Falls Sanctum's door FX is no reference of the server's). A door's or a
//! container's call is dropped quietly (its open is Activate's isOpen, as
//! ObjectAnimation's handler drops it) and an actor's is refused as the
//! streams' (`E_SCRIPTCALL_ROW`: the row is local for an actor). A value is
//! never judged (ADR-062's amendment). An accepted call takes its subject's
//! next sequence (6.11 item 1: one per object, owned by the server, so a
//! receiver applies one object's relays in order and drops a stale one) and
//! goes to the row's audience but the reporter as the row's relay native.

use std::collections::HashMap;

/// What a call of a native is (6.3's kind column).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Kind {
    /// A consequence the engine never saves (an animation event, a graph
    /// variable): the call itself is the message, relayed to run on every
    /// other seat.
    Action,
    /// A consequence the engine saves for the object, carried by its change
    /// form or its typed verb (rungs 4 and 5), never relayed as an action.
    State,
    /// A call on what nobody owns (a quest, a global, an alias; D-F1 C), run
    /// on every seat, its consequences meeting the gates at their own selves
    /// (rung 5).
    Everyone,
    /// A call whose consequence stays on its seat; never sent.
    Local,
}

/// Where a call's subject stands.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Subject {
    /// The call's self: a method native's.
    SelfRef,
    /// The call's first argument: a static native's target
    /// (Debug.SendAnimationEvent's reference).
    Arg0,
    /// None: a static native on no object.
    Nothing,
}

/// Who owns the subject's scripts (6.6, ADR-063 decision 1).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Owner {
    /// A world object's: the host of its cell.
    CellHost,
    /// An actor's: its simulator (hosted and not a puppet).
    Simulator,
    /// The player's own: its seat.
    Player,
    /// A quest's, a global's, an alias's: nobody's (D-F1 C).
    Nobody,
}

/// Who hears an accepted call.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Audience {
    /// The subject's listeners.
    Listeners,
    /// Every seat.
    Everyone,
    /// No seat.
    Nobody,
}

/// What a receiver runs for an accepted call.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Relay {
    /// The same native with the same arguments.
    Same,
    /// A non-latent twin (6.11 item 2: a receiver never runs a latent native,
    /// so no guest stack waits) with the call's arguments at `args`, in that
    /// order.
    Twin {
        /// The twin's class.
        cls: &'static str,
        /// The twin's name.
        func: &'static str,
        /// The call's arguments the twin takes, by index.
        args: &'static [u8],
    },
    /// Nothing: the row is never relayed.
    Never,
}

/// What the server keeps of an accepted call on its subject.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Keep {
    /// Nothing.
    Nothing,
    /// The call's behavior graph event as the subject's lastAnimation
    /// (CreateActor's prop, so a late joiner replays it); rung 7 retires it
    /// when the record carries the script's own state.
    Last,
}

/// A parameter's Papyrus type, as the platform reads the frame's variable.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Param {
    /// Bool.
    Bool,
    /// Int, a number on the wire.
    Int,
    /// Float, a number on the wire.
    Float,
    /// String.
    String,
    /// An object, its form id and type name on the wire, or None.
    Object,
}

/// One native's row of the table.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Row {
    /// The native's Papyrus class.
    pub cls: &'static str,
    /// The native's name.
    pub func: &'static str,
    /// What a call of it is.
    pub kind: Kind,
    /// Where its subject stands.
    pub subject: Subject,
    /// Who owns the subject's scripts.
    pub owner: Owner,
    /// Who hears an accepted call.
    pub audience: Audience,
    /// What a receiver runs for one.
    pub relay: Relay,
    /// What the server keeps of one.
    pub keep: Keep,
    /// Its parameters in order (FunctionsDump.txt's types for the native).
    pub params: &'static [Param],
}

include!("natives_table.rs");

/// The natives whose relay sends a behavior graph event, the family
/// ObjectAnimation (MsgType 71) carried: an accepted call of a row relayed
/// as one of these also writes the "animates" line ObjectAnimation's handler
/// wrote (b-puzzle-relay and its kin claim it), until rung 7 retires
/// lastAnimation.
const GRAPH_EVENT_RELAYS: [(&str, &str); 2] = [("ObjectReference", "PlayAnimation"), ("Debug", "SendAnimationEvent")];

/// Argument indices for a relay of every argument: a native takes at most
/// eleven.
const EVERY_ARG: [u8; 11] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10];

/// Where the players' figures and the server's own forms begin: a client
/// names neither (the server does).
const SERVER_NAMED: u32 = 0xff00_0000;

/// What a refusal of a [`SubjectKind::Record`] subject adds to its words:
/// the server holds no reference for it and placed it by its record.
const BY_RECORD: &str = " (a reference the server does not load, placed by its game-file record)";

/// The row of `cls`.`func`, its place in [`TABLE`]; Papyrus names compare
/// without case.
#[must_use]
pub fn row_index(cls: &str, func: &str) -> Option<usize> {
    TABLE.iter().position(|r| r.cls.eq_ignore_ascii_case(cls) && r.func.eq_ignore_ascii_case(func))
}

/// The row of `cls`.`func`.
#[must_use]
pub fn row(cls: &str, func: &str) -> Option<&'static Row> {
    row_index(cls, func).and_then(|i| TABLE.get(i))
}

impl Row {
    /// The native a receiver runs for an accepted call, none when the row is
    /// never relayed.
    #[must_use]
    pub fn relay_target(&self) -> Option<(&'static str, &'static str)> {
        match self.relay {
            Relay::Same => Some((self.cls, self.func)),
            Relay::Twin { cls, func, .. } => Some((cls, func)),
            Relay::Never => None,
        }
    }

    /// The call's arguments a relay carries, by index, in order.
    #[must_use]
    pub fn relay_args(&self) -> &'static [u8] {
        match self.relay {
            Relay::Same => EVERY_ARG.get(..self.params.len()).unwrap_or(&[]),
            Relay::Twin { args, .. } => args,
            Relay::Never => &[],
        }
    }

    /// The call's argument naming its behavior graph event (the row's first
    /// string parameter) when the row's relay sends one, for the "animates"
    /// line and a keep-last row's lastAnimation.
    #[must_use]
    pub fn graph_event_arg(&self) -> Option<u8> {
        let (cls, func) = self.relay_target()?;
        if !GRAPH_EVENT_RELAYS.iter().any(|(c, f)| c.eq_ignore_ascii_case(cls) && f.eq_ignore_ascii_case(func)) {
            return None;
        }
        self.params.iter().position(|p| *p == Param::String).and_then(|i| u8::try_from(i).ok())
    }
}

/// One argument of a reported call as the server read it: its kind, and an
/// object's form id (the value is never judged).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Arg {
    /// JSON null: an argument the boundary could not read, or a None object.
    Absent,
    /// A Bool.
    Bool,
    /// An Int or a Float.
    Number,
    /// A String.
    String,
    /// An object, by its form id.
    Object(u64),
}

impl Arg {
    fn name(self) -> &'static str {
        match self {
            Arg::Absent => "null",
            Arg::Bool => "a bool",
            Arg::Number => "a number",
            Arg::String => "a string",
            Arg::Object(_) => "an object",
        }
    }

    /// Whether this argument may stand for a parameter of `param`'s type
    /// (a None object is null).
    fn fits(self, param: Param) -> bool {
        matches!(
            (self, param),
            (Arg::Bool, Param::Bool)
                | (Arg::Number, Param::Int | Param::Float)
                | (Arg::String, Param::String)
                | (Arg::Object(_) | Arg::Absent, Param::Object)
        )
    }
}

/// A form id a reporter may name as a subject: a load-order form, or 0x14
/// for its own player; never 0 and never one the server names.
fn nameable(id: u64) -> Option<u32> {
    u32::try_from(id).ok().filter(|&id| id != 0 && id < SERVER_NAMED)
}

/// The subject of a call of `row` reported with `self_id` and `args`, as the
/// reporter named it (0x14 for its own player), for the core to gather the
/// subject's facts; none when the call names no subject the reporter may
/// name.
#[must_use]
pub fn subject_of(row: &Row, self_id: u32, args: &[Arg]) -> Option<u32> {
    match row.subject {
        Subject::SelfRef => nameable(u64::from(self_id)),
        Subject::Arg0 => match args.first() {
            Some(Arg::Object(id)) => nameable(*id),
            _ => None,
        },
        Subject::Nothing => None,
    }
}

/// What the server holds at a reported call's subject.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SubjectKind {
    /// No reference the server holds, and no game-file placement it can
    /// place by its record.
    Unknown,
    /// A world object: no actor, no door, no container.
    WorldObject,
    /// A world object the server holds no reference for: a game-file
    /// placement (a REFR, never an actor's ACHR) whose base the loader never
    /// attaches (a moving static: Bleak Falls Sanctum's door FX 0x5256c,
    /// which HallofStoriesKeyholeScript.openDoor plays FXDoorOpen on), placed
    /// by its record's cell. Judged as a world object; its listeners are the
    /// games with that cell loaded, since no reference of the server's has
    /// any, and the server keeps nothing of it.
    Record,
    /// A door or a container, whose open is Activate's isOpen.
    DoorOrContainer,
    /// An actor (the reporter's own player among them).
    Actor,
}

/// What the core knows of a reported call.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Facts {
    /// The reporter's actor.
    pub sender: u32,
    /// The message's selfId: the call's subject.
    pub self_id: u32,
    /// The message's epoch: the subject's owner epoch the reporter holds.
    pub epoch: u32,
    /// What the server holds at the subject.
    pub kind: SubjectKind,
    /// The subject's cell, 0 when the server cannot place it.
    pub cell: u32,
    /// The subject's owner by its kind: the host of its cell for a world
    /// object, held or placed by its record, 0 while nobody hosts it (and
    /// for every other kind: no action row is an actor's, the streams carry
    /// those).
    pub owner: u32,
    /// That owner's standing epoch.
    pub owner_epoch: u32,
}

/// Why a reported call is refused.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Code {
    /// The pair is no tabled action row the server relays.
    Row,
    /// The arguments are not the row's count and kinds, or the subject is
    /// none the reporter may name.
    Args,
    /// The reporter does not own the subject at the epoch.
    Owner,
    /// The reporter's budget is spent.
    Rate,
}

impl Code {
    /// The stable name the server logs.
    #[must_use]
    pub fn name(self) -> &'static str {
        match self {
            Code::Row => "E_SCRIPTCALL_ROW",
            Code::Args => "E_SCRIPTCALL_ARGS",
            Code::Owner => "E_SCRIPTCALL_OWNER",
            Code::Rate => "E_SCRIPTCALL_RATE",
        }
    }
}

/// A refused call: the code and why, for the log.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Refusal {
    /// The code.
    pub code: Code,
    /// Why, in the facts' words.
    pub why: String,
}

/// An accepted call: what the server relays, to whom, with which sequence.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Accept {
    /// The subject's form id.
    pub subject: u32,
    /// The subject's sequence after this call, 1 for its first.
    pub seq: u32,
    /// The relay native's class.
    pub relay_cls: &'static str,
    /// The relay native's name.
    pub relay_fn: &'static str,
    /// The call's arguments the relay carries, by index, in order.
    pub relay_args: &'static [u8],
    /// The relay's self: the subject for a method native's row, 0 for a
    /// static one's (the target travels among the arguments).
    pub relay_self: u32,
    /// Who hears it.
    pub audience: Audience,
    /// What the server keeps of it.
    pub keep: Keep,
    /// The argument naming its behavior graph event, for the "animates" line.
    pub graph_event_arg: Option<u8>,
}

/// The rule's word on a reported call.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Verdict {
    /// Relayed: the row's relay native to the audience but the reporter.
    Accept(Accept),
    /// Dropped quietly: a door's or a container's call.
    Skip,
    /// Refused, logged by the code's name.
    Refuse(Refusal),
}

fn refusal(code: Code, why: String) -> Refusal {
    Refusal { code, why }
}

fn refuse(code: Code, why: String) -> Verdict {
    Verdict::Refuse(refusal(code, why))
}

/// A reported call the shape step passed: its row, the subject it names and
/// the native a receiver runs for it, for the core to gather the subject's
/// facts and ask the owner step. Only [`ScriptCalls::shape`] makes one, after
/// it took the reporter's token, and [`ScriptCalls::owner`] takes it by
/// value, so the owner step judges each shaped call once and never a call
/// the shape step refused.
#[derive(Debug, PartialEq, Eq)]
pub struct Shaped {
    row: Row,
    subject: u32,
    relay_cls: &'static str,
    relay_fn: &'static str,
}

impl Shaped {
    /// The subject's form id as the reporter named it (0x14 for its own
    /// player): the call's selfId, a form the reporter may name.
    #[must_use]
    pub fn subject(&self) -> u32 {
        self.subject
    }
}

/// How many calls a reporter may send at once (a pillar's turn is two, a
/// trap's swing and reset a few; ObjectAnimation's and ActorSpeech's burst).
/// UNMEASURED: the lab's whole-game count at 104651 (6.11 item 11) sets it.
pub const BURST: u32 = 32;

/// How many calls a second after the burst. UNMEASURED, as [`BURST`].
pub const PER_SECOND: u32 = 8;

/// One reporter's budget: a token bucket on the caller's monotonic clock.
#[derive(Debug, Clone, Copy)]
struct Bucket {
    tokens: u32,
    last_refill_ms: u64,
}

impl Bucket {
    fn full(now_ms: u64) -> Self {
        Self { tokens: BURST, last_refill_ms: now_ms }
    }

    /// Credit the refill for the time since the last, then take a token.
    fn take(&mut self, now_ms: u64) -> bool {
        let elapsed_ms = now_ms.saturating_sub(self.last_refill_ms);
        let refill = elapsed_ms.saturating_mul(u64::from(PER_SECOND)).checked_div(1000).unwrap_or(0);
        if refill > 0 {
            let refill_u32 = u32::try_from(refill).unwrap_or(u32::MAX);
            self.tokens = self.tokens.saturating_add(refill_u32).min(BURST);
            // credit exactly the time those tokens took, so fractions carry
            let credited_ms = refill.saturating_mul(1000).checked_div(u64::from(PER_SECOND)).unwrap_or(0);
            self.last_refill_ms = self.last_refill_ms.saturating_add(credited_ms);
        }
        match self.tokens.checked_sub(1) {
            Some(t) => {
                self.tokens = t;
                true
            }
            None => false,
        }
    }
}

/// The server's script-call state: every reporter's budget and every
/// subject's sequence, by form id. Runtime only: a restart starts every
/// budget full and every sequence at 0 (rung 3 serves the sequence with the
/// record). A budget outlives its reporter's disconnect, as a voice's
/// recovery does, so a reconnect does not refill it.
#[derive(Debug, Default)]
pub struct ScriptCalls {
    budgets: HashMap<u32, Bucket>,
    seqs: HashMap<u32, u32>,
}

impl ScriptCalls {
    /// The shape step on one reported call of `row` (none for a pair the
    /// table lacks) with `args`, from `sender` naming `self_id`, at `now_ms`
    /// on a monotonic clock: the core asks it before it reads anything of
    /// the subject. The reporter's token comes first, spent by every call
    /// that reaches the step, refused or not, so a flood of malformed calls
    /// is cut off by the rate; then the row, the arguments and the subject's
    /// form. A call refused here costs the core no lookup.
    pub fn shape(&mut self, row: Option<&Row>, args: &[Arg], sender: u32, self_id: u32, now_ms: u64) -> Result<Shaped, Refusal> {
        if !self.budgets.entry(sender).or_insert_with(|| Bucket::full(now_ms)).take(now_ms) {
            return Err(refusal(Code::Rate, format!("past the reporter's {BURST} at once and {PER_SECOND} a second")));
        }
        let Some(row) = row else {
            return Err(refusal(Code::Row, "the pair has no row in the table".to_owned()));
        };
        match row.kind {
            Kind::Action => {}
            Kind::State => return Err(refusal(Code::Row, "a state row: its consequence travels as the object's record (rung 4), never as an action".to_owned())),
            Kind::Everyone => return Err(refusal(Code::Row, "an everyone row: every seat runs it (rung 5), never relayed as an action".to_owned())),
            Kind::Local => return Err(refusal(Code::Row, "a local row: its consequence stays on its seat".to_owned())),
        }
        let Some((relay_cls, relay_fn)) = row.relay_target() else {
            return Err(refusal(Code::Row, "an action row the table never relays".to_owned()));
        };
        if row.owner != Owner::CellHost || row.audience != Audience::Listeners {
            return Err(refusal(Code::Row, format!("an action row owned by the {:?} for the {:?}: no relay before its rung", row.owner, row.audience)));
        }
        if args.len() != row.params.len() {
            return Err(refusal(Code::Args, format!("{} argument(s), the row's {}", args.len(), row.params.len())));
        }
        for (i, (arg, param)) in args.iter().zip(row.params.iter()).enumerate() {
            if !arg.fits(*param) {
                return Err(refusal(Code::Args, format!("argument {i} is {}, the row's {param:?}", arg.name())));
            }
        }
        let Some(subject) = subject_of(row, self_id, args) else {
            return Err(refusal(Code::Args, format!("no subject the reporter may name (selfId {self_id:x})")));
        };
        if subject != self_id {
            return Err(refusal(Code::Args, format!("selfId {self_id:x} is not the call's subject {subject:x}")));
        }
        Ok(Shaped { row: *row, subject, relay_cls, relay_fn })
    }

    /// The owner step on a call the shape step passed, over the `facts` the
    /// core gathered for its subject: the subject's kind, its cell, that
    /// cell's host and the host's epoch, against `facts.sender` and
    /// `facts.epoch`. An accepted call takes its subject's next sequence.
    pub fn owner(&mut self, shaped: Shaped, facts: &Facts) -> Verdict {
        let Shaped { row, subject, relay_cls, relay_fn } = shaped;
        match facts.kind {
            SubjectKind::WorldObject | SubjectKind::Record => {}
            SubjectKind::DoorOrContainer => return Verdict::Skip,
            SubjectKind::Actor => return refuse(Code::Row, "the subject is an actor: its animations are the action and animation streams' (the row is local for an actor)".to_owned()),
            SubjectKind::Unknown => return refuse(Code::Owner, "the server holds no such reference".to_owned()),
        }
        // a placement the server holds no reference for says so in each
        // refusal, so a reader asks its record's cell, not the loader
        let placed = if facts.kind == SubjectKind::Record { BY_RECORD } else { "" };
        if facts.cell == 0 {
            return refuse(Code::Owner, format!("the subject's cell is unknown to the server{placed}"));
        }
        if facts.owner == 0 {
            return refuse(Code::Owner, format!("nobody hosts cell {:x}{placed}", facts.cell));
        }
        if facts.owner != facts.sender {
            return refuse(Code::Owner, format!("{:x} hosts cell {:x}, not the reporter{placed}", facts.owner, facts.cell));
        }
        if facts.owner_epoch != facts.epoch {
            return refuse(Code::Owner, format!("cell {:x} stands at epoch {}, the report names {}{placed}", facts.cell, facts.owner_epoch, facts.epoch));
        }
        let seq = self.seqs.entry(subject).or_insert(0);
        *seq = seq.saturating_add(1);
        Verdict::Accept(Accept {
            subject,
            seq: *seq,
            relay_cls,
            relay_fn,
            relay_args: row.relay_args(),
            relay_self: if row.subject == Subject::SelfRef { subject } else { 0 },
            audience: row.audience,
            keep: row.keep,
            graph_event_arg: row.graph_event_arg(),
        })
    }

    /// Both steps on one call in the core's order, the shape step and then
    /// the owner step on a call it passed, for the tests, whose facts are at
    /// hand before the call (the core gathers them only for a shaped call).
    #[cfg(test)]
    fn judge(&mut self, row: Option<&Row>, args: &[Arg], facts: &Facts, now_ms: u64) -> Verdict {
        match self.shape(row, args, facts.sender, facts.self_id, now_ms) {
            Ok(shaped) => self.owner(shaped, facts),
            Err(refused) => Verdict::Refuse(refused),
        }
    }

    /// The subject's sequence: its accepted calls so far.
    #[must_use]
    pub fn seq_of(&self, subject: u32) -> u32 {
        self.seqs.get(&subject).copied().unwrap_or(0)
    }
}

#[cfg(test)]
#[allow(clippy::panic)] // a test's failure message, never a packet's path
mod tests {
    use super::*;

    // Bleak Falls Temple 0x371de's puzzle pillar 0xaa8cb (docs/verbs/
    // traps.md), its host's actor and a guest's
    const PILLAR: u32 = 0xaa8cb;
    const TEMPLE: u32 = 0x371de;
    const HOST: u32 = 0xff00_0000;
    const GUEST: u32 = 0xff00_0001;

    // the platform's copy of the table (skyrim-platform's NativeRows.inc,
    // hand-written until thuum's ledger generates both) holds the same rows in
    // the same order with the same kind, subject and relay: a cheap guard
    // until one generator writes both files (rung 2's review, finding 9)
    #[test]
    fn the_platform_table_matches_this_one() {
        let inc = include_str!("../../../../skyrim-platform/src/platform_se/skyrim_platform/NativeRows.inc");
        let rows: Vec<&str> = inc.lines().map(str::trim).filter(|l| l.starts_with('{')).collect();
        assert_eq!(rows.len(), TABLE.len(), "row count");
        for (line, row) in rows.iter().zip(TABLE.iter()) {
            let fields: Vec<&str> = line.trim_start_matches('{').trim_end_matches(['}', ',', ' ']).split(',').map(str::trim).collect();
            let [cls, func, kind_field, subject_field, relay_field] = fields.as_slice() else {
                panic!("five fields: {line}");
            };
            assert_eq!(*cls, format!("\"{}\"", row.cls), "{line}");
            assert_eq!(*func, format!("\"{}\"", row.func), "{line}");
            let kind = match row.kind {
                Kind::Action => "NativeKind::Action",
                Kind::State => "NativeKind::State",
                Kind::Everyone => "NativeKind::Everyone",
                Kind::Local => "NativeKind::Local",
            };
            assert_eq!(*kind_field, kind, "{line}");
            let subject = match row.subject {
                Subject::SelfRef => "NativeSubject::Self",
                Subject::Arg0 => "NativeSubject::Arg0",
                Subject::Nothing => "NativeSubject::None",
            };
            assert_eq!(*subject_field, subject, "{line}");
            let relay = match row.relay {
                Relay::Same => "\"same\"".to_string(),
                Relay::Twin { cls, func, .. } => format!("\"twin:{cls}.{func}\""),
                Relay::Never => "\"none\"".to_string(),
            };
            assert_eq!(*relay_field, relay, "{line}");
        }
    }

    fn hosted(self_id: u32) -> Facts {
        Facts { sender: HOST, self_id, epoch: 3, kind: SubjectKind::WorldObject, cell: TEMPLE, owner: HOST, owner_epoch: 3 }
    }

    fn accepted(v: &Verdict) -> Accept {
        match v {
            Verdict::Accept(a) => *a,
            other => panic!("not accepted: {other:?}"),
        }
    }

    fn code(v: &Verdict) -> Option<Code> {
        match v {
            Verdict::Refuse(r) => Some(r.code),
            _ => None,
        }
    }

    #[test]
    fn the_hosts_animation_is_relayed_with_the_pillars_next_sequence() {
        let mut s = ScriptCalls::default();
        let row = row("ObjectReference", "PlayAnimation");
        let first = accepted(&s.judge(row, &[Arg::String], &hosted(PILLAR), 0));
        assert_eq!((first.subject, first.seq, first.relay_self), (PILLAR, 1, PILLAR));
        assert_eq!((first.relay_cls, first.relay_fn, first.relay_args), ("ObjectReference", "PlayAnimation", &[0u8][..]));
        assert_eq!((first.audience, first.keep, first.graph_event_arg), (Audience::Listeners, Keep::Last, Some(0)));
        assert_eq!(accepted(&s.judge(row, &[Arg::String], &hosted(PILLAR), 10)).seq, 2);
        assert_eq!(s.seq_of(PILLAR), 2);
        // the sequence is per object: another subject starts at 1
        assert_eq!(accepted(&s.judge(row, &[Arg::String], &hosted(0xaa8cc), 20)).seq, 1);
        assert_eq!(s.seq_of(0xaa8cd), 0);
    }

    #[test]
    fn a_latent_row_is_relayed_as_its_twin() {
        // PlayAnimationAndWait("Trigger02", "Turned02") as PlayAnimation("Trigger02")
        let mut s = ScriptCalls::default();
        let a = accepted(&s.judge(row("ObjectReference", "PlayAnimationAndWait"), &[Arg::String, Arg::String], &hosted(PILLAR), 0));
        assert_eq!((a.relay_cls, a.relay_fn, a.relay_args), ("ObjectReference", "PlayAnimation", &[0u8][..]));
        assert_eq!((a.keep, a.graph_event_arg, a.seq), (Keep::Last, Some(0), 1));
        // the names compare without case, as Papyrus's do
        assert!(matches!(s.judge(row("objectreference", "playanimationandwait"), &[Arg::String, Arg::String], &hosted(PILLAR), 0), Verdict::Accept(_)));
    }

    #[test]
    fn a_static_row_is_relayed_with_no_self_its_target_first() {
        // Debug.SendAnimationEvent(akRef, asEvent): the subject is the target,
        // named again as selfId; the relay is static (self 0), the arguments
        // unchanged, the sequence the target's
        let mut s = ScriptCalls::default();
        let row = row("Debug", "SendAnimationEvent");
        let args = [Arg::Object(u64::from(PILLAR)), Arg::String];
        let a = accepted(&s.judge(row, &args, &hosted(PILLAR), 0));
        assert_eq!((a.subject, a.relay_self, a.seq), (PILLAR, 0, 1));
        assert_eq!((a.relay_cls, a.relay_fn, a.relay_args), ("Debug", "SendAnimationEvent", &[0u8, 1][..]));
        assert_eq!((a.keep, a.graph_event_arg), (Keep::Nothing, Some(1)));
        // one sequence per object across its rows
        assert_eq!(accepted(&s.judge(super::row("ObjectReference", "PlayAnimation"), &[Arg::String], &hosted(PILLAR), 0)).seq, 2);
        // a static call's selfId is its target: 0 or another form is refused
        assert_eq!(code(&s.judge(row, &args, &hosted(0), 0)), Some(Code::Args));
        assert_eq!(code(&s.judge(row, &args, &hosted(0xaa8cc), 0)), Some(Code::Args));
        // the target must be an object the reporter may name
        assert_eq!(code(&s.judge(row, &[Arg::Absent, Arg::String], &hosted(PILLAR), 0)), Some(Code::Args));
        assert_eq!(code(&s.judge(row, &[Arg::Object(0xff00_0002), Arg::String], &hosted(PILLAR), 0)), Some(Code::Args));
    }

    #[test]
    fn a_graph_variable_row_relays_as_itself_and_keeps_nothing() {
        let mut s = ScriptCalls::default();
        for (func, value) in [("SetAnimationVariableBool", Arg::Bool), ("SetAnimationVariableInt", Arg::Number), ("SetAnimationVariableFloat", Arg::Number)] {
            let a = accepted(&s.judge(row("ObjectReference", func), &[Arg::String, value], &hosted(PILLAR), 0));
            assert_eq!((a.relay_fn, a.relay_args, a.keep, a.graph_event_arg), (func, &[0u8, 1][..], Keep::Nothing, None), "{func}");
        }
        assert_eq!(code(&s.judge(row("ObjectReference", "SetAnimationVariableBool"), &[Arg::String, Arg::Number], &hosted(PILLAR), 0)), Some(Code::Args));
    }

    #[test]
    fn a_pair_that_is_no_relayed_action_row_is_refused_by_its_row() {
        let mut s = ScriptCalls::default();
        let f = hosted(PILLAR);
        assert_eq!(code(&s.judge(row("Debug", "Notification"), &[Arg::String], &f, 0)), Some(Code::Row));
        assert_eq!(code(&s.judge(None, &[], &f, 0)), Some(Code::Row));
        // the flip's rows: state and everyone, tallied at rung 2
        assert_eq!(code(&s.judge(row("ObjectReference", "Enable"), &[Arg::Bool], &f, 0)), Some(Code::Row));
        assert_eq!(code(&s.judge(row("Quest", "SetCurrentStageID"), &[Arg::Number], &f, 0)), Some(Code::Row));
        assert_eq!(code(&s.judge(row("Game", "GetPlayer"), &[], &f, 0)), Some(Code::Row));
        // an actor's animation is the streams', the player's own among them
        assert_eq!(code(&s.judge(row("ObjectReference", "PlayAnimation"), &[Arg::String], &Facts { kind: SubjectKind::Actor, ..f }, 0)), Some(Code::Row));
        assert_eq!(s.seq_of(PILLAR), 0);
    }

    #[test]
    fn the_arguments_are_the_rows_count_and_kinds() {
        let mut s = ScriptCalls::default();
        let row = row("ObjectReference", "PlayAnimation");
        let f = hosted(PILLAR);
        assert_eq!(code(&s.judge(row, &[], &f, 0)), Some(Code::Args));
        assert_eq!(code(&s.judge(row, &[Arg::String, Arg::String], &f, 0)), Some(Code::Args));
        assert_eq!(code(&s.judge(row, &[Arg::Number], &f, 0)), Some(Code::Args));
        assert_eq!(code(&s.judge(row, &[Arg::Absent], &f, 0)), Some(Code::Args), "an unread string cannot be relayed");
        // a method row's self: never 0, never one the server names
        assert_eq!(code(&s.judge(row, &[Arg::String], &hosted(0), 0)), Some(Code::Args));
        assert_eq!(code(&s.judge(row, &[Arg::String], &hosted(0xff00_0002), 0)), Some(Code::Args));
    }

    #[test]
    fn a_door_or_a_container_is_dropped_quietly_and_takes_no_sequence() {
        let mut s = ScriptCalls::default();
        let f = Facts { kind: SubjectKind::DoorOrContainer, ..hosted(0x371e9) };
        assert_eq!(s.judge(row("ObjectReference", "PlayAnimation"), &[Arg::String], &f, 0), Verdict::Skip);
        assert_eq!(s.seq_of(0x371e9), 0);
    }

    #[test]
    fn only_the_subjects_cell_host_at_its_epoch_is_relayed() {
        let mut s = ScriptCalls::default();
        let row = row("ObjectReference", "PlayAnimation");
        let f = hosted(PILLAR);
        assert_eq!(code(&s.judge(row, &[Arg::String], &Facts { sender: GUEST, ..f }, 0)), Some(Code::Owner));
        assert_eq!(code(&s.judge(row, &[Arg::String], &Facts { owner: 0, ..f }, 0)), Some(Code::Owner));
        assert_eq!(code(&s.judge(row, &[Arg::String], &Facts { cell: 0, owner: 0, ..f }, 0)), Some(Code::Owner));
        assert_eq!(code(&s.judge(row, &[Arg::String], &Facts { kind: SubjectKind::Unknown, ..f }, 0)), Some(Code::Owner));
        // a stale epoch: the cell passed on and came back since the report
        assert_eq!(code(&s.judge(row, &[Arg::String], &Facts { epoch: 2, ..f }, 0)), Some(Code::Owner));
        assert_eq!(code(&s.judge(row, &[Arg::String], &Facts { epoch: 0, ..f }, 0)), Some(Code::Owner));
        assert_eq!(s.seq_of(PILLAR), 0);
        assert!(matches!(s.judge(row, &[Arg::String], &f, 0), Verdict::Accept(_)));
        // the words name the facts
        match s.judge(row, &[Arg::String], &Facts { sender: GUEST, ..f }, 0) {
            Verdict::Refuse(r) => assert_eq!(r.why, "ff000000 hosts cell 371de, not the reporter"),
            other => panic!("a guest's report is refused: {other:?}"),
        }
    }

    // Bleak Falls Sanctum 0x371dd's door FX 0x5256c (base RuinsPuzzleDoorFX
    // 0x52172, an MSTT the loader never attaches; lab/esm.py, 2026-11-05),
    // which HallofStoriesKeyholeScript.openDoor plays FXDoorOpen on (rung 2's
    // merged-head review, the hall door's FX)
    const DOOR_FX: u32 = 0x5256c;
    const SANCTUM: u32 = 0x371dd;

    #[test]
    fn a_placement_the_server_does_not_load_is_judged_by_its_records_cell() {
        let mut s = ScriptCalls::default();
        let row = row("ObjectReference", "PlayAnimation");
        let f = Facts { kind: SubjectKind::Record, cell: SANCTUM, ..hosted(DOOR_FX) };
        let refused = |v: Verdict| match v {
            Verdict::Refuse(r) => (r.code, r.why),
            other => panic!("not refused: {other:?}"),
        };
        // its cell's host at the epoch: relayed as a held world object is
        let a = accepted(&s.judge(row, &[Arg::String], &f, 0));
        assert_eq!((a.subject, a.seq, a.relay_self), (DOOR_FX, 1, DOOR_FX));
        assert_eq!((a.relay_cls, a.relay_fn, a.relay_args), ("ObjectReference", "PlayAnimation", &[0u8][..]));
        assert_eq!((a.audience, a.keep, a.graph_event_arg), (Audience::Listeners, Keep::Last, Some(0)));
        // a reporter that does not host the record's cell at the epoch is
        // refused, the words naming the case
        assert_eq!(refused(s.judge(row, &[Arg::String], &Facts { sender: GUEST, ..f }, 0)), (Code::Owner, format!("ff000000 hosts cell 371dd, not the reporter{BY_RECORD}")));
        assert_eq!(refused(s.judge(row, &[Arg::String], &Facts { owner: 0, ..f }, 0)), (Code::Owner, format!("nobody hosts cell 371dd{BY_RECORD}")));
        assert_eq!(refused(s.judge(row, &[Arg::String], &Facts { epoch: 2, ..f }, 0)), (Code::Owner, format!("cell 371dd stands at epoch 3, the report names 2{BY_RECORD}")));
        assert_eq!(refused(s.judge(row, &[Arg::String], &Facts { cell: 0, owner: 0, ..f }, 0)), (Code::Owner, format!("the subject's cell is unknown to the server{BY_RECORD}")));
        // no placement by a record (another record type, an actor's ACHR, no
        // record): today's refusal, its words unchanged
        assert_eq!(refused(s.judge(row, &[Arg::String], &Facts { kind: SubjectKind::Unknown, ..f }, 0)), (Code::Owner, "the server holds no such reference".to_owned()));
        // no refusal took a sequence, and the object's one sequence runs on
        // across its rows (the static row names it as its target)
        assert_eq!(accepted(&s.judge(row, &[Arg::String], &f, 0)).seq, 2);
        let send = super::row("Debug", "SendAnimationEvent");
        let target = accepted(&s.judge(send, &[Arg::Object(u64::from(DOOR_FX)), Arg::String], &f, 0));
        assert_eq!((target.subject, target.relay_self, target.seq), (DOOR_FX, 0, 3));
        assert_eq!(s.seq_of(DOOR_FX), 3);
    }

    #[test]
    fn a_reporter_sends_thirty_two_at_once_then_eight_a_second() {
        let mut s = ScriptCalls::default();
        let row = row("ObjectReference", "PlayAnimation");
        let f = hosted(PILLAR);
        for _ in 0..BURST {
            assert!(matches!(s.judge(row, &[Arg::String], &f, 1_000), Verdict::Accept(_)));
        }
        assert_eq!(code(&s.judge(row, &[Arg::String], &f, 1_000)), Some(Code::Rate));
        assert_eq!(code(&s.judge(row, &[Arg::String], &f, 1_124)), Some(Code::Rate));
        // one token every 125 ms
        assert!(matches!(s.judge(row, &[Arg::String], &f, 1_125), Verdict::Accept(_)));
        assert_eq!(code(&s.judge(row, &[Arg::String], &f, 1_125)), Some(Code::Rate));
        assert_eq!(s.seq_of(PILLAR), BURST + 1, "a refused call takes no sequence");
        // the budget is the reporter's: another reporter's is full
        assert_eq!(code(&s.judge(row, &[Arg::String], &Facts { sender: GUEST, ..f }, 1_125)), Some(Code::Owner));
        // and a second's rest refills eight
        for _ in 0..PER_SECOND {
            assert!(matches!(s.judge(row, &[Arg::String], &f, 2_125), Verdict::Accept(_)));
        }
        assert_eq!(code(&s.judge(row, &[Arg::String], &f, 2_125)), Some(Code::Rate));
        // a shape refusal spends a token as an accepted call does (the hall
        // door's FX review, finding 1): a burst of malformed calls leaves
        // none for a well-formed one
        let mut fresh = ScriptCalls::default();
        for _ in 0..BURST {
            assert_eq!(code(&fresh.judge(row, &[], &f, 0)), Some(Code::Args));
        }
        assert_eq!(code(&fresh.judge(row, &[Arg::String], &f, 0)), Some(Code::Rate));
    }

    #[test]
    fn the_codes_have_their_names() {
        assert_eq!(Code::Row.name(), "E_SCRIPTCALL_ROW");
        assert_eq!(Code::Args.name(), "E_SCRIPTCALL_ARGS");
        assert_eq!(Code::Owner.name(), "E_SCRIPTCALL_OWNER");
        assert_eq!(Code::Rate.name(), "E_SCRIPTCALL_RATE");
    }

    #[test]
    fn the_subject_stands_where_the_row_says() {
        let play = row("ObjectReference", "PlayAnimation");
        let send = row("Debug", "SendAnimationEvent");
        assert_eq!(play.and_then(|r| subject_of(r, PILLAR, &[Arg::String])), Some(PILLAR));
        assert_eq!(play.and_then(|r| subject_of(r, 0x14, &[Arg::String])), Some(0x14), "the reporter's own player");
        assert_eq!(play.and_then(|r| subject_of(r, 0, &[Arg::String])), None);
        assert_eq!(send.and_then(|r| subject_of(r, 0, &[Arg::Object(u64::from(PILLAR)), Arg::String])), Some(PILLAR));
        assert_eq!(send.and_then(|r| subject_of(r, 0, &[Arg::Object(0x1_0000_0000), Arg::String])), None);
        assert_eq!(send.and_then(|r| subject_of(r, 0, &[Arg::String, Arg::String])), None);
    }

    #[test]
    fn the_table_is_rung_twos_and_holds_together() {
        // the six action rows, the flip's twenty-four, Game.GetPlayer
        assert_eq!(TABLE.len(), 31);
        let actions: Vec<&str> = TABLE.iter().filter(|r| r.kind == Kind::Action).map(|r| r.func).collect();
        assert_eq!(actions, ["PlayAnimation", "PlayAnimationAndWait", "SendAnimationEvent", "SetAnimationVariableBool", "SetAnimationVariableInt", "SetAnimationVariableFloat"]);
        for (i, r) in TABLE.iter().enumerate() {
            let name = format!("{}.{}", r.cls, r.func);
            // one row per pair, names compared without case
            assert_eq!(row_index(r.cls, r.func), Some(i), "{name}");
            assert!(r.params.len() <= EVERY_ARG.len(), "{name}");
            // an action row relays to the subject's listeners, its twin's
            // arguments among the call's; every other row is never relayed
            if r.kind == Kind::Action {
                assert!(r.relay_target().is_some() && r.audience == Audience::Listeners && r.owner == Owner::CellHost, "{name}");
                assert!(r.relay_args().iter().all(|&a| usize::from(a) < r.params.len()), "{name}");
            } else {
                assert_eq!((r.relay, r.keep), (Relay::Never, Keep::Nothing), "{name}");
            }
            // a static native's subject is its first argument, an object
            if r.subject == Subject::Arg0 {
                assert_eq!(r.params.first(), Some(&Param::Object), "{name}");
            }
            // a kept row keeps a graph event
            if r.keep == Keep::Last {
                assert!(r.graph_event_arg().is_some(), "{name}");
            }
        }
        let everyone: Vec<String> = TABLE.iter().filter(|r| r.kind == Kind::Everyone).map(|r| format!("{}.{}", r.cls, r.func)).collect();
        assert_eq!(everyone, ["Quest.SetCurrentStageID", "Quest.Start", "Quest.Stop", "GlobalVariable.SetValue", "ReferenceAlias.ForceRefTo"]);
        assert_eq!(TABLE.iter().filter(|r| r.kind == Kind::State).count(), 19);
        assert_eq!(row("Game", "GetPlayer").map(|r| r.kind), Some(Kind::Local));
    }
}
