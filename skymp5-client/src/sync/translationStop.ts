// Whether a figure's record glide is stopped now. The view moves another
// player's figure with TranslateTo, timed so a glide is still running when
// the next sample lands; a ragdoll that starts under it (a grab's, a throw's,
// a knock's) is left with its reference still gliding toward the record, and
// the body's keyframed parts (a tail) ride with the reference while the rest
// flies. Stopped once as the body goes down here (its engine's ragdoll, this
// seat's grip or stream, or a stream it follows), the engine's ragdoll
// carries the reference; a standing figure keeps its glide. JS state alone.

export const translationStopDue = (wasDown: boolean, down: boolean): boolean => {
    return down && !wasDown;
};
