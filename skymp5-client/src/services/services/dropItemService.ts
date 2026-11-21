import { Actor, ContainerChangedEvent, MenuOpenEvent } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";

import { MsgType } from "../../messages";
import { SweetTaffySweetCantDropService } from "./sweetTaffySweetCantDropService";
import { WorldCleanerService } from "./worldCleanerService";
import { logError, logLab, logTrace } from "../../logging";
import { isSessionId, toWireEntry } from "../../sync/createdForms";
import { Entry, Inventory, getDiff, getInventory } from "../../sync/inventory";
import { DropItemMessage } from "../messages/dropItemMessage";

export class DropItemService extends ClientListener {
    // thuum tracker #98 (SCHEMA_VERSION 63, Eli's look 2: "cannot drop" a brew
    // or an enchanted item; note 3920's agreed shape, "drops ... carry the
    // item's extra data"): the player's inventory as the InventoryMenu opened
    // and after each drop since, so a drop names the stack that left with its
    // extras and created forms (craftService's read: the inventory at two
    // moments, the difference). No platform native reads a world reference's
    // own extra list
    private before: Inventory | null = null;

    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        controller.on('containerChanged', (e) => this.onContainerChanged(e));
        controller.on('menuOpen', (e) => this.onMenuOpen(e));
    }

    private onMenuOpen(e: MenuOpenEvent) {
        if (e.name !== "InventoryMenu") {
            return;
        }
        const player = this.sp.Game.getPlayer();
        this.before = player ? getInventory(player) : null;
    }

    // the stacks of baseId that left the player's inventory since `before`,
    // each with its extras (the worn flags dropped: a drop is not worn), in
    // the wire's ids (toWireEntry: a brewed potion by its template and
    // definition, a made enchantment by its definition); refreshes `before`
    private droppedStacks(baseId: number, count: number): Entry[] {
        const player = this.sp.Game.getPlayer();
        if (!player) {
            return [];
        }
        const after = getInventory(player);
        const before = this.before;
        this.before = after;
        if (!before) {
            return [];
        }
        const out: Entry[] = [];
        let left = count;
        getDiff(after, before, true).entries
            .filter((d) => d.baseId === baseId && d.count < 0)
            .forEach((d) => {
                if (left <= 0) {
                    return;
                }
                const row: Entry = JSON.parse(JSON.stringify(d));
                row.count = Math.min(-d.count, left);
                left -= row.count;
                delete row.worn;
                delete row.wornLeft;
                const wire = toWireEntry(row);
                if (wire) {
                    out.push(wire);
                } else {
                    logLab("verb:craft", `a dropped stack of ${baseId.toString(16)} has no definition this engine can read; not sent`);
                }
            });
        return out;
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

            // thuum tracker #98 (SCHEMA_VERSION 63): the stacks that left, each
            // with its extras and created forms; with no read of them (no
            // InventoryMenu open seen), a game-file base goes plain as before,
            // and a session's own form cannot go at all (its next apply gives
            // it back)
            const t = MsgType.DropItem;
            const count = e.numItems;
            const stacks = this.droppedStacks(baseId, count);
            if (stacks.length === 0) {
                if (isSessionId(baseId)) {
                    return logLab("verb:craft", `a drop of session form ${baseId.toString(16)} with no read of its stack is not sent; the record keeps it`);
                }
                stacks.push({ baseId, count });
            }
            stacks.forEach((stack) => {
                const message: DropItemMessage = { ...stack, t, baseId: stack.baseId, count: stack.count };
                if (stack.createdBase || stack.createdEnchantment || stack.health || stack.enchantmentId) {
                    logLab("verb:craft", `a drop of ${stack.count} ${stack.baseId.toString(16)} sent with its extras${stack.createdBase ? " (a brewed form by its definition)" : ""}`);
                }
                this.controller.emitter.emit("sendMessage", {
                    message,
                    reliability: "reliable"
                });
            });
        }
    }
}
