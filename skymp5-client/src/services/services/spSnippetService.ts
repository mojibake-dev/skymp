import { MsgType } from "../../messages";
import { ConnectionMessage } from "../events/connectionMessage";
import { FinishSpSnippetMessage } from "../messages/finishSpSnippetMessage";
import { SpSnippetMessage } from "../messages/spSnippetMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { LookKind, ObjectAnimationService } from "./objectAnimationService";
import { ObjectReferenceEx } from "../../extensions/objectReferenceEx";

// TODO: refactor worldViewMisc into service
import { gameFileIdOfLongId, remoteIdToLocalId, snippetIdToLocalId } from '../../view/worldViewMisc';
import { NATIVE_GRAPH_VAR_KINDS, NATIVE_LOOK_ONLY_FNS, NATIVE_LOOK_RELAYS } from "../../lib/nativeRows.generated";
import { logError, logLab, logTrace } from "../../logging";
import { WorldView } from "../../view/worldView";

// D-C3 A (PENDING ELI): the graph variable setters a relayed script call
// carries, by ScriptVar's folded kinds (3 int, 4 float, 5 bool): the
// generated table's Keep::Variable rows (thuum ADR-073 step 4's S4,
// nativeRows.generated.ts)
const GRAPH_VAR_KINDS: { readonly [fn: string]: number } = NATIVE_GRAPH_VAR_KINDS;

// thuum docs/DECISIONS.md D-W2 A (docs/research/fifty-fixes-2026-11-07.md
// 7.2(c)): the look rows as a relay names them (class.function in lower
// case): the six action rows (PlayAnimationAndWait comes as its twin
// PlayAnimation), the two motion rows and Sound.Play. Sound.Play and a static
// row (selfId 0) name their subject in their first argument; every look but
// Sound.Play's carries its subject's seq. D-W2's detection row,
// CreateDetectionEvent during a firing, comes to the cell's host alone
// (playDetection); its seq is not asked for. The generated table's look
// column (thuum ADR-073 step 4's S4, nativeRows.generated.ts), never a list
// of names here
const LOOK_ROWS = new Set<string>(NATIVE_LOOK_RELAYS);
// m4-train-50's D-W2 review, note 3 (rule 5): the rows relayed with a seq only
// as a look, so a seq-carrying one whose subject is no trap family trigger
// here is dropped; and the detection's sound level at most SoundTrap's (100)
const LOOK_ONLY_FNS = new Set<string>(NATIVE_LOOK_ONLY_FNS);
const DETECTION_LEVEL_MAX = 100;
// thuum train 52, 4.2: the line of a long id run on (or passed as) its
// game-file reference, the first three a minute for each long id, so a
// login's serve of many actors' standings crowds out no one actor's relay;
// the table starts over past its cap
const GAME_FILE_LINES = 3;
const GAME_FILE_LINE_WINDOW_MS = 60_000;
const GAME_FILE_SAID_MAX = 256;
// thuum #52 (the review of the client's 3D keep, eaa6ba47, finding 2): the
// server names a look row's keep on its relay (SpSnippet's `keep`, wire-rules
// natives LookKeep::code), so the 3D keep reads the row's own word and holds
// no row table: the kind it keeps by each code and whether it is a Stop. A
// relay without one keeps nothing and runs as it always has
export const LOOK_KEEP_CODES: Record<number, { kind: LookKind; stop: boolean }> = {
  1: { kind: "shader", stop: false },
  2: { kind: "shader", stop: true },
  3: { kind: "effect", stop: false },
  4: { kind: "effect", stop: true },
  5: { kind: "eyes", stop: false },
};
// a snippet argument's form id, 0 for None or an argument that is no object
const formIdOf = (arg: unknown): number => {
  const formId = arg !== null && typeof arg === "object" ? (arg as { formId?: unknown }).formId : undefined;
  return typeof formId === "number" && Number.isInteger(formId) && formId > 0 ? formId : 0;
};

