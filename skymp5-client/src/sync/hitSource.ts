// What a hit's source is, for whether the hit is reported: a weapon, a spell,
// a scroll, or a staff's: an equipped staff casts its enchantment, and its
// projectile's hit names that enchantment as its source (the server claims
// it against the staff's cast, which names the same enchantment). Any other
// enchantment is none: a weapon's arrives as its weapon's hit
export type HitSourceKind = "weapon" | "spell" | "scroll" | "staff" | "none";

export const hitSourceKind = (source: {
  weapon: boolean;
  spell: boolean;
  scroll: boolean;
  // the source's id when it is an enchantment, else 0
  enchantment: number;
  // the enchantments of the staffs in the aggressor's hands
  staffEnchantments: ReadonlyArray<number>;
}): HitSourceKind => {
  if (source.weapon) {
    return "weapon";
  }
  if (source.spell) {
    return "spell";
  }
  if (source.scroll) {
    return "scroll";
  }
  if (source.enchantment !== 0 && source.staffEnchantments.indexOf(source.enchantment) >= 0) {
    return "staff";
  }
  return "none";
};
