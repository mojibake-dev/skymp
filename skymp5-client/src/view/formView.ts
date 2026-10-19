import { Actor, ActorBase, createText, destroyText, Form, FormType, Game, Keyword, NetImmerse, ObjectReference, once, printConsole, setTextPos, setTextSize, setTextString, storage, TESModPlatform, Utility, worldPointToScreenPoint } from "skyrimPlatform";
import { setDefaultAnimsDisabled, applyAnimation } from "../sync/animation";
// @ts-expect-error (TODO: Remove in 2.10.0)
import { applyAnimationVariablesToActor, castReady, performAction, performActionResult, setActivateTextHidden, setActorAimHeading, setActorAimPitch } from "skyrimPlatform";
import { figureAimHeading } from "../sync/figureAim";
import { Appearance, applyAppearance } from "../sync/appearance";
import { isBadMenuShown, applyEquipment } from "../sync/equipment";
import { RespawnNeededError } from "../lib/errors";
import { FormModel } from "./model";
import { UpdateActionMessage } from "../services/messages/updateActionMessage";
import { applyMovement } from "../sync/movementApply";
import { SpawnProcess } from "./spawnProcess";
import { ObjectReferenceEx } from "../extensions/objectReferenceEx";
import { PlayerCharacterDataHolder } from "./playerCharacterDataHolder";
import { getMovement } from "../sync/movementGet";
import { setActorValuePercentage } from "../sync/actorvalues";
import { lastTryHost, tryHost } from "./hostAttempts";
import { ModelApplyUtils } from "./modelApplyUtils";
import { localIdToRemoteId } from "./worldViewMisc";
import { SpApiInteractor } from "../services/spApiInteractor";
import { WorldCleanerService } from "../services/services/worldCleanerService";
import { GamemodeUpdateService } from "../services/services/gamemodeUpdateService";

// thuum docs/verbs/cast-action.md: the want variable a hand's cast event names
// (MRh_ the right hand's, MLh_ and the perk's DualMagic_ the left hand's actions)
// the hands a cast start's event names: 0 left, 1 right; the perk's dual cast
// is the left hand's actions with both wants up, so either hand's answer
// counts for it
const castHandsOf = (eventNameLowerCase: string): number[] =>
  eventNameLowerCase.startsWith("dualmagic") ? [0, 1] : eventNameLowerCase.startsWith("mlh_") ? [0] : [1];

const castWantVariable = (eventNameLowerCase: string): string =>
  eventNameLowerCase.startsWith("mrh_") ? "bWantCastRight" : "bWantCastLeft";

export interface ScreenResolution {
  width: number;
  height: number;
}

let _screenResolution: ScreenResolution | undefined;
export const getScreenResolution = (): ScreenResolution => {
  if (!_screenResolution) {
    _screenResolution = {
      width: Utility.getINIInt("iSize W:Display"),
      height: Utility.getINIInt("iSize H:Display"),
    }
  }
  return _screenResolution;
}

export class FormView {
  constructor(private remoteRefrId?: number) { }

