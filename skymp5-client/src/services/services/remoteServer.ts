// @ts-expect-error (TODO: Remove in 2.10.0)
import { Actor, Form, FormType, MagicEffect, Menu, interruptCast, castSpellImmediate, placeAreaEffect, printConsole, applyAnimationVariablesToActor, getAnimationVariablesFromActor, ActorAnimationVariables } from 'skyrimPlatform';
import { figureAimHeading } from "../../sync/figureAim";
import { landsOnItsTarget } from "../../sync/spellDelivery";
import {
  Armor,
  Cell,
  Game,
  ObjectReference,
  TESModPlatform,
  Ui,
  Utility,
  WorldSpace,
  on, // TODO: use this.controller.on instead
  once, // TODO: use this.controller.once instead
  Spell,
  storage, // TODO: use this.sp.storage instead
} from 'skyrimPlatform';

import * as messages from '../../messages';

/* eslint-disable @typescript-eslint/no-empty-function */
import { ObjectReferenceEx } from '../../extensions/objectReferenceEx';
import { IdManager } from '../../lib/idManager';
import { nameof } from '../../lib/nameof';
import { setActorValuePercentage } from '../../sync/actorvalues';
import { applyAppearanceToPlayer } from '../../sync/appearance';
import { applyEquipment, isBadMenuShown } from '../../sync/equipment';
import { Inventory, applyInventory } from '../../sync/inventory';
import { Movement } from '../../sync/movement';
import { syncSpells } from '../../sync/spell';
import { ModelApplyUtils } from '../../view/modelApplyUtils';
import { FormModel, WorldModel } from '../../view/model';
import { LoadGameService } from './loadGameService';
import { UpdateMovementMessage } from '../messages/updateMovementMessage';
import { ChangeValuesMessage } from '../messages/changeValuesMessage';
import { UpdateAnimationMessage } from '../messages/updateAnimationMessage';
import { UpdateEquipmentMessage } from '../messages/updateEquipmentMessage';
import { RagdollService } from './ragdollService';
import { UpdateAppearanceMessage } from '../messages/updateAppearanceMessage';
import { TeleportMessage } from '../messages/teleportMessage';
import { DeathStateContainerMessage } from '../messages/deathStateContainerMessage';
import { RespawnNeededError } from '../../lib/errors';
import { OpenContainerMessage } from '../messages/openContainerMessage';
import { ActivateMessage } from '../messages/activateMessage';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { ConnectionMessage } from '../events/connectionMessage';
import { SetInventoryMessage } from '../messages/setInventoryMessage';
import { CreateActorMessage, CreateActorMessageAdditionalProps } from '../messages/createActorMessage';
import { DestroyActorMessage } from '../messages/destroyActorMessage';
import { SetRaceMenuOpenMessage } from '../messages/setRaceMenuOpenMessage';
import { UpdatePropertyMessage } from '../messages/updatePropertyMessage';
import { TeleportMessage2 } from '../messages/teleportMessage2';

// TODO: refactor worldViewMisc into service
import {
  getObjectReference,
  getViewFromStorage,
  remoteIdToLocalId,
} from '../../view/worldViewMisc';
import { TimeService } from './timeService';
import { logTrace, logError } from '../../logging';

import { SpellCastMessage } from '../messages/spellCastMessage';
import { ActorJumpMessage } from '../messages/actorJumpMessage';
import { UpdateAnimVariablesMessage } from '../messages/updateAnimVariablesMessage';
import { UpdateActionMessage } from '../messages/updateActionMessage';
import { MsgType } from '../../messages';

export const getPcInventory = (): Inventory | undefined => {
  const res = storage['pcInv'];
  if (typeof res === 'object' && (res as any)['entries']) {
    return res as Inventory;
  }
  return undefined;
};

const setPcInventory = (inv: Inventory): void => {
  storage['pcInv'] = inv;
};

let pcInvLastApply = 0;
on('update', () => {
  if (isBadMenuShown()) {
    return;
  }
  if (Date.now() - pcInvLastApply > 5000) {
    pcInvLastApply = Date.now();
    const pcInv = getPcInventory();
    if (pcInv) {
      applyInventory(Game.getPlayer()!, pcInv, false, true);
    }
  }
});

const unequipIronHelmet = () => {
  const ironHelment = Armor.from(Game.getFormEx(0x00012e4d));
  const pl = Game.getPlayer();
  if (pl) {
    pl.unequipItem(ironHelment, false, true);
  }
};

// SkyMP's rotations are degrees (the client sends ObjectReference.GetAngleX,
// Y and Z); TESModPlatform.moveRefrToPosition hands them to the engine's
// TESObjectREFR::MoveTo_Impl, which stores radians (CommonLibSSE-NG
// TESObjectREFR.cpp: GetAngleZ returns data.angle.z unconverted). A
// teleport to 180 faced 233.24 degrees (180 radians) in thuum's lab.
const degreesToRadians = (degrees: number) => degrees * Math.PI / 180;

// a burst longer than this keeps its newest events: a sender's burst, and
// what arrives while the view holds a hand's spell start for up to a second
// (view/formView.ts)
const maxQueuedAnimations = 32;
// thuum docs/verbs/cast-action.md: a cast is three actions; a burst from two
// hands stays under this
const maxQueuedActions = 16;
// the casting booleans in Skyrim Platform's master behavior list, by index
// (the fork's skyrim-platform/src/platform_se/skyrim_platform/Magic/
// AnimVariableMasterGraphIndexes.cpp, CreateDefault: IsCastingDual 5,
// IsCastingRight 7, IsCastingLeft 8, bWantCastLeft 14, bWantCastRight 15,
// bCastReady 20, NotCasting 59), packed one byte each
const figureCastingBooleanIndexes = [5, 7, 8, 14, 15, 20, 59];