export class SpSnippetService extends ClientListener {
  constructor(private sp: Sp, private controller: CombinedController) {
    super();
    this.controller.emitter.on("spSnippetMessage", (e) => this.onSpSnippetMessage(e));
    // the server's sequences are this connection's: a login, a reconnect or a
    // restarted server starts them over (HostService clears its epochs at the
    // same message)
    this.controller.emitter.on("createActorMessage", (e) => {
      if (e.message.isMe) {
        this.lastSeq.clear();
      }
    });
    this.spAny = sp as Record<string, any>;
    this.controller.on("update", () => this.flushHeld());
  }

  // thuum ADR-073 decision 13, the spell channel (#54): a relayed call (a
  // snippet with a seq) on a server copy this seat's view has not drawn yet,
  // or naming one in an argument, waits for it: another seat's created
  // reference reaches this seat with its first Enable (CreateActor), and the
  // script's next call (Raise Wall's rise, 0.1 s later) can land before the
  // view's placement. Each subject's snippets wait in their arrival order, at
  // most HOLD_MS, then run as they would have, the first a window logged
  private static readonly holdMs = 3000;
  private readonly held = new Map<number, { msg: SpSnippetMessage; arrived: number }[]>();

  // the server form a relayed snippet waits for, 0 for none: its self, else
  // an object argument, at 0xff000000 and up that no view holds yet
  private static unseenServerForm(msg: SpSnippetMessage): number {
    if (typeof msg.seq !== "number") {
      return 0;
    }
    if (msg.selfId >= 0xff000000 && snippetIdToLocalId(msg.selfId) === 0) {
      return msg.selfId;
    }
    for (const arg of msg.arguments) {
      const formId = arg !== null && typeof arg === "object" ? (arg as { formId?: number }).formId : undefined;
      if (typeof formId === "number" && formId >= 0xff000000 && snippetIdToLocalId(formId) === 0) {
        return formId;
      }
    }
    return 0;
  }

  private flushHeld(): void {
    if (this.held.size === 0) {
      return;
    }
    const now = Date.now();
    const subjects: number[] = [];
    this.held.forEach((_, subject) => subjects.push(subject));
    for (const subject of subjects) {
      const queue = this.held.get(subject) ?? [];
      const drawn = snippetIdToLocalId(subject) !== 0;
      const expired = queue.length > 0 && now - queue[0].arrived > SpSnippetService.holdMs;
      if (!drawn && !expired) {
        continue;
      }
      this.held.delete(subject);
      logLab("verb:spell-placed", `${queue.length} relayed call(s) on ${subject.toString(16)} ${drawn ? "run once its copy was drawn" : `run unheld after ${SpSnippetService.holdMs} ms, no copy drawn`}`);
      let chain: Promise<unknown> = Promise.resolve();
      for (const { msg, arrived } of queue) {
        chain = chain.then(() => this.apply(msg, arrived)).catch((e) => {
          logError(this, 'SpSnippet ' + msg.class + ' ' + msg.function + ' failed ' + e);
        });
      }
    }
  }

  private onSpSnippetMessage(event: ConnectionMessage<SpSnippetMessage>): void {
    const msg = event.message;
    const inOrder = this.takeSeq(msg);
    // a kept look's seconds count from here (ObjectAnimationService's 3D keep)
    const arrived = Date.now();

    this.controller.once('update', async () => {
      if (inOrder) {
        const waitFor = SpSnippetService.unseenServerForm(msg);
        const subject = waitFor !== 0 ? waitFor : msg.selfId;
        const queue = this.held.get(subject);
        if (waitFor !== 0 || queue !== undefined) {
          if (queue !== undefined) {
            queue.push({ msg, arrived });
          } else {
            this.held.set(subject, [{ msg, arrived }]);
          }
          return;
        }
      }
      (inOrder ? this.apply(msg, arrived) : Promise.resolve(undefined))
        .then((res) => {
          const isNoResultSnippet = msg.snippetIdx === 0xffffffff;
          if (isNoResultSnippet) {
            return;
          }

          if (res === undefined) {
            res = null;
          }

          if (res !== null
            && typeof res !== "number"
            && typeof res !== "string"
            && typeof res !== "boolean") {
            logError(this, `Unsupported SpSnippet result type '${typeof res}'`)
            return;
          }

          const message: FinishSpSnippetMessage = res === null ? {
            t: MsgType.FinishSpSnippet,
            snippetIdx: msg.snippetIdx
          }
            : {
              t: MsgType.FinishSpSnippet,
              returnValue: res,
              snippetIdx: msg.snippetIdx,
            }

          this.controller.emitter.emit("sendMessage", {
            message: message,
            reliability: "reliable"
          });
        })
        .catch((e) => {
          logError(this, 'SpSnippet ' + msg.class + ' ' + msg.function + ' failed ' + e);
        });
    });
  }

