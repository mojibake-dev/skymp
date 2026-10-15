import * as sp from "skyrimPlatform";
import { EventEmitterType } from "../events/events";

export interface ClientListenerEvents {
    on: typeof sp.on,
    once: typeof sp.once
};

// these are only available in the git version, so we augment the module
// TODO: use typings from the current codebase, not npm module
declare module "skyrimPlatform" {
    function setTextsVisibility(visibility: 'inheritBrowser' | 'off' | 'on'): void;
    function getTextsVisibility(): 'inheritBrowser' | 'off' | 'on';
    function setTextRefr(textId: number, refrFormId: number): void; // pass 0 to detach
    function setTextRefrNode(textId: number, nodeName: string): void;
    function setTextRefrOffset(textId: number, offset: number[]): void;
    function setTextRefrScreenOffset(textId: number, offset: number[]): void;
    function getTextRefr(textId: number): number;
    function getTextRefrNode(textId: number): string;
    function getTextRefrOffset(textId: number): number[];
    function getTextRefrScreenOffset(textId: number): number[];

    // thuum docs/verbs/poison.md: the fork's Skyrim Platform sends it when the
    // player answers Yes in the game's poison box, which sends no equip event
    interface PoisonAppliedEvent {
        weapon: Weapon | null;
        poison: Potion | null;
        count: number;
    }
    function on(eventName: 'poisonApplied', callback: (event: PoisonAppliedEvent) => void): EventHandle;
    function once(eventName: 'poisonApplied', callback: (event: PoisonAppliedEvent) => void): EventHandle;

    // thuum docs/verbs/cast-action.md: the fork's Skyrim Platform reports
    // every BGSAction the player's actor performs through the engine's action
    // entry, with the idle the engine chose, the graph event it sent, the
    // input type and the actor's behavior graph variables as the graph was
    // asked. No game call is allowed in its handler (the event's context).
    interface ActionPerformedEvent {
        actor: Actor;
        action: number;
        idle: number;
        eventName: string;
        type: number;
        flags: number;
        result: number;
        variables: { booleans: ArrayBuffer; floats: ArrayBuffer; integers: ArrayBuffer };
    }
    function on(eventName: 'actionPerformed', callback: (event: ActionPerformedEvent) => void): EventHandle;
    function once(eventName: 'actionPerformed', callback: (event: ActionPerformedEvent) => void): EventHandle;
}

export type Sp = Omit<typeof sp, "on" | "once">;

export abstract class ClientListener {
    // Don't let TypeScript treat this class as empty
    private _nonEmptyClassMark = '';
}

export type ClientListenerConstructor<T> = {
    new(...args: any[]): T;
    readonly name: string;
}

export type ListenerLookupController = {
    lookupListener<T extends ClientListener>(constructor: ClientListenerConstructor<T>): T;
};

export type EventsController = {
    readonly on: typeof sp.on,
    readonly once: typeof sp.once,
    readonly emitter: EventEmitterType
};

export type CombinedController = EventsController & ListenerLookupController;
