import { Actor, ActorBase, createText, destroyText, Form, FormType, Game, Keyword, NetImmerse, ObjectReference, once, printConsole, setTextPos, setTextSize, setTextString, storage, TESModPlatform, Utility, worldPointToScreenPoint } from "skyrimPlatform";
import { setDefaultAnimsDisabled, applyAnimation } from "../sync/animation";
// @ts-expect-error (TODO: Remove in 2.10.0)
import { applyAnimationVariablesToActor, performAction } from "skyrimPlatform";
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
import { lastTryHost, tryHost } from "./hostAttempts";
import { ModelApplyUtils } from "./modelApplyUtils";
import { localIdToRemoteId } from "./worldViewMisc";
import { SpApiInteractor } from "../services/spApiInteractor";
import { WorldCleanerService } from "../services/services/worldCleanerService";
import { GamemodeUpdateService } from "../services/services/gamemodeUpdateService";

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
        actor.startDeferredKill();
        actor.setActorValue("health", 1000000);
        actor.setActorValue("magicka", 1000000);
        this.localImmortal = true;
      }
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
    this.removeNickname();
  }

  private lastHarvestedApply = 0;
  private lastOpenApply = 0;
  private isSetNodeTextureSetApplied = false;
  private isSetNodeScaleApplied = false;

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
      if (model.animation.animEventName === "SkympFakeUnequip") {
        forcedWeapDrawn = false;
      } else if (model.animation.animEventName === "SkympFakeEquip") {
        forcedWeapDrawn = true;
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
    const pending = this.pendingAction;
    if (pending) {
      this.pendingAction = null;
      const taken = performAction(refr.getFormID(), pending.action, pending.idle, pending.eventName, pending.type);
      if (!taken) {
        printConsole(`cast-action: performAction refused on ${refr.getFormID().toString(16)}: ${pending.eventName}`);
      }
      return;
    }
    const queue = model.actionQueue;
    if (!queue || queue.length === 0) {
      return;
    }
    const next = queue.shift();
    if (!next) {
      return;
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
  // onto the figure's graph on the last update, performed on this one
  private pendingAction: UpdateActionMessage | null = null;
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
  private textNameId: number | undefined = undefined;


  public static isDisplayingNicknames: boolean = true;
}
