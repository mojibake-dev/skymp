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
// This game's own entry re-arms a wall (tracker #138, playtest 53): a draugr
// standing in Bleak Falls Sanctum's sphere sent its trigger an enter at each
// seat's cell load, OnTrigger reported this game's player from far off, the
// server refused it for reach (E_WORDWALL_REACH), and the once-a-session
// latch then skipped the player's real walk-in. An entry (triggerEntry, or
// the platform's stock triggerEnter for the host's own walk-in) clears the
// wall's latch and reports it on the first update at least a second later,
// so the server's record of the player's position is inside the sphere too.
// OnTrigger stays once a session, for a login or a teleport inside.
//
// Lines for the lab: "word wall <id> entered (<source>)", "word wall <id>
// reported (<source>)", the source entry, triggerEnter (the two joined by +
// when both told it) or OnTrigger.
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
          self.queueEntry(e.refrId, 'entry');
        }
      });
    } catch (e) {
      logTrace(this, `triggerEntry unavailable on this platform (${e}); OnTrigger alone`);
    }
    // the platform's stock triggerEnter (TESTriggerEnterEvent, EventHandler.cpp,
    // a tick task, so game calls are allowed): the player stepping into a word
    // wall's trigger, the host's own walk-in included, which gets no
    // triggerEntry. HYPOTHESIS until the probe (x-word-wall-npc): it fires on
    // the host, with the trigger as target and the player as cause.
    // Rule 9's cost, accepted: the client's first triggerEnter listener, so
    // subscribing switches the platform's sink on (EventManager.cpp
    // Subscribe), and every enter by any collidable into any trigger in this
    // seat's loaded cells (other seats' figures, puppets, props: the presser
    // rule's note, FridaHooks.cpp, HYPOTHESIS there) crosses into JS, a tick
    // task with two lookups and two wrappers, all but the player's own
    // dropped below. A stock event takes no native filter (no minSelfId,
    // maxSelfId or eventPattern, as a hook does); an enter comes once a
    // crossing, not twice every half second as OnTrigger (above) does
    this.controller.on('triggerEnter', (e) => {
      if (e.cause?.getFormID() !== 0x14) {
        // the HYPOTHESIS's other half, said once: the player as the event's
        // target and a wall trigger as its cause means the two fields are
        // swapped, so one probe run tells "swapped" from "never fires here"
        if (!self.swappedSaid && e.target?.getFormID() === 0x14) {
          const cause = e.cause;
          const causeBase = cause ? cause.getBaseObject() : null;
          if (cause && causeBase && WordWallService.triggerBases.has(causeBase.getFormID())) {
            self.swappedSaid = true;
            logTrace(self, `triggerEnter with the player as target and wall ${cause.getFormID().toString(16)} as cause: fields swapped`);
          }
        }
        return;
      }
      const base = e.target?.getBaseObject();
      if (base && WordWallService.triggerBases.has(base.getFormID())) {
        self.queueEntry(e.target.getFormID(), 'triggerEnter');
      }
    });
    this.controller.on('update', () => this.onUpdate());
  }

  private swappedSaid = false;

  private queueEntry(refrId: number, source: string) {
    const queued = this.entries.get(refrId);
    const sources = queued && !queued.source.includes(source) ? `${queued.source}+${source}` : source;
    this.entries.set(refrId, { at: Date.now(), source: sources });
  }

  private onUpdate() {
    if (this.pending.size === 0 && this.entries.size === 0) {
      return;
    }
    const now = Date.now();
    const entered: [number, string][] = [];
    // an entry first: its wall's latch is cleared (a report refused for reach
    // spent it) and the entry's report goes out, not a same-update OnTrigger's
    this.entries.forEach((entry, refrId) => {
      if (now - entry.at >= WordWallService.entrySettleMs) {
        this.entries.delete(refrId);
        this.reported.delete(refrId);
        entered.push([refrId, entry.source]);
      }
    });
    this.pending.forEach((refrId) => entered.push([refrId, 'OnTrigger']));
    this.pending.clear();
    for (const [refrId, source] of entered) {
      if (this.reported.has(refrId)) {
        continue;
      }
      const refr = ObjectReference.from(this.sp.Game.getFormEx(refrId));
      const base = refr?.getBaseObject();
      if (!base || !WordWallService.triggerBases.has(base.getFormID())) {
        continue;
      }
      logTrace(this, `word wall ${refrId.toString(16)} entered (${source})`);
      this.reported.add(refrId);
      this.controller.emitter.emit('sendMessage', {
        message: { t: MsgType.WordWall, wall: refrId },
        reliability: 'reliable',
      });
      logTrace(this, `word wall ${refrId.toString(16)} reported (${source})`);
    }
  }

  // the trigger references the hook noted, read on the next update
  private readonly pending = new Set<number>();
  // this game's own entries, by trigger reference: when (Date.now()) and
  // which sources told it, reported once entrySettleMs has passed
  private readonly entries = new Map<number, { at: number; source: string }>();
  // the wait after an entry before its report: the server checks the reach
  // against its own record of the player's position, a movement update behind
  // the engine's (the edge race, HYPOTHESIS until the probe)
  private static readonly entrySettleMs = 1000;
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
