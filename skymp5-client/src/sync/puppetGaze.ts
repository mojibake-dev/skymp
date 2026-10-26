// thuum docs/verbs/npc-stream.md (slice 5, the gaze): the puppets whose
// head-track target the host's state names right now, so the movement
// apply's point heuristic (the nearest actor within 128 units of the host's
// look point) stands aside for them (PuppetFaceService sets and clears).
export const puppetGazeHeld = new Set<number>();
