import {
  getExtraContainerChanges,
  getContainer,
  BaseExtraList,
  ExtraHealth,
  ObjectReference,
  TESModPlatform,
  Game,
  storage,
  ExtraCount,
  ExtraEnchantment,
  ExtraPoison,
  ExtraSoul,
  ExtraTextDisplayData,
  ExtraCharge,
  Enchantment,
  Potion,
  Actor,
  Ammo,
  printConsole,
  ActorBase,
  FormType,
  Form,
  dressBody,
  once,
  callNative,
} from "skyrimPlatform";
import { logLab } from "../logging";
import { ownName } from "./itemName";
import { CreatedForm, isSessionId, sameCreated, toLocalInventory } from "./createdForms";

export interface Extra {
  health?: number;
  enchantmentId?: number;
  maxCharge?: number;
  removeEnchantmentOnUnequip?: boolean;
  chargePercent?: number;
  name?: string;
  soul?: 0 | 1 | 2 | 3 | 4 | 5;
  poisonId?: number;
  poisonCount?: number;
  worn?: boolean;
  wornLeft?: boolean;
  // on the wire alone, a created form by its definition (sync/createdForms.ts):
  // the brewed potion or poison this stack is (then baseId is its game-file
  // template), the item's made enchantment (no enchantmentId then), a brewed
  // poison on the weapon (no poisonId then). An entry this engine reads names
  // its own forms by id instead
  createdBase?: CreatedForm;
  createdEnchantment?: CreatedForm;
  createdPoison?: CreatedForm;
  // the stack's owner, the engine's stolen mark (ExtraOwnership), as this
  // game holds it
  ownerId?: number;
}

export interface BasicEntry {
  baseId: number;
  count: number;
}

export type Entry = BasicEntry & Extra;

export interface Inventory {
  entries: Entry[];
}

// 'loxsword (Legendary)' => 'loxsword'
const getRealName = (s?: string): string => {
  if (!s) {
    return s as string;
  }

  const arr = s.split(" ");
  if (arr.length && arr[arr.length - 1].match(/^\(.*\)$/)) {
    arr.pop();
  }
  return arr.join(" ");
};

// 'aaaaaaaaaaaaaaaa' => 'aaa...'
const cropName = (s?: string): string => {
  if (!s) {
    return s as string;
  }

  const max = 128;
  return s.length >= max
    ? s
      .split("")
      .filter((x, i) => i < max)
      .join("")
      .concat("...")
    : s;
};

const checkIfNameIsGeneratedByGame = (
  aStr: string,
  bStr: string,
  formName: string
) => {
  if (!aStr.length && bStr.startsWith(formName)) {
    const bEnding = bStr.substr(formName.length);
    if (bEnding.match(/^\s\(.*\)$/)) {
      return true;
    }
  }
  return false;
};

const namesEqual = (a: Entry, b: Entry): boolean => {
  const aStr = a.name || "";
  const bStr = b.name || "";
  if (cropName(getRealName(aStr)) === cropName(getRealName(bStr))) {
    return true;
  }

  if (a.baseId === b.baseId) {
    const form = Game.getFormEx(a.baseId);
    if (form) {
      const formName = form.getName();
      if (
        checkIfNameIsGeneratedByGame(aStr, bStr, formName) ||
        checkIfNameIsGeneratedByGame(bStr, aStr, formName)
      )
        return true;
    }
  }

  return false;
};

// The record holds an item's health as the wire's f32 (Inventory.h,
// skymp.rs), so 1.1 comes back as 1.100000023841858, while extractExtraData
// reads the engine's to one decimal. Health is compared at that one decimal on
// both sides, so a tempered stack the record holds is the stack the engine
// holds, and an apply does not remove and re-add it
const sameHealth = (x: number | undefined, y: number | undefined): boolean =>
  x === undefined || y === undefined ? x === y : Math.round(x * 10) === Math.round(y * 10);

