import { ObjectReference, SendPapyrusEventHook } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { MsgType } from '../../messages';
import { logTrace } from '../../logging';

// thuum docs/verbs/word-wall.md (M4): a word wall's word is the server's.
// The engine sends OnTrigger to the wall's trigger reference twice every
// half second for as long as the player stands in its primitive (the read
// x-word-wall-read 20261101-131832: 86 lines in 10 s and no OnTriggerEnter,
// since a seat that spawns or loads inside the ~4,000-unit box never sees an
// entry); this service hears that through Skyrim Platform's Papyrus event
// hook (filtered in native to game-file references and that exact event,
// rule 9; HookPattern.cpp: a pattern without '*' is exact), keeps the
// reference for the next update (a hook handler runs outside the Papyrus
// VM's context, where natives are refused), checks there that the
// reference's base is a word wall trigger, and reports the wall once per
// session as WordWall (R1). The server reads the wall's word from its own
// record, checks the reach and teaches it into the record (R0), telling
// this game as the console's TeachWord does; a repeat teaches nothing there.
// The wall's own scripts (the chant, the glow, the camera) stay behind the
// Papyrus block until the layer admits them by name (word-wall.md, the
// chant's call).
//
// A guest's own walk in: the presser rule (D-C2 A, activationService.ts)
// tells this game's own entry into a world trigger of a cell another game
// hosts as the platform's triggerEntry, which this service takes too. OnTrigger
// alone missed it: in b-word-wall 20261110-062732 the guest's engine sent its
// trigger an enter and a leave 338 ms apart (the leave 12 ms after a
// setPosition, the player inside the 2,065-unit sphere throughout) and no
// OnTrigger. The host hears no triggerEntry for its own cell's trigger and a
// game that logs in inside hears no entry; both keep OnTrigger.
//
// Lines for the lab: "word wall <id> entered", "word wall <id> reported".
export class WordWallService extends ClientListener {
  constructor(
    private sp: Sp,
    private controller: CombinedController,
  ) {
    super();
    const self = this;
    // Skyrim Platform's Handler takes enter AND leave (Handler.cpp wraps both
    // unconditionally): without leave the add throws "Invalid argument" at the
    // plugin's start and the seat never logs in (the traps stage of
    // 2026-11-01, numidium's read; the lab driver met the same in 0844cbef).
    // The published 2.8.0 typing declares enter alone, so the handler is a
    // variable, not a literal (no excess-property check); the fork's own
    // Definitions.txt declares both
    const handler = {
      enter(ctx: SendPapyrusEventHook.Context) {
        self.pending.add(ctx.selfId);
      },
      leave() {},
    };
    this.sp.hooks.sendPapyrusEvent.add(
      handler,
      1,
      WordWallService.lastGameFileId,
      'OnTrigger',
    );
    // the presser rule's entry (a tick task, JS state alone here): this game's
    // own player stepping into a world trigger; the base is checked on the
    // update as for OnTrigger. A platform without the event keeps OnTrigger
    try {
      this.controller.on('triggerEntry', (e) => {
        if (e.enter === true && e.presserId === 0x14 && typeof e.refrId === 'number') {
          self.pending.add(e.refrId);
        }
      });
    } catch (e) {
      logTrace(this, `triggerEntry unavailable on this platform (${e}); OnTrigger alone`);
    }
    this.controller.on('update', () => this.onUpdate());
  }

  private onUpdate() {
    if (this.pending.size === 0) {
      return;
    }
    const entered: number[] = [];
    this.pending.forEach((refrId) => entered.push(refrId));
    this.pending.clear();
    for (const refrId of entered) {
      if (this.reported.has(refrId)) {
        continue;
      }
      const refr = ObjectReference.from(this.sp.Game.getFormEx(refrId));
      const base = refr?.getBaseObject();
      if (!base || !WordWallService.triggerBases.has(base.getFormID())) {
        continue;
      }
      logTrace(this, `word wall ${refrId.toString(16)} entered`);
      this.reported.add(refrId);
      this.controller.emitter.emit('sendMessage', {
        message: { t: MsgType.WordWall, wall: refrId },
        reliability: 'reliable',
      });
      logTrace(this, `word wall ${refrId.toString(16)} reported`);
    }
  }

  // the trigger references the hook noted, read on the next update
  private readonly pending = new Set<number>();
  // the walls this game reported this session (the server's record is the
  // truth; a repeat teaches nothing there either)
  private readonly reported = new Set<number>();
  // the highest game-file form id: copies the server makes sit above it and
  // carry no trigger scripts
  private static readonly lastGameFileId = 0xfeffffff;
  // the trigger ACTI every word wall's trigger reference uses in Skyrim.esm
  // (WordWallTrigger 0x5095e; lab/esm.py cellrefs on Bleak Falls Sanctum,
  // 2026-10-31); the server judges the reference itself, this is the filter
  private static readonly triggerBases = new Set<number>([0x5095e]);
}
