import { Flora, Form, FormType, MotionType, ObjectReference } from "skyrimPlatform";
import { SpApiInteractor } from "../services/spApiInteractor";
import { HostService } from "../services/services/hostService";
import { NiPoint3 } from "../sync/movement";
import { FormTypeEx } from "./formTypeEx";
import { logTrace } from "../logging";

export class ObjectReferenceEx {
  static getWorldOrCell(self: ObjectReference): number {
    let world = self.getWorldSpace();
    if (world) {
      return world.getFormID();
    }

    let cell = self.getParentCell();
    if (cell) {
      return cell.getFormID();
    }

    return 0;
  }

  static getPos(self: ObjectReference): NiPoint3 {
    return [self.getPositionX(), self.getPositionY(), self.getPositionZ()];
  };

  static getDistance(a: NiPoint3, b: NiPoint3) {
    const deltaX = a[0] - b[0];
    const deltaY = a[1] - b[1];
    const deltaZ = a[2] - b[2];
    return Math.sqrt(deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ);
  };

  static getDistanceNoZ(a: NiPoint3, b: NiPoint3) {
    const deltaX = a[0] - b[0];
    const deltaY = a[1] - b[1];
    return Math.sqrt(deltaX * deltaX + deltaY * deltaY);
  };

  // thuum docs/DECISIONS.md ADR-062 decision 1 (Eli, playtest 41: "shouldn't
  // 'scripts work' be a holistic fix?"): a world object in a cell this game
  // HOSTS keeps the engine's own activation processing, so its script hears
  // OnActivate and its activation parents fire (the plate's spike wall, the
  // lever's portcullis); every other seat keeps the block and renders the
  // relays. The lifted kinds are the activators and the movable statics,
  // the world's scripted objects; doors, containers, furniture and items keep
  // the server's own flows (their activations travel to the server as
  // before). Read through HostService at the reference's creation and at
  // every grant and release (HostService.liftCellActivation).
  static liftsInHostedCell(t: number): boolean {
    return t === FormType.Activator || t === FormType.MovableStatic;
  }

  static hostedHere(self: ObjectReference): boolean {
    try {
      const cell = self.getParentCell();
      if (!cell) {
        return false;
      }
      return SpApiInteractor.getControllerInstance().lookupListener(HostService).hostsCell(cell.getFormID());
    } catch (e) {
      return false;
    }
  }

  static dealWithRef(self: ObjectReference, base: Form, lock?: { level: number; key: number; locked: boolean }): void {
    const t = base.getType();
    const isItem = FormTypeEx.isItem(t);

    // BlackFallsBarrow02, door isn't opening via SetOpen so we're hacking it.
    // Not blocking activation & asking parent to activate until will be in the correct state
    // See also modelApplyUtils.ts
    const caveGSecretDoor01 = 0x6f703;

    // You can also block for t === FormType.Flora || t === FormType.Tree, but I don't think it's necessary.
    // thuum docs/DECISIONS.md ADR-062 decision 1 (the flip of ADR-059's first
    // read, x-bleak-traps v4): a scripted world object keeps the engine's own
    // activation processing in a cell this game hosts and nowhere else, so its
    // Papyrus script hears OnActivate and its activation parents fire on the
    // hosting seat; no list of bases chooses (the settings list that was the
    // lab's lever is gone: rules, not rows).
    // a movable static joins the blocked kinds (ADR-062 slice 1b's review: the
    // host's release re-blocks ACTI and MSTT alike, hostService.liftCellActivation,
    // so a seat that never hosted and a former host must agree)
    if (t === FormType.Furniture
      || t === FormType.Activator
      || t === FormType.MovableStatic
      || t === FormType.Container
      || isItem
      || t === FormType.NPC
      || (t === FormType.Door && self.getBaseObject()?.getFormID() !== caveGSecretDoor01)) {
      // ADR-062: lifted in a cell this game hosts for the scripted world
      // kinds (liftsInHostedCell), blocked everywhere else; the cell's host
      // state alone chooses
      const lifted = ObjectReferenceEx.liftsInHostedCell(t) && ObjectReferenceEx.hostedHere(self);
      self.blockActivation(!lifted);
    } else {
      self.blockActivation(false);
    }

    // thuum docs/verbs/locks.md: the server's lock stands on this game's
    // reference when it sends one (the engine's own lockpicking runs at the
    // server's prompt); a reference the server sends no lock for is unlocked
    // here as before. The line is the lab's read of the lock at creation
    // (x-lockpick 20261031-134850 counted the property change's line, which
    // no creation writes); a change later logs "lock <id> applied" instead.
    if (lock) {
      self.setLockLevel(lock.level);
      self.lock(lock.locked, false);
      logTrace("ObjectReferenceEx", `lock ${self.getFormID().toString(16)} set at creation: ${lock.locked ? "locked" : "unlocked"}, level ${lock.level}, key ${lock.key.toString(16)}`);
    } else if (self.isLocked()) {
      self.lock(false, false);
    }

    if (isItem) {
      self.setMotionType(MotionType.Keyframed, false);
    }

    // https://github.com/skyrim-multiplayer/issue-tracker/issues/36
    if (t === FormType.Flora && Flora.from(base)?.getIngredient()) {
      self.setMotionType(MotionType.Keyframed, false);
    }
  }
}