const extrasEqual = (a: Entry, b: Entry, ignoreWorn = false) => {
  return (
    sameHealth(a.health, b.health) &&
    a.enchantmentId === b.enchantmentId &&
    a.maxCharge === b.maxCharge &&
    !!a.removeEnchantmentOnUnequip === !!b.removeEnchantmentOnUnequip &&
    //a.chargePercent === b.chargePercent &&
    //namesEqual(a, b) &&
    a.soul === b.soul &&
    a.poisonId === b.poisonId &&
    a.poisonCount === b.poisonCount &&
    // a wire entry's created forms by their definition
    sameCreated(a.createdBase, b.createdBase) &&
    sameCreated(a.createdEnchantment, b.createdEnchantment) &&
    sameCreated(a.createdPoison, b.createdPoison) &&
    a.ownerId === b.ownerId &&
    ((!!a.worn === !!b.worn && !!a.wornLeft === !!b.wornLeft) || ignoreWorn)
  );
};

export const hasExtras = (e: Entry): boolean => {
  return !extrasEqual(e, { baseId: 0, count: 0 });
};

// two stacks of one base with the same extras, worn flags included: getDiff's
// match with ignoreWorn false
export const sameStack = (a: Entry, b: Entry): boolean => a.baseId === b.baseId && extrasEqual(a, b, false);

const extractExtraData = (
  refr: ObjectReference,
  extraList: BaseExtraList | null,
  out: Entry
): void => {
  // I see that ExtraWorn is not emitted for 0xFF actors when arrows are equipped. Fixing
  const item = Game.getFormEx(out.baseId);
  if (Ammo.from(item)) {
    const actor = Actor.from(refr);
    if (actor && actor.isEquipped(item)) {
      out.worn = true;
    }
  }

  (extraList || []).forEach((extra) => {
    switch (extra.type) {
      case "Health":
        out.health = Math.round((extra as ExtraHealth).health * 10) / 10;

        // TESModPlatform::AddItemEx makes all items at least 1.01 health
        if (out.health === 1) {
          delete out.health;
        }
        break;
      case "Count":
        out.count = (extra as ExtraCount).count;
        break;
      case "Enchantment":
        out.enchantmentId = (extra as ExtraEnchantment).enchantmentId;
        out.maxCharge = (extra as ExtraEnchantment).maxCharge;
        out.removeEnchantmentOnUnequip = (
          extra as ExtraEnchantment
        ).removeOnUnequip;
        break;
      case "Charge":
        out.chargePercent = (extra as ExtraCharge).charge;
        break;
      case "Poison":
        out.poisonId = (extra as ExtraPoison).poisonId;
        out.poisonCount = (extra as ExtraPoison).count;
        break;
      case "Soul":
        out.soul = (extra as ExtraSoul).soul;
        break;
      case "TextDisplayData":
        out.name = (extra as ExtraTextDisplayData).name;
        break;
      case "Worn":
        out.worn = true;
        break;
      case "WornLeft":
        out.wornLeft = true;
        break;
    }
    // the engine's stolen mark, an extra this fork's platform reads
    // ({type: "Ownership", owner}), by name since the client types against
    // the published platform package. Not on ammunition: addItemEx lays ammo
    // with no extra list, so an arrow's owner could never be written back,
    // and its record stays plain
    if ((extra.type as string) === "Ownership") {
      const owner = (extra as unknown as { owner?: number }).owner;
      if (owner && !Ammo.from(item)) {
        out.ownerId = owner;
      }
    }
  });
  // the name the stack keeps as its own (sync/itemName.ts): a tempered
  // stack's engine label is left to the engine, and its base's name is none
  const own = ownName(out.name, out.health, item ? item.getName() : "");
  if (own === undefined) {
    delete out.name;
  } else {
    out.name = own;
  }
};

const squash = (inv: Inventory): Inventory => {
  const res = new Array<Entry>();
  inv.entries.forEach((e) => {
    const same = res.find((x) => e.baseId === x.baseId && extrasEqual(x, e));
    if (same) {
      same.count += e.count;
    } else {
      res.push(JSON.parse(JSON.stringify(e)));
    }
  });
  // a count under one is a base-relative residue (a stack taken below the
  // base container's default), not an item; the wire's count is a u32
  // (thuum, playtest 31, 2026-10-22)
  return { entries: res.filter((x) => x.count > 0) };
};

const getExtraContainerChangesAsInventory = (
  refr: ObjectReference
): Inventory => {
  const extraContainerChanges = getExtraContainerChanges(refr.getFormID());
  const entries = new Array<Entry>();

  (extraContainerChanges || []).forEach((changesEntry) => {
    const entry: Entry = {
      baseId: changesEntry.baseId,
      count: changesEntry.countDelta,
    };

    (changesEntry.extendDataList || []).forEach((extraList) => {
      const e: Entry = {
        baseId: entry.baseId,
        count: 1,
      };
      extractExtraData(refr, extraList, e);
      entries.push(e);
      entry.count -= e.count;
    });

    if (entry.count !== 0) {
      entries.push(entry);
    }
  });

  let res: Inventory = { entries };
  res = squash(res);
  return res;
};

