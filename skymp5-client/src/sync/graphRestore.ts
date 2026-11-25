// What a served graph state does on a seat that is not its object's owner (ScriptStateService's apply of a script
// record; thuum docs/verbs/traps.md "The graph state"): the engine's own 3D-load restore of the owner's graph, or
// nothing, beside the relays this seat took of the same object (SpSnippetService.takeSeq notes each relay's seq at
// its arrival). A relay past the state's seq is newer than the state and stands. The relay of the state's own seq
// is the same act: once it has played on this 3D a restore would start the act again, so it stands too; while it
// waits for the copy's graph the restore stands for it, and the held play goes, as it does for every relay at or
// below the state's seq.

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
  if (f.last === f.seq && f.shownHere) {
    return { why: "played", restore: false, dropHeld: false };
  }
  return { why: "restore", restore: true, dropHeld: true };
}
