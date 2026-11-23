// Whether a reference's movement record places it on this seat. A prop
// PropService tracks is placed by this seat's Havok or by the stream it
// follows, never by the record. An actor's live body is the same while a grip
// or a stream places it: this seat's hand on it or its stream open, or another
// seat's live stream its copy follows; the record's root placed under a grab
// pulls the body's parts apart. A figure merely tracked in place (a standing
// one PropService watches for a knock) keeps taking its record. JS state alone.

export interface BodyPlaceFacts {
    isActor: boolean;
    // PropService tracks the reference
    tracked: boolean;
    // an actor's live body: this seat's hand on it, its stream open here,
    // another seat's live stream its copy follows
    held: boolean;
    streaming: boolean;
    followedLive: boolean;
}

export const recordMovesIt = (f: BodyPlaceFacts): boolean => {
    return f.isActor ? !(f.held || f.streaming || f.followedLive) : !f.tracked;
};

// After the hold on an actor's live body ends (a grip's throw landed, a
// stream rested, the row handed back), the record the view still carries from
// before the hold is no word about where the body is. Whether the record is
// kept off the body this frame, and the record count it is kept off at:
// placedBefore and placedNow are recordMovesIt's verdicts last frame and this
// frame, heldAt the count kept from the last frame, changes the record's count
// now (the view's numMovementChanges)
export interface AfterHand {
    placedBefore: boolean;
    placedNow: boolean;
    heldAt: number | undefined;
    changes: number;
}

export const recordAfterHand = (_h: AfterHand): { holds: boolean; heldAt: number | undefined } => {
    return { holds: false, heldAt: undefined };
};
