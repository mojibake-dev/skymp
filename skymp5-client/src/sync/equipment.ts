import {
  Actor,
  Ammo,
  Game,
  ObjectReference,
  Shout,
  Spell,
  Ui,
  Utility,
  setInventory,
} from 'skyrimPlatform';

import { logTrace } from '../logging';
import { Entry, Inventory, getInventory } from './inventory';

export const enum SpellType {
  Left,
  Right,
  Voice,
  Instant,
}

export const getEquipedSpell = (
  refr: ObjectReference,
  spellType: SpellType,
): number => {
  const actor = Actor.from(refr);

  if (!actor) {
    return 0;
  }

  switch (spellType) {
    case SpellType.Left: {
      const spell = actor.getEquippedSpell(SpellType.Left);
      return spell ? spell.getFormID() : 0;
    }
    case SpellType.Right: {
      const spell = actor.getEquippedSpell(SpellType.Right);
      return spell ? spell.getFormID() : 0;
    }
    case SpellType.Voice: {
      // thuum docs/verbs/shouts.md: the voice slot holds a power (a spell)
      // or a shout, which getEquippedSpell does not return
      const spell = actor.getEquippedSpell(SpellType.Voice);
      if (spell) {
        return spell.getFormID();
      }
      const shout = actor.getEquippedShout();
      return shout ? shout.getFormID() : 0;
    }
    case SpellType.Instant: {
      const spell = actor.getEquippedSpell(SpellType.Instant);
      return spell ? spell.getFormID() : 0;
    }
    default: {
      return 0;
    }
  }
};

export interface Equipment {
  inv: Inventory;
  leftSpell?: number;
  rightSpell?: number;
  voiceSpell?: number;
  instantSpell?: number;
  numChanges: number;
}

const filterWorn = (inv: Inventory): Inventory => {
  return { entries: inv.entries.filter((x) => x.worn || x.wornLeft) };
};

const removeUnnecessaryExtra = (inv: Inventory, ignoreAmmo: boolean): Inventory => {
  // thuum docs/verbs/marksman.md: a figure wears its quiver, as many arrows
  // as never run out (Eli, 2026-10-17: the shooter showed no arrows on the
  // other screen since playtest thirteen took the figure's). Its bow
  // releasing in this game launches an arrow of its own beside the one the
  // server's ArrowShot launches along the shooter's aim and draw
  // (x-double-arrow2 20261012-022726: two launches for one shot); Skyrim
  // Platform's launch hook drops that one (FridaHooks.cpp), so only the
  // relayed arrow flies. The player keeps its own, as many as it has.
  return {
    entries: inv.entries.map((x) => {
      const r: Entry = JSON.parse(JSON.stringify(x));
      r.chargePercent = r.maxCharge;
      if (ignoreAmmo) {
        // thuum (playtest 31, 2026-10-22): the engine's container changes
        // count a stack against the base container, so a kit given below
        // the base's default reads negative (iron arrows -23, gold -10 on
        // both lab seats); the wire's count is a u32 and refused the whole
        // equipment report on every send (MpClientPlugin-logs.txt, t5
        // E_JSON_SHAPE), so neither seat's gear reached the server. A worn
        // stack exists: one at least
        r.count = Ammo.from(Game.getFormEx(x.baseId)) ? Math.max(1, r.count) : 1;
      } else {
        r.count = Ammo.from(Game.getFormEx(x.baseId)) ? 1000 : 1;
      }
      delete r.name;
      return r;
    }),
  };
};

export const getEquipment = (ac: Actor, numChanges: number): Equipment => {
  return {
    inv: getInventory(ac),
    leftSpell: getEquipedSpell(ac, SpellType.Left),
    rightSpell: getEquipedSpell(ac, SpellType.Right),
    voiceSpell: getEquipedSpell(ac, SpellType.Voice),
    instantSpell: getEquipedSpell(ac, SpellType.Instant),
    numChanges,
  };
};