  // thuum docs/DECISIONS.md ADR-068 decision 4 (rung 2, the one seam): a
  // snippet with a seq is a script call the subject's owner made, relayed by
  // the server (ScriptCall 75 mirrored as SpSnippet 30). Its PlayAnimation on
  // a game-file reference (a self below 0xff000000; the server's own copies
  // and its actors' long ids sit above) plays through ObjectAnimationService
  // as a relayed ObjectAnimation does, with its mark and its "object
  // animation <refr> <event> played" line (b-puzzle-relay's claim). At the
  // merge with the pillar heads (843fed95 through 73ed0c52, not yet on canon)
  // that play is their playRelayed(refr, event, "relayed", remoteRefr): the
  // side jump first and the "played (relayed, ...)" line, with nothing to
  // change here. The play carries the call's seq, the kept last animation's
  // (rung 3: ObjectAnimationService plays it again at each 3D load on a seat
  // that does not host the cell). D-C3 A (PENDING ELI;
  // docs/research/fifty-fixes-2026-11-07.md 1(d)): its SetAnimationVariableBool,
  // Int or Float on a game-file reference, with a name and a value of the
  // setter's type, goes through ObjectAnimationService.setVariable the same
  // way (run, then kept with the call's seq, so a seat that does not host the
  // cell sets it again at each 3D load). thuum 3df63969's actor look rows
  // with a seq (the reanimate-fx memo's 6.4 D6 B): EffectShader.Play and
  // Stop, VisualEffect.Play and Stop and Actor.SetEyeTexture go through
  // ObjectAnimationService.look, which plays one on a 3D here, holds one for
  // its 3D's load otherwise, keeps the last Play per (subject, shader or
  // effect) and the last eye texture, and plays them again at each later 3D
  // load of a subject this seat does not own; one it does not take, and the
  // other three rows, run as before (relayedLook). Every other snippet runs as
  // it always has
  private apply(snippet: SpSnippetMessage, arrived: number): Promise<unknown> {
    const event = snippet.arguments[0];
    const look = this.lookSubject(snippet);
    // nothing else checks that a look's subject is a trigger: a modified guest
    // within reach of any held world object could move, push or alarm it on
    // every other seat through these rows (note 3)
    if (look === undefined && typeof snippet.seq === "number" && LOOK_ONLY_FNS.has(snippet.function.toLowerCase())) {
      logLab("verb:traps", `trigger look ${snippet.selfId.toString(16)} ${snippet.class}.${snippet.function} dropped: no trap trigger here (seq ${snippet.seq}) (D-W2)`);
      return Promise.resolve(undefined);
    }
    if (look !== undefined && snippet.function.toLowerCase() === "createdetectionevent") {
      this.playDetection(snippet, look);
      return Promise.resolve(undefined);
    }
    if (typeof snippet.seq === "number"
      && snippet.class.toLowerCase() === "objectreference"
      && snippet.function.toLowerCase() === "playanimation"
      && snippet.selfId > 0 && snippet.selfId < 0xff000000
      && typeof event === "string") {
      this.controller.lookupListener(ObjectAnimationService).play(snippet.selfId, event, snippet.seq);
      if (look !== undefined) {
        this.sayLook(snippet, look);
      }
      return Promise.resolve(undefined);
    }
    const kind = GRAPH_VAR_KINDS[snippet.function.toLowerCase()];
    const value = snippet.arguments[1];
    if (typeof snippet.seq === "number" && kind !== undefined
      && snippet.class.toLowerCase() === "objectreference"
      && snippet.selfId > 0 && snippet.selfId < 0xff000000
      && typeof event === "string" && event !== ""
      && (kind === 5 ? typeof value === "boolean" : typeof value === "number" && (kind === 3 ? Number.isInteger(value) : Number.isFinite(value)))) {
      this.controller.lookupListener(ObjectAnimationService).setVariable(snippet.selfId, event, kind, value, snippet.seq);
      if (look !== undefined) {
        this.sayLook(snippet, look);
      }
      return Promise.resolve(undefined);
    }
    if (look === undefined && typeof snippet.seq === "number" && this.relayedLook(snippet, snippet.seq, arrived)) {
      return Promise.resolve(undefined);
    }
    if (look === undefined) {
      return this.run(snippet);
    }
    return this.run(snippet).then((res) => {
      this.sayLook(snippet, look);
      return res;
    });
  }