  update(model: FormModel): void {
    // Other players mutate into PC clones when moving to another location
    if (model.movement) {
      if (!this.lastWorldOrCell)
        this.lastWorldOrCell = model.movement.worldOrCell;
      if (this.lastWorldOrCell !== model.movement.worldOrCell) {
        printConsole(
          `[1] worldOrCell changed, destroying FormView ${this.lastWorldOrCell.toString(
            16
          )} => ${model.movement.worldOrCell.toString(16)}`
        );
        this.lastWorldOrCell = model.movement.worldOrCell;
        this.destroy();
        this.refrId = 0;
        this.appearanceBasedBaseId = 0;
        return;
      }
    }



    // Don't spawn dead actors if not already
    if (model.isDead) {
      if (this.refrId === 0) {
        return;
      }
    }

    // Players with different worldOrCell should be invisible
    if (model.movement) {
      const worldOrCell = ObjectReferenceEx.getWorldOrCell(Game.getPlayer() as Actor);
      if (
        worldOrCell !== 0 &&
        model.movement.worldOrCell !== worldOrCell
      ) {
        this.destroy();
        this.refrId = 0;
        return;
      }
    }

    // Apply appearance before base form selection to prevent double-spawn
    if (model.appearance || (!model.appearance && this.appearanceState.appearance)) {
      if (
        !this.appearanceState.appearance ||
        model.numAppearanceChanges !== this.appearanceState.lastNumChanges
      ) {

        // Both non-null
        if (model.appearance && this.appearanceState.appearance) {
          const modelAppearanceCopy: Appearance = JSON.parse(JSON.stringify(model.appearance));
          const stateAppearanceCopy: Appearance = JSON.parse(JSON.stringify(this.appearanceState.appearance));
          modelAppearanceCopy.name = "";
          stateAppearanceCopy.name = "";
          const equalWithoutNames = JSON.stringify(modelAppearanceCopy) === JSON.stringify(stateAppearanceCopy);

          if (equalWithoutNames) {
            // Change name inplace
            const refr = ObjectReference.from(Game.getFormEx(this.refrId));
            refr?.getBaseObject()?.setName(model.appearance.name);
            refr?.setDisplayName(model.appearance.name, true);
            //printConsole("Appearance updated, changing name inplace");
          } else {
            // Force re-apply appearance on the next getAppearanceBasedBase call
            this.appearanceBasedBaseId = 0;
            //printConsole("Appearance updated");
          }
        } else {
          // Force re-apply appearance on the next getAppearanceBasedBase call
          this.appearanceBasedBaseId = 0;
          //printConsole("Appearance updated");
        }

        this.appearanceState.appearance = model.appearance || null;
        this.appearanceState.lastNumChanges = model.numAppearanceChanges as number;
      }
    }

    const refId =
      model.refrId && model.refrId < 0xff000000 ? model.refrId : undefined;
    if (refId) {
      if (this.refrId !== refId) {
        this.destroy();
        this.refrId = model.refrId as number;
        this.ready = true;
        const refr = ObjectReference.from(Game.getFormEx(this.refrId));
        if (refr) {
          const base = refr.getBaseObject();
          if (base) {
            ObjectReferenceEx.dealWithRef(refr, base);
          }
        }
      }
    } else {
      let templateChain = model.templateChain;

      // There is no place for random/leveling in 1-sized chain
      // Just spawn an NPC, do not generate a temporary TESNPC form
      if (templateChain?.length === 1) {
        templateChain = undefined;
      }

      // TODO: getLeveledBase crashes too often ATM
      let base = null; //Game.getFormEx(this.getLeveledBase(templateChain));
      if (base === null) {
        base = Game.getFormEx(model.baseId || NaN);
      }
      if (base === null) {
        base = Game.getFormEx(this.getAppearanceBasedBase());
      }
      if (base === null) {
        return;
      }

      let refr = ObjectReference.from(Game.getFormEx(this.refrId));

      let respawnRequired = false;
      if (!refr) {
        respawnRequired = true;
      } else if (!refr.getBaseObject()) {
        respawnRequired = true;
      } else if ((refr.getBaseObject() as Form).getFormID() !== base.getFormID()) {
        respawnRequired = true;
      }

      if (respawnRequired) {
        this.destroy();

        const player = Game.getPlayer() as Actor;

        const spawnMethodOriginal = {
          spawn(baseForm: Form, _spawnPosition: [number, number, number], _spawnRotation: [number, number, number]): ObjectReference {
            return player.placeAtMe(
              baseForm,
              1,
              true,
              true
            ) as ObjectReference;
          },

          triggerSpawnProcess(spawningRefr: ObjectReference, spawnPosition: [number, number, number], appearance: Appearance | null, callback: () => void) {
            new SpawnProcess(
              appearance,
              spawnPosition,
              spawningRefr.getFormID(),
              callback
            );
          }
        };

        const spawnMethodStub = {
          spawn(baseForm: Form, spawnPosition: [number, number, number], spawnRotation: [number, number, number]): ObjectReference {
            const f = storage["formViewFunc1"] as Function;
            const ref: ObjectReference = f(baseForm, spawnPosition, spawnRotation);
            return ref;
          },

          triggerSpawnProcess(spawningRefr: ObjectReference, spawnPosition: [number, number, number], appearance: Appearance | null, callback: () => void) {
            const f = storage["formViewFunc2"] as Function;
            f(spawningRefr, spawnPosition, appearance, callback);
          }
        };

        const spawnUsingStubMethod = base.getType() === FormType.NPC
          && !this.appearanceState.appearance
          && storage["formViewFunc1Set"] === true
          && storage["formViewFunc2Set"] === true;
        const spawnMethod = spawnUsingStubMethod ? spawnMethodStub : spawnMethodOriginal;

        if (model.movement) {
          refr = spawnMethod.spawn(base, model.movement.pos, model.movement.rot);
        } else {
          printConsole("model.movement was " + model.movement);
        }

        this.state = {};
        delete this.wasHostedByOther;
        if (base.getType() !== FormType.NPC) {
          refr?.setAngle(
            model.movement?.rot[0] || 0,
            model.movement?.rot[1] || 0,
            model.movement?.rot[2] || 0
          );
        } else {
          const race = Actor.from(refr)?.getRace()?.getFormID();
          const draugrRace = 0xd53;
          const falmerRace = 0x131f4;
          const chaurusRace = 0x131eb;
          const frostbiteSpiderRaceGiant = 0x4e507;
          const frostbiteSpiderRaceLarge = 0x53477;
          const dwarvenCenturionRace = 0x131f1;
          const dwarvenSphereRace = 0x131f2;
          const dwarvenSpiderRace = 0x131f3;
          const sprigganRace = 0x2013b77;
          const sprigganRace2 = 0xf3903;
          const sprigganRace3 = 0x13204;
          const sprigganRace4 = 0x401b644;
          const sprigganRace5 = 0x9aa44;
          const wolfRace = 0x1320a;

          // potential masterambushscript
          if (race === draugrRace
            || race === falmerRace
            || race === chaurusRace
            || race === frostbiteSpiderRaceGiant
            || race === frostbiteSpiderRaceLarge
            || race === dwarvenCenturionRace
            || race === dwarvenSphereRace
            || race === dwarvenSpiderRace
            || race === sprigganRace
            || race === sprigganRace2
            || race === sprigganRace3
            || race === sprigganRace4
            || race === sprigganRace5
            || race === wolfRace) {
            Actor.from(refr)?.setActorValue("Aggression", 2);
          }
        }

        if (refr !== null) {
          SpApiInteractor.getControllerInstance().lookupListener(WorldCleanerService).modWcProtection(refr.getFormID(), 1);
        }

        // TODO: reset all states?
        this.eqState = this.getDefaultEquipState();
        this.animState = this.getDefaultAnimState();

        this.ready = false;

        let spawnPos;
        if (model.movement) {
          spawnPos = model.movement.pos;
          // printConsole("Spawn NPC at movement.pos");
        } else {
          spawnPos = ObjectReferenceEx.getPos(Game.getPlayer() as Actor);
          printConsole("Spawn NPC at player pos");
        }

        if (refr) {
          spawnMethod.triggerSpawnProcess(refr, spawnPos, model.appearance || null, () => {
            this.ready = true;
            this.spawnMoment = Date.now();
          });
        } else {
          printConsole("Unable to triggerSpawnProcess for null refr");
        }

        if (model.appearance && model.appearance.name) {
          refr?.setDisplayName("" + model.appearance.name, true);
        }
        Actor.from(refr)?.setActorValue("attackDamageMult", 0);
      }
      this.refrId = (refr as ObjectReference).getFormID();
    }

    if (!this.ready) {
      return;
    }

    const refr = ObjectReference.from(Game.getFormEx(this.refrId));
    if (refr) {
      const actor = Actor.from(refr);
      if (actor && !this.localImmortal) {
        // the figure never dies by this game's hand (the server's death is
        // the only death); its values are its player's, below, not a million
        actor.startDeferredKill();
        this.localImmortal = true;
      }
      this.applyFigureMaxima(actor, model);
      this.applyAll(refr, model);

      const gamemodeUpdateService = SpApiInteractor.getControllerInstance().lookupListener(GamemodeUpdateService);
      gamemodeUpdateService.updateNeighbor(refr, model, this.state);
    }
  }

