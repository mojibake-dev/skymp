// Who simulates a prop as this seat reads the server's owner table, by the
// server's word alone: the player the last word named; with no word, or a
// word naming nobody (player 0, the host by default), the cell's host, this
// seat when it hosts the prop's cell and 0 (another seat's, or nobody's: an
// unhosted cell's prop freezes) otherwise. No lease, no silence and no
// presence test: each of those would demote an owner the server still names.
//
// A player's knocked body (the live-body kind) is its own player's game's on
// every seat, claimed at its track, except while another player's grip holds
// it: the word naming the gripper makes it the gripper's for the hold, until
// the gripper's rest returns it. So the word naming nobody at its rest gives
// it back to its player, not to the cell's host: a host that took it would
// stream its own copy of the body, and the player's own game would follow
// that stream and knock its own player in place.
export interface OwnerFacts {
  body: number;             // the player whose live body the prop is, 0 for any other prop
  word: number | undefined; // the player the server's last word named (0: nobody)
  myId: number;             // this seat's player
  hostsCell: boolean;       // this seat hosts the prop's cell
}

export const simulatorOf = (f: OwnerFacts): number => {
  if (f.body !== 0) {
    return f.word !== undefined && f.word !== 0 ? f.word : f.body;
  }
  if (f.word !== undefined && f.word !== 0) {
    return f.word;
  }
  return f.hostsCell ? f.myId : 0;
};
