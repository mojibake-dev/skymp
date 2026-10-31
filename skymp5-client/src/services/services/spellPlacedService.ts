import { ObjectReference } from 'skyrimPlatform';
import { MsgType } from '../../messages';
import { logTrace } from '../../logging';
import { ConnectionMessage } from '../events/connectionMessage';
import { PlacedRequestMessage } from '../messages/placedRequestMessage';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { WorldCleanerService } from './worldCleanerService';

// thuum docs/verbs/spell-placed-object.md (ADR-057 shape A, D70): a listed
// spell's script (Apocalypse's Raise Wall) places its objects in this game
// alone, at a pose only this engine knows (206 ahead along the caster's own
// heading, the pitch read of 2026-10-30; the server's record carries another
// yaw). At the accepted cast the server asks this game for the poses
// (PlacedRequest: the set, the bases, the window); this service finds each
// base's reference and answers once it has settled (PlacedReport), and the
// server places its copies there for every other seat. The search is centred
// where the script stands its objects, the point `aheadUnits` along the
// player's yaw (Papyrus angles: x += d sin(yaw), y += d cos(yaw)), within
// `searchRadius`, over the player's cell's references of the base's form type
// (Cell.getNumRefs and getNthRef, the lab's refs-near idiom; an exterior cell
// is one 4096-unit square, so the engine's closest-reference search is the
// fallback when the cell lists none), skipping the view's own copies (the
// cleaner's protection, as formView and dropItemService tell the engine's
// references from the view's) and references that stand disabled or deleted.
// Settled: two polls `pollMs` apart within `settleUnits` (the script's wall
// rises from 240 below at 512 units a second, so the reads differ by about 50
// until it stops; the halo stands at once). At the deadline (the window less
// a reply margin) the last read goes out settled or not; a base never found
// is traced and dropped, and the server places nothing for it.
//
// Disabled is the rule that tells an earlier cast's objects from this one's:
// the script keeps the same two references across casts (x-raise-wall v5,
// 20261031-104852 and -105243: cast one's ids, disabled at its release,
// re-enabled and raised again by cast two), so a reference is never skipped
// for having been reported before (v4's rule, which lost every second cast:
// the halo reappears at the exact pose it was reported at) and a disabled one
// is never reported (the first wall, faded at its release, is not where the
// caster sees a wall). Cast two's wall began its rise about 2.3 s after the
// request on both runs, so the server's window is 6 s (placements.rs).
interface Watch {
  set: number;
  base: number;
  deadline: number;
  lastPos?: [number, number, number];
  lastYaw: number;
  found: number;
}

export class SpellPlacedService extends ClientListener {
  constructor(
    private sp: Sp,
    private controller: CombinedController,
  ) {
    super();
    this.controller.emitter.on('placedRequestMessage', (e) =>
      this.onRequest(e),
    );
    this.controller.on('update', () => this.onUpdate());
  }

  private onRequest(e: ConnectionMessage<PlacedRequestMessage>) {
    const m = e.message;
    const now = Date.now();
    const deadline =
      now +
      Math.max(
        SpellPlacedService.pollMs,
        m.windowMs - SpellPlacedService.replyMarginMs,
      );
    for (const base of m.bases) {
      this.watches.push({ set: m.set, base, deadline, lastYaw: 0, found: 0 });
    }
    logTrace(
      this,
      `spell-placed request set ${m.set}: bases ${m.bases.map((b) => b.toString(16)).join(', ')} within ${m.windowMs} ms`,
    );
  }

