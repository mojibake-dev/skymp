import { Flora, Form, FormType, MotionType, ObjectReference } from "skyrimPlatform";
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

  static dealWithRef(self: ObjectReference, base: Form, lock?: { level: number; key: number; locked: boolean }): void {
    const t = base.getType();
    const isItem = FormTypeEx.isItem(t);

    // BlackFallsBarrow02, door isn't opening via SetOpen so we're hacking it.
    // Not blocking activation & asking parent to activate until will be in the correct state
    // See also modelApplyUtils.ts
    const caveGSecretDoor01 = 0x6f703;

    // You can also block for t === FormType.Flora || t === FormType.Tree, but I don't think it's necessary.
    if (t === FormType.Furniture
      || t === FormType.Activator
      || t === FormType.Container
      || isItem
      || t === FormType.NPC
      || (t === FormType.Door && self.getBaseObject()?.getFormID() !== caveGSecretDoor01)) {
      self.blockActivation(true);
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