  destroy(): void {
    this.isOnScreen = false;
    this.spawnMoment = 0;
    const refrId = this.refrId;
    once("update", () => {
      if (refrId >= 0xff000000) {
        const refr = ObjectReference.from(Game.getFormEx(refrId));
        if (refr) {
          refr.delete();
        }
        SpApiInteractor.getControllerInstance().lookupListener(WorldCleanerService).modWcProtection(refrId, -1);
        const ac = Actor.from(refr);
        if (ac) {
          TESModPlatform.setWeaponDrawnMode(ac, -1);
        }
      }
    })

    this.localImmortal = false;
    this.figureMaxima.clear();
    this.removeNickname();
  }

  private lastHarvestedApply = 0;
  private lastOpenApply = 0;
  private isSetNodeTextureSetApplied = false;
  private isSetNodeScaleApplied = false;

  // thuum docs/verbs/figure-values.md: the figure carries its player's
  // recorded maxima (the server's health, magicka and stamina props for an
  // observer, at spawn and on each change) as its base values, so a script
  // in this game reads Skyrim's number instead of the million that stood
  // here before. The shown fraction is kept across the set: the engine keeps
  // the current value, not the share, when a base changes
  private applyFigureMaxima(actor: Actor | null, model: FormModel) {
    // never the player's own actor: its maxima are its CreateActor's (the
    // isMe props, remoteServer.ts) and the engine's own; the server's
    // property updates reach the owner as a listener too
    if (!actor || actor.getFormID() === 0x14) {
      return;
    }
    const maxima: Array<[string, unknown]> = [
      ["health", model.health],
      ["magicka", model.magicka],
      ["stamina", model.stamina],
    ];
    for (const [name, value] of maxima) {
      if (typeof value !== "number" || !Number.isFinite(value) || value <= 0) {
        continue;
      }
      if (this.figureMaxima.get(name) === value) {
        continue;
      }
      const fraction = actor.getActorValuePercentage(name);
      actor.setActorValue(name, value);
      actor.restoreActorValue(name, value);
      setActorValuePercentage(actor, name, Math.min(Math.max(fraction, 0), 1));
      this.figureMaxima.set(name, value);
    }
  }

