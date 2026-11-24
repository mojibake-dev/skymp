// A press or a script event the server relays to the owner of what it
// touched, as the presser's act. JS state alone.

const hex = (n: number) => n.toString(16);

// a relayed press this game cannot play: its target, its presser's figure or
// both are not here
export const relayDropLine = (target: number, caster: number, hasTarget: boolean, hasCaster: boolean): string => {
  const missing = !hasTarget && !hasCaster ? "target or presser" : !hasTarget ? "target" : "presser";
  return `relayed activation ${hex(target)} by ${hex(caster)} not played: no ${missing} here`;
};

// the script events by the wire's code
const SCRIPT_EVENTS: { readonly [code: number]: string } = {
  1: "OnActivate",
  2: "OnHit",
  3: "OnItemAdded",
  4: "OnItemRemoved",
  5: "OnMagicEffectApply",
};

export interface ScriptEvent {
  code: number;
  name: string;
  source: number;     // OnHit's source, an item event's base item, an effect's apply's effect; 0 for none
  projectile: number; // OnHit's projectile, 0 for none
  count: number;      // an item event's count, 0 for none
  flags: number;      // OnHit's: 1 power, 2 sneak, 4 bash, 8 blocked
}

type EventFields = { event?: number | null; source?: number | null; projectile?: number | null; count?: number | null; flags?: number | null };

const whole = (n: number | null | undefined): number => (typeof n === "number" && Number.isInteger(n) && n > 0 ? n : 0);

// a relay's script event; undefined for a relay that carries none (an
// activation or an entry), null for a code that names no event
export const scriptEventOf = (msg: EventFields): ScriptEvent | null | undefined => {
  if (msg.event === undefined || msg.event === null) {
    return undefined;
  }
  const name = Number.isInteger(msg.event) ? SCRIPT_EVENTS[msg.event] : undefined;
  if (name === undefined) {
    return null;
  }
  return {
    code: msg.event,
    name,
    source: whole(msg.source),
    projectile: whole(msg.projectile),
    count: whole(msg.count),
    flags: whole(msg.flags),
  };
};

// the native's answer ({formId, event, sent, scripts, armed, reason}): whether
// a script took the event, how many, whether the presser's acting scope was
// armed for it, and why not when it was not sent
export interface EventAnswer {
  sent: boolean;
  scripts: number;
  armed: boolean;
  reason: string;
}

export const eventAnswerOf = (raw: unknown): EventAnswer => {
  if (typeof raw !== "string") {
    return { sent: false, scripts: 0, armed: false, reason: `answered no string (${typeof raw})` };
  }
  try {
    const o = JSON.parse(raw) as Record<string, unknown> | null;
    if (o && typeof o === "object" && typeof o["sent"] === "boolean") {
      const scripts = o["scripts"];
      const reason = o["reason"];
      return {
        sent: o["sent"],
        scripts: typeof scripts === "number" ? scripts : 0,
        armed: o["armed"] === true,
        reason: typeof reason === "string" ? reason : "",
      };
    }
  } catch (e) {
    // the raw answer below
  }
  return { sent: false, scripts: 0, armed: false, reason: `answered ${raw.slice(0, 40)}` };
};

// a sent event's line: the object, the presser, what the event carries and
// the native's answer
export const scriptEventLine = (target: number, caster: number, ev: ScriptEvent, answer: EventAnswer): string => {
  const parts: string[] = [];
  if (ev.source !== 0) {
    parts.push(`source ${hex(ev.source)}`);
  }
  if (ev.projectile !== 0) {
    parts.push(`projectile ${hex(ev.projectile)}`);
  }
  if (ev.count !== 0) {
    parts.push(`count ${ev.count}`);
  }
  if (ev.flags !== 0) {
    parts.push(`flags ${ev.flags}`);
  }
  const carried = parts.length > 0 ? ` (${parts.join(", ")})` : "";
  const said = answer.sent
    ? `sent to ${answer.scripts} script(s)${answer.armed ? ", as the presser's act" : ""}`
    : `not sent (${answer.reason !== "" ? answer.reason : "refused"})`;
  return `relayed ${ev.name} on ${hex(target)} by ${hex(caster)}${carried}: ${said}`;
};