export class RemoteServer extends ClientListener {
  constructor(private sp: Sp, private controller: CombinedController) {
    super();

    this.controller.emitter.on("setInventoryMessage", (e) => this.onSetInventoryMessage(e));
    this.controller.emitter.on("openContainerMessage", (e) => this.onOpenContainerMessage(e));
    this.controller.emitter.on("updateMovementMessage", (e) => this.onUpdateMovementMessage(e));
    this.controller.emitter.on("updateAnimationMessage", (e) => this.onUpdateAnimationMessage(e));
    this.controller.emitter.on("updateEquipmentMessage", (e) => this.onUpdateEquipmentMessage(e));
    this.controller.emitter.on("changeValuesMessage", (e) => this.onChangeValuesMessage(e));
    this.controller.emitter.on("updateAppearanceMessage", (e) => this.onUpdateAppearanceMessage(e));
    this.controller.emitter.on("teleportMessage", (e) => this.onTeleportMessage(e));
    this.controller.emitter.on("teleportMessage2", (e) => this.onTeleportMessage(e));
    this.controller.emitter.on("createActorMessage", (e) => this.onCreateActorMessage(e));
    this.controller.emitter.on("destroyActorMessage", (e) => this.onDestroyActorMessage(e));
    this.controller.emitter.on("setRaceMenuOpenMessage", (e) => this.onSetRaceMenuOpenMessage(e));
    this.controller.emitter.on("updatePropertyMessage", (e) => this.onUpdatePropertyMessage(e));
    this.controller.emitter.on("deathStateContainerMessage", (e) => this.onDeathStateContainerMessage(e));

    this.controller.emitter.on("connectionAccepted", () => this.handleConnectionAccepted());

    this.controller.emitter.on("spellCastMessage", (e) => this.onSpellCastMessage(e));
    this.controller.emitter.on("actorJumpMessage", (e) => this.onActorJumpMessage(e));
    this.controller.emitter.on("updateAnimVariablesMessage", (e) => this.onUpdateAnimVariablesMessage(e));
    this.controller.emitter.on("updateActionMessage", (e) => this.onUpdateActionMessage(e));

  }

  private onSetInventoryMessage(event: ConnectionMessage<SetInventoryMessage>): void {
    this.numSetInventory++;

    const msg = event.message;
    once('update', () => {
      setPcInventory(msg.inventory);

      let blocked = false;

      this.controller.emitter.emit('queryBlockSetInventoryEvent', {
        block: () => blocked = true
      });

      if (!blocked) {
        pcInvLastApply = 0;
      }
    });
  }

  private onOpenContainerMessage(event: ConnectionMessage<OpenContainerMessage>): void {
    once('update', async () => {
      await Utility.wait(0.1); // Give a chance to update inventory

      const remoteId = event.message.target;
      const localId = remoteIdToLocalId(remoteId);
      const refr = ObjectReference.from(Game.getFormEx(localId));

      if (refr === null) {
        logError(this, 'onOpenContainerMessage - refr not found', 'remoteId', remoteId.toString(16), 'localId', localId.toString(16));
        return;
      }

      refr.activate(Game.getPlayer(), true);

      const baseObject = refr.getBaseObject();
      const baseType = baseObject?.getType();

      let functionChecker: (() => boolean) | null = null;
      let factName = "";
      let delaySeconds = -1.0;
      if (baseType === FormType.Container) {
        functionChecker = () => Ui.isMenuOpen("ContainerMenu");
        factName = "'ContainerMenu open'";
        delaySeconds = 0.0;
      } else if (baseType === FormType.Furniture) {
        functionChecker = () => !!Game.getPlayer()?.getFurnitureReference();
        factName = "'getFurnitureReference not null'";
        delaySeconds = 1.0;
      }

      if (functionChecker === null) {
        logTrace(this, "onOpenContainerMesage - not a container or furniture", baseType);
        return;
      }

      // In SkyMP containers have 2-nd, closing activation under the hood.
      // This differs from Skyrim's behavior, where it's just one activation.

      (async () => {
        // thuum docs/verbs/activate.md (train-29): the first wait has a
        // deadline. Playtest twenty-five's log shows this loop spinning for
        // minutes (2000 Utility.wait calls, five a second) after Shadowbond's
        // swap moved the player away from a container it had just activated:
        // the menu never opened, and the server kept the player as the
        // object's occupant (MpObjectReference::occupant) until a destroy.
        // On giving up the second activation is still sent, which the server
        // takes without the reach check, so the occupant is released.
        const kOpenDeadlineMs = 15000;
        const openDeadline = Date.now() + kOpenDeadlineMs;
        let opened = true;
        logTrace(this, "onOpenContainerMesage - waiting for", factName, "to be true");
        while (!functionChecker()) {
          if (Date.now() > openDeadline) {
            opened = false;
            break;
          }
          await Utility.wait(0.1);
        }

        if (opened) {
          logTrace(this, "onOpenContainerMesage - waiting for", factName, "to be false");
          while (functionChecker()) await Utility.wait(0.1);
          logTrace(this, "onOpenContainerMesage - menu closed", factName);
        } else {
          logTrace(this, "onOpenContainerMesage - gave up waiting for", factName, "after", kOpenDeadlineMs, "ms; releasing the occupant");
        }

        const message: ActivateMessage = {
          t: messages.MsgType.Activate,
          data: {
            caster: 0x14, target: event.message.target, isSecondActivation: true
          }
        };

        logTrace(this, "onOpenContainerMesage - waiting", delaySeconds, "seconds before sending ActivateMessage");

        Utility.waitMenuMode(delaySeconds).then(() => {
          this.controller.emitter.emit("sendMessage", {
            message: message,
            reliability: "reliable"
          });

          logTrace(this, "onOpenContainerMesage - sent ActivateMessage", message);
        });
      })();
    });
  }

