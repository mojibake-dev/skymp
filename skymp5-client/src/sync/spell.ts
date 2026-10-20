import { Actor, Game, Spell, printConsole } from 'skyrimPlatform';

// thuum docs/verbs/player-globals.md's neighbour, the spells a player has
// learned (train-30, docs/PLAN.md Train-30's first row): the server's list is
// applied as a DIFF against what the actor has, never as a wipe and a
// re-add. Playtest twenty-seven (2026-10-20): on a reconnect with the game
// process up, removing every spell and adding the server's fifteen in the
// same frame left the player with its save's two base spells alone while a
// fresh launch, which removes nothing, applied all seventeen (the lab's
// reconnect-spells memo, H1). A spell the server lists stays untouched; a
// spell the actor added that the server does not list is removed; a listed
// spell the actor lacks is added; one summary line names the counts, and a
// second pass a second later adds whatever did not stick.

const spellIdsOf = (actor: Actor): number[] => {
  const ids = new Array<number>();
  for (let i = 0; i < actor.getSpellCount(); i++) {
    const spell = actor.getNthSpell(i);
    if (spell) {
      ids.push(spell.getFormID());
    }
  }
  return ids;
};

// Removes the actor's added spells the server does not list and adds the
// listed ones it lacks; returns what it changed and what it could not.
export const syncSpells = (
  actor: Actor,
  serverSpellIds: Array<number>,
): { removed: number; added: number; missing: number[] } => {
  const wanted = new Set<number>(serverSpellIds);
  const extra = spellIdsOf(actor).filter((id) => !wanted.has(id));
  let removed = 0;
  for (const id of extra) {
    const spell = Spell.from(Game.getFormEx(id));
    if (spell && actor.removeSpell(spell)) {
      removed++;
    }
  }
  let added = 0;
  const missing = new Array<number>();
  for (const id of serverSpellIds) {
    const spell = Spell.from(Game.getFormEx(id));
    if (!spell) {
      missing.push(id);
      continue;
    }
    if (actor.hasSpell(spell)) {
      continue;
    }
    if (actor.addSpell(spell, false)) {
      added++;
    } else {
      missing.push(id);
    }
  }
  printConsole(
    `syncSpells: ${serverSpellIds.length} listed, ${removed} removed, ${added} added, ${missing.length} missing` +
      (missing.length
        ? ` (${missing.map((id) => id.toString(16)).join(' ')})`
        : ''),
  );
  return { removed, added, missing };
};

export const removeAllSpells = (actor: Actor) => {
  let spellToRemove = new Array<Spell>();

  for (let i = 0; i < actor.getSpellCount(); i++) {
    const spell = actor.getNthSpell(i);

    if (spell) {
      spellToRemove.push(spell);
    }
  }

  for (let spell of spellToRemove) {
    const removeResult = actor.removeSpell(spell);
    printConsole(
      `removeResult: ${removeResult}, spellIdToRemove: ${spell
        .getFormID()
        .toString(16)}, spellName: ${spell.getName()}`,
    );
  }
};

export const learnSpells = (actor: Actor, spellsIds: Array<number>) => {
  for (let spellId of spellsIds) {
    const spell = Spell.from(Game.getFormEx(spellId));

    if (spell) {
      const addResult = actor.addSpell(spell, false);
      printConsole(
        `addResult: ${addResult}, spellIdToLearn: ${spell
          .getFormID()
          .toString(16)}, spellName: ${spell.getName()}`,
      );
    }
  }
};
