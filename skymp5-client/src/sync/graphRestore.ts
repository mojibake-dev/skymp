// What a served graph state does on a seat that is not its object's owner (ScriptStateService's apply of a script
// record; thuum docs/verbs/traps.md "The graph state"): the engine's own 3D-load restore of the owner's graph, or
// nothing, beside the relays this seat took of the same object (SpSnippetService.takeSeq notes each relay's seq at
// its arrival). A relay past the state's seq is newer than the state and stands.

export interface GraphRestoreFacts {
  last: number | undefined; // the newest relay's seq this seat took for the object
  seq: number;              // the state's own seq
  shownHere: boolean;       // the kept last animation was played on the 3D now loaded
}

export interface GraphRestoreVerdict {
  why: "past" | "played" | "restore";
  restore: boolean;
  dropHeld: boolean; // a play of the object held for its graph goes (heldPlays.ts)
}

export function graphRestore(f: GraphRestoreFacts): GraphRestoreVerdict {
  if (f.last !== undefined && f.last > f.seq) {
    return { why: "past", restore: false, dropHeld: false };
  }
  return { why: "restore", restore: true, dropHeld: false };
}