export const syncSpellEquipment = (
  ac: Actor,
  spellBaseId: number | undefined,
  spellType: SpellType,
) => {
  // a shout goes to the voice slot by its own call (thuum
  // docs/verbs/shouts.md)
  const isVoice = spellType === SpellType.Voice;
  if (spellBaseId !== undefined && spellBaseId > 0) {
    const form = Game.getFormEx(spellBaseId);
    const shout = isVoice ? Shout.from(form) : null;
    if (shout) {
      ac.equipShout(shout);
    } else {
      ac.equipSpell(Spell.from(form), spellType);
    }
  } else {
    const equipedSpell = ac.getEquippedSpell(spellType);

    if (equipedSpell) {
      ac.unequipSpell(equipedSpell, spellType);
    }

    const equipedShout = isVoice ? ac.getEquippedShout() : null;
    if (equipedShout) {
      ac.unequipShout(equipedShout);
    }
  }
};

// A reference the engine is letting go has nothing to dress, and the
// equipment natives on it crash the engine: a deleted one (a copy the
// cleaner or a respawn let go: c2's UnequipAll crash in x-handoff-door
// 20261024-001915, EXCEPTION_ACCESS_VIOLATION on "test 1" flagged
// kDeleted), and a disabled one on its way there (FormView.destroy disables
// a copy, latent until the engine has it out of its process lists, then
// deletes it, so between the two its AI process is gone and kDeleted is
// not yet set: c2's crash at rotfern's race change in thuum
// 20261114-083256, SkyrimSE+0x6D88F0 under Actor.UnequipAll reading the
// process's equipped object, its minidump read 2026-11-14). A figure the
// server holds disabled is dressed once it is enabled again: FormView
// keeps its apply pending until one lands.
export const cannotDress = (ac: Actor): boolean => ac.isDeleted() || ac.isDisabled();

// thuum docs/verbs/npc-stream.md (ADR-050, slice 3): a puppet dressed by its
// engine from the fate's roll keeps its outfit; its hands follow the host's
// record. The right hand is the worn weapon (a WEAP record among the worn
// entries), the left the wornLeft entry (a weapon, a shield, a torch); each
// hand is changed only when it differs from what the puppet holds, the item
// added to the puppet when its engine's roll lacks it (one count, silent; a
// look, not a loot), and the hands' spells follow as a figure's do.
// HYPOTHESIS H6 until a probe reads a hosted actor's draw on the puppet seat.
export const applyHands = (ac: Actor, eq: Equipment): void => {
  if (cannotDress(ac)) {
    return;
  }
  const typeOf = (baseId: number): number => Game.getFormEx(baseId)?.getType() ?? 0;
  const right = eq.inv.entries.find((e) => e.worn && typeOf(e.baseId) === 41)?.baseId ?? 0;
  const left = eq.inv.entries.find((e) => e.wornLeft)?.baseId ?? 0;
  // equipItemEx's slots: 1 the right hand, 2 the left; getEquippedObject's:
  // 1 the right hand, 0 the left
  const hands: Array<{ slot: number; location: number; baseId: number }> = [
    { slot: 1, location: 1, baseId: right },
    { slot: 2, location: 0, baseId: left },
  ];
  for (const hand of hands) {
    const current = ac.getEquippedObject(hand.location)?.getFormID() ?? 0;
    if (current === hand.baseId) {
      continue;
    }
    if (current !== 0) {
      const held = Game.getFormEx(current);
      if (held) {
        ac.unequipItem(held, false, true);
      }
    }
    if (hand.baseId !== 0) {
      const form = Game.getFormEx(hand.baseId);
      if (form) {
        if (ac.getItemCount(form) < 1) {
          ac.addItem(form, 1, true);
        }
        ac.equipItemEx(form, hand.slot, false, false);
      }
    }
  }
  syncSpellEquipment(ac, eq.leftSpell, SpellType.Left);
  syncSpellEquipment(ac, eq.rightSpell, SpellType.Right);
};

