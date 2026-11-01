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
}