// thuum docs/verbs/npc-stream.md (fork m4-npc-worn; ADR-063, ADR-065): what
// a reference wears, as its engine records it: the base ids of its inventory
// changes' entries one of whose extra lists carries ExtraWorn or
// ExtraWornLeft (ADR-065: the state the engine already keeps), the flags
// CommonLibSSE-NG's Actor::GetWornArmor tests through InventoryEntryData::IsWorn
// (src/RE/A/Actor.cpp:642-660, src/RE/I/InventoryEntryData.cpp:223-234). One
// native call (getExtraContainerChanges, InventoryApi.cpp), no form lookups,
// so a poll can afford it for every actor a game simulates. An item the
// actor wears always has a changes entry: the equip writes the flag on one
// of its lists. Sorted by base id, each once.
export const readWorn = (refrId: number): number[] => {
  const worn = new Set<number>();
  (getExtraContainerChanges(refrId) || []).forEach((entry) => {
    if (!entry.baseId) {
      return;
    }
    for (const extraList of entry.extendDataList || []) {
      if ((extraList || []).some((extra) => extra.type === "Worn" || extra.type === "WornLeft")) {
        worn.add(entry.baseId);
        return;
      }
    }
  });
  return Array.from(worn).sort((a, b) => a - b);
};

const getBaseContainerAsInventory = (refr: ObjectReference): Inventory => {
  return {
    entries: getContainer((refr.getBaseObject() as ActorBase).getFormID()),
  };
};

export const sumInventories = (lhs: Inventory, rhs: Inventory): Inventory => {
  const leftEntriesWithExtras = lhs.entries.filter((e) => hasExtras(e));
  const rightEntriesWithExtras = rhs.entries.filter((e) => hasExtras(e));
  const leftEntriesSimple = lhs.entries.filter((e) => !hasExtras(e));
  const rightEntriesSimple = rhs.entries.filter((e) => !hasExtras(e));

  leftEntriesSimple.forEach((e) => {
    const matching = rightEntriesSimple.find((x) => x.baseId === e.baseId);
    if (matching) {
      e.count += matching.count;
      matching.count = 0;
    }
  });

  return {
    entries: leftEntriesWithExtras
      .concat(rightEntriesWithExtras)
      .concat(leftEntriesSimple)
      .concat(rightEntriesSimple)
      .filter((e) => e.count !== 0),
  };
};

export const removeSimpleItemsAsManyAsPossible = (
  inv: Inventory,
  baseId: number,
  count: number
): Inventory => {
  const res: Inventory = { entries: [] };
  res.entries = JSON.parse(JSON.stringify(inv.entries));

  const entry = res.entries.find((e) => !hasExtras(e) && e.baseId === baseId);
  if (entry) {
    entry.count -= count;
  }

  res.entries = res.entries.filter((e) => e.count > 0);
  return res;
};

export const getDiff = (
  lhs: Inventory,
  rhs: Inventory,
  ignoreWorn: boolean
): Inventory => {
  const lhsCopy: Inventory = JSON.parse(JSON.stringify(lhs));
  const rhsCopy: Inventory = JSON.parse(JSON.stringify(rhs));

  rhsCopy.entries.forEach((e) => {
    const sameFromLeft = lhsCopy.entries.find(
      (x) => x.baseId === e.baseId && extrasEqual(x, e, ignoreWorn)
    );
    if (sameFromLeft) {
      sameFromLeft.count -= e.count;
    } else {
      lhsCopy.entries.push(e);
      lhsCopy.entries[lhsCopy.entries.length - 1].count *= -1;
    }
  });

  return { entries: lhsCopy.entries.filter((x) => x.count !== 0) };
};

export const getInventory = (refr: ObjectReference): Inventory => {
  return squash(
    sumInventories(
      getBaseContainerAsInventory(refr),
      getExtraContainerChangesAsInventory(refr)
    )
  );
};

const basesReset = (): Set<number> => {
  if (storage["basesResetExists"] !== true) {
    storage["basesResetExists"] = true;
    storage["basesReset"] = new Set<number>();
  }
  return storage["basesReset"] as Set<number>;
};