  // an actor look row (its relay's keep code, LOOK_KEEP_CODES) read off its snippet for
  // ObjectAnimationService.look, true when that took it. An Arg0Method row
  // (EffectShader, VisualEffect) names its subject in its first argument and
  // its form in its self; SetEyeTexture is the subject's own method, its self
  // the actor (a game-file actor the server holds by its long id,
  // MakeLongFormId) and its one argument the TextureSet. Rule 5: the server
  // checked that the reporter simulates the subject (3df63969's validator);
  // what the arguments hold is read here, and a subject or an effect form
  // that is no object, or a Play's duration that is no finite number, runs as
  // it always has (said as falling through). The update's context
  private relayedLook(snippet: SpSnippetMessage, seq: number, arrived: number): boolean {
    const objectAnimation = this.controller.lookupListener(ObjectAnimationService);
    const name = `${snippet.class}.${snippet.function}`;
    const rule = typeof snippet.keep === "number" ? LOOK_KEEP_CODES[snippet.keep] : undefined;
    if (rule === undefined) {
      return false;
    }
    const eyes = rule.kind === "eyes";
    const subject = eyes ? (snippet.selfId > 0 ? snippet.selfId : 0) : formIdOf(snippet.arguments[0]);
    const form = eyes ? formIdOf(snippet.arguments[0]) : (snippet.selfId > 0 ? snippet.selfId : 0);
    const duration = snippet.arguments[1];
    const timed = !eyes && !rule.stop;
    if (subject === 0 || (!eyes && form === 0) || (timed && (typeof duration !== "number" || !Number.isFinite(duration)))) {
      objectAnimation.lookFallsThrough(name, subject || snippet.selfId, "its arguments unread");
      return false;
    }
    return objectAnimation.look({
      row: name,
      kind: rule.kind,
      stop: rule.stop,
      subject,
      form,
      seconds: timed ? (duration as number) : -1,
      facing: rule.kind === "effect" && !rule.stop ? formIdOf(snippet.arguments[2]) : 0,
      seq,
      arrived,
    });
  }

  // D-W2 A: a relayed look's subject (the firing seat's trigger, by its server
  // id), undefined for any other snippet: a look row (LOOK_ROWS) with its seq
  // (Sound.Play's comes without one) whose subject is a trap family trigger
  // here (getTrapRole's family and trigger). The watcher plays it as any
  // snippet, under the platform's mark: PlayAnimation through
  // ObjectAnimationService.play (never kept: keepsFor leaves a family
  // reference out), the graph variables through setVariable, the motion rows
  // and Sound.Play through runMethod. The seam counts that play "(platform)"
  // and batches none, so it is never reported again, never reaches
  // PropService and is never a prop claim (the 48 rule: a trap's trigger is
  // never a trap body). A game read: the update's context
  private lookSubject(snippet: SpSnippetMessage): number | undefined {
    const row = `${snippet.class.toLowerCase()}.${snippet.function.toLowerCase()}`;
    if (!LOOK_ROWS.has(row)) {
      return undefined;
    }
    const sound = row === "sound.play";
    if (!sound && row !== "objectreference.createdetectionevent" && typeof snippet.seq !== "number") {
      return undefined;
    }
    const first = snippet.arguments[0];
    const subject = sound || !snippet.selfId
      ? (first !== null && typeof first === "object" && typeof first.formId === "number" ? first.formId : 0)
      : snippet.selfId;
    if (!subject || subject >= 0xff000000) {
      return undefined;
    }
    const role = ObjectReferenceEx.trapRole(remoteIdToLocalId(subject));
    return role !== null && role.family && role.trigger === true ? subject : undefined;
  }

