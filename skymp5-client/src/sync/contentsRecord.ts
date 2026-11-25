// Whether a dead body's change of contents on its owner's engine is recorded at once (ChangeFormService's
// contents-changed record): the record is the body's own save, and every other seat lays its copy by it.

export interface ContentsRecordFacts {
  dead: boolean;     // the body is dead on this engine
  loaded3D: boolean; // its 3D is loaded here
  falling: boolean;  // this seat still runs its fall: its stream awake, or its dead samples still settling
}

export function recordContentsNow(f: ContentsRecordFacts): boolean {
  return f.dead && f.loaded3D;
}
