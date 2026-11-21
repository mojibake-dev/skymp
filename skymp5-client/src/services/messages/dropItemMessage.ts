// A drop names its stack with the extras and created forms a put carries (the
// client's own Extra, sync/inventory.ts); the wire's DropItem has no worn flags
import { Extra } from "../../sync/inventory";
import { MsgType } from "../../messages";

export interface DropItemMessage extends Omit<Extra, "worn" | "wornLeft"> {
    t: MsgType.DropItem,
    baseId: number,
    count: number
}
