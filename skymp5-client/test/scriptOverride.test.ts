// A script override's merge into the owner's own read of the object's scripts (sync/scriptOverride.ts): each script
// the override names, found by type without case, takes its state and the variables it names, found by name without
// case in the read's own names; every other position is the read's own. Bleak Falls Sanctum's word wall trigger
// 0x709d2 carries WordWallTrigger02Script, WordWallTriggerScript and WordWallTriggerBleakFallsScript (Skyrim.esm's
// VMAD); WordWallTriggerScript's `::wordLearned_var` is its 14th variable of 38 in its pex.
import { test } from "node:test";
import * as assert from "node:assert/strict";
import { overrideByName } from "../src/sync/scriptOverride";
import { ScriptObjectState } from "../src/services/messages/scriptStateTypes";

const read = (): { scripts: ScriptObjectState[]; names: string[][] } => ({
    scripts: [
        { type: "WordWallTrigger02Script", state: "done", flags: 11, vars: [] },
        { type: "WordWallTriggerScript", state: "done", flags: 11, vars: [{ k: 5, b: true }, { k: 3, i: 60 }, { k: 5, b: false }] },
        { type: "WordWallTriggerBleakFallsScript", state: "done", flags: 11, vars: [{ k: 1, o: { kind: 0, mid: 65535, form: 0x709d2, type: "WordWallTriggerScript" } }] },
    ],
    names: [[], ["::wordLearned_var", "::myStage_var", "doOnce"], ["mySelf"]],
});

const wall = [
    { type: "WordWallTriggerScript", state: "", vars: [{ name: "::WordLearned_var", k: 5, b: false }] },
    { type: "WordWallTriggerBleakFallsScript", state: "Listening", vars: [] },
];

test("the wall's script goes back to its empty state with its learned flag false, its listener to Listening, all else the read's own", () => {
    const r = read();
    const out = overrideByName(r.scripts, r.names, wall);
    assert.deepEqual(out.scripts, [
        { type: "WordWallTrigger02Script", state: "done", flags: 11, vars: [] },
        { type: "WordWallTriggerScript", state: "", flags: 11, vars: [{ k: 5, b: false }, { k: 3, i: 60 }, { k: 5, b: false }] },
        { type: "WordWallTriggerBleakFallsScript", state: "Listening", flags: 11, vars: [{ k: 1, o: { kind: 0, mid: 65535, form: 0x709d2, type: "WordWallTriggerScript" } }] },
    ]);
    assert.deepEqual(out.left, []);
    // the read itself is not written
    assert.equal(r.scripts[1].state, "done");
    assert.deepEqual(r.scripts[1].vars[0], { k: 5, b: true });
});

test("a script the copy does not carry, or a variable its chain does not declare, is left and said", () => {
    const r = read();
    const out = overrideByName(r.scripts, r.names, [
        { type: "WordWallTriggerScript", state: "", vars: [{ name: "::noSuch_var", k: 3, i: 1 }] },
        { type: "NoSuchScript", state: "", vars: [] },
    ]);
    assert.equal(out.scripts[1].state, "");
    assert.deepEqual(out.scripts[1].vars, r.scripts[1].vars);
    assert.deepEqual(out.left, ["WordWallTriggerScript's ::noSuch_var (not declared)", "NoSuchScript (not attached)"]);
});

test("an int and a float take their own kind's field alone", () => {
    const r = read();
    const out = overrideByName(r.scripts, r.names, [
        { type: "wordwalltriggerscript", state: "done", vars: [{ name: "::MYSTAGE_VAR", k: 4, f: 0.5 }, { name: "doOnce", k: 3, i: 2 }] },
    ]);
    assert.deepEqual(out.scripts[1].vars, [{ k: 5, b: true }, { k: 4, f: 0.5 }, { k: 3, i: 2 }]);
});
