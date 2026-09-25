// thuum: the headless fakeclient against this server over real RakNet.
// Connect, log in as profile 1, move, AddItem through a console command,
// then check what the server recorded. Serves as `just test-proto` (T2) and
// as the smoke of difftest's legacy driver (docs/WIRE.md).
const assert = require("node:assert");
const { spawn } = require("node:child_process");
const path = require("node:path");

const IRON_SWORD = 0x12eb7;

const runFakeclient = () => new Promise((resolve) => {
  const bin = path.join(process.cwd(), "fakeclient");
  const child = spawn(bin, [
    "--host", "127.0.0.1", "--port", "7777", "--profile-id", "1",
    "--moves", "5", "--add-item", String(IRON_SWORD), "--add-item-count", "1",
    "--timeout-ms", "20000", "--settle-ms", "2000",
  ], { stdio: ["ignore", "pipe", "pipe"] });
  let out = "";
  child.stdout.on("data", (d) => { out += d; });
  child.stderr.on("data", (d) => process.stderr.write(d));
  child.on("close", (code) => resolve({ code, out }));
});

const main = async () => {
  const { code, out } = await runFakeclient();
  const events = out.trim().split("\n").filter(Boolean).map((l) => JSON.parse(l));
  const actor = events.find((e) => e.event === "actor");
  assert.strictEqual(code, 0, `fakeclient exit code ${code}; events: ${out.slice(-2000)}`);
  assert.ok(actor, "fakeclient received its CreateActor with isMe");

  const actorId = mp.getActorsByProfileId(1)[0];
  assert.ok(actorId, "server created an actor for profile 1");
  assert.strictEqual(mp.get(actorId, "idx"), actor.idx, "idx matches what the client saw");

  const inv = mp.get(actorId, "inventory");
  assert.ok(inv.entries.some((e) => e.baseId === IRON_SWORD && e.count >= 1), `AddItem landed: ${JSON.stringify(inv)}`);

  const loc = mp.get(actorId, "locationalData");
  const moved = Math.abs(loc.pos[0] - actor.pos[0]);
  assert.ok(moved > 100, `movement was applied server side (dx=${moved})`);
  const received = events.filter((e) => e.event === "message").length;
  assert.ok(received >= 3, `server sent messages back (${received})`);
};

main().then(() => {
  console.log("Test passed!");
  process.exit(0);
}).catch((err) => {
  console.log("Test failed!");
  console.error(err);
  process.exit(1);
});
