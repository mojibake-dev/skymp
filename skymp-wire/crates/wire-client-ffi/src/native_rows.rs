//! The natives table as Skyrim Platform's seam reads it (thuum ADR-073 step
//! 4, Eli's D-NT1 C): wire-rules' TABLE, which natives-gen writes from
//! FunctionsDump.txt and its overlay, exported through this crate's generated
//! header (skymp_wire.h: `just wire-header`, checked by `just
//! wire-header-check`), the route D-L3 A takes for ADR-073's R0 kinds. The
//! platform sizes its per-row slots by [`SKYMP_NATIVE_ROWS`] at build time
//! and reads the rows once at kDataLoaded through [`SkympNativeRows`], so it
//! keeps no copy of its own: NativeRows.inc and its parity test retire.
//!
//! The codes below are the platform's: a kind's and a subject's are its
//! NativeKind's and NativeSubject's values (ScriptNatives.cpp checks them at
//! compile time), and a record kind's is wire-rules' `RecordKind::code`, the
//! bridge's number.

use std::ffi::{c_char, CString};
use std::sync::OnceLock;

use wire_rules::natives::{Audience, Keep, Kind, Look, Owner, Param, Relay, Subject, TABLE};

use crate::{guard, log};

/// The table's row count: the platform's per-row slots are this many, and a
/// plugin whose table holds another count binds no row there. The compiler
/// checks it against the table; after natives-gen changes the count, set it
/// here and run `just wire-header`.
pub const SKYMP_NATIVE_ROWS: usize = 1408;
const _: () = assert!(
    SKYMP_NATIVE_ROWS == TABLE.len(),
    "SKYMP_NATIVE_ROWS is wire-rules' TABLE.len()"
);

/// A row's parameters: at most twelve, the dump's longest list (the
/// platform's own TESModPlatform.Add and AddItemEx). A game native takes at
/// most eleven (CommonLibSSE-NG src/RE/N/NativeFunctionBase.cpp:15, "native
/// supports max 11"), which the platform reads of a frame.
pub const SKYMP_NATIVE_MAX_PARAMS: usize = 12;

/// Kind: the call itself is the message, relayed.
pub const SKYMP_NATIVE_KIND_ACTION: u8 = 0;
/// Kind: a saved consequence, carried by its record or its verb.
pub const SKYMP_NATIVE_KIND_STATE: u8 = 1;
/// Kind: nobody's, every seat runs it.
pub const SKYMP_NATIVE_KIND_EVERYONE: u8 = 2;
/// Kind: never sent.
pub const SKYMP_NATIVE_KIND_LOCAL: u8 = 3;
/// Kind: a creation (a reference placed, a scene started), judged as a state
/// row until ADR-073 decision 13's placement.
pub const SKYMP_NATIVE_KIND_CREATE: u8 = 4;

/// Subject: the call's self.
pub const SKYMP_NATIVE_SUBJECT_SELF: u8 = 0;
/// Subject: a static's first argument.
pub const SKYMP_NATIVE_SUBJECT_ARG0: u8 = 1;
/// Subject: none.
pub const SKYMP_NATIVE_SUBJECT_NONE: u8 = 2;
/// Subject: a method's first argument, its self another form.
pub const SKYMP_NATIVE_SUBJECT_ARG0_METHOD: u8 = 3;

/// Owner: asked at the call (ADR-073 decision 6; the platform's OwnedHere and
/// OwnedForRecord until step 5's owner_of).
pub const SKYMP_NATIVE_OWNER_ASKED: u8 = 0;
/// Owner: the seat that created the subject or started the scene.
pub const SKYMP_NATIVE_OWNER_CREATOR: u8 = 1;
/// Owner: the seat's own game.
pub const SKYMP_NATIVE_OWNER_PLAYER: u8 = 2;
/// Owner: nobody.
pub const SKYMP_NATIVE_OWNER_NOBODY: u8 = 3;

/// Audience: the subject's listeners.
pub const SKYMP_NATIVE_AUDIENCE_LISTENERS: u8 = 0;
/// Audience: every seat.
pub const SKYMP_NATIVE_AUDIENCE_EVERYONE: u8 = 1;
/// Audience: no seat.
pub const SKYMP_NATIVE_AUDIENCE_NOBODY: u8 = 2;
/// Audience: the subject's cell's host alone.
pub const SKYMP_NATIVE_AUDIENCE_HOST: u8 = 3;

