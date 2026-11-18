// thuum docs/verbs/spell-cast.md (x-stream-aim 20261017-135526: a figure's
// Scattershock missiles left 2.75 degrees off the caster's own). The engine
// reads an actor's aim heading at every relaunch of an aimed projectile, and
// a figure's was its body's. The view writes the figure's aim heading each
// update (formView.ts, setActorAimHeading).
//
// thuum tracker #165 (playtest 55: a figure's Flames "stays facing the other
// way", Eli's words, while its caster turned): the held value was the cast's
// absolute heading, from its first frame, so a held stream never turned with
// the body. What is held now is the caster's offset, its aim heading less its
// record's heading at the cast (radians, in [-pi, pi)), one per hand (0 left,
// 1 right), so one hand's end leaves the other hand's stream. The view adds
// it to the record's heading of the moment, as it takes the pitch from the
// movement. A standing cast still leaves along the cast's own heading. A
// turning one follows the body. HYPOTHESIS: the caster's own graph offset
// (AimHeadingCurrent) stays near its value at the cast through a hold;
// x-stream-aim measured it constant only while standing.
const offsets = new Map<number, Map<number, number>>();

const turn = 2 * Math.PI;

// the cast's aim heading and the caster's record heading, both radians; an
// offset that is not a number is not held
export const holdFigureAimOffset = (figure: number, hand: number, aimHeading: number, recordHeading: number): void => {
  const offset = ((((aimHeading - recordHeading + Math.PI) % turn) + turn) % turn) - Math.PI;
  if (!Number.isFinite(offset)) {
    return;
  }
  let hands = offsets.get(figure);
  if (!hands) {
    hands = new Map<number, number>();
    offsets.set(figure, hands);
  }
  // the newest cast last, the one the view reads
  hands.delete(hand);
  hands.set(hand, offset);
};

// the hand's cast ended
export const releaseFigureAimOffset = (figure: number, hand: number): void => {
  const hands = offsets.get(figure);
  if (!hands) {
    return;
  }
  hands.delete(hand);
  if (hands.size === 0) {
    offsets.delete(figure);
  }
};

// the offset of the figure's newest cast still running, undefined for none
export const figureAimOffset = (figure: number): number | undefined => {
  const hands = offsets.get(figure);
  if (!hands) {
    return undefined;
  }
  // a Map's own order (the client builds for es5, no iterators)
  let newest: number | undefined = undefined;
  hands.forEach((offset) => {
    newest = offset;
  });
  return newest;
};
