import { MsgType } from "../../messages";

// thuum docs/DECISIONS.md ADR-068 rung 4 and amendment B (the state rows;
// docs/research/rung4-state-rows-2026-11-07.md (b)): a quest alias's
// reference, AliasFill's ref half (its created items are the server's R0 read
// of QUST's ALCO and ALCA, which no seat reports). Both directions, reliable.
// To the server: an alias this seat's engine filled (ReferenceAlias.
// ForceRefTo) or cleared (Clear), an everyone row every seat runs under D-F1
// C, reported by whichever seat's scripts called it (served false); the
// server keeps the latest per quest and alias and folds an equal fill. From
// the server: another seat's fill relayed (served false), and the world's
// fills served after a login's quest stages (served true), which
// AliasFillService applies through the engine's own ForceRefTo or Clear. The
// validator: quest not 0 and below 0xff000000, alias at most 0xFFFF, refr
// below 0xff000000, served never true from a client. JSON field names as
// wire-schema's AliasFill reads them
export interface AliasFillMessage {
    t: MsgType.AliasFill;
    quest: number;   // the alias's quest, a load-order form id
    alias: number;   // the alias's id on its quest (Quest.GetAlias's), 0 to 0xFFFF
    refr: number;    // the reference the alias holds, a game-file reference's id; 0 for a cleared alias
    served: boolean; // false for a report or a relay, true for the record served at a login
}
