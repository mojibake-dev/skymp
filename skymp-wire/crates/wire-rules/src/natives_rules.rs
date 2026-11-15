//! The native table's rules (thuum ADR-073 step 4, decisions 5, 6, 7 and 11;
//! the step's build spec, section C 2.1, Eli's D-NT1 C to D-NT5 A of
//! 2026-11-15). Every native skyrim-platform's FunctionsDump.txt declares
//! (1,408) gets its row's cells from rules over what the dump says of it: its
//! class's lineage, its name's words, whether it is static or latent, its
//! arguments' and its result's types. The overlay ([`OVERLAY`],
//! natives_overlay.rs) is the only hand data, and each entry is one of
//! decision 5's two admissible kinds: a decided entry citing the ADR row or
//! D-number that sets its cells, or a stopgap naming the rule that replaces
//! it and the condition that deletes it. [`generate`] refuses an entry that
//! names no native, cites nothing, or sets a cell to the rules' own value.
//!
//! The live table ([`crate::natives::TABLE`], natives_table.rs) is the
//! overlay's live entries in the overlay's order: the fifty rows of thuum
//! ADR-068's rungs, each through its decided entry, so the generated table
//! was the hand-kept one byte for byte at step 4's S2, its header aside (the
//! spec's G1; [`table_source`]); since S4 every owner on a subject is the
//! rules' asked one (decision 6), the hook column holds what the platform's
//! name lists held, and train 53's two scene starts follow the fifty, their
//! hook a stopgap until step 5a. Since S5 every other native's row follows
//! them in the dump's order, derived and staged ([`Generated::staged_rows`]),
//! and the table carries its generated O(1) index ([`index_of`]). The platform
//! reads the live rows through wire-client-ffi's generated header (D-NT1 C)
//! and the client through skymp5-client's nativeRows.generated.ts
//! ([`ts_source`]), never a copy. The `natives-gen` tool
//! (src/bin/natives-gen.rs) does the I/O; this module reads no file.
//!
//! The rules, in the order they are tried ([`Rule`]); each says its decision
//! 5 class, and `natives-gen` names each rule once with one native and then
//! counts it (decision 7's merge rule):
//! - K1, a read: a native with a result whose first word asks (Get, Is, Has,
//!   Can, Find, Will, Worn, Calculate) is Local. A stopgap: x-native-effects'
//!   AddChange evidence replaces it (the spec's G6).
//! - K2, the VM's own: a registration (RegisterFor, UnregisterFor), a wait
//!   (Wait, WaitFor), an inventory event filter, a profiling switch, a mod
//!   event. Local, a declared R3 set (decision 5 (a); "VM registrations and
//!   waits", ADR-073's R3 list).
//! - K3, the screen and the sound: a Show, an Open, a Message or a
//!   Notification (Debug.MessageBox's box, the spec's 2.2), or a member of a
//!   Message, an ImageSpaceModifier, a MusicType or a SoundCategory. Local, a
//!   declared R3 set (the same list's camera, screen, menus and music;
//!   ADR-062's "the player's own screen", DECISIONS.md:4031). It no longer
//!   takes a name for containing "Player" (the spec's New 1).
//! - K4, a render: Play, Say, SendAnimationEvent, SetAnimationVariable and
//!   SetSubGraph..Variable, and a Stop on another form's source, on an
//!   object. Action. A stopgap, replaced by x-native-effects.
//! - K5, the seat's own game: a static on no object. State, its owner the
//!   player (New 1: a static no longer means Local, so Game.TeachWord and the
//!   progression statics of ADR-062's row are the player's own state).
//! - K6, a creation: a native whose result is a reference, and Scene's Start
//!   and ForceStart. Create, its owner the creator (decision 6). A stopgap,
//!   replaced by x-native-effects.
//! - K7, nobody's: a member of a quest, an alias or a global (D-F1 C), or of a
//!   form no seat owns. Everyone. A stopgap, replaced by x-native-effects.
//! - K8, an owned object's: everything else, a member of a reference or a
//!   cell, a static on a reference, a method on another form's source. State,
//!   its owner asked of owner_of at the call (decision 6).
//!
//! The other cells follow from the kind. The subject: a static's first
//! argument when it is a reference, else nothing; a reference's or a cell's
//! own self; another form's method's first argument when it is a reference
//! (Sound.Play's shape). The audience: an action's listeners, an everyone
//! row's every seat, else nobody. The relay (ADR-068 decision 4): an action
//! or a state row on a subject, with no array parameter, runs as itself when
//! it is not latent; a latent one runs as its non-latent twin (its name less AndWait, or
//! with NoWait) when the twin takes its leading arguments, and is never
//! relayed without one, whatever its kind. The keep: a created reference's
//! placement record, else nothing. The look: an action's own channel first
//! (ADR-068 D-W2 A's "the action rows"), else never. The scope (New 1):
//! inside another player's acting scope, a non-latent call with no result
//! whose subject is nothing runs on the acting player's seat alone, PQ-3's
//! skip keyed on the subject column instead of a name.

use std::collections::BTreeMap;
use std::fmt;

use serde::Deserialize;

use crate::natives::{self, Audience, Hook, Keep, Kind, Look, Owner, Param, RecordKind, Relay, Subject};

/// One argument of a native as the dump declares it.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Arg {
    /// Its Papyrus name.
    pub name: &'static str,
    /// Its type.
    pub ty: Type,
}

/// A Papyrus type as the dump declares it: its raw type (Bool, Int, Float,
/// String, Object, one of their arrays, or None for a result) and, for an
/// object, the script it names.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Type {
    /// The raw type.
    pub raw: &'static str,
    /// An object's script, none for any other raw type.
    pub object: Option<&'static str>,
}

/// One native as FunctionsDump.txt declares it. Its `offset` is never read:
/// a callback location from whatever build made the dump, no source under
/// thuum's rule 1.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Native {
    /// Its class, as the dump spells it.
    pub cls: &'static str,
    /// Its name, as the dump spells it (Game.GetPlayer is "getplayer").
    pub name: &'static str,
    /// A global (static) native.
    pub is_static: bool,
    /// A latent native.
    pub latent: bool,
    /// Its arguments in order.
    pub args: Vec<Arg>,
    /// Its result.
    pub ret: Type,
}

/// FunctionsDump.txt read: every native in the dump's order (its classes
/// sorted by name, each class's statics before its members), and each
/// class's parent.
#[derive(Debug, Clone)]
pub struct Dump {
    natives: Vec<Native>,
    parents: BTreeMap<String, String>,
}

/// Why a dump was not read.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DumpError {
    /// The text is not the dump's JSON.
    Json(String),
    /// A type no rule knows, by the native that names it.
    UnknownType {
        /// The native, Class.Name.
        native: String,
        /// The raw type.
        raw: String,
    },
}

impl fmt::Display for DumpError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            DumpError::Json(e) => write!(f, "FunctionsDump.txt is not the dump's JSON: {e}"),
            DumpError::UnknownType { native, raw } => {
                write!(f, "{native} names the raw type {raw}, which no rule knows")
            }
        }
    }
}

#[derive(Deserialize)]
struct DumpFile<'a> {
    #[serde(borrow)]
    types: BTreeMap<&'a str, ClassEntry<'a>>,
}

#[derive(Deserialize)]
struct ClassEntry<'a> {
    #[serde(borrow, rename = "globalFunctions")]
    statics: Vec<FunctionEntry<'a>>,
    #[serde(borrow, rename = "memberFunctions")]
    members: Vec<FunctionEntry<'a>>,
    #[serde(borrow, default)]
    parent: Option<&'a str>,
}

#[derive(Deserialize)]
struct FunctionEntry<'a> {
    #[serde(borrow)]
    arguments: Vec<ArgEntry<'a>>,
    #[serde(rename = "isLatent")]
    latent: bool,
    name: &'a str,
    #[serde(borrow, rename = "returnType")]
    ret: TypeEntry<'a>,
}

#[derive(Deserialize)]
struct ArgEntry<'a> {
    name: &'a str,
    #[serde(borrow, rename = "type")]
    ty: TypeEntry<'a>,
}

#[derive(Deserialize)]
struct TypeEntry<'a> {
    #[serde(rename = "rawType")]
    raw: &'a str,
    #[serde(borrow, default, rename = "objectTypeName")]
    object: Option<&'a str>,
}

/// The raw types an argument may have; a result may also be None.
const RAW_TYPES: [&str; 10] = [
    "Bool",
    "Int",
    "Float",
    "String",
    "Object",
    "BoolArray",
    "IntArray",
    "FloatArray",
    "StringArray",
    "ObjectArray",
];