  private applyAll(refr: ObjectReference, model: FormModel) {
    let forcedWeapDrawn: boolean | null = null;

    if (PlayerCharacterDataHolder.getCrosshairRefId() === this.refrId) {
      this.lastHarvestedApply = 0;
      this.lastOpenApply = 0;
    }
    const now = Date.now();
    if (now - this.lastHarvestedApply > 666) {
      this.lastHarvestedApply = now;
      ModelApplyUtils.applyModelIsHarvested(refr, !!model.isHarvested);
    }
    if (now - this.lastOpenApply > 133) {
      this.lastOpenApply = now;
      ModelApplyUtils.applyModelIsOpen(refr, !!model.isOpen);
    }
    if (!this.isSetNodeScaleApplied) {
      this.isSetNodeScaleApplied = true;
      ModelApplyUtils.applyModelNodeScale(refr, model.setNodeScale);
    }
    if (!this.isSetNodeTextureSetApplied) {
      this.isSetNodeTextureSetApplied = true;
      ModelApplyUtils.applyModelNodeTextureSet(refr, model.setNodeTextureSet);
    }

    if (
      model.inventory &&
      PlayerCharacterDataHolder.getCrosshairRefId() == this.refrId &&
      !isBadMenuShown()
    ) {
      // Do not let actors breaking their equipment via inventory apply
      // However, actually, actors do not have inventory in their models
      // Except your clone.
      if (!Actor.from(refr)) {
        ModelApplyUtils.applyModelInventory(refr, model.inventory);
        model.inventory = undefined;
      }
    }

    if (model.animation) {
      // the relayed draw or sheathe flag forces the figure's drawn state only
      // while it is fresh, then the caster's movement decides: a redraw is an
      // engine action with no graph event (Magic_Equip), so nothing replaced
      // the sheathe's flag, and the figure stayed sheathed against its
      // movement, refusing every event and cast start until some other event
      // came (x-cast-interrupt 20261016-152919: 21 s, until a stagger)
      if (model.animation.numChanges !== this.drawnFlagNumChanges) {
        this.drawnFlagNumChanges = model.animation.numChanges;
        this.drawnFlagAt = Date.now();
      }
      if (Date.now() - this.drawnFlagAt <= FormView.drawnFlagMs) {
        if (model.animation.animEventName === "SkympFakeUnequip") {
          forcedWeapDrawn = false;
        } else if (model.animation.animEventName === "SkympFakeEquip") {
          forcedWeapDrawn = true;
        }
      }
    }

    // TODO: make host service
    const hosted = storage['hosted'];
    let alreadyHosted = false;
    if (Array.isArray(hosted)) {
      const remoteId = localIdToRemoteId(this.refrId);

      if (hosted.includes(remoteId) || hosted.includes(remoteId + 0x100000000)) {
        alreadyHosted = true;
      }
    }
    setDefaultAnimsDisabled(this.refrId, alreadyHosted ? false : true);

    if (alreadyHosted) {
      Actor.from(refr)?.clearKeepOffsetFromActor();
    }

    if (model.movement) {
      let ac = Actor.from(refr);
      if (
        this.movState.lastApply &&
        Date.now() - this.movState.lastApply > 1500
      ) {
        if (Date.now() - this.movState.lastRehost > 1000) {
          this.movState.lastRehost = Date.now();
          const remoteId = this.remoteRefrId;
          if (ac && ac.is3DLoaded()) {
            this.tryHostIfNeed(ac, remoteId as number);
            printConsole("tryHostIfNeed - reason: not seeing movement for long time");
          }
        }
      }

      if (
        +(model.numMovementChanges as number) !==
        this.movState.lastNumChanges ||
        Date.now() - this.movState.lastApply > 2000
      ) {
        this.movState.lastApply = Date.now();
        if (model.isHostedByOther || !this.movState.everApplied) {
          const backup = model.movement.isWeapDrawn;
          if (forcedWeapDrawn === true || forcedWeapDrawn === false) {
            model.movement.isWeapDrawn = forcedWeapDrawn;
          }
          try {
            applyMovement(refr, model.movement, !!model.isMyClone);
          } catch (e) {
            if (e instanceof RespawnNeededError) {
              this.lastWorldOrCell = model.movement.worldOrCell;
              this.destroy();
              this.refrId = 0;
              this.appearanceBasedBaseId = 0;
              return;
            } else {
              throw e;
            }
          }
          model.movement.isWeapDrawn = backup;

          this.movState.lastNumChanges = +(model.numMovementChanges as number);
          this.movState.everApplied = true;
        } else {
          const remoteId = this.remoteRefrId;
          if (ac && remoteId && ac.is3DLoaded()) {
            ac.clearKeepOffsetFromActor();

            // TODO: make host service
            const hosted = storage['hosted'];
            let alreadyHosted = false;
            if (Array.isArray(hosted)) {
              const remoteId = localIdToRemoteId(ac.getFormID());
              if (hosted.includes(remoteId) || hosted.includes(remoteId + 0x100000000)) {
                alreadyHosted = true;
              }
            }

            if (!alreadyHosted) {
              if (this.tryHostIfNeed(ac, remoteId)) {

                // previously, we did this cleanup on each update
                // but I guess it's too expensive and can possibly hurt FPS
                TESModPlatform.setWeaponDrawnMode(ac, -1);
              }
            }
          }
        }
      }
    }

    if (refr.is3DLoaded()) {
      // thuum docs/verbs/spell-cast.md: every relayed event since the last
      // update, oldest first, as the sender's graph took them (applyAnimation
      // skips one it has played by its numChanges); the newest alone when
      // none is queued (a spawn's). The hands' casts no longer come this way
      // (docs/verbs/cast-action.md: they are actions, replayed below)
      const queued = model.animationQueue;
      if (queued && queued.length > 0) {
        for (const anim of queued) {
          applyAnimation(refr, anim, this.animState);
        }
        queued.splice(0, queued.length);
      } else if (model.animation) {
        applyAnimation(refr, model.animation, this.animState);
      }
      // Use them only once, for spawning actors with correct animations
      this.animState.useAnimOverrides = false;

      // thuum docs/verbs/spell-cast.md (playtest nineteen: Flames aimed up
      // showed forward on the other screen): the engine aims an actor's
      // stream every frame from its aim angle, the reference's pitch or its
      // AimPitchCurrent, and nothing set either on a figure while it stood;
      // so each update a form another game drives (a player's figure, an NPC
      // another client hosts; the movement applies under the same test above)
      // takes its pitch from the movement (angle X, degrees, down positive)
      if (model.movement && !model.isMyClone && model.isHostedByOther) {
        const pitchDegrees = model.movement.rot[0];
        if (Number.isFinite(pitchDegrees) && Actor.from(refr)) {
          setActorAimPitch(refr.getFormID(), pitchDegrees * Math.PI / 180);
        }
        // and its heading from its caster's relayed cast while one runs
        // (sync/figureAim.ts; x-stream-aim 20261017-135526: the body's
        // heading left a figure's missiles 2.75 degrees off)
        const aimHeading = figureAimHeading.get(refr.getFormID());
        if (aimHeading !== undefined && Number.isFinite(aimHeading) && Actor.from(refr)) {
          setActorAimHeading(refr.getFormID(), aimHeading);
        }
      }

      this.replayActions(refr, model);
    }


    if (model.appearance) {
      const actor = Actor.from(refr);
      if (actor && !PlayerCharacterDataHolder.isInJumpState()) {
        if (PlayerCharacterDataHolder.getWorldOrCell()) {
          if (
            this.lastPcWorldOrCell &&
            PlayerCharacterDataHolder.getWorldOrCell() !== this.lastPcWorldOrCell
          ) {
            // Redraw tints if PC world/cell changed
            this.isOnScreen = false;
          }
          this.lastPcWorldOrCell = PlayerCharacterDataHolder.getWorldOrCell();
        }

        const headPos = [
          NetImmerse.getNodeWorldPositionX(actor, "NPC Head [Head]", false),
          NetImmerse.getNodeWorldPositionY(actor, "NPC Head [Head]", false),
          NetImmerse.getNodeWorldPositionZ(actor, "NPC Head [Head]", false),
        ];
        const [screenPoint] = worldPointToScreenPoint(headPos);
        const isOnScreen =
          screenPoint[0] > 0 &&
          screenPoint[1] > 0 &&
          screenPoint[2] > 0 &&
          screenPoint[0] < 1 &&
          screenPoint[1] < 1 &&
          screenPoint[2] < 1;
        if (isOnScreen != this.isOnScreen) {
          this.isOnScreen = isOnScreen;
          if (isOnScreen) {
            actor.queueNiNodeUpdate();
            (Game.getPlayer() as Actor).queueNiNodeUpdate();
          }
        }
      }
    }

    if (model.equipment) {
      if (this.eqState.lastNumChanges !== model.equipment.numChanges) {
        const ac = Actor.from(refr);
        // If we do not block inventory here, we will be able to reproduce the bug:
        // 1. Place ~90 bots and force them to reequip iron swords to the left hand (rate should be ~50ms)
        // 2. Open your inventory and reequip different items fast
        // 3. After 1-2 minutes close your inventory and see that HUD disappeared
        if (
          ac &&
          !isBadMenuShown() &&
          Date.now() - this.eqState.lastEqMoment > 500 &&
          Date.now() - this.spawnMoment > -1 &&
          this.spawnMoment > 0
        ) {
          //if (this.spawnMoment > 0 && Date.now() - this.spawnMoment > 5000) {
          if (applyEquipment(ac, model.equipment)) {
            this.eqState.lastNumChanges = model.equipment.numChanges;
          }
          this.eqState.lastEqMoment = Date.now();
          //}
          //const res: boolean = applyEquipment(ac, model.equipment);
          //if (res) this.eqState.lastNumChanges = model.equipment.numChanges;
        }
      }
    }

    // thuum docs/verbs/self-cast.md (playtest twenty: "yes hide it"): an
    // invisible figure's crosshair prompt is hidden with its name, set only
    // when its invisibility changes
    if (Actor.from(refr)) {
      const invisible = FormView.isInvisible(refr);
      if (invisible !== this.activateTextHidden) {
        this.activateTextHidden = invisible;
        setActivateTextHidden(refr.getFormID(), invisible);
      }
    }

    if (FormView.isDisplayingNicknames && this.refrId && model.appearance?.name) {
      const headPart = "NPC Head [Head]";
      const maxNicknameDrawDistance = 1000;
      const playerActor = Game.getPlayer()!;
      const isVisibleByPlayer = !model.movement?.isSneaking
        && playerActor.getDistance(refr) <= maxNicknameDrawDistance
        && playerActor.hasLOS(refr)
        && !this.isSweetHidePerson(refr)
        && !FormView.isInvisible(refr);
      if (isVisibleByPlayer) {
        const headScreenPos = worldPointToScreenPoint([
          NetImmerse.getNodeWorldPositionX(refr, headPart, false),
          NetImmerse.getNodeWorldPositionY(refr, headPart, false),
          NetImmerse.getNodeWorldPositionZ(refr, headPart, false) + 32
        ])[0];
        const resolution = getScreenResolution();
        const textXPos = Math.round(headScreenPos[0] * resolution.width);
        const textYPos = Math.round((1 - headScreenPos[1]) * resolution.height);

        if (!this.textNameId && headScreenPos[2] > 0) {
          this.textNameId = createText(textXPos, textYPos, refr.getDisplayName(), [1, 1, 1, 0.8]);
          setTextSize(this.textNameId, 0.5);
          SpApiInteractor.getControllerInstance().emitter.emit("nicknameCreate", {
            remoteRefrId: this.getRemoteRefrId(),
            textId: this.textNameId
          });
        } else {
          const deleteNickname = headScreenPos[2] < 0;
          if (deleteNickname) {
            this.removeNickname();
          }
          if (this.textNameId) {
            setTextPos(this.textNameId, textXPos, textYPos);
          }
        }
      } else {
        this.removeNickname();
      }
    } else {
      this.removeNickname();
    }
  }