  // D-W2's detection row on the cell's host (the server relays it here alone,
  // akOwner the reporter's actor by its server id): the trigger script's
  // ObjectReference.CreateDetectionEvent run on this seat's copy of the
  // trigger with akOwner the presser's figure here, its local reference, so
  // a guest's bone alarm wakes the NPCs this seat simulates. A TypeScript
  // call, under the platform's mark: the seam counts it "(platform)" and
  // never reports it again. The update's context
  private playDetection(snippet: SpSnippetMessage, subject: number): void {
    const head = `trigger look ${subject.toString(16)} ObjectReference.CreateDetectionEvent`;
    const ownerArg = snippet.arguments[0];
    const level = snippet.arguments[1];
    const ownerId = ownerArg !== null && typeof ownerArg === "object" && typeof ownerArg.formId === "number" ? ownerArg.formId : 0;
    const trigger = this.sp.ObjectReference.from(this.sp.Game.getFormEx(remoteIdToLocalId(subject)));
    const owner = ownerId !== 0 ? this.sp.Actor.from(this.sp.Game.getFormEx(remoteIdToLocalId(ownerId))) : null;
    if (!trigger || !owner || typeof level !== "number" || !Number.isInteger(level) || level < 0 || level > DETECTION_LEVEL_MAX) {
      logLab("verb:traps", `${head} skipped, ${!trigger ? "no reference" : !owner ? `no owner ${ownerId.toString(16)} here` : `sound level ${String(level).slice(0, 12)} past 0 to ${DETECTION_LEVEL_MAX}`} (D-W2)`);
      return;
    }
    trigger.createDetectionEvent(owner, level);
    logLab("verb:traps", `${head} played here (owner ${ownerId.toString(16)}, level ${level}) (D-W2)`);
  }

  // the watcher's line, once its play ran (a snippet that threw says so in
  // onSpSnippetMessage's catch instead)
  private sayLook(snippet: SpSnippetMessage, subject: number): void {
    const here = this.sp.Game.getFormEx(remoteIdToLocalId(subject)) !== null;
    const seq = typeof snippet.seq === "number" ? `seq ${snippet.seq}` : "no seq";
    logLab("verb:traps", `trigger look ${subject.toString(16)} ${snippet.class}.${snippet.function} ${here ? "played here" : "skipped, no reference"} (${seq}) (D-W2)`);
  }

  // the order of one subject's relayed script calls (ADR-068 decision 4; the
  // research's 6.11 finding 1: a record and an action for one object can land
  // in either order): the server numbers each object's relays, so one at or
  // below the last taken for its subject is stale and is dropped. The subject
  // is the self, or for a static row (selfId 0, as the server sends a static
  // native) its first argument's form, the table's subject arg0
  // (Debug.SendAnimationEvent's target). A snippet without a seq is the server
  // VM's own and always passes. JS state alone, so it runs in the messages'
  // own order, ahead of the update that applies them
  private takeSeq(snippet: SpSnippetMessage): boolean {
    const seq = snippet.seq;
    if (typeof seq !== "number") {
      return true;
    }
    const subject = SpSnippetService.subjectOf(snippet);
    if (subject === 0) {
      return true;
    }
    const last = this.lastSeq.get(subject);
    if (last !== undefined && seq <= last) {
      logTrace(this, `script call ${subject.toString(16)} ${snippet.class}.${snippet.function} seq ${seq} dropped: at or below ${last}`);
      return false;
    }
    this.lastSeq.set(subject, seq);
    return true;
  }

  // thuum docs/DECISIONS.md ADR-068 amendment A (rung 3, the script record):
  // the record's view carries the object's seq at the read, and
  // ScriptStateService replays the view where this seat's last seq for the
  // form is another (seqOf), then raises the last seq to the record's (never
  // lowers it), so a relay at or below the record's view is stale here as one
  // below the last relay is. JS state alone, any context
  seqOf(subject: number): number | undefined {
    return this.lastSeq.get(subject);
  }

