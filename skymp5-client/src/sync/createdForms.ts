import { createCreatedForm, createdFormTemplate, on, readCreatedForm } from "skyrimPlatform";
import type { Entry, Inventory } from "./inventoryDiff";
import { createdKey, sameCreated } from "./createdFormKey";
import type { CreatedEffect, CreatedForm } from "./createdFormKey";
export { createdKey, sameCreated } from "./createdFormKey";
export type { CreatedEffect, CreatedForm } from "./createdFormKey";
import { logLab } from "../logging";

// A brewed potion or poison and an enchantment made at the arcane enchanter
// are forms this engine invents, ids above 0xFEFFFFFF that name nothing on
// another game or after a relaunch, so the wire carries their definitions
// (wire-schema skymp.rs CreatedForm) and this module is the one place that
// turns this engine's form into a definition and a definition into this
// engine's form: a cache first, the platform second (readCreatedForm,
// createCreatedForm and createdFormTemplate, InventoryApi.cpp). Every entry
// this game sends goes through toWireEntry; every entry it applies goes
// through toLocalEntry. The engine behavior behind the platform's natives is
// assumed from a static read (InventoryApi.cpp).

// the kinds, the wire's CREATED_* constants
export const createdWeaponEnchantment = 0;
export const createdArmorEnchantment = 1;
export const createdPotion = 2;
export const createdPoison = 3;

// an id this engine made at runtime, never sent (the validator refuses one)
export const isSessionId = (id: number | undefined): boolean => id !== undefined && id > 0xfeffffff;

// bounded caches: a session makes a few dozen at most
const cacheMax = 1024;
const localByKey = new Map<string, number>();
const formByLocal = new Map<number, CreatedForm>();
const templateByKind = new Map<number, number>();
let platformMissingSaid = false;

// a load game clears the engine's created forms (assumed, as the platform's
// own cache assumes, InventoryApi.cpp), so the ids cached here go with it;
// the platform makes each again at its next ask
on("loadGame", () => {
  localByKey.clear();
  formByLocal.clear();
});

const remember = (localId: number, form: CreatedForm): void => {
  if (formByLocal.size >= cacheMax) {
    formByLocal.clear();
    localByKey.clear();
  }
  formByLocal.set(localId, form);
  localByKey.set(createdKey(form), localId);
};

const platformHasNatives = (): boolean => {
  // the platform's exports, absent on an older dist
  const ok = typeof readCreatedForm === "function" && typeof createCreatedForm === "function" && typeof createdFormTemplate === "function";
  if (!ok && !platformMissingSaid) {
    platformMissingSaid = true;
    logLab("verb:craft", "this platform has no created-form natives (an older dist): created forms stay this seat's own");
  }
  return ok;
};

// This engine's created form `localId` as a definition, or null when it is
// none this engine holds
export const definitionOf = (localId: number): CreatedForm | null => {
  const cached = formByLocal.get(localId);
  if (cached !== undefined) {
    return cached;
  }
  if (!isSessionId(localId) || !platformHasNatives()) {
    return null;
  }
  const read = readCreatedForm(localId);
  if (!read || read.effects.length === 0) {
    return null;
  }
  const form: CreatedForm = {
    kind: read.kind,
    effects: read.effects.map((e) => ({ mgef: e.mgef, magnitude: e.magnitude, area: e.area, duration: e.duration, cost: e.cost })),
  };
  remember(localId, form);
  return form;
};

// This engine's form for a definition: its id; 0 while the engine's
// creator, queued on the main thread, has not answered (ask again on a later
// update); -1 for a definition no creator takes
export const localIdOf = (form: CreatedForm): number => {
  const cached = localByKey.get(createdKey(form));
  if (cached !== undefined) {
    return cached;
  }
  if (!platformHasNatives()) {
    return -1;
  }
  const answer = createCreatedForm(form.kind, form.effects);
  if (answer > 0) {
    remember(answer, form);
  }
  return answer;
};

// The game-file template a created form of `kind` names on the wire, 0 when
// the engine has none
export const templateOf = (kind: number): number => {
  const cached = templateByKind.get(kind);
  if (cached !== undefined) {
    return cached;
  }
  if (!platformHasNatives()) {
    return 0;
  }
  const id = createdFormTemplate(kind);
  if (id > 0) {
    templateByKind.set(kind, id);
  }
  return id;
};