  // thuum docs/verbs/self-cast.md (playtest seventeen: "when invisible
  // player 1's name plaquard still shows"): a figure invisible in this game
  // shows no name. Its player's Invisibility runs on it here (the relayed
  // Self cast), and the effect raises its Invisibility actor value
  // (CommonLibSSE-NG include/RE/I/InvisibilityEffect.h, a value modifier;
  // kInvisibility, include/RE/A/ActorValues.h:62)
  private static isInvisible(refr: ObjectReference): boolean {
    const actor = Actor.from(refr);
    return !!actor && actor.getActorValue("Invisibility") > 0;
  }

  private isSweetHidePerson(refr: ObjectReference): boolean {
    const actor = Actor.from(refr)
    if (!actor) {
      return false;
    }
    const keyword = Keyword.getKeyword('SweetHidePerson');
    return actor.wornHasKeyword(keyword);
  }

  private removeNickname() {
    if (this.textNameId) {
      SpApiInteractor.getControllerInstance().emitter.emit("nicknameDestroy", {
        remoteRefrId: this.getRemoteRefrId(),
        textId: this.textNameId
      });
      destroyText(this.textNameId);
      this.textNameId = undefined;
    }
  }

  private getAppearanceBasedBase(): number {
    const base = ActorBase.from(Game.getFormEx(this.appearanceBasedBaseId));
    if (!base && this.appearanceState.appearance) {
      this.appearanceBasedBaseId = applyAppearance(this.appearanceState.appearance).getFormID();
    }
    return this.appearanceBasedBaseId;
  }

