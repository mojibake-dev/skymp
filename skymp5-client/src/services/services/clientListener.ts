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
    // thuum ADR-034: one of SKSE's action events for the player (type 4 is
    // the voice's fire, 2 a hand's spell fire; slot 0 left, 1 right, 2
    // voice; sourceForm the spell or shout). No game call in its handler.
    interface SkseActionEvent {
        actor: Actor;
        type: number;
        slot: number;
        sourceForm: number;
    }
    function on(eventName: 'skseAction', callback: (event: SkseActionEvent) => void): EventHandle;
    function once(eventName: 'skseAction', callback: (event: SkseActionEvent) => void): EventHandle;
    // thuum docs/verbs/marksman.md: a projectile the engine launched, with
    // the origin and angles it used (every shooter). No game call in its
    // handler.
    interface ProjectileLaunchedEvent {
        shooter: number;
        projectile: number;
        weapon: number;
        ammo: number;
        spell: number;
        castingSource: number;
        originX: number;
        originY: number;
        originZ: number;
        angleX: number;
        angleZ: number;
        power: number;
        useOrigin: boolean;
    }
    function on(eventName: 'projectileLaunched', callback: (event: ProjectileLaunchedEvent) => void): EventHandle;
    function once(eventName: 'projectileLaunched', callback: (event: ProjectileLaunchedEvent) => void): EventHandle;
    // thuum docs/verbs/marksman.md: an arrow's flight as it starts, its final
    // angles (after the player's auto-aim). No game call in its handler.
    interface ProjectileFlightEvent {
        shooter: number;
        projectile: number;
        weapon: number;
        ammo: number;
        x: number;
        y: number;
        z: number;
        angleX: number;
        angleZ: number;
        power: number;
    }
    function on(eventName: 'projectileFlight', callback: (event: ProjectileFlightEvent) => void): EventHandle;
    function once(eventName: 'projectileFlight', callback: (event: ProjectileFlightEvent) => void): EventHandle;
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