  raiseSeq(subject: number, seq: number): void {
    const last = this.lastSeq.get(subject);
    if (last === undefined || seq > last) {
      this.lastSeq.set(subject, seq);
    }
  }

  private static subjectOf(snippet: SpSnippetMessage): number {
    if (snippet.selfId) {
      return snippet.selfId;
    }
    const first = snippet.arguments[0];
    return first !== null && typeof first === "object" && typeof first.formId === "number" ? first.formId : 0;
  }

  private async run(snippet: SpSnippetMessage): Promise<unknown> {
    const functionLowerCase = snippet.function.toLowerCase();
    const classLowerCase = snippet.class.toLowerCase();

    // keep in sync with remoteServer.ts
    if (classLowerCase === "objectreference") {
      if (functionLowerCase === "setdisplayname") {
        let newName = snippet.arguments[0];
        if (typeof newName === "string") {
          // the self runMethod runs on (its line is runMethod's)
          const selfId = snippetIdToLocalId(snippet.selfId);
          const self = this.sp.ObjectReference.from(this.sp.Game.getFormEx(selfId));

          const replaceValue = self?.getBaseObject()?.getName();

          if (replaceValue !== undefined) {
            newName = newName.replace(/%original_name%/g, replaceValue);
            snippet.arguments[0] = newName;
          } else {
            logError(this, "Couldn't get a replaceValue for SetDisplayName, snippet.selfId was", snippet.selfId.toString(16));
          }
        } else {
          logError(this, "Encountered SetDisplayName with non-string argument", newName);
        }
      }
    }

    if (classLowerCase === "game") {
      if (functionLowerCase === "showracemenu" || functionLowerCase === "showlimitedracemenu") {
        logTrace(this, "showracemenu called");
        const worldView = this.controller.lookupListener(WorldView);
        worldView.setFormViewUpdateAllowed(false);

        logTrace(this, "Waiting 1.0s before calling showracemenu");
        this.sp.Utility.wait(1.0).then(() => {
          this.runStatic(snippet);
          worldView.waitGameTimeAndAllowFormViewUpdate(1.0);
        });
        return;
      }
    }

    if (classLowerCase === "skymphacks") {
      if (functionLowerCase === "additem" || functionLowerCase === "removeitem") {
        const form = this.sp.Form.from(this.deserializeArg(snippet.arguments[0], snippet));
        if (form === null) {
          logError(this, "Unable to find form with id " + snippet.arguments[0].formId.toString(16));
          return;
        }

        const sign = snippet.function === "AddItem" ? "+" : "-";
        const count = snippet.arguments[1];

        let soundId = 0x334ab;
        if (form.getFormID() !== 0xf) {
          soundId = 0x14115;
        }

        const sound = this.sp.Sound.from(this.sp.Game.getFormEx(soundId));
        if (sound !== null) {
          const name = form.getName();
          if (name.trim() === "") {
            logTrace(this, "Sound will not be played because item has no name")
          } else {
            sound.play(this.sp.Game.getPlayer());
          }
        } else {
          logError(this, "Unable to find sound with id " + soundId.toString(16));
        }

        if (count <= 0) {
          logError(this, "Positive count expected, got " + count.toString());
        } else {
          const name = form.getName();
          if (name.trim() === "") {
            logTrace(this, "Notification will not be shown because item has no name")
          } else {
            this.sp.Debug.notification(sign + " " + name + " (" + count + ")");
          }
          logTrace(this, sign + " " + name + " (" + count + ")");
        }
      } else throw new Error("Unknown SkympHack - " + snippet.function);
      return;
    }
    return snippet.selfId ? this.runMethod(snippet) : this.runStatic(snippet);
  };

  private deserializeArg(arg: any, snippet: SpSnippetMessage) {
    // a None (null on the wire, SnippetArg's Option: a relayed script call's
    // None form) passes as null, never as a form to look up
    if (arg === null) {
      return null;
    }
    if (typeof arg === "object") {
      const formId = this.localIdOf(snippet, arg.formId, true);
      const form = this.sp.Game.getFormEx(formId);
      let cl = this.spAny[arg.type];
      if (!cl) {
        const matchingKey = Object.keys(this.spAny).find((key) => {
          return key.toLowerCase() === arg.type.toLowerCase();
        });
        if (matchingKey) {
          cl = this.spAny[matchingKey];
        }
      }
      if (!cl) {
        throw new Error(`deserializeArg - Class ${arg.type} not found`);
      }
      const gameObject = cl.from(form);
      return gameObject;
    }
    return arg;
  };