  private getLeveledBase(templateChain: number[] | undefined): number {
    if (templateChain === undefined) {
      return 0;
    }

    const str = templateChain.join(',');

    if (this.leveledBaseId === 0) {
      // @ts-ignore
      const leveledBase = TESModPlatform.evaluateLeveledNpc(str);
      if (!leveledBase) {
        printConsole("Failed to evaluate leveled npc", str);
      }
      this.leveledBaseId = leveledBase?.getFormID() || 0;
    }

    return this.leveledBaseId;
  }

  private getDefaultEquipState() {
    return { lastNumChanges: 0, lastEqMoment: 0 };
  };

  private getDefaultAppearanceState() {
    return { lastNumChanges: 0, appearance: null as (null | Appearance) };
  };

  private getDefaultAnimState() {
    return { lastNumChanges: 0, useAnimOverrides: true };
  };

  private tryHostIfNeed(ac: Actor, remoteId: number) {
    const last = lastTryHost[remoteId];
    if (!last || Date.now() - last >= 1000) {
      lastTryHost[remoteId] = Date.now();

      if (
        getMovement(ac).worldOrCell ===
        getMovement(Game.getPlayer() as Actor).worldOrCell
      ) {
        tryHost(remoteId);
        return true;
      }
    }
    return false;
  };

