import { ObjectReference, Game, Actor, MotionType } from "skyrimPlatform";
import { Appearance, applyTints } from "../sync/appearance";
import { NiPoint3 } from "../sync/movement";
import { ObjectReferenceEx } from "../extensions/objectReferenceEx";
import { logTrace } from "../logging";

export class SpawnProcess {
  // isCurrent: whether the view still holds this reference (FormView's spawn
  // generation); once it does not, the chain stops before its next step, so
  // a copy its view's destroy() is disabling is never enabled or resurrected
  // again (HYPOTHESIS: the stray duplicate a superseded chain left)
  constructor(
    appearance: Appearance | null,
    pos: NiPoint3,
    refrId: number,
    private callback: () => void,
    private isCurrent: () => boolean,
  ) {
    const refr = ObjectReference.from(Game.getFormEx(refrId));
    if (!refr || refr.getFormID() !== refrId) {
      return;
    }

    refr.setPosition(...pos).then(() => this.enable(appearance, refrId));
  }

  private superseded(refrId: number, step: string): boolean {
    if (this.isCurrent()) {
      return false;
    }
    logTrace("SpawnProcess", `spawn of ${refrId.toString(16)} stopped before ${step}: its view let it go`);
    return true;
  }

  private enable(appearance: Appearance | null, refrId: number) {
    const refr = ObjectReference.from(Game.getFormEx(refrId));
    if (!refr || refr.getFormID() !== refrId) {
      return;
    }
    if (this.superseded(refrId, "applyTints and enable")) {
      return;
    }

    const ac = Actor.from(refr);
    if (ac && appearance) {
      applyTints(ac, appearance);
    }
    refr.enable(false).then(() => this.resurrect(refrId));
  }

  private resurrect(refrId: number) {
    const refr = ObjectReference.from(Game.getFormEx(refrId));
    if (!refr || refr.getFormID() !== refrId) {
      return;
    }
    if (this.superseded(refrId, "resurrect")) {
      return;
    }

    const ac = Actor.from(refr);
    if (ac) {
      return ac.resurrect().then(() => {
        this.callback();
      });
    }

    ObjectReferenceEx.dealWithRef(refr, refr.getBaseObject()!);

    return refr.setMotionType(MotionType.Keyframed, true).then(this.callback);
  }
}
