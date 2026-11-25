// How long a copy's fed sample stays fresh on its engine. The movement-step
// detour feeds Speed 0 and turns nothing from a sample older than its window
// (skyrim-platform PuppetHooks.cpp), the way a puppet whose feed stopped
// stands still. A copy's samples come at its host's rate for it: 130 ms
// while another player is near, 500 ms (farMovementMs) when none is, and
// this seat applies a far one no sooner than its own far rate after the last
// (frameCost.ts movementApplyDue), so a far copy's samples land a little over
// 500 ms apart. The window follows the copy's own cadence: twice the gap
// since its previous sample, so one late sample keeps it fed and a host that
// stopped sending stills it within two gaps. It is never under the engine's
// own 400 ms (Puppets.h kLocomotionStaleMs), a near copy's window as before,
// and never over twice the far rate, since a longer gap was a pause and no
// cadence.

export const PUPPET_STALE_LEAST_MS = 400;

export const puppetStaleMs = (gapMs: number | undefined, farRateMs: number): number => {
  const most = Math.max(PUPPET_STALE_LEAST_MS, 2 * farRateMs);
  if (gapMs === undefined || !Number.isFinite(gapMs) || gapMs <= 0) {
    return PUPPET_STALE_LEAST_MS;
  }
  return Math.min(most, Math.max(PUPPET_STALE_LEAST_MS, 2 * gapMs));
};
