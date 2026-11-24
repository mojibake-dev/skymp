// A copy the game file ties to a furniture (its linked reference: Arvel's web
// marker, a ceiling spider's ambush furniture) is its engine's to seat at the
// load. A copy frozen before that seat completes never sits: with its AI off
// the engine does not seat it, and the promote's package evaluation does not
// either (a spider off its ceiling falls, a webbed captive stands beside his
// web). So a copy standing near its linked furniture but not seated in it is
// not frozen at its first 3D until its engine seats it or SEAT_WAIT_MS pass;
// a seated copy frozen keeps its pose.

// how long a copy's engine has to seat it after its first 3D here
export const SEAT_WAIT_MS = 2000;
// the seated pose: the engine snaps a furniture's user onto the furniture's
// own place, within a unit of it; a copy this far off or nearer is seated
export const SEATED_UNITS = 4;
// near enough that the engine placed the copy at its furniture to seat it
// there (a resident linked to a bed across town is not waited for)
export const NEAR_FURNITURE_UNITS = 128;

// Seated: sit state 3 (sitting), or held in its linked furniture within
// SEATED_UNITS of it (a ceiling spider hangs in its furniture at sit state 0)
export const seated = (r: { sitState: number; furnitureIsLinked: boolean; unitsFromFurniture: number }): boolean =>
    r.sitState === 3 || (r.furnitureIsLinked && r.unitsFromFurniture <= SEATED_UNITS);

// Whether the demote waits for the copy's seat: tied to a furniture, near it
// and not seated in it, and its first 3D here less than SEAT_WAIT_MS ago
export const seatVerdict = (r: { linkedFurniture: boolean; seated: boolean; unitsFromFurniture: number; sinceMs: number }): "wait" | "freeze" =>
    r.linkedFurniture && !r.seated && r.unitsFromFurniture <= NEAR_FURNITURE_UNITS && r.sinceMs < SEAT_WAIT_MS ? "wait" : "freeze";
