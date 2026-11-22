// The highest root a followed body's stream reached since its grab let go,
// the height its fall counts from (PropFollow keeps one a follow; the
// follow's end hands it to the platform's release, which gives it to the
// copy's character controller before the copy stands). JS state alone.
export class FlightApex {
    private z: number | undefined = undefined;

    // a sample of the stream in order: its root's height and whether the
    // owner's engine held the body then. A held body is carried, not
    // falling, so a hold starts the apex over and the first sample after the
    // grip lets go (where it was let go) starts the flight
    note(rootZ: number, held: boolean): void {
        if (held) {
            this.z = undefined;
            return;
        }
        if (!Number.isFinite(rootZ)) {
            return;
        }
        this.z = this.z === undefined ? rootZ : Math.max(this.z, rootZ);
    }

    // the apex in game units, or undefined while nothing has flown
    apex(): number | undefined {
        return this.z;
    }
}