  private onUpdate() {
    if (this.watches.length === 0) {
      return;
    }
    const now = Date.now();
    if (now - this.lastPollAt < SpellPlacedService.pollMs) {
      return;
    }
    this.lastPollAt = now;
    const player = this.sp.Game.getPlayer();
    if (!player) {
      return;
    }
    const yaw = player.getAngleZ();
    const rad = (yaw * Math.PI) / 180;
    const cx =
      player.getPositionX() + SpellPlacedService.aheadUnits * Math.sin(rad);
    const cy =
      player.getPositionY() + SpellPlacedService.aheadUnits * Math.cos(rad);
    const cz = player.getPositionZ();
    const cleaner = this.controller.lookupListener(WorldCleanerService);
    const keep: Watch[] = [];
    for (const w of this.watches) {
      const refr = this.find(w, cx, cy, cz, cleaner);
      if (refr) {
        const pos: [number, number, number] = [
          refr.getPositionX(),
          refr.getPositionY(),
          refr.getPositionZ(),
        ];
        const settled =
          w.lastPos !== undefined &&
          SpellPlacedService.distance(w.lastPos, pos) <
            SpellPlacedService.settleUnits;
        w.lastPos = pos;
        w.lastYaw = refr.getAngleZ();
        w.found = refr.getFormID();
        if (settled || now >= w.deadline) {
          this.report(w, pos, settled);
          continue;
        }
      } else if (now >= w.deadline) {
        if (w.lastPos !== undefined) {
          // seen earlier, gone from the search now (the engine's own copy
          // moved out of the radius, or was removed): the last read goes out
          this.report(w, w.lastPos, false);
        } else {
          logTrace(
            this,
            `spell-placed set ${w.set} base ${w.base.toString(16)}: no reference of this game's own within ${SpellPlacedService.searchRadius} units of the point ${SpellPlacedService.aheadUnits} ahead by the deadline; nothing reported`,
          );
        }
        continue;
      }
      keep.push(w);
    }
    this.watches = keep;
  }

  // the engine's own reference of the watch's base nearest the expected
  // point: none of the view's copies (protected), none disabled or deleted
  private find(
    w: Watch,
    cx: number,
    cy: number,
    cz: number,
    cleaner: WorldCleanerService,
  ): ObjectReference | null {
    const base = this.sp.Game.getFormEx(w.base);
    if (!base) {
      return null;
    }
    const player = this.sp.Game.getPlayer();
    const cell = player ? player.getParentCell() : null;
    let best: ObjectReference | null = null;
    let bestDistance = SpellPlacedService.searchRadius;
    if (cell) {
      const type = base.getType();
      const n = cell.getNumRefs(type);
      for (let i = 0; i < n; ++i) {
        const refr = cell.getNthRef(i, type);
        if (!refr) {
          continue;
        }
        const refrBase = refr.getBaseObject();
        if (!refrBase || refrBase.getFormID() !== w.base) {
          continue;
        }
        if (!this.eligible(refr, cleaner)) {
          continue;
        }
        const d = SpellPlacedService.distance(
          [refr.getPositionX(), refr.getPositionY(), refr.getPositionZ()],
          [cx, cy, cz],
        );
        if (d < bestDistance) {
          best = refr;
          bestDistance = d;
        }
      }
    }
    if (best) {
      return best;
    }
    const found = this.sp.Game.findClosestReferenceOfType(
      base,
      cx,
      cy,
      cz,
      SpellPlacedService.searchRadius,
    );
    if (!found || !this.eligible(found, cleaner)) {
      return null;
    }
    return found;
  }

  // not deleted, not disabled (an earlier cast's object, faded at its
  // release), not the view's own copy
  private eligible(refr: ObjectReference, cleaner: WorldCleanerService): boolean {
    if (refr.isDeleted() || refr.isDisabled()) {
      return false;
    }
    return cleaner.getWcProtection(refr.getFormID()) === 0;
  }

  private report(w: Watch, pos: [number, number, number], settled: boolean) {
    this.controller.emitter.emit('sendMessage', {
      message: {
        t: MsgType.PlacedReport,
        set: w.set,
        base: w.base,
        pos: [pos[0], pos[1], pos[2]],
        yaw: w.lastYaw,
      },
      reliability: 'reliable',
    });
    logTrace(
      this,
      `spell-placed set ${w.set} base ${w.base.toString(16)}: reference ${w.found.toString(16)} at (${pos.map((v) => v.toFixed(0)).join(', ')}) yaw ${w.lastYaw.toFixed(0)} reported${settled ? '' : ' unsettled at the deadline'}`,
    );
  }

  private static distance(
    a: [number, number, number],
    b: [number, number, number],
  ): number {
    return Math.hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
  }

  private watches: Watch[] = [];
  private lastPollAt = 0;
  // where the script stands Raise Wall's objects: 206.25 (the wall) and
  // 196.875 (the halo) ahead of the caster (x-raise-wall-pitch v1)
  private static readonly aheadUnits = 200;
  // the search around that point: the caster's own motion inside the cast's
  // window and the two objects' spread, well inside the server's band
  private static readonly searchRadius = 160;
  private static readonly pollMs = 100;
  private static readonly settleUnits = 0.5;
  // a round trip to the server before its window closes
  private static readonly replyMarginMs = 500;
}
