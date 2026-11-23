// A served corpse record's pass on this seat (ChangeFormService): lay the
// record on the dead copy (ApplyChangeForm), reconcile the laid body's
// inventory to the server's rows (rerollActor on a dead copy), hold the
// record for a later pass, or drop it. The facts are read from the engine
// and the services by the caller; the choice is JS state alone.

// how long a record waits for its copy to die here before it is dropped
export const HOLD_MS = 120_000;

export interface CorpseFacts {
    // the emergency lever: a served record is held and never laid
    applyOff: boolean;
    // an apply runs on the main thread; its answer decides
    queued: boolean;
    // the native laid the record and the laid body's reconcile has not run
    reconcileOwed: boolean;
    // this seat's ContainerMenu is open
    menuOpen: boolean;
    // the record's form is an actor here
    actor: boolean;
    // the copy is dead here
    dead: boolean;
    // the kill's own pass or a bind kill's: the record lays on the seat
    // that simulates the body too
    bind: boolean;
    // this seat simulates the body
    simulatesHere: boolean;
    // this seat follows the body on the prop stream and the follow has not
    // rested
    followed: boolean;
}

export type CorpsePass =
    | "lay"        // ApplyChangeForm now
    | "reconcile"  // rerollActor now
    | "off"        // held: the lever is on
    | "queued"     // held: an apply is in flight
    | "menu"       // held until the menu closes
    | "no-actor"   // held: no actor form here yet
    | "kill"       // held until the copy dies here
    | "simulated"  // dropped: this seat simulates the body, its engine's state stands
    | "follow";    // held until the follow rests

export function corpsePass(f: CorpseFacts): CorpsePass {
    // the reconcile removes and adds the body's stacks (RemoveItem,
    // AddObjectToContainer) and posts no refresh to an open item menu, the
    // lay's hazard: it waits for the close as the lay does
    if (f.reconcileOwed) {
        return f.menuOpen ? "menu" : "reconcile";
    }
    if (f.applyOff) {
        return "off";
    }
    if (f.queued) {
        return "queued";
    }
    // a lay rebuilds the body's inventory under an open menu's item list and
    // its 3D preview (ChangeForm.cpp refuses one while the body's own loot
    // menu shows); the record waits for the close
    if (f.menuOpen) {
        return "menu";
    }
    if (!f.actor) {
        return "no-actor";
    }
    if (!f.dead) {
        return "kill";
    }
    if (!f.bind && f.simulatesHere) {
        return "simulated";
    }
    // a followed body is laid once its follow has rested, never under its
    // stream's tail
    if (f.followed) {
        return "follow";
    }
    return "lay";
}

// the poll's clock: a record held past HOLD_MS is dropped, unless its last
// pass waited for the open menu (a long loot does not run the hold out)
export function outlivesHold(heldMs: number, menuHeld: boolean): boolean {
    return heldMs >= HOLD_MS && !menuHeld;
}
