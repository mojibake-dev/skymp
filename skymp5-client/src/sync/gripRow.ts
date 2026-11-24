// Why a body an engine read named is not tracked where its engine has it
// (PropService's trackInPlace), or undefined when it is: no 3D in this game,
// or a world or cell other than the player's (0 for the player's reads as
// none, so any place passes). A body the player's own grab holds (a grip)
// skips the place read: the engine's grab is the body's place, at the hand,
// whatever world or cell its reference reads
export type PlaceRefusal = "not loaded" | "another world or cell";

// rowInPlace's refusals, PlaceRefusal's among them, as the grip's line says
// them
export type RowRefusal = "no remote id" | "no row kind" | "no trap row" | PlaceRefusal;

export const placeRefusal = (input: {
  loaded3D: boolean;
  playerWorldOrCell: number;
  refWorldOrCell: number;
  gripped: boolean;
}): PlaceRefusal | undefined => {
  if (!input.loaded3D) {
    return "not loaded";
  }
  if (!input.gripped && input.playerWorldOrCell !== 0 && input.refWorldOrCell !== input.playerWorldOrCell) {
    return "another world or cell";
  }
  return undefined;
};
