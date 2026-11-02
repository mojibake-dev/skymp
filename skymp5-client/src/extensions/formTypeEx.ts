import { FormType } from "skyrimPlatform";

export class FormTypeEx {
  static isItem(type: FormType) {
    return type === FormType.Ammo ||
      type === FormType.Armor ||
      type === FormType.Book ||
      type === FormType.Ingredient ||
      type === FormType.Light ||
      type === FormType.Potion ||
      type === FormType.ScrollItem ||
      type === FormType.SoulGem ||
      type === FormType.Weapon ||
      type === FormType.Misc ||
      // thuum docs/verbs/locks.md: a key is held as an item (the server's
      // utils::IsItem names KEYM too)
      type === FormType.Key;
  }

  // thuum docs/verbs/prop-physics.md (ADR-058 slice two): what a physics
  // prop may be: an item, or a movable static. The server creates records
  // for the movable statics its settings list alone (propStatics, the
  // Riverwood cart wheel first), so a MovableStatic this seat was told
  // about is a prop; every other MSTT never reaches the prop scan
  static isProp(type: FormType) {
    return FormTypeEx.isItem(type) || type === FormType.MovableStatic;
  }
}