/// Read FunctionsDump.txt's text, borrowed for the program's life (the tool
/// reads the file once; the tests include it).
pub fn parse_dump(text: &'static str) -> Result<Dump, DumpError> {
    let file: DumpFile<'static> =
        serde_json::from_str(text).map_err(|e| DumpError::Json(e.to_string()))?;
    let mut natives = Vec::new();
    let mut parents = BTreeMap::new();
    for (cls, entry) in file.types {
        if let Some(parent) = entry.parent.filter(|p| !p.is_empty()) {
            parents.insert(cls.to_ascii_lowercase(), parent.to_ascii_lowercase());
        }
        for (is_static, functions) in [(true, entry.statics), (false, entry.members)] {
            for f in functions {
                let ty = |t: TypeEntry<'static>| Type {
                    raw: t.raw,
                    object: t.object,
                };
                let native = Native {
                    cls,
                    name: f.name,
                    is_static,
                    latent: f.latent,
                    args: f
                        .arguments
                        .into_iter()
                        .map(|a| Arg {
                            name: a.name,
                            ty: ty(a.ty),
                        })
                        .collect(),
                    ret: ty(f.ret),
                };
                let known = |raw: &str| RAW_TYPES.contains(&raw);
                if let Some(bad) = native
                    .args
                    .iter()
                    .map(|a| a.ty.raw)
                    .find(|r| !known(r))
                    .or(Some(native.ret.raw).filter(|r| *r != "None" && !known(r)))
                {
                    return Err(DumpError::UnknownType {
                        native: format!("{cls}.{}", f.name),
                        raw: bad.to_owned(),
                    });
                }
                natives.push(native);
            }
        }
    }
    Ok(Dump { natives, parents })
}

impl Dump {
    /// Every native, in the dump's order.
    #[must_use]
    pub fn natives(&self) -> &[Native] {
        &self.natives
    }

    /// A class and its ancestors, lower-cased, the class first.
    fn lineage(&self, cls: &str) -> Vec<String> {
        let mut out: Vec<String> = Vec::new();
        let mut next = Some(cls.to_ascii_lowercase());
        while let Some(c) = next.take() {
            if out.contains(&c) {
                break;
            }
            next = self.parents.get(&c).cloned();
            out.push(c);
        }
        out
    }

    fn is_a(&self, cls: &str, ancestor: &str) -> bool {
        self.lineage(cls).iter().any(|c| c == ancestor)
    }

    /// A reference's script (ObjectReference and what extends it).
    fn is_reference(&self, cls: &str) -> bool {
        self.is_a(cls, "objectreference")
    }

    /// The native `name` of `cls` or of an ancestor, the nearest first;
    /// names compare without case.
    fn find(&self, cls: &str, name: &str) -> Option<&Native> {
        self.lineage(cls).iter().find_map(|c| {
            self.natives
                .iter()
                .find(|n| n.cls.eq_ignore_ascii_case(c) && n.name.eq_ignore_ascii_case(name))
        })
    }

    /// The native `cls`.`name` of the dump itself; names compare without case.
    #[must_use]
    pub fn native(&self, cls: &str, name: &str) -> Option<&Native> {
        self.natives
            .iter()
            .find(|n| n.cls.eq_ignore_ascii_case(cls) && n.name.eq_ignore_ascii_case(name))
    }
}

/// What a call does inside another player's acting scope (the spec's New
/// 1).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Scoped {
    /// It runs as the scope has it (the acting scope's swaps).
    Runs,
    /// It runs on the acting player's seat alone: PQ-3's skip, keyed on the
    /// row's subject instead of a name.
    ActingSeatAlone,
}

/// Which rule set a row's kind ([`derive`] tries them in this order).
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub enum Rule {
    /// K1: a read, a native with a result whose first word asks.
    Read,
    /// K2: the VM's own: registrations, waits, filters, profiling, mod events.
    Vm,
    /// K3: the screen and the sound.
    Screen,
    /// K4: a render on an object.
    Render,
    /// K5: a static on no object, the seat's own game.
    OwnGame,
    /// K6: a creation.
    Creates,
    /// K7: nobody's.
    Nobodys,
    /// K8: an owned object's.
    Owned,
}

/// Every rule, in the order [`derive`] tries them.
pub const RULES: [Rule; 8] = [
    Rule::Read,
    Rule::Vm,
    Rule::Screen,
    Rule::Render,
    Rule::OwnGame,
    Rule::Creates,
    Rule::Nobodys,
    Rule::Owned,
];

/// The replacing rule the spec's G6 names for the name-keyed rules.
const BY_EFFECTS: &str = "x-native-effects, the batched AddChange trace (the spec's G6): what the engine saves for a call decides its kind";

impl Rule {
    /// The rule's code in the step 4 spec (K1 to K8).
    #[must_use]
    pub fn code(self) -> &'static str {
        match self {
            Rule::Read => "K1",
            Rule::Vm => "K2",
            Rule::Screen => "K3",
            Rule::Render => "K4",
            Rule::OwnGame => "K5",
            Rule::Creates => "K6",
            Rule::Nobodys => "K7",
            Rule::Owned => "K8",
        }
    }

    /// The rule's line under decision 5: what kind of list it is, and for a
    /// stopgap the rule that replaces it and the condition that deletes it.
    #[must_use]
    pub fn line(self) -> &'static str {
        match self {
            Rule::Read => "a read by its name's question word: a stopgap (decision 5 (c)), replaced by x-native-effects; deletes when every K1 row's evidence is a trace",
            Rule::Vm => "VM registrations and waits: a declared R3 set (decision 5 (a); ADR-073's R3 list)",
            Rule::Screen => "the screen, menus and music: a declared R3 set (decision 5 (a); ADR-073's R3 list)",
            Rule::Render => "a render by its name's words: a stopgap (decision 5 (c)), replaced by x-native-effects; deletes when every K4 row's evidence is a trace",
            Rule::OwnGame => "a static on no object: keyed on the subject, no list (the spec's New 1)",
            Rule::Creates => "a reference's creation by its result, a scene's start by its name: a stopgap (decision 5 (c)), replaced by x-native-effects; deletes when every K6 row's evidence is a trace",
            Rule::Nobodys => "nobody's by its class (D-F1 C's quest, alias and global, and the forms no seat owns): a stopgap (decision 5 (c)), replaced by x-native-effects; deletes when every K7 row's evidence is a trace",
            Rule::Owned => "an owned object's: the default, keyed on the subject, no list",
        }
    }

    /// The replacing rule of a stopgap rule, none for a declared set or a
    /// rule keyed on the subject.
    #[must_use]
    pub fn replaced_by(self) -> Option<&'static str> {
        match self {
            Rule::Read | Rule::Render | Rule::Creates | Rule::Nobodys => Some(BY_EFFECTS),
            Rule::Vm | Rule::Screen | Rule::OwnGame | Rule::Owned => None,
        }
    }
}

/// A row's cells as the rules give them, or as the overlay sets them over
/// the rules'.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Cells {
    /// What a call is.
    pub kind: Kind,
    /// Where its subject stands.
    pub subject: Subject,
    /// Who owns the subject.
    pub owner: Owner,
    /// Who hears an accepted call.
    pub audience: Audience,
    /// What a receiver runs for one.
    pub relay: Relay,
    /// What the server keeps of one.
    pub keep: Keep,
    /// Whether it may cross as a firing's look.
    pub look: Look,
    /// What it does inside another player's acting scope.
    pub scoped: Scoped,
    /// Its parameters in order (an array's among them: the wire's
    /// SnippetArg carries no array, so such a row is never relayed).
    pub params: Vec<Param>,
    /// What the seam or the client does beyond the rest: the overlay's
    /// alone, every rule giving none.
    pub hook: Hook,
}

/// A native's derived row: the rule that set its kind and its cells.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Derived {
    /// The rule.
    pub rule: Rule,
    /// The cells.
    pub cells: Cells,
}

/// The classes nobody owns (D-F1 C: a quest's, an alias's, a global's).
const NOBODY: [&str; 3] = ["quest", "alias", "globalvariable"];
/// K3's classes: a message box, an image space, music and a sound category.
const SCREEN: [&str; 4] = [
    "message",
    "imagespacemodifier",
    "musictype",
    "soundcategory",
];
/// K3's first words: a menu shown or opened, a message box, a notification.
const SCREEN_WORDS: [&str; 4] = ["show", "open", "message", "notification"];
/// K1's question words.
const READ_WORDS: [&str; 8] = [
    "get",
    "is",
    "has",
    "can",
    "find",
    "will",
    "worn",
    "calculate",
];
/// The call's leading arguments, by index, for a twin's relay: a native
/// takes at most eleven.
const LEADING: [u8; 11] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10];

