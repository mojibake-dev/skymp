import {
  Game,
  Utility,
  once
} from "skyrimPlatform";
import { SkympClient } from "./services/services/skympClient";

import * as sp from "skyrimPlatform";

import { BlockPapyrusEventsService } from './services/services/blockPapyrusEventsService';
import { EnforceLimitationsService } from './services/services/enforceLimitationsService';
import { LoadGameService } from './services/services/loadGameService';
import { SendInputsService } from './services/services/sendInputsService';
import { SinglePlayerService } from './services/services/singlePlayerService';
import { SpApiInteractor } from './services/spApiInteractor';
import { TimeService } from "./services/services/timeService";
import { MapMarkersService } from "./services/services/mapMarkersService";
import { IngredientEffectsService } from "./services/services/ingredientEffectsService";
import { FavoritesService } from "./services/services/favoritesService";
import { RaceMenuService } from "./services/services/raceMenuService";
import { ActorValuesService } from "./services/services/actorValuesService";
import { PlayerGlobalsService } from "./services/services/playerGlobalsService";
import { QuestStageService } from "./services/services/questStageService";
import { PropService } from "./services/services/propService";
import { SpellPlacedService } from "./services/services/spellPlacedService";
import { LockService } from "./services/services/lockService";
import { WeatherService } from "./services/services/weatherService";
import { PerkService } from "./services/services/perkService";
import { SkillUseService } from "./services/services/skillUseService";
import { ArrowSyncService } from "./services/services/arrowSyncService";
import { SpVersionCheckService } from "./services/services/spVersionCheckService";
import { ConsoleCommandsService } from "./services/services/consoleCommandsService";
import { LastInvService } from "./services/services/lastInvService";
import { ActivationService } from "./services/services/activationService";
import { CraftService } from "./services/services/craftService";
import { DropItemService } from "./services/services/dropItemService";
import { HitService } from "./services/services/hitService";
import { RagdollService } from "./services/services/ragdollService";
import { DeathService } from "./services/services/deathService";
import { HeadshotPredictionService } from "./services/services/headshotPredictionService";
import { ContainersService } from "./services/services/containersService";
import { NetworkingService } from "./services/services/networkingService";
import { RemoteServer } from "./services/services/remoteServer";
import { HostService } from "./services/services/hostService";
import { PuppetService } from "./services/services/puppetService";
import { ActorFateService } from "./services/services/actorFateService";
import { DeathReportService } from "./services/services/deathReportService";
import { HostedStateService } from "./services/services/hostedStateService";
import { DamageCauseService } from "./services/services/damageCauseService";
import { NodeOverridesService } from "./services/services/nodeOverridesService";
import { PuppetFaceService } from "./services/services/puppetFaceService";
import { TalkService } from "./services/services/talkService";
import { VoiceService } from "./services/services/voiceService";
import { SpSnippetService } from "./services/services/spSnippetService";
import { SweetTaffyDynamicPerksService } from "./services/services/sweetTaffyDynamicPerksService";
import { SweetTaffySweetCantDropService } from "./services/services/sweetTaffySweetCantDropService";
import { SweetTaffyStaticPerksService } from "./services/services/sweetTaffyStaticPerksService";
import { DisableSkillAdvanceService } from "./services/services/disableSkillAdvanceService";
import { DisableFastTravelService } from "./services/services/disableFastTravelService";
import { DisableDifficultySelectionService } from "./services/services/disableDifficultySelectionService";
import { SweetTaffyPlayerCombatService } from "./services/services/sweetTaffyPlayerCombatService";
import { WorldCleanerService } from "./services/services/worldCleanerService";
import { SweetTaffySkillMenuService } from "./services/services/sweetTaffySkillMenuService";
import { LoadOrderVerificationService } from "./services/services/loadOrderVerificationService";
import { BrowserService } from "./services/services/browserService";
import { AuthService } from "./services/services/authService";
import { NetInfoService } from "./services/services/netInfoService";
import { AnimDebugService } from "./services/services/animDebugService";
import { TimersService } from "./services/services/timersService";
import { PlayerBowShotService } from "./services/services/playerBowShotService";
import { GamemodeEventSourceService } from "./services/services/gamemodeEventSourceService";
import { GamemodeUpdateService } from "./services/services/gamemodeUpdateService";
import { FrontHotReloadService } from "./services/services/frontHotReloadService";
import { BlockedAnimationsService } from "./services/services/blockedAnimationsService";
import { WorldView } from "./view/worldView";
import { KeyboardEventsService } from "./services/services/keyboardEventsService";
import { MagicSyncService } from "./services/services/magicSyncService";
import { ActionSyncService } from "./services/services/actionSyncService";
import { ShoutService } from "./services/services/shoutService";
import { MagicEffectsService } from "./services/services/magicEffectsService";
import { ProfilingService } from "./services/services/profilingService";
import { SettingsService } from "./services/services/settingsService";
import { SweetCameraEnforcementService } from "./services/services/sweetCameraEnforcementService";
import { SweetTaffyNicknamesService } from "./services/services/sweetTaffyNicknamesService";
import { ServerJsVerificationService } from "./services/services/serverJsVerificationService";
import { SweetTaffyEvalService } from "./services/services/sweetTaffyEvalService";

