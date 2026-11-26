// The bases an own script row on this seat's player holds against remoteServer.ts's inventory apply until the
// server's SetInventory carries the row (ScriptCallService.holdOwnItem), JS state alone: an AddItem's surplus
// (each landed base's, for a leveled list's roll), a RemoveItem's deficit.
export interface OwnItemCall {
    // the call's self: 0x14 for this seat's own player
    self: number;
    cls: string;
    fn: string;
    // the native table's record family of `${cls}.${fn}`
    family: string | undefined;
    // the first argument's form id, 0 for none
    item: number;
    // a leveled AddItem's landed bases (the platform's read of the roll)
    landed?: number[];
}

export interface OwnItemHold {
    formId: number;
    // the base's surplus held (a gain) or its deficit (a removal)
    keep: "surplus" | "deficit";
}

export const ownItemHolds = (c: OwnItemCall): OwnItemHold[] => {
    if (c.self !== 0x14 || c.cls !== "ObjectReference" || c.family !== "own") {
        return [];
    }
    if (c.fn === "AddItem" && c.landed !== undefined) {
        return c.landed.map((formId) => ({ formId, keep: "surplus" }));
    }
    if (c.item === 0) {
        return [];
    }
    return [{ formId: c.item, keep: c.fn === "AddItem" ? "surplus" : "deficit" }];
};