/// A name's words: its camel-case runs (SetSubGraphFloatVariable is Set,
/// Sub, Graph, Float, Variable), lower-cased.
fn words(name: &str) -> Vec<String> {
    let mut out: Vec<String> = Vec::new();
    for c in name.chars() {
        match out.last_mut() {
            Some(w) if !c.is_ascii_uppercase() => w.push(c.to_ascii_lowercase()),
            _ => out.push(c.to_ascii_lowercase().to_string()),
        }
    }
    out
}

/// Whether a name's words begin with `first`: a name the dump spells all in
/// lower case has no word boundaries left ("getplayer"), so its first word
/// is read as its prefix.
fn begins(name: &str, first: &[&str]) -> bool {
    if !name.chars().any(|c| c.is_ascii_uppercase()) {
        return name.starts_with(&first.concat());
    }
    let w = words(name);
    w.len() >= first.len() && w.iter().zip(first).all(|(a, b)| a == b)
}

fn contains_words(name: &str, run: &[&str]) -> bool {
    let w = words(name);
    w.windows(run.len())
        .any(|win| win.iter().zip(run).all(|(a, b)| a == b))
}

fn param_of(ty: &Type) -> Option<Param> {
    match ty.raw {
        "Bool" => Some(Param::Bool),
        "Int" => Some(Param::Int),
        "Float" => Some(Param::Float),
        "String" => Some(Param::String),
        "Object" => Some(Param::Object),
        "BoolArray" => Some(Param::BoolArray),
        "IntArray" => Some(Param::IntArray),
        "FloatArray" => Some(Param::FloatArray),
        "StringArray" => Some(Param::StringArray),
        "ObjectArray" => Some(Param::ObjectArray),
        _ => None,
    }
}

/// The rules' row for `native` of `dump`.
#[must_use]
pub fn derive(dump: &Dump, native: &Native) -> Derived {
    let lineage = dump.lineage(native.cls);
    let in_lineage = |set: &[&str]| lineage.iter().any(|c| set.contains(&c.as_str()));
    let owned = dump.is_reference(native.cls) || lineage.iter().any(|c| c == "cell");
    let scene = in_lineage(&["scene"]);
    let first_is_reference = native
        .args
        .first()
        .and_then(|a| a.ty.object)
        .is_some_and(|o| dump.is_reference(o));
    let subject = if native.is_static {
        if first_is_reference {
            Subject::Arg0
        } else {
            Subject::Nothing
        }
    } else if owned {
        Subject::SelfRef
    } else if !in_lineage(&NOBODY)
        && !in_lineage(&["scene", "activemagiceffect"])
        && !native.cls.eq_ignore_ascii_case("form")
        && first_is_reference
    {
        Subject::Arg0Method
    } else {
        Subject::SelfRef
    };
    let name = native.name;
    let has_result = native.ret.raw != "None";
    let creates_reference =
        native.ret.raw == "Object" && native.ret.object.is_some_and(|o| dump.is_reference(o));
    let (rule, kind) = if has_result && READ_WORDS.iter().any(|w| begins(name, &[*w])) {
        (Rule::Read, Kind::Local)
    } else if begins(name, &["register", "for"])
        || begins(name, &["unregister", "for"])
        || begins(name, &["wait"])
        || contains_words(name, &["inventory", "event", "filter"])
        || contains_words(name, &["object", "profiling"])
        || contains_words(name, &["mod", "event"])
    {
        (Rule::Vm, Kind::Local)
    } else if SCREEN_WORDS.iter().any(|w| begins(name, &[*w])) || in_lineage(&SCREEN) {
        (Rule::Screen, Kind::Local)
    } else if subject != Subject::Nothing
        && (begins(name, &["play"])
            || begins(name, &["say"])
            || begins(name, &["send", "animation", "event"])
            || begins(name, &["set", "animation", "variable"])
            || (begins(name, &["set", "sub", "graph"])
                && words(name).last().is_some_and(|w| w == "variable"))
            || (subject == Subject::Arg0Method && begins(name, &["stop"])))
    {
        (Rule::Render, Kind::Action)
    } else if native.is_static && subject == Subject::Nothing {
        (Rule::OwnGame, Kind::State)
    } else if creates_reference
        || (scene
            && (name.eq_ignore_ascii_case("Start") || name.eq_ignore_ascii_case("ForceStart")))
    {
        (Rule::Creates, Kind::Create)
    } else if !native.is_static
        && (in_lineage(&NOBODY)
            || (!owned
                && !in_lineage(&["scene", "activemagiceffect"])
                && subject != Subject::Arg0Method))
    {
        (Rule::Nobodys, Kind::Everyone)
    } else {
        (Rule::Owned, Kind::State)
    };
    let mut owner = match (kind, rule) {
        (Kind::Local, _) | (_, Rule::OwnGame) => {
            Owner::Player
        }
        (Kind::Everyone, _) => Owner::Nobody,
        (Kind::Create, _) => Owner::Creator,
        (Kind::Action | Kind::State, _) => Owner::Asked,
    };
    if scene && kind != Kind::Local {
        owner = Owner::Creator;
    }
    let audience = match kind {
        Kind::Action => Audience::Listeners,
        Kind::Everyone => Audience::Everyone,
        Kind::State | Kind::Local | Kind::Create => Audience::Nobody,
    };
    // parse_dump admits no other raw type, so every argument has its Param
    let params: Vec<Param> = native.args.iter().filter_map(|a| param_of(&a.ty)).collect();
    let relayed = matches!(
        kind,
        Kind::Action | Kind::State
    ) && subject != Subject::Nothing
        && !params.iter().any(|p| p.is_array());
    let relay = if !relayed {
        Relay::Never
    } else if !native.latent {
        Relay::Same
    } else {
        twin(dump, native).unwrap_or(Relay::Never)
    };
    let keep = if kind == Kind::Create && !scene {
        Keep::Record(RecordKind::Placement)
    } else {
        Keep::Nothing
    };
    let look = if kind == Kind::Action {
        Look::Also
    } else {
        Look::Never
    };
    let scoped = if subject == Subject::Nothing && !native.latent && !has_result {
        Scoped::ActingSeatAlone
    } else {
        Scoped::Runs
    };
    Derived {
        rule,
        cells: Cells {
            kind,
            subject,
            owner,
            audience,
            relay,
            keep,
            look,
            scoped,
            params,
            hook: Hook::None,
        },
    }
}

/// A latent native's non-latent twin (ADR-068 decision 4: a receiver never
/// runs a latent native): its name less AndWait, or with NoWait, in its
/// class or an ancestor, taking the call's leading arguments by name.
fn twin(dump: &Dump, native: &Native) -> Option<Relay> {
    let lower = native.name.to_ascii_lowercase();
    let stem = lower.strip_suffix("andwait").and_then(|_| {
        native
            .name
            .get(..native.name.len().saturating_sub("AndWait".len()))
    });
    let no_wait = format!("{}NoWait", native.name);
    let candidates = stem
        .into_iter()
        .map(str::to_owned)
        .chain(std::iter::once(no_wait));
    for candidate in candidates {
        let Some(t) = dump.find(native.cls, &candidate).filter(|t| !t.latent) else {
            continue;
        };
        let leading = native.args.len() >= t.args.len()
            && native
                .args
                .iter()
                .zip(&t.args)
                .all(|(a, b)| a.name.eq_ignore_ascii_case(b.name));
        let args = LEADING.get(..t.args.len())?;
        if leading {
            return Some(Relay::Twin {
                cls: t.cls,
                func: t.name,
                args,
            });
        }
    }
    None
}

/// The class of an overlay entry (decision 5: the only two kinds of hand
/// data admissible).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Class {
    /// A cell an accepted decision sets.
    Decided {
        /// The ADR row or D-number that sets it.
        cites: &'static str,
    },
    /// A stopgap until a rule lands (decision 5 (c)).
    Stopgap {
        /// The rule that replaces it.
        replaced_by: &'static str,
        /// The condition that deletes it.
        deletes: &'static str,
    },
}

/// A cell an overlay entry sets over the rules'.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Cell {
    /// The kind.
    Kind(Kind),
    /// The owner.
    Owner(Owner),
    /// The audience.
    Audience(Audience),
    /// The relay.
    Relay(Relay),
    /// The keep.
    Keep(Keep),
    /// The look.
    Look(Look),
    /// The acting scope's rule.
    Scoped(Scoped),
    /// The hook (the seam's name lists as one column, thuum ADR-073 step 4's
    /// S4), each code's line its decision 5 class ([`Hook::line`]).
    Hook(Hook),
}

