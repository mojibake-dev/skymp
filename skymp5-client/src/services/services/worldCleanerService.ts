import { ClientListener, CombinedController, Sp } from "./clientListener";
import { NiPoint3 } from "../../sync/movement";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";
import { Actor } from "skyrimPlatform";
import { logLab, logTrace } from "../../logging";

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
    this.takeHeld();
    this.processOneActor();
  }

  // thuum tracker #106 (x-npc-summon-v3 20261117-070001): the poll's pick is
  // held holdMs and taken in a later update only if every skip below still
  // passes then (the same base under its id, so a recycled id is no take).
  // Skyrim Platform's JsTick sends "update" before it runs its update tasks
  // (SkyrimPlatform.cpp:428-434; "update" is UpdateImpl's last step, :148),
  // and summonStart is an update task (EventHandler.cpp:153-176), so in the
  // first update after the engine placed a summon this poll ran before
  // SummonService's guard (summonService.ts:147) had counted it, and the
  // chain's then() reads no protection, so a guard landing in the same tick
  // did not stop the delete. c2's Potent Flame Atronach ff000de0 started at
  // 23:02:42.897, a disable ended at .925 and a delete at .997 (c2.log:713-716),
  // and its report 3 updates on found it gone (c2-console.log:2177). The
  // run names no target: HYPOTHESIS that it was this poll's, and that
  // isCommandedActor (below) read false in the summon's first update
  // (unread). The take's line below names the target from here on
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
    if (this.held.has(actorId) || this.held.size >= WorldCleanerService.heldMax || this.leaves(actor)) {
      return;
    }
    const base = actor.getBaseObject();
    this.held.set(actorId, { at: Date.now(), baseId: base ? base.getFormID() : 0 });
  }

  // the picks held holdMs, each read again by its id and taken if no skip
  // leaves it; a pick is never taken in the update that picked it (onUpdate
  // takes before it picks)
  private takeHeld() {
    if (this.held.size === 0) {
      return;
    }
    const now = Date.now();
    const due: number[] = [];
    this.held.forEach((h, id) => {
      if (now - h.at >= WorldCleanerService.holdMs) {
        due.push(id);
      }
    });
    for (const id of due) {
      const h = this.held.get(id);
      this.held.delete(id);
      if (!h) {
        continue;
      }
      const actor = this.sp.Actor.from(this.sp.Game.getFormEx(id));
      if (!actor) {
        continue;
      }
      const base = actor.getBaseObject();
      if ((base ? base.getFormID() : 0) !== h.baseId || this.leaves(actor)) {
        continue;
      }
      this.take(actor, h.baseId, now - h.at);
    }
  }

  // whether the cleaner leaves this actor alone (every skip of the poll)
  private leaves(actor: Actor): boolean {
    const actorId = actor.getFormID();

    const currentProtection = this.protection.get(actorId) || 0;
    if (currentProtection > 0) {
      return true;
    }

    if (actorId === 0x14 || actor.isDisabled() || actor.isDeleted()) {
      return true;
    }

    // thuum docs/verbs/npc-puppet.md (ADR-045): the engine's own placed
    // references stay; a puppet or the host's own actor is the engine's
    // reference itself (upstream deleted them to make room for copies). The
    // dead-body and chicken-anomaly branches of old were for them.
    if (actorId < 0xff000000) {
      return true;
    }

    // thuum docs/verbs/summon.md (tracker #84; lab/.cache/drafts/summons/
    // design.md step 2): an actor the engine holds commanded (this game's
    // own summon, or a hosted NPC's: Actor::IsCommandedActor, CommonLibSSE-NG
    // include/RE/A/Actor.h:586; the platform's mirror skyrimPlatform.ts:2386)
    // is never the cleaner's. Without this skip (worldCleanerService.ts:72-83)
    // the poll below deleted the caster's own atronach before the server
    // could ever hear of it (x-summon-baseline 20261116-083744: the engine's
    // 0xff0008fd disabled and deleted by 12 s, and no server word for it),
    // so a summon can never wait for its bind or its word
    if (actor.isCommandedActor()) {
      return true;
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
    return this.cleaning.has(actorId);
  }

  private take(actor: Actor, baseId: number, heldMs: number) {
    const actorId = actor.getFormID();
    const inDialogue = this.isActorInDialogue(actor);
    // the lab's line for the take (tracker #106): the target, its base, and
    // the guards it passed, before the engine is touched
    logLab("world-cleaner", `takes ${actorId.toString(16)} (base ${baseId.toString(16)}): protection ${this.getWcProtection(actorId)}, commanded ${actor.isCommandedActor()}, held ${heldMs} ms; ${inDialogue ? "in dialogue, moved and disabled" : "disable, then delete"}`);

    if (inDialogue) {
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
  // the poll's picks waiting out holdMs (tracker #106): when, and the base
  // the id named then
  private held = new Map<number, { at: number; baseId: number }>();
  // the hold: longer than the one update a summon stood unguarded, and than
  // any frames the engine holds a summon before its effect names it (unread)
  private static readonly holdMs = 1000;
  // the hold's bound (one pick an update at most); a pick past it waits for
  // a later pick
  private static readonly heldMax = 64;
  private initialPos?: NiPoint3;
  private initialCellOrWorld?: number;
}
