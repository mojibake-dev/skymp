// The charge a dropped stack keeps, from the engine's own dropped reference
// read before it goes (its ExtraCharge, which the engine's drop copies from
// the stack: TESObjectREFR::RemoveItem's drop path, 16059 then 11591, the
// re-analyst's REA read, HYPOTHESIS): the charge left when it is short of the
// reference's full one; undefined for a full one or an item with none. An
// equipped staff's or weapon's charge sits in its hand's actor value until it
// leaves the hand, so the stack read before the drop carries none
export const dropCharge = (charge: number, maxCharge: number): number | undefined =>
  Number.isFinite(charge) && Number.isFinite(maxCharge) && maxCharge > 0 && charge >= 0 && charge < maxCharge
    ? charge
    : undefined;
