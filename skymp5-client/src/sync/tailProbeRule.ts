// Which of a figure's record moves the tail probe logs (movementApply's
// translateTo): the distance of the figure's first tail bone from its reference,
// read just before a record move and two frames after. Every teleport
// (setPosition), the first moves after the figure is made, and a glide
// (translateTo) that opens the tail by more than TAIL_OPEN_UNITS or leaves
// it past TAIL_FAR_UNITS; the rest is the steady walk and is not logged.
// JS state alone.

export const TAIL_FIRST_MOVES = 10;
export const TAIL_OPEN_UNITS = 50;
export const TAIL_FAR_UNITS = 200;

export interface TailProbeFacts {
    kind: "setPosition" | "translateTo";
    // the tail bone's distance from the root before and after the move
    before: number;
    after: number;
    // the figure's moves probed before this one
    moves: number;
}

export const tailProbeLogs = (f: TailProbeFacts): boolean => {
    return f.kind === "setPosition" || f.moves < TAIL_FIRST_MOVES
        || f.after - f.before > TAIL_OPEN_UNITS || f.after > TAIL_FAR_UNITS;
};