impl Cell {
    fn name(self) -> &'static str {
        match self {
            Cell::Kind(_) => "kind",
            Cell::Owner(_) => "owner",
            Cell::Audience(_) => "audience",
            Cell::Relay(_) => "relay",
            Cell::Keep(_) => "keep",
            Cell::Look(_) => "look",
            Cell::Scoped(_) => "scoped",
            Cell::Hook(_) => "hook",
        }
    }

    /// Whether `cells` already holds this cell's value.
    fn holds(self, cells: &Cells) -> bool {
        match self {
            Cell::Kind(k) => cells.kind == k,
            Cell::Owner(o) => cells.owner == o,
            Cell::Audience(a) => cells.audience == a,
            Cell::Relay(r) => cells.relay == r,
            Cell::Keep(k) => cells.keep == k,
            Cell::Look(l) => cells.look == l,
            Cell::Scoped(s) => cells.scoped == s,
            Cell::Hook(h) => cells.hook == h,
        }
    }

    fn set(self, cells: &mut Cells) {
        match self {
            Cell::Kind(k) => cells.kind = k,
            Cell::Owner(o) => cells.owner = o,
            Cell::Audience(a) => cells.audience = a,
            Cell::Relay(r) => cells.relay = r,
            Cell::Keep(k) => cells.keep = k,
            Cell::Look(l) => cells.look = l,
            Cell::Scoped(s) => cells.scoped = s,
            Cell::Hook(h) => cells.hook = h,
        }
    }
}

/// One overlay entry.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Entry {
    /// The native, Class.Name; a live row's spelling is this one.
    pub native: &'static str,
    /// Its decision 5 class.
    pub class: Class,
    /// A row of the live table, in the overlay's order.
    pub live: bool,
    /// The comment lines the generated table writes above a live row.
    pub note: &'static [&'static str],
    /// The cells it sets over the rules'.
    pub cells: &'static [Cell],
}

impl Entry {
    /// The entry's class and name.
    fn names(&self) -> Option<(&'static str, &'static str)> {
        self.native.split_once('.')
    }
}

include!("natives_overlay.rs");

/// Why the overlay was refused: each names its entry.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Refusal {
    /// The entry names no native of the dump.
    Unknown(&'static str),
    /// Two entries name one native.
    Twice(&'static str),
    /// A decided entry cites nothing, or a stopgap names no replacing rule or
    /// no deleting condition.
    Incomplete(&'static str),
    /// The entry sets a cell to the rules' own value, or sets nothing and is
    /// not live.
    Unchanged {
        /// The entry's native.
        native: &'static str,
        /// The cell, "nothing" for an entry with none.
        cell: &'static str,
    },
    /// The entry sets one cell twice.
    Repeated {
        /// The entry's native.
        native: &'static str,
        /// The cell.
        cell: &'static str,
    },
    /// A twin relay names no non-latent native of the dump.
    NoTwin(&'static str),
    /// Two natives whose names compare equal without case, which the
    /// table's index cannot tell apart (the dump holds none).
    Collides(&'static str),
}

impl fmt::Display for Refusal {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Refusal::Unknown(n) => write!(f, "{n}: no such native in FunctionsDump.txt"),
            Refusal::Twice(n) => write!(f, "{n}: a second entry for one native"),
            Refusal::Incomplete(n) => write!(f, "{n}: incomplete (a decided entry cites its decision; a stopgap names its replacing rule and its deleting condition)"),
            Refusal::Unchanged { native, cell } => write!(f, "{native}: sets {cell} to the rules' own value"),
            Refusal::Repeated { native, cell } => write!(f, "{native}: sets {cell} twice"),
            Refusal::NoTwin(n) => write!(f, "{n}: its twin relay names no non-latent native of the dump"),
            Refusal::Collides(n) => write!(f, "{n}: another native's name compares equal without case"),
        }
    }
}

/// One native's row after the overlay.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct GeneratedRow {
    /// Its class: a live row's entry's spelling, else the dump's.
    pub cls: &'static str,
    /// Its name, spelled as its class is.
    pub func: &'static str,
    /// The native as the dump declares it.
    pub native: Native,
    /// The rules' row.
    pub derived: Derived,
    /// The cells, the overlay's over the rules'.
    pub cells: Cells,
    /// Its overlay entry.
    pub entry: Option<Entry>,
}

/// Every native's row, and the live table's.
#[derive(Debug, Clone)]
pub struct Generated {
    /// Every native's row, in the dump's order.
    pub rows: Vec<GeneratedRow>,
    /// The live table: indices into `rows`, in the overlay's order.
    pub live: Vec<usize>,
}

/// Every native's row: the rules' cells, the overlay's entries over them.
pub fn generate(dump: &Dump, overlay: &[Entry]) -> Result<Generated, Vec<Refusal>> {
    let mut refusals = Vec::new();
    let mut rows: Vec<GeneratedRow> = dump
        .natives()
        .iter()
        .map(|n| {
            let derived = derive(dump, n);
            GeneratedRow {
                cls: n.cls,
                func: n.name,
                native: n.clone(),
                cells: derived.cells.clone(),
                derived,
                entry: None,
            }
        })
        .collect();
    let mut live = Vec::new();
    for entry in overlay {
        let complete = match entry.class {
            Class::Decided { cites } => !cites.trim().is_empty(),
            Class::Stopgap {
                replaced_by,
                deletes,
            } => !replaced_by.trim().is_empty() && !deletes.trim().is_empty(),
        };
        if !complete {
            refusals.push(Refusal::Incomplete(entry.native));
        }
        let found = entry.names().and_then(|(cls, func)| {
            rows.iter()
                .position(|r| {
                    r.native.cls.eq_ignore_ascii_case(cls)
                        && r.native.name.eq_ignore_ascii_case(func)
                })
                .map(|i| (i, cls, func))
        });
        let Some((i, cls, func)) = found else {
            refusals.push(Refusal::Unknown(entry.native));
            continue;
        };
        let Some(row) = rows.get_mut(i) else {
            refusals.push(Refusal::Unknown(entry.native));
            continue;
        };
        if row.entry.is_some() {
            refusals.push(Refusal::Twice(entry.native));
            continue;
        }
        if entry.cells.is_empty() && !entry.live {
            refusals.push(Refusal::Unchanged {
                native: entry.native,
                cell: "nothing",
            });
        }
        for (n, cell) in entry.cells.iter().enumerate() {
            if entry.cells.iter().take(n).any(|c| c.name() == cell.name()) {
                refusals.push(Refusal::Repeated {
                    native: entry.native,
                    cell: cell.name(),
                });
            }
            if cell.holds(&row.derived.cells) {
                refusals.push(Refusal::Unchanged {
                    native: entry.native,
                    cell: cell.name(),
                });
            }
            if let Cell::Relay(Relay::Twin {
                cls: tc, func: tf, ..
            }) = cell
            {
                if dump.native(tc, tf).is_none_or(|t| t.latent) {
                    refusals.push(Refusal::NoTwin(entry.native));
                }
            }
            cell.set(&mut row.cells);
        }
        row.entry = Some(*entry);
        if entry.live {
            row.cls = cls;
            row.func = func;
            live.push(i);
        }
    }
    let mut keys = std::collections::BTreeSet::new();
    for row in &rows {
        if !keys.insert((row.cls.to_ascii_lowercase(), row.func.to_ascii_lowercase())) {
            refusals.push(Refusal::Collides(row.native.name));
        }
    }
    let generated = Generated { rows, live };
    if refusals.is_empty() {
        Ok(generated)
    } else {
        Err(refusals)
    }
}

impl Generated {
    /// The live table's rows, in its order.
    pub fn live_rows(&self) -> impl Iterator<Item = &GeneratedRow> {
        self.live.iter().filter_map(|&i| self.rows.get(i))
    }

    /// The derived rows the table holds staged (step 4's S5): every native
    /// no live entry names, in the dump's order.
    pub fn staged_rows(&self) -> impl Iterator<Item = &GeneratedRow> {
        self.rows
            .iter()
            .enumerate()
            .filter(|(i, _)| !self.live.contains(i))
            .map(|(_, r)| r)
    }

    /// Every row of the table in its order: the live rows, then the staged.
    pub fn table_rows(&self) -> impl Iterator<Item = &GeneratedRow> {
        self.live_rows().chain(self.staged_rows())
    }
}

/// A live row in the table's own types ([`natives::Row`]'s cells, its
/// parameters owned).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct TableRow {
    /// The class.
    pub cls: &'static str,
    /// The name.
    pub func: &'static str,
    /// The kind.
    pub kind: Kind,
    /// The subject.
    pub subject: Subject,
    /// The owner.
    pub owner: Owner,
    /// The audience.
    pub audience: Audience,
    /// The relay.
    pub relay: Relay,
    /// The keep.
    pub keep: Keep,
    /// The look.
    pub look: Look,
    /// The parameters.
    pub params: Vec<Param>,
    /// The hook.
    pub hook: Hook,
    /// Latent.
    pub latent: bool,
    /// Staged.
    pub staged: bool,
}

