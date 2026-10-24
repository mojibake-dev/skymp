const assert = require("node:assert");

const main = async () => {
  const crabActorId = 0xDC558;

  let onDeathCalledWith = null;
  let onRespawnCalledWith = null;

  mp.onDeath = (actorId, killerId) => {
    onDeathCalledWith = { actorId, killerId };
  };

  mp.onRespawn = (actorId) => {
    onRespawnCalledWith = { actorId };
  };

  // thuum docs/verbs/npc-fate.md (the deaths): a game-file actor's body
  // keeps to the server's reset clock (the npcRespawnHours setting, here
  // 0.0001 h in test_isdead.settings.json; the master's 240 in play), never
  // the per-actor spawnDelay, which times players alone; the sweep that
  // stands the body up runs once a second, so the wait below spans one
  mp.set(crabActorId, "spawnDelay", 0);

  assert.deepEqual(onDeathCalledWith, null);
  assert.deepEqual(onRespawnCalledWith, null);
  assert.strictEqual(mp.get(crabActorId, "isDead"), false);

  // variant 1
  // mp.callPapyrusFunction("method", "Actor", "DamageActorValue",
  //   { type: "form", desc: mp.getDescFromId(crabActorId) },
  //   ["Health", 1000]
  // );

  // variant 2
  mp.set(crabActorId, "isDead", true);

  assert.deepEqual(onDeathCalledWith, { actorId: crabActorId, killerId: 0 });
  assert.deepEqual(onRespawnCalledWith, null);
  assert.strictEqual(mp.get(crabActorId, "isDead"), true);

  await new Promise((resolve) => setTimeout(resolve, 2500));

  assert.deepEqual(onDeathCalledWith, { actorId: crabActorId, killerId: 0 });
  assert.deepEqual(onRespawnCalledWith, { actorId: crabActorId });
  assert.strictEqual(mp.get(crabActorId, "isDead"), false);
};

main().then(() => {
  console.log("Test passed!");
  process.exit(0);
}).catch((err) => {
  console.log("Test failed!")
  console.error(err);
  process.exit(1);
});
