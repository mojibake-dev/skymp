// The worn rows a dress laid through addItemEx (rows carrying lasting
// extras: a temper's health, an enchantment, a poison) that the engine holds
// but does not wear once the adds have landed. The platform's AddItemEx
// equips a worn row only for a weapon, a shield-like piece or ammunition
// (PapyrusTESModPlatform.cpp, AddItemEx's needEquip), so a worn armor row
// lands in the inventory unworn. JS state alone.

export interface DressedRow {
    baseId: number;
    worn?: boolean;
    wornLeft?: boolean;
}

// the base ids to equip after the dress, each once
export const toEquipAfterDress = (rows: readonly DressedRow[], isEquipped: (baseId: number) => boolean): number[] => {
    const out: number[] = [];
    for (const r of rows) {
        if ((r.worn || r.wornLeft) && r.baseId !== 0 && out.indexOf(r.baseId) < 0 && !isEquipped(r.baseId)) {
            out.push(r.baseId);
        }
    }
    return out;
};