impl TableRow {
    /// A row of the committed table.
    #[must_use]
    pub fn of(row: &natives::Row) -> Self {
        TableRow {
            cls: row.cls,
            func: row.func,
            kind: row.kind,
            subject: row.subject,
            owner: row.owner,
            audience: row.audience,
            relay: row.relay,
            keep: row.keep,
            look: row.look,
            params: row.params.to_vec(),
            hook: row.hook,
            latent: row.latent,
            staged: row.staged,
        }
    }

    /// A generated row in the table's types: staged unless a live entry
    /// names it.
    #[must_use]
    pub fn of_generated(row: &GeneratedRow) -> Self {
        TableRow {
            cls: row.cls,
            func: row.func,
            kind: row.cells.kind,
            subject: row.cells.subject,
            owner: row.cells.owner,
            audience: row.cells.audience,
            relay: row.cells.relay,
            keep: row.cells.keep,
            look: row.cells.look,
            params: row.cells.params.clone(),
            hook: row.cells.hook,
            latent: row.native.latent,
            staged: !row.entry.is_some_and(|e| e.live),
        }
    }

    /// The row's line in natives_table.rs.
    #[must_use]
    pub fn line(&self) -> String {
        let relay = match self.relay {
            Relay::Same => "Relay::Same".to_owned(),
            Relay::Never => "Relay::Never".to_owned(),
            Relay::Twin { cls, func, args } => {
                let args: Vec<String> = args.iter().map(u8::to_string).collect();
                format!(
                    "Relay::Twin {{ cls: \"{cls}\", func: \"{func}\", args: &[{}] }}",
                    args.join(", ")
                )
            }
        };
        let keep = match self.keep {
            Keep::Record(kind) => format!("Keep::Record(RecordKind::{kind:?})"),
            other => format!("Keep::{other:?}"),
        };
        let params: Vec<String> = self
            .params
            .iter()
            .map(|p| format!("Param::{p:?}"))
            .collect();
        format!(
            "    Row {{ cls: \"{}\", func: \"{}\", kind: Kind::{:?}, subject: Subject::{:?}, owner: Owner::{:?}, audience: Audience::{:?}, relay: {relay}, keep: {keep}, look: Look::{:?}, params: &[{}], hook: Hook::{:?}, latent: {}, staged: {} }},",
            self.cls,
            self.func,
            self.kind,
            self.subject,
            self.owner,
            self.audience,
            self.look,
            params.join(", "),
            self.hook,
            self.latent,
            self.staged
        )
    }
}

/// natives_table.rs's header.
const TABLE_HEADER: &str = concat!(
    "// Generated by wire-rules natives-gen (thuum ADR-073 step 4, Eli's D-NT1 C);\n",
    "// do not edit by hand. Each row is natives_rules.rs's derivation from\n",
    "// skyrim-platform's FunctionsDump.txt, every parameter list the native's own,\n",
    "// with its overlay entry's cells over it (natives_overlay.rs, which holds\n",
    "// the comment above a row and the decision each row cites, or for a\n",
    "// stopgap's hook its replacing rule); the rows are the overlay's live\n",
    "// entries in its order, so no earlier row's place moves. Edit the overlay\n",
    "// and run `cargo run -p wire-rules --bin\n",
    "// natives-gen`; natives_rules.rs's generated-table test refuses a copy that\n",
    "// differs. The platform reads these rows through wire-client-ffi's generated\n",
    "// header (skymp_wire.h's SkympNativeRows), never a copy of its own. Included\n",
    "// by natives.rs, whose types it names.\n",
);

/// The live table's doc comment, as rung 5, D-E1 A and step 4's S4 left it.
const TABLE_DOC: &str = concat!(
    "/// Every tabled native (thuum ADR-068 decision 3): rung 2's set, the six\n",
    "/// action rows, the flip's twenty-four state and everyone rows and\n",
    "/// Game.GetPlayer, local; then D-S3's three trap rows (ProcessTrapHit,\n",
    "/// SetMotionType, ApplyHavokImpulse), the first kept as its actor's death\n",
    "/// evidence; then rung 4's seven, the standing's six kept as the NPC's record\n",
    "/// and ReferenceAlias.Clear beside ForceRefTo; then D-W2's Sound.Play and\n",
    "/// CreateDetectionEvent, a firing's looks alone; then rung 5's six, a\n",
    "/// player's own race, spells, shouts and equipment, kept as its own record,\n",
    "/// and D-E1 A's head part; the flip's AddItem and RemoveItem keep the\n",
    "/// player's own inventory on its own 0x14 since D-R1 C; then train 53's two\n",
    "/// scene starts (ADR-073 step 4's S4), creations judged as state rows whose\n",
    "/// hook is V-scene's count. Every owner on a subject is asked (ADR-073\n",
    "/// decision 6), and the hook column holds what the seam's name lists held.\n",
    "/// Fifty-two live rows; then (step 4's S5) every other native of\n",
    "/// FunctionsDump.txt, staged: counted on its seat by what it would do and\n",
    "/// run there as before, refused by the server until its batch unstages.\n",
    "/// A native with no live row has no channel.\n",
);

/// The staged rows' comment in natives_table.rs, above the first.
const STAGED_NOTE: &str = concat!(
    "    // the derived rows, staged (thuum ADR-073 step 4's S5): every other\n",
    "    // native of FunctionsDump.txt in the dump's order, its cells the rules'\n",
    "    // with any overlay entry's over them, counted on its seat as\n",
    "    // would-forward, would-relay, would-filter, would-skip or would-run by\n",
    "    // its stack's root kind and run there as before; the server refuses it\n",
    "    // by its row (channel_of) until its batch unstages (S8)\n",
);

/// The index's doc comment in natives_table.rs.
const INDEX_DOC: &str = concat!(
    "/// The table's open-addressed index (natives-gen): a pair's slot is\n",
    "/// row_key_hash's low bits under INDEX_MASK, its row the first slot of the\n",
    "/// probe from there that names it, an empty slot u16::MAX; twice the rows'\n",
    "/// count rounded up to a power of two, so every probe ends at an empty slot\n",
    "/// (thuum ADR-073 step 4's S5; rule 12: no packet scans the table).\n",
);

/// The table's index as natives-gen lays it out: each row of the table, in
/// its order, at the first free slot of its key's probe.
#[must_use]
pub fn index_of(rows: &[TableRow]) -> Vec<u16> {
    let slots = rows.len().saturating_mul(2).next_power_of_two().max(64);
    let mask = slots.saturating_sub(1);
    let mut index = vec![u16::MAX; slots];
    for (i, row) in rows.iter().enumerate() {
        let Ok(at) = u16::try_from(i) else {
            break;
        };
        let hash = natives::row_key_hash(row.cls, row.func);
        let mut slot = usize::from(u16::try_from(hash & 0xffff).unwrap_or(0)) & mask;
        for _ in 0..slots {
            match index.get_mut(slot) {
                Some(free) if *free == u16::MAX => {
                    *free = at;
                    break;
                }
                _ => slot = slot.wrapping_add(1) & mask,
            }
        }
    }
    index
}

/// natives_table.rs as the rows make it: the header, the table's doc
/// comment, then each live row under its entry's note, in the overlay's
/// order, then the staged rows in the dump's, then the index. natives-gen
/// writes it; the generated-table test compares it with the committed file
/// byte for byte.
#[must_use]
pub fn table_source(generated: &Generated) -> String {
    let mut out = String::from(TABLE_HEADER);
    out.push('\n');
    out.push_str(TABLE_DOC);
    out.push_str("pub const TABLE: &[Row] = &[\n");
    let mut rows = Vec::new();
    for (n, row) in generated.table_rows().enumerate() {
        let table = TableRow::of_generated(row);
        if n == generated.live.len() {
            out.push_str(STAGED_NOTE);
        }
        for line in row.entry.filter(|e| e.live).map_or(&[][..], |e| e.note) {
            out.push_str("    //");
            if !line.is_empty() {
                out.push(' ');
                out.push_str(line);
            }
            out.push('\n');
        }
        out.push_str(&table.line());
        out.push('\n');
        rows.push(table);
    }
    out.push_str("];\n");
    let index = index_of(&rows);
    out.push('\n');
    out.push_str("/// The mask of a slot in [`INDEX`]: its length less one.\n");
    out.push_str(&format!("pub const INDEX_MASK: usize = {};\n", index.len().saturating_sub(1)));
    out.push('\n');
    out.push_str(INDEX_DOC);
    out.push_str(&format!("pub const INDEX: [u16; {}] = [\n", index.len()));
    for chunk in index.chunks(16) {
        let cells: Vec<String> = chunk.iter().map(u16::to_string).collect();
        out.push_str(&format!("    {},\n", cells.join(", ")));
    }
    out.push_str("];\n");
    out
}

