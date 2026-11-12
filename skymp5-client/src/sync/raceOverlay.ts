import { Actor, Game, Shout, Spell } from "skyrimPlatform";
import { Appearance } from "./appearance";
import { RaceOverlay } from "../services/messages/raceOverlay";

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
// player's own reports. Its wire shape, RaceOverlay, is the messages'
// (services/messages/raceOverlay.ts, RaceOverlay.h's pair); this module
// parses and applies it

// a bound on each list (rule 5: the server's word, still bounded here)
const OVERLAY_MAX = 64;
// load-order forms end where the server's copies begin
const LAST_GAME_FILE_ID = 0xfeffffff;

const isFormIds = (x: unknown): x is number[] =>
  Array.isArray(x) && x.length <= OVERLAY_MAX &&
  x.every((id) => typeof id === "number" && Number.isInteger(id) && id > 0 && id <= LAST_GAME_FILE_ID);

const isFormId = (x: unknown): x is number =>
  typeof x === "number" && Number.isInteger(x) && x > 0 && x <= LAST_GAME_FILE_ID;

// the overlay a CreateActor or UpdateAppearance carried; undefined for none
// (absent, open false, nothing in it) and for a malformed one
export const parseRaceOverlay = (x: unknown): RaceOverlay | undefined => {
  if (!x || typeof x !== "object") {
    return undefined;
  }
  const o = x as Record<string, unknown>;
  if (o["open"] === false) {
    return undefined;
  }
  const spells = o["spells"] === undefined ? [] : o["spells"];
  const shouts = o["shouts"] === undefined ? [] : o["shouts"];
  const items = o["items"] === undefined ? [] : o["items"];
  const race = o["race"];
  if (!isFormIds(spells) || !isFormIds(shouts) || !isFormIds(items) || (race !== undefined && race !== 0 && !isFormId(race))) {
    return undefined;
  }
  const out: RaceOverlay = { spells: [...spells], shouts: [...shouts], items: [...items] };
  if (isFormId(race)) {
    out.race = race;
  }
  return out.race !== undefined || spells.length + shouts.length + items.length > 0 ? out : undefined;
};

// the look a figure wears under an overlay: its race the overlay's when the
// overlay names one (the server may carry the worn race on the overlay
// alone, the record's look keeping the recorded race); the look itself
// otherwise. remoteServer keeps the result on the form's model, so the
// figure is rebuilt on the race as on any appearance change
export const withOverlayRace = (appearance: Appearance | undefined, overlay: unknown): Appearance | undefined => {
  const o = parseRaceOverlay(overlay);
  if (!appearance || !o || o.race === undefined || o.race === appearance.raceId) {
    return appearance;
  }
  return { ...appearance, raceId: o.race };
};

// the overlay on a figure, through the engine's own Papyrus calls (from the
// update): what the last overlay put on and the new one lacks comes off, the
// new one's spells and shouts are added, its items added when the figure
// holds none and equipped (a shout listed with the spells is added as a
// shout). A figure keeps only the spells the server gives
// it (ADR-046 decision 2), so its sweep keeps these (keepOnlyServerSpells's
// keepAlso); its equipment's apply takes every item off, so the overlay goes
// on again after one (FormView)
export const applyRaceOverlay = (ac: Actor, wanted: RaceOverlay | undefined, had: RaceOverlay | undefined): void => {
  const want = wanted ?? { spells: [], shouts: [], items: [] };
  const wantMagic = [...want.spells, ...want.shouts];
  if (had) {
    [...had.spells, ...had.shouts].filter((id) => wantMagic.indexOf(id) < 0).forEach((id) => {
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
  wantMagic.forEach((id) => {
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
