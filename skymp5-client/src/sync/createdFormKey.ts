// A created form's definition and its identity, from JS state alone: the
// part of createdForms.ts that needs no engine, so a test can reach it.

export interface CreatedEffect {
  mgef: number;
  magnitude: number;
  area: number;
  duration: number;
  cost: number;
}

export interface CreatedForm {
  kind: number;
  effects: CreatedEffect[];
}

// A definition's identity, the engine's creators' own match: the kind and
// each effect's MGEF, magnitude (as the engine's f32 holds it, so the
// server's shorter spelling of the same float matches), area and duration,
// in order; the cost is not compared
export const createdKey = (form: CreatedForm): string =>
  `${form.kind}|${form.effects.map((e) => `${e.mgef}:${Math.fround(e.magnitude)}:${e.area}:${e.duration}`).join(";")}`;

export const sameCreated = (a: CreatedForm | undefined, b: CreatedForm | undefined): boolean =>
  a === undefined || b === undefined ? a === b : createdKey(a) === createdKey(b);