// a player's figure: a copy on the Player base 0x7 or on a base this game
// made for its appearance (an id of 0xff000000 and up); never an actor from
// the game files (npc-puppet's puppets keep their own spells)
export const isFigureBase = (ac: Actor): boolean => {
  const base = ac.getBaseObject();
  if (!base) {
    return false;
  }
  const baseId = base.getFormID();
  return baseId === 0x7 || baseId >= 0xff000000;
};

// thuum docs/DECISIONS.md ADR-046 (2026-10-23), decision 2: a figure carries
// only what the server gives it. A distributor (SPID) hands every non-player
// actor the layer's abilities at its load, and a figure is such an actor to
// it, so a figure would run a resident's cloaks against the real player
// (Stealth Detection Fixes' madDetectionCloak on every NPC; p-spell-bolt's
// detection probes, 2026-10-23). The actor's added spells (Actor.getNthSpell
// lists them, not the base's or the race's) are kept to the ones the server
// equips; the rest are removed. Returns how many went. keepAlso: the spells
// the figure's race overlay adds (thuum ADR-068 rung 5, D-T1 A;
// sync/raceOverlay.ts), the server's too.
export const keepOnlyServerSpells = (ac: Actor, eq: Equipment, keepAlso: readonly number[] = []): number => {
  const keep = new Set<number>([eq.leftSpell ?? 0, eq.rightSpell ?? 0, eq.voiceSpell ?? 0, eq.instantSpell ?? 0, ...keepAlso]);
  const foreign: Spell[] = [];
  const count = ac.getSpellCount();
  for (let i = 0; i < count; ++i) {
    const spell = ac.getNthSpell(i);
    if (spell && !keep.has(spell.getFormID())) {
      foreign.push(spell);
    }
  }
  for (const spell of foreign) {
    ac.removeSpell(spell);
  }
  return foreign.length;
};

// thuum docs/verbs/marksman.md, finding 45 (playtest 37): after a relaunch
// the player's record lost the weapons in its hands. applyEquipment's
// setInventory lists every worn entry with its hand, but the Skyrim Platform
// native equips with forceEquip only for an actor other than 0x14
// (InventoryApi.cpp SetInventory), and on the player armor sticks while
// weapons do not; upstream knew (sendInputsService.ts onLoadGame: "Currently
// only armor is equipped after relogging"). Three seconds after the load that
// service reports the engine's hands, empty, the server replaces the record's
// equipment whole, and the bow is gone: every shot was judged against an
// empty hand. So the player's hands are set the way applyHands sets a
// puppet's: the worn weapon to the right hand, the wornLeft entry to the
// left, the worn ammo by equipItem, each only when the hand does not hold it
// already and the item is in the inventory. Nothing is added here; the
// inventory sync brings the items, and one not landed yet is the second
// apply's, 0.3 s later (remoteServer.ts applyPcInv). The delay: a Papyrus
// native called from here runs on the game thread at once, but setInventory
// queues its adds on gameThrQ, drained after the JS tick (skyrim_platform
// main.cpp OnUpdate), so a count read in applyEquipment's own tick sees the
// inventory removeAllItems just emptied; this runs a tenth of a second
// later, as sync/appearance.ts defers its own.
//
// Playtest 48 (2026-11-06, "Rotfern had to re-equip her bow again", as in
// playtest 34 and 38's finding 68): after a relaunch her first draw ended in
// attackStop with no arrow (results/playtest-48 trace-c1-1: bowAttackStart
// at 281.7 s, about 20:21:00Z, and bBowDrawn never rose), and her own
// unequip and re-equip of the bow (iRightHandType 7, 0, 7 at 290.6 s) made
// the next shot launch. A login's wipe leaves both reads this pass trusts
// unreliable: the platform's SetInventory equips every worn entry, the ammo
// too, into the RightHand slot, unforced for the player (InventoryApi.cpp
// SetInventory), where AddItemEx's ammo takes no slot
// (PapyrusTESModPlatform.cpp AddItemEx), so isEquipped can read true for a
// quiver the bow cannot nock from (HYPOTHESIS, 38's read); and in
// x-hands-relaunch v1 (20261029-092719, c1's console) the login's first pass
// equipped the ammo alone, the right hand reading the bow, while the apply
// 5 s later re-added the bow 0x3b562 as a plain item (HYPOTHESIS: the hand
// read a bow the inventory no longer held as worn). So a pass after a
// login's wipe (afterLoginWipe) does what her re-equip did: the right
// hand's weapon, read as held, is taken off and equipped again a tenth of a
// second later, and the ammo is equipped after it without asking isEquipped
// (equipPlayerHands below); any other pass keeps both checks, so a pass in
// play never takes a held weapon off, and a weapon already drawn stays in
// the hand (the loop's pass comes about 5 s after the login, and taking a
// drawn bow off would end the draw).
// HYPOTHESIS until x-hands-relaunch v2: a fresh equip of the bow and the
// ammo through the engine's own paths nocks the first draw after a
// relaunch. LATER: SetInventory's ammo through the ammo path with no slot,
// a platform change that every figure's quiver goes through as well
export const applyPlayerHands = (ac: Actor, eq: Equipment, afterLoginWipe = false): void => {
  if (cannotDress(ac)) {
    return;
  }
  const right = playerHandsOf(eq).right;
  if (afterLoginWipe && right !== 0 && !ac.isWeaponDrawn() && (ac.getEquippedObject(1)?.getFormID() ?? 0) === right) {
    const form = Game.getFormEx(right);
    if (form && ac.getItemCount(form) >= 1) {
      // the equip waits for the unequip to land: the two are separate
      // natives (Papyrus UnequipItem, SKSE's EquipItemEx), and their order
      // inside the engine's equip queue is not read
      ac.unequipItem(form, false, true);
      logTrace('Equipment', `player hands: right ${right.toString(16)} taken off after a login's wipe, equipped again in 0.1 s`);
      Utility.wait(0.1).then(() => {
        const again = Game.getPlayer();
        if (again) {
          equipPlayerHands(again, eq, true);
        }
      });
      return;
    }
  }
  equipPlayerHands(ac, eq, afterLoginWipe);
};

