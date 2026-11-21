// the client's own Extra (sync/inventory.ts), whose created forms the
// published typings' lacks
import { Extra } from "../../sync/inventory";
import { MsgType } from "../../messages";

export interface TakeItemMessage extends Extra {
    t: MsgType.TakeItem,
    baseId: number;
    count: number;
    target: number;
};