/// Keep: nothing.
pub const SKYMP_NATIVE_KEEP_NOTHING: u8 = 0;
/// Keep: the call's graph event as the subject's lastAnimation.
pub const SKYMP_NATIVE_KEEP_LAST: u8 = 1;
/// Keep: the call as its subject's evidence.
pub const SKYMP_NATIVE_KEEP_EVIDENCE: u8 = 2;
/// Keep: the call as its subject's record (the row's `record` names which).
pub const SKYMP_NATIVE_KEEP_RECORD: u8 = 3;
/// Keep: the call's graph variable on its subject.
pub const SKYMP_NATIVE_KEEP_VARIABLE: u8 = 4;

/// Look: never.
pub const SKYMP_NATIVE_LOOK_NEVER: u8 = 0;
/// Look: its own channel first, then the look channel.
pub const SKYMP_NATIVE_LOOK_ALSO: u8 = 1;
/// Look: the look channel alone.
pub const SKYMP_NATIVE_LOOK_ONLY: u8 = 2;

/// Parameter: Bool.
pub const SKYMP_NATIVE_PARAM_BOOL: u8 = 0;
/// Parameter: Int.
pub const SKYMP_NATIVE_PARAM_INT: u8 = 1;
/// Parameter: Float.
pub const SKYMP_NATIVE_PARAM_FLOAT: u8 = 2;
/// Parameter: String.
pub const SKYMP_NATIVE_PARAM_STRING: u8 = 3;
/// Parameter: an object.
pub const SKYMP_NATIVE_PARAM_OBJECT: u8 = 4;
/// Parameter: an array of Bools.
pub const SKYMP_NATIVE_PARAM_BOOL_ARRAY: u8 = 5;
/// Parameter: an array of Ints.
pub const SKYMP_NATIVE_PARAM_INT_ARRAY: u8 = 6;
/// Parameter: an array of Floats.
pub const SKYMP_NATIVE_PARAM_FLOAT_ARRAY: u8 = 7;
/// Parameter: an array of Strings.
pub const SKYMP_NATIVE_PARAM_STRING_ARRAY: u8 = 8;
/// Parameter: an array of objects.
pub const SKYMP_NATIVE_PARAM_OBJECT_ARRAY: u8 = 9;

/// Hook: nothing beyond the row's other columns.
pub const SKYMP_NATIVE_HOOK_NONE: u8 = 0;
/// Hook: Actor.Kill and KillSilent, batched for the death report.
pub const SKYMP_NATIVE_HOOK_DEATH_REPORT: u8 = 1;
/// Hook: SetDestroyed on a trap family reference, batched.
pub const SKYMP_NATIVE_HOOK_TRAP_DESTROYED: u8 = 2;
/// Hook: SetMotionType on a trap family body (its motion type the neutral
/// argument).
pub const SKYMP_NATIVE_HOOK_TRAP_MOTION: u8 = 3;
/// Hook: ApplyHavokImpulse on a trap family body (its magnitude the neutral
/// argument).
pub const SKYMP_NATIVE_HOOK_TRAP_IMPULSE: u8 = 4;
/// Hook: an alias fill, batched on every seat.
pub const SKYMP_NATIVE_HOOK_ALIAS_FILL: u8 = 5;
/// Hook: Game.GetPlayer's result swapped inside an acting scope.
pub const SKYMP_NATIVE_HOOK_ACTING_RETURN: u8 = 6;
/// Hook: a quest's stage, carrying the acting scope and its cause.
pub const SKYMP_NATIVE_HOOK_QUEST_STAGE: u8 = 7;
/// Hook: a quest's start, likewise.
pub const SKYMP_NATIVE_HOOK_QUEST_START: u8 = 8;
/// Hook: a quest's stop, likewise.
pub const SKYMP_NATIVE_HOOK_QUEST_STOP: u8 = 9;
/// Hook: a global's write, skipped on a player's own inside another's scope.
pub const SKYMP_NATIVE_HOOK_PLAYER_GLOBAL: u8 = 10;
/// Hook: a placement that may make this seat's local helper.
pub const SKYMP_NATIVE_HOOK_EFFECT_HELPER: u8 = 11;
/// Hook: a deletion that lets a local helper go.
pub const SKYMP_NATIVE_HOOK_HELPER_DELETE: u8 = 12;
/// Hook: an AddItem whose leveled roll is parked until it lands.
pub const SKYMP_NATIVE_HOOK_LEVELED_LANDING: u8 = 13;
/// Hook: a scene's start, counted inside a scope rooted at a relay.
pub const SKYMP_NATIVE_HOOK_SCENE_START: u8 = 14;
/// The largest hook code: a row with a larger one is a table this build does
/// not read.
pub const SKYMP_NATIVE_HOOK_LAST: u8 = 14;

