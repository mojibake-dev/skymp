import { Actor, Game, Shout, Spell } from "skyrimPlatform";

// thuum docs/DECISIONS.md ADR-068 rung 5, D-T1 A (docs/research/
// rung5-quests-2026-11-09.md 8(b)): a race's form overlay. While a player
// wears a race that is neither Playable nor a twin of its recorded race (the
// werewolf's WerewolfBeastRace 0xcdd84), what that race's scripts added on
// the player's own seat (the beast's level abilities, its howls,
// WolfSkinFXArmor) rides the player's figure on every other seat: on the
// figure's CreateActor (props) and on each UpdateAppearance, which names the
// overlay that holds with its look (absent: none). It is never written to
// the player's change form, and it ends at the race's return or at the
// player's disconnect. The server's equipment gate counts it for the
// player's own reports. JSON field names as wire-schema's CreateActor props
// and UpdateAppearance read them
export interface RaceOverlay {
  spells: number[]; // the SPEL and SHOU forms the race's scripts added (abilities, howls)
  items: number[];  // the items they equipped (WolfSkinFXArmor)
}

// a bound on each list (rule 5: the server's word, still bounded here)
const OVERLAY_MAX = 64;
// load-order forms end where the server's copies begin
const LAST_GAME_FILE_ID = 0xfeffffff;

const isFormIds = (x: unknown): x is number[] =>
  Array.isArray(x) && x.length <= OVERLAY_MAX &&
  x.every((id) => typeof id === "number" && Number.isInteger(id) && id > 0 && id <= LAST_GAME_FILE_ID);

// the overlay a CreateActor or UpdateAppearance carried; undefined for none,
// for empty lists and for a malformed one
export const parseRaceOverlay = (x: unknown): RaceOverlay | undefined => {
  if (!x || typeof x !== "object") {
    return undefined;
  }
  const o = x as Record<string, unknown>;
  const spells = o["spells"] === undefined ? [] : o["spells"];
  const items = o["items"] === undefined ? [] : o["items"];
  if (!isFormIds(spells) || !isFormIds(items) || spells.length + items.length === 0) {
    return undefined;
  }
  return { spells: [...spells], items: [...items] };
};

// the overlay on a figure, through the engine's own Papyrus calls (from the
// update): what the last overlay put on and the new one lacks comes off, the
// new one's spells and shouts are added, its items added when the figure
// holds none and equipped. A figure keeps only the spells the server gives
// it (ADR-046 decision 2), so its sweep keeps these (keepOnlyServerSpells's
// keepAlso); its equipment's apply takes every item off, so the overlay goes
// on again after one (FormView)
export const applyRaceOverlay = (ac: Actor, wanted: RaceOverlay | undefined, had: RaceOverlay | undefined): void => {
  const want = wanted ?? { spells: [], items: [] };
  if (had) {
    had.spells.filter((id) => want.spells.indexOf(id) < 0).forEach((id) => {
      const form = Game.getFormEx(id);
      const shout = Shout.from(form);
      if (shout) {
        ac.removeShout(shout);
        return;
      }
      const spell = Spell.from(form);
      if (spell) {
        ac.removeSpell(spell);
      }
    });
    had.items.filter((id) => want.items.indexOf(id) < 0).forEach((id) => {
      const form = Game.getFormEx(id);
      if (form) {
        ac.unequipItem(form, false, true);
        ac.removeItem(form, ac.getItemCount(form), true, null);
      }
    });
  }
  want.spells.forEach((id) => {
    const form = Game.getFormEx(id);
    const shout = Shout.from(form);
    if (shout) {
      ac.addShout(shout);
      return;
    }
    const spell = Spell.from(form);
    if (spell && !ac.hasSpell(spell)) {
      ac.addSpell(spell, false);
    }
  });
  want.items.forEach((id) => {
    const form = Game.getFormEx(id);
    if (!form) {
      return;
    }
    if (ac.getItemCount(form) < 1) {
      ac.addItem(form, 1, true);
    }
    ac.equipItem(form, false, true);
  });
};
