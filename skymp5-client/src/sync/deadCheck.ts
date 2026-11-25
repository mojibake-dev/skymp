// The view's dead check, from JS state alone: what it asks of an actor's
// engine when the engine's isDead and the model's disagree.
// - "kill": the model is dead and the engine alive, on a seat that does not
//   simulate the actor (the host's own engine kills by itself);
// - "bindKill": the same at the first frame with the actor's 3D, on any seat
//   (the body moved to the record's spot and killed silently);
// - "standUp": the model is alive and the engine dead, except on the seat
//   that simulates the actor while its own report of the death waits for the
//   server's word: the model's alive word is then the one from before the
//   death, and the body the host's engine killed stays down. `ownDeathMsAgo`
//   is the time since that report, undefined when none waits (the server's
//   death word clears it). A report the server refused gets no answer, so
//   after `wordWaitMs` the record's alive word stands the body up again;
// - "none": nothing.
export type DeadCheckAsk = "none" | "kill" | "bindKill" | "standUp";

export const deadCheckAsk = (input: {
  engineDead: boolean;
  want: boolean;
  simulatedHere: boolean;
  bind: boolean;
  ownDeathMsAgo: number | undefined;
  wordWaitMs: number;
}): DeadCheckAsk => {
  if (input.engineDead === input.want) {
    return "none";
  }
  if (input.want) {
    if (input.bind) {
      return "bindKill";
    }
    return input.simulatedHere ? "none" : "kill";
  }
  if (input.simulatedHere && input.ownDeathMsAgo !== undefined && input.ownDeathMsAgo < input.wordWaitMs) {
    return "none";
  }
  return "standUp";
};
