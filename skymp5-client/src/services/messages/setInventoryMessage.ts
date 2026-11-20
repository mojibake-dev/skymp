import { Inventory } from "skyrimPlatform";
import { MsgType } from "../../messages";

export interface SetInventoryMessage {
    t: MsgType.SetInventory;
    inventory: Inventory;
    // thuum tracker #98 (SCHEMA_VERSION 62): the seq of this player's
    // CraftReport the inventory answers; absent otherwise
    craftAck?: number;
}
