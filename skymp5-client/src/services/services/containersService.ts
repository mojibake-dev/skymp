import { Actor, ContainerChangedEvent, PickUpObjectEvent, printConsole } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { MsgType } from "../../messages";
import { getPcInventory } from "./remoteServer";
import { getInventory, getDiff, hasExtras, removeSimpleItemsAsManyAsPossible, sumInventories } from "../../sync/inventory";
import { LastInvService } from "./lastInvService";
import { sameCreated, toWireEntry, toWireInventory } from "../../sync/createdForms";

import { PutItemMessage } from "../messages/putItemMessage";
import { TakeItemMessage } from "../messages/takeItemMessage";
import { SweetTaffySweetCantDropService } from "./sweetTaffySweetCantDropService";
import { localIdToRemoteId, snippetIdToLocalId } from "../../view/worldViewMisc";
import { grantPickup, holdBook, PICKUP_HOLD_MS, pickupTake } from "../../sync/bookHold";
import { logLab } from "../../logging";
import { ConnectionMessage } from "../events/connectionMessage";
import { SpSnippetMessage } from "../messages/spSnippetMessage";

export class ContainersService extends ClientListener {
    constructor(private sp: Sp, private controller: CombinedController) {
        super();
        controller.on('containerChanged', (e) => this.onContainerChanged(e));
        controller.on('pickUpObject', (e) => this.onPickUpObject(e));
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

    // this game's engine picked a world reference up (the platform's
    // pickUpObject, from PlayerCharacter's PickUpObject: the server's grant
    // ran the activation's default processing, or the Book Menu's Take ran
    // it). It is reported as one take from that reference: the base and
    // count the player's inventory gained and the owner the engine marked the
    // stack with, the stack's other extras read from the player's inventory
    // against the record (lastInv). The base is held against the inventory
    // apply until the server's echo, and the record's baseline advances by
    // the take, so it is never sent again
    private onPickUpObject(e: PickUpObjectEvent): void {
        if (!e.refId || !e.baseId || !(e.count > 0)) {
            return;
        }
        holdBook(e.refId, e.baseId, Date.now() + PICKUP_HOLD_MS, "its pickup until the server's echo");
        const lastInvService = this.controller.lookupListener(LastInvService);
        if (!lastInvService.lastInv) {
            lastInvService.lastInv = getPcInventory();
        }
        const player = this.sp.Game.getPlayer();
        const diff = lastInvService.lastInv && player
            ? getDiff(lastInvService.lastInv, toWireInventory(getInventory(player as Actor)), false).entries
            : [];
        // the taken stack in the wire's shape, as the difference is (a created
        // potion by its template and definition, sync/createdForms.ts), so it
        // meets its own gain there and the take names no session id; a
        // session form with no definition cannot cross and is not reported
        const taken = toWireEntry({ baseId: e.baseId, count: e.count });
        if (!taken) {
            logLab("verb:pickup", `reference ${e.refId.toString(16)} taken by this game's engine: ${e.baseId.toString(16)} is a session form with no definition this game reads; not reported`);
            return;
        }
        const take = pickupTake(diff, { ...taken, ownerId: e.ownerId },
            (x) => x.baseId === taken.baseId && sameCreated(x.createdBase, taken.createdBase));
        const msg: TakeItemMessage = {
            ...JSON.parse(JSON.stringify(take)) as typeof take,
            t: MsgType.TakeItem,
            target: localIdToRemoteId(e.refId),
        };
        if (msg.name !== undefined && this.sp.Game.getFormEx(take.baseId)?.getName() === msg.name) {
            delete msg.name;
        }
        this.controller.emitter.emit("sendMessage", {
            message: msg,
            reliability: "reliable"
        });
        logLab("verb:pickup", `reference ${e.refId.toString(16)} taken by this game's engine: ${take.count} of ${take.baseId.toString(16)}${take.ownerId ? `, owner ${take.ownerId.toString(16)}` : ""}, reported as a take`);
        if (lastInvService.lastInv) {
            lastInvService.lastInv = sumInventories(lastInvService.lastInv, { entries: [take] });
        }
    }

    private onContainerChanged(e: ContainerChangedEvent) {
        const sweetCantDropService = this.controller.lookupListener(SweetTaffySweetCantDropService);

        if (e.oldContainer && e.newContainer) {
            if (
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
                    const msgs = diff.entries
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
                                target: e.oldContainer.getFormID() === 0x14
                                    ? localIdToRemoteId(e.newContainer.getFormID())
                                    : localIdToRemoteId(e.oldContainer.getFormID())
                            };
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