// the hands an equipment names for the player: the right hand the worn
// weapon (FormType.Weapon, 41), the left the wornLeft entry, the quiver the
// worn ammo (inventory.ts flags the equipped ammo worn); 0 for none
export const playerHandsOf = (eq: Equipment): { right: number; left: number; ammo: number } => {
  const typeOf = (baseId: number): number => Game.getFormEx(baseId)?.getType() ?? 0;
  return {
    right: eq.inv.entries.find((e) => e.worn && typeOf(e.baseId) === 41)?.baseId ?? 0,
    left: eq.inv.entries.find((e) => e.wornLeft)?.baseId ?? 0,
    ammo: eq.inv.entries.find((e) => e.worn && Ammo.from(Game.getFormEx(e.baseId)))?.baseId ?? 0,
  };
};

// whether an inventory apply that changed these base ids touched a hand the
// equipment names (remoteServer.ts's test before a hands pass, playtest 48)
export const handsTouchedBy = (eq: Equipment, changed: Set<number>): boolean => {
  const hands = playerHandsOf(eq);
  return [hands.right, hands.left, hands.ammo].some((baseId) => baseId !== 0 && changed.has(baseId));
};

// thuum playtest 38's finding 68 (its read: an arrow count drift from
// shooting re-adds the ammo and un-equips it): the ammo this player's engine
// has equipped among the base ids an inventory apply changed, as an
// equipment applyPlayerHands takes (the quiver alone, its isEquipped check
// kept), or undefined. Read in the apply's own tick: its adds and removes
// queue on gameThrQ and land after the tick (PapyrusTESModPlatform.cpp
// AddItemEx), so isEquipped reads the quiver as it stood before the apply;
// the first apply of a session resets the base at once (inventory.ts
// resetBase), and that apply is a login's, whose pass takes the login's
// equipment instead
export const equippedAmmoAmong = (ac: Actor, changed: Set<number>): Equipment | undefined => {
  const entries: Entry[] = [];
  changed.forEach((baseId) => {
    const form = Game.getFormEx(baseId);
    if (form && Ammo.from(form) && ac.isEquipped(form)) {
      entries.push({ baseId, count: 1, worn: true });
    }
  });
  return entries.length > 0 ? { inv: { entries }, numChanges: 0 } : undefined;
};

