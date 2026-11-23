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
