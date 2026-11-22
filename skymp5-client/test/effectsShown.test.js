// effectsShown.ts's own test: what this game draws of the server's running
// effects on an actor. Run by `yarn test` from skymp5-client.
"use strict";
const assert = require("assert");
const { effectsShown, noLongerShown } = require("../build/test/effectsShown.js");

// a shader and art pair as ids, synthetic: the rule reads no record
const FIRE_SHADER = 0xa001;
const FIRE_ART = 0xa002;
const OTHER_SHADER = 0xa003;
let passed = 0;
function test(name, fn) {
    fn();
    passed += 1;
    console.log(`ok - ${name}`);
}

test("an effect the engine here runs on the actor draws neither its shader nor its art", () => {
    // the attacker's own game: its engine added its enchanted hit to the actor it struck,
    // and the server's record of the same hit arrives after it
    const shown = effectsShown([{ shader: FIRE_SHADER, art: FIRE_ART, seconds: 3, engineRuns: true }]);
    assert.strictEqual(shown.shaders.size, 0);
    assert.strictEqual(shown.arts.size, 0);
});

test("an effect only the server runs draws its shader and art for the seconds left", () => {
    const shown = effectsShown([{ shader: FIRE_SHADER, art: FIRE_ART, seconds: 3, engineRuns: false }]);
    assert.deepStrictEqual([...shown.shaders], [[FIRE_SHADER, 3]]);
    assert.deepStrictEqual([...shown.arts], [[FIRE_ART, 3]]);
});

test("a shader or art two effects share shows for the longer, no end the longest", () => {
    const shown = effectsShown([
        { shader: FIRE_SHADER, art: FIRE_ART, seconds: 2, engineRuns: false },
        { shader: FIRE_SHADER, art: 0, seconds: 5, engineRuns: false },
        { shader: 0, art: FIRE_ART, seconds: -1, engineRuns: false },
    ]);
    assert.deepStrictEqual([...shown.shaders], [[FIRE_SHADER, 5]]);
    assert.deepStrictEqual([...shown.arts], [[FIRE_ART, -1]]);
});

test("one effect the engine runs leaves another's shader of the same id to show", () => {
    const shown = effectsShown([
        { shader: FIRE_SHADER, art: 0, seconds: 3, engineRuns: true },
        { shader: FIRE_SHADER, art: 0, seconds: 4, engineRuns: false },
    ]);
    assert.deepStrictEqual([...shown.shaders], [[FIRE_SHADER, 4]]);
});

test("a shader this game drew and draws no more is stopped, the rest kept", () => {
    const now = new Map([[FIRE_SHADER, 3]]);
    assert.deepStrictEqual(noLongerShown(new Set([FIRE_SHADER, OTHER_SHADER]), now, new Set()), [OTHER_SHADER]);
    assert.deepStrictEqual(noLongerShown(new Set(), now, new Set()), []);
});

test("a shader whose effect the engine took over is left to run out, not stopped", () => {
    const shown = effectsShown([{ shader: FIRE_SHADER, art: 0, seconds: 3, engineRuns: true }]);
    assert.deepStrictEqual([...shown.engineShaders], [FIRE_SHADER]);
    assert.deepStrictEqual(noLongerShown(new Set([FIRE_SHADER]), shown.shaders, shown.engineShaders), []);
});

console.log(`${passed} of ${passed} passed`);