// applyPlayerHands' equips: each hand only when it does not hold its item,
// the ammo only when isEquipped reads false, unless the pass follows a
// login's wipe (afterLoginWipe), whose ammo is equipped whatever isEquipped
// reads (playtest 48 above)
const equipPlayerHands = (ac: Actor, eq: Equipment, afterLoginWipe: boolean): void => {
  if (cannotDress(ac)) {
    return;
  }
  const { right, left, ammo } = playerHandsOf(eq);
  const done: string[] = [];
  // equipItemEx's slots: 1 the right hand, 2 the left; getEquippedObject's:
  // 1 the right hand, 0 the left (as applyHands)
  const hands: Array<{ name: string; slot: number; location: number; baseId: number }> = [
    { name: 'right', slot: 1, location: 1, baseId: right },
    { name: 'left', slot: 2, location: 0, baseId: left },
  ];
  for (const hand of hands) {
    if (hand.baseId === 0) {
      continue;
    }
    const current = ac.getEquippedObject(hand.location)?.getFormID() ?? 0;
    if (current === hand.baseId) {
      continue;
    }
    const form = Game.getFormEx(hand.baseId);
    if (form && ac.getItemCount(form) >= 1) {
      ac.equipItemEx(form, hand.slot, false, false);
      done.push(`${hand.name} ${hand.baseId.toString(16)}`);
    }
  }
  if (ammo !== 0) {
    const form = Game.getFormEx(ammo);
    if (form && (afterLoginWipe || !ac.isEquipped(form)) && ac.getItemCount(form) >= 1) {
      ac.equipItem(form, false, true);
      done.push(`ammo ${ammo.toString(16)}`);
    }
  }
  if (done.length > 0) {
    logTrace('Equipment', `player hands re-equipped after ${afterLoginWipe ? "a login's wipe" : 'an inventory apply'}: ${done.join(', ')}`);
  }
};

export const applyEquipment = (ac: Actor, eq: Equipment, keepAlso: readonly number[] = []): boolean => {
  if (cannotDress(ac)) {
    return false;
  }
  ac.removeAllItems(null, false, true);

  ac.unequipAll();

  ac.removeAllItems(null, false, true);

  const newInventory = removeUnnecessaryExtra(filterWorn(eq.inv), ac.getFormID() === 0x14);

  setInventory(ac.getFormID(), newInventory);

  // finding 45: the player's hands, once setInventory's adds have landed,
  // as a pass after a login's wipe (applyPlayerHands above, playtest 48):
  // this apply is the player's only at a login (remoteServer.ts applyPcInv)
  if (ac.getFormID() === 0x14) {
    Utility.wait(0.1).then(() => {
      const player = Game.getPlayer();
      if (player) {
        applyPlayerHands(player, eq, true);
      }
    });
  }

  syncSpellEquipment(ac, eq.leftSpell, SpellType.Left);
  syncSpellEquipment(ac, eq.rightSpell, SpellType.Right);
  syncSpellEquipment(ac, eq.voiceSpell, SpellType.Voice);
  syncSpellEquipment(ac, eq.instantSpell, SpellType.Instant);

  // ADR-046: a figure's spells are the server's alone (its race overlay's
  // among them)
  if (ac.getFormID() !== 0x14 && isFigureBase(ac)) {
    keepOnlyServerSpells(ac, eq, keepAlso);
  }

  return true;
};

export const isBadMenuShown = (): boolean => {
  return (
    Ui.isMenuOpen('InventoryMenu') ||
    Ui.isMenuOpen('FavoritesMenu') ||
    Ui.isMenuOpen('MagicMenu') ||
    Ui.isMenuOpen('ContainerMenu') ||
    Ui.isMenuOpen('Crafting Menu') // Actually I don't think it causes crashes
  );
};
