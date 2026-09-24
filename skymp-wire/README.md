# skymp-wire

The network edge of skymp-parity, in Rust. Spec: ../../docs/WIRE.md. Decisions:
../../docs/DECISIONS.md (ADR-010, ADR-011, ADR-012). Rules that apply to every
file here: ../../.claude/rules/rust.md and ../../.claude/rules/wire.md.

Status: skeleton. `cargo check` is the first M0 task; expect the placeholder
versions in Cargo.toml to need pinning and the renet API calls to need
checking against the pinned docs before anything compiles.

Licensing: these crates are MIT on their own. wire-bridge builds into
skymp5-server (AGPLv3) and wire-client-ffi loads into skymp5-client (GPLv3),
so the combined works carry those licenses. See ADR-014.
