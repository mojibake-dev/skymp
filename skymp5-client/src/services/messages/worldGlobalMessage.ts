import { MsgType } from "../../messages";

// thuum docs/DECISIONS.md ADR-068 rung 4 and amendment B (the state rows;
// docs/research/rung4-state-rows-2026-11-07.md (b)): the world's global
// variables, every global no per-player list and no clock owns, R2: one
// streaming seat computes, the server records. Both directions, reliable.
// From the server: the stream word (`stream` set) names this seat the world's
// streamer at its epoch (true) or ends its stream (false), at each election
// of ADR-066's one seat (wire-rules world_streamer); a relay of the
// streamer's report; and the world's record served after a login's player
// globals (served true). To the server: the streamer's once-a-second diff of
// the GLOB array (TESModPlatform.DiffWorldGlobals), its epoch the stream
// word's (WorldGlobalService). The latest value wins per global and an equal
// one is folded. The validator: at most 512 entries, a global not 0 and
// below 0xff000000, each named once, every value finite (E_VAL_NONFINITE);
// from a client never `stream`, never served, never epoch 0. JSON field names
// as wire-schema's WorldGlobal reads them
export interface WorldGlobalMessage {
    t: MsgType.WorldGlobal;
    epoch: number;    // the stream's epoch: the stream word's, a report's (never 0 from a client)
    stream?: boolean; // the stream word, from the server alone: true streams from this seat at the epoch, false ends it; absent on every other message
    entries: Array<{
        global: number; // the global's form id, a load-order one
        value: number;  // its value (a float)
    }>;
    served: boolean;  // false for a report, a relay or a stream word; true for the record served at a login
}
