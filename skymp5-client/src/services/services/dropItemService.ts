import { Actor, ContainerChangedEvent } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";

import { MsgType } from "../../messages";
import { SweetTaffySweetCantDropService } from "./sweetTaffySweetCantDropService";
import { WorldCleanerService } from "./worldCleanerService";
import { logError, logTrace } from "../../logging";

export class DropItemService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        controller.on('containerChanged', (e) => this.onContainerChanged(e));
    }

    private onContainerChanged(e: ContainerChangedEvent) {
        const sweetCantDropService = this.controller.lookupListener(SweetTaffySweetCantDropService);

        const pl = this.sp.Game.getPlayer() as Actor;
        const isPlayer: boolean =
            pl && e.oldContainer && pl.getFormID() === e.oldContainer.getFormID();
        const noContainer: boolean =
            e.newContainer === null || e.newContainer === undefined;
        const isReference: boolean = e.reference !== null;
        if (e.newContainer && e.newContainer.getFormID() === pl.getFormID())
            return;
        if (!this.sp.Ui.isMenuOpen("InventoryMenu"))
            return;
        if (
            isPlayer &&
            isReference &&
            noContainer &&
            sweetCantDropService.canDropOrPutItem(e.baseObj.getFormID())
        ) {
            const radius: number = 2000;
            const baseId = e.baseObj.getFormID();

            const player = this.sp.Game.getPlayer() as Actor;

            let set = new Set<number>();
            for (let i = 0; i < 200; i++) {
                const refrId = this.sp.Game.findRandomReferenceOfType(
                    this.sp.Game.getFormEx(baseId),
                    player.getPositionX(),
                    player.getPositionY(),
                    player.getPositionZ(),
                    radius
                )?.getFormID();
                if (refrId) {
                    set.add(refrId);
                } else {
                    break;
                }
            }

            let numFound = 0;

            const worldCleanerService = this.controller.lookupListener(WorldCleanerService);

            set.forEach((refrId) => {
                const ref = this.sp.ObjectReference.from(this.sp.Game.getFormEx(refrId));
                if (ref !== null && ref.isDeleted() === false) {
                    const refrId = ref.getFormID();

                    if (worldCleanerService.getWcProtection(refrId) === 0) {
                        // thuum ADR-071 W3 (Eli, "Fix the drop", 2026-11-11): the
                        // engine's own dropped copy is disabled, then deleted once
                        // the disable has run on a wrapper taken again (the
                        // Creation Kit's order for a delete, formView.ts's and the
                        // world cleaner's idiom). A delete alone left it listed in
                        // the dropper's cell under Skyrim Souls RE's running
                        // InventoryMenu, deleted, not disabled and never freed
                        // until its player left the cell (thuum
                        // docs/engine/havok-props.md E39, x-drop-leftover v1.1
                        // 20261111-220150), where a pausing menu freed it at once
                        // (E38). HYPOTHESIS until a read shows the reference gone
                        // at once under the running menu
                        ref.disable(false).then(() => {
                            const again = this.sp.ObjectReference.from(this.sp.Game.getFormEx(refrId));
                            if (again && !again.isDeleted()) {
                                // returned, so a refused delete reaches the catch below
                                return again.delete();
                            }
                        }).catch((err) => logError(this, `deleting the engine's dropped reference ${refrId.toString(16)} after its disable failed`, err));
                        ++numFound;
                        logTrace(this, "Found and deleted reference " + refrId.toString(16));
                    } else {
                        logTrace(this, "Found reference " + refrId.toString(16) + " but it's protected");
                    }
                }
            });

            if (!numFound) {
                return logTrace(this, "Ignoring item drop as false positive");
            }

            const t = MsgType.DropItem;
            const count = e.numItems;
            this.controller.emitter.emit("sendMessage", {
                message: {
                    t, baseId, count,
                },
                reliability: "reliable"
            });
        }
    }
}
