// Whether this seat's copy of a body can take its change-form record now (ChangeFormService.tryApply), JS state
// alone: the native lays a record only on a dead copy whose 3D is loaded (ChangeForm.cpp's premises), so a copy
// that is alive waits for the kill, and a dead copy with no 3D here (out of range, its cell not yet loaded) waits
// for its 3D instead of spending a try the native would refuse.

export interface CorpseCopyFacts {
    // Actor.isDead on this seat
    dead: boolean;
    // ObjectReference.is3DLoaded on this seat
    loaded3D: boolean;
}

// "kill": the copy is alive here; "3d": dead, its 3D not loaded; undefined: the record can be tried now
export type CorpseCopyWait = "kill" | "3d" | undefined;

export const corpseCopyWait = (f: CorpseCopyFacts): CorpseCopyWait => {
    return f.dead ? undefined : "kill";
};
