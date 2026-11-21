import { MsgType } from "../../messages";
import { CreatedForm } from "../../sync/createdForms";

export interface OnEquipMessage {
    t: MsgType.OnEquip,
    baseId: number,
    // a brewed potion drunk or a brewed poison put on a weapon, by its
    // definition; baseId is then its game-file template
    createdBase?: CreatedForm
}
