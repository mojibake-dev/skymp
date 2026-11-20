// TODO: refactor this out
import { localIdToRemoteId } from "../../view/worldViewMisc";

import { Armor, Enchantment, MenuCloseEvent, MenuOpenEvent, Weapon } from "skyrimPlatform";
import { ClientListener, CombinedController, Sp } from "./clientListener";
import { Entry, Inventory, getDiff, getInventory } from "../../sync/inventory";
import { isSessionId, toWireEntry } from "../../sync/createdForms";
import { holdPcInventoryForReply } from "./remoteServer";
import { CraftReportMessage } from "../messages/craftReportMessage";
import { MsgType } from "../../messages";
import { logError, logLab } from "../../logging";

// thuum tracker #98 (crafting; Eli's decision of 2026-11-16: relay the
// crafter's record). Every bench (forge, smelter, tanning rack, grindstone,
// workbench, cooking pot, alchemy lab, arcane enchanter, staff enchanter)
// runs the engine's Crafting Menu (HYPOTHESIS for the staff enchanter: the
// game's own menu name read from isBadMenuShown's list, sync/equipment.ts),
// so one sitting is the span from its open to its close: this engine's
// inventory read at both, the difference reported as CraftReport (MsgType
// 84) with every stack in the wire's shape (sync/createdForms.ts: a brewed
// potion or a made enchantment by its definition, read from the engine's own
// form). The server takes the lost stacks from the record and adds the
// gained ones as reported (R2), so the engine's own menu stays the whole
// computation: recipes, tempering, brewing, enchanting and disenchanting
// alike, and the legacy CraftItem (a recipe matched on the server, which
// knew no brew, no enchant and no temper) is no longer sent.
const craftingMenu = "Crafting Menu";
// wire-schema cap::CRAFT_ROWS and cap::CRAFT_LEARNED
const craftRows = 64;
const craftLearned = 128;
// how long the player's inventory apply waits for the record's answer to a
// report (remoteServer.ts holdPcInventoryForReply); its SetInventory ends
// the wait at once
const replyHoldMs = 3000;

interface Sitting {
  before: Inventory;
  furnitureId: number;
}

export class CraftService extends ClientListener {
  constructor(private sp: Sp, private controller: CombinedController) {
    super();
    controller.on("menuOpen", (e) => this.onMenuOpen(e));
    controller.on("menuClose", (e) => this.onMenuClose(e));
  }

  private onMenuOpen(e: MenuOpenEvent) {
    if (e.name !== craftingMenu) {
      return;
    }
    const player = this.sp.Game.getPlayer();
    if (!player) {
      return;
    }
    const furniture = player.getFurnitureReference();
    this.sitting = { before: getInventory(player), furnitureId: furniture ? furniture.getFormID() : 0 };
  }

  private onMenuClose(e: MenuCloseEvent) {
    if (e.name !== craftingMenu) {
      return;
    }
    const sitting = this.sitting;
    this.sitting = undefined;
    if (!sitting) {
      return;
    }
    try {
      this.report(sitting);
    } catch (err) {
      logError(this, "the crafting report failed", err);
    }
  }

  private report(sitting: Sitting) {
    const player = this.sp.Game.getPlayer();
    if (!player) {
      return;
    }
    const after = getInventory(player);
    // the worn flag left out: an item worn across a temper or an enchant is
    // the same stack, and the record's rows carry none
    const diff = getDiff(after, sitting.before, true).entries;
    const strip = (e: Entry, count: number): Entry => {
      const row: Entry = JSON.parse(JSON.stringify(e));
      row.count = count;
      delete row.worn;
      delete row.wornLeft;
      return row;
    };
    const lostLocal = diff.filter((e) => e.count < 0).map((e) => strip(e, -e.count));
    const gainedLocal = diff.filter((e) => e.count > 0).map((e) => strip(e, e.count));
    if (lostLocal.length === 0 && gainedLocal.length === 0) {
      return;
    }

    const learned = this.learnedEnchantments(lostLocal, gainedLocal);

    const toWire = (rows: Entry[], side: string): Entry[] => {
      const out: Entry[] = [];
      rows.forEach((row) => {
        const wire = toWireEntry(row);
        if (wire) {
          out.push(wire);
        } else {
          logLab("verb:craft", `a ${side} stack of ${row.baseId.toString(16)} has no definition this engine can read; left out of the report`);
        }
      });
      return out;
    };
    const lost = toWire(lostLocal, "lost");
    const gained = toWire(gainedLocal, "gained");

    const furnitureId = sitting.furnitureId;
    const workbench = furnitureId ? localIdToRemoteId(furnitureId) || furnitureId : 0;
    if (!workbench) {
      logError(this, "a crafting sitting with no bench under the player; nothing reported, the record stands");
      return;
    }

    // a sitting past one report's rows goes as several, each standing alone
    let sent = 0;
    for (let i = 0; i < Math.max(lost.length, gained.length); i += craftRows) {
      const message: CraftReportMessage = {
        t: MsgType.CraftReport,
        workbench,
        seq: ++this.seq,
        lost: lost.slice(i, i + craftRows),
        gained: gained.slice(i, i + craftRows),
        learnedEnchantments: i === 0 ? learned.slice(0, craftLearned) : [],
      };
      if (message.lost.length === 0 && message.gained.length === 0) {
        continue;
      }
      this.controller.emitter.emit("sendMessage", { message, reliability: "reliable" });
      ++sent;
    }
    if (sent > 0) {
      holdPcInventoryForReply(replyHoldMs);
    }
    logLab("verb:craft", `bench ${workbench.toString(16)}: ${lost.length} stack(s) lost, ${gained.length} gained, ${learned.length} enchantment(s) known, in ${sent} report(s)`);
  }

  // A disenchant destroys an enchanted item, and the engine marks its
  // game-file enchantment, or that enchantment's base, known (TESForm::
  // SetPlayerKnows; HYPOTHESIS, the static read of tracker #98 note 3924).
  // So for each lost stack whose item came back in no other shape (a temper
  // or an enchant gains its base again), its enchantments are the item's
  // own (ExtraEnchantment's, a game-file one) and its record's (a weapon's
  // or an armor's EITM), each with its base enchantment, and the ones this
  // engine now marks known are reported (one known before is a no-op on the
  // server's record)
  private learnedEnchantments(lost: Entry[], gained: Entry[]): number[] {
    const learned = new Set<number>();
    const consider = (enchantment: Enchantment | null) => {
      if (!enchantment) {
        return;
      }
      const id = enchantment.getFormID();
      if (!isSessionId(id) && enchantment.playerKnows()) {
        learned.add(id);
      }
    };
    lost.forEach((row) => {
      if (gained.some((g) => g.baseId === row.baseId)) {
        return;
      }
      const candidates: Array<Enchantment | null> = [];
      if (row.enchantmentId !== undefined && !isSessionId(row.enchantmentId)) {
        candidates.push(Enchantment.from(this.sp.Game.getFormEx(row.enchantmentId)));
      }
      const item = this.sp.Game.getFormEx(row.baseId);
      const weapon = Weapon.from(item);
      const armor = Armor.from(item);
      candidates.push(weapon ? weapon.getEnchantment() : armor ? armor.getEnchantment() : null);
      candidates.forEach((enchantment) => {
        consider(enchantment);
        consider(enchantment ? enchantment.getBaseEnchantment() : null);
      });
    });
    const out: number[] = [];
    learned.forEach((id) => out.push(id));
    return out;
  }

  private sitting: Sitting | undefined;
  // the last report's number in this session (the server's answer names it)
  private seq = 0;
}