  getLocalRefrId(): number {
    return this.refrId;
  }

  getRemoteRefrId(): number {
    return this.remoteRefrId as number;
  }

  // thuum docs/verbs/cast-action.md: the caster's actions, performed on the
  // figure through the engine's own action entry (the fork's performAction),
  // one per two updates: the caster's variables as the graph was asked go on
  // first, the action on the next update. The graph takes a cast's start only
  // once it has processed the want variable, at its next update: a start in
  // the same tick as its variables is refused every time, one an update later
  // is taken, holds the loop, and the Interrupt's Equipped exit ends it with
  // the engine's own timing (x-figure-action 20261015-055039, -bolt 055907,
  // -self 060152). Variables alone provoke nothing in the engine (the
  // response handler is a no-op on an idle caster, the re-analyst 2026-10-14),
  // so a queue that drains slowly leaves the figure in the caster's state.
  private replayActions(refr: ObjectReference, model: FormModel): void {
    // x-cast-interrupt (20261016-134830): a caster who sheathes mid-cast
    // performs no exit action (its engine ends the cast and performs
    // Unequip, which the figure takes through the animation relay), and the
    // view kept retrying the cast's Ready, writing the caster's casting
    // variables back each time, so the figure's hand stayed up until the
    // next start (4.9 to 5.2 s) and its graph refused the next draw. A
    // sheathed caster casts nothing: while its movement says so, the cast's
    // later actions (ready, release, exit) are dropped with the hands' want
    // variables; a start is kept, since a start can only follow a draw the
    // movement has not reported yet
    if (model.movement && model.movement.isWeapDrawn === false) {
      const isStart = (a: UpdateActionMessage) => a.eventName.toLowerCase().endsWith("start");
      let dropped = false;
      if (this.pendingAction && !isStart(this.pendingAction)) {
        this.pendingAction = null;
        this.pendingAsked = false;
        this.castRetries = 0;
        this.readyWaits = 0;
        dropped = true;
      }
      const queue = model.actionQueue;
      if (queue && queue.some((a) => !isStart(a))) {
        const starts = queue.filter(isStart);
        queue.splice(0, queue.length, ...starts);
        dropped = true;
      }
      if (dropped) {
        this.castEdgeCleared = false;
        this.dualCastOnFigure = false;
        ["bWantCastRight", "bWantCastLeft"].forEach((w) => refr.setAnimationVariableBool(w, false));
      }
    }
    const pending = this.pendingAction;
    if (pending) {
      if (!this.pendingAsked) {
        const pendingName = pending.eventName.toLowerCase();
        if (pendingName.endsWith("start")) {
          // the engine's handshake: its caster sets the want variable and
          // performs the Attack only when the graph answers BeginCast (the
          // hand's response handler); a start one update after its variables
          // was taken standing still (x-cast-gap 20261015-213755: 34 of 34,
          // each 4 to 7 ms after that answer) and refused about half the time
          // while the figure moved (playtest nineteen, x-cast-human 202506),
          // where the graph answers later. So the start waits for the answer;
          // without one for a second the want variable rises again, twice;
          // then it goes without (the shape before the handshake)
          const hands = castHandsOf(pendingName);
          const answered = hands.some((h) => castReady(refr.getFormID(), h));
          if (!answered && this.readyWaits < FormView.readyWaitLimit) {
            // the answer comes within a frame or two of the rise when it
            // comes at all (x-cast-gap 213755: 4 to 7 ms); past that the
            // start goes as caff3e04's did, one update after its variables
            // plus the retry below, which took 6 of 6 moving (b-cast-moving
            // 233639). 5df863fd waited three seconds for an answer its hook
            // never gave and every blind ask after that was refused
            // (x-cast-human 20261016-011836)
            this.readyWaits++;
            return;
          }
        }
        // queued to the game thread, which performs it right after this
        // update (MagicApi.cpp: performed inside the update, the action's
        // graph event re-entered Skyrim Platform's hooks and hung the game at
        // the first replay, x-cast-gap 20261015-211105); the graph's answer
        // is read on the next update
        performAction(refr.getFormID(), pending.action, pending.idle, pending.eventName, pending.type);
        this.pendingAsked = true;
        this.pendingWaits = 0;
        this.readyWaits = 0;
        return;
      }
      const answer = performActionResult(refr.getFormID());
      if (answer < 0 && this.pendingWaits < FormView.answerWaitLimit) {
        this.pendingWaits++;
        return;
      }
      this.pendingAsked = false;
      this.pendingAction = null;
      const taken = answer === 1;
      if (!taken) {
        // the caster's engine sets the want variable and performs the start
        // only when the graph answers, however many updates that takes; a
        // graph still finishing its last exit refused a start one update
        // after its variables (x-cast-twice-move 20261015-175846, 570 ms
        // after the exit; the playtest's rhythm), so the action goes again,
        // variables then action, for up to castRetries updates
        if (this.castRetries < FormView.castRetryLimit && model.actionQueue) {
          this.castRetries++;
          model.actionQueue.unshift(pending);
          return;
        }
        printConsole(`cast-action: performAction refused on ${refr.getFormID().toString(16)}: ${pending.eventName} after ${this.castRetries} retries`);
      }
      this.castRetries = 0;
      const name = pending.eventName.toLowerCase();
      if (name.startsWith("dualmagic")) {
        this.dualCastOnFigure = true;
      }
      if (name.includes("equipped")) {
        // the engine's own sequence after the Interrupt action is FinishCast,
        // which drops the hand's want variable; the action's variables are the
        // caster's from before it, so the figure kept the want up, and a want
        // already up when the next cast's variables land is no rise for the
        // graph: that cast was refused whole, and whether the caster's 500 ms
        // snapshot had dropped it first was a coin toss (playtest nineteen,
        // "every test only half works"; c2's log 10:38 to 10:41)
        const wants = this.dualCastOnFigure ? ["bWantCastRight", "bWantCastLeft"] : [castWantVariable(name)];
        wants.forEach((w) => refr.setAnimationVariableBool(w, false));
        this.dualCastOnFigure = false;
      }
      // the next action's variables may land on this same update
    }
    const queue = model.actionQueue;
    if (!queue || queue.length === 0) {
      return;
    }
    const first = queue[0];
    const firstName = first.eventName.toLowerCase();
    if (firstName.endsWith("start") && !this.castEdgeCleared) {
      // the graph enters a cast on the want variable's rise; one still up from
      // an earlier cast gives it none, so it is dropped one update before the
      // variables land (the same reason as above, from the other side)
      const wants = firstName.startsWith("dualmagic") ? ["bWantCastRight", "bWantCastLeft"] : [castWantVariable(firstName)];
      if (wants.some((w) => refr.getAnimationVariableBool(w))) {
        wants.forEach((w) => refr.setAnimationVariableBool(w, false));
        this.castEdgeCleared = true;
        return;
      }
    }
    this.castEdgeCleared = false;
    const next = queue.shift();
    if (!next) {
      return;
    }
    if (firstName.endsWith("start")) {
      // a stale answer is dropped before this rise (an exit's variables raise
      // the want too, and the graph answers that rise), so the wait above
      // sees the answer to this one
      castHandsOf(firstName).forEach((h) => castReady(refr.getFormID(), h));
    }
    const applied = applyAnimationVariablesToActor(refr.getFormID(), {
      booleans: new Uint8Array(next.actorAnimationVariables.booleans),
      floats: new Uint8Array(next.actorAnimationVariables.floats),
      integers: new Uint8Array(next.actorAnimationVariables.integers),
    });
    if (!applied) {
      printConsole(`cast-action: variables refused on ${refr.getFormID().toString(16)} before ${next.eventName}`);
    }
    this.pendingAction = next;
  }

