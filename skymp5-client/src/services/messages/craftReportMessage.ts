import { MsgType } from "../../messages";
import { Entry } from "../../sync/inventory";

// thuum tracker #98 (crafting; Eli's decision of 2026-11-16: relay the
// crafter's record): one sitting at a crafting bench as this game's engine
// read the player's inventory at the menu's open and its close. Client to
// server, reliable, R2: the server takes `lost` from the record when it holds
// every stack (else it refuses the report and sends the record back), adds
// `gained` as reported and keeps `learnedEnchantments`. Every stack is in the
// wire's shape: a created form by its definition (createdBase,
// createdEnchantment, createdPoison), never by this session's own 0xff... id
// (sync/createdForms.ts toWireEntry). The contract is skymp-wire's
// (wire-schema skymp.rs, CraftReport, MsgType 84).
export interface CraftReportMessage {
    t: MsgType.CraftReport;
    workbench: number; // the bench as this game's view names it to the server
    seq: number; // the report's number in this session, from 1; the server's answer (SetInventory's craftAck) names it
    lost: Entry[]; // the stacks that left the player's inventory
    gained: Entry[]; // the stacks that came into it
    learnedEnchantments: number[]; // the game-file enchantments the sitting taught
}
