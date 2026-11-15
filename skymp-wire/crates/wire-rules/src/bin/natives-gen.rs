//! natives-gen (thuum ADR-073 step 4, Eli's D-NT1 C): the natives table's
//! generator. It reads skyrim-platform's FunctionsDump.txt (never a native's
//! offset), derives every native's row by wire-rules' natives_rules and lays
//! the overlay over it, says once per rule how many natives it took and the
//! first (decision 7's merge rule: named once, then counted), and writes
//! wire-rules' natives_table.rs, the live table the server judges by and the
//! platform reads through wire-client-ffi's generated header, and since step
//! 4's S4 skymp5-client's src/lib/nativeRows.generated.ts, the columns the
//! client's services read in place of their own name lists.
//!
//! ```text
//! natives-gen [--dump <FunctionsDump.txt>] [--table <natives_table.rs>] [--client <nativeRows.generated.ts>] [--check] [--tsv <natives.tsv>]
//! ```
//!
//! `--check` writes nothing and exits 1 when the committed table or the
//! client's file is not what the rules and the overlay make (the
//! generated-table test's check, for a shell). `--tsv` writes every native's row for thuum's lab/ledger.py, which
//! joins it into docs/NATIVES.md. An overlay entry the rules refuse is named
//! and the tool exits 1.

use std::path::PathBuf;
use std::process::ExitCode;

use wire_rules::natives::Hook;
use wire_rules::natives_rules::{self, Class, OVERLAY};

/// The fork's FunctionsDump.txt, from this crate's directory.
const DUMP: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../skyrim-platform/src/platform_se/codegen/convert-files/FunctionsDump.txt"
);

/// The committed table, from this crate's directory.
const TABLE: &str = concat!(env!("CARGO_MANIFEST_DIR"), "/src/natives_table.rs");

/// The client's committed file, from this crate's directory.
const CLIENT: &str = concat!(
    env!("CARGO_MANIFEST_DIR"),
    "/../../../skymp5-client/src/lib/nativeRows.generated.ts"
);

struct Args {
    dump: PathBuf,
    table: PathBuf,
    client: PathBuf,
    check: bool,
    tsv: Option<PathBuf>,
}

fn args() -> Result<Args, String> {
    let mut out = Args {
        dump: PathBuf::from(DUMP),
        table: PathBuf::from(TABLE),
        client: PathBuf::from(CLIENT),
        check: false,
        tsv: None,
    };
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        let mut value = || {
            it.next()
                .map(PathBuf::from)
                .ok_or_else(|| format!("{arg} takes a path"))
        };
        match arg.as_str() {
            "--dump" => out.dump = value()?,
            "--table" => out.table = value()?,
            "--client" => out.client = value()?,
            "--check" => out.check = true,
            "--tsv" => out.tsv = Some(value()?),
            other => return Err(format!("unknown argument {other}; natives-gen [--dump <FunctionsDump.txt>] [--table <natives_table.rs>] [--client <nativeRows.generated.ts>] [--check] [--tsv <natives.tsv>]")),
        }
    }
    Ok(out)
}

fn run() -> Result<(), String> {
    let args = args()?;
    let text =
        std::fs::read_to_string(&args.dump).map_err(|e| format!("{}: {e}", args.dump.display()))?;
    // read once and kept for the run: the rows borrow the dump's names
    let text: &'static str = Box::leak(text.into_boxed_str());
    let dump = natives_rules::parse_dump(text).map_err(|e| e.to_string())?;
    let generated = natives_rules::generate(&dump, OVERLAY).map_err(|refusals| {
        let lines: Vec<String> = refusals
            .iter()
            .map(|r| format!("natives-gen: overlay refused: {r}"))
            .collect();
        lines.join("\n")
    })?;
    println!(
        "natives-gen: {} natives from {}",
        generated.rows.len(),
        args.dump.display()
    );
    for (rule, count, first) in natives_rules::rule_counts(&generated) {
        println!(
            "  {} {count} (first {}): {}",
            rule.code(),
            first.as_deref().unwrap_or("none"),
            rule.line()
        );
    }
    // each hook once, with its first native and its count (decision 7), and
    // its line under decision 5 (the "Content-specific code:" line its verb
    // doc owes)
    for hook in Hook::ALL.into_iter().filter(|h| *h != Hook::None) {
        let mut taken = generated.rows.iter().filter(|r| r.cells.hook == hook);
        let first = taken
            .next()
            .map(|r| format!("{}.{}", r.cls, r.func));
        println!(
            "  hook {} {} (first {}; {}): {}",
            hook.name(),
            usize::from(first.is_some()).saturating_add(taken.count()),
            first.as_deref().unwrap_or("none"),
            hook.verb(),
            hook.line()
        );
    }
    let decided = OVERLAY
        .iter()
        .filter(|e| matches!(e.class, Class::Decided { .. }))
        .count();
    println!(
        "  overlay: {} entries, {decided} decided, {} stopgaps; {} live rows",
        OVERLAY.len(),
        OVERLAY.len().saturating_sub(decided),
        generated.live.len()
    );
    let made = natives_rules::table_source(&generated).map_err(|refusals| {
        let lines: Vec<String> = refusals
            .iter()
            .map(|r| format!("natives-gen: table refused: {r}"))
            .collect();
        lines.join("\n")
    })?;
    let client = natives_rules::ts_source(&generated).map_err(|refusals| {
        let lines: Vec<String> = refusals
            .iter()
            .map(|r| format!("natives-gen: client file refused: {r}"))
            .collect();
        lines.join("\n")
    })?;
    for (path, made) in [(&args.table, &made), (&args.client, &client)] {
        let committed = std::fs::read_to_string(path).unwrap_or_default();
        if *made == committed {
            println!("  {} is up to date", path.display());
        } else if args.check {
            return Err(format!(
                "natives-gen: {} is stale: run `cargo run -p wire-rules --bin natives-gen`",
                path.display()
            ));
        } else {
            std::fs::write(path, made).map_err(|e| format!("{}: {e}", path.display()))?;
            println!("  wrote {}", path.display());
        }
    }
    if let Some(path) = args.tsv {
        std::fs::write(&path, natives_rules::tsv(&generated))
            .map_err(|e| format!("{}: {e}", path.display()))?;
        println!("  wrote {}", path.display());
    }
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(e) => {
            eprintln!("{e}");
            ExitCode::FAILURE
        }
    }
}