  private onTeleportMessage(event: ConnectionMessage<TeleportMessage> | ConnectionMessage<TeleportMessage2>): void {
    const msg = event.message;
    once('update', () => {
      const id = ("idx" in msg && typeof msg.idx === "number") ? this.getIdManager().getId(msg.idx) : this.getMyActorIndex();
      const refr = id === this.getMyActorIndex() ? Game.getPlayer() : getObjectReference(id);
      logTrace(this,
        `Teleporting id`, id, `refrId`, refr?.getFormID().toString(16), `...`,
        msg.pos,
        'cell/world is',
        msg.worldOrCell.toString(16),
      );
      const ragdollService = this.controller.lookupListener(RagdollService);

      const refrId = refr?.getFormID();

      const removeRagdollCallback = () => {
        TESModPlatform.moveRefrToPosition(
          ObjectReference.from(Game.getFormEx(refrId || 0)),
          Cell.from(Game.getFormEx(msg.worldOrCell)),
          WorldSpace.from(Game.getFormEx(msg.worldOrCell)),
          msg.pos[0],
          msg.pos[1],
          msg.pos[2],
          degreesToRadians(msg.rot[0]),
          degreesToRadians(msg.rot[1]),
          degreesToRadians(msg.rot[2]),
        );
      };
      const actor = Actor.from(refr);
      if (actor /*&& actor.getFormID() === 0x14*/) {
        ragdollService.safeRemoveRagdollFromWorld(actor, removeRagdollCallback);
      } else {
        removeRagdollCallback();
      }
    });
  }