/// One row of the table, every string NUL-terminated and valid for the
/// process's life.
#[repr(C)]
pub struct SkympNativeRow {
    /// The native's Papyrus class, as the table spells it.
    pub cls: *const c_char,
    /// The native's name.
    pub func: *const c_char,
    /// Its kind (`SKYMP_NATIVE_KIND_*`).
    pub kind: u8,
    /// Its subject (`SKYMP_NATIVE_SUBJECT_*`).
    pub subject: u8,
    /// Its subject's owner (`SKYMP_NATIVE_OWNER_*`).
    pub owner: u8,
    /// Who hears an accepted call (`SKYMP_NATIVE_AUDIENCE_*`).
    pub audience: u8,
    /// What a receiver runs: "same", "twin:<Class.Name>" or "none".
    pub relay: *const c_char,
    /// What the server keeps (`SKYMP_NATIVE_KEEP_*`).
    pub keep: u8,
    /// For a record keep, its record kind's code (1 the ref state, 2 the
    /// standing, 3 a placement, 4 a player's own, 5 a quest's map marker);
    /// 0 for any other keep.
    pub record: u8,
    /// Whether a call may cross as a firing's look (`SKYMP_NATIVE_LOOK_*`).
    pub look: u8,
    /// How many of `params` are its parameters.
    pub param_count: u8,
    /// Its parameters' types in order (`SKYMP_NATIVE_PARAM_*`).
    pub params: [u8; SKYMP_NATIVE_MAX_PARAMS],
    /// What the seam does with its call beyond the rest (`SKYMP_NATIVE_HOOK_*`,
    /// wire-rules' `Hook::code`).
    pub hook: u8,
    /// 1 for a latent native (FunctionsDump.txt's isLatent), which the
    /// platform's install check holds against the VM's binding; 0 otherwise.
    pub latent: u8,
    /// 1 for a staged row (thuum ADR-073 step 4's S5: a derived row, counted
    /// by what it would do and run as before); 0 for a live row.
    pub staged: u8,
}

fn kind_code(kind: Kind) -> u8 {
    match kind {
        Kind::Action => SKYMP_NATIVE_KIND_ACTION,
        Kind::State => SKYMP_NATIVE_KIND_STATE,
        Kind::Everyone => SKYMP_NATIVE_KIND_EVERYONE,
        Kind::Local => SKYMP_NATIVE_KIND_LOCAL,
        Kind::Create => SKYMP_NATIVE_KIND_CREATE,
    }
}

fn subject_code(subject: Subject) -> u8 {
    match subject {
        Subject::SelfRef => SKYMP_NATIVE_SUBJECT_SELF,
        Subject::Arg0 => SKYMP_NATIVE_SUBJECT_ARG0,
        Subject::Nothing => SKYMP_NATIVE_SUBJECT_NONE,
        Subject::Arg0Method => SKYMP_NATIVE_SUBJECT_ARG0_METHOD,
    }
}

fn owner_code(owner: Owner) -> u8 {
    match owner {
        Owner::Asked => SKYMP_NATIVE_OWNER_ASKED,
        Owner::Creator => SKYMP_NATIVE_OWNER_CREATOR,
        Owner::Player => SKYMP_NATIVE_OWNER_PLAYER,
        Owner::Nobody => SKYMP_NATIVE_OWNER_NOBODY,
    }
}

fn audience_code(audience: Audience) -> u8 {
    match audience {
        Audience::Listeners => SKYMP_NATIVE_AUDIENCE_LISTENERS,
        Audience::Everyone => SKYMP_NATIVE_AUDIENCE_EVERYONE,
        Audience::Nobody => SKYMP_NATIVE_AUDIENCE_NOBODY,
        Audience::Host => SKYMP_NATIVE_AUDIENCE_HOST,
    }
}

fn keep_codes(keep: Keep) -> (u8, u8) {
    match keep {
        Keep::Nothing => (SKYMP_NATIVE_KEEP_NOTHING, 0),
        Keep::Last => (SKYMP_NATIVE_KEEP_LAST, 0),
        Keep::Evidence => (SKYMP_NATIVE_KEEP_EVIDENCE, 0),
        Keep::Record(kind) => (SKYMP_NATIVE_KEEP_RECORD, kind.code()),
        Keep::Variable => (SKYMP_NATIVE_KEEP_VARIABLE, 0),
    }
}