// Whether the first apply of a game session has emptied this reference's
// inventory and laid the server's in again (resetBase below). Anything
// attached to an inventory entry before that (a favorite's mark) is thrown
// away with the entry (thuum docs/verbs/favorites.md).
export const isBaseReset = (refr: ObjectReference): boolean => {
  const base = refr.getBaseObject();
  return basesReset().has(base ? base.getFormID() : 0);
};

const resetBase = (refr: ObjectReference): void => {
  const base = refr.getBaseObject();
  const baseId = base ? base.getFormID() : 0;
  if (!basesReset().has(baseId)) {
    basesReset().add(baseId);
    TESModPlatform.resetContainer(base);

    refr.removeAllItems(null, false, true);
  }
};

// thuum playtest 38's finding 68 and playtest 48 ("Rotfern had to re-equip
// her bow again"): what an apply did, for the hands pass that may follow it
// (sync/equipment.ts applyPlayerHands). `changed` gathers the base id of
// every entry the apply added or removed, and the caller compares it with
// the items the hands hold, by id: a player's rows carry no worn flag (the
// server strips it at every put and take, ActionListener.cpp OnPutItem and
// OnTakeItem; the worn list is the equipment's, not the inventory's), so
// the worn-row test this report made before never rose and its hands pass
// never ran. A second signal beside the boolean applyInventory returns,
// which is crash protection's "every entry landed in one apply", not
// "something changed"
export interface ApplyInventoryReport {
  changed: Set<number>;
  // a created form the server's inventory names was still being made by
  // this engine, so its stack waits for a later apply
  pending?: boolean;
}

// each reference's latest apply while a retry for it waits, so the retry
// lands only while no newer inventory came for it. The number is one
// counter's for every apply, and an apply with nothing to wait for drops its
// reference's entry, so the map holds the references with a retry waiting
// alone
const applyGenerations = new Map<number, number>();
let applyCounter = 0;
// updates a retry waits for the engine's creator (queued on the main thread
// by createCreatedForm; assumes it answers within a frame or two), and the
// tries before the apply stops waiting
const createdRetryUpdates = 10;
const createdRetries = 5;

const retryAfterCreation = (refrId: number, generation: number, tries: number, apply: () => void): void => {
  let left = createdRetryUpdates;
  const wait = () => {
    if (--left > 0) {
      once("update", wait);
      return;
    }
    if (applyGenerations.get(refrId) !== generation) {
      return; // a newer inventory came; its own apply lands it
    }
    apply();
  };
  if (tries > 0) {
    once("update", wait);
  }
};

// thuum docs/DECISIONS.md D-R1 C: the bases an apply leaves as the engine
// holds them (applyInventory's `keep`), each with the side it keeps, by
// these bits: keepSurplus the engine's count above the target (a grant this
// seat reported and the server has not yet echoed), keepDeficit its count
// below it (a removal the same way). Every other base lands as before
export const keepSurplus = 1;
export const keepDeficit = 2;

const keptBy = (e: Entry, keep: ReadonlyMap<number, number>): boolean => {
  const sides = keep.get(e.baseId) ?? 0;
  return (e.count < 0 && (sides & keepSurplus) !== 0) || (e.count > 0 && (sides & keepDeficit) !== 0);
};

