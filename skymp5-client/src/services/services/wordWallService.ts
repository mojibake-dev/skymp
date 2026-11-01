import { ObjectReference } from 'skyrimPlatform';
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
// Lines for the lab: "word wall <id> entered", "word wall <id> reported".
export class WordWallService extends ClientListener {
  constructor(
    private sp: Sp,
    private controller: CombinedController,
  ) {
    super();
    const self = this;
    this.sp.hooks.sendPapyrusEvent.add(
      {
        enter(ctx) {
          self.pending.add(ctx.selfId);
        },
      },
      1,
      WordWallService.lastGameFileId,
      'OnTrigger',
    );
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
