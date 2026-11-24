// A press or a script event the server relays to the owner of what it
// touched, as the presser's act. JS state alone.

const hex = (n: number) => n.toString(16);

// a relayed press this game cannot play: its target, its presser's figure or
// both are not here
export const relayDropLine = (target: number, caster: number, hasTarget: boolean, hasCaster: boolean): string => {
  const missing = !hasTarget && !hasCaster ? "target or presser" : !hasTarget ? "target" : "presser";
  return `relayed activation ${hex(target)} by ${hex(caster)} not played: no ${missing} here`;
};
