// A hosted actor's detection of each player, as JS state alone: what its host sends in HostedActorState and what a
// guest takes from it for its own player (the guest's answer for a puppet's detection, PuppetDetection.h). The host's
// engine computes the levels (Actor::RequestDetectionLevel, another player's figure counted as the player); a puppet's
// AI is off on the guest's game, so the guest's own engine has none.

// the engine's level for no detection (RequestDetectionLevel's answer for a detector with no state)
export const UNDETECTED = -1000;
// a level moved by less than this since the last one sent, on the same side of seen, sends nothing new
export const LEVEL_STEP = 10;

export interface PlayerDetection {
  // the player's actor as the server knows it
  player: number;
  level: number;
}

// the host's entries for one actor, ascending by player, or undefined when they say nothing new against the last
// ones sent: a player's entry came or went, a level crossed into or out of sight (above 0), or moved a step
export function detectsToSend(levels: PlayerDetection[], lastSent: PlayerDetection[] | undefined): PlayerDetection[] | undefined {
  void levels;
  void lastSent;
  return undefined;
}

// the guest's level for its own player from a relayed state's entries: its entry's level, UNDETECTED when the state
// carries entries and none is this player's, undefined when it carries none (the last relayed level stands)
export function levelFor(detects: PlayerDetection[] | undefined, player: number): number | undefined {
  void detects;
  void player;
  return undefined;
}
