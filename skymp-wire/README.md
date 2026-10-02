# skymp-wire

The network edge of skymp-parity, in Rust. Spec: ../../docs/WIRE.md. Decisions:
../../docs/DECISIONS.md (ADR-010, ADR-011, ADR-012, ADR-015, ADR-019). Rules that
apply to every file here: ../../.claude/rules/rust.md and ../../.claude/rules/wire.md.

Status (M1, branch `m1-wire`, ADR-019 proposed): SkyMP's own protocol is ported.

- wire-schema: the 33 SkyMP messages (MsgType 1 to 33, wire ids 9 to 41) beside
  the nine reserved M0 messages; bounded heap collections; a byte cap per
  message; a generic finite-float check.
- wire-json: SkyMP's JSON form, rendered and recognized; the only translation
  between the wire and the in-process consumers.
- wire-codec, wire-validate, wire-transport: decode with per-message caps and
  canonical re-encoding; finite floats and world bounds for every message;
  renet over netcode with the schema's direction table, per-send delivery,
  per-direction size caps, the server password in the connect token.
- wire-client-ffi: the cdylib is MpClientPlugin.dll, SkyMP's client DLL, with the
  same seven C exports and its own network thread; the rlib is the client the
  fakeclient runs.
- wire-bridge: the cxx bridge the C++ server polls for SkyMP JSON and sends its
  own through.
- wire-fakeclient: the lab's headless client (`fakeclient`), a drop-in for the
  C++ one it replaces.
- wire-legacy: a reader for SkyMP's old binary format (test tooling): every one
  of 92 captured packets from a green two-client lab run fits the schema.
- difftest: the legacy stack (RakNet server, C++ fakeclient) against the wire
  stack (bridged server, Rust fakeclient) on the same session.
- fuzz: codec_decode, validate (with the JSON round trip), json_recognize,
  transport_ingest.

Gates: `cargo test --workspace --all-features`, clippy with the deny set,
`cargo deny check`, the committed header (`just wire-header-check`), bounded fuzzing
per push and longer on the schedule (the fork's GitLab pipeline on sky-ci).

Licensing: these crates are MIT on their own. wire-bridge builds into
skymp5-server (AGPLv3) and wire-client-ffi loads into skymp5-client (GPLv3),
so the combined works carry those licenses. See ADR-014.
