import { ClientListener, CombinedController, Sp } from "./clientListener";
import { NiPoint3 } from "../../sync/movement";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { Actor } from "skyrimPlatform";
import { logTrace } from "../../logging";

export class WorldCleanerService extends ClientListener {
  constructor(private sp: Sp, private controller: CombinedController) {
    super();
    this.controller.on("update", () => this.onUpdate());
    this.controller.emitter.on("gameLoad", () => this.onGameLoad());
  }

  modWcProtection(actorId: number, mod: number): void {
    const currentProtection = this.protection.get(actorId);
    this.protection.set(actorId, currentProtection ? currentProtection + mod : mod);
  }

  getWcProtection(actorId: number): number {
    return this.protection.get(actorId) || 0;
  }

  private onGameLoad() {
    let player = this.sp.Game.getPlayer();
    if (!player) {
      return;
    }

    this.initialPos = ObjectReferenceEx.getPos(player);
    this.initialCellOrWorld = ObjectReferenceEx.getWorldOrCell(player);
  }

  private onUpdate() {
    this.processOneActor();
  }

  private processOneActor() {
    const pc = this.sp.Game.getPlayer();
    if (pc === null) {
      return;
    }

    const actor = this.sp.Game.findRandomActor(
      pc.getPositionX(),
      pc.getPositionY(),
      pc.getPositionZ(),
      8192
    );
    if (actor === null) {
      return;
    }

    const actorId = actor.getFormID();

    const currentProtection = this.protection.get(actorId) || 0;
    if (currentProtection > 0) {
      return;
    }

    if (actorId === 0x14 || actor.isDisabled() || actor.isDeleted()) {
      return;
    }

    // thuum docs/verbs/npc-puppet.md (ADR-045): the engine's own placed
    // references stay; a puppet or the host's own actor is the engine's
    // reference itself (upstream deleted them to make room for copies). The
    // dead-body and chicken-anomaly branches of old were for them.
    if (actorId < 0xff000000) {
      return;
    }

    // thuum docs/verbs/cell-host.md (b-cell-host 20261021-192835, the lab's
    // first runs with npcEnabled on): findRandomActor hands out the same
    // actor on frame after frame while its latent disable below is still
    // running, and the guards above do not see a disable that has not
    // completed, so one deer got two disable-and-delete chains; the second
    // delete ran on a Character the first had freed (CrashLogger: a
    // MovementControllerNPC job on "Deer" [0x00000000], the form id read
    // from freed memory, EXCEPTION_ACCESS_VIOLATION at 41702+0x22).
    // HYPOTHESIS for the cause, MEASURED for the double pick; an actor
    // being cleaned is skipped until its chain ends either way
    if (this.cleaning.has(actorId)) {
      return;
    }

    if (this.isActorInDialogue(actor)) {
      // Deleting actor in dialogue crashes Skyrim
      // https://github.com/skyrim-multiplayer/issue-tracker/issues/13
      actor.setPosition(0, 0, 0);
      actor.disableNoWait(true); // Seems to not crash
      return;
    }

    this.cleaning.add(actorId);
    actor.disable(false).then(() => {
      this.cleaning.delete(actorId);
      const ac = this.sp.Actor.from(this.sp.Game.getFormEx(actorId));
      // a form gone or already deleted by the time the disable completes is
      // left alone: a second delete is the crash above
      if (!ac || ac.isDeleted() || this.isActorInDialogue(ac)) {
        return;
      }
      ac.delete();
    }).catch(() => {
      this.cleaning.delete(actorId);
    });
  }

  private isActorInDialogue(ac: Actor) {
    return ac.isInDialogueWithPlayer() || ac.getDialogueTarget() !== null;
  }

  private protection = new Map<number, number>();
  // the actors whose disable-and-delete chain is running (cell-host's read)
  private cleaning = new Set<number>();
  private initialPos?: NiPoint3;
  private initialCellOrWorld?: number;
}
