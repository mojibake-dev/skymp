import { Game, ObjectReference, callNative, settings } from 'skyrimPlatform';
import * as sp from 'skyrimPlatform';
import { Entry, Inventory, getInventory } from '../sync/inventory';
import { logLab, logTrace } from '../logging';
import { MsgType } from '../messages';
import { SpApiInteractor } from '../services/spApiInteractor';
import { localIdToRemoteId } from './worldViewMisc';

const TAG = 'verb:containers';

// thuum docs/verbs/containers.md, ADR-061 (playtest 41, 2026-11-02, Eli: "take
// one more deep dive crack at REALLY getting live omnidirectional syncing of
// containers for all interacting parties"): the one apply of a container's
// record while this seat's ContainerMenu is open on it, for a chest (a CONT
// without a view, remoteServer's UpdateProperty) and a body (an actor's view,
// formView). The record is laid by base counts: what it holds of each base
// against what the engine holds, the shortfall added with the record's extras,
// the surplus removed; so this seat's own transfer, echoed back by the server,
// changes nothing and draws nothing, and another occupant's change moves only
// the difference. The menu is then drawn again whole: the engine draws a
// ContainerMenu at its open and the in-place redraw left its rows stale in
// three playtests (37's findings 29 and 30, 40's F8, 41's item 7: a take became
// an eat, a stored sword showed as boots, a take moved nothing), so the menu is
// closed and the container is activated again through the server's own flow
// (the closing activation leaves, the new one brings the record and the
// OpenContainer, and the engine lays the list whole). The client setting
// chestRedraw "inplace" keeps the engine's inventory-update redraw for its
// trace alone (HYPOTHESIS: ContainerMenu::ProcessMessage's kInventoryUpdate
// rebuilds the ItemList's rows from InventoryChanges the platform's addItemEx
// never announced; the Frida plan in containers.md).
export type LiveContainerResult =
  'unchanged' | 'reopened' | 'redrawn' | 'redraw-unavailable';

// the reopen's gap between the close and the activation that follows, ms (the
// engine's close runs on its own frame; the lab's close-menu step waits the
// same before its next activation)
const REOPEN_WAIT_MS = 400;
// the in-place mode's batch: one redraw per burst of changes, after its last
const REDRAW_BATCH_MS = 300;

const redrawTimers = new Map<number, ReturnType<typeof setTimeout>>();

const redrawMode = (): 'reopen' | 'inplace' => {
  const s = settings['skymp5-client'] as Record<string, unknown> | undefined;
  return s && String(s['chestRedraw']) === 'inplace' ? 'inplace' : 'reopen';
};

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
  if (redrawMode() === 'inplace') {
    // one redraw per burst, after its last change (playtest 40's F8: a redraw
    // per change left the selection on the player's pane)
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
      `${what}: the record applied by base counts under the open ContainerMenu (${applied.added} added, ${applied.removed} removed), the engine's redraw ${asked ? 'queued for the batch' : 'unavailable on this dist'} (inplace)`,
    );
    return asked ? 'redrawn' : 'redraw-unavailable';
  }
  // the reopen: the engine's kHide of the menu through the platform's
  // CloseMenu (the lab's close-menu step), then the container asked again
  // through the server; the close itself sends the closing activation (the
  // open flow watches the menu shut), so the server's occupancy stays right
  callNative('TESModPlatform', 'CloseMenu', undefined, 'ContainerMenu');
  const me = localIdToRemoteId(0x14);
  if (me) {
    setTimeout(() => {
      SpApiInteractor.getControllerInstance().emitter.emit('sendMessage', {
        message: {
          t: MsgType.Activate,
          data: { caster: me, target: remoteId, isSecondActivation: false },
        },
        reliability: 'reliable',
      });
    }, REOPEN_WAIT_MS);
  }
  logLab(
    TAG,
    `${what}: the record applied by base counts under the open ContainerMenu (${applied.added} added, ${applied.removed} removed), the menu closed and the container asked again (reopen)`,
  );
  return 'reopened';
};
