import { Game, ObjectReference, once } from "skyrimPlatform";
import { Inventory } from "../sync/inventory";
import { isBadMenuShown } from "../sync/equipment";
import { logLab } from "../logging";
import { ModelApplyUtils } from "./modelApplyUtils";

interface PendingApply {
  inventory: Inventory;
  resetBaseFirst: boolean;
}

// thuum (playtest 37, 2026-10-28, Eli: "shouldn't be that hard to change that
// to on cell load right?"): the server sends a container's recorded inventory
// to every player whose grid loads it (MpObjectReference::VisitProperties), a
// game-file container's inside its CreateActor (remoteServer, no view) and a
// server-made copy's through its view (formView), so a cell load brings as
// many inventories as the cell has recorded containers at once. Each entry of
// an apply is a native call, so the applies queue here and one container lands
// per update tick, none under an item menu (the gate the crosshair apply
// keeps). A newer inventory for a queued container replaces the older; a
// container gone by its turn is skipped. The open chest's own apply (the
// opener's and the occupants' sends, memo D52 B) stays immediate in its
// callers.
export class ContainerApplyQueue {
  static enqueue(refrId: number, inventory: Inventory, resetBaseFirst = true): void {
    ContainerApplyQueue.pending.set(refrId, { inventory, resetBaseFirst });
    if (!ContainerApplyQueue.armed) {
      ContainerApplyQueue.armed = true;
      once("update", () => ContainerApplyQueue.drain());
    }
  }

  private static drain(): void {
    if (isBadMenuShown()) {
      once("update", () => ContainerApplyQueue.drain());
      return;
    }
    const next = ContainerApplyQueue.pending.entries().next();
    if (next.done) {
      ContainerApplyQueue.armed = false;
      return;
    }
    const [refrId, apply] = next.value;
    ContainerApplyQueue.pending.delete(refrId);
    const refr = ObjectReference.from(Game.getFormEx(refrId));
    if (refr) {
      ModelApplyUtils.applyModelInventory(refr, apply.inventory, apply.resetBaseFirst);
      logLab("verb:containers", `container ${refrId.toString(16)}: the server's inventory applied at its load (${apply.inventory.entries.length} entries, ${ContainerApplyQueue.pending.size} queued)`);
    }
    if (ContainerApplyQueue.pending.size > 0) {
      once("update", () => ContainerApplyQueue.drain());
    } else {
      ContainerApplyQueue.armed = false;
    }
  }

  private static readonly pending = new Map<number, PendingApply>();
  private static armed = false;
}