  private onCreateActorMessage(event: ConnectionMessage<CreateActorMessage>): void {
    const msg = event.message;
    if (this.skipFormViewCreation(msg)) {
      const refrId = msg.refrId!;
      this.onceLoad(refrId, (refr: ObjectReference) => {
        if (refr) {
          ObjectReferenceEx.dealWithRef(refr, refr.getBaseObject() as Form);
          if (msg.props) {
            if (msg.props.inventory) {
              ModelApplyUtils.applyModelInventory(refr, msg.props.inventory);
            }
            ModelApplyUtils.applyModelIsOpen(refr, !!msg.props['isOpen']);
            ModelApplyUtils.applyModelIsHarvested(
              refr,
              !!msg.props['isHarvested'],
            );

            ModelApplyUtils.applyModelNodeScale(refr, msg.props.setNodeScale);

            ModelApplyUtils.applyModelNodeTextureSet(refr, msg.props.setNodeTextureSet);

            ModelApplyUtils.applyModelIsDisabled(refr, !!msg.props['disabled']);

            // TODO: move to a separate module
            const animation = msg.props.lastAnimation;
            if (typeof animation === "string") {
              const refrid = refr.getFormID();

              (async () => {
                for (let i = 0; i < 5; i++) {
                  // retry. pillars in bleakfalls are not reliable for some reason
                  let res2 = ObjectReference.from(Game.getFormEx(refrid))?.playAnimation(animation);
                  if (res2) {
                    break;
                  }
                  await Utility.wait(2);
                }
              })();
            }


            let displayName = msg.props.displayName;

            // keep in sync with spSnippetService.ts
            if (typeof displayName === "string") {

              const replaceValue = refr.getBaseObject()?.getName();

              if (replaceValue !== undefined) {
                displayName = displayName.replace(/%original_name%/g, replaceValue);
              } else {
                logError(this, "Couldn't get a replaceValue for SetDisplayName, refr.getFormID() was", refr.getFormID().toString(16));
              }

              refr.setDisplayName(displayName, true);
              logTrace(this, `calling setDisplayName`, displayName, `for`, refr.getFormID().toString(16));
            }
          }
        } else {
          logError(this, 'Failed to apply model to', refrId.toString(16));
        }
      });
      return;
    }

    logTrace(this, "Create actor");

    const i = this.getIdManager().allocateIdFor(msg.idx);
    if (this.worldModel.forms.length <= i) {
      this.worldModel.forms.length = i + 1;
    }

    let movement: Movement | undefined = undefined;
    // an actor: a 0xff copy or figure, or an actor from the game files
    // (its own id, baseRecordType NPC_; thuum docs/verbs/npc-puppet.md)
    if (msg.refrId !== undefined && (msg.refrId >= 0xff000000 || msg.baseRecordType === 'NPC_')) {
      movement = {
        pos: msg.transform.pos,
        rot: msg.transform.rot,
        worldOrCell: msg.transform.worldOrCell,
        runMode: 'Standing',
        direction: 0,
        isInJumpState: false,
        isSneaking: false,
        isBlocking: false,
        isWeapDrawn: false,
        isDead: false,
        healthPercentage: 1.0,
        speed: 0,
      };
    }

    const form: FormModel = {
      idx: msg.idx,
      movement,
      numMovementChanges: 0,
      numAppearanceChanges: 0,
      baseId: msg.baseId,
      refrId: msg.refrId,
      isMyClone: msg.isMe,
    };
    this.worldModel.forms[i] = form;

    if (msg.appearance) {
      form.appearance = msg.appearance;
    }

    if (msg.equipment) {
      form.equipment = msg.equipment;
    }

    if (msg.isDead) {
      form.isDead = msg.isDead;
    }

    if (msg.animation) {
      form.animation = msg.animation;
    }

    if (msg.props) {
      for (const propName in msg.props) {
        (form as Record<string, unknown>)[propName] = msg.props[propName as keyof CreateActorMessageAdditionalProps];
      }
    }

    msg.customPropsJsonDumps.forEach(element => {
      let parsed: unknown;
      try {
        parsed = JSON.parse(element.propValueJsonDump);
      } catch (e) {
        if (e instanceof SyntaxError) {
          logError(this, "createActor", msg.refrId?.toString(16), "failed to parse custom prop", element.propName, element.propValueJsonDump, e.message);
        } else {
          throw e;
        }
      }
      (form as Record<string, unknown>)[element.propName] = parsed;
    });

    if (msg.isMe) {
      this.worldModel.playerCharacterFormIdx = i;
      this.worldModel.playerCharacterRefrId = msg.refrId || 0;
    }

    // TODO: move to a separate module

    if (msg.props && !msg.props.isHostedByOther) {
    }

    if (msg.props && msg.props.isRaceMenuOpen && msg.isMe) {
      this.onSetRaceMenuOpenMessage({ message: { t: MsgType.SetRaceMenuOpen, open: true } });
    }

    const numSetInventory = this.numSetInventory;

    const applyPcInv = () => {
      if (msg.equipment) {
        applyEquipment(Game.getPlayer()!, msg.equipment)
      }

      if (numSetInventory !== this.numSetInventory) {
        logTrace(this, 'Skipping inventory apply due to newer setInventory message');
        return;
      }

      if (msg.props && msg.props.inventory) {
        this.onSetInventoryMessage({
          message: {
            t: MsgType.SetInventory,
            inventory: msg.props.inventory
          }
        });
      }
    };

    if (msg.isMe && msg.props && msg.props.learnedSpells) {
      const learnedSpells = msg.props.learnedSpells;

      // thuum docs/PLAN.md Train-30's first row: the server's list applied
      // as a diff (sync/spell.ts), with a second pass a second later for
      // whatever did not stick; the wipe and re-add it replaces lost every
      // listed spell on a reconnect with the game process up (playtest
      // twenty-seven, 2026-10-20)
      once('update', () => {
        Utility.wait(1).then(() => {
          const player = Game.getPlayer();

          if (player) {
            const first = syncSpells(player, learnedSpells);
            logTrace(this,
              `player learnedSpells:`, JSON.stringify(learnedSpells), 'first pass', JSON.stringify(first),
            );
            Utility.wait(1).then(() => {
              const again = Game.getPlayer();
              if (again) {
                const second = syncSpells(again, learnedSpells);
                if (second.added > 0 || second.missing.length > 0) {
                  logTrace(this, `player learnedSpells: second pass`, JSON.stringify(second));
                }
              }
            });
          }
        });
      });
    }

    if (msg.isMe) {
      if (msg.props?.isDead) {
        once("update", () => {
          this.controller.emitter.emit("applyDeathStateEvent", {
            actor: Game.getPlayer()!,
            isDead: true
          });
        });
      }
    }

    if (msg.isMe) {
      const spawnTask = { running: false };
      once('update', () => {
        // Use MoveRefrToPosition to spawn if possible (not in main menu)
        // In case of connection lost this is essential
        if (!spawnTask.running) {
          spawnTask.running = true;
          logTrace(this, 'Using moveRefrToPosition to spawn player');
          (async () => {
            while (true) {
              logTrace(this, 'Spawning...');
              TESModPlatform.moveRefrToPosition(
                Game.getPlayer(),
                Cell.from(Game.getFormEx(msg.transform.worldOrCell)),
                WorldSpace.from(Game.getFormEx(msg.transform.worldOrCell)),
                msg.transform.pos[0],
                msg.transform.pos[1],
                msg.transform.pos[2],
                degreesToRadians(msg.transform.rot[0]),
                degreesToRadians(msg.transform.rot[1]),
                degreesToRadians(msg.transform.rot[2]),
              );
              await Utility.wait(1);
              const pl = Game.getPlayer();
              if (!pl) {
                break;
              }
              const pos = [
                pl.getPositionX(),
                pl.getPositionY(),
                pl.getPositionZ(),
              ];
              const sqr = (x: number) => x * x;
              const distance = Math.sqrt(
                sqr(pos[0] - msg.transform.pos[0]) +
                sqr(pos[1] - msg.transform.pos[1]),
              );
              if (distance < 256) {
                break;
              }
            }
          })();
          // Unfortunatelly it requires two calls to work
          Utility.wait(1).then(applyPcInv);
          Utility.wait(1.3).then(applyPcInv);
          // Note: appearance part was copy-pasted
          if (msg.appearance) {
            applyAppearanceToPlayer(msg.appearance);
          }
        }

        if (msg.props) {
          const baseActorValues = new Map<string, unknown>([
            ['healRate', msg.props.healRate],
            ['healRateMult', msg.props.healRateMult],
            ['health', msg.props.health],
            ['magickaRate', msg.props.magickaRate],
            ['magickaRateMult', msg.props.magickaRateMult],
            ['magicka', msg.props.magicka],
            ['staminaRate', msg.props.staminaRate],
            ['staminaRateMult', msg.props.staminaRateMult],
            ['stamina', msg.props.stamina],
            ['healthPercentage', msg.props.healthPercentage],
            ['staminaPercentage', msg.props.staminaPercentage],
            ['magickaPercentage', msg.props.magickaPercentage],
          ]);

          const player = Game.getPlayer();
          if (player) {
            baseActorValues.forEach((value, key) => {
              if (typeof value === 'number') {
                if (key.includes('Percentage')) {
                  const subKey = key.replace('Percentage', '');
                  const subValue = baseActorValues.get(subKey);
                  if (typeof subValue === 'number') {
                    setActorValuePercentage(player, subKey, value);
                  }
                } else {
                  player.setActorValue(key, value);
                }
              }
            });
          }
        }
      });
      once('tick', () => {
        once('tick', () => {
          if (!spawnTask.running) {
            spawnTask.running = true;

            let loadOrder = new Array<string>();
            for (let i = 0; i < this.sp.Game.getModCount(); ++i) {
              loadOrder.push(this.sp.Game.getModName(i));
            }

            logTrace(this, `loading game in world/cell`, msg.transform.worldOrCell.toString(16));
            const loadGameService = this.controller.lookupListener(LoadGameService);
            loadGameService.loadGame(
              msg.transform.pos,
              msg.transform.rot,
              msg.transform.worldOrCell,
              msg.appearance
                ? {
                  name: msg.appearance.name,
                  raceId: msg.appearance.raceId,

                  // TODO: In types, isFemale is under face, but in the reality SP expects it here. Fix required.
                  // @ts-expect-error
                  isFemale: msg.appearance.isFemale,

                  face: {
                    hairColor: msg.appearance.hairColor,
                    bodySkinColor: msg.appearance.skinColor,
                    headTextureSetId: msg.appearance.headTextureSetId,
                    headPartIds: msg.appearance.headpartIds,
                    presets: msg.appearance.presets
                  },
                }
                : undefined,
              loadOrder,
              this.controller.lookupListener(TimeService).getLoginTime()
            );
            once('update', () => {
              applyPcInv();
              Utility.wait(0.3).then(applyPcInv);
              // Note: appearance part was copy-pasted
              if (msg.appearance) {
                applyAppearanceToPlayer(msg.appearance);
              }
            });
          }
        });
      });
    }
  }

