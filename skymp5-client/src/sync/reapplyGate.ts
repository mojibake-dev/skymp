// A kept last animation's re-apply at a 3D load (ObjectAnimationService's
// pollOne): whether the kept event plays on the 3D now loaded or waits. A
// seat that hosts the reference's cell never replays it, since its script
// drives the graph and a replayed event moves the graph off the script's
// state; a record of the reference still being applied waits, its own play
// showing the view. JS state alone
export interface ReapplyFacts {
  // this seat hosts the reference's cell (HostService.epochForActor)
  hostedHere: boolean;
  // a script record of the reference is still being applied here
  viewPending: boolean;
  // the kept event is this seat's own script's play, which the seam answered
  // while the reference's 3D was not loaded here (the platform's `unloaded`)
  answered: boolean;
}

export const reapplyNow = (f: ReapplyFacts): boolean => !f.hostedHere && !f.viewPending;