/// The client's generated file (skymp5-client src/lib/nativeRows.generated.ts),
/// its header.
const TS_HEADER: &str = concat!(
    "// Generated by wire-rules natives-gen (thuum ADR-073 step 4, Eli's D-NT1 C);\n",
    "// do not edit by hand. The live table's columns (skymp-wire wire-rules\n",
    "// natives_table.rs) as the client reads them, in place of the name lists its\n",
    "// services kept (scriptCallService.ts's record rows, spSnippetService.ts's\n",
    "// look rows and graph variables): natives-gen derives each from the table's\n",
    "// keep, look, subject and relay cells, and natives_rules.rs's generated-table\n",
    "// test refuses a copy that differs. Names are the table's spelling, a\n",
    "// relay's in lower case as SpSnippetService compares them.\n",
);

/// A live row's relay native, or the row itself for one never relayed.
fn relay_or_self(row: &TableRow) -> (&'static str, &'static str) {
    match row.relay {
        Relay::Same | Relay::Never => (row.cls, row.func),
        Relay::Twin { cls, func, .. } => (cls, func),
    }
}

/// A TypeScript string literal of `text`, which a native's name never needs
/// escaped beyond a quote or a backslash.
fn ts_text(text: &str) -> String {
    format!("\"{}\"", text.replace('\\', "\\\\").replace('"', "\\\""))
}

/// nativeRows.generated.ts as the live rows make it: the record rows by
/// family, the look rows as a relay names them, the look-only rows a relay
/// names with its subject's sequence, and the graph variable rows' value
/// kinds. natives-gen writes it; the generated-table test compares it with
/// the committed file byte for byte.
#[must_use]
pub fn ts_source(generated: &Generated) -> String {
    let rows: Vec<TableRow> = generated.live_rows().map(TableRow::of_generated).collect();
    let mut out = String::from(TS_HEADER);
    out.push_str("\n// the record rows by their keep (Keep::Record): \"ref\" the ref state's, \"standing\"\n");
    out.push_str("// the standing's, \"own\" a player's own; a placement's is none of them (W3:\n");
    out.push_str("// the platform never batches it)\n");
    out.push_str("export const NATIVE_RECORD_FAMILIES: { readonly [native: string]: \"ref\" | \"standing\" | \"own\" } = {\n");
    for row in &rows {
        let family = match row.keep {
            Keep::Record(RecordKind::RefState) => "ref",
            Keep::Record(RecordKind::Standing) => "standing",
            Keep::Record(RecordKind::PlayerOwn) => "own",
            _ => continue,
        };
        out.push_str(&format!("  {}: {},\n", ts_text(&format!("{}.{}", row.cls, row.func)), ts_text(family)));
    }
    out.push_str("};\n");
    let names = |keep: &dyn Fn(&TableRow) -> Option<String>| -> Vec<String> {
        let mut seen: Vec<String> = Vec::new();
        for row in &rows {
            if let Some(name) = keep(row).filter(|n| !seen.contains(n)) {
                seen.push(name);
            }
        }
        seen
    };
    let relays = names(&|r| {
        (r.look != Look::Never).then(|| {
            let (cls, func) = relay_or_self(r);
            format!("{cls}.{func}").to_ascii_lowercase()
        })
    });
    let look_only = names(&|r| (r.look == Look::Only && r.subject != Subject::Arg0Method).then(|| r.func.to_ascii_lowercase()));
    out.push_str("\n// the look rows as a relay names them (class.function in lower case): every\n");
    out.push_str("// row whose look is Also or Only (ADR-068 D-W2 A), by its relay native, a\n");
    out.push_str("// twin's for a latent row, or by itself for a row never relayed as an action\n");
    out.push_str("export const NATIVE_LOOK_RELAYS: readonly string[] = [\n");
    for name in &relays {
        out.push_str(&format!("  {},\n", ts_text(name)));
    }
    out.push_str("];\n");
    out.push_str("\n// the look-only rows a relay names with its subject's seq (Look::Only on a\n");
    out.push_str("// subject its self names, so not Sound.Play's), by function in lower case\n");
    out.push_str("export const NATIVE_LOOK_ONLY_FNS: readonly string[] = [\n");
    for name in &look_only {
        out.push_str(&format!("  {},\n", ts_text(name)));
    }
    out.push_str("];\n");
    out.push_str("\n// the graph variable rows (Keep::Variable, the fifty fixes' D-C3 A) by\n");
    out.push_str("// function in lower case: ScriptVar's folded kind of the value, its second\n");
    out.push_str("// parameter's (3 an int, 4 a float, 5 a bool)\n");
    out.push_str("export const NATIVE_GRAPH_VAR_KINDS: { readonly [fn: string]: number } = {\n");
    for row in &rows {
        if row.keep != Keep::Variable {
            continue;
        }
        let kind = match row.params.get(1) {
            Some(Param::Int) => 3,
            Some(Param::Float) => 4,
            Some(Param::Bool) => 5,
            _ => continue,
        };
        out.push_str(&format!("  {}: {kind},\n", row.func.to_ascii_lowercase()));
    }
    out.push_str("};\n");
    out
}

fn kind_name(kind: Kind) -> String {
    format!("{kind:?}")
}

fn owner_name(owner: Owner) -> String {
    format!("{owner:?}")
}

fn relay_name(relay: Relay) -> String {
    match relay {
        Relay::Same => "same".to_owned(),
        Relay::Never => "none".to_owned(),
        Relay::Twin { cls, func, .. } => format!("twin:{cls}.{func}"),
    }
}

fn type_name(ty: &Type) -> &'static str {
    ty.object.unwrap_or(ty.raw)
}

/// A native's signature as the ledger shows it, "static Bool
/// Game.UsingGamepad() latent".
#[must_use]
pub fn signature(n: &Native) -> String {
    let args: Vec<String> = n
        .args
        .iter()
        .map(|a| format!("{} {}", type_name(&a.ty), a.name))
        .collect();
    format!(
        "{}{} {}.{}({}){}",
        if n.is_static { "static " } else { "" },
        type_name(&n.ret),
        n.cls,
        n.name,
        args.join(", "),
        if n.latent { " latent" } else { "" }
    )
}

/// The TSV's header line.
pub const TSV_HEADER: &str = "native\tsignature\tkind\tsubject\towner\taudience\trelay\tkeep\tlook\tscoped\thook\tparams\trule\trow\tstage\tsource\tevidence";

/// Every native's row as natives.tsv, which thuum's lab/ledger.py joins into
/// docs/NATIVES.md: one line per native, the header first, in the table's
/// order. `row` is the native's index in the table, `stage` "live" or
/// "staged" (step 4's S5); `source` the rule's code and decision
/// 5 line, or the entry's class and citation; `evidence` what the row rests
/// on ("static": the dump's declaration and a rule; x-native-effects writes
/// "trace").
#[must_use]
pub fn tsv(generated: &Generated) -> String {
    let mut out = String::from(TSV_HEADER);
    out.push('\n');
    for (place, row) in generated.table_rows().enumerate() {
        let c = &row.cells;
        let params = c
            .params
            .iter()
            .map(|x| format!("{x:?}"))
            .collect::<Vec<_>>()
            .join(",");
        let stage = if row.entry.is_some_and(|e| e.live) { "live" } else { "staged" };
        let source = match row.entry.map(|e| e.class) {
            Some(Class::Decided { cites }) => format!("decided: {cites}"),
            Some(Class::Stopgap {
                replaced_by,
                deletes,
            }) => format!("stopgap: replaced by {replaced_by}; deletes {deletes}"),
            None => format!("{}: {}", row.derived.rule.code(), row.derived.rule.line()),
        };
        let keep = match c.keep {
            Keep::Record(k) => format!("Record({k:?})"),
            other => format!("{other:?}"),
        };
        let fields = [
            format!("{}.{}", row.cls, row.func),
            signature(&row.native),
            kind_name(c.kind),
            format!("{:?}", c.subject),
            owner_name(c.owner),
            format!("{:?}", c.audience),
            relay_name(c.relay),
            keep,
            format!("{:?}", c.look),
            format!("{:?}", c.scoped),
            if c.hook == Hook::None { "none".to_owned() } else { c.hook.name().to_owned() },
            params,
            row.derived.rule.code().to_owned(),
            place.to_string(),
            stage.to_owned(),
            source,
            "static".to_owned(),
        ];
        out.push_str(&fields.map(|f| f.replace(['\t', '\n'], " ")).join("\t"));
        out.push('\n');
    }
    out
}

