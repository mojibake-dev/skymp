
import { ActivateMessage } from "./activateMessage";
import { ChangeValuesMessage } from "./changeValuesMessage";
import { ConsoleCommandMessage } from "./consoleCommandMessage";
import { CraftItemMessage } from "./craftItemMessage";
import { CreateActorMessage } from "./createActorMessage";
import { CustomEventMessage } from "./customEventMessage";
import { CustomPacketMessage } from "./customPacketMessage";
import { DeathStateContainerMessage } from "./deathStateContainerMessage";
import { DestroyActorMessage } from "./destroyActorMessage";
import { DropItemMessage } from "./dropItemMessage";
import { FinishSpSnippetMessage } from "./finishSpSnippetMessage";
import { HitMessage } from "./hitMessage";
import { SpellCastMessage } from "./spellCastMessage";
import { UpdateAnimVariablesMessage } from "./updateAnimVariablesMessage";
import { HostMessage } from "./hostMessage";
import { HostStartMessage } from "./hostStartMessage";
import { HostStopMessage } from "./hostStopMessage";
import { CellLoadedMessage } from "./cellLoadedMessage";
import { HostGrantMessage } from "./hostGrantMessage";
import { HostReleaseMessage } from "./hostReleaseMessage";
import { ActorSightingsMessage } from "./actorSightingsMessage";
import { ActorFateMessage } from "./actorFateMessage";
import { ActorDeathMessage } from "./actorDeathMessage";
import { OnEquipMessage } from "./onEquipMessage";
import { OpenContainerMessage } from "./openContainerMessage";
import { PlayerBowShotMessage } from "./playerBowShotMessage";
import { PutItemMessage } from "./putItemMessage";
import { SetGameTimeMessage } from "./setGameTimeMessage";
import { RestIntentMessage } from "./restIntentMessage";
import { MapMarkerDiscoveredMessage } from "./mapMarkerDiscoveredMessage";
import { IngredientEffectsKnownMessage } from "./ingredientEffectsKnownMessage";
import { FavoritesMessage } from "./favoritesMessage";
import { RaceMenuPresetMessage } from "./raceMenuPresetMessage";
import { ActorValuesMessage } from "./actorValuesMessage";
import { ConsoleOutputMessage } from "./consoleOutputMessage";
import { ArrowShotMessage } from "./arrowShotMessage";
import { MagicEffectsMessage } from "./magicEffectsMessage";
import { UpdateActionMessage } from "./updateActionMessage";
import { PlayerGlobalsMessage } from "./playerGlobalsMessage";
import { ActorJumpMessage } from "./actorJumpMessage";
import { SetInventoryMessage } from "./setInventoryMessage";
import { SetRaceMenuOpenMessage } from "./setRaceMenuOpenMessage";
import { SpSnippetMessage } from "./spSnippetMessage";
import { TakeItemMessage } from "./takeItemMessage";
import { TeleportMessage } from "./teleportMessage";
import { TeleportMessage2 } from "./teleportMessage2";
import { UpdateAnimationMessage } from "./updateAnimationMessage";
import { UpdateAppearanceMessage } from "./updateAppearanceMessage";
import { UpdateEquipmentMessage } from "./updateEquipmentMessage";
import { UpdateGamemodeDataMessage } from "./updateGameModeDataMessage";
import { UpdateMovementMessage } from "./updateMovementMessage";
import { UpdatePropertyMessage } from "./updatePropertyMessage";
import { HostedActorStateMessage } from "./hostedActorStateMessage";
import { ActorSpeechMessage } from "./actorSpeechMessage";
import { DamageCauseMessage } from "./damageCauseMessage";
import { ActorClaimMessage } from "./actorClaimMessage";
import { QuestStageMessage } from "./questStageMessage";
import { PropTransformMessage } from "./propTransformMessage";
import { PropClaimMessage } from "./propClaimMessage";
import { PropOwnerMessage } from "./propOwnerMessage";
import { PlacedRequestMessage } from "./placedRequestMessage";
import { PlacedReportMessage } from "./placedReportMessage";
import { LockReportMessage } from "./lockReportMessage";
import { LockPromptMessage } from "./lockPromptMessage";

export type AnyMessage = ActivateMessage
    | ConsoleCommandMessage
    | PutItemMessage
    | TakeItemMessage
    | CraftItemMessage
    | DropItemMessage
    | HitMessage
    | SpellCastMessage
    | UpdateAnimVariablesMessage
    | OnEquipMessage
    | UpdateMovementMessage
    | UpdateAnimationMessage
    | UpdateEquipmentMessage
    | ChangeValuesMessage
    | UpdateAppearanceMessage
    | HostMessage
    | CustomEventMessage
    | CustomPacketMessage
    | FinishSpSnippetMessage
    | TeleportMessage
    | OpenContainerMessage
    | HostStartMessage
    | HostStopMessage
    | SetInventoryMessage
    | CreateActorMessage
    | DestroyActorMessage
    | SetRaceMenuOpenMessage
    | SetGameTimeMessage
    | RestIntentMessage
    | MapMarkerDiscoveredMessage
    | IngredientEffectsKnownMessage
    | FavoritesMessage
    | RaceMenuPresetMessage
    | ActorValuesMessage
    | ConsoleOutputMessage
    | ArrowShotMessage
    | MagicEffectsMessage
    | UpdateActionMessage
    | PlayerGlobalsMessage
    | ActorJumpMessage
    | CellLoadedMessage
    | HostGrantMessage
    | HostReleaseMessage
    | ActorSightingsMessage
    | ActorFateMessage
    | ActorDeathMessage
    | HostedActorStateMessage
    | ActorSpeechMessage
    | DamageCauseMessage
    | ActorClaimMessage
    | QuestStageMessage
    | PropTransformMessage
    | PropClaimMessage
    | PropOwnerMessage
    | PlacedRequestMessage
    | PlacedReportMessage
    | LockReportMessage
    | LockPromptMessage
    | SpSnippetMessage
    | UpdateGamemodeDataMessage
    | UpdatePropertyMessage
    | DeathStateContainerMessage
    | TeleportMessage2
    | PlayerBowShotMessage
