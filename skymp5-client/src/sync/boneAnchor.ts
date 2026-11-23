// a place, x y z (movement.ts NiPoint3's shape, kept here so the module reads
// no game types)
export type Point = [number, number, number];

// The place a body of parts' sample is anchored at, the sample's pos, from
// which the wire measures each part's reach (wire-validate lib.rs
// check_prop_transform, PROP_BONE_REACH_UNITS): the body's own place (its 3D
// root's) while every part lies within reach of it; otherwise its first
// part's (an actor's NPC COM, the walk's first bone), its parts having flown
// from a root left standing, and from then on for the stream (atPart), so its
// place does not hop between the two; "past", with the part past reach of the
// anchor and its distance, when no anchor holds every part
export type BoneAnchor =
  | { pos: Point; atPart: boolean }
  | { past: number; reach: number };

const distance = (a: Point, b: Point): number =>
  Math.sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));

// the first part past reach of a place, and its distance
const firstPast = (from: Point, parts: Point[], reachMax: number): { past: number; reach: number } | undefined => {
  for (let i = 0; i < parts.length; ++i) {
    const reach = distance(parts[i], from);
    if (reach > reachMax) {
      return { past: i, reach };
    }
  }
  return undefined;
};

export const boneAnchor = (rootPos: Point, parts: Point[], reachMax: number, atPart: boolean): BoneAnchor => {
  if (parts.length === 0 || (!atPart && firstPast(rootPos, parts, reachMax) === undefined)) {
    return { pos: rootPos, atPart: false };
  }
  const first = parts[0];
  const off = firstPast(first, parts, reachMax);
  if (off !== undefined) {
    return off;
  }
  return { pos: [first[0], first[1], first[2]], atPart: true };
};

// The parts of a body past reach of its first part (the anchor of last
// resort), each with its distance from the first part and from the body's
// root, for the line that says which bone a torn body holds apart
export type PartPast = { index: number; fromFirst: number; fromRoot: number };

export const partsPast = (rootPos: Point, parts: Point[], reachMax: number): PartPast[] => {
  const out: PartPast[] = [];
  if (parts.length === 0) {
    return out;
  }
  for (let i = 0; i < parts.length; ++i) {
    const fromFirst = distance(parts[i], parts[0]);
    if (fromFirst > reachMax) {
      out.push({ index: i, fromFirst, fromRoot: distance(parts[i], rootPos) });
    }
  }
  return out;
};
