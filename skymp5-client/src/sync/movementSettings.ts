import * as sp from "skyrimPlatform";

// skymp5-client.movement.nearUnits (default 2048) and farMovementMs (default
// 500), the one near radius and far rate of both movement ends: a view
// farther than nearUnits from this player applies a new movement no more
// often than farMovementMs, and a hosted actor farther than nearUnits from
// every other player, out of combat, is sampled no more often than that
const movementSetting = (key: string, fallback: number, least: number): number => {
  const s = sp.settings["skymp5-client"] as Record<string, unknown> | undefined;
  const movement = s && typeof s["movement"] === "object" && s["movement"] ? s["movement"] as Record<string, unknown> : {};
  const v = movement[key];
  return typeof v === "number" && Number.isFinite(v) && v >= least ? v : fallback;
};

let nearUnitsCache: number | undefined;
let farMovementCache: number | undefined;

export const nearUnits = (): number => {
  if (nearUnitsCache === undefined) {
    nearUnitsCache = movementSetting("nearUnits", 2048, 0);
  }
  return nearUnitsCache;
};

export const farMovementMs = (): number => {
  if (farMovementCache === undefined) {
    farMovementCache = movementSetting("farMovementMs", 500, 0);
  }
  return farMovementCache;
};