// An entry as this engine read it, in the wire's shape: a created base by
// its template and definition, a created enchantment or poison by its
// definition. Null when its base is a session form with no definition (the
// row cannot cross); a created enchantment or poison that cannot be read is
// left off the row with its charge or doses
export const toWireEntry = (entry: Entry): Entry | null => {
  const out: Entry = JSON.parse(JSON.stringify(entry));
  if (isSessionId(out.baseId)) {
    const form = definitionOf(out.baseId);
    const template = form && (form.kind === createdPotion || form.kind === createdPoison) ? templateOf(form.kind) : 0;
    if (!form || template <= 0) {
      return null;
    }
    out.baseId = template;
    out.createdBase = form;
  }
  if (isSessionId(out.enchantmentId)) {
    const form = definitionOf(out.enchantmentId as number);
    delete out.enchantmentId;
    if (form && (form.kind === createdWeaponEnchantment || form.kind === createdArmorEnchantment)) {
      out.createdEnchantment = form;
    } else {
      delete out.maxCharge;
      delete out.chargePercent;
      delete out.removeEnchantmentOnUnequip;
    }
  }
  if (isSessionId(out.poisonId)) {
    const form = definitionOf(out.poisonId as number);
    delete out.poisonId;
    if (form && form.kind === createdPoison) {
      out.createdPoison = form;
    } else {
      delete out.poisonCount;
    }
  }
  return out;
};

export interface LocalEntry {
  // the entry in this engine's ids, or null when it cannot land (a
  // definition no creator takes, or one still being made)
  entry: Entry | null;
  // a definition whose form the engine's creator has not answered yet
  pending: boolean;
}

// An entry from the server in this engine's ids: each created definition
// resolved to the form this engine made (or makes) for it
export const toLocalEntry = (entry: Entry): LocalEntry => {
  if (entry.createdBase === undefined && entry.createdEnchantment === undefined && entry.createdPoison === undefined) {
    return { entry, pending: false };
  }
  const out: Entry = JSON.parse(JSON.stringify(entry));
  let pending = false;
  let failed = false;
  const resolve = (form: CreatedForm): number => {
    const id = localIdOf(form);
    if (id === 0) {
      pending = true;
    } else if (id < 0) {
      failed = true;
    }
    return id;
  };
  if (out.createdBase !== undefined) {
    out.baseId = resolve(out.createdBase);
    delete out.createdBase;
  }
  if (out.createdEnchantment !== undefined) {
    out.enchantmentId = resolve(out.createdEnchantment);
    delete out.createdEnchantment;
  }
  if (out.createdPoison !== undefined) {
    out.poisonId = resolve(out.createdPoison);
    delete out.createdPoison;
  }
  if (pending || failed) {
    return { entry: null, pending };
  }
  return { entry: out, pending: false };
};

let droppedSaid = 0;

// An inventory this engine read, in the wire's shape (toWireEntry); rows
// that cannot cross are left out and said
export const toWireInventory = (inventory: Inventory): Inventory => {
  const entries: Entry[] = [];
  inventory.entries.forEach((e) => {
    const wire = toWireEntry(e);
    if (wire) {
      entries.push(wire);
    } else if (droppedSaid < 32) {
      ++droppedSaid;
      logLab("verb:craft", `a stack of session form ${e.baseId.toString(16)} has no definition this engine can read; left off the wire`);
    }
  });
  return { entries };
};

export interface LocalInventory {
  inventory: Inventory;
  // a row left out because its form is still being made
  pending: boolean;
}

// An inventory from the server in this engine's ids (toLocalEntry); a row
// that cannot land yet is left out, `pending` when it will on a later try
export const toLocalInventory = (inventory: Inventory): LocalInventory => {
  let pending = false;
  const entries: Entry[] = [];
  inventory.entries.forEach((e) => {
    const local = toLocalEntry(e);
    pending = pending || local.pending;
    if (local.entry) {
      entries.push(local.entry);
    }
  });
  return { inventory: { entries }, pending };
};

// The worn rows of an equipment in this engine's ids, for the dress at a
// login and a figure's (sync/equipment.ts applyEquipment). A row whose
// created form is still being made is dressed plain, its created fields and
// what rides on them left off (the inventory loop lays the made form when it
// lands); a brewed base that cannot land yet is left out. `pending` when a
// row was dressed plain or left out because its form is still being made (a
// figure, which no inventory loop lays again, waits for it instead;
// sync/equipment.ts applyEquipment)
export const toLocalWornInventory = (inventory: Inventory): LocalInventory => {
  const entries: Entry[] = [];
  let pending = false;
  inventory.entries.forEach((e) => {
    const local = toLocalEntry(e);
    if (local.entry) {
      entries.push(local.entry);
      return;
    }
    pending = pending || local.pending;
    if (e.createdBase !== undefined) {
      return;
    }
    const plain: Entry = JSON.parse(JSON.stringify(e));
    if (plain.createdEnchantment !== undefined) {
      delete plain.createdEnchantment;
      delete plain.maxCharge;
      delete plain.chargePercent;
      delete plain.removeEnchantmentOnUnequip;
    }
    if (plain.createdPoison !== undefined) {
      delete plain.createdPoison;
      delete plain.poisonCount;
    }
    entries.push(plain);
  });
  return { inventory: { entries }, pending };
};