once("update", () => {
  Utility.setINIBool("bAlwaysActive:General", true);
  Game.setGameSettingInt("iDeathDropWeaponChance", 0);
  Utility.setINIFloat("fAutoVanityModeDelay:Camera", 3600);
});

const main = () => {
  try {
    const controller = SpApiInteractor.getControllerInstance();

    const listeners = [
      new BlockPapyrusEventsService(sp, controller),
      new LoadGameService(sp, controller),
      new SinglePlayerService(sp, controller),
      new EnforceLimitationsService(sp, controller),
      new SendInputsService(sp, controller),
      new SkympClient(sp, controller),
      new TimeService(sp, controller),
      new MapMarkersService(sp, controller),
      new IngredientEffectsService(sp, controller),
      new FavoritesService(sp, controller),
      new RaceMenuService(sp, controller),
      new ActorValuesService(sp, controller),
      new PlayerGlobalsService(sp, controller),
      new QuestStageService(sp, controller),
      new PropService(sp, controller),
      new SpellPlacedService(sp, controller),
      new LockService(sp, controller),
      new WeatherService(sp, controller),
      new PerkService(sp, controller),
      new SkillUseService(sp, controller),
      new ArrowSyncService(sp, controller),
      new SpVersionCheckService(sp, controller),
      new ConsoleCommandsService(sp, controller),
      new LastInvService(sp, controller),
      new ActivationService(sp, controller),
      new CraftService(sp, controller),
      new DropItemService(sp, controller),
      new HitService(sp, controller),
      new RagdollService(sp, controller),
      new DeathService(sp, controller),
      new HeadshotPredictionService(sp, controller),
      new ContainersService(sp, controller),
      new NetworkingService(sp, controller),
      new RemoteServer(sp, controller),
      new HostService(sp, controller),
      new PuppetService(sp, controller),
      new ActorFateService(sp, controller),
      new DeathReportService(sp, controller),
      new HostedStateService(sp, controller),
      new DamageCauseService(sp, controller),
      new NodeOverridesService(sp, controller),
      new PuppetFaceService(sp, controller),
      new TalkService(sp, controller),
      new VoiceService(sp, controller),
      new SpSnippetService(sp, controller),
      new SettingsService(sp, controller),
      new SweetTaffyDynamicPerksService(sp, controller),
      new SweetTaffyStaticPerksService(sp, controller),
      new SweetTaffySweetCantDropService(sp, controller),
      new SweetTaffyPlayerCombatService(sp, controller),
      new SweetTaffySkillMenuService(sp, controller),
      new SweetCameraEnforcementService(sp, controller),
      new SweetTaffyEvalService(sp, controller),
      new DisableSkillAdvanceService(sp, controller),
      new DisableFastTravelService(sp, controller),
      new DisableDifficultySelectionService(sp, controller),
      new WorldCleanerService(sp, controller),
      new LoadOrderVerificationService(sp, controller),
      new BrowserService(sp, controller),
      new AuthService(sp, controller),
      new NetInfoService(sp, controller),
      new AnimDebugService(sp, controller),
      new TimersService(sp, controller),
      new PlayerBowShotService(sp, controller),
      new GamemodeEventSourceService(sp, controller),
      new GamemodeUpdateService(sp, controller),
      new FrontHotReloadService(sp, controller),
      new BlockedAnimationsService(sp, controller),
      new WorldView(sp, controller),
      new KeyboardEventsService(sp, controller),
      new MagicSyncService(sp, controller),
      new ActionSyncService(sp, controller),
      new ShoutService(sp, controller),
      new MagicEffectsService(sp, controller),
      new ProfilingService(sp, controller),
      new SweetTaffyNicknamesService(sp, controller),
      new ServerJsVerificationService(sp, controller)
    ];
    SpApiInteractor.setup(listeners);
  } catch (e) {
    // TODO: handle setup failure. will output to game console by default
    throw e;
  }
};

// [18.08.2023]
// I saw "attempt to call hooks.add while in hook context" error
// I'm not sure if it's a C++ bug in SkyrimPlatform or an artifact of webpack+hotreload
// But let's for now ensure that "main" executes inside tick context
once("tick", main);
