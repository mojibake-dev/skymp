// The wire's own session family (thuum docs/verbs/mod-lock.md), keyed by
// "w" (the wire id) instead of a SkyMP MsgType: the client's Hello with one
// hash per plugin, the server's Welcome or Refuse. The wire DLL fills the
// schema version when the plugin omits it.
export interface HelloMessage {
  w: 0;
  client_build: number;
  mod_hashes: string[]; // 16 lowercase hex digits each (src/lib/modHash.ts)
  name: string;
}

export interface WelcomeMessage {
  w: 1;
  client_id: number;
  server_time_ms: number;
}

export interface RefuseMessage {
  w: 2;
  reason: number; // wire-transport's reason codes: 501 count, 502 mismatch, 503 no Hello
}
