// Whether the player's own body stays tracked while another seat's word owns
// it (a grip's or a throw's row). The rescan keeps a body it finds near the
// player, one this seat owns or streams, or one whose stream it follows live;
// a grip holding the body still lets its stream go quiet, and the player's
// own body is never among the references the rescan finds, so it was
// untracked mid-grip: its follow ended and its knocked-in-place hold was let
// go, and this seat's engine stood the player up while the other seat's grip
// still held the body. JS state alone.

export interface OwnBodyFacts {
    // the prop is this seat's player's own body
    ownPlayer: boolean;
    // the seat the newest owner word names (0 the cell's host)
    wordPlayer: number;
    myId: number;
}

export const ownBodyKeptForTheirWord = (f: OwnBodyFacts): boolean => {
    return false;
};
