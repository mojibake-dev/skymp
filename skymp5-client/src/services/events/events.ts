import { EventEmitter } from "eventemitter3";
import { GameLoadEvent } from "./gameLoadEvent";
import { SendMessageEvent } from "./sendMessageEvent";
import { SendRawMessageEvent } from "./sendRawMessageEvent";
import { AnyMessage } from "../messages/anyMessage";
import { SendMessageWithRefrIdEvent } from "./sendMessageWithRefrIdEvent";
import { ApplyDeathStateEvent } from "./applyDeathStateEvent";
import { ConnectionFailed } from "./connectionFailed";
import { ConnectionDenied } from "./connectionDenied";
import { ConnectionAccepted } from "./connectionAccepted";
import { ConnectionDisconnect } from "./connectionDisconnect";
import { ConnectionMessage } from "./connectionMessage";
import { HostStartMessage } from "../messages/hostStartMessage";
import { HostStopMessage } from "../messages/hostStopMessage";
import { SetInventoryMessage } from "../messages/setInventoryMessage";
import { OpenContainerMessage } from "../messages/openContainerMessage";
import { ChangeValuesMessage } from "../messages/changeValuesMessage";
import { CreateActorMessage } from "../messages/createActorMessage";
import { DestroyActorMessage } from "../messages/destroyActorMessage";
import { SetRaceMenuOpenMessage } from "../messages/setRaceMenuOpenMessage";
import { SetGameTimeMessage } from "../messages/setGameTimeMessage";
import { FavoritesMessage } from "../messages/favoritesMessage";
import { RaceMenuPresetMessage } from "../messages/raceMenuPresetMessage";
import { ActorValuesMessage } from "../messages/actorValuesMessage";
import { ConsoleOutputMessage } from "../messages/consoleOutputMessage";
import { ArrowShotMessage } from "../messages/arrowShotMessage";
import { MagicEffectsMessage } from "../messages/magicEffectsMessage";
import { UpdateActionMessage } from "../messages/updateActionMessage";
import { ActorSpeechMessage } from "../messages/actorSpeechMessage";
import { DamageCauseMessage } from "../messages/damageCauseMessage";
import { HostedActorStateMessage } from "../messages/hostedActorStateMessage";
import { PlayerGlobalsMessage } from "../messages/playerGlobalsMessage";
import { QuestStageMessage } from "../messages/questStageMessage";
import { PropTransformMessage } from "../messages/propTransformMessage";
import { PropOwnerMessage } from "../messages/propOwnerMessage";
import { PlacedRequestMessage } from "../messages/placedRequestMessage";
import { LockPromptMessage } from "../messages/lockPromptMessage";
import { ActorJumpMessage } from "../messages/actorJumpMessage";
import { SpSnippetMessage } from "../messages/spSnippetMessage";
import { TeleportMessage } from "../messages/teleportMessage";
import { UpdateAnimationMessage } from "../messages/updateAnimationMessage";
import { UpdateAppearanceMessage } from "../messages/updateAppearanceMessage";
import { UpdateEquipmentMessage } from "../messages/updateEquipmentMessage";
import { UpdateGamemodeDataMessage } from "../messages/updateGameModeDataMessage";
import { UpdateMovementMessage } from "../messages/updateMovementMessage";
import { UpdatePropertyMessage } from "../messages/updatePropertyMessage";
import { DeathStateContainerMessage } from "../messages/deathStateContainerMessage";
import { TeleportMessage2 } from "../messages/teleportMessage2";
import { BrowserWindowLoadedEvent } from "./browserWindowLoadedEvent";
import { AuthAttemptEvent } from "./authAttemptEvent";
import { NewLocalLagValueCalculatedEvent } from "./newLocalLagValueCalculatedEvent";
import { AuthNeededEvent } from "./authNeededEvent";
import { QueryBlockSetInventoryEvent } from "./queryBlockSetInventoryEvent";
import { QueryKeyCodeBindings } from "./queryKeyCodeBindings";
import { SpellCastMessage } from "../messages/spellCastMessage";
import { UpdateAnimVariablesMessage } from "../messages/updateAnimVariablesMessage";
import { CustomPacketMessage } from "../messages/customPacketMessage";
import { HostGrantMessage } from "../messages/hostGrantMessage";
import { ActorFateMessage } from "../messages/actorFateMessage";
import { HostReleaseMessage } from "../messages/hostReleaseMessage";

import { AnyRawMessageEvent } from "./anyRawMessageEvent";
import { NicknameCreateEvent } from "./nicknameCreateEvent";
import { NicknameDestroyEvent } from "./nicknameDestroyEvent";

