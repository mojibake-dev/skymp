import { MsgType } from "../../messages";
import { ConnectionMessage } from "../events/connectionMessage";
import { FinishSpSnippetMessage } from "../messages/finishSpSnippetMessage";
import { SpSnippetMessage } from "../messages/spSnippetMessage";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { ObjectAnimationService } from "./objectAnimationService";

// TODO: refactor worldViewMisc into service
import { remoteIdToLocalId } from '../../view/worldViewMisc';
import { logError, logTrace } from "../../logging";
import { WorldView } from "../../view/worldView";

// D-C3 A (PENDING ELI): the graph variable setters a relayed script call
// carries, by ScriptVar's folded kinds (3 int, 4 float, 5 bool)
const GRAPH_VAR_KINDS: Record<string, number> = {
  setanimationvariableint: 3,
  setanimationvariablefloat: 4,
  setanimationvariablebool: 5,
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
  }

  private onSpSnippetMessage(event: ConnectionMessage<SpSnippetMessage>): void {
    const msg = event.message;
    const inOrder = this.takeSeq(msg);

    this.controller.once('update', async () => {
      (inOrder ? this.apply(msg) : Promise.resolve(undefined))
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
  // cell sets it again at each 3D load). Every other snippet runs as it
  // always has
  private apply(snippet: SpSnippetMessage): Promise<unknown> {
    const event = snippet.arguments[0];
    if (typeof snippet.seq === "number"
      && snippet.class.toLowerCase() === "objectreference"
      && snippet.function.toLowerCase() === "playanimation"
      && snippet.selfId > 0 && snippet.selfId < 0xff000000
      && typeof event === "string") {
      this.controller.lookupListener(ObjectAnimationService).play(snippet.selfId, event, snippet.seq);
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
      return Promise.resolve(undefined);
    }
    return this.run(snippet);
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
          const selfId = remoteIdToLocalId(snippet.selfId);
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
        const form = this.sp.Form.from(this.deserializeArg(snippet.arguments[0]));
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

  private deserializeArg(arg: any) {
    // a None (null on the wire, SnippetArg's Option: a relayed script call's
    // None form) passes as null, never as a form to look up
    if (arg === null) {
      return null;
    }
    if (typeof arg === "object") {
      const formId = remoteIdToLocalId(arg.formId);
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
    const selfId = remoteIdToLocalId(snippet.selfId);
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
      snippet.arguments.map((arg) => this.deserializeArg(arg)),
    );
  };

  // A global Skyrim Platform does not bind (its own TESModPlatform natives are
  // not in the dump its bindings come from) is called by name through
  // callNative, which needs the .psc declaration and the update context
  // snippets run in (thuum docs/verbs/stagger.md: TESModPlatform.Stagger,
  // b-stagger 20261011-084217 found the snippet failing without it)
  private async runStatic(snippet: SpSnippetMessage): Promise<unknown> {
    const papyrusClass = this.spAny[snippet.class];
    const args = snippet.arguments.map((arg) => this.deserializeArg(arg));
    const f = papyrusClass ? papyrusClass[snippet.function] : undefined;
    if (typeof f === "function") {
      return await f.apply(papyrusClass, args);
    }
    return this.sp.callNative(snippet.class, snippet.function, undefined, ...args);
  };

  private spAny: Record<string, any>;
  // the last seq taken per subject (takeSeq), this connection's
  private readonly lastSeq = new Map<number, number>();
};
