// A kept last animation's re-apply at a 3D load (ObjectAnimationService's
// pollOne): whether the kept event plays on the 3D now loaded or waits. A
// seat that hosts the reference's cell never replays a relayed or recorded
// event, since its script drives the graph and a replayed event moves the
// graph off the script's state; but a play its own script made that the
// seam answered at once (no 3D here, or another player's press while this
// seat's graph advances only near its own player) never reached the graph,
// so it is shown once by the 3D poll, which puts the graph where the script
// already stands. A record of the reference still being applied
// waits, its own play showing the view. JS state alone
export interface ReapplyFacts {
  // this seat hosts the reference's cell (HostService.epochForActor)
  hostedHere: boolean;
  // a script record of the reference is still being applied here
  viewPending: boolean;
  // the kept event is this seat's own script's play, which the seam answered
  // at once (the platform's `answered`: no 3D here, or another player's press)
  answered: boolean;
}

export const reapplyNow = (f: ReapplyFacts): boolean => (!f.hostedHere || f.answered) && !f.viewPending;