/// A rule's count over every native, with the first native it took (the
/// tool's summary: decision 7, a list named once with one subject, then
/// counted).
#[must_use]
pub fn rule_counts(generated: &Generated) -> Vec<(Rule, usize, Option<String>)> {
    RULES
        .iter()
        .map(|&rule| {
            let mut taken = generated.rows.iter().filter(|r| r.derived.rule == rule);
            let first = taken
                .next()
                .map(|r| format!("{}.{}", r.native.cls, r.native.name));
            (
                rule,
                usize::from(first.is_some()).saturating_add(taken.count()),
                first,
            )
        })
        .collect()
}

#[cfg(test)]
#[allow(clippy::panic)] // a test's failure message, never a packet's path
mod tests {
    use super::*;

    const DUMP: &str = include_str!(
        "../../../../skyrim-platform/src/platform_se/codegen/convert-files/FunctionsDump.txt"
    );

    fn dump() -> Dump {
        parse_dump(DUMP).unwrap_or_else(|e| panic!("{e}"))
    }

    fn generated() -> Generated {
        generate(&dump(), OVERLAY).unwrap_or_else(|r| {
            panic!(
                "{}",
                r.iter()
                    .map(ToString::to_string)
                    .collect::<Vec<_>>()
                    .join("\n")
            )
        })
    }

    fn entry(native: &'static str, class: Class, cells: &'static [Cell]) -> Entry {
        Entry {
            native,
            class,
            live: false,
            note: &[],
            cells,
        }
    }

    fn derived(d: &Dump, cls: &str, name: &str) -> Derived {
        let n = d
            .native(cls, name)
            .unwrap_or_else(|| panic!("{cls}.{name}"));
        derive(d, n)
    }

    #[test]
    fn the_dump_holds_its_natives_and_never_an_offset() {
        let d = dump();
        assert_eq!(d.natives().len(), 1408);
        assert_eq!(d.natives().iter().filter(|n| n.is_static).count(), 299);
        assert_eq!(d.natives().iter().filter(|n| n.latent).count(), 48);
        // the reader keeps no offset: Native has no field for one
        let n = d
            .native("Game", "GetPlayer")
            .unwrap_or_else(|| panic!("GetPlayer"));
        assert_eq!((n.cls, n.name), ("Game", "getplayer"));
        assert!(matches!(parse_dump("{}"), Err(DumpError::Json(_))));
    }

    #[test]
    fn each_rule_takes_its_natives() {
        let d = dump();
        let k = |cls: &str, name: &str| {
            let r = derived(&d, cls, name);
            (r.rule, r.cells.kind, r.cells.owner)
        };
        use natives::Kind as K;
        use natives::Owner as O;
        // K1: a question with a result
        assert_eq!(
            k("Actor", "WornHasKeyword"),
            (Rule::Read, K::Local, O::Player)
        );
        assert_eq!(
            k("Game", "GetPlayer"),
            (Rule::Read, K::Local, O::Player)
        );
        // K2: a wait is the VM's own
        assert_eq!(
            k("ObjectReference", "WaitForAnimationEvent"),
            (Rule::Vm, K::Local, O::Player)
        );
        assert_eq!(
            k("utility", "Wait"),
            (Rule::Vm, K::Local, O::Player)
        );
        // K3: the screen
        assert_eq!(
            k("ImageSpaceModifier", "Apply"),
            (Rule::Screen, K::Local, O::Player)
        );
        // K4: a render on an object, a static's on its first argument
        assert_eq!(
            k("Debug", "SendAnimationEvent"),
            (Rule::Render, K::Action, Owner::Asked)
        );
        assert_eq!(
            derived(&d, "Debug", "SendAnimationEvent").cells.subject,
            Subject::Arg0
        );
        // a Player in a name is no rule: PlayerKnows asks nothing by its
        // first word and is nobody's, a Form's
        assert_eq!(
            k("Form", "PlayerKnows"),
            (Rule::Nobodys, K::Everyone, O::Nobody)
        );
        // K5: a static on no object is the player's own state, never Local
        // (New 1): the word wall's TeachWord, its skip by its subject
        let teach = derived(&d, "Game", "TeachWord");
        assert_eq!(
            (
                teach.rule,
                teach.cells.kind,
                teach.cells.subject,
                teach.cells.owner
            ),
            (
                Rule::OwnGame,
                K::State,
                Subject::Nothing,
                O::Player
            )
        );
        assert_eq!(teach.cells.scoped, Scoped::ActingSeatAlone);
        assert_eq!(
            derived(&d, "Debug", "MessageBox").cells.scoped,
            Scoped::ActingSeatAlone
        );
        // the latent wait and every read keep running in a scope
        assert_eq!(derived(&d, "utility", "Wait").cells.scoped, Scoped::Runs);
        assert_eq!(derived(&d, "Game", "GetPlayer").cells.scoped, Scoped::Runs);
        // K6: a scene's start is its starter's
        assert_eq!(
            k("Scene", "Start"),
            (Rule::Creates, Kind::Create, Owner::Creator)
        );
        assert_eq!(
            k("ObjectReference", "PlaceAtMe"),
            (Rule::Creates, Kind::Create, Owner::Creator)
        );
        // K7: a faction is nobody's
        assert_eq!(
            k("Faction", "SetEnemy"),
            (Rule::Nobodys, K::Everyone, O::Nobody)
        );
        // K8: a hire is no longer Local (decision 6's hire row, step 5)
        assert_eq!(
            k("Actor", "SetPlayerTeammate"),
            (Rule::Owned, K::State, Owner::Asked)
        );
    }

    #[test]
    fn four_twins_and_no_latent_row_relayed_as_itself() {
        let d = dump();
        let mut twins: Vec<String> = Vec::new();
        for n in d.natives() {
            let r = derive(&d, n);
            match r.cells.relay {
                Relay::Twin { cls, func, args } => {
                    assert!(n.latent, "{}.{}", n.cls, n.name);
                    assert!(
                        args.iter().all(|&a| usize::from(a) < n.args.len()),
                        "{}.{}",
                        n.cls,
                        n.name
                    );
                    twins.push(format!("{}.{} as {cls}.{func}", n.cls, n.name));
                }
                Relay::Same => assert!(
                    !n.latent,
                    "{}.{} is latent and relayed as itself",
                    n.cls, n.name
                ),
                Relay::Never => {}
            }
        }
        assert_eq!(
            twins,
            [
                "ObjectReference.Disable as ObjectReference.DisableNoWait",
                "ObjectReference.Enable as ObjectReference.EnableNoWait",
                "ObjectReference.PlayAnimationAndWait as ObjectReference.PlayAnimation",
                "Sound.PlayAndWait as Sound.Play",
            ]
        );
    }

    #[test]
    fn the_kinds_and_rules_are_pinned() {
        let g = generated();
        use natives::Kind as K;
        let kinds = |of: &dyn Fn(&GeneratedRow) -> Kind| {
            [
                K::Local,
                K::State,
                K::Everyone,
                K::Action,
                Kind::Create,
            ]
            .map(|k| g.rows.iter().filter(|r| of(r) == k).count())
        };
        // the rules' kinds (Local, State, Everyone, Action, Create), then the
        // overlay's: BlockActivation Local, the weather's release Everyone,
        // CreateDetectionEvent an action and the two placements state rows
        assert_eq!(kinds(&|r| r.derived.cells.kind), [800, 342, 239, 22, 5]);
        assert_eq!(kinds(&|r| r.cells.kind), [801, 341, 240, 23, 3]);
        let by_rule: Vec<(&str, usize)> = rule_counts(&g)
            .iter()
            .map(|(r, n, _)| (r.code(), *n))
            .collect();
        assert_eq!(
            by_rule,
            [
                ("K1", 644),
                ("K2", 127),
                ("K3", 29),
                ("K4", 22),
                ("K5", 134),
                ("K6", 5),
                ("K7", 239),
                ("K8", 208)
            ]
        );
    }

    #[test]
    fn the_overlay_reproduces_the_table() {
        let g = generated();
        let rows: Vec<TableRow> = g.table_rows().map(TableRow::of_generated).collect();
        let table: Vec<TableRow> = natives::TABLE.iter().map(TableRow::of).collect();
        assert_eq!(rows.len(), table.len());
        for (generated, committed) in rows.iter().zip(&table) {
            assert_eq!(generated, committed);
            assert_eq!(generated.line(), committed.line());
        }
        // the fifty-two live rows first, every other native after them
        // staged (S5), the dump's whole
        assert_eq!(table.len(), 1408);
        assert_eq!(table.iter().position(|r| r.staged), Some(52));
        assert!(table.iter().skip(52).all(|r| r.staged));
        assert_eq!(index_of(&rows), natives::INDEX.to_vec());
        // every live row is a decided entry's, or a stopgap's whose only cell
        // is a hook naming its own deleting condition (the scene starts,
        // V-scene's until step 5a): the table is decisions over the rules
        for r in g.live_rows() {
            match r.entry.map(|e| (e.class, e.cells)) {
                Some((Class::Decided { .. }, _)) => {}
                Some((Class::Stopgap { .. }, [Cell::Hook(hook)])) => assert!(hook.line().contains("deletes"), "{}", r.func),
                other => panic!("{}.{}: {other:?}", r.cls, r.func),
            }
        }
    }

