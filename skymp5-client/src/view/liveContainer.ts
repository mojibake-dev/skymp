import { Game, ObjectReference } from 'skyrimPlatform';
import * as sp from 'skyrimPlatform';
import { Entry, Inventory, getInventory } from '../sync/inventory';
import { logLab, logTrace } from '../logging';

const TAG = 'verb:containers';

// thuum docs/verbs/containers.md, ADR-061 (playtest 41, 2026-11-02): the one
// apply of a container's record while this seat's ContainerMenu is open on it,
// for a chest (a CONT without a view, remoteServer's UpdateProperty) and a body
// (an actor's view, formView). The record is laid by base counts: what it holds
// of each base against what the engine holds, the shortfall added with the
// record's extras, the surplus removed; so this seat's own transfer, echoed back
// by the server, changes nothing and draws nothing. Under the server's
// containerSharing "exclusive" (the default since ADR-061: one occupant per
// container, a chest's or a corpse's) the echo is all an open menu ever sees.
// A change by another occupant (the "shared" setting, the backlog's live view)
// is applied and the engine's inventory-update redraw is asked once per burst;
// Eli's rule from the same playtest, verbatim: "we already said we dont want
// the whole menu to open and close out from under a guy": no menu is ever
// closed and reopened under a player to sync it (the reopen of b0939b7e
// ping-ponged between two occupants and is gone).
export type LiveContainerResult =
  'unchanged' | 'redrawn' | 'redraw-unavailable';

// one redraw per burst of changes, after its last (playtest 40's F8: a redraw
// per change left the selection on the player's pane)
const REDRAW_BATCH_MS = 300;

const redrawTimers = new Map<number, ReturnType<typeof setTimeout>>();

// the record onto the engine's container by base counts (the engine's entries
// summed per base, the record's summed per base, the shortfall added through
// TESModPlatform.addItemEx with the record entry's extras, the first entry of
// that base, the surplus removed to nowhere)
export const applyByCounts = (
  refr: ObjectReference,
  inv: Inventory,
): { added: number; removed: number } => {
  const want = new Map<number, number>();
  const sample = new Map<number, Entry>();
  for (const e of inv.entries) {
    want.set(e.baseId, (want.get(e.baseId) ?? 0) + e.count);
    if (!sample.has(e.baseId)) {
      sample.set(e.baseId, e);
    }
  }
  const have = new Map<number, number>();
  for (const e of getInventory(refr).entries) {
    have.set(e.baseId, (have.get(e.baseId) ?? 0) + e.count);
  }
  let added = 0;
  let removed = 0;
  const bases = new Set<number>();
  want.forEach((_, baseId) => bases.add(baseId));
  have.forEach((_, baseId) => bases.add(baseId));
  bases.forEach((baseId) => {
    const delta = (want.get(baseId) ?? 0) - (have.get(baseId) ?? 0);
    const form = Game.getFormEx(baseId);
    if (!form || delta === 0) {
      return;
    }
    if (delta > 0) {
      const e = sample.get(baseId);
      sp.TESModPlatform.addItemEx(
        refr,
        form,
        delta,
        e && e.health ? e.health : 1,
        e && e.enchantmentId
          ? sp.Enchantment.from(Game.getFormEx(e.enchantmentId))
          : null,
        e && e.maxCharge ? e.maxCharge : 0,
        !!(e && e.removeEnchantmentOnUnequip),
        e && e.chargePercent ? e.chargePercent : 0,
        e && e.name ? e.name : '',
        e && e.soul ? e.soul : 0,
        e && e.poisonId ? sp.Potion.from(Game.getFormEx(e.poisonId)) : null,
        e && e.poisonCount ? e.poisonCount : 0,
      );
      added += delta;
    } else {
      refr.removeItem(form, -delta, true, null);
      removed += -delta;
    }
  });
  return { added, removed };
};

// `refr` is the engine's reference the server's `remoteId` names, with this
// seat's ContainerMenu open on it (the caller checked the menu and the
// crosshair); `body` names a corpse for the lines
export const applyLiveContainer = (
  refr: ObjectReference,
  remoteId: number,
  inv: Inventory,
  body: boolean,
): LiveContainerResult => {
  const applied = applyByCounts(refr, inv);
  const what = `live ${body ? 'body' : 'chest'} ${remoteId.toString(16)}`;
  if (applied.added === 0 && applied.removed === 0) {
    logTrace(
      'LiveContainer',
      `${what}: the record matches the engine under the open ContainerMenu (this seat's own change, echoed); nothing to draw`,
    );
    return 'unchanged';
  }
  // another occupant's change (containerSharing "shared" alone): the engine's
  // own inventory-update redraw, once per burst; never a close and a reopen
  const api = sp as unknown as Record<string, unknown>;
  const refresh = api['refreshContainerMenu'];
  const asked = typeof refresh === 'function';
  const pending = redrawTimers.get(remoteId);
  if (pending !== undefined) {
    clearTimeout(pending);
  }
  redrawTimers.set(
    remoteId,
    setTimeout(() => {
      redrawTimers.delete(remoteId);
      if (!sp.Ui.isMenuOpen('ContainerMenu')) {
        return;
      }
      const askedLater = asked
        ? !!(refresh as (id: number) => boolean)(remoteId)
        : false;
      logTrace(
        'LiveContainer',
        `${what}: the batch's redraw ${askedLater ? 'asked' : 'unavailable on this dist'}`,
      );
    }, REDRAW_BATCH_MS),
  );
  logLab(
    TAG,
    `${what}: the record applied by base counts under the open ContainerMenu (${applied.added} added, ${applied.removed} removed), the engine's redraw ${asked ? 'queued for the batch' : 'unavailable on this dist'}`,
  );
  return asked ? 'redrawn' : 'redraw-unavailable';
};
