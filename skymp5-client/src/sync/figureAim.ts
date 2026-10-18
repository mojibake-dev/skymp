// thuum docs/verbs/spell-cast.md (x-stream-aim 20261017-135526: a figure's
// Scattershock missiles left 2.75 degrees off the caster's own). The engine
// reads an actor's aim heading at every relaunch of an aimed projectile, and
// a figure's was its body's. A figure's caster's aim heading (radians), set
// from its relayed cast (remoteServer.ts) and cleared at the cast's end; the
// view writes it onto the figure each update (formView.ts, setActorAimHeading).
export const figureAimHeading = new Map<number, number>();
