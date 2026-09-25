# skymp-wire

The network edge of skymp-parity, in Rust. Spec: ../../docs/WIRE.md. Decisions:
../../docs/DECISIONS.md (ADR-010, ADR-011, ADR-012). Rules that apply to every
file here: ../../.claude/rules/rust.md and ../../.claude/rules/wire.md.

Status (M0): schema, codec, validate, and transport are real and gated
(`cargo test`, clippy with the deny set, `cargo deny check`, three fuzz
targets with committed corpora); the bridge and the client cdylib compile
and wait for M1; difftest replays sessions into the wire edge and, through
the fork's fakeclient, into the legacy server. Dependency pins are ADR-015.
CI for this workspace is the fork's GitLab pipeline on sky-ci (wire-test and
bounded wire-fuzz per push, longer fuzz on the schedule).

Licensing: these crates are MIT on their own. wire-bridge builds into
skymp5-server (AGPLv3) and wire-client-ffi loads into skymp5-client (GPLv3),
so the combined works carry those licenses. See ADR-014.