type EventTypes = {
    'gameLoad': [GameLoadEvent],

    'sendMessage': [SendMessageEvent<AnyMessage>],
    'sendRawMessage': [SendRawMessageEvent],
    'sendMessageWithRefrId': [SendMessageWithRefrIdEvent<AnyMessage>],

    'applyDeathStateEvent': [ApplyDeathStateEvent],

    'connectionFailed': [ConnectionFailed],
    'connectionDenied': [ConnectionDenied],
    'connectionAccepted': [ConnectionAccepted],
    'connectionDisconnect': [ConnectionDisconnect],

    'updateMovementMessage': [ConnectionMessage<UpdateMovementMessage>],
    'updateAnimationMessage': [ConnectionMessage<UpdateAnimationMessage>],
    'updateEquipmentMessage': [ConnectionMessage<UpdateEquipmentMessage>],
    'changeValuesMessage': [ConnectionMessage<ChangeValuesMessage>],
    'spellCastMessage': [ConnectionMessage<SpellCastMessage>],
    'updateAnimVariablesMessage': [ConnectionMessage<UpdateAnimVariablesMessage>],
    'updateAppearanceMessage': [ConnectionMessage<UpdateAppearanceMessage>],
    'teleportMessage': [ConnectionMessage<TeleportMessage>],
    'openContainerMessage': [ConnectionMessage<OpenContainerMessage>],
    'hostStartMessage': [ConnectionMessage<HostStartMessage>],
    'hostStopMessage': [ConnectionMessage<HostStopMessage>],
    'setInventoryMessage': [ConnectionMessage<SetInventoryMessage>],
    'createActorMessage': [ConnectionMessage<CreateActorMessage>],
    'destroyActorMessage': [ConnectionMessage<DestroyActorMessage>],
    'setRaceMenuOpenMessage': [ConnectionMessage<SetRaceMenuOpenMessage>],
    'spSnippetMessage': [ConnectionMessage<SpSnippetMessage>],
    'updateGamemodeDataMessage': [ConnectionMessage<UpdateGamemodeDataMessage>],
    'updatePropertyMessage': [ConnectionMessage<UpdatePropertyMessage>],
    'deathStateContainerMessage': [ConnectionMessage<DeathStateContainerMessage>],
    'teleportMessage2': [ConnectionMessage<TeleportMessage2>],
    'setGameTimeMessage': [ConnectionMessage<SetGameTimeMessage>],
    'favoritesMessage': [ConnectionMessage<FavoritesMessage>],
    'raceMenuPresetMessage': [ConnectionMessage<RaceMenuPresetMessage>],
    'actorValuesMessage': [ConnectionMessage<ActorValuesMessage>],
    'consoleOutputMessage': [ConnectionMessage<ConsoleOutputMessage>],
    'arrowShotMessage': [ConnectionMessage<ArrowShotMessage>],
    'magicEffectsMessage': [ConnectionMessage<MagicEffectsMessage>],
    'updateActionMessage': [ConnectionMessage<UpdateActionMessage>],
    'actorSpeechMessage': [ConnectionMessage<ActorSpeechMessage>],
    'damageCauseMessage': [ConnectionMessage<DamageCauseMessage>],
    'hostedActorStateMessage': [ConnectionMessage<HostedActorStateMessage>],
    'playerGlobalsMessage': [ConnectionMessage<PlayerGlobalsMessage>],
    'questStageMessage': [ConnectionMessage<QuestStageMessage>],
    'propTransformMessage': [ConnectionMessage<PropTransformMessage>],
    'propOwnerMessage': [ConnectionMessage<PropOwnerMessage>],
    'placedRequestMessage': [ConnectionMessage<PlacedRequestMessage>],
    'lockPromptMessage': [ConnectionMessage<LockPromptMessage>],
    'actorJumpMessage': [ConnectionMessage<ActorJumpMessage>],
    'hostGrantMessage': [ConnectionMessage<HostGrantMessage>],
    'actorFateMessage': [ConnectionMessage<ActorFateMessage>],
    // thuum docs/verbs/npc-puppet.md: HostService's grant or release of a cell, for PuppetService
    'cellHostChanged': [{ cell: number; hosted: boolean }],
    'hostReleaseMessage': [ConnectionMessage<HostReleaseMessage>],
    'customPacketMessage': [ConnectionMessage<CustomPacketMessage>]

    'browserWindowLoaded': [BrowserWindowLoadedEvent],
    'authAttempt': [AuthAttemptEvent],
    'authNeeded': [AuthNeededEvent],
    'anyMessage': [ConnectionMessage<AnyMessage>],
    'anyRawMessage': [AnyRawMessageEvent],
    'newLocalLagValueCalculated': [NewLocalLagValueCalculatedEvent],
    'queryBlockSetInventoryEvent': [QueryBlockSetInventoryEvent],
    'queryKeyCodeBindings': [QueryKeyCodeBindings],
    'nicknameCreate': [NicknameCreateEvent],
    'nicknameDestroy': [NicknameDestroyEvent]
}

// https://blog.makerx.com.au/a-type-safe-event-emitter-in-node-js/
interface TypedEventEmitter<TEvents extends Record<string, any>> {
    emit<TEventName extends keyof TEvents & string>(
        eventName: TEventName,
        ...eventArg: TEvents[TEventName]
    ): void;

    on<TEventName extends keyof TEvents & string>(
        eventName: TEventName,
        handler: (...eventArg: TEvents[TEventName]) => void
    ): void;

    off<TEventName extends keyof TEvents & string>(
        eventName: TEventName,
        handler: (...eventArg: TEvents[TEventName]) => void
    ): void;
}

export type EventEmitterType = TypedEventEmitter<EventTypes>;

export class EventEmitterFactory {
    static makeEventEmitter(): EventEmitterType {
        return (new EventEmitter()) as EventEmitterType;
    }
}
