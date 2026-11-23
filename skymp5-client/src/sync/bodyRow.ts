// Whether an actor's body has no prop row on this seat (PropService's
// kindOfBody), by what this seat knows of it. A dead body is a row unless its
// id is a server-made one (a player's figure or a server-made NPC: their
// bodies are no row here) or it is this seat's own player. A living actor is
// a row while it is knocked, except a server-made one: another player's
// figure (or a server-made NPC) is a row only while this seat follows a
// stream of it, its grip holds it, or this seat's own player's push threw it,
// whose throw is this seat's (the push-figure hook) so its impulse claims the
// body for the flight.
export interface BodyFacts {
  dead: boolean;
  own: boolean;        // this seat's own player
  serverMade: boolean; // a server-made id (0xff000000 and up)
  followed: boolean;   // a stream of it followed here, or this seat's grip on it
  pushedHere: boolean; // this seat's own player's push threw it
}

export const bodyHasNoRow = (f: BodyFacts): boolean =>
  f.dead ? (f.serverMade || f.own) : (!f.followed && !f.pushedHere && !f.own && f.serverMade);