fn look_code(look: Look) -> u8 {
    match look {
        Look::Never => SKYMP_NATIVE_LOOK_NEVER,
        Look::Also => SKYMP_NATIVE_LOOK_ALSO,
        Look::Only => SKYMP_NATIVE_LOOK_ONLY,
    }
}

fn param_code(param: Param) -> u8 {
    match param {
        Param::Bool => SKYMP_NATIVE_PARAM_BOOL,
        Param::Int => SKYMP_NATIVE_PARAM_INT,
        Param::Float => SKYMP_NATIVE_PARAM_FLOAT,
        Param::String => SKYMP_NATIVE_PARAM_STRING,
        Param::Object => SKYMP_NATIVE_PARAM_OBJECT,
        Param::BoolArray => SKYMP_NATIVE_PARAM_BOOL_ARRAY,
        Param::IntArray => SKYMP_NATIVE_PARAM_INT_ARRAY,
        Param::FloatArray => SKYMP_NATIVE_PARAM_FLOAT_ARRAY,
        Param::StringArray => SKYMP_NATIVE_PARAM_STRING_ARRAY,
        Param::ObjectArray => SKYMP_NATIVE_PARAM_OBJECT_ARRAY,
    }
}

/// A relay as the platform's install line names it.
fn relay_text(relay: Relay) -> String {
    match relay {
        Relay::Same => "same".to_owned(),
        Relay::Twin { cls, func, .. } => format!("twin:{cls}.{func}"),
        Relay::Never => "none".to_owned(),
    }
}

/// The rows and the strings they point into, built once.
struct Rows {
    rows: Vec<SkympNativeRow>,
    _strings: Vec<CString>,
}

// SAFETY: Rows is built once inside ROWS's initializer and never written
// after; its pointers point into the heap buffers of its own CStrings, which
// it owns for the process's life and never frees or moves (moving a CString
// into the Vec moves its handle, not its buffer). Sharing it across threads
// shares reads of immutable memory alone.
unsafe impl Send for Rows {}
// SAFETY: as for Send above: nothing in Rows is ever written after it is
// built.
unsafe impl Sync for Rows {}

static ROWS: OnceLock<Option<Rows>> = OnceLock::new();

/// The table's rows; none when a name holds a NUL or a row has more than
/// [`SKYMP_NATIVE_MAX_PARAMS`] parameters, which natives-gen never writes.
fn build() -> Option<Rows> {
    let mut strings = Vec::new();
    let mut text = |s: String| -> Option<*const c_char> {
        let c = CString::new(s).ok()?;
        let p = c.as_ptr();
        strings.push(c);
        Some(p)
    };
    let mut rows = Vec::with_capacity(TABLE.len());
    for row in TABLE {
        if row.params.len() > SKYMP_NATIVE_MAX_PARAMS {
            return None;
        }
        let mut params = [0u8; SKYMP_NATIVE_MAX_PARAMS];
        for (slot, p) in params.iter_mut().zip(row.params) {
            *slot = param_code(*p);
        }
        let (keep, record) = keep_codes(row.keep);
        rows.push(SkympNativeRow {
            cls: text(row.cls.to_owned())?,
            func: text(row.func.to_owned())?,
            kind: kind_code(row.kind),
            subject: subject_code(row.subject),
            owner: owner_code(row.owner),
            audience: audience_code(row.audience),
            relay: text(relay_text(row.relay))?,
            keep,
            record,
            look: look_code(row.look),
            param_count: u8::try_from(row.params.len()).ok()?,
            params,
            hook: row.hook.code(),
            latent: u8::from(row.latent),
            staged: u8::from(row.staged),
        });
    }
    Some(Rows {
        rows,
        _strings: strings,
    })
}

/// The natives table's rows, valid for the process's life, their count
/// written to `count` when it is not null; null with a count of 0 when the
/// rows could not be built (logged as E_NATIVE_ROWS), which the platform
/// reads as no table.
///
/// # Safety
/// `count` is null or points to a writable `size_t`.
#[no_mangle]
#[allow(non_snake_case)]
pub unsafe extern "C" fn SkympNativeRows(count: *mut usize) -> *const SkympNativeRow {
    let rows = guard("SkympNativeRows", None, || {
        ROWS.get_or_init(build)
            .as_ref()
            .map(|r| (r.rows.as_ptr(), r.rows.len()))
    });
    if rows.is_none() {
        log("E_NATIVE_ROWS: the natives table's rows could not be built");
    }
    let (ptr, len) = rows.unwrap_or((std::ptr::null(), 0));
    if !count.is_null() {
        // SAFETY: the caller guarantees a non-null `count` points to a
        // writable size_t; it is written once and not kept.
        unsafe { count.write(len) };
    }
    ptr
}