// One TESModPlatform.addItemEx of `count` of the entry's form `f` with the
// entry's extras as they are (its enchantment, charge, poison, soul, health,
// the name the server records), after pushWornState when the entry is worn;
// the caller queues the 3D update. Shared by applyInventory and the player's
// login equipment (sync/equipment.ts applyEquipment), so a worn stack with
// extras lands as the record has it and the inventory loop finds it so
export const addItemExEntry = (refr: ObjectReference, f: Form, e: Entry, count: number): void => {
  if (e.worn || e.wornLeft) {
    TESModPlatform.pushWornState(!!e.worn, !!e.wornLeft);
  }

  const addItemExArgs: [
    ObjectReference,
    Form,
    number,
    number,
    Enchantment | null,
    number,
    boolean,
    number,
    string,
    number,
    Potion | null,
    number,
  ] = [
    refr,
    f,
    count,
    e.health ? e.health : 1,
    e.enchantmentId
      ? Enchantment.from(Game.getFormEx(e.enchantmentId))
      : null,
    e.maxCharge ? e.maxCharge : 0,
    !!e.removeEnchantmentOnUnequip,
    e.chargePercent ? e.chargePercent : 0,
    // Only a name the server records: any name makes addItemEx build an
    // extra list (ExtraTextDisplayData, no ExtraCount) that the engine
    // counts as one item, so 3 Wheat would show as "Wheat" and "Wheat (2)".
    // extrasEqual ignores names, so the diff does not change.
    // the stack's own name alone: the engine adds a tempered stack's label
    // from its health (sync/itemName.ts), so a record that holds the label
    // too would show it twice
    cropName(ownName(e.name, e.health, f.getName()) ?? ""),
    e.soul ? e.soul : 0,
    e.poisonId ? Potion.from(Game.getFormEx(e.poisonId)) : null,
    e.poisonCount ? e.poisonCount : 0
  ];

  const argsToPrint = addItemExArgs.map((arg) => {
    if (arg instanceof ObjectReference) {
      return `ObjectReference(${arg.getFormID().toString(16)})`;
    } else if (arg instanceof Form) {
      return `Form(${arg.getFormID().toString(16)})`;
    } else {
      return JSON.stringify(arg);
    }
  });

  // the stack's owner, which the next addItemEx marks it with
  // (pushWornState's shape; this fork's native, called by name as the
  // client's other TESModPlatform ones)
  const owner = e.ownerId ? Game.getFormEx(e.ownerId) : null;
  if (owner) {
    callNative("TESModPlatform", "PushOwner", undefined, owner);
  }

  const ownerNote = owner ? ` owner ${owner.getFormID().toString(16)}` : "";
  printConsole(
    `TESModPlatform.addItemEx(${argsToPrint.join(", ")})${ownerNote}`
  );

  TESModPlatform.addItemEx(...addItemExArgs);
};

// the retries an inventory has left while its created forms are made
const createdTriesLeft = new WeakMap<Inventory, number>();

// The stolen mark is this game's own (R2, the server records it and never
// clears it): a stack the record holds unmarked where this game holds it
// marked (a pickup's, whose owner the record never heard) keeps the game's
// mark, as many as the record holds, rather than being swapped for a clean
// copy (a remove and an add the player would see)
const keepOwners = (record: Inventory, game: Inventory, ignoreWorn: boolean): Inventory => {
  const out: Inventory = JSON.parse(JSON.stringify(record));
  game.entries.forEach((g) => {
    if (!g.ownerId || out.entries.some((r) => r.baseId === g.baseId && extrasEqual(r, g, ignoreWorn))) {
      return;
    }
    const unmarked: Entry = { ...g, ownerId: undefined };
    const plain = out.entries.find(
      (r) => r.baseId === g.baseId && !r.ownerId && extrasEqual(r, unmarked, ignoreWorn)
    );
    if (!plain) {
      return;
    }
    const n = Math.min(plain.count, g.count);
    plain.count -= n;
    out.entries.push({ ...plain, count: n, ownerId: g.ownerId });
  });
  return { entries: out.entries.filter((e) => e.count > 0) };
};