  private onDestroyActorMessage(event: ConnectionMessage<DestroyActorMessage>): void {
    const msg = event.message;

    const i = this.getIdManager().getId(msg.idx);
    this.worldModel.forms[i] = undefined;
    getViewFromStorage()?.syncFormArray(this.worldModel);

    // Shrink to fit
    while (1) {
      const length = this.worldModel.forms.length;
      if (!length) {
        break;
      }
      if (this.worldModel.forms[length - 1]) {
        break;
      }
      this.worldModel.forms.length = length - 1;
    }

    if (this.worldModel.playerCharacterFormIdx === i) {
      this.worldModel.playerCharacterFormIdx = -1;
      this.worldModel.playerCharacterRefrId = 0;

      // TODO: move to a separate module
      once('update', () => Game.quitToMainMenu());
    }

    this.getIdManager().freeIdFor(msg.idx);
  }

  private onUpdateMovementMessage(event: ConnectionMessage<UpdateMovementMessage>): void {
    const msg = event.message;

    const i = this.getIdManager().getId(msg.idx);

    const form = this.worldModel.forms[i];

    if (form === undefined) {
      logError(this, `onUpdateMovementMessage - Form with idx`, msg.idx, `not found`);
      return;
    }

    form.movement = msg.data;
    // thuum docs/verbs/cast-teleport.md (the teleport snap): the server
    // vouched for this report as a jump, so the view places the figure
    const placedUntil = this.placedIdx.get(msg.idx);
    if (placedUntil !== undefined) {
      this.placedIdx.delete(msg.idx);
      if (Date.now() <= placedUntil) {
        form.movement.placed = true;
      }
    }
    if (!form.numMovementChanges) {
      form.numMovementChanges = 0;
    }
    form.numMovementChanges++;
  }

  // the server's word that the next report of this actor is a jump; kept a
  // second, long past the report that follows it on the same connection
  private readonly placedIdx = new Map<number, number>();
  private onActorJumpMessage(event: ConnectionMessage<ActorJumpMessage>): void {
    this.placedIdx.set(event.message.idx, Date.now() + 1000);
  }

  private onUpdateAnimationMessage(event: ConnectionMessage<UpdateAnimationMessage>): void {
    const msg = event.message;

    const i = this.getIdManager().getId(msg.idx);

    const form = this.worldModel.forms[i];

    if (form === undefined) {
      logError(this, `onUpdateAnimationMessage - Form with idx`, msg.idx, `not found`);
      return;
    }

    form.animation = msg.data;
    // thuum docs/verbs/spell-cast.md: kept in order for the view, which
    // plays every one (the caster's own graph takes a concentration cast's
    // start and release in one frame); at most the sender's own burst
    const queue = form.animationQueue || (form.animationQueue = []);
    queue.push(msg.data);
    if (queue.length > maxQueuedAnimations) {
      queue.shift();
    }
  }

