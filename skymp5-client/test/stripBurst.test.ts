// A burst of container events through ContainersService's own handler (src/services/services/containersService.ts, the
// containerChanged event). The engine raises one event per stack it moves, and each event reports every difference
// between the baseline (the record as this game last heard it) and the game's inventory. A jail's strip moves the
// player's every stack into the hold's chest in one frame, and a take-all from a faction's chest moves every stack
// with the faction's stolen mark: each stack, worn, owned and plain alike, is reported once. The platform module is a
// stand-in loaded under its name, as in test/shoutClaimUpdate.test.ts; the game's inventory after each event is this
// file's.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { MsgType } from "../src/messages";
import type { ContainersService as ContainersServiceClass } from "../src/services/services/containersService";
import type { Inventory, Entry } from "../src/sync/inventory";

const PLAYER = 0x14;
const CHEST = 0x267e8;
const FACTION = 0x9f25a;

const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});
const platform = new Proxy({ storage: {}, printConsole: () => {} }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
const Module = require("module");
const resolve = Module._resolveFilename;
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
// the game's inventory is this file's: the module's reader answers it
const inventoryModule = require("../src/sync/inventory");
let gameInventory: Inventory = { entries: [] };
inventoryModule.getInventory = () => gameInventory;
const { ContainersService } = require("../src/services/services/containersService") as { ContainersService: typeof ContainersServiceClass };

// the player's stacks as the record holds them: worn armor, plain arrows, a stolen ring, plain potions
const ARMOR: Entry = { baseId: 0x13911, count: 1, worn: true };
const ARROWS: Entry = { baseId: 0x1397d, count: 60 };
const RING: Entry = { baseId: 0x1c4e6, count: 1, ownerId: FACTION };
const POTIONS: Entry = { baseId: 0x3eadd, count: 3 };

const service = (record: Inventory) => {
  const listeners: Record<string, Array<(e?: unknown) => void>> = {};
  const sent: Array<{ t: number; baseId: number; count: number; target: number }> = [];
  const lastInv = { lastInv: { entries: record.entries.map((e) => ({ ...e })) } as Inventory | undefined };
  const sp: any = new Proxy({
    Game: { getPlayer: () => ({ getFormID: () => PLAYER }), getFormEx: () => ({ getName: () => "" }) },
  }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
  const controller: any = {
    on: (name: string, fn: (e?: unknown) => void) => { (listeners[name] ??= []).push(fn); },
    once: () => {},
    emitter: {
      on: () => {},
      emit: (name: string, e: { message: { t: number; baseId: number; count: number; target: number } }) => {
        if (name === "sendMessage") {
          sent.push(e.message);
        }
      },
    },
    lookupListener: (cls: { name: string }) => (cls.name === "LastInvService" ? lastInv
      : cls.name === "SweetTaffySweetCantDropService" ? { canDropOrPutItem: () => true } : anything),
  };
  new ContainersService(sp, controller);
  const ref = (id: number) => ({ getFormID: () => id });
  // one engine event per stack moved, the game's inventory as it stands after it
  const move = (from: number, to: number, after: Inventory) => {
    gameInventory = after;
    (listeners["containerChanged"] ?? []).forEach((fn) => fn({ oldContainer: ref(from), newContainer: ref(to), baseObj: anything, numItems: 1 }));
  };
  const reports = (t: number) => sent.filter((m) => m.t === t).map((m) => `${m.baseId.toString(16)} x${m.count} ${m.target.toString(16)}`).sort();
  return { move, reports };
};

// The engine strips in one frame and its events reach the handler after it, so every event of the burst reads the
// stripped inventory (57's strip: the first event's diff named every stack)
test("a strip's burst reports each stack the player lost once, worn and owned stacks included", () => {
  const stacks = [ARMOR, ARROWS, RING, POTIONS];
  const { move, reports } = service({ entries: stacks });
  for (let i = 0; i < stacks.length; ++i) {
    move(PLAYER, CHEST, { entries: [] });
  }
  assert.deepEqual(reports(MsgType.PutItem), ["13911 x1 267e8", "1397d x60 267e8", "1c4e6 x1 267e8", "3eadd x3 267e8"]);
  assert.deepEqual(reports(MsgType.TakeItem), []);
});

test("a strip whose events land one stack at a time reports each stack once", () => {
  const stacks = [ARMOR, ARROWS, RING, POTIONS];
  const { move, reports } = service({ entries: stacks });
  for (let i = 1; i <= stacks.length; ++i) {
    move(PLAYER, CHEST, { entries: stacks.slice(i).map((e) => ({ ...e })) });
  }
  assert.deepEqual(reports(MsgType.PutItem), ["13911 x1 267e8", "1397d x60 267e8", "1c4e6 x1 267e8", "3eadd x3 267e8"]);
});

test("a take-all's burst from a faction's chest reports each stack the player gained once, the faction's mark on each", () => {
  const taken = [ARROWS, POTIONS, { baseId: 0x12eb6, count: 1 }].map((e) => ({ ...e, ownerId: FACTION }));
  const { move, reports } = service({ entries: [] });
  for (let i = 0; i < taken.length; ++i) {
    move(CHEST, PLAYER, { entries: taken.map((e) => ({ ...e })) });
  }
  assert.deepEqual(reports(MsgType.TakeItem), ["12eb6 x1 267e8", "1397d x60 267e8", "3eadd x3 267e8"]);
  assert.deepEqual(reports(MsgType.PutItem), []);
});