  private async runMethod(snippet: SpSnippetMessage): Promise<unknown> {
    const selfId = this.localIdOf(snippet, snippet.selfId, false);
    const self = this.sp.Game.getFormEx(selfId);
    if (!self)
      throw new Error(
        `Unable to find form with id ${selfId.toString(16)}`,
      );
    let cl = this.spAny[snippet.class];
    if (!cl) {
      const matchingKey = Object.keys(this.spAny).find((key) => {
        return key.toLowerCase() === snippet.class.toLowerCase();
      });
      if (matchingKey) {
        cl = this.spAny[matchingKey];
      }
    }
    if (!cl) {
      throw new Error(`runMethod - Class ${snippet.class} not found, found`);
    }

    const selfCasted = cl.from(self);
    if (!selfCasted)
      throw new Error(
        `Form ${selfId.toString(16)} is not instance of ${snippet.class}, form type is ${self.getType()}`,
      );
    const f = selfCasted[snippet.function];
    return await f.apply(
      selfCasted,
      snippet.arguments.map((arg) => this.deserializeArg(arg, snippet)),
    );
  };

  // thuum train 52, 4.2: a snippet's id as the local form id it runs on (its
  // self) or passes (an object argument), snippetIdToLocalId's answer; a long
  // id that no view holds runs on the game-file reference it names, and says
  // so, the first three a minute for each long id
  private localIdOf(snippet: SpSnippetMessage, remoteId: number, argument: boolean): number {
    const localId = snippetIdToLocalId(remoteId);
    if (localId !== 0 && localId === gameFileIdOfLongId(remoteId)) {
      this.sayGameFileReference(snippet, remoteId, localId, argument);
    }
    return localId;
  }

  private sayGameFileReference(snippet: SpSnippetMessage, longId: number, localId: number, argument: boolean): void {
    const now = Date.now();
    const said = this.gameFileSaid.get(longId);
    if (said !== undefined && now - said.since < GAME_FILE_LINE_WINDOW_MS) {
      if (said.count >= GAME_FILE_LINES) {
        return;
      }
      said.count++;
    } else {
      if (this.gameFileSaid.size >= GAME_FILE_SAID_MAX) {
        this.gameFileSaid.clear();
      }
      this.gameFileSaid.set(longId, { since: now, count: 1 });
    }
    const head = `snippet ${snippet.class}.${snippet.function}`;
    const view = `no view holds its long id ${longId.toString(16)}`;
    logLab("verb:scripts", argument
      ? `${head} with ${localId.toString(16)}: ${view}, passed the game-file reference`
      : `${head} on ${localId.toString(16)}: ${view}, run on the game-file reference`);
  }

  // A global Skyrim Platform does not bind (its own TESModPlatform natives are
  // not in the dump its bindings come from) is called by name through
  // callNative, which needs the .psc declaration and the update context
  // snippets run in (thuum docs/verbs/stagger.md: TESModPlatform.Stagger,
  // b-stagger 20261011-084217 found the snippet failing without it)
  private async runStatic(snippet: SpSnippetMessage): Promise<unknown> {
    const papyrusClass = this.spAny[snippet.class];
    const args = snippet.arguments.map((arg) => this.deserializeArg(arg, snippet));
    const f = papyrusClass ? papyrusClass[snippet.function] : undefined;
    if (typeof f === "function") {
      return await f.apply(papyrusClass, args);
    }
    return this.sp.callNative(snippet.class, snippet.function, undefined, ...args);
  };

  private spAny: Record<string, any>;
  // the last seq taken per subject (takeSeq), this connection's
  private readonly lastSeq = new Map<number, number>();
  // 4.2's line per long id: its window's start and the lines said in it
  private readonly gameFileSaid = new Map<number, { since: number; count: number }>();
};
