import { Actor, ContainerChangedEvent, printConsole } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { getPcInventory } from "./remoteServer";
import { getInventory, getDiff, hasExtras, removeSimpleItemsAsManyAsPossible, sumInventories } from "../../sync/inventory";
import { LastInvService } from "./lastInvService";
import { toWireInventory } from "../../sync/createdForms";

import { PutItemMessage } from "../messages/putItemMessage";
import { TakeItemMessage } from "../messages/takeItemMessage";
import { SweetTaffySweetCantDropService } from "./sweetTaffySweetCantDropService";
import { localIdToRemoteId, snippetIdToLocalId } from "../../view/worldViewMisc";
import { grantPickup, holdBook, pickupTakeEntry, PICKUP_HOLD_MS, takeHeldReference } from "../../sync/bookHold";
import { logLab } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { SpSnippetMessage } from "../messages/spSnippetMessage";

export class ContainersService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        controller.on('containerChanged', (e) => this.onContainerChanged(e));
        controller.emitter.on("spSnippetMessage", (e) => this.onGrantSnippet(e));
    }

    // the server's grant of a pickup to this game: its
    // ObjectReference.Activate snippet on the item with this player and true,
    // after which this game's engine runs the pickup. The asked pickup
    // (activationService) becomes a hold of that one reference. A message's
    // context: JS state alone, no game call
    private onGrantSnippet(event: ConnectionMessage<SpSnippetMessage>): void {
        const msg = event.message;
        if (msg.class.toLowerCase() !== "objectreference" || msg.function.toLowerCase() !== "activate") {
            return;
        }
        const args = Array.isArray(msg.arguments) ? msg.arguments : [];
        const taker = args[0];
        const takerId = taker !== null && typeof taker === "object" && typeof taker.formId === "number" ? snippetIdToLocalId(taker.formId) : 0;
        if (takerId !== 0x14 || args[1] !== true) {
            return;
        }
        const localId = snippetIdToLocalId(msg.selfId);
        if (localId && grantPickup(localId, Date.now())) {
            logLab("verb:pickup", `reference ${localId.toString(16)} granted to this game: its engine's pickup runs`);
        }
    }

    private onContainerChanged(e: ContainerChangedEvent) {
        const sweetCantDropService = this.controller.lookupListener(SweetTaffySweetCantDropService);

        // a pickup: this game's engine took a world item into its player's
        // inventory (the server's grant ran its activation's default
        // processing, or the Book Menu's Take ran the pickup), so the event
        // names no old container and comes with a reference. That reference
        // is not the one taken (it can name another form, or one already
        // gone, its id alone left as referenceId), so it only tells an add
        // from a reference from an inventory add of the server's. The
        // reference taken is this game's own hold of what it is taking (a
        // granted pickup, a book's open), the oldest of the base the engine
        // added, named once. It is reported as one take from that reference:
        // the stack the engine added, its owner, the stolen mark, with it,
        // and no other difference; the base is held against the inventory
        // apply until the server's echo
        const fromReference = !!e.reference || ((e as unknown as { referenceId?: number }).referenceId ?? 0) !== 0;
        const takenId = !e.oldContainer && !!e.newContainer && e.newContainer.getFormID() === 0x14 && fromReference && e.baseObj
            ? (takeHeldReference(e.baseObj.getFormID(), Date.now()) ?? 0)
            : 0;
        const pickedUp = takenId !== 0;
        if (pickedUp) {
            holdBook(takenId, e.baseObj.getFormID(), Date.now() + PICKUP_HOLD_MS, "its pickup until the server's echo");
        }
        if ((e.oldContainer && e.newContainer) || pickedUp) {
            if (
                pickedUp ||
                e.oldContainer.getFormID() === 0x14 ||
                e.newContainer.getFormID() === 0x14
            ) {
                const lastInvService = this.controller.lookupListener(LastInvService);

                if (!lastInvService.lastInv) {
                    lastInvService.lastInv = getPcInventory();
                }
                if (lastInvService.lastInv) {
                    // compared in the wire's shape, the record's own (lastInv
                    // is the server's inventory), so a created form moves by
                    // its definition (sync/createdForms.ts)
                    const newInv = toWireInventory(getInventory(this.sp.Game.getPlayer() as Actor));

                    // It seems that 'ignoreWorn = false' fixes this:
                    // https://github.com/skyrim-multiplayer/issue-tracker/issues/43
                    // For some reason excess diff is produced when 'ignoreWorn = true'
                    // I thought that it would be vice versa but that's how it works
                    const ignoreWorn = false;
                    const diff = getDiff(lastInvService.lastInv, newInv, ignoreWorn);

                    printConsole('diff:');
                    for (let i = 0; i < diff.entries.length; ++i) {
                        printConsole(`[${i}] ${JSON.stringify(diff.entries[i])}`);
                    }
                    const reported = pickedUp ? pickupTakeEntry(diff.entries, e.baseObj.getFormID(), e.numItems) : undefined;
                    const entries = pickedUp ? (reported ? [reported] : []) : diff.entries;
                    const msgs = entries
                        .filter((entry) =>
                            // TODO: review this condition, seems to be incorrect
                            entry.count > 0
                                ? sweetCantDropService.canDropOrPutItem(entry.baseId)
                                : true,
                        )
                        .filter((entry) => entry.count !== 0)
                        .map((entry) => {
                            const entryCopy = JSON.parse(JSON.stringify(entry)) as typeof entry;
                            const msg: PutItemMessage | TakeItemMessage = {
                                ...entryCopy,
                                t: entry.count > 0 ? MsgType.PutItem : MsgType.TakeItem,
                                target: pickedUp
                                    ? localIdToRemoteId(takenId)
                                    : e.oldContainer.getFormID() === 0x14
                                        ? localIdToRemoteId(e.newContainer.getFormID())
                                        : localIdToRemoteId(e.oldContainer.getFormID())
                            };
                            if (pickedUp) {
                                logLab("verb:pickup", `reference ${takenId.toString(16)} taken by this game's engine: ${Math.abs(entry.count)} of ${entry.baseId.toString(16)}${entry.ownerId ? `, owner ${entry.ownerId.toString(16)}` : ""}, reported as a take`);
                            }
                            msg.count = Math.abs(msg.count);
                            if (this.sp.Game.getFormEx(entry.baseId)?.getName() === msg.name) {
                                delete msg.name;
                            }
                            return msg;
                        });

                    msgs.forEach((msg) => this.controller.emitter.emit("sendMessage", {
                        message: msg,
                        reliability: "reliable"
                    }));

                    // a pickup's one take advances the baseline by its stack,
                    // extras and all, so it is never sent again; every other
                    // difference is left as it stands
                    if (pickedUp) {
                        if (reported) {
                            const add = { entries: [{ ...reported, count: -reported.count }] };
                            lastInvService.lastInv = sumInventories(lastInvService.lastInv, add);
                        }
                        return;
                    }

                    // Prevent emitting 1,2,3,4,5 changes when taking/putting 5 potions one by one
                    // This code makes it 1,1,1,1,1 but works only for extra-less items
                    // At the moment of writing this I think it's not needed for items with extras
                    diff.entries.forEach((entry) => {
                        if (lastInvService.lastInv && !hasExtras(entry)) {
                            const put = entry.count > 0;
                            const take = entry.count < 0;
                            if (put) {
                                lastInvService.lastInv = removeSimpleItemsAsManyAsPossible(
                                    lastInvService.lastInv,
                                    entry.baseId,
                                    entry.count,
                                );
                            } else if (take) {
                                const add = { entries: [entry] };
                                add.entries[0].count *= -1;
                                lastInvService.lastInv = sumInventories(lastInvService.lastInv, add);
                            }
                        }
                    });
                }
            }
        }
    }
}