  private onUpdateAppearanceMessage(event: ConnectionMessage<UpdateAppearanceMessage>): void {
    const msg = event.message;

    const i = this.getIdManager().getId(msg.idx);

    const form = this.worldModel.forms[i];

    if (form === undefined) {
      logError(this, `onUpdateAppearanceMessage - Form with idx`, msg.idx, `not found`);
      return;
    }

    form.appearance = msg.data || undefined;
    if (!form.numAppearanceChanges) {
      form.numAppearanceChanges = 0;
    }
    form.numAppearanceChanges++;

    const newAppearance = msg.data;

    if (i === this.getMyActorIndex() && newAppearance) {
      this.controller.once("update", () => {
        applyAppearanceToPlayer(newAppearance);
        logTrace(this, "Applied appearance to the player");
      });
    }
  }

  private onUpdateEquipmentMessage(event: ConnectionMessage<UpdateEquipmentMessage>): void {
    const msg = event.message;

    const i = this.getIdManager().getId(msg.idx);

    const form = this.worldModel.forms[i];

    if (form === undefined) {
      logError(this, `onUpdateEquipmentMessage - Form with idx`, msg.idx, `not found`);
      return;
    }

    form.equipment = msg.data;
  }

  private onUpdatePropertyMessage(event: ConnectionMessage<UpdatePropertyMessage>): void {
    const msg = event.message;
    const msgData = this.extractUpdatePropertyMessageData(msg);

    if (this.skipFormViewCreation(msg)) {
      const refrId = msg.refrId;
      once('update', () => {
        const refr = ObjectReference.from(Game.getFormEx(refrId));
        if (!refr) {
          logError(this, 'UpdateProperty: refr not found');
          return;
        }
        if (msg.propName === 'inventory') {
          ModelApplyUtils.applyModelInventory(refr, msgData as Inventory);
        } else if (msg.propName === 'isOpen') {
          ModelApplyUtils.applyModelIsOpen(refr, !!msgData);
        } else if (msg.propName === 'isHarvested') {
          ModelApplyUtils.applyModelIsHarvested(refr, !!msgData);
        } else if (msg.propName === 'disabled') {
          ModelApplyUtils.applyModelIsDisabled(refr, !!msgData);
        }
      });
      return;
    }
    const i = this.getIdManager().getId(msg.idx);
    const form = this.worldModel.forms[i];
    (form as Record<string, unknown>)[msg.propName] = msgData;
  }

  private onDeathStateContainerMessage(event: ConnectionMessage<DeathStateContainerMessage>): void {
    const msg = event.message;

    logTrace(this, `Received death state:`, JSON.stringify(msg.tIsDead));

    const id = this.getIdManager().getId(msg.tIsDead.idx);
    const form = this.worldModel.forms[id];

    if (form === undefined) {
      logError(this, `onDeathStateContainerMessage - Form with idx`, msg.tIsDead.idx, `not found`);
      return;
    }

    if (msg.tIsDead.propName !== nameof<FormModel>('isDead')) {
      logError(this, `onDeathStateContainerMessage - Invalid propName`, msg.tIsDead.propName);
      return;
    }

    const msgData = this.extractUpdatePropertyMessageData(msg.tIsDead);
    if (typeof msgData !== 'boolean') {
      logError(this, `onDeathStateContainerMessage - Invalid data`, msgData);
      return;
    }

    if (msg.tChangeValues) {
      this.onChangeValuesMessage({ message: msg.tChangeValues });
    }
    once('update', () => this.onUpdatePropertyMessage({ message: msg.tIsDead }));

    if (msg.tTeleport) {
      this.onTeleportMessage({ message: msg.tTeleport });
    }

    once('update', () => {
      const actor =
        id === this.getWorldModel().playerCharacterFormIdx
          ? Game.getPlayer()!
          : Actor.from(Game.getFormEx(remoteIdToLocalId(form.refrId ?? 0)));
      if (actor) {
        try {
          this.controller.emitter.emit("applyDeathStateEvent", {
            actor: actor,
            isDead: msgData
          });
        } catch (e) {
          if (e instanceof RespawnNeededError) {
            // disabled first, deleted once the engine has let it go (the
            // crash of b-cell-host 20261021-192835; formView.destroy)
            const id = actor.getFormID();
            actor.disable(false).then(() => {
              const again = Actor.from(Game.getFormEx(id));
              if (again && !again.isDeleted()) {
                again.delete();
              }
            });
          } else {
            throw e;
          }
        }
      }
    });
  }

  private handleConnectionAccepted(): void {
    this.worldModel.forms = [];
    this.worldModel.playerCharacterFormIdx = -1;
    this.worldModel.playerCharacterRefrId = 0;

    logTrace(this, "Handle connection accepted");
  }

  private onChangeValuesMessage(event: ConnectionMessage<ChangeValuesMessage>): void {
    const msg = event.message;

    once('update', () => {
      const id = this.getIdManager().getId(msg.idx);
      const refr = id === this.getMyActorIndex() ? Game.getPlayer() : getObjectReference(id);
      const ac = Actor.from(refr);
      if (!ac) {
        return;
      }

      const { health, stamina, magicka } = msg.data;
      if (typeof health === "number") {
        setActorValuePercentage(ac, 'health', health);
      }
      if (typeof stamina === "number") {
        setActorValuePercentage(ac, 'stamina', stamina);
      }
      if (typeof magicka === "number") {
        setActorValuePercentage(ac, 'magicka', magicka);
      }
    });
  }

  private onSetRaceMenuOpenMessage(event: ConnectionMessage<SetRaceMenuOpenMessage>): void {
    const msg = event.message;

    if (msg.open) {
      // wait 0.3s cause we can see visual bugs when teleporting
      // and showing this menu at the same time in onConnect
      once('update', () =>
        Utility.wait(0.3).then(() => {
          unequipIronHelmet();
          Game.showRaceMenu();
        }),
      );
    } else {
      // TODO: Implement closeMenu in SkyrimPlatform
    }
  }