#[cfg(test)]
#[allow(clippy::panic)] // a test's failure message, never a packet's path
mod tests {
    use super::*;
    use std::ffi::CStr;
    use wire_rules::natives::Hook;

    fn text(p: *const c_char) -> &'static str {
        assert!(!p.is_null());
        // SAFETY: the row's strings are NUL-terminated and live for the
        // process (ROWS is never dropped).
        unsafe { CStr::from_ptr(p) }
            .to_str()
            .unwrap_or_else(|e| panic!("{e}"))
    }

    #[test]
    fn the_rows_are_the_tables() {
        let mut n = 0usize;
        // SAFETY: `n` is a writable usize for the call.
        let rows = unsafe { SkympNativeRows(&mut n) };
        assert_eq!(n, TABLE.len());
        assert!(!rows.is_null());
        // SAFETY: SkympNativeRows returned `n` rows valid for the process.
        let rows = unsafe { std::slice::from_raw_parts(rows, n) };
        for (r, t) in rows.iter().zip(TABLE) {
            assert_eq!((text(r.cls), text(r.func)), (t.cls, t.func));
            assert_eq!(
                (r.kind, r.subject, r.owner, r.audience),
                (
                    kind_code(t.kind),
                    subject_code(t.subject),
                    owner_code(t.owner),
                    audience_code(t.audience)
                )
            );
            assert_eq!((r.keep, r.record), keep_codes(t.keep));
            assert_eq!(r.look, look_code(t.look));
            assert_eq!(r.hook, t.hook.code());
            assert_eq!((r.latent, r.staged), (u8::from(t.latent), u8::from(t.staged)));
            assert_eq!(usize::from(r.param_count), t.params.len());
            let params: Vec<u8> = t.params.iter().map(|p| param_code(*p)).collect();
            assert_eq!(r.params.get(..t.params.len()), Some(params.as_slice()));
        }
        // the header's hook codes are wire-rules' own
        let codes = [
            SKYMP_NATIVE_HOOK_NONE,
            SKYMP_NATIVE_HOOK_DEATH_REPORT,
            SKYMP_NATIVE_HOOK_TRAP_DESTROYED,
            SKYMP_NATIVE_HOOK_TRAP_MOTION,
            SKYMP_NATIVE_HOOK_TRAP_IMPULSE,
            SKYMP_NATIVE_HOOK_ALIAS_FILL,
            SKYMP_NATIVE_HOOK_ACTING_RETURN,
            SKYMP_NATIVE_HOOK_QUEST_STAGE,
            SKYMP_NATIVE_HOOK_QUEST_START,
            SKYMP_NATIVE_HOOK_QUEST_STOP,
            SKYMP_NATIVE_HOOK_PLAYER_GLOBAL,
            SKYMP_NATIVE_HOOK_EFFECT_HELPER,
            SKYMP_NATIVE_HOOK_HELPER_DELETE,
            SKYMP_NATIVE_HOOK_LEVELED_LANDING,
            SKYMP_NATIVE_HOOK_SCENE_START,
        ];
        assert_eq!(codes.map(usize::from), std::array::from_fn(|i| i));
        assert_eq!(Hook::ALL.map(Hook::code), codes);
        assert_eq!(Some(&SKYMP_NATIVE_HOOK_LAST), codes.last());
        // the install line's relay words, as NativeRows.inc spelled them
        let relays: Vec<&str> = rows.iter().map(|r| text(r.relay)).collect();
        assert_eq!(
            relays.get(..3),
            Some(&["same", "twin:ObjectReference.PlayAnimation", "same"][..])
        );
        assert!(relays
            .iter()
            .all(|r| *r == "same" || *r == "none" || r.starts_with("twin:")));
        // a null count is allowed, and the rows are the same ones
        // SAFETY: a null count is the contract's other case.
        assert_eq!(
            unsafe { SkympNativeRows(std::ptr::null_mut()) },
            rows.as_ptr()
        );
    }
}
