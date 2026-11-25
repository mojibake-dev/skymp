import { ScriptStateOverrideScript, ScriptStateOverrideVar } from "../services/messages/scriptStateOverrideMessage";
import { ScriptObjectState, ScriptVar } from "../services/messages/scriptStateTypes";

// A script override (ScriptStateOverride) over this game's own read of the
// object's scripts: each script the override names, found by type without
// case as the VM finds a class, takes the override's state and the variables
// it names, each found by name without case among the read's own names (the
// chain's declared names, in the read's positions); every other script,
// state and position stays the read's own. The read is not written. `left`
// says what the override named that the copy lacks: a script not attached,
// a variable its chain does not declare, a value without its kind's field.
export const overrideByName = (
    scripts: ScriptObjectState[],
    names: string[][],
    put: ScriptStateOverrideScript[],
): { scripts: ScriptObjectState[]; left: string[] } => {
    const same = (a: string, b: string) => a.toLowerCase() === b.toLowerCase();
    const out = scripts.map((s) => ({ ...s, vars: s.vars.slice() }));
    const left: string[] = [];
    for (const p of put) {
        const n = out.findIndex((s) => same(s.type, p.type));
        if (n === -1) {
            left.push(`${p.type} (not attached)`);
            continue;
        }
        out[n] = { ...out[n], state: p.state };
        const declared = names[n] ?? [];
        for (const v of p.vars) {
            const i = declared.findIndex((name) => same(name, v.name));
            if (i === -1 || i >= out[n].vars.length) {
                left.push(`${p.type}'s ${v.name} (not declared)`);
                continue;
            }
            const value = valueOf(v);
            if (value === undefined) {
                left.push(`${p.type}'s ${v.name} (kind ${v.k} without its value)`);
                continue;
            }
            out[n].vars[i] = value;
        }
    }
    return { scripts: out, left };
};

// an override's value as the read carries one: the kind, then its one field
const valueOf = (v: ScriptStateOverrideVar): ScriptVar | undefined => {
    if (v.k === 3 && typeof v.i === "number") {
        return { k: 3, i: v.i };
    }
    if (v.k === 4 && typeof v.f === "number") {
        return { k: 4, f: v.f };
    }
    if (v.k === 5 && typeof v.b === "boolean") {
        return { k: 5, b: v.b };
    }
    return undefined;
};
