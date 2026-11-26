// The server's difficulty reaches this game's own difficulty field (DisableDifficultySelectionService,
// src/services/services/disableDifficultySelectionService.ts): the player's own create word names it, and the
// service writes it with the platform's setPlayerDifficulty on its update, again each second, so a choice in the
// game's menu is put back. The engine's DamageActor reads that field for every hit this engine applies itself (a
// trap's, a fall's), so they take the server's difficulty as the server's own hits do. The platform module is a
// stand-in loaded under its name, as in test/shoutClaimUpdate.test.ts.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import type { DisableDifficultySelectionService as ServiceClass } from "../src/services/services/disableDifficultySelectionService";

const anything: any = new Proxy(function () {
  return anything;
}, {
  get: (_t, k) => (k === "then" ? undefined : k === Symbol.toPrimitive ? () => 0 : anything),
  apply: () => anything,
  construct: () => anything,
});
const platform = new Proxy({ storage: {} }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
const Module = require("module");
const resolve = Module._resolveFilename;
const PLATFORM_NAMES = ["skyrimPlatform", "@skyrim-platform/skyrim-platform"];
Module._resolveFilename = function (request: string, ...rest: unknown[]) {
  return PLATFORM_NAMES.includes(request) ? "skyrimPlatform" : resolve.call(this, request, ...rest);
};
require.cache["skyrimPlatform"] = { id: "skyrimPlatform", filename: "skyrimPlatform", loaded: true, exports: platform } as any;
const { DisableDifficultySelectionService } = require("../src/services/services/disableDifficultySelectionService") as {
  DisableDifficultySelectionService: typeof ServiceClass;
};

const LEGENDARY = 5;
const FRAMES_PER_WRITE = 60;

const service = () => {
  const written: number[] = [];
  const updates: Array<() => void> = [];
  const messages: Record<string, (e: unknown) => void> = {};
  const sp: any = new Proxy({
    settings: { "skymp5-client": {} },
    setPlayerDifficulty: (level: number) => { written.push(level); return true; },
  }, { get: (t, k) => (k in t ? (t as any)[k] : anything) });
  const controller: any = {
    on: (name: string, fn: () => void) => { if (name === "update") updates.push(fn); },
    once: () => undefined,
    emitter: { on: (name: string, fn: (e: unknown) => void) => { messages[name] = fn; }, emit: () => undefined },
  };
  new DisableDifficultySelectionService(sp, controller);
  const frames = (n: number) => {
    for (let i = 0; i < n; i++) updates.forEach((fn) => fn());
  };
  const createActor = (isMe: boolean, props: Record<string, unknown>) =>
    messages["createActorMessage"]?.({ message: { isMe, props } });
  return { written, frames, createActor };
};

test("the player's own create word's difficulty is written to its game's difficulty field, and again each second", () => {
  const s = service();
  s.frames(FRAMES_PER_WRITE);
  assert.deepEqual(s.written, [], "nothing before the server names a difficulty");

  s.createActor(false, { difficulty: LEGENDARY });
  s.frames(FRAMES_PER_WRITE);
  assert.deepEqual(s.written, [], "another actor's create word names none");

  s.createActor(true, { difficulty: LEGENDARY });
  s.frames(FRAMES_PER_WRITE);
  assert.deepEqual(s.written, [LEGENDARY]);
  s.frames(FRAMES_PER_WRITE);
  assert.deepEqual(s.written, [LEGENDARY, LEGENDARY]);
});
