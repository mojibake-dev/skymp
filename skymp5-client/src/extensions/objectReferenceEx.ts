import { Flora, Form, FormType, MotionType, ObjectReference, settings } from "skyrimPlatform";
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

  // the base form ids the client settings name (activationUnblockedBases: a list
  // of numbers or hex strings, or one string of hex ids split on spaces and
  // commas, the shape the lab's relaunch step can write), read once; a missing
  // or malformed key unblocks nothing
  private static activationUnblocked(baseId: number): boolean {
    if (ObjectReferenceEx.unblockedBases === undefined) {
      const ids = new Set<number>();
      try {
        const s = settings["skymp5-client"] as Record<string, unknown> | undefined;
        const raw = s?.["activationUnblockedBases"];
        const items: unknown[] = Array.isArray(raw) ? raw : typeof raw === "string" ? raw.split(/[\s,]+/) : [];
        for (const v of items) {
          const n = typeof v === "number" ? v : typeof v === "string" && v !== "" ? parseInt(v, 16) : NaN;
          if (Number.isFinite(n) && n > 0) {
            ids.add(n >>> 0);
          }
        }
      } catch (e) {
        logTrace("ObjectReferenceEx", `activationUnblockedBases unreadable: ${e}`);
      }
      ObjectReferenceEx.unblockedBases = ids;
      if (ids.size > 0) {
        logTrace("ObjectReferenceEx", `activation kept on ${ids.size} base(s): ${Array.from(ids).map((x) => x.toString(16)).join(" ")}`);
      }
    }
    return ObjectReferenceEx.unblockedBases.has(baseId >>> 0);
  }
  private static unblockedBases: Set<number> | undefined;

  static dealWithRef(self: ObjectReference, base: Form, lock?: { level: number; key: number; locked: boolean }): void {
    const t = base.getType();
    const isItem = FormTypeEx.isItem(t);

    // BlackFallsBarrow02, door isn't opening via SetOpen so we're hacking it.
    // Not blocking activation & asking parent to activate until will be in the correct state
    // See also modelApplyUtils.ts
    const caveGSecretDoor01 = 0x6f703;

    // You can also block for t === FormType.Flora || t === FormType.Tree, but I don't think it's necessary.
    // thuum docs/DECISIONS.md ADR-059 (traps, puzzles): a scripted world object
    // whose base the client settings name in activationUnblockedBases keeps the
    // engine's own activation processing, so its Papyrus script (admitted by
    // name through the layer's papyrus-scripts.txt) hears OnActivate and runs
    // the trap, the lever or the pillar on this game; the read x-bleak-traps v4
    // is its first use, the hosted-cell gate its next shape. Default: none.
    if (t === FormType.Furniture
      || t === FormType.Activator
      || t === FormType.Container
      || isItem
      || t === FormType.NPC
      || (t === FormType.Door && self.getBaseObject()?.getFormID() !== caveGSecretDoor01)) {
      self.blockActivation(!ObjectReferenceEx.activationUnblocked(base.getFormID()));
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
