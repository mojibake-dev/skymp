// An actor's turn as one seat's engine simulates it and another seat's copy
// plays it. The turn rate is degrees per second as the engine's movement
// step left it: minus MiddleHighProcessData::rotationSpeed.z, the value the
// actor's graph reads as TurnDelta (docs/research/m3/re-puppet-locomotion.md
// in the thuum repo). A copy's engine is fed the rate and the sample's
// heading; its movement-step detour turns the copy toward that heading at
// the rate every frame and writes the rate where TurnDelta reads it
// (skyrim-platform PuppetHooks.cpp), so the copy turns as the simulating
// engine turned it rather than jumping to each sample's heading.

export interface StepTurn {
  turnRateDeg: number;
}

// The turn rate a hosted actor's sample carries: its engine's own, from the
// last movement step; none without a step sample.
export const sampleTurnRate = (step: StepTurn | null): number | undefined =>
  step && Number.isFinite(step.turnRateDeg) ? step.turnRateDeg : undefined;

export interface PuppetTurn {
  turnRateDeg: number;
  headingDeg: number;
}

// What a copy's engine is fed from a sample: the sender's turn rate (0 from
// a sender that carried none) and the heading the copy turns toward.
export const puppetTurn = (sample: { rot: ReadonlyArray<number>; turnRate?: number }): PuppetTurn => ({
  turnRateDeg: typeof sample.turnRate === "number" && Number.isFinite(sample.turnRate) ? sample.turnRate : 0,
  headingDeg: sample.rot[2],
});
