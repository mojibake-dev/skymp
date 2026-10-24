import { Actor, ActorBase, Cell, createText, destroyText, Form, FormType, Game, Keyword, NetImmerse, ObjectReference, once, printConsole, setTextPos, setTextSize, setTextString, settings, storage, TESModPlatform, Utility, WorldSpace, worldPointToScreenPoint } from "skyrimPlatform";
import { setDefaultAnimsDisabled, applyAnimation } from "../sync/animation";
// @ts-expect-error (TODO: Remove in 2.10.0)
import { applyAnimationVariablesToActor, castReady, performAction, performActionResult, setActivateTextHidden, setActorAimHeading, setActorAimPitch } from "skyrimPlatform";
import { figureAimHeading } from "../sync/figureAim";
import { Appearance, applyAppearance } from "../sync/appearance";
import { isBadMenuShown, applyEquipment, isFigureBase, keepOnlyServerSpells } from "../sync/equipment";
import { RespawnNeededError } from "../lib/errors";
import { logLab } from "../logging";
import { FormModel } from "./model";
import { UpdateActionMessage } from "../services/messages/updateActionMessage";
import { applyMovement } from "../sync/movementApply";
import { SpawnProcess } from "./spawnProcess";
import { ObjectReferenceEx } from "../extensions/objectReferenceEx";
import { PlayerCharacterDataHolder } from "./playerCharacterDataHolder";
import { setActorValuePercentage } from "../sync/actorvalues";
import { ModelApplyUtils } from "./modelApplyUtils";
import { localIdToRemoteId } from "./worldViewMisc";
import { SpApiInteractor } from "../services/spApiInteractor";
import { WorldCleanerService } from "../services/services/worldCleanerService";
import { GamemodeUpdateService } from "../services/services/gamemodeUpdateService";
import { HostService } from "../services/services/hostService";
import { PuppetService } from "../services/services/puppetService";

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

  // thuum docs/verbs/npc-handoff.md: the kill switch (settings
  // skymp5-client.npcHandoff, default on): off, an engine reference whose
  // record changed cell stands at the door as before train-40
  private static npcHandoffEnabled(): boolean {
    if (FormView.npcHandoff === undefined) {
      const s = settings["skymp5-client"] as Record<string, unknown> | undefined;
      FormView.npcHandoff = !s || s["npcHandoff"] !== false;
    }
    return FormView.npcHandoff;
  }
  private static npcHandoff: boolean | undefined = undefined;
  // the record cell an engine reference was last moved into (one attempt
  // per record cell; the engine's own cell says whether another is due)
  private movedInto = 0;

  // thuum docs/verbs/npc-fate.md: the fate switch (settings
  // skymp5-client.npcFate, default off until x-npc-fate-roll): on, engine
  // references are dressed by the engine from the server's roll
  private static npcFateEnabled(): boolean {
    if (FormView.npcFate === undefined) {
      const s = settings["skymp5-client"] as Record<string, unknown> | undefined;
      // on unless the settings file says false (ADR-046 decision 1 held at
      // T3: x-npc-fate-pin v3 GREEN 053914, both seats, both paths)
      FormView.npcFate = !s || s["npcFate"] !== false;
    }
    return FormView.npcFate;
  }
  private static npcFate: boolean | undefined = undefined;

  update(model: FormModel): void {
    // Other players mutate into PC clones when moving to another location
    if (model.movement) {
      if (!this.lastWorldOrCell)
        this.lastWorldOrCell = model.movement.worldOrCell;
      if (this.refrId !== 0 && this.refrId < 0xff000000) {
        // an engine reference whose record stands in another cell than this
        // engine has it (a resident through a door under its host's follow,
        // thuum docs/verbs/npc-handoff.md D6; or a view created after the
        // record went indoors: x-handoff-door 001915's second seat): the
        // reference is moved into the record's cell as the player's own
        // cross-cell move is (remoteServer's moveRefrToPosition), one attempt
        // per record cell, never destroyed; the npcHandoff switch leaves it
        // standing where the engine put it
        this.lastWorldOrCell = model.movement.worldOrCell;
        if (FormView.npcHandoffEnabled() && this.movedInto !== model.movement.worldOrCell) {
          const refr = ObjectReference.from(Game.getFormEx(this.refrId));
          const engineCell = refr ? ObjectReferenceEx.getWorldOrCell(refr) : 0;
          const target = Game.getFormEx(model.movement.worldOrCell);
          // never the actor's own simulator: the host's engine is the truth
          // and the host never hears its own reports, so its model lags the
          // engine (x-handoff-door 20261024-013234: the host's view moved
          // its own Alvor back out of the inn 133 ms after the follow)
          const mine = !!refr && SpApiInteractor.getControllerInstance().lookupListener(HostService).isMine(model, refr);
          if (refr && target && !mine && engineCell !== 0 && engineCell !== model.movement.worldOrCell) {
            const pos = model.movement.pos;
            const rot = model.movement.rot;
            TESModPlatform.moveRefrToPosition(
              refr,
              Cell.from(target),
              WorldSpace.from(target),
              pos[0], pos[1], pos[2],
              rot[0] * Math.PI / 180, rot[1] * Math.PI / 180, rot[2] * Math.PI / 180,
            );
            this.movedInto = model.movement.worldOrCell;
            logLab("verb:npc-handoff", `moved ${this.refrId.toString(16)} into ${model.movement.worldOrCell.toString(16)} (the engine had it in ${engineCell.toString(16)})`);
          }
        }
      }
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



    // Don't spawn dead actors if not already (a copy). An engine reference
    // binds dead and takes the record's death below (thuum docs/verbs/
    // npc-fate.md, the deaths: the body on a seat that was not listening at
    // the kill, and after the engine's own cell reset stood it up)
    const engineReference = !!model.refrId && model.refrId < 0xff000000;
    if (model.isDead && !engineReference) {
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
        if (this.refrId !== 0 && this.refrId < 0xff000000) {
          // an engine reference in another cell than the player's stands
          // where its record put it; nothing is applied until the player's
          // cell is its cell again (npc-handoff D6)
          return;
        }
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
          if (base && base.getType() === FormType.NPC) {
            // thuum docs/verbs/npc-puppet.md: an actor from the game files
            // binds as itself (no copy, no cleaner protection); its damage
            // is the server's number (attackDamageMult 0, as every copy's);
            // dealWithRef's blockActivation is the puppet apply's, so the
            // host's own actor keeps its engine behavior (Rule A)
            Actor.from(refr)?.setActorValue("attackDamageMult", 0);
          } else if (base) {
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

  // `lost`: the server took the form back (this game left the actor's reach,
  // by a walk or a teleport; a disconnect), as against the view rebinding
  destroy(lost = false): void {
    this.isOnScreen = false;
    this.spawnMoment = 0;
    const refrId = this.refrId;
    once("update", () => {
      if (refrId && refrId < 0xff000000 && lost) {
        // thuum docs/verbs/npc-puppet.md: an actor from the game files was
        // live here by the server's word alone, and no further word reaches
        // a game that no longer listens to it (the release of its cell comes
        // after the grace, to a game with no form for it). Its AI goes off
        // now, loaded or not: TES3MP's leaving host does the same
        // (b-npc-puppet 20261022-104032, step 29: c1's Sigrid read AI on
        // from the lab spawn after the handoff)
        const refr = ObjectReference.from(Game.getFormEx(refrId));
        if (refr && Actor.from(refr)) {
          SpApiInteractor.getControllerInstance().lookupListener(PuppetService).demoteOnLoss(refrId);
        }
      }
      if (refrId >= 0xff000000) {
        const refr = ObjectReference.from(Game.getFormEx(refrId));
        SpApiInteractor.getControllerInstance().lookupListener(WorldCleanerService).modWcProtection(refrId, -1);
        const ac = Actor.from(refr);
        if (ac) {
          TESModPlatform.setWeaponDrawnMode(ac, -1);
        }
        // thuum docs/verbs/cell-host.md: a copy this game hosts runs under
        // the engine's AI, and a delete while its movement job still runs it
        // is the crash b-cell-host 20261021-192835 took (a job thread on a
        // MovementControllerNPC for a Character whose form was gone); the
        // Creation Kit's rule for Delete is a disable first, so the copy is
        // disabled (latent: the call returns once the engine has it out of
        // its process lists) and deleted after, as the harvest path does
        // (modelApplyUtils) and the cleaner does
        if (refr) {
          refr.disable(false).then(() => {
            const again = ObjectReference.from(Game.getFormEx(refrId));
            if (again && !again.isDeleted()) {
              again.delete();
            }
          });
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
  private deadCheckAt = 0;
  private isSetNodeScaleApplied = false;

  // thuum docs/verbs/figure-values.md: the figure carries its player's
  // recorded maxima (the server's health, magicka and stamina props for an
  // observer, at spawn and on each change) as its base values, so a script
  // in this game reads Skyrim's number instead of the million that stood
  // here before. The shown fraction is kept across the set: the engine keeps
  // the current value, not the share, when a base changes
  // an NPC's model: a server-made actor or one from the game files carries
  // its base id; a player's figure and the player's own clone carry none or
  // the player base (PartOne.cpp sets baseId for a base other than 0 and 7,
  // but the message's optional defaults to 0 and the wire carries it, so a
  // player arrives as baseId 0: read on 2026-10-22 from the clean world's
  // two records)
  private static isNpcModel(model: FormModel): boolean {
    return !!model.baseId && model.baseId !== 0x7;
  }

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
    // thuum docs/verbs/npc-puppet.md (playtest 31, 2026-10-22): an actor is
    // never harvested or opened, and the harvested apply's non-flora branch
    // reads refr.isDisabled() against the model and calls enable(true) on a
    // reference the engine holds disabled; with every game-file actor bound
    // to a view that was a force-enable every 666 ms on each disabled
    // resident (a quest's duplicate, a sleeper), ten enable calls a second
    // across Riverwood, the engine disabling them back, flapping actors and
    // hitching. The server's word on an actor's disabled state comes by its
    // property message, never from here.
    const isActor = Actor.from(refr) !== null;
    if (!isActor && now - this.lastHarvestedApply > 666) {
      this.lastHarvestedApply = now;
      ModelApplyUtils.applyModelIsHarvested(refr, !!model.isHarvested);
    }
    if (!isActor && now - this.lastOpenApply > 133) {
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

    // thuum docs/verbs/npc-fate.md (the deaths): the record's death or life
    // is this seat's too. A dead record on a live engine reference is killed
    // (the bind; the engine's own cell reset standing a corpse up on a
    // re-entry, re-npc-roll.md section 9). A record the server says is alive
    // (isDead false, said explicitly: a respawn) on a dead engine reference
    // is resurrected (the respawn's container goes to the host alone, and a
    // seat that reconnects after the sweep binds a corpse: x-npc-fate-pin
    // 052150's 2,077-unit "settle" on a dead copy). A record never dead says
    // nothing (undefined) and leaves a predicted death (ADR-038) alone. Read
    // every two seconds, never per frame (natives cost).
    if (model.isDead !== undefined && this.refrId !== 0 && this.refrId !== 0x14 && this.refrId < 0xff000000 && Date.now() >= this.deadCheckAt) {
      this.deadCheckAt = Date.now() + 2000;
      const body = Actor.from(refr);
      const want = !!model.isDead;
      if (body && body.isDead() !== want) {
        SpApiInteractor.getControllerInstance().emitter.emit("applyDeathStateEvent", { actor: body, isDead: want });
      }
    }

    if (
      model.inventory &&
      PlayerCharacterDataHolder.getCrosshairRefId() == this.refrId &&
      !isBadMenuShown()
    ) {
      // Do not let actors breaking their equipment via inventory apply
      // However, actually, actors do not have inventory in their models
      // Except your clone. A dead engine reference is the server's
      // container (npc-fate.md): its inventory is the record's, applied
      // before the OpenContainer that follows it
      const asActor = Actor.from(refr);
      if (!asActor || (this.refrId < 0xff000000 && asActor.isDead())) {
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

    // thuum docs/verbs/cell-host.md: an actor this game hosts (its cell
    // granted here, the server marking it nobody else's) runs under its own
    // engine, with its default animations
    const alreadyHosted =
      SpApiInteractor.getControllerInstance().lookupListener(HostService).isMine(model, refr);
    setDefaultAnimsDisabled(this.refrId, alreadyHosted ? false : true);

    // thuum docs/verbs/npc-puppet.md (ADR-045): an NPC this game does not
    // host is a puppet (AI off, the relay's transform and events), an engine
    // reference or a server-made copy alike; one it hosts is live under the
    // engine. A player's figure is neither: its own game's reports hold it.
    const puppet = !alreadyHosted && FormView.isNpcModel(model);
    const puppetService = SpApiInteractor.getControllerInstance().lookupListener(PuppetService);
    if (puppet) {
      puppetService.ensurePuppet(refr);
    } else if (alreadyHosted) {
      puppetService.ensureLive(refr, model.movement);
    }

    if (model.movement) {
      const ac = Actor.from(refr);
      // (the re-host on a quiet figure is gone: the server elects a cell's
      // host and passes it on at its own grace, docs/verbs/cell-host.md)

      if (
        +(model.numMovementChanges as number) !==
        this.movState.lastNumChanges ||
        Date.now() - this.movState.lastApply > 2000
      ) {
        // the periodic re-apply of an unchanged record settles a displaced
        // standing figure or puppet (movementApply's deadband; the stagger
        // push left under eight units, b-duel on train-33)
        const settle = +(model.numMovementChanges as number) === this.movState.lastNumChanges;
        this.movState.lastApply = Date.now();
        if (!alreadyHosted) {
          // a figure or a puppet: the record's movement is imposed (a
          // puppet's absolutely, docs/verbs/npc-puppet.md Rule B)
          const backup = model.movement.isWeapDrawn;
          if (forcedWeapDrawn === true || forcedWeapDrawn === false) {
            model.movement.isWeapDrawn = forcedWeapDrawn;
          }
          try {
            applyMovement(refr, model.movement, !!model.isMyClone, puppet, settle);
          } catch (e) {
            if (e instanceof RespawnNeededError) {
              model.movement.isWeapDrawn = backup;
              if (this.refrId < 0xff000000) {
                // an engine reference reported from another cell stands
                // where it is until npc-handoff moves it (MoveTo); a copy
                // respawns there as before
                this.movState.lastNumChanges = +(model.numMovementChanges as number);
                return;
              }
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
          this.hostedHereApplied = false;
        } else {
          // this game hosts the actor: nothing of the record is applied, not
          // even once (docs/verbs/npc-puppet.md Rule A; the first apply of
          // old pinned the host's own copy with a keep-offset on itself);
          // the weapon-drawn override is lifted once, as the old host
          // attempt did
          if (ac && ac.is3DLoaded() && !this.hostedHereApplied) {
            this.hostedHereApplied = true;
            TESModPlatform.setWeaponDrawnMode(ac, -1);
          }
        }
      }
    }

    // thuum docs/verbs/npc-puppet.md Rule A: an actor this game hosts takes
    // no relayed event, pitch or action; its engine's are its own
    if (refr.is3DLoaded() && !alreadyHosted) {
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

    // (Rule A again: a hosted actor's equipment is its engine's)
    // thuum docs/verbs/npc-fate.md (ADR-046 decision 1): under the fate switch
    // an engine reference is dressed by the engine itself from the server's
    // roll (the outfit detour at its first dressing), the one shape for the
    // host's own actor and a puppet alike, so the apply below is for copies
    // (figures and 0xff actors) alone
    const engineDressed = FormView.npcFateEnabled() && this.refrId !== 0 && this.refrId < 0xff000000;
    if (model.equipment && !alreadyHosted && !engineDressed) {
      // thuum ADR-046, decision 2: a figure's spells stay the server's. A
      // distributor adds its abilities at the actor's load, after the
      // equipment's apply, so the figure is swept every ten seconds (a few
      // Papyrus calls per figure) beside the apply's own strip
      if (!FormView.isNpcModel(model) && this.spawnMoment > 0 && Date.now() - this.spellSweepAt > 10000) {
        this.spellSweepAt = Date.now();
        const figure = Actor.from(refr);
        if (figure && isFigureBase(figure)) {
          const removed = keepOnlyServerSpells(figure, model.equipment);
          if (removed > 0) {
            printConsole(`figure ${refr.getFormID().toString(16)}: ${removed} spell(s) a distributor added removed (ADR-046)`);
          }
        }
      }
      if (this.eqState.lastNumChanges !== model.equipment.numChanges) {
        const ac = Actor.from(refr);
        // If we do not block inventory here, we will be able to reproduce the bug:
        // 1. Place ~90 bots and force them to reequip iron swords to the left hand (rate should be ~50ms)
        // 2. Open your inventory and reequip different items fast
        // 3. After 1-2 minutes close your inventory and see that HUD disappeared
        if (ac && !ac.isDeleted() && !isBadMenuShown() &&
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
  // whether the weapon-drawn override was lifted for this actor as one this
  // game hosts (docs/verbs/cell-host.md)
  private hostedHereApplied = false;
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
  private spellSweepAt = 0;
  // the maxima set on the figure so far, by actor value name (figure-values)
  private figureMaxima = new Map<string, number>();
  private textNameId: number | undefined = undefined;


  public static isDisplayingNicknames: boolean = true;
}
