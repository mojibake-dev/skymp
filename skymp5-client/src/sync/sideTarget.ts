// Whether an NPC another game runs fights this seat's side, from its host's
// combat target (a server id; 0 for none): this seat's own player (`me`, its
// server id; 0 before this seat knows it) or one of this seat's own minions
// (`own`: the server ids of the summons its player cast and the thralls its
// player commands). A player and its summons and thralls are one side, so an
// NPC that attacks either is in a fight with this seat's player.
export const fightsThisSide = (target: number, me: number, own: ReadonlySet<number>): boolean =>
  target !== 0 && me !== 0 && (target === me || own.has(target));