  private refrId = 0;
  private ready = false;
  private animState = this.getDefaultAnimState();
  // thuum docs/verbs/cast-action.md: the relayed action whose variables went
  // onto the figure's graph on the last update, performed on this one; whether
  // a start's want variable was dropped on the last update for its rise; and
  // whether the figure is in the perk's dual cast (both wants up, one exit)
  // the relayed drawn flag's arrival (its numChanges and when), and how long
  // it outranks the caster's movement (the sender's own blockers hold the
  // movement to it for 300 ms)
  private activateTextHidden = false;
  private drawnFlagNumChanges = -1;
  private drawnFlagAt = 0;
  private static readonly drawnFlagMs = 1000;
  private pendingAction: UpdateActionMessage | null = null;
  // whether the pending action has been queued and its answer is awaited
  private pendingAsked = false;
  private pendingWaits = 0;
  private castEdgeCleared = false;
  private dualCastOnFigure = false;
  private castRetries = 0;
  // about half a second at 60 updates a second, two updates a try
  private static readonly castRetryLimit = 15;
  // updates to wait for the queue's answer before counting it refused
  private static readonly answerWaitLimit = 6;
  // updates a start waits for the graph's cast answer before it goes without
  // (the answer comes within a frame or two when it comes)
  private readyWaits = 0;
  private static readonly readyWaitLimit = 4;
  private movState = {
    lastNumChanges: 0,
    lastApply: 0,
    lastRehost: 0,
    everApplied: false,
  };
  private appearanceState = this.getDefaultAppearanceState();
  private eqState = this.getDefaultEquipState();
  private appearanceBasedBaseId = 0;
  private leveledBaseId = 0;
  private isOnScreen = false;
  private lastPcWorldOrCell = 0;
  private lastWorldOrCell = 0;
  private spawnMoment = 0;
  private wasHostedByOther: boolean | undefined = undefined;
  private state = {};
  private localImmortal = false;
  // the maxima set on the figure so far, by actor value name (figure-values)
  private figureMaxima = new Map<string, number>();
  private textNameId: number | undefined = undefined;


  public static isDisplayingNicknames: boolean = true;
}