  /** Packet handlers end **/

  getWorldModel(): WorldModel {
    return this.worldModel;
  }

  getMyActorIndex(): number {
    return this.worldModel.playerCharacterFormIdx;
  }

  getMyRemoteRefrId(): number {
    return this.worldModel.playerCharacterRefrId;
  }

  getIdManager() {
    return this.idManager_;
  }

  private get worldModel(): WorldModel {
    if (typeof storage["worldModel"] === "function") {
      storage["worldModel"] = { forms: [], playerCharacterFormIdx: -1, playerCharacterRefrId: 0 };
    }
    return storage["worldModel"] as WorldModel;
  }

  private get idManager_(): IdManager {
    if (typeof storage["idManager"] === "function") {
      // Note: full IdManager object preserved across hot-reloads, including methods.
      storage["idManager"] = new IdManager();
    }
    return storage["idManager"] as IdManager;
  }

  private onceLoad(
    refrId: number,
    callback: (refr: ObjectReference) => void,
    maxAttempts: number = 120,
  ) {
    once('update', () => {
      const refr = ObjectReference.from(Game.getFormEx(refrId));
      if (refr) {
        callback(refr);
      } else {
        maxAttempts--;
        if (maxAttempts > 0) {
          once('update', () => this.onceLoad(refrId, callback, maxAttempts));
        } else {
          logError(this, 'Failed to load object reference ' + refrId.toString(16));
        }
      }
    });
  };

  private skipFormViewCreation(
    msg: UpdatePropertyMessage | CreateActorMessage,
  ) {
    // Optimization added in #1186, however it doesn't work for doors for some reason.
    // thuum docs/verbs/npc-puppet.md: an actor from the game files (NPC_) gets a
    // form model and a view bound to the engine's own reference, as a copy did
    return msg.refrId && msg.refrId < 0xff000000 && msg.baseRecordType !== 'DOOR' && msg.baseRecordType !== 'NPC_';
  };

  private extractUpdatePropertyMessageData(updatePropertyMessage: UpdatePropertyMessage) {
    let msgData: unknown = updatePropertyMessage.data;

    if (updatePropertyMessage.dataDump !== undefined) {
      try {
        msgData = JSON.parse(updatePropertyMessage.dataDump);
      } catch (e) {
        if (e instanceof SyntaxError) {
          logError(this, 'extractUpdatePropertyMessageData - Failed to parse dataDump', updatePropertyMessage.dataDump);
          return;
        } else {
          throw e;
        }
      }
    }

    return msgData;
  }

  // thuum docs/verbs/cast-action.md: an action the caster's actor performed,
  // queued for the view in order; at most the sender's own burst
  private onUpdateActionMessage(event: ConnectionMessage<UpdateActionMessage>): void {
    const msg = event.message;

    const i = this.getIdManager().getId(msg.idx);

    const form = this.worldModel.forms[i];

    if (form === undefined) {
      logError(this, `onUpdateActionMessage - Form with idx`, msg.idx, `not found`);
      return;
    }

    const queue = form.actionQueue || (form.actionQueue = []);
    queue.push(msg);
    if (queue.length > maxQueuedActions) {
      queue.shift();
    }
  }

  private onSpellCastMessage(event: ConnectionMessage<SpellCastMessage>): void {
    const msg = event.message;

    once('update', () => {
      // thuum docs/verbs/shadowbond.md: a cast whose caster is this game's own
      // player is the server's imposition on it (a bond's concealment proc,
      // cast through the player's instant caster), the sibling of the
      // target rule below; no game ever relays a player's cast back to it,
      // so the id can only be the server's
      const casterIsMe = msg.data.caster !== 0 && msg.data.caster === this.worldModel.playerCharacterRefrId;
      const ac = Actor.from(Game.getFormEx(casterIsMe ? 0x14 : remoteIdToLocalId(msg.data.caster)));
      if (!ac) {
        return;
      }

      // a cast the server built carries no animation variables (it holds
      // none); castSpellImmediate applies the set before it casts and casts
      // nothing when the set does not apply, so the actor's own current set
      // stands in, a write-back of what the graph already holds
      const carried = msg.data.actorAnimationVariables;
      const empty = carried.booleans.length === 0 && carried.floats.length === 0 && carried.integers.length === 0;
      const actorAnimationVariables: ActorAnimationVariables = empty
        ? getAnimationVariablesFromActor(ac.getFormID())
        : {
          booleans: new Uint8Array(carried.booleans),
          floats: new Uint8Array(carried.floats),
          integers: new Uint8Array(carried.integers)
        };

      if (msg.data.interruptCast) {
        interruptCast(ac.getFormID(), msg.data.castingSource, actorAnimationVariables);
        figureAimHeading.delete(ac.getFormID());
        return;
      }

      // thuum docs/verbs/spell-cast.md: the spell the server relayed (it
      // relays only a cast of a spell the caster has equipped), not what
      // this game's figure holds by now, which its equipment's own message
      // sets and can lag the cast. A target-location cast that carries its
      // caster's launch is launched at the point the caster's game picked,
      // with that launch's angles (castSpellImmediate's last six)
      const spell = Spell.from(Game.getFormEx(msg.data.spell));
      if (spell) {
        // thuum docs/verbs/spell-cast.md (the class pass's target-actor
        // spells, x-class-target 20261018-012152): a relayed cast at this
        // game's own player names it by its server id, which
        // remoteIdToLocalId made 0 (no figure stands for the player here),
        // so the figure's target-actor spell landed on the figure itself
        // (Azure Reconstruction healed it) or on nothing (Shadowbond). A
        // spell delivered to its target lands on the player, as a figure's
        // spell does (its Health, Magicka and Stamina numbers refused there, the
        // server's instead: push.md); an aimed one keeps no target, so its
        // projectile follows the caster's relayed aim and hits only where
        // it flies
        // thuum docs/verbs/spell-cast.md: the caster's aim heading, held on
        // the figure while its hand's cast runs (sync/figureAim.ts)
        if (msg.data.castingSource === 0 || msg.data.castingSource === 1) {
          figureAimHeading.set(ac.getFormID(), msg.data.aimHeading);
        }
        const atPlayer = msg.data.target !== 0 && msg.data.target === this.worldModel.playerCharacterRefrId;
        const target = atPlayer && landsOnItsTarget(spell)
          ? 0x14
          : remoteIdToLocalId(msg.data.target);
        castSpellImmediate(ac.getFormID(), msg.data.castingSource, spell.getFormID(), target,
          msg.data.aimAngle, msg.data.aimHeading, actorAnimationVariables,
          msg.data.hasLaunch, msg.data.originX, msg.data.originY, msg.data.originZ, msg.data.launchAngleX, msg.data.launchAngleZ);
        // thuum docs/research/apocalypse/target-location.md (x-class-location
        // 20261018-013501: nothing of Thrumming Stone, Lightning Strike or
        // the rune on the other screen): a target-location spell whose
        // area effect has no projectile is placed as an explosion at the
        // point the caster's game picked, which rides the cast's launch;
        // castSpellImmediate launches nothing for it
        if (msg.data.hasLaunch && RemoteServer.placesAnExplosion(spell)) {
          placeAreaEffect(ac.getFormID(), spell.getFormID(), msg.data.castingSource,
            msg.data.originX, msg.data.originY, msg.data.originZ, msg.data.isDualCasting);
        }
      }
    });
  }