    // the generated-table check (it replaces the parity test against the
    // platform's NativeRows.inc, which D-NT1 C retires): natives_table.rs is
    // what the rules and the overlay make, byte for byte
    #[test]
    fn the_generated_table_is_the_committed_one() {
        let g = generated();
        let made = table_source(&g);
        let committed = include_str!("natives_table.rs");
        if made != committed {
            let at = made
                .lines()
                .zip(committed.lines())
                .position(|(a, b)| a != b);
            panic!("natives_table.rs is stale (first differing line {at:?}): run `cargo run -p wire-rules --bin natives-gen`");
        }
        assert_eq!(made.matches("    Row {").count(), natives::TABLE.len());
        // and the client's file (step 4's S4: the TS lists read it)
        let ts = ts_source(&g);
        let committed = include_str!("../../../../skymp5-client/src/lib/nativeRows.generated.ts");
        if ts != committed {
            panic!("skymp5-client src/lib/nativeRows.generated.ts is stale: run `cargo run -p wire-rules --bin natives-gen`");
        }
    }

    // the client's file holds exactly the name lists it replaced
    // (scriptCallService.ts's refStateRows, standingRows, playerOwnRows and
    // playerItemRows; spSnippetService.ts's LOOK_ROWS, LOOK_ONLY_FNS and
    // GRAPH_VAR_KINDS), each now a column's
    #[test]
    fn the_client_file_holds_the_lists_it_replaced() {
        let ts = ts_source(&generated());
        let section = |from: &str| -> Vec<String> {
            let start = ts.find(from).unwrap_or_else(|| panic!("{from}"));
            ts.get(start..)
                .unwrap_or_default()
                .lines()
                .skip(1)
                .take_while(|l| l.starts_with("  "))
                .map(|l| l.trim().trim_end_matches(',').to_owned())
                .collect()
        };
        let mut families: Vec<String> = Vec::new();
        for (cls, funcs, family) in [
            ("ObjectReference", &["AddItem", "RemoveItem"][..], "own"),
            ("ObjectReference", &["Enable", "EnableNoWait", "Disable", "DisableNoWait", "Delete"][..], "ref"),
            ("Actor", &["SetFactionRank", "ModFactionRank", "RemoveFromFaction", "RemoveFromAllFactions", "SetCrimeFaction", "SetRelationshipRank"][..], "standing"),
            ("Actor", &["SetRace", "AddSpell", "RemoveSpell", "AddShout", "RemoveShout", "EquipItem", "ChangeHeadPart"][..], "own"),
        ] {
            families.extend(funcs.iter().map(|f| format!("\"{cls}.{f}\": \"{family}\"")));
        }
        let mut got = section("NATIVE_RECORD_FAMILIES");
        got.sort();
        families.sort();
        assert_eq!(got, families);
        assert_eq!(
            section("NATIVE_LOOK_RELAYS"),
            [
                "\"objectreference.playanimation\"",
                "\"debug.sendanimationevent\"",
                "\"objectreference.setanimationvariablebool\"",
                "\"objectreference.setanimationvariableint\"",
                "\"objectreference.setanimationvariablefloat\"",
                "\"objectreference.setmotiontype\"",
                "\"objectreference.applyhavokimpulse\"",
                "\"sound.play\"",
                "\"objectreference.createdetectionevent\"",
            ]
        );
        assert_eq!(section("NATIVE_LOOK_ONLY_FNS"), ["\"setmotiontype\"", "\"applyhavokimpulse\"", "\"createdetectionevent\""]);
        assert_eq!(section("NATIVE_GRAPH_VAR_KINDS"), ["setanimationvariablebool: 5", "setanimationvariableint: 3", "setanimationvariablefloat: 4"]);
        assert!(!ts.contains('\u{2014}'));
    }

    #[test]
    fn the_overlay_refuses_what_is_no_decision() {
        let d = dump();
        let decided = Class::Decided {
            cites: "ADR-062 :4020",
        };
        let refused = |entries: &[Entry]| generate(&d, entries).err().unwrap_or_default();
        // a native the dump does not declare (ObjectReference.Unlock: a
        // script unlocks with Lock(False))
        assert_eq!(
            refused(&[entry(
                "ObjectReference.Unlock",
                decided,
                &[Cell::Kind(natives::Kind::Local)]
            )]),
            [Refusal::Unknown("ObjectReference.Unlock")]
        );
        // a decided entry citing nothing, a stopgap with no deleting condition
        assert_eq!(
            refused(&[entry(
                "ObjectReference.BlockActivation",
                Class::Decided { cites: " " },
                &[Cell::Kind(natives::Kind::Local)]
            )]),
            [Refusal::Incomplete("ObjectReference.BlockActivation")]
        );
        assert_eq!(
            refused(&[entry(
                "ObjectReference.BlockActivation",
                Class::Stopgap {
                    replaced_by: "a rule",
                    deletes: ""
                },
                &[Cell::Kind(natives::Kind::Local)]
            )]),
            [Refusal::Incomplete("ObjectReference.BlockActivation")]
        );
        // a cell the rules already give, and an entry that sets nothing
        assert_eq!(
            refused(&[entry(
                "Quest.Start",
                decided,
                &[Cell::Kind(natives::Kind::Everyone)]
            )]),
            [Refusal::Unchanged {
                native: "Quest.Start",
                cell: "kind"
            }]
        );
        assert_eq!(
            refused(&[entry("Quest.Start", decided, &[])]),
            [Refusal::Unchanged {
                native: "Quest.Start",
                cell: "nothing"
            }]
        );
        // one native twice, one cell twice
        let local = &[Cell::Kind(natives::Kind::Local)];
        assert_eq!(
            refused(&[
                entry("ObjectReference.BlockActivation", decided, local),
                entry("objectreference.blockactivation", decided, local)
            ]),
            [Refusal::Twice("objectreference.blockactivation")]
        );
        assert_eq!(
            refused(&[entry(
                "ObjectReference.BlockActivation",
                decided,
                &[
                    Cell::Kind(natives::Kind::Local),
                    Cell::Kind(natives::Kind::Everyone)
                ]
            )]),
            [Refusal::Repeated {
                native: "ObjectReference.BlockActivation",
                cell: "kind"
            }]
        );
        // a twin the dump does not hold
        assert_eq!(
            refused(&[entry(
                "ObjectReference.MoveTo",
                decided,
                &[Cell::Relay(Relay::Twin {
                    cls: "ObjectReference",
                    func: "MoveToNoWait",
                    args: &[0]
                })]
            )]),
            [Refusal::NoTwin("ObjectReference.MoveTo")]
        );
    }

    #[test]
    fn the_stopgaps_name_their_rule_and_the_tsv_holds_every_native() {
        let g = generated();
        let stopgaps: Vec<&str> = OVERLAY
            .iter()
            .filter(|e| matches!(e.class, Class::Stopgap { .. }))
            .map(|e| e.native)
            .collect();
        assert!(
            stopgaps.contains(&"Faction.SetCrimeGold")
                && stopgaps.contains(&"Weather.ReleaseOverride"),
            "{stopgaps:?}"
        );
        // the player's own member natives take the acting seat's skip
        let row = |n: &str| {
            g.rows
                .iter()
                .find(|r| format!("{}.{}", r.native.cls, r.native.name).eq_ignore_ascii_case(n))
                .unwrap_or_else(|| panic!("{n}"))
        };
        assert_eq!(row("Quest.SetActive").cells.scoped, Scoped::ActingSeatAlone);
        assert_eq!(row("Quest.SetActive").derived.cells.scoped, Scoped::Runs);
        // the weather runs where its script runs until step 3's record
        assert_eq!(row("Weather.ReleaseOverride").cells.scoped, Scoped::Runs);
        let tsv = tsv(&g);
        assert_eq!(tsv.lines().count(), 1409);
        assert_eq!(tsv.lines().next(), Some(TSV_HEADER));
        assert!(tsv
            .lines()
            .all(|l| l.split('\t').count() == TSV_HEADER.split('\t').count()));
        assert!(!tsv.contains('\u{2014}'));
    }
}