export const applyInventory = (
  refr: ObjectReference,
  newInventory: Inventory,
  enableCrashProtection: boolean,
  ignoreWorn = false,
  resetBaseFirst = true,
  report?: ApplyInventoryReport,
  keep?: ReadonlyMap<number, number>
): boolean => {
  // thuum docs/verbs/npc-fate.md (playtest 34): a game-file body keeps the
  // engine's dressing, so its caller passes resetBaseFirst false and the
  // server's list lands as a diff alone (resetBase stripped the corpse to its
  // underwear at the host's first loot, and resetContainer on an NPC_ base
  // shared by many placements would empty that base's later spawns)
  if (resetBaseFirst) {
    resetBase(refr);
  }
  // the server's created forms in this engine's ids (sync/createdForms.ts),
  // made by its own creators where it holds none yet; a stack whose form is
  // still being made waits, and meanwhile no stack of a form this engine made
  // is taken away (it may be that one), then the apply runs again once the
  // creator has answered, unless a newer inventory came. The player's own apply
  // is its loop's (remoteServer.ts, which reads the report's pending and comes
  // back sooner, under its menu gate), so only another reference's (a
  // container's, a body's) retries here
  const refrId = refr.getFormID();
  const generation = ++applyCounter;
  const local = toLocalInventory(newInventory);
  if (local.pending && refrId !== 0x14) {
    applyGenerations.set(refrId, generation);
  } else {
    applyGenerations.delete(refrId);
  }
  if (local.pending) {
    if (report) {
      report.pending = true;
    }
    const tries = refrId === 0x14 ? 0 : createdTriesLeft.get(newInventory) ?? createdRetries;
    retryAfterCreation(refrId, generation, tries, () => {
      const again = ObjectReference.from(Game.getFormEx(refrId));
      if (again) {
        createdTriesLeft.set(newInventory, tries - 1);
        applyInventory(again, newInventory, enableCrashProtection, ignoreWorn, false, undefined, keep);
      }
    });
  }
  const createdHeld = (e: Entry) => isSessionId(e.baseId) || isSessionId(e.enchantmentId) || isSessionId(e.poisonId);
  const current = getInventory(refr);
  const diff = getDiff(keepOwners(local.inventory, current, ignoreWorn), current, ignoreWorn).entries
    .filter((e) => keep === undefined || !keptBy(e, keep))
    .filter((e) => !local.pending || e.count > 0 || !createdHeld(e));

  let res = true;

  diff.sort((a, b) => (a.count < b.count ? -1 : 1));
  diff.forEach((e, i) => {
    if (i > 0 && enableCrashProtection) {
      res = false;
      return;
    }
    let absCount = Math.abs(e.count);

    let queueNiNodeUpdateNeeded = false;

    const worn = !!e.worn;
    const wornLeft = !!e.wornLeft;

    let oneStepCount = e.count / absCount;

    const f = Game.getFormEx(e.baseId);
    if (!f) {
      return printConsole(`Bad form ID ${e.baseId.toString(16)}`);
    }
    const type = f.getType();
    // For misc items, potions and ingredients we don't want to split them into multiple items
    // This was made to fix a performance issue with users having 10000+ of misc items (i.e. gold)
    if (
      type === FormType.Misc ||
      type === FormType.Potion ||
      type === FormType.Ingredient
    ) {
      absCount = 1;
      oneStepCount = e.count;
    } else {
      // a large difference lands whole in one call, its sign kept: one step
      // of +1 left a surplus of arrows growing by one at every apply
      if (absCount > 1000) {
        absCount = 1;
        oneStepCount = e.count;

        // Also for arrows with strange count
        if (worn && e.count < 0) {
          absCount = 0;
        }
      }

      // ammunition lands in one call either way, a removal as an addition
      if (Math.abs(e.count) > 1 && Ammo.from(Game.getFormEx(e.baseId))) {
        absCount = worn && e.count < -1000 ? 0 : 1;
        if (e.count > 60000) {
          // Why would actor have 60k arrows?
          e.count = 1;
        }
        // the step is the count as the guard above left it
        oneStepCount = e.count;
      }
    }

    // an entry this apply lands (not one crash protection held back above,
    // nor the strange-count arrows it skips)
    if (report && absCount > 0) {
      report.changed.add(e.baseId);
    }

    for (let i = 0; i < absCount; ++i) {
      if (worn || wornLeft) {
        queueNiNodeUpdateNeeded = true;
      }

      addItemExEntry(refr, f, e, oneStepCount);
    }

    if (queueNiNodeUpdateNeeded) {
      const ac = Actor.from(refr);
      if (ac) {
        ac.queueNiNodeUpdate();
      }
    }
  });

  return res;
};

