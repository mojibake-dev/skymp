// thuum tracker #98: the client's own Extra (sync/inventory.ts), whose
// created forms the published typings' lacks
import { Extra } from "../../sync/inventory";
import { MsgType } from "../../messages";

export interface PutItemMessage extends Extra {
    t: MsgType.PutItem,
    baseId: number;
    count: number;
    target: number;
};
