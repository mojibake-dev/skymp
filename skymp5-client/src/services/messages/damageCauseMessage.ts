import { MsgType } from "../../messages";

// thuum docs/verbs/npc-fate.md (ADR-050 decision 2, deaths by the engine):
// the server's number for a hit on an NPC this game hosts, as a cause: the
// victim (under its idx), the resulting health (absolute, 0 to 1), the
// aggressor's form id (0 for none) and whether the number killed through a
// headshot or a scripted kill. Server to the host, reliable; the host's
// engine applies it through its own damage entry (DamageCauseService,
// damageActor), so it reacts, records the killer and dies by itself at zero,
// which DeathReportService reports.
export interface DamageCauseMessage {
    t: MsgType.DamageCause;
    idx: number;
    attacker: number;
    health: number;
    killMove: boolean;
}
