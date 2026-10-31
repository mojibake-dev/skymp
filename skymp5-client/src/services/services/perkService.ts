import { Actor, ActorValueInfo, MenuCloseEvent, Perk } from 'skyrimPlatform';
import { ClientListener, CombinedController, Sp } from './clientListener';
import { ConnectionMessage } from '../events/connectionMessage';
import { PerkStateMessage } from '../messages/perkStateMessage';
import { MsgType } from '../../messages';
import { logError, logTrace } from '../../logging';

// thuum docs/verbs/perks.md (M5): perks are the server's transactions. The
// Skills menu's pick happens on this game first (the engine spends a point it
// holds and adds the perk); this service reads the tree perks the player holds
// at the menu's close and every five seconds, and reports each one the
// server's set lacks as a PerkPick (R1). The server judges it by the perk
// record's own conditions and the points left, and answers with PerkState,
// the whole set and the points (R0): the engine's perk points are set to it
// (Game.setPerkPoints), the perks the set holds and the game lacks are added,
// and a TREE perk the game holds but the set lacks is removed (a refused pick
// undone). Only perks the trees list (ActorValueInfo.getPerks over the
// eighteen skills, all ranks) are ever removed, so a hidden, quest or mod
// perk the engine gave on its own is left alone. Before the first PerkState
// nothing is managed; a server with `perks` off never sends one.
//
// Lines for the lab: "perk pick <id> reported", "perk state: N perk(s), P
// point(s); added [...] removed [...]", "perk universe: N tree perk(s)".
interface State {
  perks: Set<number>;
  points: number;
  applied: boolean;
}

const hex = (n: number) => n.toString(16);

// the eighteen skills' actor value names (CommonLibSSE-NG ActorValues.h, 6 to
// 23), as ActorValueInfo.getActorValueInfoByName takes them
const skillNames = [
  'OneHanded',
  'TwoHanded',
  'Marksman',
  'Block',
  'Smithing',
  'HeavyArmor',
  'LightArmor',
  'Pickpocket',
  'Lockpicking',
  'Sneak',
  'Alchemy',
  'Speechcraft',
  'Alteration',
  'Conjuration',
  'Destruction',
  'Illusion',
  'Restoration',
  'Enchanting',
];

export class PerkService extends ClientListener {
  constructor(
    private sp: Sp,
    private controller: CombinedController,
  ) {
    super();
    this.controller.emitter.on('perkStateMessage', (e) => this.onPerkState(e));
    this.controller.on('menuClose', (e) => this.onMenuClose(e));
    this.controller.on('update', () => this.onUpdate());
  }

  private onPerkState(e: ConnectionMessage<PerkStateMessage>) {
    this.state = {
      perks: new Set(e.message.perks),
      points: e.message.points,
      applied: false,
    };
    this.reported.clear();
    this.apply();
  }

  private onMenuClose(e: MenuCloseEvent) {
    if (e.name === PerkService.statsMenu) {
      this.report();
    }
  }

  private onUpdate() {
    const now = Date.now();
    if (now - this.lastTick < PerkService.tickMs) {
      return;
    }
    this.lastTick = now;
    if (!this.state) {
      return;
    }
    if (!this.state.applied) {
      this.apply();
      return;
    }
    this.report();
  }

  // the tree perks the player holds now, within the universe
  private heldTreePerks(player: Actor): Set<number> {
    const universe = this.universe();
    const held = new Set<number>();
    universe.forEach((id) => {
      const perk = Perk.from(this.sp.Game.getFormEx(id));
      if (perk && player.hasPerk(perk)) {
        held.add(id);
      }
    });
    return held;
  }

  // every perk of the eighteen trees, all ranks, owned or not: read once
  private universe(): Set<number> {
    if (this.treePerks) {
      return this.treePerks;
    }
    const player = this.sp.Game.getPlayer();
    if (!player) {
      return new Set();
    }
    const all = new Set<number>();
    for (const name of skillNames) {
      const info = ActorValueInfo.getActorValueInfoByName(name);
      if (!info) {
        continue;
      }
      for (const unowned of [false, true]) {
        const list = info.getPerks(player, unowned, true);
        if (!list) {
          continue;
        }
        for (const entry of list) {
          const perk = Perk.from(entry);
          if (perk) {
            all.add(perk.getFormID());
          }
        }
      }
    }
    if (all.size > 0) {
      this.treePerks = all;
      logTrace(this, `perk universe: ${all.size} tree perk(s)`);
    }
    return all;
  }

  // the engine's picks the server has not heard of
  private report() {
    const state = this.state;
    const player = this.sp.Game.getPlayer();
    if (!state || !state.applied || !player) {
      return;
    }
    this.heldTreePerks(player).forEach((id) => {
      if (state.perks.has(id) || this.reported.has(id)) {
        return;
      }
      this.reported.add(id);
      this.controller.emitter.emit('sendMessage', {
        message: { t: MsgType.PerkPick, perk: id },
        reliability: 'reliable',
      });
      logTrace(this, `perk pick ${hex(id)} reported`);
    });
  }

  // the server's set and points onto the engine
  private apply() {
    const state = this.state;
    const player = this.sp.Game.getPlayer();
    if (!state || !player) {
      return;
    }
    const universe = this.universe();
    if (universe.size === 0) {
      return; // the game is not up yet; the next tick tries again
    }
    const added: number[] = [];
    const removed: number[] = [];
    state.perks.forEach((id) => {
      const perk = Perk.from(this.sp.Game.getFormEx(id));
      if (!perk) {
        logError(
          this,
          `perk ${hex(id)} of the server's set is not in this game's files`,
        );
        return;
      }
      if (!player.hasPerk(perk)) {
        player.addPerk(perk);
        added.push(id);
      }
    });
    this.heldTreePerks(player).forEach((id) => {
      if (!state.perks.has(id)) {
        const perk = Perk.from(this.sp.Game.getFormEx(id));
        if (perk) {
          player.removePerk(perk);
          removed.push(id);
        }
      }
    });
    this.sp.Game.setPerkPoints(state.points);
    state.applied = true;
    logTrace(
      this,
      `perk state: ${state.perks.size} perk(s), ${state.points} point(s); added [${added.map(hex).join(' ')}] removed [${removed.map(hex).join(' ')}]`,
    );
  }

  private state: State | undefined;
  private treePerks: Set<number> | undefined;
  private readonly reported = new Set<number>();
  private lastTick = 0;
  private static readonly tickMs = 5000;
  // RE::StatsMenu::MENU_NAME (actorValuesService.ts names it the same way)
  private static readonly statsMenu = 'StatsMenu';
}
