import { Inventory } from "skyrimPlatform";
import { MsgType } from "../../messages";

// thuum docs/verbs/traps.md (M4): a linked activation the cell's host
// reported (a plate firing its spike wall, a lever opening its portcullis),
// for this game's engine to play with the caster the host's did. Server to
// client, reliable, the cell's other games.
export interface RelayedActivationMessage {
    t: MsgType.RelayedActivation;
    target: number; // the activated reference's form id, never 0
    caster: number; // the reference that activated it, never 0
    // thuum docs/DECISIONS.md D-C2 A (docs/research/fifty-fixes-2026-11-07.md
    // 1(a)): 1 OnTriggerEnter, 2 OnTriggerLeave: a guest's entry into a world
    // trigger, sent to the cell's host alone, whose engine sends that event to
    // the target's scripts with the caster (the presser) as akActionRef
    // instead of activating; absent or null for an activation
    entry?: number | null;
    // since SCHEMA_VERSION 60 (thuum tracker #117, Eli's design B): on a
    // player's press of a scripted world object, sent to the cell's host
    // alone, the presser's inventory as the server records it, which this
    // seat's acting scope answers the press's GetItemCount from (the
    // keyhole's claw; a figure carries no inventory); absent otherwise
    presserInventory?: Inventory | null;
    // since SCHEMA_VERSION 69 (the one script channel's events): an act
    // another seat made on an object this seat owns, sent to the object's
    // scripts alone, never the engine's default processing: 1 OnActivate,
    // 2 OnHit, 3 OnItemAdded, 4 OnItemRemoved, 5 OnMagicEffectApply; with
    // the event's form (OnHit's source, an item event's base item, an
    // effect's apply's effect), OnHit's projectile and flags (1 power, 2
    // sneak, 4 bash, 8 blocked), an item event's count; absent on an
    // activation and an entry
    event?: number | null;
    source?: number | null;
    projectile?: number | null;
    count?: number | null;
    flags?: number | null;
}