  // A spell with a target-location effect that has an area and no
  // projectile: the engine places its explosion at the picked point
  // (MagicSystem::Delivery::kTargetLocation 4, CommonLibSSE-NG
  // include/RE/M/MagicSystem.h:45)
  private static placesAnExplosion(spell: Spell): boolean {
    const n = spell.getNumEffects();
    for (let i = 0; i < n; ++i) {
      const effect = spell.getNthEffectMagicEffect(i);
      if (effect && effect.getDeliveryType() === 4 && !RemoteServer.hasProjectile(effect, spell.getFormID()) && spell.getNthEffectArea(i) > 0) {
        return true;
      }
    }
    return false;
  }

  // thuum docs/verbs/spell-cast.md (playtest twenty-four's c2.log: every
  // relayed rune and Lightning Strike cast ended this callback with "Cannot
  // read properties of undefined (reading 'prototype')"): Skyrim Platform
  // wraps the projectile it returns by its VM class name, PROJECTILE, which
  // its class list may not carry, so getProjectile throws for a spell that
  // HAS one. The throw came after the figure's launch and placed nothing
  // wrong, but it was the callback's end and a false lead in every read. A
  // throw counts as a projectile, as magicSyncService's sender-side guard
  // counts it; logged once per spell
  private static readonly projectileThrows = new Set<number>();
  private static hasProjectile(effect: MagicEffect, spellId: number): boolean {
    try {
      return !!effect.getProjectile();
    } catch (err) {
      if (!RemoteServer.projectileThrows.has(spellId)) {
        RemoteServer.projectileThrows.add(spellId);
        logTrace(`remoteServer: placesAnExplosion: getProjectile threw for spell ${spellId.toString(16)}: ${String(err)}`);
      }
      return true;
    }
  }

  private onUpdateAnimVariablesMessage(event: ConnectionMessage<UpdateAnimVariablesMessage>): void {
    const msg = event.message;

    once('update', () => {
      const ac = Actor.from(Game.getFormEx(remoteIdToLocalId(msg.data.actorRemoteId)));
      if (!ac) {
        return;
      }

      const actorAnimationVariables: ActorAnimationVariables = {
        booleans: new Uint8Array(msg.data.actorAnimationVariables.booleans),
        floats: new Uint8Array(msg.data.actorAnimationVariables.floats),
        integers: new Uint8Array(msg.data.actorAnimationVariables.integers)
      };

      // thuum docs/verbs/cast-action.md: on a figure the casting bits are the
      // relayed actions' (the want variables rise with a start and fall with
      // an exit, the rest is the graph's own); the caster's 500 ms snapshot
      // keeps its aim and the rest, and leaves those seven as the figure
      // holds them, so it neither raises a want the graph would then not see
      // rise nor drops one mid-cast
      if (ac.getFormID() !== 0x14) {
        const own = getAnimationVariablesFromActor(ac.getFormID());
        const ownBooleans = new Uint8Array(own.booleans);
        const snapshot = actorAnimationVariables.booleans;
        if (ownBooleans.length === snapshot.length) {
          for (const i of figureCastingBooleanIndexes) {
            if (i < snapshot.length) {
              snapshot[i] = ownBooleans[i];
            }
          }
        }
      }

      const isApplyed = applyAnimationVariablesToActor(ac.getFormID(), actorAnimationVariables);

      if (!isApplyed) {
        logError(this, 'Failed apply AnimationVariables to actor with id: ' + ac.getFormID().toString(16));
      }
    });
  }

  private numSetInventory = 0;
}