// thuum docs/verbs/npc-fate.md (playtest 48, the loot strip; Eli: "when i
// activate the person their armor is unequipped in their inventory stripping
// them naked"): a dead body's inventory is the record's, laid in as a diff
// alone (no base reset: resetBaseFirst false) with the worn flag left out of
// the match, so a worn stack counts toward its item's row and only the
// surplus over the record moves. A removal still goes by count alone
// (addItemEx with no extra list, TESModPlatform's RemoveItem with none), and
// which stack the engine takes is its own (HYPOTHESIS, H1 of the read: the
// worn one beside a loose copy), so after the diff the record's worn armor
// that the body holds but does not wear is equipped again by the platform
// (dressBody, FateApi.cpp: the one dress the fate's reconcile runs after a
// change-form lay too), never left loose. `worn` is the record's equipment
// as this seat holds it (the view's model); a piece the inventory no longer
// covers dresses nothing. Each removal of an item this body wears here is
// said, the lab's read of the rule (x-body-open-worn).
// thuum tracker #114 (playtest 53, Eli: "desynced bandit armor and some
// naked, armor flickering on search"): the match left the worn flag out but
// still compared every other extra (extrasEqual), so a stack this body holds
// whose extras the record's row lacks counted as another item. At 29602's
// shut (server.log 00:05:15.793) rotfern's apply removed and re-added every
// piece it held, IronWarhammer 0x13981, ArmorBanditHelmet 0x6f39e and
// ArmorBanditCuirass3 0x10594f, each "-1, 100" then "1, 1" (the engine's
// stacks carried ExtraHealth 100 from the corpse's change-form lay, the
// record's none; c1 console 00:05:13.506), and the removal unworn them while
// dressBody, given the record's own worn set, dressed 0 (the platform's
// "0 worn piece(s) of the record's 5 dressed again", 16:05:13:563). So the
// record's counts are laid onto the body's own stacks (bodyTarget): a stack
// it holds stands for the record's row as far as the row's count goes, with
// its own extras, the worn ones first, and only a count's surplus or deficit
// moves, the rule above
let bodyDressMissingSaid = false;
const bodyTarget = (record: Inventory, held: Inventory): Inventory => {
  const bases = new Set<number>();
  record.entries.forEach((e) => bases.add(e.baseId));
  held.entries.forEach((e) => bases.add(e.baseId));
  const entries = new Array<Entry>();
  bases.forEach((baseId) => {
    let want = 0;
    record.entries.forEach((e) => {
      if (e.baseId === baseId) {
        want += e.count;
      }
    });
    // one target row per set of extras, the worn flags left out as the
    // apply's match leaves them, so a worn stack and a loose one of the same
    // extras meet one row
    const take = (e: Entry) => {
      if (want <= 0) {
        return;
      }
      const n = Math.min(e.count, want);
      const row: Entry = JSON.parse(JSON.stringify(e));
      delete row.worn;
      delete row.wornLeft;
      row.count = n;
      const same = entries.find((x) => x.baseId === baseId && extrasEqual(x, row));
      if (same) {
        same.count += n;
      } else {
        entries.push(row);
      }
      want -= n;
    };
    held.entries
      .filter((e) => e.baseId === baseId)
      .sort((a, b) => Number(!!b.worn || !!b.wornLeft) - Number(!!a.worn || !!a.wornLeft))
      .forEach(take);
    record.entries.filter((e) => e.baseId === baseId).forEach(take);
  });
  return { entries };
};
export const applyBodyInventory = (
  refr: ObjectReference,
  newInventory: Inventory,
  worn: number[]
): boolean => {
  const refrId = refr.getFormID();
  const hex = refrId.toString(16);
  const record = new Map<number, number>();
  newInventory.entries.forEach((e) => {
    record.set(e.baseId, (record.get(e.baseId) || 0) + e.count);
  });
  const held = getInventory(refr);
  const here = new Map<number, { worn: number; loose: number }>();
  held.entries.forEach((e) => {
    const row = here.get(e.baseId) || { worn: 0, loose: 0 };
    if (e.worn || e.wornLeft) {
      row.worn += e.count;
    } else {
      row.loose += e.count;
    }
    here.set(e.baseId, row);
  });
  here.forEach((row, baseId) => {
    const holds = record.get(baseId) || 0;
    const leaving = row.worn + row.loose - holds;
    if (row.worn > 0 && leaving > 0) {
      logLab("verb:npc-fate", `body ${hex}: ${leaving} of ${baseId.toString(16)} leave, worn here ${row.worn}; the record holds ${holds}${holds < row.worn ? ", fewer than this body wears" : ""}`);
    }
  });

  const landed = applyInventory(refr, bodyTarget(newInventory, held), false, true, false);

  // the platform's export, absent on an older dist
  if (typeof dressBody !== "function") {
    if (!bodyDressMissingSaid) {
      bodyDressMissingSaid = true;
      logLab("verb:npc-fate", `body ${hex}: this platform has no dressBody (an older dist); the record's worn armor is not dressed again after an apply`);
    }
    return landed;
  }
  try {
    dressBody(refrId, worn);
  } catch (e) {
    logLab("verb:npc-fate", `body ${hex}: dressBody threw: ${e}`);
  }
  return landed;
};
