// Whether a dead body's change of contents on its owner's engine is recorded at once (ChangeFormService's
// contents-changed record): the record is the body's own save, and every other seat lays its copy by it. A body
// whose fall this seat still runs waits for its rest's record, which carries the contents with the rest: a record
// taken mid-fall holds the fall's poses and its dying life state, and a seat that lays its copy by it puts the body
// back where it died, to fall again.

export interface ContentsRecordFacts {
  dead: boolean;     // the body is dead on this engine
  loaded3D: boolean; // its 3D is loaded here
  falling: boolean;  // this seat still runs its fall: its kill here under CORPSE_REST_MIN_MS ago, its stream awake, or its dead samples still settling
}

export function recordContentsNow(f: ContentsRecordFacts): boolean {
  return f.dead && f.loaded3D && !f.falling;
}
